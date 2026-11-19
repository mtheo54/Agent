#include "ui/ConfigView.hpp"

#include "core/Log.hpp"

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <format>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

// Palette du site abrasion.
D2D1_COLOR_F Color(const char* name) {
    struct Entry { const char* n; unsigned rgb; };
    static constexpr Entry kColors[] = {
        {"bg", 0x000000}, {"text", 0xF2F2F2}, {"soft", 0xC9CDD8}, {"dim", 0x8C8F99}, {"muted", 0x55585F},
        {"line", 0x3D3D3D}, {"dash", 0x6E7180}, {"amber", 0xFFD27A}, {"green", 0x9FE3B0}, {"white", 0xFFFFFF},
        {"title", 0xA9ADB8}, {"black", 0x05060A}, {"panel", 0x0B0C10},
    };
    for (const auto& e : kColors)
        if (std::string_view(e.n) == name)
            return D2D1::ColorF(((e.rgb >> 16) & 255) / 255.f, ((e.rgb >> 8) & 255) / 255.f, (e.rgb & 255) / 255.f, 1.f);
    return D2D1::ColorF(1, 0, 1, 1);
}

std::wstring W(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

// Rendu de texte en lettres creuses (contours des glyphes) : signature du titre « signal en pause ».
class OutlineRenderer final : public IDWriteTextRenderer {
public:
    OutlineRenderer(ID2D1Factory1* f, ID2D1DeviceContext* ctx, ID2D1Brush* brush, float stroke)
        : f_(f), ctx_(ctx), brush_(brush), stroke_(stroke) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }   // objet sur la pile
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override { *p = 1.f; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(f_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return E_FAIL;
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, run->bidiLevel % 2, sink.Get());
        sink->Close();
        ComPtr<ID2D1TransformedGeometry> placed;
        f_->CreateTransformedGeometry(path.Get(), D2D1::Matrix3x2F::Translation(x, y), &placed);
        if (placed) ctx_->DrawGeometry(placed.Get(), brush_, stroke_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
    ID2D1Factory1* f_;
    ID2D1DeviceContext* ctx_;
    ID2D1Brush* brush_;
    float stroke_;
};

const wchar_t* kExpressions[][2] = {{L"joie", L"happy"},     {L"rire", L"laugh"},  {L"surprise", L"surprised"},
                                    {L"oui", L"nod"},        {L"non", L"shake"},   {L"bof", L"shrug"},
                                    {L"bravo", L"clap"},     {L"coucou", L"wave"}, {L"danse", L"dance"}};

}  // namespace

// ---------------------------------------------------------------------------
bool ConfigView::Init(ID2D1Factory1* factory, IDWriteFactory* dwrite, const std::filesystem::path& fontsDir,
                      const std::filesystem::path& rigJson) {
    factory_ = factory;
    dwrite_ = dwrite;

    // Polices du site, chargées depuis assets/fonts (Windows 10 1709+). Sinon : Consolas / Segoe UI.
    ComPtr<IDWriteFactory5> f5;
    if (dwrite && SUCCEEDED(dwrite->QueryInterface(IID_PPV_ARGS(&f5)))) {
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
            int added = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(fontsDir, ec)) {
                if (e.path().extension() != L".ttf") continue;
                ComPtr<IDWriteFontFile> file;
                if (SUCCEEDED(f5->CreateFontFileReference(e.path().c_str(), nullptr, &file)) &&
                    SUCCEEDED(builder->AddFontFile(file.Get())))
                    ++added;
            }
            ComPtr<IDWriteFontSet> set;
            ComPtr<IDWriteFontCollection1> collection;
            if (added > 0 && SUCCEEDED(builder->CreateFontSet(&set)) &&
                SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), &collection)))
                fonts_ = collection;
        }
    }
    if (!fonts_) {
        mono_ = L"Consolas";
        sans_ = L"Segoe UI";
        log::Info("Réglages : polices du site indisponibles, polices système utilisées");
    }

    previewOk_ = preview_.Load(rigJson);
    preview_.SetState(AvatarState::Idle);

    // Champ d'étoiles fixe (graine constante : même ciel à chaque ouverture).
    unsigned seed = 0xA8F1u;
    for (int i = 0; i < 240; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float x = static_cast<float>((seed >> 8) % 10400) / 10.f;
        seed = seed * 1664525u + 1013904223u;
        const float y = static_cast<float>((seed >> 8) % 7600) / 10.f;
        seed = seed * 1664525u + 1013904223u;
        stars_.push_back({x, y});
        starAlpha_.push_back(0.12f + static_cast<float>((seed >> 8) % 100) / 220.f);
    }
    return true;
}

