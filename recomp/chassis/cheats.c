#include "cheats.h"
#include "ar_decrypt.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned g_cheat_points_on;   // read by ROTK_CHEAT_POINT in the generated code (hooks.h)

static const u32 kCheatPoints[] = {
#include "cheat_points.inc"
};

enum { MAX_CHEATS = 64, MAX_OPS = 256, MAX_LINES = 4096, MAX_PATCHES = 64 };

typedef struct { u32 addr, value; } ArOp;

typedef struct {
    char name[64];
    int header_line;   // index into g_lines, rewritten when the cheat is toggled
    ArOp ops[MAX_OPS];
    int nops;
    atomic_int on;
    char problem[96];
} Cheat;

static Cheat g_cheats[MAX_CHEATS];
static int g_ncheats;
static char* g_lines[MAX_LINES];
static int g_nlines;
static char g_path[1024];
static CPUState* g_cpu;
static int (*g_is_code)(u32 addr);

// Instructions replaced this frame.
static struct { u32 addr, word; } g_patches[MAX_PATCHES];
static int g_npatches;

static const char kTemplate[] =
    "# Action Replay cheats for The Return of the King (GKLE69, US).\n"
    "# Paste codes under a name line. \"[on]\" or \"[off]\" in front of the name sets whether the\n"
    "# cheat runs; F3 in the game (or Create + Triangle on a controller) toggles them too.\n"
    "# Both encrypted codes (XXXX-XXXX-XXXXX) and decrypted ones (XXXXXXXX YYYYYYYY) work. Leave\n"
    "# out master codes. Dolphin-style \"$Name\" lines also work and start off.\n"
    "#\n"
    "# [off] Example cheat\n"
    "# XXXX-XXXX-XXXXX\n";

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// "XXXX-XXXX-XXXXX" -> 13 characters.
static int encrypted_line(const char* s, char out[13]) {
    if (strlen(s) != 15 || s[4] != '-' || s[9] != '-') return 0;
    memcpy(out, s, 4);
    memcpy(out + 4, s + 5, 4);
    memcpy(out + 8, s + 10, 5);
    return 1;
}

static void finish(Cheat* c, char (*enc)[13], int nenc) {
    if (!nenc) return;
    u32 words[2 * MAX_OPS];
    int n = nenc <= MAX_OPS ? ar_decrypt((const char (*)[13])enc, nenc, words) : -1;
    if (n < 0) {
        snprintf(c->problem, sizeof c->problem, "code does not decrypt (typo?)");
        return;
    }
    for (int i = 0; i < n && c->nops < MAX_OPS; i++) c->ops[c->nops++] = (ArOp){words[2 * i], words[2 * i + 1]};
}

static void load_file(void) {
    FILE* f = fopen(g_path, "r");
    if (!f) {
        if ((f = fopen(g_path, "w"))) {
            fputs(kTemplate, f);
            fclose(f);
        }
        return;
    }
    char buf[512];
    static char enc[MAX_OPS][13];
    int nenc = 0;
    Cheat* c = NULL;
    while (g_nlines < MAX_LINES && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        int index = g_nlines;
        g_lines[g_nlines++] = strdup(buf);
        char* s = trim(buf);
        if (!*s || *s == '#') continue;
        int on = -1;
        const char* name = NULL;
        if (!strncmp(s, "[on]", 4)) on = 1, name = s + 4;
        else if (!strncmp(s, "[off]", 5)) on = 0, name = s + 5;
        else if (*s == '$') on = 0, name = s + 1;
        if (name) {
            if (c) finish(c, enc, nenc);
            nenc = 0;
            if (g_ncheats == MAX_CHEATS) { c = NULL; continue; }
            c = &g_cheats[g_ncheats++];
            snprintf(c->name, sizeof c->name, "%s", trim((char*)name));
            c->header_line = index;
            atomic_store(&c->on, on);
            continue;
        }
        if (!c) continue;
        u32 a, v;
        char e[13];
        if (encrypted_line(s, e)) {
            if (nenc < MAX_OPS) memcpy(enc[nenc++], e, 13);
        } else if (sscanf(s, "%8x %8x", &a, &v) == 2 && c->nops < MAX_OPS) {
            c->ops[c->nops++] = (ArOp){a, v};
        } else {
            snprintf(c->problem, sizeof c->problem, "unreadable line: %.40s", s);
        }
    }
    if (c) finish(c, enc, nenc);
    fclose(f);
}

