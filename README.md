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
| **Cerveau** | Si tu lui parles en jeu, Omnis regarde **ce que le jeu sature** (charge 3D de la carte, hors lui, moyenne sur ~8 s) : **jeu limité par la carte** (AAA, carte ≥ 85 %) → petit modèle **sur le processeur**, cœur isolé ; **jeu limité par le processeur** (LoL, Valorant, CS2 à haut FPS : la carte attend) → modèle du quart de VRAM **sur la carte**, en **priorité GPU basse** et en micro-lots de 64 tokens (des calculs de quelques ms, jamais une image du jeu bloquée longtemps). Sur le processeur, il volerait justement la bande passante mémoire dont un jeu à 240 fps a besoin. Verdict gardé au moins 60 s (pas de rechargement en boucle). Dans les deux cas, **débit plafonné** à 12 tokens/s (`llmGamingTokensPerSecond`) : la voix en lit ~5 par seconde, le cerveau travaille donc par courtes salves au lieu d'occuper la carte ou la mémoire en continu (mesuré : occupation divisée d'autant que le débit). Une **photo de l'état** après sa personnalité est gardée sur disque (`%LOCALAPPDATA%\CompanionAI\brain-cache`, ~30 Mo) : 1er mot en **0,8 s au lieu de 15 s**, dès la 1re question de la partie (mesuré sur 2 threads). Après la partie, le gros modèle revient sur GPU à la première question. |
| **GPU** | Classe d'ordonnancement GPU du processus « sous la normale » (`D3DKMTSetProcessSchedulingPriorityClass`) : quand le jeu et lui veulent la carte au même moment, Windows sert le jeu d'abord. |
| **CPU** | Processus **confiné au dernier cœur physique** (ses 2 threads SMT) : le jeu garde les 5 autres cœurs pour lui seul. Priorité « inférieure à la normale », EcoQoS (Windows 11). Le fil audio garde MMCSS : pas de craquements. |
| **RAM** | Priorité mémoire basse et working set rogné à l'entrée en jeu : sous pression, ce sont ses pages qui partent, pas celles du jeu. |
| **Affichage** | L'avatar est **caché** (une fenêtre posée sur le jeu peut le sortir du *flip indépendant* : latence, FPS) ; il n'apparaît que quand il parle ou t'écoute (`avatarInGame`). Caché, rien n'est dessiné et la boucle ne se réveille que 10 fois par seconde. |
| **Voix** | Piper sur CPU (relances pré-calculées avec la voix neuronale) ; bugs, émotions et grain 8-bit : 0,045 % d'un cœur, aucune allocation dans la boucle audio. Sortie audio arrêtée quand il ne parle pas (zéro réveil). |
| **Fond** | Pas d'auto-entraînement, pas d'écoute du mix ; écoute micro seulement en push-to-talk ou VAD (2 threads Whisper). |

Tout se rétablit automatiquement à la sortie du jeu. `gameIsolation: false` pour désactiver le confinement.

### Moniteur de perfs et presets

Pendant chaque partie, Omnis mesure **ce qu'il coûte vraiment à ton jeu** :

- **FPS et 1 % low** du jeu, lus comme PresentMon / CapFrameX (événements `Present` de DXGI via ETW, jeux
  D3D11 / D3D12). Aucune injection, aucune lecture de la mémoire du jeu : compatible Vanguard et VAC.
- Chaque image est classée **« Omnis au repos »** ou **« Omnis travaille »** (cerveau qui génère, voix qui
  parle). L'écart entre les deux, c'est son impact sur *ta* partie, pas sur un benchmark.
- Processeur et carte graphique du jeu et d'Omnis, chaque seconde.
- Fichiers dans `%LOCALAPPDATA%\CompanionAI\perf` : `perf-<jeu>-<date>.csv` (une ligne par seconde, à ouvrir
  dans Excel) et `perf-<jeu>-<date>.txt`, le résumé de la partie (« Impact d'Omnis : -1,2 % de FPS → OK »
  ou « ÇA BOUFFE TROP »).

**Lire les FPS demande un droit Windows** (sinon tout le reste est mesuré, sans les FPS). Une seule fois,
dans PowerShell **en administrateur**, puis déconnecte-toi / reconnecte-toi :

