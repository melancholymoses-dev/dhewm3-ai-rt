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

idCVar r_rtReflGlow("r_rtReflGlow", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                    "Show in-flight projectiles (plasma, fireballs, ...) as glows in RT glass reflections.");
idCVar r_rtReflGlowGain("r_rtReflGlowGain", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE,
                        "Brightness multiplier for projectile glows in reflections (r_rtReflGlow).", 0.0f, 8.0f);
static idCVar r_rtReflGlowScale("r_rtReflGlowScale", "1.0", CVAR_RENDERER | CVAR_FLOAT,
                                "Radius multiplier for projectile glows in reflections.", 0.1f, 8.0f);
static idCVar r_rtReflGlowDist("r_rtReflGlowDist", "1024", CVAR_RENDERER | CVAR_FLOAT,
                               "Max distance from the camera for a projectile to glow in reflections.");
idCVar r_rtReflGlowSmoke("r_rtReflGlowSmoke", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
                         "With r_rtReflGlow, also show additive smoke-system particles (lost-soul flames, trails,\n"
                         "sparks) as glows in reflections. Clustered into discs; see r_rtReflGlowSmokeMinLum.");
static idCVar r_rtReflGlowSmokeMinLum("r_rtReflGlowSmokeMinLum", "0.3", CVAR_RENDERER | CVAR_FLOAT,
                                      "Smoke particles dimmer than this (vertex colour x stage rgb) don't glow.\n"
                                      "Raise if dim additive smoke hazes the glass.", 0.0f, 4.0f);
static idCVar r_rtReflGlowSmokeCell("r_rtReflGlowSmokeCell", "32", CVAR_RENDERER | CVAR_FLOAT,
                                    "Grid size (world units) for merging smoke particles into one glow disc.", 4.0f,
                                    256.0f);

// idSmokeParticles' shared render entity (game/SmokeParticles.cpp smokeParticle_SnapshotName).
static const char *SMOKE_MODEL_NAME = "_SmokeParticle_Snapshot_";
// Smoke this close to the camera is the player's own muzzle smoke; it would fill the glass.
static const float SMOKE_NEAR_SKIP = 32.0f;
static const int MAX_SMOKE_CLUSTERS = 256;

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
                              const idVec3 &tint, idVec3 &ioRgb, idImage *&ioImage, float &ioLum,
                              const shaderStage_t **ioStage = NULL)
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
            if (ioStage)
                *ioStage = stage;
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

// Brightness of a one-shot / limited-cycle stage at the view time (explosions), using the
// same stage age as idRenderModelPrt::InstantiateDynamicModel. Looping stages return 1.
// Approximation: one fade-in/fade-out ramp over the stage's lit span, life * (1 + bunching).
static float ParticleStageEnvelope(const idParticleStage *ps, const renderEntity_t &parms, const viewDef_t *viewDef)
{
    if (ps->cycles <= 0.0f || ps->cycleMsec <= 0)
        return 1.0f;
    const float lifeMs = ps->particleLife * 1000.0f;
    if (lifeMs <= 0.0f)
        return 0.0f;
    const float ageMs = (float)viewDef->renderView.time + parms.shaderParms[SHADERPARM_TIMEOFFSET] * 1000.0f -
                        ps->timeOffset * 1000.0f;
    if (ageMs < 0.0f)
        return 0.0f;

    const float spanMs = lifeMs * (1.0f + ps->spawnBunching);
    const float lastCycle = idMath::Ceil(ps->cycles) - 1.0f;
    const float cycle = Min(idMath::Floor(ageMs / (float)ps->cycleMsec), lastCycle);
    const float frac = (ageMs - cycle * (float)ps->cycleMsec) / spanMs;
    if (frac >= 1.0f)
        return 0.0f;

    float e = 1.0f;
    if (ps->fadeInFraction > 0.0f && frac < ps->fadeInFraction)
        e = frac / ps->fadeInFraction;
    if (ps->fadeOutFraction > 0.0f && frac > 1.0f - ps->fadeOutFraction)
        e = Min(e, (1.0f - frac) / ps->fadeOutFraction);
    return e;
}

