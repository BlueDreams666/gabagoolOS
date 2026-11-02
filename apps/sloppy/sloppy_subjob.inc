/* GabagoolOS and a music server: the worker, the files and the answers'
 * reader. See sloppy_subsonic.h. Included at the end of sloppy_libjob.c,
 * after sloppy_podjob.inc, whose file helpers (pj_real, pj_write, pj_slurp,
 * pj_buf, pj_f, pj_next, pj_fnv, the picture squares) it uses. GPLv2+.
 *
 * One lock guards everything here; the network is never waited on under
 * it. Answers are read as they arrive by a small JSON reader of our own,
 * never kept whole: a list of a thousand songs costs its strings, once.
 * The password is the token's only ingredient: each request carries
 * md5(password + salt) and a new salt (or the OpenSubsonic API key the
 * server offers), and the password itself is never logged or sent, except
 * to a server that takes no tokens over HTTPS, the API's old way. */
#include "sloppy_subsonic.h"
#include "sloppy_lastfm_state.h"           /* MD5 */

char sl_sub_folder[64] = "/Subsonic";
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
unsigned sl_sub_idle_ms = 30000;
#define SJ_IDLE_MS sl_sub_idle_ms
#else
#define SJ_IDLE_MS 30000u
#endif
#define SJ_JOBS 32
#define SJ_ANSWERS 6
#define SJ_ART_Q 48
#define SJ_POOL_CAP (768 * 1024)
#define SJ_ART_CAP (2 * 1024 * 1024)
#define SJ_SCROB_MAX 200
#define SJ_CACHE_MAX 8000
#define SJ_PIN_MAX 64
#define SJ_SERVERS SL_SUB_STATE "/servers.tsv"
#define SJ_PREFS SL_SUB_STATE "/prefs.txt"
#define SJ_CACHE SL_SUB_STATE "/cache.tsv"
#define SJ_PINS SL_SUB_STATE "/pins.tsv"
#define SJ_QUEUE SL_SUB_STATE "/queue.tsv"
#define SJ_SCROBS SL_SUB_STATE "/scrobbles.tsv"

/* a song on the card, or coming: its words in one block */
enum { SF_ID, SF_ALBUM_ID, SF_COVER, SF_TITLE, SF_ARTIST, SF_ALBUM, SF_SUFFIX, SF_REL, SF_N };
struct sj_song {
    uint32_t server, size_kb, used, sess, dur;
    uint16_t track, disc;
    uint8_t pinned, starred;
    char *f[SF_N];                        /* f[0] is the block */
};
struct sj_dl { struct sj_song s; unsigned session; bool pin; uint32_t est_kb; };
struct sj_srv { struct sl_sub_server pub; char secret[160]; };
struct sj_pin { uint32_t server; uint8_t kind; char id[48], name[96]; int n; char *ids; };   /* ids: NUL separated */
struct sj_scrob { uint32_t server, when; bool sub; char id[48]; };
struct sj_job {
    int kind, what, offset;
    unsigned req;
    bool on, save;
    uint32_t id;
    char arg[200], url[192], user[64], secret[160];
};
static pthread_mutex_t sj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sj_cond = PTHREAD_COND_INITIALIZER;
static struct {
    bool up, online, hold, stop_job;
    pthread_t thread;
    struct sj_job jobs[SJ_JOBS];
    int n_jobs;
    struct sl_sub_status st;
    unsigned req_next, session;
    /* servers */
    bool srv_read;
    struct sj_srv srv[SL_SUB_SERVERS];
    int n_srv;
    uint32_t active;
    /* settings */
    bool prefs_read;
    struct sl_sub_prefs prefs;
    /* the cache */
    bool cache_read, cache_dirty;
    struct sj_song *cache;
    int n_cache, cap_cache;
    uint32_t clock;
    long long used_kb;
    bool pins_read;
    struct sj_pin *pins;
    int n_pins;
    /* downloads */
    bool q_read;
    struct sj_dl *q;
    int n_q, cap_q;
    bool dl_on;
    struct sl_net_dl dl;
    int dl_fails;
    uint32_t dl_retry_at;
    /* answers for the interface */
    struct sl_sub_list ans[SJ_ANSWERS];
    bool ans_ok[SJ_ANSWERS];
    unsigned forgot[8];
    /* covers wanted */
    char art_q[SJ_ART_Q][48];
    int n_art;
    /* plays to tell */
    bool scrob_read;
    struct sj_scrob *scrob;
    int n_scrob;
    uint32_t scrob_retry_at;
} sj;

static uint32_t sj_ms(void) { return lj_ms(); }

/* ---------------------------------------------------------------- small things */
static void sj_hex(char *out, const unsigned char *b, size_t n)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = x[b[i] >> 4]; out[2 * i + 1] = x[b[i] & 15]; }
    out[2 * n] = '\0';
}
static bool sj_unhex(char *out, size_t cap, const char *h)
{
    size_t n = strlen(h);
    if (n % 2 || n / 2 >= cap) return false;
    for (size_t i = 0; i < n / 2; i++)
    {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (char)v;
    }
    out[n / 2] = '\0';
    return true;
}
/* a salt nobody can guess twice: the clock, a counter and the urandom the system has */
static void sj_salt(char out[17])
{
    static uint32_t x, k;
    if (!x)
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { if (read(fd, &x, sizeof x) != (ssize_t)sizeof x) x = 0; close(fd); }
        x ^= (uint32_t)time(NULL) ^ (sj_ms() << 12) ^ (uint32_t)getpid();
        if (!x) x = 0x9e3779b9u;
    }
    unsigned char b[8];
    for (int i = 0; i < 8; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = (unsigned char)(x >> 8) ^ (unsigned char)(++k * 37); }
    sj_hex(out, b, 8);
}
void sl_sub_token(const char *secret, const char *salt, char out[33])
{
    struct sl_md5 m;
    sl_md5_init(&m);
    sl_md5_add(&m, secret, strlen(secret));
    sl_md5_add(&m, salt, strlen(salt));
    sl_md5_hex(&m, out);
}
/* "music.example.com" -> https, "192.168.1.5:4533" -> http; no "/rest" or slash at the end */
bool sl_sub_url_fix(const char *in, char *out, size_t n)
{
    char b[192];
    size_t k = 0;
    for (; in && *in && k + 1 < sizeof b; in++) if ((unsigned char)*in > ' ') b[k++] = *in;
    b[k] = '\0';
    while (k && b[k - 1] == '/') b[--k] = '\0';
    if (k >= 5 && !strcasecmp(b + k - 5, "/rest")) { k -= 5; b[k] = '\0'; }
    while (k && b[k - 1] == '/') b[--k] = '\0';
    if (!k) return false;
    const char *host = b;
    bool scheme = !strncasecmp(b, "http://", 7) || !strncasecmp(b, "https://", 8);
    if (scheme) host = strstr(b, "://") + 3;
    else if (strstr(b, "://")) return false;   /* ftp:// and the like */
    if (!host[0] || host[0] == '/' || host[0] == ':') return false;
    for (const char *c = host; *c && *c != '/'; c++)
        if (!isalnum((unsigned char)*c) && !strchr(".-:[]_", *c)) return false;
    if (scheme) return snprintf(out, n, "%s", b) < (int)n;
    /* no scheme: a local address or a port is a home server, a name a proxied one */
    bool local = strchr(host, ':') || isdigit((unsigned char)host[0]) || strstr(host, ".local") || !strchr(host, '.');
    return snprintf(out, n, "%s://%s", local ? "http" : "https", b) < (int)n;
}
void sl_sub_file_name(char *out, size_t n, int disc, int track, const char *title, const char *ext)
{
    char t[120], num[16] = "";
    sl_pod_clean_name(t, sizeof t, title && title[0] ? title : "Track");
    if (track > 0 && disc > 1) snprintf(num, sizeof num, "%d-%02d ", disc, track);
    else if (track > 0) snprintf(num, sizeof num, "%02d ", track);
    snprintf(out, n, "%s%s.%s", num, t, sl_pod_clean_ext(ext, "mp3"));
}

/* ---------------------------------------------------------------- the answers' reader
 * A JSON reader fed bytes as they come. It keeps the path it is in (the
 * kind of each container and the key it is under) and tells the builder
 * of every container opened and closed and every value. The builder is
 * the Subsonic one below, or another's (Audiobookshelf's, sloppy_absjob.inc)
 * through the three calls. */
#define SJ_DEPTH 20
struct sj_json {
    int d;                                /* containers open; the root object is 1 */
    struct { bool obj; char key[24], name[24]; } st[SJ_DEPTH + 1];
    bool in_str, esc, in_lit, str_key, expect_key, bad;
    int u_left;
    uint32_t u_val, u_hi;
    char buf[640];
    size_t bn;
    /* another builder (NULL: Subsonic's) */
    void *cb;
    void (*cb_open)(void *cb, struct sj_json *j);
    void (*cb_close)(void *cb, struct sj_json *j);
    void (*cb_value)(void *cb, struct sj_json *j, const char *v, bool quoted);
};
struct sj_build {
    struct sl_sub_list *l;                /* NULL: a ping, nothing to keep */
    bool resp, failed, open, ext_api;
    int code, item_d, head_d, err_d, ext_d;
    struct sl_sub_item cur;
    char msg[96], kind[24], version[16];
};
static void sjb_open(struct sj_build *b, struct sj_json *j);
static void sjb_close(struct sj_build *b, struct sj_json *j);
static void sjb_value(struct sj_build *b, struct sj_json *j, const char *v, bool quoted);
static void sj_buf_utf8(struct sj_json *j, uint32_t v)
{
    char u[4];
    int n = v < 0x80 ? 1 : v < 0x800 ? 2 : v < 0x10000 ? 3 : 4;
    if (n == 1) u[0] = (char)v;
    else if (n == 2) { u[0] = (char)(0xc0 | v >> 6); u[1] = (char)(0x80 | (v & 63)); }
    else if (n == 3) { u[0] = (char)(0xe0 | v >> 12); u[1] = (char)(0x80 | (v >> 6 & 63)); u[2] = (char)(0x80 | (v & 63)); }
    else { u[0] = (char)(0xf0 | v >> 18); u[1] = (char)(0x80 | (v >> 12 & 63)); u[2] = (char)(0x80 | (v >> 6 & 63)); u[3] = (char)(0x80 | (v & 63)); }
    if (j->bn + (size_t)n < sizeof j->buf) { memcpy(j->buf + j->bn, u, (size_t)n); j->bn += (size_t)n; }
}
static void sj_on_value(struct sj_json *j, struct sj_build *b, const char *v, bool quoted)
{
    if (j->cb_value) j->cb_value(j->cb, j, v, quoted); else sjb_value(b, j, v, quoted);
}
static void sj_lit_end(struct sj_json *j, struct sj_build *b)
{
    j->in_lit = false;
    j->buf[j->bn] = '\0';
    if (j->d >= 1) sj_on_value(j, b, j->buf, false);
}
static void sj_feed(struct sj_json *j, struct sj_build *b, const char *data, size_t n)
{
    for (size_t i = 0; i < n && !j->bad; i++)
    {
        char c = data[i];
        if (j->in_str)
        {
            if (j->u_left)
            {
                int h = isdigit((unsigned char)c) ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
                if (h < 0) { j->u_left = 0; continue; }
                j->u_val = j->u_val << 4 | (uint32_t)h;
                if (--j->u_left) continue;
                uint32_t v = j->u_val;
                if (v >= 0xd800 && v < 0xdc00) { j->u_hi = v; continue; }
                if (v >= 0xdc00 && v < 0xe000) { if (!j->u_hi) continue; v = 0x10000 + ((j->u_hi - 0xd800) << 10) + (v - 0xdc00); }
                j->u_hi = 0;
                sj_buf_utf8(j, v);
                continue;
            }
            if (j->esc)
            {
                j->esc = false;
                char o = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
                if (c == 'u') { j->u_left = 4; j->u_val = 0; continue; }
                if (j->bn + 1 < sizeof j->buf) j->buf[j->bn++] = o;
                continue;
            }
            if (c == '\\') { j->esc = true; continue; }
            if (c != '"') { if (j->bn + 1 < sizeof j->buf) j->buf[j->bn++] = c; continue; }
            j->in_str = false;
            j->buf[j->bn] = '\0';
            if (j->str_key) { snprintf(j->st[j->d].key, sizeof j->st[j->d].key, "%s", j->buf); j->expect_key = false; }
            else if (j->d >= 1) sj_on_value(j, b, j->buf, true);
            continue;
        }
        if (j->in_lit)
        {
            if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '+')
            {
                if (j->bn + 1 < sizeof j->buf) j->buf[j->bn++] = c;
                continue;
            }
            sj_lit_end(j, b);
        }
        switch (c)
        {
            case '{': case '[':
            {
                if (j->d >= SJ_DEPTH) { j->bad = true; break; }
                const char *name = j->d == 0 ? "" : j->st[j->d].obj ? j->st[j->d].key : j->st[j->d].name;
                j->d++;
                j->st[j->d].obj = c == '{';
                snprintf(j->st[j->d].name, sizeof j->st[j->d].name, "%s", name);
                j->st[j->d].key[0] = '\0';
                j->expect_key = c == '{';
                if (j->cb_open) j->cb_open(j->cb, j); else sjb_open(b, j);
                break;
            }
            case '}': case ']':
                if (j->d <= 0) { j->bad = true; break; }
                if (j->cb_close) j->cb_close(j->cb, j); else sjb_close(b, j);
                j->d--;
                j->expect_key = false;
                break;
            case '"':
                j->in_str = true;
                j->bn = 0;
                j->str_key = j->d >= 1 && j->st[j->d].obj && j->expect_key;
                break;
            case ':': j->expect_key = false; break;
            case ',': if (j->d >= 1 && j->st[j->d].obj) j->expect_key = true; break;
            case ' ': case '\t': case '\r': case '\n': break;
            default:
                j->in_lit = true;
                j->bn = 0;
                j->buf[j->bn++] = c;
                break;
        }
    }
}

