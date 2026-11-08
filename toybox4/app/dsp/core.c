/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the core. See core.h. An "instance" is one of a track's two sounds: inst = track * 2 + kind.
 * The render works in chunks of CHUNK frames: the commands and the keys, the tape and the sequencers, the LFOs and
 * the effective parameters; then the voices (half of them on the second core), the instances' effects, the mix,
 * the master section, the metronome and the sampler's tap. */
#include "core.h"

#define CHUNK 64u
#define NINST (NTRK * 2)

volatile core_view_t CORE_VIEW;
float g_ulaw[256];
sample_ref_t g_synth_sample, g_drum_sample;

/* ---------------------------------------------------------------- the tables --- */
void dsp_tables_init(void)
{
    int i;
    for (i = 0; i < 256; i++) {          /* G.711 mu-law */
        uint32_t u = (uint32_t)(~i) & 0xFFu, e = (u >> 4) & 7u, m = u & 15u;
        int32_t mag = (int32_t)((((m << 3) + 0x84u) << e) - 0x84u);
        g_ulaw[i] = (float)((u & 0x80u) ? -mag : mag) * (1.0f / 32768.0f);
    }
}
INL uint8_t ulaw_enc(float x)
{
    int32_t s = (int32_t)(clampf(x, -0.999f, 0.999f) * 32767.0f), sign = 0, e = 7;
    uint32_t mag;
    if (s < 0) {
        s = -s;
        sign = 0x80;
    }
    if (s > 32635)
        s = 32635;
    mag = (uint32_t)s + 0x84u;
    while (e > 0 && !(mag & (0x4000u >> (7 - e))))
        e--;
    return (uint8_t)(~(sign | e << 4 | ((mag >> (e + 3)) & 15u)));
}

/* ------------------------------------------------------- main loop -> render --- */
_Static_assert(sizeof(proj_t) % 4u == 0u, "proj_t is copied by words");
/* two copies: the render plays one (r_idx), the main loop writes the other and marks it pending; the render
 * takes it at its next block. No third working copy (RAM is the budget). */
static proj_t slot[2];
static volatile uint32_t r_idx, pending, have_project;
#define P (slot[r_idx])

void core_project(const proj_t *p)
{
    uint32_t n = r_idx ^ 1u, w;
    const uint32_t *f = (const uint32_t *)(const void *)p;
    uint32_t *d = (uint32_t *)(void *)&slot[n];
    pending = 0;                         /* (the render may not take a half-written copy) */
    for (w = 0; w < sizeof(proj_t) / 4u; w++)
        d[w] = f[w];
    pending = 1;
}

/* commands: a ring of (op, a, b) the render carries out */
enum { C_KEY, C_PLAY, C_REC, C_CUR, C_LOCATE, C_TAPE, C_SEQIN, C_SEQREST, C_SEQDEL, C_SEQTOG, C_PANIC, C_SARM,
       C_SGO };
#define CMDN 64u
static volatile uint32_t cmd_q[CMDN][3];
static volatile uint32_t cmd_w, cmd_r;
static void cmd(uint32_t op, uint32_t a, uint32_t b)
{
    uint32_t w = cmd_w;
    if (w - cmd_r >= CMDN)
        return;
    cmd_q[w % CMDN][0] = op;
    cmd_q[w % CMDN][1] = a;
    cmd_q[w % CMDN][2] = b;
    cmd_w = w + 1u;
}
void core_key(int track, int kind, int note, int vel)
{
    cmd(C_KEY, (uint32_t)((track & 3) * 2 + (kind & 1)), (uint32_t)((note & 0x7F) | (vel & 0x7F) << 8));
}
static volatile float pressure_in;
void core_pressure(float p) { pressure_in = p; }
void core_play(int on) { cmd(C_PLAY, (uint32_t)on, 0); }
void core_record(int on) { cmd(C_REC, (uint32_t)on, 0); }
void core_current(int track) { cmd(C_CUR, (uint32_t)track, 0); }
void core_locate(uint32_t tick) { cmd(C_LOCATE, tick, 0); }
void core_tape_cmd(int c, int arg) { cmd(C_TAPE, (uint32_t)c, (uint32_t)arg); }
void core_seq_input(int kind, int note) { cmd(C_SEQIN, (uint32_t)kind, (uint32_t)note); }
void core_seq_rest(int kind, int tie) { cmd(C_SEQREST, (uint32_t)kind, (uint32_t)tie); }
void core_seq_delete(int kind) { cmd(C_SEQDEL, (uint32_t)kind, 0); }
void core_seq_toggle(int kind) { cmd(C_SEQTOG, (uint32_t)kind, 0); }
void core_panic(void) { cmd(C_PANIC, 0, 0); }
void core_sample_arm(int kind) { cmd(C_SARM, (uint32_t)kind, 0); }
void core_sample_go(int on) { cmd(C_SGO, (uint32_t)on, 0); }
static volatile uint8_t hold_req;
void core_hold(int on) { hold_req = (uint8_t)on; }

