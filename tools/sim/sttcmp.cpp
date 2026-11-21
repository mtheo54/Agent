// sttcmp modèle beam(0/1) fichiers.f32... : transcription comme l'appli (même vocabulaire), temps par extrait
#include <whisper.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
int main(int argc, char** argv) {
    whisper_context_params cp = whisper_context_default_params(); cp.use_gpu = false;
    whisper_context* w = whisper_init_from_file_with_params(argv[1], cp);
    const bool beam = std::atoi(argv[2]);
    for (int i = 3; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<float> pcm(b.size() / 4); std::memcpy(pcm.data(), b.data(), pcm.size() * 4);
        whisper_full_params wp = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
        if (beam) wp.beam_search.beam_size = 5;
        wp.language = "fr"; wp.n_threads = 2; wp.no_context = true; wp.suppress_blank = true;
        wp.print_progress = wp.print_realtime = wp.print_timestamps = wp.print_special = false;
        wp.initial_prompt = "Omnis, FL Studio, DAW, MAO, VST, ASIO, vu-mètre, limiteur, mode, 808, kick, snare, sidechain, mix, master, prod, beat, instru, Antelope, Zen Go, League of Legends, LoL, Valorant, Discord, AutoCAD Electrical, After Effects.";
        const auto t0 = std::chrono::steady_clock::now();
        std::string text;
        if (whisper_full(w, wp, pcm.data(), (int)pcm.size()) == 0)
            for (int s = 0; s < whisper_full_n_segments(w); ++s) text += whisper_full_get_segment_text(w, s);
        std::printf("%5.1fs |%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), text.c_str());
        std::fflush(stdout);
    }
}
