#pragma once
// ModelPlanner : quel cerveau (et quelle voix) pour CE PC, selon la règle de l'utilisateur :
//
//   Hors jeu : le compagnon (cerveau + voix neuronale) prend AU PLUS LA MOITIÉ de la VRAM.
//   En jeu   : le cerveau prend AU PLUS UN QUART de la VRAM, et moins si le jeu en occupe déjà
//              beaucoup (VRAM encore disponible mesurée en direct, marge de 1 Go laissée au jeu).
//   Petite carte : on descend jusqu'à Qwen3.5 0.8B ; en dessous, le cerveau passe sur le CPU.
//
//   En jeu, CARTE ou PROCESSEUR ? On regarde ce que le jeu sature, mesuré en direct :
//     - jeu limité par la carte graphique (3D ≥ 85 %, AAA) : cerveau sur le processeur, 1 cœur isolé ;
//     - jeu limité par le processeur (LoL, Valorant, CS2 à 240 fps : la carte attend) : cerveau sur la
//       carte, en priorité GPU basse et par petits morceaux. Sur le processeur, il volerait la bande
//       passante mémoire dont un jeu à 240 fps a justement besoin.
//
// Logique pure (aucun appel système) : la mesure du matériel est faite à part (HardwareProbe), ce qui
// permet de tester toutes les configurations.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

struct HardwareInfo {
    double vramGiB = 0.0;       // mémoire vidéo dédiée de la meilleure carte (0 = pas de vraie carte)
    double vramFreeGiB = -1.0;  // encore disponible maintenant (-1 = inconnu)
    double ramGiB = 8.0;        // mémoire vive
    double gameGpuLoad = -1.0;  // en jeu : charge 3D de la carte hors compagnon, 0..1 (-1 = inconnue)
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

// Presets de jeu : où et quoi charger quand tu lui parles en partie.
enum class GamePreset : std::uint8_t {
    Auto,      // mesuré : carte ou processeur selon ce que le jeu sature (+ ce qu'Omnis a appris sur ce jeu)
    Gpu,       // carte graphique, priorité basse : modèle du quart de VRAM (4B sur 16 Go)
    GpuLight,  // carte graphique, le plus petit modèle (0.8B, ~0,9 Go)
    Cpu,       // processeur, cœur isolé : 2B (0.8B avec moins de 16 Go de RAM)
    CpuLight,  // processeur, le plus petit modèle (0.8B)
};
const char* ToString(GamePreset p) noexcept;  // "auto", "carte", "carte-leger", "processeur", "processeur-leger"
std::optional<GamePreset> ParseGamePreset(std::string_view s);  // accepte aussi gpu / cpu / -light / léger
// Un cran plus léger (garde-fou quand ça bouffe trop) ; renvoie le même s'il n'y a plus plus léger.
GamePreset Lighter(GamePreset p) noexcept;
// Rang d'intelligence (à impact égal, on garde le plus malin) : carte 4B > processeur 2B > 0.8B.
int Smartness(GamePreset p) noexcept;

struct Download {
    std::string file, url;
    std::uint64_t bytes = 0;
};

// Module de vision (mmproj) qui va avec un cerveau Qwen3.5 : il lui permet de regarder des images.
struct VisionModule {
    std::string file, url;
    std::uint64_t bytes = 0;
};

class ModelPlanner {
public:
    static std::optional<VisionModule> VisionFor(const std::string& brainFile);
    static const std::vector<ModelInfo>& Brains();  // du plus petit au plus gros
    static const ModelInfo* FindByFile(const std::string& file);

    static constexpr double kNeuralVoiceGiB = 1.5;
    static constexpr double kNormalShare = 0.5;    // hors jeu : moitié de la VRAM au plus (cerveau + voix)
    static constexpr double kGamingShare = 0.25;   // en jeu : un quart au plus (cerveau seul)
    static constexpr double kGameMarginGiB = 1.0;  // en jeu : marge laissée au jeu sur la VRAM libre
    static constexpr double kGpuBusyEnter = 0.85;  // le jeu sature la carte à partir de là…
    static constexpr double kGpuBusyLeave = 0.70;  // …et ne la sature plus sous ce seuil (hystérésis)

    // Le jeu sature-t-il la carte graphique ? wasBusy = verdict précédent (évite les allers-retours).
    // Charge inconnue : on reste prudent, le processeur (comportement historique, 0 Mo de VRAM).
    static bool GameSaturatesGpu(double gameGpuLoad, bool wasBusy);

    static VoicePlan PlanVoice(const HardwareInfo& hw, bool neuralWanted);
    static BrainChoice PlanNormal(const HardwareInfo& hw, bool neuralVoice);
    static BrainChoice PlanGaming(const HardwareInfo& hw, bool gpuSaturated = false);
    // Preset imposé (pas Auto) : carte / processeur, normal ou léger.
    static BrainChoice PlanPreset(const HardwareInfo& hw, GamePreset preset);
    // Fichiers à télécharger pour ce plan (cerveaux normal + jeu, Whisper, Piper, voix neuronale).
    static std::vector<Download> Required(const HardwareInfo& hw, bool neuralWanted);

private:
    static BrainChoice Fit(double budgetGiB, const HardwareInfo& hw, bool gaming, const std::string& rule);
    static BrainChoice OnCpu(const HardwareInfo& hw, bool gaming, const std::string& rule);
};

}  // namespace companion
