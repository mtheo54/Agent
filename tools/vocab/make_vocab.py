# Banque de mots d'Omnis (assets/knowledge/vocab.json) : ce que Whisper doit savoir écrire, par domaine, et les
# erreurs de transcription déjà vues sur le terrain. Régénérer : python3 tools/vocab/make_vocab.py
import glob, json, os, re, sys
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "knowledge", "vocab.json")
API = sys.argv[1] if len(sys.argv) > 1 else None  # dossier des données officielles (voir fetch_api.sh), facultatif

lol = """Aatrox Ahri Akali Akshan Alistar Ambessa Amumu Anivia Annie Aphelios Ashe Aurelion Aurora Azir Bard Bel'Veth Blitzcrank Brand Braum
Briar Caitlyn Camille Cassiopeia Cho'Gath Corki Darius Diana Draven Ekko Elise Evelynn Ezreal Fiddlesticks Fiora Fizz Galio Gangplank
Garen Gnar Gragas Graves Gwen Hecarim Heimerdinger Hwei Illaoi Irelia Ivern Janna Jarvan Jax Jayce Jhin Jinx K'Sante Kai'Sa Kalista
Karma Karthus Kassadin Katarina Kayle Kayn Kennen Kha'Zix Kindred Kled Kog'Maw LeBlanc Lee Sin Leona Lillia Lissandra Lucian Lulu Lux
Malphite Malzahar Maokai Master Yi Mel Milio Miss Fortune Mordekaiser Morgana Naafiri Nami Nasus Nautilus Neeko Nidalee Nilah Nocturne
Nunu Olaf Orianna Ornn Pantheon Poppy Pyke Qiyana Quinn Rakan Rammus Rek'Sai Rell Renata Renekton Rengar Riven Rumble Ryze Samira
Sejuani Senna Seraphine Sett Shaco Shen Shyvana Singed Sion Sivir Skarner Smolder Sona Soraka Swain Sylas Syndra Tahm Kench Taliyah
Talon Taric Teemo Thresh Tristana Trundle Tryndamere Twisted Fate Twitch Udyr Urgot Varus Vayne Veigar Vel'Koz Vex Vi Viego Viktor
Vladimir Volibear Warwick Wukong Xayah Xerath Xin Zhao Yasuo Yone Yorick Yuumi Yunara Zaahen Zac Zed Zeri Ziggs Zilean Zoe Zyra"""
# Noms de plusieurs mots écrits avec des tirets ci-dessus : on les recolle.
multi = ["Lee Sin", "Master Yi", "Miss Fortune", "Tahm Kench", "Twisted Fate", "Xin Zhao", "Aurelion Sol", "Jarvan IV", "Dr. Mundo", "Nunu et Willump"]
lol_words = [w for w in lol.split() if w not in {"Lee", "Sin", "Master", "Yi", "Miss", "Fortune", "Tahm", "Kench", "Twisted", "Fate", "Xin", "Zhao", "Aurelion"}] + multi
lol_terms = """League of Legends|LoL|ranked|soloQ|flex|top lane|mid lane|bot lane|jungle|jungler|support|ADC|carry|lane|farm|last hit|wave
|freeze|slow push|fast push|back|recall|roam|gank|teamfight|split push|dragon|drake|Baron Nashor|Nashor|Héraut|Grubs|Atakhan|Elder|
nexus|inhibiteur|tour|ward|balise de contrôle|vision|flash|ignite|téléportation|smite|châtiment|exhaust|heal|barrière|purge|ultime|
build|stuff|objet mythique|runes|Conqueror|Electrocute|Lethality|tank|bruiser|assassin|mage|enchanteur|ff|surrender|gg|wp|ez|feed|
smurf|tilt|int|troll|LP|Fer|Bronze|Argent|Or|Platine|Émeraude|Diamant|Master|Grandmaster|Challenger|patch|nerf|buff|meta"""
valo = """Valorant|Brimstone|Viper|Omen|Killjoy|Cypher|Sova|Sage|Phoenix|Jett|Reyna|Raze|Breach|Skye|Yoru|Astra|KAY/O|Chamber|Neon|
Fade|Harbor|Gekko|Deadlock|Iso|Clove|Vyse|Tejo|Waylay|Ascent|Bind|Haven|Split|Icebox|Breeze|Fracture|Pearl|Lotus|Sunset|Abyss|
Corrode|Vandal|Phantom|Operator|Sheriff|Ghost|Classic|Spectre|Judge|Odin|Marshal|Outlaw|spike|defuse|plant|retake|entry|eco|
force buy|full buy|clutch|ace|headshot|one tap|spray|crosshair|eDPI|sensi|peek|jiggle peek|counter-strafe|smoke|flash|util|
duelliste|contrôleur|initiateur|sentinelle|Immortal|Radiant|Ascendant"""
music = """FL Studio|DAW|MAO|VST|VST3|ASIO|FL Studio ASIO|piano roll|channel rack|playlist|mixer|mixeur|pattern|step sequencer|
sidechain|808|kick|snare|caisse claire|hi-hat|charley|clap|open hat|perc|sub|bass|basse|lead|pad|pluck|arp|arpège|sample|loop|
preset|automation|bus|send|insert|master|mastering|mixage|mix|prod|beat|instru|drop|refrain|couplet|hook|bridge|intro|outro|break|
BPM|tempo|tonalité|gamme|accord|mineur|majeur|vu-mètre|LUFS|dB|dBFS|crête|true peak|limiteur|limiter|compresseur|compression|
ratio|threshold|attack|release|makeup gain|knee|saturation|distorsion|clipper|soft clip|EQ|égaliseur|filtre passe-haut|passe-bas|
reverb|réverbe|delay|chorus|flanger|phaser|stéréo|mono|panoramique|largeur|phase|transient|sustain|glue|parallèle|de-esser|
Fruity Limiter|Fruity Parametric EQ 2|Fruity Reeverb 2|Fruity Delay 3|Fruity Peak Controller|Gross Beat|Harmor|Sytrus|FLEX|
Vital|Serum|Massive|Kontakt|Omnisphere|Nexus|Spire|Sylenth|Ozone|Pro-Q|Pro-L|Pro-C|Saturn|Valhalla|OTT|RC-20|Auto-Tune|
Melodyne|Soundtoys|Decapitator|Kickstart|Trackspacer|Splice|trap|drill|boom bap|lo-fi|RnB|afro|house|phonk|Antelope|Zen Go|
Synergy Core|carte son|interface audio|monitoring|casque|enceintes|latence|buffer"""
video = """After Effects|Premiere Pro|DaVinci Resolve|CapCut|keyframe|clé d'animation|graph editor|Easy Ease|easing|motion blur|
flou de mouvement|précomposition|precomp|composition|calque|null|expression|wiggle|loopOut|masque|rotoscopie|tracking|
stabilisation|étalonnage|LUT|color grading|vectorscope|forme d'onde|B-roll|rush|dérushage|J-cut|L-cut|jump cut|punch-in|
sous-titres|voix off|transition|fondu|glitch|shake|zoom|overshoot|rendu|export|H.264|H.265|bitrate|proxy|miniature|
Shorts|Reels|TikTok|YouTube|V2F|hum.studio|ExtendScript"""
autocad = """AutoCAD|AutoCAD Electrical|folio|projet wdp|numéro de fil|référence croisée|bornier|borne|contacteur|relais|disjoncteur|
sectionneur|différentiel|automate|API|PLC|entrées sorties|nomenclature|cartouche|ladder|échelle|AEWIRE|AEWIRENO|AECOMPONENT|
AELADDER|AEAUDIT|AEPROJUPDATE|Profinet|Modbus|Schneider|Siemens|Legrand|NF C 15-100|section de câble|phase|neutre|terre"""
pc = """Omnis|Discord|Spotify|Twitch|OBS|Windows|Steam|Riot|RX 9060 XT|Ryzen|Radeon|VRAM|RAM|GPU|CPU|FPS|ping|lag|stream|live|
VB-Cable|push-to-talk|soundboard|myinstants|Claude|Gemini|ChatGPT|Qwen|Whisper|mode expert|mode studio|mode écoute|mode léger|
mode entraînement|vu-mètre|compétence|rage bait|masterclass"""
slang = """wesh|frérot|frère|gros|zinzin|chelou|ouf|grave|trop|vénère|rageux|tryhard|noob|bot|carry|clutch|cringe|cheum|sah|
wallah|bref|genre|en vrai|askip|jsp|tkt|mdr|ptdr|gg|ez|nul|claqué|éclaté|frais|lourd|ça claque|ça tape|c'est carré|la patate|
le flow|une dinguerie|de ouf|tranquille|vas-y|ça marche|ok d'accord"""

