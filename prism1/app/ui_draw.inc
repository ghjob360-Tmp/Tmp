/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the frame around the pictures: the header, the knobs, the lights, the pace (included by app.c). */

static void draw_header(void)
{
    char t[24];
    int pg = ui.page, i;
    cv_rect(0, 0, 240, 19, PANEL);
    /* the page and its row */
    cv_text(4, 3, &FONT_T, PG_NAME[pg], ui.pick && page_source(pg) == ui.pick ? PURP : TEAL);
    {
        int n = page_rows(pg), r = ui.row[pg];
        for (i = 0; i < n; i++)
            disc(70 + i * 6, 10, i == r ? 2 : 1, i == r ? WHITE : G2);
    }
    /* the preset: < NAME > (* edited) */
    copy_text(t, pt.name, sizeof t);
    if (dirty)
        text_cat(t, "*", sizeof t);
    ctext(150, 3, &FONT_T, t, WHITE);
    cv_text(100, 3, &FONT_T, "<", G2);
    cv_text(196, 3, &FONT_T, ">", G2);
    /* the output level */
    {
        float pk = CORE_VIEW.out_peak;
        int32_t h = (int32_t)(clampf(pk, 0.0f, 1.0f) * 14.0f);
        cv_rect(232, 3, 4, 14, G1);
        cv_rect(232, 17 - h, 4, h, pk > 0.97f ? REDC : TEAL);
    }
    /* voices in use */
    for (i = 0; i < NVOICE; i++)
        cv_rect(206 + (i & 3) * 6, 4 + (i >> 2) * 7, 4, 5, (CORE_VIEW.vmask >> i) & 1u ? TEAL : G1);
}

static void draw_toast(void)
{
    int32_t w;
    if (plat_ms() >= ui.toast_until)
        return;
    w = text_w(&FONT_T, ui.toast[0]) + text_w(&FONT_T, ui.toast[1]) + 24;
    if (w > 236)
        w = 236;
    cv_rect(120 - w / 2, 24, w, 17, PANEL);
    frame_rect(120 - w / 2, 24, w, 17, ui.pick ? PURP : TEAL_D);
    cv_text(120 - w / 2 + 8, 28, &FONT_T, ui.toast[0], ui.pick ? PURP : TEAL);
    rtext(120 + w / 2 - 8, 28, &FONT_T, ui.toast[1], WHITE);
}

static void draw_help(void)
{
    static const char *const H[] = {"SEL OSC 1-3 SAMPLE  EDIT FILTERS", "ENV ENVS  LFO LFOS  GLO+LFO RANDOM",
                                    "SEQ MATRIX  FX EFFECTS  HOME MACRO/VOICE", "SELECT TABLE MODEL SHAPE SLOT",
                                    "ALGO ROW  PRESET PRESETS", "ARP PICK UP A SOURCE, TURN A KNOB",
                                    "PLAY HOLD  REC NOTES OFF", "SAVE KEEP  GLO+SAVE NEW SLOT"};
    int i;
    cv_rect(0, 20, 240, 124, BG);
    for (i = 0; i < 8; i++)
        cv_text(6, 24 + i * 14, &FONT_T, H[i], i & 1 ? G3 : WHITE);
}

