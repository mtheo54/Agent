# CompanionAI (héritage OMNIS)

- **Étape 1** : `ResourceGovernor`, `VoiceEngine` + `VoiceFX`, `OverlayWindow` (overlay transparent click-through).
- **Étape 2** : cerveau « local d'abord » — `AgentOrchestrator`, `LocalBrain` (llama.cpp), `MemoryDB` (SQLite),
  `PermissionGate`, `WebResearcher` (wikis en liste blanche), `ClaudeClient` (dernier recours), `GameCoach` (LoL).
- **Étape 3** : studio (`WasapiLoopback` + `OmnisAnalyzer`), vision (capture DXGI, analysée en local puis
  Claude Vision en secours), conseiller de build LoL local, `SystemControl` (volume, musique, applis).
- **Étape 4** : `SelfTrainer` — auto-entraînement pendant l'inactivité du PC, dans un processus isolé ;
  personnage 2D animé (voir ci-dessous).

## Le personnage

Un bonhomme chromé gris, sans visage, **dessiné en 2D dans un style d'animation japonaise** et animé à
**24 images par seconde** (12 en jeu) :

- chrome travaillé : ombre nette bleutée, horizon reflété sur la tête et le torse, liseré de lumière
  dans l'ombre, reflets francs, étincelle ;
- encrage bleu nuit, plus épais côté ombre ; détails en traits fins (plaque de torse, jointures) ;
- articulations aux épaules, coudes et genoux, cou, mains-gants avec pouce, chaussures à semelle ;
- traînée de mouvement sur les gestes rapides, traits d'emphase manga quand il parle, émotes ;
- poses clés franches, dépassement puis stabilisation, regard qui balaie au repos.

- **physique secondaire** : chaque partie du corps suit sa pose comme un ressort amorti — la tête et
  les avant-bras arrivent avec un léger retard et rebondissent, les mouvements dépassent puis se posent ;
- **modelé à l'aérographe** par-dessus les aplats et lumière réfléchie bleutée sous les formes ;
- **accessoires selon le contexte**, dessinés avec le même rendu et apparus avec un petit « pop » :

| Contexte | Accessoire |
|---|---|
| Cyberpunk 2077 | cyber-bras néon (design original) |
| FL Studio | casque de studio |
| League of Legends, CS2, Valorant | casque gaming avec micro |
| auto-entraînement | bandeau de sport |
| recherche web | loupe |
| question à Claude | antenne qui clignote |
| sommeil (PC inactif) | bonnet de nuit |

Accessoires et contextes sont décrits dans `rig.json` (`props`, `contexts`) : on en ajoute sans recompiler.

Il a ses animations selon ce qu'il fait :

