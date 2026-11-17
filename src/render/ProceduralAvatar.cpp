#include "render/ProceduralAvatar.hpp"

#include "core/Log.hpp"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Proportions du bonhomme chromé (unités arbitraires, Y vers le haut, il regarde vers -Z, la caméra) :
// grosse tête sphérique à peine détachée du torse, torse arrondi large aux épaules, bras épais.
constexpr float kHeadY = 0.88f;
constexpr float kHeadRadius = 0.40f;
constexpr float kShoulderX = 0.36f, kShoulderY = 0.22f;
constexpr float kUpperArmLen = 0.30f, kForearmLen = 0.28f;

XMFLOAT3 StateTint(AvatarState s) noexcept {
    switch (s) {
        case AvatarState::Idle: return {0.35f, 0.85f, 1.00f};
        case AvatarState::Listening: return {0.75f, 0.92f, 1.00f};
        case AvatarState::Studio: return {0.72f, 0.45f, 1.00f};
        case AvatarState::Escalating: return {1.00f, 0.62f, 0.22f};
        case AvatarState::Training: return {0.40f, 1.00f, 0.55f};
        case AvatarState::Gaming: return {0.55f, 0.62f, 0.70f};
    }
    return {1.f, 1.f, 1.f};
}

float Smooth(float current, float target, float dt, float tau) noexcept {
    return current + (target - current) * (1.f - std::exp(-dt / tau));
}

float SmoothStep(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Environnement "chrome Frutiger Aero" évalué pour une normale de la demi-sphère visible.
XMFLOAT3 ChromeEnvironment(float x, float y) noexcept {
    const float z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
    const float horizon = -0.30f + 0.10f * x;  // horizon bas et incliné : chrome gris uniforme
    // Ciel et sol mélangés en douceur autour de l'horizon : aucune arête sur les flancs du torse.
    const XMVECTOR sky = XMVectorLerp(XMVectorSet(0.74f, 0.75f, 0.77f, 0), XMVectorSet(0.40f, 0.41f, 0.44f, 0),
                                      SmoothStep(horizon, 1.f, y));
    const XMVECTOR ground = XMVectorLerp(XMVectorSet(0.16f, 0.16f, 0.18f, 0), XMVectorSet(0.34f, 0.35f, 0.37f, 0),
                                         SmoothStep(horizon, -1.f, y));
    XMVECTOR c = XMVectorLerp(ground, sky, SmoothStep(horizon - 0.15f, horizon + 0.15f, y));
    // Ligne d'horizon brillante
    const float line = std::exp(-std::pow((y - horizon) * 18.f, 2.f));
    c = XMVectorAdd(c, XMVectorReplicate(0.18f * line));
    // Spéculaires (key light haut-gauche + fill bas-droite)
    const XMVECTOR n = XMVectorSet(x, y, z, 0);
    const float key = std::pow(std::max(0.f, XMVectorGetX(XMVector3Dot(n, XMVector3Normalize(XMVectorSet(-0.5f, 0.65f, 0.6f, 0))))), 70.f);
    const float fill = std::pow(std::max(0.f, XMVectorGetX(XMVector3Dot(n, XMVector3Normalize(XMVectorSet(0.6f, -0.3f, 0.75f, 0))))), 18.f);
    c = XMVectorAdd(c, XMVectorReplicate(1.3f * key + 0.25f * fill));
    // Assombrissement du bord : donne le volume "métal liquide"
    c = XMVectorScale(c, 0.50f + 0.50f * std::pow(z, 0.6f));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVectorSaturate(c));
    return out;
}

ComPtr<ID3DBlob> CompileShader(const std::filesystem::path& file, const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
    flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                          flags, 0, &code, &errors);
    if (FAILED(hr)) {
        log::Info("Shader {} ({}): {}", log::Narrow(file.wstring()), entry,
                  errors ? static_cast<const char*>(errors->GetBufferPointer()) : "fichier introuvable");
        return nullptr;
    }
    return code;
}

// Charge un PNG/JPEG en RGBA8 via WIC (composant Windows, aucune dépendance externe).
// COM doit être initialisé sur le thread appelant (c'est le cas du thread principal).
bool LoadImageRgba(const std::filesystem::path& file, UINT& w, UINT& h, std::vector<std::uint32_t>& pixels) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    pixels.resize(static_cast<size_t>(w) * h);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(pixels.data())));
}

}  // namespace

