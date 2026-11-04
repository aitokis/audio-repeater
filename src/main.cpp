#include "wasapi_engine.h"
#include "desktop_host.h"
#include <shellapi.h>
#include <objbase.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <algorithm>
#include <stdexcept>

namespace desktop {
// A GUI-subsystem process may already inherit redirected pipe handles. Preserve
// those for CTest/PowerShell; attach only when no usable stdout was inherited.
void console() {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) (void)AttachConsole(ATTACH_PARENT_PROCESS);
}
void write(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), size, nullptr, nullptr);
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
std::wstring hex(std::int32_t value) {
    std::wostringstream out;
    out << L"0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<std::uint32_t>(value);
    return out.str();
}
std::wstring format(const audio::Format& f) {
    const wchar_t* encoding = L"unknown";
    switch (f.encoding) {
    case audio::Encoding::float32: encoding = L"float32"; break;
    case audio::Encoding::pcm16: encoding = L"PCM16"; break;
    case audio::Encoding::pcm24: encoding = L"PCM24"; break;
    case audio::Encoding::pcm32: encoding = L"PCM32"; break;
    }
    std::wostringstream out;
    out << encoding << L" rate=" << f.rate << L" channels=" << f.channels << L" valid_bits=" << f.valid_bits;
    return out.str();
}
std::wstring stream_info(const wchar_t* name, const audio::StreamInfo& s) {
    std::wostringstream out;
    out << name << L": mode=" << (s.mode == audio::ShareMode::exclusive ? L"exclusive" : L"shared") << L" "
        << (s.format.valid() ? format(s.format) : L"format unavailable")
        << L"\n  requested_period_frames=" << s.requested_frames
        << L" supported_default/fundamental/min/max=" << s.supported.normal << L"/" << s.supported.fundamental
        << L"/" << s.supported.minimum << L"/" << s.supported.maximum
        << L"\n  actual_buffer_frames=" << s.buffer_frames << L" actual_period_frames=";
    if (s.actual_period_frames) out << s.actual_period_frames;
    else out << L"unavailable";
    out << L" default_period_100ns=" << s.default_period_100ns << L" minimum_period_100ns=" << s.minimum_period_100ns
        << L" requested_period_100ns=" << s.requested_period_100ns << L" alignment_retry=" << s.alignment_retry
        << L" API_stream_latency_100ns=" << s.latency_100ns
        << L"\n  initialization=" << (!s.initialized ? L"not completed" : s.mode == audio::ShareMode::exclusive ? L"exclusive/event" :
            s.client3 ? L"IAudioClient3" : L"shared/event fallback")
        << L" fallback_reason=" << (s.fallback_reason ? hex(s.fallback_reason) : L"none") << L"\n";
    return out.str();
}
std::wstring diagnostics(const audio::Snapshot& s) {
    std::wostringstream out;
    out << stream_info(L"Input", s.setup.input) << stream_info(L"Output", s.setup.output);
    out << L"target_input_frames=" << s.setup.target_frames << L" capacity_input_frames=" << s.setup.capacity_frames
        << L" queued_input_frames_including_SRC=" << s.metrics.queued_frames
        << L"\nFIFO/SRC_ms=" << (s.setup.input.format.rate ? 1000 * s.metrics.queued_frames / s.setup.input.format.rate : 0)
        << L" correction_ppm=" << s.metrics.correction_ppm
        << L"\nMMCSS=" << (s.setup.mmcss ? L"Audio" : L"unavailable") << L" MMCSS_error=" << s.setup.mmcss_error
        << L"\nunderflows=" << s.metrics.core.underflows << L" overflows=" << s.metrics.core.overflows
        << L" discontinuities=" << s.metrics.core.discontinuities << L" timestamp_errors=" << s.metrics.timestamp_errors
        << L" resyncs=" << s.metrics.core.resyncs << L" dropped_frames=" << s.metrics.core.dropped_frames
        << L" startup_silent_frames=" << s.metrics.core.startup_silent_frames
        << L" capture_silent_frames=" << s.metrics.capture_silent_frames
        << L" generated_frames=" << s.metrics.core.generated_frames << L"\n";
    return out.str();
}
int usage() {
    write(L"Usage: audio-repeater --self-test | --list-devices | --probe --input-id ID --output-id ID "
          L"[--duration-ms 1..5000] [--preset low-latency|stable] [--mode shared|exclusive]\n");
    return 2;
}
struct Com {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Com() { if (SUCCEEDED(result)) CoUninitialize(); }
};