def split(s): return [x.strip() for x in s.replace("\n", "").split("|") if x.strip()]


# --- Listes étendues (écrites à la main) -------------------------------------------------------------------------
plugins = """Fruity Compressor|Fruity Multiband Compressor|Fruity Soft Clipper|Fruity Blood Overdrive|Fruity Fast Dist|Fruity Squeeze
|Fruity Stereo Shaper|Fruity Stereo Enhancer|Fruity Balance|Fruity Filter|Fruity Free Filter|Fruity Flanger|Fruity Chorus|Fruity Phaser
|Fruity Vocoder|Fruity Love Philter|Fruity Convolver|Fruity WaveShaper|Fruity Notebook|Fruity Wrapper|Fruity Formula Controller
|Fruity Keyboard Controller|Fruity LFO|Fruity Envelope Controller|Fruity Granulizer|Fruity Slicer|Fruity Scratcher|Fruity DX10
|Fruity Kick|Fruity Dance|Fruity Slicer|Slicex|Edison|Patcher|Newtone|Newtime|Pitcher|Vocodex|Maximus|Transistor Bass|3x Osc|BooBass
|Toxic Biohazard|Poizone|Sakura|Morphine|Ogun|Sawer|Drumaxx|FPC|FL Keys|Directwave|Fruity Granulizer|Transient Processor|Frequency Shifter
|Frequency Splitter|Vintage Chorus|Vintage Phaser|Pitch Shifter|LuxeVerb|Fruity Limiter|Soundgoodizer|Hardcore|Emphasis|Multiband Delay
|Serum 2|Vital|Pigments|Phase Plant|Diva|Hive|Repro|Zebra|Sylenth1|Spire|Massive X|Kontakt|Komplete|Battery|Reaktor|Guitar Rig|Raum
|Replika|Omnisphere|Trilian|Keyscape|Nexus 5|Electra|Keyzone|Labs|Spitfire|Arturia|Analog Lab|Pigments|Mini V|Jup-8|CS-80|Addictive Drums
|Superior Drummer|EZdrummer|BFD|Ableton|Ableton Live|Logic Pro|Pro Tools|Cubase|Studio One|Reaper|Bitwig|Reason|GarageBand|Audacity
|FabFilter|Pro-Q 3|Pro-Q 4|Pro-L 2|Pro-C 2|Pro-MB|Pro-R|Pro-DS|Saturn 2|Timeless|Volcano|Twin|Simplon|Weiss|iZotope|Ozone 11|Neutron
|Nectar|RX|Trash|Vinyl|Waves|CLA-76|CLA-2A|SSL G-Master|H-Delay|H-Reverb|RVox|Renaissance|L1|L2|L3|Abbey Road|J37|Kramer|Puigtec
|SoundToys|EchoBoy|Decapitator|Little AlterBoy|Crystallizer|PrimalTap|MicroShift|Devil-Loc|Valhalla VintageVerb|Valhalla Supermassive
|Valhalla Room|Valhalla Delay|Valhalla Shimmer|Xfer|Serum FX|OTT|Kickstart 2|Cableguys|ShaperBox|VolumeShaper|LFOTool|Gross Beat|XLN|RC-20
|Life|XO|Addictive Keys|Output|Portal|Movement|Arcade|Exhale|Thermal|Auto-Tune Pro|Auto-Tune Access|Antares|Graillon|Little AlterBoy
|Melodyne|Waves Tune|Celemony|Soothe2|oeksound|Gullfoss|Trackspacer|Wavesfactory|Sausage Fattener|Dada Life|Kilohearts|Snap Heap
|Disperser|Plugin Alliance|bx_digital|Brainworx|Black Box|Shadow Hills|Lindell|Tokyo Dawn|TDR Nova|Kotelnikov|Slick EQ|Vladg|Voxengo SPAN
|SPAN|Youlean|Loudness Meter|Insight|Tonal Balance|Metric AB|ADPTR|Mastering The Mix|Levels|Reference|Splice|Tracklib|Loopmasters
|Cymatics|Kits.ai|Landr|BandLab|Native Instruments|Spectrasonics|Arturia|Universal Audio|UAD|Apollo|Focusrite|Scarlett|Antelope Audio
|Zen Go Synergy Core|Synergy Core|Universal Audio Volt|RME|Babyface|Audient|SSL 2|Behringer|Yamaha HS8|KRK|Adam Audio|Genelec|Beyerdynamic
|DT 770|DT 990|Audio-Technica|ATH-M50x|Sony MDR-7506|Shure SM7B|SM58|Rode NT1|Neumann U87|Akai MPK|Novation Launchkey|Arturia KeyLab|Maschine
|Push|Launchpad|MIDI|clavier maître|contrôleur MIDI|pad|potard|fader"""

