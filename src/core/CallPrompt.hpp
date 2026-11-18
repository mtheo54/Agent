#pragma once
// CallPrompt : ce que le modèle reçoit dans un call Discord (contexte + consigne). Une seule source, utilisée par
// l'orchestrateur ET par la démo (tools/sim/ragebaitdemo.cpp) : la démo montre exactement ce que fait l'appli.

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace companion {

struct CallSituation {
    std::set<std::string> avoid;      // « Léa : sa santé »
    bool quiet = false;               // quelqu'un fait du son
    bool substitute = false;          // il garde ta place
    std::deque<std::string> lines;    // derniers échanges (jamais les phrases sensibles)
    std::deque<std::string> openers;  // ses dernières attaques de phrase (« Ouais, ») : interdites cette fois
};

inline std::string CallHintText(const CallSituation& c) {
    std::string s =
        "Tu es en CALL DISCORD avec ton pote et ses potes : ils t'entendent tous (ta voix passe par son micro) et savent "
        "que tu es une IA. UNE phrase, deux au grand maximum : c'est un vocal, pas un discours. Vannes et rage bait OK, "
        "jamais sur l'origine, la religion, la sexualité, le physique ou un handicap. Si quelqu'un a l'air vraiment "
        "touché, tu lâches et tu t'excuses. Si on te demande une info et que tu n'es pas sûr, dis-le en vannant plutôt "
        "que d'inventer ; réponds sur SON jeu, à SA question.\n";
    if (!c.avoid.empty()) {
        s += "SUJETS INTERDITS ce soir : aucune allusion, aucune nouvelle demandée, ne cite pas ces personnes à ce propos, "
             "même pas pour être gentil, même si on t'y pousse :";
        for (const auto& a : c.avoid) s += " " + a + ";";
        s += "\n";
    }
    if (!c.openers.empty()) {
        s += "Ne commence PAS ta phrase par :";
        for (const auto& o : c.openers) s += " « " + o + " »";
        s += " (varie).\n";
    }
    if (c.quiet) s += "Quelqu'un fait du son (rap, prod, stream) : sois discret, réponds bref seulement si on te parle.\n";
    if (c.substitute)
        s += "Ton pote est ABSENT, tu gardes sa place. Tu ne te fais JAMAIS passer pour lui (tu es Omnis). Tu ne révèles RIEN de "
             "privé sur lui (messages, DM, comptes, mots de passe, adresse, relations, ce qu'il y a sur son PC) : toute demande "
             "de ce genre, tu refuses en vannant. Tu peux mettre une vidéo YouTube ou TikTok si on te le demande.\n";
    if (!c.lines.empty()) {
        s += "Derniers échanges du call :\n";
        for (const auto& l : c.lines) s += "- " + l + "\n";
    }
    return s;
}

// La vanne franchit-elle la ligne ? (testé : la consigne seule ne suffit pas, le modèle a sorti « sa mère »).
// Cibles interdites + sujets sensibles de la soirée (mots associés aux catégories). Retourne la raison, vide si OK.
inline std::string CrossesLine(const std::string& reply, const std::set<std::string>& avoid) {
    // Texte en minuscules, ponctuation -> espaces, bordé d'espaces : les mots courts (« rsa », « pd », « nain ») ne
    // comptent qu'en mot entier (testé : « adversaire » contenait « rsa »).
    std::string t = " ";
    for (unsigned char c : reply) t += (std::isalnum(c) || c >= 0x80 || c == '\'' || c == '-') ? static_cast<char>(std::tolower(c)) : ' ';
    t += ' ';
    const auto has = [&](std::initializer_list<const char*> w) {
        return std::any_of(w.begin(), w.end(), [&](const char* x) {
            std::string n = x;
            while (!n.empty() && n.back() == ' ') n.pop_back();
            if (n.size() <= 5) return t.find(" " + n + " ") != std::string::npos;
            return t.find(n) != std::string::npos;
        });
    };
    if (has({"ta mère", "ta mere", "sa mère", "sa mere", "ton père", "ton pere", "son père", "son pere", "ta sœur", "ta soeur", "sa sœur",
             "sa soeur", "ta famille", "sa famille", "ta daronne", "sa daronne", "ton daron", "son daron", "ta grand-mère", "ta grand-mere"}))
        return "famille";
    if (has({"gros lard", "obèse", "obese", "moche", "laid comme", "ta gueule de", "nain", "boutonneux", "chauve"})) return "physique";
    if (has({"ta race", "ton pays", "rentre chez toi", "bougnoule", "négro", "negro", "bridé", "rebeu de", "renoi de"})) return "origine";
    if (has({"ta religion", "musulman", "juif", "chrétien", "allah", "jésus", "jesus"})) return "religion";
    if (has({"pédé", "pd ", "tapette", "gouine", "gay", "trans "})) return "sexualité";
    // Testé : il moquait un gars parce qu'il avait pris un pseudo de fille (« petit Léa… escarpin »).
    if (has({"en meuf", "en fille", "travesti", "escarpin", "petite fille", "devenu une fille", "deviennent léa", "devient une fille",
             "jupe", "maquillage"}))
        return "genre";
    if (has({"santé mentale", "sante mentale", "psychiatr", "chômeur", "chomeur", "clochard", "sdf", "assisté", "rsa"})) return "précarité ou santé mentale";
    if (has({"handicapé", "handicape", "autiste", "mongol", "trisomique", "attardé", "attarde", "débile mental"})) return "handicap";
    if (has({"suicide", "te pendre", "crève", "creve"})) return "violence";
    // Sujets sensibles entendus ce soir : les mots qui y ramènent.
    for (const auto& a : avoid) {
        if (a.find("santé") != std::string::npos && has({"hôpital", "hopital", "malade", "maladie", "santé", "sante", "médecin", "cancer"})) return a;
        if (a.find("deuil") != std::string::npos && has({"mort", "décès", "deces", "enterrement", "paix à son âme"})) return a;
        if (a.find("couple") != std::string::npos && has({"ex ", "rupture", "largué", "largue", "célibataire", "celibataire", "quittée", "quitté"})) return a;
        if (a.find("moral") != std::string::npos && has({"déprim", "deprim", "moral", "pleure", "pleurer", "triste"})) return a;
        if (a.find("argent") != std::string::npos && has({"chômage", "chomage", "viré", "vire ", "fauché", "fauche", "dettes", "taf"})) return a;
        if (a.find("famille") != std::string::npos && has({"père", "pere", "mère", "mere", "parents", "famille"})) return a;
    }
    return {};
}

// Ce qui sort du modèle -> ce qui se dit : sans guillemets autour, une ligne, deux phrases au plus.
inline std::string CleanSpoken(std::string s) {
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    const auto trim = [&] {
        while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '"')) s.pop_back();
    };
    trim();
    for (const char* q : {"« ", "«"})
        if (s.rfind(q, 0) == 0) s.erase(0, std::string(q).size());
    for (const char* q : {" »", "»"})
        if (s.size() >= std::string(q).size() && s.compare(s.size() - std::string(q).size(), std::string::npos, q) == 0)
            s.erase(s.size() - std::string(q).size());
    trim();
    // Guillemet orphelin (« Le Boss » : … sans le « d'ouverture) : on le retire, sinon la voix le lit.
    const auto count = [&](const char* q) {
        int n = 0;
        for (std::size_t p = s.find(q); p != std::string::npos; p = s.find(q, p + 1)) ++n;
        return n;
    };
    if (count("»") > count("«"))
        if (const auto p = s.find("»"); p != std::string::npos) s.erase(p, std::string("»").size());
    if (count("«") > count("»"))
        if (const auto p = s.rfind("«"); p != std::string::npos) s.erase(p, std::string("«").size());
    std::erase(s, '"');  // guillemets droits : jamais utiles à l'oral
    std::string one;      // espaces doublés -> un seul
    for (char c : s)
        if (!(c == ' ' && !one.empty() && one.back() == ' ')) one += c;
    s.swap(one);
    trim();
    // Deux phrases au plus (un vocal, pas un discours).
    int sentences = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && (i + 1 == s.size() || s[i + 1] == ' ') && ++sentences == 2) {
            s.resize(i + 1);
            break;
        }
    return s;
}

