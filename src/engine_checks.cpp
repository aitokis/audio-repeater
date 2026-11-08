#include "engine_runtime.h"
#ifdef _WIN32
#include "wasapi_engine.h"
#include <ks.h>
#include <ksmedia.h>
#endif
#include <array>
#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace audio {
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("engine self-test failed"); }
constexpr auto failure = static_cast<std::int32_t>(0x80004005U);
constexpr auto device_invalidated = static_cast<std::int32_t>(0x88890004U);
constexpr auto resources_invalidated = static_cast<std::int32_t>(0x88890026U);
struct FakeClient {
    int activations{}, releases{}, modern{}, legacy{}, live{};
    std::int32_t query_result{}, modern_result{}, legacy_result{}, second_activation_result{};
    std::int32_t activate() {
        require(!live); ++activations;
        if (activations == 2 && second_activation_result < 0) return second_activation_result;
        live = activations; return 0;
    }
    std::int32_t periods(Periods& p) { p = {480, 48, 100, 960}; return query_result; }
    std::int32_t initialize3(std::uint32_t p) { require(live == 1 && p == 144); ++modern; return modern_result; }
    void release() { require(live == 1); live = 0; ++releases; }
    std::int32_t initialize_legacy() { require(live == 2); ++legacy; return legacy_result; }
};
constexpr auto unsupported = static_cast<std::int32_t>(0x88890008U);
constexpr auto unaligned = static_cast<std::int32_t>(0x88890019U);
constexpr auto buffer_error = static_cast<std::int32_t>(0x88890018U);
struct ExclusiveClient {
    int activations{}, releases{}, live{};
    std::size_t accept_at{};
    std::int32_t probe_result{unsupported}, period_result{}, first_result{}, second_result{}, buffer_result{}, activation_result{};
    std::int64_t normal{100000}, minimum{10000};
    std::uint32_t aligned_frames{480};
    std::vector<Format> probed;
    std::vector<bool> extensible;
    std::vector<std::int64_t> durations;
    std::int32_t activate() {
        require(!live); ++activations;
        if (activations == 2 && activation_result < 0) return activation_result;
        live = activations; return 0;
    }
    std::int32_t supports_format(Format f, bool ext) {
        require(live == 1 && f.valid());
        probed.push_back(f); extensible.push_back(ext);
        return probed.size() - 1 == accept_at ? 0 : probe_result;
    }
    std::int32_t device_periods(std::int64_t& n, std::int64_t& m) {
        n = normal; m = minimum; return period_result;
    }
    std::int32_t initialize_exclusive(std::int64_t duration) {
        require(live == static_cast<int>(durations.size()) + 1);
        durations.push_back(duration); return durations.size() == 1 ? first_result : second_result;
    }
    std::int32_t buffer_size(std::uint32_t& frames) { frames = aligned_frames; return buffer_result; }
    void release() { require(live == 1); live = 0; ++releases; }
};
void exclusive_setup() {
    for (const auto preset : {Preset::low_latency, Preset::stable}) {
        ExclusiveClient c; StreamInfo info;
        require(initialize_exclusive(c, true, preset, info) == 0);
        require(info.mode == ShareMode::exclusive && info.format.encoding == Encoding::pcm24 &&
            info.format.valid_bits == 24 && info.format.rate == 48000 && info.format.channels == 2);
        require(c.durations == std::vector<std::int64_t>{preset == Preset::stable ? 100000 : 10000} &&
            c.activations == 1 && !info.client3 && !info.fallback_reason && !info.alignment_retry);
    }
    { ExclusiveClient c; c.accept_at = 5; StreamInfo info;
      require(initialize_exclusive(c, true, Preset::low_latency, info) == 0);
      require(c.probed.size() == 6 && info.format.channels == 1 && info.format.encoding == Encoding::pcm32);
      for (std::size_t i = 0; i < c.probed.size(); ++i) {
          require(c.probed[i].rate == 48000 && c.probed[i].valid_bits == 24 && c.probed[i].channels == (i < 3 ? 2U : 1U));
          require(c.probed[i].encoding == (i % 3 == 2 ? Encoding::pcm32 : Encoding::pcm24));
          require(c.extensible[i] == (i % 3 != 1));
      } }
    { ExclusiveClient c; c.accept_at = 3; StreamInfo info;
      require(initialize_exclusive(c, false, Preset::stable, info) == 0);
      require(c.probed.size() == 4 && info.format.channels == 1 && info.format.channel_mask == 0);
      for (std::size_t i = 0; i < c.probed.size(); ++i)
          require(c.probed[i].encoding == Encoding::pcm16 && c.probed[i].rate == 48000 &&
              c.probed[i].valid_bits == 16 && c.probed[i].channels == (i < 2 ? 2U : 1U) && c.extensible[i] == (i % 2 == 0)); }
    for (const auto probe_result : {unsupported, static_cast<std::int32_t>(0x80070057U), 1, failure}) {
        ExclusiveClient c; c.accept_at = 99; c.probe_result = probe_result; StreamInfo info;
        require(initialize_exclusive(c, true, Preset::low_latency, info) == (probe_result == failure ? failure : unsupported));
        require(c.probed.size() == (probe_result == failure ? 1U : 6U) && c.durations.empty() && c.activations == 1);
    }
    for (int scenario = 0; scenario < 8; ++scenario) {
        ExclusiveClient c; c.first_result = unaligned; StreamInfo info;
        if (scenario == 1) c.second_result = unaligned;
        if (scenario == 2) c.aligned_frames = 0;
        if (scenario == 3) c.aligned_frames = 24001;
        if (scenario == 4) c.aligned_frames = 1; // below the minimum period
        if (scenario == 5) c.buffer_result = failure;
        if (scenario == 6) c.activation_result = failure;
        if (scenario == 7) c.second_result = failure;
        const auto result = initialize_exclusive(c, true, Preset::low_latency, info);
        require((result == 0) == (scenario == 0));
        if (scenario == 0 || scenario == 1 || scenario == 7) {
            require(c.activations == 2 && c.releases == 1 && c.durations == std::vector<std::int64_t>{10000,100000});
            require(info.alignment_retry && info.requested_frames == 480 && info.requested_period_100ns == 100000);
        }
        require(c.activations <= 2 && c.durations.size() <= 2 && !info.client3 && !info.fallback_reason);
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        ExclusiveClient c; StreamInfo info;
        if (scenario == 0) c.period_result = failure;
        if (scenario == 1) c.minimum = 0;
        if (scenario == 2) c.minimum = c.normal + 1;
        if (scenario == 3) c.normal = 5000001;
        require(initialize_exclusive(c, false, Preset::low_latency, info) < 0 && c.durations.empty());
    }
    { ExclusiveClient c; c.first_result = failure; StreamInfo info;
      require(initialize_exclusive(c, false, Preset::low_latency, info) == failure && c.activations == 1); }
    struct Padding {
        int calls{}; std::uint32_t value{123}; std::int32_t result{};
        std::int32_t padding(std::uint32_t& p) noexcept { ++calls; p = value; return result; }
    } c;
    std::uint32_t frames = 0;
    require(render_request(c, ShareMode::exclusive, 480, frames) == 0 && frames == 480 && c.calls == 0);
    require(render_request(c, ShareMode::shared, 480, frames) == 0 && frames == 357 && c.calls == 1);
    c.value = 481; require(render_request(c, ShareMode::shared, 480, frames) < 0);
    c.result = failure; require(render_request(c, ShareMode::shared, 480, frames) == failure);
}
void buffer_passes() {
    for (const bool capture : {true, false}) {
        Transport transport(48000,48000,1,8192,1024,480);
        std::vector<float> initial(1024,0.25F), samples(480,0.25F);
        require(transport.push(initial));
        BufferPass pass;
        int acquired = 0, processed = 0, released = 0;
        std::int32_t status = buffer_error;
        const auto acquire = [&] { ++acquired; return status; };
        const auto process = [&]() -> std::int32_t {
            ++processed;
            if (capture) (void)transport.push(samples);
            else (void)transport.render(samples);
            ++released; return 0;
        };
        const auto queued = transport.queued_input();
        const auto before = transport.statistics();
        for (int event = 0; event < 3; ++event)
            require(pass.run(ShareMode::exclusive,true,acquire,process) == 1);
        require(acquired == 3 && processed == 0 && released == 0 && transport.queued_input() == queued);
        require(transport.statistics().input_frames == before.input_frames &&
            transport.statistics().output_frames == before.output_frames &&
            transport.statistics().consumed_frames == before.consumed_frames);
        status = 0;
        require(pass.run(ShareMode::exclusive,true,acquire,process) == 0 && processed == 1 && released == 1);
        require(capture ? transport.statistics().input_frames == before.input_frames + 480 :
                          transport.statistics().output_frames == before.output_frames + 480);
        status = buffer_error;
        for (int event = 0; event < 3; ++event)
            require(pass.run(ShareMode::exclusive,true,acquire,process) == 1);
        require(pass.run(ShareMode::exclusive,true,acquire,process) == buffer_error);
        require(acquired == 8 && processed == 1 && released == 1); // no busy retry
        BufferPass other;
        status = 0;
        require(other.run(ShareMode::exclusive,true,acquire,process) == 0);
        status = buffer_error;
        require(pass.run(ShareMode::exclusive,true,acquire,process) == buffer_error); // other direction did not reset it
        for (const auto mode : {ShareMode::shared, ShareMode::exclusive}) {
            BufferPass fresh;
            const int count = processed;
            require(fresh.run(mode,false,acquire,process) == buffer_error); // prefill cannot succeed
            require(processed == count && released == count);
            if (mode == ShareMode::shared) require(fresh.run(mode,true,acquire,process) == buffer_error);
            status = failure;
            require(fresh.run(mode,true,acquire,process) == failure && processed == count);
            status = buffer_error;
        }
    }
}
enum class Mode { normal, setup_failure, capture_start_failure, capture_failure,
                  render_start_failure, render_failure, wait_failure, blocked_setup, blocked_wait, no_audio,
                  capture_only, render_only, capture_transient, render_transient, capture_persistent, render_persistent,
                  prefill_unavailable, prefill_timeout };
