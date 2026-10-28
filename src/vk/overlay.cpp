// The in-game overlay (see overlay.h): Dear ImGui (third_party/imgui, MIT) with the launcher's look - its colours,
// Cinzel for titles and Nunito Sans for text (assets/fonts, OFL; ImGui's own font if they are missing).
// ImGui lives on the present thread only: the main thread's mouse and figures come in under g_mu, and what the panel
// asks for goes back as actions (ovl_take_actions) the main thread carries out (SDL calls belong there).
// Drawing: ImGui's Vulkan backend shaders (overlay_spv.h) as shader objects, like the replayer's, into the swapchain
// image with dynamic rendering, after the game's picture was blitted there (present.c).
#include <algorithm>   // (the game folder's toolchain has a few C++ headers only: C ones and builtins otherwise)
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "overlay.h"
#include "overlay_spv.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/sysctl.h>
#endif

extern "C" {
#include "version.h"
extern volatile float totk_render_size[2];   // the game's scene size (src/host/render_res.c)
uint64_t runtime_now_ns(void);
}

#define SLOTS 2      // frames in flight (present.c)
#define HIST 240     // frame times kept for the graph
#define LOADS 120    // CPU / GPU samples kept (one per half second: a minute)

// ---------------- state shared with the main thread ----------------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ovl_stats g_stats;                  // under g_mu
static uint64_t g_stats_seq;               // under g_mu
enum { IN_POS, IN_BUTTON, IN_WHEEL };
static struct { int type; float a, b; } g_in[256];
static int g_nin;                          // under g_mu
// set by either thread: read and written with the __atomic builtins (LD / ST)
static int g_panel, g_counter = OVL_COUNTER_OFF, g_corner = 1, g_fps60, g_fullscreen, g_toast = 1;   // corner 1: top right, clear of the hearts
static unsigned g_actions;
#define LD(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define ST(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELAXED)
#define ASK(a) __atomic_fetch_or(&g_actions, (unsigned)(a), __ATOMIC_RELAXED)

static void settings_save() {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (FILE *f = fopen("userdata/overlay.cfg", "w")) {
        fprintf(f, "v=2\ncounter=%d\ncorner=%d\ntoast=%d\n", LD(g_counter), LD(g_corner), LD(g_toast));
        fclose(f);
    }
    pthread_mutex_unlock(&mu);
}

void ovl_settings_load(void) {
    if (FILE *f = fopen("userdata/overlay.cfg", "r")) {
        char line[64];
        int v, version = 1, counter = -1, corner = -1, toast = -1;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "v=%d", &v) == 1) version = v;
            if (sscanf(line, "counter=%d", &v) == 1 && v >= 0 && v < OVL_COUNTERS) counter = v;
            if (sscanf(line, "corner=%d", &v) == 1 && v >= 0 && v < 4) corner = v;
            if (sscanf(line, "toast=%d", &v) == 1) toast = !!v;
        }
        fclose(f);
        if (version >= 2) {   // (version 1 files: the first overlay's defaults, not a choice)
            if (counter >= 0) ST(g_counter, counter);
            if (corner >= 0) ST(g_corner, corner);
        }
        if (toast >= 0) ST(g_toast, toast);
    }
    const char *e = getenv("TOTK_FPS_OVERLAY");   // the launcher's "FPS counter": the detailed one
    if (e && *e && strcmp(e, "0") && LD(g_counter) == OVL_COUNTER_OFF) ST(g_counter, OVL_COUNTER_FULL);
}
void ovl_stats_set(const ovl_stats *s) {
    pthread_mutex_lock(&g_mu);
    g_stats = *s;
    g_stats_seq++;
    pthread_mutex_unlock(&g_mu);
}
void ovl_flags_set(int fps60, int fullscreen) { ST(g_fps60, fps60); ST(g_fullscreen, fullscreen); }
int ovl_panel(void) { return LD(g_panel); }
void ovl_panel_set(int on) { ST(g_panel, !!on); }
void ovl_counter_cycle(void) {
    static const int next[OVL_COUNTERS] = {OVL_COUNTER_FULL, OVL_COUNTER_OFF, OVL_COUNTER_FPS};   // off, detailed, FPS only
    ST(g_counter, next[LD(g_counter)]);
    settings_save();
}
static void push_input(int type, float a, float b) {
    pthread_mutex_lock(&g_mu);
    if (type == IN_POS && g_nin && g_in[g_nin - 1].type == IN_POS) g_nin--;   // only the newest position counts
    if (g_nin < (int)(sizeof g_in / sizeof *g_in)) g_in[g_nin++] = {type, a, b};
    pthread_mutex_unlock(&g_mu);
}
void ovl_mouse_pos(float x, float y) { push_input(IN_POS, x, y); }
void ovl_mouse_button(int button, int down) { push_input(IN_BUTTON, (float)button, (float)down); }
void ovl_mouse_wheel(float dy) { push_input(IN_WHEEL, 0, dy); }
unsigned ovl_take_actions(void) { return __atomic_exchange_n(&g_actions, 0u, __ATOMIC_RELAXED); }

// ---------------- figures kept by the present thread ----------------
static float g_ft[HIST];                   // frame times (ms), a ring
static int g_ft_at, g_ft_n;
void ovl_frame_time(double ms) {
    g_ft[g_ft_at] = (float)ms;
    g_ft_at = (g_ft_at + 1) % HIST;
    if (g_ft_n < HIST) g_ft_n++;
}
static float g_cpu_hist[LOADS], g_gpu_hist[LOADS];
static int g_load_n;
static ovl_stats S;                        // the newest figures
static double g_vram_used, g_vram_total;   // GB (used: 0 without VK_EXT_memory_budget)

static void vram_read() {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {};
    b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 p = {};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    if (vkc.has_memory_budget) p.pNext = &b;
    if (!vk.vkGetPhysicalDeviceMemoryProperties2) return;
    vk.vkGetPhysicalDeviceMemoryProperties2(vkc.gpu, &p);
    double used = 0, total = 0;
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; i++)
        if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            total += (double)p.memoryProperties.memoryHeaps[i].size;
            if (vkc.has_memory_budget) used += (double)b.heapUsage[i];
        }
    g_vram_used = used / 1073741824.0;
    g_vram_total = total / 1073741824.0;
}

static const char *cpu_name() {
    static char name[128];
    if (name[0]) return name;
    snprintf(name, sizeof name, "?");
#ifdef _WIN32
    DWORD n = sizeof name;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                 RRF_RT_REG_SZ, NULL, name, &n);
#else
    size_t n = sizeof name;
    sysctlbyname("machdep.cpu.brand_string", name, &n, NULL, 0);
#endif
    char *p = name;
    while (*p == ' ') p++;
    memmove(name, p, strlen(p) + 1);
    return name;
}

