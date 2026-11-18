#include "core/RageBait.hpp"

#include "core/BaitBank.hpp"
#include "core/CallPrompt.hpp"
#include "core/LocalBrain.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace companion {
namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Mots « pleins » d'une phrase (4 lettres et plus, hors mots outils) : ce qu'une vanne précise reprend.
std::vector<std::string> ContentWords(const std::string& s) {
    static const std::set<std::string> stop = {"alors", "avec", "bien", "cette", "comme", "dans", "elle", "être", "fait", "faire", "mais",
                                               "même", "moins", "nous", "pour", "plus", "quoi", "sont", "sur", "tout", "très", "trop",
                                               "vais", "vous", "était", "c'est", "j'ai", "suis", "les gars", "bon", "voilà", "genre"};
    std::vector<std::string> out;
    std::string w;
    for (char ch : Lower(s) + " ") {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c >= 0x80 || c == '\'') {
            w += ch;
        } else {
            if (w.size() >= 4 && !stop.contains(w)) out.push_back(w);
            w.clear();
        }
    }
    return out;
}

// Part des mots de la forme (hors [crochets]) retrouvés dans la vanne : a-t-il vraiment gardé la structure ?
float FormAdherence(const std::string& form, const std::string& bait) {
    std::string skeleton;
    int depth = 0;
    for (char c : form) {
        if (c == '[') ++depth;
        else if (c == ']') depth = std::max(0, depth - 1);
        else if (!depth) skeleton += c;
    }
    const auto words = ContentWords(skeleton);
    if (words.empty()) return 1.f;
    const std::string b = Lower(bait);
    int hit = 0;
    for (const auto& w : words) hit += b.find(w) != std::string::npos;
    return static_cast<float>(hit) / static_cast<float>(words.size());
}

int Words(const std::string& s) {
    int n = 0;
    bool in = false;
    for (char c : s) {
        if (c == ' ') in = false;
        else if (!in) {
            in = true;
            ++n;
        }
    }
    return n;
}

std::uint32_t Next(std::uint32_t& r) {
    r = r * 1664525u + 1013904223u;
    return r >> 8;
}

}  // namespace

const char* BaitTechnique(int i, const char** howTo) {
    struct T {
        const char* name;
        const char* how;
    };
    static const T t[] = {
        {"contre-pied", "Affirme l'exact contraire de ce qu'il pense, avec un aplomb total, comme une évidence connue de tous."},
        {"fausse stat", "Sors une statistique ou une « étude » absurde et très précise, dite sérieusement, qui l'enfonce."},
        {"compliment empoisonné", "Fais-lui un compliment qui, en fait, l'humilie (la deuxième moitié de la phrase retourne tout)."},
        {"rappel", "Ressors précisément un de ses fails ou une phrase qu'il a dite plus tôt (voir sa fiche), pour le contredire."},
        {"fausse inquiétude", "Fais semblant de t'inquiéter sincèrement pour lui à cause de ce qu'il vient de dire, en exagérant."},
        {"comparaison absurde", "Compare-le, lui ou ce qu'il dit, à quelque chose d'inattendu et de ridicule (pas à son physique)."},
        {"accord ironique", "Sois exagérément d'accord avec lui, au point que ça devienne évidemment une moquerie."},
        {"retournement", "Reprends ses propres mots et retourne-les contre lui dans la chute."},
    };
    const T& x = t[static_cast<std::size_t>(i) % std::size(t)];
    if (howTo) *howTo = x.how;
    return x.name;
}

const std::vector<std::string>& BaitTics() {
    static const std::vector<std::string> tics = {"mec", "carré", "carrément", "l'aura", "aura", "askip", "frérot", "franchement",
                                                  "vraiment", "ouais,", "d'accord"};
    return tics;
}

