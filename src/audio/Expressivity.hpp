#pragma once
// Expressivité : ce qui rend la voix humaine… et un peu robot.
//
//  - Balises dans le texte (écrites par le modèle ou la banque de répliques) : (rire), (petit rire),
//    (soupir), (hmm), (tousse), (se racle la gorge), (oh), (bug). Rires, toux, raclements et soupirs sont de
//    vrais enregistrements humains (VocalSound, CC BY-SA) ; Jouées avec de vrais sons dans la voix d'Omnis
//    (assets/voice/<voix>/vocal/<type>_<n>.wav), jamais prononcées. Une *action* inconnue est retirée.
//  - Spontanéité : un petit rire avant une réponse drôle, un « oh ! » avant une surprise, sans balise.
//  - Bugs de robot : de temps en temps une syllabe accroche (« je s-s-suis »), grain numérique qui monte,
//    parfois un ralenti façon bande. Rare, placé dans la phrase, dosé par l'émotion : jamais quand il
//    console ou calme quelqu'un.

#include "audio/SpeakingStyle.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace companion {

enum class Vocal : std::uint8_t { Laugh, Chuckle, Sigh, Hmm, Cough, Gasp, Throat, Glitch, Count };
const char* ToFileStem(Vocal v) noexcept;  // "laugh", "chuckle"…

struct SpeechPart {
    bool isVocal = false;
    Vocal vocal = Vocal::Laugh;
    std::string text;
};

// Découpe « (rire) Non mais t'es sérieux ? (tousse) Pardon. » en morceaux texte / son.
std::vector<SpeechPart> ParseExpressiveText(const std::string& text);

class VocalBank {
public:
    bool Load(const std::filesystem::path& dir, int sampleRate);  // fichiers <type>_<n>.wav
    // Une prise au hasard, jamais deux fois la même d'affilée ; nullptr si aucune.
    const std::vector<float>* Pick(Vocal v, std::uint32_t& rng);
    [[nodiscard]] bool Has(Vocal v) const { return !clips_[static_cast<size_t>(v)].empty(); }

private:
    std::array<std::vector<std::vector<float>>, static_cast<size_t>(Vocal::Count)> clips_{};
    std::array<size_t, static_cast<size_t>(Vocal::Count)> last_{};
};

// Bugs de robot appliqués à l'audio au fil du streaming. Plusieurs par phrase, de 9 types :
//   Stutter   syllabe qui accroche (« je s-s-suis »), de plus en plus numérique
//   TapeStop  la bande ralentit et s'arrête, puis repart
//   Chipmunk  bout de syllabe rejoué une octave plus haut, en accéléré
//   Dropout   coupure de buffer (30-90 ms de silence)
//   Skip      le flux saute 40-120 ms en avant
//   BitBurst  éclat numérique : 4-6 bits, échantillons tenus
//   Warble    la voix ondule (vibrato numérique 9-14 Hz)
//   Reverse   un bout de syllabe à l'envers
//   RingMod   flash « voix de robot » (modulation en anneau 40-90 Hz)
// Coût : O(n) par morceau, en place, aucune allocation après le premier morceau (tampons réservés).
class RobotGlitcher {
public:
    enum class Kind : std::uint8_t { Stutter, TapeStop, Chipmunk, Dropout, Skip, BitBurst, Warble, Reverse, RingMod, Count };

    void Prepare(int sampleRate);
    // Nouvelle phrase : décide combien de bugs, lesquels et où. quirks 0 … 1 (réglage TICS & BUGS),
    // force = balise (bug), allowBig = les gros bugs (accroche, bande, accéléré) ne sont pas en pause,
    // load = charge CPU 0 … 1 (un PC qui rame fait bugger le robot).
    void Plan(SpeakingStyle style, float quirks, std::uint32_t& rng, bool force = false, bool allowBig = true, float load = 0.f);
    // Traite un morceau du flux, dans l'ordre ; peut l'allonger ou le raccourcir.
    void Process(std::vector<float>& chunk);
    // Un seul bug d'un type donné, à une position donnée (balise (bug), démos, tests).
    void PlanSingle(Kind kind, std::size_t atSample, std::uint32_t seed);
    [[nodiscard]] bool Pending() const noexcept { return next_ < count_; }
    [[nodiscard]] bool UsedBig() const noexcept { return usedBig_; }
    [[nodiscard]] int Planned() const noexcept { return count_; }

