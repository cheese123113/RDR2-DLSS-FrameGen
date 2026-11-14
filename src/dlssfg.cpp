// DLSSFG.asi - DLSS Frame Generation for RDR2 (Vulkan).
//
// Step 2: frame generation (fg.cpp). This file holds the hooks, the log, the ini and the camera script. Changes made
// to the game's Vulkan setup, all for frame generation:
//   - vkCreateDevice: VK_KHR_buffer_device_address (+ its feature) enabled; the game only enables the EXT version
//   - vkCreateSwapchainKHR: TRANSFER_SRC/DST usage and one more image
//   - vkCreateImage: TRANSFER_SRC usage on D32S8 / RG16F images (so depth and motion vectors can be copied)
//
// RDR2.exe loads Vulkan and NVIDIA's NGX core at runtime (LoadLibrary + GetProcAddress); it has no static imports of
// either. So this patches RDR2.exe's import table entries for kernel32's GetProcAddress / LoadLibrary* and hands the
// game logging wrappers for:
//   - vkGetInstanceProcAddr / vkGetDeviceProcAddr, and through them vkCreateInstance, vkCreateDevice,
//     vkGetDeviceQueue(2), vkCreateSwapchainKHR, vkQueuePresentKHR
//   - the NGX core's NVSDK_NGX_VULKAN_* entry points (DLSS Super Resolution create/evaluate), so we can see the depth,
//     motion vectors and jitter the game already gives DLSS (frame generation will reuse them)
//   - NvLowLatencyVk (Reflex) calls the game imports
// It also asks our own copy of NGX (SDK 310.9.16) which instance/device extensions DLSS Frame Generation needs, and
// whether the game already enables them.
//
// Real Vulkan calls go through vulkan-1.dll's exported trampolines, which work with any instance/device handle.
//
// Log: DLSSFG.log next to the .asi.

#include "common.h"
#include "scripthook.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#define DLSSFG_BUILD "step2-fg " __DATE__ " " __TIME__

// ------------------------------------------------------------------------------------------------------------------
// Log
// ------------------------------------------------------------------------------------------------------------------

static FILE *g_log;
static CRITICAL_SECTION g_log_cs;
static LARGE_INTEGER g_qpf, g_t0;
wchar_t g_dir[MAX_PATH];
wchar_t g_fgdir[MAX_PATH];
bool g_fg_enabled = true;
bool g_fg_pacing = true;

static double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) / double(g_qpf.QuadPart);
}

Diag g_diag;
double qpc_ms() { return now_s() * 1000.0; }

static void diag_acquire(double ms) {
    g_diag.acq_last = ms;
    g_diag.acq_sum += ms;
    if (ms > g_diag.acq_max) g_diag.acq_max = ms;
    InterlockedIncrement(&g_diag.acq_n);
    if (ms > 4.0) InterlockedIncrement(&g_diag.acq_slow);
}

void dlog(const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "[%9.3f t%05lu] ", now_s(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

// Log a name only the first time it is seen (small fixed table; good enough for a few hundred names).
static bool first_time(const char *name) {
    static char seen[512][64];
    static int n;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < n; i++)
        if (!strncmp(seen[i], name, 63)) { ReleaseSRWLockExclusive(&lock); return false; }
    if (n < 512) { strncpy_s(seen[n], name, 63); n++; }
    ReleaseSRWLockExclusive(&lock);
    return true;
}

// Is p a readable address inside some loaded module image? (sanity check before calling through a vtable)
static bool in_module(const void *p) {
    HMODULE m = NULL;
    return p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)p, &m) && m;
}

static bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static void module_name(HMODULE m, char *out, size_t cap) {
    wchar_t w[MAX_PATH] = L"?";
    if (m) GetModuleFileNameW(m, w, MAX_PATH);
    const wchar_t *s = wcsrchr(w, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, s ? s + 1 : w, -1, out, (int)cap, NULL, NULL);
}

// ------------------------------------------------------------------------------------------------------------------
// Import table patching (RDR2.exe only)
// ------------------------------------------------------------------------------------------------------------------

static bool patch_iat(HMODULE mod, const char *dll, const char *func, void *hook, void **orig) {
    BYTE *base = (BYTE *)mod;
    auto dos = (IMAGE_DOS_HEADER *)base;
    auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll)) continue;
        auto iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        auto names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; iat->u1.Function; iat++, names++) {
            if (!d->OriginalFirstThunk || IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func)) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (orig && !*orig) *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            done = true;
        }
    }
    return done;
}

// ------------------------------------------------------------------------------------------------------------------
// Vulkan: real functions (vulkan-1.dll exports = loader trampolines, valid for any handle)
// ------------------------------------------------------------------------------------------------------------------

static struct {
    bool ready;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueSubmit2 QueueSubmit2;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2;
} T;

static FARPROC(WINAPI *o_GetProcAddress)(HMODULE, LPCSTR);

static void vk_init_trampolines() {
    if (T.ready) return;
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) { dlog("vk: vulkan-1.dll not loaded yet?"); return; }
#define GET(n) T.n = (decltype(T.n))o_GetProcAddress(vk, "vk" #n)
    GET(GetInstanceProcAddr); GET(GetDeviceProcAddr); GET(CreateInstance); GET(CreateDevice); GET(GetDeviceQueue);
    GET(GetDeviceQueue2); GET(CreateSwapchainKHR); GET(GetSwapchainImagesKHR); GET(QueuePresentKHR);
    GET(GetPhysicalDeviceProperties); GET(GetPhysicalDeviceQueueFamilyProperties);
    GET(EnumerateDeviceExtensionProperties); GET(CreateImage); GET(DestroySwapchainKHR); GET(AcquireNextImageKHR);
    GET(AcquireNextImage2KHR); GET(GetPhysicalDeviceSurfaceCapabilitiesKHR); GET(QueueSubmit); GET(QueueSubmit2);
    GET(WaitForFences); GET(GetPhysicalDeviceMemoryProperties2);
#undef GET
    T.ready = true;
    dlog("vk: trampolines from vulkan-1.dll: gipa=%p gdpa=%p createDevice=%p present=%p", T.GetInstanceProcAddr,
         T.GetDeviceProcAddr, T.CreateDevice, T.QueuePresentKHR);
}

VkInstance g_instance;
static VkDevice g_device;
static VkPhysicalDevice g_phys;
static bool g_bda_swapped;   // EXT buffer_device_address replaced by KHR in the game's device
static bool g_has_budget;    // VK_EXT_memory_budget enabled (for the VRAM log)
static volatile DWORD g_present_tid;
static VkFence volatile g_acq_fence;   // the fence the game passed to its last swapchain acquire

bool diag_memory(char *out, size_t cap) {
    if (!g_has_budget || !g_phys || !T.GetPhysicalDeviceMemoryProperties2) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    p.pNext = &b;
    T.GetPhysicalDeviceMemoryProperties2(g_phys, &p);
    double use = 0, budget = 0, size = 0;
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; i++)
        if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            use += b.heapUsage[i] / 1048576.0, budget += b.heapBudget[i] / 1048576.0;
            size += p.memoryProperties.memoryHeaps[i].size / 1048576.0;
        }
    sprintf_s(out, cap, "VRAM %.0f MB used of %.0f MB budget (%.0f MB card)", use, budget, size);
    return true;
}

