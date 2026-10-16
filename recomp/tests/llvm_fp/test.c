// Runs each LLVM-compiled guest function from ops.h on random register
// state and replays the same steps with the C build's fast_cpu.h on a copy
// (FPR, PS1, FPSCR and CR must match bit for bit) and with the exact library
// helpers on another (FPR, PS1 and CR must match). psq_l/psq_lu steps load random 32-bit
// patterns from guest RAM through an unquantized (type 0) GQR and replay with
// fast_psq_load_inline and ppc_psq_load; GPRs are compared too.
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
void func_80002300(CPUState *);
void func_80002400(CPUState *);
void func_80002500(CPUState *);
void func_80002600(CPUState *);
void func_80002700(CPUState *);
void func_80002800(CPUState *);
void func_80002900(CPUState *);
void func_80002A00(CPUState *);
void func_80002B00(CPUState *);
void func_80002C00(CPUState *);
void func_80002D00(CPUState *);
void func_80002E00(CPUState *);
void func_80002F00(CPUState *);
void func_80003000(CPUState *);
void func_80003100(CPUState *);
void func_80003200(CPUState *);
void func_80003300(CPUState *);
void func_80003400(CPUState *);
void func_80003500(CPUState *);
void func_80003600(CPUState *);
void func_80003700(CPUState *);
void func_80003800(CPUState *);
void func_80003900(CPUState *);
void func_80003A00(CPUState *);
void func_80003B00(CPUState *);

typedef void (*GuestFunction)(CPUState *);
static const GuestFunction h1_entries[] = {
    func_80001000,
    func_80001100,
    func_80001200,
    func_80001300,
    func_80001400,
    func_80001500,
    func_80001600,
    func_80001700,
    func_80001800,
    func_80001900,
    func_80001A00,
    func_80001B00,
    func_80001C00,
    func_80001D00,
    func_80001E00,
    func_80001F00,
    func_80002000,
    func_80002100,
    func_80002200,
    func_80002300,
    func_80002400,
    func_80002500,
    func_80002600,
    func_80002700,
    func_80002800,
    func_80002900,
    func_80002A00,
    func_80002B00,
    func_80002C00,
    func_80002D00,
    func_80002E00,
    func_80002F00,
    func_80003000,
    func_80003100,
    func_80003200,
    func_80003300,
    func_80003400,
    func_80003500,
    func_80003600,
    func_80003700,
    func_80003800,
    func_80003900,
    func_80003A00,
    func_80003B00,
};
_Static_assert(sizeof(h1_entries) / sizeof(h1_entries[0]) == H1_FUNCTION_COUNT,
               "one entry per ops.h function");

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
    if (pick < 85)
        return make_double((int)(next64() % 61) - 30, next64(), negative);
    if (pick < 92) {
        int exponent = (next64() & 1) ? 960 + (int)(next64() % 63) : -1022 + (int)(next64() % 62);
        return make_double(exponent, next64(), negative);
    }
    return next64();
}

static const u32 specials32[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
    0x00400000u, 0x00000100u, 0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
    0x7FFFFFFFu, 0xFFFFFFFFu, 0x7F800001u, 0xFF800001u, 0x7FA00000u, 0x7FBFFFFFu,
    0xFFBFFFFFu, 0x7F812345u,
};

static u32 random_single_bits(void) {
    u64 pick = next64() % 100;
    u32 bits = (u32)next64();
    if (pick < 40)
        return specials32[next64() % (sizeof(specials32) / sizeof(specials32[0]))];
    if (pick < 55)
        return bits & 0x807FFFFFu;
    if (pick < 70)
        return bits | 0x7F800000u;
    return bits;
}

#define PSQ_WINDOW 0x00100000u
#define PSQ_WINDOW_SIZE 256u

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

