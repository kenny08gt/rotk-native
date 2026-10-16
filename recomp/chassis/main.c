// Boot chassis for the DolRecomp C output of GKLE69.
// Loads main.dol into guest RAM, fakes the IPL/apploader low-memory setup,
// dispatches recompiled chunks, delivers exceptions through the OS's own
// handler table, and reports how far the game gets.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "cpu/cpu.h"
#include "gpu.h"
#include "hw.h"
#include "hle.h"
#include "input.h"
#include "audio.h"
#include "exi.h"
#include "ax_hle.h"
#include "overlay.h"
#include "savestate.h"
#include <libproc.h>
#include <unistd.h>

u8* g_fast_vm;   // see fast_cpu.h
u32 g_fast_vm_base, g_fast_vm_size;

#include "chunks.inc"
#ifdef ROTK_LLVM
// LLVM backend: entries it cannot take (a pc that is not a region leader) run the C backend's
// code instead. The fallback hook flags them with this pseudo-exception.
#include "cchunks.inc"
#define EXC_COLD_ENTRY 0xC01Du
static int g_running_c;
#endif

static CPUState* g_clock_cpu;
static double guest_seconds(void) { return g_clock_cpu->timebase / 40500000.0; }

// BENCH_WINDOW=start,end (guest seconds): host work spent between the two guest times, then stop.
// Instructions come from the process (run with NO_GPU=1 so that is mostly this thread). The RAM
// hash checks that two builds computed the same game state.
static void bench_window(CPUState* cpu, int* stop) {
    static double start = -1, end;
    static int state;   // 0 waiting, 1 measuring, 2 done
    static u64 insns0, cyc0;
    static double cpu0;
    if (start < 0) {
        const char* w = getenv("BENCH_WINDOW");
        if (!w || sscanf(w, "%lf,%lf", &start, &end) != 2) { start = 0; state = 2; }
    }
    if (state == 2) return;
    double g = guest_seconds();
    struct rusage_info_v4 ri;
    if (state == 0 && g >= start) {
        proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t*)&ri);
        insns0 = ri.ri_instructions, cyc0 = ri.ri_cycles;
        cpu0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e9;
        state = 1;
    } else if (state == 1 && g >= end) {
        proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t*)&ri);
        double secs = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e9 - cpu0;
        u64 hash = 1469598103934665603ull;
        for (u32 i = 0; i < cpu->ram_size; i++) hash = (hash ^ cpu->ram[i]) * 1099511628211ull;
        printf("bench window %.0f-%.0f s: %.3f G host instructions (%.1f M per guest second), %.3f G cycles, "
               "game thread %.2f s CPU (%.0f%% of real time), RAM hash %016llX\n",
               start, end, (ri.ri_instructions - insns0) / 1e9, (ri.ri_instructions - insns0) / 1e6 / (end - start),
               (ri.ri_cycles - cyc0) / 1e9, secs, 100.0 * (end - start) / secs, hash);
        if (getenv("BENCH_DUMP")) {   // guest RAM at the window end, to diff two builds
            FILE* f = fopen(getenv("BENCH_DUMP"), "wb");
            if (f) { fwrite(cpu->ram, 1, cpu->ram_size, f); fclose(f); }
        }
        state = 2;
        *stop = 1;
    }
}

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define NCHUNKS (sizeof(kChunks) / sizeof(kChunks[0]))

// OS globals and layout (Dolphin SDK).
#define OS_CURRENT_CONTEXT     0x800000D4u
#define OS_EXCEPTION_TABLE_PTR 0x80251924u   // OSExceptionTable (game-specific .sbss address)
#define CTX_GPR   0x000
#define CTX_CR    0x080
#define CTX_LR    0x084
#define CTX_CTR   0x088
#define CTX_XER   0x08C
#define CTX_SRR0  0x198
#define CTX_SRR1  0x19C
#define CTX_STATE 0x1A2
#define CTX_STATE_EXC 0x0002

enum { EXC_EXTERNAL = 4, EXC_FP_UNAVAILABLE = 7, EXC_DECREMENTER = 8 };
#define MSR_EE 0x8000u

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