/* ------------------------------------------------------------------- state --- */
static tape_t tape PLAT_POOL;
const tape_t *core_tape(void) { return &tape; }
tape_t *core_tape_rw(void) { return &tape; }
static int cur_track;
static float press, wheel, bend;

typedef struct {
    eng_state_t st;
    float *line;
    uint8_t active, inst, note, gate, stage, uni;
    uint32_t age;
    float vel, pitch, target, det, pan, env, rel_from, transpose;
} sv_t;
static sv_t SV[NSV];
static float sline[NSV][SLINE] PLAT_POOL;

typedef struct {
    eng_state_t st;
    uint8_t active, inst, key, held;
    uint32_t age;
    float vel, t;
} dv_t;
static dv_t DV[NDV];
static uint32_t age_now;

typedef struct {
    float lfo_ph, lfo, held, next, smooth, fade_t, dice[8], dice_t[8];
    uint32_t seed;
    float eff[3][8];                     /* engine, envelope, fx: the parameters with the LFO on them */
    float pitch, vol;
    uint16_t fx_type;
    fx_t fx;
    uint8_t mono_stack[8], mono_n;
    float act;                           /* recent level: an idle instance's fx sleeps */
} inst_t;
static inst_t I[NINST];
static int16_t fx_arena[NINST][6144] PLAT_POOL;
static int16_t mfx_arena[FX_MASTER_ARENA] PLAT_POOL;
static fx_t MFX;
static uint16_t mfx_type = 0xFFFF;
static master_t MS;

static float ibuf[2][NINST][2][CHUNK];
static uint8_t from_tape;                /* the note being played comes off the tape */

/* the sampler's ring (render -> main loop) */
#define SRING 4096u
static uint8_t sring[SRING];
static volatile uint32_t sr_w, sr_r;
static uint8_t samp_state, samp_kind;    /* 0 off, 1 armed, 2 recording */
static uint32_t samp_len;
static float samp_acc;
static uint32_t samp_half;
uint32_t core_sample_take(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    while (sr_r != sr_w && n < max) {
        dst[n++] = sring[sr_r & (SRING - 1u)];
        sr_r++;
    }
    return n;
}

/* --------------------------------------------------------- the parameters --- */
INL const sound_t *snd(int i) { return (i & 1) ? &P.t[i >> 1].drum : &P.t[i >> 1].synth; }
INL int idx4(uint16_t v, int n) { return (int)((float)(v > 1023u ? 1023u : v) * (float)n * (1.0f / 1024.0f)); }

/* -------------------------------------------------------------- the voices --- */
static RAMC void synth_start(sv_t *v, int i, int note, int vel, float det, float pan, int retrig, float transpose)
{
    const sound_t *s = snd(i);
    int e = s->m[M_ENGINE].type % E_NSYNTH, j;
    if (!v->active || v->inst != (uint8_t)i) {
        float *f = v->st.f;
        for (j = 0; j < 48; j++)
            f[j] = 0.0f;
        for (j = 0; j < 8; j++)
            v->st.u[j] = 0;
        v->st.u[7] = 0x1234567u + (uint32_t)(v - SV) * 7919u + age_now;
        v->env = 0.0f;
        v->pitch = (float)note;
        retrig = 1;
    }
    v->active = 1;
    v->inst = (uint8_t)i;
    v->note = (uint8_t)note;
    v->gate = 1;
    v->age = ++age_now;
    v->vel = (float)vel * (1.0f / 127.0f);
    v->target = (float)note;
    v->det = det;
    v->pan = pan;
    v->transpose = transpose;
    v->rel_from = from_tape ? 1.0f : 0.0f;   /* a note off the tape: a stop or a jump ends it */
    if (I[i].eff[1][5] < 0.01f)          /* no portamento */
        v->pitch = (float)note;
    if (retrig) {
        v->stage = 1;
        eng_on(e, &v->st, v->line, I[i].eff[0], note_hz(v->pitch + det + transpose), v->vel);
    }
    I[i].fade_t = 0.0f;
}

/* the load: voices allowed now (shrunk when the render runs late, grown back when there is room) */
static volatile int sv_lim = NSV, dv_lim = NDV;
static volatile uint32_t shed_req;
static RAMC int n_active_sv(void)
{
    int j, n = 0;
    for (j = 0; j < NSV; j++)
        n += SV[j].active != 0;
    return n;
}
static RAMC int n_active_dv(void)
{
    int j, n = 0;
    for (j = 0; j < NDV; j++)
        n += DV[j].active != 0;
    return n;
}
static RAMC void shed_one(void)          /* the render ran late: the oldest voice goes (a released one first) */
{
    int j, best = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (j = 0; j < NSV; j++)
        if (SV[j].active) {
            uint32_t a = SV[j].age - (SV[j].gate ? 0u : 0x40000000u);
            if (a < oldest) {
                oldest = a;
                best = j;
            }
        }
    if (best >= 0) {
        SV[best].active = 0;
        return;
    }
    for (j = 0; j < NDV; j++)
        if (DV[j].active && DV[j].age < oldest) {
            oldest = DV[j].age;
            best = j;
        }
    if (best >= 0)
        DV[best].active = 0;
}

