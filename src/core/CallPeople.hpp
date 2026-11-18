#pragma once
// CallPeople : une fiche par personne entendue dans tes calls, gardée sur CE PC seulement
// (%LOCALAPPDATA%\CompanionAI\call_people.json).
//
//   - empreinte de voix (nombres, jamais de son), prénom s'il l'a appris (« moi c'est Karim »,
//     ou toi : « le dernier qui a parlé c'est Karim »), « Voix 2 » sinon ;
//   - quelques phrases récentes (matière à vannes précises), ses moqueries envers Omnis ;
//   - les SUJETS À ÉVITER (deuil, santé, rupture…) : seulement la catégorie, jamais le détail.
//
// N'importe qui peut dire « Omnis, oublie-moi » : sa fiche est effacée sur-le-champ.
// Logique pure (JSON + fichier) : testée hors Windows.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace companion {

struct CallPerson {
    std::string id;    // « p3 »
    std::string name;  // vide = inconnu
    // Jusqu'à 5 empreintes (micro qui change, voix fatiguée, cri…), apprises dans les calls eux-mêmes.
    std::vector<std::vector<float>> voices;
    std::vector<int> counts;
    int samples = 0;
    [[nodiscard]] float Score(const std::vector<float>& emb) const;
    std::deque<std::string> said;    // dernières phrases (non sensibles)
    std::deque<std::string> mocks;   // ses moqueries envers Omnis
    std::vector<std::string> avoid;  // catégories sensibles : ne pas en parler
    std::int64_t lastSeen = 0;
    [[nodiscard]] std::string Label() const;  // prénom, sinon « Voix 3 »
};

class CallPeople {
public:
    struct Match {
        CallPerson* person = nullptr;
        float score = 0.f;
        bool sure = false;    // c'est lui (seuil + marge sur le 2e)
        bool strong = false;  // assez sûr pour lui attribuer une moquerie
        bool fromDiscord = false;  // nom lu sur Discord (cercle vert), pas deviné à la voix
    };

    bool Load(const std::filesystem::path& file);
    bool Save() const;

    // Qui parle ? Met à jour l'empreinte si c'est sûr ; crée une fiche si c'est clairement une nouvelle voix
    // et que la phrase est assez longue (allowCreate).
    Match Identify(const std::vector<float>& emb, bool allowCreate, std::int64_t now);
    // Discord a montré QUI parlait pendant la phrase (cercle vert + pseudo) : on apprend sa voix, sans deviner.
    Match Enroll(const std::vector<float>& emb, const std::string& name, std::int64_t now);
    CallPerson* ById(const std::string& id);
    CallPerson* ByName(const std::string& name);
    bool Forget(const std::string& id);
    void Rename(CallPerson& p, std::string name);
    static void Remember(CallPerson& p, std::string line);
    static void Mocked(CallPerson& p, std::string line);
    static void Avoid(CallPerson& p, const std::string& category);
    // Résumé pour le modèle : prénom, ce qu'il a dit, ses moqueries, ce dont il ne faut PAS parler.
    [[nodiscard]] static std::string Describe(const CallPerson& p);
    [[nodiscard]] std::size_t Size() const noexcept { return people_.size(); }

private:
    std::filesystem::path file_;
    std::deque<CallPerson> people_;  // deque : les pointeurs restent valides quand on ajoute
    int nextId_ = 1;
    static void Learn(CallPerson& p, const std::vector<float>& emb);
    CallPerson& Create(const std::vector<float>& emb, std::int64_t now);
};

// --- Ce qu'on entend dans le call (logique pure) ---

// Sujet sensible dans la phrase ? -> catégorie (« un deuil », « sa santé »…), sinon rien.
std::optional<std::string> SensitiveTopic(const std::string& lower);
// Quelqu'un se moque d'Omnis ? (la phrase le nomme et le rabaisse)
bool MocksOmnis(const std::string& lower);
// On ne l'a pas entendu ? (« quoi ? », « on t'entend pas », « parle plus fort »…)
bool DidntHear(const std::string& lower);
// Quelqu'un fait du son (rap, prod, stream de DAW) ou de la musique joue : il ne dérange pas.
bool MakingMusic(const std::string& lower);
// « moi c'est Karim », « je m'appelle Karim », « c'est Karim qui parle » -> « Karim ».
std::optional<std::string> SelfIntroduction(const std::string& text);

}  // namespace companion
