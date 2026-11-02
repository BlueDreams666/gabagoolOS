/* GabagoolOS library jobs over Wi-Fi: lyrics for every song, covers for
 * albums without. See sloppy_libjob.h. Plain POSIX: the card is the C
 * library's, under sl_net_card_root(). GPLv2+.
 *
 * Lyrics: LRCLIB's /api/get by artist, title, album and length, at most a
 * request a second; the synced words when it has them, else the plain
 * ones, saved as "<song>.lrc" beside the song (as Now Playing's own
 * search does). Covers: MusicBrainz's search for the album's release
 * group (a request a second, with the User-Agent it asks for), then the
 * Cover Art Archive's front image of it, saved as cover.jpg in the
 * album's folder, where the cover thread finds it. Songs or albums the web
 * had nothing for are kept in lyrics.miss and covers.miss and not asked
 * about again. */
#define _GNU_SOURCE 1
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include "sloppy_net.h"
#include "sloppy_libjob.h"

#define LJ_INBOX 16
#define LJ_MISS_CAP 65536                /* misses remembered (four bytes each) */
#define LJ_FAILS_WAIT 3                  /* in a row: wait a while */
#define LJ_FAILS_STOP 12                 /* in a row: give up for now */

unsigned sl_libjob_lrclib_ms = SL_LIBJOB_LRCLIB_MS, sl_libjob_mb_ms = SL_LIBJOB_MB_MS;
static uint32_t lj_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static pthread_mutex_t lj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lj_cond = PTHREAD_COND_INITIALIZER;
static struct {
    struct sl_libjob_item inbox[LJ_INBOX];
    int n_inbox;
    bool run, fed_all, online, hold, up;
    struct sl_libjob_status st;
    pthread_t thread;
} lj;
static uint32_t *lj_miss;                /* the thread's: sorted */
static int lj_n_miss;
static uint32_t lj_last_req;

static uint32_t lj_hash(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *p = a; p && *p; p++) h = (h ^ (unsigned char)tolower((unsigned char)*p)) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char *p = b; p && *p; p++) h = (h ^ (unsigned char)tolower((unsigned char)*p)) * 16777619u;
    return h;
}
static bool lj_real(char *out, size_t n, const char *rb)
{
    const char *root = sl_net_card_root();
    return root && snprintf(out, n, "%s%s", root, rb) < (int)n;
}

