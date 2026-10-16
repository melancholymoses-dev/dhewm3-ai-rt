/*
===========================================================================

dhewm3-rt Vulkan — vk_gui_texture.cpp — live in-world GUI textures for RT reflections
(docs/plans/20261015_gui_reflections.md Stage 1).

In-world GUI screens only exist as per-view quads, so a reflection ray has nothing to
sample. Each frame, R_RenderGuiSurf hands its freshly redrawn tr.guiModel to
VK_GuiTex_CaptureFromGuiModel, which builds a second 640x480 ortho view of the same
quads and queues it against the GUI's slot. The backend renders the queue at the start
of the frame into a private 512x384 target whose render pass is compatible with
vk.hdrRenderPass (so VK_RB_DrawShaderPasses and its pipelines are reused unchanged),
then blits into the slot's mipped RGBA8 idImage.

Only GUIs drawn in the primary view are captured, so cost scales with the screens on
screen and is zero elsewhere. r_rtGuiTexInterval trades refresh rate for cost.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/CmdSystem.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/GuiModel.h"
#include "ui/UserInterface.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_image.h"
#include "renderer/Vulkan/vk_profile.h"
#include "renderer/Vulkan/vk_gui_texture.h"

#include <string.h>

extern idCVar r_useRayTracing;
extern idCVar r_rtReflections;
extern idCVar r_vkLogRT;

idCVar r_rtGuiReflections("r_rtGuiReflections", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                          "Render in-world GUI screens into textures so RT reflections can show them.");
idCVar r_rtGuiTexInterval("r_rtGuiTexInterval", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
                          "Frames between refreshes of each GUI reflection texture. 1 = every frame.", 1, 60);
idCVar r_rtGuiTexRadius("r_rtGuiTexRadius", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Entity GUIs within this distance of the camera keep refreshing while off screen, "
                        "nearest first, so reflections of them stay live. 0 = only GUIs drawn on screen.");

// ---------------------------------------------------------------------------
// Slots (frontend)
// ---------------------------------------------------------------------------

struct guiTexSlot_t
{
    const idUserInterface *gui; // identity only; may dangle after a level change
    idStr guiName;
    int lastSeenFrame;   // last frame a surface showing this GUI was drawn
    int renderedFrame;   // last frame it was queued for render; -1 = never
    idImage *image;
};

static guiTexSlot_t s_slots[VK_GUITEX_SLOTS];

struct guiTexPending_t
{
    viewDef_t *view;
    int slot;
};

static guiTexPending_t s_pending[VK_GUITEX_SLOTS];
static int s_pendingCount = 0;
static int s_pendingFrame = -1;

static bool VK_GuiTex_Enabled(void)
{
    return glConfig.isVulkan && vk.rayTracingSupported && r_useRayTracing.GetBool() && r_rtReflections.GetBool() &&
           r_rtGuiReflections.GetBool();
}

static void R_GuiTexPlaceholder(idImage *image)
{
    // Real size comes from VK_Image_Upload in VK_GuiTex_BeginPass; GenerateImage would round to POT.
    // Repeat, not clamp: R_SurfaceToTextureAxis maps the GUI to frac(st), so a screen's
    // UVs may span 1..2.
    byte data[8 * 8 * 4];
    memset(data, 0, sizeof(data));
    image->GenerateImage(data, 8, 8, TF_DEFAULT, false, TR_REPEAT, TD_HIGH_QUALITY);
}

idImage *VK_GuiTex_SlotImage(int slot)
{
    if (slot < 0 || slot >= VK_GUITEX_SLOTS)
        return NULL;
    if (!s_slots[slot].image)
        s_slots[slot].image = globalImages->ImageFromFunction(va("_guiTex%d", slot), R_GuiTexPlaceholder);
    return s_slots[slot].image;
}

int VK_GuiTex_FindSlot(const idUserInterface *gui)
{
    // Disabled reads as "no slot", so r_rtGuiReflections 0 also unbinds the materials (A/B).
    if (!gui || !VK_GuiTex_Enabled())
        return -1;
    for (int i = 0; i < VK_GUITEX_SLOTS; i++)
    {
        if (s_slots[i].gui == gui)
            return i;
    }
    return -1;
}

// Existing slot, else a free one, else the least recently seen one not used this frame.
static int VK_GuiTex_AcquireSlot(const idUserInterface *gui)
{
    int slot = VK_GuiTex_FindSlot(gui);
    if (slot >= 0)
    {
        s_slots[slot].lastSeenFrame = tr.frameCount;
        return slot;
    }

    for (int i = 0; i < VK_GUITEX_SLOTS; i++)
    {
        if (s_slots[i].gui == NULL)
        {
            slot = i;
            break;
        }
        if (s_slots[i].lastSeenFrame != tr.frameCount &&
            (slot < 0 || s_slots[i].lastSeenFrame < s_slots[slot].lastSeenFrame))
            slot = i;
    }
    if (slot < 0)
    {
        static int s_fullLogFrame = -1;
        if (r_vkLogRT.GetInteger() >= 1 && s_fullLogFrame != tr.frameCount)
        {
            s_fullLogFrame = tr.frameCount;
            common->Printf("VK GUITEX: pool full (%d), '%s' not captured\n", VK_GUITEX_SLOTS, gui->Name());
        }
        return -1;
    }

    guiTexSlot_t &s = s_slots[slot];
    if (r_vkLogRT.GetInteger() >= 1)
    {
        common->Printf("VK GUITEX: slot %d <- '%s'%s%s\n", slot, gui->Name(), s.gui ? " evicting " : "",
                       s.gui ? s.guiName.c_str() : "");
    }
    s.gui = gui;
    s.guiName = gui->Name();
    s.lastSeenFrame = tr.frameCount;
    s.renderedFrame = -1;
    VK_GuiTex_SlotImage(slot);
    return slot;
}

// True if AcquireSlot could place a new GUI this frame without failing.
static bool VK_GuiTex_CanAcquire(void)
{
    for (int i = 0; i < VK_GUITEX_SLOTS; i++)
    {
        if (s_slots[i].gui == NULL || s_slots[i].lastSeenFrame != tr.frameCount)
            return true;
    }
    return false;
}

void VK_GuiTex_CaptureFromGuiModel(const idUserInterface *gui)
{
    // Top-level GUIs in the primary view only: subviews and nested GUIs would redraw
    // the same screen from another pass, and the 2D overlay has no world camera.
    if (!gui || !tr.viewDef || tr.viewDef->isSubview || tr.guiRecursionLevel != 1 ||
        tr.viewDef->renderView.viewaxis[0].LengthSqr() <= 0.0001f || !VK_GuiTex_Enabled())
        return;

    if (s_pendingFrame != tr.frameCount)
    {
        s_pendingFrame = tr.frameCount;
        s_pendingCount = 0;
    }

    const int slot = VK_GuiTex_AcquireSlot(gui);
    if (slot < 0)
        return;

    // Several surfaces can show one GUI; one render per refresh covers them all.
    guiTexSlot_t &s = s_slots[slot];
    if (s.renderedFrame >= 0 && tr.frameCount - s.renderedFrame < r_rtGuiTexInterval.GetInteger())
        return;
    if (s_pendingCount >= VK_GUITEX_SLOTS)
        return;

    viewDef_t *view = tr.guiModel->EmitToTextureView(VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT);
    if (!view)
        return;

    s.renderedFrame = tr.frameCount;
    s_pending[s_pendingCount].view = view;
    s_pending[s_pendingCount].slot = slot;
    s_pendingCount++;
}

struct guiTexCandidate_t
{
    idUserInterface *gui;
    float distSq;
};

static int VK_GuiTex_CompareCandidates(const void *a, const void *b)
{
    const float da = ((const guiTexCandidate_t *)a)->distSq;
    const float db = ((const guiTexCandidate_t *)b)->distSq;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

// Frontend, primary view, after R_AddModelSurfaces: the on-screen GUIs were captured by
// R_RenderGuiSurf and hold their slots for this frame. Entity GUIs within
// r_rtGuiTexRadius that the view culled are redrawn here nearest first, until the pool is
// full, so a screen seen only in a reflection keeps animating. Nearest-first keeps the
// set stable: off-screen GUIs never evict each other within a frame.
void VK_GuiTex_RefreshNearby(idRenderWorldLocal *world)
{
    const float radius = r_rtGuiTexRadius.GetFloat();
    if (!world || !tr.viewDef || radius <= 0.0f || !VK_GuiTex_Enabled() ||
        tr.viewDef->renderView.viewaxis[0].LengthSqr() <= 0.0001f)
        return;

    static const int MAX_CANDIDATES = 64;
    guiTexCandidate_t cands[MAX_CANDIDATES];
    int numCands = 0;

    const idVec3 &org = tr.viewDef->renderView.vieworg;
    const float radiusSq = radius * radius;
    for (int i = 0; i < world->entityDefs.Num() && numCands < MAX_CANDIDATES; i++)
    {
        const idRenderEntityLocal *ent = world->entityDefs[i];
        if (!ent)
            continue;
        const float distSq = (ent->parms.origin - org).LengthSqr();
        if (distSq > radiusSq)
            continue;
        for (int g = 0; g < MAX_RENDERENTITY_GUI && numCands < MAX_CANDIDATES; g++)
        {
            if (ent->parms.gui[g])
            {
                cands[numCands].gui = ent->parms.gui[g];
                cands[numCands].distSq = distSq;
                numCands++;
            }
        }
    }
    qsort(cands, numCands, sizeof(cands[0]), VK_GuiTex_CompareCandidates);

    int redrawn = 0, skippedFull = 0;
    for (int c = 0; c < numCands; c++)
    {
        idUserInterface *gui = cands[c].gui;
        // Drawn on screen this frame, or not due yet: keep the slot, skip the Redraw.
        const int slot = VK_GuiTex_FindSlot(gui);
        if (slot >= 0 && s_slots[slot].renderedFrame >= 0 &&
            tr.frameCount - s_slots[slot].renderedFrame < r_rtGuiTexInterval.GetInteger())
        {
            s_slots[slot].lastSeenFrame = tr.frameCount;
            continue;
        }
        if (slot < 0 && !VK_GuiTex_CanAcquire())
        {
            skippedFull = numCands - c;
            break;
        }

        // Same bracket as R_RenderGuiSurf, minus the on-surface emit.
        tr.guiRecursionLevel++;
        tr.guiModel->Clear();
        gui->Redraw(tr.viewDef->renderView.time);
        VK_GuiTex_CaptureFromGuiModel(gui);
        tr.guiModel->Clear();
        tr.guiRecursionLevel--;
        redrawn++;
    }

    if ((redrawn > 0 || skippedFull > 0) && r_vkLogRT.GetInteger() >= 2)
        common->Printf("VK GUITEX: frame=%d off-screen candidates=%d redrew=%d skipped(pool full)=%d\n",
                       tr.frameCount, numCands, redrawn, skippedFull);
}

int VK_GuiTex_PendingCount(void)
{
    return (s_pendingFrame == tr.frameCount) ? s_pendingCount : 0;
}

viewDef_t *VK_GuiTex_Pending(int index, int *outSlot)
{
    if (s_pendingFrame != tr.frameCount || index < 0 || index >= s_pendingCount)
        return NULL;
    if (outSlot)
        *outSlot = s_pending[index].slot;
    return s_pending[index].view;
}

void VK_GuiTex_ClearPending(void)
{
    s_pendingCount = 0;
}

void VK_GuiTex_BeginLevelLoad(void)
{
    // Images survive (they are referenced outside level load); only the mapping resets.
    for (int i = 0; i < VK_GUITEX_SLOTS; i++)
    {
        s_slots[i].gui = NULL;
        s_slots[i].guiName.Clear();
        s_slots[i].lastSeenFrame = -1;
        s_slots[i].renderedFrame = -1;
    }
    s_pendingCount = 0;
}

// ---------------------------------------------------------------------------
// Offscreen target (backend)
// ---------------------------------------------------------------------------

struct guiTexImage_t
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

// Attachment order matches vk.hdrRenderPass: colour, depth, gbufNormal, gbufAlbedo, motion.
static guiTexImage_t s_color, s_depth, s_aux[3];
static VkRenderPass s_renderPass = VK_NULL_HANDLE;
static VkFramebuffer s_framebuffer = VK_NULL_HANDLE;
static bool s_gpuReady = false;
static bool s_gpuFailed = false;

static bool VK_GuiTex_DepthHasStencil(void)
{
    return vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || vk.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
           vk.depthFormat == VK_FORMAT_D16_UNORM_S8_UINT;
}

static bool VK_GuiTex_CreateImage(guiTexImage_t &out, VkFormat format, VkImageUsageFlags usage,
                                  VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &ci, NULL, &out.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, out.image, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &ai, NULL, &out.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(vk.device, out.image, out.memory, 0);

    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(vk.device, &vi, NULL, &out.view) == VK_SUCCESS;
}

static void VK_GuiTex_DestroyImage(guiTexImage_t &img)
{
    if (img.view != VK_NULL_HANDLE)
        vkDestroyImageView(vk.device, img.view, NULL);
    if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(vk.device, img.image, NULL);
    if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(vk.device, img.memory, NULL);
    memset(&img, 0, sizeof(img));
}

// Same formats, sample counts and subpass as vk_swapchain.cpp's hdrRenderPass, which is
// all render-pass compatibility needs; load/store ops and layouts are free to differ.
static bool VK_GuiTex_CreateRenderPass(void)
{
    VkAttachmentDescription att[5] = {};
    for (int i = 0; i < 5; i++)
    {
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    att[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].format = vk.depthFormat;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[3].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[4].format = VK_FORMAT_R16G16_SFLOAT;

    VkAttachmentReference colorRefs[VK_HDR_COLOR_ATTACHMENT_COUNT] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = vk.gbufferSupported ? VK_HDR_COLOR_ATTACHMENT_COUNT : 1;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    // Identical to hdrRenderPass's dependency; the hazards against the previous slot's
    // blit are covered by explicit barriers in BeginPass / EndPassAndCopy.
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp = {};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = vk.gbufferSupported ? 5 : 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    return vkCreateRenderPass(vk.device, &rp, NULL, &s_renderPass) == VK_SUCCESS;
}

static bool VK_GuiTex_InitGpu(void)
{
    if (s_gpuReady)
        return true;
    if (s_gpuFailed)
        return false;

    const VkImageAspectFlags depthAspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (VK_GuiTex_DepthHasStencil() ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    bool ok = VK_GuiTex_CreateImage(s_color, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT) &&
              VK_GuiTex_CreateImage(s_depth, vk.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthAspect);
    if (ok && vk.gbufferSupported)
    {
        const VkFormat auxFormats[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT};
        for (int i = 0; i < 3 && ok; i++)
            ok = VK_GuiTex_CreateImage(s_aux[i], auxFormats[i], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
    }
    ok = ok && VK_GuiTex_CreateRenderPass();
    if (ok)
    {
        VkImageView views[5] = {s_color.view, s_depth.view, s_aux[0].view, s_aux[1].view, s_aux[2].view};
        VkFramebufferCreateInfo fb = {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = s_renderPass;
        fb.attachmentCount = vk.gbufferSupported ? 5 : 2;
        fb.pAttachments = views;
        fb.width = VK_GUITEX_WIDTH;
        fb.height = VK_GUITEX_HEIGHT;
        fb.layers = 1;
        ok = vkCreateFramebuffer(vk.device, &fb, NULL, &s_framebuffer) == VK_SUCCESS;
    }

    if (!ok)
    {
        common->Warning("VK GUITEX: offscreen target creation failed; GUI reflection textures disabled");
        s_gpuFailed = true;
        return false;
    }
    s_gpuReady = true;
    return true;
}

void VK_GuiTex_Shutdown(void)
{
    if (s_framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(vk.device, s_framebuffer, NULL);
    if (s_renderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(vk.device, s_renderPass, NULL);
    s_framebuffer = VK_NULL_HANDLE;
    s_renderPass = VK_NULL_HANDLE;
    VK_GuiTex_DestroyImage(s_color);
    VK_GuiTex_DestroyImage(s_depth);
    for (int i = 0; i < 3; i++)
        VK_GuiTex_DestroyImage(s_aux[i]);
    s_gpuReady = false;
    s_gpuFailed = false;
    s_pendingCount = 0;
}

static uint32_t VK_GuiTex_MipLevels(void)
{
    uint32_t levels = 1;
    for (int d = VK_GUITEX_WIDTH > VK_GUITEX_HEIGHT ? VK_GUITEX_WIDTH : VK_GUITEX_HEIGHT; d > 1; d >>= 1)
        levels++;
    return levels;
}

static VkPipelineStageFlags VK_GuiTex_SampleStages(void)
{
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (vk.rayTracingSupported)
        stages |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    return stages;
}

static void VK_GuiTex_ImageBarrier(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mipCount,
                                   VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess,
                                   VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                                   VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

bool VK_GuiTex_BeginPass(VkCommandBuffer cmd, int slot)
{
    if (!VK_GuiTex_InitGpu())
        return false;

    // Size the slot image on first use, or after a reloadImages/vid_restart regenerated
    // the 8x8 placeholder. Same pattern as VK_RB_CopyRender.
    idImage *img = VK_GuiTex_SlotImage(slot);
    if (!img)
        return false;
    int w = 0, h = 0;
    if (!img->backendData || !VK_Image_GetExtent(img, &w, &h) || w != VK_GUITEX_WIDTH || h != VK_GUITEX_HEIGHT)
    {
        const size_t bytes = (size_t)VK_GUITEX_WIDTH * VK_GUITEX_HEIGHT * 4u;
        byte *blank = (byte *)Mem_ClearedAlloc((int)bytes);
        VK_Image_Upload(img, blank, VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT);
        Mem_Free(blank);
        img->uploadWidth = VK_GUITEX_WIDTH;
        img->uploadHeight = VK_GUITEX_HEIGHT;
        VK_FlushPendingUploads();
        if (!VK_Image_GetExtent(img, &w, &h))
            return false;
    }

    // The previous slot's blit still reads s_color, and its attachments were just written.
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);

    // Alpha 0: texels the GUI never draws stay uncovered, and the reflection any-hit lets
    // the ray through to the housing there, as the raster blend does.
    VkClearValue clears[5] = {};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].depthStencil = {1.0f, 128};

    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = s_renderPass;
    rb.framebuffer = s_framebuffer;
    rb.renderArea.extent = {VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT};
    rb.clearValueCount = vk.gbufferSupported ? 5 : 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    return true;
}

void VK_GuiTex_EndPassAndCopy(VkCommandBuffer cmd, int slot)
{
    vkCmdEndRenderPass(cmd);

    VkImage dst = VK_NULL_HANDLE;
    if (!VK_Image_GetHandle(VK_GuiTex_SlotImage(slot), &dst) || dst == VK_NULL_HANDLE)
        return;

    const uint32_t mips = VK_GuiTex_MipLevels();
    const VkPipelineStageFlags sampleStages = VK_GuiTex_SampleStages();

    VK_GuiTex_ImageBarrier(cmd, s_color.image, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
    VK_GuiTex_ImageBarrier(cmd, dst, 0, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, sampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // RGBA16F -> RGBA8 at equal size; values above 1 clamp (Stage 3 owns brightness).
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {VK_GUITEX_WIDTH, VK_GUITEX_HEIGHT, 1};
    vkCmdBlitImage(cmd, s_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);

    // Mip chain, as VK_Image_Upload builds it; reflections sample these at distance.
    int mw = VK_GUITEX_WIDTH, mh = VK_GUITEX_HEIGHT;
    for (uint32_t i = 1; i < mips; i++)
    {
        VK_GuiTex_ImageBarrier(cmd, dst, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
        const int nw = mw > 1 ? mw >> 1 : 1;
        const int nh = mh > 1 ? mh >> 1 : 1;
        VkImageBlit mb = {};
        mb.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        mb.srcOffsets[1] = {mw, mh, 1};
        mb.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        mb.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mb,
                       VK_FILTER_LINEAR);
        VK_GuiTex_ImageBarrier(cmd, dst, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                               VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, sampleStages);
        mw = nw;
        mh = nh;
    }
    VK_GuiTex_ImageBarrier(cmd, dst, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, sampleStages);
}

// ---------------------------------------------------------------------------
// r_vkGuiTextureDump <slot> — writes mip 0 of a slot to guitex_<slot>.tga
// ---------------------------------------------------------------------------

void VK_GuiTex_Dump_f(const idCmdArgs &args)
{
    if (!glConfig.isVulkan)
    {
        common->Printf("r_vkGuiTextureDump: Vulkan backend only\n");
        return;
    }
    if (args.Argc() < 2)
    {
        for (int i = 0; i < VK_GUITEX_SLOTS; i++)
        {
            if (s_slots[i].gui)
                common->Printf("  slot %2d: '%s' seen %d rendered %d\n", i, s_slots[i].guiName.c_str(),
                               s_slots[i].lastSeenFrame, s_slots[i].renderedFrame);
        }
        common->Printf("usage: r_vkGuiTextureDump <slot>\n");
        return;
    }
    const int slot = atoi(args.Argv(1));
    idImage *img = (slot >= 0 && slot < VK_GUITEX_SLOTS) ? s_slots[slot].image : NULL;
    VkImage src = VK_NULL_HANDLE;
    int w = 0, h = 0;
    if (!img || !VK_Image_GetHandle(img, &src) || !VK_Image_GetExtent(img, &w, &h) || w != VK_GUITEX_WIDTH ||
        h != VK_GUITEX_HEIGHT)
    {
        common->Printf("r_vkGuiTextureDump: slot %d has no rendered texture\n", slot);
        return;
    }

    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk.device, &bi, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.device, buf, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk.device, &ai, NULL, &mem));
    vkBindBufferMemory(vk.device, buf, mem, 0);

    // Console commands run between frames; idle the device so the slot is not mid-update.
    VK_Prof_DeviceWaitIdle(VKPROF_WAIT_READBACK);
    VkCommandBuffer cmd = VK_BeginSingleTimeCommands();
    VK_GuiTex_ImageBarrier(cmd, src, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VK_GuiTex_ImageBarrier(cmd, src, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_GuiTex_SampleStages());
    VK_EndSingleTimeCommands(cmd);
    VK_FlushPendingUploads(); // submits the batch and waits on its fence

    void *mapped = NULL;
    vkMapMemory(vk.device, mem, 0, bytes, 0, &mapped);
    const idStr name = va("guitex_%d.tga", slot);
    R_WriteTGA(name.c_str(), (const byte *)mapped, w, h);
    vkUnmapMemory(vk.device, mem);
    vkDestroyBuffer(vk.device, buf, NULL);
    vkFreeMemory(vk.device, mem, NULL);
    common->Printf("r_vkGuiTextureDump: wrote %s ('%s', %dx%d)\n", name.c_str(), s_slots[slot].guiName.c_str(), w, h);
}