// ---------------- Vulkan objects ----------------
struct Buf { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; };
struct Font { VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorPool pool; VkDescriptorSet set; };
struct Dead { Font font; Buf stage; int ttl; };   // freed once no frame in flight can use it
static struct {
    int state;                                    // 0 not made, 1 ready, -1 failed (no overlay)
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderEXT vs, fs;
    VkSampler sampler;
    Font font;
    Buf stage;                                    // the font's pixels until the copy is recorded
    int font_w, font_h, upload;
    Buf vb[SLOTS], ib[SLOTS];
    Dead dead[8];
} G;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void buf_free(Buf &b) {
    if (b.buf) vk.vkDestroyBuffer(vkc.device, b.buf, NULL);
    if (b.mem) vk.vkFreeMemory(vkc.device, b.mem, NULL);
    b = Buf{};
}
// A host-visible buffer of at least `need` bytes (grown by half again when too small).
static bool buf_fit(Buf &b, VkDeviceSize need, VkBufferUsageFlags usage) {
    if (b.buf && b.size >= need) return true;
    buf_free(b);
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = need + need / 2;
    bi.usage = usage;
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &b.buf), "overlay buffer")) return false;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, b.buf, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &b.mem), "overlay memory") ||
        !vk_check(vk.vkBindBufferMemory(vkc.device, b.buf, b.mem, 0), "overlay bind") ||
        !vk_check(vk.vkMapMemory(vkc.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map), "overlay map")) {
        buf_free(b);
        return false;
    }
    b.size = bi.size;
    return true;
}

static void bury(const Font &f, const Buf &stage) {
    for (Dead &d : G.dead)
        if (!d.ttl) { d.font = f; d.stage = stage; d.ttl = SLOTS + 1; return; }
}
static void dead_tick() {
    for (Dead &d : G.dead) {
        if (!d.ttl || --d.ttl) continue;
        if (d.font.pool) vk.vkDestroyDescriptorPool(vkc.device, d.font.pool, NULL);
        if (d.font.view) vk.vkDestroyImageView(vkc.device, d.font.view, NULL);
        if (d.font.img) vk.vkDestroyImage(vkc.device, d.font.img, NULL);
        if (d.font.mem) vk.vkFreeMemory(vkc.device, d.font.mem, NULL);
        buf_free(d.stage);
    }
}

static bool gpu_init() {
    if (G.state) return G.state > 0;
    G.state = -1;
    VkDescriptorSetLayoutBinding bind = {};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl = {};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings = &bind;
    if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &dl, NULL, &G.dsl), "overlay set layout")) return false;
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};   // scale, translate
    VkPipelineLayoutCreateInfo pl = {};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &G.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &pl, NULL, &G.layout), "overlay layout")) return false;
    VkShaderCreateInfoEXT ci[2] = {};
    for (int i = 0; i < 2; i++) {
        ci[i].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        ci[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        ci[i].nextStage = i ? 0 : VK_SHADER_STAGE_FRAGMENT_BIT;
        ci[i].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci[i].codeSize = i ? sizeof overlay_frag_spv : sizeof overlay_vert_spv;
        ci[i].pCode = i ? (const void *)overlay_frag_spv : (const void *)overlay_vert_spv;
        ci[i].pName = "main";
        ci[i].setLayoutCount = 1;
        ci[i].pSetLayouts = &G.dsl;
        ci[i].pushConstantRangeCount = 1;
        ci[i].pPushConstantRanges = &pc;
    }
    if (!vk_check(vk.vkCreateShadersEXT(vkc.device, 1, &ci[0], NULL, &G.vs), "overlay vertex shader") ||
        !vk_check(vk.vkCreateShadersEXT(vkc.device, 1, &ci[1], NULL, &G.fs), "overlay fragment shader")) return false;
    VkSamplerCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1;
    if (!vk_check(vk.vkCreateSampler(vkc.device, &si, NULL, &G.sampler), "overlay sampler")) return false;
    G.state = 1;
    return true;
}

