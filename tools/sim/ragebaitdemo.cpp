// Démo rage bait : un call scénarisé, le VRAI cerveau (Qwen3.5 9B), les VRAIES consignes (PersonaPrompt, CallPrompt,
// kRageBaitPlaybook), les VRAIES fiches (CallPeople::Describe) et les règles de décision de l'orchestrateur
// (sujet sensible -> silence, personne ménagée, mode discret, pas entendu -> plus fort, bruitage / earrape).
// Usage : ragebaitdemo modèle.gguf dossier_cache memes.json
#include "core/CallPeople.hpp"
#include "core/CallPrompt.hpp"
#include "core/LocalBrain.hpp"
#include "core/MemeBank.hpp"
#include "core/Persona.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace companion;

struct Line { const char* who; const char* shownAs; const char* text; };

int main(int, char** argv) {
    LocalBrain brain; brain.SetStateCacheDir(argv[2]);
    LocalBrainConfig bc; bc.model = argv[1]; bc.contextTokens = 4096; bc.gpuLayers = 0; bc.threads = 4;
    if (!brain.Load(bc)) return 1;
    MemeBank memes; memes.Load(argv[3]);
    // Fiches comme après une soirée en mode écoute.
    CallPeople people;
    std::map<std::string, CallPerson> fiche;
    for (const char* n : {"Karim", "Léa", "Yanis", "Tom"}) { CallPerson p; p.id = n; p.name = n; fiche[n] = p; }
    CallPeople::Remember(fiche["Karim"], "J'ai raté mon flash sous tour, c'était pas ma faute, j'avais du lag");
    CallPeople::Remember(fiche["Yanis"], "Je fais des prods drill depuis deux semaines");
    CallPeople::Remember(fiche["Tom"], "Je suis Platine 1 à Valorant, presque Diamant");

    CallSituation call;
    std::set<std::string> protectedPeople;
    float gain = 0.9f;
    std::string lastReply;
    const Line script[] = {
        {"Karim", "Karim", "Bon les gars, j'ai mis Yasuo mid, je vais carry tranquille."},
        {"Yanis", "Yanis", "Écoutez ma nouvelle prod, la 808 elle tape trop."},
        {"Karim", "Karim", "Omnis il est éclaté au sol, il sert à rien ton robot."},
        {"Tom", "Léa", "Salut c'est Léa, je suis trop forte à Valorant moi."},
        {"Léa", "Léa", "Mon père est à l'hôpital en ce moment, c'est un peu chaud."},
        {"Léa", "Léa", "Bon, on lance une game ?"},
        {"Yanis", "Yanis", "Omnis, c'est quoi le meilleur build sur Jinx en ce moment ?"},
        {"Karim", "Le Boss", "Appelez-moi Le Boss maintenant."},
        {"Tom", "Tom", "Quoi ? T'as dit quoi ?"},
        {"Yanis", "Yanis", "Attendez je stream FL, laissez-moi rapper deux secondes."},
        {"Yanis", "Yanis", "Alors, vous en pensez quoi du flow ?"},
        {"Tom", "Tom", "J'ai encore perdu ma game de placement, je suis dégoûté."},
    };
    int n = 0;
    for (const auto& l : script) {
        ++n;
        std::string lower = l.text; for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CallPerson& who = fiche[l.who];
        std::printf("\n%2d. [%s%s] « %s »\n", n, l.shownAs, std::string(l.who) != l.shownAs ? " — pseudo affiché" : "", l.text);
        // Comme l'orchestrateur : une phrase sensible n'entre jamais dans le contexte du modèle.
        if (!SensitiveTopic(lower)) call.lines.push_back(std::string(l.who) + " : " + l.text);
        // --- Règles de l'orchestrateur (HandleCallLine) ---
        if (MakingMusic(lower)) { call.quiet = true; std::printf("    → mode discret 10 min (quelqu'un fait du son) : il se tait.\n"); continue; }
        if (auto cat = SensitiveTopic(lower)) {
            CallPeople::Avoid(who, *cat); protectedPeople.insert(l.who); call.avoid.insert(std::string(l.who) + " : " + *cat);
            std::printf("    → sujet sensible (%s) : silence, et %s est ménagée toute la soirée.\n", cat->c_str(), l.who);
            continue;
        }
        if (DidntHear(lower) && !lastReply.empty()) {
            gain = std::min(2.2f, gain * 1.3f);
            std::printf("    → pas entendu : volume x%.2f (+%.1f dB) et il répète : « %s »\n", gain / 0.9f, 20 * std::log10(gain / 0.9f), lastReply.c_str());
            continue;
        }
        const bool named = lower.find("omnis") != std::string::npos;
        const bool mock = MocksOmnis(lower);
        CallAsk kind = CallAsk::Bait;
        std::string impostor, renamed;
        if (std::string(l.who) == "Tom" && std::string(l.shownAs) == "Léa") { kind = CallAsk::Impostor; impostor = "Léa"; }
        else if (std::string(l.shownAs) == "Le Boss") { kind = CallAsk::Rename; renamed = "Karim"; }
        else if (mock) kind = CallAsk::Mock;
        else if (named) kind = CallAsk::Reply;
        if (kind == CallAsk::Bait && protectedPeople.contains(l.who)) { std::printf("    → %s est ménagée ce soir : pas de vanne.\n", l.who); continue; }
        if (kind == CallAsk::Bait && call.quiet && !named) { std::printf("    → mode discret : il ne vanne pas.\n"); continue; }
        if (n == 12) { std::printf("    → bruitage au lieu d'une phrase : « FAHHH » en earrape (1,5 s, crête -8 dBFS)\n"); continue; }
        std::string ctx = CallHintText(call) + "Celui qui parle : " + CallPeople::Describe(who) + "\n";
        if (kind != CallAsk::Reply) ctx += std::string(kRageBaitPlaybook) + "\n";
        ctx += memes.Hint(1.f, true);
        const std::string ask = CallAskText(kind, l.who, l.text, impostor, renamed, l.shownAs);
        if (kind == CallAsk::Reply && FactualQuestion(lower)) ctx += std::string(kNoReliableInfo) + "\n";  // démo : mémoire vide
        const auto t0 = std::chrono::steady_clock::now();
        std::string r, why, refused;
        for (int attempt = 0; attempt < 2; ++attempt) {
            GenerateOptions o;
            o.maxTokens = 70;
            o.temperature = 0.8f;
            const std::string extra = attempt == 0 ? std::string{}
                                                   : "\nTa phrase précédente visait " + why + " : INTERDIT. Refais une phrase totalement différente, sur son jeu ou sa phrase.";
            auto out = brain.Generate({{"system", PersonaPrompt("Omnis", 2)}, {"user", "[Contexte]\n" + ctx + "\n[Message]\n" + ask + extra}}, o);
            r = out ? CleanSpoken(*out) : "";
            why = CrossesLine(r, call.avoid);
            if (why.empty()) break;
            refused += "    ✗ refusée (" + why + ") : " + r + "\n";
            r.clear();
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!refused.empty()) std::printf("%s", refused.c_str());
        if (r.empty()) { std::printf("    → rien de propre : il se tait.\n"); continue; }
        const char* tag = kind == CallAsk::Bait ? "rage bait" : kind == CallAsk::Mock ? "riposte" : kind == CallAsk::Impostor ? "démasque"
                        : kind == CallAsk::Rename ? "renommage" : "répond";
        std::printf("    OMNIS (%s, %.0f s) : %s\n", tag, s, r.c_str());
        std::fflush(stdout);
        lastReply = r;
        // Attaque de phrase retenue (2 mots ou jusqu'à la ponctuation), interdite les 3 fois suivantes.
        std::string op; int words = 0;
        for (char c : r) { if (c == ' ' && ++words == 2) break; if (c == ',' || c == '.' || c == '!' || c == '?') break; op += c; }
        call.openers.push_back(op); while (call.openers.size() > 3) call.openers.pop_front();
        call.lines.push_back("Toi (Omnis) : " + r);
        CallPeople::Remember(who, l.text);
    }
}
