/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the drum engines (see eng.h).
 *
 * D_KIT, per key (0..1): pitch (20 Hz..12 kHz), waveform (sine, triangle, square, pitched noise), decay (10 ms..5 s),
 *   mod (below the centre: a pitch drop, up to 4 octaves; above: osc 2 frequency-modulates osc 1) |
 *   osc 2 pitch (x1/8..x8 of osc 1), osc 2 level / noise (up to the centre a square's level, then square -> noise),
 *   filter (below the centre a low-pass, above it a high-pass, open at the centre), level.
 *   A key adds only its articulation (the clap's and the tambourine's extra bursts); the sound is the parameters.
 * D_SLICE, per key: pitch (+-2 octaves), in, out, play mode (to the end / loop while held / once: until released or
 *   the out point) | direction, in fine, out fine (+-1/1023 of the sample each), level.
 * The core's velocity sets the level; here it only brightens the kit a little.
 * State: f[] below, u[0] bursts left, u[1] burst counter, u[2] active, u[3] a jump is pending, u[7] noise seed. */
#include "eng.h"

enum { DK_ENV, DK_PE, DK_P1, DK_P2, DK_I1, DK_I2, DK_DC, DK_LAST, DK_NH, DK_NPH, DK_VEL, DK_O2, DK_T, DK_ATT };
enum { DS_POS = 0, DS_GATE = 14 };       /* (DK_DC, DK_LAST shared) */
#define U_RNG 7

static RAMC NOIN float b_exp2(float x) { return fm_exp2f(x); }
static RAMC NOIN float b_exp(float x) { return fm_expf(x); }

/* the kit: pitch, wave, decay, mod | osc2 pitch, osc2 level/noise, filter, level */
static const uint16_t KIT[NDKEY][8] = {
    {145, 0, 625, 123, 512, 51, 460, 780},          /* 0 kick */
    {165, 150, 555, 205, 512, 150, 310, 760},       /* 1 kick 2 */
    {357, 100, 507, 389, 512, 870, 614, 700},       /* 2 snare */
    {390, 300, 455, 410, 512, 970, 716, 680},       /* 3 snare 2 */
    {585, 360, 227, 716, 694, 307, 767, 680},       /* 4 rim */
    {650, 1023, 474, 512, 512, 1023, 790, 760},     /* 5 clap */
    {795, 870, 520, 870, 590, 820, 965, 600},       /* 6 tambourine */
    {820, 767, 294, 920, 607, 767, 992, 600},       /* 7 closed hat */
    {820, 767, 625, 920, 607, 767, 980, 560},       /* 8 open hat */
    {760, 634, 833, 818, 647, 563, 923, 520},       /* 9 ride */
    {795, 870, 885, 972, 585, 870, 908, 560},       /* 10 crash */
    {238, 51, 642, 307, 512, 563, 430, 760},        /* 11 low tom */
    {297, 51, 615, 307, 512, 563, 450, 740},        /* 12 mid tom */
    {357, 51, 585, 307, 512, 563, 470, 720},        /* 13 high tom */
    {445, 82, 528, 430, 512, 102, 512, 700},        /* 14 conga */
    {525, 682, 558, 512, 608, 512, 757, 560},       /* 15 cowbell */
    {766, 0, 294, 491, 512, 0, 512, 680},           /* 16 clave */
    {320, 307, 528, 0, 512, 0, 512, 620},           /* 17 zap */
    {621, 614, 341, 512, 682, 307, 410, 540},       /* 18 blip */
    {540, 682, 697, 870, 761, 460, 665, 540},       /* 19 metal */
    {951, 1023, 545, 512, 512, 1023, 512, 520},     /* 20 noise burst */
    {430, 682, 738, 51, 512, 0, 410, 520},          /* 21 laser */
    {89, 0, 833, 205, 512, 0, 512, 820},            /* 22 sub drop */
    {795, 920, 30, 512, 512, 512, 512, 640},        /* 23 click */
};
/* the extra bursts (count, spacing in ms) of the keys that have them */
INL void kit_bursts(int key, uint32_t *nb, float *sp)
{
    *nb = key == 5 ? 3u : key == 6 ? 1u : 0u;
    *sp = key == 5 ? 0.0105f : 0.03f;
}

RAMC void drum_kit_defaults(dpad_t pads[NDKEY])
{
    int i, j;
    for (i = 0; i < NDKEY; i++)
        for (j = 0; j < 8; j++)
            pads[i].p[j] = KIT[i][j];
}

RAMC void drum_slice_defaults(dpad_t pads[NDKEY])
{
    int i;
    for (i = 0; i < NDKEY; i++) {
        pads[i].p[0] = 512;
        pads[i].p[1] = (uint16_t)((float)i * (1023.0f / (float)NDKEY) + 0.5f);
        pads[i].p[2] = (uint16_t)((float)(i + 1) * (1023.0f / (float)NDKEY) + 0.5f);
        pads[i].p[3] = 0;
        pads[i].p[4] = 0;
        pads[i].p[5] = 512;
        pads[i].p[6] = 512;
        pads[i].p[7] = 800;
    }
}

INL float dbip(uint16_t v)               /* 0..1023 -> -1..1, 0 at 511..512 */
{
    float x = ((float)v - 511.5f) * (1.0f / 511.5f);
    return fm_fabsf(x) < 0.003f ? 0.0f : x;
}
INL float d_saw(float p, float dt) { return 2.0f * p - 1.0f - blep(p, dt); }
INL float d_sq(float p, float dt)        /* +1 on [0, 0.5) */
{
    float q = p + 0.5f;
    if (q >= 1.0f)
        q -= 1.0f;
    return d_saw(q, dt) - d_saw(p, dt);
}

/* --------------------------------------------------------------------- KIT --- */
static RAMC void kit_on(eng_state_t *s, int key, float vel)
{
    float *f = s->f, sp;
    uint32_t nb;
    if (s->u[2])
        s->u[3] = 2;                     /* a retrigger: the new sound takes over from the old one's last sample */
    kit_bursts(key, &nb, &sp);
    f[DK_ENV] = 1.0f;
    f[DK_PE] = 1.0f;
    f[DK_P1] = 0.0f;
    f[DK_P2] = 0.0f;
    f[DK_NPH] = 0.0f;
    f[DK_O2] = 0.0f;
    f[DK_T] = 0.0f;
    f[DK_ATT] = 0.0f;
    f[DK_VEL] = vel;
    s->u[0] = nb;
    s->u[1] = 0;
    s->u[2] = 1;
}

static RAMC NOIN int kit_run(eng_state_t *s, const dpad_t *pad, int key, float *out, uint32_t n)
{
    float *f = s->f;
    const uint16_t *q = pad->p;
    float pitch = kn(q[0]), wv = kn(q[1]) * 2.999f, dec = kn(q[2]), md = dbip(q[3]), r2 = dbip(q[4]), l2 = kn(q[5]);
    float flt = dbip(q[6]), lvl = kn(q[7]), vel = f[DK_VEL], nf = (float)(int32_t)n;
    float f1 = 20.0f * b_exp2(pitch * 9.3f), t60 = 0.01f * b_exp2(dec * 9.0f), drop = 0.0f, ix = 0.0f;
    float tau, pe0, pe1, d0, d1, dd, ratio = b_exp2(r2 * 3.0f), tone2, nz2, mm, mf, g, sp;
    float env = f[DK_ENV], p1 = f[DK_P1], p2 = f[DK_P2], i1 = f[DK_I1], i2 = f[DK_I2], dc = f[DK_DC], last = f[DK_LAST];
    float nh = f[DK_NH], nph = f[DK_NPH], o2 = f[DK_O2], att = f[DK_ATT];
    uint32_t i, nb = s->u[0], bc = s->u[1], jump = s->u[3], seed = s->u[U_RNG], spn;
    int lp = flt <= 0.0f, wi = (int)wv;
    float wf = wv - (float)wi;
    if (!s->u[2]) {
        for (i = 0; i < n; i++) {
            out[i] += dc;
            dc *= 0.996f;
        }
        f[DK_DC] = fm_flush(dc);
        return fm_fabsf(dc) > 1e-4f;
    }
    kit_bursts(key, &spn, &sp);       /* (only the spacing: the count left is u[0]) */
    spn = (uint32_t)(int32_t)(sp * SR);
    if (md < 0.0f) {                     /* the pitch drop: its time follows the decay */
        drop = -md * 4.0f * (0.85f + 0.15f * vel);
    } else
        ix = md * md * 1.5f;
    tau = fm_minf(0.003f + 0.05f * t60, 0.25f);
    pe0 = f[DK_PE];
    pe1 = pe0 * b_exp(-nf * ISR / tau);
    f[DK_PE] = fm_flush(pe1);
    d0 = fm_minf(f1 * b_exp2(drop * pe0) * ISR, 0.45f);
    d1 = fm_minf(f1 * b_exp2(drop * pe1) * ISR, 0.45f);
    dd = (d1 - d0) / nf;
    tone2 = l2 < 0.5f ? l2 * 2.0f : 1.0f - (l2 - 0.5f) * 2.0f;
    nz2 = l2 < 0.5f ? 0.0f : (l2 - 0.5f) * 2.0f;
    g = lvl * lvl * 1.25f / (1.0f + 0.5f * (tone2 + nz2));
    mm = b_exp2(-9.97f / (t60 * SR));          /* -60 dB at the decay time */
    mf = b_exp2(-9.97f / (0.012f * SR));        /* a burst: 12 ms to -60 dB */
    {
        float fc, gg, a1, a2, a3, kq = 1.3f;
        fc = lp ? 60.0f * b_exp2((flt + 1.0f) * 8.4f) : 20.0f * b_exp2(flt * 9.0f);
        gg = fm_tanf(FM_PI * clampf(fc, 10.0f, 0.46f * SR) * ISR);
        a1 = 1.0f / (1.0f + gg * (gg + kq));
        a2 = gg * a1;
        a3 = gg * a2;
        for (i = 0; i < n; i++) {
            float dt1 = d0, dt2 = fm_minf(d0 * ratio, 0.45f), x, w0, w1, nz, v1, v2, v3, y;
            d0 += dd;
            if (nb) {                        /* the bursts before the main decay */
                if (++bc >= spn) {
                    bc = 0;
                    nb--;
                    env = 1.0f;
                }
            }
            env *= nb ? mf : mm;
            if (att < 1.0f)
                att += 0.15f;
            p2 += dt2;
            if (p2 >= 1.0f)
                p2 -= 1.0f;
            nz = rnd1(&seed);
            o2 = tone2 * d_sq(p2, dt2) + nz2 * nz;
            p1 += dt1;
            if (p1 >= 1.0f)
                p1 -= 1.0f;
            {
                float ph = p1 + ix * o2;     /* cross mod: osc 2 moves osc 1's phase */
                ph -= fm_floorf(ph);
                switch (wi) {
                case 0: w0 = fm_sin_turns(ph); w1 = 1.0f - 4.0f * fm_fabsf(frac1(ph + 0.25f) - 0.5f); break;
                case 1: w0 = 1.0f - 4.0f * fm_fabsf(frac1(ph + 0.25f) - 0.5f); w1 = d_sq(ph, dt1); break;
                default:
                    w0 = d_sq(ph, dt1);
                    nph += dt1 * 8.0f;       /* pitched noise: sample and hold at 8x the pitch */
                    if (nph >= 1.0f) {
                        nph -= fm_floorf(nph);
                        nh = nz;
                    }
                    w1 = nh;
                    break;
                }
            }
            x = w0 + (w1 - w0) * wf + o2 * (tone2 + nz2);
            v3 = x - i2;                     /* the filter (a TPT state-variable) */
            v1 = a1 * i1 + a2 * v3;
            v2 = i2 + a2 * i1 + a3 * v3;
            i1 = 2.0f * v1 - i1;
            i2 = 2.0f * v2 - i2;
            y = lp ? v2 : x - kq * v1 - v2;
            y *= env * att * g;
            if (jump) {
                dc += last - y;
                jump = 0;
            }
            y = soft(y);                     /* (only the hottest settings reach the knee) */
            last = y;
            out[i] += y + dc;
            dc *= 0.996f;
        }
    }
    f[DK_ENV] = env;
    f[DK_P1] = p1;
    f[DK_P2] = p2;
    f[DK_I1] = fm_flush(i1);
    f[DK_I2] = fm_flush(i2);
    f[DK_DC] = fm_flush(dc);
    f[DK_LAST] = last;
    f[DK_NH] = nh;
    f[DK_NPH] = nph;
    f[DK_O2] = o2;
    f[DK_ATT] = att;
    f[DK_T] += nf * ISR;
    s->u[0] = nb;
    s->u[1] = bc;
    s->u[3] = jump;
    s->u[U_RNG] = seed;
    if (!nb && env < 0.0003f && f[DK_T] > 0.005f) {
        s->u[2] = 0;
        return fm_fabsf(dc) > 1e-4f;
    }
    return 1;
}

/* ------------------------------------------------------------------- SLICE --- */
typedef struct {
    float a, b, rate;
} slc_t;
INL int slice_pts(const dpad_t *pad, slc_t *c)
{
    const uint16_t *q = pad->p;
    float l1 = (float)(int32_t)g_drum_sample.len - 2.0f, a, b, t;
    c->a = c->b = c->rate = 0.0f;
    if (g_drum_sample.len < 4u || !g_drum_sample.data)
        return 0;
    a = clampf(kn(q[1]) + dbip(q[5]) * (1.0f / 1023.0f), 0.0f, 1.0f) * l1;
    b = clampf(kn(q[2]) + dbip(q[6]) * (1.0f / 1023.0f), 0.0f, 1.0f) * l1;
    if (b < a) {
        t = a;
        a = b;
        b = t;
    }
    if (q[4] >= 512u) {                  /* reverse */
        t = a;
        a = b;
        b = t;
    }
    c->a = a;
    c->b = b;
    c->rate = b_exp2(dbip(q[0]) * 2.0f) * (SAMPLE_RATE / SR) * (b >= a ? 1.0f : -1.0f);
    return fm_fabsf(b - a) >= 2.0f;
}

static RAMC void slice_on(eng_state_t *s, const dpad_t *pad)
{
    slc_t c;
    if (!slice_pts(pad, &c)) {
        s->u[2] = 0;
        return;
    }
    s->f[DS_POS] = c.a;
    s->f[DS_GATE] = 1.0f;
    s->u[2] = 1;
    s->u[3] = 2;                         /* from the last sample (or silence): a fade-in, never a step */
}

static RAMC NOIN int slice_run(eng_state_t *s, const dpad_t *pad, int held, float *out, uint32_t n)
{
    float *f = s->f, pos = f[DS_POS], gate = f[DS_GATE], dc = f[DK_DC], last = f[DK_LAST], g;
    uint32_t i, play = s->u[2], jump = s->u[3];
    const uint8_t *d = g_drum_sample.data;
    float mode = kn(pad->p[3]), l1;
    int loop = mode >= 0.3333f && mode < 0.6667f, gated = mode >= 0.3333f;
    slc_t c;
    if (!slice_pts(pad, &c))
        play = 0;
    l1 = (float)(int32_t)g_drum_sample.len - 2.0f;
    g = kn(pad->p[7]);
    g *= g;
    for (i = 0; i < n; i++) {
        float y = 0.0f;
        if (play) {
            int32_t j = (int32_t)pos;
            float fr = pos - (float)j, y0 = ulaw(d[j]);
            y = (y0 + (ulaw(d[j + 1]) - y0) * fr) * g * gate;
            pos += c.rate;
            if ((c.rate > 0.0f && pos >= c.b) || (c.rate < 0.0f && pos <= c.b)) {
                if (loop && held) {
                    pos = c.a;
                } else
                    play = 0;
                jump = 1;
            }
            pos = clampf(pos, 0.0f, l1);
            if (gated && !held) {        /* released: a 5 ms fade */
                gate -= 1.0f / 220.0f;
                if (gate <= 0.0f) {
                    gate = 0.0f;
                    play = 0;
                }
            }
        }
        if (jump == 2u) {
            dc += last - y;
            jump = 0;
        } else if (jump == 1u)
            jump = 2;
        last = y;
        out[i] += y + dc;
        dc *= 0.996f;
    }
    f[DS_POS] = pos;
    f[DS_GATE] = gate;
    f[DK_DC] = fm_flush(dc);
    f[DK_LAST] = last;
    s->u[2] = play;
    s->u[3] = jump;
    return play || fm_fabsf(dc) > 1e-4f;
}

/* ---------------------------------------------------------------- dispatch --- */
RAMC void drum_on(int e, eng_state_t *s, const dpad_t *pad, int key, float vel)
{
    if (s->u[U_RNG] == 0u)
        s->u[U_RNG] = 0x5DEECE6Du;
    if (e == D_SLICE)
        slice_on(s, pad);
    else
        kit_on(s, key, vel);
}

RAMC int drum_run(int e, eng_state_t *s, const dpad_t *pad, int key, int held, float *out, uint32_t n)
{
    if (n == 0u)
        return s->u[2] != 0u;
    if (e == D_SLICE)
        return slice_run(s, pad, held, out, n);
    return kit_run(s, pad, key, out, n);
}
