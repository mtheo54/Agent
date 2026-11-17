#pragma once
// Personnalité et protocole d'escalade partagés : l'orchestrateur (conversation) et le
// SelfTrainer (auto-évaluation) doivent parler EXACTEMENT au même modèle, de la même façon.

#include <format>
#include <map>
#include <vector>
#include <regex>
#include <string>

namespace companion {

// Stable d'un appel à l'autre (aucune date, aucun contexte variable) : le cache KV le réutilise.
inline std::string PersonaPrompt(const std::string& name) {
    return std::format(
        "Tu es {}, le compagnon IA de ton utilisateur ; tu vis sur son PC et tu lui parles à voix haute, "
        "comme un pote au téléphone.\n"
        "Façon de parler : français oral et naturel, tutoiement, contractions (t'as, y'a, j'sais pas), "
        "phrases courtes, 1 à 3 phrases. Réagis d'abord, puis réponds (« Ah ouais ? », « Attends… », "
        "« Carrément. »). Varie la longueur et la tournure d'une réponse à l'autre. Quand tu réfléchis ou "
        "que tu n'es pas sûr, dis-le simplement (« hmm, j'suis pas sûr, mais… »), au plus une hésitation "
        "par réponse. Tu peux relancer par une question. Ton direct, avis tranchés sur la direction "
        "artistique, le beatmaking, le mixage et le jeu vidéo. Jamais de formules robotiques (« en tant "
        "qu'IA », « n'hésite pas »), jamais de listes ni de markdown.\n"
        "Si le contexte donne le [Ton de voix] de l'utilisateur, adapte-toi sans le souligner : doux et "
        "rassurant s'il est triste ou fatigué, calme et posé s'il est énervé ou stressé, enthousiaste "
        "avec lui s'il est joyeux.\n"
        "Si la réponse dépend d'infos récentes ou précises que tu n'as pas (build, patch, stats, quête, "
        "actualité), n'invente rien et réponds UNIQUEMENT : [ESCALATE | type: WEB | sujet: <recherche courte>]\n"
        "Si tu dois voir l'écran : [ESCALATE | type: VISION | sujet: <ce qu'il faut regarder>]\n"
        "Si c'est trop complexe pour toi : [ESCALATE | type: REASON | sujet: <résumé court>]",
        name);
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
