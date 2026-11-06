/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1's sound engine (an fm1-base o2 unit: -O2, its own compilation unit).
 *
 *   keys / MIDI / ARP -> OSC -> FILTER -> AMP (EG, tremolo) -> MOD -> DELAY -> REVERB -> out
 *
 * One voice. Everything here runs in the audio interrupt except dsp_patch / dsp_key / dsp_arp, which the main loop
 * calls: they only write a patch slot, an event ring and two flags that the interrupt picks up at its next block.
 * Control-rate work (LFOs' targets, the filter's coefficients, the effects' gains) is done every SUB frames. */
#include "plat.h"
/* fastmath.h's helpers are static inline; forced inline here, so none stays behind as a call into the flash */
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#undef inline
#include "dsp.h"

#define SR 44100.0f
#define ISR (1.0f / SR)
#define SUB 16u                                   /* control-rate period, frames */
/* the audio path runs from RAM, out of the XIP cache's way (plat.h PLAT_RAM_CODE: the build checks that it calls
 * nothing in the flash); small helpers are forced inline so no out-of-line copy lands in the flash */
#define RAMC PLAT_RAM_CODE
#define INL static inline __attribute__((always_inline))

INL uint32_t umod(uint32_t a, uint32_t b)          /* a % b for small a / b: no divide routine */
{
    while (a >= b)
        a -= b;
    return a;
}

INL float kn(uint16_t v) { return (float)(v > 1023u ? 1023u : v) * (1.0f / 1023.0f); }
INL float bip(uint16_t v) { return fm_clampf(((float)v - 512.0f) * (1.0f / 511.0f), -1.0f, 1.0f); }
INL float frac1(float x) { return x - fm_floorf(x); }

static uint32_t rng = 0x12345678u;
INL uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ------------------------------------------------------- main loop -> audio --- */
_Static_assert(sizeof(patch_t) % 4u == 0u, "patch_t is copied by words");
static patch_t slot[2];
static volatile uint32_t slot_idx, slot_seq;
static patch_t P;                                  /* the interrupt's copy */
static uint32_t p_seq = 0xFFFFFFFFu;

void dsp_patch(const patch_t *p)
{
    uint32_t n = slot_idx ^ 1u;
    slot[n] = *p;
    slot_idx = n;
    slot_seq++;
}

#define EVN 64u
static volatile uint16_t ev[EVN];
static volatile uint32_t ev_w, ev_r;
void dsp_key(int note, int on)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVN)
        return;                                    /* full: dropped (64 keys in 5.8 ms) */
    ev[w % EVN] = (uint16_t)((on ? 0x100u : 0u) | ((uint32_t)note & 0x7Fu));
    ev_w = w + 1u;
}

static volatile uint8_t arp_on_req, arp_latch_req, panic_req;
void dsp_arp(int on, int latch)
{
    arp_on_req = (uint8_t)(on != 0);
    arp_latch_req = (uint8_t)(latch != 0);
}
void dsp_panic(void) { panic_req = 1; }

/* ------------------------------------------------------------------ voice --- */
enum { EGS_IDLE, EGS_ATT, EGS_DEC, EGS_SUS, EGS_HOLD, EGS_REL };
static struct {
    int note;                                      /* the note playing (-1 none yet) */
    int gate;
    float freq_note;                               /* the note as a float, for the pitch LFO */
    float ph, sph, subph, modph, mprev;            /* oscillator phases (turns) */
    float lfo_ph, sweep_ph, trem_ph;
    float env;
    int egs;
    float att_inc, rel_coef, shape_now, cut_now;
    float ic1[2], ic2[2];                          /* the filter's two stages */
    float g, k, a1, a2, a3;
} V;

static int held[16], nheld;                        /* keys held, in the order pressed (the last on top) */
static float riser;                                /* REVERB RISER: the swell since the last note */

static RAMC void voice_trigger(int note)
{
    V.note = note;
    V.freq_note = (float)note;
    V.gate = 1;
    V.egs = EGS_ATT;
    V.sweep_ph = 0.0f;
    riser = 0.0f;
}
static RAMC void voice_release(void)
{
    V.gate = 0;
    if (V.egs != EGS_IDLE && P.eg_type != EG_AR && P.eg_type != EG_OPEN)
        V.egs = EGS_REL;
}

