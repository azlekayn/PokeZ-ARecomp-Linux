// ZA DevTools, the Linux side: a command socket for driving and watching a running game from a script or over ssh
// (tools/za/zadev, its command line). Off unless LARECOMP_DEVTOOLS=1: no thread, no socket, nothing per frame
// in a player's run. Upstream's ZA DevTools (Windows: named pipes and an ImGui window, runtime/ui/devtools.mm there) is
// a tool for a person; this one is for a script - one text command per line, one reply per command, then the next:
//   ping                     "ok frame <n>"
//   status                   frame, fps and frame times (average / 1% low / worst) over the last 600 frames
//   shot [<file>]            the game image (the game's own HUD in it, not the F1 menu) at the next frame -> png;
//                            replies once written (default screenshots/dev_<frame>.png)
//   hold <input> <frames>    holds a button (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK), a stick
//                            direction (STICK_UP.. / RSTICK_LEFT.., STICK_<degrees>) or a stick position
//                            (ls:<x>/<y>, rs:<x>/<y>, -32767..32767, y up) from the next frame, alongside the player's own
//   press <input>            hold for 6 frames
//   release                  lets go of everything held
//   key <SDL key name> [n]   a real key press (down, up n frames later, default 6): F1, Escape, Return, Up...
//   wait <frames>            replies after that many more frames (a sequence: press A, wait 60, shot)
//   mark [<note>]            "[devtools] mark" in the log with the frame: where the bug was on screen
//   f9                       the next frame's passes (with LARECOMP_DEBUG_FRAMES=1, as F9)
//   gpuprof                  GPU time per pass of the next frame ("[gpuprof]" in the log, as LARECOMP_GPUPROF_AT)
//   set <name> <n>           a switch in the running game, for an A/B at one spot: ssgi_half (LARECOMP_SSGI_HALF)
// The input goes in where LARECOMP_INPUT's does (runtime/hle/input.c script_buttons): the game sees it, the F1 menu
// doesn't - `key` reaches the menu. Socket: LARECOMP_DEVTOOLS_SOCK, else $XDG_RUNTIME_DIR/za-devtools.sock, else
// /tmp/za-devtools-<uid>.sock (this user only: 0600).
#include <SDL3/SDL.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "rt.h"

volatile int g_gpuprof_request;  // (device.c: starts a [gpuprof] frame at the next present; every platform)
#ifdef __linux__
static bool g_on;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_frame_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_frame;  // (the last frame devtools_frame saw)

// ---- frame times
enum { FT_N = 600 };
static float g_ft[FT_N];
static uint64_t g_ft_n, g_last_ns;

// ---- input held by commands
typedef struct { uint64_t from, until, b; int32_t v[4]; uint8_t set; } Hold;  // (v: lx ly rx ry; set: which of them)
enum { HOLD_MAX = 32 };
static Hold g_hold[HOLD_MAX];
static int g_nhold;
typedef struct { SDL_Scancode sc; uint64_t up_at; } KeyUp;
static KeyUp g_keyup[16];
static int g_nkeyup;

// ---- screenshot (runtime/nvn/vulkan/device.c takes g_devtools_shot_path with g_shot_request)
char g_devtools_shot_path[512];
extern volatile int g_shot_request;
extern volatile int g_debug_frame_request;

static const char *const btn_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};  // (input.c input_names: the same bits)

