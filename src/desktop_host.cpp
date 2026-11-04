#include "desktop_host.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <iterator>
#include <string_view>

namespace desktop {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t startup_description[] = L"Audio Repeater WASAPI sign-in startup";
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Windows desktop operation"); }
std::filesystem::path folder(REFKNOWNFOLDERID id) {
    PWSTR text{}; check(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &text));
    const std::filesystem::path result(text); CoTaskMemFree(text); return result;
}
ComPtr<IShellLinkW> link() {
    ComPtr<IShellLinkW> result;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result)));
    return result;
}
ComPtr<IShellLinkW> read_link(const Paths& paths) {
    auto result = link(); ComPtr<IPersistFile> file;
    check(result.As(&file)); check(file->Load(paths.startup.c_str(), STGM_READ)); return result;
}
bool owned(IShellLinkW* value) {
    wchar_t description[128]{}, arguments[128]{};
    return SUCCEEDED(value->GetDescription(description, 128)) &&
        SUCCEEDED(value->GetArguments(arguments, 128)) &&
        std::wstring_view(description) == startup_description && std::wstring_view(arguments) == L"--background";
}
void save_link(const Paths& paths, IShellLinkW* value) {
    ComPtr<IPersistFile> file; check(value->QueryInterface(IID_PPV_ARGS(&file)));
    check(file->Save(paths.startup.c_str(), TRUE));
}
void erase(const std::filesystem::path& path) {
    std::error_code error; std::filesystem::remove(path, error);
    if (error) throw std::system_error(error);
}
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Endpoint encoding");
    std::string result(static_cast<std::size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), n, nullptr, nullptr))
        throw std::runtime_error("Endpoint encoding");
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (!n) throw std::runtime_error("Endpoint encoding");
    std::wstring result(static_cast<std::size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), n))
        throw std::runtime_error("Endpoint encoding");
    return result;
}
Paths user_paths() {
    std::wstring executable(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size >= executable.size()) throw std::runtime_error("Executable path");
    executable.resize(size);
    return {folder(FOLDERID_LocalAppData) / L"AudioRepeaterWasapi" / L"settings.conf",
        folder(FOLDERID_Startup) / L"AudioRepeaterWasapi.lnk", executable};
}
bool load_preferences(const Paths& paths, Preferences& value, bool& exists) {
    exists = std::filesystem::exists(paths.settings);
    if (!exists) return true;
    if (std::filesystem::file_size(paths.settings) > 17000) return false;
    std::ifstream input(paths.settings, std::ios::binary);
    if (!input) return false;
    std::string text(17001, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return !input.bad() && text.size() <= 17000 && parse(text, value);
}
void save_preferences(const Paths& paths, const Preferences& value) {
    const auto text = serialize(value);
    std::filesystem::create_directories(paths.settings.parent_path());
    auto temporary = paths.settings; temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())) || !output.flush())
            throw std::runtime_error("Save preferences");
    }
    if (!MoveFileExW(temporary.c_str(), paths.settings.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError(); DeleteFileW(temporary.c_str());
        throw std::system_error(static_cast<int>(error), std::system_category());
    }
}
bool startup_enabled(const Paths& paths) {
    if (!std::filesystem::exists(paths.startup)) return false;
    const auto value = read_link(paths);
    if (!owned(value.Get())) return false;
    wchar_t target[32768]{};
    check(value->GetPath(target, 32768, nullptr, SLGP_RAWPATH));
    return _wcsicmp(target, paths.executable.c_str()) == 0;
}
void set_startup(const Paths& paths, bool enabled) {
    if (std::filesystem::exists(paths.startup)) {
        if (!owned(read_link(paths).Get())) throw std::runtime_error("Startup shortcut belongs to another application");
        if (!enabled) { erase(paths.startup); return; }
    }
    if (!enabled) return;
    auto value = link();
    check(value->SetPath(paths.executable.c_str())); check(value->SetArguments(L"--background"));
    check(value->SetDescription(startup_description));
    check(value->SetWorkingDirectory(paths.executable.parent_path().c_str()));
    std::filesystem::create_directories(paths.startup.parent_path());
    save_link(paths, value.Get());
}
void remove_preferences(const Paths& paths) {
    set_startup(paths, false); // a foreign shortcut aborts cleanup, without deleting it
    erase(paths.settings);
    auto temporary = paths.settings; temporary += L".tmp"; erase(temporary);
    // Remove only an empty application directory; never recurse through user files.
    std::error_code error; std::filesystem::remove(paths.settings.parent_path(), error);
    if (error && error != std::errc::directory_not_empty && error != std::errc::no_such_file_or_directory)
        throw std::system_error(error);
}
Instance::Instance(std::wstring_view test_identity) {
    std::wstring name = L"Local\\AudioRepeaterWasapi-";
    if (!test_identity.empty()) name += L"test-" + std::wstring(test_identity);
    else {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("User identity");
        DWORD bytes{}; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        const BOOL got = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes); CloseHandle(token);
        if (!got) throw std::runtime_error("User identity");
        LPWSTR sid{};
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
            throw std::runtime_error("User identity");
        name += sid; LocalFree(sid);
    }
    handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!handle_) throw std::runtime_error("Single instance");
    existing_ = GetLastError() == ERROR_ALREADY_EXISTS;
}
Instance::~Instance() { if (handle_) CloseHandle(handle_); }
struct Notifications::Impl final : IMMNotificationClient {
    std::atomic<ULONG> refs{1};
    std::atomic<HWND> target{};
    std::atomic<bool> pending{};
    ComPtr<IMMDeviceEnumerator> enumerator;
    bool registered{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs; if (!count) delete this; return count; }
    HRESULT notify() noexcept {
        const auto window = target.load();
        if (window && !pending.exchange(true) && !PostMessageW(window, devices_message, 0, 0)) pending = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return notify(); }
};
bool forward_existing(const wchar_t* class_name, bool background) {
    if (background) return true;
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        if (const auto existing = FindWindowW(class_name, nullptr))
            return PostMessageW(existing, RegisterWindowMessageW(L"AudioRepeaterWasapi.ShowSettings"), 0, 0) != FALSE;
        Sleep(50);
    }
    return false;
}
int instance_test_child(const wchar_t* identity, const wchar_t* class_name, bool background) {
    // This finite helper never creates a window or an audio worker. Its namespace
    // cannot match the production mutex/window, even with arbitrary arguments.
    const std::wstring_view id(identity), name(class_name);
    if (id.size() != 38 || id.front() != L'{' || id.back() != L'}' ||
        !name.starts_with(L"AudioRepeaterWasapiTestWindow-")) return 2;
    Instance instance(id);
    return instance.existing() && forward_existing(class_name, background) ? 0 : 1;
}
Notifications::Notifications() : impl_(new Impl) {}
Notifications::~Notifications() {
    detach();
    // RegisterEndpointNotificationCallback does not own a reference. Keep our
    // initial reference until after Unregister; callbacks never release it.
    auto* value = impl_.release(); value->Release();
}
bool Notifications::attach(HWND window) {
    detach();
    impl_->target = window;
    const auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl_->enumerator));
    if (FAILED(hr)) return false;
    impl_->registered = SUCCEEDED(impl_->enumerator->RegisterEndpointNotificationCallback(impl_.get()));
    return impl_->registered;
}
void Notifications::detach() noexcept {
    impl_->target = nullptr;
    if (impl_->registered) { impl_->enumerator->UnregisterEndpointNotificationCallback(impl_.get()); impl_->registered = false; }
    impl_->enumerator.Reset();
}
void Notifications::acknowledge() noexcept { impl_->pending = false; }
bool Tray::add(HWND window) {
    if (present_) remove();
    data_ = {}; data_.cbSize = sizeof(data_); data_.hWnd = window; data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = tray_message; data_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data_.szTip, L"Audio Repeater WASAPI");
    present_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (present_) { data_.uVersion = NOTIFYICON_VERSION_4; if (!Shell_NotifyIconW(NIM_SETVERSION, &data_)) remove(); }
    return present_;
}
void Tray::remove() noexcept { if (present_) Shell_NotifyIconW(NIM_DELETE, &data_); present_ = false; }
void Tray::tooltip(const wchar_t* value) noexcept {
    if (!present_ || std::wstring_view(data_.szTip) == value) return;
    wcsncpy_s(data_.szTip, value, _TRUNCATE); data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}