music_more = """mélodie|contre-mélodie|harmonie|progression d'accords|renversement|septième|neuvième|accord mineur|accord majeur|quinte|tierce
|octave|demi-ton|ton|gamme mineure|gamme majeure|mode dorien|mode phrygien|pentatonique|blues|chromatique|tonique|dominante|sous-dominante
|cadence|modulation|transposition|swing|groove|shuffle|triolet|syncope|mesure|temps|contretemps|noire|croche|double croche|ronde|blanche
|silence|métronome|quantize|quantification|humanize|vélocité|note|portamento|glide|legato|staccato|arpégiateur|séquenceur|step|stem|stems
|bounce|export|rendu|WAV|MP3|FLAC|24 bits|44,1 kHz|48 kHz|sample rate|bit depth|dither|normaliser|gain staging|headroom|marge|plancher
|bruit de fond|souffle|larsen|clic|craquement|ronflement|buzz|hum|fade in|fade out|crossfade|trim|cut|glue|bus de batterie|bus voix|aux
|retour|départ|envoi|parallèle|New York|sidechain compression|ducking|pumping|transitoire|enveloppe|ADSR|LFO|oscillateur|wavetable|FM
|soustractive|additive|granulaire|filtre|cutoff|résonance|drive|bitcrusher|downsampler|exciter|aural exciter|multibande|M/S|mid side
|imagerie|largeur stéréo|Haas|corrélation|phase inverse|polarité|mono compatibility|loudness war|true peak|sonie|LUFS intégrés
|plage dynamique|PLR|crest factor|limiteur brickwall|clipper doux|clip dur|tape|bande|lampe|à lampes|transistor|analogique|numérique
|vocal chain|chaîne vocale|prise de voix|cabine|booth|filtre anti-pop|pop filter|bonnette|réflexion|traitement acoustique|bass trap
|panneau absorbant|diffuseur|monitoring|retour casque|click|décompte|punch in|comping|doublage|ad-libs|backs|chœurs|harmonies vocales
|toplines|topline|pre-chorus|post-chorus|build-up|riser|downlifter|impact|sweep|white noise|reverse|reverse cymbal|vocal chop|chopped
|flip|sampling|crate digging|vinyle|loop kit|drum kit|one-shot|808 glide|slide|808 distordue|kick punchy|snare claquante|rimshot|shaker
|tambourin|cowbell|conga|bongo|tom|crash|ride|cymbale|charleston|hat ouvert|hat fermé|roll|hi-hat roll|triplets|bounce|flow|cadence rap"""

