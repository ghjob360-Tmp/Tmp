/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the screens (included at the end of ui_screens.inc). */

/* ------------------------------------------------------------- the header --- */
static void draw_header(void)
{
    static const char *const MN[4] = {"SYNTH", "DRUM", "TAPE", "MIXER"};
    static const char *const MIXP[4] = {"LEVELS", "EQ", "MASTER FX", "OUT"};
    char t[32], n[8];
    int i;
    int32_t x;
    cv_rect(0, 0, 240, 19, BG);
    cv_rect(4, 4, 11, 11, MODEC[ui.mode]);      /* the mode's colour */
    copy_text(t, MN[ui.mode], sizeof t);
    if (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) {
        const sound_t *s = cur_sound(mode_kind());
        int pg = ui.page[ui.mode];
        text_cat(t, " ", sizeof t);
        if (pg == PG_T1)
            text_cat(t, mode_kind() ? DRUM_NAME[s->m[M_ENGINE].type % D_N] : ENG_NAME[s->m[M_ENGINE].type % E_NSYNTH], sizeof t);
        else if (pg == PG_T2)
            text_cat(t, "ENVELOPE", sizeof t);
        else if (pg == PG_T3)
            text_cat(t, FX_NAME[s->m[M_FX].type % FX_N], sizeof t);
        else if (pg == PG_T4)
            text_cat(t, LFO_NAME[s->m[M_LFO].type % L_N], sizeof t);
        else
            text_cat(t, SEQ_NAME[s->m[M_SEQ].type % Q_N], sizeof t);
        if ((pg == PG_T3 && !s->m[M_FX].on) || (pg == PG_T4 && !s->m[M_LFO].on) || (pg == PG_SEQ && !s->m[M_SEQ].on))
            text_cat(t, " OFF", sizeof t);
    } else if (ui.mode == MD_MIX) {
        text_cat(t, " ", sizeof t);
        text_cat(t, ui.path ? "PATH" : MIXP[ui.page[MD_MIX] & 3], sizeof t);
    }
    cv_text(19, 3, &FONT_XS, t, K_WHITE);
    for (i = 0; i < NTRK; i++) {         /* the tracks: lit with their sound, the current one white */
        float act = fm_minf(CORE_VIEW.trk_peak[i] * 12.0f, 1.0f);
        x = 128 + i * 16;
        n[0] = (char)('1' + i);
        n[1] = 0;
        cv_rect(x, 3, 13, 13, i == cur ? K_WHITE : mix565(G1, MODEC[pj.t[i].kind ? 1 : 0], 0.2f + act * 0.8f));
        ctext(x + 6, 3, &FONT_T, n, i == cur ? BG : pj.t[i].mute ? G2 : K_WHITE);
    }
    x = 196;
    if (CORE_VIEW.playing) {             /* play: a triangle; record: a red dot */
        for (i = 0; i < 9; i++)
            cv_rect(x + i / 2, 5 + i / 2, 1, 9 - (i / 2) * 2, RGB(70, 220, 120));
    } else {
        cv_rect(x, 5, 8, 9, G2);
    }
    if (ui.armed && (!CORE_VIEW.recording || (ui_now / 250u) & 1u))
        disc(x + 15, 9, 4, REDC);
    if (plat_usb_host_state() != USBH_OFF) {   /* the USB host: its mark where the tempo is */
        usbh_mark(222, 4, K_WHITE, G2);
    } else {
        num_text(pj.tempo, n);
        rtext(238, 4, &FONT_T, n, G3);
    }
}

/* ------------------------------------------------------------ my pictures --- */
static void pic_env_synth(const float k[8])
{
    float a = k[0], dd = k[1], s = k[2], r = k[3], lv = CORE_VIEW.env[cur];
    int32_t x0 = 14, yb = 136, yt = 30, xa = x0 + 6 + (int32_t)(a * 50.0f), xd = xa + 6 + (int32_t)(dd * 50.0f),
            ys = yb - (int32_t)(s * (float)(yb - yt)), xs = xd + 34, xr = xs + 6 + (int32_t)(r * 56.0f), i;
    for (i = x0; i < xr; i += 3) {       /* under the curve, faint */
        int32_t y = i < xa ? yb - (yb - yt) * (i - x0) / (xa - x0 > 0 ? xa - x0 : 1)
                  : i < xd ? yt + (ys - yt) * (i - xa) / (xd - xa > 0 ? xd - xa : 1)
                  : i < xs ? ys : ys + (yb - ys) * (i - xs) / (xr - xs > 0 ? xr - xs : 1);
        cv_rect(i, y, 1, yb - y, G1);
    }
    tline(x0, yb, xa, yt, kcol(0));
    tline(xa, yt, xd, ys, kcol(1));
    tline(xd, ys, xs, ys, kcol(2));
    tline(xs, ys, xr, yb, kcol(3));
    disc(xa, yt, 3, kcol(0));
    disc(xd, ys, 3, kcol(1));
    disc(xs, ys, 3, kcol(2));
    disc(xr, yb, 3, kcol(3));
    if (lv > 0.002f) {                   /* the newest voice, now */
        int32_t y = yb - (int32_t)(lv * (float)(yb - yt));
        cv_rect(x0, y, 222 - x0, 1, mix565(BG, K_WHITE, 0.4f));
        disc(226, y, 4, K_WHITE);
    }
    if (ui_shift) {                      /* the second row: how it plays */
        char t[24];
        copy_text(t, PM_N_[(int)(k[4] * 3.999f)], sizeof t);
        rtext(230, 24, &FONT_XS, t, kdim(0));
    }
}

