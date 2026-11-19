#pragma once
// OverlayWindow : fenêtre Win32 transparente, toujours au premier plan, click-through,
// dessinée en Direct2D dans une swapchain DirectComposition (alpha pré-multiplié).
// Le périphérique D3D11 ne sert que de support à DXGI / DirectComposition / Direct2D.
//
// Pourquoi DirectComposition : DXGI_ALPHA_MODE_PREMULTIPLIED n'est PAS supporté par
// CreateSwapChainForHwnd (seulement par CreateSwapChainForComposition). La combinaison
// "WS_EX_LAYERED + swapchain HWND" du cahier des charges donnerait un fond noir opaque.
//
// Styles : WS_EX_NOREDIRECTIONBITMAP (pas de surface GDI) | WS_EX_LAYERED + WS_EX_TRANSPARENT
// (les clics traversent vers le jeu) | WS_EX_TOPMOST | WS_EX_TOOLWINDOW (absent d'Alt+Tab)
// | WS_EX_NOACTIVATE (ne vole jamais le focus du jeu).
//
// Limite : visible au-dessus des jeux en "plein écran fenêtré"/borderless (le défaut de la
// plupart des jeux DX12 sous Windows 11), pas au-dessus d'un plein écran exclusif.

#include <windows.h>

#include <algorithm>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace companion {

class OverlayWindow {
public:
    struct Desc {
        int width = 320;    // taille logique (à 100 %) ; multipliée par l'échelle DPI de l'écran
        int height = 420;
        int marginX = 24;   // distance au bord droit de la zone de travail
        int marginY = 24;   // distance au bord bas
    };

    OverlayWindow() = default;
    ~OverlayWindow() { Destroy(); }
    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(HINSTANCE instance, const Desc& desc);
    void Destroy();

    void SetVisible(bool visible);
    // Taille (facteur utilisateur) et coin de l'écran : 0 haut-gauche, 1 haut-droite, 2 bas-gauche, 3 bas-droite.
    void SetLayout(float userScale, int corner);
    [[nodiscard]] bool IsVisible() const noexcept { return visible_; }

    // Cible Direct2D = back buffer, effacée en transparent. Retourne le contexte de dessin.
    ID2D1DeviceContext* BeginFrame();
    // Termine le dessin et présente (sans attente VSync bloquante). Faux si la carte a été perdue (pilote
    // réinitialisé par Windows, mise à jour du pilote…) : tout est recréé, l'appelant libère ses ressources Direct2D.
    bool EndFrame();

    [[nodiscard]] HWND Hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] ID3D11Device* Device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID2D1Factory1* D2DFactory() const noexcept { return d2dFactory_.Get(); }
    [[nodiscard]] float Scale() const noexcept { return scale_ * userScale_; }  // DPI / 96 × taille choisie

    // --- Déplacement à la souris -----------------------------------------------------------------
    // La fenêtre reste « transparente aux clics » (le jeu, le bureau les reçoivent) SAUF quand l'appelant la rend
    // interactive : curseur sur le personnage, hors jeu. Alors on peut l'attraper et le poser n'importe où.
    void SetInteractive(bool on);
    [[nodiscard]] bool Dragging() const noexcept { return dragging_; }
    // Pixels écran / s, lissée ; retombe à 0 quand la souris s'arrête (sinon il resterait penché).
    [[nodiscard]] float DragVelocityX() const noexcept {
        const float idle = static_cast<float>(GetTickCount64() - lastMoveTick_);
        return idle > 60.f ? velX_ * std::max(0.f, 1.f - (idle - 60.f) / 120.f) : velX_;
    }
    [[nodiscard]] D2D1_POINT_2F GrabPoint() const noexcept { return grabLogical_; }  // point saisi (coordonnées logiques)
    bool TakeDropped(int& x, int& y);  // vrai une fois après un lâcher : coin haut-gauche (pixels écran)
    void SetPosition(int x, int y);    // position libre (pixels écran) ; x < 0 = retour au coin
    [[nodiscard]] RECT ScreenRect() const;

private:
    bool CreateDevice();
    bool CreateSwapChainAndComposition();
    bool RecoverDevice();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void PlaceFree();

    bool interactive_ = false, dragging_ = false, dropped_ = false;
    POINT grabOffset_{};               // curseur - coin de la fenêtre au moment de la saisie
    D2D1_POINT_2F grabLogical_{160.f, 60.f};
    float velX_ = 0.f;
    ULONGLONG lastMoveTick_ = 0;
    int lastX_ = 0;
    int posX_ = -1, posY_ = -1;        // position libre (pixels écran), -1 = coin

    Desc desc_{};
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool visible_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    float scale_ = 1.f, userScale_ = 1.f;
    int corner_ = 3;
    int pixelWidth_ = 0, pixelHeight_ = 0;
};

}  // namespace companion
