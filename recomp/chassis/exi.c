#include "exi.h"
#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Channel registers (EXI_Channel.cpp in Dolphin): CSR, DMA address, DMA length, control, immediate data.
enum { EXI_CSR = 0x00, EXI_MAR = 0x04, EXI_LEN = 0x08, EXI_CR = 0x0C, EXI_DATA = 0x10, EXI_STRIDE = 0x14 };
enum {
    CSR_EXIINTMASK = 1u << 0, CSR_EXIINT = 1u << 1, CSR_TCINTMASK = 1u << 2, CSR_TCINT = 1u << 3,
    CSR_CLK = 7u << 4, CSR_CS = 7u << 7, CSR_EXTINTMASK = 1u << 10, CSR_EXTINT = 1u << 11, CSR_EXT = 1u << 12,
    CSR_ROMDIS = 1u << 13,
};
enum { CR_TSTART = 1u << 0, CR_DMA = 1u << 1 };
enum { RW_READ = 0, RW_WRITE = 1, RW_READWRITE = 2 };

static struct { u32 csr, mar, len, cr, data; } g_ch[3];
static u8* g_ram;
static u32 g_ram_size;

// ---- Memory card (EXI_DeviceMemoryCard.cpp in Dolphin) ----
// A 16 Mbit (251 block) Nintendo card. The game formats it on first use.
#define CARD_MBITS 16u
#define CARD_SIZE (CARD_MBITS * 128u * 1024u)
enum {
    MC_STATUS_BUSY = 0x80, MC_STATUS_UNLOCKED = 0x40, MC_STATUS_SLEEP = 0x20, MC_STATUS_ERASEERROR = 0x10,
    MC_STATUS_PROGRAMEERROR = 0x08, MC_STATUS_READY = 0x01,
};
enum {
    CMD_NINTENDO_ID = 0x00, CMD_READ_ARRAY = 0x52, CMD_SET_INTERRUPT = 0x81, CMD_READ_STATUS = 0x83,
    CMD_READ_ID = 0x85, CMD_CLEAR_STATUS = 0x89, CMD_SECTOR_ERASE = 0xF1, CMD_PAGE_PROGRAM = 0xF2,
    CMD_CHIP_ERASE = 0xF4,
};

static struct {
    int present;
    u8* data;
    const char* path;
    int dirty;
    u8 command, status, interrupt_switch, interrupt_set;
    u32 position, address;
    u8 program_buffer[128];
    u64 done_at;   // timebase when a pending erase/program completes (0 = none)
} g_card;

static u64 g_now;

// ---- IPL device (channel 0, device 1): RTC + SRAM (EXI_DeviceIPL.cpp / Sram.cpp in Dolphin) ----
// A transfer starts with a 4-byte command (bit 31 = write, address = bits 6..30); SRAM lives at
// 0x800000 as 4 RTC bytes then the 64-byte settings block the OS DMAs in. The boot ROM and UART
// are not modelled: reads there leave memory untouched, as before.
#define SRAM_BASE 0x800000u
#define SRAM_SIZE 68u
static u8 g_sram[SRAM_SIZE] = {
    0, 0, 0, 0,                                  // RTC
    0x00, 0x2C, 0xFF, 0xD0,                      // checksum, checksum_inv over rtc_bias..flags
    0, 0, 0, 0, 0, 0, 0, 0,                      // ead0, ead1
    0, 0, 0, 0,                                  // rtc_bias
    0, 0, 0, 0x2C,                               // vi offset, ntd, language (English), flags: stereo | setup done
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'A',   // card flash IDs, slot A / B
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'B',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // wireless keyboard + pad ids
    0, 0, 0x6E, 0x6D,                            // DI error, pad, flash ID checksums A / B
    0, 0, 0, 0,                                  // GBS mode, pad
};
static struct { u32 command, received, cursor; } g_ipl;

// The CARD library only mounts a card whose flash ID (recovered from the card header's serial) is
// the one in SRAM, so derive it from the header the way Dolphin's SetCardFlashID does.
static void sram_set_card_flash_id(const u8* header) {
    u64 rand = 0;
    for (int i = 0; i < 8; i++) rand = rand << 8 | header[12 + i];
    u8 csum = 0;
    for (int i = 0; i < 12; i++) {
        rand = (rand * 0x41C64E6Dull + 0x3039ull) >> 16;
        csum += g_sram[24 + i] = (u8)(header[i] - (u8)rand);
        rand = ((rand * 0x41C64E6Dull + 0x3039ull) >> 16) & 0x7FFF;
    }
    g_sram[4 + 0x3A] = csum ^ 0xFF;
}

