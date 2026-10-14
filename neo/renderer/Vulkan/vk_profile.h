/*
===========================================================================

dhewm3-rt Vulkan backend - profiling aggregation, debug labels and resource
accounting.  Stage A0 of docs/plans/20261010_vk_core_audit.md.

Owns the metric registry: every per-frame number the backend measures is
written into the current frame slot's sample, then pushed into a rolling
window that reports mean/p5/p50/p95/max.  Raw per-frame rows go to a CSV so
the console summary can stay narrow without discarding data.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#ifndef __VK_PROFILE_H__
#define __VK_PROFILE_H__

#include <vulkan/vulkan.h>

// r_vkRTProfileGroups bitmask: which metric families reach the console and the CSV.
enum vkProfGroup_t
{
    VKPROF_GRP_PHASES = 1,
    VKPROF_GRP_FRAME = 2,
    VKPROF_GRP_UPLOAD = 4,
    VKPROF_GRP_WAITS = 8,
    VKPROF_GRP_MEMORY = 16,
    VKPROF_GRP_MIDFRAME = 32,
    VKPROF_GRP_ALL = 63,
};

// r_vkRTProfileStats bitmask.
enum vkProfStat_t
{
    VKPROF_STAT_MEAN = 1,
    VKPROF_STAT_P5 = 2,
    VKPROF_STAT_P50 = 4,
    VKPROF_STAT_P95 = 8,
    VKPROF_STAT_MAX = 16,
};

// Fixed metrics.  Order is the CSV column order and must not be reshuffled
// between runs that get compared; append new ones at the end of their group.
enum vkProfMetric_t
{
    VKPROF_GPU_TOTAL = 0,
    VKPROF_GPU_RASTER,
    VKPROF_CPU_TOTAL,
    VKPROF_CPU_RASTER,

    VKPROF_FRAME_FENCEWAIT,
    VKPROF_FRAME_ACQUIRE,
    VKPROF_FRAME_DRAIN,
    VKPROF_FRAME_UPLOAD,
    VKPROF_FRAME_SUBMIT,
    VKPROF_FRAME_PRESENT,
    VKPROF_FRAME_BACKEND,
    VKPROF_FRAME_OTHER,
    VKPROF_FRAME_PROFILEPRINT,
    VKPROF_FRAME_GAME,
    VKPROF_FRAME_FRONTEND,
    VKPROF_FRAME_FINDVIEW,
    VKPROF_FRAME_ADDLIGHTS,
    VKPROF_FRAME_ADDMODELS,

    VKPROF_UPLOAD_FLUSHES,
    VKPROF_UPLOAD_KB,
    VKPROF_UPLOAD_STATICBLOCKS,
    VKPROF_RING_DRAWS,
    VKPROF_RING_VERT_KB,
    VKPROF_RING_IDX_KB,
    VKPROF_RING_DATA_PEAK_KB,
    VKPROF_RING_UBO_PEAK_KB,
    VKPROF_RING_OVERFLOWS,
    VKPROF_VC_ALLOCS,
    VKPROF_VC_ALLOC_KB,
    VKPROF_VC_ALLOC_MS,
    VKPROF_VC_FREES,

    VKPROF_BLAS_UPDATES,
    VKPROF_BLAS_REBUILDS,
    VKPROF_BLAS_TRIS,
    VKPROF_BLAS_DYNINST,

    VKPROF_MEM_RSS_MB,
    VKPROF_MEM_VIRTMEM_MB,
    VKPROF_MEM_VRAM_USED_MB,
    VKPROF_MEM_VRAM_BUDGET_MB,

    VKPROF_MIDFRAME_IMAGELOADS,
    VKPROF_MIDFRAME_IMAGE_MS,
    VKPROF_MIDFRAME_PIPELINES,
    VKPROF_MIDFRAME_PIPELINE_MS,

    // Scene content, so a capture can be checked against the condition a finding needs.
    VKPROF_LIGHTS_VIEW,
    VKPROF_LIGHTS_SHADOW,
    VKPROF_LIGHTS_BATCHED,
    VKPROF_LIGHTS_SERIAL,
    VKPROF_SHADOW_MPIX,   // shadow-ray launch area, all dispatches, megapixels
    VKPROF_SHADOW_BREAKS, // render-pass breaks for in-loop shadow dispatch (S3 batches or serial lights)

    VKPROF_FIXED_COUNT
};

// Tagged blocking waits.  Every vkWaitForFences / vkQueueWaitIdle / vkDeviceWaitIdle
// goes through the wrappers below so the ms land under the owning module.
enum vkProfWait_t
{
    VKPROF_WAIT_UPLOAD_FLUSH = 0,
    VKPROF_WAIT_IMAGE,
    VKPROF_WAIT_ACCELSTRUCT,
    VKPROF_WAIT_AO,
    VKPROF_WAIT_GI,
    VKPROF_WAIT_GI_PROBE,
    VKPROF_WAIT_VOL,
    VKPROF_WAIT_BLOOM,
    VKPROF_WAIT_GBUFFER,
    VKPROF_WAIT_TEMPORAL,
    VKPROF_WAIT_REFLECTIONS,
    VKPROF_WAIT_TONEMAP,
    VKPROF_WAIT_UPSCALE,
    VKPROF_WAIT_SWAPCHAIN,
    VKPROF_WAIT_READBACK,
    VKPROF_WAIT_SERIALIZE,
    VKPROF_WAIT_SHUTDOWN,
    VKPROF_WAIT_OTHER,
    VKPROF_WAIT_COUNT
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// phaseNames/phaseCount come from the backend's phase enum; each becomes a
// gpu.<name> and cpu.<name> metric.  Safe to call repeatedly (vid_restart).
void VK_Prof_Init(const char *const *phaseNames, int phaseCount);
void VK_Prof_Shutdown(void);

// 0 = off, 1 = window summaries, 2 = one row per frame.
int VK_Prof_Mode(void);
bool VK_Prof_Active(void);
int VK_Prof_Groups(void);

// ---------------------------------------------------------------------------
// Per-frame samples
// ---------------------------------------------------------------------------

int VK_Prof_PhaseMetric(int phase, bool gpu);
// ms = true for wait.<tag>.ms, false for wait.<tag>.n.
int VK_Prof_WaitMetric(vkProfWait_t tag, bool ms);

void VK_Prof_BeginFrameSlot(int slot);
void VK_Prof_Set(int metric, double value);
void VK_Prof_Add(int metric, double value);
void VK_Prof_SetSlot(int slot, int metric, double value);
void VK_Prof_AddSlot(int slot, int metric, double value);
double VK_Prof_GetSlot(int slot, int metric);
void VK_Prof_Max(int metric, double value);

// Closes the slot's sample: fills the memory gauges, appends to the window,
// writes the CSV row, and prints the summary when the window is full.
void VK_Prof_PushFrame(int slot, int frameNum);

// True on the push that closed a window, so callers can emit extra lines that do
// not fit the one-value-per-metric model (the vertex-cache site table).
bool VK_Prof_WindowJustClosed(void);

// ---------------------------------------------------------------------------
// Tagged blocking waits (F2, F3)
// ---------------------------------------------------------------------------

VkResult VK_Prof_WaitForFences(vkProfWait_t tag, uint32_t fenceCount, const VkFence *fences, VkBool32 waitAll,
                               uint64_t timeout);
VkResult VK_Prof_QueueWaitIdle(vkProfWait_t tag, VkQueue queue);
VkResult VK_Prof_DeviceWaitIdle(vkProfWait_t tag);

// ---------------------------------------------------------------------------
// Startup report and mid-frame creation
// ---------------------------------------------------------------------------

// Device allocation/sampler limits and which optional extensions we got.  F4/R4
// ask how close we run to those limits; the live counts come from RMV / Nsight,
// which read them for free once r_vkDebugLabels has named the objects.
void VK_Prof_LogDeviceLimits(void);

// Mid-frame creation that is invisible in a window average but owns the hitch (F3, X4).
void VK_Prof_NoteLazyImageLoad(const char *name, double ms);
void VK_Prof_NotePipelineCreate(const char *name, double ms);

// ---------------------------------------------------------------------------
// Debug labels (A0a) — makes Nsight / RGP / RenderDoc / RMV captures readable
// ---------------------------------------------------------------------------

// Loads the VK_EXT_debug_utils entry points.  Call once after device creation.
void VK_Prof_InitDebugUtils(void);
bool VK_Prof_LabelsEnabled(void);
void VK_Prof_PushLabel(VkCommandBuffer cmd, const char *name);
void VK_Prof_PopLabel(VkCommandBuffer cmd);
void VK_Prof_NameObject(uint64_t handle, VkObjectType type, const char *name);

#endif // __VK_PROFILE_H__
