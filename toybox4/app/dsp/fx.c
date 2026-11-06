/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the nine effects (contract in fx.h).
 *
 * Common ground:
 *  - s[0..7] are the eight knobs, smoothed (one-pole per call, ~25 ms); derived gains are ramped per sample
 *    across the call, delay times glide per sample, filter coefficients are refreshed per call (or per 8 / 16
 *    frames where a follower moves them), so nothing zippers.
 *  - u[15] marks "initialised" (the first run snaps the smoothers), u[14] is the noise generator.
 *  - delay memory is the arena, int16 at 16384 = 1.0 (+-2.0 range), written with saturation and truncation toward
 *    zero (a recirculating tail always dies out instead of sticking at one LSB). Every layout is derived from
 *    arena_n each call, so a track slot (12288) gets shorter lines than the master slot (32768).
 *  - every loop that feeds back is passive (orthogonal mixing, gains < 1, one-pole filters) or soft-clipped. */
#include "fx.h"

#define AS 16384.0f
#define IAS (1.0f / 16384.0f)
#define U_RNG 14u
#define U_INIT 15u
#define INIT_MAGIC 0x7B0Bu
#define LOG2_10_20 0.166096405f          /* dB -> log2 */

/* ---------------------------------------------------------------------------------------------- helpers --- */
INL void aw(int16_t *a, uint32_t i, float x)            /* saturating write */
{
    float y = x * AS;
    if (y > 32767.0f)
        y = 32767.0f;
    if (y < -32767.0f)
        y = -32767.0f;
    a[i] = (int16_t)(int32_t)y;
}
INL float ar(const int16_t *a, uint32_t i) { return (float)a[i] * IAS; }
/* read d (>= 0, < len - 1) samples behind write position w, linear interpolation */
INL float arf(const int16_t *a, uint32_t len, uint32_t w, float d)
{
    int32_t di = (int32_t)d, i0, i1;
    float fr = d - (float)di, x0, x1;
    i0 = (int32_t)w - di;
    if (i0 < 0)
        i0 += (int32_t)len;
    i1 = i0 - 1;
    if (i1 < 0)
        i1 += (int32_t)len;
    x0 = (float)a[i0];
    x1 = (float)a[i1];
    return (x0 + (x1 - x0) * fr) * IAS;
}
INL float ari(const int16_t *a, uint32_t len, uint32_t w, uint32_t d)   /* integer read, d < len */
{
    int32_t i = (int32_t)w - (int32_t)d;
    if (i < 0)
        i += (int32_t)len;
    return (float)a[i] * IAS;
}
INL uint32_t winc(uint32_t w, uint32_t len) { return ++w >= len ? 0u : w; }
/* cubic soft clip: unity slope at 0, reaches +-1 with zero slope at +-1.5 */
INL float sclip(float x)
{
    x = x < -1.5f ? -1.5f : x > 1.5f ? 1.5f : x;
    return x - 0.148148148f * x * x * x;
}
/* transparent below 0.8, then a tanh knee to +-1 */
INL float kclip(float x)
{
    float a = fm_fabsf(x);
    if (a <= 0.8f)
        return x;
    a = 0.8f + 0.2f * fm_tanhf((a - 0.8f) * 5.0f);
    return x < 0.0f ? -a : a;
}
INL float op_coef(float fc) { return 1.0f - fm_exp2f(-fc * (FM_TWO_PI * FM_LOG2E * ISR)); }   /* one-pole */
INL float rt_gain(float len, float rt60) { return fm_exp2f(-9.96578428f * len / (rt60 * SR)); }
INL float mix_dry(float m) { return m < 0.5f ? 1.0f : 2.0f - 2.0f * m; }
INL float mix_wet(float m) { return m > 0.5f ? 1.0f : 2.0f * m; }
INL float sstep(float t) { return t * t * (3.0f - 2.0f * t); }
INL float lexp(float lo, float hi, float t) { return lo * fm_exp2f(t * fm_log2f(hi / lo)); }   /* lo..hi, exp */

/* TPT state-variable filter: c = {a1, a2, a3, k}; st = two integrators; returns low-pass, *bp = band-pass */
INL void svf_set(float *c, float fc, float k)
{
    float g = fm_tanf(FM_PI * clampf(fc, 10.0f, 19000.0f) * ISR), a1 = 1.0f / (1.0f + g * (g + k));
    c[0] = a1;
    c[1] = g * a1;
    c[2] = g * g * a1;
    c[3] = k;
}
INL float svf_run(float *st, const float *c, float x, float *bp)
{
    float v3 = x - st[1], v1 = c[0] * st[0] + c[1] * v3, v2 = st[1] + c[1] * st[0] + c[2] * v3;
    st[0] = 2.0f * v1 - st[0];
    st[1] = 2.0f * v2 - st[1];
    *bp = v1;
    return v2;
}
INL void flush(float *s, uint32_t a, uint32_t b)
{
    uint32_t i;
    for (i = a; i < b; i++)
        s[i] = fm_flush(s[i]);
}

/* smooth the knobs; returns 1 on the first run after a reset (the caller snaps its own ramps) */
INL int fx_begin(fx_t *f, const float *k)
{
    uint32_t i;
    if (f->u[U_INIT] != INIT_MAGIC) {
        f->u[U_INIT] = INIT_MAGIC;
        if (!f->u[U_RNG])
            f->u[U_RNG] = 0x2545F491u;
        for (i = 0; i < 8; i++)
            f->s[i] = clampf(k[i], 0.0f, 1.0f);
        return 1;
    }
    return 0;
}
INL void fx_smooth(fx_t *f, const float *k, uint32_t n)
{
    uint32_t i;
    float a = (float)n * (1.0f / (0.025f * SR));
    if (a > 1.0f)
        a = 1.0f;
    for (i = 0; i < 8; i++)
        f->s[i] += (clampf(k[i], 0.0f, 1.0f) - f->s[i]) * a;
}
/* a ramp over the call from the value stored in s[i] (*cur) to tgt (stored for the next call); returns the step */
INL float ramp(float *s, uint32_t i, float tgt, int first, float inv_n, float *cur)
{
    float c = first ? tgt : s[i];
    *cur = c;
    s[i] = tgt;
    return (tgt - c) * inv_n;
}

/* ================================================================================================ ECHO === */
/* stereo tape echo, decimated: two lines of arena_n / 2 at half rate (arena >= 16384: 0.74 s at 32768) or quarter
 * rate (smaller arenas: 0.28 s at 6144, 0.19 s at 4096); the write is a box average, the read interpolates */
enum { EC_DSM = 8, EC_PH1, EC_PH2, EC_NZ, EC_MOD, EC_ACCL, EC_ACCR, EC_LPL, EC_LPR, EC_HPL, EC_HPR, EC_ENV, EC_DRY,
       EC_WET, EC_FB, EC_PP };

