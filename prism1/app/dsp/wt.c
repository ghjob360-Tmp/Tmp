/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the wavetables (main loop only: plain flash code), after Vital (Matt Tytel, GPL-3.0):
 * synthesis/producers/spectral_morph.h, synthesis/lookups/wavetable.cpp, the common/wavetable components.
 *
 * A frame is a spectrum: harmonics 1..127 as x(t) = sum_h re[h] cos(2 pi h t) + im[h] sin(2 pi h t). The recipe of
 * a factory table makes it (from a formula per harmonic, from the exact Fourier series of a piecewise linear shape,
 * or from 256 samples of a smooth shape through an FFT), scaled to a peak of 1; the spectral morph (Vital's, with
 * Vital's formulas, applied once per table) then works on it, and an inverse FFT per mip level writes the frame.
 *
 * The build, in steps of one frame (each well under the 2 ms budget; tests count the multiply-adds):
 *   all morphs but SKEW: 32 steps measure each frame's peak after the morph (kept in the frame's own slots, as a
 *     float in two int16), 1 step finds the loudest, 32 steps write the frames with the table's gain;
 *   SKEW (each harmonic reads another frame): 32 steps keep every frame's spectrum (harmonics 1..124) in the frame's
 *     mip-level 1..5 slots, 32 steps measure, 1 finds the loudest, 32 write level 0 of each frame (the spectra stay
 *     intact), 32 make levels 1..5 from level 0.
 * All the state between steps lives in the job and in dst, so jobs can interleave; the static scratch below is only
 * used inside one call (wt_spectrum shares it). */
#include "wt.h"

const uint16_t WT_OFF[WT_LEVELS] = {0u, 256u, 384u, 448u, 480u, 496u};

#ifdef WT_COUNT
uint32_t wt_ops;                         /* multiply-adds, for the host tests */
#define OPS(n) (wt_ops += (uint32_t)(n))
#else
#define OPS(n) ((void)0)
#endif

#define NH 128                           /* bins 0..127 (0 = DC, always 0) */
#define NSKEW 124                        /* harmonics kept for SKEW (248 int16 slots: levels 1..5) */
#define PWL_MAX 100

static float g_re[NH], g_im[NH];         /* the working spectrum */
static float g_tre[NH], g_tim[NH];       /* a second one: recipes that blend shapes, morphs that move bins */
static union {
    float fft[512];                      /* 256 complex */
    struct {
        float t[PWL_MAX + 1], yl[PWL_MAX + 1], yr[PWL_MAX + 1], sl[PWL_MAX + 1];
        float bc[PWL_MAX + 1], bs[PWL_MAX + 1], c[PWL_MAX + 1], s[PWL_MAX + 1];
    } p;
} g_u;
static int g_pn;
static float g_sin[256];
static int g_ready;

static void init(void)
{
    int k;
    if (g_ready)
        return;
    for (k = 0; k < 256; k++)
        g_sin[k] = fm_sinf((float)k * (FM_TWO_PI / 256.0f));
    g_ready = 1;
}

/* ------------------------------------------------------------------------------------------------ FFT --- */
/* in-place complex radix-2 FFT of n <= 256 points (interleaved re, im); inv: e^{+i}, no scaling */
static void fft(float *b, int n, int inv)
{
    int i, j = 0, len, k, bit;
    for (i = 1; i < n; i++) {
        bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = b[2 * i], ti = b[2 * i + 1];
            b[2 * i] = b[2 * j];
            b[2 * i + 1] = b[2 * j + 1];
            b[2 * j] = tr;
            b[2 * j + 1] = ti;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = 256 / len;
        for (i = 0; i < n; i += len)
            for (k = 0; k < half; k++) {
                int ix = k * step;
                float wr = g_sin[(ix + 64) & 255], wi = inv ? g_sin[ix] : -g_sin[ix];
                float *a = b + 2 * (i + k), *c = b + 2 * (i + k + half);
                float xr = c[0] * wr - c[1] * wi, xi = c[0] * wi + c[1] * wr;
                c[0] = a[0] - xr;
                c[1] = a[1] - xi;
                a[0] += xr;
                a[1] += xi;
            }
        OPS((n / 2) * 10);
    }
}

/* mip level L of g_re / g_im into g_u.fft (the samples at the even indices); returns the peak */
static float level_synth(int L)
{
    int N = 256 >> L, H = N / 2 - 1, h;
    float *b = g_u.fft, pk = 0.0f;
    for (h = 0; h < 2 * N; h++)
        b[h] = 0.0f;
    for (h = 1; h <= H; h++) {
        b[2 * h] = g_re[h];
        b[2 * h + 1] = -g_im[h];
    }
    fft(b, N, 1);
    for (h = 0; h < N; h++) {
        float a = fm_fabsf(b[2 * h]);
        if (a > pk)
            pk = a;
    }
    return pk;
}

/* g_u.fft holds 256 real samples (imaginary parts 0): to g_re / g_im */
static void td_to_spec(void)
{
    int h;
    fft(g_u.fft, 256, 0);
    g_re[0] = g_im[0] = 0.0f;
    for (h = 1; h < NH; h++) {
        g_re[h] = g_u.fft[2 * h] * (2.0f / 256.0f);
        g_im[h] = -g_u.fft[2 * h + 1] * (2.0f / 256.0f);
    }
}
#define TD(n, v) (g_u.fft[2 * (n)] = (v), g_u.fft[2 * (n) + 1] = 0.0f)

/* ------------------------------------------------------------------ exact spectra of piecewise lines --- */
/* vertices in time order from t = 0: the value just before (yl) and just after (yr) each; straight between */
static void pwl_begin(void) { g_pn = 0; }
static void pwl_pt(float t, float yl, float yr)
{
    if (g_pn >= PWL_MAX)
        return;
    g_u.p.t[g_pn] = t;
    g_u.p.yl[g_pn] = yl;
    g_u.p.yr[g_pn] = yr;
    g_pn++;
}
/* by parts, per vertex: with J = yl - yr (the value's jump) and D = the slope before - the slope after,
 * re[h] = 2 sum (J sin / w + D cos / w^2), im[h] = 2 sum (-J cos / w + D sin / w^2), w = 2 pi h, at w t_k */
