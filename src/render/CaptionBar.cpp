#include "render/CaptionBar.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cwchar>

namespace companion {

// Première police présente (Segoe UI sur tout Windows ; repli si absente, sinon le texte serait vide).
const wchar_t* CaptionBar::Family() {
    Microsoft::WRL::ComPtr<IDWriteFontCollection> sys;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&sys)))
        for (const wchar_t* f : {L"Segoe UI", L"Arial", L"Tahoma", L"Verdana", L"DejaVu Sans"}) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(f, &index, &exists)) && exists) return f;
        }
    return L"Segoe UI";
}

void CaptionBar::Render(ID2D1DeviceContext* ctx, float scale, D2D1_RECT_F rect, const std::string& text, float alpha) {
    if (!ctx || text.empty() || alpha <= 0.01f) return;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return;
    if (!format_) {
        // Première police présente (Segoe UI sur tout Windows ; repli si absente, sinon le texte serait vide).
        if (FAILED(dwrite_->CreateTextFormat(Family(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, 15.f, L"fr-fr", &format_)))
            return;
        format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    if (ctx_ != ctx) {
        bg_.Reset(); fg_.Reset(); edge_.Reset();
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.04f, 0.06f, 0.78f), &bg_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 1.f), &fg_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.35f, 1.f, 0.6f, 0.55f), &edge_);
        ctx_ = ctx;
    }
    if (!bg_ || !fg_ || !edge_) return;

    const std::wstring w = log::Widen(text);
    const float pad = 10.f, maxW = rect.right - rect.left - 2 * pad, maxH = rect.bottom - rect.top - 2 * pad;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), format_.Get(), maxW, 1000.f, &layout))) return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float textH = std::min(m.height, maxH);
    // Bulle ajustée au texte, collée en haut de la zone (juste sous les pieds du personnage).
    const float boxW = std::min(maxW, m.widthIncludingTrailingWhitespace) + 2 * pad;
    const float cx = (rect.left + rect.right) / 2.f;
    const D2D1_RECT_F box{cx - boxW / 2.f, rect.top, cx + boxW / 2.f, rect.top + textH + 2 * pad};

    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    bg_->SetOpacity(alpha);
    fg_->SetOpacity(alpha);
    edge_->SetOpacity(alpha);
    const D2D1_ROUNDED_RECT rr{box, 9.f, 9.f};
    ctx->FillRoundedRectangle(rr, bg_.Get());
    ctx->DrawRoundedRectangle(rr, edge_.Get(), 1.f);
    // Trop long pour la bulle : on montre la fin (ce qu'il dit maintenant), le début est coupé proprement.
    ctx->PushAxisAlignedClip({box.left, box.top + pad - 1, box.right, box.bottom - pad + 1}, D2D1_ANTIALIAS_MODE_ALIASED);
    const float shift = m.height > maxH ? m.height - maxH : 0.f;
    ctx->DrawTextLayout({cx - maxW / 2.f, box.top + pad - shift}, layout.Get(), fg_.Get());
    ctx->PopAxisAlignedClip();
    ctx->SetTransform(D2D1::IdentityMatrix());
}

void CaptionBar::RenderMeter(ID2D1DeviceContext* ctx, float scale, D2D1_RECT_F rect, float rmsDb, float peakDb) {
    if (!ctx) return;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return;
    if (!mFormat_ && FAILED(dwrite_->CreateTextFormat(Family(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL, 11.f, L"fr-fr", &mFormat_)))
        return;
    if (meterCtx_ != ctx) {
        mBg_.Reset(); mGreen_.Reset(); mYellow_.Reset(); mRed_.Reset(); mText_.Reset();
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.04f, 0.06f, 0.82f), &mBg_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.95f, 0.5f, 1.f), &mGreen_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.8f, 0.2f, 1.f), &mYellow_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.25f, 0.25f, 1.f), &mRed_);
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.9f), &mText_);
        meterCtx_ = ctx;
    }
    if (!mBg_ || !mGreen_ || !mYellow_ || !mRed_ || !mText_) return;
    // Lissage comme un vrai vu-mètre : monte vite, redescend doucement ; crête tenue puis relâchée.
    rmsDb = std::clamp(rmsDb, -60.f, 0.f);
    peakDb = std::clamp(peakDb, -60.f, 3.f);
    shownDb_ = rmsDb > shownDb_ ? rmsDb : shownDb_ + (rmsDb - shownDb_) * 0.15f;
    holdPeak_ = peakDb > holdPeak_ ? peakDb : holdPeak_ - 0.4f;
    const auto x = [&](float db) { return rect.left + 34.f + (rect.right - rect.left - 82.f) * (std::clamp(db, -60.f, 0.f) + 60.f) / 60.f; };
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    ctx->FillRoundedRectangle({rect, 7.f, 7.f}, mBg_.Get());
    const float y0 = rect.top + 7.f, y1 = rect.bottom - 7.f;
    ctx->DrawText(L"FL", 2, mFormat_.Get(), {rect.left + 8.f, rect.top + 3.f, rect.left + 32.f, rect.bottom}, mText_.Get());
    // Zones : vert jusqu'à -18 dB, jaune jusqu'à -6, rouge au-delà.
    const float v = x(shownDb_);
    ctx->FillRectangle({x(-60.f), y0, std::min(v, x(-18.f)), y1}, mGreen_.Get());
    if (v > x(-18.f)) ctx->FillRectangle({x(-18.f), y0, std::min(v, x(-6.f)), y1}, mYellow_.Get());
    if (v > x(-6.f)) ctx->FillRectangle({x(-6.f), y0, v, y1}, mRed_.Get());
    const float p = x(holdPeak_);
    ctx->FillRectangle({p - 1.f, y0 - 2.f, p + 1.f, y1 + 2.f}, holdPeak_ > -1.f ? mRed_.Get() : mText_.Get());
    wchar_t buf[24];
    const int n = std::swprintf(buf, 24, L"%.0f dB", holdPeak_);
    ctx->DrawText(buf, static_cast<UINT32>(std::max(n, 0)), mFormat_.Get(), {rect.right - 46.f, rect.top + 3.f, rect.right - 4.f, rect.bottom},
                  holdPeak_ > -1.f ? mRed_.Get() : mText_.Get());
    ctx->SetTransform(D2D1::IdentityMatrix());
}

}  // namespace companion
