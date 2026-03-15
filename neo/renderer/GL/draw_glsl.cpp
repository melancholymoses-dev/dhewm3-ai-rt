/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
dhewm3 GLSL backend - replaces the ARB assembly program backend.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

/*
=========================================================================================

GLSL INTERACTION RENDERING

Drop-in replacement for draw_arb2.cpp, using modern GLSL (OpenGL 2.0+) instead of
ARB assembly vertex/fragment programs. Activated by r_useGLSL 1.

All uniform names and the rendering logic match the ARB backend for visual parity.

=========================================================================================
*/

#include "sys/platform.h"
#include "renderer/VertexCache.h"
#include "renderer/tr_local.h"

// ---------------------------------------------------------------------------
// GL 2.0+ function pointer declarations
// These are loaded at init time via GLimp_ExtensionPointer().
// ---------------------------------------------------------------------------

typedef GLuint(APIENTRYP PFNGLCREATESHADERPROC_t)(GLenum type);
typedef void(APIENTRYP PFNGLSHADERSOURCEPROC_t)(GLuint shader, GLsizei count, const GLchar *const *string,
                                                const GLint *length);
typedef void(APIENTRYP PFNGLCOMPILESHADERPROC_t)(GLuint shader);
typedef void(APIENTRYP PFNGLGETSHADERIVPROC_t)(GLuint shader, GLenum pname, GLint *params);
typedef void(APIENTRYP PFNGLGETSHADERINFOLOGPROC_t)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLuint(APIENTRYP PFNGLCREATEPROGRAMPROC_t)(void);
typedef void(APIENTRYP PFNGLATTACHSHADERPROC_t)(GLuint program, GLuint shader);
typedef void(APIENTRYP PFNGLBINDATTRIBLOCATIONPROC_t)(GLuint program, GLuint index, const GLchar *name);
typedef void(APIENTRYP PFNGLLINKPROGRAMPROC_t)(GLuint program);
typedef void(APIENTRYP PFNGLGETPROGRAMIVPROC_t)(GLuint program, GLenum pname, GLint *params);
typedef void(APIENTRYP PFNGLGETPROGRAMINFOLOGPROC_t)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void(APIENTRYP PFNGLUSEPROGRAMPROC_t)(GLuint program);
typedef GLint(APIENTRYP PFNGLGETUNIFORMLOCATIONPROC_t)(GLuint program, const GLchar *name);
typedef void(APIENTRYP PFNGLUNIFORM1IPROC_t)(GLint location, GLint v0);
typedef void(APIENTRYP PFNGLUNIFORM1FPROC_t)(GLint location, GLfloat v0);
typedef void(APIENTRYP PFNGLUNIFORM4FVPROC_t)(GLint location, GLsizei count, const GLfloat *value);
typedef void(APIENTRYP PFNGLUNIFORMMATRIX4FVPROC_t)(GLint location, GLsizei count, GLboolean transpose,
                                                    const GLfloat *value);
typedef void(APIENTRYP PFNGLDELETESHADERPROC_t)(GLuint shader);
typedef void(APIENTRYP PFNGLDELETEPROGRAMPROC_t)(GLuint program);
typedef void(APIENTRYP PFNGLVERTEXATTRIBPOINTERPROC_t)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                                       GLsizei stride, const void *pointer);
typedef void(APIENTRYP PFNGLENABLEVERTEXATTRIBARRAYPROC_t)(GLuint index);
typedef void(APIENTRYP PFNGLDISABLEVERTEXATTRIBARRAYPROC_t)(GLuint index);

static PFNGLCREATESHADERPROC_t qglCreateShader = NULL;
static PFNGLSHADERSOURCEPROC_t qglShaderSource = NULL;
static PFNGLCOMPILESHADERPROC_t qglCompileShader = NULL;
static PFNGLGETSHADERIVPROC_t qglGetShaderiv = NULL;
static PFNGLGETSHADERINFOLOGPROC_t qglGetShaderInfoLog = NULL;
static PFNGLCREATEPROGRAMPROC_t qglCreateProgram = NULL;
static PFNGLATTACHSHADERPROC_t qglAttachShader = NULL;
static PFNGLBINDATTRIBLOCATIONPROC_t qglBindAttribLocation = NULL;
static PFNGLLINKPROGRAMPROC_t qglLinkProgram = NULL;
static PFNGLGETPROGRAMIVPROC_t qglGetProgramiv = NULL;
static PFNGLGETPROGRAMINFOLOGPROC_t qglGetProgramInfoLog = NULL;
static PFNGLUSEPROGRAMPROC_t qglUseProgram = NULL;
static PFNGLGETUNIFORMLOCATIONPROC_t qglGetUniformLocation = NULL;
static PFNGLUNIFORM1IPROC_t qglUniform1i = NULL;
static PFNGLUNIFORM1FPROC_t qglUniform1f = NULL;
static PFNGLUNIFORM4FVPROC_t qglUniform4fv = NULL;
static PFNGLUNIFORMMATRIX4FVPROC_t qglUniformMatrix4fv = NULL;
static PFNGLDELETESHADERPROC_t qglDeleteShader = NULL;
static PFNGLDELETEPROGRAMPROC_t qglDeleteProgram = NULL;
static PFNGLVERTEXATTRIBPOINTERPROC_t qglVertexAttribPointer = NULL;
static PFNGLENABLEVERTEXATTRIBARRAYPROC_t qglEnableVertexAttribArray = NULL;
static PFNGLDISABLEVERTEXATTRIBARRAYPROC_t qglDisableVertexAttribArray = NULL;

