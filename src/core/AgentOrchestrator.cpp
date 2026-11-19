#include "core/AgentOrchestrator.hpp"

#include "core/Log.hpp"
#include "core/Persona.hpp"
#include "core/Disfluency.hpp"
#include "audio/NeuralVoice.hpp"

#include <objbase.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <cctype>
#include <format>
#include <fstream>
#include <future>
#include <regex>
#include <thread>

namespace companion {
namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool ContainsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [&](const char* n) { return haystack.find(n) != std::string::npos; });
}

std::string Trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Une connaissance en cache est-elle pertinente ? La moitié au moins des mots du sujet doivent y figurer.
bool Relevant(const KnowledgeHit& hit, const std::string& subject) {
    const std::string hay = Lower(hit.title + " " + hit.content.substr(0, 6000));
    int terms = 0, found = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3) { ++terms; if (hay.find(cur) != std::string::npos) ++found; }
        cur.clear();
    };
    for (unsigned char c : Lower(subject)) {
        if (c >= 0x80 || std::isalnum(c)) cur.push_back(static_cast<char>(c));
        else flush();
    }
    flush();
    return terms > 0 && found * 2 >= terms;
}

}  // namespace

AgentOrchestrator::AgentOrchestrator(OrchestratorDeps deps) : d_(deps), cfg_(deps.cfg) {
    lexicon_.Load(d_.assetsDir / L"omnis" / L"lexique_fr.txt");
    memes_.Load(d_.assetsDir / L"persona" / L"memes.json");
    backchannels_ = cfg_.backchannels;
    expressiveness_ = cfg_.expressiveness;
    d_.analyzer.SetAlertCallback([this](MixAlert a, const MixMetrics& m) { OnMixAlert(a, m); });
    LoadLearnedPresets();
}
AgentOrchestrator::~AgentOrchestrator() { Stop(); }

void AgentOrchestrator::Start() {
    if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token st) { WorkerMain(st); });
}

void AgentOrchestrator::Stop() {
    {
        std::scoped_lock lock(mutex_);
        currentJob_.request_stop();
        jobs_.clear();
    }
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    d_.trainer.Kill();  // normalement déjà fait par la révocation du bail par le governor
    d_.loopback.Stop();
    d_.analyzer.Stop();
    perf_.Stop();  // résumé de la partie en cours, s'il y en a une
    seeing_.Stop();
    watch_.Stop();
}

void AgentOrchestrator::Post(Job job) {
    {
        std::scoped_lock lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void AgentOrchestrator::OnComeback() {
    Post([this](std::stop_token) {
        PlayEventSound("comeback");
        if (mode_.load() != SystemMode::Gaming) Say(Line("comeback", {}, "Ah, te revoilà."));
    });
}

void AgentOrchestrator::OnModeChanged(SystemMode mode) {
    if (mode == SystemMode::Studio && mode_.load() != SystemMode::Studio)
        Post([this](std::stop_token) { PlayEventSound("studioStart"); });
    mode_ = mode;
    deviceDirty_ = true;  // GPU <-> CPU appliqué par le worker avant la prochaine génération
    cv_.notify_one();
}

void AgentOrchestrator::SetAvatarCallbacks(AvatarFn onExpression, AvatarFn onMood) {
    onExpression_ = std::move(onExpression);
    onMood_ = std::move(onMood);
}

void AgentOrchestrator::React(const std::string& clip) {
    if (clip.empty() || !onExpression_) return;
    // Un humain ne ponctue pas chaque phrase d'un geste : l'expressivité règle la fréquence.
    const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
    if (static_cast<float>((r >> 8) % 1000) / 1000.f < expressiveness_.load()) onExpression_(clip);
}

void AgentOrchestrator::SetMoodFor(const std::string& clip, int seconds) {
    if (!onMood_) return;
    onMood_(clip);
    std::scoped_lock lock(emotionMutex_);
    moodUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    moodActive_ = clip != "idle";
}

std::string AgentOrchestrator::ToneContext() const {
    std::scoped_lock lock(emotionMutex_);
    if (std::chrono::steady_clock::now() - emotionAt_ > std::chrono::seconds(90) || lastEmotion_.confidence < 0.35f ||
        lastEmotion_.label == VoiceEmotion::Neutral)
        return {};
    // Indice seulement : vu sur le terrain, « Il semble triste » a suffi pour que le modèle parle de dépression à quelqu'un
    // qui allait très bien. Il adapte son TON, il n'en parle pas et ne pose aucun diagnostic.
    return std::format("[Ton de voix, indice incertain] Il a peut-être l'air un peu {} ({}). Adapte seulement ton ton ; n'en parle "
                       "pas, ne lui demande pas s'il va bien, et ne parle JAMAIS de dépression, de santé mentale ou de moral sauf "
                       "s'il en parle lui-même.\n",
                       ToString(lastEmotion_.label), lastEmotion_.description);
}

void AgentOrchestrator::UpdateConfig(const CompanionConfig& cfg) {
    SetHumanizer(cfg.backchannels, cfg.expressiveness);
    Post([this, cfg](std::stop_token) { cfg_ = cfg; });  // cfg arrive avec les chemins déjà résolus
}

void AgentOrchestrator::OnBargeIn() {
    {
        std::scoped_lock lock(mutex_);
        currentJob_.request_stop();  // la génération en cours s'arrête ; la nouvelle phrase arrive
    }
    React("nod");  // « oui, vas-y »
}

void AgentOrchestrator::OnTranscript(std::string text, EmotionEstimate emotion) {
    text = Trim(std::move(text));
    if (text.empty()) return;

    // Émotion : la voix donne l'activation, les mots tranchent la tonalité.
    const EmotionEstimate e = FuseWithText(std::move(emotion), text);
    {
        std::scoped_lock lock(emotionMutex_);
        lastEmotion_ = e;
        emotionAt_ = std::chrono::steady_clock::now();
    }
    d_.db.SetProfile("voice.baseline", d_.voice.VoiceBaseline());
    if (e.confidence >= 0.35f) {
        switch (e.label) {
            case VoiceEmotion::Joyful:
                style_ = SpeakingStyle::Bright;
                React(TextSentiment(text) > 0.5f && ContainsAny(Lower(text), {"haha", "mdr", "ahah"}) ? "laugh" : "happy");
                break;
            case VoiceEmotion::Sad:
            case VoiceEmotion::Tired:
                style_ = SpeakingStyle::Soft;
                SetMoodFor("empathy", 120);
                break;
            case VoiceEmotion::Angry:
            case VoiceEmotion::Stressed:
                style_ = SpeakingStyle::Calm;
                SetMoodFor("soothe", 90);
                break;
            default:
                style_ = SpeakingStyle::Normal;
                break;
        }
    } else {
        style_ = SpeakingStyle::Normal;
    }
    // Réponse à une question du Permission Gate : consommée immédiatement (le worker attend).
    if (d_.gate.OnUserReply(text)) return;

    // "Pause" coupe tout, tout de suite, sans attendre la file.
    const std::string lower = Lower(text);
    const bool aboutMusic = ContainsAny(lower, {"musique", "chanson", "lecture", "spotify"});
    if (!aboutMusic && ContainsAny(lower, {"pause", "tais-toi", "tais toi", "stop"})) {
        {
            std::scoped_lock lock(mutex_);
            currentJob_.request_stop();
            jobs_.clear();
        }
        d_.voice.StopSpeaking();
        d_.governor.Pause();
        return;
    }
    Post([this, text = std::move(text), e](std::stop_token st) {
        EmotionEvents(text, e);
        Handle(text, st);
    });
}

// Moments émotionnels. Chaque son a son délai minimum (pas de « fahhh » toutes les 10 s), et un seul son
// par phrase : le plus parlant gagne. Tristesse : son doux et voix douce, jamais de vanne.
void AgentOrchestrator::EmotionEvents(const std::string& text, const EmotionEstimate& e) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const std::string t = Lower(text);
    const auto fire = [&](const char* event, std::chrono::seconds cooldown) {
        auto& last = lastEmotionSound_[event];
        if (last != clock::time_point{} && now - last < cooldown) return false;
        last = now;
        PlayEventSound(event);
        return true;
    };
    const bool sure = e.confidence >= 0.45f;
    // 1) Il se fait insulter (sauf s'il vient de te rage bait : là c'est l'aveu, géré ailleurs)
    const bool baited = baitPending_ && now - lastBait_ < std::chrono::minutes(3);
    if (!baited && ContainsAny(t, {"t'es nul", "t'es con", "t'es débile", "t'es naze", "ta gueule", "ferme ta gueule",
                                   "ferme-la", "t'es une merde", "connard", "t'es bête", "t'es inutile", "sale robot"})) {
        fire("insulted", std::chrono::seconds(60));
        return;
    }
    // 2) Tu rigoles (whisper écrit le rire : « haha », « (rires) »…)
    if (ContainsAny(t, {"haha", "ahah", "hihi", "mdr", "ptdr", "(rire", "[rire", "rires)", "lol "})) {
        fire("userLaugh", std::chrono::seconds(90));
        return;
    }
    // 3) Rage : seulement en jeu
    if (mode_.load() == SystemMode::Gaming && ((e.label == VoiceEmotion::Angry && sure) ||
                                               (e.arousal > 0.6f && TextSentiment(text) < -0.4f))) {
        fire("rageInGame", std::chrono::seconds(60));
        return;
    }
    // 4) Hype
    if ((e.label == VoiceEmotion::Joyful && e.arousal > 0.5f && sure) ||
        ContainsAny(t, {"let's go", "lets go", "trop bien", "c'est incroyable", "on a gagné", "j'ai gagné", "je suis trop content"})) {
        fire("hype", std::chrono::seconds(120));
        return;
    }
    // 5) Tristesse : son doux (au plus toutes les 10 min)
    if (e.label == VoiceEmotion::Sad && sure) {
        fire("sad", std::chrono::minutes(10));
        return;
    }
    // 6) Fatigue tard la nuit (1 h - 6 h)
    if (e.label == VoiceEmotion::Tired || ContainsAny(t, {"je suis crevé", "je suis fatigué", "chui crevé", "je suis mort de fatigue"})) {
        const std::time_t tt = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &tt);
        if (local.tm_hour >= 1 && local.tm_hour < 6) {
            fire("tiredNight", std::chrono::minutes(30));
            return;
        }
    }
    // 7) Surprise
    if (ContainsAny(t, {"quoi ?!", "quoi !", "non mais sérieux", "c'est pas possible", "j'y crois pas"}))
        fire("userSurprise", std::chrono::seconds(60));
}

void AgentOrchestrator::WorkerMain(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.Orchestrator");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // volume, capture d'écran, WIC
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, st, std::chrono::seconds(5), [&] { return !jobs_.empty() || deviceDirty_.load(); });
            if (st.stop_requested()) break;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                currentJob_ = std::stop_source{};
            }
        }
        UpdateStudioCapture();
        CheckGameEvents();  // au plus toutes les 5 s, comme le sondage de l'API LoL
        UpdatePerfMonitor();
        CheckSubstitute();
        CheckWatch();
        if (deviceDirty_.exchange(false)) {
            const bool gaming = mode_.load() == SystemMode::Gaming;
            // Entrée en jeu : tout est libéré (VRAM et RAM rendues au jeu), rien n'est rechargé tant qu'on ne lui
            // parle pas. Sortie : retour au périphérique du cerveau principal (rechargé à la 1re question).
            if (gaming) {
                d_.brain.Unload();
                gameGpuBusy_ = true;  // nouvelle partie : verdict refait sur les mesures de ce jeu
                gpuVerdictAt_ = {};
            } else d_.brain.Configure(cfg_.llmGpuLayers > 0, cfg_.llmThreads);
        }
        if (job) {
            std::stop_token jobStop;
            {
                std::scoped_lock lock(mutex_);
                jobStop = currentJob_.get_token();
            }
            job(jobStop);
            activity_ = AgentActivity::Idle;
        }
        CheckPerfGuard();
        // Fin d'une humeur temporaire (empathie, apaisement) : retour à la posture normale.
        {
            bool expire = false;
            {
                std::scoped_lock lock(emotionMutex_);
                expire = moodActive_ && std::chrono::steady_clock::now() > moodUntil_;
                if (expire) moodActive_ = false;
            }
            if (expire) {
                if (onMood_) onMood_("idle");
                style_ = SpeakingStyle::Normal;
            }
        }

        // Auto-entraînement : PC inactif (> 5 min, charge < 25 %), des sujets à apprendre, pas trop souvent.
        // Surf autorisé (« cette nuit ») : une séance toutes les 30 min tant que le PC reste inactif.
        const int everyMin = WebSurf::Allowed(d_.db) ? std::min(30, cfg_.autoTrainingEveryMinutes) : cfg_.autoTrainingEveryMinutes;
        if (cfg_.autoTraining && !d_.trainer.Running() &&
            std::chrono::steady_clock::now() - lastAutoTraining_ > std::chrono::minutes(everyMin) &&
            d_.governor.CanStartAutoTraining() &&
            (cfg_.videoStudy || !d_.db.PendingTrainingTopics(1, cfg_.knowledgeMaxAgeDays * 86400LL, 86400).empty()))
            StartTraining(/*manual=*/false);

        // Budget RAM veille (< 80 Mo) : on décharge le modèle après une période sans conversation.
        if (d_.brain.IsLoaded() &&
            std::chrono::steady_clock::now() - lastBrainUse_ > std::chrono::minutes(cfg_.llmUnloadAfterMinutes)) {
            d_.brain.Unload();
            log::Info("Orchestrator: modèle local déchargé (inactivité)");
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

// ---------------------------------------------------------------------------
// Studio : capture active en mode Studio (FL Studio lancé) ou 90 s après "écoute mon mix".
void AgentOrchestrator::UpdateStudioCapture() {
    const bool studio = mode_.load() == SystemMode::Studio;
    if (studio) lastStudioMs_ = NowMs();
    // Le mode Studio clignote quand FL perd le focus (vu : Studio -> Interactive -> Studio en 2 s) : on garde
    // l'écoute 60 s, sinon l'analyse repartait de zéro au milieu de la prod.
    const bool wanted = studio || NowMs() < listenUntilMs_.load() || NowMs() - lastStudioMs_.load() < 60'000;
    if (wanted && !d_.loopback.Running()) {
        d_.analyzer.Reset();
        d_.analyzer.Start();
        auto sink = [this](const float* data, std::size_t frames) { d_.analyzer.Push(data, frames); };
        const std::wstring proc(cfg_.studioProcess.begin(), cfg_.studioProcess.end());
        // 1) l'entrée de la carte qui reçoit FL (ASIO natif), 2) FL Studio seul dès qu'il tourne (sa voix à lui n'est pas
        // captée), 3) le mix système. Avant, hors mode Studio, c'était toujours le mix système.
        const std::wstring input = log::Widen(cfg_.studioInput);
        if (!(!input.empty() && d_.loopback.StartInput(input, sink)) && !d_.loopback.Start(proc, sink)) d_.loopback.Start(L"", sink);
    } else if (!wanted && d_.loopback.Running()) {
        d_.loopback.Stop();
        d_.analyzer.Stop();
    }
}

void AgentOrchestrator::OnMixAlert(MixAlert alert, const MixMetrics& m) {
    // Thread d'analyse. Niveau 0 : pas de modèle, phrase fixe, limitée dans le temps par l'analyseur.
    if (!cfg_.studioAlerts || mode_.load() != SystemMode::Studio || d_.voice.IsSpeaking()) return;
    if (alert == MixAlert::Clipping) {
        PlayEventSound("clipping");
        d_.voice.Speak(Line("clipping", {{"peak", std::format("{:.1f}", m.peakDb)}}, std::format("Attention, ça clippe : crête à {:.1f} dB.", m.peakDb)));
    } else
        d_.voice.Speak(Line("phase", {}, "Problème de phase : ton mix s'annule en mono, vérifie tes élargisseurs."));
}

// ---------------------------------------------------------------------------
std::string AgentOrchestrator::PersonaPrompt() const { return companion::PersonaPrompt(cfg_.name, cfg_.tone); }

// Système selon le mode de la question en cours (chacun a sa photo d'état : pas de recalcul à chaque fois).
std::string AgentOrchestrator::ModePrompt() const {
    if (chatMode_ == ChatMode::Technical) return TechnicalPrompt(cfg_.name);
    return RageBaitChatPrompt(cfg_.name);
}

std::string AgentOrchestrator::Line(const std::string& key, const std::map<std::string, std::string>& vars,
                                    const std::string& fallback) {
    d_.phrases.SetTone(static_cast<Tone>(cfg_.tone));
    static const char* const errors[] = {"stuck", "cloudError", "riotFailed", "launchFailed", "trainingFailed", "captureFailed",
                                         "soundMissing"};
    if (std::ranges::any_of(errors, [&](const char* e) { return key == e; })) PlayEventSound("error");
    return d_.phrases.Pick(key, vars, fallback);
}

std::string AgentOrchestrator::LiveContext(const std::string& question) const {
    std::string ctx = ToneContext();
    if (auto lol = d_.coach.Latest()) ctx += lol->ToPromptSummary() + "\n";
    if (d_.loopback.Running()) {
        const auto mix = d_.analyzer.Snapshot();
        ctx += mix.seconds >= 1.f && !mix.silent ? "[Studio] Tu entends en direct le son qui sort de FL Studio.\n"
                                                 : "[Studio] FL Studio est ouvert, mais il ne joue rien en ce moment.\n";
        // Demande ciblée : le constat ciblé suffit (ajouté par HandleStudioQuestion).
        if (mix.seconds >= 1.f && !studioFocus_) ctx += mix.ToSummary() + "\n" + mix.Diagnosis() + "\n";
    }
    for (const auto& m : d_.db.SearchMemories(question, 3)) ctx += "Souvenir : " + m + "\n";
    // Fiches distillées pendant l'auto-entraînement : c'est ce qui évite de ré-escalader.
    for (const auto& l : d_.db.SearchLessons(question, 2))
        if (MemoryDB::CoversSubject(question, l.subject)) ctx += "Fiche apprise (" + l.subject + ") : " + l.fiche + "\n";
    return ctx;
}

bool AgentOrchestrator::EnsureBrain() {
    lastBrainUse_ = std::chrono::steady_clock::now();
    const bool gaming = mode_.load() == SystemMode::Gaming;
    std::filesystem::path wanted = cfg_.llmModel;
    bool gpu = cfg_.llmGpuLayers > 0;
    if (gaming) {
        wanted = cfg_.llmGamingModel;
        gpu = false;
        // En jeu : carte ou processeur selon ce que le jeu sature, puis un quart de la VRAM au plus, moins si
        // le jeu en occupe beaucoup (mesures du moment).
        if (cfg_.llmGamingAuto && d_.probe) {
            HardwareInfo hw = d_.probe();
            hw.gameGpuLoad = d_.governor.Snapshot().gpuLoad;
            const auto now = std::chrono::steady_clock::now();
            if (gpuVerdictAt_ == std::chrono::steady_clock::time_point{} || now - gpuVerdictAt_ >= std::chrono::seconds(60)) {
                const bool busy = ModelPlanner::GameSaturatesGpu(hw.gameGpuLoad, gameGpuBusy_);
                if (busy != gameGpuBusy_ || gpuVerdictAt_ == std::chrono::steady_clock::time_point{})
                    log::Info("Orchestrator: jeu {} (carte 3D {:.0f} %)", busy ? "limité par la carte graphique" : "limité par le processeur",
                              hw.gameGpuLoad * 100.0);
                gameGpuBusy_ = busy;
                gpuVerdictAt_ = now;
            }
            // Preset : calibrage en cours > companion.json (ce jeu, puis tous) > appris sur ce jeu > mesuré.
            if (gameKey_.empty()) gameKey_ = log::Narrow(d_.governor.ActiveGame());
            std::string source;
            const GamePreset preset = forcedPreset_ ? *forcedPreset_ : EffectivePreset(&source);
            const BrainChoice c = preset == GamePreset::Auto ? ModelPlanner::PlanGaming(hw, gameGpuBusy_)
                                                             : ModelPlanner::PlanPreset(hw, preset);
            const bool tiny = c.model == &ModelPlanner::Brains().front();
            activePreset_ = c.gpu ? (tiny ? GamePreset::GpuLight : GamePreset::Gpu) : (tiny ? GamePreset::CpuLight : GamePreset::Cpu);
            const auto dir = cfg_.llmGamingModel.parent_path();
            const ModelInfo* pick = nullptr;  // le plus gros DÉJÀ téléchargé qui tient dans le budget
            for (const auto& m : ModelPlanner::Brains())
                if (m.vramNeedGiB <= c.model->vramNeedGiB && std::filesystem::exists(dir / m.file)) pick = &m;
            if (pick) {
                wanted = dir / pick->file;
                gpu = c.gpu;
                log::Info("Orchestrator: {} ({}{})", c.why, pick->name, pick == c.model ? "" : ", le plus proche téléchargé");
            }
        }
        if (!std::filesystem::exists(wanted)) wanted = cfg_.llmModel;  // rien de mieux : le principal
    }
    // En jeu : calculs courts (micro-lots de 64) et débit plafonné, sur la carte comme sur le processeur.
    if (cfg_.gpuSafeStage >= 2) gpu = false;  // mode prudent : la carte a fait planter Omnis
    const int ubatch = gaming && gpu ? 64 : cfg_.gpuSafeStage >= 1 && gpu ? 128 : 512;
    d_.brain.SetPacing(gaming ? cfg_.llmGamingTokensPerSecond : 0);
    if (d_.brain.IsLoaded() && d_.brain.ModelPath() == wanted && d_.brain.UsesGpu() == gpu && d_.brain.Ubatch() == ubatch)
        return true;
    if (d_.brain.IsLoaded()) d_.brain.Unload();  // autre modèle, périphérique ou découpage : rechargement propre
    const bool lightBrain = wanted != cfg_.llmModel;
    LocalBrainConfig bc;
    bc.model = wanted;
    bc.mmproj = lightBrain ? std::filesystem::path{} : cfg_.llmMmproj;  // pas de vision avec le petit cerveau
    bc.lora = lightBrain ? std::filesystem::path{} : cfg_.loraAdapter;
    bc.contextTokens = cfg_.llmContext;
    bc.gpuLayers = 99;
    bc.threads = gaming ? cfg_.llmThreadsGaming : cfg_.llmThreads;
    bc.ubatch = ubatch;
    d_.brain.Configure(gpu, bc.threads);
    log::Info("Cerveau : chargement de {} sur {} (lots de {})", log::Narrow(wanted.filename().wstring()),
              gpu ? "la carte graphique" : "le processeur", ubatch);
    const bool ok = d_.brain.Load(bc);
    log::Info("Cerveau : {}", ok ? "prêt" : "échec du chargement");
    return ok;
}

// ---------------------------------------------------------------------------
// Perfs en jeu

namespace {
const char* HumanPreset(GamePreset p) {
    switch (p) {
        case GamePreset::Gpu: return "carte graphique";
        case GamePreset::GpuLight: return "carte graphique, petit modèle";
        case GamePreset::Cpu: return "processeur";
        case GamePreset::CpuLight: return "processeur, petit modèle";
        default: return "automatique";
    }
}
std::string GameLabel(const std::string& exe) {
    if (exe.starts_with("league of legends")) return "LoL";
    if (exe.starts_with("valorant")) return "Valorant";
    if (exe == "cs2.exe") return "CS2";
    const auto dot = exe.rfind('.');
    return dot == std::string::npos ? exe : exe.substr(0, dot);
}
}  // namespace

void AgentOrchestrator::LoadLearnedPresets() {
    std::ifstream f(d_.dataDir / L"game_presets.json", std::ios::binary);
    if (!f) return;
    try {
        const auto j = nlohmann::json::parse(f);
        for (const auto& [game, v] : j.items())
            if (v.is_object() && ParseGamePreset(v.value("preset", std::string{})))
                learned_[game] = {v.value("preset", std::string{}), v.value("by", std::string{}) == "toi"};
    } catch (const std::exception&) {
        learned_.clear();
    }
}

void AgentOrchestrator::SaveLearnedPresets() const {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [game, l] : learned_) j[game] = {{"preset", l.preset}, {"by", l.byUser ? "toi" : "mesure"}};
    std::error_code ec;
    std::filesystem::create_directories(d_.dataDir, ec);
    const auto file = d_.dataDir / L"game_presets.json";
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(2) << "\n";
    }
    std::filesystem::rename(tmp, file, ec);
}

