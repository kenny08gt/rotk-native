// Inline fast paths for the hottest DolRecomp runtime helpers, swapped into the generated code via
// DOLRECOMP_CPU_HEADER. Each one handles the common case (main RAM; finite operands with FPSCR in its
// default mode: round-to-nearest, no enabled exceptions, NI clear) and calls the exact library
// version otherwise. Like Dolphin's JIT, the fast paths skip FPRF/FI/FR bookkeeping, which games
// do not read back.
#pragma once
#include "cpu/cpu.h"

#include <math.h>
#include <string.h>

// Plain inline: forcing always_inline into the huge chunk functions measured no faster and made
// the build ~8x slower.
#define FAST_INLINE static inline

// Guest RAM is accessed through these packed one-member structs rather than memcpy/char pointers:
// type-based alias analysis can then prove guest memory never aliases CPUState, so clang keeps
// guest registers in host registers across loads/stores and drops dead ctx->pc stores.
typedef struct __attribute__((packed)) { u8 v; } fast_g8;
typedef struct __attribute__((packed)) { u16 v; } fast_g16;
typedef struct __attribute__((packed)) { u32 v; } fast_g32;
typedef struct __attribute__((packed)) { u64 v; } fast_g64;

#define FAST_RAM_CACHED 0x80000000u
#define FAST_RAM_UNCACHED 0xC0000000u

// The SDK's virtual-memory window (0x7E000000+, VMInit), which the chassis backs with flat host
// memory through cpu_set_flat_vm(); the chassis sets these to the same buffer.
extern u8* g_fast_vm;
extern u32 g_fast_vm_base, g_fast_vm_size;

FAST_INLINE u8* fast_ram(CPUState* cpu, u32 addr, u32 size) {
    u32 off = addr - FAST_RAM_CACHED;
    if (off > cpu->ram_size - size) {
        off = addr - FAST_RAM_UNCACHED;
        if (off > cpu->ram_size - size) {
            off = addr - g_fast_vm_base;
            if (!g_fast_vm || off > g_fast_vm_size - size) return NULL;
            return g_fast_vm + off;
        }
    }
    return cpu->ram + off;
}

// Stores must still clear lwarx reservations and feed the write journal (WATCH); leave those to the library.
FAST_INLINE u8* fast_ram_store(CPUState* cpu, u32 addr, u32 size) {
    if (__builtin_expect(cpu->reserve_valid || g_mem_write_journal != NULL, 0)) return NULL;
    return fast_ram(cpu, addr, size);
}

FAST_INLINE u32 fast_read32(CPUState* cpu, u32 addr) {
    u8* p = fast_ram(cpu, addr, 4);
    if (__builtin_expect(!p, 0)) return mem_read32(cpu, addr);
    return __builtin_bswap32(((const fast_g32*)p)->v);
}
FAST_INLINE u16 fast_read16(CPUState* cpu, u32 addr) {
    u8* p = fast_ram(cpu, addr, 2);
    if (__builtin_expect(!p, 0)) return mem_read16(cpu, addr);
    return __builtin_bswap16(((const fast_g16*)p)->v);
}
FAST_INLINE u8 fast_read8(CPUState* cpu, u32 addr) {
    u8* p = fast_ram(cpu, addr, 1);
    return __builtin_expect(!p, 0) ? mem_read8(cpu, addr) : ((const fast_g8*)p)->v;
}
FAST_INLINE u64 fast_read64(CPUState* cpu, u32 addr) {
    u8* p = fast_ram(cpu, addr, 8);
    if (__builtin_expect(!p, 0)) return mem_read64(cpu, addr);
    return __builtin_bswap64(((const fast_g64*)p)->v);
}
FAST_INLINE void fast_write32(CPUState* cpu, u32 addr, u32 value) {
    u8* p = fast_ram_store(cpu, addr, 4);
    if (__builtin_expect(!p, 0)) { mem_write32(cpu, addr, value); return; }
    ((fast_g32*)p)->v = __builtin_bswap32(value);
}
FAST_INLINE void fast_write16(CPUState* cpu, u32 addr, u16 value) {
    u8* p = fast_ram_store(cpu, addr, 2);
    if (__builtin_expect(!p, 0)) { mem_write16(cpu, addr, value); return; }
    ((fast_g16*)p)->v = __builtin_bswap16(value);
}
FAST_INLINE void fast_write8(CPUState* cpu, u32 addr, u8 value) {
    u8* p = fast_ram_store(cpu, addr, 1);
    if (__builtin_expect(!p, 0)) { mem_write8(cpu, addr, value); return; }
    ((fast_g8*)p)->v = value;
}
FAST_INLINE void fast_write64(CPUState* cpu, u32 addr, u64 value) {
    u8* p = fast_ram_store(cpu, addr, 8);
    if (__builtin_expect(!p, 0)) { mem_write64(cpu, addr, value); return; }
    ((fast_g64*)p)->v = __builtin_bswap64(value);
}