// ---------------------------------------------------------------------------
// Global GLSL program objects (declared extern in tr_local.h)
// ---------------------------------------------------------------------------

glslPrograms_t glslProgs;
static bool glslInitialized = false;

// ---------------------------------------------------------------------------
// Embedded GLSL shader source strings
// ---------------------------------------------------------------------------

// --- Interaction vertex shader ---
static const char *interactionVertSrc = "#version 330 core\n"
                                        "layout(location = 0) in vec3 in_Position;\n"
                                        "layout(location = 3) in vec4 in_Color;\n"
                                        "layout(location = 8) in vec2 in_TexCoord;\n"
                                        "layout(location = 9) in vec3 in_Tangent;\n"
                                        "layout(location = 10) in vec3 in_BiTangent;\n"
                                        "layout(location = 11) in vec3 in_Normal;\n"
                                        "uniform vec4 u_LightOrigin;\n"
                                        "uniform vec4 u_ViewOrigin;\n"
                                        "uniform vec4 u_LightProjectionS;\n"
                                        "uniform vec4 u_LightProjectionT;\n"
                                        "uniform vec4 u_LightProjectionQ;\n"
                                        "uniform vec4 u_LightFalloffS;\n"
                                        "uniform vec4 u_BumpMatrixS;\n"
                                        "uniform vec4 u_BumpMatrixT;\n"
                                        "uniform vec4 u_DiffuseMatrixS;\n"
                                        "uniform vec4 u_DiffuseMatrixT;\n"
                                        "uniform vec4 u_SpecularMatrixS;\n"
                                        "uniform vec4 u_SpecularMatrixT;\n"
                                        "uniform vec4 u_ColorModulate;\n"
                                        "uniform vec4 u_ColorAdd;\n"
                                        "uniform mat4 u_ModelViewProjection;\n"
                                        "out vec4 vary_TexCoord_Bump;\n"
                                        "out vec4 vary_TexCoord_Diffuse;\n"
                                        "out vec4 vary_TexCoord_Specular;\n"
                                        "out vec4 vary_LightProjection;\n"
                                        "out vec2 vary_LightFalloff;\n"
                                        "out vec3 vary_LightDir;\n"
                                        "out vec3 vary_ViewDir;\n"
                                        "out vec4 vary_Color;\n"
                                        "void main() {\n"
                                        "    vec4 pos = vec4(in_Position, 1.0);\n"
                                        "    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);\n"
                                        "    vary_TexCoord_Bump.x     = dot(tc, u_BumpMatrixS);\n"
                                        "    vary_TexCoord_Bump.y     = dot(tc, u_BumpMatrixT);\n"
                                        "    vary_TexCoord_Diffuse.x  = dot(tc, u_DiffuseMatrixS);\n"
                                        "    vary_TexCoord_Diffuse.y  = dot(tc, u_DiffuseMatrixT);\n"
                                        "    vary_TexCoord_Specular.x = dot(tc, u_SpecularMatrixS);\n"
                                        "    vary_TexCoord_Specular.y = dot(tc, u_SpecularMatrixT);\n"
                                        "    vary_LightProjection.x = dot(pos, u_LightProjectionS);\n"
                                        "    vary_LightProjection.y = dot(pos, u_LightProjectionT);\n"
                                        "    vary_LightProjection.z = 0.0;\n"
                                        "    vary_LightProjection.w = dot(pos, u_LightProjectionQ);\n"
                                        "    vary_LightFalloff.x = dot(pos, u_LightFalloffS);\n"
                                        "    vary_LightFalloff.y = 0.5;\n"
                                        "    vec3 lightVec = u_LightOrigin.xyz - in_Position;\n"
                                        "    vary_LightDir.x = dot(lightVec, in_Tangent);\n"
                                        "    vary_LightDir.y = dot(lightVec, in_BiTangent);\n"
                                        "    vary_LightDir.z = dot(lightVec, in_Normal);\n"
                                        "    vec3 viewVec = u_ViewOrigin.xyz - in_Position;\n"
                                        "    vary_ViewDir.x = dot(viewVec, in_Tangent);\n"
                                        "    vary_ViewDir.y = dot(viewVec, in_BiTangent);\n"
                                        "    vary_ViewDir.z = dot(viewVec, in_Normal);\n"
                                        "    vary_Color = in_Color * u_ColorModulate + u_ColorAdd;\n"
                                        "    gl_Position = u_ModelViewProjection * pos;\n"
                                        "}\n";

