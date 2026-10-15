// Host input -> GameCube controller state (port 0), from the keyboard or the first SDL gamepad.
#pragma once
#include "cpu/cpu.h"

// Fills the 8-byte SI poll response (analog mode 3) for the given port; returns 0 if unplugged.
int input_poll(u32 port, u8 out[8]);
