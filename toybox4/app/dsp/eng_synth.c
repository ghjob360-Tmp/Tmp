/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the synth engines (see eng.h for the contract and the parameter list).
 *
 * The per-voice state (eng_state_t) is laid out the same way for every engine:
 *   f[0..7]  the eight parameters, smoothed once per block (selectors use the raw k[] instead)
 *   f[8]     time since the note-on, seconds         f[9]  velocity (timbre only: the core does the level)
 *   f[10]    a declick offset (sampler)               f[11..47] the engine's own
 *   u[0..5]  the engine's own      u[6] engine tag (e + 1: another tag = a fresh voice)
 *   u[7]     the noise seed (the core seeds it)
 * The SLINE line: STRING (two 512-float strings), SPECTRA (three 256-float wave tables), DNA (the 33-point wave).
 * eng_tables_init() (main loop, once at boot, after dsp_tables_init) builds the read-only tables of SPECTRA. */
#include "eng.h"

void eng_tables_init(void);

enum { S_T = 8, S_VEL = 9, S_DC = 10, S_A = 11 };
#define U_TAG 6
#define U_RNG 7

/* ------------------------------------------------------------------ helpers --- */
/* the per-block maths, out of line (one copy in RAM, not one per use) */
static RAMC NOIN float b_exp2(float x) { return fm_exp2f(x); }
static RAMC NOIN float b_exp(float x) { return fm_expf(x); }
static RAMC NOIN float b_log2(float x) { return fm_log2f(x); }
static RAMC NOIN float b_pow(float x, float y) { return fm_powf(x, y); }
static RAMC NOIN float b_tan(float x) { return fm_tanf(x); }
INL float wrap1(float p) { return p >= 1.0f ? p - 1.0f : p; }
INL float sin_t(float p) { return fm_sin_turns(p); }
INL float cos_t(float p) { return fm_sin_turns(p + 0.25f); }
INL float saw_b(float p, float dt) { return 2.0f * p - 1.0f - blep(p, dt); }   /* rising, -1 at p = 0 */
INL float pulse_b(float p, float dt, float w)                              /* -1 on [0, w), +1 after */
{
    float q = p - w;
    if (q < 0.0f)
        q += 1.0f;
    return saw_b(p, dt) - saw_b(q, dt) - (2.0f * w - 1.0f);
}
INL float tri_s(float p) { return 1.0f - 4.0f * fm_fabsf(frac1(p + 0.25f) - 0.5f); }   /* in phase with sin */
/* the basic waves, all with their fundamental in phase with sin: sine, triangle, saw (falling), square, pulse 25% */
INL float wave5(int i, float p, float dt)
{
    switch (i) {
    case 0: return sin_t(p);
    case 1: return tri_s(p);
    case 2: return -saw_b(p, dt);
    case 3: return -pulse_b(p, dt, 0.5f);
    default: return -pulse_b(p, dt, 0.25f);
    }
}
static RAMC NOIN float wave_morph(float w, float p, float dt)   /* w in 0..4: a blend of two neighbours */
{
    int i = (int)w;
    float fr = w - (float)i, a = wave5(i, p, dt);
    return fr > 0.001f ? a + (wave5(i + 1, p, dt) - a) * fr : a;
}
INL float fold_tri(float x) { return 1.0f - 4.0f * fm_fabsf(frac1(x * 0.25f + 0.25f) - 0.5f); }   /* x near 0 */
INL float dcb(float x, float *x1, float *y1)       /* DC blocker, ~7 Hz */
{
    float y = x - *x1 + 0.999f * *y1;
    *x1 = x;
    *y1 = fm_flush(y);
    return y;
}
static RAMC NOIN float cents(float c) { return fm_exp2f(c * (1.0f / 1200.0f)); }
INL float bip(float k, float dz)                    /* 0..1 -> -1..1 with a dead zone at the centre */
{
    float x = (k - 0.5f) * 2.0f;
    if (fm_fabsf(x) < dz)
        return 0.0f;
    return x > 0.0f ? (x - dz) / (1.0f - dz) : (x + dz) / (1.0f - dz);
}

/* TPT state-variable filter (zero-delay feedback): coefficients per block, two states per sample */
typedef struct {
    float a1, a2, a3, k;
} svf_t;
static RAMC NOIN void svf_set(svf_t *c, float fc, float res)   /* res 0..1 */
{
    float g = b_tan(FM_PI * clampf(fc, 10.0f, 0.46f * SR) * ISR);
    c->k = 2.0f - 1.96f * clampf(res, 0.0f, 1.0f);
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}
INL float svf_tick(const svf_t *c, float *ic1, float *ic2, float v0, float *bp)
{
    float v3 = v0 - *ic2, v1 = c->a1 * *ic1 + c->a2 * v3, v2 = *ic2 + c->a2 * *ic1 + c->a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    *bp = v1;
    return v2;
}

/* ================================================================== STRING ===
 * Two waveguide strings in the line (A at 0, B at 512, 512 floats each); low notes run the loops at 1/2 or 1/4
 * of the sample rate (u[4]) so a string still fits; the output is interpolated back up.
 * u[2] write index, u[3] rate divider phase, u[4] divider shift. */
enum { ST_LPA = S_A, ST_LPB, ST_PE, ST_E1, ST_E2, ST_B1, ST_B2, ST_B3, ST_B4, ST_PRV, ST_CUR, ST_DX, ST_DY };
#define ST_MASK 511u

static RAMC void on_string(eng_state_t *s, float *line, float hz, int fresh)
{
    uint32_t sh = 0, i;
    float per = SR / hz;
    while (per > 500.0f && sh < 2u) {
        per *= 0.5f;
        sh++;
    }
    if (fresh)
        for (i = 0; i < SLINE; i++)
            line[i] = 0.0f;
    s->u[4] = sh;
    s->f[ST_PE] = 1.0f;
}

