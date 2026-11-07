/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the panel. Vital's sections cut to an FM-1: pages (buttons) of rows (ALGORITHM) of four parameters
 * (K1..K4), a page's main choice on SELECT, presets on PRESET; modulation assigned by picking up a source (ARP on its
 * page) and turning any knob. Part of the unity build after the platform; the sound is in dsp/ (o2 units, RAM). */
#include <stddef.h>
#include "dsp/core.h"

#include "ui_base.inc"

/* ------------------------------------------------------------------ the state --- */
static patch_t pt;                       /* the patch being edited (the core plays a copy) */
static int8_t octave;
static int cur_preset;                   /* 0..NFACT-1 factory, NFACT.. user */
static uint8_t dirty;                    /* edited since loaded / saved */
static uint32_t changed_at;              /* the last edit (ms): the settings are kept a while after */
static uint8_t set_dirty;

/* ---------------------------------------------------------------- wavetables --- */
static wt_t wtb[NOSC + 1] PLAT_POOL;     /* three playing, one being built */
static struct {
    wt_t *play[NOSC];                    /* what each oscillator plays */
    wt_t *spare;                         /* free, or being built into */
    int built[NOSC][3];                  /* what play[k] holds: table, morph, amount (0..1023) */
    int building;                        /* the oscillator being built for, -1 none */
    wt_job_t job;
    int want_amt;
} WT;

static void tables_reset(void)
{
    int k;
    for (k = 0; k < NOSC; k++) {
        WT.play[k] = &wtb[k];
        WT.built[k][0] = -1;
    }
    WT.spare = &wtb[NOSC];
    WT.building = -1;
}
/* one step of the wavetable work: a table that differs from the patch is rebuilt in the spare, then swapped in */
static void tables_service(void)
{
    int k;
    if (WT.building >= 0) {
        if (wt_build_step(&WT.job)) {
            wt_t *old;
            k = WT.building;
            old = WT.play[k];
            WT.play[k] = WT.spare;
            WT.built[k][0] = WT.job.table;
            WT.built[k][1] = WT.job.morph;
            WT.built[k][2] = WT.want_amt;
            core_table(k, WT.play[k]);
            WT.spare = old;
            WT.building = -1;
        }
        return;
    }
    if (core_table_busy(WT.spare))       /* the render may still read the old one */
        return;
    for (k = 0; k < NOSC; k++) {
        int t = pt.v[P_OSC(k, TABLE)], m = pt.v[P_OSC(k, MORPH)], a = pt.v[P_OSC(k, MAMT)];
        if (t >= WT_NFACT)
            t = 0;
        if (m == SM_NONE)
            a = 0;
        if (WT.built[k][0] != t || WT.built[k][1] != m || WT.built[k][2] != a) {
            WT.building = k;
            WT.want_amt = a;
            wt_build_begin(&WT.job, WT.spare, t, m, (float)a / 1023.0f);
            return;
        }
    }
}
static void tables_now(void)             /* at boot / a preset: everything built before the first note */
{
    int guard = 4000;
    while (guard-- > 0) {
        int k, same = 1;
        tables_service();
        for (k = 0; k < NOSC; k++) {
            int t = pt.v[P_OSC(k, TABLE)], m = pt.v[P_OSC(k, MORPH)], a = m == SM_NONE ? 0 : pt.v[P_OSC(k, MAMT)];
            if (t >= WT_NFACT)
                t = 0;
            same &= WT.built[k][0] == t && WT.built[k][1] == m && WT.built[k][2] == a;
        }
        if (same && WT.building < 0)
            return;
    }
}

