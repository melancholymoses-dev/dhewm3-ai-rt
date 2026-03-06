/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - main render loop.

Mirrors the structure of tr_backend.cpp / draw_common.cpp for the Vulkan path.
Called from RB_ExecuteBackEndCommands() when r_backend == "vulkan".

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

#include "sys/platform.h"
#include "renderer/VertexCache.h"
#include "renderer/tr_local.h"
#include "renderer/Vulkan/vk_common.h"
#include "renderer/Vulkan/vk_raytracing.h"

// Forward declarations (defined in vk_pipeline.cpp)
struct vkPipelines_t;
extern vkPipelines_t vkPipes;
void VK_InitPipelines( void );
void VK_ShutdownPipelines( void );

// Forward declarations (defined in vk_swapchain.cpp)
void VK_CreateSwapchain( int width, int height );
void VK_DestroySwapchain( void );

// ---------------------------------------------------------------------------
// Per-frame uniform buffer ring
// Pre-allocated pool of UBO memory for interaction parameters.
// ---------------------------------------------------------------------------

static const uint32_t VK_UBO_RING_SIZE = 4096;  // max interactions per frame

struct vkUBORing_t {
	VkBuffer       buffer;
	VkDeviceMemory memory;
	void          *mapped;
	uint32_t       offset;  // current allocation offset in bytes
	uint32_t       stride;  // aligned stride per UBO entry
};

static vkUBORing_t uboRings[VK_MAX_FRAMES_IN_FLIGHT];

// Interaction UBO size (must match VkInteractionUBO in vk_pipeline.cpp).
// Struct breakdown: 14 vec4s (224) + MVP mat4 (64) + 3 vec4s (48) + applyGamma/pad (16)
// + screenSize vec2 + useShadowMask int + pad = 356 bytes -> round to 384.
static const uint32_t INTERACTION_UBO_SIZE = 384;

static void VK_CreateUBORings( void ) {
	VkPhysicalDeviceProperties devProps;
	vkGetPhysicalDeviceProperties( vk.physicalDevice, &devProps );
	uint32_t align = (uint32_t)devProps.limits.minUniformBufferOffsetAlignment;

	// Round INTERACTION_UBO_SIZE up to required alignment
	uint32_t stride = ( INTERACTION_UBO_SIZE + align - 1 ) & ~(align - 1);

	for ( int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++ ) {
		VkDeviceSize size = (VkDeviceSize)stride * VK_UBO_RING_SIZE;
		VK_CreateBuffer( size,
		                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &uboRings[i].buffer, &uboRings[i].memory );
		VK_CHECK( vkMapMemory( vk.device, uboRings[i].memory, 0, size, 0, &uboRings[i].mapped ) );
		uboRings[i].stride = stride;
		uboRings[i].offset = 0;
	}
}

static void VK_DestroyUBORings( void ) {
	for ( int i = 0; i < VK_MAX_FRAMES_IN_FLIGHT; i++ ) {
		if ( uboRings[i].memory ) vkUnmapMemory( vk.device, uboRings[i].memory );
		if ( uboRings[i].buffer ) vkDestroyBuffer( vk.device, uboRings[i].buffer, NULL );
		if ( uboRings[i].memory ) vkFreeMemory(   vk.device, uboRings[i].memory, NULL );
	}
	memset( uboRings, 0, sizeof(uboRings) );
}

// Allocate space in the UBO ring and return the byte offset
static uint32_t VK_AllocUBO( void ) {
	vkUBORing_t &ring = uboRings[vk.currentFrame];
	uint32_t off = ring.offset;
	ring.offset += ring.stride;
	if ( ring.offset >= ring.stride * VK_UBO_RING_SIZE ) {
		common->Warning( "VK: UBO ring overflowed (>%u interactions in one frame)", VK_UBO_RING_SIZE );
		ring.offset = 0;
	}
	return off;
}

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------

static void VK_MultiplyMatrix4( const float *a, const float *b, float *out ) {
	for ( int r = 0; r < 4; r++ ) {
		for ( int c = 0; c < 4; c++ ) {
			float sum = 0.f;
			for ( int k = 0; k < 4; k++ ) {
				sum += a[k*4+r] * b[c*4+k];
			}
			out[c*4+r] = sum;
		}
	}
}

