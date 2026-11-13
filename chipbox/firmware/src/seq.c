/* SPDX-License-Identifier: GPL-3.0-only */
/* The sequencer, included by chips.c: a page is 16 steps of 1/16 note for every channel, played by a clock in the audio ISR.
 *   a song is a list of slots, each one a page (1 1 1 1 2 3 1 1 ...); PLAY goes down the list and round again, a held PLAY loops one page
 *   a track is a channel (the same number); a step holds a note, its velocity and its length in clock ticks
 *   the clock ticks 24 times a beat (6 a step) and each tick lands on its own sample, not on the 60 Hz tick
 *   an arp is a channel's setting (chips.c), not a step's
 * The main loop edits the arrays and calls seq_play / seq_stop; one writer on each side, plain bytes. */
#define SEQ_LEN 16u                             /* steps in a page */
#define SEQ_PAGES 64u
#define SEQ_PPS 6u                              /* clock ticks per step */

typedef struct { uint8_t note, vel, gate, spare; } step_t;      /* note 0: nothing; gate in ticks */
#ifndef POOL
#define POOL __attribute__((section(".pool")))        /* big arrays live in the pool (a native test build defines POOL as nothing) */
#endif
static step_t pat[SEQ_PAGES][MAXCH][SEQ_LEN] POOL;
static uint32_t page_n = 1;                     /* pages in use */
static volatile uint32_t seq_bpm = 120;
#define SONG_MAX 128
static uint8_t song[SONG_MAX];                  /* the page of each slot */
static uint32_t song_len = 1;
static volatile int32_t fx_speed = 256;        /* the tape pad (fx.c): 256 normal, 512 double, 128 half, 0 stopped; pitch and level follow */
static volatile uint8_t fx_rpt;                 /* beat repeat (a punch-in effect): ticks between repeats, 0 off */
static uint8_t rpt_step, rpt_page, rpt_cnt;     /* ... of this step, on this page */
static volatile int8_t seq_rate;                /* the tape dial: -3..3, 0 pause; 1 half speed, 2 normal, 3 double; negative runs backwards */
static volatile uint8_t seq_hold, seq_seeked;   /* seq_hold: paused (the clock stands still); seq_seeked: the playhead was moved, PLAY goes on from there */
static volatile uint32_t seq_ticks;             /* clock ticks since play: a recorded note's length */
static volatile uint8_t seq_run, seq_pat, seq_next = 255, seq_step, seq_sub, seq_slot, seq_loop, seq_follow;     /* seq_loop: one page round and round; seq_follow: the rack shows the page that plays */
static uint8_t seq_view;                        /* the page the screen shows and the keys write; the player follows it at the end of the loop */
static uint32_t seq_left, seq_frac;             /* samples to the next tick, and the fraction a tick keeps */
/* MIDI clock (Menu > MIDI Clock; the settings byte `unlocked`): 0 off, 1 send, 2 follow. Our ticks are the MIDI clock's own, 24 a quarter note and 6 a step.
 * Send: each tick in the audio ISR is a clock byte; play and stop send start and stop. Follow: the main loop counts the outside ticks (ext_in) and the audio
 * ISR plays one tick for each, so the tempo is theirs; no tick for 0.4 s and our own clock is back. */
static volatile uint8_t clk_mode, ext_on, ext_reset;
static uint8_t bt_side, bt_patsync, bt_fxsync, bt_key, bt_scale, bt_last = 0xFFu;      /* Bluetooth sync, as a follower: the sidechain; following the leader's pattern (menu > Bluetooth) */
static volatile uint32_t ext_in;
static uint32_t ext_seen, ext_ms, ext_n, ext_t0;
static struct { uint8_t on, gate; } tr[MAXCH];
/* each channel walks its own steps: its speed (ticks a step) and length (ext_t); t counts its ticks from the start of its loop.
 * A channel at 1/16 and 16 steps is always where the page is, as before. A step's hits (ratchet, nudge) are a mask of the ticks they land on. */