void cheats_init(CPUState* cpu, int (*is_code)(u32 addr)) {
    g_cpu = cpu;
    g_is_code = is_code;
    const char* home = getenv("HOME");
    const char* path = getenv("ROTK_CHEATS");
    if (path) snprintf(g_path, sizeof g_path, "%s", path);
    else snprintf(g_path, sizeof g_path, "%s/Library/Application Support/rotk/cheats.txt", home ? home : ".");
    load_file();
    for (int i = 0; i < g_ncheats; i++)
        printf("cheat %s: %s (%d lines)%s%s\n", atomic_load(&g_cheats[i].on) ? "on " : "off", g_cheats[i].name,
               g_cheats[i].nops, g_cheats[i].problem[0] ? " - " : "", g_cheats[i].problem);
}

// ---- running codes (Dolphin's ActionReplay.cpp semantics, minus fill/slide and memory copy) ----

static int has_point(u32 addr) {
    for (size_t i = 0; i < sizeof kCheatPoints / sizeof kCheatPoints[0]; i++)
        if (kCheatPoints[i] == addr) return 1;
    return 0;
}

static int in_ram(u32 addr, u32 size) { return addr >= 0x80000000u && addr - 0x80000000u + size <= g_cpu->ram_size; }

static void write32(Cheat* c, u32 addr, u32 value) {
    if (g_is_code(addr)) {
        if (!has_point(addr)) {
            snprintf(c->problem, sizeof c->problem, "changes code at %08X: needs a patch point (README)", addr);
            return;
        }
        if (g_npatches < MAX_PATCHES) g_patches[g_npatches++] = (typeof(g_patches[0])){addr, value};
        return;
    }
    if (in_ram(addr, 4)) mem_write32(g_cpu, addr, value);
}

static void write16(u32 addr, u16 value) { if (in_ram(addr, 2) && !g_is_code(addr & ~3u)) mem_write16(g_cpu, addr, value); }
static void write8(u32 addr, u8 value) { if (in_ram(addr, 1) && !g_is_code(addr & ~3u)) mem_write8(g_cpu, addr, value); }
static u32 read(u32 addr, u32 size) {
    if (!in_ram(addr, size)) return 0;
    return size == 1 ? mem_read8(g_cpu, addr) : size == 2 ? mem_read16(g_cpu, addr) : mem_read32(g_cpu, addr);
}

static int compare(u32 a, u32 b, u32 type) {
    switch (type) {
    case 1: return a == b;
    case 2: return a != b;
    case 3: return (s32)a < (s32)b;
    case 4: return (s32)a > (s32)b;
    case 5: return a < b;
    case 6: return a > b;
    default: return (a & b) != 0;
    }
}

static void run(Cheat* c) {
    int skip = 0;   // >0: lines to skip; -2: until an endif line; -3: the rest
    for (int i = 0; i < c->nops; i++) {
        u32 cmd = c->ops[i].addr, data = c->ops[i].value;
        if (skip > 0) { skip--; continue; }
        if (skip == -3) return;
        if (skip == -2) {
            if (cmd == 0 && data == 0x40000000u) skip = 0;
            continue;
        }
        if (cmd == 0) {   // zero codes
            u32 z = data >> 29;
            if (z == 0) return;   // end of codes
            if (z == 2) continue;
            snprintf(c->problem, sizeof c->problem, "uses a code type this build does not support");
            return;
        }
        u32 addr = (cmd & 0x01FFFFFFu) | 0x80000000u, size = (cmd >> 25) & 3, type = (cmd >> 27) & 7, sub = cmd >> 30;
        if (type == 0) {
            if (sub == 3) continue;   // master code: not needed
            if (sub == 0) {           // RAM write and fill
                if (size == 0) for (u32 k = 0; k <= data >> 8; k++) write8(addr + k, (u8)data);
                else if (size == 1) for (u32 k = 0; k <= data >> 16; k++) write16(addr + 2 * k, (u16)data);
                else write32(c, addr, data);
            } else if (sub == 1) {    // write to pointer
                u32 ptr = read(addr, 4);
                if (size == 0) write8(ptr + (data >> 8), (u8)data);
                else if (size == 1) write16(ptr + ((data >> 16) << 1), (u16)data);
                else write32(c, ptr, data);
            } else {                  // add
                if (size == 0) write8(addr, (u8)(read(addr, 1) + data));
                else if (size == 1) write16(addr, (u16)(read(addr, 2) + data));
                else if (size == 2) write32(c, addr, read(addr, 4) + data);
                else {
                    u32 bits = read(addr, 4);
                    float f;
                    memcpy(&f, &bits, 4);
                    f += (float)data;
                    memcpy(&bits, &f, 4);
                    write32(c, addr, bits);
                }
            }
            continue;
        }
        u32 have = size == 0 ? read(addr, 1) : size == 1 ? read(addr, 2) : read(addr, 4);
        u32 want = size == 0 ? (data & 0xFF) : size == 1 ? (data & 0xFFFF) : data;
        if (!compare(have, want, type)) skip = sub <= 1 ? (int)sub + 1 : -(int)sub;
    }
}