/* ------------------------------------------------------------ helpers */
bool sl_libjob_lrc_path(const char *song, char *out, size_t n, const char *ext)
{
    const char *slash = strrchr(song, '/'), *dot = strrchr(song, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - song) : strlen(song);
    return snprintf(out, n, "%.*s%s", (int)stem, song, ext) < (int)n;
}
/* the first "key": "..." with its escapes undone; false for null or none */
bool sl_libjob_json_str(const char *json, const char *key, char *out, size_t cap)
{
    char k[48];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(json, k);
    if (cap) out[0] = '\0';
    if (!p) return false;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    size_t o = 0;
    for (p++; *p && *p != '"'; p++)
    {
        char c = *p;
        if (c == '\\' && p[1])
        {
            c = *++p;
            if (c == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]))
            {
                char h[5] = { p[1], p[2], p[3], p[4], 0 };
                unsigned v = (unsigned)strtoul(h, NULL, 16);
                p += 4;
                if (v >= 0xd800 && v < 0xdc00 && p[1] == '\\' && p[2] == 'u')
                {
                    char l[5] = { p[3], p[4], p[5], p[6], 0 };
                    unsigned lo = (unsigned)strtoul(l, NULL, 16);
                    if (lo >= 0xdc00 && lo < 0xe000) { v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                }
                char u[4];
                int m = v < 0x80 ? (u[0] = (char)v, 1) : v < 0x800 ? (u[0] = (char)(0xc0 | v >> 6), u[1] = (char)(0x80 | (v & 63)), 2) :
                        v < 0x10000 ? (u[0] = (char)(0xe0 | v >> 12), u[1] = (char)(0x80 | ((v >> 6) & 63)), u[2] = (char)(0x80 | (v & 63)), 3) :
                        (u[0] = (char)(0xf0 | v >> 18), u[1] = (char)(0x80 | ((v >> 12) & 63)), u[2] = (char)(0x80 | ((v >> 6) & 63)), u[3] = (char)(0x80 | (v & 63)), 4);
                for (int i = 0; i < m; i++) if (o + 1 < cap) out[o++] = u[i];
                continue;
            }
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
            if (c == '\r') continue;
        }
        if (o + 1 < cap) out[o++] = c;
    }
    if (cap) out[o] = '\0';
    return *p == '"';
}
/* a URL's query part: letters and digits as they are, the rest %XX */
static void lj_enc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; s && *s && n + 4 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------ the misses */
static const char *lj_miss_file(int kind) { return kind == SL_LIBJOB_LYRICS ? SL_CAST_DIR "/lyrics.miss" : SL_CAST_DIR "/covers.miss"; }
static int lj_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void lj_miss_load(int kind)
{
    char p[300];
    free(lj_miss);
    lj_miss = NULL;
    lj_n_miss = 0;
    if (!lj_real(p, sizeof p, lj_miss_file(kind))) return;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    int n = size > 0 ? (int)(size / 4) : 0;
    if (n > LJ_MISS_CAP) n = LJ_MISS_CAP;
    lj_miss = n ? malloc((size_t)n * 4) : NULL;
    if (lj_miss && read(fd, lj_miss, (size_t)n * 4) == (ssize_t)n * 4)
    {
        lj_n_miss = n;
        qsort(lj_miss, (size_t)n, 4, lj_cmp);
    }
    close(fd);
}
static bool lj_miss_has(uint32_t h)
{
    return lj_miss && bsearch(&h, lj_miss, (size_t)lj_n_miss, 4, lj_cmp) != NULL;
}
static void lj_miss_add(int kind, uint32_t h)
{
    char p[300];
    if (lj_n_miss >= LJ_MISS_CAP || !lj_real(p, sizeof p, lj_miss_file(kind))) return;
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { if (write(fd, &h, 4) != 4) {} close(fd); }
    uint32_t *m = realloc(lj_miss, (size_t)(lj_n_miss + 1) * 4);
    if (!m) return;
    lj_miss = m;
    int i = lj_n_miss++;
    while (i > 0 && lj_miss[i - 1] > h) { lj_miss[i] = lj_miss[i - 1]; i--; }
    lj_miss[i] = h;
}

/* ------------------------------------------------------------ the requests */
/* waits until a request may go (the service's pace); false when stopped */
static bool lj_pace(uint32_t gap)
{
    for (;;)
    {
        pthread_mutex_lock(&lj_lock);
        bool run = lj.run;
        pthread_mutex_unlock(&lj_lock);
        if (!run) return false;
        int32_t left = (int32_t)(lj_last_req + gap - lj_ms());
        if (left <= 0) { lj_last_req = lj_ms(); return true; }
        struct timespec ts = { 0, (left > 100 ? 100 : left) * 1000000L };
        nanosleep(&ts, NULL);
    }
}
/* a file written whole, or not at all; never over one that is there */
static bool lj_write_new(const char *path, const char *data, size_t len)
{
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok && !access(path, F_OK)) ok = false;
    if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    return true;
}
static char lj_buf[128 * 1024];          /* an answer: the thread's */
enum { LJ_FOUND, LJ_HAD, LJ_MISSING, LJ_FAILED, LJ_STOPPED };

