#pragma once
// Skills : les compétences qu'Omnis se crée lui-même, à ta demande et avec ton accord (« apprends à faire X »).
//
// Une compétence n'est PAS du code natif (un programme qui se recompile pourrait casser ton PC) : c'est un petit fichier
// JSON dans %LOCALAPPDATA%\CompanionAI\skills, écrit avec les briques déjà sûres d'Omnis, et toujours exécuté par elles :
//   - « savoir »  : la méthode de pro d'un nouveau domaine (Blender, Photoshop, Ableton…), utilisée comme les autres
//                   méthodes dès qu'une question tombe dans ce domaine ;
//   - « action »  : un geste qu'on lui apprend (« exporte en MP3 et ouvre le dossier »), joué au clavier et à la souris
//                   (plan d'actions du pilote, actions risquées toujours confirmées à voix haute) ou, dans After Effects,
//                   un script ExtendScript (contrôle de sécurité, un seul Ctrl+Z, vérification du résultat).
// Chaque création est lue à voix haute et attend TON oui (jamais retenu en « toujours ») ; « oublie la compétence X ».

#include "core/Playbooks.hpp"
#include "core/TextFr.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace companion {

struct Skill {
    std::string name;      // « exporter en mp3 »
    std::string kind;      // "savoir" ou "action"
    std::string app;       // action : "fl studio", "after effects", "autocad", "tout"
    std::string what;      // ce que ça fait, dit à voix haute avant l'accord
    std::vector<std::string> triggers;  // phrases (minuscules) qui la lancent
    std::vector<std::string> keywords;  // savoir : mots du domaine
    std::string method;    // savoir : la méthode
    std::string steps;     // action : plan du pilote (DIRE / TOUCHE / TAPE / CLIC / COMMANDE / ATTENDS)
    std::string aeScript;  // action dans After Effects : corps du script
};

inline std::string SkillSlug(std::string_view name) {
    std::string s;
    const std::string l = LowerFr(name);
    for (size_t i = 0; i < l.size(); ++i) {
        const unsigned char u = static_cast<unsigned char>(l[i]);
        if (u == 0xC3 && i + 1 < l.size()) {  // lettre accentuée -> lettre de base (« néon » -> « neon »)
            const unsigned char d = static_cast<unsigned char>(l[++i]);
            s += d <= 0xA5 ? 'a' : d == 0xA7 ? 'c' : d <= 0xAB ? 'e' : d <= 0xAF ? 'i' : d <= 0xB6 ? 'o' : 'u';
            continue;
        }
        if (std::isalnum(u)) s += static_cast<char>(u);
        else if (u >= 0x80) continue;
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "competence" : s.substr(0, 60);
}

inline nlohmann::json ToJson(const Skill& k) {
    return {{"name", k.name}, {"kind", k.kind}, {"app", k.app}, {"what", k.what}, {"triggers", k.triggers},
            {"keywords", k.keywords}, {"method", k.method}, {"steps", k.steps}, {"ae_script", k.aeScript}};
}

inline std::optional<Skill> SkillFromJson(const nlohmann::json& j) {
    try {
        Skill k;
        k.name = j.value("name", std::string{});
        k.kind = j.value("kind", std::string{});
        k.app = j.value("app", std::string{"tout"});
        k.what = j.value("what", std::string{});
        for (const auto& t : j.value("triggers", nlohmann::json::array())) k.triggers.push_back(LowerFr(t.get<std::string>()));
        for (const auto& t : j.value("keywords", nlohmann::json::array())) k.keywords.push_back(LowerFr(t.get<std::string>()));
        k.method = j.value("method", std::string{});
        k.steps = j.value("steps", std::string{});
        k.aeScript = j.value("ae_script", std::string{});
        if (k.name.empty() || (k.kind != "savoir" && k.kind != "action")) return std::nullopt;
        if (k.kind == "savoir" && (k.method.empty() || k.keywords.empty())) return std::nullopt;
        if (k.kind == "action" && k.steps.empty() && k.aeScript.empty()) return std::nullopt;
        return k;
    } catch (...) {
        return std::nullopt;
    }
}

inline std::vector<Skill> LoadSkills(const std::filesystem::path& dir) {
    std::vector<Skill> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != L".json") continue;
        std::ifstream f(e.path(), std::ios::binary);
        try {
            if (auto k = SkillFromJson(nlohmann::json::parse(f))) out.push_back(std::move(*k));
        } catch (...) {
        }
    }
    return out;
}

inline bool SaveSkill(const std::filesystem::path& dir, const Skill& k) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / (SkillSlug(k.name) + ".json"), std::ios::binary | std::ios::trunc);
    f << ToJson(k).dump(1);
    return static_cast<bool>(f);
}

inline bool RemoveSkill(const std::filesystem::path& dir, std::string_view name) {
    std::error_code ec;
    return std::filesystem::remove(dir / (SkillSlug(name) + ".json"), ec);
}

