/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the pictures, after Vital's (included by app.c): the wavetable in 3D with the warped frame, the
 * filter's response with its modulation, the envelope and LFO with their playheads, the random LFO's trace, the
 * matrix, the macros, the effect chain with each effect's picture, the voices. The picture area: y 20..143. */

INL float kv(int id) { return (float)pt.v[id] / 1023.0f; }
static float live_or(int id)             /* the value the newest voice hears, else the knob's */
{
    float lo, hi;
    if (!CORE_VIEW.active || PDESC[id].kind == PK_CHOICE)
        return kv(id);
    mod_reach(id, &lo, &hi);
    return hi > lo ? CORE_VIEW.live[id] : kv(id);
}
static void plot_poly(const int32_t *x, const int32_t *y, int n, uint16_t c, int thick)
{
    int i;
    for (i = 1; i < n; i++) {
        if (thick)
            tline(x[i - 1], y[i - 1], x[i], y[i], c);
        else
            cv_line(x[i - 1], y[i - 1], x[i], y[i], c);
    }
}

/* ------------------------------------------------------------- the oscillators --- */
static void pic_osc(int k)
{
    const wt_t *t = WT.play[k];
    int on = pt.v[P_OSC(k, ON)] & 1u, f, i, warp = pt.v[P_OSC(k, WARP)];
    float fp = live_or(P_OSC(k, FRAME)) * (float)(WT_FRAMES - 1), wa = live_or(P_OSC(k, WAMT));
    int32_t X[49], Y[49];
    char s[24];
    if (ui.glo) {                        /* GLO: the frame's spectrum */
        float mag[64];
        int a = pt.v[P_OSC(k, MORPH)] ? pt.v[P_OSC(k, MAMT)] : 0;
        wt_spectrum(pt.v[P_OSC(k, TABLE)], pt.v[P_OSC(k, MORPH)], (float)a / 1023.0f, fp, mag);
        for (i = 0; i < 64; i++) {
            int32_t h = (int32_t)(clampf(mag[i], 0.0f, 1.0f) * 92.0f);
            cv_rect(8 + i * 3 + (i >> 4), 128 - h, 2, h + 1, i < 16 ? TEAL : mix565(TEAL, PURP, (float)(i - 16) / 48.0f));
        }
        cv_text(8, 24, &FONT_T, "SPECTRUM", G3);
    } else {                             /* the stack: every other frame, back to front, the playing one teal */
        for (f = WT_FRAMES - 2; f >= 0; f -= 2) {
            float d = fm_fabsf((float)f - fp);
            int32_t ox = 18 + f * 3, oy = 112 - f * 2;
            uint16_t c = mix565(G2, G1, (float)f / (float)WT_FRAMES);
            if (d < 3.0f)
                c = mix565(TEAL_D, c, d / 3.0f);
            for (i = 0; i <= 32; i++) {
                X[i] = ox + i * 4;
                Y[i] = oy - (int32_t)(wt_peek(t, (float)f, (float)i / 32.0f) * 22.0f);
            }
            plot_poly(X, Y, 33, c, 0);
        }
        {                                /* the frame playing, through the warp */
            int32_t ox = 18 + (int32_t)(fp * 3.0f), oy = 112 - (int32_t)(fp * 2.0f);
            for (i = 0; i <= 48; i++) {
                float ph = (float)i / 48.0f, w = wt_warp_phase(warp, wa, ph);
                X[i] = ox + i * 128 / 48;
                Y[i] = oy - (int32_t)(wt_peek(t, fp, w) * 22.0f);
            }
            plot_poly(X, Y, 49, on ? TEAL : G2, 1);
        }
    }
    /* the names: table, warp, morph, frame */
    cv_text(4, 23, &FONT_T, pt.v[P_OSC(k, TABLE)] < (uint16_t)WT_NFACT ? WT_NAME[pt.v[P_OSC(k, TABLE)]] : "?", on ? WHITE : G2);
    num_text((int32_t)(fp + 1.5f), s);
    text_cat(s, " / 24", sizeof s);
    rtext(236, 23, &FONT_T, s, G3);
    if (warp)
        cv_text(4, 131, &FONT_T, WARP_NAME[warp % WARP_N], TEAL);
    if (pt.v[P_OSC(k, MORPH)])
        rtext(236, 131, &FONT_T, SM_NAME[pt.v[P_OSC(k, MORPH)] % SM_N], PURP);
    {                                    /* unison: the voices' spread (bars) */
        int n = 1 + pt.v[P_OSC(k, UNI)];
        float det = live_or(P_OSC(k, DET)), bl = kv(P_OSC(k, BLEND));
        for (i = 0; i < n; i++) {
            float x = n > 1 ? (float)i / (float)(n - 1) * 2.0f - 1.0f : 0.0f;
            int centre = n == 1 || (n & 1 ? i == n / 2 : (i == n / 2 || i == n / 2 - 1));
            int32_t h = centre ? 12 : (int32_t)(2.0f + 10.0f * bl);
            cv_rect(200 + (int32_t)(x * det * 30.0f), 108 - h, 2, h, on ? (centre ? WHITE : TEAL) : G2);
        }
        cv_rect(168, 109, 64, 1, G1);
    }
    if (!on)
        ctext(120, 70, &FONT_T, "OFF  (GLO + SEL)", G3);
}

