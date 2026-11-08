/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1: a synth in a fixed chain, OSC -> FILTER -> AMP -> MOD -> DELAY -> REVERB, with an arpeggiator; up to eight
 * voices; logue SDK units for the NTS-1 (mk1 and mkII) in the OSC, MOD, DELAY and REVERB places.
 * The panel (docs in ../README.md):
 *   EDIT OSC   SEL FILTER / FILTER EG   ENV AMP EG / FILTER EG   LFO the LFO   SEQ VOICE   FX MOD / DELAY / REVERB
 *   REC the section's next page   PLAY the unit bank   ARP a press: on / off; held: latch
 *   SELECT the section; PRESET its TYPE (a unit after the built-in ones); K1..K4 its parameters (GLO held: fine)
 *   ALGORITHM a sound (factory F1.., yours U1..U16); SAVE keeps it; OCT- / OCT+ the octave; HOME all notes off
 *   GLO + HOME the USB port as a MIDI host
 * Part of the unity build after the platform (gfx.c's drawing is in scope). The sound is in dsp.c (an o2 unit). */
#include <stddef.h>
#include "fastmath.h"
#include "dsp.h"
#include "logue/logue.h"

static float frac1(float x) { return x - fm_floorf(x); }
static void popup(const char *name, const char *val, int32_t bar, int bip);

/* ------------------------------------------------------------------------------------------ sounds --- */
#define PATCH_INIT                                                                                            \
    .osc_type = OSC_SAW, .flt_type = FLT_LP2, .eg_type = EG_ADSR, .feg_type = FEG_ADSR, .lfo_type = LFO_TRI,  \
    .voice_mode = VM_POLY, .mod_type = MOD_OFF, .dly_type = DLY_OFF, .rev_type = REV_OFF, .arp_type = ARP_UP,  \
    .arp_chord = CHORD_MAJ, .arp_oct = 1, .voices = 8, .shape = 0, .alt = 0, .noise = 0, .tune = 512,           \
    .cutoff = 640, .reso = 200, .feg_amt = 700, .keytrk = 400, .a = 30, .d = 450, .s = 800, .r = 380,         \
    .fa = 20, .fd = 420, .fs = 300, .fr = 400, .lfo_rate = 380, .lfo_pitch = 0, .lfo_shape = 0, .lfo_cut = 0, \
    .glide = 0, .detune = 300, .velo = 600, .bend = 2, .mod_time = 300, .mod_depth = 500, .mod_mix = 1023,     \
    .dly_time = 500, .dly_depth = 400, .dly_mix = 350, .dly_tone = 300, .rev_time = 500, .rev_depth = 400,    \
    .rev_mix = 300, .rev_tone = 0, .arp_len = 8, .arp_tempo = 1200, .arp_gate = 600, .arp_swing = 0

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
static const patch_t FACTORY[] = {
    {PATCH_INIT},                                                                       /* INIT */
    {PATCH_INIT, .osc_type = OSC_SAW, .shape = 120, .flt_type = FLT_LP4, .cutoff = 470, .reso = 300,
     .feg_amt = 760, .fa = 300, .fd = 650, .fs = 500, .a = 520, .d = 600, .s = 850, .r = 620,
     .mod_type = MOD_ENSEMBLE, .rev_type = REV_HALL, .rev_time = 700, .rev_mix = 420},  /* POLY PAD */
    {PATCH_INIT, .osc_type = OSC_SAW, .flt_type = FLT_LP2, .cutoff = 420, .reso = 260, .feg_amt = 820,
     .fa = 120, .fd = 380, .fs = 560, .a = 140, .s = 900, .r = 300, .velo = 800, .mod_type = MOD_CHORUS,
     .mod_mix = 500},                                                                   /* BRASS */
    {PATCH_INIT, .voice_mode = VM_MONO, .osc_type = OSC_SQR, .shape = 300, .alt = 700, .flt_type = FLT_LP4,
     .cutoff = 360, .reso = 450, .feg_amt = 820, .fd = 330, .fs = 0, .d = 500, .s = 700, .r = 200,
     .glide = 180},                                                                     /* BASS */
    {PATCH_INIT, .voice_mode = VM_LEGATO, .osc_type = OSC_SAW, .shape = 350, .lfo_rate = 420, .lfo_pitch = 180,
     .cutoff = 760, .glide = 260, .mod_type = MOD_CHORUS, .dly_type = DLY_PINGPONG, .dly_time = 420},   /* LEAD */
    {PATCH_INIT, .osc_type = OSC_TRI, .shape = 420, .flt_type = FLT_LP2, .cutoff = 380, .feg_amt = 900,
     .fd = 300, .fs = 0, .feg_type = FEG_AD, .a = 0, .d = 380, .s = 0, .r = 360, .dly_type = DLY_STEREO,
     .dly_time = 380, .dly_mix = 300},                                                  /* PLUCK */
    {PATCH_INIT, .voice_mode = VM_UNISON, .voices = 6, .detune = 420, .osc_type = OSC_SAW, .cutoff = 700,
     .feg_amt = 560, .a = 380, .r = 600, .rev_type = REV_PLATE, .rev_mix = 380},        /* SUPERSAW */
    {PATCH_INIT, .osc_type = OSC_VPM, .shape = 520, .alt = 400, .flt_type = FLT_OFF, .eg_type = EG_ADSR,
     .a = 0, .d = 700, .s = 0, .r = 700, .rev_type = REV_SPACE, .rev_mix = 450},         /* BELL */
    {PATCH_INIT, .voice_mode = VM_MONO, .osc_type = OSC_VPM, .shape = 450, .alt = 400, .eg_type = EG_AR,
     .r = 330, .flt_type = FLT_OFF, .dly_type = DLY_TAPE, .dly_time = 560, .dly_depth = 550,
     .arp_type = ARP_UPDOWN, .arp_chord = CHORD_MIN, .arp_oct = 2, .arp_len = 12},       /* ARP VPM */
    {PATCH_INIT, .osc_type = OSC_SAW, .shape = 600, .alt = 900, .eg_type = EG_OPEN, .flt_type = FLT_BP2,
     .cutoff = 500, .reso = 600, .lfo_type = LFO_DRIFT, .lfo_rate = 200, .lfo_cut = 600, .voice_mode = VM_MONO,
     .mod_type = MOD_PHASER, .rev_type = REV_SUBMARINE, .rev_mix = 600},               /* DRONE */
};
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static const char *const FACTORY_NAME[] = {"INIT", "POLY PAD", "BRASS", "BASS", "LEAD", "PLUCK", "SUPERSAW",
                                           "BELL", "ARP VPM", "DRONE"};
#define NFACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))
#define NUSER 16

#define SET_MAGIC 0x34484E43u            /* "CNH4" */
typedef struct {
    uint32_t magic;
    patch_t p;
    int8_t octave;
    uint8_t cur;                         /* the sound: 0..NFACTORY-1 factory, then the user slots */
    uint8_t rsv[2];
} settings_t;
static settings_t set;
#define BANK_MAGIC 0x34424E43u           /* "CNB4" */
typedef struct {
    uint32_t magic;
    uint32_t used;                       /* bit i: user slot i holds a sound */
    patch_t p[NUSER];
} bank_t;
static bank_t bank;
static uint8_t set_dirty;

/* -------------------------------------------------------------------------------------- the panel --- */
enum { S_OSC, S_FLT, S_EG, S_FEG, S_LFO, S_VOICE, S_MOD, S_DLY, S_REV, S_ARP, S_UNITS, NS };
enum { F_PCT, F_BIP, F_INT, F_BPM, F_CHORD, F_SEMI, F_TIME, F_VOICES, F_UNIT };
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
    uint8_t ntypes, place;               /* place: the logue place (US_*), or 0xFF */
    const char *const *types;
    uint8_t npages;
    param_t k[2][4];
    uint16_t rgb;                        /* its colour */
} section_t;

