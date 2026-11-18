#include "modules/DiscordSpeaking.hpp"

#include "core/Log.hpp"
#include "modules/SpeakingMarks.hpp"

#include <oleauto.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr GUID kClsidUia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr GUID kIidUia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// La plus grande fenêtre visible (non réduite) d'un processus Discord.exe.
HWND FindDiscordWindow() {
    std::vector<DWORD> pids;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (_wcsicmp(pe.szExeFile, L"Discord.exe") == 0) pids.push_back(pe.th32ProcessID);
        CloseHandle(snap);
    }
    if (pids.empty()) return nullptr;
    struct Ctx {
        const std::vector<DWORD>* pids;
        HWND best = nullptr;
        long area = 0;
    } ctx{&pids};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (!IsWindowVisible(h) || IsIconic(h) || std::ranges::find(*c.pids, pid) == c.pids->end()) return TRUE;
            RECT r{};
            GetClientRect(h, &r);
            if (const long a = (r.right - r.left) * (r.bottom - r.top); a > c.area) {
                c.area = a;
                c.best = h;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

std::string Utf8(const wchar_t* w, int n) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

// Pseudo affiché à cet endroit (élément accessible, ou son parent s'il n'a pas de nom).
std::string NameAt(IUIAutomation* uia, POINT pt) {
    ComPtr<IUIAutomationElement> e;
    if (FAILED(uia->ElementFromPoint(pt, &e)) || !e) return {};
    ComPtr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(&walker);
    for (int depth = 0; depth < 3 && e; ++depth) {
        BSTR b = nullptr;
        if (SUCCEEDED(e->get_CurrentName(&b)) && b && SysStringLen(b) > 0) {
            std::wstring name(b, SysStringLen(b));
            SysFreeString(b);
            // « Karim, muet », « Karim (en direct) » -> « Karim »
            if (const auto cut = name.find_first_of(L",(\n"); cut != std::wstring::npos) name.resize(cut);
            while (!name.empty() && name.back() == L' ') name.pop_back();
            if (name.size() >= 1 && name.size() <= 32) return Utf8(name.data(), static_cast<int>(name.size()));
            return {};
        }
        SysFreeString(b);
        if (!walker) break;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(e.Get(), &parent))) break;
        e = parent;
    }
    return {};
}

}  // namespace

void DiscordSpeaking::Start(std::function<bool()> shouldSample) {
    Stop();
    should_ = std::move(shouldSample);
    thread_ = std::jthread([this](std::stop_token st) { Main(st); });
}

void DiscordSpeaking::Stop() {
    thread_ = {};
    std::scoped_lock lock(mutex_);
    timeline_.clear();
}

std::vector<DiscordSpeaking::Shown> DiscordSpeaking::Sample() {
    std::vector<Shown> names;
    HWND hwnd = FindDiscordWindow();
    if (!hwnd) return names;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 200 || h < 200) return names;
    const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.f;
    // Capture du contenu de la fenêtre (marche même si elle est en partie recouverte).
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // de haut en bas
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    const bool ok = bmp && PrintWindow(hwnd, mem, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/);
    std::vector<SpeakingMark> marks;
    std::vector<AvatarSig> sigs;
    if (ok) {
        marks = FindSpeakingMarks(static_cast<const std::uint8_t*>(bits), w, h, w * 4, scale);
        for (const auto& m : marks) sigs.push_back(AvatarSignature(static_cast<const std::uint8_t*>(bits), w, h, w * 4, m));
    }
    SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (marks.empty()) return names;

    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kClsidUia, nullptr, CLSCTX_INPROC_SERVER, kIidUia, reinterpret_cast<void**>(uia.GetAddressOf()))))
        return names;
    for (std::size_t k = 0; k < marks.size(); ++k) {
        const SpeakingMark& m = marks[k];
        // Pseudo : à droite de l'anneau (liste), en bas à gauche dans la tuile (vue du call).
        POINT pt = m.tile ? POINT{m.left + static_cast<LONG>(28 * scale), m.bottom - static_cast<LONG>(18 * scale)}
                          : POINT{m.right + static_cast<LONG>(14 * scale), (m.top + m.bottom) / 2};
        ClientToScreen(hwnd, &pt);
        if (GetAncestor(WindowFromPoint(pt), GA_ROOT) != hwnd) continue;  // caché par une autre fenêtre : on ne devine pas
        std::string n = NameAt(uia.Get(), pt);
        if (!n.empty() && std::ranges::none_of(names, [&](const Shown& x) { return x.name == n; })) names.push_back({std::move(n), sigs[k]});
    }
    return names;
}

void DiscordSpeaking::Main(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.DiscordSpeaking");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Com com;
    while (!st.stop_requested()) {
        Sleep(250);
        if (!should_ || !should_()) continue;
        std::vector<Shown> people = Sample();
        const auto now = Clock::now();
        std::scoped_lock lock(mutex_);
        if (!people.empty()) lastSeen_ = now;
        timeline_.push_back({now, std::move(people)});
        while (!timeline_.empty() && now - timeline_.front().at > std::chrono::seconds(30)) timeline_.pop_front();
    }
}

DiscordSpeaking::Shown DiscordSpeaking::SpeakerBetween(Clock::time_point from, Clock::time_point to) const {
    std::scoped_lock lock(mutex_);
    std::map<std::string, int> seen;
    std::map<std::string, std::vector<std::uint8_t>> avatar;
    int samples = 0;
    for (const auto& s : timeline_) {
        if (s.at < from || s.at > to) continue;
        ++samples;
        for (const auto& p : s.people) {
            ++seen[p.name];
            if (!p.avatar.empty()) avatar[p.name] = p.avatar;
        }
    }
    if (samples < 2) return {};
    // Une seule personne vue sur au moins la moitié des images, et personne d'autre au-delà d'un quart.
    std::string who;
    for (const auto& [n, c] : seen) {
        if (c * 2 >= samples) {
            if (!who.empty()) return {};
            who = n;
        } else if (c * 4 > samples) {
            return {};
        }
    }
    if (who.empty()) return {};
    return {who, avatar[who]};
}

bool DiscordSpeaking::Seeing() const {
    std::scoped_lock lock(mutex_);
    return lastSeen_ != Clock::time_point{} && Clock::now() - lastSeen_ < std::chrono::minutes(2);
}

}  // namespace companion