// Compétence d'action déclenchée par la phrase : déclencheur entier, ou tous ses mots importants (≥ 4 lettres, comparés
// sur leurs 4 premières lettres : « exporte en mp3 » lance « exporter en mp3 »). Le déclencheur le plus long gagne.
inline const Skill* MatchSkill(const std::vector<Skill>& skills, const std::string& lower) {
    const Skill* best = nullptr;
    size_t bestLen = 0;
    const auto loose = [&](const std::string& t) {
        int words = 0;
        for (size_t p = 0; p < t.size();) {
            size_t q = t.find(' ', p);
            if (q == std::string::npos) q = t.size();
            const std::string w = t.substr(p, q - p);
            p = q + 1;
            static const char* kVerbs[] = {"faire", "fais", "mettre", "mets", "créer", "crée", "creer", "cree", "ajouter",
                                           "ajoute", "lancer", "lance", "rajouter", "rajoute"};
            if (std::any_of(std::begin(kVerbs), std::end(kVerbs), [&](const char* v) { return w == v; })) continue;  // verbe : forme libre
            if (w.size() < 4 && !std::any_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
            if (lower.find(w.substr(0, std::min<size_t>(w.size(), 4))) == std::string::npos) return false;
            ++words;
        }
        return words >= 2;
    };
    for (const auto& k : skills) {
        if (k.kind != "action") continue;
        for (const auto& t : k.triggers)
            if (t.size() >= 6 && t.size() > bestLen && (lower.find(t) != std::string::npos || loose(t))) {
                best = &k;
                bestLen = t.size();
            }
    }
    return best;
}

// Les compétences de savoir deviennent des méthodes de pro (détectées comme les autres).
inline Playbook AsPlaybook(const Skill& k) {
    return Playbook{"perso:" + SkillSlug(k.name), k.name + " (compétence apprise)", k.method, k.keywords};
}

// --- Ce que le modèle écrit pour une nouvelle compétence (format strict, lu ici) -------------------------------------

// Étape 1 : quel genre ? (grammaire)
inline constexpr const char* kSkillKindGrammar = R"(root ::= "SAVOIR" | "ACTION" | "IMPOSSIBLE")";
inline constexpr const char* kSkillKindSystem =
    "On demande à Omnis, une IA sur le PC, d'apprendre une nouvelle compétence. Réponds par un seul mot :\n"
    "SAVOIR = connaître un domaine, un logiciel, une méthode (« apprends Blender », « deviens bon en Photoshop », "
    "« apprends le mixage de podcast ») ;\n"
    "ACTION = un geste à refaire sur demande dans un logiciel (« apprends à exporter en MP3 », « apprends à faire un "
    "titre néon dans After Effects ») ;\n"
    "IMPOSSIBLE = toucher aux comptes, messages ou réseaux sociaux de quelqu'un, payer, pirater, tricher en jeu, "
    "contourner une sécurité, ou rien de précis.";

// Étape 2 (savoir) : NOM / MOTS / METHODE.
inline constexpr const char* kSkillKnowledgeSystem =
    "Tu écris la méthode de travail d'un professionnel pour le domaine demandé, en français, pour qu'une IA s'en serve "
    "en conseillant à l'oral. Format EXACT :\n"
    "NOM: <nom court du domaine>\n"
    "MOTS: <10 à 25 mots-clés du domaine en minuscules, séparés par des virgules (logiciel, outils, termes)>\n"
    "METHODE:\n"
    "1) … jusqu'à 10) : l'ordre dans lequel un pro travaille, ses réglages de départ chiffrés, ses raccourcis et ses "
    "pièges. Uniquement des faits sûrs ; si une valeur dépend de la version, dis-le. Si des sources sont fournies, base-toi "
    "dessus en priorité.";

struct KnowledgeDraft {
    std::string name, method;
    std::vector<std::string> keywords;
};

inline std::optional<KnowledgeDraft> ParseKnowledgeDraft(std::string_view t) {
    KnowledgeDraft d;
    const auto line = [&](std::string_view key) -> std::string {
        const auto p = t.find(key);
        if (p == std::string_view::npos) return {};
        const auto e = t.find('\n', p);
        std::string v(t.substr(p + key.size(), e == std::string_view::npos ? std::string_view::npos : e - p - key.size()));
        while (!v.empty() && (v.front() == ' ')) v.erase(0, 1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
        return v;
    };
    d.name = line("NOM:");
    const std::string mots = LowerFr(line("MOTS:"));
    for (size_t p = 0; p < mots.size();) {
        size_t q = mots.find(',', p);
        if (q == std::string::npos) q = mots.size();
        std::string w = mots.substr(p, q - p);
        while (!w.empty() && w.front() == ' ') w.erase(0, 1);
        while (!w.empty() && (w.back() == ' ' || w.back() == '.')) w.pop_back();
        if (w.size() >= 2) d.keywords.push_back(w);
        p = q + 1;
    }
    if (const auto p = t.find("METHODE:"); p != std::string_view::npos) d.method = std::string(t.substr(p + 8));
    while (!d.method.empty() && (d.method.front() == '\n' || d.method.front() == ' ')) d.method.erase(0, 1);
    if (d.name.empty() || d.keywords.size() < 3 || d.method.size() < 200) return std::nullopt;
    return d;
}

}  // namespace companion