static RAMC void held_remove(int note)
{
    int i, j;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note)
            held[j++] = held[i];
    nheld = j;
}

static uint8_t arp_active;
static RAMC void key_event(int note, int on)
{
    held_remove(note);
    if (on) {
        if (nheld == 16)
            held_remove(held[0]);
        held[nheld++] = note;
        if (!arp_active)
            voice_trigger(note);
    } else if (!arp_active) {
        if (nheld)
            V.note = held[nheld - 1], V.freq_note = (float)V.note;   /* back to the last key still held */
        else
            voice_release();
    }
}
void dsp_midi_note(int note, int vel) { PLAT_FAR(key_event)(note & 0x7F, vel > 0); }

/* ---------------------------------------------------------------- the arp --- */
static const int8_t CHORD[CHORD_N][4] = {{0, 12, -1, -1}, {0, 4, 7, -1}, {0, 5, 7, -1},
                                         {0, 4, 8, -1},   {0, 3, 7, -1}, {0, 3, 6, -1}};
static struct {
    int root;                                      /* latched root (-1 none) */
    uint32_t pos, steps;
    float t, step_len;                             /* frames into the step, frames per step */
    int gate_off_done;
} A = {-1, 0, 0, 0.0f, 0.0f, 1};

INL int conv_index(uint32_t i, uint32_t n) { return (int)((i & 1u) ? n - 1u - i / 2u : i / 2u); }

static RAMC int arp_pick(uint32_t k, int *notes, int n)
{
    uint32_t N = (uint32_t)n, i;
    int idx;
    switch (P.arp_type) {
    default:
    case ARP_UP: idx = (int)umod(k, N); break;
    case ARP_DOWN: idx = (int)(N - 1u - umod(k, N)); break;
    case ARP_UPDOWN:
    case ARP_DOWNUP:
        if (N < 2u) {
            idx = 0;
        } else {
            uint32_t per = 2u * N - 2u;
            i = umod(k, per);
            idx = (int)(i < N ? i : per - i);
        }
        if (P.arp_type == ARP_DOWNUP)
            idx = n - 1 - idx;
        break;
    case ARP_CONV: idx = conv_index(umod(k, N), N); break;
    case ARP_DIV: idx = conv_index(N - 1u - umod(k, N), N); break;
    case ARP_CONVDIV:
        i = umod(k, 2u * N);
        idx = i < N ? conv_index(i, N) : conv_index(2u * N - 1u - i, N);
        break;
    case ARP_DIVCONV:
        i = umod(k, 2u * N);
        idx = i < N ? conv_index(N - 1u - i, N) : conv_index(i - N, N);
        break;
    case ARP_RANDOM: idx = (int)(((rnd() >> 16) * N) >> 16); break;
    case ARP_STOCH:                                /* up, with some notes swapped or lifted an octave */
        idx = (int)umod(k, N);
        if ((rnd() & 3u) == 0u)
            idx = (int)(((rnd() >> 16) * N) >> 16);
        return notes[idx] + ((rnd() >> 24) < 52u ? 12 : 0);   /* 1 in 5: an octave up */
    }
    return notes[idx];
}

static RAMC void arp_frames(uint32_t frames)
{
    int on = arp_on_req, latch = arp_latch_req, notes[12], n = 0, o, c, root;
    if (!latch)
        A.root = -1;
    if (nheld)
        A.root = held[nheld - 1];
    root = nheld ? held[nheld - 1] : (latch ? A.root : -1);
    if (!on || root < 0) {
        if (arp_active) {                          /* the arp let go: back to the keys */
            arp_active = 0;
            if (nheld)
                voice_trigger(held[nheld - 1]);
            else
                voice_release();
        }
        A.pos = 0;
        A.t = 0.0f;
        return;
    }
    for (o = 0; o < (P.arp_oct < 1u ? 1 : P.arp_oct > 3u ? 3 : P.arp_oct); o++)
        for (c = 0; c < 4 && CHORD[P.arp_chord % CHORD_N][c] >= 0; c++)
            if (n < 12)
                notes[n++] = root + 12 * o + CHORD[P.arp_chord % CHORD_N][c];
    A.step_len = SR * 60.0f / ((float)P.arp_tempo * 0.1f * 4.0f);   /* sixteenths */
    if (!arp_active) {                             /* starts at once */
        arp_active = 1;
        A.t = A.step_len;
        A.pos = 0;
    }
    A.t += (float)frames;
    if (A.t >= A.step_len) {
        uint32_t len = P.arp_len < 1u ? 1u : P.arp_len > 24u ? 24u : P.arp_len;
        int note;
        A.t -= A.step_len;
        if (A.t >= A.step_len)
            A.t = 0.0f;
        note = arp_pick(A.pos, notes, n);
        if (++A.pos >= len)
            A.pos = 0;
        A.steps++;
        voice_trigger(note < 0 ? 0 : note > 127 ? 127 : note);
        A.gate_off_done = 0;
    } else if (!A.gate_off_done && A.t >= A.step_len * 0.5f) {
        voice_release();                           /* a 50 % gate */
        A.gate_off_done = 1;
    }
}

