#include "audio/VoiceEngine.hpp"
#include "audio/TempoStretch.hpp"

#include "audio/Expressivity.hpp"
#include "audio/VoiceColor.hpp"
#include "audio/TextNormalizerFr.hpp"
#include "core/Log.hpp"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#if COMPANION_WITH_WHISPER
#include <whisper.h>
#endif
#if COMPANION_WITH_ONNX
#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>
#endif
#if COMPANION_WITH_ESPEAK
#include <espeak-ng/speak_lib.h>
#ifndef espeakPHONEMES_IPA
#define espeakPHONEMES_IPA 0x02
#endif
#endif

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr REFERENCE_TIME kBufferDuration = 400'000;  // 40 ms (unités de 100 ns)
constexpr UINT32 kSttSampleRate = 16000;
constexpr size_t kMaxRecordingSamples = kSttSampleRate * 30;  // fenêtre Whisper

// ---------------------------------------------------------------------------
// VAD énergétique à plancher de bruit adaptatif (trames de 20 ms). Coût : quelques
// opérations par échantillon, aucune dépendance. Whisper ne tourne que sur la parole détectée.
// ---------------------------------------------------------------------------
class EnergyVad {
public:
    static constexpr size_t kFrame = kSttSampleRate / 50;  // 20 ms
    static constexpr int kStartFrames = 3;                  // 60 ms au-dessus du seuil
    static constexpr int kHangoverFrames = 35;              // 700 ms de silence = fin de phrase
    static constexpr size_t kPreRoll = kFrame * 15;         // 300 ms conservés avant le déclenchement

    // Renvoie un segment complet lorsqu'une phrase se termine.
    std::optional<std::vector<float>> Feed(std::span<const float> in) {
        std::optional<std::vector<float>> done;
        for (float v : in) {
            frame_.push_back(v);
            if (frame_.size() == kFrame) {
                if (auto seg = ProcessFrame()) done = std::move(seg);
                frame_.clear();
            }
        }
        return done;
    }
    void Reset() noexcept {
        inSpeech_ = false;
        above_ = below_ = 0;
        preRoll_.clear();
        segment_.clear();
        frame_.clear();
    }
    [[nodiscard]] bool InSpeech() const noexcept { return inSpeech_; }

private:
    std::optional<std::vector<float>> ProcessFrame() {
        double sum = 0.0;
        for (float v : frame_) sum += static_cast<double>(v) * v;
        const float rms = static_cast<float>(std::sqrt(sum / kFrame));
        const float threshold = std::max(noiseFloor_ * 3.5f, 0.008f);

        if (!inSpeech_) {
            // Le plancher descend vite et monte lentement : il suit le bruit de fond, pas la voix.
            noiseFloor_ += (rms - noiseFloor_) * (rms < noiseFloor_ ? 0.1f : 0.002f);
            preRoll_.insert(preRoll_.end(), frame_.begin(), frame_.end());
            if (preRoll_.size() > kPreRoll) preRoll_.erase(preRoll_.begin(), preRoll_.end() - kPreRoll);
            above_ = rms > threshold ? above_ + 1 : 0;
            if (above_ >= kStartFrames) {
                inSpeech_ = true;
                below_ = 0;
                segment_ = std::move(preRoll_);
                preRoll_.clear();
            }
            return std::nullopt;
        }
        segment_.insert(segment_.end(), frame_.begin(), frame_.end());
        below_ = rms < threshold ? below_ + 1 : 0;
        if (below_ >= kHangoverFrames || segment_.size() >= kMaxRecordingSamples) {
            std::vector<float> out = std::move(segment_);
            Reset();
            return out;
        }
        return std::nullopt;
    }

    float noiseFloor_ = 0.005f;
    bool inSpeech_ = false;
    int above_ = 0, below_ = 0;
    std::vector<float> frame_, preRoll_, segment_;
};

// ---------------------------------------------------------------------------
// File SPSC sans verrou (producteur : worker TTS, consommateur : thread de rendu)
// ---------------------------------------------------------------------------
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    size_t Push(std::span<const float> in) noexcept {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t n = std::min(in.size(), buf_.size() - (w - r));
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = in[i];
        write_.store(w + n, std::memory_order_release);
        return n;
    }
    size_t Pop(float* out, size_t max) noexcept {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t n = std::min(max, w - r);
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(r + i) & mask_];
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    // Consommateur uniquement : jette tout ce qui a été écrit avant `writeMark`
    // (l'audio poussé après un StopSpeaking() est conservé).
    void DiscardUntil(size_t writeMark) noexcept {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (writeMark > r) read_.store(std::min(writeMark, w), std::memory_order_release);
    }
    [[nodiscard]] size_t WriteIndex() const noexcept { return write_.load(std::memory_order_acquire); }
    [[nodiscard]] size_t Size() const noexcept {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

private:
    std::vector<float> buf_;
    size_t mask_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

WAVEFORMATEX MonoFloatFormat(UINT32 rate) {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 1;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = rate * 4;
    return f;
}

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// Ouvre le périphérique par défaut en mode partagé, événementiel, format mono float à `rate`.
// AUTOCONVERTPCM : le moteur audio Windows rééchantillonne/remixe vers le format du périphérique.
bool OpenSharedClient(EDataFlow flow, UINT32 rate, HANDLE event, ComPtr<IAudioClient>& client) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return false;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) return false;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()))))
        return false;
    WAVEFORMATEX fmt = MonoFloatFormat(rate);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &fmt, nullptr)))
        return false;
    return SUCCEEDED(client->SetEventHandle(event));
}

