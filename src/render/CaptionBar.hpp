#pragma once
// CaptionBar : sous-titres sous le personnage (ce qu'Omnis est en train de dire), dans la même fenêtre transparente.
// Calés sur le son joué (VoiceEngine::CurrentCaption), restent 2,5 s après la fin puis s'estompent. Direct2D +
// DirectWrite ; ressources recréées si le contexte change (perte de la carte graphique).

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace companion {

class CaptionBar {
public:
    static constexpr float kHeight = 118.f;  // hauteur logique réservée sous le personnage

    // rect : zone logique (avant l'échelle) ; alpha 0 … 1.
    void Render(ID2D1DeviceContext* ctx, float scale, D2D1_RECT_F rect, const std::string& text, float alpha);
    // Vu-mètre (ce qu'il entend de FL) : barre de niveau court terme (vert -> jaune -> rouge), trait de crête, valeur en dB.
    void RenderMeter(ID2D1DeviceContext* ctx, float scale, D2D1_RECT_F rect, float rmsDb, float peakDb);
    void ReleaseDeviceResources() { ctx_ = nullptr; meterCtx_ = nullptr; }

private:
    const wchar_t* Family();
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg_, fg_, edge_;
    ID2D1DeviceContext* ctx_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mBg_, mGreen_, mYellow_, mRed_, mText_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mFormat_;
    ID2D1DeviceContext* meterCtx_ = nullptr;
    float shownDb_ = -60.f, holdPeak_ = -60.f;
};

}  // namespace companion
