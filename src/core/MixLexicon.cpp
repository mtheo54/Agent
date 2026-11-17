#include "core/MixLexicon.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace companion {

std::string MixLexicon::Normalize(const std::string& in) {
    // Accents français courants (UTF-8 sur 2 octets, 0xC3 xx) → lettre de base.
    static const std::pair<const char*, char> accents[] = {
        {"à", 'a'}, {"â", 'a'}, {"ä", 'a'}, {"é", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'}, {"î", 'i'}, {"ï", 'i'},
        {"ô", 'o'}, {"ö", 'o'}, {"ù", 'u'}, {"û", 'u'}, {"ü", 'u'}, {"ç", 'c'}, {"ÿ", 'y'}, {"œ", 'o'},
        {"À", 'a'}, {"Â", 'a'}, {"É", 'e'}, {"È", 'e'}, {"Ê", 'e'}, {"Î", 'i'}, {"Ô", 'o'}, {"Ù", 'u'}, {"Û", 'u'}, {"Ç", 'c'}};
    std::string out = " ";
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c >= 0x80) {
            char repl = ' ';
            size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
            for (const auto& [utf, base] : accents)
                if (in.compare(i, std::char_traits<char>::length(utf), utf) == 0) { repl = base; len = std::char_traits<char>::length(utf); break; }
            if (repl != ' ' || out.back() != ' ') out += repl;
            i += len;
            continue;
        }
        const char l = static_cast<char>(std::tolower(c));
        if (std::isalnum(c)) out += l;
        else if (out.back() != ' ') out += ' ';
        ++i;
    }
    if (out.back() != ' ') out += ' ';
    return out;
}

bool MixLexicon::Load(const std::filesystem::path& file) {
    entries_.clear();
    std::ifstream f(file, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool defect = line.rfind("defaut ", 0) == 0;
        if (!defect && line.rfind("qualite ", 0) != 0) continue;
        const auto colon = line.find(':'), arrow = line.find("=>");
        if (colon == std::string::npos || arrow == std::string::npos || arrow < colon) continue;
        std::string id = line.substr(defect ? 7 : 8, colon - (defect ? 7 : 8));
        id.erase(std::remove(id.begin(), id.end(), ' '), id.end());
        std::string axes = line.substr(arrow + 2);
        axes.erase(0, axes.find_first_not_of(' '));
        std::string list = line.substr(colon + 1, arrow - colon - 1);
        for (size_t p = 0; p <= list.size();) {
            const size_t bar = std::min(list.find('|', p), list.size());
            const std::string expr = Normalize(list.substr(p, bar - p));
            if (expr.size() > 2) {
                const int words = static_cast<int>(std::count(expr.begin(), expr.end(), ' ')) - 1;
                entries_.push_back({defect, id, expr, axes, words});
            }
            p = bar + 1;
        }
    }
    std::ranges::sort(entries_, [](const Entry& a, const Entry& b) { return a.expression.size() > b.expression.size(); });
    return !entries_.empty();
}

std::vector<LexiconHit> MixLexicon::Match(const std::string& text) const {
    std::string t = Normalize(text);
    std::vector<LexiconHit> hits;
    for (const auto& e : entries_) {
        const size_t p = t.find(e.expression);
        if (p == std::string::npos) continue;
        if (std::ranges::none_of(hits, [&](const LexiconHit& h) { return h.id == e.id; })) {
            std::string shown = e.expression.substr(1, e.expression.size() - 2);
            hits.push_back({e.defect, e.id, shown, e.axes, e.words});
        }
        t.replace(p + 1, e.expression.size() - 2, std::string(e.expression.size() - 2, '_'));  // pas de double lecture
    }
    return hits;
}

bool MixLexicon::IsMixRequest(const std::vector<LexiconHit>& hits, const std::string& text) {
    static const char* const elementIds[] = {"kick", "voix", "basse", "caisse_claire", "hats", "melodie", "aigus", "mediums", "bas_medium"};
    static const char* const elementWords[] = {" mix ", " son ", " prod ", " master ", " beat ", " instru ", " morceau ", " 808 ",
                                               " snare ", " clap ", " pad ", " lead ", " sample ", " piano ", " guitare ", " sub ",
                                               " basse ", " kick ", " voix ", " hats ", " drums ", " batterie ", " melo ", " melodie "};
    const std::string t = Normalize(text);
    const bool element = std::ranges::any_of(hits, [](const LexiconHit& h) {
                             return std::ranges::any_of(elementIds, [&](const char* id) { return h.id == id; });
                         }) || std::ranges::any_of(elementWords, [&](const char* w) { return t.find(w) != std::string::npos; });
    for (const auto& h : hits) {
        if (!h.defect) continue;
        if (element || h.words >= 2) return true;
    }
    return false;
}

std::vector<std::string> MixLexicon::Topics(const std::vector<LexiconHit>& hits) {
    static const std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> map = {
        {{"boom", "basse", "gras", "profond"}, {"sub", "lowmono", "mud"}},
        {{"boueux", "carton", "bas_medium"}, {"mud", "sub", "lowmono"}},
        {{"mou", "punch", "kick", "ecrase", "dynamique", "dense", "caisse_claire"}, {"loud", "clip", "sub"}},
        {{"distordu", "sale", "saturation"}, {"clip", "loud"}},
        {{"agressif", "siffle", "aigus", "hats", "brillant", "air", "crispy"}, {"harsh", "dull"}},
        {{"etouffe", "lointain", "voix", "sombre", "presence", "creux"}, {"dull", "hollow", "mud"}},
        {{"nasal", "mediums", "melodie"}, {"nasal", "hollow"}},
        {{"etroit", "large", "trop_large", "mono"}, {"phase", "lowmono", "narrow"}},
        {{"fort", "club", "streaming"}, {"loud", "clip", "quiet"}},
    };
    std::vector<std::string> topics;
    for (const auto& h : hits)
        for (const auto& [ids, t] : map)
            if (std::ranges::find(ids, h.id) != ids.end())
                for (const auto& x : t) if (std::ranges::find(topics, x) == topics.end()) topics.push_back(x);
    return topics;
}

std::string MixLexicon::Describe(const std::vector<LexiconHit>& hits) {
    if (hits.empty()) return {};
    std::string s = "[Demande comprise (lexique OMNIS)]";
    for (const auto& h : hits)
    {
        static const char* const elements[] = {"kick", "voix", "basse", "caisse_claire", "hats", "melodie"};
        const bool element = std::ranges::any_of(elements, [&](const char* e) { return h.id == e; });
        s += " « " + h.expression + " » → " + (h.defect ? "se plaint de : " + h.id : element ? "parle de : " + h.id : "veut : " + h.id) + " ;";
    }
    s.back() = '\n';
    return s;
}

}  // namespace companion
