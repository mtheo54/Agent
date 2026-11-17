#pragma once
// NeuralVoice : voix neuronale expressive d'Omnis. Qwen3-TTS 0.6B (Apache 2.0) via qwentts.cpp (MIT, GGML).
//
//  - Streaming : le premier morceau d'audio sort une trame (~80 ms) après le début du décodage.
//  - Émotion : une prise de référence par style (assets/voice/<voix>/{normal,bright,soft,calm}.wav + .txt).
//    L'empreinte vocale vient toujours de « normal » (identité stable), la mélodie de la prise du style.
//  - Annulation au vol (interruption) : vérifiée à chaque pas du décodeur (~83 ms).
//  - qwentts.cpp embarque son propre ggml (GGML_MAX_NAME=128) : chargé comme DLL séparée (qwen.dll),
//    jamais lié au ggml de llama.cpp. DLL ou modèles absents → Ready() == false → repli Piper.

#include "audio/SpeakingStyle.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace companion {

struct NeuralVoiceConfig {
    std::filesystem::path library = L"qwen.dll";
    std::filesystem::path talker = L"assets/models/qwen-talker-0.6b-base-Q8_0.gguf";
    std::filesystem::path codec = L"assets/models/qwen-tokenizer-12hz-Q8_0.gguf";
    std::filesystem::path voiceDir = L"assets/voice/omnis";
    std::string device;              // "Vulkan0", "CPU"… vide = meilleur disponible
    std::string language = "French";
    float temperature = 0.8f;        // plus haut = plus vivant, moins régulier
};

class NeuralVoice {
public:
    static constexpr int kSampleRate = 24000;
    // Reçoit l'audio au fil de l'eau (mono, 24 kHz). Retourner false interrompt la synthèse.
    using ChunkFn = std::function<bool(std::span<const float>)>;

    NeuralVoice();
    ~NeuralVoice();
    NeuralVoice(const NeuralVoice&) = delete;
    NeuralVoice& operator=(const NeuralVoice&) = delete;

    bool Load(const NeuralVoiceConfig& cfg);
    [[nodiscard]] bool Ready() const noexcept;

    // Bloquant (thread de synthèse). Texte déjà normalisé ou non : on le normalise ici.
    // false : échec (l'appelant bascule sur Piper) ; true : terminé ou interrompu.
    bool Speak(const std::string& text, SpeakingStyle style, const ChunkFn& onChunk);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Lecture WAV minimale (PCM 16 bits ou float 32, mono ou stéréo → mono). Faux si format non géré.
bool ReadWavMono(const std::filesystem::path& file, std::vector<float>& out, int& sampleRate);

}  // namespace companion