static void ipl_transfer_byte(u8* byte) {
    if (g_ipl.received < 4) {
        g_ipl.command = g_ipl.command << 8 | *byte;
        *byte = 0xFF;
        g_ipl.received++;
        return;
    }
    u32 address = (g_ipl.command & 0x7FFFFFFFu) >> 6;
    if (address < SRAM_BASE || address >= SRAM_BASE + SRAM_SIZE) return;
    u32 at = (address - SRAM_BASE + g_ipl.cursor++) % SRAM_SIZE;
    if (g_ipl.command & 0x80000000u) g_sram[at] = *byte;
    else *byte = g_sram[at];
}

static int ipl_addresses_sram(void) {
    u32 address = (g_ipl.command & 0x7FFFFFFFu) >> 6;
    return g_ipl.received == 4 && address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE;
}

void exi_flush(void) {
    if (!g_card.present || !g_card.dirty) return;
    FILE* f = fopen(g_card.path, "wb");
    if (!f) { perror(g_card.path); return; }
    fwrite(g_card.data, 1, CARD_SIZE, f);
    fclose(f);
    g_card.dirty = 0;
}

void exi_init(u8* ram, u32 ram_size, const char* card_path) {
    g_ram = ram;
    g_ram_size = ram_size;
    if (!card_path) return;   // empty slot
    g_card.present = 1;
    g_card.data = malloc(CARD_SIZE);
    memset(g_card.data, 0xFF, CARD_SIZE);   // erased flash
    g_card.path = card_path;
    FILE* f = fopen(card_path, "rb");
    if (f) {
        if (fread(g_card.data, 1, CARD_SIZE, f) != CARD_SIZE) fprintf(stderr, "%s: short card image\n", card_path);
        fclose(f);
    }
    g_card.status = MC_STATUS_BUSY | MC_STATUS_UNLOCKED | MC_STATUS_READY;
    sram_set_card_flash_id(g_card.data);
    atexit(exi_flush);
}

static void card_done_later(void) {
    g_card.done_at = g_now + 400;   // ~5000 CPU cycles, as Dolphin schedules it
}

static void card_set_cs(int selected) {
    if (selected) {
        g_card.position = 0;
        return;
    }
    switch (g_card.command) {
    case CMD_SECTOR_ERASE:
        if (g_card.position > 2) {
            memset(g_card.data + (g_card.address & (CARD_SIZE - 1) & ~0x1FFFu), 0xFF, 0x2000);
            g_card.dirty = 1;
            g_card.status = (g_card.status | MC_STATUS_BUSY) & ~MC_STATUS_READY;
            card_done_later();
        }
        break;
    case CMD_CHIP_ERASE:
        if (g_card.position > 2) {
            memset(g_card.data, 0xFF, CARD_SIZE);
            g_card.dirty = 1;
            g_card.status &= ~MC_STATUS_BUSY;
        }
        break;
    case CMD_PAGE_PROGRAM:
        if (g_card.position >= 5) {
            u32 count = g_card.position - 5, i = 0;
            g_card.status &= ~MC_STATUS_BUSY;
            while (count--) {
                g_card.data[g_card.address & (CARD_SIZE - 1)] = g_card.program_buffer[i++ & 127];
                g_card.address = (g_card.address & ~0x1FFu) | ((g_card.address + 1) & 0x1FFu);
            }
            g_card.dirty = 1;
            card_done_later();
        }
        break;
    }
}

