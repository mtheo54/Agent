#pragma once
// SystemControl : commandes Windows sécurisées (rapport §1 et §5).
//
//  - Volume / muet : API officielle IAudioEndpointVolume (aucune simulation de touche).
//  - Touches multimédia (lecture/pause, suivant…) : SendInput, AUTOMATIQUEMENT REFUSÉ quand
//    un jeu tourne (barrière anti-cheat du rapport : aucune entrée simulée en jeu).
//  - Lancement d'applications : uniquement celles de la liste blanche (companion.json),
//    après accord du Permission Gate (géré par l'orchestrateur).
// COM doit être initialisé sur le thread appelant.

#include "core/Config.hpp"
#include "core/ResourceGovernor.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace companion {

class SystemControl {
public:
    enum class MediaKey { PlayPause, Next, Previous };

    SystemControl(ResourceGovernor& governor, std::vector<AppShortcut> apps)
        : governor_(governor), apps_(std::move(apps)) {}

    std::optional<float> Volume();      // 0..1
    bool SetVolume(float level01);
    bool StepVolume(float delta);       // ex. +0.1
    bool SetMute(bool mute);
    bool ToggleMute();

    // false si refusé (jeu en cours) ou en échec.
    bool Media(MediaKey key);
    [[nodiscard]] bool InputSimulationAllowed() const { return !governor_.IsGaming(); }

    [[nodiscard]] const AppShortcut* FindApp(std::string_view utterance) const;
    bool Launch(const AppShortcut& app);

private:
    ResourceGovernor& governor_;
    std::vector<AppShortcut> apps_;
};

}  // namespace companion
