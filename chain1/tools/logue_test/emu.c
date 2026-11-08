/* SPDX-License-Identifier: MPL-2.0 */
/* the interpreter's run of a test ELF: its load segments at their addresses, test_main(out) called, the words
 * written to stdout (compared with qemu-arm's). Host only: emu ELF [budget] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm.h"

static uint8_t img[4u << 20], stk[256u << 10], outb[1u << 20];

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static uint8_t e[8u << 20];
    size_t n = fread(e, 1, sizeof e, f);
    uint32_t phoff, phnum, i, entry_sym = 0;
    arm_t m;
    int rc;
    (void)n;
    fclose(f);
    phoff = *(uint32_t *)(e + 28);
    phnum = *(uint16_t *)(e + 44);
    for (i = 0; i < phnum; i++) {
        uint8_t *ph = e + phoff + 32u * i;
        uint32_t type = *(uint32_t *)ph, off = *(uint32_t *)(ph + 4), va = *(uint32_t *)(ph + 8);
        uint32_t fsz = *(uint32_t *)(ph + 16), msz = *(uint32_t *)(ph + 20);
        if (type != 1)
            continue;
        memcpy(img + (va - 0x20000000u), e + off, fsz);
        memset(img + (va - 0x20000000u) + fsz, 0, msz - fsz);
    }
    {   /* test_main's address from the symbol table */
        uint32_t shoff = *(uint32_t *)(e + 32), shnum = *(uint16_t *)(e + 48), k;
        for (k = 0; k < shnum; k++) {
            uint8_t *sh = e + shoff + 40u * k;
            if (*(uint32_t *)(sh + 4) == 2) {           /* SHT_SYMTAB */
                uint32_t so = *(uint32_t *)(sh + 16), ssz = *(uint32_t *)(sh + 20), link = *(uint32_t *)(sh + 24);
                uint8_t *strs = e + *(uint32_t *)(e + shoff + 40u * link + 16);
                uint32_t j;
                for (j = 0; j < ssz / 16u; j++) {
                    uint8_t *s = e + so + 16u * j;
                    if (!strcmp((char *)strs + *(uint32_t *)s, argc > 3 ? argv[3] : "test_main"))
                        entry_sym = *(uint32_t *)(s + 4);
                }
            }
        }
    }
    arm_reset(&m);
    arm_map(&m, 0x20000000u, sizeof img, img, 1);
    arm_map(&m, 0x30000000u, sizeof stk, stk, 1);
    arm_map(&m, 0x40000000u, sizeof outb, outb, 1);
    rc = arm_call(&m, entry_sym, 0x30000000u + sizeof stk, (uint32_t[]){0x40000000u}, 1,
                  argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0xFFFFFFFFu);
    if (rc) {
        fprintf(stderr, "fault %d at pc %08x addr %08x (r0 %08x lr %08x)\n", rc, m.fault_pc, m.fault_addr, m.r[0], m.r[14]);
        return 2;
    }
    fwrite(outb, 4, m.r[0], stdout);
    fprintf(stderr, "%u words, %u instructions\n", m.r[0], m.count);
    return 0;
}

void arm_trap(arm_t *m, uint32_t addr)    /* (no firmware here) */
{
    m->fault = ARM_F_TRAP;
    m->fault_addr = addr;
}
