/* GabagoolOS podcasts: the worker, the files and the feed parser. See
 * sloppy_podcast.h. Included at the end of sloppy_libjob.c (plain POSIX,
 * the card through sl_net_card_root(); lj_enc and sl_libjob_json_str are
 * that unit's). GPLv2+.
 *
 * One lock guards everything here. The interface takes it for the small
 * file reads and writes it asks for (a show's episode file, the shows);
 * the worker takes it only to write what a job found, never while it
 * waits on the network. Feeds are parsed as they arrive and never kept:
 * a feed of megabytes costs its parser a few KB and the newest
 * SL_POD_MAX_EPS episodes. */
#include <stdarg.h>
#include "sloppy_podcast.h"

char sl_pod_folder[64] = "/Podcasts";
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
unsigned sl_pod_idle_ms = 30000;
#define PJ_IDLE_MS sl_pod_idle_ms
#else
#define PJ_IDLE_MS 30000u
#endif
#define PJ_JOBS 24
#define PJ_QUEUE_MAX 500
#define PJ_PARSE_KEEP (2 * SL_POD_MAX_EPS)   /* items held while a feed is read; the newest SL_POD_MAX_EPS stay */
#define PJ_SEARCH_CAP (384 * 1024)
#define PJ_ART_CAP (2 * 1024 * 1024)
#define PJ_ITUNES "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=25&term="

struct pj_job { int kind; uint32_t show; char arg[400]; uint32_t *keys; int n; };
struct pj_dlq { uint32_t show, key; long long bytes; char *url, *dest, *title; };
static pthread_mutex_t pj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pj_cond = PTHREAD_COND_INITIALIZER;
static struct {
    bool up, online, hold, stop_job;
    pthread_t thread;
    struct pj_job jobs[PJ_JOBS];
    int n_jobs;
    struct sl_pod_status st;
    /* the shows, read once */
    bool shows_read;
    struct sl_pod_show *shows;            /* room for cap_shows, grown eight at a time: a few followed cost a few KB */
    int n_shows, cap_shows;
    /* downloads */
    bool q_read;
    struct pj_dlq *q;                     /* room for cap_q, grown as downloads are asked for */
    int n_q, cap_q;
    bool dl_on;                           /* q[0] is coming down */
    struct sl_net_dl dl;
    uint32_t dl_seen_ms;
    int dl_fails;
    uint32_t dl_retry_at;
    /* answers for the interface */
    struct sl_pod_found *found;
    int n_found;
    struct sl_pod_show prev_show;
    struct sl_pod_eps prev_eps;
    bool prev_ok;
    /* listening */
    bool prog_read, prog_dirty;
    struct sl_pod_progress *prog;
    int n_prog;
} pj;

