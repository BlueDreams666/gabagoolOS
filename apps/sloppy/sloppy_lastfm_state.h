/* GabagoolOS and Last.fm: the request a scrobble becomes. No LVGL, no
 * Rockbox, no I/O. GPLv2+.
 *
 * Last.fm's API 2.0 takes a form of parameters, signed: every parameter
 * but format, sorted by name, written name then value with nothing
 * between, the shared secret after them, and the MD5 of that in hex as
 * api_sig (last.fm/api/authspec). The session key a sign-in gives never
 * runs out; only the owner taking the player off their Last.fm account
 * ends it (error 9), and then the player says so and keeps what waits. */
#ifndef SLOPPY_LASTFM_STATE_H
#define SLOPPY_LASTFM_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_LFM_API "https://ws.audioscrobbler.com/2.0/"
#define SL_LFM_AUTH "https://www.last.fm/api/auth/"
#define SL_LFM_BATCH 20                 /* scrobbles per request (Last.fm takes up to 50) */
#define SL_LFM_PARAMS (4 + 7 * SL_LFM_BATCH)

/* Last.fm's error codes the player acts on */
enum { SL_LFM_E_AUTH = 4, SL_LFM_E_SESSION = 9, SL_LFM_E_KEY = 10, SL_LFM_E_OFFLINE = 11, SL_LFM_E_SIG = 13,
       SL_LFM_E_UNAUTHORISED_TOKEN = 14, SL_LFM_E_TOKEN_EXPIRED = 15, SL_LFM_E_TEMPORARY = 16,
       SL_LFM_E_SUSPENDED = 26, SL_LFM_E_RATE = 29 };

/* ------------------------------------------------------------ MD5 (RFC 1321) */
struct sl_md5 { uint32_t a, b, c, d; uint64_t len; uint8_t buf[64]; size_t n; };
static inline void sl_md5_block(struct sl_md5 *m, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t r[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = c;
        c = b;
        uint32_t x = a + f + k[i] + w[g];
        b = b + ((x << r[i]) | (x >> (32 - r[i])));
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}
static inline void sl_md5_init(struct sl_md5 *m) { m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476; m->len = 0; m->n = 0; }
static inline void sl_md5_add(struct sl_md5 *m, const void *data, size_t n)
{
    const uint8_t *p = data;
    m->len += n;
    while (n)
    {
        size_t k = 64 - m->n < n ? 64 - m->n : n;
        memcpy(m->buf + m->n, p, k);
        m->n += k; p += k; n -= k;
        if (m->n == 64) { sl_md5_block(m, m->buf); m->n = 0; }
    }
}
static inline void sl_md5_hex(struct sl_md5 *m, char out[33])
{
    uint64_t bits = m->len * 8;
    uint8_t one = 0x80, zero = 0, l[8];
    sl_md5_add(m, &one, 1);
    while (m->n != 56) sl_md5_add(m, &zero, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
    sl_md5_add(m, l, 8);
    uint32_t v[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", (unsigned)((v[i / 4] >> (8 * (i % 4))) & 0xff));
}

/* ------------------------------------------------------------ requests */
struct sl_lfm_param { char k[16]; const char *v; };

static inline int sl_lfm_param_cmp(const void *a, const void *b)
{
    return strcmp(((const struct sl_lfm_param *)a)->k, ((const struct sl_lfm_param *)b)->k);
}
/* p sorted by name, and its signature with secret */
static inline void sl_lfm_sign(struct sl_lfm_param *p, int n, const char *secret, char sig[33])
{
    struct sl_md5 m;
    qsort(p, (size_t)n, sizeof *p, sl_lfm_param_cmp);
    sl_md5_init(&m);
    for (int i = 0; i < n; i++)
    {
        sl_md5_add(&m, p[i].k, strlen(p[i].k));
        sl_md5_add(&m, p[i].v, strlen(p[i].v));
    }
    sl_md5_add(&m, secret, strlen(secret));
    sl_md5_hex(&m, sig);
}
static inline bool sl_lfm_put(char *out, size_t cap, size_t *o, const char *s, bool encode)
{
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        bool plain = !encode || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (*o + (plain ? 1 : 3) >= cap) return false;
        if (plain) out[(*o)++] = (char)c;
        else *o += (size_t)snprintf(out + *o, cap - *o, "%%%02X", c);
    }
    out[*o] = '\0';
    return true;
}
/* The form: name=value&... (as the signature sorted them), api_sig and
 * format=json at the end. Its length, 0 when it does not fit. */
static inline size_t sl_lfm_body(char *out, size_t cap, const struct sl_lfm_param *p, int n, const char *sig)
{
    size_t o = 0;
    if (cap) out[0] = '\0';
    for (int i = 0; i < n; i++)
        if ((i && !sl_lfm_put(out, cap, &o, "&", false)) || !sl_lfm_put(out, cap, &o, p[i].k, true) ||
            !sl_lfm_put(out, cap, &o, "=", false) || !sl_lfm_put(out, cap, &o, p[i].v, true))
            return 0;
    if (!sl_lfm_put(out, cap, &o, "&api_sig=", false) || !sl_lfm_put(out, cap, &o, sig, false) ||
        !sl_lfm_put(out, cap, &o, "&format=json", false))
        return 0;
    return o;
}
/* "error": N in an answer, 0 when there is none */
static inline int sl_lfm_error(const char *json)
{
    const char *e = strstr(json, "\"error\"");
    if (!e) return 0;
    e += 7;
    while (*e == ' ' || *e == ':') e++;
    return atoi(e);
}
/* a scrobble answer's "accepted" and "ignored" counts; false when it is not one */
static inline bool sl_lfm_counts(const char *json, int *accepted, int *ignored)
{
    const char *a = strstr(json, "\"accepted\""), *g = strstr(json, "\"ignored\"");
    if (!a || !g) return false;
    a += 10; g += 9;
    while (*a == ' ' || *a == ':' || *a == '"') a++;
    while (*g == ' ' || *g == ':' || *g == '"') g++;
    *accepted = atoi(a);
    *ignored = atoi(g);
    return true;
}
/* the web address a phone opens to let the player in */
static inline void sl_lfm_auth_url(char *out, size_t cap, const char *key, const char *token)
{
    snprintf(out, cap, SL_LFM_AUTH "?api_key=%s&token=%s", key, token);
}
#endif