void ConfigView::SetConfig(const CompanionConfig& cfg) {
    cfg_ = cfg;
    preview_.SetOpacity(cfg_.avatarOpacity);
    dirty_ = true;
}

IDWriteTextFormat* ConfigView::Format(const std::string& key) {
    auto& f = formats_[key];
    if (f) return f.Get();
    // Clés : "mono12", "mono12b", "sans17", "mono64"…
    const bool mono = key.rfind("mono", 0) == 0;
    const bool bold = key.back() == 'b';
    const float size = std::stof(key.substr(4, key.size() - 4 - (bold ? 1 : 0)));
    dwrite_->CreateTextFormat((mono ? mono_ : sans_).c_str(), fonts_.Get(),
                              bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, L"fr-fr", &f);
    if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return f.Get();
}

ID2D1SolidColorBrush* ConfigView::Brush(const char* name, float alpha) {
    if (brushCtx_ != ctx_) {
        brushes_.clear();
        brushCtx_ = ctx_;
    }
    auto& b = brushes_[name];
    if (!b) ctx_->CreateSolidColorBrush(Color(name), &b);
    b->SetOpacity(alpha);
    return b.Get();
}

float ConfigView::TextWidth(const std::wstring& s, const char* fmt, float spacing) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Format(fmt), 2000.f, 200.f, &layout)))
        return 0.f;
    ComPtr<IDWriteTextLayout1> l1;
    if (spacing != 0.f && SUCCEEDED(layout.As(&l1)))
        l1->SetCharacterSpacing(0.f, spacing, 0.f, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void ConfigView::Text(const std::wstring& s, const char* fmt, float x, float y, const char* color, float spacing, float maxW) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Format(fmt), maxW, 400.f, &layout))) return;
    ComPtr<IDWriteTextLayout1> l1;
    if (spacing != 0.f && SUCCEEDED(layout.As(&l1)))
        l1->SetCharacterSpacing(0.f, spacing, 0.f, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    ctx_->DrawTextLayout({x, y}, layout.Get(), Brush(color));
}

void ConfigView::OutlineText(const std::wstring& s, const char* fmt, float x, float y, const char* color, float stroke) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Format(fmt), 1000.f, 300.f, &layout))) return;
    ComPtr<IDWriteTextLayout1> l1;
    if (SUCCEEDED(layout.As(&l1))) l1->SetCharacterSpacing(0.f, 6.f, 0.f, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    OutlineRenderer renderer(factory_, ctx_, Brush(color), stroke);
    layout->Draw(nullptr, &renderer, x, y);
}

void ConfigView::Box(D2D1_RECT_F r, const std::wstring& label) {
    ctx_->FillRectangle(r, Brush("panel", 0.55f));
    ctx_->DrawRectangle(r, Brush("dash"), 1.f, dashed_.Get());
    for (const D2D1_POINT_2F c : {D2D1_POINT_2F{r.left, r.top}, {r.right, r.top}, {r.left, r.bottom}, {r.right, r.bottom}}) {
        ctx_->DrawLine({c.x - 4, c.y}, {c.x + 4, c.y}, Brush("soft"), 1.2f);  // « + » aux coins
        ctx_->DrawLine({c.x, c.y - 4}, {c.x, c.y + 4}, Brush("soft"), 1.2f);
    }
    if (!label.empty()) Text(label, "mono11b", r.left + 22, r.top + 16, "dim", 3.f);
}

void ConfigView::Leader(float x, float y, float w, const std::wstring& label, const std::wstring& status,
                        const char* statusColor, Field f, int value) {
    const std::wstring lead = L"> " + label + L" ";
    const float lw = TextWidth(lead, "mono13"), sw = TextWidth(L" " + status, "mono13");
    const float dotW = std::max(1.f, TextWidth(L"..........", "mono13") / 10.f);
    const int dots = std::max(2, static_cast<int>((w - lw - sw) / dotW));
    const bool hover = f != Field::None && Inside({x, y - 2, x + w, y + 20}, mouse_.x, mouse_.y);
    Text(lead, "mono13", x, y, hover ? "white" : "soft");
    Text(std::wstring(static_cast<size_t>(dots), L'.'), "mono13", x + lw, y, "muted");
    Text(L" " + status, "mono13", x + w - sw, y, statusColor);
    if (f != Field::None) hits_.push_back({{x, y - 2, x + w, y + 20}, f, value});
}

