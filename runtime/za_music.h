// Pokémon Z-A: the player's own music in place of the game's (runtime/hle/za_music.c).
#ifndef ZA_MUSIC_H
#define ZA_MUSIC_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// reads n bytes of the RomFS image at off; false on a failure
typedef bool (*ZaMusicRead)(void *dst, uint64_t n, uint64_t off);

// (runtime/services/fs.c) where the three audio files are in the RomFS image: indexes the music streams, loads
// music/assignments.txt. Sizes of 0: not this game, or the files aren't there: everything below does nothing.
void za_music_setup(ZaMusicRead read, uint64_t bnk_off, uint64_t bnk_size, const uint64_t pck_off[2],
                    const uint64_t pck_size[2]);
// (runtime/services/fs.c, every read of the base image) replaces what a read of an assigned track returned
void za_music_filter(uint64_t off, uint8_t *dst, uint64_t n);

enum { ZA_MUSIC_ORIGINAL, ZA_MUSIC_SILENCE, ZA_MUSIC_PREPARING, ZA_MUSIC_ACTIVE, ZA_MUSIC_FAILED };
typedef struct {
    uint32_t group;  // the track's id: the lowest stream id of its music segment (kept in music/assignments.txt)
    int stems;       // streams playing together in it
    double seconds;  // the segment's length before the game loops it
    int state;       // ZA_MUSIC_*
    char file[256];  // the file assigned ("" none)
} ZaMusicNow;

// the track read last (so with the game paused in the menu, what was playing); false: none yet
bool za_music_now(ZaMusicNow *out);
// file: a path from the data root (music/..., mods/Music/...), "-" silence, NULL or "" the game's own. Applies at once and is saved.
void za_music_assign(uint32_t group, const char *file);
int za_music_assigned_count(void);
void za_music_clear_all(void);

#ifdef __cplusplus
}
#endif
#endif
