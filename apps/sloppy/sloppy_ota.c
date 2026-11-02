/* GabagoolOS software updates from the card or the phone remote. See
 * sloppy_ota.h. Plain POSIX, no Rockbox headers but the firmware's
 * inflate (firmware/common/inflate.c, pure computation): the card is the C
 * library's, under sl_net_card_root(). GPLv2+.
 *
 * Under /.rockbox/update:
 *   incoming.gbupdate   what the phone remote sent (incoming.part on the way)
 *   stage/<path>        the new files, checked; stage/manifest.json, .sig
 *                       and stage/plan (written last: staged and whole)
 *   prev/<path>         the files the last swap replaced or removed
 *   manifest.json       the installed update's (its notes for What's new)
 *   journal             the swap, a line per step, fsynced:
 *       GBJ1 install|revert <from> <to> <source>
 *       F <i> <o|n|r|p|x> <size> <path>    o replaced, n new, r removed;
 *                                          p from prev, x deleted (revert)
 *       S swapping   D <i>...   S trial|done   S restart   S committed
 *       S undone (the undo script)   S rolledback
 * A line without its newline was cut by a power loss and is not read. */
#define _GNU_SOURCE 1
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "inflate.h"
#include "sloppy_net.h"
#include "sloppy_ota.h"
#include "sloppy_ota_key.h"

void sl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define OTA_MAN_MAX (512 * 1024)         /* a manifest of 420 files is about 70 KB */
#define OTA_FILES_MAX 2000
#define OTA_PATH_MAX 200
#define OTA_BUF (32 * 1024)
#define OTA_MARGIN (2ull << 20)          /* kept free on top of what is staged */
#define OTA_MAGIC "GBUPDATE\x01\0\0\0"
#define OTA_HEAD 20                      /* magic, manifest length, signature length */
#define OTA_PROG ".rockbox/rockbox.r1"   /* swapped last */
#define UPD(x) ".rockbox/update/" x      /* SL_OTA_DIR, from the card's top */

#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
unsigned char sl_ota_test_key[32];
bool sl_ota_test_key_set;
const char *sl_ota_crypto_lib = "libcrypto.so.1.1";
char sl_ota_running[16] = SL_VERSION;
long long sl_ota_free_fake = -1;
int sl_ota_cut_after;
int sl_ota_steps;
int sl_ota_slow_ms;
bool sl_ota_test_frozen;
#define OTA_SLOW() do { if (sl_ota_slow_ms) usleep((useconds_t)sl_ota_slow_ms * 1000); } while (0)
#define OTA_RUNNING sl_ota_running
#define OTA_CRYPTO_LIB sl_ota_crypto_lib
#else
#define OTA_RUNNING SL_VERSION
#define OTA_CRYPTO_LIB "libcrypto.so.1.1"
static int sl_ota_cut_after, sl_ota_steps;
#define OTA_SLOW() do {} while (0)
#endif
static const unsigned char ota_key_slot[32] = SL_OTA_KEY;

/* ------------------------------------------------------------ the state */
struct ota_file { char *path; uint64_t size; uint8_t sha[32]; char flag; };
struct ota_man {
    char version[16], min_from[16], build_date[16], product[24];
    uint64_t zip_size;
    uint8_t zip_sha[32];
    bool zip_sha_ok, boot_menu;
    struct ota_file *f;                  /* files, then removes (flag 'r') */
    int nf, nr;
    char notes[1200], notes_en[1200];
};
static pthread_mutex_t ota_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ota_cond = PTHREAD_COND_INITIALIZER;
static struct {
    bool up, hold, stop, want_scan, want_install;
    pthread_t thread;
    struct sl_ota_status st;
    char lang[4];
    char found[512];                     /* the file to install, a full path */
    bool booted;
    /* the upload */
    int up_fd;
    uint64_t up_got, up_len;
} ota = { .up_fd = -1, .lang = "en" };

static void ota_bump(void) { ota.st.seq++; }
static const char *ota_root(void) { const char *r = sl_net_card_root(); return r ? r : ""; }
static void ota_p(char *b, size_t n, const char *rel) { snprintf(b, n, "%s/%s", ota_root(), rel); }

/* the release key: the build's slot (or the tests') */
static const unsigned char *ota_key(void)
{
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (sl_ota_test_key_set) return sl_ota_test_key;
#endif
    if (!SL_OTA_KEY_SET) return NULL;
    for (int i = 0; i < 32; i++) if (ota_key_slot[i]) return ota_key_slot;
    return NULL;                         /* set but left zero: no key */
}

/* ------------------------------------------------------------ OpenSSL */
/* The system's libcrypto, loaded as the radio loads it (sloppy_net.c):
 * Ed25519 there needs OpenSSL 1.1.1 (EVP_PKEY_new_raw_public_key). */
#define OTA_ED25519 1087                 /* EVP_PKEY_ED25519, NID_ED25519 */
static struct {
    bool tried, ok;
    void *(*ctx_new)(void);
    void (*ctx_free)(void *);
    void *(*raw_pub)(int, void *, const unsigned char *, size_t);
    void (*pkey_free)(void *);
    int (*dv_init)(void *, void **, const void *, void *, void *);
    int (*dv)(void *, const unsigned char *, size_t, const unsigned char *, size_t);
    const void *(*sha256)(void);
    int (*d_init)(void *, const void *, void *);
    int (*d_update)(void *, const void *, size_t);
    int (*d_final)(void *, unsigned char *, unsigned int *);
} cr;
static pthread_mutex_t cr_lock = PTHREAD_MUTEX_INITIALIZER;
static bool cr_load(void)
{
    pthread_mutex_lock(&cr_lock);
    if (!cr.tried)
    {
        cr.tried = true;
        void *c = dlopen(OTA_CRYPTO_LIB, RTLD_NOW | RTLD_GLOBAL);
        if (c)
        {
            const char *(*ver)(int) = (const char *(*)(int))dlsym(c, "OpenSSL_version");
            *(void **)&cr.ctx_new = dlsym(c, "EVP_MD_CTX_new");
            *(void **)&cr.ctx_free = dlsym(c, "EVP_MD_CTX_free");
            *(void **)&cr.raw_pub = dlsym(c, "EVP_PKEY_new_raw_public_key");
            *(void **)&cr.pkey_free = dlsym(c, "EVP_PKEY_free");
            *(void **)&cr.dv_init = dlsym(c, "EVP_DigestVerifyInit");
            *(void **)&cr.dv = dlsym(c, "EVP_DigestVerify");
            *(void **)&cr.sha256 = dlsym(c, "EVP_sha256");
            *(void **)&cr.d_init = dlsym(c, "EVP_DigestInit_ex");
            *(void **)&cr.d_update = dlsym(c, "EVP_DigestUpdate");
            *(void **)&cr.d_final = dlsym(c, "EVP_DigestFinal_ex");
            cr.ok = cr.ctx_new && cr.ctx_free && cr.raw_pub && cr.pkey_free && cr.dv_init && cr.dv && cr.sha256 &&
                    cr.d_init && cr.d_update && cr.d_final;
            sl_log("update: %s, Ed25519 %s", ver ? ver(0) : "OpenSSL (no version)", cr.ok ? "there" : "missing (1.1.1 needed)");
        }
        else sl_log("update: no %s (%s): updates can't be checked", OTA_CRYPTO_LIB, dlerror());
    }
    bool ok = cr.ok;
    pthread_mutex_unlock(&cr_lock);
    return ok;
}
static bool cr_verify(const unsigned char *key, const unsigned char *sig, size_t sn, const void *msg, size_t mn)
{
    if (!cr_load() || !key || sn != 64) return false;
    void *pk = cr.raw_pub(OTA_ED25519, NULL, key, 32), *ctx = pk ? cr.ctx_new() : NULL;
    bool ok = ctx && cr.dv_init(ctx, NULL, NULL, NULL, pk) == 1 && cr.dv(ctx, sig, sn, msg, mn) == 1;
    if (ctx) cr.ctx_free(ctx);
    if (pk) cr.pkey_free(pk);
    return ok;
}
struct ota_sha { void *ctx; };
static bool sha_begin(struct ota_sha *h) { h->ctx = cr_load() ? cr.ctx_new() : NULL; return h->ctx && cr.d_init(h->ctx, cr.sha256(), NULL) == 1; }
static void sha_add(struct ota_sha *h, const void *b, size_t n) { if (h->ctx && n) cr.d_update(h->ctx, b, n); }
static bool sha_end(struct ota_sha *h, uint8_t out[32])
{
    unsigned n = 0;
    bool ok = h->ctx && cr.d_final(h->ctx, out, &n) == 1 && n == 32;
    if (h->ctx) cr.ctx_free(h->ctx);
    h->ctx = NULL;
    return ok;
}

