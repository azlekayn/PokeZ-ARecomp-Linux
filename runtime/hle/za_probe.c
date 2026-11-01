// Pokémon Z-A: the player probe (research; LARECOMP_PLAYER_PROBE=1, off by default). On the Steam Deck the player
// sometimes stays "stuck": turning and the menu still work, but roll, crouch and partner commands are all refused -
// an action state that never ends (2026-11-01, with the frame-step decoupling on: framepace.c rt_za_frame_step, a
// suspect, since below 60 fps it hands some subsystems two ticks in a frame where the game only ever handed one; it
// seems to follow a roll while the ground height changes - off a ledge, down a slope).
// This reads guest memory from the outside and never writes it or hooks game code: no game-code rebuild for anyone.
//
// Once a frame (the game thread, rt_za_frame_dt's call) it follows the player chain and logs the watched fields on
// change, with the frame-step context (frames among the last 60 that handed out a 2-tick step). The chain:
//   LARECOMP_PLAYER_CHAIN=<a>,<o1>,<o2>...  p = a; then for each offset p = u64 [p] + o  (hex). Unset: the handle
//   at 0x70061302c0 through the engine's handle table (f_70006fdec0; opencode/player-state-re.md) - which stayed 0
//   in the overworld on the Deck (b71), so the chain is to be found with the commands below.
//   LARECOMP_PLAYER_WATCH=<off>:<size>,...  the fields (default 650:4,660:1,665:1,6b0:1).
//
// Commands, for finding the chain over ssh while the game runs: write lines to the file `player_cmd` in the data
// root (it is deleted once read; checked every 30 frames). Each runs on its own thread (not the game's: a scan takes
// seconds) and writes logs/player_out_<n>.txt, with a summary line in the log:
//   dump                    the chain's object (0x1200 bytes), the last 240 frames' steps
//   mem <addr> <len>        a hex dump
//   scan <u64>              every 8-aligned copy of the value in writable guest memory (a name hash: f_7000350224's
//                           IsFalling 03c072cd0b412958, IsRotomGliding 06c1639799f12fc4)
//   ptr <lo> <hi>           every 8-aligned word holding an address in [lo, hi): who points at a found object
//   snap <addr> <len>       keep a copy; `diff` then lists the 32-bit words that changed since (normal vs stuck)
//   chain <a>,<o1>...       / watch <off>:<size>,...: as the variables above, from now on
//   regions                 the writable guest regions
//   near <x> <y> <z> <r>    (decimal) every float triple within r of the point: a position's copies (the camera's)
//   track <frames> <a>[+n],...  per frame for that many frames: its interval, each clock channel's step and n floats
//                           (default 3) at each address -> logs/track_<k>.txt (the judder: does the camera move by the
//                           frame's ticks as the player does?)
// Reads go through process_vm_readv on Linux (a page unmapped mid-scan is an error there, not a crash); elsewhere
// only addresses inside a writable guest region are read.
// Only the 2.0.2 build is known; the probe stays off unless the frame-step site ran (za_patches.py patches it on
// 2.0.2 only), so another build's different layout is never read by the default chain.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (process_vm_readv)
#endif
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "../hle.h"
#include "../platform.h"

extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);  // (runtime/kernel.c)

#define PROBE_HANDLE 0x70061302c0ull
#define PROBE_TABLE 0x7003ec21c8ull
#define OBJ_BYTES 0x1200u

static bool seen_step_site;               // rt_za_frame_step ran: this is the 2.0.2 build whose addresses these are
static uint32_t frame_steps, frame_calls;  // (the game thread) this frame's largest step handed out, subsystem calls
static uint8_t step_hist[240];            // the last 240 frames' largest step (0 = one tick, 1 = two...)
static uint32_t hist_pos;
static uint64_t frame_no;

static uint8_t chan_steps[16];  // (the game thread) this frame's step per clock channel, in call order
void za_probe_step(uint32_t game_step, uint32_t out_step) {
    (void)game_step;
    seen_step_site = true;
    if (out_step > frame_steps) frame_steps = out_step;
    if (frame_calls < 16) chan_steps[frame_calls] = (uint8_t)(out_step > 9 ? 9 : out_step);
    frame_calls++;
}

