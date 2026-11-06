// Save sync through a SaveServ server: the player's own server, paired with a PIN shown here and typed on its website.
// The client of that project is a Rust crate and nothing here builds Rust, so this speaks its small HTTP API itself
// (same settings file and sync-state format, same three-way compare); the transport is the system curl, as the start
// screen's update check, so https works without a TLS stack in the runtime and an offline server costs nothing.
//
// What is synced: save/save_0/main and save/save_0/backup (the game's two files), as one revision: slot "save" of game
// "za", an uncompressed tar with no times or owners. That is what the saveserv tool's `add za` / `run za` stores (its
// src/folder.rs pack, byte for byte, so the same save has the same hash from either), so the two share one history on the
// website instead of two entries for one game. (b113 / b114 uploaded the files raw under "pokemon-za": left on the server
// as they are, never read.) One revision because the game writes them together, and half of another device's save beside
// half of this one's would be worse than either. Anything else in the folder is left alone.
//
// The save is compared with what it looked like at the last sync (save/.saveserv-state):
// - only here changed: upload. Only the server changed: download. Both: a conflict, the player chooses.
// - no local file, the server has one: download (a new device). The reverse: upload. Deleting is never sent.
// Nothing is overwritten without a copy: the server keeps every upload as a revision, and before any download the local
// save is copied to save/backup-<date>-sync (the Saves page's Backups, its 5 newest kept).
//
// When: both ways on the start screen (it opens, and after the game). While the game runs, UPLOADS ONLY: the game holds
// its save in memory, so a file swapped under it would be overwritten at its next save, or worse, read half way. The save
// folder is polled instead of hooking the file service (Pokémon Z-A writes in place and renames open files, see
// services/fs.c): a change is sent once it has been still for SETTLE_S, at most every MIN_UPLOAD_S (the server keeps 50
// revisions per slot: frequent autosaves mustn't push the useful ones out), and at quit (saveserv_flush).
//
// Threads: everything that can wait on the network runs on one host thread (not a guest thread: it isn't stopped by a
// reboot). g_m guards the settings and the status; it is never held across a request.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "cache_sha256.h"
#include "saveserv.h"

extern "C" {
#include "paths.h"
#include "platform.h"
}

#define GAME_ID "za"
#define SAVE_SLOT "save"
#define SETTLE_S 5        // the save folder still for this long: the game has finished writing
#define MIN_UPLOAD_S 60   // in game, between uploads
#define KEEP_SYNC_BACKUPS 5
#define WAIT_MAX_MS 40000  // a request's limit (curl's max-time, 30 s) and some

static const char *const FILES[] = {"backup", "main"};  // (sorted: the tar's order)
enum { NFILES = 2 };

// ------------------------------------------------------------------ settings (shared with the saveserv tool)
struct Config {
    std::string server, token, account, device;
};
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER, g_idle = PTHREAD_COND_INITIALIZER;
static Config g_cfg;
static SaveServStatus g_st;
static bool g_inited, g_thread_up, g_busy, g_in_game, g_synced_once;
static bool g_req_sync;
static int g_req_force;  // a conflict's answer: 1 this device, 2 the server
static std::string g_pair_id, g_pair_server;
static time_t g_pair_expires;

static std::string config_path(void) {
    if (const char *e = getenv("SAVESERV_CONFIG")) return e;
#ifdef _WIN32
    const char *a = getenv("APPDATA");
    return std::string(a ? a : ".") + "/saveserv/saveserv.cfg";
#else
    if (const char *x = getenv("XDG_CONFIG_HOME")) return std::string(x) + "/saveserv/saveserv.cfg";
    const char *h = getenv("HOME");
    return std::string(h ? h : ".") + "/.config/saveserv/saveserv.cfg";
#endif
}

static void mkdirs_for(const std::string &file) {
    for (size_t i = 1; i < file.size(); i++)
        if (file[i] == '/') mkdir(file.substr(0, i).c_str(), 0755);
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string read_file(const std::string &path, bool *ok = NULL) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (ok) *ok = f != NULL;
    if (!f) return out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    if (ferror(f) && ok) *ok = false;
    fclose(f);
    return out;
}

// temp file + flush + rename: a crash never leaves a torn file. private: readable by this user only (the token)
// A finished temp file over the real one. Not plat_rename: that is the console's rename and refuses a target that exists,
// so every file here could be written once: the state's revision stuck (the next save then looked like a conflict and
// wasn't sent), and a download over an existing save failed.
static bool replace_file(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

static bool write_atomic(const std::string &path, const std::string &data, bool priv = false) {
    std::string tmp = path + ".saveserv-tmp";
    remove(tmp.c_str());
#ifndef _WIN32
    mode_t old = priv ? umask(077) : 0;
#endif
    FILE *f = fopen(tmp.c_str(), "wb");
#ifndef _WIN32
    if (priv) umask(old);
#endif
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size() && fflush(f) == 0;
#ifndef _WIN32
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
    if (ok) ok = replace_file(tmp.c_str(), path.c_str());
    if (!ok) remove(tmp.c_str());
    return ok;
}

static void config_load(void) {  // (g_m)
    g_cfg = Config();
    std::string text = read_file(config_path());
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? nl : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "server") g_cfg.server = v;
        else if (k == "token") g_cfg.token = v;
        else if (k == "account") g_cfg.account = v;
        else if (k == "device") g_cfg.device = v;
    }
}

