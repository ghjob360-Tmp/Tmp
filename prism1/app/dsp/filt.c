/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the filters, after Vital (Matt Tytel, GPL-3.0): synthesis/filters and modules/filter_module.cpp.
 *
 * Vital runs these on poly_float SIMD, mostly at 88.2 / 96 kHz (2x oversampling); here they are scalar stereo at
 * 44.1 kHz, so every one-pole is the exact TPT (trapezoidal) form with G = g / (1 + g), g = tan(pi f / fs) (Vital's
 * coefficient table is an approximation of that same G), and the loops that Vital closes with a one-sample delay are
 * kept as they are only where a saturator bounds them. The per-model notes are at each run function.
 *
 * Smoothing: filt_ctl moves the cutoff (as a note) one-pole toward its target (tau 1.5 ms) and computes the block's
 * coefficient targets; filt_run moves each coefficient linearly from where the last block ended to its target (Vital's
 * per-block ramps): the ones that set the frequency (G, the SVF's a1..a3, the comb's period, the all-pass a) a step
 * every sample, the gains, drives and mix amounts a step every 8 samples. A cutoff that jumps every block never
 * zippers or clicks.
 *
 * Denormals: at the end of each block a state under 1e-15 (-300 dB) is set to 0, so no state decays on through the
 * denormals after silence. A state that went non-finite (NaN in the input) is cleared at the end of the block.
 *
 * Layout of filt_t.s (floats): s[0] the smoothed cutoff note (all models but FORMANT), then the states (L's, then
 * R's), then the ramps: NR current values followed by NR per-sample steps, at RB. */
#include "filt.h"

const char *const FLT_NAME[FLT_N] = {"Analog", "Dirty", "Ladder", "Digital", "Diode", "Formant", "Comb", "Phaser"};
const int FLT_NSTYLE[FLT_N] = {5, 5, 5, 5, 2, 2, 9, 2};
const char *const FLT_STYLE_NAME[FLT_N][FLT_MAXSTYLE] = {
    {"12 dB", "24 dB", "Notch Blnd", "Notch Sprd", "B/P/N"},
    {"12 dB", "24 dB", "Notch Blnd", "Notch Sprd", "B/P/N"},
    {"12 dB", "24 dB", "Notch Blnd", "Notch Sprd", "B/P/N"},
    {"12 dB", "24 dB", "Notch Blnd", "Notch Sprd", "B/P/N"},
    {"Low Shelf", "Low Cut"},
    {"AOIE", "AIUO"},
    {"LoHi Comb", "LoBnd Comb", "Sprd Comb", "LoHi Fl+", "LoBnd Fl+", "Sprd Fl+", "LoHi Fl-", "LoBnd Fl-",
     "Sprd Fl-"},
    {"Positive", "Negative"},
};

#define PRIMED 0x80000000u               /* filt_t.w: the ramps hold this patch's values (else jump to the targets) */
#define WMASK (COMB_LEN - 1u)
#define MAXHZ (0.45351473923f * SR)      /* Vital's kMinNyquistMult: 20 kHz */
#define NOTE_TAU (1.0f / 66.15f)         /* the cutoff smoothing: 1 / (1.5 ms in samples) */
#if defined(__clang__)
#define SMALL __attribute__((minsize))   /* control-rate code: RAM is short */
#else
#define SMALL
#endif

/* the multi-mode styles (Vital's SynthFilter::Style, minus shelving) */
enum { ST_12, ST_24, ST_NOTCH_BLEND, ST_NOTCH_SPREAD, ST_BPN };

/* the ramps' base index and count per model; in each model's order, the ones that follow the cutoff come first (they
 * step every sample: LOOP's NF) */
static const uint8_t RB[FLT_N] = {9, 13, 9, 9, 15, 16, 5, 33};
static const uint8_t NR[FLT_N] = {10, 11, 8, 14, 7, 16, 7, 6};
/* ANALOG ramps */
enum { SK_G, SK_DN, SK_RN, SK_PN, SK_LO, SK_BA, SK_HI, SK_ML, SK_MB, SK_MH };
/* DIRTY */
enum { DT_G, DT_RFM, DT_PN, DT_NORM, DT_DRV, DT_LO, DT_BA, DT_HI, DT_ML, DT_MB, DT_MH };
/* LADDER */
enum { LD_G, LD_DN, LD_KN, LD_T0 };
/* DIGITAL */
enum { SV_A1, SV_A2, SV_A3, SV_P1, SV_P2, SV_P3, SV_DRV, SV_POST, SV_B10, SV_B11, SV_B12, SV_B20, SV_B21, SV_B22 };
/* DIODE */
enum { DD_G, DD_GH, DD_HN, DD_HPA, DD_DRV, DD_RES, DD_POST };
/* COMB */
enum { CB_PER, CB_FB, CB_SCALE, CB_G1, CB_G2, CB_LO, CB_HI };
/* PHASER */
enum { PH_A, PH_RES, PH_DRV, PH_P1, PH_P3, PH_P5 };
#define PH_GLO 45
#define PH_GHI 46
#define G20 0.00142276f                  /* the diode's 20 Hz feedback high pass: G at 20 Hz */
#define DIRTY_MAXHZ 14000.0f             /* DIRTY's loop is closed through last sample's states: chaotic above */

/* ------------------------------------------------------------------ helpers --- */
/* tanh-like soft clip: x - 4/27 x^3 on [-1.5, 1.5], +-1 beyond (slope 1 at 0, 0 at the knee) */
INL float sat(float x)
{
    x = x < -1.5f ? -1.5f : x > 1.5f ? 1.5f : x;
    return x * (1.0f - 0.148148148f * x * x);
}
/* Vital's hardTanh: linear to 0.66, then a soft knee to 1 */
INL float hclip(float x)
{
    float a = fm_fabsf(x), e;
    if (a <= 0.66f)
        return x;
    e = (a - 0.66f) * (1.0f / 0.34f);
    e = e > 1.5f ? 1.5f : e;
    e = 0.66f + 0.34f * e * (1.0f - 0.148148148f * e * e);
    return x < 0.0f ? -e : e;
}
/* one TPT one-pole low pass step: returns the output, updates the state */
INL float tpt(float *s, float x, float G)
{
    float v = (x - *s) * G, y = *s + v;
    *s = y + v;
    return y;
}
INL int16_t q16(float x)
{
    x *= 16384.0f;
    x = x < -32767.0f ? -32767.0f : x > 32767.0f ? 32767.0f : x;
    return (int16_t)x;
}

static RAMC NOIN SMALL float r_hz(float note) { return note_hz(note); }
/* g = tan(pi hz / fs) for hz in [1, MAXHZ] */
static RAMC NOIN SMALL float r_g(float hz)
{
    hz = hz < 1.0f ? 1.0f : hz > MAXHZ ? MAXHZ : hz;
    return fm_tanf(FM_PI * ISR * hz);
}
static RAMC NOIN SMALL float r_sqrt(float x) { return fm_sqrtf(x); }
static RAMC NOIN SMALL float r_exp2(float x) { return fm_exp2f(x); }
static RAMC NOIN SMALL float r_sin(float x) { return fm_sinf(x); }
#define r_db(db) r_exp2((db) * 0.16609640474f)
INL float GG(float g) { return g / (1.0f + g); }

/* Vital's blend amounts for the multi-mode styles (setupFilter in sallen_key / digital_svf / dirty): b is Vital's
 * blend -1..1; returns the drive, from base (the plain drive) and scaled (the resonance-compensated one) */
static RAMC NOIN SMALL float mm_blend(int style, float b, float base, float scaled, float *lo, float *ba, float *hi)
{
    float t, dt;
    switch (style) {
    case ST_NOTCH_SPREAD:                /* Vital kDualNotchBand ("Notch Spread") */
        t = b * 0.5f + 0.5f;
        dt = fm_minf(1.0f - b, 1.0f);
        *lo = t;
        *ba = 0.0f;
        *hi = 1.0f;
        return lerpf(base, scaled * (2.0f - t), dt);
    case ST_NOTCH_BLEND:                 /* kNotchPassSwap ("Notch Blend") */
        *lo = fm_minf(1.0f - b, 1.0f);
        *ba = 0.0f;
        *hi = fm_minf(1.0f + b, 1.0f);
        return lerpf(base, scaled, fm_fabsf(b));
    case ST_BPN: {                       /* kBandPeakNotch ("B/P/N") */
        float inv, mult, pbv;
        dt = fm_minf(1.0f - b, 1.0f);
        inv = 1.0f - dt;
        mult = r_sqrt(inv * inv * 0.5f + 0.5f);
        pbv = fm_minf(b, 0.0f);
        *lo = mult * (pbv + 1.0f);
        *ba = mult * (pbv - b + 1.0f) * 2.0f;
        *hi = *lo;
        return lerpf(base, scaled, dt);
    }
    default:                             /* 12 / 24 dB: low .. band .. high */
        *ba = r_sqrt(fm_maxf(1.0f - b * b, 0.0f));
        *lo = fm_maxf(-b, 0.0f);
        *hi = fm_maxf(b, 0.0f);
        return scaled;
    }
}

/* the formant table (formant_filter.cpp): gain dB, resonance, MIDI note, for the four formants of each vowel */
static const float VOWEL[5][4][3] = {
    {{-2, 0.66f, 75.7552343327f}, {-8, 0.75f, 84.5454706023f}, {-9, 1.0f, 100.08500317f}, {-10, 1.0f, 101.645729657f}},
    {{0, 0.66f, 67.349957715f}, {-14, 0.75f, 92.39951181f}, {-4, 1.0f, 99.7552343327f}, {-14, 1.0f, 103.349957715f}},
    {{0, 0.8f, 61.7825925179f}, {-15, 0.75f, 94.049554095f}, {-17, 1.0f, 101.03821678f}, {-20, 1.0f, 103.618371471f}},
    {{-2, 0.7f, 67.349957715f}, {-6, 0.75f, 79.349957715f}, {-14, 1.0f, 99.7552343327f}, {-14, 1.0f, 101.03821678f}},
    {{0, 0.7f, 65.0382167797f}, {-20, 0.75f, 74.3695077237f}, {-17, 1.0f, 100.408607741f}, {-14, 1.0f, 101.645729657f}},
};
enum { V_A, V_E, V_I, V_O, V_U };
/* the pad's corners: bottom left, bottom right, top left, top right, as the styles' names read (Vital's own table
 * gives its "AOIE" style the A I U O corners and the other way round) */
static const uint8_t PAD[2][4] = {{V_A, V_O, V_I, V_E}, {V_A, V_I, V_U, V_O}};
/* the comb styles: feedback (0 comb, 1 flange +, 2 flange -) and filter (0 low-high, 1 low-band, 2 band spread) */
static const uint8_t COMB_FB[9] = {0, 0, 0, 1, 1, 1, 2, 2, 2};
static const uint8_t COMB_FL[9] = {0, 1, 2, 0, 1, 2, 0, 1, 2};

/* ------------------------------------------------------------- parameters --- */
typedef struct {
    float t[16];                         /* the ramp targets, in the model's ramp order */
    float a[4];                          /* more, for filt_response */
} fp_t;

/* the coefficient targets for this block (Vital's setupFilter, per model) */
static RAMC NOIN SMALL void params(int model, int style, const filt_ctl_t *c, float note, fp_t *p)
{
    float reso = clampf(c->reso, 0.0f, 1.0f), bl = clampf(c->blend, 0.0f, 1.0f);
    float ddb = 20.0f * clampf(c->drive, 0.0f, 1.0f), dlin = r_db(ddb), dpct = ddb * (1.0f / 36.0f);
    float b = 2.0f * bl - 1.0f, hz = r_hz(note), *t = p->t, *a = p->a;
    float g, G, lo, ba, hi;

    switch (model) {
    case FLT_ANALOG: {                   /* sallen_key_filter.cpp */
        float rp = r_sqrt(reso), res = 2.15f * rp + dpct * reso * 1.1f, rsc = 2.0f * reso + 1.0f;
        float drive, post, rt, gg, norm;
        g = r_g(hz);
        G = GG(g);
        drive = mm_blend(style, b, dlin, dlin / rsc, &lo, &ba, &hi);
        post = 1.0f / r_sqrt(rsc * drive);
        rt = res / fm_maxf(1.0f, 0.18f * G + 0.97f);
        gg = G * G - G;
        norm = 1.0f / (rt * gg + 1.0f);
        t[SK_G] = G;
        t[SK_DN] = drive * norm;
        t[SK_RN] = rt * norm;
        t[SK_PN] = 1.0f / (gg + 1.0f);
        t[SK_LO] = lo;
        t[SK_BA] = ba;
        t[SK_HI] = hi;
        if (style == ST_NOTCH_SPREAD) {  /* processDual: the second stage swaps the low and high amounts */
            t[SK_ML] = hi * post;
            t[SK_MB] = 0.0f;
            t[SK_MH] = lo * post;
        } else {
            t[SK_ML] = lo * post;
            t[SK_MB] = ba * post;
            t[SK_MH] = hi * post;
        }
        a[0] = drive;
        a[1] = rt;
        break;
    }
    case FLT_DIRTY: {                    /* dirty_filter.cpp */
        float r = r_sqrt(reso), rin, res, rs2, base;
        g = r_g(fm_minf(hz, DIRTY_MAXHZ));
        G = GG(g);
        rin = clampf(r / fm_maxf(1.0f, 0.5f * G + 0.97f), 0.0f, 1.0f);
        res = lerpf(0.1f, 2.15f, rin) + dpct * 0.05f;
        rs2 = res * res;
        base = (dlin - 1.0f) * 2.0f + 1.0f;
        t[DT_DRV] = mm_blend(style, b, base, fm_maxf(0.1f, base) / (rs2 * 0.5f + 1.0f), &lo, &ba, &hi);
        t[DT_G] = G;
        t[DT_NORM] = 1.4f / (rs2 + 1.0f);
        t[DT_RFM] = res / ((res * (G * G - G) + 1.0f) * (G + 1.0f));
        t[DT_PN] = 1.0f / (G * G - G + 1.0f);
        t[DT_ML] = lo;
        t[DT_MB] = ba;
        t[DT_MH] = hi;
        if (style == ST_NOTCH_SPREAD) {  /* tickDual: the first stage swaps them */
            t[DT_LO] = hi;
            t[DT_BA] = 0.0f;
            t[DT_HI] = lo;
        } else {
            t[DT_LO] = lo;
            t[DT_BA] = ba;
            t[DT_HI] = hi;
        }
        break;
    }
    case FLT_LADDER: {                   /* ladder_filter.cpp (setStageScales) */
        static const float LP12[5] = {0, 0, 1, 0, 0}, BP12[5] = {0, 1, -1, 0, 0}, HP12[5] = {1, -2, 1, 0, 0};
        static const float LP24[5] = {0, 0, 0, 0, 1}, BP24[5] = {0, 0, -1, 2, -1}, HP24[5] = {1, -4, 6, -4, 1};
        float ra = style ? r_sin(reso * (0.5f * FM_PI)) : reso, k, dm, rsc, drive, post, bp, lp, hp, k4;
        int i;
        g = r_g(fm_minf(hz, 20000.0f));
        G = GG(g);
        k = lerpf(0.001f, 4.1f, ra) + dpct * reso * 5.0f;
        bp = r_sqrt(fm_maxf(1.0f - b * b, 0.0f));
        lp = fm_maxf(-b, 0.0f);
        hp = fm_maxf(b, 0.0f);
        dm = style ? r_sin(reso) + 1.0f : reso + 1.0f;
        rsc = lerpf(dm, 1.0f, hp);
        drive = dlin * rsc;
        post = 1.0f / r_sqrt((dlin - 1.0f) * 0.5f + 1.0f);
        if (style == ST_12) {
            for (i = 0; i < 5; i++)
                t[LD_T0 + i] = lp * LP12[i] + bp * BP12[i] + hp * HP12[i];
        } else if (style == ST_24) {
            bp = 1.0f - fm_fabsf(b);
            post = 1.0f / r_sqrt((dlin - 1.0f) * 0.25f + 1.0f);
            for (i = 0; i < 5; i++)
                t[LD_T0 + i] = lp * LP24[i] + bp * BP24[i] + hp * HP24[i];
        } else if (style == ST_NOTCH_SPREAD) {
            float lf = fm_minf(b + 1.0f, 1.0f), hf = fm_minf(1.0f - b, 1.0f);
            drive = dlin;
            t[LD_T0] = lf;
            t[LD_T0 + 1] = lf * -4.0f;
            t[LD_T0 + 2] = hf * 4.0f + lf * 8.0f;
            t[LD_T0 + 3] = hf * -8.0f - lf * 8.0f;
            t[LD_T0 + 4] = hf * 4.0f + lf * 4.0f;
        } else if (style == ST_NOTCH_BLEND) {
            float lf = fm_minf(b + 1.0f, 1.0f), hf = fm_minf(1.0f - b, 1.0f), l2 = lf * lf, lh = lf * hf;
            t[LD_T0] = l2;
            t[LD_T0 + 1] = l2 * -4.0f;
            t[LD_T0 + 2] = l2 * 6.0f + lh * 2.0f;
            t[LD_T0 + 3] = l2 * -4.0f - lh * 4.0f;
            t[LD_T0 + 4] = l2 + hf * hf + lh * 2.0f;
        } else {                         /* B/P/N */
            float dt = fm_minf(1.0f - b, 1.0f), inv = 1.0f - dt, mult = r_sqrt(inv * inv * 0.5f + 0.5f);
            float pbv = fm_minf(b, 0.0f), lh = mult * (pbv + 1.0f), band = mult * (pbv - b + 1.0f) * 2.0f;
            drive = lerpf(dlin, drive, dt);
            for (i = 0; i < 5; i++)
                t[LD_T0 + i] = lh * LP12[i] + band * BP12[i] + lh * HP12[i];
        }
        for (i = 0; i < 5; i++)
            t[LD_T0 + i] *= post;
        /* the zero-delay ladder: u = sat((drive x - k (1 - G) S) / (1 + k G^4)) */
        k4 = 1.0f / (1.0f + k * G * G * G * G);
        t[LD_G] = G;
        t[LD_DN] = drive * k4;
        t[LD_KN] = k * (1.0f - G) * k4;
        a[0] = drive;
        a[1] = k;
        break;
    }
    case FLT_DIGITAL: {                  /* digital_svf.cpp */
        float ra = reso * reso * reso, k = 1.0f / lerpf(0.5f, 16.0f, ra), drive, a1;
        int i;
        g = r_g(hz);
        drive = mm_blend(style, b, dlin, dlin / (ra * 2.0f + 1.0f), &lo, &ba, &hi);
        a1 = 1.0f / (1.0f + g * (g + k));
        t[SV_A1] = a1;
        t[SV_A2] = g * a1;
        t[SV_A3] = g * g * a1;
        a1 = 1.0f / (1.0f + g * (g + 1.0f));
        t[SV_P1] = a1;
        t[SV_P2] = g * a1;
        t[SV_P3] = g * g * a1;
        t[SV_DRV] = drive;
        t[SV_POST] = 1.0f / r_sqrt(dlin);
        t[SV_B10] = hi;                  /* out = v0 in + v1 band + v2 low (hp = in - k band - low) */
        t[SV_B11] = ba - k * hi;
        t[SV_B12] = lo - hi;
        if (style == ST_NOTCH_SPREAD) {
            t[SV_B20] = lo;
            t[SV_B21] = ba - k * lo;
            t[SV_B22] = hi - lo;
        } else {
            for (i = 0; i < 3; i++)
                t[SV_B20 + i] = t[SV_B10 + i];
        }
        a[0] = g;
        a[1] = k;
        break;
    }
    case FLT_DIODE: {                    /* diode_filter.cpp */
        float rp = reso * reso * reso, res = lerpf(0.7f, 17.0f, rp), ratio, gh;
        g = r_g(hz);
        if (style == 0) {                /* low shelf: a high pass an octave down, faded in by the blend */
            ratio = 0.5f;
            t[DD_HPA] = bl * bl;
        } else {                         /* low cut: the high pass moves from 9 octaves down to 1 */
            ratio = r_exp2(bl * 8.0f - 9.0f);
            t[DD_HPA] = 1.0f;
        }
        gh = r_g(hz * ratio);
        G = GG(gh);
        t[DD_GH] = G;
        t[DD_HN] = 1.0f / (G * G - G + 1.0f);
        t[DD_G] = GG(g);
        t[DD_DRV] = 0.5f * (res * 0.5f + 1.0f) * dlin;
        t[DD_RES] = 0.5f * res;
        t[DD_POST] = 1.0f / r_sqrt(dlin);
        break;
    }
    case FLT_FORMANT: {                  /* formant_filter.cpp + formant_manager.cpp: four DigitalSvf band passes */
        float x = clampf(c->fx, 0.0f, 1.0f), y = clampf(c->fy, 0.0f, 1.0f), tr = clampf(c->transpose, -24.0f, 24.0f);
        float fres = 0.3f + 0.7f * reso, spread = b;   /* Vital's formant resonance 0.3..1, formant spread -1..1 */
        const uint8_t *pad = PAD[style & 1];
        int i, j;
        for (i = 0; i < 4; i++) {
            float v[3], r, ra, k, d, D;
            for (j = 0; j < 3; j++) {
                float bot = lerpf(VOWEL[pad[0]][i][j], VOWEL[pad[1]][i][j], x);
                float top = lerpf(VOWEL[pad[2]][i][j], VOWEL[pad[3]][i][j], x);
                v[j] = lerpf(bot, top, y);
            }
            g = r_g(r_hz(lerpf(v[2], 80.0f, spread) + tr));
            r = clampf(v[1] * fres, 0.0f, 1.0f);
            ra = r * r * r;
            k = 1.0f / lerpf(4.0f, 30.0f, ra);
            d = 1.0f / (ra * 2.0f + 1.0f);       /* the SVF's drive compensation */
            D = 1.0f / (1.0f + g * k + g * g);
            t[4 * i] = g * D * d;                 /* the SVF's band output as a biquad, the drive folded in */
            t[4 * i + 1] = 2.0f * (g * g - 1.0f) * D;
            t[4 * i + 2] = (1.0f - g * k + g * g) * D;
            t[4 * i + 3] = r_db(clampf(v[0], -15.0f, 15.0f));
        }
        break;
    }
    case FLT_COMB: {                     /* comb_filter.cpp */
        int fb = COMB_FB[style], fl = COMB_FL[style];
        float per, minp = fb == 2 ? 4.0f : 2.0f, center = note + 84.0f * clampf(c->fx, 0.0f, 1.0f);
        float mincut = note - 48.0f, h1, h2;
        per = SR / clampf(hz, SR / (float)(COMB_LEN - 5u), MAXHZ);
        t[CB_PER] = clampf(per, minp, (float)(COMB_LEN - 5u));
        if (fb == 0) {
            float f = 2.0f * reso - 1.0f;
            f = f / r_sqrt(fm_fabsf(f) + 0.00001f);
            t[CB_FB] = f;
            t[CB_SCALE] = 1.0f - f * f * 0.5f;
        } else {
            t[CB_FB] = reso;
            t[CB_SCALE] = 1.0f / (reso + 1.0f);
        }
        if (fl == 0) {                   /* low-high: the blend fades low pass -> both (flat) -> high pass */
            h1 = clampf(r_hz(center), 1.0f, MAXHZ);
            h2 = clampf(r_hz(mincut), 1.0f, MAXHZ);
            t[CB_LO] = clampf(2.0f - 2.0f * bl, 0.0f, 1.0f);
            t[CB_HI] = clampf(2.0f * bl, 0.0f, 1.0f);
        } else {
            if (fl == 2) {               /* band spread: a band around the center, 0..8 octaves wide */
                float range = bl * 96.0f;
                h1 = r_hz(center + range);
                h2 = r_hz(fm_maxf(mincut, center - range));
            } else {                     /* low-band (added): the low cut rises to the low pass, a band */
                h1 = r_hz(center);
                h2 = r_hz(lerpf(mincut, center, bl));
            }
            h1 = clampf(h1, 1.0f, SR / 2.1f);
            h2 = clampf(h2, 1.0f, SR / 2.1f);
            t[CB_LO] = h2 / h1 + 1.0f;
            t[CB_HI] = 0.0f;
        }
        t[CB_G1] = GG(r_g(h1));
        t[CB_G2] = GG(r_g(h2));
        break;
    }
    default: {                           /* FLT_PHASER: phaser_filter.cpp (not clean) */
        float pb = 2.0f * bl, inv = style ? -1.0f : 1.0f;
        g = r_g(hz);
        G = GG(g);
        t[PH_A] = (1.0f - g) / (1.0f + g);
        t[PH_RES] = inv * reso;
        t[PH_DRV] = (reso * 0.5f + 1.0f) * dlin;
        t[PH_P1] = clampf(1.0f - pb, 0.0f, 1.0f);
        t[PH_P5] = clampf(pb - 1.0f, 0.0f, 1.0f);
        t[PH_P3] = 1.0f - t[PH_P1] - t[PH_P5];
        a[0] = fm_minf(G * 20.0f, 0.9f);
        a[1] = G * (1.0f / 20.0f);
        a[2] = inv;
        break;
    }
    }
}

/* ------------------------------------------------------------------ control --- */
RAMC SMALL void filt_reset(filt_t *f, int model, int style)
{
    uint32_t i;
    if (model < 0 || model >= FLT_N)
        model = FLT_ANALOG;
    if (style < 0 || style >= FLT_NSTYLE[model])
        style = 0;
    for (i = 0; i < 48u; i++)
        f->s[i] = 0.0f;
    if (model == FLT_COMB)
        for (i = 0; i < COMB_LEN; i++)
            f->line[0][i] = f->line[1][i] = 0;
    f->w = 0;
    f->model = model;
    f->style = style;
}

RAMC SMALL void filt_ctl(filt_t *f, const filt_ctl_t *c, uint32_t n)
{
    fp_t p;
    int model = f->model, style = f->style, prime = !(f->w & PRIMED), i, nr, rb;
    float note = clampf(c->cutoff, 0.0f, 150.0f), inv, *cur, *dl;
    if (style >= FLT_NSTYLE[model])
        style = 0;
    if (model != FLT_FORMANT) {
        if (prime)
            f->s[0] = note;
        else
            f->s[0] += (note - f->s[0]) * (1.0f - r_exp2(-(float)n * (NOTE_TAU * FM_LOG2E)));
        note = f->s[0];
    }
    params(model, style, c, note, &p);
    nr = NR[model];
    rb = RB[model];
    cur = f->s + rb;
    dl = cur + nr;
    inv = 1.0f / (float)(n ? n : 1u);
    if (model == FLT_PHASER) {
        f->s[PH_GLO] = p.a[0];
        f->s[PH_GHI] = p.a[1];
        for (i = PH_P1; i <= PH_P5; i++)          /* a weight that ends at 0 is 0 (its stages may stop) */
            if (p.t[i] == 0.0f && fm_fabsf(cur[i]) < 1e-4f)
                cur[i] = 0.0f;
    }
    for (i = 0; i < nr; i++) {
        if (prime)
            cur[i] = p.t[i];
        dl[i] = (p.t[i] - cur[i]) * inv;
    }
    f->w |= PRIMED;
}

/* ---------------------------------------------------------------------- run --- */
/* Each model runs one channel per call (called for L, then R): the states and the ramps live in locals (registers or
 * the stack, never aliased by the audio), the sample loop is not unrolled (RAM is short), and the 12 / 24 dB variants
 * share one loop with a per-sample flag. The ramps are stepped in each channel's pass (a few adds a sample). */
#if defined(__clang__)
#define NOUNROLL _Pragma("clang loop unroll(disable)")
#else
#define NOUNROLL
#endif
#define RAMP_LOAD(N)                                                                                                  \
    float cv[N], dv[N];                                                                                               \
    int ri;                                                                                                           \
    for (ri = 0; ri < N; ri++) {                                                                                      \
        cv[ri] = r[ri];                                                                                               \
        dv[ri] = r[N + ri];                                                                                           \
    }
/* the sample loop: the first NF ramps step every sample, the rest every SUB samples (by m steps, at the sub-block's
 * start) */
#define SUB 8u
#define LOOP(N, NF)                                                                                                   \
    for (i = 0; i < n;) {                                                                                             \
        uint32_t m_ = n - i < SUB ? n - i : SUB, e_ = i + m_;                                                         \
        float fm_ = (float)m_;                                                                                        \
        for (ri = NF; ri < N; ri++)                                                                                   \
            cv[ri] += dv[ri] * fm_;                                                                                   \
        NOUNROLL                                                                                                      \
        for (; i < e_; i++) {                                                                                         \
            for (ri = 0; ri < NF; ri++)                                                                               \
                cv[ri] += dv[ri];
#define END_LOOP }
#define RAMP_STORE(N)                                                                                                 \
    if (ch)                              /* (both channels start from the block's start) */                           \
        for (ri = 0; ri < N; ri++)                                                                                    \
            r[ri] = cv[ri];

/* end of block: flush the states that decayed to nothing (-300 dB: they would go on through the denormals), and
 * clear them all if any went non-finite (or huge) */
INL void guard(float *s, int n)
{
    float m = 0.0f, a;
    int i;
    for (i = 0; i < n; i++) {
        a = fm_fabsf(s[i]);
        if (a < 1e-15f)
            s[i] = 0.0f;
        m += a;
    }
    if (!(m < 1e6f))
        for (i = 0; i < n; i++)
            s[i] = 0.0f;
}

/* ANALOG: Vital's SallenKeyFilter. Two TPT one-poles with the band output fed back (positive: Q = 1 / (2 - res)),
 * solved without delay, then tanh (here the cubic sat) on the stage input. 24 dB styles: a flat (Q 1) pre-stage
 * mixed by the same blend; Notch Spread swaps the amounts between the stages. */
static RAMC NOIN void run_analog(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 4 * ch, *r = f->s + 9;
    float s1 = s[0], s2 = s[1], p1 = s[2], p2 = s[3];
    int pre = f->style != ST_12;
    uint32_t i;
    RAMP_LOAD(10)
    LOOP(10, 4)
        float G, m2, m1, in, y1, y2, bp;
        G = cv[SK_G];
        m2 = 1.0f - G;
        m1 = -m2 * m2;
        in = x[i];
        if (pre) {
            in = (in - (m1 * p1 + m2 * p2)) * cv[SK_PN];
            y1 = tpt(&p1, in, G);
            y2 = tpt(&p2, y1, G);
            bp = y1 - y2;
            in = cv[SK_LO] * y2 + cv[SK_BA] * bp + cv[SK_HI] * (in - y1 - bp);
        }
        in = sat(cv[SK_DN] * in - cv[SK_RN] * (m1 * s1 + m2 * s2));
        y1 = tpt(&s1, in, G);
        y2 = tpt(&s2, y1, G);
        bp = y1 - y2;
        x[i] = cv[SK_ML] * y2 + cv[SK_MB] * bp + cv[SK_MH] * (in - y1 - bp);
    }
    END_LOOP
    RAMP_STORE(10)
    s[0] = s1;
    s[1] = s2;
    s[2] = p1;
    s[3] = p2;
    guard(s, 4);
}

/* DIRTY: Vital's DirtyFilter. A plain 2-pole (no feedback) shaped by the blend, then a saturated loop: the loop's
 * tanh output, low passed (stage 3) and band passed (stage 4, both quickTanh one-poles), comes back with the
 * resonance; the output is the loop's tanh, so it is level-limited (~0.71 peak). In Vital the 24 dB pre-stage's
 * feedback reads states its one-poles never write (always 0), so it is a plain 2-pole: kept as it sounds. */
static RAMC NOIN void run_dirty(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 6 * ch, *r = f->s + 13;
    float s1 = s[0], s2 = s[1], s3 = s[2], s4 = s[3], p1 = s[4], p2 = s[5];
    int pre = f->style != ST_12;
    uint32_t i;
    RAMP_LOAD(11)
    LOOP(11, 3)
        float G, in, y1, y2, bp, pass, z3, z4, loop, d3, y3;
        G = cv[DT_G];
        in = x[i];
        if (pre) {
            in *= cv[DT_PN];
            y1 = tpt(&p1, in, G);
            y2 = tpt(&p2, y1, G);
            bp = y1 - y2;
            in = cv[DT_LO] * y2 + cv[DT_BA] * bp + cv[DT_HI] * (in - y1 - bp);
        }
        in *= cv[DT_NORM];
        y1 = tpt(&s1, in, G);
        y2 = tpt(&s2, y1, G);
        bp = y1 - y2;
        pass = cv[DT_ML] * y2 + cv[DT_MB] * bp + cv[DT_MH] * (in - y1 - bp);
        z3 = sat(s3);
        z4 = sat(s4);
        loop = sat(cv[DT_DRV] * pass + cv[DT_RFM] * (z4 + pass + G * (pass - z3)));
        d3 = G * (loop - z3);
        s3 += d3;
        y3 = sat(s3);
        s3 += d3;
        s4 += 2.0f * G * (loop - y3 - z4);
        x[i] = loop * (1.0f / 1.4f);
    }
    END_LOOP
    RAMP_STORE(11)
    s[0] = s1;
    s[1] = s2;
    s[2] = s3;
    s[3] = s4;
    s[4] = p1;
    s[5] = p2;
    guard(s, 6);
}

/* LADDER: Vital's LadderFilter, as a zero-delay-feedback 4-pole ladder: the loop is solved for the input (k = 4
 * oscillates at every cutoff, where Vital's one-sample estimate needs its 2x rate), then saturated; the output mixes
 * the input and the four stages by Vital's stage scales (12 / 24 dB, notch blend / spread, B/P/N). Vital's stages
 * also saturate (algebraicSat): here only the input does. */
static RAMC NOIN void run_ladder(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 4 * ch, *r = f->s + 9;
    float s0 = s[0], s1 = s[1], s2 = s[2], s3 = s[3];
    uint32_t i;
    RAMP_LOAD(8)
    LOOP(8, 3)
        float G, u, y1, y2, y3, y4;
        G = cv[LD_G];
        u = sat(cv[LD_DN] * (x[i]) - cv[LD_KN] * (((s0 * G + s1) * G + s2) * G + s3));
        y1 = tpt(&s0, u, G);
        y2 = tpt(&s1, y1, G);
        y3 = tpt(&s2, y2, G);
        y4 = tpt(&s3, y3, G);
        x[i] = cv[LD_T0] * u + cv[LD_T0 + 1] * y1 + cv[LD_T0 + 2] * y2 + cv[LD_T0 + 3] * y3 + cv[LD_T0 + 4] * y4;
    }
    END_LOOP
    RAMP_STORE(8)
    s[0] = s0;
    s[1] = s1;
    s[2] = s2;
    s[3] = s3;
    guard(s, 4);
}

