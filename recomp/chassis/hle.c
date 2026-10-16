// Native versions of hot guest routines (paired-single math). Each one computes exactly what the
// generated code computes with chassis/fast_cpu.h, but keeps guest registers in host registers.
// Whenever the generated code would leave its fast path (denormal/NaN/inf values, memory outside
// RAM and the VM window, quantization types 1-3, lwarx reservations, WATCH), the native version
// stops before that loop iteration has any side effect and hands the iteration back to the
// generated code.
//
// The generated code hands control to the dispatcher at hook points (ROTK_HLE_POINT, listed in
// chassis/hle_points.txt): loop heads, and the entries and return sites of functions called from
// inside the same chunk, which the generated code otherwise reaches with a plain goto.
//
// HLE_VERIFY=1 runs every completed native call against the generated code on the same state and
// reports any register or memory difference. NO_HLE=1 turns the native versions off.
#include "hle.h"
#include "fast_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

unsigned g_hle_points_on, g_hle_skip;

typedef struct { f64 a, b; } Pair;

// ---- exact replicas of the fast_cpu.h fast paths, returning 0 where those fall back ----

// psq_l (fast_psq_load_inline): any GQR, W = load one value and set ps1 = 1.0.
static inline int ld(CPUState* c, u32 ea, int w, int gqr, Pair* out) {
    u32 q = c->gqr[gqr];
    u32 type = (q >> 16) & 7u;
    u32 size = type == 0 ? 4 : (type & 1) ? 2 : 1;
    f64 mul = fast_psq_dequant(q >> 24);
    f64 v0, v1 = 1.0;
    if (!fast_psq_load_one(c, ea, type, mul, &v0) || (!w && !fast_psq_load_one(c, ea + size, type, mul, &v1))) return 0;
    out->a = v0;
    out->b = v1;
    return 1;
}

// psq_st (fast_psq_store_inline), split into a check and a commit so an iteration can be
// abandoned before any of its stores lands.
typedef struct {
    u8* p;
    u32 type, w;
    s32 x0, x1;   // quantized values, or float bits for type 0
} Store;

static inline int st_prep(CPUState* c, u32 ea, int w, int gqr, Pair v, Store* s) {
    u32 q = c->gqr[gqr], type = q & 7u;
    s->type = type;
    s->w = (u32)w;
    if (type >= 4) {
        if (!isfinite(v.a) || !isfinite(v.b)) return 0;
        s32 scale = (s32)((q >> 8) << 26) >> 26;
        f64 mul = fast_f64_value((u64)(1023 + scale) << 52);
        u32 size = (type & 1) ? 2 : 1;
        if (!(s->p = fast_ram_store(c, ea, w ? size : 2 * size))) return 0;
        s32 lo = type == 4 || type == 5 ? 0 : type == 6 ? -128 : -32768;
        s32 hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
        s->x0 = fast_psq_quantize(v.a, mul, lo, hi);
        s->x1 = fast_psq_quantize(v.b, mul, lo, hi);
        return 1;
    }
    if (type != 0 || (ea & 3u)) return 0;
    f32 f0 = (f32)v.a, f1 = (f32)v.b;
    u32 b0, b1;
    memcpy(&b0, &f0, 4);
    memcpy(&b1, &f1, 4);
    if (!fast_f32_plain(b0) || (!w && !fast_f32_plain(b1))) return 0;
    if (!(s->p = fast_ram_store(c, ea, w ? 4 : 8))) return 0;
    s->x0 = (s32)b0;
    s->x1 = (s32)b1;
    return 1;
}

static inline void st_commit(const Store* s) {
    int n = s->w ? 1 : 2;
    for (int i = 0; i < n; i++) {
        s32 x = i ? s->x1 : s->x0;
        switch (s->type) {
        case 0: ((fast_g32*)s->p)[i].v = __builtin_bswap32((u32)x); break;
        case 4: case 6: ((fast_g8*)s->p)[i].v = (u8)x; break;
        default: ((fast_g16*)s->p)[i].v = __builtin_bswap16((u16)x); break;
        }
    }
}

static inline int out2(f64 r0, f64 r1, Pair* d) {   // fast_paired_out
    f32 s0 = (f32)r0, s1 = (f32)r1;
    if (!isfinite(s0) || !isfinite(s1)) return 0;
    d->a = s0;
    d->b = s1;
    return 1;
}

