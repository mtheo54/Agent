# Vidéo de démo d'Omnis à partir d'une chronologie JSON (sorties réelles du cerveau, de la vision et de la voix).
#   python3 demo_video.py timeline.json sortie.mp4
# Chaque segment : {"scene", "who", "text", "audio" (f32 24 kHz mono, optionnel), "dur", "clip", "note", "gain",
#                   "speaker" (call), "fx" ("earrape")}. Le personnage est rendu par tools/avatar_preview.py.
import importlib.util, json, math, os, subprocess, sys, tempfile
import numpy as np, cairo

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("ap", f"{REPO}/tools/avatar_preview.py")
ap = importlib.util.module_from_spec(spec); spec.loader.exec_module(ap)
rig = json.load(open(f"{REPO}/assets/avatar/rig.json", encoding="utf-8"))
FS, FPS, W, H = 48000, 24, 1280, 720
GREEN, BLUE, GREY, WHITE = (0.35, 1.0, 0.6), (0.6, 0.8, 1.0), (0.62, 0.64, 0.7), (1, 1, 1)
SCENES = {
    "intro": "OMNIS · DÉMO · tout tourne en local sur le PC",
    "tech": "MODE TECHNIQUE · il cherche, puis répond UNIQUEMENT avec la source",
    "rage": "MODE RAGE BAIT · faux expert, mauvaise foi assumée",
    "watch": "« REGARDE AVEC MOI » · il voit l'écran (vision locale) et réagit",
    "call": "CALL VOCAL · il reconnaît qui parle, vanne, ménage, se tait quand il faut",
    "outro": "OMNIS · INSTALLATEUR",
}


def load24(path, gain=1.0, rms=None):
    x = np.fromfile(path, "<f4").astype(np.float64)
    if rms:
        x *= rms / (np.sqrt(np.mean(x ** 2)) + 1e-9)
    t = np.arange(int(len(x) * 2)) / 2.0
    return np.interp(t, np.arange(len(x)), x) * gain  # 24 -> 48 kHz


def earrape(x):  # même traitement que DiscordBridge : basses x4, gain x12, écrêtage, plafond -8 dBFS, 1,5 s max
    y = x[: int(1.5 * FS)].copy(); low = 0.0
    for i in range(len(y)):
        low += 0.01 * (y[i] - low); y[i] = 0.4 * max(-1.0, min(1.0, (y[i] + 3 * low) * 12))
    return y


