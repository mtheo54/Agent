#include "audio/DiscordBridge.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr UINT32 kRate = 48000;

struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring FriendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

// Id du premier périphérique actif dont le nom contient `part` (sans la casse) ; vide si aucun.
std::wstring FindEndpoint(EDataFlow flow, const std::wstring& part) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> all;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &all)))
        return {};
    UINT n = 0;
    all->GetCount(&n);
    const std::wstring want = Lower(part);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(all->Item(i, &d))) continue;
        if (Lower(FriendlyName(d.Get())).find(want) == std::wstring::npos) continue;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        std::wstring out = id;
        CoTaskMemFree(id);
        return out;
    }
    return {};
}

bool OpenClient(IMMDevice* dev, bool eventDriven, HANDLE event, ComPtr<IAudioClient>& client) {
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()))))
        return false;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kRate * 4;
    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (eventDriven) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /*20 ms*/, 0, &fmt, nullptr))) return false;
    return !eventDriven || SUCCEEDED(client->SetEventHandle(event));
}

}  // namespace

void DiscordBridge::Start(DiscordBridgeConfig cfg, CallSpeech onSpeech) {
    Stop();
    cfg_ = std::move(cfg);
    onSpeech_ = std::move(onSpeech);
    gain_ = cfg_.voiceGain;
    if (!cfg_.enabled) return;
    supervisor_ = std::jthread([this](std::stop_token st) { SuperviseMain(st); });
}

void DiscordBridge::Stop() {
    Leave();
    supervisor_ = {};
    relay_ = {};
    relaying_ = false;
}

bool DiscordBridge::Join() {
    std::scoped_lock lock(mutex_);
    if (!relaying_.load()) return false;
    if (inCall_.load()) return true;
    voiceR_.store(voiceW_.load());  // rien de vieux dans le tampon
    // Écoute du call : le son de Discord seulement (process loopback), jamais le reste du PC.
    const bool listening = discordAudio_.Start(cfg_.discordExe, [this](const float* x, std::size_t n) { OnDiscordAudio(x, n); });
    if (!listening) log::Info("Discord: écoute du call impossible (capture du processus refusée)");
    inCall_ = true;
    log::Info("Discord: Omnis rejoint le call{}", listening ? "" : " (sans entendre les autres)");
    return true;
}

void DiscordBridge::Leave() {
    std::scoped_lock lock(mutex_);
    if (!inCall_.exchange(false)) return;
    discordAudio_.Stop();
    log::Info("Discord: Omnis quitte le call");
}

// ---------------------------------------------------------------------------
void DiscordBridge::SuperviseMain(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.DiscordWatch");
    Com com;
    std::wstring cableId;
    ULONGLONG lastStart = 0;
    while (!st.stop_requested()) {
        const bool discord = WasapiLoopback::FindProcessId(cfg_.discordExe) != 0;
        const std::wstring id = FindEndpoint(eRender, cfg_.cableDevice);
        discordRunning_ = discord;
        cableFound_ = !id.empty();
        const bool want = discord && !id.empty();
        // Relais tombé (micro mal réglé, câble débranché) : nouvel essai toutes les 30 s, pas en boucle.
        if (want && (id != cableId || (!relaying_.load() && GetTickCount64() - lastStart > 30000))) {
            lastStart = GetTickCount64();
            relay_ = {};
            cableId = id;
            relay_ = std::jthread([this, id](std::stop_token s) { RelayMain(s, id); });
        } else if (!want && !cableId.empty()) {
            Leave();
            relay_ = {};
            relaying_ = false;
            cableId.clear();
        }
        if (inCall_.load() && !discord) Leave();
        for (int i = 0; i < 30 && !st.stop_requested(); ++i) Sleep(100);
    }
    Leave();
    relay_ = {};
}

