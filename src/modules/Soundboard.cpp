#include "modules/Soundboard.hpp"

#include "network/HttpClient.hpp"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>

namespace companion {

std::string Soundboard::Key(const std::string& name) {
    std::string k;
    for (unsigned char c : name) {
        if (std::isalnum(c)) k += static_cast<char>(std::tolower(c));
        else if (c >= 0x80) k += static_cast<char>(c);  // accents UTF-8 gardés tels quels
        else if (!k.empty() && k.back() != ' ') k += ' ';
    }
    while (!k.empty() && k.back() == ' ') k.pop_back();
    return k;
}

std::string Soundboard::KeyFor(const std::string& nameOrUrl) {
    if (!IsUrl(nameOrUrl)) return Key(nameOrUrl);
    std::string path = nameOrUrl;
    while (!path.empty() && path.back() == '/') path.pop_back();
    std::string slug = path.substr(path.find_last_of('/') + 1);
    // « salut-cest-frank-leboeuf-complet-97159 » : l'identifiant numérique final n'est pas un mot du nom
    if (const auto dash = slug.find_last_of('-'); dash != std::string::npos &&
        slug.find_first_not_of("0123456789", dash + 1) == std::string::npos && dash + 1 < slug.size())
        slug.erase(dash);
    return Key(slug);
}

const std::vector<std::string>& Soundboard::StarterPack() {
    static const std::vector<std::string> pack{
        "vine boom", "bruh", "c'est vrai", "fahhh", "anime wow", "sad violin",
        "airhorn", "windows error", "fail trombone", "suspense", "emotional damage", "oh no",
    };
    return pack;
}

Soundboard::Soundboard(HttpClient& http, std::filesystem::path dir, int sampleRate)
    : http_(http), dir_(std::move(dir)), rate_(sampleRate) {
    Scan();
}

void Soundboard::Scan() {
    std::error_code ec;
    std::scoped_lock lock(mutex_);
    files_.clear();
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec))
        if (e.path().extension() == ".mp3") files_[Key(e.path().stem().string())] = e.path();
}

std::vector<std::string> Soundboard::Names() const {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [k, _] : files_) out.push_back(k);
    return out;
}

bool Soundboard::Has(const std::string& name) const {
    std::scoped_lock lock(mutex_);
    return files_.count(KeyFor(name)) > 0;
}

