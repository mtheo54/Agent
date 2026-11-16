#include "modules/SystemControl.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

ComPtr<IAudioEndpointVolume> EndpointVolume() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(volume.GetAddressOf()));
    return volume;
}

std::wstring Widen(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring ExpandEnv(const std::wstring& s) {
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

}  // namespace

std::optional<float> SystemControl::Volume() {
    float v = 0.f;
    if (auto ep = EndpointVolume(); ep && SUCCEEDED(ep->GetMasterVolumeLevelScalar(&v))) return v;
    return std::nullopt;
}

bool SystemControl::SetVolume(float level) {
    auto ep = EndpointVolume();
    return ep && SUCCEEDED(ep->SetMute(FALSE, nullptr)) &&
           SUCCEEDED(ep->SetMasterVolumeLevelScalar(std::clamp(level, 0.f, 1.f), nullptr));
}

bool SystemControl::StepVolume(float delta) {
    const auto v = Volume();
    return v && SetVolume(*v + delta);
}

bool SystemControl::SetMute(bool mute) {
    auto ep = EndpointVolume();
    return ep && SUCCEEDED(ep->SetMute(mute ? TRUE : FALSE, nullptr));
}

bool SystemControl::ToggleMute() {
    auto ep = EndpointVolume();
    BOOL muted = FALSE;
    return ep && SUCCEEDED(ep->GetMute(&muted)) && SUCCEEDED(ep->SetMute(!muted, nullptr));
}

bool SystemControl::Media(MediaKey key) {
    if (!InputSimulationAllowed()) {
        log::Info("SystemControl: SendInput refusé (jeu en cours)");
        return false;
    }
    const WORD vk = key == MediaKey::PlayPause ? VK_MEDIA_PLAY_PAUSE
                    : key == MediaKey::Next    ? VK_MEDIA_NEXT_TRACK
                                               : VK_MEDIA_PREV_TRACK;
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, in, sizeof(INPUT)) == 2;
}

const AppShortcut* SystemControl::FindApp(std::string_view utterance) const {
    std::string u(utterance);
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    u = " " + u + " ";
    for (const auto& app : apps_) {
        if (u.find(app.name) != std::string::npos) return &app;
        for (const auto& a : app.aliases)
            if (u.find(" " + a + " ") != std::string::npos) return &app;  // alias = mot entier
    }
    return nullptr;
}

bool SystemControl::Launch(const AppShortcut& app) {
    const std::wstring target = ExpandEnv(Widen(app.target));
    const std::wstring args = ExpandEnv(Widen(app.args));
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = target.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&sei) != FALSE;
    log::Info("SystemControl: lancement de {} {}", app.name, ok ? "ok" : "échoué");
    return ok;
}

}  // namespace companion
