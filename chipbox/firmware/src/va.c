/* SPDX-License-Identifier: GPL-3.0-only */
/* va.c: the VA synth, the SNES and the Amiga (filled in below). Included by chips.c before seq.c. */
static uint32_t is_va(uint32_t k) { (void)k; return 0; }
static void va_on(uint32_t c, uint32_t note, uint32_t vel, uint32_t add) { (void)c; (void)note; (void)vel; (void)add; }
static void va_off(uint32_t c, uint32_t note) { (void)c; (void)note; }
static void ext_default(uint32_t kind, ext_t *x) { (void)kind; (void)x; }
