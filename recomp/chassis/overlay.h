#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-screen frame-rate counter and controls legend, drawn into Aurora's ImGui frame just before it
// ends. F1 toggles the counter; F2 or the controller's Back/Create button toggles the legend (safe
// to call from the game thread).
void overlay_draw(void);
void overlay_toggle(void);
void overlay_toggle_legend(void);
// Cheat menu: F3 or Create + Triangle opens it; the game gets no input while it is open.
int overlay_cheat_menu_open(void);
void overlay_toggle_cheat_menu(void);
void overlay_cheat_menu_move(int delta);
void overlay_cheat_menu_toggle_selected(void);
// Shows a short message (a string literal) at the top of the screen for two seconds.
void overlay_notify(const char* message);

#ifdef __cplusplus
}
#endif
