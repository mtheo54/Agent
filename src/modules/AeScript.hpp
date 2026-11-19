#pragma once
// AeScript : Omnis monte dans After Effects en écrivant un script ExtendScript (le langage de scripts d'AE), comme le
// ferait un motion designer qui automatise. Logique pure : consigne du modèle, contrôle de sécurité, enveloppe.
//
//   1) le modèle écrit le corps du script (ES3) pour la composition active ;
//   2) contrôle : pas de fichiers, pas de commandes système, pas de réseau, pas de fermeture de projet ;
//   3) enveloppe : tout dans UN groupe d'annulation (un seul Ctrl+Z défait tout), erreurs attrapées, rapport écrit
//      dans un fichier que lit Omnis (vérification), aides prêtes à l'emploi (omnisEase, omnisLayers, omnisComp) ;
//   4) lancé dans l'After Effects ouvert (AfterFX.exe -r script.jsx) ; en cas d'erreur, le modèle corrige une fois.

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace companion {

inline constexpr const char* kAeSystem =
    "Tu es un motion designer expert d'After Effects et tu écris un script ExtendScript COURT (30 lignes au plus, sans "
    "commentaires) qui fait la demande dans le projet ouvert. Réponds UNIQUEMENT avec le code (pas de texte, pas de ```), "
    "en ES3 : var seulement (pas de let, const, =>, `chaînes`), pas de try/catch ni d'undo (c'est fait pour toi).\n"
    "Utilise d'abord ces gestes de pro déjà écrits et testés (temps en secondes, f(n) = n images en secondes) :\n"
    "- omnisComp() : la composition active ; omnisLayers() : calques sélectionnés, sinon tous ; omnisAll() : tous les calques ;\n"
    "- omnisTitle(\"texte\", taille, [r,g,b]) : crée un calque texte centré, renvoie le calque ;\n"
    "- omnisPop(calque, t) : apparition en pop avec rebond (0 -> 110 -> 97 -> 100 %) et fondu, à t (défaut : début du calque) ;\n"
    "- omnisSlide(calque, \"bas\"|\"haut\"|\"gauche\"|\"droite\", t, distance) : glisse depuis ce côté avec fondu (distance 150 px par défaut) ;\n"
    "- omnisFadeOut(calque, t) : disparition en fondu (défaut : fin du calque) ;\n"
    "- omnisShake(calques, amplitude, fréquence) : tremblement de caméra (wiggle) sur des CALQUES (pas des propriétés) ; léger = 4 px, 3 ; fort = 15 px, 6 ;\n"
    "- omnisStagger(calques, images, function (calque, t) { ... }) : même geste en cascade, décalé de n images ;\n"
    "- omnisColor(calques, [r,g,b]) : couleur (0 à 1) ; texte = sa couleur, autre calque = effet Remplissage ;\n"
    "- omnisKeys(propriété, [[t, valeur], [t, valeur]...], influence) : clés + easing pro ;\n"
    "- omnisEase(propriété, influence) : easing pro sur toutes les clés (75 par défaut, 85-95 plus nerveux).\n"
    "- rapport : variable texte, mets-y une phrase courte en français sur ce que tu as fait (il la dira à voix haute).\n"
    "Sinon l'API : comp.time, comp.duration, comp.width, comp.height, comp.layers.addSolid([r,g,b], \"nom\", comp.width, "
    "comp.height, 1), comp.layers.addNull(), layer.transform.position / .scale / .opacity / .rotation, prop.setValueAtTime(t, v), "
    "prop.expression = \"wiggle(2,15)\", layer.motionBlur = true, layer.property(\"ADBE Effect Parade\").addProperty(nom) avec "
    "\"ADBE Gaussian Blur 2\", \"ADBE Drop Shadow\", \"ADBE Glo2\", \"ADBE Fill\", \"ADBE Tint\". Les clés sont numérotées à "
    "partir de 1. Texte : var st = layer.property(\"Source Text\"); var td = st.value; td.fontSize = 90; st.setValue(td).\n"
    "Exemple pour « fais apparaître les titres en cascade » : omnisStagger(omnisLayers(), 3, function (l, t) { omnisPop(l, t); }); "
    "rapport = \"Titres en pop, en cascade.\";\n"
    "Exemple pour « titre qui arrive du bas et part en fondu » : var l = omnisTitle(\"TITRE\", 120); omnisSlide(l, \"bas\"); "
    "omnisFadeOut(l); rapport = \"Titre ajouté, il monte et part en fondu.\";\n"
    "Exemple pour « fais trembler tout » : omnisShake(omnisAll(), 4, 3); rapport = \"Léger tremblement partout.\";\n"
    "Si c'est impossible ou trop vague, écris seulement : throw new Error(\"ta question courte\");";

// Recette : le modèle choisit dans un menu fermé de gestes de pro (grammaire), le code est écrit ici, donc toujours juste.
// « SCRIPT » = la demande sort du menu : on passe au script libre.
inline constexpr const char* kAeRecipeGrammar = R"(root ::= "SCRIPT\n" | cible geste geste? geste? geste?
cible ::= "CIBLE " ("selection" | "tous" | "titre " str) "\n"
geste ::= "GESTE " nom (" cascade " [1-9])? "\n"
nom ::= "couleur-" couleur | "pop" | "glisse-bas" | "glisse-haut" | "glisse-gauche" | "glisse-droite" | "fondu-entree" | "fondu-sortie" | "tremblement-leger" | "tremblement-fort" | "flou-mouvement" | "ombre" | "lueur"
couleur ::= "rouge" | "blanc" | "noir" | "jaune" | "bleu" | "vert" | "orange" | "violet" | "rose" | "gris"
str ::= "\"" [^"\n]+ "\""
)";

inline constexpr const char* kAeRecipeSystem =
    "Tu traduis une demande d'animation After Effects en recette de gestes de pro. Format, une ligne par élément :\n"
    "CIBLE selection | CIBLE tous | CIBLE titre \"TEXTE\"   (selection = calques sélectionnés ; titre = crée ce texte)\n"
    "GESTE <nom> [cascade N]   (1 à 4 gestes ; cascade N = décalé de N images d'un calque à l'autre)\n"
    "Gestes : pop (apparition avec rebond), glisse-bas / glisse-haut / glisse-gauche / glisse-droite (arrive depuis ce côté "
    "avec fondu), fondu-entree, fondu-sortie (disparaît à la fin), tremblement-leger, tremblement-fort (caméra), "
    "flou-mouvement, ombre (ombre portée), lueur, couleur-rouge / blanc / noir / jaune / bleu / vert / orange / violet / rose / "
    "gris (texte : sa couleur ; autre calque : remplissage).\n"
    "Si la demande ne se fait PAS avec ces gestes (couleur précise, effet particulier, expression, forme, masque, montage "
    "de clips…), réponds seulement SCRIPT.\n"
    "Exemple : « fais arriver les titres un par un depuis la gauche puis qu'ils partent en fondu » -> CIBLE selection / "
    "GESTE glisse-gauche cascade 3 / GESTE fondu-sortie.";

inline std::string AeQuote(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// Un geste n'est gardé que si la demande en parle (les petits modèles remplissent la recette de gestes en trop).
inline bool AeAsked(std::string_view g, const std::string& lower) {
    const auto any = [&](std::initializer_list<const char*> ws) {
        for (const char* w : ws)
            if (lower.find(w) != std::string::npos) return true;
        return false;
    };
    if (g == "pop") return any({"pop", "rebond", "rebondi", "bounce", "surgi", "jailli"}) || (any({"appara"}) && !any({"gliss", "depuis", "fondu"}));
    if (g.starts_with("glisse-")) return any({"gliss", "slide", "depuis", "arrive", "venir de", "vient de", "entre par"});
    if (g == "fondu-entree") return any({"fondu", "fade"}) && !any({"dispar", "à la fin", "a la fin", "sortie", "fade out"});
    if (g == "fondu-sortie") return any({"dispar", "à la fin", "a la fin", "sortie", "fade out", "part en fondu", "partent en fondu", "s'efface"});
    if (g.starts_with("tremblement")) return any({"trembl", "shake", "secou", "vibr"});
    if (g == "flou-mouvement") return any({"flou"});
    if (g == "ombre") return any({"ombre", "shadow"});
    if (g == "lueur") return any({"lueur", "glow", "brill", "néon", "neon"});
    if (g.starts_with("couleur-")) return lower.find(std::string(g.substr(8))) != std::string::npos;
    return false;
}

// Code de la recette (corps du script) et la phrase dite à la fin ; vide = « SCRIPT » (ou recette illisible).
// request : la demande en minuscules (garde-fou contre les gestes en trop).
inline std::string AeRecipeBody(std::string_view plan, std::string& said, const std::string& request) {
    const bool cascadeAsked = request.find("cascade") != std::string::npos || request.find("un par un") != std::string::npos ||
                              request.find("une par une") != std::string::npos || request.find("l'un après") != std::string::npos ||
                              request.find("les uns après") != std::string::npos || request.find("à la suite") != std::string::npos ||
                              request.find("décal") != std::string::npos || request.find("chacun son tour") != std::string::npos;
    bool shook = false;
    std::string target, body;
    std::vector<std::string> what;
    size_t pos = 0;
    while (pos < plan.size()) {
        size_t nl = plan.find('\n', pos);
        if (nl == std::string_view::npos) nl = plan.size();
        const std::string_view line = plan.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.starts_with("CIBLE ")) {
            const std::string_view c = line.substr(6);
            if (c == "tous") target = "var L = omnisAll();\n";
            else if (c.starts_with("titre ")) {
                std::string_view t = c.substr(6);
                if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
                target = "var L = [omnisTitle(" + AeQuote(t) + ", 120)];\n";
                what.push_back("titre « " + std::string(t) + " » ajouté");
            } else target = "var L = omnisLayers();\n";
            continue;
        }
        if (!line.starts_with("GESTE ")) continue;
        std::string_view g = line.substr(6);
        int cascade = 0;
        if (const auto c = g.find(" cascade "); c != std::string_view::npos) {
            cascade = cascadeAsked ? g[c + 9] - '0' : 0;
            g = g.substr(0, c);
        }
        if (!AeAsked(g, request) || (g.starts_with("tremblement") && std::exchange(shook, true))) continue;
        const auto each = [&](const std::string& call) {
            body += "omnisStagger(L, " + std::to_string(cascade) + ", function (l, t) { " + call + " });\n";
        };
        if (g == "pop") { each("omnisPop(l, t);"); what.push_back("pop avec rebond"); }
        else if (g.starts_with("glisse-")) {
            std::string side(g.substr(7));
            for (const char* d : {"bas", "haut", "gauche", "droite"})  // la direction dite l'emporte sur celle du modèle
                if (request.find(std::string("du ") + d) != std::string::npos || request.find(std::string("de ") + d) != std::string::npos ||
                    request.find(std::string("la ") + d) != std::string::npos || request.find(std::string("le ") + d) != std::string::npos)
                    side = d;
            each("omnisSlide(l, \"" + side + "\", t);");
            what.push_back("arrivée depuis " + std::string(side == "bas" ? "le bas" : side == "haut" ? "le haut" : side == "gauche" ? "la gauche" : "la droite"));
        } else if (g == "fondu-entree") { each("omnisFadeIn(l, t, 8);"); what.push_back("fondu d'entrée"); }
        else if (g == "fondu-sortie") { body += "for (var i = 0; i < L.length; i++) omnisFadeOut(L[i]);\n"; what.push_back("fondu de sortie"); }
        else if (g == "tremblement-leger") { body += "omnisShake(L, 4, 3);\n"; what.push_back("léger tremblement"); }
        else if (g == "tremblement-fort") { body += "omnisShake(L, 15, 6);\n"; what.push_back("gros tremblement"); }
        else if (g == "flou-mouvement") { body += "for (var i = 0; i < L.length; i++) L[i].motionBlur = true; omnisComp().motionBlur = true;\n"; what.push_back("flou de mouvement"); }
        else if (g == "ombre") { body += "for (var i = 0; i < L.length; i++) L[i].property(\"ADBE Effect Parade\").addProperty(\"ADBE Drop Shadow\");\n"; what.push_back("ombre portée"); }
        else if (g.starts_with("couleur-")) {
            static const std::pair<const char*, const char*> kRgb[] = {
                {"rouge", "[1, 0.1, 0.1]"}, {"blanc", "[1, 1, 1]"}, {"noir", "[0, 0, 0]"}, {"jaune", "[1, 0.85, 0.1]"},
                {"bleu", "[0.15, 0.4, 1]"}, {"vert", "[0.2, 0.85, 0.3]"}, {"orange", "[1, 0.5, 0.05]"}, {"violet", "[0.55, 0.2, 0.9]"},
                {"rose", "[1, 0.4, 0.7]"}, {"gris", "[0.5, 0.5, 0.5]"}};
            for (const auto& [n, rgb] : kRgb)
                if (g.substr(8) == n) {
                    body += std::string("omnisColor(L, ") + rgb + ");\n";
                    what.push_back(std::string("en ") + n);
                }
        }
        else if (g == "lueur") { body += "for (var i = 0; i < L.length; i++) L[i].property(\"ADBE Effect Parade\").addProperty(\"ADBE Glo2\");\n"; what.push_back("lueur"); }
        if (cascade && !what.empty() && g != "fondu-sortie" && !g.starts_with("tremblement") && g != "flou-mouvement" && g != "ombre" && g != "lueur")
            what.back() += " en cascade";
    }
    if (target.empty() || body.empty()) return {};
    said.clear();
    for (size_t i = 0; i < what.size(); ++i) said += (i == 0 ? "" : i + 1 == what.size() ? " et " : ", ") + what[i];
    if (!said.empty()) said[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(said[0])));
    said += ".";
    return target + body + "rapport = " + AeQuote(said) + ";";
}

