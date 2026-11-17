#pragma once
// Mesure du matériel pour ModelPlanner (Windows, DXGI) : VRAM dédiée de la meilleure carte, VRAM que
// Windows accorde MAINTENANT au programme (elle baisse quand un jeu gourmand tourne), RAM totale.
// Rapide (quelques ms) : appelée au démarrage et à chaque chargement du cerveau en jeu.

#include "core/ModelPlanner.hpp"

namespace companion {

HardwareInfo ProbeHardware();

}  // namespace companion