static int lj_lyrics(const struct sl_libjob_item *it)
{
    char rb[320], real[620];
    static const char *const exts[] = { ".lrc", ".txt", ".LRC" };
    for (int i = 0; i < 3; i++)
        if (sl_libjob_lrc_path(it->path, rb, sizeof rb, exts[i]) && lj_real(real, sizeof real, rb) && !access(real, F_OK)) return LJ_HAD;
    uint32_t h = lj_hash(it->path, "");
    if (lj_miss_has(h)) return LJ_MISSING;
    if (!it->artist[0] || !it->title[0]) return LJ_MISSING;
    char a[400], t[400], al[400], url[1400];
    lj_enc(a, sizeof a, it->artist);
    lj_enc(t, sizeof t, it->title);
    lj_enc(al, sizeof al, it->album);
    int n = snprintf(url, sizeof url, "https://lrclib.net/api/get?artist_name=%s&track_name=%s", a, t);
    if (it->album[0]) n += snprintf(url + n, sizeof url - (size_t)n, "&album_name=%s", al);
    if (it->secs) snprintf(url + n, sizeof url - (size_t)n, "&duration=%u", it->secs);
    if (!lj_pace(sl_libjob_lrclib_ms)) return LJ_STOPPED;
    int status = sl_net_http_get(url, SL_LIBJOB_UA, lj_buf, sizeof lj_buf, NULL, 30000);
    if (status == 404) { lj_miss_add(SL_LIBJOB_LYRICS, h); return LJ_MISSING; }
    if (status != 200) return LJ_FAILED;
    static char words[96 * 1024];
    if (!sl_libjob_json_str(lj_buf, "syncedLyrics", words, sizeof words) || !words[0])
        sl_libjob_json_str(lj_buf, "plainLyrics", words, sizeof words);
    if (!words[0]) { lj_miss_add(SL_LIBJOB_LYRICS, h); return LJ_MISSING; }   /* an instrumental, or empty */
    pthread_mutex_lock(&lj_lock);
    bool hold = lj.hold;
    pthread_mutex_unlock(&lj_lock);
    if (hold) return LJ_FAILED;          /* USB mode came: tried again later */
    if (!sl_libjob_lrc_path(it->path, rb, sizeof rb, ".lrc") || !lj_real(real, sizeof real, rb)) return LJ_FAILED;
    return lj_write_new(real, words, strlen(words)) ? LJ_FOUND : LJ_FAILED;
}

