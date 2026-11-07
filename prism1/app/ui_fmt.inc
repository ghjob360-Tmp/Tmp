/* SPDX-License-Identifier: GPL-3.0-only */
/* PRISM-1 1.0: the pages, their rows, and how every value reads (included by app.c). */

enum { PG_OSC1, PG_OSC2, PG_OSC3, PG_SMP, PG_F1, PG_F2, PG_E1, PG_E2, PG_E3, PG_E4, PG_E5, PG_E6, PG_L1, PG_L2,
       PG_L3, PG_L4, PG_L5, PG_L6, PG_L7, PG_L8, PG_R1, PG_R2, PG_R3, PG_R4, PG_MAT, PG_MAC, PG_FX, PG_VOICE,
       PG_N };
static const char *const PG_NAME[PG_N] = {"OSC 1", "OSC 2", "OSC 3", "SAMPLE", "FILTER 1", "FILTER 2", "ENV 1",
                                          "ENV 2", "ENV 3", "ENV 4", "ENV 5", "ENV 6", "LFO 1", "LFO 2", "LFO 3",
                                          "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8", "RANDOM 1", "RANDOM 2",
                                          "RANDOM 3", "RANDOM 4", "MATRIX", "MACROS", "EFFECTS", "VOICE"};
INL int pg_osc(int pg) { return pg >= PG_OSC1 && pg <= PG_OSC3; }
INL int pg_flt(int pg) { return pg == PG_F1 || pg == PG_F2; }
INL int pg_env(int pg) { return pg >= PG_E1 && pg <= PG_E6; }
INL int pg_lfo(int pg) { return pg >= PG_L1 && pg <= PG_L8; }
INL int pg_rnd(int pg) { return pg >= PG_R1 && pg <= PG_R4; }
/* the page's own modulation source (picking up), MS_NONE if it is not one */
static int pg_source(int pg)
{
    if (pg_env(pg)) return MS_ENV1 + pg - PG_E1;
    if (pg_lfo(pg)) return MS_LFO1 + pg - PG_L1;
    if (pg_rnd(pg)) return MS_RND1 + pg - PG_R1;
    return MS_NONE;
}

/* the controls that are not parameters */
enum { C_NONE = -1, C_MSRC = -10, C_MDST, C_MAMT, C_MPOW, C_MBIP, C_MBYP, C_DPT = -20, C_DY, C_DC, C_DCOPY,
       C_FXON = -30, C_FXPOS, C_MACRO = -40 };

#define OS (P_O2_ON - P_O1_ON)
#define FS (P_F2_ON - P_F1_ON)
#define ES (P_E2_DELAY - P_E1_DELAY)
#define LST (P_L2_SHAPE - P_L1_SHAPE)
#define RS (P_R2_STYLE - P_R1_STYLE)

static const int16_t ROW_OSC[5][4] = {{P_O1_FRAME, P_O1_WAMT, P_O1_LEVEL, P_O1_PAN},
                                      {P_O1_TABLE, P_O1_WARP, P_O1_MORPH, P_O1_MAMT},
                                      {P_O1_UNI, P_O1_DET, P_O1_BLEND, P_O1_STEREO},
                                      {P_O1_STACK, P_O1_DPOW, P_O1_FSPREAD, P_O1_DEST},
                                      {P_O1_OCT, P_O1_TUNE, P_O1_PHASE, P_O1_RPHASE}};
static const char *const ROWN_OSC[5] = {"WAVE", "TABLE", "UNISON", "STACK", "PITCH"};
static const int16_t ROW_SMP[3][4] = {{P_S_SAMPLE, P_S_LEVEL, P_S_PAN, P_S_DEST},
                                      {P_S_OCT, P_S_TUNE, P_S_KEY, P_S_LOOP},
                                      {P_S_RSTART, P_S_ON, C_NONE, C_NONE}};
static const char *const ROWN_SMP[3] = {"SOURCE", "PITCH", "PLAY"};
static const int16_t ROW_FLT[3][4] = {{P_F1_CUT, P_F1_RESO, P_F1_DRIVE, P_F1_BLEND},
                                      {P_F1_MODEL, P_F1_STYLE, P_F1_KEY, P_F1_MIX},
                                      {P_F1_FX, P_F1_FY, P_F1_TRANS, P_F_ROUTE}};
