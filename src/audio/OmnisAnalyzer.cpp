#include "audio/OmnisAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#if defined(_WIN32)
#include "core/Log.hpp"
#endif

namespace companion {
namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kBlockFrames = 4800;  // 100 ms
constexpr std::size_t kWindowBlocks = 30;   // 3 s (loudness court terme)
constexpr std::size_t kFftSize = 4096;
constexpr std::size_t kHop = 2048;
constexpr std::array<double, MixMetrics::kBands + 1> kEdges{20, 60, 250, 500, 2000, 4000, 6000, 20000};

double ToDb(double power) { return 10.0 * std::log10(std::max(power, 1e-12)); }

void Fft(std::vector<std::complex<float>>& a) {  // radix-2 itératif, en place
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace

std::string MixMetrics::ToSummary() const {
    if (silent || seconds < 0.5f) return "Mix : aucun son analysé.";
    std::string s = std::format("Mix {:.0f}s : {:.1f} LUFS, crête {:.1f} dBFS, crest {:.1f} dB, clips {}, corr {:.2f}, largeur {:.2f} | ",
                                seconds, lufsShort, peakDb, crestDb, clips, correlation, width);
    for (int i = 0; i < kBands; ++i) s += std::format("{}{:+.0f} ", kBandNames[static_cast<size_t>(i)],
                         std::clamp(bandDb[static_cast<size_t>(i)], -30.f, 30.f));  // bande vide : borne lisible
    s += "(dB vs pente réf.)";
    return s;
}

std::string MixMetrics::Diagnosis() const {
    if (silent || seconds < 0.5f) return "Diagnostic : rien à analyser.";
    struct Issue { int severity; std::string text; };
    std::vector<Issue> issues;
    const auto band = [&](int i) { return bandDb[static_cast<size_t>(i)]; };
    // Dynamique et niveau
    if (clips > 0)
        issues.push_back({clips > 100 ? 10 : 7,
                          std::format("ÇA SATURE : {} clips, crête à {:.1f} dBFS. Action : baisser le master de 4 à 6 dB "
                                      "et mettre un limiteur plafonné à -1 dB.", clips, peakDb)});
    if (lufsShort > -7.f)
        issues.push_back({9, std::format("BEAUCOUP TROP FORT : {:.1f} LUFS (cible -9 à -14 selon le style). Écrasé, "
                                         "aucune marge.", lufsShort)});
    else if (lufsShort < -20.f)
        issues.push_back({4, std::format("Trop faible : {:.1f} LUFS, il manque du niveau.", lufsShort)});
    if (crestDb < 6.f)
        issues.push_back({8, std::format("Aucune dynamique (facteur de crête {:.1f} dB, il en faut 8 à 12) : ça respire "
                                         "pas, les transitoires sont écrasés.", crestDb)});
    // Phase et image
    if (correlation < 0.f)
        issues.push_back({9, std::format("PROBLÈME DE PHASE grave (corrélation {:.2f}) : le son disparaît en mono.",
                                         correlation)});
    else if (correlation < 0.3f)
        issues.push_back({6, std::format("Phase limite (corrélation {:.2f}) : vérifie les élargisseurs.", correlation)});
    if (width < 0.05f) issues.push_back({2, "Image quasi mono : pas de largeur stéréo, tout est au centre."});
    // Spectre (écarts à la pente de référence)
    const float mids = (band(2) + band(3) + band(4)) / 3.f;
    if (band(0) > 2.f && band(0) - mids > 12.f)
        issues.push_back({7, std::format("Le sub écrase tout (+{:.0f} dB au-dessus des médiums) : 808/kick trop forts, "
                                         "ils bouffent le reste.", band(0) - mids)});
    if (band(3) < -12.f)
        issues.push_back({6, std::format("Médiums creusés ({:+.0f} dB) : la mélodie et la voix n'ont pas de place, "
                                         "ça manque de corps.", band(3))});
    if (band(2) > 5.f) issues.push_back({5, "Bas-médiums chargés : c'est boueux, nettoie vers 250-500 Hz."});
    if (band(5) < -10.f && band(6) < -10.f) issues.push_back({4, "Terne : il manque de la brillance au-dessus de 4 kHz."});
    if (band(6) > 6.f) issues.push_back({5, "Aigus agressifs (air) : hats ou cymbales qui fatiguent l'oreille."});

    std::ranges::sort(issues, std::greater{}, &Issue::severity);
    int score = 10;
    for (const auto& i : issues) score -= i.severity >= 8 ? 3 : i.severity >= 5 ? 2 : 1;
    score = std::max(score, 1);
    std::string s = std::format("Diagnostic de l'ingé son : note technique {}/10 ({}).", score,
                                score >= 8 ? "propre" : score >= 5 ? "correct mais perfectible" : "à retravailler sérieusement");
    if (issues.empty()) s += " Rien d'alarmant : niveau, dynamique, phase et spectre sont dans les clous.";
    for (const auto& i : issues) s += "\n- " + i.text;
    return s;
}

OmnisAnalyzer::OmnisAnalyzer() : ring_(1u << 18), mask_((1u << 18) - 1) {
    // Pondération K (BS.1770-4) à 48 kHz : shelf haut puis passe-haut RLB.
    const Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
    const Biquad hp{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    kShelfL_ = kShelfR_ = shelf;
    kHpL_ = kHpR_ = hp;

    fftIn_.assign(kFftSize, 0.f);
    fftBuf_.resize(kFftSize);
    window_.resize(kFftSize);
    for (std::size_t i = 0; i < kFftSize; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2.f * std::numbers::pi_v<float> * static_cast<float>(i) / (kFftSize - 1));

    // Bandes -> bins FFT, et part d'énergie de référence : DSP ∝ f^-1.5 (-4,5 dB/octave).
    double refTotal = 0;
    std::array<double, MixMetrics::kBands> ref{};
    for (int b = 0; b < MixMetrics::kBands; ++b) {
        const double f1 = kEdges[static_cast<size_t>(b)], f2 = kEdges[static_cast<size_t>(b) + 1];
        bandBins_[static_cast<size_t>(b)] = {static_cast<std::size_t>(std::ceil(f1 * kFftSize / kFs)),
                                             static_cast<std::size_t>(std::floor(f2 * kFftSize / kFs))};
        ref[static_cast<size_t>(b)] = 2.0 * (1.0 / std::sqrt(f1) - 1.0 / std::sqrt(f2));
        refTotal += ref[static_cast<size_t>(b)];
    }
    for (int b = 0; b < MixMetrics::kBands; ++b) refShareDb_[static_cast<size_t>(b)] = ToDb(ref[static_cast<size_t>(b)] / refTotal);
}

void OmnisAnalyzer::SetAlertCallback(AlertCallback cb) {
    std::scoped_lock lock(mutex_);
    alert_ = std::move(cb);
}

void OmnisAnalyzer::Start() {
    if (!thread_.joinable()) thread_ = std::jthread([this](std::stop_token st) { Run(st); });
}

void OmnisAnalyzer::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void OmnisAnalyzer::Reset() {
    std::scoped_lock lock(mutex_);
    blocks_.clear();
    bandEnergy_.fill(0.0);
    bandsValid_ = false;
}

void OmnisAnalyzer::Push(const float* in, std::size_t frames) noexcept {
    const std::size_t n = frames * 2;
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t r = read_.load(std::memory_order_acquire);
    if (ring_.size() - (w - r) < n) return;  // analyse en retard : on perd ce bloc, jamais la capture
    for (std::size_t i = 0; i < n; ++i) ring_[(w + i) & mask_] = in[i];
    write_.store(w + n, std::memory_order_release);
}

void OmnisAnalyzer::Run(std::stop_token st) {
#if defined(_WIN32)
    log::SetCurrentThreadName(L"Companion.OmnisAnalyzer");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // rapport §2
#endif
    while (!st.stop_requested()) {
        DrainNow();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void OmnisAnalyzer::DrainNow() {
    const std::size_t w = write_.load(std::memory_order_acquire);
    std::size_t r = read_.load(std::memory_order_relaxed);
    for (; r + 1 < w; r += 2) ProcessFrame(ring_[r & mask_], ring_[(r + 1) & mask_]);
    read_.store(r, std::memory_order_release);
}

void OmnisAnalyzer::ProcessFrame(float l, float r) {
    const double kl = kHpL_.Process(kShelfL_.Process(l));
    const double kr = kHpR_.Process(kShelfR_.Process(r));
    cur_.kL += kl * kl;
    cur_.kR += kr * kr;
    cur_.l2 += static_cast<double>(l) * l;
    cur_.r2 += static_cast<double>(r) * r;
    cur_.lr += static_cast<double>(l) * r;
    cur_.peak = std::max({cur_.peak, std::abs(l), std::abs(r)});
    // Clip = au moins 3 échantillons consécutifs à pleine échelle sur un canal.
    runL_ = std::abs(l) >= 0.999f ? runL_ + 1 : 0;
    runR_ = std::abs(r) >= 0.999f ? runR_ + 1 : 0;
    if (runL_ == 3 || runR_ == 3) ++cur_.clips;

    fftIn_[fftFill_++] = 0.5f * (l + r);
    if (fftFill_ == kFftSize) {
        RunFft();
        std::copy(fftIn_.begin() + kHop, fftIn_.end(), fftIn_.begin());
        fftFill_ = kFftSize - kHop;
    }
    if (++cur_.n == kBlockFrames) FinishBlock();
}

void OmnisAnalyzer::RunFft() {
    double energy = 0;
    for (std::size_t i = 0; i < kFftSize; ++i) {
        fftBuf_[i] = {fftIn_[i] * window_[i], 0.f};
        energy += static_cast<double>(fftIn_[i]) * fftIn_[i];
    }
    if (energy / kFftSize < 1e-7) return;  // silence (< -70 dBFS) : on ne fausse pas le lissage
    Fft(fftBuf_);
    std::array<double, MixMetrics::kBands> e{};
    for (int b = 0; b < MixMetrics::kBands; ++b)
        for (std::size_t k = bandBins_[static_cast<size_t>(b)].first; k <= bandBins_[static_cast<size_t>(b)].second; ++k)
            e[static_cast<size_t>(b)] += std::norm(fftBuf_[k]);
    const double alpha = 1.0 - std::exp(-static_cast<double>(kHop) / (kFs * 3.0));  // ~3 s
    std::scoped_lock lock(mutex_);
    for (int b = 0; b < MixMetrics::kBands; ++b) {
        auto& acc = bandEnergy_[static_cast<size_t>(b)];
        acc = bandsValid_ ? acc + alpha * (e[static_cast<size_t>(b)] - acc) : e[static_cast<size_t>(b)];
    }
    bandsValid_ = true;
}

void OmnisAnalyzer::FinishBlock() {
    AlertCallback cb;
    MixMetrics m;
    bool clipAlert = false, phaseAlert = false;
    {
        std::scoped_lock lock(mutex_);
        blocks_.push_back(cur_);
        if (blocks_.size() > kWindowBlocks) blocks_.pop_front();
        cur_ = Block{};
        m = ComputeLocked();
        cb = alert_;
        const auto now = std::chrono::steady_clock::now();
        if (cb && blocks_.back().clips > 0 && now - lastAlert_[0] > std::chrono::seconds(30)) {
            clipAlert = true;
            lastAlert_[0] = now;
        }
        if (cb && m.seconds >= 2.5f && m.correlation < -0.2f && now - lastAlert_[1] > std::chrono::seconds(60)) {
            phaseAlert = true;
            lastAlert_[1] = now;
        }
    }
    if (clipAlert) cb(MixAlert::Clipping, m);
    if (phaseAlert) cb(MixAlert::PhaseProblem, m);
}

MixMetrics OmnisAnalyzer::ComputeLocked() const {
    MixMetrics m;
    Block sum;
    std::size_t loud = 0;
    for (const auto& b : blocks_) {
        // Blocs silencieux exclus (comme le "gating" absolu de BS.1770 à -70 LUFS).
        const double ms = (b.l2 + b.r2) / (2.0 * std::max<std::size_t>(b.n, 1));
        if (ms < 1e-7) continue;
        ++loud;
        sum.kL += b.kL; sum.kR += b.kR; sum.l2 += b.l2; sum.r2 += b.r2; sum.lr += b.lr;
        sum.peak = std::max(sum.peak, b.peak);
        sum.clips += b.clips;
        sum.n += b.n;
    }
    m.seconds = static_cast<float>(loud) * 0.1f;
    if (sum.n == 0) return m;
    m.silent = false;
    const double n = static_cast<double>(sum.n);
    m.lufsShort = static_cast<float>(-0.691 + ToDb((sum.kL + sum.kR) / n));
    m.rmsDb = static_cast<float>(ToDb((sum.l2 + sum.r2) / (2.0 * n)));
    m.peakDb = static_cast<float>(20.0 * std::log10(std::max<double>(sum.peak, 1e-6)));
    m.crestDb = m.peakDb - m.rmsDb;
    m.clips = sum.clips;
    m.correlation = static_cast<float>(sum.lr / std::sqrt(std::max(sum.l2 * sum.r2, 1e-24)));
    const double mid = (sum.l2 + sum.r2 + 2 * sum.lr) / 4, side = (sum.l2 + sum.r2 - 2 * sum.lr) / 4;
    m.width = static_cast<float>(side / std::max(mid, 1e-12));
    if (bandsValid_) {
        double total = 0;
        for (double e : bandEnergy_) total += e;
        for (int b = 0; b < MixMetrics::kBands; ++b)
            m.bandDb[static_cast<size_t>(b)] =
                static_cast<float>(ToDb(bandEnergy_[static_cast<size_t>(b)] / std::max(total, 1e-24)) - refShareDb_[static_cast<size_t>(b)]);
    }
    return m;
}

MixMetrics OmnisAnalyzer::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return ComputeLocked();
}

}  // namespace companion