genres = """rap|rap français|trap|drill|drill UK|drill française|boom bap|cloud rap|plugg|pluggnb|rage|jerk|hyperpop|phonk|drift phonk
|memphis|lo-fi hip-hop|chill hop|jazz rap|RnB|R&B|neo soul|soul|funk|disco|house|deep house|tech house|afro house|amapiano|techno|trance
|drum and bass|DnB|jungle|dubstep|riddim|future bass|EDM|electro|synthwave|ambient|pop|K-pop|afrobeat|afrobeats|afro trap|dancehall|reggae
|reggaeton|zouk|kompa|shatta|baile funk|jersey club|Detroit|grime|garage|UK garage|2-step|breakbeat|rock|metal|punk|indie|folk|variété
|chanson française|musique classique|orchestral|cinématique|BO|bande originale|jazz|blues|gospel|country|bossa nova|samba|flamenco"""

instruments = """piano|piano électrique|Rhodes|Wurlitzer|orgue|synthé|synthétiseur|guitare|guitare électrique|guitare acoustique|basse électrique
|contrebasse|batterie|caisse claire|grosse caisse|violon|alto|violoncelle|cordes|cuivres|trompette|trombone|saxophone|flûte|clarinette
|harpe|cloches|glockenspiel|xylophone|marimba|vibraphone|kalimba|steel drum|sitar|koto|erhu|duduk|oud|accordéon|harmonica|ukulélé|banjo
|mandoline|pad|nappe|lead|pluck|bell|keys|chœur|voix|vocal|beatbox"""

rappeurs = """Ninho|Jul|PNL|SCH|Booba|Kaaris|Gazo|Tiakola|Werenoi|Damso|Nekfeu|Orelsan|Freeze Corleone|Laylow|Hamza|Dinos|Josman|Zola|Koba LaD
|Niska|Gims|Aya Nakamura|Tayc|Dadju|Soolking|Naps|Alonzo|Lacrim|Rohff|La Fouine|Kalash Criminel|Kalash|Leto|Sadek|Gradur|Heuss l'Enfoiré
|Maes|Hatik|Lomepal|Vald|Alpha Wann|Nepal|Luidji|Ziak|Kerchak|Zed|Guy2Bezbar|Kofs|Rim'K|Dosseh|Fianso|Sofiane|Kery James|Oxmo Puccino
|IAM|NTM|MC Solaar|Sinik|Youssoupha|Médine|Lefa|Black M|Maître Gims|Soprano|Vegedream|RK|Bosh|Timal|Landy|Plk|PLK|Djadja & Dinaz|Naza
|Keblack|Jok'Air|Ademo|N.O.S|MHD|Siboy|Wejdene|Franglish|Gambino|Zkr|Mister V|Rilès|Green Montana|Elams|Bramsito|Ninho|Kekra|Kanoé
|Hamza|SDM|Mig|Uzi|Kalash|1PLIKÉ140|Jolagreen23|Hugo TSR|Disiz|Kekra|La Feve|Khali|Zamdane|Rounhaa|Theodora|Ronisia|Naza|Imen Es|Yanns
|Drake|Travis Scott|Kendrick Lamar|Future|Metro Boomin|21 Savage|Lil Baby|Young Thug|Gunna|Playboi Carti|Lil Uzi Vert|Kanye West|Ye|J. Cole
|Eminem|Tyler, The Creator|A$AP Rocky|Don Toliver|Central Cee|Dave|Stormzy|Skepta|Headie One|Digga D|Pop Smoke|Fivio Foreign|Ice Spice
|Doja Cat|SZA|The Weeknd|Frank Ocean|Bad Bunny|Burna Boy|Wizkid|Rema|Tems|Asake|Ayra Starr|Pharrell|Timbaland|Murda Beatz|Wheezy
|Pi'erre Bourne|Southside|808 Mafia|Tay Keith|Nick Mira|Pyrex|Zaytoven|Mike Will|Boi-1da|Hit-Boy|Kosei|Pyroman|Katrina Squad|Holo|BBP"""

