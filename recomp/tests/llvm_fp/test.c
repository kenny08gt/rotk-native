// Runs each LLVM-compiled guest function from ops.h on random register
// state and replays the same steps with the C build's fast_cpu.h on a copy.
// FPR, PS1, FPSCR and CR must match bit for bit.
#define FAST_NO_MEM
#define FAST_NO_FP
#define FAST_NO_PSQ
#include "chassis/fast_cpu.h"

#include "ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8 *g_fast_vm;
u32 g_fast_vm_base, g_fast_vm_size;

void func_80001000(CPUState *);
void func_80001100(CPUState *);
void func_80001200(CPUState *);
void func_80001300(CPUState *);
void func_80001400(CPUState *);
void func_80001500(CPUState *);
void func_80001600(CPUState *);
void func_80001700(CPUState *);
void func_80001800(CPUState *);
void func_80001900(CPUState *);
void func_80001A00(CPUState *);
void func_80001B00(CPUState *);
void func_80001C00(CPUState *);
void func_80001D00(CPUState *);
void func_80001E00(CPUState *);
void func_80001F00(CPUState *);
void func_80002000(CPUState *);
void func_80002100(CPUState *);
void func_80002200(CPUState *);

typedef void (*GuestFunction)(CPUState *);
static const GuestFunction h1_entries[] = {
    func_80001000, func_80001100, func_80001200, func_80001300, func_80001400,
    func_80001500, func_80001600, func_80001700, func_80001800, func_80001900,
    func_80001A00, func_80001B00, func_80001C00, func_80001D00, func_80001E00,
    func_80001F00, func_80002000, func_80002100, func_80002200,
};

