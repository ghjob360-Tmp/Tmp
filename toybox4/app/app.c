/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the panel (the unity build's app part). The interaction follows the OP-1's model (see
 * docs/OP1_REFERENCE.md and README.md); the sound is in dsp/ (o2 units, from RAM, on both cores).
 *
 *   EDIT   SYNTH mode (the current track plays its synth)     GLO+EDIT  revert the synth sound to its slot
 *   ARP    DRUM mode (the current track plays its drum kit)    GLO+ARP   revert the kit
 *   HOME   TAPE, again MIXER, again TAPE                      GLO+HOME  (mixer) the sound path
 *   SEL ENV FX LFO   T1..T4: synth/drum: ENGINE, ENVELOPE, FX (again: on/off), LFO (again: on/off)
 *                    tape: tracks 1..4 (GLO: mute)   mixer: LEVELS, EQ (again: on/off), MASTER FX (again: on/off), OUT
 *   SEQ    the sequencer page (again: on / off)                GLO+SEQ   on / off from anywhere
 *   PLAY   tape play / stop                                    GLO+PLAY  reverse
 *   REC    tape record arm; on a sampler: sample (then hold a key)   GLO+REC held: erase the track's tape
 *   OCT- / OCT+   octave; tape: a bar back / on (GLO: to the start / to the end)
 *   SAVE   save the project; held: help                         GLO+SAVE  store the sound in its slot
 *   PRESET the sound slot (1..8)    ALGORITHM the tempo (GLO: the metronome)    SELECT the page's type / browser
 *   K1..K4 blue, green, white, orange; GLO: the second row      GLO + keys: tape tricks (tape), step entry (seq) */
#include <stddef.h>
#include "dsp/core.h"
#include "factory.h"
#include "ui_base.inc"
#include "ui_pics.inc"

enum { MD_SYNTH, MD_DRUM, MD_TAPE, MD_MIX };
enum { PG_T1, PG_T2, PG_T3, PG_T4, PG_SEQ, PG_PATH };

/* ------------------------------------------------------------- the project --- */
#define NSLOT 8
typedef struct {
    sound_t synth[NSLOT], drum[NSLOT];
    dpad_t dpads[NSLOT][NDKEY];
    char sname[NSLOT][12], dname[NSLOT][12];
} lib_t;
static proj_t pj;
static lib_t lib;
static int8_t octave;
static uint8_t slot_sel[2];
static uint8_t dirty;
static int cur;                          /* the current track */

static void mset(mod_t *m, int type, int on, const int16_t *p)
{
    int i;
    m->type = (uint16_t)type;
    m->on = (uint16_t)on;
    for (i = 0; i < 8; i++)
        m->p[i] = (uint16_t)(p ? p[i] : 512);
}

static void sound_default(sound_t *s, int kind)
{
    static const int16_t ENV_S[8] = {40, 520, 700, 380, 0, 0, 90, 700},
                         ENV_D[8] = {560, 512, 620, 160, 0, 0, 90, 720},
                         FXP[8] = {420, 300, 420, 300, 512, 0, 512, 0},
                         LFOP[8] = {380, 700, 0, 2, 0, 0, 0, 0},
                         SEQP[8] = {800, 0, 0, 0, 300, 0, 0, 600};
    mset(&s->m[M_ENGINE], kind ? D_KIT : E_PULSE, 1, 0);
    mset(&s->m[M_ENV], 0, 1, kind ? ENV_D : ENV_S);
    mset(&s->m[M_FX], FX_ECHO, 0, FXP);
    mset(&s->m[M_LFO], L_WAVE, 0, LFOP);
    mset(&s->m[M_SEQ], kind ? Q_HANDS : Q_ARP, 0, SEQP);
}

/* the factory library: 8 synth sounds and 8 kits */
typedef struct {
    const char *name;
    uint8_t engine, fx, fx_on, lfo, lfo_on;
    int16_t ep[8], env[8], fxp[8], lfop[8];
} preset_t;
static const preset_t SYN_PRESETS[NSLOT] = {
    {"PLUCK", E_STRING, FX_COIL, 1, L_WAVE, 0, {640, 420, 300, 200, 300, 600, 400, 150},
     {0, 420, 0, 500, 0, 0, 90, 700}, {400, 500, 600, 300, 0, 0, 0, 0}, {380, 600, 0, 2, 0, 0, 0, 0}},
    {"SQUELCH", E_PULSE, FX_KNOCK, 1, L_WAVE, 1, {400, 300, 330, 200, 650, 700, 700, 300},
     {0, 330, 350, 150, 0, 120, 90, 720}, {700, 500, 500, 600, 400, 500, 512, 800}, {300, 620, 0, 2, 0, 3, 0, 0}},
    {"BELLS", E_FM, FX_ECHO, 1, L_WAVE, 0, {520, 400, 300, 120, 200, 400, 600, 600},
     {0, 700, 0, 700, 0, 0, 90, 680}, {500, 200, 500, 300, 600, 900, 512, 0}, {300, 600, 0, 0, 0, 0, 0, 0}},
    {"SWARM", E_CLUSTER, FX_HALL, 1, L_WAVE, 1, {800, 500, 600, 500, 300, 500, 600, 300},
     {500, 700, 800, 650, 0, 0, 90, 640}, {600, 300, 500, 350, 300, 600, 500, 700}, {150, 640, 0, 2, 0, 0, 0, 0}},
    {"CHIPS", E_DIGITAL, FX_BITS, 0, L_WAVE, 0, {300, 600, 200, 500, 700, 600, 0, 0},
     {0, 300, 600, 200, 300, 0, 90, 680}, {600, 500, 0, 600, 0, 500, 512, 0}, {300, 600, 0, 0, 0, 0, 0, 0}},
    {"CZ BRASS", E_PHASE, FX_CUBE, 1, L_WAVE, 1, {400, 600, 500, 400, 0, 600, 300, 200},
     {150, 500, 700, 350, 0, 0, 90, 700}, {500, 500, 400, 300, 500, 300, 300, 600}, {450, 560, 0, 1, 0, 0, 0, 0}},
    {"ORGANIC", E_SPECTRA, FX_DOPPLER, 1, L_WAVE, 1, {300, 600, 400, 500, 500, 600, 500, 400},
     {200, 600, 800, 600, 0, 0, 90, 660}, {700, 400, 300, 300, 500, 500, 512, 400}, {200, 650, 0, 1, 0, 0, 0, 0}},
    {"VOX", E_SAMPLER, FX_HALL, 1, L_WAVE, 0, {0, 240, 1023, 1023, 0, 512, 512, 700},
     {150, 600, 900, 600, 0, 0, 90, 680}, {500, 200, 500, 300, 500, 500, 500, 600}, {300, 600, 0, 0, 0, 0, 0, 0}},
};
static const char *const KIT_NAMES[NSLOT] = {"BOX KIT", "TAPE KIT", "LO-FI", "DEEP BOX", "METAL", "PERC", "ROOM", "PHONE"};

static void factory_slices(dpad_t pads[NDKEY])
{
    int k;
    for (k = 0; k < NDKEY; k++) {
        pads[k].p[0] = 512;
        pads[k].p[1] = (uint16_t)(FACTORY_SLICE[k][0] * 1023u / 65535u);
        pads[k].p[2] = (uint16_t)(FACTORY_SLICE[k][1] * 1023u / 65535u);
        pads[k].p[3] = 0;
        pads[k].p[4] = 0;
        pads[k].p[5] = 512;
        pads[k].p[6] = 512;
        pads[k].p[7] = 800;
    }
}

static void lib_factory(void)
{
    static const int16_t FXA[8] = {500, 500, 500, 300, 500, 500, 500, 500};
    int i, k;
    for (i = 0; i < NSLOT; i++) {
        const preset_t *p = &SYN_PRESETS[i];
        sound_t *s = &lib.synth[i];
        sound_default(s, KIND_SYNTH);
        mset(&s->m[M_ENGINE], p->engine, 1, p->ep);
        mset(&s->m[M_ENV], 0, 1, p->env);
        mset(&s->m[M_FX], p->fx, p->fx_on, p->fxp);
        mset(&s->m[M_LFO], p->lfo, p->lfo_on, p->lfop);
        copy_text(lib.sname[i], p->name, 12);
    }
    for (i = 0; i < NSLOT; i++) {
        sound_t *s = &lib.drum[i];
        sound_default(s, KIND_DRUM);
        copy_text(lib.dname[i], KIT_NAMES[i], 12);
        if (i == 1 || i == 2 || i == 5 || i == 6) {   /* the tape kits: slices of the factory sample */
            s->m[M_ENGINE].type = D_SLICE;
            factory_slices(lib.dpads[i]);
        } else {
            drum_kit_defaults(lib.dpads[i]);
        }
        if (i == 2)
            mset(&s->m[M_FX], FX_BITS, 1, FXA);
        if (i == 3)
            for (k = 0; k < NDKEY; k++)
                lib.dpads[i][k].p[0] = (uint16_t)(lib.dpads[i][k].p[0] * 7u / 10u);
        if (i == 4)
            for (k = 0; k < NDKEY; k++)
                lib.dpads[i][k].p[3] = 900;
        if (i == 5)
            for (k = 0; k < NDKEY; k++)
                lib.dpads[i][k].p[0] = (uint16_t)(400 + (k % 6) * 60);
        if (i == 6)
            mset(&s->m[M_FX], FX_HALL, 1, FXA);
        if (i == 7)
            mset(&s->m[M_FX], FX_WIRE, 1, FXA);
    }
}

static void load_slot(int kind, int n)
{
    track_t *t = &pj.t[cur];
    n &= NSLOT - 1;
    if (kind == KIND_SYNTH) {
        t->synth = lib.synth[n];
    } else {
        int k;
        t->drum = lib.drum[n];
        for (k = 0; k < NDKEY; k++)
            t->pads[k] = lib.dpads[n][k];
    }
    slot_sel[kind] = (uint8_t)n;
}

static void store_slot(int kind)
{
    const track_t *t = &pj.t[cur];
    int n = slot_sel[kind] & (NSLOT - 1), k;
    if (kind == KIND_SYNTH) {
        lib.synth[n] = t->synth;
    } else {
        lib.drum[n] = t->drum;
        for (k = 0; k < NDKEY; k++)
            lib.dpads[n][k] = t->pads[k];
    }
}

/* the demo on the tape: a beat (T1, the tape kit), a bass (T2), bells (T4); T3 is yours */
static void tape_demo(tape_t *tp)
{
    static const uint8_t BASS[16] = {36, 0, 36, 48, 0, 36, 46, 0, 41, 0, 41, 53, 0, 43, 46, 48};
    static const uint8_t BELL[6] = {72, 79, 76, 84, 79, 74};
    int bar, s, t;
    for (t = 0; t < NTRK; t++)
        tp->n[t] = 0;
#define EV(tr, tk, nt, vl, kd)                                                                                     \
    do {                                                                                                           \
        if (tp->n[tr] < TAPE_EV) {                                                                                 \
            tev_t *e = &tp->ev[tr][tp->n[tr]++];                                                                   \
            e->tick = (uint32_t)(tk);                                                                              \
            e->note = (uint8_t)(nt);                                                                               \
            e->vel = (uint8_t)(vl);                                                                                \
            e->kind = (uint8_t)(kd);                                                                               \
            e->rsv = 0;                                                                                            \
        }                                                                                                          \
    } while (0)
    for (bar = 0; bar < 4; bar++)
        for (s = 0; s < 16; s++) {
            uint32_t tk = (uint32_t)(bar * 16 + s) * (TPQ / 4);
            int tr = bar == 2 ? 5 : bar == 3 ? 3 : 0;
            if (s == 0 || s == 8 || (s == 11 && (bar & 1)) || (s == 14 && bar == 3))
                EV(0, tk, 60, s == 0 ? 120 : 100, KIND_DRUM);          /* kick */
            if (s == 4 || s == 12)
                EV(0, tk, 62, 110, KIND_DRUM);                          /* snare */
            if (!(s & 1))
                EV(0, tk + 1, 67, (s & 3) ? 70 : 95, KIND_DRUM);        /* hat */
            if (s == 14 && bar == 1)
                EV(0, tk + 2, 68, 80, KIND_DRUM);                       /* open hat */
            if (BASS[s]) {
                EV(1, tk, BASS[s] + tr, 100, KIND_SYNTH);
                EV(1, tk + TPQ / 5, BASS[s] + tr, 0, KIND_SYNTH);
            }
        }
    for (s = 0; s < 6; s++) {
        uint32_t tk = (uint32_t)(s * 5 + 2) * (TPQ / 2);
        EV(3, tk, BELL[s], 90, KIND_SYNTH);
        EV(3, tk + TPQ, BELL[s], 0, KIND_SYNTH);
    }
#undef EV
    for (t = 0; t < NTRK; t++) {         /* by tick */
        int a, b;
        for (a = 1; a < tp->n[t]; a++) {
            tev_t x = tp->ev[t][a];
            b = a;
            while (b > 0 && tp->ev[t][b - 1].tick > x.tick) {
                tp->ev[t][b] = tp->ev[t][b - 1];
                b--;
            }
            tp->ev[t][b] = x;
        }
    }
}

static void project_default(void)
{
    static const uint8_t LOOP[8] = {60, 63, 67, 70, 72, 70, 67, SEQ_TIE};
    static const int16_t EQ[8] = {560, 512, 560, 0, 0, 0, 0, 0}, MF[8] = {380, 420, 300, 220, 500, 500, 500, 500},
                         MO[8] = {820, 820, 320, 400, 0, 0, 0, 0};
    int t, i;
    uint8_t *z = (uint8_t *)(void *)&pj;
    for (i = 0; i < (int)sizeof pj; i++)
        z[i] = 0;
    for (t = 0; t < NTRK; t++) {
        track_t *tk = &pj.t[t];
        int ss = t == 1 ? 1 : t == 2 ? 0 : t == 3 ? 2 : 3, ds = t == 0 ? 1 : 0;
        tk->synth = lib.synth[ss];
        tk->drum = lib.drum[ds];
        for (i = 0; i < NDKEY; i++)
            tk->pads[i] = lib.dpads[ds][i];
        tk->level = 820;
        tk->pan = 512;
        tk->kind = t == 0 ? KIND_DRUM : KIND_SYNTH;
        tk->seq_synth.loop_n = 8;
        for (i = 0; i < 8; i++)
            tk->seq_synth.loop[i] = LOOP[i];
        for (i = 0; i < 16; i++)
            tk->seq_synth.grid[i] = (uint8_t)((i & 3) == 0 ? 60 + (i >> 2) * 3 : 0);
        for (i = 0; i < 32; i++)
            tk->seq_synth.draw[i] = (uint8_t)(64 + (int)(40.0f * fm_sin_turns((float)i / 32.0f)));
        tk->seq_drum.loop_n = 4;
        tk->seq_drum.loop[0] = 60;
        tk->seq_drum.loop[1] = 67;
        tk->seq_drum.loop[2] = 62;
        tk->seq_drum.loop[3] = 67;
        for (i = 0; i < 16; i++)
            tk->seq_drum.grid[i] = (uint8_t)(i % 4 == 0 ? 60 : i % 8 == 4 ? 62 : 67);
    }
    pj.t[3].level = 700;
    pj.t[2].pan = 380;
    pj.t[3].pan = 660;
    mset(&pj.eq, 0, 1, EQ);
    mset(&pj.mfx, FX_CUBE, 1, MF);
    mset(&pj.mout, 0, 1, MO);
    pj.tempo = 100;
    pj.metro = 0;
    pj.speed = 512;
    pj.loop_on = 1;
    pj.loop_in = 0;
    pj.loop_out = 4;
    pj.sync = 1;
    pj.detune = 512;
    slot_sel[0] = 3;
    slot_sel[1] = 1;
}

/* ------------------------------------------------------------- the flash --- */
/* the project, the library and the tape: XDATA1 (0xE5000..), 3840 bytes a sector (bytes 0xF00.. kept erased);
 * the samples: the data area, a 256-byte header page, then mu-law at 22050 Hz */
#define PRJ_BASE PLAT_XDATA1_LO
#define PRJ_SECT 3840u
#define PRJ_MAGIC 0x31584254u            /* "TBX1" */
#define SYN_BASE 0x97000u
#define SYN_END 0xB3000u
#define DRM_BASE 0xB3000u
#define DRM_END PLAT_DATA_HI
#define SMP_MAGIC 0x31504D53u            /* "SMP1" */
typedef struct {
    uint32_t magic, version, len, sum;
} prj_head_t;
typedef struct {
    uint32_t magic, len, rate, rsv;
} smp_head_t;

static uint32_t sum32(const uint8_t *p, uint32_t n, uint32_t s)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        s = (s ^ p[i]) * 16777619u;
    return s;
}