streamers = """Squeezie|Gotaga|Kameto|ZeratoR|Domingo|Antoine Daniel|Ponce|Mister MV|Etoiles|Locklear|Sardoche|Wakz|Doigby|LeBouseuh|Inoxtag
|Michou|AmineMaTue|Kamel|Maghla|Baghera Jones|Joueur du Grenier|Amixem|Mcfly et Carlito|Tiboinshape|Cyprien|Norman|Natoo|Lebouseuh|Rebeudeter
|Kotei|Laink|Terracid|Bichouu|Chowh1|Jirayalecochon|Xari|Fuze III|Gius|Aminematue|Zack Nani|Le Roi des Rats|Alderiate|Kamet0|Karmine Corp
|KC|Vitality|Team Vitality|Gentle Mates|Solary|BDS|Team Heretics|Fnatic|G2|T1|Faker|Caps|Rekkles|Upset|Saken|Cabochard|Targamas|Hantera
|Vladi|Canna|Yike|Caliste|Kaori|Kamet0|Kameto|Prime|LCK|LEC|LFL|Worlds|MSI|VCT|Masters|Champions|EWC|Esports World Cup|ZEvent|Twitch|Kick"""

ae_effects = """Lumetri Color|Curves|Courbes|Levels|Niveaux|Hue/Saturation|Teinte et saturation|Tint|Teinte|Fill|Remplissage|Glow|Lueur
|Gaussian Blur|Flou gaussien|Fast Box Blur|Flou rapide|Directional Blur|Flou directionnel|Radial Blur|Flou radial|Camera Lens Blur
|Drop Shadow|Ombre portée|Bevel Alpha|Stroke|Contour|Turbulent Displace|Déplacement turbulent|Wave Warp|Distorsion ondulatoire|Mesh Warp
|Bulge|Renflement|Twirl|Tourbillon|Displacement Map|Fractal Noise|Bruit fractal|Gradient Ramp|Dégradé|4-Color Gradient|Fill|Linear Wipe
|Balayage linéaire|Radial Wipe|Venetian Blinds|Block Dissolve|CC Light Sweep|CC Light Rays|CC Particle World|CC Radial Fast Blur
|CC Force Motion Blur|CC Toner|CC Bend It|CC Page Turn|CC Lens|CC Sphere|CC Split|CC Twister|CC Ball Action|Particle Playground
|Trapcode Particular|Particular|Trapcode Shine|Shine|Optical Flares|Element 3D|Saber|Deep Glow|Twitch|Shake|Motion Tile|Duplication
|Transform|Transformer|Offset|Décalage|Mirror|Miroir|Echo|Écho|Posterize Time|Time Displacement|Timewarp|Pixel Motion Blur|RSMB
|Warp Stabilizer|Stabilisation de déformation|3D Camera Tracker|Suivi de caméra 3D|Roto Brush|Pinceau Roto|Content-Aware Fill|Keylight
|Key Cleaner|Advanced Spill Suppressor|Linear Color Key|Luma Key|Matte Choker|Simple Choker|Set Matte|Track Matte|Alpha Matte
|Luma Matte|Adjustment Layer|Calque d'effets|Solid|Solide|Shape Layer|Calque de forme|Text Layer|Calque de texte|Null Object|Objet nul
|Camera|Caméra|Light|Lumière|Parenting|Parentage|Pick Whip|Graph Editor|Éditeur de graphique|Speed Graph|Value Graph|Roving
|Hold Keyframe|Clé maintenue|Bezier|Ease In|Ease Out|Easy Ease In|Easy Ease Out|Motion Sketch|Puppet Pin|Marionnette|Trim Paths
|Repeater|Merge Paths|Wiggle Paths|Zig Zag|Pucker & Bloat|Twist|Offset Paths|Round Corners|Range Selector|Animator|Text Animator
|Per-character 3D|Source Text|Essential Graphics|MOGRT|Dynamic Link|Media Encoder|Render Queue|File d'attente de rendu|ProRes|DNxHD
|Cineform|GPU|Mercury|Proxies|Preview|RAM Preview|Work Area|Zone de travail|Precompose|Collapse Transformations|Motion Blur
|Frame Blending|Time Remapping|Remappage temporel|Time Stretch|Freeze Frame|Loop|Expressions|valueAtTime|linear|ease|clamp|random
|seedRandom|loopIn|loopOut|time|index|thisComp|thisLayer|effect|Slider Control|Checkbox Control|Point Control|Angle Control|Layer Control
|Color Control|Dropdown Menu|Duik|Joysticks n Sliders|RubberHose|Motion Bro|Flow|Ease and Wizz|Overlord|Animation Composer|Mister Horse
|Red Giant|Universe|Magic Bullet Looks|Sapphire|BCC|Boris FX|Mocha|Mocha Pro|Cinema 4D|Blender|Element|Plexus|Stardust|Newton
|Lockdown|Twixtor|Topaz|Upscale|Davinci Fusion|Fusion|Fairlight|Color Page|Node|Nœud|Power Window|Qualifier|Magic Mask|Speed Warp
|Premiere Pro|Essential Sound|Remix|Auto Reframe|Scene Edit Detection|Morph Cut|Lumetri Scopes|Ultra Key|Nest|Imbrication"""