static VkResult VKAPI_CALL hk_vkWaitForFences(VkDevice dev, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    double t0 = qpc_ms();
    VkResult r = T.WaitForFences(dev, n, f, all, timeout);
    double ms = qpc_ms() - t0;
    VkFence af = g_acq_fence;
    if (af)
        for (uint32_t i = 0; i < n; i++)
            if (f[i] == af) {
                g_diag.afence_frame += ms;
                if (ms > g_diag.afence_max) g_diag.afence_max = ms;
                break;
            }
    if (GetCurrentThreadId() == g_present_tid) {
        g_diag.pfence_frame += ms;
        if (ms > g_diag.pfence_max) g_diag.pfence_max = ms;
    } else {
        g_diag.ofence_frame += ms;
        if (ms > g_diag.ofence_max) g_diag.ofence_max = ms;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------------------------
// Our own NGX (SDK 310.9.16): what does DLSS Frame Generation want?
// ------------------------------------------------------------------------------------------------------------------

static const char *const k_project_id = "8b6f2e3d-41a7-4c09-b8d2-5e7a1c9f0d34", *const k_engine_version = "DLSSFG-RDR2-1";

static NVSDK_NGX_FeatureCommonInfo g_common;
static const wchar_t *g_paths[1];

static NVSDK_NGX_FeatureDiscoveryInfo fg_discovery() {
    memset(&g_common, 0, sizeof g_common);
    g_paths[0] = g_fgdir;
    g_common.PathListInfo.Path = g_paths;
    g_common.PathListInfo.Length = 1;
    NVSDK_NGX_FeatureDiscoveryInfo di = {};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    di.Identifier.v.ProjectDesc = {k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, k_engine_version};
    di.ApplicationDataPath = g_fgdir;
    di.FeatureInfo = &g_common;
    return di;
}

static bool has_name(const char *const *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (list[i] && !strcmp(list[i], name)) return true;
    return false;
}

static void fg_check_instance_exts(const VkInstanceCreateInfo *ci) {
    __try {
        unsigned ni = 0, nd = 0;
        const char **i = NULL, **d = NULL;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &i, &nd, &d);
        dlog("ngx(ours): RequiredExtensions -> %#x, %u instance / %u device", (unsigned)r, ni, nd);
        if (r == NVSDK_NGX_Result_Success)
            for (unsigned k = 0; k < ni; k++)
                dlog("ngx(ours):   instance ext %-45s game enables: %s", i[k],
                     has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, i[k]) ? "yes" : "NO");
        NVSDK_NGX_FeatureDiscoveryInfo di = fg_discovery();
        uint32_t fn = 0;
        VkExtensionProperties *fp = NULL;
        r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&di, &fn, &fp);
        dlog("ngx(ours): FrameGeneration instance ext requirements -> %#x, %u", (unsigned)r, fn);
        if (r == NVSDK_NGX_Result_Success)
            for (uint32_t k = 0; k < fn; k++)
                dlog("ngx(ours):   FG instance ext %-42s game enables: %s", fp[k].extensionName,
                     has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, fp[k].extensionName) ? "yes" : "NO");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dlog("ngx(ours): exception %#lx while asking for instance extensions", GetExceptionCode());
    }
}

static void fg_check_device_exts(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci) {
    if (!g_instance) { dlog("ngx(ours): no VkInstance seen, skipping device extension query"); return; }
    // Everything the GPU supports, to tell "game didn't enable it" from "GPU can't".
    static VkExtensionProperties sup[1024];
    uint32_t ns = 1024;
    if (T.EnumerateDeviceExtensionProperties(phys, NULL, &ns, sup) < 0) ns = 0;
    auto supported = [&](const char *n) {
        for (uint32_t k = 0; k < ns; k++)
            if (!strcmp(sup[k].extensionName, n)) return true;
        return false;
    };
    __try {
        unsigned ni = 0, nd = 0;
        const char **i = NULL, **d = NULL;
        if (NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &i, &nd, &d) == NVSDK_NGX_Result_Success)
            for (unsigned k = 0; k < nd; k++)
                dlog("ngx(ours):   device ext %-47s game enables: %-3s GPU supports: %s", d[k],
                     has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, d[k]) ? "yes" : "NO",
                     supported(d[k]) ? "yes" : "NO");
        NVSDK_NGX_FeatureDiscoveryInfo di = fg_discovery();
        uint32_t fn = 0;
        VkExtensionProperties *fp = NULL;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_instance, phys, &di, &fn, &fp);
        dlog("ngx(ours): FrameGeneration device ext requirements -> %#x, %u", (unsigned)r, fn);
        if (r == NVSDK_NGX_Result_Success)
            for (uint32_t k = 0; k < fn; k++)
                dlog("ngx(ours):   FG device ext %-44s game enables: %-3s GPU supports: %s", fp[k].extensionName,
                     has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, fp[k].extensionName) ? "yes" : "NO",
                     supported(fp[k].extensionName) ? "yes" : "NO");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dlog("ngx(ours): exception %#lx while asking for device extensions", GetExceptionCode());
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Vulkan hooks
// ------------------------------------------------------------------------------------------------------------------

static VkResult VKAPI_CALL hk_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                               VkInstance *out) {
    const VkApplicationInfo *ai = ci->pApplicationInfo;
    dlog("vk: vkCreateInstance app='%s' v%u engine='%s' v%u api=%u.%u.%u, %u layers, %u extensions",
         ai && ai->pApplicationName ? ai->pApplicationName : "", ai ? ai->applicationVersion : 0,
         ai && ai->pEngineName ? ai->pEngineName : "", ai ? ai->engineVersion : 0,
         ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0,
         ai ? VK_API_VERSION_PATCH(ai->apiVersion) : 0, ci->enabledLayerCount, ci->enabledExtensionCount);
    for (uint32_t i = 0; i < ci->enabledLayerCount; i++) dlog("vk:   layer %s", ci->ppEnabledLayerNames[i]);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) dlog("vk:   instance ext %s", ci->ppEnabledExtensionNames[i]);
    fg_check_instance_exts(ci);
    VkResult r = T.CreateInstance(ci, a, out);
    dlog("vk: vkCreateInstance -> %d, instance %p", r, r == VK_SUCCESS ? (void *)*out : NULL);
    if (r == VK_SUCCESS) g_instance = *out;
    return r;
}

static VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                             const VkAllocationCallbacks *a, VkDevice *out) {
    VkPhysicalDeviceProperties p = {};
    T.GetPhysicalDeviceProperties(phys, &p);
    dlog("vk: vkCreateDevice on '%s' (vendor %#x device %#x driver %#x api %u.%u.%u), %u queue infos, %u extensions",
         p.deviceName, p.vendorID, p.deviceID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion),
         VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), ci->queueCreateInfoCount,
         ci->enabledExtensionCount);
    VkQueueFamilyProperties qf[32];
    uint32_t nq = 32;
    T.GetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (uint32_t i = 0; i < nq; i++)
        dlog("vk:   GPU queue family %u: flags %#x count %u", i, qf[i].queueFlags, qf[i].queueCount);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        dlog("vk:   game creates %u queue(s) in family %u", ci->pQueueCreateInfos[i].queueCount,
             ci->pQueueCreateInfos[i].queueFamilyIndex);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) dlog("vk:   device ext %s", ci->ppEnabledExtensionNames[i]);
    for (auto s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) dlog("vk:   pNext sType %d", s->sType);
    if (ci->pEnabledFeatures) dlog("vk:   pEnabledFeatures set");
    fg_check_device_exts(phys, ci);

    // Frame generation needs VK_KHR_buffer_device_address and its feature. The game enables the older EXT version
    // (without its feature). Try KHR beside EXT first (leaves the game's setup alone), then KHR in place of EXT, then
    // the game's own setup (frame generation off).
    static const char *KHR = "VK_KHR_buffer_device_address", *EXT = "VK_EXT_buffer_device_address";
    bool has_khr = has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, KHR);
    bool has_ext = has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, EXT);
    static const char *BUDGET = "VK_EXT_memory_budget";   // read-only VRAM numbers for the log
    bool has_budget = has_name(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, BUDGET);
    bool sup_khr = false, sup_budget = false;
    {
        static VkExtensionProperties sup[1024];
        uint32_t ns = 1024;
        if (T.EnumerateDeviceExtensionProperties(phys, NULL, &ns, sup) >= 0)
            for (uint32_t k = 0; k < ns; k++) {
                if (!strcmp(sup[k].extensionName, KHR)) sup_khr = true;
                if (!strcmp(sup[k].extensionName, BUDGET)) sup_budget = true;
            }
    }
    bool add_budget = sup_budget && !has_budget;
    bool chain_has_bda = false;
    for (auto s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES ||
            s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT ||
            s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            chain_has_bda = true;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    bool bda_khr = has_khr;
    if (g_fg_enabled && sup_khr && !has_khr && ci->enabledExtensionCount < 120) {
        const char *exts[128];
        VkPhysicalDeviceBufferDeviceAddressFeatures bda = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bda.pNext = (void *)ci->pNext;
        bda.bufferDeviceAddress = VK_TRUE;
        VkDeviceCreateInfo c2 = *ci;
        if (!chain_has_bda) c2.pNext = &bda;
        uint32_t n = 0;
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) exts[n++] = ci->ppEnabledExtensionNames[i];
        exts[n++] = KHR;
        if (add_budget) exts[n++] = BUDGET;
        c2.enabledExtensionCount = n;
        c2.ppEnabledExtensionNames = exts;
        r = T.CreateDevice(phys, &c2, a, out);
        dlog("vk: vkCreateDevice with %s added beside the game's extensions%s -> %d", KHR,
             chain_has_bda ? "" : " (+ bufferDeviceAddress feature)", r);
        if (r != VK_SUCCESS && has_ext) {
            n = 0;
            for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
                if (strcmp(ci->ppEnabledExtensionNames[i], EXT)) exts[n++] = ci->ppEnabledExtensionNames[i];
            exts[n++] = KHR;
            if (add_budget) exts[n++] = BUDGET;
            c2.enabledExtensionCount = n;
            r = T.CreateDevice(phys, &c2, a, out);
            dlog("vk: vkCreateDevice with %s in place of %s -> %d", KHR, EXT, r);
            if (r == VK_SUCCESS) g_bda_swapped = true;
        }
        if (r == VK_SUCCESS) bda_khr = true, g_has_budget = has_budget || add_budget;
    }
    if (r != VK_SUCCESS) {
        g_has_budget = has_budget;
        r = T.CreateDevice(phys, ci, a, out);
        if (g_fg_enabled) dlog("vk: vkCreateDevice with the game's own extensions (frame generation off) -> %d", r);
    }
    dlog("vk: vkCreateDevice -> %d, device %p", r, r == VK_SUCCESS ? (void *)*out : NULL);
    if (r == VK_SUCCESS) {
        g_device = *out;
        g_phys = phys;
        fg_device_created(*out, phys, T.GetInstanceProcAddr, T.GetDeviceProcAddr, bda_khr);
    }
    return r;
}

static VkResult VKAPI_CALL hk_vkCreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a,
                                            VkImage *out) {
    if (g_fg_enabled && ci->imageType == VK_IMAGE_TYPE_2D && ci->samples == VK_SAMPLE_COUNT_1_BIT &&
        ci->extent.width >= 320 && ci->extent.height >= 180 &&
        (ci->format == VK_FORMAT_D32_SFLOAT_S8_UINT || ci->format == VK_FORMAT_R16G16_SFLOAT) &&
        !(ci->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) &&
        !(ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT))) {
        VkImageCreateInfo c2 = *ci;
        c2.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkResult r = T.CreateImage(dev, &c2, a, out);
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 24)
            dlog("vk: vkCreateImage %ux%u format %d usage %#x (+TRANSFER_SRC) flags %#x -> %d image %p", ci->extent.width,
                 ci->extent.height, ci->format, ci->usage, ci->flags, r, r == VK_SUCCESS ? (void *)*out : NULL);
        if (r == VK_SUCCESS) return r;
        // (should not happen; fall back to the game's own request)
    }
    return T.CreateImage(dev, ci, a, out);
}

static void VKAPI_CALL hk_vkGetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue *q) {
    T.GetDeviceQueue(dev, fam, idx, q);
    fg_queue(*q, fam);
    char key[64];
    sprintf_s(key, "q%p/%u/%u", (void *)dev, fam, idx);
    if (first_time(key)) dlog("vk: vkGetDeviceQueue family %u index %u -> %p", fam, idx, (void *)*q);
}

static void VKAPI_CALL hk_vkGetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *q) {
    T.GetDeviceQueue2(dev, info, q);
    fg_queue(*q, info->queueFamilyIndex);
    char key[64];
    sprintf_s(key, "q2%p/%u/%u", (void *)dev, info->queueFamilyIndex, info->queueIndex);
    if (first_time(key))
        dlog("vk: vkGetDeviceQueue2 family %u index %u flags %#x -> %p", info->queueFamilyIndex, info->queueIndex,
             info->flags, (void *)*q);
}

static VkResult VKAPI_CALL hk_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                                                   const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    dlog("vk: vkCreateSwapchainKHR %ux%u format %d colorspace %d minImages %u usage %#x presentMode %d "
         "composite %#x old %p",
         ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, ci->imageColorSpace, ci->minImageCount,
         ci->imageUsage, ci->presentMode, ci->compositeAlpha, (void *)ci->oldSwapchain);
    for (auto s = (const VkBaseInStructure *)ci->pNext; s; s = s->pNext) dlog("vk:   pNext sType %d", s->sType);
    fg_flush();   // (the old swapchain may be retired by this one)
    VkSwapchainCreateInfoKHR c2 = *ci;
    if (g_fg_enabled && g_phys && T.GetPhysicalDeviceSurfaceCapabilitiesKHR) {
        // Frame generation copies the finished frame out of the swapchain and the generated one in, and holds a
        // second image each frame.
        VkSurfaceCapabilitiesKHR caps = {};
        if (T.GetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, ci->surface, &caps) == VK_SUCCESS) {
            c2.imageUsage |= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) & caps.supportedUsageFlags;
            // two more: one for the generated frame, one so acquiring it rarely has to wait for a refresh
            uint32_t want = c2.minImageCount + 3;
            c2.minImageCount = caps.maxImageCount && want > caps.maxImageCount ? caps.maxImageCount : want;
            dlog("vk:   for frame generation: usage %#x -> %#x, minImages %u -> %u (surface supports usage %#x, "
                 "images %u..%u)", ci->imageUsage, c2.imageUsage, ci->minImageCount, c2.minImageCount,
                 caps.supportedUsageFlags, caps.minImageCount, caps.maxImageCount);
        }
    }
    VkResult r = T.CreateSwapchainKHR(dev, &c2, a, out);
    if (r != VK_SUCCESS && (c2.imageUsage != ci->imageUsage || c2.minImageCount != ci->minImageCount)) {
        dlog("vk: vkCreateSwapchainKHR with our changes -> %d, retrying the game's own", r);
        c2 = *ci;
        r = T.CreateSwapchainKHR(dev, &c2, a, out);
    }
    uint32_t n = 0;
    if (r == VK_SUCCESS) T.GetSwapchainImagesKHR(dev, *out, &n, NULL);
    dlog("vk: vkCreateSwapchainKHR -> %d, swapchain %p, %u images", r, r == VK_SUCCESS ? (void *)*out : NULL, n);
    if (r == VK_SUCCESS) fg_swapchain_created(*out, &c2);
    return r;
}