static void pic_smp(void)
{
    int si = pt.v[P_S_SAMPLE], on = pt.v[P_S_ON] & 1u, i;
    const smp_ref_t *r = smp_get(si);
    cv_text(4, 23, &FONT_T, si < SMP_NFACT ? SMP_NAME[si] : "?", on ? WHITE : G2);
    for (i = 0; i < 220; i += 2) {
        float a = 0.0f;
        if (r && r->data && r->len) {
            uint32_t p0 = (uint32_t)((float)i / 220.0f * (float)r->len), p1 = (uint32_t)((float)(i + 2) / 220.0f * (float)r->len), p;
            for (p = p0; p < p1 && p < r->len; p += 4) {
                float v = fm_fabsf((float)r->data[p] * (1.0f / 32768.0f));
                if (v > a)
                    a = v;
            }
        } else {                         /* a live noise: its colour as a texture */
            uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)(ui_t * 8.0f) * 40503u;
            float w = (float)((h >> 8) & 255u) / 255.0f;
            a = r && r->kind == 3 ? 0.3f + 0.3f * w * 0.5f : r && r->kind == 2 ? 0.25f + 0.45f * w : 0.2f + 0.7f * w;
        }
        {
            int32_t h = (int32_t)(a * 46.0f) + 1;
            cv_rect(10 + i, 82 - h, 2, 2 * h, on ? TEAL : G2);
        }
    }
    if (r && r->data && pt.v[P_S_LOOP]) {
        int32_t x0 = 10 + (int32_t)((float)r->loop_start / (float)r->len * 220.0f), x1 = 10 + (int32_t)((float)r->loop_end / (float)r->len * 220.0f);
        cv_rect(x0, 30, 1, 104, PURP);
        cv_rect(x1, 30, 1, 104, PURP);
    }
    rtext(236, 23, &FONT_T, pt.v[P_S_KEY] ? "KEY TRACK" : "FIXED", G3);
    if (!on)
        ctext(120, 70, &FONT_T, "OFF  (GLO + SEL)", G3);
}

