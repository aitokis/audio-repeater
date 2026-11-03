#include "wasapi_engine.h"
#include <audioclient.h>
#include <avrt.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace audio {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result) { if (FAILED(result)) throw WasapiError{result}; }
struct TaskFree { void operator()(WAVEFORMATEX* p) const noexcept { CoTaskMemFree(p); } };
using Mix = std::unique_ptr<WAVEFORMATEX, TaskFree>;
struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};
struct Property {
    PROPVARIANT value{};
    ~Property() { PropVariantClear(&value); }
};
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> result;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&result)));
    return result;
}
Mix mix_format(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
    WAVEFORMATEX* format = nullptr;
    check(client->GetMixFormat(&format));
    return Mix(format);
}
struct Stream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioClient3> client3;
    Mix mix;
    WAVEFORMATEXTENSIBLE exclusive_wave{};
    Handle event;
    StreamInfo info;
    bool started{};
    HRESULT activate() noexcept {
        return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    }
    HRESULT periods(Periods& p) noexcept {
        const auto result = client.As(&client3);
        return FAILED(result) ? result : client3->GetSharedModeEnginePeriod(
            mix.get(), &p.normal, &p.fundamental, &p.minimum, &p.maximum);
    }
    HRESULT initialize3(std::uint32_t frames) noexcept {
        return client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, frames, mix.get(), nullptr);
    }
    void release() noexcept { client3.Reset(); client.Reset(); }
    HRESULT initialize_legacy() noexcept {
        return client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  0, 0, mix.get(), nullptr);
    }
    HRESULT supports_format(const Format& format, bool extensible) noexcept {
        exclusive_wave = exclusive_wave_format(format, extensible);
        return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &exclusive_wave.Format, nullptr);
    }
    HRESULT device_periods(std::int64_t& normal, std::int64_t& minimum) noexcept {
        return client->GetDevicePeriod(&normal, &minimum);
    }
    HRESULT initialize_exclusive(std::int64_t duration) noexcept {
        return client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  duration, duration, &exclusive_wave.Format, nullptr);
    }
    HRESULT buffer_size(std::uint32_t& frames) noexcept { return client->GetBufferSize(&frames); }
    HRESULT padding(std::uint32_t& frames) noexcept { return client->GetCurrentPadding(&frames); }
    void open(IMMDeviceEnumerator* devices, const std::wstring& id, EDataFlow expected, Preset preset, ShareMode mode) {
        info.mode = mode;
        check(devices->GetDevice(id.c_str(), &device));
        ComPtr<IMMEndpoint> endpoint;
        check(device.As(&endpoint));
        EDataFlow flow{}; check(endpoint->GetDataFlow(&flow));
        if (flow != expected) throw WasapiError{E_INVALIDARG};
        if (mode == ShareMode::exclusive) {
            check(audio::initialize_exclusive(*this, expected == eCapture, preset, info));
        } else {
            mix = mix_format(device.Get()); info.format = native_format(*mix);
            check(initialize_shared(*this, preset, info));
        }
        event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event.value) throw WasapiError{HRESULT_FROM_WIN32(GetLastError())};
        check(client->SetEventHandle(event.value));
        check(client->GetBufferSize(&info.buffer_frames));
        check(client->GetDevicePeriod(&info.default_period_100ns, &info.minimum_period_100ns));
        check(client->GetStreamLatency(&info.latency_100ns));
        info.initialized = true;
        // GetDevicePeriod is a default, not evidence of the currently running period.
        if (mode == ShareMode::exclusive) info.actual_period_frames = info.buffer_frames;
        else if (!client3) (void)client.As(&client3);
        if (mode == ShareMode::shared && client3) {
            WAVEFORMATEX* current = nullptr;
            UINT32 frames = 0;
            const auto result = client3->GetCurrentSharedModeEnginePeriod(&current, &frames);
            Mix current_owner(current);
            if (SUCCEEDED(result) && current && current->nSamplesPerSec == mix->nSamplesPerSec)
                info.actual_period_frames = frames;
        }
        if (!info.buffer_frames || info.buffer_frames > info.format.rate / 2 ||
            info.default_period_100ns <= 0 || info.default_period_100ns > 5'000'000)
            throw WasapiError{E_INVALIDARG};
    }
    void stop() noexcept { if (started && client) { (void)client->Stop(); started = false; } }
};

