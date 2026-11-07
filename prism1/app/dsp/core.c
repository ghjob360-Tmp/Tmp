/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the core. See core.h. Vital (Matt Tytel, GPL-3.0) is the model: its voice architecture, routing,
 * modulation matrix (amount, bipolar, power, bypass), envelope and LFO behaviour and amplitude law. */
#include "core.h"

#define X(id, nm, kind, n, def, fl) {nm, kind, n, fl, def},
const pdesc_t PDESC[NPARAM] = {ALL_PARAMS};
#undef X
const char *const MS_NAME[MS_N] = {"-",      "ENV 1",  "ENV 2",  "ENV 3",  "ENV 4",  "ENV 5",  "ENV 6",
                                   "LFO 1",  "LFO 2",  "LFO 3",  "LFO 4",  "LFO 5",  "LFO 6",  "LFO 7",
                                   "LFO 8",  "RAND 1", "RAND 2", "RAND 3", "RAND 4", "MACRO 1", "MACRO 2",
                                   "MACRO 3", "MACRO 4", "VELOCITY", "PRESSURE", "NOTE", "OCTAVE NOTE", "MOD WHEEL",
                                   "PITCH BEND", "RANDOM"};

#include "mod.inc"

volatile core_view_t CORE_VIEW;

/* ------------------------------------------------------------- the patch handoff --- */
static patch_t pslot[2];
static volatile uint32_t p_r, p_pending, p_have;
#define PT (pslot[p_r])
static float pn[NPARAM];                 /* the patch's values, 0..1 (choices: their index, as a float) */
typedef struct {
    uint8_t src, bip;
    uint16_t dst;
    float amt, pw;
} cslot_t;
static cslot_t vsl[NSLOT], gsl[NSLOT];   /* the matrix: slots to per-voice parameters, slots to global ones */
static int nvs, ngs;
static uint16_t vdst[NSLOT], gdst[NSLOT];   /* their distinct destinations */
static int nvd, ngd;
static uint32_t src_used;                /* the sources the matrix reads (bit per MS_*) */
static uint8_t o_on[NOSC], s_on, flt_on[2];
static uint8_t v_mono;                   /* the voices are mono (no spread, centred, nothing moves the pan): the filters
                                            run one channel, half the cost */
static int fx_was_on[FXT_N];
static float gmv[NSLOT];                 /* the global modulation (effects, volume): a value per destination ... */
static uint8_t gix[NPARAM];              /* ... found by the parameter (index + 1; 0 none) */
INL float gmod(int id) { return gix[id] ? gmv[gix[id] - 1u] : 0.0f; }

static const wt_t *volatile tab[NOSC];   /* the tables (main loop: core_table) */
static const wt_t *volatile tab_used[NOSC][2];   /* what the render read last and the block before */

/* --------------------------------------------------------------------- commands --- */
#define CMDN 32u                         /* (emptied every control block: 32 is plenty) */
enum { C_KEY, C_HOLD, C_PANIC };
static volatile uint32_t cmd_q[CMDN][2], cmd_w, cmd_r;
static volatile float in_press, in_wheel, in_bend;
static void cmd(uint32_t op, uint32_t a)
{
    uint32_t w = cmd_w;
    if (w - cmd_r >= CMDN)
        return;
    cmd_q[w & (CMDN - 1u)][0] = op;
    cmd_q[w & (CMDN - 1u)][1] = a;
    cmd_w = w + 1u;
}
void core_key(int note, int vel) { cmd(C_KEY, (uint32_t)(note & 127) | (uint32_t)(vel & 127) << 8); }
void core_hold(int on) { cmd(C_HOLD, (uint32_t)on); }
void core_panic(void) { cmd(C_PANIC, 0); }
void core_pressure(float p) { in_press = p; }
void core_wheel(float w) { in_wheel = w; }
void core_bend(float b) { in_bend = b; }

void core_patch(const patch_t *p)
{
    uint32_t n = p_r ^ 1u, i;
    const uint32_t *s = (const uint32_t *)(const void *)p;
    uint32_t *d = (uint32_t *)(void *)&pslot[n];
    p_pending = 0;
    for (i = 0; i < sizeof(patch_t) / 4u; i++)
        d[i] = s[i];
    p_pending = 1;
}
void core_table(int osc, const wt_t *t)
{
    if (osc >= 0 && osc < NOSC)
        tab[osc] = t;
}
int core_table_busy(const wt_t *t)
{
    int k;
    for (k = 0; k < NOSC; k++)
        if (tab[k] == t || tab_used[k][0] == t || tab_used[k][1] == t)
            return 1;
    return 0;
}

