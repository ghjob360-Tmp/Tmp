/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the modulators (included by core.c): Vital's DAHDSR envelope with power curves, the LFO with its
 * shapes and six sync modes, the random LFO's four styles. All at the control rate (once per block, dt seconds). */

/* ------------------------------------------------------------------ envelopes --- */
enum { ES_DELAY, ES_ATTACK, ES_HOLD, ES_DECAY, ES_SUSTAIN, ES_RELEASE, ES_OFF };
typedef struct {
    float level, from, t;                /* the output (0..1), where the segment started, time in the segment */
    int stage;
} env_t;

float env_time(float v)                  /* Vital: (value)^4 seconds, value 0..2.378 (~32 s) */
{
    float x = v * 2.378f;
    return x * x * x * x;
}
INL float env_time_r(float v)
{
    float x = v * 2.378f;
    return x * x * x * x;
}
INL void env_gate(env_t *e, int on)
{
    if (on) {
        e->stage = ES_DELAY;
        e->t = 0.0f;
        e->from = e->level;              /* a retrigger rises from where it is (no click) */
    } else if (e->stage < ES_RELEASE) {
        e->stage = ES_RELEASE;
        e->t = 0.0f;
        e->from = e->level;
    }
}
/* p: delay attack hold decay sustain release (0..1), apow dpow rpow (bipolar 0..1) */
static RAMC void env_run(env_t *e, const float *p, float dt)
{
    float len, x;
    for (;;) {
        switch (e->stage) {
        case ES_DELAY:
            len = env_time_r(p[0]);
            if (e->t < len) {
                e->level = e->from * (1.0f - e->t / (len + 1e-6f));
                e->t += dt;
                return;
            }
            e->stage = ES_ATTACK;
            e->t -= len;
            e->from = e->level;
            break;
        case ES_ATTACK:
            len = env_time_r(p[1]);
            if (e->t < len) {
                x = power_scale(e->t / len, (p[6] - 0.5f) * 20.0f);
                e->level = e->from + (1.0f - e->from) * x;
                e->t += dt;
                return;
            }
            e->stage = ES_HOLD;
            e->t -= len;
            e->level = 1.0f;
            break;
        case ES_HOLD:
            len = env_time_r(p[2]);
            if (e->t < len) {
                e->level = 1.0f;
                e->t += dt;
                return;
            }
            e->stage = ES_DECAY;
            e->t -= len;
            break;
        case ES_DECAY:
            len = env_time_r(p[3]);
            if (e->t < len) {
                x = power_scale(e->t / len, -(p[7] - 0.5f) * 20.0f);
                e->level = 1.0f + (p[4] - 1.0f) * x;
                e->t += dt;
                return;
            }
            e->stage = ES_SUSTAIN;
            break;
        case ES_SUSTAIN:
            e->level += (p[4] - e->level) * fm_minf(1.0f, dt * 200.0f);   /* follows a moving sustain smoothly */
            return;
        case ES_RELEASE:
            len = env_time_r(p[5]);
            if (e->t < len) {
                x = power_scale(e->t / len, -(p[8] - 0.5f) * 20.0f);
                e->level = e->from * (1.0f - x);
                e->t += dt;
                return;
            }
            e->stage = ES_OFF;
            e->level = 0.0f;
            return;
        default:
            e->level = 0.0f;
            return;
        }
    }
}
INL float env_stage_view(const env_t *e, const float *p)
{
    float len = e->stage == ES_DELAY ? env_time_r(p[0]) : e->stage == ES_ATTACK ? env_time_r(p[1])
              : e->stage == ES_HOLD ? env_time_r(p[2]) : e->stage == ES_DECAY ? env_time_r(p[3])
              : e->stage == ES_RELEASE ? env_time_r(p[5]) : 0.0f;
    return (float)e->stage + (len > 0.0f ? clampf(e->t / len, 0.0f, 0.999f) : 0.0f);
}

