/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4's engine (an fm1-base o2 unit, running from RAM on the FM-1). See engine.h.
 * Per block of SUB frames: the tape moves and fires its events, the keys come in, each track's LFO and the
 * voices' controls are worked out; then each voice renders SUB frames into its track's buffer, each track runs
 * its effect and goes to the bus with its level and pan, and the bus runs the master delay and reverb. */
#include "plat.h"
#define inline inline __attribute__((always_inline))
#include "fastmath.h"
#undef inline
#include "engine.h"

#define SR 44100.0f
#define ISR (1.0f / SR)
#define SUB 16u
#define RAMC PLAT_RAM_CODE
#define NOIN __attribute__((noinline))
#define INL static inline __attribute__((always_inline))

INL float kn(uint16_t v) { return (float)(v > 1023u ? 1023u : v) * (1.0f / 1023.0f); }
INL float frac1(float x) { return x - fm_floorf(x); }
INL float lerp(float a, float b, float t) { return a + (b - a) * t; }
static uint32_t rng = 0x1F123BB5u;
INL uint32_t rndu(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
INL float rndf(void) { return (float)(int32_t)rndu() * (1.0f / 2147483648.0f); }
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
INL float note_hz(float n) { return 440.0f * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }

volatile eng_view_t VIEW;

/* ------------------------------------------------------- main loop -> audio --- */
_Static_assert(sizeof(proj_t) % 4u == 0u, "proj_t is copied by words");
static proj_t slot[2];
static volatile uint32_t slot_idx, slot_seq;
static proj_t P;
static uint32_t p_seq = 0xFFFFFFFFu;
static tape_t *T;

void eng_project(const proj_t *p)
{
    uint32_t n = slot_idx ^ 1u;
    slot[n] = *p;
    slot_idx = n;
    slot_seq++;
}

#define EVN 64u
static volatile uint32_t kev[EVN];
static volatile uint32_t kev_w, kev_r;
void eng_key(int track, int note, int vel)
{
    uint32_t w = kev_w;
    if (w - kev_r >= EVN)
        return;
    kev[w % EVN] = ((uint32_t)track & 3u) << 16 | ((uint32_t)vel & 0x7Fu) << 8 | ((uint32_t)note & 0x7Fu);
    kev_w = w + 1u;
}
static volatile uint8_t req_play, req_rec, req_panic, req_clear;
void eng_transport(int play, int rec)
{
    req_play = (uint8_t)(play != 0);
    req_rec = (uint8_t)(rec != 0);
}
void eng_clear_track(int track) { req_clear |= (uint8_t)(1u << (track & 3)); }
void eng_panic(void) { req_panic = 1; }

/* ------------------------------------------------------------------ voices --- */
enum { ST_IDLE, ST_ATT, ST_DEC, ST_SUS, ST_REL };
typedef struct {
    uint8_t active, track, note, gate, tape, stage;
    uint32_t age;
    float vel, transpose;                /* transpose: the tape's speed, for its notes */
    float ph, ph2, sub, mod, mprev, fmenv;
    float cph[5], drift[5];
    float env;
    float lp1, lp2, ic1, ic2;            /* filters */
    float dlen, slp;                     /* string */
    uint32_t sw;
    /* the block's controls */
    float dt, k[4], att, dec, sus, rel, g, kk, a1, a2, a3, fb, c;
} sv_t;
static sv_t SV[NSYNTHV];
#define SLEN 1024u
static float sbuf[NSYNTHV][SLEN] PLAT_POOL;

typedef struct {
    uint8_t active, track, pad, tape;
    uint32_t age, t;                     /* t: frames since the hit */
    float ph, ph2, amp, dec, f0, tone, lvl, lo, bd, hp, nf, q;
    uint32_t seed;
} dv_t;
static dv_t DV[NDRUMV];

static uint32_t age_now;
static float trk_buf[NTRACKS][SUB];

/* per track, per block */
static struct {
    float lfo_ph, lfo, held, fade_t;
    float eff[4], pitch, vol;            /* the knobs with the LFO on them, the pitch offset, the volume */
    float att, dec, sus, rel;
    float fx_lp, fx_hold, fx_ctr, fx_ic1, fx_ic2, fx_ph, fx_g, fx_k, fx_a1, fx_a2, fx_a3;
} TR[NTRACKS];

static RAMC NOIN void string_excite(sv_t *v)
{
    float *b = sbuf[v - SV], L = v->dlen, tone = 0.1f + 0.9f * v->k[1], pos = 0.05f + 0.45f * v->k[2], lp = 0.0f;
    uint32_t n = (uint32_t)L + 2u, i, p = (uint32_t)(pos * L);
    static float ex[SLEN];
    if (n > SLEN - 2u)
        n = SLEN - 2u;
    for (i = 0; i < n; i++) {           /* low-passed noise, combed at the pluck position */
        lp += tone * (rndf() - lp);
        ex[i] = lp;
    }
    for (i = 0; i < n; i++) {
        float e = ex[i] - (i >= p ? ex[i - p] : 0.0f);
        b[(v->sw + i) & (SLEN - 1u)] = e * (0.6f + 0.6f * v->vel);
    }
    v->sw = (v->sw + n) & (SLEN - 1u);
    v->slp = 0.0f;
}

static RAMC void synth_on(int track, int note, int vel, int tape, float transpose)
{
    int i, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    sv_t *v = 0;
    for (i = 0; i < NSYNTHV && !v; i++)
        if (SV[i].active && SV[i].track == track && SV[i].note == note)
            v = &SV[i];
    for (i = 0; i < NSYNTHV && !v; i++)
        if (!SV[i].active)
            v = &SV[i];
    if (!v) {
        for (i = 0; i < NSYNTHV; i++) {
            uint32_t a = SV[i].age - (SV[i].gate ? 0u : 0x40000000u);
            if (a < oldest) {
                oldest = a;
                best = i;
            }
        }
        v = &SV[best];
    }
    if (!v->active || v->track != track) {
        v->ph = frac1(rndf());
        v->ph2 = v->sub = v->mod = v->mprev = 0.0f;
        v->lp1 = v->lp2 = v->ic1 = v->ic2 = 0.0f;
        v->env = 0.0f;
        for (i = 0; i < 5; i++) {
            v->cph[i] = frac1(rndf());
            v->drift[i] = 0.0f;
        }
    }
    v->active = 1;
    v->track = (uint8_t)track;
    v->note = (uint8_t)note;
    v->gate = 1;
    v->tape = (uint8_t)tape;
    v->age = ++age_now;
    v->vel = (float)vel * (1.0f / 127.0f);
    v->transpose = transpose;
    v->stage = ST_ATT;
    v->fmenv = 1.0f;
    TR[track].fade_t = 0.0f;
    if (P.t[track].engine == ENG_STRING) {
        v->dlen = fm_clampf(SR / note_hz((float)note + transpose + TR[track].pitch), 2.0f, (float)(SLEN - 4u));
        v->k[1] = TR[track].eff[1];
        v->k[2] = TR[track].eff[2];
        string_excite(v);
    }
}

static RAMC void synth_off(int track, int note, int tape_only)
{
    int i;
    for (i = 0; i < NSYNTHV; i++)
        if (SV[i].active && SV[i].gate && SV[i].track == track && (note < 0 || SV[i].note == note)
            && (!tape_only || SV[i].tape)) {
            SV[i].gate = 0;
            SV[i].stage = ST_REL;
        }
}

static RAMC void drum_on(int track, int note, int vel, int tape)
{
    int pad = note % NPADS, i, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    dv_t *d = 0;
    const uint16_t *pp = P.t[track].pad[pad];
    for (i = 0; i < NDRUMV && !d; i++)   /* the same pad again, or a closed hat choking an open one */
        if (DV[i].active && DV[i].track == track && (DV[i].pad == pad || (pad == 2 && DV[i].pad == 3)))
            d = &DV[i];
    for (i = 0; i < NDRUMV && !d; i++)
        if (!DV[i].active)
            d = &DV[i];
    if (!d) {
        for (i = 0; i < NDRUMV; i++)
            if (DV[i].age < oldest) {
                oldest = DV[i].age;
                best = i;
            }
        d = &DV[best];
    }
    d->active = 1;
    d->track = (uint8_t)track;
    d->pad = (uint8_t)pad;
    d->tape = (uint8_t)tape;
    d->age = ++age_now;
    d->t = 0;
    d->ph = d->ph2 = 0.0f;
    d->lo = d->bd = d->hp = 0.0f;
    d->amp = (float)vel * (1.0f / 127.0f);
    d->f0 = kn(pp[0]);
    d->dec = kn(pp[1]);
    d->tone = kn(pp[2]);
    d->lvl = kn(pp[3]);
    d->seed = rndu() | 1u;
    {                                    /* the noise filter (Chamberlin), set per hit */
        static const float FC[NPADS] = {0, 3000, 8000, 7000, 1200, 0, 0, 2500, 900, 0, 0, 0};
        float fc = FC[pad] * (0.5f + d->f0);
        d->nf = 2.0f * fm_sin_turns(fm_minf(fc, 15000.0f) * ISR * 0.5f);
        d->q = pad == 4 || pad == 8 ? 0.3f : 0.9f;
    }
    VIEW.pad_hits[track][pad]++;
}

/* ---------------------------------------------------------- control rate --- */
INL float lfo_wave(int shape, float ph, float held)
{
    switch (shape) {
    case LFO_TRI: return 1.0f - 4.0f * fm_fabsf(ph - 0.5f);
    case LFO_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
    case LFO_RANDOM: return held;
    default: return fm_sin_turns(ph);
    }
}

static RAMC void track_control(int t, float dt)
{
    const track_t *tk = &P.t[t];
    float rate = 0.05f * fm_exp2f(kn(tk->lfo[0]) * 9.0f), depth = kn(tk->lfo[1]), fade = kn(tk->lfo[2]) * 3.0f, x;
    int i, eng = tk->engine < ENG_DRUM ? tk->engine : 0;
    TR[t].lfo_ph += rate * dt;
    if (TR[t].lfo_ph >= 1.0f) {
        TR[t].lfo_ph -= fm_floorf(TR[t].lfo_ph);
        TR[t].held = rndf();
    }
    TR[t].fade_t += dt;
    x = lfo_wave(tk->lfo_shape, TR[t].lfo_ph, TR[t].held) * depth;
    if (fade > 0.01f && TR[t].fade_t < fade)
        x *= TR[t].fade_t / fade;
    TR[t].lfo = x;
    for (i = 0; i < 4; i++)
        TR[t].eff[i] = fm_clampf(kn(tk->eng[eng][i]) + (tk->lfo_target == (uint16_t)i ? x * 0.5f : 0.0f), 0.0f, 1.0f);
    TR[t].pitch = tk->lfo_target == TGT_PITCH ? x * 2.0f : 0.0f;     /* +-2 semitones */
    TR[t].vol = tk->lfo_target == TGT_VOLUME ? 1.0f - 0.5f * (x + depth) : 1.0f;
    TR[t].att = 1.0f / ((0.001f * fm_powf(2000.0f, kn(tk->env[0]))) * SR);
    TR[t].dec = fm_expf(-1.0f / ((0.01f * fm_powf(300.0f, kn(tk->env[1]))) * SR));
    TR[t].sus = kn(tk->env[2]);
    TR[t].rel = fm_expf(-6.9f / ((0.005f * fm_powf(800.0f, kn(tk->env[3]))) * SR));
    if (tk->fx_type == FX_FILTER) {      /* the track filter, swept by its own LFO */
        const uint16_t *f = tk->fx[FX_FILTER - 1];
        float oct, fc, g;
        TR[t].fx_ph = frac1(TR[t].fx_ph + kn(f[2]) * 8.0f * dt);
        oct = kn(f[0]) * 9.0f + (kn(f[2]) > 0.01f ? fm_sin_turns(TR[t].fx_ph) * 1.5f : 0.0f);
        fc = fm_minf(40.0f * fm_exp2f(fm_clampf(oct, 0.0f, 9.0f)), 0.45f * SR);
        g = fm_tanf(3.14159265f * fc * ISR);
        TR[t].fx_g = g;
        TR[t].fx_k = 2.0f - 1.9f * kn(f[1]);
        TR[t].fx_a1 = 1.0f / (1.0f + g * (g + TR[t].fx_k));
        TR[t].fx_a2 = g * TR[t].fx_a1;
        TR[t].fx_a3 = g * TR[t].fx_a2;
    }
    VIEW.lfo[t] = x;
    VIEW.lfo_ph[t] = TR[t].lfo_ph;
}

static RAMC void voice_control(sv_t *v)
{
    int t = v->track, i;
    float hz;
    for (i = 0; i < 4; i++)
        v->k[i] = TR[t].eff[i];
    hz = note_hz((float)v->note + v->transpose + TR[t].pitch);
    v->dt = fm_minf(hz * ISR, 0.45f);
    switch (P.t[t].engine) {
    case ENG_STRING:
        v->dlen = fm_clampf(SR / hz, 2.0f, (float)(SLEN - 4u));
        v->fb = v->gate ? 0.9995f - 0.03f * v->k[0] * v->k[0] : 0.93f;    /* DAMP; let go: muted */
        v->c = 1.0f - 0.85f * v->k[0];
        break;
    case ENG_PULSE: {
        float fc = fm_minf(40.0f * fm_exp2f(v->k[2] * 9.0f + v->env * 1.5f), 0.45f * SR);
        v->g = fm_tanf(3.14159265f * fc * ISR);
        v->kk = 2.0f - 1.9f * v->k[3];
        v->a1 = 1.0f / (1.0f + v->g * (v->g + v->kk));
        v->a2 = v->g * v->a1;
        v->a3 = v->g * v->a2;
        break;
    }
    case ENG_CLUSTER:
        v->c = 1.0f - fm_expf(-6.2831853f * fm_minf(60.0f * fm_exp2f(v->k[2] * 8.5f), 0.45f * SR) * ISR);
        for (i = 0; i < 5; i++)          /* DRIFT: each saw wanders */
            v->drift[i] = fm_clampf(v->drift[i] + rndf() * 0.08f, -1.0f, 1.0f);
        break;
    default:
        break;
    }
    v->fmenv *= 0.986f;                  /* FM's index envelope, ~0.25 s */
}

/* ------------------------------------------------------------- synth voice --- */
static RAMC NOIN void synth_block(sv_t *v, float *out, uint32_t m)
{
    const track_t *tk = &P.t[v->track];
    float att = TR[v->track].att, dec = TR[v->track].dec, sus = TR[v->track].sus, rel = TR[v->track].rel,
          g = v->vel * 0.6f + 0.4f, dt = v->dt;
    uint32_t j;
    for (j = 0; j < m; j++) {
        float s = 0.0f, e;
        switch (v->stage) {              /* ADSR */
        case ST_ATT: v->env += att; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ST_DEC; } break;
        case ST_DEC: v->env = sus + (v->env - sus) * dec; if (!v->gate) v->stage = ST_REL; break;
        case ST_REL: v->env *= rel; if (v->env < 0.0002f) { v->env = 0.0f; v->active = 0; } break;
        default: break;
        }
        if (tk->engine == ENG_STRING && v->gate)
            v->env = 1.0f;               /* the string decays by itself; the envelope only closes it */
        e = v->env;
        switch (tk->engine) {
        case ENG_STRING: {               /* Karplus-Strong: DAMP, TONE (excite), PLUCK (excite), DRIVE */
            float p = (float)v->sw - v->dlen, f, y;
            int32_t i;
            if (p < 0.0f)
                p += (float)SLEN;
            i = (int32_t)p;
            f = p - (float)i;
            y = sbuf[v - SV][(uint32_t)i & (SLEN - 1u)] * (1.0f - f) + sbuf[v - SV][(uint32_t)(i + 1) & (SLEN - 1u)] * f;
            v->slp += v->c * (y - v->slp);
            sbuf[v - SV][v->sw] = v->slp * v->fb;
            v->sw = (v->sw + 1u) & (SLEN - 1u);
            s = fm_tanhf(y * (1.0f + 5.0f * v->k[3])) * (1.0f - 0.35f * v->k[3]);
            break;
        }
        case ENG_PULSE: {                /* WIDTH, SUB, CUTOFF, RESONANCE */
            float pw = 0.5f + 0.45f * v->k[0], sq, sb, v3, v1, v2;
            v->ph += dt;
            if (v->ph >= 1.0f)
                v->ph -= 1.0f;
            v->sub += dt * 0.5f;
            if (v->sub >= 1.0f)
                v->sub -= 1.0f;
            sq = (v->ph < pw ? 1.0f : -1.0f) + blep(v->ph, dt) - blep(frac1(v->ph + 1.0f - pw), dt);
            sb = (v->sub < 0.5f ? 1.0f : -1.0f) + blep(v->sub, dt * 0.5f) - blep(frac1(v->sub + 0.5f), dt * 0.5f);
            s = (sq + sb * v->k[1]) * 0.5f;
            v3 = s - v->ic2;
            v1 = v->a1 * v->ic1 + v->a2 * v3;
            v2 = v->ic2 + v->a2 * v->ic1 + v->a3 * v3;
            v->ic1 = 2.0f * v1 - v->ic1;
            v->ic2 = 2.0f * v2 - v->ic2;
            s = v2;
            break;
        }
        case ENG_FM: {                   /* RATIO, INDEX, FEEDBACK, INDEX ENVELOPE */
            static const float R[8] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f};
            float idx = v->k[1] * 6.0f * (1.0f - v->k[3] + v->k[3] * v->fmenv), mo;
            v->ph = frac1(v->ph + dt);
            v->mod = frac1(v->mod + dt * R[(int)(v->k[0] * 7.99f)]);
            mo = fm_sin_turns(v->mod + v->k[2] * 0.25f * v->mprev);
            v->mprev = mo;
            s = fm_sin_turns(v->ph + idx * mo * 0.159f);
            break;
        }
        case ENG_CLUSTER: {              /* SPREAD, SHAPE, CUTOFF, DRIFT */
            int c;
            float sum = 0.0f;
            for (c = 0; c < 5; c++) {
                float cents = ((float)(c - 2) * v->k[0] * 14.0f + v->drift[c] * v->k[3] * 18.0f),
                      d = dt * (1.0f + cents * 0.000578f), sw, sq, o;
                v->cph[c] += d;
                if (v->cph[c] >= 1.0f)
                    v->cph[c] -= 1.0f;
                sw = 2.0f * v->cph[c] - 1.0f - blep(v->cph[c], d);
                o = frac1(v->cph[c] + 0.5f);
                sq = sw - (2.0f * o - 1.0f - blep(o, d));
                sum += lerp(sw, sq * 0.5f, v->k[1]);
            }
            v->lp1 += v->c * (sum * 0.3f - v->lp1);
            v->lp2 += v->c * (v->lp1 - v->lp2);
            s = v->lp2;
            break;
        }
        default:
            break;
        }
        out[j] += s * e * g;
        if (!v->active)
            break;
    }
}