static VkResult VKAPI_CALL hk_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence fence) {
    static bool logged;
    if (!logged) logged = true, dlog("vk: the game submits with vkQueueSubmit (first on thread %lu)", GetCurrentThreadId());
    for (uint32_t i = 0; i < n; i++)
        if (s[i].commandBufferCount) fg_submitted(s[i].pCommandBuffers, s[i].commandBufferCount);
    bool lk = q == g_present_queue;   // our pacer thread presents on that queue
    if (lk) AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = T.QueueSubmit(q, n, s, fence);
    if (lk) ReleaseSRWLockExclusive(&g_queue_lock);
    return r;
}

static VkResult VKAPI_CALL hk_vkQueueWaitIdle(VkQueue q) {
    static PFN_vkQueueWaitIdle real;
    if (!real) real = (PFN_vkQueueWaitIdle)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkQueueWaitIdle");
    bool lk = q == g_present_queue;
    if (lk) fg_flush(), AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = real(q);
    if (lk) ReleaseSRWLockExclusive(&g_queue_lock);
    return r;
}

static VkResult VKAPI_CALL hk_vkDeviceWaitIdle(VkDevice dev) {
    static PFN_vkDeviceWaitIdle real;
    if (!real) real = (PFN_vkDeviceWaitIdle)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkDeviceWaitIdle");
    fg_flush();
    AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = real(dev);
    ReleaseSRWLockExclusive(&g_queue_lock);
    return r;
}

static void submitted2(uint32_t n, const VkSubmitInfo2 *s) {
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < s[i].commandBufferInfoCount; k++) fg_submitted(&s[i].pCommandBufferInfos[k].commandBuffer, 1);
}

static VkResult VKAPI_CALL hk_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence fence) {
    static bool logged;
    if (!logged) logged = true, dlog("vk: the game submits with vkQueueSubmit2");
    submitted2(n, s);
    bool lk = q == g_present_queue;
    if (lk) AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = T.QueueSubmit2(q, n, s, fence);
    if (lk) ReleaseSRWLockExclusive(&g_queue_lock);
    return r;
}

static VkResult VKAPI_CALL hk_vkQueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence fence) {
    static PFN_vkQueueSubmit2KHR real;
    if (!real) real = (PFN_vkQueueSubmit2KHR)T.GetDeviceProcAddr(g_device, "vkQueueSubmit2KHR");
    static bool logged;
    if (!logged) logged = true, dlog("vk: the game submits with vkQueueSubmit2KHR");
    submitted2(n, s);
    bool lk = q == g_present_queue;
    if (lk) AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = real(q, n, s, fence);
    if (lk) ReleaseSRWLockExclusive(&g_queue_lock);
    return r;
}

static void VKAPI_CALL hk_vkDestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    dlog("vk: vkDestroySwapchainKHR %p", (void *)sc);
    fg_flush();   // nothing of ours still to present on it
    fg_swapchain_destroyed(sc);
    T.DestroySwapchainKHR(dev, sc, a);
}

static VkResult VKAPI_CALL hk_vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem,
                                                    VkFence fence, uint32_t *idx) {
    static bool logged;
    if (!logged) logged = true, dlog("vk: the game acquires swapchain images on thread %lu", GetCurrentThreadId());
    double t0 = qpc_ms();
    if (fence) g_acq_fence = fence;
    AcquireSRWLockExclusive(&g_acquire_lock);
    VkResult r = T.AcquireNextImageKHR(dev, sc, timeout, sem, fence, idx);
    ReleaseSRWLockExclusive(&g_acquire_lock);
    diag_acquire(qpc_ms() - t0);
    return r;
}

static VkResult VKAPI_CALL hk_vkAcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR *info, uint32_t *idx) {
    static bool logged;
    if (!logged) logged = true, dlog("vk: the game acquires swapchain images (Acquire2) on thread %lu", GetCurrentThreadId());
    double t0 = qpc_ms();
    if (info->fence) g_acq_fence = info->fence;
    AcquireSRWLockExclusive(&g_acquire_lock);
    VkResult r = T.AcquireNextImage2KHR(dev, info, idx);
    ReleaseSRWLockExclusive(&g_acquire_lock);
    diag_acquire(qpc_ms() - t0);
    return r;
}

// Reflex marker counts (filled by the NvLL hooks below), reported with the present stats.
static volatile LONG g_nvll_markers[16], g_nvll_sleeps;
static volatile LONG64 g_nvll_last_frame, g_sim_frame, g_present_frame;
static volatile bool g_reflex_seen;
uint64_t reflex_sim_frame() { return (uint64_t)g_sim_frame; }
uint64_t reflex_present_frame() { return (uint64_t)g_present_frame; }
bool reflex_seen() { return g_reflex_seen; }
static volatile LONG g_dlss_evals;

static VkResult VKAPI_CALL hk_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    g_present_tid = GetCurrentThreadId();
    static uint64_t frames;
    static LARGE_INTEGER last;
    static double sum, mn = 1e9, mx;
    static LONG last_evals;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (frames == 0) {
        dlog("vk: first vkQueuePresentKHR on queue %p, %u swapchain(s), image %u, %u wait semaphore(s)", (void *)q,
             pi->swapchainCount, pi->pImageIndices ? pi->pImageIndices[0] : 0, pi->waitSemaphoreCount);
        for (auto s = (const VkBaseInStructure *)pi->pNext; s; s = s->pNext) dlog("vk:   pNext sType %d", s->sType);
    } else {
        double ms = double(t.QuadPart - last.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
        sum += ms;
        if (ms < mn) mn = ms;
        if (ms > mx) mx = ms;
    }
    last = t;
    frames++;
    if (frames % 600 == 0) {
        LONG ev = g_dlss_evals;
        dlog("present: frame %llu, last 599 frames avg %.3f ms (%.1f fps) min %.3f max %.3f; DLSS evals %ld; "
             "reflex markers sim %ld/%ld rs %ld/%ld present %ld/%ld sleeps %ld last frameID %lld",
             frames, sum / 599.0, 599000.0 / sum, mn, mx, ev - last_evals, g_nvll_markers[0], g_nvll_markers[1],
             g_nvll_markers[2], g_nvll_markers[3], g_nvll_markers[4], g_nvll_markers[5], g_nvll_sleeps,
             g_nvll_last_frame);
        last_evals = ev;
        sum = 0; mn = 1e9; mx = 0;
    }
    return fg_present(q, pi, T.QueuePresentKHR);
}

static PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance inst, const char *name);
static PFN_vkVoidFunction VKAPI_CALL hk_vkGetDeviceProcAddr(VkDevice dev, const char *name);

struct VkHook { const char *name; PFN_vkVoidFunction hook; };
static const VkHook k_vk_hooks[] = {
    {"vkGetInstanceProcAddr", (PFN_vkVoidFunction)hk_vkGetInstanceProcAddr},
    {"vkGetDeviceProcAddr", (PFN_vkVoidFunction)hk_vkGetDeviceProcAddr},
    {"vkCreateInstance", (PFN_vkVoidFunction)hk_vkCreateInstance},
    {"vkCreateDevice", (PFN_vkVoidFunction)hk_vkCreateDevice},
    {"vkGetDeviceQueue", (PFN_vkVoidFunction)hk_vkGetDeviceQueue},
    {"vkGetDeviceQueue2", (PFN_vkVoidFunction)hk_vkGetDeviceQueue2},
    {"vkCreateSwapchainKHR", (PFN_vkVoidFunction)hk_vkCreateSwapchainKHR},
    {"vkQueuePresentKHR", (PFN_vkVoidFunction)hk_vkQueuePresentKHR},
    {"vkCreateImage", (PFN_vkVoidFunction)hk_vkCreateImage},
    {"vkDestroySwapchainKHR", (PFN_vkVoidFunction)hk_vkDestroySwapchainKHR},
    {"vkAcquireNextImageKHR", (PFN_vkVoidFunction)hk_vkAcquireNextImageKHR},
    {"vkAcquireNextImage2KHR", (PFN_vkVoidFunction)hk_vkAcquireNextImage2KHR},
    {"vkQueueSubmit", (PFN_vkVoidFunction)hk_vkQueueSubmit},
    {"vkQueueSubmit2", (PFN_vkVoidFunction)hk_vkQueueSubmit2},
    {"vkQueueSubmit2KHR", (PFN_vkVoidFunction)hk_vkQueueSubmit2KHR},
    {"vkWaitForFences", (PFN_vkVoidFunction)hk_vkWaitForFences},
    {"vkQueueWaitIdle", (PFN_vkVoidFunction)hk_vkQueueWaitIdle},
    {"vkDeviceWaitIdle", (PFN_vkVoidFunction)hk_vkDeviceWaitIdle},
};

// Swap in our wrapper when the game asks for a hooked Vulkan function and the real one exists.
static PFN_vkVoidFunction vk_filter(const char *via, const char *name, PFN_vkVoidFunction real) {
    if (!real || !name) return real;
    for (const VkHook &h : k_vk_hooks)
        if (!strcmp(h.name, name)) {
            vk_init_trampolines();
            if (!T.ready) return real;
            char key[96];
            sprintf_s(key, "%s:%s", via, name);
            if (first_time(key)) dlog("vk: hooked %s (asked via %s)", name, via);
            return h.hook;
        }
    return real;
}

static PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    return vk_filter("gipa", name, T.GetInstanceProcAddr(inst, name));
}

static PFN_vkVoidFunction VKAPI_CALL hk_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction r = T.GetDeviceProcAddr(dev, name);
    if (!r && g_bda_swapped && name && !strcmp(name, "vkGetBufferDeviceAddressEXT")) {
        r = T.GetDeviceProcAddr(dev, "vkGetBufferDeviceAddressKHR");   // same function, promoted
        dlog("vk: game asked for vkGetBufferDeviceAddressEXT, gave it the KHR one (%p)", (void *)r);
    }
    return vk_filter("gdpa", name, r);
}

// ------------------------------------------------------------------------------------------------------------------
// The game's NGX (DLSS Super Resolution). Entry points come from the NGX core via GetProcAddress. The game's NGX
// library is old (rel_1_6), so the wrappers pass all arguments through untouched and only read the ones whose meaning
// is stable (feature id, parameter block).
// ------------------------------------------------------------------------------------------------------------------

typedef uintptr_t U;
typedef int(__cdecl *PFN_ngx8)(U, U, U, U, U, U, U, U);

enum NgxFn { N_Init, N_Init_Ext, N_Init_ProjectID, N_Shutdown, N_GetParameters, N_AllocateParameters,
             N_GetCapabilityParameters, N_DestroyParameters, N_GetScratchBufferSize, N_CreateFeature,
             N_ReleaseFeature, N_EvaluateFeature, N_EvaluateFeature_C, N_COUNT };
static PFN_ngx8 o_ngx[N_COUNT];

static void log_res(const NVSDK_NGX_Parameter *p, const char *name) {
    void *v = NULL;
    if (p->Get(name, &v) != NVSDK_NGX_Result_Success || !v) { dlog("ngx:     %-16s (none)", name); return; }
    if (!readable(v, sizeof(NVSDK_NGX_Resource_VK))) { dlog("ngx:     %-16s %p (unreadable)", name, v); return; }
    auto r = (const NVSDK_NGX_Resource_VK *)v;
    if (r->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) {
        auto &i = r->Resource.ImageViewInfo;
        dlog("ngx:     %-16s image %p view %p format %d %ux%u mip %u+%u layer %u+%u aspect %#x rw %d", name,
             (void *)i.Image, (void *)i.ImageView, i.Format, i.Width, i.Height, i.SubresourceRange.baseMipLevel,
             i.SubresourceRange.levelCount, i.SubresourceRange.baseArrayLayer, i.SubresourceRange.layerCount,
             i.SubresourceRange.aspectMask, r->ReadWrite);
    } else {
        dlog("ngx:     %-16s buffer %p size %llu", name, (void *)r->Resource.BufferInfo.Buffer,
             (unsigned long long)r->Resource.BufferInfo.SizeInBytes);
    }
}

static bool params_look_ok(U param) {
    auto p = (const NVSDK_NGX_Parameter *)param;
    return readable(p, sizeof(void *)) && readable(*(void **)p, 8 * 18) && in_module(**(void ***)p);
}

