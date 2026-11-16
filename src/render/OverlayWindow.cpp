#include "render/OverlayWindow.hpp"

#include "core/Log.hpp"

#include <array>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {
constexpr wchar_t kWindowClass[] = L"CompanionAI.Overlay";
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
}  // namespace

bool OverlayWindow::Create(HINSTANCE instance, const Desc& desc) {
    desc_ = desc;
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OverlayWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.right - desc.width - desc.marginX;
    const int y = work.bottom - desc.height - desc.marginY;

    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                          WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(exStyle, kWindowClass, L"Companion", WS_POPUP, x, y, desc.width, desc.height,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        log::Info("Overlay: CreateWindowEx a échoué ({})", GetLastError());
        return false;
    }
    // Opacité globale 100 % : le contenu DirectComposition porte la transparence par pixel.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    if (!CreateDevice() || !CreateSwapChainAndComposition() || !CreateTargets()) {
        Destroy();
        return false;
    }
    SetVisible(true);
    return true;
}

void OverlayWindow::Destroy() {
    if (context_) context_->ClearState();
    dsv_.Reset(); depth_.Reset(); msaaRtv_.Reset(); msaaColor_.Reset();
    dcompVisual_.Reset(); dcompTarget_.Reset(); dcompDevice_.Reset();
    swapChain_.Reset(); context_.Reset(); device_.Reset();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (instance_) { UnregisterClassW(kWindowClass, instance_); instance_ = nullptr; }
}

void OverlayWindow::SetVisible(bool visible) {
    if (!hwnd_) return;
    visible_ = visible;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool OverlayWindow::CreateDevice() {
    const std::array levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(),
                                   static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device_, nullptr,
                                   &context_);
    if (FAILED(hr)) {
        log::Info("Overlay: D3D11CreateDevice a échoué (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    // Priorité GPU minimale : le jeu passe toujours devant l'avatar dans l'ordonnanceur.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice))) dxgiDevice->SetGPUThreadPriority(-7);
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(device_.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    return true;
}

bool OverlayWindow::CreateSwapChainAndComposition() {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice))) return false;
    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = static_cast<UINT>(desc_.width);
    sc.Height = static_cast<UINT>(desc_.height);
    sc.Format = kColorFormat;
    sc.SampleDesc = {1, 0};
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = factory->CreateSwapChainForComposition(device_.Get(), &sc, nullptr, &swapChain_);
    if (FAILED(hr)) {
        log::Info("Overlay: CreateSwapChainForComposition a échoué (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_)))) return false;
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_))) return false;
    if (FAILED(dcompVisual_->SetContent(swapChain_.Get()))) return false;
    if (FAILED(dcompTarget_->SetRoot(dcompVisual_.Get()))) return false;
    return SUCCEEDED(dcompDevice_->Commit());
}

bool OverlayWindow::CreateTargets() {
    // Cible MSAA 4x : bords de l'avatar lissés. La résolution MSAA de couleurs pré-multipliées
    // donne des bords semi-transparents corrects sur le bureau ou le jeu.
    UINT quality = 0;
    UINT samples = desc_.msaaSamples;
    while (samples > 1 && (FAILED(device_->CheckMultisampleQualityLevels(kColorFormat, samples, &quality)) || quality == 0))
        samples /= 2;
    desc_.msaaSamples = samples;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(desc_.width);
    td.Height = static_cast<UINT>(desc_.height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = kColorFormat;
    td.SampleDesc = {samples, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &msaaColor_))) return false;
    if (FAILED(device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, &msaaRtv_))) return false;

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &depth_))) return false;
    return SUCCEEDED(device_->CreateDepthStencilView(depth_.Get(), nullptr, &dsv_));
}

void OverlayWindow::BeginFrame() {
    constexpr float transparent[4] = {0.f, 0.f, 0.f, 0.f};
    ID3D11RenderTargetView* rtv = msaaRtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, dsv_.Get());
    context_->ClearRenderTargetView(msaaRtv_.Get(), transparent);
    context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(desc_.width), static_cast<float>(desc_.height), 0.f, 1.f};
    context_->RSSetViewports(1, &vp);
}

void OverlayWindow::EndFrame() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        context_->ResolveSubresource(backBuffer.Get(), 0, msaaColor_.Get(), 0, kColorFormat);
    // SyncInterval 0 : DWM compose de toute façon (pas de tearing) ; la cadence est
    // fixée par la boucle principale (30 fps / 15 fps en jeu), pas par le taux du moniteur.
    swapChain_->Present(0, 0);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;   // ceinture + bretelles avec WS_EX_TRANSPARENT
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_CLOSE: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace companion