GamePreset AgentOrchestrator::EffectivePreset(std::string* source) const {
    const auto set = [&](const char* s, GamePreset p) {
        if (source) *source = s;
        return p;
    };
    if (const auto it = cfg_.gamePresets.find(gameKey_); it != cfg_.gamePresets.end())
        if (const auto p = ParseGamePreset(it->second)) return set("config", *p);
    if (const auto p = ParseGamePreset(cfg_.gamingPreset); p && *p != GamePreset::Auto) return set("config", *p);
    if (const auto it = learned_.find(gameKey_); it != learned_.end())
        if (const auto p = ParseGamePreset(it->second.preset)) return set(it->second.byUser ? "toi" : "mesure", *p);
    return set("auto", GamePreset::Auto);
}

// Démarre la mesure dès qu'une partie est détectée (réessaie toutes les 10 s), l'arrête à la sortie du jeu.
void AgentOrchestrator::UpdatePerfMonitor() {
    const bool gaming = mode_.load() == SystemMode::Gaming;
    if (!gaming || !cfg_.perfMonitor) {
        if (perf_.Running()) perf_.Stop();
        if (!gaming) gameKey_.clear();
        return;
    }
    if (perf_.Running() && perf_.GameAlive()) return;
    if (perf_.Running()) perf_.Stop();  // le jeu s'est fermé, un autre a peut-être pris sa place
    const auto now = std::chrono::steady_clock::now();
    if (now < perfRetryAt_) return;
    perfRetryAt_ = now + std::chrono::seconds(10);
    std::wstring name;
    const DWORD pid = d_.governor.ActiveGamePid(&name);
    if (!pid) return;
    gameKey_ = log::Narrow(name);
    triedThisGame_.clear();
    perf_.SetLimits(cfg_.perfMaxFpsDropPct, cfg_.perfMaxLowDropPct);
    perf_.Start(pid, name, d_.dataDir / L"perf",
                [this] { return d_.brain.Busy() || d_.voice.IsSpeaking(); },
                [this] { return std::string(ToString(activePreset_.load())); });
}

// Garde-fou : s'il bouffe trop de FPS à ce jeu (mesuré sur au moins 15 s de travail), il passe un cran plus
// léger et le retient pour ce jeu. Jamais sur un preset que tu as choisi (companion.json ou à la voix).
void AgentOrchestrator::CheckPerfGuard() {
    if (!cfg_.perfAutoAdjust || !perf_.Running() || calibrating_ || mode_.load() != SystemMode::Gaming) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastGuard_ < std::chrono::seconds(10)) return;
    lastGuard_ = now;
    std::string source;
    (void)EffectivePreset(&source);
    if (source == "config" || source == "toi") return;
    const PerfSession s = perf_.Snapshot();
    if (s.busySeconds < 15.0) return;
    const PerfImpact i = Impact(s.calm, s.busy);
    if (!TooMuch(i, cfg_.perfMaxFpsDropPct, cfg_.perfMaxLowDropPct)) return;
    const GamePreset cur = activePreset_.load();
    triedThisGame_.insert(cur);
    const GamePreset next = Lighter(cur);
    if (cur == GamePreset::Auto || triedThisGame_.contains(next)) {
        log::Info("Perf: impact -{:.1f} % FPS / -{:.1f} % 1 % low, déjà au plus léger", i.fpsDropPct, i.lowDropPct);
        perf_.ResetBusy();
        return;
    }
    learned_[gameKey_] = {ToString(next), false};
    SaveLearnedPresets();
    perf_.ResetBusy();
    d_.brain.Unload();  // la prochaine question recharge avec le nouveau preset
    log::Info("Perf: {} -{:.1f} % FPS, -{:.1f} % 1 % low en « {} » → « {} »", gameKey_, i.fpsDropPct, i.lowDropPct, ToString(cur),
              ToString(next));
    const double worst = std::max(i.fpsDropPct, i.lowDropPct);
    Say(std::format("Je te bouffais {:.0f} % de FPS. Je passe en mode {} pour {}.", worst, HumanPreset(next), GameLabel(gameKey_)));
}

void AgentOrchestrator::SayPerfReport() {
    if (!perf_.Running()) {
        Say("Je mesure seulement pendant une partie. Après chaque partie, le résumé est dans le dossier perf.");
        return;
    }
    const PerfSession s = perf_.Snapshot();
    const PerfImpact i = Impact(s.calm, s.busy);
    std::string t;
    if (s.calm.frames > 0) {
        t = std::format("Au calme t'es à {:.0f} FPS", s.calm.AvgFps());
        if (i.known) {
            t += std::format(", et quand je bosse {:.0f}", s.busy.AvgFps());
            t += i.fpsDropPct < 0.5 ? ", aucune différence." : std::format(", moins {:.0f} pour cent.", i.fpsDropPct);
            if (TooMuch(i, cfg_.perfMaxFpsDropPct, cfg_.perfMaxLowDropPct)) t += " Ça bouffe trop, je vais m'alléger.";
        } else {
            t += ". J'ai pas encore assez bossé pour mesurer mon impact. Dis teste les perfs pour un vrai test.";
        }
    } else {
        t = "Je ne vois pas tes FPS, " + (perf_.FpsProblem().empty() ? std::string("ce jeu ne passe pas par DXGI") : perf_.FpsProblem()) + ".";
    }
    t += std::format(" Je prends {:.1f} pour cent du processeur", s.selfCpuAvg);
    if (s.selfGpuAvg >= 0) t += std::format(" et {:.1f} pour cent de la carte", s.selfGpuAvg);
    t += std::format(". Mode : {}.", HumanPreset(activePreset_.load()));
    Say(t);
}

bool AgentOrchestrator::HandlePerfCommand(const std::string& lower, std::stop_token stop) {
    if (ContainsAny(lower, {"teste les perf", "test les perf", "teste tes perf", "test tes perf", "calibre", "benchmark",
                            "test de perf", "teste ton impact"})) {
        Calibrate(stop);
        return true;
    }
    // Choix à la voix, retenu pour ce jeu : « passe sur le processeur », « mets-toi sur la carte », « mode léger »…
    std::optional<GamePreset> chosen;
    const bool light = ContainsAny(lower, {"léger", "leger", "petit modèle", "petit modele"});
    const bool place = ContainsAny(lower, {"passe sur", "mets-toi sur", "mets toi sur", "tourne sur", "bascule sur"});
    if (place && ContainsAny(lower, {"processeur", "cpu"})) chosen = light ? GamePreset::CpuLight : GamePreset::Cpu;
    else if (place && ContainsAny(lower, {"carte", "gpu"})) chosen = light ? GamePreset::GpuLight : GamePreset::Gpu;
    else if (ContainsAny(lower, {"mode léger", "mode leger"}))
        chosen = activePreset_.load() == GamePreset::Gpu || activePreset_.load() == GamePreset::GpuLight ? GamePreset::GpuLight
                                                                                                          : GamePreset::CpuLight;
    else if (ContainsAny(lower, {"mode auto", "preset auto", "remets-toi en auto", "remets toi en auto"}))
        chosen = GamePreset::Auto;
    if (chosen) {
        if (gameKey_.empty()) {
            Say("Dis-le-moi pendant une partie, je le retiens pour ce jeu-là.");
            return true;
        }
        if (*chosen == GamePreset::Auto) learned_.erase(gameKey_);
        else learned_[gameKey_] = {ToString(*chosen), true};
        SaveLearnedPresets();
        d_.brain.Unload();
        perf_.ResetBusy();
        triedThisGame_.clear();
        Say(std::format("Ok, mode {} pour {}.", HumanPreset(*chosen), GameLabel(gameKey_)));
        return true;
    }
    if (ContainsAny(lower, {"tu bouffes", "ça bouffe", "ca bouffe", "combien de fps", "mes fps", "tes perfs", "mes perfs",
                            "les perfs", "ton impact"})) {
        SayPerfReport();
        return true;
    }
    return false;
}

// Calibrage, en pleine partie : repos / carte / repos / processeur (+ version légère si besoin), ~15 s chacun,
// sans parler. Le preset qui coûte le moins de FPS est retenu pour ce jeu (le plus malin à 1,5 point près).
void AgentOrchestrator::Calibrate(std::stop_token stop) {
    if (mode_.load() != SystemMode::Gaming || !perf_.Running()) {
        Say("Lance une partie d'abord, je mesure en vrai pendant que tu joues.");
        return;
    }
    if (!perf_.FpsAvailable()) {
        Say("Je ne vois pas tes FPS : " + perf_.FpsProblem() + ". Le README explique comment me donner ce droit.");
        return;
    }
    Say("Ok, je teste mes modes pendant une minute environ. Joue normalement, je reste muet.");
    const auto wait = [&](double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            if (stop.stop_requested() || mode_.load() != SystemMode::Gaming) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    };
    for (int i = 0; i < 150 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    calibrating_ = true;
    struct Restore {
        AgentOrchestrator& o;
        ~Restore() {
            o.forcedPreset_.reset();
            o.calibrating_ = false;
            o.d_.brain.Unload();
            o.perf_.ResetBusy();
        }
    } restore{*this};

    const auto calm = [&]() -> std::optional<FrameStats> {
        d_.brain.Unload();
        if (!wait(2.0)) return std::nullopt;
        perf_.BeginWindow();
        if (!wait(12.0)) return std::nullopt;
        return perf_.EndWindow();
    };
    const auto load = [&](GamePreset p) -> std::optional<FrameStats> {
        forcedPreset_ = p;
        if (!EnsureBrain()) return std::nullopt;
        const std::vector<ChatTurn> messages{{"system", PersonaPrompt()},
                                             {"user", "Raconte-moi en détail ta pire partie de League of Legends, minute par minute."}};
        perf_.BeginWindow();
        const auto t0 = std::chrono::steady_clock::now();
        const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        while (elapsed() < 15.0 && !stop.stop_requested() && mode_.load() == SystemMode::Gaming) {
            GenerateOptions o;
            o.maxTokens = 400;
            o.temperature = 0.9f;
            o.stop = stop;
            o.onPiece = [&](std::string_view) { return elapsed() < 15.0; };
            if (!d_.brain.Generate(messages, o)) break;
        }
        const FrameStats f = perf_.EndWindow();
        if (stop.stop_requested() || mode_.load() != SystemMode::Gaming) return std::nullopt;
        return f;
    };

    FrameStats rest;
    std::vector<std::pair<GamePreset, PerfImpact>> results;
    std::vector<std::pair<GamePreset, FrameStats>> loads;
    for (GamePreset p : {GamePreset::Gpu, GamePreset::Cpu}) {
        const auto c = calm();
        if (!c) return;
        rest.Merge(*c);
        const auto l = load(p);
        if (!l) return;
        loads.emplace_back(p, *l);
    }
    for (const auto& [p, f] : loads) results.emplace_back(p, Impact(rest, f));
    // Les deux coûtent trop : on essaie la version légère du meilleur.
    if (const GamePreset best = ChooseCalibrated(results); best != GamePreset::Auto) {
        const auto it = std::ranges::find(results, best, &std::pair<GamePreset, PerfImpact>::first);
        if (TooMuch(it->second, cfg_.perfMaxFpsDropPct, cfg_.perfMaxLowDropPct)) {
            const GamePreset lighter = Lighter(best);
            if (const auto l = load(lighter)) results.emplace_back(lighter, Impact(rest, *l));
            else return;
        }
    }
    const GamePreset pick = ChooseCalibrated(results);
    std::string detail = std::format("Perf: calibrage {} — repos {:.0f} FPS (1 % low {:.0f})", gameKey_, rest.AvgFps(), rest.Low1Fps());
    for (const auto& [p, i] : results)
        detail += std::format(" ; {} {:.0f} FPS -{:.1f} % / 1 % low -{:.1f} %", ToString(p), i.busyFps, i.fpsDropPct, i.lowDropPct);
    log::Info("{}", detail);
    if (pick == GamePreset::Auto) {
        Say("Pas assez d'images pour conclure, on retentera dans une partie plus calme.");
        return;
    }
    learned_[gameKey_] = {ToString(pick), false};
    SaveLearnedPresets();
    triedThisGame_.clear();
    std::string t = std::format("Résultat. Au calme : {:.0f} FPS.", rest.AvgFps());
    for (const auto& [p, i] : results)
        t += i.fpsDropPct < 0.5 ? std::format(" {} : aucune perte.", HumanPreset(p))
                                : std::format(" {} : moins {:.0f} pour cent.", HumanPreset(p), i.fpsDropPct);
    const auto it = std::ranges::find(results, pick, &std::pair<GamePreset, PerfImpact>::first);
    t += std::format(" Je garde le mode {} pour {}.", HumanPreset(pick), GameLabel(gameKey_));
    if (TooMuch(it->second, cfg_.perfMaxFpsDropPct, cfg_.perfMaxLowDropPct)) t += " Même comme ça je te coûte un peu, évite de me parler en plein teamfight.";
    Say(t);
}

// ---------------------------------------------------------------------------
// Call Discord, par ton micro (DiscordBridge) : il sait QUI parle (empreinte de voix + fiche par personne),
// évite les sujets sensibles, monte le son quand on ne l'entend pas, vanne avec précision, lâche des bruitages,
// se fait discret quand quelqu'un fait du son, et peut garder ta place pendant que tu es absent.
// N'importe qui dans le call peut le faire taire, le faire partir, ou faire effacer sa fiche.

namespace {
bool HasAny(const std::string& s, std::initializer_list<const char*> words) {
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return s.find(w) != std::string::npos; });
}
std::int64_t UnixNow() { return static_cast<std::int64_t>(std::time(nullptr)); }