static RAMC sv_t *synth_alloc(int i, int note)
{
    int j, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    for (j = 0; j < NSV; j++)
        if (SV[j].active && SV[j].inst == (uint8_t)i && SV[j].note == (uint8_t)note && !SV[j].uni)
            return &SV[j];
    if (n_active_sv() < sv_lim)
        for (j = 0; j < NSV; j++)
            if (!SV[j].active)
                return &SV[j];
    for (j = 0; j < NSV; j++) {          /* (the ones sounding: below the limit some are free) */
        uint32_t a = SV[j].age - (SV[j].gate ? 0u : 0x40000000u);
        if (SV[j].active && a < oldest) {
            oldest = a;
            best = j;
        }
    }
    return &SV[best];
}

static RAMC void synth_off(int i, int note)
{
    int j;
    for (j = 0; j < NSV; j++)
        if (SV[j].active && SV[j].gate && SV[j].inst == (uint8_t)i && (note < 0 || SV[j].note == (uint8_t)note)) {
            SV[j].gate = 0;
            SV[j].stage = 4;
        }
}

static RAMC void synth_note(int i, int note, int vel, float transpose)
{
    inst_t *in = &I[i];
    int pm = idx4(snd(i)->m[M_ENV].p[4], PM_N), j;
    if (vel <= 0) {                      /* note off */
        if (pm == PM_MONO || pm == PM_LEGATO) {
            int k, n = 0;
            for (k = 0; k < in->mono_n; k++)
                if (in->mono_stack[k] != (uint8_t)note)
                    in->mono_stack[n++] = in->mono_stack[k];
            in->mono_n = (uint8_t)n;
            for (j = 0; j < NSV; j++)
                if (SV[j].active && SV[j].gate && SV[j].inst == (uint8_t)i) {
                    if (n) {             /* back to the last key still held */
                        SV[j].note = in->mono_stack[n - 1];
                        SV[j].target = (float)SV[j].note;
                        if (in->eff[1][5] < 0.01f)
                            SV[j].pitch = SV[j].target;
                    } else {
                        SV[j].gate = 0;
                        SV[j].stage = 4;
                    }
                }
            return;
        }
        if (pm == PM_UNISON) {
            for (j = 0; j < NSV; j++)
                if (SV[j].active && SV[j].gate && SV[j].inst == (uint8_t)i && SV[j].note == (uint8_t)note) {
                    SV[j].gate = 0;
                    SV[j].stage = 4;
                }
            return;
        }
        synth_off(i, note);
        return;
    }
    if (pm == PM_MONO || pm == PM_LEGATO) {
        sv_t *v = 0;
        int k, n = 0;
        for (k = 0; k < in->mono_n; k++)
            if (in->mono_stack[k] != (uint8_t)note)
                in->mono_stack[n++] = in->mono_stack[k];
        if (n > 7)
            n = 7;
        in->mono_stack[n++] = (uint8_t)note;
        in->mono_n = (uint8_t)n;
        for (j = 0; j < NSV && !v; j++)
            if (SV[j].active && SV[j].inst == (uint8_t)i)
                v = &SV[j];
        if (!v)
            v = synth_alloc(i, note);
        synth_start(v, i, note, vel, 0.0f, 0.0f, pm == PM_MONO || !v->gate, transpose);
        v->uni = 0;
        return;
    }
    if (pm == PM_UNISON) {               /* four voices, spread in pitch and across the stereo */
        static const float DET[4] = {-0.12f, -0.04f, 0.04f, 0.12f}, PAN[4] = {-0.8f, -0.3f, 0.3f, 0.8f};
        float spread = 1.5f;
        int k;
        for (k = 0; k < 4; k++) {
            sv_t *v = synth_alloc(i, 200);   /* (no match: a free or the oldest) */
            synth_start(v, i, note, vel, DET[k] * spread, PAN[k], 1, transpose);
            v->uni = (uint8_t)(k + 1);
        }
        return;
    }
    {
        sv_t *v = synth_alloc(i, note);
        synth_start(v, i, note, vel, 0.0f, 0.0f, 1, transpose);
        v->uni = 0;
    }
}