// The font atlas into a new image (copied at the next draw); the old one is freed once no frame uses it.
static bool font_upload() {
    unsigned char *px;
    int w, h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    Buf stage = {};
    if (!buf_fit(stage, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    memcpy(stage.map, px, (size_t)w * h * 4);
    Font f = {};
    VkImageCreateInfo ic = {};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = {(uint32_t)w, (uint32_t)h, 1};
    ic.mipLevels = ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi = {};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    VkDescriptorSetAllocateInfo sa = {};
    sa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts = &G.dsl;
    bool ok = vk_check(vk.vkCreateImage(vkc.device, &ic, NULL, &f.img), "overlay font image");
    if (ok) {
        vk.vkGetImageMemoryRequirements(vkc.device, f.img, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = ai.memoryTypeIndex != UINT32_MAX && vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &f.mem), "overlay font memory") &&
             vk_check(vk.vkBindImageMemory(vkc.device, f.img, f.mem, 0), "overlay font bind");
    }
    if (ok) {
        vi.image = f.img;
        ok = vk_check(vk.vkCreateImageView(vkc.device, &vi, NULL, &f.view), "overlay font view") &&
             vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &f.pool), "overlay pool");
    }
    if (ok) {
        sa.descriptorPool = f.pool;
        ok = vk_check(vk.vkAllocateDescriptorSets(vkc.device, &sa, &f.set), "overlay set");
    }
    if (!ok) {   // nothing of it is in use yet: freed through the same path
        bury(f, stage);
        return false;
    }
    VkDescriptorImageInfo ii = {G.sampler, f.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = f.set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vk.vkUpdateDescriptorSets(vkc.device, 1, &wr, 0, NULL);
    if (G.font.img || G.stage.buf) bury(G.font, G.stage);
    G.font = f;
    G.stage = stage;
    G.font_w = w;
    G.font_h = h;
    G.upload = 1;
    return true;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                          VkAccessFlags dst, VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

static void font_copy(VkCommandBuffer cb) {
    if (!G.upload) return;
    G.upload = 0;
    image_barrier(cb, G.font.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = {};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {(uint32_t)G.font_w, (uint32_t)G.font_h, 1};
    vk.vkCmdCopyBufferToImage(cb, G.stage.buf, G.font.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cb, G.font.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    bury(Font{}, G.stage);   // the staging buffer: freed once this frame is done
    G.stage = Buf{};
}

// Every state shader objects need set (the same list as the replayer's draws, vkb_draw.c), for ImGui's triangles:
// alpha blended, no depth, no culling.
static void set_state(VkCommandBuffer cb, VkExtent2D ext) {
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    vk.vkCmdSetViewportWithCount(cb, 1, &vp);
    vk.vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    vk.vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vk.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vk.vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vk.vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vk.vkCmdSetPolygonModeEXT(cb, VK_POLYGON_MODE_FILL);
    vk.vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    vk.vkCmdSetLineWidth(cb, 1.0f);
    vk.vkCmdSetDepthClampEnableEXT(cb, VK_FALSE);
    if (vk.vkCmdSetDepthClipEnableEXT) vk.vkCmdSetDepthClipEnableEXT(cb, VK_TRUE);
    if (vkc.has_depth_clip_control && vk.vkCmdSetDepthClipNegativeOneToOneEXT) vk.vkCmdSetDepthClipNegativeOneToOneEXT(cb, VK_FALSE);
    if (vk.vkCmdSetProvokingVertexModeEXT) vk.vkCmdSetProvokingVertexModeEXT(cb, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT);
    vk.vkCmdSetRasterizationSamplesEXT(cb, VK_SAMPLE_COUNT_1_BIT);
    VkSampleMask mask = 0xFFFFFFFF;
    vk.vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &mask);
    vk.vkCmdSetAlphaToCoverageEnableEXT(cb, VK_FALSE);
    vk.vkCmdSetAlphaToOneEnableEXT(cb, VK_FALSE);
    vk.vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vk.vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vk.vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vk.vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vk.vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vk.vkCmdSetLogicOpEnableEXT(cb, VK_FALSE);
    VkBool32 blend = VK_TRUE;
    vk.vkCmdSetColorBlendEnableEXT(cb, 0, 1, &blend);
    VkColorBlendEquationEXT eq = {VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD};
    vk.vkCmdSetColorBlendEquationEXT(cb, 0, 1, &eq);
    VkColorComponentFlags wm = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vk.vkCmdSetColorWriteMaskEXT(cb, 0, 1, &wm);
    if (vkc.has_feedback_loop) vk.vkCmdSetAttachmentFeedbackLoopEnableEXT(cb, 0);
    VkVertexInputBindingDescription2EXT vb = {};
    vb.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
    vb.stride = sizeof(ImDrawVert);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vb.divisor = 1;
    VkVertexInputAttributeDescription2EXT va[3] = {};
    for (int i = 0; i < 3; i++) va[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
    va[0].location = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT; va[0].offset = offsetof(ImDrawVert, pos);
    va[1].location = 1; va[1].format = VK_FORMAT_R32G32_SFLOAT; va[1].offset = offsetof(ImDrawVert, uv);
    va[2].location = 2; va[2].format = VK_FORMAT_R8G8B8A8_UNORM; va[2].offset = offsetof(ImDrawVert, col);
    vk.vkCmdSetVertexInputEXT(cb, 1, &vb, 3, va);
}

static void render(VkCommandBuffer cb, VkImageView view, VkExtent2D ext, unsigned slot) {
    ImDrawData *dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount <= 0 || slot >= SLOTS) return;
    Buf &vb = G.vb[slot], &ib = G.ib[slot];
    if (!buf_fit(vb, dd->TotalVtxCount * sizeof(ImDrawVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !buf_fit(ib, dd->TotalIdxCount * sizeof(ImDrawIdx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return;
    ImDrawVert *vd = (ImDrawVert *)vb.map;
    ImDrawIdx *id = (ImDrawIdx *)ib.map;
    for (const ImDrawList *l : dd->CmdLists) {
        memcpy(vd, l->VtxBuffer.Data, l->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(id, l->IdxBuffer.Data, l->IdxBuffer.Size * sizeof(ImDrawIdx));
        vd += l->VtxBuffer.Size;
        id += l->IdxBuffer.Size;
    }
    VkRenderingAttachmentInfo ca = {};
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vk.vkCmdBeginRendering(cb, &ri);
    set_state(cb, ext);
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    VkShaderEXT objs[5] = {G.vs, G.fs, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    vk.vkCmdBindShadersEXT(cb, 5, stages, objs);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &G.font.set, 0, NULL);
    VkDeviceSize off = 0, stride = sizeof(ImDrawVert);
    vk.vkCmdBindVertexBuffers2(cb, 0, 1, &vb.buf, &off, NULL, &stride);
    vk.vkCmdBindIndexBuffer(cb, ib.buf, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    float pc[4] = {2.0f / dd->DisplaySize.x, 2.0f / dd->DisplaySize.y, 0, 0};
    pc[2] = -1.0f - dd->DisplayPos.x * pc[0];
    pc[3] = -1.0f - dd->DisplayPos.y * pc[1];
    vk.vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    uint32_t vo = 0, io = 0;
    for (const ImDrawList *l : dd->CmdLists) {
        for (const ImDrawCmd &c : l->CmdBuffer) {
            if (c.UserCallback) continue;   // (the overlay sets none)
            float x0 = std::max(c.ClipRect.x - dd->DisplayPos.x, 0.0f), y0 = std::max(c.ClipRect.y - dd->DisplayPos.y, 0.0f);
            float x1 = std::min(c.ClipRect.z - dd->DisplayPos.x, (float)ext.width), y1 = std::min(c.ClipRect.w - dd->DisplayPos.y, (float)ext.height);
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            vk.vkCmdSetScissorWithCount(cb, 1, &sc);
            vk.vkCmdDrawIndexed(cb, c.ElemCount, 1, c.IdxOffset + io, (int32_t)(c.VtxOffset + vo), 0);
        }
        vo += l->VtxBuffer.Size;
        io += l->IdxBuffer.Size;
    }
    vk.vkCmdEndRendering(cb);
}

// ---------------- look ----------------
static const ImVec4 GOLD(0.847f, 0.749f, 0.494f, 1), GOLD_HI(0.953f, 0.886f, 0.663f, 1), ZONAI(0.369f, 0.949f, 0.784f, 1),
                    CREAM(0.925f, 0.902f, 0.839f, 1), MUTED(0.663f, 0.647f, 0.580f, 1), DANGER(1.0f, 0.482f, 0.420f, 1);
static ImVec4 alpha(ImVec4 c, float a) { c.w = a; return c; }
static ImFont *f_body, *f_small, *f_title, *f_big;
static float g_scale;

static bool file_ok(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void build_fonts(float s) {
    ImFontAtlas *a = ImGui::GetIO().Fonts;
    a->Clear();
    const char *sans = "assets/fonts/NunitoSans.ttf", *serif = "assets/fonts/Cinzel.ttf";
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (file_ok(sans) && file_ok(serif)) {
        f_body = a->AddFontFromFileTTF(sans, roundf(17 * s), &cfg);
        f_small = a->AddFontFromFileTTF(sans, roundf(14 * s), &cfg);
        f_big = a->AddFontFromFileTTF(sans, roundf(30 * s), &cfg);
        f_title = a->AddFontFromFileTTF(serif, roundf(22 * s), &cfg);
    } else {   // ImGui's own (ProggyClean), scaled
        ImFontConfig d;
        d.SizePixels = roundf(13 * s); f_body = a->AddFontDefault(&d);
        d.SizePixels = roundf(13 * s); f_small = a->AddFontDefault(&d);
        d.SizePixels = roundf(26 * s); f_big = a->AddFontDefault(&d);
        d.SizePixels = roundf(20 * s); f_title = a->AddFontDefault(&d);
    }
    ImGui::GetIO().FontDefault = f_body;
}

static void build_style(float s) {
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 10; st.ChildRounding = 8; st.FrameRounding = 6; st.PopupRounding = 8;
    st.GrabRounding = 6; st.TabRounding = 6; st.ScrollbarRounding = 6;
    st.WindowBorderSize = 1; st.ChildBorderSize = 1; st.FrameBorderSize = 0; st.TabBorderSize = 0;
    st.WindowPadding = ImVec2(18, 16); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6); st.CellPadding = ImVec2(8, 5);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_Text] = CREAM;
    c[ImGuiCol_TextDisabled] = MUTED;
    c[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.059f, 0.051f, 0.92f);
    c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.065f, 0.058f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.91f, 0.83f, 0.60f, 0.22f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = alpha(ZONAI, 0.12f);
    c[ImGuiCol_FrameBgActive] = alpha(ZONAI, 0.20f);
    c[ImGuiCol_CheckMark] = ZONAI;
    c[ImGuiCol_SliderGrab] = ZONAI;
    c[ImGuiCol_SliderGrabActive] = GOLD_HI;
    c[ImGuiCol_Button] = alpha(GOLD, 0.14f);
    c[ImGuiCol_ButtonHovered] = alpha(GOLD, 0.28f);
    c[ImGuiCol_ButtonActive] = alpha(GOLD, 0.40f);
    c[ImGuiCol_Header] = alpha(GOLD, 0.16f);
    c[ImGuiCol_HeaderHovered] = alpha(GOLD, 0.28f);
    c[ImGuiCol_HeaderActive] = alpha(GOLD, 0.40f);
    c[ImGuiCol_Separator] = ImVec4(0.91f, 0.83f, 0.60f, 0.18f);
    c[ImGuiCol_Tab] = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_TabHovered] = alpha(GOLD, 0.26f);
    c[ImGuiCol_TabSelected] = alpha(GOLD, 0.18f);
    c[ImGuiCol_TabSelectedOverline] = GOLD;
    c[ImGuiCol_PlotLines] = ZONAI;
    c[ImGuiCol_PlotHistogram] = ZONAI;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.91f, 0.83f, 0.60f, 0.10f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    st.ScaleAllSizes(s);
}

// ---------------- widgets ----------------
static ImVec4 rate_colour(double fps) {   // against the frame rate the game aims at (30, or 60 with the mod)
    double target = LD(g_fps60) ? 60 : 30;
    return fps >= target * 0.95 ? ZONAI : fps >= target * 0.75 ? GOLD : DANGER;
}

static void frame_stats(float *avg, float *worst, float *low1) {   // over the frames in the graph
    *avg = *worst = *low1 = 0;
    if (!g_ft_n) return;
    float v[HIST];
    for (int i = 0; i < g_ft_n; i++) v[i] = g_ft[(g_ft_at - g_ft_n + i + HIST) % HIST];
    double sum = 0;
    for (int i = 0; i < g_ft_n; i++) { sum += v[i]; *worst = std::max(*worst, v[i]); }
    *avg = (float)(sum / g_ft_n);
    int k = std::min(g_ft_n - 1, (int)(g_ft_n * 0.99f));   // 1% low: the 99th percentile frame time as a rate
    std::nth_element(v, v + k, v + g_ft_n);
    *low1 = v[k] > 0 ? 1000.0f / v[k] : 0;
}

// The frame times, newest on the right, with the 60 and 30 FPS lines; spikes over twice the target in red.
static void frame_graph(ImVec2 size, float s) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::Dummy(size);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.30f)), 6 * s);
    float top = 50;
    for (int i = 0; i < g_ft_n; i++) top = std::max(top, g_ft[i]);
    top = std::min(ceilf(top / 25) * 25, 250.0f);
    float pad = 4 * s;
    auto y_of = [&](float ms) { return p1.y - pad - std::min(ms / top, 1.0f) * (size.y - 2 * pad); };
    const struct { float ms; const char *label; } refs[] = {{1000.0f / 60, "60"}, {1000.0f / 30, "30"}};
    for (const auto &r : refs) {
        float y = y_of(r.ms);
        dl->AddLine(ImVec2(p0.x + pad, y), ImVec2(p1.x - pad, y), ImGui::GetColorU32(alpha(GOLD, 0.30f)), 1);
        dl->AddText(f_small, f_small->FontSize, ImVec2(p1.x - pad - ImGui::CalcTextSize(r.label).x * 0.8f - 4 * s, y - f_small->FontSize),
                    ImGui::GetColorU32(alpha(GOLD, 0.6f)), r.label);
    }
    if (g_ft_n < 2) return;
    ImVec2 pts[HIST];
    float step = (size.x - 2 * pad) / (HIST - 1), target = LD(g_fps60) ? 1000.0f / 60 : 1000.0f / 30;
    for (int i = 0; i < g_ft_n; i++) {
        float ms = g_ft[(g_ft_at - g_ft_n + i + HIST) % HIST];
        pts[i] = ImVec2(p1.x - pad - (g_ft_n - 1 - i) * step, y_of(ms));
        if (ms > 2 * target) dl->AddLine(ImVec2(pts[i].x, p1.y - pad), pts[i], ImGui::GetColorU32(alpha(DANGER, 0.85f)), 2 * s);
    }
    dl->AddPolyline(pts, g_ft_n, ImGui::GetColorU32(ZONAI), 0, 1.6f * s);
}

// A bar with its value: green, gold past 75 %, red past 90 %.
static void bar(float frac, const char *text, float width, float s) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = 8 * s;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.y += (ImGui::GetTextLineHeight() - h) / 2;
    ImVec2 p1(p0.x + width, p0.y + h);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f)), h / 2);
    if (frac > 0) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(width * frac, h), p1.y),
                                    ImGui::GetColorU32(frac > 0.9f ? DANGER : frac > 0.75f ? GOLD : ZONAI), h / 2);
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
}

