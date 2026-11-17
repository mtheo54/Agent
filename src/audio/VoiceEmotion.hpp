#pragma once
// VoiceEmotion : émotion perçue dans la VOIX de l'utilisateur (ton, intonation, énergie, débit),
// 100 % locale, sans modèle à télécharger.
//
//   PCM 16 kHz ─► trames 40 ms ─► hauteur (YIN), voisement, énergie
//               ─► caractéristiques : hauteur médiane, étendue (demi-tons), pente finale
//                  (intonation montante/descendante), énergie et sa variabilité, débit
//                  (noyaux syllabiques/s), part de pauses
//               ─► comparaison à la VOIX HABITUELLE de l'utilisateur (moyennes glissantes) :
//                  une voix forte chez quelqu'un qui parle toujours fort n'est pas de la colère
//               ─► activation (calme ↔ agité) + tonalité -> émotion + description en français
//
// La prosodie seule distingue bien l'activation, moins bien la tonalité : l'orchestrateur croise
// avec les mots de la phrase (« mdr », « ça me saoule »…) et le modèle local nuance.

#include <cstdint>
#include <span>
#include <string>

namespace companion {

struct ProsodyFeatures {
    bool valid = false;
    float durationSec = 0.f;
    float pitchMedianSt = 0.f;   // demi-tons au-dessus de 100 Hz
    float pitchRangeSt = 0.f;    // étendue P10–P90 en demi-tons
    float pitchSlopeSt = 0.f;    // pente du dernier tiers (demi-tons/s) : > 0 montante
    float energyDb = -90.f;      // énergie moyenne des trames voisées (dBFS)
    float energyStdDb = 0.f;     // variabilité de l'énergie
    float voicedRatio = 0.f;
    float syllablesPerSec = 0.f;
    float pauseRatio = 0.f;
};

enum class VoiceEmotion : std::uint8_t { Neutral, Joyful, Sad, Angry, Stressed, Tired };

struct EmotionEstimate {
    VoiceEmotion label = VoiceEmotion::Neutral;
    float arousal = 0.f;     // -1 (abattu, lent, bas) … +1 (agité, fort, rapide)
    float valence = 0.f;     // -1 … +1 (indice prosodique, faible fiabilité seul)
    float confidence = 0.f;  // 0 … 1
    std::string description; // ex. « voix plus forte et plus rapide que d'habitude »
    ProsodyFeatures features;
};

// Indice de tonalité des mots (-1 négatif … +1 positif), lexique français parlé.
float TextSentiment(const std::string& text);
// Croise l'estimation vocale avec les mots : l'activation vient de la voix, la tonalité des deux.
EmotionEstimate FuseWithText(EmotionEstimate e, const std::string& text);

const char* ToString(VoiceEmotion e) noexcept;         // "neutre", "joyeux"…
const char* ToClipName(VoiceEmotion e) noexcept;       // réaction de l'avatar

class VoiceEmotionAnalyzer {
public:
    static ProsodyFeatures Extract(std::span<const float> pcm16k);

    // Estimation relative à la voix habituelle, puis mise à jour de celle-ci.
    EmotionEstimate Analyze(std::span<const float> pcm16k);
    EmotionEstimate Estimate(const ProsodyFeatures& f) const;
    void Learn(const ProsodyFeatures& f);

    // Voix habituelle persistée (profil SQLite) : "n;pitch;range;energy;rate;energyStd".
    [[nodiscard]] std::string SaveBaseline() const;
    void LoadBaseline(const std::string& s);
    [[nodiscard]] int BaselineSamples() const noexcept { return n_; }

private:
    struct Stat {
        float mean = 0.f, var = 1.f;
        void Update(float x, float alpha) {
            const float d = x - mean;
            mean += alpha * d;
            var = (1.f - alpha) * (var + alpha * d * d);
        }
        [[nodiscard]] float Z(float x, float minStd) const;
    };
    int n_ = 0;
    // Valeurs de départ typiques (avant apprentissage) : voix parlée française moyenne.
    Stat pitch_{7.f, 16.f}, range_{7.f, 6.f}, energy_{-26.f, 25.f}, rate_{4.5f, 1.2f}, energyStd_{6.f, 4.f};
};

}  // namespace companion
