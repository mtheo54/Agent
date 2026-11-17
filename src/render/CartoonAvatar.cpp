#include "render/CartoonAvatar.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <numbers>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
float Rad(float deg) { return deg * kPi / 180.f; }

D2D1_POINT_2F Add(D2D1_POINT_2F p, D2D1_POINT_2F q, float k = 1.f) { return {p.x + q.x * k, p.y + q.y * k}; }
D2D1_POINT_2F Rot(float deg, D2D1_POINT_2F v) {
    const float c = std::cos(Rad(deg)), s = std::sin(Rad(deg));
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
D2D1_POINT_2F Dir(float deg) { return {std::sin(Rad(deg)), std::cos(Rad(deg))}; }  // 0 = bas, + = droite
D2D1_POINT_2F Lerp(D2D1_POINT_2F a, D2D1_POINT_2F b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

float Ease(const std::string& kind, float t) {
    if (kind == "hold") return 0.f;
    if (kind == "linear") return t;
    if (kind == "in") return t * t * t;
    if (kind == "out") return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
    return t * t * (3.f - 2.f * t);  // "ease"
}

D2D1_COLOR_F Hex(const std::string& h) {
    auto byte = [&](size_t i) { return static_cast<float>(std::stoi(h.substr(i, 2), nullptr, 16)) / 255.f; };
    return D2D1::ColorF(byte(1), byte(3), byte(5), 1.f);
}

D2D1_POINT_2F Pt(const nlohmann::json& j) { return {j.at(0).get<float>(), j.at(1).get<float>()}; }
std::array<float, 2> Pair(const nlohmann::json& j) { return {j.at(0).get<float>(), j.at(1).get<float>()}; }

// --- Géométries ------------------------------------------------------------
// Arc de cercle approché par des courbes de Bézier cubiques (au plus 90° chacune, erreur < 0,03 % du
// rayon). Plus portable que AddArc, que certains moteurs Direct2D (Wine) rendent mal.
void AddArcBeziers(ID2D1GeometrySink* sink, D2D1_POINT_2F c, float r, float a0, float a1) {
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(a1 - a0) / (kPi / 2) - 1e-4f)));
    const float step = (a1 - a0) / static_cast<float>(n);
    const float k = 4.f / 3.f * std::tan(step / 4.f);
    for (int i = 0; i < n; ++i) {
        const float s0 = a0 + step * i, s1 = s0 + step;
        const D2D1_POINT_2F p0{c.x + r * std::cos(s0), c.y + r * std::sin(s0)};
        const D2D1_POINT_2F p3{c.x + r * std::cos(s1), c.y + r * std::sin(s1)};
        sink->AddBezier(D2D1::BezierSegment({p0.x - k * r * std::sin(s0), p0.y + k * r * std::cos(s0)},
                                            {p3.x + k * r * std::sin(s1), p3.y - k * r * std::cos(s1)}, p3));
    }
}

// Capsule effilée : même construction que capsule() dans tools/avatar_preview.py.
ComPtr<ID2D1Geometry> Capsule(ID2D1Factory1* f, D2D1_POINT_2F p0, D2D1_POINT_2F p1, float r0, float r1) {
    const float ang = std::atan2(p1.y - p0.y, p1.x - p0.x);
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure({p0.x + r0 * std::cos(ang + kPi / 2), p0.y + r0 * std::sin(ang + kPi / 2)}, D2D1_FIGURE_BEGIN_FILLED);
    AddArcBeziers(sink.Get(), p0, r0, ang + kPi / 2, ang + 3 * kPi / 2);
    sink->AddLine({p1.x + r1 * std::cos(ang - kPi / 2), p1.y + r1 * std::sin(ang - kPi / 2)});
    AddArcBeziers(sink.Get(), p1, r1, ang - kPi / 2, ang + kPi / 2);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

ComPtr<ID2D1Geometry> Ellipse(ID2D1Factory1* f, D2D1_POINT_2F c, float rx, float ry, float angleDeg = 0.f) {
    ComPtr<ID2D1EllipseGeometry> e;
    if (FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(c, rx, ry), &e))) return nullptr;
    if (angleDeg == 0.f) return e;
    ComPtr<ID2D1TransformedGeometry> t;
    f->CreateTransformedGeometry(e.Get(), D2D1::Matrix3x2F::Rotation(angleDeg, c), &t);
    return t;
}

