#include "render/OverlayWindow.hpp"

#include "core/Log.hpp"

#include <algorithm>
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
    scale_ = static_cast<float>(GetDpiForSystem()) / 96.f;  // 150 % sur un écran 4K : personnage net
    pixelWidth_ = static_cast<int>(desc.width * scale_);
    pixelHeight_ = static_cast<int>(desc.height * scale_);
    const int x = work.right - pixelWidth_ - static_cast<int>(desc.marginX * scale_);
    const int y = work.bottom - pixelHeight_ - static_cast<int>(desc.marginY * scale_);

    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                          WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(exStyle, kWindowClass, L"Companion", WS_POPUP, x, y, pixelWidth_, pixelHeight_,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        log::Info("Overlay: CreateWindowEx a échoué ({})", GetLastError());
        return false;
    }
    // Opacité globale 100 % : le contenu DirectComposition porte la transparence par pixel.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

    if (!CreateDevice() || !CreateSwapChainAndComposition()) {
        Destroy();
        return false;
    }
    SetVisible(true);
    return true;
}

void OverlayWindow::Destroy() {
    if (context_) context_->ClearState();
    d2dContext_.Reset(); d2dDevice_.Reset(); d2dFactory_.Reset();
    dcompVisual_.Reset(); dcompTarget_.Reset(); dcompDevice_.Reset();
    swapChain_.Reset(); context_.Reset(); device_.Reset();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (instance_) { UnregisterClassW(kWindowClass, instance_); instance_ = nullptr; }
}

void OverlayWindow::SetLayout(float userScale, int corner) {
    if (!hwnd_ || !swapChain_) return;
    userScale_ = std::clamp(userScale, 0.5f, 2.f);
    corner_ = std::clamp(corner, 0, 3);
    pixelWidth_ = static_cast<int>(desc_.width * scale_ * userScale_);
    pixelHeight_ = static_cast<int>(desc_.height * scale_ * userScale_);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int mx = static_cast<int>(desc_.marginX * scale_), my = static_cast<int>(desc_.marginY * scale_);
    const int x = (corner_ % 2 == 0) ? work.left + mx : work.right - pixelWidth_ - mx;
    const int y = (corner_ < 2) ? work.top + my : work.bottom - pixelHeight_ - my;
    d2dContext_->SetTarget(nullptr);  // aucune référence au back buffer avant ResizeBuffers
    swapChain_->ResizeBuffers(2, static_cast<UINT>(pixelWidth_), static_cast<UINT>(pixelHeight_), kColorFormat, 0);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, pixelWidth_, pixelHeight_, SWP_NOACTIVATE);
    if (posX_ >= 0) PlaceFree();  // posé à la main : il y reste (nouvelle taille comprise)
    dcompDevice_->Commit();
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

    // Direct2D sur le même périphérique (dessin vectoriel anticrénelé du personnage).
    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))) ||
        FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_))) {
        log::Info("Overlay: Direct2D indisponible");
        return false;
    }
    d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
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
    sc.Width = static_cast<UINT>(pixelWidth_);
    sc.Height = static_cast<UINT>(pixelHeight_);
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

ID2D1DeviceContext* OverlayWindow::BeginFrame() {
    if (!swapChain_ || !d2dContext_) {  // recréation précédente ratée : on réessaie
        if (!RecoverDevice()) return nullptr;
    }
    // Flip model : GetBuffer(0) désigne toujours le back buffer courant.
    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return nullptr;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(kColorFormat, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) return nullptr;
    d2dContext_->SetTarget(target.Get());
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    return d2dContext_.Get();
}

bool OverlayWindow::EndFrame() {
    const HRESULT draw = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    // SyncInterval 0 : DWM compose de toute façon (pas de tearing) ; la cadence (24 ou 12 i/s)
    // est fixée par l'animation, pas par le taux de rafraîchissement du moniteur.
    const HRESULT present = swapChain_->Present(0, 0);
    // Carte perdue : sans ça, l'avatar restait invisible pour toujours alors qu'Omnis tournait encore.
    if (draw == static_cast<HRESULT>(D2DERR_RECREATE_TARGET) || present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
        log::Info("Overlay: carte graphique perdue (0x{:08X} / 0x{:08X}), recréation", static_cast<unsigned>(draw),
                  static_cast<unsigned>(present));
        if (!RecoverDevice()) log::Info("Overlay: recréation impossible pour l'instant, nouvel essai à l'image suivante");
        return false;
    }
    return true;
}

