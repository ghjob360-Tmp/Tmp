/* SPDX-License-Identifier: GPL-3.0-only */
/* va.c: three voices that are not a single 8-bit chip, included by chips.c before seq.c.
 *   VA      a virtual analog synth: two oscillators (saw, pulse with its width, triangle), a sub an octave down, noise,
 *           up to seven detuned copies of the first oscillator (the supersaw), up to four notes at once (chords, pads)
 *   SNES    a sampler in the S-DSP's way: 4-bit BRR blocks, the gaussian 4-tap interpolation, 32 kHz pitch steps
 *           (a sample plays at most two octaves above its root, like the chip's 14-bit pitch), 15 sampled instruments
 *   AMIGA   Paula's way: the same instruments as 8-bit samples, no interpolation, the pitch a whole number of clock
 *           periods, the A500's output filter and its LED filter
 * The instruments are made here, at the first SNES or Amiga channel, into RAM as BRR (no flash spent on samples).
 * The channel's filter, drive, ensemble and sends (proc.c) work on all three. Settings: chan[c].x.va[] (ext_def.h). */
#include "va_tables.h"

enum { VA_W1, VA_W2, VA_SEMI, VA_FINE, VA_MIX, VA_SUB, VA_NOISE, VA_PW, VA_UNI, VA_DET, VA_SPR, VA_POLY };
enum { SM_SAMPLE = 0, SM_LED = 1, SM_SEMI = 2, SM_FINE = 3 };      /* the samplers use va[0..3] and va[11] (POLY) */
#define VA_VOICES 4u
#define VA_OSCS 8u                              /* oscillators a channel may run: poly x unison */
static uint32_t is_va(uint32_t k) { return k == KD_VA || k == KD_SNES || k == KD_AMIGA; }
static uint32_t va_poly(uint32_t c) { const uint32_t p = chan[c].x.va[VA_POLY]; return p < 1u ? 1u : p > VA_VOICES ? VA_VOICES : p; }
static uint32_t va_uni(uint32_t c)
{
    const uint32_t u = chan[c].x.va[VA_UNI], p = va_poly(c), m = VA_OSCS / p;
    return chan[c].kind != KD_VA ? 1u : u < 1u ? 1u : u > 7u ? (7u < m ? 7u : m) : u > m ? m : u;
}
static void ext_default(uint32_t kind, ext_t *x)
{
    if (kind == KD_VA) { static const uint8_t D[12] = {0, 0, 12, 4, 0, 4, 0, 0, 3, 6, 10, 1}; memcpy(x->va, D, 12); }       /* a supersaw with a sub: three copies, a little detune, wide */
    else if (kind == KD_SNES || kind == KD_AMIGA) { memset(x->va, 0, sizeof x->va); x->va[SM_SEMI] = 12; x->va[VA_POLY] = 1; x->va[SM_SAMPLE] = 2; }
}

/* ---- the voices of a channel (a mono channel uses voice 0 and the channel's own envelope) */
typedef struct {
    uint8_t on, note, vel, st;                  /* st: ST_* */
    uint16_t env, eg;                           /* the envelope Q12 (0..3840 as the tracker's), the gain the last block ended on */
    uint32_t ph[7], ph2, phs, inc[7], inc2, incs;       /* the oscillators */
    uint32_t pos, sinc;                         /* the samplers: where in the sample (blocks << 20 | samples in the block << 16 | fraction), the step */
    int16_t hist[4];                            /* the last four decoded samples (the gaussian reads them): sample k at k & 3 */
    int16_t brr[16]; int32_t p1, p2;            /* the block being decoded; the BRR filter's past */
    uint32_t nxt;                               /* the next sample to decode (counted as pos is) */
    uint8_t age;
} vav_t;
static vav_t vav[MAXCH][VA_VOICES];
static uint32_t va_rng = 0x2468ACEu;
static int32_t va_lp[MAXCH][2], va_led[MAXCH][4];       /* the Amiga's output filters */
static uint32_t va_g[MAXCH];                    /* the channel's level that is not the envelope (VOL, duck, tremolo), Q8: set at the tick */

