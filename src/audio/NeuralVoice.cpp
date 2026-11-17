#include "audio/NeuralVoice.hpp"

#include "audio/TextNormalizerFr.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#if COMPANION_WITH_QWENTTS
#define QWEN_STATIC  // types seulement : les fonctions sont résolues dans qwen.dll au chargement
#include "qwen.h"
#endif

namespace companion {

bool ReadWavMono(const std::filesystem::path& file, std::vector<float>& out, int& sampleRate) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
    const auto u16 = [&](size_t o) { std::uint16_t v; std::memcpy(&v, data.data() + o, 2); return v; };
    const auto u32 = [&](size_t o) { std::uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;
    int format = 0, channels = 0, bits = 0;
    for (size_t pos = 12; pos + 8 <= data.size();) {
        const std::uint32_t size = u32(pos + 4);
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && size >= 16) {
            format = u16(pos + 8);
            channels = u16(pos + 10);
            sampleRate = static_cast<int>(u32(pos + 12));
            bits = u16(pos + 22);
            if (format == 0xFFFE && size >= 26) format = u16(pos + 32);  // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0 && channels > 0) {
            const size_t end = std::min<size_t>(data.size(), pos + 8 + size);
            const size_t bytes = static_cast<size_t>(bits / 8) * static_cast<size_t>(channels);
            if (bytes == 0) return false;
            out.clear();
            for (size_t o = pos + 8; o + bytes <= end; o += bytes) {
                float acc = 0.f;
                for (int c = 0; c < channels; ++c) {
                    const size_t s = o + static_cast<size_t>(c) * (bits / 8);
                    if (format == 1 && bits == 16) acc += static_cast<std::int16_t>(u16(s)) / 32768.f;
                    else if (format == 3 && bits == 32) { float v; std::memcpy(&v, data.data() + s, 4); acc += v; }
                    else return false;
                }
                out.push_back(acc / static_cast<float>(channels));
            }
            return !out.empty();
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

#if COMPANION_WITH_QWENTTS

struct NeuralVoice::Impl {
    HMODULE dll = nullptr;
    decltype(&qt_init_default_params) initDefaults = nullptr;
    decltype(&qt_init) init = nullptr;
    decltype(&qt_free) free = nullptr;
    decltype(&qt_tts_default_params) ttsDefaults = nullptr;
    decltype(&qt_synthesize) synthesize = nullptr;
    decltype(&qt_extract_voice_ref) extract = nullptr;
    decltype(&qt_voice_ref_free) refFree = nullptr;
    decltype(&qt_last_error) lastError = nullptr;
    decltype(&qt_audio_free) audioFree = nullptr;

    qt_context* ctx = nullptr;
    NeuralVoiceConfig cfg;
    std::vector<float> speaker;  // empreinte de la prise « normal »
    struct StyleRef {
        qt_voice_ref ref{};
        std::string text;
        bool ok = false;
    };
    std::array<StyleRef, kSpeakingStyleCount> styles{};

    ~Impl() {
        if (refFree)
            for (auto& s : styles) if (s.ok) refFree(&s.ref);
        if (ctx && free) free(ctx);
        if (dll) FreeLibrary(dll);
    }

    template <class F>
    bool Resolve(F& fn, const char* name) {
        fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
        if (!fn) log::Info("NeuralVoice : {} absent de la DLL", name);
        return fn != nullptr;
    }
};

NeuralVoice::NeuralVoice() = default;
NeuralVoice::~NeuralVoice() = default;
bool NeuralVoice::Ready() const noexcept { return impl_ && impl_->ctx && impl_->styles[0].ok; }
void NeuralVoice::Unload() noexcept { impl_.reset(); }

bool NeuralVoice::Load(const NeuralVoiceConfig& cfg) {
    impl_.reset();
    if (!std::filesystem::exists(cfg.talker) || !std::filesystem::exists(cfg.codec)) {
        log::Info("NeuralVoice : modèles absents ({}), repli Piper", log::Narrow(cfg.talker.wstring()));
        return false;
    }
    auto d = std::make_unique<Impl>();
    d->cfg = cfg;
    d->dll = LoadLibraryW(cfg.library.wstring().c_str());
    if (!d->dll)  // MinGW nomme la DLL « libqwen.dll », MSVC « qwen.dll »
        d->dll = LoadLibraryW((cfg.library.parent_path() / (L"lib" + cfg.library.filename().wstring())).wstring().c_str());
    if (!d->dll) {
        log::Info("NeuralVoice : {} introuvable, repli Piper", log::Narrow(cfg.library.wstring()));
        return false;
    }
    if (!d->Resolve(d->initDefaults, "qt_init_default_params") || !d->Resolve(d->init, "qt_init") ||
        !d->Resolve(d->free, "qt_free") || !d->Resolve(d->ttsDefaults, "qt_tts_default_params") ||
        !d->Resolve(d->synthesize, "qt_synthesize") || !d->Resolve(d->extract, "qt_extract_voice_ref") ||
        !d->Resolve(d->refFree, "qt_voice_ref_free") || !d->Resolve(d->lastError, "qt_last_error") ||
        !d->Resolve(d->audioFree, "qt_audio_free"))
        return false;

    const std::string talker = log::Narrow(cfg.talker.wstring()), codec = log::Narrow(cfg.codec.wstring());
    qt_init_params ip;
    d->initDefaults(&ip);
    ip.talker_path = talker.c_str();
    ip.codec_path = codec.c_str();
    ip.device = cfg.device.empty() ? nullptr : cfg.device.c_str();
    d->ctx = d->init(&ip);
    if (!d->ctx) {
        log::Info("NeuralVoice : initialisation impossible ({})", d->lastError());
        return false;
    }

    // Prises de référence : extraites une fois au chargement (empreinte + codes), réutilisées à chaque phrase.
    constexpr std::array<const char*, kSpeakingStyleCount> names{"normal", "bright", "soft", "calm", "amused", "surprised", "annoyed"};
    for (size_t i = 0; i < names.size(); ++i) {
        std::vector<float> pcm;
        int rate = 0;
        const auto wav = cfg.voiceDir / (std::string(names[i]) + ".wav");
        std::ifstream txt(cfg.voiceDir / (std::string(names[i]) + ".txt"), std::ios::binary);
        if (!ReadWavMono(wav, pcm, rate) || rate != kSampleRate || !txt) {
            log::Info("NeuralVoice : prise « {} » absente ou pas en 24 kHz", names[i]);
            continue;
        }
        auto& s = d->styles[i];
        s.text.assign((std::istreambuf_iterator<char>(txt)), {});
        if (d->extract(d->ctx, pcm.data(), static_cast<int>(pcm.size()), &s.ref) != QT_STATUS_OK) {
            log::Info("NeuralVoice : extraction de « {} » impossible ({})", names[i], d->lastError());
            continue;
        }
        s.ok = true;
    }
    if (!d->styles[0].ok) return false;
    d->speaker.assign(d->styles[0].ref.ref_spk_emb, d->styles[0].ref.ref_spk_emb + d->styles[0].ref.ref_spk_dim);
    log::Info("NeuralVoice : prête ({})", cfg.device.empty() ? "périphérique auto" : cfg.device);
    impl_ = std::move(d);
    return true;
}

bool NeuralVoice::Speak(const std::string& text, SpeakingStyle style, const ChunkFn& onChunk, float temperatureJitter) {
    if (!Ready()) return false;
    auto& d = *impl_;
    const auto& s = d.styles[static_cast<size_t>(style)].ok ? d.styles[static_cast<size_t>(style)] : d.styles[0];
    const std::string spoken = NormalizeForSpeechFr(text);
    if (spoken.find_first_not_of(" \t\r\n") == std::string::npos) return true;

    struct Ctx {
        const ChunkFn* fn;
        bool stop = false;
    } c{&onChunk};
    qt_tts_params p;
    d.ttsDefaults(&p);
    p.text = spoken.c_str();
    p.lang = d.cfg.language.c_str();
    p.ref_spk_emb = d.speaker.data();
    p.ref_spk_dim = static_cast<int>(d.speaker.size());
    p.ref_codes = s.ref.ref_codes;
    p.ref_T = s.ref.ref_T;
    p.ref_text = s.text.c_str();
    p.temperature = std::clamp(d.cfg.temperature + temperatureJitter, 0.5f, 1.1f);
    // Garde-fou : ~12,5 trames/s, au plus ~3 fois la durée « normale » d'une lecture (15 car./s).
    p.max_new_tokens = std::max(48, static_cast<int>(spoken.size() * 12.5 * 3 / 15));
    p.on_chunk = [](const float* samples, int n, void* user) -> bool {
        auto* cx = static_cast<Ctx*>(user);
        if (cx->stop) return false;
        if (!(*cx->fn)(std::span<const float>(samples, static_cast<size_t>(n)))) cx->stop = true;
        return !cx->stop;
    };
    p.on_chunk_user_data = &c;
    p.cancel = [](void* user) -> bool { return static_cast<Ctx*>(user)->stop; };
    p.cancel_user_data = &c;
    qt_audio unused{};
    const auto status = d.synthesize(d.ctx, &p, &unused);
    if (unused.samples) d.audioFree(&unused);
    if (status == QT_STATUS_OK || status == QT_STATUS_CANCELLED) return true;
    log::Info("NeuralVoice : synthèse échouée ({})", d.lastError());
    return false;
}

#else  // !COMPANION_WITH_QWENTTS

struct NeuralVoice::Impl {};
NeuralVoice::NeuralVoice() = default;
NeuralVoice::~NeuralVoice() = default;
bool NeuralVoice::Ready() const noexcept { return false; }
void NeuralVoice::Unload() noexcept {}
bool NeuralVoice::Load(const NeuralVoiceConfig&) { return false; }
bool NeuralVoice::Speak(const std::string&, SpeakingStyle, const ChunkFn&, float) { return false; }

#endif

}  // namespace companion