static RAMC void drum_hit(int i, int note, int vel, int on)
{
    int key = (note + 12) % NDKEY, j, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    dv_t *d = 0;
    const sound_t *s = snd(i);
    if (!on) {                           /* a key let go: loop-while-held slices stop */
        for (j = 0; j < NDV; j++)
            if (DV[j].active && DV[j].inst == (uint8_t)i && DV[j].key == (uint8_t)key)
                DV[j].held = 0;
        return;
    }
    for (j = 0; j < NDV && !d; j++)      /* the same key retriggers its voice; a closed hat chokes an open one */
        if (DV[j].active && DV[j].inst == (uint8_t)i && (DV[j].key == (uint8_t)key || (key == 7 && DV[j].key == 8)))
            d = &DV[j];
    if (n_active_dv() < dv_lim)
        for (j = 0; j < NDV && !d; j++)
            if (!DV[j].active)
                d = &DV[j];
    if (!d) {
        for (j = 0; j < NDV; j++)
            if (DV[j].active && DV[j].age < oldest) {
                oldest = DV[j].age;
                best = j;
            }
        d = &DV[best];
    }
    if (!d->active || d->inst != (uint8_t)i) {
        for (j = 0; j < 48; j++)
            d->st.f[j] = 0.0f;
        for (j = 0; j < 8; j++)
            d->st.u[j] = 0;
        d->st.u[7] = 0xA5A5u + (uint32_t)(d - DV) * 7919u + age_now;
    }
    d->active = 1;
    d->inst = (uint8_t)i;
    d->key = (uint8_t)key;
    d->held = 1;
    d->age = ++age_now;
    d->vel = (float)vel * (1.0f / 127.0f);
    d->t = 0.0f;
    drum_on(s->m[M_ENGINE].type % D_N, &d->st, &P.t[i >> 1].pads[key], key, d->vel);
    CORE_VIEW.hits[i >> 1][key]++;
    I[i].fade_t = 0.0f;
}

/* every note goes through here: an instance, a note, a velocity (0 = off), and where it came from */
static RAMC void inst_note(int i, int note, int vel, float transpose)
{
    if (P.t[i >> 1].mute)
        return;
    if (i & 1)
        drum_hit(i, note, vel, vel > 0);
    else
        synth_note(i, note, vel, transpose);
    if (vel > 0) {
        CORE_VIEW.note[i >> 1] = (float)note;
        CORE_VIEW.vel[i >> 1] = (float)vel * (1.0f / 127.0f);
    }
}

/* the tape and the sequencers */
#include "core_tape.inc"
#include "core_seq.inc"