/* ---- the sample bank: made once, as BRR */
#define BANK_BLK 1600u                          /* 9-byte blocks: 25600 samples, 14.4 KB */
#define SAMPLES 15u
static uint8_t bank[BANK_BLK][9] POOL;
static uint32_t bank_n, bank_ready;
typedef struct { uint16_t start, len, loop; int16_t root; } smp_t;      /* blocks: first, how many, where the loop starts; root: its note, Q8 */
static smp_t smp[SAMPLES];
static const char *const SMP_NAME[SAMPLES] = {"SAW", "SQUARE", "STRINGS", "CHOIR", "BRASS", "E.PIANO", "BASS", "BELL", "PAD", "ORGAN", "FLUTE", "PLUCK", "ORCHHIT", "SYNBASS", "MARIMBA"};
static int32_t brr_dec1(int32_t s, uint32_t f, int32_t p1, int32_t p2)
{
    switch (f) {
    case 1: s += p1 - (p1 >> 4); break;
    case 2: s += 2 * p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4); break;
    case 3: s += 2 * p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4); break;
    default: break;
    }
    return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
}
static void brr_decode(const uint8_t *b, int16_t *out, int32_t *p1, int32_t *p2)       /* a block of 16 samples */
{
    const uint32_t sh = b[0] >> 4, f = b[0] >> 2 & 3u;
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        int32_t n = (int32_t)(b[1u + i / 2u] >> (i & 1u ? 0 : 4) & 15u), s;
        if (n > 7) n -= 16;
        s = sh <= 12u ? (n << sh) >> 1 : (n < 0 ? -2048 : 0);
        s = brr_dec1(s, f, *p1, *p2);
        *p2 = *p1; *p1 = s; out[i] = (int16_t)s;
    }
}
static void brr_encode(const int16_t *in, uint8_t *b, int32_t *p1, int32_t *p2)       /* the filter and shift that lose least */
{
    uint32_t f, sh, i, bf = 0, bs = 12;
    uint32_t best = 0xFFFFFFFFu;
    for (f = 0; f < 4u; f++)
        for (sh = 0; sh <= 12u; sh++) {
            int32_t a = *p1, z = *p2;
            uint32_t err = 0;
            for (i = 0; i < 16u && err < best; i++) {
                const int32_t pred = brr_dec1(0, f, a, z), want = in[i] - pred;
                int32_t n = sh ? ((want << 1) + (1 << (sh - 1))) >> sh : want << 1, s, e;
                n = n > 7 ? 7 : n < -8 ? -8 : n;
                s = brr_dec1((n << sh) >> 1, f, a, z);
                e = s - in[i]; err += (uint32_t)(e < 0 ? -e : e);
                z = a; a = s;
            }
            if (err < best) { best = err; bf = f; bs = sh; }
        }
    b[0] = (uint8_t)(bs << 4 | bf << 2);
    for (i = 0; i < 8u; i++) b[1u + i] = 0;
    for (i = 0; i < 16u; i++) {
        const int32_t pred = brr_dec1(0, bf, *p1, *p2), want = in[i] - pred;
        int32_t n = bs ? ((want << 1) + (1 << (bs - 1))) >> bs : want << 1, s;
        n = n > 7 ? 7 : n < -8 ? -8 : n;
        s = brr_dec1((n << bs) >> 1, bf, *p1, *p2);
        *p2 = *p1; *p1 = s;
        b[1u + i / 2u] |= (uint8_t)((uint32_t)(n & 15) << (i & 1u ? 0 : 4));
    }
}
static int32_t sn(uint32_t ph) { return SIN_T[(ph >> 22) & 1023u]; }        /* a sine, Q14, from a 32-bit phase */
static void bank_add(uint32_t s, const int16_t *w, uint32_t len, uint32_t loop, int32_t root)      /* len, loop: samples (multiples of 16) */
{
    uint32_t i;
    int32_t p1 = 0, p2 = 0;
    if (bank_n + len / 16u > BANK_BLK) return;
    smp[s].start = (uint16_t)bank_n; smp[s].len = (uint16_t)(len / 16u); smp[s].loop = (uint16_t)(loop / 16u); smp[s].root = (int16_t)root;
    for (i = 0; i < len; i += 16u) brr_encode(w + i, bank[bank_n++], &p1, &p2);
}
/* the instruments: a cycle is 128 samples at 32 kHz (250 Hz, a note a little over B3); attacks before the loop where the sound has one */
#define CYC 128u
#define ROOT_Q8 15158                           /* 59.21 x 256: the note of a 250 Hz cycle */
static int32_t harm(uint32_t i, const uint8_t *amp, uint32_t nh, uint32_t cyc)      /* one sample of a cycle from harmonic levels (0..255) */
{
    uint32_t h;
    int32_t v = 0;
    for (h = 0; h < nh; h++) if (amp[h]) v += sn((uint32_t)(((uint64_t)i * (h + 1u) << 32) / cyc)) * amp[h] >> 8;
    return v;
}
static void bank_make(void)                     /* (main loop: about a tenth of a second, once) */
{
    static int16_t w[6144];
    static const uint8_t SAW[40] = {255, 128, 85, 64, 51, 42, 36, 32, 28, 25, 23, 21, 19, 18, 17, 16, 15, 14, 13, 12, 12, 11, 11, 10, 10, 9, 9, 9, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 6};
    uint8_t a[40];
    uint32_t s, i, h, n;
    if (bank_ready) return;
    bank_n = 0;
    for (s = 0; s < SAMPLES; s++) {
        uint32_t att = 0;
        int32_t pk = 1;
        memset(a, 0, sizeof a);
        switch (s) {
        case 0: memcpy(a, SAW, 40); break;
        case 1: for (h = 0; h < 40u; h += 2u) a[h] = SAW[h]; break;
        case 2: for (h = 0; h < 40u; h++) a[h] = (uint8_t)(SAW[h] * (h < 8u ? 255u : 255u * 8u / (h + 1u)) >> 8); break;      /* a darker saw */
        case 3: { static const uint16_t F[3] = {700, 1150, 2600}; for (h = 0; h < 40u; h++) { uint32_t fz = 250u * (h + 1u), k, g = 20; for (k = 0; k < 3u; k++) { uint32_t d = fz > F[k] ? fz - F[k] : F[k] - fz; g += d < 350u ? (350u - d) * (k == 2u ? 90u : 200u) / 350u : 0u; } a[h] = (uint8_t)(g > 255u ? 255u : g); } break; }     /* "aah": three formants */
        case 4: for (h = 0; h < 40u; h++) { uint32_t fz = 250u * (h + 1u); a[h] = (uint8_t)(SAW[h] * (fz > 900u && fz < 2500u ? 2u : 1u) > 255u ? 255u : SAW[h] * (fz > 900u && fz < 2500u ? 2u : 1u)); } break;
        case 5: att = 4096; break;
        case 6: att = 2048; break;
        case 7: att = 6016; break;
        case 8: a[0] = 255; a[1] = 60; a[2] = 30; a[3] = 12; a[4] = 6; break;
        case 9: a[0] = 255; a[1] = 200; a[2] = 140; a[3] = 120; a[5] = 80; a[7] = 60; break;
        case 10: a[0] = 255; a[1] = 40; a[2] = 12; att = 1024; break;
        case 11: att = 4096; break;
        case 12: att = 6016; break;
        case 13: for (h = 0; h < 24u; h++) a[h] = (uint8_t)(SAW[h] * (h < 3u ? 255u : 255u * 3u / (h + 1u)) >> 8); a[0] = 255; break;       /* a round saw bass */
        default: att = 3072; break;
        }
        n = att + CYC;
        for (i = 0; i < n; i++) {
            int32_t v = 0;
            const uint32_t t = i < att ? i : att + (i - att) % CYC;       /* (the loop's cycle follows on from the attack) */
            switch (s) {
            case 5: {   const int32_t d = (int32_t)(att - (t < att ? t : att)) * 256 / (int32_t)att;     /* an electric piano: a sine, its octave, a tine that dies away */
                v = sn((uint32_t)((uint64_t)t * 0x2000000u)) + (sn((uint32_t)((uint64_t)t * 0x4000000u)) * (40 + d / 2) >> 8) + (sn((uint32_t)((uint64_t)t * 0x1C000000u)) * d * d >> 17); break; }
            case 6: {   const int32_t d = (int32_t)(att - (t < att ? t : att)) * 256 / (int32_t)att;     /* a bass: bright when plucked, round after */
                for (h = 0; h < 12u; h++) { v += sn((uint32_t)((uint64_t)t * (h + 1u) * 0x2000000u)) * (h ? (int32_t)(SAW[h] * (64 + d * 3 / 4)) >> 8 : 255) >> 8; }
                break; }
            case 7: {   const int32_t d = (int32_t)(att - (t < att ? t : att)) * 256 / (int32_t)att;     /* a bell: partials that are not harmonics, the high ones dying first */
                static const uint32_t P[4] = {0x2000000u, 0x5850000u, 0xAC00000u, 0x11C00000u};
                v = sn((uint32_t)((uint64_t)t * P[0])) * (64 + d * 3 / 4) >> 8;
                for (h = 1; h < 4u; h++) { v += sn((uint32_t)((uint64_t)t * P[h])) * d * d / (int32_t)(256u * (h + 1u)) >> 8; }
                break; }
            case 10: v = harm(t, a, 3, CYC) + (t < att ? (int32_t)((va_rng = va_rng * 1664525u + 1013904223u) >> 20) - 2048 : 0) * (int32_t)(att - t) / (int32_t)att; break;
            case 11: v = 0; break;              /* (Karplus-Strong below) */
            case 12: {  const int32_t d = (int32_t)(att - (t < att ? t : att)) * 256 / (int32_t)att;     /* an orchestra hit: a stacked chord and a burst, gone by the loop */
                static const uint32_t P[5] = {0x2000000u, 0x2FE0000u, 0x4000000u, 0x5080000u, 0x8000000u};
                for (h = 0; h < 5u; h++) { uint32_t k; for (k = 1; k < 6u; k++) v += sn((uint32_t)((uint64_t)t * P[h] * k)) / (int32_t)(k * 5u); }
                v = v * d * d >> 16;
                if (t < 600u) v += ((int32_t)((va_rng = va_rng * 1664525u + 1013904223u) >> 18) - 8192) * (int32_t)(600u - t) / 600;
                break; }
            case 14: {  const int32_t d = (int32_t)(att - (t < att ? t : att)) * 256 / (int32_t)att;     /* a marimba: a sine and its fourth harmonic, short */
                v = (sn((uint32_t)((uint64_t)t * 0x2000000u)) * (24 + d) >> 8) + (sn((uint32_t)((uint64_t)t * 0x8000000u)) * d * d >> 17); break; }
            default: v = harm(t, a, 40, CYC); break;
            }
            w[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        }
        if (s == 11) {                          /* a plucked string: noise round a 128-sample loop, averaged as it goes */
            for (i = 0; i < CYC; i++) w[i] = (int16_t)((int32_t)((va_rng = va_rng * 1664525u + 1013904223u) >> 18) - 8192);
            for (i = CYC; i < n; i++) w[i] = (int16_t)((w[i - CYC] + w[i - CYC + 1u]) * 1015 / 2048);
        }
        for (i = 0; i < n; i++) { int32_t v = w[i] < 0 ? -w[i] : w[i]; if (v > pk) pk = v; }
        for (i = 0; i < n; i++) w[i] = (int16_t)(w[i] * 22000 / pk);       /* all as loud */
        if (s == 12) for (i = att; i < n; i++) w[i] = 0;
        bank_add(s, w, n, att, ROOT_Q8);
    }
    bank_ready = 1;
}

/* ---- notes: a voice for each (a mono channel: voice 0 follows the channel's note) */
static void va_on(uint32_t c, uint32_t note, uint32_t vel, uint32_t add)      /* add 0: a new note (a mono channel retriggers), 1: a chord's note, 2: the arp's gate opens, 3: a slide */
{
    const uint32_t p = va_poly(c);
    uint32_t v = 0, i, best = 0;
    vav_t *x;
    if (p > 1u && add != 2u && add != 3u) {     /* poly: the same note again, a free voice, or the oldest */
        for (i = 0; i < p; i++) if (vav[c][i].note == note && vav[c][i].st != ST_IDLE) { v = i; goto have; }
        for (i = 0; i < p; i++) if (vav[c][i].st == ST_IDLE) { v = i; goto have; }
        for (i = 0; i < p; i++) if (vav[c][i].age > best) { best = vav[c][i].age; v = i; }
    }
have:
    x = &vav[c][v];
    if (add == 3u) { x->note = (uint8_t)note; return; }
    if (add == 0u && p > 1u) for (i = 0; i < p; i++) vav[c][i].age++;
    x->on = 1; x->note = (uint8_t)note; x->vel = (uint8_t)vel; x->age = 0;
    x->st = ST_ATK; x->env = 0;
    if (!vc[c].in.atk) { x->env = 3840u; x->st = ST_DEC; }
    if (chan[c].kind != KD_VA) {                /* a sampler: from the start of the sample */
        x->pos = 0; x->nxt = 0; x->p1 = x->p2 = 0; memset(x->hist, 0, sizeof x->hist);
    } else if (add == 0u || add == 1u) {        /* the oscillators start where they are, spread a little (free running) */
        for (i = 0; i < 7u; i++) x->ph[i] += (va_rng = va_rng * 1664525u + 1013904223u) & 0x3FFFFFFFu;
    }
}
static void va_off(uint32_t c, uint32_t note)   /* 255: every voice */
{
    uint32_t i;
    for (i = 0; i < VA_VOICES; i++)
        if ((note == 255u || vav[c][i].note == note) && vav[c][i].on) { vav[c][i].on = 0; if (vav[c][i].st != ST_IDLE) vav[c][i].st = ST_REL; }
}
static uint32_t va_tick(uint32_t c)             /* 60 Hz: the poly voices' envelopes; 1 while one sounds */
{
    const inst_t *in = &vc[c].in;
    uint32_t i, busy = 0, e = 0;
    for (i = 0; i < VA_VOICES; i++) {
        vav_t *x = &vav[c][i];
        switch (x->st) {
        case ST_ATK: x->env = (uint16_t)(x->env + (in->atk ? 3840u / in->atk : 3840u)); if (x->env >= 3840u) { x->env = 3840u; x->st = ST_DEC; } break;
        case ST_DEC: {
            const uint32_t s = (uint32_t)in->sus * 256u, d = 3840u / ((uint32_t)in->dec * 2u + 1u);
            if (x->env > s) x->env = (uint16_t)(x->env - s > d ? x->env - d : s);
            if (x->env <= s) x->st = ST_SUS;
            if (!x->on) x->st = ST_REL;
            break; }
        case ST_SUS: if (!x->on) x->st = ST_REL; break;
        case ST_REL: { const uint32_t d = 3840u / ((uint32_t)in->rel * 2u + 1u); if (x->env > d) x->env = (uint16_t)(x->env - d); else { x->env = 0; x->st = ST_IDLE; } break; }
        default: break;
        }
        if (x->st != ST_IDLE) { busy = 1; if (x->env > e) e = x->env; }
    }
    vc[c].env = (uint16_t)e;                    /* (what the meter shows) */
    return busy;
}
static int32_t va_mod;                          /* the channel's pitch now, less its note: vibrato, bend, LFO, glide (set_osc) */
static void va_set(uint32_t c, uint32_t pq, uint32_t vol)     /* the tick's pitch and level into the voices' steps */
{
    const ext_t *x = &chan[c].x;
    const uint32_t p = va_poly(c), u = va_uni(c), kind = chan[c].kind;
    const int32_t mod = (int32_t)pq - (((int32_t)vc[c].note + (vc[c].in.arp && vc[c].in.arp <= ARPS ? vc[c].arp_off : 0)) << 8);
    uint32_t v, k;
    va_mod = mod;
    va_g[c] = vol;
    for (v = 0; v < p; v++) {
        vav_t *y = &vav[c][v];
        int32_t q = p == 1u ? (int32_t)pq : ((int32_t)y->note << 8) + mod;
        if (kind == KD_VA) {
            const int32_t semi = ((int32_t)x->va[VA_SEMI] - 12) * 256 + (int32_t)x->va[VA_FINE] * 2;
            for (k = 0; k < u; k++) {           /* the copies spread evenly over DETUNE (up to +-50 cents) */
                const int32_t dq = u > 1u ? ((int32_t)k * 2 - (int32_t)(u - 1u)) * (int32_t)x->va[VA_DET] * 128 / (15 * (int32_t)(u - 1u)) : 0;
                int32_t qq = q + dq; qq = qq < 0 ? 0 : qq > (127 << 8) ? 127 << 8 : qq;
                y->inc[k] = interp32(NOTE_INC, (uint32_t)qq) * OS;
            }
            {   int32_t q2 = q + semi; q2 = q2 < 0 ? 0 : q2 > (127 << 8) ? 127 << 8 : q2; y->inc2 = interp32(NOTE_INC, (uint32_t)q2) * OS; }
            y->incs = y->inc[0] >> 1;
        } else {                                /* a sampler: the step through the sample, Q16 a sample at 44.1 kHz. The cycles were made at 250 Hz at 32 kHz,
                                                   so the step is the note's NOTE_INC / 128 (NOTE_INC: 2^32 a cycle at FS x OS) */
            int32_t d = q + ((int32_t)x->va[SM_SEMI] - 12) * 256 + (int32_t)x->va[SM_FINE] * 2;
            uint32_t inc;
            if (kind == KD_SNES && d > ROOT_Q8 + 24 * 256) d = ROOT_Q8 + 24 * 256;      /* the 14-bit pitch: two octaves up at most */
            d = d < 0 ? 0 : d > (127 << 8) ? 127 << 8 : d;
            inc = interp32(NOTE_INC, (uint32_t)d) >> 7;
            if (kind == KD_AMIGA) {             /* Paula: the rate is its clock over a whole period (124 at the fastest) */
                const uint32_t rate = (uint32_t)(((uint64_t)inc * FS) >> 16);
                uint32_t per = rate ? 3546895u / rate : 65535u;
                per = per < 124u ? 124u : per;
                inc = (3546895u / per << 10) / FS << 6;
            }
            y->sinc = inc;
        }
    }
}
static int32_t blep(uint32_t t, uint32_t dt)    /* the band-limited step's correction near a jump, Q15 */
{
    if (t < dt) { const int32_t x = (int32_t)(((t >> 16) << 15) / ((dt >> 16) + 1u)); return 2 * x - (x * x >> 15) - 32768; }
    if (t > ~dt) { const int32_t x = (int32_t)((((t - ~dt) >> 16) << 15) / ((dt >> 16) + 1u)) - 32768; return (x * x >> 15) + 2 * x + 32768; }
    return 0;
}
static int32_t va_osc(uint32_t w, uint32_t ph, uint32_t dt, uint32_t pw)      /* one oscillator, Q15 */
{
    switch (w) {
    case 1: { int32_t v = ph < pw ? 32767 : -32767; v += blep(ph, dt); v -= blep(ph - pw, dt); return v; }
    case 2: { const int32_t s = (int32_t)(ph >> 16); return (s < 32768 ? s * 2 : (65535 - s) * 2) - 32768; }
    case 3: return SIN_T[ph >> 22] * 2;
    default: return (int32_t)(ph >> 16) - 32768 - blep(ph, dt);
    }
}
static const smp_t *va_smp(uint32_t c) { return &smp[chan[c].x.va[SM_SAMPLE] % SAMPLES]; }
static void sm_fill(uint32_t c, vav_t *y, uint32_t upto)     /* decode the sample in order (the BRR filter carries on from block to block) up to sample upto */
{
    const smp_t *sm = va_smp(c);
    while (y->nxt <= upto) {
        const uint32_t k = y->nxt;
        uint32_t b = k >> 4;
        if (b >= sm->len) b = sm->loop + (b - sm->len) % (uint32_t)(sm->len > sm->loop ? sm->len - sm->loop : 1u);
        if (!(k & 15u)) brr_decode(bank[sm->start + b], y->brr, &y->p1, &y->p2);
        y->hist[k & 3u] = y->brr[k & 15u];
        y->nxt++;
    }
}
static void r_va(int32_t *out, uint32_t n, const uint8_t *act, uint32_t na)
{
    uint32_t a, i, v, k;
    for (a = 0; a < na; a++) {
        const uint32_t c = act[a], p = va_poly(c), u = va_uni(c), kind = chan[c].kind;
        const ext_t *x = &chan[c].x;
        const uint32_t pw = 0x80000000u + (uint32_t)x->va[VA_PW] * 0x7000000u, pan = vc[c].in.pan > 2u ? 1u : vc[c].in.pan;
        const int32_t m2 = x->va[VA_W2] < 3u ? (int32_t)x->va[VA_MIX] * 17 : 0, ms = (int32_t)x->va[VA_SUB] * 17, mn = (int32_t)x->va[VA_NOISE] * 12;
        const int32_t sp = (int32_t)x->va[VA_SPR];
        if (kind != KD_VA && !bank_ready) continue;
        for (v = 0; v < p; v++) {
            vav_t *y = &vav[c][v];
            int32_t g0, g1;
            if (p == 1u) g1 = (int32_t)(va_g[c] * 17u);                             /* mono: the channel's envelope is in vol_q (0..240) */
            else { if (y->st == ST_IDLE) continue; g1 = (int32_t)(y->env * (y->vel + 1u) >> 11) * (int32_t)(vc[c].in.vol + 1u) >> 4; g1 = g1 * (int32_t)va_g[c] / 240 * 17; }
            g0 = y->eg; y->eg = (uint16_t)(g1 > 65535 ? 65535 : g1);
            if (!g0 && !g1) continue;
            for (i = 0; i < n; i++) {
                const int32_t g = g0 + (g1 - g0) * (int32_t)i / (int32_t)n;          /* the level ramps over the block: no zipper */
                int32_t l = 0, r = 0, s;
                if (kind == KD_VA) {
                    for (k = 0; k < u; k++) {
                        const int32_t o = va_osc(x->va[VA_W1], y->ph[k], y->inc[k], pw), pn = u > 1u ? ((int32_t)k * 2 - (int32_t)(u - 1u)) * sp * 16 / (int32_t)(u - 1u) : 0;
                        l += o * (256 - pn) >> 8; r += o * (256 + pn) >> 8;
                        y->ph[k] += y->inc[k];
                    }
                    if (u > 1u) { l = l * 3 / (int32_t)(u + 2u); r = r * 3 / (int32_t)(u + 2u); }
                    if (m2) { s = va_osc(x->va[VA_W2], y->ph2, y->inc2, pw) * m2 >> 8; l += s; r += s; y->ph2 += y->inc2; }
                    if (ms) { s = (y->phs < 0x80000000u ? 20000 : -20000) * ms >> 8; l += s; r += s; y->phs += y->incs; }
                    if (mn) { va_rng = va_rng * 1664525u + 1013904223u; s = ((int32_t)(va_rng >> 16) - 32768) * mn >> 8; l += s; r += s; }
                } else {
                    const uint32_t si = y->pos >> 16;
                    sm_fill(c, y, si + 2u);
                    if (kind == KD_SNES) {      /* the gaussian over four samples */
                        const int16_t *gk = GAUSS4[(y->pos >> 8) & 255u];
                        s = (y->hist[(si - 1u) & 3u] * gk[0] + y->hist[si & 3u] * gk[1] + y->hist[(si + 1u) & 3u] * gk[2] + y->hist[(si + 2u) & 3u] * gk[3]) >> 11;
                    } else s = (y->hist[si & 3u] >> 8) << 8;                          /* 8 bits, held */
                    y->pos += y->sinc;
                    {   const smp_t *sm = va_smp(c);                                  /* far past the end: the counts go back by whole loops */
                        const uint32_t L = (uint32_t)(sm->len - sm->loop) << 4;
                        if (L && (y->pos >> 16) >= ((uint32_t)sm->len << 4) + L) { y->pos -= L << 16; y->nxt -= L; } }
                    l = r = s;
                }
                l = (l >> 2) * g >> 12; r = (r >> 2) * g >> 12;
                if (pan == 0u) { l += r >> 1; r = r >> 2; } else if (pan == 2u) { r += l >> 1; l = l >> 2; }
                out[2u * i] += l; out[2u * i + 1u] += r;
            }
        }
        if (kind == KD_AMIGA) {                 /* the A500's output: a gentle low-pass always, the LED filter's 12 dB when on */
            for (i = 0; i < n; i++) for (k = 0; k < 2u; k++) {
                int32_t s = out[2u * i + k];
                va_lp[c][k] += (s - va_lp[c][k]) * 21 >> 5; s = va_lp[c][k];
                if (x->va[SM_LED]) { va_led[c][k] += (s - va_led[c][k]) * 13 >> 5; va_led[c][k + 2u] += (va_led[c][k] - va_led[c][k + 2u]) * 13 >> 5; s = va_led[c][k + 2u]; }
                out[2u * i + k] = s;
            }
        }
    }
}
