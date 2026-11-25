// Mode pote (par défaut) contre rage bait, sur les vraies phrases du journal. modèle.gguf cache
#include "core/LocalBrain.hpp"
#include "core/OmnisModes.hpp"
#include "core/TextFr.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    const char* qs[] = {"Omnis, t'en penses quoi du film Speed ?", "Non, non, je te parle du film Split, avec James McAvoy.",
                        "Moi j'aime beaucoup comment tu me parles, t'es assez sympa comme gars en fait.",
                        "Dis-moi, t'es sur quel processus pour parler actuellement ?", "Non voilà, t'es trop con.",
                        "J'ai perdu ma game à cause de mes mates.", "Tu fais quoi ce soir ?", "Tuti-alma avec moi !"};
    for (const char* q : qs) {
        const ChatMode m = DetectChatMode(LowerFr(q));
        std::printf("%-10s %s\n", m == ChatMode::Technical ? "TECHNIQUE" : m == ChatMode::Pote ? "POTE" : "RAGE BAIT", q);
    }
    if (!argv[1]) return 0;
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    for (const char* q : {qs[0], qs[1], qs[2], qs[7]})
        for (int pote = 1; pote >= 0; --pote) {
            GenerateOptions o; o.maxTokens = pote ? 130 : 70; o.temperature = 0.7f;
            const auto r = brain.Generate({{"system", pote ? PoteChatPrompt("Omnis") : RageBaitChatPrompt("Omnis")}, {"user", q}}, o);
            std::printf("\n[%s] %s\n-> %s\n", pote ? "POTE" : "ancien : RAGE BAIT", q, r ? r->c_str() : "?");
            std::fflush(stdout);
        }
}