/* DIGITAL: Vital's DigitalSvf, the TPT state variable filter (Q 0.5..16, cubed resonance) mixed as
 * v0 in + v1 band + v2 low, hard-tanh on the output; 24 dB: a Q 1 pre-stage (mixed with the main stage's k, as
 * Vital), hard-tanh, the main stage. */
static RAMC NOIN void run_digital(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 4 * ch, *r = f->s + 9;
    float ic1 = s[0], ic2 = s[1], pc1 = s[2], pc2 = s[3];
    int pre = f->style != ST_12;
    uint32_t i;
    RAMP_LOAD(14)
    LOOP(14, 6)
        float in, v1, v2, v3, o;
        in = cv[SV_DRV] * (x[i]);
        if (pre) {
            v3 = in - pc2;
            v1 = cv[SV_P1] * pc1 + cv[SV_P2] * v3;
            v2 = pc2 + cv[SV_P2] * pc1 + cv[SV_P3] * v3;
            pc1 = 2.0f * v1 - pc1;
            pc2 = 2.0f * v2 - pc2;
            in = hclip(cv[SV_B10] * in + cv[SV_B11] * v1 + cv[SV_B12] * v2);
        }
        v3 = in - ic2;
        v1 = cv[SV_A1] * ic1 + cv[SV_A2] * v3;
        v2 = ic2 + cv[SV_A2] * ic1 + cv[SV_A3] * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        o = cv[SV_B20] * in + cv[SV_B21] * v1 + cv[SV_B22] * v2;
        x[i] = hclip(o) * cv[SV_POST];
    }
    END_LOOP
    RAMP_STORE(14)
    s[0] = ic1;
    s[1] = ic2;
    s[2] = pc1;
    s[3] = pc2;
    guard(s, 4);
}