[[maybe_unused]] std::u32string Utf8ToUtf32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Découpe en phrases : la première phrase est synthétisée et jouée pendant que la suite se calcule.
[[maybe_unused]] std::vector<std::string> SplitSentences(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        cur.push_back(c);
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
            if (cur.find_first_not_of(" \t\r\n.!?") != std::string::npos) out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// TTS Piper (VITS) via ONNX Runtime CPU + phonémisation espeak-ng
// ---------------------------------------------------------------------------
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
class PiperTts {
public:
    bool Load(const std::filesystem::path& model, const std::filesystem::path& espeakData, int threads) {
        std::filesystem::path jsonPath = model;
        jsonPath += L".json";
        std::ifstream jf(jsonPath);
        if (!jf) { log::Info("Piper: {}.json introuvable", log::Narrow(model.wstring())); return false; }
        try {
            const auto j = nlohmann::json::parse(jf);
            sampleRate_ = j.at("audio").at("sample_rate").get<int>();
            const auto& inf = j.value("inference", nlohmann::json::object());
            noiseScale_ = inf.value("noise_scale", 0.667f);
            lengthScale_ = inf.value("length_scale", 1.0f);
            noiseW_ = inf.value("noise_w", 0.8f);
            voice_ = j.value("espeak", nlohmann::json::object()).value("voice", std::string{"fr"});
            for (const auto& [key, ids] : j.at("phoneme_id_map").items()) {
                const auto cps = Utf8ToUtf32(key);
                if (cps.size() == 1) idMap_[cps[0]] = ids.get<std::vector<int64_t>>();
            }
        } catch (const std::exception& e) {
            log::Info("Piper: JSON invalide ({})", e.what());
            return false;
        }

        // espeak-ng ouvre ses fichiers avec fopen (page de code ANSI), pas en UTF-8 : un accent dans le chemin
        // (C:\Users\Théo\...) le rendrait muet. Chemin court 8.3 (ASCII) ; sinon nullptr = espeak cherche
        // espeak-ng-data à côté de l'exe, lui-même en ANSI (cohérent).
        std::string dataPath;
        {
            wchar_t shortPath[MAX_PATH];
            const DWORD n = GetShortPathNameW(espeakData.c_str(), shortPath, MAX_PATH);
            const std::wstring w = n && n < MAX_PATH ? std::wstring(shortPath, n) : espeakData.wstring();
            if (std::ranges::all_of(w, [](wchar_t c) { return c < 128; })) dataPath = log::Narrow(w);
        }
        if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0, dataPath.empty() ? nullptr : dataPath.c_str(), 0) < 0 ||
            espeak_SetVoiceByName(voice_.c_str()) != EE_OK) {
            log::Info("espeak-ng: initialisation impossible (données: {})", dataPath);
            return false;
        }

        try {
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(threads);
            so.SetInterOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            so.DisableMemPattern();  // longueurs variables : évite de garder des pools surdimensionnés
            session_ = std::make_unique<Ort::Session>(env_, model.c_str(), so);
            hasSid_ = session_->GetInputCount() > 3;
        } catch (const Ort::Exception& e) {
            log::Info("ONNX Runtime: {}", e.what());
            return false;
        }
        return true;
    }

    [[nodiscard]] int SampleRate() const noexcept { return sampleRate_; }

    bool Synthesize(std::string_view sentence, std::vector<float>& out, float lengthMul = 1.f, float noiseMul = 1.f) {
        std::vector<int64_t> ids = PhonemeIds(Phonemize(sentence));
        if (ids.size() <= 3) return false;

        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 2> idShape{1, static_cast<int64_t>(ids.size())};
        std::array<int64_t, 1> lengths{static_cast<int64_t>(ids.size())};
        const std::array<int64_t, 1> oneShape{1};
        std::array<float, 3> scales{noiseScale_ * noiseMul, lengthScale_ * lengthMul, noiseW_};
        const std::array<int64_t, 1> scaleShape{3};
        std::array<int64_t, 1> sid{0};

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), idShape.data(), 2));
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, lengths.data(), 1, oneShape.data(), 1));
        inputs.push_back(Ort::Value::CreateTensor<float>(mem, scales.data(), 3, scaleShape.data(), 1));
        if (hasSid_) inputs.push_back(Ort::Value::CreateTensor<int64_t>(mem, sid.data(), 1, oneShape.data(), 1));

        const char* inNames[] = {"input", "input_lengths", "scales", "sid"};
        const char* outNames[] = {"output"};
        try {
            auto outs = session_->Run(Ort::RunOptions{nullptr}, inNames, inputs.data(), inputs.size(), outNames, 1);
            const float* data = outs[0].GetTensorData<float>();
            const size_t n = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            float peak = 0.01f;
            for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(data[i]));
            const float gain = 0.8f / peak;  // normalisation crête (comme Piper)
            out.resize(n);
            for (size_t i = 0; i < n; ++i) out[i] = data[i] * gain;
        } catch (const Ort::Exception& e) {
            log::Info("Piper: inférence échouée ({})", e.what());
            return false;
        }
        return true;
    }

