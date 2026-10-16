#include "overlay.h"

#include <SDL3/SDL.h>
#include <aurora/imgui.h>
#include <stdio.h>

static bool g_show = true;
static char g_text[48] = "-- fps";

extern "C" void overlay_toggle(void) { g_show = !g_show; }

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
    if (!g_show) return;
    ImDrawList* d = ImGui::GetForegroundDrawList();
    float size = ImGui::GetFontSize() * 1.5f;
    ImVec2 pos(12, 10);
    ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0, g_text);
    d->AddRectFilled(ImVec2(pos.x - 6, pos.y - 3), ImVec2(pos.x + ext.x + 6, pos.y + ext.y + 3), IM_COL32(0, 0, 0, 150), 4);
    d->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 255, 255, 230), g_text);
}