/* ------------------------------------------------------------ small things */
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool unhex32(const char *s, uint8_t out[32])
{
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; i++)
    {
        int a = hexv(s[2 * i]), b = hexv(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}
/* "1.10" > "1.9": numbers between dots; anything else is no version */
static bool ver_ok(const char *v)
{
    if (!*v || *v == '.') return false;
    for (const char *p = v; *p; p++) if (!isdigit((unsigned char)*p) && !(*p == '.' && p[1] && p[1] != '.')) return false;
    return true;
}
static int ver_cmp(const char *a, const char *b)
{
    while (*a || *b)
    {
        unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}
/* only program files under .rockbox, and none of what the owner keeps
 * there; letters a shell script can quote without care */
static bool path_ok(const char *p)
{
    size_t n = strlen(p);
    if (n < 10 || n > OTA_PATH_MAX || strncmp(p, ".rockbox/", 9) || p[n - 1] == '/') return false;
    for (const char *c = p; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr("._-+/ ", *c)) return false;
    for (const char *s = p; s; s = strchr(s, '/') ? strchr(s, '/') + 1 : NULL)
    {
        size_t l = strcspn(s, "/");
        if (!l || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return false;
    }
    static const char *const keep_dirs[] = { ".rockbox/update/", ".rockbox/sloppy/", ".rockbox/playlists/" };
    for (unsigned i = 0; i < sizeof keep_dirs / sizeof *keep_dirs; i++) if (!strncasecmp(p, keep_dirs[i], strlen(keep_dirs[i]))) return false;
    const char *base = strrchr(p, '/') + 1, *dot = strrchr(base, '.');
    static const char *const keep_names[] = { "config.cfg", "fixed.cfg", ".playlist_control", "nvram.bin", "battery_bench.txt" };
    for (unsigned i = 0; i < sizeof keep_names / sizeof *keep_names; i++) if (!strcasecmp(base, keep_names[i])) return false;
    if (dot && (!strcasecmp(dot, ".tcd") || !strcasecmp(dot, ".bmark"))) return false;
    return true;
}
static void mkdirs(const char *path)       /* the folders above path */
{
    char b[700];
    snprintf(b, sizeof b, "%s", path);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(b, 0777); *s = '/'; }
}
static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    char b[800];
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(b, sizeof b, "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(b, &st) && S_ISDIR(st.st_mode)) rm_tree(b);
        else unlink(b);
    }
    closedir(d);
    rmdir(path);
}
static void fsync_dir_of(const char *path)
{
    char b[700];
    snprintf(b, sizeof b, "%s", path);
    char *s = strrchr(b, '/');
    if (s) *s = '\0';
    int fd = open(b, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}
static bool exists(const char *p) { struct stat st; return !stat(p, &st); }
static long long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long long)st.st_size; }
static uint64_t card_free(void)
{
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (sl_ota_free_fake >= 0) return (uint64_t)sl_ota_free_fake;
#endif
    struct statvfs v;
    return statvfs(ota_root(), &v) ? 0 : (uint64_t)v.f_bavail * v.f_frsize;
}
static bool stopping(void)
{
    pthread_mutex_lock(&ota_lock);
    bool s = ota.stop || ota.hold;
    pthread_mutex_unlock(&ota_lock);
    return s;
}
static bool read_at(int fd, void *b, size_t n, off_t at)
{
    char *p = b;
    while (n)
    {
        ssize_t r = pread(fd, p, n, at);
        if (r <= 0) return false;
        p += r; n -= (size_t)r; at += r;
    }
    return true;
}
static bool write_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
/* a file's SHA-256, its size checked on the way */
static bool sha_file(const char *path, uint8_t out[32], uint64_t *size, char *buf)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct ota_sha h;
    bool ok = sha_begin(&h);
    uint64_t n = 0;
    ssize_t r;
    while (ok && (r = read(fd, buf, OTA_BUF)) > 0) { sha_add(&h, buf, (size_t)r); n += (uint64_t)r; }
    close(fd);
    ok = sha_end(&h, out) && ok;
    if (size) *size = n;
    return ok;
}

/* ------------------------------------------------------------ the manifest */
struct js { const char *p, *e; bool bad; };
static void js_ws(struct js *j) { while (j->p < j->e && isspace((unsigned char)*j->p)) j->p++; }
static bool js_is(struct js *j, char c) { js_ws(j); if (j->p < j->e && *j->p == c) { j->p++; return true; } return false; }
static void js_utf8(char **o, char *end, unsigned c)
{
    char t[4];
    int n = c < 0x80 ? (t[0] = (char)c, 1) : c < 0x800 ? (t[0] = (char)(0xc0 | c >> 6), t[1] = (char)(0x80 | (c & 63)), 2)
          : c < 0x10000 ? (t[0] = (char)(0xe0 | c >> 12), t[1] = (char)(0x80 | (c >> 6 & 63)), t[2] = (char)(0x80 | (c & 63)), 3)
          : (t[0] = (char)(0xf0 | c >> 18), t[1] = (char)(0x80 | (c >> 12 & 63)), t[2] = (char)(0x80 | (c >> 6 & 63)), t[3] = (char)(0x80 | (c & 63)), 4);
    if (*o + n < end) { memcpy(*o, t, (size_t)n); *o += n; }
}
/* a string into out (cut at cap; NULL skips it) */
static bool js_str(struct js *j, char *out, size_t cap)
{
    if (!js_is(j, '"')) return j->bad = true, false;
    char *o = out, *end = out ? out + cap : NULL;
    while (j->p < j->e && *j->p != '"')
    {
        unsigned c = (unsigned char)*j->p++;
        if (c < 0x20) return j->bad = true, false;
        if (c == '\\')
        {
            if (j->p >= j->e) return j->bad = true, false;
            char x = *j->p++;
            if (x == 'u')
            {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) { int h = j->p < j->e ? hexv(*j->p++) : -1; if (h < 0) return j->bad = true, false; v = v << 4 | (unsigned)h; }
                if (v >= 0xd800 && v < 0xdc00 && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u')
                {
                    unsigned lo = 0;
                    for (int i = 2; i < 6; i++) { int h = hexv(j->p[i]); if (h < 0) return j->bad = true, false; lo = lo << 4 | (unsigned)h; }
                    if (lo >= 0xdc00 && lo < 0xe000) { v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00); j->p += 6; }
                }
                if (out) js_utf8(&o, end, v);
                continue;
            }
            c = x == 'n' ? '\n' : x == 't' ? '\t' : x == 'r' ? '\r' : x == 'b' ? '\b' : x == 'f' ? '\f' : (unsigned char)x;
        }
        if (out && o + 1 < end) *o++ = (char)c;
    }
    if (out) *o = '\0';
    if (j->p >= j->e) return j->bad = true, false;
    j->p++;
    return true;
}
static bool js_num(struct js *j, uint64_t *v)
{
    js_ws(j);
    if (j->p >= j->e || !isdigit((unsigned char)*j->p)) return j->bad = true, false;
    uint64_t x = 0;
    while (j->p < j->e && isdigit((unsigned char)*j->p)) { x = x * 10 + (uint64_t)(*j->p++ - '0'); if (x > (1ull << 40)) return j->bad = true, false; }
    *v = x;
    return true;
}
static void js_skip(struct js *j, int depth)
{
    js_ws(j);
    if (depth > 16 || j->p >= j->e) { j->bad = true; return; }
    char c = *j->p;
    if (c == '"') { js_str(j, NULL, 0); return; }
    if (c == '{' || c == '[')
    {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (js_is(j, close)) return;
        do
        {
            if (c == '{') { js_str(j, NULL, 0); if (!js_is(j, ':')) { j->bad = true; return; } }
            js_skip(j, depth + 1);
        } while (!j->bad && js_is(j, ','));
        if (!js_is(j, close)) j->bad = true;
        return;
    }
    while (j->p < j->e && (isalnum((unsigned char)*j->p) || strchr("+-.", *j->p))) j->p++;
}
static bool js_bool(struct js *j, bool *v)
{
    js_ws(j);
    if (j->e - j->p >= 4 && !strncmp(j->p, "true", 4)) { j->p += 4; *v = true; return true; }
    if (j->e - j->p >= 5 && !strncmp(j->p, "false", 5)) { j->p += 5; *v = false; return true; }
    return j->bad = true, false;
}
static void man_free(struct ota_man *m)
{
    for (int i = 0; m->f && i < m->nf + m->nr; i++) free(m->f[i].path);
    free(m->f);
    memset(m, 0, sizeof *m);
}
static bool man_add(struct ota_man *m, int *cap, const char *path, uint64_t size, const uint8_t *sha, char flag)
{
    if (m->nf + m->nr >= OTA_FILES_MAX) return false;
    if (m->nf + m->nr == *cap)
    {
        int nc = *cap ? *cap * 2 : 64;
        struct ota_file *f = realloc(m->f, (size_t)nc * sizeof *f);
        if (!f) return false;
        m->f = f; *cap = nc;
    }
    struct ota_file *f = &m->f[m->nf + m->nr];
    if (!(f->path = strdup(path))) return false;
    f->size = size; f->flag = flag;
    if (sha) memcpy(f->sha, sha, 32);
    if (flag == 'r') m->nr++; else m->nf++;
    return true;
}
/* E_OK, E_NOTOURS (not our manifest) or E_PATH (a file it may not write) */
static int man_parse(const char *b, size_t n, const char *lang, struct ota_man *m)
{
    memset(m, 0, sizeof *m);
    struct js j = { b, b + n, false };
    int cap = 0, why = SL_OTA_OK;
    bool files_seen = false;
    char key[32], path[OTA_PATH_MAX + 8], hex[80];
    if (!js_is(&j, '{')) return SL_OTA_E_NOTOURS;
    if (!js_is(&j, '}')) do
    {
        if (!js_str(&j, key, sizeof key) || !js_is(&j, ':')) break;
        if (!strcmp(key, "product")) js_str(&j, m->product, sizeof m->product);
        else if (!strcmp(key, "version")) js_str(&j, m->version, sizeof m->version);
        else if (!strcmp(key, "min_from")) js_str(&j, m->min_from, sizeof m->min_from);
        else if (!strcmp(key, "build_date")) js_str(&j, m->build_date, sizeof m->build_date);
        else if (!strcmp(key, "needs_boot_menu")) js_bool(&j, &m->boot_menu);
        else if (!strcmp(key, "zip") && js_is(&j, '{'))
        {
            if (!js_is(&j, '}')) do
            {
                if (!js_str(&j, key, sizeof key) || !js_is(&j, ':')) break;
                if (!strcmp(key, "size")) js_num(&j, &m->zip_size);
                else if (!strcmp(key, "sha256")) { js_str(&j, hex, sizeof hex); m->zip_sha_ok = unhex32(hex, m->zip_sha); }
                else js_skip(&j, 1);
            } while (!j.bad && js_is(&j, ','));
            if (!j.bad && !js_is(&j, '}')) j.bad = true;
        }
        else if ((!strcmp(key, "files") || !strcmp(key, "remove")) && js_is(&j, '['))
        {
            bool rem = key[0] == 'r';
            files_seen |= !rem;
            if (!js_is(&j, ']')) do
            {
                uint64_t size = 0;
                uint8_t sha[32];
                bool sha_ok = false;
                path[0] = '\0';
                if (rem) js_str(&j, path, sizeof path);
                else if (js_is(&j, '{'))
                {
                    if (!js_is(&j, '}')) do
                    {
                        if (!js_str(&j, key, sizeof key) || !js_is(&j, ':')) break;
                        if (!strcmp(key, "path")) js_str(&j, path, sizeof path);
                        else if (!strcmp(key, "size")) js_num(&j, &size);
                        else if (!strcmp(key, "sha256")) { js_str(&j, hex, sizeof hex); sha_ok = unhex32(hex, sha); }
                        else js_skip(&j, 2);
                    } while (!j.bad && js_is(&j, ','));
                    if (!j.bad && !js_is(&j, '}')) j.bad = true;
                    if (!sha_ok) j.bad = true;
                }
                else j.bad = true;
                if (j.bad) break;
                if (!path_ok(path)) { why = SL_OTA_E_PATH; sl_log("update: refused: the manifest names %.80s", path); }
                else if (!man_add(m, &cap, path, size, rem ? NULL : sha, rem ? 'r' : 'o')) j.bad = true;
            } while (!j.bad && js_is(&j, ','));
            if (!j.bad && !js_is(&j, ']')) j.bad = true;
        }
        else if (!strcmp(key, "notes") && js_is(&j, '{'))
        {
            if (!js_is(&j, '}')) do
            {
                if (!js_str(&j, key, sizeof key) || !js_is(&j, ':')) break;
                if (!strcmp(key, lang)) js_str(&j, m->notes, sizeof m->notes);
                else if (!strcmp(key, "en")) js_str(&j, m->notes_en, sizeof m->notes_en);
                else js_skip(&j, 1);
            } while (!j.bad && js_is(&j, ','));
            if (!j.bad && !js_is(&j, '}')) j.bad = true;
        }
        else js_skip(&j, 0);
    } while (!j.bad && js_is(&j, ','));
    if (!j.bad && !js_is(&j, '}')) j.bad = true;
    if (!m->notes[0]) memcpy(m->notes, m->notes_en, sizeof m->notes);
    /* the same path twice would be swapped twice */
    for (int a = 0; !j.bad && a < m->nf + m->nr; a++)
        for (int c = a + 1; c < m->nf + m->nr; c++) if (!strcmp(m->f[a].path, m->f[c].path)) { j.bad = true; break; }
    if (j.bad || strcasecmp(m->product, "GabagoolOS") || !ver_ok(m->version) || (m->min_from[0] && !ver_ok(m->min_from)) ||
        !files_seen || !m->nf || !m->zip_size || !m->zip_sha_ok)
        return SL_OTA_E_NOTOURS;
    return why;
}

