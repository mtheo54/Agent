// Fausse fenêtre Discord (thème sombre) : un avatar qui parle (anneau vert), une tuile qui parle (cadre vert),
// et des pièges verts (pastilles « en ligne », bouton « Rejoindre », texte vert). Échelle 100 % et 150 %.
#include "modules/SpeakingMarks.hpp"
#include <cmath>
#include <cstdio>
using namespace companion;
struct Img {
    int w, h; std::vector<std::uint8_t> px;
    Img(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4) {
        for (int i = 0; i < w * h; ++i) { px[i * 4] = 0x38; px[i * 4 + 1] = 0x33; px[i * 4 + 2] = 0x31; px[i * 4 + 3] = 255; }
    }
    void Put(int x, int y, int r, int g, int b, float a = 1.f) {
        if (x < 0 || y < 0 || x >= w || y >= h) return; auto* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = static_cast<std::uint8_t>(p[0] * (1 - a) + b * a); p[1] = static_cast<std::uint8_t>(p[1] * (1 - a) + g * a); p[2] = static_cast<std::uint8_t>(p[2] * (1 - a) + r * a);
    }
    void Disc(int cx, int cy, float rad, int r, int g, int b) {
        for (int y = cy - rad - 2; y <= cy + rad + 2; ++y) for (int x = cx - rad - 2; x <= cx + rad + 2; ++x) {
            const float d = std::hypot(x - cx, y - cy); const float a = std::clamp(rad + 0.5f - d, 0.f, 1.f); if (a > 0) Put(x, y, r, g, b, a); }
    }
    void Ring(int cx, int cy, float rad, float th, int r, int g, int b) {
        for (int y = cy - rad - 3; y <= cy + rad + 3; ++y) for (int x = cx - rad - 3; x <= cx + rad + 3; ++x) {
            const float d = std::fabs(std::hypot(x - cx, y - cy) - rad); const float a = std::clamp(th / 2 + 0.5f - d, 0.f, 1.f); if (a > 0) Put(x, y, r, g, b, a); }
    }
    void Rect(int l, int t, int rr, int bb, int r, int g, int b) { for (int y = t; y < bb; ++y) for (int x = l; x < rr; ++x) Put(x, y, r, g, b); }
};
int main() {
    for (float s : {1.f, 1.5f}) {
        Img im(static_cast<int>(1280 * s), static_cast<int>(720 * s));
        const auto S = [s](float v) { return static_cast<int>(v * s); };
        // Liste du salon vocal : 3 membres, le 2e parle (anneau 2 px), pastilles « en ligne » sur les autres
        for (int i = 0; i < 3; ++i) {
            const int cy = S(300 + i * 34);
            im.Disc(S(40), cy, 12 * s, 120 + i * 40, 90, 200 - i * 50);           // avatar
            if (i == 1) im.Ring(S(40), cy, 14 * s, 2 * s, 0x23, 0xA5, 0x5A);      // il parle
            else im.Disc(S(49), cy + S(9), 4.5f * s, 0x23, 0xA5, 0x5A);           // pastille en ligne (pleine)
        }
        im.Rect(S(900), S(40), S(1000), S(72), 0x23, 0xA5, 0x5A);                  // bouton vert plein
        for (int k = 0; k < 12; ++k) im.Rect(S(60 + k * 9), S(250), S(66 + k * 9), S(262), 0x23, 0xA5, 0x5A);  // « Voix connectée » en vert
        // Vue du call : 2 tuiles, la 2e parle (cadre de 3 px)
        im.Rect(S(300), S(200), S(600), S(370), 60, 60, 70);
        im.Rect(S(620), S(200), S(920), S(370), 70, 60, 60);
        for (int t = 0; t < S(3); ++t) {
            im.Rect(S(620) + t, S(200) + t, S(920) - t, S(200) + t + 1, 0x23, 0xA5, 0x5A); im.Rect(S(620) + t, S(370) - t - 1, S(920) - t, S(370) - t, 0x23, 0xA5, 0x5A);
            im.Rect(S(620) + t, S(200) + t, S(620) + t + 1, S(370) - t, 0x23, 0xA5, 0x5A); im.Rect(S(920) - t - 1, S(200) + t, S(920) - t, S(370) - t, 0x23, 0xA5, 0x5A);
        }
        const auto marks = FindSpeakingMarks(im.px.data(), im.w, im.h, im.w * 4, s);
        std::printf("échelle %.0f %% : %zu marque(s)\n", s * 100, marks.size());
        for (const auto& m : marks) std::printf("  %s (%d,%d)-(%d,%d)\n", m.tile ? "tuile " : "anneau", m.left, m.top, m.right, m.bottom);
    }
}
