/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: a Vital-style wavetable synth for the M-VAVE FM-1. The app's half of the contract with fm1-base. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "PRISM-1"
#define APP_SPLASH_RGB RGB(150, 110, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */
#define APP_OVERLOAD                     /* the render over 90 % of its time: shed a voice */
void app_overload(void);

enum { OBJ_SETTINGS, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
