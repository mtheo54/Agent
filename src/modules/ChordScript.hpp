#pragma once
// ChordScript : une suite d'accords écrite par Omnis, posée dans le piano roll de FL Studio par un script piano roll
// (FL Studio 21.2 et plus : piano roll, menu Outils > Scripts). Le modèle ne choisit que la musique (accords, durées), dans
// un format strict ; les notes, les renversements (voix qui bougent le moins possible) et le script Python sont calculés
// ici, donc toujours justes. Le modèle donne la tonalité et les DEGRÉS ; les accords sont empilés en tierces dans la
// gamme : ils sont toujours dans la tonalité.
//
//   TONALITE A mineur
//   DEGRE 1 2        la mineur, 2 mesures (4 temps chacune)
//   DEGRE 6 +7 1     fa maj7, 1 mesure
//   DEGRE 7 1        sol, 1 mesure

#include "core/TextFr.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

inline constexpr const char* kChordGrammar = R"(root ::= "TONALITE " [A-G] [#b]? (" mineur" | " majeur") "\n" deg deg deg deg
deg ::= "DEGRE " [1-7] (" +7" | " +9")? " " [1-2] "\n"
)";

struct Chord {
    std::string name;
    int root = 0;                // 0 = do … 11 = si
    std::vector<int> intervals;  // demi-tons au-dessus de la fondamentale
    int bars = 1;
};

inline std::optional<Chord> ParseChord(std::string_view name, int bars) {
    static constexpr int kLetter[] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    if (name.empty() || name[0] < 'A' || name[0] > 'G') return std::nullopt;
    Chord c;
    c.name = std::string(name);
    c.bars = std::clamp(bars, 1, 4);
    c.root = kLetter[name[0] - 'A'];
    std::string_view q = name.substr(1);
    if (!q.empty() && (q[0] == '#' || q[0] == 'b')) {
        c.root = (c.root + (q[0] == '#' ? 1 : 11)) % 12;
        q.remove_prefix(1);
    }
    struct Q { std::string_view s; std::vector<int> iv; };
    static const Q kQual[] = {{"", {0, 4, 7}},          {"m", {0, 3, 7}},         {"7", {0, 4, 7, 10}},     {"maj7", {0, 4, 7, 11}},
                              {"m7", {0, 3, 7, 10}},    {"m7b5", {0, 3, 6, 10}},  {"dim", {0, 3, 6}},       {"aug", {0, 4, 8}},
                              {"sus2", {0, 2, 7}},      {"sus4", {0, 5, 7}},      {"add9", {0, 4, 7, 14}},  {"9", {0, 4, 7, 10, 14}},
                              {"m9", {0, 3, 7, 10, 14}}, {"maj9", {0, 4, 7, 11, 14}}};
    for (const auto& x : kQual)
        if (q == x.s) {
            c.intervals = x.iv;
            return c;
        }
    return std::nullopt;
}

// Nom d'un accord (fondamentale + qualité) ; dièses ou bémols selon la tonalité.
inline std::string ChordName(int root, const std::vector<int>& iv, bool flats) {
    static const char* kSharp[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* kFlat[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    const auto has = [&](int x) { return std::find(iv.begin(), iv.end(), x) != iv.end(); };
    const bool minor = has(3), dim = has(3) && has(6), seventh = has(10) || has(11), ninth = has(14);
    std::string q;
    if (dim) q = has(10) ? "m7b5" : has(9) ? "dim7" : "dim";
    else if (!seventh) q = minor ? (ninth ? "madd9" : "m") : (ninth ? "add9" : "");
    else if (minor) q = ninth ? "m9" : "m7";
    else q = has(11) ? (ninth ? "maj9" : "maj7") : (ninth ? "9" : "7");
    return std::string((flats ? kFlat : kSharp)[root]) + q;
}

inline std::vector<Chord> ParseChordPlan(std::string_view text) {
    std::vector<Chord> out;
    int tonic = -1;
    bool minor = false, flats = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.starts_with("TONALITE ") && line.size() > 9) {
            static constexpr int kLetter[] = {9, 11, 0, 2, 4, 5, 7};
            const char l = line[9];
            if (l < 'A' || l > 'G') continue;
            tonic = kLetter[l - 'A'];
            const char acc = line.size() > 10 ? line[10] : ' ';
            if (acc == '#') tonic = (tonic + 1) % 12;
            if (acc == 'b') tonic = (tonic + 11) % 12;
            minor = line.find("mineur") != std::string_view::npos;
            // Tonalités écrites en bémols : fa, si b, mi b, la b, ré b majeur ; ré, sol, do, fa, si b, mi b mineur.
            static constexpr bool kFlatMajor[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};
            static constexpr bool kFlatMinor[12] = {1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0};
            flats = acc == 'b' || (minor ? kFlatMinor[tonic] : kFlatMajor[tonic]);
            continue;
        }
        if (tonic < 0 || !line.starts_with("DEGRE ") || line.size() < 9) continue;
        const int degree = line[6] - '1';
        int ext = 0;
        std::string_view rest = line.substr(7);
        if (rest.starts_with(" +7")) { ext = 7; rest.remove_prefix(3); }
        else if (rest.starts_with(" +9")) { ext = 9; rest.remove_prefix(3); }
        const int bars = std::atoi(std::string(rest).c_str());
        if (degree < 0 || degree > 6) continue;
        static constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11}, kMinor[] = {0, 2, 3, 5, 7, 8, 10};
        const int* sc = minor ? kMinor : kMajor;
        const auto at = [&](int k) { return sc[k % 7] + 12 * (k / 7); };  // k-ième note de la gamme (peut dépasser l'octave)
        Chord c;
        c.root = (tonic + sc[degree]) % 12;
        const int base = at(degree);
        const int count = ext == 9 ? 5 : ext == 7 ? 4 : 3;
        for (int i = 0; i < count; ++i) c.intervals.push_back(at(degree + 2 * i) - base);  // tierces empilées dans la gamme
        c.bars = std::clamp(bars, 1, 4);
        c.name = ChordName(c.root, c.intervals, flats);
        out.push_back(std::move(c));
    }
    return out;
}

