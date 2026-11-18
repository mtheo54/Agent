#pragma once
// VideoParse : ce que yt-dlp rend, en texte utile (logique pure, testée hors Windows : tools/sim/videotest.cpp).
//   - sous-titres YouTube « json3 » -> texte ;
//   - info.json -> les commentaires les plus likés (le rage bait, c'est aussi ce que les gens répondent) ;
//   - « id \t titre \t durée \t vues » (recherche) -> liste ;
//   - SHA2-256SUMS / .sha256sum -> empreinte attendue d'un fichier ;
//   - la fiche d'étude écrite par le modèle (JSON imposé par grammaire) -> leçon + formes de vannes.

#include "core/BaitBank.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace companion {

struct VideoItem {
    std::string id, url, title;
    int seconds = 0;
    long long views = 0;
};

inline std::string Json3ToText(const std::string& json) {
    std::string out;
    try {
        const auto j = nlohmann::json::parse(json);
        for (const auto& ev : j.value("events", nlohmann::json::array()))
            for (const auto& seg : ev.value("segs", nlohmann::json::array())) {
                std::string t = seg.value("utf8", std::string{});
                for (auto& c : t)
                    if (c == '\n') c = ' ';
                if (t == " " && (!out.empty() && out.back() == ' ')) continue;
                out += t;
            }
    } catch (const std::exception&) {
        return {};
    }
    std::string clean;  // espaces multiples -> un
    for (char c : out)
        if (!(c == ' ' && !clean.empty() && clean.back() == ' ')) clean += c;
    return clean;
}

inline std::string TopComments(const std::string& infoJson, int n) {
    try {
        const auto j = nlohmann::json::parse(infoJson);
        std::vector<std::pair<long long, std::string>> cs;
        for (const auto& c : j.value("comments", nlohmann::json::array())) {
            std::string t = c.value("text", std::string{});
            if (t.empty() || t.size() > 300) continue;
            cs.emplace_back(c.value("like_count", 0LL), std::move(t));
        }
        std::ranges::sort(cs, [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string out;
        for (int i = 0; i < n && i < static_cast<int>(cs.size()); ++i) out += "- " + cs[static_cast<std::size_t>(i)].second + " (" + std::to_string(cs[static_cast<std::size_t>(i)].first) + " likes)\n";
        return out;
    } catch (const std::exception&) {
        return {};
    }
}

inline std::vector<VideoItem> ParseSearchLines(const std::string& text) {
    std::vector<VideoItem> out;
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        p = e + 1;
        std::vector<std::string> f;
        std::size_t a = 0;
        for (std::size_t t; (t = line.find('\t', a)) != std::string::npos; a = t + 1) f.push_back(line.substr(a, t - a));
        f.push_back(line.substr(a));
        if (f.size() < 4 || f[0].size() < 6) continue;
        VideoItem v;
        v.id = f[0];
        v.title = f[1];
        v.seconds = std::atoi(f[2].c_str());
        v.views = std::atoll(f[3].c_str());
        v.url = "https://www.youtube.com/watch?v=" + v.id;
        out.push_back(std::move(v));
    }
    return out;
}

// Empreinte SHA-256 (64 hexadécimaux, minuscules) attendue pour `file` dans un fichier de sommes
// (« <hash>  yt-dlp.exe ») ou une fiche PowerShell (« Hash : <HASH> »). Vide si introuvable.
inline std::string ExpectedSha256(const std::string& sums, const std::string& file) {
    const auto hexRun = [](const std::string& s, std::size_t from) -> std::string {
        for (std::size_t i = from; i + 64 <= s.size(); ++i) {
            std::size_t k = 0;
            while (k < 64 && std::isxdigit(static_cast<unsigned char>(s[i + k]))) ++k;
            if (k == 64 && (i + 64 == s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + 64])))) {
                std::string h = s.substr(i, 64);
                for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return h;
            }
            if (k) i += k;
        }
        return {};
    };
    std::size_t p = 0;
    while (p < sums.size()) {  // format « hash  fichier » : la ligne du bon fichier
        std::size_t e = sums.find('\n', p);
        if (e == std::string::npos) e = sums.size();
        std::string line = sums.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > file.size() && line.compare(line.size() - file.size(), file.size(), file) == 0)
            if (auto h = hexRun(line, 0); !h.empty()) return h;
    }
    if (sums.find("Hash") != std::string::npos) return hexRun(sums, sums.find("Hash"));  // fiche « Hash : … »
    return {};
}

// Fiche d'étude imposée au modèle.
inline constexpr const char* kStudyGrammar = R"(root ::= "{" ws "\"humour\":" ws bool "," ws "\"technique\":" ws str "," ws "\"pourquoi\":" ws str "," ws "\"formes\":" ws "[" ws (str (ws "," ws str)? (ws "," ws str)? (ws "," ws str)?)? ws "]" ws "}"
str ::= "\"" ([^"\\\n] | "\\" ["\\/nt])* "\""
bool ::= "true" | "false"
ws ::= [ \n]*
)";

inline constexpr const char* kStudySystem =
    "Tu étudies l'humour pour apprendre l'art de la vanne et du rage bait ENTRE POTES : taquiner, faire réagir, jamais blesser.";

inline std::string StudyPrompt(const std::string& title, const std::string& transcript, const std::string& comments) {
    return "Vidéo : « " + title + " »\n[Transcription]\n" + transcript + "\n[Commentaires les plus likés]\n" + comments +
           "\nRéponds en JSON. humour : la vidéo contient-elle des vannes, de la répartie ou du rage bait ? technique : nom court de la "
           "technique principale. pourquoi : en une phrase, pourquoi ça fait rire ou réagir. formes : 1 à 4 FORMES de vannes "
           "réutilisables entre potes, inspirées de la vidéo mais ÉCRITES PAR TOI (pas recopiées), avec des [crochets] pour ce qui "
           "dépend de la cible (ex. [son objectif], [sa phrase], [sujet], [chose]). Jamais sur l'origine, la religion, la sexualité, le "
           "genre, le physique, un handicap, la famille, la santé ou l'argent.";
}

struct StudyResult {
    bool humour = false;
    std::string technique, why;
    std::vector<BaitForm> forms;
};

inline StudyResult ParseStudy(const std::string& json, const std::string& source) {
    StudyResult r;
    try {
        const auto j = nlohmann::json::parse(json);
        r.humour = j.value("humour", false);
        r.technique = j.value("technique", std::string{});
        r.why = j.value("pourquoi", std::string{});
        for (const auto& f : j.value("formes", std::vector<std::string>{}))
            if (f.find('[') != std::string::npos && f.size() >= 12 && f.size() <= 220) r.forms.push_back({r.technique, f, true, source});
    } catch (const std::exception&) {
    }
    return r;
}

}  // namespace companion