// --- Interaction fragment shader ---
static const char *interactionFragSrc =
    "#version 330 core\n"
    "in vec4 vary_TexCoord_Bump;\n"
    "in vec4 vary_TexCoord_Diffuse;\n"
    "in vec4 vary_TexCoord_Specular;\n"
    "in vec4 vary_LightProjection;\n"
    "in vec2 vary_LightFalloff;\n"
    "in vec3 vary_LightDir;\n"
    "in vec3 vary_ViewDir;\n"
    "in vec4 vary_Color;\n"
    "uniform sampler2D u_BumpMap;\n"
    "uniform sampler2D u_LightFalloff;\n"
    "uniform sampler2D u_LightProjection;\n"
    "uniform sampler2D u_DiffuseMap;\n"
    "uniform sampler2D u_SpecularMap;\n"
    "uniform sampler2D u_SpecularTable;\n"
    "uniform vec4 u_DiffuseColor;\n"
    "uniform vec4 u_SpecularColor;\n"
    "uniform vec4 u_GammaBrightness;\n"
    "uniform bool u_ApplyGamma;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 N = texture(u_BumpMap, vary_TexCoord_Bump.xy).rgb * 2.0 - 1.0;\n"
    "    N = normalize(N);\n"
    "    vec3 L = normalize(vary_LightDir);\n"
    "    vec3 V = normalize(vary_ViewDir);\n"
    "    vec3 H = normalize(L + V);\n"
    "    vec2 lightProjTC = vary_LightProjection.xy / vary_LightProjection.w;\n"
    "    vec3 lightColor  = texture(u_LightProjection, lightProjTC).rgb;\n"
    "    float falloff    = texture(u_LightFalloff, vary_LightFalloff).r;\n"
    "    vec3 attenuation = lightColor * falloff;\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec3 diffuse = texture(u_DiffuseMap, vary_TexCoord_Diffuse.xy).rgb;\n"
    "    diffuse *= u_DiffuseColor.rgb * NdotL;\n"
    "    float NdotH = clamp(dot(N, H), 0.0, 1.0);\n"
    "    vec3 specLookup = texture(u_SpecularTable, vec2(NdotH, 0.5)).rgb;\n"
    "    vec3 specular = texture(u_SpecularMap, vary_TexCoord_Specular.xy).rgb;\n"
    "    specular *= u_SpecularColor.rgb * specLookup;\n"
    "    vec3 color = (diffuse + specular) * attenuation * vary_Color.rgb;\n"
    "    vec4 result = vec4(color, vary_Color.a);\n"
    "    if (u_ApplyGamma) {\n"
    "        vec3 brightened = clamp(result.rgb * u_GammaBrightness.rgb, 0.0, 1.0);\n"
    "        result.rgb = pow(brightened, vec3(u_GammaBrightness.w));\n"
    "    }\n"
    "    fragColor = result;\n"
    "}\n";

// --- Shadow volume vertex shader ---
static const char *shadowVertSrc = "#version 330 core\n"
                                   "layout(location = 0) in vec4 in_Position;\n"
                                   "uniform vec4 u_LightOrigin;\n"
                                   "uniform mat4 u_ModelViewProjection;\n"
                                   "void main() {\n"
                                   "    if (in_Position.w < 0.5) {\n"
                                   "        vec3 dir = in_Position.xyz - u_LightOrigin.xyz;\n"
                                   "        gl_Position = u_ModelViewProjection * vec4(dir, 0.0);\n"
                                   "    } else {\n"
                                   "        gl_Position = u_ModelViewProjection * vec4(in_Position.xyz, 1.0);\n"
                                   "    }\n"
                                   "}\n";

// --- Shadow volume fragment shader (stencil only, no color output) ---
static const char *shadowFragSrc = "#version 330 core\n"
                                   "out vec4 fragColor;\n"
                                   "void main() {\n"
                                   "    fragColor = vec4(0.0);\n" // stencil-only pass; color is masked out
                                   "}\n";

// --- Depth pre-pass vertex shader ---
static const char *depthVertSrc = "#version 330 core\n"
                                  "layout(location = 0) in vec3 in_Position;\n"
                                  "layout(location = 8) in vec2 in_TexCoord;\n"
                                  "uniform mat4 u_ModelViewProjection;\n"
                                  "uniform vec4 u_TextureMatrixS;\n"
                                  "uniform vec4 u_TextureMatrixT;\n"
                                  "out vec2 vary_TexCoord;\n"
                                  "void main() {\n"
                                  "    vec4 tc = vec4(in_TexCoord, 0.0, 1.0);\n"
                                  "    vary_TexCoord.x = dot(tc, u_TextureMatrixS);\n"
                                  "    vary_TexCoord.y = dot(tc, u_TextureMatrixT);\n"
                                  "    gl_Position = u_ModelViewProjection * vec4(in_Position, 1.0);\n"
                                  "}\n";

