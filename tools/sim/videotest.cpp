// VideoParse sur de vraies données : recherche yt-dlp, sommes SHA-256 officielles, sous-titres json3, commentaires, fiche.
#include "modules/VideoParse.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace companion;
static std::string Read(const char* f) { std::ifstream i(f, std::ios::binary); std::stringstream s; s << i.rdbuf(); return s.str(); }
int main(int, char** argv) {
    for (const auto& v : ParseSearchLines(Read(argv[1]))) std::printf("  %s | %4d s | %8lld vues | %s\n", v.id.c_str(), v.seconds, v.views, v.title.c_str());
    std::printf("yt-dlp.exe : %s\n", ExpectedSha256(Read(argv[2]), "yt-dlp.exe").c_str());
    std::printf("yt-dlp (linux, ne doit pas être pris pour .exe) : %s\n", ExpectedSha256(Read(argv[2]), "yt-dlp").c_str());
    std::printf("deno zip   : %s\n", ExpectedSha256(Read(argv[3]), "deno-x86_64-pc-windows-msvc.zip").c_str());
    std::printf("sous-titres : « %s »\n", Json3ToText(R"({"events":[{"segs":[{"utf8":"T'as vu"},{"utf8":" ton"},{"utf8":"\nflash"}]},{"segs":[{"utf8":" "}]},{"segs":[{"utf8":"sous tour ?"}]}]})").c_str());
    std::printf("commentaires :\n%s", TopComments(R"({"comments":[{"text":"mdr il l'a détruit","like_count":420},{"text":"faible","like_count":3},{"text":"le « pas discutable » m'a tué","like_count":1200}]})", 2).c_str());
    const auto r = ParseStudy(R"({"humour": true, "technique": "retournement", "pourquoi": "il reprend les mots de l'autre pour l'enfoncer", "formes": ["Tu dis [sa phrase], moi j'entends [l'inverse].", "pas de crochet ici", "T'es [sujet] comme un [chose absurde] : ça impressionne trois secondes."]})", "https://youtu.be/x");
    std::printf("fiche : humour=%d technique=%s formes gardées=%zu\n", r.humour, r.technique.c_str(), r.forms.size());
    for (const auto& f : r.forms) std::printf("  - %s\n", f.form.c_str());
}
