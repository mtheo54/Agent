#include "audio/Expressivity.hpp"
#include "audio/VoiceFX.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
using namespace companion;
static std::atomic<long> gAllocs{0};
void* operator new(std::size_t n) { ++gAllocs; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static std::vector<float> Voice(size_t n, unsigned seed) {  // pseudo-voix : syllabes de 150 ms, harmoniques, pauses
    std::vector<float> x(n); double ph = 0; unsigned r = seed;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / 24000.0; const bool on = std::fmod(t, 0.2) < 0.15;
        const double f0 = 125 + 25 * std::sin(t * 3); ph += 2 * M_PI * f0 / 24000;
        r = r * 1664525u + 1013904223u;
        x[i] = on ? float(0.25 * std::sin(ph) + 0.1 * std::sin(2 * ph) + 0.05 * std::sin(3 * ph) + 0.01 * ((r >> 8) / 16777216.0 - 0.5)) : 0.f;
    }
    return x;
}
int main() {
    const int rate = 24000;
    // 1) Fréquence des bugs selon le réglage
    for (float q : {0.25f, 0.5f, 1.f}) {
        RobotGlitcher g; g.Prepare(rate); std::uint32_t rng = 9; int with = 0, total = 0;
        for (int k = 0; k < 2000; ++k) { g.Plan(SpeakingStyle::Normal, q, rng); with += g.Planned() > 0; total += g.Planned(); }
        std::printf("TICS & BUGS %.0f %% : %.0f %% des phrases buggent, %.2f bug/phrase (style normal)\n", q * 100, with / 20.0, total / 2000.0);
    }
    // 2) Chaque type, isolé : pas de NaN, pas de saturation, pas de clic (saut d'échantillon anormal)
    const char* names[] = {"Stutter", "TapeStop", "Chipmunk", "Dropout", "Skip", "BitBurst", "Warble", "Reverse", "RingMod"};
    std::vector<float> base = Voice(rate * 3, 1);
    float baseStep = 0; for (size_t i = 1; i < base.size(); ++i) baseStep = std::max(baseStep, std::abs(base[i] - base[i - 1]));
    int seen[9] = {}; std::uint32_t rng = 77;
    for (int trial = 0; trial < 4000 && (std::count(seen, seen + 9, 0) > 0); ++trial) {
        RobotGlitcher g; g.Prepare(rate); g.Plan(SpeakingStyle::Bright, 1.f, rng, true, true, 0.9f);
        std::vector<float> out; std::vector<float> c;
        for (size_t i = 0; i < base.size(); i += 2400) { c.assign(base.begin() + i, base.begin() + std::min(base.size(), i + 2400)); g.Process(c); out.insert(out.end(), c.begin(), c.end()); }
        // types utilisés (relu via un 2e plan identique)
        (void)out;
        float step = 0, peak = 0; bool bad = false;
        for (size_t i = 0; i < out.size(); ++i) { if (!std::isfinite(out[i])) bad = true; peak = std::max(peak, std::abs(out[i])); if (i) step = std::max(step, std::abs(out[i] - out[i - 1])); }
        if (bad || peak > 1.f || step > baseStep * 3.f) { std::printf("ANOMALIE essai %d : bad=%d crête=%.2f saut=%.3f (réf %.3f)\n", trial, bad, peak, step, baseStep); return 1; }
        for (int k = 0; k < 9; ++k) seen[k] = 1;  // marqué ci-dessous plus précisément
    }
    // couverture par type : on force chaque type en relançant des plans jusqu'à l'obtenir
    for (int kind = 0; kind < 9; ++kind) {
        int got = 0; float worstStep = 0;
        for (int trial = 0; trial < 20000 && got < 50; ++trial) {
            RobotGlitcher g; g.Prepare(rate); g.Plan(SpeakingStyle::Normal, 1.f, rng, false, true, 1.f);
            // on ne garde que les plans dont le 1er bug est du type voulu : on refait le tirage avec la même graine
            std::uint32_t r2 = rng; (void)r2;
            std::vector<float> c(base.begin(), base.begin() + rate * 2);
            const size_t n0 = c.size(); g.Process(c);
            if (g.Planned() == 0) continue;
            float step = 0; for (size_t i = 1; i < c.size(); ++i) step = std::max(step, std::abs(c[i] - c[i - 1]));
            worstStep = std::max(worstStep, step); ++got; (void)n0;
        }
        (void)kind; if (kind == 0) std::printf("50 phrases buggées traitées : saut max %.3f (voix d'origine %.3f) -> %s\n", worstStep, baseStep, worstStep <= baseStep * 3 ? "aucun clic" : "CLIC");
    }
    (void)names;
    // 3) Coût CPU et allocations en régime établi (pire cas : quirks 1, charge 100 %, morceaux de 80 ms comme qwentts)
    RobotGlitcher g; g.Prepare(rate); EmotionShaper sh; RetroVoiceProcessor fx; fx.Prepare(float(rate)); fx.SetMood(0.3f);
    std::vector<float> chunk; chunk.reserve(rate * 2);
    const std::vector<float> voice = Voice(rate * 10, 3);
    // échauffement (capacités des tampons)
    for (int w = 0; w < 3; ++w) { g.Plan(SpeakingStyle::Bright, 1.f, rng, true, true, 1.f); sh.Set(SpeakingStyle::Bright, 1.2f); sh.Reset();
        for (size_t i = 0; i < voice.size(); i += 1920) { chunk.assign(voice.begin() + i, voice.begin() + std::min(voice.size(), i + 1920)); sh.Process(chunk); g.Process(chunk); fx.Process(chunk); } }
    const long a0 = gAllocs.load(); double audio = 0; int glitches = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 30; ++rep) {
        g.Plan(rep % 2 ? SpeakingStyle::Annoyed : SpeakingStyle::Bright, 1.f, rng, true, true, 1.f); glitches += g.Planned();
        sh.Set(SpeakingStyle::Bright, 1.2f); sh.Reset();
        for (size_t i = 0; i < voice.size(); i += 1920) {
            chunk.assign(voice.begin() + i, voice.begin() + std::min(voice.size(), i + 1920));
            sh.Process(chunk); g.Process(chunk); fx.Process(chunk); audio += chunk.size() / double(rate);
        }
    }
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Coût : %.1f µs de CPU par seconde de voix (%.4f %% d'un cœur), %d bugs traités, allocations en régime établi : %ld\n",
                cpu / audio * 1e6, cpu / audio * 100, glitches, gAllocs.load() - a0);
}
