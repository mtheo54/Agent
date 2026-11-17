#pragma once
// AgentOrchestrator : routeur de la cascade d'inférence (rapport §3), version "local d'abord".
//
//   Niveau 0  C++ pur        commandes (pause, volume, musique, lance <appli>, entraîne-toi,
//                            retiens que…), alertes DSP (clipping, phase), intention "build"
//                            pendant une partie LoL (données live -> recherche ciblée)
//   Niveau 1  llama.cpp 3B   conversation, personnalité ; émet [ESCALATE | …] s'il ne sait pas
//   Niveau 1b Web LOCAL      [ESCALATE | type: WEB] : cache SQLite d'abord, puis wikis en liste
//                            blanche (avec ta permission), résumé par le modèle LOCAL. 0 token.
//   Vision    capture DXGI   "regarde mon écran" : modèle de vision LOCAL s'il est installé
//                            (peut enchaîner sur une recherche wiki locale), sinon Claude Vision
//   Studio    loopback FL    "ton avis sur mon mix" : métriques OMNIS -> modèle local
//   Niveau 2  Claude         dernier recours : REASON, VISION, ou web local infructueux,
//                            toujours après ta permission (sauf "toujours").
//
// Tout le travail tourne sur un thread dédié en priorité basse ; "pause" interrompt immédiatement.

#include "audio/OmnisAnalyzer.hpp"
#include "audio/VoiceEngine.hpp"
#include "audio/WasapiLoopback.hpp"
#include "core/Config.hpp"
#include "core/MemeBank.hpp"
#include "core/MixLexicon.hpp"
#include "core/Phrases.hpp"
#include "core/LocalBrain.hpp"
#include "core/MemoryDB.hpp"
#include "core/PermissionGate.hpp"
#include "core/ResourceGovernor.hpp"
#include "modules/GameCoach.hpp"
#include "modules/SelfTrainer.hpp"
#include "modules/SystemControl.hpp"
#include "network/ClaudeClient.hpp"
#include "network/WebResearcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

enum class AgentActivity : std::uint8_t { Idle, Thinking, Researching, Cloud };

struct OrchestratorDeps {
    const CompanionConfig& cfg;
    ResourceGovernor& governor;
    VoiceEngine& voice;
    MemoryDB& db;
    LocalBrain& brain;
    PermissionGate& gate;
    WebResearcher& web;
    ClaudeClient& claude;
    GameCoach& coach;
    SystemControl& system;
    WasapiLoopback& loopback;
    OmnisAnalyzer& analyzer;
    SelfTrainer& trainer;
    Phrases& phrases;
    std::filesystem::path dataDir;  // %LOCALAPPDATA%\CompanionAI (cache Data Dragon…)
    std::filesystem::path assetsDir;  // assets/ (lexique OMNIS…)
};

class AgentOrchestrator {
public:
    explicit AgentOrchestrator(OrchestratorDeps deps);
    ~AgentOrchestrator();
    AgentOrchestrator(const AgentOrchestrator&) = delete;
    AgentOrchestrator& operator=(const AgentOrchestrator&) = delete;

    void Start();
    void Stop();

    void OnTranscript(std::string text, EmotionEstimate emotion = {});  // thread-safe
    void OnBargeIn();  // l'utilisateur lui coupe la parole : il s'arrête et écoute
    void UpdateConfig(const CompanionConfig& cfg);  // appliquée sur le thread de l'orchestrateur
    void SetOpenSettingsCallback(std::function<void()> cb) { onOpenSettings_ = std::move(cb); }

    // Réactions de l'avatar (appelées depuis les threads de travail ; à relayer au thread de rendu).
    using AvatarFn = std::function<void(const std::string& clip)>;
    void SetAvatarCallbacks(AvatarFn onExpression, AvatarFn onMood);
    // Réglages modifiables à chaud (fenêtre de configuration).
    void SetHumanizer(bool backchannels, float expressiveness) noexcept {
        backchannels_ = backchannels;
        expressiveness_ = expressiveness;
    }
    void OnModeChanged(SystemMode mode);  // thread-safe

    [[nodiscard]] AgentActivity Activity() const noexcept { return activity_.load(); }
    [[nodiscard]] EmotionEstimate LastEmotion() const {
        std::scoped_lock lock(emotionMutex_);
        return lastEmotion_;
    }

private:
    using Job = std::function<void(std::stop_token)>;

