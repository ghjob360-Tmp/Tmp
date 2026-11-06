/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1's wavetables: 8 frames, each kept at 6 band-limited sizes (a mip level an octave apart: 256 samples with
 * 127 harmonics down to 8 samples with 3), built from a spectrum in the main loop, with a spectral morph applied to
 * the spectrum first (after Vital's spectral morphs: a whole table at a time here, not per voice). */
#pragma once
#include <stdint.h>

#define WT_FRAMES 8
#define WT_LEVELS 6
#define WT_SIZE 504u                     /* 256 + 128 + 64 + 32 + 16 + 8 */
#define WT_HARM 127

enum { WT_BASIC, WT_PULSE, WT_FORMANT, WT_HARMONIC, WT_ORGAN, WT_DIGITAL, WT_N };
enum { SM_NONE, SM_LOWPASS, SM_HIGHPASS, SM_FORMANT, SM_STRETCH, SM_SMEAR, SM_RANDOM, SM_DISPERSE, SM_SHEPARD,
       SM_N };

typedef struct {
    float d[WT_FRAMES][WT_SIZE];
} wt_t;

static const uint16_t WT_OFF[WT_LEVELS] = {0, 256, 384, 448, 480, 496};

/* main loop: the table's frames into dst (~1 ms of work) */
void wt_build(wt_t *dst, int table, int morph, float amount);
/* one sample of level 0 at frame position fp (0..WT_FRAMES-1) and phase ph (0..1): the screen's picture */
float wt_peek(const wt_t *t, float fp, float ph);
