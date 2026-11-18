// TempoStretch sur les répliques de la démo : durée, syllabes/s, blancs. in.f32 out.f32 tempo
#include "audio/TempoStretch.hpp"
#include <cstdio>
#include <cstdlib>
int main(int, char** argv) {
    std::vector<float> x; FILE* f = std::fopen(argv[1], "rb"); float v; while (std::fread(&v, 4, 1, f) == 1) x.push_back(v); std::fclose(f);
    companion::TempoStretch ts; ts.Prepare(24000); ts.SetTempo(std::atof(argv[3]));
    std::vector<float> out, c;
    for (size_t i = 0; i < x.size(); i += 1920) {  // morceaux de 80 ms, comme le streaming de la voix
        c.assign(x.begin() + i, x.begin() + std::min(x.size(), i + 1920));
        ts.Process(c); out.insert(out.end(), c.begin(), c.end());
    }
    ts.Flush(c); out.insert(out.end(), c.begin(), c.end());
    f = std::fopen(argv[2], "wb"); std::fwrite(out.data(), 4, out.size(), f); std::fclose(f);
    std::printf("%.2f s -> %.2f s\n", x.size() / 24000.0, out.size() / 24000.0);
}