static const char *const ROWN_FLT[3] = {"FILTER", "MODEL", "FORMANT"};
static const int16_t ROW_ENV[3][4] = {{P_E1_ATK, P_E1_DEC, P_E1_SUS, P_E1_REL},
                                      {P_E1_DELAY, P_E1_HOLD, P_E1_APOW, P_E1_DPOW},
                                      {P_E1_RPOW, C_NONE, C_NONE, C_NONE}};
static const char *const ROWN_ENV[3] = {"ADSR", "SHAPE", "RELEASE"};
static const int16_t ROW_LFO[4][4] = {{P_L1_RATE, P_L1_SYNC, P_L1_MODE, P_L1_AMP},
                                      {P_L1_SHAPE, P_L1_PHASE, P_L1_FADE, P_L1_DELAY},
                                      {P_L1_SMOOTH, C_NONE, C_NONE, C_NONE},
                                      {C_DPT, C_DY, C_DC, C_DCOPY}};
static const char *const ROWN_LFO[4] = {"TIME", "SHAPE", "SMOOTH", "DRAW"};
static const int16_t ROW_RND[2][4] = {{P_R1_STYLE, P_R1_RATE, P_R1_SYNC, P_R1_MODE}, {P_R1_AMP, C_NONE, C_NONE, C_NONE}};
static const char *const ROWN_RND[2] = {"RANDOM", "AMOUNT"};
static const int16_t ROW_MAT[2][4] = {{C_MSRC, C_MDST, C_MAMT, C_MPOW}, {C_MBIP, C_MBYP, C_NONE, C_NONE}};
static const char *const ROWN_MAT[2] = {"SLOT", "MODE"};
static const int16_t ROW_MAC[1][4] = {{P_MC1, P_MC2, P_MC3, P_MC4}};
static const int16_t ROW_VOICE[3][4] = {{P_V_POLY, P_V_LEGATO, P_V_GLIDE, P_V_GSLOPE},
                                        {P_V_GFORCE, P_V_BEND, P_V_VEL, P_V_TEMPO},
                                        {P_V_TUNE, P_V_VOLUME, C_NONE, C_NONE}};
static const char *const ROWN_VOICE[3] = {"VOICES", "PLAY", "MASTER"};

static int ui_fx_sel;                    /* the effect the EFFECTS page shows (FXT_*) */
static int ui_mslot;                     /* the matrix slot the MATRIX page edits */
static int ui_dpt[8];                    /* each LFO's drawn point being edited */

static int page_rows(int pg)
{
    if (pg_osc(pg)) return 5;
    if (pg == PG_SMP) return 3;
    if (pg_flt(pg)) return 3;
    if (pg_env(pg)) return 3;
    if (pg_lfo(pg)) return 4;
    if (pg_rnd(pg)) return 2;
    if (pg == PG_MAT) return 2;
    if (pg == PG_MAC) return 1;
    if (pg == PG_FX) return 1 + (FXT_NP[ui_fx_sel] + 3) / 4;
    return 3;
}
static const char *row_name(int pg, int r)
{
    static const char *const FXR[4] = {"CHAIN", "PARAMS 1", "PARAMS 2", "PARAMS 3"};
    if (pg_osc(pg)) return ROWN_OSC[r];
    if (pg == PG_SMP) return ROWN_SMP[r];
    if (pg_flt(pg)) return ROWN_FLT[r];
    if (pg_env(pg)) return ROWN_ENV[r];
    if (pg_lfo(pg)) return ROWN_LFO[r];
    if (pg_rnd(pg)) return ROWN_RND[r];
    if (pg == PG_MAT) return ROWN_MAT[r];
    if (pg == PG_MAC) return "MACROS";
    if (pg == PG_FX) return FXR[r & 3];
    return ROWN_VOICE[r];
}
/* the control at row r, knob i of page pg: a parameter (P_*) or a C_* */
static int ctl_at(int pg, int r, int i)
{
    int id;
    if (pg_osc(pg)) {
        id = ROW_OSC[r][i];
        return id + (pg - PG_OSC1) * OS;
    }
    if (pg == PG_SMP) return ROW_SMP[r][i];
    if (pg_flt(pg)) {
        id = ROW_FLT[r][i];
        return id == P_F_ROUTE || id < 0 ? id : id + (pg - PG_F1) * FS;
    }
    if (pg_env(pg)) {
        id = ROW_ENV[r][i];
        return id < 0 ? id : id + (pg - PG_E1) * ES;
    }
    if (pg_lfo(pg)) {
        id = ROW_LFO[r][i];
        return id < 0 ? id : id + (pg - PG_L1) * LST;
    }
    if (pg_rnd(pg)) {
        id = ROW_RND[r][i];
        return id < 0 ? id : id + (pg - PG_R1) * RS;
    }
    if (pg == PG_MAT) return ROW_MAT[r][i];
    if (pg == PG_MAC) return ROW_MAC[0][i];
    if (pg == PG_FX) {
        if (r == 0) return i == 0 ? C_FXON : i == 1 ? C_FXPOS : C_NONE;
        id = (r - 1) * 4 + i;
        return id < FXT_NP[ui_fx_sel] ? P_FX(ui_fx_sel) + 1 + id : C_NONE;
    }
    return ROW_VOICE[r][i];
}