static void pwl_spec(void)
{
    int n = g_pn, k, h, m = 0, slopes = 0;
    float *t = g_u.p.t, *yl = g_u.p.yl, *yr = g_u.p.yr, *sl = g_u.p.sl;
    float *bc = g_u.p.bc, *bs = g_u.p.bs, *c = g_u.p.c, *s = g_u.p.s;
    t[n] = 1.0f;                         /* the period closes on the first vertex */
    yl[n] = yl[0];
    for (k = 0; k < n; k++) {
        float dt = t[k + 1] - t[k];
        sl[k] = dt > 1e-7f ? (yl[k + 1] - yr[k]) / dt : 0.0f;
    }
    for (k = 0; k < n; k++) {            /* the vertices that matter: J -> yl, D -> yr, compacted */
        float J = yl[k] - yr[k], D = sl[k == 0 ? n - 1 : k - 1] - sl[k];
        if (J == 0.0f && fm_fabsf(D) < 1e-6f)
            continue;
        bc[m] = c[m] = fm_cosf(FM_TWO_PI * t[k]);
        bs[m] = s[m] = fm_sinf(FM_TWO_PI * t[k]);
        yl[m] = J;
        yr[m] = D;
        slopes |= D != 0.0f;
        m++;
    }
    g_re[0] = g_im[0] = 0.0f;
    for (h = 1; h < NH; h++) {
        float iw = 1.0f / (FM_TWO_PI * (float)h), js = 0.0f, jc = 0.0f, dc = 0.0f, ds = 0.0f;
        if (h > 1)
            for (k = 0; k < m; k++) {    /* e^{i 2 pi h t_k} by recurrence */
                float cc = c[k] * bc[k] - s[k] * bs[k];
                s[k] = c[k] * bs[k] + s[k] * bc[k];
                c[k] = cc;
            }
        if (slopes)
            for (k = 0; k < m; k++) {
                js += yl[k] * s[k];
                jc += yl[k] * c[k];
                dc += yr[k] * c[k];
                ds += yr[k] * s[k];
            }
        else                             /* piecewise constant: jumps only */
            for (k = 0; k < m; k++) {
                js += yl[k] * s[k];
                jc += yl[k] * c[k];
            }
        g_re[h] = 2.0f * iw * (js + dc * iw);
        g_im[h] = 2.0f * iw * (ds * iw - jc);
        OPS((slopes ? 14 : 10) * m + 8);
    }
    OPS(60 * m);
}

/* ---------------------------------------------------------------------------------- recipe helpers --- */
static void spec_clear(void)
{
    int h;
    for (h = 0; h < NH; h++)
        g_re[h] = g_im[h] = 0.0f;
}
static void spec_save(void)
{
    int h;
    for (h = 0; h < NH; h++) {
        g_tre[h] = g_re[h];
        g_tim[h] = g_im[h];
    }
}
static void spec_mix(float u)            /* g = saved + (g - saved) u */
{
    int h;
    for (h = 0; h < NH; h++) {
        g_re[h] = g_tre[h] + (g_re[h] - g_tre[h]) * u;
        g_im[h] = g_tim[h] + (g_im[h] - g_tim[h]) * u;
    }
}
/* += a sin(2 pi h t + ph) */
static void hs(int h, float a, float ph)
{
    if (h < 1 || h >= NH)
        return;
    g_re[h] += a * fm_sinf(ph);
    g_im[h] += a * fm_cosf(ph);
}
static float saw_h(int h) { return (h & 1 ? 2.0f : -2.0f) / (FM_PI * (float)h); }   /* rising ramp, sine phase */
static float schroeder(int h, float n) { return FM_PI * (float)h * (float)(h - 1) / n; }
static float powr(float x, float p) { return fm_powf(x, p); }
static float gauss(float x) { return fm_expf(-x * x); }
static float res(float x, float c, float w)      /* a resonance's magnitude */
{
    float d = (x - c) / w;
    return 1.0f / fm_sqrtf(1.0f + d * d);
}
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
static float hrnd(uint32_t a) { return (float)(hash32(a) >> 8) * (2.0f / 16777216.0f) - 1.0f; }   /* -1..1 */
static float smooth01(float x) { return x * x * (3.0f - 2.0f * x); }
static float fracf(float x) { return x - fm_floorf(x); }

/* ------------------------------------------------------------------------------- the factory tables --- */
enum { T_BASIC, T_PULSE, T_SYNCSAW, T_SYNCSQR, T_PHASESAW, T_RESOSAW, T_ADDITIVE, T_HARMSWP, T_BRIGHT, T_REED,
       T_ORGAN, T_VOWELS, T_CHOIR, T_BELL, T_GLASS, T_METAL, T_FM11, T_FM12, T_FM23, T_FMFBK, T_EPIANO, T_FOLD,
       T_SUBDRIVE, T_CZSAW, T_CZRESO, T_SKEWTRI, T_STEPS, T_DECIMATE, T_CHIP, T_RANDOM, T_SUPERSAW, T_BRASS,
       T_STRINGS, T_GROWL, T_BUZZ, T_N };
const int WT_NFACT = T_N;
const char *const WT_NAME[T_N] = {
    "BASIC", "PULSE", "SYNC SAW", "SYNC SQR", "PHASE SAW", "RESO SAW", "ADDITIVE", "HARM SWEEP", "BRIGHT", "REED",
    "ORGAN", "VOWELS", "CHOIR", "BELL", "GLASS", "METAL", "FM 1-1", "FM 1-2", "FM 2-3", "FM FBK", "EPIANO", "FOLD",
    "SUB DRIVE", "CZ SAW", "CZ RESO", "SKEW TRI", "STEPS", "DECIMATE", "CHIP", "RANDOM", "SUPERSAW", "BRASS",
    "STRINGS", "GROWL", "BUZZ"};
const char *const SM_NAME[SM_N] = {"NONE", "VOCODE", "FORM SCALE", "HARM STRCH", "INHARM STR", "SMEAR",
                                   "RANDOM AMP", "LOW PASS", "HIGH PASS", "DISPERSE", "SHEPARD", "SKEW"};

