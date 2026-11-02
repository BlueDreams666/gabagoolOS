/* GabagoolOS and an Audiobookshelf server: the worker, the files and the
 * answers' builder. See sloppy_abs.h. Included at the end of
 * sloppy_libjob.c, after sloppy_subjob.inc, whose JSON reader (sj_feed),
 * address check and file helpers it shares, and sloppy_podjob.inc's
 * (pj_real, pj_write, pj_slurp, pj_buf, pj_f, pj_next, pj_fnv, the
 * picture squares). GPLv2+.
 *
 * One lock guards everything here; the network is never waited on under
 * it. Requests go over sl_net_http_req (a Bearer token in a header, any
 * method); the files come down by sl_net_dl, resumable, the token in the
 * address as Audiobookshelf's own players send it. The token, the refresh
 * token and the password are kept 0600 and never logged.
 *
 * The place: ours is queued (on the card, the newest of each book) and
 * told with PATCH /api/me/progress; before telling, the server's is read,
 * and when it moved after ours (both on the server's clock: ours by the
 * skew its Date headers show) the server's wins and comes back as a pick. */
#include "sloppy_abs.h"
#include <sys/statvfs.h>

char sl_abs_folder[64] = "/Audiobooks";
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
unsigned sl_abs_idle_ms = 30000, sl_abs_retry_ms = 60000;
#define AJ_IDLE_MS sl_abs_idle_ms
#define AJ_RETRY_MS sl_abs_retry_ms
#else
#define AJ_IDLE_MS 30000u
#define AJ_RETRY_MS 60000u
#endif
#define AJ_JOBS 12
#define AJ_ANSWERS 6
#define AJ_ART_Q 48
#define AJ_POOL_CAP (768 * 1024)
#define AJ_ART_CAP (2 * 1024 * 1024)
#define AJ_PEND_MAX 64
#define AJ_PROG_MAX 1024
#define AJ_Q_MAX 2048
#define AJ_PICKS 8
#define AJ_TOK 640
#define AJ_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define AJ_SERVER SL_ABS_STATE "/server.tsv"
#define AJ_BOOKS SL_ABS_STATE "/books.tsv"
#define AJ_QUEUE SL_ABS_STATE "/queue.tsv"
#define AJ_PEND SL_ABS_STATE "/progress.tsv"

struct aj_srv { struct sl_abs_server pub; char token[AJ_TOK], refresh[AJ_TOK], secret[160]; };
struct aj_file { char item[48], ep[48], url[200], rel[300], title[96]; int idx, n; long long size; };
struct aj_pend { char item[48], ep[48]; uint32_t cur_ms, dur_ms; bool fin; long long t; };
struct aj_prog { uint32_t key; uint16_t permille; uint8_t fin; uint32_t cur_ms; long long upd; };
struct aj_job {
    int kind, what, page;
    unsigned req;
    bool save;
    char arg[64], arg2[160], url[192], user[64], secret[AJ_TOK + 8];
};
static pthread_mutex_t aj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aj_cond = PTHREAD_COND_INITIALIZER;
static struct {
    bool up, online, hold, stop_job;
    pthread_t thread;
    struct aj_job jobs[AJ_JOBS];
    int n_jobs;
    struct sl_abs_status st;
    unsigned req_next;
    long long skew;                       /* the server's clock less ours, seconds */
    bool skew_known;
    /* the server */
    bool srv_read, have_srv;
    struct aj_srv srv;
    /* the books on the card */
    bool books_read;
    struct sl_abs_book *books;
    int n_books, cap_books;
    /* files to come */
    bool q_read;
    struct aj_file *q;
    int n_q, cap_q;
    bool dl_on;
    struct sl_net_dl dl;
    int dl_fails;
    uint32_t dl_retry_at;
    long long dl_done;                    /* the item's files already whole, bytes */
    /* places to tell */
    bool pend_read;
    struct aj_pend *pend;
    int n_pend;
    uint32_t pend_retry_at;
    /* what the server said of each place */
    struct aj_prog *prog;
    int n_prog;
    struct sl_abs_pick picks[AJ_PICKS];
    int n_picks;
    /* answers */
    struct sl_abs_list ans[AJ_ANSWERS];
    bool ans_ok[AJ_ANSWERS];
    unsigned forgot[8];
    char art_q[AJ_ART_Q][48];
    int n_art;
} aj;

static uint32_t aj_ms(void) { return lj_ms(); }
static long long aj_now_srv_ms(void) { return ((long long)time(NULL) + aj.skew) * 1000; }
static uint32_t aj_key(const char *item, const char *ep)
{
    char k[100];
    snprintf(k, sizeof k, "%s/%s", item ? item : "", ep ? ep : "");
    return pj_fnv(k);
}
bool sl_abs_url_fix(const char *in, char *out, size_t n) { return sl_sub_url_fix(in, out, n); }
/* a JSON string's inside: quotes, backslashes and control characters escaped */
static void aj_json_str(char *out, size_t n, const char *s)
{
    size_t k = 0;
    for (; s && *s && k + 7 < n; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 32) k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = '\0';
}
/* a UTF-8 text cut by a buffer: back to the last whole letter */
static void aj_utf8_end(char *s)
{
    size_t k = strlen(s), st = k;
    while (st && ((unsigned char)s[st - 1] & 0xc0) == 0x80) st--;
    if (st && (unsigned char)s[st - 1] >= 0xc0)
    {
        unsigned char lead = (unsigned char)s[st - 1];
        size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        if (k - (st - 1) < need) s[st - 1] = '\0';
    }
}