def wrap(c, text, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        test = (cur + " " + w).strip()
        if c.text_extents(test).x_advance > width and cur:
            lines.append(cur); cur = w
        else:
            cur = test
    return lines + ([cur] if cur else [])


def main():
    tl = json.load(open(sys.argv[1], encoding="utf-8"))
    segs, t = [], 0.6
    for s in tl["segments"]:
        a = None
        if s.get("audio"):
            a = load24(s["audio"], s.get("gain", 1.0), s.get("rms", 0.08))
            if s.get("fx") == "earrape": a = earrape(a)
        d = len(a) / FS if a is not None else s.get("dur", 2.0)
        segs.append(dict(s, t0=t, t1=t + d, pcm=a)); t += d + s.get("gap", 0.55)
    END = t + 1.0
    N = int(END * FS); mix = np.zeros(N)
    for s in segs:
        if s["pcm"] is not None:
            i = int(s["t0"] * FS); mix[i:i + len(s["pcm"])] += s["pcm"][: N - i]
    mix = np.clip(mix, -1, 1)
    stereo = np.stack([mix, mix], 1).astype("<f4")
    img = {}
    for s in segs:
        if s.get("image") and s["image"] not in img:
            img[s["image"]] = cairo.ImageSurface.create_from_png(s["image"])
    rng = np.random.default_rng(3); stars = rng.random((90, 3))
    anim, pops = ap.Animator(rig), ap.PropPops()
    people = tl.get("people", [])

    def at(tt):
        cur = None
        for s in segs:
            if s["t0"] - 0.55 <= tt: cur = s
        return cur

    with tempfile.TemporaryDirectory() as tmp:
        for fi in range(int(END * FPS)):
            tt = fi / FPS; s = at(tt) or segs[0]
            sc = s["scene"]
            S = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H); c = cairo.Context(S)
            c.set_source_rgb(0.04, 0.04, 0.06); c.paint()
            for x, y, b in stars:
                c.set_source_rgba(1, 1, 1, 0.12 + 0.4 * b * (0.6 + 0.4 * math.sin(tt * 2 + x * 9))); c.rectangle(x * W, y * H, 1.6, 1.6); c.fill()
            c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(15); c.set_source_rgb(*GREEN)
            c.move_to(40, 38); c.show_text(SCENES.get(sc, ""))
            c.set_font_size(11); c.set_source_rgb(*GREY); c.move_to(40, 58); c.show_text(tl.get("credits", ""))
            speaking = s["t0"] <= tt <= s["t1"]
            # --- Panneau de la scène (à gauche)
            px, py, pw, ph = 40, 80, 760, 440
            c.set_source_rgba(1, 1, 1, 0.05); c.rectangle(px, py, pw, ph); c.fill()
            if sc == "call":
                c.select_font_face("DejaVu Sans", 0, 1); c.set_font_size(13); c.set_source_rgb(*GREY)
                c.move_to(px + 20, py + 28); c.show_text("SALON VOCAL")
                for k, p in enumerate(people):
                    cx, cy = px + 60, py + 70 + k * 62
                    talking = speaking and s.get("speaker") == p["id"]
                    c.set_source_rgb(*p["color"]); c.arc(cx, cy, 22, 0, 2 * math.pi); c.fill()
                    if talking:
                        c.set_source_rgb(0.23, 0.65, 0.36); c.set_line_width(4); c.arc(cx, cy, 26, 0, 2 * math.pi); c.stroke()
                    c.set_source_rgb(*WHITE); c.select_font_face("DejaVu Sans", 0, 1); c.set_font_size(18)
                    c.move_to(cx + 40, cy + 6); c.show_text(s.get("shown", p["name"]) if s.get("speaker") == p["id"] and s.get("shown") else p["name"])
                    if p.get("tag"):
                        c.set_font_size(12); c.set_source_rgb(*GREY); c.move_to(cx + 40, cy + 24); c.show_text(p["tag"])
                # Journal de décision
                notes = [x for x in segs if x["scene"] == "call" and x.get("note") and x["t0"] - 0.55 <= tt][-6:]
                c.select_font_face("DejaVu Sans Mono", 0, 0); c.set_font_size(12.5)
                for k, x in enumerate(notes):
                    c.set_source_rgba(1, 0.85, 0.4, 0.95 if x is s else 0.55)
                    for j, l in enumerate(wrap(c, "› " + x["note"], 400)[:2]):
                        c.move_to(px + 340, py + 60 + k * 58 + j * 17); c.show_text(l)
            elif sc == "watch" and s.get("image") in img:
                im = img[s["image"]]; k = min((pw - 40) / im.get_width(), (ph - 120) / im.get_height())
                c.save(); c.translate(px + 20, py + 20); c.scale(k, k); c.set_source_surface(im, 0, 0); c.paint(); c.restore()
                c.set_source_rgba(0, 0, 0, 0.7); c.rectangle(px + 20, py + ph - 96, pw - 40, 84); c.fill()
                c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(12); c.set_source_rgb(*GREEN)
                c.move_to(px + 32, py + ph - 76); c.show_text("CE QU'IL VOIT (vision locale, Qwen3.5 2B + mmproj)")
                c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(13); c.set_source_rgb(*WHITE)
                for j, l in enumerate(wrap(c, tl.get("vision", ""), pw - 70)[:3]):
                    c.move_to(px + 32, py + ph - 56 + j * 18); c.show_text(l)
            else:  # discussion : bulles
                hist = [x for x in segs if x["scene"] == sc and x["t0"] - 0.55 <= tt and x.get("text")][-4:]
                y = py + 20
                blocks = []
                for x in hist:
                    c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(16)
                    ls = wrap(c, x["text"], pw - 120)
                    blocks.append((x, ls, 34 + 21 * len(ls) + (22 if x.get("note") else 0)))
                while sum(b[2] + 12 for b in blocks) > ph - 30 and len(blocks) > 1: blocks.pop(0)
                for x, ls, bh in blocks:
                    me = x["who"] == "OMNIS"; bx = px + (60 if me else 20)
                    c.set_source_rgba(*(GREEN if me else BLUE), 0.14); c.rectangle(bx, y, pw - 80, bh); c.fill()
                    c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(12); c.set_source_rgb(*(GREEN if me else BLUE))
                    c.move_to(bx + 12, y + 18); c.show_text(x["who"])
                    c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(16); c.set_source_rgb(*WHITE)
                    for j, l in enumerate(ls): c.move_to(bx + 12, y + 40 + j * 21); c.show_text(l)
                    if x.get("note"):
                        c.select_font_face("DejaVu Sans Mono", 0, 0); c.set_font_size(12); c.set_source_rgb(1, 0.85, 0.4)
                        c.move_to(bx + 12, y + bh - 10); c.show_text(x["note"][:96])
                    y += bh + 12
            # --- Personnage (à droite)
            clip = s.get("clip", "talk") if (s["who"] == "OMNIS" and speaking) else s.get("idle", "listen")
            if clip not in rig["clips"]: clip = "idle"
            cl = rig["clips"][clip]; f = int(max(0.0, tt - s["t0"]) * FPS)
            f = f % cl["frames"] if cl.get("loop") else min(f, cl["frames"] - 1)
            live = pops.step(["headset"] if sc == "call" else ["headphones"] if sc == "watch" else [])
            pose = anim.step(ap.sample(cl, rig["rest"], f))
            av = ap.render_frame(rig, clip, f, scale=1.4, bg=False, pose=pose, props=live)
            c.set_source_surface(av, 830, 70); c.paint()
            # --- Sous-titre de la réplique en cours
            if s.get("text") and s["t0"] - 0.3 <= tt <= s["t1"] + 0.4:
                c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(21)
                allw = wrap(c, s["text"], W - 180)
                if len(allw) > 2:
                    prog = min(1.0, max(0.0, (tt - s["t0"]) / max(0.1, s["t1"] - s["t0"])))
                    i = min(len(allw) - 1, int(prog * len(allw))); allw = allw[max(0, i - 1): i + 1]
                bh = 34 + 29 * len(allw)
                c.set_source_rgba(0, 0, 0, 0.8); c.rectangle(60, H - bh - 24, W - 120, bh); c.fill()
                c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(14)
                c.set_source_rgb(*(GREEN if s["who"] == "OMNIS" else BLUE)); c.move_to(80, H - bh - 4)
                c.show_text(s["who"] + (f"  (affiché « {s['shown']} »)" if s.get("shown") else ""))
                c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(21); c.set_source_rgb(*WHITE)
                for j, l in enumerate(allw): c.move_to(80, H - bh + 24 + j * 29); c.show_text(l)
            S.write_to_png(os.path.join(tmp, f"{fi:05d}.png"))
        raw = os.path.join(tmp, "a.f32"); stereo.tofile(raw)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", os.path.join(tmp, "%05d.png"),
                        "-f", "f32le", "-ar", str(FS), "-ac", "2", "-i", raw, "-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "21",
                        "-preset", "veryfast", "-c:a", "aac", "-b:a", "160k", "-shortest", sys.argv[2]], check=True)
    print(sys.argv[2], f"{END:.1f}s")


if __name__ == "__main__":
    main()