static void basic_shape(int k)           /* 0 sine, 1 triangle, 2 saw, 3 square, all sine phase */
{
    int h;
    spec_clear();
    if (k == 0) {
        g_im[1] = 1.0f;
        return;
    }
    for (h = 1; h < NH; h++) {
        float fh = (float)h;
        if (k == 1 && (h & 1))
            g_im[h] = ((h & 3) == 1 ? 8.0f : -8.0f) / (FM_PI * FM_PI * fh * fh);
        else if (k == 2)
            g_im[h] = saw_h(h);
        else if (k == 3 && (h & 1))
            g_im[h] = 4.0f / (FM_PI * fh);
    }
}
static void pulse_pwl(float w)           /* +1 on [0, w), -1 after */
{
    pwl_begin();
    pwl_pt(0.0f, -1.0f, 1.0f);
    pwl_pt(w, 1.0f, -1.0f);
    pwl_spec();
}
static void chip_shape(int k)            /* 0: 4-bit triangle (32 steps), 1..3: pulse 50, 25, 12.5 % */
{
    int i;
    if (k > 0) {
        pulse_pwl(k == 1 ? 0.5f : k == 2 ? 0.25f : 0.125f);
        return;
    }
    pwl_begin();
    for (i = 0; i < 32; i++) {
        int a = i < 16 ? 15 - i : i - 16, b = i == 0 ? 15 : (i - 1 < 16 ? 15 - (i - 1) : i - 1 - 16);
        pwl_pt((float)i / 32.0f, (float)b / 7.5f - 1.0f, (float)a / 7.5f - 1.0f);
    }
    pwl_spec();
}
static void steps_shape(int q)           /* a triangle quantised to 2q+1 levels */
{
    int k;
    float fq = (float)q;
    pwl_begin();
    pwl_pt(0.0f, -1.0f, -1.0f);
    for (k = -q; k < q; k++) {           /* rising through (k + 0.5) / q */
        float v = ((float)k + 0.5f) / fq;
        pwl_pt((v + 1.0f) * 0.25f, (float)k / fq, (float)(k + 1) / fq);
    }
    for (k = q - 1; k >= -q; k--) {      /* falling */
        float v = ((float)k + 0.5f) / fq;
        pwl_pt(0.5f + (1.0f - v) * 0.25f, (float)(k + 1) / fq, (float)k / fq);
    }
    pwl_spec();
}
static void decimate_shape(int m)        /* a sine held at m points a cycle */
{
    int k;
    float fm = (float)m;
    pwl_begin();
    for (k = 0; k < m; k++) {
        float a = fm_sinf(FM_TWO_PI * ((float)k + 0.5f) / fm);
        float b = fm_sinf(FM_TWO_PI * ((float)(k == 0 ? m - 1 : k - 1) + 0.5f) / fm);
        pwl_pt((float)k / fm, b, a);
    }
    pwl_spec();
}
static void metal_shape(int k)           /* a 15-harmonic square ring-modulated by harmonic k */
{
    int m;
    spec_clear();
    for (m = 1; m <= 15; m += 2) {
        float a = 2.0f / (FM_PI * (float)m);
        int d = k > m ? k - m : m - k;
        if (d > 0 && d < NH)
            g_re[d] += a;
        if (k + m < NH)
            g_re[k + m] -= a;
    }
}
typedef struct { float f[5], a[5], b[5]; } vowel_t;
static const vowel_t VOW_T[5] = {       /* tenor: a e i o u (Hz, dB, Hz) */
    {{650, 1080, 2650, 2900, 3250}, {0, -6, -7, -8, -22}, {80, 90, 120, 130, 140}},
    {{400, 1700, 2600, 3200, 3580}, {0, -14, -12, -14, -20}, {70, 80, 100, 120, 120}},
    {{290, 1870, 2800, 3250, 3540}, {0, -15, -18, -20, -30}, {40, 90, 100, 120, 120}},
    {{400, 800, 2600, 2800, 3000}, {0, -10, -12, -12, -26}, {40, 80, 100, 120, 120}},
    {{350, 600, 2700, 2900, 3300}, {0, -20, -17, -14, -26}, {40, 60, 100, 120, 120}}};
static const vowel_t VOW_S[4] = {       /* soprano: u o a e */
    {{325, 700, 2700, 3800, 4950}, {0, -16, -35, -40, -60}, {50, 60, 170, 180, 200}},
    {{450, 800, 2830, 3800, 4950}, {0, -11, -22, -22, -50}, {70, 80, 100, 130, 135}},
    {{800, 1150, 2900, 3900, 4950}, {0, -6, -32, -20, -50}, {80, 90, 120, 130, 140}},
    {{350, 2000, 2800, 3600, 4950}, {0, -20, -15, -40, -56}, {60, 100, 120, 150, 200}}};
static void vowel(const vowel_t *tab, int n, float t, float f0, float bwmul, float tilt, float jitter)
{
    float u = t * (float)(n - 1), fr;
    int i = (int)u, h, k;
    vowel_t v;
    if (i > n - 2)
        i = n - 2;
    fr = u - (float)i;
    for (k = 0; k < 5; k++) {
        v.f[k] = tab[i].f[k] + (tab[i + 1].f[k] - tab[i].f[k]) * fr;
        v.a[k] = db_gain(tab[i].a[k] + (tab[i + 1].a[k] - tab[i].a[k]) * fr);
        v.b[k] = fm_maxf(f0, (tab[i].b[k] + (tab[i + 1].b[k] - tab[i].b[k]) * fr) * bwmul);
    }
    spec_clear();
    for (h = 1; h < NH; h++) {
        float hz = f0 * (float)h, a = 0.0f;
        if (hz > 9000.0f)
            break;
        for (k = 0; k < 5; k++)
            a += v.a[k] * res(hz, v.f[k], v.b[k] * 0.5f);
        a *= powr((float)h, -tilt) * (1.0f + jitter * hrnd(7000u + (uint32_t)h));
        hs(h, a, schroeder(h, 48.0f));
        OPS(220);
    }
}