autocad_more = """LINE|LIGNE|CIRCLE|CERCLE|ARC|RECTANG|POLYLIGN|PLINE|TRIM|AJUSTER|EXTEND|PROLONGE|OFFSET|DECALER|MOVE|DEPLACER|COPY|COPIER
|ROTATE|ROTATION|SCALE|ECHELLE|MIRROR|MIROIR|FILLET|RACCORD|CHAMFER|CHANFREIN|EXPLODE|DECOMPOS|JOIN|JOINDRE|BREAK|COUPURE|ARRAY|RESEAU
|HATCH|HACHURES|BLOCK|BLOC|INSERT|INSERER|WBLOC|ATTDEF|ATTEDIT|LAYER|CALQUE|LINETYPE|TYPELIGNE|DIMLINEAR|COTATION|DIMSTYLE|MTEXT|TEXTMULT
|TEXT|TEXTE|TABLE|TABLEAU|XREF|XATTACHER|PURGE|AUDIT|RECOVER|REPRISE|PLOT|TRACEUR|PUBLISH|PUBLIER|LAYOUT|PRESENTATION|VIEWPORT|FENETRE
|ZOOM|PAN|UNDO|ANNULER|REDO|REFAIRE|OSNAP|ACCROBJ|ORTHO|GRID|GRILLE|SNAP|RESOL|UCS|SCU|PROPERTIES|PROPRIETES|MATCHPROP|COPIERPROP
|QSELECT|SELECTRAPIDE|DESIGNCENTER|TOOLPALETTES|SHEETSET|JEU DE FEUILLES|DWG|DXF|DWT|PDF|AEPROJECT|AEMULTIBUS|AEWIRETYPE|AEDELWIRE
|AETRIMWIRE|AESTRETCHWIRE|AEDELCOMPONENT|AECOPYCOMP|AEEDITCOMPONENT|AETAG|AEFIXTAG|AERETAG|AEXREF|AESOURCE|AEDESTINATION|AEPLCIO
|AEPLCDB|AETERMINAL|AETSE|AEJUMPER|AEPANEL|AEFOOTPRINT|AENAMEPLATE|AECABLE|AEWIREANNO|AESETUPWIRE|AEREPORT|AEBOM|AECATALOG|AEEDITLIB
|AESYMBUILDER|AEICONMENU|AEPROPERTIES|AESWAP|AECOMPONENTMULTI|AECIRCUITBUILDER|schéma unifilaire|schéma multifilaire|schéma de principe
|schéma de câblage|plan d'implantation|repère|tenant|aboutissant|potentiel|équipotentielle|commun|bobine|contact NO|contact NF
|contact à fermeture|contact à ouverture|bouton poussoir|arrêt d'urgence|voyant|sélecteur|fin de course|détecteur|capteur inductif
|capteur capacitif|photoélectrique|thermique|relais thermique|disjoncteur moteur|variateur de vitesse|démarreur|étoile-triangle
|transformateur|alimentation 24 V|bornier à ressort|bornier à vis|goulotte|rail DIN|presse-étoupe|câble multiconducteur|blindage
|mise à la terre|PE|neutre|L1|L2|L3|triphasé|monophasé|400 V|230 V|24 VDC|IP65|NF C 15-100|IEC 60617|IEC 81346|norme"""

games_more = """Counter-Strike 2|CS2|Mirage|Inferno|Dust 2|Nuke|Ancient|Anubis|Vertigo|Overpass|Train|AK-47|M4A4|M4A1-S|AWP|Deagle|Desert Eagle
|Glock|USP-S|P250|Tec-9|Five-SeveN|MP9|MAC-10|UMP|P90|Galil|FAMAS|SG 553|AUG|SSG 08|Scout|Zeus|molotov|incendiaire|fumigène|flashbang
|HE|grenade|utility|lineup|eco round|force|pistol round|bomb site|site A|site B|rotate|lurk|IGL|entry fragger|AWPer|support|anchor
|Fortnite|Battle Royale|build|zone build|Zero Build|edit|box fight|piece control|tilted|Tilted Towers|V-Bucks|Battle Pass|skin|emote
|Minecraft|Creeper|Enderman|Ender Dragon|Nether|End|redstone|diamant|netherite|Elytra|enchantement|mod|serveur|survie|créatif|hardcore
|GTA|GTA 5|GTA 6|GTA Online|Rockstar|Los Santos|Vice City|Rocket League|Apex Legends|Overwatch 2|Call of Duty|Warzone|Black Ops|Fifa
|EA FC|Ultimate Team|TFT|Teamfight Tactics|Arcane|Wild Rift|Legends of Runeterra|2XKO|Riot Games|Riot Client|Vanguard|Elden Ring
|Dark Souls|Sekiro|Zelda|Mario Kart|Smash Bros|Pokémon|Brawl Stars|Clash Royale|Clash of Clans|Among Us|Fall Guys|Rainbow Six Siege
|Escape from Tarkov|Rust|ARK|Palworld|Helldivers 2|Baldur's Gate 3|Cyberpunk 2077|The Witcher 3|Hollow Knight|Hades|Stardew Valley
|Steam|Epic Games|Battle.net|Xbox|PlayStation|PS5|Switch|manette|souris|clavier mécanique|tapis|casque gaming|micro|webcam|écran 240 Hz
|144 Hz|360 Hz|input lag|ping|latence|packet loss|FPS drop|stutter|freeze|crash|lag spike|DPI|polling rate|ranked|tryhard|smurf"""

