// EXI bus (0xCC006800): three channels with a memory card in slot A (channel 0, device 0).
// Other devices (IPL/RTC/SRAM, slot B, serial ports) answer nothing, as before.
#pragma once
#include "cpu/cpu.h"

// Opens (or creates on first save) the slot A card image; NULL leaves the slot empty.
void exi_init(u8* ram, u32 ram_size, const char* card_path);
// Register access for offsets 0x6800-0x683B within the 0xCC00xxxx block.
u32 exi_read(u32 off);
void exi_write(u32 off, u32 value);
// Delayed card command completion (erase/program take a moment, then raise EXIINT).
void exi_tick(u64 timebase);
// PI_INT_EXI when any channel has an enabled interrupt pending.
u32 exi_pending(void);
// Writes the card image back to disk (also done after each program/erase burst settles).
void exi_flush(void);