static RAMC NOIN void run_string(eng_state_t *s, float *line, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    uint32_t sh = s->u[4], r = 1u << sh, rm = r - 1u, w = s->u[2], dv = s->u[3], seed = s->u[U_RNG], i;
    float rate = SR / (float)(int32_t)r, ir = 1.0f / (float)(int32_t)r, vel = f[S_VEL];
    float tens = f[0], idec = f[1], det = f[2], typ = f[3], pos = f[4], bri = f[5], body = f[6], drv = f[7];
    float b = (0.15f + 0.85f * bri * bri) * (0.75f + 0.25f * vel), comp = (1.0f - b) / b;
    float tsus = 0.12f * b_exp2(tens * 7.0f);
    float hzb = hz * cents(det * det * 60.0f + 1.5f);
    float da = clampf(rate / hz - comp, 2.0f, 508.0f), db = clampf(rate / hzb - comp, 2.0f, 508.0f);
    float ga = fm_minf(b_exp2(-9.97f / (hz * tsus)), 0.9997f), gb = fm_minf(b_exp2(-9.97f / (hzb * tsus)), 0.9997f);
    float lvb = clampf(det * 12.0f, 0.0f, 1.0f), og = 1.0f / (1.0f + 0.7f * lvb);
    int32_t dai = (int32_t)da, dbi = (int32_t)db;
    float daf = da - (float)dai, dbf = db - (float)dbi;
    uint32_t pa = (uint32_t)(int32_t)(da * (0.04f + 0.46f * pos)) + 1u, pb = (uint32_t)(int32_t)(db * (0.04f + 0.46f * pos)) + 1u;
    /* the excitation: a pluck (short, decaying) and a bow (slow attack, long, noisier) */
    float taup = 0.0006f * b_exp2(idec * 6.0f), mp = b_exp2(-1.4427f * (float)(int32_t)r / (taup * SR));
    float taub = 0.1f + 2.5f * idec * idec, t0 = f[S_T], t1 = t0 + (float)(int32_t)n * ISR;
    float bw0 = (1.0f - b_exp(-t0 * 25.0f)) * b_exp(-t0 / taub), bw1 = (1.0f - b_exp(-t1 * 25.0f)) * b_exp(-t1 / taub);
    float bws = (bw1 - bw0) / (float)(int32_t)n, bw = bw0;
    /* (the excitation's energy over the period: a short pluck on a low string is louder, a long one on a short
     * string would build up in the loop) */
    float per = da * (float)(int32_t)r;
    float ap = (1.0f - typ) * (0.6f + 0.4f * vel) * 1.4f * clampf(fm_sqrtf(per * ISR / taup), 0.2f, 3.0f);
    float ab = typ * 0.22f * (0.6f + 0.4f * vel) * clampf(fm_sqrtf(per * (1.0f / 170.0f)), 0.3f, 2.0f);
    float ec = lerpf(0.25f + 0.7f * bri, 0.08f + 0.3f * bri, typ);
    float lpa = f[ST_LPA], lpb = f[ST_LPB], pe = f[ST_PE], e1 = f[ST_E1], e2 = f[ST_E2], prv = f[ST_PRV], cur = f[ST_CUR];
    float b1 = f[ST_B1], b2 = f[ST_B2], b3 = f[ST_B3], b4 = f[ST_B4], dx = f[ST_DX], dy = f[ST_DY];
    float d3 = drv * drv * drv, dg = 1.3f * (1.0f + 10.0f * d3), dn = 1.0f / (1.0f + 0.9f * d3);   /* drive */
    svf_t c1, c2;
    svf_set(&c1, 190.0f, 0.82f);
    svf_set(&c2, 470.0f, 0.86f);
    for (i = 0; i < n; i++) {
        float o, bp1, bp2;
        if ((dv & rm) == 0u) {
            float ex, ya, yb, y0, y1;
            uint32_t j0, j1;
            e1 += ec * (rnd1(&seed) - e1);
            e2 += ec * (e1 - e2);
            ex = e2 * (pe * ap + bw * ab);
            pe *= mp;
            j0 = (w - (uint32_t)dai) & ST_MASK;
            j1 = (j0 - 1u) & ST_MASK;
            y0 = line[j0];
            y1 = line[j1];
            ya = y0 + (y1 - y0) * daf;
            lpa += b * (ya - lpa);
            line[w] = ga * lpa + ex;
            line[(w - pa) & ST_MASK] -= ex * 0.85f;
            j0 = 512u + ((w - (uint32_t)dbi) & ST_MASK);
            j1 = 512u + ((w - (uint32_t)dbi - 1u) & ST_MASK);
            y0 = line[j0];
            y1 = line[j1];
            yb = y0 + (y1 - y0) * dbf;
            lpb += b * (yb - lpb);
            line[512u + w] = gb * lpb + ex;
            line[512u + ((w - pb) & ST_MASK)] -= ex * 0.85f;
            w = (w + 1u) & ST_MASK;
            prv = cur;
            cur = (ya + lvb * yb) * og;
        }
        o = prv + (cur - prv) * (float)(int32_t)((dv & rm) + 1u) * ir;
        dv++;
        bw += bws;
        svf_tick(&c1, &b1, &b2, o, &bp1);
        svf_tick(&c2, &b3, &b4, o, &bp2);
        o += body * (bp1 * c1.k * 1.6f + bp2 * c2.k * 1.0f);
        o = dcb(o, &dx, &dy);
        o = soft(o * dg) * dn;
        out[i] += o;
    }
    f[ST_LPA] = fm_flush(lpa);
    f[ST_LPB] = fm_flush(lpb);
    f[ST_PE] = fm_flush(pe);
    f[ST_E1] = e1;
    f[ST_E2] = e2;
    f[ST_PRV] = prv;
    f[ST_CUR] = cur;
    f[ST_B1] = fm_flush(b1);
    f[ST_B2] = fm_flush(b2);
    f[ST_B3] = fm_flush(b3);
    f[ST_B4] = fm_flush(b4);
    f[ST_DX] = dx;
    f[ST_DY] = dy;
    s->u[2] = w;
    s->u[3] = dv & rm;
    s->u[U_RNG] = seed;
}

/* =================================================================== PULSE === */
enum { PU_P1 = S_A, PU_P2, PU_PS, PU_LFO, PU_I1, PU_I2, PU_FC };

static RAMC NOIN void run_pulse(eng_state_t *s, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    float width = f[0], sec = k[1], cut = f[2], pwm = f[3], res = f[4], subl = f[5], fenv = f[6], drv = f[7];
    float dt = fm_minf(hz * ISR, 0.45f), w = 0.5f - 0.46f * width, nf = (float)(int32_t)n;
    float lrate = 0.25f + 5.0f * pwm * pwm, l0 = f[PU_LFO], l1 = frac1(l0 + lrate * nf * ISR);
    float wa = clampf(w + pwm * 0.42f * sin_t(l0), 0.03f, 0.97f), wb = clampf(w + pwm * 0.42f * sin_t(l1), 0.03f, 0.97f);
    float ws = (wb - wa) / nf, dt2, lv2, ratio = 1.0f, m1 = 0.5f;
    int sync = sec >= 0.6f;
    float p1 = f[PU_P1], p2 = f[PU_P2], ps = f[PU_PS], i1 = f[PU_I1], i2 = f[PU_I2], env, fc, g, gi;
    uint32_t i;
    svf_t c;
    if (!sync) {                         /* a second pulse, detuned */
        lv2 = fm_minf(1.0f, sec * 6.0f) * 0.8f;
        dt2 = dt * cents(3.0f + sec * 40.0f);
    } else {                             /* hard sync: the second pulse is reset by the first */
        ratio = 1.0f + (sec - 0.6f) * 2.5f * 3.0f;
        dt2 = fm_minf(dt * ratio, 0.45f);
        lv2 = 1.0f;
        m1 = 0.25f;
    }
    env = b_exp(-f[S_T] * 3.3f);
    fc = cut * 9.5f + fenv * env * (0.6f + 0.4f * f[S_VEL]) * 6.0f + 0.4f * b_log2(hz * (1.0f / 261.6f));
    if (f[PU_FC] == 0.0f)                /* (octaves above 30 Hz, smoothed: a retrigger is not a step) */
        f[PU_FC] = fc;
    fc = f[PU_FC] += (fc - f[PU_FC]) * 0.45f;
    svf_set(&c, fm_minf(30.0f * b_exp2(fc), 18000.0f), res * 0.97f);
    g = (1.0f + drv * 4.0f) * 0.5f;
    gi = 1.0f / (0.6f + 0.5f * drv);
    f[PU_LFO] = l1;
    for (i = 0; i < n; i++) {
        float x, bp, y;
        int wr;
        p1 += dt;
        wr = p1 >= 1.0f;
        if (wr)
            p1 -= 1.0f;
        if (sync && wr)
            p2 = p1 * ratio;
        else
            p2 = wrap1(p2 + dt2);
        if (p2 >= 1.0f)
            p2 = frac1(p2);
        ps = wrap1(ps + dt * 0.5f);
        x = pulse_b(p1, dt, wa) * m1 + pulse_b(p2, dt2, wa) * lv2 * 0.5f + pulse_b(ps, dt * 0.5f, 0.5f) * subl * 0.6f;
        wa += ws;
        y = svf_tick(&c, &i1, &i2, x * g, &bp);
        out[i] += soft(y) * gi * 0.52f;
    }
    f[PU_P1] = p1;
    f[PU_P2] = p2;
    f[PU_PS] = ps;
    f[PU_I1] = fm_flush(i1);
    f[PU_I2] = fm_flush(i2);
}

/* ====================================================================== FM ===
 * Four sine operators; op 3 has the feedback; op 0 is always a carrier. An algorithm is a modulation matrix
 * (who modulates whom) and a carrier mix, set once per block. */