// ps_mul: lane-wise a * c.
static inline int mul(Pair a, Pair c, Pair* d) {
    f64 c0, c1;
    if (!fast_force_25_bit(c.a, &c0) || !fast_force_25_bit(c.b, &c1)) return 0;
    return out2(a.a * c0, a.b * c1, d);
}

// ps_madds0 / ps_madds1: a * scalar + b.
static inline int madds(Pair a, f64 scalar, Pair b, Pair* d) {
    f64 k;
    if (!fast_force_25_bit(scalar, &k)) return 0;
    return out2((f64)(f32)fast_single_fma(a.a, k, b.a), (f64)(f32)fast_single_fma(a.b, k, b.b), d);
}

static inline int add(Pair a, Pair b, Pair* d) { return out2(a.a + b.a, a.b + b.b, d); }
static inline int sub(Pair a, Pair b, Pair* d) { return out2(a.a - b.a, a.b - b.b, d); }

// ps_sel d, a, c, b: c where a >= 0 (compared as single), else b. Never falls back.
static inline Pair sel(Pair a, Pair c, Pair b) {
    return (Pair){(f32)a.a >= 0.0f ? c.a : b.a, (f32)a.b >= 0.0f ? c.b : b.b};
}

// fmuls (fast_fmuls): a * c as single; tiny results go to the library (FPSCR.NI).
static inline int fmuls(f64 a, f64 c, f64* d) {
    f64 y;
    if (!fast_force_25_bit(c, &y)) return 0;
    f64 r = a * y;
    f32 s = (f32)r;
    if (!isfinite(s) || (r != 0.0 && fabs(r) < 0x1p-126)) return 0;
    *d = s;
    return 1;
}

// fmadds (fast_fma, single): a * c + b.
static inline int fmadds(f64 a, f64 c, f64 b, f64* d) {
    f64 k;
    if (!fast_force_25_bit(c, &k)) return 0;
    f64 r = (f64)(f32)fast_single_fma(a, k, b);
    if (!isfinite(r)) return 0;
    *d = r;
    return 1;
}

static inline Pair get(const CPUState* c, int r) { return (Pair){c->fpr[r], c->ps1[r]}; }
static inline void put(CPUState* c, int r, Pair v) { c->fpr[r] = v.a; c->ps1[r] = v.b; }

// State every paired-single fast path needs: FP enabled, default FPSCR mode, psq_l/psq_st enabled.
static int ps_fast_state(const CPUState* c) {
    return (c->msr & 0x2000u) && fast_fp_mode(c) && fast_psq_ok(c, false);
}

static unsigned g_cap = 512;   // loop iterations per call, so interrupts are not delayed for long
static u64 g_why_load, g_why_math, g_why_store;

// ---- fn_800AAC80 lighting loops: second-order spherical harmonics of each vertex normal ----
// Normals: s16 triples through GQR3, read one vertex ahead (f23 = xy, f24 = z, 1). Colors: u8
// through GQR5 (two bytes, then one). f3..f22 hold the coefficients; f0 = 0, f1 = 1, f2 = (0, 1).
// Two copies of the loop that differ only in which registers hold the pointers.
enum { SH_LOOP_A = 0x800AAE68u, SH_EXIT_A = 0x800AAEFCu, SH_LOOP_B = 0x800AAF84u, SH_EXIT_B = 0x800AB018u };

