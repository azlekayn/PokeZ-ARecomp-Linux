// ooz as a shared library for tools/za/prompt_textures.py (the zaprompts helper): one C export around
// Kraken_Decompress. GPLv3 like the rest of this folder (COPYING). Built by tools/za/dist.sh:
//   g++ -O2 -shared -include sys/stat.h -Dmain=ooz_main kraken.cpp bitknit.cpp lzna.cpp ooz_lib.cpp -o ooz.dll
#include "stdafx.h"

int Kraken_Decompress(const byte *src, size_t src_len, byte *dst, size_t dst_len);

#ifdef _WIN32
#define OOZ_EXPORT extern "C" __declspec(dllexport)
#else
#define OOZ_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// src_len bytes of an Oodle stream -> dst_len bytes; returns the bytes written (dst_len when whole), or -1
OOZ_EXPORT int ooz_decompress(const byte *src, size_t src_len, byte *dst, size_t dst_len) {
    return Kraken_Decompress(src, src_len, dst, dst_len);
}
