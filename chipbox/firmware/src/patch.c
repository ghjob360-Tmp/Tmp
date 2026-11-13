/* SPDX-License-Identifier: GPL-3.0-only */
/* patch.c: patches you keep across projects, and the factory's synthwave ones. Hold SEL on the chip screen: the list.
 * SELECT moves; OCT+ puts the patch on the channel (its console too); SAVE writes the channel into a USER slot; OCT- or HOME closes.
 * The keys play the channel while the list is up. The user slots live in one 4 KB flash sector after the projects
 * (PATCH_AT): 31 slots of 128 bytes. A save erases the sector (~50 ms: the sound drops out). Included by chipbox.c. */
#define PATCH_SLOT 128u
#define PATCH_N 31u
#define PATCH_MAGIC 0x50u                       /* 'P' */
enum { PR_SAWLEAD, PR_JUNOPAD, PR_BASSPL, PR_BRASS, PR_ARPPLS, PR_PWMSTR, PR_SNSTR, PR_SNCHOIR, PR_SNEP, PR_ORCH, PR_AMIBASS, PR_NESARP, PRESETS };
static const char *const PRESET_NAME[PRESETS] = {"Supersaw Lead", "Juno Pad", "Pluck Bass", "Brass Stab", "Arp Pulse", "PWM Strings", "SNES Strings", "SNES Choir", "SNES E.Piano", "Orch Hit", "Amiga Bass", "NES Arp Lead"};
static uint32_t patch_on, patch_sel, patch_top, patch_dirty, patch_msg_t;
static const char *patch_msg;
static uint8_t patch_kind[PATCH_N];             /* the user slots' consoles (255: empty), read when the list opens */

