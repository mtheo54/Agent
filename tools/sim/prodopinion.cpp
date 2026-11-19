// Avis sur la prod (ressenti, sans chiffres) vs constats du mix sans mesures. modèle.gguf cache
#include "core/LocalBrain.hpp"
#include "core/Persona.hpp"
#include <cstdio>
#include <regex>
using namespace companion;
int main(int, char** argv) {
    const std::string facts = "Le sub / la 808 écrase tout (+14 dB au-dessus des médiums). Baisse la 808 de quelques dB dans le mixeur. "
                              "Trop faible (-24.0 LUFS) : monte le niveau des pistes, pas le master.";
    std::printf("constats sans mesures : %s\n\n", std::regex_replace(facts, std::regex(R"(\s*\([^)]*\d[^)]*\))"), "").c_str());
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const char* feels[] = {"ça envoie fort, c'est très dense ; punchy ; grosses basses, la 808 domine ; large, ça enveloppe ; ",
                           "plutôt calme et aéré ; du relief, des moments forts et calmes ; assez peu de basses ; son sombre, feutré ; "};
    for (const char* feel : feels)
        for (const char* q : {"Alors, t'en penses quoi de ma prod ?", "Tu kiffes ou pas ?"}) {
            GenerateOptions o; o.maxTokens = 120; o.temperature = 0.7f;
            const auto r = brain.Generate({{"system", PersonaPrompt("Omnis")},
                {"user", std::string("[Ce que tu entends de sa prod, en ce moment] ") + feel +
                 "\n[Consigne] Il te demande ton avis GÉNÉRAL sur sa prod, comme un pote qui fait de la musique. Dis si tu kiffes "
                 "ou pas et pourquoi : l'ambiance, l'énergie, ce qui accroche, ce qui manque ou ce que tu changerais. 2 ou 3 "
                 "phrases, franc mais pas méchant. AUCUN chiffre, aucun dB, aucun LUFS, pas de cours de mixage. Tu n'entends "
                 "que le son : n'invente pas de paroles ni de titre.\n[Question]\n" + q}}, o);
            std::printf("[%s] %s\n-> %s\n\n", feel[2] == '\xA7' ? "trap" : "calme", q, r ? r->c_str() : "?");
            std::fflush(stdout);
        }
}
