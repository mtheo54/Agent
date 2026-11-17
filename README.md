# CompanionAI (héritage OMNIS)

- **Étape 1** : `ResourceGovernor`, `VoiceEngine` + `VoiceFX`, `OverlayWindow` + `ProceduralAvatar` + `MatCapShader.hlsl`.
- **Étape 2** : cerveau « local d'abord » — `AgentOrchestrator`, `LocalBrain` (llama.cpp), `MemoryDB` (SQLite),
  `PermissionGate`, `WebResearcher` (wikis en liste blanche), `ClaudeClient` (dernier recours), `GameCoach` (LoL).
- **Étape 3** : studio (`WasapiLoopback` + `OmnisAnalyzer`), vision (capture DXGI, analysée en local puis
  Claude Vision en secours), conseiller de build LoL local, `SystemControl` (volume, musique, applis).
- **Étape 4** : `SelfTrainer` — auto-entraînement pendant l'inactivité du PC, dans un processus isolé ;
  avatar « bonhomme chromé » gris, sans visage, qui salue.

## Auto-entraînement (étape 4)

Après 5 minutes sans clavier ni souris (charge CPU/GPU < 25 %), ou quand tu dis « entraîne-toi » :

1. il relit les sujets qui ont provoqué un `[ESCALATE]` ;
2. il collecte des sources sur les wikis de la liste blanche — **uniquement ceux pour lesquels tu as
   répondu « toujours »**, puisque personne n'est là pour valider ;
3. le modèle local rédige une **fiche** de faits clés, puis rejoue ta question d'origine avec elle : s'il
   répond sans escalader, le sujet est appris et la prochaine fois tout reste en local ;
4. à ton retour : « Pendant ta pause, j'ai appris 2 sujets : … ».

La session tourne dans un processus enfant (`CompanionAI.exe --trainer`) en priorité IDLE, dans un Job
Object avec plafond de RAM. Toucher le clavier ou la souris, lancer un jeu ou FL Studio, ou dire « pause »
le **tue instantanément** (réseau, CPU et RAM libérés) ; chaque sujet terminé est déjà enregistré.

**Pourquoi pas de LoRA entraîné sur place** : l'entraînement de llama.cpp est expérimental, en FP32 seulement,
et demande ~24 Go pour un modèle 1B — un 3B ne tient pas dans 32 Go. Les exemples validés sont exportés dans
`%LOCALAPPDATA%\CompanionAI\training\dataset.jsonl` (format chat) : si tu entraînes un LoRA ailleurs,
dépose `lora.gguf` dans `assets/models/`, il est appliqué automatiquement.

## Cascade

