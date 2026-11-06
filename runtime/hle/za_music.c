// Pokémon Z-A: the player's own music in place of the game's. Songs go in music/ or mods/Music/, subfolders too (an
// album as it unpacks) (mp3, flac, wav here; ogg, opus, m4a through FFmpeg, as far as the build's FFmpeg has them); the
// F1 menu's Sound page assigns one to the track playing now.
// Never touches game code (no game-code rebuild), and the game can't tell: Wwise still plays "its" stream, so the
// crossfades, the battle transitions, the ducking under dialogue and the game's own BGM slider all keep working.
//
// How: Wwise streams its music from the loose RomFS file audio/wwise/bin/Switch/Stream.pck (and Stream_dlc.pck) as
// PTADPCM (format tag 0x8311: 36 bytes per channel per block, 64 samples, so 54000 bytes a second for 48 kHz stereo,
// whatever the content). A fixed rate, and each block starts from two raw samples and holds no state from the one
// before: a read of the stream at any offset can be served from any block of another song. So a song is encoded once
// to the same format (cached, cache/music/) and za_music_filter (runtime/services/fs.c, each read of the base image)
// overlays its blocks on what the read returned. The stream's header stays the game's.
//
// What this has to deal with, all seen in BGM.bnk (2.0.2):
// - One music segment can hold several streams playing together: stems, one 243.8 s segment has 7, uncorrelated and
//   each far below the mix's level (-30 dBFS each, -17 summed). So an assignment is per segment (a union of its
//   streams), its id the lowest stream id; the first stem read carries the song and the rest are served silence.
//   The game's streams keep playing, inaudibly, so no read pattern changes. (All-zero bytes wouldn't do: nibble 0 is
//   the largest negative step. Silence is index 0, nibbles 8: step 0.)
// - Segments are short next to a song (69 s, 244 s): Wwise rereads the stream at each loop. A plain offset mapping
//   would restart the song each time, so the song goes on: a read near the start after one past the middle moves
//   its base on by the segment's length. A voice still reading the old pass's tail right after (loops overlap) keeps
//   the old base. After 30 s of no reads, a read from the start begins the song again.
// - Levels: the game's mix sits near -18 dBFS (long tracks: median -20), a mastered remix near -12 with peaks at full
//   scale. Songs are brought to -18 dBFS RMS, less if their peaks need it (this is also what keeps PTADPCM clean: a
//   -1 dBFS RMS master measures 20 dB SNR, a -12 one 35).
//
// Known limits: 39 short streams (31..60 s) are "prefetched", their first bytes held in BGM.bnk in memory rather than
// read from the pck, so those start with a blip of the original. A track made of several segments in a row needs each
// assigned (each is a track here).
//
// Guest threads call the filter: the check on every read is two compares of values that never change after setup,
// and the lock is only taken for reads of a music stream.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#ifndef ZA_MUSIC_NO_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../../third_party/dr_libs/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../third_party/dr_libs/dr_wav.h"

#include "../platform.h"
#include "../rt.h"
#include "../za_music.h"

#define ASSIGN_FILE "music/assignments.txt"
#define CACHE_DIR "cache/music"
#define TARGET_DBFS (-18.0)
#define PEAK_MAX 0.89  // -1 dBFS
#define LEADER_IDLE_NS 5000000000ull  // a stem unread this long: another carries the song
#define RESTART_IDLE_NS 30000000000ull
#define TAIL_NS 3000000000ull
#define MAX_FILE_BYTES (1024ull << 20)