void ConfigView::Slider(float x, float y, float w, const std::wstring& label, float v01, const std::wstring& valueText,
                        Field f) {
    // Curseur ASCII, comme la barre « WORK [...####....] » du site ; nombre de cases selon la place.
    Text(label, "mono11", x, y, "dim", 2.f);
    const float barX = x + 150.f;
    const float cellW = std::max(1.f, TextWidth(L"##########", "mono13") / 10.f);
    const int kCells = std::clamp(static_cast<int>((w - 150.f - 70.f) / cellW) - 2, 8, 24);
    const int filled = static_cast<int>(std::round(std::clamp(v01, 0.f, 1.f) * kCells));
    const std::wstring bar = L"[" + std::wstring(static_cast<size_t>(filled), L'#') +
                             std::wstring(static_cast<size_t>(kCells - filled), L'.') + L"]";
    const float bw = TextWidth(bar, "mono13");
    const bool hover = Inside({barX, y - 4, barX + bw, y + 20}, mouse_.x, mouse_.y) || dragging_ == f;
    Text(bar, "mono13", barX, y - 2, hover ? "white" : "text");
    Text(valueText, "mono13", barX + bw + 12.f, y - 2, "amber");
    hits_.push_back({{barX + TextWidth(L"[", "mono13"), y - 6, barX + bw - TextWidth(L"]", "mono13"), y + 22}, f});
}

void ConfigView::Segmented(float x, float y, const std::wstring& label, const std::vector<std::wstring>& options,
                           int selected, Field f) {
    Text(label, "mono11", x, y, "dim", 2.f);
    float cx = x + 150.f;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        const float w = TextWidth(options[static_cast<size_t>(i)], "mono13") + 18.f;
        const D2D1_RECT_F r{cx, y - 5, cx + w, y + 19};
        if (i == selected) {
            ctx_->FillRectangle(r, Brush("white"));
            Text(options[static_cast<size_t>(i)], "mono13b", cx + 9, y - 2, "black");
        } else {
            ctx_->DrawRectangle(r, Brush(Inside(r, mouse_.x, mouse_.y) ? "soft" : "line"), 1.f);
            Text(options[static_cast<size_t>(i)], "mono13", cx + 9, y - 2, "dim");
        }
        hits_.push_back({r, f, i});
        cx += w + 8.f;
    }
}

float ConfigView::SliderValue(Field f) const {
    switch (f) {
        case Field::Scale: return (cfg_.avatarScale - 0.6f) / 1.0f;
        case Field::Opacity: return (cfg_.avatarOpacity - 0.3f) / 0.7f;
        case Field::Expressiveness: return cfg_.expressiveness;
        case Field::Quirks: return cfg_.voiceQuirks;
        case Field::Fx: return cfg_.fxIntensity;
        case Field::Rate: return (1.2f - cfg_.speechRate) / 0.35f;  // à droite = plus rapide
        default: return 0.f;
    }
}

void ConfigView::SetSliderValue(Field f, float v) {
    v = std::clamp(v, 0.f, 1.f);
    switch (f) {
        case Field::Scale: cfg_.avatarScale = 0.6f + v * 1.0f; break;
        case Field::Opacity: cfg_.avatarOpacity = 0.3f + v * 0.7f; preview_.SetOpacity(cfg_.avatarOpacity); break;
        case Field::Expressiveness: cfg_.expressiveness = v; break;
        case Field::Quirks: cfg_.voiceQuirks = v; break;
        case Field::Fx: cfg_.fxIntensity = v; break;
        case Field::Rate: cfg_.speechRate = 1.2f - v * 0.35f; break;
        default: break;
    }
    dirty_ = true;
}

bool ConfigView::Tick(float dt) {
    caretClock_ += dt;
    if (previewOk_ && preview_.Update(dt, 0.f)) dirty_ = true;
    const bool d = dirty_;
    return d;
}

