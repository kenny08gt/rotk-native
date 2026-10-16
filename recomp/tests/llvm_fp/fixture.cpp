// Builds an object with one guest function per scalar FP op (single, double,
// fused and fcmp, plus a few chained and paired-mixed sequences) through the LLVM backend, so
// test.c can compare each against recomp/chassis/fast_cpu.h.
#include "backend/llvm/llvm_backend.h"
#include "cpu/cpu.h"
#include "frontend/decoder.h"
#include "ir/dolir_builder.h"

#include <cstdio>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
static void crashed(int) {
  void *frames[64];
  int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, 2);
  _exit(3);
}

#include "ops.h"

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__,    \
                   #x);                                                        \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static bool add_chunk(DolIRModule *module, const u32 *words, u32 count,
                      u32 address) {
  PPCInst *instructions = new PPCInst[count];
  for (u32 i = 0; i < count; i++)
    instructions[i] = ppc_decode(words[i], address + i * 4u);
  const bool result = dolir_build_chunk(module, instructions, count, address);
  delete[] instructions;
  return result;
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  std::signal(SIGSEGV, crashed);
  std::signal(SIGBUS, crashed);
  DolIRModule module;
  dolir_module_init(&module);
  for (u32 i = 0; i < H1_FUNCTION_COUNT; i++) {
    const H1Function &function = h1_functions[i];
    u32 words[H1_MAX_WORDS + 1];
    u32 count = 0;
    while (count < H1_MAX_WORDS && function.steps[count].kind != H1_END) {
      words[count] = h1_encode(function.steps[count]);
      count++;
    }
    words[count++] = 0x4E800020u;
    std::fprintf(stderr, "chunk %s %u words\n", function.name, count);
    CHECK(add_chunk(&module, words, count, function.address));
  }
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.emit_ir = 1;
  options.ir_path = argv[2];
  options.fixed_memory_layout = 1;
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.mem2_size = WII_MEM2_SIZE;
  options.semantics = DOLLLVM_SEMANTICS_EXACT;
  options.runtime = DOLLLVM_RUNTIME_RECOMPCORE;
  std::fprintf(stderr, "emitting\n");
  CHECK(dolllvm_emit_object(&module, argv[1], &options, stderr));
  dolir_module_free(&module);
  return 0;
}