private:
    std::string Phonemize(std::string_view text) {
        // NB : espeak-ng "stock" ne renvoie pas la ponctuation de fin de proposition
        // (Piper utilise un fork patché). On réinjecte une virgule entre propositions.
        std::string s(text), result;
        const void* ptr = s.c_str();
        while (ptr != nullptr) {
            const char* ph = espeak_TextToPhonemes(&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);
            if (!ph) break;
            if (!result.empty()) result += ", ";
            result += ph;
        }
        return result;
    }

    std::vector<int64_t> PhonemeIds(std::string_view ipa) const {
        std::vector<int64_t> ids;
        auto append = [&](char32_t cp) -> bool {
            const auto it = idMap_.find(cp);
            if (it == idMap_.end()) return false;
            ids.insert(ids.end(), it->second.begin(), it->second.end());
            return true;
        };
        auto pad = [&] { append(U'_'); };
        append(U'^');
        pad();
        for (char32_t cp : Utf8ToUtf32(ipa))
            if (append(cp)) pad();
        append(U'$');
        return ids;
    }

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "CompanionTTS"};
    std::unique_ptr<Ort::Session> session_;
    std::unordered_map<char32_t, std::vector<int64_t>> idMap_;
    std::string voice_ = "fr";
    int sampleRate_ = 22050;
    float noiseScale_ = 0.667f, lengthScale_ = 1.f, noiseW_ = 0.8f;
    bool hasSid_ = false;
};
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct VoiceEngine::Impl {
    VoiceEngineConfig cfg;
    UINT32 outputRate = 24000;

    RetroVoiceProcessor fx;
    SpscRing ring{1u << 20};  // ~43 s à 24 kHz
    std::atomic<float> rms{0.f};
    std::atomic<VoiceTap*> tap{nullptr};
    static constexpr size_t kNoFlush = SIZE_MAX;
    std::atomic<size_t> flushMark{kNoFlush};
    std::atomic<bool> rendering{false};
    std::atomic<bool> gaming{false};

    // Voix neuronale (chargée sur le thread de synthèse, en premier travail)
    NeuralVoice neural;
    std::atomic<bool> neuralEnabled{true};
    std::atomic<bool> neuralInGame{false};
    // Expressivité : sons humains, spontanéité, bugs de robot (thread de synthèse uniquement)
    VocalBank vocals;
    RobotGlitcher glitcher;
    EmotionShaper shaper;
    TempoStretch tempo;  // débit : blancs resserrés + plus rapide sans monter dans les aigus
    std::atomic<float> voiceTempo{1.18f};
    VoiceColor color;                  // micro + bizarreries (thread de synthèse)
    std::function<std::shared_ptr<const std::vector<float>>(const std::string&)> soundResolver;
    std::atomic<float> micEffect{0.7f};
    std::atomic<float> pitchLife{0.5f};
    float appliedMic = -1.f;
    std::atomic<float> emotionIntensity{1.f};
    std::atomic<float> systemLoad{0.f};  // charge CPU (ResourceGovernor) : un PC qui rame fait bugger le robot
    std::vector<float> chunkBuf;         // morceau de voix en cours de traitement (capacité conservée)
    std::atomic<float> quirks{0.5f};
    std::uint32_t vrng = 0x5EEDu;
    std::chrono::steady_clock::time_point lastGlitch{}, lastCough{}, lastLead{};

    bool UseNeural() const noexcept {
        return neuralEnabled.load() && neural.Ready() && (!gaming.load() || neuralInGame.load());
    }
    std::atomic<uint32_t> speechGeneration{0};
    std::atomic<int> pendingSpeech{0};

    // Rendu
    std::jthread renderThread;
    HANDLE renderWake = nullptr;  // réveille le thread de rendu quand des données arrivent

    // Capture
    std::jthread captureThread;
    std::mutex captureMutex;
    std::vector<float> captured;
    std::atomic<bool> listening{false};
    std::atomic<ListenMode> listenMode{ListenMode::PushToTalk};
    std::jthread vadThread;
    std::atomic<bool> vadInSpeech{false};

    // Worker (STT + TTS), un seul thread : sérialise l'usage CPU.
    std::jthread worker;
    std::mutex jobMutex;
    std::condition_variable_any jobCv;
    std::deque<std::function<void()>> jobs;

    std::mutex cbMutex;
    TranscriptCallback onTranscript;
    VoiceEngine::RawUtterance rawCapture;  // une fois (cbMutex)

    // Émotion dans la voix
    std::mutex emotionMutex;
    VoiceEmotionAnalyzer emotion;
    std::atomic<bool> emotionEnabled{true};

    // Diction
    std::atomic<float> baseMood{0.5f};
    std::atomic<float> baseLength{1.f};

    // Relances pré-synthétisées
    std::mutex backchannelMutex;
    std::map<std::string, std::vector<std::vector<float>>> backchannels;
    std::map<std::string, size_t> lastBackchannel;
    unsigned rng = 0x9E3779B9u;

    // Interruption
    std::atomic<bool> bargeIn{true};
    std::function<void()> onBargeIn;

    static void StyleParams(SpeakingStyle st, float& lengthMul, float& noiseMul, float& moodDelta) {
        switch (st) {
            case SpeakingStyle::Bright: lengthMul = 0.93f; noiseMul = 1.2f; moodDelta = 0.2f; break;
            case SpeakingStyle::Soft: lengthMul = 1.12f; noiseMul = 0.75f; moodDelta = -0.25f; break;
            case SpeakingStyle::Calm: lengthMul = 1.06f; noiseMul = 0.85f; moodDelta = -0.15f; break;
            case SpeakingStyle::Amused: lengthMul = 0.95f; noiseMul = 1.25f; moodDelta = 0.15f; break;
            case SpeakingStyle::Surprised: lengthMul = 0.9f; noiseMul = 1.3f; moodDelta = 0.25f; break;
            case SpeakingStyle::Annoyed: lengthMul = 0.94f; noiseMul = 1.15f; moodDelta = 0.2f; break;
            default: lengthMul = 1.f; noiseMul = 1.f; moodDelta = 0.f; break;
        }
    }

    void StopSpeakingNow() {
        ++speechGeneration;
        flushMark = ring.WriteIndex();
        if (renderWake) SetEvent(renderWake);
    }

#if COMPANION_WITH_WHISPER
    whisper_context* whisper = nullptr;
#endif
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
    std::unique_ptr<PiperTts> tts;
#endif

    void Post(std::function<void()> job) {
        {
            std::scoped_lock lock(jobMutex);
            jobs.push_back(std::move(job));
        }
        jobCv.notify_one();
    }

    void WorkerMain(std::stop_token st) {
        log::SetCurrentThreadName(L"Companion.VoiceWorker");
        // BELOW_NORMAL : à pleine vitesse si le CPU est libre, s'efface devant le jeu sinon.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(jobMutex);
                if (!jobCv.wait(lock, st, [&] { return !jobs.empty(); })) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }

    // --- Rendu WASAPI ---------------------------------------------------------
    void RenderMain(std::stop_token st) {
        log::SetCurrentThreadName(L"Companion.AudioOut");
        ComScope com;
        HANDLE bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        UINT32 bufferFrames = 0;
        if (!OpenSharedClient(eRender, outputRate, bufferEvent, client) ||
            FAILED(client->GetBufferSize(&bufferFrames)) || FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
            log::Info("VoiceEngine: impossible d'ouvrir la sortie audio WASAPI");
            CloseHandle(bufferEvent);
            return;
        }
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

        bool started = false;
        ULONGLONG lastAudioTick = 0;
        while (!st.stop_requested()) {
            if (!started) {
                // Client arrêté : zéro réveil périodique tant que personne ne parle (< 1 % CPU).
                WaitForSingleObject(renderWake, INFINITE);
                if (st.stop_requested()) break;
                if (const size_t mark = flushMark.exchange(kNoFlush); mark != kNoFlush) ring.DiscardUntil(mark);
                if (ring.Size() == 0) continue;
                client->Start();
                started = true;
                rendering = true;
                lastAudioTick = GetTickCount64();
            }
            WaitForSingleObject(bufferEvent, 100);

            if (const size_t mark = flushMark.exchange(kNoFlush); mark != kNoFlush) ring.DiscardUntil(mark);

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;  // périphérique perdu
            const UINT32 avail = bufferFrames - padding;
            if (avail == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(avail, &data))) break;
            auto* out = reinterpret_cast<float*>(data);
            const size_t got = ring.Pop(out, avail);
            std::fill(out + got, out + avail, 0.f);
            const float blockRms = fx.Process({out, avail});  // la queue des filtres se termine proprement
            rms.store(got > 0 ? blockRms : 0.f, std::memory_order_relaxed);
            if (VoiceTap* t = tap.load(std::memory_order_acquire)) t->OnVoiceOut(out, avail, static_cast<int>(outputRate));
            render->ReleaseBuffer(avail, 0);

            const ULONGLONG now = GetTickCount64();
            if (got > 0) lastAudioTick = now;
            if (now - lastAudioTick > 400 && pendingSpeech.load() == 0) {
                client->Stop();
                client->Reset();
                started = false;
                rendering = false;
                rms = 0.f;
            }
        }
        if (started) client->Stop();
        rendering = false;
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CloseHandle(bufferEvent);
    }

    void PushAudio(std::span<const float> pcm, uint32_t generation) {
        while (!pcm.empty()) {
            if (speechGeneration.load() != generation) return;  // StopSpeaking() entre-temps
            const size_t n = ring.Push(pcm);
            pcm = pcm.subspan(n);
            SetEvent(renderWake);
            if (!pcm.empty()) Sleep(10);  // file pleine : on attend le rendu
        }
    }

    bool SpeakingNow() const noexcept { return pendingSpeech.load() > 0 || ring.Size() > 0 || rms.load() > 0.f; }

    // --- Capture micro : pendant le push-to-talk, ou en continu en mode VAD ---
    void CaptureMain(std::stop_token st, bool vadMode) {
        EnergyVad vad;
        size_t loudSamples = 0;
        ULONGLONG bargedAt = 0;
        log::SetCurrentThreadName(L"Companion.AudioIn");
        ComScope com;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        if (!OpenSharedClient(eCapture, kSttSampleRate, event, client) ||
            FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
            log::Info("VoiceEngine: impossible d'ouvrir le micro WASAPI");
            CloseHandle(event);
            return;
        }
        while (!st.stop_requested()) {
            WaitForSingleObject(event, 100);
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const auto* f = reinterpret_cast<const float*>(data);
                if (!vadMode) {
                    std::scoped_lock lock(captureMutex);
                    if (captured.size() + frames <= kMaxRecordingSamples) {
                        if (silent) captured.insert(captured.end(), frames, 0.f);
                        else captured.insert(captured.end(), f, f + frames);
                    }
                } else if (SpeakingNow() && GetTickCount64() - bargedAt > 1500) {
                    // Semi-duplex : on ne s'écoute pas parler (pas d'annulation d'écho). Exception : si la
                    // voix au micro est nettement plus forte que la fuite des haut-parleurs pendant 150 ms,
                    // c'est l'utilisateur qui lui coupe la parole -> il se tait et écoute.
                    double sum = 0;
                    for (UINT32 i = 0; !silent && i < frames; ++i) sum += static_cast<double>(f[i]) * f[i];
                    const float level = frames ? static_cast<float>(std::sqrt(sum / frames)) : 0.f;
                    loudSamples = bargeIn.load() && level > 0.05f ? loudSamples + frames : 0;
                    if (loudSamples >= kSttSampleRate * 15 / 100) {
                        loudSamples = 0;
                        bargedAt = GetTickCount64();
                        StopSpeakingNow();
                        std::function<void()> cb;
                        {
                            std::scoped_lock lock(cbMutex);
                            cb = onBargeIn;
                        }
                        if (cb) cb();
                        log::Info("VoiceEngine: interrompu par l'utilisateur");
                    } else {
                        vad.Reset();
                    }
                } else {
                    std::vector<float> zeros;
                    if (silent) zeros.assign(frames, 0.f);
                    auto segment = vad.Feed(silent ? std::span<const float>(zeros) : std::span<const float>(f, frames));
                    if (segment && segment->size() > kSttSampleRate / 2)  // < 500 ms : bruit
                        Post([this, pcm = std::move(*segment)]() mutable { Transcribe(std::move(pcm)); });
                }
                vadInSpeech.store(vad.InSpeech(), std::memory_order_relaxed);
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        vadInSpeech = false;
        CloseHandle(event);
    }

    // Ce que Whisper « entend » dans de la musique ou du bruit : « [Musique] », « ♪ », « Sous-titrage… », « Fé. »…
    // Vu sur le terrain : le son de FL Studio dans le micro donnait « [Musique] », et Omnis y répondait.
    static bool NotSpeech(const std::string& raw) {
        std::string t;
        int depth = 0;
        for (char c : raw) {  // sans ce qui est entre [] ou ()
            if (c == '[' || c == '(') ++depth;
            else if (c == ']' || c == ')') depth = std::max(0, depth - 1);
            else if (!depth) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        int letters = 0;
        for (unsigned char c : t) letters += std::isalpha(c) || c == 0xC3;  // lettres, accents français (pas ♪)
        if (letters < 4) return true;  // vide, « Fé. », ponctuation, notes ♪
        for (const char* h : {"sous-titr", "sous titr", "merci d'avoir regardé", "merci de votre attention", "abonnez-vous",
                              "amara.org"})
            if (t.find(h) != std::string::npos && letters < 40) return true;
        return false;
    }

    // Texte d'un segment 16 kHz mono (vide si rien de reconnu).
    std::string Whisper(std::vector<float> pcm) {
#if COMPANION_WITH_WHISPER
        if (!whisper || pcm.size() < kSttSampleRate / 4) return {};  // < 250 ms : ignoré
        if (pcm.size() < kSttSampleRate + kSttSampleRate / 10) pcm.resize(kSttSampleRate + kSttSampleRate / 10, 0.f);

        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.language = cfg.language.c_str();
        wp.n_threads = gaming.load() ? cfg.sttThreadsGaming : cfg.sttThreads;
        wp.translate = false;
        wp.no_context = true;
        wp.single_segment = false;
        wp.print_progress = false;
        wp.print_realtime = false;
        wp.print_timestamps = false;
        wp.print_special = false;
        wp.suppress_blank = true;
        // Vocabulaire attendu : Whisper l'orthographie juste (« FL Studio » et pas « fête studio »).
        wp.initial_prompt = "Omnis, FL Studio, DAW, MAO, VST, 808, kick, snare, sidechain, mix, master, prod, beat, instru, "
                            "League of Legends, LoL, Valorant, Yasuo, Jinx, Discord, AutoCAD Electrical.";

        if (whisper_full(whisper, wp, pcm.data(), static_cast<int>(pcm.size())) != 0) {
            log::Info("whisper: échec de transcription");
            return {};
        }
        std::string text;
        const int n = whisper_full_n_segments(whisper);
        for (int i = 0; i < n; ++i) text += whisper_full_get_segment_text(whisper, i);
        const auto first = text.find_first_not_of(' ');
        if (first == std::string::npos) return {};
        text.erase(0, first);
        return text;
#else
        (void)pcm;
        return {};
#endif
    }

    void Transcribe(std::vector<float> pcm) {
#if COMPANION_WITH_WHISPER
        if (!whisper || pcm.size() < kSttSampleRate / 4) return;  // < 250 ms : ignoré
        // Émotion : analysée sur la phrase brute (avant le remplissage de silence pour Whisper).
        EmotionEstimate emo;
        if (emotionEnabled.load()) {
            log::Info("Écoute : analyse de l'émotion");
            std::scoped_lock lock(emotionMutex);
            emo = emotion.Analyze(pcm);
        }
        log::Info("Écoute : {:.1f} s de voix, transcription…", static_cast<double>(pcm.size()) / kSttSampleRate);
        VoiceEngine::RawUtterance raw;
        {
            std::scoped_lock lock(cbMutex);
            raw.swap(rawCapture);
        }
        if (raw) {  // prise de voix : l'audio brut et ce qui a été dit, rien d'autre
            std::vector<float> keep = pcm;
            std::string said = Whisper(std::move(pcm));
            raw(std::move(keep), NotSpeech(said) ? std::string{} : said);
            return;
        }
        std::string text = Whisper(std::move(pcm));
        log::Info("Écoute : « {} »", text);
        if (NotSpeech(text)) {
            if (!text.empty()) log::Info("Écoute : ignoré (pas une phrase : musique, bruit ou hallucination de Whisper)");
            return;
        }

        TranscriptCallback cb;
        {
            std::scoped_lock lock(cbMutex);
            cb = onTranscript;
        }
        if (emo.features.valid) log::Info("Émotion vocale : {} ({:+.2f}, {})", ToString(emo.label), emo.arousal, emo.description);
        if (cb) cb(std::move(text), std::move(emo));
#else
        (void)pcm;
#endif
    }

    // Piper sort en 22,05 kHz ; le rendu est à 24 kHz (débit de la voix neuronale).
    static std::vector<float> Resample(const std::vector<float>& in, int from, int to) {
        if (from == to || in.empty()) return in;
        const double step = static_cast<double>(from) / to;
        std::vector<float> out(static_cast<size_t>(in.size() / step));
        for (size_t i = 0; i < out.size(); ++i) {
            const double x = i * step;
            const size_t k = static_cast<size_t>(x);
            const float f = static_cast<float>(x - k);
            out[i] = in[k] * (1.f - f) + (k + 1 < in.size() ? in[k + 1] : in[k]) * f;
        }
        return out;
    }

    bool SpeakNeural(const std::string& text, uint32_t generation, SpeakingStyle style, std::vector<float>* capture) {
        return neural.Speak(text, style, [&](std::span<const float> pcm) {
            if (speechGeneration.load() != generation) return false;  // interrompu : on arrête le décodeur
            if (capture) capture->insert(capture->end(), pcm.begin(), pcm.end());
            else PushAudio(pcm, generation);
            return true;
        });
    }

    // Un morceau de texte : voix neuronale (streaming) ou Piper, avec un éventuel bug de robot.
    void SpeakText(const std::string& text, uint32_t generation, SpeakingStyle style, bool forceGlitch) {
        using clock = std::chrono::steady_clock;
        // Petits bugs (coupure, saut, éclat, ondulation, envers, voix robot) : à tout moment ; gros bugs
        // (syllabe qui accroche, bande qui ralentit, accéléré) : au plus un toutes les 20 s.
        const bool allowBig = forceGlitch || clock::now() - lastGlitch > std::chrono::seconds(20);
        glitcher.Plan(style, quirks.load(), vrng, forceGlitch, allowBig, systemLoad.load(std::memory_order_relaxed));
        float lengthMul, noiseMul, moodDelta;
        StyleParams(style, lengthMul, noiseMul, moodDelta);
        bool done = false;
        // Débit humain : pas une vitesse fixe. Chaque phrase a son allure (un peu plus vite ou plus posée, selon
        // l'émotion), elle accélère et ralentit en vagues lentes (2 à 3,5 s), démarre un poil plus lentement et se
        // pose sur la fin (les gens ralentissent sur les derniers mots).
        const float baseTempo = voiceTempo.load() / std::max(0.5f, baseLength.load());
        float phrase = 0.94f + 0.12f * NextRandom(vrng);
        if (style == SpeakingStyle::Bright) phrase *= 1.05f;
        if (style == SpeakingStyle::Soft || style == SpeakingStyle::Calm) phrase *= 0.93f;
        const float wavePeriod = 2.0f + 1.5f * NextRandom(vrng), wavePhase = 6.2832f * NextRandom(vrng);
        const float waveDepth = 0.07f + 0.05f * NextRandom(vrng);
        const float estSeconds = std::max(1.0f, static_cast<float>(text.size()) / 15.f);  // ~15 caractères/s au débit brut
        std::size_t heard = 0;  // échantillons bruts déjà passés (horloge du contour)
        const auto paceAt = [&](std::size_t samples) {
            const float t = static_cast<float>(samples) / static_cast<float>(outputRate);
            float k = phrase * (1.f + waveDepth * std::sin(6.2832f * t / wavePeriod + wavePhase));
            if (t < 0.35f) k *= 0.95f;                    // attaque posée
            if (t > 0.82f * estSeconds) k *= 0.88f;       // fin de phrase : on ralentit
            tempo.SetTempo(baseTempo * k);
        };
        tempo.Reset();
        paceAt(0);
        // Chaîne d'une phrase : débit -> émotion -> bugs -> micro -> sortie.
        const auto emit = [&](std::vector<float>& buf) {
            if (buf.empty()) return;
            shaper.Process(buf);
            glitcher.Process(buf);
            color.Process(buf.data(), buf.size());
            PushAudio(buf, generation);
        };
        shaper.Set(style, emotionIntensity.load());
        shaper.Reset();
        const float life = pitchLife.load();
        // Mélodie vivante : dérive ±40 cents et virages (monte / redescend) dans la plupart des phrases.
        shaper.PlanPitch(style, 80.f * life, 0.8f * life, vrng);
        SyncMic();
        color.PlanWeird(style, quirks.load(), vrng);
        if (UseNeural()) {
            const float jitter = (NextRandom(vrng) - 0.5f) * 0.12f;  // jamais deux fois la même intonation
            done = neural.Speak(text, style, [&](std::span<const float> pcm) {
                if (speechGeneration.load() != generation) return false;  // interrompu : on arrête le décodeur
                chunkBuf.assign(pcm.begin(), pcm.end());  // tampon réutilisé : aucune allocation en régime établi
                paceAt(heard);
                heard += chunkBuf.size();
                tempo.Process(chunkBuf);
                emit(chunkBuf);
                return true;
            }, jitter);
            if (done && speechGeneration.load() == generation) {
                tempo.Flush(chunkBuf);
                emit(chunkBuf);
            }
        }
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
        if (!done && tts) {
            tempo.Reset();  // la voix neuronale a pu échouer en cours de route : on ne mélange pas ses restes
            std::vector<float> pcm;
            for (const auto& sentence : SplitSentences(NormalizeForSpeechFr(text))) {
                if (speechGeneration.load() != generation) break;
                if (!tts->Synthesize(sentence, pcm, lengthMul, noiseMul)) continue;
                const auto whole = Resample(pcm, tts->SampleRate(), static_cast<int>(outputRate));
                for (std::size_t i = 0; i < whole.size(); i += 1920) {  // morceaux de 80 ms : le débit varie dedans aussi
                    std::vector<float> out(whole.begin() + static_cast<std::ptrdiff_t>(i),
                                           whole.begin() + static_cast<std::ptrdiff_t>(std::min(whole.size(), i + 1920)));
                    paceAt(heard);
                    heard += out.size();
                    tempo.Process(out);
                    glitcher.Process(out);
                    color.Process(out.data(), out.size());
                    PushAudio(out, generation);
                }
            }
            tempo.Flush(chunkBuf);
            glitcher.Process(chunkBuf);
            color.Process(chunkBuf.data(), chunkBuf.size());
            if (!chunkBuf.empty()) PushAudio(chunkBuf, generation);
        }
#endif
        if (glitcher.UsedBig()) lastGlitch = clock::now();
    }

    void SyncMic() {
        const float m = micEffect.load();
        if (m != appliedMic) {
            color.SetMic(m);
            appliedMic = m;
        }
    }

    void PlayVocal(Vocal v, uint32_t generation) {
        if (v == Vocal::Glitch) return;  // traité comme un bug sur le texte suivant
        if (const auto* clip = vocals.Pick(v, vrng)) {
            SyncMic();
            chunkBuf.assign(clip->begin(), clip->end());
            color.Process(chunkBuf.data(), chunkBuf.size(), /*weird=*/false);  // même micro que sa voix
            PushAudio(chunkBuf, generation);
            // Une toux de robot finit parfois en grésillement.
            if (v == Vocal::Cough && NextRandom(vrng) < 0.5f) PushAudio(RobotGlitcher::Burst(*clip, static_cast<int>(outputRate), vrng), generation);
        }
    }

    void Synthesize(const std::string& text, uint32_t generation, SpeakingStyle style) {
        using clock = std::chrono::steady_clock;
        float lengthMul, noiseMul, moodDelta;
        StyleParams(style, lengthMul, noiseMul, moodDelta);
        fx.SetMood(std::clamp(baseMood.load() + moodDelta, 0.f, 1.f));

        auto parts = ParseExpressiveText(text);
        const float q = quirks.load();
        const auto now = clock::now();
        // Spontanéité (sans balise) : petit rire avant une réponse drôle, « oh ! » avant une surprise…
        if (q > 0.f && !parts.empty() && !parts.front().isVocal && now - lastLead > std::chrono::seconds(20)) {
            if (const Vocal lead = Spontaneity::Lead(style, vrng); lead != Vocal::Count && vocals.Has(lead)) {
                parts.insert(parts.begin(), SpeechPart{true, lead, {}});
                lastLead = now;
            }
        }
        // … et de temps en temps, il s'éclaircit la « gorge » (ou tousse) avant de parler.
        if (!parts.empty() && !parts.front().isVocal && now - lastCough > std::chrono::minutes(3) &&
            NextRandom(vrng) < Spontaneity::CoughChance(style, q)) {
            const Vocal v = vocals.Has(Vocal::Throat) && NextRandom(vrng) < 0.7f ? Vocal::Throat : Vocal::Cough;
            if (vocals.Has(v)) {
                parts.insert(parts.begin(), SpeechPart{true, v, {}});
                lastCough = now;
            }
        }

        bool forceGlitch = false;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (speechGeneration.load() != generation) break;
            const auto& p = parts[i];
            if (p.isSound) {  // bruitage : joué tel quel (pas de micro, il vient « du PC »)
                if (soundResolver)
                    if (const auto pcm = soundResolver(p.text)) PushAudio(*pcm, generation);
                continue;
            }
            if (!p.isVocal) {
                SpeakText(p.text, generation, style, forceGlitch);
                forceGlitch = false;
            } else if (p.vocal == Vocal::Glitch) {
                // (bug) avant du texte : la phrase suivante accroche ; en fin de phrase : grésillement seul.
                if (i + 1 < parts.size() && !parts[i + 1].isVocal) forceGlitch = true;
                else if (const auto* c = vocals.Pick(Vocal::Hmm, vrng)) PushAudio(RobotGlitcher::Burst(*c, static_cast<int>(outputRate), vrng), generation);
            } else {
                PlayVocal(p.vocal, generation);
            }
        }
        --pendingSpeech;
    }

    void SynthesizeBackchannels(const std::map<std::string, std::vector<std::string>>& phrases) {
        // Voix neuronale si elle est chargée (même en jeu : ces relances sont pré-calculées ici, hors jeu).
        std::map<std::string, std::vector<std::vector<float>>> ready;
        for (const auto& [category, list] : phrases) {
            SpeakingStyle st = category == "empathy" ? SpeakingStyle::Soft
                               : category == "excited" ? SpeakingStyle::Bright : SpeakingStyle::Normal;
            float lengthMul, noiseMul, moodDelta;
            StyleParams(st, lengthMul, noiseMul, moodDelta);
            for (const auto& raw : list) {
                std::string phrase;
                for (const auto& part : ParseExpressiveText(raw)) if (!part.isVocal) phrase += part.text;
                if (phrase.empty()) continue;
                std::vector<float> pcm;
                if (neuralEnabled.load() && neural.Ready()) {
                    const uint32_t gen = speechGeneration.load();
                    if (!SpeakNeural(phrase, gen, st, &pcm) || pcm.empty()) pcm.clear();
                }
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
                if (pcm.empty() && tts && tts->Synthesize(NormalizeForSpeechFr(phrase), pcm, lengthMul * baseLength.load(), noiseMul))
                    pcm = Resample(pcm, tts->SampleRate(), static_cast<int>(outputRate));
#endif
                if (!pcm.empty()) {
                    VoiceColor mic;  // même micro, appliqué une fois pour toutes
                    mic.Prepare(static_cast<int>(outputRate));
                    mic.SetMic(micEffect.load());
                    mic.Process(pcm.data(), pcm.size(), false);
                    ready[category].push_back(std::move(pcm));
                }
            }
        }
        std::scoped_lock lock(backchannelMutex);
        backchannels = std::move(ready);  // remplace l'ancien jeu (changement de ton)
        log::Info("VoiceEngine: relances prêtes");
    }
};

