#pragma once
// TempoStretch : la voix neuronale parle trop posément (mesuré : 4,3 à 5,6 syllabes/s, jusqu'à 0,8 s de blanc avant
// de commencer et 1,2 s de pause au milieu ; une conversation animée, c'est 6 à 7 syllabes/s). Deux étages, en flux :
//
//  1) blancs : le silence de tête est réduit à 30 ms, toute pause interne plafonnée à 200 ms (une respiration) ;
//  2) tempo : WSOLA (recouvrement-addition synchronisé sur la forme d'onde) — plus rapide SANS monter dans les aigus,
//     contrairement au varispeed. Trames de 20 ms, recherche ±5 ms de la meilleure jonction.
//
// Logique pure, sans allocation en régime établi (tampons réservés). Testée hors Windows (tools/sim/tempotest.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace companion {

class TempoStretch {
public:
    void Prepare(int sampleRate) {
        rate_ = sampleRate;
        n_ = sampleRate / 50;   // 20 ms
        hop_ = n_ / 2;          // 50 % de recouvrement
        search_ = sampleRate / 200;  // ±5 ms
        win_.resize(static_cast<std::size_t>(n_));
        for (int i = 0; i < n_; ++i) win_[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(6.28318530718f * static_cast<float>(i) / static_cast<float>(n_));
        in_.reserve(static_cast<std::size_t>(sampleRate) * 2);
        ola_.assign(static_cast<std::size_t>(n_), 0.f);
        Reset();
    }
    void SetTempo(float t) noexcept { tempo_ = std::clamp(t, 0.8f, 1.6f); }
    void SetMaxPause(float sec) noexcept { maxPause_ = static_cast<std::size_t>(sec * static_cast<float>(rate_)); }
    void Reset() noexcept {
        in_.clear();
        std::fill(ola_.begin(), ola_.end(), 0.f);
        pos_ = 0.0;
        prev_ = -1;
        quiet_ = 0;
        started_ = false;
        env_ = 0.f;
    }

    // Remplace le morceau par sa version resserrée (la longueur change).
    void Process(std::vector<float>& chunk) {
        Gate(chunk);
        in_.insert(in_.end(), chunk.begin(), chunk.end());
        chunk.clear();
        if (tempo_ > 0.99f && tempo_ < 1.01f) {  // pas d'étirement : on rend tel quel
            chunk.swap(in_);
            in_.clear();
            return;
        }
        Run(chunk, false);
    }
    // Fin de la phrase : ce qui reste dans les tampons.
    void Flush(std::vector<float>& out) {
        out.clear();
        if (tempo_ > 0.99f && tempo_ < 1.01f) {
            out.swap(in_);
            in_.clear();
        } else {
            Run(out, true);
            out.insert(out.end(), ola_.begin(), ola_.begin() + hop_);
        }
        Reset();
    }

private:
    int rate_ = 24000, n_ = 480, hop_ = 240, search_ = 120;
    float tempo_ = 1.f;
    std::size_t maxPause_ = 4800;  // 200 ms
    std::vector<float> win_, in_, ola_;
    double pos_ = 0.0;  // début (dans in_) de la prochaine trame idéale
    long prev_ = -1;    // début de la trame précédente retenue
    std::size_t quiet_ = 0;
    bool started_ = false;
    float env_ = 0.f;

    // Blancs : enveloppe rapide, seuil fixe (la voix synthétique a un fond très propre).
    void Gate(std::vector<float>& c) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < c.size(); ++i) {
            const float a = std::abs(c[i]);
            env_ = a > env_ ? a : env_ * 0.999f;
            const bool silent = env_ < 0.008f;
            if (!started_) {
                if (silent) continue;  // silence de tête : supprimé (le fondu de la fenêtre évite le clic)
                started_ = true;
            }
            quiet_ = silent ? quiet_ + 1 : 0;
            if (quiet_ > maxPause_) continue;  // pause trop longue : coupée à maxPause_
            c[w++] = c[i];
        }
        c.resize(w);
    }

    void Run(std::vector<float>& out, bool flush) {
        const double step = hop_ * static_cast<double>(tempo_);
        for (;;) {
            const long ideal = static_cast<long>(pos_);
            if (ideal + search_ + n_ > static_cast<long>(in_.size())) {
                if (!flush || ideal + n_ > static_cast<long>(in_.size())) break;
            }
            long best = ideal;
            if (prev_ >= 0) {
                // La jonction la plus naturelle : ce qui ressemble le plus à la suite de la trame précédente.
                const long nat = prev_ + hop_;
                double bestScore = -1e30;
                const long lo = std::max(0L, ideal - search_), hi = std::min(static_cast<long>(in_.size()) - n_, ideal + search_);
                for (long c = lo; c <= hi; c += 2) {
                    double s = 0;
                    for (int k = 0; k < hop_; k += 2) s += static_cast<double>(in_[static_cast<std::size_t>(c + k)]) * in_[static_cast<std::size_t>(nat + k)];
                    if (s > bestScore) {
                        bestScore = s;
                        best = c;
                    }
                }
            }
            for (int k = 0; k < n_; ++k) ola_[static_cast<std::size_t>(k)] += in_[static_cast<std::size_t>(best + k)] * win_[static_cast<std::size_t>(k)];
            out.insert(out.end(), ola_.begin(), ola_.begin() + hop_);
            std::copy(ola_.begin() + hop_, ola_.end(), ola_.begin());
            std::fill(ola_.begin() + (n_ - hop_), ola_.end(), 0.f);
            prev_ = best;
            pos_ += step;
            // On jette ce qui ne servira plus (garde de quoi chercher autour de la prochaine trame).
            const long keep = std::min(static_cast<long>(pos_) - search_, prev_);
            if (keep > n_ * 4) {
                in_.erase(in_.begin(), in_.begin() + keep);
                pos_ -= static_cast<double>(keep);
                prev_ -= keep;
            }
        }
    }
};

}  // namespace companion
