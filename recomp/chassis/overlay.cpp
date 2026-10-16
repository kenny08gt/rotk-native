#include "overlay.h"

#include <SDL3/SDL.h>
#include <aurora/imgui.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>

static bool g_show = true;
static std::atomic<bool> g_legend{getenv("ROTK_LEGEND") != nullptr};
static char g_text[48] = "-- fps";

extern "C" int input_pad_type(void);

static ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

extern "C" void overlay_toggle(void) { g_show = !g_show; }
extern "C" void overlay_toggle_legend(void) { g_legend = !g_legend; }

static std::atomic<const char*> g_note{nullptr};
static std::atomic<Uint64> g_note_at{0};

extern "C" void overlay_notify(const char* message) {
    g_note_at = SDL_GetTicksNS();
    g_note = message;
}

static void draw_note(void) {
    const char* note = g_note;
    double age = (SDL_GetTicksNS() - g_note_at) / 1e9;
    if (!note || age > 2.0) return;
    float alpha = age < 1.5 ? 1.0f : (float)(2.0 - age) * 2.0f;
    float size = ImGui::GetFontSize() * 2.0f;
    ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0, note);
    ImVec2 pos(ImGui::GetIO().DisplaySize.x / 2 - ext.x / 2, 60);
    ImDrawList* d = ImGui::GetForegroundDrawList();
    d->AddRectFilled(ImVec2(pos.x - 18, pos.y - 10), ImVec2(pos.x + ext.x + 18, pos.y + ext.y + 10),
                     rgb(0, 0, 0, (int)(190 * alpha)), 10);
    d->AddText(ImGui::GetFont(), size, pos, rgb(255, 255, 255, (int)(255 * alpha)), note);
}

static void text_centered(ImDrawList* d, float size, ImVec2 c, ImU32 col, const char* t) {
    ImVec2 e = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0, t);
    d->AddText(ImGui::GetFont(), size, ImVec2(c.x - e.x / 2, c.y - e.y / 2), col, t);
}

// A face button as the connected controller labels it: PlayStation shapes in their colors, else the letter.
static void face_button(ImDrawList* d, float size, ImVec2 c, float r, SDL_GamepadButtonLabel label) {
    d->AddCircleFilled(c, r, rgb(40, 40, 46));
    float k = r * 0.5f, w = r * 0.16f;
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        d->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), rgb(124, 178, 232), w);
        d->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), rgb(124, 178, 232), w);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: d->AddCircle(c, k * 1.05f, rgb(255, 102, 102), 0, w); break;
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        d->AddRect(ImVec2(c.x - k * 0.9f, c.y - k * 0.9f), ImVec2(c.x + k * 0.9f, c.y + k * 0.9f), rgb(255, 105, 248), 0, 0, w);
        break;
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        d->AddTriangle(ImVec2(c.x, c.y - k * 1.05f), ImVec2(c.x - k * 1.05f, c.y + k * 0.75f), ImVec2(c.x + k * 1.05f, c.y + k * 0.75f),
                       rgb(64, 226, 160), w);
        break;
    default: {
        static const char* letters[] = {"?", "A", "B", "X", "Y"};
        int i = label >= SDL_GAMEPAD_BUTTON_LABEL_A && label <= SDL_GAMEPAD_BUTTON_LABEL_Y ? label : 0;
        text_centered(d, size, c, rgb(235, 235, 235), letters[i]);
    }
    }
}

