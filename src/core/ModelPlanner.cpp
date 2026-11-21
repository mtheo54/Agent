#include "core/ModelPlanner.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace companion {

const char* ToString(GamePreset p) noexcept {
    switch (p) {
        case GamePreset::Gpu: return "carte";
        case GamePreset::GpuLight: return "carte-leger";
        case GamePreset::Cpu: return "processeur";
        case GamePreset::CpuLight: return "processeur-leger";
        default: return "auto";
    }
}

std::optional<GamePreset> ParseGamePreset(std::string_view s) {
    std::string t;
    for (unsigned char c : s) {
        if (c == 0xC3) continue;  // « é » en UTF-8 : on garde l'octet suivant, ramené à « e »
        t += c == 0xA9 ? 'e' : static_cast<char>(std::tolower(c));
    }
    const bool light = t.find("leger") != std::string::npos || t.find("light") != std::string::npos ||
                       t.find("lite") != std::string::npos;
    if (t == "auto" || t.empty()) return GamePreset::Auto;
    if (t.starts_with("carte") || t.starts_with("gpu")) return light ? GamePreset::GpuLight : GamePreset::Gpu;
    if (t.starts_with("processeur") || t.starts_with("cpu")) return light ? GamePreset::CpuLight : GamePreset::Cpu;
    return std::nullopt;
}

GamePreset Lighter(GamePreset p) noexcept {
    switch (p) {
        case GamePreset::Gpu: return GamePreset::GpuLight;
        case GamePreset::GpuLight: return GamePreset::CpuLight;
        case GamePreset::Cpu: return GamePreset::CpuLight;
        case GamePreset::CpuLight: return GamePreset::GpuLight;
        default: return p;
    }
}

int Smartness(GamePreset p) noexcept {
    switch (p) {
        case GamePreset::Gpu: return 3;
        case GamePreset::Cpu: return 2;
        default: return 1;
    }
}

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

const ExpertModel& ModelPlanner::Expert() {
    // Qwen3.6-35B-A3B (Apache 2.0), quantification unsloth UD-Q4_K_M. Tailles lues dans l'en-tête GGUF :
    // 2,56 Go hors experts, 19,57 Go d'experts sur 40 couches (256 experts, 8 actifs par mot).
    static const ExpertModel m{"Qwen3.6 35B-A3B", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf",
                               "https://huggingface.co/unsloth/Qwen3.6-35B-A3B-GGUF/resolve/main/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf",
                               22134528992ull, 2.56, 19.57, 40, "Qwen3.6-35B-A3B-mmproj-F16.gguf",
                               "https://huggingface.co/unsloth/Qwen3.6-35B-A3B-GGUF/resolve/main/mmproj-F16.gguf", 899283680ull};
    return m;
}

ExpertPlan ModelPlanner::PlanExpert(const HardwareInfo& hw, bool neuralVoice) {
    const ExpertModel& e = Expert();
    ExpertPlan p;
    if (hw.ramGiB < kExpertMinRamGiB) {
        p.why = std::format("{:.0f} Go de RAM : il en faut 32 pour le cerveau expert", hw.ramGiB);
        return p;
    }
    const double perLayer = e.expertsGiB / e.layers;
    // RAM : on laisse 8 Go au reste (Windows, FL Studio, le navigateur).
    const int maxCpuLayers = static_cast<int>((hw.ramGiB - 8.0) / perLayer);
    double budget = hw.vramGiB >= 0.9 ? hw.vramGiB * kNormalShare - (neuralVoice ? kNeuralVoiceGiB : 0.0) : 0.0;
    // Carte : partie dense + contexte et tampons de calcul (~0,9 Go) ; le reste en couches d'experts.
    double left = budget - e.denseGiB - 0.9;
    if (left < 0.0) {
        p.why = std::format("{:.1f} Go de VRAM pour Omnis : trop peu pour le cerveau expert", budget);
        return p;
    }
    if (left >= 0.9 + 2 * perLayer) {
        p.vision = true;
        left -= 0.9;
    }
    const int gpuLayers = std::clamp(static_cast<int>(left / perLayer), 0, e.layers);
    p.cpuMoeLayers = e.layers - gpuLayers;
    if (p.cpuMoeLayers > maxCpuLayers) {
        p.why = std::format("experts trop gros pour {:.0f} Go de RAM", hw.ramGiB);
        return p;
    }
    p.possible = true;
    p.why = std::format("{} : {} couches d'experts sur la carte, {} en RAM ({:.1f} Go){}", e.name, gpuLayers, p.cpuMoeLayers,
                        p.cpuMoeLayers * perLayer, p.vision ? ", vision comprise" : "");
    return p;
}