/* ------------------------------------------------------------ the update file */
struct ota_bundle { uint32_t mlen, slen; char *man; unsigned char sig[64]; uint64_t zoff, total; struct ota_man m; };
static void bundle_free(struct ota_bundle *b) { free(b->man); man_free(&b->m); memset(b, 0, sizeof *b); }
/* the head, the manifest and its signature: why, nothing else read */
static int bundle_open(const char *path, struct ota_bundle *b)
{
    memset(b, 0, sizeof *b);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return SL_OTA_E_CARD;
    unsigned char h[OTA_HEAD];
    struct stat st;
    int why = SL_OTA_OK;
    if (fstat(fd, &st) || !read_at(fd, h, sizeof h, 0) || memcmp(h, OTA_MAGIC, 12)) why = SL_OTA_E_NOTOURS;
    if (!why)
    {
        b->mlen = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
        b->slen = (uint32_t)h[16] | (uint32_t)h[17] << 8 | (uint32_t)h[18] << 16 | (uint32_t)h[19] << 24;
        b->total = (uint64_t)st.st_size;
        b->zoff = OTA_HEAD + (uint64_t)b->mlen + b->slen;
        if (!b->mlen || b->mlen > OTA_MAN_MAX || b->slen != 64) why = SL_OTA_E_NOTOURS;
        else if (b->zoff > b->total) why = SL_OTA_E_DAMAGED;
    }
    if (!why && !(b->man = malloc(b->mlen + 1))) why = SL_OTA_E_CARD;
    if (!why && (!read_at(fd, b->man, b->mlen, OTA_HEAD) || !read_at(fd, b->sig, 64, OTA_HEAD + b->mlen))) why = SL_OTA_E_DAMAGED;
    close(fd);
    if (!why) b->man[b->mlen] = '\0';
    return why;
}
/* the signature, then the manifest it covers, then the versions */
static int bundle_check(struct ota_bundle *b, const char *lang)
{
    if (!ota_key()) return SL_OTA_E_NOKEY;
    if (!cr_load()) return SL_OTA_E_NOCRYPTO;
    if (!cr_verify(ota_key(), b->sig, 64, b->man, b->mlen)) return SL_OTA_E_NOTOURS;
    int why = man_parse(b->man, b->mlen, lang, &b->m);
    if (why) return why;
    if (ver_cmp(b->m.version, OTA_RUNNING) <= 0) return SL_OTA_E_OLDER;
    if (b->m.min_from[0] && ver_cmp(b->m.min_from, OTA_RUNNING) > 0) return SL_OTA_E_MINFROM;
    if (b->zoff + b->m.zip_size != b->total) return SL_OTA_E_DAMAGED;   /* cut short, or more on the end */
    return SL_OTA_OK;
}

/* ------------------------------------------------------------ the zip */
struct zent { char *name; uint32_t csize, usize, loff; uint16_t method; };
struct zip { int fd; uint64_t base, len; struct zent *e; int n; };
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void zip_free(struct zip *z)
{
    for (int i = 0; z->e && i < z->n; i++) free(z->e[i].name);
    free(z->e);
    z->e = NULL; z->n = 0;
}
/* the central directory, read once: names and where each file is */
static bool zip_read(struct zip *z)
{
    unsigned char t[22 + 65535];
    uint64_t tl = z->len < sizeof t ? z->len : sizeof t;
    if (tl < 22 || !read_at(z->fd, t, (size_t)tl, (off_t)(z->base + z->len - tl))) return false;
    long at = -1;
    for (long i = (long)tl - 22; i >= 0; i--) if (le32(t + i) == 0x06054b50) { at = i; break; }
    if (at < 0) return false;
    uint32_t n = le16(t + at + 10), cs = le32(t + at + 12), co = le32(t + at + 16);
    if (!n || n > OTA_FILES_MAX * 2 || (uint64_t)co + cs > z->len) return false;
    unsigned char *cd = malloc(cs ? cs : 1);
    if (!cd || !read_at(z->fd, cd, cs, (off_t)(z->base + co))) { free(cd); return false; }
    z->e = calloc(n, sizeof *z->e);
    bool ok = z->e != NULL;
    uint32_t p = 0;
    for (uint32_t i = 0; ok && i < n; i++)
    {
        if (p + 46 > cs || le32(cd + p) != 0x02014b50) { ok = false; break; }
        uint16_t flags = le16(cd + p + 8), nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl > cs || (flags & 1)) { ok = false; break; }   /* encrypted: not ours */
        struct zent *e = &z->e[z->n];
        e->method = le16(cd + p + 10);
        e->csize = le32(cd + p + 20);
        e->usize = le32(cd + p + 24);
        e->loff = le32(cd + p + 42);
        if (!(e->name = malloc(nl + 1u))) { ok = false; break; }
        memcpy(e->name, cd + p + 46, nl);
        e->name[nl] = '\0';
        z->n++;
        p += 46u + nl + xl + cl;
    }
    free(cd);
    return ok;
}
static struct zent *zip_find(struct zip *z, const char *name)
{
    for (int i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}
struct zrd { int fd; uint64_t at, left; bool stop; };
struct zwr { int fd; struct ota_sha h; uint64_t n, cap; bool bad, over; };
static uint32_t zrd_cb(void *block, uint32_t size, void *ctx)
{
    struct zrd *r = ctx;
    if (!r->left || stopping()) { r->stop = r->left != 0; return 0; }
    uint32_t n = r->left < size ? (uint32_t)r->left : size;
    ssize_t g = pread(r->fd, block, n, (off_t)r->at);
    if (g <= 0) return 0;
    r->at += (uint64_t)g; r->left -= (uint64_t)g;
    return (uint32_t)g;
}
static uint32_t zwr_cb(const void *block, uint32_t size, void *ctx)
{
    struct zwr *w = ctx;
    if (w->n + size > w->cap) { w->bad = w->over = true; return 0; }
    if (!write_all(w->fd, block, size)) { w->bad = true; return 0; }
    sha_add(&w->h, block, size);
    w->n += size;
    pthread_mutex_lock(&ota_lock);
    ota.st.done += size;
    pthread_mutex_unlock(&ota_lock);
    return size;
}
/* one file out of the zip into dest: its size and hash checked. why */
static int zip_unpack(struct zip *z, struct zent *e, const struct ota_file *f, const char *dest, char *buf, void *inf)
{
    unsigned char lh[30];
    uint64_t lo = z->base + e->loff;
    if (e->loff + 30ull > z->len || !read_at(z->fd, lh, 30, (off_t)lo) || le32(lh) != 0x04034b50) return SL_OTA_E_DAMAGED;
    uint64_t data = lo + 30 + le16(lh + 26) + le16(lh + 28);
    if (data - z->base + e->csize > z->len || e->usize != f->size || (e->method != 0 && e->method != 8)) return SL_OTA_E_DAMAGED;
    mkdirs(dest);
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) return errno == ENOSPC ? SL_OTA_E_SPACE : SL_OTA_E_CARD;
    struct zrd r = { z->fd, data, e->csize, false };
    struct zwr w = { fd, { 0 }, 0, f->size, false, false };
    int why = sha_begin(&w.h) ? SL_OTA_OK : SL_OTA_E_NOCRYPTO;
    if (!why && e->method == 0)
    {
        uint32_t g;
        while ((g = zrd_cb(buf, OTA_BUF, &r)) > 0) if (zwr_cb(buf, g, &w) != g) break;
        if (r.left || w.bad) why = r.stop ? SL_OTA_E_STOPPED : w.over || !w.bad ? SL_OTA_E_DAMAGED : SL_OTA_E_CARD;
    }
    else if (!why)
    {
        int rc = inflate(inf, INFLATE_RAW, zrd_cb, &r, zwr_cb, &w);
        if (r.stop) why = SL_OTA_E_STOPPED;
        else if (w.bad) why = w.over ? SL_OTA_E_DAMAGED : SL_OTA_E_CARD;
        else if (rc) why = SL_OTA_E_DAMAGED;
    }
    uint8_t sha[32];
    bool hashed = sha_end(&w.h, sha);
    if (!why && (w.n != f->size || !hashed || memcmp(sha, f->sha, 32))) why = SL_OTA_E_DAMAGED;
    if (fsync(fd) && !why) why = SL_OTA_E_CARD;
    if (close(fd) && !why) why = SL_OTA_E_CARD;
    if (why) unlink(dest);
    return why;
}

