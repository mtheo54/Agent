#include "audio/TextNormalizerFr.hpp"

#include <array>
#include <cctype>
#include <regex>

namespace companion {
namespace {

constexpr std::array<const char*, 17> kUnits{"zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit",
                                            "neuf", "dix", "onze", "douze", "treize", "quatorze", "quinze", "seize"};
constexpr std::array<const char*, 7> kTens{"", "dix", "vingt", "trente", "quarante", "cinquante", "soixante"};

std::string Below100(int n) {
    if (n < 17) return kUnits[static_cast<size_t>(n)];
    if (n < 20) return std::string("dix-") + kUnits[static_cast<size_t>(n - 10)];
    if (n < 70) {
        const int t = n / 10, u = n % 10;
        if (u == 0) return kTens[static_cast<size_t>(t)];
        return std::string(kTens[static_cast<size_t>(t)]) + (u == 1 ? " et un" : std::string("-") + kUnits[static_cast<size_t>(u)]);
    }
    if (n < 80) return n == 71 ? "soixante et onze" : "soixante-" + Below100(n - 60);
    if (n == 80) return "quatre-vingts";
    return "quatre-vingt-" + Below100(n - 80);
}

std::string Below1000(int n, bool final) {
    if (n < 100) {
        std::string s = Below100(n);
        if (!final && s.ends_with("quatre-vingts")) s.pop_back();  // « quatre-vingt mille »
        return s;
    }
    const int h = n / 100, r = n % 100;
    std::string s = h == 1 ? "cent" : std::string(kUnits[static_cast<size_t>(h)]) + " cent";
    if (r == 0) return (h > 1 && final) ? s + "s" : s;
    return s + " " + Below1000(r, final);
}

}  // namespace

std::string NumberToWordsFr(std::int64_t n) {
    if (n < 0) return "moins " + NumberToWordsFr(-n);
    if (n < 1000) {
        return Below1000(static_cast<int>(n), true);
    }
    std::string out;
    const std::int64_t billions = n / 1'000'000'000, millions = (n / 1'000'000) % 1000, thousands = (n / 1000) % 1000,
                       rest = n % 1000;
    const auto add = [&](const std::string& part) { out += (out.empty() ? "" : " ") + part; };
    if (billions) add(NumberToWordsFr(billions) + (billions > 1 ? " milliards" : " milliard"));
    if (millions) add(Below1000(static_cast<int>(millions), true) + (millions > 1 ? " millions" : " million"));
    if (thousands) add(thousands == 1 ? "mille" : Below1000(static_cast<int>(thousands), false) + " mille");
    if (rest) add(Below1000(static_cast<int>(rest), true));
    return out;
}

std::string NormalizeForSpeechFr(const std::string& input) {
    std::string text;
    // Emojis : quelques-uns se disent (« 💀 » = mort de rire), les autres sont retirés (la voix les lirait mal).
    static const std::pair<const char*, const char*> spoken[] = {
        {"\xF0\x9F\x92\x80", " je suis mort "},   // 💀
        {"\xF0\x9F\x98\xAD", " je pleure "},      // 😭
        {"\xF0\x9F\x98\x82", " mdr "},            // 😂
        {"\xF0\x9F\xA4\xA3", " mdr "},            // 🤣
        {"\xF0\x9F\x94\xA5", " ça brûle "},       // 🔥
    };
    for (size_t i = 0; i < input.size();) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        bool done = false;
        for (const auto& [emoji, words] : spoken)
            if (input.compare(i, 4, emoji) == 0) { text += words; i += 4; done = true; break; }
        if (done) continue;
        if (c >= 0xF0) { i += 4; continue; }  // autres emojis (4 octets)
        if (c == 0xE2 && i + 2 < input.size() && (static_cast<unsigned char>(input[i + 1]) & 0xFC) == 0x9C) {
            i += 3;  // pictogrammes U+2700-27BF (✨, ❤…)
            continue;
        }
        if (c == 0xEF && i + 2 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xB8 &&
            static_cast<unsigned char>(input[i + 2]) == 0x8F) {
            i += 3;  // sélecteur de variante emoji
            continue;
        }
        text += input[i++];
    }
    // « 2 980 » ou « 2 980 » (espace insécable) → 2980
    static const std::regex thousands(R"((\d{1,3})(?:[  ]|\xE2\x80\xAF)(\d{3})(?!\d))");
    for (int i = 0; i < 3; ++i) text = std::regex_replace(text, thousands, "$1$2");

    static const std::regex plusAfterWord(R"(([A-Za-z])\+(\d))");  // « sub+3 »
    text = std::regex_replace(text, plusAfterWord, "$1 plus $2");

