/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the module pictures (included by app.c after ui_base.inc). Each draws into the top canvas, inside
 * the picture area (x 0..239, y PIC_Y..PIC_Y+PIC_H-1), over the black the caller cleared. The rule they all keep:
 * what K1..K4 move is drawn in that knob's colour (kcol(i): bright for 0.7 s after a turn), what the GLO row moves
 * in its dim colour (gcol(i): bright while GLO is held), so the picture says which knob does what. ui_t (seconds)
 * and the live view (the voice's envelope and note, the LFO, the sequencer step, the balls) make them move.
 * k[8]: the module's parameters as heard now (0..1). All original drawings. */

static uint16_t gcol(int i) { return ui_shift ? KC[i & 3] : kdim(i); }
static float hsh(uint32_t x)             /* 0..1 */
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (float)(x & 0xFFFFu) * (1.0f / 65535.0f);
}
static float sn(float turns) { return fm_sin_turns(turns); }
static int32_t ri(float v) { return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f); }
static int sel(float k, int n) { int i = (int)(k * (float)n); return i < 0 ? 0 : i >= n ? n - 1 : i; }
/* five shapes, s 0..4 crossfading: sine, triangle, saw, square, narrow pulse; p in turns */
static float shp(float p, float s)
{
    float ph = frac1(p), v[5];
    int i = (int)s;
    float f = s - (float)i;
    v[0] = sn(ph);
    v[1] = ph < 0.25f ? ph * 4.0f : ph < 0.75f ? 2.0f - ph * 4.0f : ph * 4.0f - 4.0f;
    v[2] = 1.0f - 2.0f * ph;
    v[3] = ph < 0.5f ? 1.0f : -1.0f;
    v[4] = ph < 0.15f ? 1.0f : -0.6f;
    if (i >= 4)
        return v[4];
    if (i < 0)
        return v[0];
    return v[i] + (v[i + 1] - v[i]) * f;
}
static void bar_up(int32_t x, int32_t ybot, int32_t w, float h, uint16_t c)
{
    int32_t hh = ri(h);
    if (hh > 0)
        cv_rect(x, ybot - hh, w, hh, c);
}
static void vbar_meter(int32_t x, int32_t y0, int32_t h, float v, uint16_t c, const char *l)   /* a labelled level */
{
    frame_rect(x, y0, 8, h, G2);
    bar_up(x + 2, y0 + h - 2, 4, v * (float)(h - 4), c);
    if (l)
        ctext(x + 4, y0 + h + 2, &FONT_T, l, c);
}
/* the LFO's target: three modules and their eight parameters, the chosen ones lit */
static void target_row(float km, float kp, int32_t y)
{
    static const char *const M[3] = {"ENGINE", "ENV", "FX"};
    int m = sel(km, 3), p = sel(kp, 8), i;
    for (i = 0; i < 3; i++) {
        int32_t x = 14 + i * 50;
        if (i == m) {
            cv_rect(x, y, 46, 13, kcol(2));
            ctext(x + 23, y + 3, &FONT_T, M[i], BG);
        } else {
            frame_rect(x, y, 46, 13, G2);
            ctext(x + 23, y + 3, &FONT_T, M[i], G3);
        }
    }
    for (i = 0; i < 8; i++) {
        int32_t x = 170 + (i & 3) * 16, yy = y - 4 + (i >> 2) * 11;
        if (i == p)
            disc(x, yy + 4, 4, kcol(3));
        else
            ring(x, yy + 4, 3, i < 4 ? KD[i & 3] : G2);
    }
}

/* ================================================================== the engines === */
static void pic_string(const float *k, float env)
{
    int i;
    float a = 1.2f + env * 24.0f, hz = 1.5f + 5.0f * k[0], pos = 0.1f + 0.8f * k[4], br = k[5];
    int32_t x0 = 30, x1 = 210, cy = 70, px = x0, py = cy, qx = x0, qy = cy + 10, xp;
    /* the decay of the impulse: a green curve in the corner */
    for (i = 0; i < 20; i++) {
        float u = (float)i / 19.0f, v = fm_expf(-u * (8.0f - 7.5f * k[1]));
        cv_rect(8 + i * 2, 40 - ri(v * 14.0f), 2, 2, kcol(1));
    }
    /* the body: two hollows that ring (GLO K3) */
    ring(100, 118, 9 + ri(9.0f * k[6]), gcol(2));
    ring(140, 118, 6 + ri(6.0f * k[6]), gcol(2));
    /* the pegs: tension turns them */
    for (i = 0; i < 2; i++) {
        int32_t x = i ? x1 + 8 : x0 - 8, ex, ey;
        disc(x, cy, 6, kcol(0));
        rot(0.0f, -6.0f - 3.0f * k[0], k[0] * 0.4f + (i ? 0.5f : 0.0f), x, cy, &ex, &ey);
        tline(x, cy, ex, ey, kcol(0));
    }
    /* the strings */
    for (i = 1; i <= 48; i++) {
        float u = (float)i / 48.0f, t = ui_t;
        float v = sn(u * 0.5f) * sn(t * hz + 0.25f) + br * 0.5f * sn(u) * sn(t * hz * 2.01f + 0.25f) +
                  br * 0.3f * sn(u * 1.5f) * sn(t * hz * 3.02f);
        float w = sn(u * 0.5f) * sn(t * (hz + 0.3f + 2.0f * k[2]) + 0.25f);
        int32_t x = x0 + ri((float)(x1 - x0) * u), y = cy - ri(a * v), y2 = cy + 10 - ri(a * w * k[2]);
        if (k[7] > 0.4f)
            cv_line(px, py - 1, x, y - 1, mix565(K_ORANGE, BG, 1.0f - (k[7] - 0.4f)));
        tline(px, py, x, y, K_WHITE);
        if (k[2] > 0.02f)
            cv_line(qx, qy, x, y2, mix565(KC[2], BG, 0.75f - 0.5f * k[2]));
        px = x;
        py = y;
        qx = x;
        qy = y2;
    }
    /* the exciter at the pluck point: a pick (orange low) or a bow (orange high) */
    xp = x0 + ri((float)(x1 - x0) * pos);
    cv_rect(xp, cy - 26, 1, 4, gcol(0));
    if (k[3] < 0.5f) {
        int32_t s = 5 + ri(4.0f * (1.0f - k[3]));
        for (i = 0; i < s; i++)
            cv_rect(xp - (s - i) / 2, cy - 22 + i, s - i + 1, 1, kcol(3));
    } else {
        int32_t L = 18 + ri(40.0f * (k[3] - 0.5f)), off = ri(sn(ui_t * 0.7f) * 10.0f * env);
        cv_rect(xp - L + off, cy - 18, 2 * L, 3, kcol(3));
        cv_rect(xp - L + off, cy - 15, 2 * L, 1, G3);
    }
}

static void hseg(int32_t x0, int32_t x1, int32_t y, uint16_t c)
{
    if (x0 < 8) x0 = 8;
    if (x1 > 232) x1 = 232;
    if (x1 > x0)
        cv_rect(x0, y, x1 - x0, 2, c);
}
static void vseg(int32_t x, int32_t y0, int32_t y1, uint16_t c)
{
    if (x >= 8 && x <= 232)
        cv_rect(x, y0 < y1 ? y0 : y1, 2, (y0 < y1 ? y1 - y0 : y0 - y1) + 2, c);
}
static void pulse_row(float phase, float cyc, float duty, int32_t top, int32_t bot, uint16_t hi, uint16_t lo)
{
    int c;
    float sc = frac1(phase);
    for (c = -1; c < 8; c++) {
        float xs = 8.0f + ((float)c - sc) * cyc;
        int32_t a = ri(xs), b = ri(xs + duty * cyc), e = ri(xs + cyc);
        if (a > 232)
            break;
        hseg(a, b, top, hi);
        vseg(b, top, bot, lo);
        hseg(b, e, bot, lo);
        vseg(e, bot, top, lo);
    }
}
static void pic_pulse(const float *k, float env)
{
    int i;
    float duty = clampf(0.5f - 0.46f * k[0] + k[3] * 0.25f * sn(ui_t * (0.2f + 1.2f * k[3])), 0.03f, 0.97f);
    int32_t py = 0;
    pulse_row(ui_t * 0.3f, 72.0f, duty, 28, 56, kcol(0), mix565(KC[0], BG, 0.5f));
    if (k[1] < 0.6f) {                   /* a second pulse: detuned, drifting */
        pulse_row(ui_t * (0.3f + 0.4f * k[1]), 72.0f, duty, 66, 80, kcol(1), mix565(KC[1], BG, 0.5f));
    } else {                             /* hard synced: restarts with the first */
        float r = 1.0f + 3.75f * (k[1] - 0.6f) / 0.4f;
        pulse_row(ui_t * 0.3f * r, 72.0f / r, duty, 66, 80, kcol(1), mix565(KC[1], BG, 0.5f));
    }
    if (k[5] > 0.03f)                    /* the sub (GLO K2) */
        pulse_row(ui_t * 0.15f, 144.0f, 0.5f, 88, 88 + ri(8.0f * k[5]), gcol(1), gcol(1));
    /* the filter: cutoff (white), resonance (GLO K1), its envelope (GLO K3) */
    {
        float fc = clampf(0.08f + 0.85f * k[2] + env * k[6] * 0.3f, 0.05f, 0.98f), q = k[4];
        for (i = 0; i <= 60; i++) {
            float f = (float)i / 60.0f, r = f / fc, r2 = r * r, d = (f - fc) / 0.05f;
            float v = 1.0f / (1.0f + r2 * r2) + q * 1.1f * fm_expf(-d * d);
            int32_t x = 10 + i * 220 / 60, y = 138 - ri(clampf(v, 0.0f, 1.6f) * 22.0f);
            if (i)
                tline(x - 220 / 60, py, x, y, kcol(2));
            py = y;
        }
        disc(10 + ri(fc * 220.0f), 138 - ri((1.0f + q * 1.1f) * 0.5f * 22.0f) - 4, 3, gcol(0));
    }
}