/* the recipe of table tab, frame f: into g_re / g_im */
static void recipe(int tab, int f)
{
    float t = (float)f / (float)(WT_FRAMES - 1), u, fr;
    int h, n, k;
    switch (tab) {
    case T_BASIC:                        /* sine -> triangle -> saw -> square */
        u = t * 3.0f;
        k = (int)u;
        if (k > 2)
            k = 2;
        basic_shape(k);
        spec_save();
        basic_shape(k + 1);
        spec_mix(u - (float)k);
        break;
    case T_PULSE:                        /* pulse width 50 % -> 3 % */
        pulse_pwl(0.5f * powr(0.06f, t));
        break;
    case T_SYNCSAW: {                    /* a saw hard-synced at 1 .. 8 times the master */
        float r = fm_exp2f(3.0f * t), fl = r - fm_floorf(r);
        int m = (int)r;
        if (fl < 1e-5f)
            fl = 1.0f;
        else
            m++;
        pwl_begin();
        pwl_pt(0.0f, 2.0f * fl - 1.0f, -1.0f);
        for (k = 1; k < m; k++)
            pwl_pt((float)k / r, 1.0f, -1.0f);
        pwl_spec();
        break;
    }
    case T_SYNCSQR: {                    /* a square synced at 1 .. 6 */
        float r = fm_exp2f(2.585f * t), fl = r - fm_floorf(r), last;
        if (fl < 1e-5f)
            fl = 1.0f;
        last = fl <= 0.5f ? 1.0f : -1.0f;
        pwl_begin();
        pwl_pt(0.0f, last, 1.0f);
        for (k = 0;; k++) {
            float a = ((float)k + 0.5f) / r, b = (float)(k + 1) / r;
            if (a < 0.99999f)
                pwl_pt(a, 1.0f, -1.0f);
            if (b < 0.99999f)
                pwl_pt(b, -1.0f, 1.0f);
            else
                break;
        }
        pwl_spec();
        break;
    }
    case T_PHASESAW:                     /* a saw plus itself delayed 0 .. half a cycle (to the saw an octave up) */
        spec_clear();
        for (h = 1; h < NH; h++) {
            float d = FM_TWO_PI * (float)h * 0.5f * t, a = saw_h(h);
            g_im[h] = a * (1.0f + fm_cosf(d));
            g_re[h] = a * fm_sinf(d);
        }
        break;
    case T_RESOSAW: {                    /* a saw through a resonant low pass, harmonic 1.2 -> 95 */
        float c = 1.2f * fm_exp2f(6.3f * t);
        spec_clear();
        for (h = 1; h < NH; h++) {
            float r = (float)h / c, d = 1.0f - r * r;
            g_im[h] = saw_h(h) / fm_sqrtf(d * d + r * r * 0.04f);
            OPS(30);
        }
        break;
    }
    case T_ADDITIVE:                     /* harmonics added one by one: 1 -> 40 */
        u = 1.0f + 39.0f * t;
        n = (int)u;
        spec_clear();
        for (h = 1; h <= n + 1 && h < NH; h++) {
            float a = 1.0f / fm_sqrtf((float)h);
            if (h > n)
                a *= u - (float)n;
            hs(h, a, schroeder(h, 40.0f));
        }
        break;
    case T_HARMSWP: {                    /* a narrow band of harmonics sweeping 1 -> 47 over the fundamental */
        float c = 1.0f + 46.0f * t * fm_sqrtf(t);
        spec_clear();
        for (h = 1; h < NH; h++)
            g_im[h] = gauss(((float)h - c) / 1.3f) + (h == 1 ? 0.35f : 0.0f);
        break;
    }
    case T_BRIGHT: {                     /* 1/h^p, p 2.6 (dark) -> 0.6 (bright), low crest factor */
        float p = 2.6f - 2.0f * t;
        spec_clear();
        for (h = 1; h < NH; h++) {
            hs(h, powr((float)h, -p), schroeder(h, 127.0f));
            OPS(95);
        }
        break;
    }
    case T_REED: {                       /* clarinet (odd) -> oboe (all, a formant rising) */
        float e = 0.08f + 0.7f * t * t, hf = 3.0f + 10.0f * t;
        spec_clear();
        for (h = 1; h < NH; h++) {
            float a = ((h & 1) ? 1.0f : e) * powr((float)h, -0.7f) * (0.3f + res((float)h, hf, 2.0f + 2.0f * t));
            hs(h, a, schroeder(h, 24.0f));
            OPS(120);
        }
        break;
    }
    case T_ORGAN: {                      /* drawbar registrations (16' is the fundamental) */
        static const uint8_t REG[6][9] = {{0, 0, 8, 0, 0, 0, 0, 0, 0}, {0, 0, 6, 8, 0, 2, 0, 0, 0},
                                           {8, 8, 8, 0, 0, 0, 0, 0, 0}, {8, 6, 8, 6, 0, 4, 0, 0, 6},
                                           {8, 8, 8, 8, 4, 6, 0, 0, 8}, {8, 8, 8, 8, 8, 8, 8, 8, 8}};
        static const uint8_t HARM[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
        u = t * 5.0f;
        k = (int)u;
        if (k > 4)
            k = 4;
        fr = u - (float)k;
        spec_clear();
        for (n = 0; n < 9; n++) {
            float a0 = REG[k][n] ? db_gain(-3.0f * (float)(8 - REG[k][n])) : 0.0f;
            float a1 = REG[k + 1][n] ? db_gain(-3.0f * (float)(8 - REG[k + 1][n])) : 0.0f;
            g_im[HARM[n]] += a0 + (a1 - a0) * fr;
        }
        break;
    }
    case T_VOWELS:                       /* tenor a -> e -> i -> o -> u */
        vowel(VOW_T, 5, t, 110.0f, 1.0f, 1.0f, 0.0f);
        break;
    case T_CHOIR:                        /* soprano choir u -> o -> a -> e: wide formants, jittered harmonics */
        vowel(VOW_S, 4, t, 220.0f, 1.8f, 1.3f, 0.25f);
        break;
    case T_BELL: {                       /* bell partials (hum 2, prime 4, tierce 5, quint 6, nominal 8, ...): soft -> struck */
        static const uint8_t PH[15] = {2, 4, 5, 6, 8, 10, 11, 13, 16, 19, 21, 27, 33, 41, 53};
        static const float PA[15] = {0.6f, 1.0f, 0.7f, 0.4f, 0.9f, 0.35f, 0.5f, 0.3f, 0.35f, 0.2f, 0.25f, 0.15f,
                                     0.12f, 0.08f, 0.05f};
        spec_clear();
        for (n = 0; n < 15; n++)
            hs(PH[n], PA[n] * fm_expf(-(float)n * (1.0f - t) * 0.45f), FM_TWO_PI * hrnd(100u + (uint32_t)n));
        break;
    }
    case T_GLASS: {                      /* a free bar's partials (1 : 2.76 : 5.4 : 8.9 ...), pure -> sparkling */
        static const uint8_t PH[9] = {1, 2, 6, 11, 18, 27, 37, 49, 62};
        static const float PA[9] = {0.25f, 1.0f, 0.7f, 0.5f, 0.4f, 0.3f, 0.22f, 0.15f, 0.1f};
        float br = 3.0f + 60.0f * t * t;
        spec_clear();
        for (n = 0; n < 9; n++) {
            float a = PA[n] * fm_expf(-(float)PH[n] / br);
            hs(PH[n], a, FM_TWO_PI * hrnd(200u + (uint32_t)n));
            hs(PH[n] + 1, 0.3f * t * a, FM_TWO_PI * hrnd(300u + (uint32_t)n));
        }
        break;
    }
    case T_METAL:                        /* square x sine ring modulation, the sine at harmonic 3 -> 23 */
        u = 3.0f + 20.0f * t;
        k = (int)u;
        if (k > 22)
            k = 22;
        metal_shape(k);
        spec_save();
        metal_shape(k + 1);
        spec_mix(u - (float)k);
        break;
    case T_FM11:
    case T_FM12:
    case T_FM23: {                       /* two-operator FM, index 0 -> 6 / 5 / 4 */
        float ci = tab == T_FM23 ? 2.0f : 1.0f, mi = tab == T_FM11 ? 1.0f : tab == T_FM12 ? 2.0f : 3.0f;
        float ix = (tab == T_FM11 ? 6.0f : tab == T_FM12 ? 5.0f : 4.0f) * t;
        for (n = 0; n < 256; n++) {
            float th = FM_TWO_PI * (float)n / 256.0f;
            TD(n, fm_sinf(ci * th + ix * fm_sinf(mi * th)));
        }
        OPS(256 * 60);
        td_to_spec();
        break;
    }
    case T_FMFBK: {                      /* feedback FM, y = sin(th + b y), b 0 -> 0.98 (sine -> saw) */
        float b = 0.98f * t, ph = 0.0f;
        for (n = 0; n < 256; n++) {      /* Kepler: phi = th + b sin(phi); Newton from the last sample's phi */
            float th = FM_TWO_PI * (float)n / 256.0f;
            ph = n ? ph + (FM_TWO_PI / 256.0f) / (1.0f - b * fm_cosf(ph)) : 0.0f;
            for (k = 0; k < 5; k++) {
                float d = 1.0f - b * fm_cosf(ph);
                ph -= (ph - b * fm_sinf(ph) - th) / (d < 0.02f ? 0.02f : d);
            }
            TD(n, fm_sinf(ph));
        }
        OPS(256 * 6 * 60);
        td_to_spec();
        break;
    }
    case T_EPIANO: {                     /* FM e-piano: body (1:1) and tine (1:14), soft -> hard */
        float i1 = 0.3f + 1.5f * t, i2 = 1.6f * t * t;
        for (n = 0; n < 256; n++) {
            float th = FM_TWO_PI * (float)n / 256.0f;
            TD(n, fm_sinf(th + i1 * fm_sinf(th)) + 0.5f * fm_sinf(th + i2 * fm_sinf(14.0f * th)));
        }
        OPS(256 * 115);
        td_to_spec();
        break;
    }
    case T_FOLD: {                       /* Vital's wave folder on a sine: sin(boost asin(sin)), boost 1 -> 9 */
        float g = 1.0f + 8.0f * t;
        for (n = 0; n < 256; n++) {
            float p = (float)n / 256.0f, a;    /* asin(sin 2 pi p): a triangle in [-pi/2, pi/2] */
            a = p < 0.25f ? p : p < 0.75f ? 0.5f - p : p - 1.0f;
            TD(n, fm_sinf(g * FM_TWO_PI * a));
        }
        OPS(256 * 35);
        td_to_spec();
        break;
    }
    case T_SUBDRIVE: {                   /* a sine driven into tanh, 1 -> 10, with a little even harmonic */
        float g = 1.0f + 9.0f * t * t, nrm = 1.0f / fm_tanhf(g);
        for (n = 0; n < 256; n++) {
            float th = FM_TWO_PI * (float)n / 256.0f;
            TD(n, fm_tanhf(g * (fm_sinf(th) + 0.2f * t * fm_sinf(2.0f * th))) * nrm);
        }
        OPS(256 * 95);
        td_to_spec();
        break;
    }
    case T_CZSAW: {                      /* Casio CZ phase distortion: cosine -> saw (knee 0.5 -> 0.02) */
        float d = 0.5f * powr(0.04f, fm_sqrtf(t));
        for (n = 0; n < 256; n++) {
            float p = (float)n / 256.0f;
            float ph = p < d ? 0.5f * p / d : 0.5f + 0.5f * (p - d) / (1.0f - d);
            TD(n, -fm_cosf(FM_TWO_PI * ph));
        }
        OPS(256 * 45);
        td_to_spec();
        break;
    }
    case T_CZRESO: {                     /* CZ resonance: (1 - p)(1 - cos(2 pi r p)), r 1 -> 16 */
        float r = 1.0f + 15.0f * powr(t, 1.2f);
        for (n = 0; n < 256; n++) {
            float p = (float)n / 256.0f;
            TD(n, (1.0f - p) * (1.0f - fm_cosf(FM_TWO_PI * r * p)));
        }
        OPS(256 * 40);
        td_to_spec();
        break;
    }
    case T_SKEWTRI: {                    /* triangle -> saw: the peak moves 0.5 -> 0.02 */
        float r = 0.5f * powr(0.04f, t);
        pwl_begin();
        pwl_pt(0.0f, -1.0f, -1.0f);
        pwl_pt(r, 1.0f, 1.0f);
        pwl_spec();
        break;
    }
    case T_STEPS:                        /* a triangle quantised: 16 -> 1 steps a half */
    case T_DECIMATE:                     /* a sine sampled and held: 48 -> 3 points a cycle */
        if (tab == T_STEPS)
            u = powr(16.0f, 1.0f - t);
        else
            u = 48.0f * powr(3.0f / 48.0f, t);
        k = (int)u;
        fr = u - (float)k;
        if (tab == T_STEPS) {
            steps_shape(k);
            if (fr > 1e-4f) {
                spec_save();
                steps_shape(k + 1);
                spec_mix(fr);
            }
        } else {
            decimate_shape(k);
            if (fr > 1e-4f) {
                spec_save();
                decimate_shape(k + 1);
                spec_mix(fr);
            }
        }
        break;
    case T_CHIP:                         /* 4-bit triangle -> pulse 50 % -> 25 % -> 12.5 % */
        u = t * 3.0f;
        k = (int)u;
        if (k > 2)
            k = 2;
        chip_shape(k);
        spec_save();
        chip_shape(k + 1);
        spec_mix(u - (float)k);
        break;
    case T_RANDOM: {                     /* six seeded random spectra, morphed in turn */
        u = t * 5.0f;
        k = (int)u;
        if (k > 4)
            k = 4;
        fr = smooth01(u - (float)k);
        spec_clear();
        for (h = 1; h < NH; h++) {
            float r0 = hrnd(1000u * (uint32_t)k + (uint32_t)h), r1 = hrnd(1000u * (uint32_t)(k + 1) + (uint32_t)h);
            float a0 = (0.1f + 0.9f * r0 * r0) * (hrnd(77000u + 1000u * (uint32_t)k + (uint32_t)h) < -0.3f ? 0.1f : 1.0f);
            float a1 = (0.1f + 0.9f * r1 * r1) * (hrnd(77000u + 1000u * (uint32_t)(k + 1) + (uint32_t)h) < -0.3f ? 0.1f : 1.0f);
            hs(h, (a0 + (a1 - a0) * fr) * powr((float)h, -0.75f), FM_TWO_PI * hrnd(999u + (uint32_t)h));
            OPS(130);
        }
        break;
    }
    case T_SUPERSAW: {                   /* seven detuned saws: frame = time, so a frame sweep plays the beating */
        static const float D[7] = {-0.48f, -0.31f, -0.14f, 0.0f, 0.15f, 0.33f, 0.5f};
        spec_clear();
        for (k = 0; k < 7; k++) {
            float p0 = k == 3 ? 0.0f : hrnd(500u + (uint32_t)k), off = p0 + D[k] * t, am = k == 3 ? 1.0f : 0.7f;
            for (h = 1; h < NH; h++) {
                float d = FM_TWO_PI * fracf((float)h * off);
                hs(h, am * saw_h(h), d);
            }
            OPS(127 * 65);
        }
        break;
    }
    case T_BRASS: {                      /* brightness rising with loudness, a formant near harmonic 11 */
        float b = 1.5f + 18.0f * t * t;
        spec_clear();
        for (h = 1; h < NH; h++)
            g_im[h] = (h & 1 ? 1.0f : -1.0f) * fm_expf(-(float)(h - 1) / b) * (1.0f + 1.5f * t * res((float)h, 11.0f, 4.0f));
        OPS(127 * 60);
        break;
    }
    case T_STRINGS: {                    /* bowed string: bow position 1/2 -> 1/16, body resonances, brighter */
        float beta = 0.5f - 0.44f * t, br = 20.0f + 80.0f * t;
        spec_clear();
        for (h = 1; h < NH; h++) {
            float a = (0.1f + fm_fabsf(fm_sinf(FM_PI * (float)h * beta))) *
                      (1.0f + 0.8f * res((float)h, 5.5f, 1.5f) + 0.5f * res((float)h, 14.0f, 3.0f)) *
                      fm_expf(-(float)h / br);
            g_im[h] = saw_h(h) * a;
            OPS(110);
        }
        break;
    }
    case T_GROWL: {                      /* talking bass: two formants on a saw, F1 up and back, F2 rising */
        float f1 = 3.0f + 9.0f * fm_sinf(FM_PI * t), f2 = 8.0f + 30.0f * t;
        spec_clear();
        for (h = 1; h < NH; h++)
            g_im[h] = saw_h(h) * (0.25f + 4.0f * res((float)h, f1, 1.2f) + 3.0f * res((float)h, f2, 2.5f)) *
                      fm_expf(-(float)h / 50.0f);
        OPS(127 * 85);
        break;
    }
    case T_BUZZ: {                       /* a band-limited impulse narrowing: sine -> buzz */
        float s = 1.2f * powr(75.0f, t);
        spec_clear();
        for (h = 1; h < NH; h++)
            g_re[h] = gauss((float)h / s);
        OPS(127 * 35);
        break;
    }
    default:
        spec_clear();
        g_im[1] = 1.0f;
    }
    g_re[0] = g_im[0] = 0.0f;
    OPS(127 * 60);                       /* the per-harmonic formulas not counted above (sin / exp per harmonic) */
}

/* the recipe at a peak of 1 */
static void frame_recipe(int tab, int f)
{
    float pk;
    recipe(tab, f);
    pk = level_synth(0);
    if (pk > 1e-9f) {
        int h;
        float g = 1.0f / pk;
        for (h = 0; h < NH; h++) {
            g_re[h] *= g;
            g_im[h] *= g;
        }
    }
}

/* --------------------------------------------------------------------- the spectral morphs (Vital's) --- */
static float amp_of(float re, float im) { return fm_sqrtf(re * re + im * im); }
static void tmp_to_spec(void)
{
    int h;
    for (h = 0; h < NH; h++) {
        g_re[h] = g_tre[h];
        g_im[h] = g_tim[h];
    }
}
static void tmp_clear(void)
{
    int h;
    for (h = 0; h < NH; h++)
        g_tre[h] = g_tim[h] = 0.0f;
}
/* add a spectrum value (re, im) at fractional bin x split over two bins (Vital's harmonic / inharmonic scale) */
static void tmp_split(float x, float re, float im)
{
    int d = (int)x;
    float t = x - (float)d;
    if (d >= NH)
        return;
    g_tre[d] += (1.0f - t) * re;
    g_tim[d] += (1.0f - t) * im;
    if (d + 1 < NH) {
        g_tre[d + 1] += t * re;
        g_tim[d + 1] += t * im;
    }
}
/* a cheap atan2 (in turns, -0.5..0.5): main loop only */
static float atan2_turns(float y, float x)
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a, r, s;
    if (ax < 1e-30f && ay < 1e-30f)
        return 0.0f;
    a = ax > ay ? ay / ax : ax / ay;
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;   /* atan on [0, 1], 1e-5 */
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = FM_PI - r;
    if (y < 0.0f)
        r = -r;
    return r * (1.0f / FM_TWO_PI);
}