static bool parse_input(const char *s, Hold *h) {
    int x, y;
    memset(h, 0, sizeof *h);
    if (sscanf(s, "ls:%d/%d", &x, &y) == 2) { h->set = 3, h->v[0] = x, h->v[1] = y; return true; }
    if (sscanf(s, "rs:%d/%d", &x, &y) == 2) { h->set = 12, h->v[2] = x, h->v[3] = y; return true; }
    static const struct { const char *n; uint8_t set; int axis; int32_t v; } dirs[] = {
        {"STICK_UP", 2, 1, 32767},    {"STICK_DOWN", 2, 1, -32767},  {"STICK_LEFT", 1, 0, -32767},  {"STICK_RIGHT", 1, 0, 32767},
        {"RSTICK_UP", 8, 3, 32767},   {"RSTICK_DOWN", 8, 3, -32767}, {"RSTICK_LEFT", 4, 2, -32767}, {"RSTICK_RIGHT", 4, 2, 32767}};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
        if (!SDL_strcasecmp(s, dirs[i].n)) { h->set = dirs[i].set, h->v[dirs[i].axis] = dirs[i].v; return true; }
    if (!SDL_strncasecmp(s, "STICK_", 6) && s[6] >= '0' && s[6] <= '9') {
        double r = atoi(s + 6) * 3.14159265358979 / 180;
        h->set = 3, h->v[0] = (int32_t)lround(cos(r) * 32767), h->v[1] = (int32_t)lround(sin(r) * 32767);
        return true;
    }
    for (int i = 0; i < 16; i++)
        if (!SDL_strcasecmp(s, btn_names[i])) { h->b = 1ull << i; return true; }
    return false;
}

// (the game thread, input.c input_frame) what commands hold at this frame: buttons ORed in, sticks set where held
void devtools_input(uint64_t frame, uint64_t *b, int32_t v[4]) {
    if (!g_on) return;
    pthread_mutex_lock(&g_lock);
    int k = 0;
    for (int i = 0; i < g_nhold; i++) {
        Hold *h = &g_hold[i];
        if (frame >= h->until) continue;  // (done: dropped)
        g_hold[k++] = *h;
        if (frame < h->from) continue;
        *b |= h->b;
        for (int a = 0; a < 4; a++) if (h->set >> a & 1) v[a] = h->v[a];
    }
    g_nhold = k;
    pthread_mutex_unlock(&g_lock);
}
bool devtools_on(void) { return g_on; }

static void push_key(SDL_Scancode sc, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc, e.key.down = down, e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    SDL_PushEvent(&e);  // (any thread)
}

// (the game thread, every presented frame: input.c input_frame)
void devtools_frame(uint64_t frame) {
    if (!g_on) return;
    uint64_t now = SDL_GetTicksNS();
    pthread_mutex_lock(&g_lock);
    if (g_last_ns) g_ft[g_ft_n++ % FT_N] = (float)(now - g_last_ns) / 1e6f;
    g_last_ns = now;
    g_frame = frame;
    int k = 0;
    for (int i = 0; i < g_nkeyup; i++) {
        if (frame >= g_keyup[i].up_at) push_key(g_keyup[i].sc, false);
        else g_keyup[k++] = g_keyup[i];
    }
    g_nkeyup = k;
    pthread_cond_broadcast(&g_frame_cv);
    pthread_mutex_unlock(&g_lock);
}

// Waits (g_lock held) until frame `f` was seen; false after `ms` without it (the game paused, loading, closing)
static bool wait_frame_locked(uint64_t f, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000, ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    while (g_frame < f)
        if (pthread_cond_timedwait(&g_frame_cv, &g_lock, &ts) == ETIMEDOUT) return g_frame >= f;
    return true;
}

static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? 1 : x > y ? -1 : 0; }

static void reply(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    for (int o = 0; o < n;) {
        ssize_t w = write(fd, buf + o, (size_t)(n - o));
        if (w <= 0) return;
        o += (int)w;
    }
}