```powershell
Add-LocalGroupMember -SID S-1-5-32-559 -Member $env:USERNAME
```

(groupe « Utilisateurs du journal de performances » ; lancer Omnis en administrateur marche aussi.)

**Presets** (`gamingPreset` pour tous les jeux, `gamePresets` par jeu dans `companion.json`, ou à la voix) :

| Preset | Où | Modèle (ta config) |
|---|---|---|
| `auto` | carte ou processeur selon ce que le jeu sature, + ce qu'Omnis a appris sur ce jeu | 4B ou 2B |
| `carte` | carte graphique, priorité basse, micro-lots | modèle du quart de VRAM (4B) |
| `carte-leger` | carte graphique | 0.8B (~0,9 Go) |
| `processeur` | cœur isolé | 2B |
| `processeur-leger` | cœur isolé | 0.8B |

À la voix, en partie :

- **« teste les perfs »** : calibrage d'une minute pendant que tu joues (repos / carte / repos / processeur, et
  la version légère si les deux coûtent trop). Il retient pour ce jeu le mode qui coûte le moins de FPS
  (à 1,5 point près, le plus intelligent) et te dit les chiffres.
- **« ça bouffe combien ? »** : FPS au calme et quand il bosse, sa part de processeur et de carte.
- **« passe sur le processeur »**, **« mets-toi sur la carte »**, **« mode léger »**, **« mode auto »** : retenu
  pour ce jeu (`game_presets.json`).

**Garde-fou** (`perfAutoAdjust`) : en `auto`, si sur au moins 15 s de travail il fait perdre plus de 5 % de
FPS ou 10 % de 1 % low, il passe un cran plus léger pour ce jeu et te le dit. Il ne touche jamais à un preset
que tu as choisi.

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
remesurée au moment où tu lui parles, 1 Go laissé au jeu). Pas assez, ou jeu qui sature déjà la carte ? Le
cerveau passe sur le processeur.

**Pourquoi pas plus gros, ni « MoE » ?** Le seul petit MoE plus malin que le 9B (Gemma 4 26B-A4B : 4 milliards
de paramètres actifs sur 26) pèse 15 à 18 Go en 4 bits ; Gemma 4 12B fait jeu égal avec Qwen3.5 9B en étant
plus lent. Le 9B reste le meilleur rapport intelligence / ressources pour ce que fait Omnis.

| PC | Hors jeu | En jeu | Téléchargé |
|---|---|---|---|
| Carte intégrée, 8 Go de RAM | Qwen3.5 0.8B (CPU) + Piper | 0.8B (CPU) | 0,7 Go |
| Carte intégrée, 16 Go | 2B (CPU) + Piper | 2B (CPU) | 1,4 Go |
| 2 Go de VRAM | 0.8B (GPU) + Piper | 0.8B (CPU) | 0,7 Go |
| 4-6 Go | 2B (GPU) + Piper | 0.8B (GPU) | 1,9 Go |
| 8 Go | 2B (GPU) + voix neuronale | 2B (GPU) | 2,7 Go |
| 12 Go | 4B (GPU) + voix neuronale | 2B (GPU) | 5,4 Go |
| 16 Go (RX 9060 XT) | **9B** (GPU) + voix neuronale | **4B** (GPU, prio basse) ; jeu qui sature la carte : 2B (CPU) | 11,6 Go |
| 24 Go | 9B + voix neuronale | 9B | 7,1 Go |

Modèles : Qwen3.5 0.8B / 2B / 4B / 9B (Q4_K_M, `unsloth/Qwen3.5-*-GGUF`, Apache 2.0), Whisper base ou tiny,
voix Piper `fr_FR-siwis-medium`, voix neuronale Qwen3-TTS 0.6B (`Serveurperso/Qwen3-TTS-GGUF`, cartes ≥ 8 Go).
Imposer un modèle : un chemin dans `llmModel` / `llmGamingModel` (`companion.json`) au lieu de `"auto"`.
Vision locale (optionnelle) : `mmproj-F16.gguf` du même dépôt que le cerveau, dans `llmMmproj`.

