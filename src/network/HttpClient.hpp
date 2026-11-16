#pragma once
// HttpClient : client HTTPS synchrone minimal sur WinHTTP (composant Windows, aucune dépendance).
// Thread-safe : une session WinHTTP partagée, une requête par appel.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace companion {

struct HttpRequest {
    std::wstring method = L"GET";
    std::string url;  // UTF-8, http:// ou https://
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::string body;
    int timeoutMs = 15000;
    // Certificat auto-signé accepté UNIQUEMENT si l'hôte est 127.0.0.1 (API locale de LoL).
    bool allowSelfSignedLocalhost = false;
    size_t maxBodyBytes = 8u << 20;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
    [[nodiscard]] bool Ok() const noexcept { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse Send(const HttpRequest& req);
    HttpResponse Get(std::string url, int timeoutMs = 15000) {
        HttpRequest r;
        r.url = std::move(url);
        r.timeoutMs = timeoutMs;
        return Send(r);
    }

    static std::string UrlEncode(std::string_view s);

private:
    void* session_ = nullptr;  // HINTERNET
};

}  // namespace companion
