#pragma once
// CartoonAvatar : le compagnon en 2D dessiné, entre l'anime et le cartoon, animé à 24 images/s.
//
// Tout le personnage est décrit dans assets/avatar/rig.json (squelette, formes, palette, animations).
// Le même fichier est lu par tools/avatar_preview.py, qui rend les animations en vidéo pour les régler
// sans recompiler. Le dessin reproduit le même algorithme :
//   - squelette 2D en cinématique directe (hanches -> torse -> tête / bras ; hanches -> jambes) ;
//   - chaque forme : aplat, ombre en croissant (forme moins la forme décalée vers la lumière),
//     reflet blanc net, contour sombre (cel shading façon anime) ;
//   - écrasement / étirement autour du point au sol (cartoon) ; émotes (points, ondes, notes, sueur).
// Cadence : une nouvelle image tous les 1/24 s (« on ones ») ; en jeu, 12 i/s (« on twos »).

#include <d2d1_1.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace companion {

enum class AvatarState : std::uint8_t {
    Idle,        // repos : respiration
    Listening,   // l'utilisateur parle : main à l'oreille, ondes sonores
    Studio,      // FL Studio : la tête suit le rythme, notes de musique
    Escalating,  // recherche / Claude : main au menton, « … »
    Training,    // auto-entraînement : footing sur place, goutte de sueur
    Gaming,      // jeu détecté : repos, semi-transparent, 12 i/s
};

class CartoonAvatar {
public:
    bool Load(const std::filesystem::path& rigJson);

    void SetState(AvatarState s) noexcept { state_ = s; }
    void Wave() noexcept { waveRequested_ = true; }  // coucou (une fois)

    // Avance l'horloge. Retourne true quand une nouvelle image doit être dessinée (24 ou 12 i/s).
    bool Update(float dtSeconds, float voiceRms) noexcept;

    void Render(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, float scale);

    [[nodiscard]] float Width() const noexcept { return canvas_[0]; }
    [[nodiscard]] float Height() const noexcept { return canvas_[1]; }

private:
    using Pose = std::map<std::string, float>;
    struct Key { int frame; float value; std::string ease; };
    struct Clip {
        int frames = 24;
        bool loop = true;
        std::string then = "idle";
        std::string emote;
        std::map<std::string, std::vector<Key>> keys;
    };
    struct Body {
        float headRadius = 64, headGap = 6, neckY = -108;
        std::vector<D2D1_POINT_2F> torso;
        D2D1_POINT_2F shoulderA{}, shoulderB{}, hipA{}, hipB{};
        float upperArm = 52, foreArm = 48, handR = 16, thigh = 58, shin = 56;
        std::array<float, 2> upperArmR{}, foreArmR{}, thighR{}, shinR{}, foot{};
    };

    Pose Sample(const Clip& clip, int frame) const;
    std::string ClipForState() const;
    void Draw(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const Pose& pose, float scale);
    void DrawEmote(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const std::string& emote, D2D1_POINT_2F headCenter);
    ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* ctx, const std::string& name, float alpha = 1.f);

    // Données du rig
    float fps_ = 24.f;
    std::array<float, 2> canvas_{320, 420};
    D2D1_POINT_2F hips_{160, 262};
    float ground_ = 395;
    Body body_;
    std::map<std::string, D2D1_COLOR_F> palette_;
    float outline_ = 4.f;
    D2D1_POINT_2F shadowOffset_{8, 7}, limbShadowOffset_{5, 4};
    Pose rest_;
    std::map<std::string, Clip> clips_;

    // Lecture
    AvatarState state_ = AvatarState::Idle;
    bool waveRequested_ = true;  // il salue au démarrage
    std::string clip_ = "idle", prevClip_;
    int frame_ = 0, prevFrame_ = 0, blend_ = 0;  // fondu de 3 images entre deux animations
    float clock_ = 0.f;
    float rmsEnv_ = 0.f;
    bool dirty_ = true;

    // Ressources Direct2D (liées au contexte)
    ID2D1DeviceContext* brushCtx_ = nullptr;
    std::map<std::string, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle1> roundStroke_;
};

}  // namespace companion