// ---- Floating point ----

FAST_INLINE u64 fast_f64_bits(f64 v) { u64 b; memcpy(&b, &v, 8); return b; }
FAST_INLINE f64 fast_f64_value(u64 b) { f64 v; memcpy(&v, &b, 8); return v; }

// FPSCR low byte = VE OE UE ZE XE NI RN. With no enabled exceptions and round-to-nearest, host arithmetic
// matches; NI (flush denormal results, set by this game) only differs for tiny results, which the
// *_out helpers below send to the library.
FAST_INLINE int fast_fp_mode(const CPUState* cpu) { return (cpu->fpscr & 0xFBu) == 0; }

// Gekko rounds the multiplier of single-precision multiplies to 25 significant bits. Normal values only;
// zero/denormal inputs return 0 so callers fall back.
FAST_INLINE int fast_force_25_bit(f64 v, f64* out) {
    u64 bits = fast_f64_bits(v);
    if ((bits & 0x7FF0000000000000ull) == 0) {
        if (bits << 1) return 0;
        *out = v;
        return 1;
    }
    *out = fast_f64_value((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull));
    return 1;
}

// fma(a, c, addend) rounded to double then single can double-round on an exact tie; same fix-up as the library.
FAST_INLINE f64 fast_single_fma(f64 a, f64 c25, f64 addend) {
    f64 r = fma(a, c25, addend);
    u64 bits = fast_f64_bits(r);
    if (__builtin_expect((bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull, 0)) {
        f64 a_prime = addend - r;
        f64 b_prime = r + a_prime;
        f64 error = fma(a, c25, a_prime) + (addend - b_prime);
        if (error != 0.0) r = fast_f64_value((error > 0.0) == (r > 0.0) ? bits + 1 : bits - 1);
    }
    return r;
}

// No input checks: NaN and infinity always propagate to a non-finite result, which the *_out helpers
// reject, so a finite result proves the inputs were finite too.
#define FAST_FINITE2(x, y) 1
#define FAST_FINITE3(x, y, z) 1

FAST_INLINE int fast_single_out(CPUState* cpu, u8 d, f64 r) {
    f32 s = (f32)r;
    if (__builtin_expect(!isfinite(s) || (r != 0.0 && fabs(r) < 0x1p-126), 0)) return 0;
    cpu->fpr[d] = cpu->ps1[d] = (f64)s;
    return 1;
}

FAST_INLINE void fast_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (fast_fp_mode(cpu) && FAST_FINITE2(x, y) && fast_single_out(cpu, d, x + y)) return;
    ppc_fadds(cpu, d, a, b);
}
FAST_INLINE void fast_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (fast_fp_mode(cpu) && FAST_FINITE2(x, y) && fast_single_out(cpu, d, x - y)) return;
    ppc_fsubs(cpu, d, a, b);
}
FAST_INLINE void fast_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 x = cpu->fpr[a], y;
    if (fast_fp_mode(cpu) && FAST_FINITE2(x, cpu->fpr[c]) && fast_force_25_bit(cpu->fpr[c], &y) &&
        fast_single_out(cpu, d, x * y))
        return;
    ppc_fmuls(cpu, d, a, c);
}
FAST_INLINE void fast_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (fast_fp_mode(cpu) && FAST_FINITE2(x, y) && y != 0.0 && fast_single_out(cpu, d, x / y)) return;
    ppc_fdivs(cpu, d, a, b);
}
FAST_INLINE void fast_frsp(CPUState* cpu, u8 d, u8 b) {
    f64 x = cpu->fpr[b];
    if (fast_fp_mode(cpu) && __builtin_expect(isfinite(x), 1) && fast_single_out(cpu, d, x)) return;
    ppc_frsp(cpu, d, b);
}

#define FAST_DOUBLE_OP(name, expr, extra)                                         \
    FAST_INLINE void fast_##name(CPUState* cpu, u8 d, u8 a, u8 b) {             \
        f64 x = cpu->fpr[a], y = cpu->fpr[b];                                     \
        if (fast_fp_mode(cpu) && FAST_FINITE2(x, y) && (extra)) {                 \
            f64 r = (expr);                                                       \
            if (__builtin_expect(isnormal(r) || r == 0.0, 1)) { cpu->fpr[d] = r; return; } \
        }                                                                         \
        ppc_##name(cpu, d, a, b);                                                 \
    }