static void draw_top(void)
{
    cv_begin(240, 144, BG);
    draw_header();
    if (ui.help)
        draw_help();
    else
        draw_picture();
    draw_toast();
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    char t[24];
    int i, pg = ui.page, r = ui.row[pg];
    cv_begin(240, 96, BG);
    cv_rect(0, 0, 240, 1, G1);
    if (ui.help) {
        ctext(120, 30, &FONT_XS, "LET GO OF SAVE TO CLOSE", G3);
        cv_blit(0, 144);
        return;
    }
    for (i = 0; i < 4; i++) {
        int c = ctl_at(pg, r, i);
        if (c == C_NONE) {
            dial(i, 0.0f, 0, 0, 0, -1.0f, 0, 0, 0);
            continue;
        }
        if (c >= 0) {
            const pdesc_t *d = &PDESC[c];
            float v, lo, hi, live = -1.0f;
            int n = 0, pick = ui.pick && (d->flags & PF_MOD);
            if (c >= P_X_CHO_ON) {
                int kk = (c - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON), ii = (c - P_X_CHO_ON) % (P_X_CMP_ON - P_X_CHO_ON);
                if (ii > 0)
                    n = fx_choice_n(kk, ii - 1);
            }
            param_text(c, pt.v[c], t);
            if (pick) {                  /* picked up: the amount from that source shows instead */
                int s;
                for (s = 0; s < NSLOT; s++)
                    if (pt.m[s].src == ui.pick && pt.m[s].dst == (uint16_t)c)
                        break;
                if (s < NSLOT) {
                    bip_text((float)pt.m[s].amt * 0.0005f + 0.5f, t);
                    text_cat(t, "%", sizeof t);
                } else {
                    copy_text(t, "+0%", sizeof t);
                }
            }
            if (d->kind == PK_CHOICE) {
                v = d->n > 1 ? (float)pt.v[c] / (float)(d->n - 1) : 0.0f;
                if (c == P_O1_TABLE || c == P_O2_TABLE || c == P_O3_TABLE)
                    v = (float)pt.v[c] / (float)(WT_NFACT > 1 ? WT_NFACT - 1 : 1);
                lo = hi = 0.0f;
            } else {
                v = (float)pt.v[c] / 1023.0f;
                mod_reach(c, &lo, &hi);
                if (hi > lo && CORE_VIEW.active)
                    live = CORE_VIEW.live[c];
            }
            (void)n;
            dial(i, v, d->kind == PK_BIP, lo, hi, live, param_name(c), t, pick);
        } else {                         /* the other controls */
            const mslot_t *m = &pt.m[ui_mslot];
            int lf = pg_lfo(pg) ? pg - PG_L1 : 0;
            float v = 0.0f;
            const char *lb = "";
            int bip = 0;
            t[0] = 0;
            switch (c) {
            case C_MSRC: lb = "SOURCE"; copy_text(t, MS_NAME[m->src % MS_N], sizeof t); v = (float)m->src / (MS_N - 1); break;
            case C_MDST: lb = "DEST";
                if (m->dst < NPARAM) copy_text(t, param_name(m->dst), sizeof t); else copy_text(t, "-", sizeof t);
                v = m->dst < NPARAM ? (float)m->dst / (float)(NPARAM - 1) : 0.0f;
                break;
            case C_MAMT: lb = "AMOUNT"; v = (float)m->amt * 0.0005f + 0.5f; bip_text(v, t); text_cat(t, "%", sizeof t); bip = 1; break;
            case C_MPOW: lb = "POWER"; v = (float)m->pow * 0.005f + 0.5f; dec1_text((float)m->pow * 0.1f, t); bip = 1; break;
            case C_MBIP: lb = "BIPOLAR"; v = (float)(m->flags & 1u); copy_text(t, ONOFF[m->flags & 1u], sizeof t); break;
            case C_MBYP: lb = "BYPASS"; v = (float)((m->flags >> 1) & 1u); copy_text(t, ONOFF[(m->flags >> 1) & 1u], sizeof t); break;
            case C_DPT: lb = "POINT"; v = (float)ui_dpt[lf] / (LFO_PTS - 1); num_text(ui_dpt[lf] + 1, t); break;
            case C_DY: lb = "HEIGHT"; v = (float)pt.lfo_y[lf][ui_dpt[lf]] / 255.0f; pct_text(v, t); break;
            case C_DC: lb = "CURVE"; v = (float)pt.lfo_c[lf][ui_dpt[lf]] * 0.005f + 0.5f; bip_text(v, t); bip = 1; break;
            case C_DCOPY: lb = "FROM SHAPE"; copy_text(t, "TURN", sizeof t); break;
            case C_FXON: lb = "ON"; v = (float)(pt.v[P_FX(ui_fx_sel)] & 1u); copy_text(t, ONOFF[pt.v[P_FX(ui_fx_sel)] & 1u], sizeof t); break;
            case C_FXPOS: {
                int q, at = 0;
                for (q = 0; q < FXT_N; q++)
                    if (pt.fx_order[q] == ui_fx_sel)
                        at = q;
                lb = "POSITION";
                v = (float)at / (FXT_N - 1);
                num_text(at + 1, t);
                break;
            }
            default: break;
            }
            dial(i, v, bip, 0, 0, -1.0f, lb, t, 0);
        }
    }
    {                                    /* the row's name, the GLO hint, the source held */
        char rn[32];
        copy_text(rn, row_name(pg, r), sizeof rn);
        if (pg == PG_MAT) {
            char n[8];
            text_cat(rn, " ", sizeof rn);
            num_text(ui_mslot + 1, n);
            text_cat(rn, n, sizeof rn);
        }
        if (pg == PG_FX) {
            text_cat(rn, "  ", sizeof rn);
            text_cat(rn, FXT_NAME[ui_fx_sel], sizeof rn);
        }
        cv_text(4, 86, &FONT_T, rn, G2);
        if (ui.pick) {
            cv_rect(120, 84, 118, 11, PURP_D);
            ctext(179, 85, &FONT_T, MS_NAME[ui.pick], PURP);
        } else if (ui.glo) {
            rtext(236, 86, &FONT_T, "FINE", G2);
        }
    }
    cv_blit(0, 144);
}

