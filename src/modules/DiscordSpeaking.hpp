#pragma once
// DiscordSpeaking : « qui parle » lu sur l'écran, comme toi — le cercle (ou cadre) vert de Discord et le pseudo à côté.
//
//  - Fenêtre Discord capturée (PrintWindow, même partiellement cachée), 4 fois par seconde, SEULEMENT pendant que
//    quelqu'un parle dans le call (sinon zéro travail), et jamais en jeu.
//  - Marques vertes repérées (SpeakingMarks), pseudo lu à côté par UI Automation (l'accessibilité de Windows, ce
//    que lit un lecteur d'écran) — seulement si cet endroit est visible à l'écran.
//  - Rien n'est cliqué, rien n'est envoyé à Discord : il regarde, c'est tout.
//
// La chronologie (30 s) dit qui parlait pendant une phrase : le nom sert d'étiquette pour apprendre sa voix.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

class DiscordSpeaking {
public:
    using Clock = std::chrono::steady_clock;

    ~DiscordSpeaking() { Stop(); }
    // shouldSample : vrai quand ça vaut le coup de regarder (quelqu'un parle dans le call, pas en jeu).
    void Start(std::function<bool()> shouldSample);
    void Stop();

    struct Shown {
        std::string name;                 // pseudo affiché (peut changer pour une blague)
        std::vector<std::uint8_t> avatar;  // empreinte de l'avatar (vide si inconnue)
        [[nodiscard]] bool empty() const { return name.empty(); }
    };
    // Celui vu « en train de parler » pendant la plupart de [from, to] ; vide si personne ou plusieurs.
    [[nodiscard]] Shown SpeakerBetween(Clock::time_point from, Clock::time_point to) const;
    // Discord a été lu avec succès il y a peu (fenêtre visible, pseudos accessibles) ?
    [[nodiscard]] bool Seeing() const;

private:
    void Main(std::stop_token st);
    std::vector<Shown> Sample();

    std::function<bool()> should_;
    std::jthread thread_;
    mutable std::mutex mutex_;
    struct Snap {
        Clock::time_point at;
        std::vector<Shown> people;
    };
    std::deque<Snap> timeline_;
    Clock::time_point lastSeen_{};
    std::vector<std::uint8_t> pixels_;
};

}  // namespace companion