struct PianoNote {
    int pitch;          // MIDI (60 = do central)
    double beat, len;   // en temps
    double velocity;    // 0..1
};

// Voicing : accord resserré autour du do central, renversement qui bouge le moins possible depuis l'accord précédent,
// fondamentale doublée à la basse (2 octaves sous). 4 temps par mesure.
inline std::vector<PianoNote> VoiceChords(const std::vector<Chord>& chords) {
    std::vector<PianoNote> notes;
    std::vector<int> prev;
    double beat = 0.0;
    for (const auto& c : chords) {
        std::vector<int> best;
        int bestCost = 1 << 30;
        for (int inv = 0; inv < static_cast<int>(c.intervals.size()); ++inv) {
            for (int base = 52; base <= 64; ++base) {
                if ((base - c.root) % 12 != 0) continue;  // base = la fondamentale dans une octave autour du mi 3
                std::vector<int> v;
                for (size_t i = 0; i < c.intervals.size(); ++i) {
                    int p = base + c.intervals[i];
                    if (static_cast<int>(i) < inv) p += 12;  // renversement : les premières notes montent d'une octave
                    v.push_back(p);
                }
                std::sort(v.begin(), v.end());
                if (v.front() < 52 || v.back() > 79) continue;
                int cost = 0;
                if (prev.empty()) {
                    cost = std::abs((v.front() + v.back()) / 2 - 64);
                } else {
                    for (int p : v) {
                        int d = 1 << 20;
                        for (int q : prev) d = std::min(d, std::abs(p - q));
                        cost += d;
                    }
                }
                if (cost < bestCost) { bestCost = cost; best = v; }
            }
        }
        if (best.empty()) continue;
        const double len = 4.0 * c.bars;
        for (int p : best) notes.push_back({p, beat, len, 0.72});
        notes.push_back({36 + ((c.root - 0) % 12 + 12) % 12, beat, len, 0.8});  // basse : do 1 … si 1
        prev = best;
        beat += len;
    }
    return notes;
}

// Le script piano roll (Python de FL Studio). Les notes sont ajoutées à partir de la position 0 du pattern.
inline std::string ChordPyScript(const std::vector<Chord>& chords, const std::vector<PianoNote>& notes) {
    std::string names;
    for (const auto& c : chords) names += (names.empty() ? "" : " ") + c.name;
    std::string s = "# Écrit par Omnis : " + names + "\nimport flpianoroll as flp\n\nNOTES = [\n";
    for (const auto& n : notes) s += std::format("    ({}, {:.3f}, {:.3f}, {:.2f}),\n", n.pitch, n.beat, n.len, n.velocity);
    s += "]\n\n"
         "def createDialog():\n"
         "    form = flp.ScriptDialog(\"Omnis - accords\", \"" + names + " : ajoute les accords et la basse au piano roll.\")\n"
         "    return form\n\n"
         "def apply(form):\n"
         "    ppq = flp.score.PPQ\n"
         "    for (pitch, beat, length, vel) in NOTES:\n"
         "        n = flp.Note()\n"
         "        n.number = pitch\n"
         "        n.time = int(beat * ppq)\n"
         "        n.length = int(length * ppq)\n"
         "        n.velocity = vel\n"
         "        flp.score.addNote(n)\n";
    return s;
}

}  // namespace companion
