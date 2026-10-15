// Aurora-backed presentation: owns the window and the per-frame begin/end cycle.
#pragma once
#include "cpu/cpu.h"

// Opens the window and points Aurora's address translation at guest RAM.
void gpu_init(int argc, char** argv, CPUState* cpu);
// Feeds command-aligned GX FIFO bytes to Aurora (hw_gpu_sink).
void gpu_consume(const u8* data, u32 len);
// Ends the current frame, pumps window events, starts the next (hw_frame_hook). Returns 0 when
// done synchronously, else a ticket that gpu_frames_completed() reaches once the frame is drawn.
u64 gpu_frame_end(void);
u64 gpu_frames_completed(void);
// Runs emu (the CPU loop) on a worker thread while this (main) thread owns the window and Aurora;
// NO_GPU or SYNC_GPU=1 runs everything on the calling thread instead.
int gpu_run(int (*emu)(int, char**), int argc, char** argv);
// True once the user closed the window.
int gpu_should_exit(void);
u64 gpu_frames_presented(void);
void gpu_shutdown(void);
