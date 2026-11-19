#include "core/ModelPlanner.hpp"
#include <cstdio>
using namespace companion;
int main() {
    struct C { double vram, ram; bool neural; const char* n; } cs[] = {{15.9, 31.2, true, "Toi : 16 Go VRAM, 32 Go RAM, voix neuronale"}, {15.9, 31.2, false, "Toi, voix Piper"},
        {8, 31.2, true, "8 Go VRAM, 32 Go RAM"}, {12, 15.8, true, "12 Go VRAM, 16 Go RAM"}, {24, 63.7, true, "24 Go VRAM, 64 Go RAM"}, {0, 31.2, false, "sans carte"}};
    for (const auto& c : cs) { HardwareInfo h; h.vramGiB = c.vram; h.ramGiB = c.ram; const auto p = ModelPlanner::PlanExpert(h, c.neural);
        std::printf("%-44s -> %s %s\n", c.n, p.possible ? "OUI" : "non", p.why.c_str()); }
}