/* ------------------------------------------------------------ the journal */
struct jop { char flag; uint64_t size; char path[OTA_PATH_MAX + 1]; bool done; };
struct jr { int kind; char from[16], to[16], src[200]; struct jop *op; int n; int state; bool restarted; };
enum { J_NONE, J_LISTED, J_SWAPPING, J_TRIAL, J_COMMITTED, J_UNDONE, J_ROLLEDBACK, J_DONE };
static void jr_free(struct jr *j) { free(j->op); memset(j, 0, sizeof *j); }
/* a power cut, for the tests: false once the steps run out */
static bool step(void)
{
    sl_ota_steps++;
    return !sl_ota_cut_after || sl_ota_steps < sl_ota_cut_after;
}
static bool jr_line(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool jr_line(int fd, const char *fmt, ...)
{
    char b[OTA_PATH_MAX + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof b - 1) return false;
    b[n++] = '\n';
    if (!step()) { write_all(fd, b, (size_t)n / 2); fsync(fd); return false; }   /* torn as a power cut tears it */
    return write_all(fd, b, (size_t)n) && !fsync(fd);
}
#define J_INSTALL UPD("journal")
#define J_REVERT UPD("journal.revert")         /* its own: the install's stays until a revert is done */
static int jr_open_append(const char *name)
{
    char p[600];
    ota_p(p, sizeof p, name);
    return open(p, O_WRONLY | O_APPEND);
}
static bool jr_read(struct jr *j, const char *name)
{
    memset(j, 0, sizeof *j);
    char p[600];
    ota_p(p, sizeof p, name);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    char line[OTA_PATH_MAX + 64];
    int cap = 0;
    bool ok = false;
    while (fgets(line, sizeof line, f))
    {
        size_t n = strlen(line);
        if (!n || line[n - 1] != '\n') break;           /* torn: the last step did not finish */
        line[n - 1] = '\0';
        if (!ok)
        {
            char kind[12];
            if (sscanf(line, "GBJ1 %11s %15s %15s %199[^\n]", kind, j->from, j->to, j->src) < 3) break;
            j->kind = !strcmp(kind, "install") ? 1 : !strcmp(kind, "revert") ? 2 : 0;
            if (!j->kind) break;
            ok = true;
            j->state = J_LISTED;
            continue;
        }
        if (line[0] == 'F' && j->state == J_LISTED)
        {
            int i;
            char flag;
            unsigned long long size;
            int off = 0;
            if (sscanf(line, "F %d %c %llu %n", &i, &flag, &size, &off) < 3 || !off || i != j->n || !path_ok(line + off)) continue;
            if (j->n == cap)
            {
                int nc = cap ? cap * 2 : 32;
                struct jop *o = realloc(j->op, (size_t)nc * sizeof *o);
                if (!o) break;
                j->op = o; cap = nc;
            }
            struct jop *o = &j->op[j->n++];
            o->flag = flag; o->size = size; o->done = false;
            snprintf(o->path, sizeof o->path, "%s", line + off);
        }
        else if (line[0] == 'D') { int i = atoi(line + 2); if (i >= 0 && i < j->n) j->op[i].done = true; }
        else if (!strcmp(line, "S swapping")) j->state = J_SWAPPING;
        else if (!strcmp(line, "S trial")) j->state = J_TRIAL;
        else if (!strcmp(line, "S done")) j->state = J_DONE;
        else if (!strcmp(line, "S committed")) j->state = J_COMMITTED;
        else if (!strcmp(line, "S undone")) j->state = J_UNDONE;
        else if (!strcmp(line, "S rolledback")) j->state = J_ROLLEDBACK;
        else if (!strcmp(line, "S restart")) j->restarted = true;
    }
    fclose(f);
    if (!ok) jr_free(j);
    return ok;
}
static void op_paths(const struct jop *o, char *t, char *s, char *pv, size_t n)
{
    snprintf(t, n, "%s/%s", ota_root(), o->path);
    snprintf(s, n, "%s" SL_OTA_DIR "/stage/%s", ota_root(), o->path);
    snprintf(pv, n, "%s" SL_OTA_DIR "/prev/%s", ota_root(), o->path);
}
/* one op forward; again after a cut it does what was left. false when it
 * can't (a staged file gone and the target not the new one) or was cut */
static bool op_forward(const struct jop *o, bool *cut)
{
    char t[600], s[600], pv[600];
    op_paths(o, t, s, pv, sizeof t);
    *cut = false;
    switch (o->flag)
    {
        case 'o':
            if (!exists(s)) return fsize(t) == (long long)o->size;   /* moved in before the cut */
            if (exists(t)) { mkdirs(pv); if (!step()) return !(*cut = true); if (rename(t, pv)) return false; }
            /* fall through */
        case 'n':
            if (!exists(s)) return fsize(t) == (long long)o->size;
            mkdirs(t);
            if (!step()) return !(*cut = true);
            if (rename(s, t)) return false;
            fsync_dir_of(t);
            return true;
        case 'r':
        case 'p':
            if (o->flag == 'r') { if (!exists(t)) return true; mkdirs(pv); if (!step()) return !(*cut = true); return !rename(t, pv); }
            if (!exists(pv)) return true;
            mkdirs(t);
            if (!step()) return !(*cut = true);
            if (rename(pv, t)) return false;
            fsync_dir_of(t);
            return true;
        case 'x':
            if (!exists(t)) return true;
            if (!step()) return !(*cut = true);
            return !unlink(t);
    }
    return false;
}
static void op_back(const struct jop *o)
{
    char t[600], s[600], pv[600];
    op_paths(o, t, s, pv, sizeof t);
    if (o->flag == 'n') { if (!exists(s)) unlink(t); }
    else if ((o->flag == 'o' || o->flag == 'r') && exists(pv)) { mkdirs(t); rename(pv, t); }
}
static void ota_tidy(bool keep_prev)       /* after a swap ends one way or the other */
{
    char p[600];
    ota_p(p, sizeof p, UPD("stage"));
    rm_tree(p);
    if (!keep_prev) { ota_p(p, sizeof p, UPD("prev")); rm_tree(p); }
}
static void undo_remove(void) { char p[600]; ota_p(p, sizeof p, SL_OTA_UNDO); unlink(p); }
static void journal_remove(void) { char p[600]; ota_p(p, sizeof p, J_INSTALL); unlink(p); }
static void revert_journal_remove(void) { char p[600]; ota_p(p, sizeof p, J_REVERT); unlink(p); }
/* the boot menu's way back: a plain script that puts prev/ back, whatever
 * step a swap reached */
static bool undo_write(const struct jr *j)
{
    char p[600], tmp[620];
    ota_p(p, sizeof p, SL_OTA_UNDO);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "#!/bin/sh\n# GabagoolOS: back from %s to %s. Run it from the boot menu if %s never starts:\n"
               "# hold Volume Down while the player starts, then Tools > Run script > this file.\n"
               "R=\"%s\"\nP=\"$R" SL_OTA_DIR "/prev\"\n", j->to, j->from, j->to, ota_root());
    for (int i = 0; i < j->n; i++)
    {
        const struct jop *o = &j->op[i];
        if (o->flag == 'n') fprintf(f, "rm -f \"$R/%s\"\n", o->path);
        else fprintf(f, "if [ -f \"$P/%s\" ]; then mkdir -p \"$(dirname \"$R/%s\")\"; mv -f \"$P/%s\" \"$R/%s\"; fi\n",
                     o->path, o->path, o->path, o->path);
    }
    fprintf(f, "rm -rf \"$R" SL_OTA_DIR "/stage\"\necho 'S undone' >> \"$R" SL_OTA_DIR "/journal\"\n"
               "rm -f \"$R/" SL_OTA_UNDO "\"\nsync\necho \"Back on %s. Restart the player.\"\n", j->from);
    bool ok = !ferror(f) && !fflush(f) && !fsync(fileno(f));
    ok = !fclose(f) && ok;
    if (ok) ok = !rename(tmp, p);
    if (!ok) unlink(tmp);
    return ok;
}
/* ops forward from the first not done; S trial or S done at the end.
 * false when cut (the tests) or impossible */
