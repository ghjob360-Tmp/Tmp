/* SPDX-License-Identifier: MPL-2.0 */
/* HELLO: the smallest fm1-base firmware that makes a sound.
 * This header is the app's half of the contract with the platform (base/device/unity.c on the FM-1,
 * base/host/plat_host.c in the host simulator, the browser emulator and the .rfm1). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "HELLO"
#define APP_SPLASH_RGB RGB(90, 200, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */

/* the store: our settings and the platform's panel calibration, and their flash offsets (the data area) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator's checks (tests/basic.txt "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