static void morph(int m, float a)
{
    int h;
    OPS(127 * 120);
    switch (m) {
    case SM_VOCODE:                      /* evenOddVocodeMorph: formants up to an octave up (Vital 0.5 .. 1) */
    case SM_FORMSCALE: {                 /* the same, formants down two octaves (Vital 0.5 .. 0) */
        float s = m == SM_VOCODE ? fm_exp2f(-a) : fm_exp2f(2.0f * a);
        tmp_clear();
        for (h = 1; h < NH; h++) {
            float sh = fm_maxf(1.0f, (float)h * s), t;
            int is = (int)sh;
            is -= (h + is) & 1;          /* same parity as h */
            t = (sh - (float)is) * 0.5f;
            if (is + 2 >= NH) {
                if (is >= NH)
                    break;
                g_tre[h] = s * g_re[is] * (1.0f - t);
                g_tim[h] = s * g_im[is] * (1.0f - t);
                continue;
            }
            g_tre[h] = s * (g_re[is] + (g_re[is + 2] - g_re[is]) * t);
            g_tim[h] = s * (g_im[is] + (g_im[is + 2] - g_im[is]) * t);
        }
        g_tre[0] = g_tim[0] = 0.0f;
        tmp_to_spec();
        break;
    }
    case SM_HARMSTRETCH: {               /* harmonicScaleMorph: harmonic h -> (h - 1) s + 1, s 1 -> 16 */
        float s = fm_exp2f(4.0f * a);
        tmp_clear();
        for (h = 1; h < NH; h++) {
            float x = fm_maxf(1.0f, (float)(h - 1) * s + 1.0f);
            if (x >= (float)NH)
                break;
            tmp_split(x, g_re[h], g_im[h]);
        }
        tmp_to_spec();
        break;
    }
    case SM_INHARMSTRETCH: {             /* inharmonicScaleMorph: h -> (h - 1) mult^(log2 h / 10) + 1, mult 1 -> 4096 */
        float lm = 12.0f * a;            /* log2 mult */
        tmp_clear();
        for (h = 1; h < NH; h++) {
            float x = fm_maxf(1.0f, fm_exp2f(lm * fm_log2f((float)h) * 0.1f) * (float)(h - 1) + 1.0f);
            if (x >= (float)NH)
                break;
            tmp_split(x, g_re[h], g_im[h]);
        }
        tmp_to_spec();
        break;
    }
    case SM_SMEAR: {                     /* smearMorph (per bin pair, as Vital's poly code): energy carried up */
        float s = (1.0f - (1.0f - a) * (1.0f - a) * (1.0f - a)) * 0.95f, carry[2];
        carry[0] = 0.0f;
        carry[1] = amp_of(g_re[1], g_im[1]) * (1.0f - s);
        g_re[1] *= 1.0f - s;
        g_im[1] *= 1.0f - s;
        for (h = 2; h < NH; h++) {
            int p = h >> 1;
            float am = amp_of(g_re[h], g_im[h]), o = am + (carry[h & 1] - am) * s;
            if (am > 1e-20f) {
                g_re[h] *= o / am;
                g_im[h] *= o / am;
            } else {                     /* no phase to keep: sine phase */
                g_re[h] = 0.0f;
                g_im[h] = o;
            }
            carry[h & 1] = o * ((float)p + 0.25f) / (float)p;
        }
        break;
    }
    case SM_RANDOMAMP: {                 /* randomAmplitudeMorph: 16 random stages, shift 0 -> 15 */
        float sh = a * 15.0f, t, mult = 1.0f + sh, center = 1.0f - sh;
        int ix = (int)sh;
        if (ix > 14)
            ix = 14;
        t = sh - (float)ix;
        for (h = 1; h < NH; h++) {
            float r1 = hrnd(0x4000u + (uint32_t)(ix * NH + h)), r2 = hrnd(0x4000u + (uint32_t)((ix + 1) * NH + h));
            float v1 = mult * fm_maxf(center - sh * r1, 0.0f), v2 = mult * fm_maxf(center - sh * r2, 0.0f);
            float g = v1 + (v2 - v1) * t;
            g_re[h] *= g;
            g_im[h] *= g;
        }
        break;
    }
    case SM_LOWPASS: {                   /* lowPassMorph: cutoff 2^(7 (1 - a)) + 1 harmonics */
        float c = fm_exp2f(7.0f * (1.0f - a)) + 1.0f;
        for (h = 1; h < NH; h++) {
            float g = fm_clampf(c - (float)h, 0.0f, 1.0f);
            g_re[h] *= g;
            g_im[h] *= g;
        }
        break;
    }
    case SM_HIGHPASS: {                  /* highPassMorph: cutoff 2^(6.5 a) harmonics */
        float c = fm_exp2f(6.5f * a);
        for (h = 1; h < NH; h++) {
            float g = fm_clampf((float)h + 1.0f - c, 0.0f, 1.0f);
            g_re[h] *= g;
            g_im[h] *= g;
        }
        break;
    }
    case SM_DISPERSE: {                  /* phaseMorph: rotate h by v ((h - 24)^2 - 23^2), v 0 -> -0.05 */
        float v = -0.05f * a;
        for (h = 1; h < NH; h++) {
            float d = ((float)h - 24.0f) * ((float)h - 24.0f) * v - 529.0f * v;
            float c = fm_cosf(d), s = fm_sinf(d), re = g_re[h], im = g_im[h];
            g_re[h] = re * c - im * s;
            g_im[h] = im * c + re * s;
        }
        break;
    }
    case SM_SHEPARD: {                   /* shepardMorph: toward the spectrum an octave up (odd bins fade) */
        float s = a;
        tmp_clear();
        for (h = 1; h < NH; h++) {
            if (h & 1) {
                g_tre[h] = g_re[h] * (1.0f - s);
                g_tim[h] = g_im[h] * (1.0f - s);
            } else {
                int k = h >> 1;
                float fa = amp_of(g_re[h], g_im[h]), sa = amp_of(g_re[k], g_im[k]);
                float am = fa + (sa - fa) * s, ratio = (fa + 0.001f) / (sa + 0.001f);
                if (ratio < 2.0f && ratio > 0.5f) {
                    /* our (re, im) is (sin, cos) of the sine phase: the angle of (im, re) */
                    float fp = atan2_turns(g_re[h], g_im[h]), sp = atan2_turns(g_re[k], g_im[k]);
                    float d = sp - fp, p;
                    d -= fm_floorf(d + 0.5f);
                    p = FM_TWO_PI * (fp + d * s);
                    g_tre[h] = am * fm_sinf(p);
                    g_tim[h] = am * fm_cosf(p);
                } else {
                    float fr = fa > 1e-20f ? 1.0f / fa : 0.0f, sr = sa > 1e-20f ? 1.0f / sa : 0.0f;
                    float nr = g_re[h] * fr + (g_re[k] * sr - g_re[h] * fr) * s;
                    float ni = g_im[h] * fr + (g_im[k] * sr - g_im[h] * fr) * s;
                    g_tre[h] = am * nr;
                    g_tim[h] = am * ni;
                }
            }
        }
        tmp_to_spec();
        break;
    }
    default:
        break;
    }
    g_re[0] = g_im[0] = 0.0f;
}

