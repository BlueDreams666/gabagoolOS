/* GabagoolOS: music in over Wi-Fi, from a browser (the transfer page) or
 * from LocalSend. See sloppy_share.h. Plain POSIX, no Rockbox headers: the
 * card is the C library's, under sl_net_card_root(). GPLv2+.
 *
 * LocalSend: protocol v2 as published at github.com/localsend/protocol,
 * receiver side, plain HTTP (the "protocol" we announce is "http", which
 * every LocalSend app speaks to a peer that asks for it): multicast
 * discovery on 224.0.0.167:53317, register and info, prepare-upload (the
 * player asks its owner first), upload (sha256 checked when the sender
 * gives one) and cancel. */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "sloppy_net.h"
#include "sloppy_share.h"
#include "sloppy_share_page.h"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0                   /* macOS, for the simulator and the tests */
#endif
int sl_share_web_port = 4399, sl_share_ls_port = 53317;
int sl_share_io_s = 15;                  /* a client that sends nothing for this long is dropped */
#define LS_GROUP "224.0.0.167"
#define SH_CONNS 4                       /* uploads at once (LocalSend sends in parallel) */
#define SH_FILES 256                     /* files in one session */
#define SH_IDLE_MS 60000                 /* a session nothing has come for in this long ends */
#define SH_ASK_MS 60000                  /* the owner has this long to answer */
#define SH_RESERVE (16ull << 20)         /* room kept free on the card */

