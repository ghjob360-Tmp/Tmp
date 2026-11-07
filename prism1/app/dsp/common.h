/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: what every DSP unit shares.
 *
 * RULES for code in the audio path (everything called from the render, on either core):
 *  - functions are RAMC (PLAT_RAM_CODE: placed in RAM, ram_layout = "packed") or INL (always inline);
 *  - they may call only other RAMC / INL functions: no libc (no memset / memcpy / sinf / expf), no division or
 *    modulo by a variable integer (a library call), no double, no 64-bit multiply;
 *  - math: fastmath.h's fm_* (forced inline below) and the helpers here; floats are single precision;
 *  - reading constant tables and the flash data window (XIP) is fine; writing flash is not;
 *  - the build checks every rule (tools/ramcode.py) and fails the link if one is broken.
 * Code that runs only in the main loop (building wavetables, initialising) is plain flash code: no rule. */
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

#define NVOICE 8                         /* voices */
#define NOSC 3                           /* wavetable oscillators per voice */
#define MAXUNI 8                         /* unison voices per oscillator */
#define CTL 32u                          /* the control-rate block: the render works in blocks of <= CTL frames */

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
INL float note_hz(float n) { return 440.0f * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }
INL float db_gain(float db) { return fm_exp2f(db * 0.16609640474f); }   /* 10^(db/20) */
/* Vital's power curve (futils::powerScale): bends 0..1 by k (0 = straight, + = slow start, - = fast start) */
INL float power_scale(float x, float k)
{
    if (k > -0.001f && k < 0.001f)
        return x;
    return (fm_expf(k * x) - 1.0f) / (fm_expf(k) - 1.0f);
}