FAST_DOUBLE_OP(fadd, x + y, 1)
FAST_DOUBLE_OP(fsub, x - y, 1)
FAST_DOUBLE_OP(fmul, x * y, 1)
FAST_DOUBLE_OP(fdiv, x / y, y != 0.0)

FAST_INLINE bool fast_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative,
                            f64* output) {
    f64 c25;
    if (fast_fp_mode(cpu) && FAST_FINITE3(a, b, c)) {
        f64 addend = subtract ? -b : b;
        f64 r;
        int ok = 1;
        if (single) {
            ok = fast_force_25_bit(c, &c25);
            r = ok ? (f64)(f32)fast_single_fma(a, c25, addend) : 0;
        } else {
            r = fma(a, c, addend);
        }
        if (ok && __builtin_expect(isfinite(r), 1)) {
            *output = negative ? -r : r;
            return true;
        }
    }
    return ppc_fma(cpu, a, c, b, single, subtract, negative, output);
}

FAST_INLINE void fast_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    if (__builtin_expect(isnan(a) || isnan(b), 0)) { ppc_fcmp(cpu, crfd, a, b, ordered); return; }
    u32 compare = a < b ? 8u : a > b ? 4u : 2u;
    cpu->fpscr = (cpu->fpscr & ~(0xFu << 12)) | (compare << 12);
    u32 shift = 4u * (7u - crfd);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}

// Paired singles: both slots finite in and out, otherwise the library handles NaN/inf selection.
FAST_INLINE int fast_paired_out(CPUState* cpu, u8 d, f64 r0, f64 r1) {
    f32 s0 = (f32)r0, s1 = (f32)r1;
    if (__builtin_expect(!isfinite(s0) || !isfinite(s1), 0)) return 0;
    cpu->fpr[d] = (f64)s0;
    cpu->ps1[d] = (f64)s1;
    return 1;
}

FAST_INLINE void fast_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    if (fast_fp_mode(cpu) && fast_paired_out(cpu, d, a0 + b0, a1 + b1)) return;
    ppc_ps_add_op(cpu, d, a, b);
}
FAST_INLINE void fast_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    if (fast_fp_mode(cpu) && fast_paired_out(cpu, d, a0 - b0, a1 - b1)) return;
    ppc_ps_sub_op(cpu, d, a, b);
}
FAST_INLINE void fast_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) {
    f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c0, c1;
    if (fast_fp_mode(cpu) && FAST_FINITE2(a0, a1) && FAST_FINITE2(cpu->fpr[c], cpu->ps1[c]) &&
        fast_force_25_bit(cpu->fpr[c], &c0) && fast_force_25_bit(cpu->ps1[c], &c1) &&
        fast_paired_out(cpu, d, a0 * c0, a1 * c1))
        return;
    ppc_ps_mul_op(cpu, d, a, c);
}
FAST_INLINE void fast_ps_muls(CPUState* cpu, u8 d, u8 a, f64 scalar_in, void (*slow)(CPUState*, u8, u8, u8), u8 c) {
    f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], s;
    if (fast_fp_mode(cpu) && FAST_FINITE3(a0, a1, scalar_in) && fast_force_25_bit(scalar_in, &s) &&
        fast_paired_out(cpu, d, a0 * s, a1 * s))
        return;
    slow(cpu, d, a, c);
}
FAST_INLINE void fast_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) { fast_ps_muls(cpu, d, a, cpu->fpr[c], ppc_ps_muls0, c); }
FAST_INLINE void fast_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) { fast_ps_muls(cpu, d, a, cpu->ps1[c], ppc_ps_muls1, c); }

