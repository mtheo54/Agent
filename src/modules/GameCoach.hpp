#pragma once
// GameCoach (partie League of Legends) : lecture passive de l'API officielle locale de Riot
// (Live Client Data, https://127.0.0.1:2999/liveclientdata/allgamedata) toutes les 4 s.
// Aucune lecture mémoire, aucune injection : 100 % compatible Vanguard.
// La capture d'écran DXGI pour les jeux solo arrive à l'étape 3.

#include "network/HttpClient.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

struct LolPlayer {
    std::string champion;
    std::string team;  // "ORDER" | "CHAOS"
    int level = 0, kills = 0, deaths = 0, assists = 0, creeps = 0;
    std::vector<std::string> items;
};

struct LolSnapshot {
    std::string gameMode;
    double gameTimeSec = 0;
    double gold = 0;
    LolPlayer me;
    std::vector<LolPlayer> allies, enemies;

    // Résumé compact (< ~100 tokens) injecté dans le contexte du modèle local.
    [[nodiscard]] std::string ToPromptSummary() const;
};

class GameCoach {
public:
    explicit GameCoach(HttpClient& http) : http_(http) {}
    ~GameCoach() { Stop(); }

    void Start();
    void Stop();

    // Dernier état connu si une partie est en cours (données de moins de 15 s).
    [[nodiscard]] std::optional<LolSnapshot> Latest() const;

    static std::optional<LolSnapshot> Parse(const std::string& json);

private:
    void Run(std::stop_token st);

    HttpClient& http_;
    std::jthread thread_;
    mutable std::mutex mutex_;
    std::optional<LolSnapshot> latest_;
    unsigned long long latestTick_ = 0;
};

}  // namespace companion
