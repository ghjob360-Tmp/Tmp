/* SPDX-License-Identifier: MPL-2.0 */
/* the reference run: qemu-arm (Linux user mode, -cpu cortex-m4) starts here, calls test_main and writes its words to
 * stdout. The interpreter's run calls test_main itself (emu.c), never this. */
#include <stdint.h>
extern uint32_t test_main(uint32_t *out);
static uint32_t out[1u << 18];
static long sys3(long n, long a, long b, long c)
{
    register long r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c, r7 __asm__("r7") = n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
void _start(void)
{
    uint32_t n = test_main(out);
    sys3(4, 1, (long)out, (long)(n * 4u));
    sys3(1, 0, 0, 0);
    for (;;)
        ;
}
