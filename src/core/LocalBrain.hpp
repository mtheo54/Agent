#pragma once
// LocalBrain : niveau 1 de la cascade — modèle 3B Q4_K_M embarqué via llama.cpp (0 token API).
//
//  - Hors jeu : couches sur la RX 9060 XT (backend Vulkan). En jeu : rechargement CPU-only
//    (0 Mo de VRAM, rapport §2), 2 threads. Le fichier GGUF est mappé en mémoire (mmap) :
//    le rechargement relit le cache disque de Windows, ~1–2 s.
//  - Réutilisation du cache KV par préfixe commun : le prompt système (stable) n'est
//    calculé qu'une fois, seuls les nouveaux tokens sont évalués à chaque tour.
//  - Grammaire GBNF optionnelle : force le format [ESCALATE | ...] quand le modèle escalade.
//  - Vision locale (optionnelle) : avec un modèle vision-langage (ex. Qwen2.5-VL-3B) et son
//    projecteur mmproj, GenerateWithImage() analyse une capture d'écran SANS quitter le PC.
//  - Adaptateur LoRA optionnel (lora.gguf, rapport §4.E) appliqué au chargement s'il est présent.
//  - Unload() libère ~2 Go de RAM pour revenir au budget veille (< 80 Mo).
//
// Non thread-safe : à utiliser depuis un seul thread (celui de l'AgentOrchestrator).

#include "core/MemoryDB.hpp"  // ChatTurn

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct llama_model;
struct llama_context;
struct mtmd_context;

namespace companion {

struct LocalBrainConfig {
    std::filesystem::path model;
    std::filesystem::path mmproj;  // vide = pas de vision
    std::filesystem::path lora;    // adaptateur LoRA (lora.gguf) appliqué s'il existe
    int contextTokens = 4096;
    int gpuLayers = 99;
    int threads = 4;
    int ubatch = 512;  // en jeu sur la carte : 64, des calculs courts qui laissent passer les images du jeu
};

struct GenerateOptions {
    int maxTokens = 220;
    float temperature = 0.7f;
    const char* grammar = nullptr;  // GBNF, racine "root"
    // Reçoit chaque morceau de texte au fil de l'eau ; retourner false arrête la génération.
    std::function<bool(std::string_view piece)> onPiece;
    std::stop_token stop;
};

class LocalBrain {
public:
    LocalBrain();
    ~LocalBrain();
    LocalBrain(const LocalBrain&) = delete;
    LocalBrain& operator=(const LocalBrain&) = delete;

    bool Load(const LocalBrainConfig& cfg);
    void Unload();
    [[nodiscard]] bool IsLoaded() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& ModelPath() const noexcept { return cfg_.model; }
    [[nodiscard]] size_t SnapshotBytes() const noexcept { return snapState_.size(); }
    // Dossier où garder les photos d'état (une par modèle + personnalité, ~30 Mo) ; vide = mémoire seulement.
    void SetStateCacheDir(std::filesystem::path dir) { stateDir_ = std::move(dir); }
    [[nodiscard]] bool Available() const;  // fichier modèle présent et llama.cpp compilé

    // Bascule GPU <-> CPU (recharge si nécessaire). threads s'applique immédiatement.
    bool Configure(bool useGpu, int threads);
    [[nodiscard]] bool UsesGpu() const noexcept { return useGpu_; }
    [[nodiscard]] int Ubatch() const noexcept { return cfg_.ubatch; }
    // Génère en ce moment ? (lu par le moniteur de perfs à chaque image du jeu : thread-safe)
    [[nodiscard]] bool Busy() const noexcept { return busy_.load(std::memory_order_relaxed); }
    // Débit maximal (tokens/s, 0 = libre). En jeu : salves courtes et espacées, au rythme de la voix.
    void SetPacing(int tokensPerSecond) noexcept { pacingTps_ = tokensPerSecond; }

    std::optional<std::string> Generate(const std::vector<ChatTurn>& messages, const GenerateOptions& opt);

    // Vision locale. Le dernier message "user" doit contenir MediaMarker() là où va l'image.
    [[nodiscard]] bool HasVision() const;
    static const char* MediaMarker();
    std::optional<std::string> GenerateWithImage(const std::vector<ChatTurn>& messages, const unsigned char* rgb,
                                                 int width, int height, const GenerateOptions& opt);

private:
    std::string ApplyTemplate(const std::vector<ChatTurn>& messages) const;
    std::string SampleLoop(const GenerateOptions& opt, int nCtx, bool trackTokens);

    LocalBrainConfig cfg_{};
    bool useGpu_ = true;
    int pacingTps_ = 0;
    std::atomic<bool> busy_{false};
    struct BusyScope {
        std::atomic<bool>& f;
        explicit BusyScope(std::atomic<bool>& b) : f(b) { f = true; }
        ~BusyScope() { f = false; }
    };
    void Pace(std::chrono::steady_clock::time_point& next) const;
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    mtmd_context* mtmd_ = nullptr;
    std::vector<int> cachedTokens_;  // tokens présents dans le cache KV (séquence 0)
    // Modèles récurrents / hybrides (Qwen3.5…) : leur état ne se « rembobine » pas, la réutilisation du
    // préfixe échoue. On garde donc une photo de l'état juste après la personnalité (prompt système,
    // stable) et on la restaure : seule la fin du prompt est recalculée (rapport : réponses instantanées).
    bool recurrent_ = false;
    std::vector<int> snapTokens_;
    std::vector<uint8_t> snapState_;
    std::filesystem::path stateDir_;
    [[nodiscard]] std::filesystem::path SnapshotFile(const std::vector<int>& sysTokens) const;
    void SaveSnapshot() const;
    void LoadSnapshot(const std::vector<int>& sysTokens);
};

}  // namespace companion
