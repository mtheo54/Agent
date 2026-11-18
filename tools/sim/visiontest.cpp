// Vision locale sur un collage de vidéo (même prompt que « regarde avec moi »). modèle mmproj image.rgb largeur hauteur
#include "core/LocalBrain.hpp"
#include "modules/VideoParse.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain;
    LocalBrainConfig bc; bc.model = argv[1]; bc.mmproj = argv[2]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    std::ifstream f(argv[3], std::ios::binary);
    std::vector<unsigned char> rgb((std::istreambuf_iterator<char>(f)), {});
    GenerateOptions o; o.maxTokens = 100; o.temperature = 0.2f;
    const auto t0 = std::chrono::steady_clock::now();
    auto out = brain.GenerateWithImage({{"user", std::string(LocalBrain::MediaMarker()) + "\n" + kWatchVisionPrompt}}, rgb.data(), std::atoi(argv[4]), std::atoi(argv[5]), o);
    std::printf("vision (%.0f s) : %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), out ? out->c_str() : "(échec)");
}
