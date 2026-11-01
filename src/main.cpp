#include "wasapi_engine.h"
#include <shellapi.h>
#include <objbase.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <algorithm>
#include <stdexcept>

namespace {
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
    out << name << L": " << format(s.format)
        << L"\n  requested_period_frames=" << s.requested_frames
        << L" supported_default/fundamental/min/max=" << s.supported.normal << L"/" << s.supported.fundamental
        << L"/" << s.supported.minimum << L"/" << s.supported.maximum
        << L"\n  actual_buffer_frames=" << s.buffer_frames << L" actual_period_frames=";
    if (s.actual_period_frames) out << s.actual_period_frames;
    else out << L"unavailable";
    out << L" default_period_100ns=" << s.default_period_100ns << L" API_stream_latency_100ns=" << s.latency_100ns
        << L"\n  initialization=" << (!s.initialized ? L"not completed" : s.client3 ? L"IAudioClient3" : L"shared/event fallback")
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
          L"[--duration-ms 1..5000] [--preset low-latency|stable]\n");
    return 2;
}
struct Com {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Com() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// All HWNDs, endpoint values, and Worker start/stop calls belong to this thread.
// The audio thread never calls UI code or waits for a UI lock.
struct Window {
    enum : int { input_id = 101, output_id, preset_id, refresh_id, start_id, stop_id };
    HINSTANCE instance;
    HWND hwnd{}, input{}, output{}, preset{}, refresh_button{}, start_button{}, stop_button{}, status{}, details{};
    std::vector<audio::Endpoint> inputs, outputs;
    std::unique_ptr<audio::Worker> worker;
    bool active{}, testing{}, passed{};
    unsigned test_step{}, ticks{};
    int exit_code{};

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        auto child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!child) throw std::runtime_error("CreateWindowEx");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return child;
    }
    static std::wstring selected(HWND combo, const std::vector<audio::Endpoint>& endpoints) {
        const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < endpoints.size()
            ? endpoints[static_cast<std::size_t>(index)].id : L"";
    }
    static bool supported(HWND combo, const std::vector<audio::Endpoint>& endpoints) {
        const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<std::size_t>(index) < endpoints.size() &&
            !endpoints[static_cast<std::size_t>(index)].format_error;
    }
    static void fill(HWND combo, const std::vector<audio::Endpoint>& endpoints, const std::wstring& previous) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int index = previous.empty() && !endpoints.empty() ? 0 : -1;
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            const auto text = endpoints[i].name + (endpoints[i].format_error ? L" (unsupported/unavailable)" : L"");
            if (SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())) < 0)
                throw std::runtime_error("CB_ADDSTRING");
            if (endpoints[i].id == previous) index = static_cast<int>(i);
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    void enable() {
        for (auto c : {input, output, preset, refresh_button}) EnableWindow(c, !active);
        EnableWindow(start_button, !active && supported(input, inputs) && supported(output, outputs));
        EnableWindow(stop_button, active);
    }
    void selection_status() {
        std::wstring message = L"Ready. Select endpoints and press Start.";
        if (inputs.empty() || outputs.empty())
            message = L"No active " + std::wstring(inputs.empty() ? L"input" : L"output") +
                L" endpoints. Connect/enable devices, then Refresh. Routing is unverified.";
        else if (!supported(input, inputs) || !supported(output, outputs))
            message = L"Select supported input and output endpoints. A removed selection is not replaced automatically.";
        SetWindowTextW(status, message.c_str());
        const auto text = L"Selected input ID: " + selected(input, inputs) + L"\r\nSelected output ID: " + selected(output, outputs) +
            L"\r\nStart queries actual formats and periods. API/queue values are not measured end-to-end latency.";
        SetWindowTextW(details, text.c_str());
        enable();
    }
    void refresh() {
        if (active) return;
        worker.reset();
        const auto old_input = selected(input, inputs), old_output = selected(output, outputs);
        inputs.clear(); outputs.clear();
        try {
            inputs = audio::enumerate_endpoints(true);
            outputs = audio::enumerate_endpoints(false);
        } catch (const audio::WasapiError& e) {
            fill(input, inputs, old_input); fill(output, outputs, old_output); enable();
            SetWindowTextW(status, (L"Device enumeration failed: " + hex(e.code) + L". Refresh to retry.").c_str());
            return;
        }
        fill(input, inputs, old_input); fill(output, outputs, old_output);
        selection_status();
    }
    void start() {
        if (active || !supported(input, inputs) || !supported(output, outputs)) return;
        audio::Settings settings{selected(input, inputs), selected(output, outputs),
            SendMessageW(preset, CB_GETCURSEL, 0, 0) == 1 ? audio::Preset::stable : audio::Preset::low_latency};
        worker = std::make_unique<audio::Worker>([settings] { return audio::make_wasapi_session(settings); });
        worker->start();
        active = true; enable();
        SetWindowTextW(status, L"Starting: negotiating streams and waiting for prefill...");
    }
    void stop() {
        if (worker) worker->stop(); // releases streams on worker and joins before controls/window change
        active = false; enable(); update();
    }
    void update() {
        if (!worker) return;
        const auto s = worker->snapshot();
        if (active && (s.state == audio::State::failed || s.state == audio::State::stopped)) {
            worker->stop(); active = false; enable();
        }
        std::wstring message;
        if (s.state == audio::State::failed) {
            message = L"Stopped with error " + hex(s.error) + L" at " +
                std::wstring(s.stage, s.stage + std::char_traits<char>::length(s.stage)) +
                L". Refresh and reselect endpoints before restarting.";
        } else if (s.state == audio::State::starting) message = L"Starting: negotiating streams and waiting for prefill...";
        else if (s.state == audio::State::running) message = L"Running. Settings are locked until Stop.";
        else message = L"Stopped. Worker joined.";
        SetWindowTextW(status, message.c_str());
        auto text = L"Input ID: " + selected(input, inputs) + L"\nOutput ID: " + selected(output, outputs) + L"\n" + diagnostics(s) +
            L"API/queue values are diagnostics, not measured end-to-end latency.";
        // Win32 multiline edit controls expect CRLF.
        for (std::size_t pos = 0; (pos = text.find(L'\n', pos)) != std::wstring::npos; pos += 2) text.insert(pos, 1, L'\r');
        const auto first_line = SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0);
        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(details, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start), reinterpret_cast<LPARAM>(&selection_end));
        SetWindowTextW(details, text.c_str());
        SendMessageW(details, EM_SETSEL, selection_start, selection_end);
        SendMessageW(details, EM_LINESCROLL, 0, first_line - SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
    void create() {
        control(L"STATIC", L"&Input (capture)", 0, 0, 16, 16, 150, 20);
        input = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, input_id, 16, 40, 780, 240);
        control(L"STATIC", L"&Output (render)", 0, 0, 16, 76, 150, 20);
        output = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, output_id, 16, 100, 780, 240);
        control(L"STATIC", L"&Buffering preset", 0, 0, 16, 138, 160, 20);
        preset = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, preset_id, 16, 162, 270, 150);
        SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency (minimum legal period)"));
        SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Stable (default period, larger queue)"));
        SendMessageW(preset, CB_SETCURSEL, 0, 0);
        refresh_button = control(L"BUTTON", L"&Refresh", WS_TABSTOP, refresh_id, 310, 160, 110, 28);
        start_button = control(L"BUTTON", L"&Start", WS_TABSTOP | BS_DEFPUSHBUTTON, start_id, 436, 160, 110, 28);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP, stop_id, 562, 160, 110, 28);
        status = control(L"STATIC", L"", 0, 0, 16, 204, 780, 48);
        details = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            0, 16, 258, 780, 370);
        refresh();
        if (!SetTimer(hwnd, 1, testing ? 50 : 250, nullptr)) throw std::runtime_error("SetTimer");
    }
    static void require(bool value) { if (!value) throw std::runtime_error("GUI self-test"); }
    void test_tick() {
        if (++ticks > 100) throw std::runtime_error("GUI self-test timeout");
        if (test_step == 0) {
            write(L"GUI native window created; enumerated inputs=" + std::to_wstring(inputs.size()) +
                L" outputs=" + std::to_wstring(outputs.size()) + L" (no routing claim).\n");
            // Deterministic empty-state test uses the same real controls. Native
            // enumeration above is reported separately from these synthetic IDs.
            inputs.clear(); outputs.clear(); fill(input, inputs, L""); fill(output, outputs, L""); selection_status();
            wchar_t text[256]{}; GetWindowTextW(status, text, 256);
            require(std::wstring_view(text).find(L"No active input endpoints") != std::wstring_view::npos);
            require(!IsWindowEnabled(start_button) && IsWindowEnabled(refresh_button) && IsWindowEnabled(preset));
            SendMessageW(hwnd, WM_COMMAND, refresh_id, 0);
            require(!active && IsWindowEnabled(refresh_button));
            inputs = {{L"nonexistent-input-0", L"Duplicate name", {}, 0}, {L"nonexistent-input-1", L"Duplicate name", {}, 0}};
            outputs = {{L"nonexistent-output", L"Synthetic output", {}, 0}};
            fill(input, inputs, L"nonexistent-input-1"); fill(output, outputs, L"");
            std::reverse(inputs.begin(), inputs.end());
            fill(input, inputs, L"nonexistent-input-1");
            require(selected(input, inputs) == L"nonexistent-input-1");
            fill(input, inputs, L"removed-id"); require(selected(input, inputs).empty());
            fill(input, inputs, L"nonexistent-input-1"); selection_status();
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            require(active && !IsWindowEnabled(input) && !IsWindowEnabled(output) && !IsWindowEnabled(preset) &&
                !IsWindowEnabled(refresh_button) && !IsWindowEnabled(start_button) && IsWindowEnabled(stop_button));
            ++test_step;
        } else if (test_step == 1 && worker->snapshot().state == audio::State::failed) {
            update(); require(!active && IsWindowEnabled(start_button) && IsWindowEnabled(refresh_button));
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            SendMessageW(hwnd, WM_COMMAND, stop_id, 0);
            require(!active && IsWindowEnabled(input));
            SendMessageW(hwnd, WM_COMMAND, start_id, 0);
            // Exercise the actual close handler while a worker is starting.
            passed = true;
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        try {
            switch (message) {
            case WM_CREATE: self->create(); return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                case refresh_id: self->refresh(); break;
                case start_id: self->start(); break;
                case stop_id: self->stop(); break;
                case input_id: case output_id:
                    if (HIWORD(wp) == CBN_SELCHANGE && !self->active) { self->worker.reset(); self->selection_status(); }
                    break;
                }
                return 0;
            case WM_TIMER: if (self->testing) self->test_tick(); else self->update(); return 0;
            case WM_SIZE:
                if (self->details) {
                    const int width = std::max(680, static_cast<int>(LOWORD(lp)) - 32);
                    MoveWindow(self->input, 16, 40, width, 240, TRUE);
                    MoveWindow(self->output, 16, 100, width, 240, TRUE);
                    MoveWindow(self->status, 16, 204, width, 48, TRUE);
                    MoveWindow(self->details, 16, 258, width, std::max(80, static_cast<int>(HIWORD(lp)) - 274), TRUE);
                }
                return 0;
            case WM_GETMINMAXINFO:
                reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {740, 450}; return 0;
            case WM_QUERYENDSESSION: self->stop(); return TRUE;
            case WM_CLOSE: self->stop(); DestroyWindow(window); return 0;
            case WM_DESTROY:
                KillTimer(window, 1);
                if (self->worker) self->worker->stop();
                if (self->testing) {
                    if (!self->passed) self->exit_code = 1;
                    write(self->exit_code ? L"gui self-test: FAILED\n" :
                        L"gui self-test: passed (native controls, synthetic empty/ID selection, settings lock, invalid endpoint failure, restart, repeated stop, close/join; no interactive/hardware QA)\n");
                }
                PostQuitMessage(self->exit_code); return 0;
            }
        } catch (...) {
            self->exit_code = 1;
            if (self->worker) self->worker->stop();
            if (message == WM_CREATE) return -1;
            DestroyWindow(window);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
int gui(HINSTANCE instance, int show, bool testing) {
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 3;
    struct Uninit { ~Uninit() { CoUninitialize(); } } uninit;
    Window app{}; app.instance = instance; app.testing = testing;
    WNDCLASSW type{};
    type.lpfnWndProc = Window::procedure; type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    type.lpszClassName = L"AudioRepeaterSharedWindow";
    if (!RegisterClassW(&type)) return 3;
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Audio Repeater WASAPI Shared",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 720, nullptr, nullptr, instance, &app);
    if (!window) return 3;
    ShowWindow(window, show);
    MSG message{};
    int result = 0;
    while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    // A message-loop error also joins before Window storage is released.
    if (app.worker) app.worker->stop();
    if (IsWindow(window)) DestroyWindow(window);
    return result < 0 ? 3 : static_cast<int>(message.wParam);
}

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
    bool input = false, output = false, timing = false, preset = false;
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
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    console();
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { write(L"Command line parsing failed.\n"); return 2; }
    int result = 3;
    try {
        if (argc == 1) result = gui(instance, show, false);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--gui-self-test") result = gui(instance, show, true);
        else result = run(argc, argv);
    }
    catch (const audio::WasapiError& e) { write(L"Device enumeration failed HRESULT=" + hex(e.code) + L"; routing is unverified.\n"); }
    catch (...) { write(L"Device enumeration or audio initialization failed; no routing success is claimed.\n"); }
    LocalFree(argv);
    return result;
}
