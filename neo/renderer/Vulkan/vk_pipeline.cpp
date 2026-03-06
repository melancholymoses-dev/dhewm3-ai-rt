/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - graphics pipeline and descriptor set management.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_shader.cpp"   // inline for now

// ---------------------------------------------------------------------------
// Per-interaction uniform block (matches draw_glsl.cpp uniforms)
// Uploaded as a push constant or small UBO per draw call.
// ---------------------------------------------------------------------------

struct VkInteractionUBO {
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
	int   applyGamma;
	float screenWidth;          // framebuffer width  (for shadow mask UV)
	float screenHeight;         // framebuffer height (for shadow mask UV)
	int   useShadowMask;        // 1 when RT shadow mask is valid this frame
	float _pad;
};
// Total: 14*16 + 64 + 3*16 + 4 + 8 + 4 + 4 = 368 bytes; INTERACTION_UBO_SIZE = 384.

// ---------------------------------------------------------------------------
// Pipeline layout for the interaction pass
// Binding layout:
//   set 0 binding 0: UBO (VkInteractionUBO)
//   set 0 binding 1..6: combined image samplers (bump, falloff, proj, diffuse, specular, specTable)
//   set 0 binding 7:    shadow mask sampler (RT output or 1x1 white fallback)
// ---------------------------------------------------------------------------

struct vkPipelines_t {
	VkDescriptorSetLayout   interactionDescLayout;
	VkPipelineLayout        interactionLayout;
	VkPipeline              interactionPipeline;

	VkDescriptorSetLayout   shadowDescLayout;
	VkPipelineLayout        shadowLayout;
	VkPipeline              shadowPipeline;

	VkDescriptorSetLayout   depthDescLayout;
	VkPipelineLayout        depthLayout;
	VkPipeline              depthPipeline;

	VkDescriptorPool        descPool;

	bool isValid;
};

vkPipelines_t vkPipes;

// ---------------------------------------------------------------------------
// Descriptor set layout helpers
// ---------------------------------------------------------------------------