/* ----------------------------------------------------------------- the filters --- */
static void fill_fctl(filt_ctl_t *c, int k, int live)
{
    float (*g)(int) = live ? live_or : kv;
    c->cutoff = 8.0f + 128.0f * g(P_FLT(k, CUT));
    c->reso = g(P_FLT(k, RESO));
    c->drive = g(P_FLT(k, DRIVE));
    c->blend = g(P_FLT(k, BLEND));
    c->fx = g(P_FLT(k, FX));
    c->fy = g(P_FLT(k, FY));
    c->transpose = (g(P_FLT(k, TRANS)) - 0.5f) * 24.0f;
}
static void response_curve(int model, int style, const filt_ctl_t *c, uint16_t col, int thick, int32_t y0, int32_t h)
{
    int32_t X[61], Y[61];
    int i;
    for (i = 0; i <= 60; i++) {
        float note = 8.0f + 128.0f * (float)i / 60.0f, m = filt_response(model, style, c, note);
        float db = 20.0f * fm_logf(m + 1e-4f) * 0.4342945f;
        X[i] = 4 + i * 232 / 60;
        Y[i] = y0 + h / 3 - (int32_t)(clampf(db, -40.0f, 20.0f) * (float)h / 60.0f);
    }
    plot_poly(X, Y, 61, col, thick);
}
static void pic_filter(int k)
{
    int model = pt.v[P_FLT(k, MODEL)] % FLT_N, style = pt.v[P_FLT(k, STYLE)], on = pt.v[P_FLT(k, ON)] & 1u, i;
    filt_ctl_t c;
    float lo, hi;
    int32_t y0 = 34, h = 96;
    for (i = 0; i < 5; i++)              /* the grid: octaves */
        cv_rect(4 + (int32_t)((float)(i * 24 + 16) / 128.0f * 232.0f), y0, 1, h, G1);
    cv_rect(4, y0 + h / 3, 232, 1, G1);  /* 0 dB */
    fill_fctl(&c, k, 0);
    mod_reach(P_FLT(k, CUT), &lo, &hi);
    if (hi > lo) {                       /* the cutoff's reach (purple), and the curve the voice hears (white) */
        int32_t xa = 4 + (int32_t)(lo * 232.0f), xb = 4 + (int32_t)(hi * 232.0f);
        cv_rect(xa, 132, xb - xa + 1, 3, PURP);
        if (CORE_VIEW.active) {
            filt_ctl_t cl;
            fill_fctl(&cl, k, 1);
            response_curve(model, style, &cl, mix565(WHITE, BG, 0.4f), 0, y0, h);
        }
    }
    response_curve(model, style, &c, on ? TEAL : G2, 1, y0, h);
    {
        int32_t cx = 4 + (int32_t)(kv(P_FLT(k, CUT)) * 232.0f);
        disc(cx, y0 + h / 3 - (int32_t)(kv(P_FLT(k, RESO)) * 30.0f), 4, on ? WHITE : G2);
    }
    {
        char s[28];
        copy_text(s, FLT_NAME[model], sizeof s);
        text_cat(s, " ", sizeof s);
        text_cat(s, style < FLT_NSTYLE[model] ? FLT_STYLE_NAME[model][style] : "", sizeof s);
        cv_text(4, 23, &FONT_T, s, on ? WHITE : G2);
    }
    if (model == FLT_FORMANT) {          /* the vowel pad */
        int32_t px = 196, py = 40, w = 36;
        float fx = live_or(P_FLT(k, FX)), fy = live_or(P_FLT(k, FY));
        cv_rect(px, py, w, w, PANEL);
        frame_rect(px, py, w, w, G2);
        disc(px + (int32_t)(kv(P_FLT(k, FX)) * (float)(w - 1)), py + w - 1 - (int32_t)(kv(P_FLT(k, FY)) * (float)(w - 1)), 3, TEAL);
        disc(px + (int32_t)(fx * (float)(w - 1)), py + w - 1 - (int32_t)(fy * (float)(w - 1)), 2, WHITE);
    }
    {                                    /* the routing: who goes where, serial or parallel */
        static const char *const SN[4] = {"1", "2", "3", "S"};
        int j;
        for (j = 0; j < 4; j++) {
            int id = j < 3 ? P_OSC(j, DEST) : P_S_DEST, d = pt.v[id], son = j < 3 ? pt.v[P_OSC(j, ON)] & 1u : pt.v[P_S_ON] & 1u;
            int hit = son && (d == 2 || d == k);
            cv_text(150 + j * 10, 23, &FONT_T, SN[j], hit ? TEAL : G1);
        }
        rtext(236, 23, &FONT_T, pt.v[P_F_ROUTE] ? "SERIAL" : "PARALLEL", G3);
    }
    if (!on)
        ctext(120, 70, &FONT_T, "OFF  (GLO + EDIT)", G3);
}