/* ----------------------------------------------------------------------- voices --- */
typedef struct {
    int on, note, gate, key;             /* sounding, its key, gate (key or hold), the key itself down */
    uint32_t order;                      /* when it started (bigger = newer) */
    float vel, nrnd, pitch, gfrom, gto, gt, glen;
    env_t env[6];
    lfo_t lfo[8];
    rnd_t rnd[4];
    osc_state_t osc[NOSC];
    smp_state_t smp;
    float mono[NOSC + 1][CTL];           /* each oscillator's last block (FM / RM sources); [NOSC] the sample */
    float amp;                           /* the amplitude at the end of the last block */
    float src[MS_N];
    int cost;                            /* oscillator voices it plays */
} voice_t;
static voice_t V[NVOICE] PLAT_POOL;
static filt_t VF[NVOICE][2] PLAT_POOL;
static float vout[NVOICE][4][CTL];       /* each voice's block: main L R, direct L R */
static volatile uint8_t vplayed[NVOICE]; /* it wrote vout this block */
static uint32_t v_order;
static int hold_on, budget = 64, mono_last = -1;
static float press, wheel, bend, tempo = 120.0f, clock_s;
static uint32_t rng_g = 0x2545F491u;
static int legato_stack[16], nstack;

/* per-core scratch: the modulation of the voice being rendered, and its buffers */
static float vmod[2][NPARAM];
enum { B_F1L, B_F1R, B_F2L, B_F2R, B_BOL, B_BOR, B_FXL, B_FXR, B_DL, B_DR, B_N };
static float scr[2][B_N][CTL];

INL float pv(const float *vm, int id) { return clampf(pn[id] + vm[id], 0.0f, 1.0f); }
INL int pc(int id) { return (int)pn[id]; }   /* a choice */

INL float src_val(const float *s, const cslot_t *c)
{
    float x = s[c->src];
    if (c->src == MS_BEND)
        x = x * 0.5f + 0.5f;
    if (c->pw != 0.0f)
        x = power_scale(clampf(x, 0.0f, 1.0f), c->pw);
    if (c->bip)
        x = x * 2.0f - 1.0f;
    return x * c->amt;
}
static RAMC void matrix_voice(const float *s, float *vm)
{
    int i;
    for (i = 0; i < nvd; i++)
        vm[vdst[i]] = 0.0f;
    for (i = 0; i < nvs; i++)
        vm[vsl[i].dst] += src_val(s, &vsl[i]);
}

/* the cost of a voice in oscillator voices (unison), for the budget */
static RAMC int voice_cost(void)
{
    int k, c = 0;
    for (k = 0; k < NOSC; k++)
        if (o_on[k])
            c += 1 + pc(P_OSC(k, UNI));
    return c + (s_on ? 1 : 0) + 1;
}

static RAMC void voice_start(voice_t *v, int note, int vel, int legato)
{
    int k;
    float glide = env_time_r(pn[P_V_GLIDE]) * 0.25f;
    v->gate = 1;
    v->key = 1;
    v->order = ++v_order;
    v->vel = (float)vel * (1.0f / 127.0f);
    if (legato && v->on) {               /* legato: only the pitch moves */
        v->gfrom = v->pitch;
        v->gto = (float)note;
        v->gt = 0.0f;
        v->glen = glide > 0.0005f ? glide : 0.0005f;
        v->note = note;
        return;
    }
    {                                    /* glide from the last note: always, or while another key is down */
        int held = 0, i;
        for (i = 0; i < NVOICE; i++)
            held |= V[i].on && V[i].key && &V[i] != v;
        v->gfrom = glide > 0.0005f && mono_last >= 0 && (pc(P_V_GFORCE) || held) ? (float)mono_last : (float)note;
    }
    v->gto = (float)note;
    v->pitch = v->gfrom;
    v->gt = 0.0f;
    v->glen = glide;
    v->note = note;
    v->nrnd = rnd01(&rng_g);
    for (k = 0; k < 6; k++)
        env_gate(&v->env[k], 1);
    if (!v->on) {
        v->amp = 0.0f;
        for (k = 0; k < 6; k++)
            v->env[k].level = 0.0f;
    }
    for (k = 0; k < 8; k++) {
        int mode = pc(P_LFO(k, MODE));
        if (mode != LM_SYNC) {
            v->lfo[k].ph = 0.0f;
            v->lfo[k].done = 0;
        }
        v->lfo[k].age = 0.0f;
    }
    for (k = 0; k < 4; k++)
        if (pc(P_RND(k, MODE)) == 1 || !v->on)
            rnd_seed(&v->rnd[k], xs32(&rng_g));
    for (k = 0; k < NOSC; k++) {
        osc_ctl_t c;
        c.phase = pn[P_OSC(k, PHASE)];
        c.rand_phase = pn[P_OSC(k, RPHASE)];
        c.unison = 1 + pc(P_OSC(k, UNI));
        c.wt = 0;
        osc_on(&v->osc[k], &c);
    }
    {
        smp_ctl_t sc;
        sc.sample = pc(P_S_SAMPLE);
        sc.note = (float)note;
        sc.loop = pc(P_S_LOOP);
        sc.level = 1.0f;
        sc.pan = 0.0f;
        sc.start = 0.0f;
        sc.random_start = pc(P_S_RSTART);
        smp_on(&v->smp, &sc);
    }
    if (!v->on) {
        filt_reset(&VF[v - V][0], pc(P_F1_MODEL), pc(P_F1_STYLE));
        filt_reset(&VF[v - V][1], pc(P_F2_MODEL), pc(P_F2_STYLE));
        for (k = 0; k <= NOSC; k++) {
            uint32_t j;
            for (j = 0; j < CTL; j++)
                v->mono[k][j] = 0.0f;
        }
    }
    v->on = 1;
    v->cost = voice_cost();
    mono_last = note;
}
static RAMC void voice_release(voice_t *v)
{
    int k;
    v->gate = 0;
    for (k = 0; k < 6; k++)
        env_gate(&v->env[k], 0);
}