#define U16(f, l) {l, offsetof(patch_t, f), 2, F_PCT, 0, 1023, 8, 0, 0}
#define TIM(f, l) {l, offsetof(patch_t, f), 2, F_TIME, 0, 1023, 8, 0, 0}
#define BIP(f, l, n, p) {l, offsetof(patch_t, f), 2, F_BIP, 0, 1023, 8, n, p}
static const char *const OSC_T[] = {"SAW", "TRI", "SQR", "VPM", "NOISE"};
static const char *const FLT_T[] = {"LP2", "LP4", "BP2", "BP4", "HP2", "HP4", "OFF"};
static const char *const EG_T[] = {"ADSR", "AR", "AR LOOP", "OPEN"};
static const char *const FEG_T[] = {"ADSR", "AD", "LOOP"};
static const char *const LFO_T[] = {"TRIANGLE", "SINE", "SAW", "SQUARE", "S&H", "DRIFT"};
static const char *const VM_T[] = {"POLY", "MONO", "LEGATO", "UNISON"};
static const char *const MOD_T[] = {"OFF", "CHORUS", "ENSEMBLE", "PHASER", "FLANGER", "TREMOLO"};
static const char *const DLY_T[] = {"OFF", "STEREO", "MONO", "PING PONG", "HI PASS", "TAPE"};
static const char *const REV_T[] = {"OFF", "HALL", "PLATE", "SPACE", "RISER", "SUBMARINE"};
static const char *const ARP_T[] = {"UP", "DOWN", "UP-DOWN", "DOWN-UP", "CONVERGE", "DIVERGE", "CONV-DIV",
                                    "DIV-CONV", "RANDOM", "STOCHASTIC"};
static const char *const CHORD_T[] = {"OCTAVE", "MAJOR", "SUS", "AUG", "MINOR", "DIM"};
static const char *const NONE_T[] = {""};

static const section_t SECT[NS] = {
    {"OSC", "OSC", offsetof(patch_t, osc_type), OSC_N, US_OSC, OSC_T, 1,
     {{U16(shape, "SHAPE"), U16(alt, "ALT"), U16(noise, "NOISE"), {"TUNE", offsetof(patch_t, tune), 2, F_SEMI, 0, 1023, 22, 0, 0}}},
     RGB(255, 150, 60)},
    {"FILTER", "FLT", offsetof(patch_t, flt_type), FLT_N, 0xFF, FLT_T, 1,
     {{U16(cutoff, "CUTOFF"), U16(reso, "RESONANCE"), BIP(feg_amt, "EG AMOUNT", '-', '+'), U16(keytrk, "KEY TRACK")}},
     RGB(80, 200, 255)},
    {"AMP EG", "EG", offsetof(patch_t, eg_type), EG_N, 0xFF, EG_T, 1,
     {{TIM(a, "ATTACK"), TIM(d, "DECAY"), U16(s, "SUSTAIN"), TIM(r, "RELEASE")}}, RGB(110, 230, 120)},
    {"FILTER EG", "FEG", offsetof(patch_t, feg_type), FEG_N, 0xFF, FEG_T, 1,
     {{TIM(fa, "ATTACK"), TIM(fd, "DECAY"), U16(fs, "SUSTAIN"), TIM(fr, "RELEASE")}}, RGB(60, 220, 200)},
    {"LFO", "LFO", offsetof(patch_t, lfo_type), LFO_N, 0xFF, LFO_T, 1,
     {{U16(lfo_rate, "RATE"), U16(lfo_pitch, "PITCH"), U16(lfo_shape, "SHAPE"), U16(lfo_cut, "CUTOFF")}},
     RGB(190, 130, 255)},
    {"VOICE", "VCE", offsetof(patch_t, voice_mode), VM_N, 0xFF, VM_T, 2,
     {{{"VOICES", offsetof(patch_t, voices), 1, F_VOICES, 1, 8, 1, 0, 0}, U16(glide, "GLIDE"), U16(detune, "DETUNE"),
       U16(velo, "VELOCITY")},
      {{"BEND", offsetof(patch_t, bend), 2, F_INT, 0, 12, 1, 0, 0}, {0}, {0}, {0}}},
     RGB(255, 220, 80)},
    {"MOD", "MOD", offsetof(patch_t, mod_type), MOD_N, US_MOD, MOD_T, 1,
     {{U16(mod_time, "TIME"), U16(mod_depth, "DEPTH"), U16(mod_mix, "MIX"), {0}}}, RGB(90, 150, 255)},
    {"DELAY", "DLY", offsetof(patch_t, dly_type), DLY_N, US_DLY, DLY_T, 1,
     {{U16(dly_time, "TIME"), U16(dly_depth, "DEPTH"), U16(dly_mix, "MIX"), U16(dly_tone, "DARK")}},
     RGB(70, 210, 170)},
    {"REVERB", "REV", offsetof(patch_t, rev_type), REV_N, US_REV, REV_T, 1,
     {{U16(rev_time, "TIME"), U16(rev_depth, "DEPTH"), U16(rev_mix, "MIX"), {0}}}, RGB(240, 110, 200)},
    {"ARP", "ARP", offsetof(patch_t, arp_type), ARP_N, 0xFF, ARP_T, 2,
     {{{"LENGTH", offsetof(patch_t, arp_len), 2, F_INT, 1, 24, 1, 0, 0},
       {"TEMPO", offsetof(patch_t, arp_tempo), 2, F_BPM, 560, 2400, 10, 0, 0},
       {"CHORD", offsetof(patch_t, arp_chord), 1, F_CHORD, 0, CHORD_N - 1, 1, 0, 0},
       {"OCTAVES", offsetof(patch_t, arp_oct), 1, F_INT, 1, 3, 1, 0, 0}},
      {U16(arp_gate, "GATE"), U16(arp_swing, "SWING"), {0}, {0}}},
     RGB(255, 180, 60)},
    {"UNITS", "", 0, 1, 0xFF, NONE_T, 1, {{{0}, {0}, {0}, {0}}}, RGB(200, 200, 210)},
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

/* a section's TYPE as a list: its built-in types, then the bank's units of its kind */
#define SECT_TYPES_MAX 24
static const uint8_t PLACE_MODULE[US_N] = {LG_MOD_OSC, LG_MOD_MODFX, LG_MOD_DELFX, LG_MOD_REVFX};
static int type_raw(int s) { return ((const uint8_t *)&set.p)[SECT[s].type_off]; }
static int type_count(int s, int *list)  /* the choices, as raw values */
{
    int n = 0, i;
    for (i = 0; i < SECT[s].ntypes; i++)
        list[n++] = i;
    if (SECT[s].place != 0xFF)
        for (i = 0; i < NUNITS; i++) {
            const unit_info_t *u = dsp_unit(i);
            if (u->used && u->module == PLACE_MODULE[SECT[s].place])
                list[n++] = SECT[s].ntypes + i;
        }
    return n;
}
static const unit_info_t *type_unit(int s)   /* the unit a section's TYPE is, or 0 */
{
    int t = type_raw(s) - SECT[s].ntypes;
    const unit_info_t *u;
    if (SECT[s].place == 0xFF || t < 0)
        return 0;
    u = dsp_unit(t);
    return u && u->used ? u : 0;
}
static const char *type_name(int s)
{
    int t = type_raw(s);
    if (t < SECT[s].ntypes)
        return SECT[s].types[t];
    return type_unit(s) ? type_unit(s)->name : "EMPTY";
}

static void unit_defaults(int place, const unit_info_t *u)
{
    int i;
    for (i = 0; i < UPARAM; i++)
        set.p.up[place][i] = i < u->nparam ? u->param[i].init : 0;
    set.p.ucrc[place] = u->crc;
}

static void type_step(int s, int d)
{
    int list[SECT_TYPES_MAX] = {0}, n = 0, i, cur = type_raw(s), at = 0;
    n = type_count(s, list);
    for (i = 0; i < n; i++)
        if (list[i] == cur)
            at = i;
    at = clamp(at + d, 0, n - 1);
    ((uint8_t *)&set.p)[SECT[s].type_off] = (uint8_t)list[at];
    if (SECT[s].place != 0xFF && list[at] >= SECT[s].ntypes) {
        const unit_info_t *u = dsp_unit(list[at] - SECT[s].ntypes);
        if (u && u->crc != set.p.ucrc[SECT[s].place])
            unit_defaults(SECT[s].place, u);
    }
}

/* a sound's units: each place's unit found by its CRC (the bank may have moved it); gone, the place goes built-in */
static int fix_units(void)
{
    int place, missing = 0;
    static const int SEC[US_N] = {S_OSC, S_MOD, S_DLY, S_REV};
    for (place = 0; place < US_N; place++) {
        int s = SEC[place], t = type_raw(s) - SECT[s].ntypes, i;
        const unit_info_t *u;
        if (t < 0)
            continue;
        u = dsp_unit(t);
        if (u && u->used && u->crc == set.p.ucrc[place])
            continue;
        for (i = 0; i < NUNITS; i++) {
            const unit_info_t *w = dsp_unit(i);
            if (w->used && w->crc == set.p.ucrc[place] && w->module == PLACE_MODULE[place])
                break;
        }
        if (i < NUNITS) {
            ((uint8_t *)&set.p)[SECT[s].type_off] = (uint8_t)(SECT[s].ntypes + i);
        } else {
            ((uint8_t *)&set.p)[SECT[s].type_off] = 0;   /* SAW, or OFF */
            missing++;
        }
    }
    return missing;
}

/* -------------------------------------------------------------------------------- MIDI and sound --- */
static volatile uint16_t cc_in[5];       /* CC 74 71 73 72 1 (cutoff, resonance, attack, release, wheel) + 1 */
static void midi_in(void)                /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st == 0x90u || st == 0x80u)
            dsp_midi_note((int)d1, st == 0x90u ? (int)d2 : 0);
        else if (st == 0xE0u)
            dsp_midi_bend((int)(d1 | d2 << 7) - 8192);
        else if (st == 0xB0u) {
            int i = d1 == 74u ? 0 : d1 == 71u ? 1 : d1 == 73u ? 2 : d1 == 72u ? 3 : d1 == 1u ? 4 : -1;
            if (i == 4)
                dsp_midi_wheel((int)d2);
            else if (i >= 0)
                cc_in[i] = (uint16_t)(d2 + 1u);
            if (d1 == 123u)
                dsp_panic();
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
void app_overload(void) { dsp_overload(); }

static void load_sound(int i)
{
    set.cur = (uint8_t)i;
    if (i < NFACTORY)
        set.p = FACTORY[i];
    else
        set.p = bank.p[i - NFACTORY];
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2) {
        set.magic = SET_MAGIC;
        set.p = FACTORY[1];
        set.octave = 0;
        set.cur = 1;
    }
    if (plat_store_load(OBJ_BANK, &bank, sizeof bank) != (int)sizeof bank || bank.magic != BANK_MAGIC) {
        uint32_t i;
        uint8_t *b = (uint8_t *)&bank;
        for (i = 0; i < sizeof bank; i++)
            b[i] = 0;
        bank.magic = BANK_MAGIC;
    }
    dsp_units_scan();
    fix_units();
    dsp_init();
    dsp_patch(&set.p);
    plat_cpu2_set(1);
}