/* ------------------------------------------------------------------ the LFOs --- */
static RAMC void inst_control(int i, float dt)
{
    inst_t *in = &I[i];
    const sound_t *s = snd(i);
    const mod_t *l = &s->m[M_LFO];
    int m, k, tgt_m = -1, tgt_p = 0;
    float x = 0.0f, amt;
    in->fade_t += dt;
    for (m = 0; m < 3; m++)
        for (k = 0; k < 8; k++)
            in->eff[m][k] = kn(s->m[m].p[k]);
    in->pitch = 0.0f;
    in->vol = 1.0f;
    if (l->on) {
        float speed = 0.05f * fm_exp2f(kn(l->p[0]) * 9.0f);
        switch (l->type % L_N) {
        case L_SENSE: {                  /* a performance source onto one parameter */
            int src = idx4(l->p[0], LS_N);
            float v = src == LS_PRESSURE ? press : src == LS_VELOCITY ? CORE_VIEW.vel[i >> 1]
                    : src == LS_ENVELOPE ? CORE_VIEW.env[i >> 1] : src == LS_LEVEL ? fm_minf(in->act * 4.0f, 1.0f) : wheel;
            float sm = 0.002f + 0.5f * (1.0f - kn(l->p[4]));
            in->smooth += (v - in->smooth) * sm;
            x = in->smooth * (kn(l->p[1]) * 2.0f - 1.0f);
            tgt_m = idx4(l->p[2], LT_N);
            tgt_p = idx4(l->p[3], 8);
            break;
        }
        case L_DICE:                     /* every parameter of a module wanders */
            amt = kn(l->p[0]);
            tgt_m = idx4(l->p[2], LT_N);
            for (k = 0; k < 8; k++) {
                in->dice_t[k] -= dt * (0.2f + kn(l->p[1]) * 8.0f);
                if (in->dice_t[k] <= 0.0f) {
                    in->dice_t[k] = 0.5f + 0.5f * rnd1(&in->seed);
                    in->held = rnd1(&in->seed);
                    in->dice_t[k] += 0.0f;
                    in->dice[k] = lerpf(in->dice[k], in->held * amt * 0.5f, 1.0f);
                }
                in->eff[tgt_m][k] = clampf(in->eff[tgt_m][k] + in->dice[k], 0.0f, 1.0f);
            }
            tgt_m = -1;
            x = in->dice[0];
            break;
        case L_WOBBLE: {                 /* tremolo: pitch and volume */
            static const float SYNC[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f};
            int sy = idx4(l->p[5], 6), sh = idx4(l->p[4], 5);
            float ph, w, f = sy ? (float)P.tempo / 60.0f / SYNC[sy] : speed;
            in->lfo_ph = frac1(in->lfo_ph + f * dt);
            ph = in->lfo_ph;
            w = sh == 1 ? 2.0f * ph - 1.0f : sh == 2 ? fm_exp2f(-ph * 6.0f) * 2.0f - 1.0f
              : sh == 3 ? (ph < 0.5f ? 1.0f : -1.0f) : sh == 4 ? (ph < 0.1f ? 1.0f : -1.0f) : fm_sin_turns(ph);
            amt = kn(l->p[3]) > 0.01f ? fm_minf(in->fade_t / (kn(l->p[3]) * 3.0f), 1.0f) : 1.0f;
            in->pitch = w * (kn(l->p[1]) * 2.0f - 1.0f) * 2.0f * amt;
            in->vol = 1.0f - fm_fabsf(kn(l->p[2]) * 2.0f - 1.0f) * (0.5f + 0.5f * w * (kn(l->p[2]) < 0.5f ? -1.0f : 1.0f)) * amt;
            x = w;
            break;
        }
        case L_WAVE: {                   /* a classic LFO onto one parameter */
            static const float SYNC[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f};
            int sy = idx4(l->p[5], 6), sh = idx4(l->p[4], 5);
            float ph, f = sy ? (float)P.tempo / 60.0f / SYNC[sy] : speed;
            if (kn(l->p[6]) > 0.5f && in->fade_t < dt * 1.5f)
                in->lfo_ph = kn(l->p[7]);        /* retrigger on a note */
            in->lfo_ph += f * dt;
            if (in->lfo_ph >= 1.0f) {
                in->lfo_ph -= fm_floorf(in->lfo_ph);
                in->next = rnd1(&in->seed);
            }
            ph = in->lfo_ph;
            x = sh == 1 ? 1.0f - 4.0f * fm_fabsf(ph - 0.5f) : sh == 2 ? 2.0f * ph - 1.0f : sh == 3 ? (ph < 0.5f ? 1.0f : -1.0f)
              : sh == 4 ? in->next : fm_sin_turns(ph);
            x *= kn(l->p[1]) * 2.0f - 1.0f;
            tgt_m = idx4(l->p[2], LT_N);
            tgt_p = idx4(l->p[3], 8);
            break;
        }
        default:                         /* MIDI: the mod wheel and the pressure */
            x = wheel * (kn(l->p[0]) * 2.0f - 1.0f) + press * (kn(l->p[1]) * 2.0f - 1.0f);
            tgt_m = idx4(l->p[2], LT_N);
            tgt_p = idx4(l->p[3], 8);
            break;
        }
        if (tgt_m >= 0)
            in->eff[tgt_m][tgt_p] = clampf(in->eff[tgt_m][tgt_p] + x * 0.5f, 0.0f, 1.0f);
    }
    in->lfo = x;
    {
        int t = i >> 1;
        if ((i & 1) == P.t[t].kind) {
            CORE_VIEW.lfo[t] = x;
            CORE_VIEW.lfo_ph[t] = in->lfo_ph;
            for (m = 0; m < 3; m++)
                for (k = 0; k < 8; k++)
                    CORE_VIEW.eff[t][m][k] = in->eff[m][k];
        }
    }
}

/* ------------------------------------------------------- one voice, a chunk --- */
static RAMC NOIN void synth_voice_chunk(sv_t *v, int core, uint32_t n)
{
    inst_t *in = &I[v->inst];
    const sound_t *s = snd(v->inst);
    int e = s->m[M_ENGINE].type % E_NSYNTH;
    float *L = ibuf[core][v->inst][0], *R = ibuf[core][v->inst][1];
    float a = in->eff[1][0], d = in->eff[1][1], su = in->eff[1][2], r = in->eff[1][3];
    float att = 1.0f / ((0.001f * fm_powf(8000.0f, a)) * SR), dec = fm_expf(-4.6f / ((0.005f * fm_powf(2000.0f, d)) * SR)),
          rel = fm_expf(-6.9f / ((0.005f * fm_powf(2000.0f, r)) * SR)), port = in->eff[1][5],
          vg = (0.35f + 0.65f * v->vel) * (0.2f + 1.6f * in->eff[1][7]) * in->vol;
    float gl = v->pan > 0.0f ? 1.0f - v->pan : 1.0f, gr = v->pan < 0.0f ? 1.0f + v->pan : 1.0f;
    uint32_t o, j;
    for (o = 0; o < n; o += CTL) {
        float tmp[CTL], hz;
        uint32_t m = n - o < CTL ? n - o : CTL;
        if (port > 0.01f)                /* portamento */
            v->pitch += (v->target - v->pitch) * (1.0f - fm_exp2f(-(float)m * ISR / (port * port * 1.5f + 0.001f)));
        else
            v->pitch = v->target;
        hz = note_hz(v->pitch + v->det + v->transpose + in->pitch + bend * (1.0f + in->eff[1][6] * 11.0f) +
                     ((float)P.detune - 512.0f) * 0.002f);
        for (j = 0; j < m; j++)
            tmp[j] = 0.0f;
        eng_run(e, &v->st, v->line, in->eff[0], hz, tmp, m);
        for (j = 0; j < m; j++) {
            switch (v->stage) {          /* ADSR */
            case 1: v->env += att; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; } break;
            case 2: v->env = su + (v->env - su) * dec; if (!v->gate) v->stage = 4; break;
            case 4: v->env *= rel; if (v->env < 0.0003f) { v->env = 0.0f; v->stage = 0; v->active = 0; } break;
            default: break;
            }
            L[o + j] += tmp[j] * v->env * vg * gl;
            R[o + j] += tmp[j] * v->env * vg * gr;
        }
        if (!v->active)
            break;
    }
}

