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
  Sa voix part d'un **francophone natif** (corpus CML-TTS, CC-BY 4.0, voir `assets/voice/omnis/SOURCES.md`) :
  la première voix, inventée par description, avait un accent anglais (le clonage copie l'accent de la
  référence). Une prise par émotion dans `assets/voice/omnis/` (`normal`, `bright`, `soft`, `calm`, `amused`,
  `surprised` + transcription) ; l'empreinte vocale vient toujours de `normal` (identité stable),
  l'intonation de la prise qui correspond à l'humeur. Les nombres et unités sont écrits en toutes lettres
  avant lecture (« -4.1 LUFS » → « moins quatre virgule un LUFS »).
  Pour lui donner une autre voix : remplace ces fichiers (WAV 24 kHz mono + texte exact).
- **Réactions humaines… et bugs de robot** (réglage `TICS & BUGS`, clé `voiceQuirks`) :
  - **vrais** rires, petits rires, toux, raclements de gorge et soupirs humains (corpus VocalSound, CC BY-SA),
    plus « hmm » et « oh ! » dans sa voix (`assets/voice/omnis/vocal/`) ; le modèle peut les placer avec
    `(rire)`, `(soupir)`, `(tousse)`, `(se racle la gorge)`, `(bug)`…, jamais prononcés ;
  - **émotions exagérées** comme chez un humain (réglage EXPRESSIVITÉ) : excité, il parle plus vite, plus
    haut et plus fort ; doux ou calme, plus lentement, plus bas ; 7 intonations (dont *agacé*) ;
  - spontanéité : une réponse drôle se dit en riant à moitié (prise `amused`), une surprise avec la voix qui
    monte (`surprised`), parfois précédée d'un petit rire ou d'un « oh ! » ;
  - **bugs de robot**, plusieurs par phrase, 9 types : syllabe qui accroche (« je s-s-suis »), bande qui
    ralentit, syllabe rejouée en accéléré, coupure de buffer, saut, éclat numérique (4-6 bits), voix qui
    ondule, bout de syllabe à l'envers, flash « voix de robot ». Réglage par défaut : ~1 phrase sur 4 (1 sur
    3 quand il est excité), une sur deux au maximum ; gros bugs au plus un toutes les 20 s ; il bugge plus
    quand ton PC est chargé ; **jamais** quand il console ou calme quelqu'un. Coût mesuré : 0,045 % d'un
    cœur au pire cas (émotions + bugs + grain 8-bit), aucune allocation mémoire dans la boucle audio.
  - l'intonation varie d'une phrase à l'autre, et il ne recommence pas deux réponses par la même interjection.
- **En jeu**, la voix passe sur Piper (CPU) pour ne pas prendre de GPU au jeu ; les relances (« hmm… ») et
  répliques courtes restent celles de la voix neuronale, pré-calculées. `neuralVoiceInGame: true` pour la garder.
  Le grain 8-bit est discret par défaut (`fxIntensity` 0,15).
- **Trois tons** (réglage `TON`, clé `tone`) : *poli*, *pote*, *sans filtre*. Sans filtre (par défaut), il
  parle comme un pote en vocal à 2 h du mat', pas comme un héros de série : ton plat, pince-sans-rire,
  gros mots normaux, il te chambre et t'insulte gentiment (bouffon, gros naze, « -1000 aura »), jamais sur
  tes origines, ta religion, ta sexualité, ton physique ou un handicap. Humour un peu sombre, jamais sur le
  suicide ni quand tu vas vraiment mal (là, pas de vannes).