static void pic_env_drum(const float k[8])
{
    float al = k[0] * 2.0f, mid = k[1] * 1.5f, rl = 0.03f + 3.0f * k[2] * k[2], tm = 0.002f + 0.15f * k[3];
    float v[96];
    int i;
    for (i = 0; i < 96; i++) {           /* one second of the shaping */
        float t = (float)i / 95.0f * 1.0f;
        v[i] = (t < tm ? lerpf(al, mid, t / tm) : mid * fm_expf(-(t - tm) / rl)) * 0.5f - 0.6f;
    }
    wave_line(16, 208, 82, 50, v, 96, K_WHITE, 1);
    disc(16, 82 - (int32_t)((al * 0.5f - 0.6f) * 50.0f), 4, kcol(0));
    disc(16 + (int32_t)(tm * 208.0f), 82 - (int32_t)((mid * 0.5f - 0.6f) * 50.0f), 4, kcol(1));
    cv_rect(16 + (int32_t)(fm_minf(rl, 1.0f) * 208.0f), 120, 2, 14, kcol(2));
    cv_rect(16, 136, (int32_t)(tm / 0.152f * 60.0f), 3, kcol(3));
}

static void pic_drum_kit(void)
{
    const sound_t *s = &pj.t[cur].drum;
    int i, sel = ui.sel_key[cur] % NDKEY, slice = s->m[M_ENGINE].type == D_SLICE;
    for (i = 0; i < NDKEY; i++) {        /* 24 pads, laid out as the keys: 12 a row */
        int32_t x = 6 + (i % 12) * 19, y = slice ? 96 : 24 + (i / 12) * 44;
        float since = (float)(ui_now - ui_hit_at[cur][i]) * 0.001f, g = since < 0.35f ? 1.0f - since / 0.35f : 0.0f;
        int32_t h = slice ? 18 : 38, bump = (int32_t)(g * 3.0f);
        if (slice && i >= 12)
            y += 22;
        cv_rect(x, y - bump, 17, h + bump, mix565(G1, MODE_C1, 0.15f + g * 0.85f));
        if (i == sel) {
            frame_rect(x - 1, y - 1 - bump, 19, h + 2 + bump, K_ORANGE);
        }
    }
    if (slice) {                         /* the sample, the chosen key's slice in its colours */
        const uint8_t *d = g_drum_sample.data;
        uint32_t n = g_drum_sample.len, j;
        const dpad_t *p = &pj.t[cur].pads[sel];
        float in = kn(p->p[1]) + (kn(p->p[5]) - 0.5f) * 0.01f, out = kn(p->p[2]) + (kn(p->p[6]) - 0.5f) * 0.01f;
        for (j = 0; j < 228 && n; j++) {
            uint32_t a = (uint32_t)((float)j / 228.0f * (float)n), b = (uint32_t)((float)(j + 1) / 228.0f * (float)n), q;
            float mx = 0.0f, u = (float)j / 228.0f;
            for (q = a; q < b && q < n; q += 7)
                mx = fm_maxf(mx, fm_fabsf(ulaw(d[q])));
            cv_rect(6 + (int32_t)j, 58 - (int32_t)(mx * 30.0f), 1, 1 + (int32_t)(mx * 60.0f),
                    u >= in && u <= out ? kcol(0) : G2);
        }
        cv_rect(6 + (int32_t)(in * 228.0f), 24, 2, 68, kcol(1));
        cv_rect(6 + (int32_t)(out * 228.0f), 24, 2, 68, kcol(2));
    }
}