enum { FM_P0 = S_A, FM_P1, FM_P2, FM_P3, FM_O3A, FM_O3B, FM_LP, FM_IX };
static const float FM_RATIO[12][4] = {
    {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 2.0f, 1.0f, 2.0f}, {1.0f, 1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 3.0f, 4.0f},
    {1.0f, 3.0f, 1.0f, 5.0f}, {1.0f, 0.5f, 1.0f, 2.0f}, {2.0f, 1.0f, 1.0f, 7.0f}, {1.0f, 14.0f, 1.0f, 3.5f},
    {1.0f, 1.41f, 1.0f, 3.53f}, {0.5f, 1.0f, 1.5f, 2.0f}, {1.0f, 4.0f, 2.0f, 8.0f}, {1.0f, 2.76f, 5.4f, 1.0f}};
static const float FM_R34[11] = {0.25f, 0.5f, 0.6f, 0.75f, 0.87f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 7.0f};
/* per algorithm: m01 m02 m03 m12 m13 m23 (j modulates i), carriers c0..c3 */
static const float FM_ALG[8][10] = {
    {1, 0, 0, 1, 0, 1, 1, 0, 0, 0},      /* 3 > 2 > 1 > 0: a stack */
    {1, 1, 0, 0, 0, 1, 1, 0, 0, 0},      /* (3 > 2) + 1 > 0 */
    {1, 0, 0, 0, 0, 1, 1, 0, 1, 0},      /* 1 > 0, 3 > 2: two pairs */
    {0, 0, 0, 1, 0, 1, 1, 1, 0, 0},      /* 3 > 2 > 1, and 0 alone */
    {1, 1, 1, 0, 0, 0, 1, 0, 0, 0},      /* 1, 2, 3 > 0 */
    {0, 0, 1, 0, 1, 1, 1, 1, 1, 0},      /* 3 > 0, 1, 2 */
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 0},      /* 3 > 2, 1, 0 */
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 1}};     /* all four in parallel */
static const float FM_DET[4] = {0.0f, 1.0f, -1.0f, 0.5f};

static RAMC NOIN void run_fm(eng_state_t *s, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    const float *rt = FM_RATIO[(int)(clampf(k[1], 0.0f, 1.0f) * 11.999f)];
    const float *al = FM_ALG[(int)(clampf(k[2], 0.0f, 1.0f) * 7.999f)];
    float r34 = FM_R34[(int)(clampf(k[5], 0.0f, 1.0f) * 10.999f)];
    float amt = f[0], det = f[3], fbk = f[4], idec = f[6], bri = f[7], vel = f[S_VEL];
    float d[4], p0 = f[FM_P0], p1 = f[FM_P1], p2 = f[FM_P2], p3 = f[FM_P3], o3a = f[FM_O3A], o3b = f[FM_O3B];
    float lp = f[FM_LP], ix, ix0 = f[FM_IX], ixs, tau, env, up, m01, m02, m03, m12, m13, m23, c0, c1, c2, c3, nc, fb, lpc;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        float r = rt[i] * (i >= 2u ? r34 : 1.0f);
        d[i] = fm_minf(hz * r * cents(FM_DET[i] * det * det * 14.0f) * ISR, 0.45f);
    }
    tau = 40.0f * b_exp2(-idec * 11.0f);
    env = b_exp(-f[S_T] / tau);
    ix = amt * amt * 2.0f * env * (0.55f + 0.45f * vel);     /* the index, in turns */
    ix = ix0 + (ix - ix0) * 0.5f;                             /* smoothed, then ramped over the block */
    f[FM_IX] = ix;
    up = bri > 0.5f ? 1.0f + (bri - 0.5f) * 2.0f : 1.0f;     /* brightness: the upper modulators harder */
    lpc = bri < 0.5f ? 1.0f - b_exp(-FM_TWO_PI * 500.0f * b_exp2(bri * 10.0f) * ISR) : 1.0f;
    m01 = al[0] * ix;
    m02 = al[1] * ix;
    m03 = al[2] * ix;
    m12 = al[3] * ix * up;
    m13 = al[4] * ix * up;
    m23 = al[5] * ix * up;
    ixs = ix > 1e-6f ? ix0 / ix : 1.0f;  /* the ramp: the matrix x (ix0 / ix .. 1) */
    ix = (1.0f - ixs) / (float)(int32_t)n;
    nc = al[6] + al[7] + al[8] + al[9];
    nc = 0.52f / (nc > 1.0f ? nc * 0.8f : 1.0f);
    c0 = al[6] * nc;
    c1 = al[7] * nc;
    c2 = al[8] * nc;
    c3 = al[9] * nc;
    fb = fbk * fbk * 0.3f;
    for (i = 0; i < n; i++) {
        float o0, o1, o2, o3, y;
        p0 = wrap1(p0 + d[0]);
        p1 = wrap1(p1 + d[1]);
        p2 = wrap1(p2 + d[2]);
        p3 = wrap1(p3 + d[3]);
        o3 = sin_t(p3 + (o3a + o3b) * fb);
        o3b = o3a;
        o3a = o3;
        ixs += ix;
        o2 = sin_t(p2 + m23 * o3 * ixs);
        o1 = sin_t(p1 + (m12 * o2 + m13 * o3) * ixs);
        o0 = sin_t(p0 + (m01 * o1 + m02 * o2 + m03 * o3) * ixs);
        y = c0 * o0 + c1 * o1 + c2 * o2 + c3 * o3;
        lp += lpc * (y - lp);
        out[i] += lp;
    }
    f[FM_P0] = p0;
    f[FM_P1] = p1;
    f[FM_P2] = p2;
    f[FM_P3] = p3;
    f[FM_O3A] = o3a;
    f[FM_O3B] = o3b;
    f[FM_LP] = fm_flush(lp);
}

/* ================================================================= CLUSTER === */
enum { CL_PH = S_A, CL_DR = CL_PH + 6, CL_SUB = CL_DR + 6, CL_I1, CL_I2 };
static const float CL_OFF[6] = {0.0f, 1.0f, -1.0f, 0.55f, -0.55f, 0.25f};

static RAMC void on_cluster(eng_state_t *s, int fresh)
{
    uint32_t i, seed = s->u[U_RNG];
    if (fresh)
        for (i = 0; i < 6u; i++)
            s->f[CL_PH + i] = 0.5f + 0.5f * rnd1(&seed);
    s->u[U_RNG] = seed;
}

static RAMC NOIN void run_cluster(eng_state_t *s, float hz, float *out, uint32_t n)
{
    float *f = s->f, t = f[S_T];
    float nw = 1.0f + f[0] * 5.0f, wenv = f[1], spr = f[2], uni = f[3], shp = f[4], drift = f[5], cut = f[6], sub = f[7];
    float dt = fm_minf(hz * ISR, 0.45f), ph[6], dtv[6], lv[6], sh, sp, g, ps = f[CL_SUB], i1 = f[CL_I1], i2 = f[CL_I2];
    uint32_t i, v, nv = (uint32_t)(int32_t)(nw + 0.999f), seed = s->u[U_RNG];
    int filt = cut < 0.98f;
    svf_t c;
    if (nv > 6u)
        nv = 6u;
    sh = shp + wenv * (1.0f - b_exp(-t * 1.4f));          /* the wave envelope: the shape moves over the note */
    if (sh > 1.0f)
        sh = 2.0f - sh;
    sp = spr * spr * 55.0f * b_exp(-t * uni * 3.0f);        /* cents; the unitor closes it */
    for (v = 0; v < nv; v++) {
        float dr = f[CL_DR + v];
        dr += (rnd1(&seed) - dr) * 0.004f;                    /* a slow random walk per wave */
        f[CL_DR + v] = dr;
        ph[v] = f[CL_PH + v];
        dtv[v] = fm_minf(dt * cents(sp * CL_OFF[v] + drift * drift * 400.0f * dr), 0.45f);
        lv[v] = clampf(nw - (float)(int32_t)v, 0.0f, 1.0f);
    }
    g = 0.34f / fm_sqrtf(nw);
    svf_set(&c, 60.0f * b_exp2(cut * 8.4f), 0.25f);
    for (i = 0; i < n; i++) {
        float x = 0.0f, bp;
        for (v = 0; v < nv; v++) {
            float p = ph[v] + dtv[v], q;
            p = wrap1(p);
            ph[v] = p;
            q = p + 0.5f;
            q = wrap1(q);
            x += lv[v] * (saw_b(p, dtv[v]) - sh * saw_b(q, dtv[v]));
        }
        x *= g;
        if (filt)
            x = svf_tick(&c, &i1, &i2, x, &bp);
        ps = wrap1(ps + dt * 0.5f);
        out[i] += x + sub * 0.3f * sin_t(ps);
    }
    for (v = 0; v < nv; v++)
        f[CL_PH + v] = ph[v];
    f[CL_SUB] = ps;
    f[CL_I1] = fm_flush(i1);
    f[CL_I2] = fm_flush(i2);
    s->u[U_RNG] = seed;
}

