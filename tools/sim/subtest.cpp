// Voix off -> mots horodatés (Whisper) -> sous-titres + synchro. subtest voix.f32 ggml-base.bin out.jsx
#include "modules/Subtitles.hpp"
#include "modules/VoiceoverWords.hpp"
#include <cstdio>
#include <fstream>
namespace companion { std::vector<float> DecodeAudio16k(const std::filesystem::path&, int) { return {}; } }
using namespace companion;
int main(int, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> pcm((std::istreambuf_iterator<char>(f)), {});
    std::vector<float> s(pcm.size() / 4);
    std::memcpy(s.data(), pcm.data(), s.size() * 4);
    auto words = TranscribeWordsPcm(s, argv[2]);
    for (auto& w : words) { w.start += 2.0; w.end += 2.0; }  // voix off posée à 2 s dans la composition
    for (const auto& w : words) std::printf("%6.2f-%6.2f %s\n", w.start, w.end, w.text.c_str());
    const auto chunks = ChunkWords(words);
    std::printf("\n%zu sous-titres :\n", chunks.size());
    for (const auto& c : chunks) std::printf("%6.2f-%6.2f %s%s\n", c.start, c.end, c.key ? "[FORT] " : "", c.text.c_str());
    for (const char* q : {"kick", "tempete", "Tempête", "le limiteur", "DLC payant", "abonne-toi", "lancements"}) {
        const auto t = FindWordTime(words, q);
        std::printf("quand « %s » : %s\n", q, t ? std::to_string(*t).c_str() : "introuvable");
    }
    std::ofstream(argv[3]) << AeWrap(SubtitleBody(chunks), "resultat.txt", WordsTable(words));
}
