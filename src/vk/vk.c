// The shared Vulkan instance and device (see vk.h).
#include "vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))

VkFunctions vk;
VkContext vkc;
static pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_queue_lock(void) { pthread_mutex_lock(&queue_mu); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_mu); }

int vk_check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    LOG("%s failed (%d)", what, (int)r);
    return 0;
}

void vk_fault_report(void) {
    static int done;
    if (!vkc.has_fault || !vk.vkGetDeviceFaultInfoEXT || done++) return;
    VkDeviceFaultCountsEXT n = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (vk.vkGetDeviceFaultInfoEXT(vkc.device, &n, NULL) != VK_SUCCESS) { LOG("device fault: no information"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (n.addressInfoCount > 16) n.addressInfoCount = 16;
    if (n.vendorInfoCount > 16) n.vendorInfoCount = 16;
    n.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    VkResult r = vk.vkGetDeviceFaultInfoEXT(vkc.device, &n, &info);
    LOG("device fault (%d): %s; %u addresses, %u vendor records", (int)r, info.description, n.addressInfoCount, n.vendorInfoCount);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown",
                                       "instruction pointer invalid", "instruction pointer fault"};
    for (uint32_t i = 0; i < n.addressInfoCount; i++)
        LOG("  %s at %#llx (+/- %#llx)", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < n.vendorInfoCount; i++)
        LOG("  vendor: %s (code %#llx, data %#llx)", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    vk.vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *p = calloc(n ? n : 1, sizeof *p);
    vk.vkEnumerateInstanceLayerProperties(&n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) found = 1;
    free(p);
    return found;
}

static int create_instance(SDL_Window *window) {
    PFN_vkGetInstanceProcAddr gipa = NULL;
    if (window) gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    else {   // no window (TOTK_HEADLESS): SDL's video subsystem is not up, so the loader is loaded here
#ifdef _WIN32
        HMODULE m = LoadLibraryA("vulkan-1.dll");
        if (m) gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
#else   // (fork: macOS) the Vulkan loader, with MoltenVK as its driver
        void *m = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!m) m = dlopen("/opt/homebrew/lib/libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);   // (Homebrew: not on the search path)
        if (m) gipa = (PFN_vkGetInstanceProcAddr)dlsym(m, "vkGetInstanceProcAddr");
#endif
    }
    if (!gipa) { LOG("no Vulkan loader (vulkan-1.dll)"); return 0; }
#define LOAD(f) vk.f = (PFN_##f)gipa(NULL, #f);
    VK_FN_GLOBAL(LOAD)
#undef LOAD
    Uint32 next = 0;
    const char *const *exts = window ? SDL_Vulkan_GetInstanceExtensions(&next) : NULL;
    // TOTK_VK_VALIDATION=1: the Khronos validation layer (Vulkan SDK), for development
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *v = getenv("TOTK_VK_VALIDATION");
    int validate = v && strcmp(v, "0") && has_layer(layer);
    if (v && strcmp(v, "0") && !validate) LOG("TOTK_VK_VALIDATION: %s not installed", layer);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "UltraTears",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts,
                               .enabledLayerCount = validate ? 1u : 0u, .ppEnabledLayerNames = &layer};
#ifdef __APPLE__
    // (fork) MoltenVK has no VK_EXT_shader_object: Khronos' shader object layer provides it over pipelines.
    // tools/setup.py writes its manifest (with the library's full path) to deps/vulkan/explicit_layer.d.
    const char *layers[2];
    uint32_t nl = 0;
    if (validate) layers[nl++] = layer;
    char lpath[1200];
    if (!getenv("VK_ADD_LAYER_PATH") && realpath("deps/vulkan/explicit_layer.d", lpath)) setenv("VK_ADD_LAYER_PATH", lpath, 1);
    const char *so_layer = "VK_LAYER_KHRONOS_shader_object";
    if (has_layer(so_layer)) layers[nl++] = so_layer;
    else LOG("%s not found (deps/vulkan/explicit_layer.d; brew install vulkan-extensionlayer): no shader objects", so_layer);
    ci.enabledLayerCount = nl;
    ci.ppEnabledLayerNames = layers;
#endif
#ifdef __APPLE__
    // (fork) MoltenVK is a "portability" driver: the loader lists it only when the instance asks for those
    const char *with_portability[64];
    uint32_t np = 0;
    for (uint32_t i = 0; i < next && np < 62; i++) with_portability[np++] = exts[i];
    with_portability[np++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    with_portability[np++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    ci.enabledExtensionCount = np;
    ci.ppEnabledExtensionNames = with_portability;
    ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    if (!vk_check(vk.vkCreateInstance(&ci, NULL, &vkc.instance), "vkCreateInstance")) return 0;
#define LOAD(f) vk.f = (PFN_##f)gipa(vkc.instance, #f);
    VK_FN_INSTANCE(LOAD)
#undef LOAD
    if (validate) LOG("validation layer on");
    if (window && !SDL_Vulkan_CreateSurface(window, vkc.instance, NULL, &vkc.surface)) {
        LOG("window surface: %s", SDL_GetError());
        vkc.surface = VK_NULL_HANDLE;
    }
    return 1;
}

typedef struct { VkExtensionProperties *e; uint32_t n; } ExtList;
static ExtList device_extensions(VkPhysicalDevice p) {
    ExtList l = {0};
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, NULL);
    l.e = calloc(l.n ? l.n : 1, sizeof *l.e);
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, l.e);
    return l;
}
static int ext_in(const ExtList *l, const char *name) {
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->e[i].extensionName, name)) return 1;
    return 0;
}

