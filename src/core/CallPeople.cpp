#include "core/CallPeople.hpp"

#include "audio/SpeakerFeatures.hpp"  // Cosine
#include "audio/SpeakerId.hpp"        // seuils

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace companion {
namespace {

bool Has(const std::string& s, std::initializer_list<const char*> words) {
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return s.find(w) != std::string::npos; });
}

std::string Clip(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    s.resize(max);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // pas de UTF-8 coupé
    if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
    return s + "…";
}

}  // namespace

std::string CallPerson::Label() const {
    if (!name.empty()) return name;
    return "Voix " + (id.size() > 1 ? id.substr(1) : id);
}

bool CallPeople::Load(const std::filesystem::path& file) {
    file_ = file;
    people_.clear();
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    try {
        const auto j = nlohmann::json::parse(f);
        nextId_ = j.value("nextId", 1);
        for (const auto& p : j.value("people", nlohmann::json::array())) {
            CallPerson c;
            c.id = p.value("id", std::string{});
            c.name = p.value("name", std::string{});
            c.voice = p.value("voice", std::vector<float>{});
            c.samples = p.value("samples", 0);
            for (const auto& s : p.value("said", std::vector<std::string>{})) c.said.push_back(s);
            for (const auto& s : p.value("mocks", std::vector<std::string>{})) c.mocks.push_back(s);
            c.avoid = p.value("avoid", std::vector<std::string>{});
            c.lastSeen = p.value("lastSeen", std::int64_t{0});
            if (!c.id.empty() && !c.voice.empty()) people_.push_back(std::move(c));
        }
    } catch (const std::exception&) {
        people_.clear();
        return false;
    }
    return true;
}

bool CallPeople::Save() const {
    if (file_.empty()) return false;
    nlohmann::json j;
    j["nextId"] = nextId_;
    j["people"] = nlohmann::json::array();
    for (const auto& p : people_)
        j["people"].push_back({{"id", p.id},
                               {"name", p.name},
                               {"voice", p.voice},
                               {"samples", p.samples},
                               {"said", std::vector<std::string>(p.said.begin(), p.said.end())},
                               {"mocks", std::vector<std::string>(p.mocks.begin(), p.mocks.end())},
                               {"avoid", p.avoid},
                               {"lastSeen", p.lastSeen}});
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump();
    }
    std::filesystem::rename(tmp, file_, ec);
    return !ec;
}

CallPeople::Match CallPeople::Identify(const std::vector<float>& emb, bool allowCreate, std::int64_t now) {
    Match m;
    for (auto& p : people_) {
        const float s = Cosine(emb, p.voice);
        if (s > m.score) {
            m.score = s;
            m.person = &p;
        }
    }
    if (m.person && m.score >= SpeakerId::kSame) {
        m.sure = true;
        CallPerson& p = *m.person;
        // Empreinte moyenne glissante (au plus 20 phrases : elle suit un micro qui change).
        const float w = 1.f / static_cast<float>(std::min(p.samples, 19) + 1);
        double norm = 0;
        for (std::size_t i = 0; i < p.voice.size(); ++i) {
            p.voice[i] = (1.f - w) * p.voice[i] + w * emb[i];
            norm += static_cast<double>(p.voice[i]) * p.voice[i];
        }
        const float inv = norm > 0 ? static_cast<float>(1.0 / std::sqrt(norm)) : 1.f;
        for (auto& v : p.voice) v *= inv;
        ++p.samples;
        p.lastSeen = now;
        return m;
    }
    if ((!m.person || m.score < SpeakerId::kOther) && allowCreate && people_.size() < 40) {
        CallPerson p;
        p.id = "p" + std::to_string(nextId_++);
        p.voice = emb;
        p.samples = 1;
        p.lastSeen = now;
        people_.push_back(std::move(p));
        m.person = &people_.back();
        m.score = 1.f;
        m.sure = true;
        return m;
    }
    m.sure = false;  // entre deux seuils : on ne sait pas, on n'attribue rien
    return m;
}

CallPerson* CallPeople::ById(const std::string& id) {
    for (auto& p : people_)
        if (p.id == id) return &p;
    return nullptr;
}

CallPerson* CallPeople::ByName(const std::string& name) {
    std::string want = name;
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& p : people_) {
        std::string n = p.name;
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!n.empty() && n == want) return &p;
    }
    return nullptr;
}

bool CallPeople::Forget(const std::string& id) {
    const auto it = std::ranges::find(people_, id, &CallPerson::id);
    if (it == people_.end()) return false;
    people_.erase(it);
    return Save();
}

void CallPeople::Rename(CallPerson& p, std::string name) { p.name = Clip(std::move(name), 24); }

void CallPeople::Remember(CallPerson& p, std::string line) {
    p.said.push_back(Clip(std::move(line), 140));
    while (p.said.size() > 8) p.said.pop_front();
}

void CallPeople::Mocked(CallPerson& p, std::string line) {
    p.mocks.push_back(Clip(std::move(line), 140));
    while (p.mocks.size() > 5) p.mocks.pop_front();
}

void CallPeople::Avoid(CallPerson& p, const std::string& category) {
    if (std::ranges::find(p.avoid, category) == p.avoid.end()) p.avoid.push_back(category);
}

