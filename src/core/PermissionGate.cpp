#include "core/PermissionGate.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cctype>

namespace companion {

const char* PermissionGate::ToString(PermissionScope s) noexcept {
    switch (s) {
        case PermissionScope::Web: return "web";
        case PermissionScope::Cloud: return "cloud";
        case PermissionScope::System: return "system";
    }
    return "?";
}

PermissionGate::Reply PermissionGate::Parse(std::string_view transcript) {
    std::string t(transcript);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* w) { return t.find(w) != std::string::npos; };
    // Ordre important : "jamais"/"toujours" priment sur "non"/"oui".
    if (has("jamais")) return Reply::Never;
    if (has("toujours")) return Reply::Always;
    if (has("non") || has("laisse tomber") || has("annule")) return Reply::No;
    if (has("oui") || has("ouais") || has("ok") || has("vas-y") || has("vas y") || has("d'accord"))
        return Reply::Yes;
    return Reply::None;
}

bool PermissionGate::Request(PermissionScope scope, std::string_view target, const std::string& question,
                             std::chrono::seconds timeout, std::stop_token stop) {
    if (auto saved = db_.GetPermission(ToString(scope), target)) return *saved;

    {
        std::scoped_lock lock(mutex_);
        pending_ = true;
        reply_ = Reply::None;
    }
    ask_(question + " (oui, non, toujours, jamais)");

    Reply reply = Reply::None;
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, stop, timeout, [&] { return reply_ != Reply::None; });
        reply = reply_;
        pending_ = false;
    }
    switch (reply) {
        case Reply::Always: db_.SetPermission(ToString(scope), target, true); return true;
        case Reply::Never: db_.SetPermission(ToString(scope), target, false); return false;
        case Reply::Yes: return true;
        default: return false;  // non, silence ou annulation
    }
}

bool PermissionGate::OnUserReply(std::string_view transcript) {
    std::scoped_lock lock(mutex_);
    if (!pending_) return false;
    const Reply r = Parse(transcript);
    if (r == Reply::None) return false;  // pas une réponse : traité comme une nouvelle demande
    reply_ = r;
    cv_.notify_all();
    return true;
}

bool PermissionGate::Pending() const {
    std::scoped_lock lock(mutex_);
    return pending_;
}

}  // namespace companion
