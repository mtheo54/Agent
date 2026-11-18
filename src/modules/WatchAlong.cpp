#include "modules/WatchAlong.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cwctype>

namespace companion {
namespace {

constexpr const wchar_t* kBrowsers[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe", L"vivaldi.exe"};

std::wstring ExeOf(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::wstring exe;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            std::wstring full(buf, n);
            exe = full.substr(full.find_last_of(L"\\/") + 1);
            for (auto& c : exe) c = static_cast<wchar_t>(std::towlower(c));
        }
        CloseHandle(p);
    }
    return exe;
}

bool IsBrowser(const std::wstring& exe) {
    return std::any_of(std::begin(kBrowsers), std::end(kBrowsers), [&](const wchar_t* b) { return exe == b; });
}

}  // namespace

bool PrivateTabTitle(const std::string& t) {
    for (const char* w : {"message", "direct", "inbox", "boîte de réception", "boite de reception", "discussion", "chat ", "conversation",
                          "whatsapp", "messenger", "gmail", "outlook", "mail"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

bool WatchAlong::Start(Segment onSegment, std::string& error) {
    Stop();
    // Le navigateur au premier plan ; sinon le premier navigateur lancé.
    HWND fg = GetAncestor(GetForegroundWindow(), GA_ROOT);
    exe_ = fg ? ExeOf(fg) : std::wstring{};
    window_ = IsBrowser(exe_) ? fg : nullptr;
    if (!window_) {
        exe_.clear();
        for (const wchar_t* b : kBrowsers)
            if (WasapiLoopback::FindProcessId(b)) {
                exe_ = b;
                break;
            }
    }
    if (exe_.empty()) {
        error = "aucun navigateur ouvert";
        return false;
    }
    onSegment_ = std::move(onSegment);
    vad_ = CallVad{};
    started_ = std::chrono::steady_clock::now();
    if (!capture_.Start(exe_, [this](const float* x, std::size_t n) { OnAudio(x, n); })) {
        error = "le son du navigateur n'est pas capturable";
        return false;
    }
    log::Info("WatchAlong: écoute de {}", log::Narrow(exe_));
    return true;
}

void WatchAlong::Stop() {
    capture_.Stop();
    window_ = nullptr;
}

std::string WatchAlong::Title() const {
    HWND h = window_;
    if (!h || !IsWindow(h)) {  // fenêtre fermée / pas au premier plan au départ : le premier plan s'il s'agit du navigateur
        HWND fg = GetAncestor(GetForegroundWindow(), GA_ROOT);
        if (fg && ExeOf(fg) == exe_) h = fg;
        else return {};
    }
    wchar_t buf[512];
    const int n = GetWindowTextW(h, buf, 512);
    return n > 0 ? log::Narrow(std::wstring_view(buf, static_cast<std::size_t>(n))) : std::string{};
}

// Son du navigateur (48 kHz stéréo) -> 16 kHz mono -> phrases (12 s au plus) -> transcription (via le callback).
void WatchAlong::OnAudio(const float* x, std::size_t frames) {
    down_.clear();
    for (std::size_t i = 0; i < frames; ++i) {
        acc_ += 0.5f * (x[2 * i] + x[2 * i + 1]);
        if (++accN_ == 3) {
            down_.push_back(acc_ / 3.f);
            acc_ = 0.f;
            accN_ = 0;
        }
    }
    if (!vad_.Feed(down_.data(), down_.size(), segment_) || !onSegment_) return;
    std::string title = Title();
    std::string lower = title;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (PrivateTabTitle(lower)) {  // messages privés à l'écran : on jette, on ne transcrit pas
        segment_.clear();
        return;
    }
    onSegment_(std::move(segment_), std::move(title));
    segment_ = {};
}

}  // namespace companion
