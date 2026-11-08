/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: a four-track pocket studio for the M-VAVE FM-1, its four knobs colour-coded. The app's half of the
 * contract with fm1-base. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "TOYBOX-4"
#define APP_SPLASH_RGB RGB(255, 118, 46)
#define APP_BLOCK 256u
#define APP_USB_HOST_NAME                /* the USB host reads its keyboard's name */
#define APP_OVERLOAD                     /* the render over 90 % of its time: shed a voice */
void app_overload(void);
#define APP_TOUCH                        /* velocity from the keys */

/* the store: only the panel calibration (at Felucca's settings pair); the project, the library, the tape and the
 * samples live in the app's own flash regions (app.c) */
enum { OBJ_PANEL, OBJ_NOBJ };
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
