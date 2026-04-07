# Multi-Surface UV Fix — Plan of Action
## Problem
The VtxAddrTable / IdxAddrTable SSBOs are currently sized and indexed per TLAS instance (one entry per instance). Each entry holds only the geometry-0 address of that instance's BLAS. When a ray hits geometry index 1+ in a multi-geometry BLAS (e.g. func_static_52999 has surf=0 and surf=1), gl_GeometryIndexEXT > 0 but there is no address stored for it. A temporary guard in rt_material.glsl:100 returns vec2(0.0) to avoid a GPU crash, but this means all non-first surfaces have broken UV lookups.

## Goal
Replace the per-instance address tables with per-geometry address tables. Each geometry in every BLAS gets its own slot. A new baseGeomIdx field in VkMaterialEntry stores the starting offset for that instance's geometries. In the shader: vtxAddrs[mat.baseGeomIdx + gl_GeometryIndexEXT].

## Files to Change
1. neo/renderer/Vulkan/vk_raytracing.h — VkMaterialEntry struct
Replace the two redundant fields (vtxBufInstance, idxBufInstance) with a single baseGeomIdx. Keep struct size at 32 bytes.

```C
// OLD:
struct VkMaterialEntry
{
    uint32_t diffuseTexIndex;
    uint32_t normalTexIndex;
    float    roughness;
    uint32_t flags;
    uint32_t vtxBufInstance;   // = instanceCustomIndex → VtxAddrTable[vtxBufInstance]
    uint32_t idxBufInstance;   // = instanceCustomIndex → IdxAddrTable[idxBufInstance]
    float    alphaThreshold;
    uint32_t pad;
};

// NEW:
struct VkMaterialEntry
{
    uint32_t diffuseTexIndex;
    uint32_t normalTexIndex;
    float    roughness;
    uint32_t flags;
    uint32_t baseGeomIdx;      // offset into per-geometry VtxAddrTable/IdxAddrTable
    float    alphaThreshold;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(VkMaterialEntry) == 32, "VkMaterialEntry size mismatch");
Also add a new constant for max total geometries:


static const uint32_t VK_MAT_MAX_GEOMS = 16384; // max total geometry slots across all instances
```
2. neo/renderer/Vulkan/vk_material_table.cpp
A. Resize addr SSBOs from MAT_MAX_INSTANCES to VK_MAT_MAX_GEOMS entries:

In VK_RT_InitMaterialTable:
```C

// OLD:
const VkDeviceSize addrSSBOSize = (VkDeviceSize)MAT_MAX_INSTANCES * sizeof(uint64_t);
// NEW:
const VkDeviceSize addrSSBOSize = (VkDeviceSize)VK_MAT_MAX_GEOMS * sizeof(uint64_t);
B. Update VK_RT_MakeMaterialEntry signature — takes baseGeomIdx instead of instanceIndex, writes all geometry addresses (not just [0]):


// OLD signature:
VkMaterialEntry VK_RT_MakeMaterialEntry(const idMaterial *shader, const vkBLAS_t *blas,
                                         uint32_t instanceIndex,
                                         uint64_t *outVtxAddr, uint64_t *outIdxAddr);

// NEW signature:
VkMaterialEntry VK_RT_MakeMaterialEntry(const idMaterial *shader, const vkBLAS_t *blas,
                                         uint32_t baseGeomIdx,
                                         uint64_t *outVtxAddrs, uint64_t *outIdxAddrs);
Inside, set entry.baseGeomIdx = baseGeomIdx and write all geometries:


// OLD (geometry addresses section):
if (blas && blas->geomCount > 0 && blas->geomVertAddrs && blas->geomIdxAddrs)
{
    *outVtxAddr = (uint64_t)blas->geomVertAddrs[0];
    *outIdxAddr = (uint64_t)blas->geomIdxAddrs[0];
}

// NEW:
entry.baseGeomIdx = baseGeomIdx;
if (blas && blas->geomCount > 0 && blas->geomVertAddrs && blas->geomIdxAddrs)
{
    for (uint32_t g = 0; g < blas->geomCount; g++)
    {
        outVtxAddrs[baseGeomIdx + g] = (uint64_t)blas->geomVertAddrs[g];
        outIdxAddrs[baseGeomIdx + g] = (uint64_t)blas->geomIdxAddrs[g];
    }
}
```
C. Update VK_RT_UploadMatTableFrame signature — add geometry counts for static and dynamic blocks:

```C
// OLD:
void VK_RT_UploadMatTableFrame(
    const VkMaterialEntry *staticEntries,  uint32_t staticCount,  bool rewriteStatic,
    const VkMaterialEntry *dynamicEntries, uint32_t dynamicCount,
    const uint64_t *staticVtx,  const uint64_t *staticIdx,
    const uint64_t *dynamicVtx, const uint64_t *dynamicIdx);

// NEW:
void VK_RT_UploadMatTableFrame(
    const VkMaterialEntry *staticEntries,  uint32_t staticCount,  bool rewriteStatic,
    const VkMaterialEntry *dynamicEntries, uint32_t dynamicCount,
    const uint64_t *geomVtxAddrs, const uint64_t *geomIdxAddrs,
    uint32_t staticGeomCount, uint32_t dynamicGeomCount, bool rewriteStaticGeoms);
```
The geometry addr arrays are flat: [0 .. staticGeomCount-1] are static, [staticGeomCount .. staticGeomCount+dynamicGeomCount-1] are dynamic. Upload:

```C
// Material entries — unchanged logic (split static/dynamic as before)

// Geometry addr tables — upload static block when rewriteStaticGeoms, always upload dynamic
if (rewriteStaticGeoms && staticGeomCount > 0)
{
    memcpy(vtxDst,                                     geomVtxAddrs,                staticGeomCount * addrSize);
    memcpy(idxDst,                                     geomIdxAddrs,                staticGeomCount * addrSize);
}
if (dynamicGeomCount > 0)
{
    memcpy(vtxDst + staticGeomCount * addrSize,        geomVtxAddrs + staticGeomCount, dynamicGeomCount * addrSize);
    memcpy(idxDst + staticGeomCount * addrSize,        geomIdxAddrs + staticGeomCount, dynamicGeomCount * addrSize);
}
```
Also update the forward declaration of VK_RT_MakeMaterialEntry wherever it appears.

3. neo/renderer/Vulkan/vk_accelstruct.cpp — TLAS rebuild
A. Add geometry address arrays and geometry counters alongside the existing instance arrays (near top of VK_RT_RebuildTLAS, around where staticVtxAddrs etc. are declared):

```C
// Add after existing per-instance addr arrays:
static uint64_t s_staticGeomVtxAddrs[VK_MAT_MAX_GEOMS];
static uint64_t s_staticGeomIdxAddrs[VK_MAT_MAX_GEOMS];
static uint64_t s_dynamicGeomVtxAddrs[VK_MAT_MAX_GEOMS];  // generous; dynamic count is small
static uint64_t s_dynamicGeomIdxAddrs[VK_MAT_MAX_GEOMS];

uint32_t staticGeomCount  = 0;
uint32_t dynamicGeomCount = 0;
```
B. In the instance loop, replace the MakeMaterialEntry call:

```C
// OLD:
uint64_t vtxA = 0, idxA = 0;
VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(shader, ent->blas, provIndex, &vtxA, &idxA);
if (isDynamicInstance)
{
    dynamicMatEntries[provIndex] = matEntry;
    dynamicVtxAddrs[provIndex]   = vtxA;
    dynamicIdxAddrs[provIndex]   = idxA;
}
else
{
    staticMatEntries[provIndex]  = matEntry;
    staticVtxAddrs[provIndex]    = vtxA;
    staticIdxAddrs[provIndex]    = idxA;
}

// NEW:
if (isDynamicInstance)
{
    uint32_t base = staticGeomCount + dynamicGeomCount;  // dynamic geoms follow static in flat array
    VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(shader, ent->blas, base,
                                                        s_dynamicGeomVtxAddrs, s_dynamicGeomIdxAddrs);
    // Adjust: MakeMaterialEntry writes at base, but dynamic array is separate — use local offset
    // Simpler: use a single flat array for both static and dynamic geometries:
    VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(shader, ent->blas, base,
                                                        s_geomVtxAddrs, s_geomIdxAddrs);
    dynamicMatEntries[provIndex] = matEntry;
    dynamicGeomCount += ent->blas ? ent->blas->geomCount : 0;
}
else
{
    uint32_t base = staticGeomCount;
    VkMaterialEntry matEntry = VK_RT_MakeMaterialEntry(shader, ent->blas, base,
                                                        s_geomVtxAddrs, s_geomIdxAddrs);
    staticMatEntries[provIndex] = matEntry;
    staticGeomCount += ent->blas ? ent->blas->geomCount : 0;
}
```
Note: Use a single flat geometry array s_geomVtxAddrs[VK_MAT_MAX_GEOMS] where static geoms occupy [0..staticGeomCount-1] and dynamic geoms follow at [staticGeomCount..staticGeomCount+dynamicGeomCount-1]. The baseGeomIdx stored in each VkMaterialEntry is the absolute index into this flat array.

