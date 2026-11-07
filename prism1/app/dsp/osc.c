/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the wavetable oscillator and the sample oscillator (RAMC), after Vital (Matt Tytel, GPL-3.0):
 * synthesis/producers/synth_oscillator.cpp (unison, stacks, blend, stereo spread, the phase distortions) and
 * synthesis/producers/sample_source.cpp.
 *
 * Per block (n <= CTL): the unison voices are set up from the control (Vital's setPhaseIncMults / setAmplitude /
 * setDistortionValues, in pairs: a pair's two voices go left and right, one sharp one flat, alternating; with an odd
 * count the centre pair is one voice in both channels), then each voice runs over the block into the left or right
 * sum (one fused loop with no warp; else a warp loop writing the bent phases, then one read loop), then the sums are
 * mixed with Vital's stereo blend, pan and level (ramped over the block) into L / R, and mono[] gets the sum.
 * The read: frame crossfade (two frames at the voice's frame position) x linear interpolation, at the mip level whose
 * top harmonic stays under 20 kHz at the voice's pitch (times the sync ratio for SYNC / FORMANT).
 *
 * Mapping of the 0..1 controls (amount 0 = no warp, exactly): Vital's distortion amount is 0..1 with no effect at
 * 0.5 for SYNC, FORMANT, BEND and SQUEEZE; here they use one half of Vital's range: SYNC / FORMANT 0.5..1 (ratio
 * 1..16), BEND 0.5..1, SQUEEZE 0.5..0. QUANTIZE, PULSE, FM, RM are Vital's 0..1 as is (FM: 6 a^2 cycles per unit).
 * Detune: the outer voices at +-detune semitones; detune power x 3 is Vital's power (the core's default 0.5 is
 * Vital's 1.5). Phase: Vital's (0.5 = start at 0), random phase +-0.5 cycle x rand_phase. */
#include "wt.h"

/* -------------------------------------------------------------------------------------------- tables --- */
const char *const WARP_NAME[WARP_N] = {"NONE", "SYNC", "FORMANT", "QUANTIZE", "BEND", "SQUEEZE", "PULSE",
                                       "FM A", "FM B", "FM SAMPLE", "RM A", "RM B", "RM SAMPLE"};
const char *const STACK_NAME[WT_STACK_N] = {"NORMAL", "DROP 12", "DROP 24", "OCTAVE", "2X OCTAVE", "POWER",
                                            "2X POWER", "MAJOR", "MINOR", "HARMONICS", "ODD HARM"};
#define FIFTH 1.49830707688f
#define MAJ3 1.25992104989f
#define MIN3 1.189207115f
static const float STACK_MULT[WT_STACK_N][MAXUNI / 2] = {   /* Vital's kStackMultipliers, per pair */
    {1.0f, 1.0f, 1.0f, 1.0f},          {0.5f, 1.0f, 1.0f, 1.0f},           {0.25f, 1.0f, 1.0f, 1.0f},
    {1.0f, 2.0f, 1.0f, 2.0f},          {1.0f, 2.0f, 3.0f, 1.0f},           {1.0f, FIFTH, 2.0f, 1.0f},
    {1.0f, FIFTH, 2.0f, 2.0f * FIFTH}, {1.0f, MAJ3, FIFTH, 2.0f},          {1.0f, MIN3, FIFTH, 2.0f},
    {1.0f, 2.0f, 3.0f, 4.0f},          {1.0f, 3.0f, 5.0f, 7.0f}};

/* ------------------------------------------------------------------------- the warps (Vital's formulas) --- */
/* phases are 0..1 (Vital's unsigned 32-bit phase); "signed" x = u in [-0.5, 0.5) is Vital's signed int phase */
enum { WK_NONE, WK_SYNC, WK_FORMANT, WK_QUANT, WK_BEND, WK_SQUEEZE, WK_PULSE, WK_FM, WK_RM };
typedef struct {
    int kind;
    float a, b, c, d;                    /* per kind: see warp_prep */
    float fmult;                         /* the frequency multiplier the mip pick must allow for */
} warp_t;