static u8* read_file(const char* path, u32* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* buf = malloc(*size);
    if (fread(buf, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return buf;
}

// ---- boot files: from an extracted sys/ directory, or straight from the disc image ----
// Disc layout: boot.bin = 0x000-0x440 (DOL offset at 0x420, FST offset/size at 0x424/0x428),
// bi2.bin = 0x440-0x2440.
static u8* read_range(FILE* f, u64 off, u32 size) {
    u8* buf = malloc(size);
    fseeko(f, (off_t)off, SEEK_SET);
    if (fread(buf, 1, size, f) != size) { fprintf(stderr, "short read from disc image\n"); exit(1); }
    return buf;
}

static u8* read_sys(const char* source, const char* name, u32* size) {
    char p[1024];
    FILE* f = fopen(source, "rb");
    if (!f) { perror(source); exit(1); }
    u8 head[0x440];
    int is_disc = fread(head, 1, sizeof head, f) == sizeof head && be32(head + 0x1C) == 0xC2339F3Du;   // GC magic
    if (!is_disc) {
        fclose(f);
        snprintf(p, sizeof p, "%s/%s", source, name);
        return read_file(p, size);
    }
    u8* out;
    if (!strcmp(name, "boot.bin")) {
        *size = sizeof head;
        out = malloc(*size);
        memcpy(out, head, *size);
    } else if (!strcmp(name, "bi2.bin")) {
        *size = 0x2000;
        out = read_range(f, 0x440, *size);
    } else if (!strcmp(name, "fst.bin")) {
        *size = be32(head + 0x428);
        out = read_range(f, be32(head + 0x424), *size);
    } else {   // main.dol: its size is the end of the furthest section
        u32 dol = be32(head + 0x420);
        u8* hdr = read_range(f, dol, 0x100);
        u32 end = 0x100;
        for (int i = 0; i < 18; i++) {
            u32 o = be32(hdr + i * 4), sz = be32(hdr + 0x90 + i * 4);
            if (sz && o + sz > end) end = o + sz;
        }
        free(hdr);
        *size = end;
        out = read_range(f, dol, end);
    }
    fclose(f);
    return out;
}

// ---- symbols (addr \t size \t name) ----
typedef struct { u32 addr, size; char name[96]; } Sym;
static Sym* g_syms;
static size_t g_nsyms;

static void load_syms(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    size_t cap = 8192;
    g_syms = malloc(cap * sizeof(Sym));
    while (fgets(line, sizeof line, f)) {
        Sym s = {0};
        if (sscanf(line, "%x\t%x\t%95[^\n]", &s.addr, &s.size, s.name) == 3) {
            if (g_nsyms == cap) g_syms = realloc(g_syms, (cap *= 2) * sizeof(Sym));
            g_syms[g_nsyms++] = s;
        }
    }
    fclose(f);
}

static const char* sym_for(u32 pc, u32* off) {
    size_t lo = 0, hi = g_nsyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= pc) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return "?";
    *off = pc - g_syms[lo - 1].addr;
    return g_syms[lo - 1].name;
}

// Instructions DolRecomp leaves to the host: SPR moves the runtime doesn't model (DEC, HID0/1,
// L2CR, WPAR, DMA regs, ...) go to the device model's SPR callbacks; cache maintenance is a no-op.
static void fallback(CPUState* cpu, u32 raw, u32 cia) {
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FF, rd = (raw >> 21) & 31;
    u16 spr = (u16)(((raw >> 16) & 0x1F) | ((raw >> 11) & 0x1F) << 5);
    if (op == 31 && xo == 467) {                 // mtspr
        cpu->spr_write(cpu, spr, cpu->gpr[rd], cia);
    } else if (op == 31 && xo == 339) {          // mfspr
        cpu->gpr[rd] = cpu->spr_read(cpu, spr, cia);
    } else if (op == 31 && (xo == 86 || xo == 54 || xo == 470 || xo == 982 || xo == 246 || xo == 278)) {
        // dcbf, dcbst, dcbi, icbi, dcbtst, dcbt: caches are coherent on the host
    } else {
#ifdef ROTK_LLVM
        if (!g_running_c) {   // the LLVM code was entered where it cannot run: hand over to the C code
            cpu->pc = cia;
            cpu->exception = EXC_COLD_ENTRY;
            return;
        }
#endif
        static u32 count;
        if (count++ < 16) fprintf(stderr, "unhandled fallback instruction 0x%08X at 0x%08X\n", raw, cia);
    }
    cpu->pc = cia + 4;
}

// ---- WATCH=addr,len: count which guest PCs write into a memory range ----
static CPUState* g_watch_cpu;
static u32 g_watch_lo, g_watch_hi;
static struct { u32 pc, count, lo, hi; } g_watch_pcs[4096];
static u32 g_watch_pc_lo, g_watch_pc_hi;

static void watch_journal(u32 offset, u32 size, void* user) {
    (void)user;
    u32 a = 0x80000000u | offset;
    if (a + size <= g_watch_lo || a >= g_watch_hi) return;
    u32 pc = g_watch_cpu->pc;
    if (g_watch_pc_hi && (pc < g_watch_pc_lo || pc >= g_watch_pc_hi)) return;
    for (int i = 0; i < 4096; i++) {
        if (g_watch_pcs[i].pc == pc || g_watch_pcs[i].count == 0) {
            if (!g_watch_pcs[i].count) g_watch_pcs[i].lo = g_watch_pcs[i].hi = a;
            g_watch_pcs[i].pc = pc;
            g_watch_pcs[i].count++;
            if (a < g_watch_pcs[i].lo) g_watch_pcs[i].lo = a;
            if (a > g_watch_pcs[i].hi) g_watch_pcs[i].hi = a;
            return;
        }
    }
}

// ---- boot ----
static void load_dol(CPUState* cpu, const u8* d) {
    // The header's BSS range spans .sdata/.sdata2, so clear it before loading sections.
    u32 bss = be32(d + 0xD8), bss_sz = be32(d + 0xDC);
    memset(cpu->ram + (bss & 0x01FFFFFF), 0, bss_sz);
    for (int i = 0; i < 18; i++) {
        u32 off = be32(d + i * 4), addr = be32(d + 0x48 + i * 4), sz = be32(d + 0x90 + i * 4);
        if (sz) memcpy(cpu->ram + (addr & 0x01FFFFFF), d + off, sz);
    }
    cpu->pc = be32(d + 0xE0);
}

static void fake_ipl(CPUState* cpu, const char* sys) {
    u32 n, fst_size, bi2_size;
    u8* boot = read_sys(sys, "boot.bin", &n);
    memcpy(cpu->ram, boot, 0x20);                    // disc ID, magic
    u8* fst = read_sys(sys, "fst.bin", &fst_size);
    u8* bi2 = read_sys(sys, "bi2.bin", &bi2_size);

    u32 fst_addr = (0x81800000u - fst_size) & ~31u;
    u32 bi2_addr = (fst_addr - bi2_size) & ~31u;
    memcpy(cpu->ram + (fst_addr & 0x01FFFFFF), fst, fst_size);
    memcpy(cpu->ram + (bi2_addr & 0x01FFFFFF), bi2, bi2_size);

    mem_write32(cpu, 0x80000020, 0x0D15EA5E);   // boot magic
    mem_write32(cpu, 0x80000024, 1);            // version
    mem_write32(cpu, 0x80000028, 0x01800000);   // physical memory size
    mem_write32(cpu, 0x8000002C, 1);            // console type: retail
    mem_write32(cpu, 0x80000030, 0);            // arena lo (OS computes)
    mem_write32(cpu, 0x80000034, bi2_addr);     // arena hi
    mem_write32(cpu, 0x80000038, fst_addr);     // FST location
    mem_write32(cpu, 0x8000003C, fst_size);     // FST max length
    mem_write32(cpu, 0x800000CC, 0);            // TV mode: NTSC
    mem_write32(cpu, 0x800000F0, 0x01800000);   // simulated memory size
    mem_write32(cpu, 0x800000F4, bi2_addr);     // BI2 location
    mem_write32(cpu, 0x800000F8, 162000000);    // bus clock
    mem_write32(cpu, 0x800000FC, 486000000);    // CPU clock
    free(boot); free(fst); free(bi2);
}

// ---- exceptions ----
// Does what the OS's low-memory vector stub does (that code is copied at runtime and never
// recompiled): save state into the current OSContext, then enter the registered handler
// with r3 = exception number, r4 = context. Handlers return via OSLoadContext (rfi).
static int deliver(CPUState* cpu, u32 exc, u32 srr0, u32 srr1) {
    u32 table = mem_read32(cpu, OS_EXCEPTION_TABLE_PTR);
    u32 handler = table ? mem_read32(cpu, table + exc * 4) : 0;
    u32 ctx = mem_read32(cpu, OS_CURRENT_CONTEXT);
    if (!handler || !ctx) return 0;
    for (u32 i = 0; i < 32; i++) mem_write32(cpu, ctx + CTX_GPR + i * 4, cpu->gpr[i]);
    mem_write32(cpu, ctx + CTX_CR, cpu->cr);
    mem_write32(cpu, ctx + CTX_LR, cpu->lr);
    mem_write32(cpu, ctx + CTX_CTR, cpu->ctr);
    mem_write32(cpu, ctx + CTX_XER, cpu->xer);
    mem_write32(cpu, ctx + CTX_SRR0, srr0);
    mem_write32(cpu, ctx + CTX_SRR1, srr1);
    mem_write16(cpu, ctx + CTX_STATE, mem_read16(cpu, ctx + CTX_STATE) | CTX_STATE_EXC);
    cpu->srr0 = srr0;
    cpu->srr1 = srr1;
    cpu->msr = (srr1 & 0x1040u) | 0x32u;   // keep ME/IP; EE, FP, PR off; IR | DR | RI on
    cpu->gpr[3] = exc;
    cpu->gpr[4] = ctx;
    cpu->pc = handler;
    cpu->exception = 0;
    return 1;
}

static void backtrace(CPUState* cpu, const char* tag) {
    u32 off = 0;
    printf("backtrace at %s: pc=%08X(%s+0x%X) lr=%08X", tag, cpu->pc, sym_for(cpu->pc, &off), off, cpu->lr);
    off = 0;
    printf("(%s+0x%X)\n", sym_for(cpu->lr, &off), off);
    u32 sp = cpu->gpr[1];
    for (int i = 0; i < 24 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
        u32 next = mem_read32(cpu, sp);
        if (next < 0x80000000u || next >= 0x81800000u) break;
        u32 ra = mem_read32(cpu, next + 4);
        off = 0;
        printf("  #%d %08X %s+0x%X\n", i, ra, sym_for(ra, &off), off);
        sp = next;
    }
}

// THREADS_AT=<guest seconds>: list every OS thread (state, priority, saved PC) with a stack walk.
static void dump_threads(CPUState* cpu) {
    static const char* states[] = {"?", "READY", "RUNNING", "?", "WAITING", "?", "?", "?", "MORIBUND"};
    u32 cur = mem_read32(cpu, 0x800000E4);   // __OSCurrentThread
    for (u32 t = mem_read32(cpu, 0x800000DC), n = 0; t && n < 64; t = mem_read32(cpu, t + 0x2FC), n++) {
        u16 st = mem_read16(cpu, t + 0x2C8);
        u32 pc = t == cur ? cpu->pc : mem_read32(cpu, t + 0x198);
        u32 sp = t == cur ? cpu->gpr[1] : mem_read32(cpu, t + 0x4);
        u32 off = 0;
        printf("thread %08X %-8s prio=%2u suspend=%d pc=%08X %s+0x%X\n", t, st < 9 ? states[st] : "?",
               mem_read32(cpu, t + 0x2D0), (s32)mem_read32(cpu, t + 0x2CC), pc, sym_for(pc, &off), off);
        for (int i = 0; i < 12 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
            u32 next = mem_read32(cpu, sp);
            if (next < 0x80000000u || next >= 0x81800000u) break;
            u32 ra = mem_read32(cpu, next + 4);
            off = 0;
            printf("    %08X %s+0x%X\n", ra, sym_for(ra, &off), off);
            sp = next;
        }
    }
    fflush(stdout);
}

// Function-entry hook (ROTK_TRACE_HOOKS builds): FN_TRACE=addr[,addr...] prints each entry with its
// argument registers (up to FN_TRACE_N lines) and the per-function count at exit.
static u32 g_fn_trace[16], g_fn_trace_hits[16], g_fn_trace_count, g_fn_trace_max = 200, g_fn_trace_lines;

void rotk_fn_entry(void* ctx, unsigned addr) {
    for (u32 i = 0; i < g_fn_trace_count; i++) {
        if (g_fn_trace[i] != addr) continue;
        g_fn_trace_hits[i]++;
        if (g_fn_trace_lines++ < g_fn_trace_max) {
            CPUState* c = ctx;
            printf("fn %08X t=%.3fs r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X lr=%08X\n", addr, c->timebase / 40500000.0,
                   c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], c->gpr[7], c->lr);
        }
    }
}

