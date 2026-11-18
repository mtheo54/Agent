// Call simulé avec de vraies voix (CAM++ en C++ : SpeakerId + CallPeople) : qui parle, prénoms, moqueries,
// sujets sensibles, sauvegarde, « oublie-moi ». Arguments : modèle.onnx dossier_f32 fichier.json
#include "audio/SpeakerId.hpp"
#include "core/CallPeople.hpp"
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
using namespace companion;
static std::vector<float> Read(const std::filesystem::path& f) {
    std::vector<float> x; FILE* h = std::fopen(f.c_str(), "rb"); float v; while (std::fread(&v, 4, 1, h) == 1) x.push_back(v); std::fclose(h); return x;
}
int main(int, char** argv) {
    SpeakerId id; if (!id.Load(argv[1])) return 1;
    std::vector<std::filesystem::path> files;
    std::map<std::string, bool> seen;
    for (auto& e : std::filesystem::directory_iterator(argv[2])) {
        std::string b = e.path().filename().string(); if (b.rfind("r_", 0) == 0) b = b.substr(2);
        const std::string spk = b.substr(0, b.find('_'));
        if (!seen[spk] && std::filesystem::file_size(e.path()) > 480000) { seen[spk] = true; files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());
    files.resize(5);
    std::vector<std::vector<float>> voices; for (auto& f : files) voices.push_back(Read(f));
    std::filesystem::remove(argv[3]);
    CallPeople people; people.Load(argv[3]);
    std::mt19937 rng(7);
    const char* names[] = {"Karim", "Léa", "Yanis", "Inès", "Tom"};
    // Bruit de fond (micro bas de gamme) ajouté par-dessus le fichier déjà dégradé.
    std::normal_distribution<float> noise(0.f, 1.f);
    const auto phrase = [&](int who, double len, bool firstHalf) {
        const auto& v = voices[who];
        const size_t n = static_cast<size_t>(len * 16000), half = v.size() / 2;
        const size_t st = firstHalf ? rng() % (half - n) : half + rng() % (v.size() - half - n);
        std::vector<float> x(v.begin() + st, v.begin() + st + n);
        double p = 0; for (float y : x) p += y * y; const float sd = static_cast<float>(std::sqrt(p / n / 15.85));  // SNR 12 dB
        for (auto& y : x) y += sd * noise(rng);
        return x;
    };
    // Phase 1 : Discord visible, 6 phrases par personne étiquetées par le pseudo (cercle vert).
    for (int round = 0; round < 6; ++round)
        for (int who = 0; who < 5; ++who)
            if (auto e = id.Embed(phrase(who, 1.6 + (rng() % 18) / 10.0, true))) people.Resolve(&*e, names[who], {}, round);
    // Phase 2 : Discord caché (en jeu) : la voix seule, sur d'autres phrases.
    int ok = 0, wrong = 0, unsure = 0, strongWrong = 0;
    for (int turn = 0; turn < 150; ++turn) {
        const int who = static_cast<int>(rng() % 5);
        auto e = id.Embed(phrase(who, 1.6 + (rng() % 18) / 10.0, false));
        if (!e) continue;
        auto m = people.Identify(*e, false, 100 + turn);
        if (!m.sure) { ++unsure; continue; }
        const bool right = m.person->name == names[who];
        right ? ++ok : ++wrong;
        if (!right && m.strong) ++strongWrong;
    }
    std::printf("micros dégradés : fiches %zu ; à la voix seule : juste %d, FAUX %d (dont %d « sûr pour une moquerie »), pas sûr %d\n",
                people.Size(), ok, wrong, strongWrong, unsure);
    // Blagues : Karim se renomme « Le Boss », puis Tom prend le pseudo « Léa ».
    {
        CallPeople::Match m;
        int phrases = 0;
        for (; phrases < 4 && !m.person; ++phrases) { auto e = id.Embed(phrase(0, 2.5, false)); m = people.Resolve(&*e, "Le Boss", {}, 500 + phrases); }
        std::printf("Karim -> « Le Boss » : décidé après %d phrase(s), fiches %zu, reconnu comme %s, ancien pseudo « %s »\n", phrases,
                    people.Size(), m.person->name.c_str(), m.renamedFrom.c_str());
        auto e2 = id.Embed(phrase(4, 3.0, false));
        auto m2 = people.Resolve(&*e2, "Léa", {}, 501);
        std::printf("Tom se fait appeler « Léa » : reconnu comme %s, usurpe %s ; la vraie Léa garde son nom : %s\n", m2.person->name.c_str(),
                    m2.impostorOf.empty() ? "personne" : m2.impostorOf.c_str(), people.ByName("Léa") ? "oui" : "non");
        people.Rename(*people.ByName("Le Boss"), "Karim");
        // Chacun se renomme à son tour (3 fois chacun) : retrouvé sous sa vraie identité, ou nouvelle fiche à tort ?
        int found = 0, dup = 0, waited = 0;
        for (int rep = 0; rep < 3; ++rep)
            for (int w = 0; w < 5; ++w) {
                const std::string orig = people.ByName(names[w]) ? names[w] : "";
                const std::string alias = "Alias" + std::to_string(rep * 5 + w);
                CallPeople::Match mm;
                int ph = 0;
                for (; ph < 4 && !mm.person; ++ph) { auto e = id.Embed(phrase(w, 1.8 + (rng() % 15) / 10.0, false)); mm = people.Resolve(&*e, alias, {}, 600); }
                waited += ph;
                if (mm.person && mm.renamedFrom == names[w]) ++found; else ++dup;
                if (mm.person) people.Rename(*mm.person, names[w]);  // il reprend son pseudo
                (void)orig;
            }
        std::printf("15 renommages : retrouvés %d, nouvelle fiche à tort %d (%.1f phrase(s) en moyenne avant de décider), fiches %zu\n",
                    found, dup, waited / 15.0, people.Size());
    }
    CallPerson* k = people.ByName("karim");
    CallPeople::Mocked(*k, "Omnis il est éclaté au sol mdr");
    CallPeople::Remember(*k, "J'ai raté mon flash sous tour");
    if (auto c = SensitiveTopic("mon père est malade, il est à l'hôpital")) CallPeople::Avoid(*people.ByName("léa"), *c);
    people.Save();
    CallPeople again; again.Load(argv[3]);
    std::printf("rechargé : %zu fiches\n  %s\n  %s\n", again.Size(), CallPeople::Describe(*again.ByName("Karim")).c_str(),
                CallPeople::Describe(*again.ByName("Léa")).c_str());
    again.Forget(again.ByName("Tom")->id);
    CallPeople third; third.Load(argv[3]);
    std::printf("après « Omnis, oublie-moi » de Tom : %zu fiches, Tom %s\n", third.Size(), third.ByName("Tom") ? "encore là" : "effacé");
    for (const char* s : {"moi c'est karim", "je m'appelle Inès", "moi c'est pas grave", "Omnis il sert à rien", "quoi ?",
                          "il est plus fort que toi", "laissez-moi rapper deux secondes", "il est parti manger", "j'ai été viré du taf"}) {
        std::string l = s; for (auto& c : l) c = std::tolower(static_cast<unsigned char>(c));
        auto intro = SelfIntroduction(s); auto sens = SensitiveTopic(l);
        std::printf("  « %s » : prénom=%s moquerie=%d pas_entendu=%d musique=%d sensible=%s\n", s, intro ? intro->c_str() : "-",
                    MocksOmnis(l), DidntHear(l), MakingMusic(l), sens ? sens->c_str() : "-");
    }
}
