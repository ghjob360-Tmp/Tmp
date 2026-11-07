/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: wavetables, the wavetable oscillator and the sample oscillator (wt.c, osc.c), after Vital (Matt
 * Tytel, GPL-3.0): synthesis/producers/synth_oscillator.*, lookups/wavetable.*, producers/sample_source.*.
 *
 * ---- tables (wt.c, main loop only) ----
 * A table is WT_FRAMES frames; each frame is kept band-limited at WT_LEVELS sizes (mip levels an octave apart:
 * 256 samples with up to 127 harmonics, 128 with 63, ... 8 with 3), as int16 (32767 = 1.0), normalised so the
 * loudest frame peaks at ~0.9. Tables are built from a recipe (the factory tables: WT_NFACT of them, each a
 * spectrum or a time-domain shape per frame, generated in code: no table data is stored) with a spectral morph
 * applied to every frame's spectrum before the inverse transform (Vital's morphs, but per table, not per voice:
 * the FM-1 cannot afford an FFT per voice per block).
 * The build is cut into steps so the main loop never stalls: wt_build_begin, then wt_build_step until it returns 1
 * (each step <= ~2 ms on the device: e.g. a frame). The scratch is the builder's own (static) memory.
 *
 * ---- the wavetable oscillator (osc.c, RAMC) ----
 * Per voice per oscillator: up to MAXUNI unison voices, each its own phase. Per block the core gives an osc_ctl_t
 * (all already modulated); osc_run ADDS its stereo output into L / R (n <= CTL frames) and also writes the mono sum
 * (before pan, after level) into mono[] (for other oscillators' FM / RM next). Everything per sample must be cheap:
 * budget ~25 flops per unison voice per sample incl. the warp (frame crossfade + linear interpolation + mip pick by
 * the voice's pitch: pick the level with no harmonic above ~20 kHz, crossfade toward the next level near the edge
 * is welcome but optional).
 * Unison (Vital): voices spread over +-detune semitones with the curve detune_power (Vital's "detune power"),
 * arranged by the stack style (WT_STACK_*: Vital's unison stack types: normal, centre drop octave, centre drop 2
 * octaves, octave, 2 octaves, power chord, 2x power chord, major, minor, harmonic series, odd harmonics); blend
 * (0 = only the centre voice(s) .. 1 = all equal); stereo spread pans them L/R alternately; frame_spread offsets
 * each voice's frame position (Vital's "frame spread"). Gain: 1/sqrt(n) so more voices are not louder.
 * Phase: phase (0..1) is where every voice starts on a note; rand_phase (0..1) how much each voice's start is
 * randomised (Vital: random phase).
 * Warps (Vital's "distortion", applied to the phase before the table read, formulas as Vital's
 * synth_oscillator.cpp: sync, formant, quantize, bend, squeeze, pulse width, then FM / RM by another source):
 *  WARP_FM_*: the source sample (src[i], -1..1) moves the phase: ph + amount * k * src (Vital scales it);
 *  WARP_RM_*: the output multiplied: out * lerp(1, src, amount).
 *  The core passes src: the other oscillator's mono[] (from this block if it ran before, else the last block), or
 *  the sample oscillator's.
 *
 * ---- the sample oscillator (osc.c, RAMC) ----
 * Plays a sample keytracked or fixed pitch, looped or not, with level and pan. The factory set (SMP_NFACT
 * sources: noises (white, pink, brown: these may be computed live rather than stored), and short generated loops
 * and hits: vinyl crackle, breath, tape hiss, metallic hits, a sub thump, ...) is generated at boot by smp_init
 * into RAM, SMP_RAM bytes at most in all. It also writes mono[] for FM / RM. */
#pragma once
#include "common.h"

#define WT_FRAMES 32
#define WT_LEVELS 6
#define WT_SIZE 504u                     /* 256 + 128 + 64 + 32 + 16 + 8 */
#define WT_HARM 127
#define SMP_RAM 32768u                   /* bytes: all the factory samples' RAM */
typedef struct {
    int16_t d[WT_FRAMES][WT_SIZE];       /* 32,256 bytes */
} wt_t;
extern const uint16_t WT_OFF[WT_LEVELS];  /* 0, 256, 384, 448, 480, 496 */

/* spectral morphs (Vital's list) */
enum { SM_NONE, SM_VOCODE, SM_FORMSCALE, SM_HARMSTRETCH, SM_INHARMSTRETCH, SM_SMEAR, SM_RANDOMAMP, SM_LOWPASS,
       SM_HIGHPASS, SM_DISPERSE, SM_SHEPARD, SM_SKEW, SM_N };
/* warps (Vital's "distortion") */
enum { WARP_NONE, WARP_SYNC, WARP_FORMANT, WARP_QUANTIZE, WARP_BEND, WARP_SQUEEZE, WARP_PULSE, WARP_FM_A,
       WARP_FM_B, WARP_FM_SMP, WARP_RM_A, WARP_RM_B, WARP_RM_SMP, WARP_N };
/* unison stacks (Vital's) */
enum { WT_STACK_NORMAL, WT_STACK_DROP1, WT_STACK_DROP2, WT_STACK_OCT, WT_STACK_OCT2, WT_STACK_POWER,
       WT_STACK_POWER2, WT_STACK_MAJOR, WT_STACK_MINOR, WT_STACK_HARM, WT_STACK_ODDHARM, WT_STACK_N };

/* the factory tables: WT_NFACT recipes; names for the screen */
extern const int WT_NFACT;
extern const char *const WT_NAME[];      /* up to 10 characters, upper case */
extern const char *const SM_NAME[SM_N];
extern const char *const WARP_NAME[WARP_N];
extern const char *const STACK_NAME[WT_STACK_N];

/* main loop: build table `table` with morph `morph` at `amount` (0..1) into dst, in steps */
typedef struct {
    wt_t *dst;
    int table, morph, step;
    float amount;
} wt_job_t;
void wt_build_begin(wt_job_t *j, wt_t *dst, int table, int morph, float amount);
int wt_build_step(wt_job_t *j);          /* 1 = done */
/* main loop: one sample of level 0 at frame position fp (0..WT_FRAMES-1, interpolated) and phase ph (0..1),
 * for the screen; and a frame's spectrum magnitude (harmonic 1..64) for the spectrum view */
float wt_peek(const wt_t *t, float fp, float ph);
void wt_spectrum(int table, int morph, float amount, float fp, float mag[64]);   /* from the recipe, 0..1 */
/* the warp applied to a phase, for the screen (the same formula as the oscillator; FM / RM return ph unchanged) */
float wt_warp_phase(int warp, float amount, float ph);

/* ---------------------------------------------------------- the wavetable oscillator --- */
typedef struct {
    const wt_t *wt;                      /* NULL = silent */
    float note;                          /* MIDI note incl. transpose, tune, pitch modulation, glide */
    float frame;                         /* 0..WT_FRAMES-1 */
    float frame_spread;                  /* 0..1 */
    int unison;                          /* 1..MAXUNI */
    float detune;                        /* 0..1 (Vital: 0..1 maps to 0..~1 semitone spread at power 1.5) */
    float detune_power;                  /* -1..1 */
    float blend;                         /* 0..1 */
    float spread;                        /* stereo spread 0..1 */
    int stack;                           /* WT_STACK_* */
    int warp;                            /* WARP_* */
    float warp_amt;                      /* 0..1 */
    float level;                         /* linear gain (Vital squares its level knob: the core passes the gain) */
    float pan;                           /* -1..1 */
    float phase, rand_phase;             /* 0..1: used by osc_on */
} osc_ctl_t;
typedef struct {
    float ph[MAXUNI];                    /* phases 0..1 */
    float last[2];                       /* anything the warps or smoothing need */
    float s[12];
    uint32_t rng;
} osc_state_t;
void osc_on(osc_state_t *s, const osc_ctl_t *c);   /* a new note (not legato): the phases from phase / rand_phase */
void osc_run(osc_state_t *s, const osc_ctl_t *c, const float *src, float *L, float *R, float *mono, uint32_t n);

/* ----------------------------------------------------------- the sample oscillator --- */
typedef struct {
    const int16_t *data;                 /* 16-bit PCM at rate, mono; NULL = computed live (kind) */
    int kind;                            /* 0 data; 1 white, 2 pink, 3 brown noise (live) */
    uint32_t len, loop_start, loop_end;  /* samples */
    float rate;                          /* its sample rate */
    float root;                          /* the MIDI note it plays at its rate */
} smp_ref_t;
extern const int SMP_NFACT;
extern const char *const SMP_NAME[];
const smp_ref_t *smp_get(int i);         /* the factory samples (generated at boot by smp_init) */
void smp_init(void);                     /* main loop, once */
typedef struct {
    int sample;                          /* index */
    float note;                          /* MIDI note (keytrack on) or 60 (off): the core decides */
    int loop;                            /* 0 one shot, 1 loop */
    float level, pan;
    float start;                         /* 0..1 of the sample, used by smp_on */
    int random_start;
} smp_ctl_t;
typedef struct {
    float pos, s[4];
    uint32_t rng;
    int done;
} smp_state_t;
void smp_on(smp_state_t *s, const smp_ctl_t *c);
void smp_run(smp_state_t *s, const smp_ctl_t *c, float *L, float *R, float *mono, uint32_t n);