int ScoreBait(const std::string& bait, const std::string& targetLine, const std::vector<std::string>& recent,
              const std::set<std::string>& avoid) {
    if (bait.empty() || !CrossesLine(bait, avoid).empty()) return -1000;
    const std::string b = " " + Lower(bait) + " ";
    int score = 0;
    // Précision : reprend au moins un mot plein de SA phrase (« Yasuo », « 808 », « carry »…).
    int hits = 0;
    for (const auto& w : ContentWords(targetLine))
        if (b.find(w) != std::string::npos) ++hits;
    score += std::min(hits, 2) * 3;
    // Tics : chaque tic coûte, plus cher s'il était déjà dans ses dernières vannes.
    for (const auto& t : BaitTics()) {
        if (b.find(" " + t) == std::string::npos) continue;
        score -= 4;  // testé : avec -2, « t'as l'aura d'un… » gagnait encore grâce aux mots repris
        for (const auto& r : recent)
            if (Lower(r).find(t) != std::string::npos) score -= 2;
    }
    // Longueur : une vanne, c'est court (8 à 22 mots ; bonus sous 16).
    const int n = Words(bait);
    if (n >= 6 && n <= 16) score += 1;
    if (n > 22) score -= (n - 22) / 3 + 2;
    if (n < 5) score -= 3;
    // Recopier sa propre vanne récente : interdit.
    for (const auto& r : recent)
        if (!r.empty() && Lower(r).substr(0, 25) == Lower(bait).substr(0, 25)) score -= 20;
    return score;
}