// GameCube button -> this controller's button -> keyboard key, with the GameCube buttons in their own colors.
static void draw_legend(void) {
    int type = input_pad_type();
    bool pad = type != SDL_GAMEPAD_TYPE_UNKNOWN;
    bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    bool nin = type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    bool xbox = type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE;
    const char* z = ps ? "R1" : nin ? "R" : "RB";
    const char* lr = ps ? "L2 / R2" : nin ? "ZL / ZR" : "LT / RT";
    const char* start = type == SDL_GAMEPAD_TYPE_PS5 || type == SDL_GAMEPAD_TYPE_PS4 ? "Options" : nin ? "+" : xbox ? "Menu" : "Start";
    struct Row { const char* gc; ImU32 col; bool round; int face; const char* padtext; const char* key; };
    const Row rows[] = {
        {"A", rgb(45, 184, 76), true, SDL_GAMEPAD_BUTTON_SOUTH, nullptr, "X"},
        {"B", rgb(226, 52, 45), true, SDL_GAMEPAD_BUTTON_EAST, nullptr, "Z"},
        {"X", rgb(190, 190, 196), true, SDL_GAMEPAD_BUTTON_WEST, nullptr, "S"},
        {"Y", rgb(190, 190, 196), true, SDL_GAMEPAD_BUTTON_NORTH, nullptr, "A"},
        {"Z", rgb(106, 79, 192), true, -1, z, "E"},
        {"L / R", rgb(150, 150, 156), false, -1, lr, "Q / W"},
        {"START", rgb(150, 150, 156), false, -1, start, "Return"},
        {"Stick", rgb(150, 150, 156), false, -1, "Left stick, D-pad", "Arrows"},
        {"C-stick", rgb(222, 190, 40), false, -1, "Right stick", "I J K L"},
    };
    const int n = sizeof rows / sizeof rows[0];
    float fs = ImGui::GetFontSize() * 1.4f, row_h = fs * 1.9f, r = fs * 0.75f;
    float col_pad = fs * 6.5f, col_key = fs * 18.5f, width = fs * 24.5f;
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImVec2 o(disp.x - width - 16, 50);
    ImDrawList* d = ImGui::GetForegroundDrawList();
    float height = row_h * (n + 3.75f) + fs * 0.5f;
    d->AddRectFilled(o, ImVec2(o.x + width, o.y + height), rgb(0, 0, 0, 185), 10);
    ImU32 dim = rgb(170, 170, 175), fg = rgb(240, 240, 240);
    float y = o.y + row_h * 0.6f;
    d->AddText(ImGui::GetFont(), fs, ImVec2(o.x + fs, y - fs / 2), dim, "GameCube");
    d->AddText(ImGui::GetFont(), fs, ImVec2(o.x + col_pad, y - fs / 2), dim, pad ? "Controller" : "No controller");
    d->AddText(ImGui::GetFont(), fs, ImVec2(o.x + col_key, y - fs / 2), dim, "Keyboard");
    for (int i = 0; i < n; i++) {
        const Row& w = rows[i];
        y += row_h;
        ImVec2 c(o.x + fs + r, y);
        if (w.round) {
            d->AddCircleFilled(c, r, w.col);
            bool light = w.col == rgb(190, 190, 196);
            text_centered(d, fs, c, light ? rgb(30, 30, 30) : rgb(255, 255, 255), w.gc);
        } else {
            ImVec2 e = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, w.gc);
            d->AddRectFilled(ImVec2(c.x - r, y - r * 0.8f), ImVec2(c.x - r + e.x + fs, y + r * 0.8f), w.col, r * 0.8f);
            d->AddText(ImGui::GetFont(), fs, ImVec2(c.x - r + fs / 2, y - e.y / 2), rgb(20, 20, 20), w.gc);
        }
        if (pad && w.face >= 0)
            face_button(d, fs, ImVec2(o.x + col_pad + r, y), r,
                        SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, (SDL_GamepadButton)w.face));
        else if (pad)
            d->AddText(ImGui::GetFont(), fs, ImVec2(o.x + col_pad, y - fs / 2), fg, w.padtext);
        d->AddText(ImGui::GetFont(), fs, ImVec2(o.x + col_key, y - fs / 2), fg, w.key);
    }
    y += row_h;
    const char* hold = ps ? "Create" : nin ? "-" : "Back";
    char line[64];
    snprintf(line, sizeof line, pad ? "Save checkpoint: F5 or %s+%s" : "Save checkpoint: F5", hold, ps ? "L1" : nin ? "L" : "LB");
    d->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(o.x + fs, y - fs / 2), fg, line);
    y += row_h * 0.75f;
    snprintf(line, sizeof line, pad ? "Load checkpoint: F9 or %s+%s" : "Load checkpoint: F9", hold, ps ? "R1" : nin ? "R" : "RB");
    d->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(o.x + fs, y - fs / 2), fg, line);
    y += row_h;
    d->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(o.x + fs, y - fs / 2), dim, pad ? "F2 or tap Create/Back: hide legend" : "F2: hide legend");
}

// Frames per second and the slowest frame, both over the last half second, so a stutter that the
// average hides still shows up.
extern "C" void overlay_draw(void) {
    static Uint64 window_start, last;
    static unsigned frames;
    static double worst_ms;
    Uint64 now = SDL_GetTicksNS();
    if (last) {
        double ms = (now - last) / 1e6;
        if (ms > worst_ms) worst_ms = ms;
    } else {
        window_start = now;
    }
    last = now;
    frames++;
    if (now - window_start >= 500000000) {
        snprintf(g_text, sizeof g_text, "%.0f fps  worst %.0f ms", frames * 1e9 / (now - window_start), worst_ms);
        window_start = now;
        frames = 0;
        worst_ms = 0;
    }
    draw_note();
    if (g_legend) draw_legend();
    if (!g_show) return;
    ImDrawList* d = ImGui::GetForegroundDrawList();
    float size = ImGui::GetFontSize() * 1.5f;
    ImVec2 pos(12, 10);
    ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0, g_text);
    d->AddRectFilled(ImVec2(pos.x - 6, pos.y - 3), ImVec2(pos.x + ext.x + 6, pos.y + ext.y + 3), IM_COL32(0, 0, 0, 150), 4);
    d->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 255, 255, 230), g_text);
}
