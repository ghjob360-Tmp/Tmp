/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the panel's behaviour and its screens (included at the end of app.c). */

static struct {
    int mode, page[4], glo;
    uint32_t keys, btn, save_t, rec_t;
    uint8_t help, playing, armed, path, erase_done;
    char toast[2][24];
    uint32_t toast_until, top_at, bot_at;
    uint8_t top_dirty, bot_dirty;
    int sel_key[NTRK];
    uint32_t trick_down, trick_t[16];
    uint16_t memo[2][14];
    uint8_t memo_set[2];
} ui;

int ui_cur_track(void) { return cur; }
static uint32_t ui_hit_seen[NTRK][NDKEY], ui_hit_at[NTRK][NDKEY];   /* drum hits: counted, when */
extern void eng_tables_init(void);

static void project_changed(void)
{
    dirty = 1;
    ui.top_dirty = ui.bot_dirty = 1;
    core_project(&pj);
}

void app_say(const char *a, const char *b)
{
    copy_text(ui.toast[0], a, sizeof ui.toast[0]);
    copy_text(ui.toast[1], b, sizeof ui.toast[1]);
    ui.toast_until = plat_ms() + 1200u;
    ui.top_dirty = 1;
    if (ann_on()) {
        char t[48];
        copy_text(t, a, sizeof t);
        text_cat(t, " ", sizeof t);
        text_cat(t, b, sizeof t);
        ann_say(t, 0, ANN_NOW);
    }
}

/* ------------------------------------------------------------- the labels --- */
static const char *const ENG_NAME[E_NSYNTH] = {"STRING", "PULSE", "FM", "CLUSTER", "DIGITAL", "PHASE", "SPECTRA",
                                               "DUAL", "VOLTAGE", "DNA", "SAMPLER"};
static const char *const DRUM_NAME[D_N] = {"KIT", "SLICES"};
static const char *const FX_NAME[FX_N] = {"ECHO", "CUBE", "TURBO", "WIRE", "KNOCK", "COIL", "DOPPLER", "HALL", "BITS"};
static const char *const LFO_NAME[L_N] = {"SENSE", "DICE", "WOBBLE", "WAVE", "MIDI"};
static const char *const SEQ_NAME[Q_N] = {"LOOP", "GRID", "SPIN", "HANDS", "DRAW", "ARP", "LATCH"};
static const char *const ENG_L[E_NSYNTH][8] = {
    {"TENSION", "IMPULSE", "DETUNE", "TYPE", "PLUCK", "BRIGHT", "BODY", "DRIVE"},
    {"WIDTH", "PULSE 2", "FILTER", "PWM", "RESO", "SUB", "F.ENV", "DRIVE"},
    {"AMOUNT", "FREQ", "TOPOLOGY", "DETUNE", "FEEDBK", "OP 3/4", "DECAY", "BRIGHT"},
    {"WAVES", "WAVE ENV", "SPREAD", "UNITOR", "SHAPE", "DRIFT", "FILTER", "SUB"},
    {"SHAPER", "OCTAVE", "DETUNE", "DIGITAL", "BITS", "WAVE", "NOISE", "FOLD"},
    {"SHIFT", "AMOUNT", "FILTER", "TILT", "WAVE", "DIST ENV", "SUB", "DETUNE"},
    {"WAVE", "FILTER", "PHASE", "CHORUS", "ODD/EVEN", "HARMONICS", "TILT", "MOTION"},
    {"X-FADE", "WAVE 1", "ENV", "X-MOD", "INTERVAL", "WAVE 2", "DETUNE 2", "CUTOFF"},
    {"AMP MOD", "SHAPER", "FILTER", "DETUNE", "GROUND", "SUB", "DRIFT", "BIAS"},
    {"FILTER", "WAVE #", "MODIFY", "NOISE", "WALK", "STEPS", "RESO", "OCTAVE"},
    {"START", "LOOP IN", "LOOP OUT", "END", "REVERSE", "IN FINE", "OUT FINE", "GAIN"}};
static const char *const DRUM_L[D_N][8] = {{"PITCH", "WAVE", "DECAY", "X-MOD", "PITCH 2", "OSC 2", "FILTER", "LEVEL"},
                                           {"PITCH", "IN", "OUT", "PLAY", "REVERSE", "IN FINE", "OUT FINE", "LEVEL"}};
static const char *const ENVS_L[8] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE", "PLAY", "GLIDE", "BEND", "VOLUME"};
static const char *const ENVD_L[8] = {"ATTACK", "MID", "RELEASE", "TIMING", 0, 0, "BEND", "VOLUME"};
static const char *const FX_L[FX_N][8] = {
    {"TIME", "SPEED", "FEEDBK", "MIX", "TONE", "PING", "FINE", "DUCK"},
    {"X SIZE", "Y SIZE", "Z FEEDBK", "MIX", "DAMP", "MOD", "PRE", "WIDTH"},
    {"FREQ", "FOLLOW", "RESO", "FREQ 2", "DRIVE", "ENV", "MODE", "MIX"},
    {"TONE", "PHONIC", "BAUD", "TELEMATIC", "BITS", "CRACKLE", "WOW", "MIX"},
    {"FREQ", "PUNCH", "ROUNDS", "POWER", "ATTACK", "RELEASE", "KNEE", "MIX"},
    {"TONE", "TURNS", "DAMPING", "MIX", "DRIP", "WIDTH", "PRE", "BOING"},
    {"FREQ", "DELAY", "FEEDBK", "SIDEBAND", "WINDOW", "SPREAD", "TONE", "MIX"},
    {"DISTANCE", "GATE", "COLOR", "MIX", "PRE", "DIFFUSE", "MOD", "WIDTH"},
    {"RATE", "BITS", "MODEL", "MIX", "JITTER", "GAIN", "TONE", "DRIVE"}};
