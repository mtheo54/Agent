#pragma once
// ProceduralAvatar : bonhomme chromé gris façon années 2000, généré en C++ (< 2000 triangles).
//
//   Grosse tête sphérique sans visage, torse arrondi large aux épaules, bras épais en deux
//   segments (épaule / coude). Geste de salut : bras levé, avant-bras qui oscille (Wave()). Texture MatCap : assets/textures/matcap_chrome.png
//   (256x256, chargée via WIC) ; si elle manque, un chrome équivalent est généré en C++.
//
// Animation : respiration et flottement sinusoïdaux au repos ; rebond de tête, inclinaison
// et gestuelle des bras pilotés par l'enveloppe RMS de la voix (attaque 25 ms / relâche 180 ms).

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace companion {

enum class AvatarState : std::uint8_t {
    Idle,         // Veille : cyan
    Listening,    // l'utilisateur parle (push-to-talk) : bleu glacier
    Studio,       // Écoute Studio (FL Studio) : violet
    Escalating,   // Escalade Claude en cours : ambre
    Training,     // Auto-Training : vert
    Gaming,       // Jeu détecté : gris discret, opacité réduite
};

class ProceduralAvatar {
public:
    bool Initialize(ID3D11Device* device, const std::filesystem::path& shaderFile,
                    const std::filesystem::path& matcapFile);
    void SetState(AvatarState s) noexcept { state_ = s; }
    void Wave(float seconds = 2.8f) noexcept { waveTimer_ = seconds; }  // coucou
    [[nodiscard]] AvatarState State() const noexcept { return state_; }

    void Update(float dtSeconds, float voiceRms) noexcept;
    void Render(ID3D11DeviceContext* ctx, float aspect);

    [[nodiscard]] std::uint32_t TriangleCount() const noexcept { return static_cast<std::uint32_t>(indices_.size() / 3); }

private:
    struct Vertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 nrm;
    };
    struct Mesh {
        UINT indexStart = 0;
        UINT indexCount = 0;
        INT baseVertex = 0;
    };
    struct ProfilePoint {
        float r, y, nr, ny;  // rayon, hauteur, normale 2D (radiale, verticale)
    };
    struct alignas(16) FrameCB {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4 tint;
        DirectX::XMFLOAT4 params;
    };
    struct alignas(16) ObjectCB {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 worldInvTranspose;
        DirectX::XMFLOAT4 part;
    };

    Mesh Lathe(const std::vector<ProfilePoint>& profile, int slices, float zScale = 1.f);
    Mesh AppendEllipsoid(float a, float b, float c, int slices, int stacks);
    Mesh AppendCapsule(float rTop, float rBottom, float yTop, float yBottom, int slices, int capRings);
    void BuildGeometry();
    bool CreateMatCapTexture(ID3D11Device* device, const std::filesystem::path& matcapFile);
    void DrawPart(ID3D11DeviceContext* ctx, const Mesh& mesh, DirectX::FXMMATRIX world, float emissive, float tintMix);

    std::vector<Vertex> vertices_;
    std::vector<std::uint16_t> indices_;
    Mesh head_, torso_, upperArm_, forearm_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_, ib_, frameCb_, objectCb_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> matcapSrv_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;

    // État d'animation
    AvatarState state_ = AvatarState::Idle;
    float time_ = 0.f;
    float rmsEnv_ = 0.f;
    float talk_ = 0.f;
    float opacity_ = 1.f;
    float waveTimer_ = 0.f;
    float waveAmt_ = 0.f;
    DirectX::XMFLOAT3 tint_{0.35f, 0.85f, 1.f};
};

}  // namespace companion