static VkDescriptorSetLayout VK_CreateInteractionDescLayout( void ) {
	VkDescriptorSetLayoutBinding bindings[8] = {};

	// Binding 0: UBO
	bindings[0].binding            = 0;
	bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[0].descriptorCount    = 1;
	bindings[0].stageFlags         = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	// Bindings 1-6: samplers (bump, falloff, lightProj, diffuse, specular, specTable)
	for ( int i = 1; i <= 6; i++ ) {
		bindings[i].binding         = (uint32_t)i;
		bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	// Binding 7: shadow mask (RT shadow output, or 1x1 white fallback when RT is off)
	bindings[7].binding         = 7;
	bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo info = {};
	info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 8;
	info.pBindings    = bindings;

	VkDescriptorSetLayout layout;
	VK_CHECK( vkCreateDescriptorSetLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

static VkDescriptorSetLayout VK_CreateShadowDescLayout( void ) {
	// Shadow pass only needs a UBO with MVP + light origin
	VkDescriptorSetLayoutBinding binding = {};
	binding.binding         = 0;
	binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutCreateInfo info = {};
	info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = 1;
	info.pBindings    = &binding;

	VkDescriptorSetLayout layout;
	VK_CHECK( vkCreateDescriptorSetLayout( vk.device, &info, NULL, &layout ) );
	return layout;
}

// ---------------------------------------------------------------------------
// Vertex input description matching idDrawVert
// ---------------------------------------------------------------------------

static void VK_GetInteractionVertexInput(
	VkVertexInputBindingDescription *binding,
	VkVertexInputAttributeDescription *attrs,
	uint32_t *numAttrs )
{
	// One binding: interleaved idDrawVert buffer
	binding->binding   = 0;
	binding->stride    = sizeof( idDrawVert );
	binding->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	// Match the GLSL attribute locations in draw_glsl.cpp
	uint32_t i = 0;

	// location=0: position (vec3)
	attrs[i] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(idDrawVert, xyz) };       i++;
	// location=3: color (rgba ubyte, normalized)
	attrs[i] = { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      offsetof(idDrawVert, color) };     i++;
	// location=8: texcoord (vec2)
	attrs[i] = { 8, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(idDrawVert, st) };        i++;
	// location=9: tangent[0] (vec3)
	attrs[i] = { 9, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(idDrawVert, tangents[0]) }; i++;
	// location=10: tangent[1] / bitangent (vec3)
	attrs[i] = { 10, 0, VK_FORMAT_R32G32B32_SFLOAT,   offsetof(idDrawVert, tangents[1]) }; i++;
	// location=11: normal (vec3)
	attrs[i] = { 11, 0, VK_FORMAT_R32G32B32_SFLOAT,   offsetof(idDrawVert, normal) };    i++;

	*numAttrs = i;
}

// ---------------------------------------------------------------------------
// VK_CreateInteractionPipeline
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateInteractionPipeline( VkPipelineLayout layout ) {
	// Load SPIR-V shaders compiled from the GLSL files
	VkShaderModule vertModule = VK_LoadSPIRV( "glprogs/glsl/interaction.vert.spv" );
	VkShaderModule fragModule = VK_LoadSPIRV( "glprogs/glsl/interaction.frag.spv" );
	if ( vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE ) {
		common->Warning( "VK: interaction shaders not found, Vulkan interaction pipeline unavailable" );
		if ( vertModule ) vkDestroyShaderModule( vk.device, vertModule, NULL );
		if ( fragModule ) vkDestroyShaderModule( vk.device, fragModule, NULL );
		return VK_NULL_HANDLE;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName  = "main";

	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName  = "main";

	// Vertex input
	VkVertexInputBindingDescription   binding;
	VkVertexInputAttributeDescription attrs[12];
	uint32_t numAttrs = 0;
	VK_GetInteractionVertexInput( &binding, attrs, &numAttrs );

	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount   = 1;
	vertexInput.pVertexBindingDescriptions      = &binding;
	vertexInput.vertexAttributeDescriptionCount = numAttrs;
	vertexInput.pVertexAttributeDescriptions    = attrs;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkViewport viewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f };
	VkRect2D   scissor  = { {0,0}, vk.swapchainExtent };

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports    = &viewport;
	viewportState.scissorCount  = 1;
	viewportState.pScissors     = &scissor;

	VkPipelineRasterizationStateCreateInfo rasterizer = {};
	rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.lineWidth   = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling = {};
	multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Depth+stencil: EQUAL depth test (for interaction pass), stencil test reads shadow mask
	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_FALSE;   // don't write depth in interaction pass
	depthStencil.depthCompareOp   = VK_COMPARE_OP_EQUAL;
	depthStencil.stencilTestEnable = VK_TRUE;
	depthStencil.front.compareOp  = VK_COMPARE_OP_NOT_EQUAL;
	depthStencil.front.compareMask = 255;
	depthStencil.front.reference  = 128;
	depthStencil.back             = depthStencil.front;

	// Blending: additive (ONE + ONE) for light accumulation
	VkPipelineColorBlendAttachmentState colorBlend = {};
	colorBlend.blendEnable         = VK_TRUE;
	colorBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlend.colorBlendOp        = VK_BLEND_OP_ADD;
	colorBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorBlend.alphaBlendOp        = VK_BLEND_OP_ADD;
	colorBlend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo blendState = {};
	blendState.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blendState.attachmentCount   = 1;
	blendState.pAttachments      = &colorBlend;

	// Dynamic state: viewport and scissor so we can resize
	VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates    = dynStates;

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount          = 2;
	pipelineInfo.pStages             = stages;
	pipelineInfo.pVertexInputState   = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState      = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState   = &multisampling;
	pipelineInfo.pDepthStencilState  = &depthStencil;
	pipelineInfo.pColorBlendState    = &blendState;
	pipelineInfo.pDynamicState       = &dynamicState;
	pipelineInfo.layout              = layout;
	pipelineInfo.renderPass          = vk.renderPass;
	pipelineInfo.subpass             = 0;

	VkPipeline pipeline;
	VK_CHECK( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline ) );

	vkDestroyShaderModule( vk.device, vertModule, NULL );
	vkDestroyShaderModule( vk.device, fragModule, NULL );

	return pipeline;
}

// ---------------------------------------------------------------------------
// VK_CreateShadowPipeline - stencil shadow volume pipeline
// ---------------------------------------------------------------------------

static VkPipeline VK_CreateShadowPipeline( VkPipelineLayout layout ) {
	VkShaderModule vertModule = VK_LoadSPIRV( "glprogs/glsl/shadow.vert.spv" );
	VkShaderModule fragModule = VK_LoadSPIRV( "glprogs/glsl/shadow.frag.spv" );
	if ( !vertModule || !fragModule ) {
		if ( vertModule ) vkDestroyShaderModule( vk.device, vertModule, NULL );
		if ( fragModule ) vkDestroyShaderModule( vk.device, fragModule, NULL );
		return VK_NULL_HANDLE;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,   vertModule, "main", NULL };
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragModule, "main", NULL };

	// Shadow vertex: only position (vec4 with w=0 or 1 for extrusion)
	VkVertexInputBindingDescription binding = { 0, sizeof(float)*4, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attr   = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };

	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount   = 1;
	vertexInput.pVertexBindingDescriptions      = &binding;
	vertexInput.vertexAttributeDescriptionCount = 1;
	vertexInput.pVertexAttributeDescriptions    = &attr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkViewport viewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0, 1 };
	VkRect2D   scissor  = { {0,0}, vk.swapchainExtent };

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports    = &viewport;
	viewportState.scissorCount  = 1;
	viewportState.pScissors     = &scissor;

	// Shadow volumes: no culling (two-sided), depth clamp for infinite extrusion
	VkPipelineRasterizationStateCreateInfo rasterizer = {};
	rasterizer.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.polygonMode      = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode         = VK_CULL_MODE_NONE;
	rasterizer.frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthClampEnable = VK_TRUE;    // needed for infinite projection
	rasterizer.lineWidth        = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling = {};
	multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Depth-fail stencil shadow (Carmack's Reverse)
	VkStencilOpState front = {};
	front.failOp      = VK_STENCIL_OP_KEEP;
	front.passOp      = VK_STENCIL_OP_KEEP;
	front.depthFailOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
	front.compareOp   = VK_COMPARE_OP_ALWAYS;
	front.compareMask = 0xFF;
	front.writeMask   = 0xFF;
	front.reference   = 0;

	VkStencilOpState back = front;
	back.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType              = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable    = VK_TRUE;
	depthStencil.depthWriteEnable   = VK_FALSE;
	depthStencil.depthCompareOp     = VK_COMPARE_OP_LESS;
	depthStencil.stencilTestEnable  = VK_TRUE;
	depthStencil.front              = front;
	depthStencil.back               = back;

	// Color write disabled for shadow pass (stencil-only)
	VkPipelineColorBlendAttachmentState colorBlend = {};
	colorBlend.colorWriteMask = 0;

	VkPipelineColorBlendStateCreateInfo blendState = {};
	blendState.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blendState.attachmentCount = 1;
	blendState.pAttachments    = &colorBlend;

	VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates    = dynStates;

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount          = 2;
	pipelineInfo.pStages             = stages;
	pipelineInfo.pVertexInputState   = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState      = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState   = &multisampling;
	pipelineInfo.pDepthStencilState  = &depthStencil;
	pipelineInfo.pColorBlendState    = &blendState;
	pipelineInfo.pDynamicState       = &dynamicState;
	pipelineInfo.layout              = layout;
	pipelineInfo.renderPass          = vk.renderPass;

	VkPipeline pipeline;
	VK_CHECK( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline ) );

	vkDestroyShaderModule( vk.device, vertModule, NULL );
	vkDestroyShaderModule( vk.device, fragModule, NULL );
	return pipeline;
}

