#include "engine_runtime.h"
#ifdef _WIN32
#include "wasapi_engine.h"
#include <ks.h>
#include <ksmedia.h>
#endif
#include <chrono>
#include <condition_variable>
#include <stdexcept>

namespace audio {
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error("engine self-test failed"); }
constexpr auto failure = static_cast<std::int32_t>(0x88890004U); // device invalidated
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
enum class Mode { normal, setup_failure, capture_start_failure, capture_failure,
                  render_start_failure, render_failure, wait_failure, blocked_setup, no_audio };
struct Audit {
    std::atomic<int> made{}, initialized{}, capture_started{}, render_started{}, captured{}, rendered{}, stopped{}, destroyed{};
    std::atomic<bool> wrong_thread{};
    Mode mode{Mode::normal};
};
class FakeSession final : public Session {
public:
    explicit FakeSession(Audit& a) : audit_(a), owner_(std::this_thread::get_id()) { ++audit_.made; }
    ~FakeSession() override { own(); ++audit_.destroyed; }
    std::int32_t initialize(std::stop_token token, SetupInfo&) override {
        own(); token_ = token; ++audit_.initialized;
        if (audit_.mode == Mode::blocked_setup) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, token, [] { return false; });
        }
        return audit_.mode == Mode::setup_failure ? failure : 0;
    }
    std::int32_t start_capture() noexcept override {
        own(); ++audit_.capture_started;
        return audit_.mode == Mode::capture_start_failure ? failure : 0;
    }
    Wake wait() noexcept override {
        own();
        if (audit_.mode == Mode::wait_failure) return Wake::failed;
        if (audit_.mode == Mode::no_audio) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); return Wake::idle;
        }
        if (token_.stop_requested()) return Wake::stopped;
        std::this_thread::yield(); return Wake::audio;
    }
    std::int32_t capture(std::stop_token) noexcept override {
        own(); ++audit_.captured; return audit_.mode == Mode::capture_failure ? failure : 0;
    }
    bool ready() const noexcept override { return audit_.captured > 0; }
    std::int32_t start_render() noexcept override {
        own(); ++audit_.render_started; return audit_.mode == Mode::render_start_failure ? failure : 0;
    }
    std::int32_t render() noexcept override {
        own(); ++audit_.rendered; return audit_.mode == Mode::render_failure ? failure : 0;
    }
    Metrics metrics() const noexcept override { return {}; }
    void stop() noexcept override { own(); ++audit_.stopped; }
private:
    void own() noexcept { if (owner_ != std::this_thread::get_id()) audit_.wrong_thread = true; }
    Audit& audit_;
    std::thread::id owner_;
    std::stop_token token_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
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
        require(a.destroyed == 2 && a.stopped == 2 && !a.wrong_thread);
        require(worker.snapshot().state == State::stopped && worker.snapshot().error == 0);
    }
    { // Cancellation while initialization waits must never start either stream.
        Audit a; a.mode = Mode::blocked_setup;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return a.initialized == 1; }); worker.stop();
        require(a.capture_started == 0 && a.render_started == 0 && a.destroyed == 1 && !a.wrong_thread);
    }
    { // Destroying/closing a running owner cancels and joins before returning.
        Audit a;
        { Worker worker([&] { return std::make_unique<FakeSession>(a); });
          worker.start(); await([&] { return a.rendered > 20; }); }
        require(a.stopped == 1 && a.destroyed == 1 && !a.wrong_thread);
    }
    { // An event source that never becomes ready must have a finite failure path.
        Audit a; a.mode = Mode::no_audio;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        worker.start(); await([&] { return worker.snapshot().state == State::failed; }); worker.stop();
        require(a.render_started == 0 && a.destroyed == 1 && worker.snapshot().error < 0);
    }
    { // Rapid cancellation remains safe before/after worker thread construction.
        Audit a;
        Worker worker([&] { return std::make_unique<FakeSession>(a); });
        for (int i = 0; i < 20; ++i) { worker.start(); worker.stop(); }
        require(a.made == 20 && a.destroyed == 20 && a.stopped == 20 && !a.wrong_thread);
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
        lifecycle();
#ifdef _WIN32
        formats();
#endif
        return true;
    } catch (...) { return false; }
}
}