// ---------------------------------------------------------------------------
// VK_RB_DrawInteraction - record draw commands for one light-surface interaction
// Called from VK_RB_DrawInteractions via RB_CreateSingleDrawInteractions callback.
// ---------------------------------------------------------------------------

// We use a file-static to pass the command buffer through the callback
static VkCommandBuffer s_cmd = VK_NULL_HANDLE;

static void VK_RB_DrawInteraction( const drawInteraction_t *din ) {
	vkUBORing_t &ring = uboRings[vk.currentFrame];

	// Allocate UBO slot
	uint32_t uboOffset = VK_AllocUBO();
	uint8_t *uboPtr = (uint8_t *)ring.mapped + uboOffset;

	// Fill UBO (layout matches VkInteractionUBO in vk_pipeline.cpp)
	float *f = (float *)uboPtr;
	memcpy( f,      din->localLightOrigin.ToFloatPtr(), 16 );  f += 4;
	memcpy( f,      din->localViewOrigin.ToFloatPtr(),  16 );  f += 4;
	memcpy( f,      din->lightProjection[0].ToFloatPtr(), 16 ); f += 4;
	memcpy( f,      din->lightProjection[1].ToFloatPtr(), 16 ); f += 4;
	memcpy( f,      din->lightProjection[2].ToFloatPtr(), 16 ); f += 4;
	memcpy( f,      din->lightProjection[3].ToFloatPtr(), 16 ); f += 4;
	memcpy( f,      din->bumpMatrix[0].ToFloatPtr(),     16 );  f += 4;
	memcpy( f,      din->bumpMatrix[1].ToFloatPtr(),     16 );  f += 4;
	memcpy( f,      din->diffuseMatrix[0].ToFloatPtr(),  16 );  f += 4;
	memcpy( f,      din->diffuseMatrix[1].ToFloatPtr(),  16 );  f += 4;
	memcpy( f,      din->specularMatrix[0].ToFloatPtr(), 16 );  f += 4;
	memcpy( f,      din->specularMatrix[1].ToFloatPtr(), 16 );  f += 4;

	// colorModulate / colorAdd
	static const float zero[4] = {0,0,0,0}, one[4]={1,1,1,1}, neg[4]={-1,-1,-1,-1};
	const float *cm, *ca;
	switch ( din->vertexColor ) {
	case SVC_IGNORE:          cm = zero; ca = one;  break;
	case SVC_MODULATE:        cm = one;  ca = zero; break;
	case SVC_INVERSE_MODULATE:cm = neg;  ca = one;  break;
	default:                  cm = zero; ca = one;  break;
	}
	memcpy( f, cm, 16 ); f += 4;
	memcpy( f, ca, 16 ); f += 4;

	// MVP matrix
	float mvp[16];
	VK_MultiplyMatrix4( backEnd.viewDef->projectionMatrix, din->surf->space->modelViewMatrix, mvp );
	memcpy( f, mvp, 64 ); f += 16;

	// diffuseColor, specularColor
	memcpy( f, din->diffuseColor.ToFloatPtr(),  16 ); f += 4;
	memcpy( f, din->specularColor.ToFloatPtr(), 16 ); f += 4;

	// gammaBrightness
	float gb[4] = { r_brightness.GetFloat(), r_brightness.GetFloat(), r_brightness.GetFloat(), 1.0f/r_gamma.GetFloat() };
	memcpy( f, gb, 16 ); f += 4;

	// applyGamma
	int *ip = (int *)f;
	*ip = r_gammaInShader.GetBool() ? 1 : 0;

	// screenSize (used by shader to compute shadow mask UV from gl_FragCoord)
	float *fsz = (float *)(ip + 1);
	fsz[0] = (float)vk.swapchainExtent.width;
	fsz[1] = (float)vk.swapchainExtent.height;

	// useShadowMask: 1 when RT shadow mask is valid this frame
#ifdef DHEWM3_RAYTRACING
	int *useSM = (int *)(fsz + 2);
	*useSM = ( vk.rayTracingSupported && vkRT.isInitialized && r_rtShadows.GetBool() &&
	           vkRT.shadowMask[vk.currentFrame].image != VK_NULL_HANDLE ) ? 1 : 0;
#else
	int *useSM = (int *)(fsz + 2);
	*useSM = 0;
#endif

	// Allocate descriptor set from pool
	VkDescriptorSetAllocateInfo dsAlloc = {};
	dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsAlloc.descriptorPool     = vkPipes.descPool;
	dsAlloc.descriptorSetCount = 1;
	dsAlloc.pSetLayouts        = &vkPipes.interactionDescLayout;

	VkDescriptorSet ds;
	VkResult dsResult = vkAllocateDescriptorSets( vk.device, &dsAlloc, &ds );
	if ( dsResult != VK_SUCCESS ) {
		common->Warning( "VK: descriptor set allocation failed" );
		return;
	}

	// Write UBO descriptor
	VkDescriptorBufferInfo bufInfo = {};
	bufInfo.buffer = ring.buffer;
	bufInfo.offset = uboOffset;
	bufInfo.range  = INTERACTION_UBO_SIZE;

	// Collect the 6 texture images in binding order (matches interaction.frag samplers):
	//  1=bump, 2=lightFalloff, 3=lightProjection, 4=diffuse, 5=specular, 6=specularTable
	idImage *texImages[6] = {
		din->bumpImage,
		din->lightFalloffImage,
		din->lightImage,          // light projection
		din->diffuseImage,
		din->specularImage,
		globalImages->specularTableImage,
	};

	extern bool VK_Image_GetDescriptorInfo( idImage *, VkDescriptorImageInfo * );
	extern void VK_Image_GetFallbackDescriptorInfo( VkDescriptorImageInfo * );

	VkDescriptorImageInfo imgInfos[6] = {};
	for ( int i = 0; i < 6; i++ ) {
		if ( !texImages[i] || !VK_Image_GetDescriptorInfo( texImages[i], &imgInfos[i] ) ) {
			VK_Image_GetFallbackDescriptorInfo( &imgInfos[i] );
		}
	}

	// Binding 7: shadow mask
#ifdef DHEWM3_RAYTRACING
	VkDescriptorImageInfo shadowMaskInfo = {};
	if ( vk.rayTracingSupported && vkRT.isInitialized &&
	     vkRT.shadowMask[vk.currentFrame].image != VK_NULL_HANDLE ) {
		shadowMaskInfo.sampler     = vkRT.shadowMaskSampler;
		shadowMaskInfo.imageView   = vkRT.shadowMask[vk.currentFrame].view;
		shadowMaskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	} else {
		VK_Image_GetFallbackDescriptorInfo( &shadowMaskInfo );
	}
#else
	VkDescriptorImageInfo shadowMaskInfo = {};
	VK_Image_GetFallbackDescriptorInfo( &shadowMaskInfo );
#endif

	VkWriteDescriptorSet writes[8] = {};

	// Binding 0: UBO
	writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet          = ds;
	writes[0].dstBinding      = 0;
	writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].descriptorCount = 1;
	writes[0].pBufferInfo     = &bufInfo;

	// Bindings 1-6: combined image samplers
	for ( int i = 0; i < 6; i++ ) {
		writes[1 + i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1 + i].dstSet          = ds;
		writes[1 + i].dstBinding      = (uint32_t)(1 + i);
		writes[1 + i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1 + i].descriptorCount = 1;
		writes[1 + i].pImageInfo      = &imgInfos[i];
	}

	// Binding 7: shadow mask
	writes[7].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[7].dstSet          = ds;
	writes[7].dstBinding      = 7;
	writes[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[7].descriptorCount = 1;
	writes[7].pImageInfo      = &shadowMaskInfo;

	vkUpdateDescriptorSets( vk.device, 8, writes, 0, NULL );

	// Bind descriptor set
	vkCmdBindDescriptorSets( s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                          vkPipes.interactionLayout, 0, 1, &ds, 0, NULL );

	// Bind vertex and index buffers from the vertex cache
	extern bool VK_VertexCache_GetBuffer( vertCache_t *, VkBuffer *, VkDeviceSize * );
	const srfTriangles_t *geo = din->surf->geo;

	VkBuffer     vertBuf;
	VkDeviceSize vertOffset;
	if ( !VK_VertexCache_GetBuffer( geo->ambientCache, &vertBuf, &vertOffset ) ) {
		return; // vertex data not ready — skip this surface
	}
	vkCmdBindVertexBuffers( s_cmd, 0, 1, &vertBuf, &vertOffset );

	VkBuffer     idxBuf;
	VkDeviceSize idxOffset;
	if ( !VK_VertexCache_GetBuffer( geo->indexCache, &idxBuf, &idxOffset ) ) {
		return; // index data not ready — skip this surface
	}
	const VkIndexType idxType = ( sizeof(glIndex_t) == 4 )
	                          ? VK_INDEX_TYPE_UINT32
	                          : VK_INDEX_TYPE_UINT16;
	vkCmdBindIndexBuffer( s_cmd, idxBuf, idxOffset, idxType );

	vkCmdDrawIndexed( s_cmd, (uint32_t)geo->numIndexes, 1, 0, 0, 0 );
}

