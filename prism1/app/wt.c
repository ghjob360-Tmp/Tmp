/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1's wavetable builder (main loop). A frame is a spectrum of 127 harmonics, each as sine and cosine
 * coefficients; a spectral morph reshapes it; then each mip level is the sum of the harmonics it can hold
 * (an inverse DFT by table lookup), scaled so level 0 peaks at 0.9.
 * The spectral morph names follow Vital's (Matt Tytel, GPL-3.0); the arithmetic here is this firmware's own. */
#include "plat.h"
#include "fastmath.h"
#include "wt.h"

static float SIN[256];
static int sin_ready;

static void sin_init(void)
{
    int i;
    for (i = 0; i < 256; i++)
        SIN[i] = fm_sin_turns((float)i / 256.0f);
    sin_ready = 1;
}

static uint32_t wt_rng;
static float wt_rnd(void)                /* -1 .. 1, deterministic */
{
    wt_rng = wt_rng * 1664525u + 1013904223u;
    return (float)(int32_t)wt_rng * (1.0f / 2147483648.0f);
}

/* ---------------------------------------------------------------- recipes --- */
/* the frame's spectrum at position t (0..1 across the table): s[h], c[h] for h = 1..127 (index h) */
static void recipe(int table, int frame, float *s, float *c)
{
    float t = (float)frame / (float)(WT_FRAMES - 1);
    int h;
    for (h = 0; h <= WT_HARM; h++)
        s[h] = c[h] = 0.0f;
    switch (table) {
    default:
    case WT_BASIC: {                     /* sine -> triangle -> square -> saw */
        float x = t * 3.0f, k[4];
        int i = (int)x;
        if (i > 2)
            i = 2;
        x -= (float)i;
        for (h = 1; h <= WT_HARM; h++) {
            float odd = (float)(h & 1), sgn = ((h - 1) / 2) & 1 ? -1.0f : 1.0f;
            k[0] = h == 1 ? 1.0f : 0.0f;
            k[1] = odd * sgn * 0.8106f / (float)(h * h);
            k[2] = odd * 1.2732f / (float)h;
            k[3] = (h & 1 ? 0.6366f : -0.6366f) / (float)h;
            s[h] = k[i] + (k[i + 1] - k[i]) * x;
        }
        break;
    }
    case WT_PULSE: {                     /* a pulse narrowing from 50 % to 5 % */
        float w = 0.5f - 0.45f * t;
        for (h = 1; h <= WT_HARM; h++) {
            float a = 0.6366f / (float)h;
            s[h] = a * (1.0f - fm_sin_turns(((float)h * w) + 0.25f));
            c[h] = a * fm_sin_turns((float)h * w);
        }
        break;
    }
    case WT_FORMANT: {                   /* a buzz through vowel formants A E I O U (an f0 of 140 Hz assumed) */
        static const float F[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {350, 2300, 3000},
                                      {450, 800, 2830}, {325, 700, 2530}};
        float x = t * 4.0f, f[3];
        int i = (int)x, j;
        if (i > 3)
            i = 3;
        x -= (float)i;
        for (j = 0; j < 3; j++)
            f[j] = F[i][j] + (F[i + 1][j] - F[i][j]) * x;
        for (h = 1; h <= WT_HARM; h++) {
            float fh = 140.0f * (float)h, e = 0.015f;
            for (j = 0; j < 3; j++) {
                float d = (fh - f[j]) / (80.0f + 40.0f * (float)j);
                e += (j ? 0.5f : 1.0f) / (1.0f + d * d);
            }
            s[h] = e * fm_rsqrtf((float)h);
        }
        break;
    }
    case WT_HARMONIC: {                  /* a soft saw with a peak sweeping up the harmonics */
        float pk = 2.0f + t * 22.0f;
        for (h = 1; h <= WT_HARM; h++) {
            float d = ((float)h - pk) * (1.0f / 1.6f);
            s[h] = 0.25f / (float)h + fm_expf(-0.5f * d * d) * fm_rsqrtf((float)h);
        }
        break;
    }
    case WT_ORGAN: {                     /* drawbar registrations: 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' */
        static const uint8_t HB[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
        static const char *const REG[WT_FRAMES] = {"808000000", "888000000", "868000004", "888800000",
                                                   "688600000", "848000888", "888888888", "800000888"};
        int j;
        for (j = 0; j < 9; j++)
            s[HB[j]] += (float)(REG[frame][j] - '0') * (1.0f / 8.0f);
        break;
    }
    case WT_DIGITAL:                     /* random harmonics, a fresh draw each frame */
        wt_rng = 0x9E3779B9u + (uint32_t)frame * 7919u;
        for (h = 1; h <= WT_HARM; h++) {
            float a = fm_powf((float)h, -0.65f);
            if (wt_rnd() < -0.2f)
                continue;
            s[h] = wt_rnd() * a;
            c[h] = wt_rnd() * a * 0.6f;
        }
        break;
    }
}

/* ----------------------------------------------------------------- morphs --- */
static void morph_apply(int morph, float a, int table, float *s, float *c)
{
    static float ts[WT_HARM + 1], tc[WT_HARM + 1];
    int h;
    switch (morph) {
    case SM_LOWPASS: {                   /* harmonics above a cutoff fade */
        float hc = 1.0f + 126.0f * (1.0f - a) * (1.0f - a);
        for (h = 1; h <= WT_HARM; h++)
            if ((float)h > hc) {
                float g = fm_expf(-((float)h - hc) / (0.5f + hc * 0.15f));
                s[h] *= g;
                c[h] *= g;
            }
        break;
    }
    case SM_HIGHPASS: {
        float hc = 1.0f + 48.0f * a * a;
        for (h = 1; h <= WT_HARM; h++)
            if ((float)h < hc) {
                float g = (float)h / hc;
                g *= g;
                s[h] *= g * g;
                c[h] *= g * g;
            }
        break;
    }
    case SM_FORMANT:                     /* the spectrum's shape moved, the harmonics staying where they are */
    case SM_STRETCH: {                   /* the harmonics themselves moved up (inharmonic in Vital; rounded here) */
        float sh = fm_exp2f((a * 2.0f - 1.0f) * 1.5f);
        for (h = 0; h <= WT_HARM; h++)
            ts[h] = tc[h] = 0.0f;
        for (h = 1; h <= WT_HARM; h++) {
            if (morph == SM_FORMANT) {
                float src = (float)h / sh, f;
                int i = (int)src;
                if (i < 1 || i >= WT_HARM)
                    continue;
                f = src - (float)i;
                ts[h] = s[i] + (s[i + 1] - s[i]) * f;
                tc[h] = c[i] + (c[i + 1] - c[i]) * f;
            } else {
                int d = (int)((float)h * (1.0f + a * 2.0f) + 0.5f);
                if (d <= WT_HARM) {
                    ts[d] += s[h];
                    tc[d] += c[h];
                }
            }
        }
        for (h = 1; h <= WT_HARM; h++) {
            s[h] = ts[h];
            c[h] = tc[h];
        }
        break;
    }
    case SM_SMEAR: {                     /* each harmonic leaks into the next ones */
        float ps = 0.0f, pc = 0.0f;
        for (h = 1; h <= WT_HARM; h++) {
            ps = s[h] = (1.0f - a) * s[h] + a * 0.92f * ps;
            pc = c[h] = (1.0f - a) * c[h] + a * 0.92f * pc;
        }
        break;
    }
    case SM_RANDOM:                      /* every harmonic's level jittered (the same draw each time) */
        wt_rng = 0x51EDu + (uint32_t)table;
        for (h = 1; h <= WT_HARM; h++) {
            float g = 1.0f + a * 1.5f * wt_rnd();
            s[h] *= g;
            c[h] *= g;
        }
        break;
    case SM_DISPERSE:                    /* phases rotated by the square of the harmonic: a chirp in each cycle */
        for (h = 1; h <= WT_HARM; h++) {
            float th = a * 0.02f * (float)(h * h), co = fm_sin_turns((th) + 0.25f), si = fm_sin_turns(th), x = s[h];
            s[h] = x * co - c[h] * si;
            c[h] = x * si + c[h] * co;
        }
        break;
    case SM_SHEPARD:                     /* an octave below blended in */
        for (h = WT_HARM; h >= 1; h--) {
            float ls = (h & 1) ? 0.0f : s[h / 2], lc = (h & 1) ? 0.0f : c[h / 2];
            s[h] = (1.0f - a) * s[h] + a * ls;
            c[h] = (1.0f - a) * c[h] + a * lc;
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ build --- */
void wt_build(wt_t *dst, int table, int morph, float amount)
{
    static float s[WT_HARM + 2], c[WT_HARM + 2];
    int f, l;
    if (!sin_ready)
        sin_init();
    for (f = 0; f < WT_FRAMES; f++) {
        float peak = 0.0f, g;
        recipe(table, f, s, c);
        morph_apply(morph, amount, table, s, c);
        s[WT_HARM + 1] = c[WT_HARM + 1] = 0.0f;
        for (l = 0; l < WT_LEVELS; l++) {
            uint32_t n = 256u >> l, H = n / 2u - 1u, stride = 1u << l, i, h;
            float *o = &dst->d[f][WT_OFF[l]];
            for (i = 0; i < n; i++) {
                float v = 0.0f;
                for (h = 1; h <= H; h++) {
                    uint32_t k = (h * i * stride) & 255u;
                    v += s[h] * SIN[k] + c[h] * SIN[(k + 64u) & 255u];
                }
                o[i] = v;
                if (l == 0 && fm_fabsf(v) > peak)
                    peak = fm_fabsf(v);
            }
        }
        g = peak > 1e-6f ? 0.9f / peak : 0.0f;
        for (l = 0; l < (int)WT_SIZE; l++)
            dst->d[f][l] *= g;
    }
}

float wt_peek(const wt_t *t, float fp, float ph)
{
    int f = (int)fp, i = (int)(ph * 256.0f) & 255;
    float x = fp - (float)f;
    if (f >= WT_FRAMES - 1) {
        f = WT_FRAMES - 2;
        x = 1.0f;
    }
    return t->d[f][i] + (t->d[f + 1][i] - t->d[f][i]) * x;
}