| Animation | Quand |
|---|---|
| `idle` | au repos (respiration) |
| `wave` | au démarrage et quand tu dis « salut » |
| `talk` | quand il parle (il rebondit aussi sur sa voix) |
| `listen` | quand tu parles : main à l'oreille, ondes sonores |
| `think` | recherche / Claude : main au menton, « … » |
| `train` | auto-entraînement : footing sur place, goutte de sueur |
| `studio` | dans FL Studio : la tête suit le rythme, notes de musique |
| `happy`, `laugh`, `surprised`, `shrug`, `nod`, `shake`, `clap`, `point` | réactions ponctuelles (à ton émotion, ou au début de sa propre phrase : « Ouais » → hoche la tête, « J'sais pas » → hausse les épaules) |
| `sad`, `empathy`, `soothe`, `dance`, `sleep` | postures d'humeur : empathique si tu es triste, apaisant si tu es énervé, somnolent quand le PC est inactif |
| `talk2` | variante de gestes quand il parle (tirée au hasard, pour ne pas répéter) |

## Façon de parler et émotions

- **Réponses quasi instantanées** : si sa réponse n'est pas prête en 650 ms, une courte relance
  pré-synthétisée part tout de suite (« Hmm… », « Attends… », « Oh… » selon ton humeur), puis il démarre
  dès la première proposition de sa phrase.
- **Style oral** : français parlé, contractions, réactions avant la réponse, hésitations naturelles et rares.
- **Émotions dans ta voix**, 100 % locales : hauteur (YIN), étendue mélodique, intonation finale,
  énergie et débit, comparés à **ta voix habituelle** (apprise au fil des conversations, mémorisée).
  La voix donne l'agitation, les mots tranchent entre joie et colère. Il adapte alors sa réponse, sa
  diction (plus vive, plus douce ou plus posée) et sa posture.
- **Tu peux lui couper la parole** en écoute continue : il se tait et t'écoute (le décodeur s'arrête net).
- **Voix neuronale** : Qwen3-TTS 0.6B (Apache 2.0) via qwentts.cpp, sur le GPU (Vulkan), en streaming.
  Sa voix est **originale** : conçue par description (« jeune homme, accent parisien, timbre chaud et un peu
  éraillé, ton complice de pote de studio ») avec Qwen3-TTS VoiceDesign, une prise par émotion dans
  `assets/voice/omnis/` (`normal`, `bright`, `soft`, `calm` + transcription). L'empreinte vocale vient toujours
  de `normal` (identité stable), l'intonation de la prise qui correspond à l'humeur. Les nombres et unités
  sont écrits en toutes lettres avant lecture (« -4.1 LUFS » → « moins quatre virgule un LUFS »).
  Pour lui donner une autre voix : remplace ces 4 fichiers (WAV 24 kHz mono + texte exact).
- **Réactions humaines… et bugs de robot** (réglage `TICS & BUGS`, clé `voiceQuirks`) :
  - vrais sons dans sa voix (`assets/voice/omnis/vocal/`) : rires, petit rire, soupir, « hmm », toux, « oh ! » ;
    le modèle peut les placer avec `(rire)`, `(soupir)`, `(tousse)`, `(bug)`…, jamais prononcés ;
  - spontanéité : une réponse drôle se dit en riant à moitié (prise `amused`), une surprise avec la voix qui
    monte (`surprised`), parfois précédée d'un petit rire ou d'un « oh ! » ;
  - de temps en temps une syllabe accroche comme un buffer qui saute (« je s-s-suis »), grain numérique qui
    monte, parfois un ralenti façon bande ; une toux de robot finit en grésillement. Au plus un bug par 45 s,
    une toux par 3 min, **jamais** quand il console ou calme quelqu'un ;
  - l'intonation varie d'une phrase à l'autre, et il ne recommence pas deux réponses par la même interjection.
- **En jeu**, la voix passe sur Piper (CPU) pour ne pas prendre de GPU au jeu ; les relances (« hmm… ») et
  répliques courtes restent celles de la voix neuronale, pré-calculées. `neuralVoiceInGame: true` pour la garder.
  Le grain 8-bit est discret par défaut (`fxIntensity` 0,15).
