#include "network/WebSurf.hpp"

#include "core/Log.hpp"
#include "core/MemoryDB.hpp"
#include "network/HttpClient.hpp"
#include "network/WebResearcher.hpp"

#include <ctime>

namespace companion {

bool WebSurf::Allowed(MemoryDB& db) {
    const auto until = std::atoll(db.GetProfile("web.freeUntil").value_or("0").c_str());
    return until > static_cast<long long>(std::time(nullptr));
}

std::vector<std::string> WebSurf::SearchLinks(const std::string& query) {
    HttpRequest r;
    r.headers = {{L"Accept-Language", L"fr-FR,fr;q=0.9"}};
    r.timeoutMs = 12000;
    r.maxBodyBytes = 1u << 20;
    r.url = "https://lite.duckduckgo.com/lite/?q=" + HttpClient::UrlEncode(query);
    auto res = http_.Send(r);
    std::vector<std::string> links = res.Ok() ? ParseDdgLite(res.body) : std::vector<std::string>{};
    if (links.empty()) {  // DuckDuckGo refuse (page « anomaly ») : Bing
        r.url = "https://www.bing.com/search?setlang=fr&q=" + HttpClient::UrlEncode(query);
        res = http_.Send(r);
        if (res.Ok()) links = ParseBing(res.body);
    }
    return links;
}

std::string WebSurf::Research(const std::string& query, int maxPages, int& pagesLeft) {
    std::string out;
    if (pagesLeft <= 0) return out;
    const auto links = SearchLinks(query);
    log::Info("WebSurf: « {} » -> {} lien(s)", query, links.size());
    int read = 0;
    for (const auto& url : links) {
        if (read >= maxPages || pagesLeft <= 0) break;
        HttpRequest r;  // GET simple : ni formulaire, ni cookie, ni connexion
        r.url = url;
        r.timeoutMs = 10000;
        r.maxBodyBytes = 2u << 20;
        r.headers = {{L"Accept", L"text/html"}, {L"Accept-Language", L"fr-FR,fr;q=0.9"}};
        const auto page = http_.Send(r);
        --pagesLeft;
        if (!page.Ok() || page.body.find("<html") == std::string::npos) continue;  // pages texte seulement
        const std::string text = WebResearcher::HtmlToText(page.body);
        if (text.size() < 400) continue;
        std::string title;
        if (const auto t = page.body.find("<title"); t != std::string::npos) {
            const auto b = page.body.find('>', t), e = page.body.find("</title>", t);
            if (b != std::string::npos && e != std::string::npos && e > b) title = WebResearcher::HtmlToText(page.body.substr(b + 1, e - b - 1));
        }
        out += "### " + title.substr(0, 120) + " (" + url + ")\n" + WebResearcher::BestPassages(text, query, 1500) + "\n";
        ++read;
    }
    return out;
}

}  // namespace companion
