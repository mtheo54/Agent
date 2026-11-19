// Phrases approximatives -> commande d'Omnis (même prompt et même grammaire que l'appli). modèle.gguf cache
#include "core/CommandCatalog.hpp"
#include "core/TextFr.hpp"
#include "core/LocalBrain.hpp"
#include <chrono>
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const std::string sys = CommandClassifierPrompt(), gram = CommandGrammar();
    for (const char* t : {"Viens sur Discord avec nous", "Tu peux te mettre en mode léger stp", "Tu peux m'expliquer le sidechain ?",
                          "Regarde un peu ce qu'il y a sur mon écran", "T'as mangé quoi toi ?", "Écoute un peu ma prod là",
                          "Baisse un peu le volume stp", "Tu peux te fermer si tu veux genre partir ça va aller",
                          "Va dans le vocal avec les gars", "Mets-toi à regarder des tiktoks avec moi", "Tu peux copier ma voix ?",
                          "Franchement la drill c'est mieux que la trap", "Ouvre-moi tes paramètres", "Lance une séance d'entraînement"}) {
        if (!LooksLikeCommand(LowerFr(t))) {
            std::printf("%-58s -> (pas une action : modèle pas consulté)\n", t);
            continue;
        }
        GenerateOptions o; o.grammar = gram.c_str(); o.maxTokens = 8; o.temperature = 0.f;
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = brain.Generate({{"system", sys}, {"user", t}}, o);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const OmnisCommand* c = out ? FindCommand(*out) : nullptr;
        std::printf("%-58s -> %-16s %s (%.1f s)\n", t, out ? out->c_str() : "?", c ? c->canonical : "", s);
    }
}