VoiceEngine::VoiceEngine() : impl_(std::make_unique<Impl>()) {}
VoiceEngine::~VoiceEngine() { Shutdown(); }

bool VoiceEngine::Initialize(const VoiceEngineConfig& cfg) {
    auto& d = *impl_;
    d.cfg = cfg;

#if COMPANION_WITH_WHISPER
    if (std::filesystem::exists(cfg.whisperModel)) {
        whisper_context_params cp = whisper_context_default_params();
        cp.use_gpu = false;  // 0 Mo VRAM
        d.whisper = whisper_init_from_file_with_params(log::Narrow(cfg.whisperModel.wstring()).c_str(), cp);
    }
    log::Info("VoiceEngine: STT {}", d.whisper ? "prêt" : "désactivé (modèle absent)");
#endif

#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
    if (std::filesystem::exists(cfg.piperModel)) {
        d.tts = std::make_unique<PiperTts>();
        if (!d.tts->Load(cfg.piperModel, cfg.espeakDataDir, cfg.ttsThreads)) d.tts.reset();
    }
    log::Info("VoiceEngine: TTS {}", d.tts ? "prêt" : "désactivé");
#endif

    d.outputRate = NeuralVoice::kSampleRate;  // rendu unique à 24 kHz ; Piper est rééchantillonné
    d.neuralEnabled = cfg.neuralEnabled;
    d.neuralInGame = cfg.neuralInGame;
    d.fx.Prepare(static_cast<float>(d.outputRate), cfg.fx);
    d.tempo.Prepare(static_cast<int>(d.outputRate));
    d.renderWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.renderThread = std::jthread([&d](std::stop_token st) { d.RenderMain(st); });
    d.worker = std::jthread([&d](std::stop_token st) { d.WorkerMain(st); });
    // Chargement de la voix neuronale en premier travail du thread de synthèse : l'appli démarre tout
    // de suite (Piper répond en attendant), les relances préparées ensuite profitent de la nouvelle voix.
    if (cfg.neuralEnabled) d.Post([&d] { d.neural.Load(d.cfg.neural); });
    d.glitcher.Prepare(static_cast<int>(d.outputRate));
    d.color.Prepare(static_cast<int>(d.outputRate));
    d.chunkBuf.reserve(static_cast<size_t>(d.outputRate) * 2);
    d.vrng ^= static_cast<std::uint32_t>(GetTickCount64());
    d.Post([&d] {
        if (d.vocals.Load(d.cfg.neural.voiceDir / L"vocal", static_cast<int>(d.outputRate))) log::Info("VoiceEngine: sons humains prêts");
    });
    return true;
}