/* ================================================================= DIGITAL === */
enum { DG_P1 = S_A, DG_P2, DG_PR, DG_HPH, DG_HV };

static RAMC NOIN void run_digital(eng_state_t *s, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    float ws = f[0], dr = f[2], dig = f[3], bits = f[4], wv = f[5] * 3.0f, nz = f[6], fold = f[7];
    int oct = (int)(clampf(k[1], 0.0f, 1.0f) * 4.999f) - 2;
    float dt = fm_minf(hz * b_exp2((float)oct) * ISR, 0.45f), dt2, lv2 = 0.0f, rm = 0.0f, dtr, sg, fg, hr, ql, qi;
    float p1 = f[DG_P1], p2 = f[DG_P2], pr = f[DG_PR], hph = f[DG_HPH], hv = f[DG_HV], nzl = nz * nz * 0.5f;
    uint32_t i, seed = s->u[U_RNG];
    if (dr < 0.5f) {                     /* detune: a second oscillator */
        lv2 = fm_minf(1.0f, dr * 10.0f);
        dt2 = fm_minf(dt * cents(2.0f + dr * 60.0f), 0.45f);
    } else {                             /* ring mod: an inharmonic sine ring */
        lv2 = 1.0f;
        dt2 = fm_minf(dt * cents(32.0f), 0.45f);
        rm = (dr - 0.5f) * 2.0f;
    }
    dtr = fm_minf(dt * (1.0f + rm * 2.618f), 0.45f);
    sg = 0.25f + 1.4f * ws;              /* the shaper: a sine of the wave */
    fg = 1.0f + fold * 3.0f;
    hr = b_exp2(-dig * dig * 6.0f);    /* the hold rate: 1 .. 1/64 */
    ql = b_exp2(15.0f - 13.0f * bits);
    qi = 1.0f / ql;
    for (i = 0; i < n; i++) {
        float x;
        p1 = wrap1(p1 + dt);
        p2 = wrap1(p2 + dt2);
        pr = wrap1(pr + dtr);
        x = wave_morph(wv, p1, dt);
        if (lv2 > 0.0f)
            x = (x + lv2 * wave_morph(wv, p2, dt2)) * (1.0f / (1.0f + 0.6f * lv2));
        if (rm > 0.0f)
            x *= 1.0f - rm + rm * sin_t(pr) * 1.3f;
        x += nzl * rnd1(&seed);
        x = sin_t(x * sg);
        if (fold > 0.01f)
            x = fold_tri(x * fg);
        hph += hr;
        if (hph >= 1.0f) {               /* sample-rate reduction, then the bits */
            hph -= 1.0f;
            hv = fm_floorf(x * ql + 0.5f) * qi;
        }
        out[i] += hv * 0.5f;
    }
    f[DG_P1] = p1;
    f[DG_P2] = p2;
    f[DG_PR] = pr;
    f[DG_HPH] = hph;
    f[DG_HV] = hv;
    s->u[U_RNG] = seed;
}

/* =================================================================== PHASE ===
 * Phase distortion: a cosine read through a piecewise-linear phase map (four segments set per block), or the
 * windowed "resonance" waves. Two of them (phase shift, detune) and a sine sub. */
enum { PH_PA = S_A, PH_PB, PH_PS, PH_SW, PH_DX, PH_DY, PH_D };   /* u[2]: started */
typedef struct {
    float x1, x2, x3, y1, y2, y3, s0, s1, s2, s3;
} pdw_t;
INL void pdw_set(pdw_t *w, float x1, float y1, float x2, float y2, float x3, float y3, float y4)
{
    x1 = clampf(x1, 0.002f, 0.996f);
    x2 = clampf(x2, x1 + 0.001f, 0.997f);
    x3 = clampf(x3, x2 + 0.001f, 0.998f);
    w->x1 = x1;
    w->x2 = x2;
    w->x3 = x3;
    w->y1 = y1;
    w->y2 = y2;
    w->y3 = y3;
    w->s0 = y1 / x1;
    w->s1 = (y2 - y1) / (x2 - x1);
    w->s2 = (y3 - y2) / (x3 - x2);
    w->s3 = (y4 - y3) / (1.0f - x3);
}
INL float pdw(const pdw_t *w, float p)
{
    if (p < w->x2)
        return p < w->x1 ? p * w->s0 : w->y1 + (p - w->x1) * w->s1;
    return p < w->x3 ? w->y2 + (p - w->x2) * w->s2 : w->y3 + (p - w->x3) * w->s3;
}
INL float pd_wave(const pdw_t *w, int res, float rr, float p)
{
    float win;
    if (!res)
        return cos_t(pdw(w, p));
    win = res == 1 ? 1.0f - p : res == 2 ? 1.0f - fm_fabsf(2.0f * p - 1.0f) : fm_minf(1.0f, 2.0f - 2.0f * p);
    return 1.0f - win * (1.0f - cos_t(p * rr));         /* 1 at both ends: no step */
}