static struct { uint16_t t; uint8_t loops, pos, sfl, slk, hit, nhit; uint32_t fmask; step_t st; } cs[MAXCH];
static uint8_t rpt_pos[MAXCH];
static uint32_t ch_spd(uint32_t t) { return STEP_SPD[chan[t].x.speed & 7u]; }
static uint32_t ch_len(uint32_t t) { const uint32_t l = chan[t].x.len; return l && l <= SEQ_LEN ? l : SEQ_LEN; }
static uint32_t ch_pos(uint32_t t) { const uint32_t sp = ch_spd(t), l = ch_len(t); return (cs[t].t / sp) % l; }      /* the step the channel is on (the screens' playhead) */
static uint32_t ch_plain(uint32_t t) { return ch_spd(t) == SEQ_PPS && ch_len(t) == SEQ_LEN; }
/* a step's own extras in its spare byte: ratchet - 1 (bits 0-1), chance (2-4), nudge in ticks (5-7) */
#define ST_RAT(s) (((s)->spare & 3u) + 1u)
#define ST_PROB(s) ((s)->spare >> 2 & 7u)
#define ST_NUDGE(s) ((s)->spare >> 5 & 7u)
static const uint8_t PROB_PC[8] = {100, 87, 75, 62, 50, 25, 0, 0};       /* 6: the first of every two loops, 7: the second */
static const char *const PROB_NAME[8] = {"100%", "87%", "75%", "62%", "50%", "25%", "1:2", "2:2"};

