/* SPDX-License-Identifier: GPL-3.0-only */
/* ext.c: the synthwave extensions' code (types and tables in ext_def.h). Included by chips.c after the voices. */
static volatile uint32_t seq_bpm;               /* (seq.c) */
static uint32_t rnd_s = 0x1234567u;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }

/* ---- the arp: the k-th note's offset in semitones */
static uint32_t arp_offset_x(uint32_t c, uint32_t shape, uint32_t dir, uint32_t k)
{
    const ext_t *x = &chan[c].x;
    int32_t iv[6];
    uint32_t n, oc = (uint32_t)x->aoct + 1u, L, i, j;
    if (shape < 1u || shape > ARPS) return 0;
    n = ARP_N[shape - 1u];
    for (j = 0; j < n; j++) iv[j] = ARP_IV[shape - 1u][j];
    if (shape == AC_USER) { iv[1] = x->au2 > 24u ? 24 : x->au2; iv[2] = x->au3 > 24u ? 24 : x->au3; n = 3; }
    if (oc > 4u) oc = 4u;
    if (oc > 1u && n > 1u && iv[n - 1u] == 12) n--;             /* (a chord that ends on its octave: that note is the next octave's root) */
    L = n * oc;
    switch (dir) {
    case 1: i = L - 1u - k % L; break;
    case 2: if (L > 1u) { k %= 2u * L - 2u; i = k < L ? k : 2u * L - 2u - k; } else i = 0; break;
    case 3: if (L > 1u) { k %= 2u * L - 2u; i = k < L ? L - 1u - k : k - (L - 2u); } else i = 0; break;
    case 4: i = rnd() % L; break;
    case 5: k %= L; i = k & 1u ? L - 1u - k / 2u : k / 2u; break;
    default: i = k % L; break;
    }
    return (uint32_t)(iv[i % n] + 12 * (int32_t)(i / n));
}

/* ---- the steps' extras: a sorted table, one entry for each step that has any (most have none) */
#define SX_MAX 1024u
enum { SXF_CHORD = 15, SXF_SLIDE = 16, SXF_LOCK = 32 };
typedef struct { uint16_t key; uint8_t lock, fl; } sx_t;          /* key: page << 8 | channel << 4 | step; fl: chord (4 bits), slide, lock set */
static sx_t sx[SX_MAX];
static volatile uint32_t sx_n, sx_busy;
#define SX_KEY(p, c, s) ((uint16_t)((p) << 8 | (c) << 4 | (s)))
static int32_t sx_find(uint32_t key)            /* the entry's index, or -(where it would go) - 1 */
{
    int32_t lo = 0, hi = (int32_t)sx_n - 1;
    while (lo <= hi) {
        const int32_t m = (lo + hi) >> 1;
        if (sx[m].key == key) return m;
        if (sx[m].key < key) lo = m + 1; else hi = m - 1;
    }
    return -lo - 1;
}
static const sx_t *sx_get(uint32_t p, uint32_t c, uint32_t s)       /* (the audio side: nothing while the main loop moves the table) */
{
    int32_t i;
    if (!sx_n || sx_busy) return 0;
    i = sx_find(SX_KEY(p, c, s));
    return i >= 0 ? &sx[i] : 0;
}
static int sx_put(uint32_t p, uint32_t c, uint32_t s, uint32_t lock, uint32_t fl)   /* fl 0: the entry goes; -1: the table is full */
{
    const uint32_t key = SX_KEY(p, c, s);
    int32_t i = sx_find(key);
    sx_busy = 1;
    if (i >= 0) {
        if (fl) { sx[i].lock = (uint8_t)lock; sx[i].fl = (uint8_t)fl; }
        else { memmove(&sx[i], &sx[i + 1], (sx_n - (uint32_t)i - 1u) * sizeof sx[0]); sx_n--; }
    } else if (fl) {
        if (sx_n >= SX_MAX) { sx_busy = 0; return -1; }
        i = -i - 1;
        memmove(&sx[i + 1], &sx[i], (sx_n - (uint32_t)i) * sizeof sx[0]);
        sx[i].key = (uint16_t)key; sx[i].lock = (uint8_t)lock; sx[i].fl = (uint8_t)fl; sx_n++;
    }
    sx_busy = 0;
    return 0;
}
static uint32_t sx_fl(uint32_t p, uint32_t c, uint32_t s) { int32_t i = sx_n ? sx_find(SX_KEY(p, c, s)) : -1; return i >= 0 ? sx[i].fl : 0u; }
static uint32_t sx_lock(uint32_t p, uint32_t c, uint32_t s) { int32_t i = sx_n ? sx_find(SX_KEY(p, c, s)) : -1; return i >= 0 ? sx[i].lock : 0u; }
static void sx_clear_step(uint32_t p, uint32_t c, uint32_t s) { sx_put(p, c, s, 0, 0); }
static void sx_sort(void)                       /* after keys were rewritten in place: in order again (insertion sort, the table is small) */
{
    uint32_t i, j;
    sx_busy = 1;
    for (i = 1; i < sx_n; i++) { sx_t t = sx[i]; for (j = i; j > 0u && sx[j - 1u].key > t.key; j--) sx[j] = sx[j - 1u]; sx[j] = t; }
    sx_busy = 0;
}
static void sx_chan_removed(uint32_t c)         /* the channel's entries go; those of the channels above move down */
{
    uint32_t i, w = 0;
    sx_busy = 1;
    for (i = 0; i < sx_n; i++) {
        const uint32_t k = sx[i].key, kc = k >> 4 & 15u;
        if (kc == c) continue;
        sx[w] = sx[i];
        if (kc > c) sx[w].key = (uint16_t)((k & 0xFF0Fu) | (kc - 1u) << 4);
        w++;
    }
    sx_n = w;
    sx_busy = 0;
}
static void sx_copy_chan(uint32_t from, uint32_t to, uint32_t pages)   /* channel `to` gets `from`'s extras (its notes were copied) */
{
    uint32_t p, s;
    for (p = 0; p < pages; p++) for (s = 0; s < 16u; s++) {
        const uint32_t f = sx_fl(p, from, s), l = sx_lock(p, from, s);
        sx_put(p, to, s, l, f);
    }
}
static void sx_copy_page(uint32_t from, uint32_t to)
{
    uint32_t c, s;
    for (c = 0; c < MAXCH; c++) for (s = 0; s < 16u; s++) sx_put(to, c, s, sx_lock(from, c, s), sx_fl(from, c, s));
}
static void sx_rotate(uint32_t p, uint32_t c, int32_t sh)       /* the rack's knob 3 slid this channel's steps: the extras go with them */
{
    uint32_t i;
    sx_busy = 1;
    for (i = 0; i < sx_n; i++)
        if ((sx[i].key >> 8) == p && (sx[i].key >> 4 & 15u) == c) sx[i].key = (uint16_t)((sx[i].key & 0xFFF0u) | (uint32_t)(((int32_t)(sx[i].key & 15u) + sh) & 15));
    sx_busy = 0;
    sx_sort();
}

