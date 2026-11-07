// ZA Recomp: what AMD's FidelityFX SDK (v1.1.4, written for MSVC) takes from the Windows C runtime, for every source in
// third_party/fsr3 (force-included by the Makefile; the SDK's own files stay as AMD shipped them where possible).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#ifdef __cplusplus
#include <cmath>
#include <codecvt>  // (ffx_vk.cpp's string conversions: only its Windows branch includes this)
#include <locale>
#include <string>
#ifndef _WIN32
#ifndef _countof
template <typename T, size_t N> constexpr size_t za_countof(T (&)[N]) { return N; }
#define _countof(a) za_countof(a)
#endif
template <size_t N> inline int wcscpy_s(wchar_t (&d)[N], const wchar_t *s) {
    wcsncpy(d, s, N - 1);
    d[N - 1] = 0;
    return 0;
}
inline int wcscpy_s(wchar_t *d, size_t n, const wchar_t *s) {
    wcsncpy(d, s, n - 1);
    d[n - 1] = 0;
    return 0;
}
#define sprintf_s snprintf
#define strcpy_s(d, n, s) (strncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0)
inline void OutputDebugStringA(const char *s) { fputs(s, stderr); }
#endif
#endif
