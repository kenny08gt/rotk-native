// Action Replay cheats from ~/Library/Application Support/rotk/cheats.txt, run once per frame
// like Dolphin runs them. Codes that write data change RAM. Codes that replace an instruction
// cannot change recompiled code, so they need a patch point at that address (cheat_points.inc,
// placed in the generated code at build time): the code returns to the dispatcher there, and
// cheats_try runs the replacement instruction instead.
#pragma once
#include "cpu/cpu.h"

// Nonzero while some cheat replaces an instruction (the dispatcher checks it before cheats_try).
extern unsigned g_cheat_points_on;

void cheats_init(CPUState* cpu, int (*is_code)(u32 addr));
// Runs the enabled cheats (game thread, at a frame boundary).
void cheats_frame(void);
// Dispatcher hook: when pc is a replaced instruction, runs the replacement and returns the guest
// cycles it took; else 0.
s64 cheats_try(CPUState* cpu);

// For the cheat menu (any thread).
int cheats_count(void);
const char* cheats_name(int i);
int cheats_enabled(int i);
// Why a cheat cannot work, or NULL.
const char* cheats_problem(int i);
// Turns a cheat on or off and writes the choice back to cheats.txt.
void cheats_toggle(int i);