static void pic_tape(void)
{
    const tape_t *tp = core_tape();
    uint32_t head = CORE_VIEW.tick;
    float rl = CORE_VIEW.reel, used = (float)head / (float)TAPE_TICKS;
    int t, i, b;
    const int32_t span = 8 * 4 * TPQ;    /* the lanes show eight bars, the head a third in */
    int32_t start = (int32_t)head - span / 3;
    /* the reels, turning; the tape wound from one to the other */
    for (b = 0; b < 2; b++) {
        int32_t cx = b ? 186 : 54, cy = 48;
        float pack = b ? used : 1.0f - used;
        disc(cx, cy, 10 + (int32_t)(pack * 16.0f), RGB(56, 42, 34));
        ring(cx, cy, 27, G2);
        disc(cx, cy, 5, G3);
        for (i = 0; i < 3; i++) {
            float a = -rl * (b ? 1.0f : 1.4f) + (float)i / 3.0f;
            tline(cx + (int32_t)(fm_sin_turns(a) * 6.0f), cy - (int32_t)(fm_sin_turns(a + 0.25f) * 6.0f),
                  cx + (int32_t)(fm_sin_turns(a) * 11.0f), cy - (int32_t)(fm_sin_turns(a + 0.25f) * 11.0f), K_WHITE);
        }
    }
    cv_line(54 - 24, 58, 112, 76, G3);
    cv_line(186 + 24, 58, 128, 76, G3);
    cv_rect(110, 74, 20, 6, CORE_VIEW.recording ? REDC : G3);   /* the head */
    {                                    /* where we are, in bars and beats */
        char s[16], n[8];
        num_text((int32_t)(head / (4u * TPQ)) + 1, s);
        text_cat(s, ".", 16);
        num_text((int32_t)(head / TPQ % 4u) + 1, n);
        text_cat(s, n, 16);
        ctext(120, 36, &FONT_B, s, K_WHITE);
        if (CORE_VIEW.breaking)
            ctext(120, 54, &FONT_T, "BREAK", K_ORANGE);
    }
    /* the ruler and the loop */
    for (b = -1; b < 10; b++) {
        int32_t bt = (start / (4 * TPQ) + b) * 4 * TPQ, x = 4 + (bt - start) * 232 / span;
        if (x < 4 || x > 236)
            continue;
        cv_rect(x, 84, 1, 54, G1);
        if (bt >= 0) {
            char n[6];
            num_text(bt / (4 * TPQ) + 1, n);
            cv_text(x + 2, 83, &FONT_T, n, G2);
        }
    }
    if (pj.loop_on && pj.loop_out > pj.loop_in) {
        int32_t lx0 = 4 + ((int32_t)pj.loop_in * 4 * TPQ - start) * 232 / span,
                lx1 = 4 + ((int32_t)pj.loop_out * 4 * TPQ - start) * 232 / span;
        if (lx0 < 4) lx0 = 4;
        if (lx1 > 236) lx1 = 236;
        if (lx1 > lx0)
            cv_rect(lx0, 96, lx1 - lx0, 2, K_ORANGE);
    }
    for (t = 0; t < NTRK; t++) {         /* the lanes: the notes as marks, the current track lit */
        int32_t y = 100 + t * 10;
        int here = t == cur;
        cv_rect(4, y, 232, 8, here ? (ui.armed ? RGB(60, 16, 22) : G1) : BG);
        for (i = 0; i < tp->n[t] && i < TAPE_EV; i++) {
            const tev_t *e = &tp->ev[t][i];
            int32_t x;
            if (!e->vel || (int32_t)e->tick < start || (int32_t)e->tick >= start + span)
                continue;
            x = 4 + ((int32_t)e->tick - start) * 232 / span;
            cv_rect(x, y + 6 - (int32_t)(e->note % 12) / 2, 2, 2,
                    here ? MODEC[e->kind ? 1 : 0] : mix565(MODEC[e->kind ? 1 : 0], BG, 0.55f));
        }
        if (pj.t[t].mute)
            cv_rect(4, y + 3, 232, 1, G2);
    }
    cv_rect(4 + (int32_t)(span / 3) * 232 / span, 84, 1, 56, CORE_VIEW.recording ? REDC : K_WHITE);
}

static void pic_tape_tricks(void)        /* GLO on the tape screen: what each white key does */
{
    static const char *const T[16] = {"IN", "OUT", "LOOP", "BREAK", "REV", "CHOP", "CHOP2", "LIFT",
                                      "DROP", "ALL", "M1", "M2", "START", "x1", "CLICK", "-"};
    static const char *const KN = "FGABCDEFGABCDEFG";   /* the white keys from the left */
    static const uint8_t GR[16] = {0, 0, 0, 3, 3, 3, 3, 1, 1, 1, 2, 2, 4, 4, 4, 4};   /* loop, play, edit, memo, go */
    int i;
    cv_rect(0, 70, 240, 74, G1);
    ctext(120, 73, &FONT_T, "GLO + A WHITE KEY", K_ORANGE);
    for (i = 0; i < 16; i++) {
        int32_t x = 2 + (i & 7) * 30, y = 86 + (i >> 3) * 29;
        int held = (int)((ui.trick_down >> i) & 1u);
        uint16_t c = GR[i] < 4 ? KC[GR[i]] : G3;
        char kn[2] = {KN[i], 0};
        cv_rect(x, y, 28, 26, held ? c : BG);
        cv_rect(x, y, 28, 2, c);
        cv_text(x + 2, y + 3, &FONT_T, kn, held ? BG : G2);
        ctext(x + 14, y + 13, &FONT_T, T[i], held ? BG : K_WHITE);
    }
}

