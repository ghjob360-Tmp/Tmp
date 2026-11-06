/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the sound engines (eng_synth.c, eng_drum.c). Contract with the core (core.c):
 *
 *  - The core owns a voice: its pitch, its amplitude envelope, its track. An engine only makes the raw sound.
 *  - k[8]: the engine module's eight parameters as 0..1 (K1..K4 = blue, green, white, orange; k[4..7] = the GLO row),
 *    already moved by the LFO. They change every CTL frames; engines may smooth them (no zipper noise).
 *  - eng_on / drum_on: a note starts (or retriggers a voice still sounding: keep phases running, do not click).
 *  - eng_run / drum_run: ADD n (<= CTL) mono samples into out[] (do not overwrite), peak about +-0.6 at k defaults.
 *  - state: all of an engine's per-voice memory is in eng_state_t (zeroed by the core before the first note of a
 *    voice); the string engine also gets a SLINE-float line per voice. No static mutable state shared between
 *    voices except read-only tables (two cores render different voices at the same time).
 *  - cost (at 360 MHz, single-precision FPU, ~1 instruction per flop): an engine should average <= 120 flops per
 *    sample per voice (8 voices + drums + effects must fit in about half of one core); FM / CLUSTER / SPECTRA may
 *    spend up to 250. Work per CTL block (coefficients, tables) is cheap; per-sample work is what counts.
 *  - every function here is RAMC (common.h rules). */
#pragma once
#include "common.h"

typedef struct {
    float f[48];                         /* free for the engine: phases, filter states, smoothed params... */
    uint32_t u[8];                       /* counters, random seeds, indices */
} eng_state_t;

/* ---- synth engines: E_STRING .. E_SAMPLER ---- */
/* the parameters (K1 blue, K2 green, K3 white, K4 orange | GLO: K1 K2 K3 K4), 0..1:
 *  STRING  tension (sustain), impulse decay, detune (a 2nd string), impulse type (pluck..bow-ish noise) |
 *          pluck position, brightness, body resonance, drive
 *  PULSE   width, second pulse (detune / sync of a 2nd pulse), filter, pwm depth |
 *          resonance, sub level, filter envelope, drive
 *  FM      amount (index), frequency (ratio set), topology (8 four-operator algorithms), detune |
 *          feedback, op 3/4 ratio, index decay (an index envelope), brightness
 *  CLUSTER waves (1..6 detuned saws), wave envelope (shape morphs over the note), spread, unitor (the detune
 *          closes over the note) | shape (saw..square), drift, filter, sub
 *  DIGITAL wave shaper, octave (-2..+2), detune + ring mod, digitalness (sample rate and bit reduction) |
 *          bits, waveform (sine, tri, saw, square), noise, fold
 *  PHASE   phase shift, distortion amount (phase distortion, CZ-like), phase filter (a resonant sweep), tilt |
 *          waveform (saw, square, pulse, double sine, saw-pulse, resonance I..III), distortion envelope, sub, detune
 *  SPECTRA wave type (additive spectra: saw, square, organ, bell, vocal...), spectral filter, phase, chorus |
 *          odd / even balance, harmonic count, spectral tilt, motion (spectrum sweeping in time)
 *  DUAL    envelope crossfade (osc1 -> osc2 over the note), waveform (osc1), envelope time, cross mod |
 *          osc2 interval, osc2 waveform, osc2 detune, filter cutoff
 *  VOLTAGE amp modulation, wave shaper (fold), phase filter, detune (3 oscillators) | ground noise, sub, drift, bias
 *  DNA     filter, wave number (a seed: 256 random waves), wave modifier (smoothing / steps), noise |
 *          seed walk (wave changes over the note), steps, resonance, octave
 *  SAMPLER start, loop in, loop out, end (g_synth_sample; root C4 at 22.05 kHz) | reverse, loop in fine,
 *          loop out fine, gain */
void eng_on(int e, eng_state_t *s, float *line, const float k[8], float hz, float vel);
void eng_run(int e, eng_state_t *s, float *line, const float k[8], float hz, float *out, uint32_t n);

/* ---- drums: D_KIT (a two-oscillator drum synth per key) and D_SLICE (24 slices of g_drum_sample) ---- */
typedef struct {
    uint16_t p[8];                       /* per key, 0..1023 */
} dpad_t;
/* D_KIT  per key: pitch, waveform (sine..noise), decay, cross mod | osc2 pitch, osc2 level / noise, filter, level.
 *        The key index (0..23) gives a default character (the core's factory kit: kick, kick 2, snare, snare 2,
 *        rim, clap, tamb, closed hat, open hat, ride, crash, then toms, zaps, blips, metal).
 * D_SLICE per key: pitch, in point, out point, play mode (to end / loop while held / once) |
 *        direction (fwd / rev), in fine, out fine, level. Positions are fractions of g_drum_sample. */
void drum_on(int e, eng_state_t *s, const dpad_t *pad, int key, float vel);
int drum_run(int e, eng_state_t *s, const dpad_t *pad, int key, int held, float *out, uint32_t n);   /* 0: done */
/* the factory kit's pads (D_KIT): the core copies them into new drum sounds */
void drum_kit_defaults(dpad_t pads[NDKEY]);
void drum_slice_defaults(dpad_t pads[NDKEY]);   /* 24 equal slices */