/* the list's words, kept once in its pool */
static uint32_t sj_pool(struct sl_sub_list *l, const char *s)
{
    if (!s || !s[0]) return 0;
    size_t n = strlen(s) + 1;
    if (l->pool_n + n > l->pool_cap)
    {
        size_t want = l->pool_cap ? l->pool_cap * 2 : 4096;
        while (want < l->pool_n + n) want *= 2;
        if (want > SJ_POOL_CAP) return 0;
        char *p = realloc(l->pool, want);
        if (!p) return 0;
        if (!l->pool) p[0] = '\0', l->pool_n = 1;
        l->pool = p;
        l->pool_cap = want;
    }
    memcpy(l->pool + l->pool_n, s, n);
    uint32_t at = (uint32_t)l->pool_n;
    l->pool_n += n;
    return at;
}
static int sj_kind_of(const char *name)
{
    if (!strcmp(name, "artist")) return SL_SUB_ARTIST;
    if (!strcmp(name, "album")) return SL_SUB_ALBUM;
    if (!strcmp(name, "song") || !strcmp(name, "entry")) return SL_SUB_SONG;
    if (!strcmp(name, "playlist")) return SL_SUB_PLAYLIST;
    return 0;
}
static void sjb_open(struct sj_build *b, struct sj_json *j)
{
    int d = j->d;
    if (!j->st[d].obj) return;
    const char *name = j->st[d].name;
    bool in_arr = d > 1 && !j->st[d - 1].obj;
    if (d == 2 && !strcmp(name, "subsonic-response")) b->resp = true;
    if (d == 3 && !strcmp(name, "error")) b->err_d = d;
    if (d == 4 && in_arr && !strcmp(name, "openSubsonicExtensions")) b->ext_d = d;
    if (!b->l || b->item_d) return;
    int kind = sj_kind_of(name);
    if (!kind) return;
    if (in_arr) { b->item_d = d; memset(&b->cur, 0, sizeof b->cur); b->cur.kind = (uint8_t)kind; }
    else if (d == 3 && kind != SL_SUB_SONG && !b->head_d) { b->head_d = d; memset(&b->l->head, 0, sizeof b->l->head); b->l->head.kind = (uint8_t)kind; }
}
static void sjb_close(struct sj_build *b, struct sj_json *j)
{
    int d = j->d;
    if (d == b->err_d) b->err_d = 0;
    if (d == b->ext_d) b->ext_d = 0;
    if (d == b->head_d) b->head_d = 0;
    if (d != b->item_d || !b->l) return;
    b->item_d = 0;
    struct sl_sub_list *l = b->l;
    if (l->n >= SL_SUB_LIST_MAX) return;
    if (l->n == l->cap)
    {
        int want = l->cap ? l->cap * 2 : 64;
        if (want > SL_SUB_LIST_MAX) want = SL_SUB_LIST_MAX;
        struct sl_sub_item *it = realloc(l->it, (size_t)want * sizeof *it);
        if (!it) return;
        l->it = it;
        l->cap = want;
    }
    l->it[l->n++] = b->cur;
}
static void sjb_field(struct sl_sub_list *l, struct sl_sub_item *it, const char *k, const char *v)
{
    long num = atol(v);
    if (!strcmp(k, "id")) it->id = sj_pool(l, v);
    else if ((!strcmp(k, "name") || !strcmp(k, "title")) && !it->title) it->title = sj_pool(l, v);
    else if (!strcmp(k, "artist") || (!strcmp(k, "owner") && !it->artist)) it->artist = sj_pool(l, v);
    else if (!strcmp(k, "album")) it->album = sj_pool(l, v);
    else if (!strcmp(k, "albumId")) it->album_id = sj_pool(l, v);
    else if (!strcmp(k, "artistId")) it->artist_id = sj_pool(l, v);
    else if (!strcmp(k, "coverArt")) it->cover = sj_pool(l, v);
    else if (!strcmp(k, "suffix")) it->suffix = sj_pool(l, v);
    else if (!strcmp(k, "track")) it->track = (uint16_t)(num > 0 && num < 65535 ? num : 0);
    else if (!strcmp(k, "discNumber")) it->disc = (uint16_t)(num > 0 && num < 65535 ? num : 0);
    else if (!strcmp(k, "year")) it->year = (uint16_t)(num > 0 && num < 65535 ? num : 0);
    else if (!strcmp(k, "duration")) it->dur = (uint32_t)(num > 0 ? num : 0);
    else if (!strcmp(k, "size")) it->size = (uint32_t)(num > 0 ? (num > 0x7fffffffL ? 0x7fffffffL : num) : 0);
    else if (!strcmp(k, "songCount") || !strcmp(k, "albumCount")) it->count = (uint16_t)(num > 0 && num < 65535 ? num : 0);
    else if (!strcmp(k, "starred")) it->starred = v[0] != '\0';
}
static void sjb_value(struct sj_build *b, struct sj_json *j, const char *v, bool quoted)
{
    int d = j->d;
    const char *k = j->st[d].obj ? j->st[d].key : j->st[d].name;
    if (d == 2 && b->resp)
    {
        if (!strcmp(k, "status")) b->failed = strcmp(v, "ok") != 0;
        else if (!strcmp(k, "type")) snprintf(b->kind, sizeof b->kind, "%s", v);
        else if (!strcmp(k, "serverVersion")) snprintf(b->version, sizeof b->version, "%s", v);
        else if (!strcmp(k, "version") && !b->version[0]) snprintf(b->version, sizeof b->version, "%s", v);
        else if (!strcmp(k, "openSubsonic")) b->open = !strcmp(v, "true");
        return;
    }
    if (b->err_d && d == b->err_d)
    {
        if (!strcmp(k, "code")) b->code = atoi(v);
        else if (!strcmp(k, "message")) snprintf(b->msg, sizeof b->msg, "%s", v);
        return;
    }
    if (b->ext_d && d == b->ext_d)
    {
        if (!strcmp(k, "name") && !strcmp(v, "apiKeyAuthentication")) b->ext_api = true;
        return;
    }
    if (!b->l || (!quoted && !strcmp(v, "null"))) return;
    if (b->item_d && d == b->item_d) sjb_field(b->l, &b->cur, k, v);
    else if (b->head_d && d == b->head_d && !b->item_d) sjb_field(b->l, &b->l->head, k, v);
}

/* ---------------------------------------------------------------- servers */
static void sj_srv_load(void)
{
    if (sj.srv_read) return;
    sj.srv_read = true;
    sj.n_srv = 0;
    size_t len;
    char *b = pj_slurp(SJ_SERVERS, 64 * 1024, &len);
    if (!b) return;
    char *s = b, *line;
    bool first = true;
    while ((line = strsep(&s, "\n")))
    {
        if (first)
        {
            first = false;
            if (strncmp(line, "SUB1\t", 5)) break;
            sj.active = (uint32_t)strtoul(line + 5, NULL, 16);
            continue;
        }
        if (!line[0] || sj.n_srv >= SL_SUB_SERVERS) continue;
        char *f = line;
        struct sj_srv *v = &sj.srv[sj.n_srv];
        memset(v, 0, sizeof *v);
        v->pub.id = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        snprintf(v->pub.url, sizeof v->pub.url, "%s", pj_next(&f));
        snprintf(v->pub.user, sizeof v->pub.user, "%s", pj_next(&f));
        int flags = atoi(pj_next(&f));
        v->pub.api_key = flags & 1; v->pub.plain = flags & 2; v->pub.open = flags & 4;
        snprintf(v->pub.kind, sizeof v->pub.kind, "%s", pj_next(&f));
        snprintf(v->pub.version, sizeof v->pub.version, "%s", pj_next(&f));
        snprintf(v->pub.name, sizeof v->pub.name, "%s", pj_next(&f));
        if (v->pub.id && v->pub.url[0] && sj_unhex(v->secret, sizeof v->secret, pj_next(&f))) sj.n_srv++;
    }
    memset(b, 0, len);
    free(b);
    bool found = false;
    for (int i = 0; i < sj.n_srv; i++) found |= sj.srv[i].pub.id == sj.active;
    if (!found) sj.active = sj.n_srv ? sj.srv[0].pub.id : 0;
}
/* the servers file holds the passwords: the owner's only, written aside then put in place, the buffer wiped */
static bool sj_srv_save(void)
{
    if (sj.hold) return false;
    struct pj_buf o = { 0 };
    pj_add(&o, "SUB1\t%08x\n", (unsigned)sj.active);
    for (int i = 0; i < sj.n_srv; i++)
    {
        const struct sj_srv *v = &sj.srv[i];
        char a[4][200], hex[2 * sizeof v->secret + 1];
        sj_hex(hex, (const unsigned char *)v->secret, strlen(v->secret));
        pj_add(&o, "%08x\t%s\t%s\t%d\t%s\t%s\t%s\t%s\n", (unsigned)v->pub.id, v->pub.url, pj_f(v->pub.user, a[0], sizeof a[0]),
               (v->pub.api_key ? 1 : 0) | (v->pub.plain ? 2 : 0) | (v->pub.open ? 4 : 0), pj_f(v->pub.kind, a[1], 24),
               pj_f(v->pub.version, a[2], 16), pj_f(v->pub.name, a[3], sizeof a[3]), hex);
        memset(hex, 0, sizeof hex);
    }
    bool ok = false;
    char p[512], tmp[520];
    pj_mkdirs(SL_SUB_STATE);
    if (!o.bad && o.b && pj_real(p, sizeof p, SJ_SERVERS) && snprintf(tmp, sizeof tmp, "%s.new", p) < (int)sizeof tmp)
    {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0)
        {
            ok = write(fd, o.b, o.n) == (ssize_t)o.n && fsync(fd) == 0;
            if (close(fd)) ok = false;
            if (ok) ok = rename(tmp, p) == 0;
            if (!ok) unlink(tmp);
        }
    }
    if (o.b) { memset(o.b, 0, o.cap); free(o.b); }
    sj.st.servers_seq++;
    sj.st.seq++;
    return ok;
}
static struct sj_srv *sj_srv_find(uint32_t id)
{
    sj_srv_load();
    if (!id) id = sj.active;
    for (int i = 0; i < sj.n_srv; i++) if (sj.srv[i].pub.id == id) return &sj.srv[i];
    return NULL;
}

/* ---------------------------------------------------------------- settings */
static void sj_prefs_load(void)
{
    if (sj.prefs_read) return;
    sj.prefs_read = true;
    sj.prefs.quality = SL_SUB_Q_ORIGINAL;
    sj.prefs.limit_gb = 2;
    char *b = pj_slurp(SJ_PREFS, 256, NULL);
    int q, g;
    if (b && sscanf(b, "SUBP %d %d", &q, &g) == 2 && q >= 0 && q <= 2 && g >= 1 && g <= 64) { sj.prefs.quality = q; sj.prefs.limit_gb = g; }
    free(b);
}

