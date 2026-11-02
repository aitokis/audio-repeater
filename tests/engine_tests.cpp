#include "engine_runtime.h"
#include <cstdio>
int main() {
    const bool passed = audio::engine_self_test();
    std::puts(passed ? "engine lifecycle/period/fallback self-test passed" : "engine self-test FAILED");
    return passed ? 0 : 1;
}
