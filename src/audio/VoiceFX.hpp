#pragma once
// VoiceFX : rack DSP "8-bit" appliqué à la voix TTS, temps réel, sans allocation.
//
//   out = dry + wet × ( BP(crush(dry)) − BP(dry) )        BP = passe-bande HPF→LPF identiques
//
//   Dans la bande (300 Hz–4,5 kHz) : fondu dry/8-bit à `wet`. Hors bande : voix intacte.
//   Les deux voies passent par des filtres identiques (même phase) : pas d'annulation,
//   les graves de la voix (fondamentale 100–250 Hz) gardent leur niveau.
//
// Corrections par rapport à l'extrait du cahier des charges :
//  - Décimation à pas fractionnaire (accumulateur de phase) : le taux cible se module en continu
//    avec l'humeur, sans saut audible, et ne dépend plus d'un facteur entier.
//  - Compteur d'échantillons sans débordement (l'int de l'extrait débordait après ~12 h à 48 kHz : UB).
//  - Quantification bipolaire correcte : 2^(bits-1) niveaux par demi-onde.
//  - Passe-bande sur la voie wet (spécifié dans le prompt maître, absent de l'extrait), en
//    soustrayant la même bande du dry : un mix parallèle naïf (dry×0,65 + BP(wet)×0,35)
//    perd jusqu'à −4 dB sous 300 Hz.
//  - Paramètres lissés par échantillon : pas de "zipper noise" quand l'humeur change.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <span>

namespace companion {

struct VoiceFXConfig {
    // Humeur 0 = voix quasi propre, 1 = crunch maximal, dans les plages du rapport
    // (8–10 bits, 12–16 kHz). Humeur neutre 0,5 = réglage nominal du rapport :
    // 9 bits, 35 % wet / 65 % dry.
    float bitDepthCalm = 10.0f, bitDepthIntense = 8.0f;
    float rateCalmHz = 16000.0f, rateIntenseHz = 12000.0f;
    float wetCalm = 0.25f, wetIntense = 0.45f;
    float bandLowHz = 300.0f, bandHighHz = 4500.0f;
    float smoothingMs = 40.0f;
};

class Biquad {
public:
    static Biquad LowPass(float fs, float fc, float q = std::numbers::sqrt2_v<float> / 2.f) noexcept {
        const auto [c, alpha] = Prewarp(fs, fc, q);
        return Normalize((1.f - c) * 0.5f, 1.f - c, (1.f - c) * 0.5f, 1.f + alpha, -2.f * c, 1.f - alpha);
    }
    static Biquad HighPass(float fs, float fc, float q = std::numbers::sqrt2_v<float> / 2.f) noexcept {
        const auto [c, alpha] = Prewarp(fs, fc, q);
        return Normalize((1.f + c) * 0.5f, -(1.f + c), (1.f + c) * 0.5f, 1.f + alpha, -2.f * c, 1.f - alpha);
    }
    float Process(float x) noexcept {  // forme directe II transposée
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }
    void Reset() noexcept { z1_ = z2_ = 0.f; }

private:
    struct Coefs { float cosw, alpha; };
    static Coefs Prewarp(float fs, float fc, float q) noexcept {
        const float w0 = 2.f * std::numbers::pi_v<float> * std::clamp(fc, 10.f, fs * 0.45f) / fs;
        return {std::cos(w0), std::sin(w0) / (2.f * q)};
    }
    static Biquad Normalize(float b0, float b1, float b2, float a0, float a1, float a2) noexcept {
        Biquad f;
        f.b0_ = b0 / a0; f.b1_ = b1 / a0; f.b2_ = b2 / a0; f.a1_ = a1 / a0; f.a2_ = a2 / a0;
        return f;
    }
    float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f, a1_ = 0.f, a2_ = 0.f, z1_ = 0.f, z2_ = 0.f;
};

class RetroVoiceProcessor {
public:
    void Prepare(float sampleRate, const VoiceFXConfig& cfg = {}) noexcept {
        cfg_ = cfg;
        sampleRate_ = sampleRate;
        invSampleRate_ = 1.f / sampleRate;
        smoothCoef_ = 1.f - std::exp(-1.f / (cfg.smoothingMs * 0.001f * sampleRate));
        hpf_ = hpfDry_ = Biquad::HighPass(sampleRate, cfg.bandLowHz);
        lpf_ = lpfDry_ = Biquad::LowPass(sampleRate, cfg.bandHighHz);
        const float m = mood_.load(std::memory_order_relaxed);
        bits_ = Lerp(cfg_.bitDepthCalm, cfg_.bitDepthIntense, m);
        rate_ = std::min(Lerp(cfg_.rateCalmHz, cfg_.rateIntenseHz, m), sampleRate_);
        wet_ = Lerp(cfg_.wetCalm, cfg_.wetIntense, m);
        phase_ = 1.f;  // force une capture au premier échantillon
        held_ = 0.f;
    }

    // Thread-safe : appelable depuis n'importe quel thread (orchestrateur, UI...).
    void SetMood(float mood01) noexcept { mood_.store(std::clamp(mood01, 0.f, 1.f), std::memory_order_relaxed); }
    void SetBypass(bool b) noexcept { bypass_.store(b, std::memory_order_relaxed); }

    // Traite un bloc mono en place. Retourne le RMS du bloc (pour l'animation de l'avatar).
    float Process(std::span<float> mono) noexcept {
        if (mono.empty()) return 0.f;
        const float mood = mood_.load(std::memory_order_relaxed);
        const bool bypass = bypass_.load(std::memory_order_relaxed);
        const float tBits = Lerp(cfg_.bitDepthCalm, cfg_.bitDepthIntense, mood);
        const float tRate = std::min(Lerp(cfg_.rateCalmHz, cfg_.rateIntenseHz, mood), sampleRate_);
        const float tWet = bypass ? 0.f : Lerp(cfg_.wetCalm, cfg_.wetIntense, mood);

        double sumSq = 0.0;
        for (float& s : mono) {
            bits_ += (tBits - bits_) * smoothCoef_;
            rate_ += (tRate - rate_) * smoothCoef_;
            wet_ += (tWet - wet_) * smoothCoef_;

            const float dry = s;
            phase_ += rate_ * invSampleRate_;
            if (phase_ >= 1.f) {  // Sample-and-Hold : aliasing volontaire = grain console
                phase_ -= std::floor(phase_);
                const float levels = std::exp2(bits_ - 1.f);
                held_ = std::round(std::clamp(dry, -1.f, 1.f) * levels) / levels;
            }
            const float crushedBand = lpf_.Process(hpf_.Process(held_));
            const float dryBand = lpfDry_.Process(hpfDry_.Process(dry));
            const float out = std::clamp(dry + wet_ * (crushedBand - dryBand), -1.f, 1.f);
            s = out;
            sumSq += static_cast<double>(out) * out;
        }
        return static_cast<float>(std::sqrt(sumSq / static_cast<double>(mono.size())));
    }

private:
    static constexpr float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

    VoiceFXConfig cfg_{};
    float sampleRate_ = 48000.f, invSampleRate_ = 1.f / 48000.f, smoothCoef_ = 0.001f;
    float bits_ = 9.f, rate_ = 16000.f, wet_ = 0.35f;
    float phase_ = 1.f, held_ = 0.f;
    Biquad hpf_, lpf_, hpfDry_, lpfDry_;
    std::atomic<float> mood_{0.5f};  // neutre = nominal du rapport
    std::atomic<bool> bypass_{false};
};

}  // namespace companion
