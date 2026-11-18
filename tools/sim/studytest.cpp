// Étape « étude d'une vidéo » avec le vrai modèle et le vrai prompt (VideoParse). Transcription d'exemple écrite
// pour le test (YouTube bloque les sous-titres depuis un datacenter). Usage : studytest modèle.gguf cache
#include "core/CallPrompt.hpp"
#include "core/LocalBrain.hpp"
#include "modules/VideoParse.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const std::string title = "Battle de répartie entre potes : le mec qui dit qu'il est Diamant";
    const std::string transcript =
        "— Moi je suis Diamant, tranquille. — Diamant ? T'es Diamant comme mon grand frère est astronaute : dans sa tête. "
        "— Non mais j'ai juste eu des mauvais mates. — Ah oui, les mauvais mates, ces gens qui te suivent de partie en partie depuis trois saisons. "
        "— Vas-y joue, toi. — Moi au moins quand je perds, je dis pas que c'est la faute du ping. — Le ping il était à 200 ! "
        "— Le ping il a bon dos, il est plus haut que ton classement.";
    const std::string comments = "- « ces gens qui te suivent depuis trois saisons » je suis mort (5400 likes)\n- le ping plus haut que ton classement 💀 (3100 likes)\n";
    GenerateOptions o; o.maxTokens = 380; o.temperature = 0.4f; o.grammar = kStudyGrammar;
    auto out = brain.Generate({{"system", kStudySystem}, {"user", StudyPrompt(title, transcript, comments)}}, o);
    std::printf("JSON brut : %s\n", out ? out->c_str() : "(rien)");
    StudyResult r = ParseStudy(out ? *out : "", "test");
    std::erase_if(r.forms, [](const BaitForm& f) { return !CrossesLine(f.form, {}).empty(); });
    std::printf("humour=%d technique=%s\npourquoi=%s\nformes gardées (%zu) :\n", r.humour, r.technique.c_str(), r.why.c_str(), r.forms.size());
    for (const auto& f : r.forms) std::printf("  - %s\n", f.form.c_str());
}
