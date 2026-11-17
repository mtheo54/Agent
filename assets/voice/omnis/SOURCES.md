# Voix d'Omnis — origine et licences

- **Voix de référence (`ref`, empreinte vocale)** : locuteur francophone natif `928` du corpus
  **CML-TTS** (Oliveira et al., 2023, licence **CC-BY 4.0**), issu d'enregistrements LibriVox (domaine
  public). Extrait `928_486_000075-0001`, version nettoyée (ai-coustics) publiée dans le dépôt
  `kyutai/tts-voices` (Kyutai, Paris).
- **Prises par émotion** (`normal`, `bright`, `soft`, `calm`, `amused`, `surprised`) et **sons humains**
  (`vocal/` : rires, soupirs, toux…) : générés à partir de cette voix avec **Qwen3-TTS 0.6B Base**
  (Alibaba, Apache 2.0) via qwentts.cpp (MIT), puis triés automatiquement (accent, identité, caractère).

Pourquoi une voix native : la première voix d'Omnis avait été *inventée* par description (Qwen3-TTS
VoiceDesign) et gardait un accent anglophone (7 % « anglais » à la détection de langue, contre 0,05 %
pour la voix actuelle). Le clonage reproduit l'accent de la référence.

Pour lui donner une autre voix : remplace ces fichiers (WAV 24 kHz mono + transcription exacte dans le
`.txt` du même nom). Ne clone jamais la voix de quelqu'un sans son accord.
