/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the factory presets (included by app.c). Each is built on the INIT patch by the helpers below, in
 * the units a person thinks in (seconds, hertz, semitones, percent), so they read as a recipe. */

#define NFACT 24
static const char *const FACTORY_NAME[NFACT] = {
    "INIT", "SUPERSAW", "WARP BASS", "VOWEL PAD", "FM BELL", "PLUCK", "WOBBLE", "ORGAN", "GLASS KEYS", "GROWL",
    "SHEPARD", "PWM STRINGS", "SUB BASS", "STEP SEQ", "NOISE SWEEP", "LORENZ PAD", "CHIP BLIP", "BRASS", "CHOIR",
    "METAL HIT", "DREAM PAD", "ACID", "SCREAMER", "TAPE KEYS"};

static patch_t *fp_;                     /* the patch being built */
static uint16_t u_(float x) { return (uint16_t)(clampf(x, 0.0f, 1.0f) * 1023.0f + 0.5f); }
static void V_(int id, int v) { fp_->v[id] = (uint16_t)v; }
static void K_(int id, float x) { fp_->v[id] = u_(x); }       /* a knob, 0..1 */
static uint16_t secs_(float s) { return u_(fm_powf(s, 0.25f) / 2.378f); }   /* an envelope time */
static uint16_t cut_(float hz) { return u_((69.0f + 12.0f * (fm_logf(hz / 440.0f) * 1.44269504f) - 8.0f) / 128.0f); }
static uint16_t lhz_(float hz) { return u_((fm_logf(hz) * 1.44269504f + 5.0f) / 10.0f); }
static uint16_t semi_(int st) { return u_(0.5f + (float)st / 96.0f); }

static void osc_(int k, const char *table, float frame, float level)
{
    V_(P_OSC(k, ON), 1);
    V_(P_OSC(k, TABLE), find_table(table));
    K_(P_OSC(k, FRAME), frame);
    K_(P_OSC(k, LEVEL), level);
}
static void uni_(int k, int voices, float detune, float blend, float stereo)
{
    V_(P_OSC(k, UNI), voices - 1);
    K_(P_OSC(k, DET), detune);
    K_(P_OSC(k, BLEND), blend);
    K_(P_OSC(k, STEREO), stereo);
}
static void env_(int k, float a, float d, float s, float r)
{
    fp_->v[P_ENV(k, ATK)] = secs_(a);
    fp_->v[P_ENV(k, DEC)] = secs_(d);
    K_(P_ENV(k, SUS), s);
    fp_->v[P_ENV(k, REL)] = secs_(r);
}
static void flt_(int k, int model, int style, float hz, float reso)
{
    V_(P_FLT(k, ON), 1);
    V_(P_FLT(k, MODEL), model);
    V_(P_FLT(k, STYLE), style);
    fp_->v[P_FLT(k, CUT)] = cut_(hz);
    K_(P_FLT(k, RESO), reso);
}
static void lfo_(int k, int shape, float hz, int sync, int mode)
{
    V_(P_LFO(k, SHAPE), shape);
    fp_->v[P_LFO(k, RATE)] = lhz_(hz);
    V_(P_LFO(k, SYNC), sync);
    V_(P_LFO(k, MODE), mode);
}
static void mod_(int src, int dst, float amt) { mslot_add(fp_, src, dst, (int)(amt * 1000.0f)); }
static void bip_(int src, int dst, float amt)
{
    int s = mslot_add(fp_, src, dst, (int)(amt * 1000.0f));
    if (s >= 0)
        fp_->m[s].flags |= 1u;
}
static void fx_(int t, float mix_or_amount)   /* an effect on; its mix (the last of its parameters named MIX) */
{
    static const int8_t MIX[FXT_N] = {9, 7, 8, 6, -1, 8, 6, 7, 9};
    V_(P_FX(t), 1);
    if (MIX[t] >= 0)
        K_(P_FX(t) + 1 + MIX[t], mix_or_amount);
}
static void fxp_(int t, int i, float v) { K_(P_FX(t) + 1 + i, v); }

