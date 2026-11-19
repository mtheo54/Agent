#pragma once
// La voix off d'un montage, mot par mot : décodée par Windows, transcrite par Whisper avec l'instant de chaque mot.
#include "modules/Subtitles.hpp"

#include <filesystem>
#include <vector>

namespace companion {
// Temps en secondes depuis le début du FICHIER audio. Vide si Whisper ou le fichier manquent.
std::vector<TimedWord> TranscribeWords(const std::filesystem::path& audio, const std::filesystem::path& whisperModel,
                                       int maxSeconds = 1800);
// Même chose depuis un son déjà décodé (16 kHz mono).
std::vector<TimedWord> TranscribeWordsPcm(const std::vector<float>& pcm16k, const std::filesystem::path& whisperModel);
}
