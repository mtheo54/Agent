#pragma once
// VoiceColor : ce qui donne à la voix d'Omnis un son « vrai » et un peu bizarre.
//
//  - Micro (en continu) : ce qu'un micro dynamique de studio + préampli + petite pièce font à une voix —
//    coupe-bas 70 Hz, effet de proximité (graves plus ronds), présence vers 5 kHz, haut un peu adouci,
//    compresseur 3:1, légère saturation de préampli, 3 premières réflexions de la pièce, souffle très bas
//    (seulement quand il parle). Réglage `micEffect` 0 … 1.
//  - Bizarreries (ponctuelles, au plus une par phrase, dosées par TICS & BUGS) : voix dédoublée et
//    désaccordée, flanger qui balaie, résonance métallique (filtre en peigne), talkie-walkie.
//
// Flux continu : les effets chevauchent librement les morceaux du streaming. O(n), en place, aucune
// allocation après Prepare() (ligne à retard pré-allouée).

#include "audio/SpeakingStyle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace companion {

class VoiceColor {
public:
    enum class Weird : std::uint8_t { None, Double, Flanger, Metal, Radio };

    void Prepare(int sampleRate);
    void SetMic(float amount) noexcept;  // 0 = voix brute, 1 = micro très présent
    // Nouvelle phrase : décide s'il y aura une bizarrerie, laquelle, où et combien de temps.
    void PlanWeird(SpeakingStyle style, float quirks, std::uint32_t& rng) noexcept;
    void PlanWeirdForced(Weird kind, std::size_t atSample, std::size_t lenSamples, std::uint32_t seed) noexcept;
    // Traite un morceau en place. weird = false : micro seulement (rires, toux… dans le même micro).
    void Process(float* x, std::size_t n, bool weird = true) noexcept;
    [[nodiscard]] Weird Planned() const noexcept { return kind_; }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float Run(float x) noexcept {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    static Biquad HighPass(float fs, float f, float q);
    static Biquad LowPass(float fs, float f, float q);
    static Biquad Peak(float fs, float f, float q, float db);
    static Biquad LowShelf(float fs, float f, float db);
    static Biquad HighShelf(float fs, float f, float db);

    int rate_ = 24000;
    float mic_ = 0.7f;
    // Micro
    Biquad hp_, prox_, presence_, top_;
    float gainLin_ = 1.f, gainTarget_ = 1.f;
    float env_ = 0.f, gainDb_ = 0.f, atk_ = 0.f, rel_ = 0.f, makeup_ = 1.f, drive_ = 1.f, driveNorm_ = 1.f;
    float noiseLp_ = 0.f, gate_ = 0.f;
    std::uint32_t noiseRng_ = 0x2545F491u;
    // Ligne à retard partagée (réflexions + bizarreries), puissance de 2
    std::vector<float> delay_;
    std::size_t mask_ = 0, w_ = 0;
    std::array<std::size_t, 3> tapSamples_{};
    // Bizarrerie planifiée
    Weird kind_ = Weird::None;
    std::size_t start_ = 0, len_ = 0, t_ = 0;  // t_ : échantillons écoulés depuis le début de la phrase
    float param_ = 0.f;
    Biquad radioHp_, radioLp_;
    float combFb_ = 0.f;

    [[nodiscard]] float Tap(float delaySamples) const noexcept;  // lecture fractionnaire
};

}  // namespace companion
