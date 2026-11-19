#include "core/MemoryDB.hpp"

#include "core/Log.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cctype>
#include <set>

namespace companion {
namespace {

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Requête préparée RAII.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) {
            log::Info("SQLite: {} ({})", sqlite3_errmsg(db), sql);
            s_ = nullptr;
        }
    }
    ~Stmt() { sqlite3_finalize(s_); }
    explicit operator bool() const noexcept { return s_ != nullptr; }
    Stmt& Bind(int i, std::string_view v) {
        sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& Bind(int i, std::int64_t v) {
        sqlite3_bind_int64(s_, i, v);
        return *this;
    }
    bool Step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    void Run() { if (s_) sqlite3_step(s_); }
    std::string Text(int col) {
        const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s_, col));
        return t ? std::string(t, static_cast<size_t>(sqlite3_column_bytes(s_, col))) : std::string{};
    }
    std::int64_t Int(int col) { return sqlite3_column_int64(s_, col); }

private:
    sqlite3_stmt* s_ = nullptr;
};

constexpr const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS profile(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS escalations(id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, type TEXT, subject TEXT,
                                       resolved_by TEXT, answer TEXT);
CREATE TABLE IF NOT EXISTS permissions(scope TEXT NOT NULL, target TEXT NOT NULL, allow INTEGER NOT NULL,
                                       PRIMARY KEY(scope, target));
CREATE VIRTUAL TABLE IF NOT EXISTS memories USING fts5(content, ts UNINDEXED,
                                                       tokenize='unicode61 remove_diacritics 2');
CREATE VIRTUAL TABLE IF NOT EXISTS knowledge USING fts5(source, title, content, ts UNINDEXED,
                                                        tokenize='unicode61 remove_diacritics 2');
CREATE VIRTUAL TABLE IF NOT EXISTS lessons USING fts5(subject, question, fiche, ts UNINDEXED,
                                                      tokenize='unicode61 remove_diacritics 2');
CREATE TABLE IF NOT EXISTS training_log(subject TEXT PRIMARY KEY, ts INTEGER NOT NULL, status TEXT NOT NULL,
                                        attempts INTEGER NOT NULL DEFAULT 1);
)SQL";

}  // namespace

MemoryDB::~MemoryDB() { Close(); }

