/* SPDX-License-Identifier: MPL-2.0 */
/* logue SDK units in CHAIN-1: loading (mk1 payloads, mkII ELF images), the NTS-1's API (the tables in logue_rom.c,
 * the calls below), and the calls into a unit. See logue.h. */
#include "logue.h"
#include "logue_rom.h"

#define LG_CODE PLAT_RAM_CODE             /* what runs on the audio path (the second core): RAM; the loading: flash */
#define INL static inline __attribute__((always_inline))

/* the guest's map: the unit's own memory, its stack and I/O, its "SDRAM" */
#define G_MK2 0x10000000u                /* a mkII image */
#define G_STACK 0x30000000u              /* the stack (6 KiB), then the I/O area (2 KiB) */
#define STACK_SZ 6144u
#define IO_SZ 2048u
#define G_IO (G_STACK + STACK_SZ)
#define G_SDRAM2 0xC0000000u             /* a mkII unit's allocations */
/* the runtime's own calls (addresses in the trap range, past the API's) */
#define T_BASE 0x0F000000u
enum { T_ALLOC, T_FREE, T_AVAIL, T_NOTIFY, T_UNKNOWN, T_N };
/* the I/O area's layout (offsets) */
#define IO_CTX 0u                        /* osc params / context: 32 bytes */
#define IO_DESC 32u                      /* mkII runtime descriptor: 32 bytes */
#define IO_ZERO 64u                      /* 192 zero bytes: what an unknown data symbol points at */
#define IO_A 256u                        /* buffers: 4 x 64 floats */
#define IO_B (IO_A + 256u)
#define IO_C (IO_A + 512u)
#define IO_D (IO_A + 768u)

#define BUDGET_FRAME 20000u              /* instructions a frame may take before the unit is stopped */
#define BUDGET_INIT 8000000u
#define BUDGET_EVENT 60000u              /* a parameter, a note: what one may take on the audio path */

