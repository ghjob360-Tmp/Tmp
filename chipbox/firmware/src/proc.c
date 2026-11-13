/* SPDX-License-Identifier: GPL-3.0-only */
/* proc.c: a channel's own chain, for a channel that needs one (a filter, drive, the ensemble, the LFO on its pan, a send):
 * chip_render draws that channel alone into a buffer of its own, this runs on it, then it joins its chip's mix.
 *   filter    a state variable filter (the trapezoidal one: it stays stable up high), LP / BP / HP, or two LPs for 24 dB;
 *             the cutoff moves with the filter envelope, the second LFO and the key, ramped over the block
 *   drive     a soft clip after a gain
 *   ensemble  copies of the voice through short delays that swing: they drift out of tune with each other, spread left to right
 *   sends     into the chorus, delay and reverb buses (mfx.c)
 * Included by chips.c before chip_render. Integers only. */
#include "proc_tables.h"

#define ENS_SLOTS 4u
#define ENS_LEN 1024u                           /* 23 ms of delay line a channel */
static int16_t ens_buf[ENS_SLOTS][ENS_LEN] POOL;
static uint8_t ens_owner[ENS_SLOTS] = {255, 255, 255, 255};
static struct {
    int32_t f[2][4];                            /* the filter's states: left, right; two stages of two */
    int32_t cut;                                /* the cutoff the last block ended on, Q8 semitones (0: none yet) */
    uint32_t eph;                               /* the ensemble's swing */
    uint16_t epos;
    uint8_t slot;                               /* its delay line + 1 (0: none) */
} pc[MAXCH];

