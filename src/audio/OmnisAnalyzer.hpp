#pragma once
// OmnisAnalyzer : analyse spectrale et dynamique héritée du plugin OMNIS (rapport §4.C).
//
// Entrée : float stéréo entrelacé 48 kHz (WasapiLoopback). Fenêtre glissante de 3 s :
//   - Loudness court terme (LUFS, pondération K de l'UIT-R BS.1770)
//   - RMS, crête échantillon, facteur de crête
//   - Clipping (>= 3 échantillons consécutifs à pleine échelle)
//   - Corrélation de phase L/R et largeur stéréo (énergie Side / Mid)
//   - Équilibre spectral sur 7 bandes (FFT 4096, Hann), comparé à une pente de référence
//     de -4,5 dB/octave (repère indicatif de mix moderne, pas une vérité absolue)
// Résumé en ~40 tokens pour le modèle local. Seuils (clipping, phase) = niveau 0 de la cascade.
//
// Push() est sans verrou (thread de capture) ; l'analyse tourne sur un thread en priorité basse.

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

struct MixMetrics {
    static constexpr int kBands = 7;
    static constexpr std::array<const char*, kBands> kBandNames{"sub", "bass", "lowmid", "mid", "himid", "pres", "air"};

    float seconds = 0.f;  // secondes de son non silencieux analysées dans la fenêtre
    bool silent = true;
    float lufsShort = -70.f;
    float rmsDb = -90.f;
    float peakDb = -90.f;
    float crestDb = 0.f;
    int clips = 0;
    float correlation = 1.f;  // +1 mono, 0 décorrélé, <0 problème de phase
    float width = 0.f;        // énergie Side / Mid
    std::array<float, kBands> bandDb{};  // écart à la pente de référence, en dB

    [[nodiscard]] std::string ToSummary() const;
};

enum class MixAlert : std::uint8_t { Clipping, PhaseProblem };

class OmnisAnalyzer {
public:
    using AlertCallback = std::function<void(MixAlert, const MixMetrics&)>;

    OmnisAnalyzer();
    ~OmnisAnalyzer() { Stop(); }

    void SetAlertCallback(AlertCallback cb);
    void Start();
    void Stop();
    void Reset();

    // Thread de capture : copie sans verrou dans une file circulaire.
    void Push(const float* interleavedStereo, std::size_t frames) noexcept;

    [[nodiscard]] MixMetrics Snapshot() const;

    // Traite immédiatement ce qui est en file (tests, ou appel synchrone).
    void DrainNow();

private:
    struct Block {
        double kL = 0, kR = 0, l2 = 0, r2 = 0, lr = 0;
        float peak = 0.f;
        int clips = 0;
        std::size_t n = 0;
    };
    struct Biquad {
        double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        double Process(double x) noexcept {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void Run(std::stop_token st);
    void ProcessFrame(float l, float r);
    void FinishBlock();
    void RunFft();
    MixMetrics ComputeLocked() const;

    // File circulaire SPSC
    std::vector<float> ring_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};

    // État d'analyse (thread d'analyse uniquement, sauf résultats protégés par mutex_)
    Biquad kShelfL_, kShelfR_, kHpL_, kHpR_;
    Block cur_;
    int runL_ = 0, runR_ = 0;
    std::vector<float> fftIn_;
    std::size_t fftFill_ = 0;
    std::vector<float> window_;
    std::vector<std::complex<float>> fftBuf_;
    std::array<std::pair<std::size_t, std::size_t>, MixMetrics::kBands> bandBins_{};
    std::array<double, MixMetrics::kBands> refShareDb_{};

    mutable std::mutex mutex_;
    std::deque<Block> blocks_;                         // 100 ms chacun, 3 s au total
    std::array<double, MixMetrics::kBands> bandEnergy_{};  // lissage exponentiel ~3 s
    bool bandsValid_ = false;

    AlertCallback alert_;
    std::chrono::steady_clock::time_point lastAlert_[2]{};
    std::jthread thread_;
};

}  // namespace companion
