/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the core (core.c, seq.c, tape.c): the project, the voices, the tracks, the LFOs, the
 * sequencers, the event tape, the mixer, and the render that runs them (on both cores).
 *
 * Ownership: the main loop owns the project (proj_t) and hands a copy to the render (core_project); everything
 * the render changes by itself (the tape's events while recording, the sequencers' live state, the voices) lives
 * in the core, and the main loop asks for changes to it through the core_cmd_* calls, which the render carries
 * out at its next block. The screen reads the core's live state (CORE_VIEW) and the tape (core_tape()). */
#pragma once
#include "common.h"
#include "eng.h"
#include "fx.h"

/* ----------------------------------------------------------------- sounds --- */
enum { KIND_SYNTH, KIND_DRUM };
typedef struct {
    mod_t m[M_NMOD];                     /* engine, envelope, fx, lfo, sequencer */
} sound_t;                               /* 100 bytes */

/* the envelope module: synth  K1..K4 attack decay sustain release | GLO: play mode, portamento, bend, volume
 *                      drum   K1..K4 attack level, mid level, release, timing | GLO: -, -, bend, volume */
enum { PM_POLY, PM_MONO, PM_LEGATO, PM_UNISON, PM_N };

/* the LFO module (K1..K4 | GLO):
 *  SENSE   source (pressure, velocity, envelope, level, mod wheel), amount (+-), target module, target param |
 *          smoothing, -, -, -
 *  DICE    amount, speed, target module (every param of it wanders), envelope (smooth) | -, -, -, -
 *  WOBBLE  speed, pitch amount (+-), volume amount (+-), envelope (fade in) | shape (sine saw exp square blip),
 *          sync (free / 1/1 .. 1/32), -, -
 *  WAVE    speed, amount (+-), target module, target param | shape, sync, retrigger, phase
 *  MIDI    CC1 amount (+-), pressure amount (+-), target module, target param | -, -, -, - */
enum { LS_PRESSURE, LS_VELOCITY, LS_ENVELOPE, LS_LEVEL, LS_WHEEL, LS_N };
enum { LT_ENGINE, LT_ENV, LT_FX, LT_N };   /* LFO target modules */

/* the sequencer module (K1..K4 | GLO), and its data (seqdata_t):
 *  LOOP    division, swing, trigger pattern, hold | direction, rotate, gate, -        steps: note / rest / tie
 *  GRID    cursor, swing, length, run | clear, rotate, direction, gate               16 steps x 1 note
 *  SPIN    speed (+- direction), gravity, opening, bounce | -, -, -, -             balls: the notes played in
 *  HANDS   swing, length, density, hold | -, -, -, -                                each white key a pattern
 *  DRAW    x cursor, draw (y at the cursor), speed, run | erase, scale, range, gate  32 points
 *  ARP     division, trigger mode, trigger pattern, hold | note length, type, octaves, swing
 *  LATCH   split point, mono / poly, transpose, hold | -, -, -, - */
#define SEQ_STEPS 128
#define SEQ_REST 0xFFu
#define SEQ_TIE 0xFEu
typedef struct {
    uint8_t loop[SEQ_STEPS];             /* LOOP: notes (MIDI), SEQ_REST, SEQ_TIE */
    uint8_t loop_n;
    uint8_t grid[16];                    /* GRID: a note per step, 0 = off */
    uint8_t draw[32];                    /* DRAW: 0..127 heights */
    uint8_t rsv[3];
} seqdata_t;                             /* 180 bytes */

typedef struct {
    sound_t synth, drum;                 /* every track has both: the mode it is played in picks one */
    dpad_t pads[NDKEY];                  /* the drum kit's keys */
    seqdata_t seq_synth, seq_drum;
    uint16_t level, pan, mute, kind;     /* kind: what its keys play now (KIND_SYNTH / KIND_DRUM) */
} track_t;

typedef struct {
    track_t t[NTRK];
    mod_t eq;                            /* p: low, mid, high, amount */
    mod_t mfx;                           /* the master FX: type, on, p[8] */
    mod_t mout;                          /* p: left, right, drive, release */
    uint16_t tempo;                      /* BPM 40..220 */
    uint16_t metro;                      /* metronome level, 0 = off */
    uint16_t speed;                      /* tape speed, 0..1023 = 0.25x .. 2x (512 = 1x, varispeed) */
    uint16_t reverse;
    uint16_t loop_on, loop_in, loop_out; /* bars */
    uint16_t sync;                       /* 0 free (speed leaves the tempo), 1 beat match (speed moves the tempo) */
    uint16_t detune;                     /* master detune, cents + 512 */
    uint16_t rsv;
} proj_t;