/* ---------------------------------------------------------------- the answers' builder */
enum { AB_LOGIN = 1, AB_STATUS, AB_LIBS, AB_ITEMS, AB_ITEM, AB_INPROG, AB_AUTHORS, AB_SERIES, AB_PROGRESS, AB_ME, AB_SESSIONS };
#define AJ_AFILE 0x40                     /* an audio file of an item with no tracks list (kept until tracks are known not to come) */
struct aj_b {
    int mode;
    struct sl_abs_list *l;
    int item_d, sub_d;
    struct sl_abs_item cur;
    bool json;                            /* an object came: it is JSON */
    /* a sign-in */
    char token[AJ_TOK], access[AJ_TOK], refresh[AJ_TOK], user[64], version[16];
    bool is_abs, user_seen;
    /* a place */
    uint32_t p_cur, p_dur;
    bool p_fin, p_seen;
    long long p_upd;
    char p_item[48], p_ep[48];
    /* a session's device */
    char device[64], d_make[32], d_model[48], d_client[32], d_os[32], d_browser[32];
    long long s_upd;
    int sessions;
    /* the text of an answer that is not JSON */
    char raw[96];
    size_t rawn;
};
/* the dotted path of a value under depth from: "media.metadata.title" */
static void aj_rel(struct sj_json *j, int from, char *out, size_t n)
{
    size_t k = 0;
    out[0] = '\0';
    for (int i = from + 1; i <= j->d && k + 1 < n; i++)
        k += (size_t)snprintf(out + k, n - k, "%s%s", k ? "." : "", j->st[i].name);
    const char *key = j->st[j->d].obj ? j->st[j->d].key : j->st[j->d].name;
    if (k + 1 < n) snprintf(out + k, n - k, "%s%s", k ? "." : "", key);
}
static uint32_t aj_pool(struct sl_abs_list *l, const char *s)
{
    if (!s || !s[0]) return 0;
    size_t n = strlen(s) + 1;
    if (l->pool_n + n > l->pool_cap)
    {
        size_t want = l->pool_cap ? l->pool_cap * 2 : 4096;
        while (want < l->pool_n + n) want *= 2;
        if (want > AJ_POOL_CAP) return 0;
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
static uint32_t aj_secs_ms(const char *v) { double d = atof(v); return d > 0 && d < 4.0e6 ? (uint32_t)(d * 1000 + 0.5) : 0; }
static bool aj_true(const char *v) { return !strcmp(v, "true"); }
static void aj_add(struct sl_abs_list *l, const struct sl_abs_item *it)
{
    if (l->n >= SL_ABS_LIST_MAX) return;
    if (l->n == l->cap)
    {
        int want = l->cap ? l->cap * 2 : 32;
        if (want > SL_ABS_LIST_MAX) want = SL_ABS_LIST_MAX;
        struct sl_abs_item *n = realloc(l->it, (size_t)want * sizeof *n);
        if (!n) return;
        l->it = n;
        l->cap = want;
    }
    l->it[l->n++] = *it;
}
static void ajb_open(void *cb, struct sj_json *j)
{
    struct aj_b *b = cb;
    int d = j->d;
    b->json = true;
    if (!j->st[d].obj || !b->l) return;
    const char *name = j->st[d].name;
    bool in_arr = d > 1 && !j->st[d - 1].obj;
    if (b->item_d && b->mode == AB_ITEM && !b->sub_d && d == 4 && in_arr && !strcmp(j->st[2].name, "media"))
    {
        int k = !strcmp(name, "chapters") ? SL_ABS_CHAPTER : !strcmp(name, "tracks") ? SL_ABS_FILE :
                !strcmp(name, "episodes") ? SL_ABS_EPISODE : !strcmp(name, "audioFiles") ? (SL_ABS_FILE | AJ_AFILE) : 0;
        if (k) { b->sub_d = d; memset(&b->cur, 0, sizeof b->cur); b->cur.kind = (uint8_t)k; }
        return;
    }
    if (b->item_d) return;
    const char *list = b->mode == AB_LIBS ? "libraries" : b->mode == AB_ITEMS || b->mode == AB_SERIES ? "results" :
                       b->mode == AB_INPROG ? "libraryItems" : b->mode == AB_AUTHORS ? "authors" : NULL;
    if (list && d == 3 && in_arr && !strcmp(name, list))
    {
        b->item_d = d;
        memset(&b->cur, 0, sizeof b->cur);
        b->cur.kind = b->mode == AB_LIBS ? SL_ABS_LIBRARY : b->mode == AB_AUTHORS ? SL_ABS_AUTHOR : b->mode == AB_SERIES ? SL_ABS_SERIES : SL_ABS_BOOK;
    }
    else if (b->mode == AB_ITEM && d == 1) { b->item_d = 1; memset(&b->l->head, 0, sizeof b->l->head); b->l->head.kind = SL_ABS_BOOK; }
}
static void ajb_close(void *cb, struct sj_json *j)
{
    struct aj_b *b = cb;
    int d = j->d;
    if (!b->l) return;
    if (b->sub_d && d == b->sub_d) { b->sub_d = 0; aj_add(b->l, &b->cur); return; }
    if (b->item_d && d == b->item_d)
    {
        b->item_d = 0;
        if (b->mode == AB_ITEM) return;
        if (b->cur.kind == SL_ABS_BOOK && b->cur.podcast) b->cur.kind = SL_ABS_PODCAST;
        aj_add(b->l, &b->cur);
    }
}
/* an item's field, by its path in the item */
static void aj_item_field(struct sl_abs_list *l, struct sl_abs_item *it, const char *p, const char *v, int mode)
{
    if (!strcmp(p, "id")) { if (!it->id) it->id = aj_pool(l, v); }
    else if (!strcmp(p, "libraryId")) it->lib = aj_pool(l, v);
    else if (!strcmp(p, "mediaType")) it->podcast = !strcmp(v, "podcast");
    else if (!strcmp(p, "name") && mode != AB_ITEMS && mode != AB_INPROG) { if (!it->title) it->title = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.title")) it->title = aj_pool(l, v);
    else if (!strcmp(p, "media.metadata.authorName") || !strcmp(p, "media.metadata.author")) { if (!it->author) it->author = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.authors.authors.name")) { if (!it->author) it->author = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.narratorName") || !strcmp(p, "media.metadata.narrators.narrators")) { if (!it->narrator) it->narrator = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.seriesName")) { if (!it->series) it->series = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.series.series.name")) { if (!it->series) it->series = aj_pool(l, v); }
    else if (!strcmp(p, "media.metadata.description")) { char d[640]; snprintf(d, sizeof d, "%s", v); aj_utf8_end(d); it->desc = aj_pool(l, d); }
    else if (!strcmp(p, "media.duration")) it->dur_ms = aj_secs_ms(v);
    else if (!strcmp(p, "media.numChapters") || !strcmp(p, "media.numEpisodes")) it->count = (uint16_t)AJ_CLAMP(atol(v), 0, 65535);
    else if (!strcmp(p, "media.numTracks") || !strcmp(p, "media.numAudioFiles")) { if (!it->count) it->count = (uint16_t)AJ_CLAMP(atol(v), 0, 65535); }
    else if (!strcmp(p, "media.size") || !strcmp(p, "size")) { long long s = atoll(v); if (s > it->size) it->size = s; }
    else if (!strcmp(p, "progressLastUpdate")) it->updated = atoll(v);
    else if (!strcmp(p, "recentEpisode.id")) it->episode = aj_pool(l, v);
    else if (!strcmp(p, "recentEpisode.title")) it->desc = aj_pool(l, v);
    else if (!strcmp(p, "userMediaProgress.currentTime")) it->cur_ms = aj_secs_ms(v);
    else if (!strcmp(p, "userMediaProgress.progress")) it->permille = (uint16_t)AJ_CLAMP((long)(atof(v) * 1000 + 0.5), 0, 1000);
    else if (!strcmp(p, "userMediaProgress.isFinished")) it->finished = aj_true(v);
    else if (!strcmp(p, "userMediaProgress.lastUpdate")) it->updated = atoll(v);
    else if (!strcmp(p, "numBooks")) it->count = (uint16_t)AJ_CLAMP(atol(v), 0, 65535);
}
static void aj_sub_field(struct sl_abs_list *l, struct sl_abs_item *it, const char *p, const char *v)
{
    int k = it->kind & ~AJ_AFILE;
    if (k == SL_ABS_CHAPTER)
    {
        if (!strcmp(p, "start")) it->start_ms = aj_secs_ms(v);
        else if (!strcmp(p, "end")) { uint32_t e = aj_secs_ms(v); it->dur_ms = e > it->start_ms ? e - it->start_ms : 0; }
        else if (!strcmp(p, "title")) it->title = aj_pool(l, v);
    }
    else if (k == SL_ABS_FILE)
    {
        if (!strcmp(p, "index")) it->count = (uint16_t)AJ_CLAMP(atol(v), 0, 65535);
        else if (!strcmp(p, "startOffset")) it->start_ms = aj_secs_ms(v);
        else if (!strcmp(p, "duration")) it->dur_ms = aj_secs_ms(v);
        else if (!strcmp(p, "contentUrl")) it->url = aj_pool(l, v);
        else if (!strcmp(p, "ino")) it->id = aj_pool(l, v);
        else if (!strcmp(p, "metadata.filename")) it->title = aj_pool(l, v);
        else if (!strcmp(p, "metadata.ext")) it->ext = aj_pool(l, v);
        else if (!strcmp(p, "metadata.size")) it->size = atoll(v);
        else if (!strcmp(p, "exclude") && aj_true(v)) it->finished = 2;   /* left out by the server */
    }
    else if (k == SL_ABS_EPISODE)
    {
        if (!strcmp(p, "id")) it->episode = aj_pool(l, v);
        else if (!strcmp(p, "title")) it->title = aj_pool(l, v);
        else if (!strcmp(p, "duration") || !strcmp(p, "audioFile.duration")) { if (!it->dur_ms) it->dur_ms = aj_secs_ms(v); }
        else if (!strcmp(p, "audioTrack.contentUrl")) it->url = aj_pool(l, v);
        else if (!strcmp(p, "audioFile.ino")) { if (!it->url) it->id = aj_pool(l, v); }
        else if (!strcmp(p, "audioFile.metadata.ext")) it->ext = aj_pool(l, v);
        else if (!strcmp(p, "audioFile.metadata.filename")) it->narrator = aj_pool(l, v);
        else if (!strcmp(p, "size") || !strcmp(p, "audioFile.metadata.size")) { long long s = atoll(v); if (s > it->size) it->size = s; }
    }
}
static void ajb_value(void *cb, struct sj_json *j, const char *v, bool quoted)
{
    struct aj_b *b = cb;
    int d = j->d;
    const char *k = j->st[d].obj ? j->st[d].key : j->st[d].name;
    char p[128];
    if (!quoted && !strcmp(v, "null")) return;
    switch (b->mode)
    {
        case AB_STATUS:
            if (d == 1 && (!strcmp(k, "isInit") || !strcmp(k, "serverVersion"))) b->is_abs = true;
            if (d == 1 && !strcmp(k, "serverVersion")) snprintf(b->version, sizeof b->version, "%s", v);
            return;
        case AB_LOGIN:
            if (d == 2 && !strcmp(j->st[2].name, "user"))
            {
                b->user_seen = true;
                if (!strcmp(k, "token")) snprintf(b->token, sizeof b->token, "%s", v);
                else if (!strcmp(k, "accessToken")) snprintf(b->access, sizeof b->access, "%s", v);
                else if (!strcmp(k, "refreshToken")) snprintf(b->refresh, sizeof b->refresh, "%s", v);
                else if (!strcmp(k, "username")) snprintf(b->user, sizeof b->user, "%s", v);
            }
            else if (d == 2 && !strcmp(j->st[2].name, "serverSettings") && !strcmp(k, "version")) snprintf(b->version, sizeof b->version, "%s", v);
            return;
        case AB_PROGRESS:
            if (d != 1) return;
            b->p_seen = true;
            if (!strcmp(k, "currentTime")) b->p_cur = aj_secs_ms(v);
            else if (!strcmp(k, "duration")) b->p_dur = aj_secs_ms(v);
            else if (!strcmp(k, "isFinished")) b->p_fin = aj_true(v);
            else if (!strcmp(k, "lastUpdate")) b->p_upd = atoll(v);
            return;
        case AB_ME:
            if (d == 3 && !strcmp(j->st[3].name, "mediaProgress"))
            {
                /* one place: kept as it closes (ajb_me_close) */
                if (!strcmp(k, "libraryItemId")) snprintf(b->p_item, sizeof b->p_item, "%s", v);
                else if (!strcmp(k, "episodeId")) snprintf(b->p_ep, sizeof b->p_ep, "%s", v);
                else if (!strcmp(k, "currentTime")) b->p_cur = aj_secs_ms(v);
                else if (!strcmp(k, "duration")) b->p_dur = aj_secs_ms(v);
                else if (!strcmp(k, "progress")) b->cur.permille = (uint16_t)AJ_CLAMP((long)(atof(v) * 1000 + 0.5), 0, 1000);
                else if (!strcmp(k, "isFinished")) b->p_fin = aj_true(v);
                else if (!strcmp(k, "lastUpdate")) b->p_upd = atoll(v);
            }
            return;
        case AB_SESSIONS:
            if (b->sessions >= 1 || d < 3 || strcmp(j->st[3].name, "sessions")) return;   /* the newest only */
            if (d == 3 && !strcmp(k, "updatedAt")) b->s_upd = atoll(v);
            else if (d == 4 && !strcmp(j->st[4].name, "deviceInfo"))
            {
                if (!strcmp(k, "deviceName")) snprintf(b->device, sizeof b->device, "%s", v);
                else if (!strcmp(k, "manufacturer")) snprintf(b->d_make, sizeof b->d_make, "%s", v);
                else if (!strcmp(k, "model")) snprintf(b->d_model, sizeof b->d_model, "%s", v);
                else if (!strcmp(k, "clientName")) snprintf(b->d_client, sizeof b->d_client, "%s", v);
                else if (!strcmp(k, "osName")) snprintf(b->d_os, sizeof b->d_os, "%s", v);
                else if (!strcmp(k, "browserName")) snprintf(b->d_browser, sizeof b->d_browser, "%s", v);
            }
            return;
    }
    if (!b->l) return;
    if (b->mode == AB_ITEMS || b->mode == AB_SERIES) { if (d == 1 && !strcmp(k, "total")) b->l->total = atoi(v); }
    if (b->sub_d)
    {
        aj_rel(j, b->sub_d, p, sizeof p);
        aj_sub_field(b->l, &b->cur, p, v);
        return;
    }
    if (!b->item_d) return;
    aj_rel(j, b->item_d, p, sizeof p);
    struct sl_abs_item *it = b->mode == AB_ITEM ? &b->l->head : &b->cur;
    if (b->mode == AB_SERIES && !strncmp(p, "books.books.", 12)) { if (!strcmp(p, "books.books.id")) it->count++; return; }
    aj_item_field(b->l, it, p, v, b->mode);
}
/* /api/me's places, one at a time as each closes */
static void aj_prog_set(const char *item, const char *ep, uint16_t permille, bool fin, uint32_t cur, long long upd);
static void ajb_me_close(void *cb, struct sj_json *j)
{
    struct aj_b *b = cb;
    if (b->mode == AB_SESSIONS && j->d == 3 && !strcmp(j->st[3].name, "sessions")) { b->sessions++; return; }
    if (b->mode != AB_ME || j->d != 3 || strcmp(j->st[3].name, "mediaProgress")) return;
    if (b->p_item[0])
    {
        pthread_mutex_lock(&aj_lock);
        aj_prog_set(b->p_item, b->p_ep, b->cur.permille, b->p_fin, b->p_cur, b->p_upd);
        pthread_mutex_unlock(&aj_lock);
    }
    b->p_item[0] = b->p_ep[0] = '\0';
    b->p_cur = b->p_dur = 0;
    b->p_fin = false;
    b->p_upd = 0;
    b->cur.permille = 0;
}
static void ajb_close_any(void *cb, struct sj_json *j)
{
    struct aj_b *b = cb;
    if (b->mode == AB_ME || b->mode == AB_SESSIONS) ajb_me_close(cb, j);
    else ajb_close(cb, j);
}

/* ---------------------------------------------------------------- the server */
static void aj_srv_load(void)
{
    if (aj.srv_read) return;
    aj.srv_read = true;
    aj.have_srv = false;
    size_t len;
    char *b = pj_slurp(AJ_SERVER, 16 * 1024, &len);
    if (!b) return;
    char *s = b, *line = strsep(&s, "\n");
    if (line && !strcmp(line, "ABS1") && (line = strsep(&s, "\n")) && line[0])
    {
        char *f = line;
        struct aj_srv *v = &aj.srv;
        memset(v, 0, sizeof *v);
        snprintf(v->pub.url, sizeof v->pub.url, "%s", pj_next(&f));
        snprintf(v->pub.user, sizeof v->pub.user, "%s", pj_next(&f));
        v->pub.api_token = atoi(pj_next(&f)) & 1;
        snprintf(v->pub.version, sizeof v->pub.version, "%s", pj_next(&f));
        snprintf(v->pub.name, sizeof v->pub.name, "%s", pj_next(&f));
        bool ok = sj_unhex(v->token, sizeof v->token, pj_next(&f));
        ok = ok && sj_unhex(v->refresh, sizeof v->refresh, pj_next(&f));
        ok = ok && sj_unhex(v->secret, sizeof v->secret, pj_next(&f));
        aj.have_srv = ok && v->pub.url[0] && v->token[0];
        if (!aj.have_srv) memset(v, 0, sizeof *v);
    }
    memset(b, 0, len);
    free(b);
}
/* the owner's only, written aside then put in place, the buffer wiped */
static bool aj_srv_save(void)
{
    if (aj.hold) return false;
    char p[512], tmp[520];
    pj_mkdirs(SL_ABS_STATE);
    if (!pj_real(p, sizeof p, AJ_SERVER) || snprintf(tmp, sizeof tmp, "%s.new", p) >= (int)sizeof tmp) return false;
    if (!aj.have_srv) { unlink(p); aj.st.server_seq++; aj.st.seq++; return true; }
    struct pj_buf o = { 0 };
    const struct aj_srv *v = &aj.srv;
    char a[3][200], h1[2 * AJ_TOK + 1], h2[2 * AJ_TOK + 1], h3[2 * sizeof v->secret + 1];
    sj_hex(h1, (const unsigned char *)v->token, strlen(v->token));
    sj_hex(h2, (const unsigned char *)v->refresh, strlen(v->refresh));
    sj_hex(h3, (const unsigned char *)v->secret, strlen(v->secret));
    pj_add(&o, "ABS1\n%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\n", v->pub.url, pj_f(v->pub.user, a[0], sizeof a[0]), v->pub.api_token ? 1 : 0,
           pj_f(v->pub.version, a[1], 16), pj_f(v->pub.name, a[2], sizeof a[2]), h1, h2, h3);
    memset(h1, 0, sizeof h1); memset(h2, 0, sizeof h2); memset(h3, 0, sizeof h3);
    bool ok = false;
    if (!o.bad && o.b)
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
    aj.st.server_seq++;
    aj.st.seq++;
    return ok;
}

/* ---------------------------------------------------------------- the books on the card */
/* a path from our own lists (rel or dir), read back: under the card's root,
 * never climbing out of a folder (a "..") nor into .rockbox */
static bool aj_rel_ok(const char *r)
{
    if (r[0] == '/') r++;
    if (!r[0] || !strncmp(r, ".rockbox", 8)) return false;
    for (const char *s = r; s; s = strchr(s, '/') ? strchr(s, '/') + 1 : NULL)
        if (s[0] == '.' && s[1] == '.' && (!s[2] || s[2] == '/')) return false;
    return true;
}
static void aj_books_load(void)
{
    if (aj.books_read) return;
    aj.books_read = true;
    aj.n_books = 0;
    char *b = pj_slurp(AJ_BOOKS, 512 * 1024, NULL);
    char *s = b, *line = b ? strsep(&s, "\n") : NULL;
    if (line && !strcmp(line, "ABB1"))
        while ((line = strsep(&s, "\n")))
        {
            if (!line[0] || aj.n_books >= SL_ABS_BOOKS) continue;
            if (aj.n_books == aj.cap_books)
            {
                int want = aj.cap_books ? aj.cap_books * 2 : 8;
                struct sl_abs_book *n = realloc(aj.books, (size_t)want * sizeof *n);
                if (!n) break;
                aj.books = n;
                aj.cap_books = want;
            }
            char *f = line;
            struct sl_abs_book *k = &aj.books[aj.n_books];
            memset(k, 0, sizeof *k);
            snprintf(k->item, sizeof k->item, "%s", pj_next(&f));
            snprintf(k->episode, sizeof k->episode, "%s", pj_next(&f));
            snprintf(k->dir, sizeof k->dir, "%s", pj_next(&f));
            snprintf(k->title, sizeof k->title, "%s", pj_next(&f));
            snprintf(k->author, sizeof k->author, "%s", pj_next(&f));
            k->dur_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            k->size = atoll(pj_next(&f));
            k->seen = atoll(pj_next(&f));
            k->whole = atoi(pj_next(&f)) != 0;
            k->files = atoi(pj_next(&f));
            if (k->item[0] && k->dir[0] && aj_rel_ok(k->dir)) aj.n_books++;
        }
    free(b);
}
static void aj_books_save(void)
{
    if (aj.hold) return;
    struct pj_buf o = { 0 };
    pj_add(&o, "ABB1\n");
    for (int i = 0; i < aj.n_books; i++)
    {
        const struct sl_abs_book *k = &aj.books[i];
        char a[5][300];   /* every text a line of its own, whatever the server sent (a "\n" in an id) */
        pj_add(&o, "%s\t%s\t%s\t%s\t%s\t%u\t%lld\t%lld\t%d\t%d\n", pj_f(k->item, a[3], sizeof a[3]), pj_f(k->episode, a[4], sizeof a[4]), pj_f(k->dir, a[0], sizeof a[0]),
               pj_f(k->title, a[1], sizeof a[1]), pj_f(k->author, a[2], sizeof a[2]), (unsigned)k->dur_ms, k->size, k->seen,
               k->whole ? 1 : 0, k->files);
    }
    pj_mkdirs(SL_ABS_STATE);
    if (!o.bad && o.b) pj_write(AJ_BOOKS, o.b, o.n);
    free(o.b);
}
static struct sl_abs_book *aj_book_find(const char *item, const char *ep)
{
    aj_books_load();
    for (int i = 0; i < aj.n_books; i++)
        if (!strcmp(aj.books[i].item, item) && !strcmp(aj.books[i].episode, ep ? ep : "")) return &aj.books[i];
    return NULL;
}

/* ---------------------------------------------------------------- what the server said of each place */
static struct aj_prog *aj_prog_find(const char *item, const char *ep)
{
    uint32_t key = aj_key(item, ep);
    for (int i = 0; i < aj.n_prog; i++) if (aj.prog[i].key == key) return &aj.prog[i];
    return NULL;
}
static void aj_prog_set(const char *item, const char *ep, uint16_t permille, bool fin, uint32_t cur, long long upd)
{
    struct aj_prog *p = aj_prog_find(item, ep);
    if (!p)
    {
        if (!aj.prog) aj.prog = calloc(AJ_PROG_MAX, sizeof *aj.prog);
        if (!aj.prog) return;
        p = aj.n_prog < AJ_PROG_MAX ? &aj.prog[aj.n_prog++] : &aj.prog[(aj.n_prog - 1)];
        p->key = aj_key(item, ep);
    }
    p->permille = permille;
    p->fin = fin;
    p->cur_ms = cur;
    p->upd = upd;
}
/* rows say how far, and whether they are on the card */
static void aj_mark(struct sl_abs_list *l)
{
    pthread_mutex_lock(&aj_lock);
    for (int i = -1; i < l->n; i++)
    {
        struct sl_abs_item *it = i < 0 ? &l->head : &l->it[i];
        if (i < 0 && l->what != SL_ABS_Q_ITEM) continue;
        const char *id = sl_abs_s(l, it->id), *ep = "";
        if (it->kind == SL_ABS_EPISODE) { id = sl_abs_s(l, l->head.id); ep = sl_abs_s(l, it->episode); }
        else if (it->kind != SL_ABS_BOOK && it->kind != SL_ABS_PODCAST) continue;
        if (l->what == SL_ABS_Q_CONTINUE) ep = sl_abs_s(l, it->episode);
        struct aj_prog *p = aj_prog_find(id, ep);
        if (p && (!it->updated || p->upd >= it->updated))
        {
            it->permille = p->permille;
            it->finished = p->fin;
            it->cur_ms = p->cur_ms;
            it->updated = p->upd;
        }
        if (!it->permille && it->cur_ms && it->dur_ms) it->permille = (uint16_t)AJ_CLAMP((long)((uint64_t)it->cur_ms * 1000 / it->dur_ms), 0, 1000);
        struct sl_abs_book *k = aj_book_find(id, ep);
        it->local = k ? (k->whole ? 1 : 2) : 0;
    }
    pthread_mutex_unlock(&aj_lock);
}

/* ---------------------------------------------------------------- a request */
struct aj_sink { struct sj_json j; struct aj_b b; };
static bool aj_json_sink(void *ctx, const char *data, size_t n)
{
    struct aj_sink *m = ctx;
    if (!m->b.json && m->b.rawn + 1 < sizeof m->b.raw)
    {
        size_t k = n < sizeof m->b.raw - 1 - m->b.rawn ? n : sizeof m->b.raw - 1 - m->b.rawn;
        memcpy(m->b.raw + m->b.rawn, data, k);
        m->b.rawn += k;
        m->b.raw[m->b.rawn] = '\0';
    }
    sj_feed(&m->j, NULL, data, n);
    return !m->j.bad;
}
/* one call; *b gets what it built. The HTTP status (or -1), the error in *err */
static int aj_http(const struct aj_srv *s, const char *method, const char *path, const char *body, const char *extra,
                   bool auth, struct aj_b *b, int *err)
{
    char url[1200], hdr[AJ_TOK + 200];
    if (snprintf(url, sizeof url, "%s%s", s->pub.url, path) >= (int)sizeof url) { *err = SL_ABS_E_ADDRESS; return -1; }
    snprintf(hdr, sizeof hdr, "%s%s%s%s", auth ? "Authorization: Bearer " : "", auth ? s->token : "", auth ? "\r\n" : "", extra ? extra : "");
    struct aj_sink *m = calloc(1, sizeof *m);
    if (!m) { *err = SL_ABS_E_NOMEM; return -1; }
    m->b = *b;
    m->j.cb = &m->b;
    m->j.cb_open = ajb_open;
    m->j.cb_close = ajb_close_any;
    m->j.cb_value = ajb_value;
    struct sl_net_req r;
    memset(&r, 0, sizeof r);
    r.method = method; r.url = url; r.ua = SL_ABS_UA; r.headers = hdr; r.body = body;
    r.timeout_ms = 20000; r.stop = &aj.stop_job; r.sink = aj_json_sink; r.ctx = m;
    int status = sl_net_http_req(&r);
    memset(hdr, 0, sizeof hdr);
    memset(url, 0, sizeof url);
    if (m->j.in_lit) sj_lit_end(&m->j, NULL);
    if (r.date > 0)
    {
        pthread_mutex_lock(&aj_lock);
        aj.skew = r.date - (long long)time(NULL);
        aj.skew_known = true;
        pthread_mutex_unlock(&aj_lock);
    }
    if (status == 401 || status == 403) *err = SL_ABS_E_AUTH;
    else if (status == 404) *err = SL_ABS_E_NOTFOUND;
    else if (status == 310) *err = SL_ABS_E_ELSEWHERE;
    else if (status < 0) *err = r.https_failed ? SL_ABS_E_HTTPS : SL_ABS_E_NET;
    else if (status >= 200 && status < 300) *err = SL_ABS_OK;
    else *err = m->b.json ? SL_ABS_E_SERVER : status >= 500 || m->b.rawn ? SL_ABS_E_SERVER : SL_ABS_E_NOTABS;
    *b = m->b;
    aj_utf8_end(b->raw);
    free(m);
    return status;
}
/* the tokens again: a refresh token's new pair, else the saved password signs in anew */
static int aj_relogin(struct aj_srv *s)
{
    struct aj_b b;
    int err = SL_ABS_E_AUTH;
    if (s->refresh[0])
    {
        char extra[AJ_TOK + 40];
        snprintf(extra, sizeof extra, "x-refresh-token: %s\r\n", s->refresh);
        memset(&b, 0, sizeof b);
        b.mode = AB_LOGIN;
        aj_http(s, "POST", "/auth/refresh", "{}", extra, false, &b, &err);
        memset(extra, 0, sizeof extra);
        if (!err && (b.access[0] || b.token[0]))
        {
            snprintf(s->token, sizeof s->token, "%s", b.access[0] ? b.access : b.token);
            if (b.refresh[0]) snprintf(s->refresh, sizeof s->refresh, "%s", b.refresh);
        }
        else err = SL_ABS_E_AUTH;
    }
    if (err && s->secret[0] && s->pub.user[0])
    {
        char u[200], pw[400], body[700];
        aj_json_str(u, sizeof u, s->pub.user);
        aj_json_str(pw, sizeof pw, s->secret);
        snprintf(body, sizeof body, "{\"username\":\"%s\",\"password\":\"%s\"}", u, pw);
        memset(&b, 0, sizeof b);
        b.mode = AB_LOGIN;
        aj_http(s, "POST", "/login", body, "x-return-tokens: true\r\n", false, &b, &err);
        memset(body, 0, sizeof body); memset(pw, 0, sizeof pw);
        if (!err && (b.access[0] || b.token[0]))
        {
            snprintf(s->token, sizeof s->token, "%s", b.access[0] ? b.access : b.token);
            snprintf(s->refresh, sizeof s->refresh, "%s", b.refresh);
        }
        else if (!err) err = SL_ABS_E_AUTH;
    }
    memset(&b, 0, sizeof b);
    if (!err)
    {
        pthread_mutex_lock(&aj_lock);
        if (aj.have_srv && !strcmp(aj.srv.pub.url, s->pub.url))
        {
            snprintf(aj.srv.token, sizeof aj.srv.token, "%s", s->token);
            snprintf(aj.srv.refresh, sizeof aj.srv.refresh, "%s", s->refresh);
            aj_srv_save();
        }
        pthread_mutex_unlock(&aj_lock);
    }
    return err;
}
/* a copy of the server to call with, the lock let go meanwhile; false: none */
static bool aj_srv_copy(struct aj_srv *out)
{
    pthread_mutex_lock(&aj_lock);
    aj_srv_load();
    bool have = aj.have_srv;
    if (have) *out = aj.srv;
    pthread_mutex_unlock(&aj_lock);
    return have;
}
/* a call with the server's token, signed in again once when it is refused */
static int aj_call(struct aj_srv *s, const char *method, const char *path, const char *body, struct aj_b *b, int *status)
{
    struct aj_b keep = *b;
    int err;
    int st = aj_http(s, method, path, body, NULL, true, b, &err);
    if (err == SL_ABS_E_AUTH && !s->pub.api_token && !aj_relogin(s))
    {
        *b = keep;
        st = aj_http(s, method, path, body, NULL, true, b, &err);
    }
    if (status) *status = st;
    if (b->l && err == SL_ABS_E_SERVER) snprintf(b->l->why, sizeof b->l->why, "%s", b->raw);
    return err;
}

/* ---------------------------------------------------------------- signing in */
static void aj_signin(struct aj_job *j)
{
    struct aj_srv t;
    memset(&t, 0, sizeof t);
    int err = SL_ABS_OK;
    pthread_mutex_lock(&aj_lock);
    aj_srv_load();
    struct aj_srv old = aj.srv;
    bool had = aj.have_srv;
    pthread_mutex_unlock(&aj_lock);
    if (!sl_sub_url_fix(j->url, t.pub.url, sizeof t.pub.url)) err = SL_ABS_E_ADDRESS;
    snprintf(t.pub.user, sizeof t.pub.user, "%s", j->user);
    /* nothing typed: what was saved for this address */
    bool same = had && !strcmp(old.pub.url, t.pub.url) && !strcmp(old.pub.user, t.pub.user);
    char secret[AJ_TOK + 8];
    snprintf(secret, sizeof secret, "%s", j->secret);
    memset(j->secret, 0, sizeof j->secret);
    if (!secret[0] && same) snprintf(secret, sizeof secret, "%s", old.pub.api_token ? old.token : old.secret);
    struct aj_b b;
    memset(&b, 0, sizeof b);
    b.mode = AB_STATUS;
    if (!err) aj_http(&t, "GET", "/status", NULL, NULL, false, &b, &err);
    if (!err && !b.is_abs) err = SL_ABS_E_NOTABS;
    if (err == SL_ABS_E_NOTFOUND) err = SL_ABS_E_NOTABS;
    snprintf(t.pub.version, sizeof t.pub.version, "%s", b.version);
    /* a token looks like one: long, three parts with dots */
    const char *d1 = strchr(secret, '.');
    bool looks_token = strlen(secret) > 60 && d1 && strchr(d1 + 1, '.') && !strchr(secret, ' ');
    if (!err && t.pub.user[0])
    {
        char u[200], pw[400], body[700];
        aj_json_str(u, sizeof u, t.pub.user);
        aj_json_str(pw, sizeof pw, secret);
        snprintf(body, sizeof body, "{\"username\":\"%s\",\"password\":\"%s\"}", u, pw);
        memset(&b, 0, sizeof b);
        b.mode = AB_LOGIN;
        aj_http(&t, "POST", "/login", body, "x-return-tokens: true\r\n", false, &b, &err);
        memset(body, 0, sizeof body); memset(pw, 0, sizeof pw);
        if (!err && (b.access[0] || b.token[0]))
        {
            snprintf(t.token, sizeof t.token, "%s", b.access[0] ? b.access : b.token);
            snprintf(t.refresh, sizeof t.refresh, "%s", b.refresh);
            snprintf(t.secret, sizeof t.secret, "%s", secret);
        }
        else if (!err) err = SL_ABS_E_NOTABS;
        if (err == SL_ABS_E_AUTH && looks_token) t.pub.user[0] = '\0';   /* an API token where the password goes */
    }
    if ((!err || err == SL_ABS_E_AUTH) && !t.pub.user[0])
    {
        /* an API token: asked who it is */
        snprintf(t.token, sizeof t.token, "%s", secret);
        t.pub.api_token = true;
        memset(&b, 0, sizeof b);
        b.mode = AB_LOGIN;
        aj_http(&t, "POST", "/api/authorize", NULL, NULL, true, &b, &err);
        if (!err && !b.user_seen) err = SL_ABS_E_NOTABS;
        if (!err) snprintf(t.pub.user, sizeof t.pub.user, "%s", b.user);
    }
    memset(secret, 0, sizeof secret);
    if (!err)
    {
        const char *host = strstr(t.pub.url, "://");
        host = host ? host + 3 : t.pub.url;
        snprintf(t.pub.name, sizeof t.pub.name, "%.*s", (int)strcspn(host, "/:"), host);
    }
    pthread_mutex_lock(&aj_lock);
    aj.st.test_err = err;
    snprintf(aj.st.test_version, sizeof aj.st.test_version, "%s", t.pub.version);
    snprintf(aj.st.test_why, sizeof aj.st.test_why, "%s", err == SL_ABS_E_SERVER ? b.raw : "");
    aj.st.test_saved = false;
    if (!err && j->save && !aj.hold)
    {
        aj.srv = t;
        aj.have_srv = true;
        aj.st.test_saved = aj_srv_save();
    }
    memset(&t, 0, sizeof t);
    memset(&b, 0, sizeof b);
    aj.st.test_seq++;
    aj.st.seq++;
    pthread_mutex_unlock(&aj_lock);
}

/* ---------------------------------------------------------------- the card's answers (offline) */
static void aj_from_card(int what, const char *arg, struct sl_abs_list *l)
{
    pthread_mutex_lock(&aj_lock);
    aj_books_load();
    if (what == SL_ABS_Q_ITEM)
    {
        struct sl_abs_book *k = NULL;
        for (int i = 0; i < aj.n_books && !k; i++) if (!strcmp(aj.books[i].item, arg)) k = &aj.books[i];
        if (!k) { l->err = SL_ABS_E_OFFLINE; pthread_mutex_unlock(&aj_lock); return; }
        l->head.kind = k->episode[0] ? SL_ABS_PODCAST : SL_ABS_BOOK;
        l->head.id = aj_pool(l, k->item);
        l->head.title = aj_pool(l, k->title);
        l->head.author = aj_pool(l, k->author);
        l->head.dur_ms = k->dur_ms;
        l->head.size = k->size;
        l->head.local = k->whole ? 1 : 2;
        /* its chapters, as they were kept */
        char name[96];
        snprintf(name, sizeof name, SL_ABS_STATE "/ch/%08x.txt", (unsigned)aj_key(k->item, k->episode));
        char *b = pj_slurp(name, 256 * 1024, NULL), *s = b, *line;
        while (b && (line = strsep(&s, "\n")))
        {
            if (!line[0]) continue;
            char *f = line;
            struct sl_abs_item c;
            memset(&c, 0, sizeof c);
            c.kind = SL_ABS_CHAPTER;
            c.start_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            c.dur_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
            c.title = aj_pool(l, pj_next(&f));
            aj_add(l, &c);
        }
        free(b);
        for (int i = 0; i < aj.n_books; i++)
            if (!strcmp(aj.books[i].item, arg) && aj.books[i].episode[0])
            {
                struct sl_abs_item e;
                memset(&e, 0, sizeof e);
                e.kind = SL_ABS_EPISODE;
                e.episode = aj_pool(l, aj.books[i].episode);
                e.title = aj_pool(l, aj.books[i].title);
                e.dur_ms = aj.books[i].dur_ms;
                e.size = aj.books[i].size;
                aj_add(l, &e);
            }
    }
    else
    {
        for (int i = 0; i < aj.n_books; i++)
        {
            const struct sl_abs_book *k = &aj.books[i];
            struct sl_abs_item it;
            memset(&it, 0, sizeof it);
            it.kind = SL_ABS_BOOK;
            it.id = aj_pool(l, k->item);
            it.episode = aj_pool(l, k->episode);
            it.title = aj_pool(l, k->title);
            it.author = aj_pool(l, k->author);
            it.dur_ms = k->dur_ms;
            it.size = k->size;
            it.local = k->whole ? 1 : 2;
            aj_add(l, &it);
        }
    }
    pthread_mutex_unlock(&aj_lock);
}

/* ---------------------------------------------------------------- answers */
static void aj_answer(const struct sl_abs_list *l)
{
    for (int i = 0; i < 8; i++) if (aj.forgot[i] == l->req) { struct sl_abs_list x = *l; sl_abs_list_free(&x); return; }
    int at = -1;
    for (int i = 0; i < AJ_ANSWERS && at < 0; i++) if (!aj.ans_ok[i]) at = i;
    if (at < 0) { at = 0; sl_abs_list_free(&aj.ans[0]); }
    aj.ans[at] = *l;
    aj.ans_ok[at] = true;
    aj.st.list_seq++;
    aj.st.seq++;
}
static int aj_item_cmp(const void *a, const void *b)
{
    const struct sl_abs_item *x = a, *y = b;
    if (x->podcast != y->podcast) return x->podcast - y->podcast;   /* books first */
    return 0;
}
static void aj_pick_from(const char *item, const char *ep, uint32_t cur, uint32_t dur, bool fin, long long upd, struct aj_srv *s);
static void aj_list(struct aj_job *j)
{
    struct sl_abs_list l;
    memset(&l, 0, sizeof l);
    l.req = j->req;
    l.what = j->what;
    l.page = j->page;
    struct aj_srv s;
    pthread_mutex_lock(&aj_lock);
    bool online = aj.online;
    pthread_mutex_unlock(&aj_lock);
    bool have = aj_srv_copy(&s);
    if (j->what == SL_ABS_Q_LOCAL || !online || !have)
    {
        if (!have && j->what != SL_ABS_Q_LOCAL) l.err = SL_ABS_E_NOSERVER;
        else { l.offline = j->what != SL_ABS_Q_LOCAL; aj_from_card(j->what == SL_ABS_Q_ITEM ? SL_ABS_Q_ITEM : SL_ABS_Q_LOCAL, j->arg, &l); }
        if (!l.err) aj_mark(&l);
        memset(&s, 0, sizeof s);
        pthread_mutex_lock(&aj_lock);
        aj_answer(&l);
        pthread_mutex_unlock(&aj_lock);
        return;
    }
    char path[600], a[160], f[400];
    lj_enc(a, sizeof a, j->arg);
    struct aj_b b;
    memset(&b, 0, sizeof b);
    b.l = &l;
    int err = SL_ABS_OK;
    switch (j->what)
    {
        case SL_ABS_Q_LIBRARIES:
            b.mode = AB_LIBS;
            err = aj_call(&s, "GET", "/api/libraries", NULL, &b, NULL);
            if (!err && l.n > 1) qsort(l.it, (size_t)l.n, sizeof *l.it, aj_item_cmp);
            break;
        case SL_ABS_Q_ITEMS:
            b.mode = AB_ITEMS;
            if (j->arg2[0]) lj_enc(f, sizeof f, j->arg2); else f[0] = '\0';
            snprintf(path, sizeof path, "/api/libraries/%s/items?limit=%d&page=%d&sort=media.metadata.title&minified=1%s%s", a, SL_ABS_PAGE_N,
                     j->page, f[0] ? "&filter=" : "", f);
            err = aj_call(&s, "GET", path, NULL, &b, NULL);
            break;
        case SL_ABS_Q_AUTHORS:
            b.mode = AB_AUTHORS;
            snprintf(path, sizeof path, "/api/libraries/%s/authors", a);
            err = aj_call(&s, "GET", path, NULL, &b, NULL);
            break;
        case SL_ABS_Q_SERIES:
            b.mode = AB_SERIES;
            snprintf(path, sizeof path, "/api/libraries/%s/series?limit=100&page=%d&sort=name", a, j->page);
            err = aj_call(&s, "GET", path, NULL, &b, NULL);
            break;
        case SL_ABS_Q_CONTINUE:
        case SL_ABS_Q_ITEM:
        {
            /* the places first (/api/me), then the list or the item */
            struct aj_b m;
            memset(&m, 0, sizeof m);
            m.mode = AB_ME;
            err = aj_call(&s, "GET", "/api/me", NULL, &m, NULL);
            if (err && err != SL_ABS_E_NOTFOUND) break;
            b.mode = j->what == SL_ABS_Q_ITEM ? AB_ITEM : AB_INPROG;
            if (j->what == SL_ABS_Q_ITEM) snprintf(path, sizeof path, "/api/items/%s?expanded=1&include=progress", a);
            else snprintf(path, sizeof path, "/api/me/items-in-progress?limit=25");
            err = aj_call(&s, "GET", path, NULL, &b, NULL);
            break;
        }
    }
    if (!err && j->what == SL_ABS_Q_ITEM)
    {
        /* tracks known: the plain audio files list is not needed; else they are the files */
        bool tracks = false;
        for (int i = 0; i < l.n; i++) tracks |= l.it[i].kind == SL_ABS_FILE;
        int w = 0;
        for (int i = 0; i < l.n; i++)
        {
            struct sl_abs_item *it = &l.it[i];
            if (it->kind == (SL_ABS_FILE | AJ_AFILE))
            {
                if (tracks || it->finished == 2) continue;
                it->kind = SL_ABS_FILE;
                char u[200];
                snprintf(u, sizeof u, "/api/items/%s/file/%s", sl_abs_s(&l, l.head.id), sl_abs_s(&l, it->id));
                it->url = aj_pool(&l, u);
                it->id = 0;
            }
            if (it->kind == SL_ABS_FILE && it->finished == 2) continue;
            if (it->kind == SL_ABS_EPISODE && !it->url && it->id)
            {
                char u[200];
                snprintf(u, sizeof u, "/api/items/%s/file/%s", sl_abs_s(&l, l.head.id), sl_abs_s(&l, it->id));
                it->url = aj_pool(&l, u);
            }
            l.it[w++] = *it;
        }
        l.n = w;
        if (l.head.podcast) l.head.kind = SL_ABS_PODCAST;
    }
    if (err == SL_ABS_E_NET || err == SL_ABS_E_HTTPS)
    {
        /* not reachable: what the card has */
        sl_abs_list_free(&l);
        memset(&l, 0, sizeof l);
        l.req = j->req;
        l.what = j->what;
        if (j->what == SL_ABS_Q_ITEM || j->what == SL_ABS_Q_CONTINUE || j->what == SL_ABS_Q_LIBRARIES)
        {
            l.offline = true;
            aj_from_card(j->what == SL_ABS_Q_ITEM ? SL_ABS_Q_ITEM : SL_ABS_Q_LOCAL, j->arg, &l);
            if (l.err) l.err = err;
        }
        else l.err = err;
    }
    else l.err = err;
    if (!l.err) aj_mark(&l);
    /* the server moved a book on the card since we last knew: its place comes as a pick */
    if (!l.err && !l.offline && (j->what == SL_ABS_Q_ITEM || j->what == SL_ABS_Q_CONTINUE))
        for (int i = -1; i < l.n; i++)
        {
            const struct sl_abs_item *it = i < 0 ? &l.head : &l.it[i];
            if (i < 0 && j->what != SL_ABS_Q_ITEM) continue;
            const char *id = i < 0 || it->kind != SL_ABS_EPISODE ? sl_abs_s(&l, it->id) : sl_abs_s(&l, l.head.id);
            const char *ep = it->kind == SL_ABS_EPISODE ? sl_abs_s(&l, it->episode) : j->what == SL_ABS_Q_CONTINUE ? sl_abs_s(&l, it->episode) : "";
            if (!it->local || !it->updated) continue;
            aj_pick_from(id, ep, it->cur_ms, it->dur_ms, it->finished, it->updated, &s);
        }
    memset(&s, 0, sizeof s);
    pthread_mutex_lock(&aj_lock);
    aj_answer(&l);
    pthread_mutex_unlock(&aj_lock);
}

/* ---------------------------------------------------------------- the place */
static void aj_pend_load(void)
{
    if (aj.pend_read) return;
    aj.pend_read = true;
    if (!aj.pend) aj.pend = calloc(AJ_PEND_MAX, sizeof *aj.pend);
    char *b = pj_slurp(AJ_PEND, 64 * 1024, NULL), *s = b, *line;
    while (b && aj.pend && (line = strsep(&s, "\n")))
    {
        if (!line[0] || aj.n_pend >= AJ_PEND_MAX) continue;
        char *f = line;
        struct aj_pend *p = &aj.pend[aj.n_pend];
        snprintf(p->item, sizeof p->item, "%s", pj_next(&f));
        snprintf(p->ep, sizeof p->ep, "%s", pj_next(&f));
        p->cur_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        p->dur_ms = (uint32_t)strtoul(pj_next(&f), NULL, 10);
        p->fin = atoi(pj_next(&f)) != 0;
        p->t = atoll(pj_next(&f));
        if (p->item[0]) aj.n_pend++;
    }
    free(b);
    aj.st.pending = aj.n_pend;
}
static void aj_pend_save(void)
{
    if (aj.hold) return;
    struct pj_buf o = { 0 };
    for (int i = 0; i < aj.n_pend; i++)
    {
        char a[2][300];
        pj_add(&o, "%s\t%s\t%u\t%u\t%d\t%lld\n", pj_f(aj.pend[i].item, a[0], sizeof a[0]), pj_f(aj.pend[i].ep, a[1], sizeof a[1]),
               (unsigned)aj.pend[i].cur_ms, (unsigned)aj.pend[i].dur_ms, aj.pend[i].fin ? 1 : 0, aj.pend[i].t);
    }
    char p[512];
    if (!o.b) { if (pj_real(p, sizeof p, AJ_PEND)) unlink(p); }
    else if (!o.bad) { pj_mkdirs(SL_ABS_STATE); pj_write(AJ_PEND, o.b, o.n); }
    free(o.b);
    aj.st.pending = aj.n_pend;
}
static int aj_pend_find(const char *item, const char *ep)
{
    for (int i = 0; i < aj.n_pend; i++) if (!strcmp(aj.pend[i].item, item) && !strcmp(aj.pend[i].ep, ep)) return i;
    return -1;
}
static void aj_pend_drop(int i)
{
    memmove(&aj.pend[i], &aj.pend[i + 1], (size_t)(aj.n_pend - i - 1) * sizeof *aj.pend);
    aj.n_pend--;
}
/* which device the place came from: the item's latest listening session, when it is that one */
static void aj_device(struct aj_srv *s, const char *item, const char *ep, long long upd, char *out, size_t n)
{
    out[0] = '\0';
    char path[300], a[160], e[160];
    lj_enc(a, sizeof a, item);
    lj_enc(e, sizeof e, ep);
    snprintf(path, sizeof path, "/api/me/item/listening-sessions/%s%s%s?itemsPerPage=1&page=0", a, ep[0] ? "/" : "", e);
    struct aj_b b;
    memset(&b, 0, sizeof b);
    b.mode = AB_SESSIONS;
    int err = aj_call(s, "GET", path, NULL, &b, NULL);
    long long gap = b.s_upd > upd ? b.s_upd - upd : upd - b.s_upd;
    if (err || !b.s_upd || gap > 10 * 60 * 1000) return;
    if (b.device[0]) snprintf(out, n, "%s", b.device);
    else if (b.d_model[0]) snprintf(out, n, "%s%s%s", b.d_make, b.d_make[0] ? " " : "", b.d_model);
    else if (b.d_browser[0] && b.d_os[0]) snprintf(out, n, "%s, %s", b.d_browser, b.d_os);
    else if (b.d_client[0]) snprintf(out, n, "%s", b.d_client);
}
/* the server's place moved after ours: it wins, unless ours (waiting to be told) is newer still */
static void aj_pick_from(const char *item, const char *ep, uint32_t cur, uint32_t dur, bool fin, long long upd, struct aj_srv *s)
{
    pthread_mutex_lock(&aj_lock);
    struct sl_abs_book *k = aj_book_find(item, ep);
    if (!k || upd <= k->seen + 1000) { pthread_mutex_unlock(&aj_lock); return; }
    aj_pend_load();
    int pi = aj_pend_find(item, ep);
    if (pi >= 0 && (aj.pend[pi].t + aj.skew) * 1000 >= upd) { pthread_mutex_unlock(&aj_lock); return; }   /* ours goes */
    if (pi >= 0) { aj_pend_drop(pi); aj_pend_save(); }
    k->seen = upd;
    aj_books_save();
    pthread_mutex_unlock(&aj_lock);
    struct sl_abs_pick pk;
    memset(&pk, 0, sizeof pk);
    snprintf(pk.item, sizeof pk.item, "%s", item);
    snprintf(pk.episode, sizeof pk.episode, "%s", ep);
    pk.cur_ms = cur;
    pk.dur_ms = dur;
    pk.finished = fin;
    pk.updated = upd;
    aj_device(s, item, ep, upd, pk.device, sizeof pk.device);
    pthread_mutex_lock(&aj_lock);
    if (aj.n_picks == AJ_PICKS) memmove(&aj.picks[0], &aj.picks[1], (AJ_PICKS - 1) * sizeof *aj.picks), aj.n_picks--;
    aj.picks[aj.n_picks++] = pk;
    aj.st.pick_seq++;
    aj.st.seq++;
    pthread_mutex_unlock(&aj_lock);
}
static void aj_progress_path(char *out, size_t n, const char *item, const char *ep)
{
    char a[160], e[160];
    lj_enc(a, sizeof a, item);
    lj_enc(e, sizeof e, ep);
    snprintf(out, n, "/api/me/progress/%s%s%s", a, ep[0] ? "/" : "", e);
}
/* the server's place now: false when it could not be read (err) */
static bool aj_progress_get(struct aj_srv *s, const char *item, const char *ep, struct aj_b *b, int *err)
{
    char path[400];
    aj_progress_path(path, sizeof path, item, ep);
    memset(b, 0, sizeof *b);
    b->mode = AB_PROGRESS;
    *err = aj_call(s, "GET", path, NULL, b, NULL);
    if (*err == SL_ABS_E_NOTFOUND) { *err = SL_ABS_OK; b->p_seen = false; }   /* never played there */
    return !*err;
}
/* the oldest place waiting, told (locked; the lock let go meanwhile) */
static bool aj_pend_send(void)
{
    if (!aj.n_pend || !aj.online || aj.hold) return false;
    struct aj_pend e = aj.pend[0];
    struct aj_srv s;
    if (!aj.have_srv) { aj.n_pend = 0; aj_pend_save(); return true; }
    s = aj.srv;
    pthread_mutex_unlock(&aj_lock);
    struct aj_b b;
    int err;
    bool server_newer = false;
    if (aj_progress_get(&s, e.item, e.ep, &b, &err) && b.p_seen)
    {
        pthread_mutex_lock(&aj_lock);
        struct sl_abs_book *k = aj_book_find(e.item, e.ep);
        long long seen = k ? k->seen : 0, ours = (e.t + aj.skew) * 1000;
        pthread_mutex_unlock(&aj_lock);
        server_newer = b.p_upd > ours && b.p_upd > seen + 1000;
    }
    if (!err && server_newer)
    {
        /* last write wins: theirs */
        pthread_mutex_lock(&aj_lock);
        int i = aj_pend_find(e.item, e.ep);
        if (i >= 0 && aj.pend[i].t == e.t) { aj_pend_drop(i); aj_pend_save(); }
        pthread_mutex_unlock(&aj_lock);
        aj_pick_from(e.item, e.ep, b.p_cur, b.p_dur, b.p_fin, b.p_upd, &s);
        pthread_mutex_lock(&aj_lock);
        aj.st.sync_seq++;
        aj.st.seq++;
        memset(&s, 0, sizeof s);
        return true;
    }
    if (!err)
    {
        char path[400], body[200];
        aj_progress_path(path, sizeof path, e.item, e.ep);
        double dur = e.dur_ms / 1000.0, cur = e.fin && e.dur_ms ? dur : e.cur_ms / 1000.0;
        double pr = e.fin ? 1.0 : dur > 0 ? cur / dur : 0;
        if (pr > 1) pr = 1;
        snprintf(body, sizeof body, "{\"duration\":%.3f,\"progress\":%.5f,\"currentTime\":%.3f,\"isFinished\":%s}", dur, pr, cur,
                 e.fin ? "true" : "false");
        memset(&b, 0, sizeof b);
        b.mode = AB_PROGRESS;
        err = aj_call(&s, "PATCH", path, body, &b, NULL);
    }
    memset(&s, 0, sizeof s);
    pthread_mutex_lock(&aj_lock);
    if (err == SL_ABS_E_NET || err == SL_ABS_E_HTTPS || err == SL_ABS_E_AUTH || err == SL_ABS_E_SERVER)
    {
        aj.pend_retry_at = aj_ms() + AJ_RETRY_MS;
        if (!aj.pend_retry_at) aj.pend_retry_at = 1;
        aj.st.sync_seq++;
        aj.st.seq++;
        return false;
    }
    /* told (or gone from the server): not again, unless a newer place came meanwhile */
    int i = aj_pend_find(e.item, e.ep);
    if (i >= 0 && aj.pend[i].t == e.t && aj.pend[i].cur_ms == e.cur_ms && aj.pend[i].fin == e.fin) aj_pend_drop(i);
    if (!err)
    {
        struct sl_abs_book *k = aj_book_find(e.item, e.ep);
        if (k) { k->seen = aj_now_srv_ms(); aj_books_save(); }
        aj_prog_set(e.item, e.ep, (uint16_t)(e.fin ? 1000 : e.dur_ms ? (uint64_t)e.cur_ms * 1000 / e.dur_ms : 0), e.fin, e.cur_ms, aj_now_srv_ms());
    }
    aj_pend_save();
    aj.st.sync_seq++;
    aj.st.seq++;
    return true;
}
static void aj_pull(struct aj_job *j)
{
    struct aj_srv s;
    if (!aj_srv_copy(&s)) return;
    struct aj_b b;
    int err;
    if (aj_progress_get(&s, j->arg, j->arg2, &b, &err) && b.p_seen) aj_pick_from(j->arg, j->arg2, b.p_cur, b.p_dur, b.p_fin, b.p_upd, &s);
    memset(&s, 0, sizeof s);
}

/* ---------------------------------------------------------------- downloads */
static void aj_whole(struct sl_abs_book *k);
static void aj_q_load(void)
{
    if (aj.q_read) return;
    aj.q_read = true;
    char *b = pj_slurp(AJ_QUEUE, 1024 * 1024, NULL), *s = b, *line;
    while (b && (line = strsep(&s, "\n")))
    {
        if (!line[0] || aj.n_q >= AJ_Q_MAX) continue;
        if (aj.n_q == aj.cap_q)
        {
            int want = aj.cap_q ? aj.cap_q * 2 : 16;
            struct aj_file *n = realloc(aj.q, (size_t)want * sizeof *n);
            if (!n) break;
            aj.q = n;
            aj.cap_q = want;
        }
        char *f = line;
        struct aj_file *q = &aj.q[aj.n_q];
        memset(q, 0, sizeof *q);
        snprintf(q->item, sizeof q->item, "%s", pj_next(&f));
        snprintf(q->ep, sizeof q->ep, "%s", pj_next(&f));
        q->idx = atoi(pj_next(&f));
        q->n = atoi(pj_next(&f));
        snprintf(q->url, sizeof q->url, "%s", pj_next(&f));
        snprintf(q->rel, sizeof q->rel, "%s", pj_next(&f));
        q->size = atoll(pj_next(&f));
        snprintf(q->title, sizeof q->title, "%s", pj_next(&f));
        if (q->item[0] && q->url[0] && q->rel[0] && aj_rel_ok(q->rel)) aj.n_q++;
    }
    free(b);
    aj.st.queued = aj.n_q;
}
static void aj_q_save(void)
{
    if (aj.hold) return;
    struct pj_buf o = { 0 };
    for (int i = 0; i < aj.n_q; i++)
    {
        const struct aj_file *q = &aj.q[i];
        char a[5][600];
        pj_add(&o, "%s\t%s\t%d\t%d\t%s\t%s\t%lld\t%s\n", pj_f(q->item, a[2], sizeof a[2]), pj_f(q->ep, a[3], sizeof a[3]), q->idx, q->n,
               pj_f(q->url, a[4], sizeof a[4]), pj_f(q->rel, a[0], sizeof a[0]), q->size, pj_f(q->title, a[1], sizeof a[1]));
    }
    char p[512];
    if (!o.b) { if (pj_real(p, sizeof p, AJ_QUEUE)) unlink(p); }
    else if (!o.bad) { pj_mkdirs(SL_ABS_STATE); pj_write(AJ_QUEUE, o.b, o.n); }
    free(o.b);
    aj.st.queued = aj.n_q > 0 ? aj.n_q - (aj.dl_on ? 1 : 0) : 0;
}
static bool aj_q_add(const struct aj_file *f)
{
    if (aj.n_q >= AJ_Q_MAX) return false;
    if (aj.n_q == aj.cap_q)
    {
        int want = aj.cap_q ? aj.cap_q * 2 : 16;
        struct aj_file *n = realloc(aj.q, (size_t)want * sizeof *n);
        if (!n) return false;
        aj.q = n;
        aj.cap_q = want;
    }
    aj.q[aj.n_q++] = *f;
    return true;
}
static void aj_dl_clear_status(void)
{
    aj.st.dl_item[0] = aj.st.dl_episode[0] = aj.st.dl_title[0] = '\0';
    aj.st.dl_file = aj.st.dl_files = 0;
    aj.st.dl_got = aj.st.dl_total = 0;
}
/* the card's folder for a name ("/Audiobooks/Frank Herbert/Dune") */
static void aj_dir_for(char *out, size_t n, const char *a, const char *b)
{
    char x[120], y[120];
    sl_pod_clean_name(x, sizeof x, a && a[0] ? a : "Unknown author");
    sl_pod_clean_name(y, sizeof y, b && b[0] ? b : "Untitled");
    if (!x[0]) snprintf(x, sizeof x, "Unknown author");
    if (!y[0]) snprintf(y, sizeof y, "Untitled");
    snprintf(out, n, "%s/%s/%s", sl_abs_folder, x, y);
}
bool sl_abs_card_free(long long *bytes)
{
    struct statvfs v;
    const char *root = sl_net_card_root();
    if (!root || statvfs(root[0] ? root : "/", &v)) return false;
    *bytes = (long long)v.f_bavail * (long long)v.f_frsize;
    return true;
}
/* the book's words beside it, as Audiobookshelf writes them: our audiobooks read title, authors, narrators, series */
static void aj_write_meta(const char *dir, const struct sl_abs_list *l, const struct sl_abs_item *ep)
{
    struct pj_buf o = { 0 };
    char t[700], a[300], n[300], s[300], d[1400];
    const struct sl_abs_item *h = &l->head;
    aj_json_str(t, sizeof t, ep ? sl_abs_s(l, ep->title) : sl_abs_s(l, h->title));
    aj_json_str(a, sizeof a, ep ? sl_abs_s(l, h->title) : sl_abs_s(l, h->author));
    aj_json_str(n, sizeof n, sl_abs_s(l, h->narrator));
    aj_json_str(s, sizeof s, sl_abs_s(l, h->series));
    aj_json_str(d, sizeof d, ep ? "" : sl_abs_s(l, h->desc));
    pj_add(&o, "{\n  \"title\": \"%s\",\n  \"authors\": [\"%s\"],\n  \"narrators\": [%s%s%s],\n  \"series\": [%s%s%s],\n  \"description\": \"%s\"\n}\n",
           t, a, n[0] ? "\"" : "", n, n[0] ? "\"" : "", s[0] && !ep ? "\"" : "", ep ? "" : s, s[0] && !ep ? "\"" : "", d);
    char rb[400];
    snprintf(rb, sizeof rb, "%s/metadata.json", dir);
    if (!o.bad && o.b) pj_write(rb, o.b, o.n);
    free(o.b);
}
static void aj_write_chapters(const char *item, const char *ep, const struct sl_abs_list *l, const struct sl_abs_item *e)
{
    struct pj_buf o = { 0 };
    for (int i = 0; i < l->n && !e; i++)
        if (l->it[i].kind == SL_ABS_CHAPTER)
        {
            char t[200];
            pj_add(&o, "%u\t%u\t%s\n", (unsigned)l->it[i].start_ms, (unsigned)l->it[i].dur_ms, pj_f(sl_abs_s(l, l->it[i].title), t, sizeof t));
        }
    char name[96];
    snprintf(name, sizeof name, SL_ABS_STATE "/ch/%08x.txt", (unsigned)aj_key(item, ep));
    pj_mkdirs(SL_ABS_STATE "/ch");
    if (o.b && !o.bad) pj_write(name, o.b, o.n);
    else { char p[512]; if (pj_real(p, sizeof p, name)) unlink(p); }
    free(o.b);
}
/* the cover beside the book, for Now Playing, Files and our audiobooks */
static void aj_folder_cover(struct aj_srv *s, const char *dir, const char *item)
{
    char rb[400], real[600], url[900], hdr[AJ_TOK + 40];
    snprintf(rb, sizeof rb, "%s/cover.jpg", dir);
    struct stat st;
    if (!pj_real(real, sizeof real, rb) || !stat(real, &st)) return;
    char a[160];
    lj_enc(a, sizeof a, item);
    snprintf(url, sizeof url, "%s/api/items/%s/cover?width=600&format=jpeg", s->pub.url, a);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", s->token);
    struct pj_mem m = { malloc(AJ_ART_CAP), 0, AJ_ART_CAP };
    if (!m.b) return;
    struct sl_net_req r;
    memset(&r, 0, sizeof r);
    r.method = "GET"; r.url = url; r.ua = SL_ABS_UA; r.headers = hdr; r.timeout_ms = 15000; r.stop = &aj.stop_job;
    r.sink = pj_mem_sink; r.ctx = &m;
    int status = sl_net_http_req(&r);
    memset(hdr, 0, sizeof hdr);
    if (status == 200 && m.n > 100)
    {
        /* written while the card is ours (USB mode waits on the lock); a picture, not an error answer */
        const unsigned char *u = (const unsigned char *)m.b;
        bool pic = (u[0] == 0xff && u[1] == 0xd8) || !memcmp(m.b, "\x89PNG", 4) || !memcmp(m.b, "RIFF", 4);
        pthread_mutex_lock(&aj_lock);
        if (pic && !aj.hold) pj_write(rb, m.b, m.n);
        pthread_mutex_unlock(&aj_lock);
    }
    free(m.b);
}
static void aj_download(struct aj_job *j)
{
    struct aj_srv s;
    int err = SL_ABS_OK;
    if (!aj_srv_copy(&s)) err = SL_ABS_E_NOSERVER;
    struct sl_abs_list l;
    memset(&l, 0, sizeof l);
    struct aj_b b;
    memset(&b, 0, sizeof b);
    b.l = &l;
    b.mode = AB_ITEM;
    char path[400], a[160];
    lj_enc(a, sizeof a, j->arg);
    snprintf(path, sizeof path, "/api/items/%s?expanded=1", a);
    if (!err) err = aj_call(&s, "GET", path, NULL, &b, NULL);
    /* the files: an episode's own, or the book's tracks in their order */
    const struct sl_abs_item *ep = NULL;
    int nf = 0;
    long long total = 0;
    if (!err)
    {
        bool tracks = false;
        for (int i = 0; i < l.n; i++) tracks |= l.it[i].kind == SL_ABS_FILE;
        for (int i = 0; i < l.n; i++)
        {
            struct sl_abs_item *it = &l.it[i];
            if (it->kind == (SL_ABS_FILE | AJ_AFILE) && !tracks && it->finished != 2)
            {
                char u[200];
                snprintf(u, sizeof u, "/api/items/%s/file/%s", j->arg, sl_abs_s(&l, it->id));
                it->url = aj_pool(&l, u);
                it->kind = SL_ABS_FILE;
            }
            if (it->kind == SL_ABS_EPISODE && j->arg2[0] && !strcmp(sl_abs_s(&l, it->episode), j->arg2))
            {
                if (!it->url && it->id)
                {
                    char u[200];
                    snprintf(u, sizeof u, "/api/items/%s/file/%s", j->arg, sl_abs_s(&l, it->id));
                    it->url = aj_pool(&l, u);
                }
                ep = it;
            }
        }
        if (j->arg2[0]) { if (ep && ep->url) { nf = 1; total = ep->size; } }
        else for (int i = 0; i < l.n; i++) if (l.it[i].kind == SL_ABS_FILE && l.it[i].url && l.it[i].finished != 2) { nf++; total += l.it[i].size; }
        if (!nf) err = j->arg2[0] && !ep ? SL_ABS_E_NOTFOUND : SL_ABS_E_NOFILES;
    }
    long long room = 0;
    if (!err && sl_abs_card_free(&room) && total > 0 && total + 64LL * 1024 * 1024 > room) err = SL_ABS_E_FULL;
    char dir[256] = "";
    if (!err)
    {
        if (ep) aj_dir_for(dir, sizeof dir, sl_abs_s(&l, l.head.title), sl_abs_s(&l, ep->title));
        else aj_dir_for(dir, sizeof dir, sl_abs_s(&l, l.head.author), sl_abs_s(&l, l.head.title));
    }
    pthread_mutex_lock(&aj_lock);
    if (!err && aj.hold) err = SL_ABS_E_HOLD;
    if (!err)
    {
        aj_books_load();
        aj_q_load();
        struct sl_abs_book *k = aj_book_find(j->arg, j->arg2);
        if (!k && aj.n_books < SL_ABS_BOOKS)
        {
            if (aj.n_books == aj.cap_books)
            {
                int want = aj.cap_books ? aj.cap_books * 2 : 8;
                struct sl_abs_book *n = realloc(aj.books, (size_t)want * sizeof *n);
                if (n) { aj.books = n; aj.cap_books = want; }
            }
            if (aj.n_books < aj.cap_books) { k = &aj.books[aj.n_books++]; memset(k, 0, sizeof *k); }
        }
        if (!k) err = SL_ABS_E_NOMEM;
        else
        {
            snprintf(k->item, sizeof k->item, "%s", j->arg);
            snprintf(k->episode, sizeof k->episode, "%s", j->arg2);
            /* another book has this folder (two episodes called "Bonus", one book in two
             * libraries): this one gets its own, or Remove and the place would be shared */
            char base[256];
            snprintf(base, sizeof base, "%s", dir);
            for (int nth = 2; nth < 100; nth++)
            {
                bool taken = false;
                for (int i = 0; i < aj.n_books && !taken; i++) taken = &aj.books[i] != k && !strcmp(aj.books[i].dir, dir);
                if (!taken) break;
                snprintf(dir, sizeof dir, "%.240s (%d)", base, nth);
            }
            snprintf(k->dir, sizeof k->dir, "%s", dir);
            snprintf(k->title, sizeof k->title, "%s", ep ? sl_abs_s(&l, ep->title) : sl_abs_s(&l, l.head.title));
            snprintf(k->author, sizeof k->author, "%s", ep ? sl_abs_s(&l, l.head.title) : sl_abs_s(&l, l.head.author));
            k->dur_ms = ep ? ep->dur_ms : l.head.dur_ms;
            k->size = total;
            k->files = nf;
            k->whole = false;
            /* the server's place is taken once the book is whole (a pull then): nothing of it seen yet */
            if (!k->seen) k->seen = 1;
            pj_mkdirs(dir);
            aj_write_meta(dir, &l, ep);
            aj_write_chapters(j->arg, j->arg2, &l, ep);
            /* each file once in the queue; a part already on the card carries on */
            for (int i = aj.n_q - 1; i >= 0; i--)
                if (!strcmp(aj.q[i].item, j->arg) && !strcmp(aj.q[i].ep, j->arg2) && !(aj.dl_on && i == 0))
                    memmove(&aj.q[i], &aj.q[i + 1], (size_t)(--aj.n_q - i) * sizeof *aj.q);
            int idx = 0;
            for (int i = 0; i < l.n; i++)
            {
                const struct sl_abs_item *it = ep ? ep : &l.it[i];
                if (!ep && (it->kind != SL_ABS_FILE || !it->url || it->finished == 2)) continue;
                struct aj_file f;
                memset(&f, 0, sizeof f);
                snprintf(f.item, sizeof f.item, "%s", j->arg);
                snprintf(f.ep, sizeof f.ep, "%s", j->arg2);
                snprintf(f.url, sizeof f.url, "%s", sl_abs_s(&l, it->url));
                f.idx = idx++;
                f.n = nf;
                f.size = it->size;
                char base[160], name[200], ext[16];
                const char *fn = ep ? sl_abs_s(&l, ep->narrator) : sl_abs_s(&l, it->title);
                const char *e = sl_abs_s(&l, it->ext);
                if (!e[0]) { e = strrchr(fn, '.'); e = e ? e : ".mp3"; }
                snprintf(ext, sizeof ext, ".%s", sl_pod_clean_ext(e, "mp3"));   /* the server's: never a way out of the folder */
                snprintf(base, sizeof base, "%s", fn[0] ? fn : ep ? sl_abs_s(&l, ep->title) : "Part");
                char *dot = strrchr(base, '.');
                if (dot && !strcasecmp(dot, ext)) *dot = '\0';
                sl_pod_clean_name(name, sizeof name, base);
                if (!name[0]) snprintf(name, sizeof name, "Part");
                if (nf > 1) snprintf(f.rel, sizeof f.rel, "%s/%02d %s%s", dir, f.idx + 1, name, ext);
                else snprintf(f.rel, sizeof f.rel, "%s/%s%s", dir, name, ext);
                snprintf(f.title, sizeof f.title, "%s", k->title);
                char real[600];
                struct stat st;
                bool there = pj_real(real, sizeof real, f.rel) && !stat(real, &st) && (!f.size || st.st_size == f.size);
                if (!there) aj_q_add(&f);
                if (ep) break;
            }
            aj_books_save();
            aj_q_save();
            /* everything there already: whole now */
            bool any = false;
            for (int i = 0; i < aj.n_q; i++) any |= !strcmp(aj.q[i].item, j->arg) && !strcmp(aj.q[i].ep, j->arg2);
            if (!any) { aj_whole(k); aj.st.books_seq++; }
        }
    }
    if (err) { aj.st.dl_err = err; aj.st.dl_seq++; }
    aj.st.books_seq++;
    aj.st.seq++;
    pthread_mutex_unlock(&aj_lock);
    if (!err) aj_folder_cover(&s, dir, j->arg);
    memset(&s, 0, sizeof s);
    sl_abs_list_free(&l);
}
/* a book just whole: where the server has it is where it starts (locked) */
static void aj_whole(struct sl_abs_book *k)
{
    k->whole = true;
    aj_books_save();
    aj.st.whole_seq++;
    struct aj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_ABS_JOB_PULL;
    snprintf(j.arg, sizeof j.arg, "%s", k->item);
    snprintf(j.arg2, sizeof j.arg2, "%s", k->episode);
    if (aj.n_jobs < AJ_JOBS) aj.jobs[aj.n_jobs++] = j;
}
static bool aj_dest_real(const char *rel, char *out, size_t n, bool part)
{
    return pj_real(out, n, rel) && (!part || strlen(out) + 6 < n) && (!part || strcat(out, ".part"));
}
static long long aj_item_total(const char *item, const char *ep, int *files)
{
    struct sl_abs_book *k = aj_book_find(item, ep);
    if (files) *files = k ? k->files : 0;
    return k ? k->size : 0;
}
static void aj_dl_start_next(void)
{
    if (aj.dl_on || !aj.n_q || aj.hold || !aj.online || !aj.have_srv || (aj.dl_retry_at && (int32_t)(aj_ms() - aj.dl_retry_at) < 0)) return;
    struct aj_file *q = &aj.q[0];
    if (!aj_book_find(q->item, q->ep))
    {
        memmove(&aj.q[0], &aj.q[1], (size_t)--aj.n_q * sizeof *aj.q);
        aj_q_save();
        return;
    }
    char url[1600], tok[3 * AJ_TOK], part[700], rb[400];
    lj_enc(tok, sizeof tok, aj.srv.token);
    int k = snprintf(url, sizeof url, "%s%s%stoken=%s", aj.srv.pub.url, q->url, strchr(q->url, '?') ? "&" : "?", tok);
    memset(tok, 0, sizeof tok);
    if (k <= 0 || k >= (int)sizeof url || !aj_dest_real(q->rel, part, sizeof part, true))
    {
        memmove(&aj.q[0], &aj.q[1], (size_t)--aj.n_q * sizeof *aj.q);
        aj_q_save();
        return;
    }
    snprintf(rb, sizeof rb, "%s", q->rel);
    char *sl = strrchr(rb, '/');
    if (sl) { *sl = '\0'; pj_mkdirs(rb); }
    bool started = sl_net_dl_start(&aj.dl, url, SL_ABS_UA, part);
    memset(url, 0, sizeof url);
    if (!started) { aj.dl_fails++; aj.dl_retry_at = aj_ms() + 30000; return; }
    aj.dl_on = true;
    snprintf(aj.st.dl_item, sizeof aj.st.dl_item, "%s", q->item);
    snprintf(aj.st.dl_episode, sizeof aj.st.dl_episode, "%s", q->ep);
    snprintf(aj.st.dl_title, sizeof aj.st.dl_title, "%s", q->title);
    int files;
    aj.st.dl_total = aj_item_total(q->item, q->ep, &files);
    aj.st.dl_files = q->n;
    aj.st.dl_file = q->idx + 1;
    /* the files before this one are whole */
    long long rest = 0;
    for (int i = 0; i < aj.n_q; i++) if (!strcmp(aj.q[i].item, q->item) && !strcmp(aj.q[i].ep, q->ep)) rest += aj.q[i].size;
    aj.dl_done = aj.st.dl_total > rest ? aj.st.dl_total - rest : 0;
    aj.st.dl_got = aj.dl_done;
    aj.st.queued = aj.n_q - 1;
    aj.st.seq++;
}
static void aj_dl_stop(bool keep_part)
{
    if (!aj.dl_on) return;
    sl_net_dl_stop(&aj.dl);
    if (!keep_part) unlink(aj.dl.path);
    aj.dl_on = false;
    aj_dl_clear_status();
    aj.st.seq++;
}
static void aj_dl_step(void)
{
    if (!aj.dl_on) return;
    struct stat st;
    if (!stat(aj.dl.path, &st) && aj.dl_done + (long long)st.st_size != aj.st.dl_got)
    {
        aj.st.dl_got = aj.dl_done + (long long)st.st_size;
        if (aj.st.dl_total < aj.st.dl_got) aj.st.dl_total = aj.st.dl_got;
        aj.st.seq++;
    }
    int r = sl_net_dl_poll(&aj.dl);
    if (r == 0) return;
    struct aj_file q = aj.q[0];
    bool whole = r == 200 || r == 206;
    char part[700], fin[700];
    aj_dest_real(q.rel, part, sizeof part, true);
    aj_dest_real(q.rel, fin, sizeof fin, false);
    aj.dl_on = false;
    aj_dl_clear_status();
    if (whole && !aj.hold && !stat(part, &st) && st.st_size > 0 && !rename(part, fin))
    {
        memmove(&aj.q[0], &aj.q[1], (size_t)--aj.n_q * sizeof *aj.q);
        aj.dl_fails = 0;
        bool more = false;
        for (int i = 0; i < aj.n_q; i++) more |= !strcmp(aj.q[i].item, q.item) && !strcmp(aj.q[i].ep, q.ep);
        if (!more)
        {
            struct sl_abs_book *k = aj_book_find(q.item, q.ep);
            if (k) aj_whole(k);
            aj.st.dl_err = SL_ABS_OK;
            aj.st.books_seq++;
        }
    }
    else if (r == 401 || r == 403)
    {
        /* the token ran out: a fresh one, then again */
        aj.dl_fails++;
        aj.dl_retry_at = aj_ms() + 2000;
        if (aj.have_srv && aj.dl_fails < 4)
        {
            struct aj_srv s = aj.srv;
            pthread_mutex_unlock(&aj_lock);
            int e = s.pub.api_token ? SL_ABS_E_AUTH : aj_relogin(&s);
            memset(&s, 0, sizeof s);
            pthread_mutex_lock(&aj_lock);
            if (e) aj.st.dl_err = SL_ABS_E_AUTH;
        }
        else { aj.st.dl_err = SL_ABS_E_AUTH; aj.dl_retry_at = aj_ms() + 60000; }
    }
    else if (r == 404 || r == 410)
    {
        unlink(part);
        memmove(&aj.q[0], &aj.q[1], (size_t)--aj.n_q * sizeof *aj.q);
        aj.st.dl_err = SL_ABS_E_NOTFOUND;
    }
    else if (!aj.hold)
    {
        /* cut off: its part stays, it carries on soon */
        aj.dl_fails++;
        aj.dl_retry_at = aj_ms() + (aj.dl_fails < 3 ? 3000u : 30000u);
        aj.st.dl_err = SL_ABS_E_NET;
    }
    aj_q_save();
    aj.st.dl_seq++;
    aj.st.seq++;
}
/* a book off the card: its files, its folder (and its author's, when empty), its chapters; never the server's place */
static void aj_rm_tree(const char *real, int depth)
{
    DIR *d = depth < 4 ? opendir(real) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[700];
        if (snprintf(p, sizeof p, "%s/%s", real, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) aj_rm_tree(p, depth + 1); else unlink(p);
    }
    if (d) closedir(d);
    rmdir(real);
}
static void aj_remove(struct aj_job *j)
{
    pthread_mutex_lock(&aj_lock);
    aj_books_load();
    aj_q_load();
    if (aj.hold) { pthread_mutex_unlock(&aj_lock); return; }
    if (aj.dl_on && !strcmp(aj.q[0].item, j->arg) && !strcmp(aj.q[0].ep, j->arg2)) aj_dl_stop(false);
    for (int i = aj.n_q - 1; i >= 0; i--)
        if (!strcmp(aj.q[i].item, j->arg) && !strcmp(aj.q[i].ep, j->arg2)) memmove(&aj.q[i], &aj.q[i + 1], (size_t)(--aj.n_q - i) * sizeof *aj.q);
    aj_q_save();
    struct sl_abs_book *k = aj_book_find(j->arg, j->arg2);
    if (k)
    {
        char real[600], up[600];
        size_t fl = strlen(sl_abs_folder);
        /* only inside the audiobook folder, never the folder itself */
        if (!strncmp(k->dir, sl_abs_folder, fl) && k->dir[fl] == '/' && aj_rel_ok(k->dir) && pj_real(real, sizeof real, k->dir))
        {
            aj_rm_tree(real, 0);
            snprintf(up, sizeof up, "%s", real);
            char *sl = strrchr(up, '/');
            if (sl && strlen(k->dir) > fl + 1 && strchr(k->dir + fl + 1, '/')) { *sl = '\0'; rmdir(up); }   /* the author's, if empty */
        }
        char name[96];
        snprintf(name, sizeof name, SL_ABS_STATE "/ch/%08x.txt", (unsigned)aj_key(k->item, k->episode));
        if (pj_real(real, sizeof real, name)) unlink(real);
        if (k->whole) aj.st.whole_seq++;
        int at = (int)(k - aj.books);
        memmove(&aj.books[at], &aj.books[at + 1], (size_t)(aj.n_books - at - 1) * sizeof *aj.books);
        aj.n_books--;
        aj_books_save();
    }
    aj.st.books_seq++;
    aj.st.seq++;
    pthread_mutex_unlock(&aj_lock);
}

/* ---------------------------------------------------------------- covers */
void sl_abs_art_name(char *out, size_t n, const char *item, int px)
{
    snprintf(out, n, SL_ABS_STATE "/art/%08x-%d.bin", (unsigned)pj_fnv(item ? item : ""), px);
}
static void aj_art(const char *item)
{
    struct aj_srv s;
    if (!item[0] || !aj_srv_copy(&s)) return;
    char url[900], a[160], hdr[AJ_TOK + 40];
    lj_enc(a, sizeof a, item);
    snprintf(url, sizeof url, "%s/api/items/%s/cover?width=400&format=jpeg", s.pub.url, a);
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", s.token);
    memset(&s, 0, sizeof s);
    struct pj_mem m = { malloc(AJ_ART_CAP), 0, AJ_ART_CAP };
    if (!m.b) return;
    struct sl_net_req r;
    memset(&r, 0, sizeof r);
    r.method = "GET"; r.url = url; r.ua = SL_ABS_UA; r.headers = hdr; r.timeout_ms = 20000; r.stop = &aj.stop_job;
    r.sink = pj_mem_sink; r.ctx = &m;
    int status = sl_net_http_req(&r);
    memset(hdr, 0, sizeof hdr);
    int w = 0, h = 0;
    unsigned char *rgb = status == 200 && m.n ? sl_art_rgb_load(m.b, (int)m.n, &w, &h) : NULL;
    free(m.b);
    char big[96], row[96];
    sl_abs_art_name(big, sizeof big, item, SL_ABS_ART_BIG);
    sl_abs_art_name(row, sizeof row, item, SL_ABS_ART_ROW);
    pthread_mutex_lock(&aj_lock);
    if (rgb && !aj.hold && pj_art_square(big, SL_ABS_STATE "/art", rgb, w, h, SL_ABS_ART_BIG, NULL) &&
        pj_art_square(row, SL_ABS_STATE "/art", rgb, w, h, SL_ABS_ART_ROW, NULL))
    { aj.st.art_seq++; aj.st.seq++; }
    pthread_mutex_unlock(&aj_lock);
    if (rgb) sl_art_rgb_free(rgb);
}

/* ---------------------------------------------------------------- the worker */
static void aj_run(struct aj_job *j)
{
    switch (j->kind)
    {
        case SL_ABS_JOB_SIGNIN: aj_signin(j); break;
        case SL_ABS_JOB_LIST: aj_list(j); break;
        case SL_ABS_JOB_DOWNLOAD: aj_download(j); break;
        case SL_ABS_JOB_PULL: aj_pull(j); break;
        case SL_ABS_JOB_REMOVE: aj_remove(j); break;
    }
}
static bool aj_pend_due(void) { return aj.n_pend && aj.online && !aj.hold && aj.have_srv && (!aj.pend_retry_at || (int32_t)(aj_ms() - aj.pend_retry_at) >= 0); }
static void *aj_loop(void *unused)
{
    (void)unused;
    uint32_t idle_from = aj_ms();
    pthread_mutex_lock(&aj_lock);
    aj_srv_load();
    aj_books_load();
    aj_q_load();
    aj_pend_load();
    for (;;)
    {
        aj_dl_start_next();
        aj_dl_step();
        if (aj.n_jobs && !aj.hold)
        {
            struct aj_job j = aj.jobs[0];
            memset(aj.jobs[0].secret, 0, sizeof aj.jobs[0].secret);
            memmove(&aj.jobs[0], &aj.jobs[1], (size_t)--aj.n_jobs * sizeof *aj.jobs);
            aj.st.job = j.kind;
            aj.st.jobs = aj.n_jobs;
            aj.stop_job = false;
            aj.st.seq++;
            pthread_mutex_unlock(&aj_lock);
            aj_run(&j);
            memset(&j, 0, sizeof j);
            pthread_mutex_lock(&aj_lock);
            aj.st.job = SL_ABS_JOB_NONE;
            aj.st.seq++;
            idle_from = aj_ms();
            continue;
        }
        if (aj_pend_due())
        {
            aj.st.job = SL_ABS_JOB_SYNC;
            aj.stop_job = false;
            bool sent = aj_pend_send();
            aj.st.job = SL_ABS_JOB_NONE;
            idle_from = aj_ms();
            if (sent) continue;
        }
        if (aj.n_art && !aj.hold && aj.online)
        {
            char item[48];
            snprintf(item, sizeof item, "%s", aj.art_q[--aj.n_art]);
            aj.st.job = SL_ABS_JOB_ART;
            aj.stop_job = false;
            pthread_mutex_unlock(&aj_lock);
            aj_art(item);
            pthread_mutex_lock(&aj_lock);
            aj.st.job = SL_ABS_JOB_NONE;
            idle_from = aj_ms();
            continue;
        }
        bool waiting = aj.dl_on || (aj.n_q && aj.online && !aj.hold && aj.have_srv);
        if (waiting) idle_from = aj_ms();
        if (!aj.n_jobs && !waiting && (int32_t)(aj_ms() - idle_from) >= (int32_t)AJ_IDLE_MS) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&aj_cond, &aj_lock, &ts);
    }
    aj.up = false;
    pthread_mutex_unlock(&aj_lock);
    return NULL;
}
static bool aj_start(void)
{
    if (aj.up) { pthread_cond_broadcast(&aj_cond); return true; }
    if (aj.thread) pthread_join(aj.thread, NULL);
    aj.thread = 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 160 * 1024);
    aj.up = pthread_create(&aj.thread, &a, aj_loop, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!aj.up) aj.thread = 0;
    return aj.up;
}
static bool aj_job_add(const struct aj_job *in)
{
    if (aj.n_jobs >= AJ_JOBS) return false;
    aj.jobs[aj.n_jobs++] = *in;
    aj.st.jobs = aj.n_jobs;
    aj.st.seq++;
    return aj_start();
}

