/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

// GL-specific initialization: extension function pointer definitions,
// extension capability checks, and GL error reporting.
// Extracted from RenderSystem_init.cpp as part of the GL/Vulkan backend split.

#include "sys/platform.h"
#include "idlib/LangDict.h"
#include "renderer/tr_local.h"

// ---------------------------------------------------------------------------
// qgl extension function pointer definitions
// (declarations live in qgl.h; extern refs are satisfied here)
// ---------------------------------------------------------------------------

// define qgl functions
#define QGLPROC(name, rettype, args) rettype(APIENTRYP q##name) args;
#include "renderer/qgl_proc.h"

void(APIENTRY *qglMultiTexCoord2fARB)(GLenum texture, GLfloat s, GLfloat t);
void(APIENTRY *qglMultiTexCoord2fvARB)(GLenum texture, GLfloat *st);
void(APIENTRY *qglActiveTextureARB)(GLenum texture);
void(APIENTRY *qglClientActiveTextureARB)(GLenum texture);

void(APIENTRY *qglTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);

void(APIENTRY *qglColorTableEXT)(int, int, int, int, int, const void *);

// EXT_stencil_two_side
PFNGLACTIVESTENCILFACEEXTPROC qglActiveStencilFaceEXT;

// ARB_texture_compression
PFNGLCOMPRESSEDTEXIMAGE2DARBPROC qglCompressedTexImage2DARB;
PFNGLGETCOMPRESSEDTEXIMAGEARBPROC qglGetCompressedTexImageARB;

// ARB_vertex_buffer_object
PFNGLBINDBUFFERARBPROC qglBindBufferARB;
PFNGLDELETEBUFFERSARBPROC qglDeleteBuffersARB;
PFNGLGENBUFFERSARBPROC qglGenBuffersARB;
PFNGLISBUFFERARBPROC qglIsBufferARB;
PFNGLBUFFERDATAARBPROC qglBufferDataARB;
PFNGLBUFFERSUBDATAARBPROC qglBufferSubDataARB;
PFNGLGETBUFFERSUBDATAARBPROC qglGetBufferSubDataARB;
PFNGLMAPBUFFERARBPROC qglMapBufferARB;
PFNGLUNMAPBUFFERARBPROC qglUnmapBufferARB;
PFNGLGETBUFFERPARAMETERIVARBPROC qglGetBufferParameterivARB;
PFNGLGETBUFFERPOINTERVARBPROC qglGetBufferPointervARB;

// ARB_vertex_program / ARB_fragment_program
PFNGLVERTEXATTRIBPOINTERARBPROC qglVertexAttribPointerARB;
PFNGLENABLEVERTEXATTRIBARRAYARBPROC qglEnableVertexAttribArrayARB;
PFNGLDISABLEVERTEXATTRIBARRAYARBPROC qglDisableVertexAttribArrayARB;
PFNGLPROGRAMSTRINGARBPROC qglProgramStringARB;
PFNGLBINDPROGRAMARBPROC qglBindProgramARB;
PFNGLGENPROGRAMSARBPROC qglGenProgramsARB;
PFNGLPROGRAMENVPARAMETER4FVARBPROC qglProgramEnvParameter4fvARB;
PFNGLPROGRAMLOCALPARAMETER4FVARBPROC qglProgramLocalParameter4fvARB;

// GL_EXT_depth_bounds_test
PFNGLDEPTHBOUNDSEXTPROC qglDepthBoundsEXT;

// DG: couldn't find any extension for this, it's supported in GL2.0 and newer, incl OpenGL ES2.0
PFNGLSTENCILOPSEPARATEPROC qglStencilOpSeparate;

// GL_ARB_debug_output
PFNGLDEBUGMESSAGECALLBACKARBPROC qglDebugMessageCallbackARB;

// ---------------------------------------------------------------------------
// GL debug output callback
// ---------------------------------------------------------------------------

enum
{
    // Not all GL.h headers know about GL_DEBUG_SEVERITY_NOTIFICATION.
    QGL_DEBUG_SEVERITY_NOTIFICATION = 0x826B
};