static void pic_levels(void)
{
    int i;
    for (i = 0; i < NTRK; i++) {         /* four arcs with the level in big numbers, a pan dot, a meter */
        const track_t *t = &pj.t[i];
        int32_t cx = 30 + i * 60, cy = 80;
        float lv = (float)t->level / 1023.0f, m = fm_minf(CORE_VIEW.trk_peak[i] * 4.0f, 1.0f);
        char n[6];
        arc(cx, cy, 24.0f, -0.4f, 0.4f, KD[i], 4);
        arc(cx, cy, 24.0f, -0.4f, -0.4f + 0.8f * lv, t->mute ? G2 : kcol(i), 4);
        arc(cx, cy, 18.0f, -0.4f, -0.4f + 0.8f * m, m > 0.9f ? REDC : K_WHITE, 2);
        num_text((int32_t)(lv * 99.0f + 0.5f), n);
        ctext(cx, cy - 8, &FONT_B, n, t->mute ? G2 : K_WHITE);
        disc(cx + (int32_t)((kn(t->pan) - 0.5f) * 36.0f), cy + 26, 3, ui_shift ? KC[i] : G2);
        ctext(cx, cy + 34, &FONT_T, t->kind ? "DRUM" : ENG_NAME[t->synth.m[M_ENGINE].type % E_NSYNTH], i == cur ? K_WHITE : G3);
    }
}

static void pic_eq(void)
{
    float lo = kn(pj.eq.p[0]) - 0.5f, mid = kn(pj.eq.p[1]) - 0.5f, hi = kn(pj.eq.p[2]) - 0.5f,
          amt = pj.eq.on ? kn(pj.eq.p[3]) : 0.0f, v[96];
    int i;
    for (i = 0; i < 96; i++) {           /* a sketch of the response: shelves and a bell */
        float u = (float)i / 95.0f, l = 1.0f / (1.0f + fm_expf((u - 0.25f) * 18.0f)),
              h = 1.0f / (1.0f + fm_expf(-(u - 0.75f) * 18.0f)), m = fm_expf(-(u - 0.5f) * (u - 0.5f) * 40.0f);
        v[i] = (lo * l + mid * m + hi * h) * amt * 1.6f;
    }
    cv_rect(12, 82, 216, 1, G2);
    wave_line(12, 216, 82, 50, v, 96, K_WHITE, 1);
    disc(12 + 54, 82 - (int32_t)(lo * amt * 80.0f), 5, kcol(0));
    disc(12 + 108, 82 - (int32_t)(mid * amt * 80.0f), 5, kcol(1));
    disc(12 + 162, 82 - (int32_t)(hi * amt * 80.0f), 5, kcol(2));
    ctext(120, 118, &FONT_XS, amt < 0.01f ? "CLEAN" : amt > 0.99f ? "MAX EQ" : "EQ", kcol(3));
}

static void pic_out(void)
{
    float l = kn(pj.mout.p[0]), r = kn(pj.mout.p[1]), drive = kn(pj.mout.p[2]), rel = kn(pj.mout.p[3]),
          gr = fm_minf(CORE_VIEW.gr * (1.0f / 24.0f), 1.0f), pk = fm_minf(CORE_VIEW.out_peak, 1.0f);
    int i;
    cv_rect(20, 36, 16, 96, G1);         /* left, right */
    cv_rect(20, 36 + (int32_t)((1.0f - l) * 96.0f), 16, (int32_t)(l * 96.0f), kcol(0));
    cv_rect(204, 36, 16, 96, G1);
    cv_rect(204, 36 + (int32_t)((1.0f - r) * 96.0f), 16, (int32_t)(r * 96.0f), kcol(1));
    for (i = 0; i < 12; i++) {           /* drive: a squeezed wave, white */
        float a = (float)i / 12.0f;
        int32_t x = 60 + i * 10, h = (int32_t)(fm_tanhf(fm_sin_turns(a * 2.0f + ui_t * 0.5f) * (1.0f + drive * 6.0f)) * 30.0f);
        cv_rect(x, 84 - (h > 0 ? h : 0), 6, h > 0 ? h : -h, kcol(2));
    }
    cv_rect(60, 124, (int32_t)(gr * 120.0f), 4, K_ORANGE);   /* the gain reduction, pumping by the release */
    cv_rect(60, 132, (int32_t)(rel * 120.0f), 2, kcol(3));
    cv_rect(60, 30, (int32_t)(pk * 120.0f), 3, pk > 0.95f ? REDC : K_WHITE);
}

