#include "core/Config.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace companion {

CompanionConfig LoadConfig(const std::filesystem::path& jsonFile) {
    CompanionConfig cfg;
    std::ifstream f(jsonFile);
    if (!f) return cfg;
    try {
        const auto j = nlohmann::json::parse(f, nullptr, true, /*ignore_comments=*/true);
        cfg.name = j.value("name", cfg.name);
        if (j.contains("llmModel")) {
            const auto utf8 = j["llmModel"].get<std::string>();
            cfg.llmModel = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
        if (j.contains("llmMmproj")) {
            const auto utf8 = j["llmMmproj"].get<std::string>();
            cfg.llmMmproj = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }
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
