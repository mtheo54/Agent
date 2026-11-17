#!/usr/bin/env python3
"""Aperçu du personnage 2D (assets/avatar/rig.json) : rend les animations à 24 i/s.

Même algorithme de dessin que src/render/CartoonAvatar.cpp (Direct2D), rendu « anime » :
  - squelette 2D en cinématique directe ;
  - pour chaque forme, à l'intérieur de la forme (découpe) : ombre, partie éclairée (forme décalée
    vers la lumière), bande de reflet chromé, liseré de lumière dans l'ombre (contour décalé),
    reflets francs ; puis le trait, plus épais côté ombre ;
  - détails en traits fins (jointures, plaque de torse), articulations, semelles, cou ;
  - effets : traînée de mouvement, traits d'emphase, étincelle, émotes.

    python tools/avatar_preview.py --sequence idle,wave,talk --out apercu.mp4
    python tools/avatar_preview.py --sheet planche.png
Dépendances : pycairo (pip install pycairo), ffmpeg pour les vidéos.
"""
import argparse, json, math, os, subprocess, tempfile

import cairo

HERE = os.path.dirname(os.path.abspath(__file__))
RIG = os.path.join(HERE, "..", "assets", "avatar", "rig.json")


# --- Animation ----------------------------------------------------------------
def ease(kind, t):
    if kind == "hold": return 0.0
    if kind == "linear": return t
    if kind == "in": return t * t * t
    if kind == "out": return 1 - (1 - t) ** 3
    return t * t * (3 - 2 * t)


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


class Animator:
    """Physique secondaire : chaque canal suit la pose cible comme un ressort amorti (24 i/s, 4 sous-pas)."""
    def __init__(self, rig):
        self.springs = rig.get("springs", {})
        self.state = {}

    def step(self, target, dt=1 / 24):
        out = {}
        for ch, tgt in target.items():
            f, z = self.springs.get(ch, self.springs.get("default", [6.0, 0.85]))
            x, v = self.state.get(ch, (tgt, 0.0))
            w = 2 * math.pi * f; h = dt / 4
            for _ in range(4):
                v += (w * w * (tgt - x) - 2 * z * w * v) * h
                x += v * h
            self.state[ch] = (x, v); out[ch] = x
        return out


# --- Géométrie ----------------------------------------------------------------
def rgb(h): return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))
def rot(a, v):
    r = math.radians(a); c, s = math.cos(r), math.sin(r)
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c)
def direction(a): r = math.radians(a); return (math.sin(r), math.cos(r))
def add(p, q, k=1.0): return (p[0] + q[0] * k, p[1] + q[1] * k)
def lerp(p, q, t): return (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)


def skeleton(rig, pose, rms=0.0):
    """Positions de toutes les articulations (repère du canevas, avant écrasement)."""
    b = rig["body"]
    H = (rig["hips"][0] + pose["x"], rig["hips"][1] + pose["y"] - rms * 30)
    T = pose["torso"]
    at = lambda v: add(H, rot(T, v))
    j = {"H": H, "T": T, "at": at}
    for side, key in (("a", "shoulderA"), ("b", "shoulderB")):
        s = at(b[key]); up = pose[side + "U"] + T; fo = up + pose[side + "F"]
        e = add(s, direction(up), b["upperArm"]); h = add(e, direction(fo), b["foreArm"])
        j[side + "Shoulder"], j[side + "Elbow"], j[side + "Hand"], j[side + "Fore"], j[side + "Up"] = s, e, h, fo, up
    for side, key in (("a", "hipA"), ("b", "hipB")):
        hp = add(H, b[key]); th = pose[side + "T"]; sh = th + pose[side + "S"]
        k = add(hp, direction(th), b["thigh"]); an = add(k, direction(sh), b["shin"])
        j[side + "Hip"], j[side + "Knee"], j[side + "Ankle"], j[side + "ShinA"] = hp, k, an, sh
    j["neck"] = at((0, b["neckY"]))
    j["ha"] = T + pose["head"]
    j["hc"] = add(add(j["neck"], rot(j["ha"], (0, -(b["headRadius"] + b["headGap"])))), (pose.get("headX", 0), pose.get("headY", 0)))
    return j


def capsule(ctx, p0, p1, r0, r1):
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]; ang = math.atan2(dy, dx)
    ctx.new_sub_path()
    ctx.arc(p0[0], p0[1], r0, ang + math.pi / 2, ang + 3 * math.pi / 2)
    ctx.arc(p1[0], p1[1], r1, ang - math.pi / 2, ang + math.pi / 2)
    ctx.close_path()


