// Action Replay code decryption (Dolphin's ARDecrypt, GPL-2.0-or-later).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decrypts one cheat's encrypted lines ("XXXX-XXXX-XXXXX" without the dashes, 13 characters each).
// Writes address/value pairs to `out` (2 * count words at most) and returns how many pairs, or -1
// when a line is malformed or the block fails its checksum.
int ar_decrypt(const char (*lines)[13], int count, uint32_t* out);

#ifdef __cplusplus
}
#endif
