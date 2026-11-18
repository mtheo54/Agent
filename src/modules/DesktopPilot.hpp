#pragma once
// DesktopPilot : Omnis prend ton clavier et ta souris, À TA DEMANDE, dans l'appli au premier plan.
//
//  - Clavier : SendInput (raccourcis, texte Unicode). Souris : clic au centre d'un bouton trouvé PAR SON NOM via
//    UI Automation (l'accessibilité de Windows) ; s'il sait « s'invoquer », on l'invoque sans même bouger la souris.
//  - AutoCAD / AutoCAD Electrical : commandes envoyées directement à AutoCAD (COM, ActiveDocument.SendCommand),
//    sinon tapées dans sa ligne de commande.
//  - Sécurité :
//      * tu bouges la souris ou touches une touche (entrée PHYSIQUE, distinguée de la sienne par Raw Input) :
//        il s'arrête net et te rend la main ;
//      * il n'agit que dans la fenêtre où tu étais quand tu as demandé : si elle n'est plus au premier plan,
//        il s'arrête au lieu de taper ailleurs ;
//      * jamais pendant un jeu (l'injection de clavier/souris est ce que traquent les anti-triche) ;
//      * Windows interdit de piloter une appli lancée en administrateur depuis une appli qui ne l'est pas.

#include "modules/PilotPlan.hpp"

#include <windows.h>

#include <atomic>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace companion {

enum class PilotApp : std::uint8_t { Other, AutoCad, FlStudio };

struct PilotTarget {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exe;    // minuscules, ex. « acad.exe »
    std::string title;   // titre de la fenêtre (UTF-8)
    PilotApp app = PilotApp::Other;
};

class DesktopPilot {
public:
    enum class Result : std::uint8_t { Done, Aborted, Failed };

    static PilotTarget Foreground();
    // Boutons, menus, onglets visibles de la fenêtre (noms UTF-8, sans doublons) : ce que le modèle peut cliquer.
    static std::vector<std::string> ClickableNames(HWND hwnd, std::size_t max = 60);

    // Exécute le plan. say() annonce les lignes DIRE (non bloquant). error reçoit la raison d'un échec / arrêt.
    Result Run(const PilotTarget& target, const std::vector<PilotStep>& steps, const std::function<void(const std::string&)>& say,
               std::stop_token stop, std::string& error);

private:
    bool Ensure(const PilotTarget& t, std::string& error);  // la bonne fenêtre est-elle (encore) au premier plan ?
    static bool SendChord(const KeyChord& k);
    static bool TypeText(const std::string& utf8);
    static bool ClickByName(HWND hwnd, const std::string& name, std::string& error);
    static bool AcadCommand(const std::string& command, std::string& error);

    // Entrée physique pendant l'exécution -> arrêt (fenêtre « message-only » + Raw Input, seulement pendant Run).
    void WatchMain(std::stop_token st);
    std::atomic<bool> userTookOver_{false};
    std::atomic<HWND> watchWnd_{nullptr};
    static LRESULT CALLBACK WatchProc(HWND, UINT, WPARAM, LPARAM);
};

}  // namespace companion