static void pic_path(void)               /* the sound path: tracks -> levels -> EQ -> master FX -> out */
{
    static const char *const N[4] = {"EQ", "FX", "DRIVE", "OUT"};
    int i;
    for (i = 0; i < NTRK; i++) {
        int32_t y = 28 + i * 26;
        uint16_t c = pj.t[i].level == 0 || pj.t[i].mute ? REDC : MODEC[pj.t[i].kind ? 1 : 0];
        char n[4] = {'T', (char)('1' + i), 0, 0};
        cv_rect(8, y, 30, 18, c);
        ctext(23, y + 2, &FONT_XS, n, BG);
        cv_line(38, y + 9, 66, 82, G2);
    }
    for (i = 0; i < 4; i++) {
        int32_t x = 66 + i * 44, on = i == 0 ? pj.eq.on : i == 1 ? pj.mfx.on : 1;
        cv_rect(x, 72, 36, 20, on ? K_WHITE : G2);
        ctext(x + 18, 75, &FONT_T, N[i], BG);
        if (i < 3)
            cv_line(x + 36, 82, x + 44, 82, G3);
    }
    {
        float pk = fm_minf(CORE_VIEW.out_peak * 2.0f, 1.0f);
        cv_rect(66, 104, (int32_t)(pk * 168.0f), 4, K_WHITE);
    }
    ctext(120, 120, &FONT_T, "A RED TRACK IS SILENT", G2);
}

static void pic_sampler(void)            /* the sampler's steps over the picture */
{
    char t[24];
    float maxs = (float)(smp.end - smp.base - 256u) / 22050.0f;
    cv_rect(0, 20, 240, 124, BG);
    if (smp.state == SM_ERASING) {
        float u = (float)(smp.next_erase - smp.base) / (float)(smp.end - smp.base);
        ctext(120, 46, &FONT_B, "PREPARING", K_WHITE);
        cv_rect(30, 80, 180, 6, G1);
        cv_rect(30, 80, (int32_t)(u * 180.0f), 6, K_ORANGE);
    } else if (smp.state == SM_READY) {
        ctext(120, 40, &FONT_B, "READY", K_WHITE);
        ctext(120, 66, &FONT_XS, "HOLD A KEY TO SAMPLE", K_ORANGE);
        ctext(120, 84, &FONT_T, "SOURCE: THE MASTER OUT", G3);
        dec1_text(maxs, t);
        text_cat(t, " s MAX", sizeof t);
        ctext(120, 100, &FONT_T, t, G3);
    } else {
        float s = (float)CORE_VIEW.sample_len / 22050.0f;
        int i;
        cv_rect(0, 20, 240, 20, REDC);
        ctext(120, 22, &FONT_XS, "RECORDING", K_WHITE);
        cv_rect(20, 70, 200, 18, G1);
        cv_rect(20, 70, (int32_t)(s / maxs * 200.0f), 18, REDC);
        for (i = 0; i <= (int)maxs; i++)
            cv_rect(20 + (int32_t)((float)i / maxs * 200.0f), 90, 1, 5, G3);
        dec1_text(s, t);
        text_cat(t, " s", sizeof t);
        ctext(120, 104, &FONT_B, t, K_WHITE);
    }
}

static void draw_help(void)              /* SAVE held: the controls of this mode */
{
    static const char *const H[4][9] = {
        {"SYNTH", "SEL ENV FX LFO  engine envelope fx lfo", "FX / LFO again  on / off", "SELECT  engine / fx / lfo type",
         "PRESET  sound slot 1-8", "GLO + knobs  the second row", "SEQ  the sequencer  (again on/off)", "GLO+SAVE store  GLO+EDIT revert",
         "REC on SAMPLER  sample the output"},
        {"DRUM", "keys  24 pads; the last one is edited", "SEL ENV FX LFO  kit envelope fx lfo", "SELECT  KIT / SLICES",
         "PRESET  kit 1-8", "GLO + knobs  the second row", "SEQ  the sequencer  (again on/off)", "REC on SLICES  sample the output",
         "GLO+SAVE store  GLO+ARP revert"},
        {"TAPE", "PLAY  play / stop   GLO+PLAY  reverse", "REC  record arm (overdub)", "SEL ENV FX LFO  track 1-4 (GLO mute)",
         "GLO + white keys  tape tricks", "SELECT scrub   OCT- OCT+ a bar", "K1 head  K2 loop in  K3 speed  K4 length",
         "GLO+REC held  erase the track", "ALGORITHM  tempo"},
        {"MIXER", "SEL  levels (GLO pans)", "ENV  EQ (again on/off)", "FX  master fx (again on/off)", "LFO  out: drive, release",
         "GLO+HOME  the sound path", "SELECT  master fx type", "HOME  back to the tape", "SAVE  save the project"}};
    int i;
    cv_rect(0, 20, 240, 124, G1);
    for (i = 0; i < 9; i++)
        cv_text(8, 22 + i * 13, &FONT_T, H[ui.mode][i], i ? K_WHITE : MODEC[ui.mode]);
}

