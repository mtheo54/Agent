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
            c.voices = p.value("voices", std::vector<std::vector<float>>{});
            if (c.voices.empty() && p.contains("voice")) c.voices.push_back(p["voice"].get<std::vector<float>>());  // ancien format
            c.counts = p.value("counts", std::vector<int>(c.voices.size(), 1));
            c.counts.resize(c.voices.size(), 1);
            c.samples = p.value("samples", 0);
            for (const auto& s : p.value("said", std::vector<std::string>{})) c.said.push_back(s);
            for (const auto& s : p.value("mocks", std::vector<std::string>{})) c.mocks.push_back(s);
            c.avoid = p.value("avoid", std::vector<std::string>{});
            for (const auto& a : p.value("aliases", std::vector<std::string>{})) c.aliases.push_back(a);
            c.avatar = p.value("avatar", std::vector<std::uint8_t>{});
            c.lastSeen = p.value("lastSeen", std::int64_t{0});
            if (!c.id.empty() && !c.voices.empty()) people_.push_back(std::move(c));
        }
    } catch (const std::exception&) {
        people_.clear();
        return false;
    }
    MergeDuplicates();
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
                               {"voices", p.voices},
                               {"counts", p.counts},
                               {"samples", p.samples},
                               {"said", std::vector<std::string>(p.said.begin(), p.said.end())},
                               {"mocks", std::vector<std::string>(p.mocks.begin(), p.mocks.end())},
                               {"avoid", p.avoid},
                               {"aliases", std::vector<std::string>(p.aliases.begin(), p.aliases.end())},
                               {"avatar", p.avatar},
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

float CallPerson::Score(const std::vector<float>& emb) const {
    float best = 0.f;
    for (const auto& v : voices) best = std::max(best, Cosine(emb, v));
    return best;
}

// Ajoute la phrase à l'empreinte la plus proche (moyenne glissante), ou en crée une nouvelle si elle est différente
// (autre micro, voix qui crie) ; 5 au plus, la moins utilisée est remplacée.
void CallPeople::Learn(CallPerson& p, const std::vector<float>& emb) {
    int nearest = -1;
    float sim = -1.f;
    for (std::size_t i = 0; i < p.voices.size(); ++i)
        if (const float s = Cosine(emb, p.voices[i]); s > sim) {
            sim = s;
            nearest = static_cast<int>(i);
        }
    ++p.samples;
    if (nearest >= 0 && sim >= 0.7f) {
        auto& v = p.voices[static_cast<std::size_t>(nearest)];
        int& n = p.counts[static_cast<std::size_t>(nearest)];
        const float w = 1.f / static_cast<float>(std::min(n, 19) + 1);
        double norm = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = (1.f - w) * v[i] + w * emb[i];
            norm += static_cast<double>(v[i]) * v[i];
        }
        const float inv = norm > 0 ? static_cast<float>(1.0 / std::sqrt(norm)) : 1.f;
        for (auto& x : v) x *= inv;
        ++n;
        return;
    }
    if (p.voices.size() < 5) {
        p.voices.push_back(emb);
        p.counts.push_back(1);
        return;
    }
    const auto least = std::ranges::min_element(p.counts) - p.counts.begin();
    p.voices[static_cast<std::size_t>(least)] = emb;
    p.counts[static_cast<std::size_t>(least)] = 1;
}

CallPerson& CallPeople::Create(const std::vector<float>& emb, std::int64_t now) {
    CallPerson p;
    p.id = "p" + std::to_string(nextId_++);
    p.voices.push_back(emb);
    p.counts.push_back(1);
    p.samples = 1;
    p.lastSeen = now;
    people_.push_back(std::move(p));
    return people_.back();
}

