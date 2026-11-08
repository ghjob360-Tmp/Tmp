/* SPDX-License-Identifier: MPL-2.0 */
/* What the interpreter is checked on: ARM code from GCC (as a logue unit is built) covering the instruction set,
 * the DSP extension (by inline assembly) and the FPU, newlib's libm and libc. test_main writes words to out; the
 * reference (qemu-arm -cpu cortex-m4) and the interpreter must write the same words. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *O;
static uint32_t on;
static void put(uint32_t v) { O[on++] = v; }
static void putf(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    put(x.u);
}
static void putd(double d)
{
    union { double d; uint64_t u; } x;
    x.d = d;
    put((uint32_t)x.u);
    put((uint32_t)(x.u >> 32));
}

static uint32_t rs = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static float rndf(void)   /* all sorts: small, large, negative, zero, specials now and then */
{
    uint32_t k = rnd() % 16u, u = rnd();
    union { float f; uint32_t u; } x;
    switch (k) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: x.u = u; return x.f;                               /* any bits (NaNs, infinities, denormals) */
    case 3: return (float)(int32_t)u;
    case 4: return (float)(int32_t)u * 1e-9f;
    default: return ((float)(int32_t)u / 2147483648.0f) * (float)(1u << (k));
    }
}

#define ASM2(ins, a, b) ({ uint32_t r_; __asm__ volatile(ins " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); r_; })
#define ASM3(ins, a, b, c) ({ uint32_t r_; __asm__ volatile(ins " %0, %1, %2, %3" : "=r"(r_) : "r"(a), "r"(b), "r"(c)); r_; })
#define ASM1(ins, a) ({ uint32_t r_; __asm__ volatile(ins " %0, %1" : "=r"(r_) : "r"(a)); r_; })

static uint32_t apsr(void)
{
    uint32_t v;
    __asm__ volatile("mrs %0, apsr" : "=r"(v));
    return v;
}
static void clear_q(void)
{
    uint32_t z = 0;
    __asm__ volatile("msr apsr_nzcvqg, %0" : : "r"(z));
}

static void ints(void)
{
    int i;
    for (i = 0; i < 400; i++) {
        uint32_t a = rnd(), b = rnd(), c = rnd(), s = rnd() % 40u;
        if (i % 7 == 0)
            b = 0;
        if (i % 11 == 0)
            a = 0x80000000u, b = 0xFFFFFFFFu;
        put(a + b);
        put(a - b);
        put(a * b);
        put(b ? a / b : 0);
        put(b ? (uint32_t)((int32_t)a / (int32_t)(b | 1u)) : 0);
        put(b ? a % b : 0);
        put((uint32_t)((uint64_t)a * b >> 32));
        put((uint32_t)((int64_t)(int32_t)a * (int32_t)b >> 32));
        put((uint32_t)(((uint64_t)a << 32 | b) / ((uint64_t)c | 1u)));
        put((uint32_t)((int64_t)((uint64_t)a << 32 | b) % (int64_t)(int32_t)(c | 1u)));
        put(ASM2("lsl", a, s));
        put(ASM2("lsr", a, s));
        put(ASM2("asr", a, s));
        put(ASM2("ror", a, s));
        put(ASM2("lsls", a, s) ^ (apsr() & 0xF0000000u));
        put(ASM2("lsrs", a, s) ^ (apsr() & 0xF0000000u));
        put(ASM2("asrs", a, s) ^ (apsr() & 0xF0000000u));
        put(ASM2("rors", a, s) ^ (apsr() & 0xF0000000u));
        put(ASM2("adds", a, b) ^ (apsr() & 0xF0000000u));
        put(ASM2("subs", a, b) ^ (apsr() & 0xF0000000u));
        put(ASM2("adcs", a, b) ^ (apsr() & 0xF0000000u));
        put(ASM2("sbcs", a, b) ^ (apsr() & 0xF0000000u));
        put(ASM1("clz", a));
        put(ASM1("rbit", a));
        put(ASM1("rev", a));
        put(ASM1("rev16", a));
        put(ASM1("revsh", a));
        put(a < b ? a : b);
        put((int32_t)a < (int32_t)b ? a : b);
        put((int32_t)a < 0 ? -a : a);
        put((a >> (s & 31)) & 0x3Fu);
        put((uint32_t)(((int32_t)a << 7) >> 20));
        put((a & ~(0xFFu << 9)) | ((b & 0xFFu) << 9));
        put(__builtin_popcount(a));
        put(__builtin_ctz(a | 1u));
        put((uint32_t)(int16_t)a + (uint32_t)(int8_t)b);
        put((a & 0xFFFFu) * (b & 0xFFFFu));
    }
}