INL void warp_prep(warp_t *w, int warp, float amt)
{
    amt = clampf(amt, 0.0f, 1.0f);
    w->fmult = 1.0f;
    w->a = w->b = w->c = w->d = 0.0f;
    w->kind = WK_NONE;
    if (!(amt > 0.0f))
        return;
    switch (warp) {
    case WARP_SYNC:
    case WARP_FORMANT:                   /* Vital: 2^((v - 0.5) 8) / 16, x 16 in syncPhase: ratio 2^(4 a) */
        w->kind = warp == WARP_SYNC ? WK_SYNC : WK_FORMANT;
        w->a = fm_exp2f(4.0f * amt);
        w->fmult = w->a;
        break;
    case WARP_QUANTIZE: {                /* Vital: 2^((1 - a)^3 0.85 32 + 1) steps a cycle */
        float q = 1.0f - amt;
        w->kind = WK_QUANT;
        w->a = fm_exp2f(q * q * q * 0.85f * 32.0f + 1.0f);
        w->b = 1.0f / w->a;
        break;
    }
    case WARP_BEND: {                    /* Vital's cubic, d = 0.5 .. 1 */
        float d = 0.5f + 0.5f * amt, e = 2.0f * (d - d * d), m1 = 3.0f * d + e, m2 = 3.0f * d - e;
        w->kind = WK_BEND;
        w->a = m2;
        w->b = m1 - 2.0f * m2;
        w->c = 1.0f - m1 + m2;
        break;
    }
    case WARP_SQUEEZE: {                 /* Vital: v 2 0.95 + 0.05, v = 0.5 .. 0 */
        float d = 1.0f - 0.95f * amt;
        w->kind = WK_SQUEEZE;
        w->a = 0.25f * d;
        w->b = 1.0f / d;
        w->c = 1.0f / (2.0f - d);
        break;
    }
    case WARP_PULSE:                     /* Vital: 1 / (1 - a) */
        w->kind = WK_PULSE;
        w->a = 1.0f / fm_maxf(1.0f - amt, 1e-6f);
        break;
    case WARP_FM_A:
    case WARP_FM_B:
    case WARP_FM_SMP:                    /* Vital: a^2 x (2^32 / 8) x 48 = 6 a^2 cycles */
        w->kind = WK_FM;
        w->a = 6.0f * amt * amt;
        break;
    case WARP_RM_A:
    case WARP_RM_B:
    case WARP_RM_SMP:
        w->kind = WK_RM;
        w->a = amt;
        break;
    default:
        break;
    }
}
INL float sgn_ph(float u) { return u >= 0.5f ? u - 1.0f : u; }
INL float uns_ph(float y) { return y < 0.0f ? y + 1.0f : y; }
INL float w_sync(float u, float r)
{
    float y = (u - 0.5f) * r + 0.5f;
    return y - fm_floorf(y);
}
INL float w_hsin(float u)                /* Vital's halfSinWindow: sin(pi u) with futils::sin */
{
    float q = 4.0f * u * (1.0f - u);
    return q * (0.776f + 0.224f * q);
}
INL float w_quant(float u, float D, float iD)
{
    float y = (float)(int32_t)(sgn_ph(u) * D) * iD;
    return uns_ph(y);
}
INL float w_bend(float u, float k1, float k2, float k3)
{
    float f = u + 0.5f, y;
    if (f >= 1.0f)
        f -= 1.0f;
    y = f * (k1 + f * (k2 + f * k3)) + 0.5f;
    return y >= 1.0f ? y - 1.0f : y;
}
INL float w_squeeze(float u, float piv, float id, float i2d)
{
    float x = sgn_ph(u), X = fm_fabsf(x);
    float y = X > piv ? 0.5f - (0.5f - X) * i2d : X * id;
    if (!(x > 0.0f))
        y = -y;
    y = uns_ph(y);
    return y >= 1.0f ? y - 1.0f : y;
}
INL float w_pulse(float u, float D, float *win)
{
    float y = sgn_ph(u) * D;
    *win = 1.0f;
    if (y <= -0.5f) {                    /* clamped at INT_MIN: Vital's pulseWidthWindow is 0 there */
        y = -0.5f;
        *win = 0.0f;
    }
    if (y > 0.5f)
        y = 0.5f;
    return uns_ph(y);
}
INL float w_fm(float u, float k, float s)
{
    float y = u + k * s;
    return y - fm_floorf(y);
}

