# Démo du mode studio : il sort son tableau (vrai code StudioBoard, rendu sous Wine), le vu-mètre suit la vraie musique,
# il danse quand ça joue, puis donne son avis sur la prod puis sur le mix (voix Qwen3-TTS, répliques du vrai cerveau).
#   python3 studio_demo.py levels dossier        -> dossier/levels.txt (une ligne par image : voulu rmsDb crêteDb)
#   python3 studio_demo.py compose dossier out.mp4  (dossier/board/%05d.png déjà rendus par boardframes.exe)
# dossier contient : beat.wav (48 kHz), v1.f32 v2.f32 v3.f32 (voix, 24 kHz mono).
import importlib.util, json, math, os, subprocess, sys, tempfile
import numpy as np, cairo

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("ap", f"{REPO}/tools/avatar_preview.py")
ap = importlib.util.module_from_spec(spec); spec.loader.exec_module(ap)
rig = json.load(open(f"{REPO}/assets/avatar/rig.json", encoding="utf-8"))
FS, FPS, W, H = 48000, 24, 1280, 720
GREEN, BLUE, GREY, WHITE = (0.35, 1.0, 0.6), (0.6, 0.8, 1.0), (0.62, 0.64, 0.7), (1, 1, 1)
MUSIC_T0 = 5.4

# Répliques : (début, qui, texte, audio)
LINES = [
    (0.6, "TOI", "Omnis, écoute ma prod.", None),
    (2.4, "OMNIS", "Vas-y, lance-la ! Je sors mon tableau.", "v1.f32"),
    (16.4, "TOI", "Alors, t'en penses quoi ?", None),
    (18.0, "OMNIS", "Ouais, ça claque carrément ! L'énergie est lourde et ça pousse. Les basses sont un peu envahissantes, "
                    "mais c'est ce qui donne le punch.", "v2.f32"),
    (26.4, "TOI", "Et le mix ?", None),
    (27.8, "OMNIS", "Sur le mix : la 808 écrase tout. Baisse-la de quelques dB dans le mixeur, et ça va respirer.", "v3.f32"),
]
BOARD_IN, BOARD_OUT = 3.6, 35.6  # le tableau sort pendant « je sors mon tableau », se range à la fin
END = 37.5


def voice(d, name):
    x = np.fromfile(os.path.join(d, name), "<f4").astype(np.float64)
    x *= 0.12 / (np.sqrt(np.mean(x ** 2)) + 1e-9)
    t = np.arange(len(x) * 2) / 2.0
    return np.interp(t, np.arange(len(x)), x)  # 24 -> 48 kHz


def music(d):
    out = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", os.path.join(d, "beat.wav"), "-ac", "2", "-ar", str(FS), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    m = np.frombuffer(out, "<f4").reshape(-1, 2).astype(np.float64)
    return m * (0.25 / (np.sqrt(np.mean(m ** 2)) + 1e-9))  # niveau d'une prod en cours (~ -12 dBFS RMS)


def timeline(d):
    segs = []
    for t0, who, text, a in LINES:
        dur = len(voice(d, a)) / FS if a else 1.4
        segs.append({"t0": t0, "t1": t0 + dur, "who": who, "text": text, "audio": a})
    return segs


def levels(d):
    m = music(d); n = int(END * FPS); out = []
    for fi in range(n):
        tt = fi / FPS; i = int((tt - MUSIC_T0) * FS)
        rms = peak = -90.0
        if 0 <= i < len(m):
            w = m[max(0, i - int(0.3 * FS)):i + 1]  # 300 ms : sonie court terme, comme l'analyseur
            rms = 10 * math.log10(np.mean(w ** 2) + 1e-12)
            pk = np.max(np.abs(m[max(0, i - int(0.1 * FS)):i + 1])); peak = 20 * math.log10(pk + 1e-9)
        wanted = 1 if BOARD_IN <= tt < BOARD_OUT else 0
        out.append(f"{wanted} {rms:.2f} {peak:.2f}")
    open(os.path.join(d, "levels.txt"), "w").write("\n".join(out) + "\n")
    return [tuple(float(v) for v in l.split()) for l in out]


