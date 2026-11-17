#include "core/MemeBank.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace companion {

float MemeBank::Next() noexcept {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.f;
}

bool MemeBank::Load(const std::filesystem::path& json) {
    memes_.clear();
    std::ifstream f(json, std::ios::binary);
    if (!f) return false;
    try {
        const auto j = nlohmann::json::parse(f, nullptr, true, /*ignore_comments=*/true);
        for (const auto& m : j.value("memes", nlohmann::json::array()))
            memes_.push_back({m.value("texte", ""), m.value("sens", ""), m.value("exemple", ""), m.value("langue", "fr"),
                              m.value("sombre", false)});
    } catch (const std::exception&) {
        memes_.clear();
        return false;
    }
    std::erase_if(memes_, [](const Meme& m) { return m.text.empty(); });
    rng_ ^= static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return !memes_.empty();
}

std::string MemeBank::Hint(float chance, bool allowDark) {
    if (memes_.empty() || Next() >= chance) return {};
    // 2 en français + 1 en anglais en moyenne (« français principalement »), jamais un meme des 12 derniers.
    std::vector<size_t> picked;
    for (int attempt = 0; attempt < 60 && picked.size() < 3; ++attempt) {
        const size_t i = static_cast<size_t>(Next() * static_cast<float>(memes_.size())) % memes_.size();
        const auto& m = memes_[i];
        if ((!allowDark && m.dark) || std::ranges::find(recent_, i) != recent_.end() ||
            std::ranges::find(picked, i) != picked.end())
            continue;
        const bool english = m.lang == "en";
        const auto nEn = std::ranges::count_if(picked, [&](size_t k) { return memes_[k].lang == "en"; });
        if (english && nEn >= 1) continue;
        picked.push_back(i);
    }
    if (picked.empty()) return {};
    std::string s = "[Memes du moment : places-en UN seul si ça colle vraiment à la situation, sinon ignore]";
    for (size_t i : picked) {
        const auto& m = memes_[i];
        s += " « " + m.text + " » = " + m.meaning + " (ex : " + m.example + ") ;";
        recent_.push_back(i);
        while (recent_.size() > 12) recent_.pop_front();
    }
    s.back() = '\n';
    return s;
}

}  // namespace companion