static RAMC int poly_n(void) { return 1 + pc(P_V_POLY); }
static RAMC int cost_now(void)
{
    int i, c = 0;
    for (i = 0; i < NVOICE; i++)
        if (V[i].on)
            c += V[i].cost;
    return c;
}
/* a voice for a new note: a free one, else the oldest released, else the oldest */
static RAMC voice_t *voice_pick(void)
{
    int i, best = -1, n = 0, lim = poly_n();
    uint32_t bo = 0xFFFFFFFFu;
    for (i = 0; i < NVOICE; i++)
        n += V[i].on;
    if (n < lim && cost_now() + voice_cost() <= budget)
        for (i = 0; i < lim; i++)
            if (!V[i].on)
                return &V[i];
    for (i = 0; i < NVOICE; i++)         /* released ones first */
        if (V[i].on && !V[i].gate && V[i].order < bo) {
            bo = V[i].order;
            best = i;
        }
    if (best < 0)
        for (i = 0; i < NVOICE; i++)
            if (V[i].on && V[i].order < bo) {
                bo = V[i].order;
                best = i;
            }
    if (best < 0)
        best = 0;
    return &V[best];
}

static RAMC void note_on(int note, int vel)
{
    if (poly_n() == 1) {                 /* mono: one voice; legato keeps the envelopes */
        voice_t *v = &V[0];
        int leg = pc(P_V_LEGATO) && v->on && v->gate;
        if (nstack < 16)
            legato_stack[nstack++] = note;
        voice_start(v, note, vel, leg);
        return;
    }
    voice_start(voice_pick(), note, vel, 0);
}
static RAMC void note_off(int note)
{
    int i;
    if (poly_n() == 1) {
        int j, k = 0;
        for (j = 0; j < nstack; j++)
            if (legato_stack[j] != note)
                legato_stack[k++] = legato_stack[j];
        nstack = k;
        if (V[0].on && V[0].note == note) {
            if (nstack > 0) {            /* back to the last key still down */
                voice_start(&V[0], legato_stack[nstack - 1], (int)(V[0].vel * 127.0f), pc(P_V_LEGATO));
            } else {
                V[0].key = 0;
                if (!hold_on)
                    voice_release(&V[0]);
            }
        }
        return;
    }
    for (i = 0; i < NVOICE; i++)
        if (V[i].on && V[i].note == note && V[i].key) {
            V[i].key = 0;
            if (!hold_on)
                voice_release(&V[i]);
        }
}