def wrap(c, text, width):
    lines, cur = [], ""
    for w in text.split():
        t = (cur + " " + w).strip()
        if c.text_extents(t).x_advance > width and cur: lines.append(cur); cur = w
        else: cur = t
    return lines + ([cur] if cur else [])


def compose(d, dst):
    segs = timeline(d); lv = levels(d); m = music(d)
    mix = np.zeros((int(END * FS) + FS, 2))
    i0 = int(MUSIC_T0 * FS); mix[i0:i0 + len(m)] += m * 0.6
    for s in segs:
        if s["audio"]:
            v = voice(d, s["audio"]); j = int(s["t0"] * FS); mix[j:j + len(v), 0] += v; mix[j:j + len(v), 1] += v
    mix = np.clip(mix, -1, 1)
    music_end = MUSIC_T0 + len(m) / FS
    anim = ap.Animator(rig); pops = ap.PropPops()
    rng = np.random.default_rng(5); stars = rng.random((80, 3))
    blocks = [(0, 0.0, 0.5), (1, 0.0, 1.0), (2, 0.25, 1.0), (3, 0.5, 1.0), (4, 0.0, 0.25), (4, 0.5, 0.75), (5, 0.75, 1.0)]
    cols = [(0.29, 0.44, 0.75), (0.39, 0.42, 0.7), (0.45, 0.45, 0.6), (0.28, 0.45, 0.5), (0.35, 0.45, 0.4), (0.45, 0.45, 0.32)]
    with tempfile.TemporaryDirectory() as tmp:
        for fi in range(int(END * FPS)):
            tt = fi / FPS
            S = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H); c = cairo.Context(S)
            c.set_source_rgb(0.04, 0.04, 0.06); c.paint()
            for x, y, b in stars:
                c.set_source_rgba(1, 1, 1, 0.1 + 0.35 * b * (0.6 + 0.4 * math.sin(tt * 2 + x * 9))); c.rectangle(x * W, y * H, 1.6, 1.6); c.fill()
            c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(15); c.set_source_rgb(*GREEN)
            c.move_to(40, 38); c.show_text("OMNIS · MODE STUDIO · il écoute ta prod, sort son tableau et danse")
            c.set_font_size(11); c.set_source_rgb(*GREY); c.move_to(40, 58)
            c.show_text("Tableau : vrai code d'Omnis (StudioBoard). Voix : Qwen3-TTS en local. Avis : sortie réelle du cerveau 9B.")
            # --- Fenêtre FL Studio (playlist + tête de lecture)
            px, py, pw, ph = 40, 80, 720, 250
            c.set_source_rgb(0.13, 0.14, 0.17); c.rectangle(px, py, pw, ph); c.fill()
            c.set_source_rgb(0.2, 0.21, 0.25); c.rectangle(px, py, pw, 26); c.fill()
            c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(13); c.set_source_rgb(0.85, 0.85, 0.9)
            c.move_to(px + 12, py + 18); c.show_text("FL Studio — ma_prod.flp · Playlist")
            for row, a, b in blocks:
                c.set_source_rgb(*cols[row]); c.rectangle(px + 20 + a * (pw - 40), py + 40 + row * 33, (b - a) * (pw - 40) - 4, 26); c.fill()
            playing = MUSIC_T0 <= tt < music_end
            if MUSIC_T0 <= tt:
                u = min(1.0, (tt - MUSIC_T0) / (music_end - MUSIC_T0))
                c.set_source_rgb(0.4, 1.0, 0.55); c.rectangle(px + 20 + u * (pw - 40), py + 32, 2, ph - 40); c.fill()
            c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(12)
            c.set_source_rgb(*(GREEN if playing else GREY)); c.move_to(px + pw - 120, py + 18)
            c.show_text("▶ LECTURE" if playing else "■ ARRÊT")
            # --- Conversation (bulles)
            hist = [s for s in segs if s["t0"] - 0.2 <= tt][-3:]
            y = py + ph + 22
            for s in hist:
                me = s["who"] == "OMNIS"; bx = px + (50 if me else 0)
                c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(16)
                ls = wrap(c, s["text"], pw - 90); bh = 34 + 21 * len(ls)
                c.set_source_rgba(*(GREEN if me else BLUE), 0.14); c.rectangle(bx, y, pw - 50, bh); c.fill()
                c.select_font_face("DejaVu Sans Mono", 0, 1); c.set_font_size(12); c.set_source_rgb(*(GREEN if me else BLUE))
                c.move_to(bx + 12, y + 18); c.show_text(s["who"])
                c.select_font_face("DejaVu Sans", 0, 0); c.set_font_size(16); c.set_source_rgb(*WHITE)
                for j, l in enumerate(ls): c.move_to(bx + 12, y + 40 + j * 21); c.show_text(l)
                y += bh + 10
            # --- Personnage : tableau derrière lui, puis lui
            cur = next((s for s in segs if s["t0"] <= tt <= s["t1"]), None)
            rms = lv[fi][1]
            if cur and cur["who"] == "OMNIS": clip = "talk"
            elif cur and cur["who"] == "TOI": clip = "listen"
            elif BOARD_IN <= tt < BOARD_OUT and rms > -30: clip = "dance"
            else: clip = "studio"
            cl = rig["clips"][clip]; f = fi % cl["frames"] if cl.get("loop") else min(fi, cl["frames"] - 1)
            pose = anim.step(ap.sample(cl, rig["rest"], f))
            ax, ay = 790, 70
            bp = os.path.join(d, "board", f"{fi:05d}.png")
            if os.path.exists(bp):
                c.set_source_surface(cairo.ImageSurface.create_from_png(bp), ax, ay); c.paint()
            av = ap.render_frame(rig, clip, f, scale=1.4, bg=False, pose=pose, props=pops.step(["headphones"]))
            c.set_source_surface(av, ax, ay); c.paint()
            # Bulle sous lui (comme dans l'appli) quand il parle
            if cur and cur["who"] == "OMNIS":
                c.select_font_face("DejaVu Sans", 0, 1); c.set_font_size(15)
                ls = wrap(c, cur["text"], 380)
                prog = (tt - cur["t0"]) / max(0.1, cur["t1"] - cur["t0"]); k = min(len(ls) - 1, int(prog * len(ls)))
                ls = ls[max(0, k - 1):k + 1]
                bw = max(c.text_extents(l).x_advance for l in ls) + 28; bh = 22 * len(ls) + 18
                bx = ax + 224 - bw / 2; by = min(ay + 575, H - bh - 10)
                c.set_source_rgba(0.04, 0.04, 0.06, 0.85); c.rectangle(bx, by, bw, bh); c.fill()
                c.set_source_rgba(0.35, 1, 0.6, 0.6); c.set_line_width(1); c.rectangle(bx, by, bw, bh); c.stroke()
                c.set_source_rgb(*WHITE)
                for j, l in enumerate(ls):
                    e = c.text_extents(l); c.move_to(ax + 224 - e.x_advance / 2, by + 26 + j * 22); c.show_text(l)
            S.write_to_png(os.path.join(tmp, f"{fi:05d}.png"))
        mix.astype("<f4").tofile(os.path.join(tmp, "a.f32"))
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", str(FPS), "-i", os.path.join(tmp, "%05d.png"),
                        "-f", "f32le", "-ar", str(FS), "-ac", "2", "-i", os.path.join(tmp, "a.f32"),
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-c:a", "aac", "-b:a", "192k", "-shortest", dst], check=True)


if __name__ == "__main__":
    if sys.argv[1] == "levels": levels(sys.argv[2])
    else: compose(sys.argv[2], sys.argv[3])
