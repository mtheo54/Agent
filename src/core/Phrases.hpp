#pragma once
// Phrases : banque de répliques du personnage (assets/persona/phrases.json), par situation et par ton.
// Une variante tirée au hasard, jamais deux fois la même d'affilée ; variables {peak}, {host}…
// Utilisée pour les répliques « niveau 0 » (alertes, confirmations) et les relances orales.

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace companion {

enum class Tone : int { Polite = 0, Buddy = 1, Unfiltered = 2 };
const char* ToKey(Tone t) noexcept;  // "poli", "pote", "sansFiltre"

class Phrases {
public:
    bool Load(const std::filesystem::path& json);
    void SetTone(Tone t) noexcept { tone_ = t; }
    [[nodiscard]] Tone GetTone() const noexcept { return tone_; }

    // Réplique pour la situation `key` au ton courant (repli : ton "pote", puis `fallback`).
    std::string Pick(const std::string& key, const std::map<std::string, std::string>& vars = {},
                     const std::string& fallback = {});
    // Toutes les variantes d'une situation au ton courant (relances à pré-synthétiser).
    std::vector<std::string> All(const std::string& key) const;

private:
    std::map<std::string, std::map<std::string, std::vector<std::string>>> bank_;
    std::map<std::string, size_t> last_;
    Tone tone_ = Tone::Unfiltered;
    unsigned rng_ = 0xC0FFEEu;
    mutable std::mutex mutex_;
};

}  // namespace companion