/* ---------------------------------------------------------------- the texts --- */
static void pct_text(float v, char *t)
{
    num_text((int32_t)(v * 100.0f + 0.5f), t);
    text_cat(t, "%", 12);
}
static void bip_text(float v, char *t)   /* -100 .. +100 */
{
    int32_t x = (int32_t)((v - 0.5f) * 200.0f + (v >= 0.5f ? 0.5f : -0.5f));
    if (x > 0) {
        copy_text(t, "+", 12);
        num_text(x, t + 1);
    } else {
        num_text(x, t);
    }
}
static void dec2_text(float v, char *b)  /* two decimals */
{
    char n[8];
    int32_t x = (int32_t)(v * 100.0f + 0.5f);
    num_text(x / 100, b);
    text_cat(b, ".", 16);
    if (x % 100 < 10)
        text_cat(b, "0", 16);
    num_text(x % 100, n);
    text_cat(b, n, 16);
}
static void secs_text(float s, char *t)
{
    if (s < 0.001f) {
        copy_text(t, "0 ms", 12);
    } else if (s < 1.0f) {
        num_text((int32_t)(s * 1000.0f + 0.5f), t);
        text_cat(t, " ms", 12);
    } else {
        dec1_text(s, t);
        text_cat(t, " s", 12);
    }
}
static void hz_text(float hz, char *t)
{
    if (hz < 10.0f) {
        dec2_text(hz, t);
        text_cat(t, " Hz", 12);
    } else if (hz < 1000.0f) {
        num_text((int32_t)(hz + 0.5f), t);
        text_cat(t, " Hz", 12);
    } else {
        dec1_text(hz * 0.001f, t);
        text_cat(t, " kHz", 12);
    }
}
static void db_text(float db, char *t)
{
    if (db > 0.05f) {
        copy_text(t, "+", 12);
        dec1_text(db, t + 1);
    } else {
        dec1_text(db, t);
    }
    text_cat(t, " dB", 12);
}
static void semis_text(int32_t s, char *t)
{
    if (s > 0) {
        copy_text(t, "+", 12);
        num_text(s, t + 1);
    } else {
        num_text(s, t);
    }
}

static const char *const ONOFF[2] = {"OFF", "ON"};
static const char *const DEST_N[5] = {"FILTER 1", "FILTER 2", "BOTH", "EFFECTS", "DIRECT"};
static const char *const LS_NAME[LS_N] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "EXP", "BUMP",
                                          "STEPS", "SMOOTH", "DRAWN"};
static const char *const LM_NAME[LM_N] = {"TRIGGER", "SYNC", "ENVELOPE", "SUS ENV", "LOOP POINT", "LOOP HOLD"};
static const char *const RS_NAME[4] = {"PERLIN", "S & H", "SINE", "LORENZ"};
static const char *const RM_NAME[2] = {"FREE", "TRIGGER"};

