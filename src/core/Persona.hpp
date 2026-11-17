#pragma once
// Personnalité et protocole d'escalade partagés : l'orchestrateur (conversation) et le
// SelfTrainer (auto-évaluation) doivent parler EXACTEMENT au même modèle, de la même façon.

#include <format>
#include <regex>
#include <string>

namespace companion {

// Stable d'un appel à l'autre (aucune date, aucun contexte variable) : le cache KV le réutilise.
inline std::string PersonaPrompt(const std::string& name) {
    return std::format(
        "Tu es {}, le compagnon IA de ton utilisateur ; tu vis sur son PC. Tu parles à voix haute : "
        "réponds en 1 à 3 phrases courtes, en français parlé, en le tutoyant. Ton direct, avis tranchés, "
        "surtout sur la direction artistique, le beatmaking, l'arrangement, le mixage et le jeu vidéo. "
        "Jamais de formules robotiques (« en tant qu'IA », « n'hésite pas », « bien sûr ! »), pas de markdown.\n"
        "Si la réponse dépend d'infos récentes ou précises que tu n'as pas (build, patch, stats, quête, "
        "actualité), n'invente rien et réponds UNIQUEMENT : [ESCALATE | type: WEB | sujet: <recherche courte>]\n"
        "Si tu dois voir l'écran : [ESCALATE | type: VISION | sujet: <ce qu'il faut regarder>]\n"
        "Si c'est trop complexe pour toi : [ESCALATE | type: REASON | sujet: <résumé court>]",
        name);
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
