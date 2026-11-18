#pragma once
// RageBait : écrire UNE bonne vanne au lieu de la première qui sort.
//
// Ce qui fait rire (psychologie de l'humour) : la SURPRISE (une chute prévisible tombe à plat), la CONNIVENCE
// (un rappel de ce que la personne a dit ou raté), l'absurde, et une cible « gratuite » qui ne blesse pas vraiment.
// Testé sur Omnis : une consigne seule donne des vannes molles, des tics (« mec », « l'aura ») et des exemples
// recopiés mot pour mot. Donc :
//   1) 3 candidates, chacune forcée sur une TECHNIQUE différente (tirées au hasard parmi 8) ;
//   2) des exemples pris dans d'AUTRES domaines (cuisine, sport, voiture) : le ton passe, la phrase ne se recopie pas ;
//   3) un tri : vanne refusée si elle franchit la ligne, bonus si elle reprend SES mots, malus pour les tics et la
//      longueur ; puis le modèle choisit la plus drôle parmi les meilleures (une réponse : 1, 2 ou 3).

#include <cstdint>
#include <functional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace companion {

class LocalBrain;

// Sujets d'entraînement « rage bait » (semés au premier call, travaillés en regardant des vidéos).
inline constexpr const char* kRageBaitTopics[] = {
    "l'art du rage bait : provoquer une réaction en restant drôle", "rhétorique de la provocation et du contre-pied",
    "timing et chute d'une vanne à l'oral", "memes TikTok français du moment", "troll entre amis ou harcèlement : la limite"};
class BaitBank;

struct BaitRequest {
    std::string persona;      // PersonaPrompt()
    std::string context;      // CallHintText + fiche de la personne
    std::string ask;          // CallAskText (cible + sa phrase)
    std::string targetLine;   // ce qu'il vient de dire (pour la précision)
    std::set<std::string> avoid;      // sujets sensibles de la soirée
    std::vector<std::string> recent;  // dernières vannes d'Omnis (tics à éviter)
    std::vector<std::string> recentTechniques;  // techniques des 2 dernières vannes : pas de « fausse stat » en boucle
    bool hasHistory = false;  // on connaît des fails / phrases de lui (technique « rappel »)
    BaitBank* bank = nullptr;          // formes qui marchent (écrites à la main + apprises en vidéo)
    std::string preferTechnique;       // « réplique au robot » quand on se moque de lui, « pseudo » pour un renommage…
    int candidates = 3;
};

struct BaitResult {
    std::string text;               // vide = rien de propre, il se tait
    std::string technique;
    std::vector<std::string> tried;  // pour le journal / la démo : « [technique] phrase (score) »
};

// Technique n° i : nom + consigne. 8 techniques.
const char* BaitTechnique(int i, const char** howTo);
// Tics surutilisés par le modèle (mesurés dans les démos).
const std::vector<std::string>& BaitTics();
// Score d'une vanne (plus haut = mieux) : précision (ses mots), tics, longueur, ligne franchie (-1000).
int ScoreBait(const std::string& bait, const std::string& targetLine, const std::vector<std::string>& recent,
              const std::set<std::string>& avoid);

BaitResult WriteBait(LocalBrain& brain, const BaitRequest& req, std::uint32_t& rng, std::stop_token stop = {});

}  // namespace companion