static void config_save(void) {  // (g_m)
    std::string s = "# SaveServ client settings\n";
    const std::pair<const char *, const std::string *> rows[] = {
        {"server", &g_cfg.server}, {"device", &g_cfg.device}, {"account", &g_cfg.account}, {"token", &g_cfg.token}};
    for (auto &r : rows)
        if (!r.second->empty()) s += std::string(r.first) + "=" + *r.second + "\n";
    std::string path = config_path();
    mkdirs_for(path);
    if (!write_atomic(path, s, true)) fprintf(stderr, "[saveserv] couldn't write %s\n", path.c_str());
}

static std::string device_name(void) {
    char host[256] = "";
    const char *e = getenv("COMPUTERNAME");
    if (e) snprintf(host, sizeof host, "%s", e);
#ifndef _WIN32
    else gethostname(host, sizeof host - 1);
#endif
    return host[0] ? host : "ZA Recomp";
}

// ------------------------------------------------------------------ HTTP over curl
struct Conn {
    std::string server, token;
};
struct Resp {
    int status = 0;
    std::string body, x_rev, x_sha;
};

// (a curl config value: " and \ escaped. Control characters can't come from the server's answers, only from a typed
// address: saveserv_pair_start refuses those)
static std::string conf_quote(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

static std::string url_enc(const std::string &s) {
    std::string o;
    char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-_.~", c)) o += (char)c;
        else snprintf(b, sizeof b, "%%%02X", c), o += b;
    }
    return o;
}

// false: no HTTP answer at all (err: curl's words). The options go through a config file, so the token is never on a
// command line where other processes can read it; -q: not the user's ~/.curlrc. proto: this address is typed by a player,
// nothing but http(s) (no file://). No redirects followed: the token isn't for whoever a redirect names.
static bool http(const Conn &c, const char *method, const std::string &path, const std::string *body, Resp &r, std::string &err) {
    static std::atomic<int> seq;
    std::string dir = std::string(rt_data_root()) + "/cache";
    mkdir(dir.c_str(), 0755);
    std::string base = dir + "/saveserv-" + std::to_string((long)getpid()) + "-" + std::to_string(++seq);
    std::string conf_p = base + ".cfg", out_p = base + ".out", hdr_p = base + ".hdr", body_p = base + ".body", log_p = base + ".log";
    std::string conf = "silent\nshow-error\nconnect-timeout = 4\nmax-time = 30\nproto = \"=http,https\"\n";
    conf += "request = " + conf_quote(method) + "\nheader = \"Expect:\"\n";
    conf += "url = " + conf_quote(c.server + path) + "\noutput = " + conf_quote(out_p) + "\ndump-header = " + conf_quote(hdr_p) + "\n";
    if (!c.token.empty()) conf += "header = " + conf_quote("Authorization: Bearer " + c.token) + "\n";
    auto cleanup = [&] {
        for (const std::string *p : {&conf_p, &out_p, &hdr_p, &body_p, &log_p}) remove(p->c_str());
    };
    if (body) {
        if (!write_atomic(body_p, *body)) { err = "temp file"; cleanup(); return false; }
        conf += "header = \"Content-Type: application/octet-stream\"\ndata-binary = " + conf_quote("@" + body_p) + "\n";
    }
    if (!write_atomic(conf_p, conf, true)) { err = "temp file"; cleanup(); return false; }
#ifdef _WIN32
    const char *sr = getenv("SystemRoot");
    std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
    std::string curl = "/usr/bin/curl";
#endif
    const char *av[] = {curl.c_str(), "-q", "-K", conf_p.c_str(), NULL};
    PlatProc p = plat_spawn(av, log_p.c_str(), false);
    int rc = p ? plat_proc_wait(p, true) : -1;
    if (rc != 0) {
        err = trim(read_file(log_p));
        if (err.empty()) err = p ? "curl failed" : "curl not found";
        cleanup();
        return false;
    }
    r.body = read_file(out_p);
    // the last block of headers (an interim 100 can come first)
    std::string hdr = read_file(hdr_p);
    size_t last = hdr.rfind("\nHTTP/");  // (at a line's start: "Server: SimpleHTTP/0.6" isn't one)
    last = last != std::string::npos ? last + 1 : hdr.compare(0, 5, "HTTP/") == 0 ? 0 : std::string::npos;
    if (last != std::string::npos) {
        hdr = hdr.substr(last);
        size_t sp = hdr.find(' ');
        r.status = sp == std::string::npos ? 0 : atoi(hdr.c_str() + sp + 1);
        for (size_t at = 0; at < hdr.size();) {
            size_t nl = hdr.find('\n', at);
            std::string line = hdr.substr(at, nl == std::string::npos ? nl : nl - at);
            at = nl == std::string::npos ? hdr.size() : nl + 1;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = line.substr(0, colon);
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return (char)tolower(ch); });
            if (k == "x-rev") r.x_rev = trim(line.substr(colon + 1));
            else if (k == "x-sha256") r.x_sha = trim(line.substr(colon + 1));
        }
    }
    cleanup();
    if (!r.status) { err = "not an HTTP answer"; return false; }
    return true;
}

