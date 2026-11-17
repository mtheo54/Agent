#include "audio/Expressivity.hpp"

#include "audio/NeuralVoice.hpp"  // ReadWavMono

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
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
namespace {

// Grain numérique : réduction de bits + échantillonnage bloqué (le même que VoiceFX, en plus dur).
void Crush(float* x, std::size_t n, float bits, int hold) noexcept {
    const float levels = std::exp2(bits - 1.f), inv = 1.f / levels;
    const std::size_t h = static_cast<std::size_t>(std::max(1, hold));
    float held = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % h == 0) held = std::round(x[i] * levels) * inv;
        x[i] = held;
    }
}

void Fade(float* x, std::size_t n, std::size_t f) noexcept {
    f = std::min(f, n / 2);
    if (f == 0) return;
    const float inv = 1.f / static_cast<float>(f);
    for (std::size_t i = 0; i < f; ++i) {
        const float g = static_cast<float>(i) * inv;
        x[i] *= g;
        x[n - 1 - i] *= g;
    }
}

// Dosage de l'effet avec des bords adoucis (aucun clic à l'entrée ni à la sortie d'un bug).
inline float EdgeMix(std::size_t i, std::size_t n, std::size_t f) noexcept {
    if (f == 0) return 1.f;
    if (i < f) return static_cast<float>(i) / static_cast<float>(f);
    if (i + f >= n) return static_cast<float>(n - 1 - i) / static_cast<float>(f);
    return 1.f;
}

}  // namespace

void RobotGlitcher::Prepare(int sampleRate) {
    rate_ = sampleRate;
    grain_.reserve(static_cast<std::size_t>(rate_));  // 1 s : bien plus que le plus long bug
    scratch_.reserve(static_cast<std::size_t>(rate_) * 2);
}

void RobotGlitcher::Plan(SpeakingStyle style, float quirks, std::uint32_t& rng, bool force, bool allowBig, float load) {
    seen_ = 0;
    count_ = next_ = 0;
    usedBig_ = false;
    // Probabilité du 1er bug, puis de chacun des suivants (décroissante) ; un PC chargé fait bugger plus.
    float p = Spontaneity::GlitchChance(style, quirks) * (1.f + 1.5f * std::max(0.f, load - 0.5f));
    if (p <= 0.f && !force) return;
    // Poids des types selon l'humeur : Stutter, TapeStop, Chipmunk, Dropout, Skip, BitBurst, Warble, Reverse, RingMod
    std::array<float, static_cast<size_t>(Kind::Count)> w{2, 1, 1, 2, 2, 1.5f, 2, 1, 1};
    switch (style) {
        case SpeakingStyle::Bright: case SpeakingStyle::Surprised: w = {3, 1, 2.5f, 1, 1, 2, 2, 1, 1}; break;
        case SpeakingStyle::Annoyed: w = {2, 2, 0.5f, 1, 1, 3, 1, 0.5f, 2.5f}; break;
        case SpeakingStyle::Amused: w = {2, 0.5f, 2, 1, 1.5f, 1, 2, 1.5f, 1}; break;
        default: break;
    }
    if (!allowBig) w[0] = w[1] = w[2] = 0.f;
    float total = 0.f;
    for (float x : w) total += x;
    std::size_t at = Samples(300.f + NextRandom(rng) * 1200.f);
    for (int i = 0; i < kMaxEvents; ++i) {
        const bool forced = force && i == 0;
        if (!forced && NextRandom(rng) >= p) break;
        p *= 0.55f;
        Event e;
        e.at = at;
        e.seed = rng | 1u;
        if (forced) {
            const float r = NextRandom(rng);
            e.kind = r < 0.5f ? Kind::Stutter : r < 0.75f ? Kind::Chipmunk : Kind::TapeStop;
        } else {
            float r = NextRandom(rng) * total;
            size_t k = 0;
            while (k + 1 < w.size() && r >= w[k]) r -= w[k++];
            e.kind = static_cast<Kind>(k);
        }
        const auto ms = [&](float a, float b) { return static_cast<std::uint16_t>(a + NextRandom(rng) * b); };
        switch (e.kind) {
            case Kind::Stutter: e.lenMs = ms(45, 55); e.reps = static_cast<std::uint8_t>(2 + NextRandom(rng) * 2.2f); break;
            case Kind::TapeStop: e.lenMs = ms(70, 50); break;
            case Kind::Chipmunk: e.lenMs = ms(60, 40); e.reps = static_cast<std::uint8_t>(2 + NextRandom(rng) * 1.5f); break;
            case Kind::Dropout: e.lenMs = ms(30, 60); break;
            case Kind::Skip: e.lenMs = ms(40, 80); break;
            case Kind::BitBurst: e.lenMs = ms(80, 120); break;
            case Kind::Warble: e.lenMs = ms(150, 150); break;
            case Kind::Reverse: e.lenMs = ms(50, 50); break;
            case Kind::RingMod: e.lenMs = ms(150, 100); break;
            default: break;
        }
        if (e.kind == Kind::Stutter || e.kind == Kind::TapeStop || e.kind == Kind::Chipmunk) usedBig_ = true;
        events_[static_cast<size_t>(count_++)] = e;
        at += Samples(e.lenMs) + Samples(600.f + NextRandom(rng) * 1900.f);  // espacés de 0,6 à 2,5 s
    }
}