static void log_params(const char *what, U param) {
    auto p = (const NVSDK_NGX_Parameter *)param;
    if (!params_look_ok(param)) {
        dlog("ngx:   %s: parameter block %p doesn't look like an NGX parameter object, not reading it", what, p);
        return;
    }
    __try {
        unsigned w = 0, h = 0, ow = 0, oh = 0, sw = 0, sh = 0;
        int pq = -1, flags = 0, reset = -1;
        float jx = 0, jy = 0, mvx = 0, mvy = 0, sharp = 0, pre = 0, es = 0, dt = 0;
        p->Get(NVSDK_NGX_Parameter_Width, &w);
        p->Get(NVSDK_NGX_Parameter_Height, &h);
        p->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
        p->Get(NVSDK_NGX_Parameter_OutHeight, &oh);
        p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pq);
        p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
        p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &sw);
        p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &sh);
        p->Get(NVSDK_NGX_Parameter_Reset, &reset);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jx);
        p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jy);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvx);
        p->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvy);
        p->Get(NVSDK_NGX_Parameter_Sharpness, &sharp);
        p->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &pre);
        p->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &es);
        p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &dt);
        dlog("ngx:   %s: in %ux%u out %ux%u subrect %ux%u quality %d createFlags %#x reset %d jitter (%.4f, %.4f) "
             "mvScale (%.3f, %.3f) sharpness %.3f preExposure %.3f exposureScale %.3f dt %.2f",
             what, w, h, ow, oh, sw, sh, pq, flags, reset, jx, jy, mvx, mvy, sharp, pre, es, dt);
        log_res(p, NVSDK_NGX_Parameter_Color);
        log_res(p, NVSDK_NGX_Parameter_Output);
        log_res(p, NVSDK_NGX_Parameter_Depth);
        log_res(p, NVSDK_NGX_Parameter_MotionVectors);
        log_res(p, NVSDK_NGX_Parameter_ExposureTexture);
        log_res(p, NVSDK_NGX_Parameter_TransparencyMask);
        log_res(p, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dlog("ngx:   %s: exception %#lx reading parameters", what, GetExceptionCode());
    }
}

static int __cdecl hk_ngx_init(U a, U b, U c, U d, U e, U f, U g, U h) {
    int r = o_ngx[N_Init](a, b, c, d, e, f, g, h);
    dlog("ngx(game): NVSDK_NGX_VULKAN_Init(appId %llu, instance? %#llx, phys? %#llx, device? %#llx) -> %#x",
         (unsigned long long)a, (unsigned long long)c, (unsigned long long)d, (unsigned long long)e, (unsigned)r);
    return r;
}
static int __cdecl hk_ngx_init_ext(U a, U b, U c, U d, U e, U f, U g, U h) {
    int r = o_ngx[N_Init_Ext](a, b, c, d, e, f, g, h);
    dlog("ngx(game): NVSDK_NGX_VULKAN_Init_Ext(%#llx, %#llx, %#llx, %#llx, %#llx) -> %#x", (unsigned long long)a,
         (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, (unsigned long long)e, (unsigned)r);
    return r;
}
static int __cdecl hk_ngx_init_pid(U a, U b, U c, U d, U e, U f, U g, U h) {
    int r = o_ngx[N_Init_ProjectID](a, b, c, d, e, f, g, h);
    dlog("ngx(game): NVSDK_NGX_VULKAN_Init_ProjectID -> %#x", (unsigned)r);
    return r;
}
static int __cdecl hk_ngx_shutdown(U a, U b, U c, U d, U e, U f, U g, U h) {
    dlog("ngx(game): NVSDK_NGX_VULKAN_Shutdown");
    return o_ngx[N_Shutdown](a, b, c, d, e, f, g, h);
}
#define NGX_PASS(fn, label)                                                                                         \
    static int __cdecl hk_ngx_##fn(U a, U b, U c, U d, U e, U f, U g, U h) {                                        \
        int r = o_ngx[N_##fn](a, b, c, d, e, f, g, h);                                                              \
        static volatile LONG n;                                                                                     \
        if (InterlockedIncrement(&n) <= 3) dlog("ngx(game): " label " -> %#x", (unsigned)r);                        \
        return r;                                                                                                   \
    }
NGX_PASS(GetParameters, "NVSDK_NGX_VULKAN_GetParameters")
NGX_PASS(AllocateParameters, "NVSDK_NGX_VULKAN_AllocateParameters")
NGX_PASS(GetCapabilityParameters, "NVSDK_NGX_VULKAN_GetCapabilityParameters")
NGX_PASS(DestroyParameters, "NVSDK_NGX_VULKAN_DestroyParameters")
NGX_PASS(GetScratchBufferSize, "NVSDK_NGX_VULKAN_GetScratchBufferSize")

// CreateFeature(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
static int __cdecl hk_ngx_CreateFeature(U cmd, U feature, U params, U handle, U e, U f, U g, U h) {
    dlog("ngx(game): CreateFeature feature %llu (1 = DLSS SR) cmdbuf %#llx", (unsigned long long)feature,
         (unsigned long long)cmd);
    log_params("create", params);
    int flags = -1;
    if (params_look_ok(params)) ((const NVSDK_NGX_Parameter *)params)->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    int r = o_ngx[N_CreateFeature](cmd, feature, params, handle, e, f, g, h);
    void *hd = (handle && readable((void *)handle, 8)) ? *(void **)handle : NULL;
    dlog("ngx(game): CreateFeature -> %#x handle %p", (unsigned)r, hd);
    if (r == NVSDK_NGX_Result_Success && feature == NVSDK_NGX_Feature_SuperSampling && flags >= 0) fg_dlss_created(flags);
    return r;
}

static int __cdecl hk_ngx_ReleaseFeature(U a, U b, U c, U d, U e, U f, U g, U h) {
    dlog("ngx(game): ReleaseFeature handle %#llx (DLSS evals so far %ld)", (unsigned long long)a, g_dlss_evals);
    return o_ngx[N_ReleaseFeature](a, b, c, d, e, f, g, h);
}

// EvaluateFeature(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, callback)
static void on_evaluate(const char *which, U cmd, U params) {
    LONG n = InterlockedIncrement(&g_dlss_evals);
    if (n <= 3 || n % 1800 == 0) {
        char what[64];
        sprintf_s(what, "%s #%ld cmdbuf %#llx", which, n, (unsigned long long)cmd);
        log_params(what, params);
    }
}
static void after_evaluate(const char *which, int r, U cmd, U params) {
    if (r != NVSDK_NGX_Result_Success) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 10) dlog("ngx(game): %s FAILED %#x", which, (unsigned)r);
        return;
    }
    if (params_look_ok(params)) {
        __try {
            fg_after_dlss_evaluate((VkCommandBuffer)cmd, (const NVSDK_NGX_Parameter *)params);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static volatile LONG n;
            if (InterlockedIncrement(&n) <= 3) dlog("fg: exception %#lx copying the DLSS inputs", GetExceptionCode());
        }
    }
}
static int __cdecl hk_ngx_EvaluateFeature(U cmd, U handle, U params, U cb, U e, U f, U g, U h) {
    on_evaluate("evaluate", cmd, params);
    int r = o_ngx[N_EvaluateFeature](cmd, handle, params, cb, e, f, g, h);
    after_evaluate("evaluate", r, cmd, params);
    return r;
}
static int __cdecl hk_ngx_EvaluateFeature_C(U cmd, U handle, U params, U cb, U e, U f, U g, U h) {
    on_evaluate("evaluate_C", cmd, params);
    int r = o_ngx[N_EvaluateFeature_C](cmd, handle, params, cb, e, f, g, h);
    after_evaluate("evaluate_C", r, cmd, params);
    return r;
}

