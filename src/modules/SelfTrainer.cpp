#include "modules/SelfTrainer.hpp"

#include "core/LocalBrain.hpp"
#include "core/Log.hpp"
#include "core/MemoryDB.hpp"
#include "core/Persona.hpp"
#include "modules/GameCoach.hpp"
#include "modules/VideoStudy.hpp"
#include "network/WebSurf.hpp"
#include "core/BaitBank.hpp"
#include "network/HttpClient.hpp"
#include "network/WebResearcher.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <regex>

namespace companion {
namespace {

constexpr const char* kFicheSystem =
    "Tu rédiges des fiches de connaissances pour un assistant vocal. À partir des sources fournies "
    "(elles peuvent être en anglais), écris en français 3 à 6 faits clés, précis et vérifiables, qui "
    "répondent à la question. Une phrase courte par fait, sans markdown, sans rien inventer. "
    "Si les sources ne répondent pas à la question, écris uniquement : AUCUNE_INFO";

std::string Trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string JoinNames(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size() && i < 3; ++i) out += (i ? (i + 1 == std::min<size_t>(v.size(), 3) ? " et " : ", ") : "") + v[i];
    if (v.size() > 3) out += " et " + std::to_string(v.size() - 3) + " autre(s)";
    return out;
}

}  // namespace