- **Rage bait** (sans filtre) : de temps en temps (au plus une fois toutes les 10 min), il te provoque avec
  une mauvaise foi parfaitement sérieuse (« FL Studio c'est un logiciel pour enfants », « la jungle c'est
  le rôle des gens qui savent pas laner », « la pizza ananas c'est la meilleure »). Si tu mords, il avoue en
  se marrant et te chambre de t'être fait avoir. Jamais si tu vas mal, jamais pendant un avis de mixage.
- **Voix « dans un vrai micro »** (`micEffect`, 0,7 par défaut) : coupe-bas, effet de proximité, présence vers
  5 kHz, compresseur, préampli qui chauffe un peu, petite pièce, souffle seulement pendant qu'il parle.
  **Hauteur vivante** (`pitchLife`) : sa voix dérive doucement (± 20 cents), attaque parfois par en dessous,
  fait des « boing » et des plongées. **Bizarreries** ponctuelles (TICS & BUGS) : voix dédoublée, flanger,
  résonance métallique, talkie-walkie. Le tout : 0,14 % d'un cœur, aucune allocation dans la boucle audio.
- **Soundboard (myinstants.com)** : « installe les sons » (pack de départ : vine boom, bruh, c'est vrai,
  fahhh…), « ajoute le son X », « joue le son X », « quels sons t'as ». Téléchargés **à ta demande** et
  gardés **sur ton PC** (`%LOCALAPPDATA%\CompanionAI\soundboard`), jamais livrés avec le programme (beaucoup
  sont des extraits protégés). Il en place parfois un dans ses répliques avec `(son: vine boom)`. Décodage
  MP3 : dr_mp3 (domaine public), niveau ramené à celui de sa voix.
  **Sons aux moments clés** (`soundEvents`, à ton goût) : démarrage « anime wow », mix qui clippe « fahhh »,
  rage bait mordu : rire de Cyril Hanouna, mort ou défaite en LoL « oh no », kill / victoire « c'est vrai »
  (au plus un toutes les 45 s, toujours pour un multi-kill), erreur « windows error », fin d'entraînement
  « suspense », retour après une absence : Frank Lebœuf (« salut c'est Frank Leboeuf »), « bonne nuit » /
  « à plus » : « windows shutdown », début d'une partie de LoL : « emotional damage ». En partie : sons
  seulement, il ne te parle pas en plein combat. Un son peut être un nom (recherche) ou un **lien
  myinstants** exact (`https://www.myinstants.com/fr/instant/…`).
  **Sons d'émotion** (ta voix + tes mots) : tu rigoles → rire de Cyril Hanouna (aussi quand il avoue son
  rage bait) ; tu es hypé → Hava Nagila ; tu rages **en jeu seulement** → « fahhh » ; tu l'insultes →
  « sad violin » (il joue la victime) ; tu as l'air triste → « soft piano » puis voix douce, jamais de vanne ;
  fatigué entre 1 h et 6 h → « yawn ». Un seul son par phrase, chacun avec son délai minimum (1 à 30 min).
- **Memes TikTok du moment** (`assets/persona/memes.json`, français surtout, anglais aussi, quelques-uns
  sombres) : de temps en temps, 2-3 lui sont proposés et il en place un si ça colle. Mis à jour en
  novembre 2026 ; les memes meurent vite, édite le fichier pour en ajouter ou en retirer. Toutes ses répliques toutes faites (alertes, confirmations, relances)
  sont dans `assets/persona/phrases.json` : modifiables sans recompiler.
- **Avis sur ton son, dans le langage d'OMNIS** (dépôt `mtheo54/Mix-master`) :
  - il comprend le jargon de beatmaker grâce au **lexique d'OMNIS** (`assets/omnis/lexique_fr.txt`) :
    « la basse elle bave », « mon kick tape pas », « ça pique les oreilles » déclenchent l'écoute du mix ;
  - le diagnostic reprend celui d'OMNIS (`MixDiagnosis`) : 0 dBFS touché, mix écrasé, **grave pas mono sous
    120 Hz**, **perte en mono**, bas-médium boueux, médiums nasaux, aigus durs… avec l'action concrète dans FL ;
  - partage des rôles : le modèle local fait **une phrase de réaction** (sa vanne), puis le **conseil exact**
    est dit mot pour mot. Un 3B à qui on confie le conseil technique le déforme.
  Simulation reproductible : `tools/sim/beat.py` (beat raté exprès) + `tools/sim/studiosim.cpp`.

## En jeu : 0 FPS perdu (rapport §2)

Dès qu'un jeu est détecté (processus connu ou appli plein écran) :

| Ressource | Ce que fait le compagnon |
|---|---|
| **VRAM** | Le cerveau (≈ 5,7 Go) et la voix neuronale (≈ 1,5 Go) **quittent la carte graphique** immédiatement. Rien n'est rechargé tant que tu ne lui parles pas. |
| **Cerveau** | Si tu lui parles en jeu : petit modèle (`llmGamingModel`, Qwen3.5 2B ≈ 1,3 Go) **sur CPU**, 2 threads. Une **photo de l'état** après sa personnalité est gardée sur disque (`%LOCALAPPDATA%\CompanionAI\brain-cache`, ~30 Mo) : 1er mot en **0,8 s au lieu de 15 s**, dès la 1re question de la partie (mesuré sur 2 threads). Après la partie, le gros modèle revient sur GPU à la première question. |
| **CPU** | Processus **confiné au dernier cœur physique** (ses 2 threads SMT) : le jeu garde les 5 autres cœurs pour lui seul. Priorité « inférieure à la normale », EcoQoS (Windows 11). Le fil audio garde MMCSS : pas de craquements. |
| **RAM** | Priorité mémoire basse et working set rogné à l'entrée en jeu : sous pression, ce sont ses pages qui partent, pas celles du jeu. |
| **Affichage** | L'avatar est **caché** (une fenêtre posée sur le jeu peut le sortir du *flip indépendant* : latence, FPS) ; il n'apparaît que quand il parle ou t'écoute (`avatarInGame`). Caché, rien n'est dessiné et la boucle ne se réveille que 10 fois par seconde. |
| **Voix** | Piper sur CPU (relances pré-calculées avec la voix neuronale) ; bugs, émotions et grain 8-bit : 0,045 % d'un cœur, aucune allocation dans la boucle audio. Sortie audio arrêtée quand il ne parle pas (zéro réveil). |
| **Fond** | Pas d'auto-entraînement, pas d'écoute du mix ; écoute micro seulement en push-to-talk ou VAD (2 threads Whisper). |

Tout se rétablit automatiquement à la sortie du jeu. `gameIsolation: false` pour désactiver le confinement.

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

## Modèles : choisis et téléchargés selon le PC

Rien de lourd n'est livré avec le programme. Au premier lancement, Omnis mesure la machine (VRAM de la
meilleure carte et VRAM encore libre via DXGI, RAM) et propose de télécharger les modèles qui lui
conviennent (en arrière-plan, avec reprise si la connexion coupe ; tu peux jouer pendant ce temps).