/* ------------------------------------------------------------- oscillator --- */
INL float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static const float VPM_RATIO[8] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f};

INL float osc_sample(float dt, float shape, float alt)
{
    float s, sub;
    int wrapped;
    V.ph += dt;
    wrapped = V.ph >= 1.0f;
    if (wrapped)
        V.ph -= 1.0f;
    switch (P.osc_type) {
    default:
    case OSC_SAW: {                                /* SHAPE: hard sync, the slave at 1..4 x */
        float r = 1.0f + 3.0f * shape, sdt = dt * r;
        V.sph += sdt;
        if (wrapped)
            V.sph = V.ph * r;
        V.sph = frac1(V.sph);
        s = 2.0f * V.sph - 1.0f - blep(V.sph, sdt);
        break;
    }
    case OSC_TRI: {                                /* SHAPE: a sine wavefolder */
        float tri = 4.0f * fm_fabsf(V.ph - 0.5f) - 1.0f;
        s = fm_sin_turns(0.25f * tri * (1.0f + 4.0f * shape));
        break;
    }
    case OSC_SQR: {                                /* SHAPE: pulse width 50 -> 95 % */
        float pw = 0.5f + 0.45f * shape;
        s = (V.ph < pw ? 1.0f : -1.0f) + blep(V.ph, dt) - blep(frac1(V.ph + 1.0f - pw), dt);
        break;
    }
    case OSC_VPM: {                                /* SHAPE: index 0..6 rad; ALT: the modulator's ratio */
        float m;
        V.modph = frac1(V.modph + dt * VPM_RATIO[(int)(alt * 7.99f)]);
        m = fm_sin_turns(V.modph + 0.06f * shape * V.mprev);
        V.mprev = m;
        return fm_sin_turns(V.ph + 0.9549f * shape * m);   /* 6 rad / 2 pi = 0.95 turns */
    }
    }
    /* ALT on the analog types: a square an octave down */
    V.subph += dt * 0.5f;
    if (V.subph >= 1.0f)
        V.subph -= 1.0f;
    sub = (V.subph < 0.5f ? 1.0f : -1.0f) + blep(V.subph, dt * 0.5f) - blep(frac1(V.subph + 0.5f), dt * 0.5f);
    return (s + alt * 0.8f * sub) * (1.0f / (1.0f + 0.5f * alt));
}

/* --------------------------------------------------------------- envelope --- */
INL float eg_sample(void)
{
    switch (V.egs) {
    case EGS_ATT:
        V.env += V.att_inc;
        if (V.env >= 1.0f) {
            V.env = 1.0f;
            V.egs = P.eg_type == EG_ADSR ? EGS_DEC
                  : P.eg_type == EG_AHR  ? (V.gate ? EGS_HOLD : EGS_REL)
                  : P.eg_type == EG_OPEN ? EGS_HOLD
                                         : EGS_REL;   /* AR, AR LOOP */
        }
        break;
    case EGS_DEC:                                  /* ADSR: decay 0.3 s toward the sustain of 0.5 */
        V.env = 0.5f + (V.env - 0.5f) * 0.99977f;
        if (!V.gate)
            V.egs = EGS_REL;
        break;
    case EGS_HOLD:
        if (!V.gate && P.eg_type != EG_OPEN)
            V.egs = EGS_REL;
        break;
    case EGS_REL:
        V.env *= V.rel_coef;
        if (V.env < 0.0002f) {
            V.env = 0.0f;
            V.egs = (P.eg_type == EG_ARLOOP && V.gate) ? EGS_ATT : EGS_IDLE;
        }
        break;
    default:
        if (P.eg_type == EG_OPEN)                  /* OPEN: always on, whatever the keys do */
            V.egs = EGS_ATT;
        break;
    }
    return V.env;
}