## Deux modes : technique ou rage bait

Avant chaque réponse, Omnis décide du mode **dans le code**, pas au hasard du modèle (`OmnisModes.hpp`) :

| Mode | Déclencheur | Comportement |
|---|---|---|
| **Technique** (prioritaire en cas de doute) | C++, DSP, plugins, mixage, MAO, électrotechnique, automates, câblage, dépannage, stratégie de jeu sérieuse ; ou une vraie question | Chirurgical, rapide, fiable. Zéro blague, zéro sarcasme. Il n'invente jamais une valeur, un registre, une broche. |
| **Rage bait** | Bavardage, excuses de défaite, ego, avis subjectif, provocation | Faux expert arrogant, calme, faussement bienveillant. Il ne reconnaît jamais avoir tort, minimise tes excuses, chipote. 2 à 3 phrases, sans insultes. |

**Recherche** : quand il lui faut une doc, une spec, une info récente ou un fait précis, il répond seulement
`[SEARCH: mots-clés]`, en 3 à 6 mots-clés secs (par exemple `[SEARCH: "Murrelektronik" Profinet BF LED fault manual]`).

1. **Où il cherche** :
   - en technique, sur le vrai web d'abord (avec ta permission, ou l'autorisation « surfe cette nuit ») ;
   - sinon, sur les wikis autorisés.
2. **Les résultats** lui reviennent dans `<contexte_web>`, avec des règles selon le mode :
   - **technique** : il répond uniquement avec ce qui est dans la source ; si l'info exacte manque, il dit
     « Information absente de la source » et lance **une** recherche plus ciblée ;
   - **rage bait** : il se sert du fait exact pour te contredire, sans le déformer.
3. **Toujours rien** (ou la même recherche une deuxième fois) : il passe la main à **Claude**, avec ta permission.

Testé avec le vrai modèle :
- il produit la balise de recherche exacte ;
- avec une source qui contient l'info, il y répond (câble, nom d'appareil PROFINET, conflit d'IP) ;
- avec une source qui ne la contient pas, il dit « Information absente de la source » et relance.

## Il prend la main (clavier et souris)

Dans l'appli au premier plan, à ta demande : **« prends la main et ouvre le mixer »**, **« clique sur
Enregistrer »**, **« trace une ligne de 0,0 à 100,0 »** (AutoCAD), **« ouvre le piano roll »** (FL Studio)…

