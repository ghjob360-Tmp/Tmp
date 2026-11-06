/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4's panel. Every screen is a picture that moves with the sound, drawn in the four knobs' colours:
 * K1 blue, K2 green, K3 white, K4 orange. What a knob turns is the thing of its colour on the screen.
 *
 *   EDIT  SOUND  (SELECT: the engine)        SEQ   TAPE   (PLAY play / stop, REC record, GLO+PLAY reverse)
 *   ENV   ENVELOPE                           HOME  MIXER  (again: the master effects)
 *   LFO   LFO    (SELECT: the shape)         SEL / PRESET  the track (T1..T4)
 *   FX    EFFECT (SELECT: the type)          ALGORITHM     the tempo
 *   ARP   metronome   SAVE   the project      GLO held      fine steps, the shift functions (shown at the bottom)
 *   GLO + REC held: the track's tape erased   OCT- / OCT+   the octave
 * Part of the unity build after the platform: gfx.c (cv_*, fonts) and announce.c are in scope. */
#include <stddef.h>
#include "fastmath.h"
#include "engine.h"

/* ---------------------------------------------------------------- colours --- */
#define K_BLUE RGB(70, 145, 255)
#define K_GREEN RGB(46, 204, 113)
#define K_WHITE RGB(236, 236, 236)
#define K_ORANGE RGB(255, 118, 46)
#define REDC RGB(255, 56, 72)
#define BG RGB(8, 8, 10)
#define G1 RGB(34, 34, 40)
#define G2 RGB(70, 70, 78)
#define G3 RGB(140, 140, 150)
static const uint16_t KC[4] = {K_BLUE, K_GREEN, K_WHITE, K_ORANGE};
static const uint16_t KD[4] = {RGB(22, 44, 82), RGB(16, 64, 38), RGB(64, 64, 68), RGB(84, 38, 16)};

static float frac1(float x) { return x - fm_floorf(x); }

static uint16_t mix565(uint16_t a, uint16_t b, float t)   /* a -> b */
{
    int r = (int)((float)(a >> 11) + ((float)(b >> 11) - (float)(a >> 11)) * t),
        g = (int)((float)((a >> 5) & 63) + ((float)((b >> 5) & 63) - (float)((a >> 5) & 63)) * t),
        bl = (int)((float)(a & 31) + ((float)(b & 31) - (float)(a & 31)) * t);
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* ------------------------------------------------------------- the project --- */
#define PROJ_MAGIC 0x54425834u           /* "TBX4" */
#define TAPE_MAGIC 0x54415045u
typedef struct {
    uint32_t magic;
    proj_t p;
    int8_t octave;
    uint8_t rsv[3];
} proj_rec_t;
typedef struct {
    uint32_t magic;
    tape_t t;
} tape_rec_t;
static proj_rec_t pr;
static tape_rec_t tr;
#define P (pr.p)
static uint8_t dirty;


static void track_defaults(track_t *t, int engine)
{
    static const uint16_t ENG0[ENG_N - 1][4] = {{300, 700, 300, 150}, {500, 500, 420, 450}, {440, 500, 200, 650},
                                                {450, 300, 520, 400}};
    int i, k;
    t->engine = (uint16_t)engine;
    for (i = 0; i < ENG_N - 1; i++)
        for (k = 0; k < 4; k++)
            t->eng[i][k] = ENG0[i][k];
    for (i = 0; i < NPADS; i++) {
        t->pad[i][0] = 420;
        t->pad[i][1] = i == 3 || i == 11 ? 650 : 380;
        t->pad[i][2] = 500;
        t->pad[i][3] = i == 0 ? 1000 : 780;
    }
    t->env[0] = 40;
    t->env[1] = 450;
    t->env[2] = 650;
    t->env[3] = 380;
    t->lfo_shape = LFO_SINE;
    t->lfo_target = TGT_K3;
    t->lfo[0] = 350;
    t->lfo[1] = 0;
    t->lfo[2] = 0;
    t->fx_type = FX_OFF;
    for (i = 0; i < FX_N - 1; i++) {
        t->fx[i][0] = 500;
        t->fx[i][1] = 600;
        t->fx[i][2] = 0;
        t->fx[i][3] = 800;
    }
    t->level = 820;
    t->pan = 512;
    t->mute = 0;
}

static void tape_add(int t, int tick, int note, int len, int vel)
{
    tape_t *tp = &tr.t;
    uint32_t on = (uint32_t)tick | (uint32_t)note << 16 | 1u << 23 | (uint32_t)vel << 24, off;
    int i, n = tp->n[t];
    off = (uint32_t)((tick + len) % (int)(P.loop_bars * 4u * PPQ)) | (uint32_t)note << 16;
    for (int k = 0; k < (len > 0 ? 2 : 1); k++) {
        uint32_t e = k ? off : on;
        if (n >= TAPE_MAX)
            return;
        for (i = n; i > 0 && (tp->ev[t][i - 1] & 0xFFFFu) > (e & 0xFFFFu); i--)
            tp->ev[t][i] = tp->ev[t][i - 1];
        tp->ev[t][i] = e;
        n++;
    }
    tp->n[t] = (uint16_t)n;
}

static void project_defaults(void)
{
    static const int8_t BASS[8] = {36, 36, 48, 36, 39, 36, 43, 41};
    int i, b;
    uint32_t w;
    for (w = 0; w < sizeof pr / 4u; w++)
        ((uint32_t *)(void *)&pr)[w] = 0;
    pr.magic = PROJ_MAGIC;
    track_defaults(&P.t[0], ENG_DRUM);
    track_defaults(&P.t[1], ENG_PULSE);
    P.t[1].eng[ENG_PULSE][2] = 330;
    P.t[1].env[1] = 380;
    P.t[1].env[2] = 400;
    P.t[1].env[3] = 150;
    track_defaults(&P.t[2], ENG_STRING);
    P.t[2].fx_type = FX_FILTER;
    P.t[2].fx[FX_FILTER - 1][0] = 640;
    P.t[2].fx[FX_FILTER - 1][2] = 120;
    track_defaults(&P.t[3], ENG_FM);
    P.t[3].lfo_target = TGT_K2;
    P.t[3].lfo[1] = 300;
    P.t[3].env[2] = 250;
    P.t[3].env[3] = 520;
    P.tempo = 112;
    P.loop_bars = 2;
    P.speed = 767;                       /* +1x */
    P.metro = 0;
    P.rv_amt = 260;
    P.rv_size = 500;
    P.dl_amt = 180;
    P.dl_fb = 420;
    P.dl_div = DIV_D8;
    /* a demo on the tape: a beat, a bass line, a bell (T3 is left for you) */
    tr.magic = TAPE_MAGIC;
    for (i = 0; i < NTRACKS; i++)
        tr.t.n[i] = 0;
    for (b = 0; b < 2; b++) {
        int bar = b * 4 * PPQ;
        tape_add(0, bar, 60, 0, 120);
        tape_add(0, bar + 2 * PPQ, 60, 0, 110);
        tape_add(0, bar + 2 * PPQ + PPQ / 2 + PPQ / 4, 60, 0, 80);
        tape_add(0, bar + PPQ, 61, 0, 110);
        tape_add(0, bar + 3 * PPQ, 61, 0, 115);
        for (i = 0; i < 8; i++)
            tape_add(0, bar + i * PPQ / 2, 62, 0, i & 1 ? 70 : 95);
        for (i = 0; i < 8; i++)
            tape_add(1, bar + i * PPQ / 2, BASS[i], PPQ / 3, 100);
    }
    tape_add(3, 4 * PPQ, 72, PPQ, 90);
    tape_add(3, 4 * PPQ + 3 * PPQ / 2, 79, PPQ, 80);
}

/* ------------------------------------------------------------------- sound --- */
static int cur_track;
static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st >= 0x80u && st < 0xF0u)
            PLAT_FAR(eng_midi)(st, d1, d2, cur_track);
    }
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    midi_in();
    PLAT_FAR(eng_render)(lr, n, (float)master_q12 / 4096.0f * 8300000.0f);
}
void app_panic(void) { eng_panic(); }