bool MemoryDB::Open(const std::filesystem::path& file) {
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::string utf8 = log::Narrow(file.wstring());
    if (sqlite3_open_v2(utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        log::Info("MemoryDB: ouverture impossible ({})", db_ ? sqlite3_errmsg(db_) : "?");
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);  // le SelfTrainer (autre processus) écrit dans la même base
    if (!Exec(kSchema)) return false;
    // Ménage au démarrage (la base ne grossit plus sans fin) : 20 000 derniers messages, pages web de plus de 120 jours
    // (rechargées si besoin ; leçons et souvenirs, eux, sont gardés), escalades de plus d'un an.
    Exec("DELETE FROM messages WHERE id <= (SELECT MAX(id) - 20000 FROM messages);"
         "DELETE FROM knowledge WHERE CAST(ts AS INTEGER) < strftime('%s','now') - 120*86400;"
         "DELETE FROM escalations WHERE ts < strftime('%s','now') - 365*86400;");
    // Migration étape 4 : question d'origine des escalades (erreur ignorée si la colonne existe déjà).
    sqlite3_exec(db_, "ALTER TABLE escalations ADD COLUMN question TEXT", nullptr, nullptr, nullptr);
    return true;
}

void MemoryDB::Close() {
    std::scoped_lock lock(mutex_);
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

bool MemoryDB::Exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        log::Info("SQLite: {}", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::string MemoryDB::ToFtsQuery(std::string_view text) {
    std::set<std::string> words;
    std::string cur;
    auto flush = [&] {
        // Longueur en octets : un mot accentué de 3 lettres fait >= 3 octets, c'est voulu.
        if (cur.size() >= 3 && words.size() < 16) words.insert(cur);
        cur.clear();
    };
    for (unsigned char c : text) {
        if (c >= 0x80 || std::isalnum(c)) cur.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
        else flush();
    }
    flush();
    std::string q;
    for (const auto& w : words) {
        if (!q.empty()) q += " OR ";
        q += '"' + w + '"';  // pas de guillemet possible dans w : seuls alnum/UTF-8 retenus
    }
    return q;
}

void MemoryDB::AddMessage(std::string_view role, std::string_view content) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO messages(ts, role, content) VALUES(?,?,?)");
    if (s) s.Bind(1, Now()).Bind(2, role).Bind(3, content).Run();
}

std::vector<ChatTurn> MemoryDB::RecentMessages(int maxTurns) {
    std::scoped_lock lock(mutex_);
    std::vector<ChatTurn> out;
    if (!db_) return out;
    Stmt s(db_, "SELECT role, content FROM (SELECT id, role, content FROM messages ORDER BY id DESC LIMIT ?) "
                "ORDER BY id ASC");
    if (!s) return out;
    s.Bind(1, static_cast<std::int64_t>(maxTurns));
    while (s.Step()) out.push_back({s.Text(0), s.Text(1)});
    return out;
}

void MemoryDB::SetProfile(std::string_view key, std::string_view value) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO profile(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (s) s.Bind(1, key).Bind(2, value).Run();
}

std::optional<std::string> MemoryDB::GetProfile(std::string_view key) {
    std::scoped_lock lock(mutex_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT value FROM profile WHERE key=?");
    if (s && s.Bind(1, key).Step()) return s.Text(0);
    return std::nullopt;
}

void MemoryDB::AddMemory(std::string_view fact) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO memories(content, ts) VALUES(?,?)");
    if (s) s.Bind(1, fact).Bind(2, Now()).Run();
}

std::vector<std::string> MemoryDB::SearchMemories(std::string_view query, int limit) {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    const std::string q = ToFtsQuery(query);
    if (!db_ || q.empty()) return out;
    Stmt s(db_, "SELECT content FROM memories WHERE memories MATCH ? ORDER BY rank LIMIT ?");
    if (!s) return out;
    s.Bind(1, q).Bind(2, static_cast<std::int64_t>(limit));
    while (s.Step()) out.push_back(s.Text(0));
    return out;
}

void MemoryDB::AddKnowledge(std::string_view source, std::string_view title, std::string_view content) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    {  // une seule version par (source, titre) : la plus récente
        Stmt del(db_, "DELETE FROM knowledge WHERE source=? AND title=?");
        if (del) del.Bind(1, source).Bind(2, title).Run();
    }
    Stmt s(db_, "INSERT INTO knowledge(source, title, content, ts) VALUES(?,?,?,?)");
    if (s) s.Bind(1, source).Bind(2, title).Bind(3, content).Bind(4, Now()).Run();
}

std::vector<KnowledgeHit> MemoryDB::SearchKnowledge(std::string_view query, int limit) {
    std::scoped_lock lock(mutex_);
    std::vector<KnowledgeHit> out;
    const std::string q = ToFtsQuery(query);
    if (!db_ || q.empty()) return out;
    // bm25 pondéré : le titre compte 4x plus que le contenu.
    Stmt s(db_, "SELECT source, title, content, ts FROM knowledge WHERE knowledge MATCH ? "
                "ORDER BY bm25(knowledge, 1.0, 4.0, 1.0) LIMIT ?");
    if (!s) return out;
    s.Bind(1, q).Bind(2, static_cast<std::int64_t>(limit));
    const auto now = Now();
    while (s.Step()) out.push_back({s.Text(0), s.Text(1), s.Text(2), now - s.Int(3)});
    return out;
}

void MemoryDB::LogEscalation(std::string_view type, std::string_view subject, std::string_view resolvedBy,
                             std::string_view answer, std::string_view question) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO escalations(ts, type, subject, resolved_by, answer, question) VALUES(?,?,?,?,?,?)");
    if (s) s.Bind(1, Now()).Bind(2, type).Bind(3, subject).Bind(4, resolvedBy).Bind(5, answer).Bind(6, question).Run();
}

