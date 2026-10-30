// Royale Gauntlet (Pokémon Z-A): a night-only roguelike run on top of the Z-A Royale randomizer. See za_gauntlet.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool za_gauntlet_on(void);
const char *za_gauntlet_rank_name(void);        // "Z".."A" for the current run
int64_t za_gauntlet_points(void);
void za_gauntlet_stats(int *wins, int *losses, int *streak, int *best_streak);
void za_gauntlet_lifetime(int64_t *best_points_ever, int *runs_completed, int *runs_failed);

// royale_random.c's seams (both no-ops when za_gauntlet_on() is false):
void za_gauntlet_night_begin(void);                                  // once per facility visit, before the roll
void za_gauntlet_pick_trainer(const char *trid, int *lo, int *hi);   // once per trainer in that visit's roll

// Fires once a win/loss is known (not wired to a real signal yet - see runtime/hle/za_gauntlet.c's header comment).
void za_gauntlet_record_outcome(const char *trid, bool won);

// runtime/services/fs.c's save read/write hooks (mirrors flags_ext_save_read/_written's shape).
void za_gauntlet_save_read(const char *path, uint64_t addr, int64_t off, int64_t size);
bool za_gauntlet_save_written(const char *path, uint64_t addr, int64_t off, int64_t size);

// F1 menu debug aid (LARECOMP_GAUNTLET_DEBUG=1): simulate outcomes until real battle-outcome detection is wired.
bool za_gauntlet_debug_on(void);
void za_gauntlet_debug_win(void);
void za_gauntlet_debug_loss(void);
void za_gauntlet_debug_night(void);
