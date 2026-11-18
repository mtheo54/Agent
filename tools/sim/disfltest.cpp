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
    std::printf("--- avec budget (1 accident / ~22 mots), une conversation ---\n");
    int since = 0, words = 0, events = 0;
    for (int round = 0; round < 8; ++round)
        for (const char* l : lines) {
            const std::string out = Humanize(l, 0.5f, rng, false, true, &since);
            words += static_cast<int>(disfl_detail::Words(l).size());
            if (out != l) { ++events; if (round == 0) std::printf("%s\n", out.c_str()); }
        }
    std::printf("=> %d accidents pour %d mots : 1 tous les %.0f mots\n", events, words, static_cast<double>(words) / events);
    std::printf("--- technique (calme) ---\n");
    for (int i = 0; i < 3; ++i) std::printf("%s\n", Humanize(lines[1], 0.5f, rng, true).c_str());
}