// ------------------------------------------------------------------ PTADPCM
// (the table's values from vgmstream's ptadpcm_decoder.c, checked against the game's streams: decoded, a block's end
// runs into the next block's raw samples as smoothly as samples within a block, across a whole track)
#define PT_SAMPLES 64
#define PT_FRAME 36
static const int32_t pt_tab[12][16][2] = {
    {{-14, 2}, {-10, 2}, {-7, 1}, {-5, 1}, {-3, 0}, {-2, 0}, {-1, 0}, {0, 0}, {0, 0}, {1, 0}, {2, 0}, {3, 0}, {5, 1}, {7, 1}, {10, 2}, {14, 2}},
    {{-28, 3}, {-20, 3}, {-14, 2}, {-10, 2}, {-7, 1}, {-5, 1}, {-3, 1}, {-1, 0}, {1, 0}, {3, 1}, {5, 1}, {7, 1}, {10, 2}, {14, 2}, {20, 3}, {28, 3}},
    {{-56, 4}, {-40, 4}, {-28, 3}, {-20, 3}, {-14, 2}, {-10, 2}, {-6, 2}, {-2, 1}, {2, 1}, {6, 2}, {10, 2}, {14, 2}, {20, 3}, {28, 3}, {40, 4}, {56, 4}},
    {{-112, 5}, {-80, 5}, {-56, 4}, {-40, 4}, {-28, 3}, {-20, 3}, {-12, 3}, {-4, 2}, {4, 2}, {12, 3}, {20, 3}, {28, 3}, {40, 4}, {56, 4}, {80, 5}, {112, 5}},
    {{-224, 6}, {-160, 6}, {-112, 5}, {-80, 5}, {-56, 4}, {-40, 4}, {-24, 4}, {-8, 3}, {8, 3}, {24, 4}, {40, 4}, {56, 4}, {80, 5}, {112, 5}, {160, 6}, {224, 6}},
    {{-448, 7}, {-320, 7}, {-224, 6}, {-160, 6}, {-112, 5}, {-80, 5}, {-48, 5}, {-16, 4}, {16, 4}, {48, 5}, {80, 5}, {112, 5}, {160, 6}, {224, 6}, {320, 7}, {448, 7}},
    {{-896, 8}, {-640, 8}, {-448, 7}, {-320, 7}, {-224, 6}, {-160, 6}, {-96, 6}, {-32, 5}, {32, 5}, {96, 6}, {160, 6}, {224, 6}, {320, 7}, {448, 7}, {640, 8}, {896, 8}},
    {{-1792, 9}, {-1280, 9}, {-896, 8}, {-640, 8}, {-448, 7}, {-320, 7}, {-192, 7}, {-64, 6}, {64, 6}, {192, 7}, {320, 7}, {448, 7}, {640, 8}, {896, 8}, {1280, 9}, {1792, 9}},
    {{-3584, 10}, {-2560, 10}, {-1792, 9}, {-1280, 9}, {-896, 8}, {-640, 8}, {-384, 8}, {-128, 7}, {128, 7}, {384, 8}, {640, 8}, {896, 8}, {1280, 9}, {1792, 9}, {2560, 10}, {3584, 10}},
    {{-7168, 11}, {-5120, 11}, {-3584, 10}, {-2560, 10}, {-1792, 9}, {-1280, 9}, {-768, 9}, {-256, 8}, {256, 8}, {768, 9}, {1280, 9}, {1792, 9}, {2560, 10}, {3584, 10}, {5120, 11}, {7168, 11}},
    {{-14336, 11}, {-10240, 11}, {-7168, 11}, {-5120, 11}, {-3584, 10}, {-2560, 10}, {-1536, 10}, {-512, 9}, {512, 9}, {1536, 10}, {2560, 10}, {3584, 10}, {5120, 11}, {7168, 11}, {10240, 11}, {14336, 11}},
    {{-28672, 11}, {-20480, 11}, {-14336, 11}, {-10240, 11}, {-7168, 11}, {-5120, 11}, {-3072, 11}, {-1024, 10}, {1024, 10}, {3072, 11}, {5120, 11}, {7168, 11}, {10240, 11}, {14336, 11}, {20480, 11}, {28672, 11}},
};