// Particle decl (.prt model or deform particle surface): brightest additive stage, tinted
// by the stage colour and age envelope, radius from the stage's particle size. Updates the
// io values only when a stage beats ioLum.
static bool PickParticleStage(const idDeclParticle *prt, const renderEntity_t &parms, const viewDef_t *viewDef,
                              idVec3 &ioRgb, idImage *&ioImage, float &ioLum, const idMaterial *&ioMat,
                              float &ioRadius)
{
    bool found = false;
    for (int i = 0; i < prt->stages.Num(); i++)
    {
        const idParticleStage *ps = prt->stages[i];
        if (!ps || ps->hidden || !ps->material)
            continue;
        const float env = ParticleStageEnvelope(ps, parms, viewDef);
        if (env <= 0.0f)
            continue;
        if (PickAdditiveStage(ps->material, parms, viewDef, ps->color.ToVec3() * env, ioRgb, ioImage, ioLum))
        {
            ioMat = ps->material;
            ioRadius = Max(ps->size.from, ps->size.to);
            found = true;
        }
    }
    return found;
}

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
        const idDeclParticle *prt =
            static_cast<const idDeclParticle *>(declManager->FindType(DECL_PARTICLE, model->Name(), false));
        if (!prt || !PickParticleStage(prt, parms, viewDef, rgb, image, lum, chosenMat, radius))
            return false;
    }
    else
    {
        // Winner's surface bounds (model space); cleared = use the entity's bounds.
        idBounds winBounds;
        winBounds.Clear();
        bool winIsParticle = false;

        for (int i = 0; i < model->NumSurfaces(); i++)
        {
            const modelSurface_t *surf = model->Surface(i);
            const idMaterial *mat = parms.customShader ? parms.customShader : surf->shader;
            if (parms.customSkin && mat)
                mat = parms.customSkin->RemapShaderBySkin(mat);
            if (!mat)
                continue;

            // deform particle/particle2: the surface emits the decl's particles and its own
            // stages never draw (rocket exhaust 'zoom' -> rocketblast, image missing).
            const deform_t def = mat->Deform();
            if (def == DFRM_PARTICLE || def == DFRM_PARTICLE2)
            {
                const idDeclParticle *prt = static_cast<const idDeclParticle *>(mat->GetDeformDecl());
                float prtRadius = 0.0f;
                if (prt && PickParticleStage(prt, parms, viewDef, rgb, image, lum, chosenMat, prtRadius))
                {
                    radius = prtRadius;
                    winIsParticle = true;
                    if (surf->geometry)
                        winBounds = surf->geometry->bounds;
                    else
                        winBounds.Clear();
                }
                continue;
            }
            if (PickAdditiveStage(mat, parms, viewDef, idVec3(1.0f, 1.0f, 1.0f), rgb, image, lum))
            {
                chosenMat = mat;
                winIsParticle = false;
                winBounds.Clear();
            }
        }

        const idBounds &b = winBounds.IsCleared() ? ent->referenceBounds : winBounds;
        if (!b.IsCleared())
        {
            R_LocalPointToGlobal(ent->modelMatrix, b.GetCenter(), pos);
            // Half the largest extent: the size of a deform-sprite quad.
            if (!winIsParticle)
            {
                const idVec3 ext = b[1] - b[0];
                radius = 0.5f * Max(ext.x, Max(ext.y, ext.z));
            }
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

// Inserts c into list (sorted nearest first, capacity VK_RT_MAX_EMITTERS).
static void InsertNearest(emitterCandidate_t *list, int &num, const emitterCandidate_t &c)
{
    int at = num;
    while (at > 0 && list[at - 1].distSq > c.distSq)
        at--;
    if (at >= VK_RT_MAX_EMITTERS)
        return;
    const int last = Min(num, VK_RT_MAX_EMITTERS - 1);
    for (int k = last; k > at; k--)
        list[k] = list[k - 1];
    list[at] = c;
    num = Min(num + 1, VK_RT_MAX_EMITTERS);
}

struct smokeCluster_t
{
    int surf;
    int cell[3];
    idBounds bounds;
    idVec3 energy; // sum of quad rgb * r^2, so merged brightness is area-weighted
    idImage *image;
    const idMaterial *mat;
};

// The smoke entity's quads (rebuilt this frame by its game callback, world space) merged
// into discs: one per (surface, grid cell). Each surface is one particle stage.
static void CollectSmokeEmitters(const idRenderEntityLocal *ent, const viewDef_t *viewDef, float maxDistSq,
                                 emitterCandidate_t *outList, int &outNum, bool log)
{
    static smokeCluster_t clusters[MAX_SMOKE_CLUSTERS];
    int numClusters = 0;
    int numQuads = 0, numLit = 0, numDropped = 0;

    const idRenderModel *model = ent->parms.hModel;
    const idVec3 &viewOrg = viewDef->renderView.vieworg;
    const float minLum = r_rtReflGlowSmokeMinLum.GetFloat();
    const float cell = Max(4.0f, r_rtReflGlowSmokeCell.GetFloat());
    const float nearSq = SMOKE_NEAR_SKIP * SMOKE_NEAR_SKIP;

    for (int s = 0; s < model->NumSurfaces(); s++)
    {
        const modelSurface_t *surf = model->Surface(s);
        const srfTriangles_t *tri = surf->geometry;
        if (!surf->shader || !tri || !tri->verts || tri->numVerts < 4)
            continue;

        idVec3 stageRgb(0.0f, 0.0f, 0.0f);
        idImage *image = NULL;
        float stageLum = 0.0f;
        const shaderStage_t *stage = NULL;
        if (!PickAdditiveStage(surf->shader, ent->parms, viewDef, idVec3(1.0f, 1.0f, 1.0f), stageRgb, image,
                               stageLum, &stage))
            continue;
        const bool useVertColor = stage && stage->vertexColor != SVC_IGNORE;

        // Particle quads are 4 consecutive verts (idSmokeParticles::UpdateRenderEntity).
        for (int v = 0; v + 3 < tri->numVerts; v += 4)
        {
            numQuads++;
            const idDrawVert *q = &tri->verts[v];
            const idVec3 centre = (q[0].xyz + q[1].xyz + q[2].xyz + q[3].xyz) * 0.25f;

            idVec3 rgb = stageRgb;
            if (useVertColor)
            {
                const float k = 1.0f / (4.0f * 255.0f);
                rgb.x *= (q[0].color[0] + q[1].color[0] + q[2].color[0] + q[3].color[0]) * k;
                rgb.y *= (q[0].color[1] + q[1].color[1] + q[2].color[1] + q[3].color[1]) * k;
                rgb.z *= (q[0].color[2] + q[1].color[2] + q[2].color[2] + q[3].color[2]) * k;
            }
            if (0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z < minLum)
                continue;
            const float distSq = (centre - viewOrg).LengthSqr();
            if (distSq < nearSq || distSq > maxDistSq)
                continue;
            numLit++;

            float r2 = 0.0f;
            for (int k = 0; k < 4; k++)
                r2 = Max(r2, (q[k].xyz - centre).LengthSqr());
            const float r = idMath::Sqrt(r2);
            if (r <= 0.0f)
                continue;

            const int cx = (int)idMath::Floor(centre.x / cell);
            const int cy = (int)idMath::Floor(centre.y / cell);
            const int cz = (int)idMath::Floor(centre.z / cell);
            int c = 0;
            for (; c < numClusters; c++)
            {
                const smokeCluster_t &sc = clusters[c];
                if (sc.surf == s && sc.cell[0] == cx && sc.cell[1] == cy && sc.cell[2] == cz)
                    break;
            }
            if (c == numClusters)
            {
                if (numClusters >= MAX_SMOKE_CLUSTERS)
                {
                    numDropped++;
                    continue;
                }
                smokeCluster_t &sc = clusters[numClusters++];
                sc.surf = s;
                sc.cell[0] = cx;
                sc.cell[1] = cy;
                sc.cell[2] = cz;
                sc.bounds.Clear();
                sc.energy.Zero();
                sc.image = image;
                sc.mat = surf->shader;
            }
            smokeCluster_t &sc = clusters[c];
            sc.bounds.AddBounds(idBounds(centre - idVec3(r, r, r), centre + idVec3(r, r, r)));
            sc.energy += rgb * r2;
        }
    }

    for (int c = 0; c < numClusters; c++)
    {
        const smokeCluster_t &sc = clusters[c];
        const idVec3 ext = sc.bounds[1] - sc.bounds[0];
        const float radius = 0.5f * Max(ext.x, Max(ext.y, ext.z));
        if (radius <= 0.0f)
            continue;
        const idVec3 pos = sc.bounds.GetCenter();
        const idVec3 rgb = sc.energy / (radius * radius);

        emitterCandidate_t cand;
        cand.e.pos[0] = pos.x;
        cand.e.pos[1] = pos.y;
        cand.e.pos[2] = pos.z;
        cand.e.radius = radius * r_rtReflGlowScale.GetFloat();
        cand.e.rgb[0] = rgb.x;
        cand.e.rgb[1] = rgb.y;
        cand.e.rgb[2] = rgb.z;
        cand.e.texIndex = sc.image ? VK_RT_GetOrAssignTexIndex(sc.image) : 0u;
        cand.distSq = (pos - viewOrg).LengthSqr();
        cand.ent = ent;
        cand.matName = sc.mat->GetName();
        cand.imageName = sc.image ? sc.image->imgName.c_str() : "<procedural>";
        InsertNearest(outList, outNum, cand);
    }

    if (log)
    {
        common->Printf("VK RT Emitter smoke: surfaces=%d quads=%d lit=%d clusters=%d dropped=%d kept=%d\n",
                       model->NumSurfaces(), numQuads, numLit, numClusters, numDropped, outNum);
        for (int k = 0; k < Min(outNum, 8); k++)
            common->Printf("  smoke[%d] mtr='%s' r=%.1f rgb=(%.2f %.2f %.2f) dist=%.0f\n", k, outList[k].matName,
                           outList[k].e.radius, outList[k].e.rgb[0], outList[k].e.rgb[1], outList[k].e.rgb[2],
                           idMath::Sqrt(outList[k].distSq));
    }
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
    static emitterCandidate_t smokeKept[VK_RT_MAX_EMITTERS];
    int numKept = 0;
    int numSmoke = 0;
    int numTagged = 0;
    const bool wantSmoke = r_rtReflGlowSmoke.GetBool();

    const idRenderWorldLocal *world = viewDef->renderWorld;
    for (int i = 0; i < world->entityDefs.Num(); i++)
    {
        const idRenderEntityLocal *ent = world->entityDefs[i];
        if (!ent || !ent->parms.rtGlow || !ent->parms.hModel || ent->parms.weaponDepthHack)
            continue;
        numTagged++;

        if (idStr::Cmp(ent->parms.hModel->Name(), SMOKE_MODEL_NAME) == 0)
        {
            if (wantSmoke)
                CollectSmokeEmitters(ent, viewDef, maxDistSq, smokeKept, numSmoke, log);
            continue;
        }

        emitterCandidate_t c;
        if (!EvalEmitter(ent, viewDef, c))
        {
            if (log)
                common->Printf("VK RT Emitter: skip ent=%d model='%s' — nothing glowing (no additive stage, or blast finished)\n",
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
        InsertNearest(kept, numKept, c);
    }

    // Projectiles keep priority; smoke discs fill the remaining slots, nearest first.
    const int numProjectiles = numKept;
    for (int k = 0; k < numSmoke && numKept < VK_RT_MAX_EMITTERS; k++)
        kept[numKept++] = smokeKept[k];

    for (int k = 0; k < numKept; k++)
        s_emitterMapped[slot][k] = kept[k].e;
    s_emitterCount[slot] = (uint32_t)numKept;

    if (log)
        common->Printf("VK RT Emitters: tagged=%d projectiles=%d smoke=%d kept=%d slot=%d%s\n", numTagged,
                       numProjectiles, numKept - numProjectiles, numKept, slot,
                       numKept > 0 ? va(" first=(%.0f %.0f %.0f) r=%.1f", kept[0].e.pos[0], kept[0].e.pos[1],
                                        kept[0].e.pos[2], kept[0].e.radius)
                                   : "");
}
