/* SPDX-License-Identifier: MPL-2.0 */
/* CHAIN-1's sound engine (an fm1-base o2 unit: -O2, its own compilation unit).
 *
 *   keys / MIDI / ARP -> voices (OSC -> FILTER -> AMP, filter envelope, LFO) -> MOD -> DELAY -> REVERB -> out
 *
 * Up to eight voices: POLY, MONO, LEGATO, or UNISON (the voices stacked on one note, detuned and spread).
 * Everything here runs in the audio interrupt except dsp_patch / dsp_key / dsp_arp / dsp_service and the bank's
 * functions, which the main loop calls: they write a patch slot, an event ring and flags the interrupt picks up at
 * its next block (dsp_service loads and unloads units, between blocks that leave their place alone).
 *
 * logue SDK units (logue/): a place (OSC, MOD, DELAY, REVERB) whose TYPE is past the built-in ones runs the unit in
 * that bank slot, on the second core, a block ahead (the oscillator) or a block behind (the effects): while any
 * effect is a unit the whole chain runs there, 5.8 ms late. A unit's memory is its place's arena, shared with the
 * built-in effect it replaces. Control-rate work is done every SUB frames. */
#include "plat.h"
/* fastmath.h's helpers are static inline; forced inline here, so none stays behind as a call into the flash */
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#undef inline
#include "dsp.h"
#include "logue/logue.h"

#define SR 44100.0f
#define ISR (1.0f / SR)
#define SUB 16u                                   /* control-rate period, frames */
#define MAXBLK 512u                               /* the platform's block (APP_BLOCK 256) with room */
#define RAMC PLAT_RAM_CODE
#define INL static inline __attribute__((always_inline))

INL uint32_t umod(uint32_t a, uint32_t b)
{
    while (a >= b)
        a -= b;
    return a;
}
INL float kn(uint16_t v) { return (float)(v > 1023u ? 1023u : v) * (1.0f / 1023.0f); }
INL float bip(uint16_t v) { return fm_clampf(((float)v - 512.0f) * (1.0f / 511.0f), -1.0f, 1.0f); }
INL float frac1(float x) { return x - fm_floorf(x); }
INL float st2r(float semis) { return fm_exp2f(semis * (1.0f / 12.0f)); }

static uint32_t rng = 0x12345678u;
INL uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
INL float rndf(void) { return (float)(int32_t)rnd() * (1.0f / 2147483648.0f); }

/* --------------------------------------------------------------------------------- main loop -> audio --- */
_Static_assert(sizeof(patch_t) % 4u == 0u && _Alignof(patch_t) >= 4u, "patch_t is copied by words");
static patch_t slot[2];
static volatile uint32_t slot_idx, slot_seq;
static patch_t P;                                  /* the interrupt's copy */
static patch_t PM;                                 /* the main loop's copy (dsp_service) */
static uint32_t p_seq = 0xFFFFFFFFu;

void dsp_patch(const patch_t *p)
{
    uint32_t n = slot_idx ^ 1u;
    slot[n] = *p;
    PM = *p;
    slot_idx = n;
    slot_seq++;
}

#define EVN 64u
static volatile uint32_t ev[EVN];
static volatile uint32_t ev_w, ev_r;
static void ev_put(uint32_t e)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVN)
        return;                                    /* full: dropped (64 keys in 5.8 ms) */
    ev[w % EVN] = e;
    ev_w = w + 1u;
}
void dsp_key(int note, int on, int vel)
{
    ev_put((on ? 0x100u : 0u) | ((uint32_t)note & 0x7Fu) | ((uint32_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel) << 16));
}

static volatile uint8_t arp_on_req, arp_latch_req, panic_req;
static volatile int32_t bend_in, wheel_in;
static volatile uint32_t shed_until;               /* ms (dsp_overload) */
static uint32_t now_ms;
void dsp_arp(int on, int latch)
{
    arp_on_req = (uint8_t)(on != 0);
    arp_latch_req = (uint8_t)(latch != 0);
}
void dsp_panic(void) { panic_req = 1; }
void dsp_midi_bend(int v) { bend_in = v; }
void dsp_midi_wheel(int v) { wheel_in = v; }
void dsp_overload(void) { shed_until = plat_ms() + 2000u; }

/* ------------------------------------------------------------------------------------------- voices --- */
enum { ES_IDLE, ES_ATT, ES_DEC, ES_REL };
typedef struct {
    int note;
    uint8_t gate, on;
    uint32_t order;                                /* when it started (the newest wins, the oldest is stolen) */
    float vel;                                     /* 0..1 */
    float pitch, target, offs, pan;                /* semitones (glide toward target), unison offset, -1..1 */
    float ph, sph, subph, modph, mprev, nz;        /* the oscillator */
    float aenv, fenv;
    uint8_t astage, fstage;
    float ic1[2], ic2[2], g, k, a1, a2, a3;        /* the filter */
    float amp;                                     /* the amp the last sample had (the next block ramps from it) */
} voice_t;
static voice_t V[NVOICE];
static uint32_t v_order;
static int held[16], held_vel[16], nheld;          /* keys held, in the order pressed (the last on top) */
static float riser;                                /* REVERB RISER: the swell since the last note */
static float last_pitch = 60.0f;                   /* where a glide starts */
static int mono_note = -1;                         /* MONO / LEGATO / UNISON: the note playing */
static float lfo_v, lfo_sh, lfo_ph, bend_st, wheel;
static uint8_t arp_active;

INL int max_voices(void)
{
    int n = P.voice_mode == VM_POLY ? (P.voices < 1u ? 1 : P.voices > NVOICE ? NVOICE : P.voices) : 1;
    if (P.osc_type >= OSC_N)
        n = 1;                                     /* a unit oscillator: one voice (the NTS-1's one) */
    if (shed_until && (int32_t)(now_ms - shed_until) < 0 && n > 4)
        n = 4;                                     /* after an overload, for two seconds */
    return n;
}
INL int unison_n(void)
{
    if (P.voice_mode != VM_UNISON || P.osc_type >= OSC_N)
        return 1;
    return P.voices < 2u ? 2 : P.voices > NVOICE ? NVOICE : P.voices;
}

static RAMC void voice_start(voice_t *v, int note, float vel, float from, float offs, float pan, int retrig)
{
    v->note = note;
    v->target = (float)note;
    v->pitch = P.glide > 4u ? from : (float)note;
    v->offs = offs;
    v->pan = pan;
    v->vel = vel;
    v->gate = 1;
    if (retrig || !v->on) {
        if (!v->on) {                              /* from silence: a clean start */
            v->ph = v->sph = v->subph = v->modph = 0.0f;
            v->mprev = 0.0f;
            v->aenv = v->fenv = 0.0f;
            v->amp = 0.0f;
            v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0.0f;
        }
        v->astage = ES_ATT;                        /* (from where the envelopes are: no click) */
        v->fstage = ES_ATT;
        v->order = ++v_order;
    }
    v->on = 1;
}

static int lg_osc_ev[8], lg_osc_nev;               /* note events for a unit oscillator: +note on, -1-note off */

