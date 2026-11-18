#pragma once
// SpeakerFeatures : énergies de bancs de filtres mel « façon Kaldi » (80 bandes, 25 ms / 10 ms, 16 kHz), l'entrée
// des modèles de reconnaissance de voix (CAM++, WeSpeaker). Logique pure, comparée à kaldi-native-fbank
// (tools/sim/fbanktest.cpp). Paramètres de Kaldi : retrait du continu, préaccentuation 0,97, fenêtre de Povey,
// FFT 512, puissance, mel 20 Hz – 8 kHz, log, pas de dithering, snip_edges.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

namespace companion {

class KaldiFbank {
public:
    static constexpr int kRate = 16000, kLen = 400, kShift = 160, kFft = 512, kMel = 80;

    KaldiFbank() {
        window_.resize(kLen);
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < kLen; ++i) window_[i] = std::pow(0.5 - 0.5 * std::cos(2.0 * pi * i / (kLen - 1)), 0.85);
        // Bancs mel (échelle de Kaldi : 1127 ln(1 + f/700)), triangles dans le domaine mel, bins 0..255.
        const auto mel = [](double f) { return 1127.0 * std::log(1.0 + f / 700.0); };
        const double lo = mel(20.0), hi = mel(kRate / 2.0), step = (hi - lo) / (kMel + 1);
        banks_.assign(kMel, std::vector<float>(kFft / 2, 0.f));
        for (int m = 0; m < kMel; ++m) {
            const double left = lo + m * step, center = left + step, right = center + step;
            for (int b = 0; b < kFft / 2; ++b) {
                const double f = mel(b * static_cast<double>(kRate) / kFft);
                if (f > left && f < right)
                    banks_[m][b] = static_cast<float>(f <= center ? (f - left) / (center - left) : (right - f) / (right - center));
            }
        }
        for (int i = 0; i < kFft; ++i) {  // facteurs de rotation
            const double a = -2.0 * pi * i / kFft;
            twiddle_.emplace_back(std::cos(a), std::sin(a));
        }
    }

    // x : 16 kHz mono ; scale : 1 (échantillons dans [-1, 1]) ou 32768 (modèles entraînés sur des entiers 16 bits).
    // Renvoie frames × 80 valeurs ; frames = 1 + (n - 400) / 160.
    std::vector<float> Compute(const float* x, std::size_t n, float scale, int& frames) const {
        frames = n < static_cast<std::size_t>(kLen) ? 0 : 1 + static_cast<int>((n - kLen) / kShift);
        std::vector<float> out(static_cast<std::size_t>(frames) * kMel);
        std::vector<double> fr(kLen);
        std::vector<std::complex<double>> buf(kFft);
        for (int t = 0; t < frames; ++t) {
            const float* s = x + static_cast<std::size_t>(t) * kShift;
            double mean = 0;
            for (int i = 0; i < kLen; ++i) mean += fr[i] = s[i] * static_cast<double>(scale);
            mean /= kLen;
            for (auto& v : fr) v -= mean;
            for (int i = kLen - 1; i > 0; --i) fr[i] -= 0.97 * fr[i - 1];
            fr[0] -= 0.97 * fr[0];
            for (int i = 0; i < kFft; ++i) buf[i] = i < kLen ? fr[i] * window_[i] : 0.0;
            Fft(buf);
            for (int m = 0; m < kMel; ++m) {
                double e = 0;
                for (int b = 0; b < kFft / 2; ++b)
                    if (banks_[m][b] != 0.f) e += banks_[m][b] * std::norm(buf[b]);
                out[static_cast<std::size_t>(t) * kMel + m] =
                    static_cast<float>(std::log(std::max(e, static_cast<double>(std::numeric_limits<float>::epsilon()))));
            }
        }
        return out;
    }

    // « global-mean » : moyenne de chaque bande retirée sur toute la phrase (CAM++).
    static void SubtractMean(std::vector<float>& f, int frames) {
        if (frames <= 0) return;
        for (int m = 0; m < kMel; ++m) {
            double s = 0;
            for (int t = 0; t < frames; ++t) s += f[static_cast<std::size_t>(t) * kMel + m];
            const float mean = static_cast<float>(s / frames);
            for (int t = 0; t < frames; ++t) f[static_cast<std::size_t>(t) * kMel + m] -= mean;
        }
    }

private:
    std::vector<double> window_;
    std::vector<std::vector<float>> banks_;
    std::vector<std::complex<double>> twiddle_;

    void Fft(std::vector<std::complex<double>>& a) const {  // radix-2 itératif, taille 512
        const int n = kFft;
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1)
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < len / 2; ++k) {
                    const auto w = twiddle_[static_cast<std::size_t>(k * (n / len))];
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
    }
};

inline float Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.f;
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return aa > 0 && bb > 0 ? static_cast<float>(ab / std::sqrt(aa * bb)) : 0.f;
}

}  // namespace companion
