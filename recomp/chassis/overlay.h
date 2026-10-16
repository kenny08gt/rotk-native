#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// On-screen frame-rate counter, drawn into Aurora's ImGui frame just before it ends. F1 toggles it.
void overlay_draw(void);
void overlay_toggle(void);

#ifdef __cplusplus
}
#endif
