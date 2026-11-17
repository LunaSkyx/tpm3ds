"""Generates the CIA banner, icon and jingle (procedurally drawn / synthesised here, nothing copied):
    assets/banner.png  256x128   HOME menu banner: starfield + yellow perspective crawl
    assets/icon.png    48x48     SMDH icon
    assets/jingle.wav  <3 s      banner sound: orchestral sting (timpani, brass, strings, cymbal)
Not used: the saga's logo / logotype, its characters or ship designs, film or game screenshots, John Williams' themes.
The look is the "crawl over a starfield" style; the sound is an orchestral fanfare with its own melody.
Run:  python -I tools/cia/make_assets.py [banner] [icon] [jingle]
"""
import math, os, sys, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets")
os.makedirs(OUT, exist_ok=True)
FONT = "C:/Windows/Fonts/bahnschrift.ttf"
YELLOW = (255, 212, 38)


def font(size, var="Bold"):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_name(var)
    except Exception:
        pass
    return f


def starfield(W, H, seed, density=1.0, glow_at=None):
    r = np.random.default_rng(seed)
    bg = np.zeros((H, W, 3), np.float32)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    bg[..., 2] = 0.010 + 0.018 * (1 - yy / H)
    bg[..., 1] = 0.004 + 0.008 * (1 - yy / H)
    if glow_at:                                     # faint blue glow behind the horizon
        gx, gy = glow_at
        d = np.sqrt(((xx - gx) / (W * 0.55)) ** 2 + ((yy - gy) / (H * 0.55)) ** 2)
        g = np.exp(-d * d * 2.2)
        bg[..., 0] += g * 0.03
        bg[..., 1] += g * 0.07
        bg[..., 2] += g * 0.17
    img = Image.fromarray(np.clip(bg * 255, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    n = int(W * H / 650 * density)
    for _ in range(n):
        x, y = r.random() * W, r.random() * H
        m = r.random() ** 5
        v = int(90 + 165 * m)
        rad = 0.5 + 2.4 * m * (W / 1024)
        tint = r.choice([(1.0, 1.0, 1.0), (0.85, 0.92, 1.0), (1.0, 0.94, 0.85)])
        d.ellipse([x - rad, y - rad, x + rad, y + rad], fill=tuple(int(v * c) for c in tint))
    return img.filter(ImageFilter.GaussianBlur(0.5))


def perspective_coeffs(src_quad_pts, dst_quad_pts):
    """PIL wants the coefficients that map OUTPUT points to INPUT points."""
    A, B = [], []
    for (x, y), (u, v) in zip(dst_quad_pts, src_quad_pts):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); B.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); B.append(v)
    return np.linalg.solve(np.array(A, float), np.array(B, float))


def crawl(W, H, lines, vp_y, top_w, bottom_y, bottom_w, fade_top=0.15, bright=1.0):
    """Yellow lines of centred text laid out flat, then drawn into a trapezoid that narrows to a vanishing point."""
    FW = 1800
    rows = [(text, font(size), size, gap_after) for text, size, gap_after in lines]
    flat = Image.new("RGBA", (FW, 4000), (0, 0, 0, 0))
    d = ImageDraw.Draw(flat)
    y = 0
    for text, f, size, gap in rows:
        l, t, r, bt = f.getbbox(text)                     # exact ink box: no empty line padding at the bottom
        d.text(((FW - (r - l)) / 2 - l, y - t), text, font=f, fill=YELLOW + (255,))
        y += (bt - t) + gap
    FH = int(y - rows[-1][3]) + 2
    flat = flat.crop((0, 0, FW, FH))
    cx = W / 2
    dst = [(cx - top_w / 2, vp_y), (cx + top_w / 2, vp_y), (cx + bottom_w / 2, bottom_y), (cx - bottom_w / 2, bottom_y)]
    src = [(0, 0), (FW, 0), (FW, FH), (0, FH)]
    co = perspective_coeffs(src, dst)
    layer = flat.transform((W, H), Image.PERSPECTIVE, tuple(co), Image.BICUBIC)
    a = np.asarray(layer, np.float32)
    yy = np.arange(H, dtype=np.float32)[:, None]
    t = np.clip((yy - vp_y) / max(1.0, (bottom_y - vp_y) * fade_top * 4.0), 0, 1) ** 1.2   # fade into the distance
    a[..., 3] *= t
    a[..., :3] *= bright
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGBA")


