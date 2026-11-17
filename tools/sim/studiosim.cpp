// Simulation : FL Studio « joue » beat.f32 → OmnisAnalyzer → alerte (banque de phrases) → avis du modèle local.
#include "audio/OmnisAnalyzer.hpp"
#include "core/LocalBrain.hpp"
#include "core/Persona.hpp"
#include "core/Phrases.hpp"
#include <cstdio>
#include <fstream>
#include <vector>
using namespace companion;
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<float> pcm((std::istreambuf_iterator<char>(f)), {});  // rempli ci-dessous
    f.clear(); f.seekg(0, std::ios::end); pcm.resize(f.tellg() / 4); f.seekg(0); f.read(reinterpret_cast<char*>(pcm.data()), pcm.size() * 4);
    Phrases phrases; phrases.Load(argv[3]);
    OmnisAnalyzer an; std::vector<std::string> alerts;
    an.SetAlertCallback([&](MixAlert k, const MixMetrics& m) {
        phrases.SetTone(Tone::Unfiltered);
        alerts.push_back(k == MixAlert::Clipping ? phrases.Pick("clipping", {{"peak", std::format("{:.1f}", m.peakDb)}})
                                                 : phrases.Pick("phase"));
    });
    for (size_t i = 0; i + 960 <= pcm.size(); i += 960) { an.Push(pcm.data() + i, 480); an.DrainNow(); }
    const auto mix = an.Snapshot();
    std::printf("SUMMARY|%s\n%s\n", mix.ToSummary().c_str(), mix.Diagnosis().c_str());
    for (auto& a : alerts) std::printf("ALERT|%s\n", a.c_str());
    std::fflush(stdout);
    LocalBrain brain; LocalBrainConfig bc; bc.model = argv[2]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 8;
    if (!brain.Load(bc)) { std::puts("LOAD FAIL"); return 1; }
    const char* question = "Alors, t'en penses quoi de mon son ? Sois honnête.";
    const std::string ctx = "[Ton de voix] Il semble joyeux (voix haute et animée, il est fier de lui).\n" + mix.ToSummary() + "\n" + mix.Diagnosis() +
        "\n[Consigne] Donne ton avis sur ce son en 2 ou 3 phrases. Appuie-toi UNIQUEMENT sur le diagnostic de l'ingé son (ne réinterprète pas les chiffres bruts) : commence par le problème le plus grave, cite un chiffre, donne UNE action concrète.\n";
    const int takes = argc > 4 ? std::atoi(argv[4]) : 2;
    for (int tone : {0, 1, 2}) for (int k = 0; k < (tone == 2 ? takes : 1); ++k) {
        std::vector<ChatTurn> msgs{{"system", PersonaPrompt("Omnis", tone)},
                                   {"user", "[Contexte]\n" + ctx + ToneReminder(tone) + "\n\n[Message]\n" + question}};
        GenerateOptions o; o.grammar = kEscalateGrammar; o.maxTokens = 120; o.temperature = 0.75f;
        auto out = brain.Generate(msgs, o);
        std::printf("TONE%d|%s\n", tone, out ? out->c_str() : "ECHEC"); std::fflush(stdout);
    }
}
