# Génère assets/textures/matcap_chrome.png (256x256), même formule que ChromeEnvironment() en C++.
# Chrome gris métal (bonhomme chromé façon années 2000) : ciel gris clair, sol anthracite, reflets blancs.
import sys, numpy as np
from PIL import Image
N = 256
c = (np.arange(N) + 0.5) / N * 2 - 1
x, y = np.meshgrid(c, -c)
r = np.sqrt(x*x + y*y); k = np.where(r > 0.999, 0.999 / np.maximum(r, 1e-9), 1.0); x, y = x*k, y*k
z = np.sqrt(np.clip(1 - x*x - y*y, 0, 1))
def ss(e0, e1, v):
    t = np.clip((v - e0) / (e1 - e0), 0, 1); return t*t*(3 - 2*t)
hz = -0.30 + 0.10*x
lerp = lambda a, b, t: np.array(a)[None, None] + (np.array(b) - np.array(a))[None, None]*t[..., None]
sky = lerp([0.74, 0.75, 0.77], [0.40, 0.41, 0.44], ss(hz, 1.0, y))
gnd = lerp([0.16, 0.16, 0.18], [0.34, 0.35, 0.37], ss(hz, -1.0, y))
blend = ss(hz - 0.15, hz + 0.15, y)  # transition douce sol -> ciel (pas d'arête sur les flancs)
col = gnd + (sky - gnd) * blend[..., None]
col += (0.18*np.exp(-((y - hz)*18)**2))[..., None]
def spec(L, p):
    L = np.array(L) / np.linalg.norm(L); return np.maximum(0, x*L[0] + y*L[1] + z*L[2])**p
col += (1.3*spec([-0.5, 0.65, 0.6], 70) + 0.25*spec([0.6, -0.3, 0.75], 18))[..., None]
col *= (0.50 + 0.50*z**0.6)[..., None]
Image.fromarray((np.clip(col, 0, 1)*255 + 0.5).astype(np.uint8), "RGB").save(sys.argv[1])
