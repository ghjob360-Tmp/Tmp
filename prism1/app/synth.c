/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1's engine (an fm1-base o2 unit, running from RAM on the FM-1).
 * Following Vital (Matt Tytel, GPL-3.0): the phase warps (synth_oscillator.cpp syncPhase / quantizePhase / bendPhase
 * / squeezePhase / pulseWidthPhase / fmPhase and the half-sine, pulse and ring windows), the envelope's power curve
 * (futils.h powerScale), the squared amplitude of ENV1 x velocity (synth_voice_handler.cpp), the filter blend from
 * low through band to high, and the effects' order. Written again here for floats and one core.
 *
 * Threads: the main loop writes a patch slot, the table pointers and a key ring; the audio interrupt does the rest.
 * Control rate: every SUB frames per voice (envelopes, LFOs, the matrix, pitches, filter coefficients). */
#include "plat.h"
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#undef inline
#include "synth.h"

#define SR 44100.0f
#define ISR (1.0f / SR)
#define SUB 16u
#define RAMC PLAT_RAM_CODE
#define INL static inline __attribute__((always_inline))

INL float kn(uint16_t v) { return (float)(v > 1023u ? 1023u : v) * (1.0f / 1023.0f); }
INL float bip(uint16_t v) { return fm_clampf(((float)v - 512.0f) * (1.0f / 511.0f), -1.0f, 1.0f); }
INL float frac1(float x) { return x - fm_floorf(x); }
INL float lerp(float a, float b, float t) { return a + (b - a) * t; }

static uint32_t rng = 0x2545F491u;
INL uint32_t rndu(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
INL float rndf(void) { return (float)(int32_t)rndu() * (1.0f / 2147483648.0f); }   /* -1..1 */

/* Vital's powerScale: (e^(p x) - 1) / (e^p - 1), linear near p = 0 */
INL float power_scale(float x, float p)
{
    if (fm_fabsf(p) < 0.005f)
        return x;
    return (fm_expf(p * x) - 1.0f) / (fm_expf(p) - 1.0f);
}

/* ------------------------------------------------------- main loop -> audio --- */
_Static_assert(sizeof(patch_t) % 4u == 0u, "patch_t is copied by words");
static patch_t slot[2];
static volatile uint32_t slot_idx, slot_seq;
static patch_t P;
static uint32_t p_seq = 0xFFFFFFFFu;
static const wt_t *volatile tab_req[2];
static const wt_t *tab[2];

void synth_patch(const patch_t *p)
{
    uint32_t n = slot_idx ^ 1u;
    slot[n] = *p;
    slot_idx = n;
    slot_seq++;
}
void synth_tables(int osc, const wt_t *t) { tab_req[osc & 1] = t; }

#define EVN 64u
static volatile uint16_t ev[EVN];
static volatile uint32_t ev_w, ev_r;
static volatile uint8_t panic_req, shed_req;
void synth_key(int note, int vel)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVN)
        return;
    ev[w % EVN] = (uint16_t)(((uint32_t)vel & 0x7Fu) << 7 | ((uint32_t)note & 0x7Fu));
    ev_w = w + 1u;
}
void synth_panic(void) { panic_req = 1; }
void synth_overload(void) { shed_req = 1; }
static float press;
void synth_pressure(float p) { press = p; }

/* ------------------------------------------------------------------ voices --- */
enum { ES_IDLE, ES_DELAY, ES_ATTACK, ES_HOLD, ES_DECAY, ES_SUSTAIN, ES_RELEASE };
typedef struct {
    uint8_t stage;
    float t, start, val, prev;           /* seconds in the stage, the level it began at, now, a block ago */
} env_t;
typedef struct {
    float ph, val, held, next, smooth;
} lfo_t;
typedef struct {
    int note, gate, active;
    uint32_t age;
    float vel, rand, pitch, target;      /* pitch: semitones, gliding toward target */
    float ph[2][3];                      /* oscillator phases, per unison voice */
    float inc[2][3];
    int lvl[2];                          /* mip level */
    float frame[2], warp[2], level[2], other;   /* other: OSC A's last sample, for B's FM / RM, and B's for A */
    float b_prev;
    env_t env[3];
    lfo_t lfo[2];
    float mods[DST_N];
    /* the filter, two channels */
    float ic1[2][2], ic2[2][2];          /* SVF: stage, channel */
    float ld[2][4];                      /* ladder: channel, pole */
    float fo1[2][3], fo2[2][3];          /* formant: channel, band */
    float g, k, a1, a2, a3, lg, lk, fg[3], fa1[3], fa2[3], fa3[3], fk;
    float comb_d, comb_fb, comb_lp[2];
    uint32_t comb_w;
    float blend, drive, noise, pan, amp, amp_prev;
} voice_t;

static voice_t V[NVOICES];
static uint32_t age_now;
static float wheel, bend;
static float lfo_free_ph[2];             /* FREE mode: one phase for every voice */
static uint32_t shed_count;
static int last_note = -1;

#define COMB_LEN 768u
static int16_t comb_buf[NVOICES][2][COMB_LEN] PLAT_POOL;

INL float env_time(uint16_t v)           /* Vital's quartic time: (v * 2)^4 s, up to 16 s */
{
    float x = kn(v) * 2.0f;
    x *= x;
    return x * x;
}

static RAMC void voice_start(voice_t *v, int note, int vel)
{
    int o, u, e;
    int legato = v->active && v->gate;
    v->note = note;
    v->gate = 1;
    v->age = ++age_now;
    v->vel = (float)vel * (1.0f / 127.0f);
    v->rand = rndf();
    v->target = (float)note;
    if (!v->active || kn(P.glide) < 0.002f)
        v->pitch = (float)note;
    if (!v->active) {
        for (o = 0; o < 2; o++)
            for (u = 0; u < 3; u++)
                v->ph[o][u] = frac1(rndf());          /* Vital's random phase */
        for (u = 0; u < 2; u++)
            v->ic1[0][u] = v->ic1[1][u] = v->ic2[0][u] = v->ic2[1][u] = 0.0f;
        v->amp_prev = 0.0f;
    }
    v->active = 1;
    if (!legato || P.poly > 1u)
        for (e = 0; e < 3; e++) {
            v->env[e].stage = ES_DELAY;
            v->env[e].t = 0.0f;
            v->env[e].start = v->env[e].val;
        }
    for (e = 0; e < 2; e++)
        if (P.lfo[e].mode != LM_FREE && (!legato || P.poly > 1u)) {
            v->lfo[e].ph = 0.0f;
            v->lfo[e].held = rndf();
            v->lfo[e].next = rndf();
        }
}

