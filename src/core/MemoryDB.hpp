#pragma once
// MemoryDB : mémoire persistante SQLite3 (rapport §5 : profil DA, historique, file d'entraînement).
//
//   profile      clé/valeur (profil DA, préférences)
//   messages     historique de conversation
//   memories     FTS5 — faits retenus sur toi et tes projets ("retiens que...")
//   knowledge    FTS5 — connaissances apprises (pages de wiki, réponses de Claude) :
//                une question déjà résolue est ensuite traitée 100 % en local
//   escalations  journal des [ESCALATE] (et de leur résolution) — matière première du SelfTrainer
//   lessons      FTS5 — fiches distillées par le SelfTrainer pendant l'inactivité du PC
//   training_log sujets déjà travaillés (appris / partiel / échec / sans-permission)
//   permissions  décisions mémorisées du Permission Gate ("toujours" / "jamais")

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace companion {

struct ChatTurn {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};

struct Lesson {
    std::string subject;
    std::string question;
    std::string fiche;
};

struct TrainingTopic {
    std::string type;  // WEB | REASON
    std::string subject;
    std::string question;
};

struct KnowledgeHit {
    std::string source;
    std::string title;
    std::string content;
    std::int64_t ageSeconds = 0;
};

class MemoryDB {
public:
    MemoryDB() = default;
    ~MemoryDB();
    MemoryDB(const MemoryDB&) = delete;
    MemoryDB& operator=(const MemoryDB&) = delete;

    bool Open(const std::filesystem::path& file);
    void Close();

    // Historique
    void AddMessage(std::string_view role, std::string_view content);
    std::vector<ChatTurn> RecentMessages(int maxTurns);

    // Profil
    void SetProfile(std::string_view key, std::string_view value);
    std::optional<std::string> GetProfile(std::string_view key);

    // Mémoire à long terme
    void AddMemory(std::string_view fact);
    std::vector<std::string> SearchMemories(std::string_view query, int limit);

    // Connaissances apprises (cache web / réponses Claude)
    void AddKnowledge(std::string_view source, std::string_view title, std::string_view content);
    std::vector<KnowledgeHit> SearchKnowledge(std::string_view query, int limit);

    // Escalades
    void LogEscalation(std::string_view type, std::string_view subject, std::string_view resolvedBy,
                       std::string_view answer, std::string_view question = {});

    // Auto-entraînement
    // Sujets escaladés pas encore appris (ni re-tentés trop récemment), du plus récent au plus ancien.
    std::vector<TrainingTopic> PendingTrainingTopics(int limit, std::int64_t relearnAfterSec, std::int64_t retryAfterSec);
    void MarkTrained(std::string_view subject, std::string_view status);
    void AddLesson(std::string_view subject, std::string_view question, std::string_view fiche);
    std::vector<Lesson> SearchLessons(std::string_view query, int limit);
    int CountLessons();

    // Permission Gate
    std::optional<bool> GetPermission(std::string_view scope, std::string_view target);
    void SetPermission(std::string_view scope, std::string_view target, bool allow);

    // Requête FTS5 sûre à partir d'un texte libre : mots >= 3 lettres, entre guillemets, en OR.
    static std::string ToFtsQuery(std::string_view text);
    // Au moins la moitié des mots (>= 3 lettres) de `subject` figurent dans `text`.
    static bool CoversSubject(std::string_view text, std::string_view subject);

private:
    bool Exec(const char* sql);

    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace companion
