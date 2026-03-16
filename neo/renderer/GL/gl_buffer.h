#pragma once
#include "renderer/VertexCache.h"

// GL-specific vertex buffer resource management.
// Called through GLBackend::VertexCache_Free -> activeBackend->VertexCache_Free.
void GL_VertexCache_Free(vertCache_t *block);