static RAMC void voice_release(voice_t *v)
{
    int e;
    v->gate = 0;
    for (e = 0; e < 3; e++)
        if (v->env[e].stage != ES_IDLE && v->env[e].stage != ES_RELEASE) {
            v->env[e].stage = ES_RELEASE;
            v->env[e].t = 0.0f;
            v->env[e].start = v->env[e].val;
        }
}

static int held_stack[16], nheld;        /* mono: the keys held, last on top */

static RAMC void note_on(int note, int vel)
{
    int i, best = 0, poly = P.poly < 1u ? 1 : P.poly > NVOICES ? NVOICES : (int)P.poly;
    uint32_t oldest = 0xFFFFFFFFu;
    last_note = note;
    if (poly == 1) {                     /* mono, legato: one voice */
        int j, k;
        for (j = k = 0; j < nheld; j++)
            if (held_stack[j] != note)
                held_stack[k++] = held_stack[j];
        nheld = k < 16 ? k : 15;
        held_stack[nheld++] = note;
        voice_start(&V[0], note, vel);
        return;
    }
    for (i = 0; i < poly; i++)           /* the same note again: that voice */
        if (V[i].active && V[i].note == note) {
            voice_start(&V[i], note, vel);
            return;
        }
    for (i = 0; i < poly; i++)           /* a free one, else the oldest released, else the oldest */
        if (!V[i].active) {
            voice_start(&V[i], note, vel);
            return;
        }
    for (i = 0; i < poly; i++) {
        uint32_t a = V[i].age - (V[i].gate ? 0u : 0x40000000u);
        if (a < oldest) {
            oldest = a;
            best = i;
        }
    }
    voice_start(&V[best], note, vel);
}

static RAMC void note_off(int note)
{
    int i;
    if (P.poly <= 1u) {
        int j, k;
        for (j = k = 0; j < nheld; j++)
            if (held_stack[j] != note)
                held_stack[k++] = held_stack[j];
        nheld = k;
        if (V[0].note == note) {
            if (nheld) {
                V[0].note = held_stack[nheld - 1];
                V[0].target = (float)V[0].note;
                if (kn(P.glide) < 0.002f)
                    V[0].pitch = V[0].target;
            } else {
                voice_release(&V[0]);
            }
        }
        return;
    }
    for (i = 0; i < NVOICES; i++)
        if (V[i].active && V[i].gate && V[i].note == note)
            voice_release(&V[i]);
}

void RAMC synth_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90u:
        if (d2) {
            note_on((int)d1, (int)d2);
            break;
        }
        /* fall through: velocity 0 */
    case 0x80u: note_off((int)d1); break;
    case 0xB0u:
        if (d1 == 1u)
            wheel = (float)d2 * (1.0f / 127.0f);
        else if (d1 == 120u || d1 == 123u)
            panic_req = 1;
        break;
    case 0xD0u: press = (float)d1 * (1.0f / 127.0f); break;
    case 0xE0u: bend = ((float)((d2 << 7) | d1) - 8192.0f) * (2.0f / 8192.0f); break;   /* +-2 semitones */
    default: break;
    }
}

/* ------------------------------------------------------ envelopes and LFOs --- */
static RAMC void env_step(env_t *e, const env_p *p, float dt, int gate)
{
    float d, sus = kn(p->sustain), apow = -bip(p->apow) * 8.0f, drpow = bip(p->drpow) * 8.0f;
    e->prev = e->val;
    e->t += dt;
    for (;;) {
        switch (e->stage) {
        case ES_DELAY:
            d = env_time(p->delay);
            if (e->t < d)
                return;
            e->t -= d;
            e->stage = ES_ATTACK;
            continue;
        case ES_ATTACK:
            d = env_time(p->attack);
            if (e->t < d) {
                e->val = lerp(e->start, 1.0f, power_scale(e->t / d, apow));
                return;
            }
            e->t -= d;
            e->stage = ES_HOLD;
            continue;
        case ES_HOLD:
            d = env_time(p->hold);
            e->val = 1.0f;
            if (e->t < d)
                return;
            e->t -= d;
            e->stage = ES_DECAY;
            continue;
        case ES_DECAY:
            d = env_time(p->decay);
            if (e->t < d) {
                e->val = 1.0f - (1.0f - sus) * power_scale(e->t / d, drpow);
                return;
            }
            e->stage = ES_SUSTAIN;
            continue;
        case ES_SUSTAIN:
            e->val = sus;
            if (!gate) {
                e->stage = ES_RELEASE;
                e->t = 0.0f;
                e->start = sus;
                continue;
            }
            return;
        case ES_RELEASE:
            d = env_time(p->release);
            if (e->t < d) {
                e->val = e->start * (1.0f - power_scale(e->t / d, drpow));
                return;
            }
            e->val = 0.0f;
            e->stage = ES_IDLE;
            return;
        default:
            e->val = 0.0f;
            return;
        }
    }
}

INL float lfo_rate(uint16_t v, float mod)      /* 0.02 .. 30 Hz, modulated in octaves */
{
    return 0.02f * fm_exp2f(kn(v) * 10.55f + mod * 4.0f);
}

