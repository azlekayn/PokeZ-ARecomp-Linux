// Driver for tools/test_saveserv.sh: runtime/saveserv.cpp on its own, with the few runtime calls it makes stubbed.
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include "saveserv.h"
extern "C" {
#include "paths.h"
#include "platform.h"
extern char **environ;
const char *rt_data_root(void) { return getenv("LARECOMP_DATA_ROOT"); }
PlatProc plat_spawn(const char *const argv[], const char *log, bool) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log ? log : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}
int plat_proc_wait(PlatProc p, bool) {
    int st = 0;
    waitpid((pid_t)p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}
int plat_rename(const char *a, const char *b) { return rename(a, b); }
}
static const char *name(SaveServState s) {
    const char *n[] = {"OFF", "PAIRING", "BUSY", "SYNCED", "OFFLINE", "CONFLICT", "NEWER", "ERROR"};
    return n[s];
}
static void settle() {
    while (saveserv_status().state == SAVESERV_BUSY) usleep(20000);
}
static void report() {
    SaveServStatus s = saveserv_status();
    const char *a[] = {"nothing", "uploaded", "downloaded"};
    printf("%s %s %s\n", name(s.state), a[s.last_action], s.message.c_str());
}
int main(int ac, char **av) {
    std::string cmd = ac > 1 ? av[1] : "";
    setvbuf(stdout, NULL, _IONBF, 0);
    if (cmd == "pair") {
        saveserv_pair_start(av[2]);
        settle();
        SaveServStatus s = saveserv_status();
        if (s.state != SAVESERV_PAIRING) { report(); return 0; }
        printf("PIN %s\n", s.pin.c_str());
        for (int i = 0; i < 200 && saveserv_status().state == SAVESERV_PAIRING; i++) usleep(100000);
        settle();
        report();
    } else if (cmd == "sync") {
        saveserv_kick();
        settle();
        report();
    } else if (cmd == "resolve") {
        saveserv_kick();
        settle();
        saveserv_resolve(!strcmp(av[2], "local"));
        settle();
        report();
    } else if (cmd == "ingame-write") {  // the game runs; it saves av[2]; the save settles: sent on its own
        saveserv_game_starting();
        report();
        sleep(2);  // (the same size in the same second looks unchanged: not something a game does just after it starts)
        FILE *f = fopen((std::string(rt_data_root()) + "/save/save_0/main").c_str(), "wb");
        fputs(av[2], f);
        fclose(f);
        sleep(11);
        report();
    } else if (cmd == "ingame-flush") {  // as above, quitting straight after the save
        saveserv_game_starting();
        FILE *f = fopen((std::string(rt_data_root()) + "/save/save_0/main").c_str(), "wb");
        fputs(av[2], f);
        fclose(f);
        saveserv_flush(5000);
        report();
    } else return 2;
    return 0;
}
