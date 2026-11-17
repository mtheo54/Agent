#include "audio/VoiceColor.hpp"

#include "audio/Expressivity.hpp"  // NextRandom

#include <algorithm>
#include <cmath>
#include <numbers>

namespace companion {
namespace {
constexpr float kPi = std::numbers::pi_v<float>;
inline float DbToLin(float db) noexcept { return std::pow(10.f, db / 20.f); }
// Limiteur doux : linéaire jusqu'à -2 dBFS, puis la crête s'arrondit vers 1 sans jamais l'atteindre.
inline float SoftLimit(float y) noexcept {
    constexpr float knee = 0.794f, room = 1.f - knee;
    const float a = std::abs(y);
    if (a <= knee) return y;
    return std::copysign(knee + room * std::tanh((a - knee) / room), y);
}
}  // namespace

// --- Filtres RBJ ---------------------------------------------------------------------------------
VoiceColor::Biquad VoiceColor::HighPass(float fs, float f, float q) {
    const float w = 2 * kPi * f / fs, c = std::cos(w), a = std::sin(w) / (2 * q), a0 = 1 + a;
    return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - a) / a0};
}
VoiceColor::Biquad VoiceColor::LowPass(float fs, float f, float q) {
    const float w = 2 * kPi * f / fs, c = std::cos(w), a = std::sin(w) / (2 * q), a0 = 1 + a;
    return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - a) / a0};
}
VoiceColor::Biquad VoiceColor::Peak(float fs, float f, float q, float db) {
    const float A = std::pow(10.f, db / 40.f), w = 2 * kPi * f / fs, c = std::cos(w), a = std::sin(w) / (2 * q);
    const float a0 = 1 + a / A;
    return {(1 + a * A) / a0, -2 * c / a0, (1 - a * A) / a0, -2 * c / a0, (1 - a / A) / a0};
}
VoiceColor::Biquad VoiceColor::LowShelf(float fs, float f, float db) {
    const float A = std::pow(10.f, db / 40.f), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const float al = s / 2 * std::sqrt(2.f), sq = 2 * std::sqrt(A) * al, a0 = (A + 1) + (A - 1) * c + sq;
    return {A * ((A + 1) - (A - 1) * c + sq) / a0, 2 * A * ((A - 1) - (A + 1) * c) / a0, A * ((A + 1) - (A - 1) * c - sq) / a0,
            -2 * ((A - 1) + (A + 1) * c) / a0, ((A + 1) + (A - 1) * c - sq) / a0};
}
VoiceColor::Biquad VoiceColor::HighShelf(float fs, float f, float db) {
    const float A = std::pow(10.f, db / 40.f), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const float al = s / 2 * std::sqrt(2.f), sq = 2 * std::sqrt(A) * al, a0 = (A + 1) - (A - 1) * c + sq;
    return {A * ((A + 1) + (A - 1) * c + sq) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0, A * ((A + 1) + (A - 1) * c - sq) / a0,
            2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - sq) / a0};
}

// --- Préparation --------------------------------------------------------------------------------
void VoiceColor::Prepare(int sampleRate) {
    rate_ = sampleRate;
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(rate_ / 20)) size <<= 1;  // ≥ 50 ms
    delay_.assign(size, 0.f);
    mask_ = size - 1;
    const float fs = static_cast<float>(rate_);
    tapSamples_ = {static_cast<std::size_t>(0.0097f * fs), static_cast<std::size_t>(0.0143f * fs), static_cast<std::size_t>(0.0211f * fs)};
    atk_ = std::exp(-1.f / (0.005f * fs));
    rel_ = std::exp(-1.f / (0.09f * fs));
    radioHp_ = HighPass(fs, 450.f, 0.8f);
    radioLp_ = LowPass(fs, 2900.f, 0.8f);
    SetMic(mic_);
}

void VoiceColor::SetMic(float amount) noexcept {
    mic_ = std::clamp(amount, 0.f, 1.f);
    const float fs = static_cast<float>(rate_);
    hp_ = HighPass(fs, 70.f, 0.7f);
    prox_ = LowShelf(fs, 160.f, 3.5f * mic_);       // effet de proximité : on parle près du micro
    presence_ = Peak(fs, 5000.f, 0.9f, 3.f * mic_);  // bosse de présence des micros dynamiques voix
    top_ = HighShelf(fs, 11000.f, -3.f * mic_);      // le haut s'adoucit (capsule dynamique)
    makeup_ = DbToLin(2.5f * mic_);
    drive_ = 1.f + 1.2f * mic_;
    driveNorm_ = 1.f / std::tanh(drive_);
}

// --- Planification des bizarreries -------------------------------------------------------------
void VoiceColor::PlanWeird(SpeakingStyle style, float quirks, std::uint32_t& rng) noexcept {
    t_ = 0;
    kind_ = Weird::None;
    float f = 1.f;
    switch (style) {
        case SpeakingStyle::Soft: case SpeakingStyle::Calm: return;  // jamais quand il console ou calme
        case SpeakingStyle::Surprised: case SpeakingStyle::Amused: f = 1.4f; break;
        case SpeakingStyle::Annoyed: f = 1.2f; break;
        default: break;
    }
    if (NextRandom(rng) >= std::clamp(quirks, 0.f, 1.f) * 0.3f * f) return;  // défaut : ~1 phrase sur 7
    const float r = NextRandom(rng);
    const Weird k = r < 0.3f ? Weird::Double : r < 0.55f ? Weird::Flanger : r < 0.8f ? Weird::Metal : Weird::Radio;
    const std::size_t at = static_cast<std::size_t>((0.2f + NextRandom(rng) * 1.8f) * static_cast<float>(rate_));
    const std::size_t len = static_cast<std::size_t>((0.4f + NextRandom(rng) * 0.8f) * static_cast<float>(rate_));
    PlanWeirdForced(k, at, len, rng);
}

