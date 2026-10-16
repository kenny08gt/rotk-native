#include "input.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

// Keyboard layout (port 0):
//   arrows      main stick + D-pad      Enter  START
//   X           A                        Z      B
//   S           X                        A      Y
//   Q / W       L / R triggers           E      Z
//   I J K L     C-stick
enum {
    BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008,
    BTN_Z = 0x0010, BTN_R = 0x0020, BTN_L = 0x0040,
    BTN_A = 0x0100, BTN_B = 0x0200, BTN_X = 0x0400, BTN_Y = 0x0800, BTN_START = 0x1000,
};

static SDL_Gamepad* g_pad;
static double (*g_clock)(void);

void input_set_clock(double (*now)(void)) { g_clock = now; }

// Scripted presses for unattended testing: AUTOPRESS="A@6,START@9.5" holds a button for 0.25 s
// at the given second (guest time with input_set_clock, else since the first poll).
static u16 autopress(void) {
    static const char* spec;
    static Uint64 t0;
    if (!spec && !(spec = SDL_getenv("AUTOPRESS"))) spec = "";
    if (!*spec) return 0;
    if (!g_clock && !t0) t0 = SDL_GetTicks();
    double now = g_clock ? g_clock() : (SDL_GetTicks() - t0) / 1000.0;
    static const struct { const char* name; u16 bit; } names[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"START", BTN_START}, {"Z", BTN_Z},
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT}, {"L", BTN_L}, {"R", BTN_R},
    };
    u16 held = 0;
    const char* p = spec;
    while (*p) {
        char name[16] = {0};
        double at = 0;
        int n = 0;
        if (SDL_sscanf(p, "%15[A-Z]@%lf%n", name, &at, &n) != 2) break;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!SDL_strcmp(name, names[i].name) && now >= at && now < at + 0.25) held |= names[i].bit;
        p += n;
        if (*p == ',') p++;
    }
    return held;
}

static SDL_Gamepad* gamepad(void) {
    if (g_pad && SDL_GamepadConnected(g_pad)) return g_pad;
    g_pad = NULL;
    // Enumerating devices is slow: look for a newly connected controller at most once a second.
    static Uint64 last_scan;
    Uint64 now = SDL_GetTicks();
    if (last_scan && now - last_scan < 1000) return NULL;
    last_scan = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0 && (g_pad = SDL_OpenGamepad(ids[0]))) printf("controller: %s\n", SDL_GetGamepadName(g_pad));
    SDL_free(ids);
    return g_pad;
}

static u8 axis8(Sint16 v) { return (u8)(128 + v / 256); }

static u8 dir8(int neg, int pos) { return (u8)(128 + (pos ? 100 : 0) - (neg ? 100 : 0)); }

static void read_pad(u8 out[8]);

// The game reads the SI status and poll registers thousands of times a frame; sample the host
// devices at most once a millisecond.
int input_poll(u32 port, u8 out[8]) {
    if (port != 0) return 0;
    static u8 cached[8];
    static Uint64 cached_at;
    Uint64 now = g_clock ? (Uint64)(g_clock() * 1e9) : SDL_GetTicksNS();
    if (!cached_at || now - cached_at >= 1000000) {
        read_pad(cached);
        cached_at = now;
    }
    memcpy(out, cached, 8);
    return 1;
}

static void read_pad(u8 out[8]) {
    const bool* k = SDL_GetKeyboardState(NULL);
    u16 b = 0;
    u8 sx = dir8(k[SDL_SCANCODE_LEFT], k[SDL_SCANCODE_RIGHT]);
    u8 sy = dir8(k[SDL_SCANCODE_DOWN], k[SDL_SCANCODE_UP]);
    u8 cx = dir8(k[SDL_SCANCODE_J], k[SDL_SCANCODE_L]);
    u8 cy = dir8(k[SDL_SCANCODE_K], k[SDL_SCANCODE_I]);
    u8 lt = k[SDL_SCANCODE_Q] ? 0xFF : 0, rt = k[SDL_SCANCODE_W] ? 0xFF : 0;
    if (k[SDL_SCANCODE_LEFT]) b |= BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= BTN_RIGHT;
    if (k[SDL_SCANCODE_UP]) b |= BTN_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= BTN_DOWN;
    if (k[SDL_SCANCODE_RETURN]) b |= BTN_START;
    if (k[SDL_SCANCODE_X]) b |= BTN_A;
    if (k[SDL_SCANCODE_Z]) b |= BTN_B;
    if (k[SDL_SCANCODE_S]) b |= BTN_X;
    if (k[SDL_SCANCODE_A]) b |= BTN_Y;
    if (k[SDL_SCANCODE_E]) b |= BTN_Z;

    SDL_Gamepad* p = gamepad();
    if (p) {
        static const struct { SDL_GamepadButton sdl; u16 gc; } map[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, BTN_A}, {SDL_GAMEPAD_BUTTON_EAST, BTN_B},
            {SDL_GAMEPAD_BUTTON_WEST, BTN_X}, {SDL_GAMEPAD_BUTTON_NORTH, BTN_Y},
            {SDL_GAMEPAD_BUTTON_START, BTN_START}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_Z},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GetGamepadButton(p, map[i].sdl)) b |= map[i].gc;
        Sint16 ax = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -8000 || ax > 8000) sx = axis8(ax);
        if (ay < -8000 || ay > 8000) sy = (u8)(255 - axis8(ay));
        Sint16 rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
        if (rx < -8000 || rx > 8000) cx = axis8(rx);
        if (ry < -8000 || ry > 8000) cy = (u8)(255 - axis8(ry));
        Sint16 l = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), r = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        if (l > 2000) { lt = (u8)(l / 128); if (l > 30000) b |= BTN_L; }
        if (r > 2000) { rt = (u8)(r / 128); if (r > 30000) b |= BTN_R; }
    }
    b |= autopress();
    if (lt == 0xFF) b |= BTN_L;
    if (rt == 0xFF) b |= BTN_R;

    // SI poll response, analog mode 3: status/buttons, 0x80 | buttons, stick, C-stick, triggers.
    out[0] = (u8)(b >> 8);
    out[1] = (u8)(0x80 | (b & 0x7F));
    out[2] = sx;
    out[3] = sy;
    out[4] = cx;
    out[5] = cy;
    out[6] = lt;
    out[7] = rt;
}