static void card(const char *id, const char *label, const char *value, ImVec4 colour, const char *sub, float w, float s) {
    ImGui::BeginChild(id, ImVec2(w, 96 * s), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(f_small);
    ImGui::TextColored(MUTED, "%s", label);
    ImGui::PopFont();
    ImGui::PushFont(f_big);
    ImGui::TextColored(colour, "%s", value);
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    ImGui::TextColored(MUTED, "%s", sub);
    ImGui::PopFont();
    ImGui::EndChild();
}


// ---------------- language: TOTK_LANG from the launcher (pt / en), else the system's ----------------
static bool g_en;
static void lang_init() {
    const char *l = getenv("TOTK_LANG");
    if (l && *l) { g_en = strcmp(l, "pt") != 0; return; }
#ifdef _WIN32
    g_en = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_PORTUGUESE;
#else
    const char *e = getenv("LANG");
    g_en = !(e && !strncmp(e, "pt", 2));
#endif
}
#define L(pt, en) (g_en ? (en) : (pt))

extern "C" {
int vkb_shader_compiling(void);                                              // gfx/vkb_shader.c
void vkb_shader_async_stats(uint64_t *made, uint64_t *loaded, uint64_t *missing);
int vkb_prewarm_progress(uint64_t *done, uint64_t *total);
}

static double g_time;          // seconds, present thread clock
static float ease(float x) { x = std::clamp(x, 0.0f, 1.0f); return 1 - (1 - x) * (1 - x) * (1 - x); }
static ImU32 rgba(ImVec4 c, float a) { c.w = a; return ImGui::GetColorU32(c); }

// ---------------- the counter ----------------
static void draw_counter(VkRect2D pic, float s) {
    int corner = LD(g_corner);
    float pad = 14 * s;
    ImVec2 pos(corner & 1 ? pic.offset.x + pic.extent.width - pad : pic.offset.x + pad,
               corner & 2 ? pic.offset.y + pic.extent.height - pad : pic.offset.y + pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(corner & 1 ? 1.0f : 0.0f, corner & 2 ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * s);
    ImGui::Begin("##counter", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    float avg, worst, low1;
    frame_stats(&avg, &worst, &low1);
    ImGui::PushFont(f_big);
    ImGui::TextColored(rate_colour(S.fps), "%.0f", S.fps);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(f_small);
    ImGui::TextColored(MUTED, "FPS");
    ImGui::TextColored(MUTED, "%.1f ms", S.fps > 0 ? 1000.0 / S.fps : 0.0);
    ImGui::PopFont();
    ImGui::EndGroup();
    if (LD(g_counter) == OVL_COUNTER_FULL) {
        ImGui::PushFont(f_small);
        frame_graph(ImVec2(190 * s, 44 * s), s);
        if (ImGui::BeginTable("##figures", 2, ImGuiTableFlags_SizingFixedFit)) {
            char t[64];
            auto row = [&t](const char *k) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(GOLD, "%s", k);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t);
            };
            snprintf(t, sizeof t, "%.0f FPS", low1); row("1% low");
            snprintf(t, sizeof t, L("%.0f%%  (sistema %.0f%%)", "%.0f%%  (system %.0f%%)"), S.cpu_proc, S.cpu_sys); row("CPU");
            snprintf(t, sizeof t, "%.0f%%  (%.1f ms)", S.gpu_load * 100, S.gpu_ms); row("GPU");
            if (g_vram_used > 0) { snprintf(t, sizeof t, "%.1f / %.1f GB", g_vram_used, g_vram_total); row("VRAM"); }
            snprintf(t, sizeof t, "%.1f GB", S.ram_proc_gb); row("RAM");
            ImGui::EndTable();
        }
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---------------- the shader notice (bottom right, while shaders are made in the background) ----------------
static double g_toast_until;
static float g_toast_a;
static int g_toast_live;           // drawn or fading: present.c keeps drawing the overlay (ovl_visible)

static void spinner(ImVec2 c, float r, float s) {   // a diamond turning a quarter, then resting (the launcher's)
    float ph = (float)fmod(g_time, 1.2) / 1.2f, a = 0.7853982f + 1.5707963f * ease(std::min(ph / 0.6f, 1.0f));
    ImVec2 p[4];
    for (int i = 0; i < 4; i++) p[i] = ImVec2(c.x + r * cosf(a + i * 1.5707963f), c.y + r * sinf(a + i * 1.5707963f));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddPolyline(p, 4, rgba(ZONAI, 0.25f), ImDrawFlags_Closed, 5 * s);
    dl->AddPolyline(p, 4, rgba(ZONAI, 1), ImDrawFlags_Closed, 2 * s);
}

static void draw_toast(VkRect2D pic, float s, float dt) {
    int n = vkb_shader_compiling();
    if (n > 0) g_toast_until = g_time + 1.2;
    bool on = LD(g_toast) && g_time < g_toast_until;
    g_toast_a = on ? std::min(1.0f, g_toast_a + dt / 0.25f) : std::max(0.0f, g_toast_a - dt / 0.4f);
    ST(g_toast_live, g_toast_a > 0 || on);
    if (g_toast_a <= 0) return;
    float k = ease(g_toast_a), pad = 24 * s;
    ImGui::SetNextWindowPos(ImVec2(pic.offset.x + pic.extent.width - pad, pic.offset.y + pic.extent.height - pad + (1 - k) * 12 * s),
                            ImGuiCond_Always, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 11 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.059f, 0.051f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border, alpha(ZONAI, 0.35f));
    ImGui::Begin("##toast", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float box = 28 * s;
    ImGui::Dummy(ImVec2(box, box));
    spinner(ImVec2(p.x + box / 2, p.y + box / 2), 8 * s, s);
    ImGui::SameLine(0, 12 * s);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(n > 0 ? L("Compilando shaders…", "Compiling shaders…") : L("Shaders prontos", "Shaders ready"));
    ImGui::PushFont(f_small);
    if (n > 0) ImGui::TextColored(MUTED, L("%d na fila  ·  o jogo segue rodando", "%d queued  ·  the game keeps running"), n);
    else ImGui::TextColored(MUTED, "%s", L("na próxima vez já vêm prontos", "ready from the start next time"));
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// ---------------- the panel (F1): from the left edge, top to bottom; a rail of tabs, the content beside it ----------------
enum { TAB_PERF, TAB_SHADERS, TAB_OPTIONS, TAB_SYSTEM, TABS };
static int g_tab;
static float g_open;               // 0 closed .. 1 open, animated
static int g_anim;                 // opening or closing: keep drawing (ovl_visible)
static double g_tab_t;             // when the tab showed: its sections come in one after another
static const char *tab_name(int t) {
    switch (t) {
    case TAB_PERF: return L("Desempenho", "Performance");
    case TAB_SHADERS: return "Shaders";
    case TAB_OPTIONS: return L("Opções", "Options");
    default: return L("Sistema", "System");
    }
}

static void stagger(int i, float s) {   // before section i: its fade and rise (pair with ImGui::PopStyleVar)
    float t = ease((float)((g_time - g_tab_t - 0.06 - i * 0.07) / 0.35));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * t);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1 - t) * 10 * s);
}

static void tab_icon(int which, ImVec2 c, float r, ImU32 col, float th) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    switch (which) {
    case TAB_PERF: {
        ImVec2 p[] = {{c.x - r, c.y + r * 0.5f}, {c.x - r * 0.35f, c.y - r * 0.15f}, {c.x + r * 0.05f, c.y + r * 0.2f},
                      {c.x + r * 0.55f, c.y - r * 0.6f}, {c.x + r, c.y - r * 0.15f}};
        dl->AddPolyline(p, 5, col, 0, th);
        break;
    }
    case TAB_SHADERS: {   // a cube
        ImVec2 p[6];
        for (int i = 0; i < 6; i++) p[i] = ImVec2(c.x + r * sinf(i * 1.0471976f), c.y - r * cosf(i * 1.0471976f));
        dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, th);
        dl->AddLine(c, p[0], col, th); dl->AddLine(c, p[2], col, th); dl->AddLine(c, p[4], col, th);
        break;
    }
    case TAB_OPTIONS:     // two sliders
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.45f), ImVec2(c.x + r, c.y - r * 0.45f), col, th);
        dl->AddLine(ImVec2(c.x - r, c.y + r * 0.45f), ImVec2(c.x + r, c.y + r * 0.45f), col, th);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.35f, c.y - r * 0.45f), r * 0.28f, col);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y + r * 0.45f), r * 0.28f, col);
        break;
    case TAB_SYSTEM:      // a chip
        dl->AddRect(ImVec2(c.x - r * 0.6f, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), col, r * 0.15f, 0, th);
        for (int i = -1; i <= 1; i += 2) {
            float o = i * r * 0.3f;
            dl->AddLine(ImVec2(c.x + o, c.y - r * 0.6f), ImVec2(c.x + o, c.y - r), col, th);
            dl->AddLine(ImVec2(c.x + o, c.y + r * 0.6f), ImVec2(c.x + o, c.y + r), col, th);
            dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + o), ImVec2(c.x - r, c.y + o), col, th);
            dl->AddLine(ImVec2(c.x + r * 0.6f, c.y + o), ImVec2(c.x + r, c.y + o), col, th);
        }
        break;
    default:              // close
        dl->AddLine(ImVec2(c.x - r * 0.6f, c.y - r * 0.6f), ImVec2(c.x + r * 0.6f, c.y + r * 0.6f), col, th);
        dl->AddLine(ImVec2(c.x + r * 0.6f, c.y - r * 0.6f), ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), col, th);
    }
}

