#pragma once
// CartoonAvatar : le compagnon en 2D dessiné, entre l'anime et le cartoon, animé à 24 images/s.
//
// Tout le personnage est décrit dans assets/avatar/rig.json (squelette, formes, palette, animations).
// Le même fichier est lu par tools/avatar_preview.py, qui rend les animations en vidéo pour les régler
// sans recompiler. Le dessin reproduit le même algorithme :
//   - squelette 2D en cinématique directe (hanches -> torse -> tête / bras ; hanches -> jambes) ;
//   - rendu anime, couche par couche à l'intérieur de chaque forme : ombre nette, partie éclairée
//     (forme décalée vers la lumière), bande de reflet chromé, liseré de lumière dans l'ombre,
//     reflets francs, détails en traits fins ; puis le trait, plus épais côté ombre ;
//   - articulations, cou, mains-gants avec pouce, semelles ; ombre au sol en deux tons ;
//   - écrasement / étirement (cartoon), traînée de mouvement, émotes (« … », ondes, notes, sueur,
//     traits d'emphase, étincelle).
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

    // Réaction ponctuelle : "happy", "laugh", "surprised", "shrug", "nod", "shake", "clap", "point"…
    // (toute animation non bouclée du rig). Jouée dès la fin de la réaction en cours.
    void Express(const std::string& clip) { pendingExpression_ = clip; }
    // Posture de fond quand il est au repos : "idle", "sad", "empathy", "soothe", "sleep", "dance"…
    void SetMood(const std::string& clip) { moodClip_ = clip; }

    // Avance l'horloge. Retourne true quand une nouvelle image doit être dessinée (24 ou 12 i/s).
    bool Update(float dtSeconds, float voiceRms) noexcept;

    // offset : position du canevas dans la cible (aperçu dans la fenêtre de configuration).
    void Render(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, float scale, D2D1_POINT_2F offset = {0.f, 0.f});
    void SetOpacity(float opacity) noexcept { opacity_ = opacity; }

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
        std::vector<std::string> trail;  // articulations qui laissent une traînée (ex. "aHand")
        std::map<std::string, std::vector<Key>> keys;
    };
    struct Body {
        float headRadius = 64, headGap = 6, neckY = -108;
        std::vector<D2D1_POINT_2F> torso;
        D2D1_POINT_2F shoulderA{}, shoulderB{}, hipA{}, hipB{};
        float upperArm = 52, foreArm = 48, handR = 16, thigh = 58, shin = 56;
        std::array<float, 2> upperArmR{}, foreArmR{}, thighR{}, shinR{}, foot{};
    };

    struct Skeleton {
        D2D1_POINT_2F H{}, neck{}, hc{}, headOffset{};
        float T = 0, ha = 0;
        D2D1_POINT_2F shoulder[2]{}, elbow[2]{}, hand[2]{}, hip[2]{}, knee[2]{}, ankle[2]{};
        float fore[2]{};
    };

    Pose Sample(const Clip& clip, int frame) const;
    Skeleton Solve(const Pose& pose) const;
    std::string ClipForState() const;
    void Draw(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const Pose& pose, const Clip& clip, float scale,
              D2D1_POINT_2F offset);
    void DrawEmote(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, const std::string& emote, D2D1_POINT_2F headCenter);
    ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* ctx, const std::string& name, float alpha = 1.f);

    // Données du rig
    float fps_ = 24.f;
    std::array<float, 2> canvas_{320, 420};
    D2D1_POINT_2F hips_{160, 262};
    float ground_ = 395;
    Body body_;
    std::map<std::string, D2D1_COLOR_F> palette_;
    float outline_ = 4.f, innerLine_ = 1.7f, rimWidth_ = 2.2f, weightRatio_ = 0.7f;
    D2D1_POINT_2F shadowOffset_{8, 7}, limbShadowOffset_{5, 4}, rimOffset_{3.2f, 2.8f}, weightOffset_{0.9f, 0.9f};
    Pose rest_;
    std::map<std::string, Clip> clips_;

    // Lecture
    AvatarState state_ = AvatarState::Idle;
    bool waveRequested_ = true;  // il salue au démarrage
    std::string pendingExpression_;
    std::string moodClip_ = "idle";
    std::string talkVariant_ = "talk";
    unsigned rng_ = 0x2545F491u;
    std::string clip_ = "idle", prevClip_;
    int frame_ = 0, prevFrame_ = 0, blend_ = 0;  // fondu de 3 images entre deux animations
    float clock_ = 0.f;
    float rmsEnv_ = 0.f;
    float opacity_ = 1.f;
    bool dirty_ = true;

    // Ressources Direct2D (liées au contexte)
    ID2D1DeviceContext* brushCtx_ = nullptr;
    std::map<std::string, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle1> roundStroke_;
};

}  // namespace companion
