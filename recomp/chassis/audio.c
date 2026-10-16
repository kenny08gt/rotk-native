#include "audio.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_AudioStream* g_stream;

// AUDIO_DUMP=path.wav: also write everything the game outputs to a WAV file (works with NO_AUDIO).
static FILE* g_dump;
static uint32_t g_dump_bytes;

static void put_le32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void dump_header(void) {
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0........\4\0\x10\0data....";
    put_le32(h + 4, 36 + g_dump_bytes);
    put_le32(h + 24, 32000);
    put_le32(h + 28, 32000 * 4);
    put_le32(h + 40, g_dump_bytes);
    fseek(g_dump, 0, SEEK_SET);
    fwrite(h, 1, 44, g_dump);
    fflush(g_dump);
}

static void dump_close(void) {
    if (!g_dump) return;
    dump_header();
    fclose(g_dump);
    g_dump = NULL;
}

void audio_init(void) {
    const char* dump = getenv("AUDIO_DUMP");
    if (dump && (g_dump = fopen(dump, "wb"))) {
        dump_header();
        atexit(dump_close);
    }
    if (getenv("NO_AUDIO")) return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (g_stream) SDL_ResumeAudioStreamDevice(g_stream);
}

void audio_push_ai_block(const uint8_t* src, uint32_t bytes) {
    if ((!g_stream && !g_dump) || bytes > 64 * 1024) return;
    int16_t out[16 * 1024];
    uint32_t frames = bytes / 4;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t r = (int16_t)(src[4 * i] << 8 | src[4 * i + 1]);
        int16_t l = (int16_t)(src[4 * i + 2] << 8 | src[4 * i + 3]);
        out[2 * i] = l;
        out[2 * i + 1] = r;
    }
    if (g_dump) {
        fseek(g_dump, 0, SEEK_END);
        fwrite(out, 4, frames, g_dump);
        g_dump_bytes += frames * 4;
    }
    // Keep latency bounded if the guest runs faster than real time.
    if (g_stream && SDL_GetAudioStreamQueued(g_stream) <= 32000 * 4 / 5)
        SDL_PutAudioStreamData(g_stream, out, (int)(frames * 4));
}

void audio_flush(void) {
    if (g_stream) SDL_ClearAudioStream(g_stream);
}