/* ------------------------------------------------------------------ presets --- */
static void patch_default(patch_t *p)
{
    int i, k;
    uint8_t *z = (uint8_t *)(void *)p;
    for (i = 0; i < (int)sizeof *p; i++)
        z[i] = 0;
    for (i = 0; i < NPARAM; i++)
        p->v[i] = PDESC[i].def;
    for (i = 0; i < NSLOT; i++)
        p->m[i].dst = 0xFFFFu;
    for (k = 0; k < 8; k++)
        for (i = 0; i < LFO_PTS; i++)
            p->lfo_y[k][i] = (uint8_t)(i < LFO_PTS / 2 ? i * 255 / (LFO_PTS / 2) : (LFO_PTS - i) * 255 / (LFO_PTS / 2));
    for (i = 0; i < FXT_N; i++)
        p->fx_order[i] = (uint8_t)i;
    for (k = 0; k < FXT_N; k++)          /* the effects' own defaults (the mixes dry-ish, sensible times) */
        for (i = 0; i < FX_NP; i++)
            p->v[P_FX(k) + 1 + i] = 512;
    {
        static const struct { uint8_t t, i; uint16_t v; } D[] = {
            {FXT_CHORUS, 0, 300}, {FXT_CHORUS, 1, 350}, {FXT_CHORUS, 2, 0}, {FXT_CHORUS, 3, 500}, {FXT_CHORUS, 4, 400},
            {FXT_CHORUS, 5, 600}, {FXT_CHORUS, 6, 512}, {FXT_CHORUS, 7, 700}, {FXT_CHORUS, 8, 500}, {FXT_CHORUS, 9, 500},
            {FXT_COMP, 0, 0}, {FXT_COMP, 1, 300}, {FXT_COMP, 2, 400}, {FXT_COMP, 3, 512}, {FXT_COMP, 4, 512},
            {FXT_COMP, 5, 512}, {FXT_COMP, 6, 600}, {FXT_COMP, 7, 1023},
            {FXT_DELAY, 0, 400}, {FXT_DELAY, 1, 600}, {FXT_DELAY, 2, 600}, {FXT_DELAY, 3, 600}, {FXT_DELAY, 4, 520},
            {FXT_DELAY, 5, 400}, {FXT_DELAY, 6, 700}, {FXT_DELAY, 7, 500}, {FXT_DELAY, 8, 300},
            {FXT_DIST, 0, 0}, {FXT_DIST, 1, 600}, {FXT_DIST, 2, 0}, {FXT_DIST, 3, 700}, {FXT_DIST, 4, 200},
            {FXT_DIST, 5, 0}, {FXT_DIST, 6, 1023},
            {FXT_EQ, 0, 0}, {FXT_EQ, 1, 250}, {FXT_EQ, 2, 512}, {FXT_EQ, 3, 300}, {FXT_EQ, 4, 0}, {FXT_EQ, 5, 600},
            {FXT_EQ, 6, 512}, {FXT_EQ, 7, 300}, {FXT_EQ, 8, 0}, {FXT_EQ, 9, 850}, {FXT_EQ, 10, 512}, {FXT_EQ, 11, 300},
            {FXT_FILTER, 0, 0}, {FXT_FILTER, 1, 0}, {FXT_FILTER, 2, 700}, {FXT_FILTER, 3, 300}, {FXT_FILTER, 4, 0},
            {FXT_FILTER, 5, 0}, {FXT_FILTER, 8, 1023},
            {FXT_FLANGER, 0, 300}, {FXT_FLANGER, 1, 0}, {FXT_FLANGER, 2, 600}, {FXT_FLANGER, 3, 400},
            {FXT_FLANGER, 4, 500}, {FXT_FLANGER, 5, 300}, {FXT_FLANGER, 6, 500},
            {FXT_PHASER, 0, 300}, {FXT_PHASER, 1, 0}, {FXT_PHASER, 2, 600}, {FXT_PHASER, 3, 600}, {FXT_PHASER, 4, 500},
            {FXT_PHASER, 5, 512}, {FXT_PHASER, 6, 300}, {FXT_PHASER, 7, 500},
            {FXT_REVERB, 0, 600}, {FXT_REVERB, 1, 500}, {FXT_REVERB, 2, 100}, {FXT_REVERB, 3, 900},
            {FXT_REVERB, 4, 700}, {FXT_REVERB, 5, 400}, {FXT_REVERB, 6, 300}, {FXT_REVERB, 7, 300},
            {FXT_REVERB, 8, 0}, {FXT_REVERB, 9, 280}};
        for (i = 0; i < (int)(sizeof D / sizeof D[0]); i++)
            p->v[P_FX(D[i].t) + 1 + D[i].i] = D[i].v;
    }
    copy_text(p->name, "INIT", NAME_LEN);
}
static int mslot_add(patch_t *p, int src, int dst, int amt)   /* amt -1000..1000; the slot's index or -1 */
{
    int i;
    for (i = 0; i < NSLOT; i++)
        if (p->m[i].src == MS_NONE || p->m[i].dst == 0xFFFFu) {
            p->m[i].src = (uint8_t)src;
            p->m[i].dst = (uint16_t)dst;
            p->m[i].amt = (int16_t)amt;
            p->m[i].pow = 0;
            p->m[i].flags = 0;
            return i;
        }
    return -1;
}
static int find_table(const char *name)  /* a factory table by its name (0 when missing) */
{
    int i;
    for (i = 0; i < WT_NFACT; i++) {
        const char *a = WT_NAME[i], *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return 0;
}

#include "presets.inc"

/* user presets: XDATA1, two a sector (3840 usable bytes of each 4 KiB) */
#define UP_MAGIC 0x50525031u             /* "PRP1" */
#define UP_SLOT 1920u
#define NUSER ((int)((PLAT_XDATA1_HI - PLAT_XDATA1_LO) / 4096u) * 2)
static uint32_t up_off(int u) { return PLAT_XDATA1_LO + (uint32_t)(u >> 1) * 4096u + (uint32_t)(u & 1) * UP_SLOT; }
static const patch_t *user_get(int u)    /* NULL = empty */
{
    const uint8_t *q = plat_data(up_off(u));
    uint32_t m;
    if (!q)
        return 0;
    m = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    return m == UP_MAGIC ? (const patch_t *)(const void *)(q + 4) : 0;
}
static uint8_t sec[3840] __attribute__((aligned(4)));   /* a sector being rewritten; also the settings' image (main loop) */
static int user_put(int u, const patch_t *p)   /* 0 = done */
{
    uint32_t base = PLAT_XDATA1_LO + (uint32_t)(u >> 1) * 4096u, i, magic = UP_MAGIC;
    const uint8_t *q = plat_data(base);
    if (!q || u < 0 || u >= NUSER)
        return -1;
    for (i = 0; i < sizeof sec; i++)
        sec[i] = q[i];
    {
        uint8_t *d = sec + (uint32_t)(u & 1) * UP_SLOT;
        const uint8_t *s = (const uint8_t *)(const void *)p;
        for (i = 0; i < 4; i++)
            d[i] = (uint8_t)(magic >> (8 * i));
        for (i = 0; i < sizeof(patch_t); i++)
            d[4 + i] = s[i];
    }
    if (plat_data_erase(base))
        return -1;
    for (i = 0; i < sizeof sec; i += 256u)
        if (plat_data_write(base + i, sec + i, 256u))
            return -1;
    return 0;
}
#define NPRESET (NFACT + NUSER)

/* the settings: the patch being edited, kept across power-off */
#define SET_MAGIC 0x50524D32u            /* "PRM2" */
typedef struct {
    uint32_t magic;
    patch_t p;
    int16_t preset;
    int8_t octave;
    uint8_t rsv;
} settings_t;
#define set (*(settings_t *)(void *)sec)   /* (no copy of its own: the sector buffer, aligned below) */
static void settings_save(void)
{
    set.magic = SET_MAGIC;
    set.p = pt;
    set.preset = (int16_t)cur_preset;
    set.octave = octave;
    plat_store_save(OBJ_SETTINGS, &set, sizeof(settings_t));
    set_dirty = 0;
}

static void patch_sane(patch_t *p)       /* whatever was loaded: every value in range */
{
    int i;
    for (i = 0; i < NPARAM; i++) {
        uint16_t mx = PDESC[i].kind == PK_CHOICE ? (uint16_t)(PDESC[i].n - 1) : 1023u;
        if (p->v[i] > mx)
            p->v[i] = PDESC[i].def <= mx ? PDESC[i].def : 0;
    }
    if (p->v[P_O1_TABLE] >= (uint16_t)WT_NFACT) p->v[P_O1_TABLE] = 0;
    if (p->v[P_O2_TABLE] >= (uint16_t)WT_NFACT) p->v[P_O2_TABLE] = 0;
    if (p->v[P_O3_TABLE] >= (uint16_t)WT_NFACT) p->v[P_O3_TABLE] = 0;
    if (p->v[P_S_SAMPLE] >= (uint16_t)SMP_NFACT) p->v[P_S_SAMPLE] = 0;
    for (i = 0; i < 2; i++)
        if (p->v[P_FLT(i, STYLE)] >= (uint16_t)FLT_NSTYLE[p->v[P_FLT(i, MODEL)]])
            p->v[P_FLT(i, STYLE)] = 0;
    for (i = 0; i < NSLOT; i++) {
        if (p->m[i].src >= MS_N || (p->m[i].dst != 0xFFFFu && p->m[i].dst >= NPARAM))
            p->m[i].src = MS_NONE;
        if (p->m[i].amt > 1000 || p->m[i].amt < -1000)
            p->m[i].amt = 0;
        if (p->m[i].pow > 100 || p->m[i].pow < -100)
            p->m[i].pow = 0;
    }
    {
        uint16_t seen = 0;
        for (i = 0; i < FXT_N; i++) {
            if (p->fx_order[i] >= FXT_N || (seen >> p->fx_order[i] & 1u))
                break;
            seen |= (uint16_t)(1u << p->fx_order[i]);
        }
        if (i < FXT_N)
            for (i = 0; i < FXT_N; i++)
                p->fx_order[i] = (uint8_t)i;
    }
    p->name[NAME_LEN - 1] = 0;
}

static void patch_send(void) { core_patch(&pt); }
static void preset_load(int n)
{
    if (n < NFACT) {
        factory(&pt, n);
    } else {
        const patch_t *u = user_get(n - NFACT);
        if (!u) {
            patch_default(&pt);
            copy_text(pt.name, "EMPTY", NAME_LEN);
        } else {
            pt = *u;
        }
    }
    patch_sane(&pt);
    cur_preset = n;
    dirty = 0;
    set_dirty = 1;
    changed_at = plat_ms();
    patch_send();
}

/* ------------------------------------------------------------------ the sound --- */
static volatile uint16_t cc_in[2];       /* CC 74 cutoff, 71 resonance (+1; 0 none) */
static void midi_in(void)                /* audio interrupt */
{
    uint32_t m;
    int port;
    while (plat_midi_in(&m, &port)) {
        uint32_t st = m >> 8 & 0xFFu, d1 = m >> 16 & 0x7Fu, d2 = m >> 24 & 0x7Fu;
        if ((st & 0xF0u) == 0xB0u && d1 == 74u)
            cc_in[0] = (uint16_t)(d2 * 1023u / 127u + 1u);
        else if ((st & 0xF0u) == 0xB0u && d1 == 71u)
            cc_in[1] = (uint16_t)(d2 * 1023u / 127u + 1u);
        else
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

#include "ui_fmt.inc"
#include "ui_input.inc"
#include "ui_pics.inc"
#include "ui_draw.inc"

void app_init(void)
{
    uint8_t *z = (uint8_t *)(void *)&ui;
    uint32_t i;
    for (i = 0; i < sizeof ui; i++)      /* (the host's reboot keeps the statics) */
        z[i] = 0;
    plat_cpu2_set(1);
    smp_init();
    core_init();
    tables_reset();
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof(settings_t)) == (int)sizeof(settings_t) && set.magic == SET_MAGIC) {
        pt = set.p;
        patch_sane(&pt);
        cur_preset = set.preset >= 0 && set.preset < NPRESET ? set.preset : 0;
        octave = set.octave >= -3 && set.octave <= 3 ? set.octave : 0;
        patch_send();
    } else {
        preset_load(0);
    }
    dirty = 0;
    tables_now();
}
