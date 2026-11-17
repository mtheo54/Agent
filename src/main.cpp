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
//   Ctrl+Alt+O : réglages du personnage (aussi : « ouvre les réglages »)
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
#include "core/Phrases.hpp"
#include "core/ResourceGovernor.hpp"
#include "modules/GameCoach.hpp"
#include "modules/SelfTrainer.hpp"
#include "modules/SystemControl.hpp"
#include "network/ClaudeClient.hpp"
#include "network/HttpClient.hpp"
#include "network/WebResearcher.hpp"
#include "render/OverlayWindow.hpp"
#include "core/Persona.hpp"
#include "render/CartoonAvatar.hpp"
#include "ui/ConfigWindow.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>
#include <string>
#include <string_view>

using namespace companion;

namespace {

enum HotkeyId : int { kHotkeyTalk = 1, kHotkeyToggle = 2, kHotkeyQuit = 3, kHotkeyVad = 4, kHotkeySettings = 5 };
constexpr UINT WM_APP_OPEN_SETTINGS = WM_APP + 2;

// Commandes de l'avatar émises par les threads de travail, appliquées par le thread de rendu.
struct AvatarCommands {
    std::mutex mutex;
    std::vector<std::string> expressions;
    std::string mood = "idle";
};

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
    vcfg.neural.library = root / L"qwen.dll";
    vcfg.neural.talker = root / L"assets" / L"models" / L"qwen-talker-0.6b-base-Q8_0.gguf";
    vcfg.neural.codec = root / L"assets" / L"models" / L"qwen-tokenizer-12hz-Q8_0.gguf";
    vcfg.neural.voiceDir = root / L"assets" / L"voice" / L"omnis";
    vcfg.neural.device = cfg.neuralVoiceDevice;
    vcfg.neuralEnabled = cfg.neuralVoice;
    vcfg.neuralInGame = cfg.neuralVoiceInGame;
    voice.Initialize(vcfg);
    voice.SetMood(cfg.fxIntensity);  // 0,5 = 9 bits, 35 % wet (rapport §4.B)
    voice.SetBaseSpeechRate(cfg.speechRate);
    voice.SetEmotionDetection(cfg.emotionDetection);
    voice.SetQuirks(cfg.voiceQuirks);
    Phrases phrases;  // répliques par ton (assets/persona/phrases.json)
    phrases.Load(root / L"assets" / L"persona" / L"phrases.json");
    phrases.SetTone(static_cast<Tone>(cfg.tone));
    const auto backchannels = [&phrases] {
        auto bank = BackchannelPhrases();
        for (auto& [kind, list] : bank)
            if (auto mine = phrases.All("backchannel." + kind); !mine.empty()) list = std::move(mine);
        return bank;
    };
    voice.PrepareBackchannels(backchannels());  // relances jouées sans latence

