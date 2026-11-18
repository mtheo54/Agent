// Plans du pilote (lecture, raccourcis, actions risquées) et découpe des voix d'un call (CallVad).
#include "audio/CallVad.hpp"
#include "modules/PilotPlan.hpp"
#include <cmath>
#include <cstdio>
#include <random>
using namespace companion;
int main() {
    const char* plan = "DIRE \"J'ouvre le mixer, tranquille.\"\nTOUCHE f9\nATTENDS 300\nTOUCHE ctrl+shift+s\nCLIC \"Supprimer\"\n"
                       "COMMANDE \"_LINE 0,0 100,0  \"\nCOMMANDE \"_ERASE \"\nTAPE \"Kick 808\"\nBLABLA truc\nTOUCHE alt+f4\n";
    const char* ops[] = {"DIRE", "TOUCHE", "TAPE", "CLIC", "COMMANDE", "ATTENDS"};
    for (const auto& s : ParsePilotPlan(plan))
        std::printf("  %-8s %-24s %s%s\n", ops[static_cast<int>(s.op)], s.arg.c_str(), s.op == PilotOp::Wait ? std::to_string(s.ms).c_str() : "",
                    IsRiskyStep(s) ? "  <- accord demandé" : "");
    for (const char* k : {"ctrl+s", "f9", "alt+f8", "espace", "ctrl+shift+r", "suppr", "ctrl+", "f99", "a+b"}) {
        const auto c = ParseChord(k);
        std::printf("  « %s » -> %s", k, c ? "" : "refusé");
        if (c) { for (auto m : c->mods) std::printf("%02X+", m); std::printf("%02X", c->vk); }
        std::printf("\n");
    }
    // Call simulé : bruit de fond, phrase de 1,5 s, silence, toux de 200 ms (ignorée), phrase de 2 s.
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.f, 0.002f);
    std::vector<float> call;
    const auto add = [&](double sec, float amp) {
        for (int i = 0; i < sec * 16000; ++i) call.push_back(noise(rng) + amp * std::sin(i * 0.12f) * (0.6f + 0.4f * std::sin(i * 0.002f)));
    };
    add(1.0, 0); add(1.5, 0.1f); add(1.2, 0); add(0.2, 0.15f); add(1.2, 0); add(2.0, 0.08f); add(1.5, 0);
    CallVad vad;
    std::vector<float> seg;
    for (size_t i = 0; i < call.size(); i += 480)
        if (vad.Feed(call.data() + i, std::min<size_t>(480, call.size() - i), seg))
            std::printf("  phrase détectée : %.2f s (fin à %.1f s)\n", seg.size() / 16000.0, (i + 480) / 16000.0);
}