static RAMC void fx_echo(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s;
    uint32_t B = f->arena_n >> 1, pos = f->u[0], ph = f->u[1], i, dec = f->arena_n >= 16384u ? 2u : 4u;
    int16_t *la = f->arena, *ra = f->arena + B;
    float dmax = (float)(dec * B) - 16.0f, dmin = 0.02f * SR, idec = 1.0f / (float)dec;
    float dt = lexp(dmin, dmax * 0.92f, s[0]) * fm_exp2f((s[6] - 0.5f) * 0.4f);
    float rate = 0.15f * fm_exp2f(s[1] * 5.0f), depth = s[1] * 60.0f / rate, dtn = (float)n * ISR;
    float clp = op_coef(fm_minf(1500.0f * fm_exp2f(s[4] * 3.4f), dec == 2u ? 16000.0f : 5000.0f)), chp = op_coef(25.0f * fm_exp2f(s[4] * 4.7f));
    float duck = s[7] * 0.9f, gl = 1.0f / (0.1f * SR), dhmax = (float)(B - 3u);
    float dsm, mod, dmod, dry, ddry, wet, dwet, fb, dfb, pp, dpp;
    float accl = s[EC_ACCL], accr = s[EC_ACCR], lpl = s[EC_LPL], lpr = s[EC_LPR], hpl = s[EC_HPL], hpr = s[EC_HPR];
    float env = s[EC_ENV];

    dt = clampf(dt, 16.0f, dmax * 0.94f);
    if (depth > 0.2f * dt)
        depth = 0.2f * dt;
    if (first)
        s[EC_DSM] = dt;
    s[EC_PH1] = frac1(s[EC_PH1] + rate * dtn);
    s[EC_PH2] = frac1(s[EC_PH2] + rate * 5.3f * dtn);
    s[EC_NZ] = s[EC_NZ] * 0.99f + rnd1(&f->u[U_RNG]) * 0.05f;
    dmod = ramp(s, EC_MOD, depth * (fm_sin_turns(s[EC_PH1]) + 0.08f * fm_sin_turns(s[EC_PH2]) + 0.6f * s[EC_NZ]),
                first, inv_n, &mod);
    ddry = ramp(s, EC_DRY, mix_dry(s[3]), first, inv_n, &dry);
    dwet = ramp(s, EC_WET, mix_wet(s[3]), first, inv_n, &wet);
    dfb = ramp(s, EC_FB, s[2] * 1.04f, first, inv_n, &fb);
    dpp = ramp(s, EC_PP, s[5], first, inv_n, &pp);
    dsm = s[EC_DSM];

    for (i = 0; i < n; i++) {
        float xl = L[i], xr = R[i], dh, yl, yr, fl, fr, il, ir, a;
        dsm += (dt - dsm) * gl;
        mod += dmod;
        dry += ddry;
        wet += dwet;
        fb += dfb;
        pp += dpp;
        dh = (dsm + mod - (float)ph) * idec;      /* line units: the read point moves smoothly */
        dh = clampf(dh, 1.0f, dhmax);
        yl = arf(la, B, pos, dh);
        yr = arf(ra, B, pos, dh);
        lpl += (yl - lpl) * clp;                  /* tone: low-pass, minus a low-pass = band */
        lpr += (yr - lpr) * clp;
        hpl += (lpl - hpl) * chp;
        hpr += (lpr - hpr) * chp;
        yl = lpl - hpl;
        yr = lpr - hpr;
        fl = sclip((yl + (yr - yl) * pp) * fb);   /* ping-pong: cross feedback */
        fr = sclip((yr + (yl - yr) * pp) * fb);
        il = xl + (0.5f * (xl + xr) - xl) * pp;    /* ping-pong: the input enters on the left only */
        ir = xr - xr * pp;
        accl += il + fl;
        accr += ir + fr;
        if (++ph >= dec) {
            aw(la, pos, accl * idec);
            aw(ra, pos, accr * idec);
            accl = accr = 0.0f;
            pos = winc(pos, B);
            ph = 0;
        }
        a = fm_fabsf(xl) + fm_fabsf(xr);
        env += (a - env) * (a > env ? 0.005f : 0.0001f);
        a = 1.0f - duck * (env * 2.0f > 1.0f ? 1.0f : env * 2.0f);
        a *= wet;
        L[i] = xl * dry + yl * a;
        R[i] = xr * dry + yr * a;
    }
    s[EC_DSM] = dsm;
    s[EC_ACCL] = accl;
    s[EC_ACCR] = accr;
    s[EC_LPL] = fm_flush(lpl);
    s[EC_LPR] = fm_flush(lpr);
    s[EC_HPL] = fm_flush(hpl);
    s[EC_HPR] = fm_flush(hpr);
    s[EC_ENV] = fm_flush(env);
    f->u[0] = pos;
    f->u[1] = ph;
}

/* ================================================================================================ CUBE === */
/* mono pre-delay (arena / 8) -> two allpass diffusers -> 4-line FDN, Hadamard mixed; line lengths from x and y */
enum { CU_D0 = 8, CU_LP0 = 12, CU_DPRE = 16, CU_LFO, CU_DRY, CU_WET, CU_WID };

