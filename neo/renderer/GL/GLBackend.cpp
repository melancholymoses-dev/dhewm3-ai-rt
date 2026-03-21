#include "renderer/RendererBackend.h"
#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/Image.h"
#include "renderer/GL/GLBackend.h"
#include "renderer/GL/gl_image.h"
#include "renderer/GL/gl_buffer.h"

// Forward declarations of the existing GL free functions
extern const void RB_CopyRender(const void *);
extern void R_CheckPortableExtensions(void);

void GLBackend::Init()
{
    // Ensure isVulkan is cleared in case of a backend switch (e.g. vid_restart).
    glConfig.isVulkan = false;

    // Load qgl function pointers now that the GL context exists.
#define QGLPROC(name, rettype, args)                                                                                   \
    q##name = (rettype(APIENTRYP) args)GLimp_ExtensionPointer(#name);                                                  \
    if (!q##name)                                                                                                      \
        common->FatalError("Unable to initialize OpenGL (%s)", #name);
#include "renderer/qgl_proc.h"
#undef QGLPROC

    // get our config strings
    glConfig.vendor_string = (const char *)qglGetString(GL_VENDOR);
    glConfig.renderer_string = (const char *)qglGetString(GL_RENDERER);
    glConfig.version_string = (const char *)qglGetString(GL_VERSION);
    glConfig.extensions_string = (const char *)qglGetString(GL_EXTENSIONS);

    // OpenGL driver constants
    GLint temp;
    qglGetIntegerv(GL_MAX_TEXTURE_SIZE, &temp);
    glConfig.maxTextureSize = temp;

    // stubbed or broken drivers may have reported 0...
    if (glConfig.maxTextureSize <= 0)
    {
        glConfig.maxTextureSize = 256;
    }

    glConfig.isInitialized = true;

    common->Printf("OpenGL vendor: %s\n", glConfig.vendor_string);
    common->Printf("OpenGL renderer: %s\n", glConfig.renderer_string);
    common->Printf("OpenGL version: %s\n", glConfig.version_string);

    // recheck all the extensions
    R_CheckPortableExtensions();

    // parse our vertex and fragment programs, possibly disabling support for
    // one of the paths if there was an error
    R_ARB2_Init();

    cmdSystem->AddCommand("reloadARBprograms", R_ReloadARBPrograms_f, CMD_FL_RENDERER, "reloads ARB programs");
    R_ReloadARBPrograms_f(idCmdArgs());

    // Initialize the GLSL backend (always try, activated via r_useGLSL)
    R_GLSL_Init();
}

void GLBackend::Shutdown()
{
    // GLimp_Shutdown() is called by the outer shutdown path (ShutdownOpenGL / R_VidRestart_f).
}
void GLBackend::PostSwapBuffers()
{
    GLimp_SwapBuffers();
}

void GLBackend::Image_Upload(idImage *img, const byte *data, int w, int h, textureFilter_t filterParm,
                             bool allowDownSizeParm, textureRepeat_t repeatParm, textureDepth_t depthParm)
{
    GL_GenerateTexture(img, data, w, h, filterParm, allowDownSizeParm, repeatParm, depthParm);
}

void GLBackend::Image_Purge(idImage *img)
{
    GL_PurgeTexture(img);
}
void GLBackend::VertexCache_Alloc(vertCache_t **vc, void *data, int size, bool indexBuffer)
{
    vertexCache.Alloc(data, size, vc, indexBuffer);
}
void GLBackend::VertexCache_Free(vertCache_t *vc)
{
    GL_VertexCache_Free(vc);
}

// Command batch lifecycle

void GLBackend::BeginCommandBatch()
{
    RB_SetDefaultGLState();
}

void GLBackend::EndCommandBatch()
{
    qglBindTexture(GL_TEXTURE_2D, 0);
    backEnd.glState.tmu[0].current2DMap = -1;
}

void GLBackend::SetBuffer(const void *data)
{
    RB_SetBuffer(data);
}

void GLBackend::SwapBuffers(const void *data)
{
    RB_SwapBuffers(data);
}

// Frame dispatch

void GLBackend::DrawView(const drawSurfsCommand_t *cmd)
{
    RB_DrawView(cmd);
}

void GLBackend::CopyRender(const copyRenderCommand_t &cmd)
{
    RB_CopyRender((const void *)&cmd);
}