#pragma once
#include "engine_runtime.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace desktop {
struct Preferences {
    std::string input_id, output_id; // UTF-8 endpoint identities, never friendly names
    audio::Preset preset{audio::Preset::low_latency};
    audio::ShareMode mode{audio::ShareMode::shared};
    bool resume{};
    bool configured() const noexcept { return !input_id.empty() && !output_id.empty(); }
    bool operator==(const Preferences&) const = default;
};
std::string serialize(const Preferences&);
bool parse(std::string_view, Preferences&);
// Controlling-thread policy: a failed worker does not cancel the user's request.
// Time is monotonic milliseconds. Device events bypass the retry delay.
class Recovery {
public:
    void start(std::uint64_t now) noexcept { wanted_ = true; failures_ = 0; next_ = now; }
    void stop() noexcept { wanted_ = false; failures_ = 0; next_ = 0; }
    void changed(std::uint64_t now) noexcept { if (wanted_) { next_ = now; failures_ = 0; } }
    void failed(std::uint64_t now) noexcept;
    void running() noexcept { failures_ = 0; }
    bool wanted() const noexcept { return wanted_; }
    bool due(std::uint64_t now) const noexcept { return wanted_ && now >= next_; }
private:
    bool wanted_{};
    unsigned failures_{};
    std::uint64_t next_{};
};
}
