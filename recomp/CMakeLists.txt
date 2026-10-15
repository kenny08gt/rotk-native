cmake_minimum_required(VERSION 3.25)
project(rotk_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

set(DOLRECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dolrecomp)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/aurora aurora EXCLUDE_FROM_ALL)

# DolRecomp CPU runtime (mem_read*, FP/paired-single helpers), built by DolRecomp's own CMake.
add_library(dolrecomp_cpu STATIC IMPORTED)
set_target_properties(dolrecomp_cpu PROPERTIES
    IMPORTED_LOCATION ${DOLRECOMP_DIR}/build/libdr_cpu.a
    INTERFACE_INCLUDE_DIRECTORIES ${DOLRECOMP_DIR}/src)

# The recompiled game: 92 generated chunks. Rebuilt only when DolRecomp output changes.
file(GLOB ROTK_CHUNKS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/out/generated/chunks/*.c)
add_library(rotk_chunks STATIC ${ROTK_CHUNKS})
target_include_directories(rotk_chunks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/out/generated)
target_link_libraries(rotk_chunks PUBLIC dolrecomp_cpu)
target_compile_options(rotk_chunks PRIVATE -O3 -mcpu=apple-m1 -w -ffp-contract=off
    -include ${CMAKE_CURRENT_SOURCE_DIR}/chassis/hooks.h)
# Profile-guided optimization of the generated code: ROTK_PGO=gen builds instrumented chunks
# (run the game, then merge default_*.profraw with `xcrun llvm-profdata merge -o rotk.profdata`),
# ROTK_PGO=use builds with ROTK_PGO_PROFILE (default: pgo/rotk.profdata).
set(ROTK_PGO "" CACHE STRING "Profile-guided optimization: gen, use or empty")
set(ROTK_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/rotk.profdata" CACHE FILEPATH "Merged profile for ROTK_PGO=use")
if(ROTK_PGO STREQUAL "gen")
    target_compile_options(rotk_chunks PRIVATE -fprofile-instr-generate)
elseif(ROTK_PGO STREQUAL "use")
    target_compile_options(rotk_chunks PRIVATE -fprofile-instr-use=${ROTK_PGO_PROFILE} -Wno-profile-instr-unprofiled)
endif()

# Function-entry tracing (FN_TRACE=addr,... at run time); off by default, costs a call per guest function.
option(ROTK_TRACE_HOOKS "Call rotk_fn_entry at every guest function entry" OFF)
if(ROTK_TRACE_HOOKS)
    target_compile_definitions(rotk_chunks PRIVATE ROTK_TRACE_HOOKS)
endif()
# Inline fast paths for memory and FP helpers (chassis/fast_cpu.h); the generated code includes this instead of cpu/cpu.h.
option(ROTK_FAST_CPU "Use the inline fast paths in chassis/fast_cpu.h" ON)
if(ROTK_FAST_CPU)
    target_compile_definitions(rotk_chunks PRIVATE "DOLRECOMP_CPU_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/chassis/fast_cpu.h\"")
    set(ROTK_FAST_DEFS "" CACHE STRING "Extra defines for fast_cpu.h, e.g. FAST_NO_PSQ (bisecting)")
    target_compile_definitions(rotk_chunks PRIVATE ${ROTK_FAST_DEFS})
endif()

add_executable(rotk chassis/main.c chassis/hw.c chassis/gpu.c chassis/input.c chassis/ax_hle.cpp chassis/audio.c chassis/exi.c)
target_include_directories(rotk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/out/generated)
target_compile_options(rotk PRIVATE -Wall -Wextra)
target_link_libraries(rotk PRIVATE rotk_chunks aurora::core aurora::gx aurora::main aurora::vi)
if(ROTK_PGO STREQUAL "gen")
    target_link_options(rotk PRIVATE -fprofile-instr-generate)
endif()