static void draw_top(void)
{
    cv_begin(240, 144, BG);
    draw_header();
    if (ui.help) {
        draw_help();
    } else if (smp.state != SM_OFF) {
        pic_sampler();
    } else if (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) {
        int kind = mode_kind(), pg = ui.page[ui.mode], t = cur;
        const sound_t *s = cur_sound(kind);
        float k[8];
        int i;
        for (i = 0; i < 8; i++)          /* the parameters as heard (the LFO on them) when this sound plays */
            k[i] = pj.t[t].kind == kind && (pg == PG_T1 || pg == PG_T2 || pg == PG_T3)
                       ? CORE_VIEW.eff[t][pg == PG_T1 ? 0 : pg == PG_T2 ? 1 : 2][i]
                       : kn(s->m[pg == PG_T1 ? M_ENGINE : pg == PG_T2 ? M_ENV : pg == PG_T3 ? M_FX : pg == PG_T4 ? M_LFO : M_SEQ].p[i]);
        if (pg == PG_T1) {
            if (kind)
                pic_drum_kit();
            else
                pic_engine(s->m[M_ENGINE].type % E_NSYNTH, k, CORE_VIEW.env[t], CORE_VIEW.note[t], CORE_VIEW.env[t] > 0.001f);
        } else if (pg == PG_T2) {
            if (kind)
                pic_env_drum(k);
            else
                pic_env_synth(k);
        } else if (pg == PG_T3) {
            pic_fx(s->m[M_FX].type % FX_N, k, fm_minf(CORE_VIEW.trk_peak[t] * 4.0f, 1.0f));
        } else if (pg == PG_T4) {
            pic_lfo(s->m[M_LFO].type % L_N, &s->m[M_LFO], CORE_VIEW.lfo[t], CORE_VIEW.lfo_ph[t]);
        } else {
            pic_seq(s->m[M_SEQ].type % Q_N, &s->m[M_SEQ], cur_seqdata(kind), CORE_VIEW.seq_step[t][kind], kind);
        }
        if ((pg == PG_T3 && !s->m[M_FX].on) || (pg == PG_T4 && !s->m[M_LFO].on) || (pg == PG_SEQ && !s->m[M_SEQ].on))
            ctext(120, 130, &FONT_T, "OFF: PRESS AGAIN TO TURN ON", G3);
    } else if (ui.mode == MD_TAPE) {
        pic_tape();
        if (ui.glo)
            pic_tape_tricks();
    } else if (ui.path) {
        pic_path();
    } else {
        switch (ui.page[MD_MIX]) {
        case PG_T1: pic_levels(); break;
        case PG_T2: pic_eq(); break;
        case PG_T3: {
            float k[8];
            int i;
            for (i = 0; i < 8; i++)
                k[i] = kn(pj.mfx.p[i]);
            pic_fx(pj.mfx.type % FX_N, k, fm_minf(CORE_VIEW.out_peak * 2.0f, 1.0f));
            if (!pj.mfx.on)
                ctext(120, 130, &FONT_T, "OFF: PRESS AGAIN TO TURN ON", G3);
            break;
        }
        default: pic_out(); break;
        }
    }
    if (ui_now < ui.toast_until && !ui.help) {   /* a toast: a pill under the header */
        int32_t w = text_w(&FONT_XS, ui.toast[0]) + (ui.toast[1][0] ? 8 + text_w(&FONT_XS, ui.toast[1]) : 0) + 16, x,
                left = (int32_t)(ui.toast_until - ui_now), y = left > 1100 ? 20 - (left - 1100) / 8 : 20;
        cv_rect(120 - w / 2, y, w, 17, G1);
        cv_rect(120 - w / 2, y + 16, w, 1, MODEC[ui.mode]);
        x = cv_text(120 - w / 2 + 8, y + 1, &FONT_XS, ui.toast[0], K_WHITE);
        cv_text(x + 8, y + 1, &FONT_XS, ui.toast[1], K_ORANGE);
    }
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    dl_t d[4];
    char t[20];
    int i;
    cv_begin(240, 96, BG);
    cv_rect(0, 0, 240, 1, G1);
    if (ui.help) {
        ctext(120, 30, &FONT_XS, "LET GO OF SAVE TO CLOSE", G3);
        cv_blit(0, 144);
        return;
    }
    get_dials(d);
    for (i = 0; i < 4; i++) {
        if (!d[i].label) {
            dial(i, 0.0f, 0, 0, ui.glo);
            continue;
        }
        dial_text(&d[i], t);
        dial(i, dial_frac(&d[i]), d[i].label, t, ui.glo);
    }
    if (ui.glo)
        ctext(120, 87, &FONT_T, "GLO", G2);
    if (dirty)
        disc(234, 90, 2, K_ORANGE);
    cv_blit(0, 144);
}

