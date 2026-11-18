// Hésitations, bégaiements, auto-corrections : exemples sur des répliques typiques (niveau 0,5 = réglage par défaut).
#include "core/Disfluency.hpp"
#include <cstdio>
using namespace companion;
int main() {
    std::uint32_t rng = 7;
    const char* lines[] = {"C'est mignon comme conviction, mais la trap a quand même conquis le monde.",
                           "Je pense que ton kick bouffe la basse, mets un sidechain sur le compresseur.",
                           "T'as raison, faut croire en ses rêves. Même quand ton clavier y croit plus.",
                           "Baisse le master de 4 dB, t'es à -6 LUFS (rire) c'est chaud.",
                           "Ouais.", "Le Boss : on va dire que c'est un pseudo ironique, ça t'évitera la honte."};
    for (const char* l : lines)
        for (int i = 0; i < 2; ++i) std::printf("%s\n", Humanize(l, 0.5f, rng).c_str());
    std::printf("--- technique (calme) ---\n");
    for (int i = 0; i < 3; ++i) std::printf("%s\n", Humanize(lines[1], 0.5f, rng, true).c_str());
}
