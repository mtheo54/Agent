// Choix du mode (technique / rage bait) sur des phrases typiques.
#include "core/OmnisModes.hpp"
#include <cstdio>
using namespace companion;
int main() {
    for (const char* s : {"Mon module Murrelektronik a la LED BF rouge en Profinet, je fais quoi ?",
                          "Comment je fais un sidechain sur FL Studio ?", "Mon plugin VST crashe à l'ouverture, segfault dans processBlock",
                          "Le disjoncteur différentiel saute dès que je branche le moteur", "Quel est le meilleur matchup pour Ahri contre Zed ?",
                          "J'ai perdu à cause de mes mates, ils étaient nuls", "Franchement je suis le meilleur Jett de France",
                          "Je pense que la drill c'est mieux que la trap", "Salut ça va", "J'ai perdu à cause du lag mais mon build était bon",
                          "C'est quoi la différence entre un contacteur et un relais ?", "Tu fais quoi ce soir", "avoue que t'es nul",
                          "T'as mangé quoi toi ?", "Comment ça ?", "Mais qu'est-ce que tu me racontes ?", "Ok, venez-y !",
                          "Pourquoi ma voix sature quand je chante fort ?", "C'est quoi un compresseur ?", "Omnis t'es là ? Tu m'entends ?",
                          "Combien de RAM il faut pour Ableton ?"}) {
        std::string l = s; for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::printf("%-12s %s\n", DetectChatMode(l) == ChatMode::Technical ? "TECHNIQUE" : "RAGE BAIT", s);
    }
}