static RAMC void take_input(void)
{
    int i;
    if (p_pending) {                     /* a new patch: its values, its compiled matrix */
        int k;
        p_r ^= 1u;
        p_pending = 0;
        p_have = 1;
        for (i = 0; i < NPARAM; i++)
            pn[i] = PDESC[i].kind == PK_CHOICE ? (float)PT.v[i] : (float)(PT.v[i] > 1023u ? 1023u : PT.v[i]) * (1.0f / 1023.0f);
        for (i = 0; i < ngd; i++)
            gix[gdst[i]] = 0;
        nvs = ngs = nvd = ngd = 0;
        src_used = 1u << MS_ENV1;
        for (i = 0; i < NSLOT; i++) {
            const mslot_t *m = &PT.m[i];
            cslot_t c;
            int j, dup = 0;
            if (m->src == MS_NONE || m->src >= MS_N || m->dst >= NPARAM || (m->flags & 2u) || !(PDESC[m->dst].flags & PF_MOD))
                continue;
            c.src = m->src;
            c.bip = (uint8_t)(m->flags & 1u);
            c.dst = m->dst;
            c.amt = (float)m->amt * 0.001f;
            c.pw = (float)m->pow * 0.1f;
            src_used |= 1u << m->src;
            if (PDESC[m->dst].flags & PF_VOICE) {
                vsl[nvs++] = c;
                for (j = 0; j < nvd; j++)
                    dup |= vdst[j] == m->dst;
                if (!dup)
                    vdst[nvd++] = m->dst;
            } else {
                gsl[ngs++] = c;
                for (j = 0; j < ngd; j++)
                    dup |= gdst[j] == m->dst;
                if (!dup) {
                    gdst[ngd++] = m->dst;
                    gix[m->dst] = (uint8_t)ngd;
                }
            }
        }
        for (k = 0; k < NOSC; k++)
            o_on[k] = (uint8_t)(pc(P_OSC(k, ON)) != 0);
        s_on = (uint8_t)(pc(P_S_ON) != 0);
        flt_on[0] = (uint8_t)(pc(P_F1_ON) != 0);
        flt_on[1] = (uint8_t)(pc(P_F2_ON) != 0);
        {                                /* mono voices? */
            int mono = 1, was = v_mono;
            for (k = 0; k < NOSC; k++)
                if (o_on[k] && ((pc(P_OSC(k, UNI)) > 0 && PT.v[P_OSC(k, STEREO)] > 0u) || PT.v[P_OSC(k, PAN)] != 512u))
                    mono = 0;
            if (s_on && PT.v[P_S_PAN] != 512u)
                mono = 0;
            for (i = 0; i < nvs; i++) {
                int d = vsl[i].dst;
                if (d == P_S_PAN || ((d >= P_O1_ON && d <= P_O3_DEST) &&
                                     (d - P_O1_ON) % (P_O2_ON - P_O1_ON) == P_O1_PAN - P_O1_ON) ||
                    ((d >= P_O1_ON && d <= P_O3_DEST) && (d - P_O1_ON) % (P_O2_ON - P_O1_ON) == P_O1_STEREO - P_O1_ON))
                    mono = 0;
            }
            v_mono = (uint8_t)mono;
            if (mono != was)             /* (a comb's other line was left as it was: start clean) */
                for (i = 0; i < NVOICE; i++) {
                    filt_reset(&VF[i][0], pc(P_F1_MODEL), pc(P_F1_STYLE));
                    filt_reset(&VF[i][1], pc(P_F2_MODEL), pc(P_F2_STYLE));
                }
        }
        for (k = 0; k < 2; k++) {        /* a filter's model or style changed: reset it in every voice */
            int md = pc(P_FLT(k, MODEL)), st = pc(P_FLT(k, STYLE));
            for (i = 0; i < NVOICE; i++)
                if (VF[i][k].model != md || VF[i][k].style != st)
                    filt_reset(&VF[i][k], md, st);
        }
        for (i = 0; i < NVOICE; i++)
            if (V[i].on)
                V[i].cost = voice_cost();
        tempo = 40.0f + 200.0f * pn[P_V_TEMPO];
    }
    press = in_press;
    wheel = in_wheel;
    bend = in_bend;
    while (cmd_r != cmd_w) {
        uint32_t op = cmd_q[cmd_r & (CMDN - 1u)][0], a = cmd_q[cmd_r & (CMDN - 1u)][1];
        cmd_r++;
        if (!p_have)
            continue;
        if (op == C_KEY) {
            int note = (int)(a & 127u), vel = (int)(a >> 8 & 127u);
            if (vel)
                note_on(note, vel);
            else
                note_off(note);
        } else if (op == C_HOLD) {
            hold_on = (int)a;
            if (!hold_on)
                for (i = 0; i < NVOICE; i++)
                    if (V[i].on && V[i].gate && !V[i].key)
                        voice_release(&V[i]);
        } else {
            for (i = 0; i < NVOICE; i++)
                V[i].on = 0;
            hold_on = 0;
            nstack = 0;
        }
    }
}

void core_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t t = st & 0xF0u;
    if (t == 0x90u)
        core_key((int)d1, (int)d2);
    else if (t == 0x80u)
        core_key((int)d1, 0);
    else if (t == 0xD0u)
        in_press = (float)d1 * (1.0f / 127.0f);
    else if (t == 0xE0u)
        in_bend = ((float)(d1 | d2 << 7) - 8192.0f) * (1.0f / 8192.0f);
    else if (t == 0xB0u && d1 == 1u)
        in_wheel = (float)d2 * (1.0f / 127.0f);
    else if (t == 0xB0u && d1 == 64u)
        core_hold(d2 >= 64u);
}

void core_overload(void)
{
    int b = budget * 4 / 5;
    budget = b < 6 ? 6 : b;
    CORE_VIEW.shed++;
}
void core_cpu(float load)                /* main loop, about once a second: room again: a little more */
{
    CORE_VIEW.cpu = load;
    if (load < 0.6f && budget < 96)
        budget += 2;
    CORE_VIEW.budget = budget;
}

/* --------------------------------------------------------------- one voice's block --- */
static RAMC void add2(float *dl, float *dr, const float *sl, const float *sr, uint32_t n)
{
    uint32_t j;
    for (j = 0; j < n; j++) {
        dl[j] += sl[j];
        dr[j] += sr[j];
    }
}
static RAMC void zero(float *d, uint32_t n)
{
    uint32_t j;
    for (j = 0; j < n; j++)
        d[j] = 0.0f;
}

/* a filter with its dry / wet mix (tmp: n floats of scratch) */
static RAMC void filt_run_mix(filt_t *f, float *L, float *R, float *tmp, uint32_t n, float mix)
{
    uint32_t j;
    if (mix >= 0.999f) {
        filt_run(f, L, R, n);
        return;
    }
    for (j = 0; j < n; j++)
        tmp[j] = L[j];
    filt_run(f, L, R, n);
    for (j = 0; j < n; j++)
        L[j] = tmp[j] + (L[j] - tmp[j]) * mix;
}