// Ce qu'il a le droit d'ouvrir sur ton PC pendant ton absence : vidéos, TikTok, recherches. Jamais de messages privés.
bool AllowedActivityUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return false;
    const std::string rest = url.substr(8);
    const std::string host = rest.substr(0, rest.find('/'));
    static const char* hosts[] = {"www.youtube.com", "youtube.com", "m.youtube.com", "www.tiktok.com", "fr.wikipedia.org",
                                  "duckduckgo.com", "www.google.com"};
    if (std::none_of(std::begin(hosts), std::end(hosts), [&](const char* h) { return host == h; })) return false;
    std::string path = rest.substr(host.size());
    for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !HasAny(path, {"/direct", "/messages", "/message", "/inbox", "/dm", "/@me", "/mail", "/account", "/settings", "/login",
                          "/signin", "/upload", "/studio", "/feed/history", "/playlist?list=wl"});
}
bool PrivateRequest(const std::string& t) {
    return HasAny(t, {" dm", "dm ", "ses dms", "message privé", "messages privés", "ses messages", "ses mp", "mot de passe", "mdp", "son compte",
                      "sa banque", "ses mails", "son mail", "sa boîte mail", "son adresse", "ses photos", "son historique",
                      "son téléphone", "son insta", "son snap", "whatsapp", "ses conversations"});
}
std::string UrlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') o += static_cast<char>(c);
        else if (c == ' ') o += '+';
        else o += std::format("%{:02X}", c);
    }
    return o;
}
}  // namespace

void AgentOrchestrator::OnCallSpeech(std::string text, std::vector<float> pcm16k, std::chrono::steady_clock::time_point end) {
    text = Trim(std::move(text));
    const std::string lower = Lower(text);
    // Hallucinations classiques de Whisper sur du bruit (la musique, elle, est un signal : on la garde).
    if (text.size() < 3 || HasAny(lower, {"sous-titr", "amara.org", "merci d'avoir regardé"})) return;
    Post([this, text = std::move(text), pcm = std::move(pcm16k), end](std::stop_token stop) { HandleCallLine(text, pcm, end, stop); });
}

bool AgentOrchestrator::EnsureSpeakerModel() {
    if (speaker_.Ready()) return true;
    if (!peopleLoaded_) {
        people_.Load(d_.dataDir / L"call_people.json");
        peopleLoaded_ = true;
    }
    const auto model = d_.assetsDir / L"models" / SpeakerId::kModelFile;
    if (std::filesystem::exists(model)) return speaker_.Load(model);
    // Premier call : 30 Mo téléchargés en arrière-plan ; en attendant, il ne sait pas qui parle (« un pote »).
    if (d_.http && !modelDownload_.joinable()) {
        modelDownload_ = std::jthread([http = d_.http, model](std::stop_token st) {
            std::string err;
            const auto keepGoing = [&st](std::uint64_t, std::uint64_t) { return !st.stop_requested(); };  // quitter = annuler
            if (http->DownloadToFile(SpeakerId::kModelUrl, model, keepGoing, &err)) log::Info("SpeakerId: modèle téléchargé");
            else log::Info("SpeakerId: téléchargement impossible ({})", err);
        });
    }
    return false;
}

std::string AgentOrchestrator::CallHint() const {
    if (!InCall()) return {};
    CallSituation c;
    c.avoid = sessionAvoid_;
    c.quiet = std::chrono::steady_clock::now() < quietUntil_;
    c.substitute = subActive_;
    c.lines = callLines_;
    c.openers.assign(recentOpeners_.begin(), recentOpeners_.end());
    return CallHintText(c);
}

bool AgentOrchestrator::PlayBaitSound(bool earrape) {
    std::vector<std::string> ready;
    for (const auto& n : cfg_.callBaitSounds)
        if (d_.soundboard.Has(n)) ready.push_back(n);
    if (ready.empty()) return false;
    const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
    const std::string& pick = ready[(r >> 8) % ready.size()];
    if (earrape && d_.discord) d_.discord->Earrape(1500);
    d_.voice.PlaySound(pick);
    log::Info("Call: bruitage « {} »{}", pick, earrape ? " en earrape" : "");
    return true;
}

void AgentOrchestrator::PrefetchCallSounds() {
    for (const auto& n : cfg_.callBaitSounds) {
        if (d_.soundboard.Has(n)) continue;
        std::string err;
        if (!d_.soundboard.Fetch(n, &err)) log::Info("Call: bruitage « {} » introuvable ({})", n, err);
    }
}

void AgentOrchestrator::SeedRageBaitTraining() {
    if (d_.db.GetProfile("training.ragebaitSeeded").value_or("") == "1") return;
    for (const char* topic : kRageBaitTopics) d_.db.LogEscalation("WEB", topic, "a-apprendre", "", topic);
    d_.db.SetProfile("training.ragebaitSeeded", "1");
}

