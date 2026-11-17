#include "network/HttpClient.hpp"

#include <fstream>

#include "core/Log.hpp"

#include <windows.h>
#include <winhttp.h>

namespace companion {
namespace {

std::wstring Widen(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

}  // namespace

HttpClient::HttpClient(const std::string& userAgent) {
    // User-Agent générique par défaut : aucune information personnelle n'est envoyée.
    // (Wikipédia limite les clients anonymes ; "userAgent" dans companion.json permet d'y ajouter un contact.)
    session_ = WinHttpOpen(Widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
}

HttpClient::~HttpClient() {
    if (session_) WinHttpCloseHandle(session_);
}

std::string HttpClient::UrlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

HttpResponse HttpClient::Send(const HttpRequest& req) {
    HttpResponse res;
    if (!session_) { res.error = "WinHttpOpen a échoué"; return res; }

    std::wstring url = Widen(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { res.error = "URL invalide"; return res; }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    Handle connect{WinHttpConnect(session_, host.c_str(), uc.nPort, 0)};
    if (!connect.h) { res.error = "connexion impossible"; return res; }
    Handle request{WinHttpOpenRequest(connect.h, req.method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h) { res.error = "requête impossible"; return res; }

    WinHttpSetTimeouts(request.h, req.timeoutMs, req.timeoutMs, req.timeoutMs, req.timeoutMs);
    if (https && req.allowSelfSignedLocalhost && host == L"127.0.0.1") {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request.h, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    std::wstring headers;
    for (const auto& [k, v] : req.headers) headers += k + L": " + v + L"\r\n";

    const BOOL sent = WinHttpSendRequest(
        request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1L),
        req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data()),
        static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) {
        res.error = "échec réseau (" + std::to_string(GetLastError()) + ")";
        return res;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);
    {
        wchar_t len[32] = {};
        DWORD lsz = sizeof(len);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz,
                                WINHTTP_NO_HEADER_INDEX))
            res.contentLength = std::wcstoull(len, nullptr, 10);
    }
    if (req.sink) {  // flux : rien n'est gardé en mémoire
        std::string buf(256 * 1024, '\0');
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
                res.error = "lecture interrompue (" + std::to_string(GetLastError()) + ")";
                break;
            }
            if (read == 0) break;
            if (!req.sink(std::string_view(buf.data(), read), res)) {
                res.error = "annulé";
                break;
            }
        }
        return res;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail) || avail == 0) break;
        const size_t old = res.body.size();
        if (old + avail > req.maxBodyBytes) { res.error = "réponse trop volumineuse"; break; }
        res.body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, res.body.data() + old, avail, &read)) break;
        res.body.resize(old + read);
    }
    return res;
}

bool HttpClient::DownloadToFile(const std::string& url, const std::filesystem::path& dest,
                                const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    auto part = dest;
    part += L".part";
    const std::uint64_t have = std::filesystem::exists(part, ec) ? std::filesystem::file_size(part, ec) : 0;
    std::ofstream out(part, std::ios::binary | (have ? std::ios::app : std::ios::trunc));
    if (!out) {
        if (error) *error = "écriture impossible";
        return false;
    }
    std::uint64_t done = have, total = 0;
    HttpRequest req;
    req.url = url;
    req.timeoutMs = 60000;
    if (have) req.headers.push_back({L"Range", L"bytes=" + std::to_wstring(have) + L"-"});
    bool first = true;
    req.sink = [&](std::string_view chunk, const HttpResponse& head) {
        if (first) {
            first = false;
            total = (head.status == 206 ? have : 0) + head.contentLength;
            // Le serveur ignore la reprise (200 au lieu de 206) : on repart de zéro.
            if (have && head.status == 200) {
                out.close();
                out.open(part, std::ios::binary | std::ios::trunc);
                done = 0;
            }
        }
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        done += chunk.size();
        return static_cast<bool>(out) && (!progress || progress(done, total));
    };
    const HttpResponse res = Send(req);
    out.close();
    if (!res.error.empty() || (res.status != 200 && res.status != 206) || (total && done != total)) {
        if (error) *error = !res.error.empty() ? res.error : "HTTP " + std::to_string(res.status);
        return false;  // le .part reste : la prochaine tentative reprendra
    }
    std::filesystem::rename(part, dest, ec);
    if (ec) {
        if (error) *error = "renommage impossible";
        return false;
    }
    return true;
}

}  // namespace companion
