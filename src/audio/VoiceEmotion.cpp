#include "audio/VoiceEmotion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <numeric>
#include <sstream>
#include <vector>

namespace companion {
namespace {

constexpr int kRate = 16000;
constexpr int kFrame = 640;  // 40 ms : au moins 2 périodes à 70 Hz
constexpr int kHop = 160;    // 10 ms
constexpr int kMinLag = kRate / 450, kMaxLag = kRate / 70;

// YIN (de Cheveigné & Kawahara, 2002) : renvoie la fréquence fondamentale, ou 0 si non voisé.
float YinPitch(const float* x, float& aperiodicity) {
    static thread_local std::vector<float> d(kMaxLag + 2);
    d[0] = 1.f;
    float running = 0.f;
    const int w = kFrame - kMaxLag;
    for (int tau = 1; tau <= kMaxLag + 1; ++tau) {
        float sum = 0.f;
        for (int i = 0; i < w; ++i) {
            const float diff = x[i] - x[i + tau];
            sum += diff * diff;
        }
        running += sum;
        d[tau] = running > 0.f ? sum * tau / running : 1.f;  // différence moyenne cumulée normalisée
    }
    int best = -1;
    for (int tau = kMinLag; tau <= kMaxLag; ++tau) {
        if (d[tau] < 0.15f) {  // premier creux sous le seuil
            while (tau + 1 <= kMaxLag && d[tau + 1] < d[tau]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        aperiodicity = 1.f;
        return 0.f;
    }
    aperiodicity = d[best];
    // Interpolation parabolique autour du minimum
    const float a = d[best - 1], b = d[best], c = d[best + 1];
    const float den = a - 2 * b + c;
    const float shift = std::abs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;
    return static_cast<float>(kRate) / (static_cast<float>(best) + shift);
}

float Percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.f;
    const size_t k = static_cast<size_t>(std::clamp(p, 0.f, 1.f) * static_cast<float>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

}  // namespace

const char* ToString(VoiceEmotion e) noexcept {
    switch (e) {
        case VoiceEmotion::Joyful: return "joyeux";
        case VoiceEmotion::Sad: return "triste";
        case VoiceEmotion::Angry: return "énervé";
        case VoiceEmotion::Stressed: return "stressé";
        case VoiceEmotion::Tired: return "fatigué";
        default: return "neutre";
    }
}

const char* ToClipName(VoiceEmotion e) noexcept {
    switch (e) {
        case VoiceEmotion::Joyful: return "happy";
        case VoiceEmotion::Sad: return "empathy";
        case VoiceEmotion::Angry: return "soothe";
        case VoiceEmotion::Stressed: return "soothe";
        case VoiceEmotion::Tired: return "empathy";
        default: return "";
    }
}

float VoiceEmotionAnalyzer::Stat::Z(float x, float minStd) const {
    return (x - mean) / std::max(std::sqrt(std::max(var, 0.f)), minStd);
}

ProsodyFeatures VoiceEmotionAnalyzer::Extract(std::span<const float> pcm) {
    ProsodyFeatures f;
    if (pcm.size() < static_cast<size_t>(kRate) / 2) return f;  // < 0,5 s : pas assez de matière
    f.durationSec = static_cast<float>(pcm.size()) / kRate;

    // Seuil de bruit : 15e centile de l'énergie des trames.
    std::vector<float> energies;
    for (size_t i = 0; i + kFrame <= pcm.size(); i += kHop) {
        double e = 0;
        for (int k = 0; k < kFrame; ++k) e += static_cast<double>(pcm[i + k]) * pcm[i + k];
        energies.push_back(10.f * std::log10(static_cast<float>(e / kFrame) + 1e-10f));
    }
    if (energies.size() < 10) return f;
    const float noise = Percentile(energies, 0.15f);
    const float speechThreshold = std::max(noise + 8.f, -55.f);

    struct Fr { float st, db; bool voiced, speech; };
    std::vector<Fr> frames;
    frames.reserve(energies.size());
    for (size_t idx = 0, i = 0; i + kFrame <= pcm.size(); i += kHop, ++idx) {
        const bool speech = energies[idx] > speechThreshold;
        float ap = 1.f, hz = 0.f;
        if (speech) hz = YinPitch(pcm.data() + i, ap);
        const bool voiced = speech && hz > 70.f && hz < 450.f && ap < 0.25f;
        frames.push_back({voiced ? 12.f * std::log2(hz / 100.f) : 0.f, energies[idx], voiced, speech});
    }

    // Lissage des sauts d'octave (médiane glissante sur 5 trames voisées).
    std::vector<float> pitches, voicedDb;
    std::vector<std::pair<float, float>> contour;  // (temps, demi-tons)
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].voiced) continue;
        std::vector<float> win;
        for (size_t k = (i >= 2 ? i - 2 : 0); k <= std::min(frames.size() - 1, i + 2); ++k)
            if (frames[k].voiced) win.push_back(frames[k].st);
        const float st = Percentile(win, 0.5f);
        pitches.push_back(st);
        voicedDb.push_back(frames[i].db);
        contour.push_back({static_cast<float>(i) * kHop / kRate, st});
    }
    const size_t speechFrames = static_cast<size_t>(std::count_if(frames.begin(), frames.end(), [](const Fr& x) { return x.speech; }));
    if (pitches.size() < 8 || speechFrames < 15) return f;

