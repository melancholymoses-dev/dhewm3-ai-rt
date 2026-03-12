#include "renderer/RendererBackend.h"
#include "renderer/tr_local.h"         // whatever GL headers you need

// Forward declarations of the existing GL free functions
// (the ones that already exist in draw_glsl.cpp, tr_backend.cpp, etc.)
extern void RB_DrawView( const void* );
extern void GL_UploadTexture( idImage*, const byte*, int, int );
// etc.

void GLBackend::DrawView( const viewDef_t* view ) {
    RB_DrawView( view );   // just delegate to the existing function
}

void GLBackend::Image_Upload( idImage* img, const byte* data, int w, int h ) {
    GL_UploadTexture( img, data, w, h );
}
// ...
