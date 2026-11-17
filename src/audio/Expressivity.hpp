#pragma once
// Expressivité : ce qui rend la voix humaine… et un peu robot.
//
//  - Balises dans le texte (écrites par le modèle ou la banque de répliques) : (rire), (petit rire),
//    (soupir), (hmm), (tousse), (oh), (bug). Jouées avec de vrais sons dans la voix d'Omnis
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

enum class Vocal : std::uint8_t { Laugh, Chuckle, Sigh, Hmm, Cough, Gasp, Glitch, Count };
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

// Bugs de robot appliqués à l'audio au fil du streaming.
class RobotGlitcher {
public:
    void Prepare(int sampleRate) noexcept { rate_ = sampleRate; }
    // Nouvelle phrase : décide s'il y aura un bug, et où. quirks 0 … 1 (réglage), force = balise (bug).
    void Plan(SpeakingStyle style, float quirks, std::uint32_t& rng, bool force = false);
    // Traite un morceau ; peut l'allonger (répétitions). À appeler dans l'ordre du flux.
    void Process(std::vector<float>& chunk);
    // Version « tout d'un coup » (Piper, sons) : applique le bug prévu sur le tampon entier.
    void ProcessAll(std::vector<float>& pcm) { Process(pcm); }
    [[nodiscard]] bool Pending() const noexcept { return armed_; }

    // Effet seul (pour la balise (bug) en fin de phrase, ou après une toux) : grésillement court.
    static std::vector<float> Burst(std::span<const float> source, int sampleRate, std::uint32_t& rng);

private:
    int rate_ = 24000;
    bool armed_ = false;
    std::size_t at_ = 0;    // position du bug (échantillons depuis le début de la phrase)
    std::size_t seen_ = 0;  // échantillons déjà passés
    int repeats_ = 2;
    float grainMs_ = 70.f;
    bool tapeStop_ = false;
    std::uint32_t seed_ = 1;
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