/* ---------------------------------------------------------------------- LFOs --- */
static const float SYNC_BEATS[14] = {0.0f, 4.0f, 2.0f, 4.0f / 3.0f, 1.5f, 1.0f, 2.0f / 3.0f, 0.75f, 0.5f, 1.0f / 3.0f,
                                     0.375f, 0.25f, 1.0f / 6.0f, 0.125f};
const char *const SYNC_NAME[14] = {"FREE", "1/1", "1/2", "1/2T", "1/4.", "1/4", "1/4T", "1/8.", "1/8", "1/8T",
                                   "1/16.", "1/16", "1/16T", "1/32"};
INL float lfo_hz_r(float v, int sync, float tempo)
{
    if (sync > 0 && sync < 14)
        return tempo * (1.0f / 60.0f) / SYNC_BEATS[sync];
    return fm_exp2f(v * 10.0f - 5.0f);   /* 1/32 .. 32 Hz */
}
float lfo_hz(float v, int sync, float tempo) { return lfo_hz_r(v, sync, tempo); }

/* a shape: x 0..1 -> 0..1 */
INL float lfo_shape_i(int shape, const uint8_t *y, const int8_t *c, float x)
{
    x = frac1(x);
    switch (shape) {
    case LS_SINE: return 0.5f - 0.5f * fm_sin_turns(x + 0.25f);
    case LS_TRI: return x < 0.5f ? x * 2.0f : 2.0f - x * 2.0f;
    case LS_SAWUP: return x;
    case LS_SAWDN: return 1.0f - x;
    case LS_SQUARE: return x < 0.5f ? 1.0f : 0.0f;
    case LS_EXPUP: return x * x * x;
    case LS_BUMP: { float s = fm_sin_turns(x * 0.5f); return s * s; }
    case LS_STEPS: return fm_floorf(x * 8.0f) * (1.0f / 7.0f) > 1.0f ? 1.0f : fm_floorf(x * 8.0f) * (1.0f / 7.0f);
    case LS_SMOOTHSTEP: { float t = x < 0.5f ? x * 2.0f : 2.0f - x * 2.0f; return t * t * (3.0f - 2.0f * t); }
    default: {                           /* drawn: LFO_PTS points around the cycle, each segment with its curve */
        float fx = x * (float)LFO_PTS, f;
        int i = (int)fx, j;
        if (i >= LFO_PTS)
            i = LFO_PTS - 1;
        j = i + 1 < LFO_PTS ? i + 1 : 0;
        f = power_scale(fx - (float)i, (float)c[i] * 0.1f);
        return ((float)y[i] + ((float)y[j] - (float)y[i]) * f) * (1.0f / 255.0f);
    }
    }
}

float lfo_shape(int shape, const uint8_t *y, const int8_t *c, float x) { return lfo_shape_i(shape, y, c, x); }