// Ce qu'un script d'Omnis n'a pas le droit de faire (le reste est annulable avec Ctrl+Z). Vide = d'accord.
inline std::string AeForbidden(std::string_view code) {
    struct Rule { const char* needle; const char* why; };
    static const Rule rules[] = {
        {"system.callSystem", "lancer des commandes système"}, {"File(", "toucher aux fichiers"}, {"Folder(", "toucher aux fichiers"},
        {"Socket", "aller sur le réseau"}, {"$.evalFile", "charger un autre script"}, {"app.quit", "fermer After Effects"},
        {"app.newProject", "fermer ton projet"}, {".close(", "fermer ton projet"}, {"app.openFast", "ouvrir un autre projet"},
        {"app.open(", "ouvrir un autre projet"}, {"app.purge", "vider tes caches"}, {"app.preferences", "changer tes préférences"},
        {"app.settings", "changer tes préférences"}, {"renderQueue.render", "lancer un rendu"}, {"app.project.save", "écraser ton projet"},
    };
    for (const auto& r : rules)
        if (code.find(r.needle) != std::string_view::npos) return r.why;
    return {};
}

// Syntaxe trop récente pour ExtendScript (ES3) : le script planterait.
inline std::string AeModernSyntax(std::string_view code) {
    const auto word = [&](std::string_view w) {
        for (size_t p = code.find(w); p != std::string_view::npos; p = code.find(w, p + 1)) {
            const bool before = p == 0 || !(std::isalnum(static_cast<unsigned char>(code[p - 1])) || code[p - 1] == '_' || code[p - 1] == '.');
            const size_t e = p + w.size();
            const bool after = e < code.size() && (code[e] == ' ' || code[e] == '\t');
            if (before && after) return true;
        }
        return false;
    };
    if (word("let") || word("const")) return "let/const n'existent pas en ExtendScript (ES3) : utilise var";
    if (code.find("=>") != std::string_view::npos) return "les fonctions fléchées n'existent pas en ExtendScript (ES3) : utilise function";
    if (code.find('`') != std::string_view::npos) return "les chaînes avec ` n'existent pas en ExtendScript (ES3) : utilise des guillemets";
    return {};
}

