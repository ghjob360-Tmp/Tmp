/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the controls (included by app.c).
 *   SEL      OSC 1 > OSC 2 > OSC 3 > SAMPLE          (GLO: that one on / off)
 *   EDIT     FILTER 1 / FILTER 2                     (GLO: on / off)
 *   ENV      ENV 1 .. 6          LFO   LFO 1 .. 8    (GLO + LFO: RANDOM 1 .. 4)
 *   SEQ      MATRIX              FX    EFFECTS (again: the effect shown on / off)
 *   HOME     MACROS / VOICE      ARP   pick up this page's modulation source, turn any knob: its amount there
 *   PLAY     hold (sustain)      REC   all notes off (GLO + hold 1 s: INIT)
 *   SAVE     keep the preset (a factory one: into the next empty user slot; GLO: always a new slot; hold: help)
 *   OCT- / OCT+  the octave      (GLO: the page before / after)
 *   SELECT   the page's main choice: table, sample, filter model, LFO shape, random style, matrix slot, effect
 *            (GLO on EFFECTS: move the effect in the chain)
 *   ALGORITHM  the row           PRESET  the presets (factory, then user)
 *   K1..K4   the row's four (GLO: fine; with a source picked up: its modulation amount) */

static struct {
    int page, row[PG_N];
    int last_osc, last_flt, last_env, last_lfo, last_rnd, last_home;
    int glo, help, pick;                 /* pick: the source picked up (MS_*), 0 none */
    uint32_t pick_at, rec_t, save_t, keys, btn, init_done;
    char toast[2][24];
    uint32_t toast_until;
    int top_dirty, bot_dirty;
    uint32_t top_at, bot_at;
    int mac_sel, hold;
    uint32_t warn_at;
} ui;

void app_say(const char *a, const char *b)
{
    copy_text(ui.toast[0], a, sizeof ui.toast[0]);
    copy_text(ui.toast[1], b, sizeof ui.toast[1]);
    ui.toast_until = plat_ms() + 1300u;
    ui.top_dirty = 1;
}
static void changed(void)
{
    dirty = 1;
    set_dirty = 1;
    changed_at = plat_ms();
    patch_send();
    ui.top_dirty = ui.bot_dirty = 1;
}
static void go_page(int p)
{
    ui.page = p;
    if (ui.row[p] >= page_rows(p))
        ui.row[p] = 0;
    if (pg_osc(p) || p == PG_SMP) ui.last_osc = p;
    if (pg_flt(p)) ui.last_flt = p;
    if (pg_env(p)) ui.last_env = p;
    if (pg_lfo(p)) ui.last_lfo = p;
    if (pg_rnd(p)) ui.last_rnd = p;
    if (p == PG_MAC || p == PG_VOICE) ui.last_home = p;
    ui.top_dirty = ui.bot_dirty = 1;
}
static int page_source(int pg)
{
    if (pg == PG_MAC)
        return MS_MAC1 + ui.mac_sel;
    return pg_source(pg);
}

/* the matrix slot from src to dst, made if there is none (-1: the matrix is full) */
static int slot_for(int src, int dst)
{
    int i;
    for (i = 0; i < NSLOT; i++)
        if (pt.m[i].src == src && pt.m[i].dst == (uint16_t)dst)
            return i;
    return mslot_add(&pt, src, dst, 0);
}

