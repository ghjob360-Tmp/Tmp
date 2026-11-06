/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the sequencers (included by core.c). Each instance (a track's synth or drum sound) has one, of
 * seven types; while it is on it takes that instance's keys: they transpose it (C4 = as written), gate it, or feed
 * it. Its notes go to the instance like the keys' would, and onto the tape while recording.
 *   LOOP   up to 128 entered steps (notes, rests, ties)       GRID  16 steps, a note each
 *   SPIN   the keys drop balls into a turning hexagon; a ball hitting a wall plays its note
 *   HANDS  every key holds a pattern: hold it to play it, a second key plays its own beside it
 *   DRAW   32 points drawn with the knobs, played as a melody in a scale
 *   ARP    the held notes, arpeggiated                        LATCH  keys stay on until pressed again */

typedef struct {
    uint8_t held[16], nheld, latched, running, gated, tie_next;
    int root;
    float clock, len, gate_at;
    int step, dir;
    uint32_t count, rng;
    uint8_t snd[8], nsnd;                /* the notes it has on */
    uint8_t lat[16], nlat;               /* LATCH: the notes held on */
    /* SPIN */
    float bx[8], by[8], vx[8], vy[8], boff[8], cool[8], ang;
    uint8_t bnote[8], bvel[8], bon[8], bn;
} sq_t;
static sq_t SQ[NINST];

static const uint8_t TRIG[8] = {0xFF, 0x55, 0x77, 0x6D, 0x49, 0xDB, 0xAF, 0x11};
static const uint8_t DIVT[7] = {96, 64, 48, 32, 24, 16, 12};   /* 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 */
/* HANDS: seven patterns (one per white key), 16 steps; synth: semitones from the key (-128 rest), drums: keys */
static const int8_t HANDS_SYN[7][16] = {
    {0, -128, 12, -128, 0, 7, -128, 12, 0, -128, 12, 7, -128, 0, 10, -128},
    {0, 3, 7, 12, 7, 3, 0, -128, 0, 3, 7, 10, 7, 3, -128, -128},
    {0, -128, -128, 0, -128, -128, 0, -128, 5, -128, -128, 5, -128, 7, -128, -128},
    {12, 7, 3, 0, 12, 7, 3, 0, 15, 10, 7, 3, 14, 10, 7, 2},
    {0, 0, 12, 0, 0, 12, 0, 12, 0, 0, 12, 0, 10, 12, 7, 12},
    {0, -128, 7, -128, 12, -128, 7, -128, 3, -128, 7, -128, 10, -128, 7, -128},
    {0, 2, 3, 5, 7, 8, 10, 12, 10, 8, 7, 5, 3, 2, 0, -128}};
static const int8_t HANDS_DRM[7][16] = {
    {0, -128, 7, -128, 2, -128, 7, -128, 0, 0, 7, -128, 2, -128, 7, 8},
    {0, 7, 7, 7, 2, 7, 7, 7, 0, 7, 7, 0, 2, 7, 7, 7},
    {0, -128, -128, 0, -128, -128, 0, -128, 2, -128, 0, -128, -128, 0, 2, -128},
    {0, 9, 7, 9, 2, 9, 7, 9, 0, 9, 7, 9, 2, 9, 5, 9},
    {11, 12, 13, 11, 12, 13, 14, 15, 11, 12, 13, 11, 12, 13, 16, 17},
    {0, -128, -128, -128, 5, -128, -128, -128, 0, -128, 0, -128, 5, -128, -128, 6},
    {18, 19, 18, 20, 18, 19, 21, 22, 18, 19, 18, 20, 23, 21, 22, 19}};
static const int8_t SCALES[5][7] = {{0, 1, 2, 3, 4, 5, 6}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10},
                                    {0, 2, 4, 7, 9, 12, 14}, {0, 3, 5, 7, 10, 12, 15}};

static void seq_init(void)
{
    int i;
    for (i = 0; i < NINST; i++)
        SQ[i].rng = 0xC0FFEE00u + (uint32_t)i * 131u;
}

INL const mod_t *qmod(int i) { return &snd(i)->m[M_SEQ]; }
INL const seqdata_t *qdat(int i) { return (i & 1) ? &P.t[i >> 1].seq_drum : &P.t[i >> 1].seq_synth; }

