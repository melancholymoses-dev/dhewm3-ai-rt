/*
===========================================================================

Vulkan Pipelines
dhewm3-rt Vulkan backend - graphics pipeline and descriptor set management.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI, and
may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source Code.

It is distributed under the same modified GNU General Public License Version 3
of the original Doom 3 GPL Source Code release.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "version.h"

PFN_vkCmdPushDescriptorSetKHR pfn_vkCmdPushDescriptorSetKHR = NULL;

extern VkShaderModule VK_LoadSPIRV(const char *path);
extern VkShaderModule VK_LoadSPIRVFromMemory(const uint32_t *code, size_t codeSize);

// ---------------------------------------------------------------------------
// Per-interaction uniform block (matches draw_glsl.cpp uniforms)
// Uploaded as a push constant or small UBO per draw call.
// ---------------------------------------------------------------------------

struct VkInteractionUBO
{
    float lightOrigin[4];
    float viewOrigin[4];
    float lightProjectionS[4];
    float lightProjectionT[4];
    float lightProjectionQ[4];
    float lightFalloffS[4];
    float bumpMatrixS[4];
    float bumpMatrixT[4];
    float diffuseMatrixS[4];
    float diffuseMatrixT[4];
    float specularMatrixS[4];
    float specularMatrixT[4];
    float colorModulate[4];
    float colorAdd[4];
    float modelViewProjection[16];
    float diffuseColor[4];
    float specularColor[4];
    float gammaBrightness[4];
    int applyGamma;
    float screenWidth;  // framebuffer width  (for shadow mask UV)
    float screenHeight; // framebuffer height (for shadow mask UV)
    int useShadowMask;  // 1 when RT shadow mask is valid this frame
    int useAO;          // 1 when RT AO mask is valid this frame
    float lightScale;   // backEnd.overBright — final color multiplier before gamma
    int useReflections; // 1 when RT reflection buffer is valid this frame
    int useGI;          // 1 when RT GI buffer is valid this frame (Phase 6.1)
};
// Total: 14*16 + 64 + 3*16 + 4 + 8 + 4 + 4 + 4 + 4 + 4 = 368 bytes; INTERACTION_UBO_SIZE = 384.

// ---------------------------------------------------------------------------
// Pipeline layout for the interaction pass
// Binding layout:
//   set 0 binding 0: UBO (VkInteractionUBO)
//   set 0 binding 1..6: combined image samplers (bump, falloff, proj, diffuse, specular, specTable)
//   set 0 binding 7:    shadow mask sampler (RT shadow output or 1x1 white fallback)
//   set 0 binding 8:    AO mask sampler (RT output or 1x1 white fallback)
//   set 0 binding 9:    reflection map sampler (RT reflection output or 1x1 black fallback)
//   set 0 binding 10:   GI map sampler (RT GI output or 1x1 black fallback; Phase 6.1)
// ---------------------------------------------------------------------------

// vkPipelines_t declared in vk_common.h
vkPipelines_t vkPipes;

#ifndef DHEWM3_BUILD_TAG
#define DHEWM3_BUILD_TAG "local"
#endif

#ifndef DHEWM3_BUILD_GIT
#define DHEWM3_BUILD_GIT GIT_COMMIT_HASH
#endif

// Runtime stamp for proving which binary is executing from logs.
static const char *VK_BUILD_SIGNATURE =
    "tag=" DHEWM3_BUILD_TAG " git=" DHEWM3_BUILD_GIT " built=" __DATE__ " " __TIME__;

// ---------------------------------------------------------------------------
// Descriptor set layout helpers
// ---------------------------------------------------------------------------

static VkDescriptorSetLayout VK_CreateInteractionDescLayout(void)
{
    VkDescriptorSetLayoutBinding bindings[11] = {};

    // Binding 0: UBO
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Bindings 1-6: samplers (bump, falloff, lightProj, diffuse, specular, specTable)
    for (int i = 1; i <= 6; i++)
    {
        bindings[i].binding = (uint32_t)i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    // Binding 7: shadow mask (RT shadow output, or 1x1 white fallback when RT is off)
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 8: AO mask (RT AO output, or 1x1 white fallback when RTAO is off)
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 9: reflection map (RT reflection output, or 1x1 black fallback when off)
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 10: GI map (RT GI output, or 1x1 black fallback when off; Phase 6.1)
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = 11;
    info.pBindings = bindings;

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &layout));
    return layout;
}

static VkDescriptorSetLayout VK_CreateShadowDescLayout(void)
{
    // Shadow pass only needs a UBO with MVP + light origin
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = 1;
    info.pBindings = &binding;

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &layout));
    return layout;
}

// ---------------------------------------------------------------------------
// Vertex input description matching idDrawVert
// ---------------------------------------------------------------------------

static void VK_GetInteractionVertexInput(VkVertexInputBindingDescription *binding,
                                         VkVertexInputAttributeDescription *attrs, uint32_t *numAttrs)
{
    // One binding: interleaved idDrawVert buffer
    binding->binding = 0;
    binding->stride = sizeof(idDrawVert);
    binding->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Match the GLSL attribute locations in draw_glsl.cpp
    uint32_t i = 0;

    // location=0: position (vec3)
    attrs[i] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(idDrawVert, xyz)};
    i++;
    // location=3: color (rgba ubyte, normalized)
    attrs[i] = {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(idDrawVert, color)};
    i++;
    // location=8: texcoord (vec2)
    attrs[i] = {8, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(idDrawVert, st)};
    i++;
    // location=9: tangent[0] (vec3)
    attrs[i] = {9, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(idDrawVert, tangents[0])};
    i++;
    // location=10: tangent[1] / bitangent (vec3)
    attrs[i] = {10, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(idDrawVert, tangents[1])};
    i++;
    // location=11: normal (vec3)
    attrs[i] = {11, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(idDrawVert, normal)};
    i++;

    *numAttrs = i;
}

// ---------------------------------------------------------------------------
// VK_CreateInteractionPipeline
// enableStencil=true  → EQUAL 128 stencil (opaque interactions behind shadow volumes)
// enableStencil=false → stencil disabled (translucent interactions, no shadow culling)
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateInteractionPipeline(VkPipelineLayout layout, bool enableStencil = true,
                                               VkCompareOp depthOp = VK_COMPARE_OP_EQUAL)
{
    // Load SPIR-V shaders compiled from the GLSL files
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/interaction.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/interaction.frag.spv");
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
    {
        common->Warning("VK: interaction shaders not found, Vulkan interaction pipeline unavailable");
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    // Cull mode is set dynamically (vkCmdSetCullModeEXT / VK_DYNAMIC_STATE_CULL_MODE_EXT)
    // so CT_TWO_SIDED materials can disable culling per draw. Default BACK_BIT is overridden
    // each draw call before vkCmdDrawIndexed, so this value is a no-op placeholder.
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    // Y-flip viewport (negative height) inverts winding order in Vulkan window space.
    // OpenGL CCW front faces become CW after Y-flip, so we tell Vulkan CW = front.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    // Depth bias enabled for per-surface polygon offset (MF_POLYGONOFFSET decals/overlays).
    // The actual bias values are set dynamically via vkCmdSetDepthBias before each draw.
    rasterizer.depthBiasEnable = VK_TRUE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth+stencil: depth test with caller-supplied compare op (default EQUAL).
    // Opaque interactions use EQUAL to match the GL path (backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL):
    // only pixels that were written by the depth prepass get lit, preventing any double-drawing.
    // Translucent interactions use LEQUAL since translucent surfaces skip the prepass.
    // Stencil: GEQUAL 128 — matches Doom3 GL path after shadow pass.
    // Lit pixels are >=128; shadowed pixels decrement below 128.
    // With no rasterised shadow pass this always passes; RT shadow mask is applied in-shader.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE; // don't write depth in interaction pass
    depthStencil.depthCompareOp = depthOp;
    depthStencil.stencilTestEnable = enableStencil ? VK_TRUE : VK_FALSE;
    depthStencil.front.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // draw where stencil >= 128 (lit area)
    depthStencil.front.compareMask = 255;
    depthStencil.front.reference = 128;
    depthStencil.back = depthStencil.front;

    // Blending: additive (ONE + ONE) for light accumulation
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    // Dynamic state: viewport, scissor, depth bias (polygon offset), and cull mode (two-sided materials).
    VkDynamicState dynStates[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                   VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 4;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);

    return pipeline;
}

// ---------------------------------------------------------------------------
// VK_CreateShadowPipelineZFail - stencil shadow volume pipeline
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateShadowPipelineZFail(VkPipelineLayout layout, bool mirrorView)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/shadow.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/shadow.frag.spv");
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 NULL,
                 0,
                 VK_SHADER_STAGE_VERTEX_BIT,
                 vertModule,
                 "main",
                 NULL};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 NULL,
                 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT,
                 fragModule,
                 "main",
                 NULL};

    // Shadow vertex: only position (vec4 with w=0 or 1 for extrusion)
    VkVertexInputBindingDescription binding = {0, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // Shadow volumes: no culling (two-sided), depth clamp for infinite extrusion
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    // Y-flip viewport inverts winding — same correction as interaction pipeline.
    // Carmack's Reverse front/back stencil ops depend on correct face identification.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthClampEnable = VK_FALSE; // Doom 3 uses infinite-far-plane projection (z=-0.999),
                                            // so shadow vertices at infinity stay within [0,1] — no clamping
                                            // needed.  Disabling matches GL behavior: fragments outside the
                                            // frustum are clipped rather than clamped, preventing spurious
                                            // stencil marks from near-plane overflow.
    rasterizer.lineWidth = 1.0f;
    // Match GL shadow polygon offset behavior; values are set dynamically from cvars per draw.
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 0.0f;
    rasterizer.depthBiasSlopeFactor = 0.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Z-fail / Carmack's Reverse uses depthFailOp only.
    // For a shadowed pixel (between near and far shadow caps):
    //   near cap (front face, faces camera): d_sv < d_scene → depth PASSES → no event
    //   far  cap (back  face, faces away  ): d_sv > d_scene → depth FAILS  → stencil fires
    // Correct convention (matching GL): front=INCR, back=DECR
    //   non-mirror: front=INCR, back=DECR
    //   mirror:     front=DECR, back=INCR (winding reversed in mirrored view)
    // In this Vulkan pipeline, front/back refer to post-viewport face classification
    // (with our Y-flipped viewport and frontFace=CLOCKWISE convention).
    VkStencilOpState front = {};
    front.failOp = VK_STENCIL_OP_KEEP;
    front.passOp = VK_STENCIL_OP_KEEP;
    front.depthFailOp = mirrorView ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_INCREMENT_AND_WRAP;
    front.compareOp = VK_COMPARE_OP_ALWAYS;
    front.compareMask = 0xFF;
    front.writeMask = 0xFF;
    front.reference = 0;

    VkStencilOpState back = front;
    back.depthFailOp = mirrorView ? VK_STENCIL_OP_INCREMENT_AND_WRAP : VK_STENCIL_OP_DECREMENT_AND_WRAP;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.stencilTestEnable = VK_TRUE;
    depthStencil.front = front;
    depthStencil.back = back;

    // Color write disabled for shadow pass (stencil-only)
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.colorWriteMask = 0;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// ---------------------------------------------------------------------------
// VK_CreateShadowPipelineZPass
// Z-pass (depth-PASS) stencil shadow pipeline for the common case where the
// camera is outside the shadow volume.  Only silhouette quads are drawn (no
// front/back caps), so there is no co-planar depth precision issue.
// Front faces: passOp = INCREMENT   (shadow volume in front → lit surface behind → in shadow)
// Back  faces: passOp = DECREMENT   (back side of volume → cancels for pixels outside)
// Both faces:  depthFailOp = KEEP   (shadow volume behind scene → not in shadow)
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateShadowPipelineZPass(VkPipelineLayout layout, bool mirrorView)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/shadow.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/shadow.frag.spv");
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 NULL,
                 0,
                 VK_SHADER_STAGE_VERTEX_BIT,
                 vertModule,
                 "main",
                 NULL};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 NULL,
                 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT,
                 fragModule,
                 "main",
                 NULL};

    VkVertexInputBindingDescription binding = {0, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // two-sided: both faces contribute
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    // Keep Z-pass offset consistent with Z-fail (GL enables polygon offset for the whole shadow pass).
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 0.0f;
    rasterizer.depthBiasSlopeFactor = 0.0f;
    rasterizer.lineWidth = 1.0f;
    // No depthClamp needed for Z-pass (no back caps at infinity)

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // GL parity note (keep in sync with draw_common.cpp RB_T_Shadow):
    // Z-pass uses passOp only.
    //   non-mirror: back=INCR, front=DECR
    //   mirror:     back=DECR, front=INCR (swapped)
    // depthFailOp stays KEEP for both faces in Z-pass.
    VkStencilOpState front = {};
    front.failOp = VK_STENCIL_OP_KEEP;
    front.passOp = mirrorView ? VK_STENCIL_OP_INCREMENT_AND_WRAP : VK_STENCIL_OP_DECREMENT_AND_WRAP;
    front.depthFailOp = VK_STENCIL_OP_KEEP;
    front.compareOp = VK_COMPARE_OP_ALWAYS;
    front.compareMask = 0xFF;
    front.writeMask = 0xFF;
    front.reference = 0;

    VkStencilOpState back = front;
    back.passOp = mirrorView ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_INCREMENT_AND_WRAP;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; // Match GL shadow pass depth func (GL_LESS)
    depthStencil.stencilTestEnable = VK_TRUE;
    depthStencil.front = front;
    depthStencil.back = back;

    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.colorWriteMask = 0; // stencil-only

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// ---------------------------------------------------------------------------
// VK_CreateGlassReflDescLayout / VK_CreateGlassReflPipeline
// Additive overlay pipeline drawn once per MC_TRANSLUCENT surface to composite
// RT reflections on top of glass.
//   binding 0 — GuiParams UBO  (vertex MVP; screen dims in texGenS.xy for frag)
//   binding 1 — reflBuffer sampler2D
// ---------------------------------------------------------------------------

static VkDescriptorSetLayout VK_CreateGlassReflDescLayout(void)
{
    VkDescriptorSetLayoutBinding bindings[2] = {};

    // Binding 0: GuiParams UBO (reused — gui.vert.spv reads it for the MVP)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: RT reflection buffer
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = 2;
    info.pBindings = bindings;

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &layout));
    return layout;
}

static VkPipeline VK_CreateGlassReflPipeline(VkPipelineLayout layout)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/gui.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/glass_refl_overlay.frag.spv");
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.f, 1.f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE; // Y-flip viewport correction
    rasterizer.lineWidth = 1.0f;
    rasterizer.depthBiasEnable = VK_TRUE; // allow vkCmdSetDepthBias when sharing cmd stream

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test LEQUAL (translucent surfaces don't write depth), depth write OFF.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Additive blend: reflections are added on top of the glass colour.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                   VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 4;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// ---------------------------------------------------------------------------
// VK_CreateGuiDescLayout / VK_CreateGuiPipeline
// Simple textured pipeline for 2D GUI surfaces and unlit shader passes.
// ---------------------------------------------------------------------------

static VkDescriptorSetLayout VK_CreateGuiDescLayout(void)
{
    VkDescriptorSetLayoutBinding bindings[2] = {};

    // Binding 0: UBO (GuiParams: MVP matrix + color modulate/add)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: combined image sampler (diffuse texture)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = 2;
    info.pBindings = bindings;

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &layout));
    return layout;
}

// ---------------------------------------------------------------------------
// VK_CreateDepthPipeline - depth-only prepass
// Writes depth without touching colour; used before the interaction pass so the
// depth buffer is fully populated before expensive per-light shading runs.
// Reuses the GUI pipeline layout (binding 0 = GuiUBO with MVP).
// fragSpv selects the fragment shader:
//   "glprogs/glsl/gui.frag.spv"        — opaque (no texture sample)
//   "glprogs/glsl/depth_clip.frag.spv" — alpha-clip (MC_PERFORATED)
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateDepthPipelineEx(VkPipelineLayout layout, const char *fragSpv)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/gui.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV(fragSpv);
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.f, 1.f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    // Y-flip viewport inverts winding — same correction as interaction pipeline.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    // Enable depth bias so per-surface polygon offset can be applied dynamically
    // via vkCmdSetDepthBias for surfaces with MF_POLYGONOFFSET (decals, overlays).
    rasterizer.depthBiasEnable = VK_TRUE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth write enabled, LESS test, no stencil.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.stencilTestEnable = VK_FALSE;

    // No colour output — depth only.
    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.colorWriteMask = 0;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                   VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 4;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

static VkPipeline VK_CreateDepthPipeline(VkPipelineLayout layout)
{
    return VK_CreateDepthPipelineEx(layout, "glprogs/glsl/gui.frag.spv");
}

// ---------------------------------------------------------------------------
// GLS blend factor translation helpers
// ---------------------------------------------------------------------------

#include "renderer/tr_local.h"

static VkBlendFactor VK_GlsSrcBlendToVk(int stateBits)
{
    switch (stateBits & GLS_SRCBLEND_BITS)
    {
    case GLS_SRCBLEND_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    default:
    case GLS_SRCBLEND_ONE:
        return VK_BLEND_FACTOR_ONE;
    case GLS_SRCBLEND_DST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;
    case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GLS_SRCBLEND_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GLS_SRCBLEND_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GLS_SRCBLEND_ALPHA_SATURATE:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
}

static VkBlendFactor VK_GlsDstBlendToVk(int stateBits)
{
    switch (stateBits & GLS_DSTBLEND_BITS)
    {
    default:
    case GLS_DSTBLEND_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case GLS_DSTBLEND_ONE:
        return VK_BLEND_FACTOR_ONE;
    case GLS_DSTBLEND_SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GLS_DSTBLEND_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GLS_DSTBLEND_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

// ---------------------------------------------------------------------------
// GUI blend pipeline cache — built on demand, one entry per unique blend state
// ---------------------------------------------------------------------------

struct VkBlendPipelineEntry
{
    uint32_t blendKey; // (drawStateBits & (GLS_SRCBLEND_BITS|GLS_DSTBLEND_BITS))
    VkPipeline pipeline;
};

static const int MAX_BLEND_PIPELINES = 32;
static VkBlendPipelineEntry s_blendCache[MAX_BLEND_PIPELINES];
static int s_blendCacheCount = 0;

// Convert Doom3 inverse channel mask bits to Vulkan colorWriteMask bits.
// In Doom3, GLS_*MASK means "disable writes" for that channel.
// Important: stage drawState 0x00000f00 (breakyglass3_nvp stage 0) is alpha-only.
// If this mapping is skipped and RGBA is always enabled, the glass pass turns white.
static VkColorComponentFlags VK_GlsColorMaskToVk(int stateBits)
{
    VkColorComponentFlags mask = 0;
    if ((stateBits & GLS_REDMASK) == 0)
        mask |= VK_COLOR_COMPONENT_R_BIT;
    if ((stateBits & GLS_GREENMASK) == 0)
        mask |= VK_COLOR_COMPONENT_G_BIT;
    if ((stateBits & GLS_BLUEMASK) == 0)
        mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((stateBits & GLS_ALPHAMASK) == 0)
        mask |= VK_COLOR_COMPONENT_A_BIT;
    return mask;
}

// Core GUI pipeline builder — explicit blend control.
// blendEnable=false overrides all blend factor args (fully opaque).
static VkPipeline VK_CreateGuiPipelineEx(VkPipelineLayout layout, bool blendEnable, VkBlendFactor srcColor,
                                         VkBlendFactor dstColor, VkBlendFactor srcAlpha, VkBlendFactor dstAlpha,
                                         bool depthTest = false, VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                                         VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                                                VK_COLOR_COMPONENT_G_BIT |
                                                                                VK_COLOR_COMPONENT_B_BIT |
                                                                                VK_COLOR_COMPONENT_A_BIT)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/gui.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/gui.frag.spv");
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: same idDrawVert layout as interaction pipeline
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    if (depthTest)
    {
        // 3D world surface: back-face cull with the Y-flip winding convention
        // (same as the interaction and depth-prepass pipelines).
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        // Depth bias for MF_POLYGONOFFSET decals/overlays — mirrors GL qglPolygonOffset
        // in RB_STD_T_RenderShaderPasses.  Values are set dynamically per-draw.
        rasterizer.depthBiasEnable = VK_TRUE;
    }
    else
    {
        // 2D GUI overlay: no culling, counter-clockwise (screen-space quads)
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.stencilTestEnable = VK_FALSE;
    if (depthTest)
    {
        // 3D world surface: test against depth prepass, don't write depth.
        // depthOp comes from the material stage's drawStateBits:
        //   GLS_DEPTHFUNC_EQUAL  → EQUAL   (opaque/perforated, must match prepass exactly)
        //   GLS_DEPTHFUNC_LESS   → LEQUAL  (translucent, extends past prepass depth)
        //   GLS_DEPTHFUNC_ALWAYS → ALWAYS  (post-process or depth-ignored)
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = depthOp;
    }
    else
    {
        // 2D GUI overlay: depth test disabled, drawn in submission order.
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
    }

    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.colorWriteMask = colorWriteMask;
    if (blendEnable)
    {
        colorBlend.blendEnable = VK_TRUE;
        colorBlend.srcColorBlendFactor = srcColor;
        colorBlend.dstColorBlendFactor = dstColor;
        colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlend.srcAlphaBlendFactor = srcAlpha;
        colorBlend.dstAlphaBlendFactor = dstAlpha;
        colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    // 3D pipelines add DEPTH_BIAS so MF_POLYGONOFFSET decals can use vkCmdSetDepthBias,
    // matching GL's qglPolygonOffset call in RB_STD_T_RenderShaderPasses.
    // CULL_MODE is dynamic so shader passes can honor per-material cull type.
    VkDynamicState dynStates[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                   VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = depthTest ? 4 : 3;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// Thin wrappers for the two pre-built GUI pipelines.
static VkPipeline VK_CreateGuiPipeline(VkPipelineLayout layout, bool alphaBlend)
{
    if (!alphaBlend)
        return VK_CreateGuiPipelineEx(layout, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
                                      VK_BLEND_FACTOR_ZERO);
    return VK_CreateGuiPipelineEx(layout, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                  VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
}

// Dedicated skybox pipeline (TG_SKYBOX_CUBE): samplerCube + per-vertex direction.
// Uses the same descriptor layout as guiLayout:
//   binding 0 = UBO, binding 1 = combined image sampler.
static VkPipeline VK_CreateSkyboxPipeline(VkPipelineLayout layout)
{
    VkShaderModule vertModule = VK_LoadSPIRV("glprogs/glsl/skybox.vert.spv");
    VkShaderModule fragModule = VK_LoadSPIRV("glprogs/glsl/skybox.frag.spv");
    if (!vertModule || !fragModule)
    {
        common->Warning("VK: skybox shaders not found, skybox pipeline unavailable");
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.f, 1.f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_FALSE;
    colorBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE_EXT};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// Look up or create a GUI pipeline matching the given drawStateBits blend state.
// depthTest=true is used for 3D world surfaces (GLS_DEPTHFUNC_ALWAYS not set);
// depthTest=false for 2D GUI overlays (GLS_DEPTHFUNC_ALWAYS set, or explicit 2D call).
// The two pre-built pipelines (opaque / alpha) are for 2D only; 3D variants are cached separately.
VkPipeline VK_GetOrCreateGuiBlendPipeline(int drawStateBits, bool depthTest)
{
    const uint32_t blendBits = (uint32_t)(drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS));
    const VkColorComponentFlags colorMask = VK_GlsColorMaskToVk(drawStateBits);
    const uint32_t colorMaskNibble = (uint32_t)(colorMask & 0xFu);

    // Determine depth compare op from the stage's drawStateBits, matching GL_State() in gl_backend.cpp.
    // GLS_DEPTHFUNC_EQUAL (0x20000): opaque/perforated stages must match prepass exactly.
    // GLS_DEPTHFUNC_ALWAYS (0x10000): depth test disabled (depth-ignored stages).
    // Default (GLS_DEPTHFUNC_LESS = 0x0): translucent stages, use LEQUAL (extends past prepass).
    VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (drawStateBits & GLS_DEPTHFUNC_EQUAL)
        depthOp = VK_COMPARE_OP_EQUAL;
    else if (drawStateBits & GLS_DEPTHFUNC_ALWAYS)
        depthOp = VK_COMPARE_OP_ALWAYS;

    // Convert blend factors early for cache key and enable logic.
    // This must happen before cache key creation to avoid collisions when blendBits=0.
    VkBlendFactor src = VK_GlsSrcBlendToVk(drawStateBits);
    VkBlendFactor dst = VK_GlsDstBlendToVk(drawStateBits);

    // Build cache key including blend factors and color mask to avoid collisions.
    // Example: (ONE, ZERO) and (ZERO, ONE) both have blendBits=0 but need different pipelines.
    // Bits[31]: depthTest. Bits[29:28]: depthOp. Bits[27:24]: src factor.
    // Bits[23:20]: dst factor. Bits[19:16]: color write mask nibble.
    uint32_t depthOpIdx = (depthOp == VK_COMPARE_OP_EQUAL) ? 1u : (depthOp == VK_COMPARE_OP_ALWAYS) ? 2u : 0u;
    const uint32_t srcIdx = (uint32_t)src & 0xFu;
    const uint32_t dstIdx = (uint32_t)dst & 0xFu;
    const uint32_t key = blendBits | (depthTest ? 0x80000000u : 0u) | (depthOpIdx << 28) | (srcIdx << 24) |
                         (dstIdx << 20) | (colorMaskNibble << 16);

    // Log GUI blend pipeline creation only at verbose translucent logging level.
    if (r_vkLogTranslucent.GetInteger() >= 2)
    {
        common->Printf("VK GuiBlendPipeline: drawState=0x%08x blendBits=0x%x depthTest=%d depthOp=%d "
                       "src=%d dst=%d colorMask=0x%x (ZERO=0,ONE=1,ALPHA=4,DST_ALPHA=9,...)\n",
                       drawStateBits, blendBits, depthTest ? 1 : 0, depthOp, (int)src, (int)dst,
                       (unsigned int)colorMask);
    }

    // Fast path: pre-built 2D pipelines (no depth test, LEQUAL default)
    // Verify blend/factors/mask all match to avoid returning wrong pipeline
    if (!depthTest && depthOpIdx == 0u)
    {
        const VkColorComponentFlags fullMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        if (blendBits == 0u && src == VK_BLEND_FACTOR_ONE && dst == VK_BLEND_FACTOR_ZERO && colorMask == fullMask)
            return vkPipes.guiOpaquePipeline;
        const uint32_t alphaKey = (uint32_t)(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
        if (blendBits == alphaKey && src == VK_BLEND_FACTOR_SRC_ALPHA && dst == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
            colorMask == fullMask)
            return vkPipes.guiAlphaPipeline;
    }

    // Search cache
    for (int i = 0; i < s_blendCacheCount; i++)
    {
        if (s_blendCache[i].blendKey == key)
            return s_blendCache[i].pipeline;
    }

    // Not found — build a new pipeline
    if (s_blendCacheCount >= MAX_BLEND_PIPELINES)
    {
        common->Warning("VK: blend pipeline cache full, falling back to alpha pipeline");
        return vkPipes.guiAlphaPipeline;
    }

    // Determine if blending should be enabled.  Even if blendBits==0, we still enable blending
    // if the converted factors are valid (non-default). A (ONE, ZERO) blend is valid and must
    // be enabled, not treated as an opaque surface.
    bool blend = (src != VK_BLEND_FACTOR_ONE || dst != VK_BLEND_FACTOR_ZERO);

    VkPipeline p = VK_CreateGuiPipelineEx(vkPipes.guiLayout, blend, src, dst, src, dst, depthTest, depthOp, colorMask);
    if (p == VK_NULL_HANDLE)
        return vkPipes.guiAlphaPipeline;

    s_blendCache[s_blendCacheCount++] = {key, p};
    return p;
}

// Destroy all dynamically-built blend pipelines (the two pre-built ones are
// destroyed separately in VK_ShutdownPipelines).
void VK_DestroyBlendPipelineCache(void)
{
    for (int i = 0; i < s_blendCacheCount; i++)
        vkDestroyPipeline(vk.device, s_blendCache[i].pipeline, NULL);
    s_blendCacheCount = 0;
}

// ---------------------------------------------------------------------------
// Fog / blend-light pipelines
// Shared descriptor layout: binding0=UBO(vert+frag), binding1=samp(frag), binding2=samp(frag)
// Used by VK_RB_FogAllLights for both fog lights and blend lights.
// ---------------------------------------------------------------------------

static VkDescriptorSetLayout VK_CreateFogDescLayout(void)
{
    VkDescriptorSetLayoutBinding bindings[3] = {};
    // binding 0: UBO used by both vertex and fragment
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 1: samp0 (fog/light image, frag only)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 2: samp1 (fogEnter/falloff image, frag only)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = 3;
    info.pBindings = bindings;

    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(vk.device, &info, NULL, &layout));
    return layout;
}

// Core fog pipeline builder.
// vertSpv/fragSpv: SPIR-V paths for the fog or blendlight shaders.
// depthOp: VK_COMPARE_OP_EQUAL (world surfaces) or VK_COMPARE_OP_LESS (frustum cap).
// cullMode: VK_CULL_MODE_FRONT_BIT (world surfaces) or VK_CULL_MODE_BACK_BIT (frustum cap).
// blendSrc/blendDst: colour blend factors.
static VkPipeline VK_CreateFogPipelineEx(VkPipelineLayout layout, const char *vertSpv, const char *fragSpv,
                                         VkCompareOp depthOp, VkCullModeFlags cullMode, VkBlendFactor blendSrc,
                                         VkBlendFactor blendDst)
{
    VkShaderModule vertModule = VK_LoadSPIRV(vertSpv);
    VkShaderModule fragModule = VK_LoadSPIRV(fragSpv);
    if (!vertModule || !fragModule)
    {
        if (vertModule)
            vkDestroyShaderModule(vk.device, vertModule, NULL);
        if (fragModule)
            vkDestroyShaderModule(vk.device, fragModule, NULL);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // Vertex input: position only from idDrawVert (location 0, offset 0, stride sizeof(idDrawVert))
    // Using VK_GetInteractionVertexInput which provides the full idDrawVert layout;
    // the shader only reads location 0 (position), extra attrs are harmlessly present.
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[12];
    uint32_t numAttrs = 0;
    VK_GetInteractionVertexInput(&binding, attrs, &numAttrs);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = numAttrs;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.f, 1.f};
    VkRect2D scissor = {{0, 0}, vk.swapchainExtent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = cullMode;
    // Y-flip viewport inverts winding — CW here matches front-facing world geometry.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test at requested compare op; no depth write; no stencil.
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = depthOp;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlend = {};
    colorBlend.blendEnable = VK_TRUE;
    colorBlend.srcColorBlendFactor = blendSrc;
    colorBlend.dstColorBlendFactor = blendDst;
    colorBlend.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlend.srcAlphaBlendFactor = blendSrc;
    colorBlend.dstAlphaBlendFactor = blendDst;
    colorBlend.alphaBlendOp = VK_BLEND_OP_ADD;
    colorBlend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState = {};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &colorBlend;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = vk.renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

    vkDestroyShaderModule(vk.device, vertModule, NULL);
    vkDestroyShaderModule(vk.device, fragModule, NULL);
    return pipeline;
}

// Small pipeline cache for blend-light variants (different blend modes per material stage).
struct VkBlendlightPipelineEntry
{
    uint32_t blendKey; // (drawStateBits & (GLS_SRCBLEND_BITS|GLS_DSTBLEND_BITS))
    VkPipeline pipeline;
};
static const int MAX_BLENDLIGHT_PIPELINES = 16;
static VkBlendlightPipelineEntry s_blendlightCache[MAX_BLENDLIGHT_PIPELINES];
static int s_blendlightCacheCount = 0;

// Return (creating on demand) a blendlight pipeline for the given blend state bits.
// All variants share depth EQUAL, no depth write, blendlight.vert/frag shaders.
VkPipeline VK_GetOrCreateBlendlightPipeline(int drawStateBits)
{
    // Pre-built default: modulate (DST_COLOR / ZERO) — the most common blend light in D3
    const uint32_t defaultKey = (uint32_t)(GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
    const uint32_t key = (uint32_t)(drawStateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS));

    if (key == defaultKey || key == 0u)
        return vkPipes.blendlightPipeline;

    for (int i = 0; i < s_blendlightCacheCount; i++)
        if (s_blendlightCache[i].blendKey == key)
            return s_blendlightCache[i].pipeline;

    if (s_blendlightCacheCount >= MAX_BLENDLIGHT_PIPELINES)
    {
        common->Warning("VK: blendlight pipeline cache full, falling back to default");
        return vkPipes.blendlightPipeline;
    }

    VkBlendFactor src = VK_GlsSrcBlendToVk(drawStateBits);
    VkBlendFactor dst = VK_GlsDstBlendToVk(drawStateBits);
    VkPipeline p = VK_CreateFogPipelineEx(vkPipes.fogLayout, "glprogs/glsl/blendlight.vert.spv",
                                          "glprogs/glsl/blendlight.frag.spv", VK_COMPARE_OP_EQUAL,
                                          VK_CULL_MODE_FRONT_BIT, src, dst);
    if (p == VK_NULL_HANDLE)
        return vkPipes.blendlightPipeline;

    s_blendlightCache[s_blendlightCacheCount++] = {key, p};
    return p;
}

void VK_DestroyBlendlightPipelineCache(void)
{
    for (int i = 0; i < s_blendlightCacheCount; i++)
        vkDestroyPipeline(vk.device, s_blendlightCache[i].pipeline, NULL);
    s_blendlightCacheCount = 0;
}

// ---------------------------------------------------------------------------
// VK_InitPipelines - create all pipelines (called after swapchain is ready)
// ---------------------------------------------------------------------------

void VK_InitPipelines(void)
{
    memset(&vkPipes, 0, sizeof(vkPipes));

    common->Printf("VK BUILD SIGNATURE: %s\n", VK_BUILD_SIGNATURE);
    fflush(NULL);

    // --- Interaction pipeline ---
    vkPipes.interactionDescLayout = VK_CreateInteractionDescLayout();
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vkPipes.interactionDescLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &vkPipes.interactionLayout));
    }
    // Opaque interactions: EQUAL matches GL (backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL).
    // Opaque weapon-depthhack interactions: LEQUAL to tolerate depth interpolation
    // mismatch between depth prepass and interaction shaders in Vulkan.
    // If that still rejects valid fragments, a stencil+ALWAYS variant is available
    // as a targeted fallback for weapon/viewmodel surfaces.
    // Translucent interactions: LEQUAL since translucent surfaces skip the depth prepass.
    vkPipes.interactionPipeline = VK_CreateInteractionPipeline(vkPipes.interactionLayout, true, VK_COMPARE_OP_EQUAL);
    vkPipes.interactionPipelineStencilLEqual =
        VK_CreateInteractionPipeline(vkPipes.interactionLayout, true, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkPipes.interactionPipelineStencilAlways =
        VK_CreateInteractionPipeline(vkPipes.interactionLayout, true, VK_COMPARE_OP_ALWAYS);
    vkPipes.interactionPipelineNoStencil =
        VK_CreateInteractionPipeline(vkPipes.interactionLayout, false, VK_COMPARE_OP_LESS_OR_EQUAL);

    // --- Shadow pipeline ---
    vkPipes.shadowDescLayout = VK_CreateShadowDescLayout();
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vkPipes.shadowDescLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &vkPipes.shadowLayout));
    }
    vkPipes.shadowPipelineZFail = VK_CreateShadowPipelineZFail(vkPipes.shadowLayout, false);      // normal view
    vkPipes.shadowPipelineZFailMirror = VK_CreateShadowPipelineZFail(vkPipes.shadowLayout, true); // mirrored view
    vkPipes.shadowPipelineZPass = VK_CreateShadowPipelineZPass(vkPipes.shadowLayout, false);      // normal view
    vkPipes.shadowPipelineZPassMirror = VK_CreateShadowPipelineZPass(vkPipes.shadowLayout, true); // mirrored view

    // --- Depth prepass pipeline (reuses guiLayout; no separate desc layout needed) ---
    // Must be created after guiLayout is ready (below), so we defer to after GUI init.

    // --- GUI pipeline ---
    vkPipes.guiDescLayout = VK_CreateGuiDescLayout();
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vkPipes.guiDescLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &vkPipes.guiLayout));
    }
    vkPipes.guiOpaquePipeline = VK_CreateGuiPipeline(vkPipes.guiLayout, false);
    vkPipes.guiAlphaPipeline = VK_CreateGuiPipeline(vkPipes.guiLayout, true);
    vkPipes.skyboxPipeline = VK_CreateSkyboxPipeline(vkPipes.guiLayout);

    // --- Depth prepass pipelines (created after guiLayout is ready) ---
    vkPipes.depthPipeline = VK_CreateDepthPipeline(vkPipes.guiLayout);
    vkPipes.depthClipPipeline = VK_CreateDepthPipelineEx(vkPipes.guiLayout, "glprogs/glsl/depth_clip.frag.spv");

    // --- Glass RT-reflection overlay pipeline ---
    vkPipes.glassReflDescLayout = VK_CreateGlassReflDescLayout();
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vkPipes.glassReflDescLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &vkPipes.glassReflLayout));
    }
    vkPipes.glassReflPipeline = VK_CreateGlassReflPipeline(vkPipes.glassReflLayout);

    // --- Fog / blend-light pipelines ---
    vkPipes.fogDescLayout = VK_CreateFogDescLayout();
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vkPipes.fogDescLayout;
        VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, NULL, &vkPipes.fogLayout));
    }
    // Fog surface pass: depth EQUAL, front-cull, SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    vkPipes.fogPipeline = VK_CreateFogPipelineEx(
        vkPipes.fogLayout, "glprogs/glsl/fog.vert.spv", "glprogs/glsl/fog.frag.spv", VK_COMPARE_OP_EQUAL,
        VK_CULL_MODE_FRONT_BIT, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    // Fog frustum cap pass: depth LESS, back-cull, SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    vkPipes.fogFrustumPipeline = VK_CreateFogPipelineEx(
        vkPipes.fogLayout, "glprogs/glsl/fog.vert.spv", "glprogs/glsl/fog.frag.spv", VK_COMPARE_OP_LESS,
        VK_CULL_MODE_BACK_BIT, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    // Blend light default: depth EQUAL, front-cull, DST_COLOR / ZERO (modulate)
    vkPipes.blendlightPipeline = VK_CreateFogPipelineEx(
        vkPipes.fogLayout, "glprogs/glsl/blendlight.vert.spv", "glprogs/glsl/blendlight.frag.spv", VK_COMPARE_OP_EQUAL,
        VK_CULL_MODE_FRONT_BIT, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO);

    // Load push descriptor function pointer (KHR extension, not in static lib).
    pfn_vkCmdPushDescriptorSetKHR =
        (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(vk.device, "vkCmdPushDescriptorSetKHR");
    if (!pfn_vkCmdPushDescriptorSetKHR)
        common->FatalError(
            "VK: vkCmdPushDescriptorSetKHR not available — driver does not support VK_KHR_push_descriptor");

    vkPipes.isValid =
        (vkPipes.interactionPipeline != VK_NULL_HANDLE && vkPipes.shadowPipelineZFail != VK_NULL_HANDLE &&
         vkPipes.shadowPipelineZFailMirror != VK_NULL_HANDLE && vkPipes.shadowPipelineZPass != VK_NULL_HANDLE &&
         vkPipes.shadowPipelineZPassMirror != VK_NULL_HANDLE);

    common->Printf("VK: Pipelines initialized (interaction=%s, shadow-zfail=%s/%s, shadow-zpass=%s/%s, depth=%s, "
                   "gui=%s/%s, skybox=%s)\n",
                   vkPipes.interactionPipeline ? "OK" : "FAIL", vkPipes.shadowPipelineZFail ? "OK" : "FAIL",
                   vkPipes.shadowPipelineZFailMirror ? "OK" : "FAIL", vkPipes.shadowPipelineZPass ? "OK" : "FAIL",
                   vkPipes.shadowPipelineZPassMirror ? "OK" : "FAIL", vkPipes.depthPipeline ? "OK" : "FAIL",
                   vkPipes.guiOpaquePipeline ? "OK" : "FAIL", vkPipes.guiAlphaPipeline ? "OK" : "FAIL",
                   vkPipes.skyboxPipeline ? "OK" : "FAIL");
}

// ---------------------------------------------------------------------------
// VK_ShutdownPipelines
// ---------------------------------------------------------------------------

void VK_ShutdownPipelines(void)
{
    VK_DestroyBlendPipelineCache();
    VK_DestroyBlendlightPipelineCache();

    if (vkPipes.interactionPipeline)
        vkDestroyPipeline(vk.device, vkPipes.interactionPipeline, NULL);
    if (vkPipes.interactionPipelineStencilLEqual)
        vkDestroyPipeline(vk.device, vkPipes.interactionPipelineStencilLEqual, NULL);
    if (vkPipes.interactionPipelineStencilAlways)
        vkDestroyPipeline(vk.device, vkPipes.interactionPipelineStencilAlways, NULL);
    if (vkPipes.interactionPipelineNoStencil)
        vkDestroyPipeline(vk.device, vkPipes.interactionPipelineNoStencil, NULL);
    if (vkPipes.interactionLayout)
        vkDestroyPipelineLayout(vk.device, vkPipes.interactionLayout, NULL);
    if (vkPipes.interactionDescLayout)
        vkDestroyDescriptorSetLayout(vk.device, vkPipes.interactionDescLayout, NULL);
    if (vkPipes.shadowPipelineZFail)
        vkDestroyPipeline(vk.device, vkPipes.shadowPipelineZFail, NULL);
    if (vkPipes.shadowPipelineZFailMirror)
        vkDestroyPipeline(vk.device, vkPipes.shadowPipelineZFailMirror, NULL);
    if (vkPipes.shadowPipelineZPass)
        vkDestroyPipeline(vk.device, vkPipes.shadowPipelineZPass, NULL);
    if (vkPipes.shadowPipelineZPassMirror)
        vkDestroyPipeline(vk.device, vkPipes.shadowPipelineZPassMirror, NULL);
    if (vkPipes.shadowLayout)
        vkDestroyPipelineLayout(vk.device, vkPipes.shadowLayout, NULL);
    if (vkPipes.shadowDescLayout)
        vkDestroyDescriptorSetLayout(vk.device, vkPipes.shadowDescLayout, NULL);
    if (vkPipes.depthPipeline)
        vkDestroyPipeline(vk.device, vkPipes.depthPipeline, NULL);
    if (vkPipes.depthClipPipeline)
        vkDestroyPipeline(vk.device, vkPipes.depthClipPipeline, NULL);
    if (vkPipes.guiOpaquePipeline)
        vkDestroyPipeline(vk.device, vkPipes.guiOpaquePipeline, NULL);
    if (vkPipes.guiAlphaPipeline)
        vkDestroyPipeline(vk.device, vkPipes.guiAlphaPipeline, NULL);
    if (vkPipes.skyboxPipeline)
        vkDestroyPipeline(vk.device, vkPipes.skyboxPipeline, NULL);
    if (vkPipes.guiLayout)
        vkDestroyPipelineLayout(vk.device, vkPipes.guiLayout, NULL);
    if (vkPipes.guiDescLayout)
        vkDestroyDescriptorSetLayout(vk.device, vkPipes.guiDescLayout, NULL);
    if (vkPipes.glassReflPipeline)
        vkDestroyPipeline(vk.device, vkPipes.glassReflPipeline, NULL);
    if (vkPipes.glassReflLayout)
        vkDestroyPipelineLayout(vk.device, vkPipes.glassReflLayout, NULL);
    if (vkPipes.glassReflDescLayout)
        vkDestroyDescriptorSetLayout(vk.device, vkPipes.glassReflDescLayout, NULL);
    if (vkPipes.fogPipeline)
        vkDestroyPipeline(vk.device, vkPipes.fogPipeline, NULL);
    if (vkPipes.fogFrustumPipeline)
        vkDestroyPipeline(vk.device, vkPipes.fogFrustumPipeline, NULL);
    if (vkPipes.blendlightPipeline)
        vkDestroyPipeline(vk.device, vkPipes.blendlightPipeline, NULL);
    if (vkPipes.fogLayout)
        vkDestroyPipelineLayout(vk.device, vkPipes.fogLayout, NULL);
    if (vkPipes.fogDescLayout)
        vkDestroyDescriptorSetLayout(vk.device, vkPipes.fogDescLayout, NULL);
    memset(&vkPipes, 0, sizeof(vkPipes));
}
