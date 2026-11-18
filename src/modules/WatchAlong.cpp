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
    {
        std::scoped_lock lock(frameMutex_);
        frames_.clear();
        prevThumb_.clear();
        lastCut_ = started_;
    }
    frameThread_ = std::jthread([this](std::stop_token st) { FrameMain(st); });
    log::Info("WatchAlong: écoute et images de {}", log::Narrow(exe_));
    return true;
}

void WatchAlong::Stop() {
    frameThread_ = {};
    capture_.Stop();
    window_ = nullptr;
    std::scoped_lock lock(frameMutex_);
    frames_.clear();  // rien n'est gardé
}

HWND WatchAlong::CurrentWindow() const {
    if (window_ && IsWindow(window_) && !IsIconic(window_)) return window_;
    HWND fg = GetAncestor(GetForegroundWindow(), GA_ROOT);
    return fg && ExeOf(fg) == exe_ && !IsIconic(fg) ? fg : nullptr;
}

// Une image toutes les 2,5 s de la fenêtre du navigateur (même partiellement cachée), réduite à 448 px ; jamais sur un
// onglet de messages. Une vignette 16×16 en niveaux de gris repère le « swipe » vers la vidéo suivante.
void WatchAlong::FrameMain(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.WatchFrames");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!st.stop_requested()) {
        for (int i = 0; i < 25 && !st.stop_requested(); ++i) Sleep(100);
        if (st.stop_requested()) break;
        std::string title = Title();
        for (auto& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (PrivateTabTitle(title)) continue;
        HWND h = CurrentWindow();
        if (!h) continue;
        RECT rc{};
        GetClientRect(h, &rc);
        const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
        if (w < 200 || ht < 200) continue;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -ht;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        const bool ok = bmp && PrintWindow(h, mem, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/);
        WatchFrame f;
        std::vector<std::uint8_t> thumb(256, 0);
        double lum = 0;
        if (ok) {
            const float scale = std::min(1.f, 448.f / static_cast<float>(std::max(w, ht)));
            f.width = std::max(1, static_cast<int>(static_cast<float>(w) * scale));
            f.height = std::max(1, static_cast<int>(static_cast<float>(ht) * scale));
            f.rgb.resize(static_cast<std::size_t>(f.width) * f.height * 3);
            const auto* src = static_cast<const std::uint8_t*>(bits);
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const int sx = std::min(w - 1, static_cast<int>(static_cast<float>(x) / scale));
                    const int sy = std::min(ht - 1, static_cast<int>(static_cast<float>(y) / scale));
                    const std::uint8_t* p = src + (static_cast<std::size_t>(sy) * w + sx) * 4;
                    std::uint8_t* d = &f.rgb[(static_cast<std::size_t>(y) * f.width + x) * 3];
                    d[0] = p[2];
                    d[1] = p[1];
                    d[2] = p[0];
                    lum += p[1];
                }
            for (int ty = 0; ty < 16; ++ty)
                for (int tx = 0; tx < 16; ++tx) {
                    const std::uint8_t* d = &f.rgb[(static_cast<std::size_t>(ty * f.height / 16) * f.width + tx * f.width / 16) * 3];
                    thumb[static_cast<std::size_t>(ty * 16 + tx)] = static_cast<std::uint8_t>((d[0] + 2 * d[1] + d[2]) / 4);
                }
        }
        SelectObject(mem, old);
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!ok || lum / (static_cast<double>(f.width) * f.height) < 3.0) continue;  // image noire (fenêtre réduite…)
        f.at = std::chrono::steady_clock::now();
        std::scoped_lock lock(frameMutex_);
        if (!prevThumb_.empty()) {
            int diff = 0;
            for (std::size_t i = 0; i < 256; ++i) diff += std::abs(static_cast<int>(thumb[i]) - static_cast<int>(prevThumb_[i]));
            if (diff / 256 > 40) lastCut_ = f.at;  // l'image a changé d'un coup : vidéo suivante
        }
        prevThumb_ = std::move(thumb);
        frames_.push_back(std::move(f));
        while (frames_.size() > 8) frames_.pop_front();  // 20 s
    }
}

std::vector<WatchFrame> WatchAlong::FramesSince(std::chrono::steady_clock::time_point since) const {
    std::scoped_lock lock(frameMutex_);
    std::vector<WatchFrame> out;
    for (const auto& f : frames_)
        if (f.at >= since) out.push_back(f);
    return out;
}

std::vector<WatchFrame> WatchAlong::FramesBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) const {
    std::scoped_lock lock(frameMutex_);
    std::vector<WatchFrame> out;
    for (const auto& f : frames_)
        if (f.at >= from && f.at < to) out.push_back(f);
    return out;
}

std::chrono::steady_clock::time_point WatchAlong::LastCut() const {
    std::scoped_lock lock(frameMutex_);
    return lastCut_;
}

WatchFrame SideBySide(const WatchFrame& a, const WatchFrame& b) {
    WatchFrame out;
    const int h = std::min(a.height, b.height);
    if (h <= 0) return a.width ? a : b;
    const int wa = a.width * h / a.height, wb = b.width * h / b.height;
    out.width = wa + wb;
    out.height = h;
    out.rgb.assign(static_cast<std::size_t>(out.width) * h * 3, 0);
    const auto blit = [&](const WatchFrame& f, int w, int x0) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = std::min(f.width - 1, x * f.width / w), sy = std::min(f.height - 1, y * f.height / h);
                const std::uint8_t* s = &f.rgb[(static_cast<std::size_t>(sy) * f.width + sx) * 3];
                std::uint8_t* d = &out.rgb[(static_cast<std::size_t>(y) * out.width + x0 + x) * 3];
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
    };
    blit(a, wa, 0);
    blit(b, wb, wa);
    out.at = b.at;
    return out;
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
