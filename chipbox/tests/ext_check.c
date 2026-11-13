/* SPDX-License-Identifier: GPL-3.0-only */
/* The synthwave extensions, checked on a computer without the panel: notes are put in the sequencer and the
 * channels' settings directly, the sound is rendered and measured.
 *   cc -O2 -DCHIPBOX_HOST -DCHIPBOX_TEST -Dmain=host_main -Ifirmware/src -Ifirmware/gen -o build/host/ext_check tests/ext_check.c && build/host/ext_check
 * Prints a line a check and exits 1 if any fails. */
#include "../firmware/src/chipbox.c"
#undef main
#include <math.h>
#include <time.h>

static int fails;
static void check(int ok, const char *what, double got, double want)
{
    printf("%s  %-44s got %.3f want %.3f\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) fails++;
}
#define MAXF (44100u * 12u)
static int32_t snd[2u * MAXF];
static uint32_t snd_n;
static void render(uint32_t frames)             /* blocks of 256, as the ISR */
{
    static int32_t b[512];
    while (frames && snd_n + 256u <= MAXF) {
        uint32_t i;
        memset(b, 0, sizeof b);
        t_base = snd_n;
        chip_render(b, 256);
        for (i = 0; i < 512u; i++) snd[2u * snd_n + i] = b[i];
        snd_n += 256u; frames = frames > 256u ? frames - 256u : 0u;
    }
}
static void fresh(uint32_t bpm)
{
    seq_stop(); chan_lock = 1; seq_clear(); chan_lock = 0;
    memset(t_on, 0, sizeof t_on); snd_n = 0; seq_bpm = bpm;
    render(512);                                /* (the chips settle) */
    snd_n = 0;
}
static double rms(uint32_t from, uint32_t to, uint32_t ch)
{
    double s = 0; uint32_t i;
    for (i = from; i < to; i++) { double v = snd[2u * i + ch]; s += v * v; }
    return sqrt(s / (to > from ? to - from : 1u));
}
static double hf(uint32_t from, uint32_t to)     /* the energy of the first difference: the highs */
{
    double s = 0; uint32_t i;
    for (i = from + 1u; i < to; i++) { double v = snd[2u * i] - snd[2u * i - 2u]; s += v * v; }
    return sqrt(s / (to > from ? to - from : 1u));
}
static double freq(uint32_t from, uint32_t to)  /* by upward zero crossings (with a little hysteresis) */
{
    uint32_t i, n = 0, first = 0, last = 0; int armed = 0;
    for (i = from; i < to; i++) {
        int32_t v = snd[2u * i] + snd[2u * i + 1u];
        if (v < -200) armed = 1;
        if (armed && v > 200) { armed = 0; if (!n) first = i; last = i; n++; }
    }
    return n > 1u ? 44100.0 * (n - 1u) / (last - first) : 0;
}
static void note_at(uint32_t pg, uint32_t c, uint32_t s, uint32_t note, uint32_t gate) { seq_put(pg, c, s, note, 100, gate); }

int main(void)
{
    int c;
    uint32_t i, s;
    double a, b, f;

    /* 1. the VA synth plays its note at pitch; a chord on it sounds three voices */
    fresh(120);
    c = chan_add(KD_VA); chan[c].x.va[VA_UNI] = 1; chan[c].x.va[VA_SUB] = 0; chan[c].x.va[VA_W1] = 3;      /* one sine */
    ev_do(EV_ON, (uint32_t)c, 69, 120); render(22050);
    f = freq(4410, 22050); check(fabs(f - 440.0) < 3.0, "VA sine A4 pitch", f, 440.0);
    a = rms(4410, 22050, 0); check(a > 1000 && a < 20000, "VA level", a, 5000);
    chan[c].x.va[VA_POLY] = 4; chan[c].x.va[VA_W1] = 0; ev_do(EV_OFF, (uint32_t)c, 0, 0); render(22050);
    ev_do(EV_ON, (uint32_t)c, 60, 120); ev_do(EV_ADD, (uint32_t)c, 64, 120); ev_do(EV_ADD, (uint32_t)c, 67, 120); render(4410);
    for (i = 0, s = 0; i < VA_VOICES; i++) s += vav[c][i].st != ST_IDLE;
    check(s == 3u, "VA chord: voices sounding", s, 3);

    /* 2. the samplers: the bank, SNES and Amiga play their note at pitch */
    fresh(120);
    c = chan_add(KD_SNES); chan[c].x.va[SM_SAMPLE] = 8;             /* the pad: mostly its fundamental */
    check(bank_ready && bank_n > 100u && bank_n <= BANK_BLK, "sample bank made (blocks)", bank_n, 1400);
    ev_do(EV_ON, (uint32_t)c, 57, 120); render(22050);
    f = freq(4410, 22050); check(fabs(f - 220.0) < 3.0, "SNES pad A3 pitch", f, 220.0);
    ev_do(EV_ON, (uint32_t)c, 69 + 24, 120); render(22050);
    f = freq(snd_n - 17640, snd_n); check(fabs(f - 1000.0) < 20.0, "SNES pitch caps two octaves over root", f, 1000.0);
    fresh(120);
    c = chan_add(KD_AMIGA); chan[c].x.va[SM_SAMPLE] = 8;
    ev_do(EV_ON, (uint32_t)c, 57, 120); render(22050);
    f = freq(4410, 22050); check(fabs(f - 220.0) < 4.0, "Amiga pad A3 pitch", f, 220.0);

    /* 3. the arp in time: 1/16 at 120 bpm is 8 notes a second, 1/16T is 12; four octaves of a 1-note chord */
    fresh(120);
    c = chan_add(KD_NES_PU); chan[c].in.arp = 9; chan[c].in.arp_rate = AR_SYNC + 3; chan[c].x.aoct = 3; chan[c].in.arp_dir = 0;
    ev_do(EV_ON, (uint32_t)c, 48, 120);
    { uint32_t k0 = vc[c].arp_k; render(44100); check(vc[c].arp_k - k0 >= 7u && vc[c].arp_k - k0 <= 9u, "arp 1/16 @120: notes a second", vc[c].arp_k - k0, 8); }
    { uint32_t mx = 0; for (i = 0; i < 8u; i++) { uint32_t o = arp_offset_x((uint32_t)c, 9, 0, i); if (o > mx) mx = o; } check(mx == 36u, "arp 4 octaves of NOTE: top offset", mx, 36); }
    chan[c].in.arp_rate = AR_SYNC + 4; ev_do(EV_ON, (uint32_t)c, 48, 120);
    { uint32_t k0 = vc[c].arp_k; render(44100); check(vc[c].arp_k - k0 >= 11u && vc[c].arp_k - k0 <= 13u, "arp 1/16T @120: notes a second", vc[c].arp_k - k0, 12); }
    chan[c].in.arp = 2; chan[c].x.aoct = 1; check(arp_offset_x((uint32_t)c, 2, 0, 3) == 12u && arp_offset_x((uint32_t)c, 2, 0, 5) == 19u, "arp MAJ over 2 octaves", arp_offset_x((uint32_t)c, 2, 0, 5), 19);
    check(arp_offset_x((uint32_t)c, 2, 1, 0) == 19u, "arp DOWN starts at the top", arp_offset_x((uint32_t)c, 2, 1, 0), 19);
    chan[c].x.agate = 4; ev_do(EV_ON, (uint32_t)c, 48, 120); render(44100 / 16);
    check(vc[c].ahold || vc[c].on, "arp gate: still running", 1, 1);

    /* 4. the steps: 1/16T and 12 steps make 24 notes a bar; ratchet 4 makes four; chance 1:2 plays every other loop */
    fresh(120);
    c = chan_add(KD_NES_PU); chan[c].x.speed = 1; chan[c].x.len = 12;
    for (i = 0; i < 12u; i++) note_at(0, (uint32_t)c, i, 60, 2);
    seq_play(0, 1); render(88200 - 300);
    check(t_on[c] == 24u, "1/16T x 12 steps: notes in a bar", t_on[c], 24);
    fresh(120);
    c = chan_add(KD_NES_PU); note_at(0, (uint32_t)c, 0, 60, 1); pat[0][c][0].spare = 3;          /* ratchet 4 */
    seq_play(0, 1); render(88200 - 300);
    check(t_on[c] == 4u, "ratchet 4: notes", t_on[c], 4);
    fresh(120);
    c = chan_add(KD_NES_PU); note_at(0, (uint32_t)c, 0, 60, 1); pat[0][c][0].spare = 6u << 2;      /* the first of two loops */
    seq_play(0, 1); render(88200 * 4u - 300u);
    check(t_on[c] == 2u, "chance 1:2 over four bars", t_on[c], 2);
    fresh(120);
    c = chan_add(KD_NES_PU); note_at(0, (uint32_t)c, 0, 60, 1); note_at(0, (uint32_t)c, 1, 60, 1); pat[0][c][1].spare = 3u << 5;     /* step 2 nudged 3 ticks */
    seq_play(0, 1); render(44100);
    check(t_at[c][1] - t_at[c][0] == 5512u + 2756u, "nudge 3 ticks: frames between", t_at[c][1] - t_at[c][0], 8268);

    /* 5. swing 25: the second sixteenth lands at 75 % of the pair */
    fresh(120); mset.swing = 25;
    c = chan_add(KD_NES_PU); note_at(0, (uint32_t)c, 0, 60, 1); note_at(0, (uint32_t)c, 1, 60, 1); note_at(0, (uint32_t)c, 2, 60, 1);
    seq_play(0, 1); render(44100);
    a = (double)(t_at[c][1] - t_at[c][0]) / (t_at[c][2] - t_at[c][0]);
    check(fabs(a - 0.75) < 0.01, "swing 25: second 16th at", a, 0.75);
    mset.swing = 0;

    /* 6. a chord step on the channels below; a slide holds the note and glides */
    fresh(120);
    c = chan_add(KD_NES_PU); chan_add(KD_NES_PU); chan_add(KD_NES_PU); chan[c].x.chv = 2;
    note_at(0, (uint32_t)c, 0, 60, 5); sx_put(0, (uint32_t)c, 0, 0, 1);            /* MAJ */
    seq_play(0, 1); render(2000);
    check(vc[c + 1].note == 64u && vc[c + 2].note == 67u, "chord step MAJ on the channels below", vc[c + 2].note, 67);
    fresh(120);
    c = chan_add(KD_NES_PU); note_at(0, (uint32_t)c, 0, 60, 5); note_at(0, (uint32_t)c, 1, 67, 5); sx_put(0, (uint32_t)c, 1, 0, SXF_SLIDE);
    seq_play(0, 1); render(5512 + 2000);
    check(t_on[c] == 2u && vc[c].on && vc[c].gl > (60 << 8) && vc[c].gl < (67 << 8), "slide: held, gliding between", vc[c].gl / 256.0, 63.5);

    /* 7. a lock moves the step's cutoff; the filter takes the highs off a saw */
    fresh(120);
    c = chan_add(KD_VA); chan[c].x.va[VA_UNI] = 1;
    ev_do(EV_ON, (uint32_t)c, 45, 120); render(22050); a = hf(11025, 22050);
    chan[c].x.ftype = 1; chan[c].x.fcut = 15; render(22050); b = hf(snd_n - 11025, snd_n);
    check(b < a * 0.3, "LP filter at cut 15: highs left", b / a, 0.1);
    chan[c].x.ftype = 4; render(22050); f = hf(snd_n - 11025, snd_n); check(f < b, "LP24 darker than LP", f / a, 0.05);
    chan[c].x.ftype = 1; chan[c].x.lkt = LK_CUT; lk_pt[c] = LK_CUT; lk_pv[c] = 127; ev_do(EV_ON, (uint32_t)c, 45, 120); render(22050);
    f = hf(snd_n - 11025, snd_n); check(f > b * 2.0, "lock CUT 127 opens it", f / a, 0.8);

    /* 8. sends: the delay's echo comes a 1/8 later (120 bpm: 11025 frames) */
    fresh(120);
    c = chan_add(KD_NES_PU); chan[c].x.sdly = 15; mset.dtime = 2; mset.dfb = 0;
    ev_do(EV_ON, (uint32_t)c, 60, 127); render(1000); ev_do(EV_OFF, (uint32_t)c, 0, 0); render(30000);
    a = rms(11025 + 200, 11025 + 900, 0); b = rms(5000, 9000, 0);
    check(a > 300 && a > b * 4.0, "delay send: echo after 1/8", a, 1000);
    fresh(120);
    c = chan_add(KD_NES_PU); chan[c].x.srev = 15; mset.rgate = 4;
    ev_do(EV_ON, (uint32_t)c, 60, 127); render(2000); ev_do(EV_OFF, (uint32_t)c, 0, 0); render(44100);
    a = rms(2500, 5000, 0); b = rms(20000, 30000, 0);
    check(a > 100 && b < a * 0.05, "gated reverb: rings, then cut", b / (a + 1), 0.0);
    mset = MSET_DEF;

    /* 9. the project keeps it all: pack, unpack, compare */
    fresh(133);
    c = chan_add(KD_VA); chan[c].x.ftype = 2; chan[c].x.fcut = 33; chan[c].x.va[VA_POLY] = 3; chan[c].x.speed = 3; chan[c].in.arp_rate = AR_SYNC + 5;
    note_at(0, (uint32_t)c, 5, 62, 5); pat[0][c][5].spare = 0x55; sx_put(0, (uint32_t)c, 5, 99, SXF_LOCK | 3); mset.swing = 12; mset.rsize = 3;
    {   static uint8_t buf[PROJ_BANK]; uint32_t n = proj_pack(buf), ok;
        ext_t keep = chan[c].x;
        proj_unpack(buf, n);
        ok = n && chan_n == 1u && !memcmp(&chan[0].x, &keep, sizeof keep) && pat[0][0][5].spare == 0x55 && sx_lock(0, 0, 5) == 99u && sx_fl(0, 0, 5) == (SXF_LOCK | 3u) && mset.swing == 12u && mset.rsize == 3u && seq_bpm == 133u && chan[0].in.arp_rate == AR_SYNC + 5;
        check(ok, "project v6 round trip", ok, 1); }

    /* 10. ensemble and drive: the sound stays bounded */
    fresh(120);
    c = chan_add(KD_GB_PU); chan[c].x.uni = 4; chan[c].x.udet = 15; chan[c].x.uspr = 15; chan[c].x.drive = 15; chan[c].x.ftype = 1; chan[c].x.fres = 15; chan[c].x.fcut = 40;
    ev_do(EV_ON, (uint32_t)c, 60, 127); render(44100);
    { int32_t mx = 0; for (i = 0; i < 2u * snd_n; i++) { int32_t v = snd[i] < 0 ? -snd[i] : snd[i]; if (v > mx) mx = v; } check(mx > 1000 && mx <= 32767, "ensemble + drive + resonance: peak", mx, 20000); }
    a = rms(22050, 44100, 0); b = rms(22050, 44100, 1); check(a > 100 && b > 100, "ensemble: both sides", b / a, 1.0);

    /* 11. every factory patch plays a chord: not silent, not clipped */
    for (i = 0; i < PRESETS; i++) {
        int32_t mx = 0; uint32_t j; char what[64];
        fresh(120);
        c = chan_add(KD_NES_PU); chan_add(KD_NES_PU); chan_add(KD_NES_PU); preset_apply(i, (uint32_t)c);
        chan[c].x.chv = 2;
        note_at(0, (uint32_t)c, 0, 57, 40); sx_put(0, (uint32_t)c, 0, 0, 2);       /* A minor */
        seq_play(0, 1); render(44100);
        for (j = 0; j < 2u * snd_n; j++) { int32_t v = snd[j] < 0 ? -snd[j] : snd[j]; if (v > mx) mx = v; }
        snprintf(what, sizeof what, "preset %-14s peak", PRESET_NAME[i]);
        check(mx > 1500 && mx < 32000, what, mx, 12000);
    }

    /* 12. the cost: a heavy synthwave setup against the demo's four chips (host time, a rough guide only) */
    {   clock_t t0; double base, heavy; uint32_t r;
        fresh(120); seq_tetris(); seq_play(0, 0);
        t0 = clock(); for (r = 0; r < 20u; r++) { snd_n = 0; render(44100 / 2); } base = (double)(clock() - t0);
        fresh(120);
        c = chan_add(KD_VA); preset_apply(PR_JUNOPAD, (uint32_t)c);
        ev_do(EV_ON, (uint32_t)c, 57, 100); ev_do(EV_ADD, (uint32_t)c, 60, 100); ev_do(EV_ADD, (uint32_t)c, 64, 100); ev_do(EV_ADD, (uint32_t)c, 67, 100);
        c = chan_add(KD_VA); preset_apply(PR_SAWLEAD, (uint32_t)c); ev_do(EV_ON, (uint32_t)c, 69, 100);
        c = chan_add(KD_VA); preset_apply(PR_BASSPL, (uint32_t)c); ev_do(EV_ON, (uint32_t)c, 33, 100);
        c = chan_add(KD_SNES); preset_apply(PR_SNSTR, (uint32_t)c); ev_do(EV_ON, (uint32_t)c, 57, 100); ev_do(EV_ADD, (uint32_t)c, 64, 100);
        mset.comp = 6; mset.sat = 3;
        t0 = clock(); for (r = 0; r < 20u; r++) { snd_n = 0; render(44100 / 2); } heavy = (double)(clock() - t0);
        printf("info  cost: heavy synthwave setup / demo song = %.1fx (host; the FM-1 shows its own CPU %% as MIDI CC 100)\n", heavy / (base > 0 ? base : 1));
    }

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
