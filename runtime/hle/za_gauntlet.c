// Royale Gauntlet (Pokémon Z-A): a night-only roguelike run built on top of the Z-A Royale randomizer
// (runtime/hle/royale_random.c). That file already re-rolls the night Battle Zone trainers once per facility visit
// (za_royale_random_roll(), called from runtime/services/fs.c's first read of trdata_array.bin) - the one event
// that already happens once per in-game night, so we use it as the day boundary instead of hooking the
// (unimplemented) day/night transition.
//
// Win a Royale bout: points go up (more for an Infinite-tier trainer). Lose one: points go down, but a single loss
// never ends the run by itself. What does: an end-of-day quota - Ranks Z..B each demand a minimum net point gain
// before the next facility visit, or the run resets to Rank Z/0 points (za_gauntlet_night_begin). Reaching Rank A
// ends the run as a win instead (za_gauntlet_record_outcome). None of this is the game's own real za_rank/za_point
// system (documented but unhooked - see ZA-NEMESIS-RE.md) - it's a custom ladder layered on top of it.
//
// Outcome detection (knowing a bout was won or lost) isn't wired to a real signal yet - za_gauntlet_record_outcome()
// works and persists correctly, but nothing calls it from an actual battle result yet. Until that lands,
// LARECOMP_GAUNTLET_DEBUG gives the F1 menu two buttons to simulate a win/loss so the rest of the pipeline (rank
// math, the daily quota, persistence) is testable on its own.
//
// Progress is custom state, not part of the game's own save: a sidecar file next to each save slot
// (savedataN.bin.gauntletx), the same shape as runtime/hle/flags_ext.c's extended event flags, but without its
// CRC-matched slot tracking (there's no Z-A equivalent of GA(LIVE_SAVE) to drive that) - the slot in play is just
// whichever save path was last read or written.
//
//   LARECOMP_GAUNTLET=1        force the mode on (overrides the setting); the F1 menu's "Royale Gauntlet mode" does it
//   LARECOMP_GAUNTLET_DEBUG=1  two extra F1 buttons to simulate a win/loss (see above)
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "../platform.h"
#include "za_gauntlet.h"

// worst to best, the full alphabet Z..A (26 tiers) - the user's own choice for this custom mode, not a recreation
// of the real Z-A Royale's rank set: RE found the real game only uses 14 of these letters (Z..U then H..A,
// skipping I..T entirely - see runtime/hle/royale_random.c's header comment and the za-trainer-spawn-re memory
// note). The thresholds/quotas below are this custom mode's own invention either way (the real
// rank_up_point_condition is un-RE'd). A has no quota - reaching it completes the run.
static const char *const RANK_NAMES[] = {"Z", "Y", "X", "W", "V", "U", "T", "S", "R", "Q", "P", "O", "N", "M", "L",
                                          "K", "J", "I", "H", "G", "F", "E", "D", "C", "B", "A"};
static const int64_t RANK_THRESHOLDS[] = {0, 100, 220, 360, 520, 700, 900, 1120, 1360, 1620, 1900, 2200, 2520, 2860,
                                           3220, 3600, 4000, 4420, 4860, 5320, 5800, 6300, 6820, 7360, 7920, 8500};
static const int RANK_QUOTAS[] = {20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190,
                                   200, 210, 220, 230, 240, 250, 260, -1};
enum { N_RANKS = 26, RANK_A = N_RANKS - 1 };

typedef struct {
    // current run
    int64_t points, day_start_points;
    bool day_started;  // false only before this run's first facility visit - nothing to grade yet
    int wins, losses, nights, streak, best_streak;
    // lifetime: survives a run ending (reset by nothing but a fresh sidecar)
    int64_t best_points_ever;
    int runs_completed, runs_failed;
} GauntletState;

static GauntletState g_state;
static char g_current_save[1100];  // path of the save last read or written (no Z-A GA(LIVE_SAVE) to track it by)

