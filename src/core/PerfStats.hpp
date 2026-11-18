#pragma once
// PerfStats : durées d'image d'un jeu -> FPS moyen et « 1 % low », impact d'Omnis, choix d'un preset.
// Logique pure (aucun appel système) : testée hors Windows (tools/sim/perftest.cpp).
//
//   FPS moyen  = images / durée totale.
//   1 % low    = FPS correspondant au 99e centile des durées d'image : les saccades qu'on ressent.
//   Impact     = baisse en % entre les images « Omnis au repos » et « Omnis travaille ».

#include "core/ModelPlanner.hpp"  // GamePreset

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace companion {

struct FrameStats {
    static constexpr int kBins = 2000;       // cases de 0,05 ms, jusqu'à 100 ms (≥ 10 fps)
    static constexpr double kBinMs = 0.05;
    std::uint64_t frames = 0;
    double ms = 0.0;  // somme des durées d'image
    std::array<std::uint32_t, kBins> hist{};

    void Add(double frameMs) noexcept {
        if (!(frameMs > 0.0) || frameMs > 250.0) return;  // alt-tab, écran de chargement : ignoré
        ++frames;
        ms += frameMs;
        ++hist[static_cast<size_t>(std::min(kBins - 1, static_cast<int>(frameMs / kBinMs)))];
    }
    void Merge(const FrameStats& o) noexcept {
        frames += o.frames;
        ms += o.ms;
        for (int i = 0; i < kBins; ++i) hist[static_cast<size_t>(i)] += o.hist[static_cast<size_t>(i)];
    }
    void Reset() noexcept {
        frames = 0;
        ms = 0.0;
        hist.fill(0);
    }
    [[nodiscard]] double Seconds() const noexcept { return ms / 1000.0; }
    [[nodiscard]] double AvgFps() const noexcept { return ms > 0.0 ? static_cast<double>(frames) * 1000.0 / ms : -1.0; }
    [[nodiscard]] double Low1Fps() const noexcept {
        if (frames < 100) return -1.0;
        const std::uint64_t target = frames - frames / 100;
        std::uint64_t acc = 0;
        for (int i = 0; i < kBins; ++i) {
            acc += hist[static_cast<size_t>(i)];
            if (acc >= target) return 1000.0 / ((i + 0.5) * kBinMs);
        }
        return 1000.0 / (kBins * kBinMs);
    }
};

struct PerfImpact {
    bool known = false;       // assez d'images des deux côtés pour conclure
    double calmFps = -1, busyFps = -1, calmLow = -1, busyLow = -1;
    double fpsDropPct = 0.0;  // baisse du FPS moyen (0 si pas de baisse)
    double lowDropPct = 0.0;  // baisse du 1 % low
    [[nodiscard]] double Score() const noexcept { return fpsDropPct + 0.5 * lowDropPct; }
};

// Il faut ~3 s d'images de chaque côté pour que la comparaison veuille dire quelque chose.
inline PerfImpact Impact(const FrameStats& calm, const FrameStats& busy) {
    PerfImpact r;
    r.calmFps = calm.AvgFps();
    r.busyFps = busy.AvgFps();
    r.calmLow = calm.Low1Fps();
    r.busyLow = busy.Low1Fps();
    if (calm.frames < 150 || busy.frames < 150 || calm.Seconds() < 3.0 || busy.Seconds() < 3.0) return r;
    r.known = true;
    r.fpsDropPct = std::max(0.0, (r.calmFps - r.busyFps) / r.calmFps * 100.0);
    if (r.calmLow > 0 && r.busyLow > 0) r.lowDropPct = std::max(0.0, (r.calmLow - r.busyLow) / r.calmLow * 100.0);
    return r;
}

// « Ça bouffe trop » : FPS moyen en baisse de plus de maxFpsDrop %, ou saccades (1 % low) de plus de maxLowDrop %.
inline bool TooMuch(const PerfImpact& i, double maxFpsDrop, double maxLowDrop) noexcept {
    return i.known && (i.fpsDropPct > maxFpsDrop || i.lowDropPct > maxLowDrop);
}

// Calibrage : le preset au plus faible impact ; à 1,5 point près, le plus intelligent (4B carte > 2B processeur).
inline GamePreset ChooseCalibrated(const std::vector<std::pair<GamePreset, PerfImpact>>& results) {
    double best = 1e9;
    for (const auto& [p, i] : results)
        if (i.known) best = std::min(best, i.Score());
    GamePreset pick = GamePreset::Auto;
    int smart = -1;
    double pickScore = 1e9;
    for (const auto& [p, i] : results) {
        if (!i.known || i.Score() > best + 1.5) continue;
        if (Smartness(p) > smart || (Smartness(p) == smart && i.Score() < pickScore)) {
            pick = p;
            smart = Smartness(p);
            pickScore = i.Score();
        }
    }
    return pick;
}

}  // namespace companion