/* ---------------------------------------------------------------- the envelopes --- */
static void pic_env(int k)
{
    float tm[6], tot = 0.0f, x = 0.0f, sus = kv(P_ENV(k, SUS));
    int32_t X[80], Y[80], n = 0, i, s;
    int picked = ui.pick == MS_ENV1 + k;
    float ap = (kv(P_ENV(k, APOW)) - 0.5f) * 20.0f, dp = -(kv(P_ENV(k, DPOW)) - 0.5f) * 20.0f, rp = -(kv(P_ENV(k, RPOW)) - 0.5f) * 20.0f;
    static const int SEG[5] = {P_E1_DELAY, P_E1_ATK, P_E1_HOLD, P_E1_DEC, P_E1_REL};
    for (i = 0; i < 5; i++) {            /* the times, as the screen shows them: a fourth root, like Vital's knobs */
        tm[i] = 0.06f + kv(SEG[i] + k * ES);
        if (i == 0 || i == 2)
            tm[i] = kv(SEG[i] + k * ES);
        tot += tm[i];
    }
    tm[5] = 0.5f;                        /* the sustain's plateau */
    tot += tm[5];
    {
        float W = 228.0f / tot, y0 = 128.0f, H = 96.0f;
        /* delay */
        X[n] = 6; Y[n++] = (int32_t)y0;
        x = tm[0] * W;
        X[n] = 6 + (int32_t)x; Y[n++] = (int32_t)y0;
        for (s = 1; s <= 12; s++) {      /* attack */
            float u = (float)s / 12.0f;
            X[n] = 6 + (int32_t)(x + tm[1] * W * u);
            Y[n++] = (int32_t)(y0 - H * power_scale(u, ap));
        }
        x += tm[1] * W;
        x += tm[2] * W;                  /* hold */
        X[n] = 6 + (int32_t)x; Y[n++] = (int32_t)(y0 - H);
        for (s = 1; s <= 12; s++) {      /* decay */
            float u = (float)s / 12.0f;
            X[n] = 6 + (int32_t)(x + tm[3] * W * u);
            Y[n++] = (int32_t)(y0 - H * (1.0f + (sus - 1.0f) * power_scale(u, dp)));
        }
        x += tm[3] * W;
        x += tm[5] * W;                  /* sustain */
        X[n] = 6 + (int32_t)x; Y[n++] = (int32_t)(y0 - H * sus);
        for (s = 1; s <= 12; s++) {      /* release */
            float u = (float)s / 12.0f;
            X[n] = 6 + (int32_t)(x + tm[4] * W * u);
            Y[n++] = (int32_t)(y0 - H * sus * (1.0f - power_scale(u, rp)));
        }
        cv_rect(6, (int32_t)y0 + 1, 228, 1, G1);
        plot_poly(X, Y, n, picked ? PURP : TEAL, 1);
        if (CORE_VIEW.active) {          /* the newest voice: where it is */
            float st = CORE_VIEW.env_stage[k];
            int stg = (int)st;
            float fr = st - (float)stg, px, lv = CORE_VIEW.src[MS_ENV1 + k];
            float a0 = 0.0f;
            static const int SI[7] = {0, 1, 2, 3, 5, 4, 4};
            for (i = 0; i < stg && i < 6; i++)
                a0 += (i == 4 ? tm[5] : tm[SI[i]]) * W;
            px = a0 + (stg < 6 ? (stg == 4 ? tm[5] * 0.5f : tm[SI[stg]] * fr) * W : 0.0f);
            if (stg < 6)
                disc(6 + (int32_t)px, (int32_t)(y0 - H * lv), 4, WHITE);
        }
    }
    {
        char t[16];
        secs_text(env_time(kv(P_ENV(k, ATK))) + env_time(kv(P_ENV(k, DEC))), t);
        cv_text(4, 23, &FONT_T, k == 0 ? "AMPLITUDE" : "", G3);
        rtext(236, 23, &FONT_T, picked ? "PICKED UP" : "", PURP);
    }
}