static uint32_t sh_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------ SHA-256 */
struct sh_sha { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; };
static const uint32_t sh_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define SH_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sh_sha_block(struct sh_sha *s, const uint8_t *p)
{
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = SH_ROR(w[i - 15], 7) ^ SH_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SH_ROR(w[i - 2], 17) ^ SH_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = a[7] + (SH_ROR(a[4], 6) ^ SH_ROR(a[4], 11) ^ SH_ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + sh_k[i] + w[i];
        uint32_t t2 = (SH_ROR(a[0], 2) ^ SH_ROR(a[0], 13) ^ SH_ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sh_sha_init(struct sh_sha *s)
{
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->n = 0;
}
static void sh_sha_add(struct sh_sha *s, const void *d, size_t n)
{
    const uint8_t *p = d;
    s->len += n;
    while (n)
    {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k; p += k; n -= k;
        if (s->n == 64) { sh_sha_block(s, s->buf); s->n = 0; }
    }
}
static void sh_sha_hex(struct sh_sha *s, char out[65])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    sh_sha_add(s, &pad, 1);
    while (s->n != 56) sh_sha_add(s, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sh_sha_add(s, l, 8);
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", (unsigned)s->h[i]);
}

/* ------------------------------------------------------------ names */
bool sl_share_is_music(const char *name)
{
    static const char *const ok[] = { "mp3", "mp2", "mp1", "flac", "m4a", "m4b", "aac", "alac", "ogg", "oga", "opus",
        "wav", "wave", "aif", "aiff", "aifc", "ape", "wv", "dsf", "dff", "mpc", "tta", "wma", "mka", "ac3", "spx",
        "cue", "lrc", "m3u", "m3u8", "jpg", "jpeg", "png", NULL };
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    for (int i = 0; ok[i]; i++) if (!strcasecmp(dot + 1, ok[i])) return true;
    return false;
}
static bool sh_is_song(const char *name)
{
    const char *dot = strrchr(name, '.');
    static const char *const side[] = { "cue", "lrc", "m3u", "m3u8", "jpg", "jpeg", "png", NULL };
    if (!sl_share_is_music(name)) return false;
    for (int i = 0; side[i]; i++) if (!strcasecmp(dot + 1, side[i])) return false;
    return true;
}
void sl_share_clean_path(char *out, size_t n, const char *rel)
{
    size_t o = 0;
    int depth = 0;
    if (n) out[0] = '\0';
    while (*rel && depth < 5)
    {
        while (*rel == '/' || *rel == '\\') rel++;
        size_t len = strcspn(rel, "/\\");
        if (!len) break;
        char part[128];
        size_t k = 0;
        for (size_t i = 0; i < len && k + 1 < sizeof part; i++)
        {
            unsigned char c = (unsigned char)rel[i];
            part[k++] = c < 0x20 || c == 0x7f || strchr("<>:\"|?*", c) ? '_' : (char)c;
        }
        /* a character not cut in half at the end */
        size_t lead_at = k;
        while (lead_at && ((unsigned char)part[lead_at - 1] & 0xc0) == 0x80) lead_at--;
        if (lead_at && (unsigned char)part[lead_at - 1] >= 0xc0)
        {
            unsigned char l = (unsigned char)part[lead_at - 1];
            size_t want = l >= 0xf0 ? 4 : l >= 0xe0 ? 3 : 2;
            if (k - (lead_at - 1) < want) k = lead_at - 1;
        }
        while (k && (part[k - 1] == ' ' || part[k - 1] == '.')) k--;   /* FAT drops these */
        part[k] = '\0';
        size_t lead = 0;
        while (part[lead] == ' ') lead++;
        rel += len;
        if (!part[lead] || !strcmp(part + lead, ".") || !strcmp(part + lead, "..")) continue;
        int w = snprintf(out + o, n - o, "%s%s", o ? "/" : "", part + lead);
        if (w < 0 || (size_t)w >= n - o) { out[o] = '\0'; break; }
        o += (size_t)w;
        depth++;
    }
}

/* ------------------------------------------------------------ the session */
struct sh_file {
    char id[96], token[24], name[192], sha[65];
    uint64_t size;
    int state;                           /* 0 waiting, 1 coming in, 2 landed, 3 failed */
};
static pthread_mutex_t sh_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sh_cond = PTHREAD_COND_INITIALIZER;
static struct sl_rx_status sh_st;
static struct {
    bool active;
    unsigned gen;                        /* moves when a session ends: its writers stop */
    char id[24], ip[48];
    struct sh_file *f;
    int n;
    uint32_t last_ms;
    int answer;                          /* -1 none yet, 0 no, 1 yes */
} ss;
static bool sh_any, sh_held;
static char sh_alias[64] = "GabagoolOS R1", sh_fp[33];

static void sh_rand(char *out, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char r[32];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, r, sizeof r) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof r)
    {
        unsigned s = (unsigned)time(NULL) ^ sh_ms() ^ (unsigned)getpid();
        for (size_t i = 0; i < sizeof r; i++) { s = s * 1103515245u + 12345u; r[i] = (unsigned char)(s >> 16); }
    }
    for (size_t i = 0; i + 1 < n && i < 64; i++) out[i] = hex[(r[i / 2] >> (i % 2 ? 0 : 4)) & 15];
    out[n - 1 < 64 ? n - 1 : 64] = '\0';
}

/* the end of a session (lock held): what landed stays, writers stop */
static void sh_end_locked(int state, const char *why)
{
    if (!ss.active) return;
    ss.active = false;
    ss.gen++;
    free(ss.f);
    ss.f = NULL;
    ss.n = 0;
    sh_st.state = state;
    sh_st.current[0] = '\0';
    snprintf(sh_st.why, sizeof sh_st.why, "%s", why ? why : "");
    sh_st.seq++;
    pthread_cond_broadcast(&sh_cond);
}
/* a new session (lock held); false when one is going */
static bool sh_begin_locked(int kind, const char *from, const char *ip)
{
    if (ss.active && sh_ms() - ss.last_ms > SH_IDLE_MS) sh_end_locked(SL_RX_ENDED, "Nothing more came");
    if (ss.active) return false;
    ss.active = true;
    ss.gen++;
    ss.last_ms = sh_ms();
    ss.answer = -1;
    snprintf(ss.ip, sizeof ss.ip, "%s", ip);
    sh_rand(ss.id, sizeof ss.id);
    free(ss.f);
    ss.f = NULL;
    ss.n = 0;
    bool web = sh_st.web_on, ls = sh_st.ls_on;
    memset(&sh_st, 0, sizeof sh_st);
    sh_st.web_on = web;
    sh_st.ls_on = ls;
    sh_st.kind = kind;
    snprintf(sh_st.from, sizeof sh_st.from, "%s", from);
    sh_st.state = SL_RX_RECEIVING;
    return true;
}
static void sh_check_done_locked(void)
{
    if (!ss.active || sh_st.kind != SL_RX_LOCALSEND || sh_st.state != SL_RX_RECEIVING) return;
    for (int i = 0; i < ss.n; i++) if (ss.f[i].state < 2) return;
    sh_end_locked(SL_RX_DONE, NULL);
}

/* free space on the card, in bytes; -1 unknown */
static long long sh_free(void)
{
    struct statvfs v;
    const char *root = sl_net_card_root();
    if (!root || statvfs(root, &v)) return -1;
    return (long long)v.f_bavail * (long long)v.f_frsize;
}

/* ------------------------------------------------------------ HTTP */
struct sh_conn {
    int fd;
    char ip[48];
    char method[8], path[96], query[1200];
    long long length;                    /* -1 none given */
    bool chunked, cont;
    long long chunk_left;                /* chunked: what is left of this chunk; -1 at the end */
    char *buf;                           /* SH_BUF: what came after the headers, then the body */
    size_t have, at;
};
#define SH_BUF 32768
static void sh_send(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n)
    {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}
static void sh_reply(int fd, int code, const char *type, const char *body, size_t len)
{
    const char *what = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" :
        code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 409 ? "Conflict" :
        code == 413 ? "Payload Too Large" : code == 415 ? "Unsupported Media Type" : code == 422 ? "Unprocessable Entity" :
        code == 429 ? "Too Many Requests" : code == 503 ? "Service Unavailable" : code == 507 ? "Insufficient Storage" :
        "Internal Server Error";
    char h[320];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", code, what, type ? type : "text/plain", (unsigned)len);
    sh_send(fd, h, (size_t)n);
    if (len) sh_send(fd, body, len);
}
static void sh_json(int fd, int code, const char *json) { sh_reply(fd, code, "application/json", json, strlen(json)); }

/* reads the head into its own buffer; c->buf then holds only body bytes */
static bool sh_parse(struct sh_conn *c)
{
    char head[8193];
    size_t got = 0;
    char *end = NULL;
    while (!end && got < sizeof head - 1)
    {
        ssize_t r = recv(c->fd, head + got, sizeof head - 1 - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
        head[got] = '\0';
        end = strstr(head, "\r\n\r\n");
    }
    if (!end) return false;
    size_t body = got - (size_t)(end + 4 - head);
    memcpy(c->buf, end + 4, body);
    c->have = body;
    c->at = 0;
    *end = '\0';
    char *sp = strchr(head, ' '), *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
    if (!sp || !sp2 || sp - head >= (long)sizeof c->method) return false;
    snprintf(c->method, sizeof c->method, "%.*s", (int)(sp - head), head);
    *sp2 = '\0';
    char *q = strchr(sp + 1, '?');
    if (q) *q++ = '\0';
    snprintf(c->path, sizeof c->path, "%s", sp + 1);
    snprintf(c->query, sizeof c->query, "%s", q ? q : "");
    c->length = -1;
    c->chunked = c->cont = false;
    c->chunk_left = 0;
    for (char *l = strstr(sp2 + 1, "\r\n"); l; l = strstr(l + 2, "\r\n"))
    {
        const char *h = l + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) c->length = strtoll(h + 15, NULL, 10);
        else if (!strncasecmp(h, "Transfer-Encoding:", 18) && strcasestr(h, "chunked")) c->chunked = true;
        else if (!strncasecmp(h, "Expect:", 7) && strcasestr(h, "100-continue")) c->cont = true;
    }
    if (c->cont) sh_send(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    return true;
}
static ssize_t sh_raw(struct sh_conn *c, char *out, size_t n)
{
    if (c->at < c->have)
    {
        size_t k = c->have - c->at < n ? c->have - c->at : n;
        memcpy(out, c->buf + c->at, k);
        c->at += k;
        return (ssize_t)k;
    }
    return recv(c->fd, out, n, 0);
}
static bool sh_raw_line(struct sh_conn *c, char *line, size_t n)
{
    size_t k = 0;
    char ch;
    while (sh_raw(c, &ch, 1) == 1)
    {
        if (ch == '\n') { line[k] = '\0'; if (k && line[k - 1] == '\r') line[k - 1] = '\0'; return true; }
        if (k + 1 < n) line[k++] = ch;
    }
    return false;
}
/* the body, a piece at a time: >0 bytes, 0 at its end, -1 broken */
static ssize_t sh_body(struct sh_conn *c, char *out, size_t n)
{
    if (c->chunked)
    {
        if (c->chunk_left < 0) return 0;
        if (!c->chunk_left)
        {
            char line[64];
            if (!sh_raw_line(c, line, sizeof line)) return -1;
            if (!line[0] && !sh_raw_line(c, line, sizeof line)) return -1;   /* the CRLF after a chunk */
            c->chunk_left = strtoll(line, NULL, 16);
            if (!c->chunk_left) { c->chunk_left = -1; sh_raw_line(c, line, sizeof line); return 0; }
        }
        ssize_t r = sh_raw(c, out, (size_t)c->chunk_left < n ? (size_t)c->chunk_left : n);
        if (r > 0) c->chunk_left -= r;
        return r <= 0 ? -1 : r;
    }
    if (c->length <= 0) return 0;
    ssize_t r = sh_raw(c, out, (size_t)c->length < n ? (size_t)c->length : n);
    if (r > 0) c->length -= r;
    return r <= 0 ? -1 : r;
}
/* a small body (JSON) whole into out; false when too long or broken */
static bool sh_body_all(struct sh_conn *c, char *out, size_t cap)
{
    size_t got = 0;
    for (;;)
    {
        if (got + 1 >= cap) return false;
        ssize_t r = sh_body(c, out + got, cap - 1 - got);
        if (r < 0) return false;
        if (!r) break;
        got += (size_t)r;
    }
    out[got] = '\0';
    return true;
}

/* ------------------------------------------------------------ JSON, enough for LocalSend */
struct sh_js { const char *p; int depth; };
static void js_ws(struct sh_js *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n') j->p++; }
static void js_utf8(char *out, size_t cap, size_t *o, unsigned cp)
{
    char b[4];
    int n = cp < 0x80 ? (b[0] = (char)cp, 1) : cp < 0x800 ? (b[0] = (char)(0xc0 | cp >> 6), b[1] = (char)(0x80 | (cp & 63)), 2) :
            cp < 0x10000 ? (b[0] = (char)(0xe0 | cp >> 12), b[1] = (char)(0x80 | ((cp >> 6) & 63)), b[2] = (char)(0x80 | (cp & 63)), 3) :
            (b[0] = (char)(0xf0 | cp >> 18), b[1] = (char)(0x80 | ((cp >> 12) & 63)), b[2] = (char)(0x80 | ((cp >> 6) & 63)), b[3] = (char)(0x80 | (cp & 63)), 4);
    if (*o + (size_t)n < cap) { memcpy(out + *o, b, (size_t)n); *o += (size_t)n; }
}
static bool js_str(struct sh_js *j, char *out, size_t cap)
{
    js_ws(j);
    if (*j->p != '"') return false;
    j->p++;
    size_t o = 0;
    while (*j->p && *j->p != '"')
    {
        if (*j->p == '\\')
        {
            j->p++;
            char e = *j->p;
            if (!e) return false;
            j->p++;
            if (e == 'u')
            {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) { if (!isxdigit((unsigned char)j->p[i])) return false; cp = cp * 16 + (unsigned)(isdigit((unsigned char)j->p[i]) ? j->p[i] - '0' : (tolower((unsigned char)j->p[i]) - 'a' + 10)); }
                j->p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && j->p[0] == '\\' && j->p[1] == 'u')
                {
                    unsigned lo = (unsigned)strtoul((char[5]){ j->p[2], j->p[3], j->p[4], j->p[5], 0 }, NULL, 16);
                    if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); j->p += 6; }
                }
                js_utf8(out, cap, &o, cp);
            }
            else
            {
                char ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
                if (o + 1 < cap) out[o++] = ch;
            }
        }
        else { if (o + 1 < cap) out[o++] = *j->p; j->p++; }
    }
    if (*j->p != '"') return false;
    j->p++;
    if (cap) out[o < cap ? o : cap - 1] = '\0';
    return true;
}
static bool js_skip(struct sh_js *j)
{
    js_ws(j);
    char c = *j->p;
    if (c == '"') { char t[2]; return js_str(j, t, sizeof t); }
    if (c == '{' || c == '[')
    {
        char close = c == '{' ? '}' : ']';
        j->p++;
        js_ws(j);
        if (*j->p == close) { j->p++; return true; }
        /* a body nested thousands deep, from anyone on the network, ran
         * the connection thread's stack out and took the player down */
        if (++j->depth > 32) return false;
        for (;;)
        {
            if (c == '{') { char k[2]; if (!js_str(j, k, sizeof k)) return false; js_ws(j); if (*j->p++ != ':') return false; }
            if (!js_skip(j)) return false;
            js_ws(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == close) { j->p++; j->depth--; return true; }
            return false;
        }
    }
    if (!c) return false;
    while (*j->p && !strchr(",}] \t\r\n", *j->p)) j->p++;   /* a number, true, false, null */
    return true;
}
/* each key of an object in turn: cb gets the key with j at its value and
 * must step past the value (or return false to stop) */