// Arc de cercle ouvert de a0 à a1 (degrés, sens horaire à l'écran, comme cairo.arc).
ComPtr<ID2D1Geometry> Arc(ID2D1Factory1* f, D2D1_POINT_2F c, float r, float a0, float a1) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure({c.x + r * std::cos(Rad(a0)), c.y + r * std::sin(Rad(a0))}, D2D1_FIGURE_BEGIN_HOLLOW);
    AddArcBeziers(sink.Get(), c, r, Rad(a0), Rad(a1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return path;
}

// Traits ouverts : suite de figures {départ, [c1, c2, fin]...} (courbes de Bézier) ou segments (2 points).
ComPtr<ID2D1Geometry> Strokes(ID2D1Factory1* f, const std::vector<std::vector<D2D1_POINT_2F>>& figures) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    for (const auto& fig : figures) {
        if (fig.size() < 2) continue;
        sink->BeginFigure(fig[0], D2D1_FIGURE_BEGIN_HOLLOW);
        if (fig.size() == 2) sink->AddLine(fig[1]);
        for (size_t i = 1; i + 2 < fig.size(); i += 3) sink->AddBezier(D2D1::BezierSegment(fig[i], fig[i + 1], fig[i + 2]));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    sink->Close();
    return path;
}

// Étincelle à 4 branches (même construction que star() en Python).
ComPtr<ID2D1Geometry> Star(ID2D1Factory1* f, D2D1_POINT_2F c, float r, float angle) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    for (int i = 0; i < 8; ++i) {
        const float a = Rad(angle + i * 45.f), rr = i % 2 == 0 ? r : r * 0.22f;
        const D2D1_POINT_2F p{c.x + std::sin(a) * rr, c.y - std::cos(a) * rr};
        if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
        else sink->AddLine(p);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

ComPtr<ID2D1Geometry> Group(ID2D1Factory1* f, std::initializer_list<ComPtr<ID2D1Geometry>> parts) {
    std::vector<ID2D1Geometry*> raw;
    for (const auto& p : parts)
        if (p) raw.push_back(p.Get());
    ComPtr<ID2D1GeometryGroup> g;
    f->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()), &g);
    return g;
}

}  // namespace

// ---------------------------------------------------------------------------
bool CartoonAvatar::Load(const std::filesystem::path& rigJson) {
    std::ifstream file(rigJson, std::ios::binary);
    if (!file) {
        log::Info("Avatar: {} introuvable", log::Narrow(rigJson.wstring()));
        return false;
    }
    try {
        const auto j = nlohmann::json::parse(file);
        fps_ = j.value("fps", 24.f);
        canvas_ = Pair(j.at("canvas"));
        hips_ = Pt(j.at("hips"));
        ground_ = j.at("ground").get<float>();
        const auto& b = j.at("body");
        body_.headRadius = b.at("headRadius");
        body_.headGap = b.at("headGap");
        body_.neckY = b.at("neckY");
        body_.torso.clear();
        for (const auto& p : b.at("torso")) body_.torso.push_back(Pt(p));
        body_.shoulderA = Pt(b.at("shoulderA"));
        body_.shoulderB = Pt(b.at("shoulderB"));
        body_.hipA = Pt(b.at("hipA"));
        body_.hipB = Pt(b.at("hipB"));
        body_.upperArm = b.at("upperArm");
        body_.foreArm = b.at("foreArm");
        body_.handR = b.at("handR");
        body_.thigh = b.at("thigh");
        body_.shin = b.at("shin");
        body_.upperArmR = Pair(b.at("upperArmR"));
        body_.foreArmR = Pair(b.at("foreArmR"));
        body_.thighR = Pair(b.at("thighR"));
        body_.shinR = Pair(b.at("shinR"));
        body_.foot = Pair(b.at("foot"));
        for (const auto& [k, v] : j.at("palette").items()) palette_[k] = Hex(v.get<std::string>());
        const auto& st = j.at("style");
        outline_ = st.at("outline");
        shadowOffset_ = Pt(st.at("shadowOffset"));
        limbShadowOffset_ = Pt(st.at("limbShadowOffset"));
        innerLine_ = st.value("innerLine", innerLine_);
        rimWidth_ = st.value("rimWidth", rimWidth_);
        weightRatio_ = st.value("weightRatio", weightRatio_);
        if (st.contains("rimOffset")) rimOffset_ = Pt(st.at("rimOffset"));
        if (st.contains("weightOffset")) weightOffset_ = Pt(st.at("weightOffset"));
        for (const auto& [k, v] : j.at("rest").items()) rest_[k] = v.get<float>();
        for (const auto& [name, c] : j.at("clips").items()) {
            Clip clip;
            clip.frames = c.at("frames");
            clip.loop = c.value("loop", true);
            clip.then = c.value("then", std::string{"idle"});
            clip.emote = c.value("emote", std::string{});
            clip.trail = c.value("trail", std::vector<std::string>{});
            for (const auto& [ch, keys] : c.at("keys").items())
                for (const auto& k : keys)
                    clip.keys[ch].push_back({k.at(0).get<int>(), k.at(1).get<float>(),
                                             k.size() > 2 ? k.at(2).get<std::string>() : std::string{"ease"}});
            clips_[name] = std::move(clip);
        }
    } catch (const std::exception& e) {
        log::Info("Avatar: rig.json invalide ({})", e.what());
        return false;
    }
    if (!clips_.contains("idle")) return false;
    log::Info("Avatar 2D: {} animations à {} i/s", clips_.size(), fps_);
    return true;
}