static RAMC void fx_cube(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s;
    uint32_t P = f->arena_n >> 3, B = (f->arena_n - P - 768u) >> 2, pos = f->u[0], pp = f->u[1], c = f->u[2], i, j;
    int16_t *pre = f->arena, *ap1 = f->arena + P, *ap2 = ap1 + 256, *ln = ap2 + 512;
    float sx = 0.06f + 0.94f * s[0] * s[0], sy = 0.06f + 0.94f * s[1] * s[1], bm = (float)B - 30.0f;
    float rt = 0.2f * fm_exp2f(s[2] * 6.5f), clp = op_coef(18000.0f * fm_exp2f(-s[4] * 4.5f));
    float md = s[5] * 20.0f, lfo, tg[4], dd[4], d[4], g[4], lp[4];
    float dpre, ddpre, dry, ddry, wet, dwet, wid, dwid;

    s[CU_LFO] = frac1(s[CU_LFO] + (0.3f + s[5] * 1.2f) * (float)n * ISR);
    lfo = s[CU_LFO];
    tg[0] = bm * sx;
    tg[1] = bm * sy * 0.871f;
    tg[2] = bm * fm_sqrtf(sx * sy) * 0.743f;
    tg[3] = bm * (sx + sy) * 0.5f * 0.619f;
    for (j = 0; j < 4; j++) {
        tg[j] = clampf(tg[j] + md * fm_sin_turns(lfo + 0.25f * (float)j), 2.0f, (float)B - 3.0f);
        dd[j] = ramp(s, CU_D0 + j, tg[j], first, inv_n, &d[j]);
        g[j] = rt_gain(tg[j], rt);
        lp[j] = s[CU_LP0 + j];
    }
    ddpre = ramp(s, CU_DPRE, 1.0f + s[6] * s[6] * (float)(P - 3u), first, inv_n, &dpre);
    ddry = ramp(s, CU_DRY, mix_dry(s[3]), first, inv_n, &dry);
    dwet = ramp(s, CU_WET, mix_wet(s[3]) * 0.7f, first, inv_n, &wet);
    dwid = ramp(s, CU_WID, s[7] * 2.0f, first, inv_n, &wid);

    for (i = 0; i < n; i++) {
        float x, v, o, y0, y1, y2, y3, t0, t1, t2, t3, h0, h1, h2, h3, m, sd;
        dpre += ddpre;
        dry += ddry;
        wet += dwet;
        wid += dwid;
        d[0] += dd[0];
        d[1] += dd[1];
        d[2] += dd[2];
        d[3] += dd[3];
        aw(pre, pp, 0.5f * (L[i] + R[i]));
        x = arf(pre, P, pp, dpre);
        pp = winc(pp, P);
        o = ar(ap1, (c - 142u) & 255u);           /* diffusers */
        v = x - 0.62f * o;
        aw(ap1, c & 255u, v);
        x = o + 0.62f * v;
        o = ar(ap2, (c - 377u) & 511u);
        v = x - 0.62f * o;
        aw(ap2, c & 511u, v);
        x = (o + 0.62f * v) * 0.5f;
        c++;
        y0 = arf(ln, B, pos, d[0]);
        y1 = arf(ln + B, B, pos, d[1]);
        y2 = arf(ln + 2u * B, B, pos, d[2]);
        y3 = arf(ln + 3u * B, B, pos, d[3]);
        lp[0] += (y0 - lp[0]) * clp;
        lp[1] += (y1 - lp[1]) * clp;
        lp[2] += (y2 - lp[2]) * clp;
        lp[3] += (y3 - lp[3]) * clp;
        t0 = lp[0] * g[0];
        t1 = lp[1] * g[1];
        t2 = lp[2] * g[2];
        t3 = lp[3] * g[3];
        h0 = 0.5f * ((t0 + t1) + (t2 + t3));
        h1 = 0.5f * ((t0 - t1) + (t2 - t3));
        h2 = 0.5f * ((t0 + t1) - (t2 + t3));
        h3 = 0.5f * ((t0 - t1) - (t2 - t3));
        aw(ln, pos, sclip(h0 + x));
        aw(ln + B, pos, sclip(h1 - x));
        aw(ln + 2u * B, pos, sclip(h2 + x));
        aw(ln + 3u * B, pos, sclip(h3 - x));
        pos = winc(pos, B);
        m = 0.5f * ((y0 - y2) + (y1 - y3));       /* stereo out: two independent sums, then width */
        sd = 0.5f * ((y0 - y2) - (y1 - y3)) * wid;
        L[i] = L[i] * dry + (m + sd) * wet;
        R[i] = R[i] * dry + (m - sd) * wet;
    }
    for (j = 0; j < 4; j++) {
        s[CU_D0 + j] = tg[j];
        s[CU_LP0 + j] = fm_flush(lp[j]);
    }
    f->u[0] = pos;
    f->u[1] = pp;
    f->u[2] = c;
}

/* =============================================================================================== TURBO === */
/* drive -> SVF 1 -> clip -> SVF 2 -> clip, both morphing lp / bp / hp; envelope follower lifts both cutoffs */
enum { TU_ST = 8, TU_ENV = 16, TU_DRY, TU_WET, TU_MK };

static RAMC void fx_turbo(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s, *st = s + TU_ST;
    float k1 = 2.0f * fm_exp2f(-s[2] * 5.3f), k2 = k1 * 1.3f, pg = fm_exp2f(s[4] * 4.5f);
    float l1 = 4.9069f + s[0] * 9.2f;                                  /* log2 Hz: 30 Hz .. 17.6 kHz */
    float l2 = lerpf(4.9069f + s[3] * 9.2f, l1 + (s[3] - 0.5f) * 6.0f, s[1]);
    float ea = s[5] * 5.0f, mo = s[6];
    float wl = mo < 0.5f ? 1.0f - 2.0f * mo : 0.0f, wh = mo > 0.5f ? 2.0f * mo - 1.0f : 0.0f, wb = 1.0f - wl - wh;
    float m0 = wh, m1 = wl - wh, m2a = (2.0f * wb - wh) * k1, m2b = (2.0f * wb - wh) * k2;   /* band: +6 dB */
    float env = s[TU_ENV], dry, ddry, wet, dwet, mk, dmk, c1[4], c2[4];
    uint32_t i, j, m;

    ddry = ramp(s, TU_DRY, 1.0f - s[7], first, inv_n, &dry);
    dwet = ramp(s, TU_WET, s[7], first, inv_n, &wet);
    dmk = ramp(s, TU_MK, 1.0f / (1.0f + 1.5f * s[4]), first, inv_n, &mk);
    for (j = 0; j < n; j += 16u) {
        float e = env * 2.5f;
        e = (e > 1.0f ? 1.0f : e) * ea;
        svf_set(c1, fm_exp2f(l1 + e), k1);
        svf_set(c2, fm_exp2f(l2 + e), k2);
        m = n - j < 16u ? n - j : 16u;
        for (i = j; i < j + m; i++) {
            float xl = L[i], xr = R[i], a, bl, br, yl, yr, w;
            dry += ddry;
            wet += dwet;
            mk += dmk;
            a = sclip(xl * pg);
            w = sclip(xr * pg);
            yl = svf_run(st, c1, a, &bl);
            yr = svf_run(st + 4, c1, w, &br);
            yl = sclip(m0 * a + m1 * yl + m2a * bl);
            yr = sclip(m0 * w + m1 * yr + m2a * br);
            a = svf_run(st + 2, c2, yl, &bl);
            w = svf_run(st + 6, c2, yr, &br);
            yl = 1.25f * sclip(0.8f * (m0 * yl + m1 * a + m2b * bl));
            yr = 1.25f * sclip(0.8f * (m0 * yr + m1 * w + m2b * br));
            a = fm_fabsf(xl) + fm_fabsf(xr);
            env += (a - env) * (a > env ? 0.008f : 0.00015f);
            L[i] = xl * dry + yl * mk * wet;
            R[i] = xr * dry + yr * mk * wet;
        }
    }
    s[TU_ENV] = fm_flush(env);
    flush(s, TU_ST, TU_ST + 8u);
}