/* DIODE: Vital's DiodeFilter. A Q 1 high pass (low shelf: an octave under the cutoff, faded in by the blend; low cut:
 * moved by the blend), then the diode ladder: tanh input with the last stage fed back, stage 1 (tanh) driven by the
 * input plus stage 2 through a 20 Hz high pass, stages 2 and 3 averaging their neighbours, stage 4 clamped. */
static RAMC NOIN void run_diode(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 7 * ch, *r = f->s + 15;
    float h1 = s[0], h2 = s[1], fl = s[2], s1 = s[3], s2 = s[4], s3 = s[5], s4 = s[6];
    int hp;
    uint32_t i;
    RAMP_LOAD(7)
    hp = cv[DD_HPA] != 0.0f || dv[DD_HPA] != 0.0f;
    LOOP(7, 3)
        float G, in, c4, t, fbin, fo, z1, d, y1, y2, y3;
        G = cv[DD_G];
        in = x[i];
        if (hp) {
            float Gh = cv[DD_GH], m2 = 1.0f - Gh, hin = (in - (m2 * h2 - m2 * m2 * h1)) * cv[DD_HN], a, b;
            a = tpt(&h1, hin, Gh);
            b = tpt(&h2, a, Gh);
            in += cv[DD_HPA] * (hin - 2.0f * a + b - in);
        }
        c4 = clampf(s4, -1.0f, 1.0f);
        t = sat(cv[DD_DRV] * in - cv[DD_RES] * c4);
        fbin = t + s2;
        fo = tpt(&fl, fbin, G20);
        z1 = sat(s1);
        d = G * (fbin - fo - z1);
        s1 += d;
        y1 = sat(s1);
        s1 += d;
        y2 = tpt(&s2, (y1 + s3) * 0.5f, G);
        y3 = tpt(&s3, (y2 + c4) * 0.5f, G);
        d = G * (y3 - c4);
        s4 += d;
        x[i] = clampf(s4, -1.0f, 1.0f) * cv[DD_POST];
        s4 += d;
    }
    END_LOOP
    RAMP_STORE(7)
    s[0] = h1;
    s[1] = h2;
    s[2] = fl;
    s[3] = s1;
    s[4] = s2;
    s[5] = s3;
    s[6] = s4;
    guard(s, 7);
}