static void command(int fd, char *c) {
    char a[256] = "", b[256] = "";
    long n = 0;
    while (*c == ' ') c++;
    if (!*c) return;
    pthread_mutex_lock(&g_lock);
    uint64_t f = g_frame;
    pthread_mutex_unlock(&g_lock);
    if (!strcmp(c, "ping")) { reply(fd, "ok frame %llu", (unsigned long long)f); return; }
    if (!strcmp(c, "status")) {
        float t[FT_N];
        pthread_mutex_lock(&g_lock);
        int k = (int)(g_ft_n < FT_N ? g_ft_n : FT_N);
        memcpy(t, g_ft, sizeof t);
        pthread_mutex_unlock(&g_lock);
        if (!k) { reply(fd, "ok frame %llu (no frame times yet)", (unsigned long long)f); return; }
        double sum = 0;
        for (int i = 0; i < k; i++) sum += t[i];
        qsort(t, (size_t)k, sizeof *t, cmp_f);
        int p1 = k / 100 > 0 ? k / 100 : 1;
        double low = 0;
        for (int i = 0; i < p1; i++) low += t[i];
        low /= p1;
        reply(fd, "ok frame %llu fps %.1f avg %.2f ms 1%%low %.1f fps worst %.1f ms (last %d frames)",
              (unsigned long long)f, 1000.0 * k / sum, sum / k, 1000.0 / low, t[0], k);
        return;
    }
    if (!strncmp(c, "shot", 4) && (c[4] == 0 || c[4] == ' ')) {
        if (sscanf(c + 4, " %255[^\n]", a) != 1) snprintf(a, sizeof a, "screenshots/dev_%05llu.png", (unsigned long long)f + 1);
        char dir[256];
        snprintf(dir, sizeof dir, "%s", a);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0, mkdir(dir, 0755);
        pthread_mutex_lock(&g_lock);
        snprintf(g_devtools_shot_path, sizeof g_devtools_shot_path, "%s", a);
        g_shot_request = 1;
        // (device.c takes it at its next present and clears the request, then writes the file: up to 2 s for it)
        remove(a);
        struct stat st;
        bool ok = wait_frame_locked(f + 1, 5000), written = false;
        for (int i = 0; ok && i < 120 && !(written = !g_shot_request && !stat(a, &st) && st.st_size > 0); i++)
            ok = wait_frame_locked(g_frame + 1, 5000);
        pthread_mutex_unlock(&g_lock);
        char abs[600];
        if (a[0] == '/') snprintf(abs, sizeof abs, "%s", a);
        else { char cwd[400]; snprintf(abs, sizeof abs, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", a); }
        if (written) reply(fd, "ok %s", abs);
        else reply(fd, "err no frame presented in 5 s (paused or loading?): %s not written", abs);
        return;
    }
    if (sscanf(c, "hold %255s %ld", a, &n) == 2 || (sscanf(c, "press %255s", a) == 1 && (n = 6))) {
        Hold h;
        if (!parse_input(a, &h) || n <= 0) { reply(fd, "err unknown input \"%s\" (or no frames)", a); return; }
        pthread_mutex_lock(&g_lock);
        h.from = g_frame + 1, h.until = h.from + (uint64_t)n;
        if (g_nhold < HOLD_MAX) g_hold[g_nhold++] = h;
        uint64_t until = h.until;
        pthread_mutex_unlock(&g_lock);
        reply(fd, "ok %s held frames %llu..%llu", a, (unsigned long long)until - (uint64_t)n, (unsigned long long)until - 1);
        return;
    }
    if (!strcmp(c, "release")) {
        pthread_mutex_lock(&g_lock);
        g_nhold = 0;
        pthread_mutex_unlock(&g_lock);
        reply(fd, "ok");
        return;
    }
    if (sscanf(c, "key %255s %ld", a, &n) >= 1) {
        SDL_Scancode sc = SDL_GetScancodeFromName(a);
        if (sc == SDL_SCANCODE_UNKNOWN) { reply(fd, "err unknown key \"%s\" (SDL key names: F1, Escape, Return, Up...)", a); return; }
        if (n <= 0) n = 6;
        push_key(sc, true);
        pthread_mutex_lock(&g_lock);
        if (g_nkeyup < 16) g_keyup[g_nkeyup++] = (KeyUp){sc, g_frame + (uint64_t)n};
        pthread_mutex_unlock(&g_lock);
        reply(fd, "ok %s", a);
        return;
    }
    if (sscanf(c, "wait %ld", &n) == 1) {
        pthread_mutex_lock(&g_lock);
        bool ok = wait_frame_locked(f + (uint64_t)(n > 0 ? n : 0), 5000 + (int)(n > 0 ? n : 0) * 100);
        uint64_t now = g_frame;
        pthread_mutex_unlock(&g_lock);
        if (ok) reply(fd, "ok frame %llu", (unsigned long long)now);
        else reply(fd, "err frame %llu: frames stopped coming (paused or loading?)", (unsigned long long)now);
        return;
    }
    if (!strncmp(c, "mark", 4) && (c[4] == 0 || c[4] == ' ')) {
        sscanf(c + 4, " %255[^\n]", b);
        LOG("[devtools] mark at frame %llu%s%s\n", (unsigned long long)f, b[0] ? ": " : "", b);
        reply(fd, "ok frame %llu", (unsigned long long)f);
        return;
    }
    if (!strcmp(c, "f9")) { g_debug_frame_request = 1; reply(fd, "ok"); return; }
    if (!strncmp(c, "set ", 4)) {
        extern int g_ssgi_half;  // (nvn/vulkan/exec.c)
        static const struct { const char *name; int *v; } sw[] = {{"ssgi_half", &g_ssgi_half}};
        char name[64];
        int v;
        if (sscanf(c + 4, "%63s %d", name, &v) == 2)
            for (size_t i = 0; i < sizeof sw / sizeof sw[0]; i++)
                if (!strcmp(name, sw[i].name)) { *sw[i].v = v; reply(fd, "ok %s %d", name, v); return; }
        reply(fd, "err set <name> <n>: ssgi_half");
        return;
    }
    if (!strcmp(c, "gpuprof")) { g_gpuprof_request = 1; reply(fd, "ok frame %llu", (unsigned long long)f); return; }
    reply(fd, "err unknown command (ping status shot hold press release key wait mark f9 gpuprof set)");
}

static void *serve(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; SDL_Delay(200); continue; }
        char buf[1024];
        size_t have = 0;
        for (;;) {
            ssize_t r = read(fd, buf + have, sizeof buf - 1 - have);
            if (r <= 0) break;
            have += (size_t)r;
            buf[have] = 0;
            char *nl;
            while ((nl = memchr(buf, '\n', have))) {
                *nl = 0;
                if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
                command(fd, buf);
                size_t used = (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, have - used);
                have -= used;
                buf[have] = 0;
            }
            if (have == sizeof buf - 1) have = 0;  // (a line too long: dropped)
        }
        close(fd);
    }
    return NULL;
}

