#pragma once
// PilotPlan : le plan d'actions qu'Omnis écrit avant de prendre ton clavier et ta souris. Logique pure.
//
//   DIRE "J'ouvre le mixer et je baisse le kick."   annonce (toujours en premier)
//   TOUCHE f9                                       raccourci : ctrl+shift+s, alt+f4, espace, entree…
//   TAPE "Kick 808"                                 texte tapé tel quel
//   CLIC "Enregistrer"                              bouton visible, trouvé par son nom (accessibilité Windows)
//   COMMANDE "_LINE 0,0 100,0 "                     AutoCAD : ligne de commande (noms anglais précédés de _)
//   ATTENDS 500                                     millisecondes
//
// Les actions risquées (fermer, supprimer, écraser, quitter) demandent ton accord à voix haute avant de partir.

#include "core/TextFr.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

enum class PilotOp : std::uint8_t { Say, Key, Type, Click, Command, Wait };

struct PilotStep {
    PilotOp op = PilotOp::Say;
    std::string arg;
    int ms = 0;
};

struct KeyChord {
    std::vector<std::uint16_t> mods;  // VK_CONTROL 0x11, VK_SHIFT 0x10, VK_MENU 0x12, VK_LWIN 0x5B
    std::uint16_t vk = 0;
};

// GBNF : ce que le modèle a le droit d'écrire (10 actions au plus, une par ligne).
inline constexpr const char* kPilotGrammar = R"(root ::= step step? step? step? step? step? step? step? step? step?
step ::= (dire | touche | tape | clic | commande | attends) "\n"
dire ::= "DIRE " str
touche ::= "TOUCHE " [a-z0-9+]+
tape ::= "TAPE " str
clic ::= "CLIC " str
commande ::= "COMMANDE " str
attends ::= "ATTENDS " [0-9] [0-9]? [0-9]? [0-9]?
str ::= "\"" [^"\n]+ "\""
)";

namespace pilot_detail {
inline std::string Lower(std::string_view s) { return LowerFr(s); }
inline std::string Unquote(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return std::string(s);
}
}  // namespace pilot_detail

inline std::vector<PilotStep> ParsePilotPlan(std::string_view text) {
    std::vector<PilotStep> steps;
    while (!text.empty() && steps.size() < 10) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const auto sp = line.find(' ');
        if (sp == std::string_view::npos) continue;
        const std::string_view verb = line.substr(0, sp), rest = line.substr(sp + 1);
        PilotStep s;
        if (verb == "DIRE") s.op = PilotOp::Say;
        else if (verb == "TOUCHE") s.op = PilotOp::Key;
        else if (verb == "TAPE") s.op = PilotOp::Type;
        else if (verb == "CLIC") s.op = PilotOp::Click;
        else if (verb == "COMMANDE") s.op = PilotOp::Command;
        else if (verb == "ATTENDS") s.op = PilotOp::Wait;
        else continue;
        s.arg = pilot_detail::Unquote(rest);
        if (s.op == PilotOp::Wait) {
            s.ms = std::clamp(std::atoi(s.arg.c_str()), 0, 5000);
            s.arg.clear();
        }
        if (s.op != PilotOp::Wait && s.arg.empty()) continue;
        steps.push_back(std::move(s));
    }
    return steps;
}