static const char *const LFO_L[L_N][8] = {{"SOURCE", "AMOUNT", "MODULE", "PARAM", "SMOOTH", 0, 0, 0},
                                          {"AMOUNT", "SPEED", "MODULE", "ENV", 0, 0, 0, 0},
                                          {"SPEED", "PITCH", "VOLUME", "FADE", "SHAPE", "SYNC", 0, 0},
                                          {"SPEED", "AMOUNT", "MODULE", "PARAM", "SHAPE", "SYNC", "RETRIG", "PHASE"},
                                          {"WHEEL", "PRESSURE", "MODULE", "PARAM", 0, 0, 0, 0}};
static const char *const SEQ_L[Q_N][8] = {{"DIVISION", "SWING", "TRIGGER", "HOLD", "DIRECTION", "ROTATE", "GATE", 0},
                                          {"CURSOR", "SWING", "LENGTH", "RUN", 0, 0, "REVERSE", "GATE"},
                                          {"SPEED", "GRAVITY", "OPENING", "BOUNCE", 0, 0, 0, 0},
                                          {"SWING", "LENGTH", "DENSITY", "HOLD", 0, 0, 0, 0},
                                          {"X", "DRAW", "SPEED", "HOLD", 0, "SCALE", "RANGE", "GATE"},
                                          {"DIVISION", "TRIGGER", "PATTERN", "HOLD", "LENGTH", "TYPE", "OCTAVES", "SWING"},
                                          {"SPLIT", "MONO", "TRANSPOSE", 0, 0, 0, 0, 0}};
