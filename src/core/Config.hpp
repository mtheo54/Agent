#pragma once
// Configuration du compagnon. Valeurs par défaut ici ; surcharge optionnelle par
// companion.json à côté de l'exécutable (voir LoadConfig).

#include <filesystem>
#include <map>
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
    // Bruitage (soundboard) joué à des moments précis ; "" = aucun. Choisis par l'utilisateur.
    std::map<std::string, std::string> soundEvents{
        {"greeting", "anime wow"},        // démarrage
        {"clipping", "fahhh"},            // le mix clippe (studio)
        {"rageBaitBite", "https://www.myinstants.com/fr/instant/cyril-hanouna-92711/"},  // il avoue son rage bait en rigolant
        {"gameDeath", "oh no"},           // mort / défaite en jeu
        {"gameWin", "c'est vrai"},        // kill, multi-kill, victoire
        {"error", "windows error"},       // bug, rien trouvé, réseau, crash
        {"blunder", ""},                  // grosse bêtise : pas de son
        {"trainingDone", "suspense"},     // fin d'entraînement
        {"studioStart", ""},              // FL Studio lancé : pas de son
        {"comeback", "https://www.myinstants.com/fr/instant/salut-cest-frank-leboeuf-complet-97159/"},  // retour après absence
        {"goodbye", "windows shutdown"},  // « bonne nuit », « à plus »
        {"gameStart", "emotional damage"},  // début d'une partie de LoL
        // Émotions (voix + mots)
        {"rageInGame", "fahhh"},          // tu rages EN JEU (jamais hors jeu)
        {"hype", "https://www.myinstants.com/fr/instant/hava-nagila-30752/"},  // tu es hypé
        {"userLaugh", "https://www.myinstants.com/fr/instant/cyril-hanouna-92711/"},  // tu rigoles
        {"insulted", "sad violin"},       // tu l'insultes : il joue la victime
        {"sad", "soft piano"},            // tu as l'air triste : son doux, puis il te console
        {"tiredNight", "yawn"},           // fatigué après 1 h du matin
        {"userSurprise", ""},             // tu es choqué : pas de son
    };
    float micEffect = 0.7f;         // effet « vrai micro » sur sa voix (0 … 1)
    float pitchLife = 0.5f;         // hauteur vivante : dérive et virages (0 … 1)
    float voiceQuirks = 0.5f;       // tics humains et bugs de robot (rire spontané, toux, syllabe qui accroche)
    bool neuralVoice = true;        // voix neuronale expressive (Qwen3-TTS sur GPU) ; sinon Piper
    bool neuralVoiceInGame = false; // en jeu : Piper (CPU) pour ne pas toucher au GPU, sauf si activé
    std::string neuralVoiceDevice;  // "Vulkan0", "CPU"… vide = automatique
    int tone = 2;                   // 0 poli, 1 pote, 2 sans filtre (argot, vannes, gros mots)
    float speechRate = 1.f;         // 0,85 rapide … 1,2 lent
    float voiceTempo = 1.18f;       // débit de la voix (1 = brut ; mesuré 4-5 syll/s brut, ~6 avec 1,18)
    bool backchannels = true;       // relances orales instantanées (« Hmm… »)
    bool bargeIn = true;            // on peut lui couper la parole (écoute continue)
    bool emotionDetection = true;   // émotion perçue dans la voix
    bool voiceActivity = false;     // écoute continue (VAD) au lieu du push-to-talk

    // --- Niveau 1 : cerveau local (llama.cpp) ---
    // Qwen3.5 9B (Apache 2.0, ~5,7 Go en Q4_K_M) : bien plus naturel que les 3B pour parler en pote.
    // « auto » : choisi selon le PC au démarrage (ModelPlanner : moitié de la VRAM au plus hors jeu,
    // un quart en jeu). Un chemin = modèle imposé.
    std::filesystem::path llmModel = L"auto";
    // Projecteur de vision (mmproj) du MÊME modèle, ex. Qwen2.5-VL-3B. Vide = pas de vision locale
    // (une capture d'écran part alors vers Claude Vision, avec ta permission).
    std::filesystem::path llmMmproj;
    int llmContext = 4096;
    int llmGpuLayers = 99;       // hors jeu : tout sur la RX 9060 XT (Vulkan)
    int llmThreads = 4;
    int llmThreadsGaming = 2;    // en jeu : 2 threads (sur le cœur isolé)
    // En jeu, le cerveau ne dépasse pas ce débit : la voix en lit ~5 par seconde, inutile d'aller plus vite.
    // Il travaille par courtes salves espacées au lieu d'occuper la carte ou la mémoire en continu (0 = libre).
    int llmGamingTokensPerSecond = 12;
    // Presets de jeu : "auto" (mesuré), "carte", "carte-leger", "processeur", "processeur-leger".
    std::string gamingPreset = "auto";                  // pour tous les jeux
    std::map<std::string, std::string> gamePresets;     // par jeu : exécutable en minuscules -> preset (prioritaire)
    // Moniteur de perfs en jeu (FPS, 1 % low, impact d'Omnis ; rapports dans %LOCALAPPDATA%\CompanionAI\perf).
    bool perfMonitor = true;
    bool perfAutoAdjust = true;      // ça bouffe trop -> preset plus léger pour ce jeu (retenu)
    double perfMaxFpsDropPct = 5.0;  // « trop » = FPS moyen en baisse de plus de 5 %…
    double perfMaxLowDropPct = 10.0; // …ou 1 % low (saccades) en baisse de plus de 10 %

    // Prise en main du clavier et de la souris à la demande (« prends la main », AutoCAD, FL Studio…).
    bool pilotEnabled = true;
    // Discord, par ton micro : câble audio virtuel (VB-Cable). Dans Discord : périphérique d'entrée « CABLE Output ».
    bool discordBridge = true;
    std::string discordCableDevice = "CABLE Input";  // sortie du câble vue par Windows
    std::string discordMicDevice;                    // vide = micro par défaut
    float callBaitChance = 0.25f;  // dans un call, chance qu'il réagisse (vanne) sans qu'on l'appelle
    // Bruitages de rage bait lâchés au hasard dans le call (soundboard myinstants, téléchargés à l'arrivée).
    std::vector<std::string> callBaitSounds{"fahhh", "pizza nizar", "emotional damage", "bruh", "vine boom"};
    std::string discordMyName;     // ton pseudo Discord : ton propre cercle vert n'est pas pris pour un pote
    bool earrape = true;           // de temps en temps, un bruitage saturé (1,5 s max, volume plafonné)
    int substituteMaxMinutes = 60; // « prends ma place » : 60 min au plus (10 à 60)
    // En jeu : petit modèle sur CPU, chargé seulement si tu lui parles (VRAM laissée au jeu). Absent :
    // le modèle principal sur CPU (plus lent).
    std::filesystem::path llmGamingModel = L"auto";
    bool llmGamingAuto = true;   // (non enregistré) en jeu, cerveau re-choisi selon la VRAM libre du moment
    int avatarInGame = 1;        // en jeu : 0 caché, 1 seulement quand il parle / t'écoute, 2 toujours
    bool gameIsolation = true;   // en jeu : confiné au dernier cœur, priorité et mémoire basses
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