void VoiceEngine::Shutdown() {
    if (!impl_) return;
    auto& d = *impl_;
    SetListenMode(ListenMode::PushToTalk);  // arrête la capture VAD
    EndListening();
    StopSpeaking();
    if (d.worker.joinable()) { d.worker.request_stop(); d.worker.join(); }
    if (d.renderThread.joinable()) {
        d.renderThread.request_stop();
        SetEvent(d.renderWake);
        d.renderThread.join();
    }
    if (d.renderWake) { CloseHandle(d.renderWake); d.renderWake = nullptr; }
#if COMPANION_WITH_WHISPER
    if (d.whisper) { whisper_free(d.whisper); d.whisper = nullptr; }
#endif
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
    d.tts.reset();
#endif
}

void VoiceEngine::SetTranscriptCallback(TranscriptCallback cb) {
    std::scoped_lock lock(impl_->cbMutex);
    impl_->onTranscript = std::move(cb);
}

void VoiceEngine::SetListenMode(ListenMode mode) {
    auto& d = *impl_;
    if (d.listenMode.exchange(mode) == mode) return;
    if (mode == ListenMode::VoiceActivity) {
        EndListening();
        d.vadThread = std::jthread([&d](std::stop_token st) { d.CaptureMain(st, /*vadMode=*/true); });
    } else if (d.vadThread.joinable()) {
        d.vadThread.request_stop();
        d.vadThread.join();
    }
    log::Info("VoiceEngine: écoute {}", mode == ListenMode::VoiceActivity ? "VAD" : "Push-to-Talk");
}