std::string CallPeople::Describe(const CallPerson& p) {
    std::string s = p.Label() + (p.name.empty() ? " (prénom inconnu)" : "");
    if (!p.said.empty()) {
        s += " — a dit récemment :";
        for (const auto& l : p.said) s += " « " + l + " »";
    }
    if (!p.mocks.empty()) {
        s += " — s'est déjà moqué de toi :";
        for (const auto& l : p.mocks) s += " « " + l + " »";
    }
    if (!p.avoid.empty()) {
        s += " — NE PARLE JAMAIS (même pas en blague) de :";
        for (const auto& a : p.avoid) s += " " + a + ",";
        s.pop_back();
    }
    return s;
}

// ---------------------------------------------------------------------------
std::optional<std::string> SensitiveTopic(const std::string& t) {
    struct Cat {
        const char* label;
        std::initializer_list<const char*> words;
    };
    static const Cat cats[] = {
        {"un deuil", {"est mort", "est morte", "est décédé", "est décédée", "décès", "enterrement", "obsèques", "deuil", "nous a quittés", "on l'a perdu"}},
        {"sa santé", {"cancer", "chimio", "hôpital", "hopital", "urgences", "maladie", "malade grave", "je me suis fait opérer", "tumeur",
                      "diagnostic"}},
        {"sa vie de couple", {"m'a quitté", "m'a quittée", "on s'est séparé", "on s'est séparés", "rupture", "divorce", "elle m'a largué",
                              "il m'a largué", "m'a trompé", "m'a trompée"}},
        {"son moral", {"dépression", "déprime", "je vais pas bien", "ça va pas fort", "ca va pas fort", "crise d'angoisse", "anxiété",
                       "suicide", "me faire du mal", "envie de rien", "je craque"}},
        {"ses soucis d'argent ou de boulot", {"viré du taf", "viré de mon taf", "viré de mon boulot", "licencié", "chômage", "chomage", "plus de taf", "pas un rond", "dettes",
                                              "expulsé", "huissier", "galère d'argent"}},
        {"sa famille", {"mes parents divorcent", "mes parents se séparent", "mon père est malade", "ma mère est malade",
                        "problèmes de famille", "soucis de famille", "placé en foyer"}},
        {"une agression ou du harcèlement", {"harcèlement", "harcelé", "harcelée", "agressé", "agressée", "agression", "violences",
                                             "frappé par mon", "frappé par ma"}},
    };
    for (const auto& c : cats)
        if (Has(t, c.words)) return std::string(c.label);
    return std::nullopt;
}

bool MocksOmnis(const std::string& t) {
    if (!Has(t, {"omnis", "omni ", "l'ia", "le bot", "le robot", "ton robot", "ton ia"})) return false;
    return Has(t, {"nul", "naze", "éclaté", "eclaté", "eclate", "bouffon", "débile", "debile", "con ", "conne", "idiot", "teubé", "teube",
                   "sert à rien", "sert a rien", "de merde", "pourri", "claqué", "claque au sol", "ferme-la", "ferme la", "ta gueule",
                   "mdr", "ptdr", "il comprend rien", "il bug", "il beug", "gênant", "genant", "cringe", "voix de", "robot de"});
}

bool DidntHear(const std::string& t) {
    std::string s = t;
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s == "quoi ?" || s == "quoi" || s == "hein ?" || s == "hein" || s == "pardon ?" || s == "comment ?") return true;
    return Has(t, {"t'as dit quoi", "il a dit quoi", "il dit quoi", "j'ai pas entendu", "on t'entend pas", "on entend rien",
                   "on l'entend pas", "on entend pas", "parle plus fort", "parle fort", "articule", "tu peux répéter", "répète ?", "repete ?", "j'ai rien compris"});
}

bool MakingMusic(const std::string& t) {
    return Has(t, {"[musique]", "(musique)", "♪", "♫", "je rap", "je rappe", "je vais rapper", "laisse-moi rapper", "laissez-moi rapper",
                   "écoutez ma prod", "ecoutez ma prod", "écoute ma prod", "ecoute ma prod", "je stream", "je suis sur fl", "je fais un son",
                   "je fais une prod", "je compose", "je mixe", "en live sur", "je suis en live"});
}

std::optional<std::string> SelfIntroduction(const std::string& text) {
    std::string lower = text;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* stop[] = {"pas", "bon", "le", "la", "les", "un", "une", "ça", "ca", "quoi", "nul", "mort", "chaud", "pareil",
                                 "normal", "comme", "vrai", "sûr", "sur", "mieux", "fini", "toi", "moi", "lui", "elle", "juste", "trop",
                                 "vraiment", "clair", "ok", "moi-même", "qui", "que", "ton", "ta", "mon", "ma", "du", "de", "des"};
    for (const char* key : {"moi c'est ", "je m'appelle ", "moi, c'est ", "ici c'est "}) {
        const auto p = lower.find(key);
        if (p == std::string::npos) continue;
        std::size_t b = p + std::string_view(key).size(), e = b;
        while (e < text.size() && (std::isalpha(static_cast<unsigned char>(text[e])) || static_cast<unsigned char>(text[e]) >= 0x80 ||
                                   text[e] == '-'))
            ++e;
        std::string name = text.substr(b, e - b);
        std::string ln = lower.substr(b, e - b);
        if (name.size() < 2 || name.size() > 20 || std::ranges::any_of(stop, [&](const char* s) { return ln == s; })) continue;
        if (std::islower(static_cast<unsigned char>(name[0]))) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }
    return std::nullopt;
}

}  // namespace companion