// ---------------------------------------------------------------------------
// Géométrie
// ---------------------------------------------------------------------------
ProceduralAvatar::Mesh ProceduralAvatar::Lathe(const std::vector<ProfilePoint>& profile, int slices, float zScale) {
    Mesh m;
    m.baseVertex = static_cast<INT>(vertices_.size());
    m.indexStart = static_cast<UINT>(indices_.size());

    for (const auto& p : profile) {
        for (int s = 0; s < slices; ++s) {
            const float a = 2.f * kPi * static_cast<float>(s) / static_cast<float>(slices);
            const float ca = std::cos(a), sa = std::sin(a);
            XMFLOAT3 n{p.nr * ca, p.ny, p.nr * sa / zScale};  // normale d'une surface écrasée en Z
            XMStoreFloat3(&n, XMVector3Normalize(XMLoadFloat3(&n)));
            vertices_.push_back({{p.r * ca, p.y, p.r * sa * zScale}, n});
        }
    }
    const auto rings = static_cast<int>(profile.size());
    for (int r = 0; r + 1 < rings; ++r) {
        const bool topPole = profile[static_cast<size_t>(r)].r < 1e-5f;
        const bool bottomPole = profile[static_cast<size_t>(r) + 1].r < 1e-5f;
        for (int s = 0; s < slices; ++s) {
            const auto i0 = static_cast<std::uint16_t>(r * slices + s);
            const auto i1 = static_cast<std::uint16_t>(r * slices + (s + 1) % slices);
            const auto i2 = static_cast<std::uint16_t>((r + 1) * slices + s);
            const auto i3 = static_cast<std::uint16_t>((r + 1) * slices + (s + 1) % slices);
            // Aux pôles, un seul triangle par quartier (pas de triangles dégénérés).
            if (!topPole) indices_.insert(indices_.end(), {i0, i1, i3});
            if (!bottomPole) indices_.insert(indices_.end(), {i0, i3, i2});
        }
    }
    m.indexCount = static_cast<UINT>(indices_.size()) - m.indexStart;
    return m;
}

ProceduralAvatar::Mesh ProceduralAvatar::AppendEllipsoid(float a, float b, float c, int slices, int stacks) {
    std::vector<ProfilePoint> profile;
    for (int i = 0; i <= stacks; ++i) {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(stacks);  // 0 = sommet
        const float sp = std::sin(phi), cp = std::cos(phi);
        float nr = sp / a, ny = cp / b;
        const float len = std::sqrt(nr * nr + ny * ny);
        profile.push_back({a * sp, b * cp, nr / len, ny / len});
    }
    return Lathe(profile, slices, c / a);
}

ProceduralAvatar::Mesh ProceduralAvatar::AppendCapsule(float rTop, float rBottom, float yTop, float yBottom,
                                                        int slices, int capRings) {
    // Cône arrondi exact : deux sphères reliées par un flanc tangent aux deux. Avec rTop > rBottom,
    // le flanc s'incline (forme en cloche) et sa normale pointe un peu vers le haut : l'angle de
    // raccord n'est pas l'équateur mais pi/2 - alpha, avec sin(alpha) = (rTop - rBottom) / longueur.
    const float len = std::max(yTop - yBottom, 1e-4f);
    const float alpha = std::asin(std::clamp((rTop - rBottom) / len, -0.99f, 0.99f));
    const float seam = 0.5f * kPi - alpha;  // angle (depuis le pôle haut) de la normale du flanc
    std::vector<ProfilePoint> profile;
    for (int k = 0; k <= capRings; ++k) {  // calotte haute : pôle -> raccord
        const float phi = seam * static_cast<float>(k) / static_cast<float>(capRings);
        profile.push_back({rTop * std::sin(phi), yTop + rTop * std::cos(phi), std::sin(phi), std::cos(phi)});
    }
    for (int k = 0; k <= capRings; ++k) {  // calotte basse : raccord -> pôle
        const float phi = seam + (kPi - seam) * static_cast<float>(k) / static_cast<float>(capRings);
        profile.push_back({rBottom * std::sin(phi), yBottom + rBottom * std::cos(phi), std::sin(phi), std::cos(phi)});
    }
    return Lathe(profile, slices);
}

void ProceduralAvatar::BuildGeometry() {
    vertices_.clear();
    indices_.clear();
    head_ = AppendEllipsoid(kHeadRadius, kHeadRadius, kHeadRadius, 22, 16);  // sans visage
    torso_ = AppendCapsule(0.42f, 0.28f, 0.02f, -0.45f, 18, 5);               // en cloche : épaules larges
    upperArm_ = AppendCapsule(0.15f, 0.14f, 0.f, -kUpperArmLen, 12, 4);       // pivot = épaule
    forearm_ = AppendCapsule(0.14f, 0.13f, 0.f, -kForearmLen, 12, 4);         // pivot = coude
    log::Info("Avatar: {} triangles, {} sommets", TriangleCount(), vertices_.size());
}

