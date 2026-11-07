// Pokémon Z-A: F1 > Audio's music, sound effect and Pokémon cry sliders. They are the game's own options, not a mixer of
// ours: the audio reaches the runtime already mixed (Wwise, one stereo stream), so nothing could be told apart here. The
// game keeps the three as 4-bit fields of one word in its options object (+0x40: bits 9-12 music, 13-16 effects, 17-20
// cries, each 0-10, default 10) and f_7000b20d04 hands that word to Wwise. So a slider changes the field and calls that
// function, on the game thread, once a frame at most: its own menu shows the same values, and they only turn things
// down (F1's Volume above them is the one that goes louder).
//
// The object is on the heap and no global names it: it's found by its two secondary method tables, which sit side by side
// at +0x20, in a slow pass over the writable guest memory (8 MB a frame, about 1.5 ms, and only until it's found).
// Addresses are 2.0.2's: on another build the rows don't appear. LARECOMP_ZA_AUDIO=0: off, the rows too.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (process_vm_readv)
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "../game_addresses.h"
#include "../hle.h"
#include "../za_audio.h"

extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);  // (runtime/kernel.c)
extern Cpu *kernel_current_cpu(void);

#define VT_A 0x7003d5a070ull      // (the options object's method tables at +0x20 and +0x28)
#define VT_B 0x7003d5a0c0ull
#define OBJ_FROM_VT (-0x20)
#define WORD_OFF 0x40
#define APPLY_FN 0x7000b20d04ull  // (the game's own: every volume in the word to Wwise)
#define CHUNK (8u << 20)

static const int k_shift[ZA_AUDIO_N] = {9, 13, 17};
static uint64_t obj;                         // (the game thread writes it; the menu reads it)
static volatile int want[ZA_AUDIO_N] = {-1, -1, -1};  // (the menu's change, for the next frame's game thread)
static uint64_t ra[256], rs[256];
static int nr, ri;
static uint64_t pos;
static uint32_t wait_frames = 120;  // (before a pass: the game makes the object during boot)
static uint8_t *buf;

static bool build_fits(void) {
    static const uint8_t id[8] = {0xB1, 0xF1, 0x2F, 0xD9, 0x19, 0xEA, 0xE8, 0x6A};  // 2.0.2
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_ZA_AUDIO");
        on = !(e && atoi(e) == 0);
    }
    return on && !memcmp(g_main_build_id, id, sizeof id);
}

static bool rd(uint64_t a, void *out, size_t n) {  // (memory the game could have unmapped since the region list)
#ifdef __linux__
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#else
    memcpy(out, (const void *)(uintptr_t)a, n);
    return true;
#endif
}

static bool word_fits(uint32_t w) {
    for (int i = 0; i < ZA_AUDIO_N; i++)
        if (((w >> k_shift[i]) & 15) > 10) return false;
    return true;
}

// One slice of the pass: a pair of method tables at +0x20 / +0x28, and a word whose volumes are volumes
static void scan_slice(void) {
    if (wait_frames) { wait_frames--; return; }
    if (!buf && !(buf = malloc(CHUNK + 8))) return;
    if (!nr) nr = kernel_data_regions(ra, rs, 256), ri = 0, pos = 0;
    while (ri < nr && pos + 16 > rs[ri]) ri++, pos = 0;
    if (ri >= nr) {
        nr = 0, wait_frames = 600;  // (not there yet: again in ten seconds)
        return;
    }
    uint64_t n = rs[ri] - pos;
    if (n > CHUNK + 8) n = CHUNK + 8;
    uint64_t base = ra[ri] + pos;
    pos += n > 8 ? n - 8 : n;  // (a pair can't fall between two slices)
    if (!rd(base, buf, n)) return;
    const uint64_t *p = (const uint64_t *)buf;
    for (uint64_t i = 0; i + 1 < n / 8; i++) {
        if (p[i] != VT_A || p[i + 1] != VT_B) continue;
        uint64_t o = base + i * 8 + OBJ_FROM_VT;
        uint32_t w;
        if (!rd(o + WORD_OFF, &w, 4) || !word_fits(w)) continue;
        obj = o;
        LOG("[za_audio] the game's options at %#llx: music %u, effects %u, cries %u (of 10)\n", (unsigned long long)o,
            (w >> k_shift[0]) & 15, (w >> k_shift[1]) & 15, (w >> k_shift[2]) & 15);
        free(buf), buf = NULL;
        return;
    }
}

// rt_za_frame_dt, once a frame on the game thread
void za_audio_frame(void) {
    if (!build_fits()) return;
    if (!obj) { scan_slice(); return; }
    static uint32_t tick;
    if (++tick % 60 == 0) {  // (still the object? a title-screen return can make a new one)
        uint64_t v[2];
        if (!rd(obj - OBJ_FROM_VT, v, sizeof v) || v[0] != VT_A || v[1] != VT_B) {
            LOG("[za_audio] the options object went: looking again\n");
            obj = 0, nr = 0, wait_frames = 120;
            return;
        }
    }
    bool change = false;
    uint32_t w = *(volatile uint32_t *)(uintptr_t)(obj + WORD_OFF);
    for (int i = 0; i < ZA_AUDIO_N; i++) {
        int v = want[i];
        if (v < 0) continue;
        want[i] = -1;
        w = (w & ~(15u << k_shift[i])) | ((uint32_t)v << k_shift[i]);
        change = true;
    }
    if (!change) return;
    *(volatile uint32_t *)(uintptr_t)(obj + WORD_OFF) = w;
    // (called from the middle of the game's frame code: every register as it was afterwards, and clear of its stack)
    Cpu *c = kernel_current_cpu();
    if (!c) return;
    Cpu saved = *c;
    c->sp = (c->sp - 0x400) & ~15ull;
    c->x[0] = obj;
    guest_call(c, APPLY_FN);
    *c = saved;
}

// F1 (the UI thread)
bool za_audio_ready(void) { return obj != 0; }
int za_audio_get(int i) {
    int v = want[i];
    if (v >= 0) return v;
    uint64_t o = obj;
    return o ? (int)((*(volatile uint32_t *)(uintptr_t)(o + WORD_OFF) >> k_shift[i]) & 15) : 10;
}
void za_audio_set(int i, int v) { want[i] = v < 0 ? 0 : v > 10 ? 10 : v; }

RT_ON_RESET(za_audio_reset) {
    obj = 0, nr = 0, wait_frames = 120;
    for (int i = 0; i < ZA_AUDIO_N; i++) want[i] = -1;
}
