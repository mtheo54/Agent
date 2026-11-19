#pragma once
// ConfigView : fenêtre de configuration du personnage, au style du site abrasion
// (fond noir étoilé, Space Grotesk / Space Mono, titre en lettres creuses, encadrés pointillés à
// coins « + », lignes de statut façon terminal « > élément ....... statut », curseurs ASCII
// « [####......] », bouton blanc à coins coupés).
//
// Dessin et interactions uniquement (Direct2D + DirectWrite, aucune fenêtre ici) : ConfigWindow
// l'héberge, et un test peut le rendre hors écran. Coordonnées logiques : 1040 x 760.

#include "core/Config.hpp"
#include "render/CartoonAvatar.hpp"

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wrl/client.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace companion {

struct ConfigStatus {
    bool brainReady = false;
    bool sttReady = false;
    bool ttsReady = false;
    bool neuralVoice = false;      // voix neuronale chargée
    bool claudeKey = false;
    int voiceSamples = 0;          // phrases apprises pour la voix habituelle
    std::string emotion = "neutre";
    float arousal = 0.f;           // -1 … +1
    // Journal de la conversation (rôle « user » / « assistant », texte), du plus ancien au plus récent.
    std::vector<std::pair<std::string, std::string>> conversation;
};

class ConfigView {
public:
    static constexpr float kWidth = 1040.f;
    static constexpr float kHeight = 1120.f;

    enum class Action { None, Save, Cancel };

    bool Init(ID2D1Factory1* factory, IDWriteFactory* dwrite, const std::filesystem::path& fontsDir,
              const std::filesystem::path& rigJson);
    void SetConfig(const CompanionConfig& cfg);
    [[nodiscard]] const CompanionConfig& Config() const noexcept { return cfg_; }
    void SetStatus(const ConfigStatus& s) { status_ = s; dirty_ = true; }

    bool Tick(float dt);  // avance l'aperçu animé (24 i/s) ; true si un redessin est nécessaire
    void Render(ID2D1DeviceContext* ctx, float scale);

    // Entrées en coordonnées logiques.
    Action MouseDown(float x, float y);
    void MouseMove(float x, float y);
    void MouseUp();
    Action Key(unsigned vk);
    void Char(wchar_t c);
    void Paste(const std::wstring& text);  // Ctrl+V (clé Claude)
    void Wheel(int notches);               // défile le journal de conversation
    [[nodiscard]] bool InCaption(float x, float y) const;  // zone de déplacement de la fenêtre
    [[nodiscard]] bool CloseHit(float x, float y) const;

private:
    enum class Field { None, Name, Scale, Opacity, Corner, Expressiveness, Fx, Rate, Backchannels, BargeIn,
                       Emotion, Listen, Tone, Engine, Quirks, AutoTraining, Cloud, CloudAsk, Save, Cancel, Close, Expression,
                       ClaudeKey, ClaudeKeyClear, Captions, Conversation };
    struct Hit {
        D2D1_RECT_F r;
        Field field;
        int value = 0;  // option d'un sélecteur, index d'expression
    };

    // Dessin
    IDWriteTextFormat* Format(const std::string& key);
    void Text(const std::wstring& s, const char* fmt, float x, float y, const char* color, float spacing = 0.f,
              float maxW = 1000.f);
    float TextWidth(const std::wstring& s, const char* fmt, float spacing = 0.f);
    void OutlineText(const std::wstring& s, const char* fmt, float x, float y, const char* color, float stroke);
    void Box(D2D1_RECT_F r, const std::wstring& label);
    void Leader(float x, float y, float w, const std::wstring& label, const std::wstring& status, const char* statusColor,
                Field f, int value = 0);
    void Slider(float x, float y, float w, const std::wstring& label, float v01, const std::wstring& valueText, Field f);
    void Segmented(float x, float y, const std::wstring& label, const std::vector<std::wstring>& options, int selected,
                   Field f);
    ID2D1SolidColorBrush* Brush(const char* name, float alpha = 1.f);

    float TextHeight(const std::wstring& s, const char* fmt, float maxW);
    void RenderConversation(D2D1_RECT_F area);
    float SliderValue(Field f) const;
    void SetSliderValue(Field f, float v01);

    ID2D1Factory1* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts_;  // polices du site (assets/fonts), sinon système
    std::wstring mono_ = L"Space Mono", sans_ = L"Space Grotesk";
    std::map<std::string, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
    ID2D1DeviceContext* ctx_ = nullptr;  // valide pendant Render
    std::map<std::string, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
    ID2D1DeviceContext* brushCtx_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed_;

    CompanionConfig cfg_;
    ConfigStatus status_;
    CartoonAvatar preview_;
    bool previewOk_ = false;
    std::vector<Hit> hits_;
    std::vector<D2D1_POINT_2F> stars_;
    std::vector<float> starAlpha_;
    Field dragging_ = Field::None;
    D2D1_RECT_F dragRect_{};
    Field focus_ = Field::None;
    float caretClock_ = 0.f;
    D2D1_POINT_2F mouse_{-1, -1};
    bool dirty_ = true;
    int scroll_ = 0;              // messages masqués en bas (0 = on suit le dernier)
    D2D1_RECT_F convRect_{};
};

}  // namespace companion