static void APIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                   const GLchar *message, const void *userParam)
{
    const char *sourceStr = "Source: Unknown";
    const char *typeStr = "Type: Unknown";
    const char *severityStr = "Severity: Unknown";

    switch (severity)
    {
#define SVRCASE(X, STR)                                                                                                \
    case GL_DEBUG_SEVERITY_##X##_ARB:                                                                                  \
        severityStr = STR;                                                                                             \
        break;
    case QGL_DEBUG_SEVERITY_NOTIFICATION:
        return;
        SVRCASE(HIGH, "Severity: High")
        SVRCASE(MEDIUM, "Severity: Medium")
        SVRCASE(LOW, "Severity: Low")
#undef SVRCASE
    }

    switch (source)
    {
#define SRCCASE(X)                                                                                                     \
    case GL_DEBUG_SOURCE_##X##_ARB:                                                                                    \
        sourceStr = "Source: " #X;                                                                                     \
        break;
        SRCCASE(API);
        SRCCASE(WINDOW_SYSTEM);
        SRCCASE(SHADER_COMPILER);
        SRCCASE(THIRD_PARTY);
        SRCCASE(APPLICATION);
        SRCCASE(OTHER);
#undef SRCCASE
    }

    switch (type)
    {
#define TYPECASE(X)                                                                                                     \
    case GL_DEBUG_TYPE_##X##_ARB:                                                                                      \
        typeStr = "Type: " #X;                                                                                         \
        break;
        TYPECASE(ERROR);
        TYPECASE(DEPRECATED_BEHAVIOR);
        TYPECASE(UNDEFINED_BEHAVIOR);
        TYPECASE(PORTABILITY);
        TYPECASE(PERFORMANCE);
        TYPECASE(OTHER);
#undef TYPECASE
    }

    common->Warning("GLDBG %s %s %s: %s\n", sourceStr, typeStr, severityStr, message);
}

// ---------------------------------------------------------------------------
// GL extension helpers
// ---------------------------------------------------------------------------

/*
=================
R_CheckExtension
=================
*/
bool R_CheckExtension(const char *name)
{
    if (!strstr(glConfig.extensions_string, name))
    {
        common->Printf("X..%s not found\n", name);
        return false;
    }

    common->Printf("...using %s\n", name);
    return true;
}