/* FORMANT: Vital's FormantFilter: four DigitalSvf band passes (Q 4..30) at the vowel pad's formants, each hard-tanh'd
 * and weighted by its gain. Each band pass is its exact biquad (transposed direct form II): same response, half the
 * work. The blend is Vital's formant spread (-1..1: formants pulled to / pushed from MIDI 80), the resonance Vital's
 * formant resonance (0.3..1). */
static RAMC NOIN void run_formant(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 8 * ch, *r = f->s + 16, z[8];
    uint32_t i;
    int k;
    RAMP_LOAD(16)
    for (k = 0; k < 8; k++)
        z[k] = s[k];
    LOOP(16, 0)
        float in = x[i], o = 0.0f;
        NOUNROLL
        for (k = 0; k < 4; k++) {
            float bx = cv[4 * k] * in, y = bx + z[2 * k];
            z[2 * k] = z[2 * k + 1] - cv[4 * k + 1] * y;
            z[2 * k + 1] = -bx - cv[4 * k + 2] * y;
            o += cv[4 * k + 3] * hclip(y);
        }
        x[i] = o;
    }
    END_LOOP
    RAMP_STORE(16)
    for (k = 0; k < 8; k++)
        s[k] = z[k];
    guard(s, 8);
}

/* COMB: Vital's CombFilter, a delay of one period of the cutoff (Catmull-Rom read) with two one-poles in the loop:
 * low-high (low pass at cutoff + fx * 84 semitones, faded to its high pass by the blend), band spread (a band
 * around that, the blend its width: 0..8 octaves), low-band (added: the band narrows from a low pass to a band);
 * comb (feedback -1..1 by the resonance), flange + / - (feedback 0..1, the dry mixed in; - reads half a period and
 * subtracts). */
