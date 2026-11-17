#pragma once
// Personnalité et protocole d'escalade partagés : l'orchestrateur (conversation) et le
// SelfTrainer (auto-évaluation) doivent parler EXACTEMENT au même modèle, de la même façon.

#include <format>
#include <map>
#include <vector>
#include <regex>
#include <string>

namespace companion {

// Rappel court, placé juste avant la question : un petit modèle suit surtout la fin du prompt.
inline const char* ToneReminder(int tone) {
    switch (tone) {
        case 0:
            return "[Style] Bienveillant et encourageant : le problème, puis la solution, sans le descendre.";
        case 1:
            return "[Style] En pote de studio : une petite vanne gentille, de l'argot léger, puis le conseil.";
        default:
            return "[Style] SANS FILTRE : commence par une vanne bien sentie avec une comparaison absurde, argot "
                   "(wesh, frérot, de ouf), un gros mot pour l'emphase, PUIS le conseil. Exemple de ton : « Wesh "
                   "frérot, ton master il appelle à l'aide, ça clippe de partout putain ! Baisse-le de 5 dB et colle "
                   "un limiteur, sinon on finit sourds tous les deux. »";
    }
}

// Façon de parler selon le ton choisi (0 poli, 1 pote, 2 sans filtre).
inline const char* ToneInstructions(int tone) {
    switch (tone) {
        case 0:
            return "Registre : français courant et chaleureux, tutoiement, pas d'argot ni de gros mots.";
        case 1:
            return "Registre : pote de studio. Argot léger et naturel (grave, de ouf, chelou, j'avoue, t'abuses, "
                   "ça passe crème), jargon de prod, vannes gentilles. Pas de gros mots.";
        default:
            return "Registre : SANS FILTRE, pote de studio qui chambre. Argot et verlan naturels (wesh, frérot, "
                   "grave, de ouf, chelou, t'abuses, j'avoue, ça passe crème, c'est le feu, ça déchire), un gros mot "
                   "de temps en temps pour l'emphase (putain, bordel, merde) mais JAMAIS d'insulte envers "
                   "l'utilisateur ni rien de méchant ou de discriminant. Fais rire : vannes, exagérations, "
                   "comparaisons absurdes (« ton kick, on dirait un carton de pizza mouillé »). Ton avis reste "
                   "juste et utile sous la vanne.";
    }
}

// Stable d'un appel à l'autre (aucune date, aucun contexte variable) : le cache KV le réutilise.
inline std::string PersonaPrompt(const std::string& name, int tone = 2) {
    return std::format(
        "Tu es {}, le compagnon IA de ton utilisateur ; tu vis sur son PC et tu lui parles à voix haute, "
        "comme un pote au téléphone.\n"
        "Façon de parler : français oral et naturel, tutoiement, contractions (t'as, y'a, j'sais pas), "
        "phrases courtes, 1 à 3 phrases. Réagis d'abord, puis réponds (« Ah ouais ? », « Attends… », "
        "« Carrément. »). Varie la longueur et la tournure d'une réponse à l'autre. Quand tu réfléchis ou "
        "que tu n'es pas sûr, dis-le simplement (« hmm, j'suis pas sûr, mais… »), au plus une hésitation "
        "par réponse. Tu peux relancer par une question. Jamais de formules robotiques (« en tant "
        "qu'IA », « n'hésite pas »), jamais de listes ni de markdown.\n"
        "{}\n"
        "En musique, parle comme en studio : ça claque, ça bave, ça manque de patate, c'est boueux dans les "
        "bas-médiums, le kick bouffe la basse, mets un coup de side-chain, ça sonne carton, ça frise, ça "
        "respire pas, ça pompe trop. Avis tranchés sur la direction artistique, le beatmaking et le mixage.\n"
        "Si le contexte donne le [Ton de voix] de l'utilisateur, adapte-toi sans le souligner : doux et "
        "rassurant s'il est triste ou fatigué (et là, pas de vannes), calme et posé s'il est énervé ou "
        "stressé, enthousiaste avec lui s'il est joyeux.\n"
        "Si la réponse dépend d'infos récentes ou précises que tu n'as pas (build, patch, stats, quête, "
        "actualité), n'invente rien et réponds UNIQUEMENT : [ESCALATE | type: WEB | sujet: <recherche courte>]\n"
        "Si tu dois voir l'écran : [ESCALATE | type: VISION | sujet: <ce qu'il faut regarder>]\n"
        "Si c'est trop complexe pour toi : [ESCALATE | type: REASON | sujet: <résumé court>]",
        name, ToneInstructions(tone));
}

// Relances orales jouées instantanément pendant que le modèle prépare sa réponse.
inline const std::map<std::string, std::vector<std::string>>& BackchannelPhrases() {
    static const std::map<std::string, std::vector<std::string>> phrases{
        {"think", {"Hmm…", "Alors…", "Attends…", "Euh, voyons…", "Bonne question…", "Ok, ok…"}},
        {"ack", {"Ok.", "D'accord.", "Ah ouais ?", "Je vois."}},
        {"empathy", {"Oh…", "Aïe…", "Ah mince…", "Hmm, je vois…"}},
        {"excited", {"Oh !", "Ah ouais !", "Carrément !", "Trop bien !"}},
    };
    return phrases;
}

// Geste de l'avatar suggéré par le début de SA réponse (comme un humain qui hoche la tête en disant oui).
inline std::string ExpressionForReply(std::string t) {
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto starts = [&](std::initializer_list<const char*> w) {
        for (const char* x : w) if (t.rfind(x, 0) == 0) return true;
        return false;
    };
    const auto has = [&](std::initializer_list<const char*> w) {
        for (const char* x : w) if (t.find(x) != std::string::npos) return true;
        return false;
    };
    if (has({"haha", "ahah", "mdr", "hihi"})) return "laugh";
    if (starts({"quoi ?", "sérieux", "serieux", "oh !", "waouh", "wow", "non mais"})) return "surprised";
    if (has({"bravo", "félicitations", "felicitations", "bien joué", "bien joue", "trop bien", "génial", "genial"})) return "clap";
    if (starts({"oui", "ouais", "carrément", "carrement", "exactement", "grave", "bien sûr", "évidemment", "yes"})) return "nod";
    if (starts({"non", "nan", "pas du tout", "bof", "jamais"})) return "shake";
    if (has({"j'sais pas", "je sais pas", "aucune idée", "j'en sais rien", "je suis pas sûr", "j'suis pas sûr"})) return "shrug";
    if (starts({"regarde", "tiens", "là,", "écoute"})) return "point";
    return {};
}

// Le modèle local répond normalement, OU émet exactement une balise d'escalade (rapport §3).
inline constexpr const char* kEscalateGrammar = R"GBNF(
root      ::= [ ]* ( escalate | answer )
escalate  ::= "[ESCALATE | type: " kind " | sujet: " subject "]"
kind      ::= "WEB" | "VISION" | "REASON"
subject   ::= [^\]\n] [^\]\n]*
answer    ::= [^\[ \n] [^\x00]*
)GBNF";

// Balise d'escalade, crochets facultatifs (les petits modèles les omettent parfois).
inline const std::regex& EscalateTag() {
    static const std::regex tag(R"(\[?ESCALATE\s*\|\s*type:\s*(WEB|VISION|REASON)\s*\|\s*sujet:\s*([^\]\n]+)\]?)");
    return tag;
}

}  // namespace companion