/* ---------------------------------------------------------------- the cache */
static void sj_song_free(struct sj_song *s) { free(s->f[0]); memset(s, 0, sizeof *s); }
static bool sj_song_set(struct sj_song *s, const char *const v[SF_N])
{
    size_t n = 0;
    for (int i = 0; i < SF_N; i++) n += strlen(v[i] ? v[i] : "") + 1;
    char *b = malloc(n), *w = b;
    if (!b) return false;
    for (int i = 0; i < SF_N; i++) { s->f[i] = w; w = pj_put(w, v[i] ? v[i] : ""); }
    return true;
}
static void sj_cache_load(void)
{
    if (sj.cache_read) return;
    sj.cache_read = true;
    size_t len;
    char *b = pj_slurp(SJ_CACHE, 8 * 1024 * 1024, &len);
    if (!b) return;
    char *s = b, *line;
    bool first = true;
    while ((line = strsep(&s, "\n")))
    {
        if (first) { first = false; if (strncmp(line, "SUBC\t", 5)) break; sj.clock = (uint32_t)strtoul(line + 5, NULL, 10); continue; }
        if (!line[0] || sj.n_cache >= SJ_CACHE_MAX) continue;
        if (sj.n_cache == sj.cap_cache)
        {
            int want = sj.cap_cache ? sj.cap_cache * 2 : 32;
            struct sj_song *c = realloc(sj.cache, (size_t)want * sizeof *c);
            if (!c) break;
            sj.cache = c;
            sj.cap_cache = want;
        }
        char *f = line;
        struct sj_song e;
        memset(&e, 0, sizeof e);
        e.server = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        e.size_kb = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        e.used = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        e.pinned = (uint8_t)atoi(pj_next(&f));
        e.track = (uint16_t)atoi(pj_next(&f));
        e.disc = (uint16_t)atoi(pj_next(&f));
        e.dur = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        e.starred = (uint8_t)atoi(pj_next(&f));
        const char *v[SF_N];
        for (int i = 0; i < SF_N; i++) v[i] = pj_next(&f);
        if (!v[SF_ID][0] || !v[SF_REL][0] || !sj_song_set(&e, v)) continue;
        sj.cache[sj.n_cache++] = e;
        sj.used_kb += e.size_kb;
    }
    free(b);
}
static void sj_cache_save(void)
{
    if (sj.hold) { sj.cache_dirty = true; return; }
    struct pj_buf o = { 0 };
    pj_add(&o, "SUBC\t%u\n", (unsigned)sj.clock);
    for (int i = 0; i < sj.n_cache; i++)
    {
        const struct sj_song *e = &sj.cache[i];
        pj_add(&o, "%08x\t%u\t%u\t%d\t%d\t%d\t%u\t%d", (unsigned)e->server, (unsigned)e->size_kb, (unsigned)e->used, e->pinned,
               e->track, e->disc, (unsigned)e->dur, e->starred);
        for (int k = 0; k < SF_N; k++) { char f[400]; pj_add(&o, "\t%s", pj_f(e->f[k], f, sizeof f)); }
        pj_add(&o, "\n");
    }
    pj_mkdirs(SL_SUB_STATE);
    sj.cache_dirty = o.bad || !o.b || !pj_write(SJ_CACHE, o.b, o.n);
    free(o.b);
}
static struct sj_song *sj_cache_find(uint32_t server, const char *id)
{
    sj_cache_load();
    for (int i = 0; i < sj.n_cache; i++)
        if (sj.cache[i].server == server && !strcmp(sj.cache[i].f[SF_ID], id)) return &sj.cache[i];
    return NULL;
}
static struct sj_song *sj_cache_rel(const char *rel)
{
    sj_cache_load();
    for (int i = 0; i < sj.n_cache; i++) if (!strcmp(sj.cache[i].f[SF_REL], rel)) return &sj.cache[i];
    return NULL;
}
static bool sj_rb_path(char *out, size_t n, const char *rel) { return snprintf(out, n, "%s/%s", sl_sub_folder, rel) < (int)n; }
/* a song's file and the folders it leaves empty (an album's cover.jpg with them) */
static void sj_cache_drop(int i)
{
    struct sj_song *e = &sj.cache[i];
    char rb[600], real[700];
    if (sj_rb_path(rb, sizeof rb, e->f[SF_REL]) && pj_real(real, sizeof real, rb))
    {
        unlink(real);
        char *sl = strrchr(real, '/');
        if (sl)
        {
            /* the album's folder: anything of ours still in it? */
            *sl = '\0';
            bool other = false;
            size_t dl = strlen(e->f[SF_REL]) - strlen(strrchr(e->f[SF_REL], '/') ? strrchr(e->f[SF_REL], '/') : "");
            for (int k = 0; k < sj.n_cache && !other; k++)
                if (k != i && !strncmp(sj.cache[k].f[SF_REL], e->f[SF_REL], dl) && sj.cache[k].f[SF_REL][dl] == '/') other = true;
            if (!other)
            {
                char cov[720];
                snprintf(cov, sizeof cov, "%s/cover.jpg", real);
                unlink(cov);
                rmdir(real);
                if ((sl = strrchr(real, '/'))) { *sl = '\0'; rmdir(real); }   /* the artist's, when it was the last */
            }
        }
    }
    sj.used_kb -= e->size_kb;
    if (sj.used_kb < 0) sj.used_kb = 0;
    sj_song_free(e);
    sj.cache[i] = sj.cache[--sj.n_cache];
}
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
unsigned sl_sub_test_limit_kb;
#endif
static long long sj_limit_kb(void)
{
    sj_prefs_load();
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
    if (sl_sub_test_limit_kb) return sl_sub_test_limit_kb;
#endif
    return (long long)sj.prefs.limit_gb * 1024 * 1024;
}
/* room for need_kb more: the least recently played go, never one kept offline or the play's own */
static bool sj_make_room(uint32_t need_kb)
{
    /* what could go, first: when even all of it is not room enough, nothing goes */
    uint64_t could = 0;
    for (int i = 0; i < sj.n_cache; i++)
        if (!sj.cache[i].pinned && !(sj.session && sj.cache[i].sess == sj.session)) could += sj.cache[i].size_kb;
    if ((uint64_t)sj.used_kb + need_kb > (uint64_t)sj_limit_kb() + could) return false;
    bool gone = false;
    while (sj.used_kb + need_kb > sj_limit_kb())
    {
        int best = -1;
        for (int i = 0; i < sj.n_cache; i++)
        {
            const struct sj_song *e = &sj.cache[i];
            if (e->pinned || (sj.session && e->sess == sj.session)) continue;
            if (best < 0 || e->used < sj.cache[best].used) best = i;
        }
        if (best < 0) break;
        sj_cache_drop(best);
        gone = true;
    }
    if (gone) { sj_cache_save(); sj.st.cache_seq++; sj.st.seq++; }
    return sj.used_kb + need_kb <= sj_limit_kb();
}