static RAMC NOIN void run_comb(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 2 * ch, *r = f->s + 5;
    float l1s = s[0], l2s = s[1];
    int fb = COMB_FB[f->style < 9 ? f->style : 0];
    float half = fb == 2 ? 0.5f : 1.0f, sg = fb == 2 ? -1.0f : 1.0f;
    uint32_t i, w = f->w & WMASK;
    int16_t *restrict b = f->line[ch];
    RAMP_LOAD(7)
    LOOP(7, 1)
        float D, t, t2, t3, rd, in, l1, st1, fo, push;
        uint32_t k;
        D = cv[CB_PER] * half;
        k = (uint32_t)(int32_t)D;
        t = D - (float)(int32_t)k;
        t2 = t * t;
        t3 = t2 * t;
        rd = ((-t + 2.0f * t2 - t3) * (float)b[(w - k + 1u) & WMASK] + (2.0f - 5.0f * t2 + 3.0f * t3) *
              (float)b[(w - k) & WMASK] + (t + 4.0f * t2 - 3.0f * t3) * (float)b[(w - k - 1u) & WMASK] +
              (t3 - t2) * (float)b[(w - k - 2u) & WMASK]) * (0.5f / 16384.0f);
        in = x[i];
        if (fb == 0)
            rd = cv[CB_SCALE] * in + rd * cv[CB_FB];
        l1 = tpt(&l1s, rd, cv[CB_G1]);
        st1 = cv[CB_LO] * l1 + cv[CB_HI] * (rd - l1);
        fo = st1 - tpt(&l2s, st1, cv[CB_G2]);
        if (fb == 0) {
            push = hclip(fo);
            x[i] = fo;
        } else {
            in *= 0.70710678f;
            push = in + sg * hclip(fo * cv[CB_FB]);
            x[i] = in * cv[CB_SCALE] + sg * fo;
        }
        b[w] = q16(push);
        w = (w + 1u) & WMASK;
    }
    END_LOOP
    RAMP_STORE(7)
    s[0] = l1s;
    s[1] = l2s;
    if (ch)
        f->w = (f->w & PRIMED) | w;
    guard(s, 2);
}

