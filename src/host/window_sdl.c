// The window (milestone 3.2): SDL3 on the process's main thread — window, keyboard, game controllers with rumble —
// while the game runs on its own thread (boot.c). The picture goes through src/vk/present.c.
//  - keyboard: physical SDL scancodes use the saved bindings in keyboard_input.c;
//  - mouse camera: relative motion drives the right stick; F8 releases/recaptures the cursor;
//  - game controllers: buttons by position as on a Switch controller (the right face button is A), sticks with Y up;
//  - rumble: the game's vibration values (hid_feedback.c) drive the controller's low / high frequency motors.
//  - motion: the controller's gyro and accelerometer feed motion_fusion.c (gyro aiming); F11 / Alt+Enter: fullscreen.
//  - the overlay (src/vk/overlay.cpp): F3 cycles the counter (off / FPS / detailed; TOTK_FPS_OVERLAY=1 starts with it),
//    F1 opens the panel (frame times, CPU, GPU, memory, options; it takes the mouse while open, Esc closes it);
//    F2: the 60 FPS mod on / off (src/host/frame_step.c).
// A launcher hands the window over with TOTK_WINDOW=x,y,w,h (pixels), TOTK_BORDERLESS=1, TOTK_FULLSCREEN=1 and
// TOTK_START_HIDDEN=1: the window stays hidden until the first game frame is shown, so its own window can stay up
// (a loading screen) until then.
#include <SDL3/SDL.h>
#include <unistd.h>
#define SDL_MAIN_HANDLED   // main() is boot.c's; SDL only gets SDL_SetMainReady
#include <SDL3/SDL_main.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
// kernel32's K32GetProcessMemoryInfo (psapi.h is not in the build's toolchain): PROCESS_MEMORY_COUNTERS
typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, quota[4], pagefile, peak_pagefile; } proc_mem;
BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, proc_mem *counters, DWORD size);
#endif

#include "host.h"
#include "keyboard_input.h"
#include "../vk/overlay.h"
#include "../vk/present.h"

static SDL_Window *window;
static SDL_Gamepad *pad;
static SDL_Mutex *pad_lock;
static bool hidden_until_frame;
uint64_t vkb_gpu_ns_total(void);   // src/gfx/backend_vk.c
void vkb_passprof_request(void);    // F9: the GPU time of each render pass of a frame, into the log
int totk_fps60(void);              // src/host/frame_step.c
void totk_fps60_set(int on);

static bool env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0");
}

