/* SPDX-License-Identifier: GPL-3.0-only */
/* ext_def.h: the types and tables of ext.c, included by chips.c before the channels (chan_t holds an ext_t). */
/* The synthwave extensions, included by chips.c after the voices: every channel's extra settings (ext_t, chan[c].x),
 * the arp's chords, octaves, directions, gate and its clock, glide, the second LFO, the steps' extras (a sparse table:
 * chords, slides, parameter locks) and the channel's own chain (filter, drive, ensemble, pan, sends) that runs on a
 * channel rendered on its own. A setting at 0 is the old behaviour, so an old project sounds as it did. Integers only. */

/* ---- the arp's chords: the old eight first (saved numbers), then the new ones; USER takes its notes from the channel */
#define ARPS 22u
enum { AC_USER = 22 };
static const uint8_t ARP_N[ARPS] = {2, 4, 4, 4, 4, 4, 4, 4, 1, 3, 3, 4, 5, 5, 4, 4, 4, 4, 6, 3, 3, 3};
static const int8_t ARP_IV[ARPS][6] = {
    {0, 12}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 5, 7, 12}, {0, 7, 12, 19}, {0, 3, 6, 9},     /* OCT MAJ MIN MAJ7 MIN7 SUS4 POWER DIM */
    {0}, {0, 4, 7}, {0, 3, 7}, {0, 4, 7, 14}, {0, 3, 7, 10, 14}, {0, 4, 7, 11, 14}, {0, 4, 7, 10}, {0, 5, 7, 10},       /* NOTE MAJ3 MIN3 ADD9 MIN9 MAJ9 DOM7 7SUS4 */
    {0, 4, 7, 9}, {0, 3, 7, 9}, {0, 3, 7, 10, 14, 17}, {0, 4, 8}, {0, 2, 7}, {0, 0, 0},                                  /* 6 MIN6 MIN11 AUG SUS2 USER */
};
static const char *const ARP_NAME[ARPS + 1u] = {"OFF", "OCT", "MAJ", "MIN", "MAJ7", "MIN7", "SUS4", "PWR", "DIM", "NOTE", "MAJ3", "MIN3", "ADD9", "MIN9", "MAJ9", "DOM7", "7SUS", "6TH", "MIN6", "M11", "AUG", "SUS2", "USER"};
/* the chord a step can hold (4 bits): an arp chord's number */
static const uint8_t STEP_CHORD[16] = {0, 10, 11, 4, 5, 15, 6, 21, 12, 13, 14, 17, 18, 8, 20, 7};
static const char *const STEP_CHORD_NAME[16] = {"-", "MAJ", "MIN", "MAJ7", "MIN7", "DOM7", "SUS4", "SUS2", "ADD9", "MIN9", "MAJ9", "6TH", "MIN6", "DIM", "AUG", "PWR"};
/* RATE 0..3: the old free speeds (60 Hz ticks); 4..10: in time with the tempo, clock ticks (24 a beat) a note */
#define AR_SYNC 4u
#define AR_MAX 10u
static const uint8_t ARP_TICKS[4] = {6, 4, 3, 2};               /* 60 Hz ticks per arp note: 10, 15, 20, 30 a second */
static const uint8_t ARP_SYNC_T[7] = {24, 12, 8, 6, 4, 3, 2};   /* 1/4 1/8 1/8T 1/16 1/16T 1/32 1/32T */
static const char *const ARP_RATE_NAME[AR_MAX + 1u] = {"10/S", "15/S", "20/S", "30/S", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"};
#define AD_MAX 5u
static const char *const ARP_DIR_NAME[AD_MAX + 1u] = {"UP", "DOWN", "UPDN", "DNUP", "RAND", "CONV"};

/* ---- a channel's extra settings (0 = as before) */
typedef struct {
    uint8_t aoct, agate, au2, au3;              /* arp: octaves - 1 (0..3), gate in eighths (0 = legato), the USER chord's 2nd and 3rd notes (semitones) */
    uint8_t ftype, fcut, fres, fdec;            /* filter: 0 off, 1 LP, 2 BP, 3 HP, 4 LP24; cutoff 0..63; resonance 0..15; its envelope's fall 0..15 */
    int8_t fenv;                                /* the envelope's reach on the cutoff, -15..15 */
    uint8_t ktrk, drive, glide;                 /* key tracking 0..3, drive 0..15, glide 0..15 */
    uint8_t ltgt, lrate, ldep, lshape;          /* the second LFO: target (LT_*), rate 0..22, depth 0..15, shape 0..4 */
    uint8_t uni, udet, uspr;                    /* ensemble: copies 0..4 (0, 1 off), detune 0..15, spread 0..15 */
    uint8_t scho, sdly, srev;                   /* sends 0..15: chorus, delay, reverb */
    uint8_t speed, len, lkt, chv;               /* the channel's steps: speed (STEP_SPD index), length (0 = 16), what a step's lock moves (LK_*), chord voices on the channels below 0..3 */
    uint8_t va[12];                             /* the VA synth, the SNES and the Amiga (va.c) */
} ext_t;
#define EXT_BYTES 38u                           /* saved bytes of an ext_t (proj.c): the fields above in order */
typedef char ext_size_ok[sizeof(ext_t) == EXT_BYTES ? 1 : -1];
enum { LT_OFF, LT_PITCH, LT_CUT, LT_VOL, LT_PAN, LT_PWM, LT_N };
static const char *const LT_NAME[LT_N] = {"OFF", "PITCH", "CUT", "VOL", "PAN", "PWM"};
static const char *const LS_NAME[5] = {"TRI", "SAW", "SQR", "S&H", "RAMP"};
static const char *const LR_NAME[23] = {"0.1", "0.2", "0.3", "0.5", "0.7", "1", "1.5", "2", "3", "4", "5", "6", "8", "10", "14", "20", "4BAR", "2BAR", "1BAR", "1/2", "1/4", "1/8", "1/16"};
static const uint16_t LR_INC[16] = {109, 218, 328, 546, 765, 1092, 1638, 2185, 3277, 4369, 5461, 6554, 8738, 10923, 15292, 21845};      /* 16 bit phase a 60 Hz tick: 0.1 .. 20 Hz */
static const uint8_t LR_BEATS4[7] = {64, 32, 16, 8, 4, 2, 1};        /* the synced rates, in quarter beats a cycle */
enum { LK_OFF, LK_CUT, LK_RES, LK_FENV, LK_VOL, LK_TONE, LK_DEC, LK_PAN, LK_DLY, LK_REV, LK_CHO, LK_DRIVE, LK_N };
static const char *const LK_NAME[LK_N] = {"OFF", "CUT", "RES", "FENV", "VOL", "TONE", "DEC", "PAN", "DLY", "REV", "CHO", "DRIVE"};
static const uint8_t STEP_SPD[8] = {6, 4, 3, 8, 12, 2, 16, 24};          /* clock ticks a step: 1/16 1/16T 1/32 1/8T 1/8 1/32T 1/4T 1/4 */
static const char *const SPD_NAME[8] = {"1/16", "1/16T", "1/32", "1/8T", "1/8", "1/32T", "1/4T", "1/4"};
static const char *const FT_NAME[5] = {"OFF", "LP", "BP", "HP", "LP24"};
#define X_RATE_DEF (AR_SYNC + 3u)               /* a new channel's arp: 1/16, in time */


/* ---- the project's master settings (the MIX view, mixview.c): saved with the project */
typedef struct {
    uint8_t swing, scale, key, spare0;          /* swing 0..25 (50 % .. 75 %); the keys' scale lock (0 off, SCALES) and its key 0..11 */
    uint8_t cmode, crate, cdep, spare1;         /* chorus: 0 off, 1 I, 2 II, 3 I+II; rate, depth 0..15 */
    uint8_t dtime, dfb, dmode, dtone;           /* delay: DLY_* time, feedback 0..15, 0 stereo / 1 ping-pong, tone 0..15 */
    uint8_t rsize, rdamp, rgate, spare2;        /* reverb: size, damping 0..15, gate 0 off, 1..15 its length */
    uint8_t comp, sat, drel, dshape;            /* master: compressor, saturation 0..15; the sidechain's release 0..15 (0 the old 180 ms), its shape 0..2 */
} mset_t;
#define MSET_BYTES 20u
static const mset_t MSET_DEF = {0, 0, 0, 0, 1, 5, 8, 0, 2, 9, 0, 10, 8, 6, 0, 0, 0, 0, 0, 0};
static mset_t mset = {0, 0, 0, 0, 1, 5, 8, 0, 2, 9, 0, 10, 8, 6, 0, 0, 0, 0, 0, 0};
typedef char mset_size_ok[sizeof(mset_t) == MSET_BYTES ? 1 : -1];
#define DLY_N 9u
static const char *const DLY_NAME[DLY_N] = {"1/16", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "3/16"};
static const uint8_t DLY_T[DLY_N] = {6, 8, 12, 18, 16, 24, 36, 48, 18};        /* clock ticks (24 a beat) */
#define SCALES 10u
static const char *const SCALE_NAME[SCALES] = {"OFF", "MAJOR", "MINOR", "DORIAN", "PHRYG", "MIXO", "HARM", "PENTA", "PENTm", "BLUES"};
static const uint16_t SCALE_MASK[SCALES] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x5AB, 0x6B5, 0x9AD, 0x295, 0x4A9, 0x4E9};      /* bit n: n semitones above the key is in the scale */
static const char *const KEY_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static uint32_t scale_snap(uint32_t note)       /* the keys' scale lock: the note, or the nearest in the scale below it */
{
    uint32_t i;
    if (!mset.scale || mset.scale >= SCALES) return note;
    for (i = 0; i < 12u && note > i; i++) if (SCALE_MASK[mset.scale] >> ((note - i + 12u - mset.key % 12u) % 12u) & 1u) return note - i;
    return note;
}
static const uint16_t GL_K[16] = {256, 164, 135, 109, 85, 66, 50, 38, 29, 21, 16, 12, 9, 6, 5, 4};          /* glide: the part of the way a 60 Hz tick goes, /256 (16 ms .. 1.2 s) */
static const uint16_t FDK[16] = {12377, 20762, 29662, 37935, 44950, 50530, 54776, 57911, 60177, 61791, 62930, 63727, 64283, 64669, 64936, 65122};          /* the filter envelope's fall a 60 Hz tick, Q16 (10 ms .. 2.7 s) */
