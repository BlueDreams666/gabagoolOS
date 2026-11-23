/* F5 (#52, #120, #151): a music server, Subsonic / OpenSubsonic / Navidrome,
 * with the offline cache (sloppy_subjob.inc, sloppy_subsonic.inc). Included
 * from ui_test.c after batch_h.inc, whose helpers it uses.
 *   sb_tests()               in the suite
 *   SLOPPY_SUB_TESTS=1       the tests alone
 *   audit screens subsonic-*, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=subsonic ./ui-test
 * A Subsonic server of our own answers on 127.0.0.1, on a thread here, over
 * a real socket: the simulator's network (sl_net_http_fake, sl_net_dl_fake)
 * is a little HTTP client that goes to it, Range headers and all. */
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define SB_FOLDER "/Card/Subsonic"
#define SB_PW "s3cret pass&=?"            /* every character a URL must escape */
#define SB_KEY "key-1234567890abcdef"

/* ---- the server */
static struct {
    int port, down_port, lfd;
    pthread_t thr;
    bool up;
    pthread_mutex_t mu;
    /* what it saw */
    int reqs, streams, art_gets, scrob_sub, scrob_now, stars, unstars, bad_auth, range_from_max;
    char last_q[1200], last_method[48], last_type[32], last_scrob[160], last_star[64];
    char salts[64][24];
    int nsalt;
    bool plain_pw_seen;                   /* p= ever arrived */
    /* how it acts */
    bool html, slow, drop_once, stall, gone, transcoded_whole;
    int slice;                            /* bytes the downloads ask for at a time */
} sbv = { .mu = PTHREAD_MUTEX_INITIALIZER, .slice = 30000 };
static const char *sb_q(const char *q, const char *key, char *out, size_t n)
{
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char *s = q; s && *s; )
    {
        const char *e = strchr(s, '&');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len > kl && !strncmp(s, key, kl) && s[kl] == '=')
        {
            size_t o = 0;
            for (const char *c = s + kl + 1; c < s + len && o + 1 < n; c++)
            {
                if (*c == '%' && c + 2 < s + len + 1) { unsigned v; sscanf(c + 1, "%2x", &v); out[o++] = (char)v; c += 2; }
                else out[o++] = *c == '+' ? ' ' : *c;
            }
            out[o] = '\0';
            return out;
        }
        s = e ? e + 1 : NULL;
    }
    return NULL;
}
static long long sb_song_size(int album, int n) { (void)album; return 60000 + n * 1000; }
/* "s<album>-<n>" */
static bool sb_song_id(const char *id, int *a, int *n) { return sscanf(id, "s%d-%d", a, n) == 2 && *a >= 1 && *n >= 1; }
static int sb_album_artist(int a) { return (a - 1) % 75 + 1; }
static void sb_artist_name(char *b, size_t n, int k) { if (k == 1) snprintf(b, n, sc_on ? "The Weeknd" : "The Paper Kite"); else snprintf(b, n, "Artist %d", k); }
/* SLOPPY_SHOWCASE: album 1 is After Hours, its first six songs */
static const char *const sc_sub_songs[6] = { "Alone Again", "Too Late", "Hardest to Love", "Scared to Live", "Snowchild", "Escape From LA" };
static const int sc_sub_secs[6] = { 250, 239, 211, 191, 247, 355 };
static void sb_album_name(char *b, size_t n, int a)
{
    if (a == 1 && sc_on) snprintf(b, n, "After Hours");
    else if (a == 1) snprintf(b, n, "Caf\\u00e9 Kite");             /* the JSON's escape */
    else if (a == 2) snprintf(b, n, "Two Discs, \\\"Live\\\"");
    else snprintf(b, n, "Album %d", a);
}
static int sb_songs_in(int a) { return a == 1 || a == 2 ? 6 : 4; }
static size_t sb_song_json(char *o, size_t cap, int a, int n)
{
    char al[64], ar[64];
    sb_album_name(al, sizeof al, a);
    sb_artist_name(ar, sizeof ar, sb_album_artist(a));
    int disc = a == 2 && n > 3 ? 2 : 1, track = a == 2 && n > 3 ? n - 3 : n;
    char no[16] = "";
    if (!sc_on || a != 1) snprintf(no, sizeof no, " %d", n);   /* "Song 3"; a real one has no number */
    return (size_t)snprintf(o, cap,
        "{\"id\":\"s%d-%d\",\"parent\":\"al%d\",\"isDir\":false,\"title\":\"%s%s\",\"album\":\"%s\",\"artist\":\"%s\",\"track\":%d,"
        "\"discNumber\":%d,\"year\":2021,\"coverArt\":\"al%d\",\"size\":%lld,\"contentType\":\"audio/flac\",\"suffix\":\"flac\","
        "\"duration\":%d,\"bitRate\":900,\"albumId\":\"al%d\",\"artistId\":\"ar%d\",\"type\":\"music\"%s,"
        "\"artists\":[{\"id\":\"ar%d\",\"name\":\"Not the title\"}],\"replayGain\":{\"trackGain\":-6.5}}",
        a, n, a, sc_on && a == 1 ? sc_sub_songs[(n - 1) % 6] : a == 1 && n == 2 ? "Bloom \\u2014 the long way" : "Song", no, al, ar, track, disc, a,
        sb_song_size(a, n), sc_on && a == 1 ? sc_sub_secs[(n - 1) % 6] : 180 + n * 7, a,
        sb_album_artist(a), a == 3 && n == 1 ? ",\"starred\":\"2026-10-01T10:00:00Z\"" : "", sb_album_artist(a));
}
static size_t sb_album_json(char *o, size_t cap, int a, bool songs)
{
    char al[64], ar[64];
    sb_album_name(al, sizeof al, a);
    sb_artist_name(ar, sizeof ar, sb_album_artist(a));
    size_t k = (size_t)snprintf(o, cap, "{\"id\":\"al%d\",\"name\":\"%s\",\"artist\":\"%s\",\"artistId\":\"ar%d\",\"coverArt\":\"al%d\","
                                "\"songCount\":%d,\"duration\":%d,\"year\":%d,\"created\":\"2026-01-01T00:00:00Z\"", a, al, ar,
                                sb_album_artist(a), a, sb_songs_in(a), 900, sc_on && a == 1 ? 2020 : 2000 + a % 26);
    if (songs)
    {
        k += (size_t)snprintf(o + k, cap - k, ",\"song\":[");
        for (int n = 1; n <= sb_songs_in(a); n++) { if (n > 1) o[k++] = ','; k += sb_song_json(o + k, cap - k, a, n); }
        k += (size_t)snprintf(o + k, cap - k, "]");
    }
    k += (size_t)snprintf(o + k, cap - k, "}");
    return k;
}
#define SB_HEAD "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\",\"type\":\"navidrome\",\"serverVersion\":\"0.53.3\",\"openSubsonic\":true"
static size_t sb_body(const char *method, const char *q, char *o, size_t cap, int *status, const char **ctype)
{
    char v[256];
    size_t k = 0;
    *status = 200;
    *ctype = "application/json";
    /* the sign-in: a token of the password and a fresh salt, or the API key; never both */
    char u[64] = "", t[64] = "", s[64] = "", key[96] = "", p[300] = "";
    sb_q(q, "u", u, sizeof u); sb_q(q, "t", t, sizeof t); sb_q(q, "s", s, sizeof s); sb_q(q, "apiKey", key, sizeof key); sb_q(q, "p", p, sizeof p);
    if (p[0]) sbv.plain_pw_seen = true;
    bool ok = false;
    int code = 40;
    if (key[0]) { ok = !strcmp(key, SB_KEY) && !u[0]; code = u[0] ? 43 : 44; }
    else if (!strcmp(u, "tai") && s[0])
    {
        char want[33];
        sl_sub_token(SB_PW, s, want);
        ok = !strcmp(want, t);
        if (sbv.nsalt < 64) snprintf(sbv.salts[sbv.nsalt++], sizeof sbv.salts[0], "%s", s);
    }
    else if (!strcmp(u, "keyuser")) code = 42;           /* takes only its API key */
    bool anon = !strcmp(method, "getOpenSubsonicExtensions");
    if (!ok && !anon)
    {
        sbv.bad_auth++;
        return (size_t)snprintf(o, cap, "{\"subsonic-response\":{\"status\":\"failed\",\"version\":\"1.16.1\",\"error\":{\"code\":%d,\"message\":\"Wrong username or password\"}}}", code);
    }
    if (!strcmp(method, "ping")) return (size_t)snprintf(o, cap, SB_HEAD "}}");
    if (anon) return (size_t)snprintf(o, cap, SB_HEAD ",\"openSubsonicExtensions\":[{\"name\":\"songLyrics\",\"versions\":[1]},{\"name\":\"apiKeyAuthentication\",\"versions\":[1]}]}}");
    if (!strcmp(method, "getArtists"))
    {
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"artists\":{\"ignoredArticles\":\"The El La\",\"index\":[");
        for (int a = 1; a <= 75; a++)
        {
            char nm[64];
            sb_artist_name(nm, sizeof nm, a);
            if (a == 1 || a % 10 == 0) k += (size_t)snprintf(o + k, cap - k, "%s{\"name\":\"%c\",\"artist\":[", a == 1 ? "" : "]},", 'A' + a / 10);
            else o[k++] = ',';
            k += (size_t)snprintf(o + k, cap - k, "{\"id\":\"ar%d\",\"name\":\"%s\",\"coverArt\":\"ar-%d\",\"albumCount\":%d}", a, nm, a, a <= 15 ? 2 : 1);
        }
        k += (size_t)snprintf(o + k, cap - k, "]}]}}}");
        return k;
    }
    if (!strcmp(method, "getAlbumList2"))
    {
        int size = atoi(sb_q(q, "size", v, sizeof v) ? v : "10"), off = atoi(sb_q(q, "offset", v, sizeof v) ? v : "0");
        sb_q(q, "type", sbv.last_type, sizeof sbv.last_type);
        int total = !strcmp(sbv.last_type, "starred") ? 3 : 90;
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"albumList2\":{\"album\":[");
        for (int a = off + 1, i = 0; a <= total && i < size; a++, i++) { if (i) o[k++] = ','; k += sb_album_json(o + k, cap - k, a, false); }
        k += (size_t)snprintf(o + k, cap - k, "]}}}");
        return k;
    }
    if (!strcmp(method, "getAlbum"))
    {
        int a = sb_q(q, "id", v, sizeof v) ? atoi(v + 2) : 0;
        if (a < 1 || a > 90) return (size_t)snprintf(o, cap, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":70,\"message\":\"Album not found\"}}}");
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"album\":");
        k += sb_album_json(o + k, cap - k, a, true);
        k += (size_t)snprintf(o + k, cap - k, "}}");
        return k;
    }
    if (!strcmp(method, "getArtist"))
    {
        int r = sb_q(q, "id", v, sizeof v) ? atoi(v + 2) : 0;
        char nm[64];
        sb_artist_name(nm, sizeof nm, r);
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"artist\":{\"id\":\"ar%d\",\"name\":\"%s\",\"albumCount\":2,\"album\":[", r, nm);
        bool first = true;
        for (int a = 1; a <= 90; a++) if (sb_album_artist(a) == r) { if (!first) o[k++] = ','; first = false; k += sb_album_json(o + k, cap - k, a, false); }
        k += (size_t)snprintf(o + k, cap - k, "]}}}");
        return k;
    }
    if (!strcmp(method, "getPlaylists"))
        return (size_t)snprintf(o, cap, SB_HEAD ",\"playlists\":{\"playlist\":[{\"id\":\"p1\",\"name\":\"Late Night Drive\",\"owner\":\"tai\",\"public\":true,"
                                "\"songCount\":5,\"duration\":1100,\"coverArt\":\"pl-p1\"},{\"id\":\"p2\",\"name\":\"Morning\",\"owner\":\"tai\",\"songCount\":2,\"duration\":400}]}}}");
    if (!strcmp(method, "getPlaylist"))
    {
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"playlist\":{\"id\":\"p1\",\"name\":\"Late Night Drive\",\"owner\":\"tai\",\"songCount\":5,\"coverArt\":\"pl-p1\",\"entry\":[");
        static const int pick[5][2] = { { 3, 1 }, { 4, 2 }, { 5, 3 }, { 1, 4 }, { 6, 1 } };
        for (int i = 0; i < 5; i++) { if (i) o[k++] = ','; k += sb_song_json(o + k, cap - k, pick[i][0], pick[i][1]); }
        k += (size_t)snprintf(o + k, cap - k, "]}}}");
        return k;
    }
    if (!strcmp(method, "search3"))
    {
        sb_q(q, "query", v, sizeof v);
        if (strcasecmp(v, "kite")) return (size_t)snprintf(o, cap, SB_HEAD ",\"searchResult3\":{}}}");
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"searchResult3\":{\"artist\":[{\"id\":\"ar1\",\"name\":\"The Paper Kite\",\"albumCount\":2,\"coverArt\":\"ar-1\"}],\"album\":[");
        k += sb_album_json(o + k, cap - k, 1, false); o[k++] = ','; k += sb_album_json(o + k, cap - k, 76, false);
        k += (size_t)snprintf(o + k, cap - k, "],\"song\":[");
        for (int n = 1; n <= 3; n++) { if (n > 1) o[k++] = ','; k += sb_song_json(o + k, cap - k, 1, n); }
        k += (size_t)snprintf(o + k, cap - k, "]}}}");
        return k;
    }
    if (!strcmp(method, "getStarred2"))
    {
        k = (size_t)snprintf(o, cap, SB_HEAD ",\"starred2\":{\"album\":[");
        k += sb_album_json(o + k, cap - k, 3, false);
        k += (size_t)snprintf(o + k, cap - k, "],\"song\":[");
        k += sb_song_json(o + k, cap - k, 3, 1); o[k++] = ','; k += sb_song_json(o + k, cap - k, 2, 4);
        k += (size_t)snprintf(o + k, cap - k, "]}}}");
        return k;
    }
    if (!strcmp(method, "scrobble"))
    {
        sb_q(q, "submission", v, sizeof v);
        if (!strcmp(v, "true")) sbv.scrob_sub++; else sbv.scrob_now++;
        snprintf(sbv.last_scrob, sizeof sbv.last_scrob, "%s", q);
        return (size_t)snprintf(o, cap, SB_HEAD "}}");
    }
    if (!strcmp(method, "star") || !strcmp(method, "unstar"))
    {
        if (method[0] == 's') sbv.stars++; else sbv.unstars++;
        sb_q(q, "id", sbv.last_star, sizeof sbv.last_star);
        return (size_t)snprintf(o, cap, SB_HEAD "}}");
    }
    if (!strcmp(method, "getCoverArt"))
    {
        sbv.art_gets++;
        if (sc_sub_jpg && sc_sub_n <= cap) { *ctype = "image/jpeg"; memcpy(o, sc_sub_jpg, sc_sub_n); return sc_sub_n; }
        h_make_png();
        *ctype = "image/png";
        if (h_png_n > cap) { *status = 500; return 0; }
        memcpy(o, h_png, h_png_n);
        return h_png_n;
    }
    *status = 404;
    return (size_t)snprintf(o, cap, "<html>Not found</html>");
}
static void sb_write_all(int fd, const char *b, size_t n)
{
    while (n) { ssize_t w = send(fd, b, n, 0); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; b += w; n -= (size_t)w; }
}
static void *sb_serve(void *unused)
{
    (void)unused;
    static char req[4096];
    size_t cap = 2 * 1024 * 1024;
    char *body = malloc(cap);
    for (;;)
    {
        int c = accept(sbv.lfd, NULL, NULL);
        if (c < 0 && errno == EINTR) continue;
        if (c < 0) break;
        size_t n = 0;
        while (n + 1 < sizeof req)
        {
            ssize_t r = recv(c, req + n, sizeof req - 1 - n, 0);
            if (r < 0 && errno == EINTR) continue;   /* the simulator's timer signals */
            if (r <= 0) break;
            n += (size_t)r;
            req[n] = '\0';
            if (strstr(req, "\r\n\r\n")) break;
        }
        req[n] = '\0';
        char path[1600] = "";
        sscanf(req, "GET %1599s", path);
        pthread_mutex_lock(&sbv.mu);
        bool slow = sbv.slow;
        pthread_mutex_unlock(&sbv.mu);
        if (slow) usleep(2500 * 1000);
        pthread_mutex_lock(&sbv.mu);
        sbv.reqs++;
        char *qm = strchr(path, '?');
        const char *q = qm ? qm + 1 : "";
        if (qm) *qm = '\0';
        const char *m = strncmp(path, "/rest/", 6) ? "" : path + 6;
        snprintf(sbv.last_method, sizeof sbv.last_method, "%s", m);
        if (strcmp(m, "stream") && strcmp(m, "getCoverArt")) snprintf(sbv.last_q, sizeof sbv.last_q, "%s", q);
        int status = 200;
        const char *ctype = "application/json";
        size_t len = 0;
        char head[512];
        if (sbv.html) { len = (size_t)snprintf(body, cap, "<!DOCTYPE html><html><body>A router's page</body></html>"); ctype = "text/html"; }
        else if (!strcmp(m, "stream"))
        {
            /* the bytes say where they are: i * 7 % 251; a Range is honoured (a transcode comes whole) */
            char id[64] = "", fmt[16] = "";
            int a = 0, k = 0;
            sb_q(q, "id", id, sizeof id);
            sb_q(q, "format", fmt, sizeof fmt);
            char u[64] = "", t[64] = "", s[64] = "";
            sb_q(q, "u", u, sizeof u); sb_q(q, "t", t, sizeof t); sb_q(q, "s", s, sizeof s);
            char want[33];
            sl_sub_token(SB_PW, s, want);
            bool authed = (!strcmp(u, "tai") && !strcmp(want, t)) || (sb_q(q, "apiKey", want, sizeof want) && !strcmp(want, SB_KEY));
            if (!authed) status = 401;
            else if (sbv.gone || !sb_song_id(id, &a, &k)) status = 404;
            else
            {
                sbv.streams++;
                long long total = strcmp(fmt, "raw") ? 30000 + k * 100 : sb_song_size(a, k), from = 0, to = total - 1;
                const char *rg = strcasestr(req, "\nRange: bytes=");
                if (rg && !strcmp(fmt, "raw"))
                {
                    sscanf(rg + 14, "%lld-%lld", &from, &to);
                    if (to >= total) to = total - 1;
                    if (from > sbv.range_from_max) sbv.range_from_max = (int)from;
                    status = from >= total ? 416 : 206;
                }
                if (status != 416)
                {
                    len = (size_t)(to - from + 1);
                    if (len > cap) len = cap;
                    for (size_t i = 0; i < len; i++) body[i] = (char)((from + (long long)i) * 7 % 251);
                    if (sbv.drop_once && from > 0) { sbv.drop_once = false; len /= 2; status = -1; }   /* cut off half way */
                }
                if (status == 206) snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %lld-%lld/%lld\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", from, from + (long long)len - 1, total, len);
            }
            ctype = "audio/flac";
        }
        else len = sb_body(m, q, body, cap, &status, &ctype);
        if (status == -1)
        {
            snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", len * 2);
            sb_write_all(c, head, strlen(head));
            sb_write_all(c, body, len);
        }
        else
        {
            if (status != 206) snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                        status, status == 200 ? "OK" : "Error", ctype, len);
            sb_write_all(c, head, strlen(head));
            sb_write_all(c, body, len);
        }
        pthread_mutex_unlock(&sbv.mu);
        close(c);
    }
    free(body);
    return NULL;
}
static int sb_listen(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    assert(!bind(fd, (struct sockaddr *)&a, sizeof a));
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    *port = ntohs(a.sin_port);
    return fd;
}
static void sb_start(void)
{
    if (sbv.up) return;
    sbv.lfd = sb_listen(&sbv.port);
    assert(!listen(sbv.lfd, 16));
    int d = sb_listen(&sbv.down_port);
    close(d);                             /* nothing answers there */
    assert(!pthread_create(&sbv.thr, NULL, sb_serve, NULL));
    sbv.up = true;
}

