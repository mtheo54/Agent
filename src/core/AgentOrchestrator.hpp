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

#include "audio/DiscordBridge.hpp"
#include "audio/OmnisAnalyzer.hpp"
#include "audio/SpeakerId.hpp"
#include "core/CallPeople.hpp"
#include "core/CallPrompt.hpp"
#include "core/BaitBank.hpp"
#include "core/OmnisModes.hpp"
#include "core/RageBait.hpp"
#include "audio/VoiceEngine.hpp"
#include "audio/WasapiLoopback.hpp"
#include "core/Config.hpp"
#include "core/GamePerfMonitor.hpp"
#include "core/MemeBank.hpp"
#include "core/MixLexicon.hpp"
#include "core/ModelPlanner.hpp"
#include "core/Phrases.hpp"
#include "modules/Soundboard.hpp"
#include "core/LocalBrain.hpp"
#include "core/MemoryDB.hpp"
#include "core/Playbooks.hpp"
#include "core/Skills.hpp"
#include "core/PermissionGate.hpp"
#include "core/ResourceGovernor.hpp"
#include "modules/DesktopPilot.hpp"
#include "modules/Subtitles.hpp"
#include "modules/DiscordSpeaking.hpp"
#include "modules/VideoStudy.hpp"
#include "modules/WatchAlong.hpp"
#include "network/WebSurf.hpp"
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
#include <set>
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
    Soundboard& soundboard;
    std::filesystem::path dataDir;  // %LOCALAPPDATA%\CompanionAI (cache Data Dragon…)
    std::filesystem::path assetsDir;  // assets/ (lexique OMNIS…)
    std::function<HardwareInfo()> probe;  // mesure du matériel (VRAM libre en direct) pour choisir le cerveau en jeu
    DiscordBridge* discord = nullptr;     // Omnis dans tes calls, par ton micro (nullptr = désactivé)
    HttpClient* http = nullptr;           // modèle de voix (premier call), page de résultats YouTube
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
    // Ce qu'un pote vient de dire dans le call Discord, avec sa phrase audio (16 kHz) pour savoir qui c'est.
    void OnCallSpeech(std::string text, std::vector<float> pcm16k, std::chrono::steady_clock::time_point end);  // thread-safe
    void UpdateConfig(const CompanionConfig& cfg);
    // Vu-mètre (thread de rendu) : affiché ? niveau court terme et crête de ce qu'il entend de FL, en dBFS.
    // Tableau du studio : sorti quand il écoute ta prod (demande d'avis, « montre ton vu-mètre », ou FL qui joue en mode Studio).
    [[nodiscard]] bool MeterVisible() const;
    [[nodiscard]] std::pair<float, float> MeterLevels() const { const auto m = d_.analyzer.Snapshot(); return {m.rmsDb, m.peakDb}; }  // appliquée sur le thread de l'orchestrateur
    void SetOpenSettingsCallback(std::function<void()> cb) { onOpenSettings_ = std::move(cb); }
    // Commandes de l'interface dites à la voix : 2 = cacher / montrer, 3 = quitter (mêmes identifiants que les raccourcis).
    void SetUiCommandCallback(std::function<void(int)> cb) { onUiCommand_ = std::move(cb); }

    // Réactions de l'avatar (appelées depuis les threads de travail ; à relayer au thread de rendu).
    using AvatarFn = std::function<void(const std::string& clip)>;
    void SetAvatarCallbacks(AvatarFn onExpression, AvatarFn onMood);
    // Réglages modifiables à chaud (fenêtre de configuration).
    void SetHumanizer(bool backchannels, float expressiveness) noexcept {
        backchannels_ = backchannels;
        expressiveness_ = expressiveness;
    }
    void OnModeChanged(SystemMode mode);
    // Retour devant le PC après une absence (fin du mode veille) : bruitage + petit mot.
    void OnComeback();
    // Moments émotionnels (voix + mots) -> bruitages choisis ; appelé sur le thread de l'orchestrateur.
    void EmotionEvents(const std::string& text, const EmotionEstimate& e);
    std::map<std::string, std::chrono::steady_clock::time_point> lastEmotionSound_;  // thread-safe

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
    bool HandleHelp(const std::string& lower);
    void LoadKnowledgePack();
    // Phrase approximative : le modèle choisit la commande (CommandCatalog), puis on l'exécute.
    bool TryFuzzyCommand(const std::string& text, std::stop_token stop);
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
    // silent : pas d'annonce, et le rapport est gardé pour ton retour (il s'entraîne pendant ton absence).
    void StartTraining(bool manual, bool silent = false);

    std::string PersonaPrompt() const;
    std::string ModePrompt() const;  // technique ou rage bait, selon la question en cours
    ChatMode chatMode_ = ChatMode::RageBait;
    int searchRound_ = 0;  // 0 = 1re recherche ; 1 = recherche plus ciblée (ensuite : Claude)
    std::string LiveContext(const std::string& question) const;
    bool EnsureBrain();
    std::string GenerateSpoken(const std::vector<ChatTurn>& messages, const char* grammar, std::stop_token stop,
                               bool& isEscalation, const ScreenShot* image = nullptr, int maxTokens = 220);
    void Say(const std::string& text);
    // Réplique de la banque (assets/persona/phrases.json) au ton choisi.
    std::string Line(const std::string& key, const std::map<std::string, std::string>& vars = {},
                     const std::string& fallback = {});
    void React(const std::string& clip);              // geste, filtré par l'expressivité
    void SetMoodFor(const std::string& clip, int seconds);
    std::string ToneContext() const;

    OrchestratorDeps d_;
    CompanionConfig cfg_;  // copie propre au thread de l'orchestrateur (mise à jour par UpdateConfig)
    std::function<void(int)> onUiCommand_;
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
    [[nodiscard]] std::string SoundHint();
    // Bruitage choisi pour un moment (cfg.soundEvents) ; rien s'il n'est pas encore téléchargé.
    void PlayEventSound(const std::string& event);
    void CheckGameEvents();  // morts, kills, victoire / défaite (LoL, données live)
    bool inLolGame_ = false;
    int lastKills_ = -1, lastDeaths_ = -1;
    std::string lastResult_;
    std::chrono::steady_clock::time_point lastGameSound_{};
    bool HandleSoundboard(const std::string& text, const std::string& lower, std::stop_token stop);
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
    // En jeu : le jeu sature-t-il la carte ? (verdict gardé au moins 60 s : pas de rechargement en boucle)
    bool gameGpuBusy_ = true;
    std::chrono::steady_clock::time_point gpuVerdictAt_{};

    // Perfs en jeu : mesure (FPS, impact), presets par jeu, garde-fou, calibrage (« teste les perfs »).
    GamePerfMonitor perf_;
    std::string gameKey_;  // exécutable du jeu en cours, minuscules (clé des presets)
    std::chrono::steady_clock::time_point perfRetryAt_{}, lastGuard_{};
    struct LearnedPreset {
        std::string preset;
        bool byUser = false;  // choisi à la voix : le garde-fou n'y touche pas
    };
    std::map<std::string, LearnedPreset> learned_;  // %LOCALAPPDATA%\CompanionAI\game_presets.json
    std::optional<GamePreset> forcedPreset_;         // pendant le calibrage
    std::atomic<GamePreset> activePreset_{GamePreset::Auto};  // ce qui est réellement chargé (lu par le moniteur)
    std::set<GamePreset> triedThisGame_;             // garde-fou : pas de ping-pong entre deux presets
    bool calibrating_ = false;
    void LoadLearnedPresets();
    void SaveLearnedPresets() const;
    [[nodiscard]] GamePreset EffectivePreset(std::string* source) const;
    void UpdatePerfMonitor();
    void CheckPerfGuard();
    bool HandlePerfCommand(const std::string& lower, std::stop_token stop);
    void Calibrate(std::stop_token stop);
    void SayPerfReport();

    // Prise en main du clavier / de la souris (« prends la main », AutoCAD, FL Studio)
    DesktopPilot pilot_;
    bool HandlePilot(const std::string& text, const std::string& lower, std::stop_token stop);
    bool HandleAfterEffects(const PilotTarget& t, const std::string& text, std::stop_token stop);  // script ExtendScript vérifié
    bool HandleFlChords(const std::string& text, std::stop_token stop);
    std::string PilotAppName(const PilotTarget& t) const;
    std::string PilotSystem(const PilotTarget& t) const;
    void RunPilotSteps(const PilotTarget& t, std::vector<PilotStep> steps, std::stop_token stop);
    // Compétences créées par Omnis à ta demande et avec ton accord (Skills.hpp).
    std::vector<Skill> skills_;
    std::optional<std::string> aeForcedBody_;  // compétence After Effects : ce script-là, sans en écrire un autre
    [[nodiscard]] std::filesystem::path SkillsDir() const { return d_.dataDir / L"skills"; }
    void ReloadSkills();
    bool HandleSkillCommand(const std::string& text, const std::string& lower, std::stop_token stop);
    void CreateSkill(const std::string& request, std::stop_token stop);
    void RunSkill(const Skill& k, const std::string& text, std::stop_token stop);
    std::optional<std::wstring> ProbeStudioInput(std::stop_token stop);  // FL en ASIO natif : l'entrée qui le reçoit
    bool studioProbed_ = false;  // une seule recherche de la source de FL par question
    void SayProdOpinion(const std::string& question, const MixMetrics& mix, std::stop_token stop);
    std::atomic<bool> studioOff_{false};     // « arrête le mode écoute »
    std::atomic<bool> meterVisible_{false};  // vu-mètre affiché sous le personnage
    std::vector<TimedWord> aeWords_;  // voix off du montage AE, mot par mot (temps de la composition)
    std::string aeWordsKey_;          // fichier + taille + position : retranscrit seulement si ça change  // accords posés par un script piano roll
    // Call Discord (par ton micro) : rejoindre / quitter, réagir à ce que disent les autres
    bool HandleCallCommand(const std::string& lower, std::stop_token stop);
    // « Prends ma voix » / « reprends ta voix » / « oublie ma voix » : clone local de la voix de l'utilisateur.
    bool HandleVoiceClone(const std::string& lower, std::stop_token stop);
    [[nodiscard]] bool UsesUserVoice() const;
    void HandleCallLine(const std::string& text, const std::vector<float>& pcm, std::chrono::steady_clock::time_point end,
                        std::stop_token stop);
    [[nodiscard]] std::string CallHint() const;
    [[nodiscard]] bool InCall() const { return d_.discord && d_.discord->InCall(); }
    std::deque<std::string> callLines_;  // derniers échanges du call (thread de l'orchestrateur)
    std::chrono::steady_clock::time_point lastCallBait_{}, callMutedUntil_{};
    // Qui parle (empreintes de voix) et fiches par personne
    SpeakerId speaker_;
    DiscordSpeaking seeing_;  // qui parle, lu sur l'écran (cercle vert + pseudo)
    bool listenOnly_ = false;  // mode écoute : il apprend les voix, ne parle que si on l'appelle
    void JoinedCall();
    void LeftCall();
    CallPeople people_;
    bool peopleLoaded_ = false;
    std::jthread modelDownload_;
    bool EnsureSpeakerModel();
    std::string lastSpeakerId_;
    std::set<std::string> protected_;    // personnes à ménager ce soir (sujet sensible entendu)
    std::set<std::string> sessionAvoid_;  // « Léa : sa santé »
    // Se faire entendre : gain qui monte quand on ne l'a pas entendu, redescend doucement sinon
    float callGain_ = 0.9f;
    int heardStreak_ = 0;
    std::chrono::steady_clock::time_point lastSpokeInCall_{}, lastEarrape_{}, quietUntil_{}, lastCallLineAt_{};
    std::string lastCallReply_;
    std::deque<std::string> recentBaits_, recentTechniques_;
    BaitBank bank_;
    // « Regarde avec moi » : il écoute le son de ton navigateur pendant que TU scrolles, apprend, réagit parfois.
    WatchAlong watch_;
    std::atomic<int> watchPending_{0};  // transcriptions en cours (1 au plus : sa voix passe avant)
    bool watchSilent_ = false;
    std::string watchTitle_, watchText_;
    std::chrono::steady_clock::time_point lastWatchComment_{};
    int watchClips_ = 0, watchForms_ = 0;
    bool HandleWatchCommand(const std::string& lower);
    void OnWatchSpeech(std::string text, std::string title);  // thread de transcription
    void FlushWatchClip(std::stop_token stop, std::chrono::steady_clock::time_point until = {});
    std::chrono::steady_clock::time_point watchClipStart_{};
    // Module de vision (mmproj) du cerveau : téléchargé à la demande, branché à chaud.
    bool EnsureVision();
    std::jthread visionDownload_;
    std::atomic<bool> visionReady_{false};
    void CheckWatch();
    void StopWatching(bool announce);  // ses 4 dernières vannes : pas de redites, pas de tics
    bool PlayBaitSound(bool earrape);
    void PrefetchCallSounds();
    // « Prends ma place » : il garde ta place 60 min au plus, te résume tout à ton retour
    bool subActive_ = false;
    std::chrono::steady_clock::time_point subStart_{}, subUntil_{}, nextActivity_{};
    std::vector<std::string> absenceLog_, shownLog_;
    void StartSubstitute(const std::string& reason);
    void EndSubstitute(bool userBack);
    void CheckSubstitute();
    void DoActivity(std::optional<std::string> request = std::nullopt);
    bool OpenOnScreen1(const std::string& url);
    void SeedRageBaitTraining();
    std::chrono::steady_clock::time_point lastAutoTraining_{};
    std::atomic<std::int64_t> listenUntilMs_{0};  // écoute du mix à la demande (hors FL Studio)
    std::atomic<std::int64_t> lastStudioMs_{-1'000'000};  // dernière fois en mode Studio (le mode clignote)
    bool soundsOffered_ = false;
    int wordsSinceDisfluency_ = 0;  // une hésitation tous les ~22 mots (Disfluency)                          // bruitages proposés une fois par session
    void OfferSounds(std::stop_token stop);
    int InstallStarterSounds(std::stop_token stop);
    // Mode expert : un gros cerveau MoE (hors jeu) pour les questions de pro, et la méthode du domaine (playbook).
    std::vector<Playbook> playbooks_;
    const Playbook* playbook_ = nullptr;  // domaine de la question en cours
    bool expertActive_ = false;
    bool expertOffered_ = false;
    std::chrono::steady_clock::time_point expertLastUse_{};
    std::jthread expertDownload_;
    std::atomic<bool> expertDownloading_{false};
    [[nodiscard]] std::filesystem::path ExpertPath() const;
    [[nodiscard]] bool ExpertInstalled() const;
    bool HandleExpertCommand(const std::string& lower, std::stop_token stop);
    void StartExpertDownload();
    void MaybeUseExpert(const std::string& lower, std::stop_token stop);
    void OfferExpert(std::stop_token stop);
};

}  // namespace companion