/* ------------------------------------------------------------------ drums --- */
INL float dnoise(dv_t *d)
{
    d->seed ^= d->seed << 13;
    d->seed ^= d->seed >> 17;
    d->seed ^= d->seed << 5;
    return (float)(int32_t)d->seed * (1.0f / 2147483648.0f);
}

static RAMC NOIN void drum_block(dv_t *d, float *out, uint32_t m)
{
    uint32_t j;
    float dsec = 0.03f + d->dec * d->dec * 1.4f, lv = d->lvl * d->amp * 0.9f;
    for (j = 0; j < m; j++, d->t++) {
        float tt = (float)d->t * ISR, env = fm_expf(-6.9f * tt / dsec), s = 0.0f, n, f;
        switch (d->pad) {
        case 0:                          /* KICK: a sine falling in pitch */
            f = (40.0f + 60.0f * d->f0) * (1.0f + 5.0f * d->tone * fm_expf(-tt * 33.0f));
            d->ph = frac1(d->ph + f * ISR);
            s = fm_sin_turns(d->ph) * 1.2f;
            break;
        case 1:                          /* SNARE: a tone and a noise */
            d->ph = frac1(d->ph + (150.0f + 150.0f * d->f0) * ISR);
            n = dnoise(d);
            d->lo += d->nf * d->bd;
            d->hp = n - d->lo - d->q * d->bd;
            d->bd += d->nf * d->hp;
            s = fm_sin_turns(d->ph) * fm_expf(-tt * 25.0f) * (1.0f - d->tone) + d->hp * (0.4f + 0.6f * d->tone);
            break;
        case 2: case 3:                  /* HATS: high-passed noise, closed or open */
            n = dnoise(d);
            d->lo += d->nf * d->bd;
            d->hp = n - d->lo - d->q * d->bd;
            d->bd += d->nf * d->hp;
            s = lerp(d->bd, d->hp, d->tone) * 0.8f;
            if (d->pad == 2)
                env = fm_expf(-6.9f * tt / (0.01f + d->dec * 0.2f));
            break;
        case 4: {                        /* CLAP: band-passed noise, three slaps */
            float ms = tt * 1000.0f, burst = ms < 30.0f ? fm_expf(-(ms - 10.0f * fm_floorf(ms / 10.0f)) * 0.4f) : env;
            n = dnoise(d);
            d->lo += d->nf * d->bd;
            d->hp = n - d->lo - d->q * d->bd;
            d->bd += d->nf * d->hp;
            s = d->bd * 1.5f;
            env = ms < 30.0f ? burst : env;
            break;
        }
        case 5: case 6:                  /* TOMS */
            f = (d->pad == 5 ? 80.0f : 160.0f) * (1.0f + d->f0) * (1.0f + d->tone * fm_expf(-tt * 12.0f));
            d->ph = frac1(d->ph + f * ISR);
            s = fm_sin_turns(d->ph);
            break;
        case 7:                          /* RIM: a click */
            d->ph = frac1(d->ph + (800.0f + 800.0f * d->f0) * ISR);
            n = dnoise(d);
            s = fm_sin_turns(d->ph) * fm_expf(-tt * 120.0f) + n * fm_expf(-tt * 400.0f) * d->tone;
            break;
        case 8: {                        /* COWBELL: two squares through a band-pass */
            float r = 0.7f + 0.6f * d->f0, sq;
            d->ph = frac1(d->ph + 540.0f * r * ISR);
            d->ph2 = frac1(d->ph2 + 800.0f * r * ISR);
            sq = (d->ph < 0.5f ? 0.5f : -0.5f) + (d->ph2 < 0.5f ? 0.5f : -0.5f);
            d->lo += d->nf * d->bd;
            d->hp = sq - d->lo - d->q * d->bd;
            d->bd += d->nf * d->hp;
            s = lerp(d->bd * 1.2f, sq * 0.5f, d->tone);
            break;
        }
        case 9:                          /* ZAP: a fast falling sweep */
            f = 60.0f + (1500.0f + 2500.0f * d->f0) * fm_expf(-tt * (20.0f + 80.0f * d->tone));
            d->ph = frac1(d->ph + f * ISR);
            s = fm_sin_turns(d->ph);
            break;
        case 10:                         /* BLIP */
            d->ph = frac1(d->ph + (600.0f + 1400.0f * d->f0) * ISR);
            s = lerp(fm_sin_turns(d->ph), d->ph < 0.5f ? 0.7f : -0.7f, d->tone);
            break;
        default:                         /* METAL: a sine phase-modulated off a ratio of 1.41 */
            d->ph = frac1(d->ph + (300.0f + 600.0f * d->f0) * ISR);
            d->ph2 = frac1(d->ph2 + (300.0f + 600.0f * d->f0) * 1.41f * ISR);
            s = fm_sin_turns(d->ph + 0.5f * d->tone * fm_sin_turns(d->ph2));
            break;
        }
        out[j] += s * env * lv;
        if (env < 0.0003f && tt > 0.01f) {
            d->active = 0;
            break;
        }
    }
}

