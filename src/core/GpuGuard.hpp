#pragma once
// GpuGuard : mode prudent automatique quand la carte graphique fait planter Omnis.
//
// Vu sur une RX 9060 XT : à la 1re question (cerveau 9B chargé sur la carte), PC figé 2 à 5 s puis Omnis disparaît —
// le pilote AMD a été réinitialisé par Windows (calcul GPU trop long, « TDR ») et a emporté le processus.
// Au démarrage on note « session en cours » ; à la fermeture propre on l'efface. Si la note est encore là au démarrage
// suivant, la session d'avant s'est mal terminée : on passe au cran de prudence suivant (jamais en arrière tout seul) :
//   1 : optimisations Vulkan qui bloquent certains pilotes AMD coupées (coopmat, produit scalaire entier), lots courts
//   2 : cerveau sur le processeur
//   3 : + voix de secours (Piper) au lieu de la voix neuronale sur la carte
// Revenir au mode normal : effacer %LOCALAPPDATA%\CompanionAI\gpu_guard.json (ou « gpuSafeStage »: 0 + ce fichier).

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace companion {

class GpuGuard {
public:
    explicit GpuGuard(std::filesystem::path dataDir) : file_(std::move(dataDir) / L"gpu_guard.json") {}

    // À appeler une fois au démarrage : renvoie le cran à appliquer, et marque la session « en cours ».
    int Begin() {
        int stage = 0;
        bool pending = false;
        if (std::ifstream f(file_); f) {
            std::string s((std::istreambuf_iterator<char>(f)), {});
            stage = Read(s, "\"stage\":");
            pending = s.find("\"pending\":true") != std::string::npos;
        }
        escalated_ = pending && stage < 3;
        if (escalated_) ++stage;
        stage_ = std::clamp(stage, 0, 3);
        Write(true);
        return stage_;
    }
    // Fermeture propre (Ctrl+Alt+Q) : la session s'est bien terminée.
    void End() { Write(false); }
    [[nodiscard]] bool Escalated() const { return escalated_; }  // vient de monter d'un cran (à annoncer)
    [[nodiscard]] int Stage() const { return stage_; }

private:
    static int Read(const std::string& s, const char* key) {
        const auto p = s.find(key);
        return p == std::string::npos ? 0 : std::atoi(s.c_str() + p + std::char_traits<char>::length(key));
    }
    void Write(bool pending) const {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        if (std::ofstream f(file_, std::ios::trunc); f)
            f << "{\"stage\":" << stage_ << ",\"pending\":" << (pending ? "true" : "false") << "}\n";
    }
    std::filesystem::path file_;
    int stage_ = 0;
    bool escalated_ = false;
};

}  // namespace companion