static int cur_track_reset(void);
void app_init(void)
{
    cur_track_reset();                   /* a power-on starts from T1 and the SOUND screen */
    if (plat_store_load(OBJ_SETTINGS, &pr, sizeof pr) != (int)sizeof pr || pr.magic != PROJ_MAGIC
        || plat_store_load(OBJ_TAPE, &tr, sizeof tr) != (int)sizeof tr || tr.magic != TAPE_MAGIC)
        project_defaults();
    eng_init(&tr.t);
    eng_project(&P);
}

/* ---------------------------------------------------------------- the UI --- */
enum { S_SOUND, S_ENV, S_LFO, S_FX, S_TAPE, S_MIX, NSCREENS };
static const char *const SCREEN_NAME[NSCREENS] = {"SOUND", "ENVELOPE", "LFO", "EFFECT", "TAPE", "MIXER"};
static const char *const ENG_NAME[ENG_N] = {"STRING", "PULSE", "FM", "CLUSTER", "DRUMS"};
static const char *const PAD_NAME[NPADS] = {"KCK", "SNR", "CHH", "OHH", "CLP", "TML", "TMH", "RIM", "COW",
                                            "ZAP", "BLP", "MTL"};
static const char *const LFO_NAME[LFO_SHAPES] = {"SINE", "TRIANGLE", "SQUARE", "RANDOM"};
static const char *const TGT_NAME[TGT_N] = {"BLUE", "GREEN", "WHITE", "ORANGE", "PITCH", "VOLUME"};
static const char *const FX_NAME[FX_N] = {"NO EFFECT", "DRIVE", "CRUSH", "FILTER"};
static const char *const DIV_NAME[DIV_N] = {"1/8", "3/16", "1/4"};

static struct {
    int screen, mix_master, octave;
    uint32_t keys, btn, now;
    uint32_t turned_at[4];
    char toast[2][24];
    uint32_t toast_until;
    uint32_t top_at, rec_t;
    uint8_t rec_armed, playing, bottom_dirty, top_dirty, glo, erase_done;
    int sel_pad[NTRACKS];
    uint32_t hit_seen[NTRACKS][NPADS], hit_at[NTRACKS][NPADS];
    uint32_t key_flash[NKEYS];
} ui;

static int cur_track_reset(void)
{
    uint8_t *u = (uint8_t *)(void *)&ui;
    uint32_t i;
    for (i = 0; i < sizeof ui; i++)
        u[i] = 0;
    cur_track = 0;
    return 0;
}

