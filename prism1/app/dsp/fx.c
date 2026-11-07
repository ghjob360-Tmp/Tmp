/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the effects, after Vital (Matt Tytel, GPL-3.0): synthesis/effects/{compressor,delay,distortion,phaser,
 * reverb}, synthesis/modules/{chorus,compressor,delay,distortion,equalizer,filter,flanger,phaser,reverb}_module and
 * synthesis/filters/{linkwitz_riley_filter,digital_svf,phaser_filter}, cut down to the FM-1's RAM and CPU.
 *
 * Everything reachable from fx_run / fx_reset is RAMC or INL (common.h's rules). The main-loop entries (fx_init,
 * fx_view, fx_sync_beats, fx_eq_response) are plain flash code.
 *
 * Shared conventions:
 *  - every continuous parameter is ramped per sample from the last block's value to this block's (Vital's way), or
 *    smoothed by a one-pole per block first where a jump would pitch-shift or ring (delay times, cutoffs, sizes);
 *  - the delay lines are never cleared: each counts the samples written since its reset ("filled") and reads older
 *    than that read 0, so fx_reset is cheap from the render;
 *  - the mixes are Vital's: equal power (sin / cos) for chorus, delay, flanger and reverb, linear for compressor,
 *    distortion, filter and phaser.
 *
 * The views (fx_view(type)->a[], updated every block):
 *  CHORUS   0 LFO phase (0..1), 1 voice pairs, 2..9 the tap delays in ms (pair i: 2+2i voice 1 left, 3+2i voice 2 left)
 *  COMP     0..2 band input level dB (low, band, high; 25 ms RMS, -100 = silent), 3..5 band output level dB,
 *           6..8 the compressor gain dB (+ = upward, - = downward; makeup not included), 9..11 upper thresholds dB,
 *           12..14 lower thresholds dB, 15 enabled bands (bit 0 low, 1 band, 2 high)
 *  DELAY    0 left time s, 1 right time s (the smoothed taps; mid ping pong: left = left + right), 2 feedback (-1..1),
 *           3 half rate (0 / 1), 4 wet level (peak, decaying)
 *  DIST     0 input peak, 1 output peak, 2 drive (linear gain; crush step / downsample period for those types)
 *  EQ       0 input peak, 1 output peak, 2..4 the smoothed cutoffs (notes) low, band, high
 *  FILTER   0 input peak, 1 output peak, 2 model, 3 style
 *  FLANGER  0 LFO phase, 1 left delay ms, 2 right delay ms
 *  PHASER   0 LFO phase, 1 left cutoff (note), 2 right cutoff (note)
 *  REVERB   0 wet level (peak, decaying), 1 input level, 2 decay s, 3 size (line scale), 4 pre-delay ms,
 *           5 chorus phase */
#include "fx.h"
#include "filt.h"

/* ================================================================ tables === */
const char *const FXT_NAME[FXT_N] = {"CHORUS", "COMP", "DELAY", "DIST", "EQ", "FILTER", "FLANGER", "PHASER", "REVERB"};
const int FXT_NP[FXT_N] = {10, 8, 9, 7, 12, 9, 7, 8, 10};
const char *const FXT_PNAME[FXT_N][FX_NP] = {
    {"voices", "rate", "sync", "depth", "delay 1", "delay 2", "feedback", "cutoff", "spread", "mix"},
    {"bands", "attack", "release", "low gain", "band gain", "high gain", "amount", "mix"},
    {"style", "time", "sync", "time 2", "sync 2", "feedback", "cutoff", "spread", "mix"},
    {"type", "drive", "filter", "cutoff", "reso", "blend", "mix"},
    {"low mode", "low cut", "low gain", "low reso", "band mode", "band cut", "band gain", "band reso", "high mode",
     "high cut", "high gain", "high reso"},
    {"model", "style", "cutoff", "reso", "drive", "blend", "x", "y", "mix"},
    {"rate", "sync", "feedback", "center", "depth", "offset", "mix"},
    {"rate", "sync", "feedback", "center", "depth", "blend", "offset", "mix"},
    {"size", "decay", "low cut", "high cut", "shelf cut", "shelf gn", "chorus", "chor rate", "pre-delay", "mix"},
};
const char *const FX_SYNC_NAME[FX_NSYNC] = {"free", "1/1", "1/2", "1/2T", "1/4.", "1/4", "1/4T", "1/8.", "1/8",
                                             "1/8T", "1/16.", "1/16", "1/16T", "1/32"};
static const float SYNC_BEATS[FX_NSYNC] = {0.0f, 4.0f, 2.0f, 4.0f / 3.0f, 1.5f, 1.0f, 2.0f / 3.0f, 0.75f, 0.5f,
                                           1.0f / 3.0f, 0.375f, 0.25f, 1.0f / 6.0f, 0.125f};

/* =============================================================== helpers === */
/* FXF: a RAM function kept out of line. fastmath's functions are always inline, one copy per call site; RAM is the
 * limit, so the RAM code calls these single copies (x_*) instead. The main-loop code (fx_init, fx_eq_response) keeps
 * the inline ones: flash code may not call RAM code directly. */
#define FXF static RAMC NOIN
#if defined(__clang__)
#define NOUNROLL _Pragma("clang loop unroll(disable)")
#elif defined(__GNUC__)
#define NOUNROLL _Pragma("GCC unroll 1")
#else
#define NOUNROLL
#endif
FXF float x_exp2(float x) { return fm_exp2f(x); }
FXF float x_tan(float x) { return fm_tanf(x); }
FXF float x_sin_turns(float x) { return fm_sin_turns(x); }
FXF float x_log2(float x) { return fm_log2f(x); }
FXF float x_tanh(float x) { return fm_tanhf(x); }
FXF float x_sqrt(float x) { return fm_sqrtf(x); }
FXF float x_note_hz(float n) { return 440.0f * x_exp2((n - 69.0f) * (1.0f / 12.0f)); }
FXF float x_db_gain(float db) { return x_exp2(db * 0.16609640474f); }
INL float clamp01(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }   /* NaN -> 0 */
INL int pidx(float p, int n) { return (int)(clamp01(p) * (float)(n - 1) + 0.5f); }
INL float sync_beats(float p) { return SYNC_BEATS[pidx(p, FX_NSYNC)]; }
/* an LFO rate: 0.01..10 Hz (exp), or one cycle per synced division */
FXF float lfo_rate(float prate, float psync, float tempo)
{
    float b = sync_beats(psync);
    if (b > 0.0f)
        return fm_maxf(tempo, 1.0f) * (1.0f / 60.0f) / b;
    return 0.01f * x_exp2(clamp01(prate) * 9.965784f);
}
INL float rd16f(int16_t v) { return (float)v * (1.0f / 16384.0f); }
INL int16_t f16(float x)
{
    x *= 16384.0f;
    if (x >= 32767.0f)
        return 32767;
    if (x <= -32767.0f)
        return -32767;
    if (!(x == x))
        return 0;
    return (int16_t)(int32_t)x;
}
/* the same for a recirculating line: above 3 LSB (-75 dBFS) the rounding error is kept and added to the next store
 * (first-order error feedback: no bias, so the line decays at its set rate); below, truncation toward zero (a
 * slight extra loss down there, so the tail dies away to silence instead of idling at a few LSB) */
INL int16_t f16e(float x, float *e)
{
    float v = x * 16384.0f, r;
    if (v < 3.0f && v > -3.0f) {
        *e = 0.0f;
        return (int16_t)(int32_t)v;
    }
    v += *e;
    if (v >= 32767.0f) {
        *e = 0.0f;
        return 32767;
    }
    if (v <= -32767.0f) {
        *e = 0.0f;
        return -32767;
    }
    if (!(v == v)) {
        *e = 0.0f;
        return 0;
    }
    r = fm_floorf(v + 0.5f);
    *e = v - r;
    return (int16_t)(int32_t)r;
}
/* read a power-of-two int16 line at d samples (>= 2) behind w, linear */
INL float rdline(const int16_t *b, uint32_t w, float d, uint32_t mask)
{
    float pos = (float)w - d, f;
    int32_t i;
    if (pos < 0.0f)
        pos += (float)(mask + 1u);
    i = (int32_t)pos;
    f = pos - (float)i;
    {
        float a = (float)b[(uint32_t)i & mask], c = (float)b[(uint32_t)(i + 1) & mask];
        return (a + (c - a) * f) * (1.0f / 16384.0f);
    }
}
/* the same for a line of any length */
INL float rdlen(const int16_t *b, uint32_t w, uint32_t len, float d)
{
    float pos = (float)w - d, f;
    uint32_t i, j;
    if (pos < 0.0f)
        pos += (float)len;
    i = (uint32_t)(int32_t)pos;
    f = pos - (float)i;
    if (i >= len)
        i -= len;
    j = i + 1u;
    if (j >= len)
        j = 0;
    {
        float a = (float)b[i], c = (float)b[j];
        return (a + (c - a) * f) * (1.0f / 16384.0f);
    }
}
/* Vital's OnePoleFilter::tickBasic and its coefficient (computeCoefficient) */
INL float op_tick(float *s, float x, float g)
{
    float d = g * (x - *s), y = *s + d;
    *s = y + d;
    return y;
}
FXF float op_coef(float hz, float sr)
{
    float w = FM_PI * clampf(hz, 1.0f, sr * 0.4535147f) / sr;
    return x_tan(w / (w + 1.0f));
}
/* Vital's equalPowerFade (wet) / its inverse (dry) */
FXF void eqpow(float m, float *dry, float *wet)
{
    m = clamp01(m);
    if (m <= 0.0f) {
        *dry = 1.0f;
        *wet = 0.0f;
        return;
    }
    if (m >= 1.0f) {
        *dry = 0.0f;
        *wet = 1.0f;
        return;
    }
    *wet = x_sin_turns(m * 0.25f);
    *dry = x_sin_turns((1.0f - m) * 0.25f);
}
/* one-pole smoothing factor for a block of n samples, time constant tau seconds */
FXF float blk_alpha(uint32_t n, float tau)
{
    return 1.0f - x_exp2(-(float)n * (FM_LOG2E * ISR) / tau);
}
INL float peak_decay(float lv, float x) { x = fm_fabsf(x); return x > lv ? x : lv * 0.9995f; }
FXF void zero_bytes(void *p, uint32_t n)
{
    unsigned char *c = (unsigned char *)p;   /* (-fno-builtin: no memset made of it) */
    while (n--)
        *c++ = 0;
}
/* a delay time's glide speed (samples per sample): toward vt, at most vmax; it gains speed by at most acc per block
 * (so a jump starts a glide without a kink), and loses it at once */
INL float glide(float *vel, float vt, float vmax, float acc)
{
    float v = *vel;
    vt = clampf(vt, -vmax, vmax);
    if (vt * v < 0.0f)
        v = clampf(vt, -acc, acc);
    else if (fm_fabsf(vt) > fm_fabsf(v))
        v += clampf(vt - v, -acc, acc);
    else
        v = vt;
    *vel = v;
    return v;
}
/* Vital's Delay filter (MultiDelay / StereoDelay): low pass at cutoff + spread, high pass at cutoff - spread */
FXF void dly_filter(float cut, float spread, float sr, float *lc, float *hc, float *fg)
{
    float rad = clamp01(spread) * 96.0f, lf, hf;
    lf = clampf(x_note_hz(cut + rad), 1.0f, sr * 0.4535147f);
    hf = clampf(x_note_hz(cut - rad), 1.0f, sr * 0.4535147f);
    *lc = op_coef(lf, sr);
    *hc = op_coef(hf, sr);
    *fg = hf / lf + 1.0f;
}

/* -------- the state-variable filter (Vital's DigitalSvf, basic 12 dB, Simper's form) with ramped coefficients --- */
typedef struct {
    float a1, a2, a3, m0, m1, m2;
} svfc_t;
typedef struct {
    svfc_t c, d;                          /* coefficients now, per-sample step */
    float note, k, m0, m1, m2;            /* smoothed design values */
    float ic1[2], ic2[2];
} svf_t;
INL float svf_g(float note)                 /* main loop */
{
    float hz = clampf(note_hz(note), 1.0f, 0.499f * SR);
    return fm_tanf(FM_PI * hz * ISR);
}
FXF float svf_g_r(float note)              /* RAM */
{
    float hz = clampf(x_note_hz(note), 1.0f, 0.499f * SR);
    return x_tan(FM_PI * hz * ISR);
}
INL float svf_tick(float *ic1, float *ic2, const svfc_t *c, float x)
{
    float v3 = x - *ic2, v1 = c->a1 * *ic1 + c->a2 * v3, v2 = *ic2 + c->a2 * *ic1 + c->a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    return c->m0 * x + c->m1 * v1 + c->m2 * v2;
}
INL void svf_step(svf_t *f)
{
    f->c.a1 += f->d.a1;
    f->c.a2 += f->d.a2;
    f->c.a3 += f->d.a3;
    f->c.m0 += f->d.m0;
    f->c.m1 += f->d.m1;
    f->c.m2 += f->d.m2;
}
/* this block's design (note, k, mix m0..m2): smooth (a), then ramp the coefficients over the n samples */
FXF void svf_block(svf_t *f, float note, float k, float m0, float m1, float m2, float a, int fresh, float inv)
{
    float g, a1;
    svfc_t t;
    if (fresh) {
        f->note = note, f->k = k, f->m0 = m0, f->m1 = m1, f->m2 = m2;
    } else {
        f->note += (note - f->note) * a;
        f->k += (k - f->k) * a;
        f->m0 += (m0 - f->m0) * a;
        f->m1 += (m1 - f->m1) * a;
        f->m2 += (m2 - f->m2) * a;
    }
    g = svf_g_r(f->note);
    a1 = 1.0f / (1.0f + g * (g + f->k));
    t.a1 = a1, t.a2 = g * a1, t.a3 = g * g * a1, t.m0 = f->m0, t.m1 = f->m1, t.m2 = f->m2;
    if (fresh) {
        f->c = t;
        f->d.a1 = f->d.a2 = f->d.a3 = f->d.m0 = f->d.m1 = f->d.m2 = 0.0f;
    } else {
        f->d.a1 = (t.a1 - f->c.a1) * inv;
        f->d.a2 = (t.a2 - f->c.a2) * inv;
        f->d.a3 = (t.a3 - f->c.a3) * inv;
        f->d.m0 = (t.m0 - f->c.m0) * inv;
        f->d.m1 = (t.m1 - f->c.m1) * inv;
        f->d.m2 = (t.m2 - f->c.m2) * inv;
    }
}
INL void svf_flush(svf_t *f)
{
    f->ic1[0] = fm_flush(f->ic1[0]), f->ic1[1] = fm_flush(f->ic1[1]);
    f->ic2[0] = fm_flush(f->ic2[0]), f->ic2[1] = fm_flush(f->ic2[1]);
}
/* Vital's resonance: q from 0.5 to 16 on a cubic; the SVF's k = 1 / q */
FXF float svf_k(float res)
{
    res = clamp01(res);
    return 1.0f / (0.5f + 15.5f * res * res * res);
}
/* the 12 dB style's pass blend (DigitalSvf::setupFilter): b -1 low pass .. 0 band pass .. 1 high pass */
FXF void svf_blend(float b, float k, float *m0, float *m1, float *m2)
{
    float band = x_sqrt(fm_maxf(1.0f - b * b, 0.0f)), lo = b < 0.0f ? -b : 0.0f, hi = b > 0.0f ? b : 0.0f;
    *m0 = hi;
    *m1 = band - k * hi;
    *m2 = lo - hi;
}

/* ============================================================== the state === */
static int16_t *ar_ch, *ar_dl, *ar_fl, *ar_rv;
static volatile fx_view_t VIEW[FXT_N];

/* ---------------------------------------------------------------- CHORUS --- */
/* Vital's ChorusModule: 1..4 voice pairs, each two taps per side (delay 1 / delay 2, the LFO half a cycle apart, the
 * right side a quarter cycle on), the pairs spread from the two delays toward their average, each with feedback and
 * Vital's delay filter. Here the taps share one line per side (the arena: 2048 samples, 46 ms), so the feedback is
 * the taps' mean and the filter is on the line's input; the modulation depth is 16 ms at most (Vital: 30 ms). */
#define CH_LEN 2048u
#define CH_MASK (CH_LEN - 1u)
#define CH_DEPTH 0.016f
static struct {
    float phase, d1, d2, depth;
    float tap[4][2][2];                   /* [pair][voice][side] delay (samples) now */
    float pg[4];                          /* pair gains (fade in / out) */
    float fb, lc, hc, fg, dry, wet;
    float lp[2], hp[2];
    uint32_t w, filled;
    int fresh;
} CH;

static RAMC NOIN void run_chorus(const float *p, float tempo, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, a = blk_alpha(n, 0.02f), d1, d2, avg, rate, fb, lc, hc, fg, dry, wet;
    float dt[4][2][2], dpg[4], dfb, dlc, dhc, dfg, ddry, dwet, norm;
    int nv = 1 + pidx(p[0], 4), np, i, v, c;
    uint32_t s;

    rate = lfo_rate(p[1], p[2], tempo);
    d1 = x_exp2(-10.0f + 4.35614f * clamp01(p[4]));
    d2 = x_exp2(-10.0f + 4.35614f * clamp01(p[5]));
    fb = (2.0f * clamp01(p[6]) - 1.0f) * 0.95f;
    dly_filter(8.0f + 128.0f * clamp01(p[7]), p[8], SR, &lc, &hc, &fg);
    eqpow(p[9], &dry, &wet);
    if (CH.fresh) {
        CH.d1 = d1, CH.d2 = d2, CH.depth = clamp01(p[3]);
        CH.fb = fb, CH.lc = lc, CH.hc = hc, CH.fg = fg, CH.dry = dry, CH.wet = wet;
    } else {
        CH.d1 += (d1 - CH.d1) * a;
        CH.d2 += (d2 - CH.d2) * a;
        CH.depth += (clamp01(p[3]) - CH.depth) * a;
    }
    CH.phase = frac1(CH.phase + rate * (float)n * ISR);
    avg = 0.5f * (CH.d1 + CH.d2);
    np = 0;
    NOUNROLL
    for (i = 0; i < 4; i++) {
        float po = (float)i * 0.25f / (float)nv, t = nv > 1 ? (float)i / (float)(nv - 1) : 0.0f;
        float tg = i < nv ? 1.0f : 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        for (v = 0; v < 2; v++) {
            float base = lerpf(v ? CH.d2 : CH.d1, avg, t);
            for (c = 0; c < 2; c++) {
                float ph = CH.phase + po + 0.25f * (float)c + 0.5f * (float)v;
                float mod = x_sin_turns(ph) * 0.5f + 1.0f;
                float ds = clampf((mod * CH.depth * CH_DEPTH + base) * SR, 2.0f, (float)CH_LEN - 4.0f);
                if (CH.fresh || CH.pg[i] <= 0.0f)
                    CH.tap[i][v][c] = ds;
                dt[i][v][c] = clampf((ds - CH.tap[i][v][c]) * inv, -0.6f, 0.6f);
            }
        }
        if (CH.fresh)
            CH.pg[i] = tg;
        dpg[i] = (tg - CH.pg[i]) * inv;
        if (CH.pg[i] > 0.0f || tg > 0.0f)
            np = i + 1;
    }
    norm = 0.0f;
    for (i = 0; i < np; i++)
        norm += fm_maxf(CH.pg[i], CH.pg[i] + dpg[i] * (float)n);
    norm = 0.5f / fm_maxf(norm, 1.0f);
    dfb = (fb - CH.fb) * inv, dlc = (lc - CH.lc) * inv, dhc = (hc - CH.hc) * inv, dfg = (fg - CH.fg) * inv;
    ddry = (dry - CH.dry) * inv, dwet = (wet - CH.wet) * inv;
    CH.fresh = 0;

    for (s = 0; s < n; s++) {
        float x[2], sum[2] = {0.0f, 0.0f}, fl = (float)CH.filled;
        x[0] = L[s], x[1] = R[s];
        CH.fb += dfb, CH.lc += dlc, CH.hc += dhc, CH.fg += dfg, CH.dry += ddry, CH.wet += dwet;
        NOUNROLL
        for (i = 0; i < np; i++) {
            float g = (CH.pg[i] += dpg[i]);
            for (v = 0; v < 2; v++)
                for (c = 0; c < 2; c++) {
                    float d = (CH.tap[i][v][c] += dt[i][v][c]);
                    if (d + 2.0f < fl)
                        sum[c] += g * rdline(ar_ch + c * CH_LEN, CH.w, d, CH_MASK);
                }
        }
        NOUNROLL
        for (c = 0; c < 2; c++) {
            float wv = clampf(x[c] + CH.fb * sum[c] * norm, -8.0f, 8.0f), lp;
            lp = op_tick(&CH.lp[c], wv * CH.fg, CH.lc);
            ar_ch[c * CH_LEN + CH.w] = f16(lp - op_tick(&CH.hp[c], lp, CH.hc));
        }
        CH.w = (CH.w + 1u) & CH_MASK;
        if (CH.filled < CH_LEN)
            CH.filled++;
        L[s] = CH.dry * x[0] + CH.wet * 0.5f * sum[0];
        R[s] = CH.dry * x[1] + CH.wet * 0.5f * sum[1];
    }
    CH.fb = fb, CH.lc = lc, CH.hc = hc, CH.fg = fg, CH.dry = dry, CH.wet = wet;
    for (i = 0; i < 4; i++)
        if (CH.pg[i] < 1e-4f)
            CH.pg[i] = 0.0f;
    for (c = 0; c < 2; c++)
        CH.lp[c] = fm_flush(CH.lp[c]), CH.hp[c] = fm_flush(CH.hp[c]);
    VIEW[FXT_CHORUS].a[0] = CH.phase;
    VIEW[FXT_CHORUS].a[1] = (float)nv;
    for (i = 0; i < 4; i++) {
        VIEW[FXT_CHORUS].a[2 + 2 * i] = CH.tap[i][0][0] * (1000.0f * ISR);
        VIEW[FXT_CHORUS].a[3 + 2 * i] = CH.tap[i][1][0] * (1000.0f * ISR);
    }
}

/* ------------------------------------------------------------------ COMP --- */
/* Vital's MultibandCompressor (OTT): Linkwitz-Riley crossovers at 120 Hz and 2.5 kHz (4th order: two Butterworth
 * SVFs, the high side as the allpass less the low side), per band and side an upper (downward) and a lower (upward)
 * mean-square envelope with Vital's attack / release (low 2.8 / 40 ms, band 1.4 / 28, high 0.7 / 15, times
 * e^(8p - 4)), thresholds and ratios fixed at Vital's defaults, scaled by the amount; the band gains are its makeup.
 * The gain is worked out every 4 samples and ramped between (Vital: every sample). */
#define CP_SUB 4u
static const float CP_ATT[3] = {2.8f, 1.4f, 0.7f}, CP_REL[3] = {40.0f, 28.0f, 15.0f};
static const float CP_UT[3] = {-28.0f, -25.0f, -30.0f}, CP_LT[3] = {-35.0f, -36.0f, -35.0f};
static const float CP_UR[3] = {0.9f, 0.857f, 1.0f}, CP_LR[3] = {0.8f, 0.8f, 0.8f};
static const uint8_t CP_MASK[4] = {7u, 3u, 6u, 2u};     /* enabled bands: multiband, low band, high band, single */
static struct {
    float a1, a2, a3;                     /* a crossover's SVF coefficients (k = sqrt 2) */
} XO[2];
static struct {
    float xa[2][4], xb[2][4];             /* the crossovers' states, per side */
    float he[3][2], le[3][2];             /* upper / lower envelopes (mean square) */
    float g[3][2], gd[3][2];              /* the compressor gain now, its per-sample step */
    float mk[3], mix;
    float msi[3], mso[3];
    uint32_t sub;
    int bands, fresh;
} CP;

INL void xover(float *st, int x, float in, float *lo, float *hi)
{
    float a1 = XO[x].a1, a2 = XO[x].a2, a3 = XO[x].a3;
    float v3 = in - st[1], v1 = a1 * st[0] + a2 * v3, v2 = st[1] + a2 * st[0] + a3 * v3, u3, u1, u2;
    st[0] = 2.0f * v1 - st[0];
    st[1] = 2.0f * v2 - st[1];
    u3 = v2 - st[3];
    u1 = a1 * st[2] + a2 * u3;
    u2 = st[3] + a2 * st[2] + a3 * u3;
    st[2] = 2.0f * u1 - st[2];
    st[3] = 2.0f * u2 - st[3];
    *lo = u2;
    *hi = in - 2.8284271f * v1 - u2;
}

static RAMC NOIN void run_comp(const float *p, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, att[3], rel[3], ut[3], lt[3], lut[3], llt[3], ur[3], lr[3], mk[3], dmk[3];
    float amount = clamp01(p[6]), mix = clamp01(p[7]), dmix, ea = x_exp2((clamp01(p[1]) * 8.0f - 4.0f) * FM_LOG2E);
    float er = x_exp2((clamp01(p[2]) * 8.0f - 4.0f) * FM_LOG2E), sumi[3] = {0, 0, 0}, sumo[3] = {0, 0, 0};
    int bands = pidx(p[0], 4), b, c;
    unsigned m;
    uint32_t s;

    if (bands != CP.bands || CP.fresh) {     /* Vital resets the filters and the envelopes on a band change */
        zero_bytes(CP.xa, sizeof CP.xa);
        zero_bytes(CP.xb, sizeof CP.xb);
        for (b = 0; b < 3; b++)
            for (c = 0; c < 2; c++) {
                CP.he[b][c] = x_exp2(CP_UT[b] * 0.33219281f);
                CP.le[b][c] = x_exp2(CP_LT[b] * 0.33219281f);   /* (Vital: 0, a 30 dB burst) */
                CP.g[b][c] = 1.0f, CP.gd[b][c] = 0.0f;
            }
        CP.bands = bands;
        CP.sub = 0;
    }
    m = CP_MASK[bands];
    NOUNROLL
    for (b = 0; b < 3; b++) {
        float as = fm_maxf(ea * CP_ATT[b] * (SR * 0.001f), 5.0f), rs = fm_maxf(er * CP_REL[b] * (SR * 0.001f), 5.0f);
        att[b] = 1.0f / (as + 1.0f);
        rel[b] = 1.0f / (rs + 1.0f);
        lut[b] = CP_UT[b] * 0.33219281f;   /* log2 of the mean-square thresholds */
        llt[b] = CP_LT[b] * 0.33219281f;
        ut[b] = x_exp2(lut[b]);
        lt[b] = x_exp2(llt[b]);
        ur[b] = CP_UR[b] * 0.5f * amount;
        lr[b] = CP_LR[b] * 0.5f * amount;
        mk[b] = x_db_gain(clamp01(p[3 + b]) * 60.0f - 30.0f);
        if (CP.fresh)
            CP.mk[b] = mk[b];
        dmk[b] = (mk[b] - CP.mk[b]) * inv;
    }
    if (CP.fresh)
        CP.mix = mix;
    dmix = (mix - CP.mix) * inv;
    CP.fresh = 0;

    for (s = 0; s < n; s++) {
        float x[2], y[2], bs[3][2];
        x[0] = L[s], x[1] = R[s];
        CP.mix += dmix;
        for (b = 0; b < 3; b++)
            CP.mk[b] += dmk[b];
        NOUNROLL
        for (c = 0; c < 2; c++) {
            float lo = 0.0f, mid = x[c], hi = 0.0f;
            if (m & 1u)                        /* low band: split at 120 Hz */
                xover(CP.xa[c], 0, mid, &lo, &mid);
            if (m & 4u)                        /* high band: split (the rest) at 2.5 kHz */
                xover(CP.xb[c], 1, mid, &mid, &hi);
            bs[0][c] = lo, bs[1][c] = mid, bs[2][c] = hi;
        }
        if (CP.sub == 0) {                     /* the gains for the next CP_SUB samples */
            NOUNROLL
            for (b = 0; b < 3; b++) {
                if (!(m & (1u << b)))
                    continue;
                for (c = 0; c < 2; c++) {
                    float e = ur[b] * (lut[b] - x_log2(CP.he[b][c])) + lr[b] * (llt[b] - x_log2(CP.le[b][c]));
                    float gt = x_exp2(e > 5.0f ? 5.0f : e);    /* Vital: at most 32 */
                    CP.gd[b][c] = (gt - CP.g[b][c]) * (1.0f / (float)CP_SUB);
                }
            }
        }
        CP.sub = (CP.sub + 1u) & (CP_SUB - 1u);
        y[0] = y[1] = 0.0f;
        NOUNROLL
        for (b = 0; b < 3; b++) {
            if (!(m & (1u << b)))
                continue;
            for (c = 0; c < 2; c++) {
                float v = bs[b][c], sq = v * v, he = CP.he[b][c], le = CP.le[b][c], g, o;
                he += (sq - he) * (sq > he ? att[b] : rel[b]);
                CP.he[b][c] = he > ut[b] ? he : ut[b];
                le += (sq - le) * (sq > le ? att[b] : rel[b]);
                CP.le[b][c] = le < lt[b] ? le : lt[b];
                g = (CP.g[b][c] += CP.gd[b][c]);
                o = v * (1.0f + CP.mix * (g * CP.mk[b] - 1.0f));
                y[c] += o;
                sumi[b] += sq;
                sumo[b] += o * o;
            }
        }
        L[s] = y[0], R[s] = y[1];
    }
    CP.mix = mix;
    NOUNROLL
    for (b = 0; b < 3; b++) {
        float k = fm_minf((float)n * (1.0f / (0.025f * SR)), 1.0f);
        CP.mk[b] = mk[b];
        CP.msi[b] += (sumi[b] * 0.5f * inv - CP.msi[b]) * k;
        CP.mso[b] += (sumo[b] * 0.5f * inv - CP.mso[b]) * k;
        for (c = 0; c < 2; c++)
            CP.le[b][c] = fm_flush(CP.le[b][c]);
    }
    for (c = 0; c < 2; c++)
        for (b = 0; b < 4; b++)
            CP.xa[c][b] = fm_flush(CP.xa[c][b]), CP.xb[c][b] = fm_flush(CP.xb[c][b]);
    NOUNROLL
    for (b = 0; b < 3; b++) {
        int on = (m >> b) & 1u;
        VIEW[FXT_COMP].a[b] = on ? fm_maxf(3.0103f * x_log2(CP.msi[b] + 1e-10f), -100.0f) : -100.0f;
        VIEW[FXT_COMP].a[3 + b] = on ? fm_maxf(3.0103f * x_log2(CP.mso[b] + 1e-10f), -100.0f) : -100.0f;
        VIEW[FXT_COMP].a[6 + b] = on ? 3.0103f * x_log2(CP.g[b][0] * CP.g[b][1] + 1e-20f) : 0.0f;
        VIEW[FXT_COMP].a[9 + b] = CP_UT[b];
        VIEW[FXT_COMP].a[12 + b] = CP_LT[b];
    }
    VIEW[FXT_COMP].a[15] = (float)m;
}

/* ----------------------------------------------------------------- DELAY --- */
/* Vital's StereoDelay: mono (both sides at time 1), stereo (right at time 2), ping pong (the mono input enters the
 * right side, the sides swap each pass), mid ping pong (stereo input, swapping, left time = time 1 + time 2); the
 * feedback through Vital's saturator (+-8; the int16 line holds +-2) and its low / high pass at cutoff +- spread x
 * 8 octaves; Vital's 20 ms half-life glide on time changes. The line is 16384 samples a side: up to 0.37 s at the
 * full rate; longer times switch the whole delay to half rate (to 0.74 s; longer is clamped): the wet fades out over
 * a block, the rate changes, the line restarts empty. */
#define DL_LEN 16384u
#define DL_MASK (DL_LEN - 1u)
static struct {
    float freq[2], per[2], vel[2];
    float fb, lc, hc, fg, dry, wet, gate;
    float lp[2], hp[2];
    float acc[2], y1[2];
    float lvl;
    uint32_t w, filled;
    int half, ph, fresh;
} DL;

FXF void dl_tick(float in0, float in1, int style, float pl, float pr, float fbl, float fbr, float *r0, float *r1)
{
    float fl = (float)DL.filled, a = 0.0f, b = 0.0f, w0, w1, lp;
    if (pl + 2.0f < fl)
        a = rdline(ar_dl, DL.w, pl, DL_MASK) * DL.gate;
    if (pr + 2.0f < fl)
        b = rdline(ar_dl + DL_LEN, DL.w, pr, DL_MASK) * DL.gate;
    switch (style) {
    case 2: {                              /* ping pong: the mono input enters on the right, swapped each pass */
        float mono = (in0 + in1) * 0.70710678f;
        w0 = b * fbr;
        w1 = mono + a * fbl;
        break;
    }
    case 3:                                /* mid ping pong */
        w0 = in1 + b * fbr;
        w1 = in0 + a * fbl;
        break;
    default:
        w0 = in0 + a * fbl;
        w1 = in1 + b * fbr;
        break;
    }
    w0 = clampf(w0, -8.0f, 8.0f);
    w1 = clampf(w1, -8.0f, 8.0f);
    lp = op_tick(&DL.lp[0], w0 * DL.fg, DL.lc);
    ar_dl[DL.w] = f16(lp - op_tick(&DL.hp[0], lp, DL.hc));
    lp = op_tick(&DL.lp[1], w1 * DL.fg, DL.lc);
    ar_dl[DL_LEN + DL.w] = f16(lp - op_tick(&DL.hp[1], lp, DL.hc));
    DL.w = (DL.w + 1u) & DL_MASK;
    if (DL.filled < DL_LEN)
        DL.filled++;
    *r0 = a;
    *r1 = b;
}

static RAMC NOIN void run_delay(const float *p, float tempo, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, t[2], sr, decay, need, pt[2], dper[2], fb, lc, hc, fg, dry, wet, gt;
    float dfb, dlc, dhc, dfg, ddry, dwet, dgate;
    int style = pidx(p[0], 4), want, c;
    uint32_t s;

    tempo = fm_maxf(tempo, 1.0f);
    for (c = 0; c < 2; c++) {
        float b = sync_beats(p[2 + 2 * c]);
        t[c] = b > 0.0f ? b * 60.0f / tempo : 0.01f * fm_exp2f(clamp01(p[1 + 2 * c]) * 6.643856f);
    }
    if (style == 0)
        t[1] = t[0];
    decay = x_exp2(-(float)n * (1.0f / (0.02f * SR)));
    for (c = 0; c < 2; c++) {
        float f = 1.0f / t[c];
        DL.freq[c] = DL.fresh ? f : f + (DL.freq[c] - f) * decay;
        pt[c] = SR / DL.freq[c];
    }
    if (style == 3)
        pt[0] += pt[1];
    need = fm_maxf(pt[0], pt[1]);
    want = DL.half ? need > 15000.0f : need > 16300.0f;
    gt = 1.0f;
    if (want != DL.half) {
        if (DL.fresh || DL.gate <= 0.0f) {   /* faded out: change the rate, restart the line */
            DL.per[0] *= want ? 0.5f : 2.0f;
            DL.per[1] *= want ? 0.5f : 2.0f;
            DL.vel[0] = DL.vel[1] = 0.0f;
            DL.half = want;
            DL.filled = 0;
            DL.ph = 0;
            DL.acc[0] = DL.acc[1] = DL.y1[0] = DL.y1[1] = 0.0f;
        } else
            gt = 0.0f;
    }
    sr = DL.half ? 0.5f * SR : SR;
    for (c = 0; c < 2; c++) {
        pt[c] = clampf(pt[c] * (DL.half ? 0.5f : 1.0f), 3.0f, (float)DL_LEN - 4.0f);
        if (DL.fresh)
            DL.per[c] = pt[c];
        pt[c] = lerpf(DL.per[c], pt[c], 0.5f);
        /* the glide's speed: at most +-0.5 in read rate, gained over ~6 ms (Vital's is unlimited: a big jump races
         * through the line, a burst of noise; here it is a tape-like slide) */
        dper[c] = glide(&DL.vel[c], (pt[c] - DL.per[c]) * inv, DL.half ? 0.25f : 0.5f,
                        (DL.half ? 0.0009f : 0.0018f) * (float)n);
        pt[c] = DL.per[c] + dper[c] * (float)n;
    }
    fb = 2.0f * clamp01(p[5]) - 1.0f;
    dly_filter(8.0f + 128.0f * clamp01(p[6]), p[7], sr, &lc, &hc, &fg);
    eqpow(p[8], &dry, &wet);
    if (DL.fresh) {
        DL.fb = fb, DL.lc = lc, DL.hc = hc, DL.fg = fg, DL.dry = dry, DL.wet = wet;
        DL.gate = gt;
    }
    DL.fresh = 0;
    dfb = (fb - DL.fb) * inv, dlc = (lc - DL.lc) * inv, dhc = (hc - DL.hc) * inv, dfg = (fg - DL.fg) * inv;
    ddry = (dry - DL.dry) * inv, dwet = (wet - DL.wet) * inv, dgate = (gt - DL.gate) * inv;

    for (s = 0; s < n; s++) {
        float x0 = L[s], x1 = R[s], o0, o1;
        DL.per[0] += dper[0], DL.per[1] += dper[1];
        DL.fb += dfb, DL.lc += dlc, DL.hc += dhc, DL.fg += dfg, DL.dry += ddry, DL.wet += dwet;
        DL.gate += dgate;
        if (!DL.half)
            dl_tick(x0, x1, style, DL.per[0], DL.per[1], DL.fb, style == 2 ? 1.0f : DL.fb, &o0, &o1);
        else {
            DL.acc[0] += x0, DL.acc[1] += x1;
            if (DL.ph) {
                float r0, r1;
                dl_tick(0.5f * DL.acc[0], 0.5f * DL.acc[1], style, DL.per[0], DL.per[1], DL.fb,
                        style == 2 ? 1.0f : DL.fb, &r0, &r1);
                o0 = 0.5f * (DL.y1[0] + r0), o1 = 0.5f * (DL.y1[1] + r1);
                DL.y1[0] = r0, DL.y1[1] = r1;
                DL.acc[0] = DL.acc[1] = 0.0f;
            } else
                o0 = DL.y1[0], o1 = DL.y1[1];
            DL.ph ^= 1;
        }
        L[s] = DL.dry * x0 + DL.wet * o0;
        R[s] = DL.dry * x1 + DL.wet * o1;
        DL.lvl = peak_decay(DL.lvl, o0 + o1);
    }
    DL.per[0] = pt[0], DL.per[1] = pt[1];
    DL.fb = fb, DL.lc = lc, DL.hc = hc, DL.fg = fg, DL.dry = dry, DL.wet = wet, DL.gate = gt;
    for (c = 0; c < 2; c++)
        DL.lp[c] = fm_flush(DL.lp[c]), DL.hp[c] = fm_flush(DL.hp[c]);
    VIEW[FXT_DELAY].a[0] = DL.per[0] / sr;
    VIEW[FXT_DELAY].a[1] = DL.per[1] / sr;
    VIEW[FXT_DELAY].a[2] = fb;
    VIEW[FXT_DELAY].a[3] = (float)DL.half;
    VIEW[FXT_DELAY].a[4] = DL.lvl;
}

/* ------------------------------------------------------------------ DIST --- */
/* Vital's Distortion (soft clip, hard clip, linear fold, sine fold, bit crush, downsample; drive -30..+30 dB) with
 * its DigitalSvf (12 dB, basic) before or after, and a linear mix. A type change crossfades over a block. */
static struct {
    svf_t f;
    float d, mix, hold[2], cnt, xin, xout;
    int type, fresh;
} DS;

/* the drive's meaning for a type: gain, crush step, or downsample period (samples) */
FXF float ds_drive(int type, float d)
{
    if (type == 4)
        return fm_maxf(d * d, 1.49e-8f);
    if (type == 5) {
        float u = 1.0f - d;
        return fm_maxf(0.495f / fm_maxf(u * u, 1.49e-8f), 1.0f);
    }
    return x_db_gain(d * 60.0f - 30.0f);
}
FXF float ds_shape(int type, float x, float dv, int c, int tick)
{
    switch (type) {
    case 0:
        return x_tanh(x * dv);
    case 1:
        return clampf(x * dv, -1.0f, 1.0f);
    case 2:
        return fm_fabsf(frac1(x * dv * 0.25f + 0.75f) * -4.0f + 2.0f) - 1.0f;
    case 3:
        return x_sin_turns(frac1(x * dv * -0.25f + 0.5f));
    case 4:
        return fm_floorf(x / dv + 0.5f) * dv;
    default:
        if (tick)
            DS.hold[c] = x;
        return DS.hold[c];
    }
}

static RAMC NOIN void run_dist(const float *p, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, d = clamp01(p[1]), mix = clamp01(p[6]), k = svf_k(p[4]), m0, m1, m2;
    float dv, ddv, dvo = 0.0f, ddvo = 0.0f, dmix, xf = 1.0f, dxf = 0.0f;
    int type = pidx(p[0], 6), order = pidx(p[2], 3), old = DS.type;
    uint32_t s;

    svf_blend(2.0f * clamp01(p[5]) - 1.0f, k, &m0, &m1, &m2);
    svf_block(&DS.f, 8.0f + 128.0f * clamp01(p[3]), k, m0, m1, m2, blk_alpha(n, 0.004f), DS.fresh, inv);
    if (DS.fresh) {
        DS.d = d, DS.mix = mix, old = type;
        DS.type = type;
    }
    if (type != old) {
        xf = 0.0f, dxf = inv;
        dvo = ds_drive(old, DS.d);
        ddvo = (ds_drive(old, d) - dvo) * inv;
        DS.cnt = 0.0f;
    }
    dv = ds_drive(type, DS.d);
    ddv = (ds_drive(type, d) - dv) * inv;
    dmix = (mix - DS.mix) * inv;
    DS.fresh = 0;
    for (s = 0; s < n; s++) {
        float x[2], y[2];
        int c, tick = 0;
        x[0] = L[s], x[1] = R[s];
        svf_step(&DS.f);
        dv += ddv, dvo += ddvo, xf += dxf;
        DS.mix += dmix;
        if (type == 5 || old == 5) {           /* downsample: hold a sample every period */
            float per = type == 5 ? dv : dvo;
            DS.cnt += 1.0f;
            if (DS.cnt >= per) {
                tick = 1;
                DS.cnt -= per;
                if (DS.cnt >= per)
                    DS.cnt = 0.0f;
            }
        }
        NOUNROLL
        for (c = 0; c < 2; c++) {
            float v = x[c], o;
            if (order == 1)
                v = svf_tick(&DS.f.ic1[c], &DS.f.ic2[c], &DS.f.c, v);
            o = ds_shape(type, v, dv, c, tick);
            if (xf < 1.0f)
                o = lerpf(ds_shape(old, v, dvo, c, tick), o, xf);
            if (order == 2)
                o = svf_tick(&DS.f.ic1[c], &DS.f.ic2[c], &DS.f.c, o);
            y[c] = lerpf(x[c], o, DS.mix);
        }
        L[s] = y[0], R[s] = y[1];
        DS.xin = peak_decay(DS.xin, x[0] + x[1]);
        DS.xout = peak_decay(DS.xout, y[0] + y[1]);
    }
    DS.d = d, DS.mix = mix, DS.type = type;
    svf_flush(&DS.f);
    VIEW[FXT_DIST].a[0] = 0.5f * DS.xin;
    VIEW[FXT_DIST].a[1] = 0.5f * DS.xout;
    VIEW[FXT_DIST].a[2] = ds_drive(type, d);
}

/* -------------------------------------------------------------------- EQ --- */
/* Vital's EqualizerModule: three DigitalSvfs in series: low shelf or high pass, peak (Vital's shelving style at blend
 * 0) or notch, high shelf or low pass; gains -18..+18 dB (Vital: +-15); Vital's resonance (q 0.5..16, cubic) and its
 * shelf cutoff shift (the cutoff moves by a quarter of the gain, in octaves of amplitude). */
static struct {
    svf_t b[3];
    float xin, xout;
    int fresh;
} EQ;

/* band i's design from the parameters: note, k, m0..m2 (A, sA: the gain's amplitude and its root, from the
 * caller's maths: inline in the main loop, x_* in RAM) */
INL float eq_db(const float *p, int i) { return clamp01(p[4 * i + 2]) * 36.0f - 18.0f; }
INL void eq_design(const float *p, int i, float A, float sA, float *note, float *k, float *m0, float *m1, float *m2)
{
    int mode = pidx(p[4 * i], 2);
    float db = eq_db(p, i), r = clamp01(p[4 * i + 3]);
    *note = 8.0f + 128.0f * clamp01(p[4 * i + 1]);
    *k = 1.0f / (0.5f + 15.5f * r * r * r);
    if (i == 0) {
        if (mode)
            *m0 = 1.0f, *m1 = -*k, *m2 = -1.0f;                          /* high pass */
        else {
            *m0 = 1.0f, *m1 = *k * (sA - 1.0f), *m2 = A - 1.0f;          /* low shelf */
            *note -= 0.4982892f * db;
        }
    } else if (i == 1) {
        if (mode)
            *m0 = 1.0f, *m1 = -*k, *m2 = 0.0f;                           /* notch */
        else
            *m0 = 1.0f, *m1 = *k * (A - 1.0f), *m2 = 0.0f;               /* peak */
    } else {
        if (mode)
            *m0 = 0.0f, *m1 = 0.0f, *m2 = 1.0f;                          /* low pass */
        else {
            *m0 = A, *m1 = *k * (sA - A), *m2 = 1.0f - A;                /* high shelf */
            *note += 0.4982892f * db;
        }
    }
}

static RAMC NOIN void run_eq(const float *p, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, a = blk_alpha(n, 0.004f);
    int i;
    uint32_t s;
    NOUNROLL
    for (i = 0; i < 3; i++) {
        float note, k, m0, m1, m2, A = x_db_gain(eq_db(p, i));
        eq_design(p, i, A, x_sqrt(A), &note, &k, &m0, &m1, &m2);
        svf_block(&EQ.b[i], note, k, m0, m1, m2, a, EQ.fresh, inv);
    }
    EQ.fresh = 0;
    for (s = 0; s < n; s++) {
        float l = L[s], r = R[s];
        EQ.xin = peak_decay(EQ.xin, l + r);
        NOUNROLL
        for (i = 0; i < 3; i++) {
            svf_t *f = &EQ.b[i];
            svf_step(f);
            l = svf_tick(&f->ic1[0], &f->ic2[0], &f->c, l);
            r = svf_tick(&f->ic1[1], &f->ic2[1], &f->c, r);
        }
        L[s] = l, R[s] = r;
        EQ.xout = peak_decay(EQ.xout, l + r);
    }
    for (i = 0; i < 3; i++) {
        svf_flush(&EQ.b[i]);
        VIEW[FXT_EQ].a[2 + i] = EQ.b[i].note;
    }
    VIEW[FXT_EQ].a[0] = 0.5f * EQ.xin;
    VIEW[FXT_EQ].a[1] = 0.5f * EQ.xout;
}

/* ---------------------------------------------------------------- FILTER --- */
/* Vital's FilterModule (the effect's filter): filt.h's models on the master bus, with a linear mix. */
static struct {
    filt_t f;
    float mix, xin, xout;
    int model, style, fresh;
} FT;

static RAMC NOIN void run_filter(const float *p, float *L, float *R, uint32_t n)
{
    float dl[CTL], dr[CTL], inv = 1.0f / (float)n, mix = clamp01(p[8]), dmix;
    int model = pidx(p[0], FLT_N), style = pidx(p[1], FLT_NSTYLE[model]);
    filt_ctl_t c;
    uint32_t s;
    if (n > CTL)
        n = CTL;
    if (FT.fresh || model != FT.model || style != FT.style) {
        filt_reset(&FT.f, model, style);
        FT.model = model, FT.style = style;
    }
    if (FT.fresh)
        FT.mix = mix;
    FT.fresh = 0;
    c.cutoff = 8.0f + 128.0f * clamp01(p[2]);
    c.reso = clamp01(p[3]);
    c.drive = clamp01(p[4]);
    c.blend = clamp01(p[5]);
    c.fx = clamp01(p[6]);
    c.fy = clamp01(p[7]);
    c.transpose = 0.0f;
    for (s = 0; s < n; s++)
        dl[s] = L[s], dr[s] = R[s];
    filt_ctl(&FT.f, &c, n);
    filt_run(&FT.f, L, R, n);
    dmix = (mix - FT.mix) * inv;
    for (s = 0; s < n; s++) {
        FT.mix += dmix;
        L[s] = lerpf(dl[s], L[s], FT.mix);
        R[s] = lerpf(dr[s], R[s], FT.mix);
        FT.xin = peak_decay(FT.xin, dl[s] + dr[s]);
        FT.xout = peak_decay(FT.xout, L[s] + R[s]);
    }
    FT.mix = mix;
    VIEW[FXT_FILTER].a[0] = 0.5f * FT.xin;
    VIEW[FXT_FILTER].a[1] = 0.5f * FT.xout;
    VIEW[FXT_FILTER].a[2] = (float)model;
    VIEW[FXT_FILTER].a[3] = (float)style;
}

/* --------------------------------------------------------------- FLANGER --- */
/* Vital's FlangerModule: a triangle LFO (the sides offset by the phase offset) sweeps a short delay around the
 * center note's period (depth 0..1: x0..x2 of the period less 0.5 ms), feedback -1..1 through a hard clip, Vital's
 * mix (0..1 here = Vital's 0..50%, equal power). The arena holds 23 ms a side: a long period (a low center note) is
 * clamped there and the depth is narrowed to fit. */
#define FL_LEN 1024u
#define FL_MASK (FL_LEN - 1u)
#define FL_MAXD ((float)(FL_LEN - 4u) * ISR)
static struct {
    float phase, note, depth, d[2], vel[2], fb, dry, wet;
    uint32_t w, filled;
    int fresh;
} FL;

INL float tri01(float ph) { return fm_fabsf(2.0f * frac1(ph + 0.75f) - 1.0f); }   /* Vital's triangleWave */

static RAMC NOIN void run_flanger(const float *p, float tempo, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, a = blk_alpha(n, 0.02f), rate = lfo_rate(p[0], p[1], tempo), off = clamp01(p[5]);
    float fb = 2.0f * clamp01(p[2]) - 1.0f, dry, wet, dd[2], dfb, ddry, dwet, center, depth;
    int c;
    uint32_t s;
    eqpow(0.5f * clamp01(p[6]), &dry, &wet);
    if (FL.fresh) {
        FL.note = 8.0f + 128.0f * clamp01(p[3]);
        FL.depth = clamp01(p[4]);
        FL.fb = fb, FL.dry = dry, FL.wet = wet;
    } else {
        FL.note += (8.0f + 128.0f * clamp01(p[3]) - FL.note) * a;
        FL.depth += (clamp01(p[4]) - FL.depth) * a;
    }
    FL.phase = frac1(FL.phase + rate * (float)n * ISR);
    center = fm_minf(1.0f / x_note_hz(FL.note), FL_MAXD);
    depth = FL.depth;
    if (center > 0.0005f)
        depth = fm_minf(depth, (FL_MAXD - 0.0005f) / (center - 0.0005f) - 1.0f);
    NOUNROLL
    for (c = 0; c < 2; c++) {
        float ph = FL.phase - 0.5f * off + (c ? off : 0.0f);
        float mod = depth * (tri01(ph) * 2.0f - 1.0f) + 1.0f;
        float d = ((center - 0.0005f) * mod + 0.0005f);
        d = clampf(fm_maxf(d, 1.0f / 20000.0f) * SR, 2.0f, (float)FL_LEN - 4.0f);
        if (FL.fresh)
            FL.d[c] = d;
        dd[c] = glide(&FL.vel[c], (d - FL.d[c]) * inv, 0.6f, 0.002f * (float)n);   /* as the delay's */
    }
    FL.fresh = 0;
    dfb = (fb - FL.fb) * inv, ddry = (dry - FL.dry) * inv, dwet = (wet - FL.wet) * inv;
    for (s = 0; s < n; s++) {
        float x[2], fl = (float)FL.filled;
        x[0] = L[s], x[1] = R[s];
        FL.fb += dfb, FL.dry += ddry, FL.wet += dwet;
        NOUNROLL
        for (c = 0; c < 2; c++) {
            float d = (FL.d[c] += dd[c]), r = 0.0f;
            if (d + 2.0f < fl)
                r = rdline(ar_fl + c * FL_LEN, FL.w, d, FL_MASK);
            ar_fl[c * FL_LEN + FL.w] = f16(clampf(x[c] + r * FL.fb, -1.0f, 1.0f));
            x[c] = FL.dry * x[c] + FL.wet * r;
        }
        FL.w = (FL.w + 1u) & FL_MASK;
        if (FL.filled < FL_LEN)
            FL.filled++;
        L[s] = x[0], R[s] = x[1];
    }
    FL.fb = fb, FL.dry = dry, FL.wet = wet;
    VIEW[FXT_FLANGER].a[0] = FL.phase;
    VIEW[FXT_FLANGER].a[1] = FL.d[0] * (1000.0f * ISR);
    VIEW[FXT_FLANGER].a[2] = FL.d[1] * (1000.0f * ISR);
}

/* ---------------------------------------------------------------- PHASER --- */
/* Vital's Phaser with its PhaserFilter (clean): a triangle LFO sweeps the cutoff (center +- depth, 0..48 semitones,
 * the sides offset), 12 one-pole allpass stages tapped after 4, 8 and 12 (the blend: 0 = 4, 0.5 = 8, 1 = 12 stages),
 * the feedback band-limited (20x above and below the cutoff) and tanh-saturated, out = (in + allpass) / 2, a linear
 * mix. The cutoff coefficient is worked out per block and ramped (Vital: per sample). */
static struct {
    float phase, center, g[2], gl[2], gh[2], res, pk[3], mix;
    float st[2][12], lo[2], hi[2], ap[2];
    float cut[2];
    int fresh;
} PH;

static RAMC NOIN void run_phaser(const float *p, float tempo, float *L, float *R, uint32_t n)
{
    float inv = 1.0f / (float)n, rate = lfo_rate(p[0], p[1], tempo), res = clamp01(p[2]);
    float center = 8.0f + 128.0f * clamp01(p[3]), depth = 48.0f * clamp01(p[4]), bl = 2.0f * clamp01(p[5]);
    float off = clamp01(p[6]), mix = clamp01(p[7]), pk[3], dg[2], dgl[2], dgh[2], dres, dpk[3], dmix;
    int c, i;
    uint32_t s;
    if (PH.fresh)                          /* the center glides (20 ms): a jump sweeps 12 stages, a chirp */
        PH.center = center;
    else
        PH.center += (center - PH.center) * blk_alpha(n, 0.02f);
    center = PH.center;
    pk[0] = clamp01(1.0f - bl);
    pk[2] = clamp01(bl - 1.0f);
    pk[1] = 1.0f - pk[0] - pk[2];
    PH.phase = frac1(PH.phase + rate * (float)n * ISR);
    NOUNROLL
    for (c = 0; c < 2; c++) {
        float ph = frac1(PH.phase + (c ? -0.5f : 0.5f) * off), m = 1.0f - fm_fabsf(2.0f * ph - 1.0f);
        float note = center + (2.0f * m - 1.0f) * depth, hz = fm_minf(x_note_hz(note), SR), w, g;
        w = FM_PI * hz * ISR;
        g = x_tan(w / (w + 1.0f));
        PH.cut[c] = note;
        if (PH.fresh)
            PH.g[c] = g, PH.gl[c] = fm_minf(g * 20.0f, 0.9f), PH.gh[c] = g * (1.0f / 20.0f);
        dg[c] = (g - PH.g[c]) * inv;
        dgl[c] = (fm_minf(g * 20.0f, 0.9f) - PH.gl[c]) * inv;
        dgh[c] = (g * (1.0f / 20.0f) - PH.gh[c]) * inv;
    }
    if (PH.fresh) {
        PH.res = res, PH.mix = mix;
        for (i = 0; i < 3; i++)
            PH.pk[i] = pk[i];
    }
    PH.fresh = 0;
    dres = (res - PH.res) * inv, dmix = (mix - PH.mix) * inv;
    for (i = 0; i < 3; i++)
        dpk[i] = (pk[i] - PH.pk[i]) * inv;
    for (s = 0; s < n; s++) {
        float x[2];
        x[0] = L[s], x[1] = R[s];
        PH.res += dres, PH.mix += dmix;
        PH.pk[0] += dpk[0], PH.pk[1] += dpk[1], PH.pk[2] += dpk[2];
        NOUNROLL
        for (c = 0; c < 2; c++) {
            float g = (PH.g[c] += dg[c]), *st = PH.st[c], lows, highs, in, y, o1, o3;
            PH.gl[c] += dgl[c], PH.gh[c] += dgh[c];
            lows = op_tick(&PH.lo[c], PH.ap[c], PH.gl[c]);
            highs = op_tick(&PH.hi[c], lows, PH.gh[c]);
            in = (PH.res * 0.5f + 1.0f) * x[c] + x_tanh(PH.res * (lows - highs));
            for (i = 0; i < 4; i++)
                y = op_tick(&st[i], in, g), in -= 2.0f * y;
            o1 = in;
            for (i = 4; i < 8; i++)
                y = op_tick(&st[i], in, g), in -= 2.0f * y;
            o3 = in;
            for (i = 8; i < 12; i++)
                y = op_tick(&st[i], in, g), in -= 2.0f * y;
            PH.ap[c] = PH.pk[0] * o1 + PH.pk[1] * o3 + PH.pk[2] * in;
            x[c] = lerpf(x[c], (x[c] + PH.ap[c]) * 0.5f, PH.mix);
        }
        L[s] = x[0], R[s] = x[1];
    }
    PH.res = res, PH.mix = mix;
    for (c = 0; c < 2; c++) {
        PH.ap[c] = fm_flush(PH.ap[c]), PH.lo[c] = fm_flush(PH.lo[c]), PH.hi[c] = fm_flush(PH.hi[c]);
        for (i = 0; i < 12; i++)
            PH.st[c][i] = fm_flush(PH.st[c][i]);
    }
    VIEW[FXT_PHASER].a[0] = PH.phase;
    VIEW[FXT_PHASER].a[1] = PH.cut[0];
    VIEW[FXT_PHASER].a[2] = PH.cut[1];
}

/* ---------------------------------------------------------------- REVERB --- */
/* Vital's Reverb, scaled down: 8 feedback lines (Vital: 16) at half rate (22.05 kHz), each through an allpass
 * diffuser (feedback 0.6, the input fed into the allpasses as Vital does), mixed by an orthogonal matrix (Vital's
 * Householder 4 within a group of four, here with a 2-point Hadamard across the two groups: Vital's is Householder
 * x Householder over 4 x 4), a one-pole high shelf in each line, the decay per line for a T60 of the decay time
 * (counting the allpass in the loop), the lines' lengths swept by Vital's quadrature chorus. The input is the mono
 * sum, pre-delayed (here before the network; Vital delays the output) and band-limited by the pre low / high cuts.
 * The size spans Vital's 2^-3 .. 2^-0.51 of its line lengths (Vital: 2^-3 .. 2^1): the arena's limit. */
#define RV_N 8
#define RV_SR (0.5f * SR)
#define RV_MMAX 0.30f                      /* the largest size: Vital's line lengths x this */
#define RV_PD 3000u                        /* pre-delay line: 136 ms at half rate */
static const float RV_D[RV_N] = {3376.6f, 4639.2f, 3852.25f, 5664.25f, 2260.77f, 3259.485f, 2632.78f, 2815.125f};
static const uint16_t RV_AP[RV_N] = {500, 399, 466, 438, 447, 403, 453, 426};
static struct {
    int16_t *line[RV_N], *ap[RV_N], *pd;
    uint32_t len[RV_N];
    float bq[5];                           /* the decimating low pass: b0, b1, b2, a1, a2 */
} RL;
static struct {
    uint32_t w[RV_N], apw[RV_N], pdw, filled;
    float off[RV_N], dec[RV_N], shs[RV_N], el[RV_N], ea[RV_N];
    float m, pds, pdel, phase, drift;
    float hamp, hc, lpc, hpc, lps, hps;
    float dry, wet;
    float bz[2], y1[2], lvl, ilvl;
    int ph, fresh;
} RV;

FXF void rv_targets(float m, float drift, float phase, float *off)
{
    int j;
    NOUNROLL
    for (j = 0; j < 4; j++) {
        float ph = phase + (float)j * (1.0f / 16.0f);
        off[j] = RV_D[j] * m + drift * x_sin_turns(ph + 0.25f);
        off[4 + j] = RV_D[4 + j] * m + drift * x_sin_turns(ph);
    }
    for (j = 0; j < RV_N; j++)
        off[j] = clampf(off[j], 2.0f, (float)RL.len[j] - 3.0f);
}

static RAMC NOIN void run_reverb(const float *p, float *L, float *R, uint32_t n)
{
    float a = blk_alpha(n, 0.05f), m, decay, dsec, hamp, hc, lpc, hpc, dry, wet, pd, off[RV_N], dec[RV_N];
    float doff[RV_N], ddec[RV_N], dhamp, dhc, dlpc, dhpc, dpd, ddry, dwet, invt, rate, amt;
    uint32_t s, nt = (n + (uint32_t)RV.ph) >> 1;
    int k;

    m = x_exp2(-3.0f + clamp01(p[0]) * (3.0f - 0.5145732f));       /* 0.125 .. 0.70 */
    dsec = 0.1f * x_exp2(clamp01(p[1]) * 9.321928f);                /* 0.1 .. 64 s */
    lpc = op_coef(x_note_hz(128.0f * clamp01(p[2])), RV_SR);
    hpc = op_coef(x_note_hz(128.0f * clamp01(p[3])), RV_SR);
    hc = op_coef(x_note_hz(128.0f * clamp01(p[4])), RV_SR);
    hamp = x_db_gain(clamp01(p[5]) * 6.0f - 6.0f);
    amt = clamp01(p[6]);
    amt *= amt;
    rate = x_exp2(clamp01(p[7]) * 11.0f - 8.0f);
    pd = clampf(clamp01(p[8]) * 0.3f * RV_SR, 1.0f, (float)RV_PD - 4.0f);
    eqpow(p[9], &dry, &wet);
    if (RV.fresh) {
        RV.m = m, RV.pds = RV.pdel = pd;
    } else {
        RV.m += (m - RV.m) * a;
        RV.pds += (pd - RV.pds) * a;
    }
    RV.drift = fm_minf(amt * 1250.0f, 0.2f * RV_D[4]) * RV.m;
    RV.phase = frac1(RV.phase + rate * (float)n * ISR);
    rv_targets(RV.m, RV.drift, RV.phase, off);
    decay = -9.965784f / (dsec * RV_SR);                             /* log2(0.001) per tick of loop */
    NOUNROLL
    for (k = 0; k < RV_N; k++)
        dec[k] = x_exp2(decay * (RV_D[k] * RV.m + (float)RV_AP[k]));
    if (RV.fresh) {
        for (k = 0; k < RV_N; k++)
            RV.off[k] = off[k], RV.dec[k] = dec[k];
        RV.hamp = hamp, RV.hc = hc, RV.lpc = lpc, RV.hpc = hpc, RV.dry = dry, RV.wet = wet;
    }
    RV.fresh = 0;
    invt = nt ? 1.0f / (float)nt : 0.0f;
    for (k = 0; k < RV_N; k++) {
        doff[k] = clampf((off[k] - RV.off[k]) * invt, -0.3f, 0.3f);   /* size / chorus glides: read rate +-0.3 */
        off[k] = RV.off[k] + doff[k] * (float)nt;
        ddec[k] = (dec[k] - RV.dec[k]) * invt;
    }
    dhamp = (hamp - RV.hamp) * invt, dhc = (hc - RV.hc) * invt;
    dlpc = (lpc - RV.lpc) * invt, dhpc = (hpc - RV.hpc) * invt;
    dpd = clampf((RV.pds - RV.pdel) * invt, -0.5f, 0.5f);
    ddry = (dry - RV.dry) * (1.0f / (float)n), dwet = (wet - RV.wet) * (1.0f / (float)n);

    for (s = 0; s < n; s++) {
        float x0 = L[s], x1 = R[s], mono = 0.5f * (x0 + x1), o0, o1, z;
        /* the decimating low pass (transposed direct form II) */
        z = RL.bq[0] * mono + RV.bz[0];
        RV.bz[0] = RL.bq[1] * mono - RL.bq[3] * z + RV.bz[1];
        RV.bz[1] = RL.bq[2] * mono - RL.bq[4] * z;
        RV.dry += ddry, RV.wet += dwet;
        RV.ilvl = peak_decay(RV.ilvl, mono);
        if (RV.ph) {
            float fl = (float)RV.filled, in, r[RV_N], o[RV_N], w[RV_N], st[RV_N], s0, s1, t[4], tot0, tot1;
            int j;
            /* pre-delay, pre-filters */
            RL.pd[RV.pdw] = f16(z);
            RV.pdel += dpd;
            in = RV.pdel + 1.0f < fl ? rdlen(RL.pd, RV.pdw, RV_PD, RV.pdel) : 0.0f;
            if (++RV.pdw >= RV_PD)
                RV.pdw = 0;
            RV.lpc += dlpc, RV.hpc += dhpc, RV.hc += dhc, RV.hamp += dhamp;
            in = (op_tick(&RV.hps, in, RV.hpc) - op_tick(&RV.lps, in, RV.lpc)) * 0.25f;
            /* the lines, through their allpasses */
            NOUNROLL
            for (k = 0; k < RV_N; k++) {
                float d = (RV.off[k] += doff[k]), av = 0.0f, v;
                RV.dec[k] += ddec[k];
                r[k] = d + 2.0f < fl ? rdlen(RL.line[k], RV.w[k], RL.len[k], d) : 0.0f;
                if (fl >= (float)RV_AP[k])
                    av = rd16f(RL.ap[k][RV.apw[k]]);
                v = r[k] - 0.6f * av;
                RL.ap[k][RV.apw[k]] = f16e(in + v, &RV.ea[k]);
                if (++RV.apw[k] >= RV_AP[k])
                    RV.apw[k] = 0;
                o[k] = av + 0.6f * v;
            }
            /* the mixing matrix: Householder 4 in each group, Hadamard 2 across */
            s0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
            s1 = 0.5f * (o[4] + o[5] + o[6] + o[7]);
            for (j = 0; j < 4; j++) {
                float u = o[j] - s0, v = o[4 + j] - s1;
                w[j] = (u + v) * 0.70710678f;
                w[4 + j] = (u - v) * 0.70710678f;
            }
            /* the high shelf, the decay, the store */
            NOUNROLL
            for (k = 0; k < RV_N; k++) {
                float hf = op_tick(&RV.shs[k], w[k], RV.hc);
                w[k] = hf + RV.hamp * (w[k] - hf);
                st[k] = RV.dec[k] * w[k];
                RL.line[k][RV.w[k]] = f16e(st[k], &RV.el[k]);
                if (++RV.w[k] >= RL.len[k])
                    RV.w[k] = 0;
            }
            /* the output: the lines' sum per lane, and Vital's feed-forward of the stores */
            s0 = 0.5f * (st[0] + st[1] + st[2] + st[3]);
            s1 = 0.5f * (st[4] + st[5] + st[6] + st[7]);
            for (j = 0; j < 4; j++) {
                float u = st[j] - s0, v = st[4 + j] - s1;
                t[j] = w[j] + w[4 + j] + 0.125f * 0.70710678f * (RV.dec[j] * (u + v) + RV.dec[4 + j] * (u - v));
            }
            tot0 = (t[0] + t[2]) * 1.41421356f;     /* 4 lines a side x sqrt 2: Vital's 8 lines' power */
            tot1 = (t[1] + t[3]) * 1.41421356f;
            o0 = 0.5f * (RV.y1[0] + tot0), o1 = 0.5f * (RV.y1[1] + tot1);
            RV.y1[0] = tot0, RV.y1[1] = tot1;
            if (RV.filled < 65535u)
                RV.filled++;
        } else
            o0 = RV.y1[0], o1 = RV.y1[1];
        RV.ph ^= 1;
        L[s] = RV.dry * x0 + RV.wet * o0;
        R[s] = RV.dry * x1 + RV.wet * o1;
        RV.lvl = peak_decay(RV.lvl, 0.5f * (o0 + o1));
    }
    for (k = 0; k < RV_N; k++)
        RV.off[k] = off[k], RV.dec[k] = dec[k], RV.shs[k] = fm_flush(RV.shs[k]);
    RV.hamp = hamp, RV.hc = hc, RV.lpc = lpc, RV.hpc = hpc, RV.dry = dry, RV.wet = wet;
    RV.lps = fm_flush(RV.lps), RV.hps = fm_flush(RV.hps);
    RV.bz[0] = fm_flush(RV.bz[0]), RV.bz[1] = fm_flush(RV.bz[1]);
    VIEW[FXT_REVERB].a[0] = RV.lvl;
    VIEW[FXT_REVERB].a[1] = RV.ilvl;
    VIEW[FXT_REVERB].a[2] = dsec;
    VIEW[FXT_REVERB].a[3] = RV.m;
    VIEW[FXT_REVERB].a[4] = RV.pdel * (1000.0f / RV_SR);
    VIEW[FXT_REVERB].a[5] = RV.phase;
}

/* ============================================================= the entries === */
/* every continuous parameter passes a one-pole (FX_TAU) at the block rate before the effect ramps it per sample:
 * a jump in a mix, a gain, a feedback or a cutoff takes a few ms, not one block. The selectors (DISC) are not
 * smoothed. */
#define FX_TAU 0.008f
static const uint16_t DISC[FXT_N] = {0x005u, 0x001u, 0x015u, 0x005u, 0x111u, 0x003u, 0x002u, 0x002u, 0x000u};
static float PS[FXT_N][FX_NP];
static uint8_t PSF[FXT_N];

RAMC void fx_reset(int type)
{
    if (type >= 0 && type < FXT_N)
        PSF[type] = 1;
    switch (type) {
    case FXT_CHORUS:
        zero_bytes(&CH, sizeof CH);
        CH.fresh = 1;
        break;
    case FXT_COMP:
        zero_bytes(&CP, sizeof CP);
        CP.fresh = 1;
        break;
    case FXT_DELAY:
        zero_bytes(&DL, sizeof DL);
        DL.gate = 1.0f;
        DL.fresh = 1;
        break;
    case FXT_DIST:
        zero_bytes(&DS, sizeof DS);
        DS.fresh = 1;
        break;
    case FXT_EQ:
        zero_bytes(&EQ, sizeof EQ);
        EQ.fresh = 1;
        break;
    case FXT_FILTER:                       /* filt_reset runs on the first block */
        FT.mix = FT.xin = FT.xout = 0.0f;
        FT.fresh = 1;
        break;
    case FXT_FLANGER:
        zero_bytes(&FL, sizeof FL);
        FL.fresh = 1;
        break;
    case FXT_PHASER:
        zero_bytes(&PH, sizeof PH);
        PH.fresh = 1;
        break;
    case FXT_REVERB:
        zero_bytes(&RV, sizeof RV);
        RV.fresh = 1;
        break;
    default:
        break;
    }
}

RAMC void fx_run(int type, const float *p, float tempo, float *L, float *R, uint32_t n)
{
    float q[FX_NP], *ps, a;
    int k, np;
    if (n == 0u || type < 0 || type >= FXT_N)
        return;
    ps = PS[type];
    np = FXT_NP[type];
    a = blk_alpha(n, FX_TAU);
    for (k = 0; k < FX_NP; k++) {
        float x = k < np ? clamp01(p[k]) : 0.0f;
        if (PSF[type] || ((DISC[type] >> k) & 1u))
            ps[k] = x;
        else
            ps[k] += (x - ps[k]) * a;
        q[k] = ps[k];
    }
    PSF[type] = 0;
    p = q;
    switch (type) {
    case FXT_CHORUS:
        run_chorus(p, tempo, L, R, n);
        break;
    case FXT_COMP:
        run_comp(p, L, R, n);
        break;
    case FXT_DELAY:
        run_delay(p, tempo, L, R, n);
        break;
    case FXT_DIST:
        run_dist(p, L, R, n);
        break;
    case FXT_EQ:
        run_eq(p, L, R, n);
        break;
    case FXT_FILTER:
        run_filter(p, L, R, n);
        break;
    case FXT_FLANGER:
        run_flanger(p, tempo, L, R, n);
        break;
    case FXT_PHASER:
        run_phaser(p, tempo, L, R, n);
        break;
    case FXT_REVERB:
        run_reverb(p, L, R, n);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------- main loop only --- */
float fx_sync_beats(float p) { return sync_beats(p); }

const volatile fx_view_t *fx_view(int type) { return &VIEW[type >= 0 && type < FXT_N ? type : 0]; }

void fx_init(int16_t *chorus, int16_t *delay, int16_t *flanger, int16_t *reverb)
{
    int16_t *q;
    uint32_t used, k;
    int t;
    ar_ch = chorus, ar_dl = delay, ar_fl = flanger, ar_rv = reverb;
    /* the reverb's arena: pre-delay, allpasses, lines (at the largest size, plus the chorus drift) */
    q = reverb;
    RL.pd = q, q += RV_PD;
    for (k = 0; k < RV_N; k++)
        RL.ap[k] = q, q += RV_AP[k];
    used = (uint32_t)(q - reverb);
    for (k = 0; k < RV_N; k++) {
        RL.line[k] = q;
        RL.len[k] = (uint32_t)(RV_D[k] * RV_MMAX + 0.2f * RV_D[4] * RV_MMAX) + 6u;
        q += RL.len[k];
        used += RL.len[k];
    }
    (void)used;                            /* <= FXA_REVERB: checked by the host test */
    /* the crossovers: Butterworth SVFs (k = sqrt 2) at 120 Hz and 2.5 kHz */
    for (t = 0; t < 2; t++) {
        float g = fm_tanf(FM_PI * (t ? 2500.0f : 120.0f) * ISR), a1 = 1.0f / (1.0f + g * (g + 1.41421356f));
        XO[t].a1 = a1, XO[t].a2 = g * a1, XO[t].a3 = g * g * a1;
    }
    /* the reverb's decimating low pass: 2-pole Butterworth at 8 kHz */
    {
        float wc = fm_tanf(FM_PI * 8000.0f * ISR), w2 = wc * wc, nrm = 1.0f / (1.0f + 1.41421356f * wc + w2);
        RL.bq[0] = w2 * nrm, RL.bq[1] = 2.0f * w2 * nrm, RL.bq[2] = w2 * nrm;
        RL.bq[3] = 2.0f * (w2 - 1.0f) * nrm, RL.bq[4] = (1.0f - 1.41421356f * wc + w2) * nrm;
    }
    for (t = 0; t < FXT_N; t++)
        PLAT_FAR(fx_reset)(t);
}

/* the EQ's magnitude at a note: the analog prototypes the SVFs are the bilinear images of, at the prewarped
 * frequency (exact for the digital filters) */
float fx_eq_response(const float *p, float note)
{
    float hz = clampf(note_hz(note), 1.0f, 0.499f * SR), wt = fm_tanf(FM_PI * hz * ISR), mag2 = 1.0f;
    int i;
    for (i = 0; i < 3; i++) {
        float nt, k, m0, m1, m2, W, re, im, dr, di, A = db_gain(eq_db(p, i));
        eq_design(p, i, A, fm_sqrtf(A), &nt, &k, &m0, &m1, &m2);
        W = wt / svf_g(nt);
        dr = 1.0f - W * W, di = k * W;
        re = m0 * dr + m2, im = m0 * di + m1 * W;
        mag2 *= (re * re + im * im) / (dr * dr + di * di + 1e-30f);
    }
    return fm_sqrtf(mag2);
}
