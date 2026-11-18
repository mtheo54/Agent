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
#include <cstdint>
#include <deque>
#include <thread>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace companion {

// Titre d'onglet « privé » (messages) : on n'écoute pas. Logique pure.
bool PrivateTabTitle(const std::string& lowerTitle);

// Une image de la vidéo (RGB, 448 px au plus de côté), gardée en mémoire seulement.
struct WatchFrame {
    std::chrono::steady_clock::time_point at;
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;
};

// Deux images côte à côte (même hauteur) : début et fin de la vidéo en une seule image pour le modèle de vision.
WatchFrame SideBySide(const WatchFrame& a, const WatchFrame& b);

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
    // Images depuis `since` (20 dernières secondes au plus) et instant du dernier « swipe » (l'image change d'un coup).
    [[nodiscard]] std::vector<WatchFrame> FramesSince(std::chrono::steady_clock::time_point since) const;
    [[nodiscard]] std::vector<WatchFrame> FramesBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) const;
    [[nodiscard]] std::chrono::steady_clock::time_point LastCut() const;

private:
    void OnAudio(const float* interleaved, std::size_t frames);
    void FrameMain(std::stop_token st);
    HWND CurrentWindow() const;

    WasapiLoopback capture_;
    Segment onSegment_;
    std::wstring exe_;
    HWND window_ = nullptr;
    CallVad vad_;
    std::vector<float> down_, segment_;
    float acc_ = 0.f;
    int accN_ = 0;
    std::chrono::steady_clock::time_point started_{};
    // Images
    std::jthread frameThread_;
    mutable std::mutex frameMutex_;
    std::deque<WatchFrame> frames_;
    std::vector<std::uint8_t> prevThumb_;
    std::chrono::steady_clock::time_point lastCut_{};
};

}  // namespace companion
