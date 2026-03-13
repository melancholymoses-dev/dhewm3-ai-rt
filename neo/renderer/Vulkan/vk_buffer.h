#pragma once
#include <renderer/VertexCache.h>

void VK_VertexCache_Alloc(vertCache_t *block, const void *data, int size, bool indexBuffer);
void VK_VertexCache_Free(vertCache_t *block);