// ------------------------------------------------------------------ a little JSON (the server's answers, nothing nested deeper)
static void js_ws(const std::string &s, size_t &i) {
    while (i < s.size() && strchr(" \t\r\n", s[i])) i++;
}
static bool js_skip(const std::string &s, size_t &i);
static bool js_string(const std::string &s, size_t &i, std::string &out) {
    if (i >= s.size() || s[i] != '"') return false;
    out.clear();
    for (i++; i < s.size(); i++) {
        char c = s[i];
        if (c == '"') { i++; return true; }
        if (c != '\\') { out += c; continue; }
        if (++i >= s.size()) return false;
        switch (s[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            if (i + 4 >= s.size()) return false;
            unsigned u = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), NULL, 16);
            i += 4;
            if (u >= 0xD800 && u < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {  // a pair
                unsigned lo = (unsigned)strtoul(s.substr(i + 3, 4).c_str(), NULL, 16);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            if (u < 0x80) out += (char)u;
            else if (u < 0x800) out += (char)(0xC0 | u >> 6), out += (char)(0x80 | (u & 63));
            else if (u < 0x10000) out += (char)(0xE0 | u >> 12), out += (char)(0x80 | (u >> 6 & 63)), out += (char)(0x80 | (u & 63));
            else out += (char)(0xF0 | u >> 18), out += (char)(0x80 | (u >> 12 & 63)), out += (char)(0x80 | (u >> 6 & 63)), out += (char)(0x80 | (u & 63));
            break;
        }
        default: out += s[i];
        }
    }
    return false;
}
static bool js_skip(const std::string &s, size_t &i) {
    js_ws(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') { std::string t; return js_string(s, i, t); }
    if (s[i] == '{' || s[i] == '[') {
        char close = s[i] == '{' ? '}' : ']';
        for (i++;;) {
            js_ws(s, i);
            if (i < s.size() && s[i] == close) { i++; return true; }
            if (!js_skip(s, i)) return false;
            js_ws(s, i);
            if (i < s.size() && (s[i] == ':' || s[i] == ',')) i++;
        }
    }
    size_t at = i;
    while (i < s.size() && !strchr(",:}] \t\r\n", s[i])) i++;
    return i > at;  // (nothing to step over: malformed, and the caller would spin on it)
}
// an object's members: name -> the value's raw text. false: not an object
static bool js_object(const std::string &s, std::map<std::string, std::string> &out) {
    size_t i = 0;
    js_ws(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    for (i++;;) {
        js_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') return true;
        if (s[i] == ',') { i++; continue; }
        std::string k;
        if (!js_string(s, i, k)) return false;
        js_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        js_ws(s, i);
        size_t at = i;
        if (!js_skip(s, i)) return false;
        out[k] = s.substr(at, i - at);
    }
}
static std::string js_str(const std::map<std::string, std::string> &m, const char *k) {
    auto it = m.find(k);
    std::string v;
    size_t i = 0;
    return it != m.end() && js_string(it->second, i, v) ? v : "";
}
static long long js_num(const std::map<std::string, std::string> &m, const char *k) {
    auto it = m.find(k);
    return it == m.end() ? 0 : atoll(it->second.c_str());
}
static std::string js_quote(const std::string &s) {
    std::string o = "\"";
    char b[8];
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += (char)c;
        else if (c < 0x20) snprintf(b, sizeof b, "\\u%04x", c), o += b;
        else o += (char)c;
    }
    return o + "\"";
}
static std::string server_error(const Resp &r) {
    std::map<std::string, std::string> m;
    std::string e = js_object(r.body, m) ? js_str(m, "error") : "";
    return "server error " + std::to_string(r.status) + (e.empty() ? "" : ": " + e);
}

// ------------------------------------------------------------------ sync state (save/.saveserv-state, the crate's format)
struct Base {
    long long rev = 0;
    std::string sha;
};
static std::string state_path(void) { return std::string(rt_data_root()) + "/save/.saveserv-state"; }
static std::map<std::string, Base> state_load(void) {  // slot -> base; other games' lines aren't ours (and are kept out)
    std::map<std::string, Base> m;
    std::string text = read_file(state_path());
    for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? nl : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        char game[64], slot[64], sha[80];
        long long rev;
        if (sscanf(line.c_str(), "%63[^\t]\t%63[^\t]\t%lld\t%79s", game, slot, &rev, sha) == 4 && !strcmp(game, GAME_ID))
            m[slot] = {rev, sha};
    }
    return m;
}
static bool state_save(const std::map<std::string, Base> &m) {
    std::string s;
    for (auto &kv : m) s += std::string(GAME_ID) + "\t" + kv.first + "\t" + std::to_string(kv.second.rev) + "\t" + kv.second.sha + "\n";
    return write_atomic(state_path(), s);
}
static std::string sha256_hex(const std::string &d) {
    CacheSha256 h;
    h.add(d.data(), d.size());
    return h.finish();
}

