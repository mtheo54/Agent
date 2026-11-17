#include "core/AgentOrchestrator.hpp"

#include "core/Log.hpp"
#include "core/Persona.hpp"

#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>

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
}

void AgentOrchestrator::Post(Job job) {
    {
        std::scoped_lock lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void AgentOrchestrator::OnModeChanged(SystemMode mode) {
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
    return std::format("[Ton de voix] Il semble {} ({}).\n", ToString(lastEmotion_.label), lastEmotion_.description);
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
    Post([this, text = std::move(text)](std::stop_token st) { Handle(text, st); });
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
        if (deviceDirty_.exchange(false)) {
            const bool gaming = mode_.load() == SystemMode::Gaming;
            d_.brain.Configure(!gaming, gaming ? cfg_.llmThreadsGaming : cfg_.llmThreads);
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
        if (cfg_.autoTraining && !d_.trainer.Running() &&
            std::chrono::steady_clock::now() - lastAutoTraining_ > std::chrono::minutes(cfg_.autoTrainingEveryMinutes) &&
            d_.governor.CanStartAutoTraining() &&
            !d_.db.PendingTrainingTopics(1, cfg_.knowledgeMaxAgeDays * 86400LL, 86400).empty())
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
    const bool wanted = studio || NowMs() < listenUntilMs_.load();
    if (wanted && !d_.loopback.Running()) {
        d_.analyzer.Reset();
        d_.analyzer.Start();
        auto sink = [this](const float* data, std::size_t frames) { d_.analyzer.Push(data, frames); };
        const std::wstring proc(cfg_.studioProcess.begin(), cfg_.studioProcess.end());
        // FL Studio seul si possible (la voix du compagnon n'est pas captée), sinon le mix système.
        if (!(studio && d_.loopback.Start(proc, sink))) d_.loopback.Start(L"", sink);
    } else if (!wanted && d_.loopback.Running()) {
        d_.loopback.Stop();
        d_.analyzer.Stop();
    }
}

void AgentOrchestrator::OnMixAlert(MixAlert alert, const MixMetrics& m) {
    // Thread d'analyse. Niveau 0 : pas de modèle, phrase fixe, limitée dans le temps par l'analyseur.
    if (!cfg_.studioAlerts || mode_.load() != SystemMode::Studio || d_.voice.IsSpeaking()) return;
    if (alert == MixAlert::Clipping)
        d_.voice.Speak(Line("clipping", {{"peak", std::format("{:.1f}", m.peakDb)}}, std::format("Attention, ça clippe : crête à {:.1f} dB.", m.peakDb)));
    else
        d_.voice.Speak(Line("phase", {}, "Problème de phase : ton mix s'annule en mono, vérifie tes élargisseurs."));
}

// ---------------------------------------------------------------------------
std::string AgentOrchestrator::PersonaPrompt() const { return companion::PersonaPrompt(cfg_.name, cfg_.tone); }

std::string AgentOrchestrator::Line(const std::string& key, const std::map<std::string, std::string>& vars,
                                    const std::string& fallback) {
    d_.phrases.SetTone(static_cast<Tone>(cfg_.tone));
    return d_.phrases.Pick(key, vars, fallback);
}

std::string AgentOrchestrator::LiveContext(const std::string& question) const {
    std::string ctx = ToneContext();
    if (auto lol = d_.coach.Latest()) ctx += lol->ToPromptSummary() + "\n";
    if (d_.loopback.Running()) {
        const auto mix = d_.analyzer.Snapshot();
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
    if (d_.brain.IsLoaded()) return true;
    const bool gaming = mode_.load() == SystemMode::Gaming;
    LocalBrainConfig bc;
    bc.model = cfg_.llmModel;
    bc.mmproj = cfg_.llmMmproj;
    bc.lora = cfg_.loraAdapter;
    bc.contextTokens = cfg_.llmContext;
    bc.gpuLayers = cfg_.llmGpuLayers;
    bc.threads = gaming ? cfg_.llmThreadsGaming : cfg_.llmThreads;
    d_.brain.Configure(!gaming, bc.threads);
    return d_.brain.Load(bc);
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
                                              std::stop_token stop, bool& isEscalation, const ScreenShot* image) {
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
        spoken = true;
        if (!said.empty()) d_.voice.Speak(said, replyStyle);
    };

    GenerateOptions opt;
    opt.grammar = grammar;
    opt.stop = stop;
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
        return true;
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
    if (!isEscalation && !Trim(pending).empty() && !stop.stop_requested()) speak(Trim(pending));
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
    const bool mixSubject = ContainsAny(lower, {"mix", "ma prod", "la prod", "mon son", "ce son", "le morceau",
                                                "mon morceau", "le master", "mon master", "mon beat", "ce beat"});
    const bool mixAsk = ContainsAny(lower, {"écoute", "ecoute", "avis", "pense", "comment", "sonne", "analyse", "trouve"});
    // Jargon de beatmaker (lexique d'OMNIS) : « la basse elle bave », « mon kick tape pas »…
    if ((mixSubject && mixAsk) || MixLexicon::IsMixRequest(lexicon_.Match(text), text)) {
        HandleStudioQuestion(text, history, stop);
        return;
    }
    HandleLevel1(text, history, "", stop);
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

    if (HandleSystemCommand(text, lower, stop)) return true;

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
    std::vector<ChatTurn> messages{{"system", PersonaPrompt()}};
    messages.insert(messages.end(), history.begin(), history.end());
    const std::string ctx = LiveContext(text) + extraContext + OpenerHint() + MemeHint() + RageBaitHint(text);
    messages.push_back({"user", ctx.empty() ? text : "[Contexte]\n" + ctx + "\n[Message]\n" + text});

    bool escalation = false;
    const std::string out = GenerateSpoken(messages, kEscalateGrammar, stop, escalation);
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
    const std::string type = m[1].str(), subject = Trim(m[2].str());
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

    // 2) Sinon, wiki en liste blanche, avec ta permission.
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
        {"system", PersonaPrompt()},
        {"user", "[Contexte]\n" + ctx + "\n[Sources]\n" + sources.substr(0, 6000) +
                     "\n[Consigne]\nRéponds à la question à partir des sources ci-dessus (elles peuvent être en "
                     "anglais, réponds en français). Si elles ne suffisent pas, dis-le franchement.\n[Question]\n" +
                     question}};
    bool escalation = false;
    const std::string answer = GenerateSpoken(messages, nullptr, stop, escalation);
    if (stop.stop_requested() || answer.empty()) return;
    d_.db.AddMessage("assistant", answer);
    d_.db.LogEscalation("WEB", subject, origin, answer, question);
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

void AgentOrchestrator::StartTraining(bool manual) {
    if (d_.trainer.Running()) {
        if (manual) Say(Line("trainingBusy", {}, "Je suis déjà en train de m'entraîner."));
        return;
    }
    auto token = d_.governor.BeginBackgroundWork(manual);
    if (!token) {
        if (manual) Say(Line("trainingRefused", {}, "Pas maintenant, un jeu ou FL Studio tourne."));
        return;
    }
    lastAutoTraining_ = std::chrono::steady_clock::now();
    d_.brain.Unload();  // la session charge son propre modèle : pas de double occupation RAM/VRAM
    if (manual) Say(Line("trainingStart", {}, "Ok, je m'entraîne. Touche à quoi que ce soit et j'arrête."));

    const bool started = d_.trainer.Start(*token, mode_.load() != SystemMode::Gaming, [this, manual](const TrainerOutcome& o) {
        d_.governor.EndBackgroundWork();
        if (o.interrupted) {
            log::Info("SelfTrainer: session interrompue");
            return;
        }
        const std::string report = d_.db.GetProfile("training.report").value_or("");
        if (!o.completed || report.empty()) return;
        // Manuel : tu attends la réponse. Automatique : tu étais absent, on te le dira à ton retour.
        if (manual) {
            d_.voice.Speak(report);
            d_.db.AddMessage("assistant", report);
        } else {
            d_.db.SetProfile("training.pending_report", report);
        }
    });
    if (!started) {
        d_.governor.EndBackgroundWork();
        if (manual) Say(Line("trainingFailed", {}, "Je n'arrive pas à lancer l'entraînement."));
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
        Say(Line("studioSilent", {}, "Je n'entends rien. Lance la lecture et redemande-moi."));
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
            if (m[1].str() == "WEB") {  // ex. nom de quête lu à l'écran -> wiki en liste blanche
                ResearchLocally(Trim(m[2].str()), question, stop);
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