static inline int pt_clamp(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

// One channel's block from PT_SAMPLES samples: each start index tried, the nearest step taken at each sample.
static void pt_encode(const int16_t *x, uint8_t *f) {
    enum { N = PT_SAMPLES - 2 };
    uint64_t best = UINT64_MAX;
    uint8_t bn[N] = {0}, nb[N];
    int bix = 0;
    for (int s = 0; s < 12; s++) {
        int h2 = x[0], h1 = x[1], ix = s;
        uint64_t e = 0;
        int i;
        for (i = 0; i < N; i++) {
            int p = 2 * h1 - h2, t = x[2 + i], bk = 0, be = INT32_MAX;
            for (int k = 0; k < 16; k++) {
                int d = abs(pt_clamp(pt_tab[ix][k][0] + p) - t);
                if (d < be || (d == be && pt_tab[ix][k][1] < pt_tab[ix][bk][1])) be = d, bk = k;
            }
            int v = pt_clamp(pt_tab[ix][bk][0] + p);
            e += (uint64_t)((int64_t)(v - t) * (v - t));
            nb[i] = (uint8_t)bk;
            ix = pt_tab[ix][bk][1];
            h2 = h1, h1 = v;
            if (e >= best) break;
        }
        if (i == N && e < best) best = e, bix = s, memcpy(bn, nb, N);
    }
    f[0] = (uint8_t)x[0], f[1] = (uint8_t)((uint16_t)x[0] >> 8);
    f[2] = (uint8_t)x[1], f[3] = (uint8_t)((uint16_t)x[1] >> 8);
    f[4] = (uint8_t)bix;
    for (int i = 0; i < PT_FRAME - 5; i++) f[5 + i] = (uint8_t)(bn[2 * i] | bn[2 * i + 1] << 4);
}

// ------------------------------------------------------------------ songs
typedef struct {
    uint32_t rate;
    uint16_t ch;
    uint8_t *data;  // written once, before `ready` is set
    uint64_t blocks;
    volatile int ready;   // 0 not yet, 1 data can be read, -1 can't be made
    bool queued;
} Variant;

typedef struct Song {
    char file[256];
    Variant v[4];  // one per format among the streams it's assigned to (48 kHz stereo nearly always)
    int nv;
    struct Song *next;
} Song;  // (never freed: a read may be using one, and there are as many as there are files in music/)

// ------------------------------------------------------------------ the index (fixed after setup)
typedef struct {
    uint64_t off, size, data;  // image offsets: the entry, its size, its first block
    uint32_t id, rate, block_bytes;
    uint16_t ch;
    int group;
    uint64_t last_block, last_ns;
} Wem;

typedef struct {
    uint32_t id;
    int stems;
    uint64_t blocks;  // its longest stream's
    uint32_t block_bytes;
    Song *song;
    bool silent;
    int leader;  // index in g_w, -1 none
    uint64_t last_ns, restart_ns, base;
} Group;

static Wem *g_w;
static int g_nw;
static Group *g_g;
static int g_ng;
static uint64_t g_lo, g_hi;  // the image range holding every music stream
static int g_last_group = -1;
static Song *g_songs;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static bool g_worker;

// ------------------------------------------------------------------ decoding a file to float samples
#ifndef ZA_MUSIC_NO_FFMPEG
static float *decode_ffmpeg(const char *path, uint64_t *frames, uint32_t *rate, uint32_t *ch) {
    AVFormatContext *fmt = NULL;
    AVCodecContext *c = NULL;
    AVPacket *pkt = NULL;
    AVFrame *fr = NULL;
    float *out = NULL;
    uint64_t have = 0, cap = 0;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return NULL;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
    const AVCodec *dec = NULL;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (si < 0 || !dec) goto done;
    c = avcodec_alloc_context3(dec);
    if (!c || avcodec_parameters_to_context(c, fmt->streams[si]->codecpar) < 0 || avcodec_open2(c, dec, NULL) < 0) goto done;
    pkt = av_packet_alloc(), fr = av_frame_alloc();
    if (!pkt || !fr) goto done;
    uint32_t nch = 0;
    bool eof = false;
    while (!eof) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) { eof = true; avcodec_send_packet(c, NULL); }
        else if (pkt->stream_index == si) avcodec_send_packet(c, pkt);
        if (r >= 0) av_packet_unref(pkt);
        while (avcodec_receive_frame(c, fr) == 0) {
            // (no swresample in the release builds' FFmpeg: the formats its decoders give, by hand)
            uint32_t n = (uint32_t)fr->ch_layout.nb_channels;
            if (!nch) nch = n, *ch = n, *rate = (uint32_t)fr->sample_rate;
            if (n != nch) continue;
            if (have + (uint64_t)fr->nb_samples > cap) {
                cap = (have + (uint64_t)fr->nb_samples) * 2;
                float *g = realloc(out, cap * nch * sizeof(float));
                if (!g) { free(out); out = NULL; have = 0; goto done; }
                out = g;
            }
            for (int s = 0; s < fr->nb_samples; s++)
                for (uint32_t k = 0; k < nch; k++) {
                    float v = 0;
                    switch (fr->format) {
                    case AV_SAMPLE_FMT_FLTP: v = ((const float *)fr->extended_data[k])[s]; break;
                    case AV_SAMPLE_FMT_FLT: v = ((const float *)fr->extended_data[0])[s * nch + k]; break;
                    case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)fr->extended_data[k])[s] / 32768.0f; break;
                    case AV_SAMPLE_FMT_S16: v = ((const int16_t *)fr->extended_data[0])[s * nch + k] / 32768.0f; break;
                    case AV_SAMPLE_FMT_S32P: v = (float)(((const int32_t *)fr->extended_data[k])[s] / 2147483648.0); break;
                    case AV_SAMPLE_FMT_S32: v = (float)(((const int32_t *)fr->extended_data[0])[s * nch + k] / 2147483648.0); break;
                    default: break;
                    }
                    out[(have + (uint64_t)s) * nch + k] = v;
                }
            have += (uint64_t)fr->nb_samples;
        }
    }
done:
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    if (!have) { free(out); return NULL; }
    *frames = have;
    return out;
}
#endif

// (what these give goes to free(): copied, so the caller has one way to let go)
static float *owned(float *p, uint64_t frames, uint32_t ch, void (*fr)(void *, const void *)) {
    if (!p) return NULL;
    float *o = malloc(frames * ch * sizeof(float));
    if (o) memcpy(o, p, frames * ch * sizeof(float));
    fr(p, NULL);
    return o;
}
static void free_mp3(void *p, const void *a) { (void)a; drmp3_free(p, NULL); }
static void free_flac(void *p, const void *a) { (void)a; drflac_free(p, NULL); }
static void free_wav(void *p, const void *a) { (void)a; drwav_free(p, NULL); }

static bool ext_is(const char *path, const char *ext) {
    const char *d = strrchr(path, '.');
    return d && !SDL_strcasecmp(d, ext);
}