void RobotGlitcher::PlanSingle(Kind kind, std::size_t atSample, std::uint32_t seed) {
    seen_ = 0;
    next_ = 0;
    count_ = 1;
    std::uint32_t rng = seed | 1u;
    Event e;
    e.at = atSample;
    e.kind = kind;
    e.seed = rng;
    const std::array<std::uint16_t, static_cast<size_t>(Kind::Count)> len{70, 100, 80, 70, 90, 150, 250, 80, 200};
    e.lenMs = len[static_cast<size_t>(kind)];
    e.reps = 3;
    events_[0] = e;
    usedBig_ = kind == Kind::Stutter || kind == Kind::TapeStop || kind == Kind::Chipmunk;
}

std::size_t RobotGlitcher::FindOnset(const std::vector<float>& x, std::size_t from) const noexcept {
    // Premier endroit après `from` où l'énergie remonte franchement (attaque de syllabe), ≤ 1/3 s plus loin.
    const std::size_t win = Samples(10.f);
    float prev = 1e9f;
    for (std::size_t i = from; i + win < x.size() && i < from + static_cast<std::size_t>(rate_ / 3); i += win) {
        float e = 0.f;
        for (std::size_t j = i; j < i + win; ++j) e += x[j] * x[j];
        if (e > prev * 4.f && e > 1e-4f * static_cast<float>(win)) return i;
        prev = std::max(e, 1e-7f);
    }
    return from;
}

void RobotGlitcher::Process(std::vector<float>& chunk) {
    const std::size_t n0 = chunk.size();
    if (next_ >= count_ || n0 == 0) {
        seen_ += n0;
        return;
    }
    // Bugs entièrement contenus dans ce morceau (ou repoussés jusqu'ici), appliqués de la fin vers le début :
    // les positions restent valides malgré les insertions / suppressions.
    const int first = next_;
    int last = next_;
    while (last < count_) {
        const auto& e = events_[static_cast<size_t>(last)];
        if (e.at + Samples(e.lenMs) > seen_ + n0) break;  // déborde : au morceau suivant
        ++last;
    }
    for (int i = last - 1; i >= first; --i) {
        const auto& e = events_[static_cast<size_t>(i)];
        std::size_t pos = e.at > seen_ ? e.at - seen_ : 0;
        if (e.kind == Kind::Stutter || e.kind == Kind::Chipmunk) pos = FindOnset(chunk, pos);
        const std::size_t len = Samples(e.lenMs);
        if (len + Samples(4.f) > chunk.size()) continue;
        pos = std::clamp<std::size_t>(pos, Samples(3.f), chunk.size() - len);
        Apply(chunk, pos, e);
    }
    next_ = last;
    seen_ += n0;
}