def ellipse(ctx, c, rx, ry, angle=0.0):
    ctx.save(); ctx.translate(*c); ctx.rotate(math.radians(angle)); ctx.scale(rx, ry)
    ctx.new_sub_path(); ctx.arc(0, 0, 1, 0, 2 * math.pi); ctx.restore()


def star(ctx, c, r, angle=0.0):  # étincelle à 4 branches
    ctx.new_sub_path()
    for i in range(8):
        a = math.radians(angle + i * 45); rr = r if i % 2 == 0 else r * 0.22
        x, y = c[0] + math.sin(a) * rr, c[1] - math.cos(a) * rr
        (ctx.move_to if i == 0 else ctx.line_to)(x, y)
    ctx.close_path()


class Painter:
    def __init__(self, ctx, rig):
        self.c, self.rig = ctx, rig
        self.P = {k: rgb(v) for k, v in rig["palette"].items()}
        st = rig["style"]
        self.lw, self.inner = st["outline"], st["innerLine"]
        self.so, self.lo = tuple(st["shadowOffset"]), tuple(st["limbShadowOffset"])
        self.ro, self.rw = tuple(st["rimOffset"]), st["rimWidth"]
        self.wo, self.wr = tuple(st["weightOffset"]), st["weightRatio"]
        self.air, self.bounce = st.get("airbrush", 0.0), st.get("bounce", 0.0)

    def part(self, path, back=False, big=False, highlight=None, band=None, details=None, rim=True, color=None):
        """Une forme au rendu anime (même ordre de couches que CartoonAvatar::Draw en C++).
        color : couleur d'aplat d'un accessoire (l'ombre en est dérivée)."""
        c, P = self.c, self.P
        if color:
            base = P[color]; shadow = tuple(x * 0.6 for x in base)
        else:
            base, shadow = (P["backBase"], P["backShadow"]) if back else (P["base"], P["shadow"])
        off = self.so if big else self.lo
        path(c); x0, y0, x1, y1 = c.path_extents(); c.new_path()
        w, h = max(x1 - x0, 1), max(y1 - y0, 1); R = max(w, h)
        c.save(); path(c); c.clip()
        c.set_source_rgb(*shadow); c.paint()                                     # 1. ombre
        c.save(); c.translate(-off[0], -off[1]); path(c); c.restore()            # 2. partie éclairée
        c.set_source_rgb(*base); c.fill()
        if self.air > 0:                                                         # 2b. modelé à l'aérographe
            cx, cy = x0 + w * 0.32, y0 + h * 0.28
            g = cairo.RadialGradient(cx, cy, 0, cx, cy, R * 0.75)
            g.add_color_stop_rgba(0, *P["airbrush"], self.air * (0.6 if back else 1.0)); g.add_color_stop_rgba(1, *P["airbrush"], 0)
            c.set_source(g); c.paint()
        if band:                                                                 # 3. bande de reflet
            c.set_source_rgb(*(P["backBand"] if back else P["band"])); band(c)
        if self.bounce > 0:                                                      # 3b. lumière réfléchie (sol)
            bx, by = x0 + w * 0.78, y1 - h * 0.04
            g = cairo.RadialGradient(bx, by, 0, bx, by, R * 0.55)
            g.add_color_stop_rgba(0, *P["bounce"], self.bounce); g.add_color_stop_rgba(1, *P["bounce"], 0)
            c.set_source(g); c.paint()
        if rim:                                                                  # 4. liseré de lumière
            c.save(); c.translate(-self.ro[0], -self.ro[1]); path(c); c.restore()
            c.set_source_rgba(*P["rim"], 0.55 if back else 0.9); c.set_line_width(self.rw); c.stroke()
        if highlight and not back:                                               # 5. reflets francs
            c.set_source_rgb(*P["highlight"]); highlight(c); c.fill()
        if details:                                                              # 6. détails fins
            c.set_source_rgba(*P["outline"], 0.85); c.set_line_width(self.inner)
            c.set_line_cap(cairo.LINE_CAP_ROUND); details(c); c.stroke()
        c.restore()
        c.set_source_rgb(*P["outline"]); c.set_line_join(cairo.LINE_JOIN_ROUND)  # 7. trait
        path(c); c.set_line_width(self.lw); c.stroke()
        c.save(); c.translate(*self.wo); path(c); c.restore()                    #    plus épais côté ombre
        c.set_line_width(self.lw * self.wr); c.stroke()


