// Z-A Royale randomizer (Pokémon Z-A): the night-time Battle Zone trainers change every load, so each night is
// different, instead of a mod's fixed table (a trdata_array.bin in mods/ is the same every night).
// The game keeps its trainer table Oodle-compressed in arc/data.trpfs, so there is no loose file to roll: it needs a mod
// holding the game's own table unpacked, mods/<any name>/romfs/world/ik_data/trainer/trdata/trdata_array.bin
// (tools/za/trdata_extract.py makes it from the player's files). runtime/services/fs.c serves a mod's file instead of
// the packed one (trpfd_unlist) and hands each read of it here (za_royale_filter): a read from the file's first byte
// rolls the `za_rank_*` (and optionally the Infinite pools `za_rank_inf*` / `za_inf_*` / `dim_rank_*`) trainers in a
// copy, which the game then reads instead. (The first version, 2026-10-29, looked for the file among the RomFS's
// loose files - it isn't one - and never rolled.) The trainer's identity (trid / trtype / zaRank), where it stands,
// and every other trainer are untouched, so Battle Zone spawns, dialogue and promotion matches keep working; party
// sizes never change.
//
// This prefix set is confirmed complete, not guessed: world/ik_data/field/npc/za_trainer_data/za_trainer_data_array
// (each placed trainer's world spawn id -> its trdata trid) lists every trainerId the game actually places, and all
// but 2 of its 2538 entries (two story-chapter battles) fall under these four prefixes: 228 `za_rank_*`, 160
// `za_rank_inf*`, 2148 `dim_rank_*` (the Dimension postgame zones), 0 `za_inf_*` (harmless no-op here).
//
// Only three fields are stored per Royale Pokémon - devId (species), level and seikaku (nature); moves are absent
// (= the game's own default moveset for the species), and so are formId / sex / item / tokusei (tokusei's default is
// RANDOM_12, a random ability), so in-place writes of those three are enough and the FlatBuffer stays valid. The
// species pool is the set the night trainers already use, so every pick is a real, battle-capable Z-A species.
//
//   LARECOMP_ROYALE_RANDOM=1   force on (overrides the setting at load); the F1 menu's "Z-A Royale randomizer" does it
//   LARECOMP_ROYALE_DELTA=<n>  level +/- around the trainer's own level (default 5)
//   LARECOMP_ROYALE_INFINITE=0 also leave the Infinite Royale pools alone (default: they are rolled too)
//   LARECOMP_ROYALE_SEED=<n>   make the rolls repeatable (default: the save's night, below)
//
// One roll per in-game night, not per load: a fresh seed at every start let a player quit and reload until the
// matchups suited them. The seed is the save's night (save_night below: its day counter and time of day, from the
// save the game loads - save/save_0/main, read at the roll, which comes after the save is mounted), so reloading
// within a night rolls the same teams and the next night rolls new ones. The table is read once per start: a night
// that begins while playing takes its teams at the next start. A save that can't be read: a fresh seed, as before.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

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
// (every read in bounds, and 4 bytes after the field - the widest one read or written here: a damaged table, or a mod's,
// can't make a roll touch bytes past the buffer; a field in a file's last 1-3 bytes is just left as it is)
static uint32_t tab_field(const uint8_t *b, size_t sz, uint32_t p, int id) {
    if (!in(sz, p, 4)) return 0;
    int32_t so = rd_i32(b + p);
    uint32_t vt = (uint32_t)((int64_t)p - so);
    if (!in(sz, vt, 4)) return 0;
    uint16_t vsize = rd_u16(b + vt);
    if (4u + 2u * (uint32_t)id + 2u > vsize || !in(sz, (uint64_t)vt + 4 + 2 * id, 2)) return 0;
    uint16_t o = rd_u16(b + vt + 4 + 2 * id);
    return o && in(sz, (uint64_t)p + o, 4) ? p + o : 0;
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

// The species pool, with the lowest and highest level the game's own night trainers use each one at
static uint16_t pool[1024];
static int16_t pool_lo[1024], pool_hi[1024];
static int npool;
static void pool_add(uint16_t v, int lv) {
    for (int i = 0; i < npool; i++)
        if (pool[i] == v) {
            if (lv < pool_lo[i]) pool_lo[i] = (int16_t)lv;
            if (lv > pool_hi[i]) pool_hi[i] = (int16_t)lv;
            return;
        }
    if (npool < 1024) pool[npool] = v, pool_lo[npool] = pool_hi[npool] = (int16_t)lv, npool++;
}
// A species fit for level `lv`: one the game's night trainers field within a few levels of it (no fully evolved
// Pokémon on a level-6 rank Z trainer, no early ones at 70). None that close: the nearest by level.
static uint16_t pool_pick(int lv) {
    enum { SLACK = 4 };
    static int cand[1024];
    int nc = 0, best = 0, bestd = 1 << 30;
    for (int i = 0; i < npool; i++) {
        int d = lv < pool_lo[i] ? pool_lo[i] - lv : lv > pool_hi[i] ? lv - pool_hi[i] : 0;
        if (d <= SLACK) cand[nc++] = i;
        if (d < bestd) bestd = d, best = i;
    }
    return pool[nc ? cand[rnd_below(nc)] : best];
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

    // Pass 1: the species pool - what the night trainers themselves use (so every pick is valid and battle-ready), and at
    // which levels (pool_pick: a rolled Pokémon is one fit for its level)
    npool = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = elems + 4 * i;
        uint32_t tr = e + rd_u32(b + e);
        char trid[64];
        if (!tr_trid(b, sz, tr, trid, sizeof trid) || !night(trid, inf)) continue;
        for (int s = 0; s < 6; s++) {
            uint32_t pk = poke(b, sz, tr, s);
            if (!pk) continue;
            uint32_t df = tab_field(b, sz, pk, F_DEVID), lf = tab_field(b, sz, pk, F_LEVEL);
            if (df && lf) { uint16_t dv = rd_u16(b + df); if (dv) pool_add(dv, rd_i32(b + lf)); }
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
        // Never much stronger than the game's own trainer: down by up to delta, up by at most a tenth of its level (+1),
        // within delta - a level-6 rank Z trainer fields 4..8, not 11; a level-50 one 45..55
        int up = hi / 10 + 1 < delta ? hi / 10 + 1 : delta;
        int a = lo - delta < 1 ? 1 : lo - delta, z = hi + up > 100 ? 100 : hi + up;
        nt++;
        for (int s = 0; s < 6; s++) {
            uint32_t pk = poke(b, sz, tr, s);
            if (!pk) continue;
            uint32_t df = tab_field(b, sz, pk, F_DEVID);
            if (!df || !rd_u16(b + df)) continue;  // empty slot: leave
            // the level first (within delta of this trainer's own), then a species fit for it
            uint32_t lf = tab_field(b, sz, pk, F_LEVEL);
            int32_t lv = lf ? a + rnd_below(z - a + 1) : lo;
            if (lf) memcpy(b + lf, &lv, 4);
            uint16_t dv = pool_pick(lv);
            memcpy(b + df, &dv, 2);
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

#ifndef ROYALE_RANDOM_TEST
// The save's night: Z-A's save is SwishCrypto (PKHeX SwishCrypto.cs / SCBlock.cs): the file less its 32-byte hash,
// xored with a repeating 0x7F-byte pad, is a run of blocks - u32 key, then a type byte and the block's bytes xored
// with an xorshift32 stream seeded by the key. Two blocks: 0x85DBDCE9 (an object; its first u32 the in-game day
// counter - 9 -> 11 across two in-game days on a test save) and 0x7B70DA66 (f32: seconds past in-game midnight).
// A night runs from evening past midnight to morning, never through noon: the days counted from noon to noon give
// each night one number. Returns 0 when the save can't be read.
static const uint8_t swish_pad[0x7F] = {
    0xA0, 0x92, 0xD1, 0x06, 0x07, 0xDB, 0x32, 0xA1, 0xAE, 0x01, 0xF5, 0xC5, 0x1E, 0x84, 0x4F, 0xE3, 0x53, 0xCA, 0x37, 0xF4,
    0xA7, 0xB0, 0x4D, 0xA0, 0x18, 0xB7, 0xC2, 0x97, 0xDA, 0x5F, 0x53, 0x2B, 0x75, 0xFA, 0x48, 0x16, 0xF8, 0xD4, 0x8A, 0x6F,
    0x61, 0x05, 0xF4, 0xE2, 0xFD, 0x04, 0xB5, 0xA3, 0x0F, 0xFC, 0x44, 0x92, 0xCB, 0x32, 0xE6, 0x1B, 0xB9, 0xB1, 0x2E, 0x01,
    0xB0, 0x56, 0x53, 0x36, 0xD2, 0xD1, 0x50, 0x3D, 0xDE, 0x5B, 0x2E, 0x0E, 0x52, 0xFD, 0xDF, 0x2F, 0x7B, 0xCA, 0x63, 0x50,
    0xA4, 0x67, 0x5D, 0x23, 0x17, 0xC0, 0x52, 0xE1, 0xA6, 0x30, 0x7C, 0x2B, 0xB6, 0x70, 0x36, 0x5B, 0x2A, 0x27, 0x69, 0x33,
    0xF5, 0x63, 0x7B, 0x36, 0x3F, 0x26, 0x9B, 0xA3, 0xED, 0x7A, 0x53, 0x00, 0xA4, 0x48, 0xB3, 0x50, 0x9E, 0x14, 0xA0, 0x52,
    0xDE, 0x7E, 0x10, 0x2B, 0x1B, 0x77, 0x6E};
typedef struct { uint32_t s; int c; } SwXs;
static uint32_t swxs_adv(uint32_t s) { s ^= s << 2; s ^= s >> 15; s ^= s << 13; return s; }
static SwXs swxs_init(uint32_t key) {
    SwXs x = {key, 0};
    for (int i = __builtin_popcount(key); i > 0; i--) x.s = swxs_adv(x.s);
    return x;
}
static uint8_t swxs_next(SwXs *x) {
    uint8_t r = (uint8_t)(x->s >> (x->c * 8));
    if (x->c == 3) x->s = swxs_adv(x->s), x->c = 0;
    else x->c++;
    return r;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// Block `key`'s first `n` bytes (decrypted) into out: false if absent
static bool swish_block(const uint8_t *d, size_t len, uint32_t want, uint8_t *out, size_t n) {
    static const uint8_t tsize[18] = {[3] = 1, [8] = 1, [9] = 2, [10] = 4, [11] = 8, [12] = 1, [13] = 2, [14] = 4, [15] = 8, [16] = 4, [17] = 8};
    size_t o = 0;
    while (o + 5 <= len) {
        uint32_t key = rd32(d + o);
        o += 4;
        SwXs x = swxs_init(key);
        uint8_t t = d[o++] ^ swxs_next(&x);
        uint64_t sz;
        if (t >= 1 && t <= 3) sz = 0;
        else if (t == 4 || t == 5) {
            if (o + 4 > len) return false;
            uint8_t b[4];
            for (int i = 0; i < 4; i++) b[i] = d[o + i] ^ swxs_next(&x);
            o += 4;
            sz = rd32(b);
            if (t == 5) {
                if (o >= len) return false;
                uint8_t sub = d[o++] ^ swxs_next(&x);
                if (sub >= 18 || !tsize[sub]) return false;
                sz *= tsize[sub];
            }
        } else if (t < 18 && tsize[t]) sz = tsize[t];
        else return false;  // (not a block: not this format)
        if (sz > len - o) return false;
        if (key == want) {
            if (sz < n) return false;
            for (size_t i = 0; i < n; i++) out[i] = d[o + i] ^ swxs_next(&x);
            return true;
        }
        o += (size_t)sz;
    }
    return false;
}
static uint32_t save_night(char *why, size_t whyn) {
    extern const char *g_save_dir;  // (services/fs.c: "save", or the test runs' scratch copy)
    char path[1200];
    for (int f = 0; f < 2; f++) {
        snprintf(path, sizeof path, "%s/save_0/%s", g_save_dir, f ? "backup" : "main");
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0) continue;
        uint8_t *d = NULL;
        size_t len = 0;
        if (!fstat(fd, &st) && st.st_size > 64 && st.st_size <= (64 << 20) && (d = malloc((size_t)st.st_size))) {
            len = (size_t)st.st_size - 32;  // (less the hash)
            if (pread(fd, d, (size_t)st.st_size, 0) != st.st_size) len = 0;
        }
        close(fd);
        for (size_t i = 0; i < len; i++) d[i] ^= swish_pad[i % 0x7F];
        uint8_t day[4], tod[4];
        bool ok = len && swish_block(d, len, 0x85DBDCE9u, day, 4) && swish_block(d, len, 0x7B70DA66u, tod, 4);
        free(d);
        if (!ok) continue;
        float secs;
        uint32_t t = rd32(tod);
        memcpy(&secs, &t, 4);
        uint32_t n = rd32(day) * 2 + (secs >= 12 * 3600.0f ? 1 : 0);  // (noon to noon: (day - 1) PM and day AM -> one)
        snprintf(why, whyn, "%s: day %u, %02d:%02d -> night %u", path, rd32(day), (int)(secs / 3600) % 24, (int)(secs / 60) % 60,
                 (n + 1) / 2);
        return (n + 1) / 2 + 1;  // (+1: never 0)
    }
    snprintf(why, whyn, "%s/save_0: no readable save", g_save_dir);
    return 0;
}
#endif

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
        char why[1400];
        uint32_t night = 0;
        if (e && *e) seed = (uint32_t)strtoul(e, NULL, 0);
        else if ((night = save_night(why, sizeof why))) {  // (the night's own roll: reloading it keeps the teams)
            uint64_t t = night * 0x9e3779b97f4a7c15ull + 0x5a52;  // (+ a constant: night n's roll is no other mode's seed n)
            t ^= t >> 33; t *= 0xff51afd7ed558ccdull; t ^= t >> 33;
            seed = (uint32_t)t;
            LOG("[royalerandom] %s\n", why);
        } else {
            LOG("[royalerandom] %s: a fresh roll\n", why);
            uint64_t t = plat_now_ns(); t ^= t >> 33; t *= 0xff51afd7ed558ccdull; t ^= t >> 33; seed = (uint32_t)t;
        }
    }
#endif
    if (delta < 0) delta = 0;
    if (delta > 50) delta = 50;
    apply(buf, size, inf, delta, seed);
}

#ifndef ROYALE_RANDOM_TEST
// fs.c (romfs_read): `n` bytes of the mod file `host` from file offset `off` were just read into `dst`. The trainer
// table: this load's roll instead (rolled again by each read from the first byte; mods off or the setting off: as read).
static pthread_mutex_t r_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *r_buf;
static size_t r_size;
static char r_host[1200];
RT_ON_RESET(royale_reset) {
    pthread_mutex_lock(&r_lock);
    free(r_buf), r_buf = NULL, r_size = 0, r_host[0] = 0;
    pthread_mutex_unlock(&r_lock);
}
void za_royale_filter(const char *host, uint64_t off, uint8_t *dst, uint64_t n) {
    static const char tail[] = "/romfs/world/ik_data/trainer/trdata/trdata_array.bin";
    size_t hl = host ? strlen(host) : 0;
    if (!n || hl < sizeof tail - 1 || strcmp(host + hl - (sizeof tail - 1), tail) || !za_royale_random_on()) return;
    pthread_mutex_lock(&r_lock);
    if (off == 0 || !r_buf || strcmp(r_host, host)) {
        free(r_buf), r_buf = NULL, r_size = 0;
        int fd = open(host, O_RDONLY);
        struct stat st;
        if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0 && st.st_size <= (64 << 20)) {  // (the real one: a few MB)
            r_buf = malloc((size_t)st.st_size);
            if (r_buf && pread(fd, r_buf, (size_t)st.st_size, 0) == st.st_size) {
                r_size = (size_t)st.st_size;
                za_royale_random_roll(r_buf, r_size);
            } else {
                free(r_buf), r_buf = NULL;
            }
        }
        if (fd >= 0) close(fd);
        snprintf(r_host, sizeof r_host, "%s", host);
        if (!r_buf) LOG("[royalerandom] %s couldn't be read: the mod's own table is used\n", host);
    }
    if (r_buf && off < r_size) memcpy(dst, r_buf + off, (size_t)(n < r_size - off ? n : r_size - off));
    pthread_mutex_unlock(&r_lock);
}
#endif