static RAMC NOIN void run_phase(eng_state_t *s, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f, t = f[S_T];
    float shift = f[0] * 0.75f, dist = f[1], pfil = f[2], tilt = f[3], denv = f[5], sub = f[6], det = f[7];
    int wf = (int)(clampf(k[4], 0.0f, 1.0f) * 7.999f), res = wf >= 5 ? wf - 4 : 0;
    float dt = fm_minf(hz * ISR, 0.45f), dtb = fm_minf(dt * cents(det * det * 25.0f), 0.45f);
    float sw = f[PH_SW], d, a, b, rr, ta, tl;
    float pa = f[PH_PA], pb = f[PH_PB], ps = f[PH_PS], dx = f[PH_DX], dy = f[PH_DY];
    int dotilt;
    uint32_t i;
    pdw_t w;
    sw = frac1(sw + (0.15f + pfil * 3.0f) * (float)(int32_t)n * ISR);   /* the phase filter: a moving sweep */
    f[PH_SW] = sw;
    d = clampf(dist + denv * 0.9f * b_exp(-t * 3.0f) + pfil * 0.45f * sin_t(sw), 0.0f, 1.0f);
    if (!s->u[2]) {                      /* (a fresh voice starts where it is going) */
        f[PH_D] = d;
        s->u[2] = 1;
    }
    d = f[PH_D] + clampf(d - f[PH_D], -0.025f, 0.025f);     /* slew-limited: the warp moves in small steps */
    f[PH_D] = d;
    a = 0.5f - 0.48f * d;
    b = 0.25f - 0.23f * d;
    rr = 1.0f + d * 14.0f;
    switch (wf) {
    case 0: pdw_set(&w, a, 0.5f, a + (1.0f - a) * 0.333f, 0.6667f, a + (1.0f - a) * 0.667f, 0.8333f, 1.0f); break;
    case 1: pdw_set(&w, a, 0.5f, 0.5f, 0.5f, 0.5f + a, 1.0f, 1.0f); break;
    case 2: a = 1.0f - 0.9f * d; pdw_set(&w, a * 0.5f, 0.5f, a, 1.0f, a + (1.0f - a) * 0.5f, 1.0f, 1.0f); break;
    case 3: pdw_set(&w, b, 0.5f, 0.5f, 1.0f, 0.5f + b, 1.5f, 2.0f); break;
    case 4: pdw_set(&w, b, 0.5f, 0.5f, 1.0f, 0.5f + a, 2.0f, 2.0f); break;
    default: pdw_set(&w, 0.25f, 0.25f, 0.5f, 0.5f, 0.75f, 0.75f, 1.0f); break;
    }
    ta = b_exp2((tilt - 0.5f) * 4.0f);   /* phase tilt: p / (p + a (1 - p)) */
    tl = ta - 1.0f;
    dotilt = fm_fabsf(tl) > 0.01f;
    for (i = 0; i < n; i++) {
        float qa, qb, y;
        pa = wrap1(pa + dt);
        pb = wrap1(pb + dtb);
        ps = wrap1(ps + dt * 0.5f);
        qa = pa;
        qb = pb + shift;
        qb = wrap1(qb);
        if (dotilt) {
            qa = qa / (1.0f + tl * (1.0f - qa));
            qb = qb / (1.0f + tl * (1.0f - qb));
        }
        y = (pd_wave(&w, res, rr, qa) + pd_wave(&w, res, rr, qb)) * 0.5f;
        y = dcb(y, &dx, &dy) + sub * 0.5f * sin_t(ps);
        out[i] += y * 0.55f;
    }
    f[PH_PA] = pa;
    f[PH_PB] = pb;
    f[PH_PS] = ps;
    f[PH_DX] = dx;
    f[PH_DY] = dy;
}

/* ================================================================= SPECTRA ===
 * Additive: a 256-sample single cycle is rebuilt in the background, 2 harmonics per block (48 in 24 blocks), from
 * the current parameters and pitch (band-limited), into one of three tables in the line; the newest table is
 * crossfaded in while the next one builds. Note-on builds the first harmonics at once.
 * u[2] build step, u[3] current table, u[4] old table, u[5] table being built. */
#define SP_N 256u
#define SP_NH 48
#define SP_HPB 2
#define SP_STEPS (SP_NH / SP_HPB)
#define SP_QUICK 12
#define SP_NROW 6
enum { SP_PH0 = S_A, SP_PH1, SP_PH2, SP_X, SP_XS, SP_G0, SP_G1, SP_G2, SP_SUM, SP_HZ, SP_TY, SP_FC, SP_PHS, SP_OE,
       SP_CNT, SP_TLT, SP_MOT, SP_TM };
static float s_sin256[SP_N];
static float s_spec[SP_NROW][SP_NH];   /* saw, square, pulse, organ, bell, glass (the vocals are formulas) */

void eng_tables_init(void)
{
    int i, h;
    for (i = 0; i < (int)SP_N; i++)
        s_sin256[i] = fm_sin_turns((float)i * (1.0f / (float)SP_N));
    for (h = 1; h <= SP_NH; h++) {
        float fh = (float)h, *sp;
        s_spec[0][h - 1] = 1.0f / fh;
        s_spec[1][h - 1] = (h & 1) ? 1.0f / fh : 0.0f;
        s_spec[2][h - 1] = fm_fabsf(fm_sin_turns(fh * 0.125f)) / fh * 1.4f;
        sp = &s_spec[3][h - 1];          /* organ: 16' 8' 5 1/3' 4' 2 2/3' 2' 1 3/5' 1' */
        *sp = h == 1 ? 1.0f : h == 2 ? 0.8f : h == 3 ? 0.7f : h == 4 ? 0.5f : h == 6 ? 0.45f : h == 8 ? 0.4f :
              h == 5 ? 0.15f : h == 10 ? 0.12f : h == 12 ? 0.2f : h == 16 ? 0.15f : 0.0f;
        sp = &s_spec[4][h - 1];          /* bell-ish: strong odd partials high up, a slow roll-off */
        *sp = h == 1 ? 1.0f : h == 3 ? 0.6f : h == 5 ? 0.25f : h == 7 ? 0.5f : h == 11 ? 0.45f : h == 13 ? 0.3f :
              h == 17 ? 0.25f : h == 19 ? 0.2f : h == 23 ? 0.12f : h == 29 ? 0.1f : 0.0f;
        sp = &s_spec[5][h - 1];          /* glass: the square numbers */
        *sp = h == 1 ? 1.0f : h == 4 ? 0.7f : h == 9 ? 0.55f : h == 16 ? 0.45f : h == 25 ? 0.3f : h == 36 ? 0.2f :
              h == 2 ? 0.2f : 0.0f;
    }
}

INL float sp_formant(float f, float f1, float f2, float f3, float g2, float g3)
{
    float a = (f - f1) * (1.0f / 110.0f), b = (f - f2) * (1.0f / 130.0f), c = (f - f3) * (1.0f / 180.0f);
    return 1.0f / (1.0f + a * a) + g2 / (1.0f + b * b) + g3 / (1.0f + c * c) + 0.02f;
}
INL float sp_base(int ty, int h, float f)
{
    float src = fm_rsqrtf((float)h);
    switch (ty) {
    case 0: return s_spec[0][h - 1];
    case 1: return s_spec[1][h - 1];
    case 2: return s_spec[2][h - 1];
    case 3: return s_spec[3][h - 1];
    case 4: return src * sp_formant(f, 800.0f, 1150.0f, 2900.0f, 0.5f, 0.25f);   /* "aah" */
    case 5: return s_spec[4][h - 1];
    case 6: return src * sp_formant(f, 450.0f, 800.0f, 2830.0f, 0.4f, 0.12f);    /* "ooh" */
    default: return s_spec[5][h - 1];
    }
}
/* one harmonic's amplitude from the snapshot; *norm gets the amplitude before filter / count / band / motion */
INL float sp_amp(const float *f, int h, float *norm)
{
    float fh = (float)h, fr = fh * f[SP_HZ], ty = f[SP_TY] * 7.0f, a, r, g, oe = f[SP_OE];
    int ti = (int)ty;
    a = sp_base(ti, h, fr);
    if (ti < 7)
        a += (sp_base(ti + 1, h, fr) - a) * (ty - (float)ti);
    if (h > 1) {                         /* odd / even */
        if (h & 1)
            a *= oe > 0.5f ? (1.0f - oe) * 2.0f : 1.0f;
        else
            a *= oe < 0.5f ? oe * 2.0f : 1.0f;
    }
    a *= b_exp2(f[SP_TLT] * b_log2(fh));   /* spectral tilt */
    *norm = 0.0f;
    g = f[SP_CNT] - fh + 1.0f;           /* the harmonic count */
    if (g <= 0.0f)
        return 0.0f;
    if (g < 1.0f)
        a *= g;
    g = (0.45f * SR - fr) * (1.0f / (0.05f * SR));   /* band limit */
    if (g <= 0.0f)
        return 0.0f;
    if (g < 1.0f)
        a *= g;
    *norm = a;                           /* (the level is set without the filter: closing it makes it quieter) */
    r = fr / f[SP_FC];                   /* the spectral filter: a steep low-pass with a bump */
    r *= r;
    g = 1.0f / (1.0f + r * r) + 0.6f * r / (1.0f + r * r * r);
    a *= g;
    if (f[SP_MOT] > 0.0f)                /* motion: each harmonic swells on its own slow cycle */
        a *= 1.0f - f[SP_MOT] + f[SP_MOT] * (0.5f + 0.5f * sin_t(f[SP_TM] * (0.15f + f[SP_MOT] * 1.2f) * (1.0f + fh * 0.13f) + fh * 0.37f));
    return a;
}
static RAMC void sp_snap(eng_state_t *s, float hz)
{
    float *f = s->f;
    f[SP_HZ] = hz;
    f[SP_TY] = clampf(f[0], 0.0f, 1.0f);
    f[SP_FC] = 110.0f * b_exp2(f[1] * 8.0f);
    f[SP_PHS] = f[2];
    f[SP_OE] = f[4];
    f[SP_CNT] = 1.0f + f[5] * (float)(SP_NH - 1);
    f[SP_TLT] = (f[6] - 0.5f) * 3.0f;
    f[SP_MOT] = f[7] * f[7];
    f[SP_TM] = f[S_T];
    f[SP_SUM] = 0.0f;
}
static RAMC void sp_add(eng_state_t *s, float *tab, int h)
{
    float nrm, a = sp_amp(s->f, h, &nrm), fh = (float)h;
    uint32_t idx, i, hu = (uint32_t)h;
    s->f[SP_SUM] += nrm * nrm;
    if (a == 0.0f)
        return;
    idx = (uint32_t)(int32_t)(s->f[SP_PHS] * (float)SP_N * frac1(fh * fh * 0.0731f + fh * 0.4142f)) & (SP_N - 1u);
    for (i = 0; i < SP_N; i++) {
        tab[i] += a * s_sin256[idx];
        idx = (idx + hu) & (SP_N - 1u);
    }
}
INL float sp_gain(float sum) { return fm_minf(0.2f * fm_rsqrtf(0.5f * sum + 1e-6f), 4.0f); }
static RAMC void sp_rotate(eng_state_t *s, float xs)
{
    uint32_t c = s->u[3], o = s->u[4], b = s->u[5];
    s->f[SP_G0 + b] = sp_gain(s->f[SP_SUM]);
    s->u[4] = c;
    s->u[3] = b;
    s->u[5] = o;
    s->f[SP_X] = 0.0f;
    s->f[SP_XS] = xs;
}