static RAMC void voice_mods(voice_t *v, float *vm, float dt)
{
    int k;
    float pr[9];
    uint32_t used = src_used;
    for (k = 0; k < 6; k++) {            /* envelopes (ENV 1 is the amplitude: always) */
        int j;
        if (!(used & (1u << (MS_ENV1 + k))))
            continue;
        for (j = 0; j < 9; j++)
            pr[j] = pv(vm, P_ENV(k, DELAY) + j);
        env_run(&v->env[k], pr, dt);
        v->src[MS_ENV1 + k] = v->env[k].level;
    }
    for (k = 0; k < 8; k++) {
        if (!(used & (1u << (MS_LFO1 + k))))
            continue;
        pr[0] = pv(vm, P_LFO(k, RATE));
        pr[1] = pv(vm, P_LFO(k, PHASE));
        pr[2] = pv(vm, P_LFO(k, FADE));
        pr[3] = pv(vm, P_LFO(k, DELAY));
        pr[4] = pv(vm, P_LFO(k, SMOOTH));
        pr[5] = pv(vm, P_LFO(k, AMP));
        lfo_run(&v->lfo[k], pr, pc(P_LFO(k, SHAPE)), pc(P_LFO(k, SYNC)), pc(P_LFO(k, MODE)), PT.lfo_y[k], PT.lfo_c[k],
                tempo, clock_s, dt, v->gate);
        v->src[MS_LFO1 + k] = v->lfo[k].out;
    }
    for (k = 0; k < 4; k++) {
        if (!(used & (1u << (MS_RND1 + k))))
            continue;
        rnd_run(&v->rnd[k], pc(P_RND(k, STYLE)), lfo_hz_r(pv(vm, P_RND(k, RATE)), pc(P_RND(k, SYNC)), tempo),
                pv(vm, P_RND(k, AMP)), dt);
        v->src[MS_RND1 + k] = v->rnd[k].out;
    }
    for (k = 0; k < 4; k++)
        v->src[MS_MAC1 + k] = pn[P_MC1 + k];
    v->src[MS_VEL] = v->vel;
    v->src[MS_PRESS] = press;
    v->src[MS_NOTE] = (float)v->note * (1.0f / 127.0f);
    {
        int o = v->note;
        while (o >= 12)
            o -= 12;
        v->src[MS_NOTEOCT] = (float)o * (1.0f / 11.0f);
    }
    v->src[MS_WHEEL] = wheel;
    v->src[MS_BEND] = bend;
    v->src[MS_RANDOM] = v->nrnd;
}

