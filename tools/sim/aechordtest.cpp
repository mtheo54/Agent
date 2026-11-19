// Scripts de logiciels : accords FL (notes + script piano roll) et scripts After Effects écrits par le modèle.
//   aechordtest outdir [modèle.gguf cache]
#include "core/LocalBrain.hpp"
#include "modules/AeScript.hpp"
#include "modules/ChordScript.hpp"
#include "modules/Subtitles.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>
using namespace companion;
static const char* kName[] = {"do", "do#", "ré", "ré#", "mi", "fa", "fa#", "sol", "sol#", "la", "la#", "si"};
int main(int argc, char** argv) {
    const std::string out = argv[1];
    const auto chords = ParseChordPlan("TONALITE A mineur\nDEGRE 1 1\nDEGRE 6 +7 1\nDEGRE 3 1\nDEGRE 7 +7 1\nDEGRE 2 +7 2\nDEGRE 4 +9 1\n");
    const auto notes = VoiceChords(chords);
    double beat = -1;
    for (const auto& n : notes) {
        if (n.beat != beat) { beat = n.beat; std::printf("\n%5.1f :", beat); }
        std::printf(" %s%d", kName[n.pitch % 12], n.pitch / 12 - 1);
    }
    std::printf("\n");
    std::ofstream(out + "/accords.pyscript") << ChordPyScript(chords, notes);
    {
        const std::string w = AeWrap("@@CORPS@@", "C:/x/resultat.txt");
        std::printf("corps à la ligne %d\n", static_cast<int>(std::count(w.begin(), w.begin() + w.find("@@CORPS@@"), '\n')) + 1);
        std::ofstream(out + "/wrap.jsx") << AeWrap("var l = omnisTitle(\"ABONNE-TOI\", 120); omnisSlide(l, \"bas\", 1); omnisPop(omnisLayers()[0]);\n"
                                                   "omnisShake(omnisLayers(), 4, 3); omnisStagger(omnisLayers(), 3, function (x, t) { omnisFadeOut(x); });\n"
                                                   "var z = null; z.boom(); rapport = \"ok\";", "C:/x/resultat.txt");
    }
    std::printf("interdit : '%s' / ES3 : '%s' / '%s'\n", AeForbidden("var f = new File(\"a\")").c_str(), AeModernSyntax("let a = 1;").c_str(),
                AeModernSyntax("var constant = 2; var letter = 1;").c_str());
    if (argc < 4) return 0;
    LocalBrain brain; brain.SetStateCacheDir(argv[3]);
    LocalBrainConfig bc; bc.model = argv[2]; bc.contextTokens = 6144; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const char* asks[] = {"Anime le texte sélectionné : qu'il apparaisse en pop avec un petit rebond, style motion design pro",
                          "Ajoute un titre « ABONNE-TOI » centré, blanc, gros, qui glisse depuis le bas avec un fondu",
                          "Mets un léger tremblement de caméra sur tous les calques",
                          "Fais apparaître tous les calques en cascade depuis la gauche, puis qu'ils disparaissent en fondu à la fin",
                          "Mets le calque sélectionné en rouge vif",
                          "Ajoute une lueur et une ombre sur le titre sélectionné"};
    // Même boucle que l'appli : script -> exécution (faux After Effects, node) -> erreur renvoyée au modèle, 1 correction.
    // Voix off factice (« … ton kick il a l'aura… abonne-toi … »), pour la synchro.
    const std::vector<TimedWord> words = {{"Salut", 0.5, 0.8}, {"ton", 1.0, 1.2}, {"kick", 1.2, 1.6}, {"il", 1.7, 1.8},
                                          {"tape", 1.8, 2.2}, {"abonne-toi", 4.0, 4.6}, {"maintenant", 4.6, 5.2}};
    std::vector<const char*> list(std::begin(asks), std::end(asks));
    if (argc > 4) list = {"Ajoute un titre ABONNE-TOI qui apparaît en pop quand je le dis",
                          "Fais glisser le titre sélectionné depuis la droite quand je dis kick",
                          "Mets un pop sur le calque sélectionné ici"};
    int i = 0, ok = 0;
    for (const char* a : list) {
        std::vector<ChatTurn> msgs{{"system", kAeSystem}, {"user", std::string("Projet ouvert : « clip.aep ».\nDemande : ") + a}};
        std::printf("\n=== %s\n", a);
        {
            GenerateOptions ro; ro.maxTokens = 80; ro.temperature = 0.2f; ro.grammar = kAeRecipeGrammar;
            const auto plan = brain.Generate({{"system", kAeRecipeSystem}, {"user", a}}, ro).value_or("");
            std::string said;
            std::string whenErr;
            const std::string body = AeRecipeBody(plan, said, LowerFr(a), [&](std::string_view w) { return FindWordTime(words, w); }, &whenErr);
            if (!whenErr.empty()) std::printf("(mot : %s)\n", whenErr.c_str());
            std::printf("--- recette\n%s", plan.c_str());
            if (!body.empty()) {
                const std::string file = out + "/ae" + std::to_string(i) + "_r.jsx";
                std::ofstream(file) << AeWrap(body, "resultat.txt", WordsTable(words));
                std::string res;
                if (FILE* pp = popen(("node " + out + "/../aemock.js " + file).c_str(), "r")) { char buf[512]; while (fgets(buf, sizeof buf, pp)) res += buf; pclose(pp); }
                std::printf("=> %s", res.c_str());
                if (res.starts_with("OK")) { ++ok; ++i; continue; }
            }
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            GenerateOptions o; o.maxTokens = 900; o.temperature = 0.2f;
            const auto r = brain.Generate(msgs, o);
            const std::string code = AeStripFences(r.value_or(""));
            const std::string file = out + "/ae" + std::to_string(i) + "_" + std::to_string(attempt) + ".jsx";
            std::ofstream(file) << AeWrap(code, "resultat.txt");
            std::string res;
            if (FILE* pp = popen(("node " + out + "/../aemock.js " + file).c_str(), "r")) {
                char buf[512];
                while (fgets(buf, sizeof buf, pp)) res += buf;
                pclose(pp);
            }
            std::printf("--- essai %d\n%s\n=> %s", attempt + 1, code.c_str(), res.c_str());
            if (res.starts_with("OK")) { ++ok; break; }
            const std::string err = res.substr(res.find('|') + 1, res.find('\n') - res.find('|') - 1);
            msgs.push_back({"assistant", code});
            msgs.push_back({"user", "After Effects a répondu : " + err + ". Corrige et réécris le script complet."});
        }
        ++i;
        std::fflush(stdout);
    }
    std::printf("\nRÉUSSIS : %d / %d\n", ok, i);
}