BaitResult WriteBait(LocalBrain& brain, const BaitRequest& req, std::uint32_t& rng, std::stop_token stop) {
    BaitResult res;
    // Exemples de TON pris ailleurs (cuisine, sport, voiture, bricolage) : ils montrent la forme, pas la phrase.
    static const char* shots =
        "Exemples du ton voulu (autres sujets, NE LES REPRENDS PAS) :\n"
        "- Il dit « j'ai réussi mes pâtes ». -> « Bravo, t'as fait bouillir de l'eau sans appeler les pompiers, on prévient Top Chef ? »\n"
        "- Il dit « je cours 10 km demain ». -> « 10 km ? Avec ta cardio, je réserve déjà le brancard au troisième. »\n"
        "- Il dit « j'ai monté mon meuble Ikea tout seul ». -> « Tout seul, et il reste que six vis : record de France. »\n";
    // Techniques tirées au hasard, toutes différentes (le « rappel » seulement si on connaît son passé).
    std::vector<int> pool;
    for (int i = 0; i < 8; ++i) {
        if (i == 3 && !req.hasHistory) continue;
        if (std::ranges::find(req.recentTechniques, std::string(BaitTechnique(i, nullptr))) != req.recentTechniques.end()) continue;
        pool.push_back(i);
    }
    std::vector<int> picks;
    while (static_cast<int>(picks.size()) < std::min<int>(req.candidates, static_cast<int>(pool.size()))) {
        const int k = pool[Next(rng) % pool.size()];
        if (std::ranges::find(picks, k) == picks.end()) picks.push_back(k);
    }
    std::string avoidWords;
    for (const auto& r : req.recent)
        if (!r.empty()) avoidWords += " « " + r.substr(0, 40) + " »";
    struct Cand {
        std::string text, tech;
        int score = 0;
    };
    std::vector<Cand> cands;
    // Vannes à partir de FORMES qui marchent (banque) : la structure et la chute sont déjà bonnes, le modèle
    // n'a qu'à y mettre SES mots. Deux formes de techniques différentes, la 1re dans la technique voulue.
    if (req.bank) {
        std::vector<std::string> usedTech;
        for (int k = 0; k < 2 && !stop.stop_requested(); ++k) {
            std::string want = k == 0 ? req.preferTechnique : std::string{};
            auto form = req.bank->Pick(want, rng);
            for (int retry = 0; form && retry < 4 && std::ranges::find(usedTech, form->technique) != usedTech.end(); ++retry)
                form = req.bank->Pick({}, rng);
            if (!form) break;
            usedTech.push_back(form->technique);
            const std::string user = "[Contexte]\n" + req.context + "\n[Message]\n" + req.ask + "\nFORME À ADAPTER (" + form->technique + ") : « " +
                                     form->form + " »\nRemplace les [crochets] par SES mots à lui (sa phrase, son jeu, son objet), garde la "
                                     "structure, les mots et la chute de la forme (ne change que les crochets), corrige la grammaire. Une seule phrase, rien d'autre.";
            GenerateOptions o;
            o.maxTokens = 48;
            o.temperature = 0.4f;
            o.stop = stop;
            const auto out = brain.Generate({{"system", req.persona}, {"user", user}}, o);
            if (!out) continue;
            Cand c{CleanSpoken(*out), "forme : " + form->technique, 0};
            if (const auto nl = c.text.find('\n'); nl != std::string::npos) c.text.resize(nl);
            if (c.text.find('[') != std::string::npos) continue;  // crochets pas remplacés : raté
            // Testé : le modèle ignore parfois la forme et ressort ses tics. Il doit en garder au moins la moitié des mots.
            if (const float adh = FormAdherence(form->form, c.text); adh < 0.5f) {
                res.tried.push_back(std::format("[{}] {} (forme pas respectée, {:.0f} %)", c.tech, c.text, adh * 100));
                continue;
            }
            c.score = ScoreBait(c.text, req.targetLine, req.recent, req.avoid);
            if (c.score > -1000) c.score += 2;  // structure écrite à la main : grammaticale et testée
            res.tried.push_back(std::format("[{}] {} ({:+})", c.tech, c.text, c.score));
            if (c.score > -1000) cands.push_back(std::move(c));
        }
        if (picks.size() > 1) picks.resize(1);  // + une vanne inventée librement, pour la surprise
    }
    for (int tech : picks) {
        if (stop.stop_requested()) return res;
        const char* how = nullptr;
        const char* name = BaitTechnique(tech, &how);
        std::string user = "[Contexte]\n" + req.context + shots + "\n[Message]\n" + req.ask + "\nTECHNIQUE IMPOSÉE : " + name + " — " + how +
                           "\nUne seule phrase de 8 à 20 mots, chute à la fin, aucune explication. N'utilise pas les mots : mec, carré, "
                           "aura, askip, frérot.";
        if (!avoidWords.empty()) user += " Ne ressemble pas à tes vannes récentes :" + avoidWords + ".";
        GenerateOptions o;
        o.maxTokens = 48;
        o.temperature = 0.9f;
        o.stop = stop;
        const auto out = brain.Generate({{"system", req.persona}, {"user", user}}, o);
        if (!out) continue;
        Cand c{CleanSpoken(*out), name, 0};
        if (const auto nl = c.text.find('\n'); nl != std::string::npos) c.text.resize(nl);
        c.score = ScoreBait(c.text, req.targetLine, req.recent, req.avoid);
        res.tried.push_back(std::format("[{}] {} ({:+})", c.tech, c.text, c.score));
        if (c.score > -1000) cands.push_back(std::move(c));
    }
    if (cands.empty()) return res;
    std::ranges::sort(cands, [](const Cand& a, const Cand& b) { return a.score > b.score; });
    // Le tri décide ; le modèle ne départage que les ex aequo (à 1 point). Testé : laissé libre, il choisit presque
    // toujours la « fausse stat », même quand une vanne plus précise est devant.
    std::vector<const Cand*> finalists;
    for (const auto& c : cands)
        if (c.score >= cands.front().score - 1 && finalists.size() < 3) finalists.push_back(&c);
    const Cand* best = finalists.front();
    if (finalists.size() > 1 && !stop.stop_requested()) {
        std::string q = "Contexte : " + req.ask + "\nVannes candidates :\n";
        for (std::size_t i = 0; i < finalists.size(); ++i) q += std::format("{}) {}\n", i + 1, finalists[i]->text);
        q += "Laquelle fait le plus rire dans un vocal entre potes ? Critères : chute SURPRENANTE (pas prévisible), PRÉCISE sur ce "
             "qu'il a dit, courte, qui pique sans blesser. Réponds uniquement par le numéro.";
        GenerateOptions o;
        o.maxTokens = 2;
        o.temperature = 0.1f;
        o.stop = stop;
        o.grammar = finalists.size() == 2 ? "root ::= [1-2]" : "root ::= [1-3]";
        if (const auto pick = brain.Generate({{"system", "Tu es un juge d'humour exigeant."}, {"user", q}}, o); pick && !pick->empty()) {
            const int k = (*pick)[0] - '1';
            if (k >= 0 && k < static_cast<int>(finalists.size())) best = finalists[static_cast<std::size_t>(k)];
        }
    }
    res.text = best->text;
    res.technique = best->tech;
    res.tried.push_back("=> retenue : [" + best->tech + "]");
    return res;
}

}  // namespace companion