std::vector<TrainingTopic> MemoryDB::PendingTrainingTopics(int limit, std::int64_t relearnAfterSec,
                                                           std::int64_t retryAfterSec) {
    std::scoped_lock lock(mutex_);
    std::vector<TrainingTopic> out;
    if (!db_) return out;
    // Un sujet appris est revu après relearnAfterSec (patchs) ; un échec est retenté après retryAfterSec.
    Stmt s(db_, R"SQL(
        SELECT e.type, e.subject, COALESCE(MAX(e.question), e.subject), MAX(e.ts) AS last
        FROM escalations e
        LEFT JOIN training_log t ON t.subject = e.subject
        WHERE e.type IN ('WEB', 'REASON') AND e.subject <> '' AND e.subject <> 'capture d''écran'
          AND (t.subject IS NULL
               OR (t.status IN ('appris', 'partiel') AND t.ts < ?1)
               OR (t.status NOT IN ('appris', 'partiel') AND t.ts < ?2 AND t.attempts < 5))
        GROUP BY e.subject
        ORDER BY last DESC
        LIMIT ?3)SQL");
    if (!s) return out;
    s.Bind(1, Now() - relearnAfterSec).Bind(2, Now() - retryAfterSec).Bind(3, static_cast<std::int64_t>(limit));
    while (s.Step()) out.push_back({s.Text(0), s.Text(1), s.Text(2)});
    return out;
}

void MemoryDB::MarkTrained(std::string_view subject, std::string_view status) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO training_log(subject, ts, status) VALUES(?,?,?) "
                "ON CONFLICT(subject) DO UPDATE SET ts=excluded.ts, status=excluded.status, attempts=attempts+1");
    if (s) s.Bind(1, subject).Bind(2, Now()).Bind(3, status).Run();
}

void MemoryDB::AddLesson(std::string_view subject, std::string_view question, std::string_view fiche) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    {
        Stmt del(db_, "DELETE FROM lessons WHERE subject=?");
        if (del) del.Bind(1, subject).Run();
    }
    Stmt s(db_, "INSERT INTO lessons(subject, question, fiche, ts) VALUES(?,?,?,?)");
    if (s) s.Bind(1, subject).Bind(2, question).Bind(3, fiche).Bind(4, Now()).Run();
}

std::vector<Lesson> MemoryDB::SearchLessons(std::string_view query, int limit) {
    std::scoped_lock lock(mutex_);
    std::vector<Lesson> out;
    const std::string q = ToFtsQuery(query);
    if (!db_ || q.empty()) return out;
    Stmt s(db_, "SELECT subject, question, fiche FROM lessons WHERE lessons MATCH ? "
                "ORDER BY bm25(lessons, 4.0, 2.0, 1.0) LIMIT ?");
    if (!s) return out;
    s.Bind(1, q).Bind(2, static_cast<std::int64_t>(limit));
    while (s.Step()) out.push_back({s.Text(0), s.Text(1), s.Text(2)});
    return out;
}

int MemoryDB::CountLessons() {
    std::scoped_lock lock(mutex_);
    if (!db_) return 0;
    Stmt s(db_, "SELECT COUNT(*) FROM lessons");
    return s && s.Step() ? static_cast<int>(s.Int(0)) : 0;
}

bool MemoryDB::CoversSubject(std::string_view text, std::string_view subject) {
    auto lower = [](std::string_view v) {
        std::string o(v);
        for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    const std::string hay = lower(text);
    int terms = 0, found = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3) {
            ++terms;
            if (hay.find(cur) != std::string::npos) ++found;
        }
        cur.clear();
    };
    for (unsigned char c : lower(subject)) {
        if (c >= 0x80 || std::isalnum(c)) cur.push_back(static_cast<char>(c));
        else flush();
    }
    flush();
    return terms > 0 && found * 2 >= terms;
}

std::optional<bool> MemoryDB::GetPermission(std::string_view scope, std::string_view target) {
    std::scoped_lock lock(mutex_);
    if (!db_) return std::nullopt;
    Stmt s(db_, "SELECT allow FROM permissions WHERE scope=? AND target=?");
    if (s && s.Bind(1, scope).Bind(2, target).Step()) return s.Int(0) != 0;
    return std::nullopt;
}

void MemoryDB::SetPermission(std::string_view scope, std::string_view target, bool allow) {
    std::scoped_lock lock(mutex_);
    if (!db_) return;
    Stmt s(db_, "INSERT INTO permissions(scope, target, allow) VALUES(?,?,?) "
                "ON CONFLICT(scope, target) DO UPDATE SET allow=excluded.allow");
    if (s) s.Bind(1, scope).Bind(2, target).Bind(3, static_cast<std::int64_t>(allow ? 1 : 0)).Run();
}

}  // namespace companion