// ---- safe reads
static uint64_t reg_a[256], reg_s[256];
static int nreg;
static bool __attribute__((unused)) in_regions(uint64_t a, uint64_t n) {
    for (int i = 0; i < nreg; i++)  // (subtractive: an address near the top doesn't wrap past the test)
        if (a >= reg_a[i] && a - reg_a[i] <= reg_s[i] && n <= reg_s[i] - (a - reg_a[i])) return true;
    return false;
}
static bool rd(uint64_t a, void *out, size_t n) {
#ifdef __linux__
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#else
    if (!in_regions(a, n)) return false;
    memcpy(out, (const void *)(uintptr_t)a, n);
    return true;
#endif
}
static uint64_t rd64(uint64_t a, bool *ok) {
    uint64_t v = 0;
    if (!rd(a, &v, 8)) *ok = false;
    return v;
}

// ---- the chain and the watched fields (set by the game thread from the commands: plain copies, read there only)
static uint64_t chain[16];
static int nchain = -1;  // -1: the handle-table default
static struct { uint32_t off, size; } watch[16];
static int nwatch;

static int parse_list(const char *s, uint64_t *out, int max) {
    int n = 0;
    while (s && *s && n < max) {
        char *end;
        out[n++] = strtoull(s, &end, 16);
        if (end == s) return n - 1;
        s = *end ? end + 1 : end;
    }
    return n;
}
static void set_watch(const char *s) {
    nwatch = 0;
    while (s && *s && nwatch < 16) {
        char *end;
        uint32_t off = (uint32_t)strtoul(s, &end, 16), size = 4;
        if (end == s) break;
        if (*end == ':') size = (uint32_t)strtoul(end + 1, &end, 10);
        if (size != 1 && size != 2 && size != 4 && size != 8) size = 4;
        watch[nwatch].off = off, watch[nwatch].size = size, nwatch++;
        s = *end ? end + 1 : end;
    }
}

// `track <frames> <addr>[+<count>],...`: for that many frames, one line a frame in logs/track_<n>.txt - the frame's
// real interval, each clock channel's step, and <count> (default 3) floats at each address (read at the frame clock,
// the start of the game's frame). Whether the camera moves by the frame's ticks as the player does is in there.
static struct { uint64_t a; uint32_t n; } trk[8];
static int ntrk;
static uint32_t trk_left;
static FILE *trk_f;
static void track_start(const char *s) {
    char *end;
    uint32_t frames = (uint32_t)strtoul(s, &end, 10);
    ntrk = 0;
    for (s = end; *s == ' '; s++) {}
    while (*s && ntrk < 8) {
        uint64_t a = strtoull(s, &end, 16);
        if (end == s) break;
        uint32_t n = 3;
        if (*end == '+') n = (uint32_t)strtoul(end + 1, &end, 10);
        trk[ntrk].a = a, trk[ntrk].n = n > 16 ? 16 : n ? n : 3, ntrk++;
        s = *end ? end + 1 : end;
    }
    if (trk_f) fclose(trk_f), trk_f = NULL;
    static int k;
    char path[64];
    snprintf(path, sizeof path, "logs/track_%d.txt", ++k);
    mkdir("logs", 0755);
    trk_f = ntrk && frames ? fopen(path, "w") : NULL;
    trk_left = trk_f ? (frames > 36000 ? 36000 : frames) : 0;
    LOG("[player] track: %u frames, %d addresses -> %s\n", trk_left, ntrk, trk_f ? path : "(not started)");
    if (trk_f) fprintf(trk_f, "# frame dt_ms p<presents so far> steps(per channel, 0 = one tick) | floats per address\n");
}
static void track_frame(uint32_t calls) {
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (!trk_left || !trk_f) { last = now; return; }
    extern uint64_t renderer_present_count(void);  // (the backend's presents so far: frame clock vs present pairing)
    fprintf(trk_f, "%llu %.2f p%llu ", (unsigned long long)frame_no, last ? (now - last) / 1e6 : 0.0,
            (unsigned long long)renderer_present_count());
    last = now;
    for (uint32_t i = 0; i < calls && i < 16; i++) fputc('0' + chan_steps[i], trk_f);
    fputs(" |", trk_f);
    for (int t = 0; t < ntrk; t++) {
        float v[16];
        if (!rd(trk[t].a, v, trk[t].n * 4)) { fputs(" ?", trk_f); continue; }
        for (uint32_t i = 0; i < trk[t].n; i++) fprintf(trk_f, " %.4f", v[i]);
        if (t + 1 < ntrk) fputs(" ;", trk_f);
    }
    fputc('\n', trk_f);
    if (!--trk_left) { fclose(trk_f), trk_f = NULL; LOG("[player] track done\n"); }
}