int RunTrainerSession(const CompanionConfig& cfg, const std::filesystem::path& dataDir, bool useGpu, const std::filesystem::path& assetsDir) {
    using clock = std::chrono::system_clock;
    MemoryDB db;
    if (!db.Open(dataDir / L"companion.db")) return 2;
    HttpClient http(cfg.userAgent);
    WebResearcher web(http, cfg.wikis);

    // 0) Données officielles Riot : rafraîchies au nouveau patch, si tu as déjà dit « toujours ».
    if (db.GetPermission("web", "ddragon.leagueoflegends.com").value_or(false)) {
        GameCoach coach(http);
        coach.DownloadStaticData(dataDir);
    }

    // 1) Sujets qui ont bloqué : non appris, ou appris il y a longtemps (patchs), ou échec d'hier.
    const auto topics = db.PendingTrainingTopics(12, cfg.knowledgeMaxAgeDays * 86400LL, 86400);
    log::Info("SelfTrainer: {} sujet(s) à travailler", topics.size());
    if (topics.empty() && !cfg.videoStudy) {
        db.SetProfile("training.report", "");
        return 0;
    }

    LocalBrain brain;
    LocalBrainConfig bc;
    bc.model = cfg.llmModel;
    bc.lora = cfg.loraAdapter;
    bc.contextTokens = cfg.llmContext;
    bc.gpuLayers = cfg.llmGpuLayers;
    bc.threads = cfg.llmThreads;
    brain.Configure(useGpu, cfg.llmThreads);
    if (!brain.Load(bc)) return 3;

    // 1b) Vidéos : il regarde des vannes / du rage bait (YouTube, tes liens TikTok) et en tire des formes réutilisables.
    // Autorisation « surfe cette nuit » : pages web libres (lecture seule) et deux fois plus de vidéos.
    const bool freeWeb = WebSurf::Allowed(db);
    WebSurf surf(http);
    int pagesLeft = 12;
    if (freeWeb) log::Info("SelfTrainer: surf libre autorisé pour cette séance");
    std::string videoReport;
    if (cfg.videoStudy) {
        VideoStudyConfig vc;
        vc.perSession = cfg.videoStudyPerSession * (freeWeb ? 2 : 1);
        vc.tutorialsPerSession = freeWeb ? 3 : 1;  // nuit de surf : plus de tutos
        if (!cfg.videoStudyQueries.empty()) vc.queries = cfg.videoStudyQueries;
        for (const wchar_t* w : {L"ggml-base-q5_1.bin", L"ggml-tiny-q5_1.bin"})
            if (!assetsDir.empty() && std::filesystem::exists(assetsDir / L"models" / w)) {
                vc.whisperModel = assetsDir / L"models" / w;
                break;
            }
        BaitBank bank;
        bank.Load(assetsDir / L"persona" / L"baits.json", dataDir / L"learned_baits.json");
        VideoStudy vs(http, dataDir, vc);
        videoReport = vs.RunSession(brain, db, bank, {});
        if (const std::string tuto = vs.RunTutorialSession(brain, db, {}); !tuto.empty())
            videoReport += (videoReport.empty() ? "" : " ") + tuto;
    }

    std::error_code ec;
    std::filesystem::create_directories(dataDir / L"training", ec);
    std::ofstream dataset(dataDir / L"training" / L"dataset.jsonl", std::ios::app | std::ios::binary);
    const std::string persona = PersonaPrompt(cfg.name, cfg.tone);
    const auto maxAge = static_cast<std::int64_t>(cfg.knowledgeMaxAgeDays) * 86400;

    std::vector<std::string> learned;
    for (const TrainingTopic& t : topics) {
        // 2) Sources : connaissances déjà en cache (pages wiki, réponses validées par Claude)…
        std::string sources;
        for (const auto& hit : db.SearchKnowledge(t.subject + " " + t.question, 4)) {
            if (hit.ageSeconds > maxAge || !MemoryDB::CoversSubject(hit.title + " " + hit.content.substr(0, 6000), t.subject))
                continue;
            sources += "### " + hit.title + "\n" + WebResearcher::BestPassages(hit.content, t.subject + " " + t.question, 1500) + "\n";
        }
        // … sinon collecte web ciblée, uniquement sur un domaine que tu as autorisé avec « toujours ».
        if (sources.size() < 300 && freeWeb) {  // surf libre autorisé : moteur de recherche + 3 pages
            const std::string found = surf.Research(t.subject + " " + t.question, 3, pagesLeft);
            if (!found.empty()) {
                db.AddKnowledge("web-libre", t.subject, found);
                sources += found;
            }
        }
        if (sources.size() < 300) {
            const WikiSource* src = web.PickSource(t.subject + " " + t.question);
            if (!src || !db.GetPermission("web", src->host).value_or(false)) {
                log::Info("SelfTrainer: « {} » : aucun domaine autorisé en permanence", t.subject);
                db.MarkTrained(t.subject, "sans-permission");
                continue;
            }
            for (auto& doc : web.Search(*src, t.subject, 2, {})) {
                db.AddKnowledge(doc.url, doc.title, doc.text);
                sources += "### " + doc.title + "\n" + doc.excerpt + "\n";
            }
        }
        if (Trim(sources).empty()) {
            db.MarkTrained(t.subject, "echec");
            continue;
        }

        // 3) Distillation : une fiche de faits clés, rédigée par le modèle local.
        GenerateOptions ficheOpt;
        ficheOpt.maxTokens = 320;
        ficheOpt.temperature = 0.2f;
        const auto fiche = brain.Generate(
            {{"system", kFicheSystem}, {"user", "[Sources]\n" + sources.substr(0, 6000) + "\n[Question]\n" + t.question}}, ficheOpt);
        if (!fiche || fiche->find("AUCUNE_INFO") != std::string::npos || Trim(*fiche).size() < 40) {
            db.MarkTrained(t.subject, "echec");
            continue;
        }

        // 4) Auto-évaluation : la question d'origine, rejouée avec la fiche, passe-t-elle en local ?
        GenerateOptions evalOpt;
        evalOpt.grammar = kEscalateGrammar;
        evalOpt.maxTokens = 160;
        evalOpt.temperature = 0.3f;
        const auto answer = brain.Generate(
            {{"system", persona},
             {"user", "[Contexte]\nFiche apprise (" + t.subject + ") : " + Trim(*fiche) + "\n[Message]\n" + t.question}},
            evalOpt);
        const bool stillEscalates = !answer || std::regex_search(*answer, EscalateTag());

        db.AddLesson(t.subject, t.question, Trim(*fiche));  // validée dans SQLite = point de reprise
        db.MarkTrained(t.subject, stillEscalates ? "partiel" : "appris");
        log::Info("SelfTrainer: « {} » -> {}", t.subject, stillEscalates ? "partiel" : "appris");
        if (stillEscalates) continue;

        learned.push_back(t.subject);
        // 5) Exemple validé, au format chat, pour un éventuel LoRA entraîné ailleurs.
        const nlohmann::json example = {
            {"messages", {{{"role", "system"}, {"content", persona}},
                          {{"role", "user"}, {"content", t.question}},
                          {{"role", "assistant"}, {"content", Trim(*answer)}}}}};
        dataset << example.dump() << "\n";
        dataset.flush();
    }

    std::string report =
        learned.empty() ? std::string{}
                        : "Pendant ta pause, j'ai appris " + std::to_string(learned.size()) + " sujet" +
                              (learned.size() > 1 ? "s" : "") + " : " + JoinNames(learned) + ".";
    if (!videoReport.empty()) report += (report.empty() ? "" : " ") + videoReport;
    db.SetProfile("training.report", report);
    db.SetProfile("training.last_ts",
                  std::to_string(std::chrono::duration_cast<std::chrono::seconds>(clock::now().time_since_epoch()).count()));
    return 0;
}

