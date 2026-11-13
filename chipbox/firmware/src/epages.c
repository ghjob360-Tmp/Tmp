/* SPDX-License-Identifier: GPL-3.0-only */
/* epages.c: the chip screen's extra pages, after a channel's own ones: four knobs each, on the channel's extras (ext_t).
 *   ARP+    octaves, gate, the USER chord's two notes          FILTER  type, cutoff, resonance, envelope
 *   SHAPE   the filter envelope's fall, key tracking, drive, glide
 *   LFO     target, rate, depth, shape                          WIDE    ensemble copies, detune, spread
 *   SENDS   chorus, delay, reverb                               STEPS   speed (triplets), length, what a lock moves, chord voices
 * The VA, SNES and Amiga have their own pages first (OSC, MIX, VOICE / SAMPLE). Included by chipbox.c before knob_set;
 * chipview.c draws them (ep_panel, ep_cards). */
#define XO(f) ((uint8_t)__builtin_offsetof(ext_t, f))
#define VO(i) ((uint8_t)(__builtin_offsetof(ext_t, va) + (i)))
enum { EW_X, EW_ARP, EW_RATE, EW_DIR };          /* where a field is: an ext_t byte, or the inst_t's arp fields */
#define EF_SIGNED 1u                             /* an int8_t */
#define EF_ZMAX 2u                               /* 0 stands for the top (a channel's 16 steps) */
#define EF_PM 4u                                 /* shown less hi / 2 (semitones either way) */
typedef struct { const char *lab; uint8_t where, off; int8_t lo; uint8_t hi, fl; const char *const *nm; } efd_t;
typedef struct { const char *name, *hint; efd_t f[4]; } epg_t;
#define EFX(l, o, lo, hi, fl, nm) {l, EW_X, o, lo, hi, fl, nm}
#define ENONE {"", EW_X, 0, 0, 0, 0, 0}
static const char *const NM_OCT[4] = {"1", "2", "3", "4"}, *const NM_GATE[8] = {"LEG", "1/8", "2/8", "3/8", "4/8", "5/8", "6/8", "7/8"};
static const char *const NM_KT[4] = {"0", "33%", "66%", "100%"}, *const NM_UNI[5] = {"OFF", "OFF", "2", "3", "4"}, *const NM_CHV[4] = {"OFF", "+1", "+2", "+3"};
static const char *const NM_W1[4] = {"SAW", "PULSE", "TRI", "SINE"}, *const NM_W2[4] = {"SAW", "PULSE", "TRI", "OFF"}, *const NM_OO[2] = {"OFF", "ON"};
enum { EPG_ARP2, EPG_FILT, EPG_SHAPE, EPG_LFO, EPG_WIDE, EPG_SEND, EPG_STEP, EPG_VAOSC, EPG_VAMIX, EPG_VAVOI, EPG_SNES, EPG_AMI, EPGS };
static const epg_t EPG[EPGS] = {
    {"ARP+", "octaves, gate, own chord", {EFX("OCT", XO(aoct), 0, 3, 0, NM_OCT), EFX("GATE", XO(agate), 0, 7, 0, NM_GATE), EFX("USER2", XO(au2), 0, 24, 0, 0), EFX("USER3", XO(au3), 0, 24, 0, 0)}},
    {"FILTER", "cutoff, resonance, env", {EFX("TYPE", XO(ftype), 0, 4, 0, FT_NAME), EFX("CUT", XO(fcut), 0, 63, 0, 0), EFX("RES", XO(fres), 0, 15, 0, 0), EFX("FENV", XO(fenv), -15, 15, EF_SIGNED, 0)}},
    {"SHAPE", "env fall, key, drive", {EFX("FDEC", XO(fdec), 0, 15, 0, 0), EFX("KEY", XO(ktrk), 0, 3, 0, NM_KT), EFX("DRIVE", XO(drive), 0, 15, 0, 0), EFX("GLIDE", XO(glide), 0, 15, 0, 0)}},
    {"LFO", "in time, or free", {EFX("TARGET", XO(ltgt), 0, LT_N - 1, 0, LT_NAME), EFX("RATE", XO(lrate), 0, 22, 0, LR_NAME), EFX("DEPTH", XO(ldep), 0, 15, 0, 0), EFX("SHAPE", XO(lshape), 0, 4, 0, LS_NAME)}},
    {"WIDE", "drifting copies", {EFX("COPIES", XO(uni), 0, 4, 0, NM_UNI), EFX("DETUNE", XO(udet), 0, 15, 0, 0), EFX("SPREAD", XO(uspr), 0, 15, 0, 0), ENONE}},
    {"SENDS", "to the MIX effects", {EFX("CHORUS", XO(scho), 0, 15, 0, 0), EFX("DELAY", XO(sdly), 0, 15, 0, 0), EFX("REVERB", XO(srev), 0, 15, 0, 0), ENONE}},
    {"STEPS", "triplets, length, locks", {EFX("SPEED", XO(speed), 0, 7, 0, SPD_NAME), EFX("STEPS", XO(len), 1, 16, EF_ZMAX, 0), EFX("LOCK", XO(lkt), 0, LK_N - 1, 0, LK_NAME), EFX("CHORD", XO(chv), 0, 3, 0, NM_CHV)}},
    {"OSC", "two oscillators", {EFX("WAVE1", VO(VA_W1), 0, 3, 0, NM_W1), EFX("WAVE2", VO(VA_W2), 0, 3, 0, NM_W2), EFX("SEMI", VO(VA_SEMI), 0, 24, EF_PM, 0), EFX("FINE", VO(VA_FINE), 0, 15, 0, 0)}},
    {"MIX", "osc2, sub, noise, PW", {EFX("OSC2", VO(VA_MIX), 0, 15, 0, 0), EFX("SUB", VO(VA_SUB), 0, 15, 0, 0), EFX("NOISE", VO(VA_NOISE), 0, 15, 0, 0), EFX("PW", VO(VA_PW), 0, 15, 0, 0)}},
    {"VOICE", "supersaw, chords", {EFX("UNISON", VO(VA_UNI), 1, 7, 0, 0), EFX("DETUNE", VO(VA_DET), 0, 15, 0, 0), EFX("SPREAD", VO(VA_SPR), 0, 15, 0, 0), EFX("POLY", VO(VA_POLY), 1, 4, 0, 0)}},
    {"SAMPLE", "instrument, tuning", {EFX("SAMPLE", VO(SM_SAMPLE), 0, SAMPLES - 1, 0, SMP_NAME), EFX("SEMI", VO(SM_SEMI), 0, 24, EF_PM, 0), EFX("FINE", VO(SM_FINE), 0, 15, 0, 0), EFX("POLY", VO(VA_POLY), 1, 4, 0, 0)}},
    {"SAMPLE", "instrument, LED filter", {EFX("SAMPLE", VO(SM_SAMPLE), 0, SAMPLES - 1, 0, SMP_NAME), EFX("SEMI", VO(SM_SEMI), 0, 24, EF_PM, 0), EFX("LED", VO(SM_LED), 0, 1, 0, NM_OO), EFX("POLY", VO(VA_POLY), 1, 4, 0, 0)}},
};
static uint32_t ep_list_k(uint32_t k, uint8_t *l)      /* a kind's extra pages, in order */
{
    uint32_t n = 0;
    if (IS_DRUM(k)) { l[n++] = EPG_FILT; l[n++] = EPG_SHAPE; l[n++] = EPG_SEND; l[n++] = EPG_STEP; return n; }
    if (k == KD_VA) { l[n++] = EPG_VAOSC; l[n++] = EPG_VAMIX; l[n++] = EPG_VAVOI; }
    else if (k == KD_SNES) l[n++] = EPG_SNES;
    else if (k == KD_AMIGA) l[n++] = EPG_AMI;
    l[n++] = EPG_ARP2; l[n++] = EPG_FILT; l[n++] = EPG_SHAPE; l[n++] = EPG_LFO;
    if (k != KD_VA) l[n++] = EPG_WIDE;
    l[n++] = EPG_SEND; l[n++] = EPG_STEP;
    return n;
}
static uint32_t ep_list(uint32_t c, uint8_t *l) { return ep_list_k(chan[c].kind, l); }
static uint32_t ep_n(uint32_t c) { uint8_t l[16]; return ep_list(c, l); }
static const epg_t *ep_pg(uint32_t c, uint32_t i) { uint8_t l[16]; const uint32_t n = ep_list(c, l); return &EPG[l[i < n ? i : 0]]; }
static int32_t ep_getb(const uint8_t *b, const efd_t *f)
{
    int32_t v = f->fl & EF_SIGNED ? (int32_t)(int8_t)b[f->off] : (int32_t)b[f->off];
    if ((f->fl & EF_ZMAX) && !v) v = f->hi;
    return v;
}
static int32_t ep_get(uint32_t c, const efd_t *f) { return ep_getb((const uint8_t *)&chan[c].x, f); }
static void ep_textb(const uint8_t *b, const efd_t *f, char *s)  /* the value as the card shows it */
{
    const int32_t v = ep_getb(b, f), n = f->fl & EF_PM ? v - (int32_t)f->hi / 2 : v;
    if (f->nm) { const char *p = f->nm[v < 0 ? 0 : v]; uint32_t i = 0; while (p[i] && i < 7u) { s[i] = p[i]; i++; } s[i] = 0; return; }
    {   uint32_t a = (uint32_t)(n < 0 ? -n : n), i = 0;
        char t[6]; uint32_t k = 0;
        if (n < 0) s[i++] = '-'; else if (f->fl & (EF_PM | EF_SIGNED) && n > 0) s[i++] = '+';
        do { t[k++] = (char)('0' + a % 10u); a /= 10u; } while (a && k < 5u);
        while (k) s[i++] = t[--k];
        s[i] = 0; }
}
static void ep_text(uint32_t c, const efd_t *f, char *s) { ep_textb((const uint8_t *)&chan[c].x, f, s); }
static void ep_setb(uint8_t *b, const efd_t *f, int32_t d)
{
    int32_t v = ep_getb(b, f) + d;
    v = v < f->lo ? f->lo : v > (int32_t)f->hi ? (int32_t)f->hi : v;
    b[f->off] = (uint8_t)v;
}
/* the MIX view's pages (mixview.c): the project's master settings */
#define MO(f) ((uint8_t)__builtin_offsetof(mset_t, f))
static const char *const NM_SW[26] = {"50%", "51%", "52%", "53%", "54%", "55%", "56%", "57%", "58%", "59%", "60%", "61%", "62%", "63%", "64%", "65%", "66%", "67%", "68%", "69%", "70%", "71%", "72%", "73%", "74%", "75%"};
static const char *const NM_CM[4] = {"OFF", "I", "II", "I+II"}, *const NM_DM[2] = {"STEREO", "PING"}, *const NM_DS[3] = {"LINEAR", "CURVE", "HOLD"};
static const char *const NM_GT[16] = {"OFF", "25ms", "50ms", "75ms", "100", "125", "150", "175", "200", "225", "250", "275", "300", "325", "350", "375"};
#define MPGS 5u
static const epg_t MPG[MPGS] = {
    {"GROOVE", "swing; the keys' scale", {EFX("SWING", MO(swing), 0, 25, 0, NM_SW), EFX("SCALE", MO(scale), 0, SCALES - 1, 0, SCALE_NAME), EFX("KEY", MO(key), 0, 11, 0, KEY_NAME), ENONE}},
    {"CHORUS", "the Juno's two modes", {EFX("MODE", MO(cmode), 0, 3, 0, NM_CM), EFX("RATE", MO(crate), 0, 15, 0, 0), EFX("DEPTH", MO(cdep), 0, 15, 0, 0), ENONE}},
    {"DELAY", "in time; ECHO pad too", {EFX("TIME", MO(dtime), 0, DLY_N - 1, 0, DLY_NAME), EFX("FEEDBK", MO(dfb), 0, 15, 0, 0), EFX("MODE", MO(dmode), 0, 1, 0, NM_DM), EFX("TONE", MO(dtone), 0, 15, 0, 0)}},
    {"REVERB", "size; gated 80s snare", {EFX("SIZE", MO(rsize), 0, 15, 0, 0), EFX("DAMP", MO(rdamp), 0, 15, 0, 0), EFX("GATE", MO(rgate), 0, 15, 0, NM_GT), ENONE}},
    {"MASTER", "glue, warmth, pump", {EFX("COMP", MO(comp), 0, 15, 0, 0), EFX("SAT", MO(sat), 0, 15, 0, 0), EFX("DUCK", MO(drel), 0, 15, 0, 0), EFX("SHAPE", MO(dshape), 0, 2, 0, NM_DS)}},
};
static void ep_knob(uint32_t c, uint32_t pg, uint32_t k, int32_t d)
{
    const efd_t *f = &ep_pg(c, pg)->f[k];
    if (!f->lab[0]) return;
    ep_setb((uint8_t *)&chan[c].x, f, d);
    live_dirty[c] = 1;
}
/* the arp page's fields, wider than before: 22 chords (USER last), 11 rates (in time from 4), 6 directions */
static uint32_t arp_knob(inst_t *in, uint32_t k, int32_t d)
{
    if (k == 0u) in->arp = (uint8_t)clamp_i((int32_t)in->arp + d, 0, (int32_t)ARPS);
    else if (k == 1u) in->arp_rate = (uint8_t)clamp_i((int32_t)in->arp_rate + d, 0, (int32_t)AR_MAX);
    else if (k == 2u) in->arp_dir = (uint8_t)clamp_i((int32_t)in->arp_dir + d, 0, (int32_t)AD_MAX);
    else return 0;
    return 1;
}