/* ----------------------------------------------------------------- the tape --- */
static struct {
    uint8_t playing, recording;
    float pos, len, prev_beat;
} TP;

static RAMC void tape_fire(int t, uint32_t e, int reverse, float transpose)
{
    int note = (int)((e >> 16) & 0x7Fu), on = (int)((e >> 23) & 1u), vel = (int)((e >> 24) & 0x7Fu);
    if (P.t[t].mute)
        return;
    if (reverse)
        on = !on;                        /* backwards, a note starts where it ended */
    if (P.t[t].engine == ENG_DRUM) {
        if (on)
            drum_on(t, note, vel ? vel : 100, 1);
        return;
    }
    if (on)
        synth_on(t, note, vel ? vel : 100, 1, transpose);
    else
        synth_off(t, note, 1);
}

/* the events with a tick in (a, b] (forward) or [b, a) (backward), a and b inside the loop */
static RAMC void tape_span(float a, float b, int reverse, float transpose)
{
    int t;
    uint32_t i;
    for (t = 0; t < NTRACKS; t++) {
        const uint32_t *ev = T->ev[t];
        uint32_t n = T->n[t] < TAPE_MAX ? T->n[t] : TAPE_MAX;
        if (!reverse) {
            for (i = 0; i < n; i++) {
                float tk = (float)(ev[i] & 0xFFFFu);
                if (tk > a && tk <= b)
                    tape_fire(t, ev[i], 0, transpose);
            }
        } else {
            for (i = n; i-- > 0;) {
                float tk = (float)(ev[i] & 0xFFFFu);
                if (tk >= b && tk < a)
                    tape_fire(t, ev[i], 1, transpose);
            }
        }
    }
}