/* ---- the simulator's network: an HTTP client for the server above */
static int sb_get(const char *url, const char *range, char **body, size_t *blen, long long *total)
{
    char host[64];
    int port = 0;
    const char *path = NULL;
    *body = NULL; *blen = 0;
    if (total) *total = -1;
    if (sscanf(url, "http://%63[^:/]:%d", host, &port) != 2 || strcmp(host, "127.0.0.1")) return -1;
    path = strchr(url + 7, '/');
    if (!path) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); return -1; }
    char req[1800];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: " SL_SUB_UA "\r\n%s%s%sConnection: close\r\n\r\n", path,
                     range ? "Range: bytes=" : "", range ? range : "", range ? "\r\n" : "");
    sb_write_all(fd, req, (size_t)n);
    size_t cap = 2 * 1024 * 1024 + 4096, got = 0;
    char *b = malloc(cap);
    for (;;) { ssize_t r = recv(fd, b + got, cap - got, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += (size_t)r; }
    close(fd);
    int status = got > 12 ? atoi(b + 9) : -1;
    char *end = got ? memmem(b, got, "\r\n\r\n", 4) : NULL;
    if (!end) { free(b); return -1; }
    size_t hl = (size_t)(end + 4 - b);
    char *cl = strcasestr(b, "Content-Length:");
    size_t want = cl && cl < end ? (size_t)atol(cl + 15) : got - hl;
    char *cr = strcasestr(b, "Content-Range: bytes ");
    if (total && cr && cr < end) { char *sl = strchr(cr, '/'); if (sl) *total = atoll(sl + 1); }
    if (got - hl < want) status = -1;     /* cut off */
    memmove(b, b + hl, got - hl);
    *body = b;
    *blen = got - hl;
    return status;
}
static int sb_http_fake(const char *url, char *out, size_t cap, const char *path)
{
    char *b;
    size_t n;
    int status = sb_get(url, NULL, &b, &n, NULL);
    if (status < 0) { free(b); return -1; }
    if (path)
    {
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(b, 1, n, f); fclose(f); }
    }
    else if (out && cap)
    {
        if (n >= cap) n = cap - 1;
        memcpy(out, b, n);
        out[n] = '\0';
        sl_net_http_fake_len = n;
    }
    free(b);
    return status;
}
/* a download: a slice a poll, asked for by Range from what the part has */
static int sb_dl_fake(const char *url, const char *path, long long have)
{
    pthread_mutex_lock(&sbv.mu);
    bool stall = sbv.stall;
    int slice = sbv.slice;
    pthread_mutex_unlock(&sbv.mu);
    if (stall) return 0;
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", have, have + slice - 1);
    char *b;
    size_t n;
    long long total;
    int status = sb_get(url, range, &b, &n, &total);
    if (status == 416) { free(b); return 206; }   /* all there */
    if (status == 206 || (status == -1 && b && n))
    {
        FILE *f = fopen(path, "ab");
        if (f) { fwrite(b, 1, n, f); fclose(f); }
        free(b);
        if (status == -1) return -1;           /* cut off: the part stays */
        return total > 0 && have + (long long)n >= total ? 206 : 0;
    }
    if (status == 200)
    {
        FILE *f = fopen(path, "wb");           /* sent whole: over the part */
        if (f) { fwrite(b, 1, n, f); fclose(f); }
        free(b);
        return 200;
    }
    free(b);
    return status;
}

