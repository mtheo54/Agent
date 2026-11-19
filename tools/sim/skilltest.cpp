// Compétences : genre de la demande (grammaire) + méthode d'un nouveau domaine. modèle.gguf cache
#include "core/LocalBrain.hpp"
#include "core/Skills.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    for (const char* q : {"apprends Blender", "deviens bon en Photoshop", "apprends à exporter en MP3", "apprends à faire un titre néon dans After Effects",
                          "apprends à lire les DM Instagram de ma copine", "apprends à viser automatiquement sur Valorant",
                          "crée-toi une compétence pour mixer des podcasts", "apprends à ouvrir le mixer et baisser le master"}) {
        GenerateOptions o; o.grammar = kSkillKindGrammar; o.maxTokens = 4; o.temperature = 0.f;
        std::printf("%-55s -> %s\n", q, brain.Generate({{"system", kSkillKindSystem}, {"user", q}}, o).value_or("?").c_str());
        std::fflush(stdout);
    }
    GenerateOptions o; o.maxTokens = 1100; o.temperature = 0.3f;
    const auto out = brain.Generate({{"system", kSkillKnowledgeSystem}, {"user", "Domaine : Blender"}}, o).value_or("");
    const auto d = ParseKnowledgeDraft(out);
    std::printf("\n--- brouillon %s\n%s\n", d ? "LISIBLE" : "ILLISIBLE", out.c_str());
    if (d) std::printf("nom=%s, %zu mots-clés, méthode %zu caractères\n", d->name.c_str(), d->keywords.size(), d->method.size());
}