float wt_warp_phase(int warp, float amount, float ph)
{
    warp_t w;
    float win;
    ph = frac1(ph);
    warp_prep(&w, warp, amount);
    switch (w.kind) {
    case WK_SYNC:
    case WK_FORMANT:
        return w_sync(ph, w.a);
    case WK_QUANT:
        return w_quant(ph, w.a, w.b);
    case WK_BEND:
        return w_bend(ph, w.a, w.b, w.c);
    case WK_SQUEEZE:
        return w_squeeze(ph, w.a, w.b, w.c);
    case WK_PULSE:
        return w_pulse(ph, w.a, &win);
    default:
        return ph;
    }
}

/* ------------------------------------------------------------------------------- the oscillator --- */
typedef struct {
    const int16_t *r0, *r1;              /* the two frames at the mip level */
    float ft, fN, w;                     /* frame mix, level size, gain (with 1/32767) */
    uint32_t mask;
} rd_t;

INL float rd(const int16_t *r0, const int16_t *r1, float ft, float fN, uint32_t mask, float p)
{
    float x = p * fN, f, a0, a1, c0, c1;
    int32_t k = (int32_t)x;
    uint32_t k0 = (uint32_t)k & mask, k1 = (k0 + 1u) & mask;
    f = x - (float)k;
    a0 = (float)r0[k0];
    a1 = (float)r0[k1];
    c0 = a0 + ((float)r1[k0] - a0) * ft;
    c1 = a1 + ((float)r1[k1] - a1) * ft;
    return c0 + (c1 - c0) * f;
}

/* the mip level for a voice: the lowest whose top harmonic, (128 >> L) - 1, stays under 20 kHz */
INL int mip_for(float inc)
{
    float top = 20000.0f * ISR / fm_maxf(inc, 1e-9f);   /* harmonics allowed */
    int L = 0;
    while (L < WT_LEVELS - 1 && (float)((128 >> L) - 1) > top)
        L++;
    return L;
}

