/* SPDX-License-Identifier: MPL-2.0 */
/* An interpreter of the ARMv7E-M instruction set (Thumb-2 with the DSP extension, and the single-precision FPU of a
 * Cortex-M4F, FPv4-SP): what a logue SDK unit for the NTS-1 (mk1: Cortex-M4, mkII: Cortex-M7 built for FPv4-SP) is
 * compiled to. The FM-1's CPU is not an ARM, so CHAIN-1 runs a unit's own machine code here, instruction by
 * instruction (logue.c).
 *
 * A machine (arm_t) is the registers and a memory map of up to 16 segments, one per top nibble of the address
 * (0x0... the firmware's tables, 0x2... a unit's code and data, ...). Every access is checked against its segment:
 * a unit reads and writes its own memory only, and anything out of place (a stray address, an instruction that is
 * not Thumb-2, a run longer than its budget) stops the machine with a fault, never the FM-1.
 * Calls out to the firmware (the logue API's functions) are addresses in a trap range: arm_trap is called with
 * the address and returns as the function would (r0 / s0). */
#pragma once
#include <stdint.h>
#include "plat.h"

enum {
    ARM_OK = 0,
    ARM_F_MEM,        /* an access outside the map, or a write to a read-only segment */
    ARM_F_UNDEF,      /* an instruction we do not have (or not Thumb-2 / FPv4-SP: a double-precision op) */
    ARM_F_BUDGET,     /* more instructions than the call's budget (a unit too slow, or stuck) */
    ARM_F_STATE,      /* a branch to ARM state (bit 0 of the address clear), SVC, BKPT, UDF */
    ARM_F_TRAP,       /* a trap address the firmware does not know */
};

typedef struct {
    uint32_t base, size;  /* [base, base + size) */
    uint8_t *p;           /* its bytes in our memory */
    uint8_t w;            /* writable */
} arm_seg_t;

typedef struct arm_s arm_t;

struct arm_s {
    uint32_t r[16];
    uint32_t n, z, c, v, q;          /* the flags, each 0 / 1 */
    uint32_t ge;                     /* GE[3:0] */
    uint32_t it;                     /* ITSTATE */
    union { float f[32]; uint32_t u[32]; } s;   /* S0..S31 */
    uint32_t fpscr;
    arm_seg_t seg[16];
    uint32_t trap_lo, trap_hi;       /* [lo, hi): calls into the firmware: arm_trap */
    void *user;
    int fault;                       /* ARM_OK while it runs */
    uint32_t fault_pc, fault_addr;
    uint32_t count;                  /* instructions executed (all calls) */
};

#define ARM_RET 0xFFFFFFF0u          /* the return address a call starts with: back to us */

void arm_reset(arm_t *m);
/* a segment at base (its top nibble picks the slot; base's low 28 bits may be nonzero) */
void arm_map(arm_t *m, uint32_t base, uint32_t size, void *p, int writable);
/* a call: fn(a0..a3) with sp, at most budget instructions -> fault (ARM_OK: returned; r0 / s0 hold the result) */
int arm_call(arm_t *m, uint32_t fn, uint32_t sp, const uint32_t *args, int nargs, uint32_t budget);

/* the firmware's side of a call into [trap_lo, trap_hi): set r0 / s0 as the function would (or m->fault). The user
 * of the interpreter defines it (logue.c) */
void arm_trap(arm_t *m, uint32_t addr);

/* the memory as the unit sees it (checked; 0 / no-op outside) */
uint8_t *arm_ptr(arm_t *m, uint32_t addr, uint32_t n, int write);
uint32_t arm_rd32(arm_t *m, uint32_t addr);
void arm_wr32(arm_t *m, uint32_t addr, uint32_t v);
