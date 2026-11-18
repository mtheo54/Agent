#pragma once
// WebSurf : « surfer » librement pour s'entraîner — SEULEMENT pendant une autorisation que tu donnes à la voix
// (« tu peux surfer cette nuit » : jusqu'à 10 h le lendemain, 14 h au plus ; « arrête de surfer » la retire).
//
//   recherche (DuckDuckGo version légère, sinon Bing) -> 3 pages -> texte -> passages utiles -> fiche apprise.
//   Lecture seule : requêtes GET simples (pas de formulaire, pas de connexion, pas de cookie, pas de téléchargement),
//   pages texte uniquement, 12 au plus par séance. Jamais : réseaux sociaux / messageries / mails / banques /
//   paiement / sites adultes.
//
// Les fonctions d'analyse sont pures (testées hors Windows : tools/sim/surftest.cpp).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace companion {

namespace surf_detail {
inline std::string UrlDecode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            o += s[i] == '+' ? ' ' : s[i];
        }
    }
    return o;
}
inline std::string HtmlAmp(std::string s) {
    for (std::size_t p; (p = s.find("&amp;")) != std::string::npos;) s.replace(p, 5, "&");
    return s;
}
inline std::string Base64UrlDecode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        const int v = val(c);
        if (v < 0) break;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}
inline std::string HostOf(const std::string& url) {
    const auto s = url.find("://");
    if (s == std::string::npos) return {};
    std::string h = url.substr(s + 3, url.find_first_of("/?#", s + 3) - (s + 3));
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h;
}
}  // namespace surf_detail

// Vidéo PUBLIQUE d'un réseau social, que l'entraînement vidéo sait regarder : YouTube (vidéos, Shorts), TikTok,
// Reels / publications Instagram. Jamais : messages privés, stories, profils, réglages, connexion.
inline bool IsSocialVideoUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || url.size() > 300 || url.find_first_of(" \"'<>") != std::string::npos) return false;
    const std::string h = surf_detail::HostOf(url);
    std::string path = url.substr(url.find('/', 8) == std::string::npos ? url.size() : url.find('/', 8));
    for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* never[] = {"/direct", "/messages", "/inbox", "/stories", "/accounts", "/login", "/settings", "/explore/people"};
    if (std::any_of(std::begin(never), std::end(never), [&](const char* n) { return path.find(n) != std::string::npos; })) return false;
    if (h == "youtu.be") return path.size() > 5;
    if (h == "www.youtube.com" || h == "youtube.com" || h == "m.youtube.com")
        return path.rfind("/watch?v=", 0) == 0 || path.rfind("/shorts/", 0) == 0;
    if (h == "www.tiktok.com" || h == "tiktok.com" || h == "m.tiktok.com") return path.find("/video/") != std::string::npos;
    if (h == "vm.tiktok.com" || h == "vt.tiktok.com") return path.size() > 3;
    if (h == "www.instagram.com" || h == "instagram.com")
        return path.rfind("/reel/", 0) == 0 || path.rfind("/reels/", 0) == 0 || path.rfind("/p/", 0) == 0;
    return false;
}

// Jamais lu, même autorisé à surfer : vie privée, argent, contenus adultes, et les moteurs eux-mêmes.
inline bool SurfBlocked(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return true;
    const std::string h = surf_detail::HostOf(url);
    if (h.empty()) return true;
    static const char* bad[] = {"facebook.", "instagram.", "messenger.", "whatsapp.", "x.com", "twitter.", "snapchat.", "discord.",
                                "telegram.", "tiktok.", "linkedin.", "outlook.", "gmail.", "proton.", "icloud.", "webmail",
                                "banque", "paypal.", "boursorama", "impots.", "ameli.", "pornhub", "xvideos", "xnxx",
                                "onlyfans", "duckduckgo.", "bing.com", "google.", "microsoft.com", "msn.com", "doubleclick"};
    // Boîtes mail et banques : par préfixe d'hôte (« mail.google.com », « bank.… ») ou nom connu, pas n'importe quelle sous-chaîne.
    if (h.rfind("mail.", 0) == 0 || h.rfind("bank", 0) == 0 || h.find(".bank") != std::string::npos) return true;
    return std::any_of(std::begin(bad), std::end(bad), [&](const char* b) { return h.find(b) != std::string::npos; });
}

// Liens de résultats de DuckDuckGo « lite » (directs, ou via //duckduckgo.com/l/?uddg=<url encodée>).
// socialVideos : ne garder QUE les vidéos publiques de réseaux sociaux (découverte pour l'entraînement vidéo).
inline std::vector<std::string> ParseDdgLite(const std::string& html, bool socialVideos = false) {
    std::vector<std::string> out;
    for (std::size_t p = html.find("result-link"); p != std::string::npos; p = html.find("result-link", p + 11)) {
        // le href est dans la même balise <a … > (avant ou après la classe)
        const std::size_t a = html.rfind("<a", p), e = html.find('>', p);
        if (a == std::string::npos || e == std::string::npos) continue;
        const std::string tag = html.substr(a, e - a);
        const auto h = tag.find("href=");
        if (h == std::string::npos) continue;
        const char q = tag[h + 5];
        const auto end = tag.find(q, h + 6);
        std::string url = surf_detail::HtmlAmp(tag.substr(h + 6, end - h - 6));
        if (const auto u = url.find("uddg="); u != std::string::npos) url = surf_detail::UrlDecode(url.substr(u + 5, url.find('&', u) - u - 5));
        if (url.rfind("//", 0) == 0) url = "https:" + url;
        const bool keep = socialVideos ? IsSocialVideoUrl(url) : !SurfBlocked(url);
        if (keep && std::ranges::find(out, url) == out.end()) out.push_back(url);
    }
    return out;
}

// Résultats Bing (<li class="b_algo"><h2><a href="…">), liens directs ou /ck/a?…&u=a1<base64url>.
inline std::vector<std::string> ParseBing(const std::string& html, bool socialVideos = false) {
    std::vector<std::string> out;
    for (std::size_t p = html.find("class=\"b_algo\""); p != std::string::npos; p = html.find("class=\"b_algo\"", p + 14)) {
        const auto h = html.find("href=\"", html.find("<h2", p));
        if (h == std::string::npos) break;
        std::string url = surf_detail::HtmlAmp(html.substr(h + 6, html.find('"', h + 6) - h - 6));
        if (const auto u = url.find("&u=a1"); u != std::string::npos) url = surf_detail::Base64UrlDecode(url.substr(u + 5, url.find('&', u + 5) - u - 5));
        const bool keep = socialVideos ? IsSocialVideoUrl(url) : !SurfBlocked(url);
        if (keep && std::ranges::find(out, url) == out.end()) out.push_back(url);
    }
    return out;
}

class HttpClient;
class MemoryDB;

class WebSurf {
public:
    explicit WebSurf(HttpClient& http) : http_(http) {}
    // Autorisation de surfer libre en cours ? (profil « web.freeUntil », heure Unix)
    static bool Allowed(MemoryDB& db);
    // Recherche puis lecture des meilleures pages : « ### titre (url)\n passages utiles » ; budget de pages partagé.
    std::string Research(const std::string& query, int maxPages, int& pagesLeft);
    // Découverte : vidéos publiques TikTok / Reels Instagram / Shorts trouvées par un moteur de recherche
    // (« site:tiktok.com vanne »). Rien n'est ouvert ici : les liens vont à l'entraînement vidéo.
    std::vector<std::string> FindSocialVideos(const std::string& query);

private:
    HttpClient& http_;
    std::vector<std::string> SearchLinks(const std::string& query, bool socialVideos = false);
};

}  // namespace companion
