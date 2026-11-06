/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1: a one-voice synth in a fixed chain, OSC -> FILTER -> EG -> MOD -> DELAY -> REVERB, with an arpeggiator.
 * The panel (docs in ../README.md):
 *   EDIT OSC   SEL FILTER   ENV EG   FX MOD / DELAY / REVERB (each press the next)   LFO OSC (its LFO)
 *   ARP  a press: arp on / off and its page; held half a second: latch
 *   SELECT the section; PRESET the section's TYPE; K1..K4 its parameters (GLO held: fine steps)
 *   ALGORITHM a factory sound; OCT- / OCT+ the octave; SAVE keeps the sound; HOME all notes off
 * Part of the unity build after the platform (gfx.c's drawing is in scope). The sound is in dsp.c (an o2 unit). */
#include <stddef.h>
#include "fastmath.h"
#include "dsp.h"

static float frac1(float x) { return x - fm_floorf(x); }
static void popup(const char *name, const char *val, int32_t bar, int bip);

/* ---------------------------------------------------------------- sounds --- */
#define PATCH_INIT                                                                                            \
    .osc_type = OSC_SAW, .flt_type = FLT_LP2, .eg_type = EG_ADSR, .mod_type = MOD_OFF, .dly_type = DLY_OFF,  \
    .rev_type = REV_OFF, .arp_type = ARP_UP, .arp_chord = CHORD_MAJ, .shape = 0, .alt = 0, .lfo_rate = 300,  \
    .lfo_depth = 512, .cutoff = 700, .reso = 200, .sweep_rate = 0, .sweep_depth = 512, .attack = 20,         \
    .release = 380, .trem_rate = 300, .trem_depth = 0, .mod_time = 300, .mod_depth = 500, .dly_time = 500,   \
    .dly_depth = 400, .dly_mix = 350, .rev_time = 500, .rev_depth = 400, .rev_mix = 300, .arp_len = 8,       \
    .arp_tempo = 1200, .arp_oct = 1

/* each sound is INIT with some fields given again: the override is the point */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
static const patch_t FACTORY[] = {
    {PATCH_INIT},                                                                       /* INIT */
    {PATCH_INIT, .osc_type = OSC_SQR, .shape = 300, .alt = 700, .flt_type = FLT_LP4, .cutoff = 420,
     .reso = 450, .sweep_rate = 120, .sweep_depth = 300, .release = 250},              /* BASS */
    {PATCH_INIT, .osc_type = OSC_SAW, .shape = 350, .lfo_rate = 420, .lfo_depth = 420, .cutoff = 820,
     .mod_type = MOD_CHORUS, .dly_type = DLY_PINGPONG, .dly_time = 420},               /* LEAD */
    {PATCH_INIT, .osc_type = OSC_TRI, .shape = 500, .lfo_rate = 150, .lfo_depth = 800, .eg_type = EG_AHR,
     .attack = 700, .release = 800, .mod_type = MOD_ENSEMBLE, .rev_type = REV_HALL, .rev_time = 800,
     .rev_mix = 500},                                                                   /* PAD */
    {PATCH_INIT, .osc_type = OSC_VPM, .shape = 450, .alt = 400, .eg_type = EG_AR, .release = 330,
     .flt_type = FLT_OFF, .dly_type = DLY_TAPE, .dly_time = 560, .dly_depth = 550, .arp_type = ARP_UPDOWN,
     .arp_chord = CHORD_MIN, .arp_oct = 2, .arp_len = 12},                             /* ARP (VPM) */
    {PATCH_INIT, .osc_type = OSC_SAW, .shape = 600, .alt = 900, .eg_type = EG_OPEN, .flt_type = FLT_BP2,
     .cutoff = 500, .reso = 600, .sweep_rate = 40, .sweep_depth = 900, .mod_type = MOD_PHASER,
     .rev_type = REV_SUBMARINE, .rev_mix = 600},                                       /* DRONE */
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static const char *const FACTORY_NAME[] = {"INIT", "BASS", "LEAD", "PAD", "ARP VPM", "DRONE"};
#define NFACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))

#define SET_MAGIC 0x43484E32u            /* "CHN2" */
typedef struct {
    uint32_t magic;
    patch_t p;
    int8_t octave;
    uint8_t rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

/* ------------------------------------------------------------- the panel --- */
enum { S_OSC, S_FLT, S_EG, S_MOD, S_DLY, S_REV, S_ARP, NS };
enum { F_PCT, F_BIP, F_INT, F_BPM, F_CHORD };
typedef struct {
    const char *label;
    uint16_t off;                        /* offsetof(patch_t, ...) */
    uint8_t size, fmt;                   /* 1 or 2 bytes */
    int16_t lo, hi, step;
    char neg, pos;                       /* F_BIP: the letters below and above the centre */
} param_t;
typedef struct {
    const char *name, *tag;              /* the page's title, its box in the chain */
    uint16_t type_off;
    uint8_t ntypes;
    const char *const *types;
    param_t k[4];
} section_t;

#define U16(f, l) {l, offsetof(patch_t, f), 2, F_PCT, 0, 1023, 8, 0, 0}
#define BIP(f, l, n, p) {l, offsetof(patch_t, f), 2, F_BIP, 0, 1023, 8, n, p}
static const char *const OSC_T[] = {"SAW", "TRI", "SQR", "VPM"};
static const char *const FLT_T[] = {"LP2", "LP4", "BP2", "BP4", "HP2", "HP4", "OFF"};
static const char *const EG_T[] = {"ADSR", "AHR", "AR", "AR LOOP", "OPEN"};
static const char *const MOD_T[] = {"OFF", "CHORUS", "ENSEMBLE", "PHASER", "FLANGER"};
static const char *const DLY_T[] = {"OFF", "STEREO", "MONO", "PING PONG", "HI PASS", "TAPE"};
static const char *const REV_T[] = {"OFF", "HALL", "PLATE", "SPACE", "RISER", "SUBMARINE"};
static const char *const ARP_T[] = {"UP", "DOWN", "UP-DOWN", "DOWN-UP", "CONVERGE", "DIVERGE", "CONV-DIV",
                                    "DIV-CONV", "RANDOM", "STOCHASTIC"};
static const char *const CHORD_T[] = {"OCTAVE", "MAJOR", "SUS", "AUG", "MINOR", "DIM"};

static const section_t SECT[NS] = {
    {"OSC", "OSC", offsetof(patch_t, osc_type), OSC_N, OSC_T,
     {U16(shape, "SHAPE"), U16(alt, "ALT"), U16(lfo_rate, "LFO RATE"), BIP(lfo_depth, "LFO DEPTH", 'P', 'S')}},
    {"FILTER", "FLT", offsetof(patch_t, flt_type), FLT_N, FLT_T,
     {U16(cutoff, "CUTOFF"), U16(reso, "RESONANCE"), U16(sweep_rate, "SWEEP RATE"),
      BIP(sweep_depth, "SWEEP DEPTH", 'D', 'U')}},
    {"EG", "EG", offsetof(patch_t, eg_type), EG_N, EG_T,
     {U16(attack, "ATTACK"), U16(release, "RELEASE"), U16(trem_rate, "TREM RATE"), U16(trem_depth, "TREM DEPTH")}},
    {"MOD", "MOD", offsetof(patch_t, mod_type), MOD_N, MOD_T,
     {U16(mod_time, "TIME"), U16(mod_depth, "DEPTH"), {0}, {0}}},
    {"DELAY", "DLY", offsetof(patch_t, dly_type), DLY_N, DLY_T,
     {U16(dly_time, "TIME"), U16(dly_depth, "DEPTH"), U16(dly_mix, "MIX"), {0}}},
    {"REVERB", "REV", offsetof(patch_t, rev_type), REV_N, REV_T,
     {U16(rev_time, "TIME"), U16(rev_depth, "DEPTH"), U16(rev_mix, "MIX"), {0}}},
    {"ARP", "ARP", offsetof(patch_t, arp_type), ARP_N, ARP_T,
     {{"LENGTH", offsetof(patch_t, arp_len), 2, F_INT, 1, 24, 1, 0, 0},
      {"TEMPO", offsetof(patch_t, arp_tempo), 2, F_BPM, 560, 2400, 10, 0, 0},
      {"CHORD", offsetof(patch_t, arp_chord), 1, F_CHORD, 0, CHORD_N - 1, 1, 0, 0},
      {"OCTAVES", offsetof(patch_t, arp_oct), 1, F_INT, 1, 3, 1, 0, 0}}},
};

static int32_t p_get(const param_t *k)
{
    const uint8_t *b = (const uint8_t *)&set.p + k->off;
    return k->size == 2 ? (int32_t)*(const uint16_t *)(const void *)b : (int32_t)*b;
}
static void p_put(const param_t *k, int32_t v)
{
    uint8_t *b = (uint8_t *)&set.p + k->off;
    v = clamp(v, k->lo, k->hi);
    if (k->size == 2)
        *(uint16_t *)(void *)b = (uint16_t)v;
    else
        *b = (uint8_t)v;
}
static int type_get(int s) { return ((const uint8_t *)&set.p)[SECT[s].type_off]; }
static void type_put(int s, int v) { ((uint8_t *)&set.p)[SECT[s].type_off] = (uint8_t)clamp(v, 0, SECT[s].ntypes - 1); }

/* ---------------------------------------------------------- MIDI and sound --- */
static volatile uint16_t cc_in[4];       /* CC 74 71 73 72 (cutoff, resonance, attack, release) + 1, 0 none */
static void midi_in(void)                /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st == 0x90u || st == 0x80u)
            dsp_midi_note((int)d1, st == 0x90u ? (int)d2 : 0);
        else if (st == 0xB0u) {
            int i = d1 == 74u ? 0 : d1 == 71u ? 1 : d1 == 73u ? 2 : d1 == 72u ? 3 : -1;
            if (i >= 0)
                cc_in[i] = (uint16_t)(d2 + 1u);
        }
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    /* dsp_render runs from RAM: called through a pointer (a direct call does not reach it from the flash) */
    PLAT_FAR(dsp_render)(lr, n, (float)master_q12 / 4096.0f * 8300000.0f);
}

void app_panic(void) { dsp_panic(); }

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2) {
        set.magic = SET_MAGIC;
        set.p = FACTORY[0];
        set.octave = 0;
    }
    dsp_init();
    dsp_patch(&set.p);
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    int sect, factory;
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn, arp_t;
    uint8_t arp_on, arp_latch, arp_held_done, redraw;
    int32_t last_note;
} ui;

