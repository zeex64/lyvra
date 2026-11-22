// Forced-include compatibility shim for building the AMD FidelityFX SDK (v1.1.4)
// with GCC/Clang on Linux. Only applied to the ffx_sdk target (via -include), never
// to consumers. Provides the MSVC CRT "secure" functions and helpers the SDK uses.
#pragma once

#ifndef _WIN32

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>

#ifdef __cplusplus
#include <cstddef>
#include <cwchar>
#include <cstring>
#include <cmath>     // ffx_vk.cpp uses floor/log2 unqualified without <cmath>
#include <bit>       // ffx_util.h uses std::popcount under C++20 without <bit>
#include <codecvt>   // ffx_vk.cpp: std::wstring_convert (non-Windows fallback)
#include <locale>
#endif

// ffx_shared sources use FFX_UNUSED on the non-Windows path without including
// ffx_util.h; same definition as ffx_util.h, so the later redefinition is benign.
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

#ifndef _countof
#ifdef __cplusplus
template <typename T, size_t N>
char (*ffx_compat_countof_helper(T (&)[N]))[N];
#define _countof(a) (sizeof(*ffx_compat_countof_helper(a)))
#else
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#endif

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#define sprintf_s(buf, size, ...)  snprintf((buf), (size), __VA_ARGS__)
#define swprintf_s(buf, size, ...) swprintf((buf), (size), __VA_ARGS__)
#define _snprintf snprintf
#define _snwprintf swprintf
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

#ifdef __cplusplus

// Truncating copies: the SDK only uses these for debug names / labels.
inline int wcscpy_s(wchar_t* dst, size_t dstCount, const wchar_t* src)
{
    if (!dst || dstCount == 0)
        return 22;  // EINVAL
    if (!src)
    {
        dst[0] = L'\0';
        return 22;
    }
    size_t n = wcslen(src);
    if (n >= dstCount)
        n = dstCount - 1;
    wmemcpy(dst, src, n);
    dst[n] = L'\0';
    return 0;
}
template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
    return wcscpy_s(dst, N, src);
}

inline int wcsncpy_s(wchar_t* dst, size_t dstCount, const wchar_t* src, size_t count)
{
    if (!dst || dstCount == 0)
        return 22;
    if (!src)
    {
        dst[0] = L'\0';
        return 22;
    }
    size_t n = wcslen(src);
    if (count != _TRUNCATE && n > count)
        n = count;
    if (n >= dstCount)
        n = dstCount - 1;
    wmemcpy(dst, src, n);
    dst[n] = L'\0';
    return 0;
}
template <size_t N>
inline int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count)
{
    return wcsncpy_s(dst, N, src, count);
}

inline int wcscat_s(wchar_t* dst, size_t dstCount, const wchar_t* src)
{
    if (!dst || !src || dstCount == 0)
        return 22;
    size_t cur = wcsnlen(dst, dstCount);
    return wcscpy_s(dst + cur, dstCount - cur, src);
}
template <size_t N>
inline int wcscat_s(wchar_t (&dst)[N], const wchar_t* src)
{
    return wcscat_s(dst, N, src);
}

inline int strcpy_s(char* dst, size_t dstCount, const char* src)
{
    if (!dst || dstCount == 0)
        return 22;
    if (!src)
    {
        dst[0] = '\0';
        return 22;
    }
    size_t n = strlen(src);
    if (n >= dstCount)
        n = dstCount - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src)
{
    return strcpy_s(dst, N, src);
}

inline int strncpy_s(char* dst, size_t dstCount, const char* src, size_t count)
{
    if (!dst || dstCount == 0)
        return 22;
    if (!src)
    {
        dst[0] = '\0';
        return 22;
    }
    size_t n = strlen(src);
    if (count != _TRUNCATE && n > count)
        n = count;
    if (n >= dstCount)
        n = dstCount - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return 0;
}

inline int memcpy_s(void* dst, size_t dstSize, const void* src, size_t count)
{
    if (!dst || (!src && count))
        return 22;
    if (count > dstSize)
        return 34;  // ERANGE
    memcpy(dst, src, count);
    return 0;
}

inline int wcstombs_s(size_t* converted, char* dst, size_t dstSize, const wchar_t* src, size_t count)
{
    size_t limit = (count == _TRUNCATE || count >= dstSize) ? (dstSize ? dstSize - 1 : 0) : count;
    size_t r     = dst ? wcstombs(dst, src, limit) : wcstombs(nullptr, src, 0);
    if (r == (size_t)-1)
    {
        if (dst && dstSize)
            dst[0] = '\0';
        if (converted)
            *converted = 0;
        return 42;  // EILSEQ
    }
    if (dst && dstSize)
        dst[r < dstSize ? r : dstSize - 1] = '\0';
    if (converted)
        *converted = r + 1;
    return 0;
}

inline int mbstowcs_s(size_t* converted, wchar_t* dst, size_t dstSize, const char* src, size_t count)
{
    size_t limit = (count == _TRUNCATE || count >= dstSize) ? (dstSize ? dstSize - 1 : 0) : count;
    size_t r     = dst ? mbstowcs(dst, src, limit) : mbstowcs(nullptr, src, 0);
    if (r == (size_t)-1)
    {
        if (dst && dstSize)
            dst[0] = L'\0';
        if (converted)
            *converted = 0;
        return 42;
    }
    if (dst && dstSize)
        dst[r < dstSize ? r : dstSize - 1] = L'\0';
    if (converted)
        *converted = r + 1;
    return 0;
}

#endif  // __cplusplus

#endif  // !_WIN32