// ---------------------------------------------------------------------------
void ConfigView::Render(ID2D1DeviceContext* ctx, float scale) {
    ctx_ = ctx;
    hits_.clear();
    if (!dashed_) {
        const float dashes[] = {3.f, 3.f};
        factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
                                    dashes, 2, &dashed_);
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    ctx->Clear(Color("bg"));
    for (size_t i = 0; i < stars_.size(); ++i)
        ctx->FillEllipse(D2D1::Ellipse(stars_[i], 0.8f, 0.8f), Brush("white", starAlpha_[i]));

    // --- En-tête : logo en lettres creuses, badge, fermeture -----------------------------------
    OutlineText(L"abrasion", "mono22", 40, 18, "title", 0.9f);
    {
        const std::wstring badge = L"CONFIGURATION";
        const float bw = TextWidth(badge, "mono11", 2.5f) + 34.f;
        const D2D1_RECT_F r{kWidth - 92 - bw, 20, kWidth - 92, 46};
        ctx->DrawRectangle(r, Brush("line"), 1.f);
        ctx->FillRectangle({r.left + 12, r.top + 10, r.left + 18, r.top + 16}, Brush("amber"));
        Text(badge, "mono11", r.left + 26, r.top + 6, "soft", 2.5f);
    }
    const bool closeHover = CloseHit(mouse_.x, mouse_.y);
    Text(L"×", "sans24", kWidth - 60, 14, closeHover ? "white" : "dim");

    // --- Colonne gauche ------------------------------------------------------------------------
    const float L = 40.f, LW = 520.f;
    ctx->DrawRectangle({L, 92, L + 6, 98}, Brush("soft"), 1.f);
    Text(L"COMPAGNON · RÉGLAGES", "mono11", L + 18, 87, "dim", 3.f);
    std::string title = cfg_.name.empty() ? std::string("compagnon") : cfg_.name;
    for (auto& c : title) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    OutlineText(W(title), "mono64b", L - 2, 104, "title", 1.3f);
    Text(L"Son corps, sa voix, sa façon de réagir. Tout reste sur ton PC.", "sans17", L, 196, "soft", 0.f, LW);

    // Personnage
    D2D1_RECT_F box{L, 240, L + LW, 404};
    Box(box, L"PERSONNAGE");
    {
        float y = box.top + 50;
        Text(L"NOM", "mono11", L + 22, y, "dim", 2.f);
        const D2D1_RECT_F field{L + 172, y - 6, L + 400, y + 20};
        ctx->DrawLine({field.left, field.bottom}, {field.right, field.bottom}, Brush(focus_ == Field::Name ? "white" : "dash"), 1.f,
                      dashed_.Get());
        const std::wstring name = W(cfg_.name);
        Text(name, "mono14", field.left + 2, y - 4, "text");
        if (focus_ == Field::Name && std::fmod(caretClock_, 1.f) < 0.55f) {
            const float cx = field.left + 3 + TextWidth(name, "mono14");
            ctx->FillRectangle({cx, y - 3, cx + 8, y + 16}, Brush("text"));  // curseur bloc, façon terminal
        }
        hits_.push_back({field, Field::Name});
        Text(L"(au redémarrage)", "mono11", L + 410, y - 1, "muted");
        y += 34;
        Slider(L + 22, y, LW - 44, L"TAILLE", SliderValue(Field::Scale), std::format(L"{:.0f} %", cfg_.avatarScale * 100), Field::Scale);
        y += 30;
        Slider(L + 22, y, LW - 44, L"OPACITÉ", SliderValue(Field::Opacity), std::format(L"{:.0f} %", cfg_.avatarOpacity * 100), Field::Opacity);
        y += 30;
        Segmented(L + 22, y, L"COIN", {L"↖", L"↗", L"↙", L"↘"}, cfg_.avatarCorner, Field::Corner);
    }

    // Voix
    box = {L, 422, L + LW, 650};
    Box(box, L"VOIX");
    {
        float y = box.top + 50;
        Slider(L + 22, y, LW - 44, L"GRAIN 8-BIT", SliderValue(Field::Fx), std::format(L"{:.0f} %", cfg_.fxIntensity * 100), Field::Fx);
        y += 30;
        Slider(L + 22, y, LW - 44, L"DÉBIT", SliderValue(Field::Rate), std::format(L"x{:.2f}", 1.f / cfg_.speechRate), Field::Rate);
        y += 28;
        Segmented(L + 22, y, L"MOTEUR", {L"neuronale", L"piper"}, cfg_.neuralVoice ? 0 : 1, Field::Engine);
        y += 28;
        Segmented(L + 22, y, L"ÉCOUTE", {L"push-to-talk", L"continue"}, cfg_.voiceActivity ? 1 : 0, Field::Listen);
        y += 28;
        Segmented(L + 22, y, L"TON", {L"poli", L"pote", L"sans filtre"}, cfg_.tone, Field::Tone);
        y += 32;
        Leader(L + 22, y, LW - 44, L"relances orales (« hmm… »)", cfg_.backchannels ? L"activé" : L"désactivé",
               cfg_.backchannels ? "green" : "muted", Field::Backchannels);
        y += 22;
        Leader(L + 22, y, LW - 44, L"me couper la parole", cfg_.bargeIn ? L"activé" : L"désactivé",
               cfg_.bargeIn ? "green" : "muted", Field::BargeIn);
    }

    // Boutons : blanc à coins coupés + annuler
    {
        const D2D1_RECT_F r{L, 676, L + 214, 718};
        ComPtr<ID2D1PathGeometry> btn;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory_->CreatePathGeometry(&btn)) && SUCCEEDED(btn->Open(&sink))) {
            const float c = 7.f;
            sink->BeginFigure({r.left + c, r.top}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F pts[] = {{r.right - c, r.top}, {r.right, r.top + c}, {r.right, r.bottom - c},
                                         {r.right - c, r.bottom}, {r.left + c, r.bottom}, {r.left, r.bottom - c},
                                         {r.left, r.top + c}};
            sink->AddLines(pts, 7);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            const bool hover = Inside(r, mouse_.x, mouse_.y);
            ctx->FillGeometry(btn.Get(), Brush(hover ? "soft" : "white"));
        }
        Text(L"Enregistrer  →", "mono15b", r.left + 22, r.top + 10, "black");
        hits_.push_back({r, Field::Save});
        const D2D1_RECT_F cancel{L + 238, 684, L + 330, 712};
        Text(L"annuler", "mono13", cancel.left, cancel.top + 4, Inside(cancel, mouse_.x, mouse_.y) ? "white" : "dim");
        hits_.push_back({cancel, Field::Cancel});
    }

    // --- Colonne droite : aperçu, état, émotions & cerveau ---------------------------------------
    const float R = 600.f, RW = 400.f;
    box = {R, 92, R + RW, 412};
    Box(box, L"APERÇU · 24 I/S");
    if (previewOk_) {
        // Aperçu à l'échelle choisie (bornée pour tenir dans le cadre), sous le titre du cadre.
        const float ps = std::clamp(0.52f * cfg_.avatarScale, 0.34f, 0.6f);
        preview_.Render(ctx, factory_, scale * ps,
                        {scale * (box.left + RW / 2 - 160.f * ps), scale * (box.top + 262.f - 400.f * ps)});
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    }
    {
        float x = box.left + 22, y = box.bottom - 34;
        for (int i = 0; i < 9; ++i) {
            const std::wstring label = std::wstring(L"> ") + kExpressions[i][0];
            const float w = TextWidth(label, "mono11") + 10.f;
            if (x + w > box.right - 14) { x = box.left + 22; y += 18; }
            const D2D1_RECT_F r{x, y - 2, x + w, y + 15};
            Text(label, "mono11", x, y, Inside(r, mouse_.x, mouse_.y) ? "white" : "dim");
            hits_.push_back({r, Field::Expression, i});
            x += w + 4;
        }
    }

    // État, façon liste terminal du site
    {
        float y = 432;
        Leader(R + 4, y, RW - 8, L"cerveau local", status_.brainReady ? L"prêt" : L"modèle absent",
               status_.brainReady ? "green" : "amber", Field::None);
        y += 22;
        Leader(R + 4, y, RW - 8, L"voix",
               !(status_.ttsReady && status_.sttReady) ? L"incomplète" : status_.neuralVoice ? L"neuronale" : L"piper",
               status_.ttsReady && status_.sttReady ? "green" : "amber", Field::None);
        y += 22;
        Leader(R + 4, y, RW - 8, L"ta voix habituelle", status_.voiceSamples >= 10 ? L"apprise" : std::format(L"{}/10 phrases", status_.voiceSamples),
               status_.voiceSamples >= 10 ? "green" : "amber", Field::None);
        y += 30;
        // Jauge d'émotion : position = activation (calme à gauche, agité à droite)
        constexpr int kCells = 21;
        const int pos = std::clamp(static_cast<int>(std::round((status_.arousal + 1.f) / 2.f * (kCells - 1))), 0, kCells - 1);
        std::wstring bar(static_cast<size_t>(kCells), L'.');
        for (int k = std::max(0, pos - 1); k <= std::min(kCells - 1, pos + 1); ++k) bar[static_cast<size_t>(k)] = L'#';
        Text(L"ÉMOTION [" + bar + L"]  " + W(status_.emotion), "mono13", R + 4, y, "text");
    }

    box = {R, 532, R + RW, 718};
    Box(box, L"ÉMOTIONS & CERVEAU");
    {
        float y = box.top + 48;
        Leader(R + 18, y, RW - 36, L"lire les émotions dans ta voix", cfg_.emotionDetection ? L"activé" : L"désactivé",
               cfg_.emotionDetection ? "green" : "muted", Field::Emotion);
        y += 26;
        Slider(R + 18, y, RW - 36, L"EXPRESSIVITÉ", SliderValue(Field::Expressiveness),
               std::format(L"{:.0f} %", cfg_.expressiveness * 100), Field::Expressiveness);
        y += 28;
        Slider(R + 18, y, RW - 36, L"TICS HUMAINS", SliderValue(Field::Quirks),
               cfg_.voiceQuirks < 0.01f ? std::wstring(L"jamais") : std::format(L"{:.0f} %", cfg_.voiceQuirks * 100), Field::Quirks);
        y += 28;
        Leader(R + 18, y, RW - 36, L"s'entraîner pendant tes pauses", cfg_.autoTraining ? L"activé" : L"désactivé",
               cfg_.autoTraining ? "green" : "muted", Field::AutoTraining);
        y += 22;
        Leader(R + 18, y, RW - 36, L"claude en dernier recours",
               !cfg_.cloudEnabled ? L"désactivé" : (status_.claudeKey ? L"activé" : L"sans clé"),
               !cfg_.cloudEnabled ? "muted" : (status_.claudeKey ? "green" : "amber"), Field::Cloud);
        y += 22;
        Leader(R + 18, y, RW - 36, L"demander avant claude", cfg_.cloudAsksPermission ? L"toujours" : L"non",
               cfg_.cloudAsksPermission ? "green" : "amber", Field::CloudAsk);
    }

    // --- Clé Claude ------------------------------------------------------------------------------
    box = {L, 740, L + LW, 816};
    Box(box, L"CLÉ CLAUDE");
    {
        const float y = box.top + 44;
        Text(L"CLÉ API", "mono11", L + 22, y, "dim", 2.f);
        const D2D1_RECT_F field{L + 120, y - 6, L + LW - 100, y + 20};
        ctx->DrawLine({field.left, field.bottom}, {field.right, field.bottom},
                      Brush(focus_ == Field::ClaudeKey ? "white" : "dash"), 1.f, dashed_.Get());
        std::wstring shown;
        const char* color = "text";
        const std::string& k = cfg_.claudeKeyEntry;
        if (!k.empty()) {  // jamais affichée en clair : début et 4 derniers caractères
            shown = W(k.substr(0, std::min<std::size_t>(7, k.size()))) + std::wstring(std::min<std::size_t>(12, k.size()), L'•') +
                    (k.size() > 11 ? W(k.substr(k.size() - 4)) : std::wstring{});
        } else if (cfg_.claudeKeyRemove) {
            shown = L"sera supprimée à l'enregistrement";
            color = "amber";
        } else if (focus_ == Field::ClaudeKey) {
            shown.clear();
        } else {
            shown = status_.claudeKey ? L"•••••• enregistrée (coffre Windows)" : L"clique puis colle ta clé (Ctrl+V)";
            color = status_.claudeKey ? "green" : "muted";
        }
        Text(shown, "mono13", field.left + 2, y - 3, color);
        if (focus_ == Field::ClaudeKey && std::fmod(caretClock_, 1.f) < 0.55f) {
            const float cx = field.left + 3 + TextWidth(shown, "mono13");
            ctx->FillRectangle({cx, y - 3, cx + 8, y + 15}, Brush("text"));
        }
        hits_.push_back({field, Field::ClaudeKey});
        if (status_.claudeKey || !k.empty()) {
            const D2D1_RECT_F clear{L + LW - 86, y - 4, L + LW - 22, y + 16};
            Text(L"effacer", "mono11", clear.left, clear.top + 2, Inside(clear, mouse_.x, mouse_.y) ? "white" : "dim");
            hits_.push_back({clear, Field::ClaudeKeyClear});
        }
    }

    // --- Affichage ------------------------------------------------------------------------------
    box = {R, 740, R + RW, 830};
    Box(box, L"AFFICHAGE ET CERVEAU");
    Leader(R + 18, box.top + 38, RW - 36, L"sous-titres sous le personnage", cfg_.captions ? L"activé" : L"désactivé",
           cfg_.captions ? "green" : "muted", Field::Captions);
    Leader(R + 18, box.top + 62, RW - 36, L"grand cerveau (pro)", cfg_.expertAuto ? L"auto" : L"sur demande",
           cfg_.expertAuto ? "green" : "muted", Field::ExpertAuto);

    // --- Journal de la conversation -------------------------------------------------------------
    box = {L, 840, R + RW, 1082};
    Box(box, L"CONVERSATION");
    RenderConversation({box.left + 22, box.top + 42, box.right - 22, box.bottom - 10});

    // --- Pied de page --------------------------------------------------------------------------
    ctx->DrawLine({0, kHeight - 24}, {kWidth, kHeight - 24}, Brush("line"), 1.f);
    Text(L"© 2026 abrasion · compagnon", "mono11", 40, kHeight - 18, "muted", 1.f);
    const std::wstring right = L"local · 24 i/s · plugin's for all";
    Text(right, "mono11", kWidth - 40 - TextWidth(right, "mono11", 1.f), kHeight - 18, "muted", 1.f);

    ctx->SetTransform(D2D1::IdentityMatrix());
    ctx_ = nullptr;
    dirty_ = false;
}