// ---------------------------------------------------------------------------
// VK_RB_DrawInteractions - per-light interaction loop
// Mirrors RB_ARB2_DrawInteractions / RB_GLSL_DrawInteractions
// ---------------------------------------------------------------------------

static void VK_RB_DrawInteractions( VkCommandBuffer cmd ) {
	s_cmd = cmd;

	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline );

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		if ( vLight->lightShader->IsFogLight() )   continue;
		if ( vLight->lightShader->IsBlendLight() )  continue;
		if ( !vLight->localInteractions && !vLight->globalInteractions &&
		     !vLight->translucentInteractions ) {
			continue;
		}

		// Set scissor for this light
		if ( r_useScissor.GetBool() ) {
			VkRect2D scissor = {};
			scissor.offset.x      = backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1;
			scissor.offset.y      = backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1;
			scissor.extent.width  = vLight->scissorRect.x2 - vLight->scissorRect.x1 + 1;
			scissor.extent.height = vLight->scissorRect.y2 - vLight->scissorRect.y1 + 1;
			vkCmdSetScissor( cmd, 0, 1, &scissor );
		}

		// Stencil shadow pass (still using geometry-based stencil volumes)
		// Switch to shadow pipeline
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.shadowPipeline );
		// TODO: record shadow volume draw calls here from vLight->globalShadows / localShadows
		// This is connected to vk_accelstruct.cpp when RT shadows replace this path.

		// Switch back to interaction pipeline for lit surfaces
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipes.interactionPipeline );

		// Draw lit interactions
		for ( const drawSurf_t *surf = vLight->localInteractions; surf; surf = surf->nextOnLight ) {
			RB_CreateSingleDrawInteractions( surf, VK_RB_DrawInteraction );
		}
		for ( const drawSurf_t *surf = vLight->globalInteractions; surf; surf = surf->nextOnLight ) {
			RB_CreateSingleDrawInteractions( surf, VK_RB_DrawInteraction );
		}

		if ( !r_skipTranslucent.GetBool() ) {
			for ( const drawSurf_t *surf = vLight->translucentInteractions; surf; surf = surf->nextOnLight ) {
				RB_CreateSingleDrawInteractions( surf, VK_RB_DrawInteraction );
			}
		}
	}

	s_cmd = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VK_RB_DrawView - main per-frame view rendering