    f.voicedRatio = static_cast<float>(pitches.size()) / static_cast<float>(speechFrames);
    f.pitchMedianSt = Percentile(pitches, 0.5f);
    f.pitchRangeSt = Percentile(pitches, 0.9f) - Percentile(pitches, 0.1f);
    const float meanDb = std::accumulate(voicedDb.begin(), voicedDb.end(), 0.f) / static_cast<float>(voicedDb.size());
    float var = 0.f;
    for (float db : voicedDb) var += (db - meanDb) * (db - meanDb);
    f.energyDb = meanDb;
    f.energyStdDb = std::sqrt(var / static_cast<float>(voicedDb.size()));

    // Intonation finale : régression linéaire sur le dernier tiers du contour.
    const size_t start = contour.size() * 2 / 3;
    if (contour.size() - start >= 4) {
        float sx = 0, sy = 0, sxx = 0, sxy = 0;
        const float n = static_cast<float>(contour.size() - start);
        for (size_t i = start; i < contour.size(); ++i) {
            sx += contour[i].first; sy += contour[i].second;
            sxx += contour[i].first * contour[i].first; sxy += contour[i].first * contour[i].second;
        }
        const float den = n * sxx - sx * sx;
        if (std::abs(den) > 1e-6f) f.pitchSlopeSt = (n * sxy - sx * sy) / den;
    }