static RAMC float lfo_step(lfo_t *l, const lfo_p *p, float *free_ph, float rate, float dt)
{
    float ph, x, wrapped = 0.0f;
    if (p->mode == LM_FREE) {
        ph = *free_ph;
    } else {
        l->ph += rate * dt;
        if (p->mode == LM_ENVELOPE && l->ph >= 1.0f)
            l->ph = 0.9999f;               /* one cycle, then it holds */
        if (l->ph >= 1.0f) {
            l->ph -= fm_floorf(l->ph);
            wrapped = 1.0f;
        }
        ph = l->ph;
    }
    if (wrapped > 0.0f || (p->mode == LM_FREE && ph < rate * dt)) {   /* a new cycle: the random ones draw */
        l->held = l->next;
        l->next = rndf();
    }
    switch (p->shape) {
    default:
    case LS_SINE: x = fm_sin_turns(ph); break;
    case LS_TRI: x = 1.0f - 4.0f * fm_fabsf(ph - 0.5f); x = -x; break;
    case LS_SAWUP: x = 2.0f * ph - 1.0f; break;
    case LS_SAWDN: x = 1.0f - 2.0f * ph; break;
    case LS_SQUARE: x = ph < 0.5f ? 1.0f : -1.0f; break;
    case LS_SH: x = l->held; break;
    case LS_SMOOTH: x = lerp(l->held, l->next, 0.5f - 0.5f * fm_sin_turns((ph * 0.5f) + 0.25f)); break;   /* Vital: sin interp */
    }
    {                                      /* Vital's smoothing: a one-pole with a half-life of smooth * 0.2 */
        float st = kn(p->smooth) * 0.5f;
        if (st > 0.0005f) {
            float m = fm_exp2f(-dt / (st * 0.2f));
            l->smooth = x + (l->smooth - x) * m;
            x = l->smooth;
        } else {
            l->smooth = x;
        }
    }
    l->val = x;
    return x;
}

/* ---------------------------------------------------------- control rate --- */
static float mod_scale[DST_N] = {0, 1, 1, 24, 1, 1, 1, 24, 1, 64, 1, 2, 1, 1, 1, 1, 1, 1};

static RAMC void voice_control(voice_t *v, float dt)
{
    float src[SRC_N], *m = v->mods, cut, st, fc, w;
    int i, o, e;
    for (e = 0; e < 3; e++)
        env_step(&v->env[e], &P.env[e], dt, v->gate);
    if (v->env[0].stage == ES_IDLE) {
        v->active = 0;
        return;
    }
    src[SRC_NONE] = 0.0f;
    src[SRC_ENV1] = v->env[0].val;
    src[SRC_ENV2] = v->env[1].val;
    src[SRC_ENV3] = v->env[2].val;
    src[SRC_VEL] = v->vel;
    src[SRC_NOTE] = ((float)v->note - 60.0f) * (1.0f / 60.0f);
    src[SRC_RANDOM] = v->rand;
    src[SRC_WHEEL] = wheel;
    src[SRC_PRESS] = press;
    src[SRC_LFO1] = src[SRC_LFO2] = 0.0f;
    for (e = 0; e < 2; e++) {           /* the LFOs first: the matrix may move their rates (from the last block) */
        float r = lfo_rate(P.lfo[e].rate, m[DST_LFO1_RATE + e]);
        src[SRC_LFO1 + e] = lfo_step(&v->lfo[e], &P.lfo[e], &lfo_free_ph[e], r, dt);
    }
    for (i = 0; i < DST_N; i++)         /* (the LFO rates above read the last block's) */
        m[i] = 0.0f;
    for (i = 0; i < NMOD; i++) {
        const mod_p *s = &P.mod[i];
        if (s->src && s->dst && s->src < SRC_N && s->dst < DST_N)
            m[s->dst] += bip(s->amt) * src[s->src];
    }
    for (i = 0; i < DST_N; i++)
        m[i] *= mod_scale[i];
    /* pitch: glide, then each oscillator's own */
    v->pitch += (v->target - v->pitch) * (1.0f - fm_exp2f(-dt / (0.001f + kn(P.glide) * kn(P.glide) * 2.0f)));
    for (o = 0; o < 2; o++) {
        const osc_p *op = &P.osc[o];
        int n = op->unison < 1u ? 1 : op->unison > 3u ? 3 : (int)op->unison, u;
        float base = v->pitch + bend + ((float)op->transpose - 24.0f) + m[o ? DST_B_PITCH : DST_A_PITCH],
              det = kn(op->detune) * kn(op->detune) * 0.5f, mx = 0.0f;      /* up to half a semitone each way */
        for (u = 0; u < n; u++) {
            float off = n == 1 ? 0.0f : n == 2 ? (u ? det : -det) : (float)(u - 1) * det;
            float inc = 440.0f * fm_exp2f((base + off - 69.0f) * (1.0f / 12.0f)) * ISR;
            v->inc[o][u] = inc > 0.49f ? 0.49f : inc;
            if (v->inc[o][u] > mx)
                mx = v->inc[o][u];
        }
        w = fm_clampf(kn(op->warp_amt) + m[o ? DST_B_WARP : DST_A_WARP], 0.0f, 1.0f);
        v->warp[o] = w;
        if (op->warp == WARP_SYNC || op->warp == WARP_FORMANT)
            mx *= fm_exp2f(4.0f * w);
        /* the mip level: the most harmonics that stay under Nyquist (level l holds 127 >> l) */
        {
            int l = 0;
            float top = 127.0f * mx;
            while (l < WT_LEVELS - 1 && top > 0.5f) {
                top *= 0.5f;
                l++;
            }
            v->lvl[o] = l;
        }
        v->frame[o] = fm_clampf(kn(op->frame) + m[o ? DST_B_FRAME : DST_A_FRAME], 0.0f, 1.0f) * (float)(WT_FRAMES - 1);
        v->level[o] = fm_clampf(kn(op->level) + m[o ? DST_B_LEVEL : DST_A_LEVEL], 0.0f, 1.0f);
    }
    v->noise = fm_clampf(kn(P.noise) + m[DST_NOISE], 0.0f, 1.0f);
    v->pan = fm_clampf(m[DST_PAN], -1.0f, 1.0f);
    v->blend = fm_clampf(kn(P.blend) * 2.0f + m[DST_BLEND], 0.0f, 2.0f);
    v->drive = 1.0f + 8.0f * fm_clampf(kn(P.drive) + m[DST_DRIVE], 0.0f, 1.0f);
    /* the filter: cutoff in semitones as Vital (8 .. 136), key tracked, modulated */
    cut = 8.0f + 128.0f * kn(P.cutoff) + kn(P.keytrack) * ((float)v->note - 60.0f) + m[DST_CUTOFF];
    st = fm_clampf(cut, 8.0f, 136.0f);
    fc = 440.0f * fm_exp2f((st - 69.0f) * (1.0f / 12.0f));
    if (fc > 0.45f * SR)
        fc = 0.45f * SR;
    {
        float r = fm_clampf(kn(P.reso) + m[DST_RESO], 0.0f, 1.0f);
        switch (P.f_model) {
        case FM_ANALOG:
            v->g = fm_tanf(3.14159265f * fc * ISR);
            v->k = 2.0f - 1.96f * r;
            v->a1 = 1.0f / (1.0f + v->g * (v->g + v->k));
            v->a2 = v->g * v->a1;
            v->a3 = v->g * v->a2;
            break;
        case FM_LADDER:
            v->lg = 1.0f - fm_expf(-6.2831853f * fc * ISR);
            v->lk = 3.9f * r;
            break;
        case FM_FORMANT: {              /* three bands at the vowel's formants, moved by the cutoff */
            static const float VOW[2][4][3] = {
                {{800, 1150, 2900}, {450, 800, 2830}, {350, 2000, 2800}, {400, 1600, 2700}},     /* A O I E */
                {{800, 1150, 2900}, {350, 2000, 2800}, {325, 700, 2530}, {450, 800, 2830}}};     /* A I U O */
            float x = v->blend * 1.5f, sh = fm_exp2f((kn(P.cutoff) - 0.5f) * 2.0f + m[DST_CUTOFF] * (1.0f / 24.0f));
            int a = (int)x, b, j;
            if (a > 2)
                a = 2;
            b = a + 1;
            x -= (float)a;
            v->fk = 0.25f - 0.2f * r;
            for (j = 0; j < 3; j++) {
                const float(*T)[3] = VOW[P.f_style & 1u];
                float f = (T[a][j] + (T[b][j] - T[a][j]) * x) * sh;
                float g = fm_tanf(3.14159265f * fm_clampf(f, 60.0f, 0.45f * SR) * ISR);
                v->fg[j] = g;
                v->fa1[j] = 1.0f / (1.0f + g * (g + v->fk));
                v->fa2[j] = g * v->fa1[j];
                v->fa3[j] = g * v->fa2[j];
            }
            break;
        }
        case FM_COMB:
            v->comb_d = fm_clampf(SR / fc, 2.0f, (float)(COMB_LEN - 2u));
            v->comb_fb = (P.f_style & 1u ? -0.97f : 0.97f) * r;
            break;
        default:
            break;
        }
    }
    {   /* ENV1 squared with velocity (Vital), the volume modulated */
        float a = v->env[0].val * lerp(1.0f, v->vel, kn(P.veltrack));
        v->amp_prev = v->amp;
        v->amp = a * a * fm_clampf(1.0f + m[DST_VOLUME], 0.0f, 2.0f);
    }
}