static void fn_trace_init(void) {
    if (getenv("FN_TRACE_N")) g_fn_trace_max = (u32)atoi(getenv("FN_TRACE_N"));
    if (!getenv("FN_TRACE")) return;
    for (char *t = getenv("FN_TRACE"), *end; *t && g_fn_trace_count < 16; t = *end ? end + 1 : end) {
        g_fn_trace[g_fn_trace_count++] = (u32)strtoul(t, &end, 16);
        if (end == t) break;
    }
}

// Native stand-ins for SDK routines that only touch the caches (no-ops on the host but called
// millions of times): DCInvalidateRange, DCFlushRange, DCStoreRange, DCFlushRangeNoSync,
// ICInvalidateRange. They leave r3/r4/r5/ctr/cr0 as the original loop would, then return.
// Returns the guest cycles charged, or 0 if pc is not one of them.
static s64 hle_cache_range(CPUState* c) {
    switch (c->pc) {
    case 0x801475A0: case 0x801475CC: case 0x801475FC: case 0x8014762C: case 0x80147658: break;
    default: return 0;
    }
    u32 len = c->gpr[4];
    u32 cr0 = (len ? 0x4u : 0x2u) | ((c->xer >> 31) & 1);   // cmplwi r4, 0
    c->cr = (c->cr & 0x0FFFFFFFu) | (cr0 << 28);
    u32 n = 0;
    if (len) {
        u32 r5 = c->gpr[3] & 31;
        n = (len + r5 + 31) >> 5;
        c->gpr[5] = r5;
        c->gpr[4] = n;
        c->ctr = 0;
        c->gpr[3] += 32 * n;
    }
    c->pc = c->lr;
    return 8 + 3 * (s64)n;
}