pc_more = """carte graphique|carte mère|processeur|mémoire vive|SSD|NVMe|disque dur|alimentation|boîtier|ventirad|watercooling|AIO|pâte thermique
|overclock|undervolt|BIOS|UEFI|XMP|EXPO|Resizable BAR|SAM|Smart Access Memory|FSR|FSR 4|DLSS|frame generation|ray tracing|path tracing
|Adrenalin|AMD Software|pilote|driver|Vulkan|DirectX 12|DirectX 11|OpenGL|shader|cache shader|VSync|FreeSync|G-Sync|HDR|Anti-Lag|Radeon Chill
|Hyper-RX|Fluid Motion Frames|RX 9070 XT|RX 9060 XT|RX 7800 XT|RTX 5090|RTX 5080|RTX 5070|RTX 4060|Ryzen 5 7500F|Ryzen 7 9800X3D|Ryzen 9
|Intel Core|Core Ultra|DDR5|DDR4|6000 MT/s|CL30|PCIe 5.0|Windows 11|Windows 10|gestionnaire des tâches|panneau de configuration|paramètres
|pare-feu|Defender|antivirus|mise à jour|redémarrer|explorateur|bureau|barre des tâches|raccourci|Ctrl|Alt|Maj|Échap|Entrée|Suppr
|clic droit|glisser-déposer|presse-papiers|capture d'écran|Outil Capture|Xbox Game Bar|OBS Studio|Streamlabs|NVIDIA Broadcast|RTX Voice
|Krisp|Voicemeeter|VB-Audio|VB-Cable|câble virtuel|CABLE Input|CABLE Output|Mixage stéréo|périphérique par défaut|exclusif|WASAPI|ASIO4ALL
|FlexASIO|latence audio|buffer size|taille de tampon|échantillons|ms|IA|intelligence artificielle|LLM|modèle|token|prompt|GPU offload
|llama.cpp|Ollama|LM Studio|Hugging Face|GGUF|quantization|Q4_K_M|Q8_0|MoE|Qwen3.6|Gemma|Mistral|Llama|DeepSeek|GPT|Gemini|Claude|Copilot
|Midjourney|Stable Diffusion|Suno|Udio|ElevenLabs|Whisper|TTS|STT|clonage vocal|voix neuronale|Discord Nitro|serveur Discord|salon vocal
|salon textuel|rôle|bot Discord|mute|deafen|push to talk|soundboard|stream Discord|partage d'écran|Spotify Premium|playlist|Deezer|SoundCloud
|YouTube Music|Apple Music|TikTok|Instagram|Reels|Shorts|Snapchat|X|Twitter|Twitch Prime|abonné|sub|follow|raid|clip|VOD|chat|modo"""

slang_more = """zebi|wsh|bg|frère|frérot|le sang|mon reuf|reuf|re|reuch|daronne|daron|wesh alors|en sah|sur la vie de ma mère|vazy|vas-y
|tranquille|tranquilou|posé|chill|ça passe|ça passe crème|crème|c'est mort|c'est chaud|t'es chaud|grave|de fou|de ouf|zinzin|ouf
|chelou|relou|vénère|vener|rageux|seum|avoir le seum|tilté|tilt|cringe|gênant|malaise|la hess|galère|galérer|bader|bad trip|flemme
|j'ai la flemme|bosser|taffer|taf|thune|oseille|biff|maille|fric|bail|ce bail|le délire|délire|dar|dinguerie|une dinguerie|banger|frappe
|ça frappe|ça tape|ça claque|claqué|éclaté|éclaté au sol|nul à chier|guez|sah quel plaisir|askip|wallah|starfoullah|hamdoullah|inchallah
|tkt|t'inquiète|jsp|je sais pas|osef|on s'en fout|mdr|ptdr|lol|xptdr|gg|ggwp|ez|noob|nooby|bot|carry|carrier|smurf|feed|feeder|int
|griefer|troll|toxic|toxique|AFK|BRB|GG EZ|nerf|buff|OP|cheat|cheaté|broken|meta|tier list|S tier|clutch|ace|one shot|full life|low life
|backdoor|outplay|outplayed|diff|jungle diff|gap|skill issue|ratio|cope|based|sigma|aura|aura farming|mewing|rizz|slay|goat|le goat
|masterclass|propre|carré|net|frais|lourd|validé|approuvé|on valide|stylé|trop stylé|énorme|pépite|pépitos|qualité|dingue|incroyable"""

