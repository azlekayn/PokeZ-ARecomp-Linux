// Pokémon Z-A: the game's own music, sound effect and cry volumes, from F1 (runtime/hle/za_audio.c).
#pragma once
#include <stdbool.h>

#define ZA_AUDIO_N 3  // 0 music, 1 sound effects, 2 Pokémon cries

#ifdef __cplusplus
extern "C" {
#endif
void za_audio_frame(void);    // (the game thread, once a frame)
bool za_audio_ready(void);    // the game's options were found: 2.0.2 only, a few seconds after boot
int za_audio_get(int which);  // 0-10
void za_audio_set(int which, int value);  // (applied on the game thread's next frame)
#ifdef __cplusplus
}
#endif
