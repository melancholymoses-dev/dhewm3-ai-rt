/*
===========================================================================

dhewm3-rt Vulkan ray tracing - reflection emitter list.

Collects in-flight projectiles (renderEntity_t::rtGlow, set by idProjectile) into a
per-frame-slot SSBO read by reflect_ray.rgen. Each emitter is one glowing disc:
position, radius, colour and texture of the entity's brightest additive material
stage, evaluated the same way the raster path evaluates it (EvaluateRegisters).
Projectiles never enter the TLAS, so no other RT pass can see them.
See docs/plans/20261002_reflection_emitters.md.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/DeclManager.h"
#include "framework/DeclParticle.h"
#include "framework/DeclSkin.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"
#include "renderer/Vulkan/vk_rt_emitters.h"

#include <string.h>

idCVar r_rtReflGlow("r_rtReflGlow", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                    "Show in-flight projectiles (plasma, fireballs, ...) as glows in RT glass reflections.");
idCVar r_rtReflGlowGain("r_rtReflGlowGain", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Brightness multiplier for projectile glows in reflections (r_rtReflGlow).", 0.0f, 8.0f);
static idCVar r_rtReflGlowScale("r_rtReflGlowScale", "1.0", CVAR_RENDERER | CVAR_FLOAT,
                                "Radius multiplier for projectile glows in reflections.", 0.1f, 8.0f);
static idCVar r_rtReflGlowDist("r_rtReflGlowDist", "1024", CVAR_RENDERER | CVAR_FLOAT,
                               "Max distance from the camera for a projectile to glow in reflections.");

extern idCVar r_vkLogRT;
extern idCVar r_rtReflectionDebugMode;
extern void VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                            VkBuffer *outBuffer, VkDeviceMemory *outMemory);

static VkBuffer s_emitterBuf[VK_MAX_FRAMES_IN_FLIGHT] = {};
static VkDeviceMemory s_emitterMem[VK_MAX_FRAMES_IN_FLIGHT] = {};
static vkRTEmitter_t *s_emitterMapped[VK_MAX_FRAMES_IN_FLIGHT] = {};
static uint32_t s_emitterCount[VK_MAX_FRAMES_IN_FLIGHT] = {};
static int s_emitterFrame[VK_MAX_FRAMES_IN_FLIGHT] = {-1, -1};

void VK_RT_InitReflEmitters(void)
{
    const VkDeviceSize size = sizeof(vkRTEmitter_t) * VK_RT_MAX_EMITTERS;
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (s_emitterBuf[i] != VK_NULL_HANDLE)
            continue;
        VK_CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_emitterBuf[i],
                        &s_emitterMem[i]);
        void *mapped = NULL;
        VK_CHECK(vkMapMemory(vk.device, s_emitterMem[i], 0, size, 0, &mapped));
        memset(mapped, 0, size);
        s_emitterMapped[i] = (vkRTEmitter_t *)mapped;
        s_emitterCount[i] = 0;
        s_emitterFrame[i] = -1;
    }
}

void VK_RT_ShutdownReflEmitters(void)
{
    for (int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (s_emitterMem[i] != VK_NULL_HANDLE)
        {
            vkUnmapMemory(vk.device, s_emitterMem[i]);
            vkFreeMemory(vk.device, s_emitterMem[i], NULL);
            s_emitterMem[i] = VK_NULL_HANDLE;
        }
        if (s_emitterBuf[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, s_emitterBuf[i], NULL);
            s_emitterBuf[i] = VK_NULL_HANDLE;
        }
        s_emitterMapped[i] = NULL;
        s_emitterCount[i] = 0;
    }
}

VkBuffer VK_RT_GetReflEmitterBuffer(int frameIdx)
{
    return s_emitterBuf[frameIdx];
}

uint32_t VK_RT_GetReflEmitterCount(int frameIdx)
{
    // A slot not rebuilt this frame holds last frame's projectiles.
    return (s_emitterFrame[frameIdx] == tr.frameCount) ? s_emitterCount[frameIdx] : 0u;
}

// Blend add (ONE, ONE) or (SRC_ALPHA, ONE): stages that only brighten, like raster glows.
static bool IsAdditiveStage(const shaderStage_t *stage)
{
    const int src = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const int dst = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return dst == GLS_DSTBLEND_ONE && (src == GLS_SRCBLEND_ONE || src == GLS_SRCBLEND_SRC_ALPHA);
}

// Brightest enabled additive stage of mat. tint multiplies the stage colour
// (particle stage colour for .prt). Returns false if mat has none.
static bool PickAdditiveStage(const idMaterial *mat, const renderEntity_t &parms, const viewDef_t *viewDef,
                              const idVec3 &tint, idVec3 &ioRgb, idImage *&ioImage, float &ioLum)
{
    if (!mat)
        return false;

    static idList<float> s_regs;
    s_regs.SetNum(Max(1, mat->GetNumRegisters()), false);
    float *regs = s_regs.Ptr();
    mat->EvaluateRegisters(regs, parms.shaderParms, viewDef, parms.referenceSound);

    bool found = false;
    for (int s = 0; s < mat->GetNumStages(); s++)
    {
        const shaderStage_t *stage = mat->GetStage(s);
        if (!regs[stage->conditionRegister] || !IsAdditiveStage(stage))
            continue;
        idVec3 rgb(regs[stage->color.registers[0]], regs[stage->color.registers[1]],
                   regs[stage->color.registers[2]]);
        if ((stage->drawStateBits & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_SRC_ALPHA)
            rgb *= regs[stage->color.registers[3]];
        rgb.x *= tint.x;
        rgb.y *= tint.y;
        rgb.z *= tint.z;
        const float lum = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
        if (lum > ioLum)
        {
            ioLum = lum;
            ioRgb = rgb;
            ioImage = stage->texture.image;
            found = true;
        }
    }
    return found;
}

struct emitterCandidate_t
{
    vkRTEmitter_t e;
    float distSq;
    const idRenderEntityLocal *ent;
    const char *matName;
    const char *imageName;
};

// Evaluates one tagged entity. Returns false if it has nothing that glows.
static bool EvalEmitter(const idRenderEntityLocal *ent, const viewDef_t *viewDef, emitterCandidate_t &out)
{
    const renderEntity_t &parms = ent->parms;
    idRenderModel *model = parms.hModel;

    idVec3 rgb(0.0f, 0.0f, 0.0f);
    idImage *image = NULL;
    const idMaterial *chosenMat = NULL;
    float lum = 0.0f;
    float radius = 0.0f;
    idVec3 pos = parms.origin;

    const bool isPrt =
        model->IsDynamicModel() == DM_CONTINUOUS && idStr::CheckExtension(model->Name(), ".prt");
    if (isPrt)
    {
        // Particle systems: one disc for the brightest additive stage, sized by that stage.
        const idDeclParticle *prt =
            static_cast<const idDeclParticle *>(declManager->FindType(DECL_PARTICLE, model->Name(), false));
        if (!prt)
            return false;
        for (int i = 0; i < prt->stages.Num(); i++)
        {
            const idParticleStage *ps = prt->stages[i];
            if (!ps || ps->hidden || !ps->material)
                continue;
            if (PickAdditiveStage(ps->material, parms, viewDef, ps->color.ToVec3(), rgb, image, lum))
            {
                chosenMat = ps->material;
                radius = Max(ps->size.from, ps->size.to);
            }
        }
    }
    else
    {
        for (int i = 0; i < model->NumSurfaces(); i++)
        {
            const modelSurface_t *surf = model->Surface(i);
            const idMaterial *mat = parms.customShader ? parms.customShader : surf->shader;
            if (parms.customSkin && mat)
                mat = parms.customSkin->RemapShaderBySkin(mat);
            if (PickAdditiveStage(mat, parms, viewDef, idVec3(1.0f, 1.0f, 1.0f), rgb, image, lum))
                chosenMat = mat;
        }
        // Half the largest extent: the size of a deform-sprite quad.
        const idBounds &b = ent->referenceBounds;
        if (!b.IsCleared())
        {
            const idVec3 ext = b[1] - b[0];
            radius = 0.5f * Max(ext.x, Max(ext.y, ext.z));
            R_LocalPointToGlobal(ent->modelMatrix, b.GetCenter(), pos);
        }
    }

    if (!chosenMat || lum <= 0.0f || radius <= 0.0f)
        return false;

    out.e.pos[0] = pos.x;
    out.e.pos[1] = pos.y;
    out.e.pos[2] = pos.z;
    out.e.radius = radius * r_rtReflGlowScale.GetFloat();
    out.e.rgb[0] = rgb.x;
    out.e.rgb[1] = rgb.y;
    out.e.rgb[2] = rgb.z;
    out.e.texIndex = image ? VK_RT_GetOrAssignTexIndex(image) : 0u;
    out.distSq = (pos - viewDef->renderView.vieworg).LengthSqr();
    out.ent = ent;
    out.matName = chosenMat->GetName();
    out.imageName = image ? image->imgName.c_str() : "<procedural>";
    return true;
}

void VK_RT_BuildReflEmitters(const viewDef_t *viewDef)
{
    const int slot = vk.currentFrame;
    if (!viewDef || !viewDef->renderWorld || s_emitterMapped[slot] == NULL)
        return;
    // Same gate as the reflection dispatch: only the primary 3D view.
    if (viewDef->isSubview || viewDef->isMirror || viewDef->renderView.viewaxis[0].LengthSqr() <= 0.0001f)
        return;
    if (s_emitterFrame[slot] == tr.frameCount)
        return;
    s_emitterFrame[slot] = tr.frameCount;
    s_emitterCount[slot] = 0;

    if (!r_rtReflGlow.GetBool() && r_rtReflectionDebugMode.GetInteger() != 8)
        return;

    // Once a second at r_vkLogRT 1: the E0 inventory.
    static unsigned int s_lastLogMs = 0;
    const unsigned int nowMs = Sys_Milliseconds();
    const bool log = r_vkLogRT.GetInteger() >= 1 && (nowMs - s_lastLogMs) >= 1000;
    if (log)
        s_lastLogMs = nowMs;

    const float maxDist = Max(0.0f, r_rtReflGlowDist.GetFloat());
    const float maxDistSq = maxDist * maxDist;

    // Nearest VK_RT_MAX_EMITTERS, kept sorted by distance (insertion; counts are small).
    static emitterCandidate_t kept[VK_RT_MAX_EMITTERS];
    int numKept = 0;
    int numTagged = 0;

    const idRenderWorldLocal *world = viewDef->renderWorld;
    for (int i = 0; i < world->entityDefs.Num(); i++)
    {
        const idRenderEntityLocal *ent = world->entityDefs[i];
        if (!ent || !ent->parms.rtGlow || !ent->parms.hModel || ent->parms.weaponDepthHack)
            continue;
        numTagged++;

        emitterCandidate_t c;
        if (!EvalEmitter(ent, viewDef, c))
        {
            if (log)
                common->Printf("VK RT Emitter: skip ent=%d model='%s' — no additive stage\n",
                               ent->parms.entityNum, ent->parms.hModel->Name());
            continue;
        }
        if (log)
            common->Printf("VK RT Emitter: ent=%d model='%s' mtr='%s' img='%s' tex=%u r=%.1f rgb=(%.2f %.2f %.2f) "
                           "dist=%.0f\n",
                           ent->parms.entityNum, ent->parms.hModel->Name(), c.matName, c.imageName, c.e.texIndex,
                           c.e.radius, c.e.rgb[0], c.e.rgb[1], c.e.rgb[2], idMath::Sqrt(c.distSq));
        if (c.distSq > maxDistSq)
            continue;

        int at = numKept;
        while (at > 0 && kept[at - 1].distSq > c.distSq)
            at--;
        if (at >= VK_RT_MAX_EMITTERS)
            continue;
        const int last = Min(numKept, VK_RT_MAX_EMITTERS - 1);
        for (int k = last; k > at; k--)
            kept[k] = kept[k - 1];
        kept[at] = c;
        numKept = Min(numKept + 1, VK_RT_MAX_EMITTERS);
    }

    for (int k = 0; k < numKept; k++)
        s_emitterMapped[slot][k] = kept[k].e;
    s_emitterCount[slot] = (uint32_t)numKept;

    if (log)
        common->Printf("VK RT Emitters: tagged=%d kept=%d slot=%d%s\n", numTagged, numKept, slot,
                       numKept > 0 ? va(" first=(%.0f %.0f %.0f) r=%.1f", kept[0].e.pos[0], kept[0].e.pos[1],
                                        kept[0].e.pos[2], kept[0].e.radius)
                                   : "");
}
