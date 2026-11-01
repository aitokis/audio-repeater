#pragma once
#include "audio_core.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace audio {
static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);
enum class Preset { low_latency, stable };
struct Periods { std::uint32_t normal{}, fundamental{}, minimum{}, maximum{}; };
std::uint32_t select_period(Periods, Preset) noexcept;
bool engine_self_test();
struct StreamInfo {
    Format format{};
    Periods supported{};
    std::uint32_t requested_frames{}, actual_period_frames{}, buffer_frames{};
    std::int64_t default_period_100ns{}, latency_100ns{};
    std::int32_t fallback_reason{};
    bool client3{}, initialized{};
};
struct SetupInfo {
    StreamInfo input{}, output{};
    std::size_t target_frames{}, capacity_frames{};
    bool mmcss{};
    std::uint32_t mmcss_error{};
};
struct Metrics {
    Statistics core{};
    std::uint64_t timestamp_errors{}, capture_silent_frames{};
    double queued_frames{}, correction_ppm{};
};
enum class State { stopped, starting, running, failed };
struct Snapshot {
    State state{};
    SetupInfo setup{};
    Metrics metrics{}; // approximate low-frequency counters, not an audio clock
    std::int32_t error{};
    const char* stage{"idle"};
};
enum class Wake { audio, idle, stopped, failed };
// Real WASAPI and deterministic fake sessions share this lifecycle. A session is
// constructed, used and destroyed on the worker; no apartment-owned objects cross it.
class Session {
public:
    virtual ~Session() = default;
    virtual std::int32_t initialize(std::stop_token, SetupInfo&) = 0;
    virtual std::int32_t start_capture() noexcept = 0;
    virtual Wake wait() noexcept = 0;
    virtual std::int32_t capture(std::stop_token) noexcept = 0;
    virtual bool ready() const noexcept = 0;
    virtual std::int32_t start_render() noexcept = 0;
    virtual std::int32_t render() noexcept = 0;
    virtual Metrics metrics() const noexcept = 0;
    virtual void stop() noexcept = 0;
};
class Worker {
public:
    using Factory = std::function<std::unique_ptr<Session>()>;
    explicit Worker(Factory factory) : factory_(std::move(factory)) {}
    ~Worker() { stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Start/stop belong to one controlling thread. snapshot is safe from that thread.
    void start();
    void stop() noexcept;
    Snapshot snapshot() const;
private:
    void run(std::stop_token) noexcept;
    void publish(Metrics) noexcept;
    Factory factory_;
    std::jthread thread_;
    std::atomic<State> state_{State::stopped};
    mutable std::mutex setup_mutex_;
    SetupInfo setup_{};
    std::atomic<std::int32_t> error_{};
    std::atomic<const char*> stage_{"idle"};
    std::atomic<std::uint64_t> input_{}, output_{}, consumed_{}, generated_{};
    std::atomic<std::uint64_t> underflows_{}, overflows_{}, resyncs_{}, dropped_{}, silent_{};
    std::atomic<std::uint64_t> startup_{}, discontinuities_{}, src_errors_{}, timestamps_{}, capture_silent_{};
    std::atomic<double> queue_{}, ppm_{};
};

// Both adapters implement these operations; fallback always destroys the failed
// client before activating a fresh IAudioClient. Failure is attempted once only.
template<class Client>
std::int32_t initialize_shared(Client& client, Preset preset, StreamInfo& info) {
    auto result = client.activate();
    if (result < 0) return result;
    result = client.periods(info.supported);
    if (result >= 0) {
        info.requested_frames = select_period(info.supported, preset);
        if (info.requested_frames) result = client.initialize3(info.requested_frames);
        else result = static_cast<std::int32_t>(0x80070057U);
    }
    if (result >= 0) { info.client3 = true; return result; }
    info.fallback_reason = result;
    client.release();
    result = client.activate();
    return result < 0 ? result : client.initialize_legacy();
}
}
