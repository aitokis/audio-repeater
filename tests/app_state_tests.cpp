#include "app_state.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value) { if (!value) throw std::runtime_error("Desktop policy regression"); }
}
int main() {
    try {
        using namespace desktop;
        const Preferences original{"capture-\xe6\x97\xa5\n=\xf0\x9f\x8e\xa7", "render-{ABC}",
            audio::Preset::stable, audio::ShareMode::exclusive, true};
        const auto text = serialize(original);
        Preferences actual;
        require(parse(text, actual) && actual == original && actual.configured());
        require(parse(serialize(Preferences{}), actual) && !actual.configured() && !actual.resume);
        for (std::size_t n = 0; n < text.size(); ++n) {
            actual = original; require(!parse(text.substr(0, n), actual) && actual == original);
        }
        for (const auto& broken : std::vector<std::string>{text + "trailing", "AudioRepeaterWasapi 2\n", "", std::string(17001, 'x')})
            require(!parse(broken, actual));
        auto corrupt = text; corrupt.replace(corrupt.find("mode=exclusive"), 14, "mode=automatic");
        require(!parse(corrupt, actual));
        for (const auto& id : std::vector<std::string>{std::string("a\0b", 3), "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", std::string(4097, 'x')}) {
            auto bad = original; bad.input_id = id;
            bool rejected{}; try { (void)serialize(bad); } catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        }
        Recovery policy;
        require(!policy.due(0)); policy.start(100); require(policy.due(100));
        policy.failed(100); require(!policy.due(1099) && policy.due(1100));
        policy.failed(1100); require(!policy.due(3099) && policy.due(3100));
        policy.changed(1200); require(policy.due(1200)); // reconnect bypasses backoff
        policy.failed(1200); require(!policy.due(2199) && policy.due(2200));
        policy.running(); policy.failed(5000); require(policy.due(6000) && !policy.due(5999));
        for (unsigned n = 0; n < 12; ++n) policy.failed(7000);
        require(!policy.due(36999) && policy.due(37000)); // bounded retries
        policy.stop(); policy.changed(40000); policy.failed(40000);
        require(!policy.wanted() && !policy.due(100000));
        policy.start(50000); policy.stop(); require(!policy.due(50000));
        std::cout << "desktop policy: passed (strict config, UTF-8, corruption, immediate reconnect, bounded backoff, Stop cancellation)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