static s64 sh_loop(CPUState* c, int rin, int rout, u32 head, u32 exit) {
    Pair k0 = get(c, 0), k1 = get(c, 1), k2 = get(c, 2), k3 = get(c, 3), k4 = get(c, 4);
    Pair k5 = get(c, 5), k6 = get(c, 6), k7 = get(c, 7), k8 = get(c, 8), k9 = get(c, 9), k10 = get(c, 10);
    Pair k11 = get(c, 11), k12 = get(c, 12), k13 = get(c, 13), k14 = get(c, 14), k15 = get(c, 15), k16 = get(c, 16);
    Pair k17 = get(c, 17), k18 = get(c, 18), k19 = get(c, 19), k20 = get(c, 20), k21 = get(c, 21), k22 = get(c, 22);
    Pair f23 = get(c, 23), f24 = get(c, 24), f25 = get(c, 25), f26 = get(c, 26), f27 = get(c, 27), f28 = get(c, 28);
    Pair f29 = get(c, 29), f30 = get(c, 30);
    u32 in = c->gpr[rin], out = c->gpr[rout], ctr = c->ctr;
    unsigned n = 0;
    int done = 0;
    while (n < g_cap) {
        Pair a29, a30, a25, a26, a27, a28, n23, n24, s27, s28;
        Store sa, sb;
        if (!madds(k17, f23.a, k3, &a29) || !madds(k18, f23.a, k4, &a30) || !mul(f23, f23, &a25) ||
            !madds(k19, f23.b, a29, &a29) || !madds(k20, f23.b, a30, &a30)) { g_why_math++; break; }
        a27 = (Pair){f23.b, f24.a};   // ps_merge10 f27, f23, f24
        if (!madds(k21, f24.a, a29, &a29) || !madds(k22, f24.a, a30, &a30) || !mul(f24, f24, &a26) ||
            !madds(k5, a25.a, a29, &a29) || !madds(k6, a25.a, a30, &a30) || !mul(a27, f23, &a27) ||
            !madds(k7, a25.b, a29, &a29) || !madds(k8, a25.b, a30, &a30) || !madds(k9, a26.a, a29, &a29) ||
            !madds(k10, a26.a, a30, &a30) || !mul(f23, f24, &a28) || !madds(k11, a27.a, a29, &a29) ||
            !madds(k12, a27.a, a30, &a30) || !madds(k15, a27.b, a29, &a29) || !madds(k16, a27.b, a30, &a30) ||
            !madds(k13, a28.a, a29, &a29) || !madds(k14, a28.a, a30, &a30)) { g_why_math++; break; }
        if (!ld(c, in + 6, 0, 3, &n23)) { g_why_load++; break; }
        if (!sub(a29, k1, &s27)) { g_why_math++; break; }
        a29 = sel(a29, a29, k0);
        if (!ld(c, in + 10, 1, 3, &n24)) { g_why_load++; break; }
        if (!sub(a30, k1, &s28)) { g_why_math++; break; }
        a30 = sel(a30, a30, k0);
        a29 = sel(s27, k1, a29);
        a30 = sel(s28, k1, a30);
        if (!add(a30, k2, &a30)) { g_why_math++; break; }
        if (!st_prep(c, out, 0, 5, a29, &sa) || !st_prep(c, out + 2, 1, 5, a30, &sb)) { g_why_store++; break; }
        st_commit(&sa);
        st_commit(&sb);
        f23 = n23, f24 = n24, f25 = a25, f26 = a26, f27 = s27, f28 = s28, f29 = a29, f30 = a30;
        out += 3;
        in += 6;
        n++;
        if (--ctr == 0) { done = 1; break; }
    }
    if (n == 0) return 0;
    put(c, 23, f23), put(c, 24, f24), put(c, 25, f25), put(c, 26, f26), put(c, 27, f27), put(c, 28, f28);
    put(c, 29, f29), put(c, 30, f30);
    c->gpr[rin] = in, c->gpr[rout] = out, c->ctr = ctr;
    c->pc = done ? exit : head;
    return 37 * (s64)n;
}

// ---- fn_800AA068 normal loops: rotate s8 normals (GQR6) by a 3x3 matrix into s16 (GQR3) ----
// Matrix rows: f8 f9 f10 / f11 f12 f13 / f24 f25 f26 (scalars). r4 = normals, r3 = output.
// The first loop does two normals per iteration, the second one.
enum { NRM_LOOP2 = 0x800AA0FCu, NRM_EXIT2 = 0x800AA180u, NRM_LOOP1 = 0x800AA18Cu, NRM_EXIT1 = 0x800AA1D4u };

typedef struct {
    Pair p3, p4;                 // loaded x and z (ps1 = 1.0)
    f64 f0, f1, f2, f5, f6, f7;  // scalar results
    Store s[3];
} Normal;