// fn_80007030 spins on a flag byte (lbz r0, 0xf(r3); extsb. r0, r0; bne) until an interrupt
// clears it, and nothing else can change the byte inside a dispatch. Once two generated runs with
// the flag set have used the same cycles, later ones are replaced by that charge and the loop's
// final r0/cr0, so timing and state match the generated code exactly.
enum { SPIN_PC = 0x80007038u, SPIN_BASE = 0x8019D830u };
static s64 g_spin_cost, g_spin_seen;
static int spin_flag_set(const CPUState* c) {
    return c->pc == SPIN_PC && c->gpr[3] == SPIN_BASE && c->ram[SPIN_BASE + 0xF - 0x80000000u] != 0;
}
static s64 spin_skip(CPUState* c) {
    if (g_spin_cost <= 0 || !spin_flag_set(c)) return 0;
    s32 r0 = (s8)c->ram[SPIN_BASE + 0xF - 0x80000000u];
    c->gpr[0] = (u32)r0;
    c->cr = (c->cr & 0x0FFFFFFFu) | ((r0 < 0 ? 8u : 4u) | ((c->xer >> 31) & 1u)) << 28;
    return g_spin_cost;
}
static void spin_learn(int flag_was_set, const CPUState* c, s64 used) {
    if (!flag_was_set || g_spin_cost < 0 || c->pc != SPIN_PC || c->ram[SPIN_BASE + 0xF - 0x80000000u] == 0) return;
    if (g_spin_seen == 0) g_spin_seen = used;
    else if (g_spin_seen != used) g_spin_cost = -1;   // not a fixed cost: never skip
    else if (g_spin_cost == 0) g_spin_cost = used;
}

typedef void (*ChunkFn)(CPUState*);
static ChunkFn chunk_in(const void* table, size_t n, u32 pc) {
    const struct { u32 base; ChunkFn fn; }* t = table;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].base <= pc) lo = mid + 1; else hi = mid;
    }
    if (lo == 0 || pc >= 0x8016F580u) return NULL;
    return t[lo - 1].fn;
}
// Direct-mapped cache in front of the binary search; pc 0 never dispatches, so it marks an empty slot.
static ChunkFn chunk_for(u32 pc) {
    static struct { u32 pc; ChunkFn fn; } cache[4096];
    u32 slot = (pc >> 2) & 4095;
    if (cache[slot].pc != pc) {
        cache[slot].pc = pc;
        cache[slot].fn = chunk_in(kChunks, NCHUNKS, pc);
    }
    return cache[slot].fn;
}