static RAMC void tape_record(int t, int note, int vel, int reverse)
{
    uint32_t *ev = T->ev[t], n = T->n[t], tick = (uint32_t)(TP.pos + 0.5f), on = vel > 0, e, i;
    if (n >= TAPE_MAX)
        return;
    if (tick >= (uint32_t)TP.len)
        tick = 0;
    if (reverse)
        on = !on;
    e = tick | (uint32_t)note << 16 | on << 23 | (uint32_t)(vel & 0x7F) << 24;
    for (i = n; i > 0 && (ev[i - 1] & 0xFFFFu) > tick; i--)   /* kept in tick order */
        ev[i] = ev[i - 1];
    ev[i] = e;
    T->n[t] = (uint16_t)(n + 1u);
}

INL float speed_now(void)
{
    float s = ((float)P.speed - 512.0f) * (2.0f / 511.0f);
    return fm_fabsf(s) < 0.04f ? 0.0f : fm_clampf(s, -2.0f, 2.0f);
}

static RAMC void tape_move(float dt)
{
    float sp = speed_now(), rate = (float)P.tempo * (1.0f / 60.0f) * (float)PPQ * sp, a = TP.pos, b,
          tr = sp != 0.0f ? 12.0f * fm_log2f(fm_maxf(fm_fabsf(sp), 0.25f)) : 0.0f;
    TP.len = (float)(P.loop_bars * 4u * PPQ);
    if (TP.pos >= TP.len)
        TP.pos = fm_floorf(TP.pos - TP.len * fm_floorf(TP.pos / TP.len));
    if (!TP.playing || sp == 0.0f)
        return;
    b = a + rate * dt;
    VIEW.reel += sp * dt * 0.6f;
    if (sp > 0.0f) {
        if (b >= TP.len) {
            tape_span(a, TP.len, 0, tr);
            tape_span(-1.0f, b - TP.len, 0, tr);
            b -= TP.len;
        } else {
            tape_span(a, b, 0, tr);
        }
    } else {
        if (b < 0.0f) {
            tape_span(a, 0.0f, 1, tr);
            tape_span(TP.len, b + TP.len, 1, tr);
            b += TP.len;
        } else {
            tape_span(a, b, 1, tr);
        }
    }
    TP.pos = b;
    {                                    /* beats: the metronome and the screen */
        float beat = fm_floorf(b / (float)PPQ);
        if (beat != TP.prev_beat) {
            TP.prev_beat = beat;
            VIEW.beat++;
        }
    }
}

