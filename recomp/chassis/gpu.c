#include "gpu.h"
#include "overlay.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <SDL3/SDL_hints.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>

// Added to Aurora for retail FIFO streams (see recomp/patches/aurora-retail-fifo.patch).
void aurora_gx_set_guest_ram(const void* base, uint32_t size);
void aurora_gx_feed_fifo(const void* data, uint32_t size);
void aurora_gx_texture_debug(unsigned long long out[4]);
// Aurora's VI window extensions (lib/dolphin/vi/vi.cpp).
void VISetWindowTitle(const char* title);
void VISetWindowFullscreen(bool fullscreen);
bool VIGetWindowFullscreen(void);
void AuroraSetViewportPolicy(int policy);   // 0 = fit (keep aspect), 1 = stretch
void AuroraGetRenderSize(uint32_t* width, uint32_t* height);

static int g_frame_active;
static atomic_int g_exit;
static u64 g_frames;

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    (void)len;
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    if (level < LOG_WARNING && !getenv("AURORA_VERBOSE")) return;
    fprintf(stderr, "[aurora %s %s] %s\n", names[level], module, message);
    if (level == LOG_FATAL) abort();
}

// Aurora persists compiled pipelines keyed by GX state, not by shader source, so pipelines
// from an older build (with different shader code) get reused. Drop the caches whenever the
// executable changes.
static void invalidate_stale_caches(void) {
    char exe[4096];
    uint32_t len = sizeof exe;
    struct stat st;
    const char* home = getenv("HOME");
    if (!home || _NSGetExecutablePath(exe, &len) != 0 || stat(exe, &st) != 0) return;
    char dir[4096], path[4200], stamp[64];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/rotk", home);
    snprintf(stamp, sizeof stamp, "%lld.%lld", (long long)st.st_mtimespec.tv_sec, (long long)st.st_size);
    snprintf(path, sizeof path, "%s/build_stamp", dir);
    char old[64] = {0};
    FILE* f = fopen(path, "r");
    if (f) {
        if (!fgets(old, sizeof old, f)) old[0] = 0;
        fclose(f);
    }
    if (!strcmp(old, stamp)) return;
    static const char* caches[] = {"pipeline_cache.db", "pipeline_cache.db-shm", "pipeline_cache.db-wal",
                                   "dawn_cache.db", "dawn_cache.db-shm", "dawn_cache.db-wal"};
    for (size_t i = 0; i < sizeof caches / sizeof caches[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, caches[i]);
        remove(path);
    }
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/build_stamp", dir);
    if ((f = fopen(path, "w"))) {
        fputs(stamp, f);
        fclose(f);
    }
}

// ---- Aurora calls (always on the main thread) ----

static void aurora_init(int argc, char** argv, CPUState* cpu) {
    invalidate_stale_caches();
    // ROTK_MSAA=<samples> and ROTK_ANISO=<level> tune image quality; FULLSCREEN=1 starts fullscreen.
    const AuroraConfig config = {
        .appName = "rotk",
        .logCallback = &log_callback,
        .windowWidth = 1280,
        .windowHeight = 960,
        .vsync = true,
        .msaa = getenv("ROTK_MSAA") ? (uint32_t)atoi(getenv("ROTK_MSAA")) : 1,
        .maxTextureAnisotropy = getenv("ROTK_ANISO") ? (uint16_t)atoi(getenv("ROTK_ANISO")) : 16,
    };
    // Fullscreen covers the display directly instead of animating into its own Space (that
    // transition never completes for this window, and an instant switch suits a game better).
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    // Before initialize: Aurora recompiles cached pipelines during startup, and shader generation
    // already has to see the retail-FIFO mode (texel-space texcoords) that this switches on.
    aurora_gx_set_guest_ram(cpu->ram, cpu->ram_size);
    aurora_initialize(argc, argv, &config);
    VISetWindowTitle("The Lord of the Rings: The Return of the King");
    // Keep the TV's 4:3 picture letterboxed in any window or display shape (ROTK_STRETCH=1 fills it).
    AuroraSetViewportPolicy(getenv("ROTK_STRETCH") ? 1 : 0);

    g_frame_active = aurora_begin_frame();
}