bool host_self_test() {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) return false;
    wchar_t identity[40]{}; if (!StringFromGUID2(id, identity, 40)) return false;
    const auto directory = std::filesystem::temp_directory_path() / (std::wstring(L"AudioRepeaterWasapi-test-") + identity);
    Paths paths{directory / L"config" / L"settings.conf", directory / L"startup" / L"AudioRepeaterWasapi.lnk",
        directory / L"old folder" / L"audio-repeater.exe"};
    auto require = [](bool pass) { if (!pass) throw std::runtime_error("Desktop host self-test"); };
    try {
        std::filesystem::create_directories(directory);
        Preferences wanted{"capture-\xe6\x97\xa5", "render-\xf0\x9f\x8e\xa7", audio::Preset::stable, audio::ShareMode::exclusive, true}, actual;
        bool exists{}; require(load_preferences(paths, actual, exists) && !exists);
        save_preferences(paths, wanted); require(load_preferences(paths, actual, exists) && exists && actual == wanted);
        set_startup(paths, true); require(startup_enabled(paths));
        paths.executable = directory / L"new folder" / L"audio-repeater.exe";
        require(!startup_enabled(paths)); set_startup(paths, true); require(startup_enabled(paths));
        set_startup(paths, false); require(!std::filesystem::exists(paths.startup));
        auto foreign = link(); check(foreign->SetPath(paths.executable.c_str())); check(foreign->SetDescription(L"Another application"));
        save_link(paths, foreign.Get());
        bool refused{}; try { set_startup(paths, true); } catch (...) { refused = true; } require(refused);
        refused = false; try { remove_preferences(paths); } catch (...) { refused = true; }
        require(refused && std::filesystem::exists(paths.startup) && std::filesystem::exists(paths.settings));
        erase(paths.startup);
        std::ofstream(paths.settings.parent_path() / L"keep.txt") << "unrelated";
        remove_preferences(paths); require(!std::filesystem::exists(paths.settings) &&
            std::filesystem::exists(paths.settings.parent_path() / L"keep.txt"));
        std::filesystem::remove_all(directory);
        Instance first(identity), second(identity); require(!first.existing() && second.existing());
        return true;
    } catch (...) { std::error_code error; std::filesystem::remove_all(directory, error); return false; }
}
}