/* ----------------------------------------------------------------- effects --- */
#define DLEN 33075u                      /* 0.75 s */
static int16_t dbuf[2][DLEN] PLAT_POOL;
#define RV_MAX 2600u
#define AP_MAX 420u
static float rv_line[4][RV_MAX] PLAT_POOL;
static float rv_ap[4][AP_MAX] PLAT_POOL;
static struct {
    uint32_t dw, rv_i[4], ap_i[4], rv_n[4], ap_n[4];
    float d_time, rv_g[4], rv_lp[4], click, click_ph, click_f;
} M;

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

static RAMC void master_control(void)
{
    static const uint16_t RL[4] = {1423, 1361, 1747, 1913}, AL[4] = {113, 89, 307, 233};
    static const float DIVS[DIV_N] = {0.5f, 0.75f, 1.0f};
    float size = 0.6f + 0.75f * kn(P.rv_size), rt = 0.5f + 7.0f * kn(P.rv_size);
    int i;
    for (i = 0; i < 4; i++) {
        M.rv_n[i] = (uint32_t)((float)RL[i] * size);
        M.ap_n[i] = (uint32_t)((float)AL[i] * size);
        if (M.rv_n[i] > RV_MAX)
            M.rv_n[i] = RV_MAX;
        if (M.ap_n[i] > AP_MAX)
            M.ap_n[i] = AP_MAX;
        if (M.rv_i[i] >= M.rv_n[i])
            M.rv_i[i] = 0;
        if (M.ap_i[i] >= M.ap_n[i])
            M.ap_i[i] = 0;
        M.rv_g[i] = fm_exp2f(-9.9658f * (float)M.rv_n[i] / (SR * rt));
    }
    {
        float tm = 60.0f / (float)P.tempo * DIVS[P.dl_div % DIV_N] * SR;
        M.d_time += (fm_minf(tm, (float)(DLEN - 4u)) - M.d_time) * 0.05f;
    }
}

