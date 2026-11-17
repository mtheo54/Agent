#include "audio/Expressivity.hpp"

#include "audio/NeuralVoice.hpp"  // ReadWavMono

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace companion {

const char* ToFileStem(Vocal v) noexcept {
    switch (v) {
        case Vocal::Laugh: return "laugh";
        case Vocal::Chuckle: return "chuckle";
        case Vocal::Sigh: return "sigh";
        case Vocal::Hmm: return "hmm";
        case Vocal::Cough: return "cough";
        case Vocal::Gasp: return "gasp";
        case Vocal::Throat: return "throat";
        default: return "glitch";
    }
}

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Mot de la balise → son. Renvoie Count si c'est une didascalie inconnue (retirée sans bruit).
Vocal Classify(const std::string& raw) {
    const std::string t = Lower(raw);
    const auto has = [&](std::initializer_list<const char*> w) {
        for (const char* x : w) if (t.find(x) != std::string::npos) return true;
        return false;
    };
    if (has({"petit rire", "rigole", "ricane", "sourit", "sourire", "hé hé", "he he", "pouffe"})) return Vocal::Chuckle;
    if (has({"rire", "rit", "ha ha", "haha", "mdr", "explose"})) return Vocal::Laugh;
    if (has({"soupir", "pff", "souffle", "blasé"})) return Vocal::Sigh;
    if (has({"racle", "gorge", "ahem", "hum hum", "s'éclaircit", "eclaircit"})) return Vocal::Throat;
    if (has({"tousse", "toux", "kof"})) return Vocal::Cough;
    if (has({"bug", "glitch", "bzz", "grésille", "gresille", "beug", "plante"})) return Vocal::Glitch;
    if (has({"hmm", "réfléchit", "reflechit", "hésite", "hesite"})) return Vocal::Hmm;
    if (has({"surpris", "oh", "inspire", "sursaute", "choqué"})) return Vocal::Gasp;
    return Vocal::Count;
}

}  // namespace

std::vector<SpeechPart> ParseExpressiveText(const std::string& text) {
    // (rire) [rire] *rire* — courtes didascalies seulement (≤ 30 caractères), pour ne pas manger une
    // vraie parenthèse de la réponse.
    static const std::regex tag(R"(\(([^()\n]{1,30})\)|\[([^\[\]\n]{1,30})\]|\*([^*\n]{1,30})\*)");
    std::vector<SpeechPart> parts;
    const auto pushText = [&](std::string s) {
        const auto a = s.find_first_not_of(" \t\n,");
        if (a == std::string::npos) return;
        s = s.substr(a);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        if (s.find_first_not_of(".!?… ") == std::string::npos) return;  // ponctuation seule
        if (!parts.empty() && !parts.back().isVocal) parts.back().text += " " + s;
        else parts.push_back({false, Vocal::Laugh, s});
    };
    size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), tag); it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const std::string inner = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
        const Vocal v = Classify(inner);
        // Parenthèse ordinaire (pas une didascalie connue) : on la garde telle quelle. *action* ou [action]
        // inconnues sont retirées (le modèle invente parfois des didascalies).
        if (v == Vocal::Count && m[1].matched) continue;
        pushText(text.substr(last, static_cast<size_t>(m.position(0)) - last));
        if (v != Vocal::Count) parts.push_back({true, v, {}});
        last = static_cast<size_t>(m.position(0) + m.length(0));
    }
    pushText(text.substr(last));
    return parts;
}

bool VocalBank::Load(const std::filesystem::path& dir, int sampleRate) {
    bool any = false;
    for (size_t v = 0; v < clips_.size(); ++v) {
        clips_[v].clear();
        for (int n = 0; n < 16; ++n) {
            std::vector<float> pcm;
            int rate = 0;
            const auto file = dir / (std::string(ToFileStem(static_cast<Vocal>(v))) + "_" + std::to_string(n) + ".wav");
            if (!std::filesystem::exists(file)) break;
            if (!ReadWavMono(file, pcm, rate) || rate != sampleRate) continue;
            clips_[v].push_back(std::move(pcm));
            any = true;
        }
    }
    return any;
}

const std::vector<float>* VocalBank::Pick(Vocal v, std::uint32_t& rng) {
    auto& list = clips_[static_cast<size_t>(v)];
    if (list.empty()) return nullptr;
    size_t i = static_cast<size_t>(NextRandom(rng) * static_cast<float>(list.size())) % list.size();
    auto& last = last_[static_cast<size_t>(v)];
    if (list.size() > 1 && i == last) i = (i + 1) % list.size();
    last = i;
    return &list[i];
}