/* PHASER: Vital's PhaserFilter: 3 x 4 first-order all-passes at the cutoff (peaks after 4, 8, 12, mixed by the
 * blend), the output band-limited (cutoff / 20 .. cutoff x 20) and fed back by the resonance, hard-tanh on the input;
 * out = (in +- all-pass) / 2. The all-passes are direct form I (3 operations); stages whose weight is 0 are not run. */
/* four all-passes (a - z^-1) / (1 - a z^-1), direct form I: z[k] the input of stage k a sample ago (= the output of
 * stage k - 1), z[4] the last stage's output a sample ago (the caller stores the new one) */
INL float ap4(float *z, float in, float a)
{
    float y;
    y = a * (in + z[1]) - z[0];
    z[0] = in;
    in = y;
    y = a * (in + z[2]) - z[1];
    z[1] = in;
    in = y;
    y = a * (in + z[3]) - z[2];
    z[2] = in;
    in = y;
    y = a * (in + z[4]) - z[3];
    z[3] = in;
    return y;
}
static RAMC NOIN void run_phaser(filt_t *f, float *restrict x, uint32_t n, int ch)
{
    float *s = f->s + 1 + 16 * ch, *r = f->s + 33, Glo = f->s[PH_GLO], Ghi = f->s[PH_GHI], z[16];
    float inv = f->style ? -1.0f : 1.0f;
    int nst, j;
    uint32_t i;
    RAMP_LOAD(6)
    nst = (cv[PH_P5] != 0.0f || dv[PH_P5] != 0.0f) ? 12 : (cv[PH_P3] != 0.0f || dv[PH_P3] != 0.0f) ? 8 : 4;
    for (j = 0; j < 16; j++)             /* the stages that do not run restart from rest */
        z[j] = j > nst && j <= 12 ? 0.0f : s[j];
    LOOP(6, 1)
        float xi = x[i], a = cv[PH_A], in, o4, o8 = 0.0f, o12 = 0.0f, lows, apo;
        lows = tpt(&z[13], z[15], Glo);
        in = hclip(cv[PH_DRV] * xi + cv[PH_RES] * (lows - tpt(&z[14], lows, Ghi)));
        o4 = ap4(z, in, a);
        if (nst > 4) {
            o8 = ap4(z + 4, o4, a);
            if (nst > 8) {
                o12 = ap4(z + 8, o8, a);
                z[12] = o12;
            } else
                z[8] = o8;
        } else
            z[4] = o4;
        apo = cv[PH_P1] * o4 + cv[PH_P3] * o8 + cv[PH_P5] * o12;
        z[15] = apo;
        x[i] = (xi + inv * apo) * 0.5f;
    }
    END_LOOP
    RAMP_STORE(6)
    for (j = 0; j < 16; j++)
        s[j] = z[j];
    guard(s, 16);
}