ListenMode VoiceEngine::GetListenMode() const noexcept { return impl_->listenMode.load(); }

void VoiceEngine::BeginListening() {
    auto& d = *impl_;
    if (d.listenMode.load() != ListenMode::PushToTalk) return;
    if (d.listening.exchange(true)) return;
    StopSpeaking();  // on ne parle pas par-dessus l'utilisateur
    {
        std::scoped_lock lock(d.captureMutex);
        d.captured.clear();
        d.captured.reserve(kSttSampleRate * 10);
    }
    d.captureThread = std::jthread([&d](std::stop_token st) { d.CaptureMain(st, /*vadMode=*/false); });
}

void VoiceEngine::EndListening() {
    auto& d = *impl_;
    if (!d.listening.exchange(false)) return;
    if (d.captureThread.joinable()) { d.captureThread.request_stop(); d.captureThread.join(); }
    std::vector<float> pcm;
    {
        std::scoped_lock lock(d.captureMutex);
        pcm.swap(d.captured);
    }
    d.Post([&d, pcm = std::move(pcm)]() mutable { d.Transcribe(std::move(pcm)); });
}

void VoiceEngine::SetVoiceTap(VoiceTap* tap) noexcept { impl_->tap.store(tap, std::memory_order_release); }

void VoiceEngine::TranscribeExternal(std::vector<float> pcm16k, std::function<void(std::string)> done) {
    Impl& d = *impl_;
    d.Post([&d, pcm = std::move(pcm16k), done = std::move(done)]() mutable {
        std::string text = d.Whisper(std::move(pcm));
        if (done) done(std::move(text));  // toujours appelé (vide = rien reconnu) : l'appelant compte ses envois
    });
}