typedef struct {
    float ph, out, sm, age;              /* phase (0..1+ for the one-shot modes), output, smoothed, seconds on */
    int done, rel;
} lfo_t;
/* p: rate phase fade delay smooth amp (0..1); shape, sync, mode */
static RAMC void lfo_run(lfo_t *l, const float *p, int shape, int sync, int mode, const uint8_t *y, const int8_t *c,
                         float tempo, float clock, float dt, int held)
{
    float hz = lfo_hz_r(p[0], sync, tempo), x, v, fade, del;
    l->age += dt;
    del = env_time_r(p[3]) * 0.5f;
    if (l->age < del) {
        l->out = l->sm = 0.5f;
        return;
    }
    switch (mode) {
    case LM_SYNC: x = frac1(clock * hz + p[1]); break;
    case LM_ENVELOPE:
        if (!l->done) {
            l->ph += hz * dt;
            if (l->ph >= 1.0f) {
                l->ph = 1.0f;
                l->done = 1;
            }
        }
        x = l->ph >= 1.0f ? 0.9999f : frac1(l->ph + p[1]);
        break;
    case LM_SUSENV:                      /* to the middle, held there while the key is down, then on to the end */
        if (held && l->ph >= 0.5f) {
            l->ph = 0.5f;
        } else if (!l->done) {
            l->ph += hz * dt;
            if (l->ph >= 1.0f) {
                l->ph = 1.0f;
                l->done = 1;
            }
        }
        x = l->ph >= 1.0f ? 0.9999f : l->ph;
        break;
    case LM_LOOPPOINT:                   /* once through, then the second half again and again */
        l->ph += hz * dt;
        if (l->ph >= 1.0f)
            l->ph = 0.5f + frac1((l->ph - 1.0f) * 2.0f) * 0.5f;
        x = l->ph;
        break;
    case LM_LOOPHOLD:                    /* the first half while held, then to the end */
        l->ph += hz * dt;
        if (held) {
            while (l->ph >= 0.5f)
                l->ph -= 0.5f;
        } else if (l->ph >= 1.0f) {
            l->ph = 1.0f;
            l->done = 1;
        }
        x = l->ph >= 1.0f ? 0.9999f : l->ph;
        break;
    default:                             /* trigger: from the phase at the note, round and round */
        l->ph = frac1(l->ph + hz * dt);
        x = frac1(l->ph + p[1]);
        break;
    }
    v = lfo_shape_i(shape, y, c, x);
    fade = env_time_r(p[2]) * 0.5f;
    if (fade > 0.0f && l->age - del < fade)
        v = 0.5f + (v - 0.5f) * (l->age - del) / fade;
    v = 0.5f + (v - 0.5f) * p[5];
    if (p[4] > 0.001f) {                 /* Vital's smooth: a one-pole of up to ~0.5 s */
        float tc = p[4] * p[4] * 0.5f, a = dt / (tc + dt);
        l->sm += (v - l->sm) * a;
    } else {
        l->sm = v;
    }
    l->out = l->sm;
    l->rel = (int)(x * 65535.0f);        /* the screen's x */
}

/* --------------------------------------------------------------- random LFOs --- */
enum { RS_PERLIN, RS_SH, RS_SINE, RS_LORENZ };
typedef struct {
    float ph, a, b, out, lx, ly, lz;
    uint32_t rng;
} rnd_t;
INL float rnd01(uint32_t *s) { return (float)(xs32(s) >> 8) * (1.0f / 16777216.0f); }
static RAMC void rnd_run(rnd_t *r, int style, float hz, float amp, float dt)
{
    float v;
    if (style == RS_LORENZ) {            /* the Lorenz attractor, stepped at the rate */
        float h = clampf(hz * dt * 0.6f, 0.0f, 0.02f);
        int k;
        for (k = 0; k < 4; k++) {
            float dx = 10.0f * (r->ly - r->lx), dy = r->lx * (28.0f - r->lz) - r->ly, dz = r->lx * r->ly - 2.6667f * r->lz;
            r->lx += dx * h;
            r->ly += dy * h;
            r->lz += dz * h;
        }
        v = clampf(0.5f + r->lx * (1.0f / 44.0f), 0.0f, 1.0f);
    } else {
        r->ph += hz * dt;
        if (r->ph >= 1.0f) {
            r->ph -= fm_floorf(r->ph);
            r->a = r->b;
            r->b = rnd01(&r->rng);
        }
        if (style == RS_SH)
            v = r->b;
        else if (style == RS_SINE)
            v = r->a + (r->b - r->a) * (0.5f - 0.5f * fm_sin_turns(r->ph * 0.5f + 0.25f));
        else {
            float t = r->ph;
            v = r->a + (r->b - r->a) * t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);   /* smoother step */
        }
    }
    r->out = 0.5f + (v - 0.5f) * amp;
}
INL void rnd_seed(rnd_t *r, uint32_t seed)
{
    r->rng = seed ? seed : 1u;
    r->a = rnd01(&r->rng);
    r->b = rnd01(&r->rng);
    r->ph = 0.0f;
    r->lx = 1.0f + r->a;
    r->ly = 1.0f;
    r->lz = 20.0f;
}