/* turn control c by e clicks */
static void turn(int c, int32_t e, int knob)
{
    int glo = ui.glo, step;
    char t[24];
    static uint32_t turn_at[4];
    uint32_t now = plat_ms(), dt = now - turn_at[knob & 3];
    turn_at[knob & 3] = now;
    step = glo ? 2 : (e > 1 || e < -1 || dt < 30u) ? (dt < 15u ? 32 : 20) : 8;   /* fast turns go further */
    if (c >= 0 && ui.pick && (PDESC[c].flags & PF_MOD)) {   /* a picked-up source: its amount here */
        int s = slot_for(ui.pick, c);
        if (s < 0) {
            app_say("MATRIX", "FULL");
            return;
        }
        pt.m[s].amt = (int16_t)clamp(pt.m[s].amt + e * (glo ? 2 : 10), -1000, 1000);
        bip_text((float)pt.m[s].amt * 0.0005f + 0.5f, t);
        text_cat(t, "%", sizeof t);
        app_say(MS_NAME[ui.pick], t);
        ui.pick_at = now;
        changed();
        return;
    }
    if (c >= 0) {
        const pdesc_t *d = &PDESC[c];
        int n = 0;
        if (c >= P_X_CHO_ON) {
            int k = (c - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON), i = (c - P_X_CHO_ON) % (P_X_CMP_ON - P_X_CHO_ON);
            if (i > 0)
                n = fx_choice_n(k, i - 1);
        }
        if (d->kind == PK_CHOICE) {
            int v = (int)pt.v[c] + (e > 0 ? 1 : -1);
            pt.v[c] = (uint16_t)clamp(v, 0, d->n - 1);
            if (c == P_F1_MODEL || c == P_F2_MODEL)
                pt.v[c + 1] = 0;         /* a new model: its first style */
            if (c == P_F1_STYLE || c == P_F2_STYLE)
                pt.v[c] = (uint16_t)clamp(pt.v[c], 0, FLT_NSTYLE[pt.v[c - 1] % FLT_N] - 1);
        } else if (n > 0) {              /* an effect's choice: the middle of its step */
            int idx = clamp(sel_of(pt.v[c], n) + (e > 0 ? 1 : -1), 0, n - 1);
            pt.v[c] = (uint16_t)((idx * 1024 + 512) / n);
        } else {
            pt.v[c] = (uint16_t)clamp((int32_t)pt.v[c] + e * step, 0, 1023);
        }
        if (c >= P_MC1 && c <= P_MC4)
            ui.mac_sel = c - P_MC1;
        changed();
        return;
    }
    /* the controls that are not parameters */
    {
        mslot_t *m = &pt.m[ui_mslot];
        int lf = pg_lfo(ui.page) ? ui.page - PG_L1 : 0;
        switch (c) {
        case C_MSRC: m->src = (uint8_t)clamp(m->src + (e > 0 ? 1 : -1), 0, MS_N - 1);
            if (m->dst == 0xFFFFu) m->dst = P_F1_CUT;
            break;
        case C_MDST: {
            int i, k = e > 0 ? e : -e;
            int d = m->dst == 0xFFFFu ? P_F1_CUT : m->dst;
            if (k > 1 || dt < 30u)
                k = k * 3;
            for (i = 0; i < k; i++)
                d = mod_next(d, e > 0 ? 1 : -1);
            m->dst = (uint16_t)d;
            break;
        }
        case C_MAMT: m->amt = (int16_t)clamp(m->amt + e * (glo ? 2 : step), -1000, 1000); break;
        case C_MPOW: m->pow = (int16_t)clamp(m->pow + e * (glo ? 1 : 4), -100, 100); break;
        case C_MBIP: m->flags = (uint8_t)(e > 0 ? m->flags | 1u : m->flags & ~1u); break;
        case C_MBYP: m->flags = (uint8_t)(e > 0 ? m->flags | 2u : m->flags & ~2u); break;
        case C_DPT: ui_dpt[lf] = clamp(ui_dpt[lf] + (e > 0 ? 1 : -1), 0, LFO_PTS - 1); ui.top_dirty = ui.bot_dirty = 1; return;
        case C_DY: pt.lfo_y[lf][ui_dpt[lf]] = (uint8_t)clamp(pt.lfo_y[lf][ui_dpt[lf]] + e * (glo ? 1 : 6), 0, 255);
            pt.v[P_LFO(lf, SHAPE)] = LS_DRAWN;
            break;
        case C_DC: pt.lfo_c[lf][ui_dpt[lf]] = (int8_t)clamp(pt.lfo_c[lf][ui_dpt[lf]] + e * (glo ? 1 : 5), -100, 100);
            pt.v[P_LFO(lf, SHAPE)] = LS_DRAWN;
            break;
        case C_DCOPY: {                  /* the shape now, as points to draw on */
            int s = pt.v[P_LFO(lf, SHAPE)], i;
            if (s == LS_DRAWN)
                return;
            for (i = 0; i < LFO_PTS; i++) {
                pt.lfo_y[lf][i] = (uint8_t)(lfo_shape(s, pt.lfo_y[lf], pt.lfo_c[lf], (float)i / (float)LFO_PTS) * 255.0f + 0.5f);
                pt.lfo_c[lf][i] = 0;
            }
            pt.v[P_LFO(lf, SHAPE)] = LS_DRAWN;
            app_say("DRAWN", "FROM THE SHAPE");
            break;
        }
        case C_FXON: pt.v[P_FX(ui_fx_sel)] = e > 0 ? 1 : 0; break;
        case C_FXPOS: {
            int i, at = 0, to;
            for (i = 0; i < FXT_N; i++)
                if (pt.fx_order[i] == ui_fx_sel)
                    at = i;
            to = clamp(at + (e > 0 ? 1 : -1), 0, FXT_N - 1);
            pt.fx_order[at] = pt.fx_order[to];
            pt.fx_order[to] = (uint8_t)ui_fx_sel;
            break;
        }
        default: return;
        }
        changed();
    }
}