static RAMC void on_spectra(eng_state_t *s, float *line, float hz, int fresh)
{
    float *tab;
    uint32_t i;
    int h;
    if (fresh) {
        s->u[3] = 0;
        s->u[4] = 1;
        s->u[5] = 2;
        for (i = 0; i < 3u * SP_N; i++)
            line[i] = 0.0f;
    }
    sp_snap(s, hz);                   /* a quick table with the first harmonics, crossfaded in over a block */
    tab = line + s->u[5] * SP_N;
    for (i = 0; i < SP_N; i++)
        tab[i] = 0.0f;
    for (h = 1; h <= SP_QUICK; h++)
        sp_add(s, tab, h);
    for (h = SP_QUICK + 1; h <= SP_NH; h++) {   /* (the norm counts them all) */
        float nrm;
        (void)sp_amp(s->f, h, &nrm);
        s->f[SP_SUM] += nrm * nrm;
    }
    sp_rotate(s, 1.0f);
    s->u[2] = 0;
}

static RAMC NOIN void run_spectra(eng_state_t *s, float *line, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    uint32_t st = s->u[2], i;
    float *tb = line + s->u[5] * SP_N, *tc, *to, x0, x1, xs, gc, go, ch = f[3], dt, d1, d2, cl, p0, p1, p2;
    int j;
    if (st == 0u) {
        sp_snap(s, hz);
        for (i = 0; i < SP_N; i++)
            tb[i] = 0.0f;
    }
    for (j = 0; j < SP_HPB; j++)
        sp_add(s, tb, (int)(st * SP_HPB) + j + 1);
    if (++st >= (uint32_t)SP_STEPS) {
        sp_rotate(s, 1.0f / (float)(SP_STEPS - 4));
        st = 0;
    }
    s->u[2] = st;
    tc = line + s->u[3] * SP_N;
    to = line + s->u[4] * SP_N;
    gc = f[SP_G0 + s->u[3]];
    go = f[SP_G0 + s->u[4]];
    x0 = f[SP_X];
    x1 = fm_minf(1.0f, x0 + f[SP_XS]);
    f[SP_X] = x1;
    xs = (x1 - x0) / (float)(int32_t)n;
    dt = fm_minf(hz * ISR, 0.45f);
    d1 = dt * cents(ch * 14.0f);
    d2 = dt * cents(-ch * 11.0f);
    cl = fm_minf(1.0f, ch * 5.0f) * 0.7f;
    gc *= 1.0f / (1.0f + 1.2f * cl);
    go *= 1.0f / (1.0f + 1.2f * cl);
    p0 = f[SP_PH0];
    p1 = f[SP_PH1];
    p2 = f[SP_PH2];
    for (i = 0; i < n; i++) {
        float xp, fr, a, b, y;
        uint32_t i0, i1;
        p0 = wrap1(p0 + dt);
        xp = p0 * (float)SP_N;
        i0 = (uint32_t)(int32_t)xp;
        fr = xp - (float)(int32_t)i0;
        i0 &= SP_N - 1u;
        i1 = (i0 + 1u) & (SP_N - 1u);
        a = tc[i0] + (tc[i1] - tc[i0]) * fr;
        b = to[i0] + (to[i1] - to[i0]) * fr;
        if (cl > 0.0f) {                 /* chorus: two detuned reads of the same tables */
            p1 = wrap1(p1 + d1);
            p2 = wrap1(p2 + d2);
            xp = p1 * (float)SP_N;
            i0 = (uint32_t)(int32_t)xp;
            fr = xp - (float)(int32_t)i0;
            i0 &= SP_N - 1u;
            i1 = (i0 + 1u) & (SP_N - 1u);
            a += cl * (tc[i0] + (tc[i1] - tc[i0]) * fr);
            b += cl * (to[i0] + (to[i1] - to[i0]) * fr);
            xp = p2 * (float)SP_N;
            i0 = (uint32_t)(int32_t)xp;
            fr = xp - (float)(int32_t)i0;
            i0 &= SP_N - 1u;
            i1 = (i0 + 1u) & (SP_N - 1u);
            a += cl * (tc[i0] + (tc[i1] - tc[i0]) * fr);
            b += cl * (to[i0] + (to[i1] - to[i0]) * fr);
        }
        y = b * go + (a * gc - b * go) * x0;
        x0 += xs;
        out[i] += soft(y);
    }
    f[SP_PH0] = p0;
    f[SP_PH1] = p1;
    f[SP_PH2] = p2;
}

/* ==================================================================== DUAL === */
enum { DU_P1 = S_A, DU_P2, DU_O2, DU_I1, DU_I2 };
static const float DU_IV[11] = {-12.0f, -7.0f, -5.0f, 0.0f, 3.0f, 4.0f, 5.0f, 7.0f, 12.0f, 19.0f, 24.0f};

static RAMC NOIN void run_dual(eng_state_t *s, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f, t = f[S_T];
    float xf = f[0], w1 = f[1] * 3.999f, et = f[2], cm = f[3], w2 = f[5] * 3.999f, det = f[6], cut = f[7];
    float iv = DU_IV[(int)(clampf(k[4], 0.0f, 1.0f) * 10.999f)];
    float dt = fm_minf(hz * ISR, 0.45f), dt2 = fm_minf(hz * b_exp2(iv * (1.0f / 12.0f)) * cents(det * det * 50.0f) * ISR, 0.45f);
    float T = 0.01f * b_exp2(et * 9.0f), x0 = xf * (1.0f - b_exp(-t / T)),
          x1 = xf * (1.0f - b_exp(-(t + (float)(int32_t)n * ISR) / T)), xs = (x1 - x0) / (float)(int32_t)n;
    float p1 = f[DU_P1], p2 = f[DU_P2], o2 = f[DU_O2], i1 = f[DU_I1], i2 = f[DU_I2], cmi = cm * cm * 0.6f;
    uint32_t i;
    svf_t c;
    svf_set(&c, 50.0f * b_exp2(cut * 8.7f) * b_pow(hz * (1.0f / 261.6f), 0.3f), 0.3f);
    for (i = 0; i < n; i++) {
        float a, y, bp, q;
        p1 = wrap1(p1 + dt);
        p2 = wrap1(p2 + dt2);
        q = frac1(p1 + cmi * o2);        /* cross mod: osc 2 moves osc 1's phase */
        a = wave_morph(w1, q, dt);
        o2 = wave_morph(w2, p2, dt2);
        y = a + (o2 - a) * x0;
        x0 += xs;
        y = svf_tick(&c, &i1, &i2, y, &bp);
        out[i] += y * 0.5f;
    }
    f[DU_P1] = p1;
    f[DU_P2] = p2;
    f[DU_O2] = o2;
    f[DU_I1] = fm_flush(i1);
    f[DU_I2] = fm_flush(i2);
}

