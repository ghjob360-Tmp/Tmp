/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the master section's EQ and output stage (contract in fx.h).
 * master_t needs no reset: the core zeroes it once (static storage); the first call snaps the smoothers.
 *  - s[0..23]: EQ (knobs, the 3 x 2 x 2 filter states, the init mark)
 *  - s[24..47]: out (knobs, detector, gains, the init mark) */
#include "fx.h"

#define LOG2_10_20 0.166096405f          /* dB -> log2 */
#define MARK 1.0f

enum { EQ_K = 0, EQ_ST = 4, EQ_INIT = 16 };
enum { OU_K = 24, OU_ENV = 28, OU_G, OU_PRE, OU_LL, OU_LR, OU_INIT };

/* trapezoidal SVF with a mixed output: y = m0 x + m1 band + m2 low (bell and shelves); c = a1, a2, a3, m0, m1, m2 */
INL float eq_tick(float *st, const float *c, float x)
{
    float v3 = x - st[1], v1 = c[0] * st[0] + c[1] * v3, v2 = st[1] + c[1] * st[0] + c[2] * v3;
    st[0] = 2.0f * v1 - st[0];
    st[1] = 2.0f * v2 - st[1];
    return c[3] * x + c[4] * v1 + c[5] * v2;
}
INL void eq_coef(float *c, float g, float k)
{
    float a1 = 1.0f / (1.0f + g * (g + k));
    c[0] = a1;
    c[1] = g * a1;
    c[2] = g * g * a1;
}

static RAMC void smooth4(float *s, uint32_t o, const float *k, uint32_t n, uint32_t init)
{
    uint32_t i;
    float a = (float)n * (1.0f / (0.03f * SR));
    if (a > 1.0f || s[init] != MARK)
        a = 1.0f;
    s[init] = MARK;
    for (i = 0; i < 4; i++)
        s[o + i] += (clampf(k[i], 0.0f, 1.0f) - s[o + i]) * a;
}

/* low shelf 150 Hz, bell 1 kHz (Q 0.9), high shelf 6 kHz; +-12 dB around 0.5, all scaled by the amount */
RAMC void eq_run(master_t *m, const float k[4], float *L, float *R, uint32_t n)
{
    float *s = m->s, cl[6], cm[6], ch[6], amt, db, A, sA, t, kk;
    uint32_t i;

    smooth4(s, EQ_K, k, n, EQ_INIT);
    amt = s[EQ_K + 3] * 24.0f;
    /* low shelf */
    db = (s[EQ_K] - 0.5f) * amt;
    A = fm_exp2f(db * (0.5f * LOG2_10_20));                 /* 10^(dB/40) */
    sA = fm_exp2f(db * (0.25f * LOG2_10_20));
    t = fm_tanf(FM_PI * 150.0f * ISR);
    kk = 1.41421356f;
    eq_coef(cl, t / sA, kk);
    cl[3] = 1.0f;
    cl[4] = kk * (A - 1.0f);
    cl[5] = A * A - 1.0f;
    /* bell */
    db = (s[EQ_K + 1] - 0.5f) * amt;
    A = fm_exp2f(db * (0.5f * LOG2_10_20));
    kk = 1.0f / (0.9f * A);
    eq_coef(cm, fm_tanf(FM_PI * 1000.0f * ISR), kk);
    cm[3] = 1.0f;
    cm[4] = kk * (A * A - 1.0f);
    cm[5] = 0.0f;
    /* high shelf */
    db = (s[EQ_K + 2] - 0.5f) * amt;
    A = fm_exp2f(db * (0.5f * LOG2_10_20));
    sA = fm_exp2f(db * (0.25f * LOG2_10_20));
    kk = 1.41421356f;
    eq_coef(ch, fm_tanf(FM_PI * 6000.0f * ISR) * sA, kk);
    ch[3] = A * A;
    ch[4] = kk * (1.0f - A) * A;
    ch[5] = 1.0f - A * A;

    for (i = 0; i < n; i++) {
        float l = L[i], r = R[i];
        l = eq_tick(s + EQ_ST, cl, l);
        r = eq_tick(s + EQ_ST + 2, cl, r);
        l = eq_tick(s + EQ_ST + 4, cm, l);
        r = eq_tick(s + EQ_ST + 6, cm, r);
        L[i] = eq_tick(s + EQ_ST + 8, ch, l);
        R[i] = eq_tick(s + EQ_ST + 10, ch, r);
    }
    for (i = EQ_ST; i < EQ_ST + 12u; i++)
        s[i] = fm_flush(s[i]);
}

