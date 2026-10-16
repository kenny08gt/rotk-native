// AX audio microcode HLE (GameCube variant): command lists, voice decoding and mixing.
#pragma once
#include <stdint.h>
#include "savestate.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ax_mail_fn)(u32 mail, int irq);

void ax_init(u8* ram, u32 ram_size, u8* aram, u32 aram_size, ax_mail_fn post_mail);
// The AX task was booted: announces DSP_INIT.
void ax_start(void);
// The AX task is booted again after another task ran (e.g. the CARD unlock): announces DSP_RESUME.
void ax_resume(void);
enum { AX_RUNNING = 1, AX_NEW_UCODE = 0, AX_RESET = -1 };
// CPU->DSP mail while the AX task runs.
int ax_mail(u32 mail);
// Checkpoint: the mail protocol state between command lists.
void ax_state(StateIO* io);
void ax_stats(unsigned long long* cmdlists, unsigned long long* voice_ms);

#ifdef __cplusplus
}
#endif
