/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the screen's building blocks (included by app.c, after the platform's gfx.c).
 * The screen: a header (y 0..19) and a picture (y 20..143) in one 240 x 144 canvas, the four knobs below it
 * (y 144..239, a 240 x 96 canvas). Every picture draws its parameters in their knobs' colours:
 * K1 blue, K2 green, K3 white, K4 orange; the GLO row the same colours, dimmer. */

#define K_BLUE RGB(70, 145, 255)
#define K_GREEN RGB(46, 204, 113)
#define K_WHITE RGB(236, 236, 236)
#define K_ORANGE RGB(255, 118, 46)
#define REDC RGB(255, 56, 72)
#define BG RGB(8, 8, 10)
#define G1 RGB(30, 30, 36)
#define G2 RGB(66, 66, 74)
#define G3 RGB(140, 140, 150)
#define MODE_C0 RGB(70, 145, 255)        /* the modes' colours: synth, drum, tape, mixer */
#define MODE_C1 RGB(46, 204, 113)
#define MODE_C2 RGB(255, 118, 46)
#define MODE_C3 RGB(170, 170, 180)
static const uint16_t KC[4] = {K_BLUE, K_GREEN, K_WHITE, K_ORANGE};
static const uint16_t KD[4] = {RGB(22, 44, 82), RGB(16, 64, 38), RGB(64, 64, 68), RGB(84, 38, 16)};
static const uint16_t MODEC[4] = {MODE_C0, MODE_C1, MODE_C2, MODE_C3};

#define PIC_Y 20                         /* the picture: x 0..239, y 20..143 */
#define PIC_H 124
#define PIC_CY 82

static uint16_t mix565(uint16_t a, uint16_t b, float t)   /* a -> b by t */
{
    int r, g, bl;
    if (t <= 0.0f)
        return a;
    if (t >= 1.0f)
        return b;
    r = (int)((float)(a >> 11) + ((float)(b >> 11) - (float)(a >> 11)) * t);
    g = (int)((float)((a >> 5) & 63) + ((float)((b >> 5) & 63) - (float)((a >> 5) & 63)) * t);
    bl = (int)((float)(a & 31) + ((float)(b & 31) - (float)(a & 31)) * t);
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* the knobs: recently turned ones are lit (their picture parts at full colour) */
static uint32_t ui_now;                  /* ms */
static float ui_t;                       /* seconds, the pictures' clock */
static uint32_t knob_at[4];
static int ui_shift;                     /* GLO held: pictures may show the second row */
static int lit(int k) { return knob_at[k & 3] && ui_now - knob_at[k & 3] < 700u; }
static uint16_t kcol(int k) { return lit(k) ? KC[k & 3] : mix565(KC[k & 3], BG, 0.22f); }
static uint16_t kdim(int k) { return mix565(KC[k & 3], BG, 0.6f); }

/* ---------------------------------------------------------------- shapes --- */
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y;
    if (r < 1) {
        cv_rect(cx, cy, 1, 1, c);
        return;
    }
    for (y = -r; y <= r; y++) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - y * y));
        cv_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}
static void ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int i, n = r < 12 ? 24 : 48;
    int32_t px = cx, py = cy - r;
    for (i = 1; i <= n; i++) {
        float a = (float)i / (float)n;
        int32_t x = cx + (int32_t)(fm_sin_turns(a) * (float)r), y = cy - (int32_t)(fm_sin_turns(a + 0.25f) * (float)r);
        cv_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
/* an arc, turns (0 at the top, clockwise), thick rings inward */
static void arc(int32_t cx, int32_t cy, float r, float a0, float a1, uint16_t c, int thick)
{
    int n = (int)((a1 - a0) * 64.0f) + 1, i, k;
    if (a1 <= a0)
        return;
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
static void tline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)   /* 2 px */
{
    cv_line(x0, y0, x1, y1, c);
    cv_line(x0, y0 + 1, x1, y1 + 1, c);
}
static void frame_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 1, c);
    cv_rect(x, y + h - 1, w, 1, c);
    cv_rect(x, y, 1, h, c);
    cv_rect(x + w - 1, y, 1, h, c);
}
static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void rtext(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(rx - text_w(f, s), y, f, s, c);
}
/* a polyline through n values (-1..1) across [x0, x0 + w], centred on cy, +-h */
static void wave_line(int32_t x0, int32_t w, int32_t cy, int32_t h, const float *v, int n, uint16_t c, int thick)
{
    int i;
    int32_t px = x0, py = cy - (int32_t)(v[0] * (float)h);
    for (i = 1; i < n; i++) {
        int32_t x = x0 + i * w / (n - 1), y = cy - (int32_t)(clampf(v[i], -1.2f, 1.2f) * (float)h);
        if (thick)
            tline(px, py, x, y, c);
        else
            cv_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}
/* a point turned by a (turns) around (cx, cy) */
static void rot(float x, float y, float a, int32_t cx, int32_t cy, int32_t *ox, int32_t *oy)
{
    float s = fm_sin_turns(a), c = fm_sin_turns(a + 0.25f);
    *ox = cx + (int32_t)(x * c - y * s);
    *oy = cy + (int32_t)(x * s + y * c);
}

/* -------------------------------------------------------------- text bits --- */
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
    while (d[n] && n < max)
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
static void dec1_text(float v, char *b)  /* one decimal */
{
    char n[12];
    int32_t x = (int32_t)(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f));
    if (x < 0) {
        *b++ = '-';
        x = -x;
    }
    num_text(x / 10, b);
    text_cat(b, ".", 16);
    num_text(x % 10, n);
    text_cat(b, n, 16);
}
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_text(int n, char *b)
{
    char o[8];
    copy_text(b, NOTE_NAME[(n % 12 + 12) % 12], 8);
    num_text(n / 12 - 1, o);
    text_cat(b, o, 8);
}

/* --------------------------------------------------------------- the knobs --- */
/* one knob of the bottom row: its colour ring, its value arc, its label and its value text */
static void dial(int i, float v, const char *label, const char *value, int alt)
{
    int32_t cx = 30 + i * 60, cy = 30;
    uint16_t col = alt ? kdim(i) : KC[i];
    if (!label) {
        disc(cx, cy, 3, G1);
        return;
    }
    v = clampf(v, 0.0f, 1.0f);
    arc(cx, cy, 20.0f, -0.375f, 0.375f, KD[i], 3);
    arc(cx, cy, 20.0f, -0.375f, -0.375f + 0.75f * v, col, lit(i) ? 5 : 3);
    tline(cx, cy, cx + (int32_t)(fm_sin_turns(-0.375f + 0.75f * v) * 12.0f),
          cy - (int32_t)(fm_sin_turns(-0.375f + 0.75f * v + 0.25f) * 12.0f), col);
    disc(cx, cy, 3, col);
    ctext(cx, 56, &FONT_T, label, lit(i) ? K_WHITE : alt ? G2 : G3);
    if (value)
        ctext(cx, 72, &FONT_XS, value, col);
}
