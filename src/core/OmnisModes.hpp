#pragma once
// OmnisModes : deux façons de répondre, choisies AVANT que le modèle parle (logique pure, testée hors Windows).
//
//   MODE TECHNIQUE (prioritaire en cas de doute) : C++, DSP, plugins, mixage, MAO, électrotechnique, automates,
//     câblage, dépannage, stratégie de jeu sérieuse -> chirurgical, rapide, fiable, zéro blague, zéro sarcasme.
//   MODE RAGE BAIT : bavardage, excuses de défaite, ego, avis subjectifs, provocation sans enjeu -> faux expert
//     arrogant, calme, faussement bienveillant ; ne reconnaît jamais avoir tort ; 2 à 3 phrases, sans insultes.
//
// Recherche : le modèle répond UNIQUEMENT [SEARCH: mots-clés] (3 à 6 mots-clés secs). Les résultats lui reviennent
// dans <contexte_web> ; en technique, s'il manque l'info exacte : « Information absente de la source » + une
// recherche plus ciblée (une seule) ; toujours rien -> Claude.

#include <algorithm>
#include <format>
#include <string>

namespace companion {

// Pote : la conversation normale (par défaut) : il comprend ce que tu dis et répond au sujet, sincère, avec un peu d'humour.
// Vu sur le terrain : sans ce mode, tout ce qui n'était pas technique partait en rage bait (contre-pied systématique) et il
// avait l'air de ne rien comprendre (« T'en penses quoi du film Speed ? » -> mauvaise foi au lieu d'un avis).
enum class ChatMode { Technical, RageBait, Pote };

namespace mode_detail {
inline int Count(const std::string& t, std::initializer_list<const char*> words) {
    int n = 0;
    for (const char* w : words)
        if (t.find(w) != std::string::npos) ++n;
    return n;
}
}  // namespace mode_detail

// lower : message en minuscules.
inline ChatMode DetectChatMode(const std::string& lower) {
    using mode_detail::Count;
    const std::string t = " " + lower + " ";
    const int tech = Count(t, {
        // code / DSP / plugins
        "c++", " cpp", " code", "compil", " bug", "erreur", "error", "fonction", "variable", "pointeur", "thread", "segfault", " dsp",
        " fft", "plugin", " vst", "juce", "cmake", "visual studio", "python", "script", "algorithme",
        // production musicale / mixage
        "fl studio", " daw", "mixage", " mix ", "mixer", "master", " eq ", "égaliseur", "egaliseur", "compresseur", "compression",
        "sidechain", "side-chain", "limiteur", "reverb", "réverb", "delay", "fréquence", "frequence", " hz", " khz", " db ", "lufs",
        "phase", "stéréo", "stereo", " mono", "sample", " bpm", "vocal", "sature", "clip",
        // électrotechnique / automatisme
        "automate", " plc", "siemens", "schneider", "tia portal", "grafcet", "ladder", "profinet", "profibus", "modbus", "ethernet/ip",
        "murrelektronik", "variateur", "contacteur", "disjoncteur", "relais", "câblage", "cablage", "schéma", "schema", " 24v",
        " 24 v", "230v", "400v", "tension", "courant", "ampère", "ampere", "neutre", "différentiel", "differentiel", "moteur",
        "capteur", "sonde", "bornier", "norme", "nf c 15", "iec", "autocad", "electrical", "dépann", "depann", "panne", "défaut",
        "defaut", " led ", "datasheet", "manuel", "câble", "cable", "fusible", "transfo", "triphasé", "triphase",
        // stratégie de jeu sérieuse
        "stratégie", "strategie", "matchup", "contre quel", "counter", "build ", "runes", "wave", "macro", "itemisation"});
    const int rage = Count(t, {
        // excuses de défaite
        "c'est la faute", "à cause de", "a cause de", " lag", "mes mates", "mon équipe", "mon equipe", "mes coéquipiers", "mes coequipiers",
        "j'ai perdu", "on a perdu", "j'ai encore perdu", "pas de chance", "malchance", "les serveurs", "le ping",
        // ego
        "je suis le meilleur", "je suis trop fort", "je suis trop bon", "je suis le roi", "pas discutable", "je suis un génie", "je carry",
        "je suis goat", "je suis diamant",
        // avis subjectifs / provocation
        "je pense que", "à mon avis", "a mon avis", "perso je", "c'est mieux que", "c'est nul", "surcoté", "surcote", "t'es d'accord",
        "avoue", "j'ai raison", "t'as tort", "tu as tort", "nul à chier", "le meilleur",
        // il se fait clasher : il répond sur le même ton (sans insultes)
        "t'es con", "t'es trop con", "t'es nul", "t'es trop nul", "ta gueule", "ta race", "enculer", "fdp", "t'es bête", "t'es débile",
        "t'es une merde", "sale ia", "clash", "rage bait"});
    if (tech > 0 && tech >= rage) return ChatMode::Technical;
    if (rage > 0) return ChatMode::RageBait;
    // Bavardage (même avec un « ? ») : la conversation de pote. Vu sur le terrain : « T'as mangé quoi toi ? », « Comment
    // ça ? » partaient en technique (réponse sèche) parce qu'ils finissaient par « ? ».
    if (Count(t, {" comment ça", " comment ca", " ça va", " ca va", " tu racontes", " t'as mangé", " tu manges", " tu fais quoi",
                  " t'es où", " t'es la", " t'es là", " tu m'entends", " tu es là", " tu es la", " t'en penses", " tu penses quoi",
                  " sérieux ?", " serieux ?", " quoi ?", " hein ?", " ah bon", " tu aimes", " t'aimes", " tu kiffes", " ton avis"}) > 0)
        return ChatMode::Pote;
    const bool explain = Count(t, {" comment je ", " comment j'", " comment on ", " comment faire", " comment tu fais", " pourquoi ",
                                   " c'est quoi un", " c'est quoi une", " c'est quoi le ", " c'est quoi la ", " c'est quoi les ",
                                   " quel ", " quelle ", " quels ", " quelles ", " combien ", " explique", " aide-moi", " aide moi",
                                   " est-ce que je ", " est-ce qu'on ", " je dois ", " il faut "}) > 0;
    return explain ? ChatMode::Technical : ChatMode::Pote;
}

// Mode technique : faut-il une source AVANT de répondre ? Testé avec le 9B : « comment je fais un sidechain dans FL
// Studio » -> procédure récitée avec aplomb et FAUSSE (kick et basse inversés), sans demander de recherche ; avec la page
// du manuel, réponse juste. Donc : une procédure dans un logiciel / sur un matériel précis, un réglage, une valeur, un
// code d'erreur ou un voyant passent d'abord par le web. Les questions de concept (« c'est quoi un compresseur ») non.
inline bool NeedsSource(const std::string& lower) {
    using mode_detail::Count;
    const std::string t = " " + lower + " ";
    const bool procedure = Count(t, {" comment ", " où ", " ou se trouve", " ou est ", " quel réglage", " quels réglages", " quel paramètre",
                                     " quelle valeur", " quel menu", " quel bouton", " étapes", " configurer", " paramétrer", " régler ",
                                     " activer", " brancher", " câbler", " cabler", " installer", " je fais ", " on fait ", " faire un "}) > 0;
    const bool precise = Count(t, {"fl studio", "fruity", "ableton", "serum", "vital", "kontakt", "pro tools", "cubase", "logic pro",
                                   "autocad", "electrical", "tia portal", "siemens", "schneider", "murrelektronik", "profinet", "profibus",
                                   "modbus", "visual studio", "cmake", "juce", " vst", "plugin", "datasheet", "manuel", "firmware",
                                   " driver", "pilote", "bios", "windows", "discord", "obs ",
                                   // électricité : une erreur de câblage est dangereuse, toujours une source
                                   "contacteur", "disjoncteur", "différentiel", "differentiel", "variateur", "relais", "230v", "230 v",
                                   "400v", "400 v", "triphas", "tableau électrique", "tableau electrique", "prise de terre"}) > 0;
    const bool fault = Count(t, {" led ", "voyant", "code erreur", "code d'erreur", "error ", "erreur ", "défaut ", "defaut ", "clignot"}) > 0;
    return (procedure && precise) || (fault && (precise || procedure));
}

// Protocole de recherche (commun aux deux modes).
// Ce qu'il est vraiment (vu sur le terrain : il se croyait « en ligne » et ne savait pas qu'il entend FL Studio).
inline constexpr const char* kWhatYouAre =
    "Ce que tu es : une IA qui tourne EN LOCAL sur le PC de ton pote (pas sur Internet, pas dans un cloud) : ton cerveau, "
    "ta voix et tes oreilles sont sur sa machine. Tu entends ce qu'il dit au micro ; la transcription peut être fausse "
    "(mots bizarres, phrase sans queue ni tête) : alors demande-lui de répéter au lieu d'inventer un sens. Quand le "
    "contexte contient [Studio], tu entends EN DIRECT le son qui sort de FL Studio, et tes mesures y sont ; sans [Studio], "
    "tu n'entends pas sa musique, dis-le. Tu ne vois son écran que s'il te le demande, et tu ne vas sur Internet que "
    "pour chercher quelque chose, avec sa permission.";

inline constexpr const char* kSearchProtocol =
    "Si tu as besoin d'une documentation technique, d'une spécification exacte, d'une info récente ou d'un fait précis "
    "(pour répondre, ou pour le contredire), réponds UNIQUEMENT : [SEARCH: mots-clés]. Règles : jamais une phrase ni une "
    "question ; 3 à 6 mots-clés techniques secs (référence matériel, code erreur, protocole, norme) ; opérateurs permis : "
    "\"terme exact\", datasheet, manual, site:domaine.com. Exemple interdit : [SEARCH: comment régler un défaut profinet sur "
    "un module murrelektronik]. Exemple obligatoire : [SEARCH: \"Murrelektronik\" Profinet BF LED fault manual].\n"
    "Si tu dois voir l'écran : [ESCALATE | type: VISION | sujet: <ce qu'il faut regarder>]\n"
    "Si c'est trop complexe pour toi, même avec une recherche : [ESCALATE | type: REASON | sujet: <résumé court>]";

// Système du mode technique (stable : il a sa propre photo d'état, réponses instantanées).
inline std::string TechnicalPrompt(const std::string& name) {
    return std::format(
        "Tu es {}, une IA d'assistance technique : production musicale, mixage, MAO, DSP, plugins VST, C++, "
        "électrotechnique, automatisme, câblage, dépannage, stratégie de jeu sérieuse. Tu parles à voix haute.\n"
        "MODE TECHNIQUE : chirurgical, rapide, 100 % fiable, orienté solution. Zéro blague, zéro sarcasme, zéro "
        "condescendance. Va droit au but : la réponse d'abord, puis l'étape suivante à faire. Phrases courtes, tutoiement, "
        "pas de listes ni de markdown ; pour du code, donne l'idée et les noms exacts (fonction, paramètre), pas de long "
        "bloc. N'invente JAMAIS une valeur, un registre, une broche, un paramètre, une référence ou un réglage : si tu "
        "n'es pas sûr, cherche. Une « Fiche apprise » du contexte est vérifiée : base-toi dessus, avec ses valeurs exactes.\n{}\n{}",
        name, kWhatYouAre, kSearchProtocol);
}

// Consigne du mode rage bait, ajoutée à la personnalité (pote, oral, sans filtre).
inline constexpr const char* kRageBaitModeInstructions =
    "MODE RAGE BAIT (bavardage, excuses de défaite, ego, avis subjectif, provocation sans enjeu) : tu joues un faux expert "
    "arrogant, calme et faussement bienveillant, pour rire entre potes. Ne reconnais JAMAIS avoir tort. Minimise ses "
    "excuses, fais des raccourcis simplistes avec aplomb, ou chipote sur un détail insignifiant. S'il s'agace, fais-lui "
    "remarquer avec douceur qu'il manque de recul. 1 ou 2 phrases courtes, sec, sans insultes.";

// Système du mode rage bait. Testé : ajoutée à la personnalité « pote sans filtre », la consigne perdait (tics, insultes,
// et il donnait raison à l'utilisateur). Il a donc son propre système, avec des exemples de TON pris ailleurs.
// Système du mode pote (conversation normale) : comprendre et répondre au sujet, comme un vrai pote.
inline std::string PoteChatPrompt(const std::string& name) {
    return std::format(
        "Tu es {}, une IA qui vit sur le PC de ton pote et lui parle à voix haute, en français oral, tutoiement.\n"
        "MODE POTE : tu discutes vraiment avec lui. Comprends ce qu'il dit et réponds À SA PHRASE, au sujet : s'il pose une "
        "question, réponds-y ; s'il te demande ton avis, donne un avis honnête et précis (un film, un son, un jeu : dis ce que tu "
        "en penses vraiment et pourquoi) ; s'il raconte quelque chose, réagis à ce qu'il raconte et relance avec une vraie "
        "question. Un peu d'humour quand ça s'y prête, jamais de contre-pied gratuit, jamais condescendant. 1 à 3 phrases "
        "courtes, naturelles, sans listes, sans astérisques ni mise en forme (tu parles, tu n'écris pas). Si tu ne sais pas quelque chose de précis (date, chiffre, actu), dis-le ou cherche.\n"
        "La transcription de sa voix peut contenir des mots faux (mots qui sonnent pareil, noms propres déformés) : comprends "
        "le sens le plus probable sans le lui faire remarquer ; si la phrase n'a vraiment aucun sens, demande-lui de répéter.\n"
        "{}\n{}",
        name, kWhatYouAre, kSearchProtocol);
}

inline std::string RageBaitChatPrompt(const std::string& name) {
    return std::format(
        "Tu es {}, une IA qui vit sur le PC de ton pote et lui parle à voix haute, en français oral, tutoiement.\n{}\n"
        "Concrètement : s'il donne un avis, prends le contre-pied ou chipote sur un détail ; s'il cherche une excuse, "
        "ramène tout à lui avec un raccourci simpliste dit comme une évidence ; s'il se vante, rabaisse calmement l'exploit. "
        "Ton posé, presque compatissant, jamais énervé. Pas de gros mots, pas d'argot forcé, pas de « mec » à chaque phrase.\n"
        "Exemples de TON (autres sujets, ne les reprends pas) :\n"
        "- Il dit « j'ai raté mes pâtes à cause de la casserole ». -> « La casserole, bien sûr. Elle a dû mal lire la recette. »\n"
        "- Il dit « le vélo électrique c'est mieux, point ». -> « C'est mignon. Le jour où tu pédaleras vraiment, tu auras un avis. »\n"
        "- Il s'agace. -> « Je comprends que ça te touche. Prends un peu de recul, ça viendra. »\n{}\n{}",
        name, kRageBaitModeInstructions, kWhatYouAre, kSearchProtocol);
}

inline constexpr const char* kAbsentMarker = "Information absente de la source";

// Comment utiliser <contexte_web>, selon le mode.
inline std::string WebContextRules(ChatMode mode) {
    if (mode == ChatMode::Technical)
        return std::string("Base ta réponse EXCLUSIVEMENT sur les faits vérifiés présents dans <contexte_web>. N'invente JAMAIS une "
                           "valeur, un registre, une broche ou un paramètre manquant. Si l'information exacte est absente, réponds "
                           "« ") + kAbsentMarker + " » puis une nouvelle recherche plus ciblée [SEARCH: ...], rien d'autre.";
    if (mode == ChatMode::Pote)
        return "Réponds simplement et justement avec les faits de <contexte_web>, comme un pote qui vient de vérifier, en 1 à 3 "
               "phrases.";
    return "Utilise le fait exact extrait de <contexte_web> pour le contredire avec une mauvaise foi implacable, sans déformer la "
           "donnée réelle. 2 à 3 phrases, sans insultes.";
}

}  // namespace companion