// usage: rotk <disc.iso>                     play (boot files come from the disc)
//        rotk <sys dir|disc> <disc.iso> <symbols.tsv> [max_dispatches] [max_seconds]   test runs
// The whole machine for a checkpoint: CPU registers (everything before the host callbacks),
// RAM, the VM window and the devices.
static void machine_state(StateIO* io, CPUState* cpu) {
    state_io(io, cpu, offsetof(CPUState, external_read));
    state_io(io, cpu->ram, cpu->ram_size);
    state_io(io, g_fast_vm, g_fast_vm_size);
    hw_state(io);
    exi_state(io);
    ax_state(io);
}

// Runs between dispatches just after a draw-done, when the GX stream is at a frame boundary.
// Returns how far a load moved guest time, in seconds, so real-time pacing can follow.
static double checkpoint_service(CPUState* cpu, int op) {
    char game[6];
    memcpy(game, cpu->ram, sizeof game);
    if (op == CHECKPOINT_SAVE) {
        StateIO io = {.cap = 96u << 20};
        io.buf = malloc(io.cap);
        machine_state(&io, cpu);
        printf("checkpoint saved at %.2f s guest (%.1f MB)\n", guest_seconds(), io.pos / 1048576.0);
        checkpoint_write(&io, game);
        overlay_notify("Checkpoint saved");
        return 0;
    }
    StateIO size = {0};
    machine_state(&size, cpu);
    StateIO io = {0};
    if (!checkpoint_read(&io, game, size.pos)) {
        printf("checkpoint: none to load\n");
        overlay_notify("No checkpoint to load");
        return 0;
    }
    u64 tb = cpu->timebase;
    machine_state(&io, cpu);
    free(io.buf);
    audio_flush();
    printf("checkpoint loaded: %.2f s guest\n", guest_seconds());
    overlay_notify("Checkpoint loaded");
    return ((double)cpu->timebase - (double)tb) / 40500000.0;
}