FAST_INLINE int fast_ps_fma(CPUState* cpu, u8 d, f64 a0, f64 c0, f64 b0, f64 a1, f64 c1, f64 b1, bool negative) {
    if (!fast_fp_mode(cpu) || !FAST_FINITE3(a0, b0, c0) || !FAST_FINITE3(a1, b1, c1)) return 0;
    f64 k0, k1;
    if (!fast_force_25_bit(c0, &k0) || !fast_force_25_bit(c1, &k1)) return 0;
    f64 r0 = (f64)(f32)fast_single_fma(a0, k0, b0), r1 = (f64)(f32)fast_single_fma(a1, k1, b1);
    return fast_paired_out(cpu, d, negative ? -r0 : r0, negative ? -r1 : r1);
}
FAST_INLINE void fast_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    f64 b0 = subtract ? -cpu->fpr[b] : cpu->fpr[b], b1 = subtract ? -cpu->ps1[b] : cpu->ps1[b];
    if (fast_ps_fma(cpu, d, cpu->fpr[a], cpu->fpr[c], b0, cpu->ps1[a], cpu->ps1[c], b1, negative)) return;
    ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative);
}
FAST_INLINE void fast_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    if (fast_ps_fma(cpu, d, cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], cpu->ps1[a], cpu->fpr[c], cpu->ps1[b], false)) return;
    ppc_ps_madds0(cpu, d, a, c, b);
}
FAST_INLINE void fast_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    if (fast_ps_fma(cpu, d, cpu->fpr[a], cpu->ps1[c], cpu->fpr[b], cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], false)) return;
    ppc_ps_madds1(cpu, d, a, c, b);
}
FAST_INLINE void fast_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 s = cpu->fpr[a] + cpu->ps1[b], keep = cpu->ps1[c];
    if (fast_fp_mode(cpu) && fast_paired_out(cpu, d, s, keep)) return;
    ppc_ps_sum0(cpu, d, a, c, b);
}
FAST_INLINE void fast_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    f64 s = cpu->fpr[a] + cpu->ps1[b], keep = cpu->fpr[c];
    if (fast_fp_mode(cpu) && fast_paired_out(cpu, d, keep, s)) return;
    ppc_ps_sum1(cpu, d, a, c, b);
}

// ---- Quantized loads/stores ----

// Zero or a normal number: the bit patterns where a plain host conversion matches the library exactly.
FAST_INLINE int fast_f32_plain(u32 bits) {
    u32 exp = (bits >> 23) & 0xFFu;
    return exp == 0 ? (bits & 0x7FFFFFFFu) == 0 : exp != 0xFFu;
}

#define FAST_HID2_LSQE 0x80000000u
#define FAST_HID2_PSE 0x20000000u

FAST_INLINE int fast_psq_ok(const CPUState* cpu, bool indexed) {
    return (cpu->hid2 & FAST_HID2_PSE) && (indexed || (cpu->hid2 & FAST_HID2_LSQE));
}

// 2^-scale for the 6-bit signed GQR scale field.
FAST_INLINE f64 fast_psq_dequant(u32 field) {
    s32 scale = (s32)(field << 26) >> 26;
    return fast_f64_value((u64)(1023 - scale) << 52);
}

FAST_INLINE int fast_psq_load_one(CPUState* cpu, u32 ea, u32 type, f64 mul, f64* out) {
    u8* p;
    switch (type) {
    case 0: {
        if ((ea & 3u) || !(p = fast_ram(cpu, ea, 4))) return 0;
        u32 bits = __builtin_bswap32(((const fast_g32*)p)->v);
        if (!fast_f32_plain(bits)) return 0;  // denormal/inf/NaN keep the library's exact bit handling
        f32 f;
        memcpy(&f, &bits, 4);
        *out = (f64)f;
        return 1;
    }
    case 4: if (!(p = fast_ram(cpu, ea, 1))) return 0; *out = (f64)(f32)((f64)((const fast_g8*)p)->v * mul); return 1;
    case 6: if (!(p = fast_ram(cpu, ea, 1))) return 0; *out = (f64)(f32)((f64)(s8)((const fast_g8*)p)->v * mul); return 1;
    case 5:
    case 7: {
        if (!(p = fast_ram(cpu, ea, 2))) return 0;
        u16 h = __builtin_bswap16(((const fast_g16*)p)->v);
        *out = (f64)(f32)((type == 5 ? (f64)h : (f64)(s16)h) * mul);
        return 1;
    }
    default: return 0;
    }
}

FAST_INLINE bool fast_psq_load_inline(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    if (fast_psq_ok(cpu, indexed)) {
        u32 q = cpu->gqr[gqr & 7u];
        u32 type = (q >> 16) & 7u;
        u32 size = type == 0 ? 4 : (type & 1) ? 2 : 1;
        f64 mul = fast_psq_dequant(q >> 24);
        f64 v0, v1 = 1.0;
        if (fast_psq_load_one(cpu, ea, type, mul, &v0) && (w || fast_psq_load_one(cpu, ea + size, type, mul, &v1))) {
            cpu->fpr[frD] = v0;
            cpu->ps1[frD] = v1;
            return true;
        }
    }
    return ppc_psq_load(cpu, frD, ea, w, gqr, indexed, cia);
}