static const char *const PM_N_[4] = {"POLY", "MONO", "LEGATO", "UNISON"};
static const char *const SRC_N_[5] = {"PRESSURE", "VELOCITY", "ENVELOPE", "LEVEL", "WHEEL"};
static const char *const MODL_N[3] = {"ENGINE", "ENVELOPE", "EFFECT"};
static const char *const WSH_N[5] = {"SINE", "SAW", "EXP", "SQUARE", "BLIP"};
static const char *const VSH_N[5] = {"SINE", "TRIANGLE", "SAW", "SQUARE", "RANDOM"};
static const char *const SYNC_N[6] = {"FREE", "1 BAR", "1/2", "1/4", "1/8", "1/16"};
static const char *const DIV_N[7] = {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const DIRN_N[4] = {"FORWARD", "REVERSE", "PING-PONG", "RANDOM"};
static const char *const ARPM_N[4] = {"FREE", "ONCE", "EACH", "EACH"};
static const char *const ARPT_N[4] = {"UP", "UP-DOWN", "+7 +9 -5", "SCALE"};
static const char *const SCALE_N[5] = {"CHROMATIC", "MAJOR", "MINOR", "PENTATONIC", "BLUES"};
static const char *const PLAYM_N[3] = {"TO END", "LOOP", "ONCE"};
static const char *const DWAVE_N[4] = {"SINE", "TRIANGLE", "SAW", "SQUARE"};
static const char *const PWAVE_N[8] = {"SAW", "SQUARE", "PULSE", "DBL SINE", "SAW-PULSE", "RES I", "RES II", "RES III"};
static const char *const SPEC_N[6] = {"SAW", "SQUARE", "ORGAN", "BELL", "VOCAL", "GLASS"};

/* ------------------------------------------------------------- the dials --- */
enum { F_PCT, F_BIP, F_NAMES, F_RANGE, F_ATT, F_DEC, F_REL, F_DREL, F_MS, F_BEND, F_HZ, F_PARAM, F_TRIG, F_NOTE,
       F_SPEED, F_BARS, F_HEAD, F_ONOFF, F_PAN, F_LOOPLEN };
typedef struct {
    const char *label;
    uint16_t *v;                         /* the value it turns (0 for an action) */
    uint8_t fmt, n;                      /* F_NAMES: n names; F_RANGE: lo + k * (n_hi) */
    int16_t lo, hi;
    const char *const *names;
    int act;                             /* an action instead of a value (tape) */
} dl_t;

static sound_t *cur_sound(int kind) { return kind ? &pj.t[cur].drum : &pj.t[cur].synth; }
static int mode_kind(void) { return ui.mode == MD_DRUM ? KIND_DRUM : KIND_SYNTH; }
static seqdata_t *cur_seqdata(int kind) { return kind ? &pj.t[cur].seq_drum : &pj.t[cur].seq_synth; }

static void dl_mod(dl_t *d, mod_t *m, const char *const *labels, int i, int alt)
{
    int j = i + (alt ? 4 : 0);
    d->label = labels[j];
    d->v = labels[j] ? &m->p[j] : 0;
}

static void get_dials(dl_t d[4])
{
    int i, alt = ui.glo, kind = mode_kind();
    for (i = 0; i < 4; i++) {
        d[i].label = 0;
        d[i].v = 0;
        d[i].fmt = F_PCT;
        d[i].n = 0;
        d[i].lo = 0;
        d[i].hi = 1023;
        d[i].names = 0;
        d[i].act = 0;
    }
    if (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) {
        sound_t *s = cur_sound(kind);
        int pg = ui.page[ui.mode];
        for (i = 0; i < 4; i++) {
            int j = i + (alt ? 4 : 0);
            if (pg == PG_T1) {
                if (kind) {
                    d[i].label = DRUM_L[s->m[M_ENGINE].type % D_N][j];
                    d[i].v = &pj.t[cur].pads[ui.sel_key[cur] % NDKEY].p[j];
                    if (s->m[M_ENGINE].type == D_SLICE && j == 3) {
                        d[i].fmt = F_NAMES; d[i].n = 3; d[i].names = PLAYM_N;
                    }
                    if (s->m[M_ENGINE].type == D_SLICE && j == 4)
                        d[i].fmt = F_ONOFF;
                    if (j == 0 && s->m[M_ENGINE].type == D_SLICE)
                        d[i].fmt = F_BIP;           /* a slice's pitch: +-2 octaves round the original */
                } else {
                    int e = s->m[M_ENGINE].type % E_NSYNTH;
                    dl_mod(&d[i], &s->m[M_ENGINE], ENG_L[e], i, alt);
                    if (e == E_FM && j == 2) { d[i].fmt = F_RANGE; d[i].lo = 1; d[i].hi = 8; }
                    if (e == E_CLUSTER && j == 0) { d[i].fmt = F_RANGE; d[i].lo = 1; d[i].hi = 6; }
                    if ((e == E_DIGITAL && j == 1) || (e == E_DNA && j == 7)) { d[i].fmt = F_RANGE; d[i].lo = -2; d[i].hi = 2; }
                    if (e == E_DIGITAL && j == 5) { d[i].fmt = F_NAMES; d[i].n = 4; d[i].names = DWAVE_N; }
                    if (e == E_PHASE && j == 4) { d[i].fmt = F_NAMES; d[i].n = 8; d[i].names = PWAVE_N; }
                    if (e == E_SPECTRA && j == 0) { d[i].fmt = F_NAMES; d[i].n = 6; d[i].names = SPEC_N; }
                    if (e == E_DNA && j == 1) { d[i].fmt = F_RANGE; d[i].lo = 0; d[i].hi = 255; }
                    if (e == E_SAMPLER && j == 4) d[i].fmt = F_ONOFF;
                }
            } else if (pg == PG_T2) {
                dl_mod(&d[i], &s->m[M_ENV], kind ? ENVD_L : ENVS_L, i, alt);
                if (!kind) {
                    static const uint8_t F[8] = {F_ATT, F_DEC, F_PCT, F_REL, F_NAMES, F_PCT, F_BEND, F_PCT};
                    d[i].fmt = F[j];
                    if (j == 4) { d[i].n = 4; d[i].names = PM_N_; }
                } else {
                    static const uint8_t F[8] = {F_PCT, F_PCT, F_DREL, F_MS, F_PCT, F_PCT, F_BEND, F_PCT};
                    d[i].fmt = F[j];
                }
            } else if (pg == PG_T3) {
                dl_mod(&d[i], &s->m[M_FX], FX_L[s->m[M_FX].type % FX_N], i, alt);
            } else if (pg == PG_T4) {
                int lt = s->m[M_LFO].type % L_N;
                dl_mod(&d[i], &s->m[M_LFO], LFO_L[lt], i, alt);
                if ((lt == L_SENSE || lt == L_WAVE || lt == L_MIDI) && j == 2) { d[i].fmt = F_NAMES; d[i].n = 3; d[i].names = MODL_N; }
                if (lt == L_DICE && j == 2) { d[i].fmt = F_NAMES; d[i].n = 3; d[i].names = MODL_N; }
                if ((lt == L_SENSE || lt == L_WAVE || lt == L_MIDI) && j == 3) d[i].fmt = F_PARAM;
                if (lt == L_SENSE && j == 0) { d[i].fmt = F_NAMES; d[i].n = 5; d[i].names = SRC_N_; }
                if ((lt == L_WAVE || lt == L_WOBBLE || lt == L_DICE) && j == (lt == L_DICE ? 1 : 0)) d[i].fmt = F_HZ;
                if ((lt == L_SENSE || lt == L_WAVE) && j == 1) d[i].fmt = F_BIP;
                if (lt == L_MIDI && j < 2) d[i].fmt = F_BIP;
                if (lt == L_WOBBLE && (j == 1 || j == 2)) d[i].fmt = F_BIP;
                if (lt == L_WOBBLE && j == 4) { d[i].fmt = F_NAMES; d[i].n = 5; d[i].names = WSH_N; }
                if (lt == L_WAVE && j == 4) { d[i].fmt = F_NAMES; d[i].n = 5; d[i].names = VSH_N; }
                if ((lt == L_WOBBLE || lt == L_WAVE) && j == 5) { d[i].fmt = F_NAMES; d[i].n = 6; d[i].names = SYNC_N; }
                if (lt == L_WAVE && j == 6) d[i].fmt = F_ONOFF;
            } else if (pg == PG_SEQ) {
                int qt = s->m[M_SEQ].type % Q_N;
                dl_mod(&d[i], &s->m[M_SEQ], SEQ_L[qt], i, alt);
                if ((qt == Q_LOOP || qt == Q_ARP) && j == 0) { d[i].fmt = F_NAMES; d[i].n = 7; d[i].names = DIV_N; }
                if (qt == Q_DRAW && j == 2) { d[i].fmt = F_NAMES; d[i].n = 7; d[i].names = DIV_N; }
                if ((qt == Q_LOOP || qt == Q_ARP) && j == 2) d[i].fmt = F_TRIG;
                if (j == 3 && qt != Q_SPIN && qt != Q_LATCH) d[i].fmt = F_ONOFF;
                if (qt == Q_LOOP && j == 4) { d[i].fmt = F_NAMES; d[i].n = 4; d[i].names = DIRN_N; }
                if (qt == Q_GRID && (j == 0 || j == 2)) { d[i].fmt = F_RANGE; d[i].lo = 1; d[i].hi = 16; }
                if (qt == Q_GRID && j == 6) d[i].fmt = F_ONOFF;
                if (qt == Q_SPIN && j == 0) d[i].fmt = F_BIP;
                if (qt == Q_HANDS && j == 1) { d[i].fmt = F_RANGE; d[i].lo = 4; d[i].hi = 16; }
                if (qt == Q_DRAW && j == 0) { d[i].fmt = F_RANGE; d[i].lo = 1; d[i].hi = 32; }
                if (qt == Q_DRAW && j == 5) { d[i].fmt = F_NAMES; d[i].n = 5; d[i].names = SCALE_N; }
                if (qt == Q_ARP && j == 1) { d[i].fmt = F_NAMES; d[i].n = 4; d[i].names = ARPM_N; }
                if (qt == Q_ARP && j == 5) { d[i].fmt = F_NAMES; d[i].n = 4; d[i].names = ARPT_N; }
                if (qt == Q_ARP && j == 6) { d[i].fmt = F_RANGE; d[i].lo = 1; d[i].hi = 3; }
                if (qt == Q_LATCH && j == 0) d[i].fmt = F_NOTE;
                if (qt == Q_LATCH && j == 1) d[i].fmt = F_ONOFF;
                if (qt == Q_LATCH && j == 2) { d[i].fmt = F_RANGE; d[i].lo = -12; d[i].hi = 12; }
            }
        }
    } else if (ui.mode == MD_TAPE) {
        static const char *const L[8] = {"HEAD", "LOOP IN", "SPEED", "LOOP LEN", "SYNC", "TEMPO", "SPEED", "METRO"};
        static const uint8_t F[8] = {F_HEAD, F_BARS, F_SPEED, F_LOOPLEN, F_ONOFF, F_RANGE, F_SPEED, F_PCT};
        uint16_t *V[8] = {0, &pj.loop_in, &pj.speed, &pj.loop_out, &pj.sync, &pj.tempo, &pj.speed, &pj.metro};
        for (i = 0; i < 4; i++) {
            int j = i + (alt ? 4 : 0);
            d[i].label = L[j];
            d[i].v = V[j];
            d[i].fmt = F[j];
            d[i].act = j + 1;
        }
    } else {
        int pg = ui.page[MD_MIX];
        for (i = 0; i < 4; i++) {
            int j = i + (alt ? 4 : 0);
            if (pg == PG_T1) {
                static const char *const L[4] = {"T1", "T2", "T3", "T4"};
                d[i].label = L[i];
                d[i].v = alt ? &pj.t[i].pan : &pj.t[i].level;
                d[i].fmt = alt ? F_PAN : F_PCT;
            } else if (pg == PG_T2) {
                static const char *const L[4] = {"LOW", "MID", "HIGH", "AMOUNT"};
                d[i].label = L[i];
                d[i].v = &pj.eq.p[i];
                d[i].fmt = i < 3 ? F_BIP : F_PCT;
            } else if (pg == PG_T3) {
                dl_mod(&d[i], &pj.mfx, FX_L[pj.mfx.type % FX_N], i, alt);
            } else if (pg == PG_T4) {
                static const char *const L[4] = {"LEFT", "RIGHT", "DRIVE", "RELEASE"};
                d[i].label = L[i];
                d[i].v = &pj.mout.p[i];
            }
            (void)j;
        }
    }
}

static void dial_text(const dl_t *d, char *t)
{
    float k = d->v ? (float)*d->v / 1023.0f : 0.0f;
    char n[16];
    t[0] = 0;
    switch (d->fmt) {
    case F_BIP: num_text((int32_t)((k - 0.5f) * 200.0f + (k < 0.5f ? -0.5f : 0.5f)), t); break;
    case F_NAMES: copy_text(t, d->names[(int)(k * (float)d->n * 0.9999f)], 16); break;
    case F_RANGE: num_text(d->lo + (int32_t)(k * (float)(d->hi - d->lo) + 0.5f), t); break;
    case F_ATT: case F_DEC: case F_REL: case F_DREL: {
        float s = d->fmt == F_ATT ? 0.001f * fm_powf(8000.0f, k) : d->fmt == F_DREL ? 0.03f + 3.0f * k * k
                                                                  : 0.005f * fm_powf(2000.0f, k);
        if (s < 1.0f) {
            num_text((int32_t)(s * 1000.0f), t);
            text_cat(t, "ms", 16);
        } else {
            dec1_text(s, t);
            text_cat(t, "s", 16);
        }
        break;
    }
    case F_MS: num_text((int32_t)(2.0f + 150.0f * k), t); text_cat(t, "ms", 16); break;
    case F_BEND: copy_text(t, "+-", 16); num_text((int32_t)(1.0f + 11.0f * k), n); text_cat(t, n, 16); break;
    case F_HZ: dec1_text(0.05f * fm_exp2f(k * 9.0f), t); text_cat(t, "Hz", 16); break;
    case F_PARAM: {                      /* the target: that module's parameter, by name */
        const sound_t *s = cur_sound(mode_kind());
        int m = (int)(kn(s->m[M_LFO].p[2]) * 3.0f * 0.9999f), p = (int)(k * 7.999f);
        const char *nm = 0;
        if (m == 0)
            nm = mode_kind() ? DRUM_L[s->m[M_ENGINE].type % D_N][p] : ENG_L[s->m[M_ENGINE].type % E_NSYNTH][p];
        else if (m == 1)
            nm = mode_kind() ? ENVD_L[p] : ENVS_L[p];
        else
            nm = FX_L[s->m[M_FX].type % FX_N][p];
        copy_text(t, nm ? nm : "-", 16);
        break;
    }
    case F_TRIG: {                       /* the trigger pattern as dots */
        static const uint8_t TR[8] = {0xFF, 0x55, 0x77, 0x6D, 0x49, 0xDB, 0xAF, 0x11};
        int b, pat = (int)(k * 7.99f);
        for (b = 0; b < 8; b++)
            t[b] = (TR[pat] >> b & 1u) ? 'o' : '.';
        t[8] = 0;
        break;
    }
    case F_NOTE:
        if (k < 0.02f)
            copy_text(t, "OFF", 16);
        else
            note_text(36 + (int)(k * 48.0f), t);
        break;
    case F_SPEED: {
        float sp = k < 0.5f ? 0.25f + 1.5f * k : 1.0f + (k - 0.5f) * 2.0f;
        copy_text(t, "x", 16);
        dec1_text(sp, n);
        text_cat(t, n, 16);
        if (pj.reverse)
            text_cat(t, " <", 16);
        break;
    }
    case F_BARS: num_text(*d->v + 1, t); break;
    case F_LOOPLEN: num_text((int32_t)pj.loop_out - (int32_t)pj.loop_in, t); text_cat(t, " BARS", 16); break;
    case F_HEAD: {
        uint32_t tk = CORE_VIEW.tick;
        num_text((int32_t)(tk / (4u * TPQ)) + 1, t);
        text_cat(t, ".", 16);
        num_text((int32_t)(tk / TPQ % 4u) + 1, n);
        text_cat(t, n, 16);
        break;
    }
    case F_ONOFF:
        if (d->v == &pj.sync)
            copy_text(t, *d->v ? "BEAT" : "FREE", 16);
        else
            copy_text(t, k > 0.5f || (d->v && *d->v == 1) ? "ON" : "OFF", 16);
        break;
    case F_PAN:
        if (*d->v > 500 && *d->v < 524)
            copy_text(t, "C", 16);
        else {
            copy_text(t, *d->v < 512 ? "L" : "R", 16);
            num_text((int32_t)(fm_fabsf(k - 0.5f) * 200.0f), n);
            text_cat(t, n, 16);
        }
        break;
    default: num_text((int32_t)(k * 100.0f + 0.5f), t); break;
    }
}

static float dial_frac(const dl_t *d)
{
    if (!d->v)
        return d->act == 1 ? (float)CORE_VIEW.tick / (float)TAPE_TICKS : 0.0f;
    if (d->fmt == F_BARS || d->fmt == F_LOOPLEN)
        return (float)(d->fmt == F_BARS ? *d->v : (int)pj.loop_out - (int)pj.loop_in) / 32.0f;
    if (d->v == &pj.tempo)
        return ((float)pj.tempo - 40.0f) / 180.0f;
    if (d->v == &pj.sync)
        return pj.sync ? 1.0f : 0.0f;
    return (float)*d->v / 1023.0f;
}

/* ------------------------------------------------------------- the input --- */
static void go_mode(int m)
{
    if (m == MD_SYNTH || m == MD_DRUM) {
        int kind = m == MD_DRUM ? KIND_DRUM : KIND_SYNTH;
        if (pj.t[cur].kind != kind) {
            pj.t[cur].kind = (uint16_t)kind;
            project_changed();
        }
    }
    ui.mode = m;
    ui.path = 0;
    ui.top_dirty = ui.bot_dirty = 1;
    if (ann_on()) {
        static const char *const N[4] = {"SYNTH", "DRUM", "TAPE", "MIXER"};
        ann_say(N[m], 0, ANN_NOW);
    }
}

static void tape_trick(int w, int down)  /* GLO + a white key on the tape screen */
{
    uint32_t bar = CORE_VIEW.tick / (4u * TPQ);
    if (!down) {                         /* the held ones end on release */
        if (w == 3) core_tape_cmd(TC_BREAK, 0);
        if (w == 5 || w == 6) core_tape_cmd(TC_CHOP, 0);
        if (w == 10 || w == 11) {        /* a memo: held 0.6 s stores, a tap recalls */
            int m = w - 10;
            if (plat_ms() - ui.trick_t[w] >= 600u) {
                int t;
                ui.memo[m][0] = pj.speed;
                ui.memo[m][1] = pj.reverse;
                for (t = 0; t < NTRK; t++) {
                    ui.memo[m][2 + t] = pj.t[t].level;
                    ui.memo[m][6 + t] = pj.t[t].pan;
                    ui.memo[m][10 + t] = pj.t[t].mute;
                }
                ui.memo_set[m] = 1;
                app_say(m ? "MEMO 2" : "MEMO 1", "STORED");
            } else if (ui.memo_set[m]) {
                int t;
                pj.speed = ui.memo[m][0];
                pj.reverse = ui.memo[m][1];
                for (t = 0; t < NTRK; t++) {
                    pj.t[t].level = ui.memo[m][2 + t];
                    pj.t[t].pan = ui.memo[m][6 + t];
                    pj.t[t].mute = ui.memo[m][10 + t];
                }
                project_changed();
                app_say(m ? "MEMO 2" : "MEMO 1", "RECALLED");
            } else {
                app_say("MEMO", "HOLD TO STORE");
            }
        }
        return;
    }
    ui.trick_t[w] = plat_ms();
    switch (w) {
    case 0: pj.loop_in = (uint16_t)bar; if (pj.loop_out <= pj.loop_in) pj.loop_out = (uint16_t)(bar + 1u);
        project_changed(); app_say("LOOP IN", "SET"); break;
    case 1: if (bar + 1u > pj.loop_in) pj.loop_out = (uint16_t)(bar + 1u); project_changed(); app_say("LOOP OUT", "SET"); break;
    case 2: pj.loop_on ^= 1u; project_changed(); app_say("LOOP", pj.loop_on ? "ON" : "OFF"); break;
    case 3: core_tape_cmd(TC_BREAK, 1); break;
    case 4: pj.reverse ^= 1u; project_changed(); app_say("TAPE", pj.reverse ? "BACKWARDS" : "FORWARDS"); break;
    case 5: core_tape_cmd(TC_CHOP, 1); break;
    case 6: core_tape_cmd(TC_CHOP, 2); break;
    case 7: core_tape_cmd(TC_LIFT, 0); dirty = 1; app_say("LIFT", pj.loop_on ? "LOOP" : "BAR"); break;
    case 8: core_tape_cmd(TC_DROP, 0); dirty = 1; app_say("DROP", ""); break;
    case 9: core_tape_cmd(TC_LIFT_ALL, 0); dirty = 1; app_say("LIFT", "ALL TRACKS"); break;
    case 12: core_locate(pj.loop_on ? (uint32_t)pj.loop_in * 4u * TPQ : 0u); break;
    case 13: pj.speed = 512; pj.reverse = 0; project_changed(); app_say("SPEED", "x1"); break;
    case 14: pj.metro = pj.metro ? 0 : 700; project_changed(); app_say("METRONOME", pj.metro ? "ON" : "OFF"); break;
    default: break;
    }
}

static void seq_entry(int kind, int note)   /* GLO + a key on the sequencer page */
{
    const sound_t *s = cur_sound(kind);
    seqdata_t *d = cur_seqdata(kind);
    int qt = s->m[M_SEQ].type % Q_N;
    char t[8];
    if (qt == Q_LOOP) {
        if (d->loop_n < SEQ_STEPS)
            d->loop[d->loop_n++] = (uint8_t)note;
        note_text(note, t);
        app_say("STEP", t);
    } else if (qt == Q_GRID) {
        int c = (int)(kn(s->m[M_SEQ].p[0]) * 15.0f + 0.5f);
        d->grid[c] = d->grid[c] == (uint8_t)note ? 0 : (uint8_t)note;
        cur_sound(kind)->m[M_SEQ].p[0] = (uint16_t)(((c + 1) % 16) * 1023 / 15);
    }
    project_changed();
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), maxp = 0;
    int glo = (int)((btn >> B_GLO) & 1u), i, kind = mode_kind();
    int32_t e;
    dl_t d[4];
    static const uint8_t WK[16] = PLAT_WHITE_KEYS;
    touch_poll();
    keys = touch_keys();
    if (glo != ui.glo) {
        ui.glo = glo;
        ui_shift = glo;
        ui.bot_dirty = ui.top_dirty = 1;
    }
    /* ---- the keys ---- */
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k, v;
        int white = -1, w, note;
        for (w = 0; w < 16; w++)
            if (WK[w] == k)
                white = w;
        if ((keys & bit) && touch_pressure(k) > maxp)
            maxp = touch_pressure(k);
        if (!(keys & bit) == !(ui.keys & bit))
            continue;
        if (ui.mode == MD_TAPE && white >= 0 && ((ui.trick_down >> white) & 1u) && !(keys & bit)) {
            ui.trick_down &= ~(1u << white);
            tape_trick(white, 0);
            continue;
        }
        if (ui.mode == MD_TAPE && glo && white >= 0 && (keys & bit)) {   /* a tape trick */
            ui.trick_down |= 1u << white;
            tape_trick(white, 1);
            continue;
        }
        kind = ui.mode == MD_DRUM ? KIND_DRUM : ui.mode == MD_SYNTH ? KIND_SYNTH : pj.t[cur].kind;
        note = kind ? 60 + (int)(k % NDKEY) : 53 + (int)k + 12 * octave;
        if (keys & bit) {
            v = touch_vel(k);
            v = v ? v : 100u;
            if (smp.state == SM_READY) { /* the sampler: the key starts the take */
                smp.key = (uint8_t)k;
                smp.state = SM_REC;
                core_sample_go(1);
                ui.top_dirty = 1;
                continue;
            }
            if (smp.state == SM_REC || smp.state == SM_ERASING)
                continue;
            if (glo && (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) && ui.page[ui.mode] == PG_SEQ) {
                seq_entry(kind, note);
                continue;
            }
            core_key(cur, kind, note, (int)v);
            plat_midi_send(((0x09u | 0x90u << 8) + ((uint32_t)cur << 8)) | (uint32_t)note << 16 | v << 24, TO_USB);
            if (kind) {                  /* a drum key: its pad is the one the knobs edit */
                ui.sel_key[cur] = (int)(k % NDKEY);
                ui.bot_dirty = 1;
            }
        } else {
            if (smp.state == SM_REC && smp.key == (uint8_t)k) {   /* the take ends */
                core_sample_go(0);
                sampler_drain();
                sampler_finish();
                {
                    char t[16];
                    dec1_text((float)smp.len / 22050.0f, t);
                    text_cat(t, " s", 16);
                    app_say("SAMPLED", t);
                }
                if (smp.kind) {          /* a fresh drum sample: 24 equal slices */
                    drum_slice_defaults(pj.t[cur].pads);
                    project_changed();
                }
                continue;
            }
            core_key(cur, kind, note, 0);
            plat_midi_send(((0x08u | 0x80u << 8) + ((uint32_t)cur << 8)) | (uint32_t)note << 16, TO_USB);
        }
    }
    core_pressure((float)maxp * (1.0f / 127.0f));
    /* ---- the buttons ---- */
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        int t_idx = k == B_SEL ? 0 : k == B_ENV ? 1 : k == B_FX ? 2 : k == B_LFO ? 3 : -1;
        if (!(btn & bit) && (ui.btn & bit) && k == B_SAVE) {   /* SAVE let go: a tap saves, a hold was help */
            if (!ui.help && !glo) {
                app_say("SAVING", "...");
                if (project_save() == 0) {
                    dirty = 0;
                    app_say("SAVED", "PROJECT");
                } else {
                    app_say("SAVE", "FAILED");
                }
            }
            ui.help = 0;
            ui.top_dirty = ui.bot_dirty = 1;
        }
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (t_idx >= 0) {                /* T1..T4 */
            if (ui.mode == MD_TAPE) {
                if (glo) {
                    pj.t[t_idx].mute ^= 1u;
                    project_changed();
                    app_say(pj.t[t_idx].mute ? "MUTE" : "UNMUTE", t_idx == 0 ? "T1" : t_idx == 1 ? "T2" : t_idx == 2 ? "T3" : "T4");
                } else {
                    cur = t_idx;
                    core_current(cur);
                    ui.top_dirty = ui.bot_dirty = 1;
                }
            } else if (ui.mode == MD_MIX) {
                if (ui.page[MD_MIX] == t_idx && t_idx == 1) {
                    pj.eq.on ^= 1u;
                    project_changed();
                    app_say("EQ", pj.eq.on ? "ON" : "OFF");
                } else if (ui.page[MD_MIX] == t_idx && t_idx == 2) {
                    pj.mfx.on ^= 1u;
                    project_changed();
                    app_say("MASTER FX", pj.mfx.on ? "ON" : "OFF");
                }
                ui.page[MD_MIX] = t_idx;
                ui.path = 0;
            } else {
                sound_t *s = cur_sound(mode_kind());
                if (ui.page[ui.mode] == t_idx && (t_idx == 2 || t_idx == 3)) {   /* again: on / off */
                    mod_t *m = &s->m[t_idx == 2 ? M_FX : M_LFO];
                    m->on ^= 1u;
                    project_changed();
                    app_say(t_idx == 2 ? "FX" : "LFO", m->on ? "ON" : "OFF");
                }
                ui.page[ui.mode] = t_idx;
            }
            ui.top_dirty = ui.bot_dirty = 1;
            continue;
        }
        switch (k) {
        case B_EDIT:
        case B_ARP: {
            int m = k == B_EDIT ? MD_SYNTH : MD_DRUM;
            if (glo) {                   /* revert to the slot */
                load_slot(m == MD_DRUM, slot_sel[m == MD_DRUM]);
                project_changed();
                app_say("REVERT", m == MD_DRUM ? lib.dname[slot_sel[1]] : lib.sname[slot_sel[0]]);
            } else if (ui.mode == m && ui.page[m] == PG_SEQ) {
                ui.page[m] = PG_T1;
            }
            go_mode(m);
            break;
        }
        case B_HOME:
            if (glo && ui.mode == MD_MIX) {
                ui.path ^= 1u;
                ui.top_dirty = 1;
            } else {
                go_mode(ui.mode == MD_TAPE ? MD_MIX : MD_TAPE);
            }
            break;
        case B_SEQ: {
            int m = ui.mode == MD_DRUM ? MD_DRUM : ui.mode == MD_SYNTH ? MD_SYNTH : (pj.t[cur].kind ? MD_DRUM : MD_SYNTH);
            sound_t *s = cur_sound(m == MD_DRUM);
            if (glo || (ui.mode == m && ui.page[m] == PG_SEQ)) {
                s->m[M_SEQ].on ^= 1u;
                project_changed();
                app_say(SEQ_NAME[s->m[M_SEQ].type % Q_N], s->m[M_SEQ].on ? "ON" : "OFF");
            }
            if (!glo) {
                go_mode(m);
                ui.page[m] = PG_SEQ;
            }
            break;
        }
        case B_PLAY:
            if (glo) {
                pj.reverse ^= 1u;
                project_changed();
                app_say("TAPE", pj.reverse ? "BACKWARDS" : "FORWARDS");
            } else {
                ui.playing ^= 1u;
                core_play(ui.playing);
            }
            ui.top_dirty = 1;
            break;
        case B_REC: {
            ui.rec_t = now;
            ui.erase_done = 0;
            if (glo)
                break;
            if ((ui.mode == MD_SYNTH || ui.mode == MD_DRUM) && ui.page[ui.mode] == PG_T1 &&
                ((ui.mode == MD_SYNTH && pj.t[cur].synth.m[M_ENGINE].type == E_SAMPLER) ||
                 (ui.mode == MD_DRUM && pj.t[cur].drum.m[M_ENGINE].type == D_SLICE))) {
                if (smp.state == SM_OFF) {
                    sampler_arm(ui.mode == MD_DRUM);
                } else if (smp.state == SM_READY) {
                    smp.state = SM_OFF;  /* cancelled: the region stays empty: the factory sample plays */
                    core_sample_go(0);
                    app_say("SAMPLER", "CANCELLED");
                }
                ui.top_dirty = 1;
                break;
            }
            ui.armed ^= 1u;
            core_record(ui.armed);
            app_say("RECORD", ui.armed ? "ARMED" : "OFF");
            break;
        }
        case B_OCTDN:
        case B_OCTUP: {
            int dir = k == B_OCTUP ? 1 : -1;
            if (ui.mode == MD_TAPE) {
                uint32_t bar = CORE_VIEW.tick / (4u * TPQ);
                if (glo)
                    core_locate(dir > 0 ? (uint32_t)(TAPE_TICKS - 4u * TPQ) : 0u);
                else
                    core_locate((uint32_t)((int32_t)bar + dir < 0 ? 0 : (int32_t)bar + dir) * 4u * TPQ);
            } else if (glo && ui.page[ui.mode] == PG_SEQ) {   /* step entry: a rest, or delete the last */
                seqdata_t *sd = cur_seqdata(mode_kind());
                if (dir > 0 && sd->loop_n < SEQ_STEPS)
                    sd->loop[sd->loop_n++] = SEQ_REST;
                else if (dir < 0 && sd->loop_n)
                    sd->loop_n--;
                project_changed();
                app_say("STEPS", dir > 0 ? "REST" : "DELETED");
            } else if (ui.mode == MD_SYNTH) {
                char n[8];
                octave = (int8_t)clamp(octave + dir, -3, 3);
                core_panic();
                num_text(octave, n);
                app_say("OCTAVE", n);
            }
            break;
        }
        case B_SAVE:
            ui.save_t = now;
            if (glo) {
                store_slot(mode_kind());
                {
                    char n[16];
                    copy_text(n, mode_kind() ? "KIT " : "SOUND ", 16);
                    num_text(slot_sel[mode_kind()] + 1, n + (mode_kind() ? 4 : 6));
                    app_say("STORED", n);
                }
                dirty = 1;
            }
            break;
        default: break;
        }
    }
    if (((btn >> B_SAVE) & 1u) && !glo && now - ui.save_t > 600u && !ui.help) {
        ui.help = 1;                     /* SAVE held: help */
        ui.top_dirty = ui.bot_dirty = 1;
    }
    if (glo && ((btn >> B_REC) & 1u) && !ui.erase_done && now - ui.rec_t > 900u) {
        ui.erase_done = 1;
        core_tape_cmd(TC_ERASE_TRACK, 0);
        dirty = 1;
        app_say("ERASED", "TRACK");
    }
    /* ---- the encoders ---- */
    if ((e = plat_enc(EN_PRESET)) != 0) {
        if (ui.mode == MD_SYNTH || ui.mode == MD_DRUM) {
            int kd = mode_kind(), n = (slot_sel[kd] + NSLOT + (e > 0 ? 1 : -1)) % NSLOT;
            char t[24];
            load_slot(kd, n);
            project_changed();
            num_text(n + 1, t);
            text_cat(t, " ", sizeof t);
            text_cat(t, kd ? lib.dname[n] : lib.sname[n], sizeof t);
            app_say(kd ? "KIT" : "SOUND", t);
        } else {
            cur = (cur + NTRK + (e > 0 ? 1 : -1)) % NTRK;
            core_current(cur);
            ui.top_dirty = ui.bot_dirty = 1;
        }
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        char t[8];
        if (glo) {
            pj.metro = (uint16_t)clamp((int32_t)pj.metro + e * 64, 0, 1023);
            num_text((int32_t)pj.metro * 100 / 1023, t);
            app_say("METRONOME", t);
        } else {
            pj.tempo = (uint16_t)clamp((int32_t)pj.tempo + e, 40, 220);
            num_text(pj.tempo, t);
            app_say("TEMPO", t);
        }
        project_changed();
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        int dir = e > 0 ? 1 : -1;
        if (ui.mode == MD_TAPE) {        /* scrub: a beat a click (GLO: a bar) */
            int32_t tk = (int32_t)CORE_VIEW.tick + e * (glo ? 4 * TPQ : TPQ);
            core_locate((uint32_t)(tk < 0 ? 0 : tk));
        } else if (ui.mode == MD_MIX) {
            if (ui.page[MD_MIX] == PG_T3) {
                pj.mfx.type = (uint16_t)(((int)pj.mfx.type + FX_N + dir) % FX_N);
                app_say("MASTER FX", FX_NAME[pj.mfx.type]);
                project_changed();
            }
        } else {
            sound_t *s = cur_sound(mode_kind());
            int pg = ui.page[ui.mode];
            mod_t *m = &s->m[pg == PG_T1 ? M_ENGINE : pg == PG_T3 ? M_FX : pg == PG_T4 ? M_LFO : pg == PG_SEQ ? M_SEQ : M_ENV];
            int n = pg == PG_T1 ? (mode_kind() ? D_N : E_NSYNTH) : pg == PG_T3 ? FX_N : pg == PG_T4 ? L_N : pg == PG_SEQ ? Q_N : 1;
            if (n > 1) {
                const char *nm;
                m->type = (uint16_t)(((int)m->type + n + dir) % n);
                if (pg == PG_T1 && mode_kind()) {   /* a drum engine: its own kind of keys */
                    if (m->type == D_SLICE)
                        factory_slices(pj.t[cur].pads);
                    else
                        drum_kit_defaults(pj.t[cur].pads);
                }
                if (pg == PG_T3 || pg == PG_T4 || pg == PG_SEQ)
                    m->on = 1;
                nm = pg == PG_T1 ? (mode_kind() ? DRUM_NAME[m->type] : ENG_NAME[m->type]) : pg == PG_T3 ? FX_NAME[m->type]
                   : pg == PG_T4 ? LFO_NAME[m->type] : SEQ_NAME[m->type];
                app_say(pg == PG_T1 ? (mode_kind() ? "DRUMS" : "ENGINE") : pg == PG_T3 ? "FX" : pg == PG_T4 ? "LFO" : "SEQUENCER", nm);
                project_changed();
            }
        }
    }
    get_dials(d);
    for (i = 0; i < 4; i++) {
        if ((e = plat_enc(EN_K1 + i)) == 0)
            continue;
        knob_at[i] = now;
        ui.bot_dirty = ui.top_dirty = 1;
        if (d[i].act) {                  /* the tape's knobs */
            switch (d[i].act) {
            case 1: {
                int32_t tk = (int32_t)CORE_VIEW.tick + e * (TPQ / 4);
                core_locate((uint32_t)(tk < 0 ? 0 : tk));
                continue;
            }
            case 2: {
                int len = (int)pj.loop_out - (int)pj.loop_in;
                pj.loop_in = (uint16_t)clamp((int32_t)pj.loop_in + e, 0, TAPE_BARS - 1);
                pj.loop_out = (uint16_t)clamp((int32_t)pj.loop_in + len, (int32_t)pj.loop_in + 1, TAPE_BARS);
                break;
            }
            case 4:
                pj.loop_out = (uint16_t)clamp((int32_t)pj.loop_out + e, (int32_t)pj.loop_in + 1, TAPE_BARS);
                break;
            case 5: pj.sync = e > 0; break;
            case 6: pj.tempo = (uint16_t)clamp((int32_t)pj.tempo + e, 40, 220); break;
            case 7: {                    /* fixed speeds: 1/4 1/2 1 1.5 2 */
                static const uint16_t SP[5] = {0, 171, 512, 767, 1023};
                int s2 = 0, j;
                for (j = 0; j < 5; j++)
                    if (pj.speed >= SP[j])
                        s2 = j;
                pj.speed = SP[clamp(s2 + (e > 0 ? 1 : -1), 0, 4)];
                break;
            }
            default:
                if (d[i].v)
                    *d[i].v = (uint16_t)clamp((int32_t)*d[i].v + e * 8, 0, 1023);
                break;
            }
            project_changed();
            continue;
        }
        if (!d[i].v)
            continue;
        {
            int step = d[i].fmt == F_NAMES ? (int)(1024 / (d[i].n ? d[i].n : 1)) : d[i].fmt == F_ONOFF ? 1023
                     : d[i].fmt == F_RANGE ? (int)(1023 / (d[i].hi - d[i].lo)) + 1 : glo ? 2 : 8;
            int32_t v = clamp((int32_t)*d[i].v + e * step, 0, 1023);
            *d[i].v = (uint16_t)v;
        }
        /* DRAW: the green knob draws at the cursor */
        if ((ui.mode == MD_SYNTH || ui.mode == MD_DRUM) && ui.page[ui.mode] == PG_SEQ && !glo && i == 1) {
            sound_t *s = cur_sound(mode_kind());
            if ((s->m[M_SEQ].type % Q_N) == Q_DRAW) {
                int x = (int)(kn(s->m[M_SEQ].p[0]) * 31.0f + 0.5f);
                cur_seqdata(mode_kind())->draw[x] = (uint8_t)(s->m[M_SEQ].p[1] >> 3);
            }
        }
        project_changed();
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.top_dirty = 1;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void) { input(); }

#include "ui_draw.inc"
