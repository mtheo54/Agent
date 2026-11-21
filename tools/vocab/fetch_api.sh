#!/bin/sh
# Données officielles pour la banque de mots : sh tools/vocab/fetch_api.sh dossier && python3 tools/vocab/make_vocab.py dossier
set -e
D="$1"; mkdir -p "$D"; cd "$D"
V=$(curl -sS https://ddragon.leagueoflegends.com/api/versions.json | python3 -c "import json,sys;print(json.load(sys.stdin)[0])")
for L in fr_FR en_US; do
  for F in championFull:champ item:item runesReforged:runes summoner:summ; do
    curl -sS -o "${F#*:}_$L.json" "https://ddragon.leagueoflegends.com/cdn/$V/data/$L/${F%%:*}.json"
  done
done
for L in fr-FR en-US; do for E in agents maps weapons gear; do curl -sS -o "valo_${E}_$L.json" "https://valorant-api.com/v1/$E?language=$L"; done; done
echo "LoL $V + Valorant : OK"