INL uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
INL uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
INL void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
INL void bzero8(uint8_t *p, uint32_t n)
{
    while (n--)
        *p++ = 0;
}
static void bcopy8(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n--)
        *d++ = *s++;
}
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static void copy_name(char *d, const uint8_t *s, uint32_t max)   /* printable ASCII, upper case */
{
    uint32_t i;
    for (i = 0; i + 1u < max && s[i]; i++)
        d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?';
    d[i] = 0;
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

const char *lg_error(int e)
{
    static const char *const T[] = {"", "NOT A UNIT", "NOT FOR NTS-1", "NO ROOM", "MISSING API", "INIT FAILED",
                                    "CRASHED", "TOO SLOW", "NEEDS MORE RAM"};
    return e >= 0 && e <= LG_E_MEMORY ? T[e] : "?";
}

int lg_check(const uint8_t *img, uint32_t len)
{
    const lg_hdr_t *h = (const lg_hdr_t *)(const void *)img;
    if (len < LG_HDR || rd32(img) != LG_MAGIC || (h->fmt != LG_FMT_MK1 && h->fmt != LG_FMT_MK2))
        return LG_E_FORMAT;
    if (rd32(img + 8) > len - LG_HDR || crc32(img + LG_HDR, rd32(img + 8)) != rd32(img + 20))
        return LG_E_FORMAT;
    return LG_E_NONE;
}

/* the unit_header of a mkII image, found through its dynamic symbols (nothing linked: the header has no pointers) */
static const uint8_t *mk2_header(const uint8_t *code, uint32_t size, uint32_t dyn)
{
    uint32_t hash = 0, symtab = 0, strtab = 0, nsym, i;
    const uint8_t *d;
    for (d = dyn + 8u <= size ? code + dyn : 0; d && rd32(d) != 0u;) {
        uint32_t o = (uint32_t)(d - code) + 8u;
        if (rd32(d) == 4u) hash = rd32(d + 4);
        if (rd32(d) == 5u) strtab = rd32(d + 4);
        if (rd32(d) == 6u) symtab = rd32(d + 4);
        d = o + 8u <= size ? code + o : 0;
    }
    if (!d || !hash || hash + 8u > size || !symtab || !strtab)
        return 0;
    nsym = rd32(code + hash + 4);
    if (symtab + 16u * nsym > size)
        return 0;
    for (i = 1; i < nsym; i++) {
        const uint8_t *sy = code + symtab + 16u * i;
        uint32_t nm = strtab + rd32(sy), v = rd32(sy + 4);
        if (rd16(sy + 14) && nm + 12u <= size && streq((const char *)code + nm, "unit_header"))
            return v + 56u + 32u * LG_NPARAM <= size ? code + v : 0;
    }
    return 0;
}

int lg_peek(const uint8_t *img, lg_info_t *in)
{
    const lg_hdr_t *h = (const lg_hdr_t *)(const void *)img;
    uint32_t i;
    in->fmt = h->fmt;
    in->crc = rd32(img + 20);
    if (h->fmt == LG_FMT_MK1) {
        static const char *const OSC_FIXED[2] = {"SHAPE", "ALT"}, *const FX[3] = {"TIME", "DEPTH", "MIX"};
        in->module = h->module;
        copy_name(in->name, (const uint8_t *)h->name, sizeof in->name);
        if (h->module == LG_MOD_OSC) {
            in->nparam = (uint8_t)(2u + (h->nparam > 6u ? 6u : h->nparam));
            for (i = 0; i < in->nparam; i++) {
                lg_param_t *q = &in->param[i];
                if (i < 2u) {
                    copy_name(q->name, (const uint8_t *)OSC_FIXED[i], sizeof q->name);
                    q->min = 0, q->max = 1023, q->init = 0, q->type = LG_T_KNOB;
                } else {
                    copy_name(q->name, (const uint8_t *)h->param[i - 2u].name, sizeof q->name);
                    q->min = h->param[i - 2u].min, q->max = h->param[i - 2u].max, q->init = q->min;
                    q->type = h->param[i - 2u].type;
                }
                q->frac = 0;
            }
        } else if (h->module >= LG_MOD_MODFX && h->module <= LG_MOD_REVFX) {
            in->nparam = h->module == LG_MOD_MODFX ? 2 : 3;
            for (i = 0; i < in->nparam; i++) {
                copy_name(in->param[i].name, (const uint8_t *)FX[i], sizeof in->param[i].name);
                in->param[i].min = 0, in->param[i].max = 1023, in->param[i].init = i == 2u ? 512 : 300;
                in->param[i].type = LG_T_KNOB, in->param[i].frac = 0;
            }
        } else {
            return LG_E_PLATFORM;
        }
        return LG_E_NONE;
    }
    {
        const uint8_t *uh = mk2_header(img + LG_HDR, h->size, h->dyn);
        uint32_t target;
        if (!uh)
            return LG_E_FORMAT;
        target = rd32(uh + 4);
        if ((target >> 8) != 5u || (target & 0xFFu) < LG_MOD_MODFX || (target & 0xFFu) > LG_MOD_OSC)
            return LG_E_PLATFORM;
        in->module = (uint8_t)(target & 0xFFu);
        copy_name(in->name, uh + 24, sizeof in->name);
        in->nparam = (uint8_t)(rd32(uh + 52) > LG_NPARAM ? LG_NPARAM : rd32(uh + 52));
        for (i = 0; i < in->nparam; i++) {
            const uint8_t *p = uh + 56u + 32u * i;
            lg_param_t *q = &in->param[i];
            q->min = (int16_t)rd16(p);
            q->max = (int16_t)rd16(p + 2);
            q->init = (int16_t)rd16(p + 6);
            q->type = p[8];
            q->frac = p[9] & 0x1Fu;                       /* the fraction (bits 0-3) and its mode (bit 4) */
            copy_name(q->name, p + 10, sizeof q->name);
        }
    }
    return LG_E_NONE;
}

/* ----------------------------------------------------------------------------------- the API calls --- */
static LG_CODE uint32_t pm_rand(uint32_t s)               /* Park-Miller-Carta, as the SDK's */
{
    uint32_t lo = 16807u * (s & 0xFFFFu), hi = 16807u * (s >> 16);
    lo += (hi & 0x7FFFu) << 16;
    lo += hi >> 15;
    return (lo & 0x7FFFFFFFu) + (lo >> 31);
}

static LG_CODE float white(lg_unit_t *u)                  /* Gaussian-ish noise, sd 0.3 */
{
    uint32_t a = pm_rand(u->rnd), b = pm_rand(a), c = pm_rand(b), d = pm_rand(c);
    u->rnd = d;
    /* four uniforms: (sum - 2) has sd 0.577; scaled to 0.3 */
    return ((float)(a >> 8) + (float)(b >> 8) + (float)(c >> 8) + (float)(d >> 8)) * (0.52f / 8388608.0f) - 1.04f;
}

static LG_CODE float bl_idx(float note, const uint8_t *notes)   /* osc_bl_*_idx: the band-limited table for note */
{
    uint32_t i = 0;
    float prev;
    while (i < 6u && (float)notes[i] < note)
        i++;
    prev = i ? (float)notes[i - 1u] : 0.0f;
    note = (float)i + (note - prev) / ((float)notes[i] - prev);
    return note > 6.0f ? 6.0f : note;
}

LG_CODE void arm_trap(arm_t *m, uint32_t addr)
{
    lg_unit_t *u = (lg_unit_t *)m->user;
    switch (addr) {
    case LOGUE_OSC_MCU_HASH: case LOGUE_FX_MCU_HASH: m->r[0] = 0x464D3121u; break;   /* "!1MF" */
    case LOGUE_OSC_RAND: case LOGUE_FX_RAND: u->rnd = pm_rand(u->rnd); m->r[0] = u->rnd; break;
    case LOGUE_OSC_WHITE: case LOGUE_FX_WHITE: m->s.f[0] = white(u); break;
    case LOGUE_OSC_BL_SAW_IDX: case LOGUE_OSC_BL_SQR_IDX: case LOGUE_OSC_BL_PAR_IDX: {
        /* wt_saw_notes, wt_sqr_notes, wt_par_notes in the ROM */
        uint32_t t = addr == LOGUE_OSC_BL_SAW_IDX ? 0x08012bd4u : addr == LOGUE_OSC_BL_SQR_IDX ? 0x08011db0u : 0x08010f8cu;
        const uint8_t *notes = arm_ptr(m, t, 8, 0);
        m->s.f[0] = notes ? bl_idx(m->s.f[0], notes) : 0.0f;
        break;
    }
    /* tempo: as the unit would count it at 48 kHz (its delays in samples come out right at 44.1) */
    case LOGUE_FX_GET_BPM: m->r[0] = (uint32_t)(u->bpm * (48000.0f / 44100.0f) * 10.0f + 0.5f); break;
    case LOGUE_FX_GET_BPMF: m->s.f[0] = u->bpm * (48000.0f / 44100.0f); break;
    case T_BASE + 4u * T_ALLOC: {                             /* sdram_alloc(size): 8-byte aligned, never freed */
        uint32_t n = (m->r[0] + 7u) & ~7u;
        if (n <= u->sdram_size - u->sdram_used) {
            m->r[0] = G_SDRAM2 + u->sdram_used;
            u->sdram_used += n;
        } else {
            m->r[0] = 0;
        }
        break;
    }
    case T_BASE + 4u * T_FREE: case T_BASE + 4u * T_NOTIFY: break;
    case T_BASE + 4u * T_AVAIL: m->r[0] = u->sdram_size - u->sdram_used; break;
    default:
        m->fault = ARM_F_TRAP;
        m->fault_addr = addr;
        break;
    }
}

/* ------------------------------------------------------------------------------------------ calls --- */
/* a call with up to 5 arguments (the fifth on the stack), budget instructions; a fault stops the unit */
static LG_CODE int ucall(lg_unit_t *u, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                        uint32_t budget)
{
    uint32_t args[4], sp = u->sp;
    uint32_t before = u->m.count;
    int rc;
    if (!fn || u->state != LG_READY)
        return -1;
    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    sp -= 8u;
    arm_wr32(&u->m, sp, a4);
    rc = arm_call(&u->m, fn, sp, args, 4, budget);
    u->load = u->m.count - before;
    if (rc) {
        u->state = LG_FAULT;
        u->err = rc == ARM_F_BUDGET ? LG_E_SLOW
               : rc == ARM_F_MEM && u->m.fault_addr >= 0xC0000000u ? LG_E_MEMORY : LG_E_FAULT;
    }
    return rc;
}

/* ------------------------------------------------------------------------------------------ mkII --- */
enum { DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT = 12, DT_REL = 17, DT_RELSZ = 18,
       DT_PLTRELSZ = 2, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27 };

typedef struct {
    uint8_t *img;
    uint32_t size, symtab, strtab, nsym;
} elf_t;

static const uint8_t *in_img(const elf_t *e, uint32_t off, uint32_t n)
{
    return off <= e->size && n <= e->size - off ? e->img + off : 0;
}

static uint32_t export_sym(const elf_t *e, const char *name)   /* a defined dynamic symbol's guest address, 0 none */
{
    uint32_t i;
    for (i = 1; i < e->nsym; i++) {
        const uint8_t *s = in_img(e, e->symtab + 16u * i, 16);
        const uint8_t *nm;
        if (!s || !rd16(s + 14))
            continue;
        nm = in_img(e, e->strtab + rd32(s), 1);
        if (nm && streq((const char *)nm, name))
            return G_MK2 + rd32(s + 4);
    }
    return 0;
}

static int mk2_link(lg_unit_t *u, elf_t *e, uint32_t dyn, const logue_sym_t *rom)
{
    uint32_t hash = 0, rel = 0, relsz = 0, jmp = 0, jmpsz = 0, i;
    const uint8_t *d;
    for (d = in_img(e, dyn, 8); d && rd32(d) != DT_NULL; d = in_img(e, (uint32_t)(d - e->img) + 8u, 8)) {
        uint32_t tag = rd32(d), val = rd32(d + 4);
        switch (tag) {
        case DT_HASH: hash = val; break;
        case DT_STRTAB: e->strtab = val; break;
        case DT_SYMTAB: e->symtab = val; break;
        case DT_REL: rel = val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmp = val; break;
        case DT_PLTRELSZ: jmpsz = val; break;
        default: break;
        }
    }
    if (!d || !hash || !in_img(e, hash, 8) || !e->symtab || !e->strtab)
        return LG_E_FORMAT;
    e->nsym = rd32(e->img + hash + 4);
    if (!in_img(e, e->symtab, 16u * e->nsym))
        return LG_E_FORMAT;
    for (i = 0; i < 2u; i++) {
        uint32_t at = i ? jmp : rel, n = (i ? jmpsz : relsz) / 8u, k;
        for (k = 0; k < n; k++) {
            const uint8_t *r = in_img(e, at + 8u * k, 8);
            uint32_t off, info, type, sym, S = 0, A;
            uint8_t *P;
            if (!r)
                return LG_E_FORMAT;
            off = rd32(r);
            info = rd32(r + 4);
            type = info & 0xFFu;
            sym = info >> 8;
            P = (uint8_t *)in_img(e, off, 4);
            if (!P || sym >= e->nsym)
                return LG_E_FORMAT;
            A = rd32(P);
            if (sym) {
                const uint8_t *s = e->img + e->symtab + 16u * sym;
                if (rd16(s + 14)) {
                    S = G_MK2 + rd32(s + 4);
                } else {                                      /* from the firmware */
                    const uint8_t *nm = in_img(e, e->strtab + rd32(s), 1);
                    const logue_sym_t *t;
                    uint32_t bind = s[12] >> 4, stype = s[12] & 15u;
                    for (t = rom; t->name && nm && !streq(t->name, (const char *)nm); t++)
                        ;
                    if (t->name) {                            /* a table, or an API call (Thumb) */
                        S = t->addr >= (rom == logue_rom_osc_syms ? LOGUE_OSC_TRAP_LO : LOGUE_FX_TRAP_LO) ? t->addr | 1u
                                                                                                          : t->addr;
                    } else if (bind == 2u) {
                        S = 0;                                /* weak: absent */
                    } else if (type == 22u || stype == 2u) {
                        S = (T_BASE + 4u * T_UNKNOWN) | 1u;   /* a function we do not have: stops the unit if called */
                    } else {
                        S = G_IO + IO_ZERO;                   /* data we do not have (newlib's __sf ...): zeros */
                    }
                }
            }
#ifdef LG_DEBUG
            {
                extern int printf(const char *, ...);
                printf("  reloc %08x type %u sym %u S %08x A %08x\n", off, type, sym, S, A);
            }
#endif
            switch (type) {
            case 23u: wr32(P, A + G_MK2); break;              /* R_ARM_RELATIVE */
            case 2u: wr32(P, S + A); break;                   /* R_ARM_ABS32 */
            case 21u: case 22u: wr32(P, S); break;            /* R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT */
            case 0u: break;
            default: return LG_E_LINK;
            }
        }
    }
    (void)u;
    return LG_E_NONE;
}

static int mk2_load(lg_unit_t *u, const lg_hdr_t *h, const uint8_t *code)
{
    elf_t e;
    uint32_t win = (h->memsz + 7u) & ~7u, hdr, i, init = 0, init_arr = 0, init_arrsz = 0, target;
    const uint8_t *d, *uh;
    int rc;
    if (h->size > h->memsz || win + STACK_SZ + IO_SZ > u->arena_size)
        return LG_E_ROOM;
    e.img = u->arena;
    e.size = h->memsz;
    bcopy8(e.img, code, h->size);
    bzero8(e.img + h->size, win - h->size);
    /* which module (its header) decides the API it is linked against */
    {
        elf_t t = e;
        uint32_t hash = 0;
        for (d = in_img(&t, h->dyn, 8); d && rd32(d) != DT_NULL; d = in_img(&t, (uint32_t)(d - t.img) + 8u, 8)) {
            if (rd32(d) == DT_HASH) hash = rd32(d + 4);
            if (rd32(d) == DT_SYMTAB) t.symtab = rd32(d + 4);
            if (rd32(d) == DT_STRTAB) t.strtab = rd32(d + 4);
        }
        if (!hash || !in_img(&t, hash, 8))
            return LG_E_FORMAT;
        t.nsym = rd32(t.img + hash + 4);
        hdr = export_sym(&t, "unit_header");
    }
    uh = hdr ? in_img(&e, hdr - G_MK2, 56u + 32u * LG_NPARAM) : 0;
    if (!uh)
        return LG_E_FORMAT;
    target = rd32(uh + 4);
    if ((target >> 8) != 5u)
        return LG_E_PLATFORM;
    u->module = (uint8_t)(target & 0xFFu);
    if (u->module < LG_MOD_MODFX || u->module > LG_MOD_OSC)
        return LG_E_PLATFORM;
    u->sdram_size = u->arena_size - win - STACK_SZ - IO_SZ;
    u->sdram_used = 0;
    arm_map(&u->m, G_MK2, win, u->arena, 1);
    arm_map(&u->m, G_STACK, STACK_SZ + IO_SZ, u->arena + win, 1);
    arm_map(&u->m, G_SDRAM2, u->sdram_size, u->arena + win + STACK_SZ + IO_SZ, 1);
    if (u->module == LG_MOD_OSC) {
        arm_map(&u->m, LOGUE_ROM_OSC_BASE, 4u * LOGUE_ROM_OSC_WORDS, (void *)(uintptr_t)logue_rom_osc, 0);
        u->m.trap_lo = LOGUE_OSC_TRAP_LO;
    } else {
        arm_map(&u->m, LOGUE_ROM_FX_BASE, 4u * LOGUE_ROM_FX_WORDS, (void *)(uintptr_t)logue_rom_fx, 0);
        u->m.trap_lo = LOGUE_FX_TRAP_LO;
    }
    u->m.trap_hi = T_BASE + 4u * T_N;
    rc = mk2_link(u, &e, h->dyn, u->module == LG_MOD_OSC ? logue_rom_osc_syms : logue_rom_fx_syms);
    if (rc)
        return rc;
    copy_name(u->name, uh + 24, sizeof u->name);
    u->nparam = (uint8_t)(rd32(uh + 52) > LG_NPARAM ? LG_NPARAM : rd32(uh + 52));
    for (i = 0; i < u->nparam; i++) {
        const uint8_t *p = uh + 56u + 32u * i;
        lg_param_t *q = &u->param[i];
        q->min = (int16_t)rd16(p);
        q->max = (int16_t)rd16(p + 2);
        q->init = (int16_t)rd16(p + 6);
        q->type = p[8];
        q->frac = p[9] & 15u;
        copy_name(q->name, p + 10, sizeof q->name);
    }
    u->f_init = export_sym(&e, "unit_init");
    u->f_render = export_sym(&e, "unit_render");
    u->f_param = export_sym(&e, "unit_set_param_value");
    u->f_on = export_sym(&e, "unit_note_on");
    u->f_off = export_sym(&e, "unit_note_off");
    u->f_alloff = export_sym(&e, "unit_all_note_off");
    u->f_suspend = export_sym(&e, "unit_suspend");
    u->f_resume = export_sym(&e, "unit_resume");
    u->f_tempo = export_sym(&e, "unit_set_tempo");
    u->f_bend = export_sym(&e, "unit_pitch_bend");
    if (!u->f_init || !u->f_render)
        return LG_E_LINK;
    /* the constructors (C++ statics), then unit_init */
    for (d = in_img(&e, h->dyn, 8); d && rd32(d) != DT_NULL; d = in_img(&e, (uint32_t)(d - e.img) + 8u, 8)) {
        if (rd32(d) == DT_INIT) init = rd32(d + 4);
        if (rd32(d) == DT_INIT_ARRAY) init_arr = rd32(d + 4);
        if (rd32(d) == DT_INIT_ARRAYSZ) init_arrsz = rd32(d + 4);
    }
    u->state = LG_READY;
    if (init && PLAT_FAR(ucall)(u, G_MK2 + init, 0, 0, 0, 0, 0, BUDGET_INIT))
        return u->err;
    for (i = 0; i < init_arrsz / 4u; i++) {
        const uint8_t *f = in_img(&e, init_arr + 4u * i, 4);
        if (f && rd32(f) && rd32(f) != 0xFFFFFFFFu && PLAT_FAR(ucall)(u, rd32(f), 0, 0, 0, 0, 0, BUDGET_INIT))
            return u->err;
    }
    {
        uint8_t *io = u->arena + win + STACK_SZ, *dsc = io + IO_DESC;
        uint8_t in = 2, out = u->module == LG_MOD_OSC ? 1 : 2;
        bzero8(io, IO_SZ);
        wr32(dsc, target);
        wr32(dsc + 4, 2u << 16);                              /* API 2.0.0 */
        wr32(dsc + 8, 48000u);                                /* (what the units expect; see logue_rom.c) */
        dsc[12] = 64;
        dsc[13] = 0;
        dsc[14] = in;
        dsc[15] = out;
        wr32(dsc + 16, G_IO + IO_CTX);
        wr32(dsc + 20, (T_BASE + 4u * T_ALLOC) | 1u);
        wr32(dsc + 24, (T_BASE + 4u * T_FREE) | 1u);
        wr32(dsc + 28, (T_BASE + 4u * T_AVAIL) | 1u);
        wr32(io + IO_CTX + 12, (T_BASE + 4u * T_NOTIFY) | 1u);   /* the osc context's notify_input_usage */
    }
    if (PLAT_FAR(ucall)(u, u->f_init, G_IO + IO_DESC, 0, 0, 0, 0, BUDGET_INIT))
        return u->err;
    if ((int8_t)u->m.r[0] != 0)
        return u->m.r[0] == (uint32_t)-16 || (int8_t)u->m.r[0] == -16 ? LG_E_MEMORY : LG_E_INIT;
    for (i = 0; i < u->nparam; i++)
        PLAT_FAR(lg_param)(u, (int)i, u->param[i].init);
    return u->state == LG_READY ? LG_E_NONE : u->err;
}

/* ------------------------------------------------------------------------------------------- mk1 --- */
static int mk1_load(lg_unit_t *u, const lg_hdr_t *h, const uint8_t *code)
{
    uint32_t org, win, sd, magic, i;
    static const char *const OSC_FIXED[2] = {"SHAPE", "ALT"};
    switch (h->module) {
    case LG_MOD_OSC: org = 0x20000000u; win = 32768u; sd = 0; magic = 0x43534F55u; break;      /* UOSC */
    case LG_MOD_MODFX: org = 0x20017800u; win = 6144u; sd = 0xC0400000u; magic = 0x444F4D55u; break;   /* UMOD */
    case LG_MOD_DELFX: org = 0x20019000u; win = 12288u; sd = 0xC0420000u; magic = 0x4C454455u; break;  /* UDEL */
    case LG_MOD_REVFX: org = 0x20019000u; win = 12288u; sd = 0xC0420000u; magic = 0x56455255u; break;  /* UREV */
    default: return LG_E_PLATFORM;
    }
    if (h->size > win || h->size < 48u)
        return LG_E_FORMAT;
    if (win + STACK_SZ + IO_SZ > u->arena_size)
        return LG_E_ROOM;
    u->module = h->module;
    bcopy8(u->arena, code, h->size);
    bzero8(u->arena + h->size, win - h->size);
    if (rd32(u->arena) != magic)
        return LG_E_FORMAT;
    if ((u->arena[8] & 0x7Fu) != 3u)                          /* built for another platform */
        return LG_E_PLATFORM;
    u->sdram_size = u->arena_size - win - STACK_SZ - IO_SZ;
    arm_map(&u->m, org, win, u->arena, 1);
    arm_map(&u->m, G_STACK, STACK_SZ + IO_SZ, u->arena + win, 1);
    if (sd)
        arm_map(&u->m, sd, u->sdram_size, u->arena + win + STACK_SZ + IO_SZ, 1);
    if (u->module == LG_MOD_OSC) {
        arm_map(&u->m, LOGUE_ROM_OSC_BASE, 4u * LOGUE_ROM_OSC_WORDS, (void *)(uintptr_t)logue_rom_osc, 0);
        u->m.trap_lo = LOGUE_OSC_TRAP_LO;
    } else {
        arm_map(&u->m, LOGUE_ROM_FX_BASE, 4u * LOGUE_ROM_FX_WORDS, (void *)(uintptr_t)logue_rom_fx, 0);
        u->m.trap_lo = LOGUE_FX_TRAP_LO;
    }
    u->m.trap_hi = T_BASE + 4u * T_N;
    bzero8(u->arena + win, STACK_SZ + IO_SZ);
    {
        uint32_t f[7];
        for (i = 0; i < 7u; i++)
            f[i] = rd32(u->arena + 16u + 4u * i);
        if (u->module == LG_MOD_OSC) {
            u->f_render = f[1];
            u->f_on = f[2];
            u->f_off = f[3];
            u->f_mute = f[4];
            u->f_value = f[5];
            u->f_param = f[6];
        } else {
            u->f_render = f[1];
            u->f_suspend = f[2];
            u->f_resume = f[3];
            u->f_param = f[4];
        }
        u->f_init = f[0];
    }
    copy_name(u->name, (const uint8_t *)h->name, sizeof u->name);
    /* the parameters: the osc's SHAPE and ALT, then its manifest's; the effects' TIME, DEPTH (and MIX) */
    if (u->module == LG_MOD_OSC) {
        u->nparam = (uint8_t)(2u + (h->nparam > 6u ? 6u : h->nparam));
        for (i = 0; i < 2u; i++) {
            copy_name(u->param[i].name, (const uint8_t *)OSC_FIXED[i], sizeof u->param[i].name);
            u->param[i].min = 0;
            u->param[i].max = 1023;
            u->param[i].init = 0;
            u->param[i].type = LG_T_KNOB;
        }
        for (i = 2; i < u->nparam; i++) {
            lg_param_t *q = &u->param[i];
            copy_name(q->name, (const uint8_t *)h->param[i - 2u].name, sizeof q->name);
            q->min = h->param[i - 2u].min;
            q->max = h->param[i - 2u].max;
            q->init = q->min;
            q->type = h->param[i - 2u].type;
        }
    } else {
        static const char *const FX[3] = {"TIME", "DEPTH", "MIX"};
        u->nparam = u->module == LG_MOD_MODFX ? 2 : 3;
        for (i = 0; i < u->nparam; i++) {
            copy_name(u->param[i].name, (const uint8_t *)FX[i], sizeof u->param[i].name);
            u->param[i].min = 0;
            u->param[i].max = 1023;
            u->param[i].init = i == 2u ? 512 : 300;
            u->param[i].type = LG_T_KNOB;
        }
    }
    u->state = LG_READY;
    if (PLAT_FAR(ucall)(u, u->f_init, 0x300u | u->module, (1u << 16) | (1u << 8), 0, 0, 0, BUDGET_INIT))
        return u->err;
    for (i = 0; i < u->nparam; i++)
        PLAT_FAR(lg_param)(u, (int)i, u->param[i].init);
    return u->state == LG_READY ? LG_E_NONE : u->err;
}

/* ------------------------------------------------------------------------------------------ public --- */
int lg_load(lg_unit_t *u, const uint8_t *img, uint8_t *arena, uint32_t arena_size)
{
    const lg_hdr_t *h = (const lg_hdr_t *)(const void *)img;
    int rc;
    arm_reset(&u->m);
    u->m.user = u;
    u->state = LG_EMPTY;
    u->arena = arena;
    u->arena_size = arena_size & ~7u;
    u->rnd = 0x1234567u;
    u->bpm = 120.0f;
    u->nparam = 0;
    u->f_init = u->f_render = u->f_param = u->f_on = u->f_off = u->f_mute = u->f_value = 0;
    u->f_suspend = u->f_resume = u->f_tempo = u->f_alloff = u->f_bend = 0;
    u->sp = G_STACK + STACK_SZ;
    u->io = G_IO;
    if (rd32(img) != LG_MAGIC)
        rc = LG_E_FORMAT;
    else if (h->fmt == LG_FMT_MK1)
        rc = mk1_load(u, h, img + LG_HDR);
    else if (h->fmt == LG_FMT_MK2)
        rc = mk2_load(u, h, img + LG_HDR);
    else
        rc = LG_E_FORMAT;
    u->fmt = h->fmt;
    if (rc) {
        u->state = LG_FAULT;
        u->err = (uint8_t)rc;
    } else {
        u->err = LG_E_NONE;
    }
    return rc;
}

void lg_unload(lg_unit_t *u) { u->state = LG_EMPTY; }

LG_CODE void lg_param(lg_unit_t *u, int i, int32_t v)
{
    if (i < 0 || i >= u->nparam || u->state != LG_READY)
        return;
    if (v < u->param[i].min)
        v = u->param[i].min;
    if (v > u->param[i].max)
        v = u->param[i].max;
    u->value[i] = v;
    if (u->fmt == LG_FMT_MK2) {
        ucall(u, u->f_param, (uint32_t)i, (uint32_t)v, 0, 0, 0, BUDGET_EVENT);
    } else if (u->module == LG_MOD_OSC) {
        uint32_t idx = i < 2 ? 6u + (uint32_t)i : (uint32_t)i - 2u;   /* SHAPE 6, ALT 7, the six 0..5 */
        ucall(u, u->f_param, idx, (uint32_t)v & 0xFFFFu, 0, 0, 0, BUDGET_EVENT);
    } else {                                                  /* the effects: 0..1023 as Q31 */
        uint32_t idx = i == 2 ? 3u : (uint32_t)i;             /* TIME 0, DEPTH 1, MIX (shift-depth) 3 */
        uint32_t q = v >= 1023 ? 0x7FFFFFFFu : (uint32_t)v * 2099202u;   /* 2^31 / 1023 */
        ucall(u, u->f_param, idx, q, 0, 0, 0, BUDGET_EVENT);
    }
}

LG_CODE void lg_osc_note(lg_unit_t *u, int on, int note, int vel)
{
    if (u->state != LG_READY || u->module != LG_MOD_OSC)
        return;
    if (u->fmt == LG_FMT_MK2) {
        if (on)
            ucall(u, u->f_on, (uint32_t)note & 0x7Fu, (uint32_t)vel & 0x7Fu, 0, 0, 0, BUDGET_EVENT);
        else
            ucall(u, u->f_off, (uint32_t)note & 0x7Fu, 0, 0, 0, 0, BUDGET_EVENT);
    } else {
        uint8_t *ctx = arm_ptr(&u->m, G_IO + IO_CTX, 16, 1);
        ctx[4] = 0;
        ctx[5] = (uint8_t)note;
        ucall(u, on ? u->f_on : u->f_off, G_IO + IO_CTX, 0, 0, 0, 0, BUDGET_EVENT);
    }
}

LG_CODE void lg_osc(lg_unit_t *u, float *out, uint32_t n, int note, uint32_t fine, float shape_lfo)
{
    uint8_t *io;
    uint32_t i, lfo;
    if (n > LG_FRAMES)
        n = LG_FRAMES;
    if (u->state != LG_READY || u->module != LG_MOD_OSC) {
        for (i = 0; i < n; i++)
            out[i] = 0.0f;
        return;
    }
    io = arm_ptr(&u->m, G_IO, IO_SZ, 1);
    shape_lfo = shape_lfo > 0.99999f ? 0.99999f : shape_lfo < -1.0f ? -1.0f : shape_lfo;
    lfo = (uint32_t)(int32_t)(shape_lfo * 2147483647.0f);
    wr32(io + IO_CTX, lfo);
    io[IO_CTX + 4] = (uint8_t)fine;
    io[IO_CTX + 5] = (uint8_t)note;
    if (u->fmt == LG_FMT_MK2) {
        const float *o = (const float *)(const void *)(io + IO_C);
        bzero8(io + IO_A, 8u * n);                            /* the audio input: none */
        if (ucall(u, u->f_render, G_IO + IO_A, G_IO + IO_C, n, 0, 0, BUDGET_FRAME * n))
            n = 0;
        for (i = 0; i < n; i++)
            out[i] = o[i];
    } else {
        const int32_t *o = (const int32_t *)(const void *)(io + IO_A);
        if (ucall(u, u->f_render, G_IO + IO_CTX, G_IO + IO_A, n, 0, 0, BUDGET_FRAME * n))
            n = 0;
        for (i = 0; i < n; i++)
            out[i] = (float)o[i] * (1.0f / 2147483648.0f);
    }
    for (; i < LG_FRAMES; i++)                               /* (a unit stopped: silence) */
        out[i] = 0.0f;
}

LG_CODE void lg_fx(lg_unit_t *u, float *lr, uint32_t n)
{
    uint8_t *io;
    float *a, *b;
    uint32_t i;
    if (u->state != LG_READY || u->module == LG_MOD_OSC)
        return;
    if (n > LG_FRAMES)
        n = LG_FRAMES;
    io = arm_ptr(&u->m, G_IO, IO_SZ, 1);
    a = (float *)(void *)(io + IO_A);
    b = (float *)(void *)(io + IO_C);
    for (i = 0; i < 2u * n; i++)
        a[i] = lr[i];
    if (u->fmt == LG_FMT_MK2) {
        if (ucall(u, u->f_render, G_IO + IO_A, G_IO + IO_C, n, 0, 0, BUDGET_FRAME * n))
            return;
    } else if (u->module == LG_MOD_MODFX) {                   /* main and sub in and out: the sub silent */
        float *sub = (float *)(void *)(io + IO_B), *subo = (float *)(void *)(io + IO_D);
        for (i = 0; i < 2u * n; i++)
            sub[i] = subo[i] = 0.0f;
        if (ucall(u, u->f_render, G_IO + IO_A, G_IO + IO_C, G_IO + IO_B, G_IO + IO_D, n, BUDGET_FRAME * n))
            return;
    } else {                                                  /* delay, reverb: in place */
        if (ucall(u, u->f_render, G_IO + IO_A, n, 0, 0, 0, BUDGET_FRAME * n))
            return;
        b = a;
    }
    for (i = 0; i < 2u * n; i++) {
        float v = b[i];
        lr[i] = v == v && v < 8.0f && v > -8.0f ? v : 0.0f;   /* (a NaN or a blow-up never reaches the output) */
    }
}

LG_CODE void lg_tempo(lg_unit_t *u, float bpm)
{
    u->bpm = bpm;
    if (u->state == LG_READY && u->fmt == LG_FMT_MK2 && u->f_tempo)   /* 16.16 fixed BPM */
        ucall(u, u->f_tempo, (uint32_t)(bpm * (48000.0f / 44100.0f) * 65536.0f), 0, 0, 0, 0, BUDGET_EVENT);
}

LG_CODE void lg_suspend(lg_unit_t *u, int on)
{
    if (u->state != LG_READY)
        return;
    ucall(u, on ? u->f_suspend : u->f_resume, 0, 0, 0, 0, 0, BUDGET_EVENT);
}