/* ================================================================= VOLTAGE === */
enum { VO_P = S_A, VO_DR = VO_P + 3, VO_PA = VO_DR + 3, VO_PS, VO_HUM, VO_NL, VO_AP, VO_SW = VO_AP + 4, VO_DX, VO_DY,
       VO_FB };

static RAMC NOIN void run_voltage(eng_state_t *s, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    float am = f[0], fold = f[1], pf = f[2], det = f[3], gnd = f[4], sub = f[5], drift = f[6], bias = (f[7] - 0.5f) * 1.2f;
    float dt = fm_minf(hz * ISR, 0.45f), d[3], p[3], pa = f[VO_PA], ps = f[VO_PS], hum = f[VO_HUM], nl = f[VO_NL];
    float ap0 = f[VO_AP], ap1 = f[VO_AP + 1], ap2 = f[VO_AP + 2], ap3 = f[VO_AP + 3], fb = f[VO_FB];
    float dx = f[VO_DX], dy = f[VO_DY], sw, fc, tg, ac, fg = 1.0f + fold * 5.0f, gl = gnd * gnd * 0.3f, pfm = pf * 0.8f;
    float spread = det * det * 30.0f;
    uint32_t i, v, seed = s->u[U_RNG];
    for (v = 0; v < 3u; v++) {
        float dr = f[VO_DR + v];
        dr += (rnd1(&seed) - dr) * 0.003f;
        f[VO_DR + v] = dr;
        p[v] = f[VO_P + v];
        d[v] = fm_minf(dt * cents(spread * (float)((int32_t)v - 1) + drift * drift * 500.0f * dr), 0.45f);
    }
    sw = frac1(f[VO_SW] + 0.23f * (float)(int32_t)n * ISR);  /* the phase filter: four all-passes, swept */
    f[VO_SW] = sw;
    fc = 300.0f * b_exp2(2.5f + 2.2f * sin_t(sw));
    tg = b_tan(FM_PI * fm_minf(fc, 15000.0f) * ISR);
    ac = (tg - 1.0f) / (tg + 1.0f);
    for (i = 0; i < n; i++) {
        float x = 0.0f, y, a;
        for (v = 0; v < 3u; v++) {
            p[v] = wrap1(p[v] + d[v]);
            x += saw_b(p[v], d[v]);
        }
        x *= 0.4f;
        pa = wrap1(pa + dt * 0.5f);
        ps = wrap1(ps + dt * 0.5f);
        x *= 1.0f - am + am * sin_t(pa) * 1.2f;               /* ampere mod (a sub-octave AM) */
        x += sub * 0.45f * pulse_b(ps, dt * 0.5f, 0.5f);
        x = fold_tri((x + bias) * fg);                         /* the folder, biased */
        if (pfm > 0.0f) {                                      /* the phase filter */
            float z = x + fb * 0.6f * pfm;
            y = ac * z + ap0; ap0 = z - ac * y; z = y;
            y = ac * z + ap1; ap1 = z - ac * y; z = y;
            y = ac * z + ap2; ap2 = z - ac * y; z = y;
            y = ac * z + ap3; ap3 = z - ac * y;
            fb = y;
            x = (x + pfm * y) * (1.0f / (1.0f + 0.5f * pfm));
        }
        hum += 50.0f * ISR;              /* ground noise: mains hum and a crackle */
        if (hum >= 1.0f)
            hum -= 1.0f;
        a = rnd1(&seed);
        nl += 0.05f * (a - nl);
        if (gl > 0.0f)
            x += gl * (sin_t(hum) + 0.5f * sin_t(hum * 3.0f) + 2.5f * nl);
        x = dcb(x, &dx, &dy);
        out[i] += x * 0.4f;
    }
    for (v = 0; v < 3u; v++)
        f[VO_P + v] = p[v];
    f[VO_PA] = pa;
    f[VO_PS] = ps;
    f[VO_HUM] = hum;
    f[VO_NL] = nl;
    f[VO_AP] = fm_flush(ap0);
    f[VO_AP + 1] = fm_flush(ap1);
    f[VO_AP + 2] = fm_flush(ap2);
    f[VO_AP + 3] = fm_flush(ap3);
    f[VO_FB] = fm_flush(fb);
    f[VO_DX] = dx;
    f[VO_DY] = dy;
    s->u[U_RNG] = seed;
}

/* ===================================================================== DNA ===
 * The wave: N (4..32) points from a hash of the wave number, blended with the next wave as the seed walks, kept in
 * line[0..N] (rebuilt only when it changes). u[2] the wave's N, f[DN_LW] the wave position it was built for. */