static RAMC void note_on(int note, int vel127)
{
    float vel = (float)vel127 * (1.0f / 127.0f), from = last_pitch;
    int i, n;
    riser = 0.0f;
    if (P.voice_mode == VM_POLY && P.osc_type < OSC_N) {
        voice_t *best = 0;
        n = max_voices();
        for (i = 0; i < n && !best; i++)           /* the same note again: that voice */
            if (V[i].on && V[i].note == note)
                best = &V[i];
        for (i = 0; i < n && !best; i++)
            if (!V[i].on)
                best = &V[i];
        if (!best) {                               /* steal: the oldest let go, else the oldest */
            uint32_t o = 0xFFFFFFFFu;
            for (i = 0; i < n; i++)
                if (!V[i].gate && V[i].order < o)
                    o = V[i].order, best = &V[i];
            if (!best)
                for (i = 0; i < n; i++)
                    if (V[i].order < o)
                        o = V[i].order, best = &V[i];
        }
        voice_start(best, note, vel, from, 0.0f, 0.0f, 1);
    } else {
        int legato = P.voice_mode == VM_LEGATO && mono_note >= 0 && V[0].gate;
        n = unison_n();
        for (i = 0; i < n; i++) {
            float spread = n > 1 ? (float)i / (float)(n - 1) * 2.0f - 1.0f : 0.0f;
            voice_start(&V[i], note, vel, legato || P.voice_mode != VM_LEGATO ? (V[i].on ? V[i].pitch : from) : (float)note,
                        spread * kn(P.detune) * 0.5f, spread * 0.8f, !legato);
        }
        mono_note = note;
        if (P.osc_type >= OSC_N && lg_osc_nev < 8 && !legato)
            lg_osc_ev[lg_osc_nev++] = note;
    }
    last_pitch = (float)note;
}

static RAMC void release_voice(voice_t *v)
{
    v->gate = 0;
    if (v->astage != ES_IDLE && P.eg_type != EG_AR && P.eg_type != EG_OPEN)
        v->astage = ES_REL;
    if (v->fstage != ES_IDLE && P.feg_type != FEG_AD)
        v->fstage = ES_REL;
}

static RAMC void note_off(int note)
{
    int i;
    if (P.voice_mode == VM_POLY && P.osc_type < OSC_N) {
        for (i = 0; i < NVOICE; i++)
            if (V[i].on && V[i].gate && V[i].note == note)
                release_voice(&V[i]);
        return;
    }
    if (note != mono_note)
        return;
    if (nheld && !arp_active) {                    /* back to the last key still held, legato */
        int k = held[nheld - 1], n = unison_n();
        for (i = 0; i < n; i++)
            V[i].target = (float)k, V[i].note = k;
        mono_note = k;
        last_pitch = (float)k;
        return;
    }
    for (i = 0; i < NVOICE; i++)
        if (V[i].on && V[i].gate)
            release_voice(&V[i]);
    if (P.osc_type >= OSC_N && lg_osc_nev < 8)
        lg_osc_ev[lg_osc_nev++] = -1 - note;
    mono_note = -1;
}

static RAMC void held_remove(int note)
{
    int i, j;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note) {
            held_vel[j] = held_vel[i];
            held[j++] = held[i];
        }
    nheld = j;
}

static RAMC void key_event(int note, int on, int vel)
{
    held_remove(note);
    if (on) {
        if (nheld == 16)
            held_remove(held[0]);
        held_vel[nheld] = vel;
        held[nheld++] = note;
        if (!arp_active)
            note_on(note, vel);
    } else if (!arp_active) {
        note_off(note);
    }
}
void dsp_midi_note(int note, int vel)
{
    ev_put((vel > 0 ? 0x100u : 0u) | ((uint32_t)note & 0x7Fu) | ((uint32_t)(vel > 0 ? vel : 64) << 16));
}

/* ---------------------------------------------------------------------------------------- the arp --- */
static const int8_t CHORD[CHORD_N][4] = {{0, 12, -1, -1}, {0, 4, 7, -1}, {0, 5, 7, -1},
                                         {0, 4, 8, -1},   {0, 3, 7, -1}, {0, 3, 6, -1}};
static struct {
    int root, vel;                                 /* latched root (-1 none) */
    uint32_t pos, steps;
    float t, step_len, gate_len;                   /* frames into the step, frames per step, gate */
    int playing;                                   /* the note it holds, -1 none */
} A = {-1, 100, 0, 0, 0.0f, 0.0f, 0.0f, -1};

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
    case ARP_STOCH:
        idx = (int)umod(k, N);
        if ((rnd() & 3u) == 0u)
            idx = (int)(((rnd() >> 16) * N) >> 16);
        return notes[idx] + ((rnd() >> 24) < 52u ? 12 : 0);
    }
    return notes[idx];
}

static RAMC void arp_stop_note(void)
{
    if (A.playing >= 0) {
        note_off(A.playing);
        A.playing = -1;
    }
}

