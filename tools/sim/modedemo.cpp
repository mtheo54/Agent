// Modes avec le vrai modèle : technique (balise [SEARCH:], réponse fondée sur <contexte_web>, info absente),
// rage bait (faux expert). Mêmes prompts que l'appli (OmnisModes.hpp, Persona.hpp).
#include "core/LocalBrain.hpp"
#include "core/Persona.hpp"
#include <cstdio>
using namespace companion;
static std::string Ask(LocalBrain& b, const std::string& sys, const std::string& user, const char* grammar, int max) {
    GenerateOptions o; o.grammar = grammar; o.maxTokens = max; o.temperature = 0.3f;
    auto r = b.Generate({{"system", sys}, {"user", user}}, o); return r ? *r : "(rien)";
}
int main(int argc, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const std::string tech = TechnicalPrompt("Omnis"), rage = RageBaitChatPrompt("Omnis");
    const char* q1 = "Mon module Murrelektronik a la LED BF rouge en Profinet, je fais quoi ?";
    std::printf("1) [technique] %s\n   -> %s\n\n", q1, Ask(brain, tech, q1, kEscalateGrammar, 80).c_str());
    const std::string ctxOk = "<contexte_web>\n### Manuel MVK Pro (murrelektronik.com)\nLED BF (Bus Fault) rouge allumée : pas de connexion PROFINET avec le contrôleur (câble, nom d'appareil PROFINET non attribué ou adresse IP en conflit). BF clignotante : le contrôleur cherche le module mais la configuration ne correspond pas (fichier GSDML). Vérifier le nom d'appareil avec l'outil d'engineering.\n</contexte_web>\n";
    std::printf("2) [technique + source qui contient l'info]\n   -> %s\n\n",
                Ask(brain, tech, ctxOk + "[Consigne]\n" + WebContextRules(ChatMode::Technical) + "\n[Question]\n" + q1, kEscalateGrammar, 200).c_str());
    const std::string ctxNo = "<contexte_web>\n### Murrelektronik - Société\nMurrelektronik est une entreprise allemande fondée en 1975, spécialisée dans l'automatisation décentralisée.\n</contexte_web>\n";
    std::printf("3) [technique + source qui NE contient PAS l'info]\n   -> %s\n\n",
                Ask(brain, tech, ctxNo + "[Consigne]\n" + WebContextRules(ChatMode::Technical) + "\n[Question]\n" + q1, kEscalateGrammar, 120).c_str());
    if (argc > 3) {  // rage bait seulement
        for (const char* q : {"J'ai perdu à cause de mes mates, ils étaient nuls.", "Franchement la drill c'est mieux que la trap, c'est pas discutable.",
                              "J'ai fait un pentakill sur Yasuo, je suis trop fort.", "T'es saoulant, t'as tort et tu le sais."})
            std::printf("[rage bait] %s\n   -> %s\n\n", q, Ask(brain, rage, q, kEscalateGrammar, 90).c_str());
        return 0;
    }
    for (const char* q : {"J'ai perdu à cause de mes mates, ils étaient nuls.", "Franchement la drill c'est mieux que la trap, c'est pas discutable."})
        std::printf("4) [rage bait] %s\n   -> %s\n\n", q, Ask(brain, rage, q, kEscalateGrammar, 110).c_str());
}
