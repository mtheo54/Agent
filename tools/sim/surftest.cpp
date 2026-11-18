// WebSurf : analyse des pages de résultats (formats DuckDuckGo lite et Bing) et liste noire.
#include "network/WebSurf.hpp"
#include <cstdio>
using namespace companion;
int main() {
    const std::string ddg = R"(<table><tr><td><a rel="nofollow" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Ffr.wikipedia.org%2Fwiki%2FRepartie&amp;rut=abc" class='result-link'>Repartie</a></td></tr>
<tr><td><a rel="nofollow" href="https://www.lemonde.fr/culture/article/2025/humour.html" class='result-link'>Humour</a></td></tr>
<tr><td><a rel="nofollow" href="https://www.instagram.com/p/xyz" class='result-link'>Insta</a></td></tr></table>)";
    for (const auto& u : ParseDdgLite(ddg)) std::printf("ddg  : %s\n", u.c_str());
    // Bing : lien direct + lien /ck/a?...&u=a1<base64url de https://www.konbini.com/fr/vannes>
    const std::string bing = R"(<li class="b_algo"><h2><a href="https://www.franceinter.fr/emissions/art-de-la-vanne" h="ID=SERP">x</a></h2></li>
<li class="b_algo"><h2><a href="https://www.bing.com/ck/a?!&amp;&amp;p=1&amp;u=a1aHR0cHM6Ly93d3cua29uYmluaS5jb20vZnIvdmFubmVz&amp;ntb=1">y</a></h2></li>
<li class="b_algo"><h2><a href="https://www.paypal.com/x">z</a></h2></li>)";
    for (const auto& u : ParseBing(bing)) std::printf("bing : %s\n", u.c_str());
    for (const char* u : {"https://mail.google.com/x", "https://www.boursorama.com", "https://fr.wikipedia.org/wiki/Vanne", "ftp://x.org", "https://discord.com/channels/@me"})
        std::printf("bloqué=%d  %s\n", SurfBlocked(u), u);
    for (const char* u : {"https://www.instagram.com/reel/C9xYz123/", "https://www.instagram.com/direct/inbox/", "https://www.instagram.com/stories/karim/123",
                          "https://www.instagram.com/karim_off/", "https://www.tiktok.com/@kev/video/7300000000000000000", "https://www.tiktok.com/@kev",
                          "https://vm.tiktok.com/ZMabc123/", "https://www.youtube.com/shorts/abcdEFGhij0", "https://www.youtube.com/watch?v=abcdEFGhij0",
                          "https://www.youtube.com/feed/history", "https://www.tiktok.com/messages?lang=fr"})
        std::printf("vidéo publique=%d  %s\n", IsSocialVideoUrl(u), u);
    const std::string ddg2 = R"(<a rel="nofollow" href="https://www.tiktok.com/@kev/video/7300000000000000001" class='result-link'>t</a>
<a rel="nofollow" href="https://www.lemonde.fr/x.html" class='result-link'>m</a>
<a rel="nofollow" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.instagram.com%2Freel%2FAbC%2F&amp;rut=1" class='result-link'>i</a>)";
    for (const auto& u : ParseDdgLite(ddg2, true)) std::printf("découverte réseaux : %s\n", u.c_str());
}