/* an image the cover thread would take, in the album's folder */
static bool lj_folder_has_cover(const char *dir)
{
    static const char *const names[] = { "cover", "folder", "front", "albumart", "album", NULL };
    DIR *d = opendir(dir);
    if (!d) return false;
    bool has = false;
    struct dirent *e;
    while (!has && (e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png"))) continue;
        for (int i = 0; names[i] && !has; i++)
            has = !strncasecmp(e->d_name, names[i], strlen(names[i])) && e->d_name + strlen(names[i]) == dot;
    }
    closedir(d);
    return has;
}
/* the release group's id when MusicBrainz is sure of it (score 90 or more) */
static bool lj_mb_group(const char *json, char *id, size_t n)
{
    const char *g = strstr(json, "\"release-groups\"");
    if (!g) return false;
    const char *i = strstr(g, "\"id\"");
    if (!i || !sl_libjob_json_str(i, "id", id, n) || strlen(id) != 36) return false;
    const char *s = strstr(g, "\"score\"");
    if (!s) return false;
    s += 7;
    while (*s == ' ' || *s == ':' || *s == '"') s++;
    return atoi(s) >= 90;
}
static void lj_lucene(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s && o + 3 < cap; s++)
    {
        if (strchr("+-&|!(){}[]^\"~*?:\\/", *s)) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = '\0';
}
static int lj_cover(const struct sl_libjob_item *it)
{
    char real[620], dir[620];
    if (it->had) return LJ_HAD;
    if (!lj_real(real, sizeof real, it->path)) return LJ_FAILED;
    snprintf(dir, sizeof dir, "%s", real);
    char *slash = strrchr(dir, '/');
    if (!slash) return LJ_FAILED;
    *slash = '\0';
    if (lj_folder_has_cover(dir)) return LJ_HAD;
    uint32_t h = lj_hash(it->artist, it->album);
    if (lj_miss_has(h)) return LJ_MISSING;
    if (!it->album[0] || !it->artist[0]) return LJ_MISSING;
    char qa[300], qb[300], q[700], enc[2100], url[2300], id[48];
    lj_lucene(qa, sizeof qa, it->album);
    lj_lucene(qb, sizeof qb, it->artist);
    snprintf(q, sizeof q, "releasegroup:\"%s\" AND artist:\"%s\"", qa, qb);
    lj_enc(enc, sizeof enc, q);
    snprintf(url, sizeof url, "https://musicbrainz.org/ws/2/release-group/?query=%s&fmt=json&limit=3", enc);
    if (!lj_pace(sl_libjob_mb_ms)) return LJ_STOPPED;
    int status = sl_net_http_get(url, SL_LIBJOB_UA, lj_buf, sizeof lj_buf, NULL, 30000);
    if (status != 200) return LJ_FAILED;
    if (!lj_mb_group(lj_buf, id, sizeof id)) { lj_miss_add(SL_LIBJOB_COVERS, h); return LJ_MISSING; }
    char path[640], tmp[660];
    snprintf(path, sizeof path, "%s/cover.jpg", dir);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    snprintf(url, sizeof url, "https://coverartarchive.org/release-group/%s/front-500", id);
    if (!lj_pace(sl_libjob_mb_ms)) return LJ_STOPPED;
    pthread_mutex_lock(&lj_lock);
    bool hold = lj.hold;
    pthread_mutex_unlock(&lj_lock);
    if (hold) return LJ_FAILED;
    status = sl_net_http_get(url, SL_LIBJOB_UA, NULL, 0, tmp, 60000);
    if (status == 404) { unlink(tmp); lj_miss_add(SL_LIBJOB_COVERS, h); return LJ_MISSING; }
    /* a JPEG, whole and not huge */
    unsigned char head[3] = { 0 };
    struct stat st;
    int fd = open(tmp, O_RDONLY);
    bool jpeg = fd >= 0 && read(fd, head, 3) == 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
    if (fd >= 0) close(fd);
    if (status != 200 || !jpeg || stat(tmp, &st) || st.st_size < 1024 || st.st_size > (4 << 20)) { unlink(tmp); return LJ_FAILED; }
    if (!access(path, F_OK) || rename(tmp, path)) { unlink(tmp); return LJ_HAD; }   /* one came meanwhile: kept */
    return LJ_FOUND;
}

/* ------------------------------------------------------------ the thread */
static void lj_set(int state, const char *why)
{
    pthread_mutex_lock(&lj_lock);
    lj.st.state = state;
    snprintf(lj.st.why, sizeof lj.st.why, "%s", why ? why : "");
    pthread_mutex_unlock(&lj_lock);
}
static const char *lj_pos_file(int kind) { return kind == SL_LIBJOB_LYRICS ? SL_CAST_DIR "/lyrics.pos" : SL_CAST_DIR "/covers.pos"; }
int sl_libjob_saved(int kind)
{
    char p[300], b[16] = "";
    int fd = lj_real(p, sizeof p, lj_pos_file(kind)) ? open(p, O_RDONLY) : -1;
    if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = '\0';
    return atoi(b) > 0 ? atoi(b) : 0;
}
static void lj_pos_save(int kind, int pos)
{
    char p[300], b[32];
    if (!lj_real(p, sizeof p, lj_pos_file(kind))) return;
    int n = snprintf(b, sizeof b, "%d\n", pos);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { if (write(fd, b, (size_t)n) != n) {} close(fd); }
}
static void *lj_loop(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lj_lock);
    int kind = lj.st.kind;
    pthread_mutex_unlock(&lj_lock);
    lj_miss_load(kind);
    int fails = 0;
    for (;;)
    {
        pthread_mutex_lock(&lj_lock);
        while (lj.run && (!lj.n_inbox || lj.hold || !lj.online) && !(lj.fed_all && !lj.n_inbox))
        {
            lj.st.state = SL_LIBJOB_WAITING;
            snprintf(lj.st.why, sizeof lj.st.why, "%s", lj.hold ? "USB mode" : !lj.online ? "Waiting for Wi-Fi" : "");
            if (!lj.hold && lj.online) lj.st.state = SL_LIBJOB_RUNNING;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 300 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&lj_cond, &lj_lock, &ts);
        }
        if (!lj.run || (lj.fed_all && !lj.n_inbox))
        {
            bool done = lj.run;
            lj.st.state = done ? SL_LIBJOB_DONE : lj.st.state == SL_LIBJOB_FAILED ? SL_LIBJOB_FAILED : SL_LIBJOB_STOPPED;
            lj.st.current[0] = '\0';
            int pos = lj.st.fed;
            lj.st.seq++;
            pthread_mutex_unlock(&lj_lock);
            lj_pos_save(kind, done ? 0 : pos);   /* the next one starts where this left off */
            break;
        }
        struct sl_libjob_item it = lj.inbox[0];
        memmove(lj.inbox, lj.inbox + 1, sizeof lj.inbox[0] * (size_t)--lj.n_inbox);
        lj.st.state = SL_LIBJOB_RUNNING;
        lj.st.why[0] = '\0';
        snprintf(lj.st.current, sizeof lj.st.current, "%s", kind == SL_LIBJOB_LYRICS ? it.title : it.album);
        pthread_mutex_unlock(&lj_lock);
        int r = kind == SL_LIBJOB_LYRICS ? lj_lyrics(&it) : lj_cover(&it);
        pthread_mutex_lock(&lj_lock);
        if (r == LJ_STOPPED) { pthread_mutex_unlock(&lj_lock); continue; }
        lj.st.fed++;
        if (r == LJ_FOUND) { lj.st.found++; if (kind == SL_LIBJOB_COVERS) { lj.st.last_found = it.index; lj.st.found_seq++; } }
        else if (r == LJ_HAD) lj.st.had++;
        else if (r == LJ_MISSING) lj.st.missing++;
        else lj.st.failed++;
        lj.st.seq++;
        int pos = lj.st.fed;
        pthread_mutex_unlock(&lj_lock);
        if (pos % 25 == 0) lj_pos_save(kind, pos);
        fails = r == LJ_FAILED ? fails + 1 : 0;
        if (fails >= LJ_FAILS_STOP)
        {
            /* the service is down, or the network is: stop, and say where */
            lj_set(SL_LIBJOB_FAILED, kind == SL_LIBJOB_LYRICS ? "LRCLIB is not answering" : "MusicBrainz is not answering");
            pthread_mutex_lock(&lj_lock);
            lj.run = false;
            pthread_mutex_unlock(&lj_lock);
        }
        else if (fails && fails % LJ_FAILS_WAIT == 0)
        {
            lj_set(SL_LIBJOB_WAITING, "Not answering, trying again soon");
            for (int i = 0; i < 300; i++)   /* 30 s, stopped at once by Stop */
            {
                pthread_mutex_lock(&lj_lock);
                bool run = lj.run;
                pthread_mutex_unlock(&lj_lock);
                if (!run) break;
                struct timespec ts = { 0, 100000000L };
                nanosleep(&ts, NULL);
            }
        }
    }
    free(lj_miss);
    lj_miss = NULL;
    lj_n_miss = 0;
    pthread_mutex_lock(&lj_lock);
    lj.up = false;
    pthread_mutex_unlock(&lj_lock);
    return NULL;
}