static bool js_object(struct sh_js *j, bool (*cb)(struct sh_js *, const char *, void *), void *ud)
{
    js_ws(j);
    if (*j->p != '{') return false;
    j->p++;
    js_ws(j);
    if (*j->p == '}') { j->p++; return true; }
    for (;;)
    {
        char key[128];
        if (!js_str(j, key, sizeof key)) return false;
        js_ws(j);
        if (*j->p++ != ':') return false;
        if (!cb(j, key, ud)) return false;
        js_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return true; }
        return false;
    }
}
static void js_escape(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------ writing a file */
/* mkdir -p for the folders of path (the C library's path) */
static void sh_mkdirs(char *path)
{
    for (char *s = strchr(path + 1, '/'); s; s = strchr(s + 1, '/'))
    {
        *s = '\0';
        mkdir(path, 0777);
        *s = '/';
    }
}
/* opens the file rel names under the receive folder, as a new file: when
 * the name is taken, " (2)", " (3)"... before its extension. shown gets
 * where it went, from the card's top. -1 when it could not. */
static int sh_create(const char *rel, char *shown, size_t sn)
{
    const char *root = sl_net_card_root();
    char clean[400], path[640];
    sl_share_clean_path(clean, sizeof clean, rel);
    if (!root || !clean[0]) return -1;
    const char *slash = strrchr(clean, '/'), *base = slash ? slash + 1 : clean;
    const char *dot = strrchr(base, '.');
    int stem = dot && dot != base ? (int)(dot - clean) : (int)strlen(clean);
    for (int k = 1; k < 1000; k++)
    {
        if (k == 1) snprintf(path, sizeof path, "%s%s/%s", root, SL_SHARE_DIR, clean);
        else snprintf(path, sizeof path, "%s%s/%.*s (%d)%s", root, SL_SHARE_DIR, stem, clean, k, clean + stem);
        sh_mkdirs(path);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) { snprintf(shown, sn, "%s", path + strlen(root)); return fd; }
        if (errno != EEXIST) return -1;
    }
    return -1;
}
/* the body into a new file under rel. 200 landed; 409 the session moved
 * on; 422 the checksum is wrong; 503 USB mode; 507 the card is full; 500
 * otherwise. Nothing half-written stays. */
