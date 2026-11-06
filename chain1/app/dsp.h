/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1's sound engine: OSC -> FILTER -> AMP (EG) -> MOD -> DELAY -> REVERB, one voice, and the arpeggiator.
 * The main loop owns the patch and posts note events; the audio interrupt (dsp_render) owns everything else. */
#pragma once
#include <stdint.h>

/* knob values are 0..1023 (10 bits); "bipolar" ones have their centre at 512 */
enum { OSC_SAW, OSC_TRI, OSC_SQR, OSC_VPM, OSC_N };
enum { FLT_LP2, FLT_LP4, FLT_BP2, FLT_BP4, FLT_HP2, FLT_HP4, FLT_OFF, FLT_N };
enum { EG_ADSR, EG_AHR, EG_AR, EG_ARLOOP, EG_OPEN, EG_N };
enum { MOD_OFF, MOD_CHORUS, MOD_ENSEMBLE, MOD_PHASER, MOD_FLANGER, MOD_N };
enum { DLY_OFF, DLY_STEREO, DLY_MONO, DLY_PINGPONG, DLY_HIPASS, DLY_TAPE, DLY_N };
enum { REV_OFF, REV_HALL, REV_PLATE, REV_SPACE, REV_RISER, REV_SUBMARINE, REV_N };
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_CONV, ARP_DIV, ARP_CONVDIV, ARP_DIVCONV, ARP_RANDOM,
       ARP_STOCH, ARP_N };
enum { CHORD_OCT, CHORD_MAJ, CHORD_SUS, CHORD_AUG, CHORD_MIN, CHORD_DIM, CHORD_N };

typedef struct {
    uint8_t osc_type, flt_type, eg_type, mod_type, dly_type, rev_type, arp_type, arp_chord;
    uint16_t shape, alt, lfo_rate, lfo_depth;              /* lfo_depth bipolar: < 512 pitch, > 512 shape */
    uint16_t cutoff, reso, sweep_rate, sweep_depth;        /* sweep_depth bipolar: < 512 down, > 512 up */
    uint16_t attack, release, trem_rate, trem_depth;
    uint16_t mod_time, mod_depth;
    uint16_t dly_time, dly_depth, dly_mix;
    uint16_t rev_time, rev_depth, rev_mix;
    uint16_t arp_len, arp_tempo;                           /* arp_len 1..24, arp_tempo in 0.1 BPM 560..2400 */
    uint8_t arp_oct;                                       /* 1..3 */
    uint8_t rsv[3];
} patch_t;

void dsp_init(void);
void dsp_patch(const patch_t *p);          /* main loop: the patch the next blocks play */
void dsp_key(int note, int on);            /* main loop: a key (MIDI note 0..127) pressed / let go */
void dsp_arp(int on, int latch);           /* main loop: the arpeggiator on / off, latched */
void dsp_panic(void);                      /* all notes off, the effects' tails cleared */
/* audio interrupt: n stereo frames into lr (24-bit), gain the output's scale */
void dsp_render(int32_t *lr, uint32_t n, float gain);
/* MIDI in (audio interrupt): note on / off from any port */
void dsp_midi_note(int note, int vel);

/* what the screen and the tests read */
int dsp_note(void);                        /* the note sounding, -1 none */
float dsp_level(void);                     /* the amp envelope, 0..1 */
uint32_t dsp_arp_steps(void);              /* arpeggiator steps played since boot */
float dsp_peak(void);                      /* the output's peak since the last call, 0..1 */