- **Trois tons** (réglage `TON`, clé `tone`) : *poli*, *pote*, *sans filtre* (par défaut : argot, verlan,
  jargon de studio, un gros mot pour l'emphase, vannes ; jamais d'insulte envers toi, et pas de vannes si tu
  as l'air triste ou fatigué). Toutes ses répliques toutes faites (alertes, confirmations, relances)
  sont dans `assets/persona/phrases.json` : modifiables sans recompiler.
- **Avis sur ton son, dans le langage d'OMNIS** (dépôt `mtheo54/Mix-master`) :
  - il comprend le jargon de beatmaker grâce au **lexique d'OMNIS** (`assets/omnis/lexique_fr.txt`) :
    « la basse elle bave », « mon kick tape pas », « ça pique les oreilles » déclenchent l'écoute du mix ;
  - le diagnostic reprend celui d'OMNIS (`MixDiagnosis`) : 0 dBFS touché, mix écrasé, **grave pas mono sous
    120 Hz**, **perte en mono**, bas-médium boueux, médiums nasaux, aigus durs… avec l'action concrète dans FL ;
  - partage des rôles : le modèle local fait **une phrase de réaction** (sa vanne), puis le **conseil exact**
    est dit mot pour mot. Un 3B à qui on confie le conseil technique le déforme.
  Simulation reproductible : `tools/sim/beat.py` (beat raté exprès) + `tools/sim/studiosim.cpp`.

## Réglages

`Ctrl+Alt+O` (ou « ouvre les réglages ») : fenêtre au style du site abrasion — nom, taille, opacité et coin
de l'écran du personnage, grain 8-bit, débit, mode d'écoute, relances, interruption, lecture des émotions,
expressivité, auto-entraînement, Claude. Aperçu animé avec boutons d'expressions, état du cerveau et de la
voix, jauge d'émotion en direct. Tout s'applique immédiatement et s'enregistre dans `companion.json`.
Polices Space Grotesk et Space Mono fournies dans `assets/fonts` (licence SIL OFL, voir `OFL.txt`).

Tout est dans `assets/avatar/rig.json` (squelette, formes, palette, images clés). Pour régler une
animation sans recompiler, rends-la en vidéo avec le même algorithme de dessin :

```bash
pip install pycairo
```

```bash
python tools/avatar_preview.py --sequence idle,wave,talk --contexts ",,cyberpunk" --out apercu.mp4
```

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
| `qwentts.cpp/` | `git clone --recurse-submodules https://github.com/ServeurpersoCom/qwentts.cpp` (MIT) | `COMPANION_WITH_QWENTTS` |

espeak-ng n'apparaît pas dans la liste `external/` du rapport, mais Piper en a besoin pour
convertir le texte en phonèmes. llama.cpp et whisper.cpp partagent une seule copie de ggml : clone-les à des dates proches.
GPU : `COMPANION_LLM_VULKAN=ON` (défaut) demande le [Vulkan SDK](https://vulkan.lunarg.com/) ; sans lui,
passe `-DCOMPANION_LLM_VULKAN=OFF` (cerveau sur CPU uniquement).

qwentts.cpp a **son propre** ggml (fork incompatible avec celui de llama.cpp) : il est construit à part en
`qwen.dll` (seules les fonctions `qt_*` sont exportées) et chargé au démarrage. Sans la DLL ou ses modèles,
Omnis parle avec Piper, sans erreur.

nlohmann_json et SQLite sont récupérés automatiquement (FetchContent). Chaque brique peut être désactivée
(`-DCOMPANION_WITH_ONNX=OFF`…) : l'overlay et le governor fonctionnent sans aucun modèle.

## Modèles (`assets/models/`, copiés à côté de l'exe)

- `qwen2.5-3b-instruct-q4_k_m.gguf` : cerveau local (~2 Go) — ou tout autre 3B instruct GGUF (`llmModel` dans `companion.json`)
- *(optionnel, vision locale)* un modèle vision-langage **et** son projecteur, ex. `Qwen2.5-VL-3B-Instruct-Q4_K_M.gguf`
  + `mmproj-Qwen2.5-VL-3B-Instruct-f16.gguf` (dépôt `ggml-org/Qwen2.5-VL-3B-Instruct-GGUF`). Il remplace alors le
  modèle texte (`llmModel` + `llmMmproj`) : un seul modèle pour parler et voir.
- `ggml-base-q5_1.bin` : Whisper base quantifié (~60 Mo)
- **Voix neuronale** (dépôt Hugging Face `Serveurperso/Qwen3-TTS-GGUF`) : `qwen-talker-0.6b-base-Q8_0.gguf`
  (~1 Go) + `qwen-tokenizer-12hz-Q8_0.gguf` (~0,3 Go). Environ 1,5 Go de VRAM.
- `fr_FR-siwis-medium.onnx` + `fr_FR-siwis-medium.onnx.json` : voix Piper française (repli, et voix en jeu)
- `espeak-ng-data/` à côté de l'exécutable
- `assets/avatar/rig.json` : le personnage 2D (fourni).

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
- `Ctrl+Alt+O` : réglages du personnage
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
