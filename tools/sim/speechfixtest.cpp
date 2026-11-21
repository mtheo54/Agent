// Autocorrecteur de transcription : corrige les erreurs connues, ne touche pas aux phrases normales.
#include "core/SpeechFix.hpp"
#include <chrono>
#include <cstdio>
using namespace companion;
int main() {
    SpeechFix fx;
    if (!fx.Load("assets/knowledge/vocab.json", "assets/knowledge/fr_frequents.txt", "/tmp/claude-0/sfuser.json")) return 1;
    std::printf("%zu clés phonétiques\n\n== Erreurs à corriger\n", fx.Size());
    const char* bad[][2] = {
        {"Quand je lui parle de fête studio, il comprend rien.", "musique"},
        {"Comment en fait déjà pour changer la sortie audio, la \"Azio\" ?", "musique"},
        {"Non, je veux voir ton vu métier.", "musique"},
        {"Arrête le mod FL, arrête le mod écoute.", "musique"},
        {"Mod d'entraînement.", "pc"},
        {"Mets un side chain sur la huit cent huit.", "musique"},
        {"J'ai oublié de remettre l'imiteur sur le master.", "musique"},
        {"Je joue Jinks et Tresh en bot lane.", "jeu"},
        {"Il a pris Kaisa et Yasso, c'est mort.", "jeu"},
        {"On va sur Assent ou sur Haiven ?", "jeu"},
        {"Mets du Serrum et un peu d'Otte sur le lead.", "musique"},
        {"Ouvre le piano rôle et le chanel rack.", "musique"},
        {"Omni, tu m'entends ?", "pc"},
        {"Je suis sur after effect, mets un wiggle.", "montage"},
    };
    for (auto& [t, d] : bad) {
        fx.SetDomains({d});
        std::vector<SpeechFix::Change> ch;
        std::printf("  %s\n   -> %s\n", t, fx.Fix(t, &ch).c_str());
    }
    std::printf("\n== Phrases normales (rien ne doit changer)\n");
    const char* good[] = {
        "Salut ! Tu me diras quand c'est fini ?", "Ouais, ouais, vas-y.", "Alors, t'en penses quoi pour le moment ?",
        "Est-ce que c'est mieux comme ça ?", "Comment tu sais ?", "On arrête tout pour le moment, faut que j'apprenne des trucs.",
        "Est-ce que tu peux faire des recherches pour t'entraîner ?", "Et bah le bus master indique qu'il atteint 0 dB dans les parties plus fortes.",
        "J'ai perdu à cause de mes mates, ils étaient nuls.", "Franchement la drill c'est mieux que la trap.", "Tu fais quoi ce soir ?",
        "Je vais manger, je reviens dans dix minutes.", "Elle voyait l'avenir non pas avec terreur, mais avec un dégoût bien rare à son âge.",
        "Julien jouit délicieusement de l'esprit et de la bravoure dont il fit preuve.", "Le jet d'eau était magnifique sur la rive.",
        "Il fait beau, on va au parc avec les enfants.", "Ma sœur est sage-femme à l'hôpital de la ville.", "J'ai vu un film d'horreur hier soir.",
        "Tu peux baisser le son un peu ?", "Mon fils joue au foot le mercredi.", "La reine a visité le château avec sa garde.",
        "Il y a trop de vent dehors, ferme la fenêtre.", "C'est quoi la différence entre un contacteur et un relais ?",
        "Je préfère le thé au café le matin.", "On se voit demain à la gare vers huit heures.",
        "J'ai fait une partie de fou hier soir, on a gagné en vingt minutes.", "Mon pote a acheté un nouveau casque pour jouer.",
        "Le boss final était trop dur, j'ai abandonné.", "Il faut que je range ma chambre avant que ma mère rentre.",
        "Tu as vu le match de foot ce week-end ?", "On a perdu parce que personne ne voulait défendre.",
        "J'ai composé une mélodie au piano cette nuit, elle est triste mais belle.", "La voisine fait encore du bruit avec sa perceuse.",
        "Je dois réviser mon examen de maths pour lundi.", "Ce restaurant fait les meilleures pizzas du quartier.",
        "Le chat a renversé mon verre sur le clavier.", "Il pleut depuis trois jours, j'en peux plus.",
        "Mon frère conduit trop vite sur l'autoroute.", "On part en vacances en Bretagne cet été.",
        "Elle chante dans une chorale le dimanche matin.",
    };
    int changed = 0;
    for (const char* t : good)
        for (const char* d : {"musique", "jeu", "montage", "pc"}) {
            fx.SetDomains({d});
            std::vector<SpeechFix::Change> ch;
            const std::string o = fx.Fix(t, &ch);
            if (!ch.empty()) { ++changed; std::printf("  [%s] %s\n   -> %s\n", d, t, o.c_str()); }
        }
    std::printf("  %d phrase(s) abîmée(s) sur %zu x 4 domaines\n", changed, std::size(good));
    {
        fx.SetDomains({"jeu"});
        const auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        for (int r = 0; r < 5; ++r)
            for (const char* t : good) { fx.Fix(t); ++n; }
        std::printf("  vitesse : %.2f ms par phrase\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n);
    }
    fx.SetDomains({"musique"});
    std::printf("\n== Consigne Whisper (musique)\n%s\n", fx.Prompt("Écoute ma prod sur FL.").c_str());
}