/* ------------------------------------------------------------------ the tape --- */
#define TAPE_BARS 128                    /* the tape: 128 bars of 4 beats */
#define TPQ 96                           /* ticks per beat */
#define TAPE_TICKS (TAPE_BARS * 4 * TPQ)
#define TAPE_EV 960                      /* events per track */
typedef struct {
    uint32_t tick;                       /* 0 .. TAPE_TICKS - 1 */
    uint8_t note, vel;                   /* vel 0 = note off */
    uint8_t kind, rsv;                   /* KIND_SYNTH / KIND_DRUM */
} tev_t;
typedef struct {
    uint16_t n[NTRK];
    tev_t ev[NTRK][TAPE_EV];             /* by tick */
} tape_t;                                /* 32 KB */

/* --------------------------------------------------------------- the calls --- */
void core_init(void);
void core_project(const proj_t *p);      /* main loop: the project the next blocks play */
const tape_t *core_tape(void);
tape_t *core_tape_rw(void);              /* main loop, only while the render is told to keep off (core_hold) */
void core_hold(int on);                  /* 1: the render stops touching the tape and the voices (loading) */

/* keys: the current track, in the given kind; vel 0 = up */
void core_key(int track, int kind, int note, int vel);
void core_pressure(float p);             /* 0..1 */
/* transport */
void core_play(int on);
void core_record(int on);                /* armed: records into the current track while playing */
void core_current(int track);
void core_locate(uint32_t tick);         /* move the head */
/* tape edits (carried out by the render): lift = cut the loop region (or 1 bar at the head) into the clipboard */
enum { TC_LIFT, TC_LIFT_ALL, TC_DROP, TC_ERASE_TRACK, TC_ERASE_ALL, TC_BREAK, TC_CHOP, TC_N };
void core_tape_cmd(int cmd, int arg);
/* the sequencer's step entry (LOOP / GRID / DRAW), on the current track */
void core_seq_input(int kind, int note);  /* a key while editing the sequencer: an entered step */
void core_seq_rest(int kind, int tie);
void core_seq_delete(int kind);
void core_seq_toggle(int kind);          /* run / stop the current track's sequencer */
void core_panic(void);
void core_overload(void);                /* the render ran late: a voice goes, fewer are allowed */
void core_cpu(float load);               /* main loop, ~1 s: the render's load (0..1); room again: one more */
/* the sampler: record the master output into a sample (synth or drum) while a key is held */
void core_sample_arm(int kind);          /* the main loop has erased the flash; recording starts with sample_go */
void core_sample_go(int on);
/* MIDI in (render context) */
void core_midi(uint32_t st, uint32_t d1, uint32_t d2);

/* the render */
void core_render(int32_t *lr, uint32_t n, float gain);

/* what the screen shows: written by the render, read by the main loop */
typedef struct {
    float trk_peak[NTRK], out_peak, gr;
    float env[NTRK], env_stage[NTRK];        /* the newest voice's amplitude envelope */
    float note[NTRK];                        /* the newest voice's note */
    float vel[NTRK];
    float lfo[NTRK], lfo_ph[NTRK];
    float eff[NTRK][3][8];                   /* each track's module params as heard now (engine, env, fx), 0..1 */
    uint32_t hits[NTRK][NDKEY];              /* drum hits, counting */
    uint32_t tick;                           /* the head */
    float reel;                              /* turns */
    uint32_t beat;
    uint8_t playing, recording, looping, breaking;
    uint8_t seq_on[NTRK][2];
    int16_t seq_step[NTRK][2];               /* the step playing */
    float balls[8][3];                       /* SPIN: x, y, age (the current track's) */
    float spin_angle;
    uint8_t nballs;
    uint8_t sampling;                        /* 1 armed, 2 recording */
    uint32_t sample_len;                     /* recorded so far (samples) */
    uint16_t voices, drums;
    uint32_t clip_n;                         /* events in the clipboard */
    uint32_t shed;                           /* voices shed for time */
    float cpu;                               /* the render's load (main loop's reading) */
    uint8_t sv_lim, dv_lim;                  /* the voices allowed now */
} core_view_t;
extern volatile core_view_t CORE_VIEW;

/* the sampler's writer (main loop): the render fills a ring with mu-law bytes; the main loop drains it to flash */
uint32_t core_sample_take(uint8_t *dst, uint32_t max);