// Start-up screen (with TOTK_START_HIDDEN, i.e. from the launcher): startup.txt in the game folder says where the start
// is - "starting", "shaders <done> <total>" while the shader list is made ahead (vkb_shader.c), "ready" once the window
// shows - and the launcher's transition screen reads it. The window stays hidden while the list is made (at most
// STARTUP_HOLD_S after the first frame): the first draws then find their shaders made instead of stalling.
#define STARTUP_HOLD_S 180
int vkb_prewarm_progress(uint64_t *done, uint64_t *total);   // gfx/vkb_shader.c
static void startup_status(const char *text) {
    FILE *f = fopen("startup.txt.tmp", "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
#ifdef _WIN32   // (rename does not replace an existing file there)
    MoveFileExA("startup.txt.tmp", "startup.txt", MOVEFILE_REPLACE_EXISTING);
#else
    rename("startup.txt.tmp", "startup.txt");
#endif
}
static bool startup_holding(void) {
    static uint64_t first_frame, last_write;
    uint64_t now = SDL_GetTicksNS(), done, total;
    if (!first_frame) first_frame = now;
    int left = vkb_prewarm_progress(&done, &total);
    bool hold = left > 0 && now - first_frame < (uint64_t)STARTUP_HOLD_S * 1000000000ull;
    if (hold && now - last_write > 250000000ull) {
        char s[64];
        snprintf(s, sizeof s, "shaders %llu %llu\n", (unsigned long long)done, (unsigned long long)total);
        startup_status(s);
        last_write = now;
    }
    return hold;
}

static void mouse_capture(int on) {
    on = on && keyboard_input_mouse_mode();
    if (SDL_GetWindowRelativeMouseMode(window) != !!on) {
        if (!SDL_SetWindowRelativeMouseMode(window, !!on))
            fprintf(stderr, "[totk-input] cannot %s mouse: %s\n", on ? "capture" : "release", SDL_GetError());
        SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
    }
    bool actual = window && SDL_GetWindowRelativeMouseMode(window);
    mouse_camera_set_capture(actual);
    runtime_input_mouse_clear();
    keyboard_input_release_mouse_buttons();
}

// Switch npad button bits (libnx HidNpadButton): A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down
static int32_t stick(Sint16 v, int invert) {
    int32_t x = invert ? -(int32_t)v : v;
    return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
}
static void pad_update_at(uint64_t event_ns) {
    RuntimeGamepadState s = {0};
    if (pad) {
        static const struct { SDL_GamepadButton b; uint64_t bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, 1}, {SDL_GAMEPAD_BUTTON_SOUTH, 2}, {SDL_GAMEPAD_BUTTON_NORTH, 4},
            {SDL_GAMEPAD_BUTTON_WEST, 8}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 16}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 32},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 64}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 128},
            {SDL_GAMEPAD_BUTTON_START, 1024}, {SDL_GAMEPAD_BUTTON_BACK, 2048}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 4096},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 8192}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 16384},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 32768}};
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GetGamepadButton(pad, map[i].b)) s.buttons |= map[i].bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) s.buttons |= 256;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) s.buttons |= 512;
        s.lx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0);
        s.ly = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), 1);
        s.rx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), 0);
        s.ry = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), 1);
        s.connected = 1;
        uint64_t ts = event_ns ? event_ns : SDL_GetTicksNS();
        mouse_camera_offer_stick((float)s.rx / 32767.0f, (float)s.ry / 32767.0f, ts);
    }
    runtime_gamepad_set(&s);
}
static void pad_update(void) { pad_update_at(SDL_GetTicksNS()); }

// hid_feedback.c: the game's vibration value for one side (0 left, 1 right). A Switch controller has a low and a high
// frequency actuator per side; an SDL controller has a low and a high frequency motor, so each takes the stronger side.
// The game sends values every frame; the 250 ms duration stops the motors if it stops sending.
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)freq_low; (void)freq_high;
    static float low[2], high[2];
    if (side > 1 || !pad_lock) return;
    SDL_LockMutex(pad_lock);
    low[side] = amp_low < 0 ? 0 : amp_low > 1 ? 1 : amp_low;
    high[side] = amp_high < 0 ? 0 : amp_high > 1 ? 1 : amp_high;
    float l = low[0] > low[1] ? low[0] : low[1], h = high[0] > high[1] ? high[0] : high[1];
    if (pad) SDL_RumbleGamepad(pad, (Uint16)(l * 65535), (Uint16)(h * 65535), 250);
    SDL_UnlockMutex(pad_lock);
}

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
// (fork) One Dock icon at a time: started by the launcher (TOTK_START_HIDDEN) the game stays out of the Dock while
// the launcher's window stands in for it (SDL_HINT_MAC_BACKGROUND_APP), and comes in with its window, with the port's
// icon (assets/icon.ico; a bare executable gets the generic "exec" one). The launcher leaves the Dock then.
static id objc_send(id obj, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(obj, sel_registerName(sel)); }
static void mac_dock_show(void) {
    id app = objc_send((id)objc_getClass("NSApplication"), "sharedApplication");
    ((BOOL (*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"), 0);   // Regular
    id path = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
                                                            sel_registerName("stringWithUTF8String:"), "assets/icon.ico");
    id img = ((id (*)(id, SEL, id))objc_msgSend)(objc_send((id)objc_getClass("NSImage"), "alloc"),
                                                 sel_registerName("initWithContentsOfFile:"), path);
    if (img) ((void (*)(id, SEL, id))objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), img);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(app, sel_registerName("activateIgnoringOtherApps:"), YES);
}
#endif

int window_init(void) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");   // 10 ms device buffer at 48 kHz
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");        // the exe's icon resource (assets/icon.ico via build/ultratears.rc) for the window
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#ifdef __APPLE__
    // (fork) dlopen does not look in Homebrew's lib folder: the Vulkan loader (which finds MoltenVK) by its path
    if (!getenv("SDL_VULKAN_LIBRARY"))
        for (const char *const *p = (const char *const[]){"/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib", NULL}; *p; p++)
            if (access(*p, R_OK) == 0) { SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, *p); break; }
    if (env_on("TOTK_START_HIDDEN")) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[totk] SDL: %s\n", SDL_GetError());
        return 0;
    }
    pad_lock = SDL_CreateMutex();
    hidden_until_frame = env_on("TOTK_START_HIDDEN");
    if (hidden_until_frame) startup_status("starting\n");
    ovl_settings_load();
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (hidden_until_frame) flags |= SDL_WINDOW_HIDDEN;
    if (env_on("TOTK_BORDERLESS")) flags |= SDL_WINDOW_BORDERLESS;
    window = SDL_CreateWindow("UltraTears", 1280, 720, flags);
    if (!window) { fprintf(stderr, "[totk] window: %s\n", SDL_GetError()); return 0; }