/* the effects' parameter kinds (fx.h) */
enum { FF_PCT, FF_CH, FF_SYNC, FF_HZ, FF_MS, FF_MSX, FF_DB, FF_NOTE, FF_BIP, FF_SEC, FF_MODEL, FF_STYLE };
typedef struct {
    uint8_t kind, n;
    int16_t lo, hi;
    const char *const *names;
} fxf_t;
static const char *const CHO_V[4] = {"1 PAIR", "2 PAIRS", "3 PAIRS", "4 PAIRS"};
static const char *const CMP_B[4] = {"L+B+H", "LOW+BAND", "BAND+HIGH", "SINGLE"};
static const char *const DLY_S[4] = {"MONO", "STEREO", "PING PONG", "MID PING"};
static const char *const DST_T[6] = {"SOFT CLIP", "HARD CLIP", "LIN FOLD", "SINE FOLD", "BIT CRUSH", "DOWNSAMPLE"};
static const char *const DST_O[3] = {"NO FILTER", "PRE", "POST"};
static const char *const EQ_L[2] = {"SHELF", "HIGH PASS"};
static const char *const EQ_B[2] = {"PEAK", "NOTCH"};
static const char *const EQ_H[2] = {"SHELF", "LOW PASS"};
#define FP {FF_PCT, 0, 0, 0, 0}
static const fxf_t FXF[FXT_N][FX_NP] = {
    /* CHORUS */ {{FF_CH, 4, 0, 0, CHO_V}, {FF_HZ, 0, 0, 0, 0}, {FF_SYNC, 14, 0, 0, 0}, FP, {FF_MS, 0, 1, 20, 0},
                  {FF_MS, 0, 1, 20, 0}, {FF_BIP, 0, 0, 0, 0}, {FF_NOTE, 0, 0, 0, 0}, FP, FP, FP, FP},
    /* COMP */ {{FF_CH, 4, 0, 0, CMP_B}, FP, FP, {FF_DB, 0, -30, 30, 0}, {FF_DB, 0, -30, 30, 0},
                {FF_DB, 0, -30, 30, 0}, FP, FP, FP, FP, FP, FP},
    /* DELAY */ {{FF_CH, 4, 0, 0, DLY_S}, {FF_MSX, 0, 10, 1000, 0}, {FF_SYNC, 14, 0, 0, 0}, {FF_MSX, 0, 10, 1000, 0},
                 {FF_SYNC, 14, 0, 0, 0}, FP, {FF_NOTE, 0, 0, 0, 0}, FP, FP, FP, FP, FP},
    /* DIST */ {{FF_CH, 6, 0, 0, DST_T}, {FF_DB, 0, -30, 30, 0}, {FF_CH, 3, 0, 0, DST_O}, {FF_NOTE, 0, 0, 0, 0}, FP, FP,
                FP, FP, FP, FP, FP, FP},
    /* EQ */ {{FF_CH, 2, 0, 0, EQ_L}, {FF_NOTE, 0, 0, 0, 0}, {FF_DB, 0, -18, 18, 0}, FP, {FF_CH, 2, 0, 0, EQ_B},
              {FF_NOTE, 0, 0, 0, 0}, {FF_DB, 0, -18, 18, 0}, FP, {FF_CH, 2, 0, 0, EQ_H}, {FF_NOTE, 0, 0, 0, 0},
              {FF_DB, 0, -18, 18, 0}, FP},
    /* FILTER */ {{FF_MODEL, FLT_N, 0, 0, 0}, {FF_STYLE, 0, 0, 0, 0}, {FF_NOTE, 0, 0, 0, 0}, FP, FP, FP, FP, FP, FP, FP,
                  FP, FP},
    /* FLANGER */ {{FF_HZ, 0, 0, 0, 0}, {FF_SYNC, 14, 0, 0, 0}, {FF_BIP, 0, 0, 0, 0}, FP, FP, FP, FP, FP, FP, FP, FP,
                   FP},
    /* PHASER */ {{FF_HZ, 0, 0, 0, 0}, {FF_SYNC, 14, 0, 0, 0}, FP, {FF_NOTE, 0, 0, 0, 0}, FP, FP, FP, FP, FP, FP, FP,
                  FP},
    /* REVERB */ {FP, {FF_SEC, 0, 0, 0, 0}, {FF_NOTE, 0, 0, 0, 0}, {FF_NOTE, 0, 0, 0, 0}, {FF_NOTE, 0, 0, 0, 0},
                  {FF_DB, 0, -6, 0, 0}, FP, {FF_HZ, 0, 0, 0, 0}, {FF_MS, 0, 0, 300, 0}, FP, FP, FP}};

