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
enum class ShareMode { shared, exclusive };
struct Periods { std::uint32_t normal{}, fundamental{}, minimum{}, maximum{}; };
std::uint32_t select_period(Periods, Preset) noexcept;
bool engine_self_test();
struct StreamInfo {
    Format format{};
    ShareMode mode{ShareMode::shared};
    Periods supported{};
    std::uint32_t requested_frames{}, actual_period_frames{}, buffer_frames{};
    std::int64_t default_period_100ns{}, minimum_period_100ns{}, requested_period_100ns{}, latency_100ns{};
    std::int32_t fallback_reason{};
    bool client3{}, initialized{}, alignment_retry{};
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
enum class Wake { capture, render, idle, stopped, failed };
// A missing exclusive packet is recoverable only while its endpoint is running.
// Each call is one event's single acquisition attempt; processing requires S_OK.
class BufferPass {
public:
    template<class Acquire, class Process>
    std::int32_t run(ShareMode mode, bool running, Acquire acquire, Process process) noexcept {
        const auto result = acquire();
        if (result == static_cast<std::int32_t>(0x88890018U) && mode == ShareMode::exclusive && running) {
            if (consecutive_ == 3) return result;
            ++consecutive_;
            return 1; // defer until the next event, without processing/releasing
        }
        if (result != 0) return result;
        consecutive_ = 0;
        return process();
    }
private:
    unsigned consecutive_{};
};
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

// Exclusive support is established only by an exact format probe. Basic PCM is
// also tried for packed samples because some drivers reject EXTENSIBLE layouts.
template<class Client>
std::int32_t initialize_exclusive(Client& client, bool capture, Preset preset, StreamInfo& info) {
    constexpr auto exclusive_invalid_argument = static_cast<std::int32_t>(0x80070057U);
    constexpr auto exclusive_unsupported_format = static_cast<std::int32_t>(0x88890008U);
    constexpr auto exclusive_buffer_not_aligned = static_cast<std::int32_t>(0x88890019U);
    info.mode = ShareMode::exclusive;
    auto result = client.activate();
    if (result < 0) return result;
    bool found = false;
    for (const unsigned channels : {2U, 1U}) {
        for (unsigned variant = 0; variant < (capture ? 3U : 2U); ++variant) {
            Format f{capture ? (variant == 2 ? Encoding::pcm32 : Encoding::pcm24) : Encoding::pcm16,
                     48000, channels, capture ? 24U : 16U, 0, variant == 1 ? 0U : channels == 2 ? 3U : 4U};
            f.block_align = f.bytes_per_sample() * channels;
            result = client.supports_format(f, variant != 1);
            if (result == 0) { info.format = f; found = true; break; }
            if (result < 0 && result != exclusive_unsupported_format && result != exclusive_invalid_argument) return result;
        }
        if (found) break;
    }
    if (!found) return exclusive_unsupported_format;
    result = client.device_periods(info.default_period_100ns, info.minimum_period_100ns);
    if (result < 0) return result;
    if (info.minimum_period_100ns <= 0 || info.default_period_100ns < info.minimum_period_100ns ||
        info.default_period_100ns > 5'000'000) return exclusive_invalid_argument;
    info.requested_period_100ns = preset == Preset::stable ? info.default_period_100ns : info.minimum_period_100ns;
    info.requested_frames = static_cast<std::uint32_t>((info.requested_period_100ns * 48000 + 5'000'000) / 10'000'000);
    result = client.initialize_exclusive(info.requested_period_100ns);
    if (result != exclusive_buffer_not_aligned) return result;
    // GetBufferSize after NOT_ALIGNED provides the driver's required frame count.
    // Only one retry is allowed, on a newly activated client.
    std::uint32_t frames = 0;
    result = client.buffer_size(frames);
    if (result < 0) return result;
    if (!frames || frames > 24000) return exclusive_invalid_argument;
    const auto duration = (std::int64_t(frames) * 10'000'000 + 24000) / 48000;
    if (duration < info.minimum_period_100ns) return exclusive_invalid_argument;
    info.alignment_retry = true;
    info.requested_frames = frames; info.requested_period_100ns = duration;
    client.release();
    result = client.activate();
    return result < 0 ? result : client.initialize_exclusive(duration);
}

template<class Client>
std::int32_t render_request(Client& client, ShareMode mode, std::uint32_t buffer_frames, std::uint32_t& frames) noexcept {
    frames = buffer_frames;
    if (mode == ShareMode::exclusive) return 0;
    std::uint32_t padding = 0;
    const auto result = client.padding(padding);
    if (result < 0) return result;
    if (padding > buffer_frames) return static_cast<std::int32_t>(0x8000FFFFU);
    frames -= padding;
    return 0;
}
}
