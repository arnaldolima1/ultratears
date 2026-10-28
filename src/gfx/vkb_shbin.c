// The driver's compiled shaders on disk: one file next to the translated shaders (<shader dir>/shader-binaries.bin),
// records of (shader key, vertex variant) -> the bytes vkGetShaderBinaryDataEXT gave. Creating a shader object from
// them skips the compile (RTX 3060: 1.1 ms instead of 2.7 ms from SPIR-V, 26 KiB a shader, 2026-10-28), and it lets
// shader objects follow the game's programs - made when the game sets a program, destroyed when it finalizes it -
// without compiling again on the way back (vkb_shader.c). The file belongs to one driver build: its header holds the
// driver's shaderBinaryUUID and shaderBinaryVersion; another driver starts the file over.
//   header (32 bytes): "UTSHBIN1", UUID (16), version (u32), 0 (u32)
//   record: tag "SHB1", key (32 bytes, the SHA-256 the key's hex digits spell), variant masks (5 x u32), size (u32), data
// A record cut short (the game stopped while writing) and what follows it are dropped when the file is opened.
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_shbin.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

enum { CAP = 1 << 17, HEADER = 32, RECORD = 60 };
static const char MAGIC[8] = {'U', 'T', 'S', 'H', 'B', 'I', 'N', '1'};
static const uint32_t TAG = 0x31424853;   // "SHB1"

typedef struct { uint8_t key[32]; vkb_shbin_variant v; uint64_t off; uint32_t size; uint8_t state; } slot_t;   // state: 0 free, 1 used, 2 dropped
static slot_t *g_tab;
static FILE *g_f;
static uint64_t g_end, g_records, g_bytes, g_hits, g_misses;
static int g_init;   // 0 not yet, 1 open, -1 none (no file: nothing kept)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int unhex(const char *key, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = key[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}
static slot_t *find(const uint8_t key[32], const vkb_shbin_variant *v, int add) {   // g_mu held
    uint32_t h;
    memcpy(&h, key, 4);
    h ^= v->m[0] * 0x9E3779B1u ^ v->m[2] * 0x85EBCA77u;
    slot_t *grave = NULL;
    for (uint32_t i = 0; i < CAP; i++) {
        slot_t *s = &g_tab[(h + i) & (CAP - 1)];
        if (s->state == 0) {
            if (!add) return NULL;
            s = grave ? grave : s;
            memcpy(s->key, key, 32);
            s->v = *v;
            return s;
        }
        if (s->state == 2) { if (!grave) grave = s; continue; }
        if (!memcmp(s->key, key, 32) && !memcmp(&s->v, v, sizeof *v)) return s;
    }
    return grave && add ? (memcpy(grave->key, key, 32), grave->v = *v, grave) : NULL;
}

static void open_file(void) {   // g_mu held
    g_init = -1;
    if (!(g_tab = calloc(CAP, sizeof *g_tab))) return;
    const char *dir = getenv("TOTK_SHADER_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/shader-binaries.bin", dir ? dir : "data/shaders/ryujinx");
    uint8_t want[HEADER] = {0};
    memcpy(want, MAGIC, 8);
    memcpy(want + 8, vkc.so.shaderBinaryUUID, 16);
    memcpy(want + 24, &vkc.so.shaderBinaryVersion, 4);
    FILE *f = fopen(path, "r+b");
    uint8_t have[HEADER];
    if (f && (fread(have, 1, HEADER, f) != HEADER || memcmp(have, want, HEADER))) {
        VLOG("shader binaries: %s is for another driver or damaged: started over", path);
        fclose(f);
        f = NULL;
    }
    if (!f) {
        if (!(f = fopen(path, "w+b")) || fwrite(want, 1, HEADER, f) != HEADER) {
            VLOG("shader binaries: cannot write %s: shaders are compiled every run", path);
            if (f) fclose(f);
            return;
        }
        fflush(f);
    }
    uint64_t at = HEADER;
    for (;;) {   // the index: record headers, data skipped
        uint8_t r[RECORD];
        if (_fseeki64(f, (long long)at, SEEK_SET) || fread(r, 1, RECORD, f) != RECORD) break;
        uint32_t tag, size;
        memcpy(&tag, r, 4);
        memcpy(&size, r + 56, 4);
        if (tag != TAG || !size) break;
        if (_fseeki64(f, (long long)(at + RECORD + size - 1), SEEK_SET)) break;
        uint8_t last;
        if (fread(&last, 1, 1, f) != 1) break;   // cut short
        vkb_shbin_variant v;
        memcpy(&v, r + 36, sizeof v);
        slot_t *s = find(r + 4, &v, 1);
        if (!s) break;
        if (s->state != 1) g_records++; else g_bytes -= s->size;
        s->off = at + RECORD; s->size = size; s->state = 1;
        g_bytes += size;
        at += RECORD + size;
    }
    if (_chsize_s(_fileno(f), (long long)at) == 0) {}   // drop a record cut short (and anything after it)
    g_f = f;
    g_end = at;
    g_init = 1;
    VLOG("shader binaries: %llu in %s (%.1f MiB)", (unsigned long long)g_records, path, g_bytes / 1048576.0);
}
static int ready(void) {   // g_mu held
    if (!g_init) open_file();
    return g_init == 1;
}

