#include "render/StudioBoard.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace companion {

float StudioBoard::Step(bool wanted, float dt) {
    t_ = std::clamp(t_ + (wanted ? dt : -dt) / 0.45f, 0.f, 1.f);  // 0,45 s pour sortir ou ranger
    // Sortie « easeOutBack » : il dépasse un peu puis se cale, comme un tableau qu'on sort d'un geste.
    const float x = t_ - 1.f;
    return 1.f + 2.4f * x * x * x + 1.4f * x * x;
}

void StudioBoard::Render(ID2D1DeviceContext* ctx, float scale, float out, float rmsDb, float peakDb) {
    if (!ctx || t_ <= 0.f) return;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return;
    if (!small_) {
        const wchar_t* family = L"Segoe UI";
        Microsoft::WRL::ComPtr<IDWriteFontCollection> sys;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(&sys)))
            for (const wchar_t* f : {L"Segoe UI", L"Arial", L"Tahoma", L"Verdana", L"DejaVu Sans"}) {
                UINT32 i = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(sys->FindFamilyName(f, &i, &exists)) && exists) { family = f; break; }
            }
        dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  9.f, L"fr-fr", &small_);
        dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  11.f, L"fr-fr", &title_);
        if (!small_ || !title_) return;
        title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (ctx_ != ctx) {
        const auto make = [&](Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& b, float r, float g, float bl, float a = 1.f) {
            b.Reset();
            ctx->CreateSolidColorBrush(D2D1::ColorF(r, g, bl, a), &b);
        };
        make(wood_, 0.62f, 0.42f, 0.24f);
        make(woodDark_, 0.42f, 0.27f, 0.14f);
        make(panel_, 0.07f, 0.09f, 0.11f, 0.96f);
        make(green_, 0.3f, 0.95f, 0.5f);
        make(yellow_, 1.f, 0.8f, 0.2f);
        make(red_, 1.f, 0.25f, 0.25f);
        make(text_, 1.f, 1.f, 1.f, 0.92f);
        make(dim_, 1.f, 1.f, 1.f, 0.35f);
        make(off_, 0.2f, 0.25f, 0.22f);
        ctx_ = ctx;
    }
    if (!wood_ || !panel_ || !green_ || !text_) return;

    // Niveaux lissés comme un vrai vu-mètre : monte vite, redescend doucement ; crête tenue puis relâchée.
    rmsDb = std::clamp(rmsDb, -60.f, 0.f);
    peakDb = std::clamp(peakDb, -60.f, 3.f);
    shownDb_ = rmsDb > shownDb_ ? rmsDb : shownDb_ + (rmsDb - shownDb_) * 0.12f;
    holdPeak_ = peakDb > holdPeak_ ? peakDb : holdPeak_ - 0.35f;
    const bool hearing = shownDb_ > -45.f;

    // Rangé : derrière son corps (centre du canevas), petit ; sorti : à sa droite, tenu à hauteur de poitrine.
    const float cx = 160.f + (268.f - 160.f) * out, cy = 238.f, s = 0.55f + 0.45f * std::min(out, 1.f);
    const float w = 96.f, h = 150.f;
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cx, cy}) * D2D1::Matrix3x2F::Rotation(-4.f * out, {cx, cy + h / 2.f}) *
                      D2D1::Matrix3x2F::Scale(scale, scale));
    const D2D1_RECT_F frame{cx - w / 2.f, cy - h / 2.f, cx + w / 2.f, cy + h / 2.f};
    // Manche (il le tient par en dessous), cadre en bois, panneau.
    ctx->FillRectangle({cx - 4.f, frame.bottom - 2.f, cx + 4.f, frame.bottom + 34.f}, woodDark_.Get());
    ctx->FillRoundedRectangle({frame, 8.f, 8.f}, wood_.Get());
    ctx->DrawRoundedRectangle({frame, 8.f, 8.f}, woodDark_.Get(), 2.f);
    const D2D1_RECT_F p{frame.left + 7.f, frame.top + 7.f, frame.right - 7.f, frame.bottom - 7.f};
    ctx->FillRoundedRectangle({p, 5.f, 5.f}, panel_.Get());
    ctx->DrawText(L"J'ÉCOUTE", 8, title_.Get(), {p.left, p.top + 3.f, p.right, p.top + 18.f}, text_.Get());
    // Voyant : allumé quand il entend vraiment du son.
    ctx->FillEllipse({{p.right - 9.f, p.top + 10.f}, 3.5f, 3.5f}, hearing ? green_.Get() : off_.Get());

    // Vu-mètre vertical : 0 dB en haut, -60 en bas ; vert jusqu'à -18, jaune jusqu'à -6, rouge au-dessus.
    const float top = p.top + 24.f, bottom = p.bottom - 20.f, bx0 = p.left + 22.f, bx1 = bx0 + 16.f;
    const auto y = [&](float db) { return bottom - (bottom - top) * (std::clamp(db, -60.f, 0.f) + 60.f) / 60.f; };
    ctx->FillRectangle({bx0, top, bx1, bottom}, off_.Get());
    const float v = y(shownDb_);
    ctx->FillRectangle({bx0, std::max(v, y(-18.f)), bx1, bottom}, green_.Get());
    if (v < y(-18.f)) ctx->FillRectangle({bx0, std::max(v, y(-6.f)), bx1, y(-18.f)}, yellow_.Get());
    if (v < y(-6.f)) ctx->FillRectangle({bx0, v, bx1, y(-6.f)}, red_.Get());
    const float pk = y(holdPeak_);
    ctx->FillRectangle({bx0 - 2.f, pk - 1.f, bx1 + 2.f, pk + 1.f}, holdPeak_ > -1.f ? red_.Get() : text_.Get());
    // Graduations.
    for (const float db : {0.f, -6.f, -12.f, -18.f, -24.f, -36.f, -48.f}) {
        wchar_t b[8];
        const int n = std::swprintf(b, 8, L"%.0f", db);
        ctx->FillRectangle({bx1 + 2.f, y(db) - 0.5f, bx1 + 6.f, y(db) + 0.5f}, dim_.Get());
        ctx->DrawText(b, static_cast<UINT32>(std::max(n, 0)), small_.Get(), {bx1 + 8.f, y(db) - 6.f, p.right, y(db) + 6.f}, dim_.Get());
    }
    // Valeur de crête en bas.
    wchar_t val[16];
    const int n = std::swprintf(val, 16, L"%.1f dB", holdPeak_);
    ctx->DrawText(val, static_cast<UINT32>(std::max(n, 0)), title_.Get(), {p.left, p.bottom - 17.f, p.right, p.bottom - 2.f},
                  holdPeak_ > -1.f ? red_.Get() : text_.Get());
    ctx->SetTransform(D2D1::IdentityMatrix());
}

}  // namespace companion
