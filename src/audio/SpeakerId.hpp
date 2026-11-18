#pragma once
// SpeakerId : empreinte d'une voix (512 nombres) avec CAM++ (3D-Speaker, Apache 2.0, entraîné sur VoxCeleb),
// en ONNX. Mesuré sur 37 voix françaises, phrases de 2 s passées par le codec Opus de Discord :
// même voix ≥ 0,60 (moyenne 0,74), voix différentes ≤ 0,51 (moyenne 0,17) ; dans un call de 5, la bonne
// personne est retrouvée 99 % du temps (tools/sim, voir README). ~30 Mo, ~30 ms de calcul par phrase sur 1 cœur.
//
// Aucune voix n'est enregistrée : seule l'empreinte (des nombres, pas de son) est gardée dans la fiche.

#include "audio/SpeakerFeatures.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace companion {

class SpeakerId {
public:
    static constexpr const char* kModelFile = "3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx";
    static constexpr const char* kModelUrl =
        "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
        "3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx";
    static constexpr unsigned long long kModelBytes = 29596978ull;
    static constexpr float kSame = 0.55f;    // au-dessus : c'est elle (0 % de fausses reconnaissances mesurées)
    static constexpr float kOther = 0.45f;   // en dessous : quelqu'un d'autre (entre les deux : pas sûr)

    SpeakerId();
    ~SpeakerId();
    bool Load(const std::filesystem::path& model);
    [[nodiscard]] bool Ready() const noexcept;
    // Phrase 16 kHz mono d'au moins 1,5 s -> empreinte normalisée (longueur 1).
    std::optional<std::vector<float>> Embed(const std::vector<float>& pcm16k);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    KaldiFbank fbank_;
};

}  // namespace companion
