/*
Player-body exclusion test shared by the RT shadow and GI ray generators.

The first-person body is rasterised into the depth buffer but kept out of the TLAS
(vk_accelstruct.cpp rejects view-local entities), while the world body stands in the
same place inside it.  Rays leaving an fp-body pixel therefore hit that duplicate
within a couple of units and the body loses both direct light and bounce, so those
pixels — and only those — must drop the player instances (TLAS mask 0x02).

The G-buffer prepass answers "is this pixel view-local geometry" exactly, in
gbufAlbedo's alpha (gbuffer.frag, GBuf_SurfaceClass; see gbuf_albedo_class.glsl
for the packing).  Camera distance cannot: a wall you stand against sits at the
same range as your own thighs, so the old sphere cancelled the player's shadow
on that wall too.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
*/

#ifndef RT_PLAYER_EXCLUDE_GLSL
#define RT_PLAYER_EXCLUDE_GLSL

#include "gbuf_albedo_class.glsl"

bool rt_IsViewLocalClass( float alpha )
{
    return GBuf_UnpackClass( alpha ) == GBUF_CLASS_VIEWLOCAL;
}

// classValid is 0 when no G-buffer was written this frame (no prepass).  Then
// fall back to the legacy camera-distance sphere rather than silently losing the
// exclusion, which would black out the body.
bool rt_PlayerExcluded( sampler2D gbufAlbedo, ivec2 coord, int classValid,
                        float camDist, float fallbackRadius )
{
    if ( classValid != 0 ) {
        return rt_IsViewLocalClass( texelFetch( gbufAlbedo, coord, 0 ).a );
    }
    return fallbackRadius > 0.0 && camDist < fallbackRadius;
}

#endif // RT_PLAYER_EXCLUDE_GLSL