#ifdef __APPLE__
    if (!hidden_until_frame) mac_dock_show();
#endif
    int x, y, w, h;
    const char *geo = getenv("TOTK_WINDOW");
    if (geo && sscanf(geo, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
#ifdef _WIN32   // in pixels, as the launcher measured its own window (SDL's sizes may be scaled by the display)
        HWND hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
#else
        SDL_SetWindowPosition(window, x, y);
        SDL_SetWindowSize(window, w, h);
#endif
    }
    if (env_on("TOTK_FULLSCREEN")) SDL_SetWindowFullscreen(window, true);   // on the display the window is on
    keyboard_input_init();   // (upstream PR #20) bindings and the mouse camera, from TOTK_KEY_* / TOTK_MOUSE_*
    mouse_capture((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    if (!vkp_init(window))
        runtime_fail("Vulkan initialization failed (Vulkan 1.3 with VK_EXT_shader_object is required)");
    return 1;
}

// Motion: the controller's accelerometer and gyro (SDL_EVENT_GAMEPAD_SENSOR_UPDATE) go to motion_fusion.c.
static void pad_attached(void) {   // pad_lock held; pad may be NULL
    if (!pad) { runtime_motion_live(0); return; }
    fprintf(stderr, "[totk] game controller: %s\n", SDL_GetGamepadName(pad));
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
        const char *axes = getenv("TOTK_GYRO_AXES");
        if (runtime_motion_axes(axes) < 0) fprintf(stderr, "[totk] TOTK_GYRO_AXES=%s is not like \"-x,y,-z\": ignored\n", axes);
        runtime_motion_live(1);
        fprintf(stderr, "[totk] controller motion sensors on (%.0f Hz gyro)\n", SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO));
    } else {
        runtime_motion_live(0);
        fprintf(stderr, "[totk] controller has no motion sensors: no gyro aiming\n");
    }
}
// SDL's controller frame: x right, y out of the face, z toward the player (the bottom edge); acceleration in m/s^2 as
// the force holding it up (face up reads y = +9.8). The Switch frame (runtime_motion_feed): x right, y toward the top
// edge, z out of the face, acceleration as gravity (face up reads z = -1 G). (x, -z, y) is a rotation, so the gyro
// keeps its sense; the acceleration also changes sign.
static void pad_sensor(const SDL_GamepadSensorEvent *e) {
    static double acc[3] = {0, 0, -1};
    const float *d = e->data;
    if (e->sensor == SDL_SENSOR_ACCEL) {
        acc[0] = -d[0] / SDL_STANDARD_GRAVITY;
        acc[1] = d[2] / SDL_STANDARD_GRAVITY;
        acc[2] = -d[1] / SDL_STANDARD_GRAVITY;
    } else if (e->sensor == SDL_SENSOR_GYRO) {
        double gyro[3] = {d[0], -d[2], d[1]};
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }
}

