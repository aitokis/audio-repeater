#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct SRC_STATE_tag;

namespace audio {

enum class Encoding { float32, pcm16, pcm24, pcm32 };
// Little endian interleaved samples. PCM valid bits are left aligned.
struct Format {
    Encoding encoding{};
    unsigned rate{};
    unsigned channels{};
    unsigned valid_bits{};
    unsigned block_align{};
    std::uint32_t channel_mask{}; // 0 unspecified; mono front-center or stereo L/R
    bool valid() const noexcept;
    unsigned bytes_per_sample() const noexcept;
};

// No overlap. Invalid format, extent, or channel count returns false without writes.
// Nonfinite input becomes zero. Integer output saturates; float output stays finite.
bool decode(const Format&, std::span<const std::byte>, std::span<float>,
            unsigned output_channels, bool silent = false) noexcept;
bool encode(const Format&, std::span<const float>, unsigned input_channels,
            std::span<std::byte>) noexcept;

// Single-thread owned, interleaved, preallocated frame queue. Partial operations
// return their actual frame counts. No synchronization or hidden allocation.
class Fifo {
public:
    Fifo(std::size_t capacity_frames, unsigned channels);
    std::size_t push(std::span<const float>) noexcept;
    std::size_t peek(std::span<float>) const noexcept;
    void discard(std::size_t frames) noexcept;
    void clear() noexcept;
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
private:
    std::vector<float> data_;
    std::size_t capacity_, head_{}, size_{};
    unsigned channels_;
};

struct ProcessResult { std::size_t consumed{}, generated{}; int error{}; };
class Resampler {
public:
    Resampler(unsigned input_rate, unsigned output_rate, unsigned channels);
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    ProcessResult process(std::span<const float>, std::span<float>, double ratio) noexcept;
    void reset() noexcept;
    double nominal_ratio() const noexcept { return nominal_; }
    double pending_input() const noexcept { return pending_; }
    std::size_t lookahead() const noexcept { return lookahead_; }
private:
    SRC_STATE_tag* state_{};
    unsigned channels_;
    double nominal_, last_ratio_{}, pending_{};
    std::size_t lookahead_;
};

class DriftController {
public:
    // Positive correction means consuming input faster: ratio=nominal/(1+ppm/1e6).
    double update(double queue_seconds, double target_seconds, double elapsed) noexcept;
    void reset() noexcept;
    double ppm() const noexcept { return correction_ * 1e6; }
private:
    double filtered_{}, integral_{}, correction_{}, elapsed_{};
};

struct Statistics {
    std::uint64_t input_frames{}, output_frames{}, consumed_frames{}, generated_frames{};
    std::uint64_t underflows{}, overflows{}, resyncs{}, dropped_frames{}, silent_frames{};
    std::uint64_t startup_silent_frames{}, discontinuities{}, src_errors{};
};

// A running transport owns its FIFO, SRC and scratch buffers. All public processing
// is single-thread owned and bounded. Construction validates and allocates once.
class Transport {
public:
    Transport(unsigned input_rate, unsigned output_rate, unsigned channels,
              std::size_t capacity_frames, std::size_t target_frames,
              std::size_t max_output_frames);
    bool push(std::span<const float>) noexcept;
    std::size_t render(std::span<float>) noexcept;
    void discontinuity() noexcept;
    double queued_input() const noexcept;
    double internal_input() const noexcept { return src_.pending_input(); }
    double correction_ppm() const noexcept { return drift_.ppm(); }
    std::size_t capacity() const noexcept { return fifo_.capacity(); }
    const Statistics& statistics() const noexcept { return stats_; }
private:
    void recover() noexcept;
    Fifo fifo_;
    Resampler src_;
    DriftController drift_;
    std::vector<float> scratch_;
    unsigned input_rate_, output_rate_, channels_;
    std::size_t target_, max_output_;
    bool started_{};
    Statistics stats_{};
};
}