// A Vulkan 1.3 GPU with a queue family for graphics and compute (and presentation, with a window); discrete first.
static int pick_device(void) {
    uint32_t n = 0;
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, NULL);
    if (!n) { LOG("no Vulkan GPU"); return 0; }
    VkPhysicalDevice *all = calloc(n, sizeof *all);
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, all);
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        vk.vkGetPhysicalDeviceProperties(all[i], &pr);
        ExtList ex = device_extensions(all[i]);
        int ok = pr.apiVersion >= VK_API_VERSION_1_3 && ext_in(&ex, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
                 (!vkc.surface || ext_in(&ex, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        free(ex.e);
        if (!ok) continue;
        uint32_t nq = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            VkBool32 present = VK_TRUE;
            if (vkc.surface) vk.vkGetPhysicalDeviceSurfaceSupportKHR(all[i], k, vkc.surface, &present);
            if ((q[k].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) || !present)
                continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (best < score) { best = score; vkc.gpu = all[i]; vkc.family = k; }
            break;
        }
        free(q);
    }
    free(all);
    if (best < 0) { LOG("no Vulkan 1.3 GPU with VK_EXT_external_memory_host%s", vkc.surface ? " that presents to the window" : ""); return 0; }
    vk.vkGetPhysicalDeviceProperties(vkc.gpu, &vkc.props);
    vk.vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &vkc.memory);
    vkc.db = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, &vkc.db};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
    vk.vkGetPhysicalDeviceProperties2(vkc.gpu, &p2);
    vkc.host_pointer_alignment = host.minImportedHostPointerAlignment;
    LOG("GPU: %s (Vulkan %u.%u.%u, driver %#x)", vkc.props.deviceName, VK_API_VERSION_MAJOR(vkc.props.apiVersion),
        VK_API_VERSION_MINOR(vkc.props.apiVersion), VK_API_VERSION_PATCH(vkc.props.apiVersion), vkc.props.driverVersion);
    return 1;
}