bool VoiceEngine::IsListening() const noexcept { return impl_->listening.load() || impl_->vadInSpeech.load(); }

void VoiceEngine::Speak(std::string text, SpeakingStyle style) {
    auto& d = *impl_;
    if (!HasTts()) { log::Info("VoiceEngine (TTS absent): {}", text); return; }
    const uint32_t gen = d.speechGeneration.load();
    ++d.pendingSpeech;
    d.Post([&d, text = std::move(text), gen, style] { d.Synthesize(text, gen, style); });
}

void VoiceEngine::PrepareBackchannels(std::map<std::string, std::vector<std::string>> phrases) {
    auto& d = *impl_;
    d.Post([&d, phrases = std::move(phrases)] { d.SynthesizeBackchannels(phrases); });
}

bool VoiceEngine::PlayBackchannel(const std::string& category) {
    auto& d = *impl_;
    if (d.SpeakingNow()) return false;
    std::vector<float> pcm;
    {
        std::scoped_lock lock(d.backchannelMutex);
        const auto it = d.backchannels.find(category);
        if (it == d.backchannels.end() || it->second.empty()) return false;
        // Jamais deux fois la même relance d'affilée.
        d.rng = d.rng * 1664525u + 1013904223u;
        size_t idx = (d.rng >> 8) % it->second.size();
        auto& last = d.lastBackchannel[category];
        if (it->second.size() > 1 && idx == last) idx = (idx + 1) % it->second.size();
        last = idx;
        pcm = it->second[idx];
    }
    d.PushAudio(pcm, d.speechGeneration.load());
    return true;
}