    // Taux de compression « 4:1 » -> « 4 pour 1 » (lu « 4, 1 » sinon). Les heures (14:30) ne finissent pas par « :1 ».
    static const std::regex ratio(R"((\d{1,3}(?:[.,]\d)?)\s?:\s?1(?![\d:]))");
    text = std::regex_replace(text, ratio, "$1 pour 1");
    // Heures : « 14:00 » -> « 14 heures », « 14:30 » -> « 14 heures 30 » ; autre « 3:12 » (score) -> « 3 12 ».
    static const std::regex hourPile(R"(\b([01]?\d|2[0-3]):00\b)");
    text = std::regex_replace(text, hourPile, "$1 heures");
    static const std::regex hour(R"(\b([01]?\d|2[0-3]):([0-5]\d)\b)");
    text = std::regex_replace(text, hour, "$1 heures $2");
    static const std::regex colonDigits(R"((\d):(\d))");
    text = std::regex_replace(text, colonDigits, "$1 $2");
    static const std::regex fraction(R"((\d+)\s?/\s?(\d+))");
    text = std::regex_replace(text, fraction, "$1 sur $2");

    // Nombre (signe, décimales) éventuellement suivi d'une unité.
    static const std::regex number(
        R"((^|[^\w.,])([-−+]?)(\d+)(?:[.,](\d+))?(?:\s?(dBFS|dBTP|dB|LUFS|kHz|Hz|ms|%|Go|Mo|€|fps|FPS|bpm|BPM|er|e)(?![\w]))?)");
    std::string out;
    auto it = std::sregex_iterator(text.begin(), text.end(), number);
    size_t last = 0;
    for (; it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        out.append(text, last, static_cast<size_t>(m.position(0)) - last);
        out += m[1].str();
        const std::string intPart = m[3].str(), dec = m[4].str(), unit = m[5].str();
        const bool negative = m[2].str() == "-" || m[2].str() == "−";
        if (intPart.size() > 12) {  // numéro trop long : chiffre par chiffre
            for (char c : intPart) out += std::string(kUnits[static_cast<size_t>(c - '0')]) + " ";
        } else {
            const auto value = std::stoll(intPart);
            if (unit == "er") {
                out += value == 1 ? "premier" : NumberToWordsFr(value) + "ième";
                last = static_cast<size_t>(m.position(0) + m.length(0));
                continue;
            }
            if (unit == "e") {
                std::string w = NumberToWordsFr(value);
                if (w.ends_with("e")) w.pop_back();
                if (w.ends_with("cinq")) w += "u";
                if (w.ends_with("neuf")) w.back() = 'v';
                out += value == 1 ? "premier" : w + "ième";
                last = static_cast<size_t>(m.position(0) + m.length(0));
                continue;
            }
            if (negative) out += "moins ";
            else if (m[2].str() == "+") out += " plus ";
            std::string words = NumberToWordsFr(value);
            if (!dec.empty() && dec.find_first_not_of('0') != std::string::npos) {
                words += " virgule ";
                size_t zeros = 0;
                while (zeros < dec.size() - 1 && dec[zeros] == '0') { words += "zéro "; ++zeros; }
                words += dec.size() - zeros <= 3 ? NumberToWordsFr(std::stoll(dec.substr(zeros))) : dec.substr(zeros);
            }
            out += words;
            const bool plural = value >= 2 || (!dec.empty() && dec.find_first_not_of('0') != std::string::npos);
            if (unit == "dB" || unit == "dBFS" || unit == "dBTP") out += plural ? " décibels" : " décibel";
            else if (unit == "LUFS") out += " LUFS";
            else if (unit == "kHz") out += " kilohertz";
            else if (unit == "Hz") out += " hertz";
            else if (unit == "ms") out += plural ? " millisecondes" : " milliseconde";
            else if (unit == "%") out += " pour cent";
            else if (unit == "Go") out += plural ? " gigaoctets" : " gigaoctet";
            else if (unit == "Mo") out += plural ? " mégaoctets" : " mégaoctet";
            else if (unit == "€") out += plural ? " euros" : " euro";
            else if (unit == "fps" || unit == "FPS") out += " FPS";
            else if (unit == "bpm" || unit == "BPM") out += " BPM";
        }
        last = static_cast<size_t>(m.position(0) + m.length(0));
    }
    out.append(text, last, std::string::npos);

    // Accord : « vingt et un heures » -> « vingt et une heures », « un heures » -> « une heure ».
    static const std::regex oneHour(R"((^|[^\w])un heures)");
    out = std::regex_replace(out, std::regex("et un heures"), "et une heures");
    out = std::regex_replace(out, oneHour, "$1une heure");
    static const std::regex amp(R"(\s&\s)");
    out = std::regex_replace(out, amp, " et ");
    return out;
}

}  // namespace companion