/* filt_run(f, buf, buf, n) (L == R) runs one channel: a mono voice pays half. The R channel's states are kept and
 * copied to L's, so a later stereo call goes on smoothly (except COMB's L delay line, which the mono calls leave
 * as it was: reset the filter when a voice goes from mono to stereo). */
static const uint8_t NSTATE[FLT_N] = {4, 6, 4, 4, 7, 8, 2, 16};

RAMC void filt_run(filt_t *f, float *L, float *R, uint32_t n)
{
    int mono = L == R, i, ns, b;
    if (!(f->w & PRIMED))                /* no filt_ctl yet: pass through */
        return;
    switch (f->model) {                  /* (direct calls: the RAM-code check refuses calls through a pointer) */
    case FLT_ANALOG: if (!mono) run_analog(f, L, n, 0); run_analog(f, R, n, 1); break;
    case FLT_DIRTY: if (!mono) run_dirty(f, L, n, 0); run_dirty(f, R, n, 1); break;
    case FLT_LADDER: if (!mono) run_ladder(f, L, n, 0); run_ladder(f, R, n, 1); break;
    case FLT_DIGITAL: if (!mono) run_digital(f, L, n, 0); run_digital(f, R, n, 1); break;
    case FLT_DIODE: if (!mono) run_diode(f, L, n, 0); run_diode(f, R, n, 1); break;
    case FLT_FORMANT: if (!mono) run_formant(f, L, n, 0); run_formant(f, R, n, 1); break;
    case FLT_COMB: if (!mono) run_comb(f, L, n, 0); run_comb(f, R, n, 1); break;
    default: if (!mono) run_phaser(f, L, n, 0); run_phaser(f, R, n, 1); break;
    }
    if (mono) {
        ns = NSTATE[f->model];
        b = f->model == FLT_FORMANT ? 0 : 1;
        for (i = 0; i < ns; i++)
            f->s[b + i] = f->s[b + ns + i];
    }
}

/* ----------------------------------------------------------------- response --- */
/* main loop: the linear part of each model, evaluated at the note's frequency (s = j tan(w / 2), the bilinear
 * frequency, so the TPT stages are exact; the one-sample loops of DIRTY / DIODE / PHASER use z^-1). */
typedef struct {
    float re, im;
} cx;
static cx cxm(cx a, cx b) { cx r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; return r; }
static cx cxd(cx a, cx b)
{
    float d = b.re * b.re + b.im * b.im;
    cx r;
    d = d > 1e-30f ? 1.0f / d : 1e30f;
    r.re = (a.re * b.re + a.im * b.im) * d;
    r.im = (a.im * b.re - a.re * b.im) * d;
    return r;
}
static cx cxa(cx a, cx b) { cx r = {a.re + b.re, a.im + b.im}; return r; }
static cx cxs(cx a, cx b) { cx r = {a.re - b.re, a.im - b.im}; return r; }
static cx cxk(cx a, float k) { cx r = {a.re * k, a.im * k}; return r; }
static cx cxr(float re) { cx r = {re, 0.0f}; return r; }
static float cxabs(cx a) { return fm_sqrtf(a.re * a.re + a.im * a.im); }

typedef struct {
    cx s, zi;                            /* the bilinear s and z^-1 at the frequency */
    float w;                             /* radians per sample */
} fr_t;
/* the TPT one-pole low pass with coefficient G */
static cx h1(const fr_t *q, float G)
{
    float gp = G / (1.0f - G);
    return cxd(cxr(gp), cxa(q->s, cxr(gp)));
}
/* 2 z^-1 G / (1 + (2G - 1) z^-1): a TPT one-pole's state as the next sample reads it (Vital's getNextState) */
static cx fh(const fr_t *q, float G)
{
    return cxd(cxk(q->zi, 2.0f * G), cxa(cxr(1.0f), cxk(q->zi, 2.0f * G - 1.0f)));
}
/* the two-stage mix l lp + b bp + h hp of a cascade of two H */
static cx mix2(cx H, float l, float b, float h)
{
    cx H2 = cxm(H, H), bp = cxs(H, H2), hp = cxs(cxs(cxr(1.0f), H), bp);
    return cxa(cxa(cxk(H2, l), cxk(bp, b)), cxk(hp, h));
}

