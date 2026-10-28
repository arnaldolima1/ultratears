// The driver's compiled shaders on disk (vkb_shbin.c): one file, records by shader key and variant.
#pragma once
#include <stddef.h>
#include <stdint.h>

// variant: the vertex shader variant's masks (vkb_variant: intmask, sintmask, outmask, out_umask, out_imask); all 0 for
// the shader itself.
typedef struct { uint32_t m[5]; } vkb_shbin_variant;

// The binary of (key, variant) made by this driver: *data (malloc) of *size bytes. 0: not there.
int vkb_shbin_get(const char *key, const vkb_shbin_variant *v, void **data, size_t *size);
int vkb_shbin_has(const char *key, const vkb_shbin_variant *v);   // without reading it
// Keep the binary of (key, variant); a later put of the same one is ignored.
void vkb_shbin_put(const char *key, const vkb_shbin_variant *v, const void *data, size_t size);
// The binary was refused by the driver (vkCreateShadersEXT): forgotten, made again from SPIR-V.
void vkb_shbin_drop(const char *key, const vkb_shbin_variant *v);
void vkb_shbin_stats(uint64_t *records, uint64_t *bytes, uint64_t *hits, uint64_t *misses);
