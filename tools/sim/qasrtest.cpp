// Qwen3-ASR (via LocalBrain::TranscribeAudio, le même code que l'appli) sur des extraits 16 kHz f32.
//   qasrtest modèle.gguf mmproj.gguf avecContexte(0/1) fichiers.f32...
#include "core/LocalBrain.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
using namespace companion;
int main(int argc, char** argv) {
    LocalBrain brain;
    LocalBrainConfig bc; bc.model = argv[1]; bc.mmproj = argv[2]; bc.contextTokens = 2048; bc.gpuLayers = 0; bc.threads = std::getenv("THREADS") ? std::atoi(std::getenv("THREADS")) : 4;
    if (!brain.Load(bc)) return 1;
    const std::string ctx = std::atoi(argv[3]) ? "Omnis, FL Studio, ASIO, vu-mètre, sidechain, 808, kick, mix, master, prod, LoL, Valorant, Jinx, Thresh, Kai'Sa, Yasuo." : "";
    for (int i = 4; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<float> pcm(b.size() / 4); std::memcpy(pcm.data(), b.data(), pcm.size() * 4);
        const auto t0 = std::chrono::steady_clock::now();
        std::string lang;
        const bool free = std::getenv("FREE_LANG") != nullptr;
        const auto r = brain.TranscribeAudio(pcm, ctx, free ? "" : "French", 160, &lang);
        std::printf("%5.1fs | [%s] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), lang.c_str(), r ? r->c_str() : "(échec)");
        std::fflush(stdout);
    }
}
