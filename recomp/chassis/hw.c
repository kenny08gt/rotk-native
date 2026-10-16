#include "hw.h"
#include "input.h"
#include "ax_hle.h"
#include "audio.h"
#include "exi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- register file for 0xCC000000-0xCC00FFFF (big-endian bytes) ----
static u8 g_hw[0x10000];
static CPUState* g_cpu;
static FILE* g_iso;
static u8* g_aram;
#define ARAM_SIZE (16u * 1024 * 1024)

static u64 get(u32 off, u8 size) {
    u64 v = 0;
    for (u8 i = 0; i < size; i++) v = v << 8 | g_hw[(off + i) & 0xFFFF];
    return v;
}

static void put(u32 off, u64 v, u8 size) {
    for (u8 i = 0; i < size; i++) g_hw[(off + size - 1 - i) & 0xFFFF] = (u8)(v >> (8 * i));
}

static u32 get32(u32 off) { return (u32)get(off, 4); }
static u16 get16(u32 off) { return (u16)get(off, 2); }
static void put32(u32 off, u32 v) { put(off, v, 4); }
static void put16(u32 off, u16 v) { put(off, v, 2); }

// DMA_TRAP=<guest addr>: report any device DMA that writes over it (CPU stores show up in WATCH).
void hw_dma_trap(const char* who, u32 phys, u32 len) {
    static long trap = -1;
    if (trap == -1) trap = getenv("DMA_TRAP") ? (long)(strtoul(getenv("DMA_TRAP"), NULL, 16) & 0x01FFFFFFu) : -2;
    if (trap >= 0 && (u32)trap - phys < len)
        fprintf(stderr, "DMA_TRAP: %s wrote %08X..%08X (t=%.3fs, pc=%08X)\n", who, phys, phys + len,
                g_cpu->timebase / 40500000.0, g_cpu->pc);
}

// True if a write of `size` bytes at `off` covers register byte `reg`.
static int covers(u32 off, u8 size, u32 reg) { return reg >= off && reg < off + size; }

// ---- MMIO log ----
#define MMIO_SLOTS 4096
static struct { u32 addr, reads, writes, last_pc; } g_log[MMIO_SLOTS];

static void note(u32 ea, int is_write) {
    u32 key = ea & 0x0FFFFFFF;
    for (u32 i = 0; i < MMIO_SLOTS; i++) {
        u32 slot = (key * 2654435761u + i) % MMIO_SLOTS;
        if (g_log[slot].addr == 0 || g_log[slot].addr == key) {
            g_log[slot].addr = key;
            if (is_write) g_log[slot].writes++; else g_log[slot].reads++;
            g_log[slot].last_pc = g_cpu->pc;
            return;
        }
    }
}

// ---- DSP / ARAM (0x5000) ----
enum {
    DSP_MBOX_IN_H = 0x5000, DSP_MBOX_OUT_H = 0x5004, DSP_MBOX_OUT_L = 0x5006, DSP_CSR = 0x500A,
    AR_MM_ADDR = 0x5020, AR_AR_ADDR = 0x5024, AR_CNT = 0x5028,
};
enum {
    CSR_RES = 0x0001, CSR_HALT = 0x0004, CSR_AIDINT = 0x0008, CSR_AIDINTMSK = 0x0010,
    CSR_ARINT = 0x0020, CSR_ARINTMSK = 0x0040, CSR_DSPINT = 0x0080, CSR_DSPINTMSK = 0x0100,
    CSR_INIT_BUSY = 0x0400, CSR_W1C = CSR_AIDINT | CSR_ARINT | CSR_DSPINT,
};

static void aram_dma(void) {
    u32 mm = get32(AR_MM_ADDR) & 0x01FFFFE0u, ar = get32(AR_AR_ADDR) & (ARAM_SIZE - 1), cnt = get32(AR_CNT);
    u32 len = cnt & 0x7FFFFFE0u;
    if (mm + len > g_cpu->ram_size || ar + len > ARAM_SIZE) len = 0;
    if (cnt & 0x80000000u) {   // ARAM -> main RAM
        hw_dma_trap("ARAM", mm, len);
        memcpy(g_cpu->ram + mm, g_aram + ar, len);
    }
    else memcpy(g_aram + ar, g_cpu->ram + mm, len);                     // main RAM -> ARAM
    put16(DSP_CSR, get16(DSP_CSR) | CSR_ARINT);
}

// DSP mailboxes. Mail to the DSP is consumed at once; replies queue in the DSP->CPU box.
// The boot ROM greets with 0x8071FEED and takes a task as 0x80F3xxxx/value mail pairs, ending with
// 0x80F3D001 + start vector. AX (HLE) then runs; between command lists it may hand the DSP to another
// task (MAIL_NEW_UCODE + ten description mails) and stay resident. The only other task this game
// runs is the CARD unlock helper, answered like Dolphin's CARDUCode: done, back to the ROM.
enum { DSP_ROM, DSP_AX, DSP_UPLOAD, DSP_CARD };
enum { UCODE_AX = 0xE2136399u, UCODE_CARD = 0x65D6CC6Fu };
static u32 g_dsp_out[64];
static u32 g_dsp_out_head, g_dsp_out_tail;
static u32 g_dsp_prev_in, g_dsp_rom_addr, g_dsp_rom_len;
static int g_dsp_state, g_dsp_ax_resident;
static u32 g_dsp_upload[10], g_dsp_upload_n;
static u64 g_dsp_mails_in;