static uint32_t seq_period(void)                /* samples in one tick at the tempo, the remainder carried so the tempo is exact */
{
    uint32_t bpm = seq_bpm < 40u ? 40u : seq_bpm > 240u ? 240u : seq_bpm;
    uint32_t q = ((FS * 5u / 2u) << 8) / bpm;
    if (mset.swing && !(clk_mode == 2u || brothers_mode == 2u)) q = q * (100u + ((seq_sub ? seq_step : seq_step + 1u) & 1u ? -2u * mset.swing : 2u * mset.swing)) / 100u;       /* swing: the even sixteenths longer, the odd ones shorter */
    q += seq_frac;
    seq_frac = q & 255u;
    q >>= 8;
    return seq_rate == 1 || seq_rate == -1 ? q * 2u : seq_rate == 3 || seq_rate == -3 ? (q > 1u ? q / 2u : 1u) : q;     /* half speed, normal, double */
}
/* a drum channel's step holds 11 drums and an accent for each: note = hits 0..7, gate = accents 0..7, vel = hits 8..10 (bits 0..2) and accents 8..10 (bits 3..5) */
static uint32_t dstep_hits(const step_t *s) { return s->note | (uint32_t)(s->vel & 7u) << 8; }
static uint32_t dstep_acc(const step_t *s) { return s->gate | (uint32_t)(s->vel >> 3 & 7u) << 8; }
static void dstep_set(step_t *s, uint32_t hits, uint32_t acc)
{
    s->note = (uint8_t)hits; s->gate = (uint8_t)acc; s->vel = (uint8_t)((hits >> 8 & 7u) | (acc >> 8 & 7u) << 3);
}
static void seq_drums_fire(uint32_t t, const step_t *s)     /* a drum channel's step: every drum in it, at once; an accent is louder */
{
    uint32_t l, h = dstep_hits(s), a = dstep_acc(s);
    if (h & 1u) duck_lvl = 256u;                /* a kick: the sidechain */
    for (l = 0; l < 11u; l++) if (h >> l & 1u) ev_do(EV_ON, t, DR_LANE_TYPE[l], a >> l & 1u ? 127u : 88u);
}
static uint32_t seq_next_slides(uint32_t t)    /* the channel's next step slides in: its note now holds on until then (the 303's way) */
{
    const uint32_t sp = ch_spd(t), l = ch_len(t), np = (cs[t].t / sp + 1u) % l;
    return !IS_DRUM(chan[t].kind) && pat[seq_pat][t][np].note && (sx_fl(seq_pat, t, np) & SXF_SLIDE);
}
static void seq_step_begin(uint32_t t, uint32_t pos, uint32_t sp)       /* a channel reaches a step: does it play (its chance), and on which ticks (ratchet, nudge) */
{
    const step_t *x = &pat[seq_pat][t][pos];
    uint32_t r, nd, i, pr, go;
    cs[t].fmask = 0; cs[t].pos = (uint8_t)pos;
    if (IS_DRUM(chan[t].kind) ? !dstep_hits(x) : !x->note) return;
    pr = ST_PROB(x);
    go = pr == 6u ? !(cs[t].loops & 1u) : pr == 7u ? (cs[t].loops & 1u) : pr == 0u || rnd() % 100u < PROB_PC[pr];
    if (!go) return;
    r = ST_RAT(x); nd = ST_NUDGE(x);
    if (nd >= sp) nd = sp - 1u;
    if (r > sp) r = sp;
    for (i = 0; i < r; i++) { uint32_t at = nd + i * sp / r; if (at >= sp) at = sp - 1u; cs[t].fmask |= 1u << at; }
    cs[t].st = *x; cs[t].hit = 0; cs[t].nhit = (uint8_t)r;
    {   const sx_t *e = sx_get(seq_pat, t, pos);
        cs[t].sfl = e ? e->fl : 0u; cs[t].slk = e ? e->lock : 0u; }
}
static void seq_fire(uint32_t t, uint32_t sp)   /* one hit of the channel's step: the note (a slide, a chord, a lock with it) or the drums */
{
    const step_t *x = &cs[t].st;
    uint32_t note, g, ch, j;
    if (IS_DRUM(chan[t].kind)) { seq_drums_fire(t, x); cs[t].hit++; return; }
    note = bt_scale ? brothers_snap(x->note, bt_scale) : x->note;
    g = x->gate;
    if (cs[t].nhit > 1u) { const uint32_t gm = sp / cs[t].nhit * 3u / 4u; g = g < gm ? g : gm ? gm : 1u; }
    if (cs[t].sfl & SXF_LOCK) { lk_pt[t] = chan[t].x.lkt; lk_pv[t] = cs[t].slk; }
    tr[t].on = 1; tr[t].gate = (uint8_t)(g ? g : 1u);
    if (seq_next_slides(t) && cs[t].hit + 1u >= cs[t].nhit && x->gate + 1u >= sp) tr[t].gate = 0;       /* held into the slide */
    ev_do(cs[t].hit == 0u && (cs[t].sfl & SXF_SLIDE) ? EV_SLIDE : EV_ON, t, note, x->vel & 127u);
    ch = cs[t].sfl & SXF_CHORD;
    if (ch) {                                   /* a chord: on this channel if it plays chords, else on the channels below it */
        const uint32_t sh = STEP_CHORD[ch], n = ARP_N[sh - 1u];
        for (j = 1; j < n; j++) {
            const uint32_t cn = note + (uint32_t)ARP_IV[sh - 1u][j];
            if (cn > 127u) break;
            if (is_va(chan[t].kind) && chan[t].x.va[11] > 1u) ev_do(EV_ADD, t, cn, x->vel & 127u);
            else if (j <= chan[t].x.chv && t + j < chan_n && !IS_DRUM(chan[t + j].kind)) { tr[t + j].on = 1; tr[t + j].gate = tr[t].gate; ev_do(EV_ON, t + j, cn, x->vel & 127u); }
        }
    }
    cs[t].hit++;
}
static void seq_clock(void)                     /* one tick, in the audio ISR */
{
    uint32_t t;
    seq_ticks++;
    for (t = 0; t < chan_n; t++)                /* what is already sounding: its length */
        if (tr[t].on && tr[t].gate && !--tr[t].gate) { if (seq_next_slides(t)) tr[t].gate = 0; else { ev_do(EV_OFF, t, 0, 0); tr[t].on = 0; } }
    arp_pulse();                                /* (before the new notes: a note's first arp note is its own) */
    if (fx_rpt) {                               /* beat repeat: the step that was held sounds again and again */
        if (!rpt_cnt) for (t = 0; t < chan_n; t++) {
            const step_t *x = &pat[rpt_page][t][ch_plain(t) ? rpt_step : rpt_pos[t]];
            if (IS_DRUM(chan[t].kind)) { if (dstep_hits(x)) seq_drums_fire(t, x); continue; }
            if (!x->note) continue;
            tr[t].on = 1; tr[t].gate = x->gate < fx_rpt ? x->gate : (uint8_t)(fx_rpt > 1u ? fx_rpt - 1u : 1u);
            ev_do(EV_ON, t, bt_scale ? brothers_snap(x->note, bt_scale) : x->note, x->vel);
        }
        if (++rpt_cnt >= fx_rpt) rpt_cnt = 0;
    }
    for (t = 0; t < chan_n; t++) {               /* each channel: a new step of its own, then the hits that land on this tick */
        const uint32_t sp = ch_spd(t), l = ch_len(t), sub = cs[t].t % sp;
        if (!fx_rpt && !sub) seq_step_begin(t, (cs[t].t / sp) % l, sp);
        if (!fx_rpt && (cs[t].fmask >> sub & 1u)) seq_fire(t, sp);
        if (++cs[t].t >= sp * l) { cs[t].t = 0; cs[t].loops++; }
    }
    if (++seq_sub >= SEQ_PPS) {
        seq_sub = 0;
        if (++seq_step >= SEQ_LEN) {
            seq_step = 0;
            if (seq_loop == 2u) {                   /* a page played once (the song screen's preview): stop, let the notes ring out */
                seq_run = 0;
                for (t = 0; t < chan_n; t++) { ev_do(EV_OFF, t, 0, 0); tr[t].on = 0; }
            }
            else if (seq_loop) { if (seq_next < page_n) { seq_pat = seq_next; seq_next = 255; } for (t = 0; t < chan_n; t++) if (ch_plain(t)) cs[t].t = 0; }
            else {                                  /* the next slot of the song, and round again at the end: every channel starts the page together */
                if (++seq_slot >= song_len) seq_slot = 0;
                seq_pat = song[seq_slot] < page_n ? song[seq_slot] : 0;
                if (seq_follow) seq_view = seq_pat;
                for (t = 0; t < chan_n; t++) cs[t].t = 0;
            }
        }
    }
}