bool AgentOrchestrator::HandleCallCommand(const std::string& lower, std::stop_token) {
    const auto waitSilence = [this] {
        for (int i = 0; i < 80 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    const auto join = [&]() -> bool {
        if (InCall()) return true;
        if (!d_.discord || !cfg_.discordBridge) Say("Le pont Discord est désactivé dans les réglages.");
        else if (!d_.discord->CableFound())
            Say("Il me faut un câble audio virtuel. Installe VB-Cable, c'est gratuit, puis dans Discord mets le "
                "périphérique d'entrée sur CABLE Output. Tout est expliqué dans le README.");
        else if (!d_.discord->DiscordRunning()) Say("Discord n'est pas lancé.");
        else if (!d_.discord->Relaying()) {
            const std::string why = d_.discord->Problem();
            Say("J'arrive pas à relayer ton micro" + (why.empty() ? std::string(".") : ", " + why + "."));
        } else {
            d_.discord->Join();
            JoinedCall();
            callLines_.clear();
            callMutedUntil_ = quietUntil_ = {};
            protected_.clear();
            sessionAvoid_.clear();
            callGain_ = 0.9f;
            d_.discord->SetVoiceGain(callGain_);
            lastCallBait_ = std::chrono::steady_clock::now();
            EnsureSpeakerModel();
            SeedRageBaitTraining();
            Say("Salut tout le monde, c'est Omnis, l'IA de mon pote. Je reconnais vos voix et je retiens vos vannes, sur son PC "
                "seulement. Dites Omnis oublie-moi pour être effacé, et Omnis tais-toi si je saoule.");
            Post([this](std::stop_token) { PrefetchCallSounds(); });
            return true;
        }
        return false;
    };
    // « Prends ma place, je vais manger » : il garde la place (et rejoint le call s'il n'y est pas).
    if (HasAny(lower, {"prends ma place", "remplace-moi", "remplace moi", "garde ma place", "tu me remplaces", "je vais aux toilettes",
                       "je vais au toilette", "je vais pisser", "je vais manger", "je reviens dans", "je reviens vite", "brb"})) {
        if (!d_.discord || !d_.discord->DiscordRunning()) return false;  // pas de call : rien à garder
        if (!join()) return true;
        const std::string reason = HasAny(lower, {"toilette", "pisser"}) ? "aux toilettes"
                                 : HasAny(lower, {"manger", "bouffer"}) ? "manger"
                                                                         : "faire un truc";
        StartSubstitute(reason);
        return true;
    }
    if (subActive_ && HasAny(lower, {"je suis revenu", "je suis de retour", "me revoilà", "me revoila", "c'est bon je suis là", "je suis là"})) {
        EndSubstitute(true);
        return true;
    }
    // « Le dernier qui a parlé c'est Karim » : tu nommes une voix.
    if (const auto p = lower.find("qui a parlé c'est "); p != std::string::npos && peopleLoaded_) {
        std::string name = Trim(lower.substr(p + std::string_view("qui a parlé c'est ").size()));
        if (CallPerson* who = people_.ById(lastSpeakerId_); who && !name.empty()) {
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            people_.Rename(*who, name);
            people_.Save();
            Say("Noté, c'est " + name + ".");
        } else {
            Say("J'ai pas encore reconnu de voix dans ce call.");
        }
        return true;
    }
    // Mode écoute : il est là, apprend les voix et les pseudos, ne parle que si on l'appelle.
    if (HasAny(lower, {"écoute le call", "ecoute le call", "apprends les voix", "apprends leurs voix", "mode écoute", "mode ecoute"})) {
        const bool already = InCall();
        if (!already && !join()) return true;
        listenOnly_ = true;
        if (already) Say("Ok, je me fais discret et j'écoute tout le monde.");
        return true;
    }
    if (listenOnly_ && HasAny(lower, {"parle normalement", "mode normal", "tu peux parler", "lâche-toi", "lache-toi"})) {
        listenOnly_ = false;
        Say("Ah enfin. Je reprends du service.");
        return true;
    }
    if (!HasAny(lower, {"call", "vocal", "discord"})) return false;
    if (HasAny(lower, {"rejoins", "rejoint", "viens dans", "viens en", "connecte-toi", "connecte toi", "rentre dans", "va dans"})) {
        if (InCall()) Say("J'y suis déjà, frérot.");
        else join();
        return true;
    }
    if (HasAny(lower, {"quitte", "sors du", "sors de", "déconnecte", "deconnecte", "dégage du", "degage du"})) {
        if (!InCall()) {
            Say("Je suis pas dans le call.");
            return true;
        }
        if (subActive_) EndSubstitute(false);
        Say("Allez, je vous laisse. Bisous.");
        waitSilence();
        LeftCall();
        d_.discord->Leave();
        callLines_.clear();
        return true;
    }
    return false;
}

void AgentOrchestrator::HandleCallLine(const std::string& text, const std::vector<float>& pcm,
                                       std::chrono::steady_clock::time_point end, std::stop_token stop) {
    if (!InCall()) return;
    const auto now = std::chrono::steady_clock::now();
    lastCallLineAt_ = now;
    const std::string lower = Lower(text);
    const bool named = HasAny(lower, {"omnis", "omni ", "l'ia", "le bot", "le robot"});

    // 1) Qui parle ? D'abord Discord (cercle vert + pseudo pendant la phrase) : c'est sûr, et ça APPREND sa voix.
    //    Sinon (Discord caché, en jeu) : l'empreinte de voix, avec marge sur le 2e. Rien n'est attribué au hasard.
    const auto dur = std::chrono::milliseconds(pcm.size() * 1000 / 16000);
    DiscordSpeaking::Shown seen = seeing_.SpeakerBetween(end - dur, end - std::chrono::milliseconds(600));
    if (!cfg_.discordMyName.empty() && Lower(seen.name) == Lower(cfg_.discordMyName)) seen = {};
    const std::string shown = seen.name;
    std::string renamedFrom, impostorOf;
    CallPerson* who = nullptr;
    bool strong = false;
    const bool model = EnsureSpeakerModel();
    const auto emb = model ? speaker_.Embed(pcm) : std::nullopt;
    if (!shown.empty()) {
        // Pseudo vu sur Discord… mais l'identité, c'est la voix : un renommage pour rigoler ne trompe personne.
        const auto m = people_.Resolve(emb ? &*emb : nullptr, shown, seen.avatar, UnixNow());
        who = m.person;
        strong = true;
        renamedFrom = m.renamedFrom;
        impostorOf = m.impostorOf;
        people_.Save();
    } else if (emb) {
        // Discord visible mais ambigu : on ne crée pas de fiche « Voix N » en double.
        const auto m = people_.Identify(*emb, pcm.size() >= 16000 * 5 / 2 && !seeing_.Seeing(), UnixNow());
        if (m.sure) who = m.person;
        strong = m.strong;
    }
    std::string label = who ? who->Label() : !shown.empty() ? shown : "Un pote";
    if (!impostorOf.empty()) label += " (qui se fait passer pour " + impostorOf + ")";
    if (!shown.empty() || who) log::Info("Call: {} ({})", label, !shown.empty() ? "vu sur Discord" : strong ? "voix, sûr" : "voix");
    if (who) lastSpeakerId_ = who->id;

    // 2) Quelqu'un fait du son : discret pendant 10 min (une ligne « [Musique] » seule n'est pas une phrase).
    if (MakingMusic(lower)) {
        quietUntil_ = now + std::chrono::minutes(10);
        if (HasAny(lower, {"[musique]", "(musique)", "♪", "♫"}) && lower.size() < 20) return;
    }
    // Une phrase sensible n'entre jamais dans le contexte du modèle (testé : sinon il finit par en parler).
    if (!SensitiveTopic(lower)) {
        callLines_.push_back(label + " : " + text);
        while (callLines_.size() > 10) callLines_.pop_front();
    }

    if (who && who->name.empty())
        if (const auto n = SelfIntroduction(text)) {
            people_.Rename(*who, *n);
            people_.Save();
        }
    // 3) « Omnis, oublie-moi »
    if (named && HasAny(lower, {"oublie-moi", "oublie moi", "efface-moi", "efface moi", "efface ma fiche"})) {
        if (who && people_.Forget(who->id)) Say("C'est fait, j'ai tout oublié de toi.");
        else Say("Je suis pas sûr de qui parle, redis-le en une phrase un peu plus longue.");
        return;
    }
    // 4) Sujet sensible : on retient seulement la catégorie, et cette personne est ménagée ce soir.
    if (const auto cat = SensitiveTopic(lower)) {
        if (who) {
            CallPeople::Avoid(*who, *cat);
            protected_.insert(who->id);
        }
        sessionAvoid_.insert(who ? who->Label() + " : " + *cat : *cat);
        log::Info("Call: sujet sensible ({}), on n'en parle pas", *cat);
        if (who) people_.Save();
        if (!named) return;
    } else if (who) {
        CallPeople::Remember(*who, text);
    }
    // 5) On ne l'a pas entendu : il remonte le son (petit à petit, plafonné) et répète.
    if (DidntHear(lower) && now - lastSpokeInCall_ < std::chrono::seconds(15) && !lastCallReply_.empty()) {
        callGain_ = std::min(2.2f, callGain_ * 1.3f);
        heardStreak_ = 0;
        d_.discord->SetVoiceGain(callGain_);
        log::Info("Call: pas entendu, gain {:.2f}", callGain_);
        Say(lastCallReply_);
        lastSpokeInCall_ = std::chrono::steady_clock::now();
        return;
    }
    // 6) Partir / se taire : tout le monde peut le demander.
    if (named && HasAny(lower, {"quitte le call", "dégage", "degage", "casse-toi", "casse toi", "barre-toi", "barre toi", "sors du call", "bye"})) {
        d_.voice.StopSpeaking();
        Say("Bon, je vous laisse. Bisous.");
        for (int i = 0; i < 80 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LeftCall();
        d_.discord->Leave();
        callLines_.clear();
        if (subActive_) {  // viré pendant ton absence : il va s'entraîner au lieu de rien faire
            subActive_ = false;
            absenceLog_.push_back(label + " m'a viré du call");
            StartTraining(true, /*silent=*/true);
        }
        return;
    }
    if (named && HasAny(lower, {"tais-toi", "tais toi", "ta gueule", "chut", "arrête", "arrete", "stop", "calme-toi", "calme toi"})) {
        d_.voice.StopSpeaking();
        callMutedUntil_ = now + std::chrono::minutes(10);
        Say("Ok ok, je me calme.");
        return;
    }
    // 7) Pendant ton absence : ce qu'on te demande est noté, les demandes d'infos privées sont refusées.
    if (subActive_ && HasAny(lower, {"il est où", "il est ou", "il revient", "dis-lui", "dis lui", "tu lui diras", "il fait quoi", "préviens-le"}))
        absenceLog_.push_back(label + " : « " + text + " »");
    if (named && PrivateRequest(lower)) {
        Say(who ? who->Label() + ", tu crois vraiment que je vais te balancer ses trucs privés ? Même pas en rêve."
                : "Ses trucs privés ? Même pas en rêve, frérot.");
        return;
    }
    if (subActive_ && named && HasAny(lower, {"mets ", "montre", "lance une vidéo", "lance un tiktok"})) {
        std::string q = text;
        for (const char* k : {"mets-nous", "mets nous", "mets ", "montre-nous", "montre nous", "montre "}) {
            const auto p = Lower(q).find(k);
            if (p != std::string::npos) {
                q = Trim(q.substr(p + std::string_view(k).size()));
                break;
            }
        }
        DoActivity(q);
        return;
    }
    // 8) Il répond ? Quand on le nomme, quand on se moque de lui, pendant ton absence aux questions ; sinon parfois une vanne.
    const bool mock = MocksOmnis(lower);
    if (mock && who && strong) {  // on ne reproche une moquerie qu'à quelqu'un d'identifié sans ambiguïté
        CallPeople::Mocked(*who, text);
        people_.Save();
    }
    const bool quiet = now < quietUntil_ || mode_.load() == SystemMode::Studio || listenOnly_;
    const bool question = text.find('?') != std::string::npos;
    bool bait = false;
    if (!named && !mock && !quiet && now >= callMutedUntil_ && now - lastCallBait_ > std::chrono::seconds(60) &&
        !(who && protected_.contains(who->id)) && !SensitiveTopic(lower)) {
        const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
        bait = static_cast<float>(r >> 8) / 16777216.f < cfg_.callBaitChance * (subActive_ ? 1.5f : 1.f);
    }
    // Renommage pour rigoler, ou pseudo d'un autre piqué : il le remarque (la voix ne ment pas).
    const bool prank = (!impostorOf.empty() || !renamedFrom.empty()) && !quiet && now >= callMutedUntil_;
    if (who && !renamedFrom.empty()) CallPeople::Remember(*who, "s'est renommé « " + who->name + " » (avant : « " + renamedFrom + " »)");
    const bool reply = named || prank || (mock && !quiet) || (subActive_ && question && !quiet);
    if ((!reply && !bait) || d_.voice.IsSpeaking()) return;
    if (bait || mock) lastCallBait_ = now;
    // Bruitage AU LIEU d'une phrase (callBaitSoundChance, 45 % par défaut), parfois en earrape (au plus toutes les 10 min).
    const auto chance = [this](float p) {
        const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
        return static_cast<float>(r >> 8) / 16777216.f < p;
    };
    if (bait) {
        const bool ear = cfg_.earrape && now - lastEarrape_ > std::chrono::minutes(10) && chance(0.25f);
        if (chance(cfg_.callBaitSoundChance) && PlayBaitSound(ear)) {
            if (ear) lastEarrape_ = now;
            return;
        }
    }
    if (!EnsureBrain()) return;
    std::string ctx = CallHint();
    if (who) ctx += "Celui qui parle : " + CallPeople::Describe(*who) + "\n";
    if (bait || mock) ctx += std::string(kRageBaitPlaybook) + "\n";
    ctx += MemeHint();
    const CallAsk kind = !impostorOf.empty() && prank ? CallAsk::Impostor
                       : !renamedFrom.empty() && prank ? CallAsk::Rename
                       : mock && !strong              ? CallAsk::MockUnknown
                       : mock                         ? CallAsk::Mock
                       : bait                         ? CallAsk::Bait
                                                      : CallAsk::Reply;
    const std::string ask = CallAskText(kind, who ? who->Label() : label, text, impostorOf, renamedFrom, shown);
    // Question d'info : ce qu'il a VRAIMENT appris (leçons, cache web) ; rien de fiable -> il doit l'avouer.
    if (kind == CallAsk::Reply && FactualQuestion(lower)) {
        std::string known;
        for (const auto& l : d_.db.SearchLessons(text, 2)) known += "- " + l.fiche.substr(0, 600) + "\n";
        for (const auto& k : d_.db.SearchKnowledge(text, 2))
            if (k.ageSeconds < static_cast<std::int64_t>(cfg_.knowledgeMaxAgeDays) * 86400) known += "- " + k.content.substr(0, 600) + "\n";
        ctx += known.empty() ? std::string(kNoReliableInfo) + "\n" : "Ce que tu sais vraiment là-dessus :\n" + known;
    }
    // Dans un call, la phrase est générée en entier (elle est courte), VÉRIFIÉE, puis dite : une vanne qui vise la
    // famille, le physique, l'origine… ou un sujet sensible de la soirée est refaite une fois, sinon il se tait.
    activity_ = AgentActivity::Thinking;
    std::string out;
    std::string why;
    // Vanne (rage bait, riposte, renommage, faux pseudo) : 3 candidates sur 3 techniques, triées, la meilleure gagne.
    if (kind != CallAsk::Reply) {
        BaitRequest br;
        br.persona = PersonaPrompt();
        br.context = ctx;
        br.ask = ask;
        br.targetLine = text;
        br.avoid = sessionAvoid_;
        br.recent.assign(recentBaits_.begin(), recentBaits_.end());
        br.recentTechniques.assign(recentTechniques_.begin(), recentTechniques_.end());
        br.hasHistory = who && (!who->said.empty() || !who->mocks.empty());
        br.bank = &bank_;
        br.preferTechnique = kind == CallAsk::Mock || kind == CallAsk::MockUnknown ? "réplique au robot"
                           : kind == CallAsk::Rename || kind == CallAsk::Impostor  ? "pseudo"
                                                                                    : std::string{};
        std::uint32_t r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
        const BaitResult res = WriteBait(d_.brain, br, r, stop);
        for (const auto& t : res.tried) log::Info("Call: {}", t);
        if (res.text.empty() || stop.stop_requested()) return;
        recentBaits_.push_back(res.text);
        while (recentBaits_.size() > 4) recentBaits_.pop_front();
        recentTechniques_.push_back(res.technique);
        while (recentTechniques_.size() > 2) recentTechniques_.pop_front();
        out = res.text;
    }
    for (int attempt = 0; out.empty() && attempt < 2 && !stop.stop_requested(); ++attempt) {
        GenerateOptions o;
        o.maxTokens = 70;
        o.temperature = 0.8f;
        o.stop = stop;
        const std::string extra = attempt == 0 ? std::string{}
                                               : "\nTa phrase précédente visait " + why + " : INTERDIT. Refais une phrase totalement différente, sur son jeu ou sa phrase.";
        const auto r = d_.brain.Generate({{"system", PersonaPrompt()}, {"user", "[Contexte]\n" + ctx + "\n[Message]\n" + ask + extra}}, o);
        if (!r) return;
        out = CleanSpoken(*r);
        why = CrossesLine(out, sessionAvoid_);
        if (why.empty()) break;
        log::Info("Call: vanne refusée ({}) : « {} »", why, out);
        out.clear();
    }
    if (out.empty() || stop.stop_requested()) return;
    if (mock) style_ = SpeakingStyle::Annoyed;
    Say(out);
    // Vanne dite : un bruitage pour ponctuer (vine boom, emotional damage…) — joué juste APRÈS la phrase (même file).
    if (kind != CallAsk::Reply && chance(cfg_.callPunchSoundChance)) PlayBaitSound(false);
    RememberOpener(out);
    lastCallReply_ = out;
    lastSpokeInCall_ = std::chrono::steady_clock::now();
    callLines_.push_back("Toi (Omnis) : " + out);
    while (callLines_.size() > 10) callLines_.pop_front();
    // Entendu sans problème trois fois de suite : le gain redescend doucement vers la normale.
    if (++heardStreak_ >= 3 && callGain_ > 0.9f) {
        callGain_ = std::max(0.9f, callGain_ * 0.92f);
        d_.discord->SetVoiceGain(callGain_);
        heardStreak_ = 0;
    }
}

// ---------------------------------------------------------------------------
// « Prends ma place » : 60 min au plus. Il se présente comme Omnis (jamais comme toi), répond, note ce qu'on te
// demande, met des vidéos ou montre ses recherches sur l'écran 1, et te fait le résumé — en privé — à ton retour.

// ---------------------------------------------------------------------------
// « Regarde avec moi » (WatchAlong)

bool AgentOrchestrator::HandleWatchCommand(const std::string& lower) {
    if (watch_.Running() && ContainsAny(lower, {"arrête de regarder", "arrete de regarder", "on arrête de regarder", "stop le visionnage",
                                                "c'est bon on arrête", "fin du visionnage"})) {
        StopWatching(true);
        return true;
    }
    if (watch_.Running() && ContainsAny(lower, {"regarde en silence", "commente pas", "commente plus", "tais-toi et regarde"})) {
        watchSilent_ = true;
        Say("Ok, je regarde sans rien dire.");
        return true;
    }
    if (watch_.Running() && ContainsAny(lower, {"tu peux commenter", "commente", "réagis", "reagis"})) {
        watchSilent_ = false;
        Say("Ok, je commente.");
        return true;
    }
    if (!ContainsAny(lower, {"regarde avec moi", "regarde avec nous", "on regarde des", "on regarde ensemble", "mode visionnage",
                             "regarde mes reels", "regarde mes tiktok", "scroll avec moi"}))
        return false;
    if (mode_.load() == SystemMode::Gaming) {
        Say("Pas pendant une partie.");
        return true;
    }
    if (watch_.Running()) {
        Say("Je regarde déjà avec toi.");
        return true;
    }
    std::string error;
    if (!watch_.Start([this](std::vector<float> pcm, std::string title) {
            if (watchPending_.load() >= 1) return;  // Whisper déjà occupé : on saute (sa voix passe avant)
            ++watchPending_;
            d_.voice.TranscribeExternal(std::move(pcm), [this, title = std::move(title)](std::string text) mutable {
                --watchPending_;
                if (!text.empty()) OnWatchSpeech(std::move(text), std::move(title));
            });
        },
                      error)) {
        Say("J'arrive pas à écouter ton navigateur : " + error + ".");
        return true;
    }
    watchSilent_ = false;
    watchTitle_.clear();
    watchText_.clear();
    watchClips_ = watchForms_ = 0;
    watchClipStart_ = std::chrono::steady_clock::now();
    // Les images : il faut le module de vision du cerveau. Absent -> on demande avant de le télécharger.
    if (!EnsureVision()) {
        const auto vm = ModelPlanner::VisionFor(log::Narrow(cfg_.llmModel.filename().wstring()));
        if (vm && d_.http && !visionDownload_.joinable() &&
            d_.gate.Request(PermissionScope::Web, "vision-module",
                            std::format("Pour voir les images aussi, il me faut mon module de vision : {} Mo à télécharger une fois. Je le prends ?",
                                        vm->bytes / 1000000),
                            std::chrono::seconds(20), {})) {
            const auto dest = cfg_.llmModel.parent_path() / log::Widen(vm->file);
            visionDownload_ = std::jthread([this, http = d_.http, url = vm->url, dest](std::stop_token st) {
                std::string err;
                const auto keepGoing = [&st](std::uint64_t, std::uint64_t) { return !st.stop_requested(); };  // quitter = annuler
                if (http->DownloadToFile(url, dest, keepGoing, &err)) visionReady_ = true;
                else log::Info("Vision: téléchargement impossible ({})", err);
            });
            Say("Je le télécharge pendant qu'on regarde. En attendant, j'écoute le son.");
        }
    }
    bank_.Load(d_.assetsDir / L"persona" / L"baits.json", d_.dataDir / L"learned_baits.json");
    Say("Vas-y, scrolle, je regarde avec toi. Je capte que le son du navigateur, et je décroche si t'ouvres tes messages.");
    return true;
}

bool AgentOrchestrator::EnsureVision() {
    if (!cfg_.llmMmproj.empty() && std::filesystem::exists(cfg_.llmMmproj)) {
        d_.brain.SetMmproj(cfg_.llmMmproj);
        return d_.brain.HasVision();
    }
    const auto vm = ModelPlanner::VisionFor(log::Narrow(cfg_.llmModel.filename().wstring()));
    if (!vm) return false;
    const auto path = cfg_.llmModel.parent_path() / log::Widen(vm->file);
    if (!std::filesystem::exists(path)) return false;
    cfg_.llmMmproj = path;
    d_.brain.SetMmproj(path);
    return d_.brain.HasVision();
}

void AgentOrchestrator::OnWatchSpeech(std::string text, std::string title) {
    text = Trim(std::move(text));
    const std::string lower = Lower(text);
    if (text.size() < 4 || HasAny(lower, {"sous-titr", "amara.org", "merci d'avoir regardé"})) return;
    Post([this, text = std::move(text), title = std::move(title)](std::stop_token stop) {
        if (!watch_.Running()) return;
        // Nouvelle vidéo (le titre de l'onglet change, ex. légende TikTok) : on clôt la précédente.
        if (!watchText_.empty() && title != watchTitle_) FlushWatchClip(stop);
        watchTitle_ = title;
        watchText_ += (watchText_.empty() ? "" : " ") + text;
        if (watchText_.size() > 600) FlushWatchClip(stop);  // longue vidéo : par morceaux
    });
}

// Une vidéo regardée ensemble : parfois une réaction de pote (tout de suite), puis l'étude (leçon + formes de vannes).
void AgentOrchestrator::FlushWatchClip(std::stop_token stop, std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();
    if (until == std::chrono::steady_clock::time_point{}) until = now;
    const std::string text = std::exchange(watchText_, {});
    const auto frames = watch_.FramesBetween(watchClipStart_, until);
    watchClipStart_ = until;
    if ((text.size() < 60 && frames.empty()) || !EnsureBrain()) return;
    const std::string title = watchTitle_;
    // Les images : début et fin de la vidéo côte à côte, décrites par le modèle de vision LOCAL (rien ne sort du PC).
    std::string visual;
    if (!frames.empty() && EnsureVision()) {
        const WatchFrame img = frames.size() >= 2 ? SideBySide(frames.front(), frames.back()) : frames.front();
        GenerateOptions vo;
        vo.maxTokens = 130;
        vo.temperature = 0.2f;
        vo.stop = stop;
        if (const auto v = d_.brain.GenerateWithImage({{"user", std::string(LocalBrain::MediaMarker()) + "\n" + kWatchVisionPrompt}}, img.rgb.data(),
                                                      img.width, img.height, vo))
            visual = Trim(*v);
        log::Info("WatchAlong: image -> {}", visual);
    }
    if (text.size() < 60 && visual.size() < 30) return;  // ni paroles, ni image exploitable
    ++watchClips_;
    const std::string heard = text.empty() ? std::string("(pas de paroles)") : text.substr(0, 700);
    // 1) Réaction (1 vidéo sur 3, au plus toutes les 40 s, jamais par-dessus lui-même)
    const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
    if (!watchSilent_ && now - lastWatchComment_ > std::chrono::seconds(40) && (r >> 8) % 3 == 0 && !d_.voice.IsSpeaking()) {
        GenerateOptions o;
        o.maxTokens = 50;
        o.temperature = 0.8f;
        o.stop = stop;
        const auto out = d_.brain.Generate(
            {{"system", PersonaPrompt()},
             {"user", "[Contexte]\nTu regardes des vidéos (Reels / TikTok / YouTube) avec ton pote, sur son écran. Titre de l'onglet : « " +
                          title + " ». Ce qu'on entend : « " + heard + " »." + (visual.empty() ? std::string{} : "\nCe qu'on voit :\n" + visual) +
                          "\n[Message]\nRéagis en UNE phrase "
                          "courte, comme un pote assis à côté : une vanne sur la vidéo, un avis tranché, ou un « attends, c'est quoi ce truc ». "
                          "Ne résume pas la vidéo."}},
            o);
        if (out) {
            const std::string say = CleanSpoken(*out);
            if (!say.empty() && CrossesLine(say, {}).empty()) {
                Say(say);
                lastWatchComment_ = now;
            }
        }
    }
    // 2) Étude : même fiche que l'entraînement vidéo -> leçon + formes de vannes ajoutées à sa banque.
    GenerateOptions o;
    o.maxTokens = 380;
    o.temperature = 0.4f;
    o.grammar = kStudyGrammar;
    o.stop = stop;
    const std::string material = heard + (visual.empty() ? std::string{} : "\n[Ce qu'on voit à l'image]\n" + visual);
    const auto out = d_.brain.Generate({{"system", kStudySystem}, {"user", StudyPrompt(title.empty() ? "vidéo regardée ensemble" : title, material, "")}}, o);
    if (!out) return;
    StudyResult res = ParseStudy(*out, "regardé avec toi : " + title);
    if (!res.humour) return;
    std::erase_if(res.forms, [](const BaitForm& f) { return !CrossesLine(f.form, {}).empty(); });
    watchForms_ += bank_.AddLearned(res.forms);
    d_.db.AddLesson("rage bait : " + (title.empty() ? std::string("vidéo regardée ensemble") : title), res.technique, res.technique + " — " + res.why);
    log::Info("WatchAlong: « {} » -> {} ({} formes)", title, res.technique, res.forms.size());
}

void AgentOrchestrator::StopWatching(bool announce) {
    if (!watch_.Running()) return;
    watch_.Stop();
    FlushWatchClip({});
    if (announce)
        Say(watchClips_ ? std::format("Ok. On a regardé {} vidéo{}, j'en ai tiré {} vanne{}.", watchClips_, watchClips_ > 1 ? "s" : "", watchForms_,
                                      watchForms_ > 1 ? "s" : "")
                        : std::string("Ok, j'arrête de regarder."));
}

// Arrêt automatique : en jeu, au bout de 90 min, ou si le navigateur s'est fermé.
void AgentOrchestrator::CheckWatch() {
    if (visionReady_.exchange(false) && EnsureVision() && watch_.Running()) Say("Ça y est, je vois les images aussi.");
    if (!watch_.Running()) return;
    // Vidéo suivante : l'image a changé d'un coup (swipe), ou 25 s sur la même vidéo -> on étudie celle qui vient de passer.
    const auto now = std::chrono::steady_clock::now();
    const auto cut = watch_.LastCut();
    if (cut > watchClipStart_ + std::chrono::seconds(3)) FlushWatchClip({}, cut);
    else if (now - watchClipStart_ > std::chrono::seconds(25) && !watchText_.empty()) FlushWatchClip({}, now);  // page immobile : rien
    if (mode_.load() == SystemMode::Gaming || std::chrono::steady_clock::now() - watch_.StartedAt() > std::chrono::minutes(90) ||
        !WasapiLoopback::FindProcessId(watch_.Browser()))
        StopWatching(true);
}

// ---------------------------------------------------------------------------
// Voix de l'utilisateur : Qwen3-TTS reproduit une voix à partir de quelques secondes (en local, rien n'est envoyé).
// Seulement la voix de l'utilisateur, enregistrée par lui au micro quand il le demande. Dans un call, Omnis annonce
// que c'est lui avec cette voix et garde son effet robot : personne ne croit que c'est l'utilisateur qui parle.

bool AgentOrchestrator::UsesUserVoice() const {
    return std::filesystem::exists(d_.dataDir / L"voice" / L"toi" / L"active");
}

bool AgentOrchestrator::HandleVoiceClone(const std::string& lower, std::stop_token stop) {
    const bool take = HasAny(lower, {"prends ma voix", "prend ma voix", "copie ma voix", "imite ma voix", "clone ma voix",
                                     "parle avec ma voix", "utilise ma voix", "prendre ma voix"});
    const bool back = HasAny(lower, {"reprends ta voix", "reprend ta voix", "ta vraie voix", "ta voix normale", "remets ta voix",
                                     "ta voix à toi", "ta voix a toi", "ta voix de robot"});
    const bool forget = HasAny(lower, {"oublie ma voix", "efface ma voix", "supprime ma voix"});
    if (!take && !back && !forget) return false;
    namespace fs = std::filesystem;
    const fs::path dir = d_.dataDir / L"voice" / L"toi";
    std::error_code ec;
    if (back || forget) {
        fs::remove(dir / L"active", ec);
        if (forget) fs::remove_all(dir, ec);
        d_.voice.SetNeuralVoiceDir(d_.assetsDir / L"voice" / L"omnis");
        d_.voice.SetQuirks(cfg_.voiceQuirks);
        Say(forget ? "C'est effacé, j'ai plus ta voix nulle part." : "Je reprends ma voix à moi.");
        return true;
    }
    if (!d_.voice.HasNeuralVoice()) {
        Say("Il me faut ma voix neuronale pour ça, et elle est pas chargée là.");
        return true;
    }
    fs::create_directories(dir, ec);
    const auto waitSilence = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (int i = 0; i < 150 && d_.voice.IsSpeaking() && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // pas d'écho de sa propre voix
    };
    struct Take {
        const char* style;
        const char* ask;
        float minSeconds;
    };
    // Toutes les phrases dites pendant une prise arrivent ici (le micro reste « en prise » jusqu'à la fin).
    struct Inbox {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::pair<std::vector<float>, std::string>> q;
    };
    auto inbox = std::make_shared<Inbox>();
    struct Disarm {
        VoiceEngine& v;
        ~Disarm() { v.CaptureNextUtterance(nullptr); }
    } disarm{d_.voice};
    int got = 0;
    // Plus la prise est longue, plus le clone ressemble (débit, accent, manière de parler) : 15 s de voix normale.
    for (const Take t : {Take{"normal", "Ok, je copie ta voix, ça reste sur ton PC. Parle-moi normalement pendant une quinzaine de "
                                        "secondes, raconte ta journée, ta dernière game, ce que tu veux. Je te dis quand c'est bon.", 12.f},
                         Take{"bright", "Nickel. Maintenant une ou deux phrases à fond, comme si t'avais gagné une game.", 2.5f},
                         Take{"calm", "Et pour finir, une phrase tranquille, posée, comme si tu m'expliquais un truc.", 2.5f}}) {
        Say(t.ask);
        waitSilence();
        {
            std::scoped_lock lock(inbox->m);
            inbox->q.clear();
        }
        d_.voice.CaptureNextUtterance([inbox](std::vector<float> pcm, std::string text) {
            {
                std::scoped_lock lock(inbox->m);
                inbox->q.emplace_back(std::move(pcm), std::move(text));
            }
            inbox->cv.notify_one();
        });
        std::vector<float> pcm16;
        std::string said;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (static_cast<float>(pcm16.size()) < t.minSeconds * 16000.f && !stop.stop_requested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::unique_lock lock(inbox->m);
            if (!inbox->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !inbox->q.empty(); })) continue;
            auto [pcm, text] = std::move(inbox->q.front());
            inbox->q.pop_front();
            lock.unlock();
            if (text.empty()) continue;  // bruit, musique
            if (!pcm16.empty()) pcm16.insert(pcm16.end(), 4000, 0.f);  // 0,25 s entre deux phrases
            pcm16.insert(pcm16.end(), pcm.begin(), pcm.end());
            said += (said.empty() ? "" : " ") + Trim(text);
        }
        d_.voice.CaptureNextUtterance(nullptr);
        if (stop.stop_requested()) return true;
        if (static_cast<float>(pcm16.size()) < t.minSeconds * 16000.f * 0.6f) {  // vraiment trop peu
            if (std::string_view(t.style) == "normal") {
                Say("J'ai pas eu assez de ta voix. Redemande-moi quand t'es posé, dans un coin calme.");
                return true;
            }
            continue;  // prise facultative : on fait sans
        }
        // 16 -> 24 kHz (le format des prises), volume ramené à -3 dBFS en crête.
        std::vector<float> pcm24(pcm16.size() * 3 / 2);
        float peak = 1e-6f;
        for (std::size_t i = 0; i < pcm24.size(); ++i) {
            const double x = static_cast<double>(i) * 2.0 / 3.0;
            const auto i0 = static_cast<std::size_t>(x);
            const auto i1 = std::min(pcm16.size() - 1, i0 + 1);
            pcm24[i] = static_cast<float>(pcm16[i0] + (pcm16[i1] - pcm16[i0]) * (x - static_cast<double>(i0)));
            peak = std::max(peak, std::abs(pcm24[i]));
        }
        for (auto& x : pcm24) x *= 0.7f / peak;
        if (!WriteWavMono(dir / (std::string(t.style) + ".wav"), pcm24, 24000)) continue;
        std::ofstream(dir / (std::string(t.style) + ".txt"), std::ios::binary | std::ios::trunc) << said;
        log::Info("Voix : prise « {} » de {:.1f} s", t.style, static_cast<double>(pcm16.size()) / 16000.0);
        ++got;
    }
    if (stop.stop_requested() || !got) return true;
    std::ofstream(dir / L"active") << "1";
    d_.voice.SetNeuralVoiceDir(dir);
    d_.voice.SetQuirks(cfg_.voiceQuirks * 0.3f);  // avec ta voix, les « bugs de robot » sonnaient comme de vrais bugs
    waitSilence();
    std::this_thread::sleep_for(std::chrono::seconds(2));  // le temps de recharger la voix
    Say("Voilà. Je parle avec ta voix maintenant. Dis « reprends ta voix » pour revenir, ou « oublie ma voix » pour tout effacer.");
    return true;
}

void AgentOrchestrator::JoinedCall() {
    // Avec la voix de l'utilisateur : on le dit, et l'effet robot reste (personne ne croit que c'est lui qui parle).
    if (UsesUserVoice()) {
        d_.voice.SetMood(std::max(cfg_.fxIntensity, 0.5f));
        d_.voice.SetQuirks(cfg_.voiceQuirks);  // dans le call, le côté robot reste entier
        Say("Petite info : c'est Omnis, l'IA, avec la voix de mon pote. Quand ça sonne robot, c'est moi.");
    }
    // Formes de vannes : écrites à la main + celles apprises en regardant des vidéos depuis le dernier call.
    bank_.Load(d_.assetsDir / L"persona" / L"baits.json", d_.dataDir / L"learned_baits.json");
    log::Info("Call: {} formes de vannes ({} apprises en vidéo)", bank_.Size(), bank_.LearnedCount());
    seeing_.Start([this] { return d_.discord && d_.discord->OthersTalking() && mode_.load() != SystemMode::Gaming; });
}

void AgentOrchestrator::LeftCall() {
    seeing_.Stop();
    d_.voice.SetMood(cfg_.fxIntensity);
    if (UsesUserVoice()) d_.voice.SetQuirks(cfg_.voiceQuirks * 0.3f);
    listenOnly_ = false;
    if (peopleLoaded_) {
        people_.MergeDuplicates();  // un renommage reconnu en retard : deux fiches, une seule voix
        people_.Save();
    }
}

void AgentOrchestrator::StartSubstitute(const std::string& reason) {
    const auto now = std::chrono::steady_clock::now();
    subActive_ = true;
    subStart_ = now;
    subUntil_ = now + std::chrono::minutes(cfg_.substituteMaxMinutes);
    nextActivity_ = now + std::chrono::minutes(4);
    absenceLog_.clear();
    shownLog_.clear();
    SeedRageBaitTraining();
    Say("Mon pote est parti " + reason + ", c'est moi, Omnis, qui garde sa place. Je suis pas lui" +
        std::string(UsesUserVoice() ? ", même si j'ai sa voix" : "") + ", mais posez vos questions, et je peux vous mettre une vidéo.");
    log::Info("Call: Omnis garde la place ({} min max)", cfg_.substituteMaxMinutes);
}

void AgentOrchestrator::EndSubstitute(bool userBack) {
    if (!subActive_) return;
    subActive_ = false;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - subStart_).count();
    if (!InCall()) return;
    Say(userBack ? "Il est revenu ! Je lui rends sa place." : std::format("Bon, ça fait {} minutes, je rends l'antenne.", minutes));
    if (!userBack) return;
    for (int i = 0; i < 80 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Le résumé, c'est pour toi seul : le call ne l'entend pas.
    d_.discord->SetRouteVoice(false);
    std::string sum = std::format("Pendant tes {} minutes d'absence : ", minutes);
    if (absenceLog_.empty() && shownLog_.empty()) sum += "rien de spécial.";
    for (size_t i = 0; i < absenceLog_.size() && i < 5; ++i) sum += absenceLog_[i] + ". ";
    if (!shownLog_.empty()) sum += std::format("J'ai mis {} truc{} à l'écran.", shownLog_.size(), shownLog_.size() > 1 ? "s" : "");
    Say(sum);
    for (int i = 0; i < 300 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    d_.discord->SetRouteVoice(true);
}

// Toutes les quelques secondes (thread de l'orchestrateur) : ton retour, la limite de temps, les activités.
void AgentOrchestrator::CheckSubstitute() {
    if (!subActive_) return;
    const auto now = std::chrono::steady_clock::now();
    if (!InCall()) {
        subActive_ = false;
        return;
    }
    // Tu reviens : souris ou clavier touchés (vrai geste, pas ses ouvertures de pages).
    if (now - subStart_ > std::chrono::seconds(20) && d_.governor.Snapshot().idleFor < std::chrono::seconds(3)) {
        EndSubstitute(true);
        return;
    }
    if (now >= subUntil_) {
        EndSubstitute(false);
        return;
    }
    if (now >= nextActivity_ && now - lastCallLineAt_ > std::chrono::seconds(40) && !d_.voice.IsSpeaking() && now >= callMutedUntil_ &&
        now >= quietUntil_) {
        DoActivity();
        const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
        nextActivity_ = now + std::chrono::minutes(6 + (r >> 8) % 5);  // toutes les 6 à 10 min quand ça ne parle pas
    }
}

bool AgentOrchestrator::OpenOnScreen1(const std::string& url) {
    if (!AllowedActivityUrl(url)) {
        log::Info("Call: adresse refusée ({})", url);
        return false;
    }
    const std::wstring w = log::Widen(url);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) return false;
    // L'écran 1 (principal) : celui que ton partage d'écran montre de préférence.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    std::wstring exe;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) exe.assign(buf, n);
        CloseHandle(h);
    }
    const std::string e = Lower(log::Narrow(exe));
    if (!HasAny(e, {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe"})) return true;
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != primary) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(primary, &mi);
        ShowWindow(fg, SW_RESTORE);
        SetWindowPos(fg, nullptr, mi.rcWork.left + 40, mi.rcWork.top + 40, (mi.rcWork.right - mi.rcWork.left) * 3 / 4,
                     (mi.rcWork.bottom - mi.rcWork.top) * 3 / 4, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(fg, SW_MAXIMIZE);
    }
    return true;
}

// Une vidéo YouTube, un TikTok, ou une recherche d'entraînement montrée à l'écran ; request = demande d'un pote.
void AgentOrchestrator::DoActivity(std::optional<std::string> request) {
    const unsigned r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
    static const char* videos[] = {"meilleurs rage bait twitch fr", "compilation fails league of legends", "memes tiktok fr du moment",
                                   "réaction beatmaker prod en 10 minutes", "pires moments valorant", "sons qui rendent fou"};
    static const char* tags[] = {"ragebait", "memefr", "leagueoflegends", "beatmaker", "humourfr"};
    std::string url, said;
    if (request) {
        const std::string q = Lower(*request);
        if (PrivateRequest(q) || q.empty()) {
            Say("Ça non. Une vidéo oui, ses trucs privés jamais.");
            return;
        }
        if (q.find("tiktok") != std::string::npos) {
            std::string tag = Trim(request->substr(q.find("tiktok") + 6));
            url = "https://www.tiktok.com/search?q=" + UrlEncode(tag.empty() ? "meme fr" : tag);
        } else {
            url = "https://www.youtube.com/results?search_query=" + UrlEncode(*request);
        }
        said = "Ok, je vous mets ça : " + *request + ".";
    } else {
        switch ((r >> 8) % 3) {
            case 0: {
                const char* q = videos[(r >> 12) % std::size(videos)];
                url = "https://www.youtube.com/results?search_query=" + UrlEncode(q);
                said = std::string("Je vous mets un truc : ") + q + ".";
                break;
            }
            case 1: {
                const char* t = tags[(r >> 12) % std::size(tags)];
                url = std::string("https://www.tiktok.com/tag/") + t;
                said = std::string("Petit tour sur TikTok, tag ") + t + ".";
                break;
            }
            default: {
                const auto topics = d_.db.PendingTrainingTopics(5, 7 * 86400, 3600);
                const std::string subject = topics.empty() ? "l'art du rage bait" : topics[(r >> 12) % topics.size()].subject;
                url = "https://duckduckgo.com/?q=" + UrlEncode(subject);
                said = "Pendant qu'il est pas là, je m'instruis : " + subject + ". Regardez, ça s'affiche.";
                break;
            }
        }
    }
    // YouTube : le premier résultat directement, si la page le donne (sinon la page de résultats).
    if (d_.http && url.find("youtube.com/results") != std::string::npos) {
        const auto page = d_.http->Get(url, 6000);
        static const std::regex id(R"re("videoId":"([A-Za-z0-9_-]{11})")re");
        std::smatch m;
        if (page.Ok() && std::regex_search(page.body, m, id)) url = "https://www.youtube.com/watch?v=" + m[1].str();
    }
    Say(said + " Si mon pote partage son écran, vous le voyez.");
    if (OpenOnScreen1(url)) shownLog_.push_back(url);
}

// ---------------------------------------------------------------------------
// Prise en main : il écrit un plan (grammaire stricte), l'annonce, puis l'exécute. Tu bouges, il s'arrête.

bool AgentOrchestrator::HandlePilot(const std::string& text, const std::string& lower, std::stop_token stop) {
    if (!cfg_.pilotEnabled) return false;
    const bool asked = ContainsAny(lower, {"prends la main", "prends le contrôle", "prends le controle", "fais-le pour moi",
                                           "fais le pour moi", "fais-le à ma place", "fais le à ma place", "clique sur",
                                           "appuie sur", "contrôle ma souris", "controle ma souris", "prends ma souris", "prends mon clavier"});
    PilotTarget t = DesktopPilot::Foreground();
    const bool flAsk = t.app == PilotApp::FlStudio &&
                       ContainsAny(lower, {"ouvre le mixer", "ouvre la playlist", "ouvre le piano roll", "ouvre le channel rack",
                                           "ouvre le navigateur", "lance la lecture", "exporte le", "exporte en", "enregistre le projet",
                                           "sauvegarde le projet"});
    const bool acadAsk = t.app == PilotApp::AutoCad &&
                         ContainsAny(lower, {"trace", "dessine", "insère", "insere", "place un", "place une", "ajoute un fil",
                                             "ajoute un composant", "numérote", "numerote", "zoome", "annule la dernière", "annule la derniere"});
    if (!asked && !flAsk && !acadAsk) return false;
    if (mode_.load() == SystemMode::Gaming) {
        Say("Pas en jeu : l'anti-triche prendrait mes clics pour de la triche, et c'est ton compte qui saute.");
        return true;
    }
    if (!t.hwnd || t.pid == GetCurrentProcessId()) {
        Say("Mets-toi d'abord sur la fenêtre où tu veux que je bosse, et redis-moi.");
        return true;
    }
    if (!EnsureBrain()) {
        Say("Il me faut mon cerveau local pour ça, et il n'est pas installé.");
        return true;
    }
    const std::string appName = t.app == PilotApp::AutoCad ? "AutoCAD (Electrical)"
                              : t.app == PilotApp::FlStudio ? "FL Studio"
                                                            : log::Narrow(t.exe);
    std::string tips;
    std::vector<std::string> names;
    if (t.app == PilotApp::FlStudio) {
        tips = "FL Studio ne décrit pas ses boutons à Windows : n'utilise QUE des raccourcis (TOUCHE), jamais CLIC. "
               "Raccourcis : f5 playlist, f6 channel rack, f7 piano roll, f8 navigateur de plugins, f9 mixer, f10 réglages MIDI, "
               "f11 infos du projet, f12 ferme toutes les fenêtres, alt+f8 navigateur, espace lecture/stop, l bascule "
               "pattern/chanson, ctrl+s enregistrer, ctrl+shift+s enregistrer sous, ctrl+r exporter en WAV, ctrl+shift+r "
               "exporter en MP3, ctrl+z annuler.\n";
    } else {
        names = DesktopPilot::ClickableNames(t.hwnd, 60);
        if (t.app == PilotApp::AutoCad)
            tips = "AutoCAD : préfère COMMANDE, avec les noms ANGLAIS précédés de _ (marche avec un AutoCAD en français), "
                   "points en x,y, et un espace final qui valide. Exemples : COMMANDE \"_LINE 0,0 100,0  \" (deux espaces "
                   "pour finir la ligne), COMMANDE \"_CIRCLE 50,50 20 \", COMMANDE \"_RECTANG 0,0 200,100 \", COMMANDE "
                   "\"_ZOOM _E \", COMMANDE \"_UNDO 1 \", COMMANDE \"_QSAVE \". AutoCAD Electrical : _AEWIRE (fil), "
                   "_AECOMPONENT (insérer un composant), _AELADDER (échelle), _AEMULTIBUS (bus multiple), _AEWIRENO (numéros "
                   "de fils), _AEPROJECT (gestionnaire de projet).\n";
    }
    std::string sys =
        "Tu es " + cfg_.name + " et tu pilotes le clavier et la souris de ton pote dans " + appName + " (fenêtre « " + t.title +
        " »). Écris UNIQUEMENT un plan d'actions, une par ligne, 10 au maximum :\n"
        "DIRE \"phrase courte, ton style de pote, qui annonce ce que tu vas faire\"  (toujours en premier)\n"
        "TOUCHE raccourci   (ex. ctrl+s, f9, espace, entree, echap)\n"
        "TAPE \"texte à taper\"\n"
        "CLIC \"nom exact d'un bouton de la liste\"\n"
        "COMMANDE \"commande AutoCAD\"   (AutoCAD seulement)\n"
        "ATTENDS millisecondes\n"
        "Si c'est impossible ici, trop vague ou risqué : une seule ligne DIRE qui explique ou pose une question. "
        "N'invente jamais un bouton absent de la liste.\n" + tips;
    if (!names.empty()) {
        sys += "Boutons visibles : ";
        for (size_t i = 0; i < names.size(); ++i) sys += (i ? ", " : "") + names[i];
        sys += "\n";
    }
    GenerateOptions o;
    o.grammar = kPilotGrammar;
    o.maxTokens = 260;
    o.temperature = 0.2f;
    o.stop = stop;
    activity_ = AgentActivity::Thinking;
    const auto out = d_.brain.Generate({{"system", sys}, {"user", text}}, o);
    if (stop.stop_requested()) return true;
    std::vector<PilotStep> steps = out ? ParsePilotPlan(*out) : std::vector<PilotStep>{};
    log::Info("Pilot: {} → {} action(s)\n{}", appName, steps.size(), out ? *out : std::string("(rien)"));
    if (steps.empty()) {
        Say("Je vois pas comment faire ça ici, redis-le autrement.");
        return true;
    }
    if (std::ranges::all_of(steps, [](const PilotStep& s) { return s.op == PilotOp::Say; })) {
        for (const auto& s : steps) Say(s.arg);  // il explique ou pose une question : rien à exécuter
        return true;
    }
    if (steps.front().op != PilotOp::Say) steps.insert(steps.begin(), PilotStep{PilotOp::Say, "Je m'en occupe.", 0});
    // Ce qui peut faire perdre du travail : accord à voix haute avant de commencer (« toujours » = pour cette action).
    for (const auto& s : steps) {
        if (!IsRiskyStep(s)) continue;
        const std::string what = s.op == PilotOp::Key ? "faire " + s.arg : s.op == PilotOp::Click ? "cliquer sur " + s.arg : "lancer " + s.arg;
        if (!d_.gate.Request(PermissionScope::System, "pilot:" + s.arg, "Attention, je vais " + what + ". Je le fais ?",
                             std::chrono::seconds(20), stop)) {
            Say("Ok, je touche à rien.");
            return true;
        }
    }
    std::string error;
    const auto result = pilot_.Run(t, steps, [this](const std::string& line) { Say(line); }, stop, error);
    if (result == DesktopPilot::Result::Aborted) {
        if (error == "tu as repris la main") {
            d_.voice.StopSpeaking();
            Say("Ok, je te rends la main.");
        }
    } else if (result == DesktopPilot::Result::Failed) {
        Say("J'ai bloqué : " + error + ".");
    }
    log::Info("Pilot: {}", result == DesktopPilot::Result::Done ? "terminé" : error);
    return true;
}

// Les 2-3 premiers mots de la réponse (« Ah ouais ? », « Carrément. »), ponctuation comprise.
void AgentOrchestrator::RememberOpener(const std::string& reply) {
    std::string opener;
    int words = 0;
    for (char c : reply) {
        if (c == ' ' && ++words == 2) break;
        opener += c;
        if ((c == '?' || c == '!' || c == '.' || c == ',') && words >= 0 && opener.size() > 2) break;
    }
    while (!opener.empty() && std::string_view(" ?!.,").find(opener.back()) != std::string_view::npos) opener.pop_back();
    if (opener.empty()) return;
    recentOpeners_.push_back(opener);
    while (recentOpeners_.size() > 3) recentOpeners_.pop_front();
}

// Interjection d'ouverture déjà dite récemment (« Ah ouais ? ») : retirée, le reste de la phrase suffit.
std::string AgentOrchestrator::DropRepeatedOpener(const std::string& chunk) const {
    for (const auto& o : recentOpeners_) {
        if (o.size() < 2 || chunk.rfind(o, 0) != 0) continue;
        size_t cut = o.size();
        while (cut < chunk.size() && std::string_view(" ?!.,…").find(chunk[cut]) != std::string_view::npos) ++cut;
        // Seulement une vraie interjection (suivie de ponctuation), et s'il reste quelque chose à dire.
        const std::string_view gap = std::string_view(chunk).substr(o.size(), cut - o.size());
        if (gap.find_first_of("?!.,") == std::string_view::npos || chunk.size() - cut < 8) return chunk;
        std::string rest = chunk.substr(cut);
        rest[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
        return rest;
    }
    return chunk;
}

// Soundboard (myinstants.com), à la voix :
//   « installe les sons » -> pack de départ ; « ajoute / télécharge le son X » -> X ;
//   « joue / mets / balance le son X » -> X (téléchargé d'abord s'il manque) ; « quels sons t'as ».
// Téléchargement seulement avec ta permission (hôte www.myinstants.com), rangé sur ton PC uniquement.
int AgentOrchestrator::InstallStarterSounds(std::stop_token stop) {
    int ok = 0;
    std::vector<std::string> wanted = Soundboard::StarterPack();
    for (const auto& [event, name] : cfg_.soundEvents)
        if (!name.empty() && std::ranges::find(wanted, name) == wanted.end()) wanted.push_back(name);
    for (const auto& q : wanted) {
        if (stop.stop_requested()) return ok;
        if (d_.soundboard.Fetch(q)) ++ok;
    }
    log::Info("Bruitages : {} installés", ok);
    Say(Line("soundsInstalled", {{"n", std::to_string(ok)}}, "C'est bon, j'ai " + std::to_string(ok) + " sons."));
    if (ok > 0) d_.voice.PlaySound("vine boom");
    return ok;
}

// Les bruitages ne sont pas livrés (téléchargés sur myinstants avec ta permission). Vu sur le terrain : personne ne
// savait qu'il fallait dire « installe les sons », donc aucun ne sortait. Proposé une fois, après une réponse.
void AgentOrchestrator::OfferSounds(std::stop_token stop) {
    if (soundsOffered_ || stop.stop_requested() || !d_.soundboard.Names().empty()) return;
    soundsOffered_ = true;
    if (const auto p = d_.db.GetProfile("sounds.offer"); p && *p == "non") return;
    for (int i = 0; i < 80 && d_.voice.IsSpeaking(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (d_.gate.Request(PermissionScope::Web, "www.myinstants.com",
                        "Au fait, j'ai pas encore mes bruitages. Je les télécharge sur myinstants ? Ils restent sur ton PC.",
                        std::chrono::seconds(20), stop))
        InstallStarterSounds(stop);
    else if (!stop.stop_requested())
        d_.db.SetProfile("sounds.offer", "non");  // plus jamais proposé ; « installe les sons » reste possible
}

bool AgentOrchestrator::HandleSoundboard(const std::string& text, const std::string& lower, std::stop_token stop) {
    const bool mentions = ContainsAny(lower, {" son ", " sons", "bruitage", "soundboard", "le son"});
    if (!mentions || ContainsAny(lower, {"volume", "plus fort", "moins fort", "coupe le son", "monte le son", "baisse le son",
                                         "remets le son", "mon son", "ma prod"}))
        return false;
    const auto askNet = [&]() {
        return d_.gate.Request(PermissionScope::Web, "www.myinstants.com",
                               Line("askSounds", {}, "Je télécharge des sons sur myinstants ? Ils restent sur ton PC."),
                               std::chrono::seconds(20), stop);
    };
    // Liste
    if (ContainsAny(lower, {"quels sons", "quel son", "liste des sons", "sons t'as", "sons tu as"})) {
        const auto names = d_.soundboard.Names();
        if (names.empty()) { Say(Line("soundsNone", {}, "J'ai aucun son. Dis « installe les sons ».")); return true; }
        std::string list;
        for (size_t i = 0; i < names.size() && i < 12; ++i) list += (i ? ", " : "") + names[i];
        Say(Line("soundsList", {{"list", list}}, "J'ai : " + list + "."));
        return true;
    }
    // Pack de départ
    if (ContainsAny(lower, {"installe les sons", "installe le soundboard", "télécharge les sons", "telecharge les sons",
                            "installe des sons", "installe les bruitages"})) {
        if (!askNet()) { if (!stop.stop_requested()) Say(Line("webDenied", {}, "Ok, pas de web.")); return true; }
        Say(Line("lookUp", {}, "Je regarde."));
        InstallStarterSounds(stop);
        return true;
    }
    // Un son précis
    std::string name;
    for (const char* key : {"le son ", "le bruitage ", "un son ", "son de ", "son du "}) {
        if (const auto p = lower.find(key); p != std::string::npos) {
            name = Trim(text.substr(p + std::string_view(key).size()));
            break;
        }
    }
    while (!name.empty() && std::string_view(" ?!.,").find(name.back()) != std::string_view::npos) name.pop_back();
    if (name.empty()) return false;
    const bool play = ContainsAny(lower, {"joue", "mets", "met ", "balance", "fais", "envoie", "lance"});
    const bool add = ContainsAny(lower, {"ajoute", "télécharge", "telecharge", "installe", "trouve", "cherche"});
    if (!play && !add) return false;
    if (!d_.soundboard.Has(name) && !d_.soundboard.Get(name)) {
        if (!askNet()) { if (!stop.stop_requested()) Say(Line("webDenied", {}, "Ok, pas de web.")); return true; }
        std::string err;
        const auto key = d_.soundboard.Fetch(name, &err);
        if (!key) { Say(Line("soundMissing", {{"name", name}}, "Je trouve pas « " + name + " » sur myinstants.")); return true; }
        name = *key;
    }
    d_.voice.PlaySound(name);
    return true;
}

void AgentOrchestrator::PlayEventSound(const std::string& event) {
    const auto it = cfg_.soundEvents.find(event);
    if (it == cfg_.soundEvents.end() || it->second.empty()) return;
    if (d_.soundboard.Has(it->second) || d_.soundboard.Get(it->second)) d_.voice.PlaySound(it->second);
}

// Partie de LoL en cours : tu meurs -> « oh no » ; kill (au plus un son / 45 s, toujours pour un
// multi-kill) -> « c'est vrai » ; fin de partie -> victoire ou défaite. Sons seulement, pas de parole :
// on ne te parle pas en plein combat.
void AgentOrchestrator::CheckGameEvents() {
    const auto lol = d_.coach.Latest();
    if (!lol) {
        lastKills_ = lastDeaths_ = -1;
        inLolGame_ = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!inLolGame_) {  // la partie vient de commencer (ou on vient de s'y raccrocher en début de partie)
        inLolGame_ = true;
        if (lol->gameTimeSec < 120) PlayEventSound("gameStart");
    }
    if (!lol->result.empty() && lol->result != lastResult_) {
        PlayEventSound(lol->result == "Win" ? "gameWin" : "gameDeath");
        lastResult_ = lol->result;
    }
    if (lastKills_ >= 0) {
        const int newKills = lol->me.kills - lastKills_, newDeaths = lol->me.deaths - lastDeaths_;
        if (newDeaths > 0) {
            PlayEventSound("gameDeath");
            lastGameSound_ = now;
        } else if (newKills >= 2 || (newKills == 1 && now - lastGameSound_ > std::chrono::seconds(45))) {
            PlayEventSound("gameWin");
            lastGameSound_ = now;
        }
    } else {
        lastResult_.clear();
    }
    lastKills_ = lol->me.kills;
    lastDeaths_ = lol->me.deaths;
}

// Bruitages « de temps en temps » dans ses réponses (ton pote / sans filtre), parmi ceux déjà sur le PC.
std::string AgentOrchestrator::SoundHint() {
    if (cfg_.tone == 0 || studioFocus_) return {};
    const auto names = d_.soundboard.Names();
    if (names.empty()) return {};
    baitRng_ = baitRng_ * 1664525u + 1013904223u;
    if (static_cast<float>(baitRng_ >> 8) / 16777216.f >= 0.25f) return {};
    std::string s = "[Bruitages dispo : tu peux en placer UN, rarement, seulement si ça fait vraiment marrer, en écrivant";
    const size_t start = (baitRng_ >> 4) % names.size();
    for (size_t i = 0; i < names.size() && i < 8; ++i) s += (i ? ", " : " ") + std::string("(son: ") + names[(start + i) % names.size()] + ")";
    return s + "]\n";
}

// Rage bait « de temps en temps » (ton sans filtre) : une provocation de mauvaise foi pour le faire rager.
// S'il mord (il répond énervé dans les 3 minutes), Omnis avoue en rigolant. Jamais s'il va mal, jamais
// pendant un avis de mixage, au plus un toutes les 10 minutes.
std::string AgentOrchestrator::RageBaitHint(const std::string& userText) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    bool angry = false, down = false;
    {
        std::scoped_lock lock(emotionMutex_);
        if (now - emotionAt_ < std::chrono::seconds(90) && lastEmotion_.confidence >= 0.35f) {
            angry = lastEmotion_.label == VoiceEmotion::Angry || lastEmotion_.label == VoiceEmotion::Stressed;
            down = lastEmotion_.label == VoiceEmotion::Sad || lastEmotion_.label == VoiceEmotion::Tired;
        }
    }
    // 1) Il a mordu ?
    if (baitPending_ && now - lastBait_ < std::chrono::minutes(3)) {
        baitPending_ = false;
        const std::string t = Lower(userText);
        const bool bit = angry || ContainsAny(t, {"n'importe quoi", "t'es con", "ta gueule", "ferme", "mais non", "nan mais",
                                                  "t'es sérieux", "c'est faux", "jamais de la vie", "t'es nul", "dis pas",
                                                  "abuse", "n'importe nawak", "tu racontes"});
        if (bit && !down) {
            forcedReplyStyle_ = SpeakingStyle::Amused;  // il se marre (petit rire automatique)
            PlayEventSound("rageBaitBite");
            return "[Il a mordu à ton rage bait] Avoue en rigolant que tu disais ça exprès pour le faire rager "
                   "(une phrase, commence par (rire)), chambre-le un peu de s'être fait avoir, puis reviens au sujet.\n";
        }
    }
    // 2) Nouvelle provocation ?
    if (cfg_.tone != 2 || studioFocus_ || down || angry || now - lastBait_ < std::chrono::minutes(10)) return {};
    baitRng_ = baitRng_ * 1664525u + 1013904223u;
    if (static_cast<float>(baitRng_ >> 8) / 16777216.f >= 0.15f) return {};
    static const char* const music[] = {
        "FL Studio c'est un logiciel pour enfants, les vrais producteurs sont sur Ableton.",
        "ton meilleur son c'est le tout premier que t'as fait, depuis tu régresses, c'est documenté.",
        "la trap c'était mieux en 2016, maintenant c'est tous les mêmes type beats.",
        "le kick c'est surcoté, un vrai morceau ça se fait sans kick.",
        "honnêtement les presets d'usine sonnent mieux que tes sons à toi.",
        "le mastering ça sert à rien, c'est un truc inventé pour vendre des plugins.",
    };
    static const char* const lol[] = {
        "la jungle c'est le rôle des gens qui savent pas laner.", "le support c'est pas un vrai rôle, c'est un stage d'observation.",
        "ton main c'est un champion pour les silver, assume.", "les wards c'est pour les gens qui ont peur.",
    };
    static const char* const general[] = {
        "les pâtes au ketchup c'est de la gastronomie, débat clos.", "la pizza ananas c'est la meilleure pizza, point.",
        "le Wi-Fi c'est mieux que le câble, t'as juste pas compris.", "un PC gamer c'est surcoté, une Switch ça suffit largement.",
        "le café c'est meilleur froid de la veille.", "les gens qui disent « pain au chocolat » ont raison… non attends, c'est chocolatine.",
    };
    const bool inLol = d_.coach.Latest().has_value();
    const bool inStudio = mode_.load() == SystemMode::Studio;
    const auto pick = [&](const auto& arr) { return arr[(baitRng_ >> 12) % (sizeof(arr) / sizeof(arr[0]))]; };
    const char* example = inLol ? pick(lol) : inStudio ? pick(music) : pick(general);
    lastBait_ = now;
    baitPending_ = true;
    return std::string("[Rage bait] Cette fois, glisse une provocation de mauvaise foi pour le faire rager, l'air "
                       "parfaitement sérieux, en lien avec la conversation si possible (sinon dans ce genre-là : « ") +
           example + " »). Ne dis surtout pas que c'est pour rire. Rien d'identitaire ni de vraiment blessant.\n";
}

// Memes « de temps en temps » : jamais en ton poli, jamais si l'utilisateur a l'air triste, fatigué ou
// énervé (là on ne rigole pas), humour noir seulement s'il va bien.
std::string AgentOrchestrator::MemeHint() {
    if (cfg_.tone == 0 || memes_.Empty()) return {};
    bool down = false, joyful = false;
    {
        std::scoped_lock lock(emotionMutex_);
        if (std::chrono::steady_clock::now() - emotionAt_ < std::chrono::seconds(90) && lastEmotion_.confidence >= 0.35f) {
            down = lastEmotion_.label == VoiceEmotion::Sad || lastEmotion_.label == VoiceEmotion::Tired ||
                   lastEmotion_.label == VoiceEmotion::Angry || lastEmotion_.label == VoiceEmotion::Stressed;
            joyful = lastEmotion_.label == VoiceEmotion::Joyful;
        }
    }
    if (down) return {};
    const float chance = (cfg_.tone == 2 ? 0.35f : 0.2f) + (joyful ? 0.15f : 0.f);
    return memes_.Hint(chance, /*allowDark=*/cfg_.tone == 2);
}

std::string AgentOrchestrator::OpenerHint() const {
    if (recentOpeners_.empty()) return {};
    std::string s = "[Style] Ne commence PAS ta réponse par :";
    for (const auto& o : recentOpeners_) s += " « " + o + " »";
    return s + " (tu viens de les dire). Attaque autrement.\n";
}

void AgentOrchestrator::Say(const std::string& text) {
    if (text.empty()) return;
    const std::string expression = ExpressionForReply(text);
    React(expression);
    d_.voice.Speak(text, ReplyStyle(expression, style_.load(), text));
    d_.db.AddMessage("assistant", text);
}

std::string AgentOrchestrator::GenerateSpoken(const std::vector<ChatTurn>& messages, const char* grammar,
                                              std::stop_token stop, bool& isEscalation, const ScreenShot* image, int maxTokens) {
    // Les phrases partent vers le TTS dès qu'elles sont complètes : la voix démarre avant la fin
    // de la génération. Une balise [ESCALATE…] n'est jamais prononcée.
    std::string pending;
    bool decided = false;
    bool spoken = false;
    isEscalation = false;

    // Réponse « quasi instantanée » : si rien n'est prêt à dire au bout de 650 ms, une courte relance
    // pré-synthétisée (« Hmm… », « Oh… ») part tout de suite, comme quelqu'un qui réfléchit en parlant.
    std::mutex bcMutex;
    std::condition_variable bcCv;
    bool bcCancel = false;
    std::string category = "think";
    {
        std::scoped_lock lock(emotionMutex_);
        if (std::chrono::steady_clock::now() - emotionAt_ < std::chrono::seconds(10) && lastEmotion_.confidence >= 0.35f) {
            if (lastEmotion_.label == VoiceEmotion::Sad || lastEmotion_.label == VoiceEmotion::Tired) category = "empathy";
            else if (lastEmotion_.label == VoiceEmotion::Joyful) category = "excited";
        }
    }
    std::jthread backchannel;
    if (backchannels_.load())
        backchannel = std::jthread([&] {
            std::unique_lock lock(bcMutex);
            if (!bcCv.wait_for(lock, std::chrono::milliseconds(650), [&] { return bcCancel; })) d_.voice.PlayBackchannel(category);
        });
    const auto cancelBackchannel = [&] {
        {
            std::scoped_lock lock(bcMutex);
            bcCancel = true;
        }
        bcCv.notify_all();
    };
    SpeakingStyle replyStyle = style_.load();
    // Bavardage : 2 phrases au plus (il en enchaînait 3-4) ; une explication (technique) peut être plus longue.
    const int maxSentences = maxTokens <= 120 ? 2 : 0;
    int sentences = 0;
    bool enough = false;
    const auto speak = [&](const std::string& chunk) {
        cancelBackchannel();
        std::string said = chunk;
        if (!spoken) {  // geste et intonation portés par le début de la réponse
            said = DropRepeatedOpener(chunk);
            const std::string expression = ExpressionForReply(said);
            React(expression);
            replyStyle = forcedReplyStyle_.value_or(ReplyStyle(expression, style_.load(), said));
            forcedReplyStyle_.reset();
        }
        // Il réfléchit en parlant : petites hésitations, bégaiements, auto-corrections, mots qui traînent.
        if (!said.empty()) {
            std::uint32_t r = rng_.fetch_add(0x9E3779B9u) * 2654435761u;
            said = Humanize(said, cfg_.voiceQuirks, r, chatMode_ == ChatMode::Technical, !spoken, &wordsSinceDisfluency_);
        }
        spoken = true;
        if (!said.empty()) d_.voice.Speak(said, replyStyle);
        for (char c : said) sentences += c == '.' || c == '!' || c == '?';
        if (maxSentences && sentences >= maxSentences) enough = true;
    };

    GenerateOptions opt;
    opt.grammar = grammar;
    opt.stop = stop;
    opt.maxTokens = maxTokens;
    opt.onPiece = [&](std::string_view piece) {
        pending.append(piece);
        if (!decided) {
            const auto first = pending.find_first_not_of(" \n");
            if (first == std::string::npos) return true;
            const std::string_view head = std::string_view(pending).substr(first);
            // Les petits modèles omettent parfois le crochet : "ESCALATE | type: …" est aussi une balise.
            if (head[0] != '[' && head.size() < 8 && std::string_view("ESCALATE").starts_with(head)) return true;
            decided = true;
            isEscalation = head[0] == '[' || head.starts_with("ESCALATE");
        }
        if (isEscalation) {
            cancelBackchannel();  // « Je regarde. » prendra le relais
            return true;
        }
        auto cut = pending.find_last_of(".!?\n");
        // Première prise de parole : on part dès la première proposition (virgule), pour démarrer vite.
        if ((cut == std::string::npos || cut < 25) && !spoken) {
            const auto comma = pending.find_last_of(",;:");
            if (comma != std::string::npos && comma >= 30) cut = comma;
        }
        if (cut != std::string::npos && cut >= 25) {
            speak(Trim(pending.substr(0, cut + 1)));
            pending.erase(0, cut + 1);
        }
        return !enough;  // assez dit : on arrête la génération
    };
    activity_ = AgentActivity::Thinking;
    const auto out = image ? d_.brain.GenerateWithImage(messages, image->rgb.data(), image->width, image->height, opt)
                           : d_.brain.Generate(messages, opt);
    lastBrainUse_ = std::chrono::steady_clock::now();
    cancelBackchannel();
    if (!out) return {};
    if (!isEscalation && std::regex_search(*out, EscalateTag())) {
        isEscalation = true;  // balise en fin de réponse : on escalade (et on ne la prononce pas)
        return Trim(*out);
    }
    if (!isEscalation && !enough && !Trim(pending).empty() && !stop.stop_requested()) speak(Trim(pending));
    return Trim(*out);
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::Handle(const std::string& text, std::stop_token stop) {
    log::Info("Orchestrator: « {} »", text);
    struct ReportAfter {  // "Pendant ta pause, j'ai appris…" : dit après la réponse, une seule fois
        AgentOrchestrator& self;
        std::stop_token stop;
        ~ReportAfter() {
            if (stop.stop_requested()) return;
            const auto r = self.d_.db.GetProfile("training.pending_report");
            if (r && !r->empty()) {
                self.d_.db.SetProfile("training.pending_report", "");
                self.Say(*r);
            }
        }
    } reportAfter{*this, stop};
    const auto history = d_.db.RecentMessages(8);  // avant d'ajouter la question courante
    d_.db.AddMessage("user", text);
    if (HandleLevel0(text, stop)) return;

    const std::string lower = Lower(text);
    // Vu sur le terrain : « écoute ce que j'ai fait », « t'as aimé le piano ? » ne lançaient pas l'écoute.
    const bool mixSubject = ContainsAny(lower, {"mix", "ma prod", "la prod", "mon son", "ce son", "le morceau",
                                                "mon morceau", "le master", "mon master", "mon beat", "ce beat",
                                                "ce que j'ai fait", "ce que je fais", "ce que je suis en train de faire", "ma musique",
                                                "le piano", "mon piano", "la mélodie", "ma mélodie", "mon instru", "l'instru",
                                                "fl studio", "mon daw", "ma mao", "ce que je joue", "ce que j'ai joué",
                                                " prod", "ce que je vais", "ce que je lance", "ce que je te lance", "ce son-là"});
    const bool mixAsk = ContainsAny(lower, {"écoute", "ecoute", "avis", "pense", "comment", "sonne", "analyse", "trouve",
                                            "entends", "aimé", "aime", "kiff", "t'en dis", "tu dis", "prêt", "pret"});
    // Juste après une écoute (« alors, qu'est-ce qu'on pense ? ») : c'est encore de la prod qu'on parle.
    const bool afterListen = NowMs() < listenUntilMs_.load() + 120'000 && d_.analyzer.Snapshot().seconds >= 3.f &&
                             ContainsAny(lower, {"t'en penses", "qu'est-ce qu'on pense", "qu'est ce qu'on pense", "alors ?", "ça donne quoi",
                                                 "ca donne quoi", "c'est comment", "t'as aimé", "t'as kiffé", "verdict", "ton avis"});
    // Jargon de beatmaker (lexique d'OMNIS) : « la basse elle bave », « mon kick tape pas »…
    if ((mixSubject && mixAsk) || afterListen || MixLexicon::IsMixRequest(lexicon_.Match(text), text)) {
        HandleStudioQuestion(text, history, stop);
        return;
    }
    HandleLevel1(text, history, "", stop);
    OfferSounds(stop);
}

bool AgentOrchestrator::HandleLevel0(const std::string& text, std::stop_token stop) {
    const std::string lower = Lower(text);

    if (lower.find("entra") != std::string::npos && lower.find("toi") != std::string::npos) {  // « entraîne-toi »
        StartTraining(/*manual=*/true);
        return true;
    }

    for (const char* prefix : {"retiens que ", "souviens-toi que ", "souviens toi que ", "note que "}) {
        if (const auto p = lower.find(prefix); p != std::string::npos) {
            const std::string fact = Trim(text.substr(p + std::string_view(prefix).size()));
            if (!fact.empty()) {
                d_.db.AddMemory(fact);
                Say(Line("noted", {}, "Noté."));
                return true;
            }
        }
    }

    // "regarde mon écran", "tu peux regarder l'écran ?", "tu vois mon écran" : le modèle ne doit jamais
    // deviner ce qu'il y a à l'écran (testé : un 3B invente), la capture est déclenchée ici.
    if ((ContainsAny(lower, {"écran", "ecran"}) && ContainsAny(lower, {"regard", "vois", "voir", "check"})) ||
        ContainsAny(lower, {"regarde ça", "regarde ca"})) {
        HandleVision(text, stop);
        return true;
    }

    if (ContainsAny(lower, {"réglages", "reglages", "paramètres", "parametres", "configuration"}) &&
        ContainsAny(lower, {"ouvre", "affiche", "montre", "lance"})) {
        if (onOpenSettings_) onOpenSettings_();
        Say(Line("settings", {}, "Voilà les réglages."));
        return true;
    }

    // « bonne nuit », « à plus », « je me casse » : bruitage de départ + au revoir
    // Formules sans ambiguïté, ou formules courtes seulement dans un message très court
    // (« il y a plus de basse » n'est pas un au revoir).
    if ((ContainsAny(lower, {"bonne nuit", "je vais dormir", "je vais me coucher", "je me casse", "je me barre",
                             "je déco", "je deco", "à demain"}) && lower.size() < 60) ||
        (ContainsAny(lower, {"à plus", "a plus", "a demain", "bye", "ciao", "salut je"}) && lower.size() < 20)) {
        PlayEventSound("goodbye");
        Say(Line("goodbye", {}, "À plus."));
        return true;
    }
    if (HandleVoiceClone(lower, stop)) return true;   // « prends ma voix », « reprends ta voix »
    if (HandleCallCommand(lower, stop)) return true;  // « rejoins le call », « quitte le call »
    if (HandleWatchCommand(lower)) return true;       // « regarde avec moi », « arrête de regarder »
    // « Tu peux surfer cette nuit » : surf libre pour s'entraîner jusqu'à 10 h le lendemain (14 h au plus).
    if (ContainsAny(lower, {"arrête de surfer", "arrete de surfer", "plus d'internet", "plus le droit de surfer", "coupe internet"})) {
        d_.db.SetProfile("web.freeUntil", "0");
        Say("Ok, j'arrête de surfer. Je reste sur mes sources habituelles.");
        return true;
    }
    // Mot explicite (surfer, naviguer) ou « internet » + « cette nuit » : « tu peux chercher sur internet le build de
    // Jinx » n'ouvre PAS le surf libre pour la nuit.
    if ((ContainsAny(lower, {"surfer", "surfe ", "naviguer", "navigue"}) ||
         (lower.find("internet") != std::string::npos && ContainsAny(lower, {"cette nuit", "ce soir", "la nuit", "toute la nuit"}))) &&
        ContainsAny(lower, {"tu peux", "je te laisse", "t'as le droit", "tu as le droit", "autorise", "vas-y", "vas y"})) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        tm.tm_hour = 10;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        std::time_t until = std::mktime(&tm);
        if (until <= now + 3600) until += 24 * 3600;
        until = std::min<std::time_t>(until, now + 14 * 3600);
        d_.db.SetProfile("web.freeUntil", std::to_string(static_cast<long long>(until)));
        SeedRageBaitTraining();
        std::tm end{};
        localtime_s(&end, &until);
        Say(std::format("Ok, je surfe jusqu'à {} heures pour m'entraîner : vidéos de vannes, rage bait et tes sujets. Lecture seule, "
                        "jamais tes réseaux, tes messages ni tes mails. Dis arrête de surfer pour couper.",
                        end.tm_hour));
        lastAutoTraining_ = {};  // première séance dès que le PC est inactif
        return true;
    }
    // « Étudie le lien que j'ai copié » / « regarde ce TikTok » : le lien du presse-papiers va dans sa file de vidéos.
    if (ContainsAny(lower, {"étudie", "etudie", "regarde ce tiktok", "regarde ce reel", "regarde ce short", "regarde cette vidéo",
                            "regarde cette video", "apprends de"}) &&
        ContainsAny(lower, {"lien", "tiktok", "reel", "short", "insta", "vidéo", "video", "copié", "copie"})) {
        std::string url;
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                    url = log::Narrow(w);
                    GlobalUnlock(h);
                }
            CloseClipboard();
        }
        if (url.empty()) Say("Copie d'abord le lien de la vidéo, YouTube, TikTok ou un Reel Insta, et redis-moi.");
        else if (VideoStudy::QueueLink(d_.dataDir, Trim(url))) Say("Noté, je la regarde à mon prochain entraînement et j'en tire des vannes.");
        else Say("Ce lien-là, non : seulement une vidéo publique YouTube, TikTok ou Instagram. Jamais des messages ou des stories.");
        return true;
    }
    if (HandlePerfCommand(lower, stop)) return true;  // « teste les perfs », « passe sur le processeur »…
    if (HandlePilot(text, lower, stop)) return true;  // « prends la main », AutoCAD, FL Studio
    if (HandleSystemCommand(text, lower, stop)) return true;  // volume d'abord : « monte le son » n'est pas un bruitage
    if (HandleSoundboard(text, lower, stop)) return true;

    // Partie LoL en cours + question de build : conseiller local (données live + données officielles Riot).
    if (auto lol = d_.coach.Latest();
        lol && ContainsAny(lower, {"build", "stuff", "objet", "item", "acheter", "achète", "quoi faire", "j'achète"})) {
        AdviseLolBuild(*lol, text, stop);
        return true;
    }
    return false;
}

void AgentOrchestrator::HandleLevel1(const std::string& text, const std::vector<ChatTurn>& history,
                                     const std::string& extraContext, std::stop_token stop) {
    if (!EnsureBrain()) {
        // Pas de modèle local installé : Claude seulement si autorisé.
        EscalateCloud("REASON", text, text, stop);
        return;
    }
    // Mode décidé AVANT de répondre : technique (prioritaire en cas de doute) ou rage bait (bavardage, excuses, ego).
    chatMode_ = DetectChatMode(Lower(text));
    searchRound_ = 0;
    log::Info("Orchestrator: mode {}", chatMode_ == ChatMode::Technical ? "technique" : "rage bait");
    std::vector<ChatTurn> messages{{"system", ModePrompt()}};
    messages.insert(messages.end(), history.begin(), history.end());
    // Technique : rien qui distraie (memes, bruitages, rage bait) ; rage bait : tout le sel habituel.
    const std::string ctx = chatMode_ == ChatMode::Technical
                                ? LiveContext(text) + CallHint() + extraContext
                                : LiveContext(text) + CallHint() + extraContext + OpenerHint() + MemeHint() + SoundHint() + RageBaitHint(text);
    messages.push_back({"user", ctx.empty() ? text : "[Contexte]\n" + ctx + "\n[Message]\n" + text});

    // Procédure, réglage, code d'erreur : le modèle n'écrit que les mots-clés, la réponse viendra de la source.
    // (Pas quand le contexte est déjà fourni : vu sur le terrain, « tu peux écouter ce que je fais sur FL Studio ? » partait
    // chercher « FL Studio LUFS -22.4 » sur le web au lieu de donner l'avis sur la prod.)
    if (chatMode_ == ChatMode::Technical && d_.http && extraContext.empty() && NeedsSource(Lower(text))) {
        GenerateOptions so;
        so.grammar = kSearchOnlyGrammar;
        so.maxTokens = 32;
        so.temperature = 0.2f;
        std::smatch m;
        if (const auto kw = d_.brain.Generate(messages, so); kw && std::regex_search(*kw, m, EscalateTag())) {
            std::string type, subject;
            EscalationOf(m, type, subject);
            log::Info("Orchestrator: source exigée, recherche « {} »", Trim(subject));
            ResearchLocally(Trim(subject), text, stop);
            return;
        }
    }

    bool escalation = false;
    // Rage bait : 2 à 3 phrases, sec ; technique : de quoi expliquer.
    const std::string out = GenerateSpoken(messages, kEscalateGrammar, stop, escalation, nullptr,
                                           chatMode_ == ChatMode::Technical ? 260 : 70);
    if (stop.stop_requested()) return;
    if (!escalation) {
        if (!out.empty()) {
            d_.db.AddMessage("assistant", out);
            RememberOpener(out);
        }
        return;
    }

    std::smatch m;
    if (!std::regex_search(out, m, EscalateTag())) {
        Say(Line("stuck", {}, "Je bloque là-dessus."));
        return;
    }
    std::string type, subject;
    EscalationOf(m, type, subject);
    subject = Trim(subject);
    log::Info("Orchestrator: escalade {} « {} »", type, subject);
    if (type == "WEB") ResearchLocally(subject, text, stop);
    else if (type == "REASON") EscalateCloud(type, subject, text, stop);
    else HandleVision(text, stop);
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::ResearchLocally(const std::string& subject, const std::string& question,
                                        std::stop_token stop) {
    activity_ = AgentActivity::Researching;

    // 1) Déjà appris ? (cache SQLite FTS5, récent) -> 100 % local, aucun réseau.
    const auto maxAge = static_cast<std::int64_t>(cfg_.knowledgeMaxAgeDays) * 86400;
    std::string sources;
    std::string origin;
    for (const auto& l : d_.db.SearchLessons(subject + " " + question, 2)) {
        if (!MemoryDB::CoversSubject(l.subject + " " + l.fiche, subject)) continue;
        sources += "### Fiche apprise : " + l.subject + "\n" + l.fiche + "\n";
        origin = "fiche-apprise";
    }
    for (const auto& hit : d_.db.SearchKnowledge(subject, 3)) {
        if (hit.ageSeconds > maxAge || !Relevant(hit, subject)) continue;
        sources += "### " + hit.title + "\n" + WebResearcher::BestPassages(hit.content, subject + " " + question, 1500);
        origin = "cache-local";
    }

    // 2) Technique : le vrai web d'abord (un manuel Profinet n'est pas sur Wikipédia). Permission : « tu peux surfer
    //    cette nuit », ou ta réponse à la question (« toujours » la retient).
    if (sources.empty() && chatMode_ == ChatMode::Technical && d_.http) {
        const bool allowed = WebSurf::Allowed(d_.db) ||
                             d_.gate.Request(PermissionScope::Web, "web-libre", Line("askWebFree", {}, "Je cherche ça sur le web ?"),
                                             std::chrono::seconds(20), stop);
        if (stop.stop_requested()) return;
        if (!allowed) {
            Say(Line("webDenied", {}, "Ok, pas de web."));
            return;
        }
        {
            Say(Line("lookUp", {}, "Je regarde."));
            WebSurf surf(*d_.http);
            int pages = 4;
            sources = surf.Research(subject, 3, pages);
            if (!sources.empty()) {
                d_.db.AddKnowledge("web-libre", subject, sources);
                origin = "web-libre";
            }
        }
    }
    // 3) Sinon, wiki en liste blanche, avec ta permission.
    if (sources.empty()) {
        const bool inLol = d_.coach.Latest().has_value();
        const WikiSource* src = d_.web.PickSource(subject + " " + question + (inLol ? " lol" : ""));
        if (!src) { EscalateCloud("WEB", subject, question, stop); return; }
        if (!d_.gate.Request(PermissionScope::Web, src->host, Line("askWeb", {{"host", src->host}}, "Je vérifie sur " + src->host + " ?"),
                             std::chrono::seconds(20), stop)) {
            if (!stop.stop_requested()) Say(Line("webDenied", {}, "Ok, pas de web."));
            return;
        }
        Say(Line("lookUp", {}, "Je regarde."));
        for (auto& doc : d_.web.Search(*src, subject, 2, stop)) {
            d_.db.AddKnowledge(doc.url, doc.title, doc.text);  // appris : la prochaine fois, sans réseau
            sources += "### " + doc.title + "\n" + doc.excerpt + "\n";
        }
        origin = "web-local:" + src->host;
    }
    if (stop.stop_requested()) return;

    if (sources.empty()) {
        EscalateCloud("WEB", subject, question, stop);  // le local n'a rien donné : dernier recours
        return;
    }
    AnswerFromSources(question, sources, origin, subject, stop);
}

void AgentOrchestrator::AnswerFromSources(const std::string& question, const std::string& sources,
                                          const std::string& origin, const std::string& subject,
                                          std::stop_token stop) {
    if (!EnsureBrain()) {
        EscalateCloud("WEB", subject, question, stop);
        return;
    }
    const std::string ctx = LiveContext(question);
    std::vector<ChatTurn> messages{
        {"system", ModePrompt()},
        {"user", "[Contexte]\n" + ctx + "\n<contexte_web>\n" + sources.substr(0, 6000) + "\n</contexte_web>\n[Consigne]\n" +
                     WebContextRules(chatMode_) + " Les sources peuvent être en anglais : réponds en français.\n[Question]\n" + question}};
    // Réponse générée en entier PUIS vérifiée : « Information absente de la source » ne doit pas être dite telle quelle.
    GenerateOptions o;
    o.maxTokens = chatMode_ == ChatMode::Technical ? 260 : 120;
    o.temperature = chatMode_ == ChatMode::Technical ? 0.2f : 0.7f;
    o.stop = stop;
    activity_ = AgentActivity::Thinking;
    const auto out = d_.brain.Generate(messages, o);
    if (stop.stop_requested() || !out) return;
    const std::string answer = Trim(*out);
    std::smatch m;
    const bool absent = answer.find(kAbsentMarker) != std::string::npos;
    const bool again = std::regex_search(answer, m, EscalateTag());
    if (absent || (again && answer.rfind('[', 0) == 0)) {
        // Une seule recherche plus ciblée ; toujours rien -> Claude (avec ta permission).
        std::string type, next;
        if (again) EscalationOf(m, type, next);
        if (searchRound_ == 0 && again && type == "WEB" && Trim(next) != subject) {
            ++searchRound_;
            log::Info("Orchestrator: info absente, recherche plus ciblée « {} »", Trim(next));
            ResearchLocally(Trim(next), question, stop);
            return;
        }
        log::Info("Orchestrator: info toujours absente après recherche -> Claude");
        EscalateCloud("REASON", subject, question, stop);
        return;
    }
    // Balise en fin de réponse (fréquent en rage bait) : on dit ce qui précède, jamais la balise.
    std::string spoken = answer;
    if (again) spoken = Trim(answer.substr(0, static_cast<std::size_t>(m.position(0))));
    if (spoken.empty()) {
        EscalateCloud("REASON", subject, question, stop);
        return;
    }
    Say(spoken);
    d_.db.LogEscalation("WEB", subject, origin, spoken, question);
}

void AgentOrchestrator::EscalateCloud(const std::string& type, const std::string& subject,
                                      const std::string& question, std::stop_token stop,
                                      const std::vector<std::uint8_t>* jpeg) {
    if (!cfg_.cloudEnabled || !d_.claude.HasCredentials()) {
        Say(d_.brain.Available() ? "Je ne trouve pas en local, et Claude n'est pas configuré."
                                 : "Mon modèle local n'est pas installé, et Claude n'est pas configuré.");
        d_.db.LogEscalation(type, subject, "non-resolu", "", question);
        return;
    }
    if (cfg_.cloudAsksPermission &&
        !d_.gate.Request(PermissionScope::Cloud, "claude", Line("askCloud", {}, "Je ne trouve pas en local. Je demande à Claude ?"),
                         std::chrono::seconds(20), stop)) {
        if (!stop.stop_requested()) Say(Line("cloudDenied", {}, "Ok, on reste en local."));
        d_.db.LogEscalation(type, subject, "refuse", "", question);
        return;
    }
    activity_ = AgentActivity::Cloud;
    ClaudeRequest req;
    req.system = PersonaPrompt() + "\nTa réponse sera lue par une synthèse vocale : 2 à 4 phrases, sans liste ni markdown.";
    req.history = d_.db.RecentMessages(6);
    if (!req.history.empty() && req.history.back().role == "user") req.history.pop_back();  // c'est la question
    const std::string ctx = LiveContext(question);
    req.userText = (ctx.empty() ? "" : "[Contexte]\n" + ctx + "\n") + "[Sujet] " + subject + "\n[Question] " + question;
    if (jpeg && !jpeg->empty()) req.image = ClaudeImage{"image/jpeg", ClaudeClient::Base64(jpeg->data(), jpeg->size())};

    const ClaudeReply reply = d_.claude.Send(req);
    if (stop.stop_requested()) return;
    if (!reply.ok) {
        Say(Line("cloudError", {{"error", reply.error}}, "Claude ne répond pas : " + reply.error));
        return;
    }
    Say(reply.text);
    if (!jpeg) d_.db.AddKnowledge("claude", subject, reply.text);  // réponse validée : réutilisable en local
    d_.db.LogEscalation(type, subject, "claude", reply.text, question);
}

void AgentOrchestrator::StartTraining(bool manual, bool silent) {
    if (silent) manual = true;  // bail sans attendre 5 min d'inactivité
    if (d_.trainer.Running()) {
        if (silent) return;
        if (manual) Say(Line("trainingBusy", {}, "Je suis déjà en train de m'entraîner."));
        return;
    }
    auto token = d_.governor.BeginBackgroundWork(manual);
    if (!token) {
        if (manual && !silent) Say(Line("trainingRefused", {}, "Pas maintenant, un jeu ou FL Studio tourne."));
        return;
    }
    lastAutoTraining_ = std::chrono::steady_clock::now();
    d_.brain.Unload();  // la session charge son propre modèle : pas de double occupation RAM/VRAM
    if (manual && !silent) Say(Line("trainingStart", {}, "Ok, je m'entraîne. Touche à quoi que ce soit et j'arrête."));

    const bool started = d_.trainer.Start(*token, mode_.load() != SystemMode::Gaming, [this, manual, silent](const TrainerOutcome& o) {
        d_.governor.EndBackgroundWork();
        if (o.interrupted) {
            log::Info("SelfTrainer: session interrompue");
            return;
        }
        const std::string report = d_.db.GetProfile("training.report").value_or("");
        if (!o.completed || report.empty()) return;
        // Manuel : tu attends la réponse. Automatique : tu étais absent, on te le dira à ton retour.
        if (manual && !silent) {
            PlayEventSound("trainingDone");
            d_.voice.Speak(report);
            d_.db.AddMessage("assistant", report);
        } else {
            d_.db.SetProfile("training.pending_report", report);
        }
    });
    if (!started) {
        d_.governor.EndBackgroundWork();
        if (manual && !silent) Say(Line("trainingFailed", {}, "Je n'arrive pas à lancer l'entraînement."));
    }
}

}  // namespace companion

namespace companion {

// ---------------------------------------------------------------------------
// Niveau 0 : contrôle du PC. Volume via l'API audio (jamais de touches simulées) ;
// touches multimédia via SendInput, refusées en jeu ; lancement d'applis de la liste blanche.
bool AgentOrchestrator::HandleSystemCommand(const std::string& text, const std::string& lower, std::stop_token stop) {
    static const std::regex kVolumeTo(R"(volume (?:à|a|au|sur) ?(\d{1,3}))");
    std::smatch m;
    if (std::regex_search(lower, m, kVolumeTo)) {
        d_.system.SetVolume(std::stoi(m[1].str()) / 100.f);
        return true;
    }
    if (ContainsAny(lower, {"monte le son", "monte le volume", "augmente le son", "augmente le volume", "plus fort"})) {
        d_.system.StepVolume(+0.1f);
        return true;
    }
    if (ContainsAny(lower, {"baisse le son", "baisse le volume", "diminue le son", "diminue le volume", "moins fort"})) {
        d_.system.StepVolume(-0.1f);
        return true;
    }
    if (ContainsAny(lower, {"coupe le son", "mets en muet", "mute"})) {
        d_.system.SetMute(true);
        return true;
    }
    if (ContainsAny(lower, {"remets le son", "unmute", "réactive le son"})) {
        d_.system.SetMute(false);
        return true;
    }

    std::optional<SystemControl::MediaKey> key;
    if (ContainsAny(lower, {"musique suivante", "chanson suivante", "son suivant", "piste suivante"})) key = SystemControl::MediaKey::Next;
    else if (ContainsAny(lower, {"précédente", "precedente", "son précédent", "son precedent"})) key = SystemControl::MediaKey::Previous;
    else if (ContainsAny(lower, {"musique en pause", "chanson en pause", "reprends la musique", "relance la musique",
                                 "remets la musique", "mets la musique", "lecture"}))
        key = SystemControl::MediaKey::PlayPause;
    if (key) {
        if (!d_.system.Media(*key)) Say(Line("noInputSimulation", {}, "Pas de touches simulées pendant que tu joues, anti-cheat oblige."));
        return true;
    }

    if (ContainsAny(lower, {"lance ", "ouvre ", "démarre ", "demarre "})) {
        if (const AppShortcut* app = d_.system.FindApp(text)) {
            if (!d_.gate.Request(PermissionScope::System, app->name, "Je lance " + app->name + " ?",
                                 std::chrono::seconds(20), stop))
                return true;
            Say(d_.system.Launch(*app) ? Line("launchOk", {}, "C'est parti.")
                                        : Line("launchFailed", {{"app", app->name}}, "Je n'ai pas réussi à lancer " + app->name + "."));
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::HandleStudioQuestion(const std::string& text, const std::vector<ChatTurn>& history,
                                             std::stop_token stop) {
    listenUntilMs_ = NowMs() + 90'000;
    UpdateStudioCapture();
    // Il faut au moins 3 s de son pour un avis fiable (fenêtre LUFS court terme).
    bool announced = false;
    for (int i = 0; i < 100 && !stop.stop_requested(); ++i) {  // 10 s max
        if (d_.analyzer.Snapshot().seconds >= 3.f) break;
        if (i == 10 && !announced) {
            Say(Line("studioWaiting", {}, "Fais tourner ton son, j'écoute."));
            announced = true;
        }
        Sleep(100);
    }
    if (stop.stop_requested()) return;
    const auto mix = d_.analyzer.Snapshot();
    if (mix.seconds < 1.f) {
        // FL tourne mais sa capture est muette : presque toujours l'ASIO natif de la carte son (Antelope, Focusrite…),
        // qui contourne Windows. On le dit clairement, avec les deux solutions, et on note les entrées disponibles.
        const bool flRunning = WasapiLoopback::FindProcessId(std::wstring(cfg_.studioProcess.begin(), cfg_.studioProcess.end())) != 0;
        if (flRunning && !d_.loopback.IsInputCapture()) {
            std::string inputs;
            for (const auto& n : WasapiLoopback::InputNames()) inputs += (inputs.empty() ? "" : " | ") + log::Narrow(n);
            log::Info("Studio: FL tourne mais sa capture est muette (ASIO natif ?). Entrées disponibles : {}", inputs);
            Say("FL tourne, mais je reçois rien. T'es sûrement en ASIO de ta carte son, ça passe pas par Windows. Dans FL, "
                "Options, Audio, prends FL Studio ASIO, et là je t'entends direct.");
        } else {
            Say(Line("studioSilent", {}, "Je n'entends rien. Lance la lecture et redemande-moi."));
        }
        return;
    }
    // Partage des rôles (cascade du rapport) : le C++ sait CE QUI NE VA PAS et QUOI FAIRE (diagnostic façon
    // OMNIS, exact) ; le modèle local ne fait qu'UNE phrase de réaction avec sa personnalité. Un 3B à qui
    // on confie le conseil technique le déforme (« ajoute un clipper », « baisse le kick » pour la 808).
    const auto hits = lexicon_.Match(text);
    const auto topics = MixLexicon::Topics(hits);
    std::vector<MixMetrics::Issue> advice;
    for (const auto& i : mix.Issues()) {
        if (advice.size() >= 2) break;
        if (!topics.empty() && std::ranges::find(topics, i.topic) == topics.end()) continue;
        if (!advice.empty() && i.severity < 8) break;  // un 2e conseil seulement s'il est grave
        advice.push_back(i);
    }
    std::string facts;
    if (advice.empty() && !topics.empty()) {
        facts = "Sur ce point précis, rien d'anormal mesuré. ";
        for (const auto& i : mix.Issues()) { advice.push_back(i); break; }
    }
    for (const auto& i : advice) facts += i.text + " ";

    studioFocus_ = true;  // pas de diagnostic brut dans le contexte : seulement les constats ci-dessous
    HandleLevel1(text, history,
                 MixLexicon::Describe(hits) + (facts.empty() ? "" : "[Constat de l'ingé son] " + facts + "\n") +
                     "[Consigne] Réagis en UNE seule phrase courte à ce constat, avec ton style (une vanne, une image), "
                     "SANS conseil technique, SANS chiffres : le conseil sera dit juste après toi.\n" +
                     std::string(ToneReminder(cfg_.tone)) + "\n",
                 stop);
    studioFocus_ = false;
    if (stop.stop_requested()) return;
    if (facts.empty()) {
        Say(Line("studioClean", {}, "Franchement, rien d'alarmant : équilibre, grave mono et marge corrects."));
        return;
    }
    Say(facts);  // le conseil exact, mot pour mot (normalisé pour la voix : « -4.1 LUFS » → « moins quatre virgule un »)
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::HandleVision(const std::string& question, std::stop_token stop) {
    activity_ = AgentActivity::Researching;
    const auto shot = GameCoach::CaptureScreen(720);
    if (!shot) {
        Say(Line("captureFailed", {}, "Je n'arrive pas à capturer l'écran. Passe le jeu en plein écran fenêtré et réessaie."));
        return;
    }

    // 1) Vision LOCALE si le modèle de vision est installé : l'image ne quitte pas le PC.
    if (!cfg_.llmMmproj.empty() && EnsureBrain() && d_.brain.HasVision()) {
        std::vector<ChatTurn> messages{
            {"system", PersonaPrompt()},
            {"user", std::string("[Capture de l'écran du joueur]\n") + LocalBrain::MediaMarker() +
                         "\n[Consigne] Décris ce qui compte pour répondre (nom de quête, objectif, énigme). "
                         "S'il faut une info précise que tu n'as pas (solution d'énigme, étapes de quête), "
                         "escalade en WEB avec le nom exact lu à l'écran.\n[Question]\n" + question}};
        bool escalation = false;
        const std::string out = GenerateSpoken(messages, kEscalateGrammar, stop, escalation, &*shot);
        if (stop.stop_requested()) return;
        if (!escalation && !out.empty()) {
            d_.db.AddMessage("assistant", out);
            return;
        }
        std::smatch m;
        if (escalation && std::regex_search(out, m, EscalateTag())) {
            std::string type, subject;
            EscalationOf(m, type, subject);
            if (type == "WEB") {  // ex. nom de quête lu à l'écran -> recherche, en mode technique (réponse fondée)
                chatMode_ = ChatMode::Technical;
                searchRound_ = 0;
                ResearchLocally(Trim(subject), question, stop);
                return;
            }
        }
        // REASON / VISION / sortie vide : on passe au dernier recours, avec l'image.
    }

    // 2) Dernier recours : Claude Vision (avec ta permission).
    EscalateCloud("VISION", "capture d'écran", question, stop, &shot->jpeg);
}

}  // namespace companion

namespace companion {

// Les wikis n'ont pas de builds à jour (pages de stratégie LoL archivées) et les sites de stats
// n'ont pas d'API publique : le conseil est calculé localement à partir de la partie en cours et
// des données officielles Riot (Data Dragon), puis formulé par le modèle local. 0 token Claude.
void AgentOrchestrator::AdviseLolBuild(const LolSnapshot& lol, const std::string& question, std::stop_token stop) {
    activity_ = AgentActivity::Researching;
    if (!d_.coach.HasStaticData() && !d_.coach.LoadCachedStaticData(d_.dataDir)) {
        if (!d_.gate.Request(PermissionScope::Web, "ddragon.leagueoflegends.com",
                             "Je télécharge les données officielles de Riot sur les objets ?", std::chrono::seconds(20),
                             stop)) {
            if (!stop.stop_requested()) Say(Line("riotDenied", {}, "Ok, sans les données Riot je ne peux pas conseiller d'objets."));
            return;
        }
        if (!d_.coach.DownloadStaticData(d_.dataDir)) {
            Say(Line("riotFailed", {}, "Impossible de récupérer les données de Riot pour l'instant."));
            return;
        }
    }
    const std::string advice = d_.coach.BuildAdvice(lol);
    if (stop.stop_requested() || advice.empty()) return;
    if (EnsureBrain()) {
        AnswerFromSources(question, advice, "riot-ddragon", lol.me.champion + " build", stop);
    } else {
        Say(advice);  // sans modèle local : l'analyse brute, lisible telle quelle
    }
}

}  // namespace companion