CartoonAvatar::Pose CartoonAvatar::Sample(const Clip& clip, int frame) const {
    Pose pose = rest_;
    for (const auto& [ch, keys] : clip.keys) {
        if (frame <= keys.front().frame) { pose[ch] = keys.front().value; continue; }
        if (frame >= keys.back().frame) { pose[ch] = keys.back().value; continue; }
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const Key& k0 = keys[i];
            const Key& k1 = keys[i + 1];
            if (k0.frame <= frame && frame < k1.frame) {
                const float t = static_cast<float>(frame - k0.frame) / static_cast<float>(k1.frame - k0.frame);
                pose[ch] = k0.value + (k1.value - k0.value) * Ease(k0.ease, t);
                break;
            }
        }
    }
    return pose;
}

std::string CartoonAvatar::ClipForState() const {
    switch (state_) {
        case AvatarState::Listening: return "listen";
        case AvatarState::Studio: return moodClip_ == "dance" ? "dance" : "studio";
        case AvatarState::Escalating: return "think";
        case AvatarState::Training: return "train";
        case AvatarState::Gaming: return "idle";
        default: return clips_.contains(moodClip_) ? moodClip_ : "idle";
    }
}

bool CartoonAvatar::Update(float dt, float voiceRms) noexcept {
    rmsEnv_ += (voiceRms - rmsEnv_) * (voiceRms > rmsEnv_ ? 0.6f : 0.15f);
    const int advance = state_ == AvatarState::Gaming ? 2 : 1;  // en jeu : « on twos », 12 i/s
    const float step = static_cast<float>(advance) / fps_;
    clock_ += std::clamp(dt, 0.f, 0.25f);
    if (clock_ < step) return false;
    clock_ = std::fmod(clock_, step);

    auto find = [&](const std::string& n) -> const Clip& { return clips_.contains(n) ? clips_.at(n) : clips_.at("idle"); };
    const Clip& cur = find(clip_);
    const bool oneShotRunning = !cur.loop && frame_ + advance < cur.frames;
    const bool speaking = rmsEnv_ > 0.03f;
    std::string next = clip_;
    if (waveRequested_ && clips_.contains("wave")) {
        next = "wave";
        waveRequested_ = false;
    } else if (!pendingExpression_.empty() && (!oneShotRunning || clip_ == "talk" || clip_ == "talk2")) {
        if (clips_.contains(pendingExpression_)) next = pendingExpression_;
        pendingExpression_.clear();
    } else if (!oneShotRunning) {
        if (speaking) {
            // Gestes variés : à chaque nouvelle prise de parole, une variante tirée au hasard.
            if (clip_ != "talk" && clip_ != "talk2") {
                rng_ = rng_ * 1664525u + 1013904223u;
                talkVariant_ = (rng_ >> 16) % 3 == 0 && clips_.contains("talk2") ? "talk2" : "talk";
            }
            next = clips_.contains(talkVariant_) ? talkVariant_ : ClipForState();
        } else {
            next = ClipForState();
        }
    }

    if (next != clip_ || (!cur.loop && !oneShotRunning)) {
        prevClip_ = clip_;
        prevFrame_ = std::min(frame_, cur.frames - 1);
        blend_ = 3;  // fondu court : pas de saut de pose
        clip_ = next;
        frame_ = 0;
    } else {
        frame_ = cur.loop ? (frame_ + advance) % cur.frames : std::min(frame_ + advance, cur.frames - 1);
        blend_ = std::max(0, blend_ - advance);
    }
    dirty_ = true;
    return true;
}

ID2D1SolidColorBrush* CartoonAvatar::Brush(ID2D1DeviceContext* ctx, const std::string& name, float alpha) {
    if (brushCtx_ != ctx) {  // nouveau contexte : ressources recréées
        brushes_.clear();
        roundStroke_.Reset();
        brushCtx_ = ctx;
    }
    auto& b = brushes_[name];
    if (!b) {
        const auto it = palette_.find(name);
        ctx->CreateSolidColorBrush(it != palette_.end() ? it->second : D2D1::ColorF(D2D1::ColorF::White), &b);
    }
    b->SetOpacity(alpha);
    return b.Get();
}