struct NgxHook { const char *name; NgxFn id; void *hook; };
static const NgxHook k_ngx_hooks[] = {
    {"NVSDK_NGX_VULKAN_Init", N_Init, (void *)hk_ngx_init},
    {"NVSDK_NGX_VULKAN_Init_Ext", N_Init_Ext, (void *)hk_ngx_init_ext},
    {"NVSDK_NGX_VULKAN_Init_ProjectID", N_Init_ProjectID, (void *)hk_ngx_init_pid},
    {"NVSDK_NGX_VULKAN_Shutdown", N_Shutdown, (void *)hk_ngx_shutdown},
    {"NVSDK_NGX_VULKAN_GetParameters", N_GetParameters, (void *)hk_ngx_GetParameters},
    {"NVSDK_NGX_VULKAN_AllocateParameters", N_AllocateParameters, (void *)hk_ngx_AllocateParameters},
    {"NVSDK_NGX_VULKAN_GetCapabilityParameters", N_GetCapabilityParameters, (void *)hk_ngx_GetCapabilityParameters},
    {"NVSDK_NGX_VULKAN_DestroyParameters", N_DestroyParameters, (void *)hk_ngx_DestroyParameters},
    {"NVSDK_NGX_VULKAN_GetScratchBufferSize", N_GetScratchBufferSize, (void *)hk_ngx_GetScratchBufferSize},
    {"NVSDK_NGX_VULKAN_CreateFeature", N_CreateFeature, (void *)hk_ngx_CreateFeature},
    {"NVSDK_NGX_VULKAN_ReleaseFeature", N_ReleaseFeature, (void *)hk_ngx_ReleaseFeature},
    {"NVSDK_NGX_VULKAN_EvaluateFeature", N_EvaluateFeature, (void *)hk_ngx_EvaluateFeature},
    {"NVSDK_NGX_VULKAN_EvaluateFeature_C", N_EvaluateFeature_C, (void *)hk_ngx_EvaluateFeature_C},
};

// ------------------------------------------------------------------------------------------------------------------
// kernel32 hooks in RDR2.exe's import table
// ------------------------------------------------------------------------------------------------------------------

static FARPROC WINAPI hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC r = o_GetProcAddress(m, name);
    if (!r || IS_INTRESOURCE(name)) return r;
    if (name[0] == 'v' && name[1] == 'k') {
        FARPROC f = (FARPROC)vk_filter("GetProcAddress", name, (PFN_vkVoidFunction)r);
        return f;
    }
    if (!strncmp(name, "NVSDK_NGX_", 10)) {
        char mod[MAX_PATH];
        module_name(m, mod, sizeof mod);
        for (const NgxHook &h : k_ngx_hooks)
            if (!strcmp(h.name, name)) {
                if (o_ngx[h.id] && o_ngx[h.id] != (PFN_ngx8)r)
                    dlog("ngx(game): %s asked again from %s with a different address %p (was %p)", name, mod, r,
                         o_ngx[h.id]);
                o_ngx[h.id] = (PFN_ngx8)r;
                if (first_time(name)) dlog("ngx(game): hooked %s from %s (%p)", name, mod, r);
                return (FARPROC)h.hook;
            }
        if (first_time(name)) dlog("ngx(game): game asked for %s from %s (not hooked)", name, mod);
    }
    return r;
}

static bool interesting_dll(const wchar_t *path) {
    if (!path) return false;
    wchar_t low[MAX_PATH];
    wcsncpy_s(low, path, _TRUNCATE);
    _wcslwr_s(low);
    return wcsstr(low, L"vulkan") || wcsstr(low, L"ngx") || wcsstr(low, L"dlss") || wcsstr(low, L"lowlatency");
}

static HMODULE(WINAPI *o_LoadLibraryW)(LPCWSTR);
static HMODULE(WINAPI *o_LoadLibraryA)(LPCSTR);
static HMODULE(WINAPI *o_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);

static void log_load(const char *fn, const wchar_t *path, HMODULE r, DWORD flags) {
    char p[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof p, NULL, NULL);
    dlog("load: %s(\"%s\", flags %#lx) -> %p", fn, p, flags, (void *)r);
}

static HMODULE WINAPI hk_LoadLibraryW(LPCWSTR path) {
    HMODULE r = o_LoadLibraryW(path);
    if (interesting_dll(path)) log_load("LoadLibraryW", path, r, 0);
    return r;
}
static HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR path, HANDLE f, DWORD flags) {
    HMODULE r = o_LoadLibraryExW(path, f, flags);
    if (interesting_dll(path)) log_load("LoadLibraryExW", path, r, flags);
    return r;
}
static HMODULE WINAPI hk_LoadLibraryA(LPCSTR path) {
    HMODULE r = o_LoadLibraryA(path);
    if (path) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
        if (interesting_dll(w)) log_load("LoadLibraryA", w, r, 0);
    }
    return r;
}

// ------------------------------------------------------------------------------------------------------------------
// NvLowLatencyVk (Reflex) hooks in RDR2.exe's import table
// ------------------------------------------------------------------------------------------------------------------

typedef int(__cdecl *PFN_nvll4)(U, U, U, U);
static PFN_nvll4 o_nvll_init, o_nvll_initdev, o_nvll_sleepmode, o_nvll_marker, o_nvll_sleep;

static int __cdecl hk_nvll_init(U a, U b, U c, U d) {
    int r = o_nvll_init(a, b, c, d);
    dlog("reflex: NvLL_VK_Initialize -> %d", r);
    return r;
}
static int __cdecl hk_nvll_initdev(U dev, U sem, U c, U d) {
    int r = o_nvll_initdev(dev, sem, c, d);
    dlog("reflex: NvLL_VK_InitLowLatencyDevice(device %#llx) -> %d, semaphore %p", (unsigned long long)dev, r,
         sem && readable((void *)sem, 8) ? *(void **)sem : NULL);
    return r;
}
static int __cdecl hk_nvll_sleepmode(U dev, U params, U c, U d) {
    int r = o_nvll_sleepmode(dev, params, c, d);
    // NVLL_VK_SET_SLEEP_MODE_PARAMS { bool bLowLatencyMode; bool bLowLatencyBoost; uint32_t minimumIntervalUs; }
    if (params && readable((void *)params, 8)) {
        const uint8_t *b = (const uint8_t *)params;
        dlog("reflex: NvLL_VK_SetSleepMode(lowLatency %u, boost %u, minIntervalUs %u) -> %d", b[0], b[1],
             *(const uint32_t *)(b + 4), r);
    } else {
        dlog("reflex: NvLL_VK_SetSleepMode(%#llx) -> %d", (unsigned long long)params, r);
    }
    return r;
}
static int __cdecl hk_nvll_marker(U dev, U params, U c, U d) {
    // NVLL_VK_LATENCY_MARKER_PARAMS { uint64_t frameID; NVLL_VK_LATENCY_MARKER_TYPE markerType; }
    if (params) {
        const uint64_t *q = (const uint64_t *)params;
        uint32_t type = (uint32_t)q[1];
        if (type < 16) InterlockedIncrement(&g_nvll_markers[type]);
        g_nvll_last_frame = (LONG64)q[0];
        if (type == 0) g_sim_frame = (LONG64)q[0];          // SIMULATION_START
        if (type == 4) g_present_frame = (LONG64)q[0];      // PRESENT_START
        g_reflex_seen = true;
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 8) dlog("reflex: marker frameID %llu type %u", q[0], type);
    }
    return o_nvll_marker(dev, params, c, d);
}
static int __cdecl hk_nvll_sleep(U dev, U value, U c, U d) {
    InterlockedIncrement(&g_nvll_sleeps);
    double t0 = qpc_ms();
    int r = o_nvll_sleep(dev, value, c, d);
    double ms = qpc_ms() - t0;
    g_diag.sleep_last = ms;
    g_diag.sleep_sum += ms;
    if (ms > g_diag.sleep_max) g_diag.sleep_max = ms;
    InterlockedIncrement(&g_diag.sleep_n);
    return r;
}