void VoiceEngine::SetBargeIn(bool enabled, std::function<void()> onBargeIn) {
    auto& d = *impl_;
    d.bargeIn = enabled;
    std::scoped_lock lock(d.cbMutex);
    d.onBargeIn = std::move(onBargeIn);
}

void VoiceEngine::SetEmotionDetection(bool enabled) noexcept { impl_->emotionEnabled = enabled; }
void VoiceEngine::SetBaseSpeechRate(float lengthScale) noexcept { impl_->baseLength = std::clamp(lengthScale, 0.7f, 1.4f); }
void VoiceEngine::SetVoiceTempo(float tempo) noexcept { impl_->voiceTempo = std::clamp(tempo, 0.8f, 1.5f); }

std::string VoiceEngine::VoiceBaseline() const {
    std::scoped_lock lock(impl_->emotionMutex);
    return impl_->emotion.SaveBaseline();
}

void VoiceEngine::LoadVoiceBaseline(const std::string& s) {
    std::scoped_lock lock(impl_->emotionMutex);
    impl_->emotion.LoadBaseline(s);
}

void VoiceEngine::StopSpeaking() { impl_->StopSpeakingNow(); }  // invalide les synthèses en file et coupe la voix

bool VoiceEngine::IsSpeaking() const noexcept { return impl_->SpeakingNow(); }

void VoiceEngine::SetMood(float mood01) noexcept {
    impl_->baseMood = std::clamp(mood01, 0.f, 1.f);
    impl_->fx.SetMood(impl_->baseMood.load());
}
float VoiceEngine::VoiceRms() const noexcept { return impl_->rms.load(std::memory_order_relaxed); }
void VoiceEngine::SetGamingMode(bool gaming) noexcept {
    auto& d = *impl_;
    if (d.gaming.exchange(gaming) == gaming) return;
    // En jeu, la voix neuronale rend sa VRAM (~1,5 Go) au jeu ; elle revient en arrière-plan après la partie
    // (Piper répond en attendant). Fait sur le thread de synthèse : jamais pendant une phrase.
    if (!d.neuralEnabled.load() || d.neuralInGame.load()) return;
    if (gaming) d.Post([&d] { if (d.gaming.load()) d.neural.Unload(); });
    else d.Post([&d] { if (!d.gaming.load() && d.neuralEnabled.load() && !d.neural.Ready()) d.neural.Load(d.cfg.neural); });
}
void VoiceEngine::CaptureNextUtterance(RawUtterance cb) {
    std::scoped_lock lock(impl_->cbMutex);
    impl_->rawCapture = std::move(cb);
}
void VoiceEngine::SetNeuralVoiceDir(std::filesystem::path dir) {
    auto& d = *impl_;
    d.Post([&d, dir = std::move(dir)] {
        d.cfg.neural.voiceDir = dir;
        d.neural.Unload();
        if (d.neuralEnabled.load() && !d.gaming.load()) d.neural.Load(d.cfg.neural);
        log::Info("Voix : prises de {}", log::Narrow(dir.filename().wstring()));
    });
}
void VoiceEngine::SetNeuralVoice(bool enabled, bool inGame) noexcept {
    auto& d = *impl_;
    d.neuralInGame = inGame;
    if (enabled && !d.neuralEnabled.exchange(true) && !d.neural.Ready()) d.Post([&d] { d.neural.Load(d.cfg.neural); });
    d.neuralEnabled = enabled;
}
bool VoiceEngine::HasNeuralVoice() const noexcept { return impl_->neural.Ready(); }
void VoiceEngine::SetQuirks(float quirks01) noexcept { impl_->quirks = std::clamp(quirks01, 0.f, 1.f); }
void VoiceEngine::LoadMissing(bool neuralWanted) {
    auto& d = *impl_;
    d.Post([&d, neuralWanted] {
#if COMPANION_WITH_WHISPER
        if (!d.whisper && std::filesystem::exists(d.cfg.whisperModel)) {
            whisper_context_params cp = whisper_context_default_params();
            cp.use_gpu = false;
            d.whisper = whisper_init_from_file_with_params(log::Narrow(d.cfg.whisperModel.wstring()).c_str(), cp);
            log::Info("VoiceEngine: STT {}", d.whisper ? "prêt" : "toujours absent");
        }
#endif
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
        if (!d.tts && std::filesystem::exists(d.cfg.piperModel)) {
            d.tts = std::make_unique<PiperTts>();
            if (!d.tts->Load(d.cfg.piperModel, d.cfg.espeakDataDir, d.cfg.ttsThreads)) d.tts.reset();
        }
#endif
        if (neuralWanted) {
            d.neuralEnabled = true;
            if (!d.neural.Ready() && !d.gaming.load()) d.neural.Load(d.cfg.neural);
        }
    });
}

void VoiceEngine::SetSoundResolver(SoundResolver resolver) {
    auto& d = *impl_;
    d.Post([&d, r = std::move(resolver)]() mutable { d.soundResolver = std::move(r); });
}
void VoiceEngine::PlaySound(const std::string& name) {
    auto& d = *impl_;
    const uint32_t gen = d.speechGeneration.load();
    ++d.pendingSpeech;
    d.Post([&d, name, gen] {
        if (d.soundResolver)
            if (const auto pcm = d.soundResolver(name)) d.PushAudio(*pcm, gen);
        --d.pendingSpeech;
    });
}
void VoiceEngine::SetMicEffect(float amount01) noexcept { impl_->micEffect = std::clamp(amount01, 0.f, 1.f); }
void VoiceEngine::SetPitchLife(float amount01) noexcept { impl_->pitchLife = std::clamp(amount01, 0.f, 1.f); }
void VoiceEngine::SetSystemLoad(float load01) noexcept { impl_->systemLoad.store(std::clamp(load01, 0.f, 1.f), std::memory_order_relaxed); }
void VoiceEngine::SetEmotionIntensity(float intensity) noexcept { impl_->emotionIntensity = std::clamp(intensity, 0.f, 1.5f); }

bool VoiceEngine::HasStt() const noexcept {
#if COMPANION_WITH_WHISPER
    return impl_->whisper != nullptr;
#else
    return false;
#endif
}

bool VoiceEngine::HasTts() const noexcept {
    if (impl_->neural.Ready()) return true;
#if COMPANION_WITH_ONNX && COMPANION_WITH_ESPEAK
    return impl_->tts != nullptr;
#else
    return false;
#endif
}

}  // namespace companion