/* -------------------------------------------------------------------- the LFOs --- */
static void pic_lfo(int k)
{
    int shape = pt.v[P_LFO(k, SHAPE)] % LS_N, i, row = ui.row[ui.page], picked = ui.pick == MS_LFO1 + k;
    int32_t X[97], Y[97];
    float amp = kv(P_LFO(k, AMP));
    for (i = 0; i <= 96; i++) {
        float x = (float)i / 96.0f, v = 0.5f + (lfo_shape(shape, pt.lfo_y[k], pt.lfo_c[k], x) - 0.5f) * amp;
        X[i] = 8 + i * 224 / 96;
        Y[i] = 130 - (int32_t)(v * 98.0f);
    }
    cv_rect(8, 81, 224, 1, G1);
    plot_poly(X, Y, 97, picked ? PURP : TEAL, 1);
    if (shape == LS_DRAWN || row == 3) { /* the points */
        for (i = 0; i < LFO_PTS; i++) {
            int32_t x = 8 + i * 224 / LFO_PTS, y = 130 - (int32_t)((float)pt.lfo_y[k][i] / 255.0f * 98.0f);
            if (i == ui_dpt[k] && row == 3)
                ring(x, y, 5, WHITE);
            disc(x, y, 2, row == 3 ? WHITE : G3);
        }
    }
    if (CORE_VIEW.active) {
        float ph = CORE_VIEW.lfo_ph[k], v = CORE_VIEW.src[MS_LFO1 + k];
        disc(8 + (int32_t)(ph * 224.0f), 130 - (int32_t)(v * 98.0f), 4, WHITE);
    }
    {
        char t[32];
        copy_text(t, LS_NAME[shape], sizeof t);
        text_cat(t, "  ", sizeof t);
        text_cat(t, LM_NAME[pt.v[P_LFO(k, MODE)] % LM_N], sizeof t);
        cv_text(4, 23, &FONT_T, t, WHITE);
        param_text(P_LFO(k, RATE), pt.v[P_LFO(k, RATE)], t);
        rtext(236, 23, &FONT_T, t, TEAL);
    }
}

static void pic_rnd(int k)
{
    int32_t X[32], Y[32];
    int i, st = pt.v[P_RND(k, STYLE)] & 3;
    for (i = 0; i < 32; i++) {
        float v = CORE_VIEW.rnd_hist[k][i];
        if (!CORE_VIEW.active) {         /* no voice: a sketch of the style */
            float x = (float)i * 0.25f + ui_t * 0.5f;
            uint32_t a = (uint32_t)x * 2654435761u, b = ((uint32_t)x + 1u) * 2654435761u;
            float ra = (float)(a >> 20) / 4096.0f, rb = (float)(b >> 20) / 4096.0f, f = x - fm_floorf(x);
            v = st == 1 ? ra : st == 2 ? ra + (rb - ra) * (0.5f - 0.5f * fm_sin_turns(f * 0.5f + 0.25f))
              : st == 3 ? 0.5f + 0.4f * fm_sin_turns(x * 0.37f) * fm_sin_turns(x * 0.11f + 0.2f)
                        : ra + (rb - ra) * f * f * (3.0f - 2.0f * f);
        }
        X[i] = 8 + i * 224 / 31;
        Y[i] = 130 - (int32_t)(clampf(v, 0.0f, 1.0f) * 98.0f);
    }
    if (st == 1) {                       /* steps */
        for (i = 1; i < 32; i++) {
            tline(X[i - 1], Y[i - 1], X[i], Y[i - 1], TEAL);
            cv_line(X[i], Y[i - 1], X[i], Y[i], TEAL_D);
        }
    } else {
        plot_poly(X, Y, 32, ui.pick == MS_RND1 + k ? PURP : TEAL, 1);
    }
    disc(X[31], Y[31], 4, WHITE);
    cv_text(4, 23, &FONT_T, RS_NAME[st], WHITE);
}