// Unquantized (type 0) stores only; quantizing stores keep the library's saturation logic.
FAST_INLINE int fast_psq_store_one(CPUState* cpu, u32 ea, f64 value) {
    f32 s = (f32)value;
    u32 bits;
    memcpy(&bits, &s, 4);
    if (!fast_f32_plain(bits)) return 0;
    u8* p = fast_ram_store(cpu, ea, 4);
    if (!p) return 0;
    ((fast_g32*)p)->v = __builtin_bswap32(bits);
    return 1;
}

// Quantizing store of one finite value: scale by 2^scale, truncate, saturate (as psq_quantize_int).
FAST_INLINE s32 fast_psq_quantize(f64 value, f64 mul, s32 lo, s32 hi) {
    f64 scaled = trunc(value * mul);
    return scaled <= lo ? lo : scaled >= hi ? hi : (s32)scaled;
}

FAST_INLINE bool fast_psq_store_quantized(CPUState* cpu, u32 type, u32 q, u32 ea, bool w, f64 v0, f64 v1) {
    if (!isfinite(v0) || !isfinite(v1)) return false;
    // 2^scale for the 6-bit signed GQR store scale field.
    s32 scale = (s32)((q >> 8) << 26) >> 26;
    f64 mul = fast_f64_value((u64)(1023 + scale) << 52);
    u32 size = (type & 1) ? 2 : 1;
    u8* p = fast_ram_store(cpu, ea, w ? size : 2 * size);
    if (!p) return false;
    s32 lo = type == 4 || type == 5 ? 0 : type == 6 ? -128 : -32768;
    s32 hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
    for (int i = 0; i < (w ? 1 : 2); i++) {
        s32 x = fast_psq_quantize(i ? v1 : v0, mul, lo, hi);
        if (size == 1) ((fast_g8*)p)[i].v = (u8)x;
        else ((fast_g16*)p)[i].v = __builtin_bswap16((u16)x);
    }
    return true;
}

FAST_INLINE bool fast_psq_store_inline(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    u32 q = cpu->gqr[gqr & 7u], type = q & 7u;
    if (type >= 4 && fast_psq_ok(cpu, indexed) && fast_psq_store_quantized(cpu, type, q, ea, w, cpu->fpr[frS], cpu->ps1[frS]))
        return true;
    if (fast_psq_ok(cpu, indexed) && type == 0 && (ea & 3u) == 0) {
        f64 v0 = cpu->fpr[frS], v1 = cpu->ps1[frS];
        // Check both before writing either, so a fallback never sees a half-done store.
        f32 s0 = (f32)v0, s1 = (f32)v1;
        u32 b0, b1;
        memcpy(&b0, &s0, 4);
        memcpy(&b1, &s1, 4);
        if (fast_f32_plain(b0) && (w || fast_f32_plain(b1)) && fast_ram_store(cpu, ea, w ? 4 : 8)) {
            fast_psq_store_one(cpu, ea, v0);
            if (!w) fast_psq_store_one(cpu, ea + 4, v1);
            return true;
        }
    }
    return ppc_psq_store(cpu, frS, ea, w, gqr, indexed, cia);
}

// FAST_NO_MEM / FAST_NO_FP / FAST_NO_PSQ turn a group back to the library helpers (for bisecting).
#ifndef FAST_NO_MEM
#define mem_read8 fast_read8
#define mem_read16 fast_read16
#define mem_read32 fast_read32
#define mem_read64 fast_read64
#define mem_write8 fast_write8
#define mem_write16 fast_write16
#define mem_write32 fast_write32
#define mem_write64 fast_write64
#endif
#ifndef FAST_NO_FP
#define ppc_fadds fast_fadds
#define ppc_fsubs fast_fsubs
#define ppc_fmuls fast_fmuls
#define ppc_fdivs fast_fdivs
#define ppc_frsp fast_frsp
#define ppc_fadd fast_fadd
#define ppc_fsub fast_fsub
#define ppc_fmul fast_fmul
#define ppc_fdiv fast_fdiv
#define ppc_fma fast_fma
#define ppc_fcmp fast_fcmp
#define ppc_ps_add_op fast_ps_add_op
#define ppc_ps_sub_op fast_ps_sub_op
#define ppc_ps_mul_op fast_ps_mul_op
#define ppc_ps_muls0 fast_ps_muls0
#define ppc_ps_muls1 fast_ps_muls1
#define ppc_ps_madd_op fast_ps_madd_op
#define ppc_ps_madds0 fast_ps_madds0
#define ppc_ps_madds1 fast_ps_madds1
#define ppc_ps_sum0 fast_ps_sum0
#define ppc_ps_sum1 fast_ps_sum1
#endif
#ifndef FAST_NO_PSQ
#define ppc_psq_load_inline fast_psq_load_inline
#define ppc_psq_store_inline fast_psq_store_inline
#endif