static RAMC void arp_frames(uint32_t frames)
{
    int on = arp_on_req, latch = arp_latch_req, notes[12], n = 0, o, c, root;
    if (!latch)
        A.root = -1;
    if (nheld) {
        A.root = held[nheld - 1];
        A.vel = held_vel[nheld - 1];
    }
    root = nheld ? held[nheld - 1] : (latch ? A.root : -1);
    if (!on || root < 0) {
        if (arp_active) {                          /* the arp let go: back to the keys */
            int i;
            arp_active = 0;
            arp_stop_note();
            for (i = 0; i < nheld; i++)
                note_on(held[i], held_vel[i]);
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
    if (!arp_active) {                             /* starts at once: the keys' own notes stop */
        int i;
        for (i = 0; i < NVOICE; i++)
            if (V[i].gate)
                release_voice(&V[i]);
        mono_note = -1;
        arp_active = 1;
        A.t = A.step_len;
        A.pos = 0;
    }
    A.t += (float)frames;
    {
        /* swing: the odd steps late */
        float len = A.step_len * ((A.pos & 1u) ? 1.0f - kn(P.arp_swing) * 0.5f : 1.0f + kn(P.arp_swing) * 0.5f);
        if (A.t >= len) {
            uint32_t L = P.arp_len < 1u ? 1u : P.arp_len > 24u ? 24u : P.arp_len;
            int note;
            A.t -= len;
            if (A.t >= len)
                A.t = 0.0f;
            note = arp_pick(A.pos, notes, n);
            if (++A.pos >= L)
                A.pos = 0;
            A.steps++;
            arp_stop_note();
            note = note < 0 ? 0 : note > 127 ? 127 : note;
            note_on(note, A.vel);
            A.playing = note;
            A.gate_len = len * (0.05f + 0.95f * kn(P.arp_gate));
        } else if (A.playing >= 0 && A.t >= A.gate_len && kn(P.arp_gate) < 0.99f) {
            arp_stop_note();
        }
    }
}

/* -------------------------------------------------------------------------------------- oscillator --- */
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

INL float osc_sample(voice_t *v, int type, float dt, float shape, float alt)
{
    float s, sub;
    int wrapped;
    v->ph += dt;
    wrapped = v->ph >= 1.0f;
    if (wrapped)
        v->ph -= 1.0f;
    switch (type) {
    default:
    case OSC_SAW: {                                /* SHAPE: hard sync, the slave at 1..4 x */
        float r = 1.0f + 3.0f * shape, sdt = dt * r;
        v->sph += sdt;
        if (wrapped)
            v->sph = v->ph * r;
        v->sph = frac1(v->sph);
        s = 2.0f * v->sph - 1.0f - blep(v->sph, sdt);
        break;
    }
    case OSC_TRI: {                                /* SHAPE: a sine wavefolder */
        float tri = 4.0f * fm_fabsf(v->ph - 0.5f) - 1.0f;
        s = fm_sin_turns(0.25f * tri * (1.0f + 4.0f * shape));
        break;
    }
    case OSC_SQR: {                                /* SHAPE: pulse width 50 -> 95 % */
        float pw = 0.5f + 0.45f * shape;
        s = (v->ph < pw ? 1.0f : -1.0f) + blep(v->ph, dt) - blep(frac1(v->ph + 1.0f - pw), dt);
        break;
    }
    case OSC_VPM: {                                /* SHAPE: index 0..6 rad; ALT: the modulator's ratio */
        float m;
        v->modph = frac1(v->modph + dt * VPM_RATIO[(int)(alt * 7.99f)]);
        m = fm_sin_turns(v->modph + 0.06f * shape * v->mprev);
        v->mprev = m;
        return fm_sin_turns(v->ph + 0.9549f * shape * m);   /* 6 rad / 2 pi = 0.95 turns */
    }
    case OSC_NOISE: {                              /* SHAPE: colour (a low-pass), ALT: pitched (a comb at the note) */
        float w = rndf(), c = 0.02f + 0.98f * (1.0f - shape) * (1.0f - shape);
        v->nz += c * (w - v->nz);
        s = v->nz * (1.0f + 1.5f * shape);
        if (alt > 0.0f)
            s = s * (1.0f - alt) + alt * s * (v->ph < 0.5f ? 1.0f : -1.0f);
        return s;
    }
    }
    /* ALT on the analog types: a square an octave down */
    v->subph += dt * 0.5f;
    if (v->subph >= 1.0f)
        v->subph -= 1.0f;
    sub = (v->subph < 0.5f ? 1.0f : -1.0f) + blep(v->subph, dt * 0.5f) - blep(frac1(v->subph + 0.5f), dt * 0.5f);
    return (s + alt * 0.8f * sub) * (1.0f / (1.0f + 0.5f * alt));
}

/* -------------------------------------------------------------------------------------- envelopes --- */
typedef struct { float ainc, dcoef, sus, rcoef; } egc_t;
static egc_t EGA, EGF;

static RAMC void eg_coef(egc_t *e, uint16_t a, uint16_t d, uint16_t s, uint16_t r)
{
    e->ainc = 1.0f / ((0.001f * fm_powf(10000.0f, kn(a))) * SR);
    e->dcoef = fm_expf(-4.6f / ((0.002f * fm_powf(5000.0f, kn(d))) * SR));
    e->sus = kn(s);
    e->rcoef = fm_expf(-6.9f / ((0.002f * fm_powf(5000.0f, kn(r))) * SR));
}

/* the amp envelope: ADSR, AR (attack, then the release whatever the gate), AR LOOP (again while held), OPEN */
INL float eg_amp(voice_t *v, int type)
{
    switch (v->astage) {
    case ES_ATT:
        v->aenv += EGA.ainc;
        if (v->aenv >= 1.0f) {
            v->aenv = 1.0f;
            v->astage = type == EG_ADSR || type == EG_OPEN ? ES_DEC : ES_REL;
        }
        break;
    case ES_DEC:
        if (type == EG_OPEN)
            v->aenv = 1.0f;
        else
            v->aenv = EGA.sus + (v->aenv - EGA.sus) * EGA.dcoef;
        if (!v->gate && type != EG_OPEN)
            v->astage = ES_REL;
        break;
    case ES_REL:
        v->aenv *= EGA.rcoef;
        if (v->aenv < 0.0002f) {
            v->aenv = 0.0f;
            v->astage = (type == EG_ARLOOP && v->gate) ? ES_ATT : ES_IDLE;
        }
        break;
    default:
        break;
    }
    return v->aenv;
}

/* the filter envelope: ADSR, AD (to 0 whatever the gate), LOOP (attack, decay to 0, again while held) */
INL float eg_filt(voice_t *v, int type)
{
    switch (v->fstage) {
    case ES_ATT:
        v->fenv += EGF.ainc;
        if (v->fenv >= 1.0f) {
            v->fenv = 1.0f;
            v->fstage = ES_DEC;
        }
        break;
    case ES_DEC: {
        float to = type == FEG_ADSR ? EGF.sus : 0.0f;
        v->fenv = to + (v->fenv - to) * EGF.dcoef;
        if (type == FEG_LOOP && v->fenv < 0.01f && v->gate)
            v->fstage = ES_ATT;
        else if (!v->gate && type == FEG_ADSR)
            v->fstage = ES_REL;
        break;
    }
    case ES_REL:
        v->fenv *= EGF.rcoef;
        if (v->fenv < 0.0002f) {
            v->fenv = 0.0f;
            v->fstage = ES_IDLE;
        }
        break;
    default:
        break;
    }
    return v->fenv;
}

/* ----------------------------------------------------------------------------------------- filter --- */
INL void filter_coef(voice_t *v, float cutoff_oct, float reso)
{
    float fc = 20.0f * fm_exp2f(fm_clampf(cutoff_oct, 0.0f, 9.95f));
    if (fc > 0.45f * SR)
        fc = 0.45f * SR;
    v->g = fm_tanf(3.14159265f * fc * ISR);
    v->k = 2.0f - 1.9f * reso;                      /* never self-oscillates */
    v->a1 = 1.0f / (1.0f + v->g * (v->g + v->k));
    v->a2 = v->g * v->a1;
    v->a3 = v->g * v->a2;
}

INL float svf(voice_t *v, int st, float x, int mode)  /* mode 0 low, 1 band, 2 high */
{
    float v3 = x - v->ic2[st], v1 = v->a1 * v->ic1[st] + v->a2 * v3, v2 = v->ic2[st] + v->a2 * v->ic1[st] + v->a3 * v3;
    v->ic1[st] = fm_flush(2.0f * v1 - v->ic1[st]);
    v->ic2[st] = fm_flush(2.0f * v2 - v->ic2[st]);
    return mode == 0 ? v2 : mode == 1 ? v1 * 1.5f : x - v->k * v1 - v2;
}

INL float filter_sample(voice_t *v, float x)
{
    int mode, poles;
    if (P.flt_type >= FLT_OFF)
        return x;
    mode = P.flt_type / 2;
    poles = P.flt_type & 1;
    x = fm_tanhf(x * 1.2f);
    x = svf(v, 0, x, mode);
    if (poles)
        x = svf(v, 1, x, mode);
    return x;
}

/* ------------------------------------------------------------------------- the places' memory --- */
/* each place's arena: the built-in effect's buffers, or a unit's memory (never both at once) */
#define AR_OSC (44u << 10)
#define AR_MOD (32u << 10)
#define AR_DLY (92u << 10)
#define AR_REV (88u << 10)
static uint8_t ar_osc[AR_OSC] PLAT_POOL __attribute__((aligned(8)));
static uint8_t ar_mod[AR_MOD] PLAT_POOL __attribute__((aligned(8)));
static uint8_t ar_dly[AR_DLY] PLAT_POOL __attribute__((aligned(8)));
static uint8_t ar_rev[AR_REV] PLAT_POOL __attribute__((aligned(8)));
static uint8_t *const AR[US_N] = {ar_osc, ar_mod, ar_dly, ar_rev};
static const uint32_t AR_SZ[US_N] = {AR_OSC, AR_MOD, AR_DLY, AR_REV};

/* who owns a place's arena: nobody (changing hands), the built-in effect, or a unit */
enum { OWN_NONE, OWN_BUILTIN, OWN_UNIT };
static struct {
    lg_unit_t u;
    volatile uint8_t own;                          /* written by the main loop, read by the render */
    int8_t slot;                                   /* the bank slot loaded (-1 none) */
    int8_t want;                                   /* (main loop) the slot the patch asks for */
    uint8_t step;                                  /* (main loop) 1: waiting for the render to let go */
    uint32_t at_block;
    int32_t applied[UPARAM];                       /* (render) the parameter values the unit has */
    uint32_t ticks;                                /* (render) its share of the last block, timer ticks */
} PL[US_N];
static volatile uint32_t blocks;                   /* render blocks so far */

/* -------------------------------------------------------------------------------------- effects --- */
#define MODLEN 2048u
#define modbuf ((float (*)[MODLEN])(void *)ar_mod)
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

static RAMC void mod_block(float *L, float *R, uint32_t n)
{
    float rate = 0.05f + 4.0f * kn(P.mod_time) * kn(P.mod_time), depth = kn(P.mod_depth), mix = kn(P.mod_mix);
    uint32_t j;
    int c;
    if (P.mod_type == MOD_TREMOLO)
        rate = 0.2f + 19.8f * kn(P.mod_time) * kn(P.mod_time);
    for (j = 0; j < n; j++) {
        float x[2] = {L[j], R[j]}, w[2], y[2];
        if ((j & (SUB - 1u)) == 0u && P.mod_type == MOD_PHASER) {
            float f = 250.0f * fm_exp2f(3.5f * (0.5f + 0.5f * fm_sin_turns(mod_lfo))), t = fm_tanf(3.14159265f * f * ISR);
            ph_a = (t - 1.0f) / (t + 1.0f);
        }
        mod_lfo = frac1(mod_lfo + rate * ISR);
        y[0] = x[0];
        y[1] = x[1];
        switch (P.mod_type) {
        case MOD_CHORUS:
            for (c = 0; c < 2; c++) {
                float d = (0.007f + 0.005f * depth * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.25f * (float)c))) * SR;
                modbuf[c][mod_w] = x[c];
                w[c] = rd(modbuf[c], MODLEN, mod_w, d);
                y[c] = 0.75f * x[c] + 0.65f * w[c];
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
            y[0] = 0.6f * x[0] + 0.5f * (t[0] + 0.5f * t[1]);
            y[1] = 0.6f * x[1] + 0.5f * (t[2] + 0.5f * t[1]);
            break;
        }
        case MOD_FLANGER:
            for (c = 0; c < 2; c++) {
                float d = (0.0005f + 0.004f * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.25f * (float)c))) * SR;
                modbuf[c][mod_w] = x[c] + 0.75f * depth * mod_fb[c];
                w[c] = rd(modbuf[c], MODLEN, mod_w, d);
                mod_fb[c] = w[c];
                y[c] = 0.7f * (x[c] + w[c]);
            }
            break;
        case MOD_PHASER:
            for (c = 0; c < 2; c++) {
                float z = x[c] + 0.6f * depth * ph_fb[c];
                int s;
                for (s = 0; s < 6; s++) {          /* first-order allpasses, all at ph_a */
                    float o = ph_a * z + ph_ap[c][s];
                    ph_ap[c][s] = fm_flush(z - ph_a * o);
                    z = o;
                }
                ph_fb[c] = z;
                y[c] = 0.7f * (x[c] + z);
            }
            break;
        case MOD_TREMOLO: {                        /* the two sides a little apart */
            float g0 = 1.0f - depth * (0.5f + 0.5f * fm_sin_turns(mod_lfo)), g1 = 1.0f - depth * (0.5f + 0.5f * fm_sin_turns(mod_lfo + 0.1f));
            y[0] = x[0] * g0;
            y[1] = x[1] * g1;
            break;
        }
        default:
            break;
        }
        mod_w = (mod_w + 1u) & (MODLEN - 1u);
        L[j] = x[0] + (y[0] - x[0]) * mix;
        R[j] = x[1] + (y[1] - x[1]) * mix;
    }
}

