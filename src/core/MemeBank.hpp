#pragma once
// MemeBank : memes et argot du moment (assets/persona/memes.json, éditable sans recompiler).
// De temps en temps, 2-3 memes tirés au hasard sont proposés au modèle (« place-en un SI ça colle »),
// jamais les mêmes d'affilée, sans humour noir si l'utilisateur va mal.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace companion {

struct Meme {
    std::string text, meaning, example, lang;
    bool dark = false;
};

class MemeBank {
public:
    bool Load(const std::filesystem::path& json);
    [[nodiscard]] bool Empty() const noexcept { return memes_.empty(); }
    // Indice pour le contexte du modèle, ou vide (pas cette fois). chance : probabilité d'en proposer.
    std::string Hint(float chance, bool allowDark);

private:
    std::vector<Meme> memes_;
    std::deque<size_t> recent_;
    std::uint32_t rng_ = 0x1234567u;
    float Next() noexcept;
};

}  // namespace companion
