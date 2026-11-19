#include "audio/WasapiLoopback.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// SDK Windows 10.0.20348+ ; redéclaré pour les toolchains qui ne l'ont pas (MinGW).
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#include <algorithm>
#include <cwctype>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

// Gestionnaire de fin d'activation asynchrone (COM minimal, agile : appelé sur un thread MTA).
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() { CloseHandle(done_); }
    HANDLE DoneEvent() const noexcept { return done_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    HANDLE done_;
};

using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                    IActivateAudioInterfaceAsyncOperation**);

// Active un IAudioClient sur l'arbre de processus `pid` (Windows 11 / Server 2022+).
HRESULT ActivateProcessLoopback(DWORD pid, ComPtr<IAudioClient>& client) {
    static const auto activate = reinterpret_cast<ActivateFn>(reinterpret_cast<void*>(
        GetProcAddress(LoadLibraryW(L"Mmdevapi.dll"), "ActivateAudioInterfaceAsync")));
    if (!activate) return E_NOTIMPL;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(handler->DoneEvent(), 5000);
        HRESULT activateHr = E_FAIL;
        ComPtr<IUnknown> unk;
        hr = op->GetActivateResult(&activateHr, &unk);
        if (SUCCEEDED(hr)) hr = activateHr;
        if (SUCCEEDED(hr)) hr = unk.As(&client);
    }
    handler->Release();
    return hr;
}

HRESULT ActivateSystemLoopback(ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
    return hr;
}

// Entrées audio actives : (identifiant, nom).
std::vector<std::pair<std::wstring, std::wstring>> Inputs() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)))
        return out;
    UINT n = 0;
    devices->GetCount(&n);
    static constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        ComPtr<IPropertyStore> props;
        LPWSTR id = nullptr;
        if (FAILED(devices->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
        std::wstring name;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = v.pwszVal;
        PropVariantClear(&v);
        out.emplace_back(id, name);
        CoTaskMemFree(id);
    }
    return out;
}

HRESULT ActivateInput(const std::wstring& id, ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(id.c_str(), &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
    return hr;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

}  // namespace

std::vector<std::wstring> WasapiLoopback::InputNames() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::wstring> names;
    for (const auto& [id, name] : Inputs()) names.push_back(name);
    if (SUCCEEDED(com)) CoUninitialize();
    return names;
}

bool WasapiLoopback::StartInput(const std::wstring& nameContains, Sink sink) {
    Stop();
    if (nameContains.empty()) return false;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring id;
    for (const auto& [devId, name] : Inputs())
        if (Lower(name).find(Lower(nameContains)) != std::wstring::npos) {
            id = devId;
            log::Info("Loopback: entrée « {} »", log::Narrow(name));
            break;
        }
    if (SUCCEEDED(com)) CoUninitialize();
    if (id.empty()) return false;
    processMode_ = false;
    inputId_ = id;
    running_ = true;
    thread_ = std::jthread([this, id, sink = std::move(sink)](std::stop_token st) { Run(st, 0, id, sink); });
    return true;
}

std::uint32_t WasapiLoopback::FindProcessId(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    // Applis multi-processus (Discord, navigateurs) : on prend la RACINE de l'arbre (son parent n'est pas un
    // processus du même nom), la capture « arbre de processus » inclut alors tous ses enfants, dont celui qui joue le son.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;  // pid, parent
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) found.emplace_back(pe.th32ProcessID, pe.th32ParentProcessID);
    CloseHandle(snap);
    for (const auto& [pid, parent] : found)
        if (std::ranges::none_of(found, [p = parent](const auto& f) { return f.first == p; })) return pid;
    return found.empty() ? 0 : found.front().first;
}

bool WasapiLoopback::Start(const std::wstring& processName, Sink sink) {
    Stop();
    std::uint32_t pid = 0;
    if (!processName.empty()) {
        pid = FindProcessId(processName);
        if (pid == 0) return false;
    }
    processMode_ = pid != 0;
    inputId_.clear();
    running_ = true;
    thread_ = std::jthread([this, pid, sink = std::move(sink)](std::stop_token st) { Run(st, pid, {}, sink); });
    return true;
}

void WasapiLoopback::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    running_ = false;
}

void WasapiLoopback::Run(std::stop_token st, std::uint32_t pid, std::wstring inputId, Sink sink) {
    log::SetCurrentThreadName(L"Companion.Loopback");
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IAudioClient> client;
    const bool input = !inputId.empty();
    HRESULT hr = input ? ActivateInput(inputId, client) : pid ? ActivateProcessLoopback(pid, client) : ActivateSystemLoopback(client);

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = kChannels * 4;
    fmt.nAvgBytesPerSec = kSampleRate * fmt.nBlockAlign;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                (input ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK) | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                2'000'000 /* 200 ms */, 0, &fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();

    if (FAILED(hr)) {
        log::Info("Loopback: échec d'initialisation ({}, hr=0x{:08X})", input ? "entrée" : pid ? "processus" : "système",
                  static_cast<unsigned>(hr));
    } else {
        log::Info("Loopback: capture {} démarrée", input ? "de l'entrée" : pid ? "du processus" : "du mix système");
        std::vector<float> silence;
        while (!st.stop_requested()) {
            // Sans son, aucun événement n'arrive : le délai permet de rester réactif à l'arrêt.
            WaitForSingleObject(event, 200);
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(static_cast<size_t>(frames) * kChannels, 0.f);
                    sink(silence.data(), frames);
                } else {
                    sink(reinterpret_cast<const float*>(data), frames);
                }
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
    }
    CloseHandle(event);
    if (SUCCEEDED(comHr)) CoUninitialize();
    running_ = false;
}

}  // namespace companion