/* SKEW (wavetableSkewMorph): harmonic h of frame position bt (0..1) reads the frame at a triangle fold of
 * bt + shift log2(h) / 11; frm(i, h, &re, &im) gives frame i's harmonic */
static float skew_index(float bt, float shift, int h)
{
    float x = (bt + shift * fm_log2f((float)h) * (1.0f / 11.0f)) * 0.5f;
    float bv = 1.0f - fracf(x) * 2.0f;
    return (1.0f - fm_fabsf(bv)) * (float)(WT_FRAMES - 1);
}
static float skew_shift(float a) { return a * a * 16.0f; }
static void skew_spec(const wt_t *d, float bt, float shift)
{
    int h;
    spec_clear();
    for (h = 1; h <= NSKEW; h++) {
        float x = skew_index(bt, shift, h), t, fa, ta, am, fr, tr, fi, ti;
        int from = (int)x;
        const int16_t *ps, *pt;
        if (from > WT_FRAMES - 2)
            from = WT_FRAMES - 2;
        t = fm_minf(1.0f, x - (float)from);
        ps = d->d[from] + 256 + 2 * (h - 1);
        pt = d->d[from + 1] + 256 + 2 * (h - 1);
        fr = (float)ps[0] * (1.0f / 16383.0f);
        fi = (float)ps[1] * (1.0f / 16383.0f);
        tr = (float)pt[0] * (1.0f / 16383.0f);
        ti = (float)pt[1] * (1.0f / 16383.0f);
        fa = amp_of(fr, fi);
        ta = amp_of(tr, ti);
        am = fa + (ta - fa) * t;         /* Vital: amplitudes and unit phasors interpolated apart */
        if (fa > 1e-12f) {
            fr /= fa;
            fi /= fa;
        }
        if (ta > 1e-12f) {
            tr /= ta;
            ti /= ta;
        }
        g_re[h] = am * (fr + (tr - fr) * t);
        g_im[h] = am * (fi + (ti - fi) * t);
        OPS(90);
    }
}