INL void master_sample(float *L, float *R)
{
    float x = (*L + *R) * 0.5f, wl, wr, amt = kn(P.dl_amt), fb = 0.85f * kn(P.dl_fb), ra = kn(P.rv_amt);
    if (amt > 0.001f) {                  /* ping-pong delay, sent from the bus */
        wl = rd16(dbuf[0], M.dw, M.d_time);
        wr = rd16(dbuf[1], M.dw, M.d_time);
        dbuf[0][M.dw] = q16(x * amt + fb * wr);
        dbuf[1][M.dw] = q16(fb * wl);
        if (++M.dw >= DLEN)
            M.dw = 0;
        *L += wl;
        *R += wr;
    }
    if (ra > 0.001f) {                   /* a small springy FDN */
        float in = x * 0.3f * ra, o[4], f[4], h[4];
        int i;
        for (i = 0; i < 4; i++) {
            float *b = rv_ap[i], d = b[M.ap_i[i]], y = d - 0.65f * in;
            b[M.ap_i[i]] = in + 0.65f * y;
            if (++M.ap_i[i] >= M.ap_n[i])
                M.ap_i[i] = 0;
            in = y;
        }
        for (i = 0; i < 4; i++) {
            o[i] = rv_line[i][M.rv_i[i]];
            M.rv_lp[i] += 0.55f * (o[i] - M.rv_lp[i]);
            f[i] = M.rv_lp[i] * M.rv_g[i];
        }
        h[0] = 0.5f * (f[0] + f[1] + f[2] + f[3]);
        h[1] = 0.5f * (f[0] - f[1] + f[2] - f[3]);
        h[2] = 0.5f * (f[0] + f[1] - f[2] - f[3]);
        h[3] = 0.5f * (f[0] - f[1] - f[2] + f[3]);
        for (i = 0; i < 4; i++) {
            rv_line[i][M.rv_i[i]] = fm_flush(in + h[i]);
            if (++M.rv_i[i] >= M.rv_n[i])
                M.rv_i[i] = 0;
        }
        *L += (o[0] + o[2]) * 0.7f;
        *R += (o[1] + o[3]) * 0.7f;
    }
}