static int sh_receive(struct sh_conn *c, const char *rel, uint64_t size, const char *sha, unsigned gen)
{
    char shown[640];
    int fd = sh_create(rel, shown, sizeof shown);
    if (fd < 0) return errno == ENOSPC ? 507 : 500;
    const char *root = sl_net_card_root();
    char path[700];
    snprintf(path, sizeof path, "%s%s", root, shown);
    const char *base = strrchr(shown, '/');
    pthread_mutex_lock(&sh_lock);
    snprintf(sh_st.current, sizeof sh_st.current, "%s", base ? base + 1 : shown);
    pthread_mutex_unlock(&sh_lock);
    struct sh_sha h;
    sh_sha_init(&h);
    char *buf = malloc(SH_BUF);
    int code = buf ? 200 : 500;
    uint64_t got = 0;
    while (code == 200)
    {
        ssize_t r = sh_body(c, buf, SH_BUF);
        if (r < 0) { code = 500; break; }
        if (!r) break;
        pthread_mutex_lock(&sh_lock);
        bool held = sh_held, moved = gen != ss.gen;
        if (!held && !moved) { sh_st.bytes += (uint64_t)r; ss.last_ms = sh_ms(); }
        pthread_mutex_unlock(&sh_lock);
        if (held) { code = 503; break; }
        if (moved) { code = 409; break; }
        if (write(fd, buf, (size_t)r) != r) { code = errno == ENOSPC ? 507 : 500; break; }
        if (sha) sh_sha_add(&h, buf, (size_t)r);
        got += (uint64_t)r;
    }
    free(buf);
    if (code == 200 && size && got != size) code = 500;   /* cut short */
    if (code == 200 && sha && sha[0])
    {
        char hex[65];
        sh_sha_hex(&h, hex);
        if (strcasecmp(hex, sha)) code = 422;
    }
    if (close(fd) && code == 200) code = 500;
    if (code != 200) unlink(path);
    return code;
}