/* ----------------------------------------------------------------- filter --- */
static RAMC void filter_coef(float cutoff_oct, float reso)
{
    float fc = 20.0f * fm_exp2f(fm_clampf(cutoff_oct, 0.0f, 9.95f));
    if (fc > 0.45f * SR)
        fc = 0.45f * SR;
    V.g = fm_tanf(3.14159265f * fc * ISR);
    V.k = 2.0f - 1.9f * reso;                      /* never self-oscillates */
    V.a1 = 1.0f / (1.0f + V.g * (V.g + V.k));
    V.a2 = V.g * V.a1;
    V.a3 = V.g * V.a2;
}

INL float svf(int st, float x, int mode)  /* mode 0 low, 1 band, 2 high */
{
    float v3 = x - V.ic2[st], v1 = V.a1 * V.ic1[st] + V.a2 * v3, v2 = V.ic2[st] + V.a2 * V.ic1[st] + V.a3 * v3;
    V.ic1[st] = 2.0f * v1 - V.ic1[st];
    V.ic2[st] = 2.0f * v2 - V.ic2[st];
    return mode == 0 ? v2 : mode == 1 ? v1 * 1.5f : x - V.k * v1 - v2;
}

INL float filter_sample(float x)
{
    int mode, poles;
    if (P.flt_type >= FLT_OFF)
        return x;
    mode = P.flt_type / 2;
    poles = P.flt_type & 1;
    x = fm_tanhf(x * 1.2f);
    x = svf(0, x, mode);
    if (poles)
        x = svf(1, x, mode);
    return x;
}

/* ---------------------------------------------------------------- effects --- */
#define MODLEN 2048u
static float modbuf[2][MODLEN] PLAT_POOL;
static uint32_t mod_w;
static float mod_lfo, mod_fast, mod_fb[2], ph_ap[2][6], ph_a, ph_fb[2];

INL float rd(const float *b, uint32_t len, uint32_t w, float d)   /* d frames ago, interpolated */
{
    float p = (float)w - d, f;
    int32_t i;
    while (p < 0.0f)
        p += (float)len;
    i = (int32_t)p;
    f = p - (float)i;
    return b[(uint32_t)i % len] * (1.0f - f) + b[(uint32_t)(i + 1) % len] * f;
}

INL void mod_sample(float *l, float *r, float rate, float depth)
{
    float x[2] = {*l, *r}, w[2];
    int c;
    mod_lfo = frac1(mod_lfo + rate * ISR);
    switch (P.mod_type) {
    case MOD_CHORUS:
        for (c = 0; c < 2; c++) {
            float d = (0.007f + 0.005f * depth * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.25f * (float)c))) * SR;
            modbuf[c][mod_w] = x[c];
            w[c] = rd(modbuf[c], MODLEN, mod_w, d);
            x[c] = 0.75f * x[c] + 0.65f * w[c];
        }
        break;
    case MOD_ENSEMBLE: {
        float t[3], m = (x[0] + x[1]) * 0.5f;
        mod_fast = frac1(mod_fast + 6.3f * ISR);
        modbuf[0][mod_w] = m;
        for (c = 0; c < 3; c++)
            t[c] = rd(modbuf[0], MODLEN, mod_w,
                      (0.006f + 0.003f * depth * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.3333f * (float)c))
                       + 0.0002f * fm_sin_turns(mod_fast + 0.3333f * (float)c)) * SR);
        x[0] = 0.6f * x[0] + 0.5f * (t[0] + 0.5f * t[1]);
        x[1] = 0.6f * x[1] + 0.5f * (t[2] + 0.5f * t[1]);
        break;
    }
    case MOD_FLANGER:
        for (c = 0; c < 2; c++) {
            float d = (0.0005f + 0.004f * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.25f * (float)c))) * SR;
            modbuf[c][mod_w] = x[c] + 0.75f * depth * mod_fb[c];
            w[c] = rd(modbuf[c], MODLEN, mod_w, d);
            mod_fb[c] = w[c];
            x[c] = 0.7f * (x[c] + w[c]);
        }
        break;
    case MOD_PHASER:
        for (c = 0; c < 2; c++) {
            float y = x[c] + 0.6f * depth * ph_fb[c];
            int s;
            for (s = 0; s < 6; s++) {              /* first-order allpasses, all at ph_a */
                float o = ph_a * y + ph_ap[c][s];
                ph_ap[c][s] = y - ph_a * o;
                y = o;
            }
            ph_fb[c] = y;
            x[c] = 0.7f * (x[c] + y);
        }
        break;
    default:
        break;
    }
    mod_w = (mod_w + 1u) & (MODLEN - 1u);
    *l = x[0];
    *r = x[1];
}

