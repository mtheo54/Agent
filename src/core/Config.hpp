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

// Application lançable à la voix ("lance Spotify"). target : chemin, commande ou URI.
// Les variables d'environnement (%LOCALAPPDATA%…) sont développées.
struct AppShortcut {
    std::string name;                  // en minuscules, cherché dans la phrase
    std::vector<std::string> aliases;  // mots entiers, en minuscules
    std::string target;
    std::string args;
};

struct CompanionConfig {
    std::string name = "Omnis";  // nom du personnage

    // --- Personnage (fenêtre de configuration) ---
    float avatarScale = 1.f;        // 0,6 … 1,6
    float avatarOpacity = 1.f;      // 0,3 … 1
    int avatarCorner = 3;           // 0 haut-gauche, 1 haut-droite, 2 bas-gauche, 3 bas-droite
    float expressiveness = 0.8f;    // fréquence des gestes et réactions (0 … 1)

    // --- Voix ---
    float fxIntensity = 0.15f;      // grain 8-bit de base (0 … 1) ; discret par défaut, la voix reste naturelle
    float voiceQuirks = 0.5f;       // tics humains et bugs de robot (rire spontané, toux, syllabe qui accroche)
    bool neuralVoice = true;        // voix neuronale expressive (Qwen3-TTS sur GPU) ; sinon Piper
    bool neuralVoiceInGame = false; // en jeu : Piper (CPU) pour ne pas toucher au GPU, sauf si activé
    std::string neuralVoiceDevice;  // "Vulkan0", "CPU"… vide = automatique
    int tone = 2;                   // 0 poli, 1 pote, 2 sans filtre (argot, vannes, gros mots)
    float speechRate = 1.f;         // 0,85 rapide … 1,2 lent
    bool backchannels = true;       // relances orales instantanées (« Hmm… »)
    bool bargeIn = true;            // on peut lui couper la parole (écoute continue)
    bool emotionDetection = true;   // émotion perçue dans la voix
    bool voiceActivity = false;     // écoute continue (VAD) au lieu du push-to-talk

    // --- Niveau 1 : cerveau local (llama.cpp) ---
    std::filesystem::path llmModel = L"assets/models/qwen2.5-3b-instruct-q4_k_m.gguf";
    // Projecteur de vision (mmproj) du MÊME modèle, ex. Qwen2.5-VL-3B. Vide = pas de vision locale
    // (une capture d'écran part alors vers Claude Vision, avec ta permission).
    std::filesystem::path llmMmproj;
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
    int knowledgeMaxAgeDays = 14;
    std::string userAgent = "CompanionAI/0.3 (desktop assistant)";  // au-delà, une info web en cache est re-vérifiée (patchs LoL)

    // --- Auto-entraînement (rapport §4.E) ---
    bool autoTraining = true;               // après 5 min d'inactivité, charge < 25 %
    int autoTrainingEveryMinutes = 120;     // au plus une session automatique par période
    int trainerMemoryLimitMb = 6144;        // plafond RAM du processus d'entraînement (Job Object)
    std::filesystem::path loraAdapter = L"assets/models/lora.gguf";  // appliqué s'il existe

    // --- Studio (héritage OMNIS) ---
    std::string studioProcess = "FL64.exe";  // capture audio ciblée sur ce processus
    bool studioAlerts = true;                // niveau 0 : "ça clippe", problème de phase

    // --- Contrôle du PC ---
    std::vector<AppShortcut> apps{
        {"fl studio", {"fl"}, "FL64.exe", ""},  // mets le chemin complet si FL n'est pas dans App Paths
        {"spotify", {}, "spotify:", ""},
        {"discord", {}, "discord://", ""},
        {"steam", {}, "steam://open/main", ""},
        {"league of legends", {"lol", "league"}, "C:\\Riot Games\\Riot Client\\RiotClientServices.exe",
         "--launch-product=league_of_legends --launch-patchline=live"},
        {"cyberpunk", {}, "steam://rungameid/1091500", ""},
    };

    // --- Niveau 2 : Claude (dernier recours) ---
    bool cloudEnabled = true;
    bool cloudAsksPermission = true;  // chaque appel cloud demande ton accord (sauf "toujours")
    std::string claudeModel = "claude-opus-5-5";
    std::string claudeEffort = "medium";
};

// Charge companion.json s'il existe (clés optionnelles, mêmes noms que ci-dessus).
CompanionConfig LoadConfig(const std::filesystem::path& jsonFile);
// Enregistre les réglages modifiables dans companion.json (les autres clés du fichier sont conservées).
bool SaveConfig(const CompanionConfig& cfg, const std::filesystem::path& jsonFile);

}  // namespace companion