bool ProceduralAvatar::CreateMatCapTexture(ID3D11Device* device, const std::filesystem::path& matcapFile) {
    constexpr int kSize = 256;
    UINT width = kSize, height = kSize;
    std::vector<std::uint32_t> pixels;
    const bool loaded = LoadImageRgba(matcapFile, width, height, pixels);
    if (loaded) {
        log::Info("Avatar: MatCap chargée ({}x{})", width, height);
    } else {
        log::Info("Avatar: {} introuvable, MatCap chrome générée", log::Narrow(matcapFile.wstring()));
        width = height = kSize;
        pixels.assign(kSize * kSize, 0);
    }
    for (int py = 0; !loaded && py < kSize; ++py) {
        for (int px = 0; px < kSize; ++px) {
            float x = (static_cast<float>(px) + 0.5f) / kSize * 2.f - 1.f;
            float y = 1.f - (static_cast<float>(py) + 0.5f) / kSize * 2.f;
            const float r = std::sqrt(x * x + y * y);
            if (r > 0.999f) { x *= 0.999f / r; y *= 0.999f / r; }  // hors disque : bord étendu
            const XMFLOAT3 c = ChromeEnvironment(x, y);
            const auto to8 = [](float v) { return static_cast<std::uint32_t>(std::lround(v * 255.f)); };
            pixels[static_cast<size_t>(py) * kSize + px] = to8(c.x) | (to8(c.y) << 8) | (to8(c.z) << 16) | 0xFF000000u;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &init, &tex))) return false;
    return SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &matcapSrv_));
}

bool ProceduralAvatar::Initialize(ID3D11Device* device, const std::filesystem::path& shaderFile,
                                  const std::filesystem::path& matcapFile) {
    BuildGeometry();

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(vertices_.size() * sizeof(Vertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{vertices_.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, &vb_))) return false;

    bd.ByteWidth = static_cast<UINT>(indices_.size() * sizeof(std::uint16_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = indices_.data();
    if (FAILED(device->CreateBuffer(&bd, &init, &ib_))) return false;

    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(FrameCB);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &frameCb_))) return false;
    bd.ByteWidth = sizeof(ObjectCB);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &objectCb_))) return false;

    auto vsBlob = CompileShader(shaderFile, "VSMain", "vs_5_0");
    auto psBlob = CompileShader(shaderFile, "PSMain", "ps_5_0");
    if (!vsBlob || !psBlob) return false;
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_)))
        return false;
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_)))
        return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout_)))
        return false;

    if (!CreateMatCapTexture(device, matcapFile)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // < 2000 triangles : le culling ne rapporte rien, évite tout souci d'ordre
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &raster_))) return false;

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(device->CreateDepthStencilState(&dd, &depth_))) return false;

    D3D11_BLEND_DESC bl{};  // blending pré-multiplié : ONE / INV_SRC_ALPHA
    auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&bl, &blend_));
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void ProceduralAvatar::Update(float dt, float voiceRms) noexcept {
    dt = std::clamp(dt, 0.f, 0.1f);
    time_ += dt;
    rmsEnv_ = Smooth(rmsEnv_, voiceRms, dt, voiceRms > rmsEnv_ ? 0.025f : 0.18f);
    talk_ = std::clamp(rmsEnv_ * 4.f, 0.f, 1.f);  // RMS parole typique 0.05..0.25

    const XMFLOAT3 target = StateTint(state_);
    tint_.x = Smooth(tint_.x, target.x, dt, 0.25f);
    tint_.y = Smooth(tint_.y, target.y, dt, 0.25f);
    tint_.z = Smooth(tint_.z, target.z, dt, 0.25f);
    opacity_ = Smooth(opacity_, state_ == AvatarState::Gaming ? 0.55f : 1.f, dt, 0.3f);
    waveTimer_ = std::max(0.f, waveTimer_ - dt);
    waveAmt_ = Smooth(waveAmt_, waveTimer_ > 0.f ? 1.f : 0.f, dt, 0.18f);
}

void ProceduralAvatar::DrawPart(ID3D11DeviceContext* ctx, const Mesh& mesh, FXMMATRIX world, float emissive,
                                float tintMix) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(objectCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* cb = static_cast<ObjectCB*>(mapped.pData);
    XMStoreFloat4x4(&cb->world, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb->worldInvTranspose, XMMatrixInverse(nullptr, world));  // (W^-1)^T, re-transposée
    cb->part = {emissive, tintMix, 0.f, 0.f};
    ctx->Unmap(objectCb_.Get(), 0);
    ctx->DrawIndexed(mesh.indexCount, mesh.indexStart, mesh.baseVertex);
}

