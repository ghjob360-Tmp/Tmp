/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1's panel: pages of rows of four parameters, as Vital's sections cut to an FM-1.
 *   SELECT a page, ALGORITHM a row, K1..K4 its four (GLO held: fine), PRESET a factory sound
 *   SEL OSC A / OSC B   EDIT FILTER   ENV ENV1 > 2 > 3   LFO LFO1 / LFO2   ARP MATRIX   FX FX (again: next effect)
 *   HOME VOICE   SAVE keep the sound   REC all notes off   OCT- / OCT+ the octave
 * Part of the unity build after the platform; the sound is in synth.c and wt.c (o2 units). */
#include <stddef.h>
#include "fastmath.h"
#include "synth.h"

/* Vital's look: a deep blue-grey ground, teal for values, purple for modulation (gfx.c's palette replaced here) */
#undef C_HI
#define C_HI RGB(64, 224, 196)
#undef C_DIM
#define C_DIM RGB(58, 62, 78)
#undef C_LINE
#define C_LINE RGB(38, 40, 52)
#undef C_GRAY
#define C_GRAY RGB(150, 156, 176)
#define C_MOD RGB(176, 132, 255)
#define C_BG RGB(22, 23, 30)

/* ------------------------------------------------------------- the patch --- */
#define SET_MAGIC 0x50524D31u            /* "PRM1" */
typedef struct {
    uint32_t magic;
    patch_t p;
    int8_t octave;
    uint8_t factory, rsv[2];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

static void osc_init(osc_p *o, int table, int frame, int level)
{
    o->table = (uint16_t)table;
    o->frame = (uint16_t)frame;
    o->warp = WARP_NONE;
    o->warp_amt = 512;
    o->morph = SM_NONE;
    o->morph_amt = 512;
    o->level = (uint16_t)level;
    o->dest = DEST_FILTER;
    o->unison = 1;
    o->detune = 300;
    o->spread = 700;
    o->transpose = 24;
}
static void env_init(env_p *e, int a, int d, int s, int r)
{
    e->delay = 0;
    e->attack = (uint16_t)a;
    e->hold = 0;
    e->decay = (uint16_t)d;
    e->sustain = (uint16_t)s;
    e->release = (uint16_t)r;
    e->apow = 512;
    e->drpow = 384;                      /* Vital's decay and release power of -2 */
}
static void mod_set(patch_t *p, int i, int src, int dst, int amt)   /* amt -100..100 */
{
    p->mod[i].src = (uint16_t)src;
    p->mod[i].dst = (uint16_t)dst;
    p->mod[i].amt = (uint16_t)(512 + amt * 511 / 100);
}

static const char *const FACTORY_NAME[] = {"INIT", "SUPERSAW", "WARP BASS", "VOWEL PAD", "FM BELL", "PLUCK",
                                           "WOBBLE", "ORGAN"};
#define NFACTORY 8

static void factory(patch_t *p, int n)
{
    int i, e;
    uint32_t w;
    for (w = 0; w < sizeof *p / 2u; w++)
        ((uint16_t *)(void *)p)[w] = 0;
    /* INIT: Vital's: a saw (the basic table's last frame), the analog filter, its envelope times */
    osc_init(&p->osc[0], WT_BASIC, 1023, 724);
    osc_init(&p->osc[1], WT_BASIC, 0, 0);
    p->f_model = FM_ANALOG;
    p->f_style = 0;
    p->cutoff = 760;
    p->reso = 250;
    p->blend = 0;
    p->keytrack = 300;
    for (e = 0; e < 3; e++)
        env_init(&p->env[e], 76, 511, 1023, 280);
    for (i = 0; i < 2; i++) {
        p->lfo[i].shape = LS_SINE;
        p->lfo[i].rate = 450;
        p->lfo[i].mode = LM_TRIGGER;
        p->lfo[i].smooth = 0;
    }
    for (i = 0; i < NMOD; i++)
        p->mod[i].amt = 512;
    p->poly = NVOICES;
    p->glide = 0;
    p->veltrack = 700;
    p->volume = 600;
    p->ch_rate = 300; p->ch_depth = 600; p->ch_mix = 500;
    p->cp_amt = 500; p->cp_mix = 1023;
    p->dl_type = DLY_OFF; p->dl_time = 520; p->dl_fb = 400; p->dl_mix = 300;
    p->ds_type = DIST_OFF; p->ds_drive = 400; p->ds_mix = 1023;
    p->fl_rate = 200; p->fl_fb = 760; p->fl_mix = 500;
    p->ph_rate = 200; p->ph_fb = 700; p->ph_mix = 500;
    p->rv_time = 600; p->rv_damp = 400; p->rv_mix = 300;
    switch (n) {
    case 1:                              /* SUPERSAW */
        p->osc[0].unison = 3; p->osc[0].detune = 520; p->osc[0].spread = 1023;
        osc_init(&p->osc[1], WT_BASIC, 1023, 500);
        p->osc[1].unison = 3; p->osc[1].detune = 380; p->osc[1].transpose = 36; p->osc[1].spread = 900;
        p->cutoff = 820; env_init(&p->env[0], 120, 511, 900, 420);
        p->ch_on = 1; p->rv_on = 1; p->rv_mix = 350;
        break;
    case 2:                              /* WARP BASS */
        osc_init(&p->osc[0], WT_PULSE, 300, 900);
        p->osc[0].warp = WARP_SQUEEZE; p->osc[0].warp_amt = 300; p->osc[0].transpose = 12;
        osc_init(&p->osc[1], WT_BASIC, 0, 600); p->osc[1].transpose = 12;
        p->poly = 1; p->glide = 180;
        p->f_model = FM_LADDER; p->f_style = 1; p->cutoff = 420; p->reso = 550;
        env_init(&p->env[1], 0, 330, 0, 200);
        mod_set(p, 0, SRC_ENV2, DST_CUTOFF, 55);
        mod_set(p, 1, SRC_ENV2, DST_A_WARP, 40);
        p->ds_type = DIST_SOFT; p->ds_drive = 300;
        break;
    case 3:                              /* VOWEL PAD */
        osc_init(&p->osc[0], WT_FORMANT, 0, 800);
        p->osc[0].unison = 3; p->osc[0].detune = 350; p->osc[0].morph = SM_SMEAR; p->osc[0].morph_amt = 450;
        osc_init(&p->osc[1], WT_HARMONIC, 300, 450); p->osc[1].transpose = 36;
        env_init(&p->env[0], 450, 700, 850, 650);
        p->lfo[0].rate = 250; p->lfo[0].shape = LS_TRI;
        mod_set(p, 0, SRC_LFO1, DST_A_FRAME, 60);
        mod_set(p, 1, SRC_LFO2, DST_B_FRAME, 40);
        p->lfo[1].rate = 180; p->lfo[1].mode = LM_FREE; p->lfo[1].shape = LS_SMOOTH; p->lfo[1].smooth = 300;
        p->cutoff = 900;
        p->ch_on = 1; p->rv_on = 1; p->rv_time = 850; p->rv_mix = 500;
        break;
    case 4:                              /* FM BELL */
        osc_init(&p->osc[0], WT_BASIC, 0, 800);
        p->osc[0].warp = WARP_FM; p->osc[0].warp_amt = 450;
        osc_init(&p->osc[1], WT_BASIC, 0, 0); p->osc[1].transpose = 43;   /* +19: a twelfth */
        p->f_model = FM_OFF;
        env_init(&p->env[0], 10, 760, 0, 650);
        env_init(&p->env[1], 0, 600, 0, 500);
        mod_set(p, 0, SRC_ENV2, DST_A_WARP, 45);
        mod_set(p, 1, SRC_VEL, DST_A_WARP, 30);
        p->dl_type = DLY_PINGPONG; p->rv_on = 1; p->rv_mix = 400;
        break;
    case 5:                              /* PLUCK */
        osc_init(&p->osc[0], WT_DIGITAL, 200, 800);
        p->osc[0].morph = SM_LOWPASS; p->osc[0].morph_amt = 300;
        osc_init(&p->osc[1], WT_PULSE, 600, 400); p->osc[1].transpose = 12;
        p->cutoff = 380; p->reso = 400;
        env_init(&p->env[0], 0, 560, 0, 420);
        env_init(&p->env[1], 0, 380, 0, 300);
        mod_set(p, 0, SRC_ENV2, DST_CUTOFF, 70);
        mod_set(p, 1, SRC_NOTE, DST_A_FRAME, 50);
        p->dl_type = DLY_PINGPONG; p->dl_time = 420; p->dl_fb = 520;
        p->rv_on = 1;
        break;
    case 6:                              /* WOBBLE */
        osc_init(&p->osc[0], WT_HARMONIC, 400, 900);
        p->osc[0].warp = WARP_SYNC; p->osc[0].warp_amt = 200; p->osc[0].unison = 2; p->osc[0].detune = 250;
        osc_init(&p->osc[1], WT_BASIC, 680, 700); p->osc[1].transpose = 12;
        p->poly = 1;
        p->f_model = FM_LADDER; p->f_style = 1; p->cutoff = 380; p->reso = 650;
        p->lfo[0].shape = LS_SINE; p->lfo[0].rate = 560; p->lfo[0].mode = LM_FREE;
        mod_set(p, 0, SRC_LFO1, DST_CUTOFF, 45);
        mod_set(p, 1, SRC_LFO1, DST_A_WARP, 30);
        mod_set(p, 2, SRC_WHEEL, DST_LFO1_RATE, 60);
        p->ds_type = DIST_SINFOLD; p->ds_drive = 350; p->ds_mix = 600;
        break;
    case 7:                              /* ORGAN */
        osc_init(&p->osc[0], WT_ORGAN, 450, 850);
        p->f_model = FM_OFF;
        env_init(&p->env[0], 30, 511, 1023, 150);
        p->ch_on = 1; p->ch_rate = 520; p->ch_depth = 450;
        p->dl_type = DLY_OFF; p->rv_on = 1; p->rv_mix = 250;
        p->ds_type = DIST_SOFT; p->ds_drive = 200; p->ds_mix = 500;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- wavetables --- */
static wt_t wt_buf[2][2] PLAT_POOL;      /* per oscillator: the one playing and the one being built */
static struct {
    int cur, built;
    uint16_t table, morph, amt;
    uint32_t at;
} wtst[2];

static void tables_update(int force)
{
    int o;
    uint32_t now = plat_ms();
    for (o = 0; o < 2; o++) {
        const osc_p *op = &set.p.osc[o];
        if (!force && wtst[o].built && op->table == wtst[o].table && op->morph == wtst[o].morph
            && op->morph_amt == wtst[o].amt)
            continue;
        if (!force && now - wtst[o].at < 40u)   /* at most 25 builds a second */
            continue;
        {
            int b = wtst[o].built ? wtst[o].cur ^ 1 : 0;
            wt_build(&wt_buf[o][b], op->table % WT_N, op->morph % SM_N, (float)op->morph_amt * (1.0f / 1023.0f));
            synth_tables(o, &wt_buf[o][b]);
            wtst[o].cur = b;
            wtst[o].built = 1;
            wtst[o].table = op->table;
            wtst[o].morph = op->morph;
            wtst[o].amt = op->morph_amt;
            wtst[o].at = now;
        }
    }
}

/* -------------------------------------------------------------- the pages --- */
enum { F_PCT, F_BIP, F_ENUM, F_INT, F_SEMI, F_TIME, F_HZ, F_CUT, F_BLEND, F_STYLE, F_MS, F_ONOFF };
typedef struct {
    const char *label;
    uint16_t off;
    uint8_t fmt;
    int16_t lo, hi, step;
    const char *const *names;
} param_t;
typedef struct {
    const char *name;
    param_t k[4];
} row_t;
typedef struct {
    const char *name, *tab;
    uint8_t nrows, kind;
    const row_t *rows;
} page_t;
enum { K_OSC, K_FILTER, K_ENV, K_LFO, K_MATRIX, K_FX, K_VOICE };

static const char *const WT_T[] = {"BASIC", "PULSE", "FORMANT", "HARMONIC", "ORGAN", "DIGITAL"};
static const char *const WARP_T[] = {"NONE", "SYNC", "FORMANT", "QUANTIZE", "BEND", "SQUEEZE", "PULSE", "FM", "RM"};
static const char *const SM_T[] = {"NONE", "LOW PASS", "HIGH PASS", "FORMANT", "STRETCH", "SMEAR", "RANDOM",
                                   "DISPERSE", "SHEPARD"};
static const char *const DEST_T[] = {"FILTER", "DIRECT"};
static const char *const FM_T[] = {"ANALOG", "LADDER", "FORMANT", "COMB", "OFF"};
static const char *const LS_T[] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "S&H", "SMOOTH"};
static const char *const LM_T[] = {"TRIGGER", "FREE", "ENVELOPE"};
static const char *const SRC_T[] = {"--", "ENV1", "ENV2", "ENV3", "LFO1", "LFO2", "VELOCITY", "NOTE", "RANDOM",
                                    "MOD WHEEL", "PRESSURE"};
static const char *const DST_T[] = {"--", "A FRAME", "A WARP", "A PITCH", "A LEVEL", "B FRAME", "B WARP", "B PITCH",
                                    "B LEVEL", "CUTOFF", "RESONANCE", "BLEND", "DRIVE", "NOISE", "PAN", "LFO1 RATE",
                                    "LFO2 RATE", "VOLUME"};
static const char *const DIST_T[] = {"OFF", "SOFT CLIP", "HARD CLIP", "LIN FOLD", "SIN FOLD", "BIT CRUSH", "DOWNSAMPLE"};
static const char *const DLY_T[] = {"OFF", "MONO", "STEREO", "PING PONG"};
static const char *const ONOFF_T[] = {"OFF", "ON"};

#define O(f) offsetof(patch_t, f)
#define PCT(f, l) {l, O(f), F_PCT, 0, 1023, 8, 0}
#define BIPP(f, l) {l, O(f), F_BIP, 0, 1023, 8, 0}
#define EN(f, l, t, n) {l, O(f), F_ENUM, 0, (n) - 1, 1, t}
#define ONF(f, l) {l, O(f), F_ONOFF, 0, 1, 1, ONOFF_T}
#define TM(f, l) {l, O(f), F_TIME, 0, 1023, 8, 0}
#define NONE {0, 0, 0, 0, 0, 0, 0}

#define OSC_ROWS(i)                                                                                            \
    {{"WAVE", {EN(osc[i].table, "TABLE", WT_T, WT_N), PCT(osc[i].frame, "FRAME"),                             \
               EN(osc[i].warp, "WARP", WARP_T, WARP_N), PCT(osc[i].warp_amt, "WARP AMT")}},                   \
     {"SPECTRUM", {EN(osc[i].morph, "MORPH", SM_T, SM_N), PCT(osc[i].morph_amt, "MORPH AMT"),                 \
                   PCT(osc[i].level, "LEVEL"), EN(osc[i].dest, "OUT", DEST_T, DEST_N)}},                      \
     {"UNISON", {{"VOICES", O(osc[i].unison), F_INT, 1, 3, 1, 0}, PCT(osc[i].detune, "DETUNE"),                \
                 PCT(osc[i].spread, "SPREAD"), {"TRANSPOSE", O(osc[i].transpose), F_SEMI, 0, 48, 1, 0}}}}
static const row_t R_OSCA[] = OSC_ROWS(0);
static const row_t R_OSCB[] = OSC_ROWS(1);
static const row_t R_FILT[] = {
    {"FILTER", {EN(f_model, "MODEL", FM_T, FM_N), {"STYLE", O(f_style), F_STYLE, 0, 1, 1, 0},
                {"CUTOFF", O(cutoff), F_CUT, 0, 1023, 8, 0}, PCT(reso, "RESONANCE")}},
    {"SHAPE", {PCT(drive, "DRIVE"), {"BLEND", O(blend), F_BLEND, 0, 1023, 8, 0}, PCT(keytrack, "KEYTRACK"),
               PCT(noise, "NOISE")}}};
#define ENV_ROWS(i)                                                                                            \
    {{"ADSR", {TM(env[i].attack, "ATTACK"), TM(env[i].decay, "DECAY"), PCT(env[i].sustain, "SUSTAIN"),        \
               TM(env[i].release, "RELEASE")}},                                                               \
     {"DELAY HOLD CURVE", {TM(env[i].delay, "DELAY"), TM(env[i].hold, "HOLD"), BIPP(env[i].apow, "ATK CURVE"), \
                           BIPP(env[i].drpow, "D/R CURVE")}}}
static const row_t R_ENV1[] = ENV_ROWS(0);
static const row_t R_ENV2[] = ENV_ROWS(1);
static const row_t R_ENV3[] = ENV_ROWS(2);
#define LFO_ROWS(i)                                                                                            \
    {{"LFO", {EN(lfo[i].shape, "SHAPE", LS_T, LS_N), {"RATE", O(lfo[i].rate), F_HZ, 0, 1023, 8, 0},           \
              EN(lfo[i].mode, "MODE", LM_T, LM_N), PCT(lfo[i].smooth, "SMOOTH")}}}
static const row_t R_LFO1[] = LFO_ROWS(0);
static const row_t R_LFO2[] = LFO_ROWS(1);
#define SLOT(i, n)                                                                                             \
    {n, {EN(mod[i].src, "SOURCE", SRC_T, SRC_N), EN(mod[i].dst, "DEST", DST_T, DST_N),                        \
         BIPP(mod[i].amt, "AMOUNT"), NONE}}
static const row_t R_MAT[] = {SLOT(0, "SLOT 1"), SLOT(1, "SLOT 2"), SLOT(2, "SLOT 3"), SLOT(3, "SLOT 4"),
                              SLOT(4, "SLOT 5"), SLOT(5, "SLOT 6"), SLOT(6, "SLOT 7"), SLOT(7, "SLOT 8")};
static const row_t R_FX[] = {
    {"CHORUS", {ONF(ch_on, "ON"), PCT(ch_rate, "RATE"), PCT(ch_depth, "DEPTH"), PCT(ch_mix, "MIX")}},
    {"COMPRESSOR", {ONF(cp_on, "ON"), PCT(cp_amt, "AMOUNT"), PCT(cp_mix, "MIX"), NONE}},
    {"DELAY", {EN(dl_type, "TYPE", DLY_T, DLY_N), {"TIME", O(dl_time), F_MS, 0, 1023, 8, 0}, PCT(dl_fb, "FEEDBACK"),
               PCT(dl_mix, "MIX")}},
    {"DISTORTION", {EN(ds_type, "TYPE", DIST_T, DIST_N), PCT(ds_drive, "DRIVE"), PCT(ds_mix, "MIX"), NONE}},
    {"FLANGER", {ONF(fl_on, "ON"), PCT(fl_rate, "RATE"), BIPP(fl_fb, "FEEDBACK"), PCT(fl_mix, "MIX")}},
    {"PHASER", {ONF(ph_on, "ON"), PCT(ph_rate, "RATE"), BIPP(ph_fb, "FEEDBACK"), PCT(ph_mix, "MIX")}},
    {"REVERB", {ONF(rv_on, "ON"), PCT(rv_time, "SIZE"), PCT(rv_damp, "DAMPING"), PCT(rv_mix, "MIX")}}};
static const row_t R_VOICE[] = {
    {"VOICE", {{"POLYPHONY", O(poly), F_INT, 1, NVOICES, 1, 0}, PCT(glide, "GLIDE"), PCT(veltrack, "VEL TRACK"),
               PCT(volume, "VOLUME")}}};

#define NR(a) ((uint8_t)(sizeof a / sizeof a[0]))
enum { PG_OSCA, PG_OSCB, PG_FILT, PG_ENV1, PG_ENV2, PG_ENV3, PG_LFO1, PG_LFO2, PG_MAT, PG_FX, PG_VOICE, NPAGES };
static const page_t PAGES[NPAGES] = {
    {"OSC A", "A", NR(R_OSCA), K_OSC, R_OSCA},       {"OSC B", "B", NR(R_OSCB), K_OSC, R_OSCB},
    {"FILTER", "F", NR(R_FILT), K_FILTER, R_FILT},   {"ENV 1 AMP", "E1", NR(R_ENV1), K_ENV, R_ENV1},
    {"ENV 2", "E2", NR(R_ENV2), K_ENV, R_ENV2},      {"ENV 3", "E3", NR(R_ENV3), K_ENV, R_ENV3},
    {"LFO 1", "L1", NR(R_LFO1), K_LFO, R_LFO1},      {"LFO 2", "L2", NR(R_LFO2), K_LFO, R_LFO2},
    {"MATRIX", "M", NR(R_MAT), K_MATRIX, R_MAT},     {"EFFECTS", "FX", NR(R_FX), K_FX, R_FX},
    {"VOICE", "V", NR(R_VOICE), K_VOICE, R_VOICE}};

static uint16_t *pv(const param_t *k) { return (uint16_t *)(void *)((uint8_t *)&set.p + k->off); }

/* the matrix destination a parameter is, and how its units read on the parameter's bar (Vital: every knob a target) */
static const float MOD_SCALE[DST_N] = {0, 1, 1, 24, 1, 1, 1, 24, 1, 64, 1, 2, 1, 1, 1, 1, 1, 1};   /* synth.c's */
static int dst_of(const param_t *k, float *per_unit)
{
    static const struct { uint16_t off; uint8_t dst; float scale; } MAP[] = {
        {O(osc[0].frame), DST_A_FRAME, 1.0f},  {O(osc[0].warp_amt), DST_A_WARP, 1.0f},
        {O(osc[0].transpose), DST_A_PITCH, 1.0f / 48.0f}, {O(osc[0].level), DST_A_LEVEL, 1.0f},
        {O(osc[1].frame), DST_B_FRAME, 1.0f},  {O(osc[1].warp_amt), DST_B_WARP, 1.0f},
        {O(osc[1].transpose), DST_B_PITCH, 1.0f / 48.0f}, {O(osc[1].level), DST_B_LEVEL, 1.0f},
        {O(cutoff), DST_CUTOFF, 1.0f / 128.0f}, {O(reso), DST_RESO, 1.0f}, {O(blend), DST_BLEND, 0.5f},
        {O(drive), DST_DRIVE, 1.0f}, {O(noise), DST_NOISE, 1.0f}, {O(lfo[0].rate), DST_LFO1_RATE, 4.0f / 10.55f},
        {O(lfo[1].rate), DST_LFO2_RATE, 4.0f / 10.55f}, {O(volume), DST_VOLUME, 1.0f}};
    uint32_t i;
    for (i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (MAP[i].off == k->off) {
            if (per_unit)
                *per_unit = MAP[i].scale;
            return MAP[i].dst;
        }
    return -1;
}
static float mod_reach(int dst, float per_unit)   /* how far the matrix can move it, in bar fractions */
{
    float r = 0.0f;
    int i;
    for (i = 0; i < NMOD; i++)
        if (set.p.mod[i].src && set.p.mod[i].dst == (uint16_t)dst)
            r += fm_fabsf(((float)set.p.mod[i].amt - 512.0f) / 511.0f) * MOD_SCALE[dst] * per_unit;
    return r;
}
static int src_of_page(int page)          /* the page's own modulation source, if it is one */
{
    switch (page) {
    case 3: return SRC_ENV1;
    case 4: return SRC_ENV2;
    case 5: return SRC_ENV3;
    case 6: return SRC_LFO1;
    case 7: return SRC_LFO2;
    default: return SRC_NONE;
    }
}

/* ---------------------------------------------------------- MIDI and sound --- */
static volatile uint16_t cc_in[2];       /* CC 74 cutoff, 71 resonance (+1; 0 none) */
static void midi_in(void)                /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if ((st & 0xF0u) == 0xB0u && (d1 == 74u || d1 == 71u))
            cc_in[d1 == 74u ? 0 : 1] = (uint16_t)(d2 + 1u);
        else if (st >= 0x80u && st < 0xF0u)
            PLAT_FAR(synth_midi)(st, d1, d2);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    PLAT_FAR(synth_render)(lr, n, (float)master_q12 / 4096.0f * 8300000.0f);
}
void app_panic(void) { synth_panic(); }
void app_overload(void) { synth_overload(); }

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2) {
        set.magic = SET_MAGIC;
        factory(&set.p, 0);
        set.octave = 0;
        set.factory = 0;
    }
    synth_init();
    tables_update(1);
    synth_patch(&set.p);
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    int page, row[NPAGES];
    char msg[2][24];
    uint32_t msg_until, keys, btn, cpu_at;
    uint8_t redraw, last_press;
    uint32_t cpu, voices;
    int mod_src;                         /* a modulation source picked up (Vital's drag), SRC_NONE when not */
    uint32_t mod_at, top_at, bot_at;
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
static void dec_text(int32_t v10, char *b)   /* tenths: 12 -> "1.2" */
{
    char n[12];
    num_text(v10 / 10, b);
    text_cat(b, ".", 16);
    num_text(v10 % 10, n);
    text_cat(b, n, 16);
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
    float x = (float)v * (1.0f / 1023.0f);
    t[0] = 0;
    switch (k->fmt) {
    case F_BIP: num_text((v - 512) * 100 / 511, t); break;
    case F_ENUM: case F_ONOFF: copy_text(t, k->names[v], 16); break;
    case F_INT: num_text(v, t); break;
    case F_SEMI: if (v > 24) copy_text(t, "+", 16); { char n[8]; num_text(v - 24, n); text_cat(t, n, 16); } break;
    case F_TIME: {                       /* (v * 2)^4 s */
        float s = x * 2.0f;
        s *= s;
        s *= s;
        if (s < 1.0f) {
            num_text((int32_t)(s * 1000.0f), t);
            text_cat(t, "ms", 16);
        } else {
            dec_text((int32_t)(s * 10.0f), t);
            text_cat(t, "s", 16);
        }
        break;
    }
    case F_HZ: dec_text((int32_t)(0.02f * fm_exp2f(x * 10.55f) * 10.0f), t); text_cat(t, "Hz", 16); break;
    case F_CUT: {
        float hz = 440.0f * fm_exp2f((8.0f + 128.0f * x - 69.0f) / 12.0f);
        if (hz >= 1000.0f) {
            dec_text((int32_t)(hz / 100.0f), t);
            text_cat(t, "k", 16);
        } else {
            num_text((int32_t)hz, t);
        }
        break;
    }
    case F_BLEND:
        copy_text(t, x < 0.25f ? "LOW " : x < 0.75f ? "BAND " : "HIGH ", 16);
        { char n[8]; num_text(v * 100 / 1023, n); text_cat(t, n, 16); }
        break;
    case F_STYLE:
        copy_text(t, set.p.f_model == FM_FORMANT ? (v ? "AIUO" : "AOIE")
                   : set.p.f_model == FM_COMB    ? (v ? "NEG" : "POS")
                                                 : (v ? "24 dB" : "12 dB"), 16);
        break;
    case F_MS: num_text((int32_t)(20.0f + 480.0f * x), t); text_cat(t, "ms", 16); break;
    default: num_text(v * 100 / 1023, t); break;
    }
}

static void changed(void)
{
    set_dirty = 1;
    ui.redraw = 1;
    synth_patch(&set.p);
}

static void go_page(int p)
{
    ui.page = p;
    ui.redraw = 1;
    if (ann_on())
        ann_say(PAGES[p].name, 0, ANN_NOW);
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, maxp = 0;
    int fine = (btn >> B_GLO) & 1u, i;
    int32_t e;
    char t[16];
    const page_t *pg = &PAGES[ui.page];
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {        /* key 0 is F3 (MIDI 53); velocity from the keys */
        uint32_t bit = 1u << k;
        int note = 53 + (int)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            synth_key(note, (int)(v ? v : 100u));
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | (v ? v : 100u) << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            synth_key(note, 0);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
        if ((keys & bit) && touch_pressure(k) > maxp)
            maxp = touch_pressure(k);
    }
    if (maxp != ui.last_press) {         /* the hardest-pressed key: PRESSURE in the matrix */
        ui.last_press = (uint8_t)maxp;
        synth_pressure((float)maxp * (1.0f / 127.0f));
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_SEL: go_page(ui.page == PG_OSCA ? PG_OSCB : PG_OSCA); break;
        case B_EDIT: go_page(PG_FILT); break;
        case B_ENV: go_page(ui.page == PG_ENV1 ? PG_ENV2 : ui.page == PG_ENV2 ? PG_ENV3 : PG_ENV1); break;
        case B_LFO: go_page(ui.page == PG_LFO1 ? PG_LFO2 : PG_LFO1); break;
        case B_ARP:
            if (ui.mod_src) {            /* put it down */
                ui.mod_src = SRC_NONE;
                app_say("MOD", "DONE");
            } else if (src_of_page(ui.page)) {   /* picked up: now turn any knob it should move */
                ui.mod_src = src_of_page(ui.page);
                ui.mod_at = plat_ms();
                app_say(SRC_T[ui.mod_src], "> TURN A KNOB");
            } else {
                go_page(PG_MAT);
            }
            break;
        case B_FX:
            if (ui.page == PG_FX)
                ui.row[PG_FX] = (ui.row[PG_FX] + 1) % (int)PAGES[PG_FX].nrows;
            go_page(PG_FX);
            break;
        case B_HOME: go_page(PG_VOICE); break;
        case B_REC: synth_panic(); app_say("ALL NOTES", "OFF"); break;
        case B_OCTDN: case B_OCTUP: {
            int o = clamp(set.octave + (k == B_OCTUP ? 1 : -1), -2, 2);
            if (o != set.octave) {
                synth_panic();
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
                app_say("SAVED", FACTORY_NAME[set.factory % NFACTORY]);
            } else {
                app_say("SAVE", "FAILED");
            }
            break;
        default: break;
        }
    }
    pg = &PAGES[ui.page];
    if ((e = plat_enc(EN_SELECT)) != 0)
        go_page((ui.page + NPAGES + (e > 0 ? 1 : -1)) % NPAGES);
    if ((e = plat_enc(EN_ALGO)) != 0 && pg->nrows > 1) {
        ui.row[ui.page] = clamp(ui.row[ui.page] + (e > 0 ? 1 : -1), 0, pg->nrows - 1);
        ui.redraw = 1;
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        set.factory = (uint8_t)((set.factory + NFACTORY + (e > 0 ? 1 : -1)) % NFACTORY);
        factory(&set.p, set.factory);
        synth_panic();
        changed();
        app_say("SOUND", FACTORY_NAME[set.factory]);
    }
    for (i = 0; i < 4; i++) {
        const param_t *pk = &pg->rows[ui.row[ui.page]].k[i];
        int32_t v;
        if ((e = plat_enc(EN_K1 + i)) == 0 || !pk->label)
            continue;
        if (ui.mod_src) {                /* a source in hand: the knob sets how much it moves this parameter */
            float pu;
            int d = dst_of(pk, &pu), s2, slot = -1;
            ui.mod_at = plat_ms();
            if (d < 0) {
                app_say(pk->label, "NOT A TARGET");
                continue;
            }
            for (s2 = 0; s2 < NMOD && slot < 0; s2++)
                if (set.p.mod[s2].src == (uint16_t)ui.mod_src && set.p.mod[s2].dst == (uint16_t)d)
                    slot = s2;
            for (s2 = 0; s2 < NMOD && slot < 0; s2++)
                if (!set.p.mod[s2].src || !set.p.mod[s2].dst)
                    slot = s2;
            if (slot < 0) {
                app_say("MATRIX", "FULL");
                continue;
            }
            if (set.p.mod[slot].src != (uint16_t)ui.mod_src || set.p.mod[slot].dst != (uint16_t)d)
                set.p.mod[slot].amt = 512;
            set.p.mod[slot].src = (uint16_t)ui.mod_src;
            set.p.mod[slot].dst = (uint16_t)d;
            set.p.mod[slot].amt = (uint16_t)clamp((int32_t)set.p.mod[slot].amt + e * (fine ? 2 : 10), 0, 1023);
            {
                char n[24], a2[8];
                copy_text(n, DST_T[d], sizeof n);
                text_cat(n, " ", sizeof n);
                num_text(((int32_t)set.p.mod[slot].amt - 512) * 100 / 511, a2);
                text_cat(n, a2, sizeof n);
                app_say(SRC_T[ui.mod_src], n);
            }
            changed();
            continue;
        }
        v = clamp((int32_t)*pv(pk) + e * (fine || pk->step == 1 ? 1 : pk->step), pk->lo, pk->hi);
        *pv(pk) = (uint16_t)v;
        if (pk->fmt == F_ENUM || pk->fmt == F_ONOFF || pk->fmt == F_STYLE) {
            char n[16];
            value_text(pk, v, n);
            app_say(pk->label, n);
        }
        changed();
    }
    for (i = 0; i < 2; i++) {
        uint16_t c = cc_in[i];
        if (c) {
            cc_in[i] = 0;
            *(i ? &set.p.reso : &set.p.cutoff) = (uint16_t)((c - 1u) * 1023u / 127u);
            changed();
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {
        static const uint8_t PBTN[NPAGES] = {B_SEL, B_SEL, B_EDIT, B_ENV, B_ENV, B_ENV, B_LFO, B_LFO, B_ARP, B_FX, B_HOME};
        uint32_t lit = btn | 1u << PBTN[ui.page] | (set_dirty ? 1u << B_SAVE : 0u);
        if (ui.mod_src && (plat_ms() / 200u) & 1u)
            lit |= 1u << B_ARP;          /* a source in hand: ARP blinks */
        plat_glow(1);
        plat_leds(lit, keys);
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------- draw --- */
static float pscale(float x, float p)    /* Vital's powerScale, for the envelope's picture */
{
    if (fm_fabsf(p) < 0.005f)
        return x;
    return (fm_expf(p * x) - 1.0f) / (fm_expf(p) - 1.0f);
}

static void plot(int32_t x0, int32_t y0, int32_t w, int32_t h, const float *v, int n, uint16_t c)
{                                         /* v: n values in -1..1, a line across w */
    int i;
    for (i = 0; i + 1 < n; i++) {
        int32_t xa = x0 + i * w / (n - 1), xb = x0 + (i + 1) * w / (n - 1);
        int32_t ya = y0 + h / 2 - (int32_t)(v[i] * (float)(h / 2)), yb = y0 + h / 2 - (int32_t)(v[i + 1] * (float)(h / 2));
        cv_line(xa, ya, xb, yb, c);
    }
}

static void disc_(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y;
    for (y = -r; y <= r; y++) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - y * y));
        cv_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* synth.c's warp, for the picture (no FM / RM: they need the other oscillator) */
static float warp_preview(int type, float p, float w, float *win)
{
    float c, a, d;
    *win = 1.0f;
    switch (type) {
    case WARP_SYNC: return p * fm_exp2f(4.0f * w) - fm_floorf(p * fm_exp2f(4.0f * w));
    case WARP_FORMANT:
        *win = fm_sin_turns(p * 0.5f);
        return p * fm_exp2f(4.0f * w) - fm_floorf(p * fm_exp2f(4.0f * w));
    case WARP_QUANTIZE: {
        float q = 1.0f - w;
        q = fm_exp2f(q * q * q * 0.85f * 10.0f + 1.0f);
        return fm_floorf(p * q) / q;
    }
    case WARP_BEND: {
        float o = (w - w * w) * 2.0f, s3 = w * 3.0f, p2 = p * p, p3 = p2 * p;
        return p3 + (s3 + o) * (p2 - p3) + (s3 - o) * (p - 2.0f * p2 + p3);
    }
    case WARP_SQUEEZE:
        d = w * 1.9f + 0.05f;
        c = p - 0.5f;
        a = fm_fabsf(c);
        a = a > d * 0.25f ? 0.5f - (0.5f - a) / (2.0f - d) : a / d;
        return (c < 0.0f ? -a : a) + 0.5f;
    case WARP_PULSE:
        d = 1.0f / fm_maxf(1.0f - w, 0.02f);
        c = (p - 0.5f) * d;
        if (c >= 0.5f || c < -0.5f) {
            *win = 0.0f;
            return 0.0f;
        }
        return c + 0.5f;
    default: return p;
    }
}

static void draw_graph(void)
{
    const page_t *pg = &PAGES[ui.page];
    float v[97];
    int i;
    const int32_t X = 10, Y = 30, W = 220, H = 76;
    cv_rect(X, Y + H / 2, W, 1, C_LINE);
    switch (pg->kind) {
    case K_OSC: {                        /* Vital's wavetable view: every frame stacked back, the one playing lit */
        int o = ui.page == PG_OSCB, f;
        const wt_t *t = &wt_buf[o][wtst[o].cur];
        const osc_p *op = &set.p.osc[o];
        float fp = SVIEW.active ? SVIEW.frame[o] : (float)op->frame * (float)(WT_FRAMES - 1) / 1023.0f,
              wa = SVIEW.active ? SVIEW.warp[o] : (float)op->warp_amt / 1023.0f;
        for (f = WT_FRAMES - 1; f >= 0; f--) {   /* the stack, back to front */
            for (i = 0; i < 49; i++)
                v[i] = wt_peek(t, (float)f, (float)i / 48.0f) * 0.9f;
            plot(X + 30 + f * 6, Y + 22 - f * 3, W - 80, H - 40, v, 49, f == (int)(fp + 0.5f) ? C_DIM : C_LINE);
        }
        for (i = 0; i < 97; i++) {           /* the playing frame, between frames, through the warp */
            float win, ph = warp_preview(op->warp, (float)i / 96.0f, wa, &win);
            v[i] = wt_peek(t, fp, ph) * win * 0.9f;
        }
        plot(X + 30 + (int32_t)(fp * 6.0f), Y + 22 - (int32_t)(fp * 3.0f), W - 80, H - 40, v, 97, C_HI);
        {
            char n[24];
            copy_text(n, WARP_T[op->warp % WARP_N], sizeof n);
            cv_text(X, Y + H - 14, &FONT_T, n, op->warp ? C_HI : C_DIM);
            copy_text(n, SM_T[op->morph % SM_N], sizeof n);
            cv_text(X, Y + H - 2, &FONT_T, n, op->morph ? C_MOD : C_DIM);
            num_text((int32_t)(fp * 10.0f + 0.5f) / 10 + 1, n);
            text_cat(n, "/8", sizeof n);
            cv_text(X + W - text_w(&FONT_T, n), Y + H - 2, &FONT_T, n, C_GRAY);
        }
        break;
    }
    case K_ENV: {                        /* DAHDSR over 4 s (or longer), Vital's curves */
        const env_p *e = &set.p.env[ui.page - PG_ENV1];
        float tm[4], tot, sus = (float)e->sustain / 1023.0f, ap = -((float)e->apow - 512.0f) / 511.0f * 8.0f,
              dp = ((float)e->drpow - 512.0f) / 511.0f * 8.0f;
        const uint16_t *tv[4] = {&e->delay, &e->attack, &e->hold, &e->decay};
        for (i = 0; i < 4; i++) {
            float x = (float)*tv[i] / 1023.0f * 2.0f;
            x *= x;
            tm[i] = x * x;
        }
        {
            float r = (float)e->release / 1023.0f * 2.0f;
            r *= r;
            r *= r;
            tot = tm[0] + tm[1] + tm[2] + tm[3] + 0.6f + r;
            for (i = 0; i < 97; i++) {
                float t = tot * (float)i / 96.0f, y;
                if (t < tm[0]) y = 0.0f;
                else if ((t -= tm[0]) < tm[1]) y = pscale(t / tm[1], ap);
                else if ((t -= tm[1]) < tm[2]) y = 1.0f;
                else if ((t -= tm[2]) < tm[3]) y = 1.0f - (1.0f - sus) * pscale(t / tm[3], dp);
                else if ((t -= tm[3]) < 0.6f) y = sus;
                else y = sus * (1.0f - pscale(fm_minf((t - 0.6f) / fm_maxf(r, 1e-4f), 1.0f), dp));
                v[i] = y * 2.0f - 1.0f;
            }
        }
        plot(X, Y, W, H, v, 97, C_HI);
        if (SVIEW.active) {              /* the newest voice's level, now (Vital's playhead) */
            int32_t y = Y + H / 2 - (int32_t)((SVIEW.env[ui.page - PG_ENV1] * 2.0f - 1.0f) * (float)(H / 2));
            cv_rect(X, y, W, 1, C_DIM);
            disc_(X + W - 4, y, 3, C_WHITE);
        }
        break;
    }
    case K_LFO: {
        const lfo_p *l = &set.p.lfo[ui.page - PG_LFO1];
        uint32_t r = 0x1234u;
        float held = 0.3f, next = -0.6f;
        for (i = 0; i < 97; i++) {
            float ph = (float)(i % 48) / 48.0f, y;
            if (i % 48 == 0) {
                held = next;
                r = r * 1103515245u + 12345u;
                next = (float)((r >> 16) & 0xFFu) / 127.5f - 1.0f;
            }
            switch (l->shape) {
            default: case LS_SINE: y = fm_sin_turns(ph); break;
            case LS_TRI: y = 4.0f * fm_fabsf(ph - 0.5f) - 1.0f; break;
            case LS_SAWUP: y = 2.0f * ph - 1.0f; break;
            case LS_SAWDN: y = 1.0f - 2.0f * ph; break;
            case LS_SQUARE: y = ph < 0.5f ? 1.0f : -1.0f; break;
            case LS_SH: y = held; break;
            case LS_SMOOTH: y = held + (next - held) * (0.5f - 0.5f * fm_sin_turns((ph * 0.5f) + 0.25f)); break;
            }
            v[i] = y * 0.9f;
        }
        plot(X, Y, W, H, v, 97, C_HI);
        if (SVIEW.active) {              /* where the LFO is, on its first cycle */
            int k = ui.page - PG_LFO1;
            disc_(X + (int32_t)(SVIEW.lfo_ph[k] * (float)W * 0.5f), Y + H / 2 - (int32_t)(SVIEW.lfo[k] * 0.9f * (float)(H / 2)),
                  4, C_WHITE);
        }
        break;
    }
    case K_FILTER: {                     /* a two-pole's response, low -> band -> high by the blend */
        float fc = 440.0f * fm_exp2f((8.0f + 128.0f * (float)set.p.cutoff / 1023.0f - 69.0f) / 12.0f),
              k = 2.0f - 1.96f * (float)set.p.reso / 1023.0f, bl = (float)set.p.blend / 1023.0f * 2.0f;
        if (set.p.f_model == FM_OFF || set.p.f_model == FM_COMB || set.p.f_model == FM_FORMANT) {
            cv_text(X, Y + 30, &FONT_M, FM_T[set.p.f_model % FM_N], C_GRAY);
            break;
        }
        for (i = 0; i < 97; i++) {
            float f = 20.0f * fm_exp2f((float)i / 96.0f * 10.0f), w = f / fc, re = 1.0f - w * w, im = k * w,
                  den = re * re + im * im, lp = 1.0f / den, bp = w * w / den, hp = w * w * w * w / den, m2, db;
            m2 = bl < 1.0f ? lp + (bp - lp) * bl : bp + (hp - bp) * (bl - 1.0f);
            if (set.p.f_style)
                m2 *= m2;
            db = 4.343f * fm_logf(fm_maxf(m2, 1e-6f));            /* 10 log10 */
            v[i] = fm_clampf(db / 30.0f + 0.4f, -1.0f, 1.0f);
        }
        plot(X, Y, W, H, v, 97, C_HI);
        break;
    }
    case K_MATRIX:                       /* the eight slots */
        for (i = 0; i < NMOD; i++) {
            const mod_p *m = &set.p.mod[i];
            char t[40], n[8];
            int32_t y = Y - 4 + i * 10;
            uint16_t c = i == ui.row[PG_MAT] ? C_WHITE : m->src && m->dst ? C_HI : C_DIM;
            copy_text(t, SRC_T[m->src % SRC_N], sizeof t);
            text_cat(t, " > ", sizeof t);
            text_cat(t, DST_T[m->dst % DST_N], sizeof t);
            cv_text(X, y, &FONT_T, t, c);
            num_text(((int32_t)m->amt - 512) * 100 / 511, n);
            cv_text(X + W - text_w(&FONT_T, n), y, &FONT_T, n, c);
        }
        break;
    case K_FX: {                         /* the chain, Vital's order */
        static const char *const TAG[7] = {"CHO", "CMP", "DLY", "DST", "FLG", "PHS", "REV"};
        int on[7] = {set.p.ch_on, set.p.cp_on, set.p.dl_type != DLY_OFF, set.p.ds_type != DIST_OFF, set.p.fl_on,
                     set.p.ph_on, set.p.rv_on};
        for (i = 0; i < 7; i++) {
            int32_t bx = X + i * 31, here = i == ui.row[PG_FX];
            cv_rect(bx, Y + 26, 28, 22, here ? C_HI : on[i] ? C_DIM : C_LINE);
            cv_text(bx + (28 - text_w(&FONT_T, TAG[i])) / 2, Y + 31, &FONT_T, TAG[i],
                    here ? C_BG : on[i] ? C_WHITE : C_GRAY);
        }
        break;
    }
    default: {                           /* VOICE: the voices sounding */
        int n = synth_voices();
        for (i = 0; i < NVOICES; i++)
            cv_rect(X + i * 36, Y + 20, 30, 36, i < n ? C_HI : i < (int)set.p.poly ? C_DIM : C_LINE);
        break;
    }
    }
}

static void draw_top(void)
{
    const page_t *pg = &PAGES[ui.page];
    const row_t *rw = &pg->rows[ui.row[ui.page]];
    char t[24], n[12];
    int32_t x;
    cv_begin(240, 120, C_BG);
    x = cv_text(8, 4, &FONT_B, pg->name, C_WHITE);
    if (pg->nrows > 1)
        cv_text(x + 8, 7, &FONT_XS, rw->name, C_HI);
    copy_text(t, "< ", sizeof t);       /* the preset, Vital's way: arrows either side */
    text_cat(t, FACTORY_NAME[set.factory % NFACTORY], sizeof t);
    text_cat(t, set_dirty ? " * >" : " >", sizeof t);
    cv_text(232 - text_w(&FONT_T, t), 4, &FONT_T, t, C_GRAY);
    copy_text(t, "CPU ", sizeof t);
    num_text((int32_t)ui.cpu, n);
    text_cat(t, n, sizeof t);
    text_cat(t, "%", sizeof t);
    cv_text(180 - text_w(&FONT_T, t), 16, &FONT_T, t, ui.cpu > 80u ? C_WHITE : C_DIM);
    {                                    /* the output meter */
        int32_t w = (int32_t)(fm_minf(SVIEW.out_peak, 1.0f) * 48.0f);
        cv_rect(184, 19, 48, 4, C_LINE);
        cv_rect(184, 19, w, 4, SVIEW.out_peak > 0.95f ? C_WHITE : C_HI);
    }
    draw_graph();
    if (ui.mod_src) {                    /* a source in hand: a purple banner */
        cv_rect(0, 106, 240, 14, C_MOD);
        copy_text(t, SRC_T[ui.mod_src], sizeof t);
        text_cat(t, "  >  TURN A KNOB", sizeof t);
        cv_text(8, 106, &FONT_T, t, C_BG);
        cv_text(232 - text_w(&FONT_T, "ARP: DONE"), 106, &FONT_T, "ARP: DONE", C_BG);
    }
    if (plat_ms() < ui.msg_until) {
        int32_t w = text_w(&FONT_XS, ui.msg[0]) + (ui.msg[1][0] ? 8 + text_w(&FONT_XS, ui.msg[1]) : 0) + 16;
        cv_rect(120 - w / 2, 26, w, 17, C_LINE);
        cv_rect(120 - w / 2, 42, w, 1, ui.mod_src ? C_MOD : C_HI);
        x = cv_text(120 - w / 2 + 8, 27, &FONT_XS, ui.msg[0], C_WHITE);
        cv_text(x + 8, 27, &FONT_XS, ui.msg[1], ui.mod_src ? C_MOD : C_HI);
    }
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    const page_t *pg = &PAGES[ui.page];
    const row_t *rw = &pg->rows[ui.row[ui.page]];
    char t[24];
    int32_t i;
    cv_begin(240, 120, C_BG);
    for (i = 0; i < 4; i++) {
        const param_t *k = &rw->k[i];
        int32_t y = 2 + i * 19, v, w;
        float pu = 0.0f, frac;
        int d;
        if (!k->label)
            continue;
        v = *pv(k);
        d = dst_of(k, &pu);
        cv_text(8, y, &FONT_XS, k->label, d >= 0 && ui.mod_src ? C_MOD : C_GRAY);
        cv_rect(92, y + 5, 90, 5, C_LINE);
        frac = k->hi > k->lo ? (float)(v - k->lo) / (float)(k->hi - k->lo) : 0.0f;
        if (k->fmt == F_BIP) {
            w = (v - 512) * 45 / 511;
            cv_rect(w < 0 ? 137 + w : 137, y + 5, w < 0 ? -w : w, 5, C_HI);
        } else {
            w = (int32_t)(frac * 90.0f);
            cv_rect(92, y + 5, w, 5, C_HI);
        }
        if (d >= 0) {
            float reach = mod_reach(d, pu);
            if (reach > 0.001f) {        /* Vital's modulation ring: the reach, purple, round the value */
                float a0 = fm_clampf(frac - reach, 0.0f, 1.0f), a1 = fm_clampf(frac + reach, 0.0f, 1.0f);
                cv_rect(92 + (int32_t)(a0 * 90.0f), y + 11, (int32_t)((a1 - a0) * 90.0f) + 1, 2, C_MOD);
            }
            if (SVIEW.active && fm_fabsf(SVIEW.mods[d]) > 0.0001f) {   /* where the newest voice has it now */
                float now = fm_clampf(frac + SVIEW.mods[d] * pu, 0.0f, 1.0f);
                cv_rect(91 + (int32_t)(now * 90.0f), y + 2, 2, 11, C_WHITE);
            }
        }
        value_text(k, v, t);
        cv_text(234 - text_w(&FONT_XS, t), y, &FONT_XS, t, C_WHITE);
    }
    for (i = 0; i < pg->nrows; i++)
        cv_rect(8 + i * 9, 80, 6, 4, i == ui.row[ui.page] ? C_HI : C_LINE);
    for (i = 0; i < NPAGES; i++) {
        int32_t bx = 4 + i * 21;
        int here = i == ui.page, src = src_of_page(i) && src_of_page(i) == ui.mod_src;
        cv_rect(bx, 92, 20, 20, here ? C_HI : src ? C_MOD : C_LINE);
        cv_text(bx + (20 - text_w(&FONT_T, PAGES[i].tab)) / 2, 96, &FONT_T, PAGES[i].tab, here || src ? C_BG : C_GRAY);
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
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BG);
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    tables_update(0);
    ann_tick(now);
    if (now - ui.cpu_at >= 500u) {       /* the load and the voices, twice a second */
        uint32_t c = plat_cpu_pct(), vn = (uint32_t)synth_voices();
        ui.cpu_at = now;
        if (c != ui.cpu || vn != ui.voices)
            ui.redraw = 1;
        ui.cpu = c;
        ui.voices = vn;
    }
    if (ui.mod_src && now - ui.mod_at > 10000u) {   /* a source left in hand puts itself down */
        ui.mod_src = SRC_NONE;
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
        ui.top_at = ui.bot_at = now;
    } else if (SVIEW.active || SVIEW.out_peak > 0.003f) {   /* live: playheads ~12 / s, the rings' ticks ~8 / s */
        if (now - ui.top_at >= 80u) {
            ui.top_at = now;
            draw_top();
        }
        if (now - ui.bot_at >= 120u) {
            ui.bot_at = now;
            draw_bottom();
        }
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page")) *got = ui.page;
    else if (!strcmp(what, "row")) *got = ui.row[ui.page];
    else if (!strcmp(what, "voices")) *got = synth_voices();
    else if (!strcmp(what, "note")) *got = synth_last_note();
    else if (!strcmp(what, "factory")) *got = set.factory;
    else if (!strcmp(what, "tableA")) *got = set.p.osc[0].table;
    else if (!strcmp(what, "warpA")) *got = set.p.osc[0].warp;
    else if (!strcmp(what, "model")) *got = set.p.f_model;
    else if (!strcmp(what, "dirty")) *got = set_dirty;
    else if (!strcmp(what, "shed")) *got = (int)synth_shed_count();
    else if (!strcmp(what, "loud")) *got = synth_peak() > 0.02f;
    else if (!strcmp(what, "modsrc")) *got = ui.mod_src;
    else if (!strncmp(what, "slot", 4) && what[4] >= '0' && what[4] <= '7' && !strcmp(what + 5, "dst"))
        *got = set.p.mod[what[4] - '0'].dst;
    else if (!strncmp(what, "slot", 4) && what[4] >= '0' && what[4] <= '7' && !strcmp(what + 5, "amt"))
        *got = ((int)set.p.mod[what[4] - '0'].amt - 512) * 100 / 511;
    else return 0;
    return 1;
}
#endif
