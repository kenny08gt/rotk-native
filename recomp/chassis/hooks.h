// Function-entry hooks placed in the generated chunks by tools/add_entry_hooks.py. They compile to
// nothing unless the chunks are built with ROTK_TRACE_HOOKS (CMake -DROTK_TRACE_HOOKS=ON).
#pragma once
#ifdef ROTK_TRACE_HOOKS
void rotk_fn_entry(void* ctx, unsigned addr);
#define ROTK_FN_ENTRY(c, a) rotk_fn_entry((c), (a))
#else
#define ROTK_FN_ENTRY(c, a) ((void)0)
#endif

// Loop-head hand-off points placed by tools/add_hle_points.py (addresses in chassis/hle_points.txt).
// With native routines on (chassis/hle.c), the generated code returns to the dispatcher there so a
// native version can run the loop; the dispatcher sets g_hle_skip when it declines, and the
// generated code then runs through that point itself.
extern unsigned g_hle_points_on, g_hle_skip;
#define ROTK_HLE_POINT(c, a)                                  \
    do {                                                      \
        if (g_hle_points_on && g_hle_skip != (a)) {           \
            (c)->pc = (a);                                    \
            return;                                           \
        }                                                     \
    } while (0)

// Cheat patch points (chassis/cheat_points.inc, chassis/cheats.c): while a cheat replaces the
// instruction at `a`, the generated code returns to the dispatcher there to run the replacement.
extern unsigned g_cheat_points_on;
#define ROTK_CHEAT_POINT(c, a)                                \
    do {                                                      \
        if (g_cheat_points_on && g_hle_skip != (a)) {         \
            (c)->pc = (a);                                    \
            return;                                           \
        }                                                     \
    } while (0)