static bool rail_button(int which, bool active, float size, float s, const char *tip) {
    ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + size, p.y + size), c(p.x + size / 2, p.y + size / 2);
    ImGui::PushID(which);
    bool clicked = ImGui::InvisibleButton("##tab", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 12 * s;
    if (active) {
        float pulse = 0.75f + 0.25f * sinf((float)g_time * 2.4f);
        for (int k = 1; k <= 4; k++)   // the glow: rings fading out
            dl->AddRect(ImVec2(p.x - k * 2 * s, p.y - k * 2 * s), ImVec2(q.x + k * 2 * s, q.y + k * 2 * s), rgba(ZONAI, (0.11f - k * 0.025f) * pulse),
                        r + k * 2 * s, 0, 2 * s);
        dl->AddRectFilled(p, q, rgba(ZONAI, 0.12f), r);
        dl->AddRect(p, q, rgba(ZONAI, 0.55f), r, 0, 1.2f * s);
    } else if (hov) dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)), r);
    tab_icon(which, c, 11 * s, ImGui::GetColorU32(active ? ZONAI : hov ? CREAM : MUTED), 1.8f * s);
    if (hov) ImGui::SetTooltip("%s", tip);
    return clicked;
}

static void hint(const char *text) {
    ImGui::PushFont(f_small);
    ImGui::TextColored(MUTED, "%s", text);
    ImGui::PopFont();
}