/* ------------------------------------------------------------------------------------------- UI --- */
static struct {
    int sect, page, ucur;                /* the section and its page; UNITS: the slot under the cursor */
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn, arp_t, save_t;
    uint8_t arp_on, arp_latch, arp_held_done, redraw, erase_armed;
    int32_t last_note;
    uint32_t last_voices;
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
static void num2(int32_t v, char *b)     /* two digits */
{
    b[0] = (char)('0' + v / 10 % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
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

static void sound_name(int i, char *t, uint32_t max)
{
    char n[4];
    if (i < NFACTORY) {
        copy_text(t, "F", max);
        num2(i + 1, n);
        text_cat(t, n, max);
        text_cat(t, " ", max);
        text_cat(t, FACTORY_NAME[i], max);
    } else {
        copy_text(t, "U", max);
        num2(i - NFACTORY + 1, n);
        text_cat(t, n, max);
        text_cat(t, (bank.used >> (i - NFACTORY)) & 1u ? " USER" : " (EMPTY)", max);
    }
}

/* the envelope times as the engine has them (dsp.c eg_coef): attack 1 ms .. 10 s, decay / release 2 ms .. 10 s */
static void time_text(const param_t *k, int32_t v, char *t)
{
    float x = (float)v / 1023.0f, ms = (k->off == offsetof(patch_t, a) || k->off == offsetof(patch_t, fa))
                                           ? 1.0f * fm_powf(10000.0f, x) : 2.0f * fm_powf(5000.0f, x);
    char n[12];
    if (ms < 1000.0f) {
        num_text((int32_t)(ms + 0.5f), t);
        text_cat(t, "MS", 12);
    } else {
        num_text((int32_t)(ms / 1000.0f), t);
        text_cat(t, ".", 12);
        num_text((int32_t)(ms / 100.0f) % 10, n);
        text_cat(t, n, 12);
        text_cat(t, "S", 12);
    }
}

static void value_text(const param_t *k, int32_t v, char *t)
{
    char n[12];
    t[0] = 0;
    switch (k->fmt) {
    case F_BIP:
        if (v == 512) {
            copy_text(t, "0", 12);
            break;
        }
        t[0] = v < 512 ? k->neg : k->pos;
        t[1] = 0;
        num_text((v < 512 ? 512 - v : v - 512) * 100 / 511, n);
        text_cat(t, n, 12);
        break;
    case F_SEMI: {
        int32_t st = (v - 512) * 24 / 511;
        if (st > 0)
            copy_text(t, "+", 12);
        num_text(st, n);
        text_cat(t, n, 12);
        text_cat(t, " ST", 12);
        break;
    }
    case F_INT: num_text(v, t); break;
    case F_VOICES:
        num_text(v, t);
        if (set.p.voice_mode == VM_MONO || set.p.voice_mode == VM_LEGATO)
            copy_text(t, "(MONO)", 12);
        break;
    case F_BPM:
        num_text(v / 10, t);
        text_cat(t, ".", 12);
        num_text(v % 10, n);
        text_cat(t, n, 12);
        break;
    case F_CHORD: copy_text(t, CHORD_T[v % CHORD_N], 12); break;
    case F_TIME: time_text(k, v, t); break;
    default: num_text(v * 100 / 1023, t); break;
    }
}

/* a unit's parameter: its own range and decimals (mkII: frac and its mode, in .frac's bits 0-3 and 4) */
static void unit_value_text(const unit_info_t *u, int i, int32_t v, char *t)
{
    int nf = u->param[i].frac & 15, dec = (u->param[i].frac >> 4) & 1;
    char n[12];
    if (u->param[i].type == LG_T_KNOB) {
        num_text(v * 100 / 1023, t);
        return;
    }
    if (nf && dec) {
        int32_t d = 1, k;
        for (k = 0; k < nf; k++)
            d *= 10;
        num_text(v / d, t);
        text_cat(t, ".", 12);
        num_text((v < 0 ? -v : v) % d, n);
        text_cat(t, n, 12);
    } else if (nf) {
        num_text(v >> nf, t);
    } else {
        num_text(v, t);
    }
    if (u->param[i].type == 1)
        text_cat(t, "%", 12);
}

/* the knobs of a section's page: its built-in parameters, or (a unit's TYPE) the unit's own */
typedef struct {
    const char *label;
    int32_t v, lo, hi;
    char text[14];
    uint8_t bip, used;
} knob_t;

static int pages(int s)
{
    const unit_info_t *u = type_unit(s);
    if (u) {
        int n = u->nparam - (s == S_OSC ? 0 : 0);
        return (n + 3) / 4 < 1 ? 1 : (n + 3) / 4;
    }
    return SECT[s].npages;
}

/* the unit's parameter on page / knob: the osc's first two are SHAPE and ALT (the patch's own) */
static int unit_index(int s, int page, int knob) { (void)s; return page * 4 + knob; }

static void knob(int s, int page, int i, knob_t *kb)
{
    const unit_info_t *u = type_unit(s);
    kb->used = 0;
    kb->bip = 0;
    if (u) {
        int j = unit_index(s, page, i);
        if (j >= u->nparam)
            return;
        kb->used = 1;
        kb->label = u->param[j].name;
        kb->lo = u->param[j].min;
        kb->hi = u->param[j].max;
        if (s == S_OSC && j < 2)
            kb->v = j ? set.p.alt : set.p.shape;
        else
            kb->v = set.p.up[SECT[s].place][j];
        kb->bip = kb->lo < 0 && kb->hi > 0;
        unit_value_text(u, j, kb->v, kb->text);
        return;
    }
    {
        const param_t *k = &SECT[s].k[page][i];
        if (!k->label)
            return;
        kb->used = 1;
        kb->label = k->label;
        kb->v = p_get(k);
        kb->lo = k->lo;
        kb->hi = k->hi;
        kb->bip = k->fmt == F_BIP || k->fmt == F_SEMI;
        value_text(k, kb->v, kb->text);
    }
}

static void knob_turn(int s, int page, int i, int32_t e, int fine)
{
    const unit_info_t *u = type_unit(s);
    if (u) {
        int j = unit_index(s, page, i);
        int32_t lo, hi, step, v;
        if (j >= u->nparam)
            return;
        lo = u->param[j].min;
        hi = u->param[j].max;
        step = fine ? 1 : (hi - lo) > 200 ? (hi - lo) / 128 : (hi - lo) > 40 ? 2 : 1;
        if (s == S_OSC && j < 2) {
            uint16_t *f = j ? &set.p.alt : &set.p.shape;
            *f = (uint16_t)clamp((int32_t)*f + e * (fine ? 1 : 8), 0, 1023);
        } else {
            v = clamp((int32_t)set.p.up[SECT[s].place][j] + e * step, lo, hi);
            set.p.up[SECT[s].place][j] = (int16_t)v;
        }
        return;
    }
    {
        const param_t *k = &SECT[s].k[page][i];
        if (k->label)
            p_put(k, p_get(k) + e * (fine ? 1 : k->step));
    }
}

static void changed(void)
{
    set_dirty = 1;
    ui.redraw = 1;
    dsp_patch(&set.p);
}

static void go_section(int s, int page)
{
    if (ui.sect != s)
        ui.erase_armed = 0;
    ui.sect = s;
    ui.page = page % pages(s);
    ui.redraw = 1;
    if (ann_on())
        ann_say(SECT[s].name, 0, ANN_NOW);
}

/* --------------------------------------------------------------------------------- the USB host --- */
static void usbh_toggle(void)
{
    int on = plat_usb_host_state() == USBH_OFF;
    plat_usb_host(on);
    app_say("USB HOST", on ? "LOOKING" : "OFF");
}
static void usbh_name(char *t, uint32_t max)
{
    const char *n = plat_usb_host_name();
    uint32_t i;
    for (i = 0; n[i] && i + 1u < max; i++)
        t[i] = n[i] >= 'a' && n[i] <= 'z' ? (char)(n[i] - 32) : n[i];
    t[i] = 0;
    if (!t[0])
        copy_text(t, "MIDI DEVICE", max);
}
static void usbh_service(void)
{
    char t[32];
    if (plat_usb_lost())
        dsp_panic();
    switch (plat_usb_host_msg()) {
    case USBH_MSG_ON: app_say("USB HOST", "PLUG IN A KEYBOARD"); break;
    case USBH_MSG_OFF: app_say("USB", "TO THE COMPUTER"); break;
    case USBH_MSG_MIDI: usbh_name(t, sizeof t); app_say(t, "CONNECTED"); break;
    case USBH_MSG_NOMIDI: app_say("USB DEVICE", "NOT MIDI"); break;
    case USBH_MSG_LOW: app_say("USB DEVICE", "LOW SPEED: NO"); break;
    case USBH_MSG_LOST: app_say("USB DEVICE", "UNPLUGGED"); break;
    default: break;
    }
}
static void usbh_mark(int32_t x, int32_t y, uint16_t on_c, uint16_t look_c)
{
    int st = plat_usb_host_state();
    if (st == USBH_OFF)
        return;
    cv_text(x, y, &FONT_T, "USB", st == USBH_CONNECTED ? on_c : look_c);
}

/* ------------------------------------------------------------------- units over USB (APP_USB_CMD) --- */
/* the host's commands, in the update's framing (fm1-base: F0 pack7(00 59 cmd len16 00 body ~sum) F7):
 *   0x70 LIST                      -> 0x78: per slot: used, module, format, name[20]
 *   0x71 PUT slot len32 crc32      the FM-1 reads the image from the host (its read requests, as an update's) into
 *                                  the slot (0xFF: the first free one) -> 0x79: status (0 done, else why), slot
 *   0x72 ERASE slot                -> 0x79 */
enum { UC_LIST = 0x70, UC_PUT = 0x71, UC_ERASE = 0x72, UC_LIST_R = 0x78, UC_RESULT = 0x79 };

static void uc_send(uint8_t cmd, const uint8_t *body, uint32_t n)
{
    static uint8_t raw[8 + 7u * 23u], out[16 + 7u * 27u];
    uint32_t i, sum = 0, acc = 0, nb = 0, k = 0;
    raw[0] = 0;
    raw[1] = 0x59;
    raw[2] = cmd;
    raw[3] = (uint8_t)n;
    raw[4] = (uint8_t)(n >> 8);
    raw[5] = 0;
    for (i = 0; i < n; i++) {
        raw[6 + i] = body[i];
        sum += body[i];
    }
    raw[6 + n] = (uint8_t)~sum;
    out[k++] = 0xF0;
    for (i = 0; i < 7u + n; i++) {                       /* pack7: little-endian bit stream, 7 bits a byte */
        acc |= (uint32_t)raw[i] << nb;
        nb += 8;
        while (nb >= 7) {
            out[k++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7;
        }
    }
    if (nb)
        out[k++] = (uint8_t)(acc & 0x7Fu);
    out[k++] = 0xF7;
    plat_sysex_send(out, k);
}

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void app_usb_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t r[7u * 23u];
    if (n < 7u)
        return;
    if (d[2] == UC_LIST) {
        int s, k;
        for (s = 0; s < NUNITS; s++) {
            const unit_info_t *u = dsp_unit(s);
            uint8_t *o = r + 23 * s;
            o[0] = u->used;
            o[1] = u->module;
            o[2] = u->fmt;
            for (k = 0; k < 20; k++)
                o[3 + k] = (uint8_t)(u->used ? u->name[k] : 0);
        }
        uc_send(UC_LIST_R, r, sizeof r);
    } else if (d[2] == UC_ERASE && n >= 8u) {
        int s = d[6];
        r[0] = (uint8_t)(dsp_unit_erase(s) ? 2 : 0);
        r[1] = (uint8_t)s;
        uc_send(UC_RESULT, r, 2);
        ui.redraw = 1;
    } else if (d[2] == UC_PUT && n >= 16u) {
        int s = d[6], rc = 0;
        uint32_t len = rd32le(d + 7), off;
        static uint8_t buf[512];
        if (s == 0xFF)
            for (s = 0; s < NUNITS && dsp_unit(s)->used; s++)
                ;
        if (s < 0 || s >= NUNITS)
            rc = 3;                                      /* the bank is full */
        else if (len < LG_HDR || len > 0x9000u)
            rc = 4;                                      /* too big for a slot */
        if (!rc) {
            app_say("UNIT", "RECEIVING");
            plat_host_patience(3000u, 120000u);
            if (dsp_unit_erase(s))
                rc = 2;
            for (off = 0; !rc && off < len; off += 512u) {
                uint32_t k = len - off < 512u ? len - off : 512u;
                if (plat_host_read(off, buf, k))
                    rc = 5;                              /* the host stopped answering */
                else if (plat_data_write(0x97000u + (uint32_t)s * 0x9000u + off, buf, k))
                    rc = 2;
            }
            if (!rc) {
                dsp_units_scan();
                rc = dsp_unit(s)->used ? 0 : 6;          /* not a unit we can run */
            }
            if (rc)
                dsp_unit_erase(s);
        }
        r[0] = (uint8_t)rc;
        r[1] = (uint8_t)s;
        uc_send(UC_RESULT, r, 2);
        if (!rc) {
            char t[24];
            copy_text(t, dsp_unit(s)->name, sizeof t);
            app_say(t, "INSTALLED");
            ui.sect = S_UNITS;
            ui.ucur = s;
        } else {
            app_say("UNIT", "NOT INSTALLED");
        }
        ui.redraw = 1;
    }
}

/* ----------------------------------------------------------------------------------------- input --- */
static void save_sound(void)
{
    int u = set.cur >= NFACTORY ? set.cur - NFACTORY : -1, i;
    char t[24], n[4];
    if (u < 0) {                                         /* a factory sound: into the first free user slot */
        for (i = 0; i < NUSER && (bank.used >> i & 1u); i++)
            ;
        u = i < NUSER ? i : NUSER - 1;
    }
    bank.p[u] = set.p;
    bank.used |= 1u << u;
    set.cur = (uint8_t)(NFACTORY + u);
    if (plat_store_save(OBJ_BANK, &bank, sizeof bank) == 0 && plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
        set_dirty = 0;
        copy_text(t, "SAVED U", sizeof t);
        num2(u + 1, n);
        text_cat(t, n, sizeof t);
        app_say(t, "");
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int fine = (btn >> B_GLO) & 1u, i;
    int32_t e;
    char t[24];
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {                        /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        int note = 53 + (int)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            if (!v)
                v = 100;
            dsp_key(note, 1, (int)v);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            dsp_key(note, 0, 0);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {                           /* presses */
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_EDIT: go_section(S_OSC, ui.sect == S_OSC ? ui.page + 1 : 0); break;
        case B_SEL: go_section(ui.sect == S_FLT ? S_FEG : S_FLT, 0); break;
        case B_ENV: go_section(ui.sect == S_EG ? S_FEG : S_EG, 0); break;
        case B_LFO: go_section(S_LFO, 0); break;
        case B_SEQ: go_section(S_VOICE, ui.sect == S_VOICE ? ui.page + 1 : 0); break;
        case B_FX: go_section(ui.sect == S_MOD ? S_DLY : ui.sect == S_DLY ? S_REV : S_MOD, 0); break;
        case B_REC: go_section(ui.sect, ui.page + 1); break;
        case B_PLAY: go_section(S_UNITS, 0); break;
        case B_ARP: ui.arp_t = now; ui.arp_held_done = 0; break;
        case B_SAVE: ui.save_t = now; break;
        case B_HOME:
            if (fine)
                usbh_toggle();
            else {
                dsp_panic();
                app_say("ALL NOTES", "OFF");
            }
            break;
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
        default: break;
        }
    }
    /* SAVE: a tap keeps the sound; on the UNITS page, held a second, erases the slot under the cursor */
    if ((btn >> B_SAVE) & 1u) {
        if (ui.sect == S_UNITS && dsp_unit(ui.ucur)->used && now - ui.save_t >= 1000u && !ui.erase_armed) {
            ui.erase_armed = 1;
            dsp_unit_erase(ui.ucur);
            fix_units();
            changed();
            app_say("UNIT", "ERASED");
        }
    } else if ((ui.btn >> B_SAVE) & 1u) {
        if (!ui.erase_armed)                             /* (a hold that erased a unit saves nothing) */
            save_sound();
        ui.erase_armed = 0;
    }
    if ((btn >> B_ARP) & 1u) {                           /* ARP held half a second: latch */
        if (!ui.arp_held_done && now - ui.arp_t >= 500u) {
            ui.arp_held_done = 1;
            ui.arp_latch ^= 1u;
            ui.arp_on = 1;
            dsp_arp(ui.arp_on, ui.arp_latch);
            go_section(S_ARP, 0);
            app_say("LATCH", ui.arp_latch ? "ON" : "OFF");
        }
    } else if ((ui.btn >> B_ARP) & 1u && !ui.arp_held_done) {
        ui.arp_on ^= 1u;
        if (!ui.arp_on)
            ui.arp_latch = 0;
        dsp_arp(ui.arp_on, ui.arp_latch);
        go_section(S_ARP, 0);
        app_say("ARP", ui.arp_on ? "ON" : "OFF");
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        go_section((ui.sect + (int)NS + (e > 0 ? 1 : -1)) % (int)NS, 0);
    if ((e = plat_enc(EN_PRESET)) != 0) {
        if (ui.sect == S_UNITS) {
            ui.ucur = clamp(ui.ucur + (e > 0 ? 1 : -1), 0, NUNITS - 1);
            ui.erase_armed = 0;
            ui.redraw = 1;
        } else {
            type_step(ui.sect, e > 0 ? 1 : -1);
            ui.page = 0;
            changed();
            popup(SECT[ui.sect].name, type_name(ui.sect), -2000, 0);
            if (ann_on())
                ann_say(type_name(ui.sect), 0, ANN_NOW);
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int c = clamp((int)set.cur + (e > 0 ? 1 : -1), 0, NFACTORY + NUSER - 1);
        if (c >= NFACTORY && !((bank.used >> (c - NFACTORY)) & 1u) && e > 0) {   /* past the last kept one: stop */
            int j;
            for (j = c; j < NFACTORY + NUSER && !((bank.used >> (j - NFACTORY)) & 1u); j++)
                ;
            c = j < NFACTORY + NUSER ? j : (int)set.cur;
        } else if (c >= NFACTORY && !((bank.used >> (c - NFACTORY)) & 1u) && e < 0) {
            int j;
            for (j = c; j >= NFACTORY && !((bank.used >> (j - NFACTORY)) & 1u); j--)
                ;
            c = j;
        }
        if (c != (int)set.cur) {
            load_sound(c);
            if (fix_units())
                app_say("UNIT MISSING", "ITS PLACE: BUILT-IN");
            else {
                sound_name(c, t, sizeof t);
                app_say("SOUND", t);
            }
            changed();
            set_dirty = 1;
        }
    }
    if (ui.sect != S_UNITS)
        for (i = 0; i < 4; i++) {
            knob_t kb;
            if ((e = plat_enc(EN_K1 + i)) == 0)
                continue;
            knob(ui.sect, ui.page, i, &kb);
            if (!kb.used)
                continue;
            knob_turn(ui.sect, ui.page, i, e, fine);
            changed();
            knob(ui.sect, ui.page, i, &kb);
            {
                int32_t span = kb.hi - kb.lo, bar = kb.bip ? (kb.v - (kb.lo + kb.hi) / 2) * 2000 / (span ? span : 1)
                                                           : (kb.v - kb.lo) * 1000 / (span ? span : 1);
                popup(kb.label, kb.text, bar, kb.bip);
            }
        }
    else
        for (i = 0; i < 4; i++)
            (void)plat_enc(EN_K1 + i);
    for (i = 0; i < 4; i++) {                            /* MIDI CCs into the sound (here, where the patch lives) */
        uint16_t c = cc_in[i];
        if (c) {
            static const uint16_t OFF[4] = {offsetof(patch_t, cutoff), offsetof(patch_t, reso),
                                            offsetof(patch_t, a), offsetof(patch_t, r)};
            cc_in[i] = 0;
            *(uint16_t *)(void *)((uint8_t *)&set.p + OFF[i]) = (uint16_t)((c - 1u) * 1023u / 127u);
            changed();
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {                                                    /* LEDs: the section's button, ARP on, SAVE unsaved */
        static const uint8_t SBTN[NS] = {B_EDIT, B_SEL, B_ENV, B_SEL, B_LFO, B_SEQ, B_FX, B_FX, B_FX, B_ARP, B_PLAY};
        uint32_t lit = btn | 1u << SBTN[ui.sect] | (ui.arp_on ? 1u << B_ARP : 0u) | (set_dirty ? 1u << B_SAVE : 0u);
        uint32_t klit = keys, vo = dsp_voices_on();
        int v;
        (void)vo;
        for (v = 0; v < NVOICE; v++) {                   /* every note sounding */
            int nn = dsp_voice_note(v) - 53 - 12 * set.octave;
            if (nn >= 0 && nn < NKEYS)
                klit |= 1u << nn;
        }
        if (pages(ui.sect) > 1)
            lit |= 1u << B_REC;
        plat_glow(1);
        plat_leds(lit, klit);
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------------------------------ draw --- */
static int sect_on(int s)
{
    switch (s) {
    case S_FLT: return set.p.flt_type != FLT_OFF;
    case S_MOD: return set.p.mod_type != MOD_OFF && (set.p.mod_type < MOD_N || type_unit(S_MOD));
    case S_DLY: return set.p.dly_type != DLY_OFF && (set.p.dly_type < DLY_N || type_unit(S_DLY));
    case S_REV: return set.p.rev_type != REV_OFF && (set.p.rev_type < REV_N || type_unit(S_REV));
    case S_ARP: return ui.arp_on;
    default: return 1;
    }
}

static uint16_t dim(uint16_t c, int k)   /* a colour k/8 as bright */
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    r = r * (uint32_t)k / 8u;
    g = g * (uint32_t)k / 8u;
    b = b * (uint32_t)k / 8u;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static struct {
    uint32_t pop_until, top_at, bot_at;
    char pop_name[24], pop_val[24];
    int32_t pop_bar;
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

static void draw_scope(uint16_t col)
{
    float buf[220], mx = 0.0f;
    int trig = dsp_scope(buf, 220);
    int32_t i;
    for (i = 0; i < 220; i += 20)
        cv_rect(10 + i, 66, 2, 1, C_LINE);
    for (i = 0; i < 220; i++)
        if (fm_fabsf(buf[i]) > mx)
            mx = fm_fabsf(buf[i]);
    vw.scale += (fm_clampf(38.0f / fm_maxf(mx, 0.02f), 30.0f, 900.0f) - vw.scale) * 0.3f;
    if (mx < 0.0005f) {
        cv_rect(10, 66, 220, 1, C_DIM);
        return;
    }
    {
        int32_t px = 10, py = 66 - (int32_t)fm_clampf(buf[0] * vw.scale, -44.0f, 44.0f);
        for (i = 1; i < 220; i++) {
            int32_t y = 66 - (int32_t)fm_clampf(buf[i] * vw.scale, -44.0f, 44.0f);
            cv_line(px, py, 10 + i, y, trig ? col : dim(col, 5));
            cv_line(px, py + 1, 10 + i, y + 1, trig ? col : dim(col, 5));
            px = 10 + i;
            py = y;
        }
    }
}

/* an envelope's shape: attack up, decay to sustain, a held stretch, release; the level now as a dot */
static void draw_env(uint16_t a, uint16_t d, uint16_t s, uint16_t r, float now, uint16_t col, int ad)
{
    float ta = 0.08f + 0.9f * (float)a / 1023.0f, td = 0.08f + 0.9f * (float)d / 1023.0f;
    float tr = 0.08f + 0.9f * (float)r / 1023.0f, sus = ad ? 0.0f : (float)s / 1023.0f, tot = ta + td + 0.6f + tr;
    int32_t x0 = 14, w = 212, yb = 106, h = 78, xa, xd, xs, xr, ys = yb - (int32_t)(sus * (float)h);
    xa = x0 + (int32_t)((float)w * ta / tot);
    xd = xa + (int32_t)((float)w * td / tot);
    xs = xd + (int32_t)((float)w * 0.6f / tot);
    xr = x0 + w;
    cv_rect(x0, yb, w, 1, C_LINE);
    cv_line(x0, yb, xa, yb - h, col);
    cv_line(xa, yb - h, xd, ys, col);
    cv_line(xd, ys, xs, ys, col);
    cv_line(xs, ys, xr, yb, col);
    cv_line(x0, yb - 1, xa, yb - h - 1, col);
    cv_line(xa, yb - h - 1, xd, ys - 1, col);
    cv_line(xd, ys - 1, xs, ys - 1, col);
    cv_line(xs, ys - 1, xr, yb - 1, col);
    cv_rect(6, yb - (int32_t)(now * (float)h) - 2, 5, 5, C_WHITE);
}

/* the filter's response: a curve from the cutoff and resonance (an analog-style sketch, not a measurement) */
static void draw_filter(uint16_t col)
{
    int32_t x, px = -1, py = 0;
    float fc = (float)set.p.cutoff / 1023.0f * 9.8f, q = (float)set.p.reso / 1023.0f;
    int type = set.p.flt_type, poles = (type & 1) ? 2 : 1;
    for (x = 0; x < 220; x++) {
        float o = (float)x / 220.0f * 10.0f, d = o - fc, g;    /* octaves from 20 Hz; d: from the cutoff */
        if (type >= FLT_OFF)
            g = 0.0f;
        else if (type / 2 == 0)
            g = d < 0.0f ? 0.0f : -12.0f * (float)poles * d;
        else if (type / 2 == 2)
            g = d > 0.0f ? 0.0f : 12.0f * (float)poles * d;
        else
            g = -6.0f * (float)poles * fm_fabsf(d);
        g += q * 14.0f * fm_expf(-d * d * 6.0f);
        {
            int32_t y = 30 - (int32_t)fm_clampf(g * 1.4f, -70.0f, 24.0f);
            if (px >= 0) {
                cv_line(px, py, 10 + x, y, col);
                cv_line(px, py + 1, 10 + x, y + 1, col);
            }
            px = 10 + x;
            py = y;
        }
    }
    cv_rect(10 + (int32_t)(fc / 10.0f * 220.0f), 18, 1, 90, C_DIM);
}

static void draw_lfo(uint16_t col)
{
    int32_t x, px = -1, py = 0;
    float v = dsp_lfo();
    for (x = 0; x < 220; x++) {
        float ph = (float)x / 220.0f, w;
        switch (set.p.lfo_type) {
        case LFO_SINE: w = fm_sin_turns(ph); break;
        case LFO_SAW: w = 1.0f - 2.0f * ph; break;
        case LFO_SQR: w = ph < 0.5f ? 1.0f : -1.0f; break;
        case LFO_SH: case LFO_DRIFT: w = fm_sin_turns(ph * 3.0f) * 0.4f + fm_sin_turns(ph * 7.0f + 0.3f) * 0.5f; break;
        default: w = 4.0f * fm_fabsf(ph - 0.5f) - 1.0f; break;
        }
        {
            int32_t y = 64 - (int32_t)(w * 36.0f);
            if (px >= 0)
                cv_line(px, py, 10 + x, y, col);
            px = 10 + x;
            py = y;
        }
    }
    cv_rect(228, 64 - (int32_t)(v * 36.0f) - 2, 6, 5, C_WHITE);
}

static void draw_arp(uint16_t col)
{
    int32_t i, n = set.p.arp_len, w = n > 12 ? 16 : 18, x0 = 120 - (n > 12 ? 12 : n) * w / 2;
    uint32_t cur = dsp_arp_step();
    for (i = 0; i < n; i++) {
        int32_t row = i / 12, col_i = i % 12, x = x0 + col_i * w, y = 40 + row * 26;
        int here = ui.arp_on && (uint32_t)(i + 1) % (uint32_t)n == cur % (uint32_t)n;
        cv_rect(x, y, w - 4, 20, here ? col : C_LINE);
    }
}

static void draw_voices_bar(int32_t x, int32_t y, uint16_t col)
{
    int v;
    for (v = 0; v < NVOICE; v++) {
        float l = dsp_voice_level(v);
        cv_rect(x + v * 6, y, 4, 7, l > 0.002f ? (l > 0.5f ? col : dim(col, 4)) : C_LINE);
    }
}

static const char *place_unit_word(int place)
{
    int err = 0, st = dsp_place_state(place, &err);
    return st < 0 ? "LOADING" : st == LG_FAULT ? lg_error(err) : "";
}

static void draw_top(void)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const section_t *s = &SECT[ui.sect];
    char t[32], o[12];
    int32_t note = dsp_note();
    uint32_t now = plat_ms();
    uint16_t col = s->rgb;
    cv_begin(240, 120, C_BLACK);
    /* the header: the sound, the voices, the note */
    sound_name(set.cur, t, sizeof t);
    if (set_dirty)
        text_cat(t, " *", sizeof t);
    cv_text(8, 3, &FONT_XS, t, C_GRAY);
    draw_voices_bar(150, 4, col);
    if (note >= 0) {
        copy_text(t, N[note % 12], sizeof t);
        num_text(note / 12 - 1, o);
        text_cat(t, o, sizeof t);
        cv_text(232 - text_w(&FONT_XS, t), 3, &FONT_XS, t, C_WHITE);
    }
    usbh_mark(118, 4, C_HI, C_GRAY);
    if (ui.arp_on)
        cv_text(118, 14, &FONT_XS, ui.arp_latch ? "ARP LATCH" : "ARP", C_AMB);
    cv_rect(0, 13, 240, 1, C_LINE);
    if (now < vw.pop_until || now < ui.msg_until) {     /* the value, large */
        const char *nm = now < vw.pop_until ? vw.pop_name : ui.msg[0], *vl = now < vw.pop_until ? vw.pop_val : ui.msg[1];
        cv_text(120 - text_w(&FONT_S, nm) / 2, 26, &FONT_S, nm, C_GRAY);
        cv_text(120 - text_w(&FONT_L, vl) / 2, 48, &FONT_L, vl, C_WHITE);
        if (now < vw.pop_until && vw.pop_bar >= -1000 && !(vw.pop_bar < 0 && !vw.pop_bip)) {
            cv_rect(30, 98, 180, 4, C_LINE);
            if (vw.pop_bip) {
                int32_t w = vw.pop_bar * 90 / 1000;
                cv_rect(w < 0 ? 120 + w : 120, 98, w < 0 ? -w : w, 4, col);
                cv_rect(119, 95, 2, 10, C_GRAY);
            } else {
                cv_rect(30, 98, vw.pop_bar * 180 / 1000, 4, col);
            }
        }
    } else {
        switch (ui.sect) {
        case S_FLT: draw_filter(col); break;
        case S_EG: draw_env(set.p.a, set.p.d, set.p.s, set.p.r, dsp_level(), col, set.p.eg_type != EG_ADSR); break;
        case S_FEG: draw_env(set.p.fa, set.p.fd, set.p.fs, set.p.fr, dsp_fenv(), col, set.p.feg_type != FEG_ADSR); break;
        case S_LFO: draw_lfo(col); break;
        case S_ARP: draw_arp(col); break;
        case S_UNITS: {
            int k = dsp_core2_load();
            copy_text(t, "CORE 2  ", sizeof t);
            num_text(k, o);
            text_cat(t, o, sizeof t);
            text_cat(t, "%", sizeof t);
            cv_text(10, 24, &FONT_S, "LOGUE UNITS", col);
            cv_text(10, 52, &FONT_XS, t, C_GRAY);
            cv_text(10, 70, &FONT_XS, "SEND .NTKDIGUNIT / .NTS1MKIIUNIT", C_DIM);
            cv_text(10, 84, &FONT_XS, "FROM THE FM-1 TOOL APP", C_DIM);
            cv_text(10, 98, &FONT_XS, "HOLD SAVE: ERASE THE SLOT", C_DIM);
            break;
        }
        default:
            draw_scope(col);
            if (s->place != 0xFF && type_raw(ui.sect) >= s->ntypes) {   /* a unit: its name, its state */
                const char *w = place_unit_word(s->place);
                cv_text(10, 18, &FONT_XS, type_name(ui.sect), col);
                if (w[0])
                    cv_text(230 - text_w(&FONT_XS, w), 18, &FONT_XS, w, RGB(255, 90, 80));
                else {
                    copy_text(t, "", sizeof t);
                    num_text(dsp_place_load(s->place), o);
                    text_cat(t, o, sizeof t);
                    text_cat(t, "%", sizeof t);
                    cv_text(230 - text_w(&FONT_XS, t), 18, &FONT_XS, t, C_DIM);
                }
            }
            break;
        }
    }
    cv_blit(0, 0);
}

static void draw_units_list(void)
{
    static const char *const MOD[5] = {"", "MOD", "DLY", "REV", "OSC"};
    int sl;
    for (sl = 0; sl < NUNITS; sl++) {
        const unit_info_t *u = dsp_unit(sl);
        int32_t y = 2 + sl * 12;
        char t[32];
        int here = sl == ui.ucur;
        if (here)
            cv_rect(4, y - 1, 232, 12, C_LINE);
        t[0] = (char)('1' + sl);
        t[1] = 0;
        cv_text(8, y, &FONT_XS, t, here ? C_WHITE : C_GRAY);
        if (!u->used) {
            cv_text(24, y, &FONT_XS, "--", C_DIM);
            continue;
        }
        cv_text(24, y, &FONT_XS, MOD[u->module <= 4 ? u->module : 0], SECT[u->module == 4 ? S_OSC : u->module == 1 ? S_MOD : u->module == 2 ? S_DLY : S_REV].rgb);
        cv_text(56, y, &FONT_XS, u->name, here ? C_WHITE : C_HI);
        cv_text(200, y, &FONT_XS, u->fmt == 2 ? "MKII" : "MK1", C_DIM);
    }
}

static void draw_bottom(void)
{
    const section_t *s = &SECT[ui.sect];
    char t[24], o[8];
    int32_t i;
    float vu = dsp_vu(), tm = (float)plat_ms() * 0.001f;
    uint16_t col = s->rgb;
    int np = pages(ui.sect);
    cv_begin(240, 120, C_BLACK);
    if (ui.sect == S_UNITS) {
        draw_units_list();
    } else {
        /* the title: the section, its TYPE, the page */
        cv_rect(0, 0, 4, 14, col);
        cv_text(8, 1, &FONT_XS, s->name, col);
        cv_text(84, 1, &FONT_XS, type_name(ui.sect), C_WHITE);
        if (np > 1) {
            num_text(ui.page + 1, t);
            text_cat(t, "/", sizeof t);
            num_text(np, o);
            text_cat(t, o, sizeof t);
            cv_text(232 - text_w(&FONT_XS, t), 1, &FONT_XS, t, C_GRAY);
        }
        for (i = 0; i < 4; i++) {
            knob_t kb;
            int32_t y = 17 + i * 17, w, span;
            knob(ui.sect, ui.page, i, &kb);
            if (!kb.used)
                continue;
            span = kb.hi - kb.lo;
            cv_text(10, y, &FONT_XS, kb.label, C_GRAY);
            cv_rect(96, y + 5, 90, 5, C_LINE);
            if (kb.bip) {
                int32_t mid = (kb.lo + kb.hi) / 2;
                w = span ? (kb.v - mid) * 90 / span : 0;
                cv_rect(w < 0 ? 141 + w : 141, y + 5, w < 0 ? -w : w, 5, col);
                cv_rect(140, y + 3, 2, 9, C_GRAY);
            } else {
                w = span ? (kb.v - kb.lo) * 90 / span : 0;
                cv_rect(96, y + 5, w, 5, col);
            }
            cv_text(232 - text_w(&FONT_XS, kb.text), y, &FONT_XS, kb.text, C_WHITE);
        }
    }
    {   /* the chain: OSC > FLT > EG > MOD > DLY > REV, lit where we are, dim when off, U for a unit */
        static const int CH[6] = {S_OSC, S_FLT, S_EG, S_MOD, S_DLY, S_REV};
        for (i = 0; i < 6; i++) {
            int sc = CH[i], here = sc == ui.sect || (sc == S_FLT && ui.sect == S_FEG), on = sect_on(sc);
            int32_t bx = 6 + i * 38;
            uint16_t c = SECT[sc].rgb;
            cv_rect(bx, 92, 34, 20, here ? c : on ? dim(c, 3) : C_LINE);
            cv_text(bx + (34 - text_w(&FONT_XS, SECT[sc].tag)) / 2, 94, &FONT_XS, SECT[sc].tag,
                    here ? C_BLACK : on ? C_WHITE : C_GRAY);
            if (SECT[sc].place != 0xFF && type_raw(sc) >= SECT[sc].ntypes)
                cv_rect(bx + 28, 93, 4, 4, here ? C_BLACK : C_WHITE);
        }
        if (vu > 0.004f) {                               /* the signal running through the chain */
            float b = fm_clampf(vu * 4.0f, 0.15f, 1.0f);
            for (i = 0; i < 8; i++) {
                float u = frac1(tm * 0.45f + (float)i / 8.0f);
                int32_t x = 6 + (int32_t)(u * 226.0f), sec = (x - 6) / 38;
                static const int CH2[6] = {S_OSC, S_FLT, S_EG, S_MOD, S_DLY, S_REV};
                int on = sec >= 6 || sect_on(CH2[sec]);
                cv_rect(x, 86, 3, 3, on ? (b > 0.6f ? C_WHITE : C_HI) : C_LINE);
            }
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
    touch_on(1);
    ui.redraw = 1;
}

void app_frame(void)
{
    int32_t note = dsp_note();
    uint32_t now = plat_ms(), vo = dsp_voices_on();
    int sounding = dsp_vu() > 0.004f;
    input();
    usbh_service();
    dsp_service();
    ann_tick(now);
    if (note != ui.last_note || vo != ui.last_voices) {
        ui.last_note = note;
        ui.last_voices = vo;
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
    } else if (sounding || now - vw.top_at < 400u || ui.sect == S_EG || ui.sect == S_FEG || ui.sect == S_LFO) {
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
    else if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "osc")) *got = set.p.osc_type;
    else if (!strcmp(what, "flt")) *got = set.p.flt_type;
    else if (!strcmp(what, "eg")) *got = set.p.eg_type;
    else if (!strcmp(what, "mod")) *got = set.p.mod_type;
    else if (!strcmp(what, "dly")) *got = set.p.dly_type;
    else if (!strcmp(what, "rev")) *got = set.p.rev_type;
    else if (!strcmp(what, "vmode")) *got = set.p.voice_mode;
    else if (!strcmp(what, "arp")) *got = ui.arp_on;
    else if (!strcmp(what, "latch")) *got = ui.arp_latch;
    else if (!strcmp(what, "cutoff")) *got = set.p.cutoff;
    else if (!strcmp(what, "factory")) *got = set.cur;
    else if (!strcmp(what, "note")) *got = dsp_note();
    else if (!strcmp(what, "voices")) *got = __builtin_popcount(dsp_voices_on());
    else if (!strcmp(what, "steps")) *got = (int)dsp_arp_steps();
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "units")) {
        int s, n = 0;
        for (s = 0; s < NUNITS; s++)
            n += dsp_unit(s)->used;
        *got = n;
    } else if (!strcmp(what, "unit_osc")) {
        int err = 0;
        *got = dsp_place_state(US_OSC, &err) == LG_READY ? 1 : -err;
    } else if (!strcmp(what, "unit_mod")) {
        int err = 0;
        *got = dsp_place_state(US_MOD, &err) == LG_READY ? 1 : -err;
    } else if (!strcmp(what, "unit_dly")) {
        int err = 0;
        *got = dsp_place_state(US_DLY, &err) == LG_READY ? 1 : -err;
    } else if (!strcmp(what, "unit_rev")) {
        int err = 0;
        *got = dsp_place_state(US_REV, &err) == LG_READY ? 1 : -err;
    } else if (!strcmp(what, "loud")) *got = dsp_peak() > 0.02f;
    else return 0;
    return 1;
}
#endif