static RAMC NOIN void drum_voice_chunk(dv_t *d, int core, uint32_t n)
{
    inst_t *in = &I[d->inst];
    const sound_t *s = snd(d->inst);
    float *L = ibuf[core][d->inst][0], *R = ibuf[core][d->inst][1];
    float al = in->eff[1][0] * 2.0f, mid = in->eff[1][1] * 1.5f, rl = 0.03f + 3.0f * in->eff[1][2] * in->eff[1][2],
          tm = 0.002f + 0.15f * in->eff[1][3], vg = (0.3f + 0.7f * d->vel) * (0.2f + 1.6f * in->eff[1][7]) * in->vol;
    uint32_t o, j;
    for (o = 0; o < n; o += CTL) {
        float tmp[CTL], g;
        uint32_t m = n - o < CTL ? n - o : CTL;
        int alive;
        for (j = 0; j < m; j++)
            tmp[j] = 0.0f;
        alive = drum_run(s->m[M_ENGINE].type % D_N, &d->st, &P.t[d->inst >> 1].pads[d->key], d->key, d->held, tmp, m);
        /* the dynamic envelope: attack level -> mid level by the timing, then the release */
        g = d->t < tm ? lerpf(al, mid, d->t / tm) : mid * fm_expf(-(d->t - tm) / rl);
        d->t += (float)m * ISR;
        for (j = 0; j < m; j++) {
            L[o + j] += tmp[j] * g * vg;
            R[o + j] += tmp[j] * g * vg;
        }
        if (!alive || (g < 0.0005f && d->t > tm)) {
            d->active = 0;
            break;
        }
    }
}

/* the second core's share: the odd voices */
static uint32_t job_n;
static RAMC void job_odd(uint32_t arg)
{
    int j;
    (void)arg;
    for (j = 1; j < NSV; j += 2)
        if (SV[j].active)
            synth_voice_chunk(&SV[j], 1, job_n);
    for (j = 1; j < NDV; j += 2)
        if (DV[j].active)
            drum_voice_chunk(&DV[j], 1, job_n);
}

/* ------------------------------------------------------------------ commands --- */
static volatile uint32_t calm;           /* seconds the render has kept up since the last late one */
void core_overload(void)                 /* (the platform: the render took over 90 % of its time) */
{
    if (sv_lim > 3)
        sv_lim--;
    if (dv_lim > 4)
        dv_lim--;
    shed_req++;
    calm = 0;
    CORE_VIEW.shed++;
    CORE_VIEW.sv_lim = (uint8_t)sv_lim;
    CORE_VIEW.dv_lim = (uint8_t)dv_lim;
}
void core_cpu(float load)                /* main loop, about once a second: room again, one voice more */
{
    CORE_VIEW.cpu = load;
    if (load < 0.6f && ++calm >= 2u) {  /* two calm seconds in a row: one voice back */
        if (sv_lim < NSV)
            sv_lim++;
        else if (dv_lim < NDV)
            dv_lim++;
    }
    CORE_VIEW.sv_lim = (uint8_t)sv_lim;
    CORE_VIEW.dv_lim = (uint8_t)dv_lim;
}

