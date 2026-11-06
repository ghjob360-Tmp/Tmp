/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1's engine: a reduced Vital-style voice architecture for the FM-1.
 *   per voice: OSC A + OSC B (wavetable, unison 1..3, phase warp) + NOISE -> FILTER (or straight past it)
 *              -> AMP (ENV1, squared, as Vital) ; ENV2, ENV3, LFO1, LFO2 and 8 modulation slots
 *   then:      CHORUS -> COMPRESSOR -> DELAY -> DISTORTION -> FLANGER -> PHASER -> REVERB (Vital's default order)
 * Vital (Matt Tytel, GPL-3.0) is the model: its warp formulas, envelope curve and amplitude law are followed here;
 * everything is cut down to what one FM-1 core can run (6 voices, 2 oscillators, 1 filter, 3 envelopes, 2 LFOs). */
#pragma once
#include <stdint.h>
#include "wt.h"

#define NVOICES 6
#define NMOD 8

enum { WARP_NONE, WARP_SYNC, WARP_FORMANT, WARP_QUANTIZE, WARP_BEND, WARP_SQUEEZE, WARP_PULSE, WARP_FM, WARP_RM,
       WARP_N };
enum { DEST_FILTER, DEST_DIRECT, DEST_N };
enum { FM_ANALOG, FM_LADDER, FM_FORMANT, FM_COMB, FM_OFF, FM_N };        /* filter models */
enum { LS_SINE, LS_TRI, LS_SAWUP, LS_SAWDN, LS_SQUARE, LS_SH, LS_SMOOTH, LS_N };
enum { LM_TRIGGER, LM_FREE, LM_ENVELOPE, LM_N };
enum { SRC_NONE, SRC_ENV1, SRC_ENV2, SRC_ENV3, SRC_LFO1, SRC_LFO2, SRC_VEL, SRC_NOTE, SRC_RANDOM, SRC_WHEEL,
       SRC_PRESS, SRC_N };
enum { DST_NONE, DST_A_FRAME, DST_A_WARP, DST_A_PITCH, DST_A_LEVEL, DST_B_FRAME, DST_B_WARP, DST_B_PITCH,
       DST_B_LEVEL, DST_CUTOFF, DST_RESO, DST_BLEND, DST_DRIVE, DST_NOISE, DST_PAN, DST_LFO1_RATE, DST_LFO2_RATE,
       DST_VOLUME, DST_N };
enum { DIST_OFF, DIST_SOFT, DIST_HARD, DIST_LINFOLD, DIST_SINFOLD, DIST_CRUSH, DIST_DOWNSAMPLE, DIST_N };
enum { DLY_OFF, DLY_MONO, DLY_STEREO, DLY_PINGPONG, DLY_N };

/* every value is a uint16_t: knobs 0..1023 (bipolar ones centred on 512), enums their index */
typedef struct {
    uint16_t table, frame, warp, warp_amt, morph, morph_amt, level, dest, unison, detune, spread, transpose;
} osc_p;                                 /* transpose: 0..48, 24 = none */
typedef struct {
    uint16_t delay, attack, hold, decay, sustain, release, apow, drpow;   /* times: (v * 2)^4 s; pows bipolar */
} env_p;
typedef struct {
    uint16_t shape, rate, mode, smooth;
} lfo_p;
typedef struct {
    uint16_t src, dst, amt, rsv;         /* amt bipolar */
} mod_p;
typedef struct {
    osc_p osc[2];
    uint16_t noise;
    uint16_t f_model, f_style, cutoff, reso, drive, blend, keytrack;   /* f_style: 0 12 dB, 1 24 dB (comb: + / -) */
    env_p env[3];
    lfo_p lfo[2];
    mod_p mod[NMOD];
    uint16_t poly, glide, veltrack, volume;
    uint16_t ch_on, ch_rate, ch_depth, ch_mix;
    uint16_t cp_on, cp_amt, cp_mix;
    uint16_t dl_type, dl_time, dl_fb, dl_mix;
    uint16_t ds_type, ds_drive, ds_mix;
    uint16_t fl_on, fl_rate, fl_fb, fl_mix;
    uint16_t ph_on, ph_rate, ph_fb, ph_mix;
    uint16_t rv_on, rv_time, rv_damp, rv_mix;
} patch_t;

void synth_init(void);
void synth_patch(const patch_t *p);                  /* main loop */
void synth_tables(int osc, const wt_t *t);           /* main loop: the oscillator's table from now on */
void synth_key(int note, int vel);                   /* main loop: vel 0 = note off */
void synth_panic(void);
void synth_overload(void);                           /* the render ran out of time: shed a voice */
void synth_pressure(float p);                        /* main loop: the keys' pressure, 0..1 */
void synth_midi(uint32_t st, uint32_t d1, uint32_t d2);   /* audio interrupt: a MIDI message in */
void synth_render(int32_t *lr, uint32_t n, float gain);  /* audio interrupt */

int synth_voices(void);                              /* voices sounding */
int synth_last_note(void);                           /* the newest voice's note, -1 none */
float synth_peak(void);                              /* output peak since the last call */
uint32_t synth_shed_count(void);
