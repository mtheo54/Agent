#pragma once
// ModelPlanner : quel cerveau (et quelle voix) pour CE PC, selon la règle de l'utilisateur :
//
//   Hors jeu : le compagnon (cerveau + voix neuronale) prend AU PLUS LA MOITIÉ de la VRAM.
//   En jeu   : le cerveau prend AU PLUS UN QUART de la VRAM, et moins si le jeu en occupe déjà
//              beaucoup (VRAM encore disponible mesurée en direct, marge de 1 Go laissée au jeu).
//   Petite carte : on descend jusqu'à Qwen3.5 0.8B ; en dessous, le cerveau passe sur le CPU.
//
// Logique pure (aucun appel système) : la mesure du matériel est faite à part (HardwareProbe), ce qui
// permet de tester toutes les configurations.

#include <cstdint>
#include <string>
#include <vector>

namespace companion {

struct HardwareInfo {
    double vramGiB = 0.0;       // mémoire vidéo dédiée de la meilleure carte (0 = pas de vraie carte)
    double vramFreeGiB = -1.0;  // encore disponible maintenant (-1 = inconnu)
    double ramGiB = 8.0;        // mémoire vive
    std::string gpuName;
};

struct ModelInfo {
    const char* id;    // "qwen3.5-4b"
    const char* name;  // « Qwen3.5 4B »
    const char* file;  // nom du fichier dans assets/models
    const char* url;   // téléchargement (Hugging Face)
    std::uint64_t bytes;
    double vramNeedGiB;  // fichier + contexte + tampons de calcul
};

struct BrainChoice {
    const ModelInfo* model = nullptr;
    bool gpu = false;
    std::string why;  // explication lisible (journal, réglages)
};

struct VoicePlan {
    bool neural = false;  // voix neuronale (≈ 1,5 Go de VRAM) ou Piper (CPU)
    const char* whisperFile = "ggml-base-q5_1.bin";
};

struct Download {
    std::string file, url;
    std::uint64_t bytes = 0;
};

class ModelPlanner {
public:
    static const std::vector<ModelInfo>& Brains();  // du plus petit au plus gros
    static const ModelInfo* FindByFile(const std::string& file);

    static constexpr double kNeuralVoiceGiB = 1.5;
    static constexpr double kNormalShare = 0.5;    // hors jeu : moitié de la VRAM au plus (cerveau + voix)
    static constexpr double kGamingShare = 0.25;   // en jeu : un quart au plus (cerveau seul)
    static constexpr double kGameMarginGiB = 1.0;  // en jeu : marge laissée au jeu sur la VRAM libre

    static VoicePlan PlanVoice(const HardwareInfo& hw, bool neuralWanted);
    static BrainChoice PlanNormal(const HardwareInfo& hw, bool neuralVoice);
    static BrainChoice PlanGaming(const HardwareInfo& hw);
    // Fichiers à télécharger pour ce plan (cerveaux normal + jeu, Whisper, Piper, voix neuronale).
    static std::vector<Download> Required(const HardwareInfo& hw, bool neuralWanted);

private:
    static BrainChoice Fit(double budgetGiB, const HardwareInfo& hw, bool gaming, const std::string& rule);
    static BrainChoice OnCpu(const HardwareInfo& hw, bool gaming, const std::string& rule);
};

}  // namespace companion