// Feature structs: queried with everything the GPU supports and passed back unchanged to vkCreateDevice.
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
static VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvids = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
static VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
static VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT fdcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceDepthClipEnableFeaturesEXT fdce = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
static VkPhysicalDeviceCustomBorderColorFeaturesEXT fcbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceProvokingVertexFeaturesEXT fpv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
static VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT fplr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
static VkPhysicalDeviceShaderObjectFeaturesEXT fso = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
static VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT ffsi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT ffl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT ffld = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
static int has_fl, has_fld;
static VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
static int has_lr;
static VkPhysicalDeviceFaultFeaturesEXT ffault = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};

static int create_device(void) {
    ExtList ex = device_extensions(vkc.gpu);
    const char *names[32];
    uint32_t nnames = 0;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    f11.pNext = &f12;
    f12.pNext = &f13;
    void **tail = &f13.pNext;
    // extension name, its feature struct (NULL: none), the flag in vkc
    struct { const char *name; void *features; int *flag; } want[] = {
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, NULL, NULL},
        {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, &fdb, &vkc.has_descriptor_buffer},
        {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, &fgpl, &vkc.has_gpl},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &feds3, &vkc.has_eds3},
        {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, &fvids, &vkc.has_vertex_input_ds},
        {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &fdcc, &vkc.has_depth_clip_control},
        {VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, &fdce, NULL},
        {VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &fcbc, NULL},
        {VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, &fpv, NULL},
        {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, &fcr, NULL},
        {VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, &fu8, NULL},
        {VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, &fplr, NULL},
        {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &fso, &vkc.has_shader_object},
        {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &ffsi, &vkc.has_interlock},
        // TOTK samples textures that are attachments of the same pass (G-buffer decals, depth-reading effects)
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, &ffl, &has_fl},
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME, &ffld, &has_fld},
        // ...and a draw that samples a target earlier draws of the pass wrote waits for them inside the pass
        {VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, &flr, &has_lr},
        // development (TOTK_VK_CHECKPOINTS=1): the markers a lost device reached
        {VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, NULL, &vkc.has_checkpoints},
        {VK_AMD_BUFFER_MARKER_EXTENSION_NAME, NULL, &vkc.has_markers},
        // the overlay's video memory figures
        {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, NULL, &vkc.has_memory_budget},
        // a lost device's fault: address and kind (no cost while nothing faults)
        {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, &ffault, &vkc.has_fault},
#ifdef __APPLE__
        // (fork) a portability driver (MoltenVK) must have this one enabled
        {"VK_KHR_portability_subset", NULL, NULL},
