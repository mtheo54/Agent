#pragma once
// Journalisation minimale : OutputDebugString (visible dans DebugView / débogueur VS).
#include <windows.h>
#include <format>
#include <string>
#include <string_view>

namespace companion::log {

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = "[Companion] " + std::format(fmt, std::forward<Args>(args)...) + "\n";
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
