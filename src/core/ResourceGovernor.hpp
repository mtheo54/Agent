#pragma once
// ResourceGovernor : arbitre "Gaming-Safe" de toutes les tâches de fond.
//
//  - Détecte jeux / DAW (liste blanche de processus + application plein écran D3D).
//  - Mesure l'inactivité (GetLastInputInfo) et la charge CPU (GetSystemTimes) / GPU (PDH).
//  - Délivre des "baux" de travail de fond (std::stop_token) et les révoque en < 50 ms :
//    pendant un entraînement, la saisie clavier/souris est surveillée via Raw Input
//    (événementiel, pas de polling). Raw Input n'est enregistré QUE pendant un bail actif,
//    pour ne pas recevoir 1000-8000 messages/s d'une souris gaming en jeu.
//
// Aucune injection, aucun hook bas niveau, aucune lecture mémoire d'un autre processus.

#include <windows.h>
#include <pdh.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace companion {

enum class SystemMode : std::uint8_t {
    Interactive,  // utilisateur présent, aucun jeu ni DAW
    Gaming,       // jeu détecté : budget ressources minimal
    Studio,       // FL Studio (ou DAW) au premier plan / lancé
    Idle,         // inactif depuis idleThreshold
};

enum class InterruptReason : std::uint8_t { UserInput, GameLaunched, DawLaunched, VoicePause, Shutdown };

const char* ToString(SystemMode m) noexcept;
const char* ToString(InterruptReason r) noexcept;

struct GovernorConfig {
    std::chrono::milliseconds idleThreshold{std::chrono::minutes{5}};
    float maxCpuLoadForTraining = 0.25f;
    float maxGpuLoadForTraining = 0.25f;
    std::chrono::milliseconds pollInterval{1000};
    std::chrono::milliseconds processScanInterval{3000};
    std::vector<std::wstring> gameProcesses{
        L"cyberpunk2077.exe", L"cs2.exe", L"valorant-win64-shipping.exe", L"league of legends.exe"};
    std::vector<std::wstring> dawProcesses{L"fl64.exe", L"fl.exe"};
    // Toute appli D3D plein écran (exclusif ou "presentation mode") est traitée comme un jeu.
    bool treatFullscreenAsGame = true;
};

struct SystemSnapshot {
    SystemMode mode = SystemMode::Interactive;
    float cpuLoad = 0.f;   // 0..1
    float gpuLoad = -1.f;  // 0..1, -1 = inconnu (n'est échantillonné qu'en Idle)
    std::chrono::milliseconds idleFor{0};
    bool backgroundWorkActive = false;
};

class ResourceGovernor {
public:
    using ModeListener = std::function<void(SystemMode previous, SystemMode current)>;
    using InterruptListener = std::function<void(InterruptReason)>;

    explicit ResourceGovernor(GovernorConfig cfg = {});
    ~ResourceGovernor();
    ResourceGovernor(const ResourceGovernor&) = delete;
    ResourceGovernor& operator=(const ResourceGovernor&) = delete;

    // Les listeners sont appelés sur le thread du governor : ils doivent rester brefs.
    void SetModeListener(ModeListener l);
    void SetInterruptListener(InterruptListener l);

    bool Start();
    void Stop();

    [[nodiscard]] SystemMode Mode() const noexcept { return mode_.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsGaming() const noexcept { return Mode() == SystemMode::Gaming; }
    // Exécutable du jeu détecté (minuscules, ex. "cyberpunk2077.exe"), vide sinon.
    [[nodiscard]] std::wstring ActiveGame() const;
    [[nodiscard]] SystemSnapshot Snapshot() const;

    // Conditions d'auto-entraînement réunies (Idle + CPU/GPU sous les seuils) ?
    [[nodiscard]] bool CanStartAutoTraining() const;

    // Demande un bail de travail de fond. manualRequest = commande vocale "Entraîne-toi"
    // (contourne le délai d'inactivité, mais jamais la détection jeu/DAW).
    // Le token est signalé dès qu'une interruption survient ; le worker DOIT le tester
    // au moins toutes les ~10 ms (ou enregistrer un std::stop_callback).
    [[nodiscard]] std::optional<std::stop_token> BeginBackgroundWork(bool manualRequest);
    void EndBackgroundWork();

    // Commande vocale "Pause".
    void Pause() { Interrupt(InterruptReason::VoicePause); }

    // À appeler depuis un thread de travail de fond : priorité CPU, E/S et mémoire basses.
    static void EnterBackgroundPriority() noexcept;
    static void LeaveBackgroundPriority() noexcept;

private:
    void ThreadMain(std::stop_token st);
    void Tick(bool forceProcessScan);
    void Interrupt(InterruptReason reason);
    void SyncRawInputRegistration();
    void ScanProcesses();
    void SetMode(SystemMode m);
    float SampleCpuLoad();
    float SampleGpuLoad();
    static bool IsFullscreenAppRunning() noexcept;
    static std::chrono::milliseconds IdleDuration() noexcept;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    GovernorConfig cfg_;
    std::unordered_set<std::wstring> games_;
    std::unordered_set<std::wstring> daws_;

    std::jthread thread_;
    HANDLE wakeEvent_ = nullptr;
    HWND msgWindow_ = nullptr;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    bool rawInputRegistered_ = false;

    std::atomic<SystemMode> mode_{SystemMode::Interactive};
    std::atomic<bool> backgroundActive_{false};
    std::atomic<bool> foregroundChanged_{false};
    std::atomic<float> cpuLoad_{0.f};
    std::atomic<float> gpuLoad_{-1.f};
    std::atomic<std::int64_t> idleMs_{0};
    bool gameRunning_ = false;
    std::wstring activeGame_;  // protégé par mutex_
    bool dawRunning_ = false;

    mutable std::mutex mutex_;  // protège stopSource_ et les listeners
    std::stop_source stopSource_{std::nostopstate};
    ModeListener modeListener_;
    InterruptListener interruptListener_;

    // Échantillonnage CPU
    ULONGLONG prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    // Échantillonnage GPU (PDH), ouvert paresseusement
    PDH_HQUERY pdhQuery_ = nullptr;
    PDH_HCOUNTER pdhGpuCounter_ = nullptr;
    std::vector<std::byte> pdhBuffer_;

    std::chrono::steady_clock::time_point lastTick_{};
    std::chrono::steady_clock::time_point lastScan_{};

    static std::atomic<ResourceGovernor*> s_instance;
};

}  // namespace companion