float ConfigView::TextHeight(const std::wstring& s, const char* fmt, float maxW) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), Format(fmt), maxW, 2000.f, &layout))) return 16.f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

// Du plus récent (en bas) au plus ancien, autant que la zone en contient ; la molette remonte dans l'historique.
void ConfigView::RenderConversation(D2D1_RECT_F area) {
    convRect_ = area;
    hits_.push_back({area, Field::Conversation});
    const auto& conv = status_.conversation;
    if (conv.empty()) {
        Text(L"Rien pour l'instant. Parle-lui, la conversation s'affiche ici.", "mono11", area.left, area.top, "muted");
        return;
    }
    scroll_ = std::clamp(scroll_, 0, static_cast<int>(conv.size()) - 1);
    const float labelW = 64.f, textW = area.right - area.left - labelW;
    ctx_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    float bottom = area.bottom;
    for (int i = static_cast<int>(conv.size()) - 1 - scroll_; i >= 0 && bottom > area.top; --i) {
        const auto& [role, text] = conv[static_cast<std::size_t>(i)];
        const std::wstring t = W(text);
        const float h = std::max(16.f, TextHeight(t, "sans13", textW)) + 6.f;
        const float top = bottom - h;
        if (top < area.top - 1.f) break;  // pas de message coupé : la molette remonte
        const bool omnis = role == "assistant";
        Text(omnis ? L"OMNIS" : L"TOI", "mono11", area.left, top + 2, omnis ? "green" : "amber", 1.5f);
        Text(t, "sans13", area.left + labelW, top, omnis ? "text" : "soft", 0.f, textW);
        bottom = top;
    }
    ctx_->PopAxisAlignedClip();
    if (scroll_ > 0) Text(std::format(L"↓ {} plus récents (molette)", scroll_), "mono11", area.right - 210, area.bottom - 14, "dim");
}