static void card_transfer_byte(u8* byte) {
    if (g_card.position == 0) {
        g_card.command = *byte;
        *byte = 0xFF;
        if (g_card.command == CMD_CLEAR_STATUS) {
            g_card.status &= ~(MC_STATUS_PROGRAMEERROR | MC_STATUS_ERASEERROR);
            g_card.status |= MC_STATUS_READY;
            g_card.interrupt_set = 0;
            g_card.position = 0;
        }
    } else {
        u32 pos = g_card.position;
        switch (g_card.command) {
        case CMD_NINTENDO_ID:
            *byte = pos == 1 ? 0x80 : (u8)(CARD_MBITS >> (24 - ((pos - 2) & 3) * 8));
            break;
        case CMD_READ_ARRAY:
            if (pos == 1) { g_card.address = (u32)*byte << 17; *byte = 0xFF; }
            else if (pos == 2) g_card.address |= (u32)*byte << 9;
            else if (pos == 3) g_card.address |= (u32)(*byte & 3) << 7;
            else if (pos == 4) g_card.address |= *byte & 0x7F;
            if (pos > 1) {
                *byte = g_card.data[g_card.address & (CARD_SIZE - 1)];
                if (pos >= 9) g_card.address = (g_card.address & ~0x1FFu) | ((g_card.address + 1) & 0x1FFu);
            }
            break;
        case CMD_READ_STATUS:
            *byte = g_card.status;
            break;
        case CMD_READ_ID:   // Nintendo brand card id 0xC221
            *byte = pos == 1 ? 0xC2 : (pos & 1) ? 0x21 : 0xC2;
            break;
        case CMD_SECTOR_ERASE:
            if (pos == 1) g_card.address = (u32)*byte << 17;
            else if (pos == 2) g_card.address |= (u32)*byte << 9;
            *byte = 0xFF;
            break;
        case CMD_SET_INTERRUPT:
            if (pos == 1) g_card.interrupt_switch = *byte;
            *byte = 0xFF;
            break;
        case CMD_PAGE_PROGRAM:
            if (pos == 1) g_card.address = (u32)*byte << 17;
            else if (pos == 2) g_card.address |= (u32)*byte << 9;
            else if (pos == 3) g_card.address |= (u32)(*byte & 3) << 7;
            else if (pos == 4) g_card.address |= *byte & 0x7F;
            if (pos >= 5) g_card.program_buffer[(pos - 5) & 0x7F] = *byte;
            *byte = 0xFF;
            break;
        default:
            *byte = 0xFF;
            break;
        }
    }
    g_card.position++;
}

static int card_interrupt(void) { return g_card.interrupt_switch && g_card.interrupt_set; }

// ---- Channels ----
// The memory card is device 0 of channel 0; nothing else answers.
static int has_card(u32 ch, u32 cs) { return g_card.present && ch == 0 && (cs & 1); }

static void transfer(u32 ch) {
    u32 cr = g_ch[ch].cr, cs = (g_ch[ch].csr & CSR_CS) >> 7, rw = (cr >> 2) & 3;
    static int log = -1;
    if (log < 0) log = getenv("EXI_LOG") != NULL;
    if (log)
        fprintf(stderr, "EXI%u cs=%u %s rw=%u %s data=%08X mar=%08X len=%u pos=%u cmd=%02X\n", ch, cs,
                (cr & CR_DMA) ? "dma" : "imm", rw, has_card(ch, cs) ? "card" : "-", g_ch[ch].data, g_ch[ch].mar,
                (cr & CR_DMA) ? g_ch[ch].len : ((cr >> 4) & 3) + 1, g_card.position, g_card.command);
    if (ch == 0 && (cs & 2)) {
        if (!(cr & CR_DMA)) {
            u32 n = ((cr >> 4) & 3) + 1, d = g_ch[ch].data, out = 0;
            for (u32 i = 0; i < n; i++, d <<= 8) {
                u8 b = rw == RW_WRITE ? (u8)(d >> 24) : 0;
                ipl_transfer_byte(&b);
                out |= (u32)b << (24 - 8 * i);
            }
            if (rw == RW_READ && ipl_addresses_sram()) g_ch[ch].data = out;
        } else if (ipl_addresses_sram()) {
            u32 mar = g_ch[ch].mar & 0x01FFFFE0u, len = g_ch[ch].len;
            for (u32 i = 0; i < len && mar + i < g_ram_size; i++) {
                u8 b = rw == RW_WRITE ? g_ram[mar + i] : 0;
                ipl_transfer_byte(&b);
                if (rw == RW_READ) g_ram[mar + i] = b;
            }
        }
    } else if (has_card(ch, cs)) {
        if (!(cr & CR_DMA)) {
            u32 n = ((cr >> 4) & 3) + 1;
            if (rw == RW_WRITE) {
                u32 d = g_ch[ch].data;
                for (u32 i = 0; i < n; i++, d <<= 8) {
                    u8 b = (u8)(d >> 24);
                    card_transfer_byte(&b);
                }
            } else if (rw == RW_READ) {
                u32 d = 0;
                for (u32 i = 0; i < n; i++) {
                    u8 b = 0;
                    card_transfer_byte(&b);
                    d |= (u32)b << (24 - 8 * i);
                }
                g_ch[ch].data = d;
            }
        } else {
            // Card DMA moves whole runs at the address set up by the preceding immediate command.
            u32 mar = g_ch[ch].mar & 0x01FFFFE0u, len = g_ch[ch].len;
            if (mar + len <= g_ram_size) {
                u32 a = g_card.address & (CARD_SIZE - 1);
                if (a + len > CARD_SIZE) len = CARD_SIZE - a;
                if (rw == RW_READ) { hw_dma_trap("EXI card", mar, len); memcpy(g_ram + mar, g_card.data + a, len); }
                else if (rw == RW_WRITE) { memcpy(g_card.data + a, g_ram + mar, len); g_card.dirty = 1; }
            }
        }
    }
    g_ch[ch].cr &= ~CR_TSTART;
    g_ch[ch].csr |= CSR_TCINT;
}

