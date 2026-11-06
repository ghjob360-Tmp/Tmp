/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the effects (fx.c) and the master section (master.c). Contract with the core:
 *  - an fx_t instance is one effect slot: a track's FX (an arena of FX_TRACK_ARENA int16) or the master FX
 *    (FX_MASTER_ARENA int16). The core gives the arena; the effect keeps its delay lines there as int16 (scale
 *    16384 = 1.0) and its small state in s[] / u[]. Every type must work in the smaller arena (shorter lines).
 *  - k[8]: K1..K4 (blue, green, white, orange) then the GLO row, 0..1, changing every call; smooth them.
 *  - fx_run: n <= 64 stereo frames, processed in place. The effect's own wet / dry mix is one of its parameters.
 *  - fx_reset: clear the state and the arena (called when the type changes; main-loop context is NOT guaranteed:
 *    it runs in the render, so it must stay quick: clear at most the arena once).
 *  - cost: <= ~150 flops per stereo frame for a track FX (four may run), <= ~350 for the master FX.
 *  - all RAMC (common.h rules).
 *
 * The types and their parameters (K1 K2 K3 K4 | GLO: K1 K2 K3 K4):
 *  ECHO     time, speed (tape-ish modulation of time), feedback, mix | tone, ping-pong, sync-free fine, ducking
 *  CUBE     x size, y size, z feedback (3D plate: a small FDN), mix | damping, modulation, pre-delay, width
 *  TURBO    freq (dual resonant filter), follow (2nd filter tracks the 1st), resonance, freq 2 | drive, envelope
 *           follower amount, mode (lp / bp / hp mix), mix
 *  WIRE     tone (band-limit), phonic (formant), baud (sample-rate hold), telematic (dropouts / noise) |
 *           bits, crackle, wow, mix
 *  KNOCK    freq (hard low-pass), punch (transient boost), rounds (compression), power (output gain) |
 *           attack, release, knee, mix
 *  COIL     tone, turns (spring length / dispersion), damping, mix | drip (chirp amount), width, pre-delay, boing
 *  DOPPLER  freq (pitch shift up / down), delay, feedback, sideband (ring / shifted mix) | window, spread, tone, mix
 *  HALL     distance (size), gate, color (damping / tone), mix | pre-delay, diffusion, modulation, width
 *  BITS     rate, bits, model (clean / dither / aliasing / gritty), mix | jitter, gain, tone, drive */
#pragma once
#include "common.h"

#define FX_TRACK_ARENA 6144u             /* int16: 0.14 s of mono line */
#define FX_MASTER_ARENA 32768u           /* int16: 0.74 s */

typedef struct {
    float s[96];
    uint32_t u[16];
    int16_t *arena;
    uint32_t arena_n;
} fx_t;

void fx_reset(int type, fx_t *f);
void fx_run(int type, fx_t *f, const float k[8], float *L, float *R, uint32_t n);

/* ---- the master section (master.c): EQ, then (the master FX, run by the core), then the output stage ---- */
typedef struct {
    float s[48];
} master_t;
/* EQ: k = low, mid, high (0..1, 0.5 flat, +-12 dB), amount (0 = clean / bypass .. 1 = full) */
void eq_run(master_t *m, const float k[4], float *L, float *R, uint32_t n);
/* output: k = left level, right level, drive (a compressor / limiter that gets louder and denser, distorting high),
 * release (long = pumping); ends with a safety soft clip to +-1 */
void out_run(master_t *m, const float k[4], float *L, float *R, uint32_t n, float *gr_out);   /* gr: gain reduction */
