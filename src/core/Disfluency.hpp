#pragma once
// Disfluency : parler comme quelqu'un qui réfléchit en parlant (demande de l'utilisateur : « qu'il bégaye un peu, se
// trompe un peu, fasse des pauses, reste sur la fin d'un mot »). Appliqué au TEXTE juste avant la voix :
//   - hésitation en début de réponse (« Euh, », « Bah… », « Attends, ») ;
//   - « euh » / « genre » / « tu vois » après une virgule ;
//   - petit bégaiement sur un mot court en tête de proposition (« je, je pense », « c'est… c'est ») ;
//   - auto-correction sur un mot long (« le compress… le compresseur ») ;
//   - mot qui traîne avant une pause (« alors… ») : les « … » sont aussi tenus côté voix (débit ralenti juste avant).
// Au plus 2 accidents par réponse ; jamais dans un nombre, une unité, une balise (son: …) / (rire) ou un mot en
// majuscules (sigles). Logique pure, testée hors Windows (tools/sim/disfltest.cpp).

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace companion {

namespace disfl_detail {
inline float Next(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 16777216.f;
}
struct Word {
    std::size_t begin, end;  // [begin, end) dans le texte
};
// Mots hors parenthèses ; ponctuation collée exclue.
inline std::vector<Word> Words(const std::string& t) {
    std::vector<Word> w;
    int depth = 0;
    std::size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (c == '(') ++depth;
        if (c == ')' && depth) --depth;
        const bool letter = std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80 || c == '\'';
        if (!depth && letter) {
            const std::size_t b = i;
            while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])) || static_cast<unsigned char>(t[i]) >= 0x80 ||
                                    t[i] == '\'' || t[i] == '-'))
                ++i;
            w.push_back({b, i});
            continue;
        }
        ++i;
    }
    return w;
}
inline bool Plain(const std::string& word) {  // pas de chiffre, pas un sigle
    int upper = 0;
    for (unsigned char c : word) {
        if (std::isdigit(c)) return false;
        upper += std::isupper(c) != 0;
    }
    return upper < 2;
}
}  // namespace disfl_detail

// level 0 … 1 (réglage « tics humains ») ; calm : réponse posée / technique (beaucoup moins d'accidents).
// lead : on peut ouvrir par une hésitation (seulement au début d'une réponse, pas à chaque morceau).
inline std::string Humanize(const std::string& text, float level, std::uint32_t& rng, bool calm = false, bool lead = true) {
    using namespace disfl_detail;
    if (level <= 0.f || text.size() < 12) return text;
    const float k = level * (calm ? 0.35f : 1.f);
    std::string t = text;
    int events = 0;
    const auto chance = [&](float p) { return events < 2 && Next(rng) < p * k; };

    // 1) Mot long qui part de travers puis se corrige (« le compress… le compresseur »).
    if (chance(0.22f)) {
        const auto w = Words(t);
        std::vector<std::size_t> longOnes;
        for (std::size_t i = 1; i < w.size(); ++i) {
            const std::string s = t.substr(w[i].begin, w[i].end - w[i].begin);
            if (s.size() >= 7 && Plain(s) && s.find('\'') == std::string::npos) longOnes.push_back(i);
        }
        if (!longOnes.empty()) {
            const auto& wd = w[longOnes[static_cast<std::size_t>(Next(rng) * static_cast<float>(longOnes.size())) % longOnes.size()]];
            const std::string word = t.substr(wd.begin, wd.end - wd.begin);
            std::size_t cut = word.size() * 6 / 10;
            while (cut > 3 && (static_cast<unsigned char>(word[cut]) & 0xC0) == 0x80) --cut;  // pas au milieu d'un accent
            t.insert(wd.begin, word.substr(0, cut) + "… ");
            ++events;
        }
    }
    // 2) Bégaiement sur un mot court en tête de proposition (« je, je pense », « c'est… c'est »).
    if (chance(0.3f)) {
        static const char* kShort[] = {"je", "j'", "c'est", "t'as", "tu", "on", "mais", "et", "ça", "il", "y'a", "t'es", "le", "la"};
        const auto w = Words(t);
        bool done = false;
        for (std::size_t i = 0; i < w.size() && !done; ++i) {
            const bool head = i == 0 || t.find_first_of(",.!?", w[i - 1].end) < w[i].begin;
            if (!head) continue;
            std::string s = t.substr(w[i].begin, w[i].end - w[i].begin);
            std::string lower;
            for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const char* sh : kShort) {
                const std::string_view v(sh);
                if (lower == v || (v.back() == '\'' && lower.rfind(v, 0) == 0)) {
                    const std::string rep = v.back() == '\'' ? s.substr(0, v.size() - 1) + "e" : s;  // « j'pense » -> « je, j'pense »
                    t.insert(w[i].begin, rep + (Next(rng) < 0.5f ? ", " : "… "));
                    ++events;
                    done = true;
                    break;
                }
            }
        }
    }
    // 3) « euh » / « genre » / « tu vois » après une virgule.
    if (chance(0.35f)) {
        static const char* kFill[] = {" euh,", " genre,", " tu vois,", " en fait,", " euh…"};
        for (std::size_t p = t.find(','); p != std::string::npos; p = t.find(',', p + 1)) {
            if (p + 1 < t.size() && t.compare(p + 1, 4, " euh") != 0 && Next(rng) < 0.6f) {
                t.insert(p + 1, kFill[static_cast<std::size_t>(Next(rng) * 5.f) % 5]);
                ++events;
                break;
            }
        }
    }
    // 4) Hésitation en tête de réponse, ou mot qui traîne (« alors… »).
    if (lead && chance(0.3f)) {
        static const char* kLead[] = {"Euh, ", "Bah… ", "Attends, ", "Hmm, ", "Alors… ", "Ben… "};
        std::string lead = kLead[static_cast<std::size_t>(Next(rng) * 6.f) % 6];
        if (!t.empty() && std::isupper(static_cast<unsigned char>(t[0])) && t.size() > 1 && !std::isupper(static_cast<unsigned char>(t[1])))
            t[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
        t = lead + t;
        ++events;
    }
    return t;
}

}  // namespace companion
