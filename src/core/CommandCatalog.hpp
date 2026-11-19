#pragma once
// CommandCatalog : tout ce qu'Omnis sait faire, au même endroit, pour qu'on n'ait rien à retenir.
//   - « qu'est-ce que tu sais faire » / « comment je fais pour… » : l'aide vient d'ici ;
//   - phrase approximative (« viens sur discord avec nous », « mets-toi en mode léger stp ») : le modèle local choisit
//     la commande dans cette liste (grammaire = identifiants), puis la phrase « canonique » est exécutée comme si on
//     l'avait dite. Pas de commande devinée pour une question ou une discussion (« AUCUNE »).
// Les phrases canoniques déclenchent les gestionnaires existants (HandleLevel0 et compagnie).

#include <string>
#include <string_view>
#include <vector>

namespace companion {

struct OmnisCommand {
    const char* id;          // identifiant (grammaire du classement)
    const char* canonical;   // phrase reconnue telle quelle par les gestionnaires
    const char* what;        // ce que ça fait (pour le modèle et pour l'aide)
    const char* category;
};

inline const std::vector<OmnisCommand>& Commands() {
    static const std::vector<OmnisCommand> k = {
        {"reglages", "ouvre les réglages", "ouvrir la fenêtre de réglages / paramètres / configuration", "base"},
        {"aide", "qu'est-ce que tu sais faire", "dire ce qu'il sait faire", "base"},
        {"page_aide", "ouvre l'aide", "ouvrir la page d'aide complète", "base"},
        {"quitter", "ferme-toi", "fermer Omnis (quitter l'application)", "base"},
        {"cacher", "cache-toi", "se cacher ou réapparaître à l'écran", "base"},
        {"avis_prod", "écoute ce que j'ai fait, t'en penses quoi", "écouter la prod / le son / le beat dans FL Studio et donner son avis", "musique"},
        {"ecran", "regarde mon écran", "regarder l'écran pour aider", "ecran"},
        {"regarde_avec_moi", "regarde avec moi", "regarder des Reels / TikTok / YouTube ensemble et réagir (« on regarde des tiktoks »)", "ecran"},
        {"arrete_regarder", "arrête de regarder", "arrêter de regarder les vidéos", "ecran"},
        {"etudie_lien", "étudie le lien copié", "étudier la vidéo drôle dont le lien est copié (pour ses vannes)", "ecran"},
        {"apprends_tuto", "apprends ce tuto, le lien est copié", "apprendre le tutoriel vidéo dont le lien est copié (fiche technique)", "ecran"},
        {"build_lol", "je build quoi", "conseiller les objets à acheter en partie de LoL", "jeu"},
        {"fps", "combien de fps", "dire son impact sur les FPS du jeu", "jeu"},
        {"teste_perfs", "teste tes perfs", "mesurer son impact en jeu et se régler", "jeu"},
        {"cpu", "passe sur le processeur", "faire tourner son cerveau sur le processeur en jeu", "jeu"},
        {"gpu", "passe sur la carte", "faire tourner son cerveau sur la carte graphique en jeu", "jeu"},
        {"leger", "mode léger", "utiliser un petit modèle en jeu", "jeu"},
        {"auto", "mode auto", "revenir au choix automatique en jeu", "jeu"},
        {"prends_main", "prends la main", "contrôler la souris et le clavier pour faire la tâche demandée", "pc"},
        {"son_plus", "monte le son", "monter le volume du PC", "pc"},
        {"son_moins", "baisse le son", "baisser le volume du PC", "pc"},
        {"son_coupe", "coupe le son", "couper le son du PC", "pc"},
        {"son_remet", "remets le son", "remettre le son du PC", "pc"},
        {"musique_suivante", "musique suivante", "passer à la musique suivante", "pc"},
        {"musique_pause", "mets la musique en pause", "mettre la musique en pause ou la reprendre", "pc"},
        {"installe_sons", "installe les sons", "télécharger les bruitages", "sons"},
        {"liste_sons", "quels sons t'as", "dire quels bruitages il a", "sons"},
        {"call_rejoins", "rejoins le call", "rejoindre le call Discord (« viens sur Discord », « va dans le vocal », « connecte-toi au call »)", "discord"},
        {"call_quitte", "quitte le call", "quitter le call Discord", "discord"},
        {"call_ecoute", "écoute le call", "écouter le call sans parler pour apprendre les voix", "discord"},
        {"remplace", "prends ma place", "garder la place dans le call pendant une absence", "discord"},
        {"revenu", "je suis revenu", "rendre la place dans le call", "discord"},
        {"voix_prends", "prends ma voix", "copier la voix de l'utilisateur", "voix"},
        {"voix_reprends", "reprends ta voix", "revenir à sa propre voix", "voix"},
        {"voix_oublie", "oublie ma voix", "effacer la voix copiée", "voix"},
        {"expert", "mode expert", "passer en mode expert / mode pro : sortir le grand cerveau pour les questions de pro", "base"},
        {"expert_stop", "mode normal", "quitter le mode expert, revenir au cerveau normal", "base"},
        {"entraine", "entraîne-toi", "lancer une séance d'entraînement maintenant", "entrainement"},
        {"surfe", "tu peux surfer cette nuit", "s'entraîner sur internet cette nuit", "entrainement"},
        {"surfe_stop", "arrête de surfer", "arrêter de surfer sur internet", "entrainement"},
    };
    return k;
}

inline const OmnisCommand* FindCommand(std::string_view id) {
    for (const auto& c : Commands())
        if (id == c.id) return &c;
    return nullptr;
}

// Système du classement (stable : mis en cache par le cerveau) et sa grammaire.
inline std::string CommandClassifierPrompt() {
    std::string s =
        "Tu reçois une phrase dite à Omnis, une IA sur le PC. Si c'est clairement une DEMANDE D'ACTION qui correspond à une "
        "commande de la liste, réponds son identifiant, rien d'autre. Si c'est une question, une discussion, une vanne, une "
        "demande d'explication, ou si aucune commande ne colle vraiment, réponds AUCUNE.\nCommandes :\n";
    for (const auto& c : Commands()) s += std::string("- ") + c.id + " : " + c.what + "\n";
    return s;
}

inline std::string CommandGrammar() {
    std::string g = "root ::= \"AUCUNE\"";
    for (const auto& c : Commands()) g += std::string(" | \"") + c.id + "\"";
    return g + "\n";
}

// Ça ressemble à une demande d'action ? (on ne consulte le modèle que dans ce cas). Mots découpés aux espaces, tirets et
// apostrophes (« mets-toi », « ouvre-moi », « l'aide ») ; lower : déjà en minuscules, accents compris.
inline bool LooksLikeCommand(const std::string& lower) {
    if (lower.size() > 140) return false;
    static const char* kVerbs[] = {"mets", "met", "lance", "ouvre", "rejoins", "rejoint", "viens", "va", "vas", "joue", "arrête",
                                   "arrete", "écoute", "ecoute", "regarde", "prends", "prend", "copie", "passe", "monte",
                                   "baisse", "coupe", "installe", "étudie", "etudie", "entraîne", "entraine", "ferme", "cache",
                                   "quitte", "active", "désactive", "remets", "affiche", "montre", "peux", "pourrais", "connecte",
                                   "rentre", "allume", "éteins", "eteins", "change", "règle", "regle", "fais"};
    std::string w;
    const auto check = [&] {
        for (const char* v : kVerbs)
            if (w == v) return true;
        return false;
    };
    for (char c : lower + " ") {
        if (c == ' ' || c == '-' || c == '\'' || c == ',' || c == '.' || c == '!' || c == '?') {
            if (check()) return true;
            w.clear();
        } else {
            w += c;
        }
    }
    return false;
}

// Réponse à « qu'est-ce que tu sais faire » : courte, à l'oral.
inline const char* kHelpSummary =
    "En gros, je fais six trucs. J'écoute ta prod dans FL Studio et je te donne mon avis. Je regarde ton écran ou des vidéos "
    "avec toi. Je t'aide en jeu, sans te faire perdre de FPS. Je viens dans tes calls Discord. Je peux prendre la main sur "
    "ta souris si tu me le demandes. Et je m'entraîne tout seul. Pour du niveau pro, dis « mode expert ». T'as pas besoin de retenir des phrases : demande-moi "
    "comme tu veux, ou dis « ouvre l'aide » pour la page complète.";

}  // namespace companion
