// DLSS Frame Generation (2x) for RDR2's Vulkan renderer.
//
// Inputs come from what the game already gives its own DLSS Super Resolution:
//   - fg_after_dlss_evaluate (the game's render workers, right after its DLSS evaluate is recorded): copies the depth
//     and motion vectors DLSS just read into one of our slots, in the same command buffer. The game reuses those images
//     every frame, so they would be overwritten before the present otherwise.
//   - fg_submitted (the game's vkQueueSubmit): marks a slot once the command buffer holding its copy goes to the GPU.
//   - fg_present (the game's present thread, inside its vkQueuePresentKHR): takes the newest submitted slot, copies the
//     finished frame (with HUD) out of the swapchain image, runs NGX Frame Generation, acquires a second swapchain image,
//     writes the generated frame into it, then presents the generated frame followed by the game's frame. With FIFO
//     (V-sync) the presentation engine paces them one refresh apart.
// The camera for the presented frame comes from the ScriptHook script, matched by the game's Reflex frame IDs. Which
// frame offset and screen-axis convention fit is checked against the game's own motion vectors (a few pixels read
// back now and then) and chosen automatically.
// A present without a slot (menus, loading screens: DLSS isn't running) goes through untouched, and the next generated
// frame starts NGX's history over. Insert toggles frame generation.
#include "common.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

static const char *const k_project_id = "8b6f2e3d-41a7-4c09-b8d2-5e7a1c9f0d34", *const k_engine_version = "DLSSFG-RDR2-1";

SRWLOCK g_acquire_lock = SRWLOCK_INIT;
SRWLOCK g_queue_lock = SRWLOCK_INIT;
VkQueue volatile g_present_queue;

static double now_ms() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}

// ------------------------------------------------------------------------------------------------------------------
// Device functions (straight from the driver via vkGetDeviceProcAddr: they bypass our own hooks)
// ------------------------------------------------------------------------------------------------------------------

static struct {
    VkDevice dev;
    VkPhysicalDevice phys;
    bool has_bda_khr;
    VkPhysicalDeviceMemoryProperties mem;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
#define DEVFN(n) PFN_vk##n n;
#define DEVFNS                                                                                                       \
    DEVFN(CreateImage) DEVFN(DestroyImage) DEVFN(GetImageMemoryRequirements) DEVFN(AllocateMemory) DEVFN(FreeMemory)   \
    DEVFN(BindImageMemory) DEVFN(CreateImageView) DEVFN(DestroyImageView) DEVFN(CmdPipelineBarrier)                    \
    DEVFN(CmdCopyImage) DEVFN(CmdBlitImage) DEVFN(CreateCommandPool) DEVFN(AllocateCommandBuffers)                     \
    DEVFN(BeginCommandBuffer) DEVFN(EndCommandBuffer) DEVFN(ResetCommandBuffer) DEVFN(QueueSubmit)                     \
    DEVFN(CreateSemaphore) DEVFN(CreateFence) DEVFN(WaitForFences) DEVFN(ResetFences) DEVFN(GetFenceStatus)            \
    DEVFN(AcquireNextImageKHR) DEVFN(QueueWaitIdle) DEVFN(DeviceWaitIdle) DEVFN(CreateBuffer) DEVFN(DestroyBuffer)     \
    DEVFN(GetBufferMemoryRequirements) DEVFN(BindBufferMemory) DEVFN(MapMemory) DEVFN(CmdCopyImageToBuffer)          \
    DEVFN(CreateQueryPool) DEVFN(CmdResetQueryPool) DEVFN(CmdWriteTimestamp) DEVFN(GetQueryPoolResults)
    DEVFNS
#undef DEVFN
    double ts_period;   // ns per timestamp tick
    bool ok;
} D;

static uint32_t g_queue_family[64];
static VkQueue g_queues[64];
static int g_nqueues;

void fg_queue(VkQueue q, uint32_t family) {
    for (int i = 0; i < g_nqueues; i++)
        if (g_queues[i] == q) return;
    if (g_nqueues < 64) g_queues[g_nqueues] = q, g_queue_family[g_nqueues++] = family;
}

static int queue_family(VkQueue q) {
    for (int i = 0; i < g_nqueues; i++)
        if (g_queues[i] == q) return (int)g_queue_family[i];
    return -1;
}