static void copy_text(char *d, const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1u < max && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
static void text_cat(char *d, const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (d[n])
        n++;
    copy_text(d + n, s, max - n);
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

void app_say(const char *a, const char *b)
{
    copy_text(ui.toast[0], a, sizeof ui.toast[0]);
    copy_text(ui.toast[1], b, sizeof ui.toast[1]);
    ui.toast_until = plat_ms() + 1100u;
    ui.top_dirty = 1;
    if (ann_on()) {
        char t[48];
        copy_text(t, a, sizeof t);
        text_cat(t, " ", sizeof t);
        text_cat(t, b, sizeof t);
        ann_say(t, 0, ANN_NOW);
    }
}

/* the four knobs of a screen: what they turn, how it reads */
enum { V_PCT, V_TIME_A, V_TIME_D, V_TIME_R, V_RATIO, V_HZ, V_TGT, V_SPEED, V_BARS, V_BPM, V_ONOFF, V_PAN };
typedef struct {
    const char *label;
    uint16_t *v;
    uint8_t fmt;
    int16_t lo, hi, step;
} dial_t;

static void dials(dial_t d[4])
{
    track_t *t = &P.t[cur_track];
    int i;
    for (i = 0; i < 4; i++) {
        d[i].label = 0;
        d[i].v = 0;
        d[i].fmt = V_PCT;
        d[i].lo = 0;
        d[i].hi = 1023;
        d[i].step = 16;
    }
    switch (ui.screen) {
    case S_SOUND:
        if (t->engine == ENG_DRUM) {
            static const char *const L[4] = {"PITCH", "DECAY", "TONE", "LEVEL"};
            for (i = 0; i < 4; i++) {
                d[i].label = L[i];
                d[i].v = &t->pad[ui.sel_pad[cur_track]][i];
            }
        } else {
            static const char *const L[4][4] = {{"DAMP", "TONE", "PLUCK", "DRIVE"}, {"WIDTH", "SUB", "CUTOFF", "RESO"},
                                                {"RATIO", "INDEX", "FEEDBK", "ENV"}, {"SPREAD", "SHAPE", "CUTOFF", "DRIFT"}};
            for (i = 0; i < 4; i++) {
                d[i].label = L[t->engine][i];
                d[i].v = &t->eng[t->engine][i];
            }
            if (t->engine == ENG_FM)
                d[0].fmt = V_RATIO;
        }
        break;
    case S_ENV: {
        static const char *const L[4] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
        static const uint8_t F[4] = {V_TIME_A, V_TIME_D, V_PCT, V_TIME_R};
        for (i = 0; i < 4; i++) {
            d[i].label = L[i];
            d[i].v = &t->env[i];
            d[i].fmt = F[i];
        }
        break;
    }
    case S_LFO:
        d[0].label = "RATE"; d[0].v = &t->lfo[0]; d[0].fmt = V_HZ;
        d[1].label = "DEPTH"; d[1].v = &t->lfo[1];
        d[2].label = "FADE IN"; d[2].v = &t->lfo[2];
        d[3].label = "TARGET"; d[3].v = &t->lfo_target; d[3].fmt = V_TGT; d[3].hi = TGT_N - 1; d[3].step = 1;
        break;
    case S_FX:
        if (t->fx_type != FX_OFF) {
            static const char *const L[3][4] = {{"DRIVE", "TONE", "BIAS", "MIX"}, {"BITS", "RATE", "FOLD", "MIX"},
                                                {"CUTOFF", "RESO", "SWEEP", "MIX"}};
            for (i = 0; i < 4; i++) {
                d[i].label = L[t->fx_type - 1][i];
                d[i].v = &t->fx[t->fx_type - 1][i];
            }
        }
        break;
    case S_TAPE:
        d[0].label = "SPEED"; d[0].v = &P.speed; d[0].fmt = V_SPEED; d[0].step = 8;
        d[1].label = "BARS"; d[1].v = &P.loop_bars; d[1].fmt = V_BARS; d[1].lo = 1; d[1].hi = 8; d[1].step = 1;
        d[2].label = "TEMPO"; d[2].v = &P.tempo; d[2].fmt = V_BPM; d[2].lo = 40; d[2].hi = 240; d[2].step = 1;
        d[3].label = "MUTE"; d[3].v = &t->mute; d[3].fmt = V_ONOFF; d[3].hi = 1; d[3].step = 1;
        break;
    case S_MIX:
        if (ui.mix_master) {
            static const char *const L[4] = {"REVERB", "SIZE", "DELAY", "FEEDBK"};
            uint16_t *v[4] = {&P.rv_amt, &P.rv_size, &P.dl_amt, &P.dl_fb};
            for (i = 0; i < 4; i++) {
                d[i].label = L[i];
                d[i].v = v[i];
            }
        } else {
            static const char *const L[4] = {"T1", "T2", "T3", "T4"};
            for (i = 0; i < 4; i++) {
                d[i].label = L[i];
                d[i].v = ui.glo ? &P.t[i].pan : &P.t[i].level;
                d[i].fmt = ui.glo ? V_PAN : V_PCT;
            }
        }
        break;
    default:
        break;
    }
}

static void value_text(const dial_t *d, char *t)
{
    int32_t v = *d->v;
    char n[12];
    float x = (float)v / 1023.0f;
    t[0] = 0;
    switch (d->fmt) {
    case V_TIME_A: case V_TIME_D: case V_TIME_R: {
        float s = d->fmt == V_TIME_A ? 0.001f * fm_powf(2000.0f, x)
                : d->fmt == V_TIME_D ? 0.01f * fm_powf(300.0f, x) : 0.005f * fm_powf(800.0f, x);
        if (s < 1.0f) {
            num_text((int32_t)(s * 1000.0f), t);
            text_cat(t, "ms", 12);
        } else {
            num_text((int32_t)s, t);
            text_cat(t, ".", 12);
            num_text((int32_t)(s * 10.0f) % 10, n);
            text_cat(t, n, 12);
            text_cat(t, "s", 12);
        }
        break;
    }
    case V_RATIO: {
        static const char *const R[8] = {"x0.5", "x1", "x1.5", "x2", "x3", "x4", "x5", "x7"};
        copy_text(t, R[(int)(x * 7.99f)], 12);
        break;
    }
    case V_HZ: {
        float hz = 0.05f * fm_exp2f(x * 9.0f);
        num_text((int32_t)hz, t);
        text_cat(t, ".", 12);
        num_text((int32_t)(hz * 10.0f) % 10, n);
        text_cat(t, n, 12);
        text_cat(t, "Hz", 12);
        break;
    }
    case V_TGT: copy_text(t, TGT_NAME[v % TGT_N], 12); break;
    case V_SPEED: {
        float s = (x * 1023.0f - 512.0f) * (2.0f / 511.0f);
        int32_t c = (int32_t)(s * 100.0f + (s < 0.0f ? -0.5f : 0.5f));
        if (c > -4 && c < 4) {
            copy_text(t, "STOP", 12);
            break;
        }
        copy_text(t, c < 0 ? "-" : "+", 12);
        num_text((c < 0 ? -c : c) / 100, n);
        text_cat(t, n, 12);
        text_cat(t, ".", 12);
        num_text((c < 0 ? -c : c) / 10 % 10, n);
        text_cat(t, n, 12);
        text_cat(t, "x", 12);
        break;
    }
    case V_BARS: num_text(v, t); text_cat(t, v == 1 ? " BAR" : " BARS", 12); break;
    case V_BPM: num_text(v, t); break;
    case V_ONOFF: copy_text(t, v ? "MUTED" : "ON", 12); break;
    case V_PAN:
        if (v > 500 && v < 524) copy_text(t, "C", 12);
        else { copy_text(t, v < 512 ? "L" : "R", 12); num_text((v < 512 ? 512 - v : v - 512) * 100 / 511, n); text_cat(t, n, 12); }
        break;
    default: num_text(v * 100 / 1023, t); break;
    }
}

static void changed(void)
{
    dirty = 1;
    ui.bottom_dirty = 1;
    ui.top_dirty = 1;
    eng_project(&P);
}

static void go(int s)
{
    if (s == S_MIX && ui.screen == S_MIX)
        ui.mix_master ^= 1;
    else if (s == S_MIX)
        ui.mix_master = 0;
    ui.screen = s;
    ui.bottom_dirty = ui.top_dirty = 1;
    if (ann_on())
        ann_say(s == S_MIX && ui.mix_master ? "MASTER" : SCREEN_NAME[s], 0, ANN_NOW);
}

static void track_select(int t)
{
    char n[4] = {'T', (char)('1' + t), 0, 0};
    cur_track = t;
    ui.bottom_dirty = ui.top_dirty = 1;
    app_say(n, ENG_NAME[P.t[t].engine % ENG_N]);
}

/* --------------------------------------------------------------- input --- */
static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int i, glo = (int)((btn >> B_GLO) & 1u);
    int32_t e;
    dial_t d[4];
    track_t *t = &P.t[cur_track];
    touch_poll();
    keys = touch_keys();
    if (glo != ui.glo) {                 /* shift: the bottom shows the shift functions */
        ui.glo = (uint8_t)glo;
        ui.bottom_dirty = 1;
    }
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int note = t->engine == ENG_DRUM ? 60 + (int)(k % NPADS) : 53 + (int)k + 12 * ui.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            v = v ? v : 100u;
            eng_key(cur_track, note, (int)v);
            plat_midi_send(((uint32_t)(0x09u | 0x90u << 8) + ((uint32_t)cur_track << 8)) | (uint32_t)note << 16 | v << 24,
                           TO_USB);
            ui.key_flash[k] = now;
        } else if (!(keys & bit) && (ui.keys & bit)) {
            eng_key(cur_track, note, 0);
            plat_midi_send(((uint32_t)(0x08u | 0x80u << 8) + ((uint32_t)cur_track << 8)) | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_EDIT: go(S_SOUND); break;
        case B_ENV: go(S_ENV); break;
        case B_LFO: go(S_LFO); break;
        case B_FX: go(S_FX); break;
        case B_SEQ: go(S_TAPE); break;
        case B_HOME: go(S_MIX); break;
        case B_SEL: track_select((cur_track + 1) % NTRACKS); break;
        case B_PLAY:
            if (glo) {                   /* the tape turned around */
                P.speed = (uint16_t)(1024 - P.speed > 1023 ? 1023 : 1024 - P.speed);
                changed();
                app_say("TAPE", (int)P.speed < 512 ? "BACKWARDS" : "FORWARDS");
            } else {
                ui.playing ^= 1u;
                eng_transport(ui.playing, ui.rec_armed);
                ui.top_dirty = 1;
            }
            break;
        case B_REC:
            ui.rec_t = now;
            ui.erase_done = 0;
            if (!glo) {
                ui.rec_armed ^= 1u;
                eng_transport(ui.playing, ui.rec_armed);
                app_say(ui.rec_armed ? "RECORD" : "RECORD", ui.rec_armed ? "ARMED" : "OFF");
            }
            break;
        case B_ARP:
            P.metro ^= 1u;
            changed();
            app_say("METRONOME", P.metro ? "ON" : "OFF");
            break;
        case B_SAVE:
            if (plat_store_save(OBJ_SETTINGS, &pr, sizeof pr) == 0 && plat_store_save(OBJ_TAPE, &tr, sizeof tr) == 0) {
                dirty = 0;
                app_say("SAVED", "PROJECT");
            } else {
                app_say("SAVE", "FAILED");
            }
            ui.bottom_dirty = 1;
            break;
        case B_OCTDN: case B_OCTUP: {
            int o = clamp(ui.octave + (k == B_OCTUP ? 1 : -1), -2, 2);
            char n[8];
            if (o != ui.octave) {
                ui.octave = o;
                num_text(o, n);
                app_say("OCTAVE", n);
            }
            break;
        }
        default: break;
        }
    }
    if (glo && ((btn >> B_REC) & 1u) && !ui.erase_done && now - ui.rec_t > 700u) {   /* GLO + REC held: erase */
        ui.erase_done = 1;
        eng_clear_track(cur_track);
        app_say("ERASED", "TRACK");
        dirty = 1;
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        track_select((cur_track + NTRACKS + (e > 0 ? 1 : -1)) % NTRACKS);
    if ((e = plat_enc(EN_ALGO)) != 0) {  /* the tempo, anywhere */
        char n[8];
        P.tempo = (uint16_t)clamp((int32_t)P.tempo + e, 40, 240);
        changed();
        num_text(P.tempo, n);
        app_say("TEMPO", n);
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {   /* the screen's type */
        int dir = e > 0 ? 1 : -1;
        switch (ui.screen) {
        case S_SOUND:
            t->engine = (uint16_t)((t->engine + ENG_N + (uint16_t)dir) % ENG_N);
            eng_panic();
            app_say("SOUND", ENG_NAME[t->engine]);
            break;
        case S_LFO:
            t->lfo_shape = (uint16_t)((t->lfo_shape + LFO_SHAPES + (uint16_t)dir) % LFO_SHAPES);
            app_say("LFO", LFO_NAME[t->lfo_shape]);
            break;
        case S_FX:
            t->fx_type = (uint16_t)((t->fx_type + FX_N + (uint16_t)dir) % FX_N);
            app_say("EFFECT", FX_NAME[t->fx_type]);
            break;
        case S_TAPE: {
            static const uint16_t BARS[4] = {1, 2, 4, 8};
            int b = 0;
            while (b < 3 && BARS[b] < P.loop_bars)
                b++;
            P.loop_bars = BARS[clamp(b + dir, 0, 3)];
            break;
        }
        case S_MIX:
            P.dl_div = (uint16_t)((P.dl_div + DIV_N + (uint16_t)dir) % DIV_N);
            app_say("DELAY", DIV_NAME[P.dl_div]);
            break;
        default: break;
        }
        changed();
    }
    dials(d);
    for (i = 0; i < 4; i++) {
        int32_t v;
        if ((e = plat_enc(EN_K1 + i)) == 0 || !d[i].v)
            continue;
        if (d[i].fmt == V_BARS) {
            static const uint16_t BARS[4] = {1, 2, 4, 8};
            int b = 0;
            while (b < 3 && BARS[b] < *d[i].v)
                b++;
            *d[i].v = BARS[clamp(b + (e > 0 ? 1 : -1), 0, 3)];
        } else {
            v = clamp((int32_t)*d[i].v + e * (glo && d[i].step > 1 ? 2 : d[i].step), d[i].lo, d[i].hi);
            *d[i].v = (uint16_t)v;
        }
        ui.turned_at[i] = now;
        changed();
    }
    if (keys != ui.keys)
        ui.top_dirty = 1;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void) { input(); }

/* ---------------------------------------------------------------- drawing --- */
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y;
    for (y = -r; y <= r; y++) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - y * y));
        cv_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}
