// Fiche technique tirée d'un tutoriel (mêmes consignes que l'appli). modèle.gguf cache
#include "core/LocalBrain.hpp"
#include "modules/VideoParse.hpp"
#include <cstdio>
using namespace companion;
int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    const char* tuto = "Salut tout le monde, aujourd'hui on va voir comment faire un sidechain sur le 808 dans FL Studio. Alors déjà vous "
        "ouvrez le mixer avec F9. Vous mettez votre kick sur la piste 1 et votre 808 sur la piste 2. Vous sélectionnez la piste du kick, "
        "et sur la piste du 808 vous faites clic droit sur la petite flèche en bas et vous choisissez sidechain to this track. Ensuite sur "
        "la piste du 808 vous mettez un Fruity Limiter, vous allez dans l'onglet COMP, et en bas vous avez le bouton sidechain, vous le "
        "mettez sur 1, la piste du kick. Là vous baissez le threshold, moi je mets à peu près moins 20 dB, le ratio je le monte vers 6, et "
        "la release je la mets assez courte, genre 80 millisecondes, sinon le 808 il revient trop tard. Attention, ne mettez pas l'attaque "
        "trop lente sinon le kick il claque pas. Et voilà, abonnez-vous.";
    const char* vlog = "Wesh la team, aujourd'hui je vous montre ma nouvelle chambre, j'ai acheté des LED, regardez c'est trop beau. Bon "
        "après on va manger un kebab avec les gars, c'était incroyable. Laissez un like si vous voulez la partie deux.";
    for (auto [title, t] : {std::pair{"Sidechain 808 FL Studio en 2 minutes", tuto}, std::pair{"MA NOUVELLE CHAMBRE (vlog)", vlog}}) {
        GenerateOptions o; o.maxTokens = 450; o.temperature = 0.2f;
        const auto out = brain.Generate({{"system", kTutorialSystem}, {"user", TutorialPrompt(title, t)}}, o);
        std::printf("=== %s\n%s\n\n", title, out ? out->c_str() : "(rien)");
    }
}
