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

# The recompiled game. ROTK_BACKEND=c (default): DolRecomp's C output in out/generated, 92 chunk
# sources compiled here. ROTK_BACKEND=llvm: its LLVM backend's objects in out-llvm/generated
# (guest registers promoted to host registers within each function).
set(ROTK_BACKEND "c" CACHE STRING "Recompiled code: c or llvm")
if(ROTK_BACKEND STREQUAL "llvm")
    set(ROTK_GEN ${CMAKE_CURRENT_SOURCE_DIR}/out-llvm/generated)
    file(GLOB ROTK_CHUNKS CONFIGURE_DEPENDS ${ROTK_GEN}/chunks/*.o)
    add_library(rotk_chunks STATIC ${ROTK_GEN}/generated.c ${ROTK_CHUNKS})
    target_include_directories(rotk_chunks PRIVATE ${ROTK_GEN})
    target_link_libraries(rotk_chunks PUBLIC dolrecomp_cpu)
    target_compile_options(rotk_chunks PRIVATE -O2 -w)
    # The LLVM functions can only be entered at their region leaders. For any other pc (resuming
    # mid-block) the dispatcher runs the C backend's code from there instead, linked as cfunc_*.
    set(ROTK_CGEN ${CMAKE_CURRENT_SOURCE_DIR}/out/generated)
    file(GLOB ROTK_CCHUNKS CONFIGURE_DEPENDS ${ROTK_CGEN}/chunks/*.c)
    file(STRINGS ${ROTK_CGEN}/generated.h ROTK_CPROTOS REGEX "^void func_[0-9A-F]+\\(CPUState\\* ctx\\);")
    set(ROTK_CRENAMES "")
    foreach(proto ${ROTK_CPROTOS})
        string(REGEX REPLACE "^void func_([0-9A-F]+).*" "\\1" base "${proto}")
        list(APPEND ROTK_CRENAMES "func_${base}=cfunc_${base}")
    endforeach()
    add_library(rotk_cchunks STATIC ${ROTK_CCHUNKS})
    target_include_directories(rotk_cchunks PRIVATE ${ROTK_CGEN})
    target_link_libraries(rotk_cchunks PUBLIC dolrecomp_cpu)
    target_compile_definitions(rotk_cchunks PRIVATE ${ROTK_CRENAMES}
        "DOLRECOMP_CPU_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/chassis/fast_cpu.h\"")
    target_compile_options(rotk_cchunks PRIVATE -O2 -mcpu=apple-m1 -w -ffp-contract=off
        -include ${CMAKE_CURRENT_SOURCE_DIR}/chassis/hooks.h)
    set(ROTK_CCHUNK_TABLE ${CMAKE_CURRENT_BINARY_DIR}/cchunks.inc)
    add_custom_command(OUTPUT ${ROTK_CCHUNK_TABLE}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_chunk_table.py ${ROTK_CGEN}/generated.h ${ROTK_CCHUNK_TABLE} cfunc_ kCChunks
        DEPENDS ${ROTK_CGEN}/generated.h ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_chunk_table.py)
else()
    set(ROTK_GEN ${CMAKE_CURRENT_SOURCE_DIR}/out/generated)
    file(GLOB ROTK_CHUNKS CONFIGURE_DEPENDS ${ROTK_GEN}/chunks/*.c)
    add_library(rotk_chunks STATIC ${ROTK_CHUNKS})
    target_include_directories(rotk_chunks PRIVATE ${ROTK_GEN})
    target_link_libraries(rotk_chunks PUBLIC dolrecomp_cpu)
    target_compile_options(rotk_chunks PRIVATE -O3 -mcpu=apple-m1 -Wno-everything -ffp-contract=off
        -include ${CMAKE_CURRENT_SOURCE_DIR}/chassis/hooks.h)
    # Profile-guided optimization of the generated code: ROTK_PGO=gen builds instrumented chunks
    # (pgo.sh trains them on the fight benchmark and writes pgo/rotk.profdata),
    # ROTK_PGO=use builds with ROTK_PGO_PROFILE (default: pgo/rotk.profdata). Use keeps the stale-profile
    # warnings on: clang silently drops the profile of any function whose generated code changed.
    set(ROTK_PGO "" CACHE STRING "Profile-guided optimization: gen, use or empty")
    set(ROTK_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/rotk.profdata" CACHE FILEPATH "Merged profile for ROTK_PGO=use")
    if(ROTK_PGO STREQUAL "gen")
        target_compile_options(rotk_chunks PRIVATE -fprofile-instr-generate)
    elseif(ROTK_PGO STREQUAL "use")
        target_compile_options(rotk_chunks PRIVATE -fprofile-instr-use=${ROTK_PGO_PROFILE}
            -Wprofile-instr-out-of-date -Wprofile-instr-unprofiled)
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
endif()
# The dispatcher's table of chunk entry points, from the generated header.
set(ROTK_CHUNK_TABLE ${CMAKE_CURRENT_BINARY_DIR}/chunks.inc)
add_custom_command(OUTPUT ${ROTK_CHUNK_TABLE}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_chunk_table.py ${ROTK_GEN}/generated.h ${ROTK_CHUNK_TABLE}
    DEPENDS ${ROTK_GEN}/generated.h ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_chunk_table.py)

add_executable(rotk ${ROTK_CHUNK_TABLE} chassis/main.c chassis/hw.c chassis/gpu.c chassis/input.c chassis/overlay.cpp chassis/ax_hle.cpp chassis/audio.c chassis/exi.c chassis/hle.c chassis/fast_helpers.c chassis/savestate.c)
target_include_directories(rotk PRIVATE ${ROTK_GEN} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(rotk PRIVATE -Wall -Wextra)
target_link_libraries(rotk PRIVATE rotk_chunks aurora::core aurora::gx aurora::main aurora::vi compression)
if(ROTK_BACKEND STREQUAL "llvm")
    target_sources(rotk PRIVATE ${ROTK_CCHUNK_TABLE})
    target_compile_definitions(rotk PRIVATE ROTK_LLVM)
    target_link_libraries(rotk PRIVATE rotk_cchunks)
endif()
if(ROTK_PGO STREQUAL "gen")
    target_link_options(rotk PRIVATE -fprofile-instr-generate)
endif()