/* ------------------------------------------------------------------ the matrix --- */
static void pic_matrix(void)
{
    int first = ui_mslot - 3, i, used = 0;
    char t[28], n[6];
    for (i = 0; i < NSLOT; i++)
        used += pt.m[i].src != MS_NONE && pt.m[i].dst != 0xFFFFu;
    if (first < 0)
        first = 0;
    if (first > NSLOT - 7)
        first = NSLOT - 7;
    num_text(used, t);
    text_cat(t, " / 32 IN USE", sizeof t);
    cv_text(4, 23, &FONT_T, t, G3);
    for (i = 0; i < 7; i++) {
        int s = first + i;
        const mslot_t *m = &pt.m[s];
        int32_t y = 37 + i * 15;
        int set_ = m->src != MS_NONE && m->dst != 0xFFFFu;
        uint16_t c = s == ui_mslot ? WHITE : set_ ? G3 : G2;
        if (s == ui_mslot)
            cv_rect(0, y - 2, 240, 14, PANEL);
        num_text(s + 1, n);
        rtext(16, y, &FONT_T, n, G2);
        if (!set_) {
            cv_text(22, y, &FONT_T, "-", G2);
            continue;
        }
        cv_text(22, y, &FONT_T, MS_NAME[m->src % MS_N], (m->flags & 2u) ? G2 : PURP);
        param_long(m->dst, t);
        cv_text(84, y, &FONT_T, t, c);
        {                                /* the amount: a bar from the middle */
            int32_t w = (int32_t)((float)m->amt * 0.03f);
            cv_rect(203, y + 3, 1, 6, G2);
            if (w > 0)
                cv_rect(204, y + 4, w, 4, (m->flags & 2u) ? G2 : PURP);
            else if (w < 0)
                cv_rect(203 + w, y + 4, -w, 4, (m->flags & 2u) ? G2 : PURP);
        }
        if (CORE_VIEW.active && !(m->flags & 2u)) {   /* the source now */
            float v = CORE_VIEW.src[m->src % MS_N];
            disc(76, y + 5, (int32_t)(clampf(fm_fabsf(v), 0.0f, 1.0f) * 3.0f) + 1, WHITE);
        }
    }
}

static void pic_macros(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        int32_t cx = 30 + i * 60, cy = 74, s, n = 0, first = -1;
        float v = kv(P_MC1 + i);
        char t[20];
        arc(cx, cy, 24.0f, -0.375f, 0.375f, G1, 5);
        arc(cx, cy, 24.0f, -0.375f, -0.375f + 0.75f * v, i == ui.mac_sel ? PURP : PURP_D, 5);
        pct_text(v, t);
        ctext(cx, cy - 4, &FONT_T, t, WHITE);
        for (s = 0; s < NSLOT; s++)
            if (pt.m[s].src == MS_MAC1 + i && pt.m[s].dst != 0xFFFFu) {
                n++;
                if (first < 0)
                    first = pt.m[s].dst;
            }
        copy_text(t, "MACRO ", sizeof t);
        num_text(i + 1, t + 6);
        ctext(cx, 30, &FONT_T, t, i == ui.mac_sel ? PURP : G3);
        if (first >= 0) {
            ctext(cx, 108, &FONT_T, param_name(first), G3);
            if (n > 1) {
                num_text(n - 1, t);
                text_cat(t, " MORE", sizeof t);
                ctext(cx, 122, &FONT_T, t, G2);
            }
        } else {
            ctext(cx, 108, &FONT_T, "ARP: ASSIGN", G2);
        }
    }
}