// ------------------------------------------------------------------ the save folder
static std::string save_dir(void) { return std::string(rt_data_root()) + "/save/save_0"; }

// main and backup's sizes and times, as one value: changes when the game writes
static std::string folder_sig(void) {
    std::string sig;
    for (const char *slot : FILES) {
        struct stat st;
        if (stat((save_dir() + "/" + slot).c_str(), &st) == 0)
            sig += std::to_string((long long)st.st_size) + ":" + std::to_string((long long)st.st_mtime) + ";";
        else sig += "-;";
    }
    return sig;
}

// The save copied aside before a download (save/backup-<date>-sync: the Saves page lists it, a Restore puts it back. Only
// the slots: not whatever else lies in the folder). The oldest of these go; backups made by other means never do.
static bool backup_before_download(void) {
    std::string root = std::string(rt_data_root()) + "/save";
    bool any = false;
    std::string files[NFILES];
    bool have[NFILES];
    for (int i = 0; i < NFILES; i++) {
        files[i] = read_file(save_dir() + "/" + FILES[i], &have[i]);
        any = any || have[i];
    }
    if (!any) return true;
    char name[64];
    time_t now = time(NULL);
    strftime(name, sizeof name, "backup-%Y%m%d-%H%M%S-sync", localtime(&now));
    std::string dir = root + "/" + name;
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    for (int i = 0; i < NFILES; i++)
        if (have[i] && !write_atomic(dir + "/" + FILES[i], files[i])) return false;
    std::vector<std::string> mine;
    if (DIR *d = opendir(root.c_str())) {
        while (struct dirent *e = readdir(d)) {
            size_t n = strlen(e->d_name);
            if (!strncmp(e->d_name, "backup-", 7) && n > 5 && !strcmp(e->d_name + n - 5, "-sync")) mine.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(mine.begin(), mine.end());  // (names sort by date)
    for (size_t i = 0; i + KEEP_SYNC_BACKUPS < mine.size(); i++) {
        std::string old = root + "/" + mine[i];
        for (const char *slot : FILES) remove((old + "/" + slot).c_str());
        rmdir(old.c_str());  // (something else put in there: it stays)
    }
    return true;
}

// ------------------------------------------------------------------ the save as one tar (the saveserv tool's format)
enum { TAR_BLOCK = 512 };
static void tar_octal(char *field, size_t len, unsigned long long v) {  // len - 1 digits; the last byte stays 0
    char t[32];
    snprintf(t, sizeof t, "%0*llo", (int)(len - 1), v);
    memcpy(field, t + strlen(t) - (len - 1), len - 1);
}
// (only names, sizes and bytes: no times or owners, so the same save always hashes the same)
static std::string tar_pack(const std::string files[NFILES], const bool have[NFILES]) {
    std::string out;
    for (int i = 0; i < NFILES; i++) {
        if (!have[i]) continue;
        char h[TAR_BLOCK] = {};
        memcpy(h, FILES[i], strlen(FILES[i]));
        tar_octal(h + 100, 8, 0644);
        tar_octal(h + 108, 8, 0);
        tar_octal(h + 116, 8, 0);
        tar_octal(h + 124, 12, files[i].size());
        tar_octal(h + 136, 12, 0);
        memset(h + 148, ' ', 8);
        h[156] = '0';
        memcpy(h + 257, "ustar\0" "00", 8);
        unsigned long sum = 0;
        for (int k = 0; k < TAR_BLOCK; k++) sum += (unsigned char)h[k];
        char c[8];
        snprintf(c, sizeof c, "%06lo", sum);
        memcpy(h + 148, c, 7);  // (with its 0; byte 155 stays a space)
        out.append(h, TAR_BLOCK);
        out += files[i];
        out.append((TAR_BLOCK - files[i].size() % TAR_BLOCK) % TAR_BLOCK, '\0');
    }
    out.append(2 * TAR_BLOCK, '\0');
    return out;
}
static bool tar_number(const char *f, size_t len, unsigned long long &v) {
    char t[32];
    size_t n = strnlen(f, len);
    if (n >= sizeof t) return false;
    memcpy(t, f, n);
    t[n] = 0;
    char *end;
    v = strtoull(t, &end, 8);
    while (*end == ' ') end++;
    return end != t && !*end;
}
// A tar from the server, trusted in nothing: checked whole before anything on disk changes, and a name that isn't one of
// the game's two files (a path, a link, another file) fails all of it.
static bool tar_unpack(const std::string &tar, std::string files[NFILES], bool have[NFILES]) {
    for (int i = 0; i < NFILES; i++) have[i] = false;
    for (size_t at = 0; at + TAR_BLOCK <= tar.size();) {
        const char *h = tar.data() + at;
        unsigned long sum = 0;
        bool zero = true;
        for (int k = 0; k < TAR_BLOCK; k++) {
            zero = zero && !h[k];
            sum += k >= 148 && k < 156 ? 32 : (unsigned char)h[k];
        }
        if (zero) return have[1];  // (the end marker; a save without its main isn't one)
        unsigned long long want, size;
        if (!tar_number(h + 148, 8, want) || want != sum || (h[156] != '0' && h[156])) return false;
        if (strnlen(h + 345, 155) || !tar_number(h + 124, 12, size)) return false;
        std::string name(h, strnlen(h, 100));
        int i = 0;
        while (i < NFILES && name != FILES[i]) i++;
        if (i == NFILES || have[i] || size > tar.size() - at - TAR_BLOCK) return false;
        files[i] = tar.substr(at + TAR_BLOCK, size);
        have[i] = true;
        at += TAR_BLOCK + (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    }
    return false;  // (no end marker: cut short)
}

// ------------------------------------------------------------------ one sync
static void finish(SaveServState st, const std::string &message = "") {
    pthread_mutex_lock(&g_m);
    g_st.state = st;
    g_st.message = message;
    pthread_mutex_unlock(&g_m);
    if (st == SAVESERV_ERROR || st == SAVESERV_OFFLINE) fprintf(stderr, "[saveserv] %s\n", message.c_str());
}

// The server says this token is gone: the website removed this device. Forgotten here; the save stays.
static void lost_pairing(void) {
    pthread_mutex_lock(&g_m);
    g_cfg.token.clear();
    g_cfg.account.clear();
    config_save();
    g_st.state = SAVESERV_OFF;
    g_st.unpaired_remotely = true;
    g_st.account.clear();
    pthread_mutex_unlock(&g_m);
    fprintf(stderr, "[saveserv] this device was removed on the website: no more syncing until it's paired again\n");
}

// force: 0 a normal sync, 1 this device's copy wins, 2 the server's copy wins
static void do_sync(int force, bool retried = false) {
    pthread_mutex_lock(&g_m);
    Conn c = {g_cfg.server, g_cfg.token};
    bool upload_only = g_in_game;
    bool paired = !c.server.empty() && !c.token.empty();
    if (paired) g_st.state = SAVESERV_BUSY;
    pthread_mutex_unlock(&g_m);
    if (!paired) return;
    if (upload_only && force == 2) force = 0;  // (never a download under the running game)

    Resp lr;
    std::string err;
    if (!http(c, "GET", "/api/saves/" GAME_ID, NULL, lr, err)) { finish(SAVESERV_OFFLINE, err); return; }
    if (lr.status == 401) { lost_pairing(); return; }
    std::map<std::string, std::string> list;
    if (lr.status != 200 || !js_object(lr.body, list)) { finish(SAVESERV_ERROR, server_error(lr)); return; }

    // The local save. An empty file, or no main, isn't a save (the game mid-write, a failed copy): never sent, and on the
    // start screen the server's replaces it like a new device's.
    std::string files[NFILES];
    bool have[NFILES];
    bool local = true;
    for (int i = 0; i < NFILES; i++) {
        files[i] = read_file(save_dir() + "/" + FILES[i], &have[i]);
        if (have[i] && files[i].empty()) local = false;
    }
    if (!have[1]) local = false;
    std::string data, sha;
    if (local) {
        data = tar_pack(files, have);
        sha = sha256_hex(data);
    }

    bool remote = false;
    long long rrev = 0, newest = 0;
    std::string rsha, newest_device;
    auto it = list.find(SAVE_SLOT);
    std::map<std::string, std::string> rm;
    if (it != list.end() && js_object(it->second, rm)) {
        remote = true;
        rrev = js_num(rm, "rev"), newest = js_num(rm, "updated");
        rsha = js_str(rm, "sha256"), newest_device = js_str(rm, "device");
    }
    std::map<std::string, Base> base = state_load();
    auto b = base.find(SAVE_SLOT);
    bool has_base = b != base.end(), lc = false, rc = false;
    if (local && remote && sha == rsha) {  // the same bytes on both sides: just remembered
        if (!has_base || b->second.rev != rrev || b->second.sha != sha) {
            base[SAVE_SLOT] = {rrev, sha};
            if (!state_save(base)) { finish(SAVESERV_ERROR, "couldn't write " + state_path()); return; }
        }
    } else {
        lc = local && (!remote || !has_base || b->second.sha != sha);
        rc = remote && (!local || !has_base || b->second.rev != rrev);
        if (force == 1) lc = local, rc = remote && !local;
        if (force == 2) rc = remote, lc = local && !remote;
    }

    auto report = [&](SaveServState st) {
        pthread_mutex_lock(&g_m);
        g_st.remote_updated = newest;
        g_st.remote_device = newest_device;
        pthread_mutex_unlock(&g_m);
        finish(st);
    };
    if (lc && rc && !upload_only) { report(SAVESERV_CONFLICT); return; }
    if (rc && upload_only) { report(SAVESERV_NEWER); return; }  // (in game: nothing sent. The start screen sorts it out.)

    SaveServAction action = SAVESERV_NOTHING;
    if (rc) {  // a download: fetched and checked, then the local save copied aside, then swapped
        Resp r;
        if (!http(c, "GET", "/api/saves/" GAME_ID "/" SAVE_SLOT, NULL, r, err)) { finish(SAVESERV_OFFLINE, err); return; }
        if (r.status == 401) { lost_pairing(); return; }
        if (r.status != 200) { finish(SAVESERV_ERROR, server_error(r)); return; }
        std::string got_sha = sha256_hex(r.body);
        if (r.body.empty() || (!r.x_sha.empty() && got_sha != r.x_sha)) { finish(SAVESERV_ERROR, "download corrupted"); return; }
        std::string got[NFILES];
        bool got_have[NFILES];
        if (!tar_unpack(r.body, got, got_have)) { finish(SAVESERV_ERROR, "the server's save isn't one this game can read: nothing changed"); return; }
        pthread_mutex_lock(&g_m);
        bool now_in_game = g_in_game;  // (PLAY pressed during that request and its wait ran out: too late to swap)
        pthread_mutex_unlock(&g_m);
        if (now_in_game) { report(SAVESERV_NEWER); return; }
        mkdir((std::string(rt_data_root()) + "/save").c_str(), 0755);
        mkdir(save_dir().c_str(), 0755);
        if (!backup_before_download()) { finish(SAVESERV_ERROR, "couldn't back the save up: nothing downloaded"); return; }
        for (int i = 0; i < NFILES; i++) {
            std::string path = save_dir() + "/" + FILES[i];
            if (got_have[i] ? !write_atomic(path, got[i]) : (remove(path.c_str()) != 0 && errno != ENOENT)) {
                finish(SAVESERV_ERROR, "couldn't write the save: see save/backup-*-sync");
                return;
            }
        }
        base[SAVE_SLOT] = {atoll(r.x_rev.c_str()), got_sha};
        state_save(base);
        action = SAVESERV_DOWNLOADED;
        fprintf(stderr, "[saveserv] downloaded the save from %s\n", newest_device.c_str());
    } else if (lc) {
        Resp r;
        // (keeping this device's copy also answers "these bytes were deleted on the website": undelete)
        std::string path = std::string("/api/saves/" GAME_ID "/" SAVE_SLOT "?base=") + std::to_string(rrev) + (force == 1 ? "&undelete=1" : "");
        if (!http(c, "PUT", path, &data, r, err)) { finish(SAVESERV_OFFLINE, err); return; }
        if (r.status == 401) { lost_pairing(); return; }
        if (r.status == 409) {  // another device got in between the list and this: compared afresh (a conflict, now)
            if (!retried) { do_sync(force, true); return; }
            finish(SAVESERV_ERROR, "the server's save keeps changing: try again");
            return;
        }
        if (r.status == 410) { finish(SAVESERV_ERROR, "this save was deleted on the website, so it isn't uploaded again"); return; }
        if (r.status != 200) { finish(SAVESERV_ERROR, server_error(r)); return; }
        std::map<std::string, std::string> m;
        js_object(r.body, m);
        base[SAVE_SLOT] = {js_num(m, "rev"), sha};
        if (!state_save(base)) { finish(SAVESERV_ERROR, "couldn't write " + state_path()); return; }
        action = SAVESERV_UPLOADED;
        fprintf(stderr, "[saveserv] uploaded the save\n");
    }
    pthread_mutex_lock(&g_m);
    if (action != SAVESERV_NOTHING) g_st.last_action = action;
    if (action == SAVESERV_DOWNLOADED) g_st.downloads++;
    g_st.when = (long long)time(NULL);
    g_st.state = SAVESERV_SYNCED;
    g_st.message.clear();
    g_synced_once = true;
    pthread_mutex_unlock(&g_m);
}

// How the website names this game (best effort, once per run: a title instead of "za")
static void send_meta_once(void) {
    static bool sent;
    pthread_mutex_lock(&g_m);
    Conn c = {g_cfg.server, g_cfg.token};
    pthread_mutex_unlock(&g_m);
    if (sent || c.token.empty()) return;
    sent = true;
    std::string body = "{\"title\":" + js_quote("Pokémon Legends: Z-A") + ",\"slots\":{\"save\":\"Save\"}}";
    Resp r;
    std::string err;
    http(c, "PUT", "/api/meta/" GAME_ID, &body, r, err);
}

// ------------------------------------------------------------------ pairing
static void pair_poll(void) {
    pthread_mutex_lock(&g_m);
    Conn c = {g_pair_server, ""};
    std::string id = g_pair_id;
    bool expired = time(NULL) > g_pair_expires;
    pthread_mutex_unlock(&g_m);
    if (id.empty()) return;
    Resp r;
    std::string err;
    if (!http(c, "GET", "/api/pair/" + url_enc(id), NULL, r, err)) return;  // (a blip: tried again in 2 s)
    pthread_mutex_lock(&g_m);
    bool still = g_pair_id == id;
    pthread_mutex_unlock(&g_m);
    if (!still) return;  // cancelled meanwhile
    if (r.status == 202 && !expired) return;
    if (r.status == 200) {
        std::map<std::string, std::string> m;
        js_object(r.body, m);
        std::string token = js_str(m, "token");
        if (!token.empty()) {
            pthread_mutex_lock(&g_m);
            g_cfg.server = g_pair_server;
            g_cfg.token = token;
            g_cfg.account = js_str(m, "account");
            config_save();
            g_st.server = g_cfg.server, g_st.account = g_cfg.account;
            g_st.pin.clear();
            g_st.state = SAVESERV_BUSY;
            g_st.unpaired_remotely = false;
            g_pair_id.clear();
            g_req_sync = true;
            pthread_mutex_unlock(&g_m);
            fprintf(stderr, "[saveserv] paired\n");
            return;
        }
    }
    pthread_mutex_lock(&g_m);
    g_pair_id.clear();
    g_st.pin.clear();
    g_st.state = SAVESERV_ERROR;
    g_st.message = "The PIN expired: pair again.";
    pthread_mutex_unlock(&g_m);
}

static void pair_begin(const std::string &server) {
    Conn c = {server, ""};
    Resp h, r;
    std::string err;
    if (!http(c, "GET", "/api/hello", NULL, h, err)) { finish(SAVESERV_OFFLINE, err); return; }
    // (the address answered, but is it one? A token and saves don't go to a server that doesn't say so)
    std::map<std::string, std::string> hm;
    if (h.status != 200 || !js_object(h.body, hm) || js_str(hm, "service") != "saveserv") {
        finish(SAVESERV_ERROR, server + " isn't a SaveServ server.");
        return;
    }
    std::string body = "{\"name\":" + js_quote(device_name()) + "}";
    if (!http(c, "POST", "/api/pair", &body, r, err)) { finish(SAVESERV_OFFLINE, err); return; }
    std::map<std::string, std::string> m;
    if (r.status != 200 || !js_object(r.body, m)) { finish(SAVESERV_ERROR, server_error(r)); return; }
    pthread_mutex_lock(&g_m);
    g_pair_server = server;
    g_pair_id = js_str(m, "id");
    g_pair_expires = time(NULL) + (time_t)js_num(m, "expires");
    g_st.pin = js_str(m, "pin");
    g_st.server = server;
    g_st.state = SAVESERV_PAIRING;
    g_st.message.clear();
    g_cfg.server = server;  // (remembered for next time, paired or not)
    config_save();
    pthread_mutex_unlock(&g_m);
}

// ------------------------------------------------------------------ the thread
static std::string g_pair_req;
static std::string g_game_sig;  // the save folder when the game started (taken there: this thread looks up to 2 s later)

static void *worker(void *) {
    std::string seen_sig;      // in game: the folder as last looked at, and the time it last changed
    time_t changed_at = 0, last_try = 0;
    bool was_in_game = false;
    pthread_mutex_lock(&g_m);
    for (;;) {
        // (timed: the pairing poll and the in-game look at the save folder. Nothing else wakes it while the game runs)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (!g_req_sync && !g_req_force && g_pair_req.empty()) pthread_cond_timedwait(&g_wake, &g_m, &ts);

        bool in_game = g_in_game, paired = !g_cfg.token.empty();
        std::string game_sig = g_game_sig;
        std::string pair_req;
        pair_req.swap(g_pair_req);
        bool sync_now = g_req_sync;
        int force = g_req_force;
        g_req_sync = false, g_req_force = 0;
        bool polling = !g_pair_id.empty();
        g_busy = !pair_req.empty() || sync_now || force || polling;
        pthread_mutex_unlock(&g_m);

        if (!pair_req.empty()) pair_begin(pair_req);
        else if (polling) pair_poll();
        else if (paired) {
            if (in_game != was_in_game) {  // the game started: its save as it stood then was synced just before
                seen_sig = game_sig;
                changed_at = last_try = 0;
                was_in_game = in_game;
            }
            bool auto_send = false;
            if (in_game) {
                std::string sig = folder_sig();
                time_t now = time(NULL);
                if (sig != seen_sig) seen_sig = sig, changed_at = now;
                auto_send = changed_at && now - changed_at >= SETTLE_S && now - last_try >= MIN_UPLOAD_S;
            }
            if (sync_now || force || auto_send) {
                changed_at = 0;  // (an upload that fails isn't retried on a timer: the next save, quit or start sends it)
                last_try = time(NULL);
                send_meta_once();
                do_sync(force);
            }
        }
        pthread_mutex_lock(&g_m);
        g_busy = false;
        pthread_cond_broadcast(&g_idle);
    }
    return NULL;
}

static void ensure_thread(void) {  // (g_m)
    if (g_thread_up) return;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);
        g_thread_up = true;
    }
}

static void init_locked(void) {
    if (g_inited) return;
    g_inited = true;
    config_load();
    g_st.server = g_cfg.server;
    g_st.account = g_cfg.account;
    if (!g_cfg.token.empty()) g_st.state = SAVESERV_SYNCED;  // (when = 0: nothing yet)
    if (!g_cfg.token.empty()) ensure_thread();
}

// ------------------------------------------------------------------ the rest of the runtime
extern "C" void saveserv_init(void) {
    pthread_mutex_lock(&g_m);
    init_locked();
    pthread_mutex_unlock(&g_m);
}

extern "C" void saveserv_kick(void) {
    pthread_mutex_lock(&g_m);
    init_locked();
    if (!g_cfg.token.empty()) {
        ensure_thread();
        g_req_sync = true;
        g_st.state = SAVESERV_BUSY;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_m);
}

// wait (g_m held) for what's running and asked for, at most max_ms
static void wait_idle(int max_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += max_ms / 1000;
    ts.tv_nsec += (max_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    while (g_busy || g_req_sync || g_req_force)
        if (pthread_cond_timedwait(&g_idle, &g_m, &ts) == ETIMEDOUT) break;
}

extern "C" void saveserv_game_starting(void) {
    pthread_mutex_lock(&g_m);
    init_locked();
    if (!g_cfg.token.empty()) {
        // (--play, or PLAY right after a reboot: no start screen to have synced first. A conflict stays one: the save
        // isn't touched, and the next start screen asks)
        if (!g_synced_once && !g_busy) {
            ensure_thread();
            g_req_sync = true;
            pthread_cond_signal(&g_wake);
        }
        wait_idle(WAIT_MAX_MS);
    }
    g_game_sig = folder_sig();
    g_in_game = true;
    g_st.in_game = true;
    pthread_mutex_unlock(&g_m);
}

extern "C" void saveserv_game_stopped(void) {
    pthread_mutex_lock(&g_m);
    g_in_game = false;
    g_st.in_game = false;
    g_synced_once = false;
    pthread_mutex_unlock(&g_m);
}

extern "C" void saveserv_flush(int max_ms) {
    pthread_mutex_lock(&g_m);
    if (g_inited && !g_cfg.token.empty() && g_thread_up) {
        g_req_sync = true;  // (the compare costs one small request when nothing changed)
        pthread_cond_signal(&g_wake);
        wait_idle(max_ms);
    }
    pthread_mutex_unlock(&g_m);
}

// ------------------------------------------------------------------ the pages (runtime/ui/saveserv_ui.mm)
SaveServStatus saveserv_status(void) {
    pthread_mutex_lock(&g_m);
    init_locked();
    SaveServStatus s = g_st;
    pthread_mutex_unlock(&g_m);
    return s;
}

std::string saveserv_server_hint(void) {
    pthread_mutex_lock(&g_m);
    init_locked();
    std::string s = g_cfg.server;
    pthread_mutex_unlock(&g_m);
    return s;
}

void saveserv_pair_start(const char *server) {
    std::string s = trim(server ? server : "");
    while (!s.empty() && s.back() == '/') s.pop_back();
    bool bad = s.empty();
    for (unsigned char c : s) bad = bad || c <= ' ' || c == 0x7f || c == '"' || c == '\\';
    pthread_mutex_lock(&g_m);
    init_locked();
    if (bad) {
        g_st.state = SAVESERV_ERROR;
        g_st.message = "That isn't a server address.";
    } else {
        if (s.find("://") == std::string::npos) s = "http://" + s;
        ensure_thread();
        g_pair_req = s;
        g_st.state = SAVESERV_BUSY;
        g_st.message.clear();
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_m);
}

void saveserv_pair_cancel(void) {
    pthread_mutex_lock(&g_m);
    g_pair_id.clear();
    g_st.pin.clear();
    g_st.state = g_cfg.token.empty() ? SAVESERV_OFF : SAVESERV_SYNCED;
    pthread_mutex_unlock(&g_m);
}

void saveserv_unpair(void) {
    pthread_mutex_lock(&g_m);
    g_cfg.token.clear();
    g_cfg.account.clear();
    config_save();
    g_st.state = SAVESERV_OFF;
    g_st.account.clear();
    g_st.message.clear();
    g_st.when = 0;
    pthread_mutex_unlock(&g_m);
}

void saveserv_resolve(bool keep_this_device) {
    pthread_mutex_lock(&g_m);
    if (!g_cfg.token.empty() && (!g_in_game || keep_this_device)) {
        ensure_thread();
        g_req_force = keep_this_device ? 1 : 2;
        g_st.state = SAVESERV_BUSY;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_m);
}
