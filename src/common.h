// Shared between dlssfg.cpp (hooks, log, camera script) and fg.cpp (frame generation).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct NVSDK_NGX_Parameter;

void dlog(const char *fmt, ...);

extern wchar_t g_dir[MAX_PATH];     // game folder (where the .asi is)
extern wchar_t g_fgdir[MAX_PATH];   // <game>\DLSSFG (nvngx_dlssg.dll, NGX's own logs)
extern VkInstance g_instance;
extern bool g_fg_enabled;           // DLSSFG.ini [DLSSFG] Enabled
extern bool g_fg_pacing;            // DLSSFG.ini [DLSSFG] Pacing

// The game camera, read by the ScriptHook script once per game update.
struct Camera {
    float pos[3];
    float rot[3];   // degrees, rotation order 2: x pitch, y roll, z yaw
    float fov;      // vertical, degrees
    uint64_t tick;
    uint64_t frame;   // the Reflex frame ID of the game update it was read in
    bool valid;
};
bool camera_get(Camera *out);                     // the latest
bool camera_get_frame(uint64_t frame, Camera *out);   // the one read during that frame's update

// Reflex frame IDs the game passes with its latency markers (NvLL_VK_SetLatencyMarker).
uint64_t reflex_sim_frame();       // last SIMULATION_START
uint64_t reflex_present_frame();   // last PRESENT_START (set just before the game's vkQueuePresentKHR)
bool reflex_seen();

// Timing diagnostics (dlssfg.cpp fills them, fg.cpp reports them).
struct Diag {
    volatile double acq_last, acq_max, acq_sum;      // the game's vkAcquireNextImage*: ms blocked
    volatile LONG acq_n, acq_slow;                    // calls, calls over 4 ms
    volatile double sleep_last, sleep_max, sleep_sum; // Reflex NvLL_VK_Sleep: ms
    volatile LONG sleep_n;
    volatile double pfence_frame, ofence_frame;       // the game's vkWaitForFences since the last present: present
                                                      // thread / other threads (ms)
    volatile double pfence_max, ofence_max;           // per call, last 10 s
    volatile double afence_frame, afence_max;         // of those, waits on the fence of the game's swapchain acquire
};
extern Diag g_diag;
double qpc_ms();
bool diag_memory(char *out, size_t cap);   // VRAM use / budget (VK_EXT_memory_budget), false if unavailable

// fg.cpp
void fg_device_created(VkDevice dev, VkPhysicalDevice phys, PFN_vkGetInstanceProcAddr gipa,
                       PFN_vkGetDeviceProcAddr gdpa, bool has_bda_khr);
void fg_queue(VkQueue q, uint32_t family);
void fg_swapchain_created(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR *ci);
void fg_swapchain_destroyed(VkSwapchainKHR sc);
void fg_dlss_created(int create_flags);
void fg_after_dlss_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Parameter *p);
void fg_submitted(const VkCommandBuffer *cbs, uint32_t n);   // the game's vkQueueSubmit*
VkResult fg_present(VkQueue q, const VkPresentInfoKHR *pi, PFN_vkQueuePresentKHR real);
extern SRWLOCK g_acquire_lock;      // the game's acquires and ours (swapchain access must be serialized)
extern SRWLOCK g_queue_lock;        // the present queue: the game's submits there and our pacer thread's presents
extern VkQueue volatile g_present_queue;
void fg_flush();                    // wait until the pacer thread has presented everything handed to it
