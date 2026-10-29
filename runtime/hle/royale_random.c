// Z-A Royale randomizer (Pokémon Z-A): the night-time Battle Zone trainers change every load, so each night is
// different, instead of a mod's fixed table (mods/…/trdata_array.bin is replaced once and never rolls again).
// runtime/services/fs.c hands us the game's own world/ik_data/trainer/trdata/trdata_array.bin bytes the moment a
// load reads its first byte (the same first-byte trigger the LAS enemy randomizer uses, runtime/hle/enemy_random.c):
// we roll the `za_rank_*` (and optionally the Infinite pools `za_rank_inf*` / `za_inf_*` / `dim_rank_*`) trainers in
// the buffer before the game parses it. The trainer's identity (trid / trtype / zaRank) and every other trainer are
// untouched, so Battle Zone spawns, dialogue and promotion matches keep working; party sizes never change.
//
// Only three fields are stored per Royale Pokémon - devId (species), level and seikaku (nature); moves are absent
// (= the game's own default moveset for the species), and so are formId / sex / item / tokusei (tokusei's default is
// RANDOM_12, a random ability), so in-place writes of those three are enough and the FlatBuffer stays valid. The
// species pool is the set the night trainers already use, so every pick is a real, battle-capable Z-A species.
//
//   LARECOMP_ROYALE_RANDOM=1   force on (overrides the setting at load); the F1 menu's "Z-A Royale randomizer" does it
//   LARECOMP_ROYALE_DELTA=<n>  level +/- around the trainer's own level (default 5)
//   LARECOMP_ROYALE_INFINITE=0 also leave the Infinite Royale pools alone (default: they are rolled too)
//   LARECOMP_ROYALE_SEED=<n>   make the rolls repeatable (default: a fresh seed per load)
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifndef ROYALE_RANDOM_TEST
#include "../hle.h"
#include "../settings.h"
#include "../platform.h"
#else
#define ENV(n) getenv(n)
#define LOG(...) do {} while (0)
#endif

// trainer.TrdataMainArray (root): id 0 = values (vector of tables). trainer.TrdataMain: id 0 = trid, id 7..12 = poke1..6.
// PokeDataBattle: id 0 = devId (UShort), 4 = level (Int), 10 = seikaku (Int), 1 = formId (Short), 2 = sex (Int),
// 3 = item (Int), 14 = rareType (Int), 15 = scaleValue (Short). WazaSet: id 0 = wazaId (UShort).
enum { F_VALUES = 0, F_TRID = 0, F_POKE1 = 7, F_DEVID = 0, F_FORMID = 1, F_SEX = 2, F_ITEM = 3, F_LEVEL = 4,
       F_SEIKAKU = 10, F_RARETYPE = 14, F_SCALEVALUE = 15 };
enum { RARE_NO_RARE = 1, SCALE_MID = 128 };