// --- Depth pre-pass fragment shader ---
static const char *depthFragSrc = "#version 330 core\n"
                                  "in vec2 vary_TexCoord;\n"
                                  "uniform sampler2D u_DiffuseMap;\n"
                                  "uniform bool u_AlphaTest;\n"
                                  "uniform float u_AlphaTestThreshold;\n"
                                  "out vec4 fragColor;\n"
                                  "void main() {\n"
                                  "    if (u_AlphaTest) {\n"
                                  "        float a = texture(u_DiffuseMap, vary_TexCoord).a;\n"
                                  "        if (a < u_AlphaTestThreshold) discard;\n"
                                  "    }\n"
                                  "    fragColor = vec4(0.0);\n"
                                  "}\n";

// ---------------------------------------------------------------------------
// Helper: compile and link a GLSL program
// ---------------------------------------------------------------------------

static GLuint R_GLSL_CompileShader(GLenum type, const char *src, const char *name)
{
    GLuint shader = qglCreateShader(type);
    qglShaderSource(shader, 1, &src, NULL);
    qglCompileShader(shader);

    GLint status = GL_FALSE;
    qglGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        char log[2048];
        qglGetShaderInfoLog(shader, sizeof(log), NULL, log);
        common->Warning("R_GLSL: Failed to compile %s shader '%s':\n%s",
                        (type == GL_VERTEX_SHADER ? "vertex" : "fragment"), name, log);
        qglDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint R_GLSL_LinkProgram(GLuint vert, GLuint frag, const char *name, const char **attribNames,
                                 const GLuint *attribLocs, int numAttribs)
{
    GLuint prog = qglCreateProgram();
    qglAttachShader(prog, vert);
    qglAttachShader(prog, frag);

    // Bind attribute locations before linking
    for (int i = 0; i < numAttribs; i++)
    {
        qglBindAttribLocation(prog, attribLocs[i], attribNames[i]);
    }

    qglLinkProgram(prog);

    GLint status = GL_FALSE;
    qglGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status == GL_FALSE)
    {
        char log[2048];
        qglGetProgramInfoLog(prog, sizeof(log), NULL, log);
        common->Warning("R_GLSL: Failed to link program '%s':\n%s", name, log);
        qglDeleteProgram(prog);
        return 0;
    }

    // Shaders are linked; we don't need to keep them attached
    qglDeleteShader(vert);
    qglDeleteShader(frag);

    return prog;
}

// ---------------------------------------------------------------------------
// Get uniform locations for the interaction program
// ---------------------------------------------------------------------------

static void R_GLSL_GetInteractionUniforms(GLuint prog, glslInteractionUniforms_t &u)
{
    u.u_LightOrigin = qglGetUniformLocation(prog, "u_LightOrigin");
    u.u_ViewOrigin = qglGetUniformLocation(prog, "u_ViewOrigin");
    u.u_LightProjectionS = qglGetUniformLocation(prog, "u_LightProjectionS");
    u.u_LightProjectionT = qglGetUniformLocation(prog, "u_LightProjectionT");
    u.u_LightProjectionQ = qglGetUniformLocation(prog, "u_LightProjectionQ");
    u.u_LightFalloffS = qglGetUniformLocation(prog, "u_LightFalloffS");
    u.u_BumpMatrixS = qglGetUniformLocation(prog, "u_BumpMatrixS");
    u.u_BumpMatrixT = qglGetUniformLocation(prog, "u_BumpMatrixT");
    u.u_DiffuseMatrixS = qglGetUniformLocation(prog, "u_DiffuseMatrixS");
    u.u_DiffuseMatrixT = qglGetUniformLocation(prog, "u_DiffuseMatrixT");
    u.u_SpecularMatrixS = qglGetUniformLocation(prog, "u_SpecularMatrixS");
    u.u_SpecularMatrixT = qglGetUniformLocation(prog, "u_SpecularMatrixT");
    u.u_ColorModulate = qglGetUniformLocation(prog, "u_ColorModulate");
    u.u_ColorAdd = qglGetUniformLocation(prog, "u_ColorAdd");
    u.u_ModelViewProjection = qglGetUniformLocation(prog, "u_ModelViewProjection");
    u.u_DiffuseColor = qglGetUniformLocation(prog, "u_DiffuseColor");
    u.u_SpecularColor = qglGetUniformLocation(prog, "u_SpecularColor");
    u.u_GammaBrightness = qglGetUniformLocation(prog, "u_GammaBrightness");
    u.u_ApplyGamma = qglGetUniformLocation(prog, "u_ApplyGamma");
    u.u_BumpMap = qglGetUniformLocation(prog, "u_BumpMap");
    u.u_LightFalloff = qglGetUniformLocation(prog, "u_LightFalloff");
    u.u_LightProjection = qglGetUniformLocation(prog, "u_LightProjection");
    u.u_DiffuseMap = qglGetUniformLocation(prog, "u_DiffuseMap");
    u.u_SpecularMap = qglGetUniformLocation(prog, "u_SpecularMap");
    u.u_SpecularTable = qglGetUniformLocation(prog, "u_SpecularTable");
}

