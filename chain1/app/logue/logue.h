/* SPDX-License-Identifier: MPL-2.0 */
/* logue SDK units for the NTS-1 (mk1: .ntkdigunit, mkII: .nts1mkiiunit) in CHAIN-1: oscillators, modulation, delay
 * and reverb effects, their own ARM machine code run by the interpreter (arm.c) against the NTS-1's API (the
 * tables in logue_rom.c, the calls in logue.c).
 *
 * A unit reaches the FM-1 packed by the host tools (tools/logue_pack.py, the Android app) as an "LGU1" image: the
 * header below and the unit's code (mk1: payload.bin as the .ntkdigunit has it; mkII: the ELF's loadable segments,
 * the dynamic section inside them, laid out from address 0). It runs in an arena of RAM it is given: its own memory
 * (the mk1's SRAM window, or the mkII image), a stack and the I/O buffers, then the "SDRAM" its delay lines take,
 * as much as is left. A unit that wants more, faults, or runs too long stops, and the slot is bypassed. */
#pragma once
#include <stdint.h>
#include "arm.h"

#define LG_MAGIC 0x3155474Cu             /* "LGU1" */
#define LG_HDR 256u
#define LG_FRAMES 32u                    /* frames per call (the NTS-1: up to 64) */
#define LG_NPARAM 11

enum { LG_FMT_MK1 = 1, LG_FMT_MK2 = 2 };
/* the logue SDK's module numbers */
enum { LG_MOD_MODFX = 1, LG_MOD_DELFX = 2, LG_MOD_REVFX = 3, LG_MOD_OSC = 4 };
enum { LG_EMPTY, LG_READY, LG_FAULT };
/* why a unit is not running */
enum { LG_E_NONE, LG_E_FORMAT, LG_E_PLATFORM, LG_E_ROOM, LG_E_LINK, LG_E_INIT, LG_E_FAULT, LG_E_SLOW, LG_E_MEMORY };

/* the image's header (little-endian, LG_HDR bytes, then the code) */
typedef struct {
    uint32_t magic;
    uint8_t fmt, module, nparam, rsv0;
    uint32_t size;                       /* code bytes after the header */
    uint32_t memsz;                      /* mkII: the image's memory (with its bss); mk1: 0 */
    uint32_t dyn;                        /* mkII: the dynamic section's address in the image */
    uint32_t crc;                        /* CRC-32 (IEEE) of the code */
    char name[20];
    uint8_t rsv1[20];
    struct {                             /* mk1: the manifest's parameters (mkII: its header has them) */
        char name[20];
        int16_t min, max;
        uint8_t type;                    /* 0 a number, 1 percent */
        uint8_t rsv[7];
    } param[6];
} lg_hdr_t;

typedef struct {
    char name[22];
    int16_t min, max, init;
    uint8_t type, frac;                  /* type: the logue SDK's k_unit_param_type_* (1 percent ...), or LG_T_KNOB */
} lg_param_t;
#define LG_T_KNOB 0x80u                  /* a mk1 knob, 0..1023: shown as a percentage */

typedef struct {
    arm_t m;
    uint8_t state, err, fmt, module;
    char name[21];
    uint8_t nparam;                      /* every parameter the unit shows (mk1 osc: shape, alt, then 6 more) */
    lg_param_t param[LG_NPARAM];
    int32_t value[LG_NPARAM];
    uint8_t *arena;
    uint32_t arena_size, sdram_used, sdram_size;
    uint32_t sp, io;                     /* the stack's top, the I/O area (guest addresses) */
    uint32_t f_init, f_render, f_param, f_on, f_off, f_mute, f_value, f_suspend, f_resume, f_tempo, f_alloff,
             f_bend;
    uint32_t rnd;                        /* the API's noise source */
    uint32_t load;                       /* instructions per frame, the last call */
    float bpm;
} lg_unit_t;

/* what an image is, without loading it */
typedef struct {
    uint8_t fmt, module, nparam;
    uint32_t crc;
    char name[21];
    lg_param_t param[LG_NPARAM];
} lg_info_t;

/* the image's checks (header, size, CRC): 0 = fine, else LG_E_FORMAT */
int lg_check(const uint8_t *img, uint32_t len);
/* its name, module and parameters (mkII: read from its unit_header, nothing linked or run): 0 = fine */
int lg_peek(const uint8_t *img, lg_info_t *info);
/* load an image (in flash or RAM) into u, in the arena given; 0 = ready, else LG_E_* (u->err too) */
int lg_load(lg_unit_t *u, const uint8_t *img, uint8_t *arena, uint32_t arena_size);
void lg_unload(lg_unit_t *u);
/* a parameter, as the unit counts it (param[i].min .. max); the osc's shape and alt are 0 .. 1023 */
void lg_param(lg_unit_t *u, int i, int32_t v);
/* an oscillator: note (MIDI 0..127, fine 0..255 above it), shape LFO -1..1, n <= LG_FRAMES frames into out */
void lg_osc_note(lg_unit_t *u, int on, int note, int vel);
void lg_osc(lg_unit_t *u, float *out, uint32_t n, int note, uint32_t fine, float shape_lfo);
/* an effect: n <= LG_FRAMES stereo frames (interleaved, -1..1) processed in place */
void lg_fx(lg_unit_t *u, float *lr, uint32_t n);
void lg_tempo(lg_unit_t *u, float bpm);
void lg_suspend(lg_unit_t *u, int on);
const char *lg_error(int e);