/* ================================================================================================ WIRE === */
/* mono: wow line -> + hiss / crackle -> band-limit -> formant -> sample-and-hold -> companded bits -> dropouts */
enum { WI_HP = 8, WI_LP1 = 10, WI_LP2 = 12, WI_FO = 14, WI_PH = 16, WI_HOLD, WI_GATE, WI_CL, WI_LFO, WI_NZ, WI_WOW,
       WI_DRY, WI_WET };
#define WOWN 2048u

static RAMC void fx_wire(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s;
    int16_t *wl = f->arena;
    uint32_t pos = f->u[0] & (WOWN - 1u), drop = f->u[1], i, *rng = &f->u[U_RNG];
    float ch[4], cl1[4], cl2[4], cf[4];
    float lo = 120.0f * fm_exp2f(s[0] * 3.0f), hi = 1500.0f * fm_exp2f(s[0] * 2.4f);
    float famt = s[1] * 2.5f, inc = fm_exp2f(-s[2] * 6.0f), tel = s[3];
    float bits = 16.0f - s[4] * 13.0f, step = fm_exp2f(1.0f - bits), istep = 1.0f / step;
    int comp = bits < 15.0f;
    float hiss = tel * tel * 0.03f, garble = tel * 0.15f;
    uint32_t dthr = (uint32_t)(int32_t)(tel * tel * tel * (2.5f / SR) * 16777216.0f);   /* 24-bit chances */
    uint32_t cthr = (uint32_t)(int32_t)(s[5] * s[5] * 0.002f * 16777216.0f);
    float depth = s[6] * s[6] * 300.0f, wow, dwow, dry, ddry, wet, dwet;
    float ph = s[WI_PH], hold = s[WI_HOLD], gate = s[WI_GATE], cl = s[WI_CL];

    svf_set(ch, lo, 1.414f);
    svf_set(cl1, hi, 1.848f);
    svf_set(cl2, hi, 0.765f);
    svf_set(cf, 600.0f * fm_exp2f(s[1] * 2.2f), 0.25f);
    s[WI_LFO] = frac1(s[WI_LFO] + (0.6f + 0.3f * s[WI_NZ]) * (float)n * ISR);
    s[WI_NZ] = s[WI_NZ] * 0.98f + rnd1(rng) * 0.08f;
    dwow = ramp(s, WI_WOW, 2.0f + depth * (0.5f + 0.4f * fm_sin_turns(s[WI_LFO]) + 0.1f * s[WI_NZ]), first, inv_n, &wow);
    ddry = ramp(s, WI_DRY, 1.0f - s[7], first, inv_n, &dry);
    dwet = ramp(s, WI_WET, s[7], first, inv_n, &wet);

    for (i = 0; i < n; i++) {
        float xl = L[i], xr = R[i], x, b, y, nz;
        uint32_t r;
        dry += ddry;
        wet += dwet;
        wow += dwow;
        aw(wl, pos, 0.5f * (xl + xr));
        x = arf(wl, WOWN, pos, wow);
        pos = (pos + 1u) & (WOWN - 1u);
        r = xs32(rng);
        nz = (float)(int32_t)r * (1.0f / 2147483648.0f);
        if ((r >> 8) < cthr && (r & 1u))   /* crackle: sparse clicks, rough noise inside each */
            cl += (nz > 0.0f ? 0.1f : -0.1f) + nz * 0.4f;
        cl *= 0.72f;
        x += hiss * nz + cl * (0.5f + 0.5f * nz);
        y = svf_run(s + WI_HP, ch, x, &b);
        x = x - ch[3] * b - y;                   /* high-pass */
        x = svf_run(s + WI_LP1, cl1, x, &b);
        x = svf_run(s + WI_LP2, cl2, x, &b);
        svf_run(s + WI_FO, cf, x, &b);
        x += famt * cf[3] * b;                   /* formant: normalised band-pass peak */
        ph += inc;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            hold = x;
            if (comp) {                          /* sqrt companding, then the step */
                float a = fm_sqrtf(fm_fabsf(x));
                a = fm_floorf(a * istep + 0.5f) * step;
                hold = x < 0.0f ? -a * a : a * a;
            }
        }
        y = hold;
        if (drop) {                              /* dropouts: the line goes away, bursts of garble */
            drop--;
            gate += (0.0f - gate) * 0.01f;
            y += garble * nz * ((drop & 2047u) < 300u ? 1.0f : 0.0f);
        } else {
            gate += (1.0f - gate) * 0.01f;
            if ((r >> 8) < dthr && !(r & 2u))
                drop = 441u + ((r >> 9) & 8191u);
        }
        y = sclip(y * gate * 1.6f);             /* make-up into a hot line */
        L[i] = xl * dry + y * wet;
        R[i] = xr * dry + y * wet;
    }
    s[WI_PH] = ph;
    s[WI_HOLD] = hold;
    s[WI_GATE] = gate;
    s[WI_CL] = fm_flush(cl);
    flush(s, WI_HP, WI_FO + 2u);
    f->u[0] = pos;
    f->u[1] = drop;
}

/* =============================================================================================== KNOCK === */
/* 24 dB Butterworth low-pass -> transient designer + compressor (gain computed every 8 frames) -> gain -> knee clip */
enum { KN_ST = 8, KN_ENV = 16, KN_EF, KN_ES, KN_G, KN_DRY, KN_WET };