static RAMC void seq_emit(int i, int note, int vel)
{
    sq_t *q = &SQ[i];
    if (note < 0 || note > 127)
        return;
    if (vel > 0 && q->nsnd < 8)
        q->snd[q->nsnd++] = (uint8_t)note;
    inst_note(i, note, vel, 0.0f);
    tape_rec(i, note, vel);
}

static RAMC void seq_all_off(int i)
{
    sq_t *q = &SQ[i];
    int k;
    for (k = 0; k < q->nsnd; k++) {
        inst_note(i, q->snd[k], 0, 0.0f);
        tape_rec(i, q->snd[k], 0);
    }
    q->nsnd = 0;
}

/* a key on an instance whose sequencer is on: 1 = the sequencer has it */
static RAMC int seq_key(int i, int note, int vel)
{
    const mod_t *m = qmod(i);
    sq_t *q = &SQ[i];
    int k, n;
    if (!m->on)
        return 0;
    switch (m->type % Q_N) {
    case Q_SPIN:                         /* a ball with the note, dropped in from the top */
        if (vel > 0) {
            int b = q->bn < 8 ? q->bn++ : (int)(xs32(&q->rng) & 7u);
            q->bx[b] = rnd1(&q->rng) * 0.2f;
            q->by[b] = 0.5f;
            q->vx[b] = rnd1(&q->rng) * 0.4f;
            q->vy[b] = 0.0f;
            q->bnote[b] = (uint8_t)note;
            q->bvel[b] = (uint8_t)vel;
            q->cool[b] = 0.0f;
        }
        return 1;
    case Q_LATCH: {
        int split = 36 + (int)(kn(m->p[0]) * 48.0f), tr = (int)((kn(m->p[2]) - 0.5f) * 24.0f);
        if (note < split && kn(m->p[0]) > 0.02f)
            return 0;                    /* below the split: played as ever */
        if (vel <= 0)
            return 1;
        for (k = 0; k < q->nlat; k++)
            if (q->lat[k] == (uint8_t)note)
                break;
        if (k < q->nlat) {               /* on again: off */
            inst_note(i, note + tr, 0, 0.0f);
            tape_rec(i, note + tr, 0);
            for (n = k; n + 1 < q->nlat; n++)
                q->lat[n] = q->lat[n + 1];
            q->nlat--;
            return 1;
        }
        if (kn(m->p[1]) > 0.5f) {        /* mono: one at a time */
            for (k = 0; k < q->nlat; k++) {
                inst_note(i, q->lat[k] + tr, 0, 0.0f);
                tape_rec(i, q->lat[k] + tr, 0);
            }
            q->nlat = 0;
        }
        if (q->nlat < 16) {
            q->lat[q->nlat++] = (uint8_t)note;
            inst_note(i, note + tr, vel, 0.0f);
            tape_rec(i, note + tr, vel);
        }
        return 1;
    }
    default:
        break;
    }
    /* the clocked ones: the keys held, in order */
    if (vel > 0) {
        for (k = n = 0; k < q->nheld; k++)
            if (q->held[k] != (uint8_t)note)
                q->held[n++] = q->held[k];
        if (n > 15)
            n = 15;
        q->held[n++] = (uint8_t)note;
        q->nheld = (uint8_t)n;
        q->root = note;
        if (kn(m->p[3]) > 0.5f || (m->type % Q_N) == Q_DRAW)
            q->latched = 1;
        if (!q->running)                 /* it starts at once, on its first step */
            q->clock = 1e9f;
        if ((m->type % Q_N) == Q_ARP && kn(m->p[1]) > 0.25f)
            q->step = -1;                /* trigger modes once / each: from the start on a new key */
    } else {
        for (k = n = 0; k < q->nheld; k++)
            if (q->held[k] != (uint8_t)note)
                q->held[n++] = q->held[k];
        q->nheld = (uint8_t)n;
        if (n)
            q->root = q->held[n - 1];
    }
    return 1;
}

static RAMC void seq_input(int i, int note) { (void)i; (void)note; }   /* entry is the main loop's (seqdata) */
static RAMC void seq_rest(int i, int tie) { (void)i; (void)tie; }
static RAMC void seq_delete(int i) { (void)i; }
static RAMC void seq_toggle(int i)
{
    seq_all_off(i);
    SQ[i].running = 0;
    SQ[i].latched = 0;
    SQ[i].nheld = 0;
}

