#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""TOYBOX-4's factory samples, synthesized here (no recordings): a 24-sound drum sample with its slice points, and
a looping vocal pad for the synth sampler. 22050 Hz, G.711 mu-law. Writes app/factory.h.

    python3 tools/gen_factory.py            (from the toybox4 directory)
"""
import math
import os
import random

SR = 22050
random.seed(4)


def ulaw(x):
    s = max(-32635, min(32635, int(x * 32767)))
    sign = 0x80 if s < 0 else 0
    if s < 0:
        s = -s
    mag = s + 0x84
    e = 7
    while e > 0 and not (mag & (0x4000 >> (7 - e))):
        e -= 1
    return (~(sign | e << 4 | ((mag >> (e + 3)) & 15))) & 0xFF


def env_exp(t, d):
    return math.exp(-t / d)


class Noise:
    def __init__(self):
        self.lp = 0.0
        self.hp_x = 0.0
        self.hp_y = 0.0

    def white(self):
        return random.uniform(-1, 1)


def svf_bp(x, st, f, q):
    """a Chamberlin state-variable filter: returns (low, band, high)"""
    fk = 2 * math.sin(math.pi * min(f, SR * 0.24) / SR)
    st[0] += fk * st[1]
    hp = x - st[0] - q * st[1]
    st[1] += fk * hp
    return st[0], st[1], hp


def kick(f0=48, drop=5.0, dec=0.45, click=0.6, dur=0.6):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        f = f0 * (1 + drop * math.exp(-t * 30))
        ph += f / SR
        s = math.sin(2 * math.pi * ph) * env_exp(t, dec)
        s += click * random.uniform(-1, 1) * env_exp(t, 0.003)
        out.append(math.tanh(s * 1.4))
    return out


def snare(f0=185, tone=0.5, dec=0.18, bright=3500, dur=0.35):
    out, ph, st = [], 0.0, [0.0, 0.0]
    for n in range(int(dur * SR)):
        t = n / SR
        ph += f0 * (1 + 0.5 * math.exp(-t * 40)) / SR
        body = math.sin(2 * math.pi * ph) * env_exp(t, 0.06)
        lo, bp, hp = svf_bp(random.uniform(-1, 1), st, bright, 0.7)
        noise = (hp * 0.6 + bp * 0.6) * env_exp(t, dec)
        out.append(math.tanh(body * tone + noise * (1.1 - tone * 0.5)))
    return out


def hat(dec=0.05, f=8000, dur=0.25, metal=0.5):
    out, st = [], [0.0, 0.0]
    ratios = [2.0, 3.0, 4.16, 5.43, 6.79, 8.21]
    phs = [0.0] * 6
    for n in range(int(dur * SR)):
        t = n / SR
        m = 0.0
        for i, r in enumerate(ratios):
            phs[i] += 205 * r / SR
            m += 1 if (phs[i] % 1.0) < 0.5 else -1
        x = m / 6 * metal + random.uniform(-1, 1) * (1 - metal)
        lo, bp, hp = svf_bp(x, st, f, 0.5)
        out.append(hp * env_exp(t, dec) * 0.9)
    return out


def clap(dur=0.4):
    out, st = [], [0.0, 0.0]
    for n in range(int(dur * SR)):
        t = n / SR
        ms = t * 1000
        e = sum(math.exp(-max(0, ms - k * 11) / 3.0) if ms >= k * 11 else 0 for k in range(3)) + env_exp(max(0, t - 0.033), 0.09) * (t > 0.033)
        lo, bp, hp = svf_bp(random.uniform(-1, 1), st, 1300, 0.35)
        out.append(math.tanh(bp * e * 1.6))
    return out


def tom(f0, dec=0.3, dur=0.5):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += f0 * (1 + 0.6 * math.exp(-t * 18)) / SR
        out.append(math.sin(2 * math.pi * ph) * env_exp(t, dec) + random.uniform(-1, 1) * env_exp(t, 0.004) * 0.3)
    return out


def conga(f0=320, dur=0.3):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += f0 * (1 + 0.15 * math.exp(-t * 60)) / SR
        out.append(math.sin(2 * math.pi * ph) * env_exp(t, 0.09) * (1 + 0.6 * math.sin(2 * math.pi * ph * 2.02) * env_exp(t, 0.02)))
    return out


def cowbell(dur=0.45):
    out, st, p1, p2 = [], [0.0, 0.0], 0.0, 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        p1 += 545 / SR
        p2 += 815 / SR
        x = (1 if p1 % 1 < 0.5 else -1) + (1 if p2 % 1 < 0.5 else -1)
        lo, bp, hp = svf_bp(x * 0.5, st, 900, 0.3)
        out.append(bp * (env_exp(t, 0.012) * 0.6 + env_exp(t, 0.12)) * 0.9)
    return out


def rim(dur=0.12):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += 1650 / SR
        out.append(math.sin(2 * math.pi * ph) * env_exp(t, 0.012) + random.uniform(-1, 1) * env_exp(t, 0.002) * 0.7)
    return out


def shaker(dur=0.18):
    out, st = [], [0.0, 0.0]
    for n in range(int(dur * SR)):
        t = n / SR
        e = math.sin(math.pi * min(t / dur, 1.0)) ** 2
        lo, bp, hp = svf_bp(random.uniform(-1, 1), st, 6500, 0.6)
        out.append(hp * e * 0.8)
    return out


def bell(f0=640, dur=1.4):
    out, ph, mph = [], 0.0, 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += f0 / SR
        mph += f0 * 3.5 / SR
        idx = 3.5 * env_exp(t, 0.4)
        out.append(math.sin(2 * math.pi * ph + idx * math.sin(2 * math.pi * mph)) * env_exp(t, 0.55) * 0.7)
    return out


def zap(dur=0.3):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += (60 + 3200 * math.exp(-t * 35)) / SR
        out.append(math.sin(2 * math.pi * ph) * env_exp(t, 0.12))
    return out


def subdrop(dur=1.0):
    out, ph = [], 0.0
    for n in range(int(dur * SR)):
        t = n / SR
        ph += (90 * math.exp(-t * 1.6) + 28) / SR
        out.append(math.tanh(math.sin(2 * math.pi * ph) * 1.6) * env_exp(t, 0.5))
    return out


def vox_hit(dur=0.45):
    """a short 'hey': a buzz through moving formants"""
    out, ph, sts = [], 0.0, [[0.0, 0.0] for _ in range(3)]
    for n in range(int(dur * SR)):
        t = n / SR
        ph += 190 * (1 - 0.15 * t) / SR
        buzz = 2 * (ph % 1.0) - 1
        k = min(t / 0.25, 1.0)
        fs = [700 - 250 * k, 1200 + 900 * k, 2600]
        s = 0.0
        for i, f in enumerate(fs):
            lo, bp, hp = svf_bp(buzz, sts[i], f, 0.12)
            s += bp * (1.0 if i == 0 else 0.6)
        out.append(math.tanh(s * 0.6) * (1 - math.exp(-t * 200)) * env_exp(t, 0.18))
    return out


def noise_burst(dur=0.3):
    out, st = [], [0.0, 0.0]
    for n in range(int(dur * SR)):
        t = n / SR
        lo, bp, hp = svf_bp(random.uniform(-1, 1), st, 2200 * math.exp(-t * 4) + 300, 0.4)
        out.append(lo * env_exp(t, 0.1))
    return out


def tamb(dur=0.3):
    h = hat(dec=0.07, f=7000, dur=dur, metal=0.75)
    s = shaker(dur)
    return [a * 0.7 + b * 0.5 for a, b in zip(h, s)]


SOUNDS = [
    ("KICK", kick()), ("KICK 2", kick(f0=58, drop=8, dec=0.25, click=0.9, dur=0.45)),
    ("SNARE", snare()), ("SNARE 2", snare(f0=230, tone=0.3, dec=0.12, bright=5200, dur=0.28)),
    ("RIM", rim()), ("CLAP", clap()), ("TAMB", tamb()),
    ("HAT", hat()), ("OPEN HAT", hat(dec=0.32, dur=0.6)), ("RIDE", hat(dec=0.6, f=5200, dur=1.0, metal=0.85)),
    ("CRASH", hat(dec=0.9, f=4200, dur=1.5, metal=0.6)),
    ("TOM LO", tom(95)), ("TOM MID", tom(140, 0.25)), ("TOM HI", tom(200, 0.2, 0.4)),
    ("CONGA", conga()), ("COWBELL", cowbell()), ("SHAKER", shaker()), ("BELL", bell()),
    ("ZAP", zap()), ("SUB", subdrop()), ("VOX", vox_hit()), ("NOISE", noise_burst()),
    ("BLIP", tom(880, 0.05, 0.15)), ("CLAVE", conga(f0=2400, dur=0.12)),
]


def vox_pad(dur=2.6):
    """a sung 'aah': a few detuned buzzes through vowel formants, looped from 0.6 s to the end"""
    out, sts, phs = [], [[0.0, 0.0] for _ in range(4)], [0.0, 0.1, 0.37]
    f0 = 261.63
    for n in range(int(dur * SR)):
        t = n / SR
        buzz = 0.0
        for i, d in enumerate((1.0, 1.004, 0.997)):
            phs[i] += f0 * d * (1 + 0.004 * math.sin(2 * math.pi * 5.2 * t)) / SR
            buzz += 2 * (phs[i] % 1.0) - 1
        s = 0.0
        for i, (f, g) in enumerate(((750, 1.0), (1150, 0.7), (2700, 0.35), (3400, 0.2))):
            lo, bp, hp = svf_bp(buzz / 3, sts[i], f, 0.1)
            s += bp * g
        att = min(t / 0.12, 1.0)
        out.append(math.tanh(s * 0.5) * att)
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    data, slices = [], []
    gap = int(0.01 * SR)
    for name, snd in SOUNDS:
        pk = max(abs(x) for x in snd) or 1.0
        start = len(data)
        data += [x / pk * 0.92 for x in snd]
        end = len(data)
        data += [0.0] * gap
        slices.append((name, start, end))
    drum = bytes(ulaw(x) for x in data)
    pad = vox_pad()
    pk = max(abs(x) for x in pad) or 1.0
    synth = bytes(ulaw(x / pk * 0.9) for x in pad)

    def arr(name, b):
        lines = [f"static const uint8_t {name}[{len(b)}] = {{"]
        for i in range(0, len(b), 24):
            lines.append("    " + ", ".join(str(x) for x in b[i:i + 24]) + ",")
        lines.append("};")
        return "\n".join(lines)

    n = len(drum)
    sl = ",\n".join(f"    {{{round(s / n * 65535)}u, {round(e / n * 65535)}u}}  /* {nm} */" for nm, s, e in slices)
    names = ", ".join(f'"{nm}"' for nm, _, _ in slices)
    out = f"""/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4's factory samples: generated by tools/gen_factory.py (synthesized, no recordings). Do not edit. */
#pragma once
#include <stdint.h>
#define FACTORY_RATE {SR}
/* the drum sample: {len(SOUNDS)} sounds, {n / SR:.2f} s */
{arr("FACTORY_DRUM", drum)}
/* each sound's slice: start, end as fractions of the sample (0..65535) */
static const uint16_t FACTORY_SLICE[{len(SOUNDS)}][2] = {{
{sl}
}};
static const char *const FACTORY_SLICE_NAME[{len(SOUNDS)}] = {{{names}}};
/* the synth sample: a sung 'aah' at C4, {len(synth) / SR:.2f} s, looping from 0.6 s */
{arr("FACTORY_SYNTH", synth)}
#define FACTORY_SYNTH_LOOP {int(0.6 * SR)}u
"""
    with open(os.path.join(root, "app", "factory.h"), "w") as f:
        f.write(out)
    print(f"factory.h: drum {n} B ({n / SR:.2f} s, {len(SOUNDS)} slices), synth {len(synth)} B")


if __name__ == "__main__":
    main()