static uint64_t resolve(uint64_t *handle_out) {
    *handle_out = 0;
    bool ok = true;
    if (nchain >= 0) {
        uint64_t p = chain[0];
        for (int i = 1; i < nchain && ok; i++) p = rd64(p, &ok) + chain[i];
        return ok ? p : 0;
    }
    uint64_t h = rd64(PROBE_HANDLE, &ok);
    *handle_out = h;
    if (!ok || !h) return 0;
    uint64_t table = rd64(PROBE_TABLE, &ok);
    uint64_t e = table + (h & 0xfffffull) * 0x20;
    if (!ok || !table || rd64(e + 0x10, &ok) != h) return 0;
    uint32_t st = 0;
    uint16_t strong = 0;
    if (!rd(e + 0x2c, &st, 4) || !rd(e + 0x2a, &strong, 2) || (int32_t)st == -1 || !strong) return 0;
    uint64_t p = rd64(e + 0x20, &ok);
    return ok && p ? p - 0x28 : 0;
}

// ---- commands (on their own thread)
static void hexdump(FILE *f, uint64_t base, uint32_t n) {
    uint8_t row[16];
    for (uint32_t o = 0; o < n; o += 16) {
        if (!rd(base + o, row, 16)) { fprintf(f, "+%04x: (unreadable)\n", o); continue; }
        fprintf(f, "+%04x:", o);
        for (int i = 0; i < 16; i += 4) fprintf(f, " %08x", *(uint32_t *)(row + i));
        fprintf(f, "\n");
    }
}

static uint8_t *snap_buf;
static uint64_t snap_addr, snap_len;
static volatile int cmd_busy;
static char cmd_text[4096];
static uint64_t cmd_player;  // the chain's object when the command was read

// Every 8-aligned word of the writable regions, in 1 MB chunks: fn(addr, value) per word; stops after max hits.
static int scan_words(FILE *f, bool (*match)(uint64_t v, const uint64_t *arg), const uint64_t *arg, int max) {
    static uint64_t buf[1 << 17];
    uint64_t ra[256], rs[256];
    int nr = kernel_data_regions(ra, rs, 256), hits = 0;
    for (int r = 0; r < nr && hits < max; r++)
        for (uint64_t a = ra[r]; a < ra[r] + rs[r] && hits < max; a += sizeof buf) {
            uint64_t n = ra[r] + rs[r] - a < sizeof buf ? ra[r] + rs[r] - a : sizeof buf;
            if (!rd(a, buf, n)) continue;
            for (uint64_t i = 0; i < n / 8 && hits < max; i++)
                if (match(buf[i], arg)) { fprintf(f, "%#llx: %016llx\n", (unsigned long long)(a + i * 8),
                                                  (unsigned long long)buf[i]); hits++; }
        }
    return hits;
}
static bool match_eq(uint64_t v, const uint64_t *arg) { return v == arg[0]; }
// `near x y z r`: every 4-aligned float triple within r of (x, y, z) (each axis; finite values only) - the copies of a
// position: the player's, the camera's target, the camera (a few metres off), render copies...
static int scan_near(FILE *f, const float *c, float r, int max) {
    static float buf[1 << 18];
    uint64_t ra[256], rs[256];
    int nr = kernel_data_regions(ra, rs, 256), hits = 0;
    for (int k = 0; k < nr && hits < max; k++)
        for (uint64_t a = ra[k]; a < ra[k] + rs[k] && hits < max; a += sizeof buf - 8) {
            uint64_t n = ra[k] + rs[k] - a < sizeof buf ? ra[k] + rs[k] - a : sizeof buf;
            if (n < 12 || !rd(a, buf, n)) continue;
            for (uint64_t i = 0; i + 2 < n / 4 && hits < max; i++) {
                float x = buf[i], y = buf[i + 1], z = buf[i + 2];
                if (x - c[0] <= r && c[0] - x <= r && y - c[1] <= r && c[1] - y <= r && z - c[2] <= r && c[2] - z <= r) {
                    fprintf(f, "%#llx: %.3f %.3f %.3f\n", (unsigned long long)(a + i * 4), x, y, z);
                    hits++, i += 2;
                }
            }
        }
    return hits;
}
static bool match_range(uint64_t v, const uint64_t *arg) { return v >= arg[0] && v < arg[1]; }