// ---------------------------------------------------------------------------
// VK_InitPipelines - create all pipelines (called after swapchain is ready)
// ---------------------------------------------------------------------------

void VK_InitPipelines( void ) {
	memset( &vkPipes, 0, sizeof(vkPipes) );

	// --- Interaction pipeline ---
	vkPipes.interactionDescLayout = VK_CreateInteractionDescLayout();
	{
		VkPipelineLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts    = &vkPipes.interactionDescLayout;
		VK_CHECK( vkCreatePipelineLayout( vk.device, &layoutInfo, NULL, &vkPipes.interactionLayout ) );
	}
	vkPipes.interactionPipeline = VK_CreateInteractionPipeline( vkPipes.interactionLayout );

	// --- Shadow pipeline ---
	vkPipes.shadowDescLayout = VK_CreateShadowDescLayout();
	{
		VkPipelineLayoutCreateInfo layoutInfo = {};
		layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts    = &vkPipes.shadowDescLayout;
		VK_CHECK( vkCreatePipelineLayout( vk.device, &layoutInfo, NULL, &vkPipes.shadowLayout ) );
	}
	vkPipes.shadowPipeline = VK_CreateShadowPipeline( vkPipes.shadowLayout );

	// --- Descriptor pool ---
	// Sizes: UBOs + samplers across many draw calls per frame
	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = 4096;
	poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = 4096 * 7;  // 6 material textures + 1 shadow mask per draw

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = 4096;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes    = poolSizes;
	poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

	VK_CHECK( vkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vkPipes.descPool ) );

	vkPipes.isValid = ( vkPipes.interactionPipeline != VK_NULL_HANDLE &&
	                    vkPipes.shadowPipeline      != VK_NULL_HANDLE );

	common->Printf( "VK: Pipelines initialized (interaction=%s, shadow=%s)\n",
	                vkPipes.interactionPipeline ? "OK" : "FAIL",
	                vkPipes.shadowPipeline      ? "OK" : "FAIL" );
}

// ---------------------------------------------------------------------------
// VK_ShutdownPipelines
// ---------------------------------------------------------------------------

void VK_ShutdownPipelines( void ) {
	if ( vkPipes.descPool )              vkDestroyDescriptorPool(     vk.device, vkPipes.descPool,              NULL );
	if ( vkPipes.interactionPipeline )   vkDestroyPipeline(           vk.device, vkPipes.interactionPipeline,   NULL );
	if ( vkPipes.interactionLayout )     vkDestroyPipelineLayout(     vk.device, vkPipes.interactionLayout,     NULL );
	if ( vkPipes.interactionDescLayout ) vkDestroyDescriptorSetLayout(vk.device, vkPipes.interactionDescLayout, NULL );
	if ( vkPipes.shadowPipeline )        vkDestroyPipeline(           vk.device, vkPipes.shadowPipeline,        NULL );
	if ( vkPipes.shadowLayout )          vkDestroyPipelineLayout(     vk.device, vkPipes.shadowLayout,          NULL );
	if ( vkPipes.shadowDescLayout )      vkDestroyDescriptorSetLayout(vk.device, vkPipes.shadowDescLayout,      NULL );
	memset( &vkPipes, 0, sizeof(vkPipes) );
}