static bool jr_run(struct jr *j, int fd, bool *cut)
{
    *cut = false;
    for (int i = 0; i < j->n; i++)
    {
        if (j->op[i].done) continue;
        if (!op_forward(&j->op[i], cut)) return false;
        if (!jr_line(fd, "D %d", i)) return !(*cut = true);
        j->op[i].done = true;
    }
    if (j->kind == 1)
    {
        char a[600], b[600];             /* the notes go with what is installed */
        ota_p(a, sizeof a, UPD("stage/manifest.json"));
        ota_p(b, sizeof b, UPD("manifest.json"));
        if (exists(a)) rename(a, b);
    }
    if (!jr_line(fd, j->kind == 1 ? "S trial" : "S done")) return !(*cut = true);
    j->state = j->kind == 1 ? J_TRIAL : J_DONE;
    return true;
}
/* the end of a revert or of an undo: back as before the update */
static void revert_done(void)
{
    char p[600];
    ota_tidy(false);
    ota_p(p, sizeof p, UPD("manifest.json"));
    unlink(p);
    undo_remove();
    journal_remove();
    revert_journal_remove();
}

/* ------------------------------------------------------------ at start */
static void st_installed(const struct jr *j)
{
    ota.st.prev = false;
    ota.st.trial = false;
    if (j->kind != 1 || (j->state != J_TRIAL && j->state != J_COMMITTED)) return;
    if (ver_cmp(j->to, OTA_RUNNING)) return;
    char p[600];
    ota_p(p, sizeof p, UPD("prev"));
    ota.st.prev = true;
    for (int i = 0; i < j->n; i++)
    {
        if (j->op[i].flag == 'n') continue;
        char pv[700];
        snprintf(pv, sizeof pv, "%s/%s", p, j->op[i].path);
        if (!exists(pv)) { ota.st.prev = false; break; }
    }
    snprintf(ota.st.prev_version, sizeof ota.st.prev_version, "%s", j->from);
    ota.st.trial = j->state == J_TRIAL;
}
void sl_ota_boot(void)
{
    struct jr j;
    sl_ota_steps = 0;
    pthread_mutex_lock(&ota_lock);
    ota.booted = true;
    ota.st.key = ota_key() != NULL;
    ota.st.restart = false;
    /* Previous version cut short: finished (its steps can always go on) */
    if (jr_read(&j, J_REVERT))
    {
        int rfd = jr_open_append(J_REVERT);
        bool cut = false;
        if (j.state == J_SWAPPING && !jr_run(&j, rfd, &cut)) sl_log("update: going back to %s could not finish", j.to);
        if (rfd >= 0) close(rfd);
        if (j.state == J_DONE)
        {
            revert_done();
            if (ver_cmp(j.to, OTA_RUNNING)) ota.st.restart = true;   /* no journal left: no second time */
            sl_log("update: back on %s%s", j.to, ota.st.restart ? "; restarting" : "");
            jr_free(&j);
            ota_bump();
            pthread_mutex_unlock(&ota_lock);
            return;
        }
        jr_free(&j);
    }
    revert_journal_remove();             /* none, never begun, or cut in its first line */
    if (!jr_read(&j, J_INSTALL)) { journal_remove(); undo_remove(); ota_bump(); pthread_mutex_unlock(&ota_lock); return; }   /* none, or cut in its first line: nothing began */
    int fd = jr_open_append(J_INSTALL);
    bool cut = false;
    if (j.state == J_LISTED)              /* stopped before the first rename: nothing changed */
    {
        sl_log("update: a swap to %s never began; the card is as it was", j.to);
        journal_remove();
        if (j.kind == 1) undo_remove();   /* a revert that never began: the undo script still holds */
    }
    else if (j.state == J_SWAPPING)
    {
        if (jr_run(&j, fd, &cut))
            sl_log("update: the swap to %s was cut short and is finished now", j.to);
        else if (!cut)
        {
            /* a staged file is gone: back to what was there */
            for (int i = j.n - 1; i >= 0; i--) op_back(&j.op[i]);
            jr_line(fd, "S rolledback");
            sl_log("update: the swap to %s could not finish; %s is back", j.to, j.from);
            j.state = J_ROLLEDBACK;
        }
    }
    if (!cut)
    {
        bool to_new = j.state == J_TRIAL || j.state == J_DONE;
        const char *want = to_new ? j.to : j.from;
        if ((j.state == J_TRIAL || j.state == J_DONE || j.state == J_ROLLEDBACK) && ver_cmp(want, OTA_RUNNING))
        {
            /* the other build is the one running: once, a restart starts the right one */
            if (!j.restarted) { jr_line(fd, "S restart"); ota.st.restart = true; }
            sl_log("update: %s is on the card, %s is running: %s", want, OTA_RUNNING, j.restarted ? "left as it is" : "restarting");
        }
        if (j.state == J_DONE || j.state == J_UNDONE) { revert_done(); sl_log("update: back on %s", j.kind == 2 ? j.to : j.from); }
        else if (j.state == J_ROLLEDBACK) { ota_tidy(true); journal_remove(); undo_remove(); }
        else if (j.state == J_COMMITTED) undo_remove();
        st_installed(&j);
        if (ota.st.trial) snprintf(ota.st.updated_to, sizeof ota.st.updated_to, "%s", j.to);
    }
    if (fd >= 0) close(fd);
    jr_free(&j);
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
}
void sl_ota_healthy(void)
{
    struct jr j;
    pthread_mutex_lock(&ota_lock);
    if (jr_read(&j, J_INSTALL))
    {
        if (j.kind == 1 && j.state == J_TRIAL && !ver_cmp(j.to, OTA_RUNNING))
        {
            int fd = jr_open_append(J_INSTALL);
            if (fd >= 0 && jr_line(fd, "S committed"))
            {
                undo_remove();
                ota.st.trial = false;
                /* the file it came from is done with */
                char p[600];
                if (j.src[0] && strcmp(j.src, "-")) { ota_p(p, sizeof p, j.src); unlink(p); }
                ota_p(p, sizeof p, UPD("incoming.gbupdate"));
                unlink(p);
                sl_log("update: %s kept", j.to);
                ota_bump();
            }
            if (fd >= 0) close(fd);
        }
        jr_free(&j);
    }
    pthread_mutex_unlock(&ota_lock);
}
void sl_ota_seen(void)
{
    pthread_mutex_lock(&ota_lock);
    ota.st.updated_to[0] = '\0';
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
}