static void seq_back(void)                      /* one tick backwards: crossing into a step from its end plays it */
{
    uint32_t t;
    seq_ticks++;
    for (t = 0; t < chan_n; t++)
        if (tr[t].on && tr[t].gate && !--tr[t].gate) { ev_do(EV_OFF, t, 0, 0); tr[t].on = 0; }
    arp_pulse();
    if (!fx_rpt)
        for (t = 0; t < chan_n; t++) {          /* each channel steps back on its own; it plays a step as it enters it from the end */
            const uint32_t sp = ch_spd(t), l = ch_len(t);
            cs[t].t = (uint16_t)(cs[t].t ? cs[t].t - 1u : sp * l - 1u);
            if (cs[t].t % sp == sp - 1u) {
                const uint32_t pos = (cs[t].t / sp) % l;
                const step_t *x = &pat[seq_pat][t][pos];
                if (IS_DRUM(chan[t].kind)) { if (dstep_hits(x)) seq_drums_fire(t, x); }
                else if (x->note) { tr[t].on = 1; tr[t].gate = x->gate; ev_do(EV_ON, t, x->note, x->vel & 127u); }
            }
        }
    if (seq_sub) { seq_sub--; return; }
    seq_sub = SEQ_PPS - 1u;
    if (seq_step) seq_step--;
    else {
        seq_step = SEQ_LEN - 1u;
        if (!seq_loop) {                        /* the song: the slot before */
            seq_slot = seq_slot ? seq_slot - 1u : (uint8_t)(song_len - 1u);
            seq_pat = song[seq_slot] < page_n ? song[seq_slot] : 0;
            if (seq_follow) seq_view = seq_pat;
        }
    }
}
static uint32_t seq_off;                         /* the sample of this render the tick is on (chips.c sets it): the brothers' leader timestamps by it */
static void seq_tick(void) { if (clk_mode == 1u && seq_rate > 0) midi_isr_send(0xF8u, 0, 0); if (brothers_mode == 1u && seq_rate > 0) brothers_lead_tick(seq_off, 60000000u / (seq_bpm < 40u ? 40u : seq_bpm > 240u ? 240u : seq_bpm) * (seq_rate == 1 ? 2u : 1u) / (seq_rate == 3 ? 2u : 1u)); if (seq_rate < 0) seq_back(); else seq_clock(); }