static void select_turn(int32_t e)       /* the page's main choice */
{
    int dir = e > 0 ? 1 : -1, pg = ui.page, id = -1;
    if (pg_osc(pg)) id = P_OSC(pg - PG_OSC1, TABLE);
    else if (pg == PG_SMP) id = P_S_SAMPLE;
    else if (pg_flt(pg)) id = P_FLT(pg - PG_F1, MODEL);
    else if (pg_lfo(pg)) id = P_LFO(pg - PG_L1, SHAPE);
    else if (pg_rnd(pg)) id = P_RND(pg - PG_R1, STYLE);
    else if (pg == PG_VOICE) id = P_V_POLY;
    if (id >= 0) {
        int n = id == P_O1_TABLE || id == P_O2_TABLE || id == P_O3_TABLE ? WT_NFACT
              : id == P_S_SAMPLE ? SMP_NFACT : PDESC[id].n;
        char t[24];
        pt.v[id] = (uint16_t)(((int)pt.v[id] + n + dir) % n);
        if (pg_flt(pg))
            pt.v[id + 1] = 0;
        param_text(id, pt.v[id], t);
        app_say(param_name(id), t);
        changed();
        return;
    }
    if (pg == PG_MAT) {
        ui_mslot = (ui_mslot + NSLOT + dir) % NSLOT;
    } else if (pg == PG_FX) {
        if (ui.glo) {
            turn(C_FXPOS, e, 0);
            return;
        }
        ui_fx_sel = (ui_fx_sel + FXT_N + dir) % FXT_N;
        if (ui.row[PG_FX] >= page_rows(PG_FX))
            ui.row[PG_FX] = page_rows(PG_FX) - 1;
    } else if (pg_env(pg)) {
        go_page(PG_E1 + (pg - PG_E1 + 6 + dir) % 6);
    } else if (pg == PG_MAC) {
        ui.mac_sel = (ui.mac_sel + 4 + dir) & 3;
    }
    ui.top_dirty = ui.bot_dirty = 1;
}

