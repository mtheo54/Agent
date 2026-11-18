#pragma once
// CallVad : découpe en phrases les voix d'un call (16 kHz mono). Seuil qui suit le bruit de fond du call,
// début après 60 ms de voix, fin après 700 ms de silence, segments de 0,6 à 12 s. Logique pure, sans allocation
// en régime établi (le segment garde sa capacité).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace companion {

class CallVad {
public:
    static constexpr int kRate = 16000;
    static constexpr int kFrame = kRate / 50;  // 20 ms
    static constexpr int kStart = 3, kEnd = 35;
    static constexpr std::size_t kMin = kRate * 6 / 10, kMax = kRate * 12;

    // Renvoie true quand un segment est complet : il est échangé dans out.
    bool Feed(const float* x, std::size_t n, std::vector<float>& out) {
        bool ready = false;
        for (std::size_t i = 0; i < n; ++i) {
            frame_[static_cast<std::size_t>(filled_++)] = x[i];
            if (filled_ == kFrame) {
                filled_ = 0;
                ready = Frame(out) || ready;
            }
        }
        return ready;
    }
    [[nodiscard]] bool InSpeech() const noexcept { return inSpeech_; }

private:
    std::array<float, kFrame> frame_{};
    int filled_ = 0;
    float noise_ = 0.003f;
    int loud_ = 0, quiet_ = 0;
    bool inSpeech_ = false;
    std::vector<float> seg_;

    bool Frame(std::vector<float>& out) {
        double sum = 0;
        for (float v : frame_) sum += static_cast<double>(v) * v;
        const float rms = static_cast<float>(std::sqrt(sum / kFrame));
        const bool loud = rms > std::max(0.008f, noise_ * 3.f);
        if (!loud) noise_ += 0.05f * (rms - noise_);  // le bruit de fond n'apprend que sur le silence
        if (!inSpeech_) {
            loud_ = loud ? loud_ + 1 : 0;
            seg_.insert(seg_.end(), frame_.begin(), frame_.end());
            if (seg_.size() > static_cast<std::size_t>(kFrame * kStart)) seg_.erase(seg_.begin(), seg_.begin() + kFrame);
            if (loud_ >= kStart) {
                inSpeech_ = true;
                quiet_ = 0;
            }
            return false;
        }
        seg_.insert(seg_.end(), frame_.begin(), frame_.end());
        quiet_ = loud ? 0 : quiet_ + 1;
        if (quiet_ < kEnd && seg_.size() < kMax) return false;
        inSpeech_ = false;
        loud_ = 0;
        const bool keep = seg_.size() >= kMin + static_cast<std::size_t>(kFrame) * (quiet_ >= kEnd ? kEnd : 0);
        if (keep) out.swap(seg_);
        seg_.clear();
        return keep;
    }
};

}  // namespace companion