/* ---------------------------------------------------------------- kept offline */
static void sj_pins_load(void)
{
    if (sj.pins_read) return;
    sj.pins_read = true;
    size_t len;
    char *b = pj_slurp(SJ_PINS, 2 * 1024 * 1024, &len);
    if (!b) return;
    sj.pins = calloc(SJ_PIN_MAX, sizeof *sj.pins);
    char *s = b, *line;
    while (sj.pins && (line = strsep(&s, "\n")))
    {
        if (!line[0] || sj.n_pins >= SJ_PIN_MAX) continue;
        char *f = line;
        struct sj_pin *p = &sj.pins[sj.n_pins];
        p->server = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        p->kind = (uint8_t)atoi(pj_next(&f));
        snprintf(p->id, sizeof p->id, "%s", pj_next(&f));
        snprintf(p->name, sizeof p->name, "%s", pj_next(&f));
        char *ids = pj_next(&f);
        size_t n = strlen(ids);
        p->ids = malloc(n + 2);
        if (!p->ids || !p->id[0]) { free(p->ids); p->ids = NULL; continue; }
        p->n = 0;
        char *w = p->ids, *id;
        while ((id = strsep(&ids, ",")))
            if (id[0]) { w = pj_put(w, id); p->n++; }
        *w = '\0';
        sj.n_pins++;
    }
    free(b);
}
static void sj_pins_save(void)
{
    if (sj.hold) return;
    struct pj_buf o = { 0 };
    for (int i = 0; i < sj.n_pins; i++)
    {
        const struct sj_pin *p = &sj.pins[i];
        char nm[100];
        pj_add(&o, "%08x\t%d\t%s\t%s\t", (unsigned)p->server, p->kind, p->id, pj_f(p->name, nm, sizeof nm));
        const char *id = p->ids;
        for (int k = 0; k < p->n; k++, id += strlen(id) + 1) pj_add(&o, "%s%s", k ? "," : "", id);
        pj_add(&o, "\n");
    }
    pj_mkdirs(SL_SUB_STATE);
    if (!o.b) { char p[512]; if (pj_real(p, sizeof p, SJ_PINS)) unlink(p); }
    else if (!o.bad) pj_write(SJ_PINS, o.b, o.n);
    free(o.b);
}
static struct sj_pin *sj_pin_find(uint32_t server, int kind, const char *id)
{
    sj_pins_load();
    for (int i = 0; i < sj.n_pins; i++)
        if (sj.pins[i].server == server && sj.pins[i].kind == kind && !strcmp(sj.pins[i].id, id)) return &sj.pins[i];
    return NULL;
}
static bool sj_pinned_song(uint32_t server, const char *id)
{
    sj_pins_load();
    for (int i = 0; i < sj.n_pins; i++)
    {
        if (sj.pins[i].server != server) continue;
        const char *s = sj.pins[i].ids;
        for (int k = 0; k < sj.pins[i].n; k++, s += strlen(s) + 1) if (!strcmp(s, id)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- downloads */
static void sj_q_drop(int k)
{
    sj_song_free(&sj.q[k].s);
    memmove(&sj.q[k], &sj.q[k + 1], (size_t)(sj.n_q - k - 1) * sizeof *sj.q);
    sj.n_q--;
}
static void sj_queue_save(void)
{
    if (sj.hold) return;
    struct pj_buf o = { 0 };
    for (int i = 0; i < sj.n_q; i++)
    {
        const struct sj_dl *q = &sj.q[i];
        if (!q->pin) continue;
        pj_add(&o, "%08x\t%u\t%d\t%d\t%u", (unsigned)q->s.server, (unsigned)q->est_kb, q->s.track, q->s.disc, (unsigned)q->s.dur);
        for (int k = 0; k < SF_N; k++) { char f[400]; pj_add(&o, "\t%s", pj_f(q->s.f[k], f, sizeof f)); }
        pj_add(&o, "\n");
    }
    char p[512];
    if (!o.b) { if (pj_real(p, sizeof p, SJ_QUEUE)) unlink(p); }
    else if (!o.bad) { pj_mkdirs(SL_SUB_STATE); pj_write(SJ_QUEUE, o.b, o.n); }
    free(o.b);
}
static bool sj_q_room(void)
{
    if (sj.n_q < sj.cap_q) return true;
    int want = sj.cap_q ? sj.cap_q * 2 : 16;
    struct sj_dl *q = realloc(sj.q, (size_t)want * sizeof *q);
    if (!q) return false;
    sj.q = q;
    sj.cap_q = want;
    return true;
}
static void sj_queue_load(void)
{
    if (sj.q_read) return;
    sj.q_read = true;
    char *b = pj_slurp(SJ_QUEUE, 2 * 1024 * 1024, NULL);
    char *s = b, *line;
    while (b && (line = strsep(&s, "\n")))
    {
        if (!line[0] || !sj_q_room()) continue;
        char *f = line;
        struct sj_dl q;
        memset(&q, 0, sizeof q);
        q.pin = true;
        q.s.server = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        q.est_kb = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        q.s.track = (uint16_t)atoi(pj_next(&f));
        q.s.disc = (uint16_t)atoi(pj_next(&f));
        q.s.dur = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        const char *v[SF_N];
        for (int i = 0; i < SF_N; i++) v[i] = pj_next(&f);
        q.s.pinned = 1;
        if (v[SF_ID][0] && v[SF_REL][0] && sj_song_set(&q.s, v)) sj.q[sj.n_q++] = q;
    }
    free(b);
    sj.st.queued = sj.n_q;
}
static bool sj_dest_real(const char *rel, char *out, size_t n, bool part)
{
    char rb[600];
    return sj_rb_path(rb, sizeof rb, rel) && pj_real(out, n, rb) && (!part || strlen(out) + 6 < n) && (!part || strcat(out, ".part"));
}
static void sj_dl_end(bool keep_part)
{
    if (!sj.dl_on) return;
    sl_net_dl_stop(&sj.dl);
    if (!keep_part) unlink(sj.dl.path);
    sj.dl_on = false;
    sj.st.dl_server = 0;
    sj.st.dl_id[0] = sj.st.dl_title[0] = '\0';
    sj.st.dl_got = sj.st.dl_total = 0;
    sj.st.seq++;
}
/* a request's address: the method, our sign-in, the rest; false when it would not fit */
static bool sj_url(char *out, size_t n, const struct sj_srv *s, const char *method, const char *params, bool anon)
{
    char auth[400], salt[17], tok[33], u[200];
    lj_enc(u, sizeof u, s->pub.user);
    if (anon) auth[0] = '\0';
    else if (s->pub.api_key)
    {
        char k[400];
        lj_enc(k, sizeof k, s->secret);
        snprintf(auth, sizeof auth, "apiKey=%s&", k);
        memset(k, 0, sizeof k);
    }
    else if (s->pub.plain)
    {
        char hex[2 * sizeof s->secret + 1];
        sj_hex(hex, (const unsigned char *)s->secret, strlen(s->secret));
        snprintf(auth, sizeof auth, "u=%s&p=enc:%s&", u, hex);
        memset(hex, 0, sizeof hex);
    }
    else
    {
        sj_salt(salt);
        sl_sub_token(s->secret, salt, tok);
        snprintf(auth, sizeof auth, "u=%s&t=%s&s=%s&", u, tok, salt);
    }
    int k = snprintf(out, n, "%s/rest/%s?%sv=" SL_SUB_API "&c=GabagoolOS&f=json%s%s", s->pub.url, method, auth,
                     params && params[0] ? "&" : "", params ? params : "");
    memset(auth, 0, sizeof auth);
    return k > 0 && k < (int)n;
}
static void sj_dl_start_next(void)
{
    if (sj.dl_on || !sj.n_q || sj.hold || !sj.online || (sj.dl_retry_at && (int32_t)(sj_ms() - sj.dl_retry_at) < 0)) return;
    struct sj_dl *q = &sj.q[0];
    struct sj_srv *s = sj_srv_find(q->s.server);
    if (!s) { sj_q_drop(0); sj_queue_save(); sj.st.queued = sj.n_q; sj.st.seq++; return; }
    if (sj_cache_find(q->s.server, q->s.f[SF_ID])) { sj_q_drop(0); sj_queue_save(); sj.st.queued = sj.n_q; sj.st.cache_seq++; sj.st.seq++; return; }
    if (!sj_make_room(q->est_kb))
    {
        /* everything there is kept offline, or this play's own: it does not come */
        sj.st.dl_err = SL_SUB_E_FULL;
        sj.st.dl_seq++;
        sj_q_drop(0);
        sj_queue_save();
        sj.st.queued = sj.n_q;
        sj.st.seq++;
        return;
    }
    char params[200], id[160], url[1024], part[700], rb[600];
    lj_enc(id, sizeof id, q->s.f[SF_ID]);
    sj_prefs_load();
    if (sj.prefs.quality == SL_SUB_Q_MP3) snprintf(params, sizeof params, "id=%s&format=mp3&maxBitRate=320", id);
    else if (sj.prefs.quality == SL_SUB_Q_OPUS) snprintf(params, sizeof params, "id=%s&format=opus&maxBitRate=128", id);
    else snprintf(params, sizeof params, "id=%s&format=raw", id);
    if (!sj_url(url, sizeof url, s, "stream", params, false) || !sj_dest_real(q->s.f[SF_REL], part, sizeof part, true))
    { sj_q_drop(0); return; }
    snprintf(rb, sizeof rb, "%s/%s", sl_sub_folder, q->s.f[SF_REL]);
    char *sl = strrchr(rb, '/');
    if (sl) { *sl = '\0'; pj_mkdirs(rb); }
    bool started = sl_net_dl_start(&sj.dl, url, SL_SUB_UA, part);
    memset(url, 0, sizeof url);
    if (!started) { sj.dl_fails++; sj.dl_retry_at = sj_ms() + 30000; return; }
    sj.dl_on = true;
    sj.st.dl_server = q->s.server;
    snprintf(sj.st.dl_id, sizeof sj.st.dl_id, "%s", q->s.f[SF_ID]);
    snprintf(sj.st.dl_title, sizeof sj.st.dl_title, "%s", q->s.f[SF_TITLE]);
    sj.st.dl_total = (long long)q->est_kb * 1024;
    sj.st.dl_got = 0;
    sj.st.queued = sj.n_q - 1;
    sj.st.seq++;
}
/* a file off the card (in /tmp), whole, if it is at most cap bytes */
static char *sj_tmp_read(const char *path, size_t cap, size_t *n)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    char *b = fd >= 0 && !fstat(fd, &st) && st.st_size > 0 && (size_t)st.st_size <= cap ? malloc((size_t)st.st_size) : NULL;
    *n = b && read(fd, b, (size_t)st.st_size) == (ssize_t)st.st_size ? (size_t)st.st_size : 0;
    if (fd >= 0) close(fd);
    if (!*n) { free(b); b = NULL; }
    return b;
}
/* the album's cover beside its songs, for Now Playing and Files: once, small, never in the way */
static void sj_folder_cover(const struct sj_srv *s, const char *rel, const char *cover)
{
    if (!cover[0]) return;
    char rb[600], real[700], tmp[720], params[200], id[160], url[1024];
    snprintf(rb, sizeof rb, "%s/%s", sl_sub_folder, rel);
    char *sl = strrchr(rb, '/');
    if (!sl) return;
    snprintf(sl, sizeof rb - (size_t)(sl - rb), "/cover.jpg");
    struct stat st;
    if (!pj_real(real, sizeof real, rb) || !stat(real, &st)) return;
    lj_enc(id, sizeof id, cover);
    snprintf(params, sizeof params, "id=%s&size=500", id);
    /* into memory (wget writes where it is told, past the card's guard), then onto the card
     * while it is ours: USB mode can start at any time during the download */
    snprintf(tmp, sizeof tmp, "/tmp/sloppy-sub-cover.part");
    if (!sj_url(url, sizeof url, s, "getCoverArt", params, false)) return;
    pthread_mutex_unlock(&sj_lock);
    int status = sl_net_http_get(url, SL_SUB_UA, NULL, 0, tmp, 15000);
    size_t n = 0;
    char *data = status == 200 ? sj_tmp_read(tmp, 2u << 20, &n) : NULL;
    unlink(tmp);
    pthread_mutex_lock(&sj_lock);
    memset(url, 0, sizeof url);
    /* a picture, not an error answer (a server says "not found" with a 200 and some JSON) */
    bool pic = data && n > 100 && (((unsigned char)data[0] == 0xff && (unsigned char)data[1] == 0xd8) || !memcmp(data, "\x89PNG", 4));
    if (pic && !sj.hold) pj_write(rb, data, n);
    free(data);
}
/* the file a download made is Subsonic's error answer, not audio */
static bool sj_is_answer(const char *path)
{
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > 16384) return false;
    char b[256];
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return false;
    b[n] = '\0';
    const char *p = b;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return (*p == '{' || *p == '<') && (strstr(b, "subsonic-response") || strstr(b, "\"error\""));
}
static void sj_dl_step(void)
{
    if (!sj.dl_on) return;
    struct stat st;
    if (!stat(sj.dl.path, &st) && (long long)st.st_size != sj.st.dl_got)
    {
        sj.st.dl_got = (long long)st.st_size;
        if (sj.st.dl_total < sj.st.dl_got) sj.st.dl_total = 0;
        sj.st.seq++;
    }
    int r = sl_net_dl_poll(&sj.dl);
    if (r == 0) return;
    struct sj_dl q = sj.q[0];
    bool whole = r == 200 || r == 206;
    char part[700], fin[700];
    sj_dest_real(q.s.f[SF_REL], part, sizeof part, true);
    sj_dest_real(q.s.f[SF_REL], fin, sizeof fin, false);
    /* Subsonic says "not found" or "can't transcode" with a 200 and a small
     * JSON or XML answer: that is no song, and must not be kept as one */
    if (whole && sj_is_answer(part)) r = 404;
    whole = r == 200 || r == 206;
    sj.dl_on = false;
    sj.st.dl_server = 0;
    sj.st.dl_id[0] = sj.st.dl_title[0] = '\0';
    sj.st.dl_got = sj.st.dl_total = 0;
    if (whole && !sj.hold && !stat(part, &st) && st.st_size > 0 && !rename(part, fin))
    {
        memmove(&sj.q[0], &sj.q[1], (size_t)(sj.n_q - 1) * sizeof *sj.q);
        sj.n_q--;
        struct sj_song e = q.s;           /* its block moves to the cache */
        e.size_kb = (uint32_t)((st.st_size + 1023) / 1024);
        e.used = ++sj.clock;
        e.sess = q.session;
        e.pinned = q.pin || sj_pinned_song(e.server, e.f[SF_ID]);
        bool room = sj.n_cache < sj.cap_cache;
        if (!room && sj.n_cache < SJ_CACHE_MAX)
        {
            int want = sj.cap_cache ? sj.cap_cache * 2 : 32;
            struct sj_song *c = realloc(sj.cache, (size_t)want * sizeof *c);
            if (c) { sj.cache = c; sj.cap_cache = want; room = true; }
        }
        if (room)
        {
            sj.cache[sj.n_cache++] = e;
            sj.used_kb += e.size_kb;
            sj_cache_save();
            struct sj_srv *sv = sj_srv_find(e.server);
            if (sv)
            {
                /* copies: the lock is let go while it comes */
                struct sj_srv s = *sv;
                char rel[400], cover[48];
                snprintf(rel, sizeof rel, "%s", e.f[SF_REL]);
                snprintf(cover, sizeof cover, "%s", e.f[SF_COVER]);
                sj_folder_cover(&s, rel, cover);
                memset(&s, 0, sizeof s);
            }
        }
        else { unlink(fin); sj_song_free(&e); }
        sj.dl_fails = 0;
        sj.st.dl_err = room ? SL_SUB_OK : SL_SUB_E_NOMEM;
        sj.st.cache_seq++;
    }
    else if (r == 404 || r == 410 || r == 403 || r == 401)
    {
        unlink(part);
        sj_q_drop(0);
        sj.st.dl_err = r == 401 || r == 403 ? SL_SUB_E_AUTH : SL_SUB_E_NOTFOUND;
    }
    else if (!sj.hold)
    {
        /* cut off: its part stays, it carries on soon */
        sj.dl_fails++;
        sj.dl_retry_at = sj_ms() + (sj.dl_fails < 3 ? 3000u : 30000u);
        sj.st.dl_err = SL_SUB_E_NET;
        if (sj.dl_fails >= 6)
        {
            struct sj_dl t = sj.q[0];
            memmove(&sj.q[0], &sj.q[1], (size_t)(sj.n_q - 1) * sizeof *sj.q);
            sj.q[sj.n_q - 1] = t;
            sj.dl_fails = 0;
        }
    }
    sj_queue_save();
    sj.st.queued = sj.n_q;
    sj.st.dl_seq++;
    sj.st.seq++;
}

/* ---------------------------------------------------------------- a request */
struct sj_mem_sink { struct sj_json j; struct sj_build b; };
static bool sj_json_sink(void *ctx, const char *data, size_t n)
{
    struct sj_mem_sink *m = ctx;
    sj_feed(&m->j, &m->b, data, n);
    return !m->j.bad;
}
/* one call, read as it comes; the error, SL_SUB_OK when it said ok */
static int sj_call(const struct sj_srv *s, const char *method, const char *params, bool anon, struct sl_sub_list *l, struct sj_build *out)
{
    char url[1400];
    if (!sj_url(url, sizeof url, s, method, params, anon)) return SL_SUB_E_ADDRESS;
    struct sj_mem_sink *m = calloc(1, sizeof *m);
    if (!m) return SL_SUB_E_NOMEM;
    m->b.l = l;
    int status = sl_net_http_stream(url, SL_SUB_UA, 20000, sj_json_sink, m, &sj.stop_job);
    memset(url, 0, sizeof url);
    if (m->j.in_lit) sj_lit_end(&m->j, &m->b);
    int err;
    if (status == 401 || status == 403) err = SL_SUB_E_AUTH;
    else if (status == 404) err = SL_SUB_E_NOTSUB;
    else if (status < 0) err = SL_SUB_E_NET;
    else if (status != 200) err = m->b.resp ? SL_SUB_E_SERVER : SL_SUB_E_NOTSUB;
    else if (!m->b.resp) err = SL_SUB_E_NOTSUB;
    else if (!m->b.failed) err = SL_SUB_OK;
    else if (m->b.code == 40 || m->b.code == 44) err = SL_SUB_E_AUTH;
    else if (m->b.code == 41 || m->b.code == 42) err = SL_SUB_E_TOKEN;
    else if (m->b.code == 70) err = SL_SUB_E_NOTFOUND;
    else err = SL_SUB_E_SERVER;
    if (out) *out = m->b;
    if (l && err == SL_SUB_E_SERVER) snprintf(l->why, sizeof l->why, "%s", m->b.msg);
    free(m);
    return err;
}

/* ---------------------------------------------------------------- the jobs */
static void sj_signin(struct sj_job *j)
{
    struct sj_srv t;
    memset(&t, 0, sizeof t);
    int err = SL_SUB_OK;
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *old = j->id ? sj_srv_find(j->id) : NULL;
    if (!j->secret[0] && old) snprintf(j->secret, sizeof j->secret, "%s", old->secret);
    if (old) t.pub = old->pub;
    /* its id, kept: the lock is let go for the pings, and a server removed meanwhile moves the list under old */
    uint32_t old_id = old ? old->pub.id : 0;
    bool had_old = old != NULL;
    old = NULL;
    pthread_mutex_unlock(&sj_lock);
    if (!sl_sub_url_fix(j->url, t.pub.url, sizeof t.pub.url)) err = SL_SUB_E_ADDRESS;
    snprintf(t.pub.user, sizeof t.pub.user, "%s", j->user);
    snprintf(t.secret, sizeof t.secret, "%s", j->secret);
    memset(j->secret, 0, sizeof j->secret);
    t.pub.api_key = t.pub.plain = false;
    struct sj_build b;
    memset(&b, 0, sizeof b);
    if (!err) err = sj_call(&t, "ping", NULL, false, NULL, &b);
    /* an OpenSubsonic server may take an API key where the password goes: tried when it refuses
     * tokens for this user, or over HTTPS after a wrong password (a key is long; over plain http
     * a mistyped password would travel as it is, so not there) */
    if (err == SL_SUB_E_TOKEN || (err == SL_SUB_E_AUTH && !strncasecmp(t.pub.url, "https://", 8) && strlen(t.secret) >= 16))
    {
        struct sj_build x;
        memset(&x, 0, sizeof x);
        if (sj_call(&t, "getOpenSubsonicExtensions", NULL, true, NULL, &x) == SL_SUB_OK && x.ext_api)
        {
            t.pub.api_key = true;
            int e2 = sj_call(&t, "ping", NULL, false, NULL, &b);
            if (e2 == SL_SUB_OK) err = SL_SUB_OK;
            else t.pub.api_key = false;
        }
    }
    if (err == SL_SUB_E_TOKEN && !strncasecmp(t.pub.url, "https://", 8))
    {
        /* no tokens (an LDAP account): the old way, the password itself, only ever over HTTPS */
        t.pub.plain = true;
        if (sj_call(&t, "ping", NULL, false, NULL, &b) == SL_SUB_OK) err = SL_SUB_OK;
        else t.pub.plain = false;
    }
    if (!err)
    {
        snprintf(t.pub.kind, sizeof t.pub.kind, "%s", b.kind);
        snprintf(t.pub.version, sizeof t.pub.version, "%s", b.version);
        t.pub.open = b.open;
    }
    pthread_mutex_lock(&sj_lock);
    sj.st.test_err = err;
    sj.st.test_api_key = t.pub.api_key;
    sj.st.test_plain = t.pub.plain;
    sj.st.test_open = t.pub.open;
    snprintf(sj.st.test_kind, sizeof sj.st.test_kind, "%s", t.pub.kind);
    snprintf(sj.st.test_version, sizeof sj.st.test_version, "%s", t.pub.version);
    snprintf(sj.st.test_why, sizeof sj.st.test_why, "%s", err == SL_SUB_E_SERVER ? b.msg : "");
    sj.st.test_saved = 0;
    if (!err && j->save && !sj.hold)
    {
        char key[300];
        snprintf(key, sizeof key, "%s\n%s", t.pub.url, t.pub.user);
        t.pub.id = pj_fnv(key);
        /* its name: what it is, or where */
        if (!t.pub.name[0] || (had_old && old_id != t.pub.id))
        {
            const char *host = strstr(t.pub.url, "://");
            host = host ? host + 3 : t.pub.url;
            if (t.pub.kind[0] && strcasecmp(t.pub.kind, "subsonic"))
            {
                snprintf(t.pub.name, sizeof t.pub.name, "%s", t.pub.kind);
                t.pub.name[0] = (char)toupper((unsigned char)t.pub.name[0]);
            }
            else snprintf(t.pub.name, sizeof t.pub.name, "%.*s", (int)strcspn(host, "/:"), host);
        }
        int at = -1;
        for (int i = 0; i < sj.n_srv; i++) if (sj.srv[i].pub.id == t.pub.id || (had_old && sj.srv[i].pub.id == old_id)) at = i;
        if (at < 0 && sj.n_srv < SL_SUB_SERVERS) at = sj.n_srv++;
        if (at >= 0)
        {
            sj.srv[at] = t;
            sj.active = t.pub.id;
            if (sj_srv_save()) sj.st.test_saved = t.pub.id;
        }
        else sj.st.test_err = SL_SUB_E_FULL;
    }
    memset(&t, 0, sizeof t);
    sj.st.test_seq++;
    sj.st.seq++;
    pthread_mutex_unlock(&sj_lock);
}
/* songs and albums say whether they are on the card, and kept */
static void sj_mark(struct sl_sub_list *l)
{
    for (int i = -1; i < l->n; i++)
    {
        struct sl_sub_item *it = i < 0 ? &l->head : &l->it[i];
        const char *id = sl_sub_s(l, it->id);
        if (!id[0]) continue;
        if (it->kind == SL_SUB_SONG)
        {
            struct sj_song *e = sj_cache_find(l->server, id);
            it->cached = e != NULL;
            it->pinned = e && e->pinned;
        }
        else if (it->kind == SL_SUB_ALBUM)
        {
            it->cached = 0;
            for (int k = 0; k < sj.n_cache && !it->cached; k++)
                it->cached = sj.cache[k].server == l->server && !strcmp(sj.cache[k].f[SF_ALBUM_ID], id);
            it->pinned = sj_pin_find(l->server, SL_SUB_ALBUM, id) != NULL;
        }
        else if (it->kind == SL_SUB_PLAYLIST) it->pinned = sj_pin_find(l->server, SL_SUB_PLAYLIST, id) != NULL;
    }
}
static int sj_song_cmp(const void *a, const void *b)
{
    const struct sl_sub_item *x = a, *y = b;
    if (x->disc != y->disc) return x->disc - y->disc;
    return x->track - y->track;
}
static void sj_item_from(struct sl_sub_list *l, struct sl_sub_item *it, const struct sj_song *e)
{
    memset(it, 0, sizeof *it);
    it->kind = SL_SUB_SONG;
    it->cached = 1;
    it->pinned = e->pinned;
    it->starred = e->starred;
    it->track = e->track;
    it->disc = e->disc;
    it->dur = e->dur;
    it->size = e->size_kb > 0x1fffff ? 0x7fffffff : e->size_kb * 1024;
    it->id = sj_pool(l, e->f[SF_ID]);
    it->album_id = sj_pool(l, e->f[SF_ALBUM_ID]);
    it->cover = sj_pool(l, e->f[SF_COVER]);
    it->title = sj_pool(l, e->f[SF_TITLE]);
    it->artist = sj_pool(l, e->f[SF_ARTIST]);
    it->album = sj_pool(l, e->f[SF_ALBUM]);
    it->suffix = sj_pool(l, e->f[SF_SUFFIX]);
}
static bool sj_list_room(struct sl_sub_list *l)
{
    if (l->n < l->cap) return true;
    if (l->cap >= SL_SUB_LIST_MAX) return false;
    int want = l->cap ? l->cap * 2 : 32;
    struct sl_sub_item *it = realloc(l->it, (size_t)want * sizeof *it);
    if (!it) return false;
    l->it = it;
    l->cap = want;
    return true;
}
/* what is on the card, as the server would have said it: locked */
static int sj_from_cache(int what, const char *arg, struct sl_sub_list *l)
{
    sj_cache_load();
    sj_pins_load();
    l->offline = true;
    if (what == SL_SUB_Q_OFFLINE)
    {
        /* the albums, the latest played first */
        uint32_t *last = NULL;
        for (int i = 0; i < sj.n_cache; i++)
        {
            const struct sj_song *e = &sj.cache[i];
            if (e->server != l->server) continue;
            int at = -1;
            for (int k = 0; k < l->n && at < 0; k++) if (!strcmp(sl_sub_s(l, l->it[k].id), e->f[SF_ALBUM_ID])) at = k;
            if (at < 0)
            {
                if (!sj_list_room(l)) break;
                uint32_t *nl = realloc(last, (size_t)l->cap * sizeof *nl);
                if (!nl) break;
                last = nl;
                struct sl_sub_item *it = &l->it[l->n];
                memset(it, 0, sizeof *it);
                it->kind = SL_SUB_ALBUM;
                it->cached = 1;
                it->id = sj_pool(l, e->f[SF_ALBUM_ID]);
                it->title = sj_pool(l, e->f[SF_ALBUM]);
                it->artist = sj_pool(l, e->f[SF_ARTIST]);
                it->cover = sj_pool(l, e->f[SF_COVER]);
                it->pinned = sj_pin_find(l->server, SL_SUB_ALBUM, e->f[SF_ALBUM_ID]) != NULL;
                last[l->n] = 0;
                at = l->n++;
            }
            l->it[at].count++;
            l->it[at].dur += e->dur;
            l->it[at].size += e->size_kb > 0x1fffff ? 0x7fffffff : e->size_kb * 1024;
            if (e->used > last[at]) last[at] = e->used;
        }
        /* the latest first: a few hundred albums at most, sorted in place */
        for (int a = 1; a < l->n; a++)
            for (int b = a; b > 0 && last[b] > last[b - 1]; b--)
            {
                struct sl_sub_item t = l->it[b]; l->it[b] = l->it[b - 1]; l->it[b - 1] = t;
                uint32_t u = last[b]; last[b] = last[b - 1]; last[b - 1] = u;
            }
        free(last);
        return SL_SUB_OK;
    }
    if (what == SL_SUB_Q_ALBUM)
    {
        for (int i = 0; i < sj.n_cache; i++)
        {
            const struct sj_song *e = &sj.cache[i];
            if (e->server != l->server || strcmp(e->f[SF_ALBUM_ID], arg) || !sj_list_room(l)) continue;
            sj_item_from(l, &l->it[l->n++], e);
        }
        if (!l->n) return SL_SUB_E_OFFLINE;
        qsort(l->it, (size_t)l->n, sizeof *l->it, sj_song_cmp);
        struct sl_sub_item *h = &l->head;
        h->kind = SL_SUB_ALBUM;
        h->id = sj_pool(l, arg);
        h->title = l->it[0].album;
        h->artist = l->it[0].artist;
        h->cover = l->it[0].cover;
        h->count = (uint16_t)l->n;
        for (int i = 0; i < l->n; i++) h->dur += l->it[i].dur;
        h->cached = 1;
        h->pinned = sj_pin_find(l->server, SL_SUB_ALBUM, arg) != NULL;
        return SL_SUB_OK;
    }
    if (what == SL_SUB_Q_PLAYLIST)
    {
        const struct sj_pin *p = sj_pin_find(l->server, SL_SUB_PLAYLIST, arg);
        if (!p) return SL_SUB_E_OFFLINE;
        const char *id = p->ids;
        for (int k = 0; k < p->n; k++, id += strlen(id) + 1)
        {
            const struct sj_song *e = sj_cache_find(l->server, id);
            if (e && sj_list_room(l)) sj_item_from(l, &l->it[l->n++], e);
        }
        struct sl_sub_item *h = &l->head;
        h->kind = SL_SUB_PLAYLIST;
        h->id = sj_pool(l, arg);
        h->title = sj_pool(l, p->name);
        h->count = (uint16_t)l->n;
        h->pinned = 1;
        if (l->n) h->cover = l->it[0].cover;
        return SL_SUB_OK;
    }
    return SL_SUB_E_OFFLINE;
}
static void sj_answer(const struct sl_sub_list *l)
{
    int at = -1;
    for (int i = 0; i < SJ_ANSWERS && at < 0; i++) if (!sj.ans_ok[i]) at = i;
    if (at < 0)
    {
        /* the oldest goes */
        at = 0;
        for (int i = 1; i < SJ_ANSWERS; i++) if (sj.ans[i].req < sj.ans[at].req) at = i;
        sl_sub_list_free(&sj.ans[at]);
    }
    sj.ans[at] = *l;
    sj.ans_ok[at] = true;
    sj.st.list_seq++;
    sj.st.seq++;
}
static bool sj_forgotten(unsigned req)
{
    for (int i = 0; i < 8; i++) if (sj.forgot[i] == req) return true;
    return false;
}
static void sj_list(struct sj_job *j)
{
    struct sl_sub_list l;
    memset(&l, 0, sizeof l);
    l.req = j->req;
    l.what = j->what;
    l.offset = j->offset;
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *sv = sj_srv_find(0), s;
    if (sv) s = *sv;
    bool online = sj.online;
    l.server = sv ? s.pub.id : 0;
    pthread_mutex_unlock(&sj_lock);
    if (!sv) { l.err = SL_SUB_E_NOSERVER; goto done; }
    if (j->what == SL_SUB_Q_OFFLINE || !online)
    {
        pthread_mutex_lock(&sj_lock);
        l.err = sj_from_cache(j->what, j->arg, &l);
        pthread_mutex_unlock(&sj_lock);
        goto done;
    }
    char p[400], a[300];
    lj_enc(a, sizeof a, j->arg);
    const char *method = NULL;
    p[0] = '\0';
    switch (j->what)
    {
        case SL_SUB_Q_ARTISTS: method = "getArtists"; break;
        case SL_SUB_Q_ARTIST: method = "getArtist"; snprintf(p, sizeof p, "id=%s", a); break;
        case SL_SUB_Q_ALBUMS: method = "getAlbumList2"; snprintf(p, sizeof p, "type=%s&size=%d&offset=%d", a, SL_SUB_PAGE_N, j->offset); break;
        case SL_SUB_Q_ALBUM: method = "getAlbum"; snprintf(p, sizeof p, "id=%s", a); break;
        case SL_SUB_Q_PLAYLISTS: method = "getPlaylists"; break;
        case SL_SUB_Q_PLAYLIST: method = "getPlaylist"; snprintf(p, sizeof p, "id=%s", a); break;
        case SL_SUB_Q_SEARCH: method = "search3"; snprintf(p, sizeof p, "query=%s&artistCount=10&albumCount=20&songCount=40", a); break;
        case SL_SUB_Q_STARRED: method = "getStarred2"; break;
    }
    l.err = method ? sj_call(&s, method, p, false, &l, NULL) : SL_SUB_E_SERVER;
    memset(&s, 0, sizeof s);
    if (l.err == SL_SUB_E_NET && (j->what == SL_SUB_Q_ALBUM || j->what == SL_SUB_Q_PLAYLIST))
    {
        /* the server is gone: what of it is on the card */
        sl_sub_list_free(&l);
        l.req = j->req; l.what = j->what;
        pthread_mutex_lock(&sj_lock);
        l.server = sj.active;
        int e = sj_from_cache(j->what, j->arg, &l);
        pthread_mutex_unlock(&sj_lock);
        l.err = e ? SL_SUB_E_NET : SL_SUB_OK;
    }
    if (!l.err && j->what == SL_SUB_Q_ALBUMS) l.more = l.n >= SL_SUB_PAGE_N;
done:
    pthread_mutex_lock(&sj_lock);
    if (!l.err) sj_mark(&l);
    if (sj_forgotten(l.req)) sl_sub_list_free(&l);
    else sj_answer(&l);
    pthread_mutex_unlock(&sj_lock);
}
void sl_sub_art_name(char *out, size_t n, uint32_t server, const char *cover, int px)
{
    char k[120];
    snprintf(k, sizeof k, "%08x/%s", (unsigned)server, cover ? cover : "");
    snprintf(out, n, SL_SUB_STATE "/art/%08x-%d.bin", (unsigned)pj_fnv(k), px);
}
static void sj_art(const char *cover)
{
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *sv = sj_srv_find(0), s;
    uint32_t sid = sv ? sv->pub.id : 0;
    if (sv) s = *sv;
    pthread_mutex_unlock(&sj_lock);
    if (!sv || !cover[0]) return;
    char params[200], id[160], url[1400];
    lj_enc(id, sizeof id, cover);
    snprintf(params, sizeof params, "id=%s&size=300", id);
    bool ok = sj_url(url, sizeof url, &s, "getCoverArt", params, false);
    memset(&s, 0, sizeof s);
    if (!ok) return;
    struct pj_mem m = { malloc(SJ_ART_CAP), 0, SJ_ART_CAP };
    if (!m.b) return;
    int status = sl_net_http_stream(url, SL_SUB_UA, 20000, pj_mem_sink, &m, &sj.stop_job);
    memset(url, 0, sizeof url);
    int w = 0, h = 0;
    unsigned char *rgb = status == 200 && m.n ? sl_art_rgb_load(m.b, (int)m.n, &w, &h) : NULL;
    free(m.b);
    if (!rgb) return;
    char big[96], row[96];
    sl_sub_art_name(big, sizeof big, sid, cover, SL_SUB_ART_BIG);
    sl_sub_art_name(row, sizeof row, sid, cover, SL_SUB_ART_ROW);
    pthread_mutex_lock(&sj_lock);
    if (!sj.hold && pj_art_square(big, SL_SUB_STATE "/art", rgb, w, h, SL_SUB_ART_BIG, NULL) &&
        pj_art_square(row, SL_SUB_STATE "/art", rgb, w, h, SL_SUB_ART_ROW, NULL))
    { sj.st.art_seq++; sj.st.seq++; }
    pthread_mutex_unlock(&sj_lock);
    sl_art_rgb_free(rgb);
}
static void sj_star(struct sj_job *j)
{
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *sv = sj_srv_find(0), s;
    if (sv) s = *sv;
    pthread_mutex_unlock(&sj_lock);
    int err = SL_SUB_E_NOSERVER;
    if (sv)
    {
        char p[200], a[160];
        lj_enc(a, sizeof a, j->arg);
        snprintf(p, sizeof p, "%s=%s", j->what == SL_SUB_ALBUM ? "albumId" : j->what == SL_SUB_ARTIST ? "artistId" : "id", a);
        err = sj_call(&s, j->on ? "star" : "unstar", p, false, NULL, NULL);
        memset(&s, 0, sizeof s);
    }
    pthread_mutex_lock(&sj_lock);
    if (!err && j->what == SL_SUB_SONG)
    {
        struct sj_song *e = sj_cache_find(sj.active, j->arg);
        if (e && e->starred != j->on) { e->starred = j->on; sj_cache_save(); }
    }
    sj.st.star_err = err;
    sj.st.star_seq++;
    sj.st.seq++;
    pthread_mutex_unlock(&sj_lock);
}
/* plays waiting to be told: on the card, so a power cut loses none */
static void sj_scrob_load(void)
{
    if (sj.scrob_read) return;
    sj.scrob_read = true;
    char *b = pj_slurp(SJ_SCROBS, 64 * 1024, NULL);
    char *s = b, *line;
    if (b) sj.scrob = calloc(SJ_SCROB_MAX, sizeof *sj.scrob);
    while (sj.scrob && (line = strsep(&s, "\n")))
    {
        if (!line[0] || sj.n_scrob >= SJ_SCROB_MAX) continue;
        char *f = line;
        struct sj_scrob *c = &sj.scrob[sj.n_scrob];
        c->server = (uint32_t)strtoul(pj_next(&f), NULL, 16);
        c->when = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        c->sub = true;
        snprintf(c->id, sizeof c->id, "%s", pj_next(&f));
        if (c->id[0]) sj.n_scrob++;
    }
    free(b);
    sj.st.scrobbles = sj.n_scrob;
}
static void sj_scrob_save(void)
{
    if (sj.hold) return;
    struct pj_buf o = { 0 };
    for (int i = 0; i < sj.n_scrob; i++)
        if (sj.scrob[i].sub) pj_add(&o, "%08x\t%u\t%s\n", (unsigned)sj.scrob[i].server, (unsigned)sj.scrob[i].when, sj.scrob[i].id);
    char p[512];
    if (!o.b) { if (pj_real(p, sizeof p, SJ_SCROBS)) unlink(p); }
    else if (!o.bad) { pj_mkdirs(SL_SUB_STATE); pj_write(SJ_SCROBS, o.b, o.n); }
    free(o.b);
    sj.st.scrobbles = sj.n_scrob;
}
/* the oldest play waiting, told; locked, the lock let go meanwhile */
static bool sj_scrob_send(void)
{
    if (!sj.n_scrob || !sj.online || sj.hold) return false;
    struct sj_scrob c = sj.scrob[0];
    struct sj_srv *sv = sj_srv_find(c.server), s;
    if (!sv)
    {
        memmove(&sj.scrob[0], &sj.scrob[1], (size_t)--sj.n_scrob * sizeof *sj.scrob);
        sj_scrob_save();
        return true;
    }
    s = *sv;
    pthread_mutex_unlock(&sj_lock);
    char p[240], a[160];
    lj_enc(a, sizeof a, c.id);
    snprintf(p, sizeof p, "id=%s&time=%llu000&submission=%s", a, (unsigned long long)c.when, c.sub ? "true" : "false");
    int err = sj_call(&s, "scrobble", p, false, NULL, NULL);
    memset(&s, 0, sizeof s);
    pthread_mutex_lock(&sj_lock);
    if (err == SL_SUB_E_NET) { sj.scrob_retry_at = sj_ms() + 60000; return false; }
    /* told, or refused for good: either way, not again */
    for (int i = 0; i < sj.n_scrob; i++)
        if (sj.scrob[i].when == c.when && !strcmp(sj.scrob[i].id, c.id) && sj.scrob[i].sub == c.sub)
        {
            memmove(&sj.scrob[i], &sj.scrob[i + 1], (size_t)(sj.n_scrob - i - 1) * sizeof *sj.scrob);
            sj.n_scrob--;
            break;
        }
    if (c.sub) sj_scrob_save();
    sj.st.scrobbles = sj.n_scrob;
    sj.st.seq++;
    return true;
}
static void sj_run(struct sj_job *j)
{
    switch (j->kind)
    {
        case SL_SUB_JOB_SIGNIN: sj_signin(j); break;
        case SL_SUB_JOB_LIST: sj_list(j); break;
        case SL_SUB_JOB_STAR: sj_star(j); break;
    }
}
static void *sj_loop(void *unused)
{
    (void)unused;
    uint32_t idle_from = sj_ms();
    pthread_mutex_lock(&sj_lock);
    sj_queue_load();
    sj_scrob_load();
    for (;;)
    {
        sj_dl_start_next();
        sj_dl_step();
        if (sj.n_jobs && !sj.hold)
        {
            struct sj_job j = sj.jobs[0];
            memset(sj.jobs[0].secret, 0, sizeof sj.jobs[0].secret);
            memmove(&sj.jobs[0], &sj.jobs[1], (size_t)--sj.n_jobs * sizeof *sj.jobs);
            sj.st.job = j.kind;
            sj.st.jobs = sj.n_jobs;
            sj.stop_job = false;
            sj.st.seq++;
            pthread_mutex_unlock(&sj_lock);
            sj_run(&j);
            memset(&j, 0, sizeof j);
            pthread_mutex_lock(&sj_lock);
            sj.st.job = SL_SUB_JOB_NONE;
            sj.st.seq++;
            idle_from = sj_ms();
            continue;
        }
        if (sj.n_art && !sj.hold && sj.online)
        {
            /* the latest asked for first: the covers on screen now */
            char cover[48];
            snprintf(cover, sizeof cover, "%s", sj.art_q[--sj.n_art]);
            sj.st.job = SL_SUB_JOB_ART;
            sj.stop_job = false;
            pthread_mutex_unlock(&sj_lock);
            sj_art(cover);
            pthread_mutex_lock(&sj_lock);
            sj.st.job = SL_SUB_JOB_NONE;
            idle_from = sj_ms();
            continue;
        }
        if (sj.n_scrob && sj.online && !sj.hold && (!sj.scrob_retry_at || (int32_t)(sj_ms() - sj.scrob_retry_at) >= 0))
        {
            sj.st.job = SL_SUB_JOB_SCROBBLE;
            bool sent = sj_scrob_send();
            sj.st.job = SL_SUB_JOB_NONE;
            idle_from = sj_ms();
            if (sent) continue;
        }
        bool waiting = sj.dl_on || (sj.n_q && sj.online && !sj.hold);
        if (waiting) idle_from = sj_ms();
        if (!sj.n_jobs && !waiting && (int32_t)(sj_ms() - idle_from) >= (int32_t)SJ_IDLE_MS) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sj_cond, &sj_lock, &ts);
    }
    sj.up = false;
    if (sj.cache_dirty) sj_cache_save();
    pthread_mutex_unlock(&sj_lock);
    return NULL;
}
static bool sj_start(void)
{
    if (sj.up) { pthread_cond_broadcast(&sj_cond); return true; }
    if (sj.thread) pthread_join(sj.thread, NULL);
    sj.thread = 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    sj.up = pthread_create(&sj.thread, &a, sj_loop, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!sj.up) sj.thread = 0;
    return sj.up;
}
static bool sj_job_add(const struct sj_job *in)
{
    if (sj.n_jobs >= SJ_JOBS) return false;
    sj.jobs[sj.n_jobs++] = *in;
    sj.st.jobs = sj.n_jobs;
    sj.st.seq++;
    if (!sj_start()) { sj.n_jobs--; return false; }
    return true;
}

