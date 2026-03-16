#include "sys/platform.h"
#include "idlib/hashing/MD4.h"
#include "renderer/tr_local.h"

#include "renderer/Image.h"
#include "renderer/RendererBackend.h"
#include "renderer/GL/gl_image.h"

static byte mipBlendColors[16][4] = {
    {0, 0, 0, 0},     {255, 0, 0, 128}, {0, 255, 0, 128}, {0, 0, 255, 128}, {255, 0, 0, 128}, {0, 255, 0, 128},
    {0, 0, 255, 128}, {255, 0, 0, 128}, {0, 255, 0, 128}, {0, 0, 255, 128}, {255, 0, 0, 128}, {0, 255, 0, 128},
    {0, 0, 255, 128}, {255, 0, 0, 128}, {0, 255, 0, 128}, {0, 0, 255, 128},
};

/*
================
GenerateImage

The alpha channel bytes should be 255 if you don't
want the channel.

We need a material characteristic to ask for specific texture modes.

Designed limitations of flexibility:

No support for texture borders.

No support for texture border color.

No support for texture environment colors or GL_BLEND or GL_DECAL
texture environments, because the automatic optimization to single
or dual component textures makes those modes potentially undefined.

No non-power-of-two images.

No palettized textures.

There is no way to specify separate wrap/clamp values for S and T

There is no way to specify explicit mip map levels

================
*/
// ---------------------------------------------------------------------------
// GL_PurgeTexture / GL_GenerateTexture
// Backend-agnostic wrappers called by GLBackend::Image_Purge / Image_Upload.
// These must NOT be called when Vulkan is active.
// ---------------------------------------------------------------------------

void GL_PurgeTexture(idImage *img)
{
    if (img->texnum != idImage::TEXTURE_NOT_LOADED)
    {
        qglDeleteTextures(1, &img->texnum);
        img->texnum = idImage::TEXTURE_NOT_LOADED;
    }

    // clear all the current binding caches, so the next bind will do a real one
    for (int i = 0; i < MAX_MULTITEXTURE_UNITS; i++)
    {
        backEnd.glState.tmu[i].current2DMap = -1;
        backEnd.glState.tmu[i].current3DMap = -1;
        backEnd.glState.tmu[i].currentCubeMap = -1;
    }
}

