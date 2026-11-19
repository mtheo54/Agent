#pragma once
// Décodage audio par Windows (Media Foundation : wav, mp3, m4a, mp4, mov…) -> 16 kHz mono, pour Whisper.
#include <filesystem>
#include <vector>

namespace companion {
std::vector<float> DecodeAudio16k(const std::filesystem::path& file, int maxSeconds);
}