#define DLEN 22528u                                /* 0.51 s at 44.1 kHz */
static int16_t dbuf[2][DLEN] PLAT_POOL;
static uint32_t d_w;
static float d_time, d_hp[2], d_lp[2], d_wow, d_flut;

INL float d_rd(int c, float d)
{
    float p = (float)d_w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DLEN;
    i = (int32_t)p;
    f = p - (float)i;
    return ((float)dbuf[c][(uint32_t)i % DLEN] * (1.0f - f) + (float)dbuf[c][(uint32_t)(i + 1) % DLEN] * f)
         * (1.0f / 16384.0f);
}
INL int16_t d_q(float x)
{
    x = fm_clampf(x, -1.99f, 1.99f) * 16384.0f;
    return (int16_t)x;
}

INL void delay_sample(float *l, float *r, float t_target, float fb, float mix)
{
    float wl, wr, inl = *l, inr = *r, d;
    d_time += (t_target - d_time) * 0.0004f;       /* the time glides (a tape-like pitch bend) */
    d = d_time;
    switch (P.dly_type) {
    case DLY_MONO:
        wl = wr = d_rd(0, d);
        dbuf[0][d_w] = d_q((inl + inr) * 0.5f + fb * wl);
        break;
    case DLY_PINGPONG:
        wl = d_rd(0, d);
        wr = d_rd(1, d);
        dbuf[0][d_w] = d_q((inl + inr) * 0.5f + fb * wr);
        dbuf[1][d_w] = d_q(fb * wl);
        break;
    case DLY_HIPASS:                               /* the repeats thin out: a one-pole high-pass at ~700 Hz */
        wl = d_rd(0, d);
        wr = d_rd(1, d * 1.02f);
        d_hp[0] += 0.095f * (wl - d_hp[0]);
        d_hp[1] += 0.095f * (wr - d_hp[1]);
        dbuf[0][d_w] = d_q(inl + fb * (wl - d_hp[0]));
        dbuf[1][d_w] = d_q(inr + fb * (wr - d_hp[1]));
        wl -= d_hp[0];
        wr -= d_hp[1];
        break;
    case DLY_TAPE:                                 /* wow and flutter, darker and saturated repeats */
        d_wow = frac1(d_wow + 0.55f * ISR);
        d_flut = frac1(d_flut + 7.1f * ISR);
        d *= 1.0f + 0.004f * fm_sin_turns(d_wow) + 0.0008f * fm_sin_turns(d_flut);
        wl = d_rd(0, d);
        wr = d_rd(1, d * 1.01f);
        d_lp[0] += 0.3f * (wl - d_lp[0]);
        d_lp[1] += 0.3f * (wr - d_lp[1]);
        dbuf[0][d_w] = d_q(fm_tanhf(inl + fb * 1.05f * d_lp[0]));
        dbuf[1][d_w] = d_q(fm_tanhf(inr + fb * 1.05f * d_lp[1]));
        wl = d_lp[0];
        wr = d_lp[1];
        break;
    default:                                       /* STEREO */
        wl = d_rd(0, d);
        wr = d_rd(1, d);
        dbuf[0][d_w] = d_q(inl + fb * wl);
        dbuf[1][d_w] = d_q(inr + fb * wr);
        break;
    }
    if (++d_w >= DLEN)
        d_w = 0;
    *l = inl * (1.0f - mix) + wl * mix;
    *r = inr * (1.0f - mix) + wr * mix;
}