static void pic_fm(const float *k, float env)
{
    static const uint8_t E[8][3][2] = {   /* modulator -> target (0xFF none); op 0 is always a carrier */
        {{3, 2}, {2, 1}, {1, 0}}, {{3, 1}, {2, 1}, {1, 0}}, {{3, 2}, {1, 0}, {255, 0}}, {{3, 2}, {2, 1}, {255, 0}},
        {{3, 0}, {2, 0}, {1, 0}}, {{3, 0}, {3, 1}, {3, 2}}, {{3, 2}, {255, 0}, {255, 0}}, {{255, 0}, {255, 0}, {255, 0}}};
    int alg = sel(k[2], 8), j, e, nc = 0, ci = 0;
    int tgt[4] = {-1, -1, -1, -1};
    float X[4], Y[4];
    char t[8];
    for (e = 0; e < 3; e++)
        if (E[alg][e][0] != 255 && tgt[E[alg][e][0]] < 0)
            tgt[E[alg][e][0]] = E[alg][e][1];
    for (j = 0; j < 4; j++)
        nc += tgt[j] < 0;
    for (j = 0; j < 4; j++) {
        if (tgt[j] < 0) {                /* a carrier: on the ground line */
            X[j] = 120.0f + ((float)ci - (float)(nc - 1) * 0.5f) * 56.0f;
            Y[j] = 104.0f;
            ci++;
        }
    }
    for (j = 1; j < 4; j++) {
        if (tgt[j] >= 0) {               /* a modulator orbits what it modulates */
            float r = 12.0f + 26.0f * k[0] * (j == 3 ? 0.8f : 1.0f), sp = 0.08f + (0.1f + hsh((uint32_t)(sel(k[1], 12) * 4 + j)) * 0.5f) * (0.3f + k[1]);
            float a = ui_t * sp * (j & 1 ? 1.0f : -1.0f) + (float)j * 0.3f;
            int tj = tgt[j];
            ring(ri(X[tj]), ri(Y[tj]), ri(r), mix565(KC[0], BG, 0.55f - 0.3f * (float)lit(0)));
            X[j] = X[tj] + sn(a) * r;
            Y[j] = Y[tj] - sn(a + 0.25f) * r * 0.8f;
        }
    }
    for (e = 0; e < 3; e++) {            /* the topology: who modulates whom */
        int a = E[alg][e][0], b = E[alg][e][1];
        if (a != 255)
            cv_line(ri(X[a]), ri(Y[a]), ri(X[b]), ri(Y[b]), kcol(2));
    }
    if (alg == 5) {                      /* one modulator, three carriers: lines to all */
        for (j = 0; j < 3; j++)
            cv_line(ri(X[3]), ri(Y[3]), ri(X[j]), ri(Y[j]), kcol(2));
    }
    cv_rect(20, 118, 200, 1, G2);
    for (j = 0; j < 4; j++) {
        int32_t x = ri(X[j]), y = ri(Y[j]);
        if (tgt[j] < 0) {
            if (k[3] > 0.02f)            /* detune: a ghost beside it */
                ring(x + ri(sn(ui_t * 0.5f) * 6.0f * k[3]), y, 8 + ri(env * 5.0f), kcol(3));
            disc(x, y, 6 + ri(env * 5.0f), K_WHITE);
        } else {
            disc(x, y, 5, kcol(1));
        }
    }
    if (k[4] > 0.03f)                    /* feedback on op 4 (GLO K1) */
        arc(ri(X[3]), ri(Y[3]) - 7, 5.0f, 0.0f, 0.2f + 0.8f * k[4], gcol(0), 1);
    copy_text(t, "ALG ", 8);
    num_text(alg + 1, t + 4);
    cv_text(8, 24, &FONT_T, t, kcol(2));
}

static void pic_cluster(const float *k, float env)
{
    int n = 1 + sel(k[0], 6), i, j;
    float amp = 9.0f + 8.0f * env, sq = k[4];
    int32_t px = 0, py = 0;
    for (j = 0; j <= 40; j++) {          /* the wave envelope: the shape moving over the note (green) */
        float u = (float)j / 40.0f, v = 0.5f + 0.5f * sn(u * (0.5f + 2.0f * k[1]) - ui_t * 0.1f * k[1]);
        int32_t x = 16 + j * 5, y = 40 - ri(v * 10.0f * (0.2f + k[1]));
        if (j)
            cv_line(px, py, x, y, kcol(1));
        px = x;
        py = y;
    }
    for (i = 0; i < n; i++) {
        float off = (float)i - (float)(n - 1) * 0.5f;
        float ph = ui_t * (0.05f + k[2] * 0.5f) * off + hsh((uint32_t)i) * k[5] * sn(ui_t * 0.1f + (float)i);
        int32_t cy = 78 + ri(off * 8.0f * (1.0f - 0.7f * k[3]));
        uint16_t c = mix565(KC[0], K_WHITE, (float)i / 6.0f);
        if (!lit(0))
            c = mix565(c, BG, 0.25f);
        for (j = 0; j <= 48; j++) {
            float p = (float)j / 16.0f + ph, s = 1.0f - 2.0f * frac1(p), v = s * (1.0f - sq) + (frac1(p) < 0.5f ? 1.0f : -1.0f) * sq;
            int32_t x = 16 + j * 4, y = cy - ri(v * amp * 0.5f);
            if (j)
                cv_line(px, py, x, y, c);
            px = x;
            py = y;
        }
        if (k[3] > 0.02f)                /* the unitor pulls them together (orange) */
            cv_line(212, cy, 212 + ri(16.0f * k[3]), 78, kcol(3));
    }
    for (i = 0; i < 2; i++) {            /* the spread: arrows apart (white) */
        int32_t w = 6 + ri(30.0f * k[2]), x = i ? 120 + w : 120 - w;
        cv_rect(i ? 120 : x, 118, w, 2, kcol(2));
        disc(x, 119, 3, kcol(2));
    }
    if (k[7] > 0.03f)                    /* the sub (GLO K4) */
        cv_rect(16, 132, ri(200.0f * k[7]), 3, gcol(3));
}

static void pic_digital(const float *k, float env)
{
    int oct = sel(k[1], 5) - 2, i, levels = 1 << (2 + ri((1.0f - k[4]) * 6.0f));
    float cyc = oct < 0 ? 1.0f / (float)(1 << -oct) : (float)(1 << oct), g = 1.0f + 3.0f * k[0];
    int32_t w = 1 + ri(k[3] * k[3] * 26.0f), x, px = 0, py = 0, amp = 22 + ri(env * 12.0f);
    float drift = ui_t * 0.1f;
    for (x = 0; x <= 216; x += 3) {      /* the shaped wave, smooth (blue) */
        float p = (float)x / 216.0f * cyc * 2.0f + drift, v = sn(sn(p) * g * 0.25f);
        int32_t y = 76 - ri(v * (float)amp);
        if (x)
            cv_line(12 + px, py, 12 + x, y, kcol(0));
        px = x;
        py = y;
    }
    if (k[2] > 0.03f) {                  /* the 2nd: detuned or ringing (white) */
        for (x = 0; x <= 216; x += 4) {
            float p = (float)x / 216.0f * cyc * 2.0f * (1.0f + k[2] * 0.5f) + drift, v = sn(p);
            cv_rect(12 + x, 76 - ri(v * (float)amp * k[2]), 2, 2, kcol(2));
        }
    }
    for (x = 0; x < 216; x += w) {       /* what comes out: held (orange) and stepped (GLO K1) */
        float p = (float)x / 216.0f * cyc * 2.0f + drift, v = sn(sn(p) * g * 0.25f);
        float q = (float)ri(v * (float)(levels / 2)) / (float)(levels / 2 > 0 ? levels / 2 : 1);
        int32_t y = 76 - ri(q * (float)amp), ww = w + x > 216 ? 216 - x : w;
        cv_rect(12 + x, y, ww > 1 ? ww - (w > 3) : 1, 2, kcol(3));
    }
    for (i = 0; i < 5; i++)              /* the octave (green) */
        disc(96 + i * 12, 126, i == oct + 2 ? 4 : 2, i == oct + 2 ? kcol(1) : G2);
    if (k[6] > 0.03f) {                  /* noise sparkles (GLO K3) */
        uint32_t f = (uint32_t)(ui_t * 20.0f);
        for (i = 0; i < ri(k[6] * 24.0f); i++)
            cv_rect(12 + ri(hsh(f * 77u + (uint32_t)i) * 216.0f), 40 + ri(hsh(f * 91u + (uint32_t)i) * 72.0f), 2, 2, gcol(2));
    }
}

