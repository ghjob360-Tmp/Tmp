/* SPDX-License-Identifier: GPL-3.0-only */
/* stepx.c: the STEP view's extras. ENV (a tap) turns the knobs' layer; the strip under the keys shows the four knobs.
 *   layer 1   KNOB 1 tempo   KNOB 2 length   KNOB 3 velocity   KNOB 4 ratchet (1..4 hits in the step)
 *   layer 2   KNOB 1 nudge   KNOB 2 chance   KNOB 3 slide      KNOB 4 chord (the step plays a chord)
 *   layer 3   KNOB 1 lock    KNOB 2 speed    KNOB 3 steps      KNOB 4 what a lock moves   (speed, steps, lock: the channel's)
 * The knobs work on the step whose key is held, else the one under the cursor. Keys held together write a chord.
 * SEL copies the channel's page; SEL held pastes it. Included by chipbox.c after stepview.c. */
static uint32_t ed_layer;
static struct { step_t st[SEQ_LEN]; uint8_t lock[SEQ_LEN], fl[SEQ_LEN], drum, full; } ed_clip;
static const char *const EDL[3][4] = {{"BPM", "LEN", "VEL", "RATCH"}, {"NUDGE", "CHANCE", "SLIDE", "CHORD"}, {"LOCK", "SPEED", "STEPS", "LK>"}};

