/* SPDX-License-Identifier: GPL-3.0-only */
/* The Chipbox core for FM1VST (https://github.com/Quixotic7/FM1VST): Chipbox (Synthwave edition), a Dinghy app, on its
 * own host shim (tests/host.c, CHIPBOX_HOST + CHIPBOX_CORE), built for the plugin and as an .rfm1 for RemoteFM1.
 * It uses the Dinghy family glue (core-glue/dinghy/dinghy_pre.h, dinghy_glue.h) with one difference: Chipbox's main
 * loop returns after each pass (CHIPBOX_STEP), so the "coroutine" here just calls it once a tick. That needs no ucontext,
 * and so it links in WebAssembly (the .rfm1), where the family's coro.c is not available.
 *   the input     into the shim: fm1_in.notes / buttons, host_edges, host_rel, enc_acc (the encoders: Dinghy's ids)
 *   the LEDs      host_led (0 off, 1 glow, 2 on)
 *   the flash     the shim's RAM stores in the device's layout: the projects (raw, PROJ_AT), the user patches (raw,
 *                 PATCH_AT), the settings (a DNGY record at 0xFC000); read before the boot, written after each pass
 * Not here: USB audio, Bluetooth (Brothers Link), the installer. */

/* the main loop's stand-in for common/coro (unique names: no clash with coro.c where it is linked) */
#define fm1coro_new cb_coro_new
#define fm1coro_resume cb_coro_resume
#define fm1coro_yield cb_coro_yield
#define fm1coro_park cb_coro_park
#define fm1coro_inside cb_coro_inside
#define fm1coro_done cb_coro_done
#define fm1coro_resumes cb_coro_resumes
#define fm1coro_stack_peak cb_coro_stack_peak
#define fm1coro_free cb_coro_free
#include "dinghy_pre.h"

#define CHIPBOX_HOST 1
#define CHIPBOX_CORE 1
#define CHIPBOX_STEP 1
#include "chipbox.c"
#include "exit_end.h"

struct fm1coro { void (*fn)(void *); void *arg; int done; uint32_t resumes; };
static int cb_in;
fm1coro_t *cb_coro_new(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    fm1coro_t *c = (fm1coro_t *)calloc(1, sizeof *c);
    (void)stack_bytes;
    if (c) { c->fn = fn; c->arg = arg; }
    return c;
}
int cb_coro_resume(fm1coro_t *c)                /* one pass: app_main returns after it (CHIPBOX_STEP) */
{
    if (!c || c->done) return 0;
    if (cb_in) return -1;
    cb_in = 1; c->fn(c->arg); cb_in = 0;
    c->resumes++;
    return c->done ? 0 : 1;
}
void cb_coro_yield(void) {}
static fm1coro_t *cb_coro_cur;
void cb_coro_park(void) { if (cb_coro_cur) cb_coro_cur->done = 1; }
int cb_coro_inside(void) { return cb_in; }
int cb_coro_done(const fm1coro_t *c) { return !c || c->done; }
uint32_t cb_coro_resumes(const fm1coro_t *c) { return c ? c->resumes : 0u; }
size_t cb_coro_stack_peak(const fm1coro_t *c) { (void)c; return 0; }
void cb_coro_free(fm1coro_t *c) { free(c); }

/* ---- the glue's hooks (dinghy_glue.h) */
static void fm1core_dg_hal_in(uint32_t notes, uint32_t held, uint32_t down, uint32_t up, const int32_t *enc7)
{
    uint32_t i;
    fm1_in.notes = notes;
    fm1_in.buttons = held;
    host_edges |= down;
    host_rel |= up;
    for (i = 0; i < 7u; i++) enc_acc[i] += enc7[i];
}
static uint8_t fm1core_dg_led_level(uint32_t id) { return id < 64u ? (host_led[id] == 2u ? 255u : host_led[id] == 1u ? 48u : 0u) : 0u; }
static int fm1core_dg_midi_put(uint8_t st, uint8_t d1, uint8_t d2)
{
    if (mq_w - mq_r >= 64u) return 0;
    mq[mq_w % 64u][0] = st; mq[mq_w % 64u][1] = d1; mq[mq_w % 64u][2] = d2;
    mq_w++;
    return 1;
}
#define CB_SETTINGS_AT 0x000FC000u
static uint32_t cb_gen_seen = ~0u;
static void fm1core_dg_stores_load(void)        /* the image -> the shim (an erased image: a new device) */
{
    uint32_t n = 0;
    memcpy(host_flash, fm1nor_image + PROJ_AT, sizeof host_flash);
    memcpy(host_patch, fm1nor_image + PATCH_AT, sizeof host_patch);
    memset(host_store, 0, sizeof host_store);
    if (fm1core_dg_sector_read(CB_SETTINGS_AT, host_store + 4, sizeof host_store - 4u, &n) == 0) { host_store[0] = 1; host_store_n = n; }
    cb_gen_seen = host_gen;
}
static void fm1core_dg_stores_poll(void)        /* the shim -> the image, when it wrote */
{
    uint32_t s;
    if (host_gen == cb_gen_seen) return;
    cb_gen_seen = host_gen;
    for (s = 0; s < sizeof host_flash; s += FM1NOR_SECTOR) fm1core_dg_raw_sync(PROJ_AT + s, host_flash + s);
    fm1core_dg_raw_sync(PATCH_AT, host_patch);
    if (host_store[0] == 1u && host_store_n) {
        uint32_t n = 0;
        static uint8_t cur[256];
        if (fm1core_dg_sector_read(CB_SETTINGS_AT, cur, sizeof cur, &n) || n != host_store_n || memcmp(cur, host_store + 4, n))
            fm1core_dg_sector_save(CB_SETTINGS_AT, host_store + 4, host_store_n);
    }
}
static int fm1core_dg_sync(void) { return 1; }  /* (Chipbox saves a project itself once it is stopped and quiet) */
static void fm1core_dg_info(char *buf, uint32_t n) { snprintf(buf, n, "project %u%s, %u channels, %u bpm", (unsigned)proj_cur + 1u, proj_dirty ? " (unsaved)" : "", (unsigned)chan_n, (unsigned)seq_bpm); }

#include "dinghy_glue.h"