static void pic_phase(const float *k, float env)
{
    float amt = clampf(k[1] + k[2] * 0.3f * sn(ui_t * 0.3f), 0.0f, 1.0f), d = 0.5f - 0.46f * amt;
    int32_t i, px = 0, py = 0, qx = 0, qy = 0, bx = 14, by = 30, B = 84;
    frame_rect(bx, by, B, B, G2);
    cv_line(bx, by + B - 1, bx + B - 1, by, G1);
    for (i = 0; i <= 32; i++) {          /* the phase, bent (green) */
        float u = (float)i / 32.0f, v = u < d ? 0.5f * u / d : 0.5f + 0.5f * (u - d) / (1.0f - d);
        int32_t x = bx + ri(u * (float)(B - 1)), y = by + B - 1 - ri(v * (float)(B - 1));
        if (i)
            tline(px, py, x, y, kcol(1));
        px = x;
        py = y;
    }
    {                                    /* the filter's sweep runs along it (white) */
        float u = 0.5f + 0.45f * sn(ui_t * (0.1f + 0.6f * k[2]));
        float v = u < d ? 0.5f * u / d : 0.5f + 0.5f * (u - d) / (1.0f - d);
        disc(bx + ri(u * (float)(B - 1)), by + B - 1 - ri(v * (float)(B - 1)), 4, kcol(2));
    }
    for (i = 0; i <= 64; i++) {          /* the output, and the second oscillator shifted (blue) */
        float u = (float)i / 32.0f, f = frac1(u), v = f < d ? 0.5f * f / d : 0.5f + 0.5f * (f - d) / (1.0f - d);
        float o = -sn(v + 0.25f) * (1.0f - k[3] * 0.4f * (1.0f - 2.0f * f));
        float f2 = frac1(u + k[0] * 0.75f), v2 = f2 < d ? 0.5f * f2 / d : 0.5f + 0.5f * (f2 - d) / (1.0f - d);
        int32_t x = 112 + i * 120 / 64, y = 72 - ri(o * (22.0f + 10.0f * env)), y2 = 72 - ri(-sn(v2 + 0.25f) * 14.0f);
        if (i) {
            cv_line(qx, qy, x, y2, kcol(0));
            tline(px, py, x, y, K_WHITE);
        }
        px = x;
        py = y;
        qx = x;
        qy = y2;
    }
    cv_line(112, 72 + ri(k[3] * 16.0f), 232, 72 - ri(k[3] * 16.0f), kcol(3));   /* the tilt (orange) */
    {
        static const char *const W[8] = {"SAW", "SQUARE", "PULSE", "DBL SINE", "SAW-PULSE", "RES I", "RES II", "RES III"};
        ctext(172, 120, &FONT_T, W[sel(k[4], 8)], gcol(0));
    }
}

static float spec_amp(int type, int n)   /* the eight wave types' harmonics, n from 1 */
{
    float fn = (float)n;
    switch (type) {
    case 0: return 1.0f / fn;
    case 1: return (n & 1) ? 1.0f / fn : 0.0f;
    case 2: return fm_fabsf(sn(fn * 0.1f)) / fn * 2.0f;
    case 3: return (n == 1 || n == 2 || n == 3 || n == 4 || n == 6 || n == 8) ? 0.8f / fm_sqrtf(fn) : 0.05f;
    case 4: { float a = (fn - 3.0f) / 1.5f, b = (fn - 8.0f) / 2.5f; return 0.25f / fn + 0.7f * fm_expf(-a * a) + 0.4f * fm_expf(-b * b); }
    case 5: return hsh((uint32_t)n * 13u) > 0.6f ? 0.9f / fm_sqrtf(fn) : 0.04f;
    case 6: return n <= 2 ? 1.0f / fn : 0.1f / fn;
    default: return ((n & 1) && n > 4) ? 0.6f : 0.08f;
    }
}
static void pic_spectra(const float *k, float env)
{
    float s = k[0] * 7.0f, f = s - (float)(int)s, fc = 1.0f + k[1] * 23.0f;
    int t0 = (int)s, t1 = t0 < 7 ? t0 + 1 : 7, nh = 1 + sel(k[5], 24), i;
    int32_t xc;
    for (i = 1; i <= 24; i++) {
        float a = spec_amp(t0, i) * (1.0f - f) + spec_amp(t1, i) * f, r = (float)i / fc;
        float lp = 1.0f / (1.0f + r * r * r * r) + (fm_fabsf((float)i - fc) < 1.0f ? 0.3f : 0.0f);
        float oe = (i & 1) ? 1.0f - fm_maxf(0.0f, k[4] - 0.5f) * 2.0f : 1.0f - fm_maxf(0.0f, 0.5f - k[4]) * 2.0f;
        float h = clampf(a * lp * oe, 0.0f, 1.0f) * (60.0f + 30.0f * env);
        int32_t x = 12 + (i - 1) * 9;
        uint16_t c = i > nh ? G1 : kcol(0);
        if (i > nh)
            h *= 0.5f;
        bar_up(x, 118, 6, h, c);
        if (k[3] > 0.03f && i <= nh)     /* chorus: a second read beside it (orange) */
            bar_up(x + 6, 118, 1, h * (0.7f + 0.3f * sn(ui_t * 0.4f + (float)i * 0.1f)), kcol(3));
        if (h > 2.0f && i <= nh)         /* the phase: light running over the tops (white) */
            cv_rect(x, 118 - ri(h) - 2 - ri(2.0f * (1.0f + sn(ui_t * 0.5f * (0.2f + k[2]) + (float)i * k[2] * 0.2f))), 6, 1, kcol(2));
    }
    xc = 12 + ri((fc - 1.0f) * 9.0f) + 3;   /* the spectral filter's cutoff (green) */
    cv_rect(xc, 30, 2, 92, kcol(1));
    disc(xc + 1, 30, 3, kcol(1));
    cv_rect(10, 120, 220, 1, G2);
    {
        static const char *const W[8] = {"SAW", "SQUARE", "PULSE", "ORGAN", "AAH", "BELL", "OOH", "GLASS"};
        ctext(120, 126, &FONT_T, W[sel(k[0], 8)], kcol(0));
    }
}

static void pic_dual(const float *k, float env)
{
    int i;
    float pos = clampf(k[0] + (1.0f - env) * (0.15f + 0.6f * (1.0f - k[2])) * (1.0f - k[0]), 0.0f, 1.0f);
    int32_t p1x = 0, p1y = 0, p2x = 0, p2y = 0;
    uint16_t c1 = mix565(K_WHITE, G2, pos), c2 = mix565(G2, K_WHITE, pos);
    for (i = 0; i <= 64; i++) {
        float u = (float)i / 32.0f + ui_t * 0.05f, iv = (float)sel(k[4], 11) - 5.0f;
        float v1 = shp(u + k[3] * 0.2f * sn(u * (1.0f + iv * 0.1f)), k[1] * 4.0f);
        float v2 = shp(u * fm_powf(2.0f, iv / 12.0f) * (1.0f + k[6] * 0.03f), k[5] * 4.0f);
        int32_t x = 20 + i * 180 / 64, y1 = 48 - ri(v1 * 16.0f), y2 = 106 - ri(v2 * 16.0f);
        if (i) {
            tline(p1x, p1y, x, y1, c1);
            tline(p2x, p2y, x, y2, c2);
        }
        if (k[3] > 0.05f && (i % (12 - sel(k[3], 10))) == 0)   /* cross mod: the threads between (orange) */
            cv_line(x, y1 + 3, x, y2 - 3, kcol(3));
        p1x = x;
        p1y = y1;
        p2x = x;
        p2y = y2;
    }
    cv_text(4, 44, &FONT_T, "1", kcol(1));
    cv_text(4, 102, &FONT_T, "2", gcol(1));
    /* the crossfade: a slider between the two (blue), its travel time (white) */
    cv_rect(219, 32, 2, 90, G2);
    disc(220, 32 + ri(pos * 90.0f), 6, kcol(0));
    cv_rect(228, 32, 3, ri(90.0f * k[2]) + 1, kcol(2));
}

