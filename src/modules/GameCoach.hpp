#pragma once
// GameCoach (rapport §4.D)
//  - League of Legends : lecture passive de l'API officielle locale de Riot
//    (Live Client Data, https://127.0.0.1:2999/liveclientdata/allgamedata) toutes les 4 s.
//  - Jeux solo : capture PONCTUELLE de l'écran (DXGI Desktop Duplication), uniquement sur
//    demande vocale, réduite en 720p (RGB pour le modèle de vision local, JPEG pour Claude).
// Aucune lecture mémoire, aucune injection : 100 % compatible Vanguard / VAC.

#include "network/HttpClient.hpp"

#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <cstdint>
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
    std::vector<int> itemIds;
};

// Données statiques officielles de Riot (Data Dragon), en français, mises en cache par patch.
struct LolItem {
    int id = 0;
    std::string name;
    int total = 0;  // prix total en pièces d'or
    int depth = 1;
    bool purchasable = false;
    bool antiHeal = false;  // applique Hémorragie (Grievous Wounds)
    std::vector<std::string> tags, maps;
    std::vector<int> from, into;
};

struct LolChampion {
    std::string id, name;
    std::vector<std::string> tags;  // Mage, Assassin, Marksman, Fighter, Tank, Support
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

struct ScreenShot {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;   // RGBRGB…, pour llama.cpp (mtmd)
    std::vector<std::uint8_t> jpeg;  // pour Claude Vision
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

    // --- Conseiller de build local (aucun site tiers, aucun token) ---
    // Charge le cache Data Dragon s'il est récent (sans réseau).
    bool LoadCachedStaticData(const std::filesystem::path& cacheDir, int maxAgeDays = 7);
    // Télécharge la dernière version (≈ 1,5 Mo, une fois par patch). À appeler après permission.
    bool DownloadStaticData(const std::filesystem::path& cacheDir);
    [[nodiscard]] bool HasStaticData() const;
    // Analyse factuelle : profil de dégâts ennemi, soins, objets adaptés, achats possibles avec ton or.
    [[nodiscard]] std::string BuildAdvice(const LolSnapshot& snap) const;

    // Capture une image de l'écran qui contient la fenêtre au premier plan.
    // COM doit être initialisé sur le thread appelant. Fonctionne en plein écran fenêtré
    // (borderless) ; un plein écran exclusif peut refuser la duplication.
    static std::optional<ScreenShot> CaptureScreen(int maxHeight = 720);

private:
    void Run(std::stop_token st);

    HttpClient& http_;
    std::jthread thread_;
    mutable std::mutex mutex_;
    std::optional<LolSnapshot> latest_;
    unsigned long long latestTick_ = 0;

    bool LoadStaticFiles(const std::filesystem::path& versionDir, const std::string& version);
    const LolChampion* FindChampion(const std::string& name) const;
    mutable std::mutex dataMutex_;
    std::string ddVersion_;
    std::unordered_map<int, LolItem> items_;
    std::vector<LolChampion> champions_;
};

}  // namespace companion