bool OverlayWindow::RecoverDevice() {
    if (context_) context_->ClearState();
    d2dContext_.Reset(); d2dDevice_.Reset(); d2dFactory_.Reset();
    dcompVisual_.Reset(); dcompTarget_.Reset(); dcompDevice_.Reset();
    swapChain_.Reset(); context_.Reset(); device_.Reset();
    // Taille courante : CreateSwapChainAndComposition utilise pixelWidth_/pixelHeight_ (déjà à la bonne taille).
    return CreateDevice() && CreateSwapChainAndComposition();
}

// ---------------------------------------------------------------------------
// Déplacement

void OverlayWindow::SetInteractive(bool on) {
    if (!hwnd_ || (on == interactive_ && !dragging_)) return;
    if (!on && dragging_) return;  // jamais lâché de force en pleine saisie
    interactive_ = on;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = on ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT)) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

bool OverlayWindow::TakeDropped(int& x, int& y) {
    if (!dropped_) return false;
    dropped_ = false;
    x = posX_;
    y = posY_;
    return true;
}

RECT OverlayWindow::ScreenRect() const {
    RECT r{};
    if (hwnd_) GetWindowRect(hwnd_, &r);
    return r;
}

void OverlayWindow::SetPosition(int x, int y) {
    posX_ = x;
    posY_ = y;
    if (x >= 0) PlaceFree();
    else SetLayout(userScale_, corner_);
}

// Position libre, gardée dans la zone de travail de l'écran où il se trouve (plusieurs écrans possibles).
void OverlayWindow::PlaceFree() {
    if (!hwnd_) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({posX_ + pixelWidth_ / 2, posY_ + pixelHeight_ / 2}, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT w = mi.rcWork;
    posX_ = std::clamp(posX_, static_cast<int>(w.left), static_cast<int>(w.right) - pixelWidth_);
    posY_ = std::clamp(posY_, static_cast<int>(w.top), static_cast<int>(w.bottom) - pixelHeight_);
    SetWindowPos(hwnd_, HWND_TOPMOST, posX_, posY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT OverlayWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return interactive_ || dragging_ ? HTCLIENT : HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // ne vole jamais le focus
        case WM_SETCURSOR:
            if (interactive_ || dragging_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));  // la main qui attrape, comme sur la Wii
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            POINT p;
            GetCursorPos(&p);
            RECT r{};
            GetWindowRect(hwnd_, &r);
            grabOffset_ = {p.x - r.left, p.y - r.top};
            grabLogical_ = {static_cast<float>(grabOffset_.x) / Scale(), static_cast<float>(grabOffset_.y) / Scale()};
            dragging_ = true;
            velX_ = 0.f;
            lastX_ = p.x;
            lastMoveTick_ = GetTickCount64();
            SetCapture(hwnd_);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_) {
                POINT p;
                GetCursorPos(&p);
                const ULONGLONG now = GetTickCount64();
                const float dt = std::max(1.f, static_cast<float>(now - lastMoveTick_)) / 1000.f;
                velX_ = 0.7f * velX_ + 0.3f * static_cast<float>(p.x - lastX_) / dt;
                lastX_ = p.x;
                lastMoveTick_ = now;
                posX_ = p.x - grabOffset_.x;
                posY_ = p.y - grabOffset_.y;
                SetWindowPos(hwnd_, HWND_TOPMOST, posX_, posY_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                if (msg == WM_LBUTTONUP) ReleaseCapture();
                PlaceFree();  // pas à moitié hors de l'écran
                dropped_ = true;
                return 0;
            }
            break;
        case WM_CLOSE: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<OverlayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->hwnd_) {
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->Handle(msg, wp, lp);
}

}  // namespace companion
