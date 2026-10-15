// GameCube hardware model for the recompiled game: MMIO register file plus the
// devices boot needs (PI interrupts, VI retrace, DI disc reads, DSP/ARAM, EXI, SI).
#pragma once
#include "cpu/cpu.h"

// PI interrupt cause bits (INTSR at 0xCC003000).
enum {
    PI_INT_DI = 1u << 2,
    PI_INT_SI = 1u << 3,
    PI_INT_EXI = 1u << 4,
    PI_INT_AI = 1u << 5,
    PI_INT_DSP = 1u << 6,
    PI_INT_VI = 1u << 8,
    PI_RESET_SWITCH_RELEASED = 1u << 16,
};

void hw_init(const char* iso_path);
void hw_attach(CPUState* cpu);
// Advance device time to the given timebase value (40.5 MHz ticks).
void hw_tick(u64 timebase);
// True when an enabled PI interrupt is pending (INTSR & INTMR).
int hw_external_pending(void);

// Decrementer passed below zero since it was last written; true once per expiry.
int hw_decrementer_expired(CPUState* cpu);

// Receives command-aligned GX FIFO bytes (big-endian, hardware format) as the GPU drains them.
typedef void (*hw_gpu_sink)(const u8* data, u32 len);
void hw_set_gpu_sink(hw_gpu_sink sink);

// Called after the GPU drains a frame's commands (draw-done sync point). Returns 0 if the frame
// is already drawn, else a ticket; draw-done is signalled once progress() reaches it.
typedef u64 (*hw_frame_hook)(void);
typedef u64 (*hw_gpu_progress)(void);
void hw_set_frame_hook(hw_frame_hook hook, hw_gpu_progress progress);

// Debug: report device DMA over the DMA_TRAP address.
void hw_dma_trap(const char* who, u32 phys, u32 len);

// MMIO access log for the end-of-run report.
void hw_report(const char* (*sym_for)(u32 pc, u32* off));