static int rank_index_for(int64_t points) {
    int ri = 0;
    for (int i = 0; i < N_RANKS; i++)
        if (points >= RANK_THRESHOLDS[i]) ri = i;
    return ri;
}

// matches royale_random.c's night()'s Infinite-tier prefixes
static bool infinite_trid(const char *trid) {
    return trid && (!strncmp(trid, "za_rank_inf", 11) || !strncmp(trid, "za_inf_", 7) || !strncmp(trid, "dim_rank_", 9));
}

RT_ON_RESET(za_gauntlet_reset) {
    memset(&g_state, 0, sizeof g_state);
    g_current_save[0] = 0;
}

bool za_gauntlet_on(void) {
    const char *e = ENV("LARECOMP_GAUNTLET");
    if (e) return atoi(e) != 0;
    return g_settings.za_gauntlet_on;
}

static bool debug_on(void) {
    const char *e = ENV("LARECOMP_GAUNTLET_DEBUG");
    return e && atoi(e) != 0;
}

const char *za_gauntlet_rank_name(void) { return RANK_NAMES[rank_index_for(g_state.points)]; }
int64_t za_gauntlet_points(void) { return g_state.points; }
void za_gauntlet_stats(int *wins, int *losses, int *streak, int *best_streak) {
    if (wins) *wins = g_state.wins;
    if (losses) *losses = g_state.losses;
    if (streak) *streak = g_state.streak;
    if (best_streak) *best_streak = g_state.best_streak;
}
void za_gauntlet_lifetime(int64_t *best_points_ever, int *runs_completed, int *runs_failed) {
    if (best_points_ever) *best_points_ever = g_state.best_points_ever;
    if (runs_completed) *runs_completed = g_state.runs_completed;
    if (runs_failed) *runs_failed = g_state.runs_failed;
}

// ---- sidecar persistence: written field-by-field (not as a struct blob) so layout can't drift across platforms

static void sidecar_path(const char *save, char *out, size_t n) { snprintf(out, n, "%s.gauntletx", save); }

#define GAUNTLET_MAGIC 0x5a414754u  // "ZAGT"
#define GAUNTLET_VERSION 1u

static void persist(void) {
    if (!g_current_save[0]) return;  // no save in play yet - nothing to persist against
    char sc[1200];
    sidecar_path(g_current_save, sc, sizeof sc);
    FILE *f = fopen(sc, "wb");
    if (!f) return;
    uint32_t u32;
    u32 = GAUNTLET_MAGIC; fwrite(&u32, sizeof u32, 1, f);
    u32 = GAUNTLET_VERSION; fwrite(&u32, sizeof u32, 1, f);
    fwrite(&g_state.points, sizeof g_state.points, 1, f);
    fwrite(&g_state.day_start_points, sizeof g_state.day_start_points, 1, f);
    u32 = g_state.day_started ? 1 : 0; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.wins; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.losses; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.nights; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.streak; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.best_streak; fwrite(&u32, sizeof u32, 1, f);
    fwrite(&g_state.best_points_ever, sizeof g_state.best_points_ever, 1, f);
    u32 = (uint32_t)g_state.runs_completed; fwrite(&u32, sizeof u32, 1, f);
    u32 = (uint32_t)g_state.runs_failed; fwrite(&u32, sizeof u32, 1, f);
    fclose(f);
}