static void ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int i;
    int32_t px = cx, py = cy - r;
    for (i = 1; i <= 48; i++) {
        float a = (float)i / 48.0f;
        int32_t x = cx + (int32_t)(fm_sin_turns(a) * (float)r), y = cy - (int32_t)(fm_sin_turns(a + 0.25f) * (float)r);
        cv_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
static void arc(int32_t cx, int32_t cy, float r, float a0, float a1, uint16_t c, int thick)
{                                         /* turns, 0 at the top, clockwise */
    int n = (int)((a1 - a0) * 64.0f) + 1, i, k;
    for (k = 0; k < thick; k++) {
        float rr = r - (float)k;
        int32_t px = cx + (int32_t)(fm_sin_turns(a0) * rr), py = cy - (int32_t)(fm_sin_turns(a0 + 0.25f) * rr);
        for (i = 1; i <= n; i++) {
            float a = a0 + (a1 - a0) * (float)i / (float)n;
            int32_t x = cx + (int32_t)(fm_sin_turns(a) * rr), y = cy - (int32_t)(fm_sin_turns(a + 0.25f) * rr);
            cv_line(px, py, x, y, c);
            px = x;
            py = y;
        }
    }
}
static void tline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)   /* two pixels thick */
{
    cv_line(x0, y0, x1, y1, c);
    cv_line(x0, y0 + 1, x1, y1 + 1, c);
}
static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static float kv(int i)                   /* the current screen's knob i, 0..1 */
{
    dial_t d[4];
    dials(d);
    return d[i].v ? (float)(*d[i].v - d[i].lo) / (float)(d[i].hi - d[i].lo) : 0.0f;
}
static int lit(int i) { return ui.turned_at[i] && ui.now - ui.turned_at[i] < 700u; }
static uint16_t kcol(int i) { return lit(i) ? KC[i] : mix565(KC[i], BG, 0.25f); }

/* ---- SOUND pictures, y 20..142 ---- */
static void pic_string(float tm)
{
    float act = fm_minf(VIEW.track_peak[cur_track] * 40.0f, 1.0f), damp = kv(0), tone = kv(1), pluck = kv(2),
          drive = kv(3);
    int32_t x0 = 24, x1 = 210, y = 82, i, px = x0, py = y, pu = x0 + (int32_t)((0.05f + 0.45f * pluck) * 186.0f);
    for (i = 1; i <= 64; i++) {          /* the string, standing waves */
        float u = (float)i / 64.0f, a = act * 26.0f;
        float yy = a * fm_sin_turns(u * 0.5f) * fm_sin_turns(tm * 5.3f)
                 + a * tone * 0.45f * fm_sin_turns(u) * fm_sin_turns(tm * 10.9f + 0.3f);
        int32_t x = x0 + (int32_t)(u * (float)(x1 - x0)), yi = y + (int32_t)yy;
        if (drive > 0.05f && (i & 1))
            yi += (int32_t)(drive * 4.0f * act * (float)((i & 2) - 1));
        tline(px, py, x, yi, drive > 0.05f ? mix565(K_WHITE, K_ORANGE, drive) : K_WHITE);
        px = x;
        py = yi;
    }
    cv_rect(14, y - 14, 10, 30, G2);     /* the bridge, the nut */
    cv_rect(x1, y - 14, 10, 30, G2);
    {                                    /* DAMP: the felt at the right end */
        int32_t w = 6 + (int32_t)(damp * 26.0f), h = 8 + (int32_t)(damp * 18.0f);
        cv_rect(x1 - w, y - h / 2 - 2, w, h, kcol(0));
    }
    for (i = 0; i < 2 + (int)(tone * 7.0f); i++) {   /* TONE: sparks at the pluck */
        float a = (float)i / (2.0f + tone * 7.0f) * 0.5f - 0.25f, l = 8.0f + 10.0f * tone * (0.6f + 0.4f * fm_sin_turns(tm * 3.0f + (float)i));
        cv_line(pu, y - 8, pu + (int32_t)(fm_sin_turns(a) * l), y - 8 - (int32_t)(fm_sin_turns(a + 0.25f) * l), kcol(1));
    }
    for (i = 0; i < 9; i++)              /* PLUCK: the pick under the string */
        cv_rect(pu - i / 2, y + 12 + i, i + 1, 1, kcol(2));
    if (drive > 0.05f)                   /* DRIVE: heat under the string */
        for (i = 0; i < 6; i++)
            cv_rect(40 + i * 28, 126 - (int32_t)(drive * 10.0f * (0.5f + 0.5f * fm_sin_turns(tm * 2.0f + (float)i * 0.3f))),
                    16, 3, kcol(3));
}

static void pic_pulse(float tm)
{
    float pw = 0.5f + 0.45f * kv(0), sub = kv(1), cut = kv(2), res = kv(3);
    int32_t i, px = 0, py = 0, x;
    float scroll = VIEW.playing || VIEW.track_peak[cur_track] > 0.001f ? tm * 0.6f : 0.0f;
    for (i = 0; i <= 200; i++) {         /* WIDTH: the pulse, its high part blue */
        float ph = frac1((float)i / 66.0f + scroll);
        int32_t y = ph < pw ? 30 : 64;
        x = 20 + i;
        if (i && y != py)
            cv_line(x, 30, x, 64, K_WHITE);
        cv_rect(x, y, 1, 2, y == 30 ? kcol(0) : K_WHITE);
        px = x;
        py = y;
    }
    for (i = 0; i <= 200; i++) {         /* SUB: an octave down, green */
        float ph = frac1((float)i / 132.0f + scroll * 0.5f);
        cv_rect(20 + i, ph < 0.5f ? 74 - (int32_t)(sub * 6.0f) : 80 + (int32_t)(sub * 6.0f), 1, 2, kcol(1));
    }
    (void)px;
    {                                    /* CUTOFF and RESO: the filter's curve */
        int32_t xc = 20 + (int32_t)(cut * 200.0f), lx = 20, ly = 112;
        for (i = 0; i <= 200; i += 4) {
            float d = ((float)(20 + i) - (float)xc) / 30.0f, y;
            y = d < 0.0f ? 0.0f : d * d * 6.0f;
            y -= res * 18.0f * fm_expf(-d * d * 4.0f);
            if (y > 28.0f)
                y = 28.0f;
            x = 20 + i;
            tline(lx, ly, x, 112 + (int32_t)y, i < (int32_t)(cut * 200.0f) + 20 ? kcol(2) : mix565(K_WHITE, BG, 0.6f));
            lx = x;
            ly = 112 + (int32_t)y;
        }
        disc(xc, 112 - (int32_t)(res * 18.0f), 3 + (int32_t)(res * 3.0f), kcol(3));
    }
}

