#pragma once
// SpeechFix : la banque de mots d'Omnis et son autocorrecteur de transcription.
//
//  - Banque (assets/knowledge/vocab.json + tes mots appris) : jeux, musique, montage, électrique, PC, argot.
//  - Whisper reçoit le vocabulaire du moment (domaine actif) et ta dernière phrase : il écrit juste du premier coup.
//  - Après Whisper :
//      1) les corrections connues (« fête studio » -> « FL Studio », « Azio » -> « ASIO »), et celles que tu lui apprends
//         (« quand je dis X, c'est Y ») ;
//      2) le phonétique : un mot qui SONNE comme un mot de la banque est remplacé (« Jinks » -> « Jinx »).
//         Garde-fou : les mots courants du français (liste des 40 000 plus fréquents, sous-titres de films) ne sont jamais
//         remplacés, sauf mots rares dans le domaine actif : une phrase normale reste intacte.
// Thread-safe (lu par le thread de la voix, enrichi par celui de l'orchestrateur).

#include "core/TextFr.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace companion {

// Clé phonétique (français, simplifiée) : « Azio » et « ASIO » -> « asio », « Jinks » et « Jinx » -> « jinks ».
inline std::string PhonKey(std::string_view in) {
    // 1) minuscules ASCII sans accents ; ç -> s ; tout sauf lettres et chiffres retiré
    std::string a;
    const std::string l = LowerFr(in);
    for (size_t i = 0; i < l.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(l[i]);
        if (c == 0xC3 && i + 1 < l.size()) {
            const unsigned char d = static_cast<unsigned char>(l[++i]);
            a += d == 0xA7 ? 's' : d <= 0xA5 ? 'a' : d <= 0xAB ? 'e' : d <= 0xAF ? 'i' : d <= 0xB6 ? 'o' : d <= 0xBC ? 'u' : 'y';
        } else if (std::isalnum(c)) {
            a += static_cast<char>(c);
        }
    }
    const auto rep = [&](std::string_view from, std::string_view to) {
        for (size_t p = a.find(from); p != std::string::npos; p = a.find(from, p + to.size())) a.replace(p, from.size(), to);
    };
    for (auto [f, t] : {std::pair{"eaux", "o"}, {"eau", "o"}, {"aux", "o"}, {"au", "o"}, {"ph", "f"}, {"qu", "k"}, {"ck", "k"},
                        {"sch", "S"}, {"ch", "S"}, {"sh", "S"}, {"gn", "N"}, {"ou", "u"}, {"oo", "u"}, {"oi", "wa"}, {"ai", "e"},
                        {"ei", "e"}, {"ay", "e"}, {"ee", "i"}, {"th", "t"}, {"tion", "sion"}})
        rep(f, t);
    // 2) voyelles nasales (devant consonne ou en fin de mot)
    const auto vowel = [](char c) { return std::string_view("aeiouy").find(c) != std::string_view::npos; };
    std::string b;
    for (size_t i = 0; i < a.size(); ++i) {
        const char c = a[i], n = i + 1 < a.size() ? a[i + 1] : '\0', n2 = i + 2 < a.size() ? a[i + 2] : '\0';
        if ((c == 'a' || c == 'e') && (n == 'n' || n == 'm') && !vowel(n2) && n2 != n) { b += 'A'; ++i; continue; }
        if (c == 'o' && (n == 'n' || n == 'm') && !vowel(n2) && n2 != n) { b += 'O'; ++i; continue; }
        if ((c == 'i' || c == 'u' || c == 'y') && (n == 'n' || n == 'm') && !vowel(n2) && n2 != n) { b += 'I'; ++i; continue; }
        b += c;
    }
    // 3) consonnes qui se prononcent pareil
    std::string k;
    for (size_t i = 0; i < b.size(); ++i) {
        const char c = b[i], n = i + 1 < b.size() ? b[i + 1] : '\0';
        if (c == 'c') k += (n == 'e' || n == 'i' || n == 'y') ? 's' : 'k';
        else if (c == 'g') k += (n == 'e' || n == 'i' || n == 'y') ? 'j' : 'g';
        else if (c == 'y') k += 'i';
        else if (c == 'z') k += 's';
        else if (c == 'x') k += "ks";
        else if (c == 'w') k += 'v';
        else if (c == 'h') continue;
        else k += c;
    }
    // 4) fins muettes et lettres doublées
    if (k.size() > 3 && (k.ends_with("er") || k.ends_with("ez") || k.ends_with("et"))) k.replace(k.size() - 2, 2, "e");
    if (k.size() > 2 && k.back() == 'e') k.pop_back();
    if (k.size() > 2 && std::string_view("tdspks").find(k.back()) != std::string_view::npos && !std::isdigit(static_cast<unsigned char>(k[k.size() - 2])))
        if (k.back() != 's' || k[k.size() - 2] != 'k') k.pop_back();  // « x » final de « Jinx » (ks) gardé
    std::string out;
    for (char c : k)
        if (out.empty() || out.back() != c) out += c;
    return out;
}