# Erreurs de transcription vues sur le terrain (journal du 18/11) ou typiques : ce que Whisper écrit -> ce qui a été dit.
fixes = {
    "fête studio": "FL Studio", "fait studio": "FL Studio", "elle studio": "FL Studio", "efel studio": "FL Studio", "f l studio": "FL Studio",
    "fl studio": "FL Studio", "azio": "ASIO", "asio": "ASIO", "a zio": "ASIO", "vu métier": "vu-mètre", "vu mètre": "vu-mètre",
    "vu meter": "vu-mètre", "vue mètre": "vu-mètre", "vu-métier": "vu-mètre", "le mod": "le mode", "mod d'entraînement": "mode d'entraînement",
    "mod d'entrainement": "mode d'entraînement", "mod fl": "mode FL", "mod écoute": "mode écoute", "mod expert": "mode expert",
    "side chain": "sidechain", "side-chain": "sidechain", "huit cent huit": "808", "huit-cent-huit": "808", "l'imiteur": "limiteur",
    "auto cad": "AutoCAD", "autocade": "AutoCAD", "after effect": "After Effects", "afteur effect": "After Effects", "première pro": "Premiere Pro",
    "da vinci": "DaVinci", "valo rante": "Valorant", "valorante": "Valorant", "omni": "Omnis", "omnisse": "Omnis",
    "homnis": "Omnis", "om nis": "Omnis", "ozone": "Ozone", "serrum": "Serum", "sérum": "Serum", "piano rôle": "piano roll",
    "channel rac": "channel rack", "chanel rack": "channel rack", "playliste": "playlist", "hi hat": "hi-hat", "aïe hat": "hi-hat",
    "yasso": "Yasuo", "yazuo": "Yasuo", "otte": "OTT", "o t t": "OTT", "assent": "Ascent", "a scent": "Ascent", "haiven": "Haven", "hey ven": "Haven", "ice box": "Icebox", "aïce box": "Icebox",
    "jet t": "Jett", "rayna": "Reyna", "kill joy": "Killjoy", "kay o": "KAY/O", "chamb'r": "Chamber",
    "décibelles": "décibels", "décibelle": "décibel", "masteur": "master", "masterring": "mastering",
}

def api_words():
    """Données officielles : LoL (Riot Data Dragon, fr_FR et en_US) et Valorant (valorant-api.com, fr-FR et en-US)."""
    out = []
    if not API: return out
    for L in ("fr_FR", "en_US"):
        champ = json.load(open(f"{API}/champ_{L}.json", encoding="utf-8"))["data"]
        for c in champ.values():
            out.append(c["name"])
            out.append(c["passive"]["name"])
            out += [sp["name"] for sp in c["spells"]]
            out += [sk["name"] for sk in c.get("skins", []) if sk.get("name") and sk["name"] != "default"]
        items = json.load(open(f"{API}/item_{L}.json", encoding="utf-8"))["data"]
        out += [it["name"] for it in items.values() if it.get("maps", {}).get("11") and it.get("gold", {}).get("purchasable")]
        for tree in json.load(open(f"{API}/runes_{L}.json", encoding="utf-8")):
            out.append(tree["name"])
            out += [r["name"] for slot in tree["slots"] for r in slot["runes"]]
        out += [s_["name"] for s_ in json.load(open(f"{API}/summ_{L}.json", encoding="utf-8"))["data"].values()]
    for L in ("fr-FR", "en-US"):
        for a in json.load(open(f"{API}/valo_agents_{L}.json", encoding="utf-8"))["data"]:
            if not a.get("isPlayableCharacter"): continue
            out.append(a["displayName"]); out.append(a["role"]["displayName"] if a.get("role") else "")
            out += [ab["displayName"] for ab in a["abilities"]]
        out += [m["displayName"] for m in json.load(open(f"{API}/valo_maps_{L}.json", encoding="utf-8"))["data"]]
        for w in json.load(open(f"{API}/valo_weapons_{L}.json", encoding="utf-8"))["data"]:
            out.append(w["displayName"])
            out += [sk["displayName"] for sk in w["skins"]]
        out += [g["displayName"] for g in json.load(open(f"{API}/valo_gear_{L}.json", encoding="utf-8"))["data"]]
    clean = []
    for w in out:
        w = re.sub(r"<[^>]+>", "", w or "").strip()
        w = w.replace("\xa0", " ")
        # Variantes de couleur des skins (« Ashe impératrice lunaire (turquoise) ») : doublons sans intérêt à l'oral.
        if "(" in w or ")" in w: continue
        if 2 <= len(w) <= 40 and "Standard" not in w and "Random" not in w and "Aléatoire" not in w: clean.append(w)
    return clean


def dedupe(words):
    seen, out = set(), []
    for w in words:
        k = w.lower()
        if k not in seen: seen.add(k); out.append(w)
    return out


# Ordre : les mots essentiels d'abord (Whisper ne reçoit que les ~45 premiers du domaine actif).
domains = {
    "jeu": dedupe(split(lol_terms) + lol_words + split(valo) + api_words() + split(games_more)),
    "musique": dedupe(split(music) + split(plugins) + split(music_more) + split(genres) + split(instruments) + split(rappeurs)),
    "montage": dedupe(split(video) + split(ae_effects)),
    "electrique": dedupe(split(autocad) + split(autocad_more)),
    "pc": dedupe(split(pc) + split(pc_more) + split(streamers)),
    "argot": dedupe(split(slang) + split(slang_more)),
}
data = {"version": 2, "domains": domains, "fixes": fixes,
        "note": "Banque de mots : guide Whisper (vocabulaire du moment) et corrige ce qui sonne pareil (SpeechFix)."}
json.dump(data, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print({k: len(v) for k, v in domains.items()}, "corrections :", len(fixes), "total :", sum(len(v) for v in domains.values()))