static int fx_choice_n(int t, int i)     /* > 0: the parameter is a choice of that many */
{
    const fxf_t *f = &FXF[t][i];
    if (f->kind == FF_CH || f->kind == FF_SYNC || f->kind == FF_MODEL)
        return f->kind == FF_SYNC ? FX_NSYNC : f->n;
    if (f->kind == FF_STYLE)
        return FLT_NSTYLE[sel_of(pt.v[P_FX(t) + 1], FLT_N)];
    return 0;
}
static int sel_of(uint16_t v, int n)     /* a 0..1023 knob as one of n */
{
    int i = (int)((uint32_t)v * (uint32_t)n / 1024u);
    return i >= n ? n - 1 : i;
}
static void fx_text(int t, int i, uint16_t raw, float v, char *s)
{
    const fxf_t *f = &FXF[t][i];
    switch (f->kind) {
    case FF_CH: copy_text(s, f->names[sel_of(raw, f->n)], 12); break;
    case FF_SYNC: copy_text(s, FX_SYNC_NAME[sel_of(raw, FX_NSYNC)], 12); break;
    case FF_MODEL: copy_text(s, FLT_NAME[sel_of(raw, FLT_N)], 12); break;
    case FF_STYLE: {
        int md = sel_of(pt.v[P_FX(t) + 1], FLT_N);
        copy_text(s, FLT_STYLE_NAME[md][sel_of(raw, FLT_NSTYLE[md])], 12);
        break;
    }
    case FF_HZ: hz_text(0.01f * fm_powf(1000.0f, v), s); break;
    case FF_MS: num_text((int32_t)((float)f->lo + (float)(f->hi - f->lo) * v + 0.5f), s); text_cat(s, " ms", 12); break;
    case FF_MSX: secs_text((float)f->lo * 0.001f * fm_powf((float)f->hi / (float)f->lo, v), s); break;
    case FF_DB: db_text((float)f->lo + (float)(f->hi - f->lo) * v, s); break;
    case FF_NOTE: hz_text(note_hz(8.0f + 128.0f * v), s); break;
    case FF_BIP: bip_text(v, s); text_cat(s, "%", 12); break;
    case FF_SEC: secs_text(0.1f * fm_powf(640.0f, v), s); break;
    default: pct_text(v, s); break;
    }
}

