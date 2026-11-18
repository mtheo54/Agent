#pragma once
// WatchAlong : « regarde avec moi ». TOI tu scrolles (tes Reels, ton TikTok, YouTube), avec TON compte, normalement ;
// Omnis écoute seulement le son de ton navigateur (capture du processus, jamais le reste du PC ni ton micro) et lit le
// titre de l'onglet (sur TikTok, la légende de la vidéo). Il en tire des vannes, et réagit parfois comme un pote.
//
// Rien n'est piloté : aucun clic, aucune requête au nom de ton compte. Il n'écoute plus dès que l'onglet passe sur des
// messages (DM, boîte de réception, discussions) : un vocal privé ne doit pas être transcrit.

#include "audio/CallVad.hpp"
#include "audio/WasapiLoopback.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace companion {

// Titre d'onglet « privé » (messages) : on n'écoute pas. Logique pure.
bool PrivateTabTitle(const std::string& lowerTitle);

class WatchAlong {
public:
    using Segment = std::function<void(std::vector<float> pcm16k, std::string title)>;

    ~WatchAlong() { Stop(); }
    // Navigateur au premier plan (ou, à défaut, le premier lancé) : Chrome, Edge, Firefox, Brave, Opera, Vivaldi.
    bool Start(Segment onSegment, std::string& error);
    void Stop();
    [[nodiscard]] bool Running() const noexcept { return capture_.Running(); }
    [[nodiscard]] std::string Title() const;  // titre de l'onglet en ce moment
    [[nodiscard]] const std::wstring& Browser() const noexcept { return exe_; }
    [[nodiscard]] std::chrono::steady_clock::time_point StartedAt() const noexcept { return started_; }

private:
    void OnAudio(const float* interleaved, std::size_t frames);

    WasapiLoopback capture_;
    Segment onSegment_;
    std::wstring exe_;
    HWND window_ = nullptr;
    CallVad vad_;
    std::vector<float> down_, segment_;
    float acc_ = 0.f;
    int accN_ = 0;
    std::chrono::steady_clock::time_point started_{};
};

}  // namespace companion
