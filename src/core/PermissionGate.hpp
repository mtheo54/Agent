#pragma once
// PermissionGate : aucune sortie réseau (web ou Claude) ni action système sensible sans ton accord.
//
// La question est posée à voix haute ; ta réponse vocale suivante la tranche :
//   "oui", "ok", "vas-y"      -> autorisé cette fois
//   "toujours"                -> autorisé et mémorisé pour cette cible (ex. un domaine)
//   "non"                     -> refusé cette fois
//   "jamais"                  -> refusé et mémorisé
// Sans réponse avant le délai : refusé.

#include "core/MemoryDB.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace companion {

enum class PermissionScope { Web, Cloud, System };

class PermissionGate {
public:
    using AskFn = std::function<void(const std::string& question)>;

    PermissionGate(MemoryDB& db, AskFn ask) : db_(db), ask_(std::move(ask)) {}

    // Bloquant : à appeler depuis un thread de travail (jamais le thread UI ni audio).
    bool Request(PermissionScope scope, std::string_view target, const std::string& question,
                 std::chrono::seconds timeout, std::stop_token stop);

    // À appeler avec chaque transcription. Retourne true si elle répondait à une question en attente.
    bool OnUserReply(std::string_view transcript);

    [[nodiscard]] bool Pending() const;

    static const char* ToString(PermissionScope s) noexcept;

private:
    enum class Reply { None, Yes, Always, No, Never };
    static Reply Parse(std::string_view transcript);

    MemoryDB& db_;
    AskFn ask_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    bool pending_ = false;
    Reply reply_ = Reply::None;
};

}  // namespace companion
