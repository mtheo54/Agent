#include "modules/GameCoach.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
            for (const auto& it : p.value("items", nlohmann::json::array())) {
                lp.items.push_back(it.value("displayName", std::string{}));
                lp.itemIds.push_back(it.value("itemID", 0));
            }
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

namespace companion {
namespace {

constexpr const char* kDdragon = "https://ddragon.leagueoflegends.com";

std::string Normalize(std::string s) {  // "Kai'Sa" == "kaisa", "Nunu et Willump" ~ "nunuetwillump"
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c) || c >= 0x80) o.push_back(static_cast<char>(std::tolower(c)));
    return o;
}

bool Has(const std::vector<std::string>& v, const char* x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::optional<std::string> ReadFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

bool GameCoach::HasStaticData() const {
    std::scoped_lock lock(dataMutex_);
    return !items_.empty() && !champions_.empty();
}

bool GameCoach::LoadStaticFiles(const std::filesystem::path& dir, const std::string& version) {
    const auto itemJson = ReadFile(dir / L"item.json");
    const auto champJson = ReadFile(dir / L"champion.json");
    if (!itemJson || !champJson) return false;
    try {
        std::unordered_map<int, LolItem> items;
        // Documents JSON nommés : itérer sur une référence dans un temporaire serait un use-after-free.
        const auto itemDoc = nlohmann::json::parse(*itemJson);
        const auto champDoc = nlohmann::json::parse(*champJson);
        for (const auto& [key, v] : itemDoc.at("data").items()) {
            LolItem it;
            it.id = std::stoi(key);
            it.name = v.value("name", std::string{});
            const auto& gold = v.value("gold", nlohmann::json::object());
            it.total = gold.value("total", 0);
            it.purchasable = gold.value("purchasable", false) && v.value("inStore", true) &&
                             !v.contains("requiredChampion") && !v.contains("requiredAlly");
            it.depth = v.value("depth", 1);
            it.tags = v.value("tags", std::vector<std::string>{});
            if (const auto maps = v.find("maps"); maps != v.end())
                for (const auto& [m, on] : maps->items())
                    if (on.is_boolean() && on.get<bool>()) it.maps.push_back(m);
            for (const auto& x : v.value("from", std::vector<std::string>{})) it.from.push_back(std::stoi(x));
            for (const auto& x : v.value("into", std::vector<std::string>{})) it.into.push_back(std::stoi(x));
            const std::string desc = v.value("description", std::string{});
            it.antiHeal = desc.find("Hémorragie") != std::string::npos || desc.find("Grievous") != std::string::npos;
            items.emplace(it.id, std::move(it));
        }
        std::vector<LolChampion> champs;
        for (const auto& [key, v] : champDoc.at("data").items())
            champs.push_back({v.value("id", key), v.value("name", key), v.value("tags", std::vector<std::string>{})});
        std::scoped_lock lock(dataMutex_);
        items_ = std::move(items);
        champions_ = std::move(champs);
        ddVersion_ = version;
    } catch (const std::exception& e) {
        log::Info("GameCoach: données Riot illisibles ({})", e.what());
        return false;
    }
    log::Info("GameCoach: données Riot {} chargées", version);
    return true;
}

bool GameCoach::LoadCachedStaticData(const std::filesystem::path& cacheDir, int maxAgeDays) {
    std::error_code ec;
    std::filesystem::path newest;
    auto newestTime = std::filesystem::file_time_type::min();  // l'époque de l'horloge varie selon la STL
    for (const auto& e : std::filesystem::directory_iterator(cacheDir / L"ddragon", ec)) {
        if (!e.is_directory()) continue;
        const auto t = std::filesystem::last_write_time(e.path() / L"item.json", ec);
        if (!ec && t > newestTime) { newestTime = t; newest = e.path(); }
    }
    if (newest.empty()) return false;
    if (std::filesystem::file_time_type::clock::now() - newestTime > std::chrono::hours(24 * maxAgeDays)) return false;
    return LoadStaticFiles(newest, newest.filename().string());
}

bool GameCoach::DownloadStaticData(const std::filesystem::path& cacheDir) {
    const auto versions = http_.Get(std::string(kDdragon) + "/api/versions.json", 8000);
    std::string version;
    try {
        if (versions.Ok()) version = nlohmann::json::parse(versions.body).at(0).get<std::string>();
    } catch (const std::exception&) {}
    if (version.empty()) return LoadCachedStaticData(cacheDir, 3650);  // hors ligne : n'importe quel cache

    const auto dir = cacheDir / L"ddragon" / std::filesystem::path(version);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const char* file : {"item.json", "champion.json"}) {
        const auto res = http_.Get(std::string(kDdragon) + "/cdn/" + version + "/data/fr_FR/" + file, 20000);
        if (!res.Ok()) {
            log::Info("GameCoach: téléchargement de {} échoué ({})", file, res.status);
            return false;
        }
        std::ofstream(dir / std::filesystem::path(file), std::ios::binary) << res.body;
    }
    return LoadStaticFiles(dir, version);
}

const LolChampion* GameCoach::FindChampion(const std::string& name) const {
    const std::string n = Normalize(name);
    for (const auto& c : champions_)
        if (Normalize(c.name) == n || Normalize(c.id) == n) return &c;
    return nullptr;
}