/* ---------------------------------------------------------------- the interface's side */
unsigned sl_abs_ask(int what, const char *arg, const char *filter, int page)
{
    struct aj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_ABS_JOB_LIST;
    j.what = what;
    j.page = page;
    snprintf(j.arg, sizeof j.arg, "%s", arg ? arg : "");
    snprintf(j.arg2, sizeof j.arg2, "%s", filter ? filter : "");
    pthread_mutex_lock(&aj_lock);
    j.req = ++aj.req_next ? aj.req_next : ++aj.req_next;
    bool ok = aj_job_add(&j);
    pthread_mutex_unlock(&aj_lock);
    return ok ? j.req : 0;
}
bool sl_abs_take(unsigned req, struct sl_abs_list *out)
{
    bool got = false;
    pthread_mutex_lock(&aj_lock);
    for (int i = 0; i < AJ_ANSWERS && !got; i++)
        if (aj.ans_ok[i] && aj.ans[i].req == req) { *out = aj.ans[i]; aj.ans_ok[i] = false; memset(&aj.ans[i], 0, sizeof aj.ans[i]); got = true; }
    pthread_mutex_unlock(&aj_lock);
    return got;
}
void sl_abs_forget_req(unsigned req)
{
    pthread_mutex_lock(&aj_lock);
    static int at;
    aj.forgot[at++ % 8] = req;
    for (int i = 0; i < AJ_ANSWERS; i++)
        if (aj.ans_ok[i] && aj.ans[i].req == req) { sl_abs_list_free(&aj.ans[i]); aj.ans_ok[i] = false; }
    pthread_mutex_unlock(&aj_lock);
}
void sl_abs_list_free(struct sl_abs_list *l)
{
    free(l->it);
    free(l->pool);
    l->it = NULL; l->pool = NULL;
    l->n = l->cap = 0;
    l->pool_n = l->pool_cap = 0;
}
void sl_abs_online(bool up)
{
    pthread_mutex_lock(&aj_lock);
    bool was = aj.online;
    aj.online = up;
    aj.st.online = up;
    aj.st.seq++;
    if (up && !was)
    {
        /* back: what waited is told now, and the files carry on */
        aj.pend_retry_at = 0;
        aj.dl_retry_at = 0;
        aj_srv_load();
        aj_pend_load();
        aj_q_load();
        if (aj.have_srv && (aj.n_pend || aj.n_q)) aj_start();
    }
    if (!up && aj.dl_on) aj_dl_stop(true);
    pthread_mutex_unlock(&aj_lock);
}
void sl_abs_hold(bool hold)
{
    pthread_mutex_lock(&aj_lock);
    aj.hold = hold;
    aj.st.hold = hold;
    if (hold) { aj.stop_job = true; aj_dl_stop(true); }
    aj.st.seq++;
    if (!hold && (aj.n_q || aj.n_pend)) aj_start();
    pthread_mutex_unlock(&aj_lock);
}
void sl_abs_status(struct sl_abs_status *s)
{
    pthread_mutex_lock(&aj_lock);
    *s = aj.st;
    pthread_mutex_unlock(&aj_lock);
}
bool sl_abs_busy(void) { return aj.up; }
void sl_abs_poll(void)
{
    pthread_mutex_lock(&aj_lock);
    if (!aj.up && aj_pend_due()) aj_start();
    if (!aj.up && aj.n_q && aj.online && !aj.hold && aj.have_srv && (!aj.dl_retry_at || (int32_t)(aj_ms() - aj.dl_retry_at) >= 0)) aj_start();
    pthread_mutex_unlock(&aj_lock);
}
bool sl_abs_server(struct sl_abs_server *out)
{
    pthread_mutex_lock(&aj_lock);
    aj_srv_load();
    bool have = aj.have_srv;
    if (have && out) *out = aj.srv.pub;
    pthread_mutex_unlock(&aj_lock);
    return have;
}
bool sl_abs_remove_server(void)
{
    pthread_mutex_lock(&aj_lock);
    aj_srv_load();
    bool ok = !aj.hold;
    if (ok)
    {
        memset(&aj.srv, 0, sizeof aj.srv);
        aj.have_srv = false;
        aj_dl_stop(true);
        ok = aj_srv_save();
    }
    pthread_mutex_unlock(&aj_lock);
    return ok;
}
bool sl_abs_signin(const char *url, const char *user, const char *secret, bool save)
{
    struct aj_job j;
    memset(&j, 0, sizeof j);
    j.kind = SL_ABS_JOB_SIGNIN;
    j.save = save;
    snprintf(j.url, sizeof j.url, "%s", url ? url : "");
    snprintf(j.user, sizeof j.user, "%s", user ? user : "");
    snprintf(j.secret, sizeof j.secret, "%s", secret ? secret : "");
    pthread_mutex_lock(&aj_lock);
    bool ok = aj_job_add(&j);
    pthread_mutex_unlock(&aj_lock);
    memset(&j, 0, sizeof j);
    return ok;
}
int sl_abs_books(struct sl_abs_book *out, int max)
{
    pthread_mutex_lock(&aj_lock);
    aj_books_load();
    int n = aj.n_books < max ? aj.n_books : max;
    if (out) memcpy(out, aj.books, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&aj_lock);
    return out ? n : aj.n_books;
}
bool sl_abs_book(const char *item, const char *episode, struct sl_abs_book *out)
{
    pthread_mutex_lock(&aj_lock);
    struct sl_abs_book *k = aj_book_find(item, episode ? episode : "");
    if (k && out) *out = *k;
    pthread_mutex_unlock(&aj_lock);
    return k != NULL;
}
bool sl_abs_book_at(const char *path, struct sl_abs_book *out)
{
    bool found = false;
    pthread_mutex_lock(&aj_lock);
    aj_books_load();
    for (int i = 0; i < aj.n_books && !found; i++)
    {
        size_t n = strlen(aj.books[i].dir);
        if (n && !strncasecmp(path, aj.books[i].dir, n) && path[n] == '/') { if (out) *out = aj.books[i]; found = true; }
    }
    pthread_mutex_unlock(&aj_lock);
    return found;
}
static bool aj_item_job(int kind, const char *item, const char *episode)
{
    struct aj_job j;
    memset(&j, 0, sizeof j);
    j.kind = kind;
    snprintf(j.arg, sizeof j.arg, "%s", item ? item : "");
    snprintf(j.arg2, sizeof j.arg2, "%s", episode ? episode : "");
    if (!j.arg[0]) return false;
    pthread_mutex_lock(&aj_lock);
    bool ok = !aj.hold && aj_job_add(&j);
    pthread_mutex_unlock(&aj_lock);
    return ok;
}
bool sl_abs_download(const char *item, const char *episode) { return aj_item_job(SL_ABS_JOB_DOWNLOAD, item, episode); }
bool sl_abs_remove(const char *item, const char *episode) { return aj_item_job(SL_ABS_JOB_REMOVE, item, episode); }
bool sl_abs_pull(const char *item, const char *episode)
{
    pthread_mutex_lock(&aj_lock);
    bool on = aj.online;
    pthread_mutex_unlock(&aj_lock);
    return on && aj_item_job(SL_ABS_JOB_PULL, item, episode);
}
int sl_abs_chapters(const char *item, const char *episode, uint32_t *start_ms, int max)
{
    char name[96];
    snprintf(name, sizeof name, SL_ABS_STATE "/ch/%08x.txt", (unsigned)aj_key(item, episode));
    char *b = pj_slurp(name, 256 * 1024, NULL), *s = b, *line;
    int n = 0;
    while (b && n < max && (line = strsep(&s, "\n"))) if (line[0]) start_ms[n++] = (uint32_t)strtoul(line, NULL, 10);
    free(b);
    return n;
}
bool sl_abs_push(const char *item, const char *episode, uint32_t cur_ms, uint32_t dur_ms, bool finished)
{
    if (!item || !item[0]) return false;
    pthread_mutex_lock(&aj_lock);
    aj_pend_load();
    bool ok = aj.pend && !aj.hold;
    if (ok)
    {
        /* the newest of each book is all that is told */
        int i = aj_pend_find(item, episode ? episode : "");
        if (i < 0 && aj.n_pend == AJ_PEND_MAX) aj_pend_drop(0);
        if (i < 0) i = aj.n_pend++;
        struct aj_pend *p = &aj.pend[i];
        snprintf(p->item, sizeof p->item, "%s", item);
        snprintf(p->ep, sizeof p->ep, "%s", episode ? episode : "");
        p->cur_ms = cur_ms;
        p->dur_ms = dur_ms;
        p->fin = finished;
        p->t = (long long)time(NULL);
        aj_pend_save();
        aj.st.seq++;
        aj_srv_load();
        if (aj.online && aj.have_srv) { aj.pend_retry_at = 0; aj_start(); }
    }
    pthread_mutex_unlock(&aj_lock);
    return ok;
}
bool sl_abs_take_pick(struct sl_abs_pick *out)
{
    pthread_mutex_lock(&aj_lock);
    bool got = aj.n_picks > 0;
    if (got)
    {
        *out = aj.picks[0];
        memmove(&aj.picks[0], &aj.picks[1], (size_t)--aj.n_picks * sizeof *aj.picks);
    }
    pthread_mutex_unlock(&aj_lock);
    return got;
}
bool sl_abs_art_have(const char *item)
{
    char name[96], p[512];
    struct stat st;
    sl_abs_art_name(name, sizeof name, item, SL_ABS_ART_ROW);
    return pj_real(p, sizeof p, name) && !stat(p, &st);
}
bool sl_abs_art_want(const char *item)
{
    if (!item || !item[0] || sl_abs_art_have(item)) return false;
    pthread_mutex_lock(&aj_lock);
    bool ok = aj.online && !aj.hold;
    for (int i = 0; ok && i < aj.n_art; i++) if (!strcmp(aj.art_q[i], item)) ok = false;
    if (ok)
    {
        if (aj.n_art == AJ_ART_Q) { memmove(&aj.art_q[0], &aj.art_q[1], (AJ_ART_Q - 1) * sizeof aj.art_q[0]); aj.n_art--; }
        snprintf(aj.art_q[aj.n_art++], sizeof aj.art_q[0], "%s", item);
        aj_start();
    }
    pthread_mutex_unlock(&aj_lock);
    return ok;
}
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
void sl_abs_test_forget(void)
{
    for (int i = 0; i < 400 && aj.up; i++) usleep(10000);
    pthread_mutex_lock(&aj_lock);
    aj_dl_stop(true);
    for (int i = 0; i < AJ_ANSWERS; i++) if (aj.ans_ok[i]) { sl_abs_list_free(&aj.ans[i]); aj.ans_ok[i] = false; }
    free(aj.books); aj.books = NULL; aj.n_books = aj.cap_books = 0; aj.books_read = false;
    free(aj.q); aj.q = NULL; aj.n_q = aj.cap_q = 0; aj.q_read = false;
    free(aj.pend); aj.pend = NULL; aj.n_pend = 0; aj.pend_read = false;
    free(aj.prog); aj.prog = NULL; aj.n_prog = 0;
    memset(&aj.srv, 0, sizeof aj.srv); aj.srv_read = aj.have_srv = false;
    aj.n_picks = aj.n_art = aj.n_jobs = 0;
    aj.pend_retry_at = aj.dl_retry_at = 0;
    aj.dl_fails = 0;
    bool on = aj.online;
    memset(&aj.st, 0, sizeof aj.st);
    aj.st.online = on;
    pthread_mutex_unlock(&aj_lock);
}
size_t sl_abs_test_static_bytes(void) { return sizeof aj; }
long long sl_abs_test_skew(void) { return aj.skew; }
#endif
