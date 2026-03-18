# Ray Tracing Lighting Engine — dhewm3 (Doom 3)

## Context

dhewm3 currently uses a classic stencil shadow volume lighting pipeline (Carmack's Reverse) with OpenGL and ARB assembly shaders — technology from 2004. The goal is to replace the lighting engine with hardware ray tracing to achieve accurate soft shadows, ray traced ambient occlusion (RTAO), and RT reflections. The rasterization path (diffuse, surface rendering, depth pre-pass) will remain as a hybrid approach.

**Target API:** Vulkan RT (`VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure`)
**Scope:** RT shadows + RTAO + RT reflections (hybrid, not full path tracing)
**Strategy:** Incremental phases — each phase leaves the game fully playable

---

## Current Architecture Summary

- **Graphics API:** OpenGL via SDL2, with `qgl.h` function pointer wrapper
- **Shaders:** ARB assembly vertex/fragment programs (`draw_arb2.cpp`)
- **Shadows:** Stencil shadow volumes (`tr_stencilshadow.cpp`, `draw_common.cpp`)
- **Lighting:** Per-light interaction passes (diffuse + specular + bump per light)
- **Render pipeline:** Front-end (visibility, PVS culling) → command queue → back-end (GL calls)
- **No Vulkan, no compute shaders, no GLSL, no hardware RT**

### Critical Files

| File | Role |
|------|------|
| `neo/renderer/RenderSystem.h` | Master render system interface |
| `neo/renderer/tr_local.h` | Internal state: `viewDef_t`, `backEndState_t`, `viewLight_t` |
| `neo/renderer/tr_backend.cpp` | Back-end command executor — main integration point |
| `neo/renderer/tr_render.cpp` | Front-end draw surface generation |
| `neo/renderer/tr_light.cpp` | Light processing, interaction culling (1400+ lines) |
| `neo/renderer/Interaction.h/.cpp` | Light-surface interaction data structures |
| `neo/renderer/draw_arb2.cpp` | ARB2 shader binding — will be replaced by GLSL/Vulkan |
| `neo/renderer/draw_common.cpp` | `RB_StencilShadowPass()` — shadow volume rendering |
| `neo/renderer/tr_stencilshadow.cpp` | Shadow volume geometry generation |
| `neo/renderer/VertexCache.h/.cpp` | GPU buffer management (must be ported to Vulkan) |
| `neo/sys/glimp.cpp` | GL/SDL context initialization — add Vulkan init here |
| `neo/CMakeLists.txt` | Build system — add Vulkan + SPIRV-Cross options |

---

## Phase 1: Modernize Shaders (ARB → GLSL)

**Goal:** Replace ARB assembly programs with modern GLSL so shaders can be cross-compiled to SPIR-V for Vulkan. Game must remain fully playable with OpenGL.

### Steps

1. **Add GLSL shader files** in a new `neo/renderer/glsl/` directory:
   - `interaction.vert` / `interaction.frag` — replaces ARB interaction programs in `draw_arb2.cpp`
   - `shadow.vert` / `shadow.frag` — replaces stencil shadow pass programs
   - `depth.vert` / `depth.frag` — depth pre-pass

2. **Add shader loading in `tr_local.h`** — `glslProgram_t` struct to hold compiled GLSL program handles

3. **Add a `draw_glsl.cpp`** backend that mirrors `draw_arb2.cpp` but uses `glCreateShader` / `glUseProgram`

4. **CMake option** `USE_GLSL_BACKEND` (default OFF initially, then ON once stable)

5. **Verify:** Run the game with `r_useGLSL 1` and compare output visually to ARB path

### Key constraints
- Keep ARB path compiling until Phase 2 is complete
- GLSL uniforms must mirror the `drawInteraction_t` struct exactly
- Use UBOs (Uniform Buffer Objects) for per-light and per-entity data

---

## Phase 2: Vulkan Rasterization Backend

**Goal:** Add a complete Vulkan rendering backend that reproduces the existing rasterized lighting (no RT yet). OpenGL remains as the fallback backend.

### New Files
```
neo/renderer/Vulkan/
  vk_common.h          — Vulkan types, helper macros
  vk_instance.cpp      — Instance, device, queue selection
  vk_swapchain.cpp     — Swapchain, framebuffers, render passes
  vk_pipeline.cpp      — Graphics pipeline, render passes, PSOs
  vk_memory.cpp        — VkDeviceMemory allocator (or integrate VMA)
  vk_buffer.cpp        — Vertex/index/uniform buffer management
  vk_image.cpp         — Texture loading, image transitions
  vk_shader.cpp        — SPIR-V shader loading (compiled from GLSL)
  vk_backend.cpp       — Main backend: mirrors tr_backend.cpp for Vulkan
```

### Modifications

- **`neo/sys/glimp.cpp`**: Add `VKimp_Init()` / `VKimp_Shutdown()` paths alongside existing `GLimp_Init()`
- **`neo/renderer/RenderSystem.h`**: Add `GetBackendType()` returning `BACKEND_OPENGL` or `BACKEND_VULKAN`
- **`neo/renderer/tr_backend.cpp`**: Dispatch `RB_ExecuteBackEndCommands()` to GL or VK backend based on CVar `r_backend`
- **`neo/renderer/VertexCache.cpp`**: Abstract buffer uploads behind `IVertexCache` interface — VK implementation uses `vkCmdCopyBuffer`
- **`neo/CMakeLists.txt`**: Add `find_package(Vulkan REQUIRED)`, link `Vulkan::Vulkan`, add `glslc` shader compilation step for SPIR-V

### Shader compilation
- Use `glslc` (from Vulkan SDK) at build time to compile `.vert`/`.frag` → `.spv`
- Embed SPIR-V as `uint32_t[]` arrays via `xxd` or a CMake custom command

### Verify
- Add CVar `r_backend "vulkan"` and confirm identical visual output to OpenGL path
- Run all game levels, check for visual regressions

---

## Phase 3: Acceleration Structure (BVH/TLAS)

**Goal:** Build and maintain Vulkan acceleration structures from the existing scene geometry so RT shaders can query them.

### New Files
```
neo/renderer/Vulkan/
  vk_raytracing.h      — RT extension function pointers, AS types
  vk_accelstruct.cpp   — BLAS/TLAS build and update logic
```

### Implementation

1. **Load RT extensions** at device init: `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`

2. **BLAS (Bottom-Level AS) per mesh:**
   - Hook into `idRenderEntityLocal` — build a BLAS when a new model is registered or geometry changes
   - Opaque geometry only initially (skip transparent surfaces for shadow BLAS)
   - Store `VkAccelerationStructureKHR blas` in entity local data

3. **TLAS (Top-Level AS) per frame:**
   - Rebuilt each frame from `viewDef_t->viewEntitys` list
   - One `VkAccelerationStructureInstanceKHR` per visible entity (using entity model matrix)
   - Store TLAS handle in `backEndState_t`

4. **Interaction with existing front-end:**
   - Front-end already computes entity visibility — reuse this to determine which BLASes go into TLAS
   - After `RB_DrawView()` begins, rebuild TLAS before shadow/lighting passes

### Key concerns
- Skinned/animated models: rebuild BLAS per frame for animated meshes
- Memory management: pool BLAS memory using the Vulkan memory allocator from Phase 2
- Synchronization: use `VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR` barriers

---

## Phase 4: Ray Traced Shadows (Replace Stencil Volumes)

**Goal:** Replace `RB_StencilShadowPass()` with a ray tracing pass that shoots shadow rays from each pixel to each light source. Output a shadow mask texture used in the lighting pass.

### New Files
```
neo/renderer/Vulkan/
  vk_shadows.cpp          — Shadow ray pipeline dispatch
neo/renderer/glsl/
  shadow_ray.rgen         — Ray generation shader
  shadow_ray.rmiss        — Miss shader (pixel is lit)
  shadow_ray.rahit        — Any-hit shader (for alpha-tested surfaces)
```

### Implementation

1. **Shadow mask render target:** Per-frame `VkImage` (R8 or R16F) at render resolution — one channel per active light (or an array for multiple lights)

2. **Ray generation shader (`shadow_ray.rgen`):**
   - Reconstruct world-space position from depth buffer (sample the depth attachment)
   - For each light affecting this pixel: shoot a ray from surface point toward light
   - If miss shader fires → write 1.0 (lit); any-hit → write 0.0 (shadowed) or partial (soft shadow)

3. **Soft shadows:** Jitter shadow rays within the light's angular extent using `lightRadius` from `renderLight_t`. Use temporal accumulation for noise reduction.

4. **Integration in `vk_backend.cpp`:**
   - After depth pre-pass, dispatch `vkCmdTraceRaysKHR()` for shadow mask
   - In the lighting pass, sample shadow mask texture instead of stencil test

5. **Remove stencil shadow path** for Vulkan backend (keep in OpenGL path):
   - Skip `tr_stencilshadow.cpp` volume generation for entities when `r_backend == vulkan`
   - Front-end still computes light interactions (needed for lighting pass), just not shadow volumes

6. **Temporal denoising:** Simple exponential moving average (EMA) on shadow mask between frames. Add later: NVIDIA NRD or AMD Denoiser library.

### Verify
- Compare shadow quality vs stencil volumes on key Doom 3 scenes (e.g., Mars City)
- Check performance with `r_showRenderTime`

---

## Phase 5: Ray Traced Ambient Occlusion (RTAO) + Reflections

**Goal:** Add RTAO to replace flat ambient lighting and RT reflections for shiny surfaces.

### RTAO

New shader: `neo/renderer/glsl/ao_ray.rgen`
- Shoot N hemispherical rays from each surface point in world space
- Count occlusion hits → write AO factor to `VkImage` AO buffer
- Apply AO factor when rendering the ambient/unlit surfaces (`SL_AMBIENT` stage)
- Use interleaved sampling + temporal accumulation for performance

### RT Reflections

New shader: `neo/renderer/glsl/reflect_ray.rgen`
- For surfaces with specular material stage: shoot reflection ray from camera hit point
- Use `TG_REFLECT_CUBE` texture as fallback miss shader environment
- Write reflected color to reflection buffer, blend with specular in lighting pass
- Initially only for highly-specular `idMaterial` surfaces (metal, glass)

### Denoising
- Integrate a spatial-temporal denoiser (Atrous filter or NRD SDK) for both AO and reflections
- NRD by NVIDIA: https://github.com/NVIDIAGameWorks/RayTracingDenoiser (MIT license)

---

## CMake Changes Summary

```cmake
# neo/CMakeLists.txt additions
option(DHEWM3_VULKAN "Enable Vulkan rendering backend" OFF)
option(DHEWM3_RAYTRACING "Enable Vulkan ray tracing (requires DHEWM3_VULKAN)" OFF)

if(DHEWM3_VULKAN)
  find_package(Vulkan REQUIRED)
  find_program(GLSLC_EXECUTABLE glslc REQUIRED)
  # Compile shaders to SPIR-V at build time
  # Add vk_*.cpp sources
endif()
```

---

## New CVar Interface

| CVar | Default | Description |
|------|---------|-------------|
| `r_backend` | `"opengl"` | `"opengl"` or `"vulkan"` |
| `r_useRayTracing` | `0` | Enable RT pipeline (requires Vulkan backend) |
| `r_rtShadows` | `1` | Ray traced shadows (replaces stencil volumes) |
| `r_rtAO` | `1` | Ray traced ambient occlusion |
| `r_rtReflections` | `0` | Ray traced reflections (expensive) |
| `r_rtShadowSamples` | `1` | Shadow rays per pixel (1=hard, 4+=soft) |
| `r_rtAOSamples` | `4` | AO rays per pixel |
| `r_rtDenoise` | `1` | Enable temporal denoising |

---

## Verification Plan

1. **Phase 1:** `r_useGLSL 1` — game renders identically to ARB path on OpenGL
2. **Phase 2:** `r_backend vulkan` — game renders identically via Vulkan rasterization
3. **Phase 3:** `vk_showTLAS 1` — debug overlay shows TLAS instance count matching visible entities
4. **Phase 4:** `r_rtShadows 1` — shadows are correct and soft; compare side-by-side with stencil path; run `timedemo` to measure frame time impact
5. **Phase 5:** `r_rtAO 1` — ambient surfaces show contact darkening; `r_rtReflections 1` — shiny metal surfaces show scene reflections

**Hardware requirement:** NVIDIA RTX (Turing+), AMD RDNA2+, or Intel Arc for hardware RT. OpenGL/rasterization fallback always available.

---

## Risk Areas

- **Animated models:** BLAS rebuild per frame is expensive — consider `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` for incremental updates
- **Alpha-tested surfaces:** Need any-hit shaders for correct shadow transparency
- **Multi-area portals:** Doom 3 portal system limits visibility — ensure TLAS only includes relevant geometry
- **Temporal artifacts:** Camera-cut denoising must reset accumulation history
- **ARB shader removal:** Keep ARB path until Vulkan is fully stable to avoid regressions