static RAMC NOIN void track_fx(int t, float *b, uint32_t m)
{
    const track_t *tk = &P.t[t];
    uint32_t j;
    if (tk->fx_type == FX_OFF || tk->fx_type >= FX_N)
        return;
    {
        const uint16_t *f = tk->fx[tk->fx_type - 1];
        float mix = kn(f[3]);
        for (j = 0; j < m; j++) {
            float x = b[j], y = x;
            switch (tk->fx_type) {
            case FX_DRIVE: {             /* DRIVE, TONE, BIAS, MIX */
                float g = 1.0f + 24.0f * kn(f[0]) * kn(f[0]);
                y = fm_tanhf(x * g + 0.4f * kn(f[2])) - fm_tanhf(0.4f * kn(f[2]));
                TR[t].fx_lp += (0.05f + 0.95f * kn(f[1])) * (y - TR[t].fx_lp);
                y = TR[t].fx_lp * 0.8f;
                break;
            }
            case FX_CRUSH: {             /* BITS, RATE, FOLD, MIX */
                float q = fm_exp2f(2.0f + (1.0f - kn(f[0])) * 12.0f);
                TR[t].fx_ctr += 1.0f;
                if (TR[t].fx_ctr >= 1.0f + kn(f[1]) * 40.0f) {
                    TR[t].fx_ctr = 0.0f;
                    TR[t].fx_hold = x;
                }
                y = fm_floorf(TR[t].fx_hold * q + 0.5f) / q;
                y = lerp(y, fm_sin_turns(0.25f * y * (1.0f + 4.0f * kn(f[2]))), kn(f[2]));
                break;
            }
            case FX_FILTER: {            /* CUTOFF, RESONANCE, SWEEP, MIX */
                float v3 = x - TR[t].fx_ic2, v1 = TR[t].fx_a1 * TR[t].fx_ic1 + TR[t].fx_a2 * v3,
                      v2 = TR[t].fx_ic2 + TR[t].fx_a2 * TR[t].fx_ic1 + TR[t].fx_a3 * v3;
                TR[t].fx_ic1 = 2.0f * v1 - TR[t].fx_ic1;
                TR[t].fx_ic2 = 2.0f * v2 - TR[t].fx_ic2;
                y = v2;
                break;
            }
            default: break;
            }
            b[j] = lerp(x, y, mix);
        }
    }
}

/* ------------------------------------------------------------------ render --- */
static RAMC void clear_all(void)
{
    uint32_t i;
    for (i = 0; i < DLEN; i++)
        dbuf[0][i] = dbuf[1][i] = 0;
    for (i = 0; i < RV_MAX; i++)
        rv_line[0][i] = rv_line[1][i] = rv_line[2][i] = rv_line[3][i] = 0.0f;
    for (i = 0; i < AP_MAX; i++)
        rv_ap[0][i] = rv_ap[1][i] = rv_ap[2][i] = rv_ap[3][i] = 0.0f;
    for (i = 0; i < 4; i++)
        M.rv_lp[i] = 0.0f;
}

void eng_init(tape_t *tape)
{
    T = tape;
    M.d_time = 0.3f * SR;
    PLAT_FAR(clear_all)();
}

void RAMC eng_midi(uint32_t st, uint32_t d1, uint32_t d2, int track)
{
    uint32_t s = st & 0xF0u;
    if (s == 0x90u && d2) {
        if (P.t[track].engine == ENG_DRUM)
            drum_on(track, (int)d1, (int)d2, 0);
        else
            synth_on(track, (int)d1, (int)d2, 0, 0.0f);
    } else if (s == 0x80u || s == 0x90u) {
        if (P.t[track].engine != ENG_DRUM)
            synth_off(track, (int)d1, 0);
    } else if (s == 0xB0u && (d1 == 120u || d1 == 123u)) {
        req_panic = 1;
    }
}

