#pragma once
// HttpClient : client HTTPS synchrone minimal sur WinHTTP (composant Windows, aucune dépendance).
// Thread-safe : une session WinHTTP partagée, une requête par appel.

#include <functional>
#include <filesystem>
#include <cstdint>
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
    // Flux : chaque morceau reçu est passé ici (avec le statut et la taille annoncée déjà connus) au lieu
    // d'être gardé en mémoire (gros téléchargements). Retourner false interrompt le transfert.
    std::function<bool(std::string_view chunk, const struct HttpResponse& head)> sink;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
    std::uint64_t contentLength = 0;  // annoncé par le serveur (0 = inconnu)
    [[nodiscard]] bool Ok() const noexcept { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    explicit HttpClient(const std::string& userAgent = "CompanionAI/0.3 (desktop assistant)");
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

    // Téléchargement d'un gros fichier sur le disque, en flux, avec reprise (.part + en-tête Range) et
    // écriture atomique (renommage à la fin). progress(fait, total) -> false pour annuler.
    bool DownloadToFile(const std::string& url, const std::filesystem::path& dest,
                        const std::function<bool(std::uint64_t done, std::uint64_t total)>& progress, std::string* error = nullptr);

private:
    void* session_ = nullptr;  // HINTERNET
};

}  // namespace companion
