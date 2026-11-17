#include "core/Config.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace companion {

CompanionConfig LoadConfig(const std::filesystem::path& jsonFile) {
    CompanionConfig cfg;
    std::ifstream f(jsonFile);
    if (!f) return cfg;
    try {
        const auto j = nlohmann::json::parse(f, nullptr, true, /*ignore_comments=*/true);
        cfg.name = j.value("name", cfg.name);
        cfg.avatarScale = j.value("avatarScale", cfg.avatarScale);
        cfg.avatarOpacity = j.value("avatarOpacity", cfg.avatarOpacity);
        cfg.avatarCorner = j.value("avatarCorner", cfg.avatarCorner);
        cfg.expressiveness = j.value("expressiveness", cfg.expressiveness);
        cfg.fxIntensity = j.value("fxIntensity", cfg.fxIntensity);
        cfg.speechRate = j.value("speechRate", cfg.speechRate);
        cfg.tone = std::clamp(j.value("tone", cfg.tone), 0, 2);
        cfg.backchannels = j.value("backchannels", cfg.backchannels);
        cfg.neuralVoice = j.value("neuralVoice", cfg.neuralVoice);
        cfg.voiceQuirks = std::clamp(j.value("voiceQuirks", cfg.voiceQuirks), 0.f, 1.f);
        cfg.micEffect = std::clamp(j.value("micEffect", cfg.micEffect), 0.f, 1.f);
        cfg.pitchLife = std::clamp(j.value("pitchLife", cfg.pitchLife), 0.f, 1.f);
        cfg.neuralVoiceInGame = j.value("neuralVoiceInGame", cfg.neuralVoiceInGame);
        cfg.neuralVoiceDevice = j.value("neuralVoiceDevice", cfg.neuralVoiceDevice);
        cfg.bargeIn = j.value("bargeIn", cfg.bargeIn);
        cfg.emotionDetection = j.value("emotionDetection", cfg.emotionDetection);
        cfg.voiceActivity = j.value("voiceActivity", cfg.voiceActivity);
        if (j.contains("llmModel")) {
            const auto utf8 = j["llmModel"].get<std::string>();
            cfg.llmModel = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
        if (j.contains("llmGamingModel")) {
            const auto utf8 = j["llmGamingModel"].get<std::string>();
            cfg.llmGamingModel = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
        cfg.avatarInGame = std::clamp(j.value("avatarInGame", cfg.avatarInGame), 0, 2);
        cfg.gameIsolation = j.value("gameIsolation", cfg.gameIsolation);
        if (j.contains("llmMmproj")) {
            const auto utf8 = j["llmMmproj"].get<std::string>();
            cfg.llmMmproj = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
        cfg.autoTraining = j.value("autoTraining", cfg.autoTraining);
        cfg.autoTrainingEveryMinutes = j.value("autoTrainingEveryMinutes", cfg.autoTrainingEveryMinutes);
        cfg.trainerMemoryLimitMb = j.value("trainerMemoryLimitMb", cfg.trainerMemoryLimitMb);
        cfg.studioProcess = j.value("studioProcess", cfg.studioProcess);
        cfg.studioAlerts = j.value("studioAlerts", cfg.studioAlerts);
        if (j.contains("apps")) {
            cfg.apps.clear();
            for (const auto& a : j["apps"])
                cfg.apps.push_back({a.at("name").get<std::string>(), a.value("aliases", std::vector<std::string>{}),
                                    a.at("target").get<std::string>(), a.value("args", std::string{})});
        }
        cfg.llmContext = j.value("llmContext", cfg.llmContext);
        cfg.llmGpuLayers = j.value("llmGpuLayers", cfg.llmGpuLayers);
        cfg.llmThreads = j.value("llmThreads", cfg.llmThreads);
        cfg.llmThreadsGaming = j.value("llmThreadsGaming", cfg.llmThreadsGaming);
        cfg.llmUnloadAfterMinutes = j.value("llmUnloadAfterMinutes", cfg.llmUnloadAfterMinutes);
        cfg.userAgent = j.value("userAgent", cfg.userAgent);
        cfg.knowledgeMaxAgeDays = j.value("knowledgeMaxAgeDays", cfg.knowledgeMaxAgeDays);
        cfg.cloudEnabled = j.value("cloudEnabled", cfg.cloudEnabled);
        cfg.cloudAsksPermission = j.value("cloudAsksPermission", cfg.cloudAsksPermission);
        cfg.claudeModel = j.value("claudeModel", cfg.claudeModel);
        cfg.claudeEffort = j.value("claudeEffort", cfg.claudeEffort);
        if (j.contains("wikis")) {
            cfg.wikis.clear();
            for (const auto& w : j["wikis"])
                cfg.wikis.push_back({w.at("host").get<std::string>(), w.value("apiPath", std::string{"/api.php"}),
                                     w.value("keywords", std::vector<std::string>{})});
        }
        log::Info("Config: {} chargée", log::Narrow(jsonFile.wstring()));
    } catch (const std::exception& e) {
        log::Info("Config: companion.json invalide ({}), valeurs par défaut", e.what());
    }
    return cfg;
}

}  // namespace companion

namespace companion {

bool SaveConfig(const CompanionConfig& cfg, const std::filesystem::path& jsonFile) {
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in(jsonFile); in) {
        try {
            j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
        } catch (const std::exception&) {
            j = nlohmann::json::object();  // fichier illisible : on repart propre
        }
    }
    j["name"] = cfg.name;
    j["avatarScale"] = cfg.avatarScale;
    j["avatarOpacity"] = cfg.avatarOpacity;
    j["avatarCorner"] = cfg.avatarCorner;
    j["expressiveness"] = cfg.expressiveness;
    j["fxIntensity"] = cfg.fxIntensity;
    j["neuralVoice"] = cfg.neuralVoice;
    j["voiceQuirks"] = cfg.voiceQuirks;
    j["micEffect"] = cfg.micEffect;
    j["pitchLife"] = cfg.pitchLife;
    j["neuralVoiceInGame"] = cfg.neuralVoiceInGame;
    j["speechRate"] = cfg.speechRate;
    j["tone"] = cfg.tone;
    j["backchannels"] = cfg.backchannels;
    j["bargeIn"] = cfg.bargeIn;
    j["emotionDetection"] = cfg.emotionDetection;
    j["voiceActivity"] = cfg.voiceActivity;
    j["autoTraining"] = cfg.autoTraining;
    j["cloudEnabled"] = cfg.cloudEnabled;
    j["cloudAsksPermission"] = cfg.cloudAsksPermission;
    std::ofstream out(jsonFile, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << "\n";
    log::Info("Config: réglages enregistrés");
    return static_cast<bool>(out);
}

}  // namespace companion