static void proc_reset(void)                    /* the channel list changed: every chain starts clean */
{
    uint32_t i;
    memset(pc, 0, sizeof pc);
    for (i = 0; i < ENS_SLOTS; i++) ens_owner[i] = 255u;
}
static uint32_t lkx(uint32_t c, uint32_t tgt, uint32_t v, uint32_t mx) { return lk_get(c, tgt, v, mx); }
static uint32_t ch_proc(uint32_t c)             /* does the channel need its own chain now */
{
    const ext_t *x = &chan[c].x;
    const uint32_t lt = vc[c].lkt;
    return x->ftype || x->drive || x->uni > 1u || x->scho || x->sdly || x->srev || x->ltgt == LT_PAN
        || lt == LK_DLY || lt == LK_REV || lt == LK_CHO || lt == LK_DRIVE;
}
static int32_t svf_cut(uint32_t c)              /* the cutoff now, Q8 semitones (MIDI note numbers) */
{
    const ext_t *x = &chan[c].x;
    const int32_t cut = (int32_t)lkx(c, LK_CUT, x->fcut, 63u), fe = vc[c].lkt == LK_FENV ? (int32_t)vc[c].lkv * 30 / 127 - 15 : x->fenv;
    int32_t q = (20 << 8) + cut * (112 << 8) / 63;
    q += fe * (int32_t)vc[c].fenv * 72 / 15 / 16;                                       /* the envelope: up to six octaves either way */
    if (x->ltgt == LT_CUT) q += vc[c].lfo2 * (int32_t)x->ldep * 48 / 15;                 /* the LFO: up to four octaves */
    if (x->ktrk) q += ((int32_t)vc[c].note - 60) * (int32_t)x->ktrk * 256 / 3;
    return q < 0 ? 0 : q > (134 << 8) ? 134 << 8 : q;
}
static int32_t svf_g(int32_t q) { const int32_t n = q >> 8, fr = q & 255, a = SVF_G[n], b = SVF_G[n < 134 ? n + 1 : n]; return a + (((b - a) * fr) >> 8); }
static void proc_filter(uint32_t c, int32_t *buf, uint32_t run, uint32_t stereo)
{
    const ext_t *x = &chan[c].x;
    const int32_t k = SVF_K[lkx(c, LK_RES, x->fres, 15u) & 15u], tgt = svf_cut(c), two = x->ftype == 4u;
    int32_t from = pc[c].cut ? pc[c].cut : tgt;
    uint32_t i0, i, ch;
    for (i0 = 0; i0 < run; i0 += 16u) {        /* the coefficients every 16 frames, the cutoff ramped across the block */
        const uint32_t n = run - i0 < 16u ? run - i0 : 16u;
        const int32_t q = from + (tgt - from) * (int32_t)(i0 + n) / (int32_t)run, g = svf_g(q);
        const int32_t den = 4096 + ((g * (g + k)) >> 12), a1 = (1 << 26) / (den ? den : 1), a2 = (g * a1) >> 12, a3 = (g * a2) >> 12;
        for (ch = 0; ch < (stereo ? 2u : 1u); ch++) {
            int32_t *st = pc[c].f[ch];
            for (i = i0; i < i0 + n; i++) {
                int32_t v0 = buf[2u * i + ch], s, v1, v2, out;
                for (s = 0; s < (two ? 2 : 1); s++) {
                    int32_t *z = st + 2 * s;
                    const int32_t v3 = v0 - z[1];
                    v1 = (int32_t)(((int64_t)a1 * z[0] + (int64_t)a2 * v3) >> 14);
                    v2 = z[1] + (int32_t)(((int64_t)a2 * z[0] + (int64_t)a3 * v3) >> 14);
                    z[0] = 2 * v1 - z[0]; z[1] = 2 * v2 - z[1];
                    z[0] = z[0] > 2000000 ? 2000000 : z[0] < -2000000 ? -2000000 : z[0];
                    z[1] = z[1] > 2000000 ? 2000000 : z[1] < -2000000 ? -2000000 : z[1];
                    out = x->ftype == 2u ? v1 : x->ftype == 3u ? v0 - (int32_t)(((int64_t)k * v1) >> 12) - v2 : v2;
                    v0 = out;
                }
                buf[2u * i + ch] = v0;
            }
        }
    }
    pc[c].cut = tgt;
    if (!stereo) for (i = 0; i < run; i++) buf[2u * i + 1u] = buf[2u * i];
}
static int32_t sclip(int32_t v)                 /* a soft clip at about +-24000: 1.5 x - 0.5 x^3 */
{
    const int32_t T = 24000;
    int32_t u;
    if (v >= T) return T;
    if (v <= -T) return -T;
    u = (v << 15) / T;                          /* Q15 */
    return (int32_t)(((int64_t)T * ((3 * u >> 1) - (int32_t)(((int64_t)u * u >> 15) * u >> 16))) >> 15);
}
static void proc_drive(uint32_t c, int32_t *buf, uint32_t run)
{
    const uint32_t d = lkx(c, LK_DRIVE, chan[c].x.drive, 15u);
    const int32_t g = 256 + (int32_t)d * 96, back = 256 * 256 / (256 + (int32_t)d * 24);      /* the gain in, and some of it back out */
    uint32_t i;
    for (i = 0; i < 2u * run; i++) buf[i] = sclip(buf[i] * g >> 8) * back >> 8;
}
static void proc_ens(uint32_t c, int32_t *buf, uint32_t run)
{
    static const int32_t G[4] = {256, 190, 156, 134};
    const ext_t *x = &chan[c].x;
    const uint32_t n = (x->uni > 4u ? 4u : x->uni) - 1u, inc = 30000u + (uint32_t)x->udet * 9000u;      /* the swing: 0.3 to 1.2 Hz */
    const int32_t depth = 20 + (int32_t)x->udet * 10, sp = x->uspr;           /* samples of swing; the spread */
    uint32_t i, j, s = pc[c].slot;
    int16_t *d;
    if (!s) {                                   /* a delay line: a free one, or none (the ensemble waits) */
        for (j = 0; j < ENS_SLOTS; j++) if (ens_owner[j] == 255u || ens_owner[j] >= chan_n || pc[ens_owner[j]].slot != j + 1u) { ens_owner[j] = (uint8_t)c; pc[c].slot = (uint8_t)(j + 1u); memset(ens_buf[j], 0, sizeof ens_buf[j]); break; }
        if (!pc[c].slot) return;
        s = pc[c].slot;
    }
    d = ens_buf[s - 1u];
    for (i = 0; i < run; i++) {
        const int32_t m = (buf[2u * i] + buf[2u * i + 1u]) >> 1;
        int32_t l = buf[2u * i], r = buf[2u * i + 1u];
        uint32_t pos = pc[c].epos;
        d[pos] = (int16_t)(m > 32767 ? 32767 : m < -32767 ? -32767 : m);
        for (j = 0; j < n; j++) {
            const uint32_t ph = (pc[c].eph >> 6) + j * 341u;                    /* the copies swing a third of a turn apart */
            const int32_t dl = (360 << 8) + depth * (int32_t)SIN_T[ph & 1023u] / 64 + (int32_t)j * (37 << 8);      /* 8 ms and on, Q8 samples */
            const uint32_t di = (uint32_t)dl >> 8, fr = (uint32_t)dl & 255u;
            const int32_t a = d[(pos - di) & (ENS_LEN - 1u)], b = d[(pos - di - 1u) & (ENS_LEN - 1u)], v = a + (((b - a) * (int32_t)fr) >> 8);
            const int32_t pan = n == 1u ? (j ? sp : -sp) * 16 : ((int32_t)j * 2 - (int32_t)(n - 1u)) * sp * 16 / (int32_t)(n - 1u);       /* -240..240 */
            l += v * (256 - pan) >> 8; r += v * (256 + pan) >> 8;
        }
        buf[2u * i] = l * G[n] >> 8; buf[2u * i + 1u] = r * G[n] >> 8;
        pc[c].epos = (uint16_t)((pos + 1u) & (ENS_LEN - 1u));
        pc[c].eph += inc >> 4;
    }
}
static void proc_chain(uint32_t c, int32_t *buf, uint32_t run, uint32_t off, uint32_t stereo)       /* off: where this run sits in the block (the buses) */
{
    const ext_t *x = &chan[c].x;
    uint32_t i;
    if (x->ftype) proc_filter(c, buf, run, stereo);
    if (x->drive || vc[c].lkt == LK_DRIVE) proc_drive(c, buf, run);
    if (x->uni > 1u) proc_ens(c, buf, run);
    if (x->ltgt == LT_PAN && x->ldep) {          /* auto-pan */
        const int32_t p = vc[c].lfo2 * (int32_t)x->ldep / 15;
        for (i = 0; i < run; i++) { buf[2u * i] = buf[2u * i] * (256 - p) >> 8; buf[2u * i + 1u] = buf[2u * i + 1u] * (256 + p) >> 8; }
    }
    {   const int32_t sc = (int32_t)lkx(c, LK_CHO, x->scho, 15u), sd = (int32_t)lkx(c, LK_DLY, x->sdly, 15u), sr = (int32_t)lkx(c, LK_REV, x->srev, 15u);
        if (sc) { bus_used |= 1u; for (i = 0; i < run; i++) bus_cho[off + i] += (buf[2u * i] + buf[2u * i + 1u]) * sc >> 5; }
        if (sd) { bus_used |= 2u; for (i = 0; i < 2u * run; i++) bus_dly[2u * off + i] += buf[i] * sd >> 4; }
        if (sr) { bus_used |= 4u; for (i = 0; i < run; i++) bus_rev[off + i] += (buf[2u * i] + buf[2u * i + 1u]) * sr >> 5; }
    }
}