#define DLEN 22528u                                /* 0.51 s at 44.1 kHz */
#define dbuf ((int16_t (*)[DLEN])(void *)ar_dly)
static uint32_t d_w;
static float d_time, d_hp[2], d_lp[2], d_tone[2], d_wow, d_flut;

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

static RAMC void delay_block(float *L, float *R, uint32_t n)
{
    float t_target = (0.01f + 0.49f * kn(P.dly_time)) * SR, fb = 0.95f * kn(P.dly_depth), mix = kn(P.dly_mix);
    float tone = 0.08f + 0.92f * (1.0f - kn(P.dly_tone) * 0.9f);   /* the repeats darker as TONE goes up */
    uint32_t j;
    for (j = 0; j < n; j++) {
        float wl, wr, inl = L[j], inr = R[j], d;
        d_time += (t_target - d_time) * 0.0004f;   /* the time glides (a tape-like pitch bend) */
        d = d_time;
        switch (P.dly_type) {
        case DLY_MONO:
            wl = wr = d_rd(0, d);
            break;
        case DLY_PINGPONG:
            wl = d_rd(0, d);
            wr = d_rd(1, d);
            break;
        case DLY_HIPASS:
            wl = d_rd(0, d);
            wr = d_rd(1, d * 1.02f);
            d_hp[0] += 0.095f * (wl - d_hp[0]);
            d_hp[1] += 0.095f * (wr - d_hp[1]);
            wl -= d_hp[0];
            wr -= d_hp[1];
            break;
        case DLY_TAPE:
            d_wow = frac1(d_wow + 0.55f * ISR);
            d_flut = frac1(d_flut + 7.1f * ISR);
            d *= 1.0f + 0.004f * fm_sin_turns(d_wow) + 0.0008f * fm_sin_turns(d_flut);
            wl = d_rd(0, d);
            wr = d_rd(1, d * 1.01f);
            d_lp[0] += 0.3f * (wl - d_lp[0]);
            d_lp[1] += 0.3f * (wr - d_lp[1]);
            wl = d_lp[0];
            wr = d_lp[1];
            break;
        default:                                   /* STEREO */
            wl = d_rd(0, d);
            wr = d_rd(1, d);
            break;
        }
        d_tone[0] += tone * (wl - d_tone[0]);
        d_tone[1] += tone * (wr - d_tone[1]);
        wl = d_tone[0] = fm_flush(d_tone[0]);
        wr = d_tone[1] = fm_flush(d_tone[1]);
        switch (P.dly_type) {
        case DLY_MONO: dbuf[0][d_w] = d_q((inl + inr) * 0.5f + fb * wl); break;
        case DLY_PINGPONG:
            dbuf[0][d_w] = d_q((inl + inr) * 0.5f + fb * wr);
            dbuf[1][d_w] = d_q(fb * wl);
            break;
        case DLY_TAPE:
            dbuf[0][d_w] = d_q(fm_tanhf(inl + fb * 1.05f * wl));
            dbuf[1][d_w] = d_q(fm_tanhf(inr + fb * 1.05f * wr));
            break;
        default:
            dbuf[0][d_w] = d_q(inl + fb * wl);
            dbuf[1][d_w] = d_q(inr + fb * wr);
            break;
        }
        if (++d_w >= DLEN)
            d_w = 0;
        L[j] = inl * (1.0f - mix) + wl * mix;
        R[j] = inr * (1.0f - mix) + wr * mix;
    }
}