// ---------------------------------------------------------------------------
// R_GLSL_Available
// ---------------------------------------------------------------------------

bool R_GLSL_Available(void)
{
    return glslInitialized && glslProgs.interaction.isValid;
}

// ---------------------------------------------------------------------------
// R_GLSL_Init - load GL 2.0 function pointers, compile and link shaders
// ---------------------------------------------------------------------------

void R_GLSL_Init(void)
{
    memset(&glslProgs, 0, sizeof(glslProgs));
    glslInitialized = false;

    if (glConfig.glVersion < 2.0f)
    {
        common->Printf("R_GLSL: OpenGL 2.0 not available, GLSL backend disabled.\n");
        return;
    }

// Load function pointers
#define LOAD_GLPROC(var, name)                                                                                         \
    var = (decltype(var))GLimp_ExtensionPointer(name);                                                                 \
    if (!var)                                                                                                          \
    {                                                                                                                  \
        common->Printf("R_GLSL: missing %s\n", name);                                                                  \
        return;                                                                                                        \
    }

    LOAD_GLPROC(qglCreateShader, "glCreateShader")
    LOAD_GLPROC(qglShaderSource, "glShaderSource")
    LOAD_GLPROC(qglCompileShader, "glCompileShader")
    LOAD_GLPROC(qglGetShaderiv, "glGetShaderiv")
    LOAD_GLPROC(qglGetShaderInfoLog, "glGetShaderInfoLog")
    LOAD_GLPROC(qglCreateProgram, "glCreateProgram")
    LOAD_GLPROC(qglAttachShader, "glAttachShader")
    LOAD_GLPROC(qglBindAttribLocation, "glBindAttribLocation")
    LOAD_GLPROC(qglLinkProgram, "glLinkProgram")
    LOAD_GLPROC(qglGetProgramiv, "glGetProgramiv")
    LOAD_GLPROC(qglGetProgramInfoLog, "glGetProgramInfoLog")
    LOAD_GLPROC(qglUseProgram, "glUseProgram")
    LOAD_GLPROC(qglGetUniformLocation, "glGetUniformLocation")
    LOAD_GLPROC(qglUniform1i, "glUniform1i")
    LOAD_GLPROC(qglUniform1f, "glUniform1f")
    LOAD_GLPROC(qglUniform4fv, "glUniform4fv")
    LOAD_GLPROC(qglUniformMatrix4fv, "glUniformMatrix4fv")
    LOAD_GLPROC(qglDeleteShader, "glDeleteShader")
    LOAD_GLPROC(qglDeleteProgram, "glDeleteProgram")
    LOAD_GLPROC(qglVertexAttribPointer, "glVertexAttribPointer")
    LOAD_GLPROC(qglEnableVertexAttribArray, "glEnableVertexAttribArray")
    LOAD_GLPROC(qglDisableVertexAttribArray, "glDisableVertexAttribArray")
#undef LOAD_GLPROC

    // --- Compile the interaction program ---
    {
        static const char *attribNames[] = {"in_Position", "in_Color",     "in_TexCoord",
                                            "in_Tangent",  "in_BiTangent", "in_Normal"};
        static const GLuint attribLocs[] = {0, 3, 8, 9, 10, 11};
        static const int numAttribs = 6;

        GLuint vert = R_GLSL_CompileShader(GL_VERTEX_SHADER, interactionVertSrc, "interaction.vert");
        GLuint frag = R_GLSL_CompileShader(GL_FRAGMENT_SHADER, interactionFragSrc, "interaction.frag");

        if (vert && frag)
        {
            glslProgs.interaction.progId =
                R_GLSL_LinkProgram(vert, frag, "interaction", attribNames, attribLocs, numAttribs);
            glslProgs.interaction.isValid = (glslProgs.interaction.progId != 0);
        }

        if (glslProgs.interaction.isValid)
        {
            R_GLSL_GetInteractionUniforms(glslProgs.interaction.progId, glslProgs.interactionUniforms);

            // Bind texture sampler units (must be done after linking)
            qglUseProgram(glslProgs.interaction.progId);
            qglUniform1i(glslProgs.interactionUniforms.u_BumpMap, 1);
            qglUniform1i(glslProgs.interactionUniforms.u_LightFalloff, 2);
            qglUniform1i(glslProgs.interactionUniforms.u_LightProjection, 3);
            qglUniform1i(glslProgs.interactionUniforms.u_DiffuseMap, 4);
            qglUniform1i(glslProgs.interactionUniforms.u_SpecularMap, 5);
            qglUniform1i(glslProgs.interactionUniforms.u_SpecularTable, 6);
            qglUseProgram(0);
        }
    }

    // --- Compile the shadow volume program ---
    {
        static const char *attribNames[] = {"in_Position"};
        static const GLuint attribLocs[] = {0};

        GLuint vert = R_GLSL_CompileShader(GL_VERTEX_SHADER, shadowVertSrc, "shadow.vert");
        GLuint frag = R_GLSL_CompileShader(GL_FRAGMENT_SHADER, shadowFragSrc, "shadow.frag");

        if (vert && frag)
        {
            glslProgs.shadow.progId = R_GLSL_LinkProgram(vert, frag, "shadow", attribNames, attribLocs, 1);
            glslProgs.shadow.isValid = (glslProgs.shadow.progId != 0);
        }

        if (glslProgs.shadow.isValid)
        {
            glslProgs.shadowUniforms.u_LightOrigin = qglGetUniformLocation(glslProgs.shadow.progId, "u_LightOrigin");
            glslProgs.shadowUniforms.u_ModelViewProjection =
                qglGetUniformLocation(glslProgs.shadow.progId, "u_ModelViewProjection");
        }
    }

    // --- Compile the depth pre-pass program ---
    {
        static const char *attribNames[] = {"in_Position", "in_TexCoord"};
        static const GLuint attribLocs[] = {0, 8};

        GLuint vert = R_GLSL_CompileShader(GL_VERTEX_SHADER, depthVertSrc, "depth.vert");
        GLuint frag = R_GLSL_CompileShader(GL_FRAGMENT_SHADER, depthFragSrc, "depth.frag");

        if (vert && frag)
        {
            glslProgs.depth.progId = R_GLSL_LinkProgram(vert, frag, "depth", attribNames, attribLocs, 2);
            glslProgs.depth.isValid = (glslProgs.depth.progId != 0);
        }

        if (glslProgs.depth.isValid)
        {
            glslProgs.depthUniforms.u_ModelViewProjection =
                qglGetUniformLocation(glslProgs.depth.progId, "u_ModelViewProjection");
            glslProgs.depthUniforms.u_TextureMatrixS =
                qglGetUniformLocation(glslProgs.depth.progId, "u_TextureMatrixS");
            glslProgs.depthUniforms.u_TextureMatrixT =
                qglGetUniformLocation(glslProgs.depth.progId, "u_TextureMatrixT");
            glslProgs.depthUniforms.u_DiffuseMap = qglGetUniformLocation(glslProgs.depth.progId, "u_DiffuseMap");
            glslProgs.depthUniforms.u_AlphaTest = qglGetUniformLocation(glslProgs.depth.progId, "u_AlphaTest");
            glslProgs.depthUniforms.u_AlphaTestThreshold =
                qglGetUniformLocation(glslProgs.depth.progId, "u_AlphaTestThreshold");

            qglUseProgram(glslProgs.depth.progId);
            qglUniform1i(glslProgs.depthUniforms.u_DiffuseMap, 4);
            qglUseProgram(0);
        }
    }

    glslInitialized = true;

    common->Printf("R_GLSL: initialized (interaction=%s, shadow=%s, depth=%s)\n",
                   glslProgs.interaction.isValid ? "OK" : "FAIL", glslProgs.shadow.isValid ? "OK" : "FAIL",
                   glslProgs.depth.isValid ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------------
// R_GLSL_Shutdown
// ---------------------------------------------------------------------------

void R_GLSL_Shutdown(void)
{
    if (!glslInitialized)
        return;
    if (!qglDeleteProgram)
        return;

    if (glslProgs.interaction.progId)
        qglDeleteProgram(glslProgs.interaction.progId);
    if (glslProgs.shadow.progId)
        qglDeleteProgram(glslProgs.shadow.progId);
    if (glslProgs.depth.progId)
        qglDeleteProgram(glslProgs.depth.progId);

    memset(&glslProgs, 0, sizeof(glslProgs));
    glslInitialized = false;
}

// ---------------------------------------------------------------------------
// Utility: compute MVP matrix from space->modelViewMatrix and projectionMatrix
// ---------------------------------------------------------------------------

static void R_GLSL_ComputeMVP(const drawSurf_t *surf, float *mvp)
{
    // MVP = projection * modelView (column-major, OpenGL convention)
    const float *mv = surf->space->modelViewMatrix;
    const float *pr = backEnd.viewDef->projectionMatrix;

    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                sum += pr[k * 4 + r] * mv[c * 4 + k]; // column-major multiply
            }
            mvp[c * 4 + r] = sum;
        }
    }
}