/* reverb: four allpass diffusers, then a 4-line feedback delay network (Hadamard), a low-pass in each line */
#define RV_MAX 3700u
#define AP_MAX 640u
static float rv_line[4][RV_MAX] PLAT_POOL;
static float rv_ap[4][AP_MAX] PLAT_POOL;
static const uint16_t RV_LEN[4] = {1687, 1601, 2053, 2251};
static const uint16_t AP_LEN[4] = {142, 107, 379, 277};
static uint32_t rv_n[4], ap_n[4], rv_i[4], ap_i[4];
static float rv_g[4], rv_lp[4], rv_damp, rv_lfo;

static RAMC void reverb_size(void)
{
    static const float SCALE[REV_N] = {1.0f, 1.0f, 0.62f, 1.6f, 1.0f, 1.25f};
    float s = SCALE[P.rev_type % REV_N];
    int i;
    for (i = 0; i < 4; i++) {
        rv_n[i] = (uint32_t)((float)RV_LEN[i] * s);
        ap_n[i] = (uint32_t)((float)AP_LEN[i] * s);
        if (rv_n[i] > RV_MAX)
            rv_n[i] = RV_MAX;
        if (ap_n[i] > AP_MAX)
            ap_n[i] = AP_MAX;
        if (rv_i[i] >= rv_n[i])
            rv_i[i] = 0;
        if (ap_i[i] >= ap_n[i])
            ap_i[i] = 0;
    }
}

