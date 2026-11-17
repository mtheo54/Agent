# Faux projet FL Studio : trap 140 BPM, 16 mesures… avec les erreurs de débutant classiques.
import numpy as np, wave, sys
fs = 48000; bpm = 140; beat = 60 / bpm; bars = 6
n = int(fs * beat * 4 * bars); t = np.arange(n) / fs
L = np.zeros(n); R = np.zeros(n)
rng = np.random.default_rng(7)
def place(sig, at, gl=1., gr=None):
    i = int(at * fs); j = min(n, i + len(sig))
    if i >= n: return
    L[i:j] += gl * sig[:j - i]; R[i:j] += (gl if gr is None else gr) * sig[:j - i]
def env(d, a=0.002, r=0.2):
    tt = np.arange(int(d * fs)) / fs
    return np.minimum(tt / a, 1) * np.exp(-tt / r)
# Kick : sweep 160→45 Hz, gain +6 dB de trop
def kick():
    d = 0.35; tt = np.arange(int(d * fs)) / fs
    f = 45 + 115 * np.exp(-tt * 40); ph = 2 * np.pi * np.cumsum(f) / fs
    return np.sin(ph) * env(d, 0.001, 0.12) * 1.6
# 808 : 40-50 Hz, saturé, beaucoup trop fort, longues queues qui se chevauchent avec le kick
def b808(f0, d):
    tt = np.arange(int(d * fs)) / fs
    f = f0 * (1 + 0.5 * np.exp(-tt * 30)); ph = 2 * np.pi * np.cumsum(f) / fs
    return np.tanh(2.5 * np.sin(ph)) * env(d, 0.003, 0.9) * 1.1
def clap():
    d = 0.25; x = rng.standard_normal(int(d * fs)) * env(d, 0.001, 0.07)
    x = np.diff(x, prepend=0)  # passe-haut grossier : clap "carton"
    return x * 0.5
def hat(open_=False):
    d = 0.25 if open_ else 0.05; x = rng.standard_normal(int(d * fs))
    x = np.diff(np.diff(x, prepend=0), prepend=0)  # très aigu, agressif
    return x * env(d, 0.0005, 0.08 if open_ else 0.012) * 0.22
def pluck(f, d=0.4):
    tt = np.arange(int(d * fs)) / fs
    saw = 2 * ((f * tt) % 1) - 1
    return saw * env(d, 0.002, 0.18) * 0.25
notes = [311.13, 369.99, 415.30, 466.16, 415.30, 369.99, 311.13, 277.18]  # mineur, très "type beat"
for bar in range(bars):
    b0 = bar * 4 * beat
    for k in [0, 1.5, 2.75]: place(kick(), b0 + k * beat)
    for k in [1, 3]: place(clap(), b0 + k * beat)
    for s in range(16):
        st = b0 + s * beat / 4
        if s % 4 == 3 and bar % 2: [place(hat(), st + r * beat / 16, 0.8) for r in range(4)]  # rolls
        else: place(hat(s == 14), st, 1.0, 0.7)
    place(b808([38.89, 38.89, 46.25, 34.65][bar % 4], 1.6), b0)
    place(b808(38.89, 1.2), b0 + 2.75 * beat)
    for s in range(8):
        p = pluck(notes[(s + bar) % 8])
        place(p, b0 + s * beat / 2, 1.0, -0.95)  # "élargisseur" maison : canal droit inversé → phase
mix = np.stack([L, R], 1) * 1.15
mix = np.clip(mix, -1, 1)  # le limiteur du master… c'est le hard clip de la carte son
mix.astype('<f4').tofile('beat.f32')
pcm = (mix * 32767).astype('<i2')
with wave.open('beat.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(fs); w.writeframes(pcm.tobytes())
print(f"{n/fs:.1f} s, crête {20*np.log10(np.abs(mix).max()):.1f} dBFS, échantillons clippés {(np.abs(mix)>=0.999).sum()}")
