#pragma once
// ConfigWindow : fenêtre Windows (sans bordure, déplaçable par l'en-tête) qui héberge ConfigView.
// Ouverte par Ctrl+Alt+O ou « ouvre les réglages ». Tourne sur le thread principal.

#include "ui/ConfigView.hpp"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>

namespace companion {

class ConfigWindow {
public:
    using SaveFn = std::function<void(const CompanionConfig&)>;
    using StatusFn = std::function<ConfigStatus()>;

    ~ConfigWindow() { Close(); }

    bool Open(HINSTANCE instance, const CompanionConfig& cfg, const std::filesystem::path& fontsDir,
              const std::filesystem::path& rigJson, SaveFn onSave, StatusFn status);
    void Close();
    [[nodiscard]] bool IsOpen() const noexcept { return hwnd_ != nullptr; }
    void Tick(float dt);  // aperçu animé et état, à appeler depuis la boucle principale

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    bool CreateTarget();
    void Act(ConfigView::Action a);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    float scale_ = 1.f;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
    ConfigView view_;
    SaveFn onSave_;
    StatusFn status_;
    float statusClock_ = 0.f;
};

}  // namespace companion