/* reverb: four allpass diffusers, then a 4-line feedback delay network (Hadamard), a low-pass in each line */
#define RV_MAX 3700u
#define AP_MAX 640u
#define rv_line ((float (*)[RV_MAX])(void *)ar_rev)
#define rv_ap ((float (*)[AP_MAX])(void *)(ar_rev + 4u * RV_MAX * 4u))
static const uint16_t RV_LEN[4] = {1687, 1601, 2053, 2251};
static const uint16_t AP_LEN[4] = {142, 107, 379, 277};
static uint32_t rv_n[4], ap_n[4], rv_i[4], ap_i[4];
static float rv_g[4], rv_lp[4], rv_damp, rv_lfo, rv_lpl, rv_lpr;

static RAMC void reverb_size(void)
{
    static const float SCALE[REV_N] = {1.0f, 1.0f, 0.62f, 1.6f, 1.0f, 1.25f};
    float s = SCALE[P.rev_type < REV_N ? P.rev_type : 0];
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

static RAMC void reverb_block(float *L, float *R, uint32_t n)
{
    float rt = 0.3f + 9.7f * kn(P.rev_time) * kn(P.rev_time), mix = kn(P.rev_mix);
    uint32_t j;
    int i;
    for (i = 0; i < 4; i++)                        /* -60 dB in rt seconds */
        rv_g[i] = fm_exp2f(-9.9658f * (float)rv_n[i] / (SR * rt));
    rv_damp = 0.05f + 0.85f * kn(P.rev_depth);
    if (P.rev_type == REV_PLATE)
        rv_damp *= 0.5f;
    if (P.rev_type == REV_SUBMARINE && rv_damp < 0.6f)
        rv_damp = 0.6f;
    for (j = 0; j < n; j++) {
        float x = (L[j] + R[j]) * 0.35f, o[4], f[4], h[4], wl, wr;
        for (i = 0; i < 4; i++) {                  /* the diffusers */
            float *b = rv_ap[i], d = b[ap_i[i]], y = d - 0.6f * x;
            b[ap_i[i]] = fm_flush(x + 0.6f * y);
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
        if (P.rev_type == REV_RISER) {             /* swells and brightens after each note */
            float c = 0.02f + 0.9f * riser;
            rv_lpl += c * (wl - rv_lpl);
            rv_lpr += c * (wr - rv_lpr);
            wl = rv_lpl * riser * 1.6f;
            wr = rv_lpr * riser * 1.6f;
        } else if (P.rev_type == REV_SUBMARINE) {  /* murky and slowly moving: a swept low-pass */
            float c;
            rv_lfo = frac1(rv_lfo + 0.18f * ISR);
            c = 0.012f + 0.03f * (0.5f + 0.5f * fm_sin_turns(rv_lfo));
            rv_lpl += c * (wl - rv_lpl);
            rv_lpr += c * (wr - rv_lpr);
            wl = rv_lpl * 2.2f;
            wr = rv_lpr * 2.2f;
        }
        L[j] = L[j] * (1.0f - mix) + wl * mix;
        R[j] = R[j] * (1.0f - mix) + wr * mix;
    }
}

static RAMC void zero(uint8_t *p, uint32_t n)
{
    uint32_t *w = (uint32_t *)(void *)p, i;
    for (i = 0; i < n / 4u; i++)
        w[i] = 0;
}

static RAMC void clear_tails(int mod, int dly, int rev)
{
    uint32_t i;
    if (mod) {
        zero(ar_mod, 2u * MODLEN * 4u);
        mod_fb[0] = mod_fb[1] = ph_fb[0] = ph_fb[1] = 0.0f;
        for (i = 0; i < 6u; i++)
            ph_ap[0][i] = ph_ap[1][i] = 0.0f;
    }
    if (dly) {
        zero(ar_dly, 2u * DLEN * 2u);
        d_tone[0] = d_tone[1] = 0.0f;
    }
    if (rev) {
        zero(ar_rev, 4u * RV_MAX * 4u + 4u * AP_MAX * 4u);
        for (i = 0; i < 4u; i++)
            rv_lp[i] = 0.0f;
        rv_lpl = rv_lpr = 0.0f;
    }
}

/* ------------------------------------------------------------------------------------------ units --- */
/* a unit effect on a block, LG_FRAMES at a time (interleaved for it) */
static RAMC void unit_fx(int place, float *L, float *R, uint32_t n)
{
    float buf[2u * LG_FRAMES];
    uint32_t o, j, t0 = plat_ticks24();
    lg_unit_t *u = &PL[place].u;
    for (o = 0; o < n; o += LG_FRAMES) {
        uint32_t m = n - o < LG_FRAMES ? n - o : LG_FRAMES;
        for (j = 0; j < m; j++) {
            buf[2u * j] = L[o + j];
            buf[2u * j + 1u] = R[o + j];
        }
        lg_fx(u, buf, m);
        for (j = 0; j < m; j++) {
            L[o + j] = buf[2u * j];
            R[o + j] = buf[2u * j + 1u];
        }
    }
    PL[place].ticks += plat_ticks24() - t0;
}

static RAMC int place_unit(int place) { return PL[place].own == OWN_UNIT && PL[place].u.state == LG_READY; }
static RAMC int place_builtin(int place) { return PL[place].own == OWN_BUILTIN; }

/* the chain on a block: each place its built-in effect, its unit, or nothing (changing hands) */
static RAMC void fx_chain(float *L, float *R, uint32_t n)
{
    if (P.mod_type >= MOD_N ? place_unit(US_MOD) : (P.mod_type != MOD_OFF && place_builtin(US_MOD))) {
        if (P.mod_type >= MOD_N)
            unit_fx(US_MOD, L, R, n);
        else
            mod_block(L, R, n);
    }
    if (P.dly_type >= DLY_N ? place_unit(US_DLY) : (P.dly_type != DLY_OFF && place_builtin(US_DLY))) {
        if (P.dly_type >= DLY_N)
            unit_fx(US_DLY, L, R, n);
        else
            delay_block(L, R, n);
    }
    if (P.rev_type >= REV_N ? place_unit(US_REV) : (P.rev_type != REV_OFF && place_builtin(US_REV))) {
        if (P.rev_type >= REV_N)
            unit_fx(US_REV, L, R, n);
        else
            reverb_block(L, R, n);
    }
}

/* the units' parameters as the patch has them (the render's side: the unit is used here only) */
static RAMC void unit_params(int place)
{
    lg_unit_t *u = &PL[place].u;
    int i;
    for (i = 0; i < u->nparam && i < UPARAM; i++) {
        int32_t want = place == US_OSC && i < 2 ? (int32_t)(i ? P.alt : P.shape) : (int32_t)P.up[place][i];
        if (want != PL[place].applied[i]) {
            PL[place].applied[i] = want;
            lg_param(u, i, want);
        }
    }
}

/* the second core's job: the unit oscillator for the next block, the chain on the last one (when units are in it) */
static float osc_buf[2][MAXBLK];                   /* [0] this block's unit oscillator, [1] the next one's */
static float mixa[2][MAXBLK], mixb[2][MAXBLK];     /* the voices' mix: this block's, and the last one's */
static float (*mix_cur)[MAXBLK] = mixa, (*mix_prev)[MAXBLK] = mixb;
static uint32_t job_n, job_prev_n, job_ticks;
static int job_note, job_fine, job_fx;
static float job_lfo;
static int job_ev[8], job_nev;
static uint32_t slow_count;

static RAMC void job(uint32_t arg)
{
    uint32_t t0 = plat_ticks24(), o, i;
    int p;
    (void)arg;
    for (p = 0; p < US_N; p++) {
        PL[p].ticks = 0;
        if (place_unit(p))
            unit_params(p);
    }
    if (P.osc_type >= OSC_N && place_unit(US_OSC)) {
        lg_unit_t *u = &PL[US_OSC].u;
        uint32_t t1 = plat_ticks24();
        for (i = 0; i < (uint32_t)job_nev; i++)
            lg_osc_note(u, job_ev[i] >= 0, job_ev[i] >= 0 ? job_ev[i] : -1 - job_ev[i], 100);
        for (o = 0; o < job_n; o += LG_FRAMES) {
            uint32_t m = job_n - o < LG_FRAMES ? job_n - o : LG_FRAMES;
            lg_osc(u, osc_buf[1] + o, m, job_note, (uint32_t)job_fine, job_lfo);
        }
        PL[US_OSC].ticks += plat_ticks24() - t1;
    }
    if (job_fx)                                    /* the last block's mix, through the chain, in place */
        fx_chain(mix_prev[0], mix_prev[1], job_prev_n);
    job_ticks = plat_ticks24() - t0;
}

/* --------------------------------------------------------------------------------------- render --- */
static float peak;
#define SCOPE_N 512u
static float scope[SCOPE_N];
static volatile uint32_t scope_w;
static float vu;
static uint8_t last_mod = 0xFF, last_dly = 0xFF, last_rev = 0xFF;
static uint8_t fx_was_on;                          /* the chain was on the second core last block */

void dsp_init(void)
{
    int i;
    for (i = 0; i < NVOICE; i++) {
        V[i].note = -1;
        V[i].k = 2.0f;
    }
    for (i = 0; i < US_N; i++) {
        PL[i].own = i == US_OSC ? OWN_NONE : OWN_BUILTIN;
        PL[i].slot = PL[i].want = -1;
    }
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
        if (P.mod_type != last_mod && last_mod == MOD_OFF && place_builtin(US_MOD))
            clear_tails(1, 0, 0);
        if (P.dly_type != last_dly && last_dly == DLY_OFF && place_builtin(US_DLY))
            clear_tails(0, 1, 0);
        if (P.rev_type != last_rev) {
            if (last_rev == REV_OFF && place_builtin(US_REV))
                clear_tails(0, 0, 1);
            reverb_size();
        }
        last_mod = P.mod_type;
        last_dly = P.dly_type;
        last_rev = P.rev_type;
        eg_coef(&EGA, P.a, P.d, P.s, P.r);
        eg_coef(&EGF, P.fa, P.fd, P.fs, P.fr);
    }
    if (panic_req) {
        int i;
        panic_req = 0;
        nheld = 0;
        A.root = -1;
        A.playing = -1;
        arp_active = 0;
        mono_note = -1;
        for (i = 0; i < NVOICE; i++) {
            V[i].gate = V[i].on = 0;
            V[i].astage = V[i].fstage = ES_IDLE;
            V[i].aenv = V[i].fenv = V[i].amp = 0.0f;
        }
        if (P.osc_type >= OSC_N && lg_osc_nev < 8)
            lg_osc_ev[lg_osc_nev++] = -1 - 0;
        clear_tails(place_builtin(US_MOD), place_builtin(US_DLY), place_builtin(US_REV));
    }
    while (ev_r != ev_w) {
        uint32_t e = ev[ev_r % EVN];
        ev_r++;
        key_event((int)(e & 0x7Fu), (int)(e >> 8 & 1u), (int)(e >> 16 & 0x7Fu));
    }
}

static RAMC float lfo_wave(float ph)
{
    switch (P.lfo_type) {
    case LFO_SINE: return fm_sin_turns(ph);
    case LFO_SAW: return 1.0f - 2.0f * ph;
    case LFO_SQR: return ph < 0.5f ? 1.0f : -1.0f;
    case LFO_SH: case LFO_DRIFT: return lfo_sh;
    default: return 4.0f * fm_fabsf(ph - 0.5f) - 1.0f;
    }
}

/* one voice for m frames (at most SUB), added into L, R */
static RAMC void voice_run(voice_t *v, float *L, float *R, uint32_t m, int type, const float *ext)
{
    float vsens = kn(P.velo), vg = 1.0f - vsens + vsens * v->vel * v->vel;
    float glide = P.glide > 4u ? fm_expf(-(float)m / ((0.002f + 2.0f * kn(P.glide) * kn(P.glide)) * SR)) : 0.0f;
    float pitch, dt, shape, cut, fe, amp0, amp1, gl, gr;
    uint32_t j;
    v->pitch = v->target + (v->pitch - v->target) * glide;
    pitch = v->pitch + v->offs + bip(P.tune) * 24.0f + bend_st
          + lfo_v * (kn(P.lfo_pitch) * kn(P.lfo_pitch) * 12.0f + wheel * 0.7f);
    dt = 440.0f * st2r(pitch - 69.0f) * ISR;
    if (dt > 0.45f)
        dt = 0.45f;
    shape = fm_clampf(kn(P.shape) + lfo_v * kn(P.lfo_shape) * 0.5f, 0.0f, 1.0f);
    fe = v->fenv;
    cut = kn(P.cutoff) * 9.8f + bip(P.feg_amt) * fe * 8.0f * (1.0f - vsens * 0.5f + vsens * 0.5f * v->vel)
        + kn(P.keytrk) * (v->pitch - 60.0f) * (1.0f / 12.0f) + lfo_v * kn(P.lfo_cut) * 3.0f;
    filter_coef(v, cut, kn(P.reso));
    amp0 = v->amp;
    gl = v->pan < 0.0f ? 1.0f : 1.0f - v->pan;
    gr = v->pan > 0.0f ? 1.0f : 1.0f + v->pan;
    for (j = 0; j < m; j++) {
        float s = ext ? ext[j] : osc_sample(v, type, dt, shape, kn(P.alt));
        float nz;
        if (P.noise > 4u) {
            nz = rndf();
            s += kn(P.noise) * (nz - s * 0.3f);
        }
        s = filter_sample(v, s);
        eg_filt(v, P.feg_type);
        amp1 = eg_amp(v, P.eg_type) * vg;
        s *= amp1 * 0.6f;
        L[j] += s * gl;
        R[j] += s * gr;
    }
    v->amp = amp1;
    (void)amp0;
    if (v->astage == ES_IDLE && P.eg_type != EG_OPEN) {
        v->on = 0;
        v->aenv = 0.0f;
    }
    if (P.eg_type == EG_OPEN && v->astage == ES_IDLE)
        v->astage = ES_ATT;
}

RAMC void dsp_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, j;
    int handed, units_osc, units_fx, k;
    now_ms = plat_ms();
    take_input();
    blocks++;
    if (p_seq == 0xFFFFFFFFu || n > MAXBLK) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    bend_st = (float)bend_in * (1.0f / 8192.0f) * (float)(P.bend > 12u ? 12u : P.bend);
    wheel = (float)wheel_in * (1.0f / 127.0f);
    units_osc = P.osc_type >= OSC_N && place_unit(US_OSC);
    units_fx = (P.mod_type >= MOD_N && place_unit(US_MOD)) || (P.dly_type >= DLY_N && place_unit(US_DLY))
            || (P.rev_type >= REV_N && place_unit(US_REV));
    /* the arp and the controls for this block, then the second core's job (it reads what they set) */
    for (i = 0; i < n; i += SUB)
        arp_frames(n - i < SUB ? n - i : SUB);
    {
        float rate = 0.02f + 30.0f * kn(P.lfo_rate) * kn(P.lfo_rate), ph = lfo_ph + rate * (float)n * ISR;
        if (ph >= 1.0f) {
            ph -= fm_floorf(ph);
            lfo_sh = P.lfo_type == LFO_DRIFT ? fm_clampf(lfo_sh + 0.4f * rndf(), -1.0f, 1.0f) : rndf();
        }
        lfo_ph = ph;
        lfo_v = lfo_wave(lfo_ph);
    }
    riser += (1.0f - riser) * (float)n * ISR * 0.6f;
    job_n = n;
    job_fx = units_fx && fx_was_on && job_prev_n == n;   /* (a block's mix waits one block for the chain) */
    job_lfo = lfo_v * kn(P.lfo_shape);
    {
        float p = V[0].pitch + bip(P.tune) * 24.0f + bend_st + lfo_v * kn(P.lfo_pitch) * kn(P.lfo_pitch) * 12.0f;
        p = fm_clampf(p, 0.0f, 127.0f);
        job_note = (int)p;
        job_fine = (int)((p - (float)job_note) * 255.0f);
    }
    for (k = 0; k < lg_osc_nev; k++)
        job_ev[k] = lg_osc_ev[k];
    job_nev = lg_osc_nev;
    lg_osc_nev = 0;
    if (!units_osc && !units_fx) {
        handed = 0;
        job_ticks = 0;
        for (k = 0; k < US_N; k++)
            PL[k].ticks = 0;
    } else {
        handed = plat_cpu2_run(job, 0);
    }
    /* the voices, into this block's mix (the job has the last one's) */
    for (i = 0; i < n; i++)
        mix_cur[0][i] = mix_cur[1][i] = 0.0f;
    for (i = 0; i < n; i += SUB) {
        uint32_t m = n - i < SUB ? n - i : SUB;
        for (k = 0; k < NVOICE; k++)
            if (V[k].on)
                voice_run(&V[k], mix_cur[0] + i, mix_cur[1] + i, m, P.osc_type < OSC_N ? P.osc_type : 0,
                          units_osc && k == 0 ? osc_buf[0] + i : 0);
    }
    if (units_osc || units_fx) {
        if (!handed || plat_cpu2_wait() < 0)
            job(0);
        for (i = 0; i < n; i++)                    /* the unit oscillator: the next block's becomes this one's */
            osc_buf[0][i] = osc_buf[1][i];
        /* a CPU guard: the job over 85 % of the block three times running stops the heaviest unit */
        if (job_ticks > (uint32_t)((float)n * ISR * 1e6f * (float)PLAT_TICKS_PER_US * 0.85f)) {
            if (++slow_count >= 3u) {
                int worst = -1;
                uint32_t wt = 0;
                for (k = 0; k < US_N; k++)
                    if (place_unit(k) && PL[k].ticks > wt)
                        wt = PL[k].ticks, worst = k;
                if (worst >= 0) {
                    PL[worst].u.state = LG_FAULT;
                    PL[worst].u.err = LG_E_SLOW;
                }
                slow_count = 0;
            }
        } else {
            slow_count = 0;
        }
    }
    /* the chain: here on this block, or the second core's on the last one (5.8 ms behind) */
    {
        float (*out)[MAXBLK] = mix_cur;
        if (units_fx) {
            out = mix_prev;
            if (!job_fx)                           /* (its first block: nothing has come through yet) */
                for (i = 0; i < n; i++)
                    out[0][i] = out[1][i] = 0.0f;
        } else {
            fx_chain(mix_cur[0], mix_cur[1], n);
        }
        for (j = 0; j < n; j++) {
            float l = fm_tanhf(out[0][j]), r = fm_tanhf(out[1][j]);
            if (fm_fabsf(l) > peak)
                peak = fm_fabsf(l);
            scope[scope_w & (SCOPE_N - 1u)] = l;
            scope_w++;
            vu = fm_maxf(vu * 0.99995f, fm_fabsf(l));
            lr[2u * j] = (int32_t)(l * gain);
            lr[2u * j + 1u] = (int32_t)(r * gain);
        }
    }
    if (units_fx) {                                /* this block's mix goes through the chain next time */
        float (*t)[MAXBLK] = mix_prev;
        mix_prev = mix_cur;
        mix_cur = t;
        job_prev_n = n;
    }
    fx_was_on = (uint8_t)units_fx;
}

