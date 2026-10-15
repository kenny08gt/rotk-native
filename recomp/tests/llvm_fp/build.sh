#!/bin/sh
# Differential test for the LLVM backend's inline scalar FP fast paths
# (patches/dolrecomp-llvm-inline-single-fp.patch): compiles guest functions through the real
# backend (fixture.cpp), runs them on random register states, and replays the same instructions
# with chassis/fast_cpu.h, comparing every FPR, PS1, FPSCR and CR bit.
# Needs the DolRecomp LLVM build (third_party/dolrecomp/build-llvm) and LLVM 20.
# usage: tests/llvm_fp/build.sh [iterations] [seed]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
dr=$root/third_party/dolrecomp
build=$dr/build-llvm
llvm=${LLVM_DIR_PREFIX:-/opt/homebrew/opt/llvm@20}
out=$here/out
mkdir -p "$out"
c++ -std=c++17 -O1 -I"$dr/src" -I"$here" -I"$llvm/include" "$here/fixture.cpp" \
  "$build/libdr_llvm.a" "$build/libdr_ir.a" "$build/libdr_frontend.a" \
  "$build/libdr_analysis.a" "$build/libdr_cpu.a" "$build/libdr_platform.a" \
  "$llvm/lib/libLLVM.dylib" -lz -Wl,-rpath,"$llvm/lib" -o "$out/fixture"
"$out/fixture" "$out/fp.o" "$out/fp.ll"
cc -std=c11 -O2 -ffp-contract=off -mcpu=apple-m1 -w -I"$dr/src" -I"$root/recomp" -I"$here" \
  "$here/test.c" "$out/fp.o" "$build/libdr_cpu.a" "$build/libdr_platform.a" -o "$out/test"
"$out/test" "$@"