/*
==================
R_CheckPortableExtensions
==================
*/
void R_CheckPortableExtensions(void)
{
    glConfig.glVersion = atof(glConfig.version_string);

    // GL_ARB_multitexture
    glConfig.multitextureAvailable = R_CheckExtension("GL_ARB_multitexture");
    if (glConfig.multitextureAvailable)
    {
        qglMultiTexCoord2fARB =
            (void(APIENTRY *)(GLenum, GLfloat, GLfloat))GLimp_ExtensionPointer("glMultiTexCoord2fARB");
        qglMultiTexCoord2fvARB = (void(APIENTRY *)(GLenum, GLfloat *))GLimp_ExtensionPointer("glMultiTexCoord2fvARB");
        qglActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer("glActiveTextureARB");
        qglClientActiveTextureARB = (void(APIENTRY *)(GLenum))GLimp_ExtensionPointer("glClientActiveTextureARB");
        qglGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, (GLint *)&glConfig.maxTextureUnits);
        if (glConfig.maxTextureUnits > MAX_MULTITEXTURE_UNITS)
        {
            glConfig.maxTextureUnits = MAX_MULTITEXTURE_UNITS;
        }
        if (glConfig.maxTextureUnits < 2)
        {
            glConfig.multitextureAvailable = false; // shouldn't ever happen
        }
        qglGetIntegerv(GL_MAX_TEXTURE_COORDS_ARB, (GLint *)&glConfig.maxTextureCoords);
        qglGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS_ARB, (GLint *)&glConfig.maxTextureImageUnits);
    }

    // GL_ARB_texture_env_combine
    glConfig.textureEnvCombineAvailable = R_CheckExtension("GL_ARB_texture_env_combine");

    // GL_ARB_texture_cube_map
    glConfig.cubeMapAvailable = R_CheckExtension("GL_ARB_texture_cube_map");

    // GL_ARB_texture_env_dot3
    glConfig.envDot3Available = R_CheckExtension("GL_ARB_texture_env_dot3");

    // GL_ARB_texture_env_add
    glConfig.textureEnvAddAvailable = R_CheckExtension("GL_ARB_texture_env_add");

    // GL_ARB_texture_non_power_of_two
    glConfig.textureNonPowerOfTwoAvailable = R_CheckExtension("GL_ARB_texture_non_power_of_two");

    // GL_ARB_texture_compression + GL_S3_s3tc
    // DRI drivers may have GL_ARB_texture_compression but no GL_EXT_texture_compression_s3tc
    if (R_CheckExtension("GL_ARB_texture_compression") && R_CheckExtension("GL_EXT_texture_compression_s3tc"))
    {
        glConfig.textureCompressionAvailable = true;
        qglCompressedTexImage2DARB =
            (PFNGLCOMPRESSEDTEXIMAGE2DARBPROC)GLimp_ExtensionPointer("glCompressedTexImage2DARB");
        qglGetCompressedTexImageARB =
            (PFNGLGETCOMPRESSEDTEXIMAGEARBPROC)GLimp_ExtensionPointer("glGetCompressedTexImageARB");
        if (R_CheckExtension("GL_ARB_texture_compression_bptc"))
        {
            glConfig.bptcTextureCompressionAvailable = true;
        }
    }
    else
    {
        glConfig.textureCompressionAvailable = false;
        glConfig.bptcTextureCompressionAvailable = false;
    }

    // GL_EXT_texture_filter_anisotropic
    glConfig.anisotropicAvailable = R_CheckExtension("GL_EXT_texture_filter_anisotropic");
    if (glConfig.anisotropicAvailable)
    {
        qglGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glConfig.maxTextureAnisotropy);
        common->Printf("   maxTextureAnisotropy: %g\n", glConfig.maxTextureAnisotropy);
    }
    else
    {
        glConfig.maxTextureAnisotropy = 1;
    }

    // GL_EXT_texture_lod_bias
    // The actual extension is broken as specificed, storing the state in the texture unit instead
    // of the texture object.  The behavior in GL 1.4 is the behavior we use.
    if (glConfig.glVersion >= 1.4 || R_CheckExtension("GL_EXT_texture_lod"))
    {
        common->Printf("...using %s\n", "GL_1.4_texture_lod_bias");
        glConfig.textureLODBiasAvailable = true;
    }
    else
    {
        common->Printf("X..%s not found\n", "GL_1.4_texture_lod_bias");
        glConfig.textureLODBiasAvailable = false;
    }

    // GL_EXT_shared_texture_palette
    glConfig.sharedTexturePaletteAvailable = R_CheckExtension("GL_EXT_shared_texture_palette");
    if (glConfig.sharedTexturePaletteAvailable)
    {
        qglColorTableEXT =
            (void(APIENTRY *)(int, int, int, int, int, const void *))GLimp_ExtensionPointer("glColorTableEXT");
    }

    // GL_EXT_texture3D (not currently used for anything)
    glConfig.texture3DAvailable = R_CheckExtension("GL_EXT_texture3D");
    if (glConfig.texture3DAvailable)
    {
        qglTexImage3D = (void(APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                          const GLvoid *))GLimp_ExtensionPointer("glTexImage3D");
    }

    // EXT_stencil_wrap
    if (R_CheckExtension("GL_EXT_stencil_wrap"))
    {
        tr.stencilIncr = GL_INCR_WRAP_EXT;
        tr.stencilDecr = GL_DECR_WRAP_EXT;
    }
    else
    {
        tr.stencilIncr = GL_INCR;
        tr.stencilDecr = GL_DECR;
    }

    // GL_EXT_stencil_two_side
    glConfig.twoSidedStencilAvailable = R_CheckExtension("GL_EXT_stencil_two_side");
    if (glConfig.twoSidedStencilAvailable)
        qglActiveStencilFaceEXT = (PFNGLACTIVESTENCILFACEEXTPROC)GLimp_ExtensionPointer("glActiveStencilFaceEXT");

    if (glConfig.glVersion >= 2.0)
    {
        common->Printf("...got GL2.0+ glStencilOpSeparate()\n");
        qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)GLimp_ExtensionPointer("glStencilOpSeparate");
    }
    else if (R_CheckExtension("GL_ATI_separate_stencil"))
    {
        common->Printf("...got glStencilOpSeparateATI() (GL_ATI_separate_stencil)\n");
        qglStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)GLimp_ExtensionPointer("glStencilOpSeparateATI");
    }
    else
    {
        common->Printf("X..don't have glStencilOpSeparateATI() or (GL2.0+) glStencilOpSeparate()\n");
        qglStencilOpSeparate = NULL;
    }

    // ARB_vertex_buffer_object
    glConfig.ARBVertexBufferObjectAvailable = R_CheckExtension("GL_ARB_vertex_buffer_object");
    if (glConfig.ARBVertexBufferObjectAvailable)
    {
        qglBindBufferARB = (PFNGLBINDBUFFERARBPROC)GLimp_ExtensionPointer("glBindBufferARB");
        qglDeleteBuffersARB = (PFNGLDELETEBUFFERSARBPROC)GLimp_ExtensionPointer("glDeleteBuffersARB");
        qglGenBuffersARB = (PFNGLGENBUFFERSARBPROC)GLimp_ExtensionPointer("glGenBuffersARB");
        qglIsBufferARB = (PFNGLISBUFFERARBPROC)GLimp_ExtensionPointer("glIsBufferARB");
        qglBufferDataARB = (PFNGLBUFFERDATAARBPROC)GLimp_ExtensionPointer("glBufferDataARB");
        qglBufferSubDataARB = (PFNGLBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer("glBufferSubDataARB");
        qglGetBufferSubDataARB = (PFNGLGETBUFFERSUBDATAARBPROC)GLimp_ExtensionPointer("glGetBufferSubDataARB");
        qglMapBufferARB = (PFNGLMAPBUFFERARBPROC)GLimp_ExtensionPointer("glMapBufferARB");
        qglUnmapBufferARB = (PFNGLUNMAPBUFFERARBPROC)GLimp_ExtensionPointer("glUnmapBufferARB");
        qglGetBufferParameterivARB =
            (PFNGLGETBUFFERPARAMETERIVARBPROC)GLimp_ExtensionPointer("glGetBufferParameterivARB");
        qglGetBufferPointervARB = (PFNGLGETBUFFERPOINTERVARBPROC)GLimp_ExtensionPointer("glGetBufferPointervARB");
    }

    // ARB_vertex_program
    glConfig.ARBVertexProgramAvailable = R_CheckExtension("GL_ARB_vertex_program");
    if (glConfig.ARBVertexProgramAvailable)
    {
        qglVertexAttribPointerARB = (PFNGLVERTEXATTRIBPOINTERARBPROC)GLimp_ExtensionPointer("glVertexAttribPointerARB");
        qglEnableVertexAttribArrayARB =
            (PFNGLENABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer("glEnableVertexAttribArrayARB");
        qglDisableVertexAttribArrayARB =
            (PFNGLDISABLEVERTEXATTRIBARRAYARBPROC)GLimp_ExtensionPointer("glDisableVertexAttribArrayARB");
        qglProgramStringARB = (PFNGLPROGRAMSTRINGARBPROC)GLimp_ExtensionPointer("glProgramStringARB");
        qglBindProgramARB = (PFNGLBINDPROGRAMARBPROC)GLimp_ExtensionPointer("glBindProgramARB");
        qglGenProgramsARB = (PFNGLGENPROGRAMSARBPROC)GLimp_ExtensionPointer("glGenProgramsARB");
        qglProgramEnvParameter4fvARB =
            (PFNGLPROGRAMENVPARAMETER4FVARBPROC)GLimp_ExtensionPointer("glProgramEnvParameter4fvARB");
        qglProgramLocalParameter4fvARB =
            (PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)GLimp_ExtensionPointer("glProgramLocalParameter4fvARB");
    }

    // ARB_fragment_program
    if (r_inhibitFragmentProgram.GetBool())
    {
        glConfig.ARBFragmentProgramAvailable = false;
    }
    else
    {
        glConfig.ARBFragmentProgramAvailable = R_CheckExtension("GL_ARB_fragment_program");
        if (glConfig.ARBFragmentProgramAvailable)
        {
            // these are the same as ARB_vertex_program
            qglProgramStringARB = (PFNGLPROGRAMSTRINGARBPROC)GLimp_ExtensionPointer("glProgramStringARB");
            qglBindProgramARB = (PFNGLBINDPROGRAMARBPROC)GLimp_ExtensionPointer("glBindProgramARB");
            qglProgramEnvParameter4fvARB =
                (PFNGLPROGRAMENVPARAMETER4FVARBPROC)GLimp_ExtensionPointer("glProgramEnvParameter4fvARB");
            qglProgramLocalParameter4fvARB =
                (PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)GLimp_ExtensionPointer("glProgramLocalParameter4fvARB");
        }
    }

    // check for minimum set
    if (!glConfig.multitextureAvailable || !glConfig.textureEnvCombineAvailable || !glConfig.cubeMapAvailable ||
        !glConfig.envDot3Available)
    {
        common->Error("%s", common->GetLanguageDict()->GetString("#str_06780"));
    }

    // GL_EXT_depth_bounds_test
    glConfig.depthBoundsTestAvailable = R_CheckExtension("EXT_depth_bounds_test");
    if (glConfig.depthBoundsTestAvailable)
    {
        qglDepthBoundsEXT = (PFNGLDEPTHBOUNDSEXTPROC)GLimp_ExtensionPointer("glDepthBoundsEXT");
    }

    // GL_ARB_debug_output
    glConfig.glDebugOutputAvailable = false;
    if (glConfig.haveDebugContext)
    {
        if (strstr(glConfig.extensions_string, "GL_ARB_debug_output"))
        {
            glConfig.glDebugOutputAvailable = true;
            qglDebugMessageCallbackARB =
                (PFNGLDEBUGMESSAGECALLBACKARBPROC)GLimp_ExtensionPointer("glDebugMessageCallbackARB");
            if (r_glDebugContext.GetBool())
            {
                common->Printf("...using GL_ARB_debug_output (r_glDebugContext is set)\n");
                qglDebugMessageCallbackARB(DebugCallback, NULL);
                qglEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
            }
            else
            {
                common->Printf("...found GL_ARB_debug_output, but not using it (r_glDebugContext is not set)\n");
            }
        }
        else
        {
            common->Printf("X..GL_ARB_debug_output not found\n");
            qglDebugMessageCallbackARB = NULL;
            if (r_glDebugContext.GetBool())
            {
                common->Warning(
                    "r_glDebugContext is set, but can't be used because GL_ARB_debug_output is not supported");
            }
        }
    }
    else
    {
        if (strstr(glConfig.extensions_string, "GL_ARB_debug_output"))
        {
            if (r_glDebugContext.GetBool())
            {
                common->Printf("...found GL_ARB_debug_output, but not using it (no debug context)\n");
            }
            else
            {
                common->Printf("...found GL_ARB_debug_output, but not using it (r_glDebugContext is not set)\n");
            }
        }
        else
        {
            common->Printf("X..GL_ARB_debug_output not found\n");
        }
    }
}

/*
==================
GL_CheckErrors
==================
*/
void GL_CheckErrors(void)
{
    int err;
    char s[64];
    int i;

    // check for up to 10 errors pending
    for (i = 0; i < 10; i++)
    {
        err = qglGetError();
        if (err == GL_NO_ERROR)
        {
            return;
        }
        switch (err)
        {
        case GL_INVALID_ENUM:
            strcpy(s, "GL_INVALID_ENUM");
            break;
        case GL_INVALID_VALUE:
            strcpy(s, "GL_INVALID_VALUE");
            break;
        case GL_INVALID_OPERATION:
            strcpy(s, "GL_INVALID_OPERATION");
            break;
        case GL_STACK_OVERFLOW:
            strcpy(s, "GL_STACK_OVERFLOW");
            break;
        case GL_STACK_UNDERFLOW:
            strcpy(s, "GL_STACK_UNDERFLOW");
            break;
        case GL_OUT_OF_MEMORY:
            strcpy(s, "GL_OUT_OF_MEMORY");
            break;
        default:
            idStr::snPrintf(s, sizeof(s), "%i", err);
            break;
        }

        if (!r_ignoreGLErrors.GetBool())
        {
            common->Printf("GL_CheckErrors: %s\n", s);
        }
    }
}