    // Débit : noyaux syllabiques = maxima locaux de l'énergie lissée (fenêtre 50 ms) qui dominent
    // le creux précédent d'au moins 1,5 dB, espacés d'au moins 80 ms (jusqu'à ~12 syllabes/s).
    std::vector<float> env(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        float s = 0; int c = 0;
        for (size_t k = (i >= 2 ? i - 2 : 0); k <= std::min(frames.size() - 1, i + 2); ++k) { s += frames[k].db; ++c; }
        env[i] = s / static_cast<float>(c);
    }
    int peaks = 0;
    size_t lastPeak = 0;
    float valley = env.empty() ? 0.f : env[0];
    for (size_t i = 1; i + 1 < env.size(); ++i) {
        valley = std::min(valley, env[i]);
        if (!frames[i].speech || env[i] < env[i - 1] || env[i] < env[i + 1]) continue;
        if (env[i] - valley >= 1.5f && (peaks == 0 || i - lastPeak >= 8)) {
            ++peaks;
            lastPeak = i;
            valley = env[i];  // le prochain pic doit redescendre puis remonter
        }
    }
    const float speechSec = static_cast<float>(speechFrames) * kHop / kRate;
    f.syllablesPerSec = speechSec > 0.2f ? static_cast<float>(peaks) / speechSec : 0.f;
    f.pauseRatio = 1.f - static_cast<float>(speechFrames) / static_cast<float>(frames.size());
    f.valid = true;
    return f;
}

EmotionEstimate VoiceEmotionAnalyzer::Estimate(const ProsodyFeatures& f) const {
    EmotionEstimate e;
    e.features = f;
    if (!f.valid) return e;
    const float zPitch = pitch_.Z(f.pitchMedianSt, 1.5f);
    const float zRange = range_.Z(f.pitchRangeSt, 1.2f);
    const float zEnergy = energy_.Z(f.energyDb, 2.5f);
    const float zRate = rate_.Z(f.syllablesPerSec, 0.6f);
    const float zVar = energyStd_.Z(f.energyStdDb, 1.2f);

    e.arousal = std::clamp(0.35f * zEnergy + 0.25f * zPitch + 0.2f * zRange + 0.2f * zRate, -2.f, 2.f) / 2.f;
    // Tonalité prosodique (indicative) : mélodie ample et montante = positif ; basse, plate, lente = négatif ;
    // forte mais mélodie plate et descendante = tension.
    e.valence = std::clamp(0.45f * zRange + 0.25f * std::clamp(f.pitchSlopeSt / 6.f, -1.f, 1.f) + 0.15f * zPitch -
                               0.25f * std::max(0.f, zEnergy - 1.f) * (zRange < 0.3f ? 1.f : 0.f),
                           -2.f, 2.f) / 2.f;

    if (e.arousal > 0.28f) {
        if (e.valence > 0.1f && zRange > 0.f) e.label = VoiceEmotion::Joyful;
        else if (zEnergy > 0.9f) e.label = VoiceEmotion::Angry;
        else e.label = VoiceEmotion::Stressed;
    } else if (e.arousal < -0.25f) {
        // Tristesse = voix basse ET monotone ; fatigue = voix basse et lente, mélodie conservée.
        e.label = zRange < -0.4f ? VoiceEmotion::Sad : VoiceEmotion::Tired;
    }
    // Confiance : intensité de l'écart × maturité de la voix habituelle (10 phrases = pleine confiance).
    e.confidence = std::clamp(std::abs(e.arousal) * 1.6f, 0.f, 1.f) * (0.45f + 0.55f * std::min(1.f, n_ / 10.f));

    // Description en français, pour le modèle local.
    std::vector<std::string> cues;
    if (zEnergy > 0.8f) cues.push_back("voix plus forte");
    if (zEnergy < -0.8f) cues.push_back("voix plus faible");
    if (zRate > 0.8f) cues.push_back("débit plus rapide");
    if (zRate < -0.8f) cues.push_back("débit plus lent");
    if (zPitch > 0.8f) cues.push_back("voix plus aiguë");
    if (zPitch < -0.8f) cues.push_back("voix plus grave");
    if (zRange > 0.8f) cues.push_back("intonation très expressive");
    if (zRange < -0.8f) cues.push_back("intonation monotone");
    if (zVar > 1.f) cues.push_back("à-coups dans la voix");
    if (f.pitchSlopeSt > 8.f) cues.push_back("finit en montant");
    std::string d;
    for (size_t i = 0; i < cues.size(); ++i) d += (i ? (i + 1 == cues.size() ? " et " : ", ") : "") + cues[i];
    e.description = d.empty() ? "voix habituelle" : d + " que d'habitude";
    return e;
}

void VoiceEmotionAnalyzer::Learn(const ProsodyFeatures& f) {
    if (!f.valid) return;
    // Apprentissage rapide au début, puis lent : la voix habituelle suit l'utilisateur sans être
    // emportée par un seul moment d'émotion.
    const float alpha = std::max(0.05f, 1.f / static_cast<float>(n_ + 2));
    pitch_.Update(f.pitchMedianSt, alpha);
    range_.Update(f.pitchRangeSt, alpha);
    energy_.Update(f.energyDb, alpha);
    rate_.Update(f.syllablesPerSec, alpha);
    energyStd_.Update(f.energyStdDb, alpha);
    ++n_;
}

EmotionEstimate VoiceEmotionAnalyzer::Analyze(std::span<const float> pcm16k) {
    const ProsodyFeatures f = Extract(pcm16k);
    EmotionEstimate e = Estimate(f);
    Learn(f);
    return e;
}

std::string VoiceEmotionAnalyzer::SaveBaseline() const {
    return std::format("{};{};{};{};{};{};{};{};{};{};{}", n_, pitch_.mean, pitch_.var, range_.mean, range_.var,
                       energy_.mean, energy_.var, rate_.mean, rate_.var, energyStd_.mean, energyStd_.var);
}

void VoiceEmotionAnalyzer::LoadBaseline(const std::string& s) {
    std::stringstream ss(s);
    std::string item;
    std::vector<float> v;
    while (std::getline(ss, item, ';')) {
        try { v.push_back(std::stof(item)); } catch (...) { return; }
    }
    if (v.size() != 11) return;
    n_ = static_cast<int>(v[0]);
    pitch_ = {v[1], v[2]}; range_ = {v[3], v[4]}; energy_ = {v[5], v[6]}; rate_ = {v[7], v[8]}; energyStd_ = {v[9], v[10]};
}

}  // namespace companion