// ---------------------------------------------------------------------------
// Processus enfant isolé (Windows)
// ---------------------------------------------------------------------------
#if defined(_WIN32)

bool SelfTrainer::Start(std::stop_token token, bool useGpu, FinishedFn onFinished) {
    std::scoped_lock lock(mutex_);
    if (running_.load()) return false;
    if (monitor_.joinable()) monitor_.join();  // session précédente terminée

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --trainer" + (useGpu ? L" --gpu" : L"");

    // Job Object : RAM plafonnée, priorité IDLE imposée, tué si CompanionAI se ferme.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    limits.BasicLimitInformation.PriorityClass = IDLE_PRIORITY_CLASS;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(cfg_.trainerMemoryLimitMb) * 1024 * 1024;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | IDLE_PRIORITY_CLASS | CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        log::Info("SelfTrainer: lancement impossible ({})", GetLastError());
        CloseHandle(job);
        return false;
    }
    // Dans le Job AVANT de démarrer : aucune instruction ne s'exécute hors des limites.
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    job_ = job;
    process_ = pi.hProcess;
    running_ = true;
    log::Info("SelfTrainer: session lancée ({})", useGpu ? "GPU" : "CPU");

    monitor_ = std::jthread([this, token, onFinished = std::move(onFinished), job, proc = pi.hProcess] {
        log::SetCurrentThreadName(L"Companion.TrainerMonitor");
        std::atomic<bool> killed{false};
        {
            // Arrêt d'urgence : la révocation du bail exécute ce rappel immédiatement, sur le thread du
            // governor. TerminateJobObject coupe réseau, CPU et RAM du processus enfant d'un coup.
            std::stop_callback emergency(token, [&] {
                killed = true;
                TerminateJobObject(job, 1);
            });
            WaitForSingleObject(proc, INFINITE);
        }
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        {
            std::scoped_lock lock(mutex_);
            CloseHandle(proc);
            CloseHandle(job);
            job_ = process_ = nullptr;
        }
        running_ = false;
        TrainerOutcome out;
        out.interrupted = killed.load();
        out.completed = !out.interrupted && code == 0;
        log::Info("SelfTrainer: fin de session (code {}, {})", code, out.interrupted ? "interrompue" : "terminée");
        if (onFinished) onFinished(out);
    });
    return true;
}

void SelfTrainer::Kill() {
    {
        std::scoped_lock lock(mutex_);
        if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
    }
    if (monitor_.joinable() && monitor_.get_id() != std::this_thread::get_id()) monitor_.join();
}

#else  // Hors Windows : pas de processus isolé (la session reste testable via RunTrainerSession).

bool SelfTrainer::Start(std::stop_token, bool, FinishedFn) { return false; }
void SelfTrainer::Kill() {}

#endif

}  // namespace companion
