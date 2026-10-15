// Differential test: chassis/fast_cpu.h fast paths vs the exact DolRecomp library helpers.
// Build: see recomp/tests/run_fast_cpu_diff.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../chassis/fast_cpu.h"
// The header renames the library helpers; undo that so the A side calls the real ones.
#undef mem_read8
#undef mem_read16
#undef mem_read32
#undef mem_read64
#undef mem_write8
#undef mem_write16
#undef mem_write32
#undef mem_write64
#undef ppc_fadds
#undef ppc_fsubs
#undef ppc_fmuls
#undef ppc_fdivs
#undef ppc_frsp
#undef ppc_fadd
#undef ppc_fsub
#undef ppc_fmul
#undef ppc_fdiv
#undef ppc_fma
#undef ppc_fcmp
#undef ppc_ps_add_op
#undef ppc_ps_sub_op
#undef ppc_ps_mul_op
#undef ppc_ps_muls0
#undef ppc_ps_muls1
#undef ppc_ps_madd_op
#undef ppc_ps_madds0
#undef ppc_ps_madds1
#undef ppc_ps_sum0
#undef ppc_ps_sum1
#undef ppc_psq_load_inline
#undef ppc_psq_store_inline

static u64 rng = 0x9E3779B97F4A7C15ull;
static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static f64 rnd_f64(void) {
    switch (rnd() % 6) {
    case 0: return (f64)(s32)(rnd() % 2000) - 1000;
    case 1: return (f64)(f32)((f64)(s64)rnd() / 9e18 * 300.0);
    case 2: return (f64)(f32)((f64)(s64)rnd() / 9e18);
    case 3: return 0.0;
    case 4: { u32 b = (u32)rnd(); f32 f; memcpy(&f, &b, 4); return isfinite(f) ? (f64)f : 1.5; }
    default: return (f64)(s64)rnd() / 9e18 * 1e6;
    }
}

static int fails;
static void check(const char* what, int i, const CPUState* a, const CPUState* b, const u8* ra, const u8* rb, u32 n) {
    if (memcmp(a->fpr, b->fpr, sizeof a->fpr) || memcmp(a->ps1, b->ps1, sizeof a->ps1) || memcmp(ra, rb, n) ||
        a->cr != b->cr) {
        if (fails++ < 20) {
            printf("MISMATCH %s iter %d\n", what, i);
            for (int r = 0; r < 32; r++)
                if (a->fpr[r] != b->fpr[r] || a->ps1[r] != b->ps1[r])
                    printf("  f%d lib=(%a,%a) fast=(%a,%a)\n", r, a->fpr[r], a->ps1[r], b->fpr[r], b->ps1[r]);
            for (u32 k = 0; k < n; k++)
                if (ra[k] != rb[k]) { printf("  ram[%u] lib=%02X fast=%02X\n", k, ra[k], rb[k]); break; }
        }
    }
}