static void save_preset(int as_new)
{
    int u = cur_preset - NFACT, i;
    if (as_new || u < 0) {
        u = -1;
        for (i = 0; i < NUSER; i++)
            if (!user_get(i)) {
                u = i;
                break;
            }
        if (u < 0) {
            app_say("SAVE", "NO EMPTY SLOT");
            return;
        }
    }
    if (user_put(u, &pt) == 0) {
        char t[24];
        cur_preset = NFACT + u;
        dirty = 0;
        set_dirty = 1;
        copy_text(t, "USER ", sizeof t);
        num_text(u + 1, t + 5);
        app_say("SAVED", t);
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), maxp = 0;
    int glo = (int)((btn >> B_GLO) & 1u), i;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    if (glo != ui.glo) {
        ui.glo = glo;
        ui_shift = glo;
        ui.bot_dirty = ui.top_dirty = 1;
    }
    for (k = 0; k < NKEYS; k++) {        /* the keys: F3 .. G5, velocity and pressure */
        uint32_t bit = 1u << k;
        int note = 53 + (int)k + 12 * octave;
        if ((keys & bit) && touch_pressure(k) > maxp)
            maxp = touch_pressure(k);
        if (!(keys & bit) == !(ui.keys & bit))
            continue;
        if (keys & bit) {
            uint32_t v = touch_vel(k);
            core_key(note, (int)(v ? v : 100u));
        } else {
            core_key(note, 0);
        }
        ui.top_dirty = 1;
    }
    core_pressure((float)maxp * (1.0f / 127.0f));
    if (cc_in[0]) {                      /* MIDI CC 74 / 71: filter 1's cutoff and resonance */
        pt.v[P_F1_CUT] = (uint16_t)(cc_in[0] - 1u);
        cc_in[0] = 0;
        changed();
    }
    if (cc_in[1]) {
        pt.v[P_F1_RESO] = (uint16_t)(cc_in[1] - 1u);
        cc_in[1] = 0;
        changed();
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) && (ui.btn & bit) && k == B_SAVE) {   /* SAVE let go: a tap saves, a hold was help */
            if (!ui.help)
                save_preset(glo);
            ui.help = 0;
            ui.top_dirty = ui.bot_dirty = 1;
        }
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_SEL: {
            int p = ui.page;
            if (glo) {
                int id = p == PG_SMP ? P_S_ON : pg_osc(p) ? P_OSC(p - PG_OSC1, ON) : -1;
                if (id >= 0) {
                    pt.v[id] ^= 1u;
                    app_say(PG_NAME[p], ONOFF[pt.v[id] & 1u]);
                    changed();
                }
                break;
            }
            go_page(pg_osc(p) || p == PG_SMP ? (p == PG_SMP ? PG_OSC1 : p + 1) : ui.last_osc);
            break;
        }
        case B_EDIT:
            if (glo && pg_flt(ui.page)) {
                int id = P_FLT(ui.page - PG_F1, ON);
                pt.v[id] ^= 1u;
                app_say(PG_NAME[ui.page], ONOFF[pt.v[id] & 1u]);
                changed();
                break;
            }
            go_page(pg_flt(ui.page) ? (ui.page == PG_F1 ? PG_F2 : PG_F1) : ui.last_flt);
            break;
        case B_ENV: go_page(pg_env(ui.page) ? PG_E1 + (ui.page - PG_E1 + 1) % 6 : ui.last_env); break;
        case B_LFO:
            if (glo)
                go_page(pg_rnd(ui.page) ? PG_R1 + (ui.page - PG_R1 + 1) % 4 : ui.last_rnd);
            else
                go_page(pg_lfo(ui.page) ? PG_L1 + (ui.page - PG_L1 + 1) % 8 : ui.last_lfo);
            break;
        case B_SEQ: go_page(PG_MAT); break;
        case B_FX:
            if (ui.page == PG_FX) {
                pt.v[P_FX(ui_fx_sel)] ^= 1u;
                app_say(FXT_NAME[ui_fx_sel], ONOFF[pt.v[P_FX(ui_fx_sel)] & 1u]);
                changed();
            } else {
                go_page(PG_FX);
            }
            break;
        case B_HOME: go_page(ui.page == PG_MAC ? PG_VOICE : ui.page == PG_VOICE ? PG_MAC : ui.last_home); break;
        case B_ARP: {
            int s = page_source(ui.page);
            if (ui.pick) {
                ui.pick = 0;
                app_say("MODULATION", "PUT DOWN");
            } else if (s != MS_NONE) {
                ui.pick = s;
                ui.pick_at = now;
                app_say(MS_NAME[s], "TURN A KNOB");
            } else {
                app_say("PICK UP", "ON ENV LFO RANDOM MACRO");
            }
            ui.bot_dirty = 1;
            break;
        }
        case B_PLAY:
            ui.hold ^= 1;
            core_hold(ui.hold);
            app_say("HOLD", ONOFF[ui.hold]);
            break;
        case B_REC:
            ui.rec_t = now;
            ui.init_done = 0;
            if (!glo) {
                core_panic();
                ui.hold = 0;
                app_say("ALL NOTES", "OFF");
            }
            break;
        case B_SAVE: ui.save_t = now; break;
        case B_OCTDN:
        case B_OCTUP: {
            int dir = k == B_OCTUP ? 1 : -1;
            if (glo) {
                go_page((ui.page + PG_N + dir) % PG_N);
            } else {
                char t[8];
                octave = (int8_t)clamp(octave + dir, -3, 3);
                semis_text(octave, t);
                app_say("OCTAVE", t);
                set_dirty = 1;
                changed_at = now;
            }
            break;
        }
        default: break;
        }
    }
    if (((btn >> B_SAVE) & 1u) && now - ui.save_t > 600u && !ui.help) {
        ui.help = 1;                     /* SAVE held: help */
        ui.top_dirty = ui.bot_dirty = 1;
    }
    if (glo && ((btn >> B_REC) & 1u) && !ui.init_done && now - ui.rec_t > 1000u) {
        ui.init_done = 1;
        patch_default(&pt);
        changed();
        app_say("INIT", "A NEW SOUND");
    }
    if (ui.pick && now - ui.pick_at > 12000u) {   /* a source picked up and forgotten */
        ui.pick = 0;
        ui.bot_dirty = 1;
    }
    if ((e = plat_enc(EN_PRESET)) != 0 && dirty && now - ui.warn_at > 2500u) {
        ui.warn_at = now;                /* edits not saved: the first turn only warns */
        app_say("NOT SAVED", "TURN AGAIN TO LEAVE");
        e = 0;
    }
    if (e != 0) {
        int n = (cur_preset + NPRESET + (e > 0 ? 1 : -1)) % NPRESET;
        char t[24];
        if (n >= NFACT && !user_get(n - NFACT)) {   /* skip the empty user slots */
            int guard = NPRESET;
            while (guard-- > 0 && n >= NFACT && !user_get(n - NFACT))
                n = (n + NPRESET + (e > 0 ? 1 : -1)) % NPRESET;
        }
        preset_load(n);
        if (n < NFACT) {
            copy_text(t, "FACTORY ", sizeof t);
            num_text(n + 1, t + 8);
        } else {
            copy_text(t, "USER ", sizeof t);
            num_text(n - NFACT + 1, t + 5);
        }
        app_say(t, pt.name);
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int n = page_rows(ui.page);
        ui.row[ui.page] = (ui.row[ui.page] + n + (e > 0 ? 1 : -1)) % n;
        ui.top_dirty = ui.bot_dirty = 1;
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        select_turn(e);
    for (i = 0; i < 4; i++) {
        int c;
        if ((e = plat_enc(EN_K1 + i)) == 0)
            continue;
        knob_at[i] = now;
        c = ctl_at(ui.page, ui.row[ui.page], i);
        if (c != C_NONE)
            turn(c, e, i);
        ui.bot_dirty = ui.top_dirty = 1;
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.top_dirty = 1;
    ui.keys = keys;
    ui.btn = btn;
}
void app_input_only(void) { input(); }
