// Device memory for the copies of game memory pools (backend_vk.c, "pool shadows"): blocks of BLOCK bytes, cut into
// pieces first-fit (a pool larger than a block gets a block of its own). One allocation per pool would exhaust the
// driver's allocation count (~4096 on Windows) with TOTK's ~2900 pools. A freed piece is reused once the GPU has finished
// every submission recorded up to then.
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
enum { BLOCK = 64u << 20, ALIGN = 256 };

typedef struct { VkDeviceSize off, size; } span;
typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; span *free; int nfree, capfree; } block_t;
typedef struct { int block; VkDeviceSize off, size; uint64_t fence; } piece;   // fence: freed, reusable when done
static block_t *g_blocks;
static int g_nblocks;
static piece *g_pieces;          // handle - 1 -> piece
static int g_npieces, g_cappieces;
static int *g_pending;           // freed handles waiting for their fence
static int g_npending, g_cappending;
static uint64_t g_bytes;         // in use
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int vkb_shadow_on(void) {   // discrete GPUs: game memory is across PCIe from the GPU
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_POOL_SHADOW");
        v = e && *e ? *e != '0' : vkc.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        VLOG("copies of game memory in device memory: %s", v ? "on (discrete GPU)" : "off (memory shared with the GPU)");
    }
    return v;
}

static int block_new(VkDeviceSize size) {
    block_t b = {0};
    b.size = size;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &b.buf), "shadow buffer")) return -1;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, b.buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) type = i;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = type};
    vkb_mtag = MT_POOL_COPY;
    if (type == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &b.mem) != VK_SUCCESS) {
        vk.vkDestroyBuffer(vkc.device, b.buf, NULL);
        VLOG("shadow memory: no %llu MiB of device memory", (unsigned long long)(size >> 20));
        return -1;
    }
    vk.vkBindBufferMemory(vkc.device, b.buf, b.mem, 0);
    b.capfree = 16;
    b.free = malloc(sizeof *b.free * (size_t)b.capfree);
    if (!b.free) die("shadow free list");
    b.free[0] = (span){0, size};
    b.nfree = 1;
    g_blocks = realloc(g_blocks, sizeof *g_blocks * (size_t)(g_nblocks + 1));
    if (!g_blocks) die("shadow blocks");
    g_blocks[g_nblocks] = b;
    return g_nblocks++;
}
static void span_free(block_t *b, VkDeviceSize off, VkDeviceSize size) {   // back into the sorted list, merged
    int i = 0;
    while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) {
        b->capfree *= 2;
        b->free = realloc(b->free, sizeof *b->free * (size_t)b->capfree);
        if (!b->free) die("shadow free list");
    }
    memmove(&b->free[i + 1], &b->free[i], sizeof *b->free * (size_t)(b->nfree - i));
    b->free[i] = (span){off, size};
    b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
        b->free[i].size += b->free[i + 1].size;
        memmove(&b->free[i + 1], &b->free[i + 2], sizeof *b->free * (size_t)(b->nfree - i - 2));
        b->nfree--;
    }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
        b->free[i - 1].size += b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], sizeof *b->free * (size_t)(b->nfree - i - 1));
        b->nfree--;
    }
}
static void reclaim(void) {   // freed pieces whose GPU work is done
    for (int k = 0; k < g_npending;) {
        piece *p = &g_pieces[g_pending[k] - 1];
        if (!mtl_wait(p->fence, 0)) { k++; continue; }
        span_free(&g_blocks[p->block], p->off, p->size);
        p->size = 0;
        g_pending[k] = g_pending[--g_npending];
    }
}

uint32_t vkb_shadow_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off) {
    size = (size + ALIGN - 1) & ~(VkDeviceSize)(ALIGN - 1);
    pthread_mutex_lock(&g_mu);
    reclaim();
    int bi = -1;
    VkDeviceSize at = 0;
    for (int i = 0; i < g_nblocks && bi < 0; i++)
        for (int k = 0; k < g_blocks[i].nfree; k++)
            if (g_blocks[i].free[k].size >= size) {
                bi = i; at = g_blocks[i].free[k].off;
                g_blocks[i].free[k].off += size; g_blocks[i].free[k].size -= size;
                if (!g_blocks[i].free[k].size) {
                    memmove(&g_blocks[i].free[k], &g_blocks[i].free[k + 1], sizeof g_blocks[i].free[0] * (size_t)(g_blocks[i].nfree - k - 1));
                    g_blocks[i].nfree--;
                }
                break;
            }
    if (bi < 0) {
        bi = block_new(size > BLOCK ? size : BLOCK);
        if (bi < 0) { pthread_mutex_unlock(&g_mu); return 0; }
        at = 0;
        g_blocks[bi].free[0].off = size; g_blocks[bi].free[0].size -= size;
        if (!g_blocks[bi].free[0].size) g_blocks[bi].nfree = 0;
    }
    if (g_npieces == g_cappieces) {
        g_cappieces = g_cappieces ? g_cappieces * 2 : 1024;
        g_pieces = realloc(g_pieces, sizeof *g_pieces * (size_t)g_cappieces);
        if (!g_pieces) die("shadow pieces");
    }
    g_pieces[g_npieces] = (piece){bi, at, size, 0};
    uint32_t h = (uint32_t)++g_npieces;
    g_bytes += size;
    *buf = g_blocks[bi].buf;
    *off = at;
    pthread_mutex_unlock(&g_mu);
    return h;
}
void vkb_shadow_free(uint32_t h) {
    if (!h) return;
    pthread_mutex_lock(&g_mu);
    piece *p = &g_pieces[h - 1];
    p->fence = mtl_last_fence() + 1;   // the submission being recorded may still use it
    g_bytes -= p->size;
    if (g_npending == g_cappending) {
        g_cappending = g_cappending ? g_cappending * 2 : 256;
        g_pending = realloc(g_pending, sizeof *g_pending * (size_t)g_cappending);
        if (!g_pending) die("shadow pending");
    }
    g_pending[g_npending++] = (int)h;
    pthread_mutex_unlock(&g_mu);
}
uint64_t vkb_shadow_bytes(void) { return g_bytes; }