static void lights(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    uint32_t b = ui.btn, k = ui.keys, i;
    if (ui.mode == MD_SYNTH) b |= 1u << B_EDIT;
    if (ui.mode == MD_DRUM) b |= 1u << B_ARP;
    if (ui.mode == MD_TAPE || ui.mode == MD_MIX) b |= 1u << B_HOME;
    if (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) {
        static const uint8_t TB[5] = {B_SEL, B_ENV, B_FX, B_LFO, B_SEQ};
        b |= 1u << TB[ui.page[ui.mode] % 5];
    } else if (ui.mode == MD_MIX) {
        static const uint8_t TB[4] = {B_SEL, B_ENV, B_FX, B_LFO};
        b |= 1u << TB[ui.page[MD_MIX] & 3];
    } else {
        static const uint8_t TB[4] = {B_SEL, B_ENV, B_FX, B_LFO};
        b |= 1u << TB[cur & 3];
    }
    if (CORE_VIEW.playing) b |= 1u << B_PLAY;
    if ((ui.armed || smp.state == SM_READY) && (!CORE_VIEW.recording || (ui_now / 250u) & 1u)) b |= 1u << B_REC;
    if (dirty) b |= 1u << B_SAVE;
    if (cur_sound(pj.t[cur].kind)->m[M_SEQ].on) b |= 1u << B_SEQ;
    if (ui.mode == MD_TAPE && CORE_VIEW.playing) {   /* the 16 white keys: where the loop (or the bar) is */
        float lo = pj.loop_on ? (float)pj.loop_in * 4.0f * TPQ : (float)(CORE_VIEW.tick / (4u * TPQ)) * 4.0f * TPQ,
              len = pj.loop_on ? (float)(pj.loop_out - pj.loop_in) * 4.0f * TPQ : 4.0f * TPQ;
        int idx = (int)(((float)CORE_VIEW.tick - lo) / len * 16.0f);
        k |= 1u << WK[idx & 15];
    } else if (ui.mode == MD_DRUM || (ui.mode == MD_TAPE && pj.t[cur].kind)) {
        for (i = 0; i < NKEYS; i++)
            if (ui_now - ui_hit_at[cur][i % NDKEY] < 120u)
                k |= 1u << i;
    }
    plat_glow(1);
    plat_leds(b, k);
}

/* ------------------------------------------------------------------ the app --- */
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st >= 0x80u && st < 0xF0u)
            PLAT_FAR(core_midi)(st, d1, d2);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    PLAT_FAR(core_render)(lr, n, (float)master_q12 / 4096.0f * 8300000.0f);
}

void app_panic(void) { core_panic(); }
void app_overload(void) { core_overload(); }

/* ---------------------------------------------------------- the second core --- */
/* Some FM-1s cannot run their second core reliably (it misreads shared RAM under load: felucca-bsp chip/fm1_cpu1.h).
 * With cpu2_fallback the platform then retires it and the first core does all the work: the same sound, fewer
 * voices at once. Said on the screen once, when it happens (or at the start, when it was barred before). */
static int sim_one_core;                 /* (the host's tests: pretend it was retired) */
static int cores_why(void) { return sim_one_core ? 1 : plat_cpu2_gave_up(); }
static int cores_now(void) { return cores_why() || !plat_cpu2_on() ? 1 : 2; }
static const char *cores_reason(int r)
{
    switch (r) {
    case 1: return "CORE 2 MISREAD";
    case 2: return "CORE 2 TOO SLOW";
    case 3: return "CORE 2 OFF SINCE START";
    case 4: return "CORE 2 FAULTED";
    case 5: return "CORE 2 NO ANSWER";
    default: return "CORE 2 OFF";
    }
}
static void cores_service(uint32_t now)  /* main loop */
{
    static int said;
    int r;
    if (said || now < 1500u)             /* (after the start's own messages) */
        return;
    r = cores_why();
    if (r) {
        said = 1;
        app_say("ONE CORE", cores_reason(r));
    }
}


void app_init(void)
{
    uint8_t *u = (uint8_t *)(void *)&ui;
    uint32_t i;
    for (i = 0; i < sizeof ui; i++)      /* (the host's reboot keeps RAM: start clean) */
        u[i] = 0;
    cur = 0;
    plat_cpu2_set(1);                    /* the second core: the odd voices */
    dsp_tables_init();
    eng_tables_init();
    core_init();
    lib_factory();
    samples_point();
    if (project_load() != 0) {
        project_default();
        tape_demo(core_tape_rw());
    }
    ui.mode = pj.t[cur].kind ? MD_DRUM : MD_SYNTH;
    ui.mode = MD_TAPE;
    core_current(cur);
    core_project(&pj);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, BG);
    ui.top_dirty = ui.bot_dirty = 1;
}

