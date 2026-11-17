# Voix d'Omnis — origine et licences

- **Voix de référence (`ref`, empreinte vocale)** : locuteur francophone natif `928` du corpus
  **CML-TTS** (Oliveira et al., 2023, licence **CC-BY 4.0**), issu d'enregistrements LibriVox (domaine
  public). Extrait `928_486_000075-0001`, version nettoyée (ai-coustics) publiée dans le dépôt
  `kyutai/tts-voices` (Kyutai, Paris).
- **Prises par émotion** (`normal`, `bright`, `soft`, `calm`, `amused`, `surprised`, `annoyed`) : générées à
  partir de cette voix avec **Qwen3-TTS 0.6B Base** (Alibaba, Apache 2.0) via qwentts.cpp (MIT), plusieurs
  prises par émotion, la plus marquée retenue automatiquement (accent, identité, relief).
- **Rires, petits rires, toux, raclements de gorge, soupirs** (`vocal/laugh_*`, `chuckle_*`, `cough_*`,
  `throat_*`, `sigh_*`) : vrais enregistrements d'hommes du corpus **VocalSound** (Gong, Yu & Glass,
  ICASSP 2022, MIT CSAIL), licence **CC BY-SA 4.0** ; découpés, filtrés (passe-haut 70 Hz) et mis au niveau
  de la voix. Ces fichiers modifiés restent sous CC BY-SA 4.0.
- `vocal/hmm_*`, `vocal/gasp_*` : générés avec la voix d'Omnis (Qwen3-TTS).

Pourquoi une voix native : la première voix d'Omnis avait été *inventée* par description (Qwen3-TTS
VoiceDesign) et gardait un accent anglophone (7 % « anglais » à la détection de langue, contre 0,05 %
pour la voix actuelle). Le clonage reproduit l'accent de la référence.

Pour lui donner une autre voix : remplace ces fichiers (WAV 24 kHz mono + transcription exacte dans le
`.txt` du même nom). Ne clone jamais la voix de quelqu'un sans son accord.