CartoonAvatar::Skeleton CartoonAvatar::Solve(const Pose& pose) const {
    const auto P = [&](const char* ch) { const auto it = pose.find(ch); return it != pose.end() ? it->second : 0.f; };
    const Body& b = body_;
    Skeleton k;
    k.H = {hips_.x + P("x"), hips_.y + P("y") - rmsEnv_ * 30.f};  // petit rebond sur la voix
    k.T = P("torso");
    const auto at = [&](D2D1_POINT_2F v) { return Add(k.H, Rot(k.T, v)); };
    for (int i = 0; i < 2; ++i) {
        const bool a = i == 0;
        k.shoulder[i] = at(a ? b.shoulderA : b.shoulderB);
        const float up = (a ? P("aU") : P("bU")) + k.T;
        k.fore[i] = up + (a ? P("aF") : P("bF"));
        k.elbow[i] = Add(k.shoulder[i], Dir(up), b.upperArm);
        k.hand[i] = Add(k.elbow[i], Dir(k.fore[i]), b.foreArm);
        k.hip[i] = Add(k.H, a ? b.hipA : b.hipB);
        const float th = a ? P("aT") : P("bT");
        const float sh = th + (a ? P("aS") : P("bS"));
        k.knee[i] = Add(k.hip[i], Dir(th), b.thigh);
        k.ankle[i] = Add(k.knee[i], Dir(sh), b.shin);
    }
    k.neck = at({0, b.neckY});
    k.ha = k.T + P("head");
    k.hc = Add(Add(k.neck, Rot(k.ha, {0, -(b.headRadius + b.headGap)})), {P("headX"), P("headY")});
    k.headOffset = {P("headX"), P("headY")};
    return k;
}

void CartoonAvatar::Render(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, float scale, D2D1_POINT_2F offset) {
    if (!ctx || !factory || clips_.empty()) return;
    auto find = [&](const std::string& n) -> const Clip& { return clips_.contains(n) ? clips_.at(n) : clips_.at("idle"); };
    Pose pose = Sample(find(clip_), frame_);
    if (blend_ > 0 && !prevClip_.empty()) {
        const Pose prev = Sample(find(prevClip_), prevFrame_);
        const float w = static_cast<float>(blend_) / 4.f;
        for (auto& [ch, v] : pose)
            if (const auto it = prev.find(ch); it != prev.end()) v += (it->second - v) * w;
    }
    if (!roundStroke_) {
        D2D1_STROKE_STYLE_PROPERTIES1 p{};  // champs remplis à la main (helper absent de certains SDK)
        p.startCap = p.endCap = p.dashCap = D2D1_CAP_STYLE_ROUND;
        p.lineJoin = D2D1_LINE_JOIN_ROUND;
        p.miterLimit = 10.f;
        p.dashStyle = D2D1_DASH_STYLE_SOLID;
        p.transformType = D2D1_STROKE_TRANSFORM_TYPE_NORMAL;
        factory->CreateStrokeStyle(p, nullptr, 0, &roundStroke_);
    }
    const float alpha = opacity_ * (state_ == AvatarState::Gaming ? 0.55f : 1.f);
    const bool dim = alpha < 0.999f;
    if (dim) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                   D2D1::IdentityMatrix(), alpha), nullptr);
    Draw(ctx, factory, pose, find(clip_), scale, offset);
    if (dim) ctx->PopLayer();
    dirty_ = false;
}

