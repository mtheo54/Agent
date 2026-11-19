// Suites d'accords choisies par le modèle (grammaire), notes calculées. modèle.gguf cache
#include "core/LocalBrain.hpp"
#include "modules/ChordScript.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 2048; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const std::string sys =
        "Tu es un compositeur et beatmaker professionnel. Donne la tonalité puis les DEGRÉS de la suite d'accords (les "
        "accords sont construits dans la gamme, toujours justes). Format : TONALITE <note anglaise A à G, # ou b> mineur|majeur, "
        "puis une ligne par accord : DEGRE <1 à 7> [+7 ou +9 pour un accord enrichi] <nombre de mesures 1 ou 2>. Exactement 4 "
        "accords (une boucle). Recettes de pro : trap / drill sombre = mineur, 1 6 3 7, ou 1 4 5 1, ou 1 6 7 1 ; RnB, lo-fi, néo-soul = +7 "
        "ou +9 partout, 2 5 1 1 en majeur ou 1 4 7 3 en mineur ; pop joyeuse = majeur, 1 5 6 4 ; triste et planant = mineur, "
        "1 7 6 7. Si l'utilisateur donne une tonalité (en sol = G, la mineur = A mineur), garde-la.";
    for (const char* q : {"fais-moi une suite d'accords trap sombre", "propose des accords lo-fi chill", "une grille pop joyeuse en sol"}) {
        GenerateOptions o; o.grammar = kChordGrammar; o.maxTokens = 80; o.temperature = 0.6f;
        const auto out = brain.Generate({{"system", sys}, {"user", q}}, o).value_or("");
        std::string names;
        for (const auto& c : ParseChordPlan(out)) names += c.name + "(" + std::to_string(c.bars) + ") ";
        std::printf("%-42s -> %s\n", q, names.c_str());
    }
}