/* a parameter's value as the screen writes it; v the raw value (0..1023 or a choice) */
static void param_text(int id, uint16_t raw, char *t)
{
    float v = (float)raw / 1023.0f;
    int f;
    t[0] = 0;
    if (id < 0)
        return;
    if (id >= P_X_CHO_ON) {              /* the effects */
        int k = (id - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON), i = (id - P_X_CHO_ON) % (P_X_CMP_ON - P_X_CHO_ON);
        if (i == 0)
            copy_text(t, ONOFF[raw & 1u], 12);
        else
            fx_text(k, i - 1, raw, v, t);
        return;
    }
    if (id <= P_O3_DEST) {               /* the oscillators */
        f = P_O1_ON + (id - P_O1_ON) % OS;
        switch (f) {
        case P_O1_ON: copy_text(t, ONOFF[raw & 1u], 12); return;
        case P_O1_TABLE: copy_text(t, raw < (uint16_t)WT_NFACT ? WT_NAME[raw] : "?", 12); return;
        case P_O1_FRAME: num_text((int32_t)(v * (float)(WT_FRAMES - 1) + 1.5f), t); return;
        case P_O1_MORPH: copy_text(t, SM_NAME[raw % SM_N], 12); return;
        case P_O1_WARP: copy_text(t, WARP_NAME[raw % WARP_N], 12); return;
        case P_O1_PAN: bip_text(v, t); return;
        case P_O1_OCT: semis_text((int32_t)fm_floorf((v - 0.5f) * 96.0f + 0.5f), t); text_cat(t, " st", 12); return;
        case P_O1_TUNE: bip_text(v, t); text_cat(t, " ct", 12); return;
        case P_O1_UNI: num_text(raw + 1, t); return;
        case P_O1_DPOW: bip_text(v, t); return;
        case P_O1_STACK: copy_text(t, STACK_NAME[raw % WT_STACK_N], 12); return;
        case P_O1_PHASE: num_text((int32_t)(v * 360.0f + 0.5f), t); text_cat(t, " deg", 12); return;
        case P_O1_DEST: copy_text(t, DEST_N[raw % 5u], 12); return;
        default: pct_text(v, t); return;
        }
    }
    if (id <= P_S_DEST) {
        switch (id) {
        case P_S_ON: case P_S_KEY: case P_S_LOOP: case P_S_RSTART: copy_text(t, ONOFF[raw & 1u], 12); return;
        case P_S_SAMPLE: copy_text(t, raw < (uint16_t)SMP_NFACT ? SMP_NAME[raw] : "?", 12); return;
        case P_S_PAN: bip_text(v, t); return;
        case P_S_OCT: semis_text((int32_t)fm_floorf((v - 0.5f) * 96.0f + 0.5f), t); text_cat(t, " st", 12); return;
        case P_S_TUNE: bip_text(v, t); text_cat(t, " ct", 12); return;
        case P_S_DEST: copy_text(t, DEST_N[raw % 5u], 12); return;
        default: pct_text(v, t); return;
        }
    }
    if (id <= P_F_ROUTE) {
        if (id == P_F_ROUTE) {
            copy_text(t, raw ? "SERIAL" : "PARALLEL", 12);
            return;
        }
        f = P_F1_ON + (id - P_F1_ON) % FS;
        switch (f) {
        case P_F1_ON: copy_text(t, ONOFF[raw & 1u], 12); return;
        case P_F1_MODEL: copy_text(t, FLT_NAME[raw % FLT_N], 12); return;
        case P_F1_STYLE: {
            int md = pt.v[id - 1] % FLT_N;
            copy_text(t, raw < (uint16_t)FLT_NSTYLE[md] ? FLT_STYLE_NAME[md][raw] : "?", 12);
            return;
        }
        case P_F1_CUT: hz_text(note_hz(8.0f + 128.0f * v), t); return;
        case P_F1_DRIVE: db_text(v * 20.0f, t); return;
        case P_F1_KEY: bip_text(v, t); text_cat(t, "%", 12); return;
        case P_F1_TRANS: semis_text((int32_t)((v - 0.5f) * 24.0f + (v >= 0.5f ? 0.5f : -0.5f)), t); text_cat(t, " st", 12); return;
        case P_F1_BLEND:
            copy_text(t, v < 0.1f ? "LOW" : v > 0.9f ? "HIGH" : (v > 0.4f && v < 0.6f) ? "BAND" : "", 12);
            if (!t[0])
                pct_text(v, t);
            return;
        default: pct_text(v, t); return;
        }
    }
    if (id <= P_E6_RPOW) {
        f = P_E1_DELAY + (id - P_E1_DELAY) % ES;
        if (f == P_E1_SUS)
            pct_text(v, t);
        else if (f == P_E1_APOW || f == P_E1_DPOW || f == P_E1_RPOW)
            bip_text(v, t);
        else
            secs_text(env_time(v), t);
        return;
    }
    if (id <= P_L8_AMP) {
        f = P_L1_SHAPE + (id - P_L1_SHAPE) % LST;
        switch (f) {
        case P_L1_SHAPE: copy_text(t, LS_NAME[raw % LS_N], 12); return;
        case P_L1_SYNC: copy_text(t, SYNC_NAME[raw % 14u], 12); return;
        case P_L1_MODE: copy_text(t, LM_NAME[raw % LM_N], 12); return;
        case P_L1_RATE: {
            int sy = pt.v[id + 1];
            if (sy) copy_text(t, SYNC_NAME[sy % 14], 12);
            else hz_text(lfo_hz(v, 0, 120.0f), t);
            return;
        }
        case P_L1_PHASE: num_text((int32_t)(v * 360.0f + 0.5f), t); text_cat(t, " deg", 12); return;
        case P_L1_FADE: case P_L1_DELAY: secs_text(env_time(v) * 0.5f, t); return;
        default: pct_text(v, t); return;
        }
    }
    if (id <= P_R4_AMP) {
        f = P_R1_STYLE + (id - P_R1_STYLE) % RS;
        switch (f) {
        case P_R1_STYLE: copy_text(t, RS_NAME[raw & 3u], 12); return;
        case P_R1_SYNC: copy_text(t, SYNC_NAME[raw % 14u], 12); return;
        case P_R1_MODE: copy_text(t, RM_NAME[raw & 1u], 12); return;
        case P_R1_RATE: {
            int sy = pt.v[id + 1];
            if (sy) copy_text(t, SYNC_NAME[sy % 14], 12);
            else hz_text(lfo_hz(v, 0, 120.0f), t);
            return;
        }
        default: pct_text(v, t); return;
        }
    }
    switch (id) {
    case P_V_POLY: num_text(raw + 1, t); return;
    case P_V_LEGATO: case P_V_GFORCE: copy_text(t, ONOFF[raw & 1u], 12); return;
    case P_V_GLIDE: secs_text(env_time(v) * 0.25f, t); return;
    case P_V_GSLOPE: bip_text(v, t); return;
    case P_V_BEND: num_text(raw, t); text_cat(t, " st", 12); return;
    case P_V_TEMPO: num_text((int32_t)(40.0f + 200.0f * v + 0.5f), t); text_cat(t, " BPM", 12); return;
    case P_V_TUNE: bip_text(v, t); text_cat(t, " ct", 12); return;
    case P_V_VOLUME: db_text(20.0f * fm_logf(v * v * 1.4f + 1e-5f) * 0.4342945f, t); return;
    default: pct_text(v, t); return;
    }
}

