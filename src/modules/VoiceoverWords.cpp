#include "modules/VoiceoverWords.hpp"

#include "audio/AudioDecode.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <thread>

#if COMPANION_WITH_WHISPER
#include <whisper.h>
#endif

namespace companion {

std::vector<TimedWord> TranscribeWords(const std::filesystem::path& audio, const std::filesystem::path& whisperModel, int maxSeconds,
                                       bool gpu) {
    if (!std::filesystem::exists(whisperModel)) return {};
    auto words = TranscribeWordsPcm(DecodeAudio16k(audio, maxSeconds), whisperModel, gpu);
    log::Info("Voix off : {} mots transcrits ({})", words.size(), log::Narrow(audio.filename().wstring()));
    return words;
}

std::vector<TimedWord> TranscribeWordsPcm(const std::vector<float>& pcm, const std::filesystem::path& whisperModel, bool gpu) {
    std::vector<TimedWord> words;
#if COMPANION_WITH_WHISPER
    if (pcm.size() < 16000) return words;
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = gpu;
    cp.flash_attn = gpu;
    whisper_context* w = whisper_init_from_file_with_params(log::Narrow(whisperModel.wstring()).c_str(), cp);
    if (!w) return words;
    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.language = "fr";
    wp.n_threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2));  // un seul gros calcul : tous les cœurs physiques
    wp.print_progress = wp.print_realtime = wp.print_timestamps = wp.print_special = false;
    // Un segment par mot, avec ses instants de début et de fin (centièmes de seconde).
    wp.token_timestamps = true;
    wp.max_len = 1;
    wp.split_on_word = true;
    wp.suppress_blank = true;
    if (whisper_full(w, wp, pcm.data(), static_cast<int>(pcm.size())) == 0) {
        for (int i = 0; i < whisper_full_n_segments(w); ++i) {
            std::string t = whisper_full_get_segment_text(w, i);
            while (!t.empty() && t.front() == ' ') t.erase(0, 1);
            if (t.empty() || t.front() == '[' || t.front() == '(') continue;  // [Musique], (rires)
            words.push_back({t, static_cast<double>(whisper_full_get_segment_t0(w, i)) / 100.0,
                             static_cast<double>(whisper_full_get_segment_t1(w, i)) / 100.0});
        }
    }
    whisper_free(w);
#else
    (void)pcm;
    (void)gpu;
    (void)whisperModel;
#endif
    return words;
}

}  // namespace companion
