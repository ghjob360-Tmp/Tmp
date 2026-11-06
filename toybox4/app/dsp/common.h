/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: what every DSP unit shares.
 *
 * RULES for code in the audio path (everything called from the render, on either core):
 *  - functions are RAMC (PLAT_RAM_CODE: placed in RAM, ram_layout = "packed") or INL (always inline);
 *  - they may call only other RAMC / INL functions: no libc (no memset / memcpy / sinf / expf), no division or
 *    modulo by a variable integer (a library call), no double, no 64-bit multiply;
 *  - math: fastmath.h's fm_* (forced inline below) and the helpers here; floats are single precision;
 *  - reading constant tables and the sample flash (XIP) is fine; writing flash is not;
 *  - the build checks every rule (tools/ramcode.py) and fails the link if one is broken. */
#pragma once
#include <stdint.h>
#include "plat.h"
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#undef inline

#define SR 44100.0f
#define ISR (1.0f / SR)
#define RAMC PLAT_RAM_CODE
#define NOIN __attribute__((noinline))
#define INL static inline __attribute__((always_inline))

#define NTRK 4                           /* tracks (each with its own sound) */
#define NSV 8                            /* synth voices, shared by the tracks */
#define NDV 8                            /* drum voices */
#define NDKEY 24                         /* drum keys (slices / pads) */
#define CTL 16u                          /* control-rate period: engines are run SIZE <= CTL frames at a time */
#define SLINE 1024u                      /* a voice's delay line (the string engine), floats */

/* ------------------------------------------------------------------ helpers --- */
INL float kn(uint16_t v) { return (float)(v > 1023u ? 1023u : v) * (1.0f / 1023.0f); }   /* 0..1023 -> 0..1 */
INL float frac1(float x) { return x - fm_floorf(x); }
INL float lerpf(float a, float b, float t) { return a + (b - a) * t; }
INL float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
INL uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}
INL float rnd1(uint32_t *s) { return (float)(int32_t)xs32(s) * (1.0f / 2147483648.0f); }   /* -1..1 */
INL float blep(float t, float dt)        /* PolyBLEP: subtract at a rising saw's wrap */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
INL float note_hz(float n) { return 440.0f * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }
INL float soft(float x) { return fm_tanhf(x); }

/* mu-law 8-bit samples (the sampler's flash): a 256-entry table, built once by dsp_tables_init (main loop) */
extern float g_ulaw[256];
INL float ulaw(uint8_t b) { return g_ulaw[b]; }
void dsp_tables_init(void);

/* the samples in flash, 22050 Hz mu-law, read in place */
typedef struct {
    const uint8_t *data;
    uint32_t len;                        /* bytes (= samples) */
} sample_ref_t;
extern sample_ref_t g_synth_sample, g_drum_sample;
#define SAMPLE_RATE 22050.0f

/* ------------------------------------------------------------- the module types --- */
/* a sound: five modules, each a type, on / off, and eight 0..1023 parameters (K1..K4, then GLO + K1..K4) */
enum { M_ENGINE, M_ENV, M_FX, M_LFO, M_SEQ, M_NMOD };
typedef struct {
    uint16_t type, on, p[8];
} mod_t;

/* synth engines */
enum { E_STRING, E_PULSE, E_FM, E_CLUSTER, E_DIGITAL, E_PHASE, E_SPECTRA, E_DUAL, E_VOLTAGE, E_DNA, E_SAMPLER,
       E_NSYNTH };
/* drum engines */
enum { D_KIT, D_SLICE, D_N };
/* effects (track FX and the master FX) */
enum { FX_ECHO, FX_CUBE, FX_TURBO, FX_WIRE, FX_KNOCK, FX_COIL, FX_DOPPLER, FX_HALL, FX_BITS, FX_N };
/* LFOs */
enum { L_SENSE, L_DICE, L_WOBBLE, L_WAVE, L_MIDI, L_N };
/* sequencers */
enum { Q_LOOP, Q_GRID, Q_SPIN, Q_HANDS, Q_DRAW, Q_ARP, Q_LATCH, Q_N };
