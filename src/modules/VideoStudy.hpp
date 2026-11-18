#pragma once
// VideoStudy : Omnis regarde des vidéos (YouTube, TikTok) pour apprendre l'art de la vanne et du rage bait.
//
//   1) Outils téléchargés une fois, depuis leurs dépôts officiels, VÉRIFIÉS (SHA-256 publié) avant d'être lancés :
//      yt-dlp (lit YouTube / TikTok) et deno (exigé par yt-dlp pour YouTube).
//   2) Quoi regarder : les liens que tu lui donnes (« étudie le lien copié », fichier to_watch.txt) — TikTok
//      compris —, puis des recherches YouTube (compilations de vannes, clashs, roasts, rage bait…).
//   3) Comprendre : titre + sous-titres (ou, sans sous-titres, l'audio décodé par Windows et transcrit par
//      Whisper, en local) + les commentaires les plus likés (la réaction, c'est la moitié du rage bait).
//   4) Retenir : le modèle écrit une fiche (technique, pourquoi ça marche) et 1 à 4 FORMES de vannes à trous,
//      filtrées (jamais l'origine, le physique…), ajoutées à la banque de vannes. La leçon va dans la mémoire.
//
// Tourne dans le processus d'auto-entraînement (priorité basse, tué dès que tu touches au PC). Aucune vidéo n'est
// gardée : l'audio temporaire est supprimé après transcription.

#include "core/BaitBank.hpp"
#include "modules/VideoParse.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace companion {

class HttpClient;
class LocalBrain;
class MemoryDB;

struct VideoStudyConfig {
    int perSession = 3;          // vidéos étudiées par séance d'entraînement
    int maxSeconds = 600;        // on saute les vidéos de plus de 10 min
    std::vector<std::string> queries{"meilleures vannes clash humour français", "rage bait compilation français",
                                     "roast battle français punchlines", "répartie mauvaise foi meilleurs moments",
                                     "chambrage entre potes vocal discord", "compilation tiktok vannes fr",
                                     "punchlines stand up français", "trashtalk gaming français", "vanne #shorts",
                                     "rage bait #shorts français"};
    // Découverte de vidéos publiques sur les réseaux (moteur de recherche, aucun compte utilisé).
    bool socialDiscovery = true;
    std::vector<std::string> socialQueries{"site:tiktok.com rage bait", "site:tiktok.com vanne humour", "site:tiktok.com clash punchline",
                                           "site:instagram.com/reel humour vanne", "site:instagram.com/reel rage bait",
                                           "site:youtube.com/shorts vanne"};
    std::filesystem::path whisperModel;  // vide = pas de transcription audio (sous-titres seulement)
};

class VideoStudy {
public:
    VideoStudy(HttpClient& http, std::filesystem::path dataDir, VideoStudyConfig cfg);

    bool EnsureTools(std::string& error);
    std::vector<VideoItem> Search(const std::string& query, int n);
    // Remplit le texte (sous-titres ou transcription) et les commentaires. Faux si rien d'exploitable.
    bool Fetch(const std::string& url, std::string& title, std::string& transcript, std::string& comments, std::string& error);

    // Séance : vidéos de la file puis recherches ; leçons dans la base, formes dans la banque. Rapport court.
    std::string RunSession(LocalBrain& brain, MemoryDB& db, BaitBank& bank, std::stop_token stop);

    // Ajoute un lien à la file (YouTube / TikTok seulement). Faux si refusé.
    static bool QueueLink(const std::filesystem::path& dataDir, const std::string& url);

private:
    HttpClient& http_;
    std::filesystem::path dir_, tools_;
    VideoStudyConfig cfg_;
    std::filesystem::path ytdlp_, deno_;

    int Run(const std::vector<std::wstring>& args, std::string& out, int timeoutSec, std::stop_token stop = {});
    std::string Transcribe(const std::filesystem::path& audio);
};

}  // namespace companion