bool sl_libjob_start(int kind, int total, int fed)
{
    pthread_mutex_lock(&lj_lock);
    if (lj.up) { pthread_mutex_unlock(&lj_lock); return false; }
    if (lj.thread) pthread_join(lj.thread, NULL);   /* the last one has ended: its thread is let go */
    lj.thread = 0;
    bool online = lj.online, hold = lj.hold;
    memset(&lj.st, 0, sizeof lj.st);
    lj.st.kind = kind;
    lj.st.total = total;
    lj.st.fed = fed;
    lj.st.last_found = -1;
    lj.st.state = SL_LIBJOB_RUNNING;
    lj.n_inbox = 0;
    lj.fed_all = false;
    lj.run = true;
    lj.online = online;
    lj.hold = hold;
    lj.up = true;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    bool ok = pthread_create(&lj.thread, &a, lj_loop, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!ok) { lj.up = lj.run = false; lj.thread = 0; lj.st.state = SL_LIBJOB_FAILED; }
    pthread_mutex_unlock(&lj_lock);
    return ok;
}
void sl_libjob_stop(void)
{
    pthread_mutex_lock(&lj_lock);
    lj.run = false;
    pthread_cond_broadcast(&lj_cond);
    pthread_mutex_unlock(&lj_lock);
}
bool sl_libjob_want(void)
{
    pthread_mutex_lock(&lj_lock);
    bool w = lj.up && lj.run && !lj.fed_all && lj.n_inbox < LJ_INBOX;
    pthread_mutex_unlock(&lj_lock);
    return w;
}
void sl_libjob_give(const struct sl_libjob_item *it)
{
    pthread_mutex_lock(&lj_lock);
    if (lj.n_inbox < LJ_INBOX) lj.inbox[lj.n_inbox++] = *it;
    pthread_cond_broadcast(&lj_cond);
    pthread_mutex_unlock(&lj_lock);
}
void sl_libjob_fed_all(void)
{
    pthread_mutex_lock(&lj_lock);
    lj.fed_all = true;
    pthread_cond_broadcast(&lj_cond);
    pthread_mutex_unlock(&lj_lock);
}
void sl_libjob_online(bool up)
{
    pthread_mutex_lock(&lj_lock);
    lj.online = up;
    pthread_cond_broadcast(&lj_cond);
    pthread_mutex_unlock(&lj_lock);
}
void sl_libjob_hold(bool hold)
{
    pthread_mutex_lock(&lj_lock);
    lj.hold = hold;
    pthread_cond_broadcast(&lj_cond);
    pthread_mutex_unlock(&lj_lock);
}
void sl_libjob_status(struct sl_libjob_status *s)
{
    pthread_mutex_lock(&lj_lock);
    *s = lj.st;
    pthread_mutex_unlock(&lj_lock);
}
bool sl_libjob_busy(void)
{
    pthread_mutex_lock(&lj_lock);
    bool up = lj.up;
    pthread_mutex_unlock(&lj_lock);
    return up;
}