// (off unless asked for; runtime/main.c, after rt_paths_init: relative paths are the data root's)
void devtools_init(void) {
    const char *e = getenv("LARECOMP_DEVTOOLS");
    if (!e || !atoi(e)) return;
    char path[108];
    const char *s = getenv("LARECOMP_DEVTOOLS_SOCK"), *xdg = getenv("XDG_RUNTIME_DIR");
    if (s && *s) snprintf(path, sizeof path, "%s", s);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/za-devtools.sock", xdg);
    else snprintf(path, sizeof path, "/tmp/za-devtools-%u.sock", (unsigned)getuid());
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    unlink(path);  // (a game that ended without removing it)
    mode_t old = umask(077);
    bool ok = ls >= 0 && !bind(ls, (struct sockaddr *)&addr, sizeof addr) && !listen(ls, 4);
    umask(old);
    if (!ok) { LOG("[devtools] socket %s: %s - off\n", path, strerror(errno)); if (ls >= 0) close(ls); return; }
    g_on = true;
    pthread_t t;
    pthread_create(&t, NULL, serve, (void *)(intptr_t)ls);
    pthread_detach(t);
    LOG("[devtools] listening on %s (tools/za/zadev)\n", path);
}
#else
void devtools_init(void) {}
void devtools_frame(uint64_t frame) { (void)frame; }
void devtools_input(uint64_t frame, uint64_t *b, int32_t v[4]) { (void)frame, (void)b, (void)v; }
bool devtools_on(void) { return false; }
char g_devtools_shot_path[512];
#endif