static void run_line(FILE *f, char *line, char *summary, size_t sn) {
    char word[16] = "";
    uint64_t a[4] = {0};
    sscanf(line, "%15s", word);
    const char *rest = line + strlen(word);
    while (*rest == ' ') rest++;
    int na = parse_list(rest, a, 4);
    fprintf(f, "== %s\n", line);
    if (!strcmp(word, "dump")) {
        fprintf(f, "object %#llx  steps (oldest first, 0 = one tick):", (unsigned long long)cmd_player);
        for (uint32_t i = 0; i < 240; i++) fprintf(f, "%s%u", i % 60 ? "" : "\n  ", step_hist[(hist_pos + i) % 240]);
        fprintf(f, "\n");
        if (cmd_player) hexdump(f, cmd_player, OBJ_BYTES);
        snprintf(summary, sn, "dump of %#llx", (unsigned long long)cmd_player);
    } else if (!strcmp(word, "mem") && na >= 1) {
        uint64_t len = na >= 2 && a[1] ? (a[1] > 0x10000 ? 0x10000 : a[1]) : 0x100;
        hexdump(f, a[0], (uint32_t)len);
        snprintf(summary, sn, "mem %#llx+%#llx", (unsigned long long)a[0], (unsigned long long)len);
    } else if (!strcmp(word, "scan") && na >= 1) {
        int n = scan_words(f, match_eq, a, 500);
        snprintf(summary, sn, "scan %016llx: %d hits", (unsigned long long)a[0], n);
    } else if (!strcmp(word, "ptr") && na >= 2) {
        int n = scan_words(f, match_range, a, 500);
        snprintf(summary, sn, "ptr [%#llx, %#llx): %d hits", (unsigned long long)a[0], (unsigned long long)a[1], n);
    } else if (!strcmp(word, "snap") && na >= 2) {
        uint64_t len = a[1] > (64u << 20) ? (64u << 20) : a[1];
        uint8_t *b = len ? realloc(snap_buf, len) : NULL;  // (realloc(p, 0) frees p: never asked for)
        if (b) snap_buf = b;
        snap_len = 0;
        if (b && rd(a[0], b, len)) snap_addr = a[0], snap_len = len;
        snprintf(summary, sn, "snap %#llx+%#llx: %s", (unsigned long long)a[0], (unsigned long long)len,
                 snap_len ? "kept" : "unreadable");
    } else if (!strcmp(word, "diff") && snap_len) {
        uint8_t *now = malloc(snap_len);
        int n = 0;
        if (now && rd(snap_addr, now, snap_len))
            for (uint64_t o = 0; o + 4 <= snap_len && n < 2000; o += 4)
                if (memcmp(now + o, snap_buf + o, 4)) {
                    fprintf(f, "+%06llx (%#llx): %08x -> %08x\n", (unsigned long long)o,
                            (unsigned long long)(snap_addr + o), *(uint32_t *)(snap_buf + o), *(uint32_t *)(now + o));
                    n++;
                }
        free(now);
        snprintf(summary, sn, "diff %#llx+%#llx: %d words changed", (unsigned long long)snap_addr,
                 (unsigned long long)snap_len, n);
    } else if (!strcmp(word, "near")) {
        float c[3] = {0}, r = 0;
        if (sscanf(rest, "%f %f %f %f", &c[0], &c[1], &c[2], &r) == 4) {
            int n = scan_near(f, c, r, 3000);
            snprintf(summary, sn, "near %.2f %.2f %.2f r %.2f: %d hits", c[0], c[1], c[2], r, n);
        } else snprintf(summary, sn, "near: want x y z r");
    } else if (!strcmp(word, "regions")) {
        uint64_t ra[256], rs[256];
        int nr = kernel_data_regions(ra, rs, 256);
        for (int i = 0; i < nr; i++)
            fprintf(f, "%#llx +%#llx\n", (unsigned long long)ra[i], (unsigned long long)rs[i]);
        snprintf(summary, sn, "%d regions", nr);
    } else {
        snprintf(summary, sn, "? %s", line);
    }
}

static void *cmd_thread(void *arg) {
    (void)arg;
    static int n;
    char path[64], summary[160];
    mkdir("logs", 0755);
    snprintf(path, sizeof path, "logs/player_out_%d.txt", ++n);
    FILE *f = fopen(path, "w");
    if (!f) { LOG("[player] can't write %s (%d)\n", path, errno); cmd_busy = 0; return NULL; }
    fprintf(f, "frame %llu\n", (unsigned long long)frame_no);
    for (char *save, *line = strtok_r(cmd_text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "chain", 5) || !strncmp(line, "watch", 5) || !strncmp(line, "track", 5)) continue;  // (the game thread's)
        summary[0] = 0;
        run_line(f, line, summary, sizeof summary);
        LOG("[player] %s -> %s\n", summary, path);
    }
    fclose(f);
    cmd_busy = 0;
    return NULL;
}