static int emu_main(int argc, char** argv) {
    if (argc != 2 && argc < 4) {
        fprintf(stderr, "usage: %s <disc.iso>\n       %s <sysdir> <disc.iso> <symbols.tsv> [max_dispatches] [max_seconds]\n",
                argv[0], argv[0]);
        return 2;
    }
    const char* sys = argv[1];
    const char* disc = argc == 2 ? argv[1] : argv[2];
    long long limit = argc > 4 ? atoll(argv[4]) : (argc == 2 ? (long long)1 << 62 : 400000000);
    double max_seconds = argc > 5 ? atof(argv[5]) : (argc == 2 ? 1e12 : 30.0);
    double t0 = wall_seconds();
    // Real-time pacing: the guest clock may not run ahead of the wall clock (NO_THROTTLE=1 to measure raw speed).
    int throttle = !getenv("NO_THROTTLE");
    double slept = 0;
    if (argc > 3) load_syms(argv[3]);
    fn_trace_init();

    static CPUState cpu;
    if (!cpu_init(&cpu)) return 1;
    hw_init(disc);
    // SDK virtual memory window (VMInit): back it with flat memory instead of MMU paging from ARAM.
    g_fast_vm = calloc(1, 32u << 20);
    g_fast_vm_base = 0x7E000000u;
    g_fast_vm_size = 32u << 20;
    cpu_set_flat_vm(g_fast_vm, g_fast_vm_base, g_fast_vm_size);
    hw_attach(&cpu);
    u32 n;
    load_dol(&cpu, read_sys(sys, "main.dol", &n));
    fake_ipl(&cpu, sys);
    cpu.msr = 0x00002032u;                // FP | IR | DR | RI
    cpu.instruction_fallback = fallback;
    int use_gpu = !getenv("NO_GPU");
    if (use_gpu) {
        gpu_init(argc, argv, &cpu);
        hw_set_gpu_sink(gpu_consume);
        hw_set_frame_hook(gpu_frame_end, gpu_frames_completed);
    }

    // PC_HIST=<seconds>: after that much host time, count dispatches per function for a hot list.
    double hist_after = getenv("PC_HIST") ? atof(getenv("PC_HIST")) : -1;
    u32* hist = hist_after >= 0 ? calloc(g_nsyms, sizeof(u32)) : NULL;
    // Per-function host time and guest cycles, so the hot list ranks by cost rather than call count.
    u64* hist_ns = hist ? calloc(g_nsyms, sizeof(u64)) : NULL;
    u64* hist_cyc = hist ? calloc(g_nsyms, sizeof(u64)) : NULL;
    long hist_slot = -1;
    if (getenv("WATCH")) {
        g_watch_cpu = &cpu;
        sscanf(getenv("WATCH"), "%x,%x", &g_watch_lo, &g_watch_hi);
        if (getenv("WATCH_PC")) sscanf(getenv("WATCH_PC"), "%x,%x", &g_watch_pc_lo, &g_watch_pc_hi);
        g_watch_hi += g_watch_lo;
        ppc_set_mem_write_journal(watch_journal, NULL);
    }
    // TRACE_AT=addr[,addr...]: report dispatches to these addresses.
    u32 trace_list[16] = {0}, trace_count = 0;
    if (getenv("TRACE_AT"))
        for (char *t = getenv("TRACE_AT"), *end; *t && trace_count < 16; t = *end ? end + 1 : end) {
            trace_list[trace_count++] = (u32)strtoul(t, &end, 16);
            if (end == t) break;
        }
    int traces = 0, trace_max = getenv("TRACE_N") ? atoi(getenv("TRACE_N")) : 3;
    double threads_at = getenv("THREADS_AT") ? atof(getenv("THREADS_AT")) : 0;
    static u8 seen[0x16F580 / 4];
    u32 ring[32] = {0};
    long long steps = 0;
    u64 cycles = 0, interrupts[16] = {0};
    // ROTK_CPU_CLOCK=x overclocks the guest CPU: its code costs 1/x of the usual guest time
    // (the timebase normally ticks once per 12 CPU cycles).
    u64 cpu_oc_div = 12, oc_carry = 0;
    if (getenv("ROTK_CPU_CLOCK") && atof(getenv("ROTK_CPU_CLOCK")) >= 0.5)
        cpu_oc_div = (u64)(12 * atof(getenv("ROTK_CPU_CLOCK")) + 0.5);
    hle_init(chunk_for);
    g_clock_cpu = &cpu;
    input_set_clock(guest_seconds);
    int checkpoint_op = 0;
    u64 checkpoint_since = 0;
    double guest_jump = 0;   // guest seconds skipped by loading checkpoints, left out of the pacing
    // CHECKPOINT_AT=save@<s> or load@<s>: one checkpoint request at that guest time, for unattended tests.
    double checkpoint_at = 0;
    int checkpoint_auto = 0;
    if (getenv("CHECKPOINT_AT")) {
        char what[8] = {0};
        if (sscanf(getenv("CHECKPOINT_AT"), "%7[a-z]@%lf", what, &checkpoint_at) == 2)
            checkpoint_auto = !strcmp(what, "save") ? CHECKPOINT_SAVE : CHECKPOINT_LOAD;
    }
    const char* why = "dispatch limit";
    for (; steps < limit; steps++) {
        // RealMode() rfi's to a physical address; run it from the cached mirror.
        if (cpu.pc < 0x01800000u) cpu.pc |= 0x80000000u;

        // Asynchronous interrupts, taken between dispatches when MSR[EE] allows.
        if (cpu.msr & MSR_EE) {
            u32 exc = hw_decrementer_expired(&cpu) ? EXC_DECREMENTER : hw_external_pending() ? EXC_EXTERNAL : 0;
            if (exc && deliver(&cpu, exc, cpu.pc, cpu.msr)) interrupts[exc]++;
        }

        u32 pc = cpu.pc;
        ring[steps & 31] = pc;
        int traced = 0;
        for (u32 i = 0; i < trace_count; i++) traced |= pc == trace_list[i];
        if (traced && traces++ < trace_max) {
            if (!getenv("TRACE_ARGS")) backtrace(&cpu, "TRACE_AT");
            // TRACE_ARGS=1: argument registers and the first words r4 points at.
            if (getenv("TRACE_ARGS")) {
                printf("  %08X t=%.3fs r3=%08X r4=%08X r5=%08X r6=%08X f1=%g |", pc, cpu.timebase / 40500000.0, cpu.gpr[3],
                       cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.fpr[1]);
                for (u32 i = 0; i < 8 && cpu.gpr[4] >= 0x80000000u && cpu.gpr[4] < 0x81800000u; i++)
                    printf(" %08X", mem_read32(&cpu, cpu.gpr[4] + 4 * i));
                printf("\n");
            }
        }
        if (threads_at > 0 && cpu.timebase / 40500000.0 >= threads_at) {
            printf("threads at %.2fs guest:\n", cpu.timebase / 40500000.0);
            dump_threads(&cpu);
            threads_at = 0;
        }
        // Native stand-ins (cache maintenance, hle.c) charge their guest cycles like a dispatch.
        s64 used = spin_skip(&cpu);
        if (!used) used = hle_cache_range(&cpu);
        if (!used) used = hle_try(&cpu);
        if (used) goto charge;
        void (*fn)(CPUState*) = chunk_for(pc);
        if (!fn) { why = "pc left recompiled text"; break; }
        g_hle_skip = pc;   // no native version took this pc: let the generated code run through it
        u32 idx = (pc - 0x80000000u) / 4;
        if (idx < sizeof seen && !seen[idx]) {
            seen[idx] = 1;
            u32 off = 0;
            const char* s = sym_for(pc, &off);
            if (g_nsyms && off == 0 && strncmp(s, "fn_", 3) != 0) printf("reached %08X %s\n", pc, s);
        }
        if (hist && (steps & 0xFF) == 0 && wall_seconds() - t0 > hist_after) hist_after = 0;
        if (hist && hist_after == 0) {
            size_t lo = 0, hi = g_nsyms;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_syms[mid].addr <= pc) lo = mid + 1; else hi = mid; }
            if (lo) hist[lo - 1]++;
            hist_slot = lo ? (long)lo - 1 : -1;
        }
        u64 fn_t0 = hist_slot >= 0 ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
        int spin_entry = g_spin_cost == 0 && spin_flag_set(&cpu);
        cpu.downcount = 4096;
        fn(&cpu);
