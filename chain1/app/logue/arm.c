/* SPDX-License-Identifier: MPL-2.0 */
/* ARMv7E-M (Thumb-2, DSP, FPv4-SP) interpreter: see arm.h. Written from the ARMv7-M Architecture Reference Manual's
 * encoding tables (A5, A6, A7); the names below are the manual's. Instructions are decoded each time they run (no
 * cache: the FM-1's RAM is short), the common 16-bit ones first. */
#include "arm.h"

#define ARM_CODE PLAT_RAM_CODE           /* the FM-1: it runs on the audio path (and the second core): RAM */
#define INL static inline __attribute__((always_inline))

#pragma STDC FP_CONTRACT OFF              /* VMLA / VMUL + VADD round twice, as the M4 does; only VFMA fuses */
#ifdef PLAT_HOST                          /* the host (the tests, the browser): exact, as an M4 */
#define ARM_FMAF(a, b, c) __builtin_fmaf((a), (b), (c))
#define ARM_SQRTF(x) __builtin_sqrtf(x)
#else                                     /* the FM-1: no fused multiply-add, no square root in its FPU (1 ulp off) */
#define ARM_FMAF(a, b, c) ((a) * (b) + (c))
#define ARM_SQRTF(x) f_sqrt(x)
#endif

/* --------------------------------------------------------------------------------------- memory --- */
INL uint8_t *mp(arm_t *m, uint32_t a, uint32_t n, int w)
{
    arm_seg_t *s = &m->seg[a >> 28];
    uint32_t o = a - s->base;
    if (o >= s->size || n > s->size - o || (w && !s->w))
        return 0;
    return s->p + o;
}

static ARM_CODE void mfault(arm_t *m, uint32_t a)
{
    if (!m->fault) {
        m->fault = ARM_F_MEM;
        m->fault_addr = a;
    }
}

INL uint32_t ld32(arm_t *m, uint32_t a)
{
    const uint8_t *p = mp(m, a, 4, 0);
    if (!p) {
        mfault(m, a);
        return 0;
    }
    if (!((uintptr_t)p & 3u))
        return *(const uint32_t *)(const void *)p;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
INL uint32_t ld16(arm_t *m, uint32_t a)
{
    const uint8_t *p = mp(m, a, 2, 0);
    if (!p) {
        mfault(m, a);
        return 0;
    }
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
INL uint32_t ld8(arm_t *m, uint32_t a)
{
    const uint8_t *p = mp(m, a, 1, 0);
    if (!p) {
        mfault(m, a);
        return 0;
    }
    return p[0];
}
INL void st32(arm_t *m, uint32_t a, uint32_t v)
{
    uint8_t *p = mp(m, a, 4, 1);
    if (!p) {
        mfault(m, a);
        return;
    }
    if (!((uintptr_t)p & 3u)) {
        *(uint32_t *)(void *)p = v;
        return;
    }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
INL void st16(arm_t *m, uint32_t a, uint32_t v)
{
    uint8_t *p = mp(m, a, 2, 1);
    if (!p) {
        mfault(m, a);
        return;
    }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
INL void st8(arm_t *m, uint32_t a, uint32_t v)
{
    uint8_t *p = mp(m, a, 1, 1);
    if (!p) {
        mfault(m, a);
        return;
    }
    p[0] = (uint8_t)v;
}

ARM_CODE uint8_t *arm_ptr(arm_t *m, uint32_t addr, uint32_t n, int write) { return mp(m, addr, n, write); }
ARM_CODE uint32_t arm_rd32(arm_t *m, uint32_t addr)
{
    const uint8_t *p = mp(m, addr, 4, 0);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0u;
}
ARM_CODE void arm_wr32(arm_t *m, uint32_t addr, uint32_t v)
{
    uint8_t *p = mp(m, addr, 4, 1);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }
}

void arm_reset(arm_t *m)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)m;
    for (i = 0; i < sizeof *m; i++)
        b[i] = 0;
}

void arm_map(arm_t *m, uint32_t base, uint32_t size, void *p, int writable)
{
    arm_seg_t *s = &m->seg[base >> 28];
    s->base = base;
    s->size = size;
    s->p = (uint8_t *)p;
    s->w = (uint8_t)(writable != 0);
}

/* ------------------------------------------------------------------------------------ arithmetic --- */
INL uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n ? v >> n | v << (32u - n) : v;
}

INL uint32_t addc(arm_t *m, uint32_t a, uint32_t b, uint32_t cin, int s)
{
    uint32_t r = a + b + cin;
    if (s) {
        m->n = r >> 31;
        m->z = r == 0u;
        m->c = cin ? r <= a : r < a;
        m->v = ((a ^ r) & (b ^ r)) >> 31;
    }
    return r;
}

INL void nz(arm_t *m, uint32_t r)
{
    m->n = r >> 31;
    m->z = r == 0u;
}

/* Shift_C: type 0 LSL, 1 LSR, 2 ASR, 3 ROR, 4 RRX; n the amount (already decoded); *c the carry in and out */
INL uint32_t shc(uint32_t v, uint32_t type, uint32_t n, uint32_t *c)
{
    if (type == 4u) {
        uint32_t r = *c << 31 | v >> 1;
        *c = v & 1u;
        return r;
    }
    if (n == 0u)
        return v;
    switch (type) {
    case 0:
        if (n >= 32u) {
            *c = n == 32u ? v & 1u : 0u;
            return 0u;
        }
        *c = v >> (32u - n) & 1u;
        return v << n;
    case 1:
        if (n >= 32u) {
            *c = n == 32u ? v >> 31 : 0u;
            return 0u;
        }
        *c = v >> (n - 1u) & 1u;
        return v >> n;
    case 2:
        if (n >= 32u) {
            *c = v >> 31;
            return (uint32_t)((int32_t)v >> 31);
        }
        *c = v >> (n - 1u) & 1u;
        return (uint32_t)((int32_t)v >> n);
    default: {
        uint32_t r = ror32(v, n);
        *c = r >> 31;
        return r;
    }
    }
}

/* DecodeImmShift then Shift_C */
INL uint32_t imm_shift(uint32_t v, uint32_t type, uint32_t imm5, uint32_t *c)
{
    if (type == 0u)
        return shc(v, 0, imm5, c);
    if (type == 3u)
        return imm5 ? shc(v, 3, imm5, c) : shc(v, 4, 1, c);
    return shc(v, type, imm5 ? imm5 : 32u, c);
}

/* ThumbExpandImm_C */
INL uint32_t expand_imm(uint32_t imm12, uint32_t *c)
{
    uint32_t b = imm12 & 0xFFu;
    if (!(imm12 & 0xC00u)) {
        switch (imm12 >> 8 & 3u) {
        case 0: return b;
        case 1: return b << 16 | b;
        case 2: return b << 24 | b << 8;
        default: return b << 24 | b << 16 | b << 8 | b;
        }
    }
    {
        uint32_t r = ror32(0x80u | (imm12 & 0x7Fu), imm12 >> 7);
        *c = r >> 31;
        return r;
    }
}

INL int cond_ok(const arm_t *m, uint32_t cond)
{
    int r;
    switch (cond >> 1) {
    case 0: r = (int)m->z; break;
    case 1: r = (int)m->c; break;
    case 2: r = (int)m->n; break;
    case 3: r = (int)m->v; break;
    case 4: r = m->c && !m->z; break;
    case 5: r = m->n == m->v; break;
    case 6: r = !m->z && m->n == m->v; break;
    default: return 1;
    }
    return (cond & 1u) ? !r : r;
}

INL int32_t ssat(int64_t v, uint32_t n, uint32_t *q)   /* SignedSatQ to n bits (1..32) */
{
    int64_t hi = ((int64_t)1 << (n - 1u)) - 1, lo = -((int64_t)1 << (n - 1u));
    if (v > hi) {
        *q = 1;
        return (int32_t)hi;
    }
    if (v < lo) {
        *q = 1;
        return (int32_t)lo;
    }
    return (int32_t)v;
}
INL uint32_t usat(int64_t v, uint32_t n, uint32_t *q)  /* UnsignedSatQ to n bits (0..31) */
{
    int64_t hi = ((int64_t)1 << n) - 1;
    if (v > hi) {
        *q = 1;
        return (uint32_t)hi;
    }
    if (v < 0) {
        *q = 1;
        return 0u;
    }
    return (uint32_t)v;
}

INL uint32_t apsr(const arm_t *m)
{
    return m->n << 31 | m->z << 30 | m->c << 29 | m->v << 28 | m->q << 27 | m->ge << 16;
}