void fg_device_created(VkDevice dev, VkPhysicalDevice phys, PFN_vkGetInstanceProcAddr gipa,
                       PFN_vkGetDeviceProcAddr gdpa, bool has_bda_khr) {
    memset(&D, 0, sizeof D);
    D.dev = dev, D.phys = phys, D.has_bda_khr = has_bda_khr;
    auto mp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    D.GetPhysicalDeviceFormatProperties =
        (PFN_vkGetPhysicalDeviceFormatProperties)gipa(g_instance, "vkGetPhysicalDeviceFormatProperties");
    if (mp) mp(phys, &D.mem);
    auto pp = (PFN_vkGetPhysicalDeviceProperties)gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (pp) {
        VkPhysicalDeviceProperties props;
        pp(phys, &props);
        D.ts_period = props.limits.timestampPeriod;
    }
    bool ok = mp && D.GetPhysicalDeviceFormatProperties;
#define DEVFN(n)                                                                                                     \
    D.n = (PFN_vk##n)gdpa(dev, "vk" #n);                                                                             \
    if (!D.n) dlog("fg: device function vk" #n " missing"), ok = false;
    DEVFNS
#undef DEVFN
    D.ok = ok;
    dlog("fg: device functions %s, VK_KHR_buffer_device_address %s", ok ? "ready" : "MISSING",
         has_bda_khr ? "enabled" : "NOT enabled (frame generation will be off)");
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < D.mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (D.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// ------------------------------------------------------------------------------------------------------------------
// Images
// ------------------------------------------------------------------------------------------------------------------

struct Img {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
};

static void free_img(Img &m) {
    if (m.view) D.DestroyImageView(D.dev, m.view, NULL);
    if (m.image) D.DestroyImage(D.dev, m.image, NULL);
    if (m.mem) D.FreeMemory(D.dev, m.mem, NULL);
    memset(&m, 0, sizeof m);
}

static bool make_img(Img &m, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    if (m.image && m.w == w && m.h == h && m.format == f) return true;
    free_img(m);
    bool depth = f == VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (D.CreateImage(D.dev, &ci, NULL, &m.image) != VK_SUCCESS) {
        m.image = VK_NULL_HANDLE;
        dlog("fg: can't create %ux%u image of format %d", w, h, f);
        return false;
    }
    VkMemoryRequirements req;
    D.GetImageMemoryRequirements(D.dev, m.image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || D.AllocateMemory(D.dev, &ai, NULL, &m.mem) != VK_SUCCESS ||
        D.BindImageMemory(D.dev, m.image, m.mem, 0) != VK_SUCCESS) {
        dlog("fg: can't allocate %llu bytes for a %ux%u image", (unsigned long long)req.size, w, h);
        free_img(m);
        return false;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    if (D.CreateImageView(D.dev, &vi, NULL, &m.view) != VK_SUCCESS) {
        dlog("fg: can't create an image view (format %d)", f);
        free_img(m);
        return false;
    }
    m.format = f, m.w = w, m.h = h;
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
}

// ------------------------------------------------------------------------------------------------------------------
// Slots: per-frame copies of the game's DLSS inputs, handed from the render workers to the present thread
// ------------------------------------------------------------------------------------------------------------------

#define NSLOTS 8

struct Slot {
    Img depth, mvec;
    Camera cam;          // the latest camera when the copy was recorded (fallback without Reflex frame IDs)
    bool reset, depth_inverted;
    uint32_t rw, rh;     // the render size (DLSS's input subrect)
    VkCommandBuffer cmd; // the game's command buffer holding the copy
    bool submitted;      // that command buffer went to a queue
};

static Slot S[NSLOTS];
static int s_next;
static int s_pending[NSLOTS], s_npending;   // in recording order
static SRWLOCK s_lock = SRWLOCK_INIT;
static int g_dlss_flags = -1;
static volatile LONG g_fg_dead;    // frame generation off for the session (NGX can't do it here)
static volatile LONG g_dropped;    // slots never presented (overrun, or older than the presented one)
static bool g_submit_seen;         // a slot's command buffer was seen in a vkQueueSubmit
static uint64_t g_nosubmit_presents;
static bool g_fifo_fallback;       // no submits ever matched: take slots in order

void fg_dlss_created(int flags) {
    g_dlss_flags = flags;
    dlog("fg: game DLSS created, flags %#x: depth %s, motion vectors %s, %s", flags,
         flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted ? "inverted (near = 1)" : "not inverted",
         flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes ? "at render resolution" : "at output resolution",
         flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered ? "jittered" : "not jittered");
}

static bool get_res(const NVSDK_NGX_Parameter *p, const char *name, NVSDK_NGX_Resource_VK *out) {
    void *v = NULL;
    if (p->Get(name, &v) != NVSDK_NGX_Result_Success || !v) return false;
    *out = *(const NVSDK_NGX_Resource_VK *)v;
    return out->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW && out->Resource.ImageViewInfo.Image;
}

static void remove_pending(int pos) {
    memmove(&s_pending[pos], &s_pending[pos + 1], (s_npending - pos - 1) * sizeof(int));
    s_npending--;
}

void fg_after_dlss_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Parameter *p) {
    if (!g_fg_enabled || g_fg_dead || !D.ok || !D.has_bda_khr) return;
    NVSDK_NGX_Resource_VK depth, mv;
    if (!get_res(p, NVSDK_NGX_Parameter_Depth, &depth) || !get_res(p, NVSDK_NGX_Parameter_MotionVectors, &mv)) {
        static int n;
        if (n++ < 3) dlog("fg: the game's DLSS evaluate has no depth or motion vectors, skipping");
        return;
    }
    unsigned rw = 0, rh = 0;
    int reset = 0;
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &rw);
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &rh);
    p->Get(NVSDK_NGX_Parameter_Reset, &reset);
    const auto &di = depth.Resource.ImageViewInfo, &mi = mv.Resource.ImageViewInfo;
    if (!rw || !rh) rw = di.Width, rh = di.Height;

    AcquireSRWLockExclusive(&s_lock);
    int idx = s_next;
    s_next = (s_next + 1) % NSLOTS;
    for (int i = 0; i < s_npending; i++)   // (only if the present thread fell NSLOTS frames behind)
        if (s_pending[i] == idx) {
            remove_pending(i);
            InterlockedIncrement(&g_dropped);
            break;
        }
    Slot &s = S[idx];
    bool ok = make_img(s.depth, di.Width, di.Height, VK_FORMAT_D32_SFLOAT,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
              make_img(s.mvec, mi.Width, mi.Height, mi.Format,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (ok) {
        // The game's images are in the layout DLSS read them in (we declare GENERAL; the driver treats read layouts
        // alike). They have TRANSFER_SRC from our vkCreateImage hook.
        full_barrier(cmd);
        barrier(cmd, s.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier(cmd, s.mvec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c = {};
        c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, di.SubresourceRange.baseMipLevel, di.SubresourceRange.baseArrayLayer, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.extent = {di.Width, di.Height, 1};
        D.CmdCopyImage(cmd, di.Image, VK_IMAGE_LAYOUT_GENERAL, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mi.SubresourceRange.baseMipLevel, mi.SubresourceRange.baseArrayLayer, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {mi.Width, mi.Height, 1};
        D.CmdCopyImage(cmd, mi.Image, VK_IMAGE_LAYOUT_GENERAL, s.mvec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cmd, s.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        barrier(cmd, s.mvec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        full_barrier(cmd);   // the game's next writes to its depth / vectors wait for our reads

        if (!camera_get(&s.cam)) s.cam.valid = false;
        s.reset = reset != 0;
        s.depth_inverted = g_dlss_flags < 0 || (g_dlss_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
        s.rw = rw, s.rh = rh;
        s.cmd = cmd;
        s.submitted = false;
        if (s_npending == NSLOTS - 1) {
            remove_pending(0);
            InterlockedIncrement(&g_dropped);
        }
        s_pending[s_npending++] = idx;
        static bool logged;
        if (!logged) {
            logged = true;
            dlog("fg: first input copy: depth %ux%u (format %d), vectors %ux%u (format %d), render %ux%u, camera %s",
                 di.Width, di.Height, di.Format, mi.Width, mi.Height, mi.Format, rw, rh,
                 s.cam.valid ? "ok" : "not available yet");
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void fg_submitted(const VkCommandBuffer *cbs, uint32_t n) {
    if (!s_npending) return;   // (racy read: harmless, the next submit catches up)
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_npending; i++) {
        Slot &s = S[s_pending[i]];
        if (s.submitted) continue;
        for (uint32_t k = 0; k < n; k++)
            if (cbs[k] == s.cmd) {
                s.submitted = true;
                g_submit_seen = true;
                break;
            }
    }
    ReleaseSRWLockExclusive(&s_lock);
}

// The slot for this present: the newest one whose copy is already on the GPU. Older ones were never presented
// (dropped); ones not submitted yet belong to later frames and stay.
static uint64_t g_pick_hist[4];   // how many submitted slots were waiting at a present: 0, 1, 2, 3+

static int pop_slot() {
    AcquireSRWLockExclusive(&s_lock);
    int pos = -1, nsub = 0;
    for (int i = 0; i < s_npending; i++)
        if (S[s_pending[i]].submitted) pos = i, nsub++;
    if (pos < 0 && s_npending && !g_submit_seen && ++g_nosubmit_presents > 300) {
        if (!g_fifo_fallback) {
            g_fifo_fallback = true;
            dlog("fg: never saw a slot's command buffer submitted; taking slots in order instead");
        }
        pos = 0, nsub = 1;
    }
    g_pick_hist[nsub < 3 ? nsub : 3]++;
    int idx = -1;
    if (pos >= 0) {
        idx = s_pending[pos];
        for (int i = 0; i < pos; i++) InterlockedIncrement(&g_dropped);
        memmove(&s_pending[0], &s_pending[pos + 1], (s_npending - pos - 1) * sizeof(int));
        s_npending -= pos + 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return idx;
}

// ------------------------------------------------------------------------------------------------------------------
// Swapchain
// ------------------------------------------------------------------------------------------------------------------

static struct {
    VkSwapchainKHR sc;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    bool usable;   // has TRANSFER_SRC | TRANSFER_DST
} SC;
static bool refresh_changed = true;   // look the display refresh up again (new swapchain: maybe a new mode)

void fg_swapchain_created(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR *ci) {
    SC.sc = sc;
    SC.format = ci->imageFormat;
    SC.extent = ci->imageExtent;
    SC.mode = ci->presentMode;
    VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    SC.usable = (ci->imageUsage & need) == need;
    dlog("fg: swapchain %p %ux%u format %d mode %d, %s", (void *)sc, SC.extent.width, SC.extent.height, SC.format,
         SC.mode, SC.usable ? "usable" : "NOT usable (no transfer usage)");
    if (SC.mode != VK_PRESENT_MODE_FIFO_KHR && SC.mode != VK_PRESENT_MODE_FIFO_RELAXED_KHR)
        dlog("fg: present mode isn't V-sync (FIFO)");
    refresh_changed = true;
}

void fg_swapchain_destroyed(VkSwapchainKHR sc) {
    if (SC.sc == sc) SC.sc = VK_NULL_HANDLE, SC.usable = false;
}

// ------------------------------------------------------------------------------------------------------------------
// Camera matrices (row-major, column vectors: p' = M p; transposed for NGX, which takes row vectors)
// ------------------------------------------------------------------------------------------------------------------

static const double DEG = 3.14159265358979323846 / 180.0;
static const double k_near = 0.1, k_far = 10000.0;   // no native gives the clip planes; rotation doesn't depend on them

static void basis(const Camera &c, double r[3], double u[3], double f[3]) {
    double p = c.rot[0] * DEG, rl = c.rot[1] * DEG, y = c.rot[2] * DEG;
    f[0] = -sin(y) * cos(p), f[1] = cos(y) * cos(p), f[2] = sin(p);   // RAGE: z up, yaw 0 looks along +y
    double r0[3] = {cos(y), sin(y), 0};
    double u0[3] = {r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0]};
    for (int k = 0; k < 3; k++) r[k] = r0[k] * cos(rl) + u0[k] * sin(rl);
    u[0] = r[1] * f[2] - r[2] * f[1], u[1] = r[2] * f[0] - r[0] * f[2], u[2] = r[0] * f[1] - r[1] * f[0];
}

static void view_matrix(const Camera &c, double v[16]) {
    double r[3], u[3], f[3];
    basis(c, r, u, f);
    const float *e = c.pos;
    double rows[3][3] = {{r[0], r[1], r[2]}, {u[0], u[1], u[2]}, {-f[0], -f[1], -f[2]}};
    for (int i = 0; i < 3; i++) {
        v[i * 4 + 0] = rows[i][0], v[i * 4 + 1] = rows[i][1], v[i * 4 + 2] = rows[i][2];
        v[i * 4 + 3] = -(rows[i][0] * e[0] + rows[i][1] * e[1] + rows[i][2] * e[2]);
    }
    v[12] = v[13] = v[14] = 0, v[15] = 1;
}

static void mul4(const double a[16], const double b[16], double o[16]) {
    double t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = s;
        }
    memcpy(o, t, sizeof t);
}

static bool inv4(const double m[16], double o[16]) {   // Gauss-Jordan, partial pivoting
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double s = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= s * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][c + 4];
    return true;
}

static void to_ngx(float out[4][4], const double m[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = (float)m[c * 4 + r];
}

static double cam_fov(const Camera &c) { return (c.fov > 1 && c.fov < 170 ? c.fov : 50.0) * DEG; }
static double scaled_fov(const Camera &c, double scale) { return 2 * atan(tan(cam_fov(c) / 2) * scale); }

// The projection (no jitter). flip: clip y the other way.
static void proj_matrix(double fov, double aspect, bool inverted, bool flip, double P[16]) {
    double n = k_near, F = k_far, fy = 1.0 / tan(fov / 2);
    double p[16] = {fy / aspect, 0, 0, 0, 0, flip ? -fy : fy, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0};
    if (inverted) p[10] = n / (F - n), p[11] = F * n / (F - n);   // reversed Z: near -> 1, far -> 0
    else p[10] = -F / (F - n), p[11] = -F * n / (F - n);
    memcpy(P, p, sizeof p);
}

// clip -> view -> world -> previous view -> previous clip
static bool clip_to_prev(const Camera &cur, const Camera &prev, const double P[16], double M[16]) {
    double V[16], Vp[16], Vi[16], Pi[16];
    view_matrix(cur, V);
    view_matrix(prev, Vp);
    if (!inv4(V, Vi) || !inv4(P, Pi)) return false;
    mul4(P, Vp, M);
    mul4(M, Vi, M);
    mul4(M, Pi, M);
    return true;
}

// A camera cut: over 10 m in a frame, or the view turned more than ~35 degrees.
static bool camera_cut(const Camera &a, const Camera &b) {
    if (!a.valid || !b.valid) return false;
    double d = 0;
    for (int k = 0; k < 3; k++) d += (a.pos[k] - b.pos[k]) * (a.pos[k] - b.pos[k]);
    double ra[3], ua[3], fa[3], rb[3], ub[3], fb[3];
    basis(a, ra, ua, fa);
    basis(b, rb, ub, fb);
    return d > 100 || fa[0] * fb[0] + fa[1] * fb[1] + fa[2] * fb[2] < 0.82;
}

// ------------------------------------------------------------------------------------------------------------------
// Camera check: does our camera reproduce the game's motion vectors? Six candidates: the camera of the presented
// frame's Reflex ID shifted by -1/0/+1, times clip y up/down. A few pixels of depth and vectors are read back every
// few frames; the candidate whose reprojection matches best (on frames where the camera moves) is used. If none
// matches, the camera matrices are left out (identity reprojection) instead of misleading NGX.
// ------------------------------------------------------------------------------------------------------------------

#define NPROBE 12
#define NCAND 10   // shift -1..+3, flip 0/1
static const float k_probe_x[4] = {0.08f, 0.25f, 0.75f, 0.92f}, k_probe_y[3] = {0.12f, 0.35f, 0.60f};

struct Probe {
    bool active;
    uint64_t frame;   // Reflex frame ID of the presented frame
    uint32_t avail;   // candidates whose camera pair existed at the present (usable live)
    uint32_t rw, rh;
    bool inverted;
    double aspect;
    uint32_t px[NPROBE], py[NPROBE];
};

static struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    double err[NCAND], base;
    int cnt[NCAND], samples, total_samples;
    int chosen;        // candidate in use: (shift + 1) * 2 + flip
    bool decided, use_camera;
    // second stage, once the shift is chosen: the field of view's scale (tan(fov/2) times this)
    double ferr[7];
    int fsamples;
    double fov_scale;
    bool fov_decided;
} C = {VK_NULL_HANDLE, VK_NULL_HANDLE, NULL, {}, 0, {}, 0, 0, 4, false, true, {}, 0, 1.0, false};   // until checked: shift +1
static const double k_fov_scale[7] = {0.7, 0.8, 0.9, 1.0, 1.1, 1.25, 1.4};

static int cand_shift(int c) { return c / 2 - 1; }
static bool cand_flip(int c) { return c & 1; }

static float half_to_float(uint16_t h) {
    uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : ldexpf((float)(m | 1024), (int)e - 25);
    return h & 0x8000 ? -v : v;
}

static bool probe_init() {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 8 * 128;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (D.CreateBuffer(D.dev, &bi, NULL, &C.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    D.GetBufferMemoryRequirements(D.dev, C.buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void *p = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || D.AllocateMemory(D.dev, &ai, NULL, &C.mem) != VK_SUCCESS ||
        D.BindBufferMemory(D.dev, C.buf, C.mem, 0) != VK_SUCCESS || D.MapMemory(D.dev, C.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return false;
    C.map = (uint8_t *)p;
    return true;
}

static void probe_record(VkCommandBuffer cb, int ring, const Slot &s, Probe &pr, uint64_t frame, uint32_t avail) {
    pr.active = true;
    pr.frame = frame;
    pr.avail = avail;
    pr.rw = s.rw, pr.rh = s.rh;
    pr.inverted = s.depth_inverted;
    pr.aspect = (double)SC.extent.width / SC.extent.height;
    VkBufferImageCopy rd[NPROBE] = {}, rm[NPROBE] = {};
    for (int i = 0; i < NPROBE; i++) {
        pr.px[i] = (uint32_t)(k_probe_x[i % 4] * s.rw), pr.py[i] = (uint32_t)(k_probe_y[i / 4] * s.rh);
        rd[i].bufferOffset = ring * 128 + i * 4;
        rd[i].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        rd[i].imageOffset = {(int32_t)pr.px[i], (int32_t)pr.py[i], 0};
        rd[i].imageExtent = {1, 1, 1};
        rm[i] = rd[i];
        rm[i].bufferOffset = ring * 128 + 64 + i * 4;
        rm[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    full_barrier(cb);
    barrier(cb, s.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, s.mvec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    D.CmdCopyImageToBuffer(cb, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, C.buf, NPROBE, rd);
    D.CmdCopyImageToBuffer(cb, s.mvec.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, C.buf, NPROBE, rm);
    barrier(cb, s.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(cb, s.mvec.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    full_barrier(cb);
}

// Predicted vector (pixels, current -> previous) for pixel (x, y) at depth d, with screen row 0 at clip y = +1.
static bool predict(const double M[16], double x, double y, double d, uint32_t rw, uint32_t rh, double *mx, double *my) {
    double c[4] = {2 * (x + 0.5) / rw - 1, 1 - 2 * (y + 0.5) / rh, d, 1}, p[4];
    for (int r = 0; r < 4; r++) p[r] = M[r * 4] * c[0] + M[r * 4 + 1] * c[1] + M[r * 4 + 2] * c[2] + M[r * 4 + 3] * c[3];
    if (fabs(p[3]) < 1e-9) return false;
    double X = (p[0] / p[3] + 1) / 2 * rw - 0.5, Y = (1 - p[1] / p[3]) / 2 * rh - 0.5;
    *mx = X - x, *my = Y - y;
    return true;
}

static void probe_read(int ring, Probe &pr) {
    if (!pr.active) return;
    pr.active = false;
    float depth[NPROBE], mvx[NPROBE], mvy[NPROBE];
    memcpy(depth, C.map + ring * 128, sizeof depth);
    double motion = 0;
    for (int i = 0; i < NPROBE; i++) {
        uint16_t h[2];
        memcpy(h, C.map + ring * 128 + 64 + i * 4, 4);
        mvx[i] = half_to_float(h[0]), mvy[i] = half_to_float(h[1]);
        motion += sqrt((double)mvx[i] * mvx[i] + (double)mvy[i] * mvy[i]);
    }
    motion /= NPROBE;
    if (motion < 2.0 || motion > 300) return;   // the camera has to move (and not be a cut)

    // Field-of-view stage (after the shift is chosen)
    if (C.decided && C.use_camera && !C.fov_decided && (pr.avail & (1u << C.chosen))) {
        Camera cur, prev;
        uint64_t f = pr.frame + cand_shift(C.chosen);
        if (camera_get_frame(f, &cur) && camera_get_frame(f - 1, &prev)) {
            double e[7];
            bool ok = true;
            for (int k = 0; k < 7 && ok; k++) {
                double P[16], M[16];
                proj_matrix(scaled_fov(cur, k_fov_scale[k]), pr.aspect, pr.inverted, cand_flip(C.chosen), P);
                ok = clip_to_prev(cur, prev, P, M);
                e[k] = 0;
                for (int i = 0; i < NPROBE && ok; i++) {
                    double px, py;
                    ok = predict(M, pr.px[i], pr.py[i], depth[i], pr.rw, pr.rh, &px, &py);
                    e[k] += sqrt((px - mvx[i]) * (px - mvx[i]) + (py - mvy[i]) * (py - mvy[i])) / NPROBE;
                }
            }
            if (ok) {
                for (int k = 0; k < 7; k++) C.ferr[k] += e[k];
                if (++C.fsamples >= 40) {
                    int best = 3;
                    for (int k = 0; k < 7; k++)
                        if (C.ferr[k] < C.ferr[best]) best = k;
                    char line[256];
                    int n = 0;
                    for (int k = 0; k < 7; k++)
                        n += sprintf_s(line + n, sizeof line - n, " x%.2f %.1f", k_fov_scale[k], C.ferr[k] / C.fsamples);
                    C.fov_decided = true;
                    if (C.ferr[best] < 0.9 * C.ferr[3]) C.fov_scale = k_fov_scale[best];
                    dlog("fg: field-of-view check over %d moving frames, avg error by scale:%s; using x%.2f", C.fsamples,
                         line, C.fov_scale);
                }
            }
        }
    }

    double err[NCAND];
    bool any = false;
    for (int c = 0; c < NCAND; c++) {
        err[c] = -1;
        if (!(pr.avail & (1u << c))) continue;   // its camera wasn't there yet at the present: can't be used live
        Camera cur, prev;
        uint64_t f = pr.frame + cand_shift(c);
        if (!camera_get_frame(f, &cur) || !camera_get_frame(f - 1, &prev)) continue;
        double P[16], M[16];
        proj_matrix(scaled_fov(cur, C.fov_scale), pr.aspect, pr.inverted, cand_flip(c), P);
        if (!clip_to_prev(cur, prev, P, M)) continue;
        double e = 0;
        bool ok = true;
        for (int i = 0; i < NPROBE && ok; i++) {
            double px, py;
            ok = predict(M, pr.px[i], pr.py[i], depth[i], pr.rw, pr.rh, &px, &py);
            e += sqrt((px - mvx[i]) * (px - mvx[i]) + (py - mvy[i]) * (py - mvy[i]));
        }
        if (!ok) continue;
        err[c] = e / NPROBE;
        C.err[c] += err[c];
        C.cnt[c]++;
        any = true;
    }
    if (!any) return;
    C.base += motion;
    C.samples++, C.total_samples++;
    char line[512];
    if (C.total_samples <= 3) {
        int n = 0;
        for (int c = 0; c < NCAND; c++)
            n += sprintf_s(line + n, sizeof line - n, " %+d/%d %.1f", cand_shift(c), (int)cand_flip(c), err[c]);
        dlog("fg: camera check sample: game vectors avg %.1f px; error by shift/flip (-1 = not available):%s", motion, line);
    }
    if (C.samples >= 40) {
        int best = -1, n = 0;
        for (int c = 0; c < NCAND; c++) {
            double m = C.cnt[c] ? C.err[c] / C.cnt[c] : -1;
            n += sprintf_s(line + n, sizeof line - n, " %+d/%d %.1f (%d)", cand_shift(c), (int)cand_flip(c), m, C.cnt[c]);
            if (C.cnt[c] >= C.samples / 2 && (best < 0 || m < C.err[best] / C.cnt[best])) best = c;
        }
        double ratio = best >= 0 ? (C.err[best] / C.cnt[best]) / (C.base / C.samples) : 1;
        dlog("fg: camera check over %d moving frames: avg game vectors %.1f px; avg error by shift/flip (samples):%s; "
             "best shift %d flip %d (error %.0f%% of the motion)",
             C.samples, C.base / C.samples, line, best >= 0 ? cand_shift(best) : 0, best >= 0 ? (int)cand_flip(best) : 0,
             ratio * 100);
        if (!C.decided) {
            C.decided = true;
            if (best >= 0 && ratio < 0.85) {   // (translation errors from the guessed clip planes push this up)
                C.chosen = best, C.use_camera = true;
                dlog("fg: using camera shift %d, flip %d", cand_shift(best), cand_flip(best));
            } else {
                C.use_camera = false;
                C.fov_decided = true;
                dlog("fg: no camera candidate matches the game's motion; leaving camera motion out of the matrices");
            }
        }
        memset(C.err, 0, sizeof C.err);
        memset(C.cnt, 0, sizeof C.cnt);
        C.base = 0;
        C.samples = 0;
    }
}

// ------------------------------------------------------------------------------------------------------------------
// NGX
// ------------------------------------------------------------------------------------------------------------------

#define NRING 4

static struct {
    bool tried, ok;
    NVSDK_NGX_Parameter *caps, *params;
    NVSDK_NGX_Handle *feature;
    uint32_t w, h, rw, rh;
    NVSDK_NGX_FeatureCommonInfo common;
    const wchar_t *paths[2];
    // present-thread resources
    int family;
    VkCommandPool pool;
    VkCommandBuffer cb[NRING];
    VkFence fence[NRING];
    VkSemaphore acq[NRING], gen_done[NRING], real_done[NRING];
    bool fence_used[NRING];
    Probe probe[NRING];
    int ring;
    Img real, gen;
    // history
    Camera prev;
    bool have_prev;
    int fails;
    uint64_t gen_count;
    // user toggle (Insert)
    bool user_off, key_down;
    // stats
    double t_log, last_present, cpu_sum, cpu_max;
    uint64_t n_gen, n_plain, n_fail, n_cpu, interval_hist[3], cam_fallbacks;
    // hitch breakdown
    double last_work, fence_wait, acq_wait;
    bool last_generated;
    int hitches_logged;
    // GPU timestamps of our frame generation submission (start, end per ring entry)
    VkQueryPool qpool;
    bool ts_used[NRING];
    uint64_t prev_ts_start;
    double gpu_last, period_last, gpu_sum, gpu_max, period_max;
    uint64_t gpu_n, period_slow;
    // pacing
    int refresh;               // display refresh in Hz (0: unknown)
    double frame_ema;          // the game's frame interval, smoothed
    double last_real_target;   // when the last game frame is (or was) due on screen
    double pace_sum, pace_max;
    uint64_t pace_n;
} N;

static int refresh_hz() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (int)dm.dmDisplayFrequency;
    return 0;
}

// Sleep to a moment (now_ms() clock) with sub-millisecond precision: a high-resolution timer, then a short spin.
static void sleep_until(double target) {
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        double rem = target - now_ms();
        if (rem <= 0 || rem > 100) return;
        if (rem > 1.2 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((rem - 1.0) * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100);
            else Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------
// The pacer thread: presents the generated and the game's frame at their times, so the game's present thread (which
// also submits its next frame) never sleeps for pacing. The game thread only waits when it runs ahead of the pacing
// (that wait is the frame cap).
// ------------------------------------------------------------------------------------------------------------------

struct PJob {
    VkQueue q;
    VkSwapchainKHR sc;
    uint32_t index;
    VkSemaphore wait;
    double target;   // now_ms() time to present at
    bool real;
};

static struct {
    HANDLE thread;
    SRWLOCK lock;
    CONDITION_VARIABLE cv, done;
    PJob jobs[8];
    int head, count;   // count includes the job being presented
    PFN_vkQueuePresentKHR present;
    volatile LONG result;   // a failed present's VkResult (handed to the game once), else VK_SUCCESS
    double late_sum, late_max, block_sum, block_max;
    uint64_t late_n, block_n;
} PC = {NULL, SRWLOCK_INIT, CONDITION_VARIABLE_INIT, CONDITION_VARIABLE_INIT};

static DWORD WINAPI pacer_main(void *) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        AcquireSRWLockExclusive(&PC.lock);
        while (!PC.count) SleepConditionVariableSRW(&PC.cv, &PC.lock, INFINITE, 0);
        PJob j = PC.jobs[PC.head];
        ReleaseSRWLockExclusive(&PC.lock);
        if (j.target > now_ms()) sleep_until(j.target);
        double late = now_ms() - j.target;
        if (late > 0) {
            PC.late_sum += late, PC.late_n++;
            if (late > PC.late_max) PC.late_max = late;
        }
        VkPresentInfoKHR p = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        p.waitSemaphoreCount = 1;
        p.pWaitSemaphores = &j.wait;
        p.swapchainCount = 1;
        p.pSwapchains = &j.sc;
        p.pImageIndices = &j.index;
        AcquireSRWLockExclusive(&g_queue_lock);
        VkResult r = PC.present(j.q, &p);
        ReleaseSRWLockExclusive(&g_queue_lock);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            InterlockedExchange(&PC.result, (LONG)r);
            static int n;
            if (n++ < 10) dlog("fg: pacer: %s frame present -> %d", j.real ? "game" : "generated", r);
        }
        AcquireSRWLockExclusive(&PC.lock);
        PC.head = (PC.head + 1) % 8;
        PC.count--;
        WakeAllConditionVariable(&PC.done);
        ReleaseSRWLockExclusive(&PC.lock);
    }
}

static void pacer_push(const PJob &j) {
    AcquireSRWLockExclusive(&PC.lock);
    if (PC.count < 8) PC.jobs[(PC.head + PC.count) % 8] = j, PC.count++;
    WakeConditionVariable(&PC.cv);
    ReleaseSRWLockExclusive(&PC.lock);
}

static void pacer_wait_idle(DWORD timeout_ms) {
    AcquireSRWLockExclusive(&PC.lock);
    while (PC.count)
        if (!SleepConditionVariableSRW(&PC.done, &PC.lock, timeout_ms, 0)) {
            dlog("fg: pacer: still %d presents pending after %lu ms", PC.count, timeout_ms);
            break;
        }
    ReleaseSRWLockExclusive(&PC.lock);
}

void fg_flush() {
    if (PC.thread) pacer_wait_idle(500);
}

static const char *ngx_result(NVSDK_NGX_Result r) {
    switch (r) {
    case NVSDK_NGX_Result_Success: return "success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature not supported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "platform error";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "feature not found";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "unable to initialize feature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "out of date (driver too old?)";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "unsupported input format";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "missing input";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "invalid parameter";
    default: return "failure";
    }
}

static bool ngx_ok(NVSDK_NGX_Result r, const char *what) {
    if (r == NVSDK_NGX_Result_Success) return true;
    dlog("fg: NGX %s: %s (%#x)", what, ngx_result(r), (unsigned)r);
    return false;
}

// NGX's own log lines, into DLSSFG.log (capped: verbose NGX can log every frame).
static void NVSDK_CONV ngx_log(const char *msg, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature from) {
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k > 300) return;
    char line[1024];
    strncpy_s(line, msg ? msg : "", _TRUNCATE);
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    dlog("ngx-log[%d/%d]: %s%s", (int)level, (int)from, line, k == 300 ? "  (NGX log capped here)" : "");
}

static bool fg_init(VkQueue q) {
    if (N.tried) return N.ok;
    N.tried = true;
    N.family = queue_family(q);
    if (N.family < 0) { dlog("fg: present queue %p not seen in vkGetDeviceQueue", (void *)q); return false; }
    // nvngx_dlssg.dll: the game folder (NGX's default application folder is RDR2.exe's) and DLSSFG\.
    N.paths[0] = g_dir;
    N.paths[1] = g_fgdir;
    N.common.PathListInfo.Path = N.paths;
    N.common.PathListInfo.Length = 2;
    N.common.LoggingInfo.LoggingCallback = ngx_log;
    N.common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    N.common.LoggingInfo.DisableOtherLoggingSinks = false;
    if (!ngx_ok(NVSDK_NGX_VULKAN_Init_with_ProjectID(k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, k_engine_version,
                                                     g_fgdir, g_instance, D.phys, D.dev, NULL, NULL, &N.common,
                                                     NVSDK_NGX_Version_API), "init") ||
        !ngx_ok(NVSDK_NGX_VULKAN_GetCapabilityParameters(&N.caps), "capabilities"))
        return false;
    int avail = 0;
    NVSDK_NGX_Parameter_GetI(N.caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
    if (!avail) {
        int why = 0;
        NVSDK_NGX_Parameter_GetI(N.caps, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &why);
        dlog("fg: NGX says Frame Generation is unavailable (init result %#x)", (unsigned)why);
        return false;
    }
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = (uint32_t)N.family;
    if (D.CreateCommandPool(D.dev, &pi, NULL, &N.pool) != VK_SUCCESS) { dlog("fg: no command pool"); return false; }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = N.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = NRING;
    if (D.AllocateCommandBuffers(D.dev, &ai, N.cb) != VK_SUCCESS) { dlog("fg: no command buffers"); return false; }
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < NRING; i++)
        if (D.CreateFence(D.dev, &fi, NULL, &N.fence[i]) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &si, NULL, &N.acq[i]) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &si, NULL, &N.gen_done[i]) != VK_SUCCESS ||
            D.CreateSemaphore(D.dev, &si, NULL, &N.real_done[i]) != VK_SUCCESS) {
            dlog("fg: no fences / semaphores");
            return false;
        }
    if (!probe_init()) dlog("fg: no readback buffer: camera check off"), C.decided = C.fov_decided = true;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = NRING * 2;
    if (D.ts_period <= 0 || D.CreateQueryPool(D.dev, &qi, NULL, &N.qpool) != VK_SUCCESS) N.qpool = VK_NULL_HANDLE;
    dlog("fg: NGX Frame Generation available; present queue family %d; Reflex frame IDs %s", N.family,
         reflex_seen() ? "seen" : "NOT seen (camera from the latest script tick)");
    return N.ok = true;
}

static void release_feature() {
    if (N.feature) NVSDK_NGX_VULKAN_ReleaseFeature(N.feature), N.feature = NULL;
    if (N.params) NVSDK_NGX_VULKAN_DestroyParameters(N.params), N.params = NULL;
    N.w = N.h = N.rw = N.rh = 0;
}

static void fill_camera(NVSDK_NGX_DLSSG_Opt_Eval_Params &op, const Camera &cur, const Camera &prev, bool inverted,
                        bool flip, bool use_motion) {
    double a = (double)SC.extent.width / SC.extent.height, fov = scaled_fov(cur, C.fov_scale);
    double P[16], Pi[16], M[16], Mi[16], I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    proj_matrix(fov, a, inverted, flip, P);
    bool ok = use_motion && clip_to_prev(cur, prev, P, M) && inv4(M, Mi);
    if (!ok) memcpy(M, I, sizeof M), memcpy(Mi, I, sizeof Mi);
    if (!inv4(P, Pi)) memcpy(Pi, I, sizeof Pi);
    to_ngx(op.cameraViewToClip, P);
    to_ngx(op.clipToCameraView, Pi);
    to_ngx(op.clipToLensClip, I);
    to_ngx(op.clipToPrevClip, M);
    to_ngx(op.prevClipToClip, Mi);
    double r[3], u[3], f[3];
    basis(cur, r, u, f);
    for (int k = 0; k < 3; k++) {
        op.cameraPos[k] = cur.pos[k];
        op.cameraRight[k] = (float)r[k], op.cameraUp[k] = (float)u[k], op.cameraFwd[k] = (float)f[k];
    }
    op.cameraNear = (float)k_near, op.cameraFar = (float)k_far, op.cameraFOV = (float)fov, op.cameraAspectRatio = (float)a;
}

// ------------------------------------------------------------------------------------------------------------------
// The present
// ------------------------------------------------------------------------------------------------------------------

static void stats(bool generated, bool failed, double t0) {
    if (generated) N.n_gen++;
    else N.n_plain++;
    if (failed) N.n_fail++;
    double now = now_ms(), cpu = now - t0;
    N.cpu_sum += cpu, N.n_cpu++;
    if (cpu > N.cpu_max) N.cpu_max = cpu;
    N.last_work = cpu;
    N.last_generated = generated;
    if (!N.t_log) N.t_log = now;
    if (now - N.t_log >= 10000) {
        double s = (now - N.t_log) / 1000.0;
        LONG an = g_diag.acq_n, sn = g_diag.sleep_n;
        dlog("fg: last %.0f s timing: the game's swapchain acquires avg %.2f ms max %.1f ms, %ld over 4 ms (of %ld); "
             "Reflex sleep avg %.2f ms max %.1f ms (%ld)",
             s, an ? g_diag.acq_sum / an : 0, g_diag.acq_max, g_diag.acq_slow, an, sn ? g_diag.sleep_sum / sn : 0,
             g_diag.sleep_max, sn);
        char mem[160] = "VRAM numbers unavailable";
        diag_memory(mem, sizeof mem);
        dlog("fg: last %.0f s GPU: our frame generation work avg %.2f ms max %.2f ms; GPU frame-to-frame max %.1f ms, "
             "%llu over 25 ms; the game's longest fence wait %.1f ms (present thread) / %.1f ms (others), on a swapchain "
             "image %.1f ms; %s; pacing %s: game thread held back avg %.2f ms max %.1f ms, pacer late avg %.2f ms max "
             "%.1f ms, game frame time %.1f ms",
             s, N.gpu_n ? N.gpu_sum / N.gpu_n : 0, N.gpu_max, N.period_max, N.period_slow, g_diag.pfence_max,
             g_diag.ofence_max, g_diag.afence_max, mem, g_fg_pacing ? "on" : "off", N.pace_n ? N.pace_sum / N.pace_n : 0,
             N.pace_max, PC.late_n ? PC.late_sum / PC.late_n : 0, PC.late_max, N.frame_ema);
        N.gpu_sum = N.gpu_max = N.period_max = N.pace_sum = N.pace_max = 0;
        N.gpu_n = N.period_slow = N.pace_n = 0;
        PC.late_sum = PC.late_max = 0;
        PC.late_n = 0;
        g_diag.pfence_max = g_diag.ofence_max = g_diag.afence_max = 0;
        g_diag.acq_sum = g_diag.acq_max = g_diag.sleep_sum = g_diag.sleep_max = 0;
        g_diag.acq_n = g_diag.acq_slow = g_diag.sleep_n = 0;
        dlog("fg: last %.0f s: %.1f game frames/s with a generated frame, %.1f without, %llu failed, %ld slots dropped; "
             "game frame intervals <=11.6 ms %llu, <=17.3 ms %llu, longer %llu; our present work avg %.2f ms max %.1f ms; "
             "submitted slots waiting at a present 0:%llu 1:%llu 2:%llu 3+:%llu; camera %s shift %d flip %d, %llu "
             "frames without a matching camera",
             s, N.n_gen / s, N.n_plain / s, N.n_fail, g_dropped, N.interval_hist[0], N.interval_hist[1],
             N.interval_hist[2], N.n_cpu ? N.cpu_sum / N.n_cpu : 0, N.cpu_max, g_pick_hist[0], g_pick_hist[1],
             g_pick_hist[2], g_pick_hist[3], !C.use_camera ? "motion left out" : C.decided ? "checked" : "unchecked",
             cand_shift(C.chosen), (int)cand_flip(C.chosen), N.cam_fallbacks);
        N.cam_fallbacks = 0;
        N.n_gen = N.n_plain = N.n_fail = N.n_cpu = 0;
        N.cpu_sum = N.cpu_max = 0;
        memset(N.interval_hist, 0, sizeof N.interval_hist);
        memset(g_pick_hist, 0, sizeof g_pick_hist);
        N.t_log = now;
    }
}

static bool insert_toggled() {
    bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    bool press = down && !N.key_down;
    N.key_down = down;
    if (!press) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// A present without frame generation: after everything the pacer still holds, straight through.
static VkResult plain_present(VkQueue q, const VkPresentInfoKHR *pi, PFN_vkQueuePresentKHR real) {
    fg_flush();
    AcquireSRWLockExclusive(&g_queue_lock);
    VkResult r = real(q, pi);
    ReleaseSRWLockExclusive(&g_queue_lock);
    N.last_real_target = now_ms();
    return r;
}

static void queue_wait_idle(VkQueue q) {
    fg_flush();
    AcquireSRWLockExclusive(&g_queue_lock);
    D.QueueWaitIdle(q);
    ReleaseSRWLockExclusive(&g_queue_lock);
}

VkResult fg_present(VkQueue q, const VkPresentInfoKHR *pi, PFN_vkQueuePresentKHR real) {
    double t0 = now_ms();
    PC.present = real;
    g_present_queue = q;
    double pfence = g_diag.pfence_frame, ofence = g_diag.ofence_frame;   // the game's GPU waits since the last present
    double afence = g_diag.afence_frame;
    g_diag.pfence_frame = g_diag.ofence_frame = g_diag.afence_frame = 0;
    if (N.last_present) {
        double iv = t0 - N.last_present;
        N.interval_hist[iv <= 11.6 ? 0 : iv <= 17.3 ? 1 : 2]++;
        if (iv < 50) N.frame_ema = N.frame_ema ? N.frame_ema * 0.9 + iv * 0.1 : iv;
        if (iv > 25 && N.last_generated && N.hitches_logged < 60) {   // gameplay frames only
            N.hitches_logged++;
            dlog("fg: hitch: %.1f ms since the last present. The game waited on fences %.1f ms on its present thread and "
                 "%.1f ms on other threads (%.1f ms of it on its swapchain image becoming free); its last acquire call "
                 "%.1f ms, Reflex sleep %.1f ms. GPU: last frame-to-frame %.1f ms, our frame generation work %.2f ms. "
                 "Our present work %.2f ms (fence wait %.2f, acquire %.2f)",
                 iv, pfence, ofence, afence, g_diag.acq_last, g_diag.sleep_last, N.period_last, N.gpu_last, N.last_work,
                 N.fence_wait, N.acq_wait);
        }
    }
    N.last_present = t0;
    if (insert_toggled()) {
        N.user_off = !N.user_off;
        dlog("fg: Insert: frame generation %s", N.user_off ? "OFF" : "ON");
    }
    int slot = pop_slot();
    if (slot < 0 || N.user_off || !g_fg_enabled || g_fg_dead || !D.ok || !D.has_bda_khr || pi->swapchainCount != 1 ||
        pi->pSwapchains[0] != SC.sc || !SC.usable) {
        if (slot >= 0 || N.ok) stats(false, false, t0);
        N.have_prev = false;
        return plain_present(q, pi, real);
    }
    if (!fg_init(q)) {
        InterlockedExchange(&g_fg_dead, 1);
        dlog("fg: frame generation is off for this session");
        return plain_present(q, pi, real);
    }
    Slot &s = S[slot];
    uint32_t W = SC.extent.width, H = SC.extent.height;
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (N.real.w != W || N.real.h != H) queue_wait_idle(q);   // a resize: nothing of ours in flight
    if (!make_img(N.real, W, H, VK_FORMAT_R8G8B8A8_UNORM, u) || !make_img(N.gen, W, H, VK_FORMAT_R8G8B8A8_UNORM, u)) {
        InterlockedExchange(&g_fg_dead, 1);
        return plain_present(q, pi, real);
    }

    int f = N.ring;
    N.ring = (N.ring + 1) % NRING;
    N.fence_wait = N.acq_wait = 0;
    if (N.fence_used[f]) {
        double tw = now_ms();
        D.WaitForFences(D.dev, 1, &N.fence[f], VK_TRUE, 1000000000ull);
        N.fence_wait = now_ms() - tw;
        D.ResetFences(D.dev, 1, &N.fence[f]);
        N.fence_used[f] = false;
        probe_read(f, N.probe[f]);
        uint64_t ts[2];
        if (N.ts_used[f] &&
            D.GetQueryPoolResults(D.dev, N.qpool, f * 2, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            N.gpu_last = (ts[1] - ts[0]) * D.ts_period / 1e6;
            N.gpu_sum += N.gpu_last, N.gpu_n++;
            if (N.gpu_last > N.gpu_max) N.gpu_max = N.gpu_last;
            if (N.prev_ts_start && ts[0] > N.prev_ts_start) {   // our submission starts once the game's frame is done:
                N.period_last = (ts[0] - N.prev_ts_start) * D.ts_period / 1e6;   // the GPU's frame-to-frame time
                if (N.period_last > N.period_max) N.period_max = N.period_last;
                if (N.period_last > 25) N.period_slow++;
            }
            N.prev_ts_start = ts[0];
        }
        N.ts_used[f] = false;
    }

    // The swapchain's images (vkGetSwapchainImagesKHR through the loader)
    static VkImage sc_images[16];
    static VkSwapchainKHR sc_images_of;
    static uint32_t sc_count;
    if (sc_images_of != SC.sc) {
        static PFN_vkGetSwapchainImagesKHR gsi;
        if (!gsi)
            gsi = (PFN_vkGetSwapchainImagesKHR)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkGetSwapchainImagesKHR");
        sc_count = 16;
        if (!gsi || gsi(D.dev, SC.sc, &sc_count, sc_images) < 0) sc_count = 0;
        sc_images_of = SC.sc;
        dlog("fg: swapchain %p has %u images", (void *)SC.sc, sc_count);
    }
    uint32_t I = pi->pImageIndices[0];
    if (I >= sc_count) {
        dlog("fg: game image index %u out of range (%u images), frame generation off", I, sc_count);
        InterlockedExchange(&g_fg_dead, 1);
        return plain_present(q, pi, real);
    }

    // A second image to show the generated frame in.
    uint32_t J = 0;
    double ta = now_ms();
    AcquireSRWLockExclusive(&g_acquire_lock);
    VkResult ar = D.AcquireNextImageKHR(D.dev, SC.sc, 100000000ull, N.acq[f], VK_NULL_HANDLE, &J);
    ReleaseSRWLockExclusive(&g_acquire_lock);
    N.acq_wait = now_ms() - ta;
    if ((ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) || J >= sc_count) {
        static int n;
        if (n++ < 10) dlog("fg: couldn't acquire a second swapchain image (%d), presenting the game's frame alone", ar);
        N.have_prev = false;
        stats(false, true, t0);
        return plain_present(q, pi, real);
    }

    // The camera of the presented frame (and the one before), by Reflex frame ID.
    uint64_t frame = reflex_seen() ? reflex_present_frame() : 0;
    int shift = cand_shift(C.chosen);
    Camera cur = s.cam, prev = N.prev;
    bool cam_by_id = false;
    uint32_t avail = 0;   // camera shifts whose pair exists right now (for the camera check)
    if (frame) {
        Camera a, b;
        for (int c = 0; c < NCAND; c += 2)
            if (camera_get_frame(frame + cand_shift(c), &a) && camera_get_frame(frame + cand_shift(c) - 1, &b))
                avail |= 3u << c;
        if (camera_get_frame(frame + shift, &a) && camera_get_frame(frame + shift - 1, &b)) cur = a, prev = b, cam_by_id = true;
    }
    if (!cam_by_id) {
        N.cam_fallbacks++;
        if (!(N.have_prev && N.prev.valid)) prev = cur;
    }
    bool reset = !N.have_prev || s.reset || camera_cut(prev, cur);
    VkImage imgI = sc_images[I], imgJ = sc_images[J];

    VkCommandBuffer cb = N.cb[f];
    D.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    D.BeginCommandBuffer(cb, &bi);
    if (N.qpool) {
        D.CmdResetQueryPool(cb, N.qpool, f * 2, 2);
        D.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, N.qpool, f * 2);
    }

    // The game's finished frame -> our RGBA8 copy (NGX's backbuffer input)
    VkImageBlit bl = {};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = bl.dstOffsets[1] = {(int32_t)W, (int32_t)H, 1};
    barrier(cb, imgI, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, N.real.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    D.CmdBlitImage(cb, imgI, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, N.real.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &bl, VK_FILTER_NEAREST);
    barrier(cb, imgI, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    barrier(cb, N.real.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(cb, N.gen.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // The camera check reads a few pixels of this frame's depth and vectors (often until decided, then now and then).
    if (C.map && frame && ((C.decided && C.fov_decided) ? N.gen_count % 120 == 0 : N.gen_count % 4 == 0))
        probe_record(cb, f, s, N.probe[f], frame, avail);

    // NGX Frame Generation
    bool generated = false;
    if (!N.feature || N.w != W || N.h != H || N.rw != s.rw || N.rh != s.rh) {
        if (N.feature) queue_wait_idle(q);
        release_feature();
        if (ngx_ok(NVSDK_NGX_VULKAN_AllocateParameters(&N.params), "parameters")) {
            NVSDK_NGX_DLSSG_Create_Params cp = {};
            cp.Width = W, cp.Height = H;
            cp.NativeBackbufferFormat = (unsigned)VK_FORMAT_R8G8B8A8_UNORM;
            cp.RenderWidth = s.rw, cp.RenderHeight = s.rh;
            cp.DynamicResolutionScaling = false;
            if (ngx_ok(NGX_VK_CREATE_DLSSG(cb, 1, 1, &N.feature, N.params, &cp), "create frame generation")) {
                N.w = W, N.h = H, N.rw = s.rw, N.rh = s.rh;
                dlog("fg: Frame Generation created: %ux%u, render %ux%u, depth %s", W, H, s.rw, s.rh,
                     s.depth_inverted ? "inverted" : "normal");
            } else {
                release_feature();
                InterlockedExchange(&g_fg_dead, 1);
            }
        }
    }
    if (N.feature) {
        NVSDK_NGX_Resource_VK color = NVSDK_NGX_Create_ImageView_Resource_VK(
            N.real.view, N.real.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, N.real.format, W, H, false);
        NVSDK_NGX_Resource_VK depth = NVSDK_NGX_Create_ImageView_Resource_VK(
            s.depth.view, s.depth.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, s.depth.format, s.depth.w, s.depth.h, false);
        NVSDK_NGX_Resource_VK mvec = NVSDK_NGX_Create_ImageView_Resource_VK(
            s.mvec.view, s.mvec.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, s.mvec.format, s.mvec.w, s.mvec.h, false);
        NVSDK_NGX_Resource_VK out = NVSDK_NGX_Create_ImageView_Resource_VK(
            N.gen.view, N.gen.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, N.gen.format, W, H, true);
        NVSDK_NGX_VK_DLSSG_Eval_Params ev = {};
        ev.pBackbuffer = &color;
        ev.pDepth = &depth;
        ev.pMVecs = &mvec;
        ev.pOutputInterpFrame = &out;
        NVSDK_NGX_DLSSG_Opt_Eval_Params op = {};
        op.multiFrameCount = op.multiFrameIndex = 1;
        fill_camera(op, cur, prev, s.depth_inverted, cand_flip(C.chosen), C.use_camera && cur.valid && prev.valid);
        op.jitterOffset[0] = op.jitterOffset[1] = 0;   // the game's vectors aren't jittered
        op.mvecScale[0] = op.mvecScale[1] = 1;          // pixels, as the game gives DLSS
        op.depthInverted = s.depth_inverted;
        op.cameraMotionIncluded = true;
        op.reset = reset;
        op.notRenderingGameFrames = false;
        op.orthoProjection = false;
        op.motionVectorsInvalidValue = FLT_MAX;
        op.motionVectorsDilated = false;
        op.menuDetectionEnabled = false;
        if (s.rw < s.depth.w || s.rh < s.depth.h) {
            op.depthSubrectBase = {0, 0}, op.depthSubrectSize = {s.rw, s.rh};
            op.mvecsSubrectBase = {0, 0}, op.mvecsSubrectSize = {s.rw, s.rh};
        }
        full_barrier(cb);
        generated = ngx_ok(NGX_VK_EVALUATE_DLSSG(cb, N.feature, N.params, &ev, &op), "evaluate frame generation");
        full_barrier(cb);
        if (!generated && ++N.fails >= 30) {
            dlog("fg: 30 failed evaluations, frame generation off for this session");
            InterlockedExchange(&g_fg_dead, 1);
        }
        if (generated) N.fails = 0;
    }

    // The generated frame (or, if NGX failed, the game's frame again) -> the second swapchain image
    Img &src = generated ? N.gen : N.real;
    barrier(cb, src.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, imgJ, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    D.CmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgJ, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_NEAREST);
    barrier(cb, imgJ, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (N.qpool) {
        D.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, N.qpool, f * 2 + 1);
        N.ts_used[f] = true;
    }
    D.EndCommandBuffer(cb);

    // Wait for the game's frame and our acquire; signal one semaphore per present.
    VkSemaphore waits[17];
    VkPipelineStageFlags stages[17];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < pi->waitSemaphoreCount && nw < 16; i++)
        waits[nw] = pi->pWaitSemaphores[i], stages[nw++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    waits[nw] = N.acq[f], stages[nw++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore signals[2] = {N.gen_done[f], N.real_done[f]};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = nw;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = signals;
    AcquireSRWLockExclusive(&g_queue_lock);
    VkResult sr = D.QueueSubmit(q, 1, &si, N.fence[f]);
    ReleaseSRWLockExclusive(&g_queue_lock);
    if (sr != VK_SUCCESS) {
        dlog("fg: vkQueueSubmit failed (%d), frame generation off", sr);
        InterlockedExchange(&g_fg_dead, 1);
        N.probe[f].active = false;
        return plain_present(q, pi, real);   // (J stays acquired; the swapchain gets recreated or runs one image short)
    }
    N.fence_used[f] = true;

    // Pacing: the pacer thread presents the generated frame at least half a frame after the last game frame, and the
    // game frame half a frame after the generated one. Half a frame is half the game's smoothed frame time, but never
    // less than one refresh, so the screen never gets more than it can show. This thread only waits (the frame cap)
    // until the pacer has presented the previous game frame. Pacing=0: both go out at once, in order.
    double half = 0;
    if (g_fg_pacing) {
        if (refresh_changed) {
            refresh_changed = false;
            N.refresh = refresh_hz();
            dlog("fg: pacing for a %d Hz display: game frames at most %.1f per second", N.refresh,
                 N.refresh > 0 ? N.refresh / 2.0 / 1.03 : 0.0);
        }
        double floor_ms = N.refresh > 0 ? 2000.0 / N.refresh * 1.03 : 0;
        // 3% under the game's own pace: a schedule that got ahead of the game drifts back (about 0.4 ms a frame)
        // instead of holding it back every frame for good. At the cap the floor rules and the wait is the cap.
        double tp = N.frame_ema * 0.97 > floor_ms ? N.frame_ema * 0.97 : floor_ms;
        if (tp > 34) tp = 34;
        half = tp / 2;
    }
    if (!PC.thread) {
        PC.thread = CreateThread(NULL, 0, pacer_main, NULL, 0, NULL);
        dlog("fg: pacer thread %s", PC.thread ? "started" : "COULD NOT START");
    }
    double tb = now_ms();
    pacer_wait_idle(200);   // the previous frame's presents are out (frame cap when the game runs ahead)
    double blocked = now_ms() - tb;
    N.pace_sum += blocked, N.pace_n++;
    if (blocked > N.pace_max) N.pace_max = blocked;
    double now = now_ms(), gen_t = now, real_t = now;
    if (half > 0) {
        gen_t = N.last_real_target + half > now ? N.last_real_target + half : now;
        real_t = gen_t + half;
    }
    PJob jg = {q, SC.sc, J, N.gen_done[f], gen_t, false}, jr = {q, pi->pSwapchains[0], I, N.real_done[f], real_t, true};
    if (PC.thread) {
        pacer_push(jg);
        pacer_push(jr);
    } else {   // (no thread: present here, back to back)
        VkPresentInfoKHR p = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        p.waitSemaphoreCount = p.swapchainCount = 1;
        AcquireSRWLockExclusive(&g_queue_lock);
        p.pWaitSemaphores = &jg.wait, p.pSwapchains = &jg.sc, p.pImageIndices = &jg.index;
        real(q, &p);
        p.pWaitSemaphores = &jr.wait, p.pSwapchains = &jr.sc, p.pImageIndices = &jr.index;
        VkResult r = real(q, &p);
        ReleaseSRWLockExclusive(&g_queue_lock);
        if (r != VK_SUCCESS) InterlockedExchange(&PC.result, (LONG)r);
    }
    N.last_real_target = real_t;
    VkResult rr = (VkResult)InterlockedExchange(&PC.result, (LONG)VK_SUCCESS), rg = VK_SUCCESS;
    if (pi->pResults) pi->pResults[0] = rr;

    if (generated) {
        N.prev = cur;
        N.have_prev = true;
        N.gen_count++;
    } else {
        N.have_prev = false;
    }
    stats(generated, !generated, t0);
    static bool logged;
    if (generated && !logged) {
        logged = true;
        dlog("fg: first generated frame presented (image %u before game image %u), reset %d, frame %llu, camera %s "
             "pos (%.1f %.1f %.1f) rot (%.1f %.1f %.1f) fov %.1f; presents -> %d / %d",
             J, I, reset, frame, cam_by_id ? "by frame ID" : "latest", cur.pos[0], cur.pos[1], cur.pos[2], cur.rot[0],
             cur.rot[1], cur.rot[2], cur.fov, rg, rr);
    }
    if (rg != VK_SUCCESS && rg != VK_SUBOPTIMAL_KHR) {
        static int n;
        if (n++ < 10) dlog("fg: generated frame present -> %d", rg);
    }
    return rr;
}
