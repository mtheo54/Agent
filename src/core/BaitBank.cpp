#include "core/BaitBank.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace companion {
namespace {

std::string Key(const std::string& s) {  // pour dédoublonner : lettres et chiffres seulement, en minuscules
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum(c) || c >= 0x80) k += static_cast<char>(std::tolower(c));
    return k;
}

std::uint32_t Next(std::uint32_t& r) {
    r = r * 1664525u + 1013904223u;
    return r >> 8;
}

void ReadFile(const std::filesystem::path& file, bool learned, std::vector<BaitForm>& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    try {
        const auto j = nlohmann::json::parse(f, nullptr, true, true);
        for (const auto& b : j.value("baits", nlohmann::json::array())) {
            BaitForm x;
            x.technique = b.value("t", std::string{});
            x.form = b.value("f", std::string{});
            x.source = b.value("src", std::string{});
            x.learned = learned;
            if (!x.form.empty() && x.form.size() < 300) out.push_back(std::move(x));
        }
    } catch (const std::exception&) {
    }
}

}  // namespace

bool BaitBank::Load(const std::filesystem::path& handWritten, const std::filesystem::path& learned) {
    forms_.clear();
    learnedFile_ = learned;
    ReadFile(handWritten, false, forms_);
    ReadFile(learned, true, forms_);
    return !forms_.empty();
}

std::size_t BaitBank::LearnedCount() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(forms_, [](const BaitForm& f) { return f.learned; }));
}

std::optional<BaitForm> BaitBank::Pick(const std::string& preferTechnique, std::uint32_t& rng) {
    if (forms_.empty()) return std::nullopt;
    std::vector<std::size_t> pool;
    for (std::size_t i = 0; i < forms_.size(); ++i)
        if (forms_[i].technique == preferTechnique && std::ranges::find(recent_, i) == recent_.end()) pool.push_back(i);
    if (pool.empty())
        for (std::size_t i = 0; i < forms_.size(); ++i)
            if (std::ranges::find(recent_, i) == recent_.end()) pool.push_back(i);
    if (pool.empty()) {
        recent_.clear();
        for (std::size_t i = 0; i < forms_.size(); ++i) pool.push_back(i);
    }
    const std::size_t k = pool[Next(rng) % pool.size()];
    recent_.push_back(k);
    while (recent_.size() > std::min<std::size_t>(40, forms_.size() / 2)) recent_.pop_front();
    return forms_[k];
}

int BaitBank::AddLearned(const std::vector<BaitForm>& forms) {
    int added = 0;
    for (const auto& f : forms) {
        if (f.form.size() < 12 || f.form.size() > 220) continue;
        const std::string k = Key(f.form);
        if (std::ranges::any_of(forms_, [&](const BaitForm& x) { return Key(x.form) == k; })) continue;
        BaitForm x = f;
        x.learned = true;
        forms_.push_back(std::move(x));
        ++added;
    }
    // 300 formes apprises au plus : les plus anciennes partent.
    while (LearnedCount() > 300)
        forms_.erase(std::ranges::find_if(forms_, [](const BaitForm& x) { return x.learned; }));
    if (added) SaveLearned();
    return added;
}

bool BaitBank::SaveLearned() const {
    if (learnedFile_.empty()) return false;
    nlohmann::json j;
    j["baits"] = nlohmann::json::array();
    for (const auto& f : forms_)
        if (f.learned) j["baits"].push_back({{"t", f.technique}, {"f", f.form}, {"src", f.source}});
    std::error_code ec;
    std::filesystem::create_directories(learnedFile_.parent_path(), ec);
    auto tmp = learnedFile_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(1);
    }
    std::filesystem::rename(tmp, learnedFile_, ec);
    return !ec;
}

}  // namespace companion