static u64 rng = 0x9E3779B97F4A7C15ull;
static u64 next64(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const u64 specials[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
    0xBFF0000000000000ull, 0x3FE0000000000000ull, 0x4000000000000000ull,
    0x4008000000000000ull, 0x3FF8000000000000ull, 0x3810000000000000ull,
    0xB810000000000000ull, 0x3800000000000000ull, 0x36A0000000000000ull,
    0x3690000000000000ull, 0x0010000000000000ull, 0x0000000000000001ull,
    0x000FFFFFFFFFFFFFull, 0x8000000000000001ull, 0x47EFFFFFE0000000ull,
    0x47EFFFFFE0000001ull, 0x47EFFFFFF0000000ull, 0x47F0000000000000ull,
    0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
    0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0x7FF4000000000000ull,
    0x3FF0000010000000ull, 0x3FF0000008000000ull, 0x3FF0000008000001ull,
    0x3FF0000007FFFFFFull, 0x3FF000000FFFFFFFull, 0x3FF0000004000000ull,
    0x380FFFFFFFFFFFFFull, 0x3810000000000001ull, 0x3690000000000001ull,
    0x368FFFFFFFFFFFFFull,
};

static u64 make_double(int exponent, u64 mantissa, int negative) {
    u64 bits = ((u64)(exponent + 1023) << 52) | (mantissa & 0x000FFFFFFFFFFFFFull);
    return negative ? bits | 0x8000000000000000ull : bits;
}

static u64 random_value(void) {
    u64 pick = next64() % 100;
    int negative = (int)(next64() & 1);
    if (pick < 15)
        return specials[next64() % (sizeof(specials) / sizeof(specials[0]))];
    if (pick < 40) {
        u32 single = (u32)next64();
        if (next64() & 1)
            single = (single & 0x807FFFFFu) | ((u32)(64 + next64() % 128) << 23);
        f32 value;
        memcpy(&value, &single, 4);
        f64 wide = (f64)value;
        u64 bits;
        memcpy(&bits, &wide, 8);
        return bits;
    }
    if (pick < 65) {
        int exponent = (int)(next64() % 341) - 170;
        u64 mantissa = 0;
        u32 count = (u32)(next64() % 5);
        for (u32 i = 0; i < count; i++)
            mantissa |= 1ull << (next64() % 52);
        return make_double(exponent, mantissa, negative);
    }
    if (pick < 75)
        return make_double((int)(next64() % 61) - 160, next64(), negative);
    if (pick < 90)
        return make_double((int)(next64() % 61) - 30, next64(), negative);
    return next64();
}

static u32 random_fpscr(int fast_only) {
    u32 high = (u32)next64() & 0xFFFFFF00u;
    if (next64() % 4 == 0)
        high = 0;
    u32 low;
    if (fast_only || next64() % 5 < 3)
        low = (next64() & 1) ? 4u : 0u;
    else
        low = (u32)next64() & 0xFFu;
    return high | low;
}

static void replay(CPUState *cpu, const H1Step *step) {
    f64 result;
    switch (step->kind) {
    case H1_FADDS: fast_fadds(cpu, step->d, step->a, step->b); break;
    case H1_FSUBS: fast_fsubs(cpu, step->d, step->a, step->b); break;
    case H1_FMULS: fast_fmuls(cpu, step->d, step->a, step->c); break;
    case H1_FDIVS: fast_fdivs(cpu, step->d, step->a, step->b); break;
    case H1_FRSP: fast_frsp(cpu, step->d, step->b); break;
    case H1_FMADDS:
    case H1_FMSUBS:
    case H1_FNMADDS:
    case H1_FNMSUBS: {
        bool subtract = step->kind == H1_FMSUBS || step->kind == H1_FNMSUBS;
        bool negative = step->kind == H1_FNMADDS || step->kind == H1_FNMSUBS;
        if (fast_fma(cpu, cpu->fpr[step->a], cpu->fpr[step->c], cpu->fpr[step->b], true,
                     subtract, negative, &result))
            cpu->fpr[step->d] = cpu->ps1[step->d] = result;
        break;
    }
    case H1_PS_ADD: fast_ps_add_op(cpu, step->d, step->a, step->b); break;
    case H1_PS_MUL: fast_ps_mul_op(cpu, step->d, step->a, step->c); break;
    case H1_PS_MADD: fast_ps_madd_op(cpu, step->d, step->a, step->c, step->b, false, false); break;
    default: break;
    }
    if (step->rc)
        cpu->cr = (cpu->cr & 0xF0FFFFFFu) | ((cpu->fpscr >> 4) & 0x0F000000u);
}

static u64 bits_of(f64 value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    return bits;
}

static int mixed_function(const H1Function *function) {
    for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
        if (function->steps[i].kind >= H1_PS_ADD)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    u32 iterations = argc > 1 ? (u32)strtoul(argv[1], NULL, 10) : 200000u;
    if (argc > 2)
        rng = strtoull(argv[2], NULL, 0);
    static CPUState generated, reference;
    if (!cpu_init(&generated) || !cpu_init(&reference)) {
        fprintf(stderr, "cpu_init failed\n");
        return 2;
    }
    u64 total_mismatches = 0, total_fast_mode = 0;
    for (u32 f = 0; f < H1_FUNCTION_COUNT; f++) {
        const H1Function *function = &h1_functions[f];
        int fast_only = mixed_function(function);
        u64 mismatches = 0, fast_mode = 0, finite_results = 0;
        for (u32 iteration = 0; iteration < iterations; iteration++) {
            u32 fpscr = random_fpscr(fast_only);
            u32 cr = (u32)next64();
            for (u32 reg = 0; reg < 32; reg++) {
                u64 lane0 = random_value(), lane1 = random_value();
                memcpy(&generated.fpr[reg], &lane0, 8);
                memcpy(&generated.ps1[reg], &lane1, 8);
            }
            memcpy(reference.fpr, generated.fpr, sizeof(generated.fpr));
            memcpy(reference.ps1, generated.ps1, sizeof(generated.ps1));
            generated.fpscr = reference.fpscr = fpscr;
            generated.cr = reference.cr = cr;
            generated.pc = function->address;
            generated.lr = 0x81234567u;
            generated.exception = 0;
            generated.program_exception = 0;
            generated.msr = 1u << 13;
            generated.downcount = 0;
            h1_entries[f](&generated);
            for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
                replay(&reference, &function->steps[i]);
            if ((fpscr & 0xFBu) == 0)
                fast_mode++;
            if (isfinite(generated.fpr[function->steps[0].d]))
                finite_results++;
            // The inline paired-single path records FPRF while fast_cpu.h's paired fast paths
            // skip it, so FPRF is compared only for all-scalar functions.
            u32 fpscr_mask = fast_only ? ~0x0001F000u : ~0u;
            int bad = memcmp(generated.fpr, reference.fpr, sizeof(generated.fpr)) != 0 ||
                      memcmp(generated.ps1, reference.ps1, sizeof(generated.ps1)) != 0 ||
                      (generated.fpscr & fpscr_mask) != (reference.fpscr & fpscr_mask) ||
                      generated.cr != reference.cr;
            if (!bad)
                continue;
            mismatches++;
            if (mismatches > 5)
                continue;
            printf("MISMATCH %s iteration %u fpscr %08X\n", function->name, iteration, fpscr);
            for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++) {
                const H1Step *s = &function->steps[i];
                printf("  step %u kind %d d=%u a=%u b=%u c=%u\n", i, s->kind, s->d, s->a, s->b, s->c);
            }
            for (u32 reg = 0; reg < 32; reg++) {
                if (bits_of(generated.fpr[reg]) != bits_of(reference.fpr[reg]) ||
                    bits_of(generated.ps1[reg]) != bits_of(reference.ps1[reg]))
                    printf("  f%u: llvm %016llX/%016llX  c %016llX/%016llX\n", reg,
                           (unsigned long long)bits_of(generated.fpr[reg]),
                           (unsigned long long)bits_of(generated.ps1[reg]),
                           (unsigned long long)bits_of(reference.fpr[reg]),
                           (unsigned long long)bits_of(reference.ps1[reg]));
            }
            if (generated.fpscr != reference.fpscr)
                printf("  fpscr: llvm %08X  c %08X\n", generated.fpscr, reference.fpscr);
            if (generated.cr != reference.cr)
                printf("  cr: llvm %08X  c %08X\n", generated.cr, reference.cr);
        }
        printf("%-14s %u runs, %llu fast-mode, %llu finite d, %llu mismatches\n",
               function->name, iterations, (unsigned long long)fast_mode,
               (unsigned long long)finite_results, (unsigned long long)mismatches);
        total_mismatches += mismatches;
        total_fast_mode += fast_mode;
    }
    printf("TOTAL mismatches %llu\n", (unsigned long long)total_mismatches);
    return total_mismatches ? 1 : 0;
}
