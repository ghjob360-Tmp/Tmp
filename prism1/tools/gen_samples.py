#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""PRISM-1's factory samples for the sample oscillator: synthesized here (no recordings), 16-bit, written to
app/dsp/samples.h as flash constants the render reads in place. Loops are made seamless: a pitched loop holds a
whole number of periods, a noise loop is crossfaded over its seam."""
import math
import os
import random

random.seed(7)
OUT = []                                 # (name, rate, root, loop_start, loop_end or 0, samples)


def norm(x, peak=0.9):
    m = max(abs(v) for v in x) or 1.0
    return [v / m * peak for v in x]


def svf(x, f, q, rate, mode="bp"):
    """a state-variable filter over a list (f may be a list per sample)"""
    lo = bp = 0.0
    y = []
    for i, v in enumerate(x):
        fc = f[i] if isinstance(f, list) else f
        g = 2 * math.sin(math.pi * min(fc, rate * 0.24) / rate)
        hp = v - lo - bp / q
        bp += g * hp
        lo += g * bp
        y.append(bp if mode == "bp" else lo if mode == "lp" else hp)
    return y


def seam(x, fade):
    """a noise loop: its end crossfaded into its start, so the loop has no seam"""
    n = len(x) - fade
    y = x[:n]
    for i in range(fade):
        t = i / fade
        y[i] = x[n + i] * (1 - t) + x[i] * t
    return y


def breath(rate=22050, dur=0.8):
    n = int(rate * dur) + 2000
    w = [random.uniform(-1, 1) for _ in range(n)]
    a = svf(w, 900, 1.4, rate)
    b = svf(w, 2600, 2.0, rate)
    c = svf(w, 5200, 3.0, rate, "bp")
    x = [a[i] * 0.8 + b[i] * 0.5 + c[i] * 0.25 for i in range(n)]
    x = [v * (0.75 + 0.25 * math.sin(2 * math.pi * 1.25 * i / rate)) for i, v in enumerate(x)]
    return seam(x, 2000)


def metal(rate=22050, dur=0.8):
    f0 = 392.0
    parts = [(1.0, 1.0, 1.2), (2.76, 0.7, 2.0), (5.40, 0.5, 2.8), (8.93, 0.35, 3.6), (13.34, 0.25, 4.5),
             (18.64, 0.18, 5.5), (3.17, 0.4, 1.6), (6.97, 0.3, 3.0)]
    x = []
    for i in range(int(rate * dur)):
        t = i / rate
        s = sum(a * math.exp(-t * d) * math.sin(2 * math.pi * f0 * r * t) for r, a, d in parts if f0 * r < rate * 0.45)
        s += random.uniform(-1, 1) * math.exp(-t * 300) * 0.6
        x.append(s)
    return x


def thump(rate=11025, dur=0.6):
    x, ph = [], 0.0
    for i in range(int(rate * dur)):
        t = i / rate
        ph += (49 + 131 * math.exp(-t * 18)) / rate
        x.append(math.tanh(1.8 * math.sin(2 * math.pi * ph)) * math.exp(-t * 4.5))
    return x


def click(rate=44100, n=512):
    x = []
    for i in range(n):
        t = i / rate
        x.append((math.sin(2 * math.pi * 3200 * t) * 0.7 + random.uniform(-1, 1) * 0.5) * math.exp(-t * 900))
    return x


def glass(rate=22050, dur=1.0):
    f0 = 1046.5
    ratios = [1.0, 2.756, 5.404, 8.933]
    x = []
    for i in range(int(rate * dur)):
        t = i / rate
        s = sum(math.sin(2 * math.pi * f0 * r * t) * math.exp(-t * (2.2 + 2.5 * k)) / (1 + k)
                for k, r in enumerate(ratios) if f0 * r < rate * 0.45)
        x.append(s * (1 - math.exp(-t * 3000)))
    return x


def vox(rate=22050, periods=210):
    """a sung 'aah' with a whole number of periods (84 samples: 262.5 Hz), so it loops clean"""
    per = 84
    n = per * periods
    buzz = []
    for i in range(n + per * 40):
        p = (i % per) / per
        buzz.append(2 * p - 1)
    formants = [(750, 9, 1.0), (1150, 10, 0.6), (2700, 14, 0.3), (3400, 16, 0.18)]
    y = [0.0] * len(buzz)
    for f, q, g in formants:
        b = svf(buzz, f, q, rate)
        y = [y[i] + b[i] * g for i in range(len(y))]
    y = y[per * 40:]                     # past the filters' start: periodic from here
    return [math.tanh(v * 1.5) for v in y]


def wind(rate=11025, dur=1.0):
    n = int(rate * dur) + 1500
    w = [random.uniform(-1, 1) for _ in range(n)]
    f = [600 + 450 * math.sin(2 * math.pi * 0.9 * i / rate) + 250 * math.sin(2 * math.pi * 2.3 * i / rate)
         for i in range(n)]
    x = svf(w, f, 4.0, rate)
    return seam(x, 1500)


def kick(rate=22050, dur=0.4):
    x, ph = [], 0.0
    for i in range(int(rate * dur)):
        t = i / rate
        ph += (45 + 170 * math.exp(-t * 32)) / rate
        x.append(math.sin(2 * math.pi * ph) * math.exp(-t * 7) + random.uniform(-1, 1) * math.exp(-t * 400) * 0.3)
    return x


def snare(rate=22050, dur=0.3):
    w = [random.uniform(-1, 1) for _ in range(int(rate * dur))]
    hpn = svf(w, 3500, 1.2, rate, "hp")
    x, ph = [], 0.0
    for i in range(len(w)):
        t = i / rate
        ph += 190 * (1 + 0.4 * math.exp(-t * 50)) / rate
        x.append(math.sin(2 * math.pi * ph) * math.exp(-t * 22) * 0.6 + hpn[i] * math.exp(-t * 14) * 0.8)
    return x


def hat(rate=22050, dur=0.12):
    x = []
    phs = [0.0] * 6
    r = [205.3, 304.4, 369.6, 522.7, 540.0, 800.0]
    for i in range(int(rate * dur)):
        t = i / rate
        s = 0.0
        for k in range(6):
            phs[k] += r[k] * 2.6 / rate
            s += 1 if (phs[k] % 1.0) < 0.5 else -1
        x.append(s / 6 * math.exp(-t * 45))
    y, lp = [], 0.0
    for v in x:                          # a one-pole high pass (the band above ~5 kHz)
        lp += 0.75 * (v - lp)
        y.append(v - lp)
    return y


def pluck(rate=22050, dur=0.9):
    """Karplus-Strong on A3 (100 samples a period at 22.05 kHz: 220.5 Hz)"""
    per = 100
    buf = svf([random.uniform(-1, 1) for _ in range(per)], 3000, 0.7, rate, "lp")
    x = []
    for i in range(int(rate * dur)):
        v = buf[i % per]
        nxt = buf[(i + 1) % per]
        buf[i % per] = 0.996 * 0.5 * (v + nxt)
        x.append(v)
    return x


def bowed(rate=22050, periods=78):
    """a bowed-string-like tone on C3: a whole number of periods (168 samples: 131.25 Hz), body resonances"""
    per = 168
    n = per * periods
    src = []
    for i in range(n + per * 30):
        p = (i % per) / per
        src.append((2 * p - 1) * 0.8 + (1 if p < 0.15 else 0) * 0.3)
    y = [0.0] * len(src)
    for f, q, g in [(280, 3, 1.0), (520, 4, 0.8), (1100, 5, 0.5), (2300, 6, 0.3), (3400, 8, 0.15)]:
        b = svf(src, f, q, rate)
        y = [y[i] + b[i] * g for i in range(len(y))]
    return y[per * 30:]


def note_of(hz):
    return 69 + 12 * math.log2(hz / 440.0)


def add(name, rate, root, x, loop=None):
    x = norm(x)
    ls, le = (loop if loop else (0, 0))
    OUT.append((name, rate, root, ls, le, [int(round(v * 32767)) for v in x]))


def main():
    add("BREATH", 22050, 60.0, breath(), (0, -1))
    add("METAL HIT", 22050, 67.0, metal())
    add("SUB THUMP", 11025, 31.0, thump())
    add("CLICK", 44100, 60.0, click())
    add("GLASS PING", 22050, 84.0, glass())
    v = vox()
    add("VOX AAH", 22050, note_of(22050 / 84), v, (84 * 40, -1))
    add("WIND", 11025, 60.0, wind(), (0, -1))
    add("KICK", 22050, 36.0, kick())
    add("SNARE", 22050, 50.0, snare())
    add("HAT", 22050, 66.0, hat())
    add("PLUCK", 22050, note_of(22050 / 100), pluck())
    b = bowed()
    add("BOWED", 22050, note_of(22050 / 168), b, (168 * 10, -1))
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "app", "dsp", "samples.h")
    total = 0
    with open(path, "w") as f:
        f.write("/* SPDX-License-Identifier: GPL-3.0-only */\n")
        f.write("/* PRISM-1's factory samples: generated by tools/gen_samples.py (synthesized, no recordings). Do not "
                "edit. */\n#pragma once\n#include <stdint.h>\n")
        for i, (name, rate, root, ls, le, d) in enumerate(OUT):
            f.write(f"static const int16_t SMPD_{i}[{len(d)}] = {{\n")
            for j in range(0, len(d), 16):
                f.write("    " + ", ".join(str(v) for v in d[j:j + 16]) + ",\n")
            f.write("};\n")
            total += 2 * len(d)
        f.write(f"#define SMPD_N {len(OUT)}\n")
        f.write("static const struct { const int16_t *d; uint32_t len, ls, le; float rate, root; const char *name; } "
                "SMPD[SMPD_N] = {\n")
        for i, (name, rate, root, ls, le, d) in enumerate(OUT):
            le2 = len(d) if le == -1 else le
            f.write(f"    {{SMPD_{i}, {len(d)}u, {ls}u, {le2}u, {rate:.1f}f, {root:.3f}f, \"{name}\"}},\n")
        f.write("};\n")
    print(f"samples.h: {len(OUT)} samples, {total} bytes")


if __name__ == "__main__":
    main()
