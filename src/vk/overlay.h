// The in-game overlay (overlay.cpp, Dear ImGui) over the game's picture: a counter in a corner (F3: off / FPS /
// detailed) and a panel (F1) with frame times, CPU, GPU, video memory and RAM, the options and the system.
// window_sdl.c (main thread) hands it the figures and, while the panel is open, the mouse; it carries out what the
// panel asks for (ovl_take_actions). present.c (present thread) draws it.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "vk.h"

typedef struct {
    double fps, shown_fps, worst_ms;           // over the last half second
    double gpu_ms, gpu_load;                   // GPU time per game frame (ms), share of the time the GPU was busy (0..1)
    double cpu_proc, cpu_sys;                  // % of all cores: this process, the whole system (< 0: unknown)
    double ram_proc_gb, ram_used_gb, ram_total_gb;
    int cpu_cores;
} ovl_stats;

enum { OVL_COUNTER_OFF, OVL_COUNTER_FPS, OVL_COUNTER_FULL, OVL_COUNTERS };
enum { OVL_ACT_FPS60 = 1, OVL_ACT_FULLSCREEN = 2, OVL_ACT_CLOSE = 4 };   // the panel asked: toggle / toggle / close it

// main thread
void ovl_settings_load(void);                  // userdata/overlay.cfg (counter, corner, notice); TOTK_FPS_OVERLAY=1: the detailed counter
void ovl_stats_set(const ovl_stats *s);
void ovl_flags_set(int fps60, int fullscreen); // what the options show
int ovl_panel(void);
void ovl_panel_set(int on);
void ovl_counter_cycle(void);                  // F3
void ovl_mouse_pos(float x, float y);          // in the window's pixels
void ovl_mouse_button(int button, int down);   // 0 left, 1 right, 2 middle
void ovl_mouse_wheel(float dy);
unsigned ovl_take_actions(void);               // OVL_ACT_* asked since the last call

// present thread
void ovl_frame_time(double ms);                // the time since the previous game frame shown
int ovl_visible(void);                         // something to draw
int ovl_repaint(void);                         // the panel is open or moving: show the last frame again at the display's pace
// Draws into the swapchain image (in COLOR_ATTACHMENT_OPTIMAL) through `view`; `picture` is where the game's picture
// is in it; `slot` is the frame in flight (0 or 1: its buffers are free again once that slot's fence was waited on).
void ovl_draw(VkCommandBuffer cb, VkImageView view, VkExtent2D extent, VkRect2D picture, unsigned slot);

#ifdef __cplusplus
}
#endif
