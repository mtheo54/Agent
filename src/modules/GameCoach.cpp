#include "modules/GameCoach.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <format>

namespace companion {

std::string LolSnapshot::ToPromptSummary() const {
    auto items = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& i : v) s += (s.empty() ? "" : ", ") + i;
        return s.empty() ? std::string("aucun") : s;
    };
    const int min = static_cast<int>(gameTimeSec) / 60, sec = static_cast<int>(gameTimeSec) % 60;
    std::string out = std::format("Partie LoL en cours ({}, {}:{:02}). Toi : {} niv {}, {}/{}/{}, {:.0f} or, objets : {}.",
                                  gameMode, min, sec, me.champion, me.level, me.kills, me.deaths, me.assists, gold,
                                  items(me.items));
    out += " Ennemis :";
    for (const auto& e : enemies)
        out += std::format(" {} ({}/{}/{}, {}) ;", e.champion, e.kills, e.deaths, e.assists, items(e.items));
    return out;
}

std::optional<LolSnapshot> GameCoach::Parse(const std::string& body) {
    try {
        const auto j = nlohmann::json::parse(body);
        LolSnapshot s;
        const auto& active = j.at("activePlayer");
        s.gold = active.value("currentGold", 0.0);
        // Identité du joueur actif : riotId (récent) ou summonerName (ancien format).
        const std::string myId = active.value("riotId", active.value("summonerName", std::string{}));
        s.gameMode = j.value("gameData", nlohmann::json::object()).value("gameMode", std::string{"?"});
        s.gameTimeSec = j.value("gameData", nlohmann::json::object()).value("gameTime", 0.0);

        std::vector<LolPlayer> players;
        int meIndex = -1;
        for (const auto& p : j.at("allPlayers")) {
            LolPlayer lp;
            lp.champion = p.value("championName", std::string{"?"});
            lp.team = p.value("team", std::string{});
            lp.level = p.value("level", 0);
            const auto& sc = p.value("scores", nlohmann::json::object());
            lp.kills = sc.value("kills", 0);
            lp.deaths = sc.value("deaths", 0);
            lp.assists = sc.value("assists", 0);
            lp.creeps = sc.value("creepScore", 0);
            for (const auto& it : p.value("items", nlohmann::json::array()))
                lp.items.push_back(it.value("displayName", std::string{}));
            const std::string id = p.value("riotId", p.value("summonerName", std::string{}));
            if (!myId.empty() && id == myId) meIndex = static_cast<int>(players.size());
            players.push_back(std::move(lp));
        }
        if (meIndex < 0) return std::nullopt;
        s.me = players[static_cast<size_t>(meIndex)];
        for (size_t i = 0; i < players.size(); ++i) {
            if (static_cast<int>(i) == meIndex) continue;
            (players[i].team == s.me.team ? s.allies : s.enemies).push_back(players[i]);
        }
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void GameCoach::Start() {
    if (!thread_.joinable()) thread_ = std::jthread([this](std::stop_token st) { Run(st); });
}

void GameCoach::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

std::optional<LolSnapshot> GameCoach::Latest() const {
    std::scoped_lock lock(mutex_);
    if (!latest_ || GetTickCount64() - latestTick_ > 15000) return std::nullopt;
    return latest_;
}

void GameCoach::Run(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.GameCoach");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::mutex m;
    std::condition_variable_any cv;
    while (!st.stop_requested()) {
        HttpRequest req;
        req.url = "https://127.0.0.1:2999/liveclientdata/allgamedata";
        req.allowSelfSignedLocalhost = true;
        req.timeoutMs = 1500;
        const auto res = http_.Send(req);
        auto snap = res.Ok() ? Parse(res.body) : std::nullopt;
        {
            std::scoped_lock lock(mutex_);
            if (snap) { latest_ = std::move(snap); latestTick_ = GetTickCount64(); }
        }
        // 4 s en partie (rapport : 3 à 5 s) ; 15 s quand aucune partie ne répond.
        std::unique_lock lock(m);
        cv.wait_for(lock, st, std::chrono::seconds(res.Ok() ? 4 : 15), [] { return false; });
    }
}

}  // namespace companion
