#pragma once
#include <cstdint>

namespace companion {

// Style de diction appliqué à la synthèse. Voix neuronale : prise de référence émotionnelle
// (assets/voice/<voix>/<style>.wav) ; repli Piper : vitesse et expressivité.
enum class SpeakingStyle : std::uint8_t {
    Normal,
    Bright,  // enthousiaste : plus vif, plus expressif
    Soft,    // doux : plus lent, plus posé
    Calm,    // apaisant : posé, régulier
};

}  // namespace companion
