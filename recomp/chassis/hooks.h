// Function-entry hooks placed in the generated chunks by tools/add_entry_hooks.py. They compile to
// nothing unless the chunks are built with ROTK_TRACE_HOOKS (CMake -DROTK_TRACE_HOOKS=ON).
#pragma once
#ifdef ROTK_TRACE_HOOKS
void rotk_fn_entry(void* ctx, unsigned addr);
#define ROTK_FN_ENTRY(c, a) rotk_fn_entry((c), (a))
#else
#define ROTK_FN_ENTRY(c, a) ((void)0)
#endif
