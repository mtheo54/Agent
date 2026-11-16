# CompanionAI (héritage OMNIS)

- **Étape 1** : `ResourceGovernor`, `VoiceEngine` + `VoiceFX`, `OverlayWindow` + `ProceduralAvatar` + `MatCapShader.hlsl`.
- **Étape 2** : cerveau « local d'abord » — `AgentOrchestrator`, `LocalBrain` (llama.cpp), `MemoryDB` (SQLite),
  `PermissionGate`, `WebResearcher` (wikis en liste blanche), `ClaudeClient` (dernier recours), `GameCoach` (LoL).

## Cascade

| Niveau | Où | Quoi |
|---|---|---|
| 0 | C++ | « pause », « entraîne-toi », « retiens que… », question de build pendant une partie LoL |
| 1 | llama.cpp (3B) | conversation ; émet `[ESCALATE \| type: … \| sujet: …]` s'il ne sait pas |
| 1b | web **local** | cache SQLite d'abord, sinon wiki en liste blanche (avec ta permission), résumé par le modèle local — 0 token |
| 2 | Claude | dernier recours (raisonnement complexe, ou le web local n'a rien donné), après ta permission |

Ce qui a été appris sur le web ou auprès de Claude est stocké : la même question est ensuite traitée sans réseau.

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

Copie `companion.example.json` en `companion.json` à côté de l'exe : modèle, liste blanche de wikis,
Claude activé ou non, permission cloud, etc. La mémoire est dans `%LOCALAPPDATA%\CompanionAI\companion.db`.

## Clé Claude (facultative)

Sans clé, tout reste local. Pour activer le dernier recours, définis la variable d'environnement
`ANTHROPIC_API_KEY`, ou enregistre la clé dans le Gestionnaire d'identification Windows
(identifiant générique `CompanionAI/anthropic`).