    // Effet seul (balise (bug) en fin de phrase, ou après une toux) : grésillement court.
    static std::vector<float> Burst(std::span<const float> source, int sampleRate, std::uint32_t& rng);

private:
    struct Event {
        std::size_t at = 0;  // position dans la phrase (échantillons d'entrée)
        Kind kind = Kind::Stutter;
        std::uint16_t lenMs = 60;
        std::uint8_t reps = 2;
        std::uint32_t seed = 1;
    };
    static constexpr int kMaxEvents = 6;
    std::array<Event, kMaxEvents> events_{};
    int count_ = 0, next_ = 0;
    bool usedBig_ = false;
    std::size_t seen_ = 0;
    int rate_ = 24000;
    std::vector<float> grain_, scratch_;  // réservés dans Prepare()

    [[nodiscard]] std::size_t Samples(float ms) const noexcept { return static_cast<std::size_t>(ms * 0.001f * static_cast<float>(rate_)); }
    std::size_t FindOnset(const std::vector<float>& x, std::size_t from) const noexcept;
    void Apply(std::vector<float>& chunk, std::size_t pos, const Event& e);
};

// Exagération des émotions sur la voix synthétisée, au fil du streaming : un humain excité parle plus vite,
// plus haut et plus fort ; triste ou doux, plus lentement, plus bas, plus doucement. « Varispeed » (comme
// une bande qui tourne plus vite) : vitesse et hauteur bougent ensemble, ce qui est exactement ce que
// fait la voix humaine sous l'émotion, sans artefact de time-stretch.
class EmotionShaper {
public:
    // intensity 0 … 1.5 (réglage EXPRESSIVITÉ) ; 0 = voix brute.
    void Set(SpeakingStyle style, float intensity) noexcept;
    // Vie dans la hauteur, par phrase : dérive lente et aléatoire (± depth cents) et, de temps en temps, un
    // virage (« boing » qui monte et redescend, plongée, attaque par en dessous). depth 0 = désactivé.
    void PlanPitch(SpeakingStyle style, float depthCents, float bendChance, std::uint32_t& rng) noexcept;
    void Reset() noexcept { pos_ = 0.0; prev_ = 0.f; havePrev_ = false; t_ = 0; }
    void Process(std::vector<float>& chunk);  // peut changer la longueur du morceau

private:
    double ratio_ = 1.0;  // > 1 : plus vite et plus aigu
    float gain_ = 1.f;
    std::vector<float> out_;  // tampon réutilisé (échangé avec le morceau : aucune allocation en régime établi)
    double pos_ = 0.0;    // position fractionnaire dans le flux d'entrée (relative au morceau courant)
    // Hauteur vivante
    float depth_ = 0.f, driftNow_ = 0.f, driftTarget_ = 0.f, driftSlew_ = 0.f;
    std::size_t t_ = 0, nextDrift_ = 0;
    std::uint32_t prng_ = 1;
    struct Bend { std::size_t at = 0; float cents = 0.f, upSec = 0.1f, downSec = 0.2f; };
    std::array<Bend, 2> bends_{};
    int nBends_ = 0;
    int rate_ = 24000;
    [[nodiscard]] float CentsAt(std::size_t t) noexcept;
    float prev_ = 0.f;
    bool havePrev_ = false;
};

// Probabilités de spontanéité selon le style (pas de rire si on console, etc.).
struct Spontaneity {
    static float GlitchChance(SpeakingStyle s, float quirks) noexcept;
    static float CoughChance(SpeakingStyle s, float quirks) noexcept;
    // Son à placer avant la phrase sans balise (petit rire si amusé, « oh » si surpris…), ou Count.
    static Vocal Lead(SpeakingStyle s, std::uint32_t& rng) noexcept;
};

inline float NextRandom(std::uint32_t& rng) noexcept {  // [0, 1)
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>(rng >> 8) / 16777216.f;
}

}  // namespace companion