def compose(bg, layer):
    out = bg.convert("RGBA")
    glow = layer.filter(ImageFilter.GaussianBlur(bg.size[0] / 160))      # soft warm glow under the lettering
    ga = glow.split()[3].point(lambda v: int(v * 0.55))
    out.paste(Image.new("RGBA", bg.size, (255, 170, 20, 255)), (0, 0), ga)
    out.alpha_composite(layer)
    return out.convert("RGB")


def banner():
    W, H = 1024, 512
    bg = starfield(W, H, 77, 1.1, glow_at=(W * 0.5, H * 0.18))
    lines = [("A  3DS  HOMEBREW  PORT", 60, 52),
             ("EPISODE  I", 90, 44),
             ("THE  PHANTOM  MENACE", 128, 0)]
    layer = crawl(W, H, lines, vp_y=H * 0.05, top_w=W * 0.52, bottom_y=H * 0.95, bottom_w=W * 0.98, fade_top=0.10)
    img = compose(bg, layer).filter(ImageFilter.GaussianBlur(0.35))
    img.resize((256, 128), Image.LANCZOS).save(os.path.join(OUT, "banner.png"))
    img.save(os.path.join(OUT, "banner_preview_1024.png"))


def icon():
    W = H = 384
    bg = starfield(W, H, 5, 1.0, glow_at=(W * 0.5, H * 0.2))
    lines = [("EPISODE I", 60, 30), ("THE", 90, 22), ("PHANTOM", 118, 14), ("MENACE", 126, 0)]
    layer = crawl(W, H, lines, vp_y=H * 0.10, top_w=W * 0.55, bottom_y=H * 0.93, bottom_w=W * 0.92, fade_top=0.10)
    img = compose(bg, layer)
    ImageDraw.Draw(img).rounded_rectangle([3, 3, W - 4, H - 4], radius=26, outline=(255, 205, 40), width=6)
    img.resize((48, 48), Image.LANCZOS).save(os.path.join(OUT, "icon.png"))
    img.save(os.path.join(OUT, "icon_preview_384.png"))


# ------------------------------------------------------------------ jingle: orchestral sting, original melody
SR = 32000
RNG = np.random.default_rng(2024)
note = lambda m: 440.0 * 2 ** ((m - 69) / 12)


def adsr(n, a, d, s, rel, hold):
    t = np.arange(n) / SR
    e = np.minimum(1, t / max(a, 1e-4))
    e = e * np.where(t > a, s + (1 - s) * np.exp(-(t - a) / max(d, 1e-4)), 1)
    return e * np.where(t > hold, np.exp(-(t - hold) / max(rel, 1e-4)), 1)


def lowpass(x, fc):
    a = math.exp(-2 * math.pi * fc / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):          # one pole; only used on short noise bursts
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def brass_section(freq, dur, gain=1.0, vib=0.0, bright=1.0):
    """Four slightly detuned, blooming sawtooth-ish voices: a brass section."""
    n = int(SR * dur)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for det, ph0 in ((-0.0045, 0.0), (0.0, 1.3), (0.0035, 2.1), (0.007, 4.0)):
        f = freq * (1 + det) * (1 + vib * 0.005 * np.sin(2 * np.pi * 5.2 * t + ph0) * np.minimum(1, t / 0.6))
        ph = 2 * np.pi * np.cumsum(f) / SR + ph0
        k = 1.2 + 4.2 * bright * (1 - np.exp(-t / 0.12)) * np.exp(-t / 1.4) + 1.2 * bright
        for h in range(1, 22):
            if freq * h > SR * 0.46:
                break
            out += np.sin(h * ph) / h * np.exp(-(h - 1) / (k * 2.4))
    return out / 4 * gain * adsr(n, 0.07, 0.6, 0.85, 0.30, dur - 0.30)


