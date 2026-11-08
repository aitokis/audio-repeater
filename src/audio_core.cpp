#include "audio_core.h"
#include <samplerate.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace audio {
namespace {
float finite(float x) noexcept { return std::isfinite(x) ? x : 0.0F; }
std::size_t samples(std::size_t frames, unsigned channels) {
    if ((channels != 1 && channels != 2) || frames == 0 ||
        frames > 1'000'000 || frames > std::numeric_limits<std::size_t>::max() / channels)
        throw std::invalid_argument("Invalid audio buffer dimensions");
    return frames * channels;
}
float mapped(std::span<const float> data, std::size_t frame, unsigned in,
             unsigned out, unsigned channel) noexcept {
    const float left = finite(data[frame * in]);
    if (in == 1) return left;
    const float right = finite(data[frame * in + 1]);
    if (out == 1) return left * 0.5F + right * 0.5F;
    return channel == 0 ? left : right;
}
std::uint32_t read_word(const std::byte* p, unsigned bytes) noexcept {
    std::uint32_t word = 0;
    for (unsigned i = 0; i < bytes; ++i)
        word |= std::uint32_t(std::to_integer<unsigned char>(p[i])) << (8 * i);
    return word;
}
float unpack(const std::byte* p, const Format& f) noexcept {
    auto word = read_word(p, f.bytes_per_sample());
    if (f.encoding == Encoding::float32) return finite(std::bit_cast<float>(word));
    const unsigned bits = 8 * f.bytes_per_sample();
    if (bits < 32 && (word & (1U << (bits - 1)))) word |= ~((1U << bits) - 1);
    if (bits == 32 && f.valid_bits == 24) word &= 0xFFFFFF00U;
    auto value = std::bit_cast<std::int32_t>(word);
    // PCM32 valid24 is MSB aligned; ignore the unused low byte.
    if (bits == 32 && f.valid_bits == 24)
        return static_cast<float>(value / 256) / 8388608.0F;
    return static_cast<float>(static_cast<double>(value) / std::ldexp(1.0, bits - 1));
}
}

unsigned Format::bytes_per_sample() const noexcept {
    switch (encoding) {
    case Encoding::float32: case Encoding::pcm32: return 4;
    case Encoding::pcm16: return 2;
    case Encoding::pcm24: return 3;
    }
    return 0;
}
bool Format::valid() const noexcept {
    if (rate < 8000 || rate > 192000 || (channels != 1 && channels != 2)) return false;
    const unsigned bytes = bytes_per_sample();
    if (!bytes || block_align != bytes * channels) return false;
    if (channel_mask != 0 && channel_mask != (channels == 1 ? 4U : 3U)) return false;
    if (encoding == Encoding::pcm32) return valid_bits == 24 || valid_bits == 32;
    return valid_bits == bytes * 8;
}
bool decode(const Format& f, std::span<const std::byte> input, std::span<float> output,
            unsigned channels, bool silent) noexcept {
    if (!f.valid() || (channels != 1 && channels != 2) || output.size() % channels) return false;
    const auto frames = output.size() / channels;
    if (!silent && (input.size() % f.block_align || input.size() / f.block_align != frames)) return false;
    for (std::size_t i = 0; i < frames; ++i) {
        float left = 0, right = 0;
        if (!silent) {
            const auto* p = input.data() + i * f.block_align;
            left = unpack(p, f);
            right = f.channels == 2 ? unpack(p + f.bytes_per_sample(), f) : left;
        }
        output[i * channels] = channels == 1 && f.channels == 2 ? left * 0.5F + right * 0.5F : left;
        if (channels == 2) output[i * channels + 1] = right;
    }
    return true;
}
bool encode(const Format& f, std::span<const float> input, unsigned channels,
            std::span<std::byte> output) noexcept {
    if (!f.valid() || (channels != 1 && channels != 2) || input.size() % channels ||
        output.size() % f.block_align || input.size() / channels != output.size() / f.block_align) return false;
    for (std::size_t i = 0; i < input.size() / channels; ++i) {
        for (unsigned c = 0; c < f.channels; ++c) {
            const float value = mapped(input, i, channels, f.channels, c);
            std::uint32_t word;
            if (f.encoding == Encoding::float32) word = std::bit_cast<std::uint32_t>(value);
            else {
                const double scale = std::ldexp(1.0, f.valid_bits - 1);
                const auto quantized = static_cast<std::int64_t>(std::round(std::clamp(
                    static_cast<double>(value) * scale, -scale, scale - 1)));
                word = static_cast<std::uint32_t>(quantized);
                if (f.bytes_per_sample() == 4 && f.valid_bits == 24) word <<= 8;
            }
            auto* p = output.data() + i * f.block_align + c * f.bytes_per_sample();
            for (unsigned b = 0; b < f.bytes_per_sample(); ++b)
                p[b] = std::byte((word >> (b * 8)) & 255U);
        }
    }
    return true;
}

