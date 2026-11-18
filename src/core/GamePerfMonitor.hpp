#pragma once
// GamePerfMonitor : mesure, pendant une partie, ce que le jeu tourne et ce qu'Omnis lui coûte.
//
//  - FPS et 1 % low du jeu : événements « Present » de DXGI (D3D11 / D3D12) reçus par ETW, la même source
//    que PresentMon, CapFrameX ou les overlays AMD / NVIDIA. Rien n'est injecté dans le jeu, aucune lecture
//    de sa mémoire : compatible anti-triche (Vanguard, VAC). Démarrer une session ETW demande d'être admin
//    ou membre du groupe « Utilisateurs du journal de performances » (voir README) ; sinon : pas de FPS,
//    mais le reste de la mesure fonctionne.
//  - Chaque image est rangée « Omnis au repos » ou « Omnis travaille » (cerveau qui génère, voix qui parle) :
//    la différence entre les deux, c'est son impact réel sur TA partie.
//  - Processeur et carte graphique (3D) du jeu et d'Omnis, chaque seconde (GetProcessTimes, compteurs PDH).
//  - Fichiers : perf-<jeu>-<date>.csv (une ligne par seconde) et perf-<jeu>-<date>.txt (résumé de la partie)
//    dans %LOCALAPPDATA%\CompanionAI\perf.
//
// Coût : un thread qui dort (ETW livre ~240 événements/s par paquets), un échantillonnage par seconde.

#include "core/PerfStats.hpp"

#include <windows.h>
#include <evntrace.h>
#include <pdh.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct _EVENT_RECORD;

namespace companion {

struct PerfSession {
    std::wstring game;
    FrameStats calm, busy;
    double seconds = 0.0;
    double busySeconds = 0.0;
    double selfCpuAvg = 0.0, selfCpuPeak = 0.0;  // % du processeur entier
    double selfGpuAvg = 0.0, selfGpuPeak = 0.0;  // % de la 3D de la carte
    double gameCpuAvg = 0.0, gameGpuAvg = 0.0;
    bool fpsAvailable = false;
};

class GamePerfMonitor {
public:
    using BusyProbe = std::function<bool()>;            // Omnis travaille-t-il en ce moment ? (appelé à chaque image)
    using PresetName = std::function<std::string()>;    // preset en cours (colonne du CSV)

    GamePerfMonitor() = default;
    ~GamePerfMonitor();
    GamePerfMonitor(const GamePerfMonitor&) = delete;
    GamePerfMonitor& operator=(const GamePerfMonitor&) = delete;

    bool Start(DWORD gamePid, std::wstring game, std::filesystem::path reportDir, BusyProbe busy, PresetName preset);
    void Stop();  // écrit le résumé de la partie
    [[nodiscard]] bool Running() const noexcept { return running_; }
    [[nodiscard]] bool FpsAvailable() const noexcept { return fpsOk_.load(); }
    [[nodiscard]] const std::string& FpsProblem() const noexcept { return fpsProblem_; }
    [[nodiscard]] const std::wstring& Game() const noexcept { return game_; }
    [[nodiscard]] bool GameAlive() const noexcept { return gameHandle_ && WaitForSingleObject(gameHandle_, 0) == WAIT_TIMEOUT; }
    [[nodiscard]] std::filesystem::path SummaryFile() const { return summaryFile_; }

    // Fenêtre de mesure contrôlée (calibrage) : toutes les images entre Begin et End.
    void BeginWindow();
    FrameStats EndWindow();

    [[nodiscard]] PerfSession Snapshot() const;
    void SetLimits(double maxFpsDropPct, double maxLowDropPct) noexcept { maxFpsDrop_ = maxFpsDropPct; maxLowDrop_ = maxLowDropPct; }
    void ResetBusy();  // nouveau preset : on juge le nouveau sur ses propres images (le « repos » est gardé)

private:
    static void WINAPI OnEvent(_EVENT_RECORD* rec);
    void OnPresent(std::int64_t qpc);
    bool StartEtw();
    void StopEtw();
    void SamplerMain(std::stop_token st);
    void SampleGpu(double& gameGpu, double& selfGpu);
    void WriteSummary();

    DWORD pid_ = 0;
    std::wstring game_;
    BusyProbe busyProbe_;
    PresetName presetName_;
    bool running_ = false;

    // ETW
    TRACEHANDLE session_ = 0;
    TRACEHANDLE trace_ = static_cast<TRACEHANDLE>(-1);
    std::thread etwThread_;
    std::vector<BYTE> props_;
    std::atomic<bool> fpsOk_{false};
    std::string fpsProblem_;
    double qpcToMs_ = 0.0;
    std::int64_t lastQpc_ = 0;

    // Images (protégées par mutex_)
    mutable std::mutex mutex_;
    PerfSession stats_;
    double sumSelfCpu_ = 0, sumGameCpu_ = 0, sumSelfGpu_ = 0, sumGameGpu_ = 0;
    int nCpu_ = 0, nGpu_ = 0;
    double maxFpsDrop_ = 5.0, maxLowDrop_ = 10.0;
    FrameStats second_;   // la seconde en cours (ligne du CSV)
    FrameStats window_;   // calibrage
    bool windowOn_ = false;

    // Échantillonnage
    std::jthread sampler_;
    HANDLE gameHandle_ = nullptr;
    ULONGLONG lastGameCpu_ = 0, lastSelfCpu_ = 0;
    std::chrono::steady_clock::time_point lastSample_{};
    PDH_HQUERY pdhQuery_ = nullptr;
    PDH_HCOUNTER pdhCounter_ = nullptr;
    std::vector<std::byte> pdhBuffer_;
    double lastGameGpu_ = -1.0, lastSelfGpu_ = -1.0;
    unsigned tick_ = 0;
    std::FILE* csv_ = nullptr;
    std::filesystem::path summaryFile_, csvFile_;
    std::chrono::system_clock::time_point startedAt_{};
};

}  // namespace companion