static void open_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (!pad) {
        pad = SDL_OpenGamepad(id);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}
static void close_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (pad && SDL_GetGamepadID(pad) == id) {
        SDL_CloseGamepad(pad);
        pad = NULL;
        int n = 0;   // another one still connected takes over
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}

static void toggle_fullscreen(void) {
    bool on = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, !on);   // borderless desktop fullscreen (no display mode change)
}

// This process's CPU use and the whole system's (all cores = 100 %), and memory, since the last call.
static void usage(ovl_stats *s) {
    s->cpu_proc = 0; s->cpu_sys = -1; s->ram_proc_gb = s->ram_used_gb = s->ram_total_gb = 0;
#ifdef _WIN32
    static uint64_t last_cpu, last_wall, last_idle, last_busy;
    FILETIME created, exited, kernel, user, idle;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        uint64_t used = ((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                        ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime);   // 100 ns units
        uint64_t wall = runtime_now_ns() / 100;
        if (last_wall && wall > last_wall && s->cpu_cores > 0) s->cpu_proc = 100.0 * (double)(used - last_cpu) / ((double)(wall - last_wall) * s->cpu_cores);
        last_cpu = used; last_wall = wall;
    }
    if (GetSystemTimes(&idle, &kernel, &user)) {   // kernel time includes the idle time
        uint64_t i = (uint64_t)idle.dwHighDateTime << 32 | idle.dwLowDateTime;
        uint64_t busy = ((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                        ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime);
        if (last_busy && busy > last_busy) s->cpu_sys = 100.0 * (1.0 - (double)(i - last_idle) / (double)(busy - last_busy));
        last_idle = i; last_busy = busy;
    }
    proc_mem pm = {sizeof pm};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) s->ram_proc_gb = pm.ws / 1073741824.0;
    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        s->ram_total_gb = ms.ullTotalPhys / 1073741824.0;
        s->ram_used_gb = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
    }
#endif
}

