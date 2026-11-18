// Statistiques d'images (PerfStats) et presets (ModelPlanner) sur des parties simulées.
#include "core/PerfStats.hpp"
#include <cstdio>
#include <random>
using namespace companion;
static FrameStats Play(double fps, double jitter, double spikePct, double spikeMs, double seconds, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(1000.0 / fps, jitter);
    std::uniform_real_distribution<double> u(0, 100);
    FrameStats f;
    while (f.Seconds() < seconds) f.Add(u(rng) < spikePct ? spikeMs : std::max(0.5, n(rng)));
    return f;
}
int main() {
    const FrameStats calm = Play(240, 0.25, 0.3, 9.0, 60, 1);
    std::printf("Repos : %.1f FPS, 1 %% low %.1f\n", calm.AvgFps(), calm.Low1Fps());
    struct C { const char* what; GamePreset p; double fps, spikes, spikeMs; };
    std::vector<std::pair<GamePreset, PerfImpact>> res;
    for (C c : {C{"carte (4B, prio basse)", GamePreset::Gpu, 237, 0.6, 9.5}, {"processeur (2B)", GamePreset::Cpu, 226, 1.5, 11},
                C{"processeur léger (0.8B)", GamePreset::CpuLight, 236, 0.5, 9}}) {
        const PerfImpact i = Impact(calm, Play(c.fps, 0.3, c.spikes, c.spikeMs, 15, 7));
        res.emplace_back(c.p, i);
        std::printf("  %-24s %.1f FPS (-%.1f %%), 1 %% low %.1f (-%.1f %%) %s\n", c.what, i.busyFps, i.fpsDropPct, i.busyLow,
                    i.lowDropPct, TooMuch(i, 5, 10) ? "-> ÇA BOUFFE TROP" : "OK");
    }
    std::printf("Calibrage -> %s (attendu : carte, plus malin que le 0.8B à impact quasi égal)\n", ToString(ChooseCalibrated(res)));
    std::printf("Trop peu d'images -> connu=%d\n", Impact(calm, Play(240, 0.3, 0, 0, 1, 3)).known);
    std::printf("Garde-fou : carte -> %s -> %s -> %s ; processeur -> %s\n", ToString(Lighter(GamePreset::Gpu)),
                ToString(Lighter(GamePreset::GpuLight)), ToString(Lighter(GamePreset::CpuLight)), ToString(Lighter(GamePreset::Cpu)));
    for (const char* s : {"auto", "carte", "carte-léger", "GPU-light", "processeur", "cpu", "processeur-leger", "n'importe"}) {
        const auto p = ParseGamePreset(s);
        std::printf("  « %s » -> %s\n", s, p ? ToString(*p) : "(refusé)");
    }
    HardwareInfo hw; hw.vramGiB = 15.9; hw.ramGiB = 32;
    for (GamePreset p : {GamePreset::Gpu, GamePreset::GpuLight, GamePreset::Cpu, GamePreset::CpuLight})
        std::printf("  %s\n", ModelPlanner::PlanPreset(hw, p).why.c_str());
    hw.vramFreeGiB = 1.2;
    std::printf("  (gros jeu, 1,2 Go libres) %s\n", ModelPlanner::PlanPreset(hw, GamePreset::GpuLight).why.c_str());
}
