#include "audio/Expressivity.hpp"
#include "audio/VoiceColor.hpp"
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
static std::vector<float> Load(const char* f) { std::vector<float> x; FILE* p = std::fopen(f, "rb"); float v; while (std::fread(&v, 4, 1, p) == 1) x.push_back(v); std::fclose(p); return x; }
static void Save(const char* f, const std::vector<float>& x) { FILE* p = std::fopen(f, "wb"); std::fwrite(x.data(), 4, x.size(), p); std::fclose(p); }
int main(int, char** argv) {
    const int rate = 24000;
    const auto line = Load(argv[1]);
    std::vector<float> demo, c; c.reserve(rate * 2); std::uint32_t rng = 5;
    const auto gap = [&](float s) { demo.insert(demo.end(), size_t(s * rate), 0.f); };
    // A) voix brute, B) + micro, C) + hauteur vivante forcée (attaque + boing), D-G) les 4 bizarreries
    for (int variant = 0; variant < 7; ++variant) {
        EmotionShaper sh; sh.Set(SpeakingStyle::Normal, 1.f); sh.Reset();
        VoiceColor col; col.Prepare(rate); col.SetMic(variant == 0 ? 0.f : 0.8f);
        if (variant == 2) { std::uint32_t r = 3; sh.PlanPitch(SpeakingStyle::Bright, 40.f, 1.f, r); }
        if (variant >= 3) col.PlanWeirdForced(static_cast<VoiceColor::Weird>(variant - 2), size_t(0.9 * rate), size_t(1.2 * rate), 99);
        for (size_t i = 0; i < line.size(); i += 1920) {
            c.assign(line.begin() + i, line.begin() + std::min(line.size(), i + 1920));
            sh.Process(c); col.Process(c.data(), c.size()); demo.insert(demo.end(), c.begin(), c.end());
        }
        gap(0.7f);
    }
    Save(argv[2], demo);
    // Coût et allocations : chaîne complète (hauteur + bugs + bizarrerie + micro + grain 8-bit), pire cas
    EmotionShaper sh; RobotGlitcher g; g.Prepare(rate); VoiceColor col; col.Prepare(rate); col.SetMic(1.f);
    RetroVoiceProcessor fx; fx.Prepare(float(rate)); fx.SetMood(0.3f);
    std::vector<float> voice; for (int k = 0; k < 4; ++k) voice.insert(voice.end(), line.begin(), line.end());
    auto run = [&](bool measure) {
        double audio = 0; auto t0 = std::chrono::steady_clock::now(); long a0 = gAllocs.load();
        for (int rep = 0; rep < 10; ++rep) {
            sh.Set(SpeakingStyle::Bright, 1.2f); sh.Reset(); sh.PlanPitch(SpeakingStyle::Bright, 40.f, 1.f, rng);
            g.Plan(SpeakingStyle::Bright, 1.f, rng, true, true, 1.f);
            col.PlanWeirdForced(VoiceColor::Weird::Metal, rate, rate * 2, rng);
            for (size_t i = 0; i < voice.size(); i += 1920) {
                c.assign(voice.begin() + i, voice.begin() + std::min(voice.size(), i + 1920));
                sh.Process(c); g.Process(c); col.Process(c.data(), c.size()); fx.Process(c); audio += c.size() / double(rate);
            }
        }
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (measure) std::printf("Chaîne complète : %.0f µs de CPU par seconde de voix (%.3f %% d'un cœur), allocations en régime établi : %ld\n",
                                 cpu / audio * 1e6, cpu / audio * 100, gAllocs.load() - a0);
    };
    run(false); run(true);
}
