#include "core/ModelPlanner.hpp"

#include <algorithm>
#include <format>

namespace companion {

const std::vector<ModelInfo>& ModelPlanner::Brains() {
    static const std::vector<ModelInfo> brains{
        {"qwen3.5-0.8b", "Qwen3.5 0.8B", "Qwen3.5-0.8B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf", 532517120ull, 0.9},
        {"qwen3.5-2b", "Qwen3.5 2B", "Qwen3.5-2B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf", 1280835840ull, 1.65},
        {"qwen3.5-4b", "Qwen3.5 4B", "Qwen3.5-4B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf", 2740937888ull, 3.05},
        {"qwen3.5-9b", "Qwen3.5 9B", "Qwen3.5-9B-Q4_K_M.gguf",
         "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q4_K_M.gguf", 5680522464ull, 5.9},
    };
    return brains;
}

const ModelInfo* ModelPlanner::FindByFile(const std::string& file) {
    for (const auto& m : Brains())
        if (file == m.file) return &m;
    return nullptr;
}

VoicePlan ModelPlanner::PlanVoice(const HardwareInfo& hw, bool neuralWanted) {
    VoicePlan v;
    // Voix neuronale seulement si elle tient confortablement dans la moitié de la VRAM avec un cerveau
    // correct à côté : cartes de 8 Go et plus. Sinon Piper (CPU) : la VRAM va au cerveau.
    v.neural = neuralWanted && hw.vramGiB >= 7.5;
    v.whisperFile = hw.ramGiB < 7.5 ? "ggml-tiny-q5_1.bin" : "ggml-base-q5_1.bin";
    return v;
}

BrainChoice ModelPlanner::OnCpu(const HardwareInfo& hw, bool gaming, const std::string& rule) {
    // CPU : en jeu le plus léger (le jeu garde le processeur) ; sinon selon la RAM (le modèle y est chargé).
    const auto& b = Brains();
    const ModelInfo* m = &b[0];
    if (!gaming) {
        if (hw.ramGiB >= 23.5) m = &b[2];
        else if (hw.ramGiB >= 11.5) m = &b[1];
    } else if (hw.ramGiB >= 15.5) {
        m = &b[1];
    }
    return {m, false, rule + std::format(" → {} sur le processeur ({:.0f} Go de RAM)", m->name, hw.ramGiB)};
}

BrainChoice ModelPlanner::Fit(double budgetGiB, const HardwareInfo& hw, bool gaming, const std::string& rule) {
    const ModelInfo* best = nullptr;
    for (const auto& m : Brains())
        if (m.vramNeedGiB <= budgetGiB) best = &m;
    if (!best) return OnCpu(hw, gaming, rule + std::format(" : {:.1f} Go, trop peu pour le plus petit modèle", budgetGiB));
    return {best, true, rule + std::format(" : {:.1f} Go → {} sur la carte graphique (~{:.1f} Go)", budgetGiB, best->name, best->vramNeedGiB)};
}

BrainChoice ModelPlanner::PlanNormal(const HardwareInfo& hw, bool neuralVoice) {
    if (hw.vramGiB < 0.9) return OnCpu(hw, false, "Pas de carte graphique dédiée");
    const double budget = hw.vramGiB * kNormalShare - (neuralVoice ? kNeuralVoiceGiB : 0.0);
    return Fit(budget, hw, false,
               std::format("Hors jeu, moitié de {:.0f} Go{}", hw.vramGiB, neuralVoice ? " moins la voix neuronale" : ""));
}

bool ModelPlanner::GameSaturatesGpu(double load, bool wasBusy) {
    if (load < 0.0) return true;
    return load >= (wasBusy ? kGpuBusyLeave : kGpuBusyEnter);
}

BrainChoice ModelPlanner::PlanGaming(const HardwareInfo& hw, bool gpuSaturated) {
    if (hw.vramGiB < 0.9) return OnCpu(hw, true, "En jeu, pas de carte graphique dédiée");
    if (gpuSaturated)
        return OnCpu(hw, true, hw.gameGpuLoad >= 0.0
                                   ? std::format("En jeu, la carte graphique est prise à {:.0f} %", hw.gameGpuLoad * 100.0)
                                   : std::string("En jeu, charge de la carte inconnue"));
    double budget = hw.vramGiB * kGamingShare;
    std::string rule = std::format("En jeu, un quart de {:.0f} Go", hw.vramGiB);
    // Gros jeu : s'il reste peu de VRAM libre, on prend encore moins (le jeu passe avant tout).
    if (hw.vramFreeGiB >= 0.0 && hw.vramFreeGiB - kGameMarginGiB < budget) {
        budget = std::max(0.0, hw.vramFreeGiB - kGameMarginGiB);
        rule = std::format("En jeu, VRAM libre {:.1f} Go moins 1 Go pour le jeu", hw.vramFreeGiB);
    }
    return Fit(budget, hw, true, rule);
}

std::vector<Download> ModelPlanner::Required(const HardwareInfo& hw, bool neuralWanted) {
    std::vector<Download> d;
    const VoicePlan v = PlanVoice(hw, neuralWanted);
    const BrainChoice normal = PlanNormal(hw, v.neural);
    // En jeu, sans mesure de VRAM libre, on prépare le modèle du quart (le plus fréquent).
    HardwareInfo calm = hw;
    calm.vramFreeGiB = -1.0;
    const BrainChoice gaming = PlanGaming(calm, false);
    const BrainChoice gamingCpu = PlanGaming(calm, true);  // jeu qui sature la carte : cerveau sur le processeur
    // + le plus petit, toujours : filet de sécurité quand un gros jeu ne laisse presque plus de VRAM.
    for (const auto* m : {normal.model, gaming.model, gamingCpu.model, &Brains().front()})
        if (m && std::ranges::none_of(d, [&](const Download& x) { return x.file == m->file; }))
            d.push_back({m->file, m->url, m->bytes});
    const std::string w = v.whisperFile;
    d.push_back({w, "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + w, w == "ggml-tiny-q5_1.bin" ? 32152673ull : 59707625ull});
    d.push_back({"fr_FR-siwis-medium.onnx",
                 "https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium/fr_FR-siwis-medium.onnx", 63221984ull});
    d.push_back({"fr_FR-siwis-medium.onnx.json",
                 "https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium/fr_FR-siwis-medium.onnx.json", 6369ull});
    if (v.neural) {
        d.push_back({"qwen-talker-0.6b-base-Q8_0.gguf",
                     "https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/main/qwen-talker-0.6b-base-Q8_0.gguf", 992615488ull});
        d.push_back({"qwen-tokenizer-12hz-Q8_0.gguf",
                     "https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/main/qwen-tokenizer-12hz-Q8_0.gguf", 291150624ull});
    }
    return d;
}

}  // namespace companion