static RAMC void take_input(void)
{
    static uint32_t shed_done;
    while (shed_done != shed_req) {      /* (each late render: one voice) */
        shed_done++;
        shed_one();
    }
    if (pending) {
        r_idx ^= 1u;
        pending = 0;
        have_project = 1;
    }
    press = pressure_in;
    while (cmd_r != cmd_w) {
        uint32_t op = cmd_q[cmd_r % CMDN][0], a = cmd_q[cmd_r % CMDN][1], b = cmd_q[cmd_r % CMDN][2];
        cmd_r++;
        switch (op) {
        case C_KEY: {
            int i = (int)a, note = (int)(b & 0x7Fu), vel = (int)(b >> 8 & 0x7Fu);
            if (!seq_key(i, note, vel)) {     /* a sequencer that takes the keys has them */
                inst_note(i, note, vel, 0.0f);
                tape_rec(i, note, vel);
            }
            break;
        }
        case C_PLAY: tape_play((int)a); break;
        case C_REC: TP.armed = (uint8_t)a; break;
        case C_CUR: cur_track = (int)a & 3; break;
        case C_LOCATE: tape_locate(a); break;
        case C_TAPE: tape_cmd((int)a, (int)b); break;
        case C_SEQIN: seq_input(cur_track * 2 + (int)a, (int)b); break;
        case C_SEQREST: seq_rest(cur_track * 2 + (int)a, (int)b); break;
        case C_SEQDEL: seq_delete(cur_track * 2 + (int)a); break;
        case C_SEQTOG: seq_toggle(cur_track * 2 + (int)a); break;
        case C_PANIC: {
            int j;
            for (j = 0; j < NSV; j++)
                SV[j].active = 0;
            for (j = 0; j < NDV; j++)
                DV[j].active = 0;
            break;
        }
        case C_SARM: samp_state = 1; samp_kind = (uint8_t)a; samp_len = 0; break;
        case C_SGO:
            if (a && samp_state == 1) {
                samp_state = 2;
                samp_len = 0;
            } else if (!a && samp_state == 2) {
                samp_state = 0;
            }
            break;
        default: break;
        }
    }
}

void core_init(void)
{
    int i;
    for (i = 0; i < NSV; i++)
        SV[i].line = sline[i];
    for (i = 0; i < NINST; i++) {
        I[i].fx.arena = fx_arena[i];
        I[i].fx.arena_n = 6144u;
        I[i].fx_type = 0xFFFF;
        I[i].seed = 0x51ED0000u + (uint32_t)i * 977u;
    }
    MFX.arena = mfx_arena;
    MFX.arena_n = FX_MASTER_ARENA;
    tape_init();
    seq_init();
}