inline int EditDistance(std::string_view a, std::string_view b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

class SpeechFix {
public:
    struct Change {
        std::string from, to;
    };

    bool Load(const std::filesystem::path& vocabJson, const std::filesystem::path& frequentWords, const std::filesystem::path& userJson) {
        std::scoped_lock lock(mutex_);
        userFile_ = userJson;
        domains_.clear();
        fixes_.clear();
        try {
            std::ifstream f(vocabJson, std::ios::binary);
            const auto j = nlohmann::json::parse(f);
            for (const auto& [dom, words] : j.at("domains").items())
                for (const auto& w : words) domains_[dom].push_back(w.get<std::string>());
            for (const auto& [from, to] : j.at("fixes").items()) fixes_[LowerFr(from)] = to.get<std::string>();
        } catch (...) {
            return false;
        }
        {
            std::ifstream f(frequentWords, std::ios::binary);
            std::string w;
            int rank = 0;
            while (std::getline(f, w)) {
                while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
                if (!w.empty()) rank_.emplace(LowerFr(w), ++rank);
            }
        }
        LoadUser();
        Rebuild();
        return true;
    }

    // Domaines actifs (« musique », « jeu », « montage », « electrique ») ; « pc » et « argot » le sont toujours.
    void SetDomains(std::vector<std::string> d) {
        std::scoped_lock lock(mutex_);
        active_ = std::move(d);
    }

    // Consigne pour Whisper : le vocabulaire du moment, tes mots, et ta dernière phrase (le contexte aide à deviner).
    std::string Prompt(const std::string& lastUserText) const {
        std::scoped_lock lock(mutex_);
        std::string p = "Omnis, ";
        size_t budget = 520;
        const auto add = [&](const std::string& w) {
            if (p.size() + w.size() + 2 > budget) return;
            p += w + ", ";
        };
        for (const auto& w : userWords_) add(w);
        for (const auto& d : active_)
            if (const auto it = domains_.find(d); it != domains_.end())
                for (size_t i = 0; i < it->second.size() && i < 45; ++i) add(it->second[i]);
        for (const char* d : {"pc", "argot"})
            if (const auto it = domains_.find(d); it != domains_.end())
                for (size_t i = 0; i < it->second.size() && i < 12; ++i) add(it->second[i]);
        if (p.size() > 2) p.replace(p.size() - 2, 2, ". ");
        if (!lastUserText.empty()) p += lastUserText.substr(0, 160);
        return p;
    }

    // Corrige une transcription ; changes reçoit ce qui a été remplacé (pour le journal).
    std::string Fix(const std::string& text, std::vector<Change>* changes = nullptr) const {
        std::scoped_lock lock(mutex_);
        std::string s = text;
        // 1) corrections explicites (bornes de mot), de la plus longue à la plus courte
        for (const auto& [from, to] : fixesByLength_) {
            for (size_t p = 0;;) {
                const std::string low = LowerFr(s);
                p = low.find(from, p);
                if (p == std::string::npos) break;
                const bool startOk = p == 0 || !IsWordByte(low[p - 1]) || low[p - 1] == '\'';  // « d'Otte » : après une élision
                const bool endOk = p + from.size() >= low.size() || !IsWordByte(low[p + from.size()]);
                if (startOk && endOk && s.compare(p, from.size(), to) != 0) {
                    const std::string rep = KeepCase(s.substr(p, from.size()), to);
                    if (changes) changes->push_back({s.substr(p, from.size()), rep});
                    s.replace(p, from.size(), rep);
                    p += rep.size();
                } else {
                    p += from.size();
                }
            }
        }
        // 2) phonétique, mot par mot et par groupes de 2 ou 3 mots
        std::vector<Token> toks = Tokenize(s);
        std::string out;
        for (size_t i = 0; i < toks.size();) {
            bool done = false;
            for (size_t n = std::min<size_t>(3, toks.size() - i); n >= 1 && !done; --n) {
                std::string span, key;
                bool anyRare = false, allBlank = true, anyBlank = false;
                int minRank = 1 << 30;
                for (size_t k = 0; k < n; ++k) {
                    span += (k ? " " : "") + toks[i + k].core;
                    key += PhonKey(toks[i + k].core);
                    const int r = Rank(toks[i + k].core);
                    minRank = std::min(minRank, r);
                    anyRare = anyRare || r == 0;
                    allBlank = allBlank && toks[i + k].core.empty();
                    anyBlank = anyBlank || toks[i + k].core.empty();
                }
                // Un « mot » vide (ponctuation seule, ex. « ? ») ne fait jamais partie d'un groupe : il serait perdu.
                if (allBlank || anyBlank || key.size() < 3) continue;
                const Entry* e = Lookup(key, anyRare, minRank);
                if (!e || LowerFr(e->display) == LowerFr(span)) continue;
                if (allWords_.contains(LowerFr(span))) continue;  // déjà un mot de la banque (« lead ») : on n'y touche pas
                // Autant de mots avant qu'après (« le lead » ne devient jamais « release », « un side chain » garde « un »).
                if (static_cast<size_t>(std::count(e->display.begin(), e->display.end(), ' ')) + 1 != n) continue;
                if (n > 1 && minRank > 0 && minRank <= 3000 && !anyRare) continue;  // « de la » + … : groupes de mots courants intouchables
                const std::string rep = KeepCase(span, e->display);
                if (changes) changes->push_back({span, rep});
                out += toks[i].lead + rep + toks[i + n - 1].trail;
                i += n;
                done = true;
            }
            if (!done) {
                out += toks[i].lead + toks[i].core + toks[i].trail;
                ++i;
            }
        }
        return out;
    }

    // Apprentissage : « quand je dis X, c'est Y » ; « retiens le mot Z » ; « oublie le mot Z ».
    void AddFix(const std::string& from, const std::string& to) {
        std::scoped_lock lock(mutex_);
        userFixes_[LowerFr(from)] = to;
        SaveUser();
        Rebuild();
    }
    void AddWord(const std::string& w) {
        std::scoped_lock lock(mutex_);
        if (std::find(userWords_.begin(), userWords_.end(), w) == userWords_.end()) userWords_.push_back(w);
        SaveUser();
        Rebuild();
    }
    bool Forget(const std::string& w) {
        std::scoped_lock lock(mutex_);
        const std::string l = LowerFr(w);
        const auto before = userWords_.size() + userFixes_.size();
        std::erase_if(userWords_, [&](const std::string& x) { return LowerFr(x) == l; });
        std::erase_if(userFixes_, [&](const auto& kv) { return kv.first == l || LowerFr(kv.second) == l; });
        const bool removed = before != userWords_.size() + userFixes_.size();
        SaveUser();
        Rebuild();
        return removed;
    }
    [[nodiscard]] size_t Size() const {
        std::scoped_lock lock(mutex_);
        return byKey_.size();
    }

private:
    struct Entry {
        std::string display, domain;
        bool core = false;  // mots essentiels du domaine (début de liste) ; les grandes listes ne corrigent que des mots rares
    };
    struct Token {
        std::string lead, core, trail;
    };

    // « Mod d'entraînement » -> « Mode d'entraînement » : la majuscule du début est gardée.
    static std::string KeepCase(const std::string& original, std::string replacement) {
        if (!original.empty() && std::isupper(static_cast<unsigned char>(original[0])) && !replacement.empty() &&
            std::islower(static_cast<unsigned char>(replacement[0])))
            replacement[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(replacement[0])));
        return replacement;
    }

    static bool IsWordByte(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return std::isalnum(u) || u >= 0x80 || c == '\'' || c == '-';
    }

    static std::vector<Token> Tokenize(const std::string& s) {
        std::vector<Token> t;
        size_t i = 0;
        while (i < s.size()) {
            Token k;
            while (i < s.size() && !IsWordByte(s[i])) k.lead += s[i++];
            while (i < s.size() && IsWordByte(s[i])) k.core += s[i++];
            while (i < s.size() && !IsWordByte(s[i]) && s[i] != ' ') k.trail += s[i++];
            while (i < s.size() && s[i] == ' ') k.trail += s[i++];
            if (!k.lead.empty() || !k.core.empty() || !k.trail.empty()) t.push_back(std::move(k));
        }
        return t;
    }

    // Rang du mot parmi les plus fréquents (1 = le plus courant), 0 = mot rare / inconnu.
    int Rank(const std::string& w) const {
        std::string l = LowerFr(w);
        if (const auto p = l.find('\''); p != std::string::npos && p + 1 < l.size()) l = l.substr(p + 1);  // « l'imiteur »
        const auto it = rank_.find(l);
        return it == rank_.end() ? 0 : it->second;
    }

    bool Active(const std::string& domain) const {
        return domain == "pc" || domain == "perso" || std::find(active_.begin(), active_.end(), domain) != active_.end();
    }

    // Mot de la banque qui sonne pareil. Entre plusieurs qui sonnent pareil : celui du domaine actif, sinon rien (ambigu).
    // Mot courant (dans la liste des 40 000) : seulement un mot essentiel du domaine actif, et jamais les 8 000 plus courants.
    const Entry* Lookup(const std::string& key, bool anyRare, int minRank) const {
        const auto pick = [&](const std::vector<const Entry*>& c) -> const Entry* {
            const Entry* best = nullptr;
            for (const Entry* e : c)
                if (best && LowerFr(best->display) != LowerFr(e->display)) return nullptr;  // deux mots différents : ambigu
                else best = e;
            return best;
        };
        if (const auto it = byKey_.find(key); it != byKey_.end()) {
            std::vector<const Entry*> active, other;
            for (const Entry& e : it->second) (Active(e.domain) ? active : other).push_back(&e);
            if (anyRare) return pick(active.empty() ? other : active);
            if (minRank <= 8000) return nullptr;
            std::vector<const Entry*> core;
            for (const Entry* e : active)
                if (e->core) core.push_back(e);
            return pick(core);
        }
        // Une lettre d'écart (« Jinks » ~ « Jinx »), seulement pour un mot rare et une clé assez longue.
        if (!anyRare || key.size() < 5) return nullptr;
        std::vector<const Entry*> closeOnes;  // (pas « near » : macro des en-têtes Windows)
        for (const auto& [k, es] : byKey_)
            if (k.size() >= 5 && std::abs(static_cast<int>(k.size()) - static_cast<int>(key.size())) <= 1 && EditDistance(k, key) == 1)
                for (const Entry& e : es)
                    if (Active(e.domain)) closeOnes.push_back(&e);
        return pick(closeOnes);
    }

    void Rebuild() {
        byKey_.clear();
        allWords_.clear();
        for (const auto& [dom, words] : domains_) {
            for (const auto& w : words) allWords_.insert(LowerFr(w));
            if (dom == "argot") continue;  // l'argot sert au prompt, pas à remplacer
            for (size_t i = 0; i < words.size(); ++i)
                if (const std::string k = PhonKey(words[i]); k.size() >= 3) byKey_[k].push_back(Entry{words[i], dom, i < kCoreWords});
        }
        for (const auto& w : userWords_) {
            allWords_.insert(LowerFr(w));
            if (const std::string k = PhonKey(w); k.size() >= 3) byKey_[k] = {Entry{w, "perso", true}};  // tes mots passent avant tout
        }
        fixesByLength_.assign(fixes_.begin(), fixes_.end());
        for (const auto& kv : userFixes_) fixesByLength_.push_back(kv);
        std::sort(fixesByLength_.begin(), fixesByLength_.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }

    void LoadUser() {
        userWords_.clear();
        userFixes_.clear();
        std::ifstream f(userFile_, std::ios::binary);
        if (!f) return;
        try {
            const auto j = nlohmann::json::parse(f);
            for (const auto& w : j.value("words", nlohmann::json::array())) userWords_.push_back(w.get<std::string>());
            // (Copie locale : « j.value(...).items() » itérerait sur un temporaire déjà détruit.)
            const nlohmann::json fixes = j.value("fixes", nlohmann::json::object());
            for (const auto& [from, to] : fixes.items()) userFixes_[from] = to.get<std::string>();
        } catch (...) {
            // fichier abîmé : on repart de zéro plutôt que de planter
        }
    }
    void SaveUser() const {
        if (userFile_.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(userFile_.parent_path(), ec);
        std::ofstream(userFile_, std::ios::binary | std::ios::trunc) << nlohmann::json{{"words", userWords_}, {"fixes", userFixes_}}.dump(1);
    }

    mutable std::mutex mutex_;
    std::filesystem::path userFile_;
    std::map<std::string, std::vector<std::string>> domains_;
    std::map<std::string, std::string> fixes_, userFixes_;
    std::vector<std::pair<std::string, std::string>> fixesByLength_;
    std::vector<std::string> userWords_, active_;
    std::unordered_map<std::string, int> rank_;
    std::unordered_map<std::string, std::vector<Entry>> byKey_;
    std::set<std::string> allWords_;
    static constexpr size_t kCoreWords = 420;  // début de chaque liste : les mots écrits à la main pour ce domaine
};

}  // namespace companion
