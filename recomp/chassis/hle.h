#pragma once
#include "cpu/cpu.h"

// Native versions of hot guest routines; see hle.c.
void hle_init(void (*(*chunk_for)(u32))(CPUState*));
// Runs a native version if cpu->pc is the start of one: returns the guest cycles charged, or 0.
s64 hle_try(CPUState* cpu);
void hle_report(void);

// Loop-head hand-off state shared with the generated chunks (see chassis/hooks.h).
extern unsigned g_hle_points_on, g_hle_skip;
