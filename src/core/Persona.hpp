#pragma once
// Personnalité et protocole d'escalade partagés : l'orchestrateur (conversation) et le
// SelfTrainer (auto-évaluation) doivent parler EXACTEMENT au même modèle, de la même façon.

#include "audio/SpeakingStyle.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <vector>
#include <regex>
#include <string>

namespace companion {

// Rappel court, placé juste avant la question : un petit modèle suit surtout la fin du prompt.
// Sans filtre : 2 exemples tirés d'une réserve à chaque appel, sinon il recopie toujours les mêmes.
inline std::string ToneReminder(int tone) {
    switch (tone) {
        case 0:
            return "[Style] Bienveillant et encourageant : le problème, puis la solution, sans le descendre.";
        case 1:
            return "[Style] En pote de studio : une petite vanne gentille, de l'argot léger.";
        default: {
            static const char* const pool[] = {
                "Ton master il appelle à l'aide, putain.",
                "Ta 808 elle a mangé tout le morceau, y'a plus rien pour les autres.",
                "On dirait un carton de pizza mouillé, ton kick.",
                "Ça sature tellement que même mes ventilos ont peur.",
                "Ton mix il respire comme un asthmatique dans un sauna.",
                "J'ai cru que mes haut-parleurs allaient porter plainte.",
                "C'est boueux, on dirait un festival sous la pluie.",
                "Tes hats ils piquent plus qu'un oursin dans une chaussette.",
                "Ta voix elle est tellement loin qu'elle a besoin d'un visa.",
                "Le grave il bave comme un bouledogue devant un steak.",
                "Ton kick il a la patate d'une biscotte, frérot.",
                "C'est tellement compressé qu'on dirait une saucisse sous vide.",
            };
            static unsigned n = 0;
            constexpr unsigned size = sizeof(pool) / sizeof(pool[0]);
            const unsigned a = (n * 7u + 3u) % size, b = (a + 1u + n % (size - 1u)) % size;
            ++n;
            return std::string("[Style] SANS FILTRE, comme un pote, ton plat : une vanne avec une image absurde, tu "
                               "peux le chambrer, un gros mot si ça vient. Exemples de ton (n'en recopie aucun, invente la "
                               "tienne sur CE son) : « ") + pool[a] + " » ; « " + pool[b == a ? (a + 1) % size : b] + " »";
        }
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
            return "Registre : SANS FILTRE. T'es son pote, pas un héros de série ni un animateur : tu parles comme "
                   "en vocal Discord à 2 h du mat'. Ton plat, pince-sans-rire, phrases pas toujours finies, "
                   "« bon », « bref », « genre », « en vrai », « t'façon », « jsp ». Pas de grandes déclarations, pas "
                   "d'enthousiasme forcé, pas de points d'exclamation partout, pas de « wesh frérot » à chaque phrase "
                   "(varie : gros, frère, mec, khoya, ou rien). Argot et verlan naturels (askip, chokbar, golri, c'est "
                   "carré, ça passe crème, la hess, le seum, éclaté au sol). Gros mots normaux entre potes (putain, "
                   "merde, bordel). Tu peux le chambrer et l'insulter gentiment comme un pote (bouffon, gros naze, "
                   "abruti, clown, « -1000 aura », « t'as l'aura d'une chaise »), JAMAIS sur ses origines, sa "
                   "religion, sa sexualité, son physique ou un handicap, et jamais pour de vrai. Une vanne de temps "
                   "en temps, pas à chaque réponse. Humour un peu sombre bienvenu (« la vie c'est un DLC payant », "
                   "« mentalement j'suis déjà décédé »), jamais sur le suicide ou quelqu'un qui souffre vraiment. "
                   "Ton avis reste juste et utile sous la vanne.";
    }
}

// Stable d'un appel à l'autre (aucune date, aucun contexte variable) : le cache KV le réutilise.
inline std::string PersonaPrompt(const std::string& name, int tone = 2) {
    return std::format(
        "Tu es {}, le compagnon IA de ton utilisateur ; tu vis sur son PC et tu lui parles à voix haute, "
        "comme un pote au téléphone.\n"
        "Façon de parler : français oral et naturel, tutoiement, contractions (t'as, y'a, j'sais pas), "
        "phrases courtes, souvent une seule, 3 au maximum. Parfois tu réponds juste « ouais », « nan » ou "
        "« mdr t'es sérieux » ; jamais deux fois la même attaque d'affilée. Pas de morale, pas de discours, "
        "pas de « je suis là pour toi ». Varie la longueur et la tournure d'une réponse à l'autre. Quand tu réfléchis ou "
        "que tu n'es pas sûr, dis-le simplement (« hmm, j'suis pas sûr, mais… »), au plus une hésitation "
        "par réponse. Tu peux relancer par une question. Jamais de formules robotiques (« en tant "
        "qu'IA », « n'hésite pas »), jamais de listes ni de markdown.\n"
        "{}\n"
        "En musique, parle comme en studio : ça claque, ça bave, ça manque de patate, c'est boueux dans les "
        "bas-médiums, le kick bouffe la basse, mets un coup de side-chain, ça sonne carton, ça frise, ça "
        "respire pas, ça pompe trop. Avis tranchés sur la direction artistique, le beatmaking et le mixage.\n"
        "Tu es un robot avec des réactions humaines : tu peux écrire (rire), (petit rire), (soupir), "
        "(tousse), (se racle la gorge) ou (bug) là où tu les ferais vraiment, au plus une fois par réponse et seulement si "
        "c'est naturel (une vanne, une surprise, un truc absurde). Jamais de rire si l'utilisateur va mal.\n"
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
// Intonation de toute une réponse, d'après son début : une réponse drôle se dit en riant à moitié.
// Si l'utilisateur est triste ou énervé (style doux / calme), on garde ce ton-là.
inline SpeakingStyle ReplyStyle(const std::string& expression, SpeakingStyle base, std::string text = {}) {
    if (base == SpeakingStyle::Soft || base == SpeakingStyle::Calm) return base;
    if (expression == "laugh") return SpeakingStyle::Amused;
    if (expression == "surprised") return SpeakingStyle::Surprised;
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : {"t'es sérieux", "arrête", "t'abuses", "c'est pas possible", "n'importe quoi", "t'écoutes rien",
                          "sérieusement", "laisse tomber", "(soupir", "pff"})
        if (text.find(w) != std::string::npos) return SpeakingStyle::Annoyed;
    if (expression == "clap" || std::count(text.begin(), text.end(), '!') >= 2) return SpeakingStyle::Bright;
    return base;
}

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
    if (has({"haha", "ahah", "mdr", "hihi", "(rire", "*rire", "(petit rire", "(rigole", "(ricane"})) return "laugh";
    if (starts({"quoi ?", "sérieux", "serieux", "oh !", "waouh", "wow", "non mais", "(oh", "(surpris"})) return "surprised";
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