std::string GameCoach::BuildAdvice(const LolSnapshot& snap) const {
    std::scoped_lock lock(dataMutex_);
    if (items_.empty()) return {};
    const std::string map = snap.gameMode == "ARAM" ? "12" : "11";
    auto onMap = [&](const LolItem& it) { return std::find(it.maps.begin(), it.maps.end(), map) != it.maps.end(); };

    // 1) Profil de dégâts ennemi : classe du champion + objets construits, pondérés par l'impact (K+A).
    double ap = 0, ad = 0;
    bool healing = false;
    std::vector<std::string> threats;
    for (const auto& e : snap.enemies) {
        const double w = 1.0 + 0.25 * (e.kills + e.assists);
        if (const LolChampion* c = FindChampion(e.champion); c && !c->tags.empty()) {
            const std::string& t = c->tags[0];
            if (t == "Mage") ap += w;
            else if (t == "Marksman" || t == "Fighter" || t == "Assassin") ad += w;
            else { ap += 0.4 * w; ad += 0.4 * w; }  // Tank / Support : menace moindre
        }
        for (int id : e.itemIds) {
            const auto it = items_.find(id);
            if (it == items_.end()) continue;
            if (Has(it->second.tags, "SpellDamage")) ap += 0.3 * w;
            if (Has(it->second.tags, "Damage") || Has(it->second.tags, "CriticalStrike")) ad += 0.3 * w;
            if (Has(it->second.tags, "LifeSteal") || Has(it->second.tags, "SpellVamp")) healing = true;
        }
        if (e.kills >= 4) threats.push_back(std::format("{} ({}/{}/{})", e.champion, e.kills, e.deaths, e.assists));
    }
    const double apShare = ap + ad > 0 ? ap / (ap + ad) : 0.5;

    // 2) Objets adaptés à ta classe.
    const LolChampion* me = FindChampion(snap.me.champion);
    const std::string myClass = me && !me->tags.empty() ? me->tags[0] : "Fighter";
    std::vector<const char*> wanted;
    if (myClass == "Mage") wanted = {"SpellDamage", "MagicPenetration"};
    else if (myClass == "Marksman") wanted = {"CriticalStrike", "AttackSpeed", "Damage"};
    else if (myClass == "Assassin") wanted = {"Damage", "ArmorPenetration"};
    else if (myClass == "Tank") wanted = {"Health", "Armor", "SpellBlock"};
    else if (myClass == "Support") wanted = {"ManaRegen", "Aura", "Health"};
    else wanted = {"Damage", "Health"};
    const std::set<int> owned(snap.me.itemIds.begin(), snap.me.itemIds.end());
    const bool hasBoots = std::any_of(owned.begin(), owned.end(), [&](int id) {
        const auto it = items_.find(id);
        return it != items_.end() && Has(it->second.tags, "Boots");
    });

    std::multimap<double, const LolItem*, std::greater<>> ranked;
    for (const auto& [id, it] : items_) {
        if (!it.purchasable || !onMap(it) || owned.count(id) || !it.into.empty() || it.depth < 3 ||
            Has(it.tags, "Consumable") || Has(it.tags, "Boots") || it.total < 2000)
            continue;
        double score = 0;
        for (const char* t : wanted) score += Has(it.tags, t) ? 2.0 : 0.0;
        if (score == 0) continue;  // hors de ta classe
        if (apShare > 0.6 && Has(it.tags, "SpellBlock")) score += 2.0;
        if (apShare < 0.4 && Has(it.tags, "Armor")) score += 2.0;
        if (healing && it.antiHeal) score += 3.0;
        ranked.emplace(score, &it);
    }

    // 3) Achats possibles maintenant : un objet conseillé, ou ses composants.
    std::vector<const LolItem*> picks;
    for (const auto& [score, it] : ranked) {
        if (picks.size() == 3) break;
        picks.push_back(it);
    }
    std::vector<const LolItem*> buyNow;
    for (const LolItem* p : picks) {
        if (p->total <= snap.gold) { buyNow.push_back(p); continue; }
        for (int c : p->from)
            if (const auto it = items_.find(c); it != items_.end() && it->second.total <= snap.gold && !owned.count(c))
                buyNow.push_back(&it->second);
    }
    std::sort(buyNow.begin(), buyNow.end(), [](auto a, auto b) { return a->total > b->total; });
    buyNow.erase(std::unique(buyNow.begin(), buyNow.end()), buyNow.end());

    std::string out = std::format("Analyse d'objets (données officielles Riot, patch {}) pour {} ({}) avec {:.0f} or. ",
                                  ddVersion_, snap.me.champion, myClass, snap.gold);
    out += std::format("En face : environ {:.0f} % de dégâts magiques", apShare * 100);
    out += healing ? ", soins ennemis détectés (anti-soin utile). " : ". ";
    if (!threats.empty()) {
        out += "Menaces :";
        for (const auto& t : threats) out += " " + t;
        out += ". ";
    }
    if (!picks.empty()) {
        out += "Objets adaptés :";
        for (const LolItem* p : picks) out += std::format(" {} ({} or{}) ;", p->name, p->total, p->antiHeal ? ", anti-soin" : "");
        out += " ";
    }
    if (!hasBoots) {
        const char* bootTag = myClass == "Mage" ? "MagicPenetration" : apShare > 0.6 ? "SpellBlock" : apShare < 0.4 ? "Armor" : "AttackSpeed";
        for (const auto& [id, it] : items_)
            if (it.purchasable && onMap(it) && Has(it.tags, "Boots") && it.depth == 2 && Has(it.tags, bootTag)) {
                out += std::format("Bottes conseillées : {} ({} or). ", it.name, it.total);
                break;
            }
    }
    if (!buyNow.empty()) {
        out += "Achetable maintenant :";
        for (size_t i = 0; i < buyNow.size() && i < 3; ++i) out += std::format(" {} ({} or) ;", buyNow[i]->name, buyNow[i]->total);
    }
    return out;
}

}  // namespace companion