static uint64_t g_rng;
static uint64_t rnd(void) {  // splitmix64 (as enemy_random.c)
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static int rnd_below(int n) { return n > 0 ? (int)(rnd() % (uint64_t)n) : 0; }

static int in(size_t sz, uint64_t p, uint64_t n) { return p <= sz && sz - p >= n; }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int32_t rd_i32(const uint8_t *p) {
    return (int32_t)(uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

// The field's byte offset in table `p` (vtable slot `id`), or 0 when the field is absent (its default applies).
static uint32_t tab_field(const uint8_t *b, size_t sz, uint32_t p, int id) {
    if (!in(sz, p, 4)) return 0;
    int32_t so = rd_i32(b + p);
    uint32_t vt = (uint32_t)((int64_t)p - so);
    if (!in(sz, vt, 4)) return 0;
    uint16_t vsize = rd_u16(b + vt);
    if (4u + 2u * (uint32_t)id + 2u > vsize) return 0;
    uint16_t o = rd_u16(b + vt + 4 + 2 * id);
    return o && in(sz, p + o, 1) ? p + o : 0;
}

static int night(const char *trid, int inf) {
    if (!strncmp(trid, "za_rank_", 8)) {
        if (!inf && !strncmp(trid, "za_rank_inf", 11)) return 0;  // the Infinite Royale tier, unless asked
        return 1;
    }
    return inf && (!strncmp(trid, "za_inf_", 7) || !strncmp(trid, "dim_rank_", 9));
}

// trid of a trainer table into `out` (NUL-terminated) - 0 when absent/unreadable.
static int tr_trid(const uint8_t *b, size_t sz, uint32_t tr, char *out, size_t cap) {
    uint32_t f = tab_field(b, sz, tr, F_TRID);
    if (!f || !in(sz, f, 4)) return 0;
    uint32_t s = f + rd_u32(b + f);
    if (!in(sz, s, 4)) return 0;
    uint32_t len = rd_u32(b + s);
    if (!in(sz, s + 4, len) || len >= cap) return 0;
    memcpy(out, b + s + 4, len);
    out[len] = 0;
    return 1;
}

// A trainer's poke table (slot 0..5), or 0.
static uint32_t poke(const uint8_t *b, size_t sz, uint32_t tr, int slot) {
    uint32_t f = tab_field(b, sz, tr, F_POKE1 + slot);
    if (!f || !in(sz, f, 4)) return 0;
    uint32_t pk = f + rd_u32(b + f);
    return in(sz, pk, 4) ? pk : 0;
}

static void add_uniq16(uint16_t *a, int *n, int cap, uint16_t v) {
    for (int i = 0; i < *n; i++) if (a[i] == v) return;
    if (*n < cap) a[(*n)++] = v;
}

static void apply(uint8_t *b, size_t sz, int inf, int delta, uint32_t seed) {
    g_rng = seed ? seed : 1;
    if (!in(sz, 0, 4)) return;
    uint32_t root = rd_u32(b);
    uint32_t f0 = tab_field(b, sz, root, F_VALUES);
    if (!f0) return;
    uint32_t vp = f0 + rd_u32(b + f0);
    if (!in(sz, vp, 4)) return;
    uint32_t n = rd_u32(b + vp);
    uint32_t elems = vp + 4;
    if (!in(sz, elems, 4ull * n)) return;

    // Pass 1: the species pool - what the night trainers themselves use (so every pick is valid and battle-ready).
    static uint16_t pool[1024];
    int npool = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = elems + 4 * i;
        uint32_t tr = e + rd_u32(b + e);
        char trid[64];
        if (!tr_trid(b, sz, tr, trid, sizeof trid) || !night(trid, inf)) continue;
        for (int s = 0; s < 6; s++) {
            uint32_t pk = poke(b, sz, tr, s);
            if (!pk) continue;
            uint32_t df = tab_field(b, sz, pk, F_DEVID);
            if (df && in(sz, df, 2)) { uint16_t dv = rd_u16(b + df); if (dv) add_uniq16(pool, &npool, 1024, dv); }
        }
    }
    if (!npool) return;

    // Pass 2: roll each night trainer's Pokemon in place.
    int nt = 0, npk = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = elems + 4 * i;
        uint32_t tr = e + rd_u32(b + e);
        char trid[64];
        if (!tr_trid(b, sz, tr, trid, sizeof trid) || !night(trid, inf)) continue;
        int lo = 0, hi = 0;
        for (int s = 0; s < 6; s++) {
            uint32_t pk = poke(b, sz, tr, s);
            if (!pk) continue;
            uint32_t df = tab_field(b, sz, pk, F_DEVID), lf = tab_field(b, sz, pk, F_LEVEL);
            if (!df || !lf || !rd_u16(b + df)) continue;
            int lv = rd_i32(b + lf);
            if (!lo || lv < lo) lo = lv;
            if (lv > hi) hi = lv;
        }
        if (!lo) lo = hi = 1;
        int a = lo - delta < 1 ? 1 : lo - delta, z = hi + delta > 100 ? 100 : hi + delta;
        nt++;
        for (int s = 0; s < 6; s++) {
            uint32_t pk = poke(b, sz, tr, s);
            if (!pk) continue;
            uint32_t df = tab_field(b, sz, pk, F_DEVID);
            if (!df || !rd_u16(b + df)) continue;  // empty slot: leave
            uint16_t dv = pool[rnd_below(npool)];
            memcpy(b + df, &dv, 2);
            uint32_t lf = tab_field(b, sz, pk, F_LEVEL);
            if (lf && in(sz, lf, 4)) { int32_t lv = a + rnd_below(z - a + 1); memcpy(b + lf, &lv, 4); }
            uint32_t sf = tab_field(b, sz, pk, F_SEIKAKU);
            if (sf && in(sz, sf, 4)) { int32_t sv = 1 + rnd_below(25); memcpy(b + sf, &sv, 4); }  // natures 1..25
            uint32_t rf = tab_field(b, sz, pk, F_RARETYPE);   // (already NO_RARE, kept for a table that has it)
            if (rf && in(sz, rf, 4)) { int32_t v = RARE_NO_RARE; memcpy(b + rf, &v, 4); }
            uint32_t cf = tab_field(b, sz, pk, F_SCALEVALUE);
            if (cf && in(sz, cf, 2)) { uint16_t v = SCALE_MID; memcpy(b + cf, &v, 2); }
            npk++;
        }
    }
    LOG("[royalerandom] seed %u: %d night trainers, %d Pokemon rolled (delta %d, infinite %d)\n",
        seed, nt, npk, delta, inf);
    (void)nt, (void)npk;
}

int za_royale_random_on(void) {
#ifdef ROYALE_RANDOM_TEST
    return 0;
#else
    const char *e = ENV("LARECOMP_ROYALE_RANDOM");
    if (e) return atoi(e) != 0;
    return g_settings.za_royale_random != 0;
#endif
}

// Roll `buf` (a whole trdata_array.bin) for one load. Reads the knobs fresh each call, so the menu takes effect.
void za_royale_random_roll(uint8_t *buf, size_t size) {
    int inf = 1, delta = 5;
    uint32_t seed;
#ifdef ROYALE_RANDOM_TEST
    const char *e = getenv("LARECOMP_ROYALE_INFINITE");
    if (e) inf = atoi(e) != 0;
    e = getenv("LARECOMP_ROYALE_DELTA");
    if (e) delta = atoi(e);
    e = getenv("LARECOMP_ROYALE_SEED");
    seed = e && *e ? (uint32_t)strtoul(e, NULL, 0) : 1234;
#else
    {
        const char *e = ENV("LARECOMP_ROYALE_INFINITE");
        if (e) inf = atoi(e) != 0;
        e = ENV("LARECOMP_ROYALE_DELTA");
        delta = e ? atoi(e) : g_settings.za_royale_delta;
        e = ENV("LARECOMP_ROYALE_SEED");
        if (e && *e) seed = (uint32_t)strtoul(e, NULL, 0);
        else { uint64_t t = plat_now_ns(); t ^= t >> 33; t *= 0xff51afd7ed558ccdull; t ^= t >> 33; seed = (uint32_t)t; }
    }
#endif
    if (delta < 0) delta = 0;
    if (delta > 50) delta = 50;
    apply(buf, size, inf, delta, seed);
}