// Retire les ```javascript … ``` que les modèles ajoutent parfois.
inline std::string AeStripFences(std::string s) {
    if (const auto p = s.find("```"); p != std::string::npos) {
        const auto nl = s.find('\n', p);
        const auto end = s.find("```", nl == std::string::npos ? p + 3 : nl);
        if (nl != std::string::npos) s = s.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1);
    }
    return s;
}

// Le script complet. resultPath : chemin du rapport avec des / (lu par Omnis : « OK|phrase » ou « ERREUR|… »).
inline std::string AeWrap(const std::string& body, const std::string& resultPath) {
    const std::string head = std::string(
               "(function () {\n"
               "var rapport = \"\";\n"
               "function omnisComp() { var c = app.project.activeItem; if (!(c instanceof CompItem)) throw new Error(\"ouvre "
               "d'abord une composition\"); return c; }\n"
               "function omnisLayers() { var c = omnisComp(); var s = c.selectedLayers; if (s.length) return s; var a = []; "
               "for (var i = 1; i <= c.numLayers; i++) a.push(c.layer(i)); return a; }\n"
               "function omnisEase(p, infl) { if (!p || !p.numKeys) return; infl = infl || 75; var t = p.propertyValueType; "
               "var n = (t == PropertyValueType.TwoD_SPATIAL || t == PropertyValueType.ThreeD_SPATIAL || t == "
               "PropertyValueType.OneD) ? 1 : (p.value.length || 1); var e = []; for (var i = 0; i < n; i++) e.push(new "
               "KeyframeEase(0, infl)); for (var k = 1; k <= p.numKeys; k++) { try { p.setInterpolationTypeAtKey(k, "
               "KeyframeInterpolationType.BEZIER); p.setTemporalEaseAtKey(k, e, e); } catch (x) {} } }\n"
               "function f(n) { return n * omnisComp().frameDuration; }\n"
               "function omnisV(p, a) { var v = p.value; if (v.length === undefined) return a[0]; var r = []; for (var i = 0; i < "
               "v.length; i++) r.push(i < a.length ? a[i] : (p.propertyValueType == PropertyValueType.ThreeD_SPATIAL ? v[i] : "
               "a[a.length - 1])); return r; }\n"
               "function omnisKeys(p, keys, infl) { for (var i = 0; i < keys.length; i++) p.setValueAtTime(keys[i][0], typeof "
               "keys[i][1] == \"number\" ? keys[i][1] : omnisV(p, keys[i][1])); omnisEase(p, infl || 80); }\n"
               "function omnisFadeIn(l, t, n) { omnisKeys(l.transform.opacity, [[t, 0], [t + f(n || 5), 100]], 70); }\n"
               "function omnisPop(l, t) { if (t === undefined) t = l.inPoint; omnisKeys(l.transform.scale, [[t, [0, 0]], [t + f(6), "
               "[110, 110]], [t + f(10), [97, 97]], [t + f(14), [100, 100]]], 85); omnisFadeIn(l, t, 3); l.motionBlur = true; "
               "omnisComp().motionBlur = true; }\n"
               "function omnisSlide(l, side, t, d) { if (t === undefined) t = l.inPoint; d = d || 150; var p = l.transform.position; "
               "var e = p.valueAtTime(t, false); var s = [e[0], e[1]]; if (side == \"bas\") s[1] += d; else if (side == \"haut\") "
               "s[1] -= d; else if (side == \"gauche\") s[0] -= d; else s[0] += d; omnisKeys(p, [[t, s], [t + f(12), [e[0], "
               "e[1]]]], 90); omnisFadeIn(l, t, 8); l.motionBlur = true; omnisComp().motionBlur = true; }\n"
               "function omnisFadeOut(l, t) { if (t === undefined) t = l.outPoint - f(10); omnisKeys(l.transform.opacity, [[t, "
               "100], [t + f(10), 0]], 70); }\n"
               "function omnisAll() { var c = omnisComp(); var a = []; for (var i = 1; i <= c.numLayers; i++) a.push(c.layer(i)); "
               "return a; }\n"
               "function omnisShake(ls, amp, freq) { if (!ls) ls = omnisAll(); if (ls.length === undefined) ls = [ls]; var n = 0; "
               "for (var i = 0; i < ls.length; i++) { var x = ls[i]; var p = x && x.transform ? x.transform.position : x; "
               "if (p && p.expression !== undefined && (!x.transform || x.transform.position == p)) { p.expression = "
               "\"wiggle(\" + (freq || 3) + \",\" + (amp || 4) + \")\"; n++; } } if (!n) throw new Error(\"aucun calque à faire "
               "trembler\"); }\n"
               "function omnisStagger(ls, n, fn) { for (var i = 0; i < ls.length; i++) fn(ls[i], ls[i].inPoint + f(n * i)); }\n"
               "function omnisColor(ls, rgb) { if (ls.length === undefined) ls = [ls]; for (var i = 0; i < ls.length; i++) { var l = "
               "ls[i]; var st = null; try { st = l.property(\"Source Text\"); } catch (x) {} if (st) { var td = st.value; "
               "td.applyFill = true; td.fillColor = rgb; st.setValue(td); } else { var e = l.property(\"ADBE Effect Parade\")"
               ".addProperty(\"ADBE Fill\"); e.property(\"Color\").setValue([rgb[0], rgb[1], rgb[2], 1]); } } }\n"
               "function omnisTitle(txt, size, color) { var c = omnisComp(); var l = c.layers.addText(txt); var st = "
               "l.property(\"Source Text\"); var td = st.value; td.fontSize = size || 100; td.fillColor = color || [1, 1, 1]; "
               "td.justification = ParagraphJustification.CENTER_JUSTIFY; st.setValue(td); var r = l.sourceRectAtTime(0, false); "
               "l.transform.anchorPoint.setValue(omnisV(l.transform.anchorPoint, [r.left + r.width / 2, r.top + r.height / 2])); "
               "l.transform.position.setValue(omnisV(l.transform.position, [c.width / 2, c.height / 2])); l.name = txt; return l; }\n"
               "function omnisPrint() { var c = app.project.activeItem; if (!(c instanceof CompItem)) return \"\"; var s = "
               "c.numLayers + \";\" + c.motionBlur; for (var i = 1; i <= c.numLayers; i++) { var l = c.layer(i); s += \"|\" + l.name "
               "+ l.inPoint + l.outPoint + l.motionBlur + (l.parent ? l.parent.index : 0); var g = l.property(\"ADBE Transform Group\"); "
               "for (var j = 1; g && j <= g.numProperties; j++) { var p = g.property(j); try { s += \",\" + p.numKeys + p.expression + "
               "p.value; } catch (x) {} } var fx = l.property(\"ADBE Effect Parade\"); if (fx) s += \"fx\" + fx.numProperties; try { "
               "var td = l.property(\"Source Text\").value; s += td.text + td.fillColor + td.fontSize + td.font; } catch (y) {} for (var k = "
               "1; fx && k <= fx.numProperties; k++) { var e = fx.property(k); for (var m = 1; m <= e.numProperties; m++) { try { s "
               "+= \";\" + e.property(m).value; } catch (z) {} } } } return s; }\n"
               "var etat = \"OK\";\n"
               "var avant = omnisPrint();\n"
               "app.beginUndoGroup(\"Omnis\");\n"
               "try {\n");
    // Numéro de ligne d'une erreur : celui du corps écrit par le modèle, pas du script enveloppé.
    const auto offset = std::to_string(std::count(head.begin(), head.end(), '\n'));
    return head + body +
           "\n} catch (err) { etat = \"ERREUR\"; rapport = (err.line ? \"ligne \" + (err.line - " + offset + ") + \" : \" : \"\") + err.toString(); }\n"
           "if (etat == \"OK\" && omnisPrint() == avant) { etat = \"ERREUR\"; rapport = \"le script n'a rien changé dans la "
           "composition (mauvais calque ou mauvaise propriété ?)\"; }\n"
           "app.endUndoGroup();\n"
           "try { var f = new File(\"" + resultPath + "\"); f.encoding = \"UTF-8\"; f.open(\"w\"); f.write(etat + \"|\" + rapport); "
           "f.close(); } catch (e2) {}\n"
           "})();\n";
}

}  // namespace companion