static void read_commands(uint64_t player) {
    struct stat st;
    bool legacy = stat("player_dump", &st) == 0;  // (b71: an empty file meant a dump)
    if (cmd_busy || (!legacy && stat("player_cmd", &st) != 0)) return;
    const char *name = legacy ? "player_dump" : "player_cmd";
    FILE *f = fopen(name, "r");
    size_t n = f ? fread(cmd_text, 1, sizeof cmd_text - 1, f) : 0;
    if (f) fclose(f);
    remove(name);
    cmd_text[n] = 0;
    if (legacy || !n) strcpy(cmd_text, "dump");
    // chain / watch apply here, on the game thread that reads them
    for (char *l = cmd_text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (!strncmp(l, "chain", 5)) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(l + 5, "\n"), l + 5);
            nchain = tmp[strspn(tmp, " ")] ? parse_list(tmp + strspn(tmp, " "), chain, 16) : -1;
            LOG("[player] chain: %d hops%s\n", nchain, nchain < 0 ? " (the handle-table default)" : "");
        } else if (!strncmp(l, "watch", 5)) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(l + 5, "\n"), l + 5);
            set_watch(tmp + strspn(tmp, " "));
            LOG("[player] watching %d fields\n", nwatch);
        } else if (!strncmp(l, "track", 5)) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(l + 5, "\n"), l + 5);
            track_start(tmp + strspn(tmp, " "));
        }
    }
    cmd_player = player;
    cmd_busy = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, cmd_thread, NULL) == 0) pthread_detach(t);
    else cmd_busy = 0;
}

void za_probe_frame(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_PLAYER_PROBE");
        on = e && atoi(e) != 0;
        const char *c = ENV("LARECOMP_PLAYER_CHAIN");
        if (c && *c) nchain = parse_list(c, chain, 16);
        const char *w = ENV("LARECOMP_PLAYER_WATCH");
        set_watch(w && *w ? w : "650:4,660:1,665:1,6b0:1");
    }
    if (!on || !seen_step_site) { frame_steps = frame_calls = 0; return; }
    // (this runs at the start of the frame clock: frame_steps is the previous frame's)
    step_hist[hist_pos] = (uint8_t)(frame_steps > 255 ? 255 : frame_steps);
    hist_pos = (hist_pos + 1) % 240;
    uint32_t calls = frame_calls;
    frame_steps = frame_calls = 0;
    frame_no++;
    track_frame(calls);
    if (frame_no % 120 == 1) nreg = kernel_data_regions(reg_a, reg_s, 256);

    uint64_t handle, player = resolve(&handle);
    static uint64_t last_player = 1, last_v[16];
    static bool fresh;
    int two = 0;  // frames among the last 60 that handed out a 2-tick (or bigger) step
    for (uint32_t i = 1; i <= 60; i++) two += step_hist[(hist_pos + 240 - i) % 240] != 0;
    if (player != last_player) {
        LOG("[player] object %#llx (handle %#llx)\n", (unsigned long long)player, (unsigned long long)handle);
        last_player = player, fresh = true;
    }
    if (player && nwatch) {
        uint64_t v[16];
        bool changed = fresh;
        for (int i = 0; i < nwatch; i++) {
            v[i] = 0;
            if (!rd(player + watch[i].off, &v[i], watch[i].size)) v[i] = ~0ull;
            changed |= v[i] != last_v[i];
        }
        if (changed) {
            char line[512];
            int o = snprintf(line, sizeof line, "[player] f%llu", (unsigned long long)frame_no);
            for (int i = 0; i < nwatch && o < (int)sizeof line - 40; i++)
                o += snprintf(line + o, sizeof line - o, " +%x=%llx%s", watch[i].off, (unsigned long long)v[i],
                              v[i] != last_v[i] ? "*" : "");
            LOG("%s | 2-tick %d/60, calls %u\n", line, two, calls);
            memcpy(last_v, v, sizeof v);
        }
        fresh = false;
    }
    if (frame_no % 30 == 0) read_commands(player);
}

RT_ON_RESET(za_probe_reset) {
    seen_step_site = false, nreg = 0, frame_no = 0, hist_pos = 0;
    memset(step_hist, 0, sizeof step_hist);
}
