/* SPDX-License-Identifier: MPL-2.0 */
/* logue units on the host: load an LGU1 image, play a note (oscillators) or a burst of noise (effects), report what
 * it costs in ARM instructions per frame and write the sound as a raw float file. Host only:
 *   units IMAGE.lgu OUT.raw [seconds] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "logue.h"

static uint8_t arena[256u << 10];

int main(int argc, char **argv)
{
    static uint8_t img[1u << 20];
    FILE *f = fopen(argv[1], "rb");
    uint32_t len = (uint32_t)fread(img, 1, sizeof img, f), frames, done = 0, peak_load = 0;
    lg_unit_t u;
    int rc, i;
    double secs = argc > 3 ? atof(argv[3]) : 2.0, insns = 0, energy = 0;
    FILE *o = fopen(argv[2], "wb");
    fclose(f);
    if ((rc = lg_check(img, len))) {
        printf("check: %s\n", lg_error(rc));
        return 1;
    }
    rc = lg_load(&u, img, arena, sizeof arena);
    printf("%s: module %d fmt %d, %s, init %u insns, sdram %u bytes\n", u.name, u.module, u.fmt,
           rc ? lg_error(rc) : "ready", u.m.count, u.sdram_used);
    if (rc) {
        printf("  fault pc %08x addr %08x\n", u.m.fault_pc, u.m.fault_addr);
        return 2;
    }
    for (i = 0; i < u.nparam; i++)
        printf("  param %d %-22s %d..%d init %d type %d\n", i, u.param[i].name, u.param[i].min, u.param[i].max,
               u.param[i].init, u.param[i].type);
    frames = (uint32_t)(secs * 44100.0);
    if (u.module == LG_MOD_OSC) {
        lg_param(&u, 0, 400);
        lg_osc_note(&u, 1, 57, 100);
    }
    while (done < frames && u.state == LG_READY) {
        float buf[2 * LG_FRAMES];
        uint32_t n = LG_FRAMES, k, before = u.m.count;
        if (u.module == LG_MOD_OSC) {
            float mono[LG_FRAMES];
            lg_osc(&u, mono, n, done < frames / 2 ? 57 : 64, 0, 0.0f);
            for (k = 0; k < n; k++)
                buf[2 * k] = buf[2 * k + 1] = mono[k];
        } else {
            for (k = 0; k < n; k++)                           /* clicks: an impulse every half second */
                buf[2 * k] = buf[2 * k + 1] = ((done + k) % 22050u) < 40u ? 0.5f : 0.0f;
            lg_fx(&u, buf, n);
        }
        insns += u.m.count - before;
        if (u.m.count - before > peak_load)
            peak_load = u.m.count - before;
        for (k = 0; k < 2 * n; k++)
            energy += buf[k] * buf[k];
        fwrite(buf, sizeof(float), 2 * n, o);
        done += n;
    }
    fclose(o);
    printf("  %s: %.0f insns/frame (peak call %.0f/frame), rms %.4f\n", u.state == LG_READY ? "ran" : lg_error(u.err),
           insns / done, (double)peak_load / LG_FRAMES, sqrt(energy / (2.0 * done)));
    if (u.state != LG_READY)
        printf("  fault pc %08x addr %08x\n", u.m.fault_pc, u.m.fault_addr);
    return u.state == LG_READY ? 0 : 3;
}