/* ---- the second LFO, at the 60 Hz tick: -256..256 */
static int32_t lfo2_step(uint32_t c)
{
    const ext_t *x = &chan[c].x;
    uint32_t inc, ph;
    int32_t v;
    if (x->lrate < 16u) inc = LR_INC[x->lrate];
    else { const uint32_t b4 = LR_BEATS4[(x->lrate - 16u) % 7u], bpm = seq_bpm < 40u ? 40u : seq_bpm > 240u ? 240u : seq_bpm; inc = 65536u * 4u * bpm / (3600u * b4); }
    ph = vc[c].lph2;
    vc[c].lph2 = (uint16_t)(ph + inc);
    switch (x->lshape) {
    case 1: v = 256 - (int32_t)(ph >> 7); break;                                        /* saw down */
    case 2: v = ph < 32768u ? 256 : -256; break;
    case 3: if ((uint16_t)(ph + inc) < ph || !vc[c].lsh) vc[c].lsh = (int16_t)((int32_t)(rnd() & 511u) - 256); v = vc[c].lsh; break;      /* sample and hold: a new level each cycle */
    case 4: v = (int32_t)(ph >> 7) - 256; break;                                        /* ramp up */
    default: v = ph < 32768u ? -256 + (int32_t)(ph >> 6) : 768 - (int32_t)(ph >> 6); break;
    }
    return v;
}

/* ---- parameter locks: a step's value for one of the channel's settings, while its note plays */
static uint32_t lk_get(uint32_t c, uint32_t tgt, uint32_t v, uint32_t mx)       /* the setting, or the step's lock on it (0..127 scaled to 0..mx) */
{
    if (vc[c].lkt == tgt && tgt) return (uint32_t)vc[c].lkv * mx / 127u;
    return v;
}
static void lk_apply_in(uint32_t c)             /* a note began: the locks that are in the sound itself */
{
    inst_t *in = &vc[c].in;
    const uint32_t t = vc[c].lkt, v = vc[c].lkv;
    if (!t) return;
    if (t == LK_VOL) in->vol = (uint8_t)(v * 15u / 127u);
    else if (t == LK_DEC) in->dec = (uint8_t)(v * 15u / 127u);
    else if (t == LK_PAN) in->pan = (uint8_t)(v * 3u / 128u);
    else if (t == LK_TONE) { in->duty = (uint8_t)(v * (chan[c].kind == KD_VRC6_PU || chan[c].kind == KD_ZX ? 7u : 3u) / 127u); in->wave = (uint8_t)(v * 15u / 127u); }
}
