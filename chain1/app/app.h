/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1: OSC > FILTER > EG > MOD > DELAY > REVERB, up to eight voices, an arpeggiator, logue SDK units.
 * This header is the app's half of the contract with the platform (base/device/unity.c on the FM-1,
 * base/host/plat_host.c in the host simulator, the browser emulator and the .rfm1). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "CHAIN-1"
#define APP_SPLASH_RGB RGB(255, 140, 60)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */

/* the store: our settings and the platform's panel calibration, and their flash offsets (the data area) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BANK, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xD8000u}   /* (the unit bank: 0x97000..0xD5FFF, dsp.c) */

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_TOUCH                        /* velocity (and pressure) from the keys (lib/touch.h) */
#define APP_OVERLOAD                     /* the render over 90 % of its time: fewer voices for a while */
void app_overload(void);
#define APP_USB_CMD                      /* logue units sent from a computer or phone (app.c: 0x70..0x72) */
void app_usb_cmd(const uint8_t *d, uint32_t n);

/* the host simulator's checks (tests/basic.txt "expect") */
#define APP_USB_HOST_NAME                /* the USB host reads its keyboard's name */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