/* ------------------------------------------------------------ the swap */
/* the staged plan, its manifest's signature checked again */
static int plan_read(struct ota_man *m, char *src, size_t sn)
{
    char p[600];
    memset(m, 0, sizeof *m);
    ota_p(p, sizeof p, UPD("stage/plan"));
    FILE *f = fopen(p, "r");
    if (!f) return SL_OTA_E_DAMAGED;
    char line[OTA_PATH_MAX + 64];
    int cap = 0, why = SL_OTA_OK;
    if (!fgets(line, sizeof line, f) || sscanf(line, "PLAN %15s %199[^\n]", m->version, src) < 2) why = SL_OTA_E_DAMAGED;
    (void)sn;
    while (!why && fgets(line, sizeof line, f))
    {
        char flag;
        unsigned long long size;
        int off = 0;
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%c %llu %n", &flag, &size, &off) < 2 || !off || !path_ok(line + off) || !strchr("onr", flag) ||
            !man_add(m, &cap, line + off, size, NULL, flag)) why = SL_OTA_E_DAMAGED;
    }
    fclose(f);
    if (why) { man_free(m); return why; }
    /* the manifest it came from, signed */
    char mp[600], sp[600];
    ota_p(mp, sizeof mp, UPD("stage/manifest.json"));
    ota_p(sp, sizeof sp, UPD("stage/manifest.sig"));
    long long ml = fsize(mp);
    char *mb = ml > 0 && ml <= OTA_MAN_MAX ? malloc((size_t)ml) : NULL;
    unsigned char sig[64];
    int a = mb ? open(mp, O_RDONLY) : -1, b = open(sp, O_RDONLY);
    bool ok = a >= 0 && b >= 0 && read_at(a, mb, (size_t)ml, 0) && read_at(b, sig, 64, 0) && fsize(sp) == 64;
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    if (!ok) why = SL_OTA_E_DAMAGED;
    else if (!ota_key()) why = SL_OTA_E_NOKEY;
    else if (!cr_verify(ota_key(), sig, 64, mb, (size_t)ml)) why = SL_OTA_E_NOTOURS;
    else
    {
        struct ota_man mm;
        int w = man_parse(mb, (size_t)ml, "en", &mm);
        if (w || strcmp(mm.version, m->version)) why = w ? w : SL_OTA_E_DAMAGED;
        man_free(&mm);
    }
    free(mb);
    if (why) man_free(m);
    return why;
}
int sl_ota_swap(void)
{
    struct ota_man m;
    char src[200] = "-";
    pthread_mutex_lock(&ota_lock);
    sl_ota_steps = 0;
    int why = ota.hold ? SL_OTA_E_USB : ota.up ? SL_OTA_E_STOPPED : plan_read(&m, src, sizeof src);
    if (why) { pthread_mutex_unlock(&ota_lock); return why; }
    if (ver_cmp(m.version, OTA_RUNNING) <= 0) { man_free(&m); pthread_mutex_unlock(&ota_lock); return SL_OTA_E_OLDER; }
    /* every staged file still whole, as checked */
    struct jr j = { 1, "", "", "", NULL, 0, J_LISTED, false };
    snprintf(j.from, sizeof j.from, "%s", OTA_RUNNING);
    snprintf(j.to, sizeof j.to, "%s", m.version);
    snprintf(j.src, sizeof j.src, "%s", src);
    j.op = calloc((size_t)(m.nf + m.nr) + 1, sizeof *j.op);
    if (!j.op) why = SL_OTA_E_CARD;
    /* removes first, then the files, the program last */
    for (int pass = 0; !why && pass < 3; pass++)
        for (int i = 0; i < m.nf + m.nr; i++)
        {
            struct ota_file *f = &m.f[i];
            bool prog = !strcmp(f->path, OTA_PROG);
            if ((pass == 0) != (f->flag == 'r') || (pass == 2) != prog || (pass == 0 && prog)) continue;
            struct jop *o = &j.op[j.n++];
            o->flag = f->flag; o->size = f->size;
            snprintf(o->path, sizeof o->path, "%s", f->path);
            if (f->flag != 'r')
            {
                char t[600], s[600], pv[600];
                op_paths(o, t, s, pv, sizeof t);
                if (fsize(s) != (long long)f->size) { why = SL_OTA_E_DAMAGED; break; }
            }
        }
    man_free(&m);
    int fd = -1;
    if (!why)
    {
        /* the last update's undo set goes: this one's comes in its place */
        char p[600], tmp[620];
        ota_p(p, sizeof p, J_INSTALL);
        unlink(p);
        revert_journal_remove();
        ota_p(tmp, sizeof tmp, UPD("prev"));
        rm_tree(tmp);
        ota_p(tmp, sizeof tmp, UPD("manifest.json"));
        unlink(tmp);
        if (!undo_write(&j)) why = SL_OTA_E_CARD;
        fd = why ? -1 : open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (!why && fd < 0) why = SL_OTA_E_CARD;
    }
    bool cut = false;
    if (!why)
    {
        bool ok = jr_line(fd, "GBJ1 install %s %s %s", j.from, j.to, j.src);
        for (int i = 0; ok && i < j.n; i++) ok = jr_line(fd, "F %d %c %llu %s", i, j.op[i].flag, (unsigned long long)j.op[i].size, j.op[i].path);
        ok = ok && jr_line(fd, "S swapping");
        if (!ok) cut = true;
        else if (!jr_run(&j, fd, &cut) && !cut)
        {
            for (int i = j.n - 1; i >= 0; i--) op_back(&j.op[i]);
            jr_line(fd, "S rolledback");
            why = SL_OTA_E_CARD;
        }
    }
    if (fd >= 0) close(fd);
    if (cut) why = SL_OTA_E_STOPPED;     /* the tests' power cut: nothing after it happens */
    if (!why)
    {
        ota_tidy(true);
        sl_log("update: %s swapped in (%d files); restarting", j.to, j.n);
        ota.st.state = SL_OTA_NONE;
    }
    jr_free(&j);
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
    return why;
}
int sl_ota_revert(void)
{
    struct jr i, r;
    pthread_mutex_lock(&ota_lock);
    sl_ota_steps = 0;
    int why = ota.hold ? SL_OTA_E_USB : SL_OTA_OK;
    if (!why && (!jr_read(&i, J_INSTALL) || i.kind != 1 || (i.state != J_TRIAL && i.state != J_COMMITTED))) { why = SL_OTA_E_DAMAGED; if (i.kind) jr_free(&i); }
    if (why) { pthread_mutex_unlock(&ota_lock); return why; }
    memset(&r, 0, sizeof r);
    r.kind = 2;
    snprintf(r.from, sizeof r.from, "%s", i.to);
    snprintf(r.to, sizeof r.to, "%s", i.from);
    snprintf(r.src, sizeof r.src, "-");
    r.op = calloc((size_t)i.n + 1, sizeof *r.op);
    for (int pass = 0; r.op && pass < 2; pass++)   /* the program last again */
        for (int k = 0; k < i.n; k++)
        {
            if ((pass == 1) != !strcmp(i.op[k].path, OTA_PROG)) continue;
            struct jop *o = &r.op[r.n++];
            *o = i.op[k];
            o->flag = i.op[k].flag == 'n' ? 'x' : 'p';
            o->done = false;
        }
    jr_free(&i);
    char p[600];
    ota_p(p, sizeof p, J_REVERT);
    int fd = r.op ? open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    bool cut = false;
    if (fd < 0) why = SL_OTA_E_CARD;
    else
    {
        bool ok = jr_line(fd, "GBJ1 revert %s %s -", r.from, r.to);
        for (int k = 0; ok && k < r.n; k++) ok = jr_line(fd, "F %d %c %llu %s", k, r.op[k].flag, (unsigned long long)r.op[k].size, r.op[k].path);
        ok = ok && jr_line(fd, "S swapping");
        if (!ok || !jr_run(&r, fd, &cut)) why = cut || !ok ? SL_OTA_E_STOPPED : SL_OTA_E_CARD;
        close(fd);
    }
    if (!why) { revert_done(); ota.st.prev = false; ota.st.trial = false; sl_log("update: back on %s; restarting", r.to); }
    jr_free(&r);
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
    return why;
}