static float *decode_file(const char *path, uint64_t *frames, uint32_t *rate, uint32_t *ch) {
    bool mp3 = ext_is(path, ".mp3"), flac = ext_is(path, ".flac"), wav = ext_is(path, ".wav");
    if (mp3 || flac || wav) {
        FILE *f = fopen(path, "rb");
        if (!f) return NULL;
        struct stat sb;
        if (fstat(fileno(f), &sb) || (uint64_t)sb.st_size > MAX_FILE_BYTES) { fclose(f); return NULL; }
        uint8_t *d = malloc((size_t)sb.st_size + 1);
        size_t n = d ? fread(d, 1, (size_t)sb.st_size, f) : 0;
        fclose(f);
        if (!d) return NULL;
        float *o = NULL;
        unsigned int c = 0, r = 0;
        if (mp3) {
            drmp3_config cfg;
            drmp3_uint64 fr = 0;
            float *p = drmp3_open_memory_and_read_pcm_frames_f32(d, n, &cfg, &fr, NULL);
            c = cfg.channels, r = cfg.sampleRate, *frames = fr;
            o = owned(p, fr, c, free_mp3);
        } else if (flac) {
            drflac_uint64 fr = 0;
            float *p = drflac_open_memory_and_read_pcm_frames_f32(d, n, &c, &r, &fr, NULL);
            *frames = fr;
            o = owned(p, fr, c, free_flac);
        } else {
            drwav_uint64 fr = 0;
            float *p = drwav_open_memory_and_read_pcm_frames_f32(d, n, &c, &r, &fr, NULL);
            *frames = fr;
            o = owned(p, fr, c, free_wav);
        }
        free(d);
        *rate = r, *ch = c;
        return o;
    }
#ifndef ZA_MUSIC_NO_FFMPEG
    return decode_ffmpeg(path, frames, rate, ch);
#else
    return NULL;
#endif
}

// ------------------------------------------------------------------ making a song's blocks (a worker thread)
static uint64_t hash_str(uint64_t h, const char *s) {
    while (*s) h = (h ^ (uint8_t)*s++) * 0x100000001B3ull;
    return h;
}
static uint64_t hash_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) h = (h ^ ((v >> (i * 8)) & 255)) * 0x100000001B3ull;
    return h;
}

// PCM at the stream's format, levelled; *out_frames samples, 2 s of silence between the loops of a song
static bool prepare_pcm(float *in, uint64_t frames, uint32_t rate, uint32_t ch, const Variant *v, int16_t **out,
                        uint64_t *out_frames) {
    // RMS over what isn't near silence (a quiet intro or a long fade shouldn't make the rest louder)
    double sum = 0, peak = 0;
    uint64_t counted = 0;
    const uint64_t win = rate / 10 ? rate / 10 : 1;
    for (uint64_t a = 0; a < frames; a += win) {
        uint64_t b = a + win < frames ? a + win : frames;
        double w = 0;
        for (uint64_t i = a * ch; i < b * ch; i++) {
            double s = in[i];
            w += s * s;
            if (fabs(s) > peak) peak = fabs(s);
        }
        if (w / (double)((b - a) * ch) > 1e-5) sum += w, counted += (b - a) * ch;  // (above -50 dBFS)
    }
    double gain = 1;
    if (counted) {
        double rms = sqrt(sum / (double)counted);
        gain = pow(10, TARGET_DBFS / 20) / (rms > 1e-6 ? rms : 1e-6);
    }
    if (peak > 0 && gain * peak > PEAK_MAX) gain = PEAK_MAX / peak;
    for (uint64_t i = 0; i < frames * ch; i++) in[i] = (float)(in[i] * gain);
    const uint64_t fade = rate / 20;  // (no click where the song meets the silence)
    for (uint64_t i = 0; i < fade && i < frames; i++) {
        float g = (float)i / (float)fade;
        for (uint32_t k = 0; k < ch; k++) in[(frames - 1 - i) * ch + k] *= g;
    }
    LOG("[music] %llu frames, %u Hz, %u channels: gain %.1f dB\n", (unsigned long long)frames, rate, ch, 20 * log10(gain));

    SDL_AudioSpec src = {SDL_AUDIO_F32, (int)ch, (int)rate}, dst = {SDL_AUDIO_S16, v->ch, (int)v->rate};
    Uint8 *conv = NULL;
    int conv_len = 0;
    if ((uint64_t)frames * ch * sizeof(float) > INT32_MAX) return false;
    if (!SDL_ConvertAudioSamples(&src, (const Uint8 *)in, (int)(frames * ch * sizeof(float)), &dst, &conv, &conv_len)) {
        LOG("[music] can't convert: %s\n", SDL_GetError());
        return false;
    }
    uint64_t got = (uint64_t)conv_len / (2u * v->ch), gap = 2ull * v->rate;
    int16_t *o = calloc(got + gap + PT_SAMPLES, 2u * v->ch);
    if (!o) { SDL_free(conv); return false; }
    memcpy(o, conv, (size_t)conv_len);
    SDL_free(conv);
    *out = o, *out_frames = got + gap;
    return true;
}

