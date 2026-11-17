#include "render/CartoonAvatar.hpp"

#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
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
// Demi-cercle de `from` à `to` autour de `c`, en passant par `mid` (sens horaire à l'écran).
void HalfArc(ID2D1GeometrySink* sink, D2D1_POINT_2F mid, D2D1_POINT_2F to, float r) {
    sink->AddArc(D2D1::ArcSegment(mid, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
}

// Capsule effilée : même construction que capsule() dans tools/avatar_preview.py.
ComPtr<ID2D1Geometry> Capsule(ID2D1Factory1* f, D2D1_POINT_2F p0, D2D1_POINT_2F p1, float r0, float r1) {
    const float dx = p1.x - p0.x, dy = p1.y - p0.y;
    const float ang = std::atan2(dy, dx);
    auto on = [](D2D1_POINT_2F c, float r, float a) { return D2D1::Point2F(c.x + r * std::cos(a), c.y + r * std::sin(a)); };
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure(on(p0, r0, ang + kPi / 2), D2D1_FIGURE_BEGIN_FILLED);
    HalfArc(sink.Get(), on(p0, r0, ang + kPi), on(p0, r0, ang + 3 * kPi / 2), r0);
    sink->AddLine(on(p1, r1, ang - kPi / 2));
    HalfArc(sink.Get(), on(p1, r1, ang), on(p1, r1, ang + kPi / 2), r1);
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
        for (const auto& [k, v] : j.at("rest").items()) rest_[k] = v.get<float>();
        for (const auto& [name, c] : j.at("clips").items()) {
            Clip clip;
            clip.frames = c.at("frames");
            clip.loop = c.value("loop", true);
            clip.then = c.value("then", std::string{"idle"});
            clip.emote = c.value("emote", std::string{});
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
        case AvatarState::Studio: return "studio";
        case AvatarState::Escalating: return "think";
        case AvatarState::Training: return "train";
        default: return "idle";
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
    std::string next = clip_;
    if (waveRequested_ && clips_.contains("wave")) {
        next = "wave";
        waveRequested_ = false;
    } else if (!oneShotRunning) {
        next = rmsEnv_ > 0.03f && clips_.contains("talk") ? "talk" : ClipForState();
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

void CartoonAvatar::Render(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, float scale) {
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
    const bool dim = state_ == AvatarState::Gaming;
    if (dim) ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                   D2D1::IdentityMatrix(), 0.55f), nullptr);
    Draw(ctx, factory, pose, scale);
    if (dim) ctx->PopLayer();
    dirty_ = false;
}

void CartoonAvatar::Draw(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const Pose& pose, float scale) {
    const auto P = [&](const char* ch) { const auto it = pose.find(ch); return it != pose.end() ? it->second : 0.f; };
    const Body& b = body_;

    // Écrasement / étirement autour du point au sol, puis mise à l'échelle DPI.
    const D2D1_POINT_2F g{hips_.x, ground_};
    const float sq = P("sq");
    const auto global = D2D1::Matrix3x2F::Scale(scale, scale);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-g.x, -g.y) * D2D1::Matrix3x2F::Scale(1.f + sq * 0.6f, 1.f - sq) *
                      D2D1::Matrix3x2F::Translation(g.x, g.y) * global);

    const D2D1_POINT_2F H{hips_.x + P("x"), hips_.y + P("y") - rmsEnv_ * 30.f};  // petit rebond sur la voix
    const float T = P("torso");
    const auto at = [&](D2D1_POINT_2F local) { return Add(H, Rot(T, local)); };

    // Une forme : aplat + ombre en croissant + reflet + contour (même algorithme que cel() en Python).
    const auto cel = [&](const ComPtr<ID2D1Geometry>& geom, const char* base, const char* shadow, D2D1_POINT_2F off,
                         const ComPtr<ID2D1Geometry>& highlight) {
        if (!geom) return;
        ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geom.Get()), nullptr);
        D2D1_RECT_F r{};
        geom->GetBounds(nullptr, &r);
        ctx->FillRectangle(D2D1::RectF(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2), Brush(ctx, shadow));
        ComPtr<ID2D1TransformedGeometry> lit;
        factory->CreateTransformedGeometry(geom.Get(), D2D1::Matrix3x2F::Translation(-off.x, -off.y), &lit);
        if (lit) ctx->FillGeometry(lit.Get(), Brush(ctx, base));
        if (highlight) ctx->FillGeometry(highlight.Get(), Brush(ctx, "highlight"));
        ctx->PopLayer();
        ctx->DrawGeometry(geom.Get(), Brush(ctx, "outline"), outline_, roundStroke_.Get());
    };
    const auto streak = [&](D2D1_POINT_2F p0, D2D1_POINT_2F p1, float r) {
        const D2D1_POINT_2F o{-r * 0.35f, -r * 0.35f};
        return Capsule(factory, Add(Lerp(p0, p1, 0.18f), o), Add(Lerp(p0, p1, 0.55f), o), 2.6f, 2.0f);
    };

    const auto drawArm = [&](bool front) {
        const char* base = front ? "base" : "backBase";
        const char* shade = front ? "shadow" : "backShadow";
        const D2D1_POINT_2F s = at(front ? b.shoulderA : b.shoulderB);
        const float up = (front ? P("aU") : P("bU")) + T;
        const float fo = up + (front ? P("aF") : P("bF"));
        const D2D1_POINT_2F e = Add(s, Dir(up), b.upperArm);
        const D2D1_POINT_2F h = Add(e, Dir(fo), b.foreArm);
        cel(Capsule(factory, s, e, b.upperArmR[0], b.upperArmR[1]), base, shade, limbShadowOffset_,
            front ? streak(s, e, b.upperArmR[0]) : nullptr);
        cel(Capsule(factory, e, h, b.foreArmR[0], b.foreArmR[1]), base, shade, limbShadowOffset_,
            front ? streak(e, h, b.foreArmR[0]) : nullptr);
        cel(Ellipse(factory, h, b.handR, b.handR), base, shade, limbShadowOffset_,
            front ? Ellipse(factory, Add(h, {-5, -6}), 4.5f, 3.f, -30.f) : nullptr);
    };
    const auto drawLeg = [&](bool front) {
        const char* base = front ? "base" : "backBase";
        const char* shade = front ? "shadow" : "backShadow";
        const D2D1_POINT_2F hp = Add(H, front ? b.hipA : b.hipB);
        const float th = front ? P("aT") : P("bT");
        const float sh = th + (front ? P("aS") : P("bS"));
        const float fa = front ? P("aFoot") : P("bFoot");
        const D2D1_POINT_2F kn = Add(hp, Dir(th), b.thigh);
        const D2D1_POINT_2F an = Add(kn, Dir(sh), b.shin);
        cel(Capsule(factory, hp, kn, b.thighR[0], b.thighR[1]), base, shade, limbShadowOffset_,
            front ? streak(hp, kn, b.thighR[0]) : nullptr);
        cel(Capsule(factory, kn, an, b.shinR[0], b.shinR[1]), base, shade, limbShadowOffset_,
            front ? streak(kn, an, b.shinR[0]) : nullptr);
        const float out = front ? -1.f : 1.f;  // pieds tournés vers l'extérieur
        const float ang = front ? out * (8.f + fa) : out * 8.f + fa;
        cel(Ellipse(factory, Add(an, {out * 9.f, 7.f}), b.foot[0], b.foot[1], ang), base, shade, limbShadowOffset_, nullptr);
    };

    // Ordre : bras arrière, jambes, torse, tête, bras avant.
    drawArm(false);
    drawLeg(false);
    drawLeg(true);

    ComPtr<ID2D1PathGeometry> torso;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(factory->CreatePathGeometry(&torso)) && SUCCEEDED(torso->Open(&sink)) && body_.torso.size() >= 4) {
        sink->BeginFigure(at(body_.torso[0]), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i + 2 < body_.torso.size(); i += 3)
            sink->AddBezier(D2D1::BezierSegment(at(body_.torso[i]), at(body_.torso[i + 1]), at(body_.torso[i + 2])));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    cel(torso, "base", "shadow", shadowOffset_, Ellipse(factory, at({-27, -78}), 7.f, 17.f, T + 18.f));

    const D2D1_POINT_2F neck = at({0, b.neckY});
    const float ha = T + P("head");
    const D2D1_POINT_2F hc = Add(neck, Rot(ha, {0, -(b.headRadius + b.headGap)}));
    cel(Ellipse(factory, hc, b.headRadius, b.headRadius), "base", "shadow", shadowOffset_,
        Group(factory, {Ellipse(factory, Add(hc, Rot(ha, {-26, -28})), 20.f, 10.5f, ha - 38.f),
                        Ellipse(factory, Add(hc, Rot(ha, {-6, -44})), 4.5f, 4.5f)}));

    drawArm(true);

    // Émotes : hors écrasement, pour rester nettes (position de la tête recalculée dans ce repère).
    const auto squash = D2D1::Matrix3x2F::Translation(-g.x, -g.y) * D2D1::Matrix3x2F::Scale(1.f + sq * 0.6f, 1.f - sq) *
                        D2D1::Matrix3x2F::Translation(g.x, g.y);
    ctx->SetTransform(global);
    auto find = [&](const std::string& n) -> const Clip& { return clips_.contains(n) ? clips_.at(n) : clips_.at("idle"); };
    DrawEmote(ctx, factory, find(clip_).emote, squash.TransformPoint(hc));
    ctx->SetTransform(D2D1::IdentityMatrix());
}