static void aurora_feed(const u8* data, u32 len) {
    if (g_frame_active) aurora_gx_feed_fifo(data, len);
}

static void aurora_frame(void) {
    if (g_frame_active) {
        overlay_draw();
        aurora_end_frame();
        g_frames++;
        if (getenv("GPU_STATS") && g_frames % 60 == 0) {
            const AuroraStats* st = aurora_get_stats();
            unsigned long long t[4];
            aurora_gx_texture_debug(t);
            uint32_t rw, rh;
            AuroraGetRenderSize(&rw, &rh);
            fprintf(stderr, "frame %llu: render %ux%u draws=%u merged=%u verts=%u textures_up=%u | tex resolved=%llu "
                    "no_data=%llu palette_no_tlut=%llu null=%llu\n", g_frames, rw, rh, st->drawCallCount,
                    st->mergedDrawCallCount, st->lastVertSize, st->lastTextureUploadSize, t[0], t[1], t[2], t[3]);
        }
    }
    // FULLSCREEN=1: switch once the window is on screen (macOS ignores it on a still-hidden window).
    static int fullscreen_pending = -1;
    if (fullscreen_pending < 0) fullscreen_pending = getenv("FULLSCREEN") != NULL;
    if (fullscreen_pending && g_frames > 2) {
        VISetWindowFullscreen(true);
        fullscreen_pending = 0;
    }
    for (const AuroraEvent* e = aurora_update(); e && e->type != AURORA_NONE; e++) {
        if (e->type == AURORA_EXIT) atomic_store(&g_exit, 1);
        // Fullscreen toggle: Control-Command-F (the macOS convention) or Option-Return.
        if (e->type == AURORA_SDL_EVENT && e->sdl.type == SDL_EVENT_KEY_DOWN && !e->sdl.key.repeat) {
            SDL_Keymod m = e->sdl.key.mod;
            if ((e->sdl.key.key == SDLK_F && (m & SDL_KMOD_GUI) && (m & SDL_KMOD_CTRL)) ||
                (e->sdl.key.key == SDLK_RETURN && (m & SDL_KMOD_ALT)))
                VISetWindowFullscreen(!VIGetWindowFullscreen());
            if (e->sdl.key.key == SDLK_F1) overlay_toggle();
            if (e->sdl.key.key == SDLK_F2) overlay_toggle_legend();
        }
    }
    g_frame_active = aurora_begin_frame();
}

static void aurora_close(void) {
    if (g_frame_active) aurora_end_frame();
    aurora_shutdown();
}

// ---- Threaded mode: the emulated CPU runs on a worker; this thread owns the window and Aurora ----
// The CPU thread appends records to `fill`; the main thread swaps it with `drain` and processes them
// outside the lock. Records: u32 kind, u32 len, then len bytes (padded to 4).

enum { REC_FIFO = 1, REC_FRAME = 2 };

typedef struct {
    u8* data;
    size_t len, cap;
} RecBuf;

static int g_async;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;   // main thread: new records / init request / CPU done
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;   // CPU thread: frame completed / init finished
static RecBuf g_fill;
static u64 g_frames_submitted;
static atomic_ullong g_frames_completed;
static int g_init_requested, g_init_done, g_cpu_finished;
static CPUState* g_init_cpu;
static u64 g_cpu_wait_ns, g_gpu_busy_ns;
static int g_argc;
static char** g_argv;

static void rec_append(RecBuf* b, u32 kind, const u8* data, u32 len) {
    size_t need = b->len + 8 + ((len + 3) & ~3u);
    if (need > b->cap) {
        b->cap = need * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, &kind, 4);
    memcpy(b->data + b->len + 4, &len, 4);
    if (len) memcpy(b->data + b->len + 8, data, len);
    b->len = need;
}