/* ---- the main loop's side */
static void seq_play(uint32_t slot, uint32_t loop)      /* the song from a slot; loop 1: the page on show round and round; loop 2: that page once */
{
    uint32_t t;
    memset(tr, 0, sizeof tr);
    if (!(seq_seeked && loop == 1u)) seq_step = seq_sub = 0;       /* a page you seeked in goes on from where you left the playhead */
    for (t = 0; t < MAXCH; t++) { cs[t].t = (uint16_t)(t < chan_n && ch_plain(t) ? seq_step * SEQ_PPS + seq_sub : 0u); cs[t].loops = 0; cs[t].fmask = 0; vc[t].lph2 = 0; }
    seq_seeked = 0; seq_left = seq_frac = 0; seq_rate = 2; seq_hold = 0;
    seq_next = 255; seq_loop = (uint8_t)loop;
    if (loop) { seq_pat = seq_view; seq_follow = 0; }
    else {
        seq_slot = (uint8_t)(slot < song_len ? slot : 0u);
        seq_pat = song[seq_slot] < page_n ? song[seq_slot] : 0;
        seq_view = seq_pat; seq_follow = 1;
    }
    ext_reset = 1;                                  /* an outside clock: the next tick is the first step */
    if (clk_mode == 1u) midi_send(0xFAu, 0, 0);
    if (brothers_mode == 1u) brothers_lead_run(1);
    seq_run = 1;
}
static void seq_stop(void)
{
    uint32_t t, was = seq_run;
    seq_run = 0; seq_rate = 0; seq_hold = 0;
    if (was && clk_mode == 1u) midi_send(0xFCu, 0, 0);
    if (was && brothers_mode == 1u) brothers_lead_run(0);
    for (t = 0; t < chan_n; t++) ev_push(EV_OFF, t, 0, 0);
}
static void seq_shuttle(int32_t d)              /* SELECT turned: the tape dial moves, from a stop it starts the tape */
{
    int32_t r = (int32_t)seq_rate + d;
    r = r < -3 ? -3 : r > 3 ? 3 : r;
    seq_seeked = 1;
    if (r == seq_rate) return;
    if (!seq_run) {                             /* from a stop: the page on show, from where the playhead is */
        uint32_t t;
        memset(tr, 0, sizeof tr);
        for (t = 0; t < MAXCH; t++) { cs[t].t = (uint16_t)(t < chan_n && ch_plain(t) ? seq_step * SEQ_PPS + seq_sub : 0u); cs[t].fmask = 0; }
        seq_left = seq_frac = 0; seq_next = 255; seq_loop = 1; seq_pat = seq_view; seq_follow = 0;
        seq_hold = 1; seq_rate = 0;
        if (clk_mode == 1u) midi_send(0xFAu, 0, 0);
        if (brothers_mode == 1u) brothers_lead_run(1);
        seq_run = 1;
    }
    seq_rate = (int8_t)r;
    if (r == 0) { uint32_t t; seq_hold = 1; for (t = 0; t < chan_n; t++) ev_push(EV_OFF, t, 0, 0); }       /* paused: the notes let go */
    else seq_hold = 0;
}
static uint32_t page_empty(uint32_t p)
{
    uint32_t t, s;
    for (t = 0; t < MAXCH; t++)
        for (s = 0; s < SEQ_LEN; s++)
            if (pat[p][t][s].note) return 0;
    return 1;
}
static void seq_page(int32_t d)                 /* flip the page on show through the pages there are */
{
    seq_view = (uint8_t)(d < 0 ? (seq_view ? seq_view - 1u : page_n - 1u) : (seq_view + 1u < page_n ? seq_view + 1u : 0u));
    seq_follow = 0;
    if (seq_run && seq_loop == 1u) seq_next = seq_view;
}
static uint32_t page_used(uint32_t p)
{
    uint32_t i;
    for (i = 0; i < song_len; i++) if (song[i] == p) return 1;
    return 0;
}
static void pages_trim(void)                    /* an empty last page that no slot holds goes */
{
    while (page_n > 1u && !page_used(page_n - 1u) && page_empty(page_n - 1u)) page_n--;
    if (seq_view >= page_n) seq_view = (uint8_t)(page_n - 1u);
}
static void song_insert_blank(uint32_t at)      /* a new slot after slot `at` holding a new empty page; the cursor's new place, or `at` when full */
{
    uint32_t pos = at < song_len ? at + 1u : song_len;
    if (page_n >= SEQ_PAGES || song_len >= SONG_MAX) return;
    memmove(&song[pos + 1u], &song[pos], song_len - pos);
    song[pos] = (uint8_t)page_n++;
    song_len++;
    seq_view = song[pos];
}
static void song_remove(uint32_t at)
{
    if (song_len <= 1u || at >= song_len) return;
    memmove(&song[at], &song[at + 1u], song_len - at - 1u);
    song_len--;
    pages_trim();
}
static void song_cycle(uint32_t at, int32_t d)  /* the slot's page: the next one there is, or the one before */
{
    if (at >= song_len) return;
    song[at] = (uint8_t)(d < 0 ? (song[at] ? song[at] - 1u : page_n - 1u) : (song[at] + 1u < page_n ? song[at] + 1u : 0u));
    seq_view = song[at];
    pages_trim();
}
static int seq_dup_page(uint32_t p)              /* a copy of page p as a new last page: its number, or -1 when full */
{
    if (page_n >= SEQ_PAGES || p >= page_n) return -1;
    memcpy(pat[page_n], pat[p], sizeof pat[p]);
    sx_copy_page(p, page_n);
    return (int)page_n++;
}
static void seq_toggle(uint32_t track, uint32_t step, uint32_t note)    /* a key on a step: set that note, or clear the step if it already has it */
{
    step_t *x = &pat[seq_view][track][step];
    if (x->note == note) { x->note = 0; x->spare = 0; sx_clear_step(seq_view, track, step); return; }
    if (!x->note) { x->vel = 100; x->gate = 5; }
    x->note = (uint8_t)note;
}
static void seq_chan_removed(uint32_t c)        /* a channel is gone: the tracks above it move down */
{
    uint32_t p;
    memset(tr, 0, sizeof tr);
    for (p = 0; p < SEQ_PAGES; p++) {
        memmove(&pat[p][c], &pat[p][c + 1u], (MAXCH - 1u - c) * sizeof pat[p][0]);
        memset(&pat[p][MAXCH - 1u], 0, sizeof pat[p][0]);
    }
    sx_chan_removed(c);
}
static void seq_clear(void)                     /* an empty project: no channels, one page */
{
    chan_n = 0;
    memset(pat, 0, sizeof pat);
    memset(tr, 0, sizeof tr);
    sx_busy = 1; sx_n = 0; sx_busy = 0;
    page_n = 1; seq_view = 0; seq_pat = 0; seq_next = 255; seq_step = seq_sub = 0; seq_slot = 0; seq_loop = 0; seq_follow = 0;
    song_len = 1; memset(song, 0, sizeof song);
    seq_bpm = 120;
}