/* ------------------------------------------------------------ the transfer page's API */
static void sh_web(struct sh_conn *c)
{
    char v[400];
    if (!strcmp(c->method, "GET") && (!strcmp(c->path, "/") || !strcmp(c->path, "/index.html")))
    { sh_reply(c->fd, 200, "text/html; charset=utf-8", sl_share_html, sizeof sl_share_html - 1); return; }
    if (!strcmp(c->method, "GET") && !strcmp(c->path, "/api/info"))
    {
        char a[140], j[256];
        js_escape(a, sizeof a, sh_alias);
        long long fr = sh_free();
        snprintf(j, sizeof j, "{\"name\":\"%s\",\"free\":%lld}", a, fr > 0 ? fr : 0);
        sh_json(c->fd, 200, j);
        return;
    }
    if (strcmp(c->method, "POST")) { sh_json(c->fd, 404, "{\"error\":\"not found\"}"); return; }
    if (!strcmp(c->path, "/api/begin"))
    {
        long long files = sl_net_query_param(c->query, "files", v, sizeof v) ? atoll(v) : 0;
        long long bytes = sl_net_query_param(c->query, "bytes", v, sizeof v) ? atoll(v) : 0;
        long long fr = sh_free();
        pthread_mutex_lock(&sh_lock);
        int code = sh_held ? 503 : fr >= 0 && (unsigned long long)bytes + SH_RESERVE > (unsigned long long)fr ? 507 :
                   !sh_begin_locked(SL_RX_WEB, "", c->ip) ? 409 : 200;
        if (code == 200)
        {
            sh_st.files = (int)files;
            sh_st.total = (uint64_t)bytes;
            sh_st.seq++;
        }
        pthread_mutex_unlock(&sh_lock);
        sh_json(c->fd, code, code == 200 ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
    }
    if (!strcmp(c->path, "/api/end"))
    {
        pthread_mutex_lock(&sh_lock);
        if (ss.active && sh_st.kind == SL_RX_WEB && !strcmp(ss.ip, c->ip)) sh_end_locked(SL_RX_DONE, NULL);
        pthread_mutex_unlock(&sh_lock);
        sh_json(c->fd, 200, "{\"ok\":true}");
        return;
    }
    if (!strcmp(c->path, "/api/upload"))
    {
        char rel[400];
        sl_net_query_param(c->query, "path", rel, sizeof rel);
        uint64_t size = sl_net_query_param(c->query, "size", v, sizeof v) ? strtoull(v, NULL, 10) : 0;
        pthread_mutex_lock(&sh_lock);
        int code = sh_held ? 503 : 200;
        if (code == 200 && !(ss.active && sh_st.kind == SL_RX_WEB && !strcmp(ss.ip, c->ip)))
        {
            /* a page that did not say begin: a session of one */
            if (sh_begin_locked(SL_RX_WEB, "", c->ip)) { sh_st.files = 1; sh_st.total = size; sh_st.seq++; }
            else code = 409;
        }
        if (code == 200 && !sh_any && !sl_share_is_music(rel)) { code = 415; sh_st.refused++; }
        unsigned gen = ss.gen;
        pthread_mutex_unlock(&sh_lock);
        if (code == 200) code = sh_receive(c, rel, size, NULL, gen);
        pthread_mutex_lock(&sh_lock);
        if (gen == ss.gen) { if (code == 200) sh_st.done++; else if (code != 415) sh_st.failed++; ss.last_ms = sh_ms(); }
        pthread_mutex_unlock(&sh_lock);
        if (code != 200) { char drain[512]; while (sh_body(c, drain, sizeof drain) > 0) {} }   /* the browser reads the answer after sending */
        sh_json(c->fd, code, code == 200 ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
    }
    sh_json(c->fd, 404, "{\"error\":\"not found\"}");
}

/* ------------------------------------------------------------ LocalSend */
static void ls_info(char *out, size_t cap, bool announce)
{
    char a[140];
    js_escape(a, sizeof a, sh_alias);
    snprintf(out, cap, "{\"alias\":\"%s\",\"version\":\"2.1\",\"deviceModel\":\"HiBy R1\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"http\",\"download\":false,\"announce\":%s}",
             a, sh_fp, sl_share_bound_port(true), announce ? "true" : "false");
}
/* prepare-upload's files, one at a time into the session */
struct ls_prep { struct sh_file *f; int n, offered, refused, songs; uint64_t total; char alias[64]; };
struct ls_one { char id[96], name[192], sha[65]; uint64_t size; };
static bool ls_file_key(struct sh_js *j, const char *key, void *ud)
{
    struct ls_one *o = ud;
    if (!strcmp(key, "id")) return js_str(j, o->id, sizeof o->id) || js_skip(j);
    if (!strcmp(key, "fileName")) return js_str(j, o->name, sizeof o->name) || js_skip(j);
    if (!strcmp(key, "sha256")) { js_ws(j); if (*j->p == '"') return js_str(j, o->sha, sizeof o->sha); return js_skip(j); }
    if (!strcmp(key, "size")) { js_ws(j); o->size = strtoull(j->p, NULL, 10); return js_skip(j); }
    return js_skip(j);
}
static bool ls_files_key(struct sh_js *j, const char *key, void *ud)
{
    struct ls_prep *p = ud;
    struct ls_one o;
    memset(&o, 0, sizeof o);
    snprintf(o.id, sizeof o.id, "%s", key);
    if (!js_object(j, ls_file_key, &o)) return false;
    p->offered++;
    if (!o.name[0] || (!sh_any && !sl_share_is_music(o.name)) || p->n >= SH_FILES) { p->refused++; return true; }
    struct sh_file *f = &p->f[p->n++];
    memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", o.id);
    snprintf(f->name, sizeof f->name, "%s", o.name);
    snprintf(f->sha, sizeof f->sha, "%s", strlen(o.sha) == 64 ? o.sha : "");
    f->size = o.size;
    p->total += o.size;
    p->songs += sh_is_song(o.name);
    return true;
}
static bool ls_info_key(struct sh_js *j, const char *key, void *ud)
{
    struct ls_prep *p = ud;
    if (!strcmp(key, "alias")) return js_str(j, p->alias, sizeof p->alias) || js_skip(j);
    return js_skip(j);
}
static bool ls_prep_key(struct sh_js *j, const char *key, void *ud)
{
    if (!strcmp(key, "info")) return js_object(j, ls_info_key, ud);
    if (!strcmp(key, "files")) return js_object(j, ls_files_key, ud);
    return js_skip(j);
}
static void ls_prepare(struct sh_conn *c)
{
    /* the files' list, previews and all: as long as it says, up to 1 MB */
    size_t bcap = c->length > 0 && c->length < (1 << 20) ? (size_t)c->length + 16 : c->length < 0 ? 256 * 1024 : 0;
    char *body = bcap ? malloc(bcap) : NULL;
    struct ls_prep p;
    memset(&p, 0, sizeof p);
    p.f = calloc(SH_FILES, sizeof *p.f);
    bool ok = body && p.f && sh_body_all(c, body, bcap);
    struct sh_js j = { body };
    if (ok) ok = js_object(&j, ls_prep_key, &p);
    free(body);
    if (!ok) { free(p.f); sh_json(c->fd, 400, "{\"message\":\"Invalid body\"}"); return; }
    if (!p.alias[0]) snprintf(p.alias, sizeof p.alias, "%s", c->ip);
    long long fr = sh_free();
    pthread_mutex_lock(&sh_lock);
    int code = sh_held ? 503 : 0;
    if (!code && !p.n)
    {
        /* nothing it may take: said on the player, refused at once */
        if (sh_begin_locked(SL_RX_LOCALSEND, p.alias, c->ip))
        {
            sh_st.refused = p.refused;
            sh_end_locked(SL_RX_ENDED, "Only songs are received");
        }
        code = 403;
    }
    if (!code && fr >= 0 && p.total + SH_RESERVE > (unsigned long long)fr)
    {
        if (sh_begin_locked(SL_RX_LOCALSEND, p.alias, c->ip)) { sh_st.total = p.total; sh_end_locked(SL_RX_ENDED, "Not enough room on the card"); }
        code = 403;
    }
    if (!code && !sh_begin_locked(SL_RX_LOCALSEND, p.alias, c->ip)) code = 409;
    if (code) { pthread_mutex_unlock(&sh_lock); free(p.f); sh_json(c->fd, code, "{\"message\":\"Rejected\"}"); return; }
    for (int i = 0; i < p.n; i++) sh_rand(p.f[i].token, sizeof p.f[i].token);
    ss.f = p.f;
    ss.n = p.n;
    sh_st.state = SL_RX_ASK;
    sh_st.files = p.n;
    sh_st.songs = p.songs;
    sh_st.refused = p.refused;
    sh_st.total = p.total;
    sh_st.seq++;
    unsigned gen = ss.gen;
    /* the owner says yes or no on the player */
    uint32_t until = sh_ms() + SH_ASK_MS;
    while (ss.answer < 0 && gen == ss.gen && (int32_t)(until - sh_ms()) > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sh_cond, &sh_lock, &ts);
    }
    if (gen != ss.gen) { pthread_mutex_unlock(&sh_lock); sh_json(c->fd, 403, "{\"message\":\"Rejected\"}"); return; }
    if (ss.answer != 1)
    {
        sh_end_locked(SL_RX_ENDED, ss.answer < 0 ? "No answer on the player" : "Declined");
        pthread_mutex_unlock(&sh_lock);
        sh_json(c->fd, 403, "{\"message\":\"Rejected\"}");
        return;
    }
    sh_st.state = SL_RX_RECEIVING;
    sh_st.seq++;
    ss.last_ms = sh_ms();
    /* the answer: the session, and a token for each file taken */
    /* a line each: an id escaped to 199 bytes at most, a token, quotes and a
     * comma (160 was short: an id of quotes ran the reply past its buffer) */
    size_t cap = 64 + (size_t)ss.n * 240, o = 0;
    char *out = malloc(cap);
    if (!out) { sh_end_locked(SL_RX_ENDED, "No memory"); pthread_mutex_unlock(&sh_lock); sh_json(c->fd, 500, "{}"); return; }
    o += (size_t)snprintf(out + o, cap - o, "{\"sessionId\":\"%s\",\"files\":{", ss.id);
    for (int i = 0; i < ss.n; i++)
    {
        char id[200];
        js_escape(id, sizeof id, ss.f[i].id);
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\":\"%s\"", i ? "," : "", id, ss.f[i].token);
    }
    snprintf(out + o, cap - o, "}}");
    pthread_mutex_unlock(&sh_lock);
    sh_json(c->fd, 200, out);
    free(out);
}
static void ls_upload(struct sh_conn *c)
{
    char sid[40], fid[100], tok[40];
    if (!sl_net_query_param(c->query, "sessionId", sid, sizeof sid) || !sl_net_query_param(c->query, "fileId", fid, sizeof fid) ||
        !sl_net_query_param(c->query, "token", tok, sizeof tok))
    { sh_json(c->fd, 400, "{\"message\":\"Missing parameters\"}"); return; }
    pthread_mutex_lock(&sh_lock);
    int k = -1, code = 0;
    if (sh_held) code = 503;
    else if (!ss.active || sh_st.kind != SL_RX_LOCALSEND || sh_st.state != SL_RX_RECEIVING || strcmp(ss.id, sid)) code = 409;
    else if (strcmp(ss.ip, c->ip)) code = 403;
    else
    {
        for (int i = 0; i < ss.n; i++) if (!strcmp(ss.f[i].id, fid)) { k = i; break; }
        if (k < 0 || strcmp(ss.f[k].token, tok) || ss.f[k].state) code = 403;
    }
    char name[192], sha[65];
    uint64_t size = 0;
    unsigned gen = ss.gen;
    if (!code)
    {
        ss.f[k].state = 1;
        snprintf(name, sizeof name, "%s", ss.f[k].name);
        snprintf(sha, sizeof sha, "%s", ss.f[k].sha);
        size = ss.f[k].size;
        ss.last_ms = sh_ms();
    }
    pthread_mutex_unlock(&sh_lock);
    if (code) { sh_json(c->fd, code, "{\"message\":\"Rejected\"}"); return; }
    code = sh_receive(c, name, size, sha[0] ? sha : NULL, gen);
    pthread_mutex_lock(&sh_lock);
    if (gen == ss.gen && k < ss.n)
    {
        ss.f[k].state = code == 200 ? 2 : 3;
        if (code == 200) sh_st.done++; else sh_st.failed++;
        sh_check_done_locked();
    }
    pthread_mutex_unlock(&sh_lock);
    if (code == 200) sh_reply(c->fd, 200, NULL, NULL, 0);
    else sh_json(c->fd, code, "{\"message\":\"Failed\"}");
}
static void sh_ls(struct sh_conn *c)
{
    char out[512];
    if (!strcmp(c->path, "/api/localsend/v2/info") || !strcmp(c->path, "/api/localsend/v1/info"))
    { ls_info(out, sizeof out, false); sh_json(c->fd, 200, out); return; }
    if (strcmp(c->method, "POST")) { sh_json(c->fd, 404, "{}"); return; }
    if (!strcmp(c->path, "/api/localsend/v2/register"))
    {
        char drain[2048];
        sh_body_all(c, drain, sizeof drain);
        ls_info(out, sizeof out, false);
        sh_json(c->fd, 200, out);
        return;
    }
    if (!strcmp(c->path, "/api/localsend/v2/prepare-upload")) { ls_prepare(c); return; }
    if (!strcmp(c->path, "/api/localsend/v2/upload")) { ls_upload(c); return; }
    if (!strcmp(c->path, "/api/localsend/v2/cancel"))
    {
        char sid[40];
        sl_net_query_param(c->query, "sessionId", sid, sizeof sid);
        pthread_mutex_lock(&sh_lock);
        if (ss.active && sh_st.kind == SL_RX_LOCALSEND && !strcmp(ss.id, sid) && !strcmp(ss.ip, c->ip))
            sh_end_locked(SL_RX_ENDED, "Cancelled by the sender");
        pthread_mutex_unlock(&sh_lock);
        sh_reply(c->fd, 200, NULL, NULL, 0);
        return;
    }
    sh_json(c->fd, 404, "{}");
}