int run(int argc, wchar_t** argv) {
    if (argc < 2) return usage();
    const std::wstring_view mode(argv[1]);
    if (mode == L"--self-test" && argc == 2) {
        const bool passed = audio::engine_self_test();
        write(passed ? L"self-test: passed (periods, fallback, formats, lifecycle, cancellation; no hardware routing)\n"
                     : L"self-test: FAILED\n");
        return passed ? 0 : 1;
    }
    if (mode == L"--list-devices" && argc == 2) {
        Com com;
        if (FAILED(com.result)) { write(L"COM initialization failed " + hex(com.result) + L"\n"); return 3; }
        for (const bool capture : {true, false}) {
            const auto endpoints = audio::enumerate_endpoints(capture);
            write(capture ? L"Input endpoints:\n" : L"Output endpoints:\n");
            if (endpoints.empty()) write(L"  No active endpoints (routing has not been tested).\n");
            for (const auto& e : endpoints) write(L"  " + e.name + L"\n    id=" + e.id + L"\n    native=" +
                (e.format_error ? L"unsupported/unavailable " + hex(e.format_error) : format(e.format)) + L"\n");
        }
        return 0;
    }
    if (mode != L"--probe") return usage();
    audio::Settings settings;
    unsigned duration = 1000;
    bool input = false, output = false, timing = false, preset = false, sharing = false;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) return usage();
        const std::wstring_view key(argv[i]), value(argv[i + 1]);
        if (value.empty()) return usage();
        if (key == L"--input-id" && !input) { settings.input_id = value; input = true; }
        else if (key == L"--output-id" && !output) { settings.output_id = value; output = true; }
        else if (key == L"--duration-ms" && !timing) {
            timing = true; duration = 0;
            if (value.size() > 4) return usage();
            for (const auto c : value) { if (c < L'0' || c > L'9') return usage(); duration = duration * 10 + static_cast<unsigned>(c - L'0'); }
            if (!duration || duration > 5000) return usage();
        } else if (key == L"--preset" && !preset) {
            preset = true;
            if (value == L"stable") settings.preset = audio::Preset::stable;
            else if (value != L"low-latency") return usage();
        } else if (key == L"--mode" && !sharing) {
            sharing = true;
            if (value == L"exclusive") settings.mode = audio::ShareMode::exclusive;
            else if (value != L"shared") return usage();
        } else return usage();
    }
    if (!input || !output) return usage();
    audio::Worker worker([settings] { return audio::make_wasapi_session(settings); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
    worker.start();
    bool running = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = worker.snapshot();
        if (snapshot.state == audio::State::failed || snapshot.state == audio::State::stopped) break;
        running = running || snapshot.state == audio::State::running;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.stop(); // always joins; no detached worker survives an exit path
    const auto snapshot = worker.snapshot();
    write(diagnostics(snapshot));
    if (snapshot.error < 0) {
        const bool invalidated = snapshot.error == static_cast<std::int32_t>(0x88890004U) ||
                                 snapshot.error == static_cast<std::int32_t>(0x88890026U);
        write(std::wstring(invalidated ? L"Endpoint invalidated; refresh/reselect before restarting. " : L"Probe failed. ") +
              L"stage=" + std::wstring(snapshot.stage, snapshot.stage + std::char_traits<char>::length(snapshot.stage)) +
              L" HRESULT=" + hex(snapshot.error) + L"\n");
        return 3;
    }
    if (!running) { write(L"Probe ended before audio startup completed; routing is unverified.\n"); return 4; }
    write(L"Probe stopped and worker joined. API/queue values are diagnostics, not measured end-to-end latency.\n");
    return 0;
}
}
using namespace desktop;
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    console();
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { write(L"Command line parsing failed.\n"); return 2; }
    int result = 3;
    try {
        if (argc == 1) result = gui(instance, show, false, false);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--gui-self-test") result = gui(instance, show, true, false);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--background") result = gui(instance, show, false, true);
        else if (argc == 5 && std::wstring_view(argv[1]) == L"--instance-self-test-child" &&
            (std::wstring_view(argv[4]) == L"normal" || std::wstring_view(argv[4]) == L"background"))
            result = instance_test_child(argv[2], argv[3], std::wstring_view(argv[4]) == L"background");
        else result = run(argc, argv);
    }
    catch (const audio::WasapiError& e) { write(L"Device enumeration failed HRESULT=" + hex(e.code) + L"; routing is unverified.\n"); }
    catch (...) { write(L"Device enumeration or audio initialization failed; no routing success is claimed.\n"); }
    LocalFree(argv);
    return result;
}