static void pic_voltage(const float *k, float env)
{
    int i;
    float wob = env * sn(ui_t * (0.3f + 6.0f * k[0])) * (0.4f + 0.6f * k[0]);
    float noise = k[4] * (hsh((uint32_t)(ui_t * 30.0f)) - 0.5f) * 0.06f;
    int32_t ex, ey, px = 0, py = 0;
    arc(120, 98, 58.0f, -0.2f, 0.2f, G2, 2);
    for (i = 0; i <= 8; i++) {
        int32_t a, b, c, d;
        rot(0.0f, -58.0f, -0.2f + 0.05f * (float)i, 120, 98, &a, &b);
        rot(0.0f, -50.0f, -0.2f + 0.05f * (float)i, 120, 98, &c, &d);
        cv_line(a, b, c, d, i > 6 ? REDC : G3);
    }
    rot(0.0f, -54.0f, -0.17f + 0.34f * (0.15f + 0.7f * env) + 0.08f * wob + noise, 120, 98, &ex, &ey);
    tline(120, 98, ex, ey, kcol(0));     /* the needle, wobbled by the amp mod (blue) */
    disc(120, 98, 4, K_WHITE);
    for (i = 0; i < 4; i++) {            /* the phaser's four stages, turning (white) */
        int32_t a, b;
        rot(0.0f, -5.0f, ui_t * (0.1f + 0.8f * k[2]) + (float)i * 0.25f, 24 + (i & 1) * 18, 40 + (i >> 1) * 18, &a, &b);
        ring(24 + (i & 1) * 18, 40 + (i >> 1) * 18, 6, kcol(2));
        disc(a, b, 2, kcol(2));
    }
    for (i = 0; i <= 40; i++) {          /* the folder (green) */
        float u = (float)i / 40.0f, v = sn(sn(u * 2.0f) * (0.25f + 1.0f * k[1]));
        int32_t x = 72 + i * 96 / 40, y = 124 - ri(v * 10.0f);
        if (i)
            cv_line(px, py, x, y, kcol(1));
        px = x;
        py = y;
    }
    for (i = 0; i < 3; i++) {            /* three cables between the jacks, slack with the detune (orange) */
        int32_t x0 = 186 + i * 8, x1 = 206 + i * 8, j, qx = x0, qy = 34, sag = 6 + ri((14.0f + 4.0f * (float)i) * k[3]);
        disc(x0, 34, 3, G3);
        disc(x1, 34, 3, G3);
        for (j = 1; j <= 8; j++) {
            float u = (float)j / 8.0f;
            int32_t x = x0 + ri(u * (float)(x1 - x0)), y = 34 + ri(4.0f * u * (1.0f - u) * (float)sag);
            cv_line(qx, qy, x, y, kcol(3));
            qx = x;
            qy = y;
        }
    }
}

static void pic_dna(const float *k, float env)
{
    int i, nr = 4 + sel(k[5], 29), wn = sel(k[1], 256);
    float A = 12.0f + 22.0f * k[0], sp = 0.12f + 0.3f * k[4];
    int32_t ax = 0, ay = 0, bx = 0, by = 0;
    for (i = 0; i <= 54; i++) {
        float u = (float)i / 54.0f, s = sn(u * 2.0f + ui_t * sp), z = sn(u * 2.0f + ui_t * sp + 0.25f);
        int32_t x = 12 + i * 4, ya = 76 - ri(A * s), yb = 76 + ri(A * s);
        if (i) {
            tline(ax, ay, x, ya, z > 0.0f ? kcol(0) : mix565(KC[0], BG, 0.6f));
            tline(bx, by, x, yb, z < 0.0f ? K_WHITE : G2);
        }
        ax = x;
        ay = ya;
        bx = x;
        by = yb;
    }
    for (i = 0; i < nr; i++) {           /* the rungs: the wave's points (green), shaped by the modifier (white) */
        float u = ((float)i + 0.5f) / (float)nr, s = sn(u * 2.0f + ui_t * sp);
        int32_t x = 12 + ri(u * 216.0f), ya = 76 - ri(A * s), yb = 76 + ri(A * s);
        float h = hsh((uint32_t)(wn * 64 + i));
        uint16_t c = mix565(kcol(1), BG, h * 0.6f);
        if (k[2] < 0.33f) {
            cv_rect(x - 1, ya < yb ? ya : yb, 3, (ya < yb ? yb - ya : ya - yb) + 1, c);
        } else if (k[2] < 0.66f) {
            cv_line(x, ya, x, yb, c);
        } else {
            int32_t mx = x + ri((h - 0.5f) * 10.0f);
            cv_line(x, ya, mx, (ya + yb) / 2, c);
            cv_line(mx, (ya + yb) / 2, x, yb, c);
        }
        disc(x, (ya + yb) / 2 + ri((h - 0.5f) * (float)(yb - ya) * 0.8f), 2 + ri(env * 2.0f), kcol(2));
    }
    if (k[3] > 0.02f) {                  /* noise (orange) */
        uint32_t f = (uint32_t)(ui_t * 15.0f);
        for (i = 0; i < ri(k[3] * 30.0f); i++)
            cv_rect(12 + ri(hsh(f * 31u + (uint32_t)i) * 216.0f), 30 + ri(hsh(f * 57u + (uint32_t)i) * 94.0f), 2, 2, kcol(3));
    }
}

static void pic_eng_sampler(const float *k, float env, int active)
{
    const sample_ref_t *s = &g_synth_sample;
    int32_t c, x0 = 12, W = 216, cy = 74;
    float st = k[0], li = k[1], lo = fm_maxf(k[2], k[1] + 0.01f), en = k[3];
    if (!s->data || s->len < 64u) {
        ctext(120, 66, &FONT_B, "EMPTY", G3);
        ctext(120, 92, &FONT_T, "REC ON THIS PAGE: SAMPLE THE OUTPUT", G2);
        return;
    }
    cv_rect(x0 + ri(li * (float)W), 34, ri((lo - li) * (float)W) + 1, 80, G1);   /* the loop */
    for (c = 0; c < W; c += 2) {
        uint32_t a = (uint32_t)((float)c / (float)W * (float)s->len), b = (uint32_t)((float)(c + 2) / (float)W * (float)s->len), i, st2;
        float mx = 0.0f;
        st2 = (b - a) / 6u + 1u;
        for (i = a; i < b && i < s->len; i += st2) {
            float v = fm_fabsf(ulaw(s->data[i]));
            if (v > mx)
                mx = v;
        }
        {
            float u = (float)c / (float)W;
            int32_t h = ri(mx * 34.0f) + 1;
            uint16_t col = u < st || u > en ? G2 : (u >= li && u <= lo) ? K_WHITE : G3;
            cv_rect(x0 + c, cy - h, 2, 2 * h, col);
        }
    }
    {                                    /* the four markers in their knobs' colours */
        const float M[4] = {st, li, lo, en};
        static const char *const L[4] = {"S", "[", "]", "E"};
        int i;
        for (i = 0; i < 4; i++) {
            int32_t x = x0 + ri(M[i] * (float)(W - 1));
            cv_rect(x, 30, 2, 88, kcol(i));
            ctext(x + 1, 120, &FONT_T, L[i], kcol(i));
        }
    }
    if (active) {                        /* the head: through the start, round the loop */
        float rev = k[4] > 0.5f ? -1.0f : 1.0f, u = st + frac1(ui_t * 0.4f) * (lo - st);
        if (rev < 0.0f)
            u = en - frac1(ui_t * 0.4f) * (en - li);
        cv_rect(x0 + ri(u * (float)(W - 1)), 34, 2, 80, mix565(K_ORANGE, K_WHITE, env));
    }
    if (k[4] > 0.5f)
        ctext(120, 24, &FONT_T, "<< REVERSE", gcol(0));
}

/* a synth engine: env = the newest voice's amplitude (0..1), note = its MIDI note, active = a voice sounds */
static void pic_engine(int type, const float k[8], float env, float note, int active)
{
    (void)note;
    if (!active)
        env = 0.0f;
    switch (type) {
    case E_STRING: pic_string(k, env); break;
    case E_PULSE: pic_pulse(k, env); break;
    case E_FM: pic_fm(k, env); break;
    case E_CLUSTER: pic_cluster(k, env); break;
    case E_DIGITAL: pic_digital(k, env); break;
    case E_PHASE: pic_phase(k, env); break;
    case E_SPECTRA: pic_spectra(k, env); break;
    case E_DUAL: pic_dual(k, env); break;
    case E_VOLTAGE: pic_voltage(k, env); break;
    case E_DNA: pic_dna(k, env); break;
    default: pic_eng_sampler(k, env, active); break;
    }
}

/* ================================================================== the effects === */
static void mix_bar(float m) { vbar_meter(226, 30, 84, m, kcol(3), "MIX"); }