void RobotGlitcher::Apply(std::vector<float>& x, std::size_t pos, const Event& e) {
    std::uint32_t rng = e.seed;
    const std::size_t len = Samples(e.lenMs), edge = Samples(3.f);
    float* p = x.data() + pos;
    switch (e.kind) {
        case Kind::Dropout:  // buffer vide : fondu de sortie, silence, fondu d'entrée
            for (std::size_t i = 0; i < len; ++i) p[i] *= 1.f - EdgeMix(i, len, edge);
            break;
        case Kind::Skip: {  // le flux saute en avant
            for (std::size_t i = 0; i < edge; ++i) p[static_cast<std::ptrdiff_t>(i) - static_cast<std::ptrdiff_t>(edge)] *= 1.f - static_cast<float>(i) / static_cast<float>(edge);
            x.erase(x.begin() + static_cast<std::ptrdiff_t>(pos), x.begin() + static_cast<std::ptrdiff_t>(pos + len));
            p = x.data() + pos;
            for (std::size_t i = 0; i < edge && pos + i < x.size(); ++i) p[i] *= static_cast<float>(i) / static_cast<float>(edge);
            break;
        }
        case Kind::BitBurst: {  // éclat numérique
            scratch_.assign(p, p + len);
            Crush(scratch_.data(), len, 4.f + 2.f * NextRandom(rng), 3 + static_cast<int>(NextRandom(rng) * 4.f));
            for (std::size_t i = 0; i < len; ++i) {
                const float m = EdgeMix(i, len, edge);
                p[i] = p[i] * (1.f - m) + scratch_[i] * 0.9f * m;
            }
            break;
        }
        case Kind::Warble: {  // vibrato numérique : lecture à vitesse modulée, même durée
            scratch_.assign(p, p + len);
            const float rate = 9.f + 5.f * NextRandom(rng), depth = 0.05f + 0.04f * NextRandom(rng);
            const float w = 2.f * std::numbers::pi_v<float> * rate / static_cast<float>(rate_);
            double q = 0.0;
            for (std::size_t i = 0; i < len; ++i) {
                const std::size_t k = std::min(static_cast<std::size_t>(q), len - 2);
                const float f = static_cast<float>(q - static_cast<double>(k));
                const float y = scratch_[k] * (1.f - f) + scratch_[k + 1] * f;
                const float m = EdgeMix(i, len, edge * 2);
                p[i] = scratch_[i] * (1.f - m) + y * m;
                q = std::clamp(q + 1.0 + depth * std::sin(w * static_cast<float>(i)), 0.0, static_cast<double>(len - 2));
            }
            break;
        }
        case Kind::Reverse: {  // bout de syllabe à l'envers, raccords en fondu enchaîné
            scratch_.assign(p, p + len);
            for (std::size_t i = 0; i < len; ++i) {
                const float m = EdgeMix(i, len, edge);
                p[i] = scratch_[i] * (1.f - m) + scratch_[len - 1 - i] * m;
            }
            break;
        }
        case Kind::RingMod: {  // flash voix de robot
            const float w = 2.f * std::numbers::pi_v<float> * (40.f + 50.f * NextRandom(rng)) / static_cast<float>(rate_);
            for (std::size_t i = 0; i < len; ++i) {
                const float m = 0.75f * EdgeMix(i, len, edge * 4);
                p[i] *= (1.f - m) + std::sin(w * static_cast<float>(i)) * m * 1.4f;
            }
            break;
        }
        case Kind::Stutter: case Kind::Chipmunk: case Kind::TapeStop: {
            grain_.assign(p, p + len);
            scratch_.clear();
            if (e.kind == Kind::Chipmunk) {  // une octave plus haut (1 échantillon sur 2), répété
                const std::size_t half = len / 2;
                for (int r = 0; r < e.reps; ++r) {
                    const std::size_t s0 = scratch_.size();
                    for (std::size_t i = 0; i < half; ++i) scratch_.push_back(grain_[2 * i] * (0.95f - 0.1f * static_cast<float>(r)));
                    Fade(scratch_.data() + s0, half, edge / 2);
                    scratch_.insert(scratch_.end(), Samples(6.f + 10.f * NextRandom(rng)), 0.f);
                }
            } else if (e.kind == Kind::Stutter) {  // chaque accroc un peu plus « numérique »
                for (int r = 0; r < e.reps; ++r) {
                    const std::size_t s0 = scratch_.size();
                    scratch_.insert(scratch_.end(), grain_.begin(), grain_.end());
                    Crush(scratch_.data() + s0, len, 9.f - 1.6f * static_cast<float>(r) - NextRandom(rng), 1 + r * 2);
                    for (std::size_t i = s0; i < scratch_.size(); ++i) scratch_[i] *= 0.95f - 0.08f * static_cast<float>(r);
                    Fade(scratch_.data() + s0, len, edge);
                    scratch_.insert(scratch_.end(), Samples(8.f + 12.f * NextRandom(rng)), 0.f);
                }
            } else {  // la bande ralentit, la hauteur chute, puis il « redémarre »
                double q = 0.0, speed = 1.0;
                const double decel = 1.0 / (0.18 * rate_);
                while (speed > 0.15 && q + 1 < static_cast<double>(len)) {
                    const std::size_t k = static_cast<std::size_t>(q);
                    const float f = static_cast<float>(q - static_cast<double>(k));
                    scratch_.push_back((grain_[k] * (1.f - f) + grain_[k + 1] * f) * static_cast<float>(speed));
                    q += speed;
                    speed -= decel;
                }
                Fade(scratch_.data(), scratch_.size(), edge);
                scratch_.insert(scratch_.end(), Samples(60.f), 0.f);
            }
            x.insert(x.begin() + static_cast<std::ptrdiff_t>(pos), scratch_.begin(), scratch_.end());
            break;
        }
        default: break;
    }
}