/* ----------------------------------------------------------------------------------------- the build --- */
static void put_f(int16_t *d, float v)
{
    fm_bits_t b;
    b.f = v;
    d[0] = (int16_t)(uint16_t)(b.u & 0xFFFFu);
    d[1] = (int16_t)(uint16_t)(b.u >> 16);
}
static float get_f(const int16_t *d)
{
    fm_bits_t b;
    b.u = (uint32_t)(uint16_t)d[0] | ((uint32_t)(uint16_t)d[1] << 16);
    return b.f;
}
static int16_t q16(float v)
{
    v *= 32767.0f;
    v += v >= 0.0f ? 0.5f : -0.5f;
    if (v > 32767.0f)
        v = 32767.0f;
    if (v < -32767.0f)
        v = -32767.0f;
    return (int16_t)(int32_t)v;
}
static float peak_all(void)
{
    int L;
    float pk = 0.0f;
    for (L = 0; L < WT_LEVELS; L++)
        pk = fm_maxf(pk, level_synth(L));
    return pk;
}
static void write_levels(int16_t *d, int from, int to, float gain)
{
    int L, n;
    for (L = from; L <= to; L++) {
        int N = 256 >> L;
        level_synth(L);
        for (n = 0; n < N; n++)
            d[WT_OFF[L] + n] = q16(g_u.fft[2 * n] * gain);
    }
}
/* the table's gain: the loudest frame to 0.9. A morph that leaves less than -60 dB of the recipe (which peaks at
 * 1) leaves silence (Vital's high pass / random amplitudes can remove everything): never amplified rounding */