/* ------------------------------------------------------------ the worker */
static void st_from(const struct ota_bundle *b, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(ota.st.file, sizeof ota.st.file, "%s", strcmp(base, "incoming.gbupdate") ? base : "");
    snprintf(ota.st.version, sizeof ota.st.version, "%s", b->m.version);
    snprintf(ota.st.min_from, sizeof ota.st.min_from, "%s", b->m.min_from);
    snprintf(ota.st.build_date, sizeof ota.st.build_date, "%s", b->m.build_date);
    memcpy(ota.st.notes, b->m.notes, sizeof ota.st.notes);
    ota.st.size = b->total;
    ota.st.files = b->m.nf;
    ota.st.boot_menu = b->m.boot_menu;
}
/* how much an answer matters, to pick one among several files */
static int why_rank(int why)
{
    static const int r[] = { [SL_OTA_E_MINFROM] = 6, [SL_OTA_E_DAMAGED] = 5, [SL_OTA_E_PATH] = 5, [SL_OTA_E_NOTOURS] = 4,
                             [SL_OTA_E_NOKEY] = 7, [SL_OTA_E_NOCRYPTO] = 7, [SL_OTA_E_OLDER] = 3 };
    return why >= 0 && why < (int)(sizeof r / sizeof *r) ? r[why] : 1;
}
static bool is_update_name(const char *n)
{
    size_t l = strlen(n), e = strlen(SL_OTA_EXT);
    return n[0] != '.' && l > e && !strcasecmp(n + l - e, SL_OTA_EXT);
}
static void job_scan(void)
{
    char lang[4];
    pthread_mutex_lock(&ota_lock);
    snprintf(lang, sizeof lang, "%s", ota.lang);
    ota.st.key = ota_key() != NULL;
    pthread_mutex_unlock(&ota_lock);
    bool crypto = cr_load();
    /* staged already: ready, if it still holds */
    struct ota_man pm;
    char src[200];
    if (!plan_read(&pm, src, sizeof src))
    {
        bool newer = ver_cmp(pm.version, OTA_RUNNING) > 0;
        int changed = pm.nf;
        char ver[16];
        snprintf(ver, sizeof ver, "%s", pm.version);
        man_free(&pm);
        if (newer)
        {
            struct ota_bundle b;
            char mp[600];
            ota_p(mp, sizeof mp, UPD("stage/manifest.json"));
            long long ml = fsize(mp);
            memset(&b, 0, sizeof b);
            b.man = ml > 0 ? malloc((size_t)ml) : NULL;
            int fd = b.man ? open(mp, O_RDONLY) : -1;
            if (fd >= 0 && read_at(fd, b.man, (size_t)ml, 0) && !man_parse(b.man, (size_t)ml, lang, &b.m))
            {
                pthread_mutex_lock(&ota_lock);
                st_from(&b, src);
                if (!strcmp(src, "-")) ota.st.file[0] = '\0';
                ota.st.changed = changed;
                ota.st.crypto = crypto;
                ota.st.state = SL_OTA_READY;
                ota.st.why = SL_OTA_OK;
                ota_bump();
                pthread_mutex_unlock(&ota_lock);
                if (fd >= 0) close(fd);
                bundle_free(&b);
                return;
            }
            if (fd >= 0) close(fd);
            bundle_free(&b);
        }
        (void)ver;
    }
    /* the card's top, and what the remote sent */
    char best[512] = "", rej[512] = "";
    struct ota_bundle bb, rb;
    int best_rank = -1;
    int rej_why = SL_OTA_OK;
    memset(&bb, 0, sizeof bb);
    memset(&rb, 0, sizeof rb);
    DIR *d = opendir(ota_root());
    struct dirent *e;
    char path[512];
    bool more = d != NULL;
    bool incoming_done = false;
    while (more || !incoming_done)
    {
        bool phone = false;               /* what the remote sent: kept only while it is the one to install */
        if (more)
        {
            e = readdir(d);
            if (!e) { more = false; continue; }
            if (!is_update_name(e->d_name)) continue;
            snprintf(path, sizeof path, "%s/%s", ota_root(), e->d_name);
        }
        else
        {
            incoming_done = true;
            phone = true;
            ota_p(path, sizeof path, UPD("incoming.gbupdate"));
            if (!exists(path)) continue;
        }
        if (stopping()) break;
        struct ota_bundle b;
        int why = bundle_open(path, &b);
        if (!why) why = bundle_check(&b, lang);
        if (why && phone) unlink(path);   /* refused or not newer: not left filling the card (its reason is in memory) */
        if (why == SL_OTA_E_OLDER && !ver_cmp(b.m.version, OTA_RUNNING)) { bundle_free(&b); continue; }   /* the one installed: quiet */
        if (!why)
        {
            if (!best[0] || ver_cmp(b.m.version, bb.m.version) > 0)
            {
                bundle_free(&bb);
                bb = b;
                snprintf(best, sizeof best, "%s", path);
                continue;
            }
        }
        else if (why_rank(why) > best_rank)
        {
            best_rank = why_rank(why);
            rej_why = why;
            bundle_free(&rb);
            rb = b;
            snprintf(rej, sizeof rej, "%s", path);
            continue;
        }
        bundle_free(&b);
    }
    if (d) closedir(d);
    pthread_mutex_lock(&ota_lock);
    ota.st.crypto = crypto;
    ota.st.changed = 0;
    ota.st.need = 0;
    if (best[0])
    {
        st_from(&bb, best);
        snprintf(ota.found, sizeof ota.found, "%s", best);
        ota.st.state = SL_OTA_FOUND;
        ota.st.why = SL_OTA_OK;
    }
    else if (rej[0])
    {
        st_from(&rb, rej);
        ota.found[0] = '\0';
        ota.st.state = SL_OTA_REJECTED;
        ota.st.why = rej_why;
    }
    else
    {
        ota.found[0] = '\0';
        ota.st.state = SL_OTA_NONE;
        ota.st.why = SL_OTA_OK;
        ota.st.file[0] = ota.st.version[0] = '\0';
        ota.st.notes[0] = '\0';
        /* What's new, of the update installed */
        char mp[600];
        ota_p(mp, sizeof mp, UPD("manifest.json"));
        long long ml = fsize(mp);
        char *mb = ml > 0 && ml <= OTA_MAN_MAX ? malloc((size_t)ml) : NULL;
        int fd = mb ? open(mp, O_RDONLY) : -1;
        struct ota_man m;
        if (fd >= 0 && read_at(fd, mb, (size_t)ml, 0) && man_parse(mb, (size_t)ml, lang, &m) != SL_OTA_E_NOTOURS)
        {
            if (!ver_cmp(m.version, OTA_RUNNING)) memcpy(ota.st.notes, m.notes, sizeof ota.st.notes);
            man_free(&m);
        }
        if (fd >= 0) close(fd);
        free(mb);
    }
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
    bundle_free(&bb);
    bundle_free(&rb);
}
static void stage_wipe(void) { char p[600]; ota_p(p, sizeof p, UPD("stage")); rm_tree(p); }
/* the whole check, then the changed files into stage/ */
static int job_install(const char *path, const char *lang)
{
    struct ota_bundle b;
    int why = bundle_open(path, &b);
    if (!why) why = bundle_check(&b, lang);
    char *buf = malloc(OTA_BUF);
    void *inf = NULL;
    if (!why && !buf) why = SL_OTA_E_CARD;
    struct zip z = { -1, b.zoff, b.m.zip_size, NULL, 0 };
    if (!why && (z.fd = open(path, O_RDONLY)) < 0) why = SL_OTA_E_CARD;
    /* the zip, whole */
    if (!why)
    {
        pthread_mutex_lock(&ota_lock);
        ota.st.phase = SL_OTA_PH_ZIP; ota.st.done = 0; ota.st.total = b.m.zip_size;
        ota_bump();
        pthread_mutex_unlock(&ota_lock);
        struct ota_sha h;
        if (!sha_begin(&h)) why = SL_OTA_E_NOCRYPTO;
        uint64_t at = 0;
        while (!why && at < b.m.zip_size)
        {
            if (stopping()) { why = SL_OTA_E_STOPPED; break; }
            size_t n = b.m.zip_size - at < OTA_BUF ? (size_t)(b.m.zip_size - at) : OTA_BUF;
            if (!read_at(z.fd, buf, n, (off_t)(b.zoff + at))) { why = SL_OTA_E_DAMAGED; break; }
            sha_add(&h, buf, n);
            at += n;
            OTA_SLOW();
            pthread_mutex_lock(&ota_lock);
            ota.st.done = at;
            pthread_mutex_unlock(&ota_lock);
        }
        uint8_t sha[32];
        bool hashed = sha_end(&h, sha);
        if (!why && (!hashed || memcmp(sha, b.m.zip_sha, 32))) why = SL_OTA_E_DAMAGED;
    }
    if (!why && !zip_read(&z)) why = SL_OTA_E_DAMAGED;
    /* what differs from the card: only that is staged */
    uint64_t need = 0, total = 0;
    int changed = 0;
    if (!why)
    {
        pthread_mutex_lock(&ota_lock);
        ota.st.phase = SL_OTA_PH_COMPARE; ota.st.done = 0; ota.st.total = (uint64_t)b.m.nf;
        ota_bump();
        pthread_mutex_unlock(&ota_lock);
        for (int i = 0; !why && i < b.m.nf; i++)
        {
            struct ota_file *f = &b.m.f[i];
            if (stopping()) { why = SL_OTA_E_STOPPED; break; }
            if (!zip_find(&z, f->path)) { why = SL_OTA_E_DAMAGED; break; }
            char t[600];
            snprintf(t, sizeof t, "%s/%s", ota_root(), f->path);
            long long have = fsize(t);
            uint8_t sha[32];
            uint64_t sz;
            if (have < 0) f->flag = 'n';
            else if ((uint64_t)have == f->size && sha_file(t, sha, &sz, buf) && sz == f->size && !memcmp(sha, f->sha, 32)) f->flag = 's';
            else f->flag = 'o';
            if (f->flag != 's') { need += f->size; changed++; }
            pthread_mutex_lock(&ota_lock);
            ota.st.done = (uint64_t)i + 1;
            pthread_mutex_unlock(&ota_lock);
        }
        total = need;
        uint64_t have = card_free();
        if (!why && have < need + OTA_MARGIN)
        {
            why = SL_OTA_E_SPACE;
            pthread_mutex_lock(&ota_lock);
            ota.st.need = need + OTA_MARGIN - have;
            pthread_mutex_unlock(&ota_lock);
        }
    }
    /* out of the zip, each hashed as it lands */
    if (!why)
    {
        stage_wipe();
        inf = inflate_align <= 8 ? malloc(inflate_size) : NULL;   /* malloc's alignment is enough (aligned_alloc refuses 4 on macOS) */
        if (!inf) why = SL_OTA_E_CARD;
        pthread_mutex_lock(&ota_lock);
        ota.st.phase = SL_OTA_PH_UNPACK; ota.st.done = 0; ota.st.total = total; ota.st.changed = changed;
        ota_bump();
        pthread_mutex_unlock(&ota_lock);
        for (int i = 0; !why && i < b.m.nf; i++)
        {
            struct ota_file *f = &b.m.f[i];
            if (f->flag == 's') continue;
            char s[600];
            snprintf(s, sizeof s, "%s" SL_OTA_DIR "/stage/%s", ota_root(), f->path);
            why = zip_unpack(&z, zip_find(&z, f->path), f, s, buf, inf);
        }
    }
    /* the plan, last: written whole means staged whole */
    if (!why)
    {
        char p[600], tmp[620];
        ota_p(p, sizeof p, UPD("stage/manifest.json"));
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        bool ok = fd >= 0 && write_all(fd, b.man, b.mlen) && !fsync(fd);
        if (fd >= 0) close(fd);
        ota_p(p, sizeof p, UPD("stage/manifest.sig"));
        fd = ok ? open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
        ok = fd >= 0 && write_all(fd, b.sig, 64) && !fsync(fd);
        if (fd >= 0) close(fd);
        ota_p(p, sizeof p, UPD("stage/plan"));
        snprintf(tmp, sizeof tmp, "%s.part", p);
        FILE *f = ok ? fopen(tmp, "w") : NULL;
        const char *rel = path + strlen(ota_root()) + 1;
        if (f)
        {
            fprintf(f, "PLAN %s %s\n", b.m.version, strncmp(rel, ".rockbox/update/", 16) ? rel : "-");
            for (int i = 0; i < b.m.nf + b.m.nr; i++)
            {
                const struct ota_file *x = &b.m.f[i];
                if (x->flag == 's') continue;
                if (x->flag == 'r') { char t[600]; snprintf(t, sizeof t, "%s/%s", ota_root(), x->path); if (!exists(t)) continue; }
                fprintf(f, "%c %llu %s\n", x->flag, (unsigned long long)x->size, x->path);
            }
            ok = !ferror(f) && !fflush(f) && !fsync(fileno(f));
            ok = !fclose(f) && ok;
        }
        else ok = false;
        if (ok) ok = !rename(tmp, p);
        if (!ok) why = errno == ENOSPC ? SL_OTA_E_SPACE : SL_OTA_E_CARD;
    }
    if (why) stage_wipe();
    if (z.fd >= 0) close(z.fd);
    zip_free(&z);
    free(inf);
    free(buf);
    bundle_free(&b);
    return why;
}
static void *ota_loop(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&ota_lock);
    for (;;)
    {
        if (ota.hold) { ota.want_install = false; }
        if (ota.want_install && !ota.hold)
        {
            ota.want_install = false;
            ota.stop = false;
            char path[512], lang[4];
            snprintf(path, sizeof path, "%s", ota.found);
            snprintf(lang, sizeof lang, "%s", ota.lang);
            ota.st.state = SL_OTA_WORKING;
            ota.st.why = SL_OTA_OK;
            ota.st.phase = SL_OTA_PH_ZIP;
            ota.st.done = ota.st.total = 0;
            ota_bump();
            pthread_mutex_unlock(&ota_lock);
            int why = job_install(path, lang);
            pthread_mutex_lock(&ota_lock);
            if (why == SL_OTA_E_STOPPED && ota.hold) why = SL_OTA_E_USB;
            sl_log("update: %s %s", path + strlen(ota_root()), why ? "not staged" : "checked and staged");
            if (why == SL_OTA_E_STOPPED) { ota.want_scan = true; }
            else
            {
                ota.st.state = why ? SL_OTA_REJECTED : SL_OTA_READY;
                ota.st.why = why;
                /* the remote's copy is done with: what matters is staged, or the file itself is bad
                 * (kept when it was only the card: no room, USB mode) */
                if (!why || why == SL_OTA_E_DAMAGED || why == SL_OTA_E_NOTOURS || why == SL_OTA_E_PATH)
                {
                    char p[600];
                    ota_p(p, sizeof p, UPD("incoming.gbupdate"));
                    if (!strcmp(path, p)) unlink(p);
                }
            }
            ota.stop = false;
            ota_bump();
            continue;
        }
        if (ota.want_scan && !ota.hold)
        {
            ota.want_scan = false;
            ota.stop = false;
            int keep = ota.st.state;
            if (keep != SL_OTA_RECEIVING) { ota.st.state = SL_OTA_LOOKING; ota_bump(); }
            pthread_mutex_unlock(&ota_lock);
            job_scan();
            pthread_mutex_lock(&ota_lock);
            continue;
        }
        break;
    }
    ota.up = false;
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
    return NULL;
}
static bool ota_start(void)                /* locked */
{
    if (ota.up) { pthread_cond_broadcast(&ota_cond); return true; }
    if (ota.thread) pthread_join(ota.thread, NULL);
    ota.thread = 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 192 * 1024);   /* the zip's end is read onto the stack (64 KB) */
    ota.up = pthread_create(&ota.thread, &a, ota_loop, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!ota.up) ota.thread = 0;
    return ota.up;
}
void sl_ota_lang(const char *code)
{
    pthread_mutex_lock(&ota_lock);
    snprintf(ota.lang, sizeof ota.lang, "%s", code && *code ? code : "en");
    pthread_mutex_unlock(&ota_lock);
}
void sl_ota_scan(void)
{
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (sl_ota_test_frozen) return;
#endif
    pthread_mutex_lock(&ota_lock);
    if (!ota.hold && ota.st.state != SL_OTA_WORKING) { ota.want_scan = true; ota_start(); }
    pthread_mutex_unlock(&ota_lock);
}
bool sl_ota_install(void)
{
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (sl_ota_test_frozen) return true;
#endif
    pthread_mutex_lock(&ota_lock);
    bool ok = !ota.hold && ota.found[0] && ota.st.state == SL_OTA_FOUND;
    if (ok) { ota.want_install = true; ok = ota_start(); }
    pthread_mutex_unlock(&ota_lock);
    return ok;
}
void sl_ota_cancel(void)
{
    pthread_mutex_lock(&ota_lock);
    if (ota.up) ota.stop = true;
    pthread_mutex_unlock(&ota_lock);
}
void sl_ota_status(struct sl_ota_status *s)
{
    pthread_mutex_lock(&ota_lock);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (!sl_ota_test_frozen)
#endif
    ota.st.key = ota_key() != NULL;
    if (ota.up_fd >= 0) { ota.st.rx = ota.up_got; ota.st.rx_total = ota.up_len; }
    *s = ota.st;
    pthread_mutex_unlock(&ota_lock);
}
bool sl_ota_busy(void)
{
    pthread_mutex_lock(&ota_lock);
    bool b = ota.up;
    pthread_mutex_unlock(&ota_lock);
    return b;
}
static void upload_drop(void)              /* locked */
{
    char p[600];
    if (ota.up_fd >= 0) close(ota.up_fd);
    ota.up_fd = -1;
    ota_p(p, sizeof p, UPD("incoming.part"));
    unlink(p);
}
void sl_ota_hold(bool hold)
{
    pthread_mutex_lock(&ota_lock);
    ota.hold = hold;
    if (hold)
    {
        ota.stop = true;
        if (ota.up_fd >= 0) { upload_drop(); ota.st.state = SL_OTA_REJECTED; ota.st.why = SL_OTA_E_USB; }
    }
    else
    {
        ota.stop = false;
        if (ota.st.why == SL_OTA_E_USB) { ota.want_scan = true; ota_start(); }
    }
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
}