/* ------------------------------------------------------------ the servers */
struct sh_server {
    volatile bool run;
    bool up;
    int sock, port;
    bool localsend;
    pthread_t thread;
};
static struct sh_server sh_srv[2];               /* 0 the page, 1 LocalSend */
static int sh_conn_fds[SH_CONNS] = { -1, -1, -1, -1 };
static int sh_conns;
struct sh_job { int fd; bool localsend; char ip[48]; };
static void *sh_conn_thread(void *arg)
{
    struct sh_job job = *(struct sh_job *)arg;
    free(arg);
    struct sh_conn c;
    memset(&c, 0, sizeof c);
    c.fd = job.fd;
    snprintf(c.ip, sizeof c.ip, "%s", job.ip);
    c.buf = malloc(SH_BUF);
    if (c.buf && sh_parse(&c)) { if (job.localsend) sh_ls(&c); else sh_web(&c); }
    free(c.buf);
    pthread_mutex_lock(&sh_lock);
    for (int i = 0; i < SH_CONNS; i++) if (sh_conn_fds[i] == job.fd) sh_conn_fds[i] = -1;
    sh_conns--;
    pthread_cond_broadcast(&sh_cond);
    pthread_mutex_unlock(&sh_lock);
    close(job.fd);
    return NULL;
}
static void *sh_loop(void *arg)
{
    struct sh_server *s = arg;
    while (s->run)
    {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s->sock, &rd);
        struct timeval tv = { 1, 0 };          /* a connection wakes it at once; this is for stop and the idle check */
        pthread_mutex_lock(&sh_lock);
        if (ss.active && sh_st.state == SL_RX_RECEIVING && sh_ms() - ss.last_ms > SH_IDLE_MS)
            sh_end_locked(sh_st.kind == SL_RX_WEB && (sh_st.done || !sh_st.failed) ? SL_RX_DONE : SL_RX_ENDED, "Nothing more came");
        bool room = sh_conns < SH_CONNS;
        pthread_mutex_unlock(&sh_lock);
        if (!room) { struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL); continue; }
        if (select(s->sock + 1, &rd, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        int fd = accept(s->sock, (struct sockaddr *)&from, &fl);
        if (fd < 0) continue;
        struct timeval io = { sl_share_io_s, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof io);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof io);
        struct sh_job *job = calloc(1, sizeof *job);
        if (!job) { close(fd); continue; }
        job->fd = fd;
        job->localsend = s->localsend;
        if (from.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&from)->sin_addr, job->ip, sizeof job->ip);
        else inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&from)->sin6_addr, job->ip, sizeof job->ip);
        pthread_mutex_lock(&sh_lock);
        sh_conns++;
        for (int i = 0; i < SH_CONNS; i++) if (sh_conn_fds[i] < 0) { sh_conn_fds[i] = fd; break; }
        pthread_mutex_unlock(&sh_lock);
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 160 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, sh_conn_thread, job))
        {
            pthread_mutex_lock(&sh_lock);
            sh_conns--;
            for (int i = 0; i < SH_CONNS; i++) if (sh_conn_fds[i] == fd) sh_conn_fds[i] = -1;
            pthread_mutex_unlock(&sh_lock);
            free(job);
            close(fd);
        }
        pthread_attr_destroy(&a);
    }
    return NULL;
}
static bool sh_start(struct sh_server *s, int port, bool localsend)
{
    if (s->up) return true;
    s->sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->sock < 0) return false;
    int one = 1;
    setsockopt(s->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s->sock, (struct sockaddr *)&a, sizeof a) || listen(s->sock, 8)) { close(s->sock); s->sock = -1; return false; }
    socklen_t al = sizeof a;
    getsockname(s->sock, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    s->localsend = localsend;
    s->run = true;
    if (pthread_create(&s->thread, NULL, sh_loop, s)) { s->run = false; close(s->sock); s->sock = -1; return false; }
    s->up = true;
    return true;
}
static void sh_stop(struct sh_server *s)
{
    if (!s->up) return;
    s->run = false;
    pthread_join(s->thread, NULL);
    close(s->sock);
    s->sock = -1;
    s->up = false;
}
/* the uploads still running, cut short and waited for (up to 2 s) */
static void sh_drain(void)
{
    pthread_mutex_lock(&sh_lock);
    for (int i = 0; i < SH_CONNS; i++) if (sh_conn_fds[i] >= 0) shutdown(sh_conn_fds[i], SHUT_RDWR);
    uint32_t until = sh_ms() + 2000;
    while (sh_conns > 0 && (int32_t)(until - sh_ms()) > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sh_cond, &sh_lock, &ts);
    }
    pthread_mutex_unlock(&sh_lock);
}
int sl_share_bound_port(bool localsend) { return sh_srv[localsend].up ? sh_srv[localsend].port : 0; }

