/*
===========================================================================

dhewm3-rt Vulkan backend - profiling aggregation, debug labels and resource
accounting.  Stage A0 of docs/plans/20261010_vk_core_audit.md.

The console prints a window summary; the CSV keeps every frame.  Before this,
r_vkRTProfile 1 printed one frame in 30 and discarded the rest, so a hitch was
only logged if it happened to land on the sampled frame.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_profile.h"
#include "framework/FileSystem.h"
#include "sys/sys_public.h" // Sys_GetProcessMemoryBytes
#include <SDL.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Cvars (debug only — deliberately absent from the settings menu)
// ---------------------------------------------------------------------------

static idCVar r_vkRTProfile("r_vkRTProfile", "0", CVAR_RENDERER | CVAR_INTEGER,
                            "GPU/CPU phase profiling (0=off, 1=window summaries, 2=one row per frame)");
static idCVar r_vkRTProfileWindow("r_vkRTProfileWindow", "30", CVAR_RENDERER | CVAR_INTEGER,
                                  "Frames per summary window when r_vkRTProfile=1 (1-256)");
static idCVar r_vkRTProfileGroups("r_vkRTProfileGroups", "63", CVAR_RENDERER | CVAR_INTEGER,
                                  "Console filter only, every group is always collected and always in the CSV: "
                                  "1=phases 2=frame 4=upload 8=waits 16=memory 32=midframe");
static idCVar r_vkRTProfileStats("r_vkRTProfileStats", "29", CVAR_RENDERER | CVAR_INTEGER,
                                 "Summary columns: 1=mean 2=p5 4=p50 8=p95 16=max");
static idCVar r_vkRTProfileSpikeMs("r_vkRTProfileSpikeMs", "0", CVAR_RENDERER | CVAR_FLOAT,
                                   "Dump every metric for any frame whose backend ms exceeds this (0=off)");
static idCVar r_vkRTProfileCSV("r_vkRTProfileCSV", "", CVAR_RENDERER,
                               "Write one CSV row per frame to this path (empty=off)");
static idCVar r_vkDebugLabels("r_vkDebugLabels", "0", CVAR_RENDERER | CVAR_BOOL,
                              "Emit VK_EXT_debug_utils pass labels and object names for Nsight/RGP/RenderDoc/RMV");

// ---------------------------------------------------------------------------
// Metric registry
// ---------------------------------------------------------------------------

static const int VKPROF_MAX_METRICS = 192;
static const int VKPROF_MAX_WINDOW = 256;
static const int VKPROF_NAME_POOL = 8192;

struct vkProfMetricDesc_t
{
    const char *name;
    uint32_t group;
    int prec;
};

static vkProfMetricDesc_t s_desc[VKPROF_MAX_METRICS];
static int s_metricCount = 0;
static int s_waitMetricBase = -1;
static int s_phaseMetricBase = -1;
static int s_phaseCount = 0;

static char s_namePool[VKPROF_NAME_POOL];
static int s_namePoolUsed = 0;

static const char *VK_Prof_PoolName(const char *fmt, const char *arg)
{
    char tmp[128];
    idStr::snPrintf(tmp, sizeof(tmp), fmt, arg);
    const int len = (int)strlen(tmp) + 1;
    if (s_namePoolUsed + len > VKPROF_NAME_POOL)
        return "overflow";
    char *dst = s_namePool + s_namePoolUsed;
    memcpy(dst, tmp, len);
    s_namePoolUsed += len;
    return dst;
}

static int s_metricsRefused = 0;

static int VK_Prof_Register(const char *name, uint32_t group, int prec)
{
    if (s_metricCount >= VKPROF_MAX_METRICS)
    {
        s_metricsRefused++;
        return -1;
    }
    const int id = s_metricCount++;
    s_desc[id].name = name;
    s_desc[id].group = group;
    s_desc[id].prec = prec;
    return id;
}

// Indexed by vkProfMetric_t; registered in enum order so the ids line up.
static const vkProfMetricDesc_t s_fixedDesc[VKPROF_FIXED_COUNT] = {
    {"gpu.total", VKPROF_GRP_PHASES, 3},
    {"gpu.raster", VKPROF_GRP_PHASES, 3},
    {"cpu.total", VKPROF_GRP_PHASES, 3},
    {"cpu.raster", VKPROF_GRP_PHASES, 3},

    {"frame.fenceWait", VKPROF_GRP_FRAME, 3},
    {"frame.acquire", VKPROF_GRP_FRAME, 3},
    {"frame.drain", VKPROF_GRP_FRAME, 3},
    {"frame.upload", VKPROF_GRP_FRAME, 3},
    {"frame.submit", VKPROF_GRP_FRAME, 3},
    {"frame.present", VKPROF_GRP_FRAME, 3},
    {"frame.backend", VKPROF_GRP_FRAME, 3},
    {"frame.other", VKPROF_GRP_FRAME, 3},
    {"frame.game", VKPROF_GRP_FRAME, 3},
    {"frame.frontend", VKPROF_GRP_FRAME, 3},
    {"frame.findView", VKPROF_GRP_FRAME, 3},
    {"frame.addLights", VKPROF_GRP_FRAME, 3},
    {"frame.addModels", VKPROF_GRP_FRAME, 3},

    {"upload.flushes", VKPROF_GRP_UPLOAD, 1},
    {"upload.KB", VKPROF_GRP_UPLOAD, 0},
    {"upload.staticBlocks", VKPROF_GRP_UPLOAD, 1},
    {"ring.draws", VKPROF_GRP_UPLOAD, 0},
    {"ring.vertKB", VKPROF_GRP_UPLOAD, 0},
    {"ring.idxKB", VKPROF_GRP_UPLOAD, 0},
    {"ring.dataPeakKB", VKPROF_GRP_UPLOAD, 0},
    {"ring.uboPeakKB", VKPROF_GRP_UPLOAD, 0},
    {"ring.overflows", VKPROF_GRP_UPLOAD, 0},
    {"vc.allocs", VKPROF_GRP_UPLOAD, 0},
    {"vc.allocKB", VKPROF_GRP_UPLOAD, 0},
    {"vc.allocMs", VKPROF_GRP_UPLOAD, 3},
    {"vc.frees", VKPROF_GRP_UPLOAD, 0},

    {"blas.updates", VKPROF_GRP_UPLOAD, 1},
    {"blas.rebuilds", VKPROF_GRP_UPLOAD, 1},
    {"blas.tris", VKPROF_GRP_UPLOAD, 0},
    {"blas.dynInst", VKPROF_GRP_UPLOAD, 1},

    {"mem.rssMB", VKPROF_GRP_MEMORY, 1},
    {"mem.virtMemMB", VKPROF_GRP_MEMORY, 1},
    {"mem.vramUsedMB", VKPROF_GRP_MEMORY, 1},
    {"mem.vramBudgetMB", VKPROF_GRP_MEMORY, 1},

    {"midframe.imageLoads", VKPROF_GRP_MIDFRAME, 1},
    {"midframe.imageMs", VKPROF_GRP_MIDFRAME, 3},
    {"midframe.pipelines", VKPROF_GRP_MIDFRAME, 1},
    {"midframe.pipelineMs", VKPROF_GRP_MIDFRAME, 3},
};

static const char *const s_waitTagNames[VKPROF_WAIT_COUNT] = {
    "uploadFlush", "image",       "accelStruct", "ao",       "gi",        "giProbe",
    "vol",         "bloom",       "gbuffer",     "temporal", "reflect",   "tonemap",
    "upscale",     "swapchain",   "readback",    "serialize", "shutdown", "other",
};

// ---------------------------------------------------------------------------
// Sample storage
// ---------------------------------------------------------------------------

static double s_cur[VK_MAX_FRAMES_IN_FLIGHT][VKPROF_MAX_METRICS];
static double s_window[VKPROF_MAX_WINDOW][VKPROF_MAX_METRICS];
static int s_windowFrameNum[VKPROF_MAX_WINDOW];
static int s_windowCount = 0;
static bool s_initialized = false;
static bool s_windowJustClosed = false;

// ---------------------------------------------------------------------------
// Debug utils entry points
// ---------------------------------------------------------------------------

static PFN_vkCmdBeginDebugUtilsLabelEXT s_vkCmdBeginDebugUtilsLabelEXT = NULL;
static PFN_vkCmdEndDebugUtilsLabelEXT s_vkCmdEndDebugUtilsLabelEXT = NULL;
static PFN_vkSetDebugUtilsObjectNameEXT s_vkSetDebugUtilsObjectNameEXT = NULL;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int VK_Prof_Mode(void)
{
    return r_vkRTProfile.GetInteger();
}

bool VK_Prof_Active(void)
{
    return s_initialized && r_vkRTProfile.GetInteger() > 0;
}

int VK_Prof_Groups(void)
{
    return r_vkRTProfileGroups.GetInteger();
}

static int VK_Prof_WindowSize(void)
{
    const int n = r_vkRTProfileWindow.GetInteger();
    return (n < 1) ? 1 : ((n > VKPROF_MAX_WINDOW) ? VKPROF_MAX_WINDOW : n);
}

void VK_Prof_Init(const char *const *phaseNames, int phaseCount)
{
    s_metricCount = 0;
    s_metricsRefused = 0;
    s_namePoolUsed = 0;
    s_windowCount = 0;
    memset(s_cur, 0, sizeof(s_cur));

    for (int i = 0; i < VKPROF_FIXED_COUNT; i++)
        VK_Prof_Register(s_fixedDesc[i].name, s_fixedDesc[i].group, s_fixedDesc[i].prec);

    s_waitMetricBase = s_metricCount;
    for (int i = 0; i < VKPROF_WAIT_COUNT; i++)
    {
        VK_Prof_Register(VK_Prof_PoolName("wait.%s.ms", s_waitTagNames[i]), VKPROF_GRP_WAITS, 3);
        VK_Prof_Register(VK_Prof_PoolName("wait.%s.n", s_waitTagNames[i]), VKPROF_GRP_WAITS, 0);
    }

    s_phaseMetricBase = s_metricCount;
    s_phaseCount = phaseCount;
    for (int i = 0; i < phaseCount; i++)
    {
        VK_Prof_Register(VK_Prof_PoolName("gpu.%s", phaseNames[i]), VKPROF_GRP_PHASES, 3);
        VK_Prof_Register(VK_Prof_PoolName("cpu.%s", phaseNames[i]), VKPROF_GRP_PHASES, 3);
    }

    if (s_metricsRefused > 0)
        common->Warning("VK PROFILE: %d metrics dropped, table holds %d; raise VKPROF_MAX_METRICS", s_metricsRefused,
                        VKPROF_MAX_METRICS);

    s_initialized = true;
}

static void VK_Prof_CloseCSV(void);

void VK_Prof_Shutdown(void)
{
    VK_Prof_CloseCSV();
    s_initialized = false;
    s_windowCount = 0;
}

int VK_Prof_PhaseMetric(int phase, bool gpu)
{
    if (!s_initialized || phase < 0 || phase >= s_phaseCount)
        return -1;
    return s_phaseMetricBase + phase * 2 + (gpu ? 0 : 1);
}

int VK_Prof_WaitMetric(vkProfWait_t tag, bool ms)
{
    if (!s_initialized || s_waitMetricBase < 0 || (int)tag < 0 || (int)tag >= VKPROF_WAIT_COUNT)
        return -1;
    return s_waitMetricBase + (int)tag * 2 + (ms ? 0 : 1);
}

// ---------------------------------------------------------------------------
// Sample accessors
// ---------------------------------------------------------------------------

static void VK_Prof_SampleMemory(int slot);
static void VK_Prof_CheckCSV(void);

// The one entry point that runs every frame whether or not profiling is on, so the
// CSV open/close check lives here: PushFrame is unreachable with r_vkRTProfile 0,
// which would otherwise leave the file open with a window of rows unflushed.
void VK_Prof_BeginFrameSlot(int slot)
{
    VK_Prof_CheckCSV();

    // A partial window from before profiling was switched off describes whatever the
    // settings were back then.  PushFrame is unreachable while off, so those rows sat
    // in the ring and the first window after re-enabling blended them with fresh ones
    // — the 2026-10-12 capture reported a p50 from the old 0.667 render scale in its
    // first window and the correct 1:1 value in its p95.  Same for a window resize.
    {
        static bool s_wasActive = false;
        static int s_lastWindowSize = -1;
        const bool active = VK_Prof_Active();
        const int windowSize = VK_Prof_WindowSize();
        if (active != s_wasActive || windowSize != s_lastWindowSize)
        {
            s_windowCount = 0;
            s_wasActive = active;
            s_lastWindowSize = windowSize;
        }
    }

    if (slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT)
        return;
    memset(s_cur[slot], 0, sizeof(s_cur[slot]));

    // Sampled at frame open, not at push: push happens two frames later, so sampling
    // there attributed "now" to a frame that had already been presented.
    //
    // Deliberately NOT gated on the memory group bit.  r_vkRTProfileGroups filters
    // what the console prints; it must not decide what gets collected, or a group the
    // console happens to hide comes back as a column of zeros in the CSV.
    if (VK_Prof_Active())
        VK_Prof_SampleMemory(slot);
}

void VK_Prof_SetSlot(int slot, int metric, double value)
{
    if (!s_initialized || slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT || metric < 0 || metric >= s_metricCount)
        return;
    s_cur[slot][metric] = value;
}

void VK_Prof_AddSlot(int slot, int metric, double value)
{
    if (!s_initialized || slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT || metric < 0 || metric >= s_metricCount)
        return;
    s_cur[slot][metric] += value;
}

double VK_Prof_GetSlot(int slot, int metric)
{
    if (!s_initialized || slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT || metric < 0 || metric >= s_metricCount)
        return 0.0;
    return s_cur[slot][metric];
}

void VK_Prof_Set(int metric, double value)
{
    VK_Prof_SetSlot((int)vk.currentFrame, metric, value);
}

void VK_Prof_Add(int metric, double value)
{
    VK_Prof_AddSlot((int)vk.currentFrame, metric, value);
}

void VK_Prof_Max(int metric, double value)
{
    const int slot = (int)vk.currentFrame;
    if (!s_initialized || slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT || metric < 0 || metric >= s_metricCount)
        return;
    if (value > s_cur[slot][metric])
        s_cur[slot][metric] = value;
}

// ---------------------------------------------------------------------------
// Memory gauges
// ---------------------------------------------------------------------------

static void VK_Prof_SampleMemory(int slot)
{
    const double toMB = 1.0 / (1024.0 * 1024.0);

    VK_Prof_SetSlot(slot, VKPROF_MEM_RSS_MB, (double)Sys_GetProcessMemoryBytes() * toMB);
    VK_Prof_SetSlot(slot, VKPROF_MEM_VIRTMEM_MB, (double)vertexCacheVirtMemBytes * toMB);

    if (!vk.memoryBudgetSupported || vk.physicalDevice == VK_NULL_HANDLE)
        return;

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    props2.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(vk.physicalDevice, &props2);

    // Device-local heaps only: the host heap's "usage" is system RAM and would
    // double-count what mem.rssMB already reports.
    VkDeviceSize used = 0, avail = 0;
    const VkPhysicalDeviceMemoryProperties &mp = props2.memoryProperties;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
    {
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            used += budget.heapUsage[h];
            avail += budget.heapBudget[h];
        }
    }
    VK_Prof_SetSlot(slot, VKPROF_MEM_VRAM_USED_MB, (double)used * toMB);
    VK_Prof_SetSlot(slot, VKPROF_MEM_VRAM_BUDGET_MB, (double)avail * toMB);
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

static idFile *s_csvFile = NULL;
static idStr s_csvPath;
static idStr s_csvPending;

static void VK_Prof_CloseCSV(void)
{
    if (!s_csvFile)
        return;
    if (s_csvPending.Length() > 0)
    {
        s_csvFile->Write(s_csvPending.c_str(), s_csvPending.Length());
        s_csvPending.Clear();
    }
    fileSystem->CloseFile(s_csvFile);
    common->Printf("VK PROFILE: closed CSV '%s'\n", s_csvPath.c_str());
    s_csvFile = NULL;
    s_csvPath.Clear();
}

// Opens on first use and reopens when the cvar changes.  The header is written
// from the full metric table, so columns are stable even when a group is off.
static void VK_Prof_CheckCSV(void)
{
    const char *want = r_vkRTProfileCSV.GetString();
    // Switching profiling off closes the file rather than stranding it open with a
    // window of rows still buffered.
    if (!want || !want[0] || !VK_Prof_Active())
    {
        VK_Prof_CloseCSV();
        return;
    }
    if (s_csvFile && s_csvPath.Icmp(want) == 0)
        return;

    VK_Prof_CloseCSV();
    s_csvFile = fileSystem->OpenFileWrite(want);
    if (!s_csvFile)
    {
        common->Warning("VK PROFILE: could not open CSV '%s'", want);
        r_vkRTProfileCSV.SetString("");
        return;
    }
    s_csvPath = want;

    idStr header = "frame";
    for (int m = 0; m < s_metricCount; m++)
    {
        header += ",";
        header += s_desc[m].name;
    }
    header += "\n";
    s_csvFile->Write(header.c_str(), header.Length());
    common->Printf("VK PROFILE: writing per-frame CSV to '%s' (%d columns)\n", want, s_metricCount);
}

static void VK_Prof_AppendCSVRow(int frameNum, const double *sample)
{
    if (!s_csvFile)
        return;
    s_csvPending += va("%d", frameNum);
    for (int m = 0; m < s_metricCount; m++)
        s_csvPending += va(",%.4f", sample[m]);
    s_csvPending += "\n";
}

static void VK_Prof_FlushCSV(void)
{
    if (!s_csvFile || s_csvPending.Length() == 0)
        return;
    s_csvFile->Write(s_csvPending.c_str(), s_csvPending.Length());
    s_csvPending.Clear();
}

// ---------------------------------------------------------------------------
// Window statistics
// ---------------------------------------------------------------------------

static int VK_Prof_CompareDouble(const void *a, const void *b)
{
    const double da = *(const double *)a, db = *(const double *)b;
    return (da < db) ? -1 : ((da > db) ? 1 : 0);
}

struct vkProfStats_t
{
    double mean, p5, p50, p95, max;
};

static void VK_Prof_ComputeStats(int metric, int count, vkProfStats_t *out)
{
    double sorted[VKPROF_MAX_WINDOW];
    double sum = 0.0;
    for (int i = 0; i < count; i++)
    {
        sorted[i] = s_window[i][metric];
        sum += sorted[i];
    }
    qsort(sorted, (size_t)count, sizeof(double), VK_Prof_CompareDouble);

    out->mean = sum / (double)count;
    out->p5 = sorted[(int)(0.05 * (count - 1) + 0.5)];
    out->p50 = sorted[(int)(0.50 * (count - 1) + 0.5)];
    out->p95 = sorted[(int)(0.95 * (count - 1) + 0.5)];
    out->max = sorted[count - 1];
}

static void VK_Prof_PrintWindow(int count, int lastFrame)
{
    const uint32_t groups = (uint32_t)r_vkRTProfileGroups.GetInteger();
    const int stats = r_vkRTProfileStats.GetInteger();

    // Title and column header on separate lines so the columns line up with the
    // data rows below, which are indented by two spaces.
    common->Printf("VK PROFILE WINDOW: frames=%d thru=%d groups=0x%x\n", count, lastFrame, groups);
    idStr head = va("  %-24s", "metric");
    if (stats & VKPROF_STAT_MEAN)
        head += "    mean";
    if (stats & VKPROF_STAT_P5)
        head += "      p5";
    if (stats & VKPROF_STAT_P50)
        head += "     p50";
    if (stats & VKPROF_STAT_P95)
        head += "     p95";
    if (stats & VKPROF_STAT_MAX)
        head += "     max";
    common->Printf("%s\n", head.c_str());

    for (int m = 0; m < s_metricCount; m++)
    {
        if ((s_desc[m].group & groups) == 0)
            continue;

        vkProfStats_t st;
        VK_Prof_ComputeStats(m, count, &st);
        // A metric that is zero across the whole window is either a dead phase or an
        // inactive feature; printing 26 of those is what made the old line unreadable.
        if (st.max == 0.0)
            continue;

        const int prec = s_desc[m].prec;
        idStr line = va("  %-24s", s_desc[m].name);
        if (stats & VKPROF_STAT_MEAN)
            line += va("%8.*f", prec, st.mean);
        if (stats & VKPROF_STAT_P5)
            line += va("%8.*f", prec, st.p5);
        if (stats & VKPROF_STAT_P50)
            line += va("%8.*f", prec, st.p50);
        if (stats & VKPROF_STAT_P95)
            line += va("%8.*f", prec, st.p95);
        if (stats & VKPROF_STAT_MAX)
            line += va("%8.*f", prec, st.max);
        common->Printf("%s\n", line.c_str());
    }
}

// One line holding every non-zero metric for a single frame.  Used for the spike
// dump (window averages cannot show a hitch) and for mode 2.
static void VK_Prof_PrintFrameLine(const char *prefix, int frameNum, const double *sample)
{
    const uint32_t groups = (uint32_t)r_vkRTProfileGroups.GetInteger();
    idStr line = va("%s: frame=%d", prefix, frameNum);
    for (int m = 0; m < s_metricCount; m++)
    {
        if ((s_desc[m].group & groups) == 0 || sample[m] == 0.0)
            continue;
        line += va(" %s=%.*f", s_desc[m].name, s_desc[m].prec, sample[m]);
    }
    common->Printf("%s\n", line.c_str());
}

bool VK_Prof_WindowJustClosed(void)
{
    return s_windowJustClosed;
}

void VK_Prof_PushFrame(int slot, int frameNum)
{
    s_windowJustClosed = false;
    if (!VK_Prof_Active() || slot < 0 || slot >= VK_MAX_FRAMES_IN_FLIGHT)
        return;

    const double *sample = s_cur[slot];

    const float spikeMs = r_vkRTProfileSpikeMs.GetFloat();
    if (spikeMs > 0.0f && sample[VKPROF_FRAME_BACKEND] > (double)spikeMs)
        VK_Prof_PrintFrameLine("VK PROFILE SPIKE", frameNum, sample);

    VK_Prof_AppendCSVRow(frameNum, sample);

    // Mode 2 is one row per frame, not a one-frame window: a block of 40-plus lines
    // at 60 fps distorts what it is measuring, and every percentile would be the
    // same number anyway.
    if (r_vkRTProfile.GetInteger() >= 2)
    {
        VK_Prof_PrintFrameLine("VK PROFILE FRAME", frameNum, sample);
        VK_Prof_FlushCSV();
        s_windowCount = 0;
        s_windowJustClosed = true;
        return;
    }

    if (s_windowCount < VKPROF_MAX_WINDOW)
    {
        memcpy(s_window[s_windowCount], sample, sizeof(double) * VKPROF_MAX_METRICS);
        s_windowFrameNum[s_windowCount] = frameNum;
        s_windowCount++;
    }

    if (s_windowCount >= VK_Prof_WindowSize())
    {
        VK_Prof_PrintWindow(s_windowCount, s_windowFrameNum[s_windowCount - 1]);
        VK_Prof_FlushCSV();
        s_windowCount = 0;
        s_windowJustClosed = true;
    }
}

// ---------------------------------------------------------------------------
// Tagged blocking waits
// ---------------------------------------------------------------------------

static void VK_Prof_NoteWait(vkProfWait_t tag, uint64_t startCounter)
{
    if (!s_initialized || s_waitMetricBase < 0 || (int)tag < 0 || (int)tag >= VKPROF_WAIT_COUNT)
        return;
    const uint64_t freq = SDL_GetPerformanceFrequency();
    if (freq == 0)
        return;
    const double ms = ((double)(SDL_GetPerformanceCounter() - startCounter) * 1000.0) / (double)freq;
    VK_Prof_Add(s_waitMetricBase + (int)tag * 2, ms);
    VK_Prof_Add(s_waitMetricBase + (int)tag * 2 + 1, 1.0);
}

VkResult VK_Prof_WaitForFences(vkProfWait_t tag, uint32_t fenceCount, const VkFence *fences, VkBool32 waitAll,
                               uint64_t timeout)
{
    const uint64_t start = SDL_GetPerformanceCounter();
    const VkResult r = vkWaitForFences(vk.device, fenceCount, fences, waitAll, timeout);
    VK_Prof_NoteWait(tag, start);
    return r;
}

VkResult VK_Prof_QueueWaitIdle(vkProfWait_t tag, VkQueue queue)
{
    const uint64_t start = SDL_GetPerformanceCounter();
    const VkResult r = vkQueueWaitIdle(queue);
    VK_Prof_NoteWait(tag, start);
    return r;
}

VkResult VK_Prof_DeviceWaitIdle(vkProfWait_t tag)
{
    const uint64_t start = SDL_GetPerformanceCounter();
    const VkResult r = vkDeviceWaitIdle(vk.device);
    VK_Prof_NoteWait(tag, start);
    return r;
}

// ---------------------------------------------------------------------------
// Startup report
// ---------------------------------------------------------------------------

void VK_Prof_LogDeviceLimits(void)
{
    VkPhysicalDeviceProperties props = {};
    if (vk.physicalDevice != VK_NULL_HANDLE)
        vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);

    common->Printf("VK PROFILE LIMITS: maxMemoryAllocationCount=%u maxSamplerAllocationCount=%u | memoryBudget=%s "
                   "debugUtils=%s\n",
                   props.limits.maxMemoryAllocationCount, props.limits.maxSamplerAllocationCount,
                   vk.memoryBudgetSupported ? "yes" : "no", vk.debugUtilsSupported ? "yes" : "no");
}

void VK_Prof_NoteLazyImageLoad(const char *name, double ms)
{
    VK_Prof_Add(VKPROF_MIDFRAME_IMAGELOADS, 1.0);
    VK_Prof_Add(VKPROF_MIDFRAME_IMAGE_MS, ms);
    if (VK_Prof_Active() && ms >= 1.0)
        common->Printf("VK PROFILE MIDFRAME: image '%s' loaded during recording, %.2f ms\n", name ? name : "?", ms);
}

void VK_Prof_NotePipelineCreate(const char *name, double ms)
{
    VK_Prof_Add(VKPROF_MIDFRAME_PIPELINES, 1.0);
    VK_Prof_Add(VKPROF_MIDFRAME_PIPELINE_MS, ms);
    if (VK_Prof_Active())
        common->Printf("VK PROFILE MIDFRAME: pipeline '%s' created, %.2f ms\n", name ? name : "?", ms);
}

// ---------------------------------------------------------------------------
// Debug labels
// ---------------------------------------------------------------------------

void VK_Prof_InitDebugUtils(void)
{
    s_vkCmdBeginDebugUtilsLabelEXT = NULL;
    s_vkCmdEndDebugUtilsLabelEXT = NULL;
    s_vkSetDebugUtilsObjectNameEXT = NULL;

    if (!vk.debugUtilsSupported || vk.instance == VK_NULL_HANDLE)
        return;

    s_vkCmdBeginDebugUtilsLabelEXT =
        (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(vk.instance, "vkCmdBeginDebugUtilsLabelEXT");
    s_vkCmdEndDebugUtilsLabelEXT =
        (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(vk.instance, "vkCmdEndDebugUtilsLabelEXT");
    s_vkSetDebugUtilsObjectNameEXT =
        (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(vk.instance, "vkSetDebugUtilsObjectNameEXT");

    common->Printf("VK PROFILE: debug labels %s (r_vkDebugLabels to enable)\n",
                   s_vkCmdBeginDebugUtilsLabelEXT ? "available" : "unavailable");
}

bool VK_Prof_LabelsEnabled(void)
{
    return r_vkDebugLabels.GetBool() && s_vkCmdBeginDebugUtilsLabelEXT != NULL;
}

void VK_Prof_PushLabel(VkCommandBuffer cmd, const char *name)
{
    if (!VK_Prof_LabelsEnabled() || cmd == VK_NULL_HANDLE)
        return;
    VkDebugUtilsLabelEXT label = {};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name ? name : "(unnamed)";
    s_vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void VK_Prof_PopLabel(VkCommandBuffer cmd)
{
    if (!VK_Prof_LabelsEnabled() || cmd == VK_NULL_HANDLE || !s_vkCmdEndDebugUtilsLabelEXT)
        return;
    s_vkCmdEndDebugUtilsLabelEXT(cmd);
}

void VK_Prof_NameObject(uint64_t handle, VkObjectType type, const char *name)
{
    if (!s_vkSetDebugUtilsObjectNameEXT || !r_vkDebugLabels.GetBool() || handle == 0 || !name)
        return;
    VkDebugUtilsObjectNameInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    s_vkSetDebugUtilsObjectNameEXT(vk.device, &info);
}