void ProceduralAvatar::Render(ID3D11DeviceContext* ctx, float aspect) {
    const float t = time_;
    const float breath = std::sin(2.f * kPi * 0.22f * t);
    const float floatY = 0.035f * std::sin(2.f * kPi * 0.18f * t);

    // Caméra
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.f, 0.36f, -4.4f, 1.f), XMVectorSet(0.f, 0.33f, 0.f, 1.f),
                                           XMVectorSet(0.f, 1.f, 0.f, 0.f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(34.f), aspect, 0.1f, 20.f);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(frameCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* cb = static_cast<FrameCB*>(mapped.pData);
        XMStoreFloat4x4(&cb->viewProj, XMMatrixTranspose(view * proj));
        XMStoreFloat4x4(&cb->view, XMMatrixTranspose(view));
        cb->tint = {tint_.x, tint_.y, tint_.z, 1.f};
        cb->params = {t, talk_, 0.45f + 0.4f * talk_, opacity_};  // liseré d'état discret sur le chrome gris
        ctx->Unmap(frameCb_.Get(), 0);
    }

    const UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = vb_.Get();
    ID3D11Buffer* cbs[] = {frameCb_.Get(), objectCb_.Get()};
    ID3D11ShaderResourceView* srv = matcapSrv_.Get();
    ID3D11SamplerState* smp = sampler_.Get();
    constexpr float blendFactor[4] = {0, 0, 0, 0};
    ctx->IASetInputLayout(layout_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(ib_.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetDepthStencilState(depth_.Get(), 0);
    ctx->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);

    const XMMATRIX body = XMMatrixTranslation(0.f, floatY, 0.f);

    // Torse : respiration (gonfle en Y, s'affine légèrement en XZ)
    const XMMATRIX torso = XMMatrixScaling(1.f - 0.01f * breath, 1.f + 0.02f * breath, 1.f - 0.01f * breath) * body;
    DrawPart(ctx, torso_, torso, 0.f, 0.12f);

    // Tête flottante : rebond sur la voix, inclinaison conversationnelle
    const float bounce = 0.06f * talk_ * std::abs(std::sin(t * 9.f)) + 0.015f * breath;
    const float tilt = 0.05f * std::sin(t * 1.3f) + 0.09f * talk_ * std::sin(t * 4.7f);
    const float nod = -0.04f + 0.08f * talk_ * std::sin(t * 6.1f);
    const XMMATRIX head = XMMatrixRotationRollPitchYaw(nod, 0.12f * std::sin(t * 0.7f), tilt) *
                          XMMatrixTranslation(0.f, kHeadY + bounce, 0.f) * body;
    DrawPart(ctx, head_, head, 0.f, 0.1f);

    // Bras : épaule (roll = écartement, pitch = balancier) puis coude.
    // Salut : le bras droit du personnage (à gauche de l'écran) se lève, l'avant-bras oscille.
    for (float side : {-1.f, 1.f}) {
        const float w = side < 0.f ? waveAmt_ : 0.f;
        const float rest = 1.f - w;
        const float phase = side * 1.7f;
        const float spread = (0.10f + 0.30f * talk_ + 0.02f * breath) * rest + 2.6f * w;
        const float swing = (0.45f * talk_ * std::sin(t * 6.f + phase) + 0.04f * std::sin(t * 0.9f + phase)) * rest;
        const float elbow = (0.2f + talk_ * (0.5f + 0.4f * std::sin(t * 7.f + phase))) * rest;
        const float waveForearm = w * (0.5f + 0.38f * std::sin(t * 9.f));  // avant-bras vertical qui bat

        const XMMATRIX shoulder = XMMatrixRotationX(swing) * XMMatrixRotationZ(side * spread) *
                                  XMMatrixTranslation(side * kShoulderX, kShoulderY, 0.f) * body;
        DrawPart(ctx, upperArm_, shoulder, 0.f, 0.12f);
        const XMMATRIX forearm = XMMatrixRotationX(elbow) * XMMatrixRotationZ(side * waveForearm) *
                                 XMMatrixTranslation(0.f, -kUpperArmLen, 0.f) * shoulder;
        DrawPart(ctx, forearm_, forearm, 0.f, 0.12f);
    }
}

}  // namespace companion
