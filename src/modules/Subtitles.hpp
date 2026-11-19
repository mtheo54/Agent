#pragma once
// Subtitles : la voix off d'un montage, mot par mot (Whisper), pour deux gestes de monteur pro :
//   - sous-titres façon YouTube / TikTok : 1 à 3 mots à l'écran, calés sur la parole, le mot fort seul et en couleur ;
//   - synchro : « fais apparaître ABONNE-TOI quand je le dis » -> l'instant exact où le mot est prononcé.
// Logique pure (testable) : découpe, mots forts, recherche d'un mot, corps du script After Effects.

#include "core/TextFr.hpp"
#include "modules/AeScript.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

struct TimedWord {
    std::string text;   // tel que transcrit (ponctuation comprise)
    double start = 0, end = 0;  // secondes, temps de la COMPOSITION
};

struct SubChunk {
    std::string text;
    double start = 0, end = 0;
    bool key = false;  // mot fort : seul à l'écran, plus gros, en couleur
};

namespace subs_detail {
// Minuscules sans accents ni ponctuation (« Abonne-toi ! » -> « abonne toi »).
inline std::string Norm(std::string_view s) {
    std::string l = LowerFr(s), o;
    for (size_t i = 0; i < l.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(l[i]);
        if (c == 0xC3 && i + 1 < l.size()) {  // lettres accentuées (UTF-8) ramenées à la lettre de base
            const unsigned char d = static_cast<unsigned char>(l[++i]);
            if (d >= 0xA0 && d <= 0xA5) o += 'a';
            else if (d == 0xA7) o += 'c';
            else if (d >= 0xA8 && d <= 0xAB) o += 'e';
            else if (d >= 0xAC && d <= 0xAF) o += 'i';
            else if (d >= 0xB2 && d <= 0xB6) o += 'o';
            else if (d >= 0xB9 && d <= 0xBC) o += 'u';
            continue;
        }
        if (std::isalnum(c)) o += static_cast<char>(c);
        else if (!o.empty() && o.back() != ' ') o += ' ';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

inline size_t Letters(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80 && (std::isalnum(c) || c >= 0xC0);
    return n;
}

inline bool EndsPhrase(std::string_view w) {
    while (!w.empty() && w.back() == ' ') w.remove_suffix(1);
    return !w.empty() && std::string_view(".!?,;:").find(w.back()) != std::string_view::npos;
}
}  // namespace subs_detail

// Mot fort : un nombre, un mot en capitales, ou un mot long qui n'est pas un mot outil.
inline bool IsKeyWord(std::string_view raw) {
    const std::string n = subs_detail::Norm(raw);
    if (n.empty()) return false;
    if (std::any_of(n.begin(), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return true;
    bool caps = subs_detail::Letters(raw) >= 3;
    for (unsigned char c : raw)
        if (std::islower(c)) caps = false;
    if (caps) return true;
    static const char* kTool[] = {"aujourd", "maintenant", "toujours", "vraiment", "comment", "pourquoi", "beaucoup", "quelque",
                                  "quelques", "pendant", "parce", "lorsque", "simplement", "justement", "exactement", "seulement",
                                  "derriere", "ensuite", "plusieurs", "certains", "certaines", "tellement", "absolument", "carrement"};
    for (const char* t : kTool)
        if (n.starts_with(t)) return false;
    return subs_detail::Letters(n) >= 8;
}

// 1 à maxWords mots par sous-titre (maxChars au plus) ; coupure aux ponctuations et aux pauses ; mot fort seul.
inline std::vector<SubChunk> ChunkWords(const std::vector<TimedWord>& words, int maxWords = 3, size_t maxChars = 18) {
    std::vector<SubChunk> out;
    SubChunk cur;
    int n = 0;
    const auto flush = [&] {
        if (n) out.push_back(cur);
        cur = {};
        n = 0;
    };
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& w = words[i];
        std::string t = w.text;
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.empty()) continue;
        if (subs_detail::Letters(t) == 0) {  // « ? » ou « ! » transcrit à part : collé au mot d'avant
            if (t.front() == '?' || t.front() == '!') t = " " + t;  // typographie française : « planté ? »
            if (n) {
                cur.text += t;
                if (subs_detail::EndsPhrase(t)) flush();
            } else if (!out.empty()) {
                out.back().text += t;
            }
            continue;
        }
        if (IsKeyWord(t)) {
            // Petit mot juste avant (« le », « de », « un ») : il part avec le mot fort plutôt que de rester seul.
            if (n == 1 && subs_detail::Letters(cur.text) <= 3) {
                out.push_back({cur.text + " " + t, cur.start, w.end, true});
                cur = {};
                n = 0;
                continue;
            }
            flush();
            out.push_back({t, w.start, w.end, true});
            continue;
        }
        const bool pause = n && w.start - cur.end > 0.35;
        if (n && (n >= maxWords || pause || cur.text.size() + 1 + t.size() > maxChars)) flush();
        if (!n) cur.start = w.start;
        cur.text += (n ? " " : "") + t;
        cur.end = w.end;
        ++n;
        if (subs_detail::EndsPhrase(t)) flush();
    }
    flush();
    // Pas de virgule ni de point en fin de sous-titre (style pro) ; « ? » et « ! » restent.
    for (auto& c : out)
        while (!c.text.empty() && std::string_view(",.;:").find(c.text.back()) != std::string_view::npos) c.text.pop_back();
    std::erase_if(out, [](const SubChunk& c) { return subs_detail::Letters(c.text) == 0; });
    // Un sous-titre reste au moins 0,25 s et jusqu'au suivant s'il est proche (pas de clignotement).
    for (size_t i = 0; i < out.size(); ++i) {
        auto& c = out[i];
        if (i + 1 < out.size() && out[i + 1].start - c.end < 0.3) c.end = out[i + 1].start;
        c.end = std::max(c.end, c.start + 0.25);
        if (i + 1 < out.size()) c.end = std::min(c.end, out[i + 1].start);
    }
    return out;
}

// Instant où la phrase est prononcée (début du premier mot) : mots exacts d'abord, sinon début de mot (≥ 4 lettres).
inline std::optional<double> FindWordTime(const std::vector<TimedWord>& words, std::string_view phrase) {
    std::vector<std::string> want;
    {
        const std::string n = subs_detail::Norm(phrase);
        size_t p = 0;
        while (p < n.size()) {
            const size_t q = n.find(' ', p);
            want.push_back(n.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    if (want.empty()) return std::nullopt;
    // Les mots transcrits, éclatés (« abonne-toi » = « abonne » + « toi »), avec l'instant de leur mot d'origine.
    std::vector<std::pair<std::string, double>> flat;
    for (const auto& w : words) {
        const std::string n = subs_detail::Norm(w.text);
        size_t p = 0;
        while (p < n.size()) {
            const size_t q = n.find(' ', p);
            flat.emplace_back(n.substr(p, q == std::string::npos ? std::string::npos : q - p), w.start);
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    for (size_t i = 0; i + want.size() <= flat.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < want.size() && ok; ++k) ok = flat[i + k].first == want[k];
        if (ok) return flat[i].second;
    }
    const std::string& first = *std::max_element(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.size() < b.size(); });
    if (first.size() >= 4)
        for (const auto& [w, t] : flat)
            if (w.size() >= 4 && (w.starts_with(first.substr(0, std::min<size_t>(first.size(), 6))) ||
                                  first.starts_with(w.substr(0, std::min<size_t>(w.size(), 6)))))
                return t;
    return std::nullopt;
}

// Corps du script : un calque texte par sous-titre (omnisSub, défini dans l'enveloppe), sur la piste du bas.
inline std::string SubtitleBody(const std::vector<SubChunk>& chunks, const std::string& color = "[1, 1, 1]",
                                const std::string& keyColor = "[1, 0.85, 0.1]") {
    std::string b = "var C = " + color + ", K = " + keyColor + ";\n";
    char buf[64];
    for (const auto& c : chunks) {
        std::snprintf(buf, sizeof buf, ", %.3f, %.3f, %s);\n", c.start, c.end, c.key ? "true" : "false");
        b += "omnisSub(" + AeQuote(c.text) + buf;
    }
    std::snprintf(buf, sizeof buf, "%zu", chunks.size());
    return b + "rapport = \"" + buf + " sous-titres posés, les mots forts en couleur.\";";
}

// Table des mots pour les scripts libres (omnisWhen) : [["mot", début], …].
inline std::string WordsTable(const std::vector<TimedWord>& words) {
    std::string t = "var omnisWordsT = [";
    char buf[32];
    for (size_t i = 0; i < words.size(); ++i) {
        std::snprintf(buf, sizeof buf, ", %.3f]", words[i].start);
        t += (i ? ", [" : "[") + AeQuote(subs_detail::Norm(words[i].text)) + buf;
    }
    return t + "];\n";
}

}  // namespace companion
