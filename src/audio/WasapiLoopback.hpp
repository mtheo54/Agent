#pragma once
// WasapiLoopback : écoute directe du son du PC, sans plugin VST sur le Master (rapport §4.C).
//
//  - Mode processus (Windows 11) : capture UNIQUEMENT l'arbre de processus de FL64.exe
//    (ActivateAudioInterfaceAsync + PROCESS_LOOPBACK). La voix du compagnon, Discord ou le
//    navigateur ne polluent pas l'analyse.
//  - Mode système (repli) : loopback du périphérique de sortie par défaut (tout le mix du PC).
//  - Mode entrée : une ENTRÉE de la carte son choisie par son nom (ex. le canal « Loopback » d'une Antelope).
//    Indispensable quand FL Studio joue en ASIO natif (Antelope, Focusrite…) : l'ASIO contourne le moteur audio
//    de Windows, la capture du processus ne reçoit alors que du silence.
//
// Format livré : float stéréo entrelacé 48 kHz (conversion par le moteur audio Windows).
// Le thread de capture ne fait que copier : l'analyse tourne ailleurs, en priorité basse.

#include <atomic>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace companion {

class WasapiLoopback {
public:
    static constexpr std::uint32_t kSampleRate = 48000;
    static constexpr std::uint32_t kChannels = 2;
    // Appelé sur le thread de capture avec des trames stéréo entrelacées. Doit être rapide.
    using Sink = std::function<void(const float* interleaved, std::size_t frames)>;

    ~WasapiLoopback() { Stop(); }

    // processName vide = mix système. Retourne false si le processus est introuvable
    // (l'appelant peut alors retenter en mode système).
    bool Start(const std::wstring& processName, Sink sink);
    // Entrée dont le nom contient `nameContains` (sans casse). Faux si aucune ne correspond.
    bool StartInput(const std::wstring& nameContains, Sink sink);
    // Noms des entrées audio (pour le journal et les réglages).
    static std::vector<std::wstring> InputNames();
    // Sorties (haut-parleurs, carte son, casque) sous la forme « [sortie] nom » : StartInput les écoute en loopback.
    static constexpr const wchar_t* kOutputPrefix = L"[sortie] ";
    static std::vector<std::wstring> OutputNames();
    void Stop();
    [[nodiscard]] bool Running() const noexcept { return running_.load(); }
    [[nodiscard]] bool IsProcessCapture() const noexcept { return processMode_; }
    [[nodiscard]] bool IsInputCapture() const noexcept { return !inputId_.empty(); }

    static std::uint32_t FindProcessId(const std::wstring& exeName);

private:
    void Run(std::stop_token st, std::uint32_t pid, std::wstring inputId, Sink sink, bool outputDevice = false);

    std::jthread thread_;
    std::atomic<bool> running_{false};
    bool processMode_ = false;
    std::wstring inputId_;
};

}  // namespace companion
