#pragma once
// ClaudeClient : niveau 2 de la cascade (dernier recours), API Messages en HTTPS via WinHTTP.
//
//  - Modèle par défaut : claude-opus-5-5, effort explicite (défaut "medium").
//  - Prompt Caching : le prompt système porte cache_control (mis en cache au-delà de
//    ~512 tokens sur ce modèle ; en dessous, la requête fonctionne simplement sans cache).
//  - Repli serveur en cas de refus de sécurité : fallbacks "default" (bêta
//    server-side-fallback-2026-07-01). stop_reason "refusal" est toujours vérifié.
//  - Clé API : variable d'environnement ANTHROPIC_API_KEY, sinon Gestionnaire
//    d'identification Windows (identifiant générique "CompanionAI/anthropic").
//    Jamais en clair dans un fichier du projet.

#include "core/MemoryDB.hpp"  // ChatTurn
#include "network/HttpClient.hpp"

#include <optional>
#include <string>
#include <vector>

namespace companion {

struct ClaudeImage {
    std::string mediaType = "image/jpeg";
    std::string base64;
};

struct ClaudeRequest {
    std::string system;
    std::vector<ChatTurn> history;  // tours user/assistant précédents (optionnel)
    std::string userText;
    std::optional<ClaudeImage> image;  // Vision (capture DXGI, étape 3)
    int maxTokens = 8000;              // inclut la réflexion adaptative du modèle
};

struct ClaudeReply {
    bool ok = false;
    std::string text;
    std::string error;
    std::string stopReason;
    int inputTokens = 0, outputTokens = 0, cacheReadTokens = 0;
};

class ClaudeClient {
public:
    ClaudeClient(HttpClient& http, std::string model, std::string effort);

    [[nodiscard]] bool HasCredentials() const { return !apiKey_.empty(); }
    ClaudeReply Send(const ClaudeRequest& req);

    static std::string Base64(const void* data, size_t size);

private:
    static std::string LoadApiKey();

    HttpClient& http_;
    std::string model_;
    std::string effort_;
    std::string apiKey_;
};

}  // namespace companion
