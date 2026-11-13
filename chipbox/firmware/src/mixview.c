/* SPDX-License-Identifier: GPL-3.0-only */
/* mixview.c: the MIX view, the fourth one (HOME after SONG): the project's master settings, a page of four knobs each,
 * ALGORITHM turns the page. GROOVE (swing, the keys' scale and key), CHORUS, DELAY, REVERB, MASTER (compressor, saturation,
 * the sidechain's release and shape). The channels send to the effects from their SENDS page. Included by chipbox.c after chipview.c. */
static uint32_t mix_page;
static const theme_t MIX_T = {"MIX", {"", "", ""}, RGB(18, 10, 30), RGB(30, 16, 46), RGB(244, 236, 255), RGB(255, 70, 170), RGB(110, 80, 140), RGB(244, 236, 255), RGB(60, 220, 255)};
static void mix_panel(void)
{
    const theme_t *t = &MIX_T;
    const epg_t *e = &MPG[mix_page];
    int32_t i;
    cv_begin(240, CV_ENV_H + 30, t->bg);
    cv_rect(3, 0, 234, CV_ENV_H + 30, t->bar);
    for (i = 0; i < (int32_t)MPGS; i++) cv_rect(170 + i * 12, 8, 8, 8, (uint32_t)i == mix_page ? t->acc : t->dim);
    cv_text(12, 6, &FONT_L, e->name, t->fg);
    cv_text(12, 42, &FONT_S, e->hint, t->dim);
    if (mix_page == 0u) {                       /* the swing: eight sixteenths, the odd ones late */
        for (i = 0; i < 8; i++) { const int32_t x = 20 + i * 26 + (i & 1 ? (int32_t)mset.swing * 26 / 25 / 2 : 0); cv_rect(x, 66, 6, 30, t->acc); }
    } else if (mix_page == 2u) {                /* the echoes, each quieter (ping-pong: up and down) */
        for (i = 0; i < 7; i++) { int32_t h = 34 - i * (40 - (int32_t)mset.dfb * 2) / 8; if (h < 2) break; cv_rect(20 + i * 30, mset.dmode && (i & 1) ? 66 : 100 - h, 10, h, mix16(t->bar, t->acc, 255u - (uint32_t)i * 25u)); }
    } else if (mix_page == 3u) {                /* the tail; gated: cut short */
        const int32_t len = mset.rgate ? 20 + (int32_t)mset.rgate * 8 : 60 + (int32_t)mset.rsize * 8;
        for (i = 0; i < 200 && i < len + 4; i++) { int32_t h = mset.rgate ? (i < len ? 30 : 0) : 30 - i * 30 / len; if (h > 0) cv_rect(20 + i, 100 - h, 1, h, t->acc); }
    } else if (mix_page == 4u) {                /* the compressor's curve: in across, out up */
        const int32_t th = 80 - (int32_t)mset.comp * 4;
        for (i = 0; i < 100; i++) { int32_t o = i < th ? i : th + (i - th) / 4; o = o * (256 + (int32_t)mset.comp * 14) / 256; if (o > 100) o = 100; cv_rect(30 + i * 18 / 10, 100 - o * 36 / 100, 2, 2, t->acc); }
    }
    cv_blit(0, 44);
}
static void mix_update(uint32_t force)
{
    static uint32_t sig;
    uint32_t h = mix_page, i;
    for (i = 0; i < MSET_BYTES; i++) h = h * 31u + ((const uint8_t *)&mset)[i];
    if (!force && h == sig) return;
    sig = h;
    if (force) view_clear(MIX_T.bg);
    mix_panel();
    ep_cards_b(&MIX_T, 99u, &MPG[mix_page], (const uint8_t *)&mset);
}
static void mix_knob(uint32_t k, int32_t d)
{
    const efd_t *f = &MPG[mix_page].f[k];
    if (f->lab[0]) ep_setb((uint8_t *)&mset, f, d);
}