// ------------------------------------------------------------------------------------------------------------------
// Camera: a ScriptHook script reads the game's final rendered camera every game update
// ------------------------------------------------------------------------------------------------------------------

static SRWLOCK g_cam_lock = SRWLOCK_INIT;
static Camera g_cam, g_cam_hist[32];   // history indexed by Reflex frame ID % 32

bool camera_get(Camera *out) {
    AcquireSRWLockShared(&g_cam_lock);
    *out = g_cam;
    ReleaseSRWLockShared(&g_cam_lock);
    return out->valid;
}

bool camera_get_frame(uint64_t frame, Camera *out) {
    AcquireSRWLockShared(&g_cam_lock);
    *out = g_cam_hist[frame % 32];
    ReleaseSRWLockShared(&g_cam_lock);
    return out->valid && out->frame == frame;
}

static void camera_script() {
    dlog("cam: script running");
    uint64_t tick = 0;
    for (;;) {
        Vector3 p = sh::Invoke<Vector3>(0x5352E025EC2B416Full);       // CAM::GET_FINAL_RENDERED_CAM_COORD
        Vector3 r = sh::Invoke<Vector3>(0x602685BD85DD26CAull, 2);    // CAM::GET_FINAL_RENDERED_CAM_ROT (order 2)
        float fov = sh::Invoke<float>(0x04AF77971E508F6Aull);         // CAM::GET_FINAL_RENDERED_CAM_FOV
        uint64_t frame = reflex_sim_frame();   // the game update this script tick runs in
        Camera c = {{p.x, p.y, p.z}, {r.x, r.y, r.z}, fov, ++tick, frame, true};
        AcquireSRWLockExclusive(&g_cam_lock);
        g_cam = c;
        if (frame) g_cam_hist[frame % 32] = c;
        ReleaseSRWLockExclusive(&g_cam_lock);
        if (tick == 1 || tick % 6000 == 0)
            dlog("cam: tick %llu frame %llu (last presented %llu) pos (%.2f %.2f %.2f) rot (%.2f %.2f %.2f) fov %.2f", tick,
                 frame, reflex_present_frame(), p.x, p.y, p.z, r.x, r.y, r.z, fov);
        WAIT(0);
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Entry
// ------------------------------------------------------------------------------------------------------------------

static void start(HMODULE self) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
    InitializeCriticalSection(&g_log_cs);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wcscpy_s(g_dir, path);
    if (wchar_t *s = wcsrchr(g_dir, L'\\')) *s = 0;
    swprintf_s(g_fgdir, L"%s\\DLSSFG", g_dir);
    wchar_t logp[MAX_PATH];
    wcscpy_s(logp, path);
    if (wchar_t *dot = wcsrchr(logp, L'.')) wcscpy_s(dot, MAX_PATH - (dot - logp), L".log");
    g_log = _wfsopen(logp, L"a", _SH_DENYNO);

    FILETIME ct, et, kt, ut, nowft;
    GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
    GetSystemTimeAsFileTime(&nowft);
    double since = double(((ULARGE_INTEGER &)nowft).QuadPart - ((ULARGE_INTEGER &)ct).QuadPart) / 1e7;
    dlog("===== DLSSFG %s loaded, pid %lu, %.3f s after process start =====", DLSSFG_BUILD, GetCurrentProcessId(),
         since);
    dlog("init: vulkan-1.dll already loaded: %s, nvngx.dll loaded: %s, nvngx_dlssg.dll in DLSSFG folder: %s",
         GetModuleHandleW(L"vulkan-1.dll") ? "YES (too late?)" : "no", GetModuleHandleW(L"nvngx.dll") ? "yes" : "no",
         [] {
             wchar_t p[MAX_PATH];
             swprintf_s(p, L"%s\\nvngx_dlssg.dll", g_fgdir);
             return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES ? "yes" : "NO";
         }());

    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\DLSSFG.ini", g_dir);
    g_fg_enabled = GetPrivateProfileIntW(L"DLSSFG", L"Enabled", 1, ini) != 0;
    g_fg_pacing = GetPrivateProfileIntW(L"DLSSFG", L"Pacing", 1, ini) != 0;
    dlog("init: DLSSFG.ini Enabled=%d Pacing=%d%s", g_fg_enabled, g_fg_pacing,
         GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES ? " (no ini file, default)" : "");

    if (sh::Load()) {
        sh::scriptRegister(self, camera_script);
        dlog("init: camera script registered with ScriptHookRDR2");
    } else {
        dlog("init: ScriptHookRDR2.dll not found: no camera data (frame generation still runs, with less information)");
    }

    HMODULE exe = GetModuleHandleW(NULL);
    bool a = patch_iat(exe, "KERNEL32.dll", "GetProcAddress", (void *)hk_GetProcAddress, (void **)&o_GetProcAddress);
    bool b = patch_iat(exe, "KERNEL32.dll", "LoadLibraryW", (void *)hk_LoadLibraryW, (void **)&o_LoadLibraryW);
    bool c = patch_iat(exe, "KERNEL32.dll", "LoadLibraryExW", (void *)hk_LoadLibraryExW, (void **)&o_LoadLibraryExW);
    bool d = patch_iat(exe, "KERNEL32.dll", "LoadLibraryA", (void *)hk_LoadLibraryA, (void **)&o_LoadLibraryA);
    if (!o_GetProcAddress) o_GetProcAddress = GetProcAddress;
    dlog("init: RDR2.exe import hooks: GetProcAddress %d, LoadLibraryW %d, LoadLibraryExW %d, LoadLibraryA %d", a, b,
         c, d);

    bool r1 = patch_iat(exe, "NvLowLatencyVk.dll", "NvLL_VK_Initialize", (void *)hk_nvll_init, (void **)&o_nvll_init);
    bool r2 = patch_iat(exe, "NvLowLatencyVk.dll", "NvLL_VK_InitLowLatencyDevice", (void *)hk_nvll_initdev,
                        (void **)&o_nvll_initdev);
    bool r3 = patch_iat(exe, "NvLowLatencyVk.dll", "NvLL_VK_SetSleepMode", (void *)hk_nvll_sleepmode,
                        (void **)&o_nvll_sleepmode);
    bool r4 = patch_iat(exe, "NvLowLatencyVk.dll", "NvLL_VK_SetLatencyMarker", (void *)hk_nvll_marker,
                        (void **)&o_nvll_marker);
    bool r5 = patch_iat(exe, "NvLowLatencyVk.dll", "NvLL_VK_Sleep", (void *)hk_nvll_sleep, (void **)&o_nvll_sleep);
    dlog("init: Reflex import hooks: Initialize %d InitLowLatencyDevice %d SetSleepMode %d SetLatencyMarker %d Sleep %d",
         r1, r2, r3, r4, r5);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        start(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (sh::scriptUnregister) sh::scriptUnregister(inst);
        dlog("===== DLSSFG unloading (DLSS evals %ld) =====", g_dlss_evals);
    }
    return TRUE;
}
