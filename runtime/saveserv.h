// Save sync through a SaveServ server (github: wingrepo/SaveServ, the player's own server): runtime/saveserv.cpp. Off
// (no network at all) until this device is paired, from the start screen's Saves page or the F1 menu's Saves tab.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void saveserv_init(void);           // settings read, the sync thread up if paired (any thread; again is harmless)
void saveserv_kick(void);           // a sync now, in the background (start screen: both ways; in game: uploads only)
void saveserv_game_starting(void);  // before the game boots: waits for a running sync, and no more downloads from here on
void saveserv_game_stopped(void);   // the game is down (a reboot): the next start screen / boot syncs both ways again
void saveserv_flush(int max_ms);    // quitting: a save not sent yet goes now (at most max_ms, a next start sends it otherwise)

#ifdef __cplusplus
}
#include <string>

enum SaveServState {
    SAVESERV_OFF,        // not paired
    SAVESERV_PAIRING,    // pin holds the PIN to enter on the website
    SAVESERV_BUSY,
    SAVESERV_SYNCED,     // last_action, when
    SAVESERV_OFFLINE,    // the server didn't answer: playing from the local save, as ever
    SAVESERV_CONFLICT,   // this device and the server both changed: saveserv_resolve (start screen only)
    SAVESERV_NEWER,      // (in game) the server has a newer save: nothing sent, a restart picks it up
    SAVESERV_ERROR,      // message
};
enum SaveServAction { SAVESERV_NOTHING, SAVESERV_UPLOADED, SAVESERV_DOWNLOADED };

struct SaveServStatus {
    SaveServState state = SAVESERV_OFF;
    SaveServAction last_action = SAVESERV_NOTHING;
    int64_t when = 0;             // the last good sync (Unix time)
    int64_t remote_updated = 0;   // CONFLICT, NEWER: the server's save, when and from which device
    std::string remote_device;
    std::string server, account, pin, message;
    bool unpaired_remotely = false;  // the website removed this device (state OFF)
    bool in_game = false;
    uint32_t downloads = 0;       // counts the downloads (the start screen's Saves page reads the folder again)
};

SaveServStatus saveserv_status(void);
void saveserv_pair_start(const char *server);  // "host" or "http(s)://host"; PIN in the status, or an error
void saveserv_pair_cancel(void);
void saveserv_unpair(void);                    // forgets the token here (the website still lists it until removed there)
void saveserv_resolve(bool keep_this_device);  // a conflict. Neither copy is lost: see saveserv.cpp
std::string saveserv_server_hint(void);        // the server in the settings file, even when not paired
#endif
