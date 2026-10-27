// Scene render resolution (TOTK_RENDER_RES=<w>x<h>, e.g. 2560x1440): the size the game renders its 3D scene at, read by
// the patched renderer setup (tools/lifter/patches.py, render_res) instead of the docked 1600x900. The scene is composed
// into the 1920x1080 canvas, so a bigger one is supersampled. Ported from MacNativeGames'
// games/totk/runtime/game_patches_v143.c (MIT).
// The scene's render targets come from DynamicTextureAllocatorMem, a fixed heap sized by
// System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs: above 1600x900 the game runs out of it. tools/make_res_overlay.py
// writes that file with the heaps grown (data/romfs-patches/scene-<w>x<h>, served by fs.c in place of the original), and
// the application pool the game is told of (svc.c) grows by the same amount, so no other heap shrinks.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"

#define SCENE_W 1600
#define SCENE_H 900
#define DTA_KIB 113664.0   // DynamicTextureAllocatorMem as shipped

volatile float totk_render_size[2];   // the patched renderer setup's width and height

static int g_res = -1;
static unsigned g_w, g_h;
static char g_overlay[256];

// 1 with a scene size other than the game's own (in totk_render_size).
int totk_render_res(void) {
    if (g_res >= 0) return g_res;
    const char *e = getenv("TOTK_RENDER_RES");
    unsigned w = 0, h = 0;
    char rest;
    g_res = e && *e && strcmp(e, "0") && sscanf(e, "%ux%u%c", &w, &h, &rest) == 2 && w >= 640 && h >= 360 && w <= 4096 && h <= 4096 &&
            (w != SCENE_W || h != SCENE_H);
    if (e && *e && strcmp(e, "0") && !g_res && !(w == SCENE_W && h == SCENE_H))
        fprintf(stderr, "[totk] TOTK_RENDER_RES=%s is not <width>x<height> (640x360 .. 4096x4096): the game's own 1600x900\n", e);
    if (g_res) {
        g_w = w; g_h = h;
        totk_render_size[0] = (float)w;
        totk_render_size[1] = (float)h;
        if ((uint64_t)w * h > (uint64_t)SCENE_W * SCENE_H) {
            snprintf(g_overlay, sizeof g_overlay, "data/romfs-patches/scene-%ux%u", w, h);
            char heapdef[512];
            struct stat st;
            snprintf(heapdef, sizeof heapdef, "%s/System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs", g_overlay);
            if (stat(heapdef, &st)) {
                fprintf(stderr, "[totk] %s missing: run tools/make_res_overlay.py %ux%u (the launcher does when the resolution is chosen)\n", heapdef, w, h);
                runtime_fail("the heap overlay for TOTK_RENDER_RES is missing (tools/make_res_overlay.py)");
            }
        }
        fprintf(stderr, "[totk] scene render resolution %ux%u (TOTK_RENDER_RES)%s%s\n", w, h, g_overlay[0] ? ", heaps from " : "", g_overlay);
    }
    return g_res;
}

// The RomFS overlay folder for the bigger heaps, NULL without one.
const char *totk_romfs_overlay(void) { return totk_render_res() && g_overlay[0] ? g_overlay : NULL; }

// Bytes added to the application pool: the heaps' growth for the scene, a 1.6 margin on its pixel ratio (the same
// formula as tools/make_res_overlay.py: keep the two in step).
uint64_t totk_render_res_extra_bytes(void) {
    if (!totk_render_res() || (uint64_t)g_w * g_h <= (uint64_t)SCENE_W * SCENE_H) return 0;
    double grow = ((double)g_w * g_h / ((double)SCENE_W * SCENE_H) - 1) * 1.6;
    uint64_t kib = (uint64_t)ceil(DTA_KIB * (1 + grow) / 1024) * 1024 - (uint64_t)DTA_KIB;
    return kib << 10;
}