static void dsp_post(u32 mail, int irq) {
    g_dsp_out[g_dsp_out_tail++ % 64] = mail;
    if (irq) put16(DSP_CSR, get16(DSP_CSR) | CSR_DSPINT);
    if (getenv("DSP_LOG")) fprintf(stderr, "DSP-> %08X%s\n", mail, irq ? " (irq)" : "");
}

static void dsp_reset_run(void) {
    g_dsp_out_head = g_dsp_out_tail = 0;
    g_dsp_prev_in = 0;
    g_dsp_state = DSP_ROM;
    g_dsp_ax_resident = 0;
    dsp_post(0x8071FEED, 0);
}

// Dolphin's HashEctor over the task's IRAM image: identifies which microcode the game loads.
static u32 dsp_ucode_hash(u32 addr, u32 len) {
    addr &= 0x01FFFFFFu;
    if (addr + len > g_cpu->ram_size) return 0;
    u32 crc = 0;
    for (u32 i = 0; i < len; i++) {
        crc ^= g_cpu->ram[addr + i];
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

static void dsp_start_task(u32 crc) {
    if (getenv("DSP_LOG")) fprintf(stderr, "DSP task %08X\n", crc);
    if (crc == UCODE_CARD) {
        g_dsp_state = DSP_CARD;
        dsp_post(0xDCD10000u, 1);   // DSP_INIT
        return;
    }
    if (crc != UCODE_AX) fprintf(stderr, "warning: unknown DSP microcode %08X, treating it as AX\n", crc);
    g_dsp_state = DSP_AX;
    if (g_dsp_ax_resident) ax_resume();
    else ax_start();
    g_dsp_ax_resident = 1;
}

static void dsp_mail_in(u32 mail) {
    g_dsp_mails_in++;
    if (getenv("DSP_LOG")) fprintf(stderr, "DSP<- %08X\n", mail);
    switch (g_dsp_state) {
    case DSP_AX: {
        int r = ax_mail(mail);
        if (r == AX_NEW_UCODE) { g_dsp_state = DSP_UPLOAD; g_dsp_upload_n = 0; }
        else if (r == AX_RESET) { g_dsp_state = DSP_ROM; g_dsp_prev_in = 0; g_dsp_ax_resident = 0; }
        return;
    }
    case DSP_UPLOAD:   // mram dest/size/dram, iram mram addr/size/dest/startpc, dram mram/size/dest
        g_dsp_upload[g_dsp_upload_n++] = mail;
        if (g_dsp_upload_n == 10) dsp_start_task(dsp_ucode_hash(g_dsp_upload[3], g_dsp_upload[4] & 0xFFFF));
        return;
    case DSP_CARD:     // 0xFF000000 = unlock; the HLE has nothing to compute
        dsp_post(0xDCD10003u, 1);   // DSP_DONE
        g_dsp_state = DSP_ROM;
        g_dsp_prev_in = 0;
        return;
    }
    if (g_dsp_prev_in == 0x80F3A001u) g_dsp_rom_addr = mail;
    if (g_dsp_prev_in == 0x80F3A002u) g_dsp_rom_len = mail & 0xFFFF;
    if (g_dsp_prev_in == 0x80F3D001u) {   // boot-task upload ends with the start vector
        dsp_start_task(dsp_ucode_hash(g_dsp_rom_addr, g_dsp_rom_len));
        return;
    }
    g_dsp_prev_in = mail;
}

// AI DMA (0x5030-0x503A): the DSP output buffer streams to the audio DAC at 32 kHz, 16-bit stereo.
// The block is replayed continuously; each completion raises AIDINT, which drives AX's frame callback.
enum { AI_DMA_HI = 0x5030, AI_DMA_LO = 0x5032, AI_DMA_LEN = 0x5036, AI_DMA_LEFT = 0x503A, AIDMA_START = 0x8000 };
static u64 g_aid_next_tb, g_aid_blocks;
static u32 g_aid_block_addr;

static void aid_latch(void) { g_aid_block_addr = ((u32)get16(AI_DMA_HI) << 16 | get16(AI_DMA_LO)) & 0x03FFFFE0u; }

static u64 aid_block_ticks(void) {
    u32 bytes = (get16(AI_DMA_LEN) & 0x7FFF) * 32u;
    return (u64)bytes / 4 * 40500000ull / 32000;   // bytes -> stereo samples -> timebase ticks
}

static void aid_tick(u64 tb) {
    if (!(get16(AI_DMA_LEN) & AIDMA_START) || !(get16(AI_DMA_LEN) & 0x7FFF)) { g_aid_next_tb = 0; return; }
    if (!g_aid_next_tb) { g_aid_next_tb = tb + aid_block_ticks(); aid_latch(); }
    while (tb >= g_aid_next_tb) {
        u32 bytes = (get16(AI_DMA_LEN) & 0x7FFF) * 32u;
        if (g_aid_block_addr + bytes <= g_cpu->ram_size) audio_push_ai_block(g_cpu->ram + g_aid_block_addr, bytes);
        aid_latch();   // the next block plays whatever address is programmed now
        put16(DSP_CSR, get16(DSP_CSR) | CSR_AIDINT);
        g_aid_blocks++;
        g_aid_next_tb += aid_block_ticks();
    }
}

static u32 dsp_pending(void) {
    u16 c = get16(DSP_CSR);
    return ((c & CSR_AIDINT) && (c & CSR_AIDINTMSK)) || ((c & CSR_ARINT) && (c & CSR_ARINTMSK)) ||
           ((c & CSR_DSPINT) && (c & CSR_DSPINTMSK)) ? PI_INT_DSP : 0;
}

// ---- DI (0x6000) ----
enum {
    DI_SR = 0x6000, DI_CVR = 0x6004, DI_CMD0 = 0x6008, DI_CMD1 = 0x600C, DI_CMD2 = 0x6010,
    DI_MAR = 0x6014, DI_LEN = 0x6018, DI_CR = 0x601C, DI_IMM = 0x6020,
};
enum { DISR_DEINTMSK = 0x02, DISR_DEINT = 0x04, DISR_TCINTMSK = 0x08, DISR_TCINT = 0x10, DISR_W1C = 0x54 };

static u64 g_di_reads, g_di_bytes;
static u64 g_di_done_at;   // timebase when the in-flight read completes (0 = idle)

static void di_complete(void) {
    put32(DI_LEN, 0);
    put32(DI_CR, get32(DI_CR) & ~1u);
    put32(DI_SR, get32(DI_SR) | DISR_TCINT);
}

// Reads take drive time, like the hardware and Dolphin: some loaders behave differently when a read
// is already finished by the time they look. ~0.5 ms seek plus ~3 MB/s; DI_INSTANT=1 completes at once.
static u64 di_read_ticks(u32 len) {
    static int instant = -1;
    if (instant < 0) instant = getenv("DI_INSTANT") != NULL;
    if (instant) return 0;
    return 40500000ull / 2000 + (u64)len * 40500000ull / (3u << 20);
}

static void di_command(void) {
    u32 cmd = get32(DI_CMD0), arg1 = get32(DI_CMD1), arg2 = get32(DI_CMD2);
    u32 mar = get32(DI_MAR) & 0x01FFFFE0u, len = get32(DI_LEN);
    switch (cmd >> 24) {
    case 0xA8: {   // read: disc ID (sub-command 0x40) or data at arg1 << 2
        u64 disc_off = (cmd & 0xC0) == 0x40 ? 0 : (u64)arg1 << 2;
        if (mar + len <= g_cpu->ram_size && g_iso) {
            fseeko(g_iso, (off_t)disc_off, SEEK_SET);
            hw_dma_trap("DI", mar, len);
            size_t got = fread(g_cpu->ram + mar, 1, len, g_iso);
            if (got < len) memset(g_cpu->ram + mar + got, 0, len - got);
            g_di_reads++;
            g_di_bytes += len;
            if (getenv("DI_LOG")) fprintf(stderr, "DI read %09llx %u t=%.3f\n", (unsigned long long)disc_off, len, g_cpu->timebase / 40500000.0);
        }
        (void)arg2;
        u64 ticks = di_read_ticks(len);
        if (ticks) {   // data is in place now; the transfer-complete interrupt comes later
            g_di_done_at = g_cpu->timebase + ticks;
            return;
        }
        break;
    }
    case 0x12:     // inquiry: drive info block
        if (mar + 0x20 <= g_cpu->ram_size) memset(g_cpu->ram + mar, 0, 0x20);
        break;
    case 0xE0:     // request error: no error
        put32(DI_IMM, 0);
        break;
    default:       // seek, stop motor, audio streaming setup, ...: complete with no data
        if (getenv("DI_LOG")) fprintf(stderr, "DI cmd %08X %08X %08X\n", cmd, arg1, arg2);
        break;
    }
    di_complete();
}

static u32 di_pending(void) {
    u32 s = get32(DI_SR);
    return ((s & DISR_TCINT) && (s & DISR_TCINTMSK)) || ((s & DISR_DEINT) && (s & DISR_DEINTMSK)) ? PI_INT_DI : 0;
}

// ---- SI (0x6400): controller port 0 has a standard controller fed from host input ----
enum {
    SI_POLL = 0x6430, SI_COMCSR = 0x6434, SI_SR = 0x6438, SI_IOBUF = 0x6480,
    SICOM_TCINT = 0x80000000u, SICOM_TCINTMSK = 0x40000000u, SICOM_COMERR = 0x20000000u, SICOM_TSTART = 1u,
    SISR_NOREP = 0x08, SISR_RDST = 0x20, SISR_ERRORS = 0x0F,
};
static u64 g_si_transfers;

static int si_connected(u32 ch) { u8 tmp[8]; return input_poll(ch, tmp); }
static u32 si_field(u32 ch, u32 bits) { return bits << (24 - 8 * ch); }

static void si_transfer(u32 csr) {
    u32 ch = (csr >> 1) & 3, inlen = (csr >> 8) & 0x7F;
    if (inlen == 0) inlen = 128;
    u8* io = g_hw + SI_IOBUF;
    u8 cmd = io[0];
    g_si_transfers++;
    if (getenv("SI_LOG")) fprintf(stderr, "SI transfer #%llu ch=%u cmd=%02X csr=%08X tb=%.3fs %s\n", g_si_transfers, ch, cmd,
                                  csr, g_cpu->timebase / 40500000.0, si_connected(ch) ? "-> controller" : "-> no reply");
    if (!si_connected(ch)) {
        put32(SI_SR, get32(SI_SR) | si_field(ch, SISR_NOREP));
        put32(SI_COMCSR, (csr & ~SICOM_TSTART) | SICOM_TCINT | SICOM_COMERR);
        return;
    }
    memset(io, 0, inlen);
    switch (cmd) {
    case 0x00: case 0xFF:   // type / reset: standard controller
        io[0] = 0x09; io[1] = 0x00; io[2] = 0x00;
        break;
    case 0x41: case 0x42: { // origin / recalibrate: neutral sticks and triggers
        static const u8 origin[10] = {0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1F, 0x1F, 0x00, 0x00};
        memcpy(io, origin, inlen < 10 ? inlen : 10);
        break;
    }
    default:
        break;
    }
    put32(SI_COMCSR, (csr & ~(SICOM_TSTART | SICOM_COMERR)) | SICOM_TCINT);
}

// Poll response for channel ch: high word (status, buttons, stick) or low word (C-stick, triggers).
static u32 si_inbuf(u32 ch, int high) {
    u8 st[8];
    if (!input_poll(ch, st)) return high ? 0x80000000u : 0;   // ERRSTAT: no controller
    return high ? (u32)st[0] << 24 | (u32)st[1] << 16 | (u32)st[2] << 8 | st[3]
                : (u32)st[4] << 24 | (u32)st[5] << 16 | (u32)st[6] << 8 | st[7];
}

static u32 si_status(void) {
    u32 sr = get32(SI_SR), poll = get32(SI_POLL);
    for (u32 ch = 0; ch < 4; ch++)
        if ((poll >> (7 - ch)) & 1) {
            if (si_connected(ch)) sr |= si_field(ch, SISR_RDST);
            else sr |= si_field(ch, SISR_NOREP);
        }
    return sr;
}

static u32 si_pending(void) {
    u32 c = get32(SI_COMCSR);
    return (c & SICOM_TCINT) && (c & SICOM_TCINTMSK) ? PI_INT_SI : 0;
}

// ---- VI (0x2000) ----
enum { VI_DI0 = 0x2030, VI_DI_INT = 0x80000000u, VI_DI_ENB = 0x10000000u };
static u64 g_next_retrace;
static u64 g_retraces;
#define TB_PER_RETRACE (40500000ull / 60)

static u32 vi_pending(void) {
    for (u32 i = 0; i < 4; i++) {
        u32 d = get32(VI_DI0 + 4 * i);
        if ((d & VI_DI_INT) && (d & VI_DI_ENB)) return PI_INT_VI;
    }
    return 0;
}

// ---- AI (0x6C00): the sample counter advances while playing ----
enum { AI_CR = 0x6C00, AI_SCNT = 0x6C08, AICR_PSTAT = 0x01, AICR_AFR = 0x02, AICR_SCRESET = 0x20 };
static u64 g_ai_base_count, g_ai_base_tb;

static u64 ai_count_with(u32 cr) {
    if (!(cr & AICR_PSTAT)) return g_ai_base_count;
    u64 rate = (cr & AICR_AFR) ? 48000 : 32000;
    return g_ai_base_count + (g_cpu->timebase - g_ai_base_tb) * rate / 40500000ull;
}

static u64 ai_count(void) { return ai_count_with(get32(AI_CR)); }

static void ai_control(u32 old, u32 w) {
    g_ai_base_count = (w & AICR_SCRESET) ? 0 : ai_count_with(old);
    g_ai_base_tb = g_cpu->timebase;
    put32(AI_CR, w & ~AICR_SCRESET);
}

// ---- CP (0x0000): the stand-in GPU drains the FIFO instantly, so an armed breakpoint hits at once ----
enum { CP_SR = 0x0000, CP_CR = 0x0002, CPSR_GP_IDLE = 0x0C, CPSR_BP_INT = 0x10, CPCR_BP_EN = 0x02, CPCR_BP_INT_EN = 0x20 };
enum { PI_INT_CP = 1u << 11 };
static u64 g_cp_breakpoints;

static u32 cp_pending(void) {
    u16 cr = get16(CP_CR);
    return (cr & CPCR_BP_EN) && (cr & CPCR_BP_INT_EN) ? PI_INT_CP : 0;
}

// ---- GX FIFO: write-gather pipe -> CPU FIFO in RAM -> GPU drain ----
// WGPipe bytes land in guest RAM at the PI write pointer (wrapping base..end), which is right
// both for live rendering and for display-list recording (GXBeginDisplayList points the CPU
// FIFO at a RAM buffer and unlinks it from the GPU). While linked, the GPU drains read->write
// pointer at command-aligned sync points: the draw-done BP write and breakpoint arming.
enum { PI_FIFO_BASE = 0x300C, PI_FIFO_END = 0x3010, PI_FIFO_WP = 0x3014, PI_WP_WRAP = 0x04000000u };
enum { CP_FIFO_RW_DIST = 0x30, CP_FIFO_WP = 0x34, CP_FIFO_RP = 0x38, CP_FIFO_BP = 0x3C };
enum { CPCR_READ_EN = 0x01, CPCR_LINK = 0x10 };
enum { PE_INTSR = 0x100A, PE_FINISH_EN = 0x02, PE_TOKEN_INT = 0x04, PE_FINISH_INT = 0x08, PE_W1C = 0x0C };
enum { PI_INT_PE_TOKEN = 1u << 9, PI_INT_PE_FINISH = 1u << 10 };

static u64 g_fifo_tail;   // the last bytes written while linked, newest in the low byte
static u64 g_fifo_bytes, g_draw_dones, g_gpu_bytes, g_dl_bytes;
static hw_gpu_sink g_gpu_sink;

void hw_set_gpu_sink(hw_gpu_sink sink) { g_gpu_sink = sink; }

static hw_frame_hook g_frame_hook;
static hw_gpu_progress g_gpu_progress;
static u64 g_draw_done_ticket;   // frame still being drawn on the GPU thread; draw-done fires when it completes
void hw_set_frame_hook(hw_frame_hook hook, hw_gpu_progress progress) { g_frame_hook = hook; g_gpu_progress = progress; }

// CP FIFO pointers are split into 16-bit lo/hi halves.
static u32 cp_ptr(u32 off) { return (u32)get16(off) | (u32)(get16(off + 2) & 0x3FFF) << 16; }
static void cp_set_ptr(u32 off, u32 v) { put16(off, (u16)v); put16(off + 2, (u16)(v >> 16)); }

static int fifo_linked(void) { return (get16(CP_CR) & (CPCR_LINK | CPCR_READ_EN)) == (CPCR_LINK | CPCR_READ_EN); }

static void gpu_drain(void) {
    if (!fifo_linked()) return;
    u32 base = get32(PI_FIFO_BASE) & 0x03FFFFE0u, end = get32(PI_FIFO_END) & 0x03FFFFFFu;
    u32 wp = get32(PI_FIFO_WP) & 0x03FFFFFFu, rp = cp_ptr(CP_FIFO_RP);
    if (rp < base || rp > end) rp = base;
    if (rp == wp) return;
    // Commands may straddle the ring's wrap point, so join both halves before handing them over.
    static u8* staging;
    static u32 staging_cap;
    u32 len = 0;
    while (rp != wp) {
        u32 stop = wp >= rp ? wp : end + 1;
        u32 n = stop - rp;
        if (len + n > staging_cap) {
            staging_cap = (len + n) * 2;
            staging = realloc(staging, staging_cap);
        }
        memcpy(staging + len, g_cpu->ram + rp, n);
        len += n;
        rp = stop > end ? base : stop;
    }
    if (g_gpu_sink) g_gpu_sink(staging, len);
    g_gpu_bytes += len;
    cp_set_ptr(CP_FIFO_RP, rp);
    cp_set_ptr(CP_FIFO_WP, wp);
    cp_set_ptr(CP_FIFO_RW_DIST, 0);
}

static void fifo_byte(u8 b) {
    u32 wpr = get32(PI_FIFO_WP);
    u32 wp = wpr & 0x03FFFFFFu, end = get32(PI_FIFO_END) & 0x03FFFFFFu;
    if (wp < g_cpu->ram_size) g_cpu->ram[wp] = b;
    wp++;
    if (wp > end) { wp = get32(PI_FIFO_BASE) & 0x03FFFFE0u; wpr ^= PI_WP_WRAP; }
    put32(PI_FIFO_WP, (wpr & PI_WP_WRAP) | wp);
    g_fifo_bytes++;
    if (!fifo_linked()) {
        static u32 dl_start = ~0u, dl_logged;
        if (dl_start == ~0u || wp == (get32(PI_FIFO_BASE) & 0x03FFFFE0u) + 1) dl_start = wp - 1;
        if (++g_dl_bytes % 256 == 0 && dl_logged < 3 && getenv("FIFO_LOG")) {
            dl_logged++;
            fprintf(stderr, "display list at %08X, first bytes:", dl_start);
            for (u32 i = 0; i < 48; i++) fprintf(stderr, " %02X", g_cpu->ram[dl_start + i]);
            fprintf(stderr, "\n");
        }
        return;
    }

    g_fifo_tail = g_fifo_tail << 8 | b;
    if ((g_fifo_tail & 0xFFFFFFFFFFull) == 0x6145000002ull) {   // BP 0x45 = 2: draw done
        gpu_drain();
        g_draw_dones++;
        g_draw_done_ticket = g_frame_hook ? g_frame_hook() : 0;
        if (!g_draw_done_ticket) put16(PE_INTSR, get16(PE_INTSR) | PE_FINISH_INT);
    }
}

// A whole store to the gather pipe: same effect as fifo_byte per byte, with the write pointer kept in a
// register. Falls back to fifo_byte when the store wraps the ring, leaves RAM, or the FIFO is unlinked.
static void fifo_store(u64 v, u8 size) {
    u32 wpr = get32(PI_FIFO_WP);
    u32 wp = wpr & 0x03FFFFFFu, end = get32(PI_FIFO_END) & 0x03FFFFFFu;
    if (!fifo_linked() || wp + size > end || wp + size > g_cpu->ram_size) {
        for (int i = size - 1; i >= 0; i--) fifo_byte((u8)(v >> (8 * i)));
        return;
    }
    u64 tail = g_fifo_tail;
    for (int i = size - 1; i >= 0; i--) {
        u8 b = (u8)(v >> (8 * i));
        g_cpu->ram[wp++] = b;
        tail = tail << 8 | b;
        if ((tail & 0xFFFFFFFFFFull) == 0x6145000002ull) {
            put32(PI_FIFO_WP, (wpr & PI_WP_WRAP) | wp);
            g_fifo_bytes += (u64)(size - i);
            g_fifo_tail = tail;
            gpu_drain();
            g_draw_dones++;
            g_draw_done_ticket = g_frame_hook ? g_frame_hook() : 0;
            if (!g_draw_done_ticket) put16(PE_INTSR, get16(PE_INTSR) | PE_FINISH_INT);
            for (i--; i >= 0; i--) fifo_byte((u8)(v >> (8 * i)));
            return;
        }
    }
    put32(PI_FIFO_WP, (wpr & PI_WP_WRAP) | wp);
    g_fifo_bytes += size;
    g_fifo_tail = tail;
}

static u32 pe_pending(void) {
    u16 sr = get16(PE_INTSR);
    return (sr & PE_FINISH_INT) && (sr & PE_FINISH_EN) ? PI_INT_PE_FINISH : 0;
}

// ---- PI (0x3000) ----
enum { PI_INTSR = 0x3000, PI_INTMR = 0x3004 };

static u32 pi_cause(void) {
    return di_pending() | si_pending() | dsp_pending() | vi_pending() | pe_pending() | cp_pending() | exi_pending() |
           PI_RESET_SWITCH_RELEASED;
}

int hw_external_pending(void) { return (pi_cause() & get32(PI_INTMR) & 0xFFFFu) != 0; }

void hw_tick(u64 tb) {
    aid_tick(tb);
    if (g_di_done_at && tb >= g_di_done_at) {
        g_di_done_at = 0;
        di_complete();
    }
    exi_tick(tb);
    if (g_draw_done_ticket && g_gpu_progress() >= g_draw_done_ticket) {
        g_draw_done_ticket = 0;
        put16(PE_INTSR, get16(PE_INTSR) | PE_FINISH_INT);
    }
    while (tb >= g_next_retrace) {
        g_next_retrace += TB_PER_RETRACE;
        g_retraces++;
        for (u32 i = 0; i < 4; i++) {
            u32 d = get32(VI_DI0 + 4 * i);
            if (d & VI_DI_ENB) put32(VI_DI0 + 4 * i, d | VI_DI_INT);
        }
    }
}

// ---- MMIO entry points ----
// Slice a computed 32-bit register at `base` for an access of `size` bytes at `off`.
static u64 slice32(u32 value, u32 base, u32 off, u8 size) {
    u32 shift = (base + 4 - (off + size)) * 8;
    return size >= 4 ? value : (value >> shift) & ((1u << (8 * size)) - 1);
}

// The SDK's virtual-memory window (flat host memory, see main.c). The C backend's helpers reach it
// through cpu_set_flat_vm; the LLVM backend's inline memory path only knows RAM and lands here.
extern u8* g_fast_vm;
extern u32 g_fast_vm_base, g_fast_vm_size;
static u8* vm_ptr(u32 ea, u8 size) {
    u32 off = ea - g_fast_vm_base;
    return g_fast_vm && off <= g_fast_vm_size - size ? g_fast_vm + off : NULL;
}

static u64 ext_read(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    u8* vm = vm_ptr(ea, size);
    if (vm) {
        u64 v = 0;
        for (u8 i = 0; i < size; i++) v = v << 8 | vm[i];
        return v;
    }
    if ((ea & 0xFFFF0000u) != 0xCC000000u) {
        static u32 logged;
        if (getenv("UNMAPPED_LOG") && logged++ < 20) fprintf(stderr, "unmapped read %08X size %u pc %08X\n", ea, size, cpu->pc);
        return 0;
    }
    note(ea, 0);
    u32 off = ea & 0xFFFF;
    if (off >= 0x6800 && off < 0x683C) return slice32(exi_read(off & ~3u), off & ~3u, off, size);
    // Computed registers, any access width.
    if (off >= SI_SR && off < SI_SR + 4) {
        u64 r = slice32(si_status(), SI_SR, off, size);
        static int logged;
        if (getenv("SI_LOG") && (get32(SI_POLL) & 0xF0) && logged++ < 6) fprintf(stderr, "SISR read off=%X size=%u -> %llX (poll %08X)\n", off, size, r, get32(SI_POLL));
        return r;
    }
    if (off >= 0x6404 && off < 0x6434 && (off - 0x6404) % 12 < 8) {
        u32 ch = (off - 0x6404) / 12, base = 0x6404 + ch * 12 + ((off - 0x6404) % 12 >= 4 ? 4 : 0);
        return slice32(si_inbuf(ch, base == 0x6404 + ch * 12), base, off, size);
    }
    switch (off) {
    case PI_INTSR:
        return size == 4 ? pi_cause() : 0;
    case CP_SR:   // GP idle; breakpoint flag mirrors an armed breakpoint
        return CPSR_GP_IDLE | ((get16(CP_CR) & CPCR_BP_EN) ? CPSR_BP_INT : 0);
    case DSP_CSR:   // reset completes immediately; the DSP is never busy initializing
        put16(DSP_CSR, get16(DSP_CSR) & ~(CSR_RES | CSR_INIT_BUSY));
        break;
    case DSP_MBOX_OUT_H: {
        if (g_dsp_out_head == g_dsp_out_tail) return 0;
        u32 m = g_dsp_out[g_dsp_out_head % 64];
        return size == 4 ? m | 0x80000000u : (m >> 16) | 0x8000u;
    }
    case DSP_MBOX_OUT_L: {
        if (g_dsp_out_head == g_dsp_out_tail) return 0;
        return g_dsp_out[g_dsp_out_head++ % 64] & 0xFFFF;
    }
    case AI_SCNT:
        return size == 4 ? (u32)ai_count() : 0;
    case 0x5016:    // ARAM controller ready
        return get(off, size) | 1;
    case 0x202C: {   // VI current position: derive the line from time since last retrace
        u64 into = TB_PER_RETRACE - (g_next_retrace - g_cpu->timebase) % TB_PER_RETRACE;
        u32 line = (u32)(into * 525 / TB_PER_RETRACE) + 1;
        return size == 4 ? (line << 16) | 1 : line;
    }
    }
    return get(off, size);
}

static void ext_write(CPUState* cpu, u32 ea, u64 v, u8 size) {
    (void)cpu;
    u8* vm = vm_ptr(ea, size);
    if (vm) {
        for (int i = size - 1; i >= 0; i--, v >>= 8) vm[i] = (u8)v;
        return;
    }
    if ((ea & 0xFFFFF000u) == 0xCC008000u) {   // write-gather pipe
        fifo_store(v, size);
        return;
    }
    note(ea, 1);
    if ((ea & 0xFFFF0000u) != 0xCC000000u) return;
    u32 off = ea & 0xFFFF;
    if (off >= 0x6800 && off < 0x683C) {   // EXI registers are only accessed as words
        exi_write(off & ~3u, (u32)v);
        return;
    }

    if (covers(off, size, DSP_CSR + 1)) {   // write-1-to-clear interrupt bits, others stored
        u16 old = get16(DSP_CSR);
        put(off, v, size);
        u16 w = get16(DSP_CSR);
        put16(DSP_CSR, (u16)((w & ~CSR_W1C) | (old & CSR_W1C & ~w)));
        if ((old & CSR_HALT) && !(w & CSR_HALT)) dsp_reset_run();
        return;
    }
    if (covers(off, size, DSP_MBOX_IN_H + 3)) {   // low half completes the mail
        put(off, v, size);
        u32 mail = get32(DSP_MBOX_IN_H);
        put16(DSP_MBOX_IN_H, get16(DSP_MBOX_IN_H) & 0x7FFF);   // consumed
        dsp_mail_in(mail);
        return;
    }
    if (covers(off, size, PE_INTSR + 1)) {
        u16 old = get16(PE_INTSR);
        put(off, v, size);
        u16 w = get16(PE_INTSR);
        put16(PE_INTSR, (u16)((w & ~PE_W1C) | (old & PE_W1C & ~w)));
        return;
    }
    if (covers(off, size, DI_SR + 3)) {
        u32 old = get32(DI_SR);
        put(off, v, size);
        u32 w = get32(DI_SR);
        put32(DI_SR, (w & ~DISR_W1C) | (old & DISR_W1C & ~w));
        return;
    }
    if (covers(off, size, SI_COMCSR)) {
        u32 old = get32(SI_COMCSR);
        put(off, v, size);
        u32 w = get32(SI_COMCSR);
        w = (w & ~SICOM_TCINT) | (old & SICOM_TCINT & ~(u32)(w & SICOM_TCINT));   // TCINT: write 1 to clear
        put32(SI_COMCSR, w);
        if (w & SICOM_TSTART) si_transfer(w);
        return;
    }
    if (covers(off, size, SI_SR)) {   // per-channel error bits: write 1 to clear
        u32 old = get32(SI_SR);
        put(off, v, size);
        u32 w = get32(SI_SR), errs = 0x0F0F0F0Fu;
        put32(SI_SR, (old & errs & ~w) | (w & ~errs & ~0x80000000u));
        return;
    }
    if (covers(off, size, AI_CR + 3)) {
        u32 old = get32(AI_CR);
        put(off, v, size);
        ai_control(old, get32(AI_CR));
        return;
    }
    put(off, v, size);
    if (covers(off, size, CP_CR + 1) && (get16(CP_CR) & CPCR_BP_EN)) {   // GPU reaches the breakpoint at once
        gpu_drain();
        g_cp_breakpoints++;
    }
    if (covers(off, size, AR_CNT + 3)) aram_dma();          // low half of the count starts the DMA
    if (covers(off, size, DI_CR + 3) && (get32(DI_CR) & 1)) di_command();
}

// ---- SPRs the runtime delegates (DEC, HID0/1, L2CR, WPAR, DMA, ...) ----
static u32 g_spr[1024];
static s64 g_dec_value;
static u64 g_dec_tb;

static u32 spr_read(CPUState* cpu, u16 spr, u32 cia) {
    (void)cia;
    if (spr == 22) return (u32)(g_dec_value - (s64)(cpu->timebase - g_dec_tb));
    return g_spr[spr & 1023];
}

static void spr_write(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    (void)cia;
    if ((spr == 922 || spr == 923) && getenv("DMA_LOG")) {
        static int n;
        if (n++ < 12) fprintf(stderr, "mtspr %u = %08X at %08X\n", spr, value, cia);
    }
    if (spr == 22) {
        g_dec_value = (s32)value;
        g_dec_tb = cpu->timebase;
        if (getenv("DEC_LOG")) fprintf(stderr, "mtdec %d at tb=%.4fs pc=%08X\n", (s32)value, cpu->timebase / 40500000.0, cia);
        return;
    }
    g_spr[spr & 1023] = value;
}

int hw_decrementer_expired(CPUState* cpu) {
    static u64 fired_for = ~0ull;
    if (g_dec_value - (s64)(cpu->timebase - g_dec_tb) >= 0 || fired_for == g_dec_tb) return 0;
    fired_for = g_dec_tb;
    return 1;
}

void hw_init(const char* iso_path) {
    g_iso = fopen(iso_path, "rb");
    if (!g_iso) perror(iso_path);
    g_aram = calloc(1, ARAM_SIZE);
    audio_init();
    g_next_retrace = TB_PER_RETRACE;
    g_spr[287] = 0x00083214;   // PVR: Gekko
}

void hw_attach(CPUState* cpu) {
    g_cpu = cpu;
    ax_init(cpu->ram, cpu->ram_size, g_aram, ARAM_SIZE, dsp_post);
    // Slot A memory card image: MEMCARD=<path> overrides, MEMCARD=none leaves the slot empty.
    static char card_path[1024];
    const char* mc = getenv("MEMCARD");
    if (mc && strcmp(mc, "none") != 0) snprintf(card_path, sizeof card_path, "%s", mc);
    else if (!mc && getenv("HOME")) snprintf(card_path, sizeof card_path, "%s/Library/Application Support/rotk/MemoryCardA.raw", getenv("HOME"));
    exi_init(cpu->ram, cpu->ram_size, card_path[0] ? card_path : NULL);
    cpu->external_read = ext_read;
    cpu->external_write = ext_write;
    cpu->spr_read = spr_read;
    cpu->spr_write = spr_write;
}

void hw_report(const char* (*sym_for)(u32 pc, u32* off)) {
    printf("devices: %llu VI retraces, %llu disc reads (%.1f MB), %llu mails to DSP, DSP task %s\n", g_retraces,
           g_di_reads, g_di_bytes / 1048576.0, g_dsp_mails_in, g_dsp_state == DSP_AX ? "running" : "not running");
    unsigned long long ax_lists, ax_voice_ms;
    ax_stats(&ax_lists, &ax_voice_ms);
    printf("AI: %llu DMA blocks played; AX: %llu command lists, %llu voice-milliseconds mixed\n", g_aid_blocks, ax_lists,
           ax_voice_ms);
    printf("SI: %llu controller transfers, poll=%08X status=%08X comcsr=%08X\n", g_si_transfers, get32(SI_POLL),
           si_status(), get32(SI_COMCSR));
    printf("GX FIFO: %.2f MB written (%.2f MB recorded into display lists), %.2f MB drained to GPU, "
           "%llu draw-done markers, %llu breakpoints armed\n", g_fifo_bytes / 1048576.0, g_dl_bytes / 1048576.0,
           g_gpu_bytes / 1048576.0, g_draw_dones, g_cp_breakpoints);
    printf("MMIO touched (addr reads writes last_pc):\n");
    for (u32 i = 0; i < MMIO_SLOTS; i++)
        if (g_log[i].addr) {
            u32 off = 0;
            const char* s = sym_for(g_log[i].last_pc, &off);
            printf("  %08X r=%u w=%u  %s+0x%X\n", g_log[i].addr | 0xC0000000u, g_log[i].reads, g_log[i].writes, s, off);
        }
}
