#include "core/Phrases.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace companion {

const char* ToKey(Tone t) noexcept {
    switch (t) {
        case Tone::Polite: return "poli";
        case Tone::Buddy: return "pote";
        default: return "sansFiltre";
    }
}

bool Phrases::Load(const std::filesystem::path& json) {
    std::ifstream f(json, std::ios::binary);
    if (!f) {
        log::Info("Phrases : {} introuvable, répliques par défaut", log::Narrow(json.wstring()));
        return false;
    }
    try {
        const auto j = nlohmann::json::parse(f);
        std::scoped_lock lock(mutex_);
        for (const auto& [key, tones] : j.items()) {
            if (!tones.is_object()) continue;  // "_doc"
            for (const auto& [tone, list] : tones.items()) bank_[key][tone] = list.get<std::vector<std::string>>();
        }
    } catch (const std::exception& e) {
        log::Info("Phrases : fichier invalide ({})", e.what());
        return false;
    }
    return true;
}

std::vector<std::string> Phrases::All(const std::string& key) const {
    std::scoped_lock lock(mutex_);
    const auto it = bank_.find(key);
    if (it == bank_.end()) return {};
    for (const char* tone : {ToKey(tone_), "pote", "poli"})
        if (const auto t = it->second.find(tone); t != it->second.end() && !t->second.empty()) return t->second;
    return {};
}

std::string Phrases::Pick(const std::string& key, const std::map<std::string, std::string>& vars,
                          const std::string& fallback) {
    const auto list = All(key);
    std::string out = fallback;
    if (!list.empty()) {
        std::scoped_lock lock(mutex_);
        rng_ = rng_ * 1664525u + 1013904223u;
        size_t idx = (rng_ >> 8) % list.size();
        auto& last = last_[key];
        if (list.size() > 1 && idx == last) idx = (idx + 1) % list.size();
        last = idx;
        out = list[idx];
    }
    for (const auto& [name, value] : vars) {
        const std::string tag = "{" + name + "}";
        for (size_t p = out.find(tag); p != std::string::npos; p = out.find(tag, p + value.size()))
            out.replace(p, tag.size(), value);
    }
    return out;
}

}  // namespace companion