/* ----------------------------------------------------------------- the effects --- */
static float fxv(int t, int i) { return (float)pt.v[P_FX(t) + 1 + i] / 1023.0f; }
static void pic_fx(void)
{
    static const char *const AB[FXT_N] = {"CHO", "CMP", "DLY", "DST", "EQ", "FLT", "FLG", "PHS", "REV"};
    int i, t = ui_fx_sel;
    const volatile fx_view_t *vw = fx_view(t);
    for (i = 0; i < FXT_N; i++) {        /* the chain, in order */
        int f = pt.fx_order[i], on = pt.v[P_FX(f)] & 1u;
        int32_t x = 3 + i * 26;
        cv_rect(x, 22, 24, 15, on ? TEAL_D : G1);
        if (f == t)
            frame_rect(x - 1, 21, 26, 17, WHITE);
        ctext(x + 12, 25, &FONT_T, AB[f], on ? WHITE : G2);
        if (i < FXT_N - 1)
            cv_rect(x + 24, 29, 2, 1, G2);
    }
    cv_text(4, 42, &FONT_T, FXT_NAME[t], pt.v[P_FX(t)] & 1u ? TEAL : G2);
    {
        int32_t y0 = 56, h = 80, X[61], Y[61];
        switch (t) {
        case FXT_EQ: {
            for (i = 0; i <= 60; i++) {
                float p[FX_NP], m, db;
                int q;
                for (q = 0; q < FX_NP; q++)
                    p[q] = fxv(t, q);
                m = fx_eq_response(p, 8.0f + 128.0f * (float)i / 60.0f);
                db = 20.0f * fm_logf(m + 1e-4f) * 0.4342945f;
                X[i] = 4 + i * 232 / 60;
                Y[i] = y0 + h / 2 - (int32_t)(clampf(db, -24.0f, 24.0f) * (float)h / 52.0f);
            }
            cv_rect(4, y0 + h / 2, 232, 1, G1);
            plot_poly(X, Y, 61, TEAL, 1);
            for (i = 0; i < 3; i++)
                disc(4 + (int32_t)(fxv(t, 1 + i * 4) * 232.0f), y0 + h / 2 - (int32_t)((fxv(t, 2 + i * 4) - 0.5f) * 36.0f * (float)h / 52.0f), 4, WHITE);
            break;
        }
        case FXT_FILTER: {
            filt_ctl_t c;
            int md = sel_of(pt.v[P_FX(t) + 1], FLT_N), st = sel_of(pt.v[P_FX(t) + 2], FLT_NSTYLE[md]);
            c.cutoff = 8.0f + 128.0f * fxv(t, 2);
            c.reso = fxv(t, 3);
            c.drive = fxv(t, 4);
            c.blend = fxv(t, 5);
            c.fx = fxv(t, 6);
            c.fy = fxv(t, 7);
            c.transpose = 0.0f;
            cv_rect(4, y0 + h / 3, 232, 1, G1);
            response_curve(md, st, &c, TEAL, 1, y0, h);
            break;
        }
        case FXT_COMP: {                 /* the three bands: level in, gain change */
            static const char *const BN[3] = {"LOW", "BAND", "HIGH"};
            for (i = 0; i < 3; i++) {
                int32_t x = 30 + i * 70, lh = (int32_t)(clampf(vw->a[i], 0.0f, 1.0f) * 70.0f);
                float gr = vw->a[3 + i];
                int32_t gh = (int32_t)(clampf(gr, -24.0f, 24.0f) * 1.4f);
                cv_rect(x, 130 - 70, 16, 70, G1);
                cv_rect(x, 130 - lh, 16, lh, TEAL);
                cv_rect(x + 22, 95, 10, 1, G2);
                if (gh > 0)
                    cv_rect(x + 22, 95 - gh, 10, gh, PURP);
                else if (gh < 0)
                    cv_rect(x + 22, 95, 10, -gh, AMBER);
                ctext(x + 16, 132, &FONT_T, BN[i], G3);
            }
            break;
        }
        case FXT_DELAY: {                /* the taps: fading copies on a line */
            float fb = fxv(t, 5), a = 1.0f;
            int32_t step = 18 + (int32_t)(fxv(t, 1) * 30.0f);
            cv_rect(4, 120, 232, 1, G1);
            for (i = 0; i < 12 && 10 + i * step < 236; i++) {
                int32_t x = 10 + i * step, hh = (int32_t)(a * 60.0f);
                cv_rect(x, 120 - hh, 3, hh, i ? mix565(TEAL, BG, 1.0f - a) : WHITE);
                a *= 0.25f + 0.73f * fb;
            }
            break;
        }
        case FXT_DIST: {                 /* the transfer curve */
            int ty = sel_of(pt.v[P_FX(t) + 1], 6);
            float drive = db_gain(-30.0f + 60.0f * fxv(t, 1));
            for (i = 0; i <= 60; i++) {
                float x = (float)i / 30.0f - 1.0f, y = x * drive;
                if (ty == 0) y = fm_tanhf(y);
                else if (ty == 1) y = clampf(y, -1.0f, 1.0f);
                else if (ty == 2) { y = y * 0.25f + 0.25f; y = 4.0f * fm_fabsf(y - fm_floorf(y + 0.5f)) - 1.0f; }
                else if (ty == 3) y = fm_sin_turns(y * 0.25f);
                else if (ty == 4) y = fm_floorf(x * (2.0f + 30.0f * (1.0f - fxv(t, 1))) + 0.5f) / (2.0f + 30.0f * (1.0f - fxv(t, 1)));
                X[i] = 60 + i * 2;
                Y[i] = y0 + h / 2 - (int32_t)(clampf(y, -1.2f, 1.2f) * 36.0f);
            }
            frame_rect(58, y0 + 2, 124, h - 4, G1);
            plot_poly(X, Y, 61, TEAL, 1);
            break;
        }
        case FXT_REVERB: {               /* the decay, in a room */
            float dec = 0.1f * fm_powf(640.0f, fxv(t, 1)), sz = fxv(t, 0);
            for (i = 0; i <= 60; i++) {
                float s = (float)i / 60.0f * 4.0f;
                X[i] = 4 + i * 232 / 60;
                Y[i] = 130 - (int32_t)(70.0f * fm_expf(-6.9f * s / (dec + 0.05f)));
            }
            plot_poly(X, Y, 61, TEAL, 1);
            frame_rect(160 - (int32_t)(sz * 40.0f), 70 - (int32_t)(sz * 20.0f), 20 + (int32_t)(sz * 80.0f), 20 + (int32_t)(sz * 40.0f), G2);
            break;
        }
        default: {                       /* chorus, flanger, phaser: their sweep, and where it is */
            float ph = vw->a[0];
            for (i = 0; i <= 60; i++) {
                X[i] = 4 + i * 232 / 60;
                Y[i] = y0 + h / 2 - (int32_t)(fm_sin_turns((float)i / 60.0f * 2.0f) * 30.0f * (0.3f + 0.7f * fxv(t, t == FXT_CHORUS ? 3 : 4)));
            }
            plot_poly(X, Y, 61, TEAL, 1);
            disc(4 + (int32_t)(frac1(ph) * 116.0f), y0 + h / 2 - (int32_t)(fm_sin_turns(ph) * 30.0f * (0.3f + 0.7f * fxv(t, t == FXT_CHORUS ? 3 : 4))), 4, WHITE);
            break;
        }
        }
    }
    if (!(pt.v[P_FX(t)] & 1u))
        rtext(236, 42, &FONT_T, "OFF: PRESS FX", G2);
}

