// Démo : sorties réelles du cerveau local (mêmes prompts que l'appli) pour la vidéo de démonstration.
//   1) mode technique : balise [SEARCH:], puis réponse fondée sur la page trouvée (<contexte_web>)
//   2) mode rage bait (discussion)
//   3) « regarde avec moi » : réaction à ce que la vision a vu (texte passé en argument)
// Usage : omnisdemo modèle.gguf dossier_cache "description vision" [watch]
#include "core/CallPrompt.hpp"
#include "core/LocalBrain.hpp"
#include "core/OmnisModes.hpp"
#include "core/Persona.hpp"
#include <cstdio>
using namespace companion;
static std::string Ask(LocalBrain& b, const std::string& sys, const std::string& user, const char* grammar, int max, float temp) {
    GenerateOptions o; o.grammar = grammar; o.maxTokens = max; o.temperature = temp;
    auto r = b.Generate({{"system", sys}, {"user", user}}, o);
    return r ? *r : "";
}
int main(int argc, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const bool watchOnly = argc > 4;  // 4e argument : seulement la réaction « regarde avec moi »
    const std::string tech = TechnicalPrompt("Omnis");
    {  // ce que fait maintenant l'appli : question qui exige une source -> le modèle n'écrit que les mots-clés
        const char* q0 = "Comment je fais un sidechain propre dans FL Studio ? Ma basse bave sur le kick.";
        std::printf("TECH_KW|%s\n", Ask(brain, tech, q0, kSearchOnlyGrammar, 32, 0.2f).c_str());
    }
    if (!watchOnly) {
    const char* q = "Comment je fais un sidechain propre dans FL Studio ? Ma basse bave sur le kick.";
    std::printf("TECH_Q|%s\n", q);
    std::printf("TECH_1|%s\n", Ask(brain, tech, q, kEscalateGrammar, 60, 0.3f).c_str());
    const std::string page =
        "<contexte_web>\n### FL Studio Manual — Mixer: sidechain (image-line.com)\n"
        "Pour qu'un son en fasse baisser un autre : sélectionne la piste du mixer du KICK, puis sur la piste de la BASSE fais clic droit sur la "
        "flèche d'envoi > « Sidechain to this track ». Le kick est alors envoyé en sidechain, sans être entendu en double. "
        "Sur la piste de la basse, ajoute Fruity Limiter, passe en mode COMP, et choisis l'entrée sidechain avec le bouton SIDECHAIN (numéro de la "
        "piste du kick). Règle THRES (seuil) assez bas pour que la basse descende à chaque kick, RATIO autour de 4:1 à 10:1, et REL (release) "
        "court, environ 50 à 150 ms, pour que la basse remonte avant le kick suivant.\n</contexte_web>\n";
    std::printf("TECH_2|%s\n", Ask(brain, tech, page + "[Consigne]\n" + WebContextRules(ChatMode::Technical) + "\n[Question]\n" + q, kEscalateGrammar, 170, 0.3f).c_str());
    const char* r = "Franchement la drill c'est mieux que la trap, c'est pas discutable.";
    std::printf("RAGE_Q|%s\n", r);
    std::printf("RAGE_1|%s\n", CleanSpoken(Ask(brain, RageBaitChatPrompt("Omnis"), r, kEscalateGrammar, 80, 0.8f)).c_str());
    }
    const std::string visual = argv[3];
    std::string out;
    for (int i = 0; i < 3 && out.empty(); ++i) {
        out = CleanSpoken(Ask(brain, PersonaPrompt("Omnis", 2),
            "[Contexte]\nTu regardes des vidéos (Reels / TikTok / YouTube) avec ton pote, sur son écran. Titre de l'onglet : « Quand ton pote dit "
            "qu'il va carry 😭 #lol - TikTok ». Ce qu'on entend : « son de défaite de League of Legends, rires ». \nCe qu'on voit :\n" + visual +
            "\n[Message]\nRéagis en UNE phrase courte, comme un pote assis à côté : une vanne sur la vidéo, un avis tranché, ou un « attends, c'est "
            "quoi ce truc ». Ne résume pas la vidéo.", nullptr, 50, 0.8f));
        if (!CrossesLine(out, {}).empty()) out.clear();
    }
    std::printf("WATCH_1|%s\n", out.c_str());
}