/* -------------------------------------------------------------- oscillators --- */
/* a warp of the phase p (0..1); *win gets the window (1 = none) */
INL float warp_phase(int type, float p, float w, float other, float *win)
{
    float c, a, d;
    *win = 1.0f;
    switch (type) {
    case WARP_SYNC: return frac1(p * fm_exp2f(4.0f * w));
    case WARP_FORMANT:                   /* sync under a half-sine window */
        *win = fm_sin_turns(p * 0.5f);
        return frac1(p * fm_exp2f(4.0f * w));
    case WARP_QUANTIZE: {                /* Vital: 2^(0.85 (1-w)^3 bits + 1) steps */
        float q = 1.0f - w;
        q = fm_exp2f(q * q * q * 0.85f * 10.0f + 1.0f);
        return fm_floorf(p * q) / q;
    }
    case WARP_BEND: {                    /* Vital's cubic; w = 0.5 leaves the phase alone */
        float o = (w - w * w) * 2.0f, s3 = w * 3.0f, p2 = p * p, p3 = p2 * p;
        return p3 + (s3 + o) * (p2 - p3) + (s3 - o) * (p - 2.0f * p2 + p3);
    }
    case WARP_SQUEEZE:                   /* Vital: d = 0.05..1.95, 1 = none (w = 0.5) */
        d = w * 1.9f + 0.05f;
        c = p - 0.5f;
        a = fm_fabsf(c);
        a = a > d * 0.25f ? 0.5f - (0.5f - a) / (2.0f - d) : a / d;
        return (c < 0.0f ? -a : a) + 0.5f;
    case WARP_PULSE:                     /* the cycle squeezed into 1 - w of it, silence after (Vital's window) */
        d = 1.0f / fm_maxf(1.0f - w, 0.02f);
        c = (p - 0.5f) * d;
        if (c >= 0.5f || c < -0.5f) {
            *win = 0.0f;
            return 0.0f;
        }
        return c + 0.5f;
    case WARP_FM: return frac1(p + other * w * w * 4.0f);
    case WARP_RM: *win = lerp(1.0f, other, w); return p;
    default: return p;
    }
}

INL float wt_read(const wt_t *t, int lvl, float fp, float p)
{
    uint32_t n = 256u >> lvl, i0, i1;
    int f0 = (int)fp, f1;
    float x = p * (float)n, fr, ff = fp - (float)f0, a, b;
    const float *A, *B;
    if (f0 >= WT_FRAMES - 1) {
        f0 = WT_FRAMES - 1;
        ff = 0.0f;
    }
    f1 = f0 + 1 < WT_FRAMES ? f0 + 1 : f0;
    i0 = (uint32_t)(int32_t)x & (n - 1u);
    fr = x - fm_floorf(x);
    i1 = (i0 + 1u) & (n - 1u);
    A = &t->d[f0][WT_OFF[lvl]];
    B = &t->d[f1][WT_OFF[lvl]];
    a = A[i0] + (A[i1] - A[i0]) * fr;
    b = B[i0] + (B[i1] - B[i0]) * fr;
    return a + (b - a) * ff;
}

