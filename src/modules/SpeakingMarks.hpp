#pragma once
// SpeakingMarks : repère, dans une capture de la fenêtre Discord, QUI parle — le cercle vert autour d'un avatar
// (liste du salon vocal) ou le cadre vert d'une tuile (vue du call). Logique pure (image BGRA), testée hors Windows.
//
//   gardé  : anneau vert creux de 16 à 72 px (×échelle), à peu près carré  -> pseudo juste à DROITE
//            cadre vert creux d'au moins 120 × 70 px (tuile)              -> pseudo en BAS À GAUCHE, dedans
//   ignoré : pastille « en ligne » (petite et pleine), bouton vert (plein), texte vert (morceaux trop petits)

#include <algorithm>
#include <cstdint>
#include <vector>

namespace companion {

struct SpeakingMark {
    int left = 0, top = 0, right = 0, bottom = 0;  // en pixels de l'image
    bool tile = false;
};

// Vert « parle » de Discord (#23A55A, ancien #3BA55C) et ses bords lissés.
inline bool IsSpeakingGreen(const std::uint8_t* bgra) {
    const int b = bgra[0], g = bgra[1], r = bgra[2];
    return g >= 140 && r <= 95 && b <= 135 && g - r >= 75 && g - b >= 40;
}

// Un pixel sur deux dans chaque sens (les anneaux font 2 à 3 px d'épaisseur : ils restent connexes en 8-voisinage).
inline std::vector<SpeakingMark> FindSpeakingMarks(const std::uint8_t* bgra, int w, int h, int stride, float scale) {
    constexpr int step = 2;
    const int gw = w / step, gh = h / step;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(gw) * gh, 0);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
            mask[static_cast<std::size_t>(y) * gw + x] = IsSpeakingGreen(bgra + static_cast<std::size_t>(y * step) * stride + x * step * 4);
    std::vector<SpeakingMark> out;
    std::vector<int> stack;
    for (int y0 = 0; y0 < gh; ++y0)
        for (int x0 = 0; x0 < gw; ++x0) {
            if (mask[static_cast<std::size_t>(y0) * gw + x0] != 1) continue;
            int l = x0, r = x0, t = y0, b = y0, count = 0;
            stack.assign(1, y0 * gw + x0);
            mask[static_cast<std::size_t>(y0) * gw + x0] = 2;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % gw, y = i / gw;
                ++count;
                l = std::min(l, x); r = std::max(r, x); t = std::min(t, y); b = std::max(b, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                        auto& m = mask[static_cast<std::size_t>(ny) * gw + nx];
                        if (m == 1) {
                            m = 2;
                            stack.push_back(ny * gw + nx);
                        }
                    }
            }
            const float bw = static_cast<float>((r - l + 1) * step), bh = static_cast<float>((b - t + 1) * step);
            const float fill = static_cast<float>(count) / static_cast<float>((r - l + 1) * (b - t + 1));
            SpeakingMark m{l * step, t * step, (r + 1) * step, (b + 1) * step, false};
            const float ratio = bw / bh;
            if (bw >= 16 * scale && bw <= 72 * scale && ratio > 0.75f && ratio < 1.33f && fill < 0.45f) out.push_back(m);
            else if (bw >= 120 * scale && bh >= 70 * scale && fill < 0.15f) {
                m.tile = true;
                out.push_back(m);
            }
        }
    return out;
}

}  // namespace companion
