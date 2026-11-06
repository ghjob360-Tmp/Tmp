/* SPDX-License-Identifier: MPL-2.0 */
/* TOYBOX-4 1.0: the module pictures (included by app.c after ui_base.inc). PLACEHOLDER: replaced by the full set.
 * Each draws into the top canvas, inside the picture area (x 0..239, y PIC_Y..PIC_Y+PIC_H-1), using the knob
 * colours (kcol(k) for K1..K4: dim unless that knob was just turned; kdim(k) for the GLO row) and ui_t (seconds)
 * for motion. k[8]: the module's parameters as heard now (0..1). */

static void pic_placeholder(const char *what, const float *k)
{
    int i;
    ctext(120, 60, &FONT_B, what, G3);
    for (i = 0; i < 4; i++)
        cv_rect(30 + i * 50, 110 - (int32_t)(k[i] * 30.0f), 20, (int32_t)(k[i] * 30.0f), kcol(i));
}

/* a synth engine: env = the newest voice's amplitude (0..1), note = its MIDI note, active = a voice sounds */
static void pic_engine(int type, const float k[8], float env, float note, int active)
{
    static const char *const N[E_NSYNTH] = {"STRING", "PULSE", "FM", "CLUSTER", "DIGITAL", "PHASE", "SPECTRA",
                                            "DUAL", "VOLTAGE", "DNA", "SAMPLER"};
    (void)env;
    (void)note;
    (void)active;
    pic_placeholder(N[type % E_NSYNTH], k);
}
/* an effect: level = the signal's level going in (0..1) */
static void pic_fx(int type, const float k[8], float level)
{
    static const char *const N[FX_N] = {"ECHO", "CUBE", "TURBO", "WIRE", "KNOCK", "COIL", "DOPPLER", "HALL", "BITS"};
    (void)level;
    pic_placeholder(N[type % FX_N], k);
}
/* an LFO: m its module (raw parameters), val its output now (-1..1), ph its phase (0..1) */
static void pic_lfo(int type, const mod_t *m, float val, float ph)
{
    static const char *const N[L_N] = {"SENSE", "DICE", "WOBBLE", "WAVE", "MIDI"};
    float k[8];
    int i;
    (void)val;
    (void)ph;
    for (i = 0; i < 8; i++)
        k[i] = kn(m->p[i]);
    pic_placeholder(N[type % L_N], k);
}
/* a sequencer: d its data, step the step playing (-1 none), kind KIND_SYNTH / KIND_DRUM, the live view for SPIN */
static void pic_seq(int type, const mod_t *m, const seqdata_t *d, int step, int kind)
{
    static const char *const N[Q_N] = {"LOOP", "GRID", "SPIN", "HANDS", "DRAW", "ARP", "LATCH"};
    float k[8];
    int i;
    (void)d;
    (void)step;
    (void)kind;
    for (i = 0; i < 8; i++)
        k[i] = kn(m->p[i]);
    pic_placeholder(N[type % Q_N], k);
}