1. Il écrit un plan court (au plus 10 actions, format imposé : il ne peut rien faire d'autre), puis l'**annonce**
   (« J'ouvre le mixer et je te mets le kick en solo »).
2. Ce qui peut faire perdre du travail (fermer, supprimer, enregistrer sous, `_ERASE`, `_PURGE`…) attend ton
   **« oui »** à voix haute.
3. Il exécute. **Tu bouges la souris ou tu touches une touche : il s'arrête net** et te rend la main (il distingue
   tes gestes des siens). Il n'agit que dans la fenêtre où tu étais : si elle change, il s'arrête au lieu de taper
   ailleurs.

| Appli | Comment il agit |
|---|---|
| **AutoCAD / AutoCAD Electrical** | Commandes envoyées directement à AutoCAD (automatisation COM), noms anglais `_LINE`, `_AEWIRE`, `_AECOMPONENT`… qui marchent aussi sur un AutoCAD en français. Repli : tapées dans la ligne de commande. |
| **FL Studio** | Raccourcis seulement (F5 playlist, F6 channel rack, F7 piano roll, F9 mixer, Ctrl+R export WAV…) : FL ne décrit pas ses boutons à Windows. |
| **Autres applis** | Boutons, menus et onglets trouvés **par leur nom** (accessibilité de Windows) : il les invoque sans bouger ta souris quand c'est possible, sinon il clique au centre puis remet la souris où elle était. |

**Jamais en jeu** : l'injection de clavier et de souris, c'est exactement ce que traquent Vanguard et VAC.
Windows interdit aussi de piloter une appli lancée en administrateur depuis une appli qui ne l'est pas.
`pilotEnabled: false` pour couper la fonction.

## Dans tes calls Discord (par ton micro)

Omnis ne pilote **pas** ton compte Discord : c'est interdit par Discord et ça peut te faire bannir. Sa voix passe
**par ton micro**, grâce à un câble audio virtuel :

1. Installe **VB-Cable** (gratuit, vb-audio.com), une seule fois.
2. Dans Discord : *Paramètres → Voix et vidéo → Périphérique d'entrée* = **CABLE Output**.
3. Omnis relaie ton vrai micro vers le câble dès que Discord tourne (10 ms de latence, ~0,1 % d'un cœur) :
   tes potes t'entendent comme avant.
4. **« Omnis, rejoins le call »** : il se présente (« c'est Omnis, l'IA de mon pote »), sa voix et ses bruitages
   passent dans le call, et il **écoute le son de Discord** (seulement Discord, jamais le reste du PC) pour
   comprendre ce que disent les autres.

Dans le call, il répond quand on dit son nom, et lâche de temps en temps une vanne ou un petit rage bait sans
qu'on l'appelle (`callBaitChance`, au plus une par minute). Mêmes règles qu'avec toi : jamais sur l'origine, la
religion, la sexualité, le physique ou un handicap, et il lâche si quelqu'un est vraiment blessé.
**N'importe qui dans le call** peut dire « Omnis, tais-toi » (il se calme 10 minutes) ou « Omnis, dégage »
(il quitte le call). Toi : « quitte le call ».

**Il sait qui parle, d'abord en regardant Discord.** Quand quelqu'un parle, Discord entoure son avatar d'un
cercle vert (ou encadre sa tuile). Omnis capture la fenêtre Discord, 4 fois par seconde et seulement pendant que
quelqu'un parle (jamais en jeu), repère le vert et lit le pseudo à côté grâce à l'accessibilité de Windows. Il ne
clique rien et n'envoie rien à Discord : il regarde, comme toi. Il faut que le pseudo soit visible à l'écran, par
exemple Discord sur ton 2e écran. Mets ton pseudo dans `discordMyName` : ton propre cercle sera ignoré.

**Chaque phrase étiquetée par Discord lui apprend la voix de cette personne**, à travers SON micro, aussi pourri
soit-il : jusqu'à 5 empreintes par personne (CAM++, ~30 Mo téléchargés au premier call). Quand Discord n'est pas
visible (toi en plein jeu), la voix prend le relais. Il exige alors une marge nette sur la 2e voix la plus proche,
pour ne pas confondre deux voix qui se ressemblent.

Ce que donnent les mesures :

| Conditions | Résultat |
|---|---|
| Micro bas de gamme (350-3200 Hz, 10 bits), Opus 12 kb/s, bruit, 6 phrases apprises par personne | 72 % des phrases attribuées à la voix seule, **0 erreur** ; le reste est « pas sûr », et rien n'est attribué |
| Micros corrects | 99 % de bonnes attributions |
| Moquerie | Reprochée seulement à quelqu'un identifié sans ambiguïté. Sinon il demande « qui a osé ? » |

**Les pseudos, ça se change pour rigoler : l'identité, c'est la voix.** Le pseudo n'est qu'un surnom de la fiche.
Pour savoir qui est derrière un pseudo, il suit cet ordre :

1. **la voix**, si elle est nette ;
2. **l'avatar**, s'il n'appartient qu'à une personne (les avatars par défaut de Discord se ressemblent tous) ;
3. le pseudo.

| Blague | Ce qu'il fait |
|---|---|
| Karim se renomme « Le Boss » | Il sait que c'est Karim, garde « Karim » dans ses anciens pseudos et le vanne là-dessus. Si la voix n'est pas nette sur une seule phrase, il attend jusqu'à 4 phrases et compare leur moyenne avant de décider « renommage » ou « nouvelle personne ». |
| Tom prend le pseudo de Léa pour piéger tout le monde | Sa voix le trahit : la fiche de Léa reste à Léa, et il grille Tom devant tout le monde (« t'as beau t'appeler Léa… »). |
| Deux fiches pour la même voix (renommage repéré en retard) | Fusionnées à la sortie du call, anciens pseudos compris. |

Mesuré avec des micros dégradés : 15 renommages sur 15 retrouvés, aucune fiche créée à tort, en général dès la
1re phrase. Le pseudo usurpé est démasqué à chaque essai.

**« Omnis, écoute le call »** (mode écoute) : il est là, écoute tout le monde et apprend les pseudos et les voix,
mais ne parle que si on l'appelle. « Parle normalement » pour le relâcher. Le meilleur moyen de démarrer : une
soirée en mode écoute avec Discord visible, et il connaît tout le monde. Chaque pote a sa **fiche** (sur ton PC seulement,
`call_people.json`) : prénom (« moi c'est Karim », ou toi : « le dernier qui a parlé c'est Karim »), ses dernières
phrases, ses moqueries envers Omnis, et les **sujets à éviter**. Il prévient en arrivant, et **« Omnis, oublie-moi »**
efface la fiche de celui qui le dit. Aucun son n'est enregistré, juste des nombres.

| Situation | Ce qu'il fait |
|---|---|
| Sujet sensible (deuil, santé, rupture, moral, argent, famille, agression) | Il n'en parle pas, même pas en blague, et ne vanne plus cette personne de la soirée. La fiche ne garde que la catégorie, jamais le détail. |
| Quelqu'un se moque d'Omnis | Il sait qui (voix), le note dans sa fiche et riposte **précis**, avec ce qu'il sait de lui (ses fails, ses phrases). |
| On ne l'a pas entendu (« quoi ? », « parle plus fort ») | Il remonte son volume de 30 % et répète (jusqu'à +7 dB), puis redescend doucement une fois bien entendu. |
| Rage bait | Ciblé sur ce que la personne vient de dire (contre-pied, fausse stat, compliment empoisonné, rappel d'un fail). Une fois sur trois, un bruitage à la place (`callBaitSounds` : FAHHH, Pizza Nizar…), et de temps en temps en **earrape**. |
| Quelqu'un rappe, fait une prod, stream sa DAW, ou de la musique joue | Mode discret 10 min : plus de vannes ni de bruitages, il répond bref seulement si on lui parle. |
| On lui parle par-dessus | Il n'attend pas un silence pour répondre. |

**Earrape plafonné** : saturation et basses écrasées pendant 1,5 s au plus, au plus toutes les 10 minutes, crête à
-8 dBFS (~-10 dBFS efficaces, une dizaine de dB au-dessus d'une voix normale) : bien chiant, mais sans danger pour
les oreilles au casque. `earrape: false` pour couper.

### Comment il écrit une vanne

D'après ce qui fait rire (surprise, vérité hyper précise sur la cible, rappel d'un fail, règle de trois, plusieurs
chutes pour une même situation), chaque vanne est le résultat d'un petit concours. Côté bruitages, une vanne
spontanée est remplacée par un son 45 % du temps (`callBaitSoundChance`), et une vanne dite est ponctuée d'un son
juste après une fois sur deux (`callPunchSoundChance`) : environ 7 rage baits sur 10 ont leur bruitage.

1. **Deux vannes à partir de formes qui marchent** (`assets/persona/baits.json`, 59 formes écrites à la main, et
   celles apprises en vidéo). Il ne change que les [crochets] par SES mots ; s'il ne respecte pas la forme, elle est
   rejetée.
2. **Une vanne inventée** sur une technique tirée au hasard (jamais deux fois la même de suite).
3. **Un tri** : il faut reprendre ses mots à lui, être court, sans tics (« mec », « l'aura »…), et sans franchir la
   ligne (famille, physique, origine, religion, sexualité, genre, handicap, santé, argent, sujets sensibles du soir).
   Sinon c'est rejeté. À égalité, le modèle choisit la plus surprenante.

### Il s'entraîne en regardant des vidéos (YouTube, TikTok)

Pendant ses séances d'auto-entraînement (PC inactif, tué dès que tu touches à quoi que ce soit), il regarde
3 vidéos (`videoStudyPerSession`) :

- **Quoi** : d'abord les liens que tu lui donnes, puis des recherches YouTube à tour de rôle (clashs, roasts,
  punchlines, rage bait, trashtalk gaming…).
  - Copie un lien YouTube ou TikTok et dis « **étudie le lien copié** », ou ajoute-le dans `to_watch.txt`.
  - TikTok bloque la recherche automatique (l'extracteur de hashtags de yt-dlp est marqué cassé) : pour TikTok, ce
    sont tes liens.
- **Comprendre** :
  - le titre ;
  - les sous-titres, ou sinon l'audio, décodé par Windows et transcrit par Whisper sur ton PC ;
  - les commentaires les plus likés (la réaction, c'est la moitié du rage bait).
- **Retenir** : une leçon (la technique, pourquoi ça marche) et 1 à 4 **formes de vannes** réécrites par lui, avec des
  trous, filtrées comme ses vannes, puis ajoutées à sa banque (300 au plus). Rien n'est gardé de la vidéo elle-même.
- **Outils** : [yt-dlp](https://github.com/yt-dlp/yt-dlp) et [deno](https://github.com/denoland/deno) (exigé par
  yt-dlp pour YouTube), téléchargés une fois depuis leurs pages officielles. Ils ne sont lancés que si leur empreinte
  **SHA-256 correspond à celle publiée**. yt-dlp est repris tous les 30 jours (YouTube change souvent).

### Surfer pour s'entraîner (« tu peux surfer cette nuit »)

« **Omnis, tu peux surfer cette nuit** » : jusqu'à 10 h le lendemain (14 h au plus). « **Arrête de surfer** » coupe.
Pendant ce temps, tant que le PC est inactif :

- une séance d'entraînement toutes les 30 min au lieu de 2 h, avec deux fois plus de vidéos ;
- ses sujets sont cherchés sur le web libre (DuckDuckGo, sinon Bing), avec 3 pages lues par sujet et 12 au plus
  par séance ;
- la lecture est seule autorisée : pas de formulaire, pas de connexion, pas de téléchargement, uniquement des
  pages texte ;
- jamais : réseaux sociaux, messageries, mails, banques, paiement, sites adultes.

Il faut un message explicite (« surfer », « naviguer », ou « internet » + « cette nuit ») : « cherche sur internet
le build de Jinx » n'ouvre pas le surf pour la nuit.

### « Prends ma place » (toilettes, repas…)

« Omnis, prends ma place, je vais manger » : il rejoint le call si besoin et garde ta place **60 minutes au plus**
(`substituteMaxMinutes`, 10 à 60).

- Il annonce que tu es parti et que **c'est lui, Omnis** : il ne se fait jamais passer pour toi.
- Il répond aux questions, note ce qu'on te demande (« dis-lui de venir ce soir »).
- Il refuse **toute demande d'info privée** : DM de n'importe quel réseau, messages, mails, comptes, mots de passe,
  adresse.
- Toutes les 6 à 10 minutes quand ça ne parle pas, ou à la demande d'un pote (« Omnis, mets-nous une vidéo de… »),
  il ouvre **une vidéo YouTube, un TikTok, ou une recherche d'entraînement** sur l'**écran 1**. Les adresses sont
  limitées à YouTube, TikTok, Wikipédia, DuckDuckGo et Google. Messagerie, compte, réglages et historique sont
  bloqués.
- Il ne pilote rien d'autre sur ton PC pendant ton absence.
- **Partage d'écran** : lance toi-même ton partage de l'écran 1 avant de partir (Omnis ne clique pas dans
  Discord, ce serait automatiser ton compte). Tes potes voient alors ce qu'il ouvre.
- **À ton retour** (souris ou clavier touchés, ou « je suis revenu ») : il le dit au call, puis te fait le
  résumé **à toi seul** (le call ne l'entend pas).
- **Viré du call pendant ton absence** : il part s'entraîner, en recherches utiles. La première fois, il ajoute
  à son programme l'art du rage bait (contre-pied, timing, chute, memes FR du moment, et la limite entre troll et
  harcèlement). Le rapport t'attend à ton retour.

Mets un casque : avec des haut-parleurs, ton micro reprendrait sa voix une deuxième fois.
Ton micro par défaut dans Windows doit rester ton vrai micro (pas le câble) ; sinon règle `discordMicDevice`.

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