void app_frame(void)
{
    int t, p;
    int alive;
    ui_now = plat_ms();
    ui_t = (float)ui_now * 0.001f;
    input();
    sampler_service();
    usbh_service();
    cores_service(ui_now);
    {                                    /* the load, about once a second: the voices allowed grow back */
        static uint32_t cpu_at;
        if (ui_now - cpu_at > 1000u) {
            cpu_at = ui_now;
            core_cpu((float)plat_cpu_pct() * 0.01f);
        }
    }
    ann_tick(ui_now);
    for (t = 0; t < NTRK; t++)           /* the drum hits: when */
        for (p = 0; p < NDKEY; p++)
            if (CORE_VIEW.hits[t][p] != ui_hit_seen[t][p]) {
                ui_hit_seen[t][p] = CORE_VIEW.hits[t][p];
                ui_hit_at[t][p] = ui_now;
            }
    alive = CORE_VIEW.playing || CORE_VIEW.out_peak > 0.002f || ui_now < ui.toast_until + 100u || smp.state != SM_OFF;
    for (t = 0; t < 4; t++)
        if (knob_at[t] && ui_now - knob_at[t] < 800u) {
            alive = 1;
            if (ui_now - knob_at[t] > 700u)
                ui.bot_dirty = 1;
        }
    CORE_VIEW.out_peak *= 0.85f;
    /* the picture ~30 / s while anything moves, ~6 when idle; the knobs on change, ~8 / s while the tape plays */
    if (ui.top_dirty || ui_now - ui.top_at >= (alive ? 33u : 160u)) {
        ui.top_at = ui_now;
        ui.top_dirty = 0;
        draw_top();
    }
    if (ui.bot_dirty || (ui.mode == MD_TAPE && CORE_VIEW.playing && ui_now - ui.bot_at >= 120u)) {
        ui.bot_at = ui_now;
        ui.bot_dirty = 0;
        draw_bottom();
    }
    lights();
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "mode")) *got = ui.mode;
    else if (!strcmp(what, "page")) *got = ui.page[ui.mode];
    else if (!strcmp(what, "track")) *got = cur;
    else if (!strcmp(what, "kind")) *got = pj.t[cur].kind;
    else if (!strcmp(what, "engine")) *got = cur_sound(mode_kind())->m[M_ENGINE].type;
    else if (!strcmp(what, "fx_on")) *got = cur_sound(mode_kind())->m[M_FX].on;
    else if (!strcmp(what, "lfo_on")) *got = cur_sound(mode_kind())->m[M_LFO].on;
    else if (!strcmp(what, "seq_on")) *got = cur_sound(mode_kind())->m[M_SEQ].on;
    else if (!strcmp(what, "seq_type")) *got = cur_sound(mode_kind())->m[M_SEQ].type;
    else if (!strcmp(what, "playing")) *got = CORE_VIEW.playing;
    else if (!strcmp(what, "recording")) *got = CORE_VIEW.recording;
    else if (!strcmp(what, "events")) *got = core_tape()->n[cur];
    else if (!strcmp(what, "tempo")) *got = pj.tempo;
    else if (!strcmp(what, "loop_on")) *got = pj.loop_on;
    else if (!strcmp(what, "reverse")) *got = pj.reverse;
    else if (!strcmp(what, "voices")) *got = CORE_VIEW.voices;
    else if (!strcmp(what, "sv_lim")) *got = CORE_VIEW.sv_lim;
    else if (!strcmp(what, "shed")) *got = (int)CORE_VIEW.shed;
    else if (!strcmp(what, "cores")) *got = cores_now();
    else if (!strcmp(what, "sim_overload")) { core_overload(); *got = 1; }   /* (test hooks: they act) */
    else if (!strcmp(what, "sim_one_core")) { sim_one_core = 1; *got = 1; }
    else if (!strcmp(what, "drums")) *got = CORE_VIEW.drums;
    else if (!strcmp(what, "loud")) *got = CORE_VIEW.out_peak > 0.01f;
    else if (!strcmp(what, "dirty")) *got = dirty;
    else if (!strcmp(what, "slot")) *got = slot_sel[mode_kind()];
    else if (!strcmp(what, "sampler")) *got = smp.state;
    else if (!strcmp(what, "sample_len")) *got = (int)(mode_kind() ? g_drum_sample.len : g_synth_sample.len);
    else if (!strcmp(what, "sampled_ds")) *got = (int)((mode_kind() ? g_drum_sample.len : g_synth_sample.len) / 2205u);
    else if (!strcmp(what, "clip")) *got = (int)CORE_VIEW.clip_n;
    else if (!strcmp(what, "balls")) *got = CORE_VIEW.nballs;
    else if (!strcmp(what, "loop_n")) *got = cur_seqdata(mode_kind())->loop_n;
    else if (!strcmp(what, "help")) *got = ui.help;
    else if (!strcmp(what, "path")) *got = ui.path;
    else if (!strcmp(what, "mfx_on")) *got = pj.mfx.on;
    else if (!strcmp(what, "mute")) *got = pj.t[cur].mute;
    else return 0;
    return 1;
}
#endif