    void Post(Job job);
    void WorkerMain(std::stop_token st);
    void Handle(const std::string& text, std::stop_token stop);
    bool HandleLevel0(const std::string& text, std::stop_token stop);
    void HandleLevel1(const std::string& text, const std::vector<ChatTurn>& history, const std::string& extraContext,
                      std::stop_token stop);
    bool HandleSystemCommand(const std::string& text, const std::string& lower, std::stop_token stop);
    void HandleStudioQuestion(const std::string& text, const std::vector<ChatTurn>& history, std::stop_token stop);
    void HandleVision(const std::string& question, std::stop_token stop);
    void AdviseLolBuild(const LolSnapshot& lol, const std::string& question, std::stop_token stop);
    void UpdateStudioCapture();
    void OnMixAlert(MixAlert alert, const MixMetrics& m);
    void ResearchLocally(const std::string& subject, const std::string& question, std::stop_token stop);
    void AnswerFromSources(const std::string& question, const std::string& sources, const std::string& origin,
                           const std::string& subject, std::stop_token stop);
    void EscalateCloud(const std::string& type, const std::string& subject, const std::string& question,
                       std::stop_token stop, const std::vector<std::uint8_t>* jpeg = nullptr);
    void StartTraining(bool manual);

    std::string PersonaPrompt() const;
    std::string LiveContext(const std::string& question) const;
    bool EnsureBrain();
    std::string GenerateSpoken(const std::vector<ChatTurn>& messages, const char* grammar, std::stop_token stop,
                               bool& isEscalation, const ScreenShot* image = nullptr);
    void Say(const std::string& text);
    // Réplique de la banque (assets/persona/phrases.json) au ton choisi.
    std::string Line(const std::string& key, const std::map<std::string, std::string>& vars = {},
                     const std::string& fallback = {});
    void React(const std::string& clip);              // geste, filtré par l'expressivité
    void SetMoodFor(const std::string& clip, int seconds);
    std::string ToneContext() const;

    OrchestratorDeps d_;
    CompanionConfig cfg_;  // copie propre au thread de l'orchestrateur (mise à jour par UpdateConfig)
    std::function<void()> onOpenSettings_;
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Job> jobs_;
    std::stop_source currentJob_;
    std::atomic<AgentActivity> activity_{AgentActivity::Idle};
    std::atomic<SystemMode> mode_{SystemMode::Interactive};

    // Humanisation
    mutable std::mutex emotionMutex_;
    EmotionEstimate lastEmotion_;
    std::chrono::steady_clock::time_point emotionAt_{};
    std::atomic<SpeakingStyle> style_{SpeakingStyle::Normal};
    // Ses dernières façons d'attaquer une réponse : interdites la fois suivante (pas de « Ah ouais ? » en boucle).
    std::deque<std::string> recentOpeners_;
    MixLexicon lexicon_;  // jargon de beatmaker (lexique d'OMNIS)
    MemeBank memes_;      // memes TikTok FR / EN du moment (assets/persona/memes.json)
    [[nodiscard]] std::string MemeHint();
    [[nodiscard]] std::string RageBaitHint(const std::string& userText);
    std::chrono::steady_clock::time_point lastBait_{};
    bool baitPending_ = false;
    std::uint32_t baitRng_ = 0xBA17u;
    std::optional<SpeakingStyle> forcedReplyStyle_;  // prochaine réponse (ex. il se marre après un rage bait)
    bool studioFocus_ = false;  // question studio ciblée en cours (thread de l'orchestrateur)
    void RememberOpener(const std::string& reply);
    [[nodiscard]] std::string OpenerHint() const;
    [[nodiscard]] std::string DropRepeatedOpener(const std::string& chunk) const;
    std::chrono::steady_clock::time_point moodUntil_{};
    bool moodActive_ = false;
    AvatarFn onExpression_, onMood_;
    std::atomic<bool> backchannels_{true};
    std::atomic<float> expressiveness_{0.8f};
    std::atomic<unsigned> rng_{0x51F15EEDu};
    std::atomic<bool> deviceDirty_{true};
    std::chrono::steady_clock::time_point lastBrainUse_{};
    std::chrono::steady_clock::time_point lastAutoTraining_{};
    std::atomic<std::int64_t> listenUntilMs_{0};  // écoute du mix à la demande (hors FL Studio)
};

}  // namespace companion