static inline int normal_one(CPUState* c, u32 in, u32 out, const f64* m, Normal* v) {
    Pair p0;
    if (!ld(c, in, 1, 6, &v->p3) || !ld(c, in + 1, 1, 6, &p0) || !ld(c, in + 2, 1, 6, &v->p4)) { g_why_load++; return 0; }
    f64 x = v->p3.a, y = p0.a, z = v->p4.a;
    if (!fmuls(y, m[3], &v->f2) || !fmuls(y, m[4], &v->f1) || !fmuls(y, m[5], &v->f0) ||
        !fmadds(x, m[0], v->f2, &v->f2) || !fmadds(x, m[1], v->f1, &v->f1) || !fmadds(x, m[2], v->f0, &v->f0) ||
        !fmadds(z, m[6], v->f2, &v->f5) || !fmadds(z, m[7], v->f1, &v->f6) || !fmadds(z, m[8], v->f0, &v->f7)) {
        g_why_math++;
        return 0;
    }
    if (!st_prep(c, out, 1, 3, (Pair){v->f5, v->f5}, &v->s[0]) || !st_prep(c, out + 2, 1, 3, (Pair){v->f6, v->f6}, &v->s[1]) ||
        !st_prep(c, out + 4, 1, 3, (Pair){v->f7, v->f7}, &v->s[2])) {
        g_why_store++;
        return 0;
    }
    return 1;
}

static s64 normal_loop(CPUState* c, int per, u32 head, u32 exit) {
    const f64 m[9] = {c->fpr[8], c->fpr[9], c->fpr[10], c->fpr[11], c->fpr[12], c->fpr[13], c->fpr[24], c->fpr[25], c->fpr[26]};
    u32 r3 = c->gpr[3], r4 = c->gpr[4], ctr = c->ctr;
    Normal last;
    unsigned n = 0;
    int done = 0;
    while (n < g_cap) {
        Normal v[2];
        int ok = 1;
        for (int i = 0; i < per && ok; i++) ok = normal_one(c, r4 + 3 * i, r3 + 6 * i, m, &v[i]);
        if (!ok) break;
        for (int i = 0; i < per; i++)
            for (int k = 0; k < 3; k++) st_commit(&v[i].s[k]);
        last = v[per - 1];
        r4 += 3 * per;
        r3 += 6 * per;
        n++;
        if (--ctr == 0) { done = 1; break; }
    }
    if (n == 0) return 0;
    c->fpr[0] = c->ps1[0] = last.f0, c->fpr[1] = c->ps1[1] = last.f1, c->fpr[2] = c->ps1[2] = last.f2;
    put(c, 3, last.p3), put(c, 4, last.p4);
    c->fpr[5] = c->ps1[5] = last.f5, c->fpr[6] = c->ps1[6] = last.f6, c->fpr[7] = c->ps1[7] = last.f7;
    c->gpr[3] = r3, c->gpr[4] = r4, c->ctr = ctr;
    c->pc = done ? exit : head;
    return (per == 2 ? 33 : 18) * (s64)n;
}

// ---- vertex skinning: fn_800A9C74 / fn_800A9E1C (one bone) and fn_800A9CEC / fn_800A9E94 (blend) ----
// r3 = 10-byte vertex records: s16 x y z through GQR7, u8 blend weight at +6 (GQR5), bone index
// at +7 (one bone) or +8 (blend; 0xFF = none). r6 = bone matrices (64 bytes: rows f0..f5,
// translation f6 f7), cached across vertices by index in r0. r4 = output: floats (12 bytes, GQR0)
// or s16 (6 bytes, GQR4). The blend versions mix toward the result already in the output.
enum {
    SKIN1_F = 0x800A9C74u, SKIN1_F_LOOP = 0x800A9C7Cu, SKIN2_F = 0x800A9CECu, SKIN2_F_LOOP = 0x800A9CF8u,
    SKIN1_Q = 0x800A9E1Cu, SKIN1_Q_LOOP = 0x800A9E24u, SKIN2_Q = 0x800A9E94u, SKIN2_Q_LOOP = 0x800A9EA0u,
};

static inline u32 cmpw_bits(const CPUState* c, u32 a, u32 b) {
    s32 x = (s32)a, y = (s32)b;
    return (x < y ? 8u : x > y ? 4u : 2u) | ((c->xer >> 31) & 1u);
}

