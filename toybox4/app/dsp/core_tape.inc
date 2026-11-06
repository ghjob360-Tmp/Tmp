/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the event tape (included by core.c). Four tracks of note events on a 128-bar timeline. Played
 * at any speed (0.25x .. 2x: its notes follow the speed in pitch, as a tape's would), backwards, looped between
 * two bar lines; recorded over (always overdubbed); edited by lifting a region into the clipboard and dropping it
 * at the head; and played with: break (silent, the time running on) and chop (the last beat, again and again). */

static struct {
    uint8_t playing, armed, breaking, chopping;
    float head, chop_start, chop_len, prev_beat;
} TP;
#define CLIP_EV 512u
static tev_t clip[CLIP_EV] PLAT_POOL;
static uint32_t clip_n;
static uint8_t clip_all;                 /* lifted from all four tracks: each event keeps its track in rsv */
static uint8_t last_vel[NINST][128];

static void tape_init(void)
{
    TP.head = 0.0f;
    TP.prev_beat = -1.0f;
}

INL float tape_speed(void)               /* 0..1023 -> 0.25x .. 1x (512) .. 2x */
{
    float k = kn(P.speed);
    return k < 0.5f ? 0.25f + 1.5f * k : 1.0f + (k - 0.5f) * 2.0f;
}

INL uint32_t loop_lo(void) { return (uint32_t)P.loop_in * 4u * TPQ; }
INL uint32_t loop_hi(void) { return (uint32_t)P.loop_out * 4u * TPQ; }
INL int looping(void) { return P.loop_on && loop_hi() > loop_lo(); }

static RAMC void tape_release(void)      /* the tape's notes end (a stop, a jump, a wrap) */
{
    int j;
    for (j = 0; j < NSV; j++)
        if (SV[j].active && SV[j].gate && SV[j].rel_from > 0.5f) {   /* rel_from: a note off the tape */
            SV[j].gate = 0;
            SV[j].stage = 4;
        }
    for (j = 0; j < NDV; j++)
        if (DV[j].active)
            DV[j].held = 0;
}

/* the first event of track t with a tick > a (or >= a when incl) */
static RAMC uint32_t tape_find(int t, float a, int incl)
{
    uint32_t lo = 0, hi = tape.n[t] < TAPE_EV ? tape.n[t] : TAPE_EV;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        float tk = (float)tape.ev[t][mid].tick;
        if (incl ? tk < a : tk <= a)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static RAMC void tape_fire_ev(int t, const tev_t *e, int reverse, float transpose)
{
    int i = t * 2 + (e->kind & 1), on = e->vel > 0, note = e->note, vel = e->vel;
    if (reverse)                         /* backwards, a note starts where it ended */
        on = !on;
    if (on && !vel)
        vel = last_vel[i][note & 127] ? last_vel[i][note & 127] : 100;
    if (on)
        last_vel[i][note & 127] = (uint8_t)vel;
    from_tape = 1;
    inst_note(i, note, on ? vel : 0, transpose);
    from_tape = 0;
}

/* fire (a, b] forward, or [b, a) backward */
static RAMC void tape_span(float a, float b, int reverse, float transpose)
{
    int t;
    uint32_t k, n;
    for (t = 0; t < NTRK; t++) {
        n = tape.n[t] < TAPE_EV ? tape.n[t] : TAPE_EV;
        if (!reverse) {
            for (k = tape_find(t, a, 0); k < n && (float)tape.ev[t][k].tick <= b; k++)
                tape_fire_ev(t, &tape.ev[t][k], 0, transpose);
        } else {
            uint32_t s = tape_find(t, b, 1), e2 = tape_find(t, a, 1);   /* [b, a) */
            for (k = e2; k > s; k--)
                tape_fire_ev(t, &tape.ev[t][k - 1u], 1, transpose);
        }
    }
}

static RAMC void tape_play(int on)
{
    if (on && !TP.playing)
        TP.prev_beat = -1.0f;
    if (!on && TP.playing)
        tape_release();
    TP.playing = (uint8_t)on;
}

static RAMC void tape_locate(uint32_t tick)
{
    tape_release();
    TP.head = (float)(tick < TAPE_TICKS ? tick : TAPE_TICKS - 1u);
}

static RAMC void tape_insert(int t, const tev_t *e)
{
    uint32_t n = tape.n[t], i;
    if (n >= TAPE_EV)
        return;
    i = tape_find(t, (float)e->tick, 0);  /* after the events at the same tick */
    {
        uint32_t k;
        for (k = n; k > i; k--)
            tape.ev[t][k] = tape.ev[t][k - 1u];
    }
    tape.ev[t][i] = *e;
    tape.n[t] = (uint16_t)(n + 1u);
}

/* a key on the current track, while recording: onto the tape at the head */
static RAMC void tape_rec(int i, int note, int vel)
{
    tev_t e;
    if (!TP.playing || !TP.armed || (i >> 1) != cur_track || TP.breaking)
        return;
    e.tick = (uint32_t)TP.head;
    if (e.tick >= TAPE_TICKS)
        e.tick = TAPE_TICKS - 1u;
    e.note = (uint8_t)note;
    e.kind = (uint8_t)(i & 1);
    e.rsv = 0;
    e.vel = (uint8_t)vel;
    if (P.reverse) {                     /* recorded backwards: stored the way forwards will want it */
        if (vel > 0) {
            last_vel[i][note & 127] = (uint8_t)vel;
            e.vel = 0;
        } else {
            e.vel = last_vel[i][note & 127] ? last_vel[i][note & 127] : 100;
        }
    }
    if ((i & 1) && !e.vel && !P.reverse)
        return;                          /* drums: only the hits (and the loop slices' ends would be lost: fine) */
    tape_insert(i >> 1, &e);
}

static RAMC void tape_region(uint32_t *lo, uint32_t *hi)
{
    if (looping()) {
        *lo = loop_lo();
        *hi = loop_hi();
    } else {                             /* the bar under the head */
        uint32_t bar = (uint32_t)TP.head / (4u * TPQ);
        *lo = bar * 4u * TPQ;
        *hi = *lo + 4u * TPQ;
    }
}

static RAMC void tape_cmd(int c, int arg)
{
    uint32_t lo, hi, k, n, w;
    int t;
    switch (c) {
    case TC_LIFT:
    case TC_LIFT_ALL:                    /* the region into the clipboard, out of the tape */
        tape_region(&lo, &hi);
        clip_n = 0;
        clip_all = (uint8_t)(c == TC_LIFT_ALL);
        for (t = 0; t < NTRK; t++) {
            if (c == TC_LIFT && t != cur_track)
                continue;
            n = tape.n[t];
            for (k = w = 0; k < n; k++) {
                tev_t e = tape.ev[t][k];
                if (e.tick >= lo && e.tick < hi) {
                    if (clip_n < CLIP_EV) {
                        e.tick -= lo;
                        e.rsv = (uint8_t)t;
                        clip[clip_n++] = e;
                    }
                } else {
                    tape.ev[t][w++] = e;
                }
            }
            tape.n[t] = (uint16_t)w;
        }
        tape_release();
        break;
    case TC_DROP: {                      /* the clipboard at the head (a bar line when the head is near one) */
        uint32_t at = (uint32_t)TP.head;
        for (k = 0; k < clip_n; k++) {
            tev_t e = clip[k];
            e.tick += at;
            if (e.tick >= TAPE_TICKS)
                continue;
            tape_insert(clip_all ? e.rsv & 3 : cur_track, &e);
        }
        break;
    }
    case TC_ERASE_TRACK: tape.n[cur_track] = 0; tape_release(); break;
    case TC_ERASE_ALL:
        for (t = 0; t < NTRK; t++)
            tape.n[t] = 0;
        tape_release();
        break;
    case TC_BREAK:                       /* silent while held; the time runs on */
        TP.breaking = (uint8_t)arg;
        if (arg)
            tape_release();
        break;
    case TC_CHOP:                        /* the beat (or a fraction of it) under the head, over and over */
        if (arg && !TP.chopping) {
            float len = (float)TPQ / (float)(arg > 0 && arg < 9 ? arg : 1);
            TP.chop_len = len;
            TP.chop_start = fm_floorf(TP.head / len) * len;
        }
        TP.chopping = (uint8_t)(arg != 0);
        break;
    default: break;
    }
    CORE_VIEW.clip_n = clip_n;
}

static RAMC void tape_move(float dt)
{
    float sp, dir, a, b, tr;
    CORE_VIEW.playing = TP.playing;
    CORE_VIEW.recording = (uint8_t)(TP.playing && TP.armed);
    CORE_VIEW.looping = (uint8_t)looping();
    CORE_VIEW.breaking = TP.breaking;
    CORE_VIEW.tick = (uint32_t)TP.head;
    if (!TP.playing)
        return;
    sp = tape_speed();
    dir = P.reverse ? -1.0f : 1.0f;
    tr = 12.0f * fm_log2f(sp);           /* the tape's notes follow its speed */
    a = TP.head;
    b = a + (float)P.tempo * (1.0f / 60.0f) * (float)TPQ * sp * dir * dt;
    CORE_VIEW.reel += sp * dir * dt * 0.7f;
    if (TP.chopping && fm_fabsf(b - TP.chop_start) >= TP.chop_len) {   /* chop: back to the slice's start */
        tape_release();
        a = b = TP.chop_start + (dir > 0.0f ? 0.0f : TP.chop_len - 0.01f);
        b += 0.001f * dir;
        a -= 0.5f * dir;
    }
    if (dir > 0.0f) {
        if (looping() && b >= (float)loop_hi()) {
            if (!TP.breaking)
                tape_span(a, (float)loop_hi() - 0.001f, 0, tr);
            tape_release();
            b = (float)loop_lo() + (b - (float)loop_hi());
            if (!TP.breaking)
                tape_span((float)loop_lo() - 0.5f, b, 0, tr);
        } else if (b >= (float)TAPE_TICKS) {
            if (!TP.breaking)
                tape_span(a, (float)TAPE_TICKS, 0, tr);
            b = (float)TAPE_TICKS - 1.0f;
            tape_play(0);
        } else if (!TP.breaking) {
            tape_span(a, b, 0, tr);
        }
    } else {
        float lo = looping() ? (float)loop_lo() : 0.0f, hi = looping() ? (float)loop_hi() : (float)TAPE_TICKS;
        if (b < lo) {
            if (!TP.breaking)
                tape_span(a, lo, 1, tr);
            tape_release();
            if (looping()) {
                b = hi - (lo - b);
                if (!TP.breaking)
                    tape_span(hi, b, 1, tr);
            } else {
                b = 0.0f;
                tape_play(0);
            }
        } else if (!TP.breaking) {
            tape_span(a, b, 1, tr);
        }
    }
    TP.head = b;
    {
        float beat = fm_floorf(b / (float)TPQ);
        if (beat != TP.prev_beat) {
            TP.prev_beat = beat;
            CORE_VIEW.beat++;
        }
    }
    CORE_VIEW.tick = (uint32_t)TP.head;
}