static RAMC void fx_knock(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s, *st = s + KN_ST;
    float c1[4], c2[4];
    float ca = 1.0f - fm_exp2f(-FM_LOG2E / (lexp(0.0001f, 0.08f, s[4]) * SR));
    float cr = 1.0f - fm_exp2f(-FM_LOG2E / (lexp(0.02f, 1.5f, s[5]) * SR));
    float T = -30.0f * s[2], sl = 1.0f - 1.0f / (1.0f + 9.0f * s[2]), W = 24.0f * s[4 + 2] + 0.01f;
    float mkup = 0.4f * sl * -T, punch = s[1] * 2.0f, out = (s[3] - 0.5f) * 36.0f;
    float env = s[KN_ENV], ef = s[KN_EF], es = s[KN_ES], g = s[KN_G], dry, ddry, wet, dwet;
    uint32_t i, j, m;

    svf_set(c1, 60.0f * fm_exp2f(s[0] * 8.3f), 1.848f);
    svf_set(c2, 60.0f * fm_exp2f(s[0] * 8.3f), 0.765f);
    ddry = ramp(s, KN_DRY, 1.0f - s[7], first, inv_n, &dry);
    dwet = ramp(s, KN_WET, s[7], first, inv_n, &wet);
    if (first)
        g = 1.0f;
    for (j = 0; j < n; j += 8u) {
        float lv = 6.0206f * fm_log2f(env + 1e-6f), ov = lv - T, gr, tr, dg;
        if (ov <= -0.5f * W)
            gr = 0.0f;
        else if (ov < 0.5f * W)
            gr = -sl * (ov + 0.5f * W) * (ov + 0.5f * W) / (2.0f * W);
        else
            gr = -sl * ov;
        tr = fm_log2f((ef + 1e-6f) / (es + 1e-6f));
        tr = punch * (tr < 0.0f ? 0.0f : tr > 1.0f ? 1.0f : tr) * 6.0206f;
        m = n - j < 8u ? n - j : 8u;
        dg = (fm_exp2f((gr + mkup + tr + out) * LOG2_10_20) - g) / (float)m;
        for (i = j; i < j + m; i++) {
            float xl = L[i], xr = R[i], b, yl, yr, pk;
            dry += ddry;
            wet += dwet;
            g += dg;
            yl = svf_run(st, c1, xl, &b);
            yl = svf_run(st + 2, c2, yl, &b);
            yr = svf_run(st + 4, c1, xr, &b);
            yr = svf_run(st + 6, c2, yr, &b);
            pk = fm_fabsf(yl) > fm_fabsf(yr) ? fm_fabsf(yl) : fm_fabsf(yr);
            env += (pk - env) * (pk > env ? ca : cr);
            ef += (pk - ef) * (pk > ef ? 0.045f : 0.0006f);
            es += (pk - es) * (pk > es ? 0.0019f : 0.0006f);
            L[i] = xl * dry + kclip(yl * g) * wet;
            R[i] = xr * dry + kclip(yr * g) * wet;
        }
    }
    s[KN_ENV] = fm_flush(env);
    s[KN_EF] = fm_flush(ef);
    s[KN_ES] = fm_flush(es);
    s[KN_G] = g;
    flush(s, KN_ST, KN_ST + 8u);
}

/* ================================================================================================ COIL === */
/* mono pre-delay -> spring loop: 8 stretched allpasses (float, in s[]) -> delay line (two taps) -> damping ->
 * feedback. "boing": transients kick a slow damped oscillator that bends the line length */
enum { CO_D = 8, CO_DPRE, CO_LP, CO_HP, CO_OL, CO_OR, CO_EF, CO_ES, CO_BP, CO_BV, CO_DRY, CO_WET, CO_FB, CO_A,
       CO_WID, CO_AP = 24 };                    /* CO_AP .. CO_AP + 63: eight 8-slot allpass buffers */
static const uint8_t co_m[8] = {8, 8, 7, 8, 6, 8, 7, 5};

static RAMC void fx_coil(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s, *ap = s + CO_AP;
    uint32_t P = f->arena_n >> 3, Dn = f->arena_n - P, pos = f->u[0], pp = f->u[1], c = f->u[2], i, j;
    int16_t *pre = f->arena, *dl = f->arena + P;
    float dlmax = fm_minf((float)Dn - 200.0f, 0.11f * SR);
    float dlen = lexp(0.012f * SR, dlmax, s[1]), rt = 4.0f * fm_exp2f(-s[2] * 4.5f);
    float clp = op_coef(6000.0f * fm_exp2f(-s[2] * 2.0f)), chp = op_coef(70.0f);
    float cot = op_coef(800.0f * fm_exp2f(s[0] * 4.3f)), boing = s[7] * 90.0f;
    float d, dd, dpre, ddpre, dry, ddry, wet, dwet, fb, dfb, a, da, wid, dwid;
    float lp = s[CO_LP], hp = s[CO_HP], ol = s[CO_OL], orr = s[CO_OR], ef = s[CO_EF], es = s[CO_ES];
    float bp = s[CO_BP], bv = s[CO_BV];

    dd = ramp(s, CO_D, dlen, first, inv_n, &d);
    ddpre = ramp(s, CO_DPRE, 1.0f + s[6] * s[6] * (float)(P - 3u), first, inv_n, &dpre);
    ddry = ramp(s, CO_DRY, mix_dry(s[3]), first, inv_n, &dry);
    dwet = ramp(s, CO_WET, mix_wet(s[3]), first, inv_n, &wet);
    dfb = ramp(s, CO_FB, rt_gain(dlen, rt), first, inv_n, &fb);
    da = ramp(s, CO_A, 0.35f + 0.5f * s[4], first, inv_n, &a);
    dwid = ramp(s, CO_WID, s[5] * 2.0f, first, inv_n, &wid);

    for (i = 0; i < n; i++) {
        float xl = L[i], xr = R[i], x, ax, rd, tb, u, o, v, m, sd, off;
        uint32_t ci = c & 7u;
        d += dd;
        dpre += ddpre;
        dry += ddry;
        wet += dwet;
        fb += dfb;
        a += da;
        wid += dwid;
        x = 0.5f * (xl + xr);
        aw(pre, pp, x);
        x = arf(pre, P, pp, dpre);
        pp = winc(pp, P);
        ax = fm_fabsf(x);                         /* boing: transient -> damped ~5 Hz oscillator */
        ef += (ax - ef) * (ax > ef ? 0.05f : 0.002f);
        es += (ax - es) * 0.002f;
        bv += (ef > es ? ef - es : 0.0f) * 0.002f - bp * 1.0e-6f - bv * 0.0004f;
        bp += bv;
        off = boing * sclip(bp * 4.0f);
        rd = arf(dl, Dn, pos, d + 100.0f + off);
        tb = arf(dl, Dn, pos, d * 0.61f + 100.0f - off);
        lp += (rd - lp) * clp;
        hp += (lp - hp) * chp;
        u = 0.6f * x + (lp - hp) * fb;
        for (j = 0; j < 8; j++) {                 /* dispersion: the chirp */
            float *b = ap + 8u * j;
            o = b[(c - co_m[j]) & 7u];
            v = u - a * o;
            b[ci] = v;
            u = o + a * v;
        }
        c++;
        aw(dl, pos, sclip(u));
        pos = winc(pos, Dn);
        ol += (rd - ol) * cot;
        orr += (tb - orr) * cot;
        m = 0.5f * (ol + orr);
        sd = 0.5f * (ol - orr) * wid;
        L[i] = xl * dry + (m + sd) * wet;
        R[i] = xr * dry + (m - sd) * wet;
    }
    s[CO_LP] = fm_flush(lp);
    s[CO_HP] = fm_flush(hp);
    s[CO_OL] = fm_flush(ol);
    s[CO_OR] = fm_flush(orr);
    s[CO_EF] = fm_flush(ef);
    s[CO_ES] = fm_flush(es);
    s[CO_BP] = fm_flush(bp);
    s[CO_BV] = fm_flush(bv);
    flush(s, CO_AP, CO_AP + 64u);
    f->u[0] = pos;
    f->u[1] = pp;
    f->u[2] = c;
}

/* ============================================================================================= DOPPLER === */
/* one mono line (the whole arena); per side two read heads sweep a window at (1 - ratio) and crossfade;
 * a quadrature oscillator ring-modulates for the sideband mix; feedback through the tone filter */