namespace companion {
namespace {
bool HasAny(const std::string& t, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (t.find(w) != std::string::npos) return true;
    return false;
}
}  // namespace

float TextSentiment(const std::string& text) {
    std::string t(text);
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    float s = 0.f;
    if (HasAny(t, {"trop bien", "génial", "genial", "incroyable", "content", "heureux", "super", "parfait", "j'adore",
                   "je kiffe", "trop cool", "gagné", "gagne", "mdr", "haha", "ahah", "lol", "trop fort", "magnifique", "merci"}))
        s += 0.7f;
    if (HasAny(t, {"saoule", "soule", "chiant", "nul", "déteste", "deteste", "énervé", "enerve", "putain", "merde", "marre",
                   "galère", "galere", "plante", "raté", "rate ", "perdu", "j'en peux plus", "insupportable", "pourri"}))
        s -= 0.7f;
    if (HasAny(t, {"triste", "déprim", "deprim", "seul", "envie de rien", "plus envie", "pleur", "mal au coeur", "ça va pas"}))
        s -= 0.6f;
    return std::clamp(s, -1.f, 1.f);
}

EmotionEstimate FuseWithText(EmotionEstimate e, const std::string& text) {
    const float t = TextSentiment(text);
    std::string lower(text);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool tiredWords = HasAny(lower, {"fatigué", "fatigue", "crevé", "creve", "dormir", "coucher", "épuisé", "epuise"});
    e.valence = std::clamp(0.4f * e.valence + 0.6f * t, -1.f, 1.f);
    if (e.arousal > 0.2f) {  // agité : les mots tranchent entre joie et colère
        if (t > 0.3f) e.label = VoiceEmotion::Joyful;
        else if (t < -0.3f) e.label = VoiceEmotion::Angry;
    } else if (e.arousal < -0.15f) {  // abattu : triste ou fatigué
        if (tiredWords) e.label = VoiceEmotion::Tired;
        else if (t < -0.3f) e.label = VoiceEmotion::Sad;
    } else if (std::abs(t) > 0.5f && e.label == VoiceEmotion::Neutral) {  // voix neutre, mots marqués
        e.label = t > 0 ? VoiceEmotion::Joyful : (tiredWords ? VoiceEmotion::Tired : VoiceEmotion::Sad);
        e.confidence = std::max(e.confidence, 0.4f);
    }
    return e;
}

}  // namespace companion