def strings(freqs, dur, gain=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    out = np.zeros(n)
    trem = 1 + 0.18 * np.sin(2 * np.pi * 6.1 * t)
    for f in freqs:
        for det in (-0.006, -0.002, 0.002, 0.006):
            ph = 2 * np.pi * f * (1 + det) * t + RNG.random() * 6
            s = np.zeros(n)
            for h in range(1, 10):
                s += np.sin(h * ph) / h ** 1.3
            out += s
    return out / (4 * len(freqs)) * gain * trem * adsr(n, 0.55, 3, 1.0, 0.35, dur - 0.35)


def timpani(freq, dur=1.3, gain=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    f = freq * (1 + 0.45 * np.exp(-t / 0.035))
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) + 0.5 * np.sin(2.0 * ph + 0.4) * np.exp(-t / 0.25) + 0.3 * np.sin(2.9 * ph) * np.exp(-t / 0.12)
    thud = lowpass(RNG.standard_normal(n), 900) * np.exp(-t / 0.03) * 2.2
    return (body * np.exp(-t / 0.55) + thud) * gain * np.minimum(1, t / 0.002)


def cymbal(dur, gain=1.0, swell=0.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    x = RNG.standard_normal(n)
    for _ in range(2):
        x = x - np.convolve(x, np.ones(6) / 6, "same")                 # crude high-pass
    e = np.exp(-t / 0.9) if swell == 0 else (np.minimum(1, t / swell) * np.exp(-np.maximum(0, t - swell) / 0.8))
    return x * e * gain


def snare_roll(dur, gain=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    x = RNG.standard_normal(n)
    x = x - np.convolve(x, np.ones(4) / 4, "same")
    gate = (np.sin(2 * np.pi * 26 * t) > -0.2).astype(float)          # rapid roll
    return x * gate * (0.15 + 0.85 * (t / dur) ** 1.8) * gain


def place(buf, sig, t0, pan=0.0, gain=1.0):
    i = int(t0 * SR)
    n = min(len(sig), buf.shape[0] - i)
    if n <= 0:
        return
    buf[i:i + n, 0] += sig[:n] * gain * math.cos((pan + 1) * math.pi / 4)
    buf[i:i + n, 1] += sig[:n] * gain * math.sin((pan + 1) * math.pi / 4)


def trumpet(freq, dur, vel=1.0, vib=0.0):
    """Additive brass: strong 2nd-5th harmonics, brightness that blooms with the attack, optional late vibrato."""
    n = int(SR * dur)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for det in (-0.0030, 0.0030):
        f = freq * (1 + det) * (1 + vib * 0.004 * np.sin(2 * np.pi * 5.3 * t) * np.minimum(1, t / 0.5))
        ph = 2 * np.pi * np.cumsum(f) / SR
        for h in range(1, 16):
            if freq * h > SR * 0.45:
                break
            w = h ** -0.75 * math.exp(-(h / 7.5) ** 2)
            bloom = np.minimum(1, t / (0.02 + 0.012 * h))                # high partials arrive a little later
            out += w * bloom * np.sin(h * ph)
    return out / 2 * vel * adsr(n, 0.035, 0.4, 0.88, 0.22, dur - 0.22)


def horns(freq, dur, vel=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for det in (-0.004, 0.0, 0.004):
        ph = 2 * np.pi * freq * (1 + det) * t
        for h in range(1, 9):
            out += math.exp(-(h / 3.0) ** 2) / h * np.sin(h * ph)
    return out / 3 * vel * adsr(n, 0.08, 0.5, 0.9, 0.3, dur - 0.3)


def string_pad(freqs, dur, vel=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for f in freqs:
        for det in (-0.005, 0.0, 0.005):
            ph = 2 * np.pi * f * (1 + det) * t
            out += np.sin(ph) + 0.4 * np.sin(2 * ph) + 0.18 * np.sin(3 * ph)
    return out / (3 * len(freqs)) * vel * adsr(n, 0.5, 3, 1.0, 0.4, dur - 0.4) * (1 + 0.08 * np.sin(2 * np.pi * 5.8 * t))


def timp(freq, vel=1.0, dur=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    f = freq * (1 + 0.35 * np.exp(-t / 0.03))
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) * np.exp(-t / 0.5) + 0.35 * np.sin(1.6 * ph) * np.exp(-t / 0.18)
    knock = np.sin(2 * np.pi * 180 * t) * np.exp(-t / 0.012) * 0.5
    return (body + knock) * vel * np.minimum(1, t / 0.0015)


def crash(dur, vel=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    x = RNG.standard_normal(n)
    x = x - np.convolve(x, np.ones(5) / 5, "same")                    # high-passed, then smoothed so it is not harsh
    x = np.convolve(x, np.ones(2) / 2, "same")
    return x * np.exp(-t / 0.7) * vel * np.minimum(1, t / 0.004)


def schroeder_reverb(x, wet=0.28):
    """Four combs + two all-passes per channel (different delays left/right): a smooth hall, no noise hiss."""
    out = np.zeros_like(x)
    for c in range(2):
        sig = x[:, c]
        acc = np.zeros_like(sig)
        for d in (1687, 1931, 2203, 2441) if c == 0 else (1759, 1993, 2251, 2503):
            d = int(d * SR / 44100)
            y = sig.copy()
            for i in range(d, len(y)):
                y[i] += 0.80 * y[i - d]
            acc += y
        acc /= 4
        for d, g in ((int(556 * SR / 44100), 0.5), (int(441 * SR / 44100), 0.5)):
            y = acc.copy()
            for i in range(d, len(y)):
                y[i] = -g * acc[i] + acc[i - d] + g * y[i - d]
            acc = y
        out[:, c] = acc
    out *= np.max(np.abs(x)) / (np.max(np.abs(out)) + 1e-9)
    return x + out * wet


def jingle():
    """An original heroic fanfare in C: rising arpeggio, a long held peak, a stepwise descent, then a big final chord."""
    BEAT = 60.0 / 150.0                               # 150 bpm
    dur = 2.97
    n = int(SR * dur)
    dry = np.zeros((n, 2))
    B = lambda k: k * BEAT
    # foundation: timpani hit + roll, low brass drone
    place(dry, timp(note(36), 1.3, 1.2), 0.0, -0.1)
    place(dry, timp(note(43), 1.0, 1.0), 0.0, 0.1)
    for i in range(6):                                # timpani roll into the melody
        place(dry, timp(note(36), 0.25 + 0.07 * i, 0.5), B(0.5) + i * 0.085, -0.1)
    place(dry, horns(note(36), 2.9, 0.75), 0.0, -0.3)
    place(dry, horns(note(43), 2.9, 0.45), 0.0, 0.3)
    # melody (trumpets), all notes original: G4 C5 E5 | G5 (held) | F5 E5 D5 | C6 (final, with chord)
    place(dry, trumpet(note(67), BEAT * 0.55, 0.85), B(1.0), -0.1)
    place(dry, trumpet(note(72), BEAT * 0.55, 0.9), B(1.5), -0.05)
    place(dry, trumpet(note(76), BEAT * 0.55, 0.95), B(2.0), 0.0)
    place(dry, trumpet(note(79), BEAT * 1.9, 1.0, 1.0), B(2.5), 0.05)           # G5, long, vibrato
    place(dry, trumpet(note(77), BEAT * 0.48, 0.9), B(4.5), 0.0)                # F5
    place(dry, trumpet(note(76), BEAT * 0.48, 0.9), B(5.0), 0.0)                # E5
    place(dry, trumpet(note(74), BEAT * 0.48, 0.92), B(5.5), 0.0)               # D5
    # harmony under the held note
    place(dry, trumpet(note(64), BEAT * 1.9, 0.5, 0.6), B(2.5), -0.3)           # E4
    place(dry, trumpet(note(71), BEAT * 1.9, 0.5, 0.6), B(2.5), 0.3)            # B4
    # arrival: big C major chord with the melody on top
    t_end = B(6.0)
    for m, g, pan in ((84, 1.0, 0.0), (79, 0.65, -0.2), (76, 0.6, 0.2), (72, 0.6, -0.1), (67, 0.55, 0.1)):
        place(dry, trumpet(note(m), dur - t_end, g, 1.0 if m == 84 else 0.4), t_end, pan)
    place(dry, horns(note(48), dur - t_end, 0.9), t_end, 0.0)
    place(dry, horns(note(36), dur - t_end, 0.9), t_end, 0.0)
    place(dry, timp(note(36), 1.5, 1.0), t_end, 0.0)
    place(dry, timp(note(43), 1.1, 1.0), t_end, 0.0)
    place(dry, crash(1.2, 0.30), t_end, 0.0)
    # strings: warm bed that swells into the arrival
    place(dry, string_pad([note(48), note(55), note(60), note(64)], 2.4, 0.5), 0.0, -0.35)
    place(dry, string_pad([note(60), note(64), note(67), note(72)], dur - B(3.0), 0.55), B(3.0), 0.35)
    out = schroeder_reverb(dry, 0.30)
    out *= np.minimum(1, (dur - np.arange(n) / SR) / 0.35)[:, None]
    out = np.tanh(out / np.max(np.abs(out)) * 1.3) / np.tanh(1.3)             # soft limiter
    out *= 0.89 / np.max(np.abs(out))
    with wave.open(os.path.join(OUT, "jingle.wav"), "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((out * 32767).astype("<i2").tobytes())


if __name__ == "__main__":
    for k in (sys.argv[1:] or ["banner", "icon", "jingle"]):
        globals()[k]()
        print("made", k)
