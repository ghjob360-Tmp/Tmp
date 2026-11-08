/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: every parameter of a patch, as Vital has it: one table (name, kind, range, default, whether the
 * matrix may modulate it). A patch is the values (uint16: continuous knobs 0..1023, choices their index), the
 * modulation matrix, the LFOs' drawn shapes and a name. The matrix modulates a parameter in its 0..1 space; the
 * core maps the sum to the parameter's units every control block (per voice, or once for the global ones). */
#pragma once
#include <stdint.h>

/* kinds */
enum { PK_KNOB, PK_BIP, PK_CHOICE };     /* knob 0..1023; bipolar knob (512 = centre); a choice (index) */
/* flags */
#define PF_MOD 1u                        /* the matrix may modulate it */
#define PF_VOICE 2u                      /* per voice (else global: the effects, the master) */

/* the X-macro: X(id, "NAME", kind, n (choices) , default, flags) */
#define OSC_PARAMS(k)                                                                                          \
    X(O##k##_ON, "ON", PK_CHOICE, 2, (k) == 1, 0)                                                              \
    X(O##k##_TABLE, "TABLE", PK_CHOICE, 64, 0, 0)                                                              \
    X(O##k##_FRAME, "FRAME", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                 \
    X(O##k##_FSPREAD, "FR SPREAD", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                          \
    X(O##k##_MORPH, "MORPH", PK_CHOICE, 12, 0, 0)                                                              \
    X(O##k##_MAMT, "MORPH AMT", PK_KNOB, 0, 512, 0)                                                            \
    X(O##k##_WARP, "WARP", PK_CHOICE, 13, 0, 0)                                                                \
    X(O##k##_WAMT, "WARP AMT", PK_KNOB, 0, 512, PF_MOD | PF_VOICE)                                             \
    X(O##k##_LEVEL, "LEVEL", PK_KNOB, 0, 724, PF_MOD | PF_VOICE)                                               \
    X(O##k##_PAN, "PAN", PK_BIP, 0, 512, PF_MOD | PF_VOICE)                                                    \
    X(O##k##_OCT, "TRANSPOSE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)  /* -48..48 semitones, steps of 1 */         \
    X(O##k##_TUNE, "TUNE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)      /* -100..100 cents */                      \
    X(O##k##_UNI, "UNISON", PK_CHOICE, 8, 0, 0)                    /* index 0..7 = 1..8 voices */             \
    X(O##k##_DET, "DETUNE", PK_KNOB, 0, 300, PF_MOD | PF_VOICE)                                                \
    X(O##k##_DPOW, "DET POWER", PK_BIP, 0, 768, PF_MOD | PF_VOICE)                                             \
    X(O##k##_BLEND, "BLEND", PK_KNOB, 0, 820, PF_MOD | PF_VOICE)                                               \
    X(O##k##_STEREO, "STEREO", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)                                            \
    X(O##k##_STACK, "STACK", PK_CHOICE, 11, 0, 0)                                                              \
    X(O##k##_PHASE, "PHASE", PK_KNOB, 0, 512, PF_MOD | PF_VOICE)                                               \
    X(O##k##_RPHASE, "RND PHASE", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)                                         \
    X(O##k##_DEST, "OUT", PK_CHOICE, 5, 0, 0)                      /* FILTER 1, FILTER 2, BOTH, EFFECTS, DIRECT */

#define FLT_PARAMS(k)                                                                                          \
    X(F##k##_ON, "ON", PK_CHOICE, 2, (k) == 1, 0)                                                              \
    X(F##k##_MODEL, "MODEL", PK_CHOICE, 8, 0, 0)                                                               \
    X(F##k##_STYLE, "STYLE", PK_CHOICE, 9, 0, 0)                                                               \
    X(F##k##_CUT, "CUTOFF", PK_KNOB, 0, 760, PF_MOD | PF_VOICE)   /* note 8..136 */                           \
    X(F##k##_RESO, "RESONANCE", PK_KNOB, 0, 200, PF_MOD | PF_VOICE)                                            \
    X(F##k##_DRIVE, "DRIVE", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                 \
    X(F##k##_BLEND, "BLEND", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                 \
    X(F##k##_KEY, "KEY TRACK", PK_BIP, 0, 512, PF_MOD | PF_VOICE)                                              \
    X(F##k##_MIX, "MIX", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)                                                  \
    X(F##k##_FX, "FORMANT X", PK_KNOB, 0, 512, PF_MOD | PF_VOICE)  /* comb / phaser: spread */                \
    X(F##k##_FY, "FORMANT Y", PK_KNOB, 0, 512, PF_MOD | PF_VOICE)                                              \
    X(F##k##_TRANS, "F.TRANSPOSE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)

#define ENV_PARAMS(k)                                                                                          \
    X(E##k##_DELAY, "DELAY", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)     /* times: Vital's (v * 2.38)^4 s, ~0..32 s */ \
    X(E##k##_ATK, "ATTACK", PK_KNOB, 0, 160, PF_MOD | PF_VOICE)                                                \
    X(E##k##_HOLD, "HOLD", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                   \
    X(E##k##_DEC, "DECAY", PK_KNOB, 0, 420, PF_MOD | PF_VOICE)                                                 \
    X(E##k##_SUS, "SUSTAIN", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)                                              \
    X(E##k##_REL, "RELEASE", PK_KNOB, 0, 330, PF_MOD | PF_VOICE)                                               \
    X(E##k##_APOW, "ATK CURVE", PK_BIP, 0, 512, PF_VOICE)                                                      \
    X(E##k##_DPOW, "DEC CURVE", PK_BIP, 0, 820, PF_VOICE)                                                      \
    X(E##k##_RPOW, "REL CURVE", PK_BIP, 0, 820, PF_VOICE)

#define LFO_PARAMS(k)                                                                                          \
    X(L##k##_SHAPE, "SHAPE", PK_CHOICE, 10, 0, 0)                  /* LFO_SHAPE_*; the last is the drawn one */ \
    X(L##k##_RATE, "RATE", PK_KNOB, 0, 450, PF_MOD | PF_VOICE)     /* 1/32 .. 32 Hz, exp */                    \
    X(L##k##_SYNC, "SYNC", PK_CHOICE, 14, 0, 0)                    /* 0 free, else a tempo division */        \
    X(L##k##_MODE, "MODE", PK_CHOICE, 6, 0, 0)                     /* LFO_MODE_* */                           \
    X(L##k##_PHASE, "PHASE", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                 \
    X(L##k##_FADE, "FADE IN", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                \
    X(L##k##_DELAY, "DELAY", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                                 \
    X(L##k##_SMOOTH, "SMOOTH", PK_KNOB, 0, 0, PF_MOD | PF_VOICE)                                               \
    X(L##k##_AMP, "AMOUNT", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)

#define RND_PARAMS(k)                                                                                          \
    X(R##k##_STYLE, "STYLE", PK_CHOICE, 4, 0, 0)                   /* PERLIN, S&H, SINE, LORENZ */            \
    X(R##k##_RATE, "RATE", PK_KNOB, 0, 450, PF_MOD | PF_VOICE)                                                 \
    X(R##k##_SYNC, "SYNC", PK_CHOICE, 14, 0, 0)                                                                \
    X(R##k##_MODE, "MODE", PK_CHOICE, 2, 0, 0)                     /* FREE, TRIGGER */                         \
    X(R##k##_AMP, "AMOUNT", PK_KNOB, 0, 1023, PF_MOD | PF_VOICE)

#define FX_PARAMS(N)                                                                                           \
    X(N##_ON, "ON", PK_CHOICE, 2, 0, 0)                                                                        \
    X(N##_P0, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P1, "", PK_KNOB, 0, 512, PF_MOD)                              \
    X(N##_P2, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P3, "", PK_KNOB, 0, 512, PF_MOD)                              \
    X(N##_P4, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P5, "", PK_KNOB, 0, 512, PF_MOD)                              \
    X(N##_P6, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P7, "", PK_KNOB, 0, 512, PF_MOD)                              \
    X(N##_P8, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P9, "", PK_KNOB, 0, 512, PF_MOD)                              \
    X(N##_P10, "", PK_KNOB, 0, 512, PF_MOD) X(N##_P11, "", PK_KNOB, 0, 512, PF_MOD)

#define ALL_PARAMS                                                                                             \
    OSC_PARAMS(1) OSC_PARAMS(2) OSC_PARAMS(3)                                                                  \
    X(S_ON, "ON", PK_CHOICE, 2, 0, 0)                                                                          \
    X(S_SAMPLE, "SAMPLE", PK_CHOICE, 32, 0, 0)                                                                 \
    X(S_LEVEL, "LEVEL", PK_KNOB, 0, 600, PF_MOD | PF_VOICE)                                                    \
    X(S_PAN, "PAN", PK_BIP, 0, 512, PF_MOD | PF_VOICE)                                                         \
    X(S_OCT, "TRANSPOSE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)                                                   \
    X(S_TUNE, "TUNE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)                                                       \
    X(S_KEY, "KEY TRACK", PK_CHOICE, 2, 1, 0)                                                                  \
    X(S_LOOP, "LOOP", PK_CHOICE, 2, 1, 0)                                                                      \
    X(S_RSTART, "RND START", PK_CHOICE, 2, 1, 0)                                                               \
    X(S_DEST, "OUT", PK_CHOICE, 5, 0, 0)                                                                       \
    FLT_PARAMS(1) FLT_PARAMS(2)                                                                                \
    X(F_ROUTE, "ROUTING", PK_CHOICE, 2, 0, 0)                      /* PARALLEL, SERIAL (FILTER 1 -> FILTER 2) */ \
    ENV_PARAMS(1) ENV_PARAMS(2) ENV_PARAMS(3) ENV_PARAMS(4) ENV_PARAMS(5) ENV_PARAMS(6)                        \
    LFO_PARAMS(1) LFO_PARAMS(2) LFO_PARAMS(3) LFO_PARAMS(4) LFO_PARAMS(5) LFO_PARAMS(6) LFO_PARAMS(7)          \
    LFO_PARAMS(8)                                                                                              \
    RND_PARAMS(1) RND_PARAMS(2) RND_PARAMS(3) RND_PARAMS(4)                                                    \
    X(MC1, "MACRO 1", PK_KNOB, 0, 0, 0) X(MC2, "MACRO 2", PK_KNOB, 0, 0, 0)                                    \
    X(MC3, "MACRO 3", PK_KNOB, 0, 0, 0) X(MC4, "MACRO 4", PK_KNOB, 0, 0, 0)                                    \
    X(V_POLY, "VOICES", PK_CHOICE, 8, 7, 0)                        /* index 0..7 = 1..8 */                    \
    X(V_LEGATO, "LEGATO", PK_CHOICE, 2, 0, 0)                                                                  \
    X(V_GLIDE, "GLIDE", PK_KNOB, 0, 0, 0)                                                                      \
    X(V_GSLOPE, "GLIDE SLOPE", PK_BIP, 0, 512, 0)                                                              \
    X(V_GFORCE, "ALWAYS GLIDE", PK_CHOICE, 2, 0, 0)                                                            \
    X(V_BEND, "BEND RANGE", PK_CHOICE, 25, 2, 0)                                                               \
    X(V_VEL, "VEL TRACK", PK_KNOB, 0, 600, 0)                                                                  \
    X(V_TEMPO, "TEMPO", PK_KNOB, 0, 409, 0)                        /* 40..240 BPM */                           \
    X(V_TUNE, "TUNE", PK_BIP, 0, 512, PF_MOD | PF_VOICE)          /* -1..1 semitone: the whole synth */      \
    X(V_VOLUME, "VOLUME", PK_KNOB, 0, 760, PF_MOD)                                                             \
    FX_PARAMS(X_CHO) FX_PARAMS(X_CMP) FX_PARAMS(X_DLY) FX_PARAMS(X_DST) FX_PARAMS(X_EQ) FX_PARAMS(X_FLT)       \
    FX_PARAMS(X_FLG) FX_PARAMS(X_PHS) FX_PARAMS(X_REV)

#define X(id, name, kind, n, def, fl) P_##id,
enum { ALL_PARAMS NPARAM };
#undef X

/* strides, so code can address the k-th oscillator / filter / envelope / LFO / random LFO / effect */
#define P_OSC(k, f) (P_O1_##f + (k) * (P_O2_ON - P_O1_ON))
#define P_FLT(k, f) (P_F1_##f + (k) * (P_F2_ON - P_F1_ON))
#define P_ENV(k, f) (P_E1_##f + (k) * (P_E2_DELAY - P_E1_DELAY))
#define P_LFO(k, f) (P_L1_##f + (k) * (P_L2_SHAPE - P_L1_SHAPE))
#define P_RND(k, f) (P_R1_##f + (k) * (P_R2_STYLE - P_R1_STYLE))
#define P_FX(t) (P_X_CHO_ON + (t) * (P_X_CMP_ON - P_X_CHO_ON))   /* t: FXT_*; + 1 + i = its parameter i */

typedef struct {
    const char *name;
    uint8_t kind, n, flags;
    uint16_t def;
} pdesc_t;
extern const pdesc_t PDESC[NPARAM];

/* LFO shapes and modes (Vital's) */
enum { LS_SINE, LS_TRI, LS_SAWUP, LS_SAWDN, LS_SQUARE, LS_EXPUP, LS_BUMP, LS_STEPS, LS_SMOOTHSTEP, LS_DRAWN, LS_N };
enum { LM_TRIGGER, LM_SYNC, LM_ENVELOPE, LM_SUSENV, LM_LOOPPOINT, LM_LOOPHOLD, LM_N };
#define LFO_PTS 16                       /* a drawn shape: 16 points (y 0..255) and a curve per segment */

/* the modulation sources */
enum { MS_NONE, MS_ENV1, MS_ENV2, MS_ENV3, MS_ENV4, MS_ENV5, MS_ENV6, MS_LFO1, MS_LFO2, MS_LFO3, MS_LFO4, MS_LFO5,
       MS_LFO6, MS_LFO7, MS_LFO8, MS_RND1, MS_RND2, MS_RND3, MS_RND4, MS_MAC1, MS_MAC2, MS_MAC3, MS_MAC4, MS_VEL,
       MS_PRESS, MS_NOTE, MS_NOTEOCT, MS_WHEEL, MS_BEND, MS_RANDOM, MS_N };
#define NSLOT 32                         /* matrix slots */
typedef struct {
    uint8_t src, flags;                  /* flags: 1 bipolar (the source -1..1), 2 bypass */
    uint16_t dst;                        /* a parameter (P_*), 0xFFFF none */
    int16_t amt;                         /* -1000..1000: the share of the destination's whole range */
    int16_t pow;                         /* -100..100: Vital's power (curve on the source), / 10 */
} mslot_t;

#define NAME_LEN 16
typedef struct {
    _Alignas(4) uint16_t v[NPARAM];      /* (_Alignas: the whole patch aligned: see below) */
    mslot_t m[NSLOT];
    uint8_t lfo_y[8][LFO_PTS];           /* the drawn shapes */
    int8_t lfo_c[8][LFO_PTS];            /* each segment's curve, -100..100 */
    uint8_t fx_order[9];                 /* FXT_* in chain order */
    char name[NAME_LEN];
    uint8_t rsv[3];
} patch_t;                              /* (aligned and a multiple of 4: core_patch copies it by words, and the
                                           FM-1's CPU faults on an unaligned word: 1290 bytes put the second slot
                                           at an odd half-word and the firmware crashed at its first patch) */
_Static_assert(sizeof(patch_t) % 4u == 0u && _Alignof(patch_t) >= 4u, "patch_t is copied by words");