CallPeople::Match CallPeople::Identify(const std::vector<float>& emb, bool allowCreate, std::int64_t now) {
    Match m;
    float second = 0.f;
    for (auto& p : people_) {
        const float s = p.Score(emb);
        if (s > m.score) {
            second = m.score;
            m.score = s;
            m.person = &p;
        } else if (s > second) {
            second = s;
        }
    }
    const float margin = m.score - second;
    if (m.person && m.score >= SpeakerId::kSame && margin >= SpeakerId::kMargin) {
        m.sure = true;
        m.strong = m.score >= SpeakerId::kStrong && margin >= SpeakerId::kStrongMargin;
        if (m.strong) Learn(*m.person, emb);  // n'apprend que de ce qui est sans ambiguïté
        m.person->lastSeen = now;
        return m;
    }
    if ((!m.person || m.score < SpeakerId::kOther) && allowCreate && people_.size() < 40) {
        m.person = &Create(emb, now);
        m.score = 1.f;
        m.sure = m.strong = true;
        return m;
    }
    m.sure = m.strong = false;  // entre deux : on ne sait pas, on n'attribue rien
    return m;
}

namespace {
bool SameAvatarSig(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != 48 || b.size() != 48) return false;
    int diff = 0;
    for (std::size_t i = 0; i < 48; ++i) diff += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return diff / 48 < 16;
}
}  // namespace

CallPeople::Match CallPeople::Resolve(const std::vector<float>* emb, const std::string& shownName,
                                      const std::vector<std::uint8_t>& avatar, std::int64_t now) {
    Match m;
    m.fromDiscord = m.sure = true;
    // 1) La voix, si elle est nette (seuil fort + marge) : un pseudo se change en 2 s, pas une voix.
    CallPerson* byVoice = nullptr;   // voix nette : assez pour démasquer un faux pseudo
    CallPerson* voiceSure = nullptr;  // voix sûre : assez pour suivre quelqu'un qui prend un pseudo neuf
    if (emb) {
        float best = 0.f, second = 0.f;
        CallPerson* top = nullptr;
        for (auto& p : people_) {
            const float sc = p.Score(*emb);
            if (sc > best) {
                second = best;
                best = sc;
                top = &p;
            } else if (sc > second) {
                second = sc;
            }
        }
        if (best >= SpeakerId::kStrong && best - second >= SpeakerId::kStrongMargin) byVoice = top;
        if (best >= SpeakerId::kSame && best - second >= SpeakerId::kMargin) voiceSure = top;
    }
    CallPerson* byName = ByName(shownName);
    // 2) L'avatar, seulement s'il n'appartient qu'à une personne (les avatars par défaut se ressemblent tous).
    CallPerson* byAvatar = nullptr;
    if (avatar.size() == 48) {
        int matches = 0;
        for (auto& p : people_)
            if (SameAvatarSig(p.avatar, avatar)) {
                byAvatar = &p;
                ++matches;
            }
        if (matches != 1) byAvatar = nullptr;
    }
    CallPerson* p = nullptr;
    if (byVoice) {
        p = byVoice;
        if (byName && byName != p) m.impostorOf = byName->Label();  // il a pris le pseudo d'un autre : sa voix le trahit
    } else if (byAvatar) {
        p = byAvatar;
    } else if (byName) {
        p = byName;
    } else if (voiceSure) {
        p = voiceSure;  // pseudo neuf, à personne : c'est lui qui s'est renommé
    } else if (emb && !shownName.empty()) {
        // Pseudo neuf, voix pas encore nette sur UNE phrase : on attend quelques phrases et on compare leur moyenne
        // (bien plus fiable). Renommage d'un pote connu, ou vraie nouvelle personne au bout de 4 phrases.
        auto it = std::ranges::find(pending_, shownName, &std::pair<std::string, std::vector<std::vector<float>>>::first);
        if (it == pending_.end()) {
            pending_.emplace_back(shownName, std::vector<std::vector<float>>{});
            it = std::prev(pending_.end());
        }
        it->second.push_back(*emb);
        std::vector<float> mean(emb->size(), 0.f);
        for (const auto& e : it->second)
            for (std::size_t i = 0; i < mean.size(); ++i) mean[i] += e[i];
        float best = 0.f, second = 0.f;
        CallPerson* top = nullptr;
        for (auto& q : people_) {
            const float sc = q.Score(mean);  // cosinus : la moyenne n'a pas besoin d'être normalisée
            if (sc > best) {
                second = best;
                best = sc;
                top = &q;
            } else if (sc > second) {
                second = sc;
            }
        }
        bool created = false;
        if (top && it->second.size() >= 2 && best >= SpeakerId::kSame && best - second >= SpeakerId::kMargin) {
            p = top;  // c'était lui, renommé
        } else if (it->second.size() >= 4) {
            p = &Create(it->second.front(), now);  // vraiment quelqu'un de nouveau
            created = true;
        } else {
            m.person = nullptr;  // pas encore décidé : le pseudo affiché sert d'étiquette, rien n'est appris
            m.sure = m.strong = false;
            return m;
        }
        const auto learned = std::move(it->second);
        pending_.erase(it);
        // Les phrases en attente sont apprises (la 1re a servi à créer la fiche ; la dernière est apprise plus bas).
        for (std::size_t i = created ? 1 : 0; i + 1 < learned.size(); ++i) Learn(*p, learned[i]);
    } else {
        // Nouveau pseudo, voix et avatar pas (encore) reconnus : une fiche sans nom à qui la voix ressemble, sinon une neuve.
        CallPerson* unnamed = nullptr;
        float best = 0.f;
        if (emb)
            for (auto& q : people_)
                if (q.name.empty())
                    if (const float sc = q.Score(*emb); sc > best) {
                        best = sc;
                        unnamed = &q;
                    }
        if (unnamed && best >= SpeakerId::kStrong) p = unnamed;
        else if (emb) p = &Create(*emb, now);
        else {
            people_.push_back(CallPerson{});
            p = &people_.back();
            p->id = "p" + std::to_string(nextId_++);
        }
    }
    if (m.impostorOf.empty() && !shownName.empty() && p->name != shownName) {
        if (!p->name.empty()) {
            m.renamedFrom = p->name;
            if (std::ranges::find(p->aliases, p->name) == p->aliases.end()) p->aliases.push_back(p->name);
            while (p->aliases.size() > 6) p->aliases.pop_front();
        }
        Rename(*p, shownName);
    }
    if (emb) Learn(*p, *emb);
    if (avatar.size() == 48 && m.impostorOf.empty()) p->avatar = avatar;
    p->lastSeen = now;
    m.person = p;
    m.strong = true;
    m.score = 1.f;
    return m;
}