// Relais : ton micro (+ sa voix quand il est dans le call) -> câble virtuel, cadencé par le câble (10 ms).
void DiscordBridge::RelayMain(std::stop_token st, std::wstring cableId) {
    log::SetCurrentThreadName(L"Companion.DiscordRelay");
    Com com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> cable, mic;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDevice(cableId.c_str(), &cable)))
        return;
    if (!cfg_.micDevice.empty()) {
        const std::wstring micId = FindEndpoint(eCapture, cfg_.micDevice);
        if (!micId.empty()) en->GetDevice(micId.c_str(), &mic);
    }
    if (!mic && FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &mic))) return;
    // Micro par défaut = le câble lui-même : on renverrait le câble dans le câble (larsen numérique).
    if (Lower(FriendlyName(mic.Get())).find(L"cable") != std::wstring::npos) {
        std::scoped_lock lock(mutex_);
        problem_ = "ton micro par défaut de Windows est le câble : remets ton vrai micro par défaut (ou règle discordMicDevice)";
        log::Info("Discord: micro par défaut = câble, relais refusé");
        return;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioClient> out, in;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;
    UINT32 bufferFrames = 0;
    if (!OpenClient(cable.Get(), true, event, out) || !OpenClient(mic.Get(), false, nullptr, in) ||
        FAILED(out->GetBufferSize(&bufferFrames)) || FAILED(out->GetService(IID_PPV_ARGS(&render))) ||
        FAILED(in->GetService(IID_PPV_ARGS(&capture))) || FAILED(in->Start()) || FAILED(out->Start())) {
        {
            std::scoped_lock lock(mutex_);
            problem_ = "ouverture du câble ou du micro impossible";
        }
        log::Info("Discord: ouverture du câble ou du micro impossible");
        CloseHandle(event);
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        problem_.clear();
    }
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    relaying_ = true;
    log::Info("Discord: relais du micro vers le câble actif ({} → {})", log::Narrow(FriendlyName(mic.Get())),
              log::Narrow(FriendlyName(cable.Get())));

    std::vector<float> micRing(1u << 15);  // 680 ms
    std::size_t mr = 0, mw = 0;
    const std::size_t mask = micRing.size() - 1;
    while (!st.stop_requested()) {
        WaitForSingleObject(event, 50);
        // 1) Micro -> tampon
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const auto* f = reinterpret_cast<const float*>(data);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                micRing[mw & mask] = silent ? 0.f : f[i];
                ++mw;
            }
            capture->ReleaseBuffer(frames);
        }
        // Dérive d'horloge micro / câble : jamais plus de 100 ms de retard (on jette le plus vieux).
        if (mw - mr > kRate / 10) mr = mw - kRate / 50;
        // 2) Tampon + sa voix -> câble
        UINT32 padding = 0;
        if (FAILED(out->GetCurrentPadding(&padding))) break;  // câble débranché : la surveillance relancera
        UINT32 avail = bufferFrames - padding;
        const std::size_t micAvail = mw - mr;
        if (micAvail > 0 && micAvail < avail) avail = static_cast<UINT32>(micAvail);  // on ne joue que ce qu'on a
        if (avail == 0) continue;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) break;
        auto* o = reinterpret_cast<float*>(data);
        const bool call = inCall_.load(std::memory_order_relaxed) && route_.load(std::memory_order_relaxed);
        const float gain = gain_.load(std::memory_order_relaxed);
        const bool ear = GetTickCount64() < earrapeUntil_.load(std::memory_order_relaxed);
        std::size_t vr = voiceR_.load(std::memory_order_relaxed);
        const std::size_t vw = voiceW_.load(std::memory_order_acquire);
        for (UINT32 i = 0; i < avail; ++i) {
            float x = mr < mw ? micRing[(mr++) & mask] : 0.f;
            if (vr != vw) {
                float v = voice_[vr & (kVoiceRing - 1)] * gain;
                if (ear) {
                    // Basses x4, gain x12, écrêtage dur : le son « earrape »… mais plafonné à 0,4 (-8 dBFS).
                    earLow_ += 0.02f * (v - earLow_);
                    const float d = (v + 3.f * earLow_) * 12.f;
                    v = 0.4f * std::clamp(d, -1.f, 1.f);
                }
                if (call) x += v;
                ++vr;
            }
            o[i] = std::abs(x) > 0.95f ? std::copysign(0.95f + 0.05f * std::tanh((std::abs(x) - 0.95f) * 20.f), x) : x;
        }
        voiceR_.store(vr, std::memory_order_release);
        render->ReleaseBuffer(avail, 0);
    }
    out->Stop();
    in->Stop();
    relaying_ = false;
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CloseHandle(event);
}

// Rendu audio de la voix (24 kHz) -> 48 kHz -> tampon du relais. Temps réel : aucun verrou, aucune allocation.
void DiscordBridge::OnVoiceOut(const float* mono, std::size_t frames, int sampleRate) noexcept {
    if (!inCall_.load(std::memory_order_relaxed) || sampleRate <= 0) return;
    const double step = static_cast<double>(sampleRate) / kRate;
    std::size_t w = voiceW_.load(std::memory_order_relaxed);
    const std::size_t r = voiceR_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < frames; ++i) {
        const float x = mono[i];
        while (upPhase_ < 1.0) {
            if (w - r >= kVoiceRing) break;  // relais arrêté : on ne déborde pas
            voice_[w & (kVoiceRing - 1)] = upLast_ + (x - upLast_) * static_cast<float>(upPhase_);
            ++w;
            upPhase_ += step;
        }
        upPhase_ -= 1.0;
        if (upPhase_ < 0.0) upPhase_ = 0.0;
        upLast_ = x;
    }
    voiceW_.store(w, std::memory_order_release);
}

void DiscordBridge::Earrape(int ms) noexcept {
    earrapeUntil_.store(GetTickCount64() + static_cast<unsigned long long>(std::clamp(ms, 0, 1500)));
}

// Son de Discord (48 kHz stéréo) -> mono 16 kHz -> phrases -> Whisper (via le callback).
void DiscordBridge::OnDiscordAudio(const float* x, std::size_t frames) {
    down_.clear();
    for (std::size_t i = 0; i < frames; ++i) {
        decimAcc_ += 0.5f * (x[2 * i] + x[2 * i + 1]);
        if (++decimN_ == 3) {
            down_.push_back(decimAcc_ / 3.f);
            decimAcc_ = 0.f;
            decimN_ = 0;
        }
    }
    const bool ready = vad_.Feed(down_.data(), down_.size(), segment_);
    othersTalking_.store(vad_.InSpeech(), std::memory_order_relaxed);
    if (ready && onSpeech_) {
        onSpeech_(std::move(segment_));
        segment_ = {};
    }
}

}  // namespace companion