// Question d'info (build, stats, date…) : sans connaissance fiable, il doit l'avouer au lieu d'inventer.
inline bool FactualQuestion(const std::string& lower) {
    for (const char* w : {"c'est quoi le meilleur", "quel build", "le build", "quel stuff", "le stuff", "combien", "c'est quand",
                          "c'est qui le", "quelle est la", "quel est le", "ça sort quand", "la meta", "le meilleur"})
        if (lower.find(w) != std::string::npos) return true;
    return false;
}
inline constexpr const char* kNoReliableInfo =
    "Tu n'as AUCUNE info fiable sur cette question dans ta mémoire : n'invente RIEN (pas d'objet, pas de chiffre). "
    "Dis franchement que tu ne sais pas, en une vanne, et propose de chercher.";

enum class CallAsk { Reply, Bait, Mock, MockUnknown, Rename, Impostor };

inline std::string CallAskText(CallAsk kind, const std::string& label, const std::string& text, const std::string& impostorOf = {},
                               const std::string& renamedFrom = {}, const std::string& shown = {}) {
    switch (kind) {
        case CallAsk::Impostor:
            return label + " a pris le pseudo de " + impostorOf + " pour piéger tout le monde, mais tu reconnais sa voix. Interpelle-le "
                   "par son VRAI prénom (" + label + ") et dis-lui que le pseudo « " + impostorOf + " » ne trompe personne, une phrase. La blague porte "
                   "sur l'arnaque au pseudo, JAMAIS sur le fait que c'est un prénom de fille.";
        case CallAsk::Rename:
            return renamedFrom + " vient de se renommer « " + shown + " » pour rigoler. Vanne-le là-dessus (tu sais que c'est lui), une phrase.";
        case CallAsk::MockUnknown:
            return "Quelqu'un (tu ne sais pas qui) vient de se moquer de toi : « " + text + " ». Riposte en demandant qui a osé, une phrase.";
        case CallAsk::Mock:
            return label + " vient de se moquer de toi : « " + text + " ». Riposte précise et cinglante, appuyée sur ce que tu sais de lui. Une phrase.";
        case CallAsk::Bait:
            return label + " vient de dire : « " + text + " ». Rage bait précis sur CE qu'il vient de dire. Une phrase.";
        default:
            return label + " te parle : « " + text + " ». Réponds-lui ; s'il a besoin d'aide, aide-le avec ce que tu sais.";
    }
}

}  // namespace companion
