#pragma once
// WebResearcher : recherche web LOCALE (0 token Claude), sur une liste blanche de wikis MediaWiki.
//
//   question ─► choix du wiki (mots-clés) ─► api.php?list=search ─► api.php?action=parse
//            ─► HTML nettoyé en texte brut ─► passages les plus pertinents (score par mots-clés)
//
// Le texte récupéré est ensuite résumé par le modèle local (LocalBrain) et mis en cache
// dans MemoryDB : la même question sera traitée sans réseau la fois suivante.
// API MediaWiki publique et documentée : pas de scraping de moteur de recherche.

#include "core/Config.hpp"
#include "network/HttpClient.hpp"

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

struct WebDocument {
    std::string url;
    std::string title;
    std::string text;       // texte complet nettoyé (borné), mis en cache
    std::string excerpt;    // passages pertinents pour la question (~2500 caractères)
};

class WebResearcher {
public:
    WebResearcher(HttpClient& http, std::vector<WikiSource> sources) : http_(http), sources_(std::move(sources)) {}

    // Choisit la source dont les mots-clés correspondent le mieux ; sinon la dernière (repli général).
    [[nodiscard]] const WikiSource* PickSource(std::string_view query) const;

    std::vector<WebDocument> Search(const WikiSource& src, std::string_view query, int maxPages,
                                    std::stop_token stop);

    // Utilitaires (publics pour les tests)
    static std::string HtmlToText(std::string_view html);
    static std::string BestPassages(std::string_view text, std::string_view query, size_t maxChars);

private:
    HttpClient& http_;
    std::vector<WikiSource> sources_;
};

}  // namespace companion