/* ------------------------------------------------------------ the upload */
int sl_ota_upload_begin(uint64_t len)
{
    pthread_mutex_lock(&ota_lock);
    int code = 0;
    if (!ota_key()) code = 403;
    else if (ota.hold) code = 503;
    else if (ota.up_fd >= 0 || ota.st.state == SL_OTA_WORKING) code = 409;
    else if (!len || len > (1ull << 31)) code = 413;
    else if (card_free() < 3 * len) code = 507;   /* the file, then what it unpacks to */
    if (!code)
    {
        char p[600];
        ota_p(p, sizeof p, UPD("incoming.part"));
        mkdirs(p);
        ota.up_fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (ota.up_fd < 0) code = 507;
        else
        {
            ota.up_got = 0; ota.up_len = len;
            ota.st.rx = 0; ota.st.rx_total = len;
            ota.st.state = SL_OTA_RECEIVING; ota.st.why = SL_OTA_OK;
            ota.found[0] = '\0';
        }
    }
    if (code == 507 && ota.up_fd < 0) { ota.st.need = 3 * len > card_free() ? 3 * len - card_free() : 0; }
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
    return code;
}
bool sl_ota_upload_write(const void *b, size_t n)
{
    pthread_mutex_lock(&ota_lock);
    bool ok = ota.up_fd >= 0 && !ota.hold && ota.up_got + n <= ota.up_len && write_all(ota.up_fd, b, n);
    if (ok) ota.up_got += n;
    pthread_mutex_unlock(&ota_lock);
    return ok;
}
void sl_ota_upload_end(bool complete)
{
    pthread_mutex_lock(&ota_lock);
    if (ota.up_fd < 0) { pthread_mutex_unlock(&ota_lock); return; }
    complete = complete && ota.up_got == ota.up_len && !fsync(ota.up_fd);
    if (close(ota.up_fd)) complete = false;
    ota.up_fd = -1;
    char a[600], b[600];
    ota_p(a, sizeof a, UPD("incoming.part"));
    ota_p(b, sizeof b, UPD("incoming.gbupdate"));
    if (complete && !rename(a, b))
    {
        ota.st.state = SL_OTA_LOOKING;
        ota.want_scan = true;
        ota_start();
    }
    else
    {
        unlink(a);
        ota.st.state = SL_OTA_REJECTED;
        ota.st.why = ota.hold ? SL_OTA_E_USB : SL_OTA_E_DAMAGED;
    }
    sl_log("update: %llu of %llu bytes from the remote%s", (unsigned long long)ota.up_got, (unsigned long long)ota.up_len,
           complete ? "" : ", not kept");
    ota_bump();
    pthread_mutex_unlock(&ota_lock);
}

#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
size_t sl_ota_test_static_bytes(void) { return sizeof ota + sizeof cr; }
void sl_ota_test_reset(void)
{
    pthread_mutex_lock(&ota_lock);
    ota.stop = true;
    pthread_mutex_unlock(&ota_lock);
    for (;;)
    {
        pthread_mutex_lock(&ota_lock);
        bool up = ota.up;
        pthread_mutex_unlock(&ota_lock);
        if (!up) break;
        usleep(2000);
    }
    pthread_mutex_lock(&ota_lock);
    if (ota.thread) pthread_join(ota.thread, NULL);
    ota.thread = 0;
    if (ota.up_fd >= 0) upload_drop();
    unsigned seq = ota.st.seq;
    memset(&ota.st, 0, sizeof ota.st);
    ota.st.seq = seq + 1;
    ota.found[0] = '\0';
    ota.stop = ota.hold = ota.want_scan = ota.want_install = false;
    pthread_mutex_unlock(&ota_lock);
    pthread_mutex_lock(&cr_lock);
    memset(&cr, 0, sizeof cr);           /* loaded again, maybe from another library */
    pthread_mutex_unlock(&cr_lock);
}
void sl_ota_test_force(const struct sl_ota_status *s)
{
    pthread_mutex_lock(&ota_lock);
    unsigned seq = ota.st.seq;
    ota.st = *s;
    ota.st.seq = seq + 1;
    pthread_mutex_unlock(&ota_lock);
}
#endif
