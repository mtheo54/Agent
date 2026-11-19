#pragma once
// VoiceEngine : pipeline vocal 100 % CPU (0 Mo de VRAM).
//
//   Micro ─WASAPI capture (16 kHz mono, auto-conversion)─► [Push-to-Talk | VAD] ─► whisper.cpp ─► texte
//   Texte ─► phonémisation espeak-ng ─► Piper (ONNX Runtime CPU) ─► file SPSC
//         ─► thread de rendu WASAPI (MMCSS "Audio") ─► VoiceFX 8-bit ─► haut-parleurs
//                                                         └─► RMS atomique ─► avatar 3D
//
// Le RMS est mesuré au moment du rendu : l'animation est synchronisée avec ce que l'on entend.

#include "audio/VoiceEmotion.hpp"
#include "audio/VoiceFX.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>
#include "audio/NeuralVoice.hpp"
#include "audio/SpeakingStyle.hpp"

#include <memory>
#include <string>

namespace companion {

struct VoiceEngineConfig {
    std::filesystem::path whisperModel = L"assets/models/ggml-base-q5_1.bin";
    std::filesystem::path piperModel = L"assets/models/fr_FR-siwis-medium.onnx";  // + .onnx.json
    std::filesystem::path espeakDataDir = L"espeak-ng-data";
    std::string language = "fr";
    int sttThreads = 4;        // hors jeu
    int sttThreadsGaming = 2;  // en jeu : on laisse 4 cœurs au jeu
    int ttsThreads = 2;
    // Voix neuronale (Qwen3-TTS, GPU) ; Piper = repli (modèles absents, ou en jeu pour ne pas toucher au GPU).
    NeuralVoiceConfig neural{};
    bool neuralEnabled = true;
    bool neuralInGame = false;  // la voix neuronale utilise le GPU : désactivée en jeu par défaut (0 % de FPS)
    VoiceFXConfig fx{};
};

// Activation de l'écoute (rapport §4.B : "Push-to-Talk ou détection de voix (VAD)").
enum class ListenMode : std::uint8_t {
    PushToTalk,     // BeginListening()/EndListening()
    VoiceActivity,  // micro ouvert en continu, segments de parole détectés automatiquement
};

// Robinet sur la voix telle qu'on l'entend (après effets) : le pont Discord la mêle à ton micro.
// Appelé sur le thread de rendu audio (temps réel) : ne doit ni bloquer ni allouer.
class VoiceTap {
public:
    virtual ~VoiceTap() = default;
    virtual void OnVoiceOut(const float* mono, std::size_t frames, int sampleRate) noexcept = 0;
};

class VoiceEngine {
public:
    // Texte reconnu + émotion perçue dans la voix (ton, intonation, énergie, débit).
    using TranscriptCallback = std::function<void(std::string text, EmotionEstimate emotion)>;

    VoiceEngine();
    ~VoiceEngine();
    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    // Le rendu audio démarre toujours ; STT/TTS sont optionnels (modèles absents = désactivés).
    bool Initialize(const VoiceEngineConfig& cfg);
    void Shutdown();

    // --- STT ---
    void SetTranscriptCallback(TranscriptCallback cb);  // appelé sur le thread worker
    void SetListenMode(ListenMode mode);
    [[nodiscard]] ListenMode GetListenMode() const noexcept;
    void BeginListening();  // Push-to-Talk uniquement
    void EndListening();    // déclenche la transcription asynchrone
    // Vrai pendant un push-to-talk, ou pendant un segment de parole détecté par la VAD.
    [[nodiscard]] bool IsListening() const noexcept;

    // --- TTS ---
    void Speak(std::string text, SpeakingStyle style = SpeakingStyle::Normal);  // asynchrone, mis en file

