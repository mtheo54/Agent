#pragma once
// MixLexicon : le lexique de la demande d'OMNIS (Abrasion Labs, data/lexique_fr.txt, embarqué dans
// assets/omnis/). Reconnaît le jargon de beatmaker (« la basse elle bave », « c'est boueux », « ça siffle »,
// « manque de patate ») et dit ce que la personne veut : axe et sens (bas_medium +2 = trop de bas-médium).
// Sert à router vers l'analyse studio et à expliquer la demande au modèle local.

#include <filesystem>
#include <string>
#include <vector>

namespace companion {

struct LexiconHit {
    bool defect = false;      // « defaut » (une plainte) ou « qualite » (un souhait)
    std::string id;           // boueux, brillant, mou…
    std::string expression;   // ce qui a été reconnu dans la phrase
    std::string axes;         // « bas_medium +2, bas +1 »
    int words = 1;
};

class MixLexicon {
public:
    bool Load(const std::filesystem::path& file);
    [[nodiscard]] bool Loaded() const noexcept { return !entries_.empty(); }
    // Expressions reconnues, les plus longues d'abord, sans chevauchement.
    [[nodiscard]] std::vector<LexiconHit> Match(const std::string& text) const;
    // Vrai si la phrase parle vraiment de mix : une plainte (« ça bave », « c'est boueux ») ET un élément
    // musical cité (kick, 808, voix, hats, mix…) ou une expression de plusieurs mots. « grave » (très) ou
    // « bordel » (juron) seuls ne comptent pas.
    [[nodiscard]] static bool IsMixRequest(const std::vector<LexiconHit>& hits, const std::string& text);
    // Sujets du diagnostic de mix concernés par la demande (« ça bave » → sub, lowmono, mud). Vide = tout.
    [[nodiscard]] static std::vector<std::string> Topics(const std::vector<LexiconHit>& hits);
    // Résumé pour le contexte du modèle (vide si rien).
    [[nodiscard]] static std::string Describe(const std::vector<LexiconHit>& hits);

    static std::string Normalize(const std::string& utf8);  // minuscules, sans accents, ponctuation → espace

private:
    struct Entry {
        bool defect;
        std::string id, expression, axes;
        int words;
    };
    std::vector<Entry> entries_;  // triées par longueur décroissante
};

}  // namespace companion
