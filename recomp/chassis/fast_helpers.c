// Strong definitions of DolRecomp's floating-point and paired-single helpers. The library's
// versions are weak wrappers around ppc_*_exact (patches/dolrecomp-fast-fp-helpers.patch); these
// run the fast paths of chassis/fast_cpu.h first and fall back to the exact versions. The C
// backend inlines the same fast paths, so this mainly speeds up the LLVM backend, which calls the
// helpers out of line.
//
// fast_cpu.h's fallbacks call the library by name; point those names at the exact versions so a
// fallback never comes back here.
#define FAST_NO_FP
#define ppc_fadds ppc_fadds_exact
#define ppc_fsubs ppc_fsubs_exact
#define ppc_fmuls ppc_fmuls_exact
#define ppc_fdivs ppc_fdivs_exact
#define ppc_frsp ppc_frsp_exact
#define ppc_fadd ppc_fadd_exact
#define ppc_fsub ppc_fsub_exact
#define ppc_fmul ppc_fmul_exact
#define ppc_fdiv ppc_fdiv_exact
#define ppc_fma ppc_fma_exact
#define ppc_fcmp ppc_fcmp_exact
#define ppc_ps_add_op ppc_ps_add_op_exact
#define ppc_ps_sub_op ppc_ps_sub_op_exact
#define ppc_ps_mul_op ppc_ps_mul_op_exact
#define ppc_ps_muls0 ppc_ps_muls0_exact
#define ppc_ps_muls1 ppc_ps_muls1_exact
#define ppc_ps_madd_op ppc_ps_madd_op_exact
#define ppc_ps_madds0 ppc_ps_madds0_exact
#define ppc_ps_madds1 ppc_ps_madds1_exact
#define ppc_ps_sum0 ppc_ps_sum0_exact
#define ppc_ps_sum1 ppc_ps_sum1_exact
#include "fast_cpu.h"
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

#define THREE(op) \
    void ppc_##op(CPUState* cpu, u8 d, u8 a, u8 b) { fast_##op(cpu, d, a, b); }
#define FOUR(op) \
    void ppc_##op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) { fast_##op(cpu, d, a, c, b); }

THREE(fadds)
THREE(fsubs)
THREE(fmuls)
THREE(fdivs)
THREE(fadd)
THREE(fsub)
THREE(fmul)
THREE(fdiv)
THREE(ps_add_op)
THREE(ps_sub_op)
THREE(ps_mul_op)
THREE(ps_muls0)
THREE(ps_muls1)
FOUR(ps_madds0)
FOUR(ps_madds1)
FOUR(ps_sum0)
FOUR(ps_sum1)

void ppc_frsp(CPUState* cpu, u8 d, u8 b) { fast_frsp(cpu, d, b); }
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) { fast_fcmp(cpu, crfd, a, b, ordered); }
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output) {
    return fast_fma(cpu, a, c, b, single, subtract, negative, output);
}
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    fast_ps_madd_op(cpu, d, a, c, b, subtract, negative);
}