void CartoonAvatar::Draw(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const Pose& pose, const Clip& clip, float scale,
                         D2D1_POINT_2F offset) {
    const auto P = [&](const char* ch) { const auto it = pose.find(ch); return it != pose.end() ? it->second : 0.f; };
    const Body& b = body_;
    const Skeleton k = Solve(pose);
    const auto at = [&](D2D1_POINT_2F v) { return Add(k.H, Rot(k.T, v)); };
    const D2D1_POINT_2F g{hips_.x, ground_};
    const float sq = P("sq");
    const auto global = D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(offset.x, offset.y);
    const auto squash = D2D1::Matrix3x2F::Translation(-g.x, -g.y) * D2D1::Matrix3x2F::Scale(1.f + sq * 0.6f, 1.f - sq) *
                        D2D1::Matrix3x2F::Translation(g.x, g.y);
    const auto stroke = [&](const ComPtr<ID2D1Geometry>& geom, ID2D1Brush* br, float w) {
        if (geom) ctx->DrawGeometry(geom.Get(), br, w, roundStroke_.Get());
    };
    const auto fill = [&](const ComPtr<ID2D1Geometry>& geom, ID2D1Brush* br) {
        if (geom) ctx->FillGeometry(geom.Get(), br);
    };
    const auto shifted = [&](const ComPtr<ID2D1Geometry>& geom, D2D1_POINT_2F d) {
        ComPtr<ID2D1TransformedGeometry> t;
        factory->CreateTransformedGeometry(geom.Get(), D2D1::Matrix3x2F::Translation(d.x, d.y), &t);
        return ComPtr<ID2D1Geometry>(t);
    };

    // Ombre au sol, deux tons nets (hors écrasement).
    ctx->SetTransform(global);
    const float lift = std::max(0.f, -P("y")) / 40.f;
    ctx->FillEllipse(D2D1::Ellipse({g.x, g.y + 6}, 66.f * (1 - lift * 0.3f), 9.f), Brush(ctx, "groundShadow", 0.35f));
    ctx->FillEllipse(D2D1::Ellipse({g.x + 4, g.y + 6}, 38.f * (1 - lift * 0.3f), 5.f), Brush(ctx, "groundCore", 0.35f));

    ctx->SetTransform(squash * global);

    // Traînée de mouvement : positions des deux images précédentes de la même animation.
    for (const auto& name : clip.trail) {
        const int idx = name == "bHand" ? 1 : 0;
        std::vector<D2D1_POINT_2F> pts;
        for (int back = 2; back >= 1; --back)
            if (frame_ - back >= 0) pts.push_back(Solve(Sample(clip, frame_ - back)).hand[idx]);
        pts.push_back(k.hand[idx]);
        const float dx = pts.back().x - pts.front().x, dy = pts.back().y - pts.front().y;
        if (pts.size() < 2 || std::sqrt(dx * dx + dy * dy) <= 14.f) continue;
        const float n = static_cast<float>(pts.size());
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            ctx->DrawLine(pts[i], pts[i + 1], Brush(ctx, "speed", 0.25f + 0.5f * i / n), 6.f + 12.f * i / n, roundStroke_.Get());
    }

    // Une forme au rendu anime (même ordre de couches que Painter.part() en Python).
    struct Layers {
        ComPtr<ID2D1Geometry> highlight;
        std::function<void()> band;     // dessiné dans la forme (bande de reflet, semelle…)
        ComPtr<ID2D1Geometry> details;  // traits fins
        bool rim = true;
    };
    const auto part = [&](const ComPtr<ID2D1Geometry>& geom, bool back, bool big, const Layers& L) {
        if (!geom) return;
        const D2D1_POINT_2F off = big ? shadowOffset_ : limbShadowOffset_;
        ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geom.Get()), nullptr);
        D2D1_RECT_F r{};
        geom->GetBounds(nullptr, &r);
        ctx->FillRectangle(D2D1::RectF(r.left - 4, r.top - 4, r.right + 4, r.bottom + 4),
                           Brush(ctx, back ? "backShadow" : "shadow"));                                   // 1. ombre
        fill(shifted(geom, {-off.x, -off.y}), Brush(ctx, back ? "backBase" : "base"));                  // 2. lumière
        if (L.band) L.band();                                                                           // 3. reflet
        if (L.rim) stroke(shifted(geom, {-rimOffset_.x, -rimOffset_.y}), Brush(ctx, "rim", back ? 0.55f : 0.9f),
                          rimWidth_);                                                                   // 4. liseré
        if (L.highlight && !back) fill(L.highlight, Brush(ctx, "highlight"));                           // 5. reflets
        if (L.details) stroke(L.details, Brush(ctx, "outline", 0.85f), innerLine_);                     // 6. détails
        ctx->PopLayer();
        stroke(geom, Brush(ctx, "outline"), outline_);                                                  // 7. trait
        stroke(shifted(geom, weightOffset_), Brush(ctx, "outline"), outline_ * weightRatio_);           //    côté ombre
    };
    const auto streak = [&](D2D1_POINT_2F p0, D2D1_POINT_2F p1, float r) {
        const D2D1_POINT_2F o{-r * 0.35f, -r * 0.35f};
        return Group(factory, {Capsule(factory, Add(Lerp(p0, p1, 0.18f), o), Add(Lerp(p0, p1, 0.55f), o), 2.6f, 2.0f),
                               Ellipse(factory, Add(Lerp(p0, p1, 0.70f), o), 1.8f, 1.8f)});
    };
    const auto limbBand = [&](D2D1_POINT_2F p0, D2D1_POINT_2F p1, float r, bool back) {
        return [&, p0, p1, r, back] {
            const D2D1_POINT_2F o{r * 0.25f, r * 0.2f};
            fill(Capsule(factory, Add(Lerp(p0, p1, 0.05f), o), Add(Lerp(p0, p1, 0.95f), o), r * 0.28f, r * 0.24f),
                 Brush(ctx, back ? "backBand" : "band"));
        };
    };
    const auto angleOf = [](D2D1_POINT_2F p0, D2D1_POINT_2F p1) {
        return std::atan2(p1.x - p0.x, p1.y - p0.y) * 180.f / kPi;
    };
    const auto joint = [&](D2D1_POINT_2F p, float ang, float r, bool back) {
        Layers L;
        L.rim = false;
        L.highlight = Ellipse(factory, Add(p, {-r * 0.35f, -r * 0.12f}), r * 0.32f, r * 0.09f, -ang);
        part(Ellipse(factory, p, r * 1.06f, r * 0.38f, -ang), back, false, L);
    };
    const auto hand = [&](int i, bool back) {
        const D2D1_POINT_2F h = k.hand[i];
        const float fo = k.fore[i], R = b.handR;
        const D2D1_POINT_2F td = Dir(fo + (i == 0 ? 55.f : -55.f));  // pouce vers le corps
        Layers L;
        L.highlight = Ellipse(factory, Add(h, {-5, -6}), 4.5f, 3.f, -30.f);
        L.details = Strokes(factory, {{Add(h, Rot(-fo, {-6, 4})), Add(h, Rot(-fo, {6, 4}))}});
        part(Group(factory, {Ellipse(factory, h, R, R * 0.92f, -fo), Capsule(factory, Add(h, td, R * 0.35f), Add(h, td, R * 1.05f), 5.5f, 4.5f)}),
             back, false, L);
    };
    const auto arm = [&](int i, bool back) {
        const D2D1_POINT_2F s = k.shoulder[i], e = k.elbow[i], h = k.hand[i];
        Layers up, fo;
        up.highlight = streak(s, e, b.upperArmR[0]);
        up.band = limbBand(s, e, b.upperArmR[0], back);
        fo.highlight = streak(e, h, b.foreArmR[0]);
        fo.band = limbBand(e, h, b.foreArmR[0], back);
        part(Capsule(factory, s, e, b.upperArmR[0], b.upperArmR[1]), back, false, up);
        part(Capsule(factory, e, h, b.foreArmR[0], b.foreArmR[1]), back, false, fo);
        joint(e, angleOf(e, h), b.foreArmR[0], back);
        hand(i, back);
        joint(s, angleOf(s, e), b.upperArmR[0] * 0.95f, back);
    };
    const auto leg = [&](int i, bool back) {
        const D2D1_POINT_2F hp = k.hip[i], kn = k.knee[i], an = k.ankle[i];
        Layers th, sh, ft;
        th.highlight = streak(hp, kn, b.thighR[0]);
        th.band = limbBand(hp, kn, b.thighR[0], back);
        sh.highlight = streak(kn, an, b.shinR[0]);
        sh.band = limbBand(kn, an, b.shinR[0], back);
        part(Capsule(factory, hp, kn, b.thighR[0], b.thighR[1]), back, false, th);
        part(Capsule(factory, kn, an, b.shinR[0], b.shinR[1]), back, false, sh);
        joint(kn, angleOf(kn, an), b.shinR[0], back);
        const float out = i == 0 ? -1.f : 1.f;  // pieds tournés vers l'extérieur
        const float fa = i == 0 ? P("aFoot") : P("bFoot");
        const float ang = i == 0 ? out * (8.f + fa) : out * 8.f + fa;
        const D2D1_POINT_2F fc = Add(an, {out * 9.f, 7.f});
        const float fw = b.foot[0], fh = b.foot[1];
        ft.band = [&, fc, ang, fw, fh] {  // semelle
            fill(Ellipse(factory, Add(fc, Rot(ang, {0, fh * 0.95f})), fw * 1.05f, fh * 0.75f, ang), Brush(ctx, "sole"));
        };
        ft.highlight = Ellipse(factory, Add(fc, Rot(ang, {-out * 6.f, -5.f})), 7.f, 2.5f, ang);
        ft.details = Strokes(factory, {{Add(fc, Rot(ang, {out * 6.f, -fh * 0.9f})), Add(fc, Rot(ang, {out * 9.f, 0}))}});
        part(Ellipse(factory, fc, fw, fh, ang), back, false, ft);
    };

    // Ordre : bras arrière, jambe arrière, jambe avant, cou, torse, tête, bras avant.
    arm(1, true);
    leg(1, true);
    leg(0, false);

    Layers neck;
    neck.rim = false;
    part(Capsule(factory, Add(k.neck, Rot(k.T, {0, 6})), Add(Add(k.neck, Rot(k.ha, {0, -16})), k.headOffset, 0.5f), 13.f, 12.f),
         true, false, neck);

    ComPtr<ID2D1PathGeometry> torso;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(factory->CreatePathGeometry(&torso)) && SUCCEEDED(torso->Open(&sink)) && b.torso.size() >= 4) {
        sink->BeginFigure(at(b.torso[0]), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i + 2 < b.torso.size(); i += 3)
            sink->AddBezier(D2D1::BezierSegment(at(b.torso[i]), at(b.torso[i + 1]), at(b.torso[i + 2])));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    Layers tl;
    tl.band = [&] {  // reflet d'horizon : bande sombre courbe sous la poitrine
        const D2D1_POINT_2F p0 = at({-52, -54}), p1 = at({0, -40}), p2 = at({52, -54});
        stroke(Strokes(factory, {{p0, Lerp(p0, p1, 0.6f), Lerp(p2, p1, 0.6f), p2}}), Brush(ctx, "band"), 9.f);
    };
    tl.details = Strokes(factory, {{at({-34, -96}), at({-30, -70}), at({26, -70}), at({30, -96})},
                                   {at({-4, -66}), at({-6, -44}), at({-6, -22}), at({-4, -6})}});
    tl.highlight = Group(factory, {Ellipse(factory, at({-27, -80}), 6.5f, 16.f, k.T + 18.f),
                                   Ellipse(factory, at({-20, -54}), 2.6f, 5.f, k.T + 18.f)});
    part(torso, false, true, tl);

    const float hr = b.headRadius, ha = k.ha;
    const D2D1_POINT_2F hc = k.hc;
    Layers hl;
    hl.band = [&] {  // horizon reflété sur la sphère chromée (dépasse : découpé net par la forme)
        const D2D1_POINT_2F cc = Add(hc, Rot(ha, {0, -hr * 1.25f}));
        stroke(Arc(factory, cc, hr * 1.62f, 35.f + ha, 145.f + ha), Brush(ctx, "band"), hr * 0.17f);
        stroke(Arc(factory, cc, hr * 1.62f + hr * 0.13f, 35.f + ha, 145.f + ha), Brush(ctx, "rim", 0.8f), 2.f);
    };
    hl.highlight = Group(factory, {Ellipse(factory, Add(hc, Rot(ha, {-26, -28})), 20.f, 10.5f, ha - 38.f),
                                   Ellipse(factory, Add(hc, Rot(ha, {-6, -44})), 4.5f, 4.5f),
                                   Ellipse(factory, Add(hc, Rot(ha, {-38, -6})), 3.f, 7.f, ha - 10.f)});
    part(Ellipse(factory, hc, hr, hr), false, true, hl);

    arm(0, false);

    // Effets et émotes : hors écrasement, à la position écrasée de la tête.
    ctx->SetTransform(global);
    DrawEmote(ctx, factory, clip.emote, squash.TransformPoint(hc));
    ctx->SetTransform(D2D1::IdentityMatrix());
}