float filt_response(int model, int style, const filt_ctl_t *c, float note)
{
    fp_t p;
    fr_t q;
    float hz, *t = p.t;
    cx H, r;
    if (model < 0 || model >= FLT_N)
        return 1.0f;
    if (style < 0 || style >= FLT_NSTYLE[model])
        style = 0;
    PLAT_FAR(params)(model, style, c, clampf(c->cutoff, 0.0f, 150.0f), &p);
    hz = clampf(note_hz(note), 1.0f, 0.49f * SR);
    q.w = FM_TWO_PI * hz * ISR;
    q.s.re = 0.0f;
    q.s.im = fm_tanf(0.5f * q.w);
    q.zi.re = fm_cosf(q.w);
    q.zi.im = -fm_sinf(q.w);
    switch (model) {
    case FLT_ANALOG: {
        cx bp, in1;
        H = h1(&q, t[SK_G]);
        bp = cxs(H, cxm(H, H));
        in1 = cxd(cxr(p.a[0]), cxs(cxr(1.0f), cxk(bp, p.a[1])));
        r = cxm(mix2(H, t[SK_ML], t[SK_MB], t[SK_MH]), in1);
        if (style != ST_12)
            r = cxm(r, cxd(mix2(H, t[SK_LO], t[SK_BA], t[SK_HI]), cxs(cxr(1.0f), bp)));
        break;
    }
    case FLT_DIRTY: {
        float G = t[DT_G], rfm = t[DT_RFM];
        cx W, den;
        H = h1(&q, G);
        W = cxk(mix2(H, t[DT_ML], t[DT_MB], t[DT_MH]), t[DT_NORM]);
        den = cxs(cxr(1.0f), cxk(cxm(fh(&q, G), cxs(cxr(1.0f - G), H)), rfm));
        r = cxk(cxd(W, den), (t[DT_DRV] + rfm * (1.0f + G)) * (1.0f / 1.4f));
        if (style != ST_12)
            r = cxm(r, cxk(mix2(H, t[DT_LO], t[DT_BA], t[DT_HI]), t[DT_PN]));
        break;
    }
    case FLT_LADDER: {
        cx P = cxr(1.0f), sum = cxr(0.0f);
        int i;
        H = h1(&q, t[LD_G]);
        for (i = 0; i < 5; i++) {
            sum = cxa(sum, cxk(P, t[LD_T0 + i]));
            if (i < 4)
                P = cxm(P, H);
        }
        r = cxd(cxk(sum, p.a[0]), cxa(cxr(1.0f), cxk(P, p.a[1])));
        break;
    }
    case FLT_DIGITAL: {
        float g = p.a[0], k = p.a[1];
        cx s2 = cxm(q.s, q.s), lp, bp, den;
        den = cxa(cxa(s2, cxk(q.s, k * g)), cxr(g * g));
        lp = cxd(cxr(g * g), den);
        bp = cxd(cxk(q.s, g), den);
        r = cxa(cxa(cxr(t[SV_B20]), cxk(bp, t[SV_B21])), cxk(lp, t[SV_B22]));
        if (style != ST_12) {
            den = cxa(cxa(s2, cxk(q.s, g)), cxr(g * g));
            lp = cxd(cxr(g * g), den);
            bp = cxd(cxk(q.s, g), den);
            r = cxm(r, cxa(cxa(cxr(t[SV_B10]), cxk(bp, t[SV_B11])), cxk(lp, t[SV_B12])));
        }
        r = cxk(r, t[SV_DRV] * t[SV_POST]);
        break;
    }
    case FLT_DIODE: {
        float G = t[DD_G];
        cx Hh = h1(&q, t[DD_GH]), hp, A, FA, hl, c3, c2, c1, half = cxr(0.5f), one = cxr(1.0f), loopg;
        hp = cxd(cxm(cxs(one, Hh), cxs(one, Hh)), cxs(one, cxs(Hh, cxm(Hh, Hh))));
        hp = cxa(one, cxk(cxs(hp, one), t[DD_HPA]));
        A = h1(&q, G);
        FA = fh(&q, G);
        hl = cxs(one, h1(&q, G20));
        c3 = cxd(cxm(half, A), cxs(one, cxm(cxm(half, A), FA)));
        c2 = cxd(cxm(half, A), cxs(one, cxm(cxm(half, FA), c3)));
        c1 = cxd(cxm(A, hl), cxs(one, cxm(cxm(hl, FA), c2)));
        loopg = cxm(cxm(c3, c2), c1);
        r = cxd(cxk(hp, t[DD_DRV]), cxa(one, cxk(cxm(FA, loopg), t[DD_RES])));
        r = cxk(cxm(cxm(A, loopg), r), t[DD_POST]);
        break;
    }
    case FLT_FORMANT: {
        cx z2 = cxm(q.zi, q.zi), num = cxs(cxr(1.0f), z2);
        int i;
        r = cxr(0.0f);
        for (i = 0; i < 4; i++) {
            cx den = cxa(cxa(cxr(1.0f), cxk(q.zi, t[4 * i + 1])), cxk(z2, t[4 * i + 2]));
            r = cxa(r, cxk(cxd(num, den), t[4 * i] * t[4 * i + 3]));
        }
        break;
    }
    case FLT_COMB: {
        int fb = COMB_FB[style];
        float D = fb == 2 ? t[CB_PER] * 0.5f : t[CB_PER];
        cx L1 = h1(&q, t[CB_G1]), F, zd, one = cxr(1.0f);
        F = cxa(cxk(L1, t[CB_LO]), cxk(cxs(one, L1), t[CB_HI]));
        F = cxm(F, cxs(one, h1(&q, t[CB_G2])));
        zd.re = fm_cosf(q.w * D);
        zd.im = -fm_sinf(q.w * D);
        if (fb == 0) {
            r = cxd(cxk(F, t[CB_SCALE]), cxs(one, cxk(cxm(F, zd), t[CB_FB])));
        } else {
            float sg = fb == 1 ? 1.0f : -1.0f;
            cx FZ = cxm(F, zd), push = cxd(cxr(0.70710678f), cxs(one, cxk(FZ, sg * t[CB_FB])));
            r = cxa(cxr(0.70710678f * t[CB_SCALE]), cxk(cxm(FZ, push), sg));
        }
        break;
    }
    default: {                           /* PHASER */
        cx ap = cxd(cxs(cxr(t[PH_A]), q.zi), cxs(cxr(1.0f), cxk(q.zi, t[PH_A]))), a4, a8, a12, P, bpf, one = cxr(1.0f);
        a4 = cxm(cxm(ap, ap), cxm(ap, ap));
        a8 = cxm(a4, a4);
        a12 = cxm(a8, a4);
        P = cxa(cxa(cxk(a4, t[PH_P1]), cxk(a8, t[PH_P3])), cxk(a12, t[PH_P5]));
        bpf = cxm(h1(&q, p.a[0]), cxs(one, h1(&q, p.a[1])));
        r = cxd(cxk(P, t[PH_DRV]), cxs(one, cxk(cxm(cxm(P, bpf), q.zi), t[PH_RES])));
        r = cxk(cxa(one, cxk(r, p.a[2])), 0.5f);
        break;
    }
    }
    hz = cxabs(r);
    return hz < 16.0f ? hz : 16.0f;      /* (a self-oscillating setting has no linear response: a tall peak) */
}