static void copy_text(char *d, const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1u < max && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
static void text_cat(char *d, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (d[n])
        n++;
    copy_text(d + n, s, max - n);
}
static void num_text(int32_t v, char *b)
{
    char t[12];
    int k = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[k++] = (char)('0' + u % 10u);
    while ((u /= 10u) && k < 11);
    if (v < 0)
        *b++ = '-';
    while (k)
        *b++ = t[--k];
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    copy_text(ui.msg[0], a, sizeof ui.msg[0]);
    copy_text(ui.msg[1], b, sizeof ui.msg[1]);
    ui.msg_until = plat_ms() + 1200u;
    ui.redraw = 1;
    if (ann_on())
        ann_say(ui.msg[0], 0, ANN_NOW);
}

static void value_text(const param_t *k, int32_t v, char *t)
{
    char n[12];
    t[0] = 0;
    switch (k->fmt) {
    case F_BIP:
        if (v == 512) {
            copy_text(t, "--", 12);
            break;
        }
        t[0] = v < 512 ? k->neg : k->pos;
        t[1] = 0;
        num_text((v < 512 ? 512 - v : v - 512) * 100 / 511, n);
        text_cat(t, n, 12);
        break;
    case F_INT: num_text(v, t); break;
    case F_BPM:
        num_text(v / 10, t);
        text_cat(t, ".", 12);
        num_text(v % 10, n);
        text_cat(t, n, 12);
        break;
    case F_CHORD: copy_text(t, CHORD_T[v % CHORD_N], 12); break;
    default: num_text(v * 100 / 1023, t); break;
    }
}

static void changed(void)
{
    set_dirty = 1;
    ui.redraw = 1;
    dsp_patch(&set.p);
}

static void go_section(int s)
{
    ui.sect = s;
    ui.redraw = 1;
    if (ann_on())
        ann_say(SECT[s].name, 0, ANN_NOW);
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k, now = plat_ms();
    int fine = (btn >> B_GLO) & 1u, i;
    int32_t e;
    char t[16];
    for (k = 0; k < NKEYS; k++) {        /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        int note = 53 + (int)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            dsp_key(note, 1);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | 100u << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            dsp_key(note, 0);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {           /* presses */
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_EDIT: case B_LFO: go_section(S_OSC); break;
        case B_SEL: go_section(S_FLT); break;
        case B_ENV: go_section(S_EG); break;
        case B_FX: go_section(ui.sect == S_MOD ? S_DLY : ui.sect == S_DLY ? S_REV : S_MOD); break;
        case B_ARP: ui.arp_t = now; ui.arp_held_done = 0; break;
        case B_HOME: dsp_panic(); app_say("ALL NOTES", "OFF"); break;
        case B_OCTDN: case B_OCTUP: {
            int o = clamp(set.octave + (k == B_OCTUP ? 1 : -1), -2, 2);
            if (o != set.octave) {
                dsp_panic();
                set.octave = (int8_t)o;
                set_dirty = 1;
                num_text(o, t);
                app_say("OCTAVE", t);
            }
            break;
        }
        case B_SAVE:
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
            break;
        default: break;
        }
    }
    if ((btn >> B_ARP) & 1u) {           /* ARP held half a second: latch */
        if (!ui.arp_held_done && now - ui.arp_t >= 500u) {
            ui.arp_held_done = 1;
            ui.arp_latch ^= 1u;
            ui.arp_on = 1;
            dsp_arp(ui.arp_on, ui.arp_latch);
            ui.sect = S_ARP;
            app_say("LATCH", ui.arp_latch ? "ON" : "OFF");
        }
    } else if ((ui.btn >> B_ARP) & 1u && !ui.arp_held_done) {   /* a press: on / off */
        ui.arp_on ^= 1u;
        if (!ui.arp_on)
            ui.arp_latch = 0;
        dsp_arp(ui.arp_on, ui.arp_latch);
        ui.sect = S_ARP;
        app_say("ARP", ui.arp_on ? "ON" : "OFF");
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        go_section((ui.sect + (int)NS + (e > 0 ? 1 : -1)) % (int)NS);
    if ((e = plat_enc(EN_PRESET)) != 0) {
        type_put(ui.sect, type_get(ui.sect) + (e > 0 ? 1 : -1));
        changed();
        popup(SECT[ui.sect].name, SECT[ui.sect].types[type_get(ui.sect)], -2000, 0);
        if (ann_on())
            ann_say(SECT[ui.sect].types[type_get(ui.sect)], 0, ANN_NOW);
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        ui.factory = (ui.factory + NFACTORY + (e > 0 ? 1 : -1)) % NFACTORY;
        set.p = FACTORY[ui.factory];
        changed();
        app_say("SOUND", FACTORY_NAME[ui.factory]);
    }
    for (i = 0; i < 4; i++) {
        const param_t *pk = &SECT[ui.sect].k[i];
        if ((e = plat_enc(EN_K1 + i)) == 0 || !pk->label)
            continue;
        p_put(pk, p_get(pk) + e * (fine ? 1 : pk->step));
        changed();
        {                                /* the value, large, a moment (NTS-1) */
            int32_t v = p_get(pk), bar = pk->fmt == F_BIP ? (v - 512) * 1000 / 511
                                                          : (v - pk->lo) * 1000 / (pk->hi - pk->lo);
            value_text(pk, v, t);
            popup(pk->label, t, bar, pk->fmt == F_BIP);
        }
    }
    for (i = 0; i < 4; i++) {            /* MIDI CCs into the sound (here, where the patch lives) */
        uint16_t c = cc_in[i];
        if (c) {
            static const uint16_t OFF[4] = {offsetof(patch_t, cutoff), offsetof(patch_t, reso),
                                            offsetof(patch_t, attack), offsetof(patch_t, release)};
            cc_in[i] = 0;
            *(uint16_t *)(void *)((uint8_t *)&set.p + OFF[i]) = (uint16_t)((c - 1u) * 1023u / 127u);
            changed();
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {                                    /* LEDs: the section's button, ARP while on, SAVE while unsaved */
        static const uint8_t SBTN[NS] = {B_EDIT, B_SEL, B_ENV, B_FX, B_FX, B_FX, B_ARP};
        uint32_t lit = btn | 1u << SBTN[ui.sect] | (ui.arp_on ? 1u << B_ARP : 0u) | (set_dirty ? 1u << B_SAVE : 0u);
        uint32_t klit = keys;
        int nn = dsp_note() - 53 - 12 * set.octave;
        if (nn >= 0 && nn < NKEYS)
            klit |= 1u << nn;            /* the note sounding (the arp's too) */
        plat_glow(1);
        plat_leds(lit, klit);
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------- draw --- */
static int sect_on(int s)
{
    switch (s) {
    case S_FLT: return set.p.flt_type != FLT_OFF;
    case S_MOD: return set.p.mod_type != MOD_OFF;
    case S_DLY: return set.p.dly_type != DLY_OFF;
    case S_REV: return set.p.rev_type != REV_OFF;
    case S_ARP: return ui.arp_on;
    default: return 1;
    }
}

/* NTS-1 style: the top is an oscilloscope of the output, until a knob or a type changes: then the parameter's
 * name and its value, large, for a moment, then the scope again. The chain at the bottom carries the signal: dots
 * run through it while it sounds, dimmed through the sections that are off. */
static struct {
    uint32_t pop_until, top_at, bot_at;
    char pop_name[24], pop_val[24];
    int32_t pop_bar;                     /* 0..1000, -1 none; a bipolar one: -1000..1000 with pop_bip */
    uint8_t pop_bip;
    float scale;
} vw = {0, 0, 0, "", "", -1, 0, 30.0f};

static void popup(const char *name, const char *val, int32_t bar, int bip)
{
    copy_text(vw.pop_name, name, sizeof vw.pop_name);
    copy_text(vw.pop_val, val, sizeof vw.pop_val);
    vw.pop_bar = bar;
    vw.pop_bip = (uint8_t)bip;
    vw.pop_until = plat_ms() + 1400u;
    ui.redraw = 1;
}

static void draw_top(void)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const section_t *s = &SECT[ui.sect];
    char t[32], o[12];
    int32_t i, note = dsp_note();
    uint32_t now = plat_ms();
    cv_begin(240, 120, C_BLACK);
    /* the header: where we are, small */
    copy_text(t, s->name, sizeof t);
    text_cat(t, "  ", sizeof t);
    text_cat(t, s->types[type_get(ui.sect)], sizeof t);
    cv_text(8, 3, &FONT_XS, t, C_GRAY);
    if (note >= 0) {
        copy_text(t, N[note % 12], sizeof t);
        num_text(note / 12 - 1, o);
        text_cat(t, o, sizeof t);
        cv_text(232 - text_w(&FONT_XS, t), 3, &FONT_XS, t, C_HI);
    }
    if (ui.arp_on)
        cv_text(150, 3, &FONT_XS, ui.arp_latch ? "ARP LATCH" : "ARP", C_AMB);
    if (now < vw.pop_until || now < ui.msg_until) {   /* the value, large */
        const char *nm = now < vw.pop_until ? vw.pop_name : ui.msg[0], *vl = now < vw.pop_until ? vw.pop_val : ui.msg[1];
        cv_text(120 - text_w(&FONT_S, nm) / 2, 26, &FONT_S, nm, C_GRAY);
        cv_text(120 - text_w(&FONT_L, vl) / 2, 48, &FONT_L, vl, C_WHITE);
        if (now < vw.pop_until && vw.pop_bar >= -1000 && !(vw.pop_bar < 0 && !vw.pop_bip)) {
            cv_rect(30, 96, 180, 4, C_LINE);
            if (vw.pop_bip) {
                int32_t w = vw.pop_bar * 90 / 1000;
                cv_rect(w < 0 ? 120 + w : 120, 96, w < 0 ? -w : w, 4, C_HI);
                cv_rect(119, 93, 2, 10, C_GRAY);
            } else {
                cv_rect(30, 96, vw.pop_bar * 180 / 1000, 4, C_HI);
            }
        }
    } else {                             /* the oscilloscope */
        float buf[220], mx = 0.0f;
        int trig = dsp_scope(buf, 220);
        for (i = 0; i < 220; i += 20)    /* a dotted grid */
            cv_rect(10 + i, 66, 2, 1, C_LINE);
        for (i = 22; i < 112; i += 22)
            cv_rect(120, i, 1, 2, C_LINE);
        for (i = 0; i < 220; i++)
            if (fm_fabsf(buf[i]) > mx)
                mx = fm_fabsf(buf[i]);
        /* a gentle auto-gain, as the NTS-1's scope keeps its picture in frame */
        vw.scale += (fm_clampf(38.0f / fm_maxf(mx, 0.02f), 30.0f, 900.0f) - vw.scale) * 0.3f;
        if (mx < 0.0005f) {
            cv_rect(10, 66, 220, 1, C_DIM);
        } else {
            int32_t px = 10, py = 66 - (int32_t)(buf[0] * vw.scale);
            for (i = 1; i < 220; i++) {
                int32_t y = 66 - (int32_t)fm_clampf(buf[i] * vw.scale, -44.0f, 44.0f);
                cv_line(px, py, 10 + i, y, trig ? C_WHITE : C_HI);
                cv_line(px, py + 1, 10 + i, y + 1, trig ? C_WHITE : C_HI);
                px = 10 + i;
                py = y;
            }
        }
    }
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    const section_t *s = &SECT[ui.sect];
    char t[24];
    int32_t i;
    float vu = dsp_vu(), tm = (float)plat_ms() * 0.001f;
    cv_begin(240, 120, C_BLACK);
    for (i = 0; i < 4; i++) {
        const param_t *k = &s->k[i];
        int32_t y = 2 + i * 21, v, w;
        if (!k->label)
            continue;
        v = p_get(k);
        cv_text(10, y, &FONT_XS, k->label, C_GRAY);
        cv_rect(96, y + 5, 100, 6, C_LINE);
        if (k->fmt == F_BIP) {
            w = (v - 512) * 50 / 511;
            cv_rect(w < 0 ? 146 + w : 146, y + 5, w < 0 ? -w : w, 6, C_HI);
            cv_rect(145, y + 3, 2, 10, C_GRAY);
        } else {
            w = (v - k->lo) * 100 / (k->hi - k->lo);
            cv_rect(96, y + 5, w, 6, C_HI);
        }
        value_text(k, v, t);
        cv_text(232 - text_w(&FONT_XS, t), y, &FONT_XS, t, C_WHITE);
    }
    for (i = 0; i < S_ARP; i++) {        /* OSC > FLT > EG > MOD > DLY > REV: lit where we are, dim when off */
        int32_t bx = 6 + i * 38;
        int here = i == ui.sect, on = sect_on(i);
        cv_rect(bx, 92, 34, 20, here ? C_HI : on ? C_DIM : C_LINE);
        cv_text(bx + (34 - text_w(&FONT_XS, SECT[i].tag)) / 2, 94, &FONT_XS, SECT[i].tag,
                here ? C_BLACK : on ? C_WHITE : C_GRAY);
    }
    if (vu > 0.004f) {                   /* the signal running through the chain */
        float b = fm_clampf(vu * 4.0f, 0.15f, 1.0f);
        for (i = 0; i < 8; i++) {
            float u = frac1(tm * 0.45f + (float)i / 8.0f);
            int32_t x = 6 + (int32_t)(u * 226.0f), sec = (x - 6) / 38;
            int on = sec >= S_ARP || sect_on(sec);
            cv_rect(x, 85, 3, 3, on ? (b > 0.6f ? C_WHITE : C_HI) : C_LINE);
        }
    }
    cv_blit(0, 120);
}

static void draw(void)
{
    draw_top();
    draw_bottom();
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    int32_t note = dsp_note();
    uint32_t now = plat_ms();
    int sounding = dsp_vu() > 0.004f;
    input();
    ann_tick(now);
    if (note != ui.last_note) {
        ui.last_note = note;
        ui.redraw = 1;
    }
    if ((ui.msg_until && now >= ui.msg_until) || (vw.pop_until && now >= vw.pop_until)) {
        ui.msg_until = vw.pop_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
        vw.top_at = vw.bot_at = now;
    } else if (sounding || now - vw.top_at < 400u) {   /* the scope ~15 / s, the chain's dots ~7 / s */
        if (now - vw.top_at >= 66u) {
            vw.top_at = now;
            draw_top();
        }
        if (sounding && now - vw.bot_at >= 140u) {
            vw.bot_at = now;
            draw_bottom();
        }
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "section")) *got = ui.sect;
    else if (!strcmp(what, "osc")) *got = set.p.osc_type;
    else if (!strcmp(what, "flt")) *got = set.p.flt_type;
    else if (!strcmp(what, "eg")) *got = set.p.eg_type;
    else if (!strcmp(what, "mod")) *got = set.p.mod_type;
    else if (!strcmp(what, "dly")) *got = set.p.dly_type;
    else if (!strcmp(what, "rev")) *got = set.p.rev_type;
    else if (!strcmp(what, "arp")) *got = ui.arp_on;
    else if (!strcmp(what, "latch")) *got = ui.arp_latch;
    else if (!strcmp(what, "cutoff")) *got = set.p.cutoff;
    else if (!strcmp(what, "factory")) *got = ui.factory;
    else if (!strcmp(what, "note")) *got = dsp_note();
    else if (!strcmp(what, "steps")) *got = (int)dsp_arp_steps();
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "loud")) *got = dsp_peak() > 0.02f;   /* sound since the last look */
    else return 0;
    return 1;
}
#endif
