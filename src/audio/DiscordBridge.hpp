#pragma once
// DiscordBridge : Omnis dans tes calls Discord, PAR TON MICRO (aucun compte piloté, aucun bot, aucune API).
//
//   Ton micro ──┐
//               ├─ mélange ─> « CABLE Input » (VB-Cable) ─> Discord : périphérique d'entrée « CABLE Output »
//   Sa voix ────┘   (seulement quand il est « dans le call »)
//
//   Discord.exe ─> capture du son de CE processus (loopback par processus) ─> détection de parole ─> Whisper
//                 (ce que disent tes potes ; ta propre voix n'y est pas, Discord ne te renvoie pas ton micro)
//
// Le relais du micro tourne dès que Discord et le câble sont là (sinon, Discord réglé sur le câble ne
// t'entendrait plus) : ~0,1 % d'un cœur, 10 ms de latence ajoutée. Sa voix n'y passe que s'il a rejoint le call.

#include "audio/CallVad.hpp"
#include "audio/VoiceEngine.hpp"  // VoiceTap
#include "audio/WasapiLoopback.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

struct DiscordBridgeConfig {
    bool enabled = true;
    std::wstring cableDevice = L"CABLE Input";  // sortie audio du câble virtuel (nom, sans la casse)
    std::wstring micDevice;                     // vide = micro par défaut de Windows
    std::wstring discordExe = L"Discord.exe";
    float voiceGain = 0.9f;                     // sa voix dans le call (ton micro reste à 1)
};

class DiscordBridge final : public VoiceTap {
public:
    using CallSpeech = std::function<void(std::vector<float> pcm16k)>;

    DiscordBridge() = default;
    ~DiscordBridge() override { Stop(); }
    DiscordBridge(const DiscordBridge&) = delete;
    DiscordBridge& operator=(const DiscordBridge&) = delete;

    void Start(DiscordBridgeConfig cfg, CallSpeech onSpeech);  // surveillance toutes les 3 s
    void Stop();

    [[nodiscard]] bool CableFound() const noexcept { return cableFound_.load(); }
    [[nodiscard]] bool DiscordRunning() const noexcept { return discordRunning_.load(); }
    [[nodiscard]] bool Relaying() const noexcept { return relaying_.load(); }
    [[nodiscard]] bool InCall() const noexcept { return inCall_.load(); }
    [[nodiscard]] std::string Problem() const {
        std::scoped_lock lock(mutex_);
        return problem_;
    }

    bool Join();   // sa voix part dans le call, il écoute les autres
    void Leave();

    // Volume de sa voix dans le call (1 = normal ; monte s'il n'est pas entendu, plafonné à 2,2 soit +7 dB).
    void SetVoiceGain(float g) noexcept { gain_.store(std::clamp(g, 0.3f, 2.2f)); }
    [[nodiscard]] float VoiceGain() const noexcept { return gain_.load(); }
    // « Earrape » de rage bait : saturation et basses écrasées sur sa voix / ses bruitages pendant au plus 1,5 s.
    // Plafonné (crête -8 dBFS, ~ -10 dBFS efficaces) : très désagréable, sans danger pour les oreilles au casque.
    void Earrape(int ms) noexcept;
    // false = il parle en privé (toi seul l'entends, ex. le résumé quand tu reviens), le call n'entend rien.
    void SetRouteVoice(bool on) noexcept { route_.store(on); }
    // Quelqu'un parle en ce moment dans le call ?
    [[nodiscard]] bool OthersTalking() const noexcept { return othersTalking_.load(); }

    void OnVoiceOut(const float* mono, std::size_t frames, int sampleRate) noexcept override;

private:
    void SuperviseMain(std::stop_token st);
    void RelayMain(std::stop_token st, std::wstring cableId);
    void OnDiscordAudio(const float* interleaved, std::size_t frames);

    DiscordBridgeConfig cfg_;
    CallSpeech onSpeech_;
    std::jthread supervisor_, relay_;
    std::atomic<bool> cableFound_{false}, discordRunning_{false}, relaying_{false}, inCall_{false}, route_{true}, othersTalking_{false};
    std::atomic<float> gain_{0.9f};
    std::atomic<unsigned long long> earrapeUntil_{0};
    float earLow_ = 0.f;  // filtre des basses de l'earrape (thread du relais)
    std::string problem_;       // protégé par mutex_
    mutable std::mutex mutex_;  // problem_, Join / Leave
    WasapiLoopback discordAudio_;

    // Sa voix -> relais (un seul producteur : le rendu audio ; un seul consommateur : le relais), 48 kHz.
    static constexpr std::size_t kVoiceRing = 1u << 16;  // 1,36 s
    std::vector<float> voice_ = std::vector<float>(kVoiceRing);
    std::atomic<std::size_t> voiceW_{0}, voiceR_{0};
    double upPhase_ = 0.0;
    float upLast_ = 0.f;

    // Audio du call -> 16 kHz -> VAD (thread de capture Discord)
    CallVad vad_;
    std::vector<float> down_, segment_;
    float decimAcc_ = 0.f;
    int decimN_ = 0;
};

}  // namespace companion