/* one oscillator, all its unison voices, into l / r */
static RAMC __attribute__((noinline)) void osc_run(voice_t *v, int o, float other, float *l, float *r, float *mono)
{
    const osc_p *op = &P.osc[o];
    const wt_t *t = tab[o];
    int n = op->unison < 1u ? 1 : op->unison > 3u ? 3 : (int)op->unison, u;
    float sl = 0.0f, sr = 0.0f, sum = 0.0f, spread = kn(op->spread);
    static const float NORM[4] = {0.0f, 1.0f, 0.7071f, 0.5774f};
    for (u = 0; u < n; u++) {
        float win, ph, s, pan;
        v->ph[o][u] += v->inc[o][u];
        if (v->ph[o][u] >= 1.0f)
            v->ph[o][u] -= 1.0f;
        ph = warp_phase((int)op->warp, v->ph[o][u], v->warp[o], other, &win);
        s = wt_read(t, v->lvl[o], v->frame[o], ph) * win;
        pan = n == 1 ? 0.0f : ((float)u / (float)(n - 1) * 2.0f - 1.0f) * spread;   /* -1 .. 1 */
        sl += s * (1.0f - pan);
        sr += s * (1.0f + pan);
        sum += s;
    }
    *l = sl * 0.5f * NORM[n] * v->level[o];
    *r = sr * 0.5f * NORM[n] * v->level[o];
    *mono = sum * NORM[n];
}

/* ------------------------------------------------------------------ filter --- */
INL float svf_tick(float *ic1, float *ic2, float x, float a1, float a2, float a3, float k, float blend)
{
    float v3 = x - *ic2, v1 = a1 * *ic1 + a2 * v3, v2 = *ic2 + a2 * *ic1 + a3 * v3, hp;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    hp = x - k * v1 - v2;
    return blend < 1.0f ? lerp(v2, v1, blend) : lerp(v1, hp, blend - 1.0f);   /* low -> band -> high */
}

static RAMC __attribute__((noinline)) float filter_tick(voice_t *v, int c, float x)
{
    switch (P.f_model) {
    case FM_ANALOG:
        x = fm_tanhf(x * v->drive) / v->drive;
        x = svf_tick(&v->ic1[0][c], &v->ic2[0][c], x, v->a1, v->a2, v->a3, v->k, v->blend);
        if (P.f_style & 1u)
            x = svf_tick(&v->ic1[1][c], &v->ic2[1][c], x, v->a1, v->a2, v->a3, v->k, v->blend);
        return x;
    case FM_LADDER: {                    /* four one-poles, tanh in the loop */
        float *y = v->ld[c], in = fm_tanhf((x - v->lk * y[3]) * v->drive), lp, bp, hp;
        y[0] += v->lg * (in - y[0]);
        y[1] += v->lg * (y[0] - y[1]);
        y[2] += v->lg * (y[1] - y[2]);
        y[3] += v->lg * (y[2] - y[3]);
        if (P.f_style & 1u) {            /* 24 dB taps */
            lp = y[3];
            bp = 4.0f * (y[1] - 2.0f * y[2] + y[3]);
            hp = in - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3];
        } else {                         /* 12 dB */
            lp = y[1];
            bp = 2.0f * (y[0] - y[1]);
            hp = in - 2.0f * y[0] + y[1];
        }
        return (v->blend < 1.0f ? lerp(lp, bp, v->blend) : lerp(bp, hp, v->blend - 1.0f)) * (1.0f + v->lk * 0.3f);
    }
    case FM_FORMANT: {
        float o = 0.0f;
        int j;
        x = fm_tanhf(x * v->drive);
        for (j = 0; j < 3; j++) {
            float v3 = x - v->fo2[c][j], v1 = v->fa1[j] * v->fo1[c][j] + v->fa2[j] * v3,
                  v2 = v->fo2[c][j] + v->fa2[j] * v->fo1[c][j] + v->fa3[j] * v3;
            v->fo1[c][j] = 2.0f * v1 - v->fo1[c][j];
            v->fo2[c][j] = 2.0f * v2 - v->fo2[c][j];
            o += v1 * (j ? 0.6f : 1.0f);
        }
        return o * 1.8f;
    }
    case FM_COMB: {                      /* a feedback comb at the cutoff's pitch, damped by the blend */
        int16_t *b = comb_buf[v - V][c];
        float p = (float)v->comb_w - v->comb_d, f, d;
        int32_t i;
        if (p < 0.0f)
            p += (float)COMB_LEN;
        i = (int32_t)p;
        f = p - (float)i;
        d = ((float)b[(uint32_t)i % COMB_LEN] * (1.0f - f) + (float)b[(uint32_t)(i + 1) % COMB_LEN] * f)
          * (1.0f / 16384.0f);
        v->comb_lp[c] += (1.0f - 0.45f * v->blend) * (d - v->comb_lp[c]);
        x = fm_tanhf(x * v->drive * 0.5f + v->comb_fb * v->comb_lp[c]);
        b[v->comb_w] = (int16_t)(fm_clampf(x, -1.99f, 1.99f) * 16384.0f);
        return x;
    }
    default:
        return x;
    }
}

/* ----------------------------------------------------------------- effects --- */
#define MODLEN 2048u
static float chbuf[2][MODLEN] PLAT_POOL;
#define FLLEN 512u
static float flbuf[2][FLLEN] PLAT_POOL;
#define DLEN 22528u
static int16_t dbuf[2][DLEN] PLAT_POOL;
#define RV_MAX 3000u
#define AP_MAX 512u
static float rv_line[4][RV_MAX] PLAT_POOL;
static float rv_ap[4][AP_MAX] PLAT_POOL;

static struct {
    uint32_t chw, flw, dw;
    float ch_lfo, fl_lfo, fl_fb[2], ph_lfo, ph_ap[2][6], ph_fb[2], ph_a;
    float d_time, cp_env, ds_hold[2], ds_ctr;
    uint32_t rv_i[4], ap_i[4], rv_n[4], ap_n[4];
    float rv_g[4], rv_lp[4], rv_damp;
} F;