static s64 skin(CPUState* c, int blend, int quant, u32 head) {
    u32 out_step = quant ? 6 : 12, out_hi = quant ? 4 : 8, out_gqr = quant ? 4 : 0;
    u32 idx_off = blend ? 8 : 7;
    u32 r0 = 0xFFFFFFFFu, r3 = c->gpr[3], r4 = c->gpr[4], r5 = c->gpr[5], r6 = c->gpr[6], ctr = c->gpr[5];
    if (blend) c->gpr[7] = 0xFF;
    Pair m[8], f8 = get(c, 8), f9 = get(c, 9), f11 = get(c, 11), f12 = get(c, 12), f13 = get(c, 13);
    for (int i = 0; i < 8; i++) m[i] = get(c, i);
    u32 cr0 = c->cr >> 28;
    unsigned n = 0;
    int done = 0;
    while (n < g_cap) {
        const u8* pb = fast_ram(c, r3 + idx_off, 1);
        if (!pb) { g_why_load++; break; }
        u32 b = ((const fast_g8*)pb)->v;
        if (blend && b == 0xFF) {   // no second bone: leave this vertex
            cr0 = cmpw_bits(c, b, 0xFF);
            r5 = b, r3 += 10, r4 += out_step, n++;
            if (--ctr == 0) { done = 1; break; }
            continue;
        }
        u32 n_cr0 = cmpw_bits(c, b, r0), n_r0 = r0, n_r5 = b;
        Pair nm[8];
        memcpy(nm, m, sizeof m);
        if (b != r0) {
            n_r0 = b;
            n_r5 = (b << 6) + r6;
            int ok = 1;
            for (int i = 0; i < 8 && ok; i++) ok = ld(c, n_r5 + 8 * i, 0, 0, &nm[i]);
            if (!ok) { g_why_load++; break; }
        }
        Pair v8, v9, t12, t13, w11 = f11, o8, o9;
        Store sa, sb;
        if (!ld(c, r3, 0, 7, &v8) || !ld(c, r3 + 4, 1, 7, &v9)) { g_why_load++; break; }
        if (!madds(nm[0], v8.a, nm[6], &t12) || !madds(nm[1], v8.a, nm[7], &t13) || !madds(nm[2], v8.b, t12, &t12) ||
            !madds(nm[3], v8.b, t13, &t13) || !madds(nm[4], v9.a, t12, &t12) || !madds(nm[5], v9.a, t13, &t13)) {
            g_why_math++;
            break;
        }
        Pair s8 = t12, s9 = t13;
        if (blend) {
            if (!ld(c, r3 + 6, 1, 5, &w11) || !ld(c, r4, 0, out_gqr, &o8) || !ld(c, r4 + out_hi, 1, out_gqr, &o9)) {
                g_why_load++;
                break;
            }
            if (!sub(o8, t12, &s8) || !sub(o9, t13, &s9) || !madds(s8, w11.a, t12, &s8) || !madds(s9, w11.a, t13, &s9)) {
                g_why_math++;
                break;
            }
        }
        if (!st_prep(c, r4, 0, out_gqr, s8, &sa) || !st_prep(c, r4 + out_hi, 1, out_gqr, s9, &sb)) { g_why_store++; break; }
        st_commit(&sa);
        st_commit(&sb);
        memcpy(m, nm, sizeof m);
        r0 = n_r0, r5 = n_r5, cr0 = n_cr0;
        f8 = blend ? s8 : v8, f9 = blend ? s9 : v9, f11 = w11, f12 = t12, f13 = t13;
        r3 += 10, r4 += out_step, n++;
        if (--ctr == 0) { done = 1; break; }
    }
    for (int i = 0; i < 8; i++) put(c, i, m[i]);
    put(c, 8, f8), put(c, 9, f9), put(c, 12, f12), put(c, 13, f13);
    if (blend) put(c, 11, f11);
    c->gpr[0] = r0, c->gpr[3] = r3, c->gpr[4] = r4, c->gpr[5] = r5, c->ctr = ctr;
    c->cr = (c->cr & 0x0FFFFFFFu) | (cr0 << 28);
    c->pc = done ? c->lr : head;
    return 3 + 22 * (s64)n;   // the entry setup is done either way; the loop head resumes it
}

// ---- table, dispatch and verification ----

typedef struct {
    u32 pc;
    s64 (*run)(CPUState*);
    u32 exit;                                  // pc where a completed call stops (0 = return to lr)
    void (*writes)(const CPUState*, u32*, u32*);   // guest range a completed call may store to
    const char* name;
} Native;