/* ---- helpers */
static char sb_url_s[64];
static bool sb_is(const char *rb, long long size)
{
    struct stat st;
    return h_file(rb, &st) && st.st_size == size && h_file_pattern(rb, size);
}
static unsigned sb_seq0;
static bool sb_test_came(void) { return sub_st.test_seq != sb_seq0; }
static bool sb_idle(void) { struct sl_sub_status s; sl_sub_status(&s); return !s.job && !s.jobs && !s.dl_id[0] && !s.queued; }
static struct sub_view *sb_view;
static bool sb_answered(void) { return sb_view && !sb_view->waiting; }
static void sb_wait(bool (*done)(void), int ms)
{
    uint64_t until = h_ms() + (uint64_t)ms;
    while (h_ms() < until && !done()) { sub_poll(); frame(30); usleep(6000); }
    sub_poll();
    frame(40);
}
static struct sub_view *sb_top(void) { return sub_top_view(); }
static int sb_count_files(const char *dir)
{
    char cmd[400];
    snprintf(cmd, sizeof cmd, "find '" H_ROOT "%s' -type f \\( -name '*.flac' -o -name '*.mp3' \\) 2>/dev/null | wc -l", dir);
    FILE *f = popen(cmd, "r");
    int n = 0;
    if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; pclose(f); }
    return n;
}
static bool sb_never(void) { return false; }
static bool sb_ahead_done(void)
{
    return sb_idle() && sub_ses.pl_n == 1 + SL_SUB_AHEAD && queued == 1 + SL_SUB_AHEAD &&
           sb_count_files(SB_FOLDER) == 1 + SL_SUB_AHEAD;
}
static void sb_setup(void)
{
    sb_start();
    snprintf(sl_sub_folder, sizeof sl_sub_folder, "%s", SB_FOLDER);
    sl_sub_idle_ms = 1500;
    if (system("mkdir -p " H_ROOT "/Card")) {}
    sl_net_http_fake = sb_http_fake;
    sl_net_dl_fake = sb_dl_fake;
    snprintf(sb_url_s, sizeof sb_url_s, "127.0.0.1:%d", sbv.port);
    pthread_mutex_lock(&sbv.mu);
    sbv.html = sbv.slow = sbv.drop_once = sbv.stall = sbv.gone = false;
    sbv.slice = 30000;
    pthread_mutex_unlock(&sbv.mu);
}
static void sb_reset(void)
{
    while (npages > 1) { page_pop(); frame(300); }
    sub_ses_end();
    sl_sub_test_forget();
    assert(!system("rm -rf " H_ROOT SL_SUB_STATE " " H_ROOT SB_FOLDER));
    sl_sub_test_forget();
    sl_sub_test_limit_kb = 0;
    memset(&sub_st, 0, sizeof sub_st);
    sl_sub_status(&sub_st);
}
/* signed in as a person would: the page, the words, Sign in */
static void sb_signin(const char *user, const char *secret)
{
    while (npages > 1) { page_pop(); frame(300); }
    sub_edit_page(0); frame(300);
    lv_textarea_set_text(sub_url_ta, sb_url_s);
    lv_textarea_set_text(sub_user_ta, user);
    lv_textarea_set_text(sub_pass_ta, secret);
    sb_seq0 = sub_st.test_seq;
    sub_save_cb(NULL);
    sb_wait(sb_test_came, 8000);
}