#ifdef ROTK_LLVM
        if (cpu.exception == EXC_COLD_ENTRY) {
            static u64 cold_entries;
            cold_entries++;
            if (getenv("COLD_LOG") && cold_entries < 40) fprintf(stderr, "cold entry at %08X\n", cpu.pc);
            cpu.exception = 0;
            void (*cfn)(CPUState*) = chunk_in(kCChunks, sizeof kCChunks / sizeof kCChunks[0], cpu.pc);
            if (cfn) {
                g_running_c = 1;
                g_hle_skip = cpu.pc;
                cfn(&cpu);
                g_running_c = 0;
            }
        }
#endif
        // Timebase ticks at bus/4 = CPU/12; charge the cycles this dispatch consumed.
        used = 4096 - cpu.downcount;
        if (used < 1) used = 1;
        spin_learn(spin_entry, &cpu, used);
        if (hist_slot >= 0) {
            hist_ns[hist_slot] += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - fn_t0;
            hist_cyc[hist_slot] += (u64)used;
            hist_slot = -1;
        }
    charge:
        cycles += (u64)used;
        if (cpu_oc_div == 12) {
            cpu.timebase += (u64)(used > 12 ? used / 12 : 1);
        } else {
            oc_carry += (u64)used;
            cpu.timebase += oc_carry / cpu_oc_div;
            oc_carry %= cpu_oc_div;
        }
        hw_tick(cpu.timebase);
        if ((steps & 0xFFF) == 0) {
            if (checkpoint_at > 0 && guest_seconds() >= checkpoint_at) {
                checkpoint_request(checkpoint_auto);
                checkpoint_at = 0;
            }
            if (!checkpoint_op && (checkpoint_op = checkpoint_take())) checkpoint_since = hw_draw_dones();
            if (checkpoint_op && hw_draw_dones() != checkpoint_since) {
                guest_jump += checkpoint_service(&cpu, checkpoint_op);
                checkpoint_op = 0;
            }
            double now = wall_seconds() - t0;
            if (now > max_seconds) { why = "time limit"; break; }
            int bench_stop = 0;
            bench_window(&cpu, &bench_stop);
            if (bench_stop) { why = "bench window end"; break; }
            double ahead = cpu.timebase / 40500000.0 - guest_jump - now;
            if (throttle && ahead > 0.002) {
                struct timespec ts = {0, (long)(ahead * 1e9)};
                nanosleep(&ts, NULL);
                slept += ahead;
            }
            // PERF_LOG=1: every 10 s, how fast the guest could have run in that window without pacing.
            // "cpu" is the same measure against this thread's CPU time, so other load on the Mac
            // affects it much less.
            static double mark_wall, mark_guest, mark_slept, mark_cpu;
            static u64 mark_frames;
            static int perf_log = -1;
            if (perf_log < 0) perf_log = getenv("PERF_LOG") != NULL;
            if (perf_log && now - mark_wall >= 10) {
                double g = cpu.timebase / 40500000.0;
                double tcpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e9;
                u64 frames = use_gpu ? gpu_frames_presented() : 0;
                printf("perf %5.0fs: capacity %.0f%% of real time, cpu %.0f%%, %.1f fps\n", now,
                       100.0 * (g - mark_guest) / (now - mark_wall - (slept - mark_slept)),
                       100.0 * (g - mark_guest) / (tcpu - mark_cpu), (frames - mark_frames) / (now - mark_wall));
                fflush(stdout);
                mark_wall = now, mark_guest = g, mark_slept = slept, mark_frames = frames, mark_cpu = tcpu;
            }
        }
        if (use_gpu && gpu_should_exit()) { why = "window closed"; break; }

        if (cpu.exception == PPC_EXC_SYSTEM_CALL) {
            // The OS's 0xC00 handler only syncs caches (it lives in low memory, not recompiled): resume after sc.
            cpu.pc = cpu.srr0;
            cpu.msr = cpu.srr1;
            cpu.exception = 0;
            continue;
        }
        if (cpu.exception == PPC_EXC_FP_UNAVAILABLE) {
            // Lazy FPU switching: let the OS's OSSwitchFPUContext save/restore FPRs per thread.
            if (deliver(&cpu, EXC_FP_UNAVAILABLE, cpu.srr0, cpu.srr1)) { interrupts[EXC_FP_UNAVAILABLE]++; continue; }
            why = "FP unavailable with no OS handler";
            break;
        }
        if (cpu.exception) { why = "exception raised"; break; }
    }

    double secs = wall_seconds() - t0;
    u32 off = 0;
    printf("\nstopped: %s after %lld dispatches (%.1fs host, %.2fs guest); pc=%08X (%s+0x%X) lr=%08X exception=0x%X\n",
           why, steps, secs, cpu.timebase / 40500000.0, cpu.pc, sym_for(cpu.pc, &off), off, cpu.lr, cpu.exception);
    printf("guest cycles: %llu\n", cycles);
    printf("speed: %.0f guest MHz (real Gekko: 486 MHz) = %.1f%% of real time\n",
           cycles / secs / 1e6, 100.0 * (cpu.timebase / 40500000.0) / secs);
    if (throttle)
        printf("pacing: slept %.1fs; unpaced capacity %.1f%% of real time\n", slept,
               100.0 * (cpu.timebase / 40500000.0) / (secs - slept));
    printf("fpscr=%08X hid2=%08X\n", cpu.fpscr, cpu.hid2);
    printf("interrupts delivered: external=%llu decrementer=%llu fp_unavailable=%llu\n",
           interrupts[EXC_EXTERNAL], interrupts[EXC_DECREMENTER], interrupts[EXC_FP_UNAVAILABLE]);
    backtrace(&cpu, "exit");
    if (hist) {
        u64 total_ns = 0, total_cyc = 0;
        for (size_t i = 0; i < g_nsyms; i++) total_ns += hist_ns[i], total_cyc += hist_cyc[i];
        printf("hot dispatch roots by host time (%.2fs host, %.2fs guest sampled; ns/cycle real Gekko = 2.06):\n",
               total_ns / 1e9, total_cyc / 486e6);
        printf("  %6s %10s %8s %10s  function\n", "host%", "dispatches", "ns/cyc", "Mcycles");
        for (int k = 0; k < 40; k++) {
            size_t best = 0;
            for (size_t i = 1; i < g_nsyms; i++) if (hist_ns[i] > hist_ns[best]) best = i;
            if (!hist_ns[best]) break;
            printf("  %5.1f%% %10u %8.2f %10.1f  %08X %s\n", 100.0 * hist_ns[best] / total_ns, hist[best],
                   hist_cyc[best] ? (double)hist_ns[best] / hist_cyc[best] : 0, hist_cyc[best] / 1e6,
                   g_syms[best].addr, g_syms[best].name);
            hist_ns[best] = 0;
        }
    }
    printf("recent dispatch pcs:");
    for (int i = 0; i < 32; i++) {
        u32 q = ring[(steps + 1 + i) & 31];
        if (q) { off = 0; printf(" %08X(%s+0x%X)", q, sym_for(q, &off), off); }
    }
    printf("\n");
    if (getenv("DUMP_MEM")) {   // DUMP_MEM="addr,len,path": raw guest memory at exit
        u32 a = 0, l = 0;
        char path[512] = {0};
        sscanf(getenv("DUMP_MEM"), "%x,%x,%511s", &a, &l, path);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(cpu.ram + (a & 0x01FFFFFF), 1, l, f); fclose(f); }
    }
    if (getenv("DUMP_WORDS")) {   // DUMP_WORDS="80250ba4,802519e4": guest words at exit
        char* list = strdup(getenv("DUMP_WORDS"));
        for (char* t = strtok(list, ","); t; t = strtok(NULL, ","))
            printf("word %08X = %08X\n", (u32)strtoul(t, NULL, 16), mem_read32(&cpu, (u32)strtoul(t, NULL, 16)));
        free(list);
    }
    if (g_watch_cpu) {
        printf("writers of %08X-%08X:\n", g_watch_lo, g_watch_hi);
        for (int k = 0; k < 30; k++) {
            int i = -1;
            for (int j = 0; j < 4096; j++) if (g_watch_pcs[j].count && (i < 0 || g_watch_pcs[j].count > g_watch_pcs[i].count)) i = j;
            if (i < 0) break;
            u32 o = 0;
            printf("  %10u  %08X %s+0x%X  -> %08X..%08X\n", g_watch_pcs[i].count, g_watch_pcs[i].pc,
                   sym_for(g_watch_pcs[i].pc, &o), o, g_watch_pcs[i].lo, g_watch_pcs[i].hi);
            g_watch_pcs[i].count = 0;
        }
    }
    for (u32 i = 0; i < g_fn_trace_count; i++) printf("fn %08X entered %u times\n", g_fn_trace[i], g_fn_trace_hits[i]);
    hw_report(sym_for);
    hle_report();
    if (use_gpu) {
        printf("frames presented: %llu\n", gpu_frames_presented());
        gpu_shutdown();
    }
    return 0;
}