typedef struct {
    void *p;
    uint32_t n;
} part_t;
static uint8_t misc[4];
static void parts(part_t *pt)
{
    pt[0].p = &pj;
    pt[0].n = sizeof pj;
    pt[1].p = &lib;
    pt[1].n = sizeof lib;
    pt[2].p = misc;
    pt[2].n = 4;
    pt[3].p = core_tape_rw();
    pt[3].n = sizeof(tape_t);
}

static int project_save(void)
{
    static uint8_t page[256];
    part_t pt[4];
    prj_head_t h;
    uint32_t total, i, pos = 0, pi = 0, off = 0, k, sect = 0;
    int err = 0;
    misc[0] = (uint8_t)octave;
    misc[1] = slot_sel[0];
    misc[2] = slot_sel[1];
    misc[3] = (uint8_t)cur;
    core_hold(1);
    parts(pt);
    h.magic = PRJ_MAGIC;
    h.version = 1;
    h.len = 0;
    h.sum = 2166136261u;
    for (i = 0; i < 4; i++) {
        h.len += pt[i].n;
        h.sum = sum32((const uint8_t *)pt[i].p, pt[i].n, h.sum);
    }
    total = sizeof h + h.len;
    while (pos < total && !err) {        /* sector by sector, page by page */
        uint32_t base = PRJ_BASE + sect * 4096u, in_s;
        if (base >= PLAT_XDATA1_HI || plat_data_erase(base)) {
            err = -1;
            break;
        }
        for (in_s = 0; in_s < PRJ_SECT && pos < total; in_s += 256u) {
            for (k = 0; k < 256u; k++, pos++) {
                uint8_t b = 0xFF;
                if (pos < sizeof h) {
                    b = ((const uint8_t *)&h)[pos];
                } else if (pos < total) {
                    while (pi < 4 && off >= pt[pi].n) {
                        pi++;
                        off = 0;
                    }
                    if (pi < 4)
                        b = ((const uint8_t *)pt[pi].p)[off++];
                }
                page[k] = b;
            }
            if (plat_data_write(base + in_s, page, 256)) {
                err = -1;
                break;
            }
        }
        sect++;
    }
    core_hold(0);
    return err;
}