Fifo::Fifo(std::size_t capacity, unsigned channels)
    : data_(samples(capacity, channels)), capacity_(capacity), channels_(channels) {}
std::size_t Fifo::push(std::span<const float> input) noexcept {
    if (input.size() % channels_) return 0;
    const auto count = std::min(input.size() / channels_, capacity_ - size_);
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned c = 0; c < channels_; ++c)
            data_[((head_ + size_ + i) % capacity_) * channels_ + c] = finite(input[i * channels_ + c]);
    size_ += count;
    return count;
}
std::size_t Fifo::peek(std::span<float> output) const noexcept {
    if (output.size() % channels_) return 0;
    const auto count = std::min(output.size() / channels_, size_);
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned c = 0; c < channels_; ++c)
            output[i * channels_ + c] = data_[((head_ + i) % capacity_) * channels_ + c];
    return count;
}
void Fifo::discard(std::size_t frames) noexcept {
    frames = std::min(frames, size_);
    head_ = (head_ + frames) % capacity_;
    size_ -= frames;
}
void Fifo::clear() noexcept { head_ = size_ = 0; }

Resampler::Resampler(unsigned input_rate, unsigned output_rate, unsigned channels)
    : channels_(channels), nominal_(0), lookahead_(0) {
    if (input_rate < 8000 || input_rate > 192000 || output_rate < 8000 || output_rate > 192000 ||
        (channels != 1 && channels != 2)) throw std::invalid_argument("Invalid SRC format");
    nominal_ = static_cast<double>(output_rate) / input_rate;
    lookahead_ = static_cast<std::size_t>(std::ceil(64 / std::min(nominal_ / 1.002, 1.0))) + 64;
    int error = 0;
    state_ = src_new(SRC_SINC_FASTEST, static_cast<int>(channels), &error);
    if (!state_) throw std::runtime_error(src_strerror(error));
}
Resampler::~Resampler() { src_delete(state_); }
void Resampler::reset() noexcept { src_reset(state_); last_ratio_ = pending_ = 0; }
ProcessResult Resampler::process(std::span<const float> input, std::span<float> output, double ratio) noexcept {
    if (!std::isfinite(ratio) || ratio < nominal_ / 1.002 - 1e-12 || ratio > nominal_ / 0.998 + 1e-12 ||
        input.size() % channels_ || output.size() % channels_ ||
        input.size() / channels_ > static_cast<std::size_t>(std::numeric_limits<long>::max()) ||
        output.size() / channels_ > static_cast<std::size_t>(std::numeric_limits<long>::max())) return {0, 0, -1};
    if (output.empty()) return {};
    // src_process requires nonnull input even when the extent is zero.
    const float empty = 0;
    SRC_DATA data{};
    data.data_in = input.empty() ? &empty : input.data();
    data.data_out = output.data();
    data.input_frames = static_cast<long>(input.size() / channels_);
    data.output_frames = static_cast<long>(output.size() / channels_);
    data.src_ratio = ratio;
    const int error = src_process(state_, &data);
    if (error) return {0, 0, error};
    // Pinned sinc implementation advances by 1/ratio for each generated frame.
    // It interpolates from last_ratio toward target over output_frames (not gen).
    // Mirror that phase advance to include input buffered inside SRC in queue size.
    const double start = last_ratio_ == 0 ? ratio : last_ratio_;
    const bool changing = std::abs(start - ratio) > 1e-10;
    double advance = 0, actual = start;
    for (long i = 0; i < data.output_frames_gen; ++i) {
        if (changing) actual = start + static_cast<double>(i) * (ratio - start) / data.output_frames;
        advance += 1 / actual;
    }
    const auto generated_samples = static_cast<std::size_t>(data.output_frames_gen) * channels_;
    for (std::size_t i = 0; i < generated_samples; ++i) output[i] = finite(output[i]);
    last_ratio_ = actual;
    pending_ += static_cast<double>(data.input_frames_used) - advance;
    return {static_cast<std::size_t>(data.input_frames_used),
            static_cast<std::size_t>(data.output_frames_gen), 0};
}

void DriftController::reset() noexcept { filtered_ = integral_ = correction_ = elapsed_ = 0; }
double DriftController::update(double queue, double target, double elapsed) noexcept {
    if (!std::isfinite(queue) || !std::isfinite(target) || !std::isfinite(elapsed) || elapsed <= 0) return ppm();
    elapsed_ += elapsed;
    if (elapsed_ < 0.1 - 1e-12) return ppm();
    const double dt = elapsed_;
    elapsed_ = 0;
    const double error = queue - target;
    filtered_ += (1 - std::exp(-dt)) * (error - filtered_);
    const double proportional = 0.2 * filtered_;
    const double candidate = integral_ + 0.01 * filtered_ * dt;
    // Conditional integration prevents windup when saturated in the error direction.
    if ((proportional + candidate <= 0.002 || filtered_ < 0) &&
        (proportional + candidate >= -0.002 || filtered_ > 0))
        integral_ = std::clamp(candidate, -0.002, 0.002);
    const double desired = std::clamp(proportional + integral_, -0.002, 0.002);
    correction_ += std::clamp(desired - correction_, -0.0002 * dt, 0.0002 * dt);
    correction_ = std::clamp(correction_, -0.002, 0.002);
    return ppm();
}