struct Audit {
    std::atomic<int> made{}, initialized{}, capture_started{}, render_started{}, captured{}, rendered{}, stopped{}, destroyed{};
    std::atomic<bool> wrong_thread{}, wrong_dispatch{}, wrong_lifetime{}, after_failure{}, waiting{};
    Mode mode{Mode::normal}, replacement{Mode::normal};
    std::int32_t error{failure};
    bool recover{};
    std::chrono::steady_clock::time_point first_created{}, last_created{};
    int first_captured{}, first_rendered{}, first_render_started{};
};
class FakeSession final : public Session {
public:
    explicit FakeSession(Audit& a) : audit_(a), owner_(std::this_thread::get_id()),
        mode_(a.recover && a.made > 0 ? a.replacement : a.mode) {
        if (a.made != a.destroyed || a.made != a.stopped) a.wrong_lifetime = true;
        a.last_created = std::chrono::steady_clock::now();
        if (a.made == 0) a.first_created = a.last_created;
        ++audit_.made;
    }
    ~FakeSession() override { own(); if (!stopped_) audit_.wrong_lifetime = true; ++audit_.destroyed; }
    std::int32_t initialize(std::stop_token token, SetupInfo& setup) override {
        operation(); token_ = token; ++audit_.initialized;
        if (audit_.made == 1) setup.target_frames = 123;
        if (mode_ == Mode::blocked_setup) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, token, [] { return false; });
        }
        return result(mode_ == Mode::setup_failure ? audit_.error : 0);
    }
    std::int32_t start_capture() noexcept override {
        operation(); ++audit_.capture_started;
        return result(mode_ == Mode::capture_start_failure ? audit_.error : 0);
    }
    Wake wait() noexcept override {
        operation();
        if (mode_ == Mode::wait_failure) { failed_ = true; return Wake::failed; }
        if (mode_ == Mode::blocked_wait) {
            audit_.waiting = true;
            std::unique_lock lock(mutex_);
            condition_.wait(lock, token_, [] { return false; });
            return Wake::stopped;
        }
        if (mode_ == Mode::no_audio) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); return Wake::idle;
        }
        if (token_.stop_requested()) return Wake::stopped;
        std::this_thread::yield();
        if (mode_ == Mode::capture_only || mode_ == Mode::prefill_timeout) return wake_ = Wake::capture;
        if (mode_ == Mode::render_only && render_started_) return wake_ = Wake::render;
        next_render_ = !next_render_;
        return wake_ = next_render_ ? Wake::capture : Wake::render;
    }
    std::int32_t capture(std::stop_token) noexcept override {
        operation(); if (wake_ != Wake::capture) audit_.wrong_dispatch = true;
        if (mode_ == Mode::capture_transient || mode_ == Mode::capture_persistent)
            return result(capture_pass_.run(ShareMode::exclusive,true,
                [&] { return ++capture_attempts_ <= 3 || mode_ == Mode::capture_persistent ? buffer_error : 0; },
                [&] { ++audit_.captured; ++captured_; return 0; }));
        ++audit_.captured; ++captured_; return result(mode_ == Mode::capture_failure ? audit_.error : 0);
    }
    bool ready() const noexcept override { operation(); return mode_ != Mode::prefill_timeout && captured_ >= 2; }
    std::int32_t start_render() noexcept override {
        operation(); if (wake_ != Wake::capture) audit_.wrong_dispatch = true;
        if (mode_ == Mode::prefill_unavailable)
            return result(render_pass_.run(ShareMode::exclusive,false,
                [&] { return mode_ == Mode::prefill_unavailable ? buffer_error : 0; },
                [&] { ++audit_.rendered; return 0; }));
        ++audit_.render_started; render_started_ = true; return result(mode_ == Mode::render_start_failure ? audit_.error : 0);
    }
    std::int32_t render() noexcept override {
        operation(); if (wake_ != Wake::render || !audit_.render_started) audit_.wrong_dispatch = true;
        if (mode_ == Mode::render_transient || mode_ == Mode::render_persistent)
            return result(render_pass_.run(ShareMode::exclusive,true,
                [&] { return ++render_attempts_ <= 3 || mode_ == Mode::render_persistent ? buffer_error : 0; },
                [&] { ++audit_.rendered; return 0; }));
        ++audit_.rendered; return result(mode_ == Mode::render_failure ? audit_.error : 0);
    }
    Metrics metrics() const noexcept override {
        operation(); Metrics m; m.core.input_frames = audit_.made == 1 ? 123 : 0; return m;
    }
    void stop() noexcept override {
        own(); stopped_ = true;
        if (audit_.made == 1) {
            audit_.first_captured = audit_.captured; audit_.first_rendered = audit_.rendered;
            audit_.first_render_started = audit_.render_started;
        }
        ++audit_.stopped;
    }