static RAMC void voice_block(int vi, uint32_t n, int core)   /* core: whose scratch (0 the first, 1 the second) */
{
    voice_t *v = &V[vi];
    int k, route = pc(P_F_ROUTE);
    float *vm = vmod[core], (*b)[CTL] = scr[core], dt = (float)n * ISR, amp1, a0, da, pitch, vt, e1;
    uint32_t j, has = 0;                 /* bit per buffer pair with something in it */
    for (k = 0; k < B_N; k++)
        zero(b[k], n);
    matrix_voice(v->src, vm);            /* the modulators run with the last block's sources ... */
    voice_mods(v, vm, dt);
    matrix_voice(v->src, vm);            /* ... and the sound with this block's */
    if (v->glen > 0.0f && v->gt < v->glen) {   /* glide (Vital's slope: a power curve over the time) */
        v->gt += dt;
        v->pitch = v->gfrom + (v->gto - v->gfrom) * power_scale(clampf(v->gt / v->glen, 0.0f, 1.0f), (pn[P_V_GSLOPE] - 0.5f) * 12.0f);
    } else {
        v->pitch = v->gto;
    }
    {
        pitch = v->pitch + bend * pn[P_V_BEND] + (pv(vm, P_V_TUNE) - 0.5f) * 2.0f;
    }
    if (s_on) {                        /* the sample first: the oscillators may FM / RM from it */
        smp_ctl_t sc;
        int d = pc(P_S_DEST);
        float *L = d == 1 ? b[B_F2L] : d == 2 ? b[B_BOL] : d == 3 ? b[B_FXL] : d == 4 ? b[B_DL] : b[B_F1L];
        float *R = L + CTL;
        float lv = pv(vm, P_S_LEVEL);
        sc.sample = pc(P_S_SAMPLE);
        sc.note = (pc(P_S_KEY) ? pitch : 60.0f) + fm_floorf((pn[P_S_OCT] - 0.5f) * 96.0f + 0.5f) + vm[P_S_OCT] * 96.0f +
                  (pv(vm, P_S_TUNE) - 0.5f) * 2.0f;
        sc.loop = pc(P_S_LOOP);
        sc.level = lv * lv;
        sc.pan = (pv(vm, P_S_PAN) - 0.5f) * 2.0f;
        sc.start = 0.0f;
        sc.random_start = pc(P_S_RSTART);
        smp_run(&v->smp, &sc, L, R, v->mono[NOSC], n);
        has |= 1u << (d > 4 ? 0 : d);
    }
    for (k = 0; k < NOSC; k++) {
        osc_ctl_t c;
        int d, w, ka, kb;
        const float *src = 0;
        float *L, *R, lv;
        if (!o_on[k])
            continue;
        d = pc(P_OSC(k, DEST));
        L = d == 1 ? b[B_F2L] : d == 2 ? b[B_BOL] : d == 3 ? b[B_FXL] : d == 4 ? b[B_DL] : b[B_F1L];
        R = L + CTL;
        c.wt = tab[k];
        c.note = pitch + fm_floorf((pn[P_OSC(k, OCT)] - 0.5f) * 96.0f + 0.5f) + vm[P_OSC(k, OCT)] * 96.0f +
                 (pv(vm, P_OSC(k, TUNE)) - 0.5f) * 2.0f;
        c.frame = pv(vm, P_OSC(k, FRAME)) * (float)(WT_FRAMES - 1);
        c.frame_spread = pv(vm, P_OSC(k, FSPREAD));
        c.unison = 1 + pc(P_OSC(k, UNI));
        c.detune = pv(vm, P_OSC(k, DET));
        c.detune_power = (pv(vm, P_OSC(k, DPOW)) - 0.5f) * 2.0f;
        c.blend = pv(vm, P_OSC(k, BLEND));
        c.spread = pv(vm, P_OSC(k, STEREO));
        c.stack = pc(P_OSC(k, STACK));
        c.warp = w = pc(P_OSC(k, WARP));
        c.warp_amt = pv(vm, P_OSC(k, WAMT));
        lv = pv(vm, P_OSC(k, LEVEL));
        c.level = lv * lv;
        c.pan = (pv(vm, P_OSC(k, PAN)) - 0.5f) * 2.0f;
        c.phase = pn[P_OSC(k, PHASE)];
        c.rand_phase = pn[P_OSC(k, RPHASE)];
        ka = k == 0 ? 1 : 0;             /* "A" and "B": the other two, in order */
        kb = k == 2 ? 1 : 2;
        if (w == WARP_FM_A || w == WARP_RM_A)
            src = v->mono[ka];
        else if (w == WARP_FM_B || w == WARP_RM_B)
            src = v->mono[kb];
        else if (w == WARP_FM_SMP || w == WARP_RM_SMP)
            src = v->mono[NOSC];
        osc_run(&v->osc[k], &c, src, L, R, v->mono[k], n);
        has |= 1u << (d > 4 ? 0 : d);
    }
    /* the filters */
    {
        float *i1l = b[B_F1L], *i1r = b[B_F1R], *i2l = b[B_F2L], *i2r = b[B_F2R];
        int run1, run2;
        if (has & 4u) {                  /* "both" into each */
            add2(i1l, i1r, b[B_BOL], b[B_BOR], n);
            if (route == 0)
                add2(i2l, i2r, b[B_BOL], b[B_BOR], n);
            has |= 1u;
        }
        run1 = (has & 1u) != 0;
        if (run1 && flt_on[0]) {
            filt_ctl_t fc;
            float mix = pv(vm, P_F1_MIX);
            fc.cutoff = 8.0f + 128.0f * pv(vm, P_F1_CUT) + (pv(vm, P_F1_KEY) - 0.5f) * 2.0f * (pitch - 60.0f);
            fc.reso = pv(vm, P_F1_RESO);
            fc.drive = pv(vm, P_F1_DRIVE);
            fc.blend = pv(vm, P_F1_BLEND);
            fc.fx = pv(vm, P_F1_FX);
            fc.fy = pv(vm, P_F1_FY);
            fc.transpose = (pv(vm, P_F1_TRANS) - 0.5f) * 24.0f;
            filt_ctl(&VF[vi][0], &fc, n);
            if (v_mono) {                /* one channel, then copied */
                filt_run_mix(&VF[vi][0], i1l, i1l, b[B_BOL], n, mix);
                for (j = 0; j < n; j++)
                    i1r[j] = i1l[j];
            } else if (mix >= 0.999f) {
                filt_run(&VF[vi][0], i1l, i1r, n);
            } else {
                float *tl = b[B_BOL], *tr = b[B_BOR];
                for (j = 0; j < n; j++) {
                    tl[j] = i1l[j];
                    tr[j] = i1r[j];
                }
                filt_run(&VF[vi][0], i1l, i1r, n);
                for (j = 0; j < n; j++) {
                    i1l[j] = tl[j] + (i1l[j] - tl[j]) * mix;
                    i1r[j] = tr[j] + (i1r[j] - tr[j]) * mix;
                }
            }
        }
        if (route == 1 && run1) {        /* serial: filter 1 into filter 2 */
            add2(i2l, i2r, i1l, i1r, n);
            has |= 2u;
            zero(i1l, n);
            zero(i1r, n);
        }
        run2 = (has & 2u) != 0;
        if (run2 && flt_on[1]) {
            filt_ctl_t fc;
            float mix = pv(vm, P_F2_MIX);
            fc.cutoff = 8.0f + 128.0f * pv(vm, P_F2_CUT) + (pv(vm, P_F2_KEY) - 0.5f) * 2.0f * (pitch - 60.0f);
            fc.reso = pv(vm, P_F2_RESO);
            fc.drive = pv(vm, P_F2_DRIVE);
            fc.blend = pv(vm, P_F2_BLEND);
            fc.fx = pv(vm, P_F2_FX);
            fc.fy = pv(vm, P_F2_FY);
            fc.transpose = (pv(vm, P_F2_TRANS) - 0.5f) * 24.0f;
            filt_ctl(&VF[vi][1], &fc, n);
            if (v_mono) {                /* one channel, then copied */
                filt_run_mix(&VF[vi][1], i2l, i2l, b[B_BOL], n, mix);
                for (j = 0; j < n; j++)
                    i2r[j] = i2l[j];
            } else if (mix >= 0.999f) {
                filt_run(&VF[vi][1], i2l, i2r, n);
            } else {
                float *tl = b[B_BOL], *tr = b[B_BOR];
                for (j = 0; j < n; j++) {
                    tl[j] = i2l[j];
                    tr[j] = i2r[j];
                }
                filt_run(&VF[vi][1], i2l, i2r, n);
                for (j = 0; j < n; j++) {
                    i2l[j] = tl[j] + (i2l[j] - tl[j]) * mix;
                    i2r[j] = tr[j] + (i2r[j] - tr[j]) * mix;
                }
            }
        }
    }
    /* the amplitude (Vital: ENV 1 with the velocity, squared), per sample between the blocks */
    vt = pn[P_V_VEL];
    e1 = v->env[0].level * (1.0f - vt + vt * v->vel);
    amp1 = e1 * e1;
    a0 = v->amp;
    da = (amp1 - a0) / (float)n;
    {
        float *ol = vout[vi][0], *orr = vout[vi][1], *dl = vout[vi][2], *dr = vout[vi][3];
        for (j = 0; j < n; j++) {
            float a = a0 + da * (float)(j + 1u);
            ol[j] = (b[B_F1L][j] + b[B_F2L][j] + b[B_FXL][j]) * a;
            orr[j] = (b[B_F1R][j] + b[B_F2R][j] + b[B_FXR][j]) * a;
            dl[j] = b[B_DL][j] * a;
            dr[j] = b[B_DR][j] * a;
        }
    }
    v->amp = amp1;
    vplayed[vi] = 1;
    if (v->env[0].stage == ES_OFF) {     /* done */
        v->on = 0;
        v->amp = 0.0f;
    }
}