INL float rdf(const float *b, uint32_t len, uint32_t w, float d)
{
    float p = (float)w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)len;
    i = (int32_t)p;
    f = p - (float)i;
    return b[(uint32_t)i & (len - 1u)] * (1.0f - f) + b[(uint32_t)(i + 1) & (len - 1u)] * f;
}
INL float rd16(const int16_t *b, uint32_t w, float d)
{
    float p = (float)w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DLEN;
    i = (int32_t)p;
    f = p - (float)i;
    return ((float)b[(uint32_t)i % DLEN] * (1.0f - f) + (float)b[(uint32_t)(i + 1) % DLEN] * f) * (1.0f / 16384.0f);
}
INL int16_t q16(float x) { return (int16_t)(fm_clampf(x, -1.99f, 1.99f) * 16384.0f); }

static RAMC void fx_control(void)
{
    static const uint16_t RL[4] = {1687, 1601, 2053, 2251}, AL[4] = {142, 107, 379, 277};
    float size = 0.6f + 0.7f * kn(P.rv_time), rt = 0.4f + 11.6f * kn(P.rv_time) * kn(P.rv_time), t;
    int i;
    for (i = 0; i < 4; i++) {
        F.rv_n[i] = (uint32_t)((float)RL[i] * size);
        F.ap_n[i] = (uint32_t)((float)AL[i] * size);
        if (F.rv_n[i] > RV_MAX)
            F.rv_n[i] = RV_MAX;
        if (F.ap_n[i] > AP_MAX)
            F.ap_n[i] = AP_MAX;
        if (F.rv_i[i] >= F.rv_n[i])
            F.rv_i[i] = 0;
        if (F.ap_i[i] >= F.ap_n[i])
            F.ap_i[i] = 0;
        F.rv_g[i] = fm_exp2f(-9.9658f * (float)F.rv_n[i] / (SR * rt));
    }
    F.rv_damp = 0.05f + 0.85f * kn(P.rv_damp);
    if (P.ph_on) {
        float f = 200.0f * fm_exp2f(3.6f * (0.5f + 0.5f * fm_sin_turns(F.ph_lfo)));
        t = fm_tanf(3.14159265f * f * ISR);
        F.ph_a = (t - 1.0f) / (t + 1.0f);
    }
}