static void load_for(const char *save) {
    memset(&g_state, 0, sizeof g_state);
    char sc[1200];
    sidecar_path(save, sc, sizeof sc);
    FILE *f = fopen(sc, "rb");
    if (!f) return;
    uint32_t magic = 0, version = 0, u32;
    if (fread(&magic, sizeof magic, 1, f) != 1 || magic != GAUNTLET_MAGIC) { fclose(f); return; }
    if (fread(&version, sizeof version, 1, f) != 1) { fclose(f); return; }  // (only version 1 exists so far)
    fread(&g_state.points, sizeof g_state.points, 1, f);
    fread(&g_state.day_start_points, sizeof g_state.day_start_points, 1, f);
    fread(&u32, sizeof u32, 1, f); g_state.day_started = u32 != 0;
    fread(&u32, sizeof u32, 1, f); g_state.wins = (int)u32;
    fread(&u32, sizeof u32, 1, f); g_state.losses = (int)u32;
    fread(&u32, sizeof u32, 1, f); g_state.nights = (int)u32;
    fread(&u32, sizeof u32, 1, f); g_state.streak = (int)u32;
    fread(&u32, sizeof u32, 1, f); g_state.best_streak = (int)u32;
    fread(&g_state.best_points_ever, sizeof g_state.best_points_ever, 1, f);
    fread(&u32, sizeof u32, 1, f); g_state.runs_completed = (int)u32;
    fread(&u32, sizeof u32, 1, f); g_state.runs_failed = (int)u32;
    fclose(f);
}

static void use_slot(const char *path) {
    if (!strcmp(g_current_save, path)) return;
    strlcpy(g_current_save, path, sizeof g_current_save);
    load_for(path);
}

void za_gauntlet_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)addr, (void)size;
    if (!za_gauntlet_on() || off != 0) return;
    use_slot(path);
}

bool za_gauntlet_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    (void)addr, (void)size;
    if (za_gauntlet_on() && off == 0) { use_slot(path); persist(); }
    return true;
}

// ---- the run itself

static void reset_run(void) {
    g_state.points = 0;
    g_state.day_start_points = 0;
    g_state.wins = g_state.losses = g_state.streak = g_state.nights = 0;
    // best_streak and the lifetime fields are untouched - a run ending doesn't erase them
}

void za_gauntlet_night_begin(void) {
    if (!za_gauntlet_on()) return;
    if (g_state.day_started) {
        int ri = rank_index_for(g_state.day_start_points);  // grade against the rank held when the night began
        int quota = RANK_QUOTAS[ri];
        int64_t gained = g_state.points - g_state.day_start_points;
        if (quota >= 0 && gained < quota) {
            LOG("[gauntlet] night %d as Rank %s: needed +%d points, got %+lld - run over\n",
                g_state.nights, RANK_NAMES[ri], quota, (long long)gained);
            g_state.runs_failed++;
            reset_run();
        }
    }
    g_state.day_start_points = g_state.points;
    g_state.day_started = true;
    g_state.nights++;
    persist();
}

void za_gauntlet_pick_trainer(const char *trid, int *lo, int *hi) {
    (void)trid, (void)lo, (void)hi;  // MVP: no override - royale_random.c's own roll stands.
    // Future seam: a Nemesis module could wrap this to bias the level band for a remembered trainer.
}

void za_gauntlet_record_outcome(const char *trid, bool won) {
    if (!za_gauntlet_on()) return;
    if (won) {
        g_state.points += infinite_trid(trid) ? 90 : 40;
        g_state.wins++;
        if (++g_state.streak > g_state.best_streak) g_state.best_streak = g_state.streak;
    } else {
        int64_t penalty = g_state.points / 5;  // 20%
        if (penalty < 20) penalty = 20;
        g_state.points -= penalty;
        if (g_state.points < 0) g_state.points = 0;
        g_state.losses++;
        g_state.streak = 0;
    }
    if (g_state.points > g_state.best_points_ever) g_state.best_points_ever = g_state.points;
    if (rank_index_for(g_state.points) >= RANK_A) {
        LOG("[gauntlet] Rank A reached (%lld points, %d nights) - run complete!\n",
            (long long)g_state.points, g_state.nights);
        g_state.runs_completed++;
        reset_run();
    }
    persist();
}

// ---- F1 menu debug aid (LARECOMP_GAUNTLET_DEBUG=1): simulate a bout's outcome without real battle-outcome detection

bool za_gauntlet_debug_on(void) { return debug_on(); }
void za_gauntlet_debug_win(void) { za_gauntlet_record_outcome("", true); }
void za_gauntlet_debug_loss(void) { za_gauntlet_record_outcome("", false); }
void za_gauntlet_debug_night(void) { za_gauntlet_night_begin(); }