static void ed_val(uint32_t c, uint32_t st, uint32_t l, uint32_t k, char *s)      /* the knob's value, as the strip shows it */
{
    const step_t *x = &pat[seq_view][c][st];
    const uint32_t fl = sx_fl(seq_view, c, st), dr = IS_DRUM(chan[c].kind);
    const char *w = 0;
    int32_t v = -1;
    if (l == 0u) { if (k == 0u) v = (int32_t)seq_bpm; else if (k == 1u && !dr) v = x->note ? ((int32_t)x->gate + 1) / 6 : -1; else if (k == 2u && !dr) v = x->note ? (int32_t)(x->vel & 127u) : -1; else if (k == 3u) v = (int32_t)ST_RAT(x); }
    else if (l == 1u) { if (k == 0u) v = (int32_t)ST_NUDGE(x); else if (k == 1u) w = PROB_NAME[ST_PROB(x)]; else if (k == 2u && !dr) w = fl & SXF_SLIDE ? "ON" : "OFF"; else if (k == 3u && !dr) w = STEP_CHORD_NAME[fl & SXF_CHORD]; }
    else { if (k == 0u && !dr) { if (fl & SXF_LOCK) v = (int32_t)sx_lock(seq_view, c, st); else w = "OFF"; } else if (k == 1u) w = SPD_NAME[chan[c].x.speed & 7u]; else if (k == 2u) v = (int32_t)ch_len(c); else if (k == 3u && !dr) w = LK_NAME[chan[c].x.lkt % LK_N]; }
    if (w) { uint32_t i = 0; while (w[i] && i < 6u) { s[i] = w[i]; i++; } s[i] = 0; }
    else if (v >= 0) fmt_u(s, (uint32_t)v);
    else { s[0] = '-'; s[1] = 0; }
}
static void ed_strip(uint32_t c, uint32_t st, const theme_t *t)       /* four cells: the knob's job and its value */
{
    uint32_t k;
    cv_begin(240, 34, R_BG);
    for (k = 0; k < 4u; k++) {
        char v[8];
        const int32_t x = (int32_t)k * 60;
        const char *lab = EDL[ed_layer][k];
        ed_val(c, st, ed_layer, k, v);
        cv_rect(x + 1, 0, 58, 34, mix16(R_BG, t->acc, 40));
        cv_rect(x + 1, 0, 58, 2, ed_layer == 0u ? t->acc : ed_layer == 1u ? RGB(80, 200, 255) : RGB(255, 200, 60));
        cv_text(x + 30 - text_w(&FONT_S, lab) / 2, 2, &FONT_S, lab, mix16(R_BG, R_WHITE, 150));
        cv_text(x + 30 - text_w(&FONT_S, v) / 2, 17, &FONT_S, v, R_WHITE);
    }
    cv_blit(0, 178);
}
static void ed_marks(uint32_t c, uint32_t i, int32_t x, int32_t y, uint16_t col)        /* a block's extras: ratchet dots, slide, chord, lock, chance */
{
    const step_t *s = &pat[seq_view][c][i];
    const uint32_t fl = sx_fl(seq_view, c, i), r = ST_RAT(s);
    uint32_t j;
    for (j = 1; j < r; j++) cv_rect(x + 20 - (int32_t)j * 5, y + 5, 3, 3, col);
    if (fl & SXF_SLIDE) { cv_rect(x + 3, y + 31, 6, 2, col); cv_rect(x + 8, y + 29, 2, 2, col); }
    if (fl & SXF_CHORD) { cv_rect(x + 12, y + 29, 5, 2, col); cv_rect(x + 13, y + 28, 2, 6, col); }
    if (fl & SXF_LOCK) cv_rect(x + 21, y + 29, 4, 4, col);
    if (ST_PROB(s)) cv_rect(x + 3, y + 5, 3, 3, RGB(255, 200, 60));
    if (ST_NUDGE(s)) cv_rect(x + 24, y + 12, 2, 10, col);
}
static void ed_knob(uint32_t c, uint32_t st, uint32_t k, int32_t e)
{
    step_t *x = &pat[seq_view][c][st];
    const uint32_t dr = IS_DRUM(chan[c].kind), fl = sx_fl(seq_view, c, st), lk = sx_lock(seq_view, c, st);
    const uint32_t has = dr ? dstep_hits(x) != 0u : x->note != 0u;
    if (ed_layer == 0u) {
        if (k == 1u && has && !dr) { int32_t n = ((int32_t)x->gate + 1) / 6 + e, mx = (int32_t)(SEQ_LEN - st); n = n < 1 ? 1 : n > mx ? mx : n; x->gate = (uint8_t)(n * 6 - 1); }
        else if (k == 2u && has && !dr) x->vel = (uint8_t)clamp_i((int32_t)(x->vel & 127u) + e * 4, 1, 127);
        else if (k == 3u && has) x->spare = (uint8_t)((x->spare & ~3u) | (uint32_t)clamp_i((int32_t)(x->spare & 3u) + e, 0, 3));
        else return;
    } else if (ed_layer == 1u) {
        if (k == 0u && has) x->spare = (uint8_t)((x->spare & 0x1Fu) | (uint32_t)clamp_i((int32_t)ST_NUDGE(x) + e, 0, 5) << 5);
        else if (k == 1u && has) x->spare = (uint8_t)((x->spare & 0xE3u) | (uint32_t)clamp_i((int32_t)ST_PROB(x) + e, 0, 7) << 2);
        else if (k == 2u && has && !dr) sx_put(seq_view, c, st, lk, e > 0 ? fl | SXF_SLIDE : fl & ~(uint32_t)SXF_SLIDE);
        else if (k == 3u && has && !dr) sx_put(seq_view, c, st, lk, (fl & ~(uint32_t)SXF_CHORD) | (uint32_t)clamp_i((int32_t)(fl & SXF_CHORD) + e, 0, 15));
        else return;
    } else {
        if (k == 0u && has && !dr) {                    /* the lock: OFF, then 0..127 */
            int32_t v = (fl & SXF_LOCK ? (int32_t)lk + 1 : 0) + e * 4;
            v = v < 0 ? 0 : v > 128 ? 128 : v;
            sx_put(seq_view, c, st, v ? (uint32_t)(v - 1) : 0u, v ? fl | SXF_LOCK : fl & ~(uint32_t)SXF_LOCK);
            if (v && !chan[c].x.lkt) chan[c].x.lkt = LK_CUT;
        }
        else if (k == 1u) chan[c].x.speed = (uint8_t)clamp_i((int32_t)chan[c].x.speed + e, 0, 7);
        else if (k == 2u) chan[c].x.len = (uint8_t)clamp_i((int32_t)ch_len(c) + e, 1, 16);
        else if (k == 3u && !dr) chan[c].x.lkt = (uint8_t)clamp_i((int32_t)chan[c].x.lkt + e, 0, LK_N - 1);
        else return;
    }
    proj_edited();
}
static uint32_t bits12(uint32_t m) { uint32_t n = 0; while (m) { n += m & 1u; m >>= 1; } return n; }
static uint32_t chord_match(const uint8_t *notes, uint32_t n, uint32_t *root)     /* keys held together: the lowest is the root, the rest pick the nearest chord */
{
    uint32_t i, s, best = 0, bs = 0, lo = 255u, want = 0;
    for (i = 0; i < n; i++) if (notes[i] < lo) lo = notes[i];
    for (i = 0; i < n; i++) want |= 1u << ((notes[i] - lo) % 12u);
    *root = lo;
    for (s = 1; s < 16u; s++) {
        const uint32_t a = STEP_CHORD[s] - 1u;
        uint32_t m = 0, j, sc;
        for (j = 0; j < ARP_N[a]; j++) m |= 1u << ((uint32_t)ARP_IV[a][j] % 12u);
        sc = bits12(m & want) * 4u - bits12(m ^ want);
        if (m == want) return s;
        if (sc > bs) { bs = sc; best = s; }
    }
    return best;
}
static void ed_copy(uint32_t c)
{
    uint32_t s;
    for (s = 0; s < SEQ_LEN; s++) { ed_clip.st[s] = pat[seq_view][c][s]; ed_clip.lock[s] = (uint8_t)sx_lock(seq_view, c, s); ed_clip.fl[s] = (uint8_t)sx_fl(seq_view, c, s); }
    ed_clip.drum = (uint8_t)IS_DRUM(chan[c].kind); ed_clip.full = 1;
}
static int ed_paste(uint32_t c)                  /* 0: pasted (notes onto notes, drums onto drums) */
{
    uint32_t s;
    if (!ed_clip.full || ed_clip.drum != IS_DRUM(chan[c].kind)) return -1;
    for (s = 0; s < SEQ_LEN; s++) { pat[seq_view][c][s] = ed_clip.st[s]; sx_put(seq_view, c, s, ed_clip.lock[s], ed_clip.fl[s]); }
    proj_edited();
    return 0;
}
