/* SPDX-License-Identifier: MPL-2.0 */
/* HELLO: a two-operator FM voice on the keys.
 *   keys          play (last note wins), sent out over USB-MIDI too; MIDI in plays it as well
 *   OCT- / OCT+   octave -2..+2
 *   K1            FM depth (modulation index 0..8)
 *   SAVE          keeps octave and depth across a power cycle (lit while there is something to save)
 *   MASTER        volume (the platform's)
 * Part of the unity build after the platform: gfx.c (cv_*, fonts, colours) and announce.c are in scope. */
#include "fastmath.h"

#define SET_MAGIC 0x48454C31u            /* "HEL1" */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t depth;                       /* 0 .. 16: modulation index = depth / 2 */
    uint8_t rsv[2];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

/* ------------------------------------------------------------------ voice --- */
static volatile int32_t voice_note = -1;
static float car_phase, mod_phase, level, gain_now;

static void midi_in(void)                /* audio interrupt: what came in over USB / TRS */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (st == 0x90u && d2)
            voice_note = (int32_t)d1;
        else if ((st == 0x80u || st == 0x90u) && voice_note == (int32_t)d1)
            voice_note = -1;
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    int32_t note;
    float target, inc, index, gain;
    midi_in();
    note = voice_note;
    target = note >= 0 ? 0.3f : 0.0f;
    inc = note >= 0 ? 440.0f * fm_exp2f((float)(note - 69) / 12.0f) / 44100.0f : 0.0f;
    index = (float)set.depth * 0.5f;
    gain = (float)master_q12 / 4096.0f * 8300000.0f;    /* 24-bit output */
    for (i = 0; i < n; i++) {
        float m, s;
        level += (target - level) * 0.002f;             /* ~10 ms attack / release */
        gain_now += (gain - gain_now) * 0.004f;
        mod_phase += inc;                               /* modulator at 1:1 */
        if (mod_phase >= 1.0f)
            mod_phase -= 1.0f;
        car_phase += inc;
        if (car_phase >= 1.0f)
            car_phase -= 1.0f;
        m = fm_sinf(mod_phase * 6.2831853f) * index * level;   /* the index follows the envelope */
        s = fm_sinf(car_phase * 6.2831853f + m) * level * gain_now;
        lr[2 * i] = (int32_t)s;
        lr[2 * i + 1] = (int32_t)s;
    }
}

void app_panic(void)
{
    voice_note = -1;
    level = 0.0f;
}

/* --------------------------------------------------------------- settings --- */
void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2 || set.depth > 16u) {
        set.magic = SET_MAGIC;
        set.octave = 0;
        set.depth = 4;
    }
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint8_t redraw;
} ui;

static void copy_text(char *d, const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1u < max && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

void app_say(const char *a, const char *b)
{
    copy_text(ui.msg[0], a, sizeof ui.msg[0]);
    copy_text(ui.msg[1], b, sizeof ui.msg[1]);
    ui.msg_until = plat_ms() + 1200u;
    ui.redraw = 1;
    if (ann_on())
        ann_say(ui.msg[0], 0, ANN_NOW);
}

static void num_text(int32_t v, char *b)
{
    char t[12];
    int k = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[k++] = (char)('0' + u % 10u);
    while ((u /= 10u) && k < 11);
    if (v < 0)
        *b++ = '-';
    while (k)
        *b++ = t[--k];
    *b = 0;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    char t[12];
    for (k = 0; k < NKEYS; k++) {            /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            voice_note = note;
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | 100u << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            if (voice_note == note)
                voice_note = -1;
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN || k == B_OCTUP) {
            int32_t o = clamp(set.octave + (k == B_OCTUP ? 1 : -1), -2, 2);
            if (o != set.octave) {
                voice_note = -1;
                set.octave = (int8_t)o;
                set_dirty = 1;
                num_text(o, t);
                app_say("OCTAVE", t);
            }
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
        }
    }
    if ((e = plat_enc(EN_K1)) != 0) {
        set.depth = (uint8_t)clamp((int32_t)set.depth + e, 0, 16);
        set_dirty = 1;
        num_text(set.depth, t);
        app_say("DEPTH", t);
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u), keys);
}

void app_input_only(void) { input(); }

static void draw(void)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char t[24], o[12];
    int32_t note = voice_note, x;
    /* the canvas holds 240 x 144 at most (gfx.c APP_CANVAS_PX): two halves of 120 rows */
    cv_begin(240, 120, C_BLACK);
    cv_text(12, 12, &FONT_B, APP_NAME, C_WHITE);
    cv_text(12, 40, &FONT_XS, "FM-1 CUSTOM FIRMWARE", C_GRAY);
    if (plat_ms() < ui.msg_until) {
        x = cv_text(12, 90, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 90, &FONT_B, ui.msg[1], C_HI);
    } else if (note >= 0) {
        copy_text(t, N[note % 12], sizeof t);
        num_text(note / 12 - 1, o);
        copy_text(t + (t[1] == '#' ? 2 : 1), o, sizeof t - 2u);
        cv_text(12, 90, &FONT_B, t, C_HI);
    }
    cv_blit(0, 0);
    cv_begin(240, 120, C_BLACK);
    num_text(set.octave, o);
    cv_text(12, 80, &FONT_XS, "OCT", C_GRAY);
    cv_text(42, 80, &FONT_XS, o, C_HI);
    num_text(set.depth, o);
    cv_text(90, 80, &FONT_XS, "DEPTH", C_GRAY);
    cv_text(132, 80, &FONT_XS, o, C_HI);
    num_text((int32_t)(plat_master() * 100u / 4096u), o);
    cv_text(170, 80, &FONT_XS, "VOL", C_GRAY);
    cv_text(200, 80, &FONT_XS, o, C_HI);
    cv_blit(0, 120);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_pct;
    static int32_t last_note = -2;
    uint32_t pct = plat_master() * 100u / 4096u;
    input();
    ann_tick(plat_ms());
    if (pct != last_pct || voice_note != last_note || (ui.msg_until && plat_ms() >= ui.msg_until)) {
        last_pct = pct;
        last_note = voice_note;
        if (plat_ms() >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "depth"))
        *got = set.depth;
    else if (!strcmp(what, "note"))
        *got = voice_note;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else
        return 0;
    return 1;
}
#endif