void gpu_init(int argc, char** argv, CPUState* cpu) {
    if (!g_async) {
        aurora_init(argc, argv, cpu);
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_init_cpu = cpu;
    g_init_requested = 1;
    pthread_cond_signal(&g_wake);
    while (!g_init_done) pthread_cond_wait(&g_done, &g_lock);
    pthread_mutex_unlock(&g_lock);
}

void gpu_consume(const u8* data, u32 len) {
    if (!g_async) {
        aurora_feed(data, len);
        return;
    }
    pthread_mutex_lock(&g_lock);
    rec_append(&g_fill, REC_FIFO, data, len);
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

u64 gpu_frame_end(void) {
    if (!g_async) {
        aurora_frame();
        return 0;
    }
    u64 w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_lock);
    // At most one frame in flight, like a double-buffered GPU: wait for the previous one first.
    while (atomic_load(&g_frames_completed) < g_frames_submitted && !atomic_load(&g_exit))
        pthread_cond_wait(&g_done, &g_lock);
    rec_append(&g_fill, REC_FRAME, NULL, 0);
    u64 ticket = ++g_frames_submitted;
    g_cpu_wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return ticket;
}

u64 gpu_frames_completed(void) { return atomic_load(&g_frames_completed); }

int gpu_should_exit(void) { return atomic_load(&g_exit); }
u64 gpu_frames_presented(void) { return g_frames; }

void gpu_shutdown(void) {
    if (!g_async) aurora_close();
    // Threaded mode: the main thread closes Aurora once the CPU thread returns.
}

typedef struct {
    int (*emu)(int, char**);
    int argc;
    char** argv;
    int result;
} EmuThread;

static void* emu_thread(void* arg) {
    EmuThread* t = arg;
    t->result = t->emu(t->argc, t->argv);
    pthread_mutex_lock(&g_lock);
    g_cpu_finished = 1;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int gpu_run(int (*emu)(int, char**), int argc, char** argv) {
    if (getenv("NO_GPU") || getenv("SYNC_GPU")) return emu(argc, argv);
    g_async = 1;
    g_argc = argc;
    g_argv = argv;
    EmuThread t = {emu, argc, argv, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);   // recompiled guest call chains recurse on the host stack
    // The game thread is the bottleneck: ask for performance cores even when the Mac is busy.
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t th;
    pthread_create(&th, &attr, emu_thread, &t);

    RecBuf drain = {0};
    int initialized = 0;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_fill.len && !g_cpu_finished && !(g_init_requested && !initialized))
            pthread_cond_wait(&g_wake, &g_lock);
        if (g_init_requested && !initialized) {
            pthread_mutex_unlock(&g_lock);
            aurora_init(g_argc, g_argv, g_init_cpu);
            pthread_mutex_lock(&g_lock);
            initialized = g_init_done = 1;
            pthread_cond_broadcast(&g_done);
            continue;
        }
        if (!g_fill.len && g_cpu_finished) break;
        RecBuf tmp = g_fill;
        g_fill = drain;
        g_fill.len = 0;
        drain = tmp;
        pthread_mutex_unlock(&g_lock);

        u64 b0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (size_t off = 0; off < drain.len;) {
            u32 kind, len;
            memcpy(&kind, drain.data + off, 4);
            memcpy(&len, drain.data + off + 4, 4);
            if (kind == REC_FIFO) {
                aurora_feed(drain.data + off + 8, len);
            } else {
                aurora_frame();
                pthread_mutex_lock(&g_lock);
                atomic_fetch_add(&g_frames_completed, 1);
                pthread_cond_broadcast(&g_done);
                pthread_mutex_unlock(&g_lock);
            }
            off += 8 + ((len + 3) & ~3u);
        }
        g_gpu_busy_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - b0;
        pthread_mutex_lock(&g_lock);
        if (atomic_load(&g_exit)) pthread_cond_broadcast(&g_done);
    }
    pthread_mutex_unlock(&g_lock);
    pthread_join(th, NULL);
    printf("gpu thread: busy %.1fs; cpu thread waited %.1fs for frames\n", g_gpu_busy_ns / 1e9, g_cpu_wait_ns / 1e9);
    if (initialized) aurora_close();
    return t.result;
}