static const uint8_t *prj_byte(uint32_t pos)
{
    return plat_data(PRJ_BASE + (pos / PRJ_SECT) * 4096u + pos % PRJ_SECT);
}

static int project_load(void)
{
    const uint8_t *q = plat_data(PRJ_BASE);
    prj_head_t h;
    part_t pt[4];
    uint32_t i, pos, pi, off, s;
    if (!q)
        return -1;
    for (i = 0; i < sizeof h; i++)
        ((uint8_t *)&h)[i] = *prj_byte(i);
    parts(pt);
    if (h.magic != PRJ_MAGIC || h.version != 1 || h.len != pt[0].n + pt[1].n + pt[2].n + pt[3].n)
        return -1;
    s = 2166136261u;                     /* the sum, before anything is copied */
    for (pos = sizeof h, i = 0; i < h.len; i++, pos++)
        s = (s ^ *prj_byte(pos)) * 16777619u;
    if (s != h.sum)
        return -1;
    core_hold(1);
    for (pos = sizeof h, pi = 0, off = 0, i = 0; i < h.len; i++, pos++) {
        while (pi < 4 && off >= pt[pi].n) {
            pi++;
            off = 0;
        }
        if (pi >= 4)
            break;
        ((uint8_t *)pt[pi].p)[off++] = *prj_byte(pos);
    }
    octave = (int8_t)misc[0];
    slot_sel[0] = misc[1] & 7u;
    slot_sel[1] = misc[2] & 7u;
    cur = misc[3] & 3u;
    if (octave < -3 || octave > 3)
        octave = 0;
    core_hold(0);
    return 0;
}

