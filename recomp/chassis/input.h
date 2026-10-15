// Host input -> GameCube controller state (port 0), from the keyboard or the first SDL gamepad.
#pragma once
#include "cpu/cpu.h"

// Fills the 8-byte SI poll response (analog mode 3) for the given port; returns 0 if unplugged.
int input_poll(u32 port, u8 out[8]);

// Time base for scripted presses (AUTOPRESS) and input sampling: guest seconds when set, so a
// scripted run does the same thing at any host speed. Without it, host time.
void input_set_clock(double (*now)(void));
