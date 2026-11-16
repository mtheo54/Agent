#pragma once
// OverlayWindow : fenêtre Win32 transparente, toujours au premier plan, click-through,
// rendue en D3D11 via une swapchain DirectComposition (alpha pré-multiplié).
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
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace companion {

class OverlayWindow {
public:
    struct Desc {
        int width = 320;
        int height = 420;
        int marginX = 24;   // distance au bord droit de la zone de travail
        int marginY = 24;   // distance au bord bas
        UINT msaaSamples = 4;
    };

    OverlayWindow() = default;
    ~OverlayWindow() { Destroy(); }
    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(HINSTANCE instance, const Desc& desc);
    void Destroy();

    void SetVisible(bool visible);
    [[nodiscard]] bool IsVisible() const noexcept { return visible_; }

    // Lie la cible MSAA + profondeur et l'efface en transparent (0,0,0,0).
    void BeginFrame();
    // Résout le MSAA dans le back buffer et présente (sans attente VSync bloquante).
    void EndFrame();

    [[nodiscard]] HWND Hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] ID3D11Device* Device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return context_.Get(); }
    [[nodiscard]] int Width() const noexcept { return desc_.width; }
    [[nodiscard]] int Height() const noexcept { return desc_.height; }

private:
    bool CreateDevice();
    bool CreateSwapChainAndComposition();
    bool CreateTargets();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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

    Microsoft::WRL::ComPtr<ID3D11Texture2D> msaaColor_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> msaaRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv_;
};

}  // namespace companion