bool sl_share_web_start(void)
{
    bool ok = sh_start(&sh_srv[0], sl_share_web_port, false);
    pthread_mutex_lock(&sh_lock);
    sh_st.web_on = ok;
    pthread_mutex_unlock(&sh_lock);
    return ok;
}
void sl_share_web_stop(void)
{
    sh_stop(&sh_srv[0]);
    pthread_mutex_lock(&sh_lock);
    sh_st.web_on = false;
    if (ss.active && sh_st.kind == SL_RX_WEB) sh_end_locked(sh_st.done ? SL_RX_DONE : SL_RX_ENDED, "Transfer stopped");
    pthread_mutex_unlock(&sh_lock);
    if (!sh_srv[1].up) sh_drain();
}

/* ---- LocalSend's discovery: multicast UDP */
static volatile bool ls_run;
static bool ls_up;
static pthread_t ls_thread;
static int ls_udp = -1;
static volatile int ls_announce;                 /* announcements still to send */
static void ls_send(const struct sockaddr_in *to, bool announce)
{
    char msg[512];
    ls_info(msg, sizeof msg, announce);
    sendto(ls_udp, msg, strlen(msg), 0, (const struct sockaddr *)to, sizeof *to);
}
static bool ls_ann_key(struct sh_js *j, const char *key, void *ud)
{
    char *f = ud;                                /* fingerprint[64], then announce at [64] */
    if (!strcmp(key, "fingerprint")) return js_str(j, f, 64) || js_skip(j);
    if (!strcmp(key, "announce")) { js_ws(j); f[64] = !strncmp(j->p, "true", 4); return js_skip(j); }
    return js_skip(j);
}
static void *ls_loop(void *unused)
{
    (void)unused;
    struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons((uint16_t)sl_share_bound_port(true)) };
    inet_pton(AF_INET, LS_GROUP, &group.sin_addr);
    uint32_t next = 0;
    int sent = 0;
    while (ls_run)
    {
        /* our own announcement: at the start, a second and three seconds later */
        if (ls_announce && (int32_t)(sh_ms() - next) >= 0)
        {
            ls_send(&group, true);
            ls_announce--;
            next = sh_ms() + (sent++ ? 2000 : 1000);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls_udp, &rd);
        struct timeval tv = { 1, 0 };          /* an announcement wakes it at once */
        if (select(ls_udp + 1, &rd, NULL, NULL, &tv) <= 0) continue;
        char b[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(ls_udp, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        b[n] = '\0';
        char f[65] = "";
        struct sh_js j = { b };
        if (!js_object(&j, ls_ann_key, f) || !strcmp(f, sh_fp) || !f[64]) continue;
        /* someone looking: we are here, to the group and to them */
        ls_send(&group, false);
        ls_send(&from, false);
    }
    return NULL;
}
bool sl_share_ls_start(const char *alias)
{
    pthread_mutex_lock(&sh_lock);
    if (alias && alias[0]) snprintf(sh_alias, sizeof sh_alias, "%s", alias);
    if (!sh_fp[0]) sh_rand(sh_fp, sizeof sh_fp);
    pthread_mutex_unlock(&sh_lock);
    if (!sh_start(&sh_srv[1], sl_share_ls_port, true)) return false;
    if (!ls_up)
    {
        ls_udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(ls_udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(ls_udp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)sl_share_bound_port(true)), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (ls_udp < 0 || bind(ls_udp, (struct sockaddr *)&a, sizeof a))
        {
            if (ls_udp >= 0) close(ls_udp);
            ls_udp = -1;
            sh_stop(&sh_srv[1]);
            return false;
        }
        struct ip_mreq m;
        inet_pton(AF_INET, LS_GROUP, &m.imr_multiaddr);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(ls_udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);   /* no Wi-Fi yet: unicast still works */
        unsigned char loop = 0;
        setsockopt(ls_udp, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        ls_announce = 3;
        ls_run = true;
        if (pthread_create(&ls_thread, NULL, ls_loop, NULL)) { ls_run = false; close(ls_udp); ls_udp = -1; sh_stop(&sh_srv[1]); return false; }
        ls_up = true;
    }
    pthread_mutex_lock(&sh_lock);
    sh_st.ls_on = true;
    pthread_mutex_unlock(&sh_lock);
    return true;
}
void sl_share_ls_announce(void) { if (ls_up) ls_announce = 1; }
void sl_share_ls_stop(void)
{
    if (ls_up)
    {
        ls_run = false;
        pthread_join(ls_thread, NULL);
        close(ls_udp);
        ls_udp = -1;
        ls_up = false;
    }
    sh_stop(&sh_srv[1]);
    pthread_mutex_lock(&sh_lock);
    sh_st.ls_on = false;
    if (ss.active && sh_st.kind == SL_RX_LOCALSEND) sh_end_locked(SL_RX_ENDED, "LocalSend turned off");
    pthread_mutex_unlock(&sh_lock);
    if (!sh_srv[0].up) sh_drain();
}

/* ------------------------------------------------------------ the interface's side */
void sl_share_status(struct sl_rx_status *s)
{
    pthread_mutex_lock(&sh_lock);
    *s = sh_st;
    pthread_mutex_unlock(&sh_lock);
}
void sl_share_answer(bool accept)
{
    pthread_mutex_lock(&sh_lock);
    if (ss.active && sh_st.state == SL_RX_ASK) { ss.answer = accept; pthread_cond_broadcast(&sh_cond); }
    pthread_mutex_unlock(&sh_lock);
}
void sl_share_seen(void)
{
    pthread_mutex_lock(&sh_lock);
    if (sh_st.state == SL_RX_DONE || sh_st.state == SL_RX_ENDED) sh_st.state = SL_RX_IDLE;
    pthread_mutex_unlock(&sh_lock);
}
void sl_share_allow_any(bool any)
{
    pthread_mutex_lock(&sh_lock);
    sh_any = any;
    pthread_mutex_unlock(&sh_lock);
}
void sl_share_hold(bool hold)
{
    pthread_mutex_lock(&sh_lock);
    sh_held = hold;
    if (hold && ss.active) sh_end_locked(SL_RX_ENDED, "USB mode stopped it");
    pthread_mutex_unlock(&sh_lock);
    if (hold) sh_drain();                         /* the card is handed over with nothing open on it */
}