int vkb_shbin_get(const char *key, const vkb_shbin_variant *v, void **data, size_t *size) {
    uint8_t k[32];
    if (!unhex(key, k)) return 0;
    pthread_mutex_lock(&g_mu);
    slot_t *s = ready() ? find(k, v, 0) : NULL;
    int ok = 0;
    if (s && s->state == 1 && (*data = malloc(s->size))) {
        ok = !_fseeki64(g_f, (long long)s->off, SEEK_SET) && fread(*data, 1, s->size, g_f) == s->size;
        if (ok) *size = s->size; else { free(*data); *data = NULL; }
    }
    if (ok) g_hits++; else g_misses++;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int vkb_shbin_has(const char *key, const vkb_shbin_variant *v) {
    uint8_t k[32];
    if (!unhex(key, k)) return 0;
    pthread_mutex_lock(&g_mu);
    slot_t *s = ready() ? find(k, v, 0) : NULL;
    int has = s && s->state == 1;
    pthread_mutex_unlock(&g_mu);
    return has;
}

void vkb_shbin_put(const char *key, const vkb_shbin_variant *v, const void *data, size_t size) {
    uint8_t k[32];
    if (!size || size > (64u << 20) || !unhex(key, k)) return;
    pthread_mutex_lock(&g_mu);
    slot_t *s = ready() ? find(k, v, 1) : NULL;
    if (s && s->state != 1) {
        uint8_t r[RECORD];
        uint32_t n = (uint32_t)size;
        memcpy(r, &TAG, 4);
        memcpy(r + 4, k, 32);
        memcpy(r + 36, v, sizeof *v);
        memcpy(r + 56, &n, 4);
        if (!_fseeki64(g_f, (long long)g_end, SEEK_SET) && fwrite(r, 1, RECORD, g_f) == RECORD && fwrite(data, 1, size, g_f) == size &&
            fflush(g_f) == 0) {
            s->off = g_end + RECORD; s->size = n; s->state = 1;
            g_end += RECORD + size;
            g_records++;
            g_bytes += size;
        } else if (s->state == 0) memset(s, 0, sizeof *s);
    }
    pthread_mutex_unlock(&g_mu);
}

void vkb_shbin_drop(const char *key, const vkb_shbin_variant *v) {
    uint8_t k[32];
    if (!unhex(key, k)) return;
    pthread_mutex_lock(&g_mu);
    slot_t *s = ready() ? find(k, v, 0) : NULL;
    if (s && s->state == 1) { s->state = 2; g_records--; g_bytes -= s->size; }
    pthread_mutex_unlock(&g_mu);
}

void vkb_shbin_stats(uint64_t *records, uint64_t *bytes, uint64_t *hits, uint64_t *misses) {
    pthread_mutex_lock(&g_mu);
    *records = g_records; *bytes = g_bytes; *hits = g_hits; *misses = g_misses;
    pthread_mutex_unlock(&g_mu);
}
