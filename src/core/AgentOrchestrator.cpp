#include "core/AgentOrchestrator.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>

namespace companion {
namespace {

// Le modèle local répond normalement, OU émet exactement une balise d'escalade (rapport §3).
constexpr const char* kEscalateGrammar = R"GBNF(
root      ::= [ ]* ( escalate | answer )
escalate  ::= "[ESCALATE | type: " kind " | sujet: " subject "]"
kind      ::= "WEB" | "VISION" | "REASON"
subject   ::= [^\]\n] [^\]\n]*
answer    ::= [^\[ \n] [^\x00]*
)GBNF";

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

AgentOrchestrator::AgentOrchestrator(OrchestratorDeps deps) : d_(deps) {}
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
    if (training_.joinable()) training_.join();  // le governor a déjà révoqué le bail
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

void AgentOrchestrator::OnTranscript(std::string text) {
    text = Trim(std::move(text));
    if (text.empty()) return;
    // Réponse à une question du Permission Gate : consommée immédiatement (le worker attend).
    if (d_.gate.OnUserReply(text)) return;

    // "Pause" coupe tout, tout de suite, sans attendre la file.
    const std::string lower = Lower(text);
    if (ContainsAny(lower, {"pause", "tais-toi", "tais toi", "stop"})) {
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
    while (!st.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, st, std::chrono::seconds(30), [&] { return !jobs_.empty() || deviceDirty_.load(); });
            if (st.stop_requested()) break;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                currentJob_ = std::stop_source{};
            }
        }
        if (deviceDirty_.exchange(false)) {
            const bool gaming = mode_.load() == SystemMode::Gaming;
            d_.brain.Configure(!gaming, gaming ? d_.cfg.llmThreadsGaming : d_.cfg.llmThreads);
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
        // Budget RAM veille (< 80 Mo) : on décharge le modèle après une période sans conversation.
        if (d_.brain.IsLoaded() &&
            std::chrono::steady_clock::now() - lastBrainUse_ > std::chrono::minutes(d_.cfg.llmUnloadAfterMinutes)) {
            d_.brain.Unload();
            log::Info("Orchestrator: modèle local déchargé (inactivité)");
        }
    }
}

// ---------------------------------------------------------------------------
std::string AgentOrchestrator::PersonaPrompt() const {
    // Stable d'un appel à l'autre (aucune date, aucun contexte variable) : le cache KV le réutilise.
    return std::format(
        "Tu es {}, le compagnon IA de ton utilisateur ; tu vis sur son PC. Tu parles à voix haute : "
        "réponds en 1 à 3 phrases courtes, en français parlé, en le tutoyant. Ton direct, avis tranchés, "
        "surtout sur la direction artistique, le beatmaking, l'arrangement, le mixage et le jeu vidéo. "
        "Jamais de formules robotiques (« en tant qu'IA », « n'hésite pas », « bien sûr ! »), pas de markdown.\n"
        "Si la réponse dépend d'infos récentes ou précises que tu n'as pas (build, patch, stats, quête, "
        "actualité), n'invente rien et réponds UNIQUEMENT : [ESCALATE | type: WEB | sujet: <recherche courte>]\n"
        "Si tu dois voir l'écran : [ESCALATE | type: VISION | sujet: <ce qu'il faut regarder>]\n"
        "Si c'est trop complexe pour toi : [ESCALATE | type: REASON | sujet: <résumé court>]",
        d_.cfg.name);
}

std::string AgentOrchestrator::LiveContext(const std::string& question) const {
    std::string ctx;
    if (auto lol = d_.coach.Latest()) ctx += lol->ToPromptSummary() + "\n";
    for (const auto& m : d_.db.SearchMemories(question, 3)) ctx += "Souvenir : " + m + "\n";
    return ctx;
}

bool AgentOrchestrator::EnsureBrain() {
    lastBrainUse_ = std::chrono::steady_clock::now();
    if (d_.brain.IsLoaded()) return true;
    const bool gaming = mode_.load() == SystemMode::Gaming;
    LocalBrainConfig bc;
    bc.model = d_.cfg.llmModel;
    bc.contextTokens = d_.cfg.llmContext;
    bc.gpuLayers = d_.cfg.llmGpuLayers;
    bc.threads = gaming ? d_.cfg.llmThreadsGaming : d_.cfg.llmThreads;
    d_.brain.Configure(!gaming, bc.threads);
    return d_.brain.Load(bc);
}

