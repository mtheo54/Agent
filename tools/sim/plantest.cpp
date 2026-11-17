#include "core/ModelPlanner.hpp"
#include <cstdio>
using namespace companion;
int main() {
    std::printf("%-26s | %-58s | %-58s | %s\n", "PC", "HORS JEU", "EN JEU (VRAM libre inconnue)", "TÉLÉCHARGEMENT");
    struct Case { const char* name; double vram, ram; };
    for (Case c : {Case{"Intégrée, 8 Go RAM", 0, 8}, {"Intégrée, 16 Go RAM", 0, 16}, {"1 Go VRAM, 8 Go", 1, 8}, {"2 Go VRAM, 8 Go", 2, 8},
                   {"4 Go VRAM, 16 Go", 4, 16}, {"6 Go VRAM, 16 Go", 6, 16}, {"8 Go VRAM, 16 Go", 8, 16}, {"12 Go VRAM, 32 Go", 12, 32},
                   {"16 Go VRAM, 32 Go (toi)", 15.9, 32}, {"24 Go VRAM, 64 Go", 24, 64}}) {
        HardwareInfo hw; hw.vramGiB = c.vram; hw.ramGiB = c.ram;
        const auto v = ModelPlanner::PlanVoice(hw, true);
        const auto n = ModelPlanner::PlanNormal(hw, v.neural), g = ModelPlanner::PlanGaming(hw);
        double gb = 0; for (auto& d : ModelPlanner::Required(hw, true)) gb += d.bytes / 1e9;
        std::printf("%-26s | %-12s %-3s + %-6s %-26s | %-12s %-3s %-38s | %.1f Go\n", c.name, n.model->name, n.gpu ? "GPU" : "CPU",
                    v.neural ? "neuro" : "piper", "", g.model->name, g.gpu ? "GPU" : "CPU", "", gb);
    }
    std::printf("\nGros jeu sur ta carte (16 Go) selon la VRAM qu'il laisse libre :\n");
    for (double fr : {12.0, 6.0, 4.0, 3.0, 2.0, 1.5, 0.8}) {
        HardwareInfo hw; hw.vramGiB = 15.9; hw.ramGiB = 32; hw.vramFreeGiB = fr;
        const auto g = ModelPlanner::PlanGaming(hw);
        std::printf("  %4.1f Go libres -> %s\n", fr, g.why.c_str());
    }

    std::printf("\nCarte ou processeur, selon ce que le jeu sature (ta carte, 16 Go) :\n");
    bool busy = true;  // verdict initial prudent
    for (double load : {-1.0, 0.45, 0.80, 0.90, 0.78, 0.65, 0.99}) {
        HardwareInfo hw; hw.vramGiB = 15.9; hw.ramGiB = 32; hw.gameGpuLoad = load;
        busy = ModelPlanner::GameSaturatesGpu(load, busy);
        const auto g = ModelPlanner::PlanGaming(hw, busy);
        std::printf("  carte 3D %5.0f %% -> %s\n", load * 100, g.why.c_str());
    }
}
