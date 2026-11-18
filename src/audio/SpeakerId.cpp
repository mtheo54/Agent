#include "audio/SpeakerId.hpp"

#include "core/Log.hpp"

#include <array>
#include <cmath>

#if COMPANION_WITH_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace companion {

#if COMPANION_WITH_ONNX
struct SpeakerId::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "speaker-id"};
    std::unique_ptr<Ort::Session> session;
    std::string input = "x", output = "embedding";
};

SpeakerId::SpeakerId() : impl_(std::make_unique<Impl>()) {}
SpeakerId::~SpeakerId() = default;

bool SpeakerId::Load(const std::filesystem::path& model) {
    if (impl_->session) return true;
    if (!std::filesystem::exists(model)) return false;
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);  // un cœur : jamais en concurrence avec un jeu
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, model.c_str(), so);
        Ort::AllocatorWithDefaultOptions alloc;
        impl_->input = impl_->session->GetInputNameAllocated(0, alloc).get();
        impl_->output = impl_->session->GetOutputNameAllocated(0, alloc).get();
    } catch (const Ort::Exception& e) {
        log::Info("SpeakerId: {}", e.what());
        impl_->session.reset();
        return false;
    }
    log::Info("SpeakerId: modèle de voix chargé");
    return true;
}

bool SpeakerId::Ready() const noexcept { return impl_->session != nullptr; }

std::optional<std::vector<float>> SpeakerId::Embed(const std::vector<float>& pcm16k) {
    if (!impl_->session || pcm16k.size() < KaldiFbank::kRate * 3 / 2) return std::nullopt;
    // 8 s au plus : au-delà, l'empreinte ne gagne rien et le calcul s'allonge.
    const std::size_t n = std::min<std::size_t>(pcm16k.size(), KaldiFbank::kRate * 8);
    int frames = 0;
    std::vector<float> feats = fbank_.Compute(pcm16k.data(), n, 1.f, frames);  // CAM++ : échantillons dans [-1, 1]
    if (frames < 100) return std::nullopt;
    KaldiFbank::SubtractMean(feats, frames);  // « global-mean »
    try {
        const std::array<int64_t, 3> shape{1, frames, KaldiFbank::kMel};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, feats.data(), feats.size(), shape.data(), shape.size());
        const char* inName = impl_->input.c_str();
        const char* outName = impl_->output.c_str();
        auto out = impl_->session->Run(Ort::RunOptions{nullptr}, &inName, &in, 1, &outName, 1);
        const float* e = out[0].GetTensorData<float>();
        const auto count = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
        std::vector<float> emb(e, e + count);
        double norm = 0;
        for (float v : emb) norm += static_cast<double>(v) * v;
        if (norm <= 0) return std::nullopt;
        const float inv = static_cast<float>(1.0 / std::sqrt(norm));
        for (auto& v : emb) v *= inv;
        return emb;
    } catch (const Ort::Exception& e) {
        log::Info("SpeakerId: {}", e.what());
        return std::nullopt;
    }
}
#else
struct SpeakerId::Impl {};
SpeakerId::SpeakerId() : impl_(std::make_unique<Impl>()) {}
SpeakerId::~SpeakerId() = default;
bool SpeakerId::Load(const std::filesystem::path&) { return false; }
bool SpeakerId::Ready() const noexcept { return false; }
std::optional<std::vector<float>> SpeakerId::Embed(const std::vector<float>&) { return std::nullopt; }
#endif

}  // namespace companion
