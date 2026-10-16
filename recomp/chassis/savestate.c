#include "savestate.h"

#include <compression.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void state_io(StateIO* io, void* data, size_t n) {
    if (io->loading) {
        if (io->pos + n > io->cap) { io->short_read = 1; return; }
        memcpy(data, io->buf + io->pos, n);
    } else if (io->buf) {
        if (io->pos + n > io->cap) {
            io->cap = (io->pos + n) * 2;
            io->buf = realloc(io->buf, io->cap);
        }
        memcpy(io->buf + io->pos, data, n);
    }
    io->pos += n;
}

static atomic_int g_request;

void checkpoint_request(int op) { atomic_store(&g_request, op); }
int checkpoint_take(void) { return atomic_exchange(&g_request, 0); }

// File: header, then the state LZ4-compressed (or stored as is when that does not shrink it).
typedef struct {
    char magic[8];
    char game[6];
    uint16_t compressed;
    uint64_t raw_size, file_size;
} Header;
static const char kMagic[8] = {'R', 'O', 'T', 'K', 'C', 'K', 'P', '1'};

// ROTK_CHECKPOINT=<path> uses another file (tests keep the player's checkpoint untouched).
static void checkpoint_path(char* out, size_t size, const char* suffix) {
    const char* home = getenv("HOME");
    const char* path = getenv("ROTK_CHECKPOINT");
    if (path) snprintf(out, size, "%s%s", path, suffix);
    else snprintf(out, size, "%s/Library/Application Support/rotk/checkpoint.state%s", home ? home : ".", suffix);
}

typedef struct {
    uint8_t* raw;
    size_t len;
    char game[6];
} WriteJob;

static pthread_t g_writer;
static int g_writer_running;

static void* write_job(void* arg) {
    WriteJob* job = arg;
    Header h = {.raw_size = job->len};
    memcpy(h.magic, kMagic, sizeof kMagic);
    memcpy(h.game, job->game, 6);
    uint8_t* packed = malloc(job->len);
    size_t n = packed ? compression_encode_buffer(packed, job->len, job->raw, job->len, NULL, COMPRESSION_LZ4) : 0;
    h.compressed = n != 0;
    h.file_size = n ? n : job->len;
    char path[1024], tmp[1040];
    checkpoint_path(path, sizeof path, "");
    checkpoint_path(tmp, sizeof tmp, ".tmp");
    FILE* f = fopen(tmp, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(n ? packed : job->raw, 1, h.file_size, f) == h.file_size;
    if (f && fclose(f) != 0) ok = 0;
    if (ok) rename(tmp, path);
    else fprintf(stderr, "checkpoint: could not write %s\n", tmp);
    free(packed);
    free(job->raw);
    free(job);
    return NULL;
}

static void wait_for_writer(void) {
    if (g_writer_running) pthread_join(g_writer, NULL);
    g_writer_running = 0;
}

void checkpoint_write(StateIO* io, const char game[6]) {
    wait_for_writer();
    WriteJob* job = malloc(sizeof *job);
    job->raw = io->buf;
    job->len = io->pos;
    memcpy(job->game, game, 6);
    io->buf = NULL;
    g_writer_running = pthread_create(&g_writer, NULL, write_job, job) == 0;
    if (!g_writer_running) write_job(job);
}

int checkpoint_read(StateIO* io, const char game[6], size_t expected) {
    wait_for_writer();   // a load right after a save reads the new file
    char path[1024];
    checkpoint_path(path, sizeof path, "");
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    Header h;
    uint8_t* packed = NULL;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, kMagic, sizeof kMagic) && !memcmp(h.game, game, 6) &&
             h.raw_size == expected && h.file_size <= expected;
    if (ok) {
        packed = malloc(h.file_size);
        io->buf = malloc(h.raw_size);
        ok = packed && io->buf && fread(packed, 1, h.file_size, f) == h.file_size;
    }
    fclose(f);
    if (ok && h.compressed)
        ok = compression_decode_buffer(io->buf, h.raw_size, packed, h.file_size, NULL, COMPRESSION_LZ4) == h.raw_size;
    else if (ok)
        memcpy(io->buf, packed, h.file_size);
    free(packed);
    if (!ok) {
        free(io->buf);
        io->buf = NULL;
        return 0;
    }
    io->cap = h.raw_size;
    io->pos = 0;
    io->loading = 1;
    return 1;
}