void GL_GenerateTexture(idImage *img, const byte *pic, int width, int height, textureFilter_t filterParm,
                        bool allowDownSizeParm, textureRepeat_t repeatParm, textureDepth_t depthParm)
{
    bool preserveBorder;
    byte *scaledBuffer;
    int scaled_width, scaled_height;
    byte *shrunk;

    GL_PurgeTexture(img);

    img->filter = filterParm;
    img->allowDownSize = allowDownSizeParm;
    img->repeat = repeatParm;
    img->depth = depthParm;

    // don't let mip mapping smear the texture into the clamped border
    preserveBorder = (img->repeat == TR_CLAMP_TO_ZERO);

    // make sure it is a power of 2
    scaled_width = MakePowerOfTwo(width);
    scaled_height = MakePowerOfTwo(height);

    if (scaled_width != width || scaled_height != height)
    {
        common->Error("R_CreateImage: not a power of 2 image");
    }

    // Optionally modify our width/height based on options/hardware
    img->GetDownsize(scaled_width, scaled_height);

    scaledBuffer = NULL;

    // generate the texture number
    qglGenTextures(1, &img->texnum);

    // select proper internal format before we resample
    img->internalFormat = img->SelectInternalFormat(&pic, 1, width, height, img->depth);

    // copy or resample data as appropriate for first MIP level
    if ((scaled_width == width) && (scaled_height == height))
    {
        // we must copy even if unchanged, because the border zeroing
        // would otherwise modify const data
        scaledBuffer = (byte *)R_StaticAlloc(sizeof(unsigned) * scaled_width * scaled_height);
        memcpy(scaledBuffer, pic, width * height * 4);
    }
    else
    {
        // resample down as needed (FIXME: this doesn't seem like it resamples
        // anymore!) scaledBuffer = R_ResampleTexture( pic, width, height, width >>=
        // 1, height >>= 1 );
        scaledBuffer = R_MipMap(pic, width, height, preserveBorder);
        width >>= 1;
        height >>= 1;
        if (width < 1)
        {
            width = 1;
        }
        if (height < 1)
        {
            height = 1;
        }

        while (width > scaled_width || height > scaled_height)
        {
            shrunk = R_MipMap(scaledBuffer, width, height, preserveBorder);
            R_StaticFree(scaledBuffer);
            scaledBuffer = shrunk;

            width >>= 1;
            height >>= 1;
            if (width < 1)
            {
                width = 1;
            }
            if (height < 1)
            {
                height = 1;
            }
        }

        // one might have shrunk down below the target size
        scaled_width = width;
        scaled_height = height;
    }

    img->uploadHeight = scaled_height;
    img->uploadWidth = scaled_width;
    img->type = TT_2D;

    // zero the border if desired, allowing clamped projection textures
    // even after picmip resampling or careless artists.
    if (img->repeat == TR_CLAMP_TO_ZERO)
    {
        byte rgba[4];

        rgba[0] = rgba[1] = rgba[2] = 0;
        rgba[3] = 255;
        R_SetBorderTexels((byte *)scaledBuffer, width, height, rgba);
    }
    if (img->repeat == TR_CLAMP_TO_ZERO_ALPHA)
    {
        byte rgba[4];

        rgba[0] = rgba[1] = rgba[2] = 255;
        rgba[3] = 0;
        R_SetBorderTexels((byte *)scaledBuffer, width, height, rgba);
    }

    if (img->generatorFunction == NULL && ((img->depth == TD_BUMP && globalImages->image_writeNormalTGA.GetBool()) ||
                                           (img->depth != TD_BUMP && globalImages->image_writeTGA.GetBool())))
    {
        // Optionally write out the texture to a .tga
        char filename[MAX_IMAGE_NAME];
        img->ImageProgramStringToCompressedFileName(img->imgName, filename);
        char *ext = strrchr(filename, '.');
        if (ext)
        {
            strcpy(ext, ".tga");
            R_WriteTGA(filename, scaledBuffer, scaled_width, scaled_height, false);
        }
    }

    // swap the red and alpha for rxgb support
    // do this even on tga normal maps so we only have to use
    // one fragment program
    // if the image is precompressed ( either in palletized mode or true rxgb mode
    // ) then it is loaded above and the swap never happens here
    if (img->depth == TD_BUMP && globalImages->image_useNormalCompression.GetInteger() != 1)
    {
        for (int i = 0; i < scaled_width * scaled_height * 4; i += 4)
        {
            scaledBuffer[i + 3] = scaledBuffer[i];
            scaledBuffer[i] = 0;
        }
    }
    // upload the main image level
    img->Bind();

    if (img->internalFormat == GL_COLOR_INDEX8_EXT)
    {
        img->UploadCompressedNormalMap(scaled_width, scaled_height, scaledBuffer, 0);
    }
    else
    {
        qglTexImage2D(GL_TEXTURE_2D, 0, img->internalFormat, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      scaledBuffer);
    }

    // create and upload the mip map levels, which we do in all cases, even if we
    // don't think they are needed
    int miplevel = 0;
    while (scaled_width > 1 || scaled_height > 1)
    {
        // preserve the border after mip map unless repeating
        shrunk = R_MipMap(scaledBuffer, scaled_width, scaled_height, preserveBorder);
        R_StaticFree(scaledBuffer);
        scaledBuffer = shrunk;

        scaled_width >>= 1;
        scaled_height >>= 1;
        if (scaled_width < 1)
        {
            scaled_width = 1;
        }
        if (scaled_height < 1)
        {
            scaled_height = 1;
        }
        miplevel++;

        // this is a visualization tool that shades each mip map
        // level with a different color so you can see the
        // rasterizer's texture level selection algorithm
        // Changing the color doesn't help with lumminance/alpha/intensity
        // formats...
        if (img->depth == TD_DIFFUSE && globalImages->image_colorMipLevels.GetBool())
        {
            R_BlendOverTexture((byte *)scaledBuffer, scaled_width * scaled_height, mipBlendColors[miplevel]);
        }

        // upload the mip map
        if (img->internalFormat == GL_COLOR_INDEX8_EXT)
        {
            img->UploadCompressedNormalMap(scaled_width, scaled_height, scaledBuffer, miplevel);
        }
        else
        {
            qglTexImage2D(GL_TEXTURE_2D, miplevel, img->internalFormat, scaled_width, scaled_height, 0, GL_RGBA,
                          GL_UNSIGNED_BYTE, scaledBuffer);
        }
    }

    if (scaledBuffer != 0)
    {
        R_StaticFree(scaledBuffer);
    }

    img->SetImageFilterAndRepeat();

    // see if we messed anything up
    GL_CheckErrors();
}