static void tab_performance(float s) {
    float avg, worst, low1;
    frame_stats(&avg, &worst, &low1);
    float gap = ImGui::GetStyle().ItemSpacing.x, w = (ImGui::GetContentRegionAvail().x - gap) / 2;
    char v[32], sub[64];
    stagger(1, s);
    snprintf(v, sizeof v, "%.0f", S.fps);
    snprintf(sub, sizeof sub, "1%% low %.0f", low1);
    card("##c_fps", "FPS", v, rate_colour(S.fps), sub, w, s);
    ImGui::SameLine();
    snprintf(v, sizeof v, "%.1f ms", avg);
    snprintf(sub, sizeof sub, L("pior %.0f ms", "worst %.0f ms"), worst);
    card("##c_ft", L("TEMPO DE FRAME", "FRAME TIME"), v, CREAM, sub, w, s);
    snprintf(v, sizeof v, "%.0f%%", S.gpu_load * 100);
    snprintf(sub, sizeof sub, L("%.1f ms por frame", "%.1f ms per frame"), S.gpu_ms);
    card("##c_gpu", "GPU", v, S.gpu_load > 0.9 ? DANGER : CREAM, sub, w, s);
    ImGui::SameLine();
    snprintf(v, sizeof v, "%.0f%%", S.cpu_proc);
    snprintf(sub, sizeof sub, L("sistema %.0f%%  ·  %d núcleos", "system %.0f%%  ·  %d cores"), S.cpu_sys, S.cpu_cores);
    card("##c_cpu", L("CPU (JOGO)", "CPU (GAME)"), v, CREAM, sub, w, s);
    ImGui::PopStyleVar();

    ImGui::Spacing();
    stagger(2, s);
    ImGui::PushFont(f_small);
    ImGui::TextColored(MUTED, L("TEMPO DE FRAME  ·  ÚLTIMOS %d FRAMES", "FRAME TIME  ·  LAST %d FRAMES"), HIST);
    ImGui::PopFont();
    frame_graph(ImVec2(ImGui::GetContentRegionAvail().x, 120 * s), s);
    ImGui::PopStyleVar();

    ImGui::Spacing();
    stagger(3, s);
    if (ImGui::BeginTable("##use", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 110 * s);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, 110 * s);
        auto row = [&](const char *k, float frac, const char *text) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(GOLD, "%s", k);
            ImGui::TableNextColumn();
            bar(frac, "", ImGui::GetContentRegionAvail().x, s);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };
        char t[64];
        snprintf(t, sizeof t, "%.0f%%", S.cpu_proc);
        row(L("CPU (jogo)", "CPU (game)"), (float)S.cpu_proc / 100, t);
        snprintf(t, sizeof t, "%.0f%%", S.gpu_load * 100);
        row("GPU", (float)S.gpu_load, t);
        if (g_vram_used > 0) {
            snprintf(t, sizeof t, "%.1f / %.1f GB", g_vram_used, g_vram_total);
            row("VRAM", (float)(g_vram_used / g_vram_total), t);
        }
        if (S.ram_total_gb > 0) {
            snprintf(t, sizeof t, "%.1f / %.0f GB", S.ram_used_gb, S.ram_total_gb);
            row(L("RAM (sistema)", "RAM (system)"), (float)(S.ram_used_gb / S.ram_total_gb), t);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

static void tab_shaders(float s) {
    uint64_t made, loaded, missing, done, total;
    vkb_shader_async_stats(&made, &loaded, &missing);
    int left = vkb_prewarm_progress(&done, &total), n = vkb_shader_compiling();
    float w = ImGui::GetContentRegionAvail().x;
    stagger(1, s);
    ImGui::BeginChild("##sh_now", ImVec2(w, 84 * s), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float box = 40 * s;
    ImGui::Dummy(ImVec2(box, box));
    if (n > 0) spinner(ImVec2(p.x + box / 2, p.y + box / 2), 12 * s, s);
    else tab_icon(TAB_SHADERS, ImVec2(p.x + box / 2, p.y + box / 2), 13 * s, ImGui::GetColorU32(ZONAI), 1.8f * s);
    ImGui::SameLine(0, 14 * s);
    ImGui::BeginGroup();
    ImGui::PushFont(f_big);
    if (n > 0) ImGui::TextColored(ZONAI, L("%d na fila", "%d queued"), n);
    else ImGui::TextColored(CREAM, "%s", L("Tudo pronto", "All ready"));
    ImGui::PopFont();
    hint(n > 0 ? L("compilando em segundo plano", "compiling in the background") : L("nenhum shader sendo compilado", "no shader being compiled"));
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Spacing();
    stagger(2, s);
    if (ImGui::BeginTable("##sh", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char *k, const char *fmt, unsigned long long v, unsigned long long v2) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(GOLD, "%s", k);
            ImGui::TableNextColumn();
            ImGui::Text(fmt, v, v2);
        };
        row(L("Prontos nesta sessão", "Ready this session"), "%llu%.0llu", (unsigned long long)loaded, 0ull);
        row(L("Feitos em segundo plano", "Made in the background"), "%llu%.0llu", (unsigned long long)made, 0ull);
        if (total && left >= 0) row(L("Pré-compilados na abertura", "Made ahead at start"), "%llu / %llu", (unsigned long long)done, (unsigned long long)total);
        if (missing) row(L("Sem tradução", "Without translation"), "%llu%.0llu", (unsigned long long)missing, 0ull);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    stagger(3, s);
    ImGui::PushTextWrapPos(0);
    hint(L("Shaders novos são compilados em segundo plano: um objeto pode aparecer um instante depois, mas o jogo não "
           "trava. Todos ficam anotados e, na próxima vez que você abrir o jogo, já vêm prontos.",
           "New shaders are compiled in the background: an object may show up a moment later, but the game does not "
           "freeze. They are all noted down and come ready the next time you start the game."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
}

static void tab_options(float s) {
    stagger(1, s);
    bool fps60 = LD(g_fps60);
    if (ImGui::Checkbox(L("Mod de 60 FPS  (F2)", "60 FPS mod  (F2)"), &fps60)) {
        ST(g_fps60, (int)fps60);   // shown at once; the main thread switches it
        ASK(OVL_ACT_FPS60);
    }
    hint(L("Mais fluido; exige mais da CPU e da GPU.", "Smoother; asks more of the CPU and GPU."));
    ImGui::Spacing();
    float combo_w = 230 * s;
    int counter = LD(g_counter);
    ImGui::SetNextItemWidth(combo_w);
    if (ImGui::Combo(L("Contador  (F3)", "Counter  (F3)"), &counter, L("Desligado\0Só FPS\0Detalhado\0", "Off\0FPS only\0Detailed\0"))) {
        ST(g_counter, counter);
        settings_save();
    }
    int corner = LD(g_corner);
    ImGui::SetNextItemWidth(combo_w);
    if (ImGui::Combo(L("Posição do contador", "Counter position"), &corner,
                     L("Superior esquerdo\0Superior direito\0Inferior esquerdo\0Inferior direito\0", "Top left\0Top right\0Bottom left\0Bottom right\0"))) {
        ST(g_corner, corner);
        settings_save();
    }
    bool toast = LD(g_toast);
    if (ImGui::Checkbox(L("Aviso de shaders compilando", "Notice while shaders compile"), &toast)) {
        ST(g_toast, (int)toast);
        settings_save();
    }
    ImGui::Spacing();
    bool full = LD(g_fullscreen);
    if (ImGui::Checkbox(L("Tela cheia  (F11)", "Fullscreen  (F11)"), &full)) {
        ST(g_fullscreen, (int)full);
        ASK(OVL_ACT_FULLSCREEN);
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    stagger(2, s);
    ImGui::PushTextWrapPos(0);
    hint(L("F8 solta ou prende o mouse da câmera. Controles e sensibilidade ficam no launcher.",
           "F8 frees or captures the camera mouse. Controls and sensitivity are in the launcher."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
}

static void tab_system(float s) {
    stagger(1, s);
    if (ImGui::BeginTable("##sys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char *k, const char *fmt, ...) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(GOLD, "%s", k);
            ImGui::TableNextColumn();
            va_list ap;
            va_start(ap, fmt);
            ImGui::TextV(fmt, ap);
            va_end(ap);
        };
        row(L("Placa de vídeo", "Graphics card"), "%s", vkc.props.deviceName);
        row("Vulkan", "%u.%u.%u", VK_API_VERSION_MAJOR(vkc.props.apiVersion), VK_API_VERSION_MINOR(vkc.props.apiVersion),
            VK_API_VERSION_PATCH(vkc.props.apiVersion));
        if (g_vram_total > 0) row(L("Memória de vídeo", "Video memory"), "%.1f GB", g_vram_total);
        row(L("Processador", "Processor"), "%s", cpu_name());
        row(L("Núcleos lógicos", "Logical cores"), "%d", S.cpu_cores);
        if (S.ram_total_gb > 0) row(L("Memória RAM", "RAM"), "%.1f GB", S.ram_total_gb);
        ImGuiIO &io = ImGui::GetIO();
        row(L("Janela", "Window"), "%.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
        if (totk_render_size[0] > 0) row(L("Resolução da cena", "Scene resolution"), "%.0f x %.0f", totk_render_size[0], totk_render_size[1]);
        else row(L("Resolução da cena", "Scene resolution"), "%s", L("1600 x 900 (a do jogo)", "1600 x 900 (the game's)"));
        row("UltraTears", "%s (%s)", ULTRS_VERSION, ULTRS_COMMIT);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

static void draw_panel(float s) {
    ImGuiIO &io = ImGui::GetIO();
    float k = ease(g_open), H = io.DisplaySize.y;
    float w = std::min(600 * s, io.DisplaySize.x * 0.8f), x = -w * (1 - k), rail = 76 * s;
    ImDrawList *bg = ImGui::GetBackgroundDrawList();   // the game dimmed, darker on the panel's side; the panel's shadow
    ImU32 d0 = ImGui::GetColorU32(ImVec4(0.016f, 0.031f, 0.027f, 0.72f * k)), d1 = ImGui::GetColorU32(ImVec4(0.016f, 0.031f, 0.027f, 0.25f * k));
    bg->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize, d0, d1, d1, d0);
    ImU32 sh0 = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.5f * k)), sh1 = ImGui::GetColorU32(ImVec4(0, 0, 0, 0));
    bg->AddRectFilledMultiColor(ImVec2(x + w, 0), ImVec2(x + w + 60 * s, H), sh0, sh1, sh1, sh0);

    ImGui::SetNextWindowPos(ImVec2(x, 0));
    ImGui::SetNextWindowSize(ImVec2(w, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.059f, 0.051f, 0.95f));
    ImGui::Begin("##panel", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x + w - 0.5f, 0), ImVec2(x + w - 0.5f, H), rgba(GOLD, 0.25f), 1);
    dl->AddRectFilled(ImVec2(x, 0), ImVec2(x + rail, H), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.02f)));
    dl->AddLine(ImVec2(x + rail, 0), ImVec2(x + rail, H), rgba(GOLD, 0.10f), 1);

    // the rail: the mark, the tabs, close at the bottom
    {
        ImVec2 c(x + rail / 2, 40 * s);
        float r = 13 * s, glow = 0.6f + 0.4f * sinf((float)g_time * 1.6f);
        ImVec2 q[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
        dl->AddPolyline(q, 4, rgba(ZONAI, 0.18f * glow), ImDrawFlags_Closed, 7 * s);
        dl->AddPolyline(q, 4, rgba(GOLD, 1), ImDrawFlags_Closed, 1.4f * s);
        float bs = 52 * s;
        ImGui::SetCursorPosY(76 * s);
        for (int t = 0; t < TABS; t++) {
            ImGui::SetCursorPosX((rail - bs) / 2);
            if (rail_button(t, g_tab == t, bs, s, tab_name(t)) && g_tab != t) { g_tab = t; g_tab_t = g_time; }
            ImGui::Dummy(ImVec2(1, 4 * s));
        }
        float cs = 44 * s;
        ImGui::SetCursorPos(ImVec2((rail - cs) / 2, H - 22 * s - cs));
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##close", ImVec2(cs, cs))) { ST(g_panel, 0); ASK(OVL_ACT_CLOSE); }
        bool hov = ImGui::IsItemHovered();
        dl->AddRect(p, ImVec2(p.x + cs, p.y + cs), rgba(GOLD, hov ? 0.5f : 0.22f), 10 * s, 0, 1);
        tab_icon(-1, ImVec2(p.x + cs / 2, p.y + cs / 2), 9 * s, ImGui::GetColorU32(hov ? CREAM : MUTED), 2 * s);
        if (hov) ImGui::SetTooltip("%s", L("Fechar (F1 / Esc)", "Close (F1 / Esc)"));
    }

    // the content
    float px = 30 * s, top = 26 * s, foot = f_small->FontSize + 22 * s;
    ImGui::SetCursorPos(ImVec2(rail + px, top));
    ImGui::BeginChild("##content", ImVec2(w - rail - 2 * px, H - top - foot - 10 * s), 0, ImGuiWindowFlags_NoBackground);
    stagger(0, s);
    ImGui::PushFont(f_title);
    ImGui::TextColored(GOLD_HI, "ULTRATEARS");
    ImGui::PopFont();
    if (g_tab == TAB_PERF) {
        char goal[32];
        snprintf(goal, sizeof goal, L("alvo %d FPS", "target %d FPS"), LD(g_fps60) ? 60 : 30);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(goal).x * 0.85f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (f_title->FontSize - f_small->FontSize) * 0.5f);
        hint(goal);
    }
    hint(tab_name(g_tab));
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(1, 6 * s));
    switch (g_tab) {
    case TAB_PERF: tab_performance(s); break;
    case TAB_SHADERS: tab_shaders(s); break;
    case TAB_OPTIONS: tab_options(s); break;
    default: tab_system(s);
    }
    ImGui::EndChild();
    float fy = H - foot;
    dl->AddLine(ImVec2(x + rail + px, fy), ImVec2(x + w - px, fy), rgba(GOLD, 0.12f), 1);
    ImGui::SetCursorPos(ImVec2(rail + px, fy - ImGui::GetWindowPos().y + 9 * s));
    hint(L("F1 ou Esc fecha  ·  F2 60 FPS  ·  F3 contador  ·  F11 tela cheia", "F1 or Esc closes  ·  F2 60 FPS  ·  F3 counter  ·  F11 fullscreen"));
    ImGui::End();
}

int ovl_repaint(void) { return LD(g_panel) || LD(g_anim); }

int ovl_visible(void) {
    return LD(g_panel) || LD(g_anim) || LD(g_counter) != OVL_COUNTER_OFF || LD(g_toast_live) || (LD(g_toast) && vkb_shader_compiling() > 0);
}

// ---------------- the frame ----------------
void ovl_draw(VkCommandBuffer cb, VkImageView view, VkExtent2D ext, VkRect2D pic, unsigned slot) {
    if (!view || !ext.width || !ext.height || !gpu_init()) return;
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;
        io.BackendRendererName = "ultratears-vk";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        lang_init();
    }
    ImGuiIO &io = ImGui::GetIO();
    dead_tick();
    float s = std::clamp(roundf(ext.height / 1080.0f * 8) / 8, 0.75f, 2.5f);   // in eighths: few rebuilds while resizing
    if (s != g_scale) {
        build_fonts(s);
        build_style(s);
        if (!font_upload()) { G.state = -1; return; }
        g_scale = s;
    }
    static uint64_t last;
    uint64_t now = runtime_now_ns();
    float dt = last ? std::min(std::max((now - last) / 1e9f, 1e-4f), 0.1f) : 1.0f / 60;
    last = now;
    g_time += dt;
    io.DeltaTime = dt;
    io.DisplaySize = ImVec2((float)ext.width, (float)ext.height);

    pthread_mutex_lock(&g_mu);   // the main thread's input and figures
    bool panel = LD(g_panel);
    for (int i = 0; i < g_nin; i++) {
        if (!panel) break;
        if (g_in[i].type == IN_POS) io.AddMousePosEvent(g_in[i].a, g_in[i].b);
        else if (g_in[i].type == IN_BUTTON && g_in[i].a >= 0 && g_in[i].a < 3) io.AddMouseButtonEvent((int)g_in[i].a, g_in[i].b != 0);
        else if (g_in[i].type == IN_WHEEL) io.AddMouseWheelEvent(0, g_in[i].b);
    }
    g_nin = 0;
    static uint64_t seen;
    bool fresh = g_stats_seq != seen;
    seen = g_stats_seq;
    S = g_stats;
    pthread_mutex_unlock(&g_mu);
    if (!panel) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    if (fresh) {   // twice a second
        g_cpu_hist[g_load_n % LOADS] = (float)S.cpu_proc;
        g_gpu_hist[g_load_n % LOADS] = (float)(S.gpu_load * 100);
        g_load_n++;
        vram_read();
    }
    if (panel && g_open == 0) g_tab_t = g_time;   // opening: the sections come in again
    g_open = panel ? std::min(1.0f, g_open + dt / 0.32f) : std::max(0.0f, g_open - dt / 0.22f);
    ST(g_anim, g_open > 0 && g_open < 1);

    ImGui::NewFrame();
    if (g_open > 0) draw_panel(s);
    else if (LD(g_counter) != OVL_COUNTER_OFF) draw_counter(pic, s);
    draw_toast(pic, s, dt);
    ImGui::Render();
    font_copy(cb);
    render(cb, view, ext, slot);
}