// HLE_VERIFY timing: host time of the native and the generated version of the same calls.
typedef struct { u64 calls, native_ns, generated_ns; } Timing;

static s64 run_sh_a(CPUState* c) { return sh_loop(c, 5, 6, SH_LOOP_A, SH_EXIT_A); }
static s64 run_sh_b(CPUState* c) { return sh_loop(c, 4, 5, SH_LOOP_B, SH_EXIT_B); }

static s64 run_nrm2(CPUState* c) { return normal_loop(c, 2, NRM_LOOP2, NRM_EXIT2); }
static s64 run_nrm1(CPUState* c) { return normal_loop(c, 1, NRM_LOOP1, NRM_EXIT1); }

static s64 run_skin1_f(CPUState* c) { return skin(c, 0, 0, SKIN1_F_LOOP); }
static s64 run_skin2_f(CPUState* c) { return skin(c, 1, 0, SKIN2_F_LOOP); }
static s64 run_skin1_q(CPUState* c) { return skin(c, 0, 1, SKIN1_Q_LOOP); }
static s64 run_skin2_q(CPUState* c) { return skin(c, 1, 1, SKIN2_Q_LOOP); }

static void writes_skin_f(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[4]; *len = c->gpr[5] > (1u << 20) ? 0 : 12 * c->gpr[5]; }
static void writes_skin_q(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[4]; *len = c->gpr[5] > (1u << 20) ? 0 : 6 * c->gpr[5]; }

static void writes_sh_a(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[6]; *len = c->ctr > (1u << 20) ? 0 : 3 * c->ctr; }
static void writes_sh_b(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[5]; *len = c->ctr > (1u << 20) ? 0 : 3 * c->ctr; }

static void writes_nrm2(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[3]; *len = c->ctr > (1u << 20) ? 0 : 12 * c->ctr; }
static void writes_nrm1(const CPUState* c, u32* lo, u32* len) { *lo = c->gpr[3]; *len = c->ctr > (1u << 20) ? 0 : 6 * c->ctr; }

static const Native g_natives[] = {
    {SH_LOOP_A, run_sh_a, SH_EXIT_A, writes_sh_a, "lighting A"},
    {SH_LOOP_B, run_sh_b, SH_EXIT_B, writes_sh_b, "lighting B"},
    {NRM_LOOP2, run_nrm2, NRM_EXIT2, writes_nrm2, "normals x2"},
    {NRM_LOOP1, run_nrm1, NRM_EXIT1, writes_nrm1, "normals x1"},
    {SKIN1_F, run_skin1_f, 0, writes_skin_f, "skin 1 bone, float"},
    {SKIN2_F, run_skin2_f, 0, writes_skin_f, "skin blend, float"},
    {SKIN1_Q, run_skin1_q, 0, writes_skin_q, "skin 1 bone, s16"},
    {SKIN2_Q, run_skin2_q, 0, writes_skin_q, "skin blend, s16"},
};

static Timing g_timing[sizeof g_natives / sizeof g_natives[0]];

static int g_mode = -1;   // 0 off, 1 on, 2 verify
static u64 g_calls, g_checked, g_mismatches, g_declined, g_partial;

typedef void (*ChunkFn)(CPUState*);
static ChunkFn (*g_chunk_for)(u32);

void hle_init(ChunkFn (*chunk_for)(u32)) {
    g_chunk_for = chunk_for;
    g_mode = getenv("NO_HLE") ? 0 : getenv("HLE_VERIFY") ? 2 : 1;
    g_hle_points_on = g_mode > 0;
    if (g_mode == 2) g_cap = ~0u;   // verify whole calls
}

static const Native* find(u32 pc) {
    for (size_t i = 0; i < sizeof g_natives / sizeof g_natives[0]; i++)
        if (g_natives[i].pc == pc) return &g_natives[i];
    return NULL;
}

static int same_state(const CPUState* a, const CPUState* b) {
    return !memcmp(a->gpr, b->gpr, sizeof a->gpr) && !memcmp(a->fpr, b->fpr, sizeof a->fpr) &&
           !memcmp(a->ps1, b->ps1, sizeof a->ps1) && a->ctr == b->ctr && a->cr == b->cr && a->lr == b->lr &&
           a->pc == b->pc && a->fpscr == b->fpscr && a->xer == b->xer;
}

