#pragma once
// SelfTrainer : auto-entraînement pendant l'inactivité du PC (rapport §4.E).
//
// Pourquoi pas de fine-tuning LoRA dans llama.cpp : son entraînement est expérimental
// (« very much WIP »), en pleine précision FP32 seulement, et demande ~24 Go pour un modèle 1B.
// Un 3B ne tient pas dans 32 Go. On applique donc la même idée — transformer ce qui a bloqué
// en savoir local — par DISTILLATION EN MÉMOIRE :
//
//   1. relit dans SQLite les sujets qui ont provoqué un [ESCALATE] ;
//   2. collecte web ciblée, 0 token Claude, UNIQUEMENT sur les domaines de la liste blanche que
//      tu as déjà autorisés avec « toujours » (personne n'est là pour répondre pendant ta pause) ;
//   3. le modèle local rédige une « fiche » de faits clés à partir des sources et des réponses
//      déjà validées par Claude ;
//   4. auto-évaluation : il rejoue la question d'origine avec la fiche. S'il répond sans escalader,
//      le sujet est « appris » : la prochaine fois, la réponse est 100 % locale ;
//   5. chaque exemple validé est ajouté à training/dataset.jsonl (format chat) : si tu entraînes
//      un LoRA ailleurs (GPU), dépose le lora.gguf dans assets/models et LocalBrain l'applique.
//
// Isolation (rapport §2) : la session tourne dans un PROCESSUS ENFANT (le même exécutable lancé
// avec --trainer), en IDLE_PRIORITY_CLASS + mode arrière-plan (E/S et mémoire basses), placé dans
// un Job Object avec plafond de RAM. Arrêt d'urgence : TerminateJobObject, appelé directement par
// le jeton du ResourceGovernor -> réseau coupé, RAM et CPU libérés immédiatement (< 50 ms).
// « Checkpoint » : chaque sujet est validé dans SQLite dès qu'il est fini ; une coupure perd au
// plus le sujet en cours.

#include "core/Config.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace companion {

struct TrainerOutcome {
    bool completed = false;    // session allée au bout
    bool interrupted = false;  // coupée par le governor (saisie, jeu, DAW, "pause")
    int learned = 0;           // sujets appris pendant la session
    std::string report;        // phrase à dire à l'utilisateur
};

class SelfTrainer {
public:
    using FinishedFn = std::function<void(const TrainerOutcome&)>;

    SelfTrainer(const CompanionConfig& cfg, std::filesystem::path dataDir) : cfg_(cfg), dataDir_(std::move(dataDir)) {}
    ~SelfTrainer() { Kill(); }

    // Lance la session. `token` vient de ResourceGovernor::BeginBackgroundWork : sa révocation tue
    // le processus enfant immédiatement. onFinished est appelé une fois, sur un thread interne.
    bool Start(std::stop_token token, bool useGpu, FinishedFn onFinished);
    void Kill();
    [[nodiscard]] bool Running() const noexcept { return running_.load(); }

private:
    const CompanionConfig& cfg_;
    std::filesystem::path dataDir_;
    std::atomic<bool> running_{false};
    std::jthread monitor_;
    std::mutex mutex_;
    void* job_ = nullptr;      // HANDLE
    void* process_ = nullptr;  // HANDLE
};

// Point d'entrée du processus enfant (CompanionAI.exe --trainer [--gpu]). Retourne le code de sortie.
// Portable (aucun appel Win32 direct) : testable hors Windows.
int RunTrainerSession(const CompanionConfig& cfg, const std::filesystem::path& dataDir, bool useGpu);

}  // namespace companion