void VoiceColor::PlanWeirdForced(Weird kind, std::size_t atSample, std::size_t lenSamples, std::uint32_t seed) noexcept {
    t_ = 0;
    kind_ = kind;
    start_ = atSample;
    len_ = lenSamples;
    std::uint32_t rng = seed | 1u;
    param_ = 180.f + 140.f * NextRandom(rng);  // Metal : fréquence de résonance (Hz)
    combFb_ = 0.72f;
    radioHp_.z1 = radioHp_.z2 = radioLp_.z1 = radioLp_.z2 = 0.f;
}

float VoiceColor::Tap(float d) const noexcept {
    const float pos = static_cast<float>(w_ + delay_.size()) - d;
    const std::size_t i = static_cast<std::size_t>(pos);
    const float f = pos - static_cast<float>(i);
    return delay_[i & mask_] * (1.f - f) + delay_[(i + 1) & mask_] * f;
}

// --- Traitement ---------------------------------------------------------------------------------
void VoiceColor::Process(float* x, std::size_t n, bool weird) noexcept {
    if (delay_.empty()) return;
    const float fs = static_cast<float>(rate_);
    const float fade = 0.03f * fs;
    const float thr = -20.f, ratio = 3.f;
    const bool useMic = mic_ > 0.001f;
    for (std::size_t i = 0; i < n; ++i, ++t_) {
        float s = x[i];
        // 1) Bizarrerie en cours (avant le micro : c'est « sa voix » qui est bizarre, le micro la capte)
        float wet = 0.f;
        if (weird && kind_ != Weird::None && t_ >= start_ && t_ < start_ + len_) {
            const float k = static_cast<float>(t_ - start_), env = std::min({1.f, k / fade, static_cast<float>(start_ + len_ - t_) / fade});
            const float tt = k / fs;
            switch (kind_) {
                case Weird::Double:  // une deuxième voix légèrement en retard et désaccordée
                    wet = s + 0.7f * Tap((0.018f + 0.004f * std::sin(2 * kPi * 0.9f * tt)) * fs);
                    break;
                case Weird::Flanger:  // balayage métallique « avion qui passe »
                    wet = 0.6f * s + 0.6f * Tap((0.001f + 0.0045f * (0.5f + 0.5f * std::sin(2 * kPi * 0.7f * tt))) * fs);
                    break;
                case Weird::Metal:  // résonance en peigne : voix de robot dans un tuyau
                    wet = s + combFb_ * Tap(fs / param_);
                    break;
                case Weird::Radio: {  // talkie-walkie : bande étroite + saturation
                    const float b = radioLp_.Run(radioHp_.Run(s));
                    wet = std::tanh(b * 3.f) * 0.6f;
                    break;
                }
                default: break;
            }
            delay_[w_ & mask_] = kind_ == Weird::Metal ? wet : s;  // le peigne se nourrit de sa sortie
            s = s * (1.f - env) + wet * env;
        } else {
            delay_[w_ & mask_] = s;
        }
        ++w_;
        if (!useMic) {
            x[i] = SoftLimit(s);
            continue;
        }
        // 2) Micro : filtres, compresseur, préampli, pièce, souffle
        float y = top_.Run(presence_.Run(prox_.Run(hp_.Run(s))));
        const float a = std::abs(y);
        env_ = a > env_ ? atk_ * env_ + (1.f - atk_) * a : rel_ * env_ + (1.f - rel_) * a;
        if ((t_ & 15u) == 0) {  // calcul de gain tous les 16 échantillons (log/pow coûteux), lissé entre deux
            const float over = 20.f * std::log10(env_ + 1e-6f) - thr;
            gainDb_ = over > 0.f ? -over * (1.f - 1.f / ratio) * mic_ : 0.f;
            gainTarget_ = DbToLin(gainDb_) * makeup_;
        }
        gainLin_ += (gainTarget_ - gainLin_) * 0.06f;
        y *= gainLin_;
        y = y * (1.f - 0.5f * mic_) + std::tanh(y * drive_) * driveNorm_ * 0.5f * mic_;  // préampli
        // premières réflexions (petite pièce traitée) : la voix n'est plus « collée » au haut-parleur
        y += mic_ * (0.14f * delay_[(w_ - tapSamples_[0]) & mask_] + 0.09f * delay_[(w_ - tapSamples_[1]) & mask_] +
                     0.056f * delay_[(w_ - tapSamples_[2]) & mask_]);
        // souffle du préampli, seulement pendant qu'il parle (porte lente), ~ -64 dBFS à fond
        gate_ = a > 0.01f ? std::min(1.f, gate_ + 0.002f) : std::max(0.f, gate_ - 0.0005f);
        noiseRng_ = noiseRng_ * 1664525u + 1013904223u;
        const float white = static_cast<float>(noiseRng_ >> 8) / 8388608.f - 1.f;
        noiseLp_ += (white - noiseLp_) * 0.3f;
        y += noiseLp_ * 0.0006f * mic_ * gate_;
        x[i] = SoftLimit(y);
    }
}

}  // namespace companion