// ---------------------------------------------------------------------------
bool ConfigView::InCaption(float x, float y) const { return y < 70.f && !CloseHit(x, y); }
bool ConfigView::CloseHit(float x, float y) const { return x > kWidth - 76.f && x < kWidth - 20.f && y > 8.f && y < 56.f; }

ConfigView::Action ConfigView::MouseDown(float x, float y) {
    mouse_ = {x, y};
    if (CloseHit(x, y)) return Action::Cancel;
    focus_ = Field::None;
    for (const Hit& h : hits_) {
        if (!Inside(h.r, x, y)) continue;
        dirty_ = true;
        switch (h.field) {
            case Field::Scale: case Field::Opacity: case Field::Expressiveness: case Field::Quirks: case Field::Fx: case Field::Rate:
                dragging_ = h.field;
                dragRect_ = h.r;
                SetSliderValue(h.field, (x - h.r.left) / (h.r.right - h.r.left));
                return Action::None;
            case Field::Corner: cfg_.avatarCorner = h.value; return Action::None;
            case Field::Listen: cfg_.voiceActivity = h.value == 1; return Action::None;
            case Field::Tone: cfg_.tone = h.value; return Action::None;
            case Field::Engine: cfg_.neuralVoice = h.value == 0; return Action::None;
            case Field::Backchannels: cfg_.backchannels = !cfg_.backchannels; return Action::None;
            case Field::BargeIn: cfg_.bargeIn = !cfg_.bargeIn; return Action::None;
            case Field::Emotion: cfg_.emotionDetection = !cfg_.emotionDetection; return Action::None;
            case Field::AutoTraining: cfg_.autoTraining = !cfg_.autoTraining; return Action::None;
            case Field::Cloud: cfg_.cloudEnabled = !cfg_.cloudEnabled; return Action::None;
            case Field::CloudAsk: cfg_.cloudAsksPermission = !cfg_.cloudAsksPermission; return Action::None;
            case Field::Name: focus_ = Field::Name; caretClock_ = 0.f; return Action::None;
            case Field::ClaudeKey:
                focus_ = Field::ClaudeKey;
                cfg_.claudeKeyRemove = false;
                caretClock_ = 0.f;
                return Action::None;
            case Field::ClaudeKeyClear:
                cfg_.claudeKeyEntry.clear();
                cfg_.claudeKeyRemove = status_.claudeKey;
                return Action::None;
            case Field::Captions: cfg_.captions = !cfg_.captions; return Action::None;
            case Field::ExpertAuto: cfg_.expertAuto = !cfg_.expertAuto; return Action::None;
            case Field::Expression: {
                const std::wstring clip = kExpressions[h.value][1];
                const std::string c(clip.begin(), clip.end());
                if (c == "wave") preview_.Wave();
                else if (c == "dance") preview_.SetMood(c);
                else { preview_.SetMood("idle"); preview_.Express(c); }
                return Action::None;
            }
            case Field::Save: return Action::Save;
            case Field::Cancel: return Action::Cancel;
            default: return Action::None;
        }
    }
    dirty_ = true;
    return Action::None;
}