static void pic_voice(void)
{
    int i;
    char t[24];
    for (i = 0; i < NVOICE; i++) {
        int32_t x = 8 + i * 29;
        int on = (CORE_VIEW.vmask >> i) & 1u, en = i <= pt.v[P_V_POLY];
        cv_rect(x, 40, 25, 50, on ? TEAL_D : en ? G1 : BG);
        frame_rect(x, 40, 25, 50, en ? G2 : G1);
        if (on) {
            note_text(CORE_VIEW.vnote[i], t);
            ctext(x + 12, 60, &FONT_T, t, WHITE);
        }
    }
    copy_text(t, "CPU ", sizeof t);
    num_text((int32_t)(CORE_VIEW.cpu * 100.0f), t + 4);
    text_cat(t, "%", sizeof t);
    cv_text(8, 100, &FONT_T, t, G3);
    copy_text(t, "UNISON BUDGET ", sizeof t);
    num_text(CORE_VIEW.budget, t + 14);
    cv_text(8, 114, &FONT_T, t, G3);
    if (CORE_VIEW.shed) {
        copy_text(t, "SHED ", sizeof t);
        num_text((int32_t)CORE_VIEW.shed, t + 5);
        rtext(232, 100, &FONT_T, t, AMBER);
    }
    rtext(232, 114, &FONT_T, pt.v[P_V_POLY] ? "POLY" : pt.v[P_V_LEGATO] ? "MONO LEGATO" : "MONO", TEAL);
}

static void draw_picture(void)
{
    int pg = ui.page;
    if (pg_osc(pg)) pic_osc(pg - PG_OSC1);
    else if (pg == PG_SMP) pic_smp();
    else if (pg_flt(pg)) pic_filter(pg - PG_F1);
    else if (pg_env(pg)) pic_env(pg - PG_E1);
    else if (pg_lfo(pg)) pic_lfo(pg - PG_L1);
    else if (pg_rnd(pg)) pic_rnd(pg - PG_R1);
    else if (pg == PG_MAT) pic_matrix();
    else if (pg == PG_MAC) pic_macros();
    else if (pg == PG_FX) pic_fx();
    else pic_voice();
}