static void fx_echo(const float *k, float lv)
{
    int n = 1 + sel(k[2], 9), j;
    float gap = 12.0f + 26.0f * k[0], mv = frac1(ui_t * 0.5f);
    disc(24, 72, 8 + ri(lv * 6.0f), K_WHITE);
    for (j = 1; j <= n; j++) {
        float x = 24.0f + ((float)j - 1.0f + mv) * gap * 0.75f + gap * 0.5f;
        float y = 72.0f + sn(ui_t * (0.2f + 1.5f * k[1]) + (float)j * 0.17f) * 10.0f * k[1];
        float f = fm_powf(0.3f + 0.65f * k[2], (float)j - 1.0f + mv);
        int32_t r = 3 + ri(7.0f * f);
        if (x > 214.0f)
            break;
        if (k[5] > 0.5f)                 /* ping-pong (GLO K2) */
            y += (j & 1 ? -1.0f : 1.0f) * 16.0f * (k[5] - 0.5f) * 2.0f;
        ring(ri(x), ri(y), r, mix565(K_WHITE, BG, 1.0f - f * 0.9f - 0.05f));
        if (f > 0.3f)
            disc(ri(x), ri(y), r / 2, mix565(KC[2], BG, 1.0f - f));
        cv_rect(ri(x) - 1, 112, 2, 6, kcol(0));
    }
    cv_rect(24, 114, 190, 1, kcol(0));
    ctext(60, 124, &FONT_T, "FEEDBACK", kcol(2));
    ctext(160, 124, &FONT_T, "TIME", kcol(0));
    mix_bar(k[3]);
}

static void fx_cube(const float *k, float lv)
{
    float sx = 18.0f + 36.0f * k[0], sy = 14.0f + 30.0f * k[1], sz = 14.0f + 36.0f * k[2], a = ui_t * 0.06f;
    float ca = sn(a + 0.25f), sa = sn(a), cb = 0.94f, sb = 0.34f, g = 1.0f + lv * 0.06f;
    int32_t PXv[8], PYv[8];
    int i;
    for (i = 0; i < 8; i++) {
        float x = (i & 1 ? sx : -sx) * g, y = (i & 2 ? sy : -sy) * g, z = (i & 4 ? sz : -sz) * g;
        float x2 = x * ca + z * sa, z2 = -x * sa + z * ca, y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb;
        float p = 160.0f / (160.0f + z3);
        PXv[i] = 112 + ri(x2 * p);
        PYv[i] = 74 - ri(y2 * p);
    }
    for (i = 0; i < 8; i++) {            /* the edges, by their axis: x blue, y green, z white */
        if (!(i & 1)) tline(PXv[i], PYv[i], PXv[i | 1], PYv[i | 1], kcol(0));
        if (!(i & 2)) tline(PXv[i], PYv[i], PXv[i | 2], PYv[i | 2], kcol(1));
        if (!(i & 4)) tline(PXv[i], PYv[i], PXv[i | 4], PYv[i | 4], kcol(2));
    }
    for (i = 0; i < 4 + ri(k[3] * 14.0f); i++) {   /* the sound inside, bouncing (orange: the mix) */
        float u = hsh((uint32_t)i * 7u), v = hsh((uint32_t)i * 13u), w = ui_t * (0.2f + 0.3f * u);
        float x = (sn(w + u) * 0.8f) * sx, y = (sn(w * 1.3f + v) * 0.8f) * sy;
        disc(112 + ri(x * ca), 74 - ri(y * cb), 1 + ri(lv * 2.0f), kcol(3));
    }
    mix_bar(k[3]);
}

static void fx_turbo(const float *k, float lv)
{
    float f1 = 0.06f + 0.86f * k[0], f2 = clampf(k[1] > 0.5f ? f1 * (1.0f + (k[1] - 0.5f) * 1.6f) : 0.06f + 0.86f * k[3], 0.05f, 0.97f);
    float q = 0.2f + 1.4f * k[2];
    int i;
    int32_t px = 0, py = 0;
    for (i = 0; i < 52; i++) {           /* what goes in, moving (grey) */
        float u = (float)i / 52.0f, h = hsh((uint32_t)i * 3u + (uint32_t)(ui_t * 12.0f)) * lv;
        bar_up(10 + i * 4, 122, 3, h * 40.0f * fm_expf(-u * 1.5f), G1);
    }
    for (i = 0; i <= 72; i++) {
        float f = (float)i / 72.0f, d1 = (f - f1) / 0.035f, d2 = (f - f2) / 0.035f;
        float v = 0.55f / (1.0f + fm_powf(f / f1, 4.0f)) + q * 0.5f * (fm_expf(-d1 * d1) + fm_expf(-d2 * d2));
        int32_t x = 10 + i * 3, y = 122 - ri(clampf(v, 0.0f, 1.6f) * 52.0f);
        if (i)
            tline(px, py, x, y, kcol(2));
        px = x;
        py = y;
    }
    disc(10 + ri(f1 * 216.0f), 30, 5, kcol(0));
    disc(10 + ri(f2 * 216.0f), 30, 5, kcol(3));
    tline(10 + ri(f1 * 216.0f), 30, 10 + ri(f2 * 216.0f), 30, kcol(1));
    cv_rect(10, 123, 216, 1, G2);
    if (k[4] > 0.05f)                    /* drive (GLO K1) */
        ctext(120, 128, &FONT_T, "DRIVE", gcol(0));
}

static void fx_wire(const float *k, float lv)
{
    int i, gapr = 0;
    int32_t px = 30, py = 60;
    uint32_t f = (uint32_t)(ui_t * 5.0f);
    for (i = 0; i < 2; i++) {            /* two cans (blue: the tone, their size) */
        int32_t x = i ? 216 : 24, w = 10 + ri(10.0f * k[0]);
        cv_rect(x - w / 2, 52, w, 18, kcol(0));
        cv_rect(x - w / 2, 52, w, 2, K_WHITE);
    }
    for (i = 1; i <= 46; i++) {          /* the wire: formant bumps (green), gaps (orange) */
        float u = (float)i / 46.0f;
        int32_t x = 30 + ri(u * 180.0f), y = 60 + ri(26.0f * u * (1.0f - u) * 4.0f * 0.5f + sn(u * (2.0f + 6.0f * k[1])) * 4.0f * k[1]);
        if (hsh((uint32_t)i * 17u + f * 101u) < k[3] * 0.5f) {
            gapr = 1;
            cv_rect(x, y + 4, 2, 2, kcol(3));
        } else {
            tline(px, py, x, y, gapr ? kcol(3) : kcol(1));
            gapr = 0;
        }
        px = x;
        py = y;
    }
    {                                    /* the signal: packets, sampled at the baud (white) */
        float sp = 12.0f + 40.0f * (1.0f - k[2]), off = frac1(ui_t * 0.4f) * sp;
        float x;
        for (x = 34.0f + off; x < 206.0f; x += sp) {
            float u = (x - 30.0f) / 180.0f;
            int32_t y = 60 + ri(13.0f * u * (1.0f - u) * 4.0f) - 4;
            cv_rect(ri(x) - 3, y - 3, 6, 6, mix565(BG, K_WHITE, 0.4f + 0.6f * lv));
        }
    }
    {
        static const char *const L[4] = {"TONE", "PHONIC", "BAUD", "TELEMATIC"};
        for (i = 0; i < 4; i++)
            ctext(30 + i * 60, 112, &FONT_T, L[i], kcol(i));
    }
}

static void fx_knock(const float *k, float lv)
{
    int i;
    float ratio = 1.0f + 9.0f * k[2], th = 0.6f - 0.3f * k[2];
    int32_t px = 0, py = 0, bx = 14, by = 30, B = 84;
    frame_rect(bx, by, B, B, G2);
    for (i = 0; i <= 28; i++) {          /* in -> out: the rounds (white) */
        float u = (float)i / 28.0f, o = u < th ? u : th + (u - th) / ratio;
        int32_t x = bx + ri(u * (float)(B - 1)), y = by + B - 1 - ri(o * (float)(B - 1));
        if (i)
            tline(px, py, x, y, kcol(2));
        px = x;
        py = y;
    }
    {
        float u = clampf(lv, 0.0f, 1.0f), o = u < th ? u : th + (u - th) / ratio;
        disc(bx + ri(u * (float)(B - 1)), by + B - 1 - ri(o * (float)(B - 1)), 4, K_WHITE);
    }
    px = 112;
    py = 100;
    for (i = 0; i <= 40; i++) {          /* a hit: its punch (green), cut by the freq (blue) */
        float u = (float)i / 40.0f, v = (u < 0.05f ? u / 0.05f : fm_expf(-(u - 0.05f) * 6.0f)) * (0.4f + 0.6f * k[1] * (u < 0.15f));
        float sm = v * (0.3f + 0.7f * k[0]);
        int32_t x = 112 + i * 2, y = 100 - ri(v * 60.0f), y2 = 100 - ri(sm * 60.0f);
        if (i) {
            cv_line(px, py, x, y, kcol(1));
            cv_rect(x, y2, 2, 1, kcol(0));
        }
        px = x;
        py = y;
    }
    cv_rect(112, 101, 84, 1, G2);
    vbar_meter(210, 30, 84, k[3], kcol(3), "PWR");
}