/* the samples: a user sample in its region, or the factory one */
static void samples_point(void)
{
    const smp_head_t *h = (const smp_head_t *)(const void *)plat_data(SYN_BASE);
    if (h && h->magic == SMP_MAGIC && h->len > 0 && h->len <= SYN_END - SYN_BASE - 256u) {
        g_synth_sample.data = plat_data(SYN_BASE + 256u);
        g_synth_sample.len = h->len;
    } else {
        g_synth_sample.data = FACTORY_SYNTH;
        g_synth_sample.len = sizeof FACTORY_SYNTH;
    }
    h = (const smp_head_t *)(const void *)plat_data(DRM_BASE);
    if (h && h->magic == SMP_MAGIC && h->len > 0 && h->len <= DRM_END - DRM_BASE - 256u) {
        g_drum_sample.data = plat_data(DRM_BASE + 256u);
        g_drum_sample.len = h->len;
    } else {
        g_drum_sample.data = FACTORY_DRUM;
        g_drum_sample.len = sizeof FACTORY_DRUM;
    }
}

/* ------------------------------------------------------------ the sampler --- */
/* OP-1 style: on a sampler sound, REC prepares (the region erased: the sound stops a moment), then a held key
 * records the master output until it is let go (or the region is full) */
enum { SM_OFF, SM_ERASING, SM_READY, SM_REC };
static struct {
    uint8_t state, kind, key;
    uint32_t base, end, next_erase, wr, len, page_n, done_at;
    uint8_t page[256];
} smp;