static void dsp(void)
{
    int i;
    for (i = 0; i < 300; i++) {
        uint32_t a = rnd(), b = rnd(), c = rnd(), q;
        if (i & 1) {
            a = (a & 0x80008000u) | 0x7FF07FF0u;
            b = (b & 0x80008000u) | 0x7F007F00u;
        }
        clear_q();
        put(ASM2("qadd", a, b)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM2("qsub", a, b)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM2("qdadd", a, b)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM2("qdsub", a, b)); put(apsr() >> 27 & 1u); clear_q();
        __asm__ volatile("ssat %0, #12, %1" : "=r"(q) : "r"(a)); put(q); put(apsr() >> 27 & 1u); clear_q();
        __asm__ volatile("ssat %0, #24, %1, asr #3" : "=r"(q) : "r"(a)); put(q); clear_q();
        __asm__ volatile("usat %0, #10, %1" : "=r"(q) : "r"(a)); put(q); put(apsr() >> 27 & 1u); clear_q();
        __asm__ volatile("usat %0, #31, %1, lsl #2" : "=r"(q) : "r"(a)); put(q); clear_q();
        __asm__ volatile("ssat16 %0, #9, %1" : "=r"(q) : "r"(a)); put(q); clear_q();
        __asm__ volatile("usat16 %0, #7, %1" : "=r"(q) : "r"(a)); put(q); clear_q();
        put(ASM2("smulbb", a, b)); put(ASM2("smulbt", a, b)); put(ASM2("smultb", a, b)); put(ASM2("smultt", a, b));
        put(ASM3("smlabb", a, b, c)); put(ASM3("smlatt", a, b, c)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM2("smulwb", a, b)); put(ASM2("smulwt", a, b));
        put(ASM3("smlawb", a, b, c)); put(ASM3("smlawt", a, b, c)); clear_q();
        put(ASM2("smuad", a, b)); put(ASM2("smuadx", a, b)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM3("smlad", a, b, c)); put(ASM3("smladx", a, b, c)); put(apsr() >> 27 & 1u); clear_q();
        put(ASM2("smusd", a, b)); put(ASM2("smusdx", a, b));
        put(ASM3("smlsd", a, b, c)); put(ASM3("smlsdx", a, b, c)); clear_q();
        put(ASM2("smmul", a, b)); put(ASM2("smmulr", a, b));
        put(ASM3("smmla", a, b, c)); put(ASM3("smmlar", a, b, c));
        put(ASM3("smmls", a, b, c)); put(ASM3("smmlsr", a, b, c));
        put(ASM2("usad8", a, b)); put(ASM3("usada8", a, b, c));
        put(ASM3("mls", a, b, c));
        {
            uint32_t lo = c, hi = a ^ b;
            __asm__ volatile("smlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("umaal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("smlalbt %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("smlald %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("smlaldx %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("smlsld %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
            __asm__ volatile("smlsldx %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(a), "r"(b)); put(lo); put(hi);
        }
#define PAR(op) put(ASM2(op, a, b)); put(apsr() >> 16 & 15u)
        PAR("sadd16"); PAR("qadd16"); PAR("shadd16"); PAR("uadd16"); PAR("uqadd16"); PAR("uhadd16");
        PAR("sasx"); PAR("qasx"); PAR("shasx"); PAR("uasx"); PAR("uqasx"); PAR("uhasx");
        PAR("ssax"); PAR("qsax"); PAR("shsax"); PAR("usax"); PAR("uqsax"); PAR("uhsax");
        PAR("ssub16"); PAR("qsub16"); PAR("shsub16"); PAR("usub16"); PAR("uqsub16"); PAR("uhsub16");
        PAR("sadd8"); PAR("qadd8"); PAR("shadd8"); PAR("uadd8"); PAR("uqadd8"); PAR("uhadd8");
        PAR("ssub8"); PAR("qsub8"); PAR("shsub8"); PAR("usub8"); PAR("uqsub8"); PAR("uhsub8");
        put(ASM2("sel", a, b));
        put(ASM1("sxtb16", a)); put(ASM1("uxtb16", a));
        put(ASM2("sxtab", a, b)); put(ASM2("uxtah", a, b)); put(ASM2("sxtab16", a, b)); put(ASM2("uxtab16", a, b));
        __asm__ volatile("sxtah %0, %1, %2, ror #16" : "=r"(q) : "r"(a), "r"(b)); put(q);
        __asm__ volatile("uxtb %0, %1, ror #8" : "=r"(q) : "r"(a)); put(q);
        __asm__ volatile("pkhbt %0, %1, %2, lsl #5" : "=r"(q) : "r"(a), "r"(b)); put(q);
        __asm__ volatile("pkhtb %0, %1, %2, asr #9" : "=r"(q) : "r"(a), "r"(b)); put(q);
        __asm__ volatile("sbfx %0, %1, #3, #17" : "=r"(q) : "r"(a)); put(q);
        __asm__ volatile("ubfx %0, %1, #11, #21" : "=r"(q) : "r"(a)); put(q);
        q = c; __asm__ volatile("bfi %0, %1, #5, #13" : "+r"(q) : "r"(a)); put(q);
        q = c; __asm__ volatile("bfc %0, #20, #12" : "+r"(q)); put(q);
        __asm__ volatile("rrx %0, %1" : "=r"(q) : "r"(a)); put(q);
        __asm__ volatile("orn %0, %1, %2, ror #7" : "=r"(q) : "r"(a), "r"(b)); put(q);
        __asm__ volatile("teq %0, %1, lsl #3\n mrs %0, apsr" : "+r"(q) : "r"(b)); put(q);
    }
}

typedef union { float f; uint32_t u; } fu;
static uint32_t vcvt_s32(float f) { uint32_t r; __asm__ volatile("vcvt.s32.f32 %0, %0" : "=t"(r) : "0"(f)); return r; }
static uint32_t vcvt_u32(float f) { uint32_t r; __asm__ volatile("vcvt.u32.f32 %0, %0" : "=t"(r) : "0"(f)); return r; }
static uint32_t vcvtr_s32(float f) { uint32_t r; __asm__ volatile("vcvtr.s32.f32 %0, %0" : "=t"(r) : "0"(f)); return r; }
static uint32_t vcvtr_u32(float f) { uint32_t r; __asm__ volatile("vcvtr.u32.f32 %0, %0" : "=t"(r) : "0"(f)); return r; }
static float fix_to_f(uint32_t v) { float r; __asm__ volatile("vcvt.f32.s32 %0, %0, #12" : "=t"(r) : "0"(v)); return r; }
static float ufix16_to_f(uint32_t v) { float r; __asm__ volatile("vcvt.f32.u16 %0, %0, #3" : "=t"(r) : "0"(v)); return r; }
static uint32_t f_to_fix(float f) { uint32_t r; __asm__ volatile("vcvt.s32.f32 %0, %0, #20" : "=t"(r) : "0"(f)); return r; }
static uint32_t f_to_sfix16(float f) { uint32_t r; __asm__ volatile("vcvt.s16.f32 %0, %0, #7" : "=t"(r) : "0"(f)); return r; }
static float uint_to_f(uint32_t v) { float r; __asm__ volatile("vcvt.f32.u32 %0, %0" : "=t"(r) : "0"(v)); return r; }
static float half_lo(uint32_t v) { float r; __asm__ volatile("vcvtb.f32.f16 %0, %0" : "=t"(r) : "0"(v)); return r; }
static float half_hi(uint32_t v) { float r; __asm__ volatile("vcvtt.f32.f16 %0, %0" : "=t"(r) : "0"(v)); return r; }
static uint32_t to_half(float f, uint32_t keep)
{
    fu k;
    k.u = keep;
    __asm__ volatile("vcvtb.f16.f32 %0, %1" : "+t"(k.f) : "t"(f));
    return k.u;
}
static uint32_t fcmp(float a, float b)
{
    uint32_t r;
    __asm__ volatile("vcmp.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(r) : "t"(a), "t"(b));
    return r >> 28;
}
static uint32_t fcmp0(float a)
{
    uint32_t r;
    __asm__ volatile("vcmpe.f32 %1, #0\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr" : "=r"(r) : "t"(a));
    return r >> 28;
}

static void floats(void)
{
    int i;
    for (i = 0; i < 600; i++) {
        float a = rndf(), b = rndf(), c = rndf();
        uint32_t u = rnd();
        putf(a + b); putf(a - b); putf(a * b); putf(a / b);
        putf(c + a * b); putf(c - a * b); putf(-(c + a * b)); putf(-(a * b));
        putf(__builtin_fmaf(a, b, c)); putf(__builtin_fmaf(-a, b, c)); putf(__builtin_fmaf(a, b, -c));
        putf(-__builtin_fmaf(a, b, c));
        putf(sqrtf(fabsf(a))); putf(fabsf(b)); putf(-c);
        put(fcmp(a, b)); put(fcmp0(c));
        put(vcvt_s32(a)); put(vcvt_u32(b)); put(vcvtr_s32(c)); put(vcvtr_u32(a));
        put(vcvt_s32(a * 1e-6f)); put(vcvtr_s32(b * 1e-3f));
        putf((float)(int32_t)u); putf(uint_to_f(u)); putf(fix_to_f(u)); putf(ufix16_to_f(u));
        put(f_to_fix(a * 1e-9f)); put(f_to_sfix16(b * 1e-7f)); put(f_to_fix(c));
        putf(half_lo(u)); putf(half_hi(u)); put(to_half(a * 1e-30f, u)); put(to_half(b, u)); put(to_half(c * 1e-6f, u));
        putf(a > b ? a : b); putf(fmaxf(a, c)); putf(fminf(b, c));
        putf(0.5f); putf(-1.75f); putf(31.0f); putf(0.1328125f);
    }
}

static void libm(void)
{
    int i;
    for (i = 0; i < 250; i++) {
        float x = ((float)(int32_t)rnd() / 2147483648.0f) * 10.0f, y = (float)(rnd() & 0xFFFFu) / 6000.0f;
        putf(sinf(x)); putf(cosf(x)); putf(tanf(x * 0.1f)); putf(expf(x)); putf(logf(y + 1e-3f)); putf(powf(y, x * 0.3f));
        putf(tanhf(x)); putf(atan2f(x, y)); putf(floorf(x)); putf(ceilf(x)); putf(roundf(x)); putf(fmodf(x, 1.3f));
        putf(exp2f(x)); putf(log2f(y + 0.5f)); putf(log10f(y + 2.0f)); putf(asinf(x * 0.09f)); putf(sinhf(x * 0.5f));
        putd(sin((double)x)); putd(exp((double)y)); putd((double)x * (double)y + 1.0 / 3.0); putd(sqrt((double)y));
        putd(pow((double)y, 1.5)); put((uint32_t)(int32_t)((double)x * 1e6));
    }
}

static int cmp(const void *a, const void *b) { return (*(const int32_t *)a > *(const int32_t *)b) - (*(const int32_t *)a < *(const int32_t *)b); }

static uint32_t sw(uint32_t k)   /* a jump table (TBB / TBH) */
{
    switch (k % 23u) {
    case 0: return 7; case 1: return 77; case 2: return k * 3u; case 3: return k ^ 0x55u; case 4: return 1;
    case 5: return 5; case 6: return k + 99u; case 7: return k >> 3; case 8: return 1234; case 9: return k << 2;
    case 10: return 9; case 11: return 0; case 12: return k - 1u; case 13: return 13; case 14: return k * k;
    case 15: return 15; case 16: return ~k; case 17: return 17; case 18: return k | 1u; case 19: return 19;
    case 20: return k & 0xF0u; case 21: return 21;
    default: return 22;
    }
}

static uint32_t fib(uint32_t n) { return n < 2u ? n : fib(n - 1u) + fib(n - 2u); }

static void misc(void)
{
    int32_t arr[200];
    char buf[160];
    int i;
    for (i = 0; i < 200; i++)
        arr[i] = (int32_t)rnd();
    qsort(arr, 200, sizeof arr[0], cmp);
    for (i = 0; i < 200; i += 3)
        put((uint32_t)arr[i]);
    for (i = 0; i < 500; i++)
        put(sw(rnd()));
    put(fib(20));
    for (i = 0; i < 30; i++) {
        int n = snprintf(buf, sizeof buf, "%d %u %x %s %c|%08lx", (int)rnd(), (unsigned)rnd(), (unsigned)rnd(), "logue",
                         (char)('a' + i), (unsigned long)rnd());
        uint32_t h = 2166136261u;
        int k;
        for (k = 0; k < n; k++)
            h = (h ^ (uint8_t)buf[k]) * 16777619u;
        put(h);
        put((uint32_t)n);
    }
    {
        static uint8_t big[1000];
        for (i = 0; i < 1000; i++)
            big[i] = (uint8_t)rnd();
        memmove(big + 3, big + 1, 700);
        memcpy(big + 800, big + 5, 150);
        memset(big + 401, 0x5A, 77);
        for (i = 0; i < 1000; i += 4)
            put((uint32_t)big[i] | (uint32_t)big[i + 1] << 8 | (uint32_t)big[i + 2] << 16 | (uint32_t)big[i + 3] << 24);
        put((uint32_t)strlen((const char *)big + 401));
        put((uint32_t)memcmp(big, big + 2, 30));
    }
    {   /* unaligned word access (allowed on the M4) */
        static uint8_t ub[64];
        uint32_t v;
        for (i = 0; i < 64; i++)
            ub[i] = (uint8_t)(i * 37);
        for (i = 0; i < 20; i++) {
            __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(ub + i));
            put(v);
            v ^= 0xA5A5A5A5u;
            __asm__ volatile("str %0, [%1]" : : "r"(v), "r"(ub + 40 + (i & 7)) : "memory");
            __asm__ volatile("ldrh %0, [%1]" : "=r"(v) : "r"(ub + 2 * i + 1));
            put(v);
        }
        for (i = 0; i < 64; i++)
            put(ub[i]);
    }
}

uint32_t test_main(uint32_t *out)
{
    O = out;
    on = 0;
#ifndef SKIP
#define SKIP 0
#endif
    if (!(SKIP & 1)) ints();
    if (!(SKIP & 2)) dsp();
    if (!(SKIP & 4)) floats();
    if (!(SKIP & 8)) libm();
    if (!(SKIP & 16)) misc();
    return on;
}