static void fx_coil(const float *k, float lv)
{
    int i, N = 120;
    float turns = 5.0f + 14.0f * k[1], damp = 0.5f + 5.0f * k[2];
    int32_t px = 0, py = 0;
    for (i = 0; i <= N; i++) {
        float u = (float)i / (float)N, ph = u * turns;
        float shake = lv * fm_expf(-damp * u) * sn(u * 3.0f - ui_t * 1.5f) * 10.0f;
        int32_t x = 28 + ri(u * 184.0f + sn(ph + 0.25f) * 4.0f), y = 70 + ri(sn(ph) * 14.0f + shake);
        uint16_t c = sn(ph + 0.25f) > 0.0f ? K_WHITE : G2;
        if (i)
            cv_line(px, py, x, y, c);
        px = x;
        py = y;
    }
    for (i = 0; i <= 30; i++) {          /* the damping: how fast it dies along (white) */
        float u = (float)i / 30.0f;
        cv_rect(28 + ri(u * 184.0f), 96 + ri((1.0f - fm_expf(-damp * u)) * 20.0f), 2, 2, kcol(2));
    }
    disc(22, 70, 7 + ri(5.0f * k[0]), kcol(0));
    disc(218, 70, 7 + ri(5.0f * (1.0f - k[0])), kcol(0));
    ctext(120, 26, &FONT_T, "TURNS", kcol(1));
    if (k[4] > 0.03f) {                  /* drips (GLO K1) */
        for (i = 0; i < 3; i++) {
            float d = frac1(ui_t * 0.5f + (float)i * 0.33f);
            disc(60 + i * 60, 86 + ri(d * 34.0f), 2, gcol(0));
        }
    }
    mix_bar(k[3]);
}

static void fx_doppler(const float *k, float lv)
{
    int j;
    float sp = 0.06f + 0.2f * k[1], dir = k[0] - 0.5f;
    float x = 104.0f + 62.0f * sn(ui_t * sp);
    for (j = 1; j <= 3 + sel(k[2], 6); j++) {   /* the fronts it left behind (white: their number) */
        float tb = (float)j * 0.6f, xo = 104.0f + 62.0f * sn((ui_t - tb) * sp), r = fm_minf(tb * (26.0f + 30.0f * fm_fabsf(dir)), 44.0f);
        ring(ri(xo), 72, ri(r), mix565(kcol(2), BG, (float)j * 0.1f));
    }
    if (k[3] > 0.05f) {                  /* the sideband (orange) */
        for (j = 0; j < 4; j++)
            arc(ri(x), 72, 14.0f + (float)j * 5.0f, dir > 0.0f ? 0.1f : 0.6f, dir > 0.0f ? 0.4f : 0.9f, kcol(3), 1);
    }
    cv_rect(28, 118, 168, 2, kcol(1));   /* its track (green: the delay) */
    disc(ri(x), 72, 9 + ri(lv * 4.0f), kcol(0));
    disc(ri(x), 72, 4, BG);
    ctext(112, 26, &FONT_T, dir > 0.02f ? "UP" : dir < -0.02f ? "DOWN" : "-", kcol(0));
    mix_bar(k[7]);
}

static void fx_hall(const float *k, float lv)
{
    int32_t w = 170 - ri(130.0f * k[0]), h = 86 - ri(64.0f * k[0]), x0 = 112 - w / 2, y0 = 76 - h / 2, i;
    uint16_t c = kcol(0);
    cv_line(4, 26, x0, y0, G2);
    cv_line(220, 26, x0 + w, y0, G2);
    cv_line(4, 128, x0, y0 + h, G2);
    cv_line(220, 128, x0 + w, y0 + h, G2);
    frame_rect(x0, y0, w + 1, h + 1, c);
    for (i = 1; i < 6; i++) {            /* the colour: lines on the floor (white) */
        float u = (float)i / 6.0f;
        int32_t y = y0 + h + ri(u * u * (float)(128 - y0 - h));
        cv_rect(4 + ri((float)(x0 - 4) * (1.0f - u * u)), y, 216 - ri((float)(216 - w) * (1.0f - u * u)), 1, mix565(kcol(2), BG, 0.7f * (1.0f - k[2])));
    }
    if (k[1] > 0.02f) {                  /* the gate: a shutter on the far wall (green) */
        int32_t gw = ri((float)w * k[1] * 0.5f);
        cv_rect(x0 + 1, y0 + 1, gw, h - 1, mix565(KC[1], BG, 0.5f));
        cv_rect(x0 + w - gw, y0 + 1, gw, h - 1, mix565(KC[1], BG, 0.5f));
    }
    for (i = 0; i < 4 + ri(lv * 8.0f); i++) {   /* the sound, going to the walls and back (orange: the mix) */
        float u = frac1(ui_t * 0.3f + hsh((uint32_t)i) ), px = hsh((uint32_t)i * 5u) - 0.5f, py = hsh((uint32_t)i * 9u) - 0.5f;
        float d = u < 0.5f ? u * 2.0f : 2.0f - u * 2.0f;
        int32_t x = 112 + ri(px * (220.0f - (220.0f - (float)w) * d)), y = 76 + ri(py * (100.0f - (100.0f - (float)h) * d));
        disc(x, y, 2, mix565(kcol(3), BG, d * 0.6f));
    }
    mix_bar(k[3]);
}

static void fx_bits(const float *k, float lv)
{
    int32_t w = 2 + ri(k[0] * k[0] * 24.0f), x, levels = 1 << (1 + ri((1.0f - k[1]) * 6.0f)), px = 0, py = 0;
    float amp = 30.0f + lv * 10.0f;
    for (x = 0; x < 204; x += w) {       /* held at the rate (blue), stepped by the bits (green) */
        float p = (float)x / 204.0f * 2.0f + ui_t * 0.05f, v = sn(p);
        float q = (float)ri(v * (float)levels * 0.5f) / ((float)levels * 0.5f);
        int32_t y = 72 - ri(q * amp), ww = x + w > 204 ? 204 - x : w;
        if (k[2] > 0.5f)                 /* model: gritty / aliased (white) */
            y += ri((hsh((uint32_t)x + (uint32_t)(ui_t * 10.0f) * 7u) - 0.5f) * 10.0f * (k[2] - 0.5f));
        cv_rect(12 + x, y - 2, ww > 2 ? ww - 1 : 1, 5, kcol(1));
        cv_rect(12 + x, 120, 1, 4, kcol(0));
    }
    for (x = 0; x <= 204; x += 4) {      /* the clean wave behind (orange: the mix) */
        float p = (float)x / 204.0f * 2.0f + ui_t * 0.05f;
        int32_t y = 72 - ri(sn(p) * amp);
        if (x)
            cv_line(12 + px, py, 12 + x, y, mix565(kcol(3), BG, k[3]));
        px = x;
        py = y;
    }
    if (k[2] > 0.0f && k[2] <= 0.5f) {   /* dither (white) */
        int i;
        for (i = 0; i < ri(k[2] * 40.0f); i++)
            cv_rect(12 + ri(hsh((uint32_t)i * 3u + (uint32_t)(ui_t * 20.0f) * 11u) * 204.0f), 40 + ri(hsh((uint32_t)i * 7u) * 64.0f), 1, 1, kcol(2));
    }
    mix_bar(k[3]);
}

/* an effect: level = the signal's level going in (0..1) */
static void pic_fx(int type, const float k[8], float level)
{
    switch (type) {
    case FX_ECHO: fx_echo(k, level); break;
    case FX_CUBE: fx_cube(k, level); break;
    case FX_TURBO: fx_turbo(k, level); break;
    case FX_WIRE: fx_wire(k, level); break;
    case FX_KNOCK: fx_knock(k, level); break;
    case FX_COIL: fx_coil(k, level); break;
    case FX_DOPPLER: fx_doppler(k, level); break;
    case FX_HALL: fx_hall(k, level); break;
    default: fx_bits(k, level); break;
    }
}

