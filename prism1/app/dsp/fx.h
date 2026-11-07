/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the effects (fx.c, RAMC), after Vital (Matt Tytel, GPL-3.0): synthesis/effects/, modules/ (the effect modules).
 * The master bus runs the nine in the patch's order (reorderable, each on / off), as Vital's effect chain.
 *
 * Contract:
 *  - fx_init once (main loop) gives each effect its RAM: the arena sizes below are what the core reserves.
 *  - fx_run(type, ...): n <= CTL stereo frames in place; p[]: the effect's parameters as 0..1 floats (already
 *    modulated, changing every block: smooth what would click); tempo in BPM for the synced times.
 *  - fx_reset(type): clear its state (the effect was switched on, or the patch changed).
 *  - cost per stereo frame: ~60 flops for the light ones (EQ, distortion, filter), ~150 chorus / flanger / phaser /
 *    delay, ~300 the compressor (three bands) and ~400 the reverb. All of them on at once must fit in ~1500.
 *  - delay lines live in the arenas as int16 (16384 = 1.0), or run at half rate where the sound allows (reverb).
 *  - the view: fx_view(type) returns a few floats the screen draws (the compressor's band levels and gain
 *    reductions, the chorus / flanger / phaser LFO phase, the delay's taps): see fx_view_t.
 *
 * The parameters (index: meaning; "sync" values are 0..1 picking from FX_SYNC_DIV, 0 = free time):
 *  CHORUS     0 voices (1..4 pairs), 1 rate (0.01..10 Hz, exp), 2 sync, 3 depth, 4 delay 1 (1..20 ms),
 *             5 delay 2, 6 feedback (-1..1 as 0..1), 7 filter cutoff (note 8..136), 8 filter spread, 9 mix
 *  COMP       0 bands (low+band+high, low+band, band+high, single), 1 attack, 2 release, 3 low gain (-30..+30 dB),
 *             4 band gain, 5 high gain, 6 amount (Vital's OTT "mix of upward and downward" depth), 7 mix
 *             (Vital's multiband compressor: thresholds and ratios fixed at OTT-like defaults)
 *  DELAY      0 style (mono, stereo, ping pong, mid ping pong), 1 time (free: 10 ms..1 s exp), 2 sync,
 *             3 time 2 (right, stereo styles), 4 sync 2, 5 feedback, 6 filter cutoff, 7 filter spread, 8 mix
 *  DIST       0 type (soft clip, hard clip, linear fold, sine fold, bit crush, downsample), 1 drive (-30..+30 dB),
 *             2 filter order (none, pre, post), 3 filter cutoff, 4 filter resonance, 5 filter blend (lp..bp..hp),
 *             6 mix
 *  EQ         0 low mode (shelf, high pass), 1 low cutoff, 2 low gain (-18..18 dB), 3 low reso, 4 band mode (peak,
 *             notch), 5 band cutoff, 6 band gain, 7 band reso, 8 high mode (shelf, low pass), 9 high cutoff,
 *             10 high gain, 11 high reso
 *  FILTER     0 model (FLT_*), 1 style, 2 cutoff, 3 reso, 4 drive, 5 blend, 6 fx, 7 fy, 8 mix   (uses filt.h)
 *  FLANGER    0 rate, 1 sync, 2 feedback (-1..1), 3 center (delay), 4 depth, 5 stereo phase offset, 6 mix
 *  PHASER     0 rate, 1 sync, 2 feedback, 3 center (note), 4 depth (octaves), 5 blend, 6 stereo phase offset,
 *             7 mix
 *  REVERB     0 size, 1 decay (0.1..64 s exp), 2 pre low cut, 3 pre high cut, 4 high shelf cutoff, 5 high shelf
 *             gain, 6 chorus amount, 7 chorus rate, 8 pre-delay (0..300 ms), 9 mix */
#pragma once
#include "common.h"

enum { FXT_CHORUS, FXT_COMP, FXT_DELAY, FXT_DIST, FXT_EQ, FXT_FILTER, FXT_FLANGER, FXT_PHASER, FXT_REVERB, FXT_N };
#define FX_NP 12
extern const char *const FXT_NAME[FXT_N];
extern const int FXT_NP[FXT_N];                        /* parameters used */
extern const char *const FXT_PNAME[FXT_N][FX_NP];      /* short names, <= 9 characters */
/* the synced divisions: 0 = free; then 1/1, 1/2, 1/2T, 1/4., 1/4, 1/4T, 1/8., 1/8, 1/8T, 1/16., 1/16, 1/16T, 1/32 */
#define FX_NSYNC 14
extern const char *const FX_SYNC_NAME[FX_NSYNC];
float fx_sync_beats(float p);                           /* 0..1 -> beats (0 = free) */

/* the arenas (int16 samples), reserved by the core */
#define FXA_CHORUS 4096u
#define FXA_DELAY 65536u                 /* stereo: 2 x 32768: 0.74 s each at full rate, or 1.5 s at half rate */
#define FXA_FLANGER 2048u
#define FXA_REVERB 32768u

typedef struct {
    float a[16];                         /* for the screen, effect-defined */
} fx_view_t;

void fx_init(int16_t *chorus, int16_t *delay, int16_t *flanger, int16_t *reverb);
void fx_reset(int type);
void fx_run(int type, const float *p, float tempo, float *L, float *R, uint32_t n);
const volatile fx_view_t *fx_view(int type);
/* main loop: the EQ's / FILTER's magnitude at a note for the screen (1 = 0 dB) */
float fx_eq_response(const float *p, float note);
