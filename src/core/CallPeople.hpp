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
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace companion {

struct CallPerson {
    std::string id;    // « p3 »
    std::string name;  // pseudo actuel (vide = inconnu) ; l'identité, c'est la VOIX, pas le pseudo
    std::deque<std::string> aliases;  // anciens pseudos (renommages pour rigoler)
    std::vector<std::uint8_t> avatar;  // empreinte de son avatar Discord
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
        std::string renamedFrom;   // il a changé de pseudo (ancien pseudo)
        std::string impostorOf;    // il a pris le pseudo de quelqu'un d'autre (sa voix le trahit)
    };

    bool Load(const std::filesystem::path& file);
    bool Save() const;
    // Deux fiches, une seule voix (renommage pas reconnu tout de suite) : on fusionne. Invalide les pointeurs
    // (CallPerson*) : à appeler hors d'un traitement de phrase (au chargement, à la sortie du call).
    void MergeDuplicates();

    // Qui parle ? Met à jour l'empreinte si c'est sûr ; crée une fiche si c'est clairement une nouvelle voix
    // et que la phrase est assez longue (allowCreate).
    Match Identify(const std::vector<float>& emb, bool allowCreate, std::int64_t now);
    // Discord a montré qui parlait (pseudo + avatar). Le pseudo peut être une blague : la voix tranche si elle est
    // nette, puis l'avatar (s'il n'est qu'à une personne), puis le pseudo. Apprend la voix sous la bonne identité.
    Match Resolve(const std::vector<float>* emb, const std::string& shownName, const std::vector<std::uint8_t>& avatar,
                  std::int64_t now);
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
    // Pseudo jamais vu : ses premières phrases attendent ici (4 au plus) avant de décider « renommage » ou « nouveau ».
    std::vector<std::pair<std::string, std::vector<std::vector<float>>>> pending_;
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
