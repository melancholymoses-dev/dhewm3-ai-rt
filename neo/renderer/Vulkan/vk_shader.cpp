/*
===========================================================================

Doom 3 GPL Source Code
dhewm3 Vulkan backend - SPIR-V shader loading.

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

#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// VK_LoadSPIRV - load a compiled SPIR-V file from disk
// Returns VK_NULL_HANDLE on failure.
// ---------------------------------------------------------------------------

VkShaderModule VK_LoadSPIRV( const char *path ) {
	// Try the game filesystem first
	byte *fileData = NULL;
	int   fileLen  = fileSystem->ReadFile( path, (void **)&fileData, NULL );

	if ( fileLen <= 0 || !fileData ) {
		common->Warning( "VK_LoadSPIRV: could not find '%s'", path );
		return VK_NULL_HANDLE;
	}

	if ( fileLen % 4 != 0 ) {
		common->Warning( "VK_LoadSPIRV: '%s' size is not a multiple of 4 bytes", path );
		fileSystem->FreeFile( fileData );
		return VK_NULL_HANDLE;
	}

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = (size_t)fileLen;
	createInfo.pCode    = (const uint32_t *)fileData;

	VkShaderModule module = VK_NULL_HANDLE;
	VkResult result = vkCreateShaderModule( vk.device, &createInfo, NULL, &module );

	fileSystem->FreeFile( fileData );

	if ( result != VK_SUCCESS ) {
		common->Warning( "VK_LoadSPIRV: vkCreateShaderModule failed for '%s' (result=%d)", path, (int)result );
		return VK_NULL_HANDLE;
	}

	return module;
}

// ---------------------------------------------------------------------------
// VK_LoadSPIRVFromMemory - create a shader module from an in-memory SPIR-V blob
// ---------------------------------------------------------------------------

VkShaderModule VK_LoadSPIRVFromMemory( const uint32_t *code, size_t codeSize ) {
	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = codeSize;
	createInfo.pCode    = code;

	VkShaderModule module = VK_NULL_HANDLE;
	VK_CHECK( vkCreateShaderModule( vk.device, &createInfo, NULL, &module ) );
	return module;
}
