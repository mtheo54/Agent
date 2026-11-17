#pragma once
// Soundboard : bruitages et sons de memes (myinstants.com), téléchargés À LA DEMANDE sur le PC de
// l'utilisateur et gardés en cache local (%LOCALAPPDATA%\CompanionAI\soundboard). Rien n'est livré avec
// le programme : beaucoup de ces sons sont des extraits protégés, on ne les redistribue pas.
//
//  - Fetch("vine boom") : recherche sur myinstants, télécharge le 1er résultat (MP3, ≤ 8 Mo).
//  - Get("vine boom")   : son décodé (dr_mp3), mono 24 kHz, coupé à 7 s, au niveau de la voix ; cache mémoire.
//  - Les sons se placent dans une réplique avec la balise (son: vine boom).
// Le réseau passe par la PermissionGate de l'orchestrateur (hôte www.myinstants.com).

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace companion {

class HttpClient;

class Soundboard {
public:
    Soundboard(HttpClient& http, std::filesystem::path dir, int sampleRate);

    // Télécharge le premier résultat de la recherche ; renvoie le nom sous lequel il est rangé.
    std::optional<std::string> Fetch(const std::string& query, std::string* error = nullptr);
    // Son prêt à jouer (nullptr si inconnu ou illisible). Thread-safe.
    std::shared_ptr<const std::vector<float>> Get(const std::string& name);
    [[nodiscard]] std::vector<std::string> Names() const;
    [[nodiscard]] bool Has(const std::string& name) const;
    // Sons proposés par « installe les sons » (recherches myinstants).
    static const std::vector<std::string>& StarterPack();
    static std::string Key(const std::string& name);  // « Vine Boom ! » -> « vine boom »
    // Nom OU lien myinstants (…/instant/salut-cest-frank-leboeuf-complet-97159/) -> clé de rangement.
    static std::string KeyFor(const std::string& nameOrUrl);
    static bool IsUrl(const std::string& s) { return s.rfind("http", 0) == 0 && s.find("myinstants.com") != std::string::npos; }

    // Décodage + mise en forme (exposé pour les tests) : MP3 -> mono, rééchantillonné, coupé, normalisé.
    static std::vector<float> DecodeMp3(const std::vector<unsigned char>& mp3, int sampleRate);

private:
    HttpClient& http_;
    std::filesystem::path dir_;
    int rate_;
    mutable std::mutex mutex_;
    std::map<std::string, std::filesystem::path> files_;                  // nom -> fichier .mp3
    std::map<std::string, std::shared_ptr<const std::vector<float>>> pcm_;  // décodés (cache mémoire)
    void Scan();
};

// Parse une page de recherche myinstants : (chemin du mp3, titre) du premier résultat.
std::optional<std::pair<std::string, std::string>> ParseMyInstantsFirst(const std::string& html);
// Meilleur résultat pour une recherche : le 1er dont le titre contient tous les mots cherchés, sinon le 1er.
std::optional<std::pair<std::string, std::string>> ParseMyInstantsBest(const std::string& html, const std::string& query);

}  // namespace companion