/* ----------------------------------------------------------------------------------- floats --- */
INL uint32_t f2u(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}
INL float u2f(uint32_t u)
{
    union { float f; uint32_t u; } x;
    x.u = u;
    return x.f;
}
INL int f_nan(float f) { return (f2u(f) & 0x7FFFFFFFu) > 0x7F800000u; }
#ifndef PLAT_HOST
INL float f_sqrt(float x)                  /* Newton from the bit trick: within an ulp or so */
{
    float y;
    if (!(x > 0.0f))
        return x == 0.0f ? x : u2f(0x7FC00000u);
    if (x >= u2f(0x7F800000u))
        return x;
    y = u2f(0x5F375A86u - (f2u(x) >> 1));
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    y = x * y;
    return 0.5f * (y + x / y);
}
#endif
INL int u_nan(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
INL int u_snan(uint32_t u) { return u_nan(u) && !(u & 0x400000u); }
INL float f_neg(float f) { return u2f(f2u(f) ^ 0x80000000u); }

/* the M4's NaNs (FPProcessNaNs, DN off): a signalling operand first, then a quiet one, each made quiet, in operand
 * order; an invalid operation with numbers makes the default NaN. Only looked at when the result is a NaN */
static ARM_CODE float nan_fix(uint32_t a, uint32_t b, uint32_t c, int k)
{
    uint32_t op[3];
    int i;
    op[0] = a;
    op[1] = b;
    op[2] = c;
    for (i = 0; i < k; i++)
        if (u_snan(op[i]))
            return u2f(op[i] | 0x400000u);
    for (i = 0; i < k; i++)
        if (u_nan(op[i]))
            return u2f(op[i]);
    return u2f(0x7FC00000u);
}
#define NAN2(res, x, y) (f_nan(res) ? nan_fix(f2u(x), f2u(y), 0, 2) : (res))
#define NAN3(res, x, y, z) (f_nan(res) ? nan_fix(f2u(x), f2u(y), f2u(z), 3) : (res))

/* round to an integral value, nearest-even (FPSCR's default mode) */
INL float f_rne(float f)
{
    float a = f < 0.0f ? -f : f;
    if (!(a < 8388608.0f))
        return f;
    a = (a + 8388608.0f) - 8388608.0f;
    return f < 0.0f ? -a : a;
}
INL float f_trunc(float f)
{
    float a = f < 0.0f ? -f : f;
    if (!(a < 8388608.0f))
        return f;
    {
        int32_t i = (int32_t)a;
        a = (float)i;
    }
    return f < 0.0f ? -a : a;
}

/* FPToFixed to 32 bits (signed / unsigned), saturating; f already scaled and rounded to an integral value */
INL uint32_t f_to_int(float f, int sgn)
{
    if (f_nan(f))
        return 0u;
    if (sgn) {
        if (f >= 2147483648.0f)
            return 0x7FFFFFFFu;
        if (f < -2147483648.0f)
            return 0x80000000u;
        return (uint32_t)(int32_t)f;
    }
    if (f >= 4294967296.0f)
        return 0xFFFFFFFFu;
    if (f <= 0.0f)
        return 0u;
    if (f >= 2147483648.0f)
        return 0x80000000u + (uint32_t)(int32_t)(f - 2147483648.0f);
    return (uint32_t)(int32_t)f;
}

INL float f_from_u32(uint32_t u)
{
    if (u < 0x80000000u)
        return (float)(int32_t)u;
    {                                                       /* (no unsigned convert routine in the flash) */
        uint32_t h = u >> 1 | (u & 1u);                     /* halved, the lost bit kept sticky: rounds once */
        return (float)(int32_t)h * 2.0f;
    }
}

INL float f_scale2(float f, int e)                          /* f * 2^e, e in -32..32 */
{
    while (e > 0) {
        int k = e > 30 ? 30 : e;
        f *= (float)(1u << k);
        e -= k;
    }
    while (e < 0) {
        int k = -e > 30 ? 30 : -e;
        f *= 1.0f / (float)(1u << k);
        e += k;
    }
    return f;
}

INL void f_cmp(arm_t *m, float a, float b)
{
    uint32_t f;
    if (f_nan(a) || f_nan(b))
        f = 0x3u;                                           /* unordered: C V */
    else if (a == b)
        f = 0x6u;                                           /* Z C */
    else if (a < b)
        f = 0x8u;                                           /* N */
    else
        f = 0x2u;                                           /* C */
    m->fpscr = (m->fpscr & 0x0FFFFFFFu) | f << 28;
}

INL float vfp_expand(uint32_t imm8)
{
    uint32_t b6 = imm8 >> 6 & 1u, e = (b6 ^ 1u) << 7 | (b6 ? 0x7Cu : 0u) | (imm8 >> 4 & 3u);
    return u2f((imm8 >> 7) << 31 | e << 23 | (imm8 & 15u) << 19);
}

/* ------------------------------------------------------------------------------ the interpreter --- */
/* a PC written by a load / BX / POP: bit 0 must be set (Thumb); ARM_RET returns to us */
#define BXPC(v)                                                                                                       \
    do {                                                                                                              \
        uint32_t t_ = (v);                                                                                            \
        if (!(t_ & 1u) && t_ != ARM_RET) {                                                                            \
            m->fault = ARM_F_STATE;                                                                                   \
            m->fault_addr = t_;                                                                                       \
        }                                                                                                             \
        npc = t_ & ~1u;                                                                                               \
    } while (0)

#define UNDEF()                                                                                                       \
    do {                                                                                                              \
        m->fault = ARM_F_UNDEF;                                                                                       \
        m->fault_addr = (uint32_t)hw1 << 16 | hw2;                                                                    \
        return 0;                                                                                                     \
    } while (0)

/* the value of register i as an operand: the PC reads as this instruction + 4 */
#define RV(i) ((i) == 15u ? pc + 4u : r[(i)])

/* a register written with a 32-bit data-processing result: PC means a branch (ALUWritePC) */
#define WR(d, val)                                                                                                    \
    do {                                                                                                              \
        uint32_t v_ = (val);                                                                                          \
        if ((d) == 15u)                                                                                               \
            npc = v_ & ~1u;                                                                                           \
        else                                                                                                          \
            r[(d)] = v_;                                                                                              \
    } while (0)

static ARM_CODE uint32_t vfp(arm_t *m, uint32_t pc, uint32_t hw1, uint32_t hw2);
static ARM_CODE uint32_t t32(arm_t *m, uint32_t pc, uint32_t hw1, uint32_t hw2);

/* one 16-bit instruction (already passed its IT condition) -> the next pc */
static ARM_CODE uint32_t t16(arm_t *m, uint32_t pc, uint32_t hw1)
{
    uint32_t *r = m->r, npc = pc + 2u, hw2 = 0;
    int setf = !(m->it & 0xFu);                             /* outside an IT block the 16-bit forms set flags */
    switch (hw1 >> 10) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: {   /* shifts, ADD, SUB */
        uint32_t op = hw1 >> 11 & 3u, rd = hw1 & 7u, rm = hw1 >> 3 & 7u;
        if (op < 3u) {                                      /* LSL / LSR / ASR #imm5 (LSL #0: MOVS) */
            uint32_t c = m->c, v = imm_shift(r[rm], op, hw1 >> 6 & 31u, &c);
            r[rd] = v;
            if (setf) {
                nz(m, v);
                m->c = c;
            }
        } else {
            uint32_t rn = rm, b = (hw1 & 0x400u) ? hw1 >> 6 & 7u : r[hw1 >> 6 & 7u];
            r[rd] = (hw1 & 0x200u) ? addc(m, r[rn], ~b, 1, setf) : addc(m, r[rn], b, 0, setf);
        }
        break;
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        uint32_t rd = hw1 >> 8 & 7u, imm = hw1 & 0xFFu;
        switch (hw1 >> 11 & 3u) {
        case 0: r[rd] = imm; if (setf) nz(m, imm); break;              /* MOVS */
        case 1: addc(m, r[rd], ~imm, 1, 1); break;                     /* CMP */
        case 2: r[rd] = addc(m, r[rd], imm, 0, setf); break;           /* ADDS */
        default: r[rd] = addc(m, r[rd], ~imm, 1, setf); break;         /* SUBS */
        }
        break;
    }
    case 0x10: {                                            /* data processing */
        uint32_t rd = hw1 & 7u, rm = hw1 >> 3 & 7u, a = r[rd], b = r[rm], v, c = m->c;
        switch (hw1 >> 6 & 15u) {
        case 0x0: v = a & b; r[rd] = v; if (setf) nz(m, v); break;                     /* AND */
        case 0x1: v = a ^ b; r[rd] = v; if (setf) nz(m, v); break;                     /* EOR */
        case 0x2: v = shc(a, 0, b & 0xFFu, &c); r[rd] = v; if (setf) { nz(m, v); m->c = c; } break;   /* LSL */
        case 0x3: v = shc(a, 1, b & 0xFFu, &c); r[rd] = v; if (setf) { nz(m, v); m->c = c; } break;   /* LSR */
        case 0x4: v = shc(a, 2, b & 0xFFu, &c); r[rd] = v; if (setf) { nz(m, v); m->c = c; } break;   /* ASR */
        case 0x5: r[rd] = addc(m, a, b, m->c, setf); break;                            /* ADC */
        case 0x6: r[rd] = addc(m, a, ~b, m->c, setf); break;                           /* SBC */
        case 0x7: v = shc(a, 3, b & 0xFFu, &c); r[rd] = v; if (setf) { nz(m, v); m->c = c; } break;   /* ROR */
        case 0x8: nz(m, a & b); break;                                                 /* TST */
        case 0x9: r[rd] = addc(m, ~b, 0, 1, setf); break;                              /* RSB #0 (NEG) */
        case 0xA: addc(m, a, ~b, 1, 1); break;                                         /* CMP */
        case 0xB: addc(m, a, b, 0, 1); break;                                          /* CMN */
        case 0xC: v = a | b; r[rd] = v; if (setf) nz(m, v); break;                     /* ORR */
        case 0xD: v = a * b; r[rd] = v; if (setf) nz(m, v); break;                     /* MUL */
        case 0xE: v = a & ~b; r[rd] = v; if (setf) nz(m, v); break;                    /* BIC */
        default: v = ~b; r[rd] = v; if (setf) nz(m, v); break;                         /* MVN */
        }
        break;
    }
    case 0x11: {                                            /* special data, branch and exchange */
        uint32_t rm = hw1 >> 3 & 15u, rdn = (hw1 >> 4 & 8u) | (hw1 & 7u);
        switch (hw1 >> 8 & 3u) {
        case 0: WR(rdn, RV(rdn) + RV(rm)); break;                                      /* ADD (no flags) */
        case 1: addc(m, RV(rdn), ~RV(rm), 1, 1); break;                                /* CMP */
        case 2: WR(rdn, RV(rm)); break;                                                /* MOV */
        default:                                                                       /* BX / BLX */
            if (hw1 & 0x80u)
                r[14] = (pc + 2u) | 1u;
            BXPC(RV(rm));
            break;
        }
        break;
    }
    case 0x12: case 0x13:                                   /* LDR literal */
        r[hw1 >> 8 & 7u] = ld32(m, ((pc + 4u) & ~3u) + (hw1 & 0xFFu) * 4u);
        break;
    case 0x14: case 0x15: case 0x16: case 0x17: {           /* load / store, register offset */
        uint32_t rt = hw1 & 7u, a = r[hw1 >> 3 & 7u] + r[hw1 >> 6 & 7u];
        switch (hw1 >> 9 & 7u) {
        case 0: st32(m, a, r[rt]); break;
        case 1: st16(m, a, r[rt]); break;
        case 2: st8(m, a, r[rt]); break;
        case 3: r[rt] = (uint32_t)(int32_t)(int8_t)ld8(m, a); break;
        case 4: r[rt] = ld32(m, a); break;
        case 5: r[rt] = ld16(m, a); break;
        case 6: r[rt] = ld8(m, a); break;
        default: r[rt] = (uint32_t)(int32_t)(int16_t)ld16(m, a); break;
        }
        break;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: {   /* STR LDR STRB LDRB #imm */
        uint32_t rt = hw1 & 7u, rn = hw1 >> 3 & 7u, imm = hw1 >> 6 & 31u;
        switch (hw1 >> 11 & 3u) {
        case 0: st32(m, r[rn] + imm * 4u, r[rt]); break;
        case 1: r[rt] = ld32(m, r[rn] + imm * 4u); break;
        case 2: st8(m, r[rn] + imm, r[rt]); break;
        default: r[rt] = ld8(m, r[rn] + imm); break;
        }
        break;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {   /* STRH LDRH #imm; SP-relative */
        if (!(hw1 & 0x1000u)) {
            uint32_t rt = hw1 & 7u, a = r[hw1 >> 3 & 7u] + (hw1 >> 6 & 31u) * 2u;
            if (hw1 & 0x800u)
                r[rt] = ld16(m, a);
            else
                st16(m, a, r[rt]);
        } else {
            uint32_t rt = hw1 >> 8 & 7u, a = r[13] + (hw1 & 0xFFu) * 4u;
            if (hw1 & 0x800u)
                r[rt] = ld32(m, a);
            else
                st32(m, a, r[rt]);
        }
        break;
    }
    case 0x28: case 0x29: case 0x2A: case 0x2B:             /* ADR; ADD Rd, SP, #imm */
        r[hw1 >> 8 & 7u] = ((hw1 & 0x800u) ? r[13] : (pc + 4u) & ~3u) + (hw1 & 0xFFu) * 4u;
        break;
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:             /* miscellaneous */
        switch (hw1 >> 8 & 15u) {
        case 0x0:                                           /* ADD / SUB SP, SP, #imm7 * 4 */
            r[13] += (hw1 & 0x80u) ? 0u - (hw1 & 0x7Fu) * 4u : (hw1 & 0x7Fu) * 4u;
            break;
        case 0x1: case 0x3: case 0x9: case 0xB: {           /* CBZ / CBNZ */
            uint32_t zero = r[hw1 & 7u] == 0u;
            if (zero != ((hw1 >> 11) & 1u))
                npc = pc + 4u + ((hw1 >> 3 & 0x1Fu) << 1 | (hw1 >> 3 & 0x40u));
            break;
        }
        case 0x2: {                                         /* SXTH SXTB UXTH UXTB */
            uint32_t v = r[hw1 >> 3 & 7u], d = hw1 & 7u;
            switch (hw1 >> 6 & 3u) {
            case 0: r[d] = (uint32_t)(int32_t)(int16_t)v; break;
            case 1: r[d] = (uint32_t)(int32_t)(int8_t)v; break;
            case 2: r[d] = v & 0xFFFFu; break;
            default: r[d] = v & 0xFFu; break;
            }
            break;
        }
        case 0x4: case 0x5: {                               /* PUSH */
            uint32_t list = (hw1 & 0xFFu) | (hw1 & 0x100u ? 0x4000u : 0u), a, i, k = 0;
            for (i = 0; i < 16u; i++)
                k += list >> i & 1u;
            a = r[13] - 4u * k;
            for (i = 0; i < 16u; i++)
                if (list >> i & 1u) {
                    st32(m, a, r[i]);
                    a += 4u;
                }
            if (!m->fault)
                r[13] -= 4u * k;
            break;
        }
        case 0xC: case 0xD: {                               /* POP */
            uint32_t list = hw1 & 0xFFu, a = r[13], i;
            for (i = 0; i < 8u; i++)
                if (list >> i & 1u) {
                    r[i] = ld32(m, a);
                    a += 4u;
                }
            if (hw1 & 0x100u) {
                uint32_t v = ld32(m, a);
                a += 4u;
                r[13] = a;
                BXPC(v);
            } else {
                r[13] = a;
            }
            break;
        }
        case 0x6:                                           /* CPS: no interrupts here */
            break;
        case 0xA: {                                         /* REV REV16 (REVSH) */
            uint32_t v = r[hw1 >> 3 & 7u], d = hw1 & 7u;
            switch (hw1 >> 6 & 3u) {
            case 0: r[d] = v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; break;
            case 1: r[d] = (v >> 8 & 0x00FF00FFu) | (v << 8 & 0xFF00FF00u); break;
            case 3: r[d] = (uint32_t)(int32_t)(int16_t)((v >> 8 & 0xFFu) | (v & 0xFFu) << 8); break;
            default: UNDEF();
            }
            break;
        }
        case 0xE:                                           /* BKPT */
            m->fault = ARM_F_STATE;
            m->fault_addr = pc;
            break;
        case 0xF:                                           /* IT, or a hint (NOP, YIELD, WFE, WFI, SEV) */
            if (hw1 & 0xFu)
                m->it = (hw1 & 0xFFu) | 0x100u;               /* (0x100: set by this instruction, not advanced) */
            break;
        default: UNDEF();
        }
        break;
    case 0x30: case 0x31: case 0x32: case 0x33: {           /* STM / LDM (IA, writeback) */
        uint32_t rn = hw1 >> 8 & 7u, list = hw1 & 0xFFu, a = r[rn], i;
        if (!(hw1 & 0x800u)) {
            for (i = 0; i < 8u; i++)
                if (list >> i & 1u) {
                    st32(m, a, r[i]);
                    a += 4u;
                }
            r[rn] = a;
        } else {
            uint32_t wb = !(list >> rn & 1u);
            for (i = 0; i < 8u; i++)
                if (list >> i & 1u) {
                    r[i] = ld32(m, a);
                    a += 4u;
                }
            if (wb)
                r[rn] = a;
        }
        break;
    }
    case 0x34: case 0x35: case 0x36: case 0x37: {           /* B<c>; UDF; SVC */
        uint32_t cond = hw1 >> 8 & 15u;
        if (cond >= 14u) {
            m->fault = ARM_F_STATE;
            m->fault_addr = pc;
            break;
        }
        if (cond_ok(m, cond))
            npc = pc + 4u + (uint32_t)((int32_t)(int8_t)(hw1 & 0xFFu) * 2);
        break;
    }
    case 0x38: case 0x39:                                   /* B */
        npc = pc + 4u + (uint32_t)(((int32_t)(hw1 << 21)) >> 20);
        break;
    default:
        UNDEF();
    }
    return npc;
}

/* LDM / STM, IA or DB, any registers (32-bit forms) */
static ARM_CODE uint32_t ldstm(arm_t *m, uint32_t pc, uint32_t rn, uint32_t list, int load, int db, int wb)
{
    uint32_t *r = m->r, npc = pc + 4u, k = 0, i, a, start;
    for (i = 0; i < 16u; i++)
        k += list >> i & 1u;
    start = db ? r[rn] - 4u * k : r[rn];
    a = start;
    if (load) {
        uint32_t newpc = 0, gotpc = 0;
        for (i = 0; i < 16u; i++)
            if (list >> i & 1u) {
                uint32_t v = ld32(m, a);
                a += 4u;
                if (i == 15u) {
                    newpc = v;
                    gotpc = 1;
                } else {
                    r[i] = v;
                }
            }
        if (wb && !(list >> rn & 1u))
            r[rn] = db ? start : r[rn] + 4u * k;
        if (gotpc)
            BXPC(newpc);
    } else {
        for (i = 0; i < 16u; i++)
            if (list >> i & 1u) {
                st32(m, a, r[i]);
                a += 4u;
            }
        if (wb)
            r[rn] = db ? start : r[rn] + 4u * k;
    }
    return npc;
}

/* the signed / unsigned parallel add and subtract (SADD16 ... UHSUB8) */
static ARM_CODE uint32_t parallel(arm_t *m, uint32_t op1, uint32_t kind, uint32_t a, uint32_t b)
{
    /* op1: 1 ADD16, 2 ASX, 6 SAX, 5 SUB16, 0 ADD8, 4 SUB8; kind: 0 S, 1 Q, 2 SH, 4 U, 5 UQ, 6 UH */
    uint32_t res = 0, ge = 0, i, uns = kind >> 2, mode = kind & 3u;
    if (op1 == 0u || op1 == 4u) {                           /* bytes */
        for (i = 0; i < 4u; i++) {
            int32_t x = uns ? (int32_t)(a >> (8u * i) & 0xFFu) : (int32_t)(int8_t)(a >> (8u * i));
            int32_t y = uns ? (int32_t)(b >> (8u * i) & 0xFFu) : (int32_t)(int8_t)(b >> (8u * i));
            int32_t s = op1 == 0u ? x + y : x - y, o;
            if (mode == 0u) {
                o = s;
                if (uns ? (op1 == 0u ? s >= 0x100 : s >= 0) : s >= 0)
                    ge |= 1u << i;
            } else if (mode == 1u) {
                uint32_t q = 0;
                o = uns ? (int32_t)usat(s, 8, &q) : ssat(s, 8, &q);
            } else {
                o = s >> 1;
            }
            res |= ((uint32_t)o & 0xFFu) << (8u * i);
        }
    } else {                                                /* halfwords */
        int32_t xl = uns ? (int32_t)(a & 0xFFFFu) : (int32_t)(int16_t)a;
        int32_t xh = uns ? (int32_t)(a >> 16) : (int32_t)(int16_t)(a >> 16);
        int32_t yl = uns ? (int32_t)(b & 0xFFFFu) : (int32_t)(int16_t)b;
        int32_t yh = uns ? (int32_t)(b >> 16) : (int32_t)(int16_t)(b >> 16);
        int32_t lo, hi, ol, oh;
        int add_lo, add_hi;
        switch (op1) {
        case 1: lo = xl + yl; hi = xh + yh; add_lo = add_hi = 1; break;        /* ADD16 */
        case 2: lo = xl - yh; hi = xh + yl; add_lo = 0; add_hi = 1; break;     /* ASX */
        case 6: lo = xl + yh; hi = xh - yl; add_lo = 1; add_hi = 0; break;     /* SAX */
        default: lo = xl - yl; hi = xh - yh; add_lo = add_hi = 0; break;       /* SUB16 */
        }
        if (mode == 0u) {
            ol = lo;
            oh = hi;
            if (uns ? (add_lo ? lo >= 65536 : lo >= 0) : lo >= 0)
                ge |= 3u;
            if (uns ? (add_hi ? hi >= 65536 : hi >= 0) : hi >= 0)
                ge |= 12u;
        } else if (mode == 1u) {
            uint32_t q = 0;
            ol = uns ? (int32_t)usat(lo, 16, &q) : ssat(lo, 16, &q);
            oh = uns ? (int32_t)usat(hi, 16, &q) : ssat(hi, 16, &q);
        } else {
            ol = lo >> 1;
            oh = hi >> 1;
        }
        res = ((uint32_t)ol & 0xFFFFu) | ((uint32_t)oh & 0xFFFFu) << 16;
    }
    if (mode == 0u)
        m->ge = ge;
    return res;
}

INL int32_t q_add(arm_t *m, int32_t a, int32_t b)
{
    uint32_t q = 0;
    int32_t r = ssat((int64_t)a + b, 32, &q);
    m->q |= q;
    return r;
}

/* one 32-bit instruction -> the next pc */
static ARM_CODE uint32_t t32(arm_t *m, uint32_t pc, uint32_t hw1, uint32_t hw2)
{
    uint32_t *r = m->r, npc = pc + 4u;
    uint32_t op1 = hw1 >> 11 & 3u;
    if (op1 == 1u) {
        if (hw1 & 0x400u)                                   /* coprocessor: the FPU */
            return vfp(m, pc, hw1, hw2);
        if (!(hw1 & 0x200u)) {
            if (!(hw1 & 0x40u)) {                           /* load / store multiple */
                uint32_t op = hw1 >> 7 & 3u;
                if (op != 1u && op != 2u)
                    UNDEF();
                return ldstm(m, pc, hw1 & 15u, hw2, hw1 >> 4 & 1u, op == 2u, hw1 >> 5 & 1u);
            }
            /* load / store dual or exclusive, table branch */
            {
                uint32_t o1 = hw1 >> 7 & 3u, o2 = hw1 >> 4 & 3u, rn = hw1 & 15u, rt = hw2 >> 12;
                if (o1 == 0u && o2 == 0u) {                 /* STREX: always succeeds */
                    st32(m, r[rn] + (hw2 & 0xFFu) * 4u, r[rt]);
                    r[hw2 >> 8 & 15u] = 0;
                } else if (o1 == 0u && o2 == 1u) {          /* LDREX */
                    r[rt] = ld32(m, r[rn] + (hw2 & 0xFFu) * 4u);
                } else if (o1 == 1u && o2 == 0u) {          /* STREXB / STREXH */
                    uint32_t op3 = hw2 >> 4 & 15u;
                    if (op3 == 4u)
                        st8(m, r[rn], r[rt]);
                    else if (op3 == 5u)
                        st16(m, r[rn], r[rt]);
                    else
                        UNDEF();
                    r[hw2 & 15u] = 0;
                } else if (o1 == 1u && o2 == 1u) {          /* TBB TBH LDREXB LDREXH */
                    uint32_t op3 = hw2 >> 4 & 15u, base = RV(rn);
                    if (op3 == 0u)
                        npc = pc + 4u + 2u * ld8(m, base + r[hw2 & 15u]);
                    else if (op3 == 1u)
                        npc = pc + 4u + 2u * ld16(m, base + 2u * r[hw2 & 15u]);
                    else if (op3 == 4u)
                        r[rt] = ld8(m, r[rn]);
                    else if (op3 == 5u)
                        r[rt] = ld16(m, r[rn]);
                    else
                        UNDEF();
                } else {                                    /* LDRD / STRD (immediate, literal) */
                    uint32_t p = hw1 >> 8 & 1u, u = hw1 >> 7 & 1u, w = hw1 >> 5 & 1u, l = hw1 >> 4 & 1u;
                    uint32_t rt2 = hw2 >> 8 & 15u, off = (hw2 & 0xFFu) * 4u;
                    uint32_t base = rn == 15u ? (pc + 4u) & ~3u : r[rn];
                    uint32_t na = u ? base + off : base - off, a = p ? na : base;
                    if (l) {
                        uint32_t v1 = ld32(m, a), v2 = ld32(m, a + 4u);
                        r[rt] = v1;
                        r[rt2] = v2;
                    } else {
                        st32(m, a, r[rt]);
                        st32(m, a + 4u, r[rt2]);
                    }
                    if (w)
                        r[rn] = na;
                }
            }
            return npc;
        }
        if ((hw1 & 0xFE00u) == 0xEA00u) {                   /* data processing (shifted register) */
            uint32_t op = hw1 >> 5 & 15u, s = hw1 >> 4 & 1u, rn = hw1 & 15u, rd = hw2 >> 8 & 15u, rm = hw2 & 15u;
            uint32_t type = hw2 >> 4 & 3u, imm5 = (hw2 >> 10 & 0x1Cu) | (hw2 >> 6 & 3u), c = m->c, b, a, v;
            if (op == 6u) {                                 /* PKHBT / PKHTB */
                uint32_t cc = 0;
                b = imm_shift(r[rm], type & 2u, imm5, &cc);
                a = r[rn];
                r[rd] = (type & 2u) ? (a & 0xFFFF0000u) | (b & 0xFFFFu) : (b & 0xFFFF0000u) | (a & 0xFFFFu);
                return npc;
            }
            b = imm_shift(RV(rm), type, imm5, &c);
            a = rn == 15u ? 0u : r[rn];
            switch (op) {
            case 0x0: v = a & b; if (rd != 15u) r[rd] = v; if (s) { nz(m, v); m->c = c; } break;   /* AND / TST */
            case 0x1: v = a & ~b; r[rd] = v; if (s) { nz(m, v); m->c = c; } break;                 /* BIC */
            case 0x2: v = (rn == 15u ? 0u : a) | b; WR(rd, v); if (s) { nz(m, v); m->c = c; } break;   /* ORR / MOV */
            case 0x3: v = rn == 15u ? ~b : a | ~b; r[rd] = v; if (s) { nz(m, v); m->c = c; } break; /* ORN / MVN */
            case 0x4: v = a ^ b; if (rd != 15u) r[rd] = v; if (s) { nz(m, v); m->c = c; } break;   /* EOR / TEQ */
            case 0x8: v = addc(m, RV(rn), b, 0, (int)s); if (rd != 15u) r[rd] = v; break;          /* ADD / CMN */
            case 0xA: r[rd] = addc(m, a, b, m->c, (int)s); break;                                   /* ADC */
            case 0xB: r[rd] = addc(m, a, ~b, m->c, (int)s); break;                                  /* SBC */
            case 0xD: v = addc(m, RV(rn), ~b, 1, (int)s); if (rd != 15u) r[rd] = v; break;         /* SUB / CMP */
            case 0xE: r[rd] = addc(m, ~a, b, 1, (int)s); break;                                     /* RSB */
            default: UNDEF();
            }
            return npc;
        }
        UNDEF();
    }
    if (op1 == 2u) {
        if (!(hw2 & 0x8000u)) {
            uint32_t rn = hw1 & 15u, rd = hw2 >> 8 & 15u;
            if (!(hw1 & 0x200u)) {                          /* data processing (modified immediate) */
                uint32_t op = hw1 >> 5 & 15u, s = hw1 >> 4 & 1u, c = m->c, a = rn == 15u ? 0u : r[rn], v;
                uint32_t imm = expand_imm((hw1 >> 10 & 1u) << 11 | (hw2 >> 4 & 0x700u) | (hw2 & 0xFFu), &c);
                switch (op) {
                case 0x0: v = a & imm; if (rd != 15u) r[rd] = v; if (s) { nz(m, v); m->c = c; } break;
                case 0x1: v = a & ~imm; r[rd] = v; if (s) { nz(m, v); m->c = c; } break;
                case 0x2: v = a | imm; r[rd] = v; if (s) { nz(m, v); m->c = c; } break;
                case 0x3: v = rn == 15u ? ~imm : a | ~imm; r[rd] = v; if (s) { nz(m, v); m->c = c; } break;
                case 0x4: v = a ^ imm; if (rd != 15u) r[rd] = v; if (s) { nz(m, v); m->c = c; } break;
                case 0x8: v = addc(m, RV(rn), imm, 0, (int)s); if (rd != 15u) r[rd] = v; break;
                case 0xA: r[rd] = addc(m, a, imm, m->c, (int)s); break;
                case 0xB: r[rd] = addc(m, a, ~imm, m->c, (int)s); break;
                case 0xD: v = addc(m, RV(rn), ~imm, 1, (int)s); if (rd != 15u) r[rd] = v; break;
                case 0xE: r[rd] = addc(m, ~a, imm, 1, (int)s); break;
                default: UNDEF();
                }
                return npc;
            }
            {                                               /* data processing (plain binary immediate) */
                uint32_t op = hw1 >> 4 & 0x1Fu, imm12 = (hw1 >> 10 & 1u) << 11 | (hw2 >> 4 & 0x700u) | (hw2 & 0xFFu);
                uint32_t imm5 = (hw2 >> 10 & 0x1Cu) | (hw2 >> 6 & 3u), sat = hw2 & 0x1Fu, q = 0;
                switch (op) {
                case 0x00: r[rd] = (rn == 15u ? (pc + 4u) & ~3u : r[rn]) + imm12; break;   /* ADDW / ADR */
                case 0x0A: r[rd] = (rn == 15u ? (pc + 4u) & ~3u : r[rn]) - imm12; break;   /* SUBW / ADR */
                case 0x04: r[rd] = imm12 | (hw1 & 15u) << 12; break;                       /* MOVW */
                case 0x0C: r[rd] = (r[rd] & 0xFFFFu) | (imm12 | (hw1 & 15u) << 12) << 16; break;   /* MOVT */
                case 0x10: case 0x12: {                     /* SSAT (SSAT16) */
                    if (op == 0x12 && imm5 == 0u) {
                        int32_t lo = ssat((int16_t)r[rn], sat + 1u, &q), hi = ssat((int16_t)(r[rn] >> 16), sat + 1u, &q);
                        r[rd] = ((uint32_t)lo & 0xFFFFu) | (uint32_t)hi << 16;
                    } else {
                        uint32_t cc = 0, v = imm_shift(r[rn], (hw1 >> 4 & 2u), imm5, &cc);
                        r[rd] = (uint32_t)ssat((int32_t)v, sat + 1u, &q);
                    }
                    m->q |= q;
                    break;
                }
                case 0x18: case 0x1A: {                     /* USAT (USAT16) */
                    if (op == 0x1A && imm5 == 0u) {
                        uint32_t lo = usat((int16_t)r[rn], sat, &q), hi = usat((int16_t)(r[rn] >> 16), sat, &q);
                        r[rd] = lo | hi << 16;
                    } else {
                        uint32_t cc = 0, v = imm_shift(r[rn], (hw1 >> 4 & 2u), imm5, &cc);
                        r[rd] = usat((int32_t)v, sat, &q);
                    }
                    m->q |= q;
                    break;
                }
                case 0x14: case 0x1C: {                     /* SBFX / UBFX */
                    uint32_t w = sat + 1u, v = r[rn] >> imm5;
                    if (imm5 + w > 32u)
                        UNDEF();
                    v = w == 32u ? v : v & ((1u << w) - 1u);
                    if (op == 0x14 && w < 32u && (v >> (w - 1u) & 1u))
                        v |= ~((1u << w) - 1u);
                    r[rd] = v;
                    break;
                }
                case 0x16: {                                /* BFI / BFC */
                    uint32_t msb = sat, lsb = imm5, mask;
                    if (msb < lsb)
                        UNDEF();
                    mask = (msb - lsb == 31u ? 0xFFFFFFFFu : ((1u << (msb - lsb + 1u)) - 1u)) << lsb;
                    r[rd] = (r[rd] & ~mask) | ((rn == 15u ? 0u : r[rn] << lsb) & mask);
                    break;
                }
                default: UNDEF();
                }
                return npc;
            }
        }
        /* branches and miscellaneous control */
        {
            uint32_t o = hw2 >> 12 & 5u;
            if (o == 0u) {
                if ((hw1 >> 7 & 7u) != 7u) {                /* B<c> T3 */
                    if (cond_ok(m, hw1 >> 6 & 15u)) {
                        uint32_t s = hw1 >> 10 & 1u, j1 = hw2 >> 13 & 1u, j2 = hw2 >> 11 & 1u;
                        uint32_t imm = s << 20 | j2 << 19 | j1 << 18 | (hw1 & 0x3Fu) << 12 | (hw2 & 0x7FFu) << 1;
                        npc = pc + 4u + (uint32_t)(((int32_t)(imm << 11)) >> 11);
                    }
                    return npc;
                }
                if ((hw1 & 0xFFE0u) == 0xF380u) {           /* MSR */
                    uint32_t sysm = hw2 & 0xFFu, v = r[hw1 & 15u];
                    if (sysm < 4u) {
                        if (hw2 & 0x800u) {
                            m->n = v >> 31;
                            m->z = v >> 30 & 1u;
                            m->c = v >> 29 & 1u;
                            m->v = v >> 28 & 1u;
                            m->q = v >> 27 & 1u;
                        }
                        if (hw2 & 0x400u)
                            m->ge = v >> 16 & 15u;
                    }
                    return npc;
                }
                if ((hw1 & 0xFFE0u) == 0xF3E0u) {           /* MRS */
                    uint32_t sysm = hw2 & 0xFFu;
                    r[hw2 >> 8 & 15u] = sysm < 4u ? apsr(m) : sysm == 8u || sysm == 9u ? r[13] : 0u;
                    return npc;
                }
                if (hw1 == 0xF3AFu || hw1 == 0xF3BFu)       /* hints, barriers, CLREX */
                    return npc;
                UNDEF();
            }
            {
                uint32_t s = hw1 >> 10 & 1u, j1 = hw2 >> 13 & 1u, j2 = hw2 >> 11 & 1u;
                uint32_t i1 = (j1 ^ s) ^ 1u, i2 = (j2 ^ s) ^ 1u;
                uint32_t imm = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FFu) << 12 | (hw2 & 0x7FFu) << 1;
                int32_t off = ((int32_t)(imm << 7)) >> 7;
                if (o == 1u) {                              /* B T4 */
                    npc = pc + 4u + (uint32_t)off;
                } else if (o == 5u) {                       /* BL */
                    r[14] = (pc + 4u) | 1u;
                    npc = pc + 4u + (uint32_t)off;
                } else {                                    /* BLX imm: to ARM state */
                    m->fault = ARM_F_STATE;
                    m->fault_addr = pc;
                }
            }
            return npc;
        }
    }
    /* op1 == 3 */
    if ((hw1 & 0xFE00u) == 0xF800u) {   /* load / store single */
        uint32_t sgn = hw1 >> 8 & 1u, size = hw1 >> 5 & 3u, l = hw1 >> 4 & 1u, rn = hw1 & 15u, rt = hw2 >> 12;
        uint32_t a, wbv = 0, wb = 0, v = 0;
        if (size == 3u || (sgn && (!l || size == 2u)))
            UNDEF();
        if (rn == 15u) {                                    /* literal */
            uint32_t base = (pc + 4u) & ~3u, imm = hw2 & 0xFFFu;
            if (!l)
                UNDEF();
            a = (hw1 & 0x80u) ? base + imm : base - imm;
        } else if (hw1 & 0x80u) {                           /* imm12 */
            a = r[rn] + (hw2 & 0xFFFu);
        } else if (hw2 & 0x800u) {                          /* imm8, P U W */
            uint32_t p = hw2 >> 10 & 1u, u = hw2 >> 9 & 1u, imm = hw2 & 0xFFu;
            uint32_t na = u ? r[rn] + imm : r[rn] - imm;
            a = p ? na : r[rn];
            wb = hw2 >> 8 & 1u;
            wbv = na;
        } else {                                            /* register, LSL #imm2 */
            if (hw2 & 0x7C0u)
                UNDEF();
            a = r[rn] + (r[hw2 & 15u] << (hw2 >> 4 & 3u));
        }
        if (l) {
            if (rt == 15u && size != 2u)                    /* PLD / PLI */
                return npc;
            switch (size) {
            case 0: v = ld8(m, a); if (sgn) v = (uint32_t)(int32_t)(int8_t)v; break;
            case 1: v = ld16(m, a); if (sgn) v = (uint32_t)(int32_t)(int16_t)v; break;
            default: v = ld32(m, a); break;
            }
            if (wb)
                r[rn] = wbv;
            if (rt == 15u)
                BXPC(v);
            else
                r[rt] = v;
        } else {
            switch (size) {
            case 0: st8(m, a, r[rt]); break;
            case 1: st16(m, a, r[rt]); break;
            default: st32(m, a, r[rt]); break;
            }
            if (wb)
                r[rn] = wbv;
        }
        return npc;
    }
    if ((hw1 & 0xFF00u) == 0xFA00u) {                       /* data processing (register) */
        uint32_t rn = hw1 & 15u, rd = hw2 >> 8 & 15u, rm = hw2 & 15u;
        if ((hw2 & 0xF000u) != 0xF000u)
            UNDEF();
        if (!(hw1 & 0x80u)) {
            if (!(hw2 & 0x80u)) {                           /* LSL LSR ASR ROR (register) */
                uint32_t c = m->c, v = shc(r[rn], hw1 >> 5 & 3u, r[rm] & 0xFFu, &c);
                r[rd] = v;
                if (hw1 & 0x10u) {
                    nz(m, v);
                    m->c = c;
                }
            } else {                                        /* (S|U)XTA(H|B|B16) */
                uint32_t v = ror32(r[rm], 8u * (hw2 >> 4 & 3u)), add = rn == 15u ? 0u : r[rn], res;
                switch (hw1 >> 4 & 7u) {
                case 0: res = add + (uint32_t)(int32_t)(int16_t)v; break;
                case 1: res = add + (v & 0xFFFFu); break;
                case 2: res = ((add + (uint32_t)(int32_t)(int8_t)v) & 0xFFFFu) |
                              ((add >> 16) + (uint32_t)(int32_t)(int8_t)(v >> 16)) << 16; break;
                case 3: res = ((add + (v & 0xFFu)) & 0xFFFFu) | ((add >> 16) + (v >> 16 & 0xFFu)) << 16; break;
                case 4: res = add + (uint32_t)(int32_t)(int8_t)v; break;
                case 5: res = add + (v & 0xFFu); break;
                default: UNDEF();
                }
                r[rd] = res;
            }
            return npc;
        }
        if (!(hw2 & 0x80u)) {                               /* parallel add / subtract */
            uint32_t op = hw1 >> 4 & 7u, kind = (hw2 >> 4 & 3u) | (hw2 >> 4 & 4u);
            if ((kind & 3u) == 3u || op == 3u || op == 7u)
                UNDEF();
            r[rd] = parallel(m, op, kind, r[rn], r[rm]);
            return npc;
        }
        {                                                   /* miscellaneous */
            uint32_t o1 = hw1 >> 4 & 3u, o2 = hw2 >> 4 & 3u, v = r[rm];
            if ((hw2 & 0xC0u) != 0x80u)
                UNDEF();
            switch (o1 << 2 | o2) {
            case 0x0: r[rd] = (uint32_t)q_add(m, (int32_t)r[rm], (int32_t)r[rn]); break;   /* QADD */
            case 0x1: r[rd] = (uint32_t)q_add(m, (int32_t)r[rm], q_add(m, (int32_t)r[rn], (int32_t)r[rn])); break;
            case 0x2: {                                                                    /* QSUB */
                uint32_t q = 0;
                r[rd] = (uint32_t)ssat((int64_t)(int32_t)r[rm] - (int32_t)r[rn], 32, &q);
                m->q |= q;
                break;
            }
            case 0x3: {                                                                    /* QDSUB */
                uint32_t q = 0;
                int32_t d = q_add(m, (int32_t)r[rn], (int32_t)r[rn]);
                r[rd] = (uint32_t)ssat((int64_t)(int32_t)r[rm] - d, 32, &q);
                m->q |= q;
                break;
            }
            case 0x4: r[rd] = v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; break;   /* REV */
            case 0x5: r[rd] = (v >> 8 & 0x00FF00FFu) | (v << 8 & 0xFF00FF00u); break;                 /* REV16 */
            case 0x6: {                                                                    /* RBIT */
                uint32_t o = 0, i;
                for (i = 0; i < 32u; i++)
                    o |= (v >> i & 1u) << (31u - i);
                r[rd] = o;
                break;
            }
            case 0x7: r[rd] = (uint32_t)(int32_t)(int16_t)((v >> 8 & 0xFFu) | (v & 0xFFu) << 8); break;  /* REVSH */
            case 0x8: {                                                                    /* SEL */
                uint32_t a = r[rn], o = 0, i;
                for (i = 0; i < 4u; i++)
                    o |= ((m->ge >> i & 1u) ? a : v) & (0xFFu << (8u * i));
                r[rd] = o;
                break;
            }
            case 0xC: {                                                                    /* CLZ */
                uint32_t n = 0;
                if (!v)
                    n = 32;
                else
                    while (!(v & 0x80000000u)) {
                        v <<= 1;
                        n++;
                    }
                r[rd] = n;
                break;
            }
            default: UNDEF();
            }
            return npc;
        }
    }
    if ((hw1 & 0xFF80u) == 0xFB00u) {                       /* multiply, multiply accumulate */
        uint32_t o1 = hw1 >> 4 & 7u, o2 = hw2 >> 4 & 3u, rn = hw1 & 15u, ra = hw2 >> 12, rd = hw2 >> 8 & 15u;
        uint32_t rm = hw2 & 15u, a = r[rn], b = r[rm], acc = ra == 15u ? 0u : r[ra];
        switch (o1) {
        case 0:
            if (o2 == 0u)
                r[rd] = a * b + acc;                        /* MUL / MLA */
            else if (o2 == 1u)
                r[rd] = r[ra] - a * b;                      /* MLS */
            else
                UNDEF();
            break;
        case 1: {                                           /* SMLA<x><y> / SMUL<x><y> */
            int32_t x = (int16_t)((hw2 & 0x20u) ? a >> 16 : a), y = (int16_t)((hw2 & 0x10u) ? b >> 16 : b);
            int32_t p = x * y;
            if (ra == 15u) {
                r[rd] = (uint32_t)p;
            } else {
                int64_t s = (int64_t)p + (int32_t)acc;
                if (s != (int32_t)s)
                    m->q = 1;
                r[rd] = (uint32_t)(int32_t)s;
            }
            break;
        }
        case 2: case 4: {                                   /* SMLAD / SMUAD, SMLSD / SMUSD */
            uint32_t bb = (hw2 & 0x10u) ? ror32(b, 16) : b;
            int32_t p1 = (int16_t)a * (int16_t)bb, p2 = (int16_t)(a >> 16) * (int16_t)(bb >> 16);
            int64_t s = o1 == 2u ? (int64_t)p1 + p2 : (int64_t)p1 - p2;
            if (ra != 15u)
                s += (int32_t)acc;
            if (s != (int32_t)s)
                m->q = 1;
            r[rd] = (uint32_t)(int32_t)s;
            break;
        }
        case 3: {                                           /* SMLAW<y> / SMULW<y> */
            int64_t p = (int64_t)(int32_t)a * (int16_t)((hw2 & 0x10u) ? b >> 16 : b);
            int32_t hi = (int32_t)(p >> 16);
            if (ra == 15u) {
                r[rd] = (uint32_t)hi;
            } else {
                int64_t s = (int64_t)hi + (int32_t)acc;
                if (s != (int32_t)s)
                    m->q = 1;
                r[rd] = (uint32_t)(int32_t)s;
            }
            break;
        }
        case 5: case 6: {                                   /* SMMLA / SMMUL, SMMLS (R: rounded) */
            uint64_t p = (uint64_t)((int64_t)(int32_t)a * (int32_t)b), s;   /* (modulo 2^64, as the M4) */
            if (o1 == 5u)
                s = (ra == 15u ? 0u : (uint64_t)acc << 32) + p;
            else
                s = ((uint64_t)acc << 32) - p;
            if (hw2 & 0x10u)
                s += 0x80000000u;
            r[rd] = (uint32_t)(s >> 32);
            break;
        }
        default: {                                          /* USAD8 / USADA8 */
            uint32_t sum = 0, i;
            for (i = 0; i < 4u; i++) {
                int32_t d = (int32_t)(a >> (8u * i) & 0xFFu) - (int32_t)(b >> (8u * i) & 0xFFu);
                sum += (uint32_t)(d < 0 ? -d : d);
            }
            r[rd] = sum + acc;
            break;
        }
        }
        return npc;
    }
    if ((hw1 & 0xFF80u) == 0xFB80u) {                       /* long multiply, divide */
        uint32_t o1 = hw1 >> 4 & 7u, o2 = hw2 >> 4 & 15u, rn = hw1 & 15u, lo = hw2 >> 12, hi = hw2 >> 8 & 15u;
        uint32_t a = r[rn], b = r[hw2 & 15u];
        switch (o1) {
        case 0: {                                           /* SMULL */
            int64_t p = (int64_t)(int32_t)a * (int32_t)b;
            r[lo] = (uint32_t)p;
            r[hi] = (uint32_t)((uint64_t)p >> 32);
            break;
        }
        case 1:                                             /* SDIV */
            r[hi] = b == 0u ? 0u : (a == 0x80000000u && b == 0xFFFFFFFFu) ? a : (uint32_t)((int32_t)a / (int32_t)b);
            break;
        case 2: {                                           /* UMULL */
            uint64_t p = (uint64_t)a * b;
            r[lo] = (uint32_t)p;
            r[hi] = (uint32_t)(p >> 32);
            break;
        }
        case 3:                                             /* UDIV */
            r[hi] = b == 0u ? 0u : a / b;
            break;
        case 4: {                                           /* SMLAL, SMLAL<x><y>, SMLALD */
            uint64_t acc = (uint64_t)r[hi] << 32 | r[lo];
            int64_t p;
            if (o2 == 0u)
                p = (int64_t)(int32_t)a * (int32_t)b;
            else if ((o2 & 0xCu) == 8u)
                p = (int64_t)((int16_t)((o2 & 2u) ? a >> 16 : a)) * (int16_t)((o2 & 1u) ? b >> 16 : b);
            else if ((o2 & 0xEu) == 0xCu) {
                uint32_t bb = (o2 & 1u) ? ror32(b, 16) : b;
                p = (int64_t)((int16_t)a * (int16_t)bb) + (int64_t)((int16_t)(a >> 16) * (int16_t)(bb >> 16));
            } else
                UNDEF();
            acc += (uint64_t)p;                             /* (modulo 2^64) */
            r[lo] = (uint32_t)acc;
            r[hi] = (uint32_t)(acc >> 32);
            break;
        }
        case 5: {                                           /* SMLSLD */
            uint64_t acc = (uint64_t)r[hi] << 32 | r[lo];
            uint32_t bb = (o2 & 1u) ? ror32(b, 16) : b;
            if ((o2 & 0xEu) != 0xCu)
                UNDEF();
            acc += (uint64_t)((int64_t)((int16_t)a * (int16_t)bb) - (int64_t)((int16_t)(a >> 16) * (int16_t)(bb >> 16)));
            r[lo] = (uint32_t)acc;
            r[hi] = (uint32_t)(acc >> 32);
            break;
        }
        case 6: {                                           /* UMLAL / UMAAL */
            uint64_t p = (uint64_t)a * b;
            if (o2 == 0u)
                p += (uint64_t)r[hi] << 32 | r[lo];
            else if (o2 == 6u)
                p += (uint64_t)r[hi] + r[lo];
            else
                UNDEF();
            r[lo] = (uint32_t)p;
            r[hi] = (uint32_t)(p >> 32);
            break;
        }
        default: UNDEF();
        }
        return npc;
    }
    if ((hw1 & 0xEC00u) == 0xEC00u)
        return vfp(m, pc, hw1, hw2);
    UNDEF();
}

/* the FPU: VFPv4 single precision (coprocessors 10 and 11) */
static ARM_CODE uint32_t vfp(arm_t *m, uint32_t pc, uint32_t hw1, uint32_t hw2)
{
    uint32_t *r = m->r, npc = pc + 4u, cp = hw2 >> 8 & 15u;
    float *S = m->s.f;
    uint32_t *U = m->s.u;
    if ((cp != 10u && cp != 11u) || (hw1 & 0x1000u))   /* (0xFExx: ARMv8's VSEL, VMAXNM ...: not on an M4) */
        UNDEF();
    if ((hw1 & 0xFE00u) == 0xEC00u && (hw1 & 0x1A0u) != 0u) {   /* extension register load / store */
        uint32_t p = hw1 >> 8 & 1u, u = hw1 >> 7 & 1u, d = hw1 >> 6 & 1u, w = hw1 >> 5 & 1u, l = hw1 >> 4 & 1u;
        uint32_t rn = hw1 & 15u, vd = hw2 >> 12 & 15u, imm = (hw2 & 0xFFu) * 4u, first, n, i, a;
        uint32_t base = rn == 15u ? (pc + 4u) & ~3u : r[rn];
        if (cp == 11u) {                                    /* D registers: two S each */
            first = (d << 4 | vd) * 2u;
            n = (hw2 & 0xFEu);                              /* (FLDMX: the odd word left out) */
        } else {
            first = vd << 1 | d;
            n = hw2 & 0xFFu;
        }
        if (p == 1u && w == 0u) {                           /* VLDR / VSTR */
            a = u ? base + imm : base - imm;
            n = cp == 11u ? 2u : 1u;
        } else {                                            /* VLDM / VSTM (VPUSH / VPOP) */
            if (p == u)
                UNDEF();
            a = u ? base : base - imm;
            if (w)
                r[rn] = u ? base + imm : base - imm;
        }
        if (first + n > 32u)
            UNDEF();
        for (i = 0; i < n; i++, a += 4u) {
            if (l)
                U[first + i] = ld32(m, a);
            else
                st32(m, a, U[first + i]);
        }
        return npc;
    }
    if ((hw1 & 0xFFE0u) == 0xEC40u) {                       /* VMOV two core registers <-> two S / one D */
        uint32_t rt = hw2 >> 12, rt2 = hw1 & 15u, sm = (hw2 & 15u) << 1 | (hw2 >> 5 & 1u);
        if (cp == 11u)
            sm = ((hw2 >> 5 & 1u) << 4 | (hw2 & 15u)) * 2u;
        if (sm > 30u)
            UNDEF();
        if (hw1 & 0x10u) {
            r[rt] = U[sm];
            r[rt2] = U[sm + 1u];
        } else {
            U[sm] = r[rt];
            U[sm + 1u] = r[rt2];
        }
        return npc;
    }
    if ((hw1 & 0xEF00u) != 0xEE00u)
        UNDEF();
    if (hw2 & 0x10u) {                                      /* 8, 16 and 32-bit transfers */
        uint32_t rt = hw2 >> 12, a = hw1 >> 5 & 7u, l = hw1 >> 4 & 1u;
        if (cp != 10u)
            UNDEF();
        if (a == 0u) {                                      /* VMOV Rt <-> Sn */
            uint32_t sn = (hw1 & 15u) << 1 | (hw2 >> 7 & 1u);
            if (l)
                r[rt] = U[sn];
            else
                U[sn] = r[rt];
            return npc;
        }
        if (a == 7u && (hw1 & 15u) == 1u) {                 /* VMRS / VMSR FPSCR */
            if (l) {
                if (rt == 15u) {
                    m->n = m->fpscr >> 31;
                    m->z = m->fpscr >> 30 & 1u;
                    m->c = m->fpscr >> 29 & 1u;
                    m->v = m->fpscr >> 28 & 1u;
                } else {
                    r[rt] = m->fpscr;
                }
            } else {
                m->fpscr = r[rt];
            }
            return npc;
        }
        UNDEF();
    }
    {                                                       /* data processing */
        uint32_t opc = (hw1 >> 4 & 0x8u) | (hw1 >> 4 & 3u), b6 = hw2 >> 6 & 1u;
        uint32_t sd = (hw2 >> 12 & 15u) << 1 | (hw1 >> 6 & 1u);
        uint32_t sn = (hw1 & 15u) << 1 | (hw2 >> 7 & 1u);
        uint32_t sm = (hw2 & 15u) << 1 | (hw2 >> 5 & 1u);
        float a, b, t;
        if (cp == 11u) {                                    /* double precision: not on an FPv4-SP */
            UNDEF();
        }
        a = S[sn];
        b = S[sm];
        switch (opc) {
        case 0x0: {                                         /* VMLA / VMLS: FPAdd(d, (-)product) */
            float d = S[sd], x;
            t = a * b;
            t = NAN2(t, a, b);
            if (b6)
                t = f_neg(t);
            x = d + t;
            S[sd] = NAN2(x, d, t);
            break;
        }
        case 0x1: {                                         /* VNMLS / VNMLA: FPAdd(-d, (-)product) */
            float d = f_neg(S[sd]), x;
            t = a * b;
            t = NAN2(t, a, b);
            if (b6)
                t = f_neg(t);
            x = d + t;
            S[sd] = NAN2(x, d, t);
            break;
        }
        case 0x2: t = a * b; t = NAN2(t, a, b); S[sd] = b6 ? f_neg(t) : t; break;  /* VMUL / VNMUL */
        case 0x3: t = b6 ? a - b : a + b; S[sd] = NAN2(t, a, b); break;            /* VADD / VSUB */
        case 0x8: if (b6) UNDEF(); t = a / b; S[sd] = NAN2(t, a, b); break;        /* VDIV */
        case 0x9: {                                         /* VFNMS / VFNMA: FPMulAdd(-d, (-)n, m) */
            float d = f_neg(S[sd]), n = b6 ? f_neg(a) : a;
            t = ARM_FMAF(n, b, d);
            S[sd] = NAN3(t, d, n, b);
            break;
        }
        case 0xA: {                                         /* VFMA / VFMS: FPMulAdd(d, (-)n, m) */
            float d = S[sd], n = b6 ? f_neg(a) : a;
            t = ARM_FMAF(n, b, d);
            S[sd] = NAN3(t, d, n, b);
            break;
        }
        case 0xB: {
            uint32_t opc2 = hw1 & 15u, b7 = hw2 >> 7 & 1u;
            if (!b6) {                                      /* VMOV immediate */
                S[sd] = vfp_expand((hw1 & 15u) << 4 | (hw2 & 15u));
                break;
            }
            switch (opc2) {
            case 0x0: if (b7) U[sd] = U[sm] & 0x7FFFFFFFu; else U[sd] = U[sm]; break;       /* VABS / VMOV */
            case 0x1:
                if (b7) {                                   /* VSQRT */
                    t = ARM_SQRTF(b);
                    S[sd] = f_nan(t) ? nan_fix(f2u(b), 0, 0, 1) : t;
                }
                else
                    U[sd] = U[sm] ^ 0x80000000u;            /* VNEG */
                break;
            case 0x4: f_cmp(m, S[sd], b); break;                                             /* VCMP(E) */
            case 0x5: f_cmp(m, S[sd], 0.0f); break;                                          /* VCMP(E) #0 */
            case 0x8:                                       /* VCVT.F32.S32 / .U32 */
                S[sd] = b7 ? (float)(int32_t)U[sm] : f_from_u32(U[sm]);
                break;
            case 0xC: case 0xD: {                           /* VCVT(R).U32 / .S32 .F32 */
                float f = b7 ? f_trunc(b) : f_rne(b);
                U[sd] = f_to_int(f, opc2 == 0xDu);
                break;
            }
            case 0xA: case 0xB: case 0xE: case 0xF: {       /* VCVT between float and fixed point */
                uint32_t uns = opc2 & 1u, sx = b7, size = sx ? 32u : 16u;
                uint32_t imm4 = (hw2 & 15u) << 1 | (hw2 >> 5 & 1u), fb = size - imm4;
                if (opc2 & 4u) {                            /* to fixed: round toward zero */
                    float f = f_trunc(f_scale2(S[sd], (int)fb));
                    uint32_t v;
                    if (size == 16u) {
                        int32_t lim_hi = uns ? 65535 : 32767, lim_lo = uns ? 0 : -32768;
                        int32_t iv = f_nan(f) ? 0 : f >= (float)lim_hi ? lim_hi : f <= (float)lim_lo ? lim_lo : (int32_t)f;
                        v = (uint32_t)iv & 0xFFFFu;
                        if (!uns && iv < 0)
                            v |= 0xFFFF0000u;
                    } else {
                        v = f_to_int(f, !uns);
                    }
                    U[sd] = v;
                } else {                                    /* from fixed */
                    uint32_t v = U[sd];
                    float f;
                    if (size == 16u)
                        f = uns ? (float)(int32_t)(v & 0xFFFFu) : (float)(int32_t)(int16_t)v;
                    else
                        f = uns ? f_from_u32(v) : (float)(int32_t)v;
                    S[sd] = f_scale2(f, -(int)fb);
                }
                break;
            }
            case 0x2: case 0x3: {                           /* VCVTB / VCVTT half <-> single */
                uint32_t top = b7;
                if (opc2 == 0x2u) {                         /* half -> single */
                    uint32_t h = top ? U[sm] >> 16 : U[sm] & 0xFFFFu, sg = h >> 15, e = h >> 10 & 31u, f = h & 0x3FFu;
                    uint32_t o;
                    if (e == 0u) {
                        float v = (float)f * (1.0f / 16777216.0f);
                        o = f2u(v) | sg << 31;
                    } else if (e == 31u) {
                        o = sg << 31 | 0x7F800000u | f << 13 | (f ? 0x400000u : 0u);
                    } else {
                        o = sg << 31 | (e + 112u) << 23 | f << 13;
                    }
                    U[sd] = o;
                } else {                                    /* single -> half (to nearest) */
                    uint32_t x = U[sm], sg = x >> 16 & 0x8000u, h;
                    int32_t e = (int32_t)(x >> 23 & 0xFFu) - 127 + 15;
                    uint32_t f = x & 0x7FFFFFu;
                    if ((x & 0x7F800000u) == 0x7F800000u)
                        h = sg | 0x7C00u | (f ? 0x200u | f >> 13 : 0u);
                    else if (e >= 31)
                        h = sg | 0x7C00u;
                    else if (e <= 0) {
                        float v = u2f(x & 0x7FFFFFFFu) * 16777216.0f;
                        h = sg | (uint32_t)(int32_t)f_rne(v);
                    } else {
                        uint32_t mant = f >> 13, rest = f & 0x1FFFu;
                        h = sg | (uint32_t)e << 10 | mant;
                        if (rest > 0x1000u || (rest == 0x1000u && (mant & 1u)))
                            h++;
                    }
                    U[sd] = top ? (U[sd] & 0xFFFFu) | h << 16 : (U[sd] & 0xFFFF0000u) | h;
                }
                break;
            }
            default: UNDEF();
            }
            break;
        }
        default: UNDEF();
        }
    }
    return npc;
}

/* run from m->r[15] until it returns to ARM_RET, faults or spends budget instructions. The code segment the PC is in
 * is kept (its bounds, its bytes' host address): a fetch inside it is one compare and one load */
static ARM_CODE int run(arm_t *m, uint32_t budget)
{
    uintptr_t cbase = 0;
    uint32_t clo = 1, cspan = 0, count = 0;     /* (empty: the first fetch looks the segment up) */
    int rc = ARM_OK;
    for (;;) {
        uint32_t pc = m->r[15], hw1, npc;
        if (pc - clo > cspan) {                 /* outside the code segment kept: the slow way */
            arm_seg_t *sg;
            uint32_t o;
            if (pc == ARM_RET)
                break;
            if (pc - m->trap_lo < m->trap_hi - m->trap_lo) {   /* a call into the firmware */
                arm_trap(m, pc);
                if (m->fault) {
                    rc = m->fault;
                    break;
                }
                m->r[15] = m->r[14] & ~1u;
                continue;
            }
            sg = &m->seg[pc >> 28];
            o = pc - sg->base;
            if (o >= sg->size || sg->size < 4u || o > sg->size - 4u || (pc & 1u)) {
                m->fault = ARM_F_MEM;
                m->fault_pc = m->fault_addr = pc;
                rc = ARM_F_MEM;
                break;
            }
            clo = sg->base;
            cspan = sg->size - 4u;              /* (a 32-bit instruction's second half is inside too) */
            cbase = (uintptr_t)sg->p - clo;
        }
        if (++count > budget) {
            m->fault = ARM_F_BUDGET;
            m->fault_pc = pc;
            rc = ARM_F_BUDGET;
            count--;
            break;
        }
        hw1 = *(const uint16_t *)(cbase + pc);
        if ((hw1 & 0xE000u) == 0xE000u && (hw1 & 0x1800u)) {   /* 32-bit */
            uint32_t hw2 = *(const uint16_t *)(cbase + pc + 2u);
            if (m->it && (m->it & 0xFu) && !cond_ok(m, m->it >> 4 & 15u))
                npc = pc + 4u;
            else
                npc = t32(m, pc, hw1, hw2);
        } else {
            if (m->it && (m->it & 0xFu) && !(m->it & 0x100u) && !cond_ok(m, m->it >> 4 & 15u))
                npc = pc + 2u;
            else
                npc = t16(m, pc, hw1);
        }
        if (m->fault) {
            m->fault_pc = pc;
            rc = m->fault;
            break;
        }
        if (m->it) {
            if (m->it & 0x100u)
                m->it &= 0xFFu;                 /* the IT itself: its block starts next */
            else if (m->it & 0xFu)              /* ITAdvance */
                m->it = (m->it & 7u) ? (m->it & 0xE0u) | ((m->it << 1) & 0x1Fu) : 0u;
        }
        m->r[15] = npc;
    }
    m->count += count;
    return rc;
}

ARM_CODE int arm_call(arm_t *m, uint32_t fn, uint32_t sp, const uint32_t *args, int nargs, uint32_t budget)
{
    int i;
    for (i = 0; i < nargs && i < 4; i++)
        m->r[i] = args[i];
    m->r[13] = sp;
    m->r[14] = ARM_RET | 1u;
    m->r[15] = fn & ~1u;
    m->it = 0;
    m->fault = ARM_OK;
    return run(m, budget);
}