/* ---------------------------------------------------------------------------------- the bank --- */
#define BANK_LO 0x97000u                           /* the data area; the store's own sectors start at 0xD8000 */
#define BANK_SLOT 0x9000u                          /* 36 KiB a slot */
static unit_info_t BANKI[NUNITS];

const unit_info_t *dsp_unit(int s) { return s >= 0 && s < NUNITS ? &BANKI[s] : 0; }

int dsp_units_scan(void)
{
    int s, n = 0;
    for (s = 0; s < NUNITS; s++) {
        const uint8_t *img = plat_data(BANK_LO + (uint32_t)s * BANK_SLOT);
        unit_info_t *b = &BANKI[s];
        lg_info_t li;
        b->used = 0;
        if (!img || lg_check(img, BANK_SLOT) || lg_peek(img, &li))
            continue;
        b->used = 1;
        b->module = li.module;
        b->fmt = li.fmt;
        b->crc = li.crc;
        b->nparam = li.nparam;
        {
            int i, k;
            for (k = 0; k < 21; k++)
                b->name[k] = li.name[k];
            for (i = 0; i < li.nparam && i < UPARAM; i++) {
                for (k = 0; k < 22; k++)
                    b->param[i].name[k] = li.param[i].name[k];
                b->param[i].min = li.param[i].min;
                b->param[i].max = li.param[i].max;
                b->param[i].init = li.param[i].init;
                b->param[i].type = li.param[i].type;
                b->param[i].frac = li.param[i].frac;
            }
        }
        n++;
    }
    return n;
}