INL void fx_sample(float *L, float *R)
{
    float x[2] = {*L, *R}, w;
    int c;
    if (P.ch_on) {                       /* CHORUS: two modulated taps per side */
        float mix = kn(P.ch_mix), dep = kn(P.ch_depth);
        F.ch_lfo = frac1(F.ch_lfo + (0.05f + 4.0f * kn(P.ch_rate) * kn(P.ch_rate)) * ISR);
        for (c = 0; c < 2; c++) {
            float o = 0.25f * (float)c;
            chbuf[c][F.chw] = x[c];
            w = rdf(chbuf[c], MODLEN, F.chw, (0.008f + 0.008f * dep * (0.5f + 0.5f * fm_sin_turns(F.ch_lfo + o))) * SR)
              + rdf(chbuf[c], MODLEN, F.chw, (0.014f + 0.01f * dep * (0.5f + 0.5f * fm_sin_turns(F.ch_lfo + o + 0.5f))) * SR);
            x[c] = lerp(x[c], 0.5f * w, mix);
        }
        F.chw = (F.chw + 1u) & (MODLEN - 1u);
    }
    if (P.cp_on) {                       /* COMPRESSOR: one band, up and down toward a level (OTT-like) */
        float lvl = fm_maxf(fm_fabsf(x[0]), fm_fabsf(x[1])), amt = kn(P.cp_amt), g;
        F.cp_env += (lvl - F.cp_env) * (lvl > F.cp_env ? 0.004f : 0.0003f);
        g = fm_clampf(fm_powf(0.25f / fm_maxf(F.cp_env, 0.002f), amt * 0.7f), 0.25f, 8.0f);
        for (c = 0; c < 2; c++)
            x[c] = lerp(x[c], x[c] * g, kn(P.cp_mix));
    }
    if (P.dl_type != DLY_OFF) {          /* DELAY */
        float fb = 0.95f * kn(P.dl_fb), mix = kn(P.dl_mix), wl, wr;
        F.d_time += ((0.02f + 0.48f * kn(P.dl_time)) * SR - F.d_time) * 0.0005f;
        wl = rd16(dbuf[0], F.dw, F.d_time);
        wr = rd16(dbuf[1], F.dw, P.dl_type == DLY_STEREO ? F.d_time * 0.75f : F.d_time);
        if (P.dl_type == DLY_MONO) {
            dbuf[0][F.dw] = q16((x[0] + x[1]) * 0.5f + fb * wl);
            wr = wl;
        } else if (P.dl_type == DLY_PINGPONG) {
            dbuf[0][F.dw] = q16((x[0] + x[1]) * 0.5f + fb * wr);
            dbuf[1][F.dw] = q16(fb * wl);
        } else {
            dbuf[0][F.dw] = q16(x[0] + fb * wl);
            dbuf[1][F.dw] = q16(x[1] + fb * wr);
        }
        if (++F.dw >= DLEN)
            F.dw = 0;
        x[0] = lerp(x[0], wl, mix);
        x[1] = lerp(x[1], wr, mix);
    }
    if (P.ds_type != DIST_OFF) {         /* DISTORTION: Vital's six */
        float g = fm_exp2f(kn(P.ds_drive) * 5.0f), mix = kn(P.ds_mix);
        int hold = 0;
        if (P.ds_type == DIST_DOWNSAMPLE) {
            F.ds_ctr += 1.0f;
            if (F.ds_ctr >= 1.0f + kn(P.ds_drive) * 30.0f) {
                F.ds_ctr = 0.0f;
                hold = 1;
            }
        }
        for (c = 0; c < 2; c++) {
            float y = x[c] * g;
            switch (P.ds_type) {
            case DIST_SOFT: y = fm_tanhf(y); break;
            case DIST_HARD: y = fm_clampf(y, -1.0f, 1.0f); break;
            case DIST_LINFOLD: y = 1.0f - 4.0f * fm_fabsf(frac1(0.25f * y + 0.25f) - 0.5f); y = -y; break;
            case DIST_SINFOLD: y = fm_sin_turns(0.25f * y); break;
            case DIST_CRUSH: {
                float q = fm_exp2f(8.0f - 7.0f * kn(P.ds_drive));
                y = fm_floorf(x[c] * q + 0.5f) / q;
                break;
            }
            case DIST_DOWNSAMPLE:
                if (hold)
                    F.ds_hold[c] = x[c];
                y = F.ds_hold[c];
                break;
            default: break;
            }
            x[c] = lerp(x[c], y * (P.ds_type <= DIST_SINFOLD ? 0.7f : 1.0f), mix);
        }
    }
    if (P.fl_on) {                       /* FLANGER */
        float mix = kn(P.fl_mix), fb = 0.9f * bip(P.fl_fb);
        F.fl_lfo = frac1(F.fl_lfo + (0.03f + 3.0f * kn(P.fl_rate) * kn(P.fl_rate)) * ISR);
        for (c = 0; c < 2; c++) {
            float d = (0.0006f + 0.0045f * (0.5f + 0.5f * fm_sin_turns(F.fl_lfo + 0.25f * (float)c))) * SR;
            flbuf[c][F.flw] = x[c] + fb * F.fl_fb[c];
            w = rdf(flbuf[c], FLLEN, F.flw, d);
            F.fl_fb[c] = w;
            x[c] = lerp(x[c], 0.7f * (x[c] + w), mix);
        }
        F.flw = (F.flw + 1u) & (FLLEN - 1u);
    }
    if (P.ph_on) {                       /* PHASER: six first-order allpasses */
        float mix = kn(P.ph_mix), fb = 0.8f * bip(P.ph_fb);
        F.ph_lfo = frac1(F.ph_lfo + (0.03f + 3.0f * kn(P.ph_rate) * kn(P.ph_rate)) * ISR);
        for (c = 0; c < 2; c++) {
            float y = x[c] + fb * F.ph_fb[c];
            int s;
            for (s = 0; s < 6; s++) {
                float o = F.ph_a * y + F.ph_ap[c][s];
                F.ph_ap[c][s] = y - F.ph_a * o;
                y = o;
            }
            F.ph_fb[c] = y;
            x[c] = lerp(x[c], 0.7f * (x[c] + y), mix);
        }
    }
    if (P.rv_on) {                       /* REVERB: diffusers, then a 4-line Hadamard FDN */
        float in = (x[0] + x[1]) * 0.3f, o[4], f[4], h[4], mix = kn(P.rv_mix);
        int i;
        for (i = 0; i < 4; i++) {
            float *b = rv_ap[i], d = b[F.ap_i[i]], y = d - 0.6f * in;
            b[F.ap_i[i]] = in + 0.6f * y;
            if (++F.ap_i[i] >= F.ap_n[i])
                F.ap_i[i] = 0;
            in = y;
        }
        for (i = 0; i < 4; i++) {
            o[i] = rv_line[i][F.rv_i[i]];
            F.rv_lp[i] += (1.0f - F.rv_damp) * (o[i] - F.rv_lp[i]);
            f[i] = F.rv_lp[i] * F.rv_g[i];
        }
        h[0] = 0.5f * (f[0] + f[1] + f[2] + f[3]);
        h[1] = 0.5f * (f[0] - f[1] + f[2] - f[3]);
        h[2] = 0.5f * (f[0] + f[1] - f[2] - f[3]);
        h[3] = 0.5f * (f[0] - f[1] - f[2] + f[3]);
        for (i = 0; i < 4; i++) {
            rv_line[i][F.rv_i[i]] = fm_flush(in + h[i]);
            if (++F.rv_i[i] >= F.rv_n[i])
                F.rv_i[i] = 0;
        }
        x[0] = lerp(x[0], (o[0] + o[2]) * 0.6f, mix);
        x[1] = lerp(x[1], (o[1] + o[3]) * 0.6f, mix);
    }
    *L = x[0];
    *R = x[1];
}

/* ------------------------------------------------------------------ render --- */
static float peak;
volatile synth_view_t SVIEW;
static RAMC void view_update(void);

static RAMC void clear_all(void)
{
    uint32_t i, c, v;
    for (i = 0; i < MODLEN; i++)
        chbuf[0][i] = chbuf[1][i] = 0.0f;
    for (i = 0; i < FLLEN; i++)
        flbuf[0][i] = flbuf[1][i] = 0.0f;
    for (i = 0; i < DLEN; i++)
        dbuf[0][i] = dbuf[1][i] = 0;
    for (i = 0; i < RV_MAX; i++)
        rv_line[0][i] = rv_line[1][i] = rv_line[2][i] = rv_line[3][i] = 0.0f;
    for (i = 0; i < AP_MAX; i++)
        rv_ap[0][i] = rv_ap[1][i] = rv_ap[2][i] = rv_ap[3][i] = 0.0f;
    for (v = 0; v < NVOICES; v++)
        for (c = 0; c < 2; c++)
            for (i = 0; i < COMB_LEN; i++)
                comb_buf[v][c][i] = 0;
    for (i = 0; i < 4; i++)
        F.rv_lp[i] = 0.0f;
    F.fl_fb[0] = F.fl_fb[1] = F.ph_fb[0] = F.ph_fb[1] = 0.0f;
    for (i = 0; i < 6; i++)
        F.ph_ap[0][i] = F.ph_ap[1][i] = 0.0f;
}

void synth_init(void)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        V[i].note = -1;
    F.d_time = 0.25f * SR;
    PLAT_FAR(clear_all)();
}