    // --- Cerveau --------------------------------------------------------------
    MemoryDB db;
    const auto dataDir = DataDir();
    db.Open(dataDir / L"companion.db");
    if (auto baseline = db.GetProfile("voice.baseline")) voice.LoadVoiceBaseline(*baseline);  // ta voix habituelle
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
        {cfg, governor, voice, db, brain, gate, web, claude, coach, system, loopback, analyzer, trainer, phrases, dataDir, root / L"assets"});

    std::atomic<SystemMode> mode{SystemMode::Interactive};
    governor.SetModeListener([&](SystemMode, SystemMode current) {
        mode = current;
        voice.SetGamingMode(current == SystemMode::Gaming);
        orchestrator.OnModeChanged(current);
        SetEcoQoS(current == SystemMode::Gaming);
    });
    // Réactions et humeurs décidées par l'orchestrateur -> personnage (thread de rendu).
    AvatarCommands avatarCmds;
    orchestrator.SetAvatarCallbacks(
        [&avatarCmds](const std::string& clip) {
            std::scoped_lock lock(avatarCmds.mutex);
            avatarCmds.expressions.push_back(clip);
        },
        [&avatarCmds](const std::string& clip) {
            std::scoped_lock lock(avatarCmds.mutex);
            avatarCmds.mood = clip;
        });
    voice.SetBargeIn(cfg.bargeIn, [&orchestrator] { orchestrator.OnBargeIn(); });
    if (cfg.voiceActivity) voice.SetListenMode(ListenMode::VoiceActivity);
    overlay.SetLayout(cfg.avatarScale, cfg.avatarCorner);
    avatar.SetOpacity(cfg.avatarOpacity);

    const DWORD mainThread = GetCurrentThreadId();
    orchestrator.SetOpenSettingsCallback([mainThread] { PostThreadMessageW(mainThread, WM_APP_OPEN_SETTINGS, 0, 0); });

    // Fenêtre de réglages : enregistrement dans companion.json et application immédiate.
    ConfigWindow settings;
    const auto applySettings = [&](const CompanionConfig& edited) {
        // Seuls les champs de la fenêtre sont recopiés (les chemins résolus restent ceux de cfg).
        cfg.name = edited.name;
        cfg.avatarScale = edited.avatarScale;
        cfg.avatarOpacity = edited.avatarOpacity;
        cfg.avatarCorner = edited.avatarCorner;
        cfg.expressiveness = edited.expressiveness;
        cfg.fxIntensity = edited.fxIntensity;
        cfg.speechRate = edited.speechRate;
        const bool toneChanged = cfg.tone != edited.tone;
        cfg.tone = edited.tone;
        cfg.backchannels = edited.backchannels;
        cfg.neuralVoice = edited.neuralVoice;
        cfg.voiceQuirks = edited.voiceQuirks;
        cfg.bargeIn = edited.bargeIn;
        cfg.emotionDetection = edited.emotionDetection;
        cfg.voiceActivity = edited.voiceActivity;
        cfg.autoTraining = edited.autoTraining;
        cfg.cloudEnabled = edited.cloudEnabled;
        cfg.cloudAsksPermission = edited.cloudAsksPermission;
        SaveConfig(cfg, root / L"companion.json");
        voice.SetMood(cfg.fxIntensity);
        voice.SetBaseSpeechRate(cfg.speechRate);
        voice.SetEmotionDetection(cfg.emotionDetection);
        voice.SetNeuralVoice(cfg.neuralVoice, cfg.neuralVoiceInGame);
        voice.SetQuirks(cfg.voiceQuirks);
        if (toneChanged) {
            phrases.SetTone(static_cast<Tone>(cfg.tone));
            voice.PrepareBackchannels(backchannels());
        }
        voice.SetBargeIn(cfg.bargeIn, [&orchestrator] { orchestrator.OnBargeIn(); });
        voice.SetListenMode(cfg.voiceActivity ? ListenMode::VoiceActivity : ListenMode::PushToTalk);
        overlay.SetLayout(cfg.avatarScale, cfg.avatarCorner);
        avatar.SetOpacity(cfg.avatarOpacity);
        orchestrator.UpdateConfig(cfg);
    };
    const auto settingsStatus = [&]() {
        ConfigStatus st;
        st.brainReady = std::filesystem::exists(cfg.llmModel);
        st.sttReady = voice.HasStt();
        st.ttsReady = voice.HasTts();
        st.neuralVoice = voice.HasNeuralVoice();
        st.claudeKey = claude.HasCredentials();
        const std::string baseline = voice.VoiceBaseline();
        st.voiceSamples = std::atoi(baseline.substr(0, baseline.find(';')).c_str());
        const EmotionEstimate e = orchestrator.LastEmotion();
        st.emotion = ToString(e.label);
        st.arousal = e.arousal;
        return st;
    };
    const auto openSettings = [&] {
        settings.Open(instance, cfg, root / L"assets" / L"fonts", root / L"assets" / L"avatar" / L"rig.json", applySettings,
                      settingsStatus);
    };

    std::atomic<bool> waveRequested{true};  // il salue au démarrage
    voice.SetTranscriptCallback([&orchestrator, &waveRequested](std::string text, EmotionEstimate emotion) {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("salut") != std::string::npos || lower.find("bonjour") != std::string::npos ||
            lower.find("coucou") != std::string::npos || lower.find("hello") != std::string::npos)
            waveRequested = true;  // il te rend ton salut
        orchestrator.OnTranscript(std::move(text), std::move(emotion));
    });

    orchestrator.Start();
    coach.Start();
    governor.Start();

    RegisterHotKey(nullptr, kHotkeyTalk, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    RegisterHotKey(nullptr, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
    RegisterHotKey(nullptr, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(nullptr, kHotkeyVad, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V');
    RegisterHotKey(nullptr, kHotkeySettings, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');

    voice.Speak(phrases.Pick("greeting", {}, "Salut. Je suis en ligne."));

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
                    case kHotkeySettings: openSettings(); break;
                    case kHotkeyVad:
                        voice.SetListenMode(voice.GetListenMode() == ListenMode::PushToTalk ? ListenMode::VoiceActivity
                                                                                            : ListenMode::PushToTalk);
                        voice.Speak(voice.GetListenMode() == ListenMode::VoiceActivity ? "Je t'écoute en continu."
                                                                                       : "Retour au push-to-talk.");
                        break;
                }
                continue;
            }
            if (msg.message == WM_APP_OPEN_SETTINGS) {
                openSettings();
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
        const bool sleeping = m == SystemMode::Idle && state == AvatarState::Idle;
        {
            std::scoped_lock lock(avatarCmds.mutex);
            for (const auto& clip : avatarCmds.expressions) avatar.Express(clip);
            avatarCmds.expressions.clear();
            // PC inactif depuis 5 min (et pas d'entraînement) : il pique du nez.
            avatar.SetMood(sleeping ? std::string("sleep") : avatarCmds.mood);
        }
        // Accessoires selon le contexte : jeu en cours, studio, entraînement, recherche, Claude, sommeil.
        {
            std::vector<std::string> contexts;
            const std::wstring game = governor.ActiveGame();
            if (game == L"cyberpunk2077.exe") contexts.push_back("cyberpunk");
            else if (game == L"league of legends.exe") contexts.push_back("lol");
            else if (!game.empty()) contexts.push_back("fps");
            if (m == SystemMode::Studio) contexts.push_back("studio");
            if (state == AvatarState::Training) contexts.push_back("train");
            if (activity == AgentActivity::Researching) contexts.push_back("research");
            if (activity == AgentActivity::Cloud) contexts.push_back("cloud");
            if (sleeping) contexts.push_back("sleep");
            avatar.SetContexts(contexts);
        }
        settings.Tick(dt);
        const bool newFrame = avatar.Update(dt, voice.VoiceRms());

        if (newFrame && overlay.IsVisible()) {
            if (ID2D1DeviceContext* ctx = overlay.BeginFrame()) {
                avatar.Render(ctx, overlay.D2DFactory(), overlay.Scale());
                overlay.EndFrame();
            }
        }
    }

    for (int id : {kHotkeyTalk, kHotkeyToggle, kHotkeyQuit, kHotkeyVad, kHotkeySettings}) UnregisterHotKey(nullptr, id);
    settings.Close();
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