static void factory(patch_t *p, int n)
{
    patch_default(p);
    fp_ = p;
    if (n < 0 || n >= NFACT)
        n = 0;
    copy_text(p->name, FACTORY_NAME[n], NAME_LEN);
    /* every preset starts from a saw through an open analog low pass, Vital's INIT */
    osc_(0, "BASIC", 0.68f, 0.71f);
    flt_(0, FLT_ANALOG, 0, 12000.0f, 0.15f);
    env_(0, 0.004f, 0.6f, 1.0f, 0.12f);
    switch (n) {
    case 1:                              /* SUPERSAW: seven detuned saws an octave apart, wide; chorus and a hall */
        osc_(0, "SUPERSAW", 0.4f, 0.71f);
        uni_(0, 7, 0.32f, 0.8f, 1.0f);
        osc_(1, "BASIC", 0.68f, 0.5f);
        uni_(1, 3, 0.2f, 0.6f, 0.8f);
        fp_->v[P_O2_OCT] = semi_(-12);
        flt_(0, FLT_ANALOG, 1, 7000.0f, 0.1f);
        env_(0, 0.01f, 1.0f, 0.9f, 0.5f);
        mod_(MS_VEL, P_F1_CUT, 0.15f);
        fx_(FXT_CHORUS, 0.35f);
        fx_(FXT_REVERB, 0.25f);
        break;
    case 2:                              /* WARP BASS: a sync sweep on each note, a ladder, a little drive */
        V_(P_V_POLY, 0);
        V_(P_V_LEGATO, 1);
        fp_->v[P_V_GLIDE] = secs_(0.05f);
        osc_(0, "SYNC SAW", 0.0f, 0.75f);
        osc_(1, "BASIC", 0.0f, 0.62f);
        fp_->v[P_O2_OCT] = semi_(-12);
        flt_(0, FLT_LADDER, 1, 700.0f, 0.35f);
        env_(0, 0.002f, 0.4f, 0.85f, 0.08f);
        env_(1, 0.0f, 0.35f, 0.0f, 0.1f);
        mod_(MS_ENV2, P_O1_FRAME, 0.8f);
        mod_(MS_ENV2, P_F1_CUT, 0.3f);
        fx_(FXT_DIST, 0.5f);
        fxp_(FXT_DIST, 1, 0.6f);
        fx_(FXT_COMP, 0.6f);
        break;
    case 3:                              /* VOWEL PAD: vowels walked by a slow LFO, three voices, a hall */
        osc_(0, "VOWELS", 0.0f, 0.7f);
        uni_(0, 3, 0.15f, 0.7f, 0.9f);
        osc_(1, "CHOIR", 0.3f, 0.45f);
        lfo_(0, LS_TRI, 0.12f, 0, LM_TRIGGER);
        mod_(MS_LFO1, P_O1_FRAME, 0.95f);
        lfo_(1, LS_SINE, 0.07f, 0, LM_SYNC);
        mod_(MS_LFO2, P_O2_FRAME, 0.6f);
        flt_(0, FLT_ANALOG, 0, 5000.0f, 0.1f);
        env_(0, 0.6f, 1.5f, 0.85f, 1.8f);
        fx_(FXT_CHORUS, 0.3f);
        fx_(FXT_REVERB, 0.4f);
        break;
    case 4:                              /* FM BELL: a sine FM'd by a silent sine 3 octaves up, the index falling */
        osc_(0, "BASIC", 0.0f, 0.75f);
        V_(P_O1_WARP, WARP_FM_A);
        K_(P_O1_WAMT, 0.25f);
        osc_(1, "BASIC", 0.0f, 0.0f);
        fp_->v[P_O2_OCT] = semi_(19);
        V_(P_O2_DEST, 4);
        V_(P_F1_ON, 0);
        env_(0, 0.002f, 3.0f, 0.0f, 2.5f);
        env_(1, 0.0f, 1.2f, 0.1f, 1.0f);
        mod_(MS_ENV2, P_O1_WAMT, 0.45f);
        mod_(MS_VEL, P_O1_WAMT, 0.2f);
        fx_(FXT_DELAY, 0.2f);
        fx_(FXT_REVERB, 0.35f);
        break;
    case 5:                              /* PLUCK: a bright wave closed fast by envelope 2; ping pong */
        osc_(0, "BRIGHT", 0.6f, 0.72f);
        uni_(0, 2, 0.08f, 0.5f, 0.6f);
        flt_(0, FLT_ANALOG, 0, 300.0f, 0.2f);
        env_(0, 0.001f, 0.5f, 0.0f, 0.35f);
        env_(1, 0.0f, 0.22f, 0.0f, 0.2f);
        mod_(MS_ENV2, P_F1_CUT, 0.55f);
        mod_(MS_VEL, P_F1_CUT, 0.15f);
        fx_(FXT_DELAY, 0.3f);
        fxp_(FXT_DELAY, 0, 0.6f);        /* ping pong */
        fx_(FXT_REVERB, 0.2f);
        break;
    case 6:                              /* WOBBLE: growl and a ladder swung by an eighth-note LFO */
        V_(P_V_POLY, 0);
        osc_(0, "GROWL", 0.3f, 0.75f);
        osc_(1, "BASIC", 1.0f, 0.55f);
        fp_->v[P_O2_OCT] = semi_(-12);
        flt_(0, FLT_LADDER, 1, 400.0f, 0.45f);
        lfo_(0, LS_SINE, 2.0f, 8, LM_SYNC);
        mod_(MS_LFO1, P_F1_CUT, 0.45f);
        mod_(MS_LFO1, P_O1_FRAME, 0.7f);
        fx_(FXT_DIST, 0.6f);
        fxp_(FXT_DIST, 1, 0.7f);
        fx_(FXT_COMP, 0.8f);
        break;
    case 7:                              /* ORGAN: drawbars on macro 1, a fast chorus as the rotor */
        osc_(0, "ORGAN", 0.2f, 0.7f);
        V_(P_F1_ON, 0);
        env_(0, 0.005f, 0.1f, 1.0f, 0.08f);
        K_(P_MC1, 0.2f);
        mod_(MS_MAC1, P_O1_FRAME, 0.8f);
        mod_(MS_WHEEL, P_X_CHO_P1, 0.4f);
        fx_(FXT_CHORUS, 0.5f);
        fxp_(FXT_CHORUS, 1, 0.55f);
        fx_(FXT_REVERB, 0.15f);
        break;
    case 8:                              /* GLASS KEYS: glass and an electric piano, struck harder by the velocity */
        osc_(0, "GLASS", 0.25f, 0.6f);
        osc_(1, "EPIANO", 0.3f, 0.6f);
        env_(0, 0.002f, 2.2f, 0.15f, 1.2f);
        env_(1, 0.0f, 0.8f, 0.0f, 0.5f);
        mod_(MS_ENV2, P_O1_FRAME, 0.5f);
        mod_(MS_VEL, P_O2_FRAME, 0.6f);
        flt_(0, FLT_ANALOG, 0, 9000.0f, 0.1f);
        fx_(FXT_PHASER, 0.25f);
        fx_(FXT_REVERB, 0.35f);
        break;
    case 9:                              /* GROWL: a driven sub through a talking formant filter */
        V_(P_V_POLY, 0);
        osc_(0, "SUB DRIVE", 0.6f, 0.8f);
        osc_(1, "SYNC SAW", 0.3f, 0.5f);
        flt_(0, FLT_FORMANT, 0, 1000.0f, 0.75f);
        lfo_(0, LS_TRI, 2.0f, 5, LM_SYNC);
        mod_(MS_LFO1, P_F1_FX, 0.8f);
        V_(P_R1_STYLE, 0);
        fp_->v[P_R1_RATE] = lhz_(3.0f);
        mod_(MS_RND1, P_F1_FY, 0.6f);
        fx_(FXT_DIST, 0.4f);
        fxp_(FXT_DIST, 0, 0.6f);         /* sine fold */
        break;
    case 10:                             /* SHEPARD: a resonant saw climbing forever (the morph), very wet */
        osc_(0, "RESO SAW", 0.4f, 0.7f);
        uni_(0, 4, 0.15f, 0.7f, 1.0f);
        V_(P_O1_MORPH, SM_SHEPARD);
        K_(P_O1_MAMT, 0.8f);
        lfo_(0, LS_SAWUP, 0.08f, 0, LM_SYNC);
        mod_(MS_LFO1, P_O1_FRAME, 0.6f);
        env_(0, 0.35f, 1.0f, 1.0f, 3.0f);
        fx_(FXT_DELAY, 0.35f);
        fx_(FXT_REVERB, 0.55f);
        break;
    case 11:                             /* PWM STRINGS: pulse width breathing, strings under it */
        osc_(0, "PULSE", 0.2f, 0.62f);
        uni_(0, 4, 0.2f, 0.75f, 1.0f);
        lfo_(0, LS_TRI, 0.6f, 0, LM_TRIGGER);
        mod_(MS_LFO1, P_O1_FRAME, 0.5f);
        osc_(1, "STRINGS", 0.5f, 0.5f);
        uni_(1, 2, 0.1f, 0.6f, 0.8f);
        flt_(0, FLT_ANALOG, 0, 4500.0f, 0.1f);
        env_(0, 0.35f, 1.0f, 0.9f, 0.9f);
        fx_(FXT_CHORUS, 0.35f);
        fx_(FXT_REVERB, 0.35f);
        break;
    case 12:                             /* SUB BASS: a sine, a triangle an octave up, soft clipped */
        V_(P_V_POLY, 0);
        V_(P_V_LEGATO, 1);
        fp_->v[P_V_GLIDE] = secs_(0.08f);
        osc_(0, "BASIC", 0.0f, 0.82f);
        osc_(1, "BASIC", 0.33f, 0.35f);
        fp_->v[P_O2_OCT] = semi_(12);
        V_(P_F1_ON, 0);
        env_(0, 0.004f, 0.3f, 0.9f, 0.15f);
        fx_(FXT_DIST, 0.3f);
        fxp_(FXT_DIST, 1, 0.55f);
        break;
    case 13: {                           /* STEP SEQ: a drawn LFO stepping the pitch through a minor arpeggio */
        static const int8_t ST[16] = {0, 12, 7, 3, 0, 15, 10, 7, 0, 12, 7, 5, 3, 7, 10, 19};
        int i;
        osc_(0, "CZ SAW", 0.5f, 0.72f);
        flt_(0, FLT_ANALOG, 0, 800.0f, 0.3f);
        env_(1, 0.0f, 0.12f, 0.0f, 0.1f);
        lfo_(0, LS_DRAWN, 2.0f, 1, LM_TRIGGER);
        for (i = 0; i < LFO_PTS; i++) {
            fp_->lfo_y[0][i] = (uint8_t)(ST[i] * 255 / 24);
            fp_->lfo_c[0][i] = 0;
        }
        mod_(MS_LFO1, P_O1_OCT, 0.25f);  /* 0..1 -> 0..24 semitones */
        lfo_(1, LS_SAWDN, 8.0f, 11, LM_SYNC);
        mod_(MS_LFO2, P_F1_CUT, 0.35f);
        env_(0, 0.002f, 0.3f, 0.8f, 0.2f);
        fx_(FXT_DELAY, 0.3f);
        fxp_(FXT_DELAY, 2, 0.6f);
        fx_(FXT_REVERB, 0.2f);
        break;
    }
    case 14:                             /* NOISE SWEEP: pink noise through a swept comb */
        V_(P_O1_ON, 0);
        V_(P_S_ON, 1);
        V_(P_S_SAMPLE, 1);
        K_(P_S_LEVEL, 0.7f);
        flt_(0, FLT_COMB, 3, 300.0f, 0.75f);
        lfo_(0, LS_SINE, 0.1f, 0, LM_TRIGGER);
        mod_(MS_LFO1, P_F1_CUT, 0.4f);
        env_(0, 0.8f, 1.0f, 1.0f, 2.0f);
        fx_(FXT_REVERB, 0.45f);
        break;
    case 15:                             /* LORENZ PAD: chaos moving the choir and a random table */
        osc_(0, "CHOIR", 0.2f, 0.8f);
        uni_(0, 3, 0.12f, 0.7f, 0.9f);
        osc_(1, "RANDOM", 0.5f, 0.6f);
        V_(P_R1_STYLE, 3);
        fp_->v[P_R1_RATE] = lhz_(0.5f);
        mod_(MS_RND1, P_O1_FRAME, 0.7f);
        V_(P_R2_STYLE, 0);
        fp_->v[P_R2_RATE] = lhz_(0.3f);
        mod_(MS_RND2, P_O2_FRAME, 0.9f);
        bip_(MS_RND2, P_F1_CUT, 0.15f);
        flt_(0, FLT_ANALOG, 0, 3000.0f, 0.2f);
        env_(0, 0.8f, 2.0f, 0.85f, 2.5f);
        fx_(FXT_CHORUS, 0.3f);
        fx_(FXT_REVERB, 0.5f);
        break;
    case 16:                             /* CHIP BLIP: 8-bit tones, quantised, a dotted-eighth echo */
        osc_(0, "CHIP", 0.5f, 0.92f);
        V_(P_O1_WARP, WARP_QUANTIZE);
        K_(P_O1_WAMT, 0.4f);
        V_(P_F1_ON, 0);
        env_(0, 0.001f, 0.25f, 0.35f, 0.12f);
        env_(1, 0.0f, 0.1f, 0.0f, 0.1f);
        mod_(MS_ENV2, P_O1_FRAME, -0.4f);
        fx_(FXT_DELAY, 0.3f);
        fxp_(FXT_DELAY, 2, 0.54f);       /* 1/8. */
        fx_(FXT_DIST, 0.4f);
        fxp_(FXT_DIST, 0, 0.75f);        /* bit crush */
        break;
    case 17:                             /* BRASS: brighter as it swells, as brass is */
        osc_(0, "BRASS", 0.2f, 0.7f);
        uni_(0, 2, 0.1f, 0.6f, 0.6f);
        flt_(0, FLT_ANALOG, 0, 900.0f, 0.15f);
        env_(0, 0.06f, 0.6f, 0.8f, 0.3f);
        env_(1, 0.12f, 0.5f, 0.5f, 0.3f);
        mod_(MS_ENV2, P_O1_FRAME, 0.7f);
        mod_(MS_ENV2, P_F1_CUT, 0.35f);
        mod_(MS_VEL, P_F1_CUT, 0.15f);
        fx_(FXT_REVERB, 0.25f);
        break;
    case 18:                             /* CHOIR: six voices of a wide choir, a vowel filter, a big room */
        osc_(0, "CHOIR", 0.0f, 0.85f);
        uni_(0, 6, 0.18f, 0.7f, 1.0f);
        flt_(0, FLT_FORMANT, 1, 1000.0f, 0.6f);
        K_(P_F1_FX, 0.3f);
        K_(P_F1_FY, 0.6f);
        lfo_(0, LS_SINE, 0.15f, 0, LM_TRIGGER);
        mod_(MS_LFO1, P_O1_FRAME, 0.7f);
        mod_(MS_MAC1, P_F1_FX, 0.6f);
        env_(0, 0.5f, 1.0f, 0.9f, 1.5f);
        fx_(FXT_REVERB, 0.5f);
        break;
    case 19:                             /* METAL HIT: metal struck, rung through a comb */
        osc_(0, "METAL", 0.6f, 0.9f);
        V_(P_S_ON, 1);
        V_(P_S_SAMPLE, 6);
        K_(P_S_LEVEL, 0.75f);
        flt_(0, FLT_COMB, 0, 600.0f, 0.85f);
        env_(0, 0.001f, 1.2f, 0.0f, 0.8f);
        env_(1, 0.0f, 0.3f, 0.0f, 0.2f);
        mod_(MS_ENV2, P_O1_FRAME, -0.5f);
        fx_(FXT_REVERB, 0.35f);
        break;
    case 20:                             /* DREAM PAD: folds and harmonics drifting, phaser, delay, hall */
        osc_(0, "FOLD", 0.2f, 0.6f);
        uni_(0, 4, 0.12f, 0.7f, 1.0f);
        osc_(1, "HARM SWEEP", 0.3f, 0.5f);
        lfo_(0, LS_SINE, 0.09f, 0, LM_TRIGGER);
        mod_(MS_LFO1, P_O1_FRAME, 0.45f);
        V_(P_R1_STYLE, 2);
        fp_->v[P_R1_RATE] = lhz_(0.25f);
        mod_(MS_RND1, P_O2_FRAME, 0.8f);
        flt_(0, FLT_ANALOG, 0, 5000.0f, 0.15f);
        env_(0, 0.45f, 2.0f, 0.9f, 3.0f);
        fx_(FXT_PHASER, 0.3f);
        fx_(FXT_DELAY, 0.25f);
        fx_(FXT_REVERB, 0.55f);
        break;
    case 21:                             /* ACID: a gliding saw, a screaming ladder, envelope and accent */
        V_(P_V_POLY, 0);
        V_(P_V_LEGATO, 1);
        fp_->v[P_V_GLIDE] = secs_(0.07f);
        osc_(0, "BASIC", 0.68f, 0.75f);
        flt_(0, FLT_LADDER, 1, 350.0f, 0.72f);
        env_(0, 0.002f, 0.4f, 0.8f, 0.1f);
        env_(1, 0.0f, 0.25f, 0.0f, 0.15f);
        mod_(MS_ENV2, P_F1_CUT, 0.5f);
        mod_(MS_VEL, P_F1_CUT, 0.2f);
        fx_(FXT_DIST, 0.45f);
        fxp_(FXT_DIST, 1, 0.65f);
        fx_(FXT_DELAY, 0.15f);
        break;
    case 22:                             /* SCREAMER: a synced lead with vibrato on the wheel, hard clipped */
        V_(P_V_POLY, 0);
        V_(P_V_LEGATO, 1);
        fp_->v[P_V_GLIDE] = secs_(0.1f);
        osc_(0, "SYNC SAW", 0.4f, 0.7f);
        uni_(0, 3, 0.1f, 0.6f, 0.7f);
        lfo_(0, LS_SINE, 5.5f, 0, LM_TRIGGER);
        K_(P_L1_FADE, 0.35f);
        bip_(MS_LFO1, P_O1_TUNE, 0.15f);
        mod_(MS_WHEEL, P_O1_FRAME, 0.5f);
        flt_(0, FLT_ANALOG, 0, 5000.0f, 0.2f);
        fx_(FXT_DIST, 0.5f);
        fxp_(FXT_DIST, 0, 0.3f);         /* hard clip */
        fx_(FXT_DELAY, 0.25f);
        fx_(FXT_REVERB, 0.2f);
        break;
    case 23:                             /* TAPE KEYS: an electric piano on a wobbly tape */
        osc_(0, "EPIANO", 0.4f, 0.72f);
        V_(P_R1_STYLE, 0);
        fp_->v[P_R1_RATE] = lhz_(0.7f);
        bip_(MS_RND1, P_O1_TUNE, 0.12f);
        bip_(MS_RND1, P_V_TUNE, 0.05f);
        flt_(0, FLT_ANALOG, 0, 6000.0f, 0.1f);
        env_(0, 0.002f, 2.5f, 0.3f, 0.6f);
        mod_(MS_VEL, P_O1_FRAME, 0.4f);
        fx_(FXT_COMP, 0.5f);
        fx_(FXT_CHORUS, 0.25f);
        fx_(FXT_EQ, 1.0f);
        fxp_(FXT_EQ, 10, 0.4f);
        break;
    default:
        break;
    }
    {                                    /* the levels, evened out across the presets (measured: the RMS of a phrase) */
        static const uint16_t VOL[NFACT] = {760, 780, 900, 940, 900, 940, 760, 740, 1010, 700, 1023, 1023, 700, 830,
                                            1010, 1023, 1023, 1023, 1023, 1023, 1023, 760, 780, 1023};
        p->v[P_V_VOLUME] = VOL[n];
    }
}
