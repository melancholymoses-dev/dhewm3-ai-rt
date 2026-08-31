/*
===========================================================================

dhewm3-rt Vulkan ray tracing — auto-relight (docs/plans/auto_relight.md AR1-5).

Load-time CPU pass that turns Doom 3's glowing fixtures (LED screens, light
panels, strip lights, consoles) into real, shadow-casting idRenderLights with
zero map editing. The pipeline:

  AR1  Surface harvest  — walk every world-model surface, keep triangles
       whose material is emissive (shared judgment with the material table,
       VK_RT_MaterialIsEmissive), and bucket them into a world-space grid.
  AR2  Clustering        — union adjacent occupied grid cells (6-connectivity)
       into physical fixtures; reject clusters whose triangle normals
       disagree too much (wraparound glow strips).
  AR3a Scoring (AR_Score)      — score = area x emissive luminance x style
       weight, tag each cluster with its resolved portal area.
  AR4  Dedupe (AR_Dedupe)      — drop clusters within r_rtAutoRelightDedupeDist
       of an existing, color-similar map light (the mappers already lit it).
       Runs BEFORE the budget caps below (2026-08-23, code review finding —
       used to run after: a room's highest-scoring fixtures could consume its
       whole quota and only THEN get rejected as already-lit, starving
       genuinely-uncovered lower-scored fixtures in the same room of a slot
       they should have won).
  AR3b Budget (AR_ApplyBudget) — over whatever AR4 left as CANDIDATE, keep the
       top r_rtAutoRelightMaxPerArea clusters PER PORTAL AREA, then the top
       r_rtAutoRelightMax clusters map-wide as an overall ceiling (2026-08-23:
       was a single map-wide cap, which let one or two large/bright fixture
       clusters starve every other room's budget — see the cvar comment above
       AR_ApplyBudget for the in-game finding).
  AR5  Light synthesis   — AddLightDef() a real point light per surviving
       cluster: origin offset off the surface, a world-axis-aligned bound of
       the fixture's (tangent x2.5, normal reach) footprint as lightRadius,
       average bright-texel color scaled by r_rtAutoRelightIntensity (baked into
       the RGB parms — slot 3 on a light is TIMESCALE, not intensity; 2026-08-30).
       Always shadow-casting (noShadows = false) — see AR5 comment below for
       why that matters. Tagged with a distinct light material
       (lights/rtAutoRelight, base/materials/rt_auto_relight.mtr) instead of
       the generic default, so debug dumps can identify them (2026-08-23).

AR6 (noShadows unlock) and AR7 (weapon/projectile def patches) are separate
companion rules per the doc, not implemented here.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/RenderWorld_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

#include "idlib/containers/HashIndex.h"

#include <string.h>

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------

idCVar r_rtAutoRelight("r_rtAutoRelight", "1", CVAR_RENDERER | CVAR_BOOL,
                       "auto_relight.md: master toggle for engine-synthesized lights from emissive "
                       "world surfaces; regenerates on map load.");

idCVar r_rtAutoRelightDebug("r_rtAutoRelightDebug", "1", CVAR_RENDERER | CVAR_INTEGER,
                            "auto_relight.md: 0 = off; 1 = console table of every harvested cluster "
                            "(centroid/area/score/verdict/material) printed once, at the next map load. "
                            "Independent of r_rtAutoRelight so the harvest/cluster/score step can be "
                            "validated before enabling synthesis.");

idCVar r_rtAutoRelightMax("r_rtAutoRelightMax", "64", CVAR_RENDERER | CVAR_INTEGER,
                          "auto_relight.md AR3: overall map-wide ceiling, applied AFTER the per-area cap below "
                          "(safety valve for maps with many areas, not the primary budget knob anymore).");

// 2026-08-23: added after in-game validation on alphalabs2 showed the single global
// top-16-by-score cap letting two physically large/bright fixture clusters (a pulsing
// floor grate, a wall light bank) consume the ENTIRE map's budget between them, leaving
// every other room with zero synthesized lights despite having plenty of emissive
// fixtures of their own (score = area * luminance * styleWeight lets sheer physical
// size dominate). This is the primary budget knob now: score/keep top-N PER PORTAL
// AREA (world->PointInArea on the cluster centroid) so "a few per scene" actually
// holds as the player moves between rooms, instead of two rooms eating the whole map.
idCVar r_rtAutoRelightMaxPerArea("r_rtAutoRelightMaxPerArea", "4", CVAR_RENDERER | CVAR_INTEGER,
                                 "auto_relight.md AR3: max synthesized lights kept per portal area, by "
                                 "descending score, before the map-wide r_rtAutoRelightMax ceiling is applied.");

idCVar r_rtAutoRelightIntensity("r_rtAutoRelightIntensity", "0.5", CVAR_RENDERER | CVAR_FLOAT,
                                "auto_relight.md AR5: brightness scale for synthesized lights, multiplied "
                                "into their RGB — start dim, these are accents, not primary illumination. "
                                "(Was written to SHADERPARM_ALPHA until 2026-08-30, which made it a "
                                "GI/volumetric-only control with no effect on direct lighting.)");

// 2026-08-23: temporary debug visualization, not a gameplay setting — forces every
// synthesized light to hot pink at a high fixed intensity so the AR3 placement/budget
// decisions are trivially spottable while walking the level, the same way cranking
// r_rtVolAnisotropy to 1 makes beams obvious. Applied only at the AR5 synthesis step
// (below), after AR4 dedupe has already used the real emissive color for its hue
// match, so this can't affect dedupe correctness — it only overrides what the
// resulting light actually looks like once created.
idCVar r_rtAutoRelightDebugColor("r_rtAutoRelightDebugColor", "0", CVAR_RENDERER | CVAR_BOOL,
                                 "auto_relight.md: DEBUG — force all synthesized lights to hot pink at high "
                                 "intensity, overriding r_rtAutoRelightIntensity and the fixture's real color, "
                                 "so placement is obvious while navigating. Not for normal play.");

idCVar r_rtAutoRelightReach("r_rtAutoRelightReach", "160", CVAR_RENDERER | CVAR_FLOAT,
                            "auto_relight.md AR5: synthesized light radius along the fixture's surface normal.");

idCVar r_rtAutoRelightOffset("r_rtAutoRelightOffset", "8", CVAR_RENDERER | CVAR_FLOAT,
                             "auto_relight.md AR5: synthesized light origin offset off the surface, along "
                             "the fixture's normal — keeps the light out of the wall.");

idCVar r_rtAutoRelightDedupeDist(
    "r_rtAutoRelightDedupeDist", "96", CVAR_RENDERER | CVAR_FLOAT,
    "auto_relight.md AR4: existing-map-light suppression radius around a proposed synthesized light origin.");

// ---------------------------------------------------------------------------
// AR1 — grid bucketing
//
// Cell size is a v1 constant, not a cvar (auto_relight.md AR2): 48 units is
// roughly one Doom 3 light-panel tile. Triangles are streamed directly into
// their cell's area-weighted accumulators rather than stored per-triangle —
// cheap and keeps memory use independent of world triangle count.
// ---------------------------------------------------------------------------

static const float AR_GRID_CELL_SIZE = 48.0f;

struct arCell_t
{
    int ix, iy, iz; // cell coordinates, exact match on hash collision

    idVec3 wPosSum;    // sum(triangle centroid * triangle area)
    idVec3 normalSum;  // sum(triangle face normal * triangle area)
    idVec3 mins, maxs; // world-space AABB of contributing triangle verts
    float area;        // sum(triangle area)
    int triCount;

    // Representative material/image for the cell, chosen by whichever
    // triangle contributed the most area — used for AR3 scoring (luminance
    // sample) and AR5 synthesis (color), and for console-log identification.
    const idMaterial *repMaterial;
    idImage *repEmissiveImage;
    bool repIsScreen; // GUI surface or cinematic/videomap stage (AR3 style weight)
    float repMaterialArea;
};

// Classic integer spatial hash (Teschner et al.), folded into idHashIndex's
// non-negative int key space.
static ID_INLINE int AR_CellHashKey(int ix, int iy, int iz)
{
    const unsigned int h = (unsigned int)ix * 73856093u ^ (unsigned int)iy * 19349663u ^ (unsigned int)iz * 83492791u;
    return (int)(h & 0x7fffffff);
}

static int AR_FindOrAddCell(idList<arCell_t> &cells, idHashIndex &cellHash, int ix, int iy, int iz)
{
    const int key = AR_CellHashKey(ix, iy, iz);
    for (int c = cellHash.First(key); c != -1; c = cellHash.Next(c))
    {
        if (cells[c].ix == ix && cells[c].iy == iy && cells[c].iz == iz)
            return c;
    }

    arCell_t cell;
    cell.ix = ix;
    cell.iy = iy;
    cell.iz = iz;
    cell.wPosSum.Zero();
    cell.normalSum.Zero();
    cell.mins.Zero();
    cell.maxs.Zero();
    cell.area = 0.0f;
    cell.triCount = 0;
    cell.repMaterial = NULL;
    cell.repEmissiveImage = NULL;
    cell.repIsScreen = false;
    cell.repMaterialArea = 0.0f;

    const int idx = cells.Append(cell);
    cellHash.Add(key, idx);
    return idx;
}

// ---------------------------------------------------------------------------
// AR1 — surface harvest
//
// Walks every static world-model surface (tr.primaryWorld's per-portal-area
// "_area%i" models — same geometry the static BLAS build walks in
// vk_accelstruct.cpp) and streams emissive triangles into the grid. Mirrors
// the BLAS walk's surface filters (MF_POLYGONOFFSET, particle/sprite/flare
// deforms) so a triangle that couldn't cast an RT shadow isn't harvested as
// a light source either.
// ---------------------------------------------------------------------------

static void AR_HarvestEmissiveSurfaces(idRenderWorldLocal *world, idList<arCell_t> &cells, idHashIndex &cellHash,
                                       int &outEmissiveSurfCount, int &outEmissiveTriCount,
                                       int &outSkippedGeomSurfCount)
{
    outEmissiveSurfCount = 0;
    outEmissiveTriCount = 0;
    outSkippedGeomSurfCount = 0;

    for (int m = 0; m < world->localModels.Num(); m++)
    {
        idRenderModel *model = world->localModels[m];
        if (!model)
            continue;

        const int numSurfaces = model->NumSurfaces();
        for (int s = 0; s < numSurfaces; s++)
        {
            const modelSurface_t *surf = model->Surface(s);
            if (!surf || !surf->geometry || !surf->shader)
                continue;

            if (surf->shader->TestMaterialFlag(MF_POLYGONOFFSET))
                continue; // decals coplanar with the surface beneath them

            const deform_t def = surf->shader->Deform();
            if (def == DFRM_PARTICLE || def == DFRM_PARTICLE2 || def == DFRM_SPRITE || def == DFRM_FLARE)
                continue; // view-facing billboards, not physical fixtures

            idImage *emissiveImg = NULL;
            bool guiEmissive = false;
            bool isCinematic = false;
            if (!VK_RT_MaterialIsEmissive(surf->shader, &emissiveImg, &guiEmissive, &isCinematic))
                continue;
            const bool isScreen = guiEmissive || isCinematic; // AR3: "screens are the money shot"

            const srfTriangles_t *geo = surf->geometry;
            if (!geo->verts || !geo->indexes || geo->numVerts == 0 || geo->numIndexes == 0)
            {
                outSkippedGeomSurfCount++;
                continue;
            }

            outEmissiveSurfCount++;

            for (int t = 0; t + 2 < geo->numIndexes; t += 3)
            {
                const idDrawVert &v0 = geo->verts[geo->indexes[t + 0]];
                const idDrawVert &v1 = geo->verts[geo->indexes[t + 1]];
                const idDrawVert &v2 = geo->verts[geo->indexes[t + 2]];

                const idVec3 edge1 = v1.xyz - v0.xyz;
                const idVec3 edge2 = v2.xyz - v0.xyz;
                const idVec3 cross = edge1.Cross(edge2);
                const float doubleArea = cross.Length();
                if (doubleArea < 1e-6f)
                    continue; // degenerate triangle

                const float triArea = doubleArea * 0.5f;
                const idVec3 faceNormal = cross / doubleArea;
                const idVec3 centroid = (v0.xyz + v1.xyz + v2.xyz) * (1.0f / 3.0f);

                const int ix = (int)idMath::Floor(centroid.x / AR_GRID_CELL_SIZE);
                const int iy = (int)idMath::Floor(centroid.y / AR_GRID_CELL_SIZE);
                const int iz = (int)idMath::Floor(centroid.z / AR_GRID_CELL_SIZE);

                const int cellIdx = AR_FindOrAddCell(cells, cellHash, ix, iy, iz);
                arCell_t &cell = cells[cellIdx];

                if (cell.triCount == 0)
                {
                    cell.mins = v0.xyz;
                    cell.maxs = v0.xyz;
                }

                cell.wPosSum += centroid * triArea;
                cell.normalSum += faceNormal * triArea;
                cell.area += triArea;
                cell.triCount++;

                for (int k = 0; k < 3; k++)
                {
                    const idVec3 &p = (k == 0) ? v0.xyz : (k == 1 ? v1.xyz : v2.xyz);
                    for (int a = 0; a < 3; a++)
                    {
                        if (p[a] < cell.mins[a])
                            cell.mins[a] = p[a];
                        if (p[a] > cell.maxs[a])
                            cell.maxs[a] = p[a];
                    }
                }

                if (triArea > cell.repMaterialArea)
                {
                    cell.repMaterial = surf->shader;
                    cell.repEmissiveImage = emissiveImg;
                    cell.repIsScreen = isScreen;
                    cell.repMaterialArea = triArea;
                }

                outEmissiveTriCount++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AR2 — clustering
//
// Flood-fills 6-connected occupied grid cells into physical-fixture clusters,
// area-weighting the centroid/normal and unioning the world-space AABB. A
// cluster is rejected (kept in the output list, flagged) when its triangle
// normals disagree too much — |sum of area-weighted unit normals| / totalArea
// < 0.7 means the "fixture" wraps around a corner or is otherwise not a single
// flat-ish emitter (a glow strip on a curved pipe, for example).
//
// Tangent-plane half-extents (feeds AR5's light shape) are approximated from
// the cluster's cell-level AABB corners rather than a second per-triangle
// pass — slightly generous, fine for v1.
// ---------------------------------------------------------------------------

static const float AR_NORMAL_AGREEMENT_MIN = 0.7f;

enum arVerdict_t
{
    AR_VERDICT_CANDIDATE = 0,   // not yet finalized by AR3/AR4/AR5
    AR_VERDICT_REJECTED_NORMAL, // AR2: normal spread too wide, never scored
    AR_VERDICT_AREA_CAPPED,     // AR3: below the top r_rtAutoRelightMaxPerArea within its own portal area
    AR_VERDICT_BUDGET_DROPPED,  // AR3: below the top r_rtAutoRelightMax map-wide (after the per-area cap)
    AR_VERDICT_DEDUPED,         // AR4: an existing map light already covers this fixture
    AR_VERDICT_LIT,             // AR5: synthesized as a real light
};

struct arCluster_t
{
    idVec3 centroid;
    idVec3 normal;         // unit average face normal; zero if rejected
    float normalAgreement; // |sum area-weighted unit normals| / totalArea, in [0,1]
    float totalArea;
    idVec3 mins, maxs;       // union of contributing cells' world AABB
    float tangentHalfWidth;  // half-extent along the tangent "left" axis
    float tangentHalfHeight; // half-extent along the tangent "up" axis
    int cellCount;
    int triCount;

    const idMaterial *repMaterial;
    idImage *repEmissiveImage;
    bool repIsScreen;

    // Filled in by AR3/AR4/AR5 — see arVerdict_t.
    float emissiveLuma;          // AR3: average luminance of repEmissiveImage, [0,1]
    idVec3 emissiveColor;        // AR3: luminance-weighted ("bright-texel") average color, [0,1] per channel
    float score;                 // AR3: totalArea * emissiveLuma * styleWeight
    int areaNum;                 // AR3: world->PointInArea(centroid); -1 if unresolved (own pseudo-bucket)
    idVec3 origin;               // AR4/AR5: centroid + normal * r_rtAutoRelightOffset
    int dedupeMatchLightIndex;   // AR4: index into world->lightDefs, or -1
    qhandle_t synthesizedHandle; // AR5: AddLightDef() handle, or -1
    arVerdict_t verdict;
};

static void AR_ClusterCells(idList<arCell_t> &cells, idHashIndex &cellHash, idList<arCluster_t> &outClusters,
                            int &outRejectedCount)
{
    outRejectedCount = 0;

    idList<bool> visited;
    visited.SetNum(cells.Num());
    for (int i = 0; i < cells.Num(); i++)
        visited[i] = false;

    static const int NEIGHBOR_OFFSETS[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

    idList<int> queue;

    for (int start = 0; start < cells.Num(); start++)
    {
        if (visited[start])
            continue;

        queue.SetNum(0);
        queue.Append(start);
        visited[start] = true;

        idVec3 wPosSum(0.0f, 0.0f, 0.0f);
        idVec3 normalSum(0.0f, 0.0f, 0.0f);
        idVec3 mins = cells[start].mins;
        idVec3 maxs = cells[start].maxs;
        float totalArea = 0.0f;
        int triCount = 0;
        int cellCount = 0;
        const idMaterial *repMaterial = NULL;
        idImage *repEmissiveImage = NULL;
        bool repIsScreen = false;
        float repMaterialArea = 0.0f;

        for (int qi = 0; qi < queue.Num(); qi++)
        {
            const int c = queue[qi];
            const arCell_t &cell = cells[c];

            wPosSum += cell.wPosSum;
            normalSum += cell.normalSum;
            totalArea += cell.area;
            triCount += cell.triCount;
            cellCount++;

            for (int a = 0; a < 3; a++)
            {
                if (cell.mins[a] < mins[a])
                    mins[a] = cell.mins[a];
                if (cell.maxs[a] > maxs[a])
                    maxs[a] = cell.maxs[a];
            }

            if (cell.repMaterialArea > repMaterialArea)
            {
                repMaterialArea = cell.repMaterialArea;
                repMaterial = cell.repMaterial;
                repEmissiveImage = cell.repEmissiveImage;
                repIsScreen = cell.repIsScreen;
            }

            for (int n = 0; n < 6; n++)
            {
                const int nx = cell.ix + NEIGHBOR_OFFSETS[n][0];
                const int ny = cell.iy + NEIGHBOR_OFFSETS[n][1];
                const int nz = cell.iz + NEIGHBOR_OFFSETS[n][2];
                const int nkey = AR_CellHashKey(nx, ny, nz);

                for (int ni = cellHash.First(nkey); ni != -1; ni = cellHash.Next(ni))
                {
                    if (visited[ni])
                        continue;
                    if (cells[ni].ix == nx && cells[ni].iy == ny && cells[ni].iz == nz)
                    {
                        visited[ni] = true;
                        queue.Append(ni);
                        break;
                    }
                }
            }
        }

        if (totalArea < 1e-4f)
            continue; // defensive; shouldn't happen (every cell has >= 1 triangle)

        arCluster_t cluster;
        cluster.totalArea = totalArea;
        cluster.centroid = wPosSum / totalArea;
        cluster.mins = mins;
        cluster.maxs = maxs;
        cluster.cellCount = cellCount;
        cluster.triCount = triCount;
        cluster.repMaterial = repMaterial;
        cluster.repEmissiveImage = repEmissiveImage;
        cluster.repIsScreen = repIsScreen;
        cluster.emissiveLuma = 1.0f;
        cluster.emissiveColor = idVec3(1.0f, 1.0f, 1.0f);
        cluster.score = 0.0f;
        cluster.areaNum = -1; // resolved in AR_ScoreAndBudget
        cluster.origin = cluster.centroid;
        cluster.dedupeMatchLightIndex = -1;
        cluster.synthesizedHandle = -1;
        cluster.verdict = AR_VERDICT_CANDIDATE;

        const float normalLen = normalSum.Length();
        cluster.normalAgreement = normalLen / totalArea;

        if (cluster.normalAgreement < AR_NORMAL_AGREEMENT_MIN)
        {
            cluster.verdict = AR_VERDICT_REJECTED_NORMAL;
            cluster.normal.Zero();
            cluster.tangentHalfWidth = 0.0f;
            cluster.tangentHalfHeight = 0.0f;
            outClusters.Append(cluster);
            outRejectedCount++;
            continue;
        }

        cluster.normal = normalSum / normalLen;

        idVec3 left, up;
        cluster.normal.OrthogonalBasis(left, up);

        float minL = 0.0f, maxL = 0.0f, minU = 0.0f, maxU = 0.0f;
        for (int corner = 0; corner < 8; corner++)
        {
            idVec3 p;
            p.x = (corner & 1) ? maxs.x : mins.x;
            p.y = (corner & 2) ? maxs.y : mins.y;
            p.z = (corner & 4) ? maxs.z : mins.z;
            const idVec3 rel = p - cluster.centroid;
            const float l = rel * left;
            const float u = rel * up;
            if (corner == 0)
            {
                minL = maxL = l;
                minU = maxU = u;
            }
            else
            {
                if (l < minL)
                    minL = l;
                if (l > maxL)
                    maxL = l;
                if (u < minU)
                    minU = u;
                if (u > maxU)
                    maxU = u;
            }
        }
        cluster.tangentHalfWidth = (maxL - minL) * 0.5f;
        cluster.tangentHalfHeight = (maxU - minU) * 0.5f;

        outClusters.Append(cluster);
    }
}

// ---------------------------------------------------------------------------
// AR3 helper — per-image average luminance + "bright-texel" average color
//
// idImage does not retain CPU-side texel data after GPU upload (Image.h), so
// this reloads the raw file via R_LoadImage (same "canonical 32-bit format"
// loader used by every other image path) and caches the result by idImage*
// for the lifetime of one VK_AutoRelight_Generate() call — a handful of
// distinct emissive images per map, not worth caching across map loads (and
// caching across loads would risk a stale idImage* after the level-load
// image purge, the same hazard VK_RT_BeginLevelLoad exists to avoid for
// BLASes).
//
// Large images are stride-sampled rather than walked texel-by-texel to keep
// this bounded — "small images, cheap" per auto_relight.md AR3.
// ---------------------------------------------------------------------------

struct arImageStat_t
{
    const idImage *image;
    float luma;   // plain average luminance across sampled texels, [0,1]
    idVec3 color; // luminance-weighted average color ("bright-texel" bias), [0,1] per channel
};

static void AR_GetImageLumaColor(idList<arImageStat_t> &cache, const idImage *img, float &outLuma, idVec3 &outColor)
{
    outLuma = 1.0f;
    outColor = idVec3(1.0f, 1.0f, 1.0f); // auto_relight.md AR3 fallback: white

    if (!img)
        return;

    for (int i = 0; i < cache.Num(); i++)
    {
        if (cache[i].image == img)
        {
            outLuma = cache[i].luma;
            outColor = cache[i].color;
            return;
        }
    }

    byte *pic = NULL;
    int width = 0, height = 0;
    R_LoadImage(img->imgName.c_str(), &pic, &width, &height, NULL, false);

    arImageStat_t stat;
    stat.image = img;
    stat.luma = 1.0f;
    stat.color = idVec3(1.0f, 1.0f, 1.0f);

    if (pic && width > 0 && height > 0)
    {
        const int numTexels = width * height;
        const int stride = (numTexels > 4096) ? (numTexels / 4096) : 1;

        double sumLuma = 0.0, sumWeightedR = 0.0, sumWeightedG = 0.0, sumWeightedB = 0.0;
        int sampledCount = 0;

        for (int t = 0; t < numTexels; t += stride)
        {
            const byte *texel = pic + (size_t)t * 4;
            const float r = texel[0] / 255.0f;
            const float g = texel[1] / 255.0f;
            const float b = texel[2] / 255.0f;
            const float luma = 0.299f * r + 0.587f * g + 0.114f * b;

            sumLuma += luma;
            sumWeightedR += r * luma;
            sumWeightedG += g * luma;
            sumWeightedB += b * luma;
            sampledCount++;
        }

        if (sampledCount > 0)
        {
            stat.luma = (float)(sumLuma / sampledCount);
            if (sumLuma > 1e-6)
            {
                stat.color.x = (float)(sumWeightedR / sumLuma);
                stat.color.y = (float)(sumWeightedG / sumLuma);
                stat.color.z = (float)(sumWeightedB / sumLuma);
            }
            else
            {
                stat.color = idVec3(0.0f, 0.0f, 0.0f); // fully black image
            }
        }

        Mem_Free(pic);
    }
    // else: file not found (imgName may be an image-program expression, not a
    // plain path) or empty — keep the white/1.0 fallback per AR3.

    cache.Append(stat);
    outLuma = stat.luma;
    outColor = stat.color;
}

// Sorts `idx` (indices into `clusters`) by descending score, in place.
// Cluster counts are small (tens, not thousands) — insertion sort is simple
// and plenty fast; shared by both the per-area and map-wide passes below.
static void AR_SortByScoreDescending(idList<arCluster_t> &clusters, idList<int> &idx)
{
    for (int i = 1; i < idx.Num(); i++)
    {
        const int key = idx[i];
        const float keyScore = clusters[key].score;
        int j = i - 1;
        while (j >= 0 && clusters[idx[j]].score < keyScore)
        {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
}

// ---------------------------------------------------------------------------
// AR3a — scoring (no budget yet)
//
// Split from budgeting (2026-08-23, code review finding): scoring/area-tagging
// has to happen before AR4 dedupe (dedupe needs c.emissiveColor), but the actual
// CAPPING must happen AFTER dedupe — see AR_ApplyBudget's header comment for why
// doing it in the other order silently starves rooms of lights they should get.
// ---------------------------------------------------------------------------

static void AR_Score(idRenderWorldLocal *world, idList<arCluster_t> &clusters)
{
    idList<arImageStat_t> imageStatCache;

    for (int i = 0; i < clusters.Num(); i++)
    {
        arCluster_t &c = clusters[i];
        if (c.verdict != AR_VERDICT_CANDIDATE)
            continue; // AR2-rejected — never scored

        AR_GetImageLumaColor(imageStatCache, c.repEmissiveImage, c.emissiveLuma, c.emissiveColor);

        const float styleWeight = c.repIsScreen ? 1.5f : 1.0f; // GUI/videomap: "screens are the money shot"
        c.score = c.totalArea * c.emissiveLuma * styleWeight;
        c.areaNum = world->PointInArea(c.centroid); // -1 (unresolved) gets its own bucket below
    }
}

// ---------------------------------------------------------------------------
// AR3b — budget (per-area cap, then map-wide cap)
//
// Must run AFTER AR4 dedupe, not before (2026-08-23, code review finding — was
// AR_ScoreAndBudget, ran before dedupe). Running the caps first let a room's
// highest-scoring fixtures consume its per-area quota even when those exact
// fixtures were about to be rejected by dedupe (already covered by a real
// mapper-placed light) — a room whose brightest fixtures all happen to already
// be hand-lit could end up with ZERO synthesized lights despite having other,
// genuinely-uncovered fixtures that never got a chance to compete for a slot.
// Building candidateIdx here (scanning for verdict == CANDIDATE) naturally
// excludes anything AR4 already marked DEDUPED, so only fixtures that will
// actually become real lights compete for the limited slots.
//
// Two-stage cap (2026-08-23, replacing a single map-wide top-N — see
// r_rtAutoRelightMaxPerArea's comment above for why): first keep the top
// r_rtAutoRelightMaxPerArea clusters WITHIN EACH PORTAL AREA (by score), then
// take everything that survived that and apply r_rtAutoRelightMax as an
// overall map-wide ceiling. This is what makes "a few per scene" hold as the
// player moves between rooms, while the map-wide cap still exists as a safety
// valve for maps with many areas.
// ---------------------------------------------------------------------------

static void AR_ApplyBudget(idList<arCluster_t> &clusters)
{
    idList<int> candidateIdx;
    for (int i = 0; i < clusters.Num(); i++)
    {
        if (clusters[i].verdict == AR_VERDICT_CANDIDATE)
            candidateIdx.Append(i);
    }

    // --- Stage 1: per-area cap ---
    // Distinct area numbers present among candidates (small set — number of
    // rooms touched by an emissive fixture, not number of clusters).
    idList<int> distinctAreas;
    for (int i = 0; i < candidateIdx.Num(); i++)
    {
        const int areaNum = clusters[candidateIdx[i]].areaNum;
        if (distinctAreas.FindIndex(areaNum) < 0)
            distinctAreas.Append(areaNum);
    }

    const int maxPerArea = Max(0, r_rtAutoRelightMaxPerArea.GetInteger());
    idList<int> survivedPerArea;
    idList<int> areaGroup;
    for (int a = 0; a < distinctAreas.Num(); a++)
    {
        areaGroup.SetNum(0);
        for (int i = 0; i < candidateIdx.Num(); i++)
        {
            if (clusters[candidateIdx[i]].areaNum == distinctAreas[a])
                areaGroup.Append(candidateIdx[i]);
        }

        AR_SortByScoreDescending(clusters, areaGroup);

        for (int rank = 0; rank < areaGroup.Num(); rank++)
        {
            if (rank < maxPerArea)
                survivedPerArea.Append(areaGroup[rank]);
            else
                clusters[areaGroup[rank]].verdict = AR_VERDICT_AREA_CAPPED;
        }
    }

    // --- Stage 2: map-wide ceiling over whatever survived stage 1 ---
    AR_SortByScoreDescending(clusters, survivedPerArea);

    const int maxLights = Max(0, r_rtAutoRelightMax.GetInteger());
    for (int rank = 0; rank < survivedPerArea.Num(); rank++)
    {
        arCluster_t &c = clusters[survivedPerArea[rank]];
        if (rank >= maxLights)
            c.verdict = AR_VERDICT_BUDGET_DROPPED;
        // else: stays AR_VERDICT_CANDIDATE, handed to AR5 next.
    }
}

// ---------------------------------------------------------------------------
// AR4 — dedupe against existing map lights
// ---------------------------------------------------------------------------

// "Broadly similar" color test: near-white light of either color counts as a
// wildcard match (a designer's white fill and a colored synthesized panel
// aren't a real double-light clash), otherwise compare hue via normalized-
// color dot product. Favors skipping synthesis on a borderline call — over-
// filtering only makes AR1-5 do less (GI/relight is additive-only, pillar 2),
// never wrong in the "double-bright" direction the doc warns about.
static bool AR_ColorsSimilar(const idVec3 &a, const idVec3 &b)
{
    const float maxA = Max(a.x, Max(a.y, a.z));
    const float minA = Min(a.x, Min(a.y, a.z));
    const float satA = (maxA > 1e-4f) ? (maxA - minA) / maxA : 0.0f;

    const float maxB = Max(b.x, Max(b.y, b.z));
    const float minB = Min(b.x, Min(b.y, b.z));
    const float satB = (maxB > 1e-4f) ? (maxB - minB) / maxB : 0.0f;

    if (satA < 0.15f || satB < 0.15f)
        return true;

    idVec3 na = a, nb = b;
    na.Normalize();
    nb.Normalize();
    return (na * nb) > 0.85f; // ~32 degrees of hue angle
}

static void AR_Dedupe(idRenderWorldLocal *world, idList<arCluster_t> &clusters)
{
    const float dedupeDist = Max(0.0f, r_rtAutoRelightDedupeDist.GetFloat());
    const float dedupeDistSq = dedupeDist * dedupeDist;
    const float offset = r_rtAutoRelightOffset.GetFloat();

    for (int i = 0; i < clusters.Num(); i++)
    {
        arCluster_t &c = clusters[i];
        if (c.verdict != AR_VERDICT_CANDIDATE)
            continue;

        c.origin = c.centroid + c.normal * offset;

        for (int li = 0; li < world->lightDefs.Num(); li++)
        {
            const idRenderLightLocal *ldef = world->lightDefs[li];
            if (!ldef)
                continue;

            const renderLight_t &lp = ldef->parms;
            // 2026-08-30: globalLightOrigin, not parms.origin. AR4 is asking "is this
            // fixture already lit by a hand-placed light" — that is a question about
            // where the light emits from, and a mapper can offset the emitter from the
            // entity by thousands of units via lightCenter (mars_city1 light_4842:
            // ~5800). Measuring to parms.origin misses exactly the lights that ARE on
            // the fixture but whose entity sits elsewhere, and would then synthesize a
            // duplicate on top of one.
            const float distSq = (ldef->globalLightOrigin - c.origin).LengthSqr();
            if (distSq > dedupeDistSq)
                continue;

            const idVec3 existingColor(lp.shaderParms[SHADERPARM_RED], lp.shaderParms[SHADERPARM_GREEN],
                                       lp.shaderParms[SHADERPARM_BLUE]);
            if (!AR_ColorsSimilar(c.emissiveColor, existingColor))
                continue;

            c.verdict = AR_VERDICT_DEDUPED;
            c.dedupeMatchLightIndex = li;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// AR5 — light synthesis
//
// RT shadow/GI/volumetric consumers (vk_shadows.cpp, vk_gi.cpp, vol_march.comp)
// deliberately ignore point-light axis rotation and read lightRadius as
// literal world X/Y/Z half-extents (see vk_shadows.cpp's anisotropic
// soft-shadow comment). Orienting the ellipsoid via renderLight.axis alone
// would therefore not actually shape the penumbra for an angled panel, so
// this bounds the true (left*halfWidth, up*halfHeight, normal*reach) oriented
// box with its world-axis-aligned AABB instead — exact when the panel is
// already axis-aligned (the common case in Doom 3's blocky architecture),
// slightly generous otherwise. renderLight.axis is left identity to match.
// ---------------------------------------------------------------------------

static void AR_Synthesize(idRenderWorldLocal *world, idList<arCluster_t> &clusters)
{
    const float reach = Max(1.0f, r_rtAutoRelightReach.GetFloat());
    const float intensity = r_rtAutoRelightIntensity.GetFloat();

    // base/materials/rt_auto_relight.mtr — byte-identical to lights/defaultPointLight,
    // exists purely so every debug tool that prints lightShader->GetName()
    // (r_rtGILightDump, r_showLights, ...) shows a distinct, greppable name for
    // synthesized lights instead of the generic default every other shader-less
    // point light also falls back to. Looked up once, not per-cluster.
    const idMaterial *arLightShader = declManager->FindMaterial("lights/rtAutoRelight");

    for (int i = 0; i < clusters.Num(); i++)
    {
        arCluster_t &c = clusters[i];
        if (c.verdict != AR_VERDICT_CANDIDATE)
            continue;

        idVec3 left, up;
        c.normal.OrthogonalBasis(left, up);

        const float hw = c.tangentHalfWidth * 2.5f;
        const float hh = c.tangentHalfHeight * 2.5f;

        renderLight_t rl;
        memset(&rl, 0, sizeof(rl));

        rl.axis[0][0] = 1.0f;
        rl.axis[1][1] = 1.0f;
        rl.axis[2][2] = 1.0f;
        rl.origin = c.origin;

        rl.pointLight = true;
        rl.noShadows = false; // AR5: synthesized lights are always real shadow casters.
        rl.noSpecular = false;
        rl.parallel = false;

        rl.lightRadius.x = idMath::Fabs(left.x) * hw + idMath::Fabs(up.x) * hh + idMath::Fabs(c.normal.x) * reach;
        rl.lightRadius.y = idMath::Fabs(left.y) * hw + idMath::Fabs(up.y) * hh + idMath::Fabs(c.normal.y) * reach;
        rl.lightRadius.z = idMath::Fabs(left.z) * hw + idMath::Fabs(up.z) * hh + idMath::Fabs(c.normal.z) * reach;

        // 2026-08-30: intensity is baked into RGB rather than parked in parm3.
        //
        // Slot 3 on a *light* is SHADERPARM_TIMESCALE, not an intensity — see the note
        // in vk_gi.cpp's considerLight for the full derivation. Writing intensity there
        // only ever worked because our own GI/vol light collector was reading it back
        // out with the same wrong assumption; the interaction path that actually draws
        // these lights takes brightness from RGB alone (tr_render.cpp:872-874,
        // draw_common.cpp:2079). So r_rtAutoRelightIntensity was silently a GI/vol-only
        // control, with no effect on the direct lighting from the very fixtures it
        // synthesizes. Baking it into RGB makes one number mean one thing everywhere.
        rl.shaderParms[SHADERPARM_RED] = c.emissiveColor.x * intensity;
        rl.shaderParms[SHADERPARM_GREEN] = c.emissiveColor.y * intensity;
        rl.shaderParms[SHADERPARM_BLUE] = c.emissiveColor.z * intensity;
        rl.shaderParms[SHADERPARM_TIMESCALE] = 1.0f; // Light.cpp:159's spawn default

        if (r_rtAutoRelightDebugColor.GetBool())
        {
            // DEBUG: see r_rtAutoRelightDebugColor's comment above. Overrides the
            // real fixture color/intensity computed above — AR4's dedupe already
            // ran against the real color, so this can't skew that decision.
            // 4.0 is the same "well above the normal 0.5 default — unmissable" level
            // this used to carry in parm3, now expressed in RGB like everything else.
            const float debugIntensity = 4.0f;
            rl.shaderParms[SHADERPARM_RED] = 1.0f * debugIntensity;
            rl.shaderParms[SHADERPARM_GREEN] = 0.0f;
            rl.shaderParms[SHADERPARM_BLUE] = 1.0f * debugIntensity;
        }

        rl.shader = arLightShader; // lights/rtAutoRelight — see comment above

        c.synthesizedHandle = world->AddLightDef(&rl);
        c.verdict = AR_VERDICT_LIT;
    }
}

// ---------------------------------------------------------------------------
// Debug table helpers
// ---------------------------------------------------------------------------

static const char *AR_VerdictName(arVerdict_t v)
{
    switch (v)
    {
    case AR_VERDICT_REJECTED_NORMAL:
        return "REJECTED";
    case AR_VERDICT_AREA_CAPPED:
        return "AREA-CAP";
    case AR_VERDICT_BUDGET_DROPPED:
        return "BUDGET-DROP";
    case AR_VERDICT_DEDUPED:
        return "DEDUPED";
    case AR_VERDICT_LIT:
        return "LIT";
    case AR_VERDICT_CANDIDATE:
    default:
        return "CANDIDATE";
    }
}

// ---------------------------------------------------------------------------
// VK_AutoRelight_Generate
// ---------------------------------------------------------------------------

void VK_AutoRelight_Generate(idRenderWorldLocal *world)
{
    if (!world)
        return;

    if (!r_rtAutoRelight.GetBool() && r_rtAutoRelightDebug.GetInteger() <= 0)
        return;

    const int startMsec = Sys_Milliseconds();

    idList<arCell_t> cells;
    idHashIndex cellHash;
    int emissiveSurfCount = 0, emissiveTriCount = 0, skippedGeomSurfCount = 0;
    AR_HarvestEmissiveSurfaces(world, cells, cellHash, emissiveSurfCount, emissiveTriCount, skippedGeomSurfCount);

    idList<arCluster_t> clusters;
    int rejectedCount = 0;
    AR_ClusterCells(cells, cellHash, clusters, rejectedCount);

    AR_Score(world, clusters);

    // Dedupe is read-only (no AddLightDef side effect) — always run it so the
    // debug table previews DEDUPED verdicts even with r_rtAutoRelight 0.
    // Synthesis mutates world state, so it stays gated on the master toggle.
    // Runs BEFORE the budget caps (2026-08-23, code review finding) — see
    // AR_ApplyBudget's header comment for why the other order silently starves
    // rooms whose highest-scoring fixtures already have a real map light.
    AR_Dedupe(world, clusters);

    AR_ApplyBudget(clusters);

    int litCount = 0, budgetDroppedCount = 0, areaCappedCount = 0, dedupedCount = 0, candidateCount = 0;
    if (r_rtAutoRelight.GetBool())
        AR_Synthesize(world, clusters);

    for (int i = 0; i < clusters.Num(); i++)
    {
        switch (clusters[i].verdict)
        {
        case AR_VERDICT_LIT:
            litCount++;
            break;
        case AR_VERDICT_AREA_CAPPED:
            areaCappedCount++;
            break;
        case AR_VERDICT_BUDGET_DROPPED:
            budgetDroppedCount++;
            break;
        case AR_VERDICT_DEDUPED:
            dedupedCount++;
            break;
        case AR_VERDICT_CANDIDATE: // only possible when r_rtAutoRelight 0 (AR5 didn't run)
            candidateCount++;
            break;
        default:
            break;
        }
    }

    const int elapsedMsec = Sys_Milliseconds() - startMsec;

    if (r_rtAutoRelight.GetBool())
    {
        common->Printf("VK AutoRelight: %d emissive surfaces (%d skipped, no CPU geometry), %d triangles -> %d "
                       "grid cells -> %d clusters (%d lit, %d deduped, %d area-capped, %d budget-dropped, %d "
                       "rejected: normal spread) in %d msec\n",
                       emissiveSurfCount, skippedGeomSurfCount, emissiveTriCount, cells.Num(), clusters.Num(), litCount,
                       dedupedCount, areaCappedCount, budgetDroppedCount, rejectedCount, elapsedMsec);
    }
    else
    {
        common->Printf("VK AutoRelight: %d emissive surfaces (%d skipped, no CPU geometry), %d triangles -> %d "
                       "grid cells -> %d clusters (%d would-be-lit, %d deduped, %d area-capped, %d budget-dropped, "
                       "%d rejected: normal spread) in %d msec [r_rtAutoRelight 0 — preview only, no synthesis]\n",
                       emissiveSurfCount, skippedGeomSurfCount, emissiveTriCount, cells.Num(), clusters.Num(),
                       candidateCount, dedupedCount, areaCappedCount, budgetDroppedCount, rejectedCount, elapsedMsec);
    }

    if (r_rtAutoRelightDebug.GetInteger() >= 1)
    {
        // "rm" is the portal area number (world->PointInArea on the centroid) that
        // the per-area cap (r_rtAutoRelightMaxPerArea) groups by — -1 means
        // unresolved (its own pseudo-bucket, see AR_ScoreAndBudget). Not to be
        // confused with "surfA", the fixture's physical emissive surface area.
        common->Printf("  #  verdict      score  surfA  tris cells  rm agree      centroid           color       "
                       "material\n");
        for (int i = 0; i < clusters.Num(); i++)
        {
            const arCluster_t &c = clusters[i];
            idStr extra;
            if (c.verdict == AR_VERDICT_DEDUPED)
                extra = va(" (vs light %d)", c.dedupeMatchLightIndex);
            else if (c.verdict == AR_VERDICT_LIT)
                extra = va(" (handle %d)", c.synthesizedHandle);

            common->Printf("%3d  %-11s %6.1f %7.1f %4d  %3d %3d %4.2f (%7.1f %7.1f %7.1f) (%3.2f %3.2f %3.2f)  %s%s\n",
                           i, AR_VerdictName(c.verdict), c.score, c.totalArea, c.triCount, c.cellCount, c.areaNum,
                           c.normalAgreement, c.centroid.x, c.centroid.y, c.centroid.z, c.emissiveColor.x,
                           c.emissiveColor.y, c.emissiveColor.z, c.repMaterial ? c.repMaterial->GetName() : "<none>",
                           extra.c_str());
        }
    }
}