// ---------------------------------------------------------------------------
// RB_GLSL_DrawInteraction - draw a single light-surface interaction
// Mirrors RB_ARB2_DrawInteraction()
// ---------------------------------------------------------------------------

static void RB_GLSL_DrawInteraction(const drawInteraction_t *din)
{
    const glslInteractionUniforms_t &u = glslProgs.interactionUniforms;

    // Upload vertex program parameters
    qglUniform4fv(u.u_LightOrigin, 1, din->localLightOrigin.ToFloatPtr());
    qglUniform4fv(u.u_ViewOrigin, 1, din->localViewOrigin.ToFloatPtr());
    qglUniform4fv(u.u_LightProjectionS, 1, din->lightProjection[0].ToFloatPtr());
    qglUniform4fv(u.u_LightProjectionT, 1, din->lightProjection[1].ToFloatPtr());
    qglUniform4fv(u.u_LightProjectionQ, 1, din->lightProjection[2].ToFloatPtr());
    qglUniform4fv(u.u_LightFalloffS, 1, din->lightProjection[3].ToFloatPtr());
    qglUniform4fv(u.u_BumpMatrixS, 1, din->bumpMatrix[0].ToFloatPtr());
    qglUniform4fv(u.u_BumpMatrixT, 1, din->bumpMatrix[1].ToFloatPtr());
    qglUniform4fv(u.u_DiffuseMatrixS, 1, din->diffuseMatrix[0].ToFloatPtr());
    qglUniform4fv(u.u_DiffuseMatrixT, 1, din->diffuseMatrix[1].ToFloatPtr());
    qglUniform4fv(u.u_SpecularMatrixS, 1, din->specularMatrix[0].ToFloatPtr());
    qglUniform4fv(u.u_SpecularMatrixT, 1, din->specularMatrix[1].ToFloatPtr());

    // Vertex color modulation (same SVC_* logic as ARB backend)
    static const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    static const float one[4] = {1.f, 1.f, 1.f, 1.f};
    static const float neg[4] = {-1.f, -1.f, -1.f, -1.f};

    switch (din->vertexColor)
    {
    case SVC_IGNORE:
        qglUniform4fv(u.u_ColorModulate, 1, zero);
        qglUniform4fv(u.u_ColorAdd, 1, one);
        break;
    case SVC_MODULATE:
        qglUniform4fv(u.u_ColorModulate, 1, one);
        qglUniform4fv(u.u_ColorAdd, 1, zero);
        break;
    case SVC_INVERSE_MODULATE:
        qglUniform4fv(u.u_ColorModulate, 1, neg);
        qglUniform4fv(u.u_ColorAdd, 1, one);
        break;
    }

    // Fragment parameters
    qglUniform4fv(u.u_DiffuseColor, 1, din->diffuseColor.ToFloatPtr());
    qglUniform4fv(u.u_SpecularColor, 1, din->specularColor.ToFloatPtr());

    // Gamma/brightness (mirror PP_GAMMA_BRIGHTNESS logic)
    if (r_gammaInShader.GetBool())
    {
        float gb[4];
        gb[0] = gb[1] = gb[2] = r_brightness.GetFloat();
        gb[3] = 1.0f / r_gamma.GetFloat();
        qglUniform4fv(u.u_GammaBrightness, 1, gb);
        qglUniform1i(u.u_ApplyGamma, 1);
    }
    else
    {
        qglUniform1i(u.u_ApplyGamma, 0);
    }

    // Bind textures (units match the ARB backend)
    // Unit 1: bump map
    qglActiveTextureARB(GL_TEXTURE1_ARB);
    din->bumpImage->Bind();

    // Unit 2: light falloff
    qglActiveTextureARB(GL_TEXTURE2_ARB);
    din->lightFalloffImage->Bind();

    // Unit 3: light projection
    qglActiveTextureARB(GL_TEXTURE3_ARB);
    din->lightImage->Bind();

    // Unit 4: diffuse map
    qglActiveTextureARB(GL_TEXTURE4_ARB);
    din->diffuseImage->Bind();

    // Unit 5: specular map
    qglActiveTextureARB(GL_TEXTURE5_ARB);
    din->specularImage->Bind();

    // Restore active texture
    qglActiveTextureARB(GL_TEXTURE0_ARB);

    RB_DrawElementsWithCounters(din->surf->geo);
}