void ConfigView::MouseMove(float x, float y) {
    mouse_ = {x, y};
    if (dragging_ != Field::None) SetSliderValue(dragging_, (x - dragRect_.left) / (dragRect_.right - dragRect_.left));
    dirty_ = true;
}

void ConfigView::MouseUp() { dragging_ = Field::None; }

ConfigView::Action ConfigView::Key(unsigned vk) {
    if (vk == VK_ESCAPE) return Action::Cancel;
    if (vk == VK_RETURN) return Action::Save;
    if (focus_ == Field::ClaudeKey && vk == VK_BACK && !cfg_.claudeKeyEntry.empty()) {
        cfg_.claudeKeyEntry.pop_back();
        dirty_ = true;
    }
    if (focus_ == Field::Name && vk == VK_BACK && !cfg_.name.empty()) {
        // Retire le dernier caractère UTF-8 complet.
        size_t i = cfg_.name.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(cfg_.name[i]) & 0xC0) == 0x80) --i;
        cfg_.name.erase(i);
        dirty_ = true;
    }
    return Action::None;
}

void ConfigView::Char(wchar_t c) {
    if (focus_ == Field::ClaudeKey) {  // clé : ASCII imprimable, sans espace
        if (c > 32 && c < 127 && cfg_.claudeKeyEntry.size() < 256) cfg_.claudeKeyEntry += static_cast<char>(c);
        dirty_ = true;
        return;
    }
    if (focus_ != Field::Name || c < 32 || cfg_.name.size() >= 24) return;
    char buf[8];
    const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, sizeof(buf), nullptr, nullptr);
    if (n > 0) cfg_.name.append(buf, static_cast<size_t>(n));
    dirty_ = true;
}

void ConfigView::Paste(const std::wstring& text) {
    if (focus_ != Field::ClaudeKey && focus_ != Field::Name) return;
    if (focus_ == Field::ClaudeKey) cfg_.claudeKeyEntry.clear();  // coller remplace (on colle une clé entière)
    for (wchar_t c : text) Char(c);
}

void ConfigView::Wheel(int notches) {
    if (!Inside(convRect_, mouse_.x, mouse_.y)) return;
    scroll_ = std::max(0, scroll_ + notches);  // molette vers le haut = plus ancien
    dirty_ = true;
}

}  // namespace companion