std::optional<VisionModule> ModelPlanner::VisionFor(const std::string& brainFile) {
    // Tailles relevées sur les dépôts unsloth (en-tête X-Linked-Size).
    struct V {
        const char* brain;
        const char* repo;
        std::uint64_t bytes;
    };
    static const V v[] = {{"Qwen3.5-9B-Q4_K_M.gguf", "Qwen3.5-9B", 918166080ull},
                          {"Qwen3.5-4B-Q4_K_M.gguf", "Qwen3.5-4B", 672423616ull},
                          {"Qwen3.5-2B-Q4_K_M.gguf", "Qwen3.5-2B", 668227264ull}};
    for (const auto& x : v)
        if (brainFile == x.brain)
            return VisionModule{std::string(x.repo) + "-mmproj-F16.gguf",
                                std::string("https://huggingface.co/unsloth/") + x.repo + "-GGUF/resolve/main/mmproj-F16.gguf", x.bytes};
    return std::nullopt;
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
    v.whisperGamingFile = hw.ramGiB < 7.5 ? "ggml-tiny-q5_1.bin" : "ggml-base-q5_1.bin";
    v.whisperFile = v.whisperGamingFile;
    // Vu sur le terrain : avec « base », la moitié des mots étaient faux en français. Carte de 6 Go et plus : Whisper
    // large-v3-turbo (bien plus juste, ~0,9 Go sur la carte), hors jeu seulement ; en jeu, le léger sur le processeur.
    if (hw.vramGiB >= 5.5 && hw.ramGiB >= 11.5) {
        v.whisperFile = "ggml-large-v3-turbo-q5_0.bin";
        v.whisperGpu = true;
    }
    // Qwen3-ASR : sur le jargon (FL Studio, ASIO, Kai'Sa, Omnis…), avec la banque de mots en contexte, plus juste que Whisper
    // (testé sur un micro dégradé) et 4 fois plus rapide sur processeur. Hors jeu : 1.7B sur la carte (~2,6 Go) ; en jeu :
    // 0.6B sur le processeur (bien meilleur que Whisper base, ~1 Go de RAM).
    if (hw.ramGiB >= 11.5) {
        v.asrGamingFile = "Qwen3-ASR-0.6B-Q8_0.gguf";
        v.asrGamingMmproj = "Qwen3-ASR-0.6B-mmproj-Q8_0.gguf";
        if (hw.vramGiB >= 7.5) {
            v.asrFile = "Qwen3-ASR-1.7B-Q8_0.gguf";
            v.asrMmproj = "Qwen3-ASR-1.7B-mmproj-Q8_0.gguf";
        } else {  // petite carte : le 0.6B partout, sur le processeur
            v.asrFile = v.asrGamingFile;
            v.asrMmproj = v.asrGamingMmproj;
        }
    }
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

BrainChoice ModelPlanner::PlanPreset(const HardwareInfo& hw, GamePreset preset) {
    const ModelInfo* tiny = &Brains().front();
    const std::string tag = std::format("Preset « {} »", ToString(preset));
    switch (preset) {
        case GamePreset::Gpu: {
            BrainChoice c = PlanGaming(hw, false);
            c.why = tag + ", " + c.why;
            return c;
        }
        case GamePreset::GpuLight: {
            const bool fits = hw.vramGiB >= 0.9 && (hw.vramFreeGiB < 0.0 || hw.vramFreeGiB - kGameMarginGiB >= tiny->vramNeedGiB);
            if (fits) return {tiny, true, tag + std::format(" → {} sur la carte graphique", tiny->name)};
            return {tiny, false, tag + std::format(" : pas la place sur la carte → {} sur le processeur", tiny->name)};
        }
        case GamePreset::Cpu: return OnCpu(hw, true, tag);
        case GamePreset::CpuLight: return {tiny, false, tag + std::format(" → {} sur le processeur", tiny->name)};
        default: return PlanGaming(hw, ModelPlanner::GameSaturatesGpu(hw.gameGpuLoad, true));
    }
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
    for (const std::string& w : {std::string(v.whisperGamingFile), std::string(v.whisperFile)}) {
        if (std::ranges::any_of(d, [&](const Download& x) { return x.file == w; })) continue;
        const std::uint64_t bytes = w == "ggml-tiny-q5_1.bin" ? 32152673ull : w == "ggml-large-v3-turbo-q5_0.bin" ? 574041195ull : 59707625ull;
        d.push_back({w, "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + w, bytes});
    }
    struct Asr { const char* file; const char* url; std::uint64_t bytes; };
    static const Asr kAsr[] = {
        {"Qwen3-ASR-1.7B-Q8_0.gguf", "https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main/Qwen3-ASR-1.7B-Q8_0.gguf", 2165035232ull},
        {"Qwen3-ASR-1.7B-mmproj-Q8_0.gguf", "https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF/resolve/main/mmproj-Qwen3-ASR-1.7B-Q8_0.gguf", 355709376ull},
        {"Qwen3-ASR-0.6B-Q8_0.gguf", "https://huggingface.co/ggml-org/Qwen3-ASR-0.6B-GGUF/resolve/main/Qwen3-ASR-0.6B-Q8_0.gguf", 804749536ull},
        {"Qwen3-ASR-0.6B-mmproj-Q8_0.gguf", "https://huggingface.co/ggml-org/Qwen3-ASR-0.6B-GGUF/resolve/main/mmproj-Qwen3-ASR-0.6B-Q8_0.gguf", 214392544ull}};
    for (const char* f : {v.asrFile, v.asrMmproj, v.asrGamingFile, v.asrGamingMmproj})
        for (const auto& a : kAsr)
            if (*f && std::string_view(f) == a.file && std::ranges::none_of(d, [&](const Download& x) { return x.file == a.file; }))
                d.push_back({a.file, a.url, a.bytes});
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
