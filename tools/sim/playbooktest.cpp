// Méthode de pro (playbook) : détection du domaine + réponse avec / sans méthode. modèle.gguf cache playbooks.json
#include "core/LocalBrain.hpp"
#include "core/OmnisModes.hpp"
#include "core/Playbooks.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    const auto books = LoadPlaybooks(argv[3]);
    const char* qs[] = {"Comment je rends mes animations de texte plus pro dans After Effects ?",
                        "Mon kick et ma 808 se mélangent, ça fait de la bouillie, je fais quoi ?",
                        "Comment je numérote mes fils proprement sur AutoCAD Electrical ?",
                        "Je perds toujours ma lane en mid, comment je gère mieux la vague ?",
                        "Comment j'arrête de rater mes duels sur Valorant ?", "C'est quoi le meilleur débit pour streamer sur Twitch ?"};
    for (const char* q : qs) {
        const Playbook* b = DetectPlaybook(books, LowerFr(q));
        std::printf("%-20s %s\n", b ? b->id.c_str() : "(aucun)", q);
    }
    if (!argv[1] || std::string(argv[1]) == "-") return 0;
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 6144; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    for (int qi : {0, 2, 3, 4}) {
        const Playbook* b = DetectPlaybook(books, LowerFr(qs[qi]));
        for (int with = 1; with < 2; ++with) {
            std::string sys = TechnicalPrompt("Omnis");
            if (with && b) sys += "\nMÉTHODE DE PRO (" + b->name + ") : pense comme un professionnel de ce domaine. Prends seulement les 2 ou 3 points de cette méthode qui répondent à SA question (pas toute la méthode), en 3 à 5 phrases, et finis par la prochaine chose à faire."
                                  " Parle comme si c'était ton expérience : ne dis jamais « la méthode », « étape 5 » ni un numéro de point.\n" + b->method;
            GenerateOptions o; o.maxTokens = 260; o.temperature = 0.2f;
            const auto r = brain.Generate({{"system", sys}, {"user", qs[qi]}}, o);
            std::printf("\n=== %s | %s\n%s\n", with ? "AVEC méthode" : "sans méthode", qs[qi], r ? r->c_str() : "(rien)");
            std::fflush(stdout);
        }
    }
}
