#pragma once
// Playbooks : la méthode d'un pro, par domaine (assets/knowledge/playbooks.json).
// Une question technique qui tombe dans un domaine (FL Studio, montage, AutoCAD Electrical, LoL…) reçoit la méthode
// de ce domaine dans son prompt système : l'ordre dans lequel un pro travaille, ses réflexes, ses chiffres de départ et
// ses pièges. Le prompt d'un domaine est stable : il a sa propre photo d'état (pas de recalcul à chaque question).

#include "core/TextFr.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace companion {

struct Playbook {
    std::string id, name, method;
    std::vector<std::string> keywords;  // minuscules, accents compris
};

inline std::vector<Playbook> LoadPlaybooks(const std::filesystem::path& file) {
    std::vector<Playbook> out;
    std::ifstream f(file, std::ios::binary);
    if (!f) return out;
    try {
        const auto j = nlohmann::json::parse(f);
        for (const auto& x : j.at("playbooks")) {
            Playbook p;
            p.id = x.value("id", std::string{});
            p.name = x.value("name", std::string{});
            p.method = x.value("method", std::string{});
            for (const auto& k : x.value("keywords", nlohmann::json::array())) p.keywords.push_back(LowerFr(k.get<std::string>()));
            if (!p.id.empty() && !p.method.empty()) out.push_back(std::move(p));
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

// Le domaine de la question (mot-clé le plus long trouvé, entouré de séparateurs) ; nullptr = aucun.
inline const Playbook* DetectPlaybook(const std::vector<Playbook>& books, const std::string& lower) {
    const std::string padded = " " + lower + " ";
    const auto sep = [](char c) { return c == ' ' || c == ',' || c == '.' || c == '?' || c == '!' || c == '\'' || c == '-' || c == ':'; };
    const Playbook* best = nullptr;
    size_t bestLen = 0;
    for (const auto& b : books)
        for (const auto& k : b.keywords) {
            if (k.size() <= bestLen) continue;
            for (size_t p = padded.find(k); p != std::string::npos; p = padded.find(k, p + 1))
                if (sep(padded[p - 1]) && p + k.size() < padded.size() && sep(padded[p + k.size()])) {
                    best = &b;
                    bestLen = k.size();
                    break;
                }
        }
    return best;
}

}  // namespace companion