/* ---------------------------------------------------------------- the interface's calls */
unsigned sl_sub_ask(int what, const char *arg, int offset)
{
    struct sj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_SUB_JOB_LIST;
    j.what = what;
    j.offset = offset;
    snprintf(j.arg, sizeof j.arg, "%s", arg ? arg : "");
    pthread_mutex_lock(&sj_lock);
    j.req = ++sj.req_next;
    if (!j.req) j.req = ++sj.req_next;
    unsigned r = sj_job_add(&j) ? j.req : 0;
    pthread_mutex_unlock(&sj_lock);
    return r;
}
bool sl_sub_take(unsigned req, struct sl_sub_list *out)
{
    bool ok = false;
    pthread_mutex_lock(&sj_lock);
    for (int i = 0; i < SJ_ANSWERS && !ok; i++)
        if (sj.ans_ok[i] && sj.ans[i].req == req)
        {
            *out = sj.ans[i];
            memset(&sj.ans[i], 0, sizeof sj.ans[i]);
            sj.ans_ok[i] = false;
            ok = true;
        }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
void sl_sub_forget_req(unsigned req)
{
    if (!req) return;
    pthread_mutex_lock(&sj_lock);
    static int k;
    sj.forgot[k++ & 7] = req;
    for (int i = 0; i < SJ_ANSWERS; i++)
        if (sj.ans_ok[i] && sj.ans[i].req == req) { sl_sub_list_free(&sj.ans[i]); sj.ans_ok[i] = false; }
    pthread_mutex_unlock(&sj_lock);
}
void sl_sub_list_free(struct sl_sub_list *l)
{
    if (!l) return;
    free(l->it);
    free(l->pool);
    memset(l, 0, sizeof *l);
}
bool sl_sub_list_dup(const struct sl_sub_list *l, struct sl_sub_list *out)
{
    *out = *l;
    out->it = NULL; out->pool = NULL; out->cap = 0; out->pool_cap = 0;
    if (l->n)
    {
        out->it = malloc((size_t)l->n * sizeof *out->it);
        if (!out->it) { memset(out, 0, sizeof *out); return false; }
        memcpy(out->it, l->it, (size_t)l->n * sizeof *out->it);
        out->cap = l->n;
    }
    if (l->pool_n)
    {
        out->pool = malloc(l->pool_n);
        if (!out->pool) { free(out->it); memset(out, 0, sizeof *out); return false; }
        memcpy(out->pool, l->pool, l->pool_n);
        out->pool_cap = l->pool_n;
    }
    return true;
}
void sl_sub_online(bool up)
{
    pthread_mutex_lock(&sj_lock);
    bool was = sj.online;
    sj.online = up;
    sj.st.online = up;
    sj.st.seq++;
    if (up && !was)
    {
        char real[512];
        struct stat st;
        if (sj.n_q || sj.n_scrob || (pj_real(real, sizeof real, SJ_QUEUE) && !stat(real, &st)) ||
            (pj_real(real, sizeof real, SJ_SCROBS) && !stat(real, &st)))
        { sj.dl_retry_at = sj.scrob_retry_at = 0; sj_start(); }
    }
    if (!up && sj.dl_on) sj_dl_end(true);
    pthread_cond_broadcast(&sj_cond);
    pthread_mutex_unlock(&sj_lock);
}
void sl_sub_hold(bool hold)
{
    pthread_mutex_lock(&sj_lock);
    if (hold)
    {
        if (sj.cache_dirty) sj_cache_save();
        sj_dl_end(true);                  /* wget lets go of its file now */
        sj.stop_job = true;
    }
    sj.hold = hold;
    sj.st.hold = hold;
    sj.st.seq++;
    if (!hold && (sj.n_q || sj.cache_dirty) && sj.online) { sj.dl_retry_at = 0; sj_start(); }
    pthread_cond_broadcast(&sj_cond);
    pthread_mutex_unlock(&sj_lock);
}
void sl_sub_status(struct sl_sub_status *s)
{
    pthread_mutex_lock(&sj_lock);
    *s = sj.st;
    pthread_mutex_unlock(&sj_lock);
}
bool sl_sub_busy(void)
{
    pthread_mutex_lock(&sj_lock);
    bool up = sj.up;
    pthread_mutex_unlock(&sj_lock);
    return up;
}
void sl_sub_poll(void)
{
    pthread_mutex_lock(&sj_lock);
    if (!sj.up && !sj.hold && sj.online && (sj.n_q || sj.n_art || (sj.n_scrob && (!sj.scrob_retry_at || (int32_t)(sj_ms() - sj.scrob_retry_at) >= 0))))
        sj_start();
    pthread_mutex_unlock(&sj_lock);
}
int sl_sub_servers(struct sl_sub_server *out, int max)
{
    pthread_mutex_lock(&sj_lock);
    sj_srv_load();
    int n = sj.n_srv < max ? sj.n_srv : max;
    for (int i = 0; i < n; i++) out[i] = sj.srv[i].pub;
    pthread_mutex_unlock(&sj_lock);
    return n;
}
uint32_t sl_sub_active(void)
{
    pthread_mutex_lock(&sj_lock);
    sj_srv_load();
    uint32_t a = sj.active;
    pthread_mutex_unlock(&sj_lock);
    return a;
}
bool sl_sub_server(uint32_t id, struct sl_sub_server *out)
{
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *s = sj_srv_find(id);
    if (s && out) *out = s->pub;
    pthread_mutex_unlock(&sj_lock);
    return s != NULL;
}
bool sl_sub_url_https(uint32_t id)
{
    struct sl_sub_server s;
    return sl_sub_server(id, &s) && !strncasecmp(s.url, "https://", 8);
}
bool sl_sub_use(uint32_t id)
{
    pthread_mutex_lock(&sj_lock);
    bool ok = sj_srv_find(id) && !sj.hold;
    if (ok && sj.active != id)
    {
        sj.active = id;
        for (int i = 0; i < sj.n_q; ) if (!sj.q[i].pin && !(sj.dl_on && i == 0)) sj_q_drop(i); else i++;
        ok = sj_srv_save();
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_remove(uint32_t id)
{
    pthread_mutex_lock(&sj_lock);
    struct sj_srv *s = sj_srv_find(id);
    bool ok = s && !sj.hold;
    if (ok)
    {
        int at = (int)(s - sj.srv);
        if (sj.dl_on && sj.st.dl_server == id) sj_dl_end(false);
        for (int i = 0; i < sj.n_q; ) if (sj.q[i].s.server == id) sj_q_drop(i); else i++;
        sj_queue_save();
        memset(s, 0, sizeof *s);
        memmove(&sj.srv[at], &sj.srv[at + 1], (size_t)(sj.n_srv - at - 1) * sizeof *sj.srv);
        sj.n_srv--;
        if (sj.active == id) sj.active = sj.n_srv ? sj.srv[0].pub.id : 0;
        if (sj.n_srv) ok = sj_srv_save();
        else
        {
            char p[512];
            if (pj_real(p, sizeof p, SJ_SERVERS)) unlink(p);
            sj.st.servers_seq++;
        }
        sj.st.queued = sj.n_q;
        sj.st.seq++;
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_signin(const char *url, const char *user, const char *secret, uint32_t id, bool save)
{
    struct sj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_SUB_JOB_SIGNIN;
    j.id = id;
    j.save = save;
    snprintf(j.url, sizeof j.url, "%s", url ? url : "");
    snprintf(j.user, sizeof j.user, "%s", user ? user : "");
    snprintf(j.secret, sizeof j.secret, "%s", secret ? secret : "");
    pthread_mutex_lock(&sj_lock);
    bool ok = sj_job_add(&j);
    pthread_mutex_unlock(&sj_lock);
    memset(&j, 0, sizeof j);
    return ok;
}
void sl_sub_prefs(struct sl_sub_prefs *p)
{
    pthread_mutex_lock(&sj_lock);
    sj_prefs_load();
    *p = sj.prefs;
    pthread_mutex_unlock(&sj_lock);
}
bool sl_sub_prefs_set(const struct sl_sub_prefs *p)
{
    pthread_mutex_lock(&sj_lock);
    sj_prefs_load();
    bool ok = !sj.hold;
    if (ok)
    {
        sj.prefs = *p;
        char b[64];
        int n = snprintf(b, sizeof b, "SUBP %d %d\n", p->quality, p->limit_gb);
        pj_mkdirs(SL_SUB_STATE);
        ok = pj_write(SJ_PREFS, b, (size_t)n);
        /* a smaller limit: room made now */
        sj_cache_load();
        unsigned s0 = sj.session;
        sj.session = 0;
        sj_make_room(0);
        sj.session = s0;
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
unsigned sl_sub_session_new(void)
{
    pthread_mutex_lock(&sj_lock);
    sj_queue_load();
    unsigned s = ++sj.session;
    /* the last play's songs still to come: not now (a part stays for when they are asked again) */
    for (int i = 0; i < sj.n_q; )
    {
        if (sj.q[i].pin) { i++; continue; }
        if (i == 0 && sj.dl_on) sj_dl_end(true);
        sj_q_drop(i);
    }
    sj.st.queued = sj.n_q;
    sj.st.seq++;
    pthread_mutex_unlock(&sj_lock);
    return s;
}
/* a list's song as the cache keeps it; its folder from the album's artist when the list is an album */
static bool sj_song_of(const struct sl_sub_list *l, int i, struct sj_song *e, int quality)
{
    const struct sl_sub_item *it = &l->it[i];
    if (it->kind != SL_SUB_SONG || !it->id) return false;
    const char *ext = quality == SL_SUB_Q_MP3 ? "mp3" : quality == SL_SUB_Q_OPUS ? "opus" : sl_sub_s(l, it->suffix);
    char a[100], al[100], fn[160], rel[400];
    const char *artist = l->head.kind == SL_SUB_ALBUM && l->head.artist ? sl_sub_s(l, l->head.artist) : sl_sub_s(l, it->artist);
    sl_pod_clean_name(a, sizeof a, artist[0] ? artist : "Unknown artist");
    sl_pod_clean_name(al, sizeof al, it->album ? sl_sub_s(l, it->album) : "Unknown album");
    sl_sub_file_name(fn, sizeof fn, it->disc, it->track, sl_sub_s(l, it->title), ext[0] ? ext : "mp3");
    snprintf(rel, sizeof rel, "%s/%s/%s", a, al, fn);
    /* another song's name already: this one's id makes it its own */
    const struct sj_song *o = sj_cache_rel(rel);
    if (o && (o->server != l->server || strcmp(o->f[SF_ID], sl_sub_s(l, it->id))))
    {
        char *dot = strrchr(fn, '.');
        if (dot) *dot = '\0';
        snprintf(rel, sizeof rel, "%s/%s/%s %.8s.%s", a, al, fn, sl_sub_s(l, it->id), ext[0] ? ext : "mp3");
    }
    const char *v[SF_N] = { sl_sub_s(l, it->id), sl_sub_s(l, it->album_id), sl_sub_s(l, it->cover), sl_sub_s(l, it->title),
                            sl_sub_s(l, it->artist), sl_sub_s(l, it->album), ext, rel };
    memset(e, 0, sizeof *e);
    e->server = l->server;
    e->track = it->track;
    e->disc = it->disc;
    e->dur = it->dur;
    e->starred = it->starred;
    return sj_song_set(e, v);
}
static uint32_t sj_est_kb(const struct sl_sub_item *it, int quality)
{
    uint32_t dur = it->dur ? it->dur : 240;
    if (quality == SL_SUB_Q_MP3) return dur * 40 + 64;         /* 320 kbit/s */
    if (quality == SL_SUB_Q_OPUS) return dur * 16 + 64;        /* 128 */
    return it->size ? it->size / 1024 + 1 : dur * 120;         /* a FLAC's, about */
}
int sl_sub_fetch(const struct sl_sub_list *l, const int *idx, int n, unsigned session)
{
    int queued = 0;
    pthread_mutex_lock(&sj_lock);
    sj_prefs_load();
    sj_cache_load();
    sj_queue_load();
    for (int k = 0; k < n; k++)
    {
        if (idx[k] < 0 || idx[k] >= l->n) continue;
        const struct sl_sub_item *it = &l->it[idx[k]];
        struct sj_song *have = sj_cache_find(l->server, sl_sub_s(l, it->id));
        if (have) { have->sess = session; continue; }   /* this play's: not given up for room */
        bool waiting = false;
        for (int i = 0; i < sj.n_q && !waiting; i++)
            if (sj.q[i].s.server == l->server && !strcmp(sj.q[i].s.f[SF_ID], sl_sub_s(l, it->id)))
            {
                waiting = true;
                sj.q[i].session = session;
                /* a kept one already on its way, wanted now: to the front (after what is coming down) */
                int to = sj.dl_on ? 1 : 0;
                if (i > to) { struct sj_dl t = sj.q[i]; memmove(&sj.q[to + 1], &sj.q[to], (size_t)(i - to) * sizeof *sj.q); sj.q[to] = t; }
            }
        if (waiting || !sj_q_room()) continue;
        struct sj_dl q;
        memset(&q, 0, sizeof q);
        if (!sj_song_of(l, idx[k], &q.s, sj.prefs.quality)) continue;
        q.session = session;
        q.est_kb = sj_est_kb(it, sj.prefs.quality);
        /* a play's songs before the ones kept offline, in the play's order */
        int at = sj.dl_on ? 1 : 0;
        while (at < sj.n_q && !sj.q[at].pin) at++;
        memmove(&sj.q[at + 1], &sj.q[at], (size_t)(sj.n_q - at) * sizeof *sj.q);
        sj.q[at] = q;
        sj.n_q++;
        queued++;
    }
    sj.st.queued = sj.n_q - (sj.dl_on ? 1 : 0);
    sj.st.seq++;
    if (queued) { sj.dl_retry_at = 0; sj_start(); }
    pthread_mutex_unlock(&sj_lock);
    return queued;
}
bool sl_sub_cached(uint32_t server, const char *id, char *path, size_t n)
{
    pthread_mutex_lock(&sj_lock);
    const struct sj_song *e = sj_cache_find(server, id);
    bool ok = e && (!path || sj_rb_path(path, n, e->f[SF_REL]));
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_is_cached_path(const char *path)
{
    size_t n = strlen(sl_sub_folder);
    return path && n > 1 && !strncasecmp(path, sl_sub_folder, n) && path[n] == '/';
}
bool sl_sub_which(const char *path, uint32_t *server, char *id, size_t n)
{
    if (!sl_sub_is_cached_path(path)) return false;
    pthread_mutex_lock(&sj_lock);
    const struct sj_song *e = sj_cache_rel(path + strlen(sl_sub_folder) + 1);
    if (e) { if (server) *server = e->server; if (id) snprintf(id, n, "%s", e->f[SF_ID]); }
    pthread_mutex_unlock(&sj_lock);
    return e != NULL;
}
void sl_sub_touch(const char *path)
{
    if (!sl_sub_is_cached_path(path)) return;
    pthread_mutex_lock(&sj_lock);
    struct sj_song *e = sj_cache_rel(path + strlen(sl_sub_folder) + 1);
    if (e) { e->used = ++sj.clock; sj.cache_dirty = true; if (!sj.hold && !sj.up) sj_cache_save(); else sj_start(); }
    pthread_mutex_unlock(&sj_lock);
}
static void sj_repin(uint32_t server)
{
    for (int i = 0; i < sj.n_cache; i++)
        if (sj.cache[i].server == server) sj.cache[i].pinned = sj_pinned_song(server, sj.cache[i].f[SF_ID]);
}
bool sl_sub_pin(const struct sl_sub_list *l, bool on)
{
    int kind = l->head.kind;
    const char *id = sl_sub_s(l, l->head.id);
    if ((kind != SL_SUB_ALBUM && kind != SL_SUB_PLAYLIST) || !id[0]) return false;
    pthread_mutex_lock(&sj_lock);
    sj_cache_load();
    sj_pins_load();
    sj_queue_load();
    sj_prefs_load();
    bool ok = !sj.hold;
    struct sj_pin *p = sj_pin_find(l->server, kind, id);
    if (ok && on && !p)
    {
        if (!sj.pins) sj.pins = calloc(SJ_PIN_MAX, sizeof *sj.pins);
        size_t need = 1;
        for (int i = 0; i < l->n; i++) need += strlen(sl_sub_s(l, l->it[i].id)) + 1;
        ok = sj.pins && sj.n_pins < SJ_PIN_MAX;
        char *ids = ok ? malloc(need) : NULL;
        ok = ids != NULL;
        if (ok)
        {
            p = &sj.pins[sj.n_pins++];
            memset(p, 0, sizeof *p);
            p->server = l->server;
            p->kind = (uint8_t)kind;
            snprintf(p->id, sizeof p->id, "%s", id);
            snprintf(p->name, sizeof p->name, "%s", sl_sub_s(l, l->head.title));
            p->ids = ids;
            char *w = ids;
            for (int i = 0; i < l->n; i++) if (l->it[i].kind == SL_SUB_SONG && l->it[i].id) { w = pj_put(w, sl_sub_s(l, l->it[i].id)); p->n++; }
            *w = '\0';
            /* what is here is kept; the rest comes, after what is playing */
            for (int i = 0; i < l->n; i++)
            {
                const struct sl_sub_item *it = &l->it[i];
                if (it->kind != SL_SUB_SONG) continue;
                struct sj_song *e = sj_cache_find(l->server, sl_sub_s(l, it->id));
                if (e) { e->pinned = 1; continue; }
                bool waiting = false;
                for (int k = 0; k < sj.n_q && !waiting; k++)
                    if (sj.q[k].s.server == l->server && !strcmp(sj.q[k].s.f[SF_ID], sl_sub_s(l, it->id))) { waiting = true; sj.q[k].pin = true; }
                if (waiting || !sj_q_room()) continue;
                struct sj_dl q;
                memset(&q, 0, sizeof q);
                if (!sj_song_of(l, i, &q.s, sj.prefs.quality)) continue;
                q.pin = true;
                q.s.pinned = 1;
                q.est_kb = sj_est_kb(it, sj.prefs.quality);
                sj.q[sj.n_q++] = q;
            }
            sj_pins_save();
            sj_cache_save();
            sj_queue_save();
            sj.dl_retry_at = 0;
            sj_start();
        }
    }
    else if (ok && !on && p)
    {
        free(p->ids);
        *p = sj.pins[--sj.n_pins];
        sj_repin(l->server);
        for (int k = 0; k < sj.n_q; )
            if (sj.q[k].s.server == l->server && sj.q[k].pin && !sj.q[k].session && !sj_pinned_song(l->server, sj.q[k].s.f[SF_ID]))
            { if (k == 0 && sj.dl_on) sj_dl_end(false); sj_q_drop(k); }
            else k++;
        sj_pins_save();
        sj_cache_save();
        sj_queue_save();
    }
    sj.st.queued = sj.n_q - (sj.dl_on ? 1 : 0);
    sj.st.cache_seq++;
    sj.st.seq++;
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_pinned(int kind, const char *id)
{
    pthread_mutex_lock(&sj_lock);
    sj_srv_load();
    bool p = id && sj_pin_find(sj.active, kind, id) != NULL;
    pthread_mutex_unlock(&sj_lock);
    return p;
}
bool sl_sub_forget(const char *album_id)
{
    pthread_mutex_lock(&sj_lock);
    sj_srv_load();
    sj_cache_load();
    sj_pins_load();
    bool ok = !sj.hold && album_id;
    if (ok)
    {
        struct sj_pin *p = sj_pin_find(sj.active, SL_SUB_ALBUM, album_id);
        if (p) { free(p->ids); *p = sj.pins[--sj.n_pins]; sj_pins_save(); sj_repin(sj.active); }
        for (int i = sj.n_cache - 1; i >= 0; i--)
            if (sj.cache[i].server == sj.active && !strcmp(sj.cache[i].f[SF_ALBUM_ID], album_id) && !sj.cache[i].pinned) sj_cache_drop(i);
        sj_cache_save();
        sj.st.cache_seq++;
        sj.st.seq++;
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_clear(void)
{
    pthread_mutex_lock(&sj_lock);
    sj_cache_load();
    bool ok = !sj.hold;
    if (ok)
    {
        for (int i = sj.n_cache - 1; i >= 0; i--) if (!sj.cache[i].pinned) sj_cache_drop(i);
        sj_cache_save();
        sj.st.cache_seq++;
        sj.st.seq++;
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
void sl_sub_usage(long long *used, long long *limit, int *songs, int *albums)
{
    pthread_mutex_lock(&sj_lock);
    sj_cache_load();
    if (used) *used = sj.used_kb * 1024;
    if (limit) *limit = sj_limit_kb() * 1024;
    if (songs) *songs = sj.n_cache;
    if (albums)
    {
        int a = 0;
        for (int i = 0; i < sj.n_cache; i++)
        {
            bool seen = false;
            for (int k = 0; k < i && !seen; k++)
                seen = sj.cache[k].server == sj.cache[i].server && !strcmp(sj.cache[k].f[SF_ALBUM_ID], sj.cache[i].f[SF_ALBUM_ID]);
            a += !seen;
        }
        *albums = a;
    }
    pthread_mutex_unlock(&sj_lock);
}
bool sl_sub_art_have(const char *cover)
{
    char rb[120], real[600];
    struct stat st;
    sl_sub_art_name(rb, sizeof rb, sl_sub_active(), cover, SL_SUB_ART_ROW);
    return cover && cover[0] && pj_real(real, sizeof real, rb) && !stat(real, &st);
}
bool sl_sub_art_want(const char *cover)
{
    if (!cover || !cover[0] || strlen(cover) >= sizeof sj.art_q[0]) return false;
    pthread_mutex_lock(&sj_lock);
    int at = -1;
    for (int i = 0; i < sj.n_art && at < 0; i++) if (!strcmp(sj.art_q[i], cover)) at = i;
    if (at >= 0) memmove(&sj.art_q[at], &sj.art_q[at + 1], (size_t)(sj.n_art - at - 1) * sizeof sj.art_q[0]), sj.n_art--;
    if (sj.n_art == SJ_ART_Q) { memmove(&sj.art_q[0], &sj.art_q[1], (size_t)(SJ_ART_Q - 1) * sizeof sj.art_q[0]); sj.n_art--; }
    snprintf(sj.art_q[sj.n_art++], sizeof sj.art_q[0], "%s", cover);
    bool ok = !sj.online || sj_start();
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_scrobble(const char *path, uint32_t when, bool submission)
{
    if (!sl_sub_is_cached_path(path)) return false;
    pthread_mutex_lock(&sj_lock);
    sj_scrob_load();
    const struct sj_song *e = sj_cache_rel(path + strlen(sl_sub_folder) + 1);
    if (!sj.scrob) sj.scrob = calloc(SJ_SCROB_MAX, sizeof *sj.scrob);
    bool ok = e && sj.scrob;
    if (ok)
    {
        /* a "playing now" is worth nothing later: only the latest waits */
        if (!submission)
        {
            for (int i = 0; i < sj.n_scrob; )
            {
                if (!sj.scrob[i].sub) { memmove(&sj.scrob[i], &sj.scrob[i + 1], (size_t)(sj.n_scrob - i - 1) * sizeof *sj.scrob); sj.n_scrob--; }
                else i++;
            }
        }
        if (sj.n_scrob == SJ_SCROB_MAX) { memmove(&sj.scrob[0], &sj.scrob[1], (size_t)(SJ_SCROB_MAX - 1) * sizeof *sj.scrob); sj.n_scrob--; }
        struct sj_scrob *c = &sj.scrob[sj.n_scrob++];
        c->server = e->server;
        c->when = when;
        c->sub = submission;
        snprintf(c->id, sizeof c->id, "%s", e->f[SF_ID]);
        if (submission) sj_scrob_save();
        sj.st.scrobbles = sj.n_scrob;
        sj.st.seq++;
        if (sj.online) sj_start();
    }
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
bool sl_sub_star(int kind, const char *id, bool on)
{
    struct sj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_SUB_JOB_STAR;
    j.what = kind;
    j.on = on;
    snprintf(j.arg, sizeof j.arg, "%s", id ? id : "");
    pthread_mutex_lock(&sj_lock);
    bool ok = j.arg[0] && sj_job_add(&j);
    pthread_mutex_unlock(&sj_lock);
    return ok;
}
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
void sl_sub_test_forget(void)
{
    pthread_mutex_lock(&sj_lock);
    for (int tries = 0; sj.up && tries < 400; tries++)
    {
        sj.stop_job = true;
        pthread_cond_broadcast(&sj_cond);
        if (!sj.n_jobs && !sj.dl_on && !sj.n_q && !sj.n_art && !sj.n_scrob) break;
        pthread_mutex_unlock(&sj_lock);
        struct timespec ts = { 0, 20000000L };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&sj_lock);
    }
    sj_dl_end(true);
    for (int i = 0; i < sj.n_q; i++) sj_song_free(&sj.q[i].s);
    free(sj.q); sj.q = NULL; sj.n_q = sj.cap_q = 0; sj.q_read = false;
    for (int i = 0; i < sj.n_cache; i++) sj_song_free(&sj.cache[i]);
    free(sj.cache); sj.cache = NULL; sj.n_cache = sj.cap_cache = 0; sj.cache_read = false; sj.used_kb = 0; sj.cache_dirty = false;
    for (int i = 0; i < sj.n_pins; i++) free(sj.pins[i].ids);
    free(sj.pins); sj.pins = NULL; sj.n_pins = 0; sj.pins_read = false;
    free(sj.scrob); sj.scrob = NULL; sj.n_scrob = 0; sj.scrob_read = false; sj.scrob_retry_at = 0;
    for (int i = 0; i < SJ_ANSWERS; i++) if (sj.ans_ok[i]) { sl_sub_list_free(&sj.ans[i]); sj.ans_ok[i] = false; }
    memset(sj.srv, 0, sizeof sj.srv); sj.n_srv = 0; sj.srv_read = false; sj.active = 0;
    sj.prefs_read = false; sj.n_art = 0; sj.n_jobs = 0;
    sj.dl_fails = 0; sj.dl_retry_at = 0;
    sj.st.queued = 0; sj.st.scrobbles = 0;
    pthread_mutex_unlock(&sj_lock);
}
size_t sl_sub_test_static_bytes(void) { return sizeof sj; }
int sl_sub_test_parse(const char *json, size_t slice, int what, struct sl_sub_list *out)
{
    memset(out, 0, sizeof *out);
    out->what = what;
    struct sj_mem_sink *m = calloc(1, sizeof *m);
    if (!m) return -1;
    m->b.l = out;
    size_t n = strlen(json);
    for (size_t at = 0; at < n; at += slice) sj_feed(&m->j, &m->b, json + at, n - at < slice ? n - at : slice);
    if (m->j.in_lit) sj_lit_end(&m->j, &m->b);
    int r = !m->b.resp ? SL_SUB_E_NOTSUB : m->b.failed ? m->b.code : 0;
    free(m);
    return r;
}
#endif