private:
    void own() const noexcept { if (owner_ != std::this_thread::get_id()) audit_.wrong_thread = true; }
    void operation() const noexcept { own(); if (failed_) audit_.after_failure = true; }
    std::int32_t result(std::int32_t value) noexcept { if (value < 0) failed_ = true; return value; }
    Audit& audit_;
    std::thread::id owner_;
    Mode mode_;
    bool failed_{}, stopped_{}, render_started_{};
    unsigned captured_{};
    std::stop_token token_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool next_render_{};
    Wake wake_{Wake::idle};
    BufferPass capture_pass_, render_pass_;
    unsigned capture_attempts_{}, render_attempts_{};
};
template<class Predicate> void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void lifecycle() {
    for (const auto mode : {Mode::setup_failure, Mode::capture_start_failure, Mode::capture_failure,
                           Mode::render_start_failure, Mode::render_failure, Mode::wait_failure}) {
        Audit a; a.mode = mode;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return worker.snapshot().state == State::failed; });
        worker.stop(); worker.stop();
        require(worker.snapshot().error < 0 && a.made == 1 && a.destroyed == 1 && a.stopped == 1 && !a.wrong_thread);
        if (mode == Mode::setup_failure) require(a.capture_started == 0);
        if (mode == Mode::capture_failure) require(a.render_started == 0);
        // A failed start must not poison a subsequent start on the same worker.
        a.mode = Mode::normal; worker.start();
        await([&] { return worker.snapshot().state == State::running; });
        worker.stop(); worker.stop();
        require(a.destroyed == 2 && a.stopped == 2 && !a.wrong_thread && !a.wrong_dispatch);
        require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
    }
    { // Cancellation while initialization waits must never start either stream.
        Audit a; a.mode = Mode::blocked_setup;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.initialized == 1; }); worker.stop();
        require(a.capture_started == 0 && a.render_started == 0 && a.destroyed == 1 && !a.wrong_thread);
    }
    { // Stop must wake a worker blocked waiting for its next audio event.
        Audit a; a.mode = Mode::blocked_wait;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.waiting.load(); }); worker.stop();
        require(a.destroyed == 1 && a.stopped == 1 && !a.wrong_thread);
    }
    { // Destroying/closing a running owner cancels and joins before returning.
        Audit a;
        { Worker worker([&] { return std::make_unique<FakeSession>(a); });
          worker.start(); await([&] { return a.rendered > 20; }); }
        require(a.stopped == 1 && a.destroyed == 1 && !a.wrong_thread);
    }
    { // Rapid cancellation remains safe before/after worker thread construction.
        Audit a;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        for (int i = 0; i < 20; ++i) { worker.start(); worker.stop(); }
        require(a.made <= 20 && a.destroyed == a.made && a.stopped == a.made && !a.wrong_thread && !a.wrong_lifetime);
    }
    for (const auto mode : {Mode::capture_transient, Mode::render_transient, Mode::capture_persistent, Mode::render_persistent,
                           Mode::prefill_unavailable}) {
        Audit a; a.mode = mode;
        Worker worker([&] { return std::make_unique<FakeSession>(a); }); worker.start();
        const bool persistent = mode == Mode::capture_persistent || mode == Mode::render_persistent || mode == Mode::prefill_unavailable;
        await([&] { return persistent ? worker.snapshot().state == State::failed : a.rendered > 20; });
        if (persistent) require(worker.snapshot().error == buffer_error);
        else require(worker.snapshot().state == State::running);
        worker.stop();
        require(a.destroyed == 1 && a.stopped == 1 && !a.wrong_thread && !a.wrong_dispatch);
        if (mode == Mode::capture_persistent) require(a.captured == 0 && a.render_started == 0);
        if (mode == Mode::render_persistent) require(a.rendered == 0);
        if (mode == Mode::prefill_unavailable) require(a.rendered == 0 && a.render_started == 0);
    }
}
void session_rebuilds() {
    for (const auto error : {device_invalidated, resources_invalidated}) {
        for (const auto mode : {Mode::setup_failure, Mode::capture_start_failure, Mode::capture_failure,
                               Mode::render_start_failure, Mode::render_failure}) {
            Audit a; a.mode = mode; a.error = error; a.recover = true;
            Worker worker([&] { return std::make_unique<FakeSession>(a); });
            worker.start();
            await([&] { return a.made == 2 && worker.snapshot().state == State::running; });
            worker.stop();
            require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
            require(a.made == 2 && a.destroyed == 2 && a.stopped == 2 &&
                    !a.wrong_thread && !a.wrong_dispatch && !a.wrong_lifetime && !a.after_failure);
            require(worker.snapshot().setup.target_frames == 0 && worker.snapshot().metrics.core.input_frames == 0);
        }
    }
    { // Test the real watchdog concurrently to keep the complete self-test short.
        std::array<Audit, 4> audits;
        const std::array modes{Mode::no_audio, Mode::capture_only, Mode::render_only, Mode::prefill_timeout};
        std::vector<std::unique_ptr<Worker>> workers;
        for (std::size_t i = 0; i < audits.size(); ++i) {
            auto& a = audits[i]; a.mode = modes[i]; a.recover = true;
            workers.push_back(std::make_unique<Worker>([&a] { return std::make_unique<FakeSession>(a); }));
            workers.back()->start();
        }
        for (std::size_t i = 0; i < audits.size(); ++i) {
            auto& a = audits[i]; auto& worker = *workers[i];
            await([&] { return a.made == 2 && worker.snapshot().state == State::running; });
            worker.stop();
            require(a.made == 2 && a.destroyed == 2 && a.stopped == 2 &&
                    !a.wrong_thread && !a.wrong_dispatch && !a.wrong_lifetime && !a.after_failure);
            require(worker.snapshot().error == 0);
            if (modes[i] == Mode::no_audio) require(a.first_captured == 0 && a.first_render_started == 0);
            if (modes[i] == Mode::capture_only) require(a.first_captured > 2 && a.first_rendered == 0 && a.first_render_started == 1);
            if (modes[i] == Mode::render_only) require(a.first_captured == 2 && a.first_rendered > 0 && a.first_render_started == 1);
            if (modes[i] == Mode::prefill_timeout) require(a.first_captured > 2 && a.first_render_started == 0);
        }
    }
    { // Stop during the retry delay must cancel rather than create another session.
        Audit a; a.mode = Mode::setup_failure; a.error = device_invalidated;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.destroyed == 1 && worker.snapshot().stage == std::string_view("reinitialize"); });
        require(worker.snapshot().state == State::starting && worker.snapshot().setup.target_frames == 0);
        const auto before = std::chrono::steady_clock::now();
        worker.stop();
        require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
        require(a.made == 1 && a.destroyed == 1 && a.stopped == 1 && !a.after_failure && !a.wrong_lifetime);
        require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
    }
    { // Repeated invalidation keeps a bounded pause between fresh attempts.
        Audit a; a.mode = Mode::setup_failure; a.error = resources_invalidated;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.destroyed >= 3; }); worker.stop();
        require(a.last_created - a.first_created >= std::chrono::milliseconds(190));
        require(a.made == a.destroyed && a.made == a.stopped && !a.after_failure && !a.wrong_lifetime);
        require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
    }
    { // Cancellation also interrupts initialization of a replacement session.
        Audit a; a.mode = Mode::render_failure; a.error = resources_invalidated;
        a.recover = true; a.replacement = Mode::blocked_setup;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.initialized == 2; });
        require(worker.snapshot().setup.target_frames == 0 && worker.snapshot().metrics.core.input_frames == 0);
        worker.stop();
        require(a.made == 2 && a.capture_started == 1 && a.render_started == 1 && a.destroyed == 2 &&
                a.stopped == 2 && !a.after_failure && !a.wrong_lifetime);
        require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
    }
    { // An API timeout is not the event watchdog and must retain ordinary failure handling.
        Audit a; a.mode = Mode::setup_failure; a.error = static_cast<std::int32_t>(0x800705B4U);
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return worker.snapshot().state == State::failed; }); worker.stop();
        require(a.made == 1 && a.destroyed == 1 && worker.snapshot().error == a.error && !a.after_failure);
    }
}
#ifdef _WIN32
void formats() {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 22};
    ext.Samples.wValidBitsPerSample = 32; ext.dwChannelMask = 3;
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    require(native_format(ext.Format).encoding == Encoding::float32);
    ext.SubFormat = KSDATAFORMAT_SUBTYPE_PCM; ext.Samples.wValidBitsPerSample = 24;
    require(native_format(ext.Format).valid_bits == 24);
    const auto rejects = [](const WAVEFORMATEX& f) {
        bool rejected = false; try { (void)native_format(f); } catch (...) { rejected = true; } require(rejected);
    };
    auto bad = ext; bad.Format.cbSize = 0; rejects(bad.Format);
    bad = ext; bad.Format.nBlockAlign = 7; rejects(bad.Format);
    bad = ext; bad.Format.nAvgBytesPerSec = 1; rejects(bad.Format);
    bad = ext; bad.dwChannelMask = 12; rejects(bad.Format);
    bad = ext; bad.SubFormat = GUID_NULL; rejects(bad.Format);
    bad = ext; bad.Samples.wValidBitsPerSample = 20; rejects(bad.Format);
    WAVEFORMATEX pcm{WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0};
    require(native_format(pcm).encoding == Encoding::pcm16);
    pcm.wBitsPerSample = 24; pcm.nBlockAlign = 3; pcm.nAvgBytesPerSec = 132300;
    require(native_format(pcm).encoding == Encoding::pcm24);
    pcm.nChannels = 6; rejects(pcm);
    for (const auto encoding : {Encoding::pcm24, Encoding::pcm32, Encoding::pcm16}) {
        for (const unsigned channels : {1U, 2U}) {
            Format f{encoding, 48000, channels, encoding == Encoding::pcm16 ? 16U : 24U, 0, channels == 1 ? 4U : 3U};
            f.block_align = f.bytes_per_sample() * channels;
            for (const bool extensible : {true, false}) {
                if (!extensible && encoding == Encoding::pcm32) continue;
                const auto wave = exclusive_wave_format(f, extensible);
                const auto actual = native_format(wave.Format);
                require(actual.encoding == f.encoding && actual.rate == 48000 && actual.channels == channels &&
                    actual.valid_bits == f.valid_bits && actual.block_align == f.block_align &&
                    actual.channel_mask == (extensible ? f.channel_mask : 0));
            }
        }
    }
}
#endif
}
bool engine_self_test() {
    try {
        require(select_period({480,48,100,960}, Preset::low_latency) == 144);
        require(select_period({480,48,100,960}, Preset::stable) == 480);
        require(select_period({480,0,100,960}, Preset::low_latency) == 0);
        require(select_period({479,48,100,960}, Preset::stable) == 0);
        require(select_period({480,48,100,120}, Preset::low_latency) == 0);
        require(select_period({480,48,960,100}, Preset::low_latency) == 0);
        require(select_period({480,0xFFFFFFFFU,0xFFFFFFFEU,0xFFFFFFFEU}, Preset::low_latency) == 0);
        require(select_queue_target(48000,275,0.003,Preset::low_latency) == 576);
        require(select_queue_target(48000,275,0.003,Preset::stable) == 576);
        require(select_queue_target(44100,250,0.003,Preset::low_latency) == 530);
        require(select_queue_target(48000,1100,0.003,Preset::low_latency) == 1100);
        require(select_queue_target(48000,275,0.020,Preset::low_latency) == 960);
        require(select_queue_target(48000,275,0.010,Preset::stable) == 960);
        { FakeClient c; StreamInfo info;
          require(initialize_shared(c, Preset::low_latency, info) == 0);
          require(info.client3 && info.requested_frames == 144 && !info.fallback_reason && c.activations == 1 && c.legacy == 0); }
        for (int scenario = 0; scenario < 4; ++scenario) {
            FakeClient c; StreamInfo info;
            if (scenario == 0) c.query_result = failure;
            else c.modern_result = failure;
            if (scenario == 2) c.legacy_result = failure;
            if (scenario == 3) c.second_activation_result = failure;
            const auto result = initialize_shared(c, Preset::low_latency, info);
            require((result < 0) == (scenario >= 2));
            require(!info.client3 && info.fallback_reason == failure && c.activations == 2 && c.releases == 1);
            require(c.legacy == (scenario == 3 ? 0 : 1));
        }
        for (const auto error : {device_invalidated, resources_invalidated}) {
            for (const bool query : {true, false}) {
                FakeClient c; StreamInfo info;
                if (query) c.query_result = error; else c.modern_result = error;
                require(initialize_shared(c, Preset::low_latency, info) == error);
                require(c.activations == 1 && c.releases == 0 && c.legacy == 0);
            }
        }
        exclusive_setup();
        buffer_passes();
        lifecycle();
        session_rebuilds();
#ifdef _WIN32
        formats();
#endif
        return true;
    } catch (...) { return false; }
}
}
