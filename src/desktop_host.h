#pragma once
#include "app_state.h"
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <memory>
#include <string_view>

namespace desktop {
inline constexpr UINT tray_message = WM_APP + 1, devices_message = WM_APP + 2;
inline constexpr wchar_t window_class[] = L"AudioRepeaterWasapiWindow";
std::string utf8(const std::wstring&);
std::wstring wide(const std::string&);
struct Paths {
    std::filesystem::path settings, startup, executable;
};
Paths user_paths();
bool load_preferences(const Paths&, Preferences&, bool& exists);
void save_preferences(const Paths&, const Preferences&);
bool startup_enabled(const Paths&);
void set_startup(const Paths&, bool);
void remove_preferences(const Paths&);
bool host_self_test();
class Instance {
public:
    explicit Instance(std::wstring_view test_identity = {});
    ~Instance();
    bool existing() const noexcept { return existing_; }
private:
    HANDLE handle_{};
    bool existing_{};
};
bool forward_existing(const wchar_t* class_name, bool background);
int instance_test_child(const wchar_t* identity, const wchar_t* class_name, bool background);
class Notifications {
public:
    Notifications();
    ~Notifications();
    bool attach(HWND);
    void detach() noexcept;
    void acknowledge() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Tray {
public:
    bool add(HWND);
    void remove() noexcept;
    bool present() const noexcept { return present_; }
    void tooltip(const wchar_t*) noexcept;
    ~Tray() { remove(); }
private:
    NOTIFYICONDATAW data_{};
    bool present_{};
};
// Functions shared by the finite CLI and the desktop window.
void write(const std::wstring&);
std::wstring hex(std::int32_t);
std::wstring diagnostics(const audio::Snapshot&);
int gui(HINSTANCE, int, bool testing, bool background);
}
