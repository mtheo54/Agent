#include "ui/ConfigWindow.hpp"

#include "core/Log.hpp"

#include <windowsx.h>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {
constexpr wchar_t kClass[] = L"CompanionAI.Settings";
}

bool ConfigWindow::Open(HINSTANCE instance, const CompanionConfig& cfg, const std::filesystem::path& fontsDir,
                        const std::filesystem::path& rigJson, SaveFn onSave, StatusFn status) {
    if (hwnd_) {  // déjà ouverte : au premier plan
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        return true;
    }
    instance_ = instance;
    onSave_ = std::move(onSave);
    status_ = std::move(status);
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                              reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    view_ = ConfigView{};
    view_.Init(factory_.Get(), dwrite_.Get(), fontsDir, rigJson);
    view_.SetConfig(cfg);
    if (status_) view_.SetStatus(status_());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ConfigWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    scale_ = static_cast<float>(GetDpiForSystem()) / 96.f;
    const int w = static_cast<int>(ConfigView::kWidth * scale_), h = static_cast<int>(ConfigView::kHeight * scale_);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"abrasion · compagnon", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                            (work.left + work.right - w) / 2, (work.top + work.bottom - h) / 2, w, h, nullptr, nullptr,
                            instance, this);
    if (!hwnd_ || !CreateTarget()) {
        Close();
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    return true;
}

bool ConfigWindow::CreateTarget() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    target_.Reset();
    ctx_.Reset();
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(factory_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))),
            &target_)))
        return false;
    target_->SetDpi(96.f, 96.f);  // on gère l'échelle nous-mêmes (scale_)
    return SUCCEEDED(target_.As(&ctx_));
}

void ConfigWindow::Close() {
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
    ctx_.Reset();
    target_.Reset();
}

void ConfigWindow::Tick(float dt) {
    if (!hwnd_) return;
    statusClock_ += dt;
    if (status_ && statusClock_ > 0.5f) {  // état (émotion, voix apprise…) rafraîchi deux fois par seconde
        statusClock_ = 0.f;
        view_.SetStatus(status_());
    }
    if (view_.Tick(dt)) Paint();
}

void ConfigWindow::Paint() {
    if (!ctx_) return;
    target_->BeginDraw();
    view_.Render(ctx_.Get(), scale_);
    if (target_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) CreateTarget();
}

void ConfigWindow::Act(ConfigView::Action a) {
    if (a == ConfigView::Action::Save) {
        if (onSave_) onSave_(view_.Config());
        Close();
    } else if (a == ConfigView::Action::Cancel) {
        Close();
    }
}

LRESULT CALLBACK ConfigWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<ConfigWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ConfigWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    const auto lx = [&](LPARAM l) { return static_cast<float>(GET_X_LPARAM(l)) / scale_; };
    const auto ly = [&](LPARAM l) { return static_cast<float>(GET_Y_LPARAM(l)) / scale_; };
    switch (msg) {
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &p);
            return view_.InCaption(static_cast<float>(p.x) / scale_, static_cast<float>(p.y) / scale_) ? HTCAPTION : HTCLIENT;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd_);
            Act(view_.MouseDown(lx(lp), ly(lp)));
            if (hwnd_) Paint();
            return 0;
        case WM_MOUSEMOVE:
            view_.MouseMove(lx(lp), ly(lp));
            Paint();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            view_.MouseUp();
            return 0;
        case WM_KEYDOWN:
            Act(view_.Key(static_cast<unsigned>(wp)));
            if (hwnd_) Paint();
            return 0;
        case WM_CHAR:
            view_.Char(static_cast<wchar_t>(wp));
            Paint();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            Close();
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

}  // namespace companion