/* transparent below 0.8, a tanh knee to +-1 */
INL float knee_clip(float x)
{
    float a = fm_fabsf(x);
    if (a <= 0.8f)
        return x;
    a = 0.8f + 0.2f * fm_tanhf((a - 0.8f) * 5.0f);
    return x < 0.0f ? -a : a;
}

/* level: 0.8 = unity, 1 = +3.9 dB (square law). drive: 0 = clean; up: +24 dB into a compressor whose threshold
 * falls to -9 dBFS and whose ratio goes to a limit, faster attack, make-up, then tanh saturation (drive^2). release:
 * 30 ms .. 1.45 s. *gr_out: the largest gain reduction of the call, in dB (>= 0). */
RAMC void out_run(master_t *m, const float k[4], float *L, float *R, uint32_t n, float *gr_out)
{
    float *s = m->s, d, pre, dpre, T, sl, mk, ca, cr, sat, sg, isg, env, g, ll, dll, lr, dlr, grmax = 0.0f;
    uint32_t i, j, c;
    int first = s[OU_INIT] != MARK;

    smooth4(s, OU_K, k, n, OU_INIT);
    d = s[OU_K + 2];
    T = -1.0f - 8.0f * d;
    sl = d < 0.5f ? 1.6f * d : 0.8f + 0.4f * (d - 0.5f);       /* slope 0 .. 1 (limit) */
    ca = 1.0f - fm_exp2f(-FM_LOG2E / ((0.002f - 0.0017f * d) * SR));
    cr = 1.0f - fm_exp2f(-FM_LOG2E / (0.03f * fm_exp2f(s[OU_K + 3] * 5.6f) * SR));
    sat = d * d;
    sg = 1.0f + 2.0f * d;
    isg = 1.0f / fm_tanhf(sg);                                /* full scale stays full scale: louder below */
    mk = -T * sl * 0.85f;                                     /* make-up, dB: denser and louder */
    if (first) {
        s[OU_G] = fm_exp2f(mk * LOG2_10_20);
        s[OU_PRE] = fm_exp2f(d * 4.0f);
        s[OU_LL] = s[OU_K] * s[OU_K] * 1.5625f;
        s[OU_LR] = s[OU_K + 1] * s[OU_K + 1] * 1.5625f;
    }
    pre = s[OU_PRE];
    dpre = (fm_exp2f(d * 4.0f) - pre) / (float)n;
    ll = s[OU_LL];
    dll = (s[OU_K] * s[OU_K] * 1.5625f - ll) / (float)n;
    lr = s[OU_LR];
    dlr = (s[OU_K + 1] * s[OU_K + 1] * 1.5625f - lr) / (float)n;
    env = s[OU_ENV];
    g = s[OU_G];
    for (j = 0; j < n; j += 8u) {
        float lv = 6.0206f * fm_log2f(env + 1e-6f), ov = lv - T, gr, dg;
        if (ov <= -3.0f)
            gr = 0.0f;
        else if (ov < 3.0f)
            gr = -sl * (ov + 3.0f) * (ov + 3.0f) * (1.0f / 12.0f);
        else
            gr = -sl * ov;
        if (-gr > grmax)
            grmax = -gr;
        c = n - j < 8u ? n - j : 8u;
        dg = (fm_exp2f((gr + mk) * LOG2_10_20) - g) / (float)c;
        for (i = j; i < j + c; i++) {
            float l, r, pk;
            pre += dpre;
            ll += dll;
            lr += dlr;
            g += dg;
            l = L[i] * pre;
            r = R[i] * pre;
            pk = fm_fabsf(l) > fm_fabsf(r) ? fm_fabsf(l) : fm_fabsf(r);
            env += (pk - env) * (pk > env ? ca : cr);
            l *= g;
            r *= g;
            if (sat > 0.0001f) {
                l += (fm_tanhf(l * sg) * isg - l) * sat;
                r += (fm_tanhf(r * sg) * isg - r) * sat;
            }
            L[i] = knee_clip(l * ll);
            R[i] = knee_clip(r * lr);
        }
    }
    s[OU_ENV] = fm_flush(env);
    s[OU_G] = g;
    s[OU_PRE] = pre;
    s[OU_LL] = ll;
    s[OU_LR] = lr;
    if (gr_out)
        *gr_out = grmax;
}