__attribute__((noinline)) static s64 verify(CPUState* c, const Native* nat) {
    u32 lo, len;
    nat->writes(c, &lo, &len);
    u8* mem = len ? fast_ram(c, lo, len) : NULL;
    if (!mem) return ps_fast_state(c) ? nat->run(c) : 0;
    u32 stop = nat->exit ? nat->exit : c->lr;
    CPUState before = *c;
    u8* saved = malloc(len);
    memcpy(saved, mem, len);
    u64 t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    s64 cycles = ps_fast_state(c) ? nat->run(c) : 0;
    u64 t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!cycles || c->pc != stop) {   // not handled, or stopped early: nothing to compare
        free(saved);
        return cycles;
    }
    CPUState native = *c;
    u8* native_mem = malloc(len);
    memcpy(native_mem, mem, len);
    // Re-run the same call with the generated code from the saved state, stopping at the exit point.
    memcpy(mem, saved, len);
    *c = before;
    g_hle_skip = before.pc;
    u64 t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (c->pc != stop && !c->exception) {
        c->downcount = 1 << 30;
        g_chunk_for(c->pc)(c);
        g_hle_skip = c->pc;
    }
    Timing* tm = &g_timing[nat - g_natives];
    tm->calls++;
    tm->native_ns += t1 - t0;
    tm->generated_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t2;
    g_checked++;
    int ok = same_state(c, &native) && !memcmp(mem, native_mem, len);
    if (!ok && g_mismatches++ < 10) {
        fprintf(stderr, "HLE_VERIFY mismatch at %08X (lr %08X, %u bytes out)\n", before.pc, before.lr, len);
        for (int r = 0; r < 32; r++) {
            if (c->gpr[r] != native.gpr[r])
                fprintf(stderr, "  r%d: generated %08X native %08X\n", r, c->gpr[r], native.gpr[r]);
            if (memcmp(&c->fpr[r], &native.fpr[r], 8) || memcmp(&c->ps1[r], &native.ps1[r], 8))
                fprintf(stderr, "  f%d: generated (%a, %a) native (%a, %a)\n", r, c->fpr[r], c->ps1[r], native.fpr[r],
                        native.ps1[r]);
        }
        if (c->ctr != native.ctr || c->cr != native.cr || c->pc != native.pc)
            fprintf(stderr, "  ctr %08X/%08X cr %08X/%08X pc %08X/%08X\n", c->ctr, native.ctr, c->cr, native.cr, c->pc,
                    native.pc);
        for (u32 i = 0; i < len; i++)
            if (mem[i] != native_mem[i]) {
                fprintf(stderr, "  mem %08X: generated %02X native %02X\n", lo + i, mem[i], native_mem[i]);
                break;
            }
    }
    free(saved);
    free(native_mem);
    return cycles;   // the generated result stays; charge the same time
}

s64 hle_try(CPUState* c) {
    if (g_mode <= 0) return 0;
    const Native* nat = find(c->pc);
    if (!nat) return 0;
    g_calls++;
    u32 stop = nat->exit ? nat->exit : c->lr;
    s64 cycles = g_mode == 2 ? verify(c, nat) : ps_fast_state(c) ? nat->run(c) : 0;
    if (!cycles) g_declined++;
    else if (c->pc != stop) g_partial++;
    return cycles;
}

void hle_report(void) {
    if (g_mode <= 0) return;
    printf("HLE: %llu calls, %llu declined, %llu stopped early (load %llu, math %llu, store %llu)\n", g_calls,
           g_declined, g_partial, g_why_load, g_why_math, g_why_store);
    if (g_mode != 2) return;
    printf("HLE verify: %llu compared, %llu mismatches\n", g_checked, g_mismatches);
    for (size_t i = 0; i < sizeof g_natives / sizeof g_natives[0]; i++) {
        const Timing* t = &g_timing[i];
        if (t->calls)
            printf("  %-26s %9llu calls  native %6.0f ns  generated %6.0f ns  (%.1fx, %.2f s saved)\n", g_natives[i].name,
                   t->calls, (double)t->native_ns / t->calls, (double)t->generated_ns / t->calls,
                   (double)t->generated_ns / (t->native_ns ? t->native_ns : 1), (t->generated_ns - (double)t->native_ns) / 1e9);
    }
}