static void sampler_arm(int kind)
{
    smp.kind = (uint8_t)kind;
    smp.base = kind ? DRM_BASE : SYN_BASE;
    smp.end = kind ? DRM_END : SYN_END;
    smp.next_erase = smp.base;
    smp.state = SM_ERASING;
    if (kind) {                          /* the region is going: the factory sample meanwhile */
        g_drum_sample.data = FACTORY_DRUM;
        g_drum_sample.len = sizeof FACTORY_DRUM;
    } else {
        g_synth_sample.data = FACTORY_SYNTH;
        g_synth_sample.len = sizeof FACTORY_SYNTH;
    }
}

static void sampler_finish(void)
{
    smp_head_t h;
    if (smp.page_n) {
        uint32_t k;
        for (k = smp.page_n; k < 256u; k++)
            smp.page[k] = 0xFF;
        plat_data_write(smp.wr, smp.page, 256);
        smp.page_n = 0;
    }
    h.magic = SMP_MAGIC;
    h.len = smp.len;
    h.rate = 22050;
    h.rsv = 0;
    if (smp.len > 2205u)                 /* a tenth of a second at least */
        plat_data_write(smp.base, &h, sizeof h);
    samples_point();
    smp.state = SM_OFF;
    smp.done_at = plat_ms();
}

static void sampler_drain(void)
{
    uint8_t buf[512];
    uint32_t n, k;
    while ((n = core_sample_take(buf, sizeof buf)) > 0)
        for (k = 0; k < n && smp.wr + 256u <= smp.end; k++) {
            smp.page[smp.page_n++] = buf[k];
            smp.len++;
            if (smp.page_n == 256u) {
                plat_data_write(smp.wr, smp.page, 256);
                smp.wr += 256u;
                smp.page_n = 0;
            }
        }
}

static void sampler_service(void)        /* every frame */
{
    if (smp.state == SM_ERASING) {       /* a few sectors a frame: the screen shows the progress */
        int k;
        for (k = 0; k < 4 && smp.next_erase < smp.end; k++, smp.next_erase += 4096u)
            plat_data_erase(smp.next_erase);
        if (smp.next_erase >= smp.end) {
            smp.state = SM_READY;
            smp.wr = smp.base + 256u;
            smp.len = 0;
            smp.page_n = 0;
            core_sample_arm(smp.kind);
        }
    } else if (smp.state == SM_REC) {
        sampler_drain();
        if (smp.wr + 256u > smp.end) {   /* full */
            core_sample_go(0);
            sampler_finish();
        }
    }
}

#include "ui_screens.inc"