Transport::Transport(unsigned in_rate, unsigned out_rate, unsigned channels,
                     std::size_t capacity, std::size_t target, std::size_t max_out)
    : fifo_(capacity, channels), src_(in_rate, out_rate, channels),
      scratch_(samples(capacity, channels)), input_rate_(in_rate), output_rate_(out_rate),
      channels_(channels), target_(target), max_output_(max_out) {
    if (max_out == 0 || max_out > 1'000'000) throw std::invalid_argument("Invalid render block size");
    const auto demand = static_cast<std::size_t>(std::ceil(static_cast<double>(max_out) / (src_.nominal_ratio() / 1.002)));
    if (target <= demand + src_.lookahead() ||
        target > capacity / 4 || demand + src_.lookahead() > capacity - target)
        throw std::invalid_argument("Insufficient transport capacity or prefill");
}
double Transport::queued_input() const noexcept {
    return static_cast<double>(fifo_.size()) + std::max(0.0, src_.pending_input());
}
void Transport::recover() noexcept {
    stats_.dropped_frames += static_cast<std::uint64_t>(std::ceil(queued_input()));
    ++stats_.resyncs;
    fifo_.clear(); src_.reset(); drift_.reset(); started_ = false;
}
void Transport::discontinuity() noexcept { ++stats_.discontinuities; }
bool Transport::push(std::span<const float> input) noexcept {
    if (input.size() % channels_) return false;
    const auto frames = input.size() / channels_;
    stats_.input_frames += frames;
    const bool overflow = queued_input() + static_cast<double>(frames) > static_cast<double>(capacity());
    if (overflow) ++stats_.overflows;
    if (overflow || queued_input() + static_cast<double>(frames) >= 2.0 * target_) {
        // SRC history is retained. Fit the newest FIFO/input frames into the
        // remaining target budget, rounding down for fractional SRC backlog.
        const auto budget = static_cast<std::size_t>(std::max(0.0,
            std::floor(static_cast<double>(target_) - std::max(0.0, src_.pending_input()))));
        const auto keep_input = std::min(frames, budget);
        const auto keep_fifo = std::min(fifo_.size(), budget - keep_input);
        stats_.dropped_frames += fifo_.size() - keep_fifo + frames - keep_input;
        ++stats_.resyncs;
        fifo_.discard(fifo_.size() - keep_fifo);
        fifo_.push(input.subspan((frames - keep_input) * channels_));
        return false;
    }
    return fifo_.push(input) == frames;
}
std::size_t Transport::render(std::span<float> output) noexcept {
    if (output.size() % channels_ || output.size() / channels_ > max_output_) return 0;
    const auto frames = output.size() / channels_;
    std::fill(output.begin(), output.end(), 0.0F);
    stats_.output_frames += frames;
    if (!frames) return 0;
    if (!started_) {
        if (queued_input() < static_cast<double>(target_)) {
            stats_.startup_silent_frames += frames;
            return 0;
        }
        started_ = true;
    }
    const double ppm = drift_.update(queued_input() / input_rate_, static_cast<double>(target_) / input_rate_,
                                     static_cast<double>(frames) / output_rate_);
    const double ratio = src_.nominal_ratio() / (1 + ppm * 1e-6);
    std::size_t generated = 0;
    // Every productive iteration consumes >=1 input frame or produces >=1 output.
    // An explicit bound also protects against upstream nonprogress behavior.
    for (std::size_t attempt = 0; generated < frames && attempt <= frames + fifo_.capacity(); ++attempt) {
        const double required = std::ceil(static_cast<double>(frames - generated) / ratio) + static_cast<double>(src_.lookahead()) - src_.pending_input();
        const auto feed = std::min(fifo_.size(), static_cast<std::size_t>(std::max(0.0, required)));
        fifo_.peek(std::span<float>(scratch_.data(), feed * channels_));
        const auto result = src_.process(std::span<const float>(scratch_.data(), feed * channels_),
                                        output.subspan(generated * channels_), ratio);
        if (result.error) { ++stats_.src_errors; recover(); break; }
        fifo_.discard(result.consumed);
        stats_.consumed_frames += result.consumed;
        stats_.generated_frames += result.generated;
        generated += result.generated;
        if (!result.consumed && !result.generated) break;
    }
    if (generated < frames) {
        ++stats_.underflows;
        stats_.silent_frames += frames - generated;
        // Output was zeroed before processing. Keep its generated prefix and
        // retain FIFO, SRC, priming and drift history for the next capture packet.
    }
    return generated;
}
}