static void pic_fm(float tm)
{
    float ratio_k = kv(0), idx = kv(1), fb = kv(2), envk = kv(3), env = VIEW.env[cur_track];
    static const float R[8] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f};
    float r = R[(int)(ratio_k * 7.99f)], orbit = 18.0f + 40.0f * idx, ang = tm * (0.12f + 0.1f * r);
    int32_t cx = 120, cy = 82, mx, my, i;
    ring(cx, cy, (int32_t)orbit, kcol(1));                         /* INDEX: the orbit */
    for (i = 1; i <= (int)(fb * 14.0f); i++) {                    /* FEEDBACK: the trail */
        float a = ang - (float)i * 0.018f;
        disc(cx + (int32_t)(fm_sin_turns(a) * orbit), cy - (int32_t)(fm_sin_turns(a + 0.25f) * orbit), 2,
             mix565(kcol(2), BG, (float)i / 16.0f));
    }
    for (i = 1; i <= 10; i++) {                                    /* ENV: a comet tail while a note sounds */
        float a = ang - (float)i * 0.03f, bright = envk * env * (1.0f - (float)i / 11.0f);
        if (bright > 0.05f)
            disc(cx + (int32_t)(fm_sin_turns(a) * (orbit + 4.0f)), cy - (int32_t)(fm_sin_turns(a + 0.25f) * (orbit + 4.0f)), 2,
                 mix565(BG, K_ORANGE, bright));
    }
    disc(cx, cy, 12 + (int32_t)(env * 5.0f), K_WHITE);           /* the carrier */
    mx = cx + (int32_t)(fm_sin_turns(ang) * orbit);
    my = cy - (int32_t)(fm_sin_turns(ang + 0.25f) * orbit);
    disc(mx, my, 7, kcol(0));                                      /* RATIO: the modulator, its speed */
    {
        static const char *const RN[8] = {"1/2", "1", "3/2", "2", "3", "4", "5", "7"};
        cv_text(mx + 10, my - 6, &FONT_T, RN[(int)(ratio_k * 7.99f)], kcol(0));
    }
}

static void pic_cluster(float tm)
{
    float spread = kv(0), shape = kv(1), cut = kv(2), drift = kv(3), act = fm_minf(VIEW.track_peak[cur_track] * 30.0f, 1.0f);
    int32_t c, p;
    disc(120, 82, 14 + (int32_t)(cut * 46.0f), mix565(BG, K_WHITE, 0.08f + 0.06f * cut));   /* CUTOFF: the haze */
    for (c = 0; c < 5; c++)
        for (p = 0; p < 7; p++) {
            float sx = (float)(c - 2) * (8.0f + 30.0f * spread), ph = tm * (0.3f + 0.05f * (float)c) + (float)p / 7.0f,
                  jx = drift * 6.0f * fm_sin_turns(tm * 1.7f + (float)(c * 7 + p) * 0.37f),
                  jy = drift * 6.0f * fm_sin_turns(tm * 1.3f + (float)(c * 5 + p) * 0.29f);
            int32_t x = 120 + (int32_t)(sx + jx), y = 82 + (int32_t)(fm_sin_turns(ph) * (18.0f + 22.0f * act) + jy);
            uint16_t col = mix565(kcol(0), K_WHITE, (float)p / 10.0f);
            if (shape > 0.5f)                                      /* SHAPE: round -> square */
                cv_rect(x - 2, y - 2, 5, 5, mix565(col, kcol(1), shape));
            else
                disc(x, y, 2, mix565(col, kcol(1), shape));
            if (drift > 0.3f && p == 0)                             /* DRIFT: orange strays */
                disc(x + (int32_t)jx * 2, y + (int32_t)jy * 2, 1, kcol(3));
        }
}

static void pic_drums(void)
{
    int i;
    int sel = ui.sel_pad[cur_track];
    for (i = 0; i < NPADS; i++) {        /* the pads, 4 x 3, lit when hit */
        int32_t x = 8 + (i % 4) * 37, y = 22 + (i / 4) * 40;
        float since = (float)(ui.now - ui.hit_at[cur_track][i]) * 0.001f, glow = since < 0.4f ? 1.0f - since / 0.4f : 0.0f;
        uint16_t fill = mix565(G1, K_WHITE, glow * 0.85f);
        int32_t bump = (int32_t)(glow * 3.0f);
        cv_rect(x - bump, y - bump, 33 + 2 * bump, 34 + 2 * bump, fill);
        if (i == sel) {
            cv_rect(x - 2 - bump, y - 2 - bump, 37 + 2 * bump, 2, K_ORANGE);
            cv_rect(x - 2 - bump, y + 34 + bump, 37 + 2 * bump, 2, K_ORANGE);
        }
        ctext(x + 16, y + 11, &FONT_T, PAD_NAME[i], glow > 0.5f ? BG : i == sel ? K_WHITE : G3);
    }
    {                                    /* the chosen pad: its sound as a picture */
        const uint16_t *p = P.t[cur_track].pad[sel];
        float pitch = (float)p[0] / 1023.0f, dec = (float)p[1] / 1023.0f, tone = (float)p[2] / 1023.0f,
              lvl = (float)p[3] / 1023.0f;
        int32_t x0 = 160, i2, lx = x0, ly = 0;
        ctext(198, 22, &FONT_B, PAD_NAME[sel], K_WHITE);
        cv_rect(160, 44, 4, 70, G1);                               /* PITCH: a blue column */
        cv_rect(160, 44 + (int32_t)((1.0f - pitch) * 70.0f), 4, (int32_t)(pitch * 70.0f), kcol(0));
        for (i2 = 0; i2 <= 60; i2 += 2) {                          /* DECAY: the green fall, LEVEL its height */
            float t = (float)i2 / 60.0f, a = lvl * fm_expf(-t * 6.0f / (0.15f + dec)) * fm_sin_turns(t * (4.0f + pitch * 10.0f));
            int32_t y = 80 - (int32_t)(a * 30.0f);
            if (i2)
                cv_line(lx, ly, x0 + 10 + i2, y, kcol(1));
            lx = x0 + 10 + i2;
            ly = y;
        }
        for (i2 = 0; i2 < (int)(tone * 24.0f); i2++)               /* TONE: white grit */
            cv_rect(172 + (int32_t)((i2 * 37) % 56), 118 + (int32_t)((i2 * 13) % 10), 2, 2, kcol(2));
        cv_rect(232, 44 + (int32_t)((1.0f - lvl) * 70.0f), 4, (int32_t)(lvl * 70.0f), kcol(3));   /* LEVEL */
    }
}