// ---------------------------------------------------------------------------
void RobotGlitcher::Plan(SpeakingStyle style, float quirks, std::uint32_t& rng, bool force) {
    seen_ = 0;
    armed_ = force || NextRandom(rng) < Spontaneity::GlitchChance(style, quirks);
    if (!armed_) return;
    // Pas dans la toute première syllabe (on doit d'abord entendre la voix normale), ni trop loin
    // (les phrases courtes doivent pouvoir bugger).
    at_ = static_cast<std::size_t>((0.35f + NextRandom(rng) * 1.4f) * static_cast<float>(rate_));
    repeats_ = 2 + static_cast<int>(NextRandom(rng) * 2.2f);  // 2 à 4 répétitions
    grainMs_ = 45.f + NextRandom(rng) * 55.f;                  // une demi-syllabe
    tapeStop_ = NextRandom(rng) < 0.3f;
    seed_ = rng | 1u;
}

namespace {

// Grain numérique : réduction de bits + échantillonnage bloqué (le même que VoiceFX, en plus dur).
void Crush(std::span<float> x, float bits, int hold) {
    const float levels = std::exp2(bits - 1.f);
    float held = 0.f;
    for (size_t i = 0; i < x.size(); ++i) {
        if (i % static_cast<size_t>(std::max(1, hold)) == 0) held = std::round(x[i] * levels) / levels;
        x[i] = held;
    }
}

void Fade(std::span<float> x, size_t n) {
    n = std::min(n, x.size() / 2);
    for (size_t i = 0; i < n; ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(n);
        x[i] *= g;
        x[x.size() - 1 - i] *= g;
    }
}

// Ralenti « bande qui s'arrête » : lecture de plus en plus lente, la hauteur chute.
std::vector<float> TapeStop(std::span<const float> src, float seconds, int rate) {
    std::vector<float> out;
    double pos = 0.0, speed = 1.0;
    const double decel = 1.0 / (seconds * rate);
    while (speed > 0.15 && pos + 1 < static_cast<double>(src.size())) {
        const size_t k = static_cast<size_t>(pos);
        const float f = static_cast<float>(pos - static_cast<double>(k));
        out.push_back((src[k] * (1.f - f) + src[k + 1] * f) * static_cast<float>(speed));
        pos += speed;
        speed -= decel;
    }
    return out;
}

}  // namespace

void RobotGlitcher::Process(std::vector<float>& chunk) {
    if (!armed_ || chunk.empty()) {
        seen_ += chunk.size();
        return;
    }
    const std::size_t grain = static_cast<std::size_t>(grainMs_ * 0.001f * static_cast<float>(rate_));
    if (seen_ + chunk.size() < at_ + grain) {  // pas encore là
        seen_ += chunk.size();
        return;
    }
    std::size_t start = at_ > seen_ ? at_ - seen_ : 0;
    // Caler le bug sur une attaque : premier endroit après `start` où l'énergie remonte franchement.
    const std::size_t win = static_cast<std::size_t>(0.01f * static_cast<float>(rate_));
    float prev = 1e9f;
    for (std::size_t i = start; i + win < chunk.size() && i < start + static_cast<std::size_t>(rate_ / 3); i += win) {
        float e = 0.f;
        for (std::size_t j = i; j < i + win; ++j) e += chunk[j] * chunk[j];
        if (e > prev * 4.f && e > 1e-4f * static_cast<float>(win)) { start = i; break; }
        prev = std::max(e, 1e-7f);
    }
    if (start + grain > chunk.size()) start = chunk.size() - std::min(grain, chunk.size());
    const std::vector<float> g(chunk.begin() + static_cast<std::ptrdiff_t>(start),
                               chunk.begin() + static_cast<std::ptrdiff_t>(start + std::min(grain, chunk.size() - start)));
    std::vector<float> insert;
    std::uint32_t rng = seed_;
    for (int r = 0; r < repeats_; ++r) {
        std::vector<float> rep = g;
        // Chaque accroc est un peu plus « numérique » : bits en moins, échantillons tenus plus longtemps.
        Crush(rep, 9.f - 1.6f * static_cast<float>(r) - NextRandom(rng), 1 + r * 2);
        for (auto& s : rep) s *= 0.95f - 0.08f * static_cast<float>(r);
        Fade(rep, static_cast<size_t>(rate_ / 500));
        insert.insert(insert.end(), rep.begin(), rep.end());
        insert.insert(insert.end(), static_cast<size_t>(rate_ * (0.008f + 0.012f * NextRandom(rng))), 0.f);  // micro-coupure
    }
    if (tapeStop_) {
        auto ts = TapeStop(g, 0.18f, rate_);
        Fade(ts, static_cast<size_t>(rate_ / 400));
        insert.insert(insert.end(), ts.begin(), ts.end());
        insert.insert(insert.end(), static_cast<size_t>(rate_ * 0.06f), 0.f);  // il « redémarre »
    }
    chunk.insert(chunk.begin() + static_cast<std::ptrdiff_t>(start), insert.begin(), insert.end());
    seen_ += chunk.size();
    armed_ = false;
}