class WasapiSession final : public Session {
public:
    explicit WasapiSession(Settings settings) : settings_(std::move(settings)) {}
    ~WasapiSession() override {
        stop(); callback_.reset(); render_.Reset(); capture_.Reset();
        output_.release(); input_.release();
        output_.device.Reset(); input_.device.Reset();
        if (com_) CoUninitialize();
    }
    std::int32_t initialize(std::stop_token token, SetupInfo& setup) override {
        input_.info.mode = output_.info.mode = settings_.mode;
        setup.input = input_.info; setup.output = output_.info;
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return hr;
        com_ = true;
        try {
            stop_event_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop_event_.value) return HRESULT_FROM_WIN32(GetLastError());
            callback_.emplace(token, Signal{stop_event_.value});
            if (token.stop_requested()) return S_OK;
            auto devices = enumerator();
            input_.open(devices.Get(), settings_.input_id, eCapture, settings_.preset, settings_.mode);
            setup.input = input_.info;
            if (token.stop_requested()) return S_OK;
            output_.open(devices.Get(), settings_.output_id, eRender, settings_.preset, settings_.mode);
            setup.output = output_.info;
            if (token.stop_requested()) return S_OK;
            check(input_.client->GetService(IID_PPV_ARGS(&capture_)));
            check(output_.client->GetService(IID_PPV_ARGS(&render_)));
            const auto in_rate = input_.info.format.rate;
            const auto out_rate = output_.info.format.rate;
            const auto channels = output_.info.format.channels;
            const double ratio = static_cast<double>(out_rate) / in_rate;
            const auto lookahead = static_cast<std::size_t>(std::ceil(64 / std::min(ratio / 1.002, 1.0))) + 64;
            const auto demand = static_cast<std::size_t>(std::ceil(output_.info.buffer_frames / (ratio / 1.002)));
            const auto period_seconds = [](const StreamInfo& s) {
                return s.actual_period_frames ? static_cast<double>(s.actual_period_frames) / s.format.rate
                                              : static_cast<double>(s.default_period_100ns) / 1e7;
            };
            const double period = std::max(period_seconds(input_.info), period_seconds(output_.info));
            // The core requires room for one maximum render request plus filter
            // lookahead; this safety floor can exceed the period-based starting target.
            target_ = select_queue_target(in_rate, demand + lookahead + 1, period, settings_.preset);
            const auto capacity = 4 * target_ + input_.info.buffer_frames + lookahead;
            if (target_ > in_rate / 2 || capacity > in_rate * 2 || capacity > 1'000'000)
                return E_INVALIDARG;
            transport_ = std::make_unique<Transport>(in_rate, out_rate, channels, capacity,
                                                    target_, output_.info.buffer_frames);
            capture_scratch_.resize(static_cast<std::size_t>(input_.info.buffer_frames) * channels);
            render_scratch_.resize(static_cast<std::size_t>(output_.info.buffer_frames) * channels);
            setup.target_frames = target_; setup.capacity_frames = capacity;
            DWORD index = 0;
            mmcss_ = AvSetMmThreadCharacteristicsW(L"Audio", &index);
            setup.mmcss = mmcss_ != nullptr;
            setup.mmcss_error = mmcss_ ? 0 : GetLastError();
            return S_OK;
        } catch (const WasapiError& e) {
            setup.input = input_.info; setup.output = output_.info; return e.code;
        }
    }
    std::int32_t start_capture() noexcept override {
        const auto hr = input_.client->Start(); input_.started = SUCCEEDED(hr); return hr;
    }
    Wake wait() noexcept override {
        const HANDLE handles[] = {stop_event_.value, input_.event.value, output_.event.value};
        const auto result = WaitForMultipleObjects(3, handles, FALSE, 100);
        if (result == WAIT_OBJECT_0) return Wake::stopped;
        if (result == WAIT_OBJECT_0 + 1) return Wake::capture;
        if (result == WAIT_OBJECT_0 + 2) return Wake::render;
        return result == WAIT_TIMEOUT ? Wake::idle : Wake::failed;
    }
    std::int32_t capture(std::stop_token token) noexcept override {
        // Finite drain, even when a driver continually supplies packets. Every
        // acquired packet is released on this same thread before cancellation.
        for (unsigned packet = 0; packet < (settings_.mode == ShareMode::exclusive ? 1U : 8U) && !token.stop_requested(); ++packet) {
            HRESULT hr = S_OK;
            if (settings_.mode == ShareMode::shared) {
                UINT32 next = 0;
                hr = capture_->GetNextPacketSize(&next);
                if (FAILED(hr) || !next) return hr;
            }
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            hr = capture_pass_.run(settings_.mode, input_.started,
                [&] { return capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr); }, [&]() -> std::int32_t {
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) transport_->discontinuity();
                    if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestamps_;
                    bool decoded = false;
                    if (frames <= input_.info.buffer_frames && (silent || data)) {
                        auto floats = std::span<float>(capture_scratch_.data(), static_cast<std::size_t>(frames) * output_.info.format.channels);
                        const auto bytes = silent ? std::span<const std::byte>{} : std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(data), static_cast<std::size_t>(frames) * input_.info.format.block_align);
                        decoded = decode(input_.info.format, bytes, floats, output_.info.format.channels, silent);
                        if (decoded) { (void)transport_->push(floats); if (silent) silent_capture_ += frames; }
                    }
                    const auto release = capture_->ReleaseBuffer(frames);
                    if (FAILED(release)) return release;
                    return decoded ? S_OK : E_INVALIDARG;
                });
            if (FAILED(hr)) return hr;
            if (hr != S_OK) return S_OK; // empty/deferred: wait for another event
        }
        return S_OK;
    }
    bool ready() const noexcept override { return transport_->queued_input() >= static_cast<double>(target_); }
    std::int32_t start_render() noexcept override {
        // Discard notifications from initialization/prefill waiting; subsequent
        // buffer requests must follow a fresh event from the running stream.
        if (!ResetEvent(output_.event.value)) return HRESULT_FROM_WIN32(GetLastError());
        auto hr = render(); // prefill the endpoint before starting its clock
        if (hr != S_OK) return FAILED(hr) ? hr : E_UNEXPECTED;
        hr = output_.client->Start(); output_.started = SUCCEEDED(hr); return hr;
    }
    std::int32_t render() noexcept override {
        UINT32 frames = 0;
        auto hr = render_request(output_, settings_.mode, output_.info.buffer_frames, frames);
        if (FAILED(hr)) return hr;
        if (!frames) return S_OK;
        BYTE* data = nullptr;
        return render_pass_.run(settings_.mode, output_.started,
            [&] { return render_->GetBuffer(frames, &data); }, [&]() -> std::int32_t {
                auto floats = std::span<float>(render_scratch_.data(), static_cast<std::size_t>(frames) * output_.info.format.channels);
                (void)transport_->render(floats);
                const bool silent = std::all_of(floats.begin(), floats.end(), [](float f) { return f == 0; });
                bool encoded = true;
                if (!silent) encoded = encode(output_.info.format, floats, output_.info.format.channels,
                    {reinterpret_cast<std::byte*>(data), static_cast<std::size_t>(frames) * output_.info.format.block_align});
                const auto release = render_->ReleaseBuffer(frames, silent || !encoded ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
                if (FAILED(release)) return release;
                return encoded ? S_OK : E_INVALIDARG;
            });
    }
    Metrics metrics() const noexcept override {
        return transport_ ? Metrics{transport_->statistics(), timestamps_, silent_capture_,
                                    transport_->queued_input(), transport_->correction_ppm()} : Metrics{};
    }
    void stop() noexcept override {
        output_.stop(); input_.stop();
        if (mmcss_) { AvRevertMmThreadCharacteristics(mmcss_); mmcss_ = nullptr; }
    }
private:
    struct Signal { HANDLE event; void operator()() const noexcept { SetEvent(event); } };
    Settings settings_;
    bool com_{};
    Stream input_, output_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioRenderClient> render_;
    Handle stop_event_;
    std::optional<std::stop_callback<Signal>> callback_;
    HANDLE mmcss_{};
    std::unique_ptr<Transport> transport_;
    std::vector<float> capture_scratch_, render_scratch_;
    std::size_t target_{};
    std::uint64_t timestamps_{}, silent_capture_{};
    BufferPass capture_pass_, render_pass_;
};
}
WAVEFORMATEXTENSIBLE exclusive_wave_format(const Format& format, bool extensible) noexcept {
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format.wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
    wave.Format.nChannels = static_cast<WORD>(format.channels);
    wave.Format.nSamplesPerSec = format.rate;
    wave.Format.wBitsPerSample = static_cast<WORD>(format.bytes_per_sample() * 8);
    wave.Format.nBlockAlign = static_cast<WORD>(format.block_align);
    wave.Format.nAvgBytesPerSec = format.rate * format.block_align;
    if (extensible) {
        wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wave.Samples.wValidBitsPerSample = static_cast<WORD>(format.valid_bits);
        wave.dwChannelMask = format.channel_mask;
        wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    return wave;
}
Format native_format(const WAVEFORMATEX& wave) {
    bool is_float = wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm = wave.wFormatTag == WAVE_FORMAT_PCM;
    unsigned valid_bits = wave.wBitsPerSample;
    std::uint32_t mask = 0;
    if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave.cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) throw WasapiError{E_INVALIDARG};
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        is_float = IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        is_pcm = IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
        valid_bits = ext.Samples.wValidBitsPerSample; mask = ext.dwChannelMask;
    } else if (wave.cbSize != 0) throw WasapiError{E_INVALIDARG};
    if ((!is_float && !is_pcm) || (is_float && wave.wBitsPerSample != 32)) throw WasapiError{E_INVALIDARG};
    Encoding encoding = Encoding::float32;
    if (is_pcm) {
        switch (wave.wBitsPerSample) {
        case 16: encoding = Encoding::pcm16; break;
        case 24: encoding = Encoding::pcm24; break;
        case 32: encoding = Encoding::pcm32; break;
        default: throw WasapiError{E_INVALIDARG};
        }
    }
    Format format{encoding, wave.nSamplesPerSec, wave.nChannels, valid_bits, wave.nBlockAlign, mask};
    if (!format.valid() || wave.nAvgBytesPerSec != std::uint64_t(wave.nSamplesPerSec) * wave.nBlockAlign)
        throw WasapiError{E_INVALIDARG};
    return format;
}
std::vector<Endpoint> enumerate_endpoints(bool capture) {
    auto devices = enumerator();
    ComPtr<IMMDeviceCollection> collection;
    check(devices->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection));
    UINT count = 0; check(collection->GetCount(&count));
    std::vector<Endpoint> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device; check(collection->Item(i, &device));
        LPWSTR id = nullptr; check(device->GetId(&id));
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(id, CoTaskMemFree);
        Endpoint endpoint; endpoint.id = id;
        ComPtr<IPropertyStore> properties;
        check(device->OpenPropertyStore(STGM_READ, &properties));
        Property name; check(properties->GetValue(PKEY_Device_FriendlyName, &name.value));
        endpoint.name = name.value.vt == VT_LPWSTR ? name.value.pwszVal : L"(unnamed endpoint)";
        try { auto mix = mix_format(device.Get()); endpoint.format = native_format(*mix); }
        catch (const WasapiError& e) { endpoint.format_error = e.code; }
        result.push_back(std::move(endpoint));
    }
    return result;
}
std::unique_ptr<Session> make_wasapi_session(Settings settings) {
    return std::make_unique<WasapiSession>(std::move(settings));
}
}