def bone_frame(j, bone):
    """Origine et angle (degrés, sens écran) d'un os, pour placer un accessoire."""
    if bone == "head": return j["hc"], j["ha"]
    if bone == "torso": return j["H"], j["T"]
    side = bone[-1].lower()
    if bone.startswith("upper"): return j[side + "Shoulder"], -j[side + "Up"]
    if bone.startswith("fore"): return j[side + "Elbow"], -j[side + "Fore"]
    return j[side + "Hand"], -j[side + "Fore"]  # hand


def draw_prop(ctx, pn, rig, j, name, scale, frame):
    P = pn.P
    for pr in rig["props"][name]["parts"]:
        o, ang = bone_frame(j, pr["bone"])
        def place(c, local_angle=0.0, center=(0, 0)):
            c.translate(*o); c.rotate(math.radians(ang)); c.scale(scale, scale)
            c.translate(*center); c.rotate(math.radians(local_angle))
        t = pr["type"]
        if t == "glow":
            pts = [add(o, rot(ang, (x * scale, y * scale))) for x, y in pr["points"]]
            for wmul, alpha, col in ((3.2, 0.22, P[pr["color"]]), (1.0, 1.0, P[pr["color"]]), (0.35, 0.9, (1, 1, 1))):
                ctx.set_source_rgba(*col, alpha); ctx.set_line_width(pr["width"] * wmul * scale); ctx.set_line_cap(cairo.LINE_CAP_ROUND)
                ctx.move_to(*pts[0]); [ctx.line_to(*q) for q in pts[1:]]; ctx.stroke()
            continue
        if t == "arc":
            cc = add(o, rot(ang, (pr["c"][0] * scale, pr["c"][1] * scale))); r = pr["radius"] * scale
            a0, a1 = math.radians(pr["a0"] + ang), math.radians(pr["a1"] + ang)
            for width, col in ((pr["width"] * scale + 2 * pn.lw, P["outline"]), (pr["width"] * scale, P[pr["fill"]])):
                ctx.set_source_rgb(*col); ctx.set_line_width(width); ctx.set_line_cap(cairo.LINE_CAP_ROUND)
                ctx.new_sub_path(); ctx.arc(cc[0], cc[1], r, a0, a1); ctx.stroke()
            ctx.set_source_rgba(*P["airbrush"], 0.35); ctx.set_line_width(max(1.5, pr["width"] * scale * 0.25))
            ctx.new_sub_path(); ctx.arc(cc[0], cc[1], r - pr["width"] * scale * 0.2, a0 + 0.15, (a0 + a1) / 2); ctx.stroke()
            continue
        if t == "ellipse":
            def path(c, pr=pr):
                c.save(); place(c, pr.get("angle", 0), pr["c"]); c.scale(pr["r"][0], pr["r"][1])
                c.new_sub_path(); c.arc(0, 0, 1, 0, 2 * math.pi); c.restore()
        elif t == "rrect":
            def path(c, pr=pr):
                c.save(); place(c, pr.get("angle", 0), pr["c"])
                w, h, r = pr["size"][0] / 2, pr["size"][1] / 2, pr["radius"]
                c.new_sub_path()
                c.arc(w - r, -h + r, r, -math.pi / 2, 0); c.arc(w - r, h - r, r, 0, math.pi / 2)
                c.arc(-w + r, h - r, r, math.pi / 2, math.pi); c.arc(-w + r, -h + r, r, math.pi, 1.5 * math.pi)
                c.close_path(); c.restore()
        elif t == "capsule":
            def path(c, pr=pr):
                p0 = add(o, rot(ang, (pr["p0"][0] * scale, pr["p0"][1] * scale)))
                p1 = add(o, rot(ang, (pr["p1"][0] * scale, pr["p1"][1] * scale)))
                capsule(c, p0, p1, pr["r0"] * scale, pr["r1"] * scale)
        else:  # poly
            def path(c, pr=pr):
                pts = [add(o, rot(ang, (x * scale, y * scale))) for x, y in pr["points"]]
                c.new_sub_path(); c.move_to(*pts[0]); [c.line_to(*q) for q in pts[1:]]; c.close_path()
        hl = None
        if pr.get("highlight"):
            def hl(c, path=path):  # reflet : petite ellipse en haut à gauche de la forme
                path(c); x0, y0, x1, y1 = c.path_extents(); c.new_path()
                ellipse(c, (x0 + (x1 - x0) * 0.3, y0 + (y1 - y0) * 0.25), max(1.5, (x1 - x0) * 0.16), max(1.2, (y1 - y0) * 0.09), -25)
        color = pr["fill"]
        if pr.get("blink") and (frame // 6) % 2:
            color = "propDark"
        pn.part(path, color=color, highlight=hl, rim=pr["fill"] not in ("neon", "led"))


def draw(ctx, rig, pose, frame, clip, rms=0.0, history=(), props=None):
    b = rig["body"]; pn = Painter(ctx, rig); P = pn.P
    gx, gy = rig["hips"][0], rig["ground"]
    sx, sy = 1 + pose["sq"] * 0.6, 1 - pose["sq"]
    j = skeleton(rig, pose, rms)
    at, T = j["at"], j["T"]

    # Ombre au sol, deux tons nets (hors écrasement)
    lift = max(0.0, -pose["y"]) / 40.0
    ctx.set_source_rgba(*P["groundShadow"], 0.35); ellipse(ctx, (gx, gy + 6), 66 * (1 - lift * 0.3), 9); ctx.fill()
    ctx.set_source_rgba(*P["groundCore"], 0.35); ellipse(ctx, (gx + 4, gy + 6), 38 * (1 - lift * 0.3), 5); ctx.fill()

    ctx.save(); ctx.translate(gx, gy); ctx.scale(sx, sy); ctx.translate(-gx, -gy)

    # Traînée de mouvement (positions des images précédentes) : derrière le personnage
    for name in clip.get("trail", []):
        pts = [h[name] for h in history] + [j[name]]
        if len(pts) >= 2 and math.dist(pts[0], pts[-1]) > 14:
            for i in range(len(pts) - 1):
                ctx.set_source_rgba(*P["speed"], 0.25 + 0.5 * i / len(pts))
                ctx.set_line_width(6 + 12 * i / len(pts)); ctx.set_line_cap(cairo.LINE_CAP_ROUND)
                ctx.move_to(*pts[i]); ctx.line_to(*pts[i + 1]); ctx.stroke()

    def streak(p0, p1, r):
        def f(c):
            o = (-r * 0.35, -r * 0.35)
            capsule(c, add(lerp(p0, p1, 0.18), o), add(lerp(p0, p1, 0.55), o), 2.6, 2.0)
            ellipse(c, add(lerp(p0, p1, 0.70), o), 1.8, 1.8)
        return f

    def limb_band(p0, p1, r):  # bande de reflet le long du membre, côté ombre
        def f(c):
            o = (r * 0.25, r * 0.2)
            capsule(c, add(lerp(p0, p1, 0.05), o), add(lerp(p0, p1, 0.95), o), r * 0.28, r * 0.24); c.fill()
        return f

    def joint(p, ang, r, back):  # anneau d'articulation perpendiculaire au membre
        pn.part(lambda c: ellipse(c, p, r * 1.06, r * 0.38, -ang), back=back, rim=False,
                highlight=lambda c: ellipse(c, add(p, (-r * 0.35, -r * 0.12)), r * 0.32, r * 0.09, -ang))

    def hand(side, back):
        h, fo = j[side + "Hand"], j[side + "Fore"]
        tdir = direction(fo + (55 if side == "a" else -55))  # pouce vers l'intérieur (vers le corps)
        R = b["handR"]
        def path(c):
            ellipse(c, h, R, R * 0.92, -fo)
            capsule(c, add(h, tdir, R * 0.35), add(h, tdir, R * 1.05), 5.5, 4.5)
        pn.part(path, back=back, highlight=lambda c: ellipse(c, add(h, (-5, -6)), 4.5, 3, -30),
                details=lambda c: (c.move_to(*add(h, rot(-fo, (-6, 4)))), c.line_to(*add(h, rot(-fo, (6, 4))))))

    def arm(side, back):
        s, e, h = j[side + "Shoulder"], j[side + "Elbow"], j[side + "Hand"]
        ur, fr = b["upperArmR"], b["foreArmR"]
        pn.part(lambda c: capsule(c, s, e, ur[0], ur[1]), back, highlight=streak(s, e, ur[0]), band=limb_band(s, e, ur[0]))
        pn.part(lambda c: capsule(c, e, h, fr[0], fr[1]), back, highlight=streak(e, h, fr[0]), band=limb_band(e, h, fr[0]))
        ang = math.degrees(math.atan2(h[0] - e[0], h[1] - e[1]))
        joint(e, ang, fr[0], back)
        hand(side, back)
        joint(s, math.degrees(math.atan2(e[0] - s[0], e[1] - s[1])), ur[0] * 0.95, back)

    def leg(side, back):
        hp, k, an = j[side + "Hip"], j[side + "Knee"], j[side + "Ankle"]
        tr, sr = b["thighR"], b["shinR"]
        pn.part(lambda c: capsule(c, hp, k, tr[0], tr[1]), back, highlight=streak(hp, k, tr[0]), band=limb_band(hp, k, tr[0]))
        pn.part(lambda c: capsule(c, k, an, sr[0], sr[1]), back, highlight=streak(k, an, sr[0]), band=limb_band(k, an, sr[0]))
        joint(k, math.degrees(math.atan2(an[0] - k[0], an[1] - k[1])), sr[0], back)
        out = -1 if side == "a" else 1
        fa = pose[side + "Foot"]; ang = out * (8 + fa) if side == "a" else out * 8 + fa
        fc = add(an, (out * 9, 7)); fw, fh = b["foot"]
        def sole(c):  # semelle : bas du pied
            c.set_source_rgb(*P["sole"]); ellipse(c, add(fc, rot(ang, (0, fh * 0.95))), fw * 1.05, fh * 0.75, ang); c.fill()
        pn.part(lambda c: ellipse(c, fc, fw, fh, ang), back, band=sole,
                highlight=lambda c: ellipse(c, add(fc, rot(ang, (-out * 6, -5))), 7, 2.5, ang),
                details=lambda c: (c.move_to(*add(fc, rot(ang, (out * 6, -fh * 0.9)))), c.line_to(*add(fc, rot(ang, (out * 9, 0))))))

    props = props or {}
    def layer(name):
        for pname, sc in props.items():
            if sc > 0.01 and rig["props"][pname]["layer"] == name: draw_prop(ctx, pn, rig, j, pname, sc, frame)

    # Ordre : accessoires « back », bras arrière, jambe arrière, jambe avant, cou, torse, « preHead », tête, « head », bras avant, « front »
    layer("back")
    arm("b", True)
    leg("b", True)
    leg("a", False)

    neck = j["neck"]
    neck_top = add(add(neck, rot(j["ha"], (0, -16))), (pose.get("headX", 0) * 0.5, pose.get("headY", 0) * 0.5))
    pn.part(lambda c: capsule(c, add(neck, rot(T, (0, 6))), neck_top, 13, 12),
            back=True, rim=False)

    pts = [at(p) for p in b["torso"]]
    def torso(c):
        c.new_sub_path(); c.move_to(*pts[0])
        for i in range(1, len(pts), 3): c.curve_to(*pts[i], *pts[i + 1], *pts[i + 2])
        c.close_path()
    def torso_band(c):  # reflet d'horizon du chrome : bande sombre courbe sous la poitrine
        ctx2 = c; ctx2.set_line_width(9)
        p0, p1, p2 = at((-52, -54)), at((0, -40)), at((52, -54))
        ctx2.move_to(*p0); ctx2.curve_to(*lerp(p0, p1, 0.6), *lerp(p2, p1, 0.6), *p2); ctx2.stroke()
    def torso_details(c):  # plaque de poitrine et jointure centrale
        p0, p2 = at((-34, -96)), at((30, -96))
        c.move_to(*p0); c.curve_to(*at((-30, -70)), *at((26, -70)), *p2)
        c.move_to(*at((-4, -66))); c.curve_to(*at((-6, -44)), *at((-6, -22)), *at((-4, -6)))
    pn.part(torso, big=True, band=torso_band, details=torso_details,
            highlight=lambda c: (ellipse(c, at((-27, -80)), 6.5, 16, T + 18), ellipse(c, at((-20, -54)), 2.6, 5, T + 18)))

    hc, ha, hr = j["hc"], j["ha"], b["headRadius"]
    def head_band(c):  # horizon reflété sur la sphère chromée
        cc = add(hc, rot(ha, (0, -hr * 1.25)))
        c.set_line_width(hr * 0.17)
        c.arc(cc[0], cc[1], hr * 1.62, math.radians(35 + ha), math.radians(145 + ha)); c.stroke()  # dépasse : découpé net
        c.set_source_rgba(*P["rim"], 0.8); c.set_line_width(2)
        c.arc(cc[0], cc[1], hr * 1.62 + hr * 0.13, math.radians(35 + ha), math.radians(145 + ha)); c.stroke()
    def head_hl(c):
        ellipse(c, add(hc, rot(ha, (-26, -28))), 20, 10.5, ha - 38)
        ellipse(c, add(hc, rot(ha, (-6, -44))), 4.5, 4.5)
        ellipse(c, add(hc, rot(ha, (-38, -6))), 3, 7, ha - 10)
    layer("preHead")
    pn.part(lambda c: ellipse(c, hc, hr, hr), big=True, band=head_band, highlight=head_hl)
    layer("head")

    arm("a", False)
    layer("front")
    ctx.restore()

    # Effets et émotes : hors écrasement, à la position écrasée de la tête
    hc = (gx + (hc[0] - gx) * sx, gy + (hc[1] - gy) * sy)
    emote(ctx, P, clip.get("emote"), frame, hc, hr)
    return j


def emote(ctx, P, kind, frame, hc, hr):
    top = (hc[0], hc[1] - hr)
    ctx.set_source_rgb(*P["emote"]); ctx.set_line_width(3.2); ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    if kind == "dots":
        for i in range(3):
            if (frame // 8) % 4 > i:
                ctx.new_sub_path(); ctx.arc(hc[0] + 58 + i * 16, top[1] + 10 - i * 10, 4.5 + i, 0, 2 * math.pi); ctx.fill()
    elif kind == "sound":
        for i in range(3):
            ph = ((frame / 16.0) + i / 3.0) % 1.0
            ctx.set_source_rgba(*P["emote"], 1 - ph)
            ctx.new_sub_path(); ctx.arc(hc[0] - hr - 4, hc[1] - 6, 14 + ph * 26, math.radians(140), math.radians(220)); ctx.stroke()
    elif kind == "notes":
        for i in range(2):
            ph = ((frame / 24.0) + i * 0.5) % 1.0
            x = hc[0] + 70 - i * 12 + math.sin(ph * 6.28) * 6; y = hc[1] - 10 - ph * 60
            ctx.set_source_rgba(*P["emote"], 1 - ph)
            ellipse(ctx, (x, y), 7, 5, -20); ctx.fill()
            ctx.move_to(x + 6, y - 1); ctx.line_to(x + 6, y - 24); ctx.line_to(x + 14, y - 18); ctx.stroke()
    elif kind == "sweat":
        ph = (frame % 24) / 24.0
        x, y = hc[0] + hr * 0.75, hc[1] - hr * 0.55 + ph * 14
        ctx.set_source_rgb(*P["accent"])
        ctx.move_to(x, y - 12); ctx.curve_to(x + 8, y - 2, x + 7, y + 7, x, y + 7); ctx.curve_to(x - 7, y + 7, x - 8, y - 2, x, y - 12)
        ctx.fill_preserve(); ctx.set_source_rgb(*P["outline"]); ctx.set_line_width(2); ctx.stroke()
    elif kind == "emphasis":  # traits d'emphase manga sur les accents de la phrase
        if frame % 12 in (5, 6, 7):
            for a in (-60, -30, 0):
                r0, r1 = hr + 10, hr + 24
                ca, sa = math.cos(math.radians(a - 90)), math.sin(math.radians(a - 90))
                ctx.move_to(hc[0] + 18 + ca * r0, hc[1] + sa * r0); ctx.line_to(hc[0] + 18 + ca * r1, hc[1] + sa * r1); ctx.stroke()
    elif kind == "joy":  # éclats de joie autour de la tête
        for i, (dx, dy, a0) in enumerate(((-hr - 22, -hr * 0.5, 0), (hr + 20, -hr * 0.7, 20), (hr + 30, hr * 0.1, 40))):
            s = 0.6 + 0.4 * math.sin((frame + i * 5) * 0.5)
            ctx.set_source_rgb(*P["highlight"]); star(ctx, (hc[0] + dx, hc[1] + dy), 11 * s, a0); ctx.fill_preserve()
            ctx.set_source_rgba(*P["accent"], 0.9); ctx.set_line_width(1.4); ctx.stroke()
    elif kind == "laugh":  # traits de vibration de chaque côté
        if frame < 26:
            for side in (-1, 1):
                for k in range(3):
                    x = hc[0] + side * (hr + 10 + k * 7); y0 = hc[1] - 18 + k * 4
                    ctx.move_to(x, y0); ctx.line_to(x + side * 4, y0 + 14 - k * 3); ctx.stroke()
    elif kind == "exclaim":  # « ! » qui surgit
        if frame >= 1:
            s = min(1.0, frame / 4.0) * (1.0 if frame < 22 else max(0.0, 1 - (frame - 22) / 6.0))
            x, y = hc[0] + hr * 1.08, hc[1] - hr * 0.35
            ctx.set_source_rgba(*P["accent"], s); ctx.set_line_width(6 * s + 0.1)
            ctx.move_to(x, y - 26 * s); ctx.line_to(x + 2, y - 6 * s); ctx.stroke()
            ctx.new_sub_path(); ctx.arc(x + 2.5, y + 3, 3.4 * s + 0.1, 0, 2 * math.pi); ctx.fill()
    elif kind == "question":  # « ? »
        if frame >= 4:
            s = min(1.0, (frame - 4) / 4.0)
            x, y = hc[0] + hr * 0.9, top[1] - 6
            ctx.set_source_rgba(*P["emote"], s); ctx.set_line_width(4)
            ctx.arc(x, y - 16, 8, math.radians(200), math.radians(400)); ctx.line_to(x, y - 3); ctx.stroke()
            ctx.new_sub_path(); ctx.arc(x, y + 5, 2.8, 0, 2 * math.pi); ctx.fill()
    elif kind == "gloom":  # lignes verticales de déprime (manga)
        ctx.set_source_rgba(*P["emote"], 0.55); ctx.set_line_width(2.2)
        for i in range(5):
            x = hc[0] - hr * 0.7 + i * hr * 0.35; ln = 16 + (i % 2) * 10
            y = top[1] - 6 - ((frame + i * 9) % 32) * 0.4
            ctx.move_to(x, y - ln); ctx.line_to(x, y); ctx.stroke()
    elif kind == "zzz":
        for i in range(3):
            ph = ((frame / 48.0) + i / 3.0) % 1.0
            x, y, s = hc[0] + hr * 0.6 + ph * 30, top[1] - ph * 50, 6 + ph * 8
            ctx.set_source_rgba(*P["emote"], 1 - ph); ctx.set_line_width(2.6)
            ctx.move_to(x, y); ctx.line_to(x + s, y); ctx.line_to(x, y + s); ctx.line_to(x + s, y + s); ctx.stroke()
    elif kind == "sparkle":  # étincelle sur le chrome, au sommet du geste
        if 8 <= frame <= 20:
            s = 1 - abs(frame - 13) / 7.0
            ctx.set_source_rgb(*P["highlight"]); star(ctx, (hc[0] - hr * 0.55, hc[1] - hr * 0.6), 13 * s, 15); ctx.fill_preserve()
            ctx.set_source_rgba(*P["accent"], 0.9); ctx.set_line_width(1.2); ctx.stroke()


class PropPops:
    """Apparition « pop » des accessoires (6 images, léger dépassement), disparition en 4 images."""
    def __init__(self): self.state = {}  # nom -> [échelle, âge]

    def step(self, wanted):
        for p in set(self.state) | set(wanted):
            sc, age = self.state.get(p, [0.0, 0])
            if p in wanted:
                age += 1; k = min(1.0, age / 6.0)
                sc = math.sqrt(k) * (1 + 0.18 * math.sin(math.pi * k)) if k < 1 else 1.0
            else:
                sc, age = max(0.0, sc - 0.25), 0
            self.state[p] = [sc, age]
        return {p: v[0] for p, v in self.state.items() if v[0] > 0.01}


def props_for(rig, contexts):
    names = []
    for c in contexts:
        for p in rig.get("contexts", {}).get(c, []):
            if p not in names: names.append(p)
    return names


def render_frame(rig, clip_name, frame, scale=2, bg=True, pose=None, props=None):
    w, h = rig["canvas"]
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, int(w * scale), int(h * scale))
    ctx = cairo.Context(surf); ctx.scale(scale, scale)
    if bg:
        g = cairo.LinearGradient(0, 0, 0, h); g.add_color_stop_rgb(0, 0.90, 0.94, 0.99); g.add_color_stop_rgb(1, 0.74, 0.83, 0.94)
        ctx.set_source(g); ctx.paint()
    clip = rig["clips"][clip_name]
    history = []
    for k in (2, 1):  # deux images précédentes pour la traînée
        if frame - k >= 0 and clip.get("trail"):
            history.append(skeleton(rig, sample(clip, rig["rest"], frame - k)))
    draw(ctx, rig, pose or sample(clip, rig["rest"], frame), frame, clip, history=history, props=props)
    return surf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", default=RIG)
    ap.add_argument("--clip", default="wave")
    ap.add_argument("--loops", type=int, default=2)
    ap.add_argument("--out")
    ap.add_argument("--sheet")
    ap.add_argument("--props-sheet", dest="props_sheet")
    ap.add_argument("--sequence", help="ex. idle,wave,talk : enchaîne plusieurs animations dans une vidéo")
    ap.add_argument("--contexts", default="", help="contextes par animation, ex. ',cyberpunk,studio' (accessoires)")
    a = ap.parse_args()
    rig = json.load(open(a.rig, encoding="utf-8"))
    if a.sheet:
        names = list(rig["clips"]); w, h = rig["canvas"]; cols = 7; rows = (len(names) + cols - 1) // cols
        sheet = cairo.ImageSurface(cairo.FORMAT_ARGB32, w * cols * 2, (h + 30) * 2 * rows)
        c = cairo.Context(sheet); c.set_source_rgb(1, 1, 1); c.paint()
        for idx, n in enumerate(names):
            i, row = idx % cols, idx // cols
            c.save(); c.translate(0, row * (h + 30) * 2)
            f = {"wave": 13, "train": 6, "talk": 6, "idle": 40, "happy": 10, "laugh": 9, "surprised": 5, "shrug": 12,
                 "nod": 3, "shake": 3, "clap": 8, "point": 10, "talk2": 8}.get(n, rig["clips"][n]["frames"] // 2)
            c.set_source_surface(render_frame(rig, n, f, scale=2), i * w * 2, 0); c.paint()
            c.set_source_rgb(0.1, 0.1, 0.1); c.select_font_face("Sans", 0, 1); c.set_font_size(30)
            c.move_to(i * w * 2 + 20, h * 2 + 44); c.show_text(n); c.restore()
        sheet.write_to_png(a.sheet); return
    if a.props_sheet:
        demos = [("idle", 40, "cyberpunk"), ("studio", 3, "studio"), ("talk", 6, "lol"), ("train", 6, "train"),
                 ("think", 24, "research"), ("think", 24, "cloud"), ("sleep", 48, "sleep")]
        w, h = rig["canvas"]
        sheet = cairo.ImageSurface(cairo.FORMAT_ARGB32, w * len(demos) * 2, (h + 30) * 2)
        c = cairo.Context(sheet); c.set_source_rgb(1, 1, 1); c.paint()
        for i, (n, f, ctx_name) in enumerate(demos):
            anim = Animator(rig)
            for k in range(f + 1): pose = anim.step(sample(rig["clips"][n], rig["rest"], k))
            props = {p: 1.0 for p in props_for(rig, [ctx_name])}
            c.set_source_surface(render_frame(rig, n, f, scale=2, pose=pose, props=props), i * w * 2, 0); c.paint()
            c.set_source_rgb(0.1, 0.1, 0.1); c.select_font_face("Sans", 0, 1); c.set_font_size(30)
            c.move_to(i * w * 2 + 20, h * 2 + 44); c.show_text(ctx_name)
        sheet.write_to_png(a.props_sheet); return
    names = a.sequence.split(",") if a.sequence else [a.clip]
    ctxs = a.contexts.split(",") if a.contexts else []
    plan = []
    for k, name in enumerate(names):
        clip = rig["clips"][name]
        context = ctxs[k] if k < len(ctxs) else ""
        plan += [(name, f % clip["frames"], context) for f in range(clip["frames"] * (1 if not clip.get("loop") else a.loops))]
    anim = Animator(rig)
    pops = PropPops()
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, f, context) in enumerate(plan):
            live = pops.step(props_for(rig, [context] if context else []))
            pose = anim.step(sample(rig["clips"][name], rig["rest"], f))
            render_frame(rig, name, f, pose=pose, props=live).write_to_png(os.path.join(tmp, f"{i:04d}.png"))
        out = a.out or f"{a.sequence.replace(',', '_') if a.sequence else a.clip}.mp4"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(rig["fps"]), "-i", os.path.join(tmp, "%04d.png"),
                        "-pix_fmt", "yuv420p", "-crf", "16", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", out], check=True)
        print(out)


if __name__ == "__main__":
    main()