static uint32_t pj_ms(void) { return lj_ms(); }
static uint32_t pj_now(void) { return (uint32_t)time(NULL); }
/* the card's file, as the C library names it */
static bool pj_real(char *out, size_t n, const char *rb)
{
    const char *root = sl_net_card_root();
    return root && snprintf(out, n, "%s%s", root, rb) < (int)n;
}
static void pj_mkdirs(const char *rb)
{
    char p[512];
    if (!pj_real(p, sizeof p, rb)) return;
    size_t r = strlen(sl_net_card_root());
    for (char *s = p + r + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0777); *s = '/'; }
    mkdir(p, 0777);
}
/* a small file whole, NUL ended; the caller frees it */
static char *pj_slurp(const char *rb, size_t cap, size_t *len)
{
    char p[512];
    struct stat st;
    if (len) *len = 0;
    if (!pj_real(p, sizeof p, rb) || stat(p, &st) || st.st_size < 0 || (size_t)st.st_size > cap) return NULL;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return NULL;
    char *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size)
    {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b) return NULL;
    b[got] = '\0';
    if (len) *len = got;
    return b;
}
/* written aside, then put in place: a cut-off write never shows */
static bool pj_write(const char *rb, const char *data, size_t n)
{
    char p[512], tmp[520];
    if (!pj_real(p, sizeof p, rb) || snprintf(tmp, sizeof tmp, "%s.new", p) >= (int)sizeof tmp) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t put = 0;
    while (put < n)
    {
        ssize_t r = write(fd, data + put, n - put);
        if (r <= 0) break;
        put += (size_t)r;
    }
    bool ok = put == n && fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) unlink(tmp);
    return ok;
}
/* a growing text, for the files */
struct pj_buf { char *b; size_t n, cap; bool bad; };
static void pj_add(struct pj_buf *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pj_add(struct pj_buf *o, const char *fmt, ...)
{
    if (o->bad) return;
    for (;;)
    {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(o->b ? o->b + o->n : NULL, o->b ? o->cap - o->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { o->bad = true; return; }
        if (o->b && o->n + (size_t)k < o->cap) { o->n += (size_t)k; return; }
        size_t want = (o->cap ? o->cap * 2 : 4096) + (size_t)k;
        char *nb = realloc(o->b, want);
        if (!nb) { o->bad = true; return; }
        o->b = nb;
        o->cap = want;
    }
}
/* a field fit for a line of a .tsv: no tabs or line ends */
static const char *pj_f(const char *s, char *b, size_t n)
{
    size_t k = 0;
    for (; s && *s && k + 1 < n; s++) b[k++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    b[k] = '\0';
    return b;
}
/* the next tab-separated field of a line, NUL ended in place */
static char *pj_next(char **s)
{
    char *f = *s;
    if (!f) return (char *)"";
    char *t = strchr(f, '\t');
    if (t) { *t = '\0'; *s = t + 1; } else *s = NULL;
    return f;
}
/* s and its NUL at w; past them */
static char *pj_put(char *w, const char *s)
{
    size_t n = strlen(s) + 1;
    memcpy(w, s, n);
    return w + n;
}
static uint32_t pj_fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}

/* ---------------------------------------------------------------- names */
/* fit for a FAT name: no / \ : * ? " < > |, no control characters, no
 * dots or spaces at the end, at most n - 1 bytes and never half a letter */
/* an extension a server sent ("flac", ".mp3"): only letters and digits,
 * so it can never climb out of its folder ("x/../../a") */
const char *sl_pod_clean_ext(const char *e, const char *fallback)
{
    if (e && *e == '.') e++;
    size_t n = 0;
    while (e && isalnum((unsigned char)e[n])) n++;
    return e && n && n <= 8 && !e[n] ? e : fallback;
}
void sl_pod_clean_name(char *out, size_t n, const char *s)
{
    size_t k = 0;
    bool space = false;
    for (; s && *s && k + 1 < n; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || strchr("/\\:*?\"<>|", c)) c = ' ';
        if (c == ' ') { if (k) space = true; continue; }
        if (space) { if (k + 2 >= n) break; out[k++] = ' '; space = false; }
        out[k++] = (char)c;
    }
    /* a letter cut short: back to its start */
    size_t st = k;
    while (st && ((unsigned char)out[st - 1] & 0xc0) == 0x80) st--;
    if (st && (unsigned char)out[st - 1] >= 0xc0)
    {
        unsigned char lead = (unsigned char)out[st - 1];
        size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        if (k - (st - 1) < need) k = st - 1;
    }
    while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) k--;
    out[k] = '\0';
    if (!k) snprintf(out, n, "Podcast");
}
/* "2026-10-21 The title.mp3": sorted by date in Files, its kind from its address */
void sl_pod_file_name(char *out, size_t n, uint32_t date, const char *title, const char *url)
{
    static const char *const exts[] = { ".mp3", ".m4a", ".m4b", ".aac", ".ogg", ".oga", ".opus", ".flac", ".wav" };
    const char *ext = ".mp3";
    char path[512];
    snprintf(path, sizeof path, "%s", url ? url : "");
    path[strcspn(path, "?#")] = '\0';
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    for (unsigned i = 0; dot && (!slash || dot > slash) && i < sizeof exts / sizeof exts[0]; i++)
        if (!strcasecmp(dot, exts[i])) ext = exts[i];
    char day[16] = "", t[120];
    if (date)
    {
        time_t at = (time_t)date;
        struct tm tm;
        if (gmtime_r(&at, &tm)) snprintf(day, sizeof day, "%04d-%02d-%02d ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }
    sl_pod_clean_name(t, sizeof t, title && title[0] ? title : "Episode");
    snprintf(out, n, "%s%s%s", day, t, ext);
}
bool sl_pod_is_episode(const char *path)
{
    size_t n = strlen(sl_pod_folder);
    return path && n > 1 && !strncasecmp(path, sl_pod_folder, n) && path[n] == '/';
}

/* ---------------------------------------------------------------- times */
static uint32_t pj_civil(int y, int m, int d, int hh, int mm, int ss)
{
    /* days from 1970-01-01 (Howard Hinnant's days_from_civil) */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    long t = days * 86400L + hh * 3600L + mm * 60L + ss;
    return t > 0 ? (uint32_t)t : 0;
}
/* "Tue, 21 Oct 2026 10:00:00 +0000" (RFC 822, as feeds write it, with
 * the zones they use), or ISO 8601 "2026-10-21T10:00:00Z" (Atom) */
uint32_t sl_pod_rfc822(const char *s)
{
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    if (!s) return 0;
    while (*s == ' ') s++;
    int y, m, d, hh = 0, mm = 0, ss = 0;
    if (isdigit((unsigned char)s[0]) && strlen(s) >= 10 && s[4] == '-' && s[7] == '-')
    {
        y = atoi(s); m = atoi(s + 5); d = atoi(s + 8);
        const char *t = s + 10;
        if (*t == 'T' || *t == ' ') { hh = atoi(t + 1); if (t[3] == ':') mm = atoi(t + 4); if (t[6] == ':') ss = atoi(t + 7); t += 9; }
        while (*t && *t != 'Z' && *t != '+' && *t != '-') t++;
        long off = 0;
        if (*t == '+' || *t == '-') { int oh = atoi(t + 1), om = t[3] == ':' ? atoi(t + 4) : atoi(t + 3) % 100; if (t[3] != ':' && strlen(t + 1) >= 4) { oh = atoi(t + 1) / 100; } off = (oh * 60L + om) * 60L * (*t == '-' ? -1 : 1); }
        if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970) return 0;
        long v = (long)pj_civil(y, m, d, hh, mm, ss) - off;
        return v > 0 ? (uint32_t)v : 0;
    }
    const char *c = strchr(s, ',');
    if (c) s = c + 1;
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    d = (int)strtol(s, (char **)&s, 10);
    while (*s == ' ' || *s == '-') s++;
    m = 0;
    for (int i = 0; i < 12; i++) if (!strncasecmp(s, mon[i], 3)) m = i + 1;
    if (!m) return 0;
    while (*s && *s != ' ' && *s != '-') s++;
    while (*s == ' ' || *s == '-') s++;
    y = (int)strtol(s, (char **)&s, 10);
    if (y < 100) y += y < 70 ? 2000 : 1900;
    while (*s == ' ') s++;
    if (isdigit((unsigned char)*s))
    {
        hh = (int)strtol(s, (char **)&s, 10);
        if (*s == ':') mm = (int)strtol(s + 1, (char **)&s, 10);
        if (*s == ':') ss = (int)strtol(s + 1, (char **)&s, 10);
    }
    while (*s == ' ') s++;
    long off = 0;
    if (*s == '+' || *s == '-')
    {
        long z = strtol(s + 1, NULL, 10);
        off = ((z / 100) * 60 + z % 100) * 60 * (*s == '-' ? -1 : 1);
    }
    else
    {
        static const struct { const char *n; int h; } zones[] = {
            { "EDT", -4 }, { "EST", -5 }, { "CDT", -5 }, { "CST", -6 }, { "MDT", -6 }, { "MST", -7 }, { "PDT", -7 }, { "PST", -8 },
            { "BST", 1 }, { "CET", 1 }, { "CEST", 2 } };
        for (unsigned i = 0; i < sizeof zones / sizeof zones[0]; i++)
            if (!strncasecmp(s, zones[i].n, strlen(zones[i].n))) off = zones[i].h * 3600L;
    }
    if (d < 1 || d > 31 || y < 1970) return 0;
    long v = (long)pj_civil(y, m, d, hh, mm, ss) - off;
    return v > 0 ? (uint32_t)v : 0;
}
uint32_t sl_pod_duration(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    long parts[3] = { 0, 0, 0 };
    int n = 0;
    while (n < 3)
    {
        parts[n++] = strtol(s, (char **)&s, 10);
        if (*s != ':') break;
        s++;
    }
    long v = n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : n == 2 ? parts[0] * 60 + parts[1] : parts[0];
    return v > 0 && v < 100 * 3600 ? (uint32_t)v : 0;
}

/* ---------------------------------------------------------------- the parser */
/* A byte at a time: tags, their attributes when they matter, the text of
 * the few elements a podcast needs, CDATA, comments and entities. What it
 * does not need (descriptions, show notes) passes without being kept. */
enum { PX_TEXT, PX_TAG, PX_COMMENT, PX_CDATA, PX_PI };
#define PX_TAG_CAP 2048
#define PX_TEXT_CAP 1024
struct sl_pod_parser {
    struct sl_pod_feed_cb cb;
    int state, depth;
    bool feed, in_item, in_image, show_said;
    char tag[PX_TAG_CAP];
    size_t tn;
    char cap_name[32];                    /* the element whose text is kept, "" none */
    int cap_depth;
    char text[PX_TEXT_CAP];
    size_t xn;
    char ent[12];
    int en;                               /* in an entity: its letters so far, -1 not */
    char match[10];                       /* the end of a comment or CDATA being looked for */
    size_t mn;
    /* the channel */
    char ch_title[160], ch_author[96], ch_owner[96], ch_image[384], ch_image_url[384];
    /* the item being read */
    char it_title[512], it_guid[384], it_url[512], it_date[64], it_dur[24], it_alt[512];
    long long it_bytes, it_alt_bytes;
};
static void px_text_add(struct sl_pod_parser *p, const char *s, size_t n)
{
    if (!p->cap_name[0]) return;
    for (size_t i = 0; i < n && p->xn + 1 < sizeof p->text; i++)
    {
        char c = s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ? ' ' : s[i];
        if (c == ' ' && (!p->xn || p->text[p->xn - 1] == ' ')) continue;   /* runs of space are one */
        p->text[p->xn++] = c;
    }
}
static void px_utf8(char *b, size_t *n, unsigned v)
{
    if (v < 0x80) b[(*n)++] = (char)v;
    else if (v < 0x800) { b[(*n)++] = (char)(0xc0 | v >> 6); b[(*n)++] = (char)(0x80 | (v & 0x3f)); }
    else if (v < 0x10000) { b[(*n)++] = (char)(0xe0 | v >> 12); b[(*n)++] = (char)(0x80 | ((v >> 6) & 0x3f)); b[(*n)++] = (char)(0x80 | (v & 0x3f)); }
    else { b[(*n)++] = (char)(0xf0 | v >> 18); b[(*n)++] = (char)(0x80 | ((v >> 12) & 0x3f)); b[(*n)++] = (char)(0x80 | ((v >> 6) & 0x3f)); b[(*n)++] = (char)(0x80 | (v & 0x3f)); }
}
/* "amp" to "&", "#233" to é; out has room for four bytes */
static size_t px_entity(const char *e, char *out)
{
    static const struct { const char *n; const char *v; } named[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
        { "rsquo", "\xe2\x80\x99" }, { "lsquo", "\xe2\x80\x98" }, { "rdquo", "\xe2\x80\x9d" }, { "ldquo", "\xe2\x80\x9c" },
        { "ndash", "\xe2\x80\x93" }, { "mdash", "\xe2\x80\x94" }, { "hellip", "\xe2\x80\xa6" } };
    size_t n = 0;
    if (e[0] == '#')
    {
        unsigned v = (unsigned)(e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10));
        if (v == 0 || v > 0x10ffff || (v >= 0xd800 && v < 0xe000)) v = 0xfffd;
        if (v < 32 && v != 9 && v != 10 && v != 13) v = ' ';
        px_utf8(out, &n, v);
        return n;
    }
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcmp(e, named[i].n)) { memcpy(out, named[i].v, strlen(named[i].v)); return strlen(named[i].v); }
    return 0;
}
/* entities undone, in place */
static void px_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; )
    {
        if (*r == '&')
        {
            char *semi = strchr(r, ';');
            if (semi && semi - r <= 10)
            {
                char e[12], u[8];
                memcpy(e, r + 1, (size_t)(semi - r - 1));
                e[semi - r - 1] = '\0';
                size_t k = px_entity(e, u);
                if (k && (size_t)(semi + 1 - r) >= k) { memcpy(w, u, k); w += k; r = semi + 1; continue; }
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}
/* the value of attribute a in a tag's text, entities undone */
static bool px_attr(const char *tag, const char *a, char *out, size_t n)
{
    size_t al = strlen(a);
    out[0] = '\0';
    for (const char *s = tag; (s = strstr(s, a)) != NULL; s += al)
    {
        if (s > tag && !isspace((unsigned char)s[-1])) continue;
        const char *v = s + al;
        while (isspace((unsigned char)*v)) v++;
        if (*v != '=') continue;
        v++;
        while (isspace((unsigned char)*v)) v++;
        char q = *v;
        if (q != '"' && q != '\'') continue;
        const char *e = strchr(v + 1, q);
        if (!e) return false;
        size_t k = (size_t)(e - v - 1) < n - 1 ? (size_t)(e - v - 1) : n - 1;
        memcpy(out, v + 1, k);
        out[k] = '\0';
        px_unescape(out);
        return true;
    }
    return false;
}
static void px_trim(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = '\0';
    size_t k = strspn(s, " ");
    if (k) memmove(s, s + k, n - k + 1);
}
static bool px_is(const char *name, const char *want) { return !strcasecmp(name, want); }
static void px_item_end(struct sl_pod_parser *p)
{
    if (!p->it_url[0] && p->it_alt[0]) { snprintf(p->it_url, sizeof p->it_url, "%s", p->it_alt); p->it_bytes = p->it_alt_bytes; }
    if (p->it_url[0] && p->cb.item)
        p->cb.item(p->cb.ctx, p->it_title, p->it_guid, p->it_url, p->it_date, p->it_dur, p->it_bytes);
}
static void px_show(struct sl_pod_parser *p)
{
    if (p->show_said || !p->cb.show) return;
    p->show_said = true;
    p->cb.show(p->cb.ctx, p->ch_title, p->ch_author[0] ? p->ch_author : p->ch_owner,
               p->ch_image[0] ? p->ch_image : p->ch_image_url);
}
/* a whole tag: <name attrs>, </name> or <name/> */
static void px_tag(struct sl_pod_parser *p)
{
    p->tag[p->tn] = '\0';
    char *t = p->tag;
    bool end = *t == '/';
    if (end) t++;
    size_t tl = strlen(t);
    bool empty = tl && t[tl - 1] == '/';
    char name[32];
    size_t k = 0;
    while (t[k] && !isspace((unsigned char)t[k]) && t[k] != '/' && t[k] != '>' && k + 1 < sizeof name) { name[k] = t[k]; k++; }
    name[k] = '\0';
    if (!name[0]) return;
    if (end)
    {
        if (p->cap_name[0] && p->depth == p->cap_depth && px_is(name, p->cap_name))
        {
            p->text[p->xn] = '\0';
            px_trim(p->text);
            const char *v = p->text;
            if (p->in_item)
            {
                if (px_is(name, "title")) snprintf(p->it_title, sizeof p->it_title, "%s", v);
                else if (px_is(name, "guid") || px_is(name, "id")) snprintf(p->it_guid, sizeof p->it_guid, "%s", v);
                else if (px_is(name, "pubDate") || px_is(name, "published") || (px_is(name, "updated") && !p->it_date[0]))
                    snprintf(p->it_date, sizeof p->it_date, "%s", v);
                else if (px_is(name, "itunes:duration")) snprintf(p->it_dur, sizeof p->it_dur, "%s", v);
            }
            else if (p->in_image && px_is(name, "url")) snprintf(p->ch_image_url, sizeof p->ch_image_url, "%s", v);
            else if (px_is(name, "title") && !p->ch_title[0]) snprintf(p->ch_title, sizeof p->ch_title, "%s", v);
            else if (px_is(name, "itunes:author") && !p->ch_author[0]) snprintf(p->ch_author, sizeof p->ch_author, "%s", v);
            else if ((px_is(name, "itunes:name") || px_is(name, "name")) && !p->ch_owner[0]) snprintf(p->ch_owner, sizeof p->ch_owner, "%s", v);
            p->cap_name[0] = '\0';
        }
        if (px_is(name, "item") || px_is(name, "entry")) { if (p->in_item) px_item_end(p); p->in_item = false; }
        else if (px_is(name, "image") && !p->in_item) p->in_image = false;
        if (p->depth > 0) p->depth--;
        return;
    }
    if (!empty) p->depth++;
    if (px_is(name, "rss") || px_is(name, "feed") || px_is(name, "channel")) p->feed = true;
    if (px_is(name, "item") || px_is(name, "entry"))
    {
        px_show(p);                       /* the channel's words come before its items */
        p->in_item = true;
        p->it_title[0] = p->it_guid[0] = p->it_url[0] = p->it_date[0] = p->it_dur[0] = p->it_alt[0] = '\0';
        p->it_bytes = p->it_alt_bytes = 0;
        return;
    }
    char v[512];
    if (p->in_item)
    {
        if (px_is(name, "enclosure") || (px_is(name, "link") && px_attr(t, "rel", v, sizeof v) && !strcasecmp(v, "enclosure")))
        {
            char type[64] = "";
            px_attr(t, "type", type, sizeof type);
            if ((px_attr(t, "url", v, sizeof v) || px_attr(t, "href", v, sizeof v)) && !p->it_url[0] &&
                (!type[0] || !strncasecmp(type, "audio", 5) || strstr(v, ".mp3") || strstr(v, ".m4a")))
            {
                snprintf(p->it_url, sizeof p->it_url, "%s", v);
                if (px_attr(t, "length", v, sizeof v)) p->it_bytes = atoll(v);
            }
        }
        else if (px_is(name, "media:content") && !p->it_alt[0] && px_attr(t, "url", v, sizeof v))
        {
            char type[64] = "";
            px_attr(t, "type", type, sizeof type);
            if (!strncasecmp(type, "audio", 5))
            {
                snprintf(p->it_alt, sizeof p->it_alt, "%s", v);
                if (px_attr(t, "fileSize", v, sizeof v)) p->it_alt_bytes = atoll(v);
            }
        }
        else if (!empty && (px_is(name, "title") || px_is(name, "guid") || px_is(name, "id") || px_is(name, "pubDate") ||
                            px_is(name, "published") || px_is(name, "updated") || px_is(name, "itunes:duration")))
        {
            snprintf(p->cap_name, sizeof p->cap_name, "%s", name);
            p->cap_depth = p->depth;
            p->xn = 0;
        }
        return;
    }
    if (px_is(name, "itunes:image") && px_attr(t, "href", v, sizeof v) && !p->ch_image[0])
        snprintf(p->ch_image, sizeof p->ch_image, "%s", v);
    else if (px_is(name, "image") && !empty) p->in_image = true;
    else if (!empty && ((px_is(name, "title") && !p->in_image) || px_is(name, "itunes:author") || (p->in_image && px_is(name, "url")) ||
                        px_is(name, "itunes:name") || px_is(name, "name")))
    {
        snprintf(p->cap_name, sizeof p->cap_name, "%s", name);
        p->cap_depth = p->depth;
        p->xn = 0;
    }
}
struct sl_pod_parser *sl_pod_parse_begin(const struct sl_pod_feed_cb *cb)
{
    struct sl_pod_parser *p = calloc(1, sizeof *p);
    if (p) { p->cb = *cb; p->en = -1; }
    return p;
}
void sl_pod_parse_feed(struct sl_pod_parser *p, const char *data, size_t n)
{
    for (size_t i = 0; p && i < n; i++)
    {
        char c = data[i];
        switch (p->state)
        {
            case PX_TEXT:
                if (c == '<') { p->state = PX_TAG; p->tn = 0; p->en = -1; break; }
                if (p->en >= 0)
                {
                    if (c == ';')
                    {
                        char u[8];
                        p->ent[p->en] = '\0';
                        size_t k = px_entity(p->ent, u);
                        if (k) px_text_add(p, u, k);
                        else { px_text_add(p, "&", 1); px_text_add(p, p->ent, (size_t)p->en); px_text_add(p, ";", 1); }
                        p->en = -1;
                    }
                    else if (p->en < (int)sizeof p->ent - 1 && (isalnum((unsigned char)c) || c == '#')) p->ent[p->en++] = c;
                    else { px_text_add(p, "&", 1); px_text_add(p, p->ent, (size_t)p->en); p->en = -1; px_text_add(p, &c, 1); }
                    break;
                }
                if (c == '&') { p->en = 0; break; }
                px_text_add(p, &c, 1);
                break;
            case PX_TAG:
                if (c == '>')
                {
                    p->state = PX_TEXT;
                    if (p->tn && p->tag[0] != '!' && p->tag[0] != '?') px_tag(p);
                    break;
                }
                if (p->tn + 1 < sizeof p->tag) p->tag[p->tn++] = c;
                /* comments, CDATA and the head's instructions have ends of their own */
                if (p->tn == 3 && !memcmp(p->tag, "!--", 3)) { p->state = PX_COMMENT; snprintf(p->match, sizeof p->match, "-->"); p->mn = 0; }
                else if (p->tn == 8 && !memcmp(p->tag, "![CDATA[", 8)) { p->state = PX_CDATA; snprintf(p->match, sizeof p->match, "]]>"); p->mn = 0; }
                else if (p->tn == 1 && c == '?') { p->state = PX_PI; snprintf(p->match, sizeof p->match, "?>"); p->mn = 0; }
                break;
            default:
            {
                size_t ml = strlen(p->match);
                if (c == p->match[p->mn]) { if (++p->mn == ml) { p->state = PX_TEXT; p->mn = 0; } break; }
                /* what was taken for the end was not: it is text (CDATA keeps it) */
                if (p->state == PX_CDATA && p->mn) px_text_add(p, p->match, p->mn);
                p->mn = c == p->match[0] ? 1 : 0;
                if (!p->mn && p->state == PX_CDATA) px_text_add(p, &c, 1);
                break;
            }
        }
    }
}
bool sl_pod_parse_end(struct sl_pod_parser *p)
{
    if (!p) return false;
    bool feed = p->feed;
    if (feed) px_show(p);
    free(p);
    return feed;
}

/* ---------------------------------------------------------------- the shows */
#define PJ_SHOWS SL_POD_STATE "/shows.tsv"
#define PJ_QUEUE SL_POD_STATE "/queue.tsv"
#define PJ_PROGRESS SL_POD_STATE "/progress.tsv"
static void pj_eps_name(char *out, size_t n, uint32_t id) { snprintf(out, n, SL_POD_STATE "/%08x.eps", (unsigned)id); }
void sl_pod_art_name(char *out, size_t n, uint32_t id, int px) { snprintf(out, n, SL_POD_STATE "/art/%08x-%d.bin", (unsigned)id, px); }
uint32_t sl_pod_feed_id(const char *feed)
{
    /* the same feed by http or https, with or without a slash at the end, is one show */
    char b[400];
    const char *s = feed ? feed : "";
    if (!strncasecmp(s, "https://", 8)) s += 8;
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    size_t k = 0;
    for (; *s && k + 1 < sizeof b; s++) b[k++] = (char)tolower((unsigned char)*s);
    while (k && b[k - 1] == '/') k--;
    b[k] = '\0';
    return pj_fnv(b);
}
/* room for one more show; false when there is none */
static bool pj_show_room(void)
{
    if (pj.n_shows < pj.cap_shows) return true;
    if (pj.cap_shows >= SL_POD_MAX_SHOWS) return false;
    struct sl_pod_show *s = realloc(pj.shows, (size_t)(pj.cap_shows + 8) * sizeof *s);
    if (!s) return false;
    pj.shows = s;
    pj.cap_shows += 8;
    return true;
}
static void pj_shows_load(void)
{
    if (pj.shows_read) return;
    pj.shows_read = true;
    free(pj.shows);
    pj.shows = NULL;
    pj.n_shows = pj.cap_shows = 0;
    char *b = pj_slurp(PJ_SHOWS, 4 * 1024 * 1024, NULL);
    if (!b || strncmp(b, "PODS1\n", 6)) { free(b); return; }
    for (char *line = b + 6, *nl; line && *line && pj_show_room(); line = nl)
    {
        nl = strchr(line, '\n');
        if (nl) *nl++ = '\0';
        struct sl_pod_show *s = &pj.shows[pj.n_shows];
        memset(s, 0, sizeof *s);
        char *f = line;
        s->id = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        s->added = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        s->refreshed = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        s->newest = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        s->speed = (uint16_t)atoi(pj_next(&f));
        s->n_eps = (uint16_t)atoi(pj_next(&f));
        s->n_unheard = (uint16_t)atoi(pj_next(&f));
        s->n_dl = (uint16_t)atoi(pj_next(&f));
        s->art = atoi(pj_next(&f)) != 0;
        s->tint = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        snprintf(s->folder, sizeof s->folder, "%s", pj_next(&f));
        snprintf(s->feed, sizeof s->feed, "%s", pj_next(&f));
        snprintf(s->image, sizeof s->image, "%s", pj_next(&f));
        snprintf(s->title, sizeof s->title, "%s", pj_next(&f));
        snprintf(s->author, sizeof s->author, "%s", pj_next(&f));
        if (s->id && s->feed[0] && s->folder[0]) pj.n_shows++;
    }
    free(b);
}
static bool pj_shows_save(void)
{
    if (pj.hold) return false;
    pj_mkdirs(SL_POD_STATE);
    struct pj_buf o = { 0 };
    char a[4][400], c[400];
    pj_add(&o, "PODS1\n");
    for (int i = 0; i < pj.n_shows; i++)
    {
        const struct sl_pod_show *s = &pj.shows[i];
        pj_add(&o, "%08x\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%06x\t%s\t%s\t%s\t%s\t%s\n", (unsigned)s->id, (unsigned)s->added,
               (unsigned)s->refreshed, (unsigned)s->newest, (unsigned)s->speed, (unsigned)s->n_eps, (unsigned)s->n_unheard,
               (unsigned)s->n_dl, s->art ? 1 : 0, (unsigned)s->tint, pj_f(s->folder, a[0], sizeof a[0]), pj_f(s->feed, a[1], sizeof a[1]),
               pj_f(s->image, a[2], sizeof a[2]), pj_f(s->title, a[3], sizeof a[3]), pj_f(s->author, c, sizeof c));
    }
    bool ok = !o.bad && pj_write(PJ_SHOWS, o.b, o.n);
    free(o.b);
    pj.st.shows_seq++;
    pj.st.seq++;
    return ok;
}
static struct sl_pod_show *pj_show(uint32_t id)
{
    pj_shows_load();
    for (int i = 0; i < pj.n_shows; i++) if (pj.shows[i].id == id) return &pj.shows[i];
    return NULL;
}
/* the newest first, as the grid shows them */
static int pj_show_cmp(const void *a, const void *b)
{
    const struct sl_pod_show *x = a, *y = b;
    uint32_t tx = x->newest ? x->newest : x->added, ty = y->newest ? y->newest : y->added;
    return tx < ty ? 1 : tx > ty ? -1 : strcasecmp(x->title, y->title);
}
struct sl_pod_show *sl_pod_shows_copy(int *n)
{
    pthread_mutex_lock(&pj_lock);
    pj_shows_load();
    *n = pj.n_shows;
    struct sl_pod_show *out = malloc(((size_t)pj.n_shows + 1) * sizeof *out);
    if (out && pj.n_shows) memcpy(out, pj.shows, (size_t)pj.n_shows * sizeof *out);
    if (!out) *n = 0;
    pthread_mutex_unlock(&pj_lock);
    if (out && *n > 1) qsort(out, (size_t)*n, sizeof *out, pj_show_cmp);
    return out;
}
int sl_pod_show_count(void)
{
    pthread_mutex_lock(&pj_lock);
    pj_shows_load();
    int n = pj.n_shows;
    pthread_mutex_unlock(&pj_lock);
    return n;
}
int sl_pod_shows(struct sl_pod_show *out, int max)
{
    pthread_mutex_lock(&pj_lock);
    pj_shows_load();
    int n = pj.n_shows < max ? pj.n_shows : max;
    if (out && n) memcpy(out, pj.shows, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&pj_lock);
    if (out && n > 1) qsort(out, (size_t)n, sizeof *out, pj_show_cmp);
    return n;
}
bool sl_pod_show(uint32_t id, struct sl_pod_show *out)
{
    pthread_mutex_lock(&pj_lock);
    struct sl_pod_show *s = pj_show(id);
    if (s && out) *out = *s;
    pthread_mutex_unlock(&pj_lock);
    return s != NULL;
}
bool sl_pod_following(const char *feed)
{
    uint32_t id = sl_pod_feed_id(feed);
    return sl_pod_show(id, NULL);
}
/* a folder for a new show: its name, made unique among the others */
static void pj_new_folder(struct sl_pod_show *s, const char *title)
{
    char base[80];
    sl_pod_clean_name(base, sizeof base, title && title[0] ? title : "Podcast");
    for (int k = 1; k < 100; k++)
    {
        if (k == 1) snprintf(s->folder, sizeof s->folder, "%s", base);
        else snprintf(s->folder, sizeof s->folder, "%s (%d)", base, k);
        bool taken = false;
        for (int i = 0; i < pj.n_shows && !taken; i++) taken = &pj.shows[i] != s && !strcasecmp(pj.shows[i].folder, s->folder);
        if (!taken) return;
    }
}

/* ---------------------------------------------------------------- episodes */
static int pj_ep_cmp(const void *a, const void *b)
{
    const struct sl_pod_ep *x = a, *y = b;
    return x->date < y->date ? 1 : x->date > y->date ? -1 : 0;
}
/* a show's file, parsed in place: the file's text is the pool */
static bool pj_eps_read(uint32_t show, struct sl_pod_eps *e)
{
    memset(e, 0, sizeof *e);
    e->show = show;
    char name[96];
    pj_eps_name(name, sizeof name, show);
    char *b = pj_slurp(name, 8 * 1024 * 1024, NULL);
    if (!b) return false;
    if (strncmp(b, "PODE1\n", 6)) { free(b); return false; }
    int lines = 0;
    for (char *s = b + 6; *s; s++) if (*s == '\n') lines++;
    e->ep = calloc((size_t)lines + 1, sizeof *e->ep);
    if (!e->ep) { free(b); return false; }
    e->pool = b;
    for (char *line = b + 6, *nl; line && *line; line = nl)
    {
        nl = strchr(line, '\n');
        if (nl) *nl++ = '\0';
        struct sl_pod_ep *p = &e->ep[e->n];
        char *f = line;
        p->key = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        p->date = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        p->dur = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        p->bytes = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        p->flags = (uint8_t)atoi(pj_next(&f));
        p->file = pj_next(&f);
        p->url = pj_next(&f);
        p->title = pj_next(&f);
        p->dl = p->file[0] ? SL_POD_DL_DONE : SL_POD_DL_NONE;
        if (p->key && p->url[0]) e->n++;
    }
    return true;
}
static bool pj_eps_write(uint32_t show, const struct sl_pod_eps *e)
{
    if (pj.hold) return false;
    pj_mkdirs(SL_POD_STATE);
    struct pj_buf o = { 0 };
    char a[520], b[520], c[520];
    pj_add(&o, "PODE1\n");
    for (int i = 0; i < e->n; i++)
    {
        const struct sl_pod_ep *p = &e->ep[i];
        pj_add(&o, "%08x\t%u\t%u\t%u\t%u\t%s\t%s\t%s\n", (unsigned)p->key, (unsigned)p->date, (unsigned)p->dur,
               (unsigned)p->bytes, (unsigned)p->flags, pj_f(p->file, a, sizeof a), pj_f(p->url, b, sizeof b),
               pj_f(p->title, c, sizeof c));
    }
    char name[96];
    pj_eps_name(name, sizeof name, show);
    bool ok = !o.bad && pj_write(name, o.b, o.n);
    free(o.b);
    /* the show's counts, as the grid and its page say them */
    struct sl_pod_show *s = pj_show(show);
    if (s && ok)
    {
        s->n_eps = (uint16_t)e->n;
        s->n_unheard = s->n_dl = 0;
        s->newest = 0;
        for (int i = 0; i < e->n; i++)
        {
            if (!(e->ep[i].flags & SL_POD_HEARD)) s->n_unheard++;
            if (e->ep[i].file[0]) s->n_dl++;
            if (e->ep[i].date > s->newest) s->newest = e->ep[i].date;
        }
        pj_shows_save();
    }
    pj.st.eps_seq++;
    pj.st.eps_show = show;
    pj.st.seq++;
    return ok;
}
void sl_pod_eps_free(struct sl_pod_eps *e)
{
    if (!e) return;
    free(e->ep);
    free(e->pool);
    memset(e, 0, sizeof *e);
}
static void pj_queue_load(void);
bool sl_pod_eps_load(uint32_t show, struct sl_pod_eps *out)
{
    pthread_mutex_lock(&pj_lock);
    bool ok = pj_eps_read(show, out);
    if (ok) qsort(out->ep, (size_t)out->n, sizeof *out->ep, pj_ep_cmp);
    pj_queue_load();
    for (int i = 0; ok && i < out->n; i++)
    {
        if (out->ep[i].dl == SL_POD_DL_DONE) continue;
        for (int k = 0; k < pj.n_q; k++)
            if (pj.q[k].show == show && pj.q[k].key == out->ep[i].key)
                out->ep[i].dl = k == 0 && pj.dl_on ? SL_POD_DL_ACTIVE : SL_POD_DL_QUEUED;
    }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
static struct sl_pod_ep *pj_find_ep(struct sl_pod_eps *e, uint32_t key)
{
    for (int i = 0; i < e->n; i++) if (e->ep[i].key == key) return &e->ep[i];
    return NULL;
}
bool sl_pod_mark(uint32_t show, const uint32_t *keys, int n, bool heard)
{
    pthread_mutex_lock(&pj_lock);
    struct sl_pod_eps e;
    bool ok = !pj.hold && pj_eps_read(show, &e);
    if (ok)
    {
        for (int i = 0; i < n; i++)
        {
            struct sl_pod_ep *p = pj_find_ep(&e, keys[i]);
            if (p) p->flags = heard ? (uint8_t)(p->flags | SL_POD_HEARD) : (uint8_t)(p->flags & ~SL_POD_HEARD);
        }
        ok = pj_eps_write(show, &e);
        sl_pod_eps_free(&e);
    }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
bool sl_pod_ep_path(uint32_t show, const struct sl_pod_ep *ep, char *out, size_t n)
{
    out[0] = '\0';
    if (!ep || !ep->file || !ep->file[0]) return false;
    struct sl_pod_show s;
    if (!sl_pod_show(show, &s)) return false;
    return snprintf(out, n, "%s/%s/%s", sl_pod_folder, s.folder, ep->file) < (int)n;
}
bool sl_pod_which(const char *path, uint32_t *show, uint32_t *key, char *title, size_t tn)
{
    static char last[400], last_title[160];
    static uint32_t last_show, last_key;
    if (!sl_pod_is_episode(path)) return false;
    if (!strcmp(path, last))
    {
        *show = last_show; *key = last_key;
        if (title && tn) snprintf(title, tn, "%s", last_title);
        return last_show != 0;
    }
    snprintf(last, sizeof last, "%s", path);
    last_show = last_key = 0;
    last_title[0] = '\0';
    const char *rest = path + strlen(sl_pod_folder) + 1, *slash = strchr(rest, '/');
    if (!slash) return false;
    char folder[128];
    snprintf(folder, sizeof folder, "%.*s", (int)(slash - rest), rest);
    uint32_t id = 0;
    pthread_mutex_lock(&pj_lock);
    pj_shows_load();
    for (int i = 0; i < pj.n_shows && !id; i++) if (!strcasecmp(pj.shows[i].folder, folder)) id = pj.shows[i].id;
    struct sl_pod_eps e;
    if (id && pj_eps_read(id, &e))
    {
        for (int i = 0; i < e.n; i++)
            if (!strcasecmp(e.ep[i].file, slash + 1))
            {
                last_show = id; last_key = e.ep[i].key;
                snprintf(last_title, sizeof last_title, "%s", e.ep[i].title);
                break;
            }
        sl_pod_eps_free(&e);
    }
    pthread_mutex_unlock(&pj_lock);
    *show = last_show;
    *key = last_key;
    if (title && tn) snprintf(title, tn, "%s", last_title);
    return last_show != 0;
}

/* ---------------------------------------------------------------- listening */
static void pj_prog_load(void)
{
    if (pj.prog_read) return;
    pj.prog_read = true;
    pj.prog = calloc(SL_POD_PROGRESS, sizeof *pj.prog);
    pj.n_prog = 0;
    char *b = pj.prog ? pj_slurp(PJ_PROGRESS, 512 * 1024, NULL) : NULL;
    if (b && !strncmp(b, "PODP1\n", 6))
        for (char *line = b + 6, *nl; line && *line && pj.n_prog < SL_POD_PROGRESS; line = nl)
        {
            nl = strchr(line, '\n');
            if (nl) *nl++ = '\0';
            struct sl_pod_progress *p = &pj.prog[pj.n_prog];
            char *f = line;
            p->show = (uint32_t)strtoul(pj_next(&f), NULL, 16);
            p->key = (uint32_t)strtoul(pj_next(&f), NULL, 16);
            p->pos_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            p->dur_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            p->when = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            snprintf(p->path, sizeof p->path, "%s", pj_next(&f));
            snprintf(p->title, sizeof p->title, "%s", pj_next(&f));
            if (p->show && p->key) pj.n_prog++;
        }
    free(b);
}
static void pj_prog_save(void)
{
    if (!pj.prog_dirty || pj.hold) return;
    pj_mkdirs(SL_POD_STATE);
    struct pj_buf o = { 0 };
    char a[320], b[200];
    pj_add(&o, "PODP1\n");
    for (int i = 0; i < pj.n_prog; i++)
    {
        const struct sl_pod_progress *p = &pj.prog[i];
        pj_add(&o, "%08x\t%08x\t%u\t%u\t%u\t%s\t%s\n", (unsigned)p->show, (unsigned)p->key, (unsigned)p->pos_ms,
               (unsigned)p->dur_ms, (unsigned)p->when, pj_f(p->path, a, sizeof a), pj_f(p->title, b, sizeof b));
    }
    if (!o.bad && pj_write(PJ_PROGRESS, o.b, o.n)) pj.prog_dirty = false;
    free(o.b);
}
static int pj_prog_find(uint32_t show, uint32_t key)
{
    for (int i = 0; i < pj.n_prog; i++) if (pj.prog[i].show == show && pj.prog[i].key == key) return i;
    return -1;
}
void sl_pod_progress_set(uint32_t show, uint32_t key, uint32_t pos_ms, uint32_t dur_ms, const char *path, const char *title)
{
    pthread_mutex_lock(&pj_lock);
    pj_prog_load();
    int i = pj_prog_find(show, key);
    if (pj.prog && i != 0)
    {
        /* to the front: the latest first; the oldest goes when it is full */
        struct sl_pod_progress p;
        if (i > 0) p = pj.prog[i];
        else { memset(&p, 0, sizeof p); if (pj.n_prog == SL_POD_PROGRESS) pj.n_prog--; i = pj.n_prog++; }
        memmove(&pj.prog[1], &pj.prog[0], (size_t)i * sizeof p);
        pj.prog[0] = p;
    }
    if (pj.prog)
    {
        struct sl_pod_progress *p = &pj.prog[0];
        bool moved = p->pos_ms / 1000 != pos_ms / 1000 || p->show != show;
        p->show = show; p->key = key; p->pos_ms = pos_ms; p->when = pj_now();
        if (dur_ms) p->dur_ms = dur_ms;
        if (path) snprintf(p->path, sizeof p->path, "%s", path);
        if (title) snprintf(p->title, sizeof p->title, "%s", title);
        if (moved) pj.prog_dirty = true;
    }
    pthread_mutex_unlock(&pj_lock);
}
bool sl_pod_progress_get(uint32_t show, uint32_t key, struct sl_pod_progress *out)
{
    pthread_mutex_lock(&pj_lock);
    pj_prog_load();
    int i = pj_prog_find(show, key);
    if (i >= 0 && out) *out = pj.prog[i];
    pthread_mutex_unlock(&pj_lock);
    return i >= 0;
}
int sl_pod_progress_list(struct sl_pod_progress *out, int max)
{
    pthread_mutex_lock(&pj_lock);
    pj_prog_load();
    int n = 0;
    for (int i = 0; i < pj.n_prog && n < max; i++) if (pj_show(pj.prog[i].show)) out[n++] = pj.prog[i];
    pthread_mutex_unlock(&pj_lock);
    return n;
}
void sl_pod_progress_flush(void)
{
    pthread_mutex_lock(&pj_lock);
    pj_prog_save();
    pthread_mutex_unlock(&pj_lock);
}
void sl_pod_finished(uint32_t show, uint32_t key)
{
    pthread_mutex_lock(&pj_lock);
    pj_prog_load();
    int i = pj_prog_find(show, key);
    if (i >= 0)
    {
        memmove(&pj.prog[i], &pj.prog[i + 1], (size_t)(pj.n_prog - i - 1) * sizeof *pj.prog);
        pj.n_prog--;
        pj.prog_dirty = true;
        pj_prog_save();
    }
    pthread_mutex_unlock(&pj_lock);
    sl_pod_mark(show, &key, 1, true);
}

/* ---------------------------------------------------------------- the queue */
static void pj_q_free(struct pj_dlq *q) { free(q->url); free(q->dest); free(q->title); memset(q, 0, sizeof *q); }
static bool pj_q_room(void)
{
    if (pj.n_q < pj.cap_q) return true;
    if (pj.cap_q >= PJ_QUEUE_MAX) return false;
    struct pj_dlq *q = realloc(pj.q, (size_t)(pj.cap_q + 16) * sizeof *q);
    if (!q) return false;
    pj.q = q;
    pj.cap_q += 16;
    return true;
}
static void pj_queue_load(void)
{
    if (pj.q_read) return;
    pj.q_read = true;
    free(pj.q);
    pj.q = NULL;
    pj.n_q = pj.cap_q = 0;
    char *b = pj_slurp(PJ_QUEUE, 2 * 1024 * 1024, NULL);
    if (b && !strncmp(b, "PODQ1\n", 6))
        for (char *line = b + 6, *nl; line && *line && pj_q_room(); line = nl)
        {
            nl = strchr(line, '\n');
            if (nl) *nl++ = '\0';
            struct pj_dlq *q = &pj.q[pj.n_q];
            memset(q, 0, sizeof *q);
            char *f = line;
            q->show = (uint32_t)strtoul(pj_next(&f), NULL, 16);
            q->key = (uint32_t)strtoul(pj_next(&f), NULL, 16);
            q->bytes = atoll(pj_next(&f));
            q->url = strdup(pj_next(&f));
            q->dest = strdup(pj_next(&f));
            q->title = strdup(pj_next(&f));
            if (q->show && q->url && q->dest && q->title && q->url[0] && q->dest[0]) pj.n_q++;
            else pj_q_free(q);
        }
    free(b);
    pj.st.queued = pj.n_q;
}
static void pj_queue_save(void)
{
    if (pj.hold) return;
    char p[512];
    if (!pj.n_q) { if (pj_real(p, sizeof p, PJ_QUEUE)) unlink(p); return; }
    pj_mkdirs(SL_POD_STATE);
    struct pj_buf o = { 0 };
    char a[520], b[520], c[200];
    pj_add(&o, "PODQ1\n");
    for (int i = 0; i < pj.n_q; i++)
        pj_add(&o, "%08x\t%08x\t%lld\t%s\t%s\t%s\n", (unsigned)pj.q[i].show, (unsigned)pj.q[i].key, pj.q[i].bytes,
               pj_f(pj.q[i].url, a, sizeof a), pj_f(pj.q[i].dest, b, sizeof b), pj_f(pj.q[i].title, c, sizeof c));
    if (!o.bad) pj_write(PJ_QUEUE, o.b, o.n);
    free(o.b);
}
static void pj_wake(void);
int sl_pod_download(uint32_t show, const uint32_t *keys, int n)
{
    int added = 0;
    pthread_mutex_lock(&pj_lock);
    pj_queue_load();
    struct sl_pod_show *s = pj_show(show);
    struct sl_pod_eps e;
    if (s && pj_eps_read(show, &e))
    {
        for (int i = 0; i < n; i++)
        {
            struct sl_pod_ep *p = pj_find_ep(&e, keys[i]);
            bool there = !p || p->file[0];
            for (int k = 0; k < pj.n_q && !there; k++) there = pj.q[k].show == show && pj.q[k].key == keys[i];
            if (there) continue;
            if (!pj_q_room()) break;
            char name[160], dest[300];
            sl_pod_file_name(name, sizeof name, p->date, p->title, p->url);
            snprintf(dest, sizeof dest, "%s/%s", s->folder, name);
            struct pj_dlq *q = &pj.q[pj.n_q];
            q->show = show; q->key = p->key; q->bytes = p->bytes;
            q->url = strdup(p->url); q->dest = strdup(dest); q->title = strdup(p->title);
            if (!q->url || !q->dest || !q->title) { pj_q_free(q); break; }
            pj.n_q++;
            added++;
        }
        sl_pod_eps_free(&e);
    }
    if (added) { pj_queue_save(); pj.st.queued = pj.n_q - (pj.dl_on ? 1 : 0); pj.st.seq++; pj.dl_fails = 0; pj.dl_retry_at = 0; }
    pthread_mutex_unlock(&pj_lock);
    if (added) pj_wake();
    return added;
}
/* the part a download writes, beside where it goes */
static bool pj_dest_real(const char *dest, char *out, size_t n, bool part)
{
    char rb[400];
    return snprintf(rb, sizeof rb, "%s/%s%s", sl_pod_folder, dest, part ? ".part" : "") < (int)sizeof rb && pj_real(out, n, rb);
}
static void pj_dl_end(bool keep_part);
bool sl_pod_cancel(uint32_t show, uint32_t key)
{
    bool found = false;
    pthread_mutex_lock(&pj_lock);
    pj_queue_load();
    for (int k = 0; k < pj.n_q && !found; k++)
    {
        if (pj.q[k].show != show || pj.q[k].key != key) continue;
        found = true;
        if (k == 0 && pj.dl_on) pj_dl_end(true);
        char part[512];
        if (!pj.hold && pj_dest_real(pj.q[k].dest, part, sizeof part, true)) unlink(part);
        pj_q_free(&pj.q[k]);
        memmove(&pj.q[k], &pj.q[k + 1], (size_t)(pj.n_q - k - 1) * sizeof *pj.q);
        pj.n_q--;
    }
    if (found)
    {
        pj_queue_save();
        pj.st.queued = pj.n_q - (pj.dl_on ? 1 : 0);
        pj.st.eps_seq++;
        pj.st.eps_show = show;
        pj.st.seq++;
    }
    pthread_mutex_unlock(&pj_lock);
    return found;
}
/* the download coming down stops (its part kept, or not) */
static void pj_dl_end(bool keep_part)
{
    if (!pj.dl_on) return;
    sl_net_dl_stop(&pj.dl);
    if (!keep_part) unlink(pj.dl.path);
    pj.dl_on = false;
    pj.st.dl_show = pj.st.dl_key = 0;
    pj.st.dl_got = pj.st.dl_total = 0;
    pj.st.queued = pj.n_q;                /* the one stopped waits again */
    pj.st.seq++;
}

/* ---------------------------------------------------------------- reading a feed */
struct pj_item { uint32_t key, date, dur; long long bytes; char *title, *url; };
struct pj_read {
    char title[160], author[96], image[384];
    struct pj_item *it;
    int n, cap;
    bool shown;
};
static void pj_read_show(void *ctx, const char *title, const char *author, const char *image)
{
    struct pj_read *r = ctx;
    r->shown = true;
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->author, sizeof r->author, "%s", author);
    snprintf(r->image, sizeof r->image, "%s", image);
}
static int pj_item_cmp(const void *a, const void *b)
{
    const struct pj_item *x = a, *y = b;
    return x->date < y->date ? 1 : x->date > y->date ? -1 : 0;
}
/* the newest SL_POD_MAX_EPS: when the list fills it is sorted and cut */
static void pj_read_trim(struct pj_read *r)
{
    qsort(r->it, (size_t)r->n, sizeof *r->it, pj_item_cmp);
    while (r->n > SL_POD_MAX_EPS) { r->n--; free(r->it[r->n].title); free(r->it[r->n].url); }
}
static void pj_read_item(void *ctx, const char *title, const char *guid, const char *url, const char *date, const char *dur, long long bytes)
{
    struct pj_read *r = ctx;
    if (!r->it) { r->cap = PJ_PARSE_KEEP; r->it = calloc((size_t)r->cap, sizeof *r->it); if (!r->it) return; }
    uint32_t key = pj_fnv(guid && guid[0] ? guid : url);
    for (int i = r->n - 1; i >= 0 && i >= r->n - 8; i--) if (r->it[i].key == key) return;   /* a feed that says one twice */
    if (r->n == r->cap) pj_read_trim(r);
    struct pj_item *it = &r->it[r->n];
    it->key = key;
    it->date = sl_pod_rfc822(date);
    it->dur = sl_pod_duration(dur);
    it->bytes = bytes > 0 ? bytes : 0;
    it->title = strdup(title && title[0] ? title : "Episode");
    it->url = strdup(url);
    if (it->title && it->url) r->n++;
    else { free(it->title); free(it->url); }
}
static void pj_read_free(struct pj_read *r)
{
    for (int i = 0; i < r->n; i++) { free(r->it[i].title); free(r->it[i].url); }
    free(r->it);
    memset(r, 0, sizeof *r);
}
static bool pj_parse_sink(void *ctx, const char *data, size_t n)
{
    sl_pod_parse_feed(ctx, data, n);
    return true;
}
/* the feed at url, read as it comes: 1 a feed, 0 not one, -1 no answer */
static int pj_fetch_feed(const char *url, struct pj_read *r)
{
    memset(r, 0, sizeof *r);
    struct sl_pod_feed_cb cb = { pj_read_show, pj_read_item, r };
    struct sl_pod_parser *p = sl_pod_parse_begin(&cb);
    if (!p) return -1;
    int status = sl_net_http_stream(url, SL_POD_UA, 30000, pj_parse_sink, p, &pj.stop_job);
    bool feed = sl_pod_parse_end(p);
    if (r->n) pj_read_trim(r);
    if (status != 200) { pj_read_free(r); return -1; }
    return feed && (r->n || r->shown) ? 1 : 0;
}
/* the feed's episodes, with what was known of each kept: heard, and the
 * file downloaded. Episodes gone from the feed stay while they are on the
 * card. Returns how many are new; the show's file is written. Locked. */
static int pj_merge(uint32_t show, struct pj_read *r, bool first)
{
    struct sl_pod_eps old, e;
    bool had = pj_eps_read(show, &old);
    memset(&e, 0, sizeof e);
    e.show = show;
    int extra = 0;
    for (int i = 0; had && i < old.n; i++) if (old.ep[i].file[0]) extra++;
    e.ep = calloc((size_t)(r->n + extra) + 1, sizeof *e.ep);
    if (!e.ep) { if (had) sl_pod_eps_free(&old); return -1; }
    int fresh = 0;
    for (int i = 0; i < r->n; i++)
    {
        struct sl_pod_ep *p = &e.ep[e.n++];
        p->key = r->it[i].key; p->date = r->it[i].date; p->dur = r->it[i].dur;
        p->bytes = r->it[i].bytes > 0xffffffffLL ? 0xffffffffu : (uint32_t)r->it[i].bytes;
        p->title = r->it[i].title; p->url = r->it[i].url; p->file = (char *)"";
        struct sl_pod_ep *o = had ? pj_find_ep(&old, p->key) : NULL;
        if (o) { p->flags = o->flags; p->file = o->file; if (!p->dur) p->dur = o->dur; }
        else
        {
            fresh++;
            /* following a show with years of episodes: the old ones are not news */
            if (first && i >= 3) p->flags |= SL_POD_HEARD;
        }
    }
    for (int i = 0; had && i < old.n; i++)
        if (old.ep[i].file[0] && !pj_find_ep(&e, old.ep[i].key)) e.ep[e.n++] = old.ep[i];
    bool ok = pj_eps_write(show, &e);
    free(e.ep);
    if (had) sl_pod_eps_free(&old);
    return ok ? fresh : -1;
}

/* ---------------------------------------------------------------- art */
/* an LVGL RGB565 square, area-averaged from the middle of rgb */
/* an RGB picture as a px square (its middle), written to name in dir; its mean colour in tint */
static bool pj_art_square(const char *name, const char *dir, const unsigned char *rgb, int w, int h, int px, uint32_t *tint)
{
    uint64_t sr = 0, sg = 0, sb = 0;
    int s = w < h ? w : h, ox = (w - s) / 2, oy = (h - s) / 2;
    int out = px < s ? px : s;
    uint16_t *dst = malloc((size_t)out * out * 2 + 12);
    if (!dst) return false;
    for (int y = 0; y < out; y++)
    {
        int y0 = oy + y * s / out, y1 = oy + (y + 1) * s / out;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < out; x++)
        {
            int x0 = ox + x * s / out, x1 = ox + (x + 1) * s / out;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                const unsigned char *q = rgb + ((size_t)yy * w + x0) * 3;
                for (int xx = x0; xx < x1; xx++, q += 3) { r += q[0]; g += q[1]; b += q[2]; n++; }
            }
            r /= n; g /= n; b /= n;
            sr += r; sg += g; sb += b;
            dst[6 + y * out + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    /* LVGL 9's image head: magic, colour format (RGB565 = 0x12), flags, w, h, stride, reserved */
    uint8_t *hd = (uint8_t *)dst;
    memset(hd, 0, 12);
    hd[0] = 0x19; hd[1] = 0x12;
    hd[4] = (uint8_t)out; hd[5] = (uint8_t)(out >> 8);
    hd[6] = (uint8_t)out; hd[7] = (uint8_t)(out >> 8);
    hd[8] = (uint8_t)(out * 2); hd[9] = (uint8_t)((out * 2) >> 8);
    pj_mkdirs(dir);
    bool ok = pj_write(name, (const char *)dst, (size_t)out * out * 2 + 12);
    free(dst);
    if (tint)
    {
        uint64_t k = (uint64_t)out * out;
        *tint = (uint32_t)((sr / k) << 16 | (sg / k) << 8 | (sb / k));
        if (!*tint) *tint = 0x010101;     /* black art is still art: 0 means none */
    }
    return ok;
}
static bool pj_art_write(uint32_t id, const unsigned char *rgb, int w, int h, int px, uint32_t *tint)
{
    char name[96];
    sl_pod_art_name(name, sizeof name, id, px);
    return pj_art_square(name, SL_POD_STATE "/art", rgb, w, h, px, tint);
}
struct pj_mem { unsigned char *b; size_t n, cap; };
static bool pj_mem_sink(void *ctx, const char *data, size_t n)
{
    struct pj_mem *m = ctx;
    if (m->n + n > m->cap) return false;  /* too big a picture: not for us */
    memcpy(m->b + m->n, data, n);
    m->n += n;
    return true;
}
/* the directory's pictures come at any size asked: 300 is enough for 219 */
static void pj_art_smaller(char *url)
{
    char *s = strstr(url, "600x600");
    if (!s) s = strstr(url, "100x100");
    if (s) memcpy(s, "300x300", 7);
}
static bool pj_art_from(uint32_t id, const char *url)
{
    if (!url || !url[0]) return false;
    struct pj_mem m = { malloc(PJ_ART_CAP), 0, PJ_ART_CAP };
    if (!m.b) return false;
    int status = sl_net_http_stream(url, SL_POD_UA, 20000, pj_mem_sink, &m, &pj.stop_job);
    int w = 0, h = 0;
    unsigned char *rgb = status == 200 && m.n ? sl_art_rgb_load(m.b, (int)m.n, &w, &h) : NULL;
    free(m.b);
    bool ok = false;
    if (rgb)
    {
        pthread_mutex_lock(&pj_lock);
        uint32_t tint = 0;
        ok = !pj.hold && pj_art_write(id, rgb, w, h, SL_POD_ART_BIG, &tint) && pj_art_write(id, rgb, w, h, SL_POD_ART_ROW, NULL);
        struct sl_pod_show *s = pj_show(id);
        if (ok && s) { s->art = true; s->tint = tint; pj_shows_save(); }
        pthread_mutex_unlock(&pj_lock);
        sl_art_rgb_free(rgb);
    }
    return ok;
}
static int pj_search_parse(const char *json, struct sl_pod_found *out, int max);
/* a show's picture: the feed's own, or (too big to open here, or none) the
 * directory's copy of it, found by the show's name and matched by its feed */
static void pj_art(uint32_t id)
{
    struct sl_pod_show s;
    if (!sl_pod_show(id, &s) || s.art) return;
    char url[400];
    snprintf(url, sizeof url, "%s", s.image);
    pj_art_smaller(url);
    if (pj_art_from(id, url)) return;
    char q[400], u[600];
    lj_enc(q, sizeof q, s.title);
    snprintf(u, sizeof u, PJ_ITUNES "%s", q);
    char *json = malloc(PJ_SEARCH_CAP);
    struct pj_mem m = { (unsigned char *)json, 0, PJ_SEARCH_CAP - 1 };
    struct sl_pod_found *f = calloc(SL_POD_MAX_FOUND, sizeof *f);
    if (json && f && sl_net_http_stream(u, SL_POD_UA, 20000, pj_mem_sink, &m, &pj.stop_job) == 200)
    {
        json[m.n] = '\0';
        int n = pj_search_parse(json, f, SL_POD_MAX_FOUND);
        for (int i = 0; i < n; i++)
            if (sl_pod_feed_id(f[i].feed) == id && f[i].art[0])
            {
                pj_art_smaller(f[i].art);
                if (pj_art_from(id, f[i].art)) break;
            }
    }
    free(json);
    free(f);
}

/* ---------------------------------------------------------------- the directory */
static bool pj_json_num(const char *json, const char *key, long *out)
{
    char k[48];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *s = strstr(json, k);
    if (!s) return false;
    *out = strtol(s + strlen(k), NULL, 10);
    return true;
}
/* iTunes Search's answer: {"resultCount":n,"results":[{...},{...}]} */
static int pj_search_parse(const char *json, struct sl_pod_found *out, int max)
{
    const char *s = strstr(json, "\"results\"");
    if (!s || !(s = strchr(s, '['))) return -1;
    int n = 0;
    char obj[8192];
    for (s++; *s && n < max; )
    {
        while (*s && *s != '{' && *s != ']') s++;
        if (*s != '{') break;
        /* the object's end, past its strings' braces */
        const char *e = s;
        int depth = 0;
        bool str = false;
        for (; *e; e++)
        {
            if (str) { if (*e == '\\' && e[1]) e++; else if (*e == '"') str = false; continue; }
            if (*e == '"') str = true;
            else if (*e == '{') depth++;
            else if (*e == '}' && --depth == 0) break;
        }
        if (!*e) break;
        size_t len = (size_t)(e - s + 1) < sizeof obj - 1 ? (size_t)(e - s + 1) : sizeof obj - 1;
        memcpy(obj, s, len);
        obj[len] = '\0';
        s = e + 1;
        struct sl_pod_found *f = &out[n];
        memset(f, 0, sizeof *f);
        if (!sl_libjob_json_str(obj, "feedUrl", f->feed, sizeof f->feed)) continue;
        if (!sl_libjob_json_str(obj, "collectionName", f->title, sizeof f->title))
            sl_libjob_json_str(obj, "trackName", f->title, sizeof f->title);
        sl_libjob_json_str(obj, "artistName", f->author, sizeof f->author);
        if (!sl_libjob_json_str(obj, "artworkUrl600", f->art, sizeof f->art))
            sl_libjob_json_str(obj, "artworkUrl100", f->art, sizeof f->art);
        sl_libjob_json_str(obj, "primaryGenreName", f->genre, sizeof f->genre);
        long c = 0;
        if (pj_json_num(obj, "trackCount", &c)) f->count = (int)c;
        if (f->title[0]) n++;
    }
    return n;
}
static void pj_search(const char *term)
{
    char q[300], url[512];
    lj_enc(q, sizeof q, term);
    snprintf(url, sizeof url, PJ_ITUNES "%s", q);
    char *json = malloc(PJ_SEARCH_CAP);
    struct sl_pod_found *f = calloc(SL_POD_MAX_FOUND, sizeof *f);
    struct pj_mem m = { (unsigned char *)json, 0, PJ_SEARCH_CAP - 1 };
    int n = -1;
    if (json && f && sl_net_http_stream(url, SL_POD_UA, 20000, pj_mem_sink, &m, &pj.stop_job) == 200)
    {
        json[m.n] = '\0';
        n = pj_search_parse(json, f, SL_POD_MAX_FOUND);
    }
    free(json);
    pthread_mutex_lock(&pj_lock);
    free(pj.found);
    pj.found = n >= 0 ? f : NULL;
    if (n < 0) free(f);
    pj.n_found = n > 0 ? n : 0;
    pj.st.found_n = n;
    pj.st.found_seq++;
    pj.st.seq++;
    if (n < 0) snprintf(pj.st.why, sizeof pj.st.why, "The podcast directory did not answer");
    pthread_mutex_unlock(&pj_lock);
}
int sl_pod_found_take(struct sl_pod_found *out, int max)
{
    pthread_mutex_lock(&pj_lock);
    int n = pj.n_found < max ? pj.n_found : max;
    if (n && out) memcpy(out, pj.found, (size_t)n * sizeof *out);
    /* taken: the interface has its copy, ours goes */
    free(pj.found);
    pj.found = NULL;
    pj.n_found = 0;
    pthread_mutex_unlock(&pj_lock);
    return n;
}
void sl_pod_preview_forget(void)
{
    pthread_mutex_lock(&pj_lock);
    sl_pod_eps_free(&pj.prev_eps);
    pj.prev_ok = false;
    pthread_mutex_unlock(&pj_lock);
}

/* ---------------------------------------------------------------- the jobs */
static void pj_done(int job, bool ok, int count, const char *title, const char *why)
{
    pthread_mutex_lock(&pj_lock);
    pj.st.done_job = job;
    pj.st.done_ok = ok;
    pj.st.done_count = count;
    snprintf(pj.st.done_title, sizeof pj.st.done_title, "%s", title ? title : "");
    if (why) snprintf(pj.st.why, sizeof pj.st.why, "%s", why);
    pj.st.done_seq++;
    pj.st.seq++;
    pthread_mutex_unlock(&pj_lock);
}
/* a show from a feed read now: added (first), its episodes written */
static int pj_take_feed(uint32_t id, const char *feed, struct pj_read *r, const char *title, const char *author, const char *art, bool *added)
{
    pthread_mutex_lock(&pj_lock);
    *added = false;
    int fresh = -1;
    struct sl_pod_show *s = pj_show(id);
    if (!s && !pj.hold && pj_show_room())
    {
        s = &pj.shows[pj.n_shows++];
        memset(s, 0, sizeof *s);
        s->id = id;
        s->added = pj_now();
        snprintf(s->feed, sizeof s->feed, "%s", feed);
        pj_new_folder(s, r->title[0] ? r->title : title);
        *added = true;
    }
    if (s && !pj.hold)
    {
        /* the feed's own words over the directory's, which only fill gaps */
        if (r->title[0]) snprintf(s->title, sizeof s->title, "%s", r->title);
        else if (!s->title[0] && title) snprintf(s->title, sizeof s->title, "%s", title);
        if (r->author[0]) snprintf(s->author, sizeof s->author, "%s", r->author);
        else if (!s->author[0] && author) snprintf(s->author, sizeof s->author, "%s", author);
        if (art && art[0] && !s->image[0]) snprintf(s->image, sizeof s->image, "%s", art);
        else if (r->image[0] && strcmp(s->image, r->image) && !(art && art[0])) { snprintf(s->image, sizeof s->image, "%s", r->image); }
        if (!s->title[0]) snprintf(s->title, sizeof s->title, "%s", s->folder);
        s->refreshed = pj_now();
        fresh = pj_merge(id, r, *added);
        if (fresh < 0 && *added) pj.n_shows--;   /* nothing could be written: not followed after all */
        else pj_shows_save();
    }
    pthread_mutex_unlock(&pj_lock);
    return fresh;
}
static bool pj_preview_has(const char *feed)
{
    return pj.prev_ok && pj.prev_show.feed[0] && sl_pod_feed_id(pj.prev_show.feed) == sl_pod_feed_id(feed);
}
static void pj_refresh(uint32_t id, int kind, const char *feed, const char *title, const char *author, const char *art)
{
    char url[400], name[160];
    struct sl_pod_show s;
    if (kind == SL_POD_JOB_REFRESH)
    {
        if (!sl_pod_show(id, &s)) return;
        snprintf(url, sizeof url, "%s", s.feed);
        snprintf(name, sizeof name, "%s", s.title);
    }
    else { snprintf(url, sizeof url, "%s", feed); snprintf(name, sizeof name, "%s", title ? title : ""); }
    struct pj_read r;
    int got = pj_fetch_feed(url, &r);
    if (got <= 0)
    {
        pj_done(kind, false, 0, name, got < 0 ? "The feed did not answer" : "That address is not a podcast feed");
        return;
    }
    bool added = false;
    int fresh = pj_take_feed(id, url, &r, title, author, art, &added);
    pj_read_free(&r);
    struct sl_pod_show now;
    if (sl_pod_show(id, &now)) snprintf(name, sizeof name, "%s", now.title);
    pj_done(kind, fresh >= 0, fresh, name, fresh >= 0 ? NULL : "Could not save the show");
    if (fresh >= 0) pj_art(id);
}
/* what was read, as a show and its episodes in one block (freed as sl_pod_eps_free does) */
static bool pj_read_to(const char *feed, struct pj_read *r, struct sl_pod_show *s, struct sl_pod_eps *e)
{
    memset(s, 0, sizeof *s);
    memset(e, 0, sizeof *e);
    s->id = sl_pod_feed_id(feed);
    snprintf(s->feed, sizeof s->feed, "%s", feed);
    snprintf(s->title, sizeof s->title, "%s", r->title);
    snprintf(s->author, sizeof s->author, "%s", r->author);
    snprintf(s->image, sizeof s->image, "%s", r->image);
    s->n_eps = (uint16_t)r->n;
    size_t pool = 0;
    for (int i = 0; i < r->n; i++) pool += strlen(r->it[i].title) + strlen(r->it[i].url) + 3;
    e->ep = calloc((size_t)r->n + 1, sizeof *e->ep);
    e->pool = malloc(pool + 1);
    if (!e->ep || !e->pool) { free(e->ep); free(e->pool); memset(e, 0, sizeof *e); return false; }
    char *w = e->pool;
    for (int i = 0; i < r->n; i++)
    {
        struct sl_pod_ep *p = &e->ep[e->n++];
        p->key = r->it[i].key; p->date = r->it[i].date; p->dur = r->it[i].dur;
        p->bytes = r->it[i].bytes > 0xffffffffLL ? 0xffffffffu : (uint32_t)r->it[i].bytes;
        p->title = w; w = pj_put(w, r->it[i].title);
        p->url = w; w = pj_put(w, r->it[i].url);
        p->file = w; *w++ = '\0';
    }
    e->show = s->id;
    return true;
}
static void pj_preview(const char *feed)
{
    struct pj_read r;
    int got = pj_fetch_feed(feed, &r);
    struct sl_pod_show s;
    struct sl_pod_eps e;
    memset(&s, 0, sizeof s);
    memset(&e, 0, sizeof e);
    if (got > 0 && !pj_read_to(feed, &r, &s, &e)) got = -1;
    pj_read_free(&r);
    pthread_mutex_lock(&pj_lock);
    sl_pod_eps_free(&pj.prev_eps);
    pj.prev_show = s;
    pj.prev_eps = e;
    pj.prev_ok = got > 0;
    pj.st.preview_ok = got > 0 ? 1 : got;
    pj.st.preview_seq++;
    pj.st.seq++;
    pthread_mutex_unlock(&pj_lock);
}
bool sl_pod_preview_take(struct sl_pod_show *show, struct sl_pod_eps *eps)
{
    pthread_mutex_lock(&pj_lock);
    bool ok = pj.prev_ok;
    if (ok)
    {
        *show = pj.prev_show;
        /* a copy: the preview stays, so following it needs no second read */
        memset(eps, 0, sizeof *eps);
        size_t pool = 0;
        for (int i = 0; i < pj.prev_eps.n; i++) pool += strlen(pj.prev_eps.ep[i].title) + strlen(pj.prev_eps.ep[i].url) + 3;
        eps->ep = calloc((size_t)pj.prev_eps.n + 1, sizeof *eps->ep);
        eps->pool = malloc(pool + 1);
        if (eps->ep && eps->pool)
        {
            char *w = eps->pool;
            eps->show = pj.prev_eps.show;
            for (int i = 0; i < pj.prev_eps.n; i++)
            {
                struct sl_pod_ep *p = &eps->ep[eps->n++];
                *p = pj.prev_eps.ep[i];
                p->title = w; w = pj_put(w, pj.prev_eps.ep[i].title);
                p->url = w; w = pj_put(w, pj.prev_eps.ep[i].url);
                p->file = w; *w++ = '\0';
            }
        }
        else { free(eps->ep); free(eps->pool); memset(eps, 0, sizeof *eps); ok = false; }
    }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
/* follow what was previewed, from what was read then */
static bool pj_follow_previewed(const char *title, const char *author, const char *art)
{
    pthread_mutex_lock(&pj_lock);
    bool have = pj.prev_ok;
    struct pj_read r;
    memset(&r, 0, sizeof r);
    char feed[400] = "";
    if (have)
    {
        snprintf(feed, sizeof feed, "%s", pj.prev_show.feed);
        snprintf(r.title, sizeof r.title, "%s", pj.prev_show.title);
        snprintf(r.author, sizeof r.author, "%s", pj.prev_show.author);
        snprintf(r.image, sizeof r.image, "%s", pj.prev_show.image);
        r.cap = pj.prev_eps.n + 1;
        r.it = calloc((size_t)r.cap, sizeof *r.it);
        for (int i = 0; r.it && i < pj.prev_eps.n; i++)
        {
            struct pj_item *it = &r.it[r.n];
            const struct sl_pod_ep *p = &pj.prev_eps.ep[i];
            it->key = p->key; it->date = p->date; it->dur = p->dur; it->bytes = p->bytes;
            it->title = strdup(p->title); it->url = strdup(p->url);
            if (it->title && it->url) r.n++; else { free(it->title); free(it->url); }
        }
        have = r.it != NULL;
    }
    pthread_mutex_unlock(&pj_lock);
    if (!have) return false;
    uint32_t id = sl_pod_feed_id(feed);
    bool added = false;
    int fresh = pj_take_feed(id, feed, &r, title, author, art, &added);
    pj_read_free(&r);
    struct sl_pod_show now;
    char name[160] = "";
    if (sl_pod_show(id, &now)) snprintf(name, sizeof name, "%s", now.title);
    pj_done(SL_POD_JOB_FOLLOW, fresh >= 0, fresh, name, fresh >= 0 ? NULL : "Could not save the show");
    if (fresh >= 0) pj_art(id);
    return true;
}
/* the files of a show's episodes, and the marks that they are there */
static void pj_delete(uint32_t show, const uint32_t *keys, int n)
{
    pthread_mutex_lock(&pj_lock);
    struct sl_pod_show *s = pj_show(show);
    struct sl_pod_eps e;
    int gone = 0;
    if (s && !pj.hold && pj_eps_read(show, &e))
    {
        for (int i = 0; i < e.n; i++)
        {
            bool want = false;
            for (int k = 0; k < n && !want; k++) want = keys[k] == e.ep[i].key;
            if (!want || !e.ep[i].file[0]) continue;
            char rb[400], real[512];
            snprintf(rb, sizeof rb, "%s/%s/%s", sl_pod_folder, s->folder, e.ep[i].file);
            if (pj_real(real, sizeof real, rb) && (unlink(real) == 0 || errno == ENOENT)) { e.ep[i].file = (char *)""; gone++; }
        }
        if (gone) pj_eps_write(show, &e);
        sl_pod_eps_free(&e);
    }
    pthread_mutex_unlock(&pj_lock);
    pj_done(SL_POD_JOB_DELETE, gone > 0, gone, NULL, NULL);
}

/* ---------------------------------------------------------------- OPML */
static void pj_opml_import(const char *rb)
{
    char *b = pj_slurp(rb, 4 * 1024 * 1024, NULL);
    if (!b) { pj_done(SL_POD_JOB_OPML, false, 0, NULL, "Could not read that file"); return; }
    int added = 0, seen = 0;
    for (char *s = b; (s = strstr(s, "<outline")) != NULL; )
    {
        char *e = strchr(s, '>');
        if (!e) break;
        *e = '\0';
        char feed[400], title[160];
        if (px_attr(s, "xmlUrl", feed, sizeof feed) && feed[0])
        {
            if (!px_attr(s, "text", title, sizeof title) || !title[0]) px_attr(s, "title", title, sizeof title);
            seen++;
            uint32_t id = sl_pod_feed_id(feed);
            pthread_mutex_lock(&pj_lock);
            bool stop = pj.stop_job || pj.hold;
            bool have = pj_show(id) != NULL;
            pthread_mutex_unlock(&pj_lock);
            if (stop) break;
            if (!have)
            {
                struct pj_read r;
                int got = pj_fetch_feed(feed, &r);
                if (got > 0)
                {
                    bool fresh_show = false;
                    if (pj_take_feed(id, feed, &r, title, "", "", &fresh_show) >= 0 && fresh_show) added++;
                    pj_read_free(&r);
                    pj_art(id);
                }
            }
        }
        s = e + 1;
    }
    free(b);
    pj_done(SL_POD_JOB_OPML, seen > 0, added, NULL, seen ? NULL : "No podcasts in that file");
}
static void pj_xml(struct pj_buf *o, const char *s)
{
    for (; *s; s++)
        switch (*s)
        {
            case '&': pj_add(o, "&amp;"); break;
            case '<': pj_add(o, "&lt;"); break;
            case '>': pj_add(o, "&gt;"); break;
            case '"': pj_add(o, "&quot;"); break;
            default: pj_add(o, "%c", *s); break;
        }
}
bool sl_pod_opml_export(char *path_out, size_t n)
{
    struct pj_buf o = { 0 };
    pthread_mutex_lock(&pj_lock);
    pj_shows_load();
    pj_add(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\">\n<head><title>GabagoolOS podcasts</title></head>\n<body>\n");
    for (int i = 0; i < pj.n_shows; i++)
    {
        pj_add(&o, "<outline type=\"rss\" text=\"");
        pj_xml(&o, pj.shows[i].title);
        pj_add(&o, "\" title=\"");
        pj_xml(&o, pj.shows[i].title);
        pj_add(&o, "\" xmlUrl=\"");
        pj_xml(&o, pj.shows[i].feed);
        pj_add(&o, "\"/>\n");
    }
    pj_add(&o, "</body>\n</opml>\n");
    char rb[96];
    snprintf(rb, sizeof rb, "%s/Podcasts.opml", sl_pod_folder);
    pj_mkdirs(sl_pod_folder);
    bool ok = !pj.hold && !o.bad && pj_write(rb, o.b, o.n);
    pthread_mutex_unlock(&pj_lock);
    free(o.b);
    if (ok && path_out) snprintf(path_out, n, "%s", rb);
    return ok;
}
int sl_pod_opml_list(char names[][96], int max)
{
    int n = 0;
    /* the card's top is the podcasts folder's parent */
    char top[64];
    snprintf(top, sizeof top, "%s", sl_pod_folder);
    char *cut = strrchr(top, '/');
    if (cut) cut[cut == top ? 1 : 0] = '\0';
    for (int where = 0; where < 2; where++)
    {
        char real[512];
        if (!pj_real(real, sizeof real, where ? sl_pod_folder : top)) continue;
        DIR *d = opendir(real);
        struct dirent *ent;
        while (d && n < max && (ent = readdir(d)) != NULL)
        {
            size_t l = strlen(ent->d_name);
            if (ent->d_name[0] == '.' || l < 6 || strcasecmp(ent->d_name + l - 5, ".opml")) continue;
            snprintf(names[n++], 96, "%s/%s", where ? sl_pod_folder : strcmp(top, "/") ? top : "", ent->d_name);
        }
        if (d) closedir(d);
    }
    return n;
}

/* ---------------------------------------------------------------- the worker */
static void pj_dl_start_next(void)
{
    if (pj.dl_on || !pj.n_q || pj.hold || !pj.online || (pj.dl_retry_at && (int32_t)(pj_ms() - pj.dl_retry_at) < 0)) return;
    struct pj_dlq *q = &pj.q[0];
    char part[512], dir[400], rdir[512];
    snprintf(dir, sizeof dir, "%s/%s", sl_pod_folder, q->dest);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    pj_mkdirs(dir);
    if (!pj_dest_real(q->dest, part, sizeof part, true) || !pj_real(rdir, sizeof rdir, dir)) return;
    if (!sl_net_dl_start(&pj.dl, q->url, SL_POD_UA, part)) { pj.dl_fails++; pj.dl_retry_at = pj_ms() + 30000; return; }
    pj.dl_on = true;
    pj.dl_seen_ms = pj_ms();
    pj.st.dl_show = q->show;
    pj.st.dl_key = q->key;
    pj.st.dl_total = q->bytes;
    pj.st.dl_got = 0;
    pj.st.queued = pj.n_q - 1;
    pj.st.eps_seq++;
    pj.st.eps_show = q->show;
    pj.st.seq++;
}
/* one look at the download: how far, and its end */
static void pj_dl_step(void)
{
    if (!pj.dl_on) return;
    struct stat st;
    if (!stat(pj.dl.path, &st) && (long long)st.st_size != pj.st.dl_got) { pj.st.dl_got = (long long)st.st_size; pj.st.seq++; }
    int r = sl_net_dl_poll(&pj.dl);
    if (r == 0) return;
    struct pj_dlq q = pj.q[0];
    bool whole = r == 200 || r == 206;
    char part[512], fin[512];
    pj_dest_real(q.dest, part, sizeof part, true);
    pj_dest_real(q.dest, fin, sizeof fin, false);
    pj.dl_on = false;
    pj.st.dl_show = pj.st.dl_key = 0;
    pj.st.dl_got = pj.st.dl_total = 0;
    if (whole && !pj.hold && !rename(part, fin))
    {
        /* its show's file says it is here */
        struct sl_pod_eps e;
        if (pj_eps_read(q.show, &e))
        {
            struct sl_pod_ep *p = pj_find_ep(&e, q.key);
            const char *name = strrchr(q.dest, '/');
            struct stat fs;
            if (p)
            {
                p->file = (char *)(name ? name + 1 : q.dest);
                /* the file's own size from now on: a feed's often says something else */
                if (!stat(fin, &fs) && fs.st_size > 0) p->bytes = fs.st_size > 0xffffffffLL ? 0xffffffffu : (uint32_t)fs.st_size;
                pj_eps_write(q.show, &e);
            }
            sl_pod_eps_free(&e);
        }
        memmove(&pj.q[0], &pj.q[1], (size_t)(pj.n_q - 1) * sizeof *pj.q);
        pj.n_q--;
        pj_q_free(&q);
        pj.dl_fails = 0;
        pj.st.dl_ok = true;
    }
    else if (r == 404 || r == 410 || r == 403)
    {
        /* gone from the web: not tried again */
        unlink(part);
        memmove(&pj.q[0], &pj.q[1], (size_t)(pj.n_q - 1) * sizeof *pj.q);
        pj.n_q--;
        pj_q_free(&q);
        pj.st.dl_ok = false;
        snprintf(pj.st.why, sizeof pj.st.why, "An episode is no longer on the web");
    }
    else
    {
        /* cut off: its part stays, and it carries on after a while */
        pj.dl_fails++;
        pj.dl_retry_at = pj_ms() + (pj.dl_fails < 3 ? 5000u : 60000u);
        pj.st.dl_ok = false;
        snprintf(pj.st.why, sizeof pj.st.why, "A download was cut off. It carries on soon");
        if (pj.dl_fails >= 6)
        {
            /* the same one six times: to the end of the line */
            memmove(&pj.q[0], &pj.q[1], (size_t)(pj.n_q - 1) * sizeof *pj.q);
            pj.q[pj.n_q - 1] = q;
            pj.dl_fails = 0;
        }
    }
    pj_queue_save();
    pj.st.queued = pj.n_q;
    pj.st.dl_seq++;
    pj.st.eps_seq++;
    pj.st.eps_show = q.show;
    pj.st.seq++;
}
static void pj_run(struct pj_job *j)
{
    switch (j->kind)
    {
        case SL_POD_JOB_REFRESH:
            if (j->show) pj_refresh(j->show, SL_POD_JOB_REFRESH, NULL, NULL, NULL, NULL);
            else
            {
                /* every show, one after another; the count is the new episodes */
                int n = 0, fresh = 0;
                struct sl_pod_show *all = sl_pod_shows_copy(&n);
                for (int i = 0; i < n; i++)
                {
                    pthread_mutex_lock(&pj_lock);
                    bool stop = pj.stop_job || pj.hold;
                    pthread_mutex_unlock(&pj_lock);
                    if (stop) break;
                    struct pj_read r;
                    if (pj_fetch_feed(all[i].feed, &r) > 0)
                    {
                        bool added;
                        int k = pj_take_feed(all[i].id, all[i].feed, &r, NULL, NULL, NULL, &added);
                        if (k > 0) fresh += k;
                        pj_read_free(&r);
                        if (!all[i].art) pj_art(all[i].id);
                    }
                }
                free(all);
                pj_done(SL_POD_JOB_REFRESH, true, fresh, NULL, NULL);
            }
            break;
        case SL_POD_JOB_FOLLOW:
        {
            /* title, author and picture from the directory ride in arg, after the feed */
            char *f = j->arg, *feed = pj_next(&f), *title = pj_next(&f), *author = pj_next(&f), *art = pj_next(&f);
            pthread_mutex_lock(&pj_lock);
            bool prev = pj_preview_has(feed);
            pthread_mutex_unlock(&pj_lock);
            if (!prev || !pj_follow_previewed(title, author, art))
                pj_refresh(sl_pod_feed_id(feed), SL_POD_JOB_FOLLOW, feed, title, author, art);
            break;
        }
        case SL_POD_JOB_SEARCH: pj_search(j->arg); break;
        case SL_POD_JOB_PREVIEW: pj_preview(j->arg); break;
        case SL_POD_JOB_OPML: pj_opml_import(j->arg); break;
        case SL_POD_JOB_DELETE: pj_delete(j->show, j->keys, j->n); break;
        case SL_POD_JOB_ART: pj_art(j->show); break;
    }
}
static void *pj_loop(void *unused)
{
    (void)unused;
    uint32_t idle_from = pj_ms();
    pthread_mutex_lock(&pj_lock);
    pj_queue_load();
    for (;;)
    {
        pj_dl_start_next();
        pj_dl_step();
        if (pj.n_jobs && !pj.hold)
        {
            struct pj_job j = pj.jobs[0];
            memmove(&pj.jobs[0], &pj.jobs[1], (size_t)--pj.n_jobs * sizeof *pj.jobs);
            pj.st.job = j.kind;
            pj.st.jobs = pj.n_jobs;
            pj.stop_job = false;
            pj.st.seq++;
            pthread_mutex_unlock(&pj_lock);
            pj_run(&j);
            free(j.keys);
            pthread_mutex_lock(&pj_lock);
            pj.st.job = SL_POD_JOB_NONE;
            pj.st.seq++;
            idle_from = pj_ms();
            continue;
        }
        bool waiting = pj.dl_on || (pj.n_q && pj.online && !pj.hold);
        if (waiting) idle_from = pj_ms();
        if (!pj.n_jobs && !waiting && (int32_t)(pj_ms() - idle_from) >= (int32_t)PJ_IDLE_MS) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 300 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&pj_cond, &pj_lock, &ts);
    }
    pj.up = false;
    pj_prog_save();
    pthread_mutex_unlock(&pj_lock);
    return NULL;
}
/* the thread, if it is not running; locked */
static bool pj_start(void)
{
    if (pj.up) { pthread_cond_broadcast(&pj_cond); return true; }
    if (pj.thread) pthread_join(pj.thread, NULL);   /* the last one has ended */
    pj.thread = 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    pj.up = pthread_create(&pj.thread, &a, pj_loop, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!pj.up) pj.thread = 0;
    return pj.up;
}
static void pj_wake(void)
{
    pthread_mutex_lock(&pj_lock);
    pj_start();
    pthread_mutex_unlock(&pj_lock);
}
static bool pj_job(int kind, uint32_t show, const char *arg, const uint32_t *keys, int n)
{
    pthread_mutex_lock(&pj_lock);
    bool ok = pj.n_jobs < PJ_JOBS;
    /* the same job waiting already is enough */
    for (int i = 0; ok && i < pj.n_jobs; i++)
        if (pj.jobs[i].kind == kind && pj.jobs[i].show == show && !strcmp(pj.jobs[i].arg, arg ? arg : "") && !keys)
        { pthread_mutex_unlock(&pj_lock); return true; }
    if (ok)
    {
        struct pj_job *j = &pj.jobs[pj.n_jobs];
        memset(j, 0, sizeof *j);
        j->kind = kind;
        j->show = show;
        snprintf(j->arg, sizeof j->arg, "%s", arg ? arg : "");
        if (keys && n > 0)
        {
            j->keys = malloc((size_t)n * sizeof *keys);
            if (j->keys) { memcpy(j->keys, keys, (size_t)n * sizeof *keys); j->n = n; }
        }
        pj.n_jobs++;
        pj.st.jobs = pj.n_jobs;
        pj.st.seq++;
        ok = pj_start();
        if (!ok) { free(j->keys); pj.n_jobs--; }
    }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
bool sl_pod_follow(const char *feed, const char *title, const char *author, const char *art)
{
    if (!feed || strncasecmp(feed, "http", 4)) return false;
    char a[4][400], arg[400];
    snprintf(arg, sizeof arg, "%s\t%s\t%s\t%s", pj_f(feed, a[0], sizeof a[0]), pj_f(title ? title : "", a[1], 160),
             pj_f(author ? author : "", a[2], 96), pj_f(art ? art : "", a[3], sizeof a[3]));
    return pj_job(SL_POD_JOB_FOLLOW, 0, arg, NULL, 0);
}
bool sl_pod_refresh(uint32_t id) { return pj_job(SL_POD_JOB_REFRESH, id, NULL, NULL, 0); }
bool sl_pod_search(const char *term) { return term && term[0] && pj_job(SL_POD_JOB_SEARCH, 0, term, NULL, 0); }
bool sl_pod_preview(const char *feed) { return feed && !strncasecmp(feed, "http", 4) && pj_job(SL_POD_JOB_PREVIEW, 0, feed, NULL, 0); }
bool sl_pod_opml_import(const char *path) { return path && path[0] && pj_job(SL_POD_JOB_OPML, 0, path, NULL, 0); }
bool sl_pod_delete(uint32_t show, const uint32_t *keys, int n) { return n > 0 && pj_job(SL_POD_JOB_DELETE, show, NULL, keys, n); }
bool sl_pod_unfollow(uint32_t id, bool delete_files)
{
    pthread_mutex_lock(&pj_lock);
    struct sl_pod_show *s = pj_show(id);
    bool ok = s && !pj.hold;
    if (ok)
    {
        /* its downloads stop and leave the line */
        pj_queue_load();
        for (int k = pj.n_q - 1; k >= 0; k--)
        {
            if (pj.q[k].show != id) continue;
            if (k == 0 && pj.dl_on) pj_dl_end(false);
            pj_q_free(&pj.q[k]);
            memmove(&pj.q[k], &pj.q[k + 1], (size_t)(pj.n_q - k - 1) * sizeof *pj.q);
            pj.n_q--;
        }
        pj_queue_save();
        pj.st.queued = pj.n_q;
        char rb[400], real[512];
        if (delete_files)
        {
            struct sl_pod_eps e;
            if (pj_eps_read(id, &e))
            {
                for (int i = 0; i < e.n; i++)
                    if (e.ep[i].file[0] && snprintf(rb, sizeof rb, "%s/%s/%s", sl_pod_folder, s->folder, e.ep[i].file) < (int)sizeof rb &&
                        pj_real(real, sizeof real, rb)) unlink(real);
                sl_pod_eps_free(&e);
            }
            snprintf(rb, sizeof rb, "%s/%s", sl_pod_folder, s->folder);
            if (pj_real(real, sizeof real, rb)) rmdir(real);   /* only when nothing else is in it */
        }
        pj_eps_name(rb, sizeof rb, id);
        if (pj_real(real, sizeof real, rb)) unlink(real);
        static const int px[] = { SL_POD_ART_BIG, SL_POD_ART_ROW };
        for (int i = 0; i < 2; i++) { sl_pod_art_name(rb, sizeof rb, id, px[i]); if (pj_real(real, sizeof real, rb)) unlink(real); }
        pj_prog_load();
        for (int i = pj.n_prog - 1; i >= 0; i--)
            if (pj.prog[i].show == id)
            {
                memmove(&pj.prog[i], &pj.prog[i + 1], (size_t)(pj.n_prog - i - 1) * sizeof *pj.prog);
                pj.n_prog--;
                pj.prog_dirty = true;
            }
        pj_prog_save();
        *s = pj.shows[--pj.n_shows];
        pj_shows_save();
    }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
bool sl_pod_set_speed(uint32_t id, int percent)
{
    pthread_mutex_lock(&pj_lock);
    struct sl_pod_show *s = pj_show(id);
    bool ok = s && !pj.hold;
    if (ok) { s->speed = (uint16_t)percent; ok = pj_shows_save(); }
    pthread_mutex_unlock(&pj_lock);
    return ok;
}
void sl_pod_online(bool up)
{
    pthread_mutex_lock(&pj_lock);
    bool was = pj.online;
    pj.online = up;
    pj.st.online = up;
    if (up && !was)
    {
        /* downloads that were waiting for Wi-Fi start: the queue's file says if there are any */
        char real[512];
        struct stat st;
        if (pj.n_q || (pj_real(real, sizeof real, PJ_QUEUE) && !stat(real, &st))) { pj.dl_retry_at = 0; pj_start(); }
    }
    if (!up && pj.dl_on) pj_dl_end(true);
    pthread_cond_broadcast(&pj_cond);
    pthread_mutex_unlock(&pj_lock);
}
void sl_pod_hold(bool hold)
{
    pthread_mutex_lock(&pj_lock);
    if (hold)
    {
        pj_prog_save();                   /* where it was, before the card goes */
        pj_dl_end(true);                  /* wget lets go of its file now */
        pj.stop_job = true;               /* a job's requests end soon; it writes nothing */
    }
    pj.hold = hold;
    pj.st.hold = hold;
    pj.st.seq++;
    if (!hold && pj.n_q && pj.online) { pj.dl_retry_at = 0; pj_start(); }
    pthread_cond_broadcast(&pj_cond);
    pthread_mutex_unlock(&pj_lock);
}
void sl_pod_status(struct sl_pod_status *s)
{
    pthread_mutex_lock(&pj_lock);
    *s = pj.st;
    pthread_mutex_unlock(&pj_lock);
}
bool sl_pod_busy(void)
{
    pthread_mutex_lock(&pj_lock);
    bool up = pj.up;
    pthread_mutex_unlock(&pj_lock);
    return up;
}
void sl_pod_poll(void)
{
    pthread_mutex_lock(&pj_lock);
    if (!pj.up && pj.n_q && pj.online && !pj.hold) pj_start();
    pthread_mutex_unlock(&pj_lock);
}
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
/* the tests' fresh start: what is in memory goes, the files are read again */
void sl_pod_test_forget(void)
{
    pthread_mutex_lock(&pj_lock);
    while (pj.up)
    {
        pj.stop_job = true;
        pthread_cond_broadcast(&pj_cond);
        pthread_mutex_unlock(&pj_lock);
        struct timespec ts = { 0, 20000000L };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&pj_lock);
        if (!pj.n_jobs && !pj.dl_on && !pj.n_q) break;
    }
    pj_dl_end(true);
    for (int i = 0; i < pj.n_q; i++) pj_q_free(&pj.q[i]);
    free(pj.q); pj.q = NULL; pj.n_q = pj.cap_q = 0; pj.q_read = false;
    free(pj.shows); pj.shows = NULL; pj.n_shows = pj.cap_shows = 0; pj.shows_read = false;
    free(pj.prog); pj.prog = NULL; pj.n_prog = 0; pj.prog_read = false; pj.prog_dirty = false;
    free(pj.found); pj.found = NULL; pj.n_found = 0;
    sl_pod_eps_free(&pj.prev_eps); pj.prev_ok = false;
    pj.dl_fails = 0; pj.dl_retry_at = 0;
    pthread_mutex_unlock(&pj_lock);
}
/* the parser and the directory's reader, for the tests: xml fed slice bytes at a time */
int sl_pod_test_parse(const char *xml, size_t slice, struct sl_pod_show *show, struct sl_pod_eps *eps)
{
    struct pj_read r;
    memset(&r, 0, sizeof r);
    struct sl_pod_feed_cb cb = { pj_read_show, pj_read_item, &r };
    struct sl_pod_parser *p = sl_pod_parse_begin(&cb);
    size_t n = strlen(xml);
    for (size_t at = 0; at < n; at += slice) sl_pod_parse_feed(p, xml + at, n - at < slice ? n - at : slice);
    bool feed = sl_pod_parse_end(p);
    if (r.n) pj_read_trim(&r);
    pj_read_to("https://test", &r, show, eps);
    pj_read_free(&r);
    return feed;
}
int sl_pod_test_search(const char *json, struct sl_pod_found *out, int max) { return pj_search_parse(json, out, max); }
void sl_pod_test_art_smaller(char *url) { pj_art_smaller(url); }
uint32_t sl_pod_test_key(const char *guid) { return pj_fnv(guid); }
size_t sl_pod_test_static_bytes(void) { return sizeof pj; }
#endif