// Game frame rate and shown frame rate in the title and the overlay's figures, twice a second.
static void title_update(void) {
    static uint64_t t0, game0, shown0, gpu0;
    uint64_t now = runtime_now_ns(), game = vkp_frames(), shown = vkp_presented(), gpu = vkb_gpu_ns_total();
    ovl_flags_set(totk_fps60(), (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
    if (!t0) { t0 = now; game0 = game; shown0 = shown; gpu0 = gpu; return; }
    if (now - t0 < 500000000) return;
    double secs = (now - t0) / 1e9;
    ovl_stats s = {.fps = (game - game0) / secs, .shown_fps = (shown - shown0) / secs, .worst_ms = vkp_worst_frame_ms(),
                   .cpu_cores = SDL_GetNumLogicalCPUCores()};
    s.gpu_ms = game > game0 ? (gpu - gpu0) / 1e6 / (double)(game - game0) : 0;
    s.gpu_load = (gpu - gpu0) / (double)(now - t0);
    if (s.gpu_load > 1) s.gpu_load = 1;
    usage(&s);
    ovl_stats_set(&s);
    char title[128];
    snprintf(title, sizeof title, "UltraTears  |  game %.1f fps  |  shown %.1f fps", s.fps, s.shown_fps);
    SDL_SetWindowTitle(window, title);
    t0 = now; game0 = game; shown0 = shown; gpu0 = gpu;
}

// Hang watchdog: no game frame for 3 s (and again at 10 s) with the game started: every thread's stack goes into the
// log (profiler.c), to tell where a freeze is stuck.
static void hang_watch(void) {
#ifdef _WIN32
    static uint64_t last_frames, since;
    static int told;
    uint64_t f = vkp_frames(), now = SDL_GetTicksNS();
    if (f != last_frames || !vkp_game_shown() || hidden_until_frame) { last_frames = f; since = now; told = 0; return; }
    if (told < 1 && now - since > 3000000000ull) { told = 1; runtime_dump_stacks("no game frame for 3 s"); }
    if (told < 2 && now - since > 10000000000ull) { told = 2; runtime_dump_stacks("no game frame for 10 s"); }
#endif
}

// F1: the panel takes the mouse (free cursor) until it closes, then the camera has it again.
static void panel_toggle(int on) {
    ovl_panel_set(on);
    mouse_capture(!on && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS));
    if (on) {
        float x, y;
        SDL_GetMouseState(&x, &y);
        float d = SDL_GetWindowPixelDensity(window);
        ovl_mouse_pos(x * d, y * d);
    }
}
_Noreturn void window_loop(void) {
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    fprintf(stderr, "[totk] window closed\n");
                    _Exit(0);
                case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F8) {
                        if (keyboard_input_mouse_mode() && !ovl_panel()) mouse_capture(!SDL_GetWindowRelativeMouseMode(window));
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                        (e.key.scancode == SDL_SCANCODE_F11 ||
                         (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        toggle_fullscreen();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F9) {
                        vkb_passprof_request();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F3) {
                        ovl_counter_cycle();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F1) {
                        panel_toggle(!ovl_panel());
                        break;
                    }
                    if (ovl_panel() && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                        panel_toggle(0);
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F2) {
                        totk_fps60_set(!totk_fps60());
                        break;
                    }
                    if (!e.key.repeat) keyboard_input_key(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN);
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION: {
                    if (ovl_panel()) {   // the panel has the mouse
                        float d = SDL_GetWindowPixelDensity(window);
                        ovl_mouse_pos(e.motion.x * d, e.motion.y * d);
                        break;
                    }
                    bool captured = SDL_GetWindowRelativeMouseMode(window);
                    mouse_camera_set_capture(captured);
                    if (captured) {
                        uint64_t ts = e.motion.timestamp ? e.motion.timestamp : SDL_GetTicksNS();
                        mouse_camera_push_motion((double)e.motion.xrel, (double)e.motion.yrel, ts);
                        runtime_input_mouse_motion(e.motion.xrel, e.motion.yrel);
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ovl_panel()) {
                        ovl_mouse_button(e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2, 1);
                        break;
                    }
                    if (keyboard_input_mouse_mode() && !SDL_GetWindowRelativeMouseMode(window)) {
                        if (e.button.button == SDL_BUTTON_LEFT) mouse_capture(1);
                        break;   // recapture is not an attack; free-cursor clicks do not drive the game
                    }
                    keyboard_input_mouse_button(e.button.button, 1);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ovl_panel()) ovl_mouse_button(e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2, 0);
                    keyboard_input_mouse_button(e.button.button, 0);   // (a button held when the panel opened is released)
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    if (ovl_panel()) ovl_mouse_wheel(e.wheel.y);
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: mouse_capture(0); mouse_camera_reset(MOUSE_RESET_FOCUS_LOST); keyboard_input_clear(); break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: mouse_capture(!ovl_panel()); pad_update(); break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkp_resized(); break;
                case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    pad_update_at(e.gaxis.timestamp ? e.gaxis.timestamp : SDL_GetTicksNS());
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    pad_update_at(e.gbutton.timestamp ? e.gbutton.timestamp : SDL_GetTicksNS());
                    break;
                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: pad_sensor(&e.gsensor); break;
                default: break;
                }
            } while (SDL_PollEvent(&e));
        }
        if (hidden_until_frame && vkp_game_shown() && !startup_holding()) {
            hidden_until_frame = false;
            startup_status("ready\n");
#ifdef __APPLE__
            mac_dock_show();
#endif
            SDL_ShowWindow(window);
            SDL_RaiseWindow(window);
        }
        unsigned act = ovl_take_actions();   // what the panel asked for
        if (act & OVL_ACT_FPS60) totk_fps60_set(!totk_fps60());
        if (act & OVL_ACT_FULLSCREEN) toggle_fullscreen();
        if (act & OVL_ACT_CLOSE) panel_toggle(0);
        if (pad) pad_update();
        title_update();
        hang_watch();
    }
}