static RAMC void take_input(void)
{
    uint32_t s = slot_seq;
    int i, t;
    if (s != p_seq) {
        const uint32_t *f = (const uint32_t *)(const void *)&slot[slot_idx];
        uint32_t *d = (uint32_t *)(void *)&P, w;
        p_seq = s;
        for (w = 0; w < sizeof P / 4u; w++)
            d[w] = f[w];
    }
    if (req_panic) {
        req_panic = 0;
        for (i = 0; i < NSYNTHV; i++)
            SV[i].active = 0;
        for (i = 0; i < NDRUMV; i++)
            DV[i].active = 0;
        clear_all();
    }
    if (req_clear) {
        for (t = 0; t < NTRACKS; t++)
            if (req_clear >> t & 1u) {
                T->n[t] = 0;
                synth_off(t, -1, 1);
            }
        req_clear = 0;
    }
    if (req_play != TP.playing) {
        TP.playing = req_play;
        if (!TP.playing)                 /* stopped: the tape's notes end, the tape goes back to the start */
            for (t = 0; t < NTRACKS; t++)
                synth_off(t, -1, 1);
        else
            TP.pos = 0.0f, TP.prev_beat = -1.0f;
    }
    TP.recording = (uint8_t)(req_rec && TP.playing);
    while (kev_r != kev_w) {             /* the keys: played, and taped while recording */
        uint32_t e = kev[kev_r % EVN];
        int tr = (int)(e >> 16 & 3u), note = (int)(e & 0x7Fu), vel = (int)(e >> 8 & 0x7Fu);
        kev_r++;
        if (P.t[tr].engine == ENG_DRUM) {
            if (vel)
                drum_on(tr, note, vel, 0);
        } else if (vel) {
            synth_on(tr, note, vel, 0, 0.0f);
        } else {
            synth_off(tr, note, 0);
        }
        if (TP.recording && speed_now() != 0.0f && (vel || P.t[tr].engine != ENG_DRUM))
            tape_record(tr, note, vel, speed_now() < 0.0f);
    }
}

RAMC void eng_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, j;
    int t, v;
    take_input();
    if (p_seq == 0xFFFFFFFFu) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    master_control();
    for (i = 0; i < n; i += SUB) {
        uint32_t m = n - i < SUB ? n - i : SUB;
        float dt = (float)m * ISR, bl[SUB], br[SUB];
        uint32_t beat0 = VIEW.beat;
        tape_move(dt);
        if (VIEW.beat != beat0 && P.metro && TP.playing) {   /* a click: higher on the bar */
            M.click = 1.0f;
            M.click_f = (fm_floorf(TP.pos / (float)PPQ) == 0.0f || (int)(TP.pos / (float)PPQ) % 4 == 0) ? 1760.0f : 880.0f;
        }
        for (t = 0; t < NTRACKS; t++) {
            track_control(t, dt);
            for (j = 0; j < m; j++)
                trk_buf[t][j] = 0.0f;
        }
        for (v = 0; v < NSYNTHV; v++)
            if (SV[v].active) {
                voice_control(&SV[v]);
                synth_block(&SV[v], trk_buf[SV[v].track], m);
            }
        for (v = 0; v < NDRUMV; v++)
            if (DV[v].active)
                drum_block(&DV[v], trk_buf[DV[v].track], m);
        for (j = 0; j < m; j++)
            bl[j] = br[j] = 0.0f;
        for (t = 0; t < NTRACKS; t++) {
            const track_t *tk = &P.t[t];
            float lv = kn(tk->level) * kn(tk->level) * 1.6f * TR[t].vol, pan = (kn(tk->pan) - 0.5f) * 2.0f,
                  gl = lv * (pan > 0.0f ? 1.0f - pan : 1.0f), gr = lv * (pan < 0.0f ? 1.0f + pan : 1.0f), pk = 0.0f;
            track_fx(t, trk_buf[t], m);
            for (j = 0; j < m; j++) {
                float x = trk_buf[t][j];
                bl[j] += x * gl;
                br[j] += x * gr;
                if (fm_fabsf(x * lv) > pk)
                    pk = fm_fabsf(x * lv);
            }
            VIEW.track_peak[t] = fm_maxf(VIEW.track_peak[t] * 0.985f, pk);
        }
        for (j = 0; j < m; j++) {
            float l = bl[j] * 0.5f, r = br[j] * 0.5f;
            master_sample(&l, &r);
            if (M.click > 0.0005f) {
                float c;
                M.click_ph = frac1(M.click_ph + M.click_f * ISR);
                c = fm_sin_turns(M.click_ph) * M.click * 0.25f;
                M.click *= 0.9985f;
                l += c;
                r += c;
            }
            l = fm_tanhf(l);
            r = fm_tanhf(r);
            if (fm_fabsf(l) > VIEW.out_peak)
                VIEW.out_peak = fm_fabsf(l);
            lr[2u * (i + j)] = (int32_t)(l * gain);
            lr[2u * (i + j) + 1u] = (int32_t)(r * gain);
        }
    }
    for (t = 0; t < NTRACKS; t++) {      /* the newest voice of each track, for the screen */
        uint32_t best = 0;
        VIEW.env[t] = 0.0f;
        for (v = 0; v < NSYNTHV; v++)
            if (SV[v].active && SV[v].track == t && SV[v].age >= best) {
                best = SV[v].age;
                VIEW.env[t] = SV[v].env;
                VIEW.env_note[t] = (float)SV[v].note;
            }
    }
    VIEW.tape_pos = TP.pos;
    VIEW.playing = TP.playing;
    VIEW.recording = TP.recording;
}