/* ---- ENVELOPE: four segments in the four colours, the voice's level running along it ---- */
static void pic_env(void)
{
    const track_t *t = &P.t[cur_track];
    float a = (float)t->env[0] / 1023.0f, d = (float)t->env[1] / 1023.0f, s = (float)t->env[2] / 1023.0f,
          r = (float)t->env[3] / 1023.0f, lv = VIEW.env[cur_track];
    int32_t x0 = 14, yb = 134, yt = 30, xa = x0 + 6 + (int32_t)(a * 52.0f), xd = xa + 6 + (int32_t)(d * 52.0f),
            ys = yb - (int32_t)(s * (float)(yb - yt)), xs = xd + 40, xr = xs + 6 + (int32_t)(r * 54.0f), i;
    if (t->engine == ENG_DRUM) {
        ctext(120, 64, &FONT_B, "DRUMS", G3);
        ctext(120, 88, &FONT_XS, "each pad has its own decay", G2);
        return;
    }
    for (i = x0; i < xr; i += 3) {       /* the area under, faint */
        int32_t y = i < xa ? yb - (yb - yt) * (i - x0) / fm_maxf((float)(xa - x0), 1.0f)
                  : i < xd ? yt + (ys - yt) * (i - xa) / fm_maxf((float)(xd - xa), 1.0f)
                  : i < xs ? ys
                           : ys + (yb - ys) * (i - xs) / fm_maxf((float)(xr - xs), 1.0f);
        cv_rect(i, y, 1, yb - y, G1);
    }
    tline(x0, yb, xa, yt, kcol(0));
    tline(xa, yt, xd, ys, kcol(1));
    tline(xd, ys, xs, ys, kcol(2));
    tline(xs, ys, xr, yb, kcol(3));
    disc(xa, yt, 3, kcol(0));
    disc(xd, ys, 3, kcol(1));
    disc(xs, ys, 3, kcol(2));
    disc(xr, yb, 3, kcol(3));
    if (lv > 0.002f) {                   /* the newest voice's level, now */
        int32_t y = yb - (int32_t)(lv * (float)(yb - yt));
        cv_rect(x0, y, 226 - x0, 1, mix565(BG, K_WHITE, 0.5f));
        disc(226, y, 4, K_WHITE);
    }
}

/* ---- LFO: the wave in its target's colour, a dot riding it ---- */
static void pic_lfo(void)
{
    const track_t *t = &P.t[cur_track];
    int tg = t->lfo_target % TGT_N;
    uint16_t col = tg < 4 ? KC[tg] : tg == TGT_PITCH ? RGB(200, 120, 255) : G3;
    float depth = (float)t->lfo[1] / 1023.0f, fade = (float)t->lfo[2] / 1023.0f, ph = VIEW.lfo_ph[cur_track];
    int32_t i, lx = 14, ly = 82, x, y, w = 168;
    uint32_t seed = 7u;
    float held = 0.2f;
    for (i = 0; i <= w; i += 2) {        /* two cycles */
        float u = (float)i / (float)w * 2.0f, p = frac1(u), v, f = fade > 0.01f ? fm_minf(u / (fade * 2.0f), 1.0f) : 1.0f;
        if ((int)(u * 4.0f) != (int)((u - 2.0f / (float)w) * 4.0f)) {
            seed = seed * 1103515245u + 12345u;
            held = (float)((seed >> 16) & 255u) / 127.5f - 1.0f;
        }
        v = t->lfo_shape == LFO_TRI ? 1.0f - 4.0f * fm_fabsf(p - 0.5f) : t->lfo_shape == LFO_SQUARE ? (p < 0.5f ? 1.0f : -1.0f)
          : t->lfo_shape == LFO_RANDOM ? held : fm_sin_turns(p);
        x = 14 + i;
        y = 82 - (int32_t)(v * f * (6.0f + 44.0f * depth));
        if (i)
            tline(lx, ly, x, y, mix565(col, BG, 0.15f));
        lx = x;
        ly = y;
    }
    if (fade > 0.01f)                    /* FADE IN: a white ramp under the start */
        cv_line(14, 136, 14 + (int32_t)(fade * 2.0f * (float)w / 2.0f), 128, kcol(2));
    {                                    /* the dot: where the LFO is now */
        float v = VIEW.lfo[cur_track];
        disc(14 + (int32_t)(ph * (float)w / 2.0f), 82 - (int32_t)(v * 50.0f), 5, K_WHITE);
    }
    {                                    /* TARGET: a little knob of its colour, an arrow to it */
        cv_line(190, 82, 204, 82, col);
        cv_line(200, 78, 204, 82, col);
        cv_line(200, 86, 204, 82, col);
        disc(218, 82, 12, col);
        disc(218, 82, 5, BG);
        ctext(218, 100, &FONT_T, TGT_NAME[tg], col);
        ctext(60, 24, &FONT_T, LFO_NAME[t->lfo_shape % LFO_SHAPES], G3);
    }
}

/* ---- EFFECT ---- */
static void pic_fx(void)
{
    const track_t *t = &P.t[cur_track];
    int32_t i, lx = 20, ly = 82, x, y;
    if (t->fx_type == FX_OFF) {
        cv_rect(20, 82, 200, 2, G2);
        ctext(120, 96, &FONT_XS, "SELECT: drive, crush, filter", G2);
        return;
    }
    {
        const uint16_t *f = t->fx[t->fx_type - 1];
        float a = (float)f[0] / 1023.0f, b = (float)f[1] / 1023.0f, c = (float)f[2] / 1023.0f, mix = (float)f[3] / 1023.0f;
        for (i = 0; i <= 180; i += 2) {
            float u = (float)i / 180.0f, s = fm_sin_turns(u * 2.0f), o = s;
            if (t->fx_type == FX_DRIVE) {
                float g = 1.0f + 24.0f * a * a;
                o = fm_tanhf(s * g + 0.4f * c) - fm_tanhf(0.4f * c);
            } else if (t->fx_type == FX_CRUSH) {
                float q = fm_exp2f(1.0f + (1.0f - a) * 5.0f), step = 1.0f + b * 20.0f, uu = fm_floorf(u * 180.0f / step) * step / 180.0f;
                o = fm_floorf(fm_sin_turns(uu * 2.0f) * q + 0.5f) / q;
                o = o + (fm_sin_turns(0.25f * o * (1.0f + 4.0f * c)) - o) * c;
            } else {
                o = s * (1.0f / (1.0f + fm_expf((u - a) * 14.0f)));   /* a picture of a sweep */
            }
            o = s + (o - s) * mix;
            x = 20 + i;
            cv_rect(x, 82 - (int32_t)(s * 42.0f), 1, 1, G2);       /* the dry wave, faint */
            y = 82 - (int32_t)(fm_clampf(o, -1.1f, 1.1f) * 42.0f);
            if (i)
                tline(lx, ly, x, y, K_WHITE);
            lx = x;
            ly = y;
        }
        if (t->fx_type == FX_DRIVE) {    /* DRIVE: the walls the wave hits; BIAS: their shift */
            int32_t wall = 40 - (int32_t)(a * 30.0f), off = (int32_t)(c * 10.0f);
            cv_rect(20, 82 - wall - off, 180, 1, kcol(0));
            cv_rect(20, 82 + wall - off, 180, 1, kcol(0));
            cv_rect(20, 136, (int32_t)(b * 180.0f), 3, kcol(1));     /* TONE */
            cv_rect(20, 26, (int32_t)(c * 180.0f), 2, kcol(2));
        } else if (t->fx_type == FX_CRUSH) {
            for (i = 0; i < 5; i++)
                cv_rect(20 + i * 36, 132, 4 + (int32_t)(b * 26.0f), 4, kcol(1));   /* RATE: the hold width */
            cv_rect(20, 26, (int32_t)(a * 180.0f), 2, kcol(0));                  /* BITS */
            cv_rect(20, 30, (int32_t)(c * 180.0f), 2, kcol(2));                  /* FOLD */
        } else {
            int32_t xc = 20 + (int32_t)(a * 180.0f);
            cv_rect(xc, 30, 2, 104, kcol(0));                                     /* CUTOFF */
            disc(xc, 40 - (int32_t)(b * 8.0f), 3 + (int32_t)(b * 4.0f), kcol(1));   /* RESO */
            if (c > 0.01f)                                                         /* SWEEP */
                for (i = -1; i <= 1; i += 2)
                    cv_line(xc + i * 8, 120, xc + i * (8 + (int32_t)(c * 20.0f)), 120, kcol(2));
        }
        cv_rect(210, 134 - (int32_t)(mix * 100.0f), 10, (int32_t)(mix * 100.0f), kcol(3));   /* MIX */
        cv_rect(210, 34, 10, 1, G2);
    }
}