void CartoonAvatar::DrawEmote(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const std::string& emote,
                              D2D1_POINT_2F hc) {
    if (emote.empty()) return;
    const float hr = body_.headRadius;
    const float topY = hc.y - hr;
    const int frame = frame_;
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
            const D2D1_POINT_2F c{hc.x - hr - 4.f, hc.y - 6.f};
            const float r = 14.f + ph * 26.f;
            ComPtr<ID2D1PathGeometry> arc;
            ComPtr<ID2D1GeometrySink> s;
            if (FAILED(factory->CreatePathGeometry(&arc)) || FAILED(arc->Open(&s))) continue;
            s->BeginFigure(D2D1::Point2F(c.x + r * std::cos(Rad(140)), c.y + r * std::sin(Rad(140))), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + r * std::cos(Rad(220)), c.y + r * std::sin(Rad(220))),
                                       D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
            s->Close();
            ctx->DrawGeometry(arc.Get(), Brush(ctx, "emote", 1.f - ph), 3.2f, roundStroke_.Get());
        }
    } else if (emote == "notes") {  // notes de musique qui montent
        for (int i = 0; i < 2; ++i) {
            const float ph = std::fmod(frame / 24.f + i * 0.5f, 1.f);
            const float x = hc.x + 70.f - i * 12.f + std::sin(ph * 6.28f) * 6.f, y = hc.y - 10.f - ph * 60.f;
            auto* br = Brush(ctx, "emote", 1.f - ph);
            if (auto head = Ellipse(factory, {x, y}, 7.f, 5.f, -20.f)) ctx->FillGeometry(head.Get(), br);
            ctx->DrawLine({x + 6.f, y - 1.f}, {x + 6.f, y - 24.f}, br, 3.2f, roundStroke_.Get());
            ctx->DrawLine({x + 6.f, y - 24.f}, {x + 14.f, y - 18.f}, br, 3.2f, roundStroke_.Get());
        }
    } else if (emote == "sweat") {  // goutte de sueur anime
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
        ctx->DrawGeometry(drop.Get(), Brush(ctx, "outline"), 2.f, roundStroke_.Get());
    }
}

}  // namespace companion
