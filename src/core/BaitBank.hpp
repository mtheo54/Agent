#pragma once
// BaitBank : des FORMES de vannes qui marchent, à adapter à la phrase du pote.
//   - assets/persona/baits.json : écrites à la main (règle de trois, fausse piste, compliment empoisonné…) ;
//   - %LOCALAPPDATA%\CompanionAI\learned_baits.json : apprises en regardant des vidéos (VideoStudy), 300 au plus.
// Une forme déjà servie ne revient pas avant d'avoir épuisé les autres de la même technique.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace companion {

struct BaitForm {
    std::string technique;
    std::string form;    // avec des [crochets] à remplacer
    bool learned = false;
    std::string source;  // vidéo d'où elle vient (formes apprises)
};

class BaitBank {
public:
    bool Load(const std::filesystem::path& handWritten, const std::filesystem::path& learned);
    // Une forme au hasard (de cette technique si possible), jamais une des dernières servies.
    std::optional<BaitForm> Pick(const std::string& preferTechnique, std::uint32_t& rng);
    // Ajoute des formes apprises (dédoublonnées), puis enregistre. Retourne le nombre ajouté.
    int AddLearned(const std::vector<BaitForm>& forms);
    [[nodiscard]] std::size_t Size() const noexcept { return forms_.size(); }
    [[nodiscard]] std::size_t LearnedCount() const noexcept;

private:
    std::vector<BaitForm> forms_;
    std::deque<std::size_t> recent_;
    std::filesystem::path learnedFile_;
    bool SaveLearned() const;
};

}  // namespace companion