INL void reverb_sample(float *l, float *r, float mix)
{
    float x = (*l + *r) * 0.35f, o[4], f[4], h[4], wl, wr;
    int i;
    for (i = 0; i < 4; i++) {                      /* the diffusers */
        float *b = rv_ap[i], d = b[ap_i[i]], y = d - 0.6f * x;
        b[ap_i[i]] = x + 0.6f * y;
        if (++ap_i[i] >= ap_n[i])
            ap_i[i] = 0;
        x = y;
    }
    for (i = 0; i < 4; i++) {
        o[i] = rv_line[i][rv_i[i]];
        rv_lp[i] += (1.0f - rv_damp) * (o[i] - rv_lp[i]);
        f[i] = rv_lp[i] * rv_g[i];
    }
    h[0] = 0.5f * (f[0] + f[1] + f[2] + f[3]);
    h[1] = 0.5f * (f[0] - f[1] + f[2] - f[3]);
    h[2] = 0.5f * (f[0] + f[1] - f[2] - f[3]);
    h[3] = 0.5f * (f[0] - f[1] - f[2] + f[3]);
    for (i = 0; i < 4; i++) {
        rv_line[i][rv_i[i]] = fm_flush(x + h[i]);
        if (++rv_i[i] >= rv_n[i])
            rv_i[i] = 0;
    }
    wl = (o[0] + o[2]) * 0.6f;
    wr = (o[1] + o[3]) * 0.6f;
    if (P.rev_type == REV_RISER) {                 /* swells and brightens after each note */
        static float lpl, lpr;
        float c = 0.02f + 0.9f * riser;
        lpl += c * (wl - lpl);
        lpr += c * (wr - lpr);
        wl = lpl * riser * 1.6f;
        wr = lpr * riser * 1.6f;
    } else if (P.rev_type == REV_SUBMARINE) {      /* murky and slowly moving: a swept low-pass */
        static float lpl, lpr;
        float c;
        rv_lfo = frac1(rv_lfo + 0.18f * ISR);
        c = 0.012f + 0.03f * (0.5f + 0.5f * fm_sin_turns(rv_lfo));
        lpl += c * (wl - lpl);
        lpr += c * (wr - lpr);
        wl = lpl * 2.2f;
        wr = lpr * 2.2f;
    }
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

static RAMC void reverb_control(void)
{
    float rt = 0.3f + 9.7f * kn(P.rev_time) * kn(P.rev_time);
    int i;
    for (i = 0; i < 4; i++)                        /* -60 dB in rt seconds */
        rv_g[i] = fm_exp2f(-9.9658f * (float)rv_n[i] / (SR * rt));
    rv_damp = 0.05f + 0.85f * kn(P.rev_depth);
    if (P.rev_type == REV_PLATE)
        rv_damp *= 0.5f;
    if (P.rev_type == REV_SUBMARINE && rv_damp < 0.6f)
        rv_damp = 0.6f;
}

/* ----------------------------------------------------------------- render --- */
static float peak;
/* the oscilloscope: the last SCOPE_N output samples (left), written by the interrupt, read by the screen */
#define SCOPE_N 512u
static float scope[SCOPE_N];
static volatile uint32_t scope_w;
static float vu;                                   /* a slow level for the keys' lights */
static uint8_t last_mod = 0xFF, last_dly = 0xFF, last_rev = 0xFF;

static RAMC void clear_tails(int mod, int dly, int rev)
{
    uint32_t i;
    if (mod) {
        for (i = 0; i < MODLEN; i++)
            modbuf[0][i] = modbuf[1][i] = 0.0f;
        mod_fb[0] = mod_fb[1] = ph_fb[0] = ph_fb[1] = 0.0f;
        for (i = 0; i < 6; i++)
            ph_ap[0][i] = ph_ap[1][i] = 0.0f;
    }
    if (dly)
        for (i = 0; i < DLEN; i++)
            dbuf[0][i] = dbuf[1][i] = 0;
    if (rev) {
        for (i = 0; i < RV_MAX; i++)
            rv_line[0][i] = rv_line[1][i] = rv_line[2][i] = rv_line[3][i] = 0.0f;
        for (i = 0; i < AP_MAX; i++)
            rv_ap[0][i] = rv_ap[1][i] = rv_ap[2][i] = rv_ap[3][i] = 0.0f;
        for (i = 0; i < 4; i++)
            rv_lp[i] = 0.0f;
    }
}

void dsp_init(void)
{
    V.note = -1;
    V.k = 2.0f;
    PLAT_FAR(clear_tails)(1, 1, 1);
}

static RAMC void take_input(void)
{
    uint32_t s = slot_seq;
    if (s != p_seq) {
        p_seq = s;
        {                                          /* by words: no memcpy (in the flash; -fno-builtin keeps the loop) */
            const uint32_t *f = (const uint32_t *)(const void *)&slot[slot_idx];
            uint32_t *t = (uint32_t *)(void *)&P, w;
            for (w = 0; w < sizeof P / 4u; w++)
                t[w] = f[w];
        }
        if (P.mod_type != last_mod && last_mod == MOD_OFF)
            clear_tails(1, 0, 0);
        if (P.dly_type != last_dly && last_dly == DLY_OFF)
            clear_tails(0, 1, 0);
        if (P.rev_type != last_rev) {
            if (last_rev == REV_OFF)
                clear_tails(0, 0, 1);
            reverb_size();
        }
        last_mod = P.mod_type;
        last_dly = P.dly_type;
        last_rev = P.rev_type;
    }
    if (panic_req) {
        panic_req = 0;
        nheld = 0;
        A.root = -1;
        arp_active = 0;
        V.gate = 0;
        V.egs = EGS_IDLE;
        V.env = 0.0f;
        clear_tails(1, 1, 1);
    }
    while (ev_r != ev_w) {
        uint16_t e = ev[ev_r % EVN];
        ev_r++;
        key_event(e & 0x7F, e >> 8);
    }
}

RAMC void dsp_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, j;
    take_input();
    if (p_seq == 0xFFFFFFFFu) {                    /* no patch yet */
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    for (i = 0; i < n; i += SUB) {
        uint32_t m = n - i < SUB ? n - i : SUB;
        float lfo, ld, shape, pitch, dt, sweep, sd, cut, trem_rate, trem_d, mrate, mdepth, dt_t, dfb, dmix, rmix;
        arp_frames(m);
        /* control rate: the LFOs and what they move */
        V.lfo_ph = frac1(V.lfo_ph + 30.0f * kn(P.lfo_rate) * kn(P.lfo_rate) * (float)m * ISR);
        lfo = 4.0f * fm_fabsf(V.lfo_ph - 0.5f) - 1.0f;          /* a triangle, -1..1 */
        ld = bip(P.lfo_depth);
        pitch = V.freq_note + (ld < 0.0f ? -ld * 7.0f * lfo : 0.0f);
        shape = fm_clampf(kn(P.shape) + (ld > 0.0f ? ld * 0.5f * lfo : 0.0f), 0.0f, 1.0f);
        dt = 440.0f * fm_exp2f((pitch - 69.0f) * (1.0f / 12.0f)) * ISR;
        if (dt > 0.45f)
            dt = 0.45f;
        V.sweep_ph += 30.0f * kn(P.sweep_rate) * kn(P.sweep_rate) * (float)m * ISR;
        if (V.sweep_ph >= 1.0f)
            V.sweep_ph -= fm_floorf(V.sweep_ph);
        sd = bip(P.sweep_depth);
        sweep = sd * 4.0f * V.sweep_ph;                          /* octaves, up or down */
        cut = kn(P.cutoff) * 9.8f + sweep;
        V.cut_now += (cut - V.cut_now) * 0.25f;
        filter_coef(V.cut_now, kn(P.reso));
        V.att_inc = 1.0f / ((0.001f * fm_powf(5000.0f, kn(P.attack))) * SR);
        V.rel_coef = fm_expf(-6.9f / ((0.001f * fm_powf(8000.0f, kn(P.release))) * SR));
        trem_rate = 60.0f * kn(P.trem_rate) * kn(P.trem_rate);
        trem_d = kn(P.trem_depth);
        mrate = 0.05f + 4.0f * kn(P.mod_time) * kn(P.mod_time);
        mdepth = kn(P.mod_depth);
        if (P.mod_type == MOD_PHASER) {
            float f = 250.0f * fm_exp2f(3.5f * (0.5f + 0.5f * fm_sin_turns(mod_lfo))), t = fm_tanf(3.14159265f * f * ISR);
            ph_a = (t - 1.0f) / (t + 1.0f);
        }
        dt_t = (0.01f + 0.49f * kn(P.dly_time)) * SR;
        dfb = 0.95f * kn(P.dly_depth);
        dmix = kn(P.dly_mix);
        rmix = kn(P.rev_mix);
        reverb_control();
        riser += (1.0f - riser) * (float)m * ISR * 0.6f;
        for (j = 0; j < m; j++) {
            float s, l, r, env;
            s = osc_sample(dt, shape, kn(P.alt));
            s = filter_sample(s);
            env = eg_sample();
            V.trem_ph = frac1(V.trem_ph + trem_rate * ISR);
            s *= env * (1.0f - trem_d * (0.5f + 0.5f * fm_sin_turns(V.trem_ph))) * 0.5f;
            l = r = s;
            if (P.mod_type != MOD_OFF)
                mod_sample(&l, &r, mrate, mdepth);
            if (P.dly_type != DLY_OFF)
                delay_sample(&l, &r, dt_t, dfb, dmix);
            if (P.rev_type != REV_OFF)
                reverb_sample(&l, &r, rmix);
            l = fm_tanhf(l);
            r = fm_tanhf(r);
            if (fm_fabsf(l) > peak)
                peak = fm_fabsf(l);
            scope[scope_w & (SCOPE_N - 1u)] = l;
            scope_w++;
            vu = fm_maxf(vu * 0.99995f, fm_fabsf(l));
            lr[2u * (i + j)] = (int32_t)(l * gain);
            lr[2u * (i + j) + 1u] = (int32_t)(r * gain);
        }
    }
}

int dsp_note(void) { return V.gate ? V.note : -1; }
float dsp_vu(void) { return vu; }
/* n samples of the output into out, starting at a rising zero crossing (a steady picture), oldest first */
int dsp_scope(float *out, uint32_t n)
{
    uint32_t w = scope_w, i, start = w - n, best = start, k;
    for (k = 0; k < SCOPE_N - n - 1u; k++) {            /* back from the newest that still leaves n after it */
        uint32_t a = w - n - 1u - k;
        if (scope[a & (SCOPE_N - 1u)] < 0.0f && scope[(a + 1u) & (SCOPE_N - 1u)] >= 0.0f) {
            best = a + 1u;
            break;
        }
    }
    for (i = 0; i < n; i++)
        out[i] = scope[(best + i) & (SCOPE_N - 1u)];
    return best != start;
}
float dsp_level(void) { return V.env; }
uint32_t dsp_arp_steps(void) { return A.steps; }
float dsp_peak(void)
{
    float p = peak;
    peak = 0.0f;
    return p;
}
