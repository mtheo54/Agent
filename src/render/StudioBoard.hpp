#pragma once
// StudioBoard : le tableau qu'Omnis sort de derrière lui quand il écoute ta prod. Un vu-mètre vertical gradué en dB
// (vert, jaune, rouge), un trait de crête tenu, la valeur en dB et un voyant qui s'allume quand il entend vraiment du son :
// tu vois tout de suite s'il écoute bien. Dessiné AVANT le personnage : il sort de derrière lui en glissant.

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace companion {

class StudioBoard {
public:
    // out : 0 = rangé derrière lui, 1 = sorti (animé par l'appelant) ; niveaux en dBFS.
    void Render(ID2D1DeviceContext* ctx, float scale, float out, float rmsDb, float peakDb);
    void ReleaseDeviceResources() { ctx_ = nullptr; }
    // Avance l'animation vers « sorti » ou « rangé » ; retourne la position 0..1 (sortie avec un petit rebond).
    float Step(bool wanted, float dt);
    [[nodiscard]] bool Hidden() const noexcept { return t_ <= 0.f; }

private:
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_, title_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> wood_, woodDark_, panel_, green_, yellow_, red_, text_, dim_, off_;
    ID2D1DeviceContext* ctx_ = nullptr;
    float t_ = 0.f, shownDb_ = -60.f, holdPeak_ = -60.f;
};

}  // namespace companion