static void make_variant(Song *song, Variant *v) {
    char path[320], cpath[64];
    // (a path from the data root, as the menu gives it; a bare name, written by hand: in music/)
    if (!strncmp(song->file, "music/", 6) || !strncmp(song->file, "mods/", 5)) snprintf(path, sizeof path, "%s", song->file);
    else snprintf(path, sizeof path, "music/%s", song->file);
    struct stat sb;
    if (stat(path, &sb)) {
        LOG("[music] %s: not found\n", path);
        v->ready = -1;
        return;
    }
    uint64_t h = hash_u64(hash_u64(hash_u64(hash_u64(hash_str(0xCBF29CE484222645ull, song->file), (uint64_t)sb.st_size),
                                            (uint64_t)sb.st_mtime), v->rate), v->ch);
    snprintf(cpath, sizeof cpath, CACHE_DIR "/%016llx.pta", (unsigned long long)h);
    const uint32_t block_bytes = (uint32_t)PT_FRAME * v->ch;
    FILE *f = fopen(cpath, "rb");
    if (f) {
        char magic[4];
        uint64_t blocks = 0;
        if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "ZMU1", 4) && fread(&blocks, 8, 1, f) == 1 &&
            blocks && blocks < MAX_FILE_BYTES) {
            uint8_t *d = malloc(blocks * block_bytes);
            if (d && fread(d, block_bytes, blocks, f) == blocks) {
                fclose(f);
                v->data = d, v->blocks = blocks;
                __atomic_store_n(&v->ready, 1, __ATOMIC_RELEASE);
                LOG("[music] %s: from the cache, %.0f s\n", song->file, (double)blocks * PT_SAMPLES / v->rate);
                return;
            }
            free(d);
        }
        fclose(f);
    }
    uint64_t t0 = plat_now_ns(), frames = 0, nout = 0;
    uint32_t rate = 0, ch = 0;
    float *pcm = decode_file(path, &frames, &rate, &ch);
    int16_t *o = NULL;
    if (!pcm || !frames || !rate || !ch || !prepare_pcm(pcm, frames, rate, ch, v, &o, &nout)) {
        LOG("[music] %s: can't read this file (format not supported by this build?)\n", song->file);
        free(pcm);
        v->ready = -1;
        return;
    }
    free(pcm);
    uint64_t blocks = nout / PT_SAMPLES;
    uint8_t *d = malloc(blocks * block_bytes);
    if (!d) { free(o); v->ready = -1; return; }
    int16_t x[PT_SAMPLES];
    for (uint64_t b = 0; b < blocks; b++)
        for (uint32_t k = 0; k < v->ch; k++) {
            for (int s = 0; s < PT_SAMPLES; s++) x[s] = o[(b * PT_SAMPLES + (uint64_t)s) * v->ch + k];
            pt_encode(x, d + b * block_bytes + k * PT_FRAME);
        }
    free(o);
    SDL_CreateDirectory(CACHE_DIR);
    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.tmp", cpath);
    f = fopen(tmp, "wb");
    if (f) {
        bool ok = fwrite("ZMU1", 1, 4, f) == 4 && fwrite(&blocks, 8, 1, f) == 1 && fwrite(d, block_bytes, blocks, f) == blocks;
        ok = !fclose(f) && ok;
        if (!ok || !SDL_RenamePath(tmp, cpath)) remove(tmp);
    }
    v->data = d, v->blocks = blocks;
    __atomic_store_n(&v->ready, 1, __ATOMIC_RELEASE);
    LOG("[music] %s: encoded, %.0f s of music in %.1f s\n", song->file, (double)blocks * PT_SAMPLES / v->rate,
        (double)(plat_now_ns() - t0) / 1e9);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        Song *song = NULL;
        Variant *v = NULL;
        pthread_mutex_lock(&g_lock);
        while (!v) {
            for (Song *s = g_songs; s && !v; s = s->next)
                for (int i = 0; i < s->nv && !v; i++)
                    if (s->v[i].queued && !s->v[i].ready) song = s, v = &s->v[i];
            if (v) v->queued = false;
            else pthread_cond_wait(&g_q_cond, &g_lock);
        }
        pthread_mutex_unlock(&g_lock);
        make_variant(song, v);  // (no lock: seconds of work, and reads of the game's music must not wait for it)
    }
    return NULL;
}

// (g_lock held) the song for a file, its variants for the group's formats queued
static Song *song_for(const char *file, int group) {
    Song *s = g_songs;
    while (s && strcmp(s->file, file)) s = s->next;
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) return NULL;
        snprintf(s->file, sizeof s->file, "%s", file);
        s->next = g_songs;
        g_songs = s;
    }
    for (int i = 0; i < g_nw; i++) {
        if (g_w[i].group != group) continue;
        int k = 0;
        while (k < s->nv && (s->v[k].rate != g_w[i].rate || s->v[k].ch != g_w[i].ch)) k++;
        if (k == s->nv && s->nv < 4) {
            s->v[k].rate = g_w[i].rate, s->v[k].ch = g_w[i].ch, s->v[k].queued = true;
            s->nv++;
        }
    }
    if (!g_worker) {
        pthread_t t;
        g_worker = !pthread_create(&t, NULL, worker, NULL);
        if (g_worker) pthread_detach(t);
    }
    pthread_cond_signal(&g_q_cond);
    return s;
}

// ------------------------------------------------------------------ assignments
static int group_by_id(uint32_t id) {
    for (int i = 0; i < g_ng; i++) if (g_g[i].id == id) return i;
    return -1;
}

static void save_assignments(void) {  // (g_lock held)
    SDL_CreateDirectory("music");
    FILE *f = fopen(ASSIGN_FILE, "w");
    if (!f) { LOG("[music] can't write %s\n", ASSIGN_FILE); return; }
    fputs("# Which song plays in place of which track of the game: <track id> <file in this folder, or - for silence>\n"
          "# (made by the F1 menu's Sound page)\n", f);
    for (int i = 0; i < g_ng; i++) {
        if (g_g[i].silent) fprintf(f, "%u -\n", g_g[i].id);
        else if (g_g[i].song) fprintf(f, "%u %s\n", g_g[i].id, g_g[i].song->file);
    }
    fclose(f);
}