C. Remove the patching of vtxBufInstance/idxBufInstance — those fields no longer exist. The baseGeomIdx is assigned once during the loop and doesn't need patching (it isn't derived from instanceCustomIndex). Remove these blocks:

```C
// DELETE these:
staticMatEntries[i].vtxBufInstance = i;
staticMatEntries[i].idxBufInstance = i;
// and:
dynamicMatEntries[i].vtxBufInstance = staticCount + i;
dynamicMatEntries[i].idxBufInstance = staticCount + i;
```
D. Update the UploadMatTableFrame call:

```C
// OLD:
VK_RT_UploadMatTableFrame(staticMatEntries, staticCount, rewriteStatic,
                           dynamicMatEntries, dynamicCount,
                           staticVtxAddrs, staticIdxAddrs,
                           dynamicVtxAddrs, dynamicIdxAddrs);

// NEW:
VK_RT_UploadMatTableFrame(staticMatEntries, staticCount, rewriteStatic,
                           dynamicMatEntries, dynamicCount,
                           s_geomVtxAddrs, s_geomIdxAddrs,
                           staticGeomCount, dynamicGeomCount, rewriteStatic);
```
Also remove the now-unused staticVtxAddrs, staticIdxAddrs, dynamicVtxAddrs, dynamicIdxAddrs arrays.

4. neo/renderer/glsl/rt_material.glsl — shader
A. Update the MaterialEntry struct to match the C++ side:

```C
// OLD:
struct MaterialEntry {
    uint  diffuseTexIndex;
    uint  normalTexIndex;
    float roughness;
    uint  flags;
    uint  vtxBufInstance;
    uint  idxBufInstance;
    float alphaThreshold;
    uint  pad;
};

// NEW:
struct MaterialEntry {
    uint  diffuseTexIndex;
    uint  normalTexIndex;
    float roughness;
    uint  flags;
    uint  baseGeomIdx;
    float alphaThreshold;
    uint  pad0;
    uint  pad1;
};
```
B. Update rt_InterpolateUV — remove the gl_GeometryIndexEXT != 0 guard, use baseGeomIdx + gl_GeometryIndexEXT:

```C
// OLD:
vec2 rt_InterpolateUV(uint matIdx, int primId, vec2 bary)
{
    MaterialEntry mat = materials[matIdx];

    // Guard: the address table stores only surface 0's geometry per TLAS instance.
    // TODO: refactor vtxAddrs/idxAddrs to be indexed by baseGeomIdx + gl_GeometryIndexEXT
    if (gl_GeometryIndexEXT != 0)
        return vec2(0.0);

    uint64_t idxAddr = idxAddrs[mat.idxBufInstance];
    uint64_t vtxAddr = vtxAddrs[mat.vtxBufInstance];
    if (idxAddr == 0ul || vtxAddr == 0ul)
        return vec2(0.0);
    // ... rest of function

// NEW:
vec2 rt_InterpolateUV(uint matIdx, int primId, vec2 bary)
{
    MaterialEntry mat = materials[matIdx];

    uint geomSlot = mat.baseGeomIdx + uint(gl_GeometryIndexEXT);
    uint64_t idxAddr = idxAddrs[geomSlot];
    uint64_t vtxAddr = vtxAddrs[geomSlot];
    if (idxAddr == 0ul || vtxAddr == 0ul)
        return vec2(0.0);
    // ... rest of function unchanged
```
Summary of Array Layout

Flat geometry address arrays (s_geomVtxAddrs, s_geomIdxAddrs):
  [0]                          ← static instance 0, geom 0
  [1]                          ← static instance 0, geom 1  (if multi-surface)
  [2]                          ← static instance 1, geom 0
  ...
  [staticGeomCount]            ← dynamic instance 0, geom 0
  [staticGeomCount + 1]        ← dynamic instance 0, geom 1
  ...

VkMaterialEntry.baseGeomIdx = absolute index of geom 0 for that instance.
Shader lookup: vtxAddrs[mat.baseGeomIdx + gl_GeometryIndexEXT]
What Does NOT Change
VK_MAT_MAX_TEXTURES, bindless texture handling — untouched
vkBLAS_t struct — untouched (geomVertAddrs[]/geomIdxAddrs[] already exist per-geometry)
VK_RT_BuildBLASForModel / VK_RT_BuildBLASSingle — untouched
shadow_ray.rahit, reflect_ray.rahit, ao_ray.rgen — no changes needed (they call rt_InterpolateUV which gets fixed)