/* Podcasts: their worker, files and feed parser, on this unit's helpers */
#include "sloppy_podjob.inc"

/* A music server (Subsonic, OpenSubsonic, Navidrome): its worker, the cache, the answers' reader */
#include "sloppy_subjob.inc"

/* Audiobookshelf: its worker, the books it brings, the place told both ways (its answers through the reader above) */
#include "sloppy_absjob.inc"

/* ---------------------------------------------------------------- audiobook covers */
/* A book's own picture (a file in its folder, or the one in its audio:
 * ID3 unsynchronised or a Vorbis comment's base64 picture block undone)
 * made into the Audiobooks pages' squares on a thread of its own, one
 * book at a time, under the covers' rule for memory. What could not be
 * read leaves a marker, so it is not read again every visit. Written as
 * the podcasts' pictures are (pj_art_square), nothing in USB mode. */
#define BA_MAX (8 * 1024 * 1024)            /* bigger is not a cover */
static struct {
    int state;                              /* 0 idle, 1 working, 2 done */
    bool ok, short_of_memory;
    uint32_t key, off, size;
    int flags;
    char src[400];
    pthread_t thread;
} ba;
/* MemAvailable, -1 off Linux */
static long ba_mem_available_kb(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[96];
    long kb = -1;
    while (fgets(line, sizeof line, fp))
        if (sscanf(line, "MemAvailable: %ld", &kb) == 1) break;
    fclose(fp);
    return kb;
}
void sl_book_art_name(char *out, size_t n, uint32_t key, int px)
{
    if (px) snprintf(out, n, SL_BOOK_ART_DIR "/%08x-%d.bin", (unsigned)key, px);
    else snprintf(out, n, SL_BOOK_ART_DIR "/%08x.none", (unsigned)key);
}
static int ba_b64(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' ? 62 : c == '/' ? 63 : -1;
}
static uint32_t ba_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* the picture's own bytes out of what was read: n is what is left */
static unsigned char *ba_unwrap(unsigned char *b, size_t *n)
{
    if (ba.flags & SL_BOOK_ART_UNSYNC)
    {   /* ID3: every FF 00 was FF */
        size_t o = 0;
        for (size_t i = 0; i < *n; i++) { b[o++] = b[i]; if (b[i] == 0xff && i + 1 < *n && !b[i + 1]) i++; }
        *n = o;
    }
    if (ba.flags & SL_BOOK_ART_BASE64)
    {   /* a FLAC picture block in base64: type, mime, words, four sizes, then the picture */
        size_t o = 0;
        unsigned acc = 0;
        int bits = 0;
        for (size_t i = 0; i < *n; i++)
        {
            int v = ba_b64(b[i]);
            if (v < 0) continue;
            acc = (acc << 6) | (unsigned)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; b[o++] = (unsigned char)(acc >> bits); }
        }
        size_t at = 4;
        if (o < at + 4) return NULL;
        at += 4 + ba_be32(b + at);                  /* past the mime type */
        if (o < at + 4) return NULL;
        at += 4 + ba_be32(b + at);                  /* past the words */
        if (o < at + 20) return NULL;
        uint32_t len = ba_be32(b + at + 16);
        at += 20;
        if (len > o - at) return NULL;
        *n = len;
        return b + at;
    }
    return b;
}
static bool ba_make(void)
{
    char real[512];
    struct stat st;
    if (!lj_real(real, sizeof real, ba.src) || stat(real, &st) || st.st_size <= 0) return false;
    uint32_t off = ba.size ? ba.off : 0, size = ba.size ? ba.size : st.st_size > BA_MAX ? BA_MAX + 1 : (uint32_t)st.st_size;
    if (size > BA_MAX || (long long)off + size > (long long)st.st_size) return false;
    /* read whole before its picture is found: not when that leaves the R1
     * short (it froze out of memory, 2 Nov); tried again on a later visit */
    long avail = ba_mem_available_kb();
    if (avail >= 0 && (long)(size / 1024) + 6144 > avail) { ba.short_of_memory = true; return false; }
    unsigned char *in = malloc(size);
    int fd = in ? open(real, O_RDONLY) : -1;
    size_t got = 0;
    if (fd >= 0 && lseek(fd, (off_t)off, SEEK_SET) == (off_t)off)
        while (got < size)
        {
            ssize_t r = read(fd, in + got, size - got < 65536 ? size - got : 65536);
            if (r <= 0) break;
            got += (size_t)r;
        }
    if (fd >= 0) close(fd);
    unsigned char *pic = got == size ? ba_unwrap(in, &got) : NULL;
    int w = 0, h = 0;
    unsigned char *rgb = pic && got ? sl_art_rgb_load(pic, (int)got, &w, &h) : NULL;
    free(in);
    if (!rgb) return false;
    char name[96];
    pthread_mutex_lock(&pj_lock);
    bool ok = !pj.hold;
    sl_book_art_name(name, sizeof name, ba.key, SL_BOOK_ART_ROW);
    ok = ok && pj_art_square(name, SL_BOOK_ART_DIR, rgb, w, h, SL_BOOK_ART_ROW, NULL);
    sl_book_art_name(name, sizeof name, ba.key, SL_BOOK_ART_BIG);
    ok = ok && pj_art_square(name, SL_BOOK_ART_DIR, rgb, w, h, SL_BOOK_ART_BIG, NULL);
    pthread_mutex_unlock(&pj_lock);
    sl_art_rgb_free(rgb);
    return ok;
}
static void *ba_worker(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);   /* after audio and the screen */
#endif
    ba.ok = ba_make();
    if (!ba.ok && !ba.short_of_memory)
    {   /* not a picture we can read: said once, on the card */
        char name[96];
        sl_book_art_name(name, sizeof name, ba.key, 0);
        pthread_mutex_lock(&pj_lock);
        if (!pj.hold) { pj_mkdirs(SL_BOOK_ART_DIR); pj_write(name, "", 0); }
        pthread_mutex_unlock(&pj_lock);
    }
    __atomic_store_n(&ba.state, 2, __ATOMIC_RELEASE);
    return NULL;
}
bool sl_book_art_start(uint32_t key, const char *src, uint32_t off, uint32_t size, int flags)
{
    if (__atomic_load_n(&ba.state, __ATOMIC_ACQUIRE) || !src || !src[0]) return false;
    ba.key = key; ba.off = off; ba.size = size; ba.flags = flags; ba.ok = ba.short_of_memory = false;
    snprintf(ba.src, sizeof ba.src, "%s", src);
    __atomic_store_n(&ba.state, 1, __ATOMIC_RELEASE);
    if (pthread_create(&ba.thread, NULL, ba_worker, NULL)) { ba.state = 0; return false; }
    return true;
}
int sl_book_art_poll(uint32_t *key)
{
    int s = __atomic_load_n(&ba.state, __ATOMIC_ACQUIRE);
    if (s != 2) return s == 1 ? 0 : -2;
    pthread_join(ba.thread, NULL);
    ba.state = 0;
    if (key) *key = ba.key;
    return ba.ok ? 1 : -1;
}
int sl_book_art_state(uint32_t key)
{
    char name[96], real[512];
    sl_book_art_name(name, sizeof name, key, SL_BOOK_ART_BIG);
    if (lj_real(real, sizeof real, name) && !access(real, R_OK))
    {
        sl_book_art_name(name, sizeof name, key, SL_BOOK_ART_ROW);
        if (lj_real(real, sizeof real, name) && !access(real, R_OK)) return 1;
    }
    sl_book_art_name(name, sizeof name, key, 0);
    return lj_real(real, sizeof real, name) && !access(real, F_OK) ? -1 : 0;
}
/* the cover's own colour, from its small square: 0 none */
uint32_t sl_book_art_tint(uint32_t key)
{
    char name[96], real[512];
    sl_book_art_name(name, sizeof name, key, SL_BOOK_ART_ROW);
    FILE *f = lj_real(real, sizeof real, name) ? fopen(real, "rb") : NULL;
    if (!f) return 0;
    uint16_t px[SL_BOOK_ART_ROW * SL_BOOK_ART_ROW];
    unsigned char hd[12];
    size_t n = fread(hd, 1, sizeof hd, f) == sizeof hd ? fread(px, 2, SL_BOOK_ART_ROW * SL_BOOK_ART_ROW, f) : 0;
    fclose(f);
    if (!n) return 0;
    uint64_t r = 0, g = 0, b = 0;
    for (size_t i = 0; i < n; i++) { r += px[i] >> 11; g += (px[i] >> 5) & 63; b += px[i] & 31; }
    uint32_t t = (uint32_t)((r * 255 / 31 / n) << 16 | (g * 255 / 63 / n) << 8 | (b * 255 / 31 / n));
    return t ? t : 0x010101;
}
