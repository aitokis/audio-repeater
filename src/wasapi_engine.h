#pragma once
#include "engine_runtime.h"
#include <windows.h>
#include <mmreg.h>
#include <string>
#include <vector>

namespace audio {
struct WasapiError { HRESULT code; };
struct Endpoint {
    std::wstring id, name;
    Format format{};
    std::int32_t format_error{};
};
// The caller initializes COM in its apartment for this finite enumeration.
std::vector<Endpoint> enumerate_endpoints(bool capture);
Format native_format(const WAVEFORMATEX&);
struct Settings {
    std::wstring input_id, output_id;
    Preset preset{Preset::low_latency};
};
std::unique_ptr<Session> make_wasapi_session(Settings);
bool engine_self_test();
}