int dsp_unit_erase(int s)
{
    uint32_t off;
    int p;
    if (s < 0 || s >= NUNITS)
        return -1;
    for (p = 0; p < US_N; p++)                     /* a place playing it lets go first */
        if (PL[p].slot == s) {
            PL[p].own = OWN_NONE;
            PL[p].slot = -1;
            PL[p].u.state = LG_EMPTY;
        }
    for (off = 0; off < BANK_SLOT; off += 0x1000u)
        if (plat_data_erase(BANK_LO + (uint32_t)s * BANK_SLOT + off))
            return -2;
    dsp_units_scan();
    return 0;
}

int dsp_unit_put(int s, const uint8_t *img, uint32_t len)
{
    uint32_t off;
    if (s < 0 || s >= NUNITS || len > BANK_SLOT || lg_check(img, len))
        return -1;
    if (dsp_unit_erase(s))
        return -2;
    for (off = 0; off < len; off += 256u) {
        uint32_t k = len - off < 256u ? len - off : 256u;
        uint8_t buf[256];
        uint32_t i;
        for (i = 0; i < k; i++)
            buf[i] = img[off + i];
        if (plat_data_write(BANK_LO + (uint32_t)s * BANK_SLOT + off, buf, k))
            return -3;
    }
    dsp_units_scan();
    return BANKI[s].used ? 0 : -4;
}

