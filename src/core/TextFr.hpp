#pragma once
// Minuscules en français, accents compris. std::tolower ne touche pas l'UTF-8 : « Écoute », « À fond », « Ça » restaient
// en capitale et les mots-clés (« écoute », « à », « ça ») n'étaient pas trouvés quand Whisper commence la phrase par
// une capitale accentuée.

#include <cctype>
#include <string>
#include <string_view>

namespace companion {

inline std::string LowerFr(std::string_view s) {
    std::string o(s);
    for (std::size_t i = 0; i < o.size(); ++i) {
        const auto c = static_cast<unsigned char>(o[i]);
        if (c == 0xC3 && i + 1 < o.size()) {  // À..Þ (sauf ×) -> à..þ
            const auto d = static_cast<unsigned char>(o[i + 1]);
            if (d >= 0x80 && d <= 0x9E && d != 0x97) o[i + 1] = static_cast<char>(d + 0x20);
            ++i;
        } else if (c == 0xC5 && i + 1 < o.size() && static_cast<unsigned char>(o[i + 1]) == 0x92) {  // Œ -> œ
            o[i + 1] = static_cast<char>(0x93);
            ++i;
        } else if (c < 0x80) {
            o[i] = static_cast<char>(std::tolower(c));
        }
    }
    return o;
}

}  // namespace companion