std::vector<float> RobotGlitcher::Burst(std::span<const float> source, int sampleRate, std::uint32_t& rng) {
    // Grésillement : 3 éclats très courts du son source (ou de bruit), réduits à 4-5 bits.
    std::vector<float> out;
    const size_t n = static_cast<size_t>(0.035f * static_cast<float>(sampleRate));
    for (int k = 0; k < 3; ++k) {
        std::vector<float> b(n);
        const size_t off = source.size() > n ? static_cast<size_t>(NextRandom(rng) * static_cast<float>(source.size() - n)) : 0;
        for (size_t i = 0; i < n; ++i)
            b[i] = i + off < source.size() ? source[i + off] : (NextRandom(rng) * 2.f - 1.f) * 0.2f;
        Crush(b, 4.f + NextRandom(rng), 3 + k * 2);
        Fade(b, n / 8);
        out.insert(out.end(), b.begin(), b.end());
        out.insert(out.end(), static_cast<size_t>(sampleRate * 0.015f), 0.f);
    }
    return out;
}

// ---------------------------------------------------------------------------
void EmotionShaper::Set(SpeakingStyle style, float intensity) noexcept {
    // (écart de vitesse, gain en dB) à intensité 1
    float speed = 0.f, db = 0.f;
    switch (style) {
        case SpeakingStyle::Bright: speed = 0.045f; db = 1.5f; break;
        case SpeakingStyle::Surprised: speed = 0.055f; db = 2.f; break;
        case SpeakingStyle::Amused: speed = 0.035f; db = 1.f; break;
        case SpeakingStyle::Annoyed: speed = 0.04f; db = 2.f; break;
        case SpeakingStyle::Soft: speed = -0.045f; db = -3.f; break;
        case SpeakingStyle::Calm: speed = -0.03f; db = -1.5f; break;
        default: break;
    }
    intensity = std::clamp(intensity, 0.f, 1.5f);
    ratio_ = 1.0 + static_cast<double>(speed * intensity);
    gain_ = std::pow(10.f, db * intensity / 20.f);
}

void EmotionShaper::Process(std::vector<float>& chunk) {
    if (chunk.empty()) return;
    if (std::abs(ratio_ - 1.0) < 1e-4) {
        for (auto& s : chunk) s = std::clamp(s * gain_, -1.f, 1.f);
        return;
    }
    // Interpolation linéaire continue d'un morceau à l'autre : l'échantillon « -1 » est le dernier du
    // morceau précédent, pos_ est relatif au début du morceau courant.
    std::vector<float> out;
    out.reserve(static_cast<size_t>(static_cast<double>(chunk.size()) / ratio_) + 2);
    const auto at = [&](long i) { return i < 0 ? (havePrev_ ? prev_ : chunk[0]) : chunk[static_cast<size_t>(i)]; };
    while (pos_ < static_cast<double>(chunk.size()) - 1.0) {
        const long k = static_cast<long>(std::floor(pos_));
        const float f = static_cast<float>(pos_ - static_cast<double>(k));
        out.push_back(std::clamp((at(k) * (1.f - f) + at(k + 1) * f) * gain_, -1.f, 1.f));
        pos_ += ratio_;
    }
    pos_ -= static_cast<double>(chunk.size());
    prev_ = chunk.back();
    havePrev_ = true;
    chunk.swap(out);
}

float Spontaneity::GlitchChance(SpeakingStyle s, float quirks) noexcept {
    float f = 0.f;
    switch (s) {
        case SpeakingStyle::Normal: f = 1.f; break;
        case SpeakingStyle::Bright: f = 1.3f; break;
        case SpeakingStyle::Amused: f = 1.2f; break;
        case SpeakingStyle::Surprised: f = 1.6f; break;  // le choc le fait planter
        case SpeakingStyle::Annoyed: f = 1.2f; break;
        default: f = 0.f;                               // doux / calme : jamais quand il console
    }
    return std::clamp(quirks, 0.f, 1.f) * 0.14f * f;
}

float Spontaneity::CoughChance(SpeakingStyle s, float quirks) noexcept {
    return (s == SpeakingStyle::Normal || s == SpeakingStyle::Bright) ? std::clamp(quirks, 0.f, 1.f) * 0.035f : 0.f;
}

Vocal Spontaneity::Lead(SpeakingStyle s, std::uint32_t& rng) noexcept {
    const float r = NextRandom(rng);
    switch (s) {
        case SpeakingStyle::Amused: return r < 0.45f ? Vocal::Chuckle : r < 0.6f ? Vocal::Laugh : Vocal::Count;
        case SpeakingStyle::Surprised: return r < 0.4f ? Vocal::Gasp : Vocal::Count;
        case SpeakingStyle::Annoyed: return r < 0.35f ? Vocal::Sigh : Vocal::Count;  // « pfff… »
        default: return Vocal::Count;
    }
}

}  // namespace companion