// ---------------------------------------------------------------------------
// RB_GLSL_CreateDrawInteractions - set up vertex arrays, run interaction loop
// Mirrors RB_ARB2_CreateDrawInteractions()
// ---------------------------------------------------------------------------

static void RB_GLSL_CreateDrawInteractions(const drawSurf_t *surf)
{
    if (!surf)
        return;

    const glslInteractionUniforms_t &u = glslProgs.interactionUniforms;

    GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | backEnd.depthFunc);

    qglUseProgram(glslProgs.interaction.progId);

    // Enable vertex attribute arrays
    qglEnableVertexAttribArray(8);  // texcoord
    qglEnableVertexAttribArray(9);  // tangent
    qglEnableVertexAttribArray(10); // bitangent
    qglEnableVertexAttribArray(11); // normal
    qglEnableVertexAttribArray(3);  // color

    // Texture unit 0: cube map for normalization (keep binding for ambient lights)
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    if (backEnd.vLight->lightShader->IsAmbientLight())
    {
        globalImages->ambientNormalMap->Bind();
    }
    else
    {
        globalImages->normalCubeMapImage->Bind();
    }

    // Texture unit 6: specular lookup table
    qglActiveTextureARB(GL_TEXTURE6_ARB);
    globalImages->specularTableImage->Bind();
    qglActiveTextureARB(GL_TEXTURE0_ARB);

    for (; surf; surf = surf->nextOnLight)
    {
        idDrawVert *ac = (idDrawVert *)vertexCache.Position(surf->geo->ambientCache);

        // Bind vertex attribute pointers
        qglVertexAttribPointer(0, 3, GL_FLOAT, false, sizeof(idDrawVert), ac->xyz.ToFloatPtr());
        qglVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, true, sizeof(idDrawVert), ac->color);
        qglVertexAttribPointer(8, 2, GL_FLOAT, false, sizeof(idDrawVert), ac->st.ToFloatPtr());
        qglVertexAttribPointer(9, 3, GL_FLOAT, false, sizeof(idDrawVert), ac->tangents[0].ToFloatPtr());
        qglVertexAttribPointer(10, 3, GL_FLOAT, false, sizeof(idDrawVert), ac->tangents[1].ToFloatPtr());
        qglVertexAttribPointer(11, 3, GL_FLOAT, false, sizeof(idDrawVert), ac->normal.ToFloatPtr());

        // Upload MVP matrix for this surface
        float mvp[16];
        R_GLSL_ComputeMVP(surf, mvp);
        qglUniformMatrix4fv(u.u_ModelViewProjection, 1, GL_FALSE, mvp);

        RB_CreateSingleDrawInteractions(surf, RB_GLSL_DrawInteraction);
    }

    // Disable vertex attribute arrays
    qglDisableVertexAttribArray(8);
    qglDisableVertexAttribArray(9);
    qglDisableVertexAttribArray(10);
    qglDisableVertexAttribArray(11);
    qglDisableVertexAttribArray(3);

    // Unbind textures
    qglActiveTextureARB(GL_TEXTURE6_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE5_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE4_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE3_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE2_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE1_ARB);
    qglBindTexture(GL_TEXTURE_2D, 0);
    qglActiveTextureARB(GL_TEXTURE0_ARB);

    qglUseProgram(0);
}

