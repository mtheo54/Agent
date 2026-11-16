#pragma once
// Configuration du compagnon. Valeurs par défaut ici ; surcharge optionnelle par
// companion.json à côté de l'exécutable (voir LoadConfig).

#include <filesystem>
#include <string>
#include <vector>

namespace companion {

// Source de connaissance web locale : un wiki MediaWiki (api.php). Liste blanche stricte.
struct WikiSource {
    std::string host;                   // ex. "leagueoflegends.fandom.com"
    std::string apiPath;                // ex. "/api.php" (Fandom) ou "/w/api.php" (Wikipédia)
    std::vector<std::string> keywords;  // mots qui orientent vers cette source
};

struct CompanionConfig {
    std::string name = "Omnis";  // nom du personnage

    // --- Niveau 1 : cerveau local (llama.cpp) ---
    std::filesystem::path llmModel = L"assets/models/qwen2.5-3b-instruct-q4_k_m.gguf";
    int llmContext = 4096;
    int llmGpuLayers = 99;       // hors jeu : tout sur la RX 9060 XT (Vulkan)
    int llmThreads = 4;
    int llmThreadsGaming = 2;    // en jeu : CPU uniquement, 2 threads, 0 Mo de VRAM
    int llmUnloadAfterMinutes = 10;  // retour sous 80 Mo de RAM en veille (rapport §2)

    // --- Recherche web locale (0 token Claude) ---
    std::vector<WikiSource> wikis{
        {"leagueoflegends.fandom.com", "/api.php",
         {"lol", "league", "champion", "build", "stuff", "item", "objet", "rune", "matchup", "jungle", "lane"}},
        {"cyberpunk.fandom.com", "/api.php",
         {"cyberpunk", "night city", "quête", "quete", "mission", "johnny", "v ", "edgerunner"}},
        {"fr.wikipedia.org", "/w/api.php", {}},  // repli général
    };
    int knowledgeMaxAgeDays = 14;  // au-delà, une info web en cache est re-vérifiée (patchs LoL)

    // --- Niveau 2 : Claude (dernier recours) ---
    bool cloudEnabled = true;
    bool cloudAsksPermission = true;  // chaque appel cloud demande ton accord (sauf "toujours")
    std::string claudeModel = "claude-opus-5-5";
    std::string claudeEffort = "medium";
};

// Charge companion.json s'il existe (clés optionnelles, mêmes noms que ci-dessus).
CompanionConfig LoadConfig(const std::filesystem::path& jsonFile);

}  // namespace companion