/* the notes of the step that starts now, into out[] (<= 8); returns how many; *tie: hold the last ones */
static RAMC int seq_step_notes(int i, const mod_t *m, sq_t *q, int *out, int *tie)
{
    const seqdata_t *d = qdat(i);
    int tr = (i & 1) ? 0 : q->root - 60, n = 0, s, len, k;
    *tie = 0;
    switch (m->type % Q_N) {
    case Q_LOOP:
        len = d->loop_n;
        if (!len)
            return 0;
        {
            int dirm = (int)(kn(m->p[4]) * 3.99f), rot = (int)(kn(m->p[5]) * (float)(len - 1) + 0.5f);
            if (dirm == 1)
                s = len - 1 - (int)(q->count % (uint32_t)SEQ_STEPS) % len;
            else if (dirm == 2 && len > 1) {
                int per = 2 * len - 2, c = (int)(q->count % (uint32_t)(2 * SEQ_STEPS - 2)) % per;
                s = c < len ? c : per - c;
            } else if (dirm == 3)
                s = (int)(xs32(&q->rng) & 127u) % len;
            else
                s = (int)(q->count % (uint32_t)SEQ_STEPS) % len;
            s = (s + rot) % len;
        }
        q->step = s;
        if (!(TRIG[(int)(kn(m->p[2]) * 7.99f)] >> (q->count & 7u) & 1u))
            return 0;
        if (d->loop[s] == SEQ_TIE) {
            *tie = 1;
            return 0;
        }
        if (d->loop[s] == SEQ_REST)
            return 0;
        out[n++] = d->loop[s] + tr;
        return n;
    case Q_GRID:
        len = 1 + (int)(kn(m->p[2]) * 15.99f);
        if (kn(m->p[6]) > 0.5f)          /* backwards */
            s = len - 1 - (int)(q->count % 16u) % len;
        else
            s = (int)(q->count % 16u) % len;
        q->step = s;
        if (d->grid[s])
            out[n++] = d->grid[s] + tr;
        return n;
    case Q_HANDS: {
        static const uint8_t PC_TO_PAT[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
        float dens = kn(m->p[2]);
        len = 4 + (int)(kn(m->p[1]) * 12.99f);
        s = (int)(q->count % 16u) % len;
        q->step = s;
        for (k = 0; k < q->nheld && k < 2; k++) {
            int key = q->held[q->nheld - 1 - k], pat = PC_TO_PAT[key % 12];
            int8_t v = (i & 1) ? HANDS_DRM[pat][s] : HANDS_SYN[pat][s];
            uint32_t h = (uint32_t)(s * 2654435761u + (uint32_t)pat * 40503u) >> 24;
            if (v == -128 || (float)h > dens * 255.0f + 40.0f)
                continue;
            out[n++] = (i & 1) ? 60 + v : key + v;
        }
        return n;
    }
    case Q_DRAW: {
        int sc = (int)(kn(m->p[5]) * 4.99f), deg;
        float range = 2.0f + kn(m->p[6]) * 22.0f, v;
        s = (int)(q->count % 32u);
        q->step = s;
        v = ((float)d->draw[s] - 64.0f) / 64.0f * range;   /* semitones, then into the scale */
        if (sc == 0) {
            out[n++] = q->root + (int)(v + (v < 0.0f ? -0.5f : 0.5f));
        } else {
            float deg_f = v * 7.0f / 12.0f;
            int oct;
            deg = (int)fm_floorf(deg_f + 0.5f);
            oct = deg >= 0 ? deg / 7 : -((-deg + 6) / 7);
            out[n++] = (i & 1) ? 60 + ((deg % 24 + 24) % 24) : q->root + oct * 12 + SCALES[sc][deg - oct * 7];
        }
        return n;
    }
    case Q_ARP: {
        int list[48], ln = 0, src[16], sn = 0, type = (int)(kn(m->p[5]) * 3.99f), octs = 1 + (int)(kn(m->p[6]) * 2.99f), o2;
        static const int8_t ADD2[4] = {0, 7, 9, -5}, ADD3[6] = {0, 2, 4, 5, 7, 9};
        for (k = 0; k < q->nheld; k++)   /* the held notes, low to high */
            src[sn++] = q->held[k];
        if (!sn && q->latched && q->root)
            src[sn++] = q->root;
        for (k = 1; k < sn; k++) {
            int x = src[k], j = k;
            while (j > 0 && src[j - 1] > x) {
                src[j] = src[j - 1];
                j--;
            }
            src[j] = x;
        }
        for (o2 = 0; o2 < octs; o2++)
            for (k = 0; k < sn; k++) {
                int a2;
                if (type == 2)
                    for (a2 = 0; a2 < 4 && ln < 48; a2++)
                        list[ln++] = src[k] + ADD2[a2] + 12 * o2;
                else if (type == 3)
                    for (a2 = 0; a2 < 6 && ln < 48; a2++)
                        list[ln++] = src[k] + ADD3[a2] + 12 * o2;
                else if (ln < 48)
                    list[ln++] = src[k] + 12 * o2;
            }
        if (!ln)
            return 0;
        q->step++;
        if (type == 1 && ln > 1) {       /* up and down */
            int per = 2 * ln - 2, c = q->step % per;
            s = c < ln ? c : per - c;
        } else {
            if (q->step >= ln && kn(m->p[1]) > 0.25f && kn(m->p[1]) < 0.5f) {   /* once: then it stops */
                q->step = ln;
                return 0;
            }
            s = q->step % ln;
        }
        if (!(TRIG[(int)(kn(m->p[2]) * 7.99f)] >> (q->count & 7u) & 1u))
            return 0;
        out[n++] = list[s];
        return n;
    }
    default:
        return 0;
    }
}

static RAMC void spin_run(int i, const mod_t *m, sq_t *q, float dt)
{
    float spd = (kn(m->p[0]) - 0.5f) * 2.0f, g = kn(m->p[1]) * 3.0f, open = kn(m->p[2]), e = 0.3f + 0.65f * kn(m->p[3]);
    int b, k;
    q->ang = frac1(q->ang + spd * 0.4f * dt);
    for (b = 0; b < q->bn; b++) {
        if (q->bon[b]) {                 /* a hit's note: off after 120 ms */
            q->boff[b] -= dt;
            if (q->boff[b] <= 0.0f) {
                q->bon[b] = 0;
                inst_note(i, q->bnote[b], 0, 0.0f);
                tape_rec(i, q->bnote[b], 0);
            }
        }
        q->cool[b] -= dt;
        q->vy[b] -= g * dt;
        q->bx[b] += q->vx[b] * dt;
        q->by[b] += q->vy[b] * dt;
        for (k = 0; k < 6; k++) {        /* the six walls, turning */
            float a = q->ang + ((float)k + 0.5f) / 6.0f, nx = fm_sin_turns(a), ny = fm_sin_turns(a + 0.25f),
                  dist = q->bx[b] * nx + q->by[b] * ny;
            if (dist > 0.82f) {
                float tx = -ny, ty = nx, along = q->bx[b] * tx + q->by[b] * ty, vn = q->vx[b] * nx + q->vy[b] * ny;
                if (k == 4 && fm_fabsf(along) < open * 0.45f)
                    continue;            /* through the opening */
                if (vn > 0.0f) {
                    q->vx[b] -= (1.0f + e) * vn * nx;
                    q->vy[b] -= (1.0f + e) * vn * ny;
                    q->vx[b] += tx * spd * 0.6f;   /* the turning wall throws it on */
                    q->vy[b] += ty * spd * 0.6f;
                    if (q->cool[b] <= 0.0f && vn > 0.08f) {
                        int vel = (int)clampf(vn * 90.0f * (float)q->bvel[b] / 100.0f, 10.0f, 127.0f);
                        if (q->bon[b]) {
                            inst_note(i, q->bnote[b], 0, 0.0f);
                            tape_rec(i, q->bnote[b], 0);
                        }
                        inst_note(i, q->bnote[b], vel, 0.0f);
                        tape_rec(i, q->bnote[b], vel);
                        q->bon[b] = 1;
                        q->boff[b] = 0.12f;
                        q->cool[b] = 0.05f;
                    }
                }
                q->bx[b] -= (dist - 0.82f) * nx;
                q->by[b] -= (dist - 0.82f) * ny;
            }
        }
        if (q->bx[b] * q->bx[b] + q->by[b] * q->by[b] > 2.0f) {   /* fell out: gone */
            if (q->bon[b]) {
                inst_note(i, q->bnote[b], 0, 0.0f);
                tape_rec(i, q->bnote[b], 0);
            }
            q->bn--;
            q->bx[b] = q->bx[q->bn];
            q->by[b] = q->by[q->bn];
            q->vx[b] = q->vx[q->bn];
            q->vy[b] = q->vy[q->bn];
            q->bnote[b] = q->bnote[q->bn];
            q->bvel[b] = q->bvel[q->bn];
            q->bon[b] = q->bon[q->bn];
            q->boff[b] = q->boff[q->bn];
            q->cool[b] = q->cool[q->bn];
            b--;
        }
    }
}

static RAMC void seq_run(float dt)
{
    int i;
    float rate = (float)P.tempo * (1.0f / 60.0f) * (float)TPQ * (P.sync ? tape_speed() : 1.0f);
    for (i = 0; i < NINST; i++) {
        const mod_t *m = qmod(i);
        sq_t *q = &SQ[i];
        int t = i >> 1, kind = i & 1, type = m->type % Q_N, active;
        if (!m->on) {
            if (q->nsnd || q->running || q->nlat || q->bn) {
                seq_all_off(i);
                while (q->nlat) {
                    q->nlat--;
                    inst_note(i, q->lat[q->nlat], 0, 0.0f);
                }
                q->bn = 0;
                q->running = 0;
                q->latched = 0;
                q->nheld = 0;
            }
            CORE_VIEW.seq_on[t][kind] = 0;
            continue;
        }
        CORE_VIEW.seq_on[t][kind] = 1;
        if (type == Q_SPIN) {
            spin_run(i, m, q, dt);
            if (t == cur_track && kind == P.t[t].kind) {
                int b;
                CORE_VIEW.nballs = q->bn;
                CORE_VIEW.spin_angle = q->ang;
                for (b = 0; b < q->bn; b++) {
                    CORE_VIEW.balls[b][0] = q->bx[b];
                    CORE_VIEW.balls[b][1] = q->by[b];
                    CORE_VIEW.balls[b][2] = q->bon[b] ? 1.0f : 0.0f;
                }
            }
            continue;
        }
        if (type == Q_LATCH)
            continue;
        active = q->nheld > 0 || q->latched;
        if (type == Q_GRID && kn(m->p[3]) > 0.5f)
            active = 1;                  /* GRID with RUN on: free running */
        if (!active) {
            if (q->running) {
                seq_all_off(i);
                q->running = 0;
            }
            q->count = 0;
            q->step = -1;
            continue;
        }
        if (!q->running) {
            q->running = 1;
            q->clock = 1e9f;
            q->count = 0;
        }
        q->clock += rate * dt;
        if (q->gated && q->clock >= q->gate_at && !q->tie_next) {
            seq_all_off(i);
            q->gated = 0;
        }
        if (q->clock >= q->len) {        /* the next step */
            int notes[8], nn, tie, k, div = type == Q_DRAW ? (int)(kn(m->p[2]) * 6.99f) : type == Q_HANDS ? 4
                                             : (int)(kn(m->p[0]) * 6.99f);
            float base = (float)DIVT[div], sw = (type == Q_HANDS ? kn(m->p[0]) : type == Q_ARP ? kn(m->p[7]) : kn(m->p[1])) * 0.6f,
                  gate = type == Q_ARP ? 0.1f + 0.9f * kn(m->p[4]) : type == Q_GRID ? 0.1f + 0.9f * kn(m->p[7])
                       : type == Q_DRAW ? 0.1f + 0.9f * kn(m->p[7]) : 0.1f + 0.9f * kn(m->p[6]);
            if (type == Q_HANDS)
                gate = 0.6f;
            q->clock = q->clock > q->len * 4.0f ? 0.0f : q->clock - q->len;
            nn = seq_step_notes(i, m, q, notes, &tie);
            if (!tie)
                seq_all_off(i);
            for (k = 0; k < nn; k++)
                seq_emit(i, notes[k], 100);
            q->tie_next = (uint8_t)tie;
            q->len = base * ((q->count & 1u) ? 1.0f - sw : 1.0f + sw);   /* swing: long, short */
            q->gate_at = q->len * gate;
            q->gated = 1;
            q->count++;
        }
        if (t == cur_track)
            CORE_VIEW.seq_step[t][kind] = (int16_t)q->step;
    }
}
