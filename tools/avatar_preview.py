#!/usr/bin/env python3
"""Aperçu du personnage 2D (assets/avatar/rig.json) : rend les animations à 24 i/s.

Même algorithme de dessin que src/render/CartoonAvatar.cpp (Direct2D) :
squelette en cinématique directe, formes pleines, ombre en aplat (cel shading) par décalage,
reflet blanc, contour sombre. Sert à régler les animations sans recompiler le compagnon.

    python tools/avatar_preview.py --clip wave --out wave.mp4        # vidéo (ffmpeg)
    python tools/avatar_preview.py --sheet planche.png               # planche de poses
Dépendances : pycairo (pip install pycairo), ffmpeg pour les vidéos.
"""
import argparse, json, math, os, subprocess, sys, tempfile

import cairo

HERE = os.path.dirname(os.path.abspath(__file__))
RIG = os.path.join(HERE, "..", "assets", "avatar", "rig.json")


def ease(kind, t):
    if kind == "hold": return 0.0
    if kind == "linear": return t
    if kind == "in": return t * t * t
    if kind == "out": return 1 - (1 - t) ** 3
    return t * t * (3 - 2 * t)  # "ease" : lent-rapide-lent


def sample(clip, rest, frame):
    pose = dict(rest)
    for ch, keys in clip["keys"].items():
        if frame <= keys[0][0]: pose[ch] = keys[0][1]; continue
        if frame >= keys[-1][0]: pose[ch] = keys[-1][1]; continue
        for k0, k1 in zip(keys, keys[1:]):
            if k0[0] <= frame < k1[0]:
                t = (frame - k0[0]) / (k1[0] - k0[0])
                pose[ch] = k0[1] + (k1[1] - k0[1]) * ease(k0[2] if len(k0) > 2 else "ease", t)
                break
    return pose


def rgb(h): return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))


def rot(a, v):
    r = math.radians(a); c, s = math.cos(r), math.sin(r)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c)


def direction(a):  # 0 = vers le bas, + = vers la droite
    r = math.radians(a); return (math.sin(r), math.cos(r))


def add(p, q, k=1.0): return (p[0] + q[0] * k, p[1] + q[1] * k)


# --- Formes -----------------------------------------------------------------
def capsule(ctx, p0, p1, r0, r1):
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]; L = math.hypot(dx, dy) or 1e-3
    ang = math.atan2(dy, dx)
    ctx.new_sub_path()
    ctx.arc(p0[0], p0[1], r0, ang + math.pi / 2, ang + 3 * math.pi / 2)
    ctx.arc(p1[0], p1[1], r1, ang - math.pi / 2, ang + math.pi / 2)
    ctx.close_path()


def ellipse(ctx, c, rx, ry, angle=0.0):
    ctx.save(); ctx.translate(*c); ctx.rotate(math.radians(angle)); ctx.scale(rx, ry)
    ctx.new_sub_path(); ctx.arc(0, 0, 1, 0, 2 * math.pi); ctx.restore()


def cel(ctx, path_fn, base, shadow, offset, outline, lw, highlight_fn=None):
    """Aplat + ombre portée en croissant (forme moins forme décalée) + reflet + contour."""
    ctx.save(); path_fn(ctx); ctx.clip()
    ctx.set_source_rgb(*shadow); ctx.paint()
    ctx.save(); ctx.translate(-offset[0], -offset[1]); path_fn(ctx); ctx.restore()
    ctx.set_source_rgb(*base); ctx.fill()
    if highlight_fn: ctx.set_source_rgb(1, 1, 1); highlight_fn(ctx); ctx.fill()
    ctx.restore()
    path_fn(ctx); ctx.set_source_rgb(*outline); ctx.set_line_width(lw)
    ctx.set_line_join(cairo.LINE_JOIN_ROUND); ctx.stroke()


