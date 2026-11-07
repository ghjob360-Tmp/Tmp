/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the core (core.c, mod.c): the voices, the modulators (6 envelopes, 8 LFOs, 4 random LFOs, the
 * macros), the modulation matrix, the routing (3 wavetable oscillators + the sample oscillator -> 2 filters, serial
 * or parallel -> the effect chain, or straight out), and the render on both cores.
 *
 * Ownership: the main loop owns the patch (patch_t) and hands copies to the render (core_patch); the wavetables are
 * built by the main loop and handed over as pointers (core_table). Keys and MIDI go through a command ring. The
 * screen reads CORE_VIEW (written by the render). */
#pragma once
#include "common.h"
#include "params.h"
#include "wt.h"
#include "filt.h"
#include "fx.h"

void core_init(void);                    /* main loop, once (after smp_init) */
void core_patch(const patch_t *p);       /* main loop: the patch from the next block on */
void core_table(int osc, const wt_t *t); /* main loop: oscillator osc plays t from the next block (the old one is free
                                            once core_table_busy(old) says 0) */
int core_table_busy(const wt_t *t);      /* 1 = the render may still read t */
void core_key(int note, int vel);        /* vel 0 = off */
void core_hold(int on);                  /* the sustain pedal (PLAY: hold) */
void core_pressure(float p);             /* 0..1 */
void core_wheel(float w);                /* 0..1 */
void core_bend(float b);                 /* -1..1 */
void core_panic(void);
void core_overload(void);                /* the render ran late: shrink the voice budget */
void core_cpu(float load);               /* main loop, ~1 s: the render's load (0..1): room again, a little more */
void core_midi(uint32_t st, uint32_t d1, uint32_t d2);   /* audio interrupt context */
void core_render(int32_t *lr, uint32_t n, float gain);

/* what the screen shows: written by the render (the newest voice; the global modulation), read by the main loop */
typedef struct {
    int active;                          /* a voice sounds */
    int voices;                          /* how many */
    uint16_t vmask;                      /* which */
    uint8_t vnote[NVOICE];
    float src[MS_N];                     /* the newest voice's sources now (0..1; BEND -1..1) */
    float live[NPARAM];                  /* every modulated parameter's value now (0..1), the newest voice / global */
    float env_stage[6];                  /* the envelopes: stage (0 delay 1 attack 2 hold 3 decay 4 sustain 5 release
                                            6 off) + the time in it (0..1) */
    float lfo_ph[8];                     /* the LFOs' phase (0..1, the shape's x) */
    float rnd_hist[4][32];               /* the random LFOs' recent values, newest last */
    float frame[NOSC];                   /* the oscillators' frame (modulated) */
    float out_peak, out_l, out_r;
    float cpu;                           /* the render's share of its time (0..1) */
    int budget;                          /* the unison budget (oscillator voices) the core allows now */
    uint32_t shed;                       /* voices shed for time */
} core_view_t;
extern volatile core_view_t CORE_VIEW;

/* the LFO shapes, for the screen too (x 0..1 -> 0..1); drawn: the patch's points */
float lfo_shape(int shape, const uint8_t *y, const int8_t *c, float x);
/* the times: Vital's envelope time law and the LFO rate, for the screen's numbers */
float env_time(float v);                 /* 0..1 -> seconds */
float lfo_hz(float v, int sync, float tempo);   /* free rate 0..1 -> Hz; or the sync division at tempo */
extern const char *const SYNC_NAME[14];
extern const char *const MS_NAME[MS_N];