// Replaces RB_DrawView() for the Vulkan path.
// ---------------------------------------------------------------------------

void VK_RB_DrawView( const void *data ) {
	if ( !vk.isInitialized || !vkPipes.isValid ) return;

	const drawSurfsCommand_t *cmd = (const drawSurfsCommand_t *)data;
	backEnd.viewDef = cmd->viewDef;

	// --- Wait for previous frame's fence ---
	vkWaitForFences( vk.device, 1, &vk.inFlightFences[vk.currentFrame], VK_TRUE, UINT64_MAX );

	// --- Acquire swapchain image ---
	uint32_t imageIndex;
	VkResult acquireResult = vkAcquireNextImageKHR(
		vk.device, vk.swapchain, UINT64_MAX,
		vk.imageAvailableSemaphores[vk.currentFrame],
		VK_NULL_HANDLE, &imageIndex );

	if ( acquireResult == VK_ERROR_OUT_OF_DATE_KHR ) {
		// Swapchain needs recreation (window resized)
		return;
	}

	vk.currentImageIdx = imageIndex;
	vkResetFences( vk.device, 1, &vk.inFlightFences[vk.currentFrame] );

	// Reset UBO ring for this frame
	uboRings[vk.currentFrame].offset = 0;

	// --- Record command buffer ---
	VkCommandBuffer cmdBuf = vk.commandBuffers[vk.currentFrame];
	vkResetCommandBuffer( cmdBuf, 0 );

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer( cmdBuf, &beginInfo );

	// Begin render pass
	VkClearValue clearValues[2] = {};
	clearValues[0].color        = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
	clearValues[1].depthStencil = { 1.0f, 128 };  // depth=1.0, stencil=128

	VkRenderPassBeginInfo rpBegin = {};
	rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpBegin.renderPass        = vk.renderPass;
	rpBegin.framebuffer       = vk.swapchainFramebuffers[imageIndex];
	rpBegin.renderArea.offset = { 0, 0 };
	rpBegin.renderArea.extent = vk.swapchainExtent;
	rpBegin.clearValueCount   = 2;
	rpBegin.pClearValues      = clearValues;

	vkCmdBeginRenderPass( cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE );

	// Set viewport
	VkViewport viewport = { 0, 0,
	                        (float)vk.swapchainExtent.width,
	                        (float)vk.swapchainExtent.height,
	                        0.0f, 1.0f };
	vkCmdSetViewport( cmdBuf, 0, 1, &viewport );
	VkRect2D fullScissor = { {0,0}, vk.swapchainExtent };
	vkCmdSetScissor( cmdBuf, 0, 1, &fullScissor );

	// If RT is available and r_rtShadows is on, dispatch shadow rays before interaction pass
	if ( vk.rayTracingSupported && vkRT.isInitialized && r_rtShadows.GetBool() ) {
		vkCmdEndRenderPass( cmdBuf );  // RT dispatch happens outside render pass
		VK_RT_RebuildTLAS( cmdBuf, backEnd.viewDef );
		VK_RT_DispatchShadowRays( cmdBuf, backEnd.viewDef );
		// Re-open render pass for rasterization
		vkCmdBeginRenderPass( cmdBuf, &rpBegin, VK_SUBPASS_CONTENTS_INLINE );
		vkCmdSetViewport( cmdBuf, 0, 1, &viewport );
		vkCmdSetScissor( cmdBuf, 0, 1, &fullScissor );
	}

	// Draw all light interactions
	VK_RB_DrawInteractions( cmdBuf );

	vkCmdEndRenderPass( cmdBuf );
	VK_CHECK( vkEndCommandBuffer( cmdBuf ) );

	// --- Submit ---
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo = {};
	submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount   = 1;
	submitInfo.pWaitSemaphores      = &vk.imageAvailableSemaphores[vk.currentFrame];
	submitInfo.pWaitDstStageMask    = &waitStage;
	submitInfo.commandBufferCount   = 1;
	submitInfo.pCommandBuffers      = &cmdBuf;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores    = &vk.renderFinishedSemaphores[vk.currentFrame];

	VK_CHECK( vkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, vk.inFlightFences[vk.currentFrame] ) );

	// --- Present ---
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = &vk.renderFinishedSemaphores[vk.currentFrame];
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = &vk.swapchain;
	presentInfo.pImageIndices      = &imageIndex;

	vkQueuePresentKHR( vk.presentQueue, &presentInfo );

	vk.currentFrame = ( vk.currentFrame + 1 ) % VK_MAX_FRAMES_IN_FLIGHT;
}

// ---------------------------------------------------------------------------
// VK_Init / VK_Shutdown - called from glimp.cpp
// ---------------------------------------------------------------------------

// Forward declarations (vk_instance.cpp)
void VKimp_Init( SDL_Window *window );
void VKimp_Shutdown( void );

// Forward declarations (vk_image.cpp)
extern void VK_Image_Init( void );
extern void VK_Image_Shutdown( void );

void VKimp_PostInit( int width, int height ) {
	VK_CreateSwapchain( width, height );
	VK_InitPipelines();
	VK_CreateUBORings();
	VK_Image_Init();

	if ( vk.rayTracingSupported ) {
		VK_RT_Init();
		VK_RT_InitShadows();   // build shadow RT pipeline + shadow mask images
	}

	common->Printf( "VK: Backend ready\n" );
}

void VKimp_PreShutdown( void ) {
	if ( !vk.isInitialized ) return;
	vkDeviceWaitIdle( vk.device );

	if ( vk.rayTracingSupported && vkRT.isInitialized ) {
		VK_RT_Shutdown();
	}

	VK_Image_Shutdown();
	VK_DestroyUBORings();
	VK_ShutdownPipelines();
	VK_DestroySwapchain();
}
