// CompanionAI — point d'entrée.
//
// Étape 1 : ResourceGovernor, voix (STT/TTS + VoiceFX 8-bit), overlay transparent + avatar
//           (personnage 2D anime x cartoon à 24 i/s, décrit dans assets/avatar/rig.json).
// Étape 2 : cerveau local d'abord — AgentOrchestrator (niveaux 0 / 1 / web local / Claude),
//           LocalBrain (llama.cpp), MemoryDB (SQLite), PermissionGate, coach LoL (API locale Riot).
// Étape 3 : studio (WasapiLoopback + OmnisAnalyzer), vision (capture DXGI, locale puis Claude),
//           SystemControl (volume, musique, applis ; aucune touche simulée en jeu).
//
// Raccourcis globaux (RegisterHotKey : API officielle, aucun hook clavier) :
//   Ctrl+Alt+C : push-to-talk (bascule : appuyer pour parler, ré-appuyer pour envoyer)
//   Ctrl+Alt+V : bascule Push-to-Talk <-> détection de voix (VAD)
//   Ctrl+Alt+H : afficher / masquer l'avatar
//   Ctrl+Alt+Q : quitter

#include "audio/OmnisAnalyzer.hpp"
#include "audio/VoiceEngine.hpp"
#include "audio/WasapiLoopback.hpp"
#include "core/AgentOrchestrator.hpp"
#include "core/Config.hpp"
#include "core/LocalBrain.hpp"
#include "core/Log.hpp"
#include "core/MemoryDB.hpp"
#include "core/PermissionGate.hpp"
#include "core/ResourceGovernor.hpp"
#include "modules/GameCoach.hpp"
#include "modules/SelfTrainer.hpp"
#include "modules/SystemControl.hpp"
#include "network/ClaudeClient.hpp"
#include "network/HttpClient.hpp"
#include "network/WebResearcher.hpp"
#include "render/OverlayWindow.hpp"
#include "render/CartoonAvatar.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

using namespace companion;

namespace {

enum HotkeyId : int { kHotkeyTalk = 1, kHotkeyToggle = 2, kHotkeyQuit = 3, kHotkeyVad = 4 };

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

// %LOCALAPPDATA%\CompanionAI : données utilisateur (base SQLite), inscriptible sans droits admin.
std::filesystem::path DataDir() {
    PWSTR path = nullptr;
    std::filesystem::path dir = ExeDir() / L"data";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) dir = std::filesystem::path(path) / L"CompanionAI";
    CoTaskMemFree(path);
    return dir;
}