// « ctrl+shift+s », « f9 », « espace », « entree », « echap », « suppr », « a » -> codes de touches Windows.
inline std::optional<KeyChord> ParseChord(std::string_view text) {
    KeyChord k;
    std::string s = pilot_detail::Lower(text);
    size_t start = 0;
    while (start <= s.size()) {
        const size_t plus = s.find('+', start);
        const std::string part = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        start = plus == std::string::npos ? s.size() + 1 : plus + 1;
        if (part.empty()) return std::nullopt;
        std::uint16_t mod = 0;
        if (part == "ctrl" || part == "control") mod = 0x11;
        else if (part == "shift" || part == "maj") mod = 0x10;
        else if (part == "alt") mod = 0x12;
        else if (part == "win") mod = 0x5B;
        if (mod) {
            k.mods.push_back(mod);
            continue;
        }
        if (k.vk) return std::nullopt;  // deux touches principales
        if (part.size() == 1 && std::isalpha(static_cast<unsigned char>(part[0]))) k.vk = static_cast<std::uint16_t>(std::toupper(part[0]));
        else if (part.size() == 1 && std::isdigit(static_cast<unsigned char>(part[0]))) k.vk = static_cast<std::uint16_t>(part[0]);
        else if (part.size() >= 2 && part[0] == 'f' && std::all_of(part.begin() + 1, part.end(), ::isdigit)) {
            const int n = std::atoi(part.c_str() + 1);
            if (n < 1 || n > 24) return std::nullopt;
            k.vk = static_cast<std::uint16_t>(0x70 + n - 1);
        } else {
            static const std::pair<const char*, std::uint16_t> named[] = {
                {"espace", 0x20}, {"space", 0x20}, {"entree", 0x0D}, {"enter", 0x0D}, {"echap", 0x1B}, {"esc", 0x1B},
                {"tab", 0x09}, {"suppr", 0x2E}, {"delete", 0x2E}, {"del", 0x2E}, {"retour", 0x08}, {"backspace", 0x08},
                {"haut", 0x26}, {"up", 0x26}, {"bas", 0x28}, {"down", 0x28}, {"gauche", 0x25}, {"left", 0x25},
                {"droite", 0x27}, {"right", 0x27}, {"debut", 0x24}, {"home", 0x24}, {"fin", 0x23}, {"end", 0x23},
                {"pageup", 0x21}, {"pagedown", 0x22}, {"plus", 0x6B}, {"moins", 0x6D}, {"minus", 0x6D}};
            for (const auto& [name, vk] : named)
                if (part == name) k.vk = vk;
            if (!k.vk) return std::nullopt;
        }
    }
    if (!k.vk) return std::nullopt;
    return k;
}

// Ce qui peut faire perdre du travail : demande ton accord avant d'agir.
inline bool IsRiskyStep(const PilotStep& s) {
    using pilot_detail::Lower;
    const std::string a = Lower(s.arg);
    const auto has = [&](std::initializer_list<const char*> words) {
        return std::any_of(words.begin(), words.end(), [&](const char* w) { return a.find(w) != std::string::npos; });
    };
    switch (s.op) {
        case PilotOp::Key: {
            const auto k = ParseChord(s.arg);
            if (!k) return true;
            const bool ctrl = std::ranges::find(k->mods, std::uint16_t{0x11}) != k->mods.end();
            const bool alt = std::ranges::find(k->mods, std::uint16_t{0x12}) != k->mods.end();
            const bool shift = std::ranges::find(k->mods, std::uint16_t{0x10}) != k->mods.end();
            if (alt && k->vk == 0x73) return true;                                  // alt+f4
            if (ctrl && (k->vk == 'W' || k->vk == 'Q' || k->vk == 'N')) return true;  // fermer / quitter / nouveau
            if (ctrl && shift && k->vk == 'S') return true;                         // enregistrer sous (écrasement)
            if (k->vk == 0x2E) return true;                                         // suppr
            return false;
        }
        case PilotOp::Command:
            return has({"erase", "effacer", "delete", "purge", "close", "quit", "exit", "fermer", "quitter", "saveas",
                        "enregsous", "wblock", "_new", "nouveau", "overkill", "aeerase", "aedel"});
        case PilotOp::Click:
            return has({"supprim", "delete", "effac", "fermer", "close", "quitter", "exit", "ecraser", "écraser", "overwrite",
                        "remplacer", "replace", "ne pas enregistrer", "don't save", "dont save", "désinstaller", "uninstall"});
        default: return false;
    }
}

}  // namespace companion