static void seq_put(uint32_t p, uint32_t t, uint32_t s, uint32_t note, uint32_t vel, uint32_t gate)
{
    step_t *x = &pat[p][t][s];
    x->note = (uint8_t)note; x->vel = (uint8_t)vel; x->gate = (uint8_t)gate; x->spare = 0;
}
static int chan_add(uint32_t kind);
static void fm_set(uint32_t c, uint32_t alg, uint32_t ratio, uint32_t index, uint32_t fb, uint32_t decay)      /* an FM channel's operators */
{
    chan[c].in.duty = (uint8_t)alg; chan[c].in.wave = (uint8_t)ratio; chan[c].in.wf = (uint8_t)index; chan[c].in.wc = (uint8_t)fb; chan[c].in.wt = (uint8_t)decay;
    fm_from_classic(chan[c].kind, &chan[c].in, &chan[c].fm);       /* (the old knobs, as an operator patch) */
}

/* THE BLUETOOTH DEMO: a locked project to play with all three brothers (D minor, 124 BPM): page 1 the groove, page 2 the lift. Drum kicks on the first lane, so the sidechain pumps. */
static void seq_bt_demo(void)
{
    static const uint8_t BASS[2][16] = {{38, 0, 0, 38, 0, 0, 41, 0, 38, 0, 0, 43, 0, 0, 41, 0}, {46, 0, 0, 46, 0, 0, 45, 0, 43, 0, 0, 41, 0, 41, 43, 0}};
    static const uint8_t LEAD[2][16] = {{62, 0, 65, 0, 69, 0, 65, 0, 62, 0, 65, 0, 67, 0, 65, 0}, {70, 0, 74, 0, 77, 0, 74, 0, 69, 0, 72, 0, 76, 0, 72, 0}};
    uint32_t p, s;
    seq_clear();
    chan_add(KD_DRUM0); chan_add(KD_NES_TRI); chan_add(KD_NES_PU);
    chan[2].in.duty = 1; chan[2].in.vol = 8;
    seq_bpm = 124; page_n = 2; song_len = 1; song[0] = 0;
    for (p = 0; p < 2u; p++)
        for (s = 0; s < 16u; s++) {
            uint32_t hit = (s % 4u == 0u ? 1u : 0u) | (s == 4u || s == 12u ? 2u : 0u) | (s % 4u == 2u ? 128u : 0u);          /* a drum step: bit 0 kick, 1 snare, 7 closed hat */
            if (hit) seq_put(p, 0, s, hit, 0, s == 0u ? 1u : 0u);
            if (BASS[p][s]) seq_put(p, 1, s, BASS[p][s], 127, 9);
            if (LEAD[p][s]) seq_put(p, 2, s, LEAD[p][s], 100, 9);
        }
}