static float table_gain(const int16_t *d)
{
    float mx = get_f(d + 2);
    return mx > 1e-3f ? 0.9f / mx : 0.0f;
}
static void put_max(wt_t *dst)
{
    int f;
    float mx = 0.0f;
    for (f = 0; f < WT_FRAMES; f++)
        mx = fm_maxf(mx, get_f(dst->d[f]));
    for (f = 0; f < WT_FRAMES; f++)
        put_f(dst->d[f] + 2, mx);
}

void wt_build_begin(wt_job_t *j, wt_t *dst, int table, int morph_, float amount)
{
    j->dst = dst;
    j->table = table < 0 ? 0 : table >= WT_NFACT ? WT_NFACT - 1 : table;
    j->morph = morph_ < 0 || morph_ >= SM_N ? SM_NONE : morph_;
    j->amount = fm_clampf(amount, 0.0f, 1.0f);
    if (!(j->amount > 0.0f))
        j->morph = SM_NONE;              /* amount 0 is no morph, exactly */
    j->step = 0;
}

int wt_build_step(wt_job_t *j)
{
    const int F = WT_FRAMES;
    int s = j->step;
    wt_t *d = j->dst;
    init();
    if (!d)
        return 1;
    if (j->morph != SM_SKEW) {
        if (s < F) {                     /* measure */
            frame_recipe(j->table, s);
            morph(j->morph, j->amount);
            put_f(d->d[s], peak_all());
        } else if (s == F) {
            put_max(d);
        } else if (s <= 2 * F) {         /* write */
            int f = s - F - 1;
            float g = table_gain(d->d[f]);
            frame_recipe(j->table, f);
            morph(j->morph, j->amount);
            write_levels(d->d[f], 0, WT_LEVELS - 1, g);
        }
        j->step = ++s;
        return s > 2 * F;
    }
    if (s < F) {                         /* keep the spectra (peak 1: |c_h| <= 2) */
        int h;
        frame_recipe(j->table, s);
        for (h = 1; h <= NSKEW; h++) {
            d->d[s][256 + 2 * (h - 1)] = (int16_t)(int32_t)fm_clampf(g_re[h] * 16383.0f, -32767.0f, 32767.0f);
            d->d[s][257 + 2 * (h - 1)] = (int16_t)(int32_t)fm_clampf(g_im[h] * 16383.0f, -32767.0f, 32767.0f);
        }
    } else if (s < 2 * F) {              /* measure */
        int g = s - F;
        skew_spec(d, (float)g / (float)(F - 1), skew_shift(j->amount));
        put_f(d->d[g], peak_all());
    } else if (s == 2 * F) {
        put_max(d);
    } else if (s <= 3 * F) {             /* level 0 */
        int g = s - 2 * F - 1;
        float gain = table_gain(d->d[g]);
        skew_spec(d, (float)g / (float)(F - 1), skew_shift(j->amount));
        write_levels(d->d[g], 0, 0, gain);   /* level 0 only: 1..5 still hold the spectra */
    } else if (s <= 4 * F) {             /* levels 1..5 from level 0 */
        int g = s - 3 * F - 1, n, h;
        for (n = 0; n < 256; n++)
            TD(n, (float)d->d[g][n] * (1.0f / 32767.0f));
        td_to_spec();
        for (h = 64; h < NH; h++)
            g_re[h] = g_im[h] = 0.0f;
        write_levels(d->d[g], 1, WT_LEVELS - 1, 1.0f);
    }
    j->step = ++s;
    return s > 4 * F;
}

/* ------------------------------------------------------------------------------ peek and spectrum --- */
float wt_peek(const wt_t *t, float fp, float ph)
{
    int i0, i1, k0, k1;
    float fr, x, f, a, b;
    if (!t)
        return 0.0f;
    fp = fm_clampf(fp, 0.0f, (float)(WT_FRAMES - 1));
    i0 = (int)fp;
    i1 = i0 < WT_FRAMES - 1 ? i0 + 1 : i0;
    fr = fp - (float)i0;
    x = fracf(ph) * 256.0f;
    k0 = (int)x & 255;
    k1 = (k0 + 1) & 255;
    f = x - (float)(int)x;
    a = (float)t->d[i0][k0] + ((float)t->d[i0][k1] - (float)t->d[i0][k0]) * f;
    b = (float)t->d[i1][k0] + ((float)t->d[i1][k1] - (float)t->d[i1][k0]) * f;
    return (a + (b - a) * fr) * (1.0f / 32767.0f);
}

void wt_spectrum(int table, int morph_, float amount, float fp, float mag[64])
{
    int h, i0;
    float fr, mx = 0.0f;
    init();
    table = table < 0 ? 0 : table >= WT_NFACT ? WT_NFACT - 1 : table;
    if (morph_ < 0 || morph_ >= SM_N || !(amount > 0.0f))
        morph_ = SM_NONE;
    amount = fm_clampf(amount, 0.0f, 1.0f);
    fp = fm_clampf(fp, 0.0f, (float)(WT_FRAMES - 1));
    for (h = 0; h < 64; h++)
        mag[h] = 0.0f;
    if (morph_ == SM_SKEW) {             /* magnitudes only: each source frame once */
        float bt = fp / (float)(WT_FRAMES - 1), sh = skew_shift(amount);
        int f;
        for (f = 0; f < WT_FRAMES; f++) {
            int need = 0;
            for (h = 1; h <= 64 && !need; h++) {
                float x = skew_index(bt, sh, h);
                int from = (int)x;
                if (from > WT_FRAMES - 2)
                    from = WT_FRAMES - 2;
                need = from == f || from + 1 == f;
            }
            if (!need)
                continue;
            frame_recipe(table, f);
            for (h = 1; h <= 64; h++) {
                float x = skew_index(bt, sh, h), t;
                int from = (int)x;
                if (from > WT_FRAMES - 2)
                    from = WT_FRAMES - 2;
                t = fm_minf(1.0f, x - (float)from);
                if (from == f)
                    mag[h - 1] += (1.0f - t) * amp_of(g_re[h], g_im[h]);
                if (from + 1 == f)
                    mag[h - 1] += t * amp_of(g_re[h], g_im[h]);
            }
        }
    } else {
        i0 = (int)fp;
        fr = fp - (float)i0;
        frame_recipe(table, i0);
        morph(morph_, amount);
        for (h = 1; h <= 64; h++)
            mag[h - 1] = amp_of(g_re[h], g_im[h]);
        if (fr > 1e-4f && i0 < WT_FRAMES - 1) {
            frame_recipe(table, i0 + 1);
            morph(morph_, amount);
            for (h = 1; h <= 64; h++)
                mag[h - 1] += (amp_of(g_re[h], g_im[h]) - mag[h - 1]) * fr;
        }
    }
    for (h = 0; h < 64; h++)
        mx = fm_maxf(mx, mag[h]);
    if (mx > 1e-12f)
        for (h = 0; h < 64; h++)
            mag[h] /= mx;
}
