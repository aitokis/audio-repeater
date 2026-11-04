#include "app_state.h"
#include <array>
#include <stdexcept>

namespace desktop {
namespace {
constexpr std::size_t max_id = 4096;
bool valid_id(std::string_view text) {
    if (text.size() > max_id || text.find('\0') != std::string_view::npos) return false;
    // Reject malformed UTF-8, overlong encodings, surrogates, and out-of-range scalars.
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) continue;
        unsigned count{}, code{}, minimum{};
        if (c >= 0xC2 && c <= 0xDF) { count = 1; code = c & 31; minimum = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { count = 2; code = c & 15; minimum = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { count = 3; code = c & 7; minimum = 0x10000; }
        else return false;
        if (i + count > text.size()) return false;
        while (count--) {
            c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xC0) != 0x80) return false;
            code = (code << 6) | (c & 63);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    }
    return true;
}
std::string encode(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
bool decode(std::string_view text, std::string& out) {
    if (text.size() > 2 * max_id || text.size() % 2) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0) return false;
        out += static_cast<char>((a << 4) | b);
    }
    return valid_id(out);
}
}
std::string serialize(const Preferences& value) {
    if (!valid_id(value.input_id) || !valid_id(value.output_id)) throw std::invalid_argument("Endpoint ID");
    return "AudioRepeaterWasapi 1\ninput=" + encode(value.input_id) + "\noutput=" + encode(value.output_id) +
        "\npreset=" + (value.preset == audio::Preset::stable ? "stable" : "low-latency") +
        "\nmode=" + (value.mode == audio::ShareMode::exclusive ? "exclusive" : "shared") +
        "\nresume=" + (value.resume ? "1\n" : "0\n");
}
bool parse(std::string_view text, Preferences& value) {
    if (text.size() > 17000 || !text.starts_with("AudioRepeaterWasapi 1\n")) return false;
    text.remove_prefix(std::string_view("AudioRepeaterWasapi 1\n").size());
    Preferences candidate;
    constexpr std::array keys{"input=", "output=", "preset=", "mode=", "resume="};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos) return false;
        auto line = text.substr(0, end);
        const std::string_view key(keys[i]);
        if (!line.starts_with(key)) return false;
        line.remove_prefix(key.size()); text.remove_prefix(end + 1);
        if (i == 0 && !decode(line, candidate.input_id)) return false;
        if (i == 1 && !decode(line, candidate.output_id)) return false;
        if (i == 2) {
            if (line == "stable") candidate.preset = audio::Preset::stable;
            else if (line != "low-latency") return false;
        }
        if (i == 3) {
            if (line == "exclusive") candidate.mode = audio::ShareMode::exclusive;
            else if (line != "shared") return false;
        }
        if (i == 4) { if (line != "0" && line != "1") return false; candidate.resume = line == "1"; }
    }
    if (!text.empty()) return false;
    value = std::move(candidate);
    return true;
}
void Recovery::failed(std::uint64_t now) noexcept {
    if (!wanted_) return;
    constexpr std::array<std::uint64_t, 6> delay{1000, 2000, 4000, 8000, 16000, 30000};
    next_ = now + delay[failures_];
    if (failures_ + 1 < delay.size()) ++failures_;
}
}
