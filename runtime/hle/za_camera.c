// Pokémon Z-A: F1 > Game's camera distance. The follow camera is three rings (top, middle, bottom: Cinemachine-like, the
// camera slides between them by pitch), read from world/ik_obj_template/field/field_camera_/field_camera_0.trsot into
// several structures on the heap as float pairs { height, radius }, one ring every 0x50 bytes. The game reads those every
// frame, so changing them moves the camera at once (tested live, 2026-11-06), and they stay through areas and battles.
// Height is scaled with radius, which keeps the camera's angle. The follow and ladder orbits have the same numbers and
// both match; indoors, selfie and cutscene cameras have others and are left alone, as is the battle and lock-on framing.
//
// No global names them: they're found by their numbers, all three rings (one pair alone also matches something else),
// so any game version with these values works and a mod that changes them is simply left as it is. On a thread of its
// own, not the game's: a pass reads all the writable guest memory, and nothing here calls the game. Through
// process_vm_readv / writev, as what was found can be unmapped meanwhile. It does nothing at all until the slider has
// left 1x. A pass every 10 s after that picks up any structure made later. LARECOMP_ZA_CAMERA=0: off, the row too.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // (process_vm_readv)
#endif
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "../hle.h"
#include "../settings.h"

#define RING_STEP 0x50
#define MAX_SITES 64
#define CHUNK (32u << 20)

bool za_camera_available(void) {
#ifdef __linux__
    const char *e = ENV("LARECOMP_ZA_CAMERA");
    return !(e && atoi(e) == 0);
#else
    return false;  // (a scan needs reads that fail safely: only written for Linux so far)
#endif
}

#ifdef __linux__
extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);  // (runtime/kernel.c)

static const float k_rings[3][2] = {{7.0f, 3.0f}, {2.0999999f, 4.5f}, {1.1f, 0.5f}};  // { height, radius }: the game's
typedef struct { uint64_t mid; float scale; } Site;  // (mid: the middle ring; scale: what's written there now)
static Site sites[MAX_SITES];
static int nsites;
static volatile int reset_req;

static bool rd(uint64_t a, void *out, size_t n) {
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}
static bool wr(uint64_t a, const void *in, size_t n) {
    struct iovec l = {(void *)in, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

// All three rings hold the game's numbers times scale
static bool rings_are(uint64_t mid, float scale) {
    for (int i = 0; i < 3; i++) {
        float v[2];
        if (!rd(mid + (int64_t)(i - 1) * RING_STEP, v, sizeof v)) return false;
        for (int j = 0; j < 2; j++)
            if (fabsf(v[j] - k_rings[i][j] * scale) > 0.002f) return false;
    }
    return true;
}

static void note_site(uint64_t mid, float scale) {
    for (int i = 0; i < nsites; i++)
        if (sites[i].mid == mid) return;
    if (nsites == MAX_SITES) return;
    sites[nsites++] = (Site){mid, scale};
    LOG("[za_camera] the follow camera's rings at %#llx (%.2fx)\n", (unsigned long long)mid, scale);
}

static void apply(float want) {
    for (int i = 0; i < nsites;) {
        if (sites[i].scale == want) { i++; continue; }
        if (!rings_are(sites[i].mid, sites[i].scale)) {  // (freed and used again, or the game wrote its own)
            LOG("[za_camera] the rings at %#llx went\n", (unsigned long long)sites[i].mid);
            sites[i] = sites[--nsites];
            continue;
        }
        for (int r = 0; r < 3; r++) {
            float v[2] = {k_rings[r][0] * want, k_rings[r][1] * want};
            wr(sites[i].mid + (int64_t)(r - 1) * RING_STEP, v, sizeof v);
        }
        sites[i].scale = want;
        i++;
    }
}

// The middle ring's pair at either scale: the game's own numbers, or a copy made from a structure already scaled
static void scan_chunk(const uint8_t *buf, uint64_t n, uint64_t base, float want) {
    float p1[2] = {k_rings[1][0], k_rings[1][1]}, p2[2] = {k_rings[1][0] * want, k_rings[1][1] * want};
    uint64_t w1, w2;
    memcpy(&w1, p1, 8), memcpy(&w2, p2, 8);
    for (uint64_t i = 0; i + 8 <= n; i += 4) {
        uint64_t w;
        memcpy(&w, buf + i, 8);
        if (w != w1 && w != w2) continue;
        uint64_t mid = base + i;
        if (w == w1 && rings_are(mid, 1.0f)) note_site(mid, 1.0f);
        else if (w == w2 && rings_are(mid, want)) note_site(mid, want);
    }
}

static void *camera_thread(void *arg) {
    (void)arg;
    uint8_t *buf = malloc(CHUNK + 8);
    uint64_t ra[256], rs[256], pos = 0;
    int nr = 0, ri = 0;
    bool active = false;
    uint32_t wait_ms = 0;
    if (!buf) return NULL;
    for (;;) {
        if (reset_req) {
            reset_req = 0;
            nsites = 0, nr = 0, wait_ms = 2000;  // (another boot: everything found is gone)
        }
        float want = g_settings.camera_distance;
        if (want < 0.5f || want > 3.0f) want = 1.0f;  // (not a value of ours)
        if (want != 1.0f) active = true;  // (at 1x, never changed: nothing to do, nothing read)
        if (!active) { usleep(100000); continue; }
        apply(want);
        if (wait_ms) {
            usleep(20000);
            wait_ms = wait_ms > 20 ? wait_ms - 20 : 0;
            continue;
        }
        if (!nr) nr = kernel_data_regions(ra, rs, 256), ri = 0, pos = 0;
        while (ri < nr && pos + 8 > rs[ri]) ri++, pos = 0;
        if (ri >= nr) {
            nr = 0, wait_ms = 10000;
            continue;
        }
        uint64_t n = rs[ri] - pos;
        if (n > CHUNK + 8) n = CHUNK + 8;
        uint64_t base = ra[ri] + pos;
        pos += n > 8 ? n - 8 : n;  // (a pair can't fall between two chunks)
        if (rd(base, buf, n)) scan_chunk(buf, n, base, want);
        usleep(4000);  // (on a core of its own, and only while a pass runs)
    }
    return NULL;
}
#endif

// rt_za_frame_dt, once a frame: only starts the thread, on the first frame of Pokémon Z-A
void za_camera_frame(void) {
#ifdef __linux__
    static bool started;
    if (started) return;
    started = true;
    if (!za_camera_available()) return;
    pthread_t t;
    if (!pthread_create(&t, NULL, camera_thread, NULL)) pthread_detach(t);
#endif
}

#ifdef __linux__
RT_ON_RESET(za_camera_reset) { reset_req = 1; }
#endif