static RAMC void take_input(void)
{
    uint32_t s = slot_seq;
    int i;
    if (s != p_seq) {
        const uint32_t *f = (const uint32_t *)(const void *)&slot[slot_idx];
        uint32_t *t = (uint32_t *)(void *)&P, w;
        p_seq = s;
        for (w = 0; w < sizeof P / 4u; w++)
            t[w] = f[w];
    }
    tab[0] = tab_req[0];
    tab[1] = tab_req[1];
    if (panic_req) {
        panic_req = 0;
        nheld = 0;
        for (i = 0; i < NVOICES; i++) {
            V[i].active = 0;
            V[i].gate = 0;
            V[i].env[0].stage = V[i].env[1].stage = V[i].env[2].stage = ES_IDLE;
            V[i].env[0].val = V[i].env[1].val = V[i].env[2].val = 0.0f;
        }
        clear_all();
    }
    if (shed_req) {                      /* out of time: the oldest voice goes (a released one first) */
        uint32_t oldest = 0xFFFFFFFFu;
        int best = -1;
        shed_req = 0;
        for (i = 0; i < NVOICES; i++)
            if (V[i].active) {
                uint32_t a = V[i].age - (V[i].gate ? 0u : 0x40000000u);
                if (a < oldest) {
                    oldest = a;
                    best = i;
                }
            }
        if (best >= 0) {
            V[best].active = 0;
            V[best].env[0].stage = ES_IDLE;
            shed_count++;
        }
    }
    while (ev_r != ev_w) {
        uint16_t e = ev[ev_r % EVN];
        uint32_t vel = (uint32_t)e >> 7;
        ev_r++;
        if (vel)
            note_on(e & 0x7F, (int)vel);
        else
            note_off(e & 0x7F);
    }
}

RAMC void synth_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, j, vi;
    take_input();
    if (p_seq == 0xFFFFFFFFu || !tab[0] || !tab[1]) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    fx_control();
    for (i = 0; i < n; i += SUB) {
        uint32_t m = n - i < SUB ? n - i : SUB;
        float bl[SUB], br[SUB], dt = (float)m * ISR;
        for (j = 0; j < m; j++)
            bl[j] = br[j] = 0.0f;
        for (vi = 0; vi < 2u; vi++)       /* FREE LFOs: one phase for all */
            lfo_free_ph[vi] = frac1(lfo_free_ph[vi] + lfo_rate(P.lfo[vi].rate, 0.0f) * dt);
        for (vi = 0; vi < NVOICES; vi++) {
            voice_t *v = &V[vi];
            float a0, da, pl, pr;
            int stereo;
            if (!v->active)
                continue;
            voice_control(v, dt);
            if (!v->active)
                continue;
            stereo = (P.osc[0].unison > 1u && P.osc[0].spread > 0u) || (P.osc[1].unison > 1u && P.osc[1].spread > 0u);
            a0 = v->amp_prev;
            da = (v->amp - v->amp_prev) / (float)m;          /* ENV1 at audio rate, by line */
            pl = 1.0f - v->pan;
            pr = 1.0f + v->pan;
            for (j = 0; j < m; j++) {
                float al, ar, am, bl_, br_, bm, nz, fl = 0.0f, fr = 0.0f, dl = 0.0f, dr = 0.0f, amp;
                osc_run(v, 0, v->b_prev, &al, &ar, &am);
                osc_run(v, 1, am, &bl_, &br_, &bm);
                v->b_prev = bm;
                nz = v->noise > 0.0f ? rndf() * v->noise * 0.5f : 0.0f;
                if (P.osc[0].dest == DEST_DIRECT) { dl += al; dr += ar; } else { fl += al; fr += ar; }
                if (P.osc[1].dest == DEST_DIRECT) { dl += bl_; dr += br_; } else { fl += bl_; fr += br_; }
                fl += nz;
                fr += nz;
                if (P.f_model != FM_OFF) {
                    if (stereo) {
                        fl = filter_tick(v, 0, fl);
                        fr = filter_tick(v, 1, fr);
                    } else {
                        fl = fr = filter_tick(v, 0, (fl + fr) * 0.5f);
                    }
                }
                if (P.f_model == FM_COMB && ++v->comb_w >= COMB_LEN)
                    v->comb_w = 0;
                amp = a0 + da * (float)(j + 1u);
                bl[j] += (fl + dl) * amp * pl;
                br[j] += (fr + dr) * amp * pr;
            }
        }
        for (j = 0; j < m; j++) {
            float l = bl[j] * 0.45f, r = br[j] * 0.45f;
            fx_sample(&l, &r);
            l = fm_tanhf(l * (0.4f + 1.2f * kn(P.volume)));
            r = fm_tanhf(r * (0.4f + 1.2f * kn(P.volume)));
            if (fm_fabsf(l) > peak)
                peak = fm_fabsf(l);
            lr[2u * (i + j)] = (int32_t)(l * gain);
            lr[2u * (i + j) + 1u] = (int32_t)(r * gain);
        }
    }
    view_update();
    SVIEW.out_peak = fm_maxf(SVIEW.out_peak * 0.9f, peak);
}

static RAMC void view_update(void)
{
    int i, best = -1, k;
    uint32_t age = 0;
    for (i = 0; i < NVOICES; i++)
        if (V[i].active && V[i].age >= age) {
            age = V[i].age;
            best = i;
        }
    SVIEW.active = best >= 0;
    if (best < 0)
        return;
    for (k = 0; k < DST_N; k++)
        SVIEW.mods[k] = V[best].mods[k];
    for (k = 0; k < 3; k++)
        SVIEW.env[k] = V[best].env[k].val;
    for (k = 0; k < 2; k++) {
        SVIEW.lfo[k] = V[best].lfo[k].val;
        SVIEW.lfo_ph[k] = P.lfo[k].mode == LM_FREE ? lfo_free_ph[k] : V[best].lfo[k].ph;
        SVIEW.frame[k] = V[best].frame[k];
        SVIEW.warp[k] = V[best].warp[k];
    }
}

int synth_voices(void)
{
    int i, n = 0;
    for (i = 0; i < NVOICES; i++)
        n += V[i].active;
    return n;
}
int synth_last_note(void) { return last_note; }
float synth_peak(void)
{
    float p = peak;
    peak = 0.0f;
    return p;
}
uint32_t synth_shed_count(void) { return shed_count; }