// Launched without arguments (e.g. from the app bundle): use the disc chosen last time, or ask for
// it with the standard macOS file chooser and remember the answer.
static char* choose_disc(void) {
    static char path[2048];
    char saved[1024];
    const char* home = getenv("HOME");
    if (!home) return NULL;
    snprintf(saved, sizeof saved, "%s/Library/Application Support/rotk/disc_path", home);
    FILE* f = fopen(saved, "r");
    if (f) {
        int ok = fgets(path, sizeof path, f) != NULL;
        fclose(f);
        path[strcspn(path, "\n")] = 0;
        FILE* disc = ok ? fopen(path, "rb") : NULL;
        if (disc) { fclose(disc); return path; }
    }
    FILE* p = popen("osascript -e 'POSIX path of (choose file with prompt \"Choose your Lord of the Rings: "
                    "The Return of the King (GameCube) disc image\")' 2>/dev/null", "r");
    if (!p) return NULL;
    int ok = fgets(path, sizeof path, p) != NULL;
    pclose(p);
    if (!ok) return NULL;
    path[strcspn(path, "\n")] = 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/rotk", home);
    mkdir(dir, 0755);
    if ((f = fopen(saved, "w"))) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    return path;
}

int main(int argc, char** argv) {
    if (argc == 1) {
        char* disc = choose_disc();
        if (!disc) return 2;
        char* args[] = {argv[0], disc, NULL};
        return gpu_run(emu_main, 2, args);
    }
    return gpu_run(emu_main, argc, argv);
}