static void lights(void)
{
    uint32_t b = ui.btn, k = ui.keys, i;
    int pg = ui.page;
    if (pg_osc(pg) || pg == PG_SMP) b |= 1u << B_SEL;
    if (pg_flt(pg)) b |= 1u << B_EDIT;
    if (pg_env(pg)) b |= 1u << B_ENV;
    if (pg_lfo(pg) || pg_rnd(pg)) b |= 1u << B_LFO;
    if (pg == PG_MAT) b |= 1u << B_SEQ;
    if (pg == PG_FX) b |= 1u << B_FX;
    if (pg == PG_MAC || pg == PG_VOICE) b |= 1u << B_HOME;
    if (ui.pick ? (ui_now / 200u) & 1u : page_source(pg) != MS_NONE) b |= 1u << B_ARP;
    if (ui.hold) b |= 1u << B_PLAY;
    if (dirty) b |= 1u << B_SAVE;
    for (i = 0; i < NVOICE; i++)         /* the keys of the voices sounding */
        if ((CORE_VIEW.vmask >> i) & 1u) {
            int key = (int)CORE_VIEW.vnote[i] - 53 - 12 * octave;
            if (key >= 0 && key < NKEYS)
                k |= 1u << key;
        }
    plat_glow(1);
    plat_leds(b, k);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, BG);
    ui.last_osc = PG_OSC1;
    ui.last_flt = PG_F1;
    ui.last_env = PG_E1;
    ui.last_lfo = PG_L1;
    ui.last_rnd = PG_R1;
    ui.last_home = PG_MAC;
    ui.top_dirty = ui.bot_dirty = 1;
}

void app_frame(void)
{
    int alive, t;
    static uint32_t cpu_at;
    ui_now = plat_ms();
    ui_t = (float)ui_now * 0.001f;
    input();
    tables_service();
    if (set_dirty && ui_now - changed_at > 3000u)   /* the edits kept, a while after the last */
        settings_save();
    if (ui_now - cpu_at > 1000u) {
        cpu_at = ui_now;
        core_cpu((float)plat_cpu_pct() * 0.01f);
    }
    alive = CORE_VIEW.active || CORE_VIEW.out_peak > 0.002f || ui_now < ui.toast_until + 100u;
    for (t = 0; t < 4; t++)
        if (knob_at[t] && ui_now - knob_at[t] < 800u) {
            alive = 1;
            if (ui_now - knob_at[t] > 700u)
                ui.bot_dirty = 1;
        }
    CORE_VIEW.out_peak *= 0.85f;
    /* the picture ~30 / s while anything moves, ~6 idle; the knobs on change, ~10 / s while a voice moves them */
    if (ui.top_dirty || ui_now - ui.top_at >= (alive ? 33u : 160u)) {
        ui.top_at = ui_now;
        ui.top_dirty = 0;
        draw_top();
    }
    if (ui.bot_dirty || (CORE_VIEW.active && ui_now - ui.bot_at >= 100u)) {
        ui.bot_at = ui_now;
        ui.bot_dirty = 0;
        draw_bottom();
    }
    lights();
}

#ifdef PLAT_HOST
#include <string.h>
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "row")) *got = ui.row[ui.page];
    else if (!strcmp(what, "voices")) *got = CORE_VIEW.voices;
    else if (!strcmp(what, "loud")) *got = CORE_VIEW.out_peak > 0.01f;
    else if (!strcmp(what, "preset")) *got = cur_preset;
    else if (!strcmp(what, "dirty")) *got = dirty;
    else if (!strcmp(what, "pick")) *got = ui.pick;
    else if (!strcmp(what, "slots")) {
        int i, n = 0;
        for (i = 0; i < NSLOT; i++)
            n += pt.m[i].src != MS_NONE && pt.m[i].dst != 0xFFFFu;
        *got = n;
    } else if (!strcmp(what, "fx_sel")) *got = ui_fx_sel;
    else if (!strcmp(what, "fx_first")) *got = pt.fx_order[0];
    else if (!strcmp(what, "table")) *got = pt.v[P_O1_TABLE];
    else if (!strcmp(what, "tables_ready")) *got = WT.building < 0;
    else if (!strcmp(what, "budget")) *got = CORE_VIEW.budget;
    else if (!strcmp(what, "hold")) *got = ui.hold;
    else if (!strcmp(what, "nuser")) *got = NUSER;
    else return 0;
    return 1;
}
#endif
