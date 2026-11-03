#include "engine_runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace audio {
std::uint32_t select_period(Periods p, Preset preset) noexcept {
    if (!p.fundamental || !p.minimum || p.maximum < p.minimum) return 0;
    const auto value = preset == Preset::stable ? p.normal : p.minimum;
    const std::uint64_t rounded = ((std::uint64_t(value) + p.fundamental - 1) / p.fundamental) * p.fundamental;
    if (preset == Preset::stable && rounded != p.normal) return 0;
    if (rounded < p.minimum || rounded > p.maximum || rounded > std::numeric_limits<std::uint32_t>::max()) return 0;
    return static_cast<std::uint32_t>(rounded);
}
std::size_t select_queue_target(unsigned input_rate, std::size_t minimum_frames,
                              double period_seconds, Preset preset) noexcept {
    // A 12 ms floor leaves scheduling headroom even for very short device periods.
    return std::max({minimum_frames, (std::size_t(input_rate) * 12 + 999) / 1000,
        static_cast<std::size_t>(std::ceil(period_seconds * input_rate * (preset == Preset::stable ? 2 : 1)))});
}
void Worker::start() {
    stop();
    { std::lock_guard lock(setup_mutex_); setup_ = {}; }
    publish({}); error_ = 0; stage_ = "initialize"; state_ = State::starting;
    try { thread_ = std::jthread([this](std::stop_token token) { run(token); }); }
    catch (...) { state_ = State::failed; throw; }
}
void Worker::stop() noexcept {
    if (thread_.joinable()) { thread_.request_stop(); thread_.join(); }
    if (state_ != State::failed) state_ = State::stopped;
}
void Worker::publish(Metrics m) noexcept {
    input_ = m.core.input_frames; output_ = m.core.output_frames;
    consumed_ = m.core.consumed_frames; generated_ = m.core.generated_frames;
    underflows_ = m.core.underflows; overflows_ = m.core.overflows;
    resyncs_ = m.core.resyncs; dropped_ = m.core.dropped_frames;
    silent_ = m.core.silent_frames; startup_ = m.core.startup_silent_frames;
    discontinuities_ = m.core.discontinuities; src_errors_ = m.core.src_errors;
    timestamps_ = m.timestamp_errors; capture_silent_ = m.capture_silent_frames;
    queue_ = m.queued_frames; ppm_ = m.correction_ppm;
}
Snapshot Worker::snapshot() const {
    Snapshot s;
    s.state = state_.load(); s.error = error_.load(); s.stage = stage_.load();
    { std::lock_guard lock(setup_mutex_); s.setup = setup_; }
    s.metrics.core = {input_, output_, consumed_, generated_, underflows_, overflows_,
                      resyncs_, dropped_, silent_, startup_, discontinuities_, src_errors_};
    s.metrics.timestamp_errors = timestamps_; s.metrics.capture_silent_frames = capture_silent_;
    s.metrics.queued_frames = queue_; s.metrics.correction_ppm = ppm_;
    return s;
}
void Worker::run(std::stop_token token) noexcept {
    std::unique_ptr<Session> session;
    std::int32_t result = 0;
    const char* stage = "initialize";
    try {
        session = factory_();
        SetupInfo setup;
        result = session->initialize(token, setup);
        { std::lock_guard lock(setup_mutex_); setup_ = setup; }
        if (result >= 0 && !token.stop_requested()) {
            stage = "start capture"; result = session->start_capture();
            bool rendered = false;
            auto last_capture = std::chrono::steady_clock::now();
            auto last_render = last_capture;
            const auto startup = last_capture;
            while (result >= 0 && !token.stop_requested()) {
                stage = "wait";
                const auto wake = session->wait();
                if (token.stop_requested() || wake == Wake::stopped) break;
                if (wake == Wake::failed) { result = static_cast<std::int32_t>(0x80004005U); break; }
                const auto now = std::chrono::steady_clock::now();
                if (now - last_capture > std::chrono::seconds(2) ||
                    (rendered && now - last_render > std::chrono::seconds(2)) ||
                    (!rendered && now - startup > std::chrono::seconds(2))) {
                    stage = "audio event/prefill timeout";
                    result = static_cast<std::int32_t>(0x800705B4U); break;
                }
                if (wake == Wake::idle) continue;
                if (wake == Wake::capture) {
                    last_capture = now;
                    stage = "capture"; result = session->capture(token);
                    if (result < 0 || token.stop_requested()) break;
                    if (!rendered && session->ready()) {
                        stage = "start render"; result = session->start_render();
                        if (result < 0) break;
                        last_render = now;
                        rendered = true; state_ = State::running;
                    }
                } else if (rendered && wake == Wake::render) {
                    last_render = now;
                    stage = "render"; result = session->render();
                }
                publish(session->metrics());
            }
        }
    } catch (...) { result = static_cast<std::int32_t>(0x80004005U); }
    if (session) { session->stop(); publish(session->metrics()); session.reset(); }
    error_ = result; stage_ = result < 0 ? stage : "stopped";
    state_ = result < 0 ? State::failed : State::stopped;
}
}