void SetEcoQoS(bool on) {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    // Processus enfant d'auto-entraînement (lancé par SelfTrainer dans un Job Object, priorité IDLE).
    if (cmdLine && std::wstring_view(cmdLine).find(L"--trainer") != std::wstring_view::npos) {
        SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);  // E/S et mémoire basses
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
        const auto root = ExeDir();
        CompanionConfig cfg = LoadConfig(root / L"companion.json");
        if (cfg.llmModel.is_relative()) cfg.llmModel = root / cfg.llmModel;
        if (cfg.loraAdapter.is_relative()) cfg.loraAdapter = root / cfg.loraAdapter;
        const bool gpu = std::wstring_view(cmdLine).find(L"--gpu") != std::wstring_view::npos;
        const int code = RunTrainerSession(cfg, DataDir(), gpu);
        CoUninitialize();
        return code;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetEcoQoS(false);  // activé dynamiquement en mode Gaming
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\CompanionAI.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    const auto root = ExeDir();
    CompanionConfig cfg = LoadConfig(root / L"companion.json");
    if (cfg.llmModel.is_relative()) cfg.llmModel = root / cfg.llmModel;
    if (!cfg.llmMmproj.empty() && cfg.llmMmproj.is_relative()) cfg.llmMmproj = root / cfg.llmMmproj;
    if (cfg.loraAdapter.is_relative()) cfg.loraAdapter = root / cfg.loraAdapter;

    // --- Overlay + avatar ---------------------------------------------------
    OverlayWindow overlay;
    if (!overlay.Create(instance, {})) {
        MessageBoxW(nullptr, L"Impossible de créer l'overlay DirectX 11.", L"CompanionAI", MB_ICONERROR);
        return 1;
    }
    CartoonAvatar avatar;
    if (!avatar.Load(root / L"assets" / L"avatar" / L"rig.json")) {
        MessageBoxW(nullptr, L"assets/avatar/rig.json introuvable ou invalide.", L"CompanionAI", MB_ICONERROR);
        return 1;
    }

    // --- Voix -----------------------------------------------------------------
    VoiceEngine voice;
    VoiceEngineConfig vcfg;
    vcfg.whisperModel = root / L"assets" / L"models" / L"ggml-base-q5_1.bin";
    vcfg.piperModel = root / L"assets" / L"models" / L"fr_FR-siwis-medium.onnx";
    vcfg.espeakDataDir = root / L"espeak-ng-data";
    voice.Initialize(vcfg);
    voice.SetMood(0.5f);  // humeur neutre = 9 bits, 35 % wet (rapport §4.B)

    // --- Cerveau --------------------------------------------------------------
    MemoryDB db;
    const auto dataDir = DataDir();
    db.Open(dataDir / L"companion.db");
    HttpClient http(cfg.userAgent);
    LocalBrain brain;
    PermissionGate gate(db, [&voice](const std::string& question) { voice.Speak(question); });
    WebResearcher web(http, cfg.wikis);
    ClaudeClient claude(http, cfg.claudeModel, cfg.claudeEffort);
    GameCoach coach(http);
    ResourceGovernor governor;
    SystemControl system(governor, cfg.apps);
    WasapiLoopback loopback;
    OmnisAnalyzer analyzer;
    SelfTrainer trainer(cfg, dataDir);
    AgentOrchestrator orchestrator(
        {cfg, governor, voice, db, brain, gate, web, claude, coach, system, loopback, analyzer, trainer, dataDir});

    std::atomic<SystemMode> mode{SystemMode::Interactive};
    governor.SetModeListener([&](SystemMode, SystemMode current) {
        mode = current;
        voice.SetGamingMode(current == SystemMode::Gaming);
        orchestrator.OnModeChanged(current);
        SetEcoQoS(current == SystemMode::Gaming);
    });
    std::atomic<bool> waveRequested{true};  // il salue au démarrage
    voice.SetTranscriptCallback([&orchestrator, &waveRequested](std::string text) {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("salut") != std::string::npos || lower.find("bonjour") != std::string::npos ||
            lower.find("coucou") != std::string::npos || lower.find("hello") != std::string::npos)
            waveRequested = true;  // il te rend ton salut
        orchestrator.OnTranscript(std::move(text));
    });

    orchestrator.Start();
    coach.Start();
    governor.Start();

    RegisterHotKey(nullptr, kHotkeyTalk, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    RegisterHotKey(nullptr, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
    RegisterHotKey(nullptr, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(nullptr, kHotkeyVad, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');

    voice.Speak("Salut. Je suis en ligne.");

    // --- Boucle principale : messages + rendu cadencé -------------------------
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    bool running = true;
    while (running) {
        const SystemMode m = mode.load();
        // L'animation est à 24 i/s (12 en jeu) : on se réveille au rythme de 24 Hz et l'avatar
        // dit lui-même quand une nouvelle image est due. Rien n'est redessiné entre deux images.
        const auto frameBudget = std::chrono::microseconds(41'667);
        const auto waitMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(frameBudget - (clock::now() - last)).count();
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::max<long long>(0, waitMs)), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            if (msg.message == WM_HOTKEY) {
                switch (static_cast<int>(msg.wParam)) {
                    case kHotkeyTalk:
                        if (voice.IsListening()) voice.EndListening();
                        else voice.BeginListening();
                        break;
                    case kHotkeyToggle: overlay.SetVisible(!overlay.IsVisible()); break;
                    case kHotkeyQuit: running = false; break;
                    case kHotkeyVad:
                        voice.SetListenMode(voice.GetListenMode() == ListenMode::PushToTalk ? ListenMode::VoiceActivity
                                                                                            : ListenMode::PushToTalk);
                        voice.Speak(voice.GetListenMode() == ListenMode::VoiceActivity ? "Je t'écoute en continu."
                                                                                       : "Retour au push-to-talk.");
                        break;
                }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const auto now = clock::now();
        if (now - last < frameBudget) continue;
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        // État visuel : interaction en cours d'abord, puis contexte système.
        const AgentActivity activity = orchestrator.Activity();
        AvatarState state = AvatarState::Idle;
        if (voice.IsListening()) state = AvatarState::Listening;
        else if (activity == AgentActivity::Researching || activity == AgentActivity::Cloud) state = AvatarState::Escalating;
        else if (governor.Snapshot().backgroundWorkActive) state = AvatarState::Training;
        else if (m == SystemMode::Gaming) state = AvatarState::Gaming;
        else if (m == SystemMode::Studio) state = AvatarState::Studio;
        avatar.SetState(state);
        if (waveRequested.exchange(false)) avatar.Wave();
        const bool newFrame = avatar.Update(dt, voice.VoiceRms());

        if (newFrame && overlay.IsVisible()) {
            if (ID2D1DeviceContext* ctx = overlay.BeginFrame()) {
                avatar.Render(ctx, overlay.D2DFactory(), overlay.Scale());
                overlay.EndFrame();
            }
        }
    }

    for (int id : {kHotkeyTalk, kHotkeyToggle, kHotkeyQuit, kHotkeyVad}) UnregisterHotKey(nullptr, id);
    governor.Stop();      // révoque un éventuel bail d'entraînement
    orchestrator.Stop();  // interrompt la génération en cours
    coach.Stop();
    voice.Shutdown();
    brain.Unload();
    db.Close();
    overlay.Destroy();
    if (singleInstance) CloseHandle(singleInstance);
    CoUninitialize();
    return 0;
}