## Chat History and Background
Agent:
Glass IS in the BLAS on frame 128 (the initial full rebuild):


VK RT GLASS: model='func_static_52999' surf=0 shader='textures/glass/glass1' gpu=no verts=4 idx=6
VK RT GLASS: model='func_static_52999' surf=1 shader='textures/glass/glass1' gpu=no verts=4 idx=6
VK RT GLASS: model='func_static_53001' surf=0 shader='textures/glass/glass1' gpu=no verts=4 idx=6
...
VK RT GLASS: model='func_static_53008' surf=0 shader='textures/glass/glass2' gpu=no verts=45 idx=192
The geometry pipeline is working. After frame 128, the static BLAS is cached and not rebuilt — that's why you don't see the GLASS log in later frames (staticRewritten=no).

So the geometry problem is solved. The reason reflections look unchanged is that the rchit shader has no glass awareness — reflection rays that hit textures/glass/glass1 treat it as an opaque surface, blocking reflections behind it rather than passing through.

The two remaining issues are:

Multi-surface OOB: each func_static has surf=0 and surf=1. The gl_GeometryIndexEXT != 0 guard returns vec2(0.0) for surf=1 (broken UV). This is the deferred proper fix.
Glass shader: the rchit shader needs to let rays pass through glass geometry (treat it as transparent/transmissive).
Do you want to tackle the glass shader handling next — adding a transparency pass-through in reflect_ray.rchit for glass surfaces? Or address the multi-surface UV issue first?


User: What is the UV Issue?

Agent: Each BLAS can contain multiple geometries (one per surface). When a ray hits a BLAS, the shader gets gl_GeometryIndexEXT — the index of which geometry within that BLAS was hit (0, 1, 2...).

The material table (VkMaterialEntry) currently stores only geomVertAddrs[0] and geomIdxAddrs[0] — addresses for the first geometry only. When a ray hits geometry index 1+ (e.g. func_static_52999 surf=1), the shader uses the wrong address.

The quick fix from the previous session was this guard in the UV interpolation shader:


// rt_InterpolateUV
if (gl_GeometryIndexEXT != 0) return vec2(0.0);  // avoid OOB crash
So for glass, func_static_52999 surf=0 gets correct UVs, surf=1 gets vec2(0.0) (black/wrong). It prevents a crash but breaks all non-first surfaces.

The proper fix would store per-geometry addresses as arrays indexed by gl_GeometryIndexEXT, something like:


VkMaterialEntry {
    baseGeomIdx       // where this model's geometries start in a global array
}
// separate arrays:
geomVertAddrs[baseGeomIdx + gl_GeometryIndexEXT]
geomIdxAddrs[baseGeomIdx + gl_GeometryIndexEXT]
That refactor touches the material table, TLAS rebuild, and the UV interpolation shader. It's the "proper fix" that was deferred.

For glass specifically, since all surfs use the same material (glass1), the UV issue matters if we want correct UV lookups per-triangle on non-first surfaces. For a pure pass-through transparency approach (just letting rays continue through glass without UV lookup), it's less critical — we'd just need to know the material is glass, not sample its texture.

User: is the glass shader rchit shader fix necessary?  if we only need the nearest surface to allow reflections then this doesn't matter?  or would this cause glass to appear opaque since none of the light from behind the glass gets through

Agent:
Good question. There are two separate issues:

Reflections OFF glass (rchit) — this is where you'd implement Fresnel. Not strictly necessary for a first pass.

Glass blocking reflections (rahit) — this IS a problem. Glass is now in the BLAS as opaque geometry. Any reflection ray (from a shiny floor or wall) that passes through the area where glass sits will hit the glass and terminate there, returning the glass material color (black/fallback). So a shiny surface seen through glass would have its reflections incorrectly blocked by the glass geometry.

The same applies to AO and shadow rays — glass will cast fake shadows and create false occlusion.

The minimum fix is in rahit — call ignoreIntersectionEXT() for MAT_FLAG_GLASS surfaces so all RT rays pass through glass, treating it as invisible to rays. That's a one-liner in each rahit shader.

reflect_ray.rahit already handles alpha-tested surfaces. Add:


if ((mat.flags & MAT_FLAG_GLASS) != 0u)
{
    ignoreIntersectionEXT();
    return;
}
Same addition needed in shadow_ray.rahit and wherever AO has a rahit.

That gives you correct behavior (rays pass through glass, nothing blocked). You lose reflections off the glass surface, but you gain that nothing is incorrectly occluded. The full rchit Fresnel work can come after.

Want to proceed with the rahit pass-through fix, or do you want the full Fresnel rchit at the same time?