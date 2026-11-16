#include "network/WebResearcher.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>

namespace companion {
namespace {

constexpr size_t kMaxPageChars = 20000;

std::string Lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

std::set<std::string> Terms(std::string_view query) {
    std::set<std::string> terms;
    std::string cur;
    auto flush = [&] { if (cur.size() >= 3) terms.insert(cur); cur.clear(); };
    for (unsigned char c : Lower(query)) {
        if (c >= 0x80 || std::isalnum(c)) cur.push_back(static_cast<char>(c));
        else flush();
    }
    flush();
    return terms;
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

const WikiSource* WebResearcher::PickSource(std::string_view query) const {
    if (sources_.empty()) return nullptr;
    const std::string q = " " + Lower(query) + " ";
    const WikiSource* best = &sources_.back();
    int bestScore = 0;
    for (const auto& s : sources_) {
        int score = 0;
        for (const auto& k : s.keywords)
            if (q.find(Lower(k)) != std::string::npos) ++score;
        if (score > bestScore) { bestScore = score; best = &s; }
    }
    return best;
}

std::string WebResearcher::HtmlToText(std::string_view html) {
    std::string out;
    out.reserve(html.size() / 3);
    size_t i = 0;
    auto skipBlock = [&](std::string_view tag) {  // supprime <script>…</script>, <style>…</style>
        const std::string close = "</" + std::string(tag);
        const size_t end = Lower(html.substr(i, std::min<size_t>(html.size() - i, 200000))).find(close);
        if (end == std::string::npos) { i = html.size(); return; }
        const size_t gt = html.find('>', i + end);
        i = gt == std::string_view::npos ? html.size() : gt + 1;
    };
    while (i < html.size() && out.size() < kMaxPageChars) {
        const char c = html[i];
        if (c == '<') {
            const std::string head = Lower(html.substr(i, 8));
            if (head.starts_with("<script") || head.starts_with("<style")) {
                skipBlock(head.starts_with("<script") ? "script" : "style");
                continue;
            }
            const size_t end = html.find('>', i);
            if (end == std::string_view::npos) break;
            // Fins de blocs -> retour à la ligne ; cellules -> séparateur.
            const std::string tag = Lower(html.substr(i, std::min<size_t>(end - i, 6)));
            if (tag.starts_with("<img")) {  // icônes (objets, sorts) : le nom est dans alt="…"
                const std::string_view img = html.substr(i, end - i);
                if (const size_t a = img.find("alt=\""); a != std::string_view::npos) {
                    const size_t e = img.find('"', a + 5);
                    if (e != std::string_view::npos && e > a + 5) {
                        out.push_back(' ');
                        out += HtmlToText(img.substr(a + 5, e - a - 5));  // décode les entités
                        out.push_back(' ');
                    }
                }
            }
            if (tag.starts_with("</p") || tag.starts_with("<br") || tag.starts_with("</li") ||
                tag.starts_with("</h") || tag.starts_with("</tr") || tag.starts_with("</div"))
                out.push_back('\n');
            else if (tag.starts_with("</td") || tag.starts_with("</th"))
                out += " | ";
            i = end + 1;
        } else if (c == '&') {
            const size_t end = html.find(';', i);
            if (end != std::string_view::npos && end - i <= 10) {
                const std::string_view ent = html.substr(i + 1, end - i - 1);
                if (ent == "amp") out.push_back('&');
                else if (ent == "lt") out.push_back('<');
                else if (ent == "gt") out.push_back('>');
                else if (ent == "quot") out.push_back('"');
                else if (ent == "apos" || ent == "#39") out.push_back('\'');
                else if (ent == "nbsp" || ent == "#160") out.push_back(' ');
                else if (ent.starts_with("#x") || ent.starts_with("#X"))
                    AppendUtf8(out, static_cast<std::uint32_t>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)));
                else if (ent.starts_with("#"))
                    AppendUtf8(out, static_cast<std::uint32_t>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10)));
                i = end + 1;
            } else {
                out.push_back(c);
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    // Normalisation des espaces : un seul espace, lignes vides fusionnées.
    std::string clean;
    clean.reserve(out.size());
    bool space = false, newline = false;
    for (char ch : out) {
        if (ch == '\n' || ch == '\r') { newline = true; space = false; continue; }
        if (ch == ' ' || ch == '\t') { space = true; continue; }
        if (newline && !clean.empty()) clean.push_back('\n');
        else if (space && !clean.empty()) clean.push_back(' ');
        newline = space = false;
        clean.push_back(ch);
    }
    return clean;
}

std::string WebResearcher::BestPassages(std::string_view text, std::string_view query, size_t maxChars) {
    const auto terms = Terms(query);
    struct Line { size_t start, len; int score; };
    std::vector<Line> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string l = Lower(text.substr(pos, end - pos));
        int score = 0;
        for (const auto& t : terms)
            for (size_t f = l.find(t); f != std::string::npos; f = l.find(t, f + t.size())) ++score;
        if (end > pos + 20) lines.push_back({pos, end - pos, score});
        pos = end + 1;
    }
    // Les lignes les mieux notées, restituées dans l'ordre du document.
    std::vector<size_t> order(lines.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lines[a].score > lines[b].score; });
    std::vector<size_t> keep;
    size_t total = 0;
    for (size_t idx : order) {
        if (total + lines[idx].len > maxChars) continue;
        if (lines[idx].score == 0 && !keep.empty()) break;
        keep.push_back(idx);
        total += lines[idx].len + 1;
    }
    std::sort(keep.begin(), keep.end());
    std::string out;
    for (size_t idx : keep) {
        out.append(text.substr(lines[idx].start, lines[idx].len));
        out.push_back('\n');
    }
    return out;
}

std::vector<WebDocument> WebResearcher::Search(const WikiSource& src, std::string_view query, int maxPages,
                                               std::stop_token stop) {
    std::vector<WebDocument> docs;
    const std::string base = "https://" + src.host + src.apiPath;
    const auto search = http_.Get(base + "?action=query&list=search&format=json&formatversion=2&srlimit=" +
                                  std::to_string(maxPages) + "&srsearch=" + HttpClient::UrlEncode(query));
    if (!search.Ok()) {
        log::Info("Web: recherche échouée sur {} ({} {})", src.host, search.status, search.error);
        return docs;
    }
    std::vector<std::string> titles;
    try {
        // Le document doit vivre pendant la boucle (une référence dans un temporaire serait détruite).
        const auto json = nlohmann::json::parse(search.body);
        for (const auto& r : json.at("query").at("search")) titles.push_back(r.at("title").get<std::string>());
    } catch (const std::exception& e) {
        log::Info("Web: réponse de recherche illisible ({})", e.what());
        return docs;
    }

    for (const auto& title : titles) {
        if (stop.stop_requested()) break;
        const auto page = http_.Get(base + "?action=parse&prop=text&format=json&formatversion=2&redirects=1"
                                           "&disablelimitreport=1&page=" + HttpClient::UrlEncode(title));
        if (!page.Ok()) continue;
        try {
            const auto j = nlohmann::json::parse(page.body);
            WebDocument d;
            d.title = j.at("parse").value("title", title);
            d.url = "https://" + src.host + "/wiki/" + HttpClient::UrlEncode(d.title);
            d.text = HtmlToText(j.at("parse").at("text").get<std::string>());
            d.excerpt = BestPassages(d.text, query, 2500);
            if (!d.excerpt.empty()) docs.push_back(std::move(d));
        } catch (const std::exception&) {
            continue;
        }
    }
    log::Info("Web: {} page(s) utile(s) sur {}", docs.size(), src.host);
    return docs;
}

}  // namespace companion