void CallPeople::MergeDuplicates() {
    // Par indices : deque::erase au milieu invalide TOUS les itérateurs et références.
    for (std::size_t ia = 0; ia < people_.size(); ++ia)
        for (std::size_t ib = ia + 1; ib < people_.size();) {
            CallPerson& keep = people_[ia];
            CallPerson& gone = people_[ib];
            float sim = 0.f;
            for (const auto& va : keep.voices)
                for (const auto& vb : gone.voices) sim = std::max(sim, Cosine(va, vb));
            // Très strict (deux voix différentes ne dépassent pas ~0,56 même quand elles se ressemblent).
            if (sim < 0.72f || keep.voices.empty() || gone.voices.empty()) {
                ++ib;
                continue;
            }
            if (keep.name.empty()) keep.name = gone.name;
            else if (!gone.name.empty() && gone.name != keep.name) keep.aliases.push_back(gone.name);
            for (auto& al : gone.aliases) keep.aliases.push_back(al);
            while (keep.aliases.size() > 6) keep.aliases.pop_front();
            for (auto& l : gone.said) Remember(keep, l);
            for (auto& l : gone.mocks) Mocked(keep, l);
            for (auto& c : gone.avoid) Avoid(keep, c);
            for (const auto& v : gone.voices) Learn(keep, v);
            if (keep.avatar.empty()) keep.avatar = gone.avatar;
            keep.samples += gone.samples;
            people_.erase(people_.begin() + static_cast<std::ptrdiff_t>(ib));  // ib reste sur l'élément suivant
        }
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
    if (!p.aliases.empty()) {
        s += " — s'est déjà fait appeler (renommages pour rigoler) :";
        for (const auto& a : p.aliases) s += " « " + a + " »";
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
