/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the filters (filt.c, RAMC), after Vital (Matt Tytel, GPL-3.0): synthesis/filters/ (all).
 * One filt_t is a stereo filter: a voice has two (FILTER 1, FILTER 2; the core routes: serial / parallel, and which
 * oscillators feed which), and the FILTER effect uses one on the master bus.
 *
 * The models and their styles (Vital's):
 *  ANALOG   (Vital's SallenKey filter: a 2-pole with a non-linear feedback)   styles: 12 dB, 24 dB, notch pass,
 *           notch swap, dual notch band
 *  DIRTY    (Vital's DirtyFilter: a driven, saturating 2/4-pole)             styles: 12 dB, 24 dB, notch pass,
 *           notch swap, dual notch band
 *  LADDER   (Vital's LadderFilter: a 4-pole ladder, tanh stages)               styles: 12 dB, 24 dB, notch pass,
 *           notch swap, dual notch band
 *  DIGITAL  (Vital's DigitalSvf: clean state variable)                         styles: 12 dB, 24 dB, notch pass,
 *           notch swap, dual notch band
 *  DIODE    (Vital's DiodeFilter)                                              styles: low shelf, low cut (the
 *           blend still applies)
 *  FORMANT  (Vital's FormantFilter: vowels on an X/Y pad)                      styles: AOIE, AIUO
 *  COMB     (Vital's CombFilter)                                               styles: low high comb, low band
 *           comb, band spread comb, low high flange +, low band flange +, band spread flange +, low high flange -,
 *           low band flange -, band spread flange -
 *  PHASER   (Vital's PhaserFilter)                                             styles: positive, negative
 * The blend (Vital): 0 low pass .. 0.5 band pass .. 1 high pass for the multi-mode models (Vital's -1..1 "blend"
 * maps here to 0..1); for notch styles, how the notch is made; for comb / phaser, Vital's meaning.
 *
 * Contract:
 *  - filt_ctl at the control rate (every block, n <= CTL): it may do the expensive coefficient maths (tanf, expf
 *    via fastmath) and should smooth any jump (the cutoff moves every block under modulation: no zipper, no click);
 *  - filt_run per sample: budget ~40 flops per stereo frame for the analog / digital / ladder models, ~60 for
 *    formant / comb / phaser; stable at every setting (resonance 1 may self-oscillate but must not blow up: soft
 *    limit inside the feedback); denormal-safe;
 *  - filt_reset on a new voice (not legato) and when the model changes.
 *  - comb / phaser need a delay line: COMB_LEN int16 per channel in the filt_t (keep it small: comb notes down to
 *    ~50 Hz at most: 1024 samples). */
#pragma once
#include "common.h"

enum { FLT_ANALOG, FLT_DIRTY, FLT_LADDER, FLT_DIGITAL, FLT_DIODE, FLT_FORMANT, FLT_COMB, FLT_PHASER, FLT_N };
#define FLT_MAXSTYLE 9
extern const char *const FLT_NAME[FLT_N];
extern const int FLT_NSTYLE[FLT_N];
extern const char *const FLT_STYLE_NAME[FLT_N][FLT_MAXSTYLE];   /* short: <= 10 characters */

#define COMB_LEN 512u
typedef struct {
    float cutoff;                        /* MIDI note (keytrack applied by the core), Vital: 8 .. 136 */
    float reso;                          /* 0..1 */
    float drive;                         /* 0..1 (Vital: 0..20 dB) */
    float blend;                         /* 0..1 */
    float fx, fy;                        /* FORMANT: the vowel pad 0..1; COMB / PHASER: fx = Vital's "cutoff spread"
                                            or the band width */
    float transpose;                     /* FORMANT: formant transpose, semitones -12..12 */
} filt_ctl_t;
typedef struct {
    float s[48];                         /* state + smoothed coefficients */
    int16_t line[2][COMB_LEN];           /* comb delays, int16 (16384 = 1.0) */
    uint32_t w;
    int model, style;
} filt_t;                                /* ~4.3 KB: a voice has two (16 in all: RAM is the limit) */

void filt_reset(filt_t *f, int model, int style);
void filt_ctl(filt_t *f, const filt_ctl_t *c, uint32_t n);   /* this block's settings (n frames follow) */
void filt_run(filt_t *f, float *L, float *R, uint32_t n);    /* in place */
/* main loop: the magnitude response at note (MIDI) for the screen, 0..~4 (1 = 0 dB); approximate is fine */
float filt_response(int model, int style, const filt_ctl_t *c, float note);
