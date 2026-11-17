#pragma once
// Normalisation du texte avant synthèse vocale : les modèles TTS lisent mal « 2980 », « -4.1 dB » ou « 1/10 ».
// On écrit nombres, décimales, fractions et unités courantes en toutes lettres, en français.

#include <cstdint>
#include <string>

namespace companion {

std::string NumberToWordsFr(std::int64_t n);
std::string NormalizeForSpeechFr(const std::string& text);

}  // namespace companion
