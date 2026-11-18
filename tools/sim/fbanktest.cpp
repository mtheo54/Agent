// Calcule les fbank Kaldi (SpeakerFeatures) d'un fichier f32 16 kHz mono : in.f32 out.f32 scale
#include "audio/SpeakerFeatures.hpp"
#include <cstdio>
#include <cstdlib>
int main(int, char** argv) {
    FILE* f = std::fopen(argv[1], "rb"); std::vector<float> x; float v;
    while (std::fread(&v, 4, 1, f) == 1) x.push_back(v); std::fclose(f);
    companion::KaldiFbank fb; int frames = 0;
    auto out = fb.Compute(x.data(), x.size(), std::atof(argv[3]), frames);
    f = std::fopen(argv[2], "wb"); std::fwrite(out.data(), 4, out.size(), f); std::fclose(f);
    std::printf("%d\n", frames);
}
