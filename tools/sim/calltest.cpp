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
        if (!seen[spk] && std::filesystem::file_size(e.path()) > 16000 * 4 * 6) { seen[spk] = true; files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());
    files.resize(5);
    std::vector<std::vector<float>> voices; for (auto& f : files) voices.push_back(Read(f));
    std::filesystem::remove(argv[3]);
    CallPeople people; people.Load(argv[3]);
    std::mt19937 rng(7);
    std::map<int, std::string> truthToId; int ok = 0, total = 0, unsure = 0;
    const char* names[] = {"Karim", "Léa", "Yanis", "Inès", "Tom"};
    for (int turn = 0; turn < 60; ++turn) {
        const int who = turn < 5 ? turn : static_cast<int>(rng() % 5);
        const auto& v = voices[who];
        const double len = turn < 5 ? 3.0 : 1.6 + (rng() % 30) / 10.0;  // présentations de 3 s, puis phrases de 1,6 à 4,5 s
        const size_t n = static_cast<size_t>(len * 16000), st = rng() % (v.size() - n);
        auto e = id.Embed(std::vector<float>(v.begin() + st, v.begin() + st + n));
        if (!e) continue;
        auto m = people.Identify(*e, len >= 2.0, turn);
        if (!m.person || !m.sure) { ++unsure; continue; }
        if (turn < 5) { truthToId[who] = m.person->id; people.Rename(*m.person, names[who]); continue; }
        ++total; ok += truthToId[who] == m.person->id;
    }
    std::printf("fiches créées : %zu (5 voix) ; bonne personne %d/%d ; pas sûr (rien attribué) %d\n", people.Size(), ok, total, unsure);
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