    // --- Parole humaine ---
    // Relances courtes (« Hmm… », « Attends… ») synthétisées à l'avance par catégorie, puis jouées
    // instantanément pendant que le modèle prépare sa réponse. Catégories : think, ack, empathy, excited.
    void PrepareBackchannels(std::map<std::string, std::vector<std::string>> phrases);
    bool PlayBackchannel(const std::string& category);  // false si rien de prêt ou s'il parle déjà
    // Interruption : en écoute continue, si l'utilisateur parle par-dessus, il se tait.
    void SetBargeIn(bool enabled, std::function<void()> onBargeIn = {});
    void SetEmotionDetection(bool enabled) noexcept;
    void SetBaseSpeechRate(float lengthScale) noexcept;  // 0,85 rapide … 1,2 lent
    // Débit de base (1 = celui du modèle, 1,18 par défaut : blancs resserrés et parole plus vive, même hauteur).
    void SetVoiceTempo(float tempo) noexcept;
    [[nodiscard]] std::string VoiceBaseline() const;     // voix habituelle apprise (à persister)
    void LoadVoiceBaseline(const std::string& s);
    void StopSpeaking();           // vide la file et coupe la voix immédiatement
    [[nodiscard]] bool IsSpeaking() const noexcept;

    // --- Expression ---
    void SetMood(float mood01) noexcept;           // grain 8-bit de base (les styles le modulent)
    [[nodiscard]] float VoiceRms() const noexcept;  // pour CartoonAvatar (thread-safe)
    void SetGamingMode(bool gaming) noexcept;       // réduit les threads STT, voix Piper (sauf neuralInGame)
    void SetNeuralVoice(bool enabled, bool inGame) noexcept;
    // Tics humains et bugs de robot (rires spontanés, toux, syllabe qui accroche) : 0 jamais … 1 souvent.
    void SetQuirks(float quirks01) noexcept;
    // Exagération des émotions dans la voix (vitesse, hauteur, volume) : 0 … 1,5.
    void SetEmotionIntensity(float intensity) noexcept;
    void SetSystemLoad(float load01) noexcept;
    // Soundboard : la balise (son: nom) et PlaySound() demandent le son au résolveur (PCM 24 kHz mono).
    using SoundResolver = std::function<std::shared_ptr<const std::vector<float>>(const std::string&)>;
    void SetSoundResolver(SoundResolver resolver);
    // Après un téléchargement de modèles : charge ce qui manquait (Whisper, Piper, voix neuronale).
    void LoadMissing(bool neuralWanted);
    void PlaySound(const std::string& name);
    void SetMicEffect(float amount01) noexcept;  // « vrai micro » : proximité, présence, compresseur, pièce
    void SetPitchLife(float amount01) noexcept;  // hauteur qui dérive et vire un peu (0 = voix droite)  // charge CPU 0 … 1 (plus de bugs quand le PC rame)
    [[nodiscard]] bool HasNeuralVoice() const noexcept;  // voix neuronale chargée

    // Pont Discord : sortie de la voix (nullptr = aucun) et transcription de l'audio des autres (16 kHz mono).
    void SetVoiceTap(VoiceTap* tap) noexcept;
    // Sous-titres : la phrase qu'il est en train de dire (calée sur le son réellement joué), et depuis quand il s'est tu.
    struct Caption {
        std::string text;            // vide : rien dit récemment
        bool speaking = false;
        float silentSeconds = 0.f;   // depuis la fin du son (pour estomper)
    };
    [[nodiscard]] Caption CurrentCaption() const;
    // Prendre la voix de l'utilisateur : chaque phrase dite au micro est livrée brute (16 kHz) avec sa transcription,
    // au lieu d'être traitée comme une demande, jusqu'à CaptureNextUtterance(nullptr).
    using RawUtterance = std::function<void(std::vector<float> pcm16k, std::string text)>;
    void CaptureNextUtterance(RawUtterance cb);
    // Change la voix neuronale (dossier de prises <style>.wav/.txt en 24 kHz), rechargée sur le thread de synthèse.
    void SetNeuralVoiceDir(std::filesystem::path dir);
    void TranscribeExternal(std::vector<float> pcm16k, std::function<void(std::string)> done);

    [[nodiscard]] bool HasStt() const noexcept;
    [[nodiscard]] bool HasTts() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace companion