**La règle** : hors jeu, le compagnon (cerveau + voix neuronale) prend **au plus la moitié de la VRAM** ;
en jeu, le cerveau prend **au plus un quart**, et moins si le jeu en occupe déjà beaucoup (VRAM libre
remesurée au moment où tu lui parles, 1 Go laissé au jeu). Pas assez ? Le cerveau passe sur le processeur.

| PC | Hors jeu | En jeu | Téléchargé |
|---|---|---|---|
| Carte intégrée, 8 Go de RAM | Qwen3.5 0.8B (CPU) + Piper | 0.8B (CPU) | 0,7 Go |
| Carte intégrée, 16 Go | 2B (CPU) + Piper | 2B (CPU) | 1,4 Go |
| 2 Go de VRAM | 0.8B (GPU) + Piper | 0.8B (CPU) | 0,7 Go |
| 4-6 Go | 2B (GPU) + Piper | 0.8B (GPU) | 1,9 Go |
| 8 Go | 2B (GPU) + voix neuronale | 2B (GPU) | 2,7 Go |
| 12 Go | 4B (GPU) + voix neuronale | 2B (GPU) | 5,4 Go |
| 16 Go (RX 9060 XT) | **9B** (GPU) + voix neuronale | **4B** (GPU) ; gros jeu : 2B ou 0.8B | 9,8 Go |
| 24 Go | 9B + voix neuronale | 9B | 7,1 Go |

Modèles : Qwen3.5 0.8B / 2B / 4B / 9B (Q4_K_M, `unsloth/Qwen3.5-*-GGUF`, Apache 2.0), Whisper base ou tiny,
voix Piper `fr_FR-siwis-medium`, voix neuronale Qwen3-TTS 0.6B (`Serveurperso/Qwen3-TTS-GGUF`, cartes ≥ 8 Go).
Imposer un modèle : un chemin dans `llmModel` / `llmGamingModel` (`companion.json`) au lieu de `"auto"`.
Vision locale (optionnelle) : `mmproj-F16.gguf` du même dépôt que le cerveau, dans `llmMmproj`.

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