static void patch_pack(uint32_t c, uint8_t *p)  /* the channel's sound into a slot's 128 bytes */
{
    const inst_t *x = &chan[c].in;
    uint32_t i, sum = 0;
    memset(p, 0, PATCH_SLOT);
    p[0] = PATCH_MAGIC; p[1] = chan[c].kind;
    p[2] = x->wave; p[3] = x->duty; p[4] = x->atk; p[5] = x->dec; p[6] = x->sus; p[7] = x->rel; p[8] = x->vib_spd; p[9] = x->vib_dep;
    p[10] = (uint8_t)x->sweep; p[11] = x->pan; p[12] = x->vol; p[13] = x->arp; p[14] = x->arp_rate; p[15] = x->arp_dir; p[16] = x->wf; p[17] = x->wc; p[18] = x->wt;
    memcpy(p + 19, &chan[c].x, EXT_BYTES);
    p[57] = x->duck;
    if (fm_plen(chan[c].kind)) { p[58] = 1; fm_pack(chan[c].kind, &chan[c].fm, p + 60); }
    else if (chan[c].wcus && wd_ok(chan[c].kind)) { p[58] = 2; wd_pack(c, p + 60); }
    for (i = 0; i < PATCH_SLOT - 1u; i++) sum += p[i];
    p[PATCH_SLOT - 1u] = (uint8_t)(sum ^ 0xA5u);
}
static int patch_ok(const uint8_t *p)
{
    uint32_t i, sum = 0;
    if (p[0] != PATCH_MAGIC || p[1] >= KINDS || IS_DRUM(p[1])) return 0;
    for (i = 0; i < PATCH_SLOT - 1u; i++) sum += p[i];
    return p[PATCH_SLOT - 1u] == (uint8_t)(sum ^ 0xA5u);
}
static void patch_onto(uint32_t c, uint32_t kind)       /* the channel becomes this console (its notes stay; drums are not patches) */
{
    if (chan[c].kind != kind && !IS_DRUM(chan[c].kind)) { seq_stop(); chan_set_kind(c, kind); }
}
static void patch_unpack(const uint8_t *p, uint32_t c)
{
    inst_t *x = &chan[c].in;
    patch_onto(c, p[1]);
    if (chan[c].kind != p[1]) return;
    chan_lock = 1;
    x->wave = p[2]; x->duty = p[3]; x->atk = p[4]; x->dec = p[5]; x->sus = p[6]; x->rel = p[7]; x->vib_spd = p[8]; x->vib_dep = p[9];
    x->sweep = (int8_t)p[10]; x->pan = p[11]; x->vol = p[12]; x->arp = p[13]; x->arp_rate = p[14]; x->arp_dir = p[15]; x->wf = p[16]; x->wc = p[17]; x->wt = p[18];
    {   const uint8_t sp = chan[c].x.speed, ln = chan[c].x.len, lk = chan[c].x.lkt, cv = chan[c].x.chv;     /* how the channel's steps run is not the sound: it stays */
        memcpy(&chan[c].x, p + 19, EXT_BYTES);
        chan[c].x.speed = sp; chan[c].x.len = ln; chan[c].x.lkt = lk; chan[c].x.chv = cv; }
    x->duck = p[57];
    chan[c].wcus = 0;
    if (p[58] == 1u && fm_plen(p[1])) fm_unpack(p[1], &chan[c].fm, p + 60);
    else if (p[58] == 2u && wd_ok(p[1])) wd_unpack(c, p + 60);
    wkey[c] = 0;
    chan_lock = 0;
    if (p[1] == KD_SNES || p[1] == KD_AMIGA) bank_make();
    live_dirty[c] = 1;
}
static void preset_apply(uint32_t i, uint32_t c)        /* a factory patch: a console and the settings that make it */
{
    static const uint8_t KIND[PRESETS] = {KD_VA, KD_VA, KD_VA, KD_VA, KD_VA, KD_VA, KD_SNES, KD_SNES, KD_SNES, KD_SNES, KD_AMIGA, KD_NES_PU};
    inst_t *in;
    ext_t *x;
    uint8_t sp, ln, lk, cv;
    patch_onto(c, KIND[i]);
    if (chan[c].kind != KIND[i]) return;
    chan_lock = 1;
    in = &chan[c].in; x = &chan[c].x;
    sp = x->speed; ln = x->len; lk = x->lkt; cv = x->chv;
    *in = INST_DEF[KIND[i]]; in->arp_rate = X_RATE_DEF;
    memset(x, 0, sizeof *x); ext_default(KIND[i], x);
    x->speed = sp; x->len = ln; x->lkt = lk; x->chv = cv;
    switch (i) {
    case PR_SAWLEAD: x->va[VA_UNI] = 7; x->va[VA_DET] = 7; x->va[VA_SPR] = 12; x->va[VA_SUB] = 3; x->ftype = 1; x->fcut = 48; x->fres = 3; x->glide = 2;
        x->scho = 5; x->sdly = 8; x->srev = 5; in->atk = 0; in->dec = 6; in->sus = 12; in->rel = 5; in->vib_dep = 2; in->vib_spd = 6; break;
    case PR_JUNOPAD: x->va[VA_POLY] = 4; x->va[VA_UNI] = 2; x->va[VA_DET] = 4; x->va[VA_W2] = 1; x->va[VA_MIX] = 8; x->va[VA_PW] = 6; x->va[VA_SUB] = 0;
        x->ftype = 1; x->fcut = 30; x->fres = 2; x->fenv = 4; x->fdec = 11; x->ltgt = LT_CUT; x->lrate = 2; x->ldep = 3;
        x->scho = 12; x->srev = 10; in->atk = 9; in->dec = 8; in->sus = 12; in->rel = 11; in->vol = 11; break;
    case PR_BASSPL: x->va[VA_UNI] = 1; x->va[VA_SUB] = 10; x->ftype = 4; x->fcut = 12; x->fres = 5; x->fenv = 10; x->fdec = 5; x->drive = 4;
        in->atk = 0; in->dec = 5; in->sus = 6; in->rel = 2; in->duck = 8; break;
    case PR_BRASS: x->va[VA_POLY] = 4; x->va[VA_UNI] = 1; x->va[VA_W2] = 0; x->va[VA_MIX] = 12; x->va[VA_FINE] = 6; x->va[VA_SUB] = 0;
        x->ftype = 1; x->fcut = 24; x->fres = 1; x->fenv = 8; x->fdec = 6; x->srev = 5; in->atk = 1; in->dec = 6; in->sus = 10; in->rel = 4; break;
    case PR_ARPPLS: x->va[VA_W1] = 1; x->va[VA_PW] = 6; x->va[VA_UNI] = 1; x->va[VA_SUB] = 0; x->ftype = 1; x->fcut = 38; x->fenv = 5; x->fdec = 4;
        in->arp = 5; in->arp_rate = AR_SYNC + 3; x->aoct = 1; x->agate = 4; x->sdly = 9; in->atk = 0; in->dec = 4; in->sus = 8; in->rel = 3; break;
    case PR_PWMSTR: x->va[VA_W1] = 1; x->va[VA_PW] = 3; x->va[VA_POLY] = 4; x->va[VA_UNI] = 2; x->va[VA_DET] = 3; x->va[VA_SUB] = 0;
        x->ltgt = LT_PWM; x->lrate = 3; x->ldep = 9; x->ftype = 1; x->fcut = 36; x->scho = 10; x->srev = 8; in->atk = 7; in->dec = 8; in->sus = 13; in->rel = 9; break;
    case PR_SNSTR: x->va[SM_SAMPLE] = 2; x->va[VA_POLY] = 4; x->scho = 6; x->srev = 9; in->atk = 6; in->sus = 13; in->rel = 9; break;
    case PR_SNCHOIR: x->va[SM_SAMPLE] = 3; x->va[VA_POLY] = 3; x->srev = 11; in->atk = 7; in->sus = 13; in->rel = 10; in->vib_dep = 2; in->vib_spd = 5; break;
    case PR_SNEP: x->va[SM_SAMPLE] = 5; x->va[VA_POLY] = 4; x->scho = 9; x->srev = 5; in->atk = 0; in->dec = 9; in->sus = 7; in->rel = 6; break;
    case PR_ORCH: x->va[SM_SAMPLE] = 12; x->srev = 10; in->atk = 0; in->sus = 15; in->rel = 8; break;
    case PR_AMIBASS: x->va[SM_SAMPLE] = 13; x->va[SM_SEMI] = 0; x->va[SM_LED] = 1; in->atk = 0; in->dec = 7; in->sus = 9; in->rel = 3; in->duck = 6; break;
    case PR_NESARP: in->duty = 1; in->arp = 2; in->arp_rate = AR_SYNC + 5; x->aoct = 1; x->sdly = 6; in->vib_dep = 0; break;
    default: break;
    }
    chan_lock = 0;
    if (KIND[i] == KD_SNES || KIND[i] == KD_AMIGA) bank_make();
    live_dirty[c] = 1;
}
static void patch_scan(void)
{
    uint32_t i;
    uint8_t p[PATCH_SLOT];
    for (i = 0; i < PATCH_N; i++) patch_kind[i] = pf_read(PATCH_AT + i * PATCH_SLOT, p, PATCH_SLOT) == 0 && patch_ok(p) ? p[1] : 255u;
}
static int patch_save(uint32_t slot, uint32_t c)        /* the sector read, the slot changed, erased, written back */
{
    uint8_t *sec = proj_buf;                    /* (12 KB of scratch that only a project save uses) */
    if (slot >= PATCH_N || pf_read(PATCH_AT, sec, 0x1000u)) return -1;
    patch_pack(c, sec + slot * PATCH_SLOT);
    if (pf_erase(PATCH_AT) || pf_write(PATCH_AT, sec, 0x1000u)) return -1;
    patch_scan();
    return 0;
}
static uint32_t patch_rows(void) { return PRESETS + PATCH_N; }
static void patch_open(void) { patch_scan(); patch_on = 1; patch_dirty = 1; patch_msg = 0; if (patch_sel >= patch_rows()) patch_sel = 0; }
static void patch_draw(void)
{
    const int32_t w = 232, h = 200, x = 4, y0 = 34, rows = 7;
    const uint16_t col = RGB(255, 70, 170);
    uint32_t b, i;
    if (patch_sel < patch_top) patch_top = patch_sel;
    if (patch_sel >= patch_top + (uint32_t)rows) patch_top = patch_sel + 1u - (uint32_t)rows;
    win_shadow(x, y0, w, h);
    for (b = 0; b * 80u < (uint32_t)h; b++) {
        win_band(b, w, h);
        win_chrome(w, h, "Patches", col);
        for (i = 0; i < (uint32_t)rows; i++) {
            const uint32_t r = patch_top + i;
            const int32_t y = 29 + (int32_t)i * 21;
            char s[40]; uint32_t n = 0; const char *a;
            if (r >= patch_rows()) break;
            if (r < PRESETS) { a = PRESET_NAME[r]; while (*a && n < 30u) s[n++] = *a++; }
            else {
                const uint32_t u = r - PRESETS;
                s[n++] = 'U'; s[n++] = 's'; s[n++] = 'e'; s[n++] = 'r'; s[n++] = ' ';
                if (u + 1u >= 10u) s[n++] = (char)('0' + (u + 1u) / 10u);
                s[n++] = (char)('0' + (u + 1u) % 10u); s[n++] = ':'; s[n++] = ' ';
                a = patch_kind[u] == 255u ? "empty" : ADD_NAME[KIND_CHIP[patch_kind[u]]];
                while (*a && n < 30u) s[n++] = *a++;
            }
            s[n] = 0;
            if (r == patch_sel) { mR(4, y - 3, w - 8, 19, R_SEL); mR(4, y - 3, 3, 19, col); }
            mT(14, y, s, r == patch_sel ? R_WHITE : r < PRESETS ? RGB(255, 180, 220) : RGB(200, 200, 210));
        }
        win_foot(w, h, patch_msg && fm1_ms - patch_msg_t < 1500u ? patch_msg : patch_sel < PRESETS ? "OCT+  Load" : "OCT+ Load SAVE");
        cv_blit(x, y0 + b * 80u);
    }
}
static void patch_input(uint32_t b, uint32_t home_tap, uint32_t c)     /* the list is up: everything but the keys comes here */
{
    int32_t e = fm1_enc_take(ENC_SELECT), k;
    if (!e) e = fm1_enc_take(ENC_PRESETS);
    if (e) { patch_sel = (uint32_t)clamp_i((int32_t)patch_sel + (e < 0 ? -1 : 1), 0, (int32_t)patch_rows() - 1); patch_dirty = 1; }
    fm1_enc_take(ENC_ALGORITHM);
    for (k = 0; k < 4; k++) fm1_enc_take(ENC_KNOB1 + (uint32_t)k);
    if ((b & (1u << BTN_OCT_UP)) && c < chan_n) {
        if (patch_sel < PRESETS) { preset_apply(patch_sel, c); patch_msg = "Loaded"; }
        else {
            uint8_t p[PATCH_SLOT];
            const uint32_t u = patch_sel - PRESETS;
            if (pf_read(PATCH_AT + u * PATCH_SLOT, p, PATCH_SLOT) == 0 && patch_ok(p)) { patch_unpack(p, c); patch_msg = "Loaded"; } else patch_msg = "Empty slot";
        }
        patch_msg_t = fm1_ms; patch_dirty = 1; proj_edited();
    }
    if ((b & (1u << BTN_SAVE)) && c < chan_n && patch_sel >= PRESETS) {
        if (IS_DRUM(chan[c].kind)) patch_msg = "Not drums";
        else patch_msg = patch_save(patch_sel - PRESETS, c) == 0 ? "Saved" : "Save failed";
        patch_msg_t = fm1_ms; patch_dirty = 1;
    }
    if ((b & ((1u << BTN_OCT_DOWN) | (1u << BTN_GLO))) || home_tap) patch_on = 0;
    if (patch_msg && fm1_ms - patch_msg_t >= 1500u && fm1_ms - patch_msg_t < 1600u) patch_dirty = 1;
}