static void sb_tests(void)
{
    struct mp3entry track0 = test_track;
    int status0 = test_status;
    bool conn0 = wf_st.connected;
    sb_setup();
    sb_reset();
    wf_st.connected = true; sl_sub_online(true);
    /* ---- the pieces */
    char tok[33], u[200];
    sl_sub_token("sesame", "c19b2d", tok);
    assert(!strcmp(tok, "26719a1196d2a940705a59634eb18eab"));     /* the Subsonic API's own example */
    assert(sl_sub_url_fix("192.168.1.5:4533", u, sizeof u) && !strcmp(u, "http://192.168.1.5:4533"));
    assert(sl_sub_url_fix(" music.example.com/ ", u, sizeof u) && !strcmp(u, "https://music.example.com"));
    assert(sl_sub_url_fix("http://nas.local:4040/rest/", u, sizeof u) && !strcmp(u, "http://nas.local:4040"));
    assert(sl_sub_url_fix("https://x.org/navidrome", u, sizeof u) && !strcmp(u, "https://x.org/navidrome"));
    assert(!sl_sub_url_fix("ftp://x.org", u, sizeof u) && !sl_sub_url_fix("", u, sizeof u) && !sl_sub_url_fix("a b?c", u, sizeof u));
    sl_sub_file_name(u, sizeof u, 2, 3, "What: Now?", "flac");
    assert(!strcmp(u, "2-03 What Now.flac"));
    sl_sub_file_name(u, sizeof u, 1, 7, "", "mp3");
    assert(!strcmp(u, "07 Track.mp3"));
    /* a server's suffix is never a path: only letters and digits */
    sl_sub_file_name(u, sizeof u, 1, 1, "A", "x/../../../../etc/foo");
    assert(!strcmp(u, "01 A.mp3"));
    sl_sub_file_name(u, sizeof u, 1, 1, "A", ".FLAC");
    assert(!strcmp(u, "01 A.FLAC"));
    assert(!strcmp(sl_pod_clean_ext("..", "m"), "m") && !strcmp(sl_pod_clean_ext("abcdefghi", "m"), "m") && !strcmp(sl_pod_clean_ext("m4b", "x"), "m4b"));
    {
        /* the reader, a byte at a time and in sevens: escapes, a surrogate pair, objects nested in a song, nulls */
        static const char js[] = "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\",\"album\":{\"id\":\"al9\",\"name\":\"Caf\\u00e9 \\\"Q\\\"\","
            "\"artist\":\"A\\/B\",\"year\":1999,\"song\":[{\"id\":\"x1\",\"title\":\"Hi \\ud83c\\udfb5\",\"track\":3,\"discNumber\":null,"
            "\"artists\":[{\"id\":\"zz\",\"title\":\"no\"}],\"duration\":61,\"size\":12345678901,\"genres\":[{\"name\":\"Pop\"}],\"starred\":\"2026\"},"
            "{\"id\":\"x2\",\"title\":\"Two\",\"track\":4,\"duration\":-3}]}}}";
        static const size_t slices[] = { 1, 7, 4096 };
        for (int i = 0; i < 3; i++)
        {
            struct sl_sub_list l;
            assert(sl_sub_test_parse(js, slices[i], SL_SUB_Q_ALBUM, &l) == 0);
            assert(l.n == 2 && l.head.kind == SL_SUB_ALBUM && !strcmp(sl_sub_s(&l, l.head.title), "Caf\xc3\xa9 \"Q\"") && l.head.year == 1999);
            assert(!strcmp(sl_sub_s(&l, l.head.artist), "A/B") && !strcmp(sl_sub_s(&l, l.it[0].id), "x1"));
            assert(!strcmp(sl_sub_s(&l, l.it[0].title), "Hi \xf0\x9f\x8e\xb5") && l.it[0].track == 3 && l.it[0].dur == 61 && l.it[0].starred);
            assert(l.it[0].size == 0x7fffffff && !l.it[1].dur && !l.it[1].starred);
            sl_sub_list_free(&l);
        }
        struct sl_sub_list l;
        assert(sl_sub_test_parse("{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":40,\"message\":\"no\"}}}", 3, SL_SUB_Q_ALBUM, &l) == 40);
        sl_sub_list_free(&l);
        assert(sl_sub_test_parse("<html><body>{}</body></html>", 5, SL_SUB_Q_ALBUM, &l) == SL_SUB_E_NOTSUB);
        sl_sub_list_free(&l);
    }
    printf("PASS: subsonic: the token (the API's example), addresses, file names, the answers' reader in any slices.\n");

    /* ---- no server: the page says what to do; signing in, and its errors in words */
    subsonic_page(); frame(300);
    assert(h_text_ci(h_top(), "No music server yet."));
    while (npages > 1) { page_pop(); frame(300); }
    sb_signin("tai", "wrong");
    assert(sub_st.test_err == SL_SUB_E_AUTH && !sl_sub_server(0, NULL) && sub_edit_obj);
    assert(h_text_ci(sub_edit_obj, "Wrong user name or password."));
    {
        /* the token: u, t (32 hex), a salt new each time, never the password */
        char t[64] = "", s[64] = "", v[16] = "", c[32] = "", f[16] = "", p[64];
        assert(sb_q(sbv.last_q, "u", u, sizeof u) && !strcmp(u, "tai") && sb_q(sbv.last_q, "t", t, sizeof t) && strlen(t) == 32);
        assert(sb_q(sbv.last_q, "s", s, sizeof s) && strlen(s) >= 6 && sb_q(sbv.last_q, "v", v, sizeof v) && sb_q(sbv.last_q, "c", c, sizeof c) &&
               sb_q(sbv.last_q, "f", f, sizeof f) && !strcmp(f, "json") && !sb_q(sbv.last_q, "p", p, sizeof p));
    }
    pthread_mutex_lock(&sbv.mu); sbv.html = true; pthread_mutex_unlock(&sbv.mu);
    sb_signin("tai", SB_PW);
    assert(sub_st.test_err == SL_SUB_E_NOTSUB);
    pthread_mutex_lock(&sbv.mu); sbv.html = false; pthread_mutex_unlock(&sbv.mu);
    snprintf(sb_url_s, sizeof sb_url_s, "127.0.0.1:%d", sbv.down_port);
    sb_signin("tai", SB_PW);
    assert(sub_st.test_err == SL_SUB_E_NET && h_text_ci(sub_edit_obj, "The server didn't answer."));
    snprintf(sb_url_s, sizeof sb_url_s, "my nas?.local");
    sb_signin("tai", SB_PW);
    assert(h_text_ci(sub_edit_obj, "That doesn't look like an address."));
    snprintf(sb_url_s, sizeof sb_url_s, "127.0.0.1:%d", sbv.port);
    /* an API key where the password goes: the OpenSubsonic way, for a server that offers it */
    sb_signin("keyuser", SB_KEY);
    struct sl_sub_server srv;
    assert(!sub_st.test_err && sub_st.test_api_key && sl_sub_server(0, &srv) && srv.api_key && srv.open && !strcmp(srv.name, "Navidrome"));
    assert(strstr(sbv.last_q, "apiKey=" SB_KEY) && !strstr(sbv.last_q, "u="));
    assert(sl_sub_remove(srv.id) && !sl_sub_server(0, NULL));
    int salts0 = sbv.nsalt;
    sb_signin("tai", SB_PW);
    assert(!sub_st.test_err && sub_st.test_saved && !sub_st.test_api_key && sl_sub_server(0, &srv) && !strcmp(srv.version, "0.53.3"));
    assert(!sub_edit_obj || h_top() != sub_edit_obj);   /* signed in: the page goes */
    assert(tagcache_folder_listed(sl_sub_folder));      /* the cache stays out of the library: songs show once */
    {
        /* the password: on the card for the owner only, not as it is, nowhere else */
        struct stat st;
        char p[600];
        snprintf(p, sizeof p, H_ROOT "%s", SL_SUB_STATE "/servers.tsv");
        assert(!stat(p, &st) && (st.st_mode & 077) == 0);
        assert(!h_has(SL_SUB_STATE "/servers.tsv", "s3cret") && !h_has("/.rockbox/sloppy/sloppy.log", "s3cret"));
        assert(sbv.nsalt >= salts0 + 1 && !sbv.plain_pw_seen);
        for (int i = salts0; i < sbv.nsalt; i++) for (int k = 0; k < i; k++) assert(strcmp(sbv.salts[i], sbv.salts[k]));
        /* read again from the card, the same */
        sl_sub_test_forget();
        struct sl_sub_server again;
        assert(sl_sub_server(0, &again) && again.id == srv.id && !strcmp(again.url, srv.url));
    }
    printf("PASS: subsonic: signing in: wrong password, not a Subsonic server, no answer and a bad address each say so; an API key; the token new each time; the password 0600 and never as it is.\n");

    /* ---- browsing */
    while (npages > 1) { page_pop(); frame(300); }
    subsonic_page(); frame(200);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.n == SL_SUB_PAGE_N && !strcmp(sbv.last_type, "newest") && h_text_ci(h_top(), "Album 3"));
    sub_link_open(0);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->what == SL_SUB_Q_ARTISTS && sb_view->l.n == 75 && h_text_ci(h_top(), "The Paper Kite") && !h_text_ci(h_top(), "Artist 70"));
    h_click(h_row(h_top(), "More  " LV_SYMBOL_RIGHT));
    assert(sb_view->start == SUB_ARTISTS_PER && h_text_ci(h_top(), "Artist 70") && sbv.reqs);
    page_pop(); frame(300);
    sub_link_open(1);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.n == SL_SUB_PAGE_N && sb_view->l.more);
    h_click(h_row(h_top(), "More  " LV_SYMBOL_RIGHT));
    sb_wait(sb_answered, 8000);
    {
        char off[16];
        assert(sb_q(sbv.last_q, "offset", off, sizeof off) && atoi(off) == 40 && h_text_ci(h_top(), "Album 41"));
    }
    sub_order_pick(3);                    /* Starred */
    sb_wait(sb_answered, 8000);
    assert(!strcmp(sbv.last_type, "starred") && sb_view->l.n == 3 && !sb_view->l.more);
    page_pop(); frame(300);
    sub_album_page("al2", "Two Discs");
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.n == 6 && h_text_ci(h_top(), "Two Discs, \"Live\"") && h_text_ci(h_top(), "DISC 2") && h_text_ci(h_top(), "Keep offline"));
    page_pop(); frame(300);
    sub_link_open(2);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.n == 2 && h_text_ci(h_top(), "Late Night Drive"));
    page_pop(); frame(300);
    sub_link_open(3);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.n == 3 && h_text_ci(h_top(), "Album 3"));
    page_pop(); frame(300);
    sub_find_page(); frame(200);
    lv_textarea_set_text(sub_find_ta, "kite");
    sub_find_go_cb(NULL);
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->what == SL_SUB_Q_SEARCH && sb_view->l.n == 6 && h_text_ci(h_top(), "The Paper Kite") && h_text_ci(h_top(), "Bloom"));
    while (npages > 1) { page_pop(); frame(300); }
    sub_list_page(SL_SUB_Q_ALBUM, "al999", "Gone");
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    assert(sb_view->l.err == SL_SUB_E_NOTFOUND && h_text_ci(h_top(), "It's no longer on the server."));
    /* covers came, made on the worker */
    sb_wait(sb_idle, 8000);
    assert(sbv.art_gets > 0 && sl_sub_art_have("al3"));
    printf("PASS: subsonic: artists (paged), albums by order (paged by the server), an album of two discs, playlists, starred, search, a missing album; covers.\n");

    /* ---- a slow server never holds the UI */
    {
        pthread_mutex_lock(&sbv.mu); sbv.slow = true; pthread_mutex_unlock(&sbv.mu);
        sub_list_page(SL_SUB_Q_PLAYLISTS, "", "Playlists");
        uint64_t worst = 0, w0 = h_ms();
        while (h_ms() - w0 < 1500) { uint64_t f0 = h_ms(); sub_poll(); frame(16); if (h_ms() - f0 > worst) worst = h_ms() - f0; usleep(4000); }
        printf("subsonic: a server that takes 2.5 s: worst UI frame %u ms meanwhile\n", (unsigned)worst);
        assert(worst < 100 && sb_top()->waiting && lv_obj_get_user_data(lv_obj_get_child(sb_top()->body, 0)) == (void *)1);
        pthread_mutex_lock(&sbv.mu); sbv.slow = false; pthread_mutex_unlock(&sbv.mu);
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        while (npages > 1) { page_pop(); frame(300); }
    }

    /* ---- playing: the first comes down, plays, the next few come while it does */
    test_status = 0;
    sub_album_page("al1", "Kite");
    sb_view = sb_top();
    sb_wait(sb_answered, 8000);
    int streams0 = sbv.streams;
    sub_play(sb_view, -1, false);
    for (int i = 0; i < 300 && !sub_ses.pl_n; i++) { sub_poll(); frame(20); usleep(5000); }
    char rb[400];
    assert(sub_ses.pl_n >= 1 && started == 0 && test_status == AUDIO_STATUS_PLAY);
    assert(sl_sub_cached(srv.id, "s1-1", test_track.path, sizeof test_track.path));   /* Rockbox plays it now */
    assert(sl_sub_cached(srv.id, "s1-1", rb, sizeof rb) && !strncmp(rb, SB_FOLDER "/The Paper Kite/Caf", strlen(SB_FOLDER "/The Paper Kite/Caf")));
    assert(sb_is(rb, sb_song_size(1, 1)));
    {
        struct playlist_track_info inf;
        assert(playlist_get_track_info(NULL, 0, &inf) >= 0 && !strcmp(inf.filename, rb));
    }
    /* idle alone is not enough: between two songs the worker rests until
     * the next tick asks for the next one. Then hold on: none further may
     * come (the whole album once did, after the few ahead were in) */
    sb_wait(sb_ahead_done, 8000);
    sb_wait(sb_never, 1500);
    /* read ahead: the one playing and three more, no further */
    assert(sb_count_files(SB_FOLDER) == 1 + SL_SUB_AHEAD && sub_ses.pl_n == 1 + SL_SUB_AHEAD && queued == 1 + SL_SUB_AHEAD);
    {
        /* the cover beside the songs, for Now Playing */
        struct stat st;
        char cov[400];
        snprintf(cov, sizeof cov, "%s/The Paper Kite/Caf\xc3\xa9 Kite/cover.jpg", SB_FOLDER);
        assert(h_file(cov, &st) && st.st_size > 100);
    }
    /* the third plays: the rest come */
    started = 2;
    test_track.elapsed = 1000;
    sl_sub_cached(srv.id, "s1-3", test_track.path, sizeof test_track.path);
    sub_poll();
    sb_wait(sb_idle, 8000);
    for (int i = 0; i < 20; i++) { sub_poll(); frame(20); }
    assert(sb_count_files(SB_FOLDER) == 6 && sub_ses.pl_n == 6 && queued == 6 && sbv.streams >= streams0 + 6);
    printf("PASS: subsonic: Play downloads the first song, plays it with its cover beside it, and fetches %d ahead; the rest as it plays.\n", SL_SUB_AHEAD);

    /* ---- a download cut off half way carries on from where it was (Range) */
    {
        pthread_mutex_lock(&sbv.mu); sbv.drop_once = true; sbv.range_from_max = 0; pthread_mutex_unlock(&sbv.mu);
        sub_album_page("al3", "Album 3");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        int idx[1] = { 0 };
        unsigned s = sl_sub_session_new();
        assert(sl_sub_fetch(&sb_view->l, idx, 1, s) == 1);
        uint64_t t0 = h_ms();
        while (h_ms() - t0 < 12000 && !sl_sub_cached(srv.id, "s3-1", rb, sizeof rb)) { sub_poll(); frame(30); usleep(10000); }
        assert(sl_sub_cached(srv.id, "s3-1", rb, sizeof rb) && sb_is(rb, sb_song_size(3, 1)) && sbv.range_from_max >= sbv.slice);
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: subsonic: a download cut off carries on by Range from its part, whole and right.\n");

    /* ---- plays told to the server; a star from a song's sheet */
    {
        int now0 = sbv.scrob_now, sub0 = sbv.scrob_sub;
        sl_sub_cached(srv.id, "s1-2", test_track.path, sizeof test_track.path);
        sub_started(&test_track, 1234);
        struct sl_play p = { .path_hash = 1234, .when = 1792576800u, .ms = 200000 };
        sub_counted(&p, 210000);
        sb_wait(sb_idle, 8000);
        for (int i = 0; i < 60 && sbv.scrob_sub == sub0; i++) { sub_poll(); frame(30); usleep(10000); }
        assert(sbv.scrob_now == now0 + 1 && sbv.scrob_sub == sub0 + 1 && strstr(sbv.last_scrob, "id=s1-2") && strstr(sbv.last_scrob, "time=1792576800000"));
        sub_album_page("al1", "Kite");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sub_song_sheet(sb_view, 2); frame(300);
        assert(acts_open_now && h_text_ci(acts, "Star"));
        lv_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.user_data = (void *)1;
        sub_sheet_cb(&ev); frame(300);
        sb_wait(sb_idle, 8000);
        for (int i = 0; i < 60 && !sbv.stars; i++) { sub_poll(); frame(30); usleep(10000); }
        assert(sbv.stars == 1 && !strcmp(sbv.last_star, "s1-3"));
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: subsonic: a play is told to the server (now playing, then counted, its time); a song starred from its sheet.\n");

    /* ---- kept offline, and the room on the card: the least recently played go, never the kept */
    {
        sub_ses_end();
        sl_sub_session_new();
        sub_album_page("al2", "Two Discs");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sub_keep_apply(1);
        uint64_t t0 = h_ms();
        while (h_ms() - t0 < 12000 && !sb_idle()) { sub_poll(); frame(30); usleep(10000); }
        assert(sl_sub_pinned(SL_SUB_ALBUM, "al2"));
        for (int n = 1; n <= 6; n++) { char id[16]; snprintf(id, sizeof id, "s2-%d", n); assert(sl_sub_cached(srv.id, id, NULL, 0)); }
        while (npages > 1) { page_pop(); frame(300); }
        long long used, limit;
        sl_sub_usage(&used, &limit, NULL, NULL);
        /* room for what is here and three more songs: playing album 4 makes room from album 1's, the oldest */
        sl_sub_test_limit_kb = (unsigned)(used / 1024) + 3 * 70;
        sub_album_page("al4", "Album 4");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sub_play(sb_view, -1, false);
        t0 = h_ms();
        while (h_ms() - t0 < 15000 && (!sb_idle() || sub_ses.pl_n < 4)) { sub_poll(); frame(30); usleep(10000); }
        sl_sub_usage(&used, &limit, NULL, NULL);
        assert(used <= limit && sub_ses.pl_n == 4);
        for (int n = 1; n <= 6; n++) { char id[16]; snprintf(id, sizeof id, "s2-%d", n); assert(sl_sub_cached(srv.id, id, NULL, 0)); }   /* kept */
        for (int n = 1; n <= 4; n++) { char id[16]; snprintf(id, sizeof id, "s4-%d", n); assert(sl_sub_cached(srv.id, id, NULL, 0)); }   /* this play's */
        assert(!sl_sub_cached(srv.id, "s1-1", NULL, 0));   /* the oldest went */
        sl_sub_test_limit_kb = 0;
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: subsonic: an album kept offline comes down whole and stays; at the limit the least recently played go first.\n");

    /* ---- USB mode: a download stops at once, its part kept, nothing written; it carries on after */
    {
        sub_ses_end();
        sl_sub_session_new();
        pthread_mutex_lock(&sbv.mu); sbv.slice = 8000; pthread_mutex_unlock(&sbv.mu);
        sub_album_page("al5", "Album 5");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sub_keep_apply(1);
        uint64_t t0 = h_ms();
        struct sl_sub_status s;
        do { sub_poll(); frame(30); usleep(10000); sl_sub_status(&s); } while (h_ms() - t0 < 5000 && s.dl_got < 8000);
        assert(s.dl_id[0] && s.dl_got >= 8000);
        struct stat c0;
        assert(h_file(SL_SUB_STATE "/cache.tsv", &c0));
        sl_sub_hold(true);
        sl_sub_status(&s);
        assert(!s.dl_id[0] && s.hold);
        int files0 = sb_count_files(SB_FOLDER);
        char part[600];
        snprintf(part, sizeof part, "find '" H_ROOT SB_FOLDER "' -name '*.part' | wc -l");
        FILE *f = popen(part, "r"); int parts = 0; if (f) { if (fscanf(f, "%d", &parts) != 1) parts = 0; pclose(f); }
        assert(parts == 1);
        for (int i = 0; i < 40; i++) { sub_poll(); frame(30); usleep(10000); }
        struct stat c1;
        assert(h_file(SL_SUB_STATE "/cache.tsv", &c1) && c1.st_mtime == c0.st_mtime && c1.st_size == c0.st_size && sb_count_files(SB_FOLDER) == files0);
        sl_sub_hold(false);
        pthread_mutex_lock(&sbv.mu); sbv.slice = 30000; pthread_mutex_unlock(&sbv.mu);
        t0 = h_ms();
        while (h_ms() - t0 < 12000 && !sb_idle()) { sub_poll(); frame(30); usleep(10000); }
        for (int n = 1; n <= 4; n++) { char id[16]; snprintf(id, sizeof id, "s5-%d", n); assert(sl_sub_cached(srv.id, id, rb, sizeof rb) && sb_is(rb, sb_song_size(5, n))); }
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: subsonic: USB mode stops a download at once, its part kept and nothing written, and it carries on after.\n");

    /* ---- offline: no Wi-Fi, or the server gone: what is on the card shows and plays */
    {
        wf_st.connected = false; sl_sub_online(false);
        sub_poll(); frame(100);
        subsonic_page(); frame(200);
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        assert(sb_view->l.offline && sb_view->l.n >= 3 && h_text_ci(h_top(), "Offline") && h_text_ci(h_top(), "Two Discs"));
        sub_album_page("al2", "Two Discs");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        assert(!sb_view->l.err && sb_view->l.offline && sb_view->l.n == 6 && sb_view->l.head.pinned && h_text_ci(h_top(), "Offline: the songs on the card"));
        test_status = 0;
        sub_play(sb_view, -1, true);
        for (int i = 0; i < 40 && !sub_ses.pl_n; i++) { sub_poll(); frame(20); }
        assert(sub_ses.pl_n == 6 && test_status == AUDIO_STATUS_PLAY);   /* all on the card: at once */
        while (npages > 1) { page_pop(); frame(300); }
        sub_offline_page(); frame(200);
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        assert(sb_view->l.n >= 3 && h_text_ci(h_top(), "Remove songs not kept offline"));
        while (npages > 1) { page_pop(); frame(300); }
        /* Wi-Fi back but the server gone: an album on the card still opens */
        wf_st.connected = true; sl_sub_online(true);
        snprintf(srv.url, sizeof srv.url, "x");
        sl_sub_test_forget();
        {
            /* the server moved away: its address now answers nothing */
            char p[600];
            snprintf(p, sizeof p, "sed -i '' 's/:%d/:%d/' '" H_ROOT SL_SUB_STATE "/servers.tsv'", sbv.port, sbv.down_port);
            assert(!system(p));
            sl_sub_test_forget();
        }
        sub_album_page("al5", "Album 5");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        assert(!sb_view->l.err && sb_view->l.offline && sb_view->l.n == 4);
        while (npages > 1) { page_pop(); frame(300); }
        char p[600];
        snprintf(p, sizeof p, "sed -i '' 's/:%d/:%d/' '" H_ROOT SL_SUB_STATE "/servers.tsv'", sbv.down_port, sbv.port);
        assert(!system(p));
        sl_sub_test_forget();
    }
    printf("PASS: subsonic: offline (no Wi-Fi, or the server gone) the card's albums show, open and play.\n");

    /* ---- memory: what the pages hold, and what stays after (the R1 has 4-5 MB free) */
    {
        sub_ses_end();
        sl_sub_session_new();
        while (npages > 1) { page_pop(); frame(300); }
        sb_wait(sb_idle, 8000);
        for (int i = 0; i < 10; i++) { sub_poll(); frame(100); }
        malloc_statistics_t m0, m1, m2, m3;
        malloc_zone_statistics(NULL, &m0);
        subsonic_page(); frame(200);
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sb_wait(sb_idle, 8000);
        for (int i = 0; i < 6; i++) { sub_poll(); frame(60); }
        malloc_zone_statistics(NULL, &m1);
        sub_album_page("al1", "Kite");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sub_list_page(SL_SUB_Q_ARTISTS, "", "Artists");
        sb_view = sb_top();
        sb_wait(sb_answered, 8000);
        sb_wait(sb_idle, 8000);           /* the covers made: the test's own HTTP buffers gone */
        for (int i = 0; i < 6; i++) { sub_poll(); frame(60); }
        malloc_zone_statistics(NULL, &m2);
        while (npages > 1) { page_pop(); frame(300); }
        sb_wait(sb_idle, 8000);
        for (int i = 0; i < 10; i++) { sub_poll(); frame(100); }
        malloc_zone_statistics(NULL, &m3);
        long home = ((long)m1.size_in_use - (long)m0.size_in_use) / 1024, more = ((long)m2.size_in_use - (long)m1.size_in_use) / 1024;
        long after = ((long)m3.size_in_use - (long)m0.size_in_use) / 1024;
        printf("subsonic: memory: the worker's state %u KB always; the server's page %ld KB, an album and 75 artists %ld KB more; %ld KB left after they close\n",
               (unsigned)(sl_sub_test_static_bytes() / 1024), home, more, after);
#if !__has_feature(address_sanitizer)
        assert(after < 48);
#endif
    }
    printf("PASS: subsonic: memory back to where it was once the pages close.\n");
    while (npages > 1) { page_pop(); frame(300); }
    if (np_open) { np_hide(0); frame(500); }
    if (acts_open_now) { acts_close(); frame(300); }
    sub_ses_end();
    sl_sub_session_new();
    sb_wait(sb_idle, 4000);
    test_track = track0; test_status = status0;
    started = 0;
    wf_st.connected = conn0; sl_sub_online(conn0);
    sl_net_http_fake = NULL; sl_net_dl_fake = NULL;
    tagcache_set_folder_excluded(sl_sub_folder, false);   /* as the suite had it */
}

/* ---- the screens, for the audit and the shots */
static bool sb_au_dirty, sb_au_conn0, sb_au_made;
static struct mp3entry sb_track0;
static void sb_au_reset(void)
{
    if (!sb_au_dirty) return;
    sb_au_dirty = false;
    if (acts_open_now) { acts_close(); frame(300); }
    pthread_mutex_lock(&sbv.mu); sbv.stall = sbv.slow = false; pthread_mutex_unlock(&sbv.mu);
    sub_ses_end();
    for (int i = 0; i < 100 && !sb_idle(); i++) { sub_poll(); frame(20); usleep(5000); }
    test_track = sb_track0;
    wf_st.connected = sb_au_conn0; sl_sub_online(sb_au_conn0);
    sl_net_http_fake = NULL; sl_net_dl_fake = NULL;
    tagcache_set_folder_excluded(sl_sub_folder, false);
}
static void sb_au_state(bool server)
{
    if (!sb_au_dirty) { sb_au_dirty = true; sb_au_conn0 = wf_st.connected; sb_track0 = test_track; }
    sb_setup();
    wf_st.connected = true; sl_sub_online(true);
    if (!server) { sb_reset(); sb_au_made = false; return; }
    if (!sb_au_made)
    {
        /* signed in, two albums on the card (one kept offline), the covers made */
        sb_reset();
        sb_signin("tai", SB_PW);
        while (npages > 1) { page_pop(); frame(300); }
        subsonic_page(); sb_view = sb_top(); sb_wait(sb_answered, 8000);
        sub_album_page("al1", "Kite"); sb_view = sb_top(); sb_wait(sb_answered, 8000);
        int idx[3] = { 0, 1, 2 };
        sl_sub_fetch(&sb_view->l, idx, 3, sl_sub_session_new());
        sub_album_page("al2", "Two"); sb_view = sb_top(); sb_wait(sb_answered, 8000);
        sub_keep_apply(1);
        uint64_t t0 = h_ms();
        while (h_ms() - t0 < 15000 && !sb_idle()) { sub_poll(); frame(30); usleep(8000); }
        sub_list_page(SL_SUB_Q_ARTISTS, "", "Artists"); sb_view = sb_top(); sb_wait(sb_answered, 8000);
        t0 = h_ms();
        while (h_ms() - t0 < 15000 && !sb_idle()) { sub_poll(); frame(30); usleep(8000); }
        sb_au_made = true;
    }
    while (npages > 1) { page_pop(); frame(300); }
    sub_poll();
}
static void sb_open_wait(void) { sb_view = sb_top(); sb_wait(sb_answered, 8000); for (int i = 0; i < 6; i++) { sub_poll(); frame(60); } }
static void sb_au(int which)
{
    sb_au_state(which != 0);
    switch (which)
    {
        case 0: subsonic_page(); break;                                   /* no server */
        case 1: sub_edit_page(0); lv_textarea_set_text(sub_url_ta, "music.example.com"); lv_textarea_set_text(sub_user_ta, "tai");
                lv_textarea_set_text(sub_pass_ta, "secret"); sub_test_say("Signing in\xe2\x80\xa6", C_SUB); break;
        case 2: sub_edit_page(0); lv_textarea_set_text(sub_url_ta, sb_url_s); lv_textarea_set_text(sub_user_ta, "tai");
                lv_textarea_set_text(sub_pass_ta, "wrong"); sb_seq0 = sub_st.test_seq; sub_test_cb(NULL); sb_wait(sb_test_came, 8000); break;
        case 3: subsonic_page(); sb_open_wait(); break;                   /* home */
        case 4: sub_list_page(SL_SUB_Q_ALBUMS, "newest", "Albums"); sb_open_wait(); break;
        case 5: sub_list_page(SL_SUB_Q_ARTISTS, "", "Artists"); sb_open_wait(); break;
        case 6: sub_album_page("al1", "Kite"); sb_open_wait(); break;
        case 7:                                                           /* a song coming down, its percent */
        {
            sub_album_page("al1", "Kite"); sb_open_wait();
            pthread_mutex_lock(&sbv.mu); sbv.slice = 30000; pthread_mutex_unlock(&sbv.mu);
            int idx[1] = { 4 };
            sl_sub_fetch(&sb_view->l, idx, 1, sl_sub_session_new());
            struct sl_sub_status s;
            uint64_t t0 = h_ms();
            do { sub_poll(); frame(30); usleep(5000); sl_sub_status(&s); } while (h_ms() - t0 < 5000 && s.dl_got < 30000);
            pthread_mutex_lock(&sbv.mu); sbv.stall = true; pthread_mutex_unlock(&sbv.mu);
            for (int i = 0; i < 10; i++) { sub_poll(); frame(40); }
            break;
        }
        case 8: wf_st.connected = false; sl_sub_online(false); sub_poll(); subsonic_page(); sb_open_wait(); break;   /* offline */
        case 9: sub_offline_page(); sb_open_wait(); break;
        case 10: sub_list_page(SL_SUB_Q_SEARCH, "kite", "kite"); sb_open_wait(); break;
        case 11: sub_list_page(SL_SUB_Q_SEARCH, "zzz", "zzz"); sb_open_wait(); break;
        case 12: sub_servers_page(); break;
        case 13: sub_album_page("al1", "Kite"); sb_open_wait(); sub_song_sheet(sb_view, 1); break;
        case 14: sub_playlist_page("p1", "Late Night Drive"); sb_open_wait(); break;
        case 15: sub_album_page("al2", "Two"); sb_open_wait(); break;     /* kept offline, two discs */
        case 16: sub_find_page(); lv_textarea_set_text(sub_find_ta, "kite"); break;
        case 17: pthread_mutex_lock(&sbv.mu); sbv.slow = true; pthread_mutex_unlock(&sbv.mu);
                 sub_list_page(SL_SUB_Q_PLAYLISTS, "", "Playlists"); frame(200); break;   /* waiting */
        case 18: sub_list_page(SL_SUB_Q_ALBUM, "al999", "Gone"); sb_open_wait(); break;   /* an error */
        case 19: { struct sl_sub_server s; sl_sub_server(0, &s); sub_edit_page(s.id); } break;
    }
}