void AgentOrchestrator::Say(const std::string& text) {
    if (text.empty()) return;
    d_.voice.Speak(text);
    d_.db.AddMessage("assistant", text);
}

std::string AgentOrchestrator::GenerateSpoken(const std::vector<ChatTurn>& messages, const char* grammar,
                                              std::stop_token stop, bool& isEscalation) {
    // Les phrases partent vers le TTS dès qu'elles sont complètes : la voix démarre avant la fin
    // de la génération. Une balise [ESCALATE…] n'est jamais prononcée.
    std::string pending;
    bool decided = false;
    isEscalation = false;
    GenerateOptions opt;
    opt.grammar = grammar;
    opt.stop = stop;
    opt.onPiece = [&](std::string_view piece) {
        pending.append(piece);
        if (!decided) {
            const auto first = pending.find_first_not_of(" \n");
            if (first == std::string::npos) return true;
            decided = true;
            isEscalation = pending[first] == '[';
        }
        if (isEscalation) return true;
        const auto cut = pending.find_last_of(".!?\n");
        if (cut != std::string::npos && cut >= 25) {
            d_.voice.Speak(Trim(pending.substr(0, cut + 1)));
            pending.erase(0, cut + 1);
        }
        return true;
    };
    activity_ = AgentActivity::Thinking;
    const auto out = d_.brain.Generate(messages, opt);
    lastBrainUse_ = std::chrono::steady_clock::now();
    if (!out) return {};
    if (!isEscalation && !Trim(pending).empty() && !stop.stop_requested()) d_.voice.Speak(Trim(pending));
    return Trim(*out);
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::Handle(const std::string& text, std::stop_token stop) {
    log::Info("Orchestrator: « {} »", text);
    const auto history = d_.db.RecentMessages(8);  // avant d'ajouter la question courante
    d_.db.AddMessage("user", text);
    if (HandleLevel0(text, stop)) return;
    HandleLevel1(text, history, stop);
}

bool AgentOrchestrator::HandleLevel0(const std::string& text, std::stop_token stop) {
    const std::string lower = Lower(text);

    if (lower.find("entra") != std::string::npos && lower.find("toi") != std::string::npos) {  // « entraîne-toi »
        StartTraining();
        return true;
    }

    for (const char* prefix : {"retiens que ", "souviens-toi que ", "souviens toi que ", "note que "}) {
        if (const auto p = lower.find(prefix); p != std::string::npos) {
            const std::string fact = Trim(text.substr(p + std::string_view(prefix).size()));
            if (!fact.empty()) {
                d_.db.AddMemory(fact);
                Say("Noté.");
                return true;
            }
        }
    }

    if (ContainsAny(lower, {"regarde mon écran", "regarde l'écran", "regarde mon ecran"})) {
        Say("La capture d'écran arrive avec le module jeux solo, je ne peux pas encore voir ton écran.");
        return true;
    }

    // Partie LoL en cours + question de build : recherche ciblée directe, sans passer par le modèle.
    if (auto lol = d_.coach.Latest();
        lol && ContainsAny(lower, {"build", "stuff", "objet", "item", "acheter", "achète", "quoi faire", "j'achète"})) {
        ResearchLocally(lol->me.champion + " build items", text, stop);
        return true;
    }
    return false;
}

void AgentOrchestrator::HandleLevel1(const std::string& text, const std::vector<ChatTurn>& history,
                                     std::stop_token stop) {
    if (!EnsureBrain()) {
        // Pas de modèle local installé : Claude seulement si autorisé.
        EscalateCloud("REASON", text, text, stop);
        return;
    }
    std::vector<ChatTurn> messages{{"system", PersonaPrompt()}};
    messages.insert(messages.end(), history.begin(), history.end());
    const std::string ctx = LiveContext(text);
    messages.push_back({"user", ctx.empty() ? text : "[Contexte]\n" + ctx + "\n[Message]\n" + text});

    bool escalation = false;
    const std::string out = GenerateSpoken(messages, kEscalateGrammar, stop, escalation);
    if (stop.stop_requested()) return;
    if (!escalation) {
        if (!out.empty()) d_.db.AddMessage("assistant", out);
        return;
    }

    static const std::regex kTag(R"(\[ESCALATE\s*\|\s*type:\s*(WEB|VISION|REASON)\s*\|\s*sujet:\s*([^\]]+)\])");
    std::smatch m;
    if (!std::regex_search(out, m, kTag)) {
        Say("Je bloque là-dessus.");
        return;
    }
    const std::string type = m[1].str(), subject = Trim(m[2].str());
    log::Info("Orchestrator: escalade {} « {} »", type, subject);
    if (type == "WEB") ResearchLocally(subject, text, stop);
    else if (type == "REASON") EscalateCloud(type, subject, text, stop);
    else Say("Il faudrait que je voie ton écran, et ça arrive à la prochaine étape.");
}

// ---------------------------------------------------------------------------
void AgentOrchestrator::ResearchLocally(const std::string& subject, const std::string& question,
                                        std::stop_token stop) {
    activity_ = AgentActivity::Researching;

    // 1) Déjà appris ? (cache SQLite FTS5, récent) -> 100 % local, aucun réseau.
    const auto maxAge = static_cast<std::int64_t>(d_.cfg.knowledgeMaxAgeDays) * 86400;
    std::string sources;
    std::string origin;
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
        if (!d_.gate.Request(PermissionScope::Web, src->host, "Je vérifie sur " + src->host + " ?",
                             std::chrono::seconds(20), stop)) {
            if (!stop.stop_requested()) Say("Ok, pas de web.");
            return;
        }
        Say("Je regarde.");
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
    d_.db.LogEscalation("WEB", subject, origin, answer);
}

void AgentOrchestrator::EscalateCloud(const std::string& type, const std::string& subject,
                                      const std::string& question, std::stop_token stop) {
    if (!d_.cfg.cloudEnabled || !d_.claude.HasCredentials()) {
        Say(d_.brain.Available() ? "Je ne trouve pas en local, et Claude n'est pas configuré."
                                 : "Mon modèle local n'est pas installé, et Claude n'est pas configuré.");
        d_.db.LogEscalation(type, subject, "non-resolu", "");
        return;
    }
    if (d_.cfg.cloudAsksPermission &&
        !d_.gate.Request(PermissionScope::Cloud, "claude", "Je ne trouve pas en local. Je demande à Claude ?",
                         std::chrono::seconds(20), stop)) {
        if (!stop.stop_requested()) Say("Ok, on reste en local.");
        d_.db.LogEscalation(type, subject, "refuse", "");
        return;
    }
    activity_ = AgentActivity::Cloud;
    ClaudeRequest req;
    req.system = PersonaPrompt() + "\nTa réponse sera lue par une synthèse vocale : 2 à 4 phrases, sans liste ni markdown.";
    req.history = d_.db.RecentMessages(6);
    if (!req.history.empty() && req.history.back().role == "user") req.history.pop_back();  // c'est la question
    const std::string ctx = LiveContext(question);
    req.userText = (ctx.empty() ? "" : "[Contexte]\n" + ctx + "\n") + "[Sujet] " + subject + "\n[Question] " + question;

    const ClaudeReply reply = d_.claude.Send(req);
    if (stop.stop_requested()) return;
    if (!reply.ok) {
        Say("Claude ne répond pas : " + reply.error);
        return;
    }
    Say(reply.text);
    d_.db.AddKnowledge("claude", subject, reply.text);  // réponse validée : réutilisable en local
    d_.db.LogEscalation(type, subject, "claude", reply.text);
}

void AgentOrchestrator::StartTraining() {
    auto token = d_.governor.BeginBackgroundWork(/*manualRequest=*/true);
    if (!token) {
        Say("Pas maintenant, un jeu ou FL Studio tourne.");
        return;
    }
    Say("Ok, je m'entraîne. Touche à quoi que ce soit et j'arrête.");
    if (training_.joinable()) training_.join();  // bail précédent déjà terminé
    // Placeholder du SelfTrainer (étape 4) : boucle interrompable qui vérifie le token toutes les 10 ms.
    training_ = std::jthread([this, t = *token] {
        ResourceGovernor::EnterBackgroundPriority();
        while (!t.stop_requested()) Sleep(10);
        ResourceGovernor::LeaveBackgroundPriority();
        d_.governor.EndBackgroundWork();
    });
}

}  // namespace companion
