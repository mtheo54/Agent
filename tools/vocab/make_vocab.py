# Banque de mots d'Omnis (assets/knowledge/vocab.json) : ce que Whisper doit savoir écrire, par domaine, et les
# erreurs de transcription déjà vues sur le terrain. Régénérer : python3 tools/vocab/make_vocab.py
import json, os
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "knowledge", "vocab.json")

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

domains = {
    "jeu": split(lol_terms) + lol_words + split(valo),
    "musique": split(music),
    "montage": split(video),
    "electrique": split(autocad),
    "pc": split(pc),
    "argot": split(slang),
}
data = {"version": 1, "domains": domains, "fixes": fixes,
        "note": "Banque de mots : guide Whisper (vocabulaire du moment) et corrige ce qui sonne pareil (SpeechFix)."}
json.dump(data, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print({k: len(v) for k, v in domains.items()}, "corrections :", len(fixes), "total :", sum(len(v) for v in domains.values()))
