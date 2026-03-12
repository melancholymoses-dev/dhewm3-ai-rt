Here's my analysis and a phased plan based on the codebase structure.

---

## Current State Summary

The mixing is **concentrated in 5 files** - the `Vulkan/` subdirectory is already well-separated:

| File | Problem |
|---|---|
| [tr_backend.cpp](neo/renderer/tr_backend.cpp) | Dispatch hub mixes GL state machine with Vulkan routing |
| [VertexCache.cpp](neo/renderer/VertexCache.cpp) | 4 `#ifdef` blocks call `VK_VertexCache_Alloc/Free` from GL VBO logic |
| [Image_load.cpp](neo/renderer/Image_load.cpp) | GL texture upload has `VK_Image_Upload/Purge` injection points |
| [RenderSystem_init.cpp](neo/renderer/RenderSystem_init.cpp) | 7 `#ifdef` guards interleave Vulkan init into GL init sequence |
| [Image.h](neo/renderer/Image.h) / [VertexCache.h](neo/renderer/VertexCache.h) | Backend-specific fields (`vkData`, `vkBuffer`) inside shared structs |

The deeper architectural issue: there is **no `IBackend` interface** - backends are wired together via `#ifdef` injection and a runtime CVar check in `RB_ExecuteBackEndCommands`.

---

## Plan

### Phase 1 — Define a Backend Interface (the keystone)

Create [neo/renderer/RenderBackend.h](neo/renderer/RenderBackend.h) with a C++ abstract interface (or a plain function-pointer table if you want zero overhead):

```cpp
struct IBackend {
    // Lifecycle
    virtual void Init() = 0;
    virtual void Shutdown() = 0;
    virtual void PostSwapBuffers() = 0;

    // Resource management
    virtual void Image_Upload(idImage*, const byte*, int w, int h) = 0;
    virtual void Image_Purge(idImage*) = 0;
    virtual void VertexCache_Alloc(vertCache_t*, const void* data, int size, bool indexBuffer) = 0;
    virtual void VertexCache_Free(vertCache_t*) = 0;

    // Frame dispatch
    virtual void DrawView(const viewDef_t*) = 0;
    virtual void CopyRender(const copyRenderCommand_t&) = 0;
};

extern IBackend* activeBackend;  // set at init time
```

Replace the two `extern void VK_*` forward declarations in shared files with calls through `activeBackend->*`.

---

### Phase 2 — Create `GL/` to Mirror `Vulkan/`

Move GL-specific files into [neo/renderer/GL/](neo/renderer/GL/):

```
neo/renderer/GL/
  gl_backend.cpp      ← extract GL half of tr_backend.cpp
  gl_image.cpp        ← extract GL half of Image_load.cpp upload/purge
  gl_buffer.cpp       ← extract GL half of VertexCache.cpp alloc/free
  gl_instance.cpp     ← extract GL half of RenderSystem_init.cpp
  GLBackend.h/.cpp    ← implements IBackend
```

Keep [draw_arb2.cpp](neo/renderer/draw_arb2.cpp), [draw_glsl.cpp](neo/renderer/draw_glsl.cpp), [qgl.h](neo/renderer/qgl.h) in `GL/` as well.

Parallel to the existing `Vulkan/VKBackend.h/.cpp` implementing `IBackend`.

---

### Phase 3 — Remove Backend Fields from Shared Structs

**`idImage`** ([Image.h](neo/renderer/Image.h)):
- Remove `#ifdef DHEWM3_VULKAN vkImageData_t *vkData`
- Add `void* backendData = nullptr` (or a `uint64_t backendHandle`)
- `GL/gl_image.cpp` casts it to its own struct; `Vulkan/vk_image.cpp` does the same

**`vertCache_t`** ([VertexCache.h](neo/renderer/VertexCache.h)):
- Remove `#ifdef DHEWM3_VULKAN uint64_t vkBuffer; uint64_t vkMemory`
- The GL impl stores the VBO handle, the Vulkan impl stores the device buffer handle — both via `void* backendHandle` or a union

This eliminates the `#ifdef` contamination in the data layer entirely.

---

### Phase 4 — Clean Up `tr_backend.cpp`

[tr_backend.cpp](neo/renderer/tr_backend.cpp) becomes a thin dispatcher:

```cpp
void RB_ExecuteBackEndCommands(const emptyCommand_t* cmds) {
    for (const emptyCommand_t* cmd = cmds; cmd; cmd = cmd->next) {
        switch (cmd->commandId) {
        case RC_DRAW_VIEW:
            activeBackend->DrawView(...);  // no more usingVulkan check
            break;
        case RC_SWAP_BUFFERS:
            activeBackend->PostSwapBuffers();
            break;
        // ...
        }
    }
}
```

All the `GL_State`, `GL_Cull`, `RB_SetDefaultGLState` functions move to `GL/gl_backend.cpp`.

---

### Phase 5 — Clean Up `RenderSystem_init.cpp`

[RenderSystem_init.cpp](neo/renderer/RenderSystem_init.cpp) currently has 7 `#ifdef DHEWM3_VULKAN` guards. Replace with:

```cpp
// At init time, select and instantiate the backend
if (r_backend.GetString() == "vulkan") {
    activeBackend = new VKBackend();
} else {
    activeBackend = new GLBackend();
}
activeBackend->Init();
```

No more `#ifdef` needed in this file.

---

### Phase 6 — Audit and Remove Remaining `#ifdef`s

After Phases 1-5, the only remaining `#ifdef DHEWM3_VULKAN` blocks should be:
- Inside `Vulkan/*.cpp` files themselves (for the `DHEWM3_RAYTRACING` nesting — keep those)
- The `BE_VULKAN` enum value in `tr_local.h` (which can be removed if the enum is replaced by the interface)
- Build system files

Run `grep -rn "DHEWM3_VULKAN" neo/renderer/ --include="*.cpp" --include="*.h"` to confirm all are gone from shared files.

---

## Recommended Order

1. **Phase 1 first** — the interface definition unblocks everything else and is low-risk (no behavior change)
2. **Phase 3** — decouples the structs before anything calls them differently  
3. **Phase 2 + 4** — move GL code, update dispatch (can be done file by file)
4. **Phase 5** — clean init last, after all paths are wired through the interface
5. **Phase 6** — grep audit to catch stragglers

The `DHEWM3_RAYTRACING` guards inside `Vulkan/` are already clean and don't need to change — they're a second-level concern within the Vulkan backend.