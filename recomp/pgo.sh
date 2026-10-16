#!/bin/sh
# Retrain the PGO profile (pgo/rotk.profdata) on the deterministic fight benchmark.
# Run after the generated code changes (new DolRecomp patches or a regenerated out/): clang drops
# the profile of every function whose code changed, which costs about 5% in the fight.
# usage: ./pgo.sh   (then rebuild build/ to use the new profile)
set -eu
cd "$(dirname "$0")"
cmake -S . -B build-pgogen -DCMAKE_BUILD_TYPE=Release -DROTK_PGO=gen > /dev/null
cmake --build build-pgogen -j"$(sysctl -n hw.ncpu)"
rm -f build-pgogen/*.profraw
LLVM_PROFILE_FILE="$PWD/build-pgogen/train-%p.profraw" ROTK_BIN=./build-pgogen/rotk ./benchw.sh pgotrain 1
xcrun llvm-profdata merge -o pgo/rotk.profdata build-pgogen/*.profraw
echo "wrote pgo/rotk.profdata"