static void set_group(int g, const char *file) {  // (g_lock held)
    g_g[g].silent = file && !strcmp(file, "-");
    g_g[g].song = file && file[0] && !g_g[g].silent ? song_for(file, g) : NULL;
    g_g[g].leader = -1;
}

void za_music_assign(uint32_t group, const char *file) {
    pthread_mutex_lock(&g_lock);
    int g = group_by_id(group);
    if (g >= 0) {
        set_group(g, file);
        save_assignments();
        LOG("[music] track %u -> %s\n", group, file && file[0] ? file : "(the game's own)");
    }
    pthread_mutex_unlock(&g_lock);
}

int za_music_assigned_count(void) {
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ng; i++) n += g_g[i].silent || g_g[i].song;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void za_music_clear_all(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ng; i++) set_group(i, NULL);
    save_assignments();
    pthread_mutex_unlock(&g_lock);
}

bool za_music_now(ZaMusicNow *out) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    if (g_last_group >= 0) {
        const Group *g = &g_g[g_last_group];
        out->group = g->id;
        out->stems = g->stems;
        out->file[0] = 0;
        out->state = ZA_MUSIC_ORIGINAL;
        double rate = 48000;
        for (int i = 0; i < g_nw; i++) if (g_w[i].group == g_last_group) { rate = g_w[i].rate; break; }
        out->seconds = (double)g->blocks * PT_SAMPLES / rate;
        if (g->silent) {
            out->state = ZA_MUSIC_SILENCE;
            snprintf(out->file, sizeof out->file, "-");
        } else if (g->song) {
            snprintf(out->file, sizeof out->file, "%s", g->song->file);
            int ready = 1, failed = 0;
            for (int i = 0; i < g->song->nv; i++) {
                int r = __atomic_load_n(&g->song->v[i].ready, __ATOMIC_ACQUIRE);
                if (r == 0) ready = 0;
                if (r < 0) failed = 1;
            }
            out->state = failed ? ZA_MUSIC_FAILED : ready ? ZA_MUSIC_ACTIVE : ZA_MUSIC_PREPARING;
        }
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

// ------------------------------------------------------------------ the read overlay
static void fill_silence(uint8_t *dst, uint64_t n, uint64_t pos, const Wem *w) {
    const uint32_t fr = w->block_bytes / w->ch;
    for (uint64_t i = 0; i < n; i++) dst[i] = (pos + i) % fr < 5 ? 0 : 0x88;
}

// (g_lock held) [a, b): the part of the read in this stream's blocks
static void overlay_wem(Wem *w, Group *g, int wi, uint8_t *dst, uint64_t a, uint64_t b, uint64_t now) {
    const uint64_t pos = a - w->data, block = pos / w->block_bytes, len = (w->off + w->size - w->data) / w->block_bytes;
    const Variant *v = NULL;
    if (g->song) {
        for (int i = 0; i < g->song->nv; i++)
            if (g->song->v[i].rate == w->rate && g->song->v[i].ch == w->ch &&
                __atomic_load_n(&g->song->v[i].ready, __ATOMIC_ACQUIRE) == 1)
                v = &g->song->v[i];
        if (!v) return;  // (being made, or can't be: the game's own until then)
    }
    const uint64_t since = now - g->last_ns;
    const bool near_start = block < (len / 4 < 750 * 4 ? len / 4 : 750 * 4);  // (a quarter of it, 4 s at most)
    // (its own reads, not the group's: the other stems go on being read when it stops)
    if (g->leader < 0 || now - g_w[g->leader].last_ns > LEADER_IDLE_NS) g->leader = wi;
    if (since > RESTART_IDLE_NS && near_start) g->base = 0, w->last_block = 0;

    if (g->silent || wi != g->leader) {
        fill_silence(dst, b - a, pos, w);
        w->last_block = block;
        return;
    }
    const uint64_t total = v->blocks * w->block_bytes;
    if (near_start && w->last_block > len / 2) {
        g->base = (g->base + len) % v->blocks;
        g->restart_ns = now;
    }
    uint64_t base = g->base;
    if (!near_start && block > len / 2 && g->restart_ns && now - g->restart_ns < TAIL_NS)
        base = (base + v->blocks - len % v->blocks) % v->blocks;  // (the old pass's end, overlapping the new one's start)
    else
        w->last_block = block;
    uint64_t at = (base * w->block_bytes + pos) % total, left = b - a;
    while (left) {
        uint64_t k = total - at < left ? total - at : left;
        memcpy(dst, v->data + at, k);
        dst += k, left -= k, at = 0;
    }
}

void za_music_filter(uint64_t off, uint8_t *dst, uint64_t n) {
    if (off >= g_hi || off + n <= g_lo || !g_nw) return;
    int lo = 0, hi = g_nw - 1, first = g_nw;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_w[mid].off + g_w[mid].size > off) first = mid, hi = mid - 1;
        else lo = mid + 1;
    }
    if (first == g_nw || g_w[first].off >= off + n) return;  // (the pck's other streams: sound effects, voices)
    const uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    for (int i = first; i < g_nw && g_w[i].off < off + n; i++) {
        Wem *w = &g_w[i];
        Group *g = &g_g[w->group];
        g_last_group = w->group;
        const uint64_t a = off > w->data ? off : w->data, end = w->off + w->size, b = off + n < end ? off + n : end;
        if (a < b && (g->song || g->silent)) overlay_wem(w, g, i, dst + (a - off), a, b, now);
        g->last_ns = w->last_ns = now;
    }
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ setup
typedef struct { uint32_t track, wem; } TrackSrc;
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_src(const void *a, const void *b) { return cmp_u32(&((const TrackSrc *)a)->track, &((const TrackSrc *)b)->track); }
static int cmp_wem(const void *a, const void *b) {
    uint64_t x = ((const Wem *)a)->off, y = ((const Wem *)b)->off;
    return x < y ? -1 : x > y;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int uf_find(int *p, int i) {
    while (p[i] != i) i = p[i] = p[p[i]];
    return i;
}
static int id_index(const uint32_t *ids, int n, uint32_t id) {
    const uint32_t *f = bsearch(&id, ids, (size_t)n, 4, cmp_u32);
    return f ? (int)(f - ids) : -1;
}

void za_music_setup(ZaMusicRead read, uint64_t bnk_off, uint64_t bnk_size, const uint64_t pck_off[2], const uint64_t pck_size[2]) {
    pthread_mutex_lock(&g_lock);
    g_nw = 0, g_ng = 0, g_lo = g_hi = 0, g_last_group = -1;  // (a game restart: the old tables stay allocated, a few KB)
    Wem *wems = NULL;
    Group *groups = NULL;
    uint8_t *bnk = NULL;
    TrackSrc *src = NULL;
    uint32_t *ids = NULL;
    int *uf = NULL, nsrc = 0, nids = 0, nw = 0, ng = 0;
    if (!bnk_size || bnk_size > (64u << 20) || !(bnk = malloc(bnk_size)) || !read(bnk, bnk_size, bnk_off)) goto out;

    // BGM.bnk's hierarchy: music tracks (11) name their streams, music segments (10) their tracks
    const uint8_t *hirc = NULL;
    uint64_t hirc_n = 0;
    for (uint64_t o = 0; o + 8 <= bnk_size;) {
        uint64_t n = rd32(bnk + o + 4);
        if (o + 8 + n > bnk_size) break;
        if (!memcmp(bnk + o, "HIRC", 4)) hirc = bnk + o + 8, hirc_n = n;
        o += 8 + n;
    }
    if (!hirc || hirc_n < 4) goto out;
    int cap = 4096;
    src = malloc(sizeof(TrackSrc) * (size_t)cap);
    if (!src) goto out;
    uint32_t count = rd32(hirc);
    uint64_t p = 4;
    for (uint32_t i = 0; i < count && p + 9 <= hirc_n; i++) {
        uint8_t type = hirc[p];
        uint64_t size = rd32(hirc + p + 1);
        if (size < 4 || p + 5 + size > hirc_n) break;
        if (type == 11 && size >= 9) {
            uint32_t id = rd32(hirc + p + 5), ns = rd32(hirc + p + 10);
            const uint8_t *q = hirc + p + 14, *e = hirc + p + 5 + size;
            for (uint32_t s = 0; s < ns && q + 14 <= e && nsrc < cap; s++, q += 14)
                src[nsrc++] = (TrackSrc){id, rd32(q + 5)};
        }
        p += 5 + size;
    }
    if (!nsrc) goto out;
    qsort(src, (size_t)nsrc, sizeof *src, cmp_src);
    ids = malloc(4 * (size_t)nsrc);
    if (!ids) goto out;
    for (int i = 0; i < nsrc; i++) ids[i] = src[i].wem;
    qsort(ids, (size_t)nsrc, 4, cmp_u32);
    for (int i = 0; i < nsrc; i++) if (!nids || ids[nids - 1] != ids[i]) ids[nids++] = ids[i];
    uf = malloc(sizeof(int) * (size_t)nids);
    if (!uf) goto out;
    for (int i = 0; i < nids; i++) uf[i] = i;
    // A segment's children: looked for as any 32-bit word of its body naming a track (its layout before the list
    // varies with its parameters; ids are 32-bit hashes, so a chance match isn't a worry)
    p = 4;
    for (uint32_t i = 0; i < count && p + 9 <= hirc_n; i++) {
        uint8_t type = hirc[p];
        uint64_t size = rd32(hirc + p + 1);
        if (size < 4 || p + 5 + size > hirc_n) break;
        if (type == 10) {
            int root = -1;
            for (uint64_t q = p + 9; q + 4 <= p + 5 + size; q++) {
                TrackSrc key = {rd32(hirc + q), 0};
                const TrackSrc *f = bsearch(&key, src, (size_t)nsrc, sizeof *src, cmp_src);
                if (!f) continue;
                while (f > src && f[-1].track == key.track) f--;
                for (; f < src + nsrc && f->track == key.track; f++) {
                    int w = uf_find(uf, id_index(ids, nids, f->wem));
                    if (root < 0) root = w;
                    else uf[w] = root;
                }
            }
        }
        p += 5 + size;
    }

    // The packages' tables: where each stream of those is, its format from its own header
    for (int k = 0; k < 2; k++) {
        uint8_t h[28];
        if (pck_size[k] < 28 || !read(h, 28, pck_off[k]) || memcmp(h, "AKPK", 4)) continue;
        uint64_t lang = rd32(h + 12), bank = rd32(h + 16), stm = rd32(h + 20), at = pck_off[k] + 28 + lang + bank;
        if (stm < 4 || stm > (16u << 20) || 28 + lang + bank + stm > pck_size[k]) continue;
        uint8_t *t = malloc(stm);
        if (!t || !read(t, stm, at)) { free(t); continue; }
        uint32_t n = rd32(t);
        for (uint32_t i = 0; i < n && 4 + (uint64_t)(i + 1) * 20 <= stm; i++) {
            const uint8_t *e = t + 4 + (uint64_t)i * 20;
            uint32_t id = rd32(e), bs = rd32(e + 4), size = rd32(e + 8);
            uint64_t off = (uint64_t)rd32(e + 12) * (bs ? bs : 1);
            int ix = id_index(ids, nids, id);
            if (ix < 0 || off + size > pck_size[k] || size < 0x40) continue;
            uint8_t w[0x1000];  // (the header: 96 bytes on most, 296 on the ones with loop chunks: smpl, cue, LIST)
            uint64_t hn = size < sizeof w ? size : sizeof w;
            if (!read(w, hn, pck_off[k] + off) || memcmp(w, "RIFF", 4)) continue;
            uint16_t tag = 0, ch = 0, ba = 0;
            uint32_t rate = 0;
            uint64_t data = 0;
            for (uint64_t o = 12; o + 8 <= hn;) {
                uint64_t cn = rd32(w + o + 4);
                if (!memcmp(w + o, "fmt ", 4) && o + 8 + 14 <= hn)
                    tag = rd16(w + o + 8), ch = rd16(w + o + 10), rate = rd32(w + o + 12), ba = rd16(w + o + 20);
                if (!memcmp(w + o, "data", 4)) { data = o + 8; break; }
                o += 8 + cn + (cn & 1);
            }
            // (PTADPCM of the block size the game uses: the only kind seen. Anything else stays the game's own.)
            if (tag != 0x8311 || !data || !ch || ch > 2 || ba != PT_FRAME * ch || !rate) continue;
            Wem *g = realloc(wems, sizeof(Wem) * (size_t)(nw + 1));
            if (!g) continue;
            wems = g;
            wems[nw++] = (Wem){pck_off[k] + off, size, pck_off[k] + off + data, id, rate, ba, ch, ix, 0, 0};
        }
        free(t);
    }
    if (!nw) goto out;
    qsort(wems, (size_t)nw, sizeof *wems, cmp_wem);

    // Groups: one per set of streams in a segment together; its id the lowest stream id (ids is sorted: the first)
    groups = calloc((size_t)nids, sizeof *groups);
    int *group_of = malloc(sizeof(int) * (size_t)nids);
    if (!groups || !group_of) { free(group_of); goto out; }
    for (int i = 0; i < nids; i++) group_of[i] = -1;
    for (int i = 0; i < nids; i++) {
        int r = uf_find(uf, i);
        if (group_of[r] < 0) {
            group_of[r] = ng;
            groups[ng].id = ids[i];
            groups[ng].leader = -1;
            ng++;
        }
    }
    for (int i = 0; i < nw; i++) {
        Wem *w = &wems[i];
        Group *g = &groups[group_of[uf_find(uf, w->group)]];
        w->group = (int)(g - groups);
        g->stems++;
        uint64_t len = (w->off + w->size - w->data) / w->block_bytes;
        if (len > g->blocks) g->blocks = len;
        g->block_bytes = w->block_bytes;
        if (!g_lo || w->off < g_lo) g_lo = w->off;
        if (w->off + w->size > g_hi) g_hi = w->off + w->size;
    }
    free(group_of);
    g_w = wems, g_nw = nw, g_g = groups, g_ng = ng;
    wems = NULL, groups = NULL;
    LOG("[music] %d music streams in %d tracks\n", g_nw, g_ng);

    FILE *f = fopen(ASSIGN_FILE, "r");
    if (f) {
        char line[400];
        int loaded = 0;
        while (fgets(line, sizeof line, f)) {
            char *end;
            if (line[0] == '#') continue;
            unsigned long id = strtoul(line, &end, 10);
            if (end == line || *end != ' ') continue;
            end++;
            end[strcspn(end, "\r\n")] = 0;
            int g = group_by_id((uint32_t)id);
            if (g >= 0 && *end) set_group(g, end), loaded++;
        }
        fclose(f);
        LOG("[music] %d tracks assigned\n", loaded);
    }
out:
    if (wems) free(wems);
    if (groups) free(groups);
    free(uf), free(ids), free(src), free(bnk);
    pthread_mutex_unlock(&g_lock);
}