/* ===================================================================== the LFOs === */
static void dice_face(int32_t cx, int32_t cy, float s, float a, int n, uint16_t c)
{
    static const int8_t P[7][6][2] = {{{0}}, {{0, 0}}, {{-1, -1}, {1, 1}}, {{-1, -1}, {0, 0}, {1, 1}},
                                      {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}, {{-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}},
                                      {{-1, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {1, 1}}};
    int32_t X[4], Y[4], i;
    for (i = 0; i < 4; i++)
        rot(i == 0 || i == 3 ? -s : s, i < 2 ? -s : s, a, cx, cy, &X[i], &Y[i]);
    for (i = 0; i < 4; i++)
        tline(X[i], Y[i], X[(i + 1) & 3], Y[(i + 1) & 3], c);
    for (i = 0; i < n; i++) {
        int32_t x, y;
        rot((float)P[n][i][0] * s * 0.5f, (float)P[n][i][1] * s * 0.5f, a, cx, cy, &x, &y);
        disc(x, y, 2, K_WHITE);
    }
}
/* an LFO: m its module (raw parameters), val its output now (-1..1), ph its phase (0..1) */
static void pic_lfo(int type, const mod_t *m, float val, float ph)
{
    float k[8];
    int i;
    for (i = 0; i < 8; i++)
        k[i] = kn(m->p[i]);
    switch (type) {
    case L_SENSE: {
        static const char *const S[5] = {"PRESSURE", "VELOCITY", "ENVELOPE", "LEVEL", "WHEEL"};
        float amt = (k[1] - 0.5f) * 2.0f;
        int32_t ex, ey;
        arc(120, 104, 60.0f, -0.25f, 0.25f, G2, 2);
        if (amt > 0.0f)
            arc(120, 104, 56.0f, 0.0f, 0.25f * amt, kcol(1), 4);
        else if (amt < 0.0f)
            arc(120, 104, 56.0f, 0.25f * amt, 0.0f, kcol(1), 4);
        rot(0.0f, -50.0f, 0.25f * clampf(val, -1.0f, 1.0f), 120, 104, &ex, &ey);
        tline(120, 104, ex, ey, K_WHITE);
        disc(120, 104, 5, K_WHITE);
        ctext(120, 26, &FONT_T, S[sel(k[0], 5)], kcol(0));
        target_row(k[2], k[3], 116);
        break;
    }
    case L_DICE: {
        float rs = 0.4f + 5.0f * k[1];
        uint32_t f = (uint32_t)(ui_t * rs);
        float roll = frac1(ui_t * rs);
        dice_face(82, 64, 12.0f + 10.0f * k[0], roll * 0.25f * (1.0f - k[3]), 1 + (int)(hsh(f * 3u) * 5.99f), kcol(0));
        dice_face(150, 70, 10.0f + 8.0f * k[0], -roll * 0.25f * (1.0f - k[3]) + 0.1f, 1 + (int)(hsh(f * 5u + 1u) * 5.99f), kcol(1));
        cv_rect(196, 70, 30, 2, G2);
        cv_rect(210, 70 - ri(clampf(val, -1.0f, 1.0f) * 30.0f), 6, 3, kcol(3));
        target_row(k[2], 0.0f, 116);
        break;
    }
    case L_WOBBLE: {
        int32_t px = 0, py = 0, fx0 = 0, fy0 = 0, cy = 70 + ri(val * 14.0f * (k[1] - 0.5f) * 2.0f);
        float rr = 28.0f * (1.0f + 0.25f * val * (k[2] - 0.5f) * 2.0f);
        for (i = 0; i <= 32; i++) {      /* a jelly: wobbled up and down (green: pitch), in and out (white: volume) */
            float a = (float)i / 32.0f, r = rr * (1.0f + 0.07f * sn(a * 3.0f + ui_t * (0.2f + 2.0f * k[0])));
            int32_t x = 120 + ri(sn(a) * r * 1.2f), y = cy - ri(sn(a + 0.25f) * r * (0.85f + 0.15f * val));
            if (i)
                tline(px, py, x, y, kcol(2));
            else {
                fx0 = x;
                fy0 = y;
            }
            px = x;
            py = y;
        }
        (void)fx0;
        (void)fy0;
        disc(108, cy - 6, 3, K_WHITE);
        disc(132, cy - 6, 3, K_WHITE);
        cv_rect(30, cy, 2, 2, G2);
        cv_line(40, 70, 40, cy, kcol(1));
        {
            int32_t ox, oy;                 /* the speed, orbiting (blue) */
            rot(0.0f, -44.0f, ph, 120, 70, &ox, &oy);
            disc(ox, oy, 3, kcol(0));
        }
        arc(200, 110, 12.0f, 0.0f, 0.02f + 0.98f * k[3], kcol(3), 3);   /* the fade in (orange) */
        ctext(200, 128, &FONT_T, "FADE", kcol(3));
        {
            static const char *const W[5] = {"SINE", "SAW", "EXP", "SQUARE", "BLIP"};
            cv_text(8, 26, &FONT_T, W[sel(k[4], 5)], gcol(0));
        }
        break;
    }
    case L_WAVE: {
        float s = (float)sel(k[4], 5), amt = (k[1] - 0.5f) * 2.0f;
        int32_t px = 0, py = 0, x;
        for (x = 0; x <= 200; x += 4) {
            float p = (float)x / 200.0f, v = s == 4.0f ? hsh((uint32_t)(p * 8.0f)) * 2.0f - 1.0f : shp(p, s == 0.0f ? 0.0f : s == 1.0f ? 1.0f : s == 2.0f ? 2.0f : 3.0f);
            int32_t y = 66 - ri(v * amt * 34.0f);
            if (x)
                tline(20 + px, py, 20 + x, y, kcol(1));
            px = x;
            py = y;
        }
        {
            float v = s == 4.0f ? hsh((uint32_t)(ph * 8.0f)) * 2.0f - 1.0f : shp(ph, s);
            disc(20 + ri(ph * 200.0f), 66 - ri(v * amt * 34.0f), 5, K_WHITE);
        }
        cv_rect(20, 66, 200, 1, G2);
        for (i = 0; i < 1 + sel(k[0], 6); i++)   /* the speed (blue) */
            cv_text(8 + i * 8, 24, &FONT_T, ">", kcol(0));
        target_row(k[2], k[3], 116);
        break;
    }
    default: {                           /* MIDI: a socket, its pins lit by what arrives */
        ring(70, 66, 30, K_WHITE);
        ring(70, 66, 29, K_WHITE);
        cv_rect(66, 34, 8, 4, K_WHITE);
        for (i = 0; i < 5; i++) {
            int32_t x, y;
            rot(0.0f, -18.0f, -0.25f + (float)i * 0.125f, 70, 66, &x, &y);
            disc(x, y, 3, fm_fabsf(val) * 5.0f > (float)i ? K_ORANGE : G2);
        }
        {
            float a = (k[0] - 0.5f) * 2.0f, b = (k[1] - 0.5f) * 2.0f;
            cv_rect(140, 66, 80, 1, G2);
            cv_rect(a > 0 ? 180 : 180 + ri(a * 40.0f), 44, ri(fm_fabsf(a) * 40.0f) + 1, 8, kcol(0));
            cv_rect(b > 0 ? 180 : 180 + ri(b * 40.0f), 80, ri(fm_fabsf(b) * 40.0f) + 1, 8, kcol(1));
            ctext(180, 32, &FONT_T, "WHEEL", kcol(0));
            ctext(180, 92, &FONT_T, "PRESSURE", kcol(1));
        }
        target_row(k[2], k[3], 116);
        break;
    }
    }
}

/* =============================================================== the sequencers === */
static void step_ticks(float kdiv, int32_t y)   /* the division: ticks, closer for faster (blue) */
{
    static const char *const D[7] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
    cv_text(8, y, &FONT_T, D[sel(kdiv, 7)], kcol(0));
}
static void seq_extra(const char *label, float on, int32_t x, int32_t y)   /* hold / run style switches */
{
    if (on > 0.5f) {
        cv_rect(x - 22, y, 44, 13, kcol(3));
        ctext(x, y + 3, &FONT_T, label, BG);
    } else {
        frame_rect(x - 22, y, 44, 13, kdim(3));
        ctext(x, y + 3, &FONT_T, label, kdim(3));
    }
}
/* a sequencer: d its data, step the step playing (-1 none), kind KIND_SYNTH / KIND_DRUM */
static void pic_seq(int type, const mod_t *m, const seqdata_t *d, int step, int kind)
{
    static const uint8_t TRIGP[8] = {0xFF, 0x55, 0x77, 0x6D, 0x49, 0xDB, 0xAF, 0x11};
    float k[8];
    int i;
    for (i = 0; i < 8; i++)
        k[i] = kn(m->p[i]);
    switch (type) {
    case Q_LOOP: {
        int n = d->loop_n, lo = 127, hi = 0, first = 0, vis;
        if (n == 0) {
            ctext(120, 60, &FONT_B, "EMPTY", G3);
            ctext(120, 86, &FONT_T, "GLO + KEYS: ENTER STEPS", G2);
            ctext(120, 100, &FONT_T, "GLO + OCT: REST / DELETE", G2);
            break;
        }
        for (i = 0; i < n; i++)
            if (d->loop[i] < SEQ_TIE) {
                if (d->loop[i] < lo) lo = d->loop[i];
                if (d->loop[i] > hi) hi = d->loop[i];
            }
        if (hi < lo) { lo = 60; hi = 60; }
        if (hi - lo < 12) { lo -= (12 - (hi - lo)) / 2; hi = lo + 12; }
        vis = n < 16 ? n : 16;
        if (step >= 0 && n > 16)
            first = (step / 16) * 16;
        for (i = 0; i < vis && first + i < n; i++) {
            int s = first + i, j;
            uint8_t v = d->loop[s];
            float cw = 216.0f / (float)vis, sw = (s & 1) ? k[1] * cw * 0.5f : 0.0f;
            int32_t x = 12 + ri((float)i * cw + sw), w = ri(cw * (0.3f + 0.65f * (k[6] > 0.0f ? k[6] : 0.7f)));
            if (v == SEQ_TIE) {          /* a tie: the note before goes on */
                for (j = s - 1; j >= 0 && d->loop[j] == SEQ_TIE; j--) {}
                v = j >= 0 ? d->loop[j] : SEQ_REST;
                if (v != SEQ_REST)
                    cv_rect(x - 2, 112 - ri((float)(v - lo) / (float)(hi - lo) * 76.0f), w + 2, 3, G3);
                continue;
            }
            if (v == SEQ_REST) {
                disc(x + w / 2, 112, 1, G2);
                continue;
            }
            cv_rect(x, 112 - ri((float)(v - lo) / (float)(hi - lo) * 76.0f), w > 2 ? w : 2, 5,
                    s == step ? K_WHITE : ((TRIGP[sel(k[2], 8)] >> (s & 7)) & 1u) ? mix565(K_WHITE, BG, 0.4f) : G2);
        }
        if (step >= 0 && step >= first && step < first + vis)
            cv_rect(12 + ri((float)(step - first) * 216.0f / (float)vis), 120, ri(216.0f / (float)vis), 3, K_ORANGE);
        for (i = 0; i < 8; i++)          /* the trigger pattern (white) */
            disc(104 + i * 9, 28, 2, ((TRIGP[sel(k[2], 8)] >> i) & 1u) ? kcol(2) : G1);
        step_ticks(k[0], 24);
        if (k[1] > 0.02f)
            cv_text(60, 24, &FONT_T, "SWING", kcol(1));
        seq_extra("HOLD", k[3], 212, 24);
        break;
    }
    case Q_GRID: {
        int cur = sel(k[0], 16), len = 1 + sel(k[2], 16);
        for (i = 0; i < 16; i++) {
            int32_t x = 16 + (i & 7) * 26 + ((i & 1) ? ri(k[1] * 8.0f) : 0), y = 42 + (i >> 3) * 42, h;
            uint16_t c = i >= len ? G1 : i == step ? K_WHITE : G3;
            frame_rect(x, y, 22, 36, c);
            if (d->grid[i]) {
                h = 4 + ((int)d->grid[i] - (kind ? 60 : 48)) * 32 / 36;
                h = h < 4 ? 4 : h > 32 ? 32 : h;
                cv_rect(x + 3, y + 34 - h, 16, h, i == step ? K_WHITE : i >= len ? G2 : kcol(2));
            }
            if (i == cur) {
                frame_rect(x - 2, y - 2, 26, 40, kcol(0));
                frame_rect(x - 1, y - 1, 24, 38, kcol(0));
            }
        }
        seq_extra("RUN", k[3], 212, 24);
        if (m->on)
            ctext(120, 128, &FONT_T, "GLO + KEY: PUT A NOTE", G2);
        break;
    }
    case Q_SPIN: {
        float a = CORE_VIEW.spin_angle, open = k[2], R = 52.0f, ap = 0.947f * R;
        int32_t cx = 120, cy = 78;
        for (i = 0; i < 6; i++) {        /* six walls; wall 4 opens (white) */
            float a0 = a + (float)i / 6.0f, a1 = a + (float)(i + 1) / 6.0f;
            int32_t x0 = cx + ri(sn(a0) * ap), y0 = cy - ri(sn(a0 + 0.25f) * ap), x1 = cx + ri(sn(a1) * ap), y1 = cy - ri(sn(a1 + 0.25f) * ap);
            if (i == 4 && open > 0.02f) {
                float g = clampf(open * 0.45f / 0.947f, 0.0f, 0.5f);   /* the gap, as a share of the wall */
                int32_t mx0 = x0 + ri((float)(x1 - x0) * (0.5f - g)), my0 = y0 + ri((float)(y1 - y0) * (0.5f - g));
                int32_t mx1 = x0 + ri((float)(x1 - x0) * (0.5f + g)), my1 = y0 + ri((float)(y1 - y0) * (0.5f + g));
                tline(x0, y0, mx0, my0, kcol(0));
                tline(mx1, my1, x1, y1, kcol(0));
                disc(mx0, my0, 2, kcol(2));
                disc(mx1, my1, 2, kcol(2));
            } else {
                tline(x0, y0, x1, y1, kcol(0));
            }
        }
        for (i = 0; i < CORE_VIEW.nballs && i < 8; i++) {   /* the balls: lit as they hit */
            int32_t x = cx + ri(CORE_VIEW.balls[i][0] * R), y = cy - ri(CORE_VIEW.balls[i][1] * R);
            disc(x, y, 4, CORE_VIEW.balls[i][2] > 0.5f ? K_WHITE : kcol(3));
        }
        cv_line(20, 40, 20, 40 + ri(k[1] * 70.0f), kcol(1));   /* gravity (green) */
        disc(20, 40 + ri(k[1] * 70.0f), 3, kcol(1));
        ctext(20, 118, &FONT_T, "G", kcol(1));
        if (CORE_VIEW.nballs == 0 && m->on)
            ctext(120, 132, &FONT_T, "PLAY KEYS: DROP BALLS", G2);
        break;
    }
    case Q_HANDS: {
        int len = 1 + sel(k[1], 16);
        for (i = 0; i < 7; i++) {        /* each white key's pattern: a row */
            int j;
            for (j = 0; j < 16; j++) {
                int32_t x = 30 + j * 12 + ((j & 1) ? ri(k[0] * 5.0f) : 0), y = 30 + i * 13;
                int on = hsh((uint32_t)(i * 16 + j) * 3u + (uint32_t)kind * 977u) < 0.25f + 0.7f * k[2];
                uint16_t c = j >= len ? G1 : on ? (j == (step & 15) ? K_WHITE : kcol(2)) : G1;
                if (on)
                    disc(x, y + 4, 3, c);
                else
                    cv_rect(x, y + 4, 1, 1, G2);
            }
            {
                static const char *const W[7] = {"C", "D", "E", "F", "G", "A", "B"};
                cv_text(10, 30 + i * 13, &FONT_T, W[i], G3);
            }
        }
        if (step >= 0)
            cv_rect(26 + (step & 15) * 12, 28, 9, 2, K_ORANGE);
        seq_extra("HOLD", k[3], 212, 24);
        if (k[0] > 0.02f)
            cv_text(10, 128, &FONT_T, "SWING", kcol(0));
        break;
    }
    case Q_DRAW: {
        int32_t px = 0, py = 0, cx = 12 + ri(k[0] * 31.0f) * 7;
        cv_rect(cx, 26, 2, 98, kcol(0));
        for (i = 0; i < 32; i++) {
            int32_t x = 12 + i * 7, y = 120 - (int32_t)d->draw[i] * 90 / 127;
            if (i)
                tline(px, py, x, y, (step & 31) == i ? K_WHITE : kcol(2));
            disc(x, y, (step & 31) == i ? 4 : 1, (step & 31) == i ? K_WHITE : G3);
            px = x;
            py = y;
        }
        disc(cx + 1, 120 - ri(k[1] * 90.0f), 5, kcol(1));   /* the pen (green) */
        seq_extra("HOLD", k[3], 212, 24);
        break;
    }
    case Q_ARP: {
        int type2 = sel(k[5], 4), oc = 1 + sel(k[6], 3), n = 3 * oc, j;
        int v[24];
        for (j = 0; j < n && j < 12; j++)
            v[j] = (j / 3) * 12 + (j % 3 == 0 ? 0 : j % 3 == 1 ? 4 : 7);
        if (type2 == 1) {                /* up and down */
            for (j = 1; j < n - 1 && n + j - 1 < 24; j++)
                v[n + j - 1] = v[n - 1 - j];
            n = n + n - 2 > 24 ? 24 : (n > 1 ? n + n - 2 : n);
        } else if (type2 == 2) {
            for (j = 0; j < n; j++)
                v[j] = (j % 3 == 0 ? 0 : j % 3 == 1 ? 7 : 16) + (j / 3) * 4;
        } else if (type2 == 3) {
            for (j = 0; j < n; j++)
                v[j] = j * 2;
        }
        for (j = 0; j < n && j < 16; j++) {   /* the stairs */
            int32_t x = 12 + j * 13, h = 6 + v[j] * 2;
            int hit = step >= 0 && (step % n) == j;
            cv_rect(x, 118 - h, 11, h > 90 ? 90 : h, hit ? K_WHITE : ((TRIGP[sel(k[2], 8)] >> (j & 7)) & 1u) ? kcol(1) : G1);
        }
        {
            static const char *const T[4] = {"UP", "UP-DOWN", "+7 +9 -5", "SCALE"};
            rtext(232, 44, &FONT_T, T[type2], gcol(1));
        }
        step_ticks(k[0], 24);
        seq_extra("HOLD", k[3], 212, 24);
        break;
    }
    default: {                           /* LATCH: a keyboard, split at the blue line */
        int split = sel(k[0], 15), j;
        for (j = 0; j < 15; j++) {
            int32_t x = 15 + j * 14;
            cv_rect(x, 50, 13, 60, j < split ? G3 : K_WHITE);
        }
        for (j = 0; j < 14; j++) {
            int s = j % 7;
            if (s != 2 && s != 6)
                cv_rect(15 + j * 14 + 9, 50, 8, 36, BG);
        }
        cv_rect(15 + split * 14 - 2, 40, 3, 78, kcol(0));
        ctext(15 + split * 14, 120, &FONT_T, "SPLIT", kcol(0));
        cv_text(10, 26, &FONT_T, k[1] > 0.5f ? "MONO" : "POLY", kcol(1));
        {
            char t[8];
            int tr = sel(k[2], 25) - 12;
            num_text(tr, t);
            ctext(120, 26, &FONT_T, t, kcol(2));
        }
        break;
    }
    }
}