enum { DO_PL = 8, DO_PR, DO_D, DO_W, DO_OC, DO_OS, DO_LPL, DO_LPR, DO_HPL, DO_HPR, DO_DRY, DO_WET, DO_FB, DO_SB };

static RAMC void fx_doppler(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s;
    uint32_t N = f->arena_n, pos = f->u[0], i;
    int16_t *ln = f->arena;
    float wmax = fm_minf(0.15f * SR, (float)N * 0.33f), wt = lexp(0.015f * SR, wmax, s[4]);
    float dt = s[1] * s[1] * ((float)N - 12.0f - wt), r = fm_exp2f((s[0] - 0.5f) * 2.0f);
    float rl = r * fm_exp2f(s[5] * 0.03f), rr = r * fm_exp2f(-s[5] * 0.03f), iw, incl, incr;
    float fc = 40.0f * fm_exp2f(fm_fabsf(2.0f * s[0] - 1.0f) * 6.5f), cr, sr, oc, os, nn;
    float clp = op_coef(1500.0f * fm_exp2f(s[6] * 3.4f)), chp = op_coef(25.0f * fm_exp2f(s[6] * 4.7f));
    float pl, pr, d, ddl, w, dw, dry, ddry, wet, dwet, fb, dfb, sb, dsb;
    float lpl = s[DO_LPL], lpr = s[DO_LPR], hpl = s[DO_HPL], hpr = s[DO_HPR];

    if (first) {
        s[DO_PL] = 0.0f;
        s[DO_PR] = 0.25f;
        s[DO_OC] = 1.0f;
        s[DO_OS] = 0.0f;
    }
    ddl = ramp(s, DO_D, dt, first, inv_n, &d);
    dw = ramp(s, DO_W, wt, first, inv_n, &w);
    ddry = ramp(s, DO_DRY, mix_dry(s[7]), first, inv_n, &dry);
    dwet = ramp(s, DO_WET, mix_wet(s[7]), first, inv_n, &wet);
    dfb = ramp(s, DO_FB, s[2], first, inv_n, &fb);
    dsb = ramp(s, DO_SB, s[3], first, inv_n, &sb);
    pl = s[DO_PL];
    pr = s[DO_PR];
    iw = 1.0f / (0.5f * (w + wt));
    incl = (1.0f - rl) * iw;
    incr = (1.0f - rr) * iw;
    cr = fm_sin_turns(fc * ISR + 0.25f);
    sr = fm_sin_turns(fc * ISR);
    oc = s[DO_OC];
    os = s[DO_OS];
    nn = 1.5f - 0.5f * (oc * oc + os * os);       /* keep the oscillator on the unit circle */
    oc *= nn;
    os *= nn;

    for (i = 0; i < n; i++) {
        float xl = L[i], xr = R[i], q, wa, yl, yr, t;
        d += ddl;
        w += dw;
        dry += ddry;
        wet += dwet;
        fb += dfb;
        sb += dsb;
        pl += incl;
        if (pl >= 1.0f)
            pl -= 1.0f;
        else if (pl < 0.0f)
            pl += 1.0f;
        pr += incr;
        if (pr >= 1.0f)
            pr -= 1.0f;
        else if (pr < 0.0f)
            pr += 1.0f;
        q = pl < 0.5f ? pl + 0.5f : pl - 0.5f;
        wa = sstep(1.0f - fm_fabsf(2.0f * pl - 1.0f));
        yl = arf(ln, N, pos, d + 2.0f + pl * w) * wa + arf(ln, N, pos, d + 2.0f + q * w) * (1.0f - wa);
        q = pr < 0.5f ? pr + 0.5f : pr - 0.5f;
        wa = sstep(1.0f - fm_fabsf(2.0f * pr - 1.0f));
        yr = arf(ln, N, pos, d + 2.0f + pr * w) * wa + arf(ln, N, pos, d + 2.0f + q * w) * (1.0f - wa);
        t = oc * cr - os * sr;                    /* quadrature carrier */
        os = os * cr + oc * sr;
        oc = t;
        yl += (yl * oc * 1.41f - yl) * sb;
        yr += (yr * os * 1.41f - yr) * sb;
        lpl += (yl - lpl) * clp;
        lpr += (yr - lpr) * clp;
        hpl += (lpl - hpl) * chp;
        hpr += (lpr - hpr) * chp;
        yl = lpl - hpl;
        yr = lpr - hpr;
        aw(ln, pos, 0.5f * (xl + xr) + sclip(0.5f * (yl + yr) * fb));
        pos = winc(pos, N);
        L[i] = xl * dry + yl * wet;
        R[i] = xr * dry + yr * wet;
    }
    s[DO_PL] = pl;
    s[DO_PR] = pr;
    s[DO_OC] = oc;
    s[DO_OS] = os;
    s[DO_LPL] = fm_flush(lpl);
    s[DO_LPR] = fm_flush(lpr);
    s[DO_HPL] = fm_flush(hpl);
    s[DO_HPR] = fm_flush(hpr);
    f->u[0] = pos;
}

/* ================================================================================================ HALL === */
/* mono pre-delay (arena / 8) -> allpass diffusers -> FDN, Householder mixed, two lines modulated -> gate.
 * Master (arena >= 24576): 4 diffusers, 8 lines of (arena - pre-delay - diffusers) / 8 (75 ms at 32768).
 * Track: 2 diffusers, 4 lines of (arena - ...) / 4 (28 ms at 6144): a smaller room, a shorter decay. */
enum { HA_LP = 8, HA_SZ = 16, HA_DPRE, HA_PH, HA_M1, HA_M2, HA_ENV, HA_GATE, HA_DRY, HA_WET, HA_WID, HA_AD, HA_LIM };
static const float ha_r8[8] = {1.000f, 0.913f, 0.827f, 0.759f, 0.681f, 0.617f, 0.553f, 0.502f};
static const float ha_r4[4] = {1.000f, 0.827f, 0.681f, 0.553f};
static const float ha_in8[8] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f};
static const float ha_in4[4] = {1.0f, -1.0f, 1.0f, -1.0f};
static const uint16_t ha_big_n[4] = {256, 256, 512, 1024}, ha_big_d[4] = {142, 229, 383, 613};
static const uint16_t ha_sm_n[2] = {128, 256}, ha_sm_d[2] = {97, 173};