std::optional<std::pair<std::string, std::string>> ParseMyInstantsFirst(const std::string& html) {
    static const std::regex play(R"(play\('(/media/sounds/[^']+\.mp3)')");
    static const std::regex title(R"(class="instant-link[^"]*"[^>]*>([^<]+)<)");
    std::smatch m;
    if (!std::regex_search(html, m, play)) return std::nullopt;
    const std::string path = m[1].str();
    const auto rest = html.substr(static_cast<size_t>(m.position(0)));
    std::smatch t;
    return std::make_pair(path, std::regex_search(rest, t, title) ? t[1].str() : path);
}

std::optional<std::string> Soundboard::Fetch(const std::string& query, std::string* error) {
    const std::string key = KeyFor(query);
    if (key.empty()) return std::nullopt;
    if (Has(key)) return key;
    // Lien direct vers un son (page « instant ») : son 1er lecteur est le son lui-même. Seul myinstants.com
    // est accepté (pas de téléchargement depuis n'importe quel site).
    const std::string url = IsUrl(query) ? (query.rfind("https://www.myinstants.com/", 0) == 0 ? query : std::string{})
                                         : "https://www.myinstants.com/fr/search/?name=" + HttpClient::UrlEncode(query);
    if (url.empty()) {
        if (error) *error = "lien non pris en charge";
        return std::nullopt;
    }
    const auto page = http_.Get(url, 10000);
    if (!page.Ok()) {
        if (error) *error = page.error.empty() ? "recherche impossible" : page.error;
        return std::nullopt;
    }
    const auto hit = ParseMyInstantsFirst(page.body);
    if (!hit) {
        if (error) *error = "aucun son trouvé";
        return std::nullopt;
    }
    const auto mp3 = http_.Get("https://www.myinstants.com" + hit->first, 15000);
    if (!mp3.Ok() || mp3.body.size() < 512) {
        if (error) *error = "téléchargement impossible";
        return std::nullopt;
    }
    // Vérifie que c'est bien un MP3 décodable avant de le garder.
    const std::vector<unsigned char> bytes(mp3.body.begin(), mp3.body.end());
    auto pcm = DecodeMp3(bytes, rate_);
    if (pcm.empty()) {
        if (error) *error = "fichier illisible";
        return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const std::string fname = key + ".mp3";
    const auto file = dir_ / std::filesystem::path(std::u8string(fname.begin(), fname.end()));
    std::ofstream(file, std::ios::binary).write(mp3.body.data(), static_cast<std::streamsize>(mp3.body.size()));
    std::scoped_lock lock(mutex_);
    files_[key] = file;
    pcm_[key] = std::make_shared<const std::vector<float>>(std::move(pcm));
    return key;
}

std::shared_ptr<const std::vector<float>> Soundboard::Get(const std::string& name) {
    const std::string key = KeyFor(name);
    std::filesystem::path file;
    {
        std::scoped_lock lock(mutex_);
        if (const auto it = pcm_.find(key); it != pcm_.end()) return it->second;
        auto it = files_.find(key);
        if (it == files_.end()) {  // nom approché : « vine » trouve « vine boom »
            for (auto f = files_.begin(); f != files_.end(); ++f)
                if (f->first.find(key) != std::string::npos || key.find(f->first) != std::string::npos) { it = f; break; }
        }
        if (it == files_.end()) return nullptr;
        if (const auto p = pcm_.find(it->first); p != pcm_.end()) return p->second;
        file = it->second;
    }
    std::ifstream f(file, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), {});
    auto pcm = DecodeMp3(bytes, rate_);
    if (pcm.empty()) return nullptr;
    auto shared = std::make_shared<const std::vector<float>>(std::move(pcm));
    std::scoped_lock lock(mutex_);
    pcm_[Key(file.stem().string())] = shared;
    return shared;
}

std::vector<float> Soundboard::DecodeMp3(const std::vector<unsigned char>& mp3, int sampleRate) {
    drmp3_config info{};
    drmp3_uint64 frames = 0;
    float* raw = drmp3_open_memory_and_read_pcm_frames_f32(mp3.data(), mp3.size(), &info, &frames, nullptr);
    if (!raw || frames == 0 || info.channels == 0) {
        if (raw) drmp3_free(raw, nullptr);
        return {};
    }
    // Mono
    const size_t inRate = info.sampleRate, ch = info.channels;
    const size_t maxIn = std::min<size_t>(static_cast<size_t>(frames), inRate * 8);  // 8 s lus au plus
    std::vector<float> mono(maxIn);
    for (size_t i = 0; i < maxIn; ++i) {
        float s = 0.f;
        for (size_t c = 0; c < ch; ++c) s += raw[i * ch + c];
        mono[i] = s / static_cast<float>(ch);
    }
    drmp3_free(raw, nullptr);
    // Anti-repliement avant de descendre en fréquence (2 passes de passe-bas à 0,42 × la nouvelle fréquence)
    if (inRate > static_cast<size_t>(sampleRate)) {
        const float fc = 0.42f * static_cast<float>(sampleRate), w = 2.f * 3.14159265f * fc / static_cast<float>(inRate);
        const float c = std::cos(w), a = std::sin(w) / (2.f * 0.7071f), a0 = 1.f + a;
        const float b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0, b2 = b0, a1 = -2 * c / a0, a2 = (1 - a) / a0;
        for (int pass = 0; pass < 2; ++pass) {
            float z1 = 0, z2 = 0;
            for (auto& x : mono) {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                x = y;
            }
        }
    }
    // Rééchantillonnage linéaire
    const double step = static_cast<double>(inRate) / sampleRate;
    std::vector<float> out(static_cast<size_t>(static_cast<double>(mono.size()) / step));
    for (size_t i = 0; i < out.size(); ++i) {
        const double p = static_cast<double>(i) * step;
        const size_t k = static_cast<size_t>(p);
        const float f = static_cast<float>(p - static_cast<double>(k));
        out[i] = mono[k] * (1.f - f) + (k + 1 < mono.size() ? mono[k + 1] : mono[k]) * f;
    }
    // Silences de début / fin retirés, 7 s au plus (fondu de 300 ms si coupé)
    float peak = 0.f;
    for (float x : out) peak = std::max(peak, std::abs(x));
    if (peak < 1e-4f) return {};
    size_t a = 0, b = out.size();
    while (a < b && std::abs(out[a]) < peak * 0.02f) ++a;
    while (b > a && std::abs(out[b - 1]) < peak * 0.02f) --b;
    std::vector<float> clip(out.begin() + static_cast<std::ptrdiff_t>(a), out.begin() + static_cast<std::ptrdiff_t>(b));
    const size_t cap = static_cast<size_t>(sampleRate) * 7;
    if (clip.size() > cap) {
        clip.resize(cap);
        const size_t fade = static_cast<size_t>(sampleRate) * 3 / 10;
        for (size_t i = 0; i < fade; ++i) clip[cap - 1 - i] *= static_cast<float>(i) / static_cast<float>(fade);
    }
    // Niveau : les sons de memes sont souvent « au taquet » ; on les ramène au niveau de sa voix
    // (RMS des passages actifs ≈ -20 dBFS), crête plafonnée à -3 dBFS.
    double sum = 0;
    size_t n = 0;
    for (float x : clip)
        if (std::abs(x) > peak * 0.1f) { sum += static_cast<double>(x) * x; ++n; }
    const float rms = n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : peak;
    float gain = 0.1f / std::max(rms, 1e-6f);
    gain = std::min(gain, 0.708f / peak);
    for (auto& x : clip) x *= gain;
    return clip;
}

}  // namespace companion
