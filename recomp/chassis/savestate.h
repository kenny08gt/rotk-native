// Checkpoints (save states): the whole emulated machine written to one file and read back.
// Each module serializes its own state with state_io, which copies one way when saving and the
// other way when loading, so a field cannot be saved without also being restored.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t* buf;
    size_t pos, cap;
    int loading;
    int short_read;   // loading ran past the end of the file
} StateIO;

void state_io(StateIO* io, void* data, size_t n);
#define STATE(io, v) state_io((io), &(v), sizeof(v))

enum { CHECKPOINT_SAVE = 1, CHECKPOINT_LOAD = 2 };
// Asks the game thread to save or load at its next safe point (callable from any thread).
void checkpoint_request(int op);
// The pending request, or 0; clears it.
int checkpoint_take(void);

// Writes the serialized machine (io->buf, io->pos bytes) to the checkpoint file in the background,
// taking ownership of the buffer. `game` is the disc's 6-character ID.
void checkpoint_write(StateIO* io, const char game[6]);
// Reads the checkpoint file into io for loading. Returns 0 when there is none, or when it was made
// by another game or holds a different amount of state than `expected` bytes (another build).
int checkpoint_read(StateIO* io, const char game[6], size_t expected);

#ifdef __cplusplus
}
#endif