/* NL = 4 or 8 lines (a constant at each call: the loops unroll); modulated lines: 1 and NL - 2 */
INL void hall_body(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n, const uint32_t NL)
{
    float *s = f->s, *lp = s + HA_LP;
    const uint16_t *dn = NL == 8u ? ha_big_n : ha_sm_n, *dd = NL == 8u ? ha_big_d : ha_sm_d;
    const float *rat = NL == 8u ? ha_r8 : ha_r4, *sg = NL == 8u ? ha_in8 : ha_in4;
    uint32_t nd = NL == 8u ? 4u : 2u, P = f->arena_n >> 3, base = P, B, pos, pp = f->u[1], c = f->u[2], tim = f->u[3];
    uint32_t i, j, di[8];
    int16_t *pre = f->arena, *ln, *db[4];
    float g[8], sz, rt, clp, md, m1, m2, dm1, dm2, dpre, ddpre, dry, ddry, wet, dwet, wid, dwid, ad, dad;
    float hold, gamt, env = s[HA_ENV], gate = s[HA_GATE], lim, lpk = 0.0f, hh = 2.0f / (float)NL;

    for (j = 0; j < nd; j++) {
        db[j] = f->arena + base;
        base += dn[j];
    }
    B = (f->arena_n - base) / NL;                 /* NL is 4 or 8: a shift */
    ln = f->arena + base;
    pos = f->u[0] < B ? f->u[0] : 0u;
    if (first) {
        s[HA_SZ] = s[0];
        s[HA_LIM] = 1.0f;
    }
    s[HA_SZ] += (s[0] - s[HA_SZ]) * fm_minf(1.0f, (float)n * (1.0f / (0.3f * SR)));
    sz = (0.3f + 0.7f * s[HA_SZ]) * ((float)B - 24.0f);
    rt = 0.35f * fm_exp2f(s[0] * 4.3f) * fm_maxf(0.45f, fm_minf(1.0f, fm_sqrtf((float)B * (1.0f / 3296.0f))));
    clp = op_coef(1500.0f * fm_exp2f(s[2] * 3.6f));
    lim = s[HA_LIM];
    for (j = 0; j < NL; j++) {
        float dj = rat[j] * sz;
        di[j] = (uint32_t)(int32_t)dj;
        g[j] = rt_gain(dj, rt) * lim;             /* lim < 1 only when the loop runs hot (loud, endless input) */
    }
    md = s[6] * 12.0f;
    s[HA_PH] = frac1(s[HA_PH] + 0.43f * (float)n * ISR);
    dm1 = ramp(s, HA_M1, 13.0f + md * fm_sin_turns(s[HA_PH]), first, inv_n, &m1);
    dm2 = ramp(s, HA_M2, 13.0f + md * fm_sin_turns(s[HA_PH] + 0.37f), first, inv_n, &m2);
    ddpre = ramp(s, HA_DPRE, 1.0f + s[4] * s[4] * (float)(P - 3u), first, inv_n, &dpre);
    ddry = ramp(s, HA_DRY, mix_dry(s[3]), first, inv_n, &dry);
    dwet = ramp(s, HA_WET, mix_wet(s[3]) * (NL == 8u ? 0.5f : 0.75f), first, inv_n, &wet);
    dwid = ramp(s, HA_WID, s[7] * 2.0f, first, inv_n, &wid);
    dad = ramp(s, HA_AD, 0.75f * s[5], first, inv_n, &ad);
    hold = 0.8f * SR * fm_exp2f(-s[1] * 4.0f);
    gamt = s[1] * 20.0f > 1.0f ? 1.0f : s[1] * 20.0f;

    for (i = 0; i < n; i++) {
        float x, ax, y[8], sum, m, sd, o, v, gv;
        dry += ddry;
        wet += dwet;
        wid += dwid;
        dpre += ddpre;
        ad += dad;
        m1 += dm1;
        m2 += dm2;
        x = 0.5f * (L[i] + R[i]);
        ax = fm_fabsf(x);                         /* gate: hold open while the input plays */
        env = ax > env ? ax : env * 0.9995f;
        if (env > 0.02f)
            tim = (uint32_t)(int32_t)hold;
        else if (tim)
            tim--;
        gate += ((tim ? 1.0f : 0.0f) - gate) * (tim ? 0.02f : 0.0012f);
        aw(pre, pp, x);
        x = arf(pre, P, pp, dpre);
        pp = winc(pp, P);
        for (j = 0; j < nd; j++) {                /* diffusers */
            uint32_t mk = dn[j] - 1u;
            o = ar(db[j], (c - dd[j]) & mk);
            v = x - ad * o;
            aw(db[j], c & mk, v);
            x = o + ad * v;
        }
        c++;
        x *= 0.35f;
        sum = 0.0f;
        for (j = 0; j < NL; j++) {
            if (j == 1u)
                y[j] = arf(ln + j * B, B, pos, (float)di[j] - m1);
            else if (j == NL - 2u)
                y[j] = arf(ln + j * B, B, pos, (float)di[j] - m2);
            else
                y[j] = ari(ln + j * B, B, pos, di[j]);
            lp[j] += (y[j] - lp[j]) * clp;
            sum += lp[j] * g[j];
        }
        sum *= hh;                                /* Householder: t - (2/N) sum t */
        lpk = fm_maxf(lpk, fm_fabsf(sum));
        for (j = 0; j < NL; j++)
            aw(ln + j * B, pos, lp[j] * g[j] - sum + sg[j] * x);
        pos = winc(pos, B);
        o = y[0] - y[2];
        v = y[1] - y[3];
        if (NL == 8u) {
            o += y[4] - y[6];
            v += y[5] - y[7];
        }
        m = 0.5f * (o + v);
        sd = 0.5f * (o - v) * wid;
        gv = wet * (1.0f - gamt + gamt * gate);
        L[i] = L[i] * dry + kclip((m + sd) * gv);
        R[i] = R[i] * dry + kclip((m - sd) * gv);
    }
    s[HA_LIM] = lpk > 0.4f ? lim * fm_maxf(0.9f, 0.4f / lpk) : lim + (1.0f - lim) * 0.02f;
    s[HA_ENV] = fm_flush(env);
    s[HA_GATE] = fm_flush(gate);
    flush(s, HA_LP, HA_LP + NL);
    f->u[0] = pos;
    f->u[1] = pp;
    f->u[2] = c;
    f->u[3] = tim;
}

static RAMC void fx_hall(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    if (f->arena_n >= 24576u)
        hall_body(f, first, inv_n, L, R, n, 8u);
    else
        hall_body(f, first, inv_n, L, R, n, 4u);
}

/* ================================================================================================ BITS === */
/* drive -> pre-gain -> (anti-alias) -> sample-and-hold with jitter -> quantiser -> (linear hold) -> tone.
 * The model knob morphs four feature weights, so it has no steps: clean (pre-filter + linear hold, rounding),
 * dither (+ TPDF dither), aliasing (raw hold, truncation), gritty (+ overflow wrap and sqrt companding). */
enum { BI_PH = 8, BI_INC, BI_HL, BI_HR, BI_PL, BI_PR, BI_AL, BI_AR, BI_TL, BI_TR, BI_UL, BI_UR, BI_DRY, BI_WET,
       BI_GN };

