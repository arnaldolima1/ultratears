// 60 FPS mod (TOTK_FPS60=1): the measured frame step the patched game code reads (tools/lifter/patches.py, FPS60).
// Ported from MacNativeGames' games/totk/runtime/game_patches_v143.c (MIT): the engine steps its time by constants made
// for 30 Hz; with the frame controller allowed to pick interval 1 the game presents up to 60 times a second, and these
// values tell it how much time really passed. hle_nvn.c calls runtime_frame_step_update at every present.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// {seconds * 30 (the 30 Hz ratio), seconds, seconds * 60 (the nominal interval)}
volatile float totk_step[3] = {1.0f, 1.0f / 30.0f, 2.0f};

static int g_on = -1;
static void step_store(double seconds) {
    totk_step[0] = (float)(seconds * 30.0);
    totk_step[1] = (float)seconds;
    totk_step[2] = (float)(seconds * 60.0);
}

int totk_fps60(void) {
    if (g_on < 0) {
        const char *e = getenv("TOTK_FPS60");
        int on = e && !strcmp(e, "1");
        if (on) step_store(1.0 / 60.0);
        fprintf(stderr, "[totk] 60 FPS mod %s (TOTK_FPS60)\n", on ? "on: frame controller may pick interval 1, real-time frame step"
                                                                   : "off: the game's own 30 FPS");
        g_on = on;
    }
    return g_on;
}

// F2 in the F1 panel (window_sdl.c): on or off while playing; the patched sites check totk_fps60() each time.
void totk_fps60_set(int on) {
    if (totk_fps60() == on) return;
    if (on) step_store(1.0 / 60.0);
    g_on = on;
    fprintf(stderr, "[totk] 60 FPS mod switched %s\n", on ? "on" : "off");
}

// The average of the last five present intervals: no 1/120 s floor (a short interval counted longer ran the game
// fast), a hitch counted as at most 1/15 s, a pause (over 0.25 s) starting the average again.
void runtime_frame_step_update(uint64_t now_ns) {
    static uint64_t last;
    static double samples[5];
    static unsigned count, next;
    if (!totk_fps60() || !now_ns) return;
    if (!last) { last = now_ns; return; }
    double dt = (double)(now_ns - last) * 1e-9;
    last = now_ns;
    if (dt > 0.25 || dt < 0.001) { count = next = 0; return; }
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    samples[next++ % 5] = dt;
    if (count < 5) count++;
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += samples[i];
    step_store(sum / count);
}