/* a parameter's short name for the matrix (with its section): "OSC 2 FRAME" */
static void param_long(int id, char *t)
{
    t[0] = 0;
    if (id < 0 || id >= NPARAM) {
        copy_text(t, "-", 20);
        return;
    }
    if (id <= P_O3_DEST) {
        copy_text(t, PG_NAME[(id - P_O1_ON) / OS], 20);
    } else if (id <= P_S_DEST) {
        copy_text(t, "SAMPLE", 20);
    } else if (id < P_F_ROUTE) {
        copy_text(t, (id - P_F1_ON) / FS ? "FILTER 2" : "FILTER 1", 20);
    } else if (id >= P_E1_DELAY && id <= P_E6_RPOW) {
        copy_text(t, PG_NAME[PG_E1 + (id - P_E1_DELAY) / ES], 20);
    } else if (id >= P_L1_SHAPE && id <= P_L8_AMP) {
        copy_text(t, PG_NAME[PG_L1 + (id - P_L1_SHAPE) / LST], 20);
    } else if (id >= P_R1_STYLE && id <= P_R4_AMP) {
        copy_text(t, PG_NAME[PG_R1 + (id - P_R1_STYLE) / RS], 20);
    } else if (id >= P_X_CHO_ON) {
        copy_text(t, FXT_NAME[(id - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON)], 20);
    } else {
        copy_text(t, "VOICE", 20);
    }
    text_cat(t, " ", 20);
    if (id >= P_X_CHO_ON) {
        int k = (id - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON), i = (id - P_X_CHO_ON) % (P_X_CMP_ON - P_X_CHO_ON);
        text_cat(t, i ? FXT_PNAME[k][i - 1] : "ON", 20);
    } else {
        text_cat(t, PDESC[id].name, 20);
    }
}
static const char *param_name(int id)
{
    if (id >= P_X_CHO_ON) {
        int k = (id - P_X_CHO_ON) / (P_X_CMP_ON - P_X_CHO_ON), i = (id - P_X_CHO_ON) % (P_X_CMP_ON - P_X_CHO_ON);
        return i ? FXT_PNAME[k][i - 1] : "ON";
    }
    return id >= 0 && id < NPARAM ? PDESC[id].name : "";
}
/* the modulatable parameters, in order: the matrix's destination list */
static int mod_next(int id, int dir)
{
    int i = id, n;
    for (n = 0; n < NPARAM + 1; n++) {
        i += dir;
        if (i < 0)
            i = NPARAM - 1;
        if (i >= NPARAM)
            i = 0;
        if (PDESC[i].flags & PF_MOD)
            return i;
    }
    return id;
}
/* how far the matrix can move a parameter now: lo .. hi of its 0..1 range (the knobs' purple rings) */
static void mod_reach(int id, float *lo, float *hi)
{
    int i;
    float v, a = 0.0f, b = 0.0f;
    *lo = *hi = 0.0f;
    if (id < 0 || PDESC[id].kind == PK_CHOICE)
        return;
    v = (float)pt.v[id] / 1023.0f;
    for (i = 0; i < NSLOT; i++) {
        const mslot_t *m = &pt.m[i];
        float am;
        if (m->src == MS_NONE || m->dst != (uint16_t)id || (m->flags & 2u))
            continue;
        am = (float)m->amt * 0.001f;
        if ((m->flags & 1u) || m->src == MS_BEND) {
            a -= fm_fabsf(am);
            b += fm_fabsf(am);
        } else if (am > 0.0f) {
            b += am;
        } else {
            a += am;
        }
    }
    *lo = clampf(v + a, 0.0f, 1.0f);
    *hi = clampf(v + b, 0.0f, 1.0f);
}