enum { DN_PH = S_A, DN_I1, DN_I2, DN_LW, DN_DX, DN_DY };
INL float dna_pt(uint32_t wv, uint32_t i)
{
    uint32_t x = (wv & 255u) * 0x9E3779B1u + i * 0x85EBCA77u + 0x27D4EB2Fu;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static RAMC NOIN void run_dna(eng_state_t *s, float *line, const float *k, float hz, float *out, uint32_t n)
{
    float *f = s->f;
    float cut = f[0], mod = f[2], nz = f[3], walk = f[4], res = f[6];
    int oct = (int)(clampf(k[7], 0.0f, 1.0f) * 4.999f) - 2;
    uint32_t np = 4u + (uint32_t)(int32_t)(clampf(k[5], 0.0f, 1.0f) * 28.0f + 0.5f), i, seed = s->u[U_RNG];
    float wpos = k[1] * 255.0f + f[S_T] * walk * walk * 12.0f, fnp = (float)(int32_t)np;
    float dt = fm_minf(hz * b_exp2((float)oct) * ISR, 0.45f), ph = f[DN_PH], i1 = f[DN_I1], i2 = f[DN_I2];
    float st, sm, nzl = nz * nz * 0.6f, dx = f[DN_DX], dy = f[DN_DY];
    svf_t c;
    wpos -= 256.0f * fm_floorf(wpos * (1.0f / 256.0f));
    if (fm_fabsf(wpos - f[DN_LW]) > 0.002f || np != s->u[2]) {   /* rebuild the wave */
        uint32_t a = (uint32_t)(int32_t)wpos;
        float fr = wpos - (float)(int32_t)a, mean = 0.0f, mx = 0.0f, g;
        for (i = 0; i < np; i++) {
            float x = dna_pt(a, i);
            x += (dna_pt(a + 1u, i) - x) * fr;
            line[i] = x;
            mean += x;
        }
        mean /= fnp;
        for (i = 0; i < np; i++) {
            line[i] -= mean;
            mx = fm_maxf(mx, fm_fabsf(line[i]));
        }
        g = 1.0f / fm_maxf(mx, 0.05f);
        for (i = 0; i < np; i++)
            line[i] *= g;
        line[np] = line[0];
        f[DN_LW] = wpos;
        s->u[2] = np;
    }
    /* the wave modifier: 0 steps, 0.5 straight lines, 1 smooth curves */
    st = mod < 0.5f ? 1.0f / fm_maxf(mod * 2.0f, 0.004f) : 1.0f;
    sm = mod > 0.5f ? (mod - 0.5f) * 2.0f : 0.0f;
    svf_set(&c, 60.0f * b_exp2(cut * 8.3f) * fm_sqrtf(hz * b_exp2((float)oct) * (1.0f / 261.6f)), res * 0.95f);
    for (i = 0; i < n; i++) {
        float x, fr, y, bp;
        uint32_t j;
        ph = wrap1(ph + dt);
        x = ph * fnp;
        j = (uint32_t)(int32_t)x;
        if (j >= np)
            j = np - 1u;
        fr = x - (float)(int32_t)j;
        fr = clampf((fr - 0.5f) * st + 0.5f, 0.0f, 1.0f);
        fr += (fr * fr * (3.0f - 2.0f * fr) - fr) * sm;
        y = line[j] + (line[j + 1u] - line[j]) * fr;
        y += nzl * rnd1(&seed);
        y = svf_tick(&c, &i1, &i2, y, &bp);
        y = dcb(y, &dx, &dy);
        out[i] += soft(y * 0.8f) * 0.6f;
    }
    f[DN_PH] = ph;
    f[DN_I1] = fm_flush(i1);
    f[DN_I2] = fm_flush(i2);
    f[DN_DX] = dx;
    f[DN_DY] = dy;
    s->u[U_RNG] = seed;
}

/* ================================================================= SAMPLER ===
 * g_synth_sample from start to end (or backwards), looping between loop in and loop out when the play position
 * crosses loop out. Every jump (note-on, loop, the end) is smoothed by a decaying offset (f[S_DC]).
 * u[2] playing, u[3] a jump is pending. */
enum { SM_POS = S_A, SM_LAST };
typedef struct {
    float a, b, li, lo, dir;
} smp_pts_t;
INL void sm_points(const float *k, float len, smp_pts_t *p)
{
    float l1 = len - 2.0f, a = k[0] * l1, b = k[3] * l1, t;
    p->li = clampf((k[1] + bip(k[5], 0.02f) * (1.0f / 128.0f)) * l1, 0.0f, l1);
    p->lo = clampf((k[2] + bip(k[6], 0.02f) * (1.0f / 128.0f)) * l1, 0.0f, l1);
    if (p->lo < p->li) {
        t = p->lo;
        p->lo = p->li;
        p->li = t;
    }
    if (k[4] > 0.5f) {                   /* reverse: from the end to the start */
        t = a;
        a = b;
        b = t;
    }
    p->a = a;
    p->b = b;
    p->dir = b >= a ? 1.0f : -1.0f;
}

static RAMC void on_sampler(eng_state_t *s, const float *k)
{
    smp_pts_t p;
    if (g_synth_sample.len < 4u) {
        s->u[2] = 0;
        return;
    }
    sm_points(k, (float)(int32_t)g_synth_sample.len, &p);
    s->f[SM_POS] = p.a;
    s->u[2] = 1;
    s->u[3] = 2;                         /* the first new sample takes over from the last one */
}

static RAMC NOIN void run_sampler(eng_state_t *s, float hz, float *out, uint32_t n)
{
    float *f = s->f, pos = f[SM_POS], last = f[SM_LAST], dc = f[S_DC];
    uint32_t i, play = s->u[2], jump = s->u[3], len = g_synth_sample.len;
    const uint8_t *d = g_synth_sample.data;
    smp_pts_t p;
    float rate, step, gain = 2.4f * f[7] * f[7], l1;
    int lim = gain > 1.0f;
    int loop;
    if (len < 4u || !d) {
        play = 0;
        len = 4u;
    }
    l1 = (float)(int32_t)len - 2.0f;
    sm_points(f, (float)(int32_t)len, &p);
    loop = p.lo - p.li > 32.0f;
    rate = hz * (1.0f / 261.6256f) * (SAMPLE_RATE / SR);
    step = rate * p.dir;
    for (i = 0; i < n; i++) {
        float y = 0.0f;
        if (play) {
            int32_t j = (int32_t)pos;
            float fr = pos - (float)j, y0 = ulaw(d[j]);
            y = (y0 + (ulaw(d[j + 1]) - y0) * fr) * gain;
            pos += step;
            if (loop) {
                if (step > 0.0f && pos >= p.lo && pos - step < p.lo) {
                    pos -= p.lo - p.li;
                    jump = 1;
                } else if (step < 0.0f && pos <= p.li && pos - step > p.li) {
                    pos += p.lo - p.li;
                    jump = 1;
                }
            }
            if ((step > 0.0f && pos >= p.b) || (step < 0.0f && pos <= p.b) || pos < 0.0f || pos > l1) {
                play = 0;
                jump = 1;
                pos = clampf(pos, 0.0f, l1);
            }
        }
        if (jump) {                      /* (applied to the sample after the jump) */
            if (jump == 2u) {
                dc += last - y;
                jump = 0;
            } else
                jump = 2;
        }
        last = y;
        out[i] += lim ? soft(y + dc) : y + dc;
        dc *= 0.996f;
    }
    f[SM_POS] = pos;
    f[SM_LAST] = last;
    f[S_DC] = fm_flush(dc);
    s->u[2] = play;
    s->u[3] = jump;
}

/* ============================================================ the dispatch === */
RAMC void eng_on(int e, eng_state_t *s, float *line, const float k[8], float hz, float vel)
{
    int fresh = s->u[U_TAG] != (uint32_t)e + 1u, i;
    if (hz < 4.0f)
        hz = 4.0f;
    if (fresh) {
        uint32_t seed = s->u[U_RNG];
        for (i = 0; i < 48; i++)
            s->f[i] = 0.0f;
        for (i = 0; i < 8; i++)
            s->u[i] = 0;
        s->u[U_RNG] = seed ? seed : 0x7F4A7C15u;
        s->u[U_TAG] = (uint32_t)e + 1u;
        for (i = 0; i < 8; i++)
            s->f[i] = k[i];
    }
    s->f[S_T] = 0.0f;
    s->f[S_VEL] = vel;
    switch (e) {
    case E_STRING: on_string(s, line, hz, fresh); break;
    case E_CLUSTER: on_cluster(s, fresh); break;
    case E_SPECTRA: on_spectra(s, line, hz, fresh); break;
    case E_SAMPLER: on_sampler(s, k); break;
    default: break;
    }
}

RAMC void eng_run(int e, eng_state_t *s, float *line, const float k[8], float hz, float *out, uint32_t n)
{
    int i;
    if (n == 0u)
        return;
    if (s->u[U_TAG] != (uint32_t)e + 1u)   /* the engine changed under a sounding voice */
        eng_on(e, s, line, k, hz, 0.7f);
    for (i = 0; i < 8; i++)
        s->f[i] += (k[i] - s->f[i]) * 0.3f;
    hz = clampf(hz, 4.0f, 16000.0f);
    switch (e) {
    case E_STRING: run_string(s, line, hz, out, n); break;
    case E_PULSE: run_pulse(s, k, hz, out, n); break;
    case E_FM: run_fm(s, k, hz, out, n); break;
    case E_CLUSTER: run_cluster(s, hz, out, n); break;
    case E_DIGITAL: run_digital(s, k, hz, out, n); break;
    case E_PHASE: run_phase(s, k, hz, out, n); break;
    case E_SPECTRA: run_spectra(s, line, hz, out, n); break;
    case E_DUAL: run_dual(s, k, hz, out, n); break;
    case E_VOLTAGE: run_voltage(s, hz, out, n); break;
    case E_DNA: run_dna(s, line, k, hz, out, n); break;
    case E_SAMPLER: run_sampler(s, hz, out, n); break;
    default: break;
    }
    s->f[S_T] += (float)(int32_t)n * ISR;
}