int main(void) {
    static CPUState A, B;
    cpu_init(&A);
    cpu_init(&B);
    A.hid2 = B.hid2 = 0xE0000000u;
    A.msr = B.msr = 0x2000;
    const u32 N = 4096;
    for (int i = 0; i < 200000; i++) {
        for (u32 k = 0; k < N; k++) A.ram[k] = (u8)rnd();
        memcpy(B.ram, A.ram, N);
        for (int r = 0; r < 32; r++) { A.fpr[r] = B.fpr[r] = rnd_f64(); A.ps1[r] = B.ps1[r] = rnd_f64(); }
        u32 gq = (u32)rnd();
        // keep type in {0,4..7}
        u32 lt = (u32[]){0, 4, 5, 6, 7}[rnd() % 5], st = (u32[]){0, 4, 5, 6, 7}[rnd() % 5];
        gq = (gq & ~0x00070007u) | (lt << 16) | st;
        for (int g = 0; g < 8; g++) A.gqr[g] = B.gqr[g] = gq;
        A.fpscr = B.fpscr = (rnd() & 1) ? 0x4 : 0;
        A.cr = B.cr = 0;
        u8 d = rnd() % 32, a = rnd() % 32, b = rnd() % 32, c = rnd() % 32;
        u32 ea = 0x80000000u + (u32)(rnd() % (N - 16));
        bool w = rnd() & 1;
        int op = rnd() % 22;
        const char* name = "?";
        switch (op) {
#define BOTH(nm, lib, fast) name = nm; lib; fast; break
        case 0: BOTH("psq_l", ppc_psq_load(&A, d, ea, w, 1, false, 0), fast_psq_load_inline(&B, d, ea, w, 1, false, 0));
        case 1: BOTH("psq_st", ppc_psq_store(&A, d, ea, w, 1, false, 0), fast_psq_store_inline(&B, d, ea, w, 1, false, 0));
        case 2: BOTH("fadds", ppc_fadds(&A, d, a, b), fast_fadds(&B, d, a, b));
        case 3: BOTH("fsubs", ppc_fsubs(&A, d, a, b), fast_fsubs(&B, d, a, b));
        case 4: BOTH("fmuls", ppc_fmuls(&A, d, a, c), fast_fmuls(&B, d, a, c));
        case 5: BOTH("fdivs", ppc_fdivs(&A, d, a, b), fast_fdivs(&B, d, a, b));
        case 6: BOTH("frsp", ppc_frsp(&A, d, b), fast_frsp(&B, d, b));
        case 7: BOTH("fadd", ppc_fadd(&A, d, a, b), fast_fadd(&B, d, a, b));
        case 8: BOTH("fmul", ppc_fmul(&A, d, a, c), fast_fmul(&B, d, a, c));
        case 9: BOTH("fdiv", ppc_fdiv(&A, d, a, b), fast_fdiv(&B, d, a, b));
        case 10: {
            name = "fma";
            bool s = rnd() & 1, sub = rnd() & 1, neg = rnd() & 1;
            f64 ra = 0, rb = 0;
            bool ka = ppc_fma(&A, A.fpr[a], A.fpr[c], A.fpr[b], s, sub, neg, &ra);
            bool kb = fast_fma(&B, B.fpr[a], B.fpr[c], B.fpr[b], s, sub, neg, &rb);
            if (ka) A.fpr[d] = A.ps1[d] = ra;
            if (kb) B.fpr[d] = B.ps1[d] = rb;
            break;
        }
        case 11: BOTH("fcmp", ppc_fcmp(&A, d % 8, A.fpr[a], A.fpr[b], true), fast_fcmp(&B, d % 8, B.fpr[a], B.fpr[b], true));
        case 12: BOTH("ps_add", ppc_ps_add_op(&A, d, a, b), fast_ps_add_op(&B, d, a, b));
        case 13: BOTH("ps_sub", ppc_ps_sub_op(&A, d, a, b), fast_ps_sub_op(&B, d, a, b));
        case 14: BOTH("ps_mul", ppc_ps_mul_op(&A, d, a, c), fast_ps_mul_op(&B, d, a, c));
        case 15: BOTH("ps_muls0", ppc_ps_muls0(&A, d, a, c), fast_ps_muls0(&B, d, a, c));
        case 16: BOTH("ps_muls1", ppc_ps_muls1(&A, d, a, c), fast_ps_muls1(&B, d, a, c));
        case 17: { bool s = rnd() & 1, n = rnd() & 1; BOTH("ps_madd", ppc_ps_madd_op(&A, d, a, c, b, s, n), fast_ps_madd_op(&B, d, a, c, b, s, n)); }
        case 18: BOTH("ps_madds0", ppc_ps_madds0(&A, d, a, c, b), fast_ps_madds0(&B, d, a, c, b));
        case 19: BOTH("ps_madds1", ppc_ps_madds1(&A, d, a, c, b), fast_ps_madds1(&B, d, a, c, b));
        case 20: BOTH("ps_sum0", ppc_ps_sum0(&A, d, a, c, b), fast_ps_sum0(&B, d, a, c, b));
        case 21: {
            name = "mem";
            u32 v = (u32)rnd();
            int sz = 1 << (rnd() % 4);
            switch (sz) {
            case 1: A.fpr[d] = mem_read8(&A, ea); mem_write8(&A, ea + 1, (u8)v); B.fpr[d] = fast_read8(&B, ea); fast_write8(&B, ea + 1, (u8)v); break;
            case 2: A.fpr[d] = mem_read16(&A, ea); mem_write16(&A, ea + 2, (u16)v); B.fpr[d] = fast_read16(&B, ea); fast_write16(&B, ea + 2, (u16)v); break;
            case 4: A.fpr[d] = mem_read32(&A, ea); mem_write32(&A, ea + 4, v); B.fpr[d] = fast_read32(&B, ea); fast_write32(&B, ea + 4, v); break;
            default: A.fpr[d] = (f64)mem_read64(&A, ea); mem_write64(&A, ea + 8, v * 3ull); B.fpr[d] = (f64)fast_read64(&B, ea); fast_write64(&B, ea + 8, v * 3ull); break;
            }
            break;
        }
        }
        check(name, i, &A, &B, A.ram, B.ram, N);
    }
    printf("%d mismatches\n", fails);
    return fails != 0;
}