/* ---- TAPE: two reels, the tape between them, four lanes scrolling under the head ---- */
static void reel(int32_t cx, int32_t cy, float pack, float ang)
{
    int i;
    disc(cx, cy, 14 + (int32_t)(pack * 18.0f), RGB(52, 40, 34));   /* the tape wound on it */
    ring(cx, cy, 33, G2);
    disc(cx, cy, 7, G3);
    for (i = 0; i < 3; i++) {            /* three spokes, turning */
        float a = ang + (float)i / 3.0f;
        tline(cx + (int32_t)(fm_sin_turns(a) * 8.0f), cy - (int32_t)(fm_sin_turns(a + 0.25f) * 8.0f),
              cx + (int32_t)(fm_sin_turns(a) * 13.0f), cy - (int32_t)(fm_sin_turns(a + 0.25f) * 13.0f), K_WHITE);
    }
}

static void pic_tape(void)
{
    float len = (float)(P.loop_bars * 4u * PPQ), pos = VIEW.tape_pos, u = len > 0.0f ? pos / len : 0.0f;
    int t, i;
    reel(58, 52, 1.0f - u, -VIEW.reel * 1.3f);
    reel(182, 52, u, -VIEW.reel);
    cv_line(58 - 30, 64, 112, 92, G3);   /* the tape's path past the head */
    cv_line(182 + 30, 64, 128, 92, G3);
    cv_rect(112, 90, 16, 6, VIEW.recording ? REDC : G3);
    for (t = 0; t < NTRACKS; t++) {      /* the lanes: a loop's worth, the head in the middle */
        int32_t y = 102 + t * 10, n = tr.t.n[t];
        int here = t == cur_track;
        cv_rect(10, y, 220, 8, here ? (ui.rec_armed ? RGB(60, 16, 22) : G1) : BG);
        for (i = 0; i < (int)P.loop_bars; i++) {   /* bar lines */
            float bu = frac1((float)(i * 4 * PPQ) / len - u + 0.5f) - 0.5f;
            cv_rect(120 + (int32_t)(bu * 220.0f), y, 1, 8, G2);
        }
        for (i = 0; i < n && i < TAPE_MAX; i++) {
            uint32_t e = tr.t.ev[t][i];
            if (e >> 23 & 1u) {
                float eu = frac1((float)(e & 0xFFFFu) / len - u + 0.5f) - 0.5f;
                int32_t x = 120 + (int32_t)(eu * 220.0f), ny = y + 6 - (int32_t)(((e >> 16) & 0x7Fu) % 12u) / 2;
                cv_rect(x - 1, ny, 3, 2, here ? KC[t] : mix565(KC[t], BG, 0.5f));
            }
        }
    }
    cv_rect(120, 98, 1, 44, VIEW.recording ? REDC : K_WHITE);   /* the head */
}

/* ---- MIXER: four faders in the four colours, meters beside them; the master's spring and echoes ---- */
static void pic_mix(float tm)
{
    int i;
    if (!ui.mix_master) {
        for (i = 0; i < NTRACKS; i++) {
            const track_t *t = &P.t[i];
            int32_t x = 22 + i * 54, top = 30, h = 96, lv = (int32_t)((float)t->level / 1023.0f * (float)h),
                    mtr = (int32_t)(fm_minf(VIEW.track_peak[i] * 3.0f, 1.0f) * (float)h);
            cv_rect(x, top, 22, h, G1);
            cv_rect(x, top + h - lv, 22, lv, t->mute ? G2 : kcol(i));
            cv_rect(x + 26, top + h - mtr, 4, mtr, mtr > h * 9 / 10 ? REDC : K_WHITE);
            disc(x + 11 + (int32_t)(((float)t->pan - 512.0f) / 512.0f * 11.0f), top - 6, 3, ui.glo ? KC[i] : G2);
            ctext(x + 11, top + h + 4, &FONT_T, ENG_NAME[t->engine % ENG_N], i == cur_track ? K_WHITE : G3);
            if (t->mute)
                ctext(x + 11, top + h / 2 - 6, &FONT_T, "M", BG);
        }
    } else {
        float rv = (float)P.rv_amt / 1023.0f, sz = (float)P.rv_size / 1023.0f, dl = (float)P.dl_amt / 1023.0f,
              fb = (float)P.dl_fb / 1023.0f, wob = fm_minf(VIEW.out_peak * 3.0f, 1.0f);
        int32_t lx = 14, ly = 82, x, y;
        for (i = 0; i <= 96; i++) {      /* the spring: SIZE its coils, REVERB its brightness */
            float u = (float)i / 96.0f;
            x = 14 + i;
            y = 82 + (int32_t)(fm_sin_turns(u * (3.0f + sz * 9.0f) + tm * wob) * (10.0f + 8.0f * rv));
            if (i)
                tline(lx, ly, x, y, mix565(kcol(1), kcol(0), rv));
            lx = x;
            ly = y;
        }
        {                                 /* the echoes: DELAY their light, FEEDBACK how many */
            int n = 1 + (int)(fb * 7.0f), k;
            float beat = frac1((float)VIEW.tape_pos / (float)PPQ);
            for (k = 0; k < n; k++) {
                float g = dl * fm_powf(0.82f, (float)k) * (k == 0 ? 1.0f : fb + 0.2f);
                int32_t r = 5 + (int32_t)(g * 10.0f) + (k == 0 && VIEW.playing ? (int32_t)((1.0f - beat) * 4.0f) : 0);
                disc(136 + k * 14, 82, r, mix565(BG, k == 0 ? kcol(2) : kcol(3), fm_minf(g * 1.6f + 0.35f, 1.0f)));
            }
            ctext(176, 112, &FONT_T, DIV_NAME[P.dl_div % DIV_N], G3);
        }
        ctext(62, 112, &FONT_T, "SPRING", G3);
    }
}