def draw(ctx, rig, pose, frame, emote=None, rms=0.0):
    b, pal, st = rig["body"], rig["palette"], rig["style"]
    P = {k: rgb(v) for k, v in pal.items()}
    lw = st["outline"]; so = tuple(st["shadowOffset"]); lo = tuple(st["limbShadowOffset"])
    gx, gy = rig["hips"][0], rig["ground"]
    # Écrasement / étirement autour du point au sol
    ctx.save(); ctx.translate(gx, gy); ctx.scale(1 + pose["sq"] * 0.6, 1 - pose["sq"]); ctx.translate(-gx, -gy)
    H = (rig["hips"][0] + pose["x"], rig["hips"][1] + pose["y"] - rms * 6)
    T = pose["torso"]

    def at(local): return add(H, rot(T, local))

    def arm(side):
        sh = at(b["shoulderA" if side == "a" else "shoulderB"])
        up = pose[side + "U"] + T; fo = up + pose[side + "F"]
        el = add(sh, direction(up), b["upperArm"]); ha = add(el, direction(fo), b["foreArm"])
        return sh, el, ha

    def leg(side):
        hp = add(H, b["hipA" if side == "a" else "hipB"])
        th = pose[side + "T"]; sh = th + pose[side + "S"]
        kn = add(hp, direction(th), b["thigh"]); an = add(kn, direction(sh), b["shin"])
        return hp, kn, an, pose[side + "Foot"]

    def streak(p0, p1, r):  # reflet allongé sur un membre, côté lumière (haut-gauche)
        def f(c):
            q0 = (p0[0] + (p1[0] - p0[0]) * 0.18 - r * 0.35, p0[1] + (p1[1] - p0[1]) * 0.18 - r * 0.35)
            q1 = (p0[0] + (p1[0] - p0[0]) * 0.55 - r * 0.35, p0[1] + (p1[1] - p0[1]) * 0.55 - r * 0.35)
            capsule(c, q0, q1, 2.6, 2.0)
        return f

    def draw_arm(side, back):
        base, sh_ = (P["backBase"], P["backShadow"]) if back else (P["base"], P["shadow"])
        s, e, h = arm(side)
        ur, fr = b["upperArmR"], b["foreArmR"]
        cel(ctx, lambda c: capsule(c, s, e, ur[0], ur[1]), base, sh_, lo, P["outline"], lw, None if back else streak(s, e, ur[0]))
        cel(ctx, lambda c: capsule(c, e, h, fr[0], fr[1]), base, sh_, lo, P["outline"], lw, None if back else streak(e, h, fr[0]))
        cel(ctx, lambda c: ellipse(c, h, b["handR"], b["handR"]), base, sh_, lo, P["outline"], lw,
            None if back else (lambda c: ellipse(c, add(h, (-5, -6)), 4.5, 3, -30)))

    def draw_leg(side, back):
        base, sh_ = (P["backBase"], P["backShadow"]) if back else (P["base"], P["shadow"])
        hp, kn, an, fa = leg(side)
        tr, sr = b["thighR"], b["shinR"]
        cel(ctx, lambda c: capsule(c, hp, kn, tr[0], tr[1]), base, sh_, lo, P["outline"], lw, None if back else streak(hp, kn, tr[0]))
        cel(ctx, lambda c: capsule(c, kn, an, sr[0], sr[1]), base, sh_, lo, P["outline"], lw, None if back else streak(kn, an, sr[0]))
        out = -1 if side == "a" else 1  # pieds tournés vers l'extérieur
        fc = add(an, (out * 9, 7)); ang = out * (8 + fa) if side == "a" else out * 8 + fa
        cel(ctx, lambda c: ellipse(c, fc, b["foot"][0], b["foot"][1], ang), base, sh_, lo, P["outline"], lw)

    # Ordre : bras arrière, jambes, torse, tête, bras avant
    draw_arm("b", True)
    draw_leg("b", True)
    draw_leg("a", False)

    pts = [at(p) for p in b["torso"]]
    def torso(c):
        c.new_sub_path(); c.move_to(*pts[0])
        for i in range(1, len(pts), 3): c.curve_to(*pts[i], *pts[i + 1], *pts[i + 2])
        c.close_path()
    cel(ctx, torso, P["base"], P["shadow"], so, P["outline"], lw,
        lambda c: ellipse(c, at((-27, -78)), 7, 17, T + 18))

    neck = at((0, b["neckY"]))
    hc = add(neck, rot(T + pose["head"], (0, -(b["headRadius"] + b["headGap"]))))
    hr = b["headRadius"]; ha = T + pose["head"]
    def head_hl(c):
        ellipse(c, add(hc, rot(ha, (-26, -28))), 20, 10.5, ha - 38)
        ellipse(c, add(hc, rot(ha, (-6, -44))), 4.5, 4.5)
    cel(ctx, lambda c: ellipse(c, hc, hr, hr), P["base"], P["shadow"], so, P["outline"], lw, head_hl)

    draw_arm("a", False)
    ctx.restore()

    # Émotes façon anime : hors écrasement pour rester nettes, à la position écrasée de la tête
    sx, sy = 1 + pose["sq"] * 0.6, 1 - pose["sq"]
    hc = (gx + (hc[0] - gx) * sx, gy + (hc[1] - gy) * sy)
    top = (hc[0], hc[1] - hr)
    ctx.set_source_rgb(*P["emote"]); ctx.set_line_width(3.2); ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    if emote == "dots":
        for i in range(3):  # trois points qui apparaissent l'un après l'autre
            if (frame // 8) % 4 > i:
                ctx.new_sub_path(); ctx.arc(hc[0] + 58 + i * 16, top[1] + 10 - i * 10, 4.5 + i, 0, 2 * math.pi); ctx.fill()
    elif emote == "sound":
        for i in range(3):  # ondes sonores qui partent de la tête
            ph = ((frame / 16.0) + i / 3.0) % 1.0
            ctx.set_source_rgba(*P["emote"], 1 - ph)
            ctx.new_sub_path(); ctx.arc(hc[0] - hr - 4, hc[1] - 6, 14 + ph * 26, math.radians(140), math.radians(220)); ctx.stroke()
    elif emote == "notes":
        for i in range(2):  # notes de musique qui montent
            ph = ((frame / 24.0) + i * 0.5) % 1.0
            x = hc[0] + 70 - i * 12 + math.sin(ph * 6.28) * 6; y = hc[1] - 10 - ph * 60
            ctx.set_source_rgba(*P["emote"], 1 - ph)
            ellipse(ctx, (x, y), 7, 5, -20); ctx.fill()
            ctx.move_to(x + 6, y - 1); ctx.line_to(x + 6, y - 24); ctx.line_to(x + 14, y - 18); ctx.stroke()
    elif emote == "sweat":
        ph = (frame % 24) / 24.0  # goutte de sueur anime
        x, y = hc[0] + hr * 0.75, hc[1] - hr * 0.55 + ph * 14
        ctx.set_source_rgb(*P["accent"])
        ctx.move_to(x, y - 12); ctx.curve_to(x + 8, y - 2, x + 7, y + 7, x, y + 7); ctx.curve_to(x - 7, y + 7, x - 8, y - 2, x, y - 12)
        ctx.fill_preserve(); ctx.set_source_rgb(*P["outline"]); ctx.set_line_width(2); ctx.stroke()


def render_frame(rig, clip_name, frame, scale=2, bg=True):
    w, h = rig["canvas"]
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, int(w * scale), int(h * scale))
    ctx = cairo.Context(surf); ctx.scale(scale, scale)
    if bg:
        g = cairo.LinearGradient(0, 0, 0, h); g.add_color_stop_rgb(0, 0.88, 0.93, 0.98); g.add_color_stop_rgb(1, 0.70, 0.80, 0.92)
        ctx.set_source(g); ctx.paint()
        ctx.set_source_rgba(0, 0, 0, 0.12); ellipse(ctx, (rig["hips"][0], rig["ground"] + 6), 62, 8); ctx.fill()
    clip = rig["clips"][clip_name]
    pose = sample(clip, rig["rest"], frame)
    draw(ctx, rig, pose, frame, clip.get("emote"))
    return surf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", default=RIG)
    ap.add_argument("--clip", default="wave")
    ap.add_argument("--loops", type=int, default=2)
    ap.add_argument("--out")
    ap.add_argument("--sheet")
    ap.add_argument("--sequence", help="ex. idle,wave,talk : enchaîne plusieurs animations dans une vidéo")
    a = ap.parse_args()
    rig = json.load(open(a.rig, encoding="utf-8"))
    if a.sheet:  # une image par animation (image clé du milieu)
        names = list(rig["clips"]); w, h = rig["canvas"]
        sheet = cairo.ImageSurface(cairo.FORMAT_ARGB32, w * len(names), h + 30)
        c = cairo.Context(sheet); c.set_source_rgb(1, 1, 1); c.paint()
        for i, n in enumerate(names):
            f = {"wave": 14, "train": 6}.get(n, rig["clips"][n]["frames"] // 2)
            c.set_source_surface(render_frame(rig, n, f, scale=1), i * w, 0); c.paint()
            c.set_source_rgb(0.1, 0.1, 0.1); c.select_font_face("Sans", 0, 1); c.set_font_size(16)
            c.move_to(i * w + 12, h + 22); c.show_text(n)
        sheet.write_to_png(a.sheet); return
    plan = []  # (animation, image)
    for name in (a.sequence.split(",") if a.sequence else [a.clip]):
        clip = rig["clips"][name]
        plan += [(name, f % clip["frames"]) for f in range(clip["frames"] * (1 if not clip.get("loop") else a.loops))]
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, f) in enumerate(plan):
            render_frame(rig, name, f).write_to_png(os.path.join(tmp, f"{i:04d}.png"))
        out = a.out or f"{a.sequence.replace(',', '_') if a.sequence else a.clip}.mp4"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(rig["fps"]), "-i", os.path.join(tmp, "%04d.png"),
                        "-pix_fmt", "yuv420p", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", out], check=True)
        print(out)


if __name__ == "__main__":
    main()