/* --------------------------------------------------------------------- the places, main loop --- */
static int want_slot(int place)
{
    int t = place == US_OSC ? PM.osc_type - OSC_N : place == US_MOD ? PM.mod_type - MOD_N
          : place == US_DLY ? PM.dly_type - DLY_N : PM.rev_type - REV_N;
    return t >= 0 && t < NUNITS && BANKI[t].used ? t : -1;
}

void dsp_service(void)
{
    int p;
    for (p = 0; p < US_N; p++) {
        int want = want_slot(p);
        if (want == PL[p].slot && PL[p].step == 0 &&
            PL[p].own == (want >= 0 ? OWN_UNIT : (p == US_OSC ? OWN_NONE : OWN_BUILTIN)))
            continue;
        if (PL[p].step == 0) {                     /* the render lets go of the place first */
            PL[p].own = OWN_NONE;
            PL[p].step = 1;
            PL[p].at_block = blocks;
            continue;
        }
        if (blocks - PL[p].at_block < 2u)          /* a block may still be using it */
            continue;
        PL[p].step = 0;
        PL[p].slot = (int8_t)want;
        if (want >= 0) {
            int i;
            const uint8_t *img = plat_data(BANK_LO + (uint32_t)want * BANK_SLOT);
            for (i = 0; i < UPARAM; i++)
                PL[p].applied[i] = -32768;
            if (PLAT_FAR(lg_load)(&PL[p].u, img, AR[p], AR_SZ[p]) == LG_E_NONE)
                PL[p].own = OWN_UNIT;
            else
                PL[p].own = OWN_NONE;              /* (its error stays for the screen) */
        } else {
            PL[p].u.state = LG_EMPTY;
            if (p != US_OSC) {
                PLAT_FAR(zero)(AR[p], AR_SZ[p]);
                if (p == US_REV) {
                    int i;
                    for (i = 0; i < 4; i++)
                        rv_lp[i] = 0.0f;
                }
                PL[p].own = OWN_BUILTIN;
            }
        }
    }
}

int dsp_place_slot(int p) { return p >= 0 && p < US_N ? PL[p].slot : -1; }
int dsp_place_state(int p, int *err)
{
    if (p < 0 || p >= US_N)
        return LG_EMPTY;
    if (err)
        *err = PL[p].u.err;
    if (PL[p].step)
        return -1;                                 /* loading */
    return PL[p].slot < 0 ? LG_EMPTY : PL[p].u.state;
}
int dsp_place_load(int p)
{
    float blk = 256.0f * ISR * 1e6f * (float)PLAT_TICKS_PER_US;
    return p >= 0 && p < US_N ? (int)((float)PL[p].ticks * 100.0f / blk) : 0;
}
int dsp_core2_load(void) { return (int)((float)job_ticks * 100.0f / (256.0f * ISR * 1e6f * (float)PLAT_TICKS_PER_US)); }

/* ------------------------------------------------------------------------------------- readings --- */
int dsp_note(void)
{
    uint32_t o = 0;
    int k, note = -1;
    for (k = 0; k < NVOICE; k++)
        if (V[k].on && V[k].gate && V[k].order >= o)
            o = V[k].order, note = V[k].note;
    return note;
}
uint32_t dsp_voices_on(void)
{
    uint32_t b = 0;
    int k;
    for (k = 0; k < NVOICE; k++)
        if (V[k].on)
            b |= 1u << k;
    return b;
}
int dsp_voice_note(int v) { return v >= 0 && v < NVOICE && V[v].on && V[v].gate ? V[v].note : -1; }
float dsp_voice_level(int v) { return v >= 0 && v < NVOICE && V[v].on ? V[v].aenv : 0.0f; }
static int newest(void)
{
    uint32_t o = 0;
    int k, w = 0;
    for (k = 0; k < NVOICE; k++)
        if (V[k].on && V[k].order >= o)
            o = V[k].order, w = k;
    return w;
}
float dsp_level(void) { return V[newest()].aenv; }
float dsp_fenv(void) { return V[newest()].fenv; }
float dsp_lfo(void) { return lfo_v; }
float dsp_vu(void) { return vu; }
int dsp_scope(float *out, uint32_t n)
{
    uint32_t w = scope_w, i, start = w - n, best = start, k;
    for (k = 0; k < SCOPE_N - n - 1u; k++) {
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
uint32_t dsp_arp_steps(void) { return A.steps; }
uint32_t dsp_arp_step(void) { return A.pos; }
float dsp_peak(void)
{
    float p = peak;
    peak = 0.0f;
    return p;
}