void RAMC core_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    int i = cur_track * 2 + (P.t[cur_track].kind & 1);
    switch (st & 0xF0u) {
    case 0x90u:
        if (d2) {
            if (!seq_key(i, (int)d1, (int)d2)) {
                inst_note(i, (int)d1, (int)d2, 0.0f);
                tape_rec(i, (int)d1, (int)d2);
            }
            break;
        }
        /* fall through */
    case 0x80u:
        if (!seq_key(i, (int)d1, 0)) {
            inst_note(i, (int)d1, 0, 0.0f);
            tape_rec(i, (int)d1, 0);
        }
        break;
    case 0xB0u: if (d1 == 1u) wheel = (float)d2 * (1.0f / 127.0f); break;
    case 0xD0u: press = (float)d1 * (1.0f / 127.0f); break;
    case 0xE0u: bend = ((float)((d2 << 7) | d1) - 8192.0f) * (1.0f / 8192.0f); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ render --- */
static float metro_env, metro_ph, metro_f;

RAMC void core_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t o, j;
    int i, v;
    take_input();
    if (!have_project || hold_req) {
        for (j = 0; j < 2u * n; j++)
            lr[j] = 0;
        return;
    }
    for (o = 0; o < n; o += CHUNK) {
        uint32_t m = n - o < CHUNK ? n - o : CHUNK, beat0 = CORE_VIEW.beat;
        float dt = (float)m * ISR, L[CHUNK], R[CHUNK];
        int handed;
        tape_move(dt);
        seq_run(dt);
        if (CORE_VIEW.beat != beat0 && P.metro && TP.playing) {   /* the metronome */
            metro_env = kn(P.metro);
            metro_f = (TP.head / (float)TPQ) - 4.0f * fm_floorf(TP.head / (4.0f * TPQ)) < 0.5f ? 1760.0f : 1175.0f;
        }
        for (i = 0; i < NINST; i++) {
            inst_control(i, dt);
            for (j = 0; j < m; j++)
                ibuf[0][i][0][j] = ibuf[0][i][1][j] = ibuf[1][i][0][j] = ibuf[1][i][1][j] = 0.0f;
        }
        job_n = m;                       /* the odd voices to the second core, the even ones here */
        handed = plat_cpu2_run(job_odd, 0);
        for (v = 0; v < NSV; v += 2)
            if (SV[v].active)
                synth_voice_chunk(&SV[v], 0, m);
        for (v = 0; v < NDV; v += 2)
            if (DV[v].active)
                drum_voice_chunk(&DV[v], 0, m);
        if (!handed || plat_cpu2_wait() < 0)
            job_odd(0);
        for (j = 0; j < m; j++)
            L[j] = R[j] = 0.0f;
        for (i = 0; i < NINST; i++) {    /* each instance: its effect, its track's level and pan */
            inst_t *in = &I[i];
            const sound_t *s = snd(i);
            const track_t *t = &P.t[i >> 1];
            float *bl = ibuf[0][i][0], *br = ibuf[0][i][1], pk = 0.0f, lv, pan, gl, gr;
            for (j = 0; j < m; j++) {
                bl[j] += ibuf[1][i][0][j];
                br[j] += ibuf[1][i][1][j];
                if (fm_fabsf(bl[j]) > pk)
                    pk = fm_fabsf(bl[j]);
            }
            in->act = fm_maxf(in->act * 0.995f, pk);
            if (s->m[M_FX].on && in->act > 0.00005f) {
                uint16_t ft = s->m[M_FX].type % FX_N;
                if (ft != in->fx_type) {
                    in->fx_type = ft;
                    fx_reset(ft, &in->fx);
                }
                fx_run(ft, &in->fx, in->eff[2], bl, br, m);
                in->act = fm_maxf(in->act, 0.0001f);   /* a tail keeps it awake a while */
            }
            lv = kn(t->level);
            lv = lv * lv * 1.6f;
            pan = (kn(t->pan) - 0.5f) * 2.0f;
            gl = lv * (pan > 0.0f ? 1.0f - pan : 1.0f);
            gr = lv * (pan < 0.0f ? 1.0f + pan : 1.0f);
            if (TP.breaking && TP.playing)
                gl = gr = 0.0f;
            pk = 0.0f;
            for (j = 0; j < m; j++) {
                L[j] += bl[j] * gl;
                R[j] += br[j] * gr;
                if (fm_fabsf(bl[j] * lv) > pk)
                    pk = fm_fabsf(bl[j] * lv);
            }
            CORE_VIEW.trk_peak[i >> 1] = fm_maxf(CORE_VIEW.trk_peak[i >> 1] * 0.98f, pk);
        }
        {                                /* the master: EQ, the master FX, the output stage */
            float ke[4] = {kn(P.eq.p[0]), kn(P.eq.p[1]), kn(P.eq.p[2]), P.eq.on ? kn(P.eq.p[3]) : 0.0f},
                  ko[4] = {kn(P.mout.p[0]), kn(P.mout.p[1]), kn(P.mout.p[2]), kn(P.mout.p[3])}, km[8], gr;
            for (j = 0; j < m; j++) {
                L[j] *= 0.5f;
                R[j] *= 0.5f;
            }
            eq_run(&MS, ke, L, R, m);
            if (P.mfx.on) {
                uint16_t ft = P.mfx.type % FX_N;
                for (j = 0; j < 8; j++)
                    km[j] = kn(P.mfx.p[j]);
                if (ft != mfx_type) {
                    mfx_type = ft;
                    fx_reset(ft, &MFX);
                }
                fx_run(ft, &MFX, km, L, R, m);
            }
            if (metro_env > 0.0005f)
                for (j = 0; j < m; j++) {
                    float c;
                    metro_ph = frac1(metro_ph + metro_f * ISR);
                    c = fm_sin_turns(metro_ph) * metro_env * 0.3f;
                    metro_env *= 0.9975f;
                    L[j] += c;
                    R[j] += c;
                }
            out_run(&MS, ko, L, R, m, &gr);
            CORE_VIEW.gr = gr;
        }
        for (j = 0; j < m; j++) {
            float l = L[j], r = R[j];
            if (fm_fabsf(l) > CORE_VIEW.out_peak)
                CORE_VIEW.out_peak = fm_fabsf(l);
            if (samp_state == 2) {       /* the sampler: the master out, at half the rate, mu-law */
                samp_acc += (l + r) * 0.25f;
                if (++samp_half >= 2u) {
                    samp_half = 0;
                    if (sr_w - sr_r < SRING) {
                        sring[sr_w & (SRING - 1u)] = ulaw_enc(samp_acc);
                        sr_w++;
                        samp_len++;
                    }
                    samp_acc = 0.0f;
                }
            }
            lr[2u * (o + j)] = (int32_t)(l * gain);
            lr[2u * (o + j) + 1u] = (int32_t)(r * gain);
        }
    }
    {                                    /* the screen's view of the voices */
        int t;
        uint32_t best[NTRK] = {0, 0, 0, 0};
        uint16_t nv = 0, nd = 0;
        for (t = 0; t < NTRK; t++)
            CORE_VIEW.env[t] = 0.0f;
        for (v = 0; v < NSV; v++)
            if (SV[v].active) {
                t = SV[v].inst >> 1;
                nv++;
                if (SV[v].age >= best[t]) {
                    best[t] = SV[v].age;
                    CORE_VIEW.env[t] = SV[v].env;
                    CORE_VIEW.env_stage[t] = (float)SV[v].stage;
                }
            }
        for (v = 0; v < NDV; v++)
            nd += DV[v].active;
        CORE_VIEW.voices = nv;
        CORE_VIEW.drums = nd;
        CORE_VIEW.sampling = samp_state;
        CORE_VIEW.sample_len = samp_len;
    }
}