#endif
    };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        if (!strcmp(want[i].name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && !vkc.surface) continue;
        if ((want[i].flag == &vkc.has_checkpoints || want[i].flag == &vkc.has_markers) && !getenv("TOTK_VK_CHECKPOINTS")) continue;
        if (!ext_in(&ex, want[i].name)) { LOG("extension %s not available", want[i].name); continue; }
        names[nnames++] = want[i].name;
        if (want[i].flag) *want[i].flag = 1;
        if (want[i].features) { *tail = want[i].features; tail = (void **)&((VkBaseOutStructure *)want[i].features)->pNext; }
    }
    free(ex.e);
    vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f2);
    vkc.has_shader_object = vkc.has_shader_object && fso.shaderObject;
    if (!vkc.has_shader_object) {
        LOG("VK_EXT_shader_object is required but is not supported by this GPU or driver");
        return 0;
    }
    vkc.has_feedback_loop = has_fl && has_fld && ffl.attachmentFeedbackLoopLayout && ffld.attachmentFeedbackLoopDynamicState;
    vkc.has_local_read = has_lr && flr.dynamicRenderingLocalRead;
    vkc.has_fault = vkc.has_fault && ffault.deviceFault;
    ffault.deviceFaultVendorBinary = VK_FALSE;   // (a crash dump blob: not read)
    // Without the extensions (AMD's driver has the layout but not the dynamic state that shader objects need) a target
    // sampled while bound stays attached in GENERAL, as DXVK does for feedback loops; with local read the barrier
    // inside the pass orders it. TOTK_VK_FB_GENERAL=0: such textures read empty instead (the old behaviour);
    // TOTK_VK_FB_GENERAL=depth: only depth targets (colour ones read empty).
    const char *fbg = getenv("TOTK_VK_FB_GENERAL");
    vkc.fb_layout = vkc.has_feedback_loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                  : !(fbg && !strcmp(fbg, "0")) ? VK_IMAGE_LAYOUT_GENERAL : 0;
    vkc.fb_color = vkc.has_feedback_loop || !(fbg && !strcmp(fbg, "depth"));
    if (!vkc.has_feedback_loop)
        LOG("no attachment feedback loops: %s", !vkc.fb_layout ? "textures that are attachments of their pass read empty"
                                              : vkc.fb_color ? "textures that are attachments of their pass are sampled in GENERAL"
                                                             : "depth attachments of their pass are sampled in GENERAL, colour ones read empty");
    if (vkc.has_feedback_loop && !vkc.has_local_read) LOG("no barriers inside a pass: a draw sampling its own target may miss earlier draws' writes");
    // TOTK_VK_ROBUST=1 (development): bounds-checked buffer access, to tell a shader's out-of-range access from other
    // causes of a lost device
    if (!getenv("TOTK_VK_ROBUST")) f2.features.robustBufferAccess = VK_FALSE;
    else LOG("robust buffer access %s (TOTK_VK_ROBUST)", f2.features.robustBufferAccess ? "on" : "not supported");
    vkc.has_descriptor_buffer = vkc.has_descriptor_buffer && fdb.descriptorBuffer;
    if (vkc.has_descriptor_buffer) fdb.descriptorBufferCaptureReplay = VK_FALSE;   // tools only, costs driver work
    if (vkc.has_descriptor_buffer)
        LOG("descriptor buffer: uniform %zu, storage %zu, combined image sampler %zu, storage image %zu bytes; offset alignment %llu; "
            "ranges resource %llu / sampler %llu", vkc.db.uniformBufferDescriptorSize, vkc.db.storageBufferDescriptorSize,
            vkc.db.combinedImageSamplerDescriptorSize, vkc.db.storageImageDescriptorSize, (unsigned long long)vkc.db.descriptorBufferOffsetAlignment,
            (unsigned long long)vkc.db.maxResourceDescriptorBufferRange, (unsigned long long)vkc.db.maxSamplerDescriptorBufferRange);
    fdb.descriptorBufferImageLayoutIgnored = fdb.descriptorBufferImageLayoutIgnored && vkc.has_descriptor_buffer;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vkc.family,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = nnames, .ppEnabledExtensionNames = names};
    if (!vk_check(vk.vkCreateDevice(vkc.gpu, &ci, NULL, &vkc.device), "vkCreateDevice")) return 0;
#define LOAD(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(vkc.device, #f);
    VK_FN_DEVICE(LOAD)
#undef LOAD
    vk.vkGetDeviceQueue(vkc.device, vkc.family, 0, &vkc.queue);
    LOG("device: %u extensions; descriptor buffer %d, pipeline library %d, dynamic state 3 %d, vertex input %d, "
        "timeline semaphores %d, host pointer alignment %#llx", nnames, vkc.has_descriptor_buffer, vkc.has_gpl,
        vkc.has_eds3, vkc.has_vertex_input_ds, f12.timelineSemaphore, (unsigned long long)vkc.host_pointer_alignment);
    if (!f12.timelineSemaphore || !f13.dynamicRendering || !f13.synchronization2) { LOG("GPU lacks Vulkan 1.3 core features"); return 0; }
    return 1;
}

int vk_init(SDL_Window *window) {
    static int state;   // 0 not tried, 1 ready, -1 failed
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!state) state = create_instance(window) && pick_device() && create_device() ? 1 : -1;
    pthread_mutex_unlock(&mu);
    return state == 1;
}
