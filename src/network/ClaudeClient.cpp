#include "network/ClaudeClient.hpp"

#include "core/Log.hpp"

#include <windows.h>
#include <wincred.h>

#include <nlohmann/json.hpp>

#include <cstdlib>

namespace companion {

ClaudeClient::ClaudeClient(HttpClient& http, std::string model, std::string effort)
    : http_(http), model_(std::move(model)), effort_(std::move(effort)), apiKey_(LoadApiKey()) {
    log::Info("Claude: {}", HasCredentials() ? "clé API trouvée" : "aucune clé API (niveau 2 désactivé)");
}

std::string ClaudeClient::LoadApiKey() {
    if (const char* env = std::getenv("ANTHROPIC_API_KEY"); env && *env) return env;
    PCREDENTIALW cred = nullptr;
    if (CredReadW(L"CompanionAI/anthropic", CRED_TYPE_GENERIC, 0, &cred) && cred) {
        std::string key(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        CredFree(cred);
        // Saisie via cmdkey : le secret est stocké en UTF-16 -> on le reconvertit.
        if (key.size() >= 2 && key[1] == '\0') {
            std::wstring w(reinterpret_cast<const wchar_t*>(key.data()), key.size() / 2);
            key = log::Narrow(w);
        }
        return key;
    }
    return {};
}

std::string ClaudeClient::Base64(const void* data, size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned v = (p[i] << 16) | (i + 1 < size ? p[i + 1] << 8 : 0) | (i + 2 < size ? p[i + 2] : 0);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kTable[v & 63] : '=');
    }
    return out;
}

ClaudeReply ClaudeClient::Send(const ClaudeRequest& req) {
    ClaudeReply reply;
    if (!HasCredentials()) {
        reply.error = "aucune clé API";
        return reply;
    }

    using json = nlohmann::json;
    json messages = json::array();
    for (const auto& t : req.history)
        if (t.role == "user" || t.role == "assistant") messages.push_back({{"role", t.role}, {"content", t.content}});
    json content = json::array();
    if (req.image)
        content.push_back({{"type", "image"},
                           {"source", {{"type", "base64"}, {"media_type", req.image->mediaType}, {"data", req.image->base64}}}});
    content.push_back({{"type", "text"}, {"text", req.userText}});
    messages.push_back({{"role", "user"}, {"content", content}});

    json body = {
        {"model", model_},
        {"max_tokens", req.maxTokens},
        // Prompt système stable en tête, marqué pour le cache (préfixe identique d'un appel à l'autre).
        {"system", json::array({{{"type", "text"}, {"text", req.system}, {"cache_control", {{"type", "ephemeral"}}}}})},
        {"messages", messages},
        {"output_config", {{"effort", effort_}}},
        {"fallbacks", "default"},
    };

    HttpRequest http;
    http.method = L"POST";
    http.url = "https://api.anthropic.com/v1/messages";
    http.timeoutMs = 180000;
    http.body = body.dump();
    const std::wstring key(apiKey_.begin(), apiKey_.end());  // clé ASCII
    http.headers = {{L"content-type", L"application/json"},
                    {L"x-api-key", key},
                    {L"anthropic-version", L"2026-10-21"},
                    {L"anthropic-beta", L"server-side-fallback-2026-07-01"}};

    const HttpResponse res = http_.Send(http);
    if (res.status == 0) {
        reply.error = res.error;
        return reply;
    }
    try {
        const auto j = json::parse(res.body);
        if (!res.Ok()) {
            reply.error = "HTTP " + std::to_string(res.status) + " : " +
                          j.value("error", json::object()).value("message", std::string{"erreur inconnue"});
            return reply;
        }
        reply.stopReason = j.value("stop_reason", std::string{});
        if (const auto& u = j.value("usage", json::object()); !u.empty()) {
            reply.inputTokens = u.value("input_tokens", 0);
            reply.outputTokens = u.value("output_tokens", 0);
            reply.cacheReadTokens = u.value("cache_read_input_tokens", 0);
        }
        if (reply.stopReason == "refusal") {
            reply.error = "Claude a refusé la demande";
            return reply;
        }
        for (const auto& block : j.at("content"))
            if (block.value("type", std::string{}) == "text") reply.text += block.value("text", std::string{});
        reply.ok = !reply.text.empty();
        if (!reply.ok) reply.error = "réponse vide (" + reply.stopReason + ")";
        log::Info("Claude: {} tokens entrée ({} en cache), {} sortie", reply.inputTokens, reply.cacheReadTokens,
                  reply.outputTokens);
    } catch (const std::exception& e) {
        reply.error = std::string("réponse illisible : ") + e.what();
    }
    return reply;
}

}  // namespace companion