void CartoonAvatar::DrawEmote(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const std::string& emote,
                              D2D1_POINT_2F hc) {
    if (emote.empty()) return;
    const float hr = body_.headRadius;
    const float topY = hc.y - hr;
    const int frame = frame_;
    auto* stroke = roundStroke_.Get();
    if (emote == "dots") {  // « … » qui apparaissent l'un après l'autre
        for (int i = 0; i < 3; ++i)
            if ((frame / 8) % 4 > i) {
                const float r = 4.5f + static_cast<float>(i);
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hc.x + 58.f + i * 16.f, topY + 10.f - i * 10.f), r, r),
                                 Brush(ctx, "emote"));
            }
    } else if (emote == "sound") {  // ondes sonores
        for (int i = 0; i < 3; ++i) {
            const float ph = std::fmod(frame / 16.f + i / 3.f, 1.f);
            if (auto arc = Arc(factory, {hc.x - hr - 4.f, hc.y - 6.f}, 14.f + ph * 26.f, 140.f, 220.f))
                ctx->DrawGeometry(arc.Get(), Brush(ctx, "emote", 1.f - ph), 3.2f, stroke);
        }
    } else if (emote == "notes") {  // notes de musique qui montent
        for (int i = 0; i < 2; ++i) {
            const float ph = std::fmod(frame / 24.f + i * 0.5f, 1.f);
            const float x = hc.x + 70.f - i * 12.f + std::sin(ph * 6.28f) * 6.f, y = hc.y - 10.f - ph * 60.f;
            auto* br = Brush(ctx, "emote", 1.f - ph);
            if (auto head = Ellipse(factory, {x, y}, 7.f, 5.f, -20.f)) ctx->FillGeometry(head.Get(), br);
            ctx->DrawLine({x + 6.f, y - 1.f}, {x + 6.f, y - 24.f}, br, 3.2f, stroke);
            ctx->DrawLine({x + 6.f, y - 24.f}, {x + 14.f, y - 18.f}, br, 3.2f, stroke);
        }
    } else if (emote == "sweat") {  // goutte de sueur
        const float ph = static_cast<float>(frame % 24) / 24.f;
        const float x = hc.x + hr * 0.75f, y = hc.y - hr * 0.55f + ph * 14.f;
        ComPtr<ID2D1PathGeometry> drop;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(factory->CreatePathGeometry(&drop)) || FAILED(drop->Open(&s))) return;
        s->BeginFigure({x, y - 12.f}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddBezier(D2D1::BezierSegment({x + 8.f, y - 2.f}, {x + 7.f, y + 7.f}, {x, y + 7.f}));
        s->AddBezier(D2D1::BezierSegment({x - 7.f, y + 7.f}, {x - 8.f, y - 2.f}, {x, y - 12.f}));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        ctx->FillGeometry(drop.Get(), Brush(ctx, "accent"));
        ctx->DrawGeometry(drop.Get(), Brush(ctx, "outline"), 2.f, stroke);
    } else if (emote == "emphasis") {  // traits d'emphase manga sur les accents de la phrase
        if (const int m = frame % 12; m >= 5 && m <= 7)
            for (float a : {-60.f, -30.f, 0.f}) {
                const float ca = std::cos(Rad(a - 90.f)), sa = std::sin(Rad(a - 90.f));
                const float r0 = hr + 10.f, r1 = hr + 24.f;
                ctx->DrawLine({hc.x + 18.f + ca * r0, hc.y + sa * r0}, {hc.x + 18.f + ca * r1, hc.y + sa * r1},
                              Brush(ctx, "emote"), 3.2f, stroke);
            }
    } else if (emote == "joy") {  // éclats de joie autour de la tête
        const float spots[3][3] = {{-hr - 22.f, -hr * 0.5f, 0.f}, {hr + 20.f, -hr * 0.7f, 20.f}, {hr + 30.f, hr * 0.1f, 40.f}};
        for (int i = 0; i < 3; ++i) {
            const float s = 0.6f + 0.4f * std::sin((frame + i * 5) * 0.5f);
            if (auto st = Star(factory, {hc.x + spots[i][0], hc.y + spots[i][1]}, 11.f * s, spots[i][2])) {
                ctx->FillGeometry(st.Get(), Brush(ctx, "highlight"));
                ctx->DrawGeometry(st.Get(), Brush(ctx, "accent", 0.9f), 1.4f, stroke);
            }
        }
    } else if (emote == "laugh") {  // traits de vibration de chaque côté
        if (frame < 26)
            for (float side : {-1.f, 1.f})
                for (int k = 0; k < 3; ++k) {
                    const float x = hc.x + side * (hr + 10.f + k * 7.f), y0 = hc.y - 18.f + k * 4.f;
                    ctx->DrawLine({x, y0}, {x + side * 4.f, y0 + 14.f - k * 3.f}, Brush(ctx, "emote"), 3.2f, stroke);
                }
    } else if (emote == "exclaim") {  // « ! » qui surgit
        if (frame >= 1) {
            const float s = std::min(1.f, frame / 4.f) * (frame < 22 ? 1.f : std::max(0.f, 1.f - (frame - 22) / 6.f));
            const float x = hc.x + hr * 1.08f, y = hc.y - hr * 0.35f;
            ctx->DrawLine({x, y - 26.f * s}, {x + 2.f, y - 6.f * s}, Brush(ctx, "accent", s), 6.f * s + 0.1f, stroke);
            ctx->FillEllipse(D2D1::Ellipse({x + 2.5f, y + 3.f}, 3.4f * s + 0.1f, 3.4f * s + 0.1f), Brush(ctx, "accent", s));
        }
    } else if (emote == "question") {  // « ? »
        if (frame >= 4) {
            const float s = std::min(1.f, (frame - 4) / 4.f);
            const float x = hc.x + hr * 0.9f, y = topY - 6.f;
            ComPtr<ID2D1PathGeometry> q;
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(factory->CreatePathGeometry(&q)) && SUCCEEDED(q->Open(&sk))) {
                sk->BeginFigure({x + 8.f * std::cos(Rad(200)), y - 16.f + 8.f * std::sin(Rad(200))}, D2D1_FIGURE_BEGIN_HOLLOW);
                AddArcBeziers(sk.Get(), {x, y - 16.f}, 8.f, Rad(200), Rad(400));
                sk->AddLine({x, y - 3.f});
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                ctx->DrawGeometry(q.Get(), Brush(ctx, "emote", s), 4.f, stroke);
            }
            ctx->FillEllipse(D2D1::Ellipse({x, y + 5.f}, 2.8f, 2.8f), Brush(ctx, "emote", s));
        }
    } else if (emote == "gloom") {  // lignes verticales de déprime (manga)
        for (int i = 0; i < 5; ++i) {
            const float x = hc.x - hr * 0.7f + i * hr * 0.35f, ln = 16.f + (i % 2) * 10.f;
            const float y = topY - 6.f - ((frame + i * 9) % 32) * 0.4f;
            ctx->DrawLine({x, y - ln}, {x, y}, Brush(ctx, "emote", 0.55f), 2.2f, stroke);
        }
    } else if (emote == "zzz") {
        for (int i = 0; i < 3; ++i) {
            const float ph = std::fmod(frame / 48.f + i / 3.f, 1.f);
            const float x = hc.x + hr * 0.6f + ph * 30.f, y = topY - ph * 50.f, s = 6.f + ph * 8.f;
            auto* br = Brush(ctx, "emote", 1.f - ph);
            if (auto z = Strokes(factory, {{{x, y}, {x + s, y}}, {{x + s, y}, {x, y + s}}, {{x, y + s}, {x + s, y + s}}}))
                ctx->DrawGeometry(z.Get(), br, 2.6f, stroke);
        }
    } else if (emote == "sparkle") {  // étincelle sur le chrome au sommet du geste
        if (frame >= 8 && frame <= 20) {
            const float s = 1.f - std::abs(frame - 13) / 7.f;
            if (auto st = Star(factory, {hc.x - hr * 0.55f, hc.y - hr * 0.6f}, 13.f * s, 15.f)) {
                ctx->FillGeometry(st.Get(), Brush(ctx, "highlight"));
                ctx->DrawGeometry(st.Get(), Brush(ctx, "accent", 0.9f), 1.2f, stroke);
            }
        }
    }
}

}  // namespace companion
