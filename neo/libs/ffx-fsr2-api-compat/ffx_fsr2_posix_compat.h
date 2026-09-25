/*
===========================================================================
Annex K shims for the vendored AMD FSR 2.2.1 sources.

ffx_fsr2.cpp and vk/ffx_fsr2_vk.cpp call wcscpy_s / wcstombs_s, which only
exist in MSVC's CRT.  Rather than patch AMD's files (which must stay
byte-identical to the pinned upstream tag), this header is force-included into
the four FSR2 translation units on non-MSVC compilers via -include.

It lives outside libs/ffx-fsr2-api/ deliberately: that directory is upstream's
and nothing of ours belongs in it.

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
===========================================================================
*/

#pragma once

#if !defined(_MSC_VER)

#include <cstddef>
#include <cerrno>
#include <cstdlib>
#include <cwchar>

// MSVC's array-reference overload: the destination bound is deduced, so the
// call sites need no change.  Truncates rather than overruns.
template <size_t N> static inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t *src)
{
    if (!src)
    {
        dst[0] = L'\0';
        return EINVAL;
    }
    size_t i = 0;
    for (; i + 1 < N && src[i] != L'\0'; i++)
        dst[i] = src[i];
    dst[i] = L'\0';
    return (src[i] == L'\0') ? 0 : ERANGE;
}

// wcstombs_s(&converted, dst, dstBytes, src, maxCount).  *converted counts the
// terminating null, matching MSVC.
static inline int wcstombs_s(size_t *converted, char *dst, size_t dstBytes, const wchar_t *src, size_t maxCount)
{
    if (!dst || dstBytes == 0)
    {
        if (converted)
            *converted = 0;
        return EINVAL;
    }
    if (!src)
    {
        dst[0] = '\0';
        if (converted)
            *converted = 0;
        return EINVAL;
    }

    const size_t limit = (maxCount < dstBytes - 1) ? maxCount : dstBytes - 1;
    const size_t n = wcstombs(dst, src, limit);
    if (n == (size_t)-1)
    {
        dst[0] = '\0';
        if (converted)
            *converted = 0;
        return EILSEQ;
    }
    dst[n] = '\0';
    if (converted)
        *converted = n + 1;
    return 0;
}

#endif // !_MSC_VER