// library != 0 replays with the exact library helpers instead of fast_cpu.h's fast paths, the way
// the generated slow arms call them.
static void replay(CPUState *cpu, const H1Step *step, int library) {
    f64 result;
    bool subtract = step->kind == H1_FMSUBS || step->kind == H1_FNMSUBS ||
                    step->kind == H1_FMSUB || step->kind == H1_FNMSUB;
    bool negative = step->kind == H1_FNMADDS || step->kind == H1_FNMSUBS ||
                    step->kind == H1_FNMADD || step->kind == H1_FNMSUB;
    switch (step->kind) {
    case H1_FADDS: library ? ppc_fadds(cpu, step->d, step->a, step->b) : fast_fadds(cpu, step->d, step->a, step->b); break;
    case H1_FSUBS: library ? ppc_fsubs(cpu, step->d, step->a, step->b) : fast_fsubs(cpu, step->d, step->a, step->b); break;
    case H1_FMULS: library ? ppc_fmuls(cpu, step->d, step->a, step->c) : fast_fmuls(cpu, step->d, step->a, step->c); break;
    case H1_FDIVS: library ? ppc_fdivs(cpu, step->d, step->a, step->b) : fast_fdivs(cpu, step->d, step->a, step->b); break;
    case H1_FRSP: library ? ppc_frsp(cpu, step->d, step->b) : fast_frsp(cpu, step->d, step->b); break;
    case H1_FADD: library ? ppc_fadd(cpu, step->d, step->a, step->b) : fast_fadd(cpu, step->d, step->a, step->b); break;
    case H1_FSUB: library ? ppc_fsub(cpu, step->d, step->a, step->b) : fast_fsub(cpu, step->d, step->a, step->b); break;
    case H1_FMUL: library ? ppc_fmul(cpu, step->d, step->a, step->c) : fast_fmul(cpu, step->d, step->a, step->c); break;
    case H1_FDIV: library ? ppc_fdiv(cpu, step->d, step->a, step->b) : fast_fdiv(cpu, step->d, step->a, step->b); break;
    case H1_FMADDS:
    case H1_FMSUBS:
    case H1_FNMADDS:
    case H1_FNMSUBS:
    case H1_FMADD:
    case H1_FMSUB:
    case H1_FNMADD:
    case H1_FNMSUB: {
        bool single = step->kind <= H1_FNMSUBS;
        f64 a = cpu->fpr[step->a], c = cpu->fpr[step->c], b = cpu->fpr[step->b];
        bool ok = library ? ppc_fma(cpu, a, c, b, single, subtract, negative, &result)
                          : fast_fma(cpu, a, c, b, single, subtract, negative, &result);
        if (ok) {
            cpu->fpr[step->d] = result;
            if (single)
                cpu->ps1[step->d] = result;
        }
        break;
    }
    case H1_FCMPU:
    case H1_FCMPO: {
        f64 a = cpu->fpr[step->a], b = cpu->fpr[step->b];
        bool ordered = step->kind == H1_FCMPO;
        library ? ppc_fcmp(cpu, step->d, a, b, ordered) : fast_fcmp(cpu, step->d, a, b, ordered);
        break;
    }
    case H1_PS_ADD: fast_ps_add_op(cpu, step->d, step->a, step->b); break;
    case H1_PS_MUL: fast_ps_mul_op(cpu, step->d, step->a, step->c); break;
    case H1_PS_MADD: fast_ps_madd_op(cpu, step->d, step->a, step->c, step->b, false, false); break;
    case H1_PSQ_L:
    case H1_PSQ_LU: {
        u32 ea = cpu->gpr[step->a] + step->b;
        bool w = step->c >> 3;
        u8 gqr = step->c & 7;
        bool ok = library ? ppc_psq_load(cpu, step->d, ea, w, gqr, false, 0)
                          : fast_psq_load_inline(cpu, step->d, ea, w, gqr, false, 0);
        if (ok && step->kind == H1_PSQ_LU)
            cpu->gpr[step->a] = ea;
        break;
    }
    case H1_LI: cpu->gpr[step->d] = step->b; break;
    case H1_MTGQR: cpu->gqr[step->c] = cpu->gpr[step->d]; break;
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

static int psq_function(const H1Function *function) {
    for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
        if (function->steps[i].kind == H1_PSQ_L || function->steps[i].kind == H1_PSQ_LU)
            return 1;
    return 0;
}

static int mixed_function(const H1Function *function) {
    for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
        if (function->steps[i].kind >= H1_PS_ADD && function->steps[i].kind <= H1_PS_MADD)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    u32 iterations = argc > 1 ? (u32)strtoul(argv[1], NULL, 10) : 200000u;
    if (argc > 2)
        rng = strtoull(argv[2], NULL, 0);
    static CPUState generated, reference, library;
    if (!cpu_init(&generated) || !cpu_init(&reference) || !cpu_init(&library)) {
        fprintf(stderr, "cpu_init failed\n");
        return 2;
    }
    u64 total_mismatches = 0, total_fast_mode = 0;
    for (u32 f = 0; f < H1_FUNCTION_COUNT; f++) {
        const H1Function *function = &h1_functions[f];
        int fast_only = mixed_function(function);
        int psq = psq_function(function);
        u64 mismatches = 0, library_mismatches = 0, fast_mode = 0, finite_results = 0;
        int has_rc = 0;
        for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
            has_rc |= function->steps[i].rc;
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
            memcpy(library.fpr, generated.fpr, sizeof(generated.fpr));
            memcpy(library.ps1, generated.ps1, sizeof(generated.ps1));
            for (u32 reg = 0; reg < 32; reg++)
                generated.gpr[reg] = (u32)next64();
            for (u32 i = 0; i < 8; i++)
                generated.gqr[i] = (u32)next64() & ~0x00070000u;
            if (psq) {
                for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
                    if (function->steps[i].kind == H1_PSQ_L || function->steps[i].kind == H1_PSQ_LU)
                        generated.gpr[function->steps[i].a] =
                            GC_RAM_BASE + PSQ_WINDOW + (u32)(next64() % 32) * 4u;
                for (u32 offset = 0; offset < PSQ_WINDOW_SIZE; offset += 4) {
                    u32 word = __builtin_bswap32(random_single_bits());
                    memcpy(generated.ram + PSQ_WINDOW + offset, &word, 4);
                }
                memcpy(reference.ram + PSQ_WINDOW, generated.ram + PSQ_WINDOW, PSQ_WINDOW_SIZE);
                memcpy(library.ram + PSQ_WINDOW, generated.ram + PSQ_WINDOW, PSQ_WINDOW_SIZE);
            }
            memcpy(reference.gpr, generated.gpr, sizeof(generated.gpr));
            memcpy(library.gpr, generated.gpr, sizeof(generated.gpr));
            memcpy(reference.gqr, generated.gqr, sizeof(generated.gqr));
            memcpy(library.gqr, generated.gqr, sizeof(generated.gqr));
            generated.hid2 = reference.hid2 = library.hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
            reference.exception = library.exception = 0;
            generated.fpscr = reference.fpscr = library.fpscr = fpscr;
            generated.cr = reference.cr = library.cr = cr;
            generated.pc = function->address;
            generated.lr = 0x81234567u;
            generated.exception = 0;
            generated.program_exception = 0;
            generated.msr = 1u << 13;
            generated.downcount = 0;
            h1_entries[f](&generated);
            for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
                replay(&reference, &function->steps[i], 0);
            for (u32 i = 0; i < H1_MAX_WORDS && function->steps[i].kind != H1_END; i++)
                replay(&library, &function->steps[i], 1);
            if ((fpscr & 0xFBu) == 0)
                fast_mode++;
            if (isfinite(generated.fpr[function->steps[0].d]))
                finite_results++;
            // The inline paired-single path records FPRF while fast_cpu.h's paired fast paths
            // skip it, so FPRF is compared only for all-scalar functions.
            u32 fpscr_mask = fast_only ? ~0x0001F000u : ~0u;
            int bad = memcmp(generated.gpr, reference.gpr, sizeof(generated.gpr)) != 0 ||
                      generated.exception != 0 || reference.exception != 0 ||
                      memcmp(generated.fpr, reference.fpr, sizeof(generated.fpr)) != 0 ||
                      memcmp(generated.ps1, reference.ps1, sizeof(generated.ps1)) != 0 ||
                      (generated.fpscr & fpscr_mask) != (reference.fpscr & fpscr_mask) ||
                      generated.cr != reference.cr;
            // The exact library must produce the same register values. It also records FPSCR
            // status bits the fast paths skip, so FPSCR (and CR1 copied from it by Rc forms) is
            // not compared. Paired ops replay through fast_cpu.h on both copies, whose paired
            // fast paths do not flush tiny results, so only all-scalar functions are compared.
            if (!fast_only) {
                u32 cr_mask = has_rc ? ~0x0F000000u : ~0u;
                int library_bad =
                    memcmp(generated.gpr, library.gpr, sizeof(generated.gpr)) != 0 ||
                    library.exception != 0 ||
                    memcmp(generated.fpr, library.fpr, sizeof(generated.fpr)) != 0 ||
                    memcmp(generated.ps1, library.ps1, sizeof(generated.ps1)) != 0 ||
                    (generated.cr & cr_mask) != (library.cr & cr_mask);
                if (library_bad && ++library_mismatches <= 3) {
                    printf("LIBRARY MISMATCH %s iteration %u fpscr %08X\n", function->name,
                           iteration, fpscr);
                    for (u32 reg = 0; reg < 32; reg++)
                        if (bits_of(generated.fpr[reg]) != bits_of(library.fpr[reg]) ||
                            bits_of(generated.ps1[reg]) != bits_of(library.ps1[reg]))
                            printf("  f%u: llvm %016llX/%016llX  lib %016llX/%016llX\n", reg,
                                   (unsigned long long)bits_of(generated.fpr[reg]),
                                   (unsigned long long)bits_of(generated.ps1[reg]),
                                   (unsigned long long)bits_of(library.fpr[reg]),
                                   (unsigned long long)bits_of(library.ps1[reg]));
                    if ((generated.cr & cr_mask) != (library.cr & cr_mask))
                        printf("  cr: llvm %08X  lib %08X\n", generated.cr, library.cr);
                }
            }
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
            for (u32 reg = 0; reg < 32; reg++)
                if (generated.gpr[reg] != reference.gpr[reg])
                    printf("  r%u: llvm %08X  c %08X\n", reg, generated.gpr[reg], reference.gpr[reg]);
            if (generated.exception || reference.exception)
                printf("  exception: llvm %u  c %u\n", generated.exception, reference.exception);
            if (generated.cr != reference.cr)
                printf("  cr: llvm %08X  c %08X\n", generated.cr, reference.cr);
        }
        printf("%-16s %u runs, %llu fast-mode, %llu finite d, %llu mismatches, %llu library\n",
               function->name, iterations, (unsigned long long)fast_mode,
               (unsigned long long)finite_results, (unsigned long long)mismatches,
               (unsigned long long)library_mismatches);
        total_mismatches += mismatches + library_mismatches;
        total_fast_mode += fast_mode;
    }
    printf("TOTAL mismatches %llu\n", (unsigned long long)total_mismatches);
    return total_mismatches ? 1 : 0;
}