| Niveau | Où | Quoi |
|---|---|---|
| 0 | C++ | « pause », « entraîne-toi », « retiens que… », question de build pendant une partie LoL |
| 1 | llama.cpp (3B) | conversation ; émet `[ESCALATE \| type: … \| sujet: …]` s'il ne sait pas |
| 1b | web **local** | cache SQLite d'abord, sinon wiki en liste blanche (avec ta permission), résumé par le modèle local — 0 token |
| 2 | Claude | dernier recours (raisonnement complexe, ou le web local n'a rien donné), après ta permission |

Ce qui a été appris sur le web ou auprès de Claude est stocké : la même question est ensuite traitée sans réseau.

## Ce que tu peux lui dire

| Phrase | Ce qui se passe |
|---|---|
| « Ton avis sur mon mix ? » | Écoute FL Studio (ou le son du PC) 3 s, mesures OMNIS (LUFS, crête, clipping, phase, spectre), avis du modèle local |
| *(automatique dans FL Studio)* | « Attention, ça clippe », « problème de phase » (niveau 0, limité dans le temps) |
| « Je build quoi ? » (en partie LoL) | Profil de dégâts ennemi, anti-soin, objets de ta classe, achats possibles avec ton or — données officielles Riot, calcul local |
| « Regarde mon écran, je bloque sur cette quête » | Capture 720p → modèle de vision local (peut enchaîner sur le wiki Cyberpunk) → sinon Claude Vision, avec ta permission |
| « Monte le son », « volume à 30 », « coupe le son » | API audio Windows (aucune touche simulée) |
| « Musique suivante », « mets la musique en pause » | Touches multimédia — **refusées pendant un jeu** (anti-cheat) |
| « Lance Spotify » | Applis de la liste blanche (`apps` dans `companion.json`), après ta permission |
| « Retiens que mon projet s'appelle… » | Mémoire SQLite, réutilisée dans les conversations |
| « Entraîne-toi » | Lance une session d'auto-entraînement tout de suite |
| « Salut ! » | Il te rend ton salut (il salue aussi au démarrage) |

## Dépendances (dans `external/`)

| Dossier | Source | Option CMake |
|---|---|---|
| `llama.cpp/` | `git clone https://github.com/ggml-org/llama.cpp` | `COMPANION_WITH_LLAMA` |
| `whisper.cpp/` | `git clone https://github.com/ggml-org/whisper.cpp` | `COMPANION_WITH_WHISPER` |
| `onnxruntime/` | release `onnxruntime-win-x64-<version>.zip`, décompressée | `COMPANION_WITH_ONNX` |
| `espeak-ng/` | install espeak-ng (`include/`, `lib/`, `bin/espeak-ng.dll`) | `COMPANION_WITH_ESPEAK` |

espeak-ng n'apparaît pas dans la liste `external/` du rapport, mais Piper en a besoin pour
convertir le texte en phonèmes. llama.cpp et whisper.cpp partagent une seule copie de ggml : clone-les à des dates proches.
GPU : `COMPANION_LLM_VULKAN=ON` (défaut) demande le [Vulkan SDK](https://vulkan.lunarg.com/) ; sans lui,
passe `-DCOMPANION_LLM_VULKAN=OFF` (cerveau sur CPU uniquement).

nlohmann_json et SQLite sont récupérés automatiquement (FetchContent). Chaque brique peut être désactivée
(`-DCOMPANION_WITH_ONNX=OFF`…) : l'overlay et le governor fonctionnent sans aucun modèle.

## Modèles (`assets/models/`, copiés à côté de l'exe)

- `qwen2.5-3b-instruct-q4_k_m.gguf` : cerveau local (~2 Go) — ou tout autre 3B instruct GGUF (`llmModel` dans `companion.json`)
- *(optionnel, vision locale)* un modèle vision-langage **et** son projecteur, ex. `Qwen2.5-VL-3B-Instruct-Q4_K_M.gguf`
  + `mmproj-Qwen2.5-VL-3B-Instruct-f16.gguf` (dépôt `ggml-org/Qwen2.5-VL-3B-Instruct-GGUF`). Il remplace alors le
  modèle texte (`llmModel` + `llmMmproj`) : un seul modèle pour parler et voir.
- `ggml-base-q5_1.bin` : Whisper base quantifié (~60 Mo)
- `fr_FR-siwis-medium.onnx` + `fr_FR-siwis-medium.onnx.json` : voix Piper française
- `espeak-ng-data/` à côté de l'exécutable
- `assets/textures/matcap_chrome.png` : texture chrome 256x256 (fournie ; régénérable avec
  `gen_matcap.py`). Si elle manque, l'avatar génère un chrome équivalent en C++.

## Build (Visual Studio 2022, x64)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release
```

## Raccourcis

- `Ctrl+Alt+C` : push-to-talk (appuyer pour parler, ré-appuyer pour envoyer)
- `Ctrl+Alt+V` : bascule push-to-talk ↔ détection de voix (VAD, micro ouvert en continu)
- `Ctrl+Alt+H` : afficher / masquer l'avatar
- `Ctrl+Alt+Q` : quitter

Logs : `OutputDebugString` (DebugView de Sysinternals, ou la fenêtre Sortie de VS).

## Configuration (optionnelle)

Copie `companion.example.json` en `companion.json` à côté de l'exe : modèle, vision, liste blanche de wikis,
applis lançables, alertes studio, Claude activé ou non, permission cloud, etc. Le cache des données Riot est dans
`%LOCALAPPDATA%\CompanionAI\ddragon`. La mémoire est dans `%LOCALAPPDATA%\CompanionAI\companion.db`.

## Clé Claude (facultative)

Sans clé, tout reste local. Pour activer le dernier recours, définis la variable d'environnement
`ANTHROPIC_API_KEY`, ou enregistre la clé dans le Gestionnaire d'identification Windows
(identifiant générique `CompanionAI/anthropic`).
