// Linux/POSIX stand-in for third_party/ooz/stdafx.h so Powzix ooz (Kraken/BitKnit/LZNA) builds with
// g++ and tools/za/trdata_extract.py (or prompt_textures.py) can read Z-A's Oodle packs on Linux.
// Copy the ooz sources + this file into a scratch dir as stdafx.h, then:
//   g++ -O2 -shared -fPIC -msse2 -Dmain=ooz_main kraken.cpp bitknit.cpp lzna.cpp ooz_lib.cpp -o ooz.so
// (ooz is GPLv3: third_party/ooz/README.ZARecomp.md. This header is a build shim only.)
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <sys/stat.h>
#include <emmintrin.h>
#include <xmmintrin.h>
#include <x86intrin.h>

typedef unsigned char byte;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef short int16;
typedef unsigned int uint32;
typedef int int32;
typedef unsigned long long uint64;
typedef long long int64;
typedef long long __int64;  // MSVC spelling used by the standalone ooz tool

// MSVC intrinsics -> GCC builtins.
static inline unsigned char _BitScanReverse(unsigned long *index, unsigned int mask) {
    if (!mask) return 0;
    *index = 31u - (unsigned)__builtin_clz(mask);
    return 1;
}
static inline unsigned char _BitScanForward(unsigned long *index, unsigned int mask) {
    if (!mask) return 0;
    *index = (unsigned)__builtin_ctz(mask);
    return 1;
}
#define _byteswap_ushort(x) __builtin_bswap16((uint16)(x))
#define _byteswap_ulong(x) __builtin_bswap32((uint32)(x))
#define _byteswap_uint64(x) __builtin_bswap64((uint64)(x))
#define __forceinline inline __attribute__((always_inline))
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// Windows bits the standalone ooz tool references (no-ops on Linux; we only need Kraken_Decompress).
#define WINAPI
typedef void *HINSTANCE;
typedef void *FARPROC;
static inline HINSTANCE LoadLibraryA(const char *) { return 0; }
static inline FARPROC GetProcAddress(HINSTANCE, const char *) { return 0; }
static inline void FreeLibrary(HINSTANCE) {}
typedef long long LARGE_INTEGER;
static inline void QueryPerformanceCounter(LARGE_INTEGER *v) { *v = 0; }
static inline void QueryPerformanceFrequency(LARGE_INTEGER *v) { *v = 1; }