/* Korobeiniki (the Russian folk song, public domain; the Tetris tune): eight bars, one page each, in A minor. The first project of a new device */
static void seq_tetris(void)
{
    static const uint8_t N[8][6] = {{76, 71, 72, 74, 72, 71}, {69, 69, 72, 76, 74, 72}, {71, 72, 74, 76, 0, 0}, {72, 69, 69, 0, 0, 0},
                                    {74, 77, 81, 79, 77, 0}, {76, 72, 76, 74, 72, 0}, {71, 71, 72, 74, 76, 0}, {72, 69, 69, 0, 0, 0}};
    static const uint8_t L[8][6] = {{4, 2, 2, 4, 2, 2}, {4, 2, 2, 4, 2, 2}, {6, 2, 4, 4, 0, 0}, {4, 4, 8, 0, 0, 0},
                                    {6, 2, 4, 2, 2, 0}, {6, 2, 4, 2, 2, 0}, {4, 2, 2, 4, 4, 0}, {4, 4, 8, 0, 0, 0}};
    static const uint8_t BASS[8] = {40, 45, 40, 45, 50, 48, 40, 45};                      /* the chord root of each bar: E A E A D C E A */
    static const uint8_t STAB[8][3] = {{56, 59, 64}, {57, 60, 64}, {56, 59, 64}, {57, 60, 64}, {57, 62, 65}, {55, 60, 64}, {56, 59, 64}, {57, 60, 64}};
    uint32_t p, i, s, k;
    seq_clear();
    chan_add(KD_NES_PU); chan_add(KD_NES_TRI); chan_add(KD_GB_PU); chan_add(KD_SMS);
    chan[2].in.duty = 1; chan[2].in.vol = 9;                                    /* the stabs: thin pulse */
    chan[3].in.vol = 5;                                                         /* the lead doubled an octave down, quiet */
    seq_bpm = 144; page_n = 8; song_len = 8;
    for (p = 0; p < 8u; p++) {
        song[p] = (uint8_t)p;
        for (i = 0, s = 0; i < 6u && L[p][i]; s += L[p][i], i++) {
            uint32_t g = L[p][i] * 6u - 3u;
            seq_put(p, 0, s, N[p][i], 110, g);
            seq_put(p, 3, s, N[p][i] - 12u, 100, g);
        }
        for (k = 0; k < 8u; k++) seq_put(p, 1, k * 2u, BASS[p] + (k & 1u ? 12u : 0u), 127, 9);       /* bass: root, octave, root, octave */
        for (k = 0; k < 4u; k++) seq_put(p, 2, 2u + k * 4u, STAB[p][1u + (k & 1u)], 100, 4);          /* off-beat chord tones */
    }
}