std::vector<float> RobotGlitcher::Burst(std::span<const float> source, int sampleRate, std::uint32_t& rng) {
    // Grésillement : 3 éclats très courts du son source (ou de bruit), réduits à 4-5 bits.
    std::vector<float> out;
    const size_t n = static_cast<size_t>(0.035f * static_cast<float>(sampleRate));
    out.reserve(3 * (n + static_cast<size_t>(sampleRate * 0.015f)));
    for (int k = 0; k < 3; ++k) {
        const size_t s0 = out.size();
        const size_t off = source.size() > n ? static_cast<size_t>(NextRandom(rng) * static_cast<float>(source.size() - n)) : 0;
        for (size_t i = 0; i < n; ++i)
            out.push_back(i + off < source.size() ? source[i + off] : (NextRandom(rng) * 2.f - 1.f) * 0.2f);
        Crush(out.data() + s0, n, 4.f + NextRandom(rng), 3 + k * 2);
        Fade(out.data() + s0, n, n / 8);
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

void EmotionShaper::PlanPitch(SpeakingStyle style, float depthCents, float bendChance, std::uint32_t& rng) noexcept {
    depth_ = std::max(0.f, depthCents);
    prng_ = rng | 1u;
    driftNow_ = driftTarget_ = 0.f;
    nextDrift_ = 0;
    nBends_ = 0;
    if (style == SpeakingStyle::Soft || style == SpeakingStyle::Calm) bendChance *= 0.3f;  // posé : presque pas
    // Attaque « par en dessous » en début de phrase (les humains attaquent rarement pile sur la note).
    if (NextRandom(rng) < bendChance) bends_[static_cast<size_t>(nBends_++)] = {static_cast<std::size_t>(0.02f * rate_), -120.f - 80.f * NextRandom(rng), 0.001f, 0.18f};
    // Virage au milieu : « boing » (monte puis redescend) ou plongée.
    if (NextRandom(rng) < bendChance) {
        const bool up = NextRandom(rng) < 0.6f;
        bends_[static_cast<size_t>(nBends_++)] = {static_cast<std::size_t>((0.6f + 1.8f * NextRandom(rng)) * rate_),
                                                  up ? 180.f + 140.f * NextRandom(rng) : -(220.f + 160.f * NextRandom(rng)),
                                                  up ? 0.12f : 0.2f, up ? 0.22f : 0.3f};
    }
}

float EmotionShaper::CentsAt(std::size_t t) noexcept {
    // Dérive : nouvelle cible toutes les 0,25-0,6 s, rejointe en douceur (pas d'escalier audible).
    if (depth_ > 0.f && t >= nextDrift_) {
        driftTarget_ = (NextRandom(prng_) * 2.f - 1.f) * depth_;
        const std::size_t span = static_cast<std::size_t>((0.25f + 0.35f * NextRandom(prng_)) * rate_);
        driftSlew_ = (driftTarget_ - driftNow_) / static_cast<float>(span / 64 + 1);
        nextDrift_ = t + span;
    }
    if (std::abs(driftTarget_ - driftNow_) > std::abs(driftSlew_)) driftNow_ += driftSlew_;
    float c = driftNow_;
    for (int i = 0; i < nBends_; ++i) {  // virages : montée en upSec, retour en downSec (forme lisse)
        const auto& b = bends_[static_cast<size_t>(i)];
        if (t < b.at) continue;
        const float s = static_cast<float>(t - b.at) / static_cast<float>(rate_);
        if (s < b.upSec) c += b.cents * (0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * s / b.upSec));
        else if (s < b.upSec + b.downSec) c += b.cents * (0.5f + 0.5f * std::cos(std::numbers::pi_v<float> * (s - b.upSec) / b.downSec));
    }
    return c;
}

void EmotionShaper::Process(std::vector<float>& chunk) {
    if (chunk.empty()) return;
    if (std::abs(ratio_ - 1.0) < 1e-4 && depth_ <= 0.f && nBends_ == 0) {
        for (auto& s : chunk) s = std::clamp(s * gain_, -1.f, 1.f);
        return;
    }
    // Interpolation linéaire continue d'un morceau à l'autre : l'échantillon « -1 » est le dernier du
    // morceau précédent, pos_ est relatif au début du morceau courant.
    auto& out = out_;  // réutilisé : échangé avec le morceau à la fin
    out.clear();
    // Même capacité que le tampon du morceau : après l'échange, les deux restent grands (aucune
    // réallocation plus loin, même quand un bug allonge le morceau).
    out.reserve(std::max(chunk.capacity(), static_cast<size_t>(static_cast<double>(chunk.size()) / ratio_) + 2));
    const auto at = [&](long i) { return i < 0 ? (havePrev_ ? prev_ : chunk[0]) : chunk[static_cast<size_t>(i)]; };
    double step = ratio_;
    while (pos_ < static_cast<double>(chunk.size()) - 1.0) {
        if ((t_ & 63u) == 0)  // hauteur mise à jour tous les 64 échantillons (2,7 ms) : exp2 rare
            step = ratio_ * std::exp2(static_cast<double>(CentsAt(t_)) / 1200.0);
        const long k = static_cast<long>(std::floor(pos_));
        const float f = static_cast<float>(pos_ - static_cast<double>(k));
        out.push_back(std::clamp((at(k) * (1.f - f) + at(k + 1) * f) * gain_, -1.f, 1.f));
        pos_ += step;
        ++t_;
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
    // Réglage par défaut (0,5) : ~1 phrase sur 4 bugge (1 sur 3 excité) ; au maximum, une sur deux.
    return std::clamp(quirks, 0.f, 1.f) * 0.5f * f;
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