static void draw_top(void)
{
    float tm = (float)ui.now * 0.001f;
    int i;
    char n[12];
    cv_begin(240, 144, BG);
    {                                    /* the header: the screen, the tracks, the tape */
        const char *name = ui.screen == S_MIX && ui.mix_master ? "MASTER"
                         : ui.screen == S_SOUND ? ENG_NAME[P.t[cur_track].engine % ENG_N] : SCREEN_NAME[ui.screen];
        cv_text(8, 3, &FONT_XS, name, K_WHITE);
        for (i = 0; i < NTRACKS; i++) {
            int32_t x = 110 + i * 18;
            float act = fm_minf(VIEW.track_peak[i] * 30.0f, 1.0f);
            n[0] = (char)('1' + i);
            n[1] = 0;
            if (i == cur_track)
                cv_rect(x - 1, 3, 14, 13, K_WHITE);
            else
                cv_rect(x - 1, 3, 14, 13, mix565(G1, KC[i], act * 0.7f));
            ctext(x + 6, 3, &FONT_T, n, i == cur_track ? BG : G3);
        }
        if (VIEW.playing) {              /* play: a triangle; record: a red dot, blinking while it records */
            for (i = 0; i < 9; i++)
                cv_rect(196 + i / 2, 5 + i / 2, 1, 9 - (i / 2) * 2, RGB(70, 220, 120));
        } else {
            cv_rect(196, 5, 8, 9, G2);
        }
        if (ui.rec_armed && (!VIEW.recording || (ui.now / 250u) & 1u))
            disc(212, 9, 4, REDC);
        num_text(P.tempo, n);
        cv_text(234 - text_w(&FONT_T, n), 4, &FONT_T, n, G3);
    }
    switch (ui.screen) {
    case S_SOUND:
        switch (P.t[cur_track].engine) {
        case ENG_STRING: pic_string(tm); break;
        case ENG_PULSE: pic_pulse(tm); break;
        case ENG_FM: pic_fm(tm); break;
        case ENG_CLUSTER: pic_cluster(tm); break;
        default: pic_drums(); break;
        }
        break;
    case S_ENV: pic_env(); break;
    case S_LFO: pic_lfo(); break;
    case S_FX: pic_fx(); break;
    case S_TAPE: pic_tape(); break;
    default: pic_mix(tm); break;
    }
    if (ui.now < ui.toast_until) {       /* a toast: a small pill under the header, the picture stays in view */
        int32_t w = text_w(&FONT_XS, ui.toast[0]) + (ui.toast[1][0] ? 8 + text_w(&FONT_XS, ui.toast[1]) : 0) + 16, x,
                left = (int32_t)(ui.toast_until - ui.now);
        int32_t y = left > 1000 ? 20 - (int32_t)(left - 1000) / 10 : 20;     /* it drops in */
        cv_rect(120 - w / 2, y, w, 17, G1);
        cv_rect(120 - w / 2, y + 16, w, 1, K_ORANGE);
        x = cv_text(120 - w / 2 + 8, y + 1, &FONT_XS, ui.toast[0], K_WHITE);
        cv_text(x + 8, y + 1, &FONT_XS, ui.toast[1], K_ORANGE);
    }
    cv_blit(0, 0);
}

static void draw_bottom(void)
{
    dial_t d[4];
    int i;
    char t[16];
    dials(d);
    cv_begin(240, 96, BG);
    cv_rect(0, 0, 240, 1, G1);
    for (i = 0; i < 4; i++) {            /* four knobs, their colours, their values */
        int32_t cx = 30 + i * 60, cy = 30;
        float v;
        if (!d[i].v) {
            disc(cx, cy, 3, G1);
            continue;
        }
        v = (float)(*d[i].v - d[i].lo) / (float)(d[i].hi - d[i].lo);
        arc(cx, cy, 20.0f, -0.375f, 0.375f, KD[i], 3);
        arc(cx, cy, 20.0f, -0.375f, -0.375f + 0.75f * v, KC[i], lit(i) ? 5 : 3);
        tline(cx, cy, cx + (int32_t)(fm_sin_turns(-0.375f + 0.75f * v) * 12.0f),
              cy - (int32_t)(fm_sin_turns(-0.375f + 0.75f * v + 0.25f) * 12.0f), KC[i]);
        disc(cx, cy, 3, KC[i]);
        if (!ui.glo) {
            ctext(cx, 56, &FONT_T, d[i].label, lit(i) ? K_WHITE : G3);
            value_text(&d[i], t);
            ctext(cx, 72, &FONT_XS, t, KC[i]);
        } else {
            value_text(&d[i], t);
            ctext(cx, 56, &FONT_T, t, KC[i]);
        }
    }
    if (ui.glo) {                        /* shift: what GLO does here */
        const char *h = ui.screen == S_MIX && !ui.mix_master ? "GLO: knobs pan the tracks"
                      : ui.screen == S_TAPE ? "GLO+PLAY reverse  GLO+REC hold: erase"
                                            : "GLO: fine steps  GLO+PLAY reverse";
        ctext(120, 76, &FONT_T, h, K_WHITE);
    }
    if (dirty)
        disc(234, 90, 2, K_ORANGE);      /* unsaved */
    cv_blit(0, 144);
}

/* the keys' and buttons' lights */
static void lights(void)
{
    static const uint8_t SB[NSCREENS] = {B_EDIT, B_ENV, B_LFO, B_FX, B_SEQ, B_HOME};
    uint32_t b = ui.btn | 1u << SB[ui.screen], k = ui.keys, i;
    if (ui.playing)
        b |= 1u << B_PLAY;
    if (ui.rec_armed && (!VIEW.recording || (ui.now / 250u) & 1u))
        b |= 1u << B_REC;
    if (P.metro)
        b |= 1u << B_ARP;
    if (dirty)
        b |= 1u << B_SAVE;
    if (ui.screen == S_TAPE && VIEW.playing) {   /* the 16 white keys: where the loop is */
        static const uint8_t WK[16] = PLAT_WHITE_KEYS;
        float len = (float)(P.loop_bars * 4u * PPQ);
        k |= 1u << WK[(int)(VIEW.tape_pos / len * 16.0f) & 15];
    } else if (P.t[cur_track].engine == ENG_DRUM) {
        for (i = 0; i < NKEYS; i++)       /* the pads: lit a moment after each hit */
            if (ui.now - ui.hit_at[cur_track][i % NPADS] < 120u)
                k |= 1u << i;
    }
    plat_glow(1);
    plat_leds(b, k);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, BG);
    ui.top_dirty = ui.bottom_dirty = 1;
}

void app_frame(void)
{
    int t, p, alive;
    ui.now = plat_ms();
    input();
    ann_tick(ui.now);
    for (t = 0; t < NTRACKS; t++)        /* hits: when, and which pad the knobs now edit */
        for (p = 0; p < NPADS; p++)
            if (VIEW.pad_hits[t][p] != ui.hit_seen[t][p]) {
                ui.hit_seen[t][p] = VIEW.pad_hits[t][p];
                ui.hit_at[t][p] = ui.now;
                if (ui.sel_pad[t] != p) {
                    ui.sel_pad[t] = p;
                    if (t == cur_track)
                        ui.bottom_dirty = 1;
                }
            }
    alive = VIEW.playing || VIEW.out_peak > 0.002f || ui.now < ui.toast_until + 100u;
    for (t = 0; t < 4; t++)
        if (lit(t) || (ui.turned_at[t] && ui.now - ui.turned_at[t] < 800u))
            alive = 1, ui.bottom_dirty |= ui.now - ui.turned_at[t] > 700u && ui.now - ui.turned_at[t] < 800u;
    VIEW.out_peak *= 0.8f;
    /* the top moves at ~15 frames a second while anything does, ~4 when idle (it still breathes) */
    if (ui.top_dirty || ui.now - ui.top_at >= (alive ? 66u : 250u)) {
        ui.top_at = ui.now;
        ui.top_dirty = 0;
        draw_top();
    }
    if (ui.bottom_dirty) {
        ui.bottom_dirty = 0;
        draw_bottom();
    }
    lights();
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "screen")) *got = ui.screen;
    else if (!strcmp(what, "track")) *got = cur_track;
    else if (!strcmp(what, "engine")) *got = P.t[cur_track].engine;
    else if (!strcmp(what, "playing")) *got = VIEW.playing;
    else if (!strcmp(what, "recording")) *got = VIEW.recording;
    else if (!strcmp(what, "events")) *got = tr.t.n[cur_track];
    else if (!strcmp(what, "tempo")) *got = P.tempo;
    else if (!strcmp(what, "beat")) *got = (int)VIEW.beat;
    else if (!strcmp(what, "speed_neg")) *got = P.speed < 512;
    else if (!strcmp(what, "dirty")) *got = dirty;
    else if (!strcmp(what, "loud")) *got = VIEW.out_peak > 0.01f;
    else if (!strcmp(what, "pad")) *got = ui.sel_pad[cur_track];
    else return 0;
    return 1;
}
#endif