INL float bits_q(float x, float step, float istep, float dith, float bias, float grit, uint32_t *rng)
{
    float y;
    if (grit > 0.0f) {
        float w = x - 2.0f * fm_floorf((x + 1.0f) * 0.5f);      /* overflow wraps around */
        float c = clampf(x, -1.0f, 1.0f);
        x = c + (w - c) * grit;
        c = fm_sqrtf(fm_fabsf(x));
        x += ((x < 0.0f ? -c : c) - x) * grit;
    }
    y = fm_floorf(x * istep + bias + dith * (rnd1(rng) + rnd1(rng)) * 0.5f) * step;
    if (grit > 0.0f)
        y += ((y < 0.0f ? -y * y : y * y) - y) * grit;
    return y;
}

static RAMC void fx_bits(fx_t *f, int first, float inv_n, float *L, float *R, uint32_t n)
{
    float *s = f->s;
    uint32_t i, *rng = &f->u[U_RNG];
    float inc0 = fm_exp2f(-(1.0f - s[0]) * 7.0f), bits = 1.0f + 15.0f * s[1], step = fm_exp2f(1.0f - bits);
    float istep = 1.0f / step, m3 = s[2] * 3.0f, jit = s[4] * 0.9f;
    float smooth = m3 < 1.0f ? 1.0f : m3 < 2.0f ? 2.0f - m3 : 0.0f;
    float dith = m3 < 1.0f ? m3 : m3 < 2.0f ? 2.0f - m3 : 0.0f;
    float bias = 0.5f - 0.5f * (m3 < 1.0f ? 0.0f : m3 < 2.0f ? m3 - 1.0f : 1.0f);
    float grit = m3 > 2.0f ? m3 - 2.0f : 0.0f;
    float dg = 1.0f + s[7] * 11.0f, dmk = 1.0f / (1.0f + s[7] * 1.2f), dam = s[7] * 8.0f > 1.0f ? 1.0f : s[7] * 8.0f;
    float caa = op_coef(fm_minf(0.45f * inc0 * SR, 18000.0f));
    float tlp = s[6] < 0.5f ? 800.0f * fm_exp2f(s[6] * 2.0f * 4.6f) : 20000.0f;
    float thp = s[6] > 0.5f ? 20.0f * fm_exp2f((s[6] - 0.5f) * 2.0f * 6.6f) : 5.0f;
    float clt = op_coef(tlp), cht = op_coef(thp);
    float ph = s[BI_PH], inc = s[BI_INC], hl = s[BI_HL], hr = s[BI_HR], pl = s[BI_PL], pr = s[BI_PR];
    float al = s[BI_AL], arr = s[BI_AR], tl = s[BI_TL], tr = s[BI_TR], ul = s[BI_UL], ur = s[BI_UR];
    float dry, ddry, wet, dwet, gn, dgn;

    ddry = ramp(s, BI_DRY, 1.0f - s[3], first, inv_n, &dry);
    dwet = ramp(s, BI_WET, s[3], first, inv_n, &wet);
    dgn = ramp(s, BI_GN, fm_exp2f((s[5] - 0.5f) * 4.0f), first, inv_n, &gn);
    if (first)
        inc = inc0;

    for (i = 0; i < n; i++) {
        float xl = L[i], xr = R[i], a, b, yl, yr;
        dry += ddry;
        wet += dwet;
        gn += dgn;
        a = xl + (sclip(xl * dg) * dmk - xl) * dam;
        b = xr + (sclip(xr * dg) * dmk - xr) * dam;
        a *= gn;
        b *= gn;
        al += (a - al) * caa;
        arr += (b - arr) * caa;
        a += (al - a) * smooth;
        b += (arr - b) * smooth;
        ph += inc;
        if (ph >= 1.0f) {
            ph -= 1.0f;
            if (ph >= 1.0f)
                ph = 0.0f;
            pl = hl;
            pr = hr;
            hl = bits_q(a, step, istep, dith, bias, grit, rng);
            hr = bits_q(b, step, istep, dith, bias, grit, rng);
            inc = inc0 * (1.0f + jit * rnd1(rng));
        }
        a = 1.0f / gn;
        yl = (hl + (pl + (hl - pl) * ph - hl) * smooth) * a;
        yr = (hr + (pr + (hr - pr) * ph - hr) * smooth) * a;
        tl += (yl - tl) * clt;
        tr += (yr - tr) * clt;
        ul += (tl - ul) * cht;
        ur += (tr - ur) * cht;
        L[i] = xl * dry + (tl - ul) * wet;
        R[i] = xr * dry + (tr - ur) * wet;
    }
    s[BI_PH] = ph;
    s[BI_INC] = inc;
    s[BI_HL] = hl;
    s[BI_HR] = hr;
    s[BI_PL] = pl;
    s[BI_PR] = pr;
    s[BI_AL] = fm_flush(al);
    s[BI_AR] = fm_flush(arr);
    s[BI_TL] = fm_flush(tl);
    s[BI_TR] = fm_flush(tr);
    s[BI_UL] = fm_flush(ul);
    s[BI_UR] = fm_flush(ur);
}

/* ================================================================================================= API === */
RAMC void fx_reset(int type, fx_t *f)
{
    uint32_t i, n = f->arena_n;
    int16_t *a = f->arena;
    (void)type;
    for (i = 0; i < 96u; i++)
        f->s[i] = 0.0f;
    for (i = 0; i < 16u; i++)
        f->u[i] = 0u;
    if (a)
        for (i = 0; i < n; i++)
            a[i] = 0;
}

RAMC void fx_run(int type, fx_t *f, const float k[8], float *L, float *R, uint32_t n)
{
    int first;
    float inv_n;
    if (!n || !f->arena || f->arena_n < 4096u)
        return;
    first = fx_begin(f, k);
    if (!first)
        fx_smooth(f, k, n);
    inv_n = 1.0f / (float)n;
    switch (type) {
    case FX_ECHO: fx_echo(f, first, inv_n, L, R, n); break;
    case FX_CUBE: fx_cube(f, first, inv_n, L, R, n); break;
    case FX_TURBO: fx_turbo(f, first, inv_n, L, R, n); break;
    case FX_WIRE: fx_wire(f, first, inv_n, L, R, n); break;
    case FX_KNOCK: fx_knock(f, first, inv_n, L, R, n); break;
    case FX_COIL: fx_coil(f, first, inv_n, L, R, n); break;
    case FX_DOPPLER: fx_doppler(f, first, inv_n, L, R, n); break;
    case FX_HALL: fx_hall(f, first, inv_n, L, R, n); break;
    case FX_BITS: fx_bits(f, first, inv_n, L, R, n); break;
    default: break;
    }
}
