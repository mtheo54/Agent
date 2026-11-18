#pragma once
// Journalisation : OutputDebugString (DebugView / débogueur VS) + fichier omnis.log (OpenFile), écrit ligne par
// ligne sans tampon : après un plantage, les dernières lignes disent où ça s'est arrêté.
#include <windows.h>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace companion::log {

inline HANDLE& FileHandle() {
    static HANDLE h = INVALID_HANDLE_VALUE;
    return h;
}
// %LOCALAPPDATA%\CompanionAI\omnis.log (l'ancien devient omnis.prev.log). Une fois, au démarrage.
inline void OpenFile(const std::wstring& path) {
    MoveFileExW(path.c_str(), (path.substr(0, path.size() - 4) + L".prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    FileHandle() = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
}
// Texte brut (déjà terminé par \n), horodaté : aussi utilisé pour les messages de llama.cpp / ggml / whisper.
inline void Raw(std::string_view text) {
    const HANDLE h = FileHandle();
    if (h == INVALID_HANDLE_VALUE || text.empty()) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[32];
    const int n = std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::string line(stamp, static_cast<size_t>(n));
    line += text;
    if (line.back() != '\n') line += '\n';
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = "[Companion] " + std::format(fmt, std::forward<Args>(args)...) + "\n";
    Raw(std::string_view(msg).substr(12));
    const int n = MultiByteToWideChar(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), w.data(), n);
    OutputDebugStringW(w.c_str());
}

// SetThreadDescription (Win10 1607+) résolu dynamiquement : visible dans le débogueur / profileurs.
inline void SetCurrentThreadName(const wchar_t* name) noexcept {
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (fn) fn(GetCurrentThread(), name);
}

inline std::string Narrow(std::wstring_view w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring Widen(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace companion::log