u32 exi_read(u32 off) {
    u32 ch = (off - 0x6800) / EXI_STRIDE, reg = (off - 0x6800) % EXI_STRIDE;
    if (ch > 2) return 0;
    switch (reg) {
    case EXI_CSR: {
        u32 v = g_ch[ch].csr & ~CSR_EXT;
        if (ch == 0 && g_card.present) v |= CSR_EXT;   // slot A: card inserted
        if (ch == 0 && card_interrupt()) v |= CSR_EXIINT;
        return v;
    }
    case EXI_MAR: return g_ch[ch].mar;
    case EXI_LEN: return g_ch[ch].len;
    case EXI_CR: return g_ch[ch].cr;
    default: return g_ch[ch].data;
    }
}

void exi_write(u32 off, u32 v) {
    u32 ch = (off - 0x6800) / EXI_STRIDE, reg = (off - 0x6800) % EXI_STRIDE;
    if (ch > 2) return;
    switch (reg) {
    case EXI_CSR: {
        u32 old = g_ch[ch].csr, keep = old & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT | CSR_ROMDIS);
        keep &= ~(v & (CSR_EXIINT | CSR_TCINT | (ch < 2 ? CSR_EXTINT : 0)));   // write 1 to clear
        if (ch == 0 && (v & CSR_EXIINT)) g_card.interrupt_set = 0;
        u32 mask = CSR_EXIINTMASK | CSR_TCINTMASK | CSR_CLK | CSR_CS | (ch < 2 ? CSR_EXTINTMASK : 0);
        if (ch == 0) { mask |= CSR_ROMDIS; keep &= ~CSR_ROMDIS; }
        g_ch[ch].csr = keep | (v & mask);
        u32 old_cs = (old & CSR_CS) >> 7, new_cs = (g_ch[ch].csr & CSR_CS) >> 7;
        if (ch == 0 && g_card.present && ((old_cs ^ new_cs) & 1)) card_set_cs(new_cs & 1);
        if (ch == 0 && ((old_cs ^ new_cs) & 2) && (new_cs & 2)) g_ipl.command = g_ipl.received = g_ipl.cursor = 0;
        break;
    }
    case EXI_MAR: g_ch[ch].mar = v; break;
    case EXI_LEN: g_ch[ch].len = v; break;
    case EXI_CR:
        g_ch[ch].cr = v;
        if (v & CR_TSTART) transfer(ch);
        break;
    default: g_ch[ch].data = v; break;
    }
}

void exi_tick(u64 timebase) {
    g_now = timebase;
    if (g_card.done_at && timebase >= g_card.done_at) {
        g_card.done_at = 0;
        g_card.status = (g_card.status | MC_STATUS_READY) & ~MC_STATUS_BUSY;
        g_card.interrupt_set = 1;
        exi_flush();
    }
}

u32 exi_pending(void) {
    for (u32 ch = 0; ch < 3; ch++) {
        u32 csr = exi_read(0x6800 + ch * EXI_STRIDE);
        if (((csr & CSR_EXIINT) && (csr & CSR_EXIINTMASK)) || ((csr & CSR_TCINT) && (csr & CSR_TCINTMASK)) ||
            ((csr & CSR_EXTINT) && (csr & CSR_EXTINTMASK)))
            return PI_INT_EXI;
    }
    return 0;
}

// The card image itself stays out of checkpoints, as in Dolphin: loading one never undoes a save.
void exi_state(StateIO* io) {
    STATE(io, g_ch);
    STATE(io, g_card.command);
    STATE(io, g_card.status);
    STATE(io, g_card.interrupt_switch);
    STATE(io, g_card.interrupt_set);
    STATE(io, g_card.position);
    STATE(io, g_card.address);
    STATE(io, g_card.program_buffer);
    STATE(io, g_card.done_at);
    STATE(io, g_now);
    STATE(io, g_sram);
    STATE(io, g_ipl);
}