/* the second core's share: the odd voices */
static uint32_t job_n;
static RAMC void job_odd(uint32_t arg)
{
    int i;
    (void)arg;
    for (i = 1; i < NVOICE; i += 2)
        if (V[i].on)
            voice_block(i, job_n, 1);
}

/* ---------------------------------------------------------------------- render --- */
static float fxp[FX_NP];
RAMC void core_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t o, j;
    int i, k;
    take_input();
    if (!p_have) {
        for (j = 0; j < 2u * n; j++)
            lr[j] = 0;
        return;
    }
    for (k = 0; k < NOSC; k++) {
        tab_used[k][1] = tab_used[k][0];
        tab_used[k][0] = tab[k];
    }
    for (o = 0; o < n; o += CTL) {
        uint32_t m = n - o < CTL ? n - o : CTL;
        float L[CTL], R[CTL], DL[CTL], DR[CTL], pk = 0.0f, dt = (float)m * ISR;
        int handed, newest = -1;
        uint32_t no = 0;
        clock_s += dt;
        if (clock_s > 100000.0f)
            clock_s -= 100000.0f;
        job_n = m;
        handed = plat_cpu2_run(job_odd, 0);
        for (i = 0; i < NVOICE; i += 2)
            if (V[i].on)
                voice_block(i, m, 0);
        if (!handed || plat_cpu2_wait() < 0)
            job_odd(0);
        for (j = 0; j < m; j++)
            L[j] = R[j] = DL[j] = DR[j] = 0.0f;
        for (i = 0; i < NVOICE; i++) {   /* the voices that sounded this block (one may have ended in it) */
            if (vplayed[i]) {
                add2(L, R, vout[i][0], vout[i][1], m);
                add2(DL, DR, vout[i][2], vout[i][3], m);
                vplayed[i] = 0;
            }
            if (V[i].on && V[i].order > no) {
                no = V[i].order;
                newest = i;
            }
        }
        /* the global modulation: the newest voice's sources (Vital modulates its effects the same way) */
        {
            const float *s = newest >= 0 ? V[newest].src : 0;
            float s0[MS_N];
            if (!s) {
                for (k = 0; k < MS_N; k++)
                    s0[k] = 0.0f;
                for (k = 0; k < 4; k++)
                    s0[MS_MAC1 + k] = pn[P_MC1 + k];
                s0[MS_WHEEL] = wheel;
                s0[MS_BEND] = bend;
                s0[MS_PRESS] = press;
                s = s0;
            }
            for (k = 0; k < ngd; k++)
                gmv[k] = 0.0f;
            for (k = 0; k < ngs; k++)
                gmv[gix[gsl[k].dst] - 1u] += src_val(s, &gsl[k]);
        }
        /* the effect chain, in the patch's order */
        for (k = 0; k < FXT_N; k++) {
            int t = PT.fx_order[k], on, pi;
            if (t < 0 || t >= FXT_N)
                continue;
            on = (int)pn[P_FX(t)] != 0;
            if (on && !fx_was_on[t])
                fx_reset(t);
            fx_was_on[t] = on;
            if (!on)
                continue;
            for (pi = 0; pi < FX_NP; pi++) {
                int id = P_FX(t) + 1 + pi;
                fxp[pi] = clampf(pn[id] + gmod(id), 0.0f, 1.0f);
            }
            fx_run(t, fxp, tempo, L, R, m);
        }
        {
            float vol = clampf(pn[P_V_VOLUME] + gmod(P_V_VOLUME), 0.0f, 1.0f);
            vol = vol * vol * 1.4f;
            for (j = 0; j < m; j++) {
                float l = fm_tanhf((L[j] + DL[j]) * vol), r = fm_tanhf((R[j] + DR[j]) * vol);
                if (fm_fabsf(l) > pk)
                    pk = fm_fabsf(l);
                if (fm_fabsf(r) > pk)
                    pk = fm_fabsf(r);
                lr[2u * (o + j)] = (int32_t)(l * gain);
                lr[2u * (o + j) + 1u] = (int32_t)(r * gain);
            }
        }
        /* the view */
        {
            int nv = 0;
            uint16_t mask = 0;
            for (i = 0; i < NVOICE; i++)
                if (V[i].on) {
                    nv++;
                    mask |= (uint16_t)(1u << i);
                    CORE_VIEW.vnote[i] = (uint8_t)V[i].note;
                }
            CORE_VIEW.voices = nv;
            CORE_VIEW.vmask = mask;
            CORE_VIEW.active = newest >= 0;
            if (pk > CORE_VIEW.out_peak)
                CORE_VIEW.out_peak = pk;
            if (newest >= 0) {
                voice_t *v = &V[newest];
                for (k = 0; k < MS_N; k++)
                    CORE_VIEW.src[k] = v->src[k];
                matrix_voice(v->src, vmod[0]);
                for (k = 0; k < nvd; k++)
                    CORE_VIEW.live[vdst[k]] = (uint16_t)(pv(vmod[0], vdst[k]) * 65535.0f);
                for (k = 0; k < 6; k++) {
                    float pr[9];
                    int q;
                    for (q = 0; q < 9; q++)
                        pr[q] = pv(vmod[0], P_ENV(k, DELAY) + q);
                    CORE_VIEW.env_stage[k] = env_stage_view(&v->env[k], pr);
                }
                for (k = 0; k < 8; k++)
                    CORE_VIEW.lfo_ph[k] = (float)v->lfo[k].rel * (1.0f / 65535.0f);
                for (k = 0; k < NOSC; k++)
                    CORE_VIEW.frame[k] = pv(vmod[0], P_OSC(k, FRAME));
            }
            for (k = 0; k < ngd; k++)
                CORE_VIEW.live[gdst[k]] = (uint16_t)(clampf(pn[gdst[k]] + gmv[k], 0.0f, 1.0f) * 65535.0f);
        }
    }
    {                                    /* the random LFOs' traces, about 40 a second */
        static uint32_t tick;
        tick += n;
        if (tick >= 1100u) {
            int newest = -1;
            uint32_t no = 0;
            tick = 0;
            for (i = 0; i < NVOICE; i++)
                if (V[i].on && V[i].order > no) {
                    no = V[i].order;
                    newest = i;
                }
            for (k = 0; k < 4; k++) {
                for (j = 0; j < 31u; j++)
                    CORE_VIEW.rnd_hist[k][j] = CORE_VIEW.rnd_hist[k][j + 1u];
                CORE_VIEW.rnd_hist[k][31] = newest >= 0 ? (uint8_t)(clampf(V[newest].rnd[k].out, 0.0f, 1.0f) * 255.0f) : CORE_VIEW.rnd_hist[k][31];
            }
        }
    }
}

void core_init(void)
{
    int i;
    static int16_t a_cho[FXA_CHORUS] PLAT_POOL, a_dly[FXA_DELAY] PLAT_POOL, a_flg[FXA_FLANGER] PLAT_POOL,
        a_rev[FXA_REVERB] PLAT_POOL;
    for (i = 0; i < NVOICE; i++) {
        uint8_t *z = (uint8_t *)(void *)&V[i];
        uint32_t q;
        for (q = 0; q < sizeof(voice_t); q++)
            z[q] = 0;
        PLAT_FAR(filt_reset)(&VF[i][0], 0, 0);   /* (RAM code, from the flash) */
        PLAT_FAR(filt_reset)(&VF[i][1], 0, 0);
        V[i].osc[0].rng = 0x1234567u + (uint32_t)i * 7919u;
    }
    fx_init(a_cho, a_dly, a_flg, a_rev);
    for (i = 0; i < FXT_N; i++)
        fx_was_on[i] = 0;
    p_have = 0;
    p_pending = 0;
    cmd_r = cmd_w = 0;
    budget = 64;
    CORE_VIEW.budget = budget;
}
