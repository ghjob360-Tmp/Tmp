/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1's sound engine: up to eight voices (OSC -> FILTER -> AMP, a filter envelope, one LFO), then the chain
 * MOD -> DELAY -> REVERB, and the arpeggiator. Any of OSC, MOD, DELAY and REVERB may instead be a logue SDK unit
 * for the NTS-1 (logue/logue.h), run on the second core.
 * The main loop owns the patch and posts note events; the audio interrupt (dsp_render) owns everything else. */
#pragma once
#include <stdint.h>

/* knob values are 0..1023 (10 bits); "bipolar" ones have their centre at 512 */
enum { OSC_SAW, OSC_TRI, OSC_SQR, OSC_VPM, OSC_NOISE, OSC_N };
enum { FLT_LP2, FLT_LP4, FLT_BP2, FLT_BP4, FLT_HP2, FLT_HP4, FLT_OFF, FLT_N };
enum { EG_ADSR, EG_AR, EG_ARLOOP, EG_OPEN, EG_N };
enum { FEG_ADSR, FEG_AD, FEG_LOOP, FEG_N };
enum { LFO_TRI, LFO_SINE, LFO_SAW, LFO_SQR, LFO_SH, LFO_DRIFT, LFO_N };
enum { VM_POLY, VM_MONO, VM_LEGATO, VM_UNISON, VM_N };
enum { MOD_OFF, MOD_CHORUS, MOD_ENSEMBLE, MOD_PHASER, MOD_FLANGER, MOD_TREMOLO, MOD_N };
enum { DLY_OFF, DLY_STEREO, DLY_MONO, DLY_PINGPONG, DLY_HIPASS, DLY_TAPE, DLY_N };
enum { REV_OFF, REV_HALL, REV_PLATE, REV_SPACE, REV_RISER, REV_SUBMARINE, REV_N };
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_CONV, ARP_DIV, ARP_CONVDIV, ARP_DIVCONV, ARP_RANDOM,
       ARP_STOCH, ARP_N };
enum { CHORD_OCT, CHORD_MAJ, CHORD_SUS, CHORD_AUG, CHORD_MIN, CHORD_DIM, CHORD_N };

/* the four places a logue unit can take: its TYPE is the section's built-in count + the unit's bank slot */
enum { US_OSC, US_MOD, US_DLY, US_REV, US_N };
#define NUNITS 7                         /* the unit bank's slots (flash) */
#define UPARAM 11                        /* a unit's parameters at most (logue.h LG_NPARAM) */
#define NVOICE 8

typedef struct {
    _Alignas(4) uint8_t osc_type;        /* (_Alignas: the whole patch aligned: the render copies it by words) */
    uint8_t flt_type, eg_type, feg_type, lfo_type, voice_mode, mod_type, dly_type;
    uint8_t rev_type, arp_type, arp_chord, arp_oct;   /* arp_oct 1..3 */
    uint8_t voices;                      /* poly: most at once 1..8; unison: the stack 2..8 */
    uint8_t rsv0[3];
    uint16_t shape, alt, noise, tune;    /* tune bipolar: -24..+24 semitones */
    uint16_t cutoff, reso, feg_amt, keytrk;   /* feg_amt bipolar */
    uint16_t a, d, s, r;                 /* the amp envelope */
    uint16_t fa, fd, fs, fr;             /* the filter envelope */
    uint16_t lfo_rate, lfo_pitch, lfo_shape, lfo_cut;
    uint16_t glide, detune, velo, bend;  /* bend: the pitch bend's range in semitones 0..12 */
    uint16_t mod_time, mod_depth, mod_mix, rsv1;
    uint16_t dly_time, dly_depth, dly_mix, dly_tone;
    uint16_t rev_time, rev_depth, rev_mix, rev_tone;
    uint16_t arp_len, arp_tempo, arp_gate, arp_swing; /* arp_len 1..24, arp_tempo 0.1 BPM 560..2400, gate/swing % */
    int16_t up[US_N][UPARAM];            /* the units' own parameters, as each counts them */
    uint32_t ucrc[US_N];                 /* which unit each place had (its image's CRC), to find it again */
} patch_t;

/* a unit in the bank, as the screen shows it */
typedef struct {
    uint8_t used, module, fmt, state, err;   /* module: logue.h LG_MOD_*; state LG_EMPTY / READY / FAULT */
    char name[21];
    uint32_t crc;
    uint8_t nparam;
    struct { char name[22]; int16_t min, max, init; uint8_t type, frac; } param[UPARAM];
} unit_info_t;

void dsp_init(void);
void dsp_patch(const patch_t *p);          /* main loop: the patch the next blocks play */
void dsp_key(int note, int on, int vel);   /* main loop: a key (MIDI note 0..127) pressed (vel 1..127) / let go */
void dsp_arp(int on, int latch);           /* main loop: the arpeggiator on / off, latched */
void dsp_panic(void);                      /* all notes off, the effects' tails cleared */
void dsp_service(void);                    /* main loop: units in and out of their places (loading runs here) */
/* audio interrupt: n stereo frames into lr (24-bit), gain the output's scale */
void dsp_render(int32_t *lr, uint32_t n, float gain);
/* MIDI in (audio interrupt): notes, the pitch bend (-8192..8191), the mod wheel (0..127) */
void dsp_midi_note(int note, int vel);
void dsp_midi_bend(int v);
void dsp_midi_wheel(int v);
void dsp_overload(void);                   /* the render ran late: one voice fewer for a while */

/* the unit bank (flash): what is there, and the image of one (in flash, for logue.c) */
int dsp_units_scan(void);                  /* main loop: reads the bank again -> units found */
const unit_info_t *dsp_unit(int slot);     /* slot 0..NUNITS-1 */
int dsp_unit_put(int slot, const uint8_t *img, uint32_t len);   /* (host tests) write an image into a slot */
int dsp_unit_erase(int slot);
/* a place's unit: its bank slot (-1 built-in), its state (logue.h LG_*), why it stopped, its load (% of a block) */
int dsp_place_slot(int place);
int dsp_place_state(int place, int *err);
int dsp_place_load(int place);

/* what the screen and the tests read */
int dsp_note(void);                        /* the newest note sounding, -1 none */
uint32_t dsp_voices_on(void);              /* bit v: voice v sounds */
int dsp_voice_note(int v);                 /* its note while held, -1 */
float dsp_voice_level(int v);              /* its amp envelope, 0..1 */
float dsp_level(void);                     /* the newest voice's amp envelope */
float dsp_fenv(void);                      /* the newest voice's filter envelope */
float dsp_lfo(void);                       /* the LFO, -1..1 */
uint32_t dsp_arp_steps(void);              /* arpeggiator steps played since boot */
uint32_t dsp_arp_step(void);               /* the step it is on */
float dsp_peak(void);                      /* the output's peak since the last call, 0..1 */
float dsp_vu(void);                        /* a slowly falling level, 0..1 */
int dsp_scope(float *out, uint32_t n);     /* the last n (< 512) output samples from a rising zero crossing */
int dsp_core2_load(void);                  /* the second core's work, % of a block (units) */