/* one voice over the block, added into acc; returns its phase */
static RAMC NOIN float voice_run(float x, float inc, const rd_t *v, const warp_t *wp, const float *src, float *acc,
                                 float *pw, float *win, uint32_t n)
{
    const int16_t *r0 = v->r0, *r1 = v->r1;
    const float ft = v->ft, fN = v->fN, w = v->w;
    const uint32_t mask = v->mask;
    uint32_t i;
    int windowed = 0;
    switch (wp->kind) {
    case WK_SYNC:
        for (i = 0; i < n; i++) {
            pw[i] = w_sync(x, wp->a);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        break;
    case WK_FORMANT:
        for (i = 0; i < n; i++) {
            pw[i] = w_sync(x, wp->a);
            win[i] = w_hsin(x);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        windowed = 1;
        break;
    case WK_QUANT:
        for (i = 0; i < n; i++) {
            pw[i] = w_quant(x, wp->a, wp->b);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        break;
    case WK_BEND:
        for (i = 0; i < n; i++) {
            pw[i] = w_bend(x, wp->a, wp->b, wp->c);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        break;
    case WK_SQUEEZE:
        for (i = 0; i < n; i++) {
            pw[i] = w_squeeze(x, wp->a, wp->b, wp->c);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        break;
    case WK_PULSE:
        for (i = 0; i < n; i++) {
            pw[i] = w_pulse(x, wp->a, &win[i]);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        windowed = 1;
        break;
    case WK_FM:
        for (i = 0; i < n; i++) {
            pw[i] = w_fm(x, wp->a, src[i]);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        break;
    default:                             /* no warp (RM is applied to the sums): one fused loop */
        for (i = 0; i < n; i++) {
            acc[i] += w * rd(r0, r1, ft, fN, mask, x);
            x += inc;
            if (x >= 1.0f)
                x -= 1.0f;
        }
        return x;
    }
    if (windowed)
        for (i = 0; i < n; i++)
            acc[i] += w * win[i] * rd(r0, r1, ft, fN, mask, pw[i]);
    else
        for (i = 0; i < n; i++)
            acc[i] += w * rd(r0, r1, ft, fN, mask, pw[i]);
    return x;
}

INL void rd_setup(rd_t *v, const wt_t *t, float fp, float inc, float w)
{
    int i0 = (int)fp, i1, L = mip_for(inc);
    if (i0 > WT_FRAMES - 1)
        i0 = WT_FRAMES - 1;
    i1 = i0 < WT_FRAMES - 1 ? i0 + 1 : i0;
    v->ft = fp - (float)i0;
    v->r0 = t->d[i0] + WT_OFF[L];
    v->r1 = t->d[i1] + WT_OFF[L];
    v->fN = (float)(256 >> L);
    v->mask = (256u >> L) - 1u;
    v->w = w;
}

RAMC void osc_on(osc_state_t *s, const osc_ctl_t *c)
{
    int v;
    float ph0 = c->phase - 0.5f, rp = clampf(c->rand_phase, 0.0f, 1.0f) * 0.5f;
    if (!s->rng)
        s->rng = 0x2545F491u;
    for (v = 0; v < MAXUNI; v++)
        s->ph[v] = frac1(ph0 + rp * rnd1(&s->rng));
    s->s[4] = 0.0f;                      /* the output mix jumps to its first value */
    s->last[0] = s->last[1] = 0.0f;
}

RAMC void osc_run(osc_state_t *s, const osc_ctl_t *c, const float *src, float *L, float *R, float *mono, uint32_t n)
{
    float acc[2][CTL], pw[CTL], win[CTL];
    float base, kp, cents, ca, da, tinc, fsp, div, t[5], d[5], sm, cm, gl, gr, lev, ff;
    const float *stk;
    warp_t wp;
    rd_t rv;
    int nv, np, odd, p, side;
    uint32_t i;
    if (n > CTL)
        n = CTL;
    if (!c->wt) {
        for (i = 0; i < n; i++)
            mono[i] = 0.0f;
        s->s[4] = 0.0f;
        return;
    }
    nv = c->unison < 1 ? 1 : c->unison > MAXUNI ? MAXUNI : c->unison;
    np = (nv + 1) >> 1;
    odd = nv & 1;
    div = nv > 1 ? (float)(nv - 1) : 1.0f;
    kp = clampf(c->detune_power, -1.0f, 1.0f) * 3.0f;
    cents = clampf(c->detune, 0.0f, 1.0f) * (100.0f / 1200.0f);   /* in octaves */
    base = note_hz(clampf(c->note, -24.0f, 150.0f)) * ISR;
    stk = STACK_MULT[c->stack < 0 || c->stack >= WT_STACK_N ? 0 : c->stack];
    warp_prep(&wp, c->warp, c->warp_amt);
    if (wp.kind == WK_FM && !src)
        wp.kind = WK_NONE;
    /* Vital's setAmplitude */
    if (nv <= 2) {
        ca = 1.0f;
        da = 0.0f;
    } else {
        float bl = clampf(c->blend, 0.0f, 1.0f), ib = 1.0f - bl, cen = 1.0f + (0.4f - 1.0f) * bl;
        float det = 0.6f * (1.0f - ib * ib), nrm = fm_rsqrtf(cen * cen + det * det * (float)(np - 1));
        ca = cen * nrm;
        da = det * nrm;
    }
    tinc = np > 2 ? 1.0f / (float)(np - 1) : 1.0f;
    fsp = clampf(c->frame_spread, 0.0f, 1.0f) * (float)(WT_FRAMES / 2);
    for (i = 0; i < n; i++)
        acc[0][i] = acc[1][i] = 0.0f;
    for (p = 0; p < np; p++) {
        float tt = (float)(2 * p + (odd ? 0 : 1)) / div, up = fm_exp2f(power_scale(tt, kp) * cents), m = stk[p];
        float fp = clampf(c->frame + (float)p * tinc * fsp, 0.0f, (float)(WT_FRAMES - 1));
        float w = (p == 0 ? ca : da) * (1.0f / 32767.0f);
        if (!(w > 0.0f) && !(p == 0 && odd))
            continue;                    /* blend 0: silent detuned pairs; their phases rest */
        if (p == 0 && odd) {             /* the centre: one voice, both channels */
            float inc = fm_minf(base * m, 0.5f);
            rd_setup(&rv, c->wt, fp, inc * wp.fmult, w);
            s->ph[0] = voice_run(s->ph[0], inc, &rv, &wp, src, acc[0], pw, win, n);
            for (i = 0; i < n; i++)
                acc[1][i] = acc[0][i];
            continue;
        }
        for (side = 0; side < 2; side++) {
            int vi = odd ? 2 * p - 1 + side : 2 * p + side;
            float r = ((p & 1) == side) ? up : 1.0f / up;   /* pair 0: left sharp, right flat; then alternating */
            float inc = fm_minf(base * m * r, 0.5f);
            rd_setup(&rv, c->wt, fp, inc * wp.fmult, w);
            s->ph[vi] = voice_run(s->ph[vi], inc, &rv, &wp, src, acc[side], pw, win, n);
        }
    }
    /* the mix: Vital's stereo blend (equal power), pan, level; ramped over the block */
    ff = clampf(c->spread, 0.0f, 1.0f) * 0.5f + 0.5f;
    sm = fm_sinf(0.5f * FM_PI * ff);
    cm = fm_cosf(0.5f * FM_PI * ff);
    if (nv == 1) {                       /* one voice: both channels hold it already */
        sm = 1.0f;
        cm = 0.0f;
    }
    {
        float pan = clampf(c->pan, -1.0f, 1.0f);
        gl = 1.41421356f * fm_sinf(0.25f * FM_PI * (1.0f - pan));
        gr = 1.41421356f * fm_sinf(0.25f * FM_PI * (1.0f + pan));
    }
    lev = fm_maxf(c->level, 0.0f);
    t[0] = lev * gl * sm;
    t[1] = lev * gl * cm;
    t[2] = lev * gr * sm;
    t[3] = lev * gr * cm;
    t[4] = 0.5f;                         /* the FM / RM source: before the level (a silent modulator still modulates) */
    {
        float in = 1.0f / (float)n;
        int k;
        if (s->s[4] == 0.0f)
            for (k = 0; k < 5; k++)
                s->s[5 + k] = t[k];
        for (k = 0; k < 5; k++)
            d[k] = (t[k] - s->s[5 + k]) * in;
    }
    {
        float cll = s->s[5], clr = s->s[6], crr = s->s[7], crl = s->s[8], cmo = s->s[9];
        const float *al = acc[0], *ar = acc[1];
        if (wp.kind == WK_RM && src) {
            float ra = wp.a;
            for (i = 0; i < n; i++) {
                float g = 1.0f + (src[i] - 1.0f) * ra, l = al[i] * g, r = ar[i] * g;
                cll += d[0];
                clr += d[1];
                crr += d[2];
                crl += d[3];
                cmo += d[4];
                L[i] += l * cll + r * clr;
                R[i] += r * crr + l * crl;
                mono[i] = (l + r) * cmo;
            }
        } else {
            for (i = 0; i < n; i++) {
                float l = al[i], r = ar[i];
                cll += d[0];
                clr += d[1];
                crr += d[2];
                crl += d[3];
                cmo += d[4];
                L[i] += l * cll + r * clr;
                R[i] += r * crr + l * crl;
                mono[i] = (l + r) * cmo;
            }
        }
    }
    for (i = 0; i < 5; i++)
        s->s[5 + i] = t[i];
    s->s[4] = 1.0f;
}

/* ---------------------------------------------------------------------------- the sample oscillator --- */
/* kinds: 0 data; live: 1 white, 2 pink, 3 brown, 4 vinyl crackle, 5 tape hiss */
enum { SK_DATA, SK_WHITE, SK_PINK, SK_BROWN, SK_CRACKLE, SK_HISS };
const int SMP_NFACT = 10;
const char *const SMP_NAME[10] = {"WHITE", "PINK", "BROWN", "CRACKLE", "TAPE HISS", "BREATH", "METAL HIT",
                                  "SUB THUMP", "CLICK", "GLASS PING"};
#define SMP_WORDS (SMP_RAM / 2u)
static int16_t smp_mem[SMP_WORDS] PLAT_POOL;
static smp_ref_t smp_tab[10];

RAMC const smp_ref_t *smp_get(int i)
{
    return &smp_tab[i < 0 ? 0 : i >= SMP_NFACT ? SMP_NFACT - 1 : i];
}

/* boot: the generated samples (main loop: plain code) */
static uint32_t g_rng = 0x1234567u;
static float grnd(void) { return rnd1(&g_rng); }
/* normalise a run of generated int16 (written with 8x headroom) to a peak of 0.9 */
static void norm16(int16_t *d, uint32_t n)
{
    uint32_t i;
    int32_t mx = 1;
    for (i = 0; i < n; i++) {
        int32_t a = d[i] < 0 ? -d[i] : d[i];
        if (a > mx)
            mx = a;
    }
    for (i = 0; i < n; i++)
        d[i] = (int16_t)(int32_t)((float)d[i] * (0.9f * 32767.0f) / (float)mx);
}
static int16_t f16(float v)
{
    v = clampf(v * 4096.0f, -32767.0f, 32767.0f);   /* generation scale: 8x headroom, then norm16 */
    return (int16_t)(int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
}
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;
static void bq_bp(bq_t *q, float hz, float qq, float rate)   /* RBJ band pass (0 dB peak) */
{
    float w = FM_TWO_PI * hz / rate, al = fm_sinf(w) / (2.0f * qq), a0 = 1.0f + al;
    q->b0 = al / a0;
    q->b1 = 0.0f;
    q->b2 = -al / a0;
    q->a1 = -2.0f * fm_cosf(w) / a0;
    q->a2 = (1.0f - al) / a0;
    q->z1 = q->z2 = 0.0f;
}
static float bq(bq_t *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}
static void ref(int i, int kind, const int16_t *data, uint32_t len, uint32_t ls, uint32_t le, float rate, float root)
{
    smp_tab[i].kind = kind;
    smp_tab[i].data = data;
    smp_tab[i].len = len;
    smp_tab[i].loop_start = ls;
    smp_tab[i].loop_end = le;
    smp_tab[i].rate = rate;
    smp_tab[i].root = root;
}

void smp_init(void)
{
    int16_t *m = smp_mem;
    uint32_t i, n;
    int k;
    ref(0, SK_WHITE, 0, 44100u, 0u, 44100u, SR, 60.0f);
    ref(1, SK_PINK, 0, 44100u, 0u, 44100u, SR, 60.0f);
    ref(2, SK_BROWN, 0, 44100u, 0u, 44100u, SR, 60.0f);
    ref(3, SK_CRACKLE, 0, 44100u, 0u, 44100u, SR, 60.0f);
    ref(4, SK_HISS, 0, 44100u, 0u, 44100u, SR, 60.0f);
    {   /* BREATH: a loop of breathy noise (two bands, filtered twice round so the loop is seamless), 22.05 kHz */
        bq_t a, b;
        n = 5632u;
        bq_bp(&a, 1500.0f, 0.6f, 22050.0f);
        bq_bp(&b, 600.0f, 1.5f, 22050.0f);
        for (k = 0; k < 2; k++) {
            g_rng = 0x51u;               /* the same noise twice: the second pass starts in the loop's end state */
            for (i = 0; i < n; i++) {
                float x = grnd(), env = 0.65f + 0.35f * fm_sinf(FM_TWO_PI * (float)i / (float)n);
                float y = bq(&a, x) + 0.6f * bq(&b, x);
                if (k == 1)
                    m[i] = f16(y * env);
            }
        }
        norm16(m, n);
        ref(5, SK_DATA, m, n, 0u, n, 22050.0f, 60.0f);
        m += n;
    }
    {   /* METAL HIT: eight inharmonic partials on 392 Hz (G4) and a noise tick, 22.05 kHz */
        static const float RT[8] = {1.0f, 1.47f, 2.09f, 2.56f, 3.14f, 3.87f, 4.6f, 5.82f};
        static const float AM[8] = {1.0f, 0.8f, 0.7f, 0.6f, 0.5f, 0.45f, 0.35f, 0.3f};
        static const float TC[8] = {0.18f, 0.12f, 0.1f, 0.07f, 0.06f, 0.045f, 0.035f, 0.03f};
        n = 4096u;
        g_rng = 0x77u;
        for (i = 0; i < n; i++) {
            float tt = (float)i / 22050.0f, y = 0.0f;
            for (k = 0; k < 8; k++)
                y += AM[k] * fm_expf(-tt / TC[k]) * fm_sinf(FM_TWO_PI * 392.0f * RT[k] * tt + (float)k);
            y += 0.6f * grnd() * fm_expf(-tt / 0.004f);
            m[i] = f16(y * fm_minf(1.0f, (float)i / 8.0f));
        }
        norm16(m, n);
        ref(6, SK_DATA, m, n, 0u, n, 22050.0f, 67.0f);
        m += n;
    }
    {   /* SUB THUMP: a sine falling 180 -> 49 Hz (G1), 11.025 kHz */
        float ph = 0.0f;
        n = 2048u;
        for (i = 0; i < n; i++) {
            float tt = (float)i / 11025.0f, hz = 49.0f + 131.0f * fm_expf(-tt / 0.03f);
            ph += hz / 11025.0f;
            ph -= fm_floorf(ph);
            m[i] = f16(fm_sinf(FM_TWO_PI * ph) * fm_expf(-tt / 0.09f) * fm_minf(1.0f, (float)i / 16.0f));
        }
        norm16(m, n);
        ref(7, SK_DATA, m, n, 0u, n, 11025.0f, 31.0f);
        m += n;
    }
    {   /* CLICK: a few ms of decaying high tone and noise, 44.1 kHz */
        n = 256u;
        g_rng = 0x99u;
        for (i = 0; i < n; i++) {
            float tt = (float)i / SR;
            m[i] = f16((fm_sinf(FM_TWO_PI * 3100.0f * tt) + 0.7f * grnd()) * fm_expf(-tt / 0.0012f));
        }
        norm16(m, n);
        ref(8, SK_DATA, m, n, 0u, n, SR, 60.0f);
        m += n;
    }
    {   /* GLASS PING: a free bar's partials on C6, 22.05 kHz */
        n = 2048u;
        for (i = 0; i < n; i++) {
            float tt = (float)i / 22050.0f;
            float y = fm_sinf(FM_TWO_PI * 1046.5f * tt) * fm_expf(-tt / 0.06f) +
                      0.5f * fm_sinf(FM_TWO_PI * 1046.5f * 2.756f * tt) * fm_expf(-tt / 0.03f) +
                      0.3f * fm_sinf(FM_TWO_PI * 1046.5f * 5.404f * tt) * fm_expf(-tt / 0.015f);
            m[i] = f16(y * fm_minf(1.0f, (float)i / 4.0f));
        }
        norm16(m, n);
        ref(9, SK_DATA, m, n, 0u, n, 22050.0f, 84.0f);
        m += n;
    }
}

RAMC void smp_on(smp_state_t *s, const smp_ctl_t *c)
{
    const smp_ref_t *r = smp_get(c->sample);
    float len = (float)r->len;
    if (!s->rng)
        s->rng = 0x6C8E9CF5u;
    s->s[0] = s->s[1] = s->s[2] = 0.0f;
    s->s[3] = -1.0f;                     /* the level ramp starts at its first value */
    s->done = 0;
    s->pos = c->random_start ? (rnd1(&s->rng) * 0.5f + 0.5f) * len : clampf(c->start, 0.0f, 1.0f) * len;
    if (r->kind == SK_DATA) {
        if (c->loop && s->pos >= (float)r->loop_end)
            s->pos = (float)r->loop_start;
        if (s->pos > len - 1.0f)
            s->pos = len - 1.0f;
        if (s->pos < 0.0f)
            s->pos = 0.0f;
    } else {
        s->pos = 0.0f;                   /* live kinds: pos is the white noise's resampling phase */
    }
}

RAMC void smp_run(smp_state_t *s, const smp_ctl_t *c, float *L, float *R, float *mono, uint32_t n)
{
    const smp_ref_t *r = smp_get(c->sample);
    float lev = fm_maxf(c->level, 0.0f), cur = s->s[3] < 0.0f ? lev : s->s[3], dl, pan = clampf(c->pan, -1.0f, 1.0f);
    float gl = 1.41421356f * fm_sinf(0.25f * FM_PI * (1.0f - pan)), gr = 1.41421356f * fm_sinf(0.25f * FM_PI * (1.0f + pan));
    float step = r->rate * ISR * fm_exp2f((clampf(c->note, -48.0f, 160.0f) - r->root) * (1.0f / 12.0f));
    uint32_t i;
    if (n > CTL)
        n = CTL;
    dl = (lev - cur) / (float)n;
    s->s[3] = lev;
    switch (r->kind) {
    case SK_WHITE: {                     /* a noise "sample" played at pitch: new values at step x the rate */
        float a = s->s[0], b = s->s[1], ph = s->pos;
        for (i = 0; i < n; i++) {
            float y;
            if (step >= 1.0f) {
                y = rnd1(&s->rng) * 0.6f;
            } else {
                ph += step;
                if (ph >= 1.0f) {
                    ph -= 1.0f;
                    a = b;
                    b = rnd1(&s->rng) * 0.6f;
                }
                y = a + (b - a) * ph;
            }
            cur += dl;
            y *= cur;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->s[0] = a;
        s->s[1] = b;
        s->pos = ph;
        return;
    }
    case SK_PINK: {                      /* Paul Kellet's economy pink filter */
        float b0 = s->s[0], b1 = s->s[1], b2 = s->s[2];
        for (i = 0; i < n; i++) {
            float w = rnd1(&s->rng), y;
            b0 = 0.99765f * b0 + w * 0.0990460f;
            b1 = 0.96300f * b1 + w * 0.2965164f;
            b2 = 0.57000f * b2 + w * 1.0526913f;
            y = (b0 + b1 + b2 + w * 0.1848f) * 0.16f;
            cur += dl;
            y *= cur;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->s[0] = fm_flush(b0);
        s->s[1] = fm_flush(b1);
        s->s[2] = fm_flush(b2);
        return;
    }
    case SK_BROWN: {                     /* leaky integrated white */
        float b = s->s[0];
        for (i = 0; i < n; i++) {
            float y;
            b = 0.997f * b + 0.035f * rnd1(&s->rng);
            y = b * 1.4f;
            cur += dl;
            y *= cur;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->s[0] = fm_flush(b);
        return;
    }
    case SK_CRACKLE: {                   /* vinyl: sparse clicks, rarer pops, a faint rumble */
        float ck = s->s[0], pop = s->s[1], rb = s->s[2];
        for (i = 0; i < n; i++) {
            uint32_t x = xs32(&s->rng);
            float y;
            if ((x & 0xFFFFu) < 40u)    /* ~27 clicks a second */
                ck += (float)(int32_t)((x >> 16) & 0xFFu) * (1.0f / 128.0f) - 1.0f;
            if (((x >> 8) & 0xFFFFu) < 3u) /* ~2 pops a second */
                pop += ((x & 0x10000000u) ? 0.8f : -0.8f);
            rb = 0.995f * rb + 0.004f * rnd1(&s->rng);
            y = ck + pop + rb * 4.0f;
            ck *= 0.45f;
            pop *= 0.93f;
            cur += dl;
            y *= cur * 0.8f;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->s[0] = fm_flush(ck);
        s->s[1] = fm_flush(pop);
        s->s[2] = fm_flush(rb);
        return;
    }
    case SK_HISS: {                      /* tape hiss: white, first-difference tilted, softened */
        float pv = s->s[0], lp = s->s[1];
        for (i = 0; i < n; i++) {
            float w = rnd1(&s->rng), y = w - 0.7f * pv;
            pv = w;
            lp += (y - lp) * 0.6f;
            cur += dl;
            y = lp * 0.45f * cur;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->s[0] = pv;
        s->s[1] = fm_flush(lp);
        return;
    }
    default:
        break;
    }
    /* data: linear interpolation, looped or one shot */
    {
        const int16_t *d = r->data;
        float pos = s->pos, len = (float)r->len, ls = (float)r->loop_start, le = (float)r->loop_end, ll = le - ls;
        int loop = c->loop && ll >= 2.0f;
        if (!d || s->done) {
            for (i = 0; i < n; i++)
                mono[i] = 0.0f;
            return;
        }
        if (loop && step > ll)
            step = ll;
        for (i = 0; i < n; i++) {
            float y = 0.0f;
            if (!s->done) {
                int32_t k = (int32_t)pos;
                float f = pos - (float)k, a = (float)d[k], b;
                uint32_t k1 = (uint32_t)k + 1u;
                if (loop && k1 >= r->loop_end)
                    k1 = r->loop_start;
                b = k1 < r->len ? (float)d[k1] : 0.0f;
                y = (a + (b - a) * f) * (1.0f / 32767.0f);
                pos += step;
                if (loop) {
                    if (pos >= le)
                        pos -= ll;
                } else if (pos >= len - 1.0f) {
                    s->done = 1;
                }
            }
            cur += dl;
            y *= cur;
            mono[i] = y;
            L[i] += y * gl;
            R[i] += y * gr;
        }
        s->pos = pos;
    }
}