void cheats_frame(void) {
    g_npatches = 0;
    for (int i = 0; i < g_ncheats; i++)
        if (atomic_load(&g_cheats[i].on) && !g_cheats[i].problem[0]) run(&g_cheats[i]);
    g_cheat_points_on = g_npatches > 0;
}

// ---- replaced instructions ----

// Runs one instruction of the kinds cheats put in code: integer and float loads and stores, li/addi,
// lis/addis, ori/oris (nop is ori 0,0,0) and relative branches. Returns the next pc, or 0.
static u32 exec(CPUState* c, u32 pc, u32 w) {
    u32 op = w >> 26, rd = (w >> 21) & 31, ra = (w >> 16) & 31, uimm = w & 0xFFFF;
    s32 simm = (s16)uimm;
    u32 base = ra ? c->gpr[ra] : 0, ea = base + (u32)simm;
    switch (op) {
    case 14: c->gpr[rd] = base + (u32)simm; break;
    case 15: c->gpr[rd] = base + (uimm << 16); break;
    case 24: c->gpr[ra] = c->gpr[rd] | uimm; break;
    case 25: c->gpr[ra] = c->gpr[rd] | (uimm << 16); break;
    case 32: c->gpr[rd] = mem_read32(c, ea); break;
    case 34: c->gpr[rd] = mem_read8(c, ea); break;
    case 40: c->gpr[rd] = mem_read16(c, ea); break;
    case 42: c->gpr[rd] = (u32)(s32)(s16)mem_read16(c, ea); break;
    case 36: mem_write32(c, ea, c->gpr[rd]); break;
    case 38: mem_write8(c, ea, (u8)c->gpr[rd]); break;
    case 44: mem_write16(c, ea, (u16)c->gpr[rd]); break;
    case 48: {
        u32 bits = mem_read32(c, ea);
        float f;
        memcpy(&f, &bits, 4);
        c->fpr[rd] = c->ps1[rd] = f;
        break;
    }
    case 52: {
        float f = (float)c->fpr[rd];
        u32 bits;
        memcpy(&bits, &f, 4);
        mem_write32(c, ea, bits);
        break;
    }
    case 18:
        if (w & 3) return 0;   // absolute or linking branches
        return pc + (u32)(((s32)(w << 6)) >> 6 & ~3);
    default: return 0;
    }
    return pc + 4;
}

s64 cheats_try(CPUState* cpu) {
    if (!g_cheat_points_on) return 0;
    for (int i = 0; i < g_npatches; i++) {
        if (g_patches[i].addr != cpu->pc) continue;
        // CHEAT_LOG=1: registers around each replaced instruction (first 40), to see what a code does.
        static int log = -1, logged;
        if (log < 0) log = getenv("CHEAT_LOG") != NULL;
        if (log && logged++ < 40)
            printf("cheat %08X: r0=%d r3=%d r28=%08X r30=%08X\n", cpu->pc, cpu->gpr[0], cpu->gpr[3], cpu->gpr[28], cpu->gpr[30]);
        u32 next = exec(cpu, cpu->pc, g_patches[i].word);
        if (!next) {
            static int warned;
            if (!warned++) fprintf(stderr, "cheat: cannot run instruction %08X at %08X\n", g_patches[i].word, cpu->pc);
            return 0;
        }
        static u32 seen[MAX_PATCHES];
        int known = 0;
        for (int k = 0; k < MAX_PATCHES && seen[k] && !known; k++) known = seen[k] == cpu->pc;
        for (int k = 0; k < MAX_PATCHES && !known; k++)
            if (!seen[k]) {
                seen[k] = cpu->pc;
                printf("cheat: replaced instruction at %08X runs\n", cpu->pc);
                break;
            }
        cpu->pc = next;
        return 1;
    }
    return 0;
}

// ---- menu ----

int cheats_count(void) { return g_ncheats; }
const char* cheats_name(int i) { return g_cheats[i].name; }
int cheats_enabled(int i) { return atomic_load(&g_cheats[i].on); }
const char* cheats_problem(int i) { return g_cheats[i].problem[0] ? g_cheats[i].problem : NULL; }

void cheats_toggle(int i) {
    if (i < 0 || i >= g_ncheats) return;
    Cheat* c = &g_cheats[i];
    int on = !atomic_load(&c->on);
    atomic_store(&c->on, on);
    char line[128];
    snprintf(line, sizeof line, "%s %s", on ? "[on]" : "[off]", c->name);
    free(g_lines[c->header_line]);
    g_lines[c->header_line] = strdup(line);
    FILE* f = fopen(g_path, "w");
    if (!f) return;
    for (int k = 0; k < g_nlines; k++) fprintf(f, "%s\n", g_lines[k]);
    fclose(f);
}
