// Host audio output: AI DMA blocks (32 kHz, big-endian s16, right channel first) to the default device.
#pragma once
#include <stdint.h>

void audio_init(void);
void audio_push_ai_block(const uint8_t* be_rl_samples, uint32_t bytes);
// Drops queued output (after loading a checkpoint).
void audio_flush(void);
