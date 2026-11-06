/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4's engine: four tracks, each an instrument (one of four synth engines or a drum kit) with an envelope,
 * an LFO aimed at one of its four knobs, an effect, a level and a pan; a master reverb and delay; and a four-track
 * event tape (a loop of 1..8 bars) that plays and records the tracks, at any speed, backwards too.
 * The main loop owns the project (proj_t) and posts keys; the audio interrupt owns the voices and the tape's motion. */
#pragma once
#include <stdint.h>

#define NTRACKS 4
#define NSYNTHV 8                        /* synth voices, shared by the tracks */
#define NDRUMV 6                         /* drum voices */
#define NPADS 12
#define TAPE_MAX 192                     /* events per track */
#define PPQ 96                           /* tape ticks per beat */

enum { ENG_STRING, ENG_PULSE, ENG_FM, ENG_CLUSTER, ENG_DRUM, ENG_N };
enum { LFO_SINE, LFO_TRI, LFO_SQUARE, LFO_RANDOM, LFO_SHAPES };
enum { TGT_K1, TGT_K2, TGT_K3, TGT_K4, TGT_PITCH, TGT_VOLUME, TGT_N };
enum { FX_OFF, FX_DRIVE, FX_CRUSH, FX_FILTER, FX_N };
enum { DIV_8, DIV_D8, DIV_4, DIV_N };    /* delay: 1/8, dotted 1/8, 1/4 */

/* every knob value is 0..1023 */
typedef struct {
    uint16_t engine;
    uint16_t eng[ENG_N - 1][4];          /* each synth engine keeps its own four */
    uint16_t pad[NPADS][4];              /* drum pads: pitch, decay, tone, level */
    uint16_t env[4];                     /* attack, decay, sustain, release */
    uint16_t lfo_shape, lfo_target, lfo[3];   /* rate, depth, fade-in */
    uint16_t fx_type, fx[FX_N - 1][4];
    uint16_t level, pan, mute;
} track_t;                               /* 99 halfwords */

typedef struct {
    track_t t[NTRACKS];
    uint16_t tempo;                      /* BPM */
    uint16_t loop_bars;                  /* 1, 2, 4, 8 */
    uint16_t speed;                      /* 0..1023: -2x .. +2x (512 stopped; < 512 backwards) */
    uint16_t metro;
    uint16_t rv_amt, rv_size, dl_amt, dl_fb, dl_div;
    uint16_t rsv;
} proj_t;

typedef struct {
    uint16_t n[NTRACKS];
    uint32_t ev[NTRACKS][TAPE_MAX];      /* tick (16) | note (7) << 16 | on (1) << 23 | vel (7) << 24, by tick */
} tape_t;

void eng_init(tape_t *tape);             /* the tape lives in the app (it is saved with the project) */
void eng_project(const proj_t *p);       /* main loop: the project the next blocks play */
void eng_key(int track, int note, int vel);   /* main loop: a key (vel 0 = up), played and, if recording, taped */
void eng_transport(int play, int rec);   /* main loop */
void eng_clear_track(int track);         /* main loop: the track's tape erased */
void eng_panic(void);
void eng_midi(uint32_t st, uint32_t d1, uint32_t d2, int track);   /* audio interrupt: MIDI in */
void eng_render(int32_t *lr, uint32_t n, float gain);

/* what the screen animates (read from the main loop, written by the interrupt) */
typedef struct {
    float track_peak[NTRACKS];           /* each track's level, decaying */
    float env[NTRACKS], env_note[NTRACKS];   /* the newest voice's envelope and note per track */
    float lfo[NTRACKS], lfo_ph[NTRACKS];
    uint32_t pad_hits[NTRACKS][NPADS];   /* hits per pad, counting */
    float tape_pos;                      /* ticks into the loop */
    float reel;                          /* the reels' angle, turns */
    uint32_t beat;                       /* beats played */
    float out_peak;
    uint8_t playing, recording;
} eng_view_t;
extern volatile eng_view_t VIEW;