// ---------------------------------------------------------------------------
// RB_GLSL_StencilShadowPass - draw shadow volumes using the GLSL shadow program
// Mirrors RB_StencilShadowPass() but with GLSL instead of ARB vertex program.
// ---------------------------------------------------------------------------

static void RB_GLSL_StencilShadowPass(const drawSurf_t *drawSurfs)
{
    if (!drawSurfs)
        return;
    if (!glslProgs.shadow.isValid)
        return;

    qglUseProgram(glslProgs.shadow.progId);
    qglEnableVertexAttribArray(0);

    // Shadow pass uses the same stencil state as the ARB version.
    // The glState machinery is unchanged - we just swap the shader.
    RB_StencilShadowPass(drawSurfs); // delegate to existing stencil logic

    qglDisableVertexAttribArray(0);
    qglUseProgram(0);
}

// ---------------------------------------------------------------------------
// RB_GLSL_DrawInteractions - main per-view interaction loop
// Mirrors RB_ARB2_DrawInteractions()
// ---------------------------------------------------------------------------

void RB_GLSL_DrawInteractions(void)
{
    viewLight_t *vLight;

    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglDisableClientState(GL_TEXTURE_COORD_ARRAY);

    for (vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next)
    {
        backEnd.vLight = vLight;

        if (vLight->lightShader->IsFogLight())
            continue;
        if (vLight->lightShader->IsBlendLight())
            continue;

        if (!vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions)
        {
            continue;
        }

        // Clear stencil for this light if shadows are present
        if (vLight->globalShadows || vLight->localShadows)
        {
            backEnd.currentScissor = vLight->scissorRect;
            if (r_useScissor.GetBool())
            {
                qglScissor(backEnd.viewDef->viewport.x1 + backEnd.currentScissor.x1,
                           backEnd.viewDef->viewport.y1 + backEnd.currentScissor.y1,
                           backEnd.currentScissor.x2 + 1 - backEnd.currentScissor.x1,
                           backEnd.currentScissor.y2 + 1 - backEnd.currentScissor.y1);
            }
            qglClear(GL_STENCIL_BUFFER_BIT);
        }
        else
        {
            qglStencilFunc(GL_ALWAYS, 128, 255);
        }

        // Shadow + interaction passes (same order as ARB path)
        RB_StencilShadowPass(vLight->globalShadows);
        RB_GLSL_CreateDrawInteractions(vLight->localInteractions);
        RB_StencilShadowPass(vLight->localShadows);
        RB_GLSL_CreateDrawInteractions(vLight->globalInteractions);

        // Translucent surfaces: no stencil shadow
        if (r_skipTranslucent.GetBool())
            continue;

        qglStencilFunc(GL_ALWAYS, 128, 255);
        backEnd.depthFunc = GLS_DEPTHFUNC_LESS;
        RB_GLSL_CreateDrawInteractions(vLight->translucentInteractions);
        backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
    }

    qglStencilFunc(GL_ALWAYS, 128, 255);
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglEnableClientState(GL_TEXTURE_COORD_ARRAY);
}
