/* R1 (owners on r/Hiby): Audiobookshelf, its books on the card and the
 * place kept in step both ways (sloppy_absjob.inc, sloppy_abs.inc).
 * Included from ui_test.c after batch_sub.inc, whose socket helpers and
 * download client it shares.
 *   ab_tests()               in the suite
 *   SLOPPY_ABS_TESTS=1       the tests alone
 *   audit screens abs-*, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=abs- ./ui-test
 * An Audiobookshelf server of our own answers on 127.0.0.1, on a thread
 * here, over a real socket: the requests are sloppy_net.c's own client
 * (sl_net_http_req: methods, a Bearer header, a JSON body, chunked
 * answers); the files come by Range through batch_sub's download client. */
#define AV_FOLDER "/Card/Audiobooks"
#define AV_DIR AV_FOLDER "/Frank Herbert/Dune"
#define AV_PW "pw &\"x\\y"                 /* a quote and a backslash: the JSON escapes them */
#define AV_API "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOiJ1MSIsInR5cGUiOiJhcGkifQ.c2lnbmF0dXJlLXRlc3Q"
#define AV_F1 90000LL
#define AV_F2 70000LL
#define AV_E1 50000LL

struct av_prog { char key[64]; double cur, dur; bool fin, have; long long upd; };
static struct {
    int port, lfd;
    pthread_t thr;
    bool up;
    pthread_mutex_t mu;
    /* what it saw */
    int reqs, logins, refreshes, patches, bad_auth, file_gets, range_from_max, me_gets, chunked_sent;
    char last_auth[700], last_path[700], last_filter[200], last_patch[300], last_patch_path[200], last_ua[200];
    /* how it acts */
    char access[120];
    int access_n;
    bool slow, expire, html;
    long long skew;                       /* its clock less ours, seconds */
    char device[64];
    struct av_prog prog[8];
    int nprog;
} av = { .mu = PTHREAD_MUTEX_INITIALIZER };
static long long av_now_ms(void) { return ((long long)time(NULL) + av.skew) * 1000; }
static struct av_prog *av_prog(const char *key, bool make)
{
    for (int i = 0; i < av.nprog; i++) if (!strcmp(av.prog[i].key, key)) return &av.prog[i];
    if (!make || av.nprog >= 8) return NULL;
    struct av_prog *p = &av.prog[av.nprog++];
    memset(p, 0, sizeof *p);
    snprintf(p->key, sizeof p->key, "%s", key);
    return p;
}
/* a JSON string's value of key in body (the escapes undone) */
static bool av_json_str(const char *body, const char *key, char *out, size_t n)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *s = strstr(body, pat);
    if (!s) return false;
    s += strlen(pat);
    size_t k = 0;
    for (; *s && *s != '"' && k + 1 < n; s++) { if (*s == '\\' && s[1]) s++; out[k++] = *s; }
    out[k] = '\0';
    return true;
}
static double av_json_num(const char *body, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *s = strstr(body, pat);
    return s ? atof(s + strlen(pat)) : -1;
}
static const char *av_title(int n) { static char b[32]; if (n == 1) return "Dune"; if (n == 2) return "Children of Dune"; snprintf(b, sizeof b, "Book %02d", n); return b; }
static const char *av_author(int n) { static char b[32]; if (n <= 2) return "Frank Herbert"; snprintf(b, sizeof b, "Author %d", n); return b; }
static long long av_size(int n) { return n == 1 ? AV_F1 + AV_F2 : n == 2 ? 400LL * 1024 * 1024 : 50000000LL; }
static size_t av_mini(char *o, size_t cap, int n, bool upd)
{
    struct av_prog *p = n == 1 ? av_prog("b1", false) : NULL;
    return (size_t)snprintf(o, cap, "{\"id\":\"b%d\",\"libraryId\":\"lib1\",\"mediaType\":\"book\",\"media\":{\"metadata\":{\"title\":\"%s\",\"authorName\":\"%s\","
        "\"narratorName\":\"Scott Brick\",\"seriesName\":\"Dune Saga #%d\"},\"numChapters\":3,\"numTracks\":2,\"duration\":%d.5,\"size\":%lld},"
        "\"size\":%lld%s%lld}", n, av_title(n), av_author(n), n, 7200 + n, av_size(n), av_size(n), upd ? ",\"progressLastUpdate\":" : ",\"addedAt\":",
        upd && p ? p->upd : 1700000000000LL);
}
static size_t av_body(const char *method, const char *path, const char *q, const char *body, char *o, size_t cap, int *status, bool *chunk)
{
    size_t k = 0;
    *status = 200;
    *chunk = false;
    char v[200];
    if (!strcmp(path, "/status"))
        return (size_t)snprintf(o, cap, "{\"isInit\":true,\"language\":\"en-us\",\"authMethods\":[\"local\"],\"authFormData\":{},\"serverVersion\":\"2.30.0\"}");
    if (!strcmp(path, "/login") && !strcmp(method, "POST"))
    {
        av.logins++;
        char u[64] = "", pw[96] = "";
        av_json_str(body, "username", u, sizeof u);
        av_json_str(body, "password", pw, sizeof pw);
        if (strcmp(u, "tai") || strcmp(pw, AV_PW)) { *status = 401; return (size_t)snprintf(o, cap, "Invalid username or password"); }
        snprintf(av.access, sizeof av.access, "access-%d-abcdefghijklmnopqrstuvwxyz.0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ", ++av.access_n);
        av.expire = false;
        return (size_t)snprintf(o, cap, "{\"user\":{\"id\":\"u1\",\"username\":\"tai\",\"type\":\"admin\",\"token\":\"legacy-token\",\"accessToken\":\"%s\","
                                "\"refreshToken\":\"refresh-1\",\"mediaProgress\":[]},\"userDefaultLibraryId\":\"lib1\",\"serverSettings\":{\"version\":\"2.30.0\"},\"Source\":\"docker\"}", av.access);
    }
    if (!strcmp(path, "/auth/refresh"))
    {
        av.refreshes++;
        snprintf(av.access, sizeof av.access, "access-%d-refreshed.0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ", ++av.access_n);
        av.expire = false;
        return (size_t)snprintf(o, cap, "{\"user\":{\"id\":\"u1\",\"username\":\"tai\",\"accessToken\":\"%s\",\"refreshToken\":\"refresh-2\"}}", av.access);
    }
    /* the API's own: a Bearer token (or ?token= for files) */
    char tok[300] = "";
    const char *auth = av.last_auth;
    if (!strncmp(auth, "Bearer ", 7)) snprintf(tok, sizeof tok, "%s", auth + 7);
    else if (sb_q(q, "token", v, sizeof v)) snprintf(tok, sizeof tok, "%s", v);
    bool ok = (!av.expire && av.access[0] && !strcmp(tok, av.access)) || !strcmp(tok, AV_API);
    if (!ok) { av.bad_auth++; *status = 401; return (size_t)snprintf(o, cap, "Unauthorized"); }
    if (!strcmp(path, "/api/authorize"))
        return (size_t)snprintf(o, cap, "{\"user\":{\"id\":\"u1\",\"username\":\"tai\"},\"userDefaultLibraryId\":\"lib1\",\"serverSettings\":{\"version\":\"2.30.0\"}}");
    if (!strcmp(path, "/api/libraries"))
    {
        *chunk = true;                    /* this one comes in chunks */
        return (size_t)snprintf(o, cap, "{\"libraries\":[{\"id\":\"lib2\",\"name\":\"Podcasts\",\"mediaType\":\"podcast\",\"icon\":\"podcast\"},"
                                "{\"id\":\"lib1\",\"name\":\"Audiobooks\",\"mediaType\":\"book\",\"icon\":\"audiobookshelf\"}]}");
    }
    if (!strcmp(path, "/api/libraries/lib1/items"))
    {
        int page = sb_q(q, "page", v, sizeof v) ? atoi(v) : 0, lim = sb_q(q, "limit", v, sizeof v) ? atoi(v) : 40;
        char f[200] = "";
        sb_q(q, "filter", f, sizeof f);
        snprintf(av.last_filter, sizeof av.last_filter, "%s", f);
        int from = 1, to = 45;
        if (f[0]) to = 2;                 /* Frank Herbert's, or the Dune Saga */
        int total = to - from + 1;
        k = (size_t)snprintf(o, cap, "{\"results\":[");
        int first = from + page * lim;
        for (int n = first; n <= to && n < first + lim; n++) { if (n > first) o[k++] = ','; k += av_mini(o + k, cap - k, n, false); }
        k += (size_t)snprintf(o + k, cap - k, "],\"total\":%d,\"limit\":%d,\"page\":%d,\"mediaType\":\"book\",\"minified\":true}", total, lim, page);
        return k;
    }
    if (!strcmp(path, "/api/libraries/lib2/items"))
        return (size_t)snprintf(o, cap, "{\"results\":[{\"id\":\"p1\",\"libraryId\":\"lib2\",\"mediaType\":\"podcast\",\"media\":{\"metadata\":{\"title\":\"Night Shift Radio\","
                                "\"author\":\"KNSR\"},\"numEpisodes\":2,\"size\":100000}}],\"total\":1,\"limit\":40,\"page\":0}");
    if (!strcmp(path, "/api/libraries/lib1/authors"))
        return (size_t)snprintf(o, cap, "{\"authors\":[{\"id\":\"a1\",\"name\":\"Frank Herbert\",\"numBooks\":2},{\"id\":\"a3\",\"name\":\"Author 3\",\"numBooks\":1}]}");
    if (!strcmp(path, "/api/libraries/lib1/series"))
        return (size_t)snprintf(o, cap, "{\"results\":[{\"id\":\"s1\",\"name\":\"Dune Saga\",\"books\":[{\"id\":\"b1\",\"media\":{}},{\"id\":\"b2\"}]}],\"total\":1}");
    if (!strcmp(path, "/api/me"))
    {
        av.me_gets++;
        k = (size_t)snprintf(o, cap, "{\"id\":\"u1\",\"username\":\"tai\",\"mediaProgress\":[");
        for (int i = 0; i < av.nprog; i++)
        {
            struct av_prog *p = &av.prog[i];
            if (!p->have) continue;
            char item[48] = "", ep[48] = "";
            sscanf(p->key, "%47[^/]/%47s", item, ep);
            k += (size_t)snprintf(o + k, cap - k, "%s{\"id\":\"mp%d\",\"libraryItemId\":\"%s\",\"episodeId\":%s%s%s,\"duration\":%.1f,\"progress\":%.4f,"
                                  "\"currentTime\":%.3f,\"isFinished\":%s,\"lastUpdate\":%lld}", k > 60 ? "," : "", i, item, ep[0] ? "\"" : "", ep[0] ? ep : "null",
                                  ep[0] ? "\"" : "", p->dur, p->dur > 0 ? p->cur / p->dur : 0, p->cur, p->fin ? "true" : "false", p->upd);
        }
        k += (size_t)snprintf(o + k, cap - k, "]}");
        return k;
    }
    if (!strcmp(path, "/api/me/items-in-progress"))
    {
        k = (size_t)snprintf(o, cap, "{\"libraryItems\":[");
        struct av_prog *p = av_prog("b1", false);
        if (p && p->have && !p->fin) k += av_mini(o + k, cap - k, 1, true);
        k += (size_t)snprintf(o + k, cap - k, "]}");
        return k;
    }
    if (!strncmp(path, "/api/me/progress/", 17))
    {
        const char *key = path + 17;
        if (!strcmp(method, "PATCH"))
        {
            av.patches++;
            snprintf(av.last_patch, sizeof av.last_patch, "%s", body);
            snprintf(av.last_patch_path, sizeof av.last_patch_path, "%s", path);
            struct av_prog *p = av_prog(key, true);
            p->cur = av_json_num(body, "currentTime");
            p->dur = av_json_num(body, "duration");
            p->fin = strstr(body, "\"isFinished\":true") != NULL;
            p->have = true;
            p->upd = av_now_ms();
            return (size_t)snprintf(o, cap, "OK");
        }
        struct av_prog *p = av_prog(key, false);
        if (!p || !p->have) { *status = 404; return (size_t)snprintf(o, cap, "Not Found"); }
        return (size_t)snprintf(o, cap, "{\"id\":\"mp\",\"libraryItemId\":\"b1\",\"duration\":%.1f,\"progress\":%.4f,\"currentTime\":%.3f,\"isFinished\":%s,"
                                "\"hideFromContinueListening\":false,\"lastUpdate\":%lld,\"startedAt\":1700000000000}", p->dur, p->dur > 0 ? p->cur / p->dur : 0, p->cur,
                                p->fin ? "true" : "false", p->upd);
    }
    if (!strncmp(path, "/api/me/item/listening-sessions/", 32))
    {
        struct av_prog *p = av_prog(path + 32, false);
        if (!av.device[0] || !p) return (size_t)snprintf(o, cap, "{\"total\":0,\"numPages\":0,\"itemsPerPage\":1,\"sessions\":[]}");
        return (size_t)snprintf(o, cap, "{\"total\":3,\"numPages\":3,\"itemsPerPage\":1,\"sessions\":[{\"id\":\"ls1\",\"libraryItemId\":\"b1\",\"updatedAt\":%lld,"
                                "\"deviceInfo\":{\"id\":\"d1\",\"deviceName\":\"%s\",\"clientName\":\"Abs Android\",\"osName\":\"Android\"}}]}", p->upd - 4000, av.device);
    }
    if (!strcmp(path, "/api/items/b1") || !strcmp(path, "/api/items/b2"))
    {
        bool one = path[12] == '1';
        struct av_prog *p = one && strstr(q, "include=progress") ? av_prog("b1", false) : NULL;
        char um[300] = "";
        if (p && p->have)
            snprintf(um, sizeof um, ",\"userMediaProgress\":{\"id\":\"mp\",\"libraryItemId\":\"b1\",\"duration\":%.1f,\"progress\":%.4f,\"currentTime\":%.3f,\"isFinished\":%s,\"lastUpdate\":%lld}",
                     p->dur, p->dur > 0 ? p->cur / p->dur : 0, p->cur, p->fin ? "true" : "false", p->upd);
        if (!one)
            return (size_t)snprintf(o, cap, "{\"id\":\"b2\",\"libraryId\":\"lib1\",\"mediaType\":\"book\",\"media\":{\"metadata\":{\"title\":\"Children of Dune\",\"authorName\":\"Frank Herbert\"},"
                                    "\"duration\":61000,\"size\":%lld,\"chapters\":[],\"tracks\":[{\"index\":1,\"startOffset\":0,\"duration\":61000,\"contentUrl\":\"/api/items/b2/file/301\","
                                    "\"metadata\":{\"filename\":\"Children.m4b\",\"ext\":\".m4b\",\"size\":%lld}}]},\"size\":%lld}", av_size(2), av_size(2), av_size(2));
        return (size_t)snprintf(o, cap,
            "{\"id\":\"b1\",\"ino\":\"9\",\"libraryId\":\"lib1\",\"mediaType\":\"book\",\"libraryFiles\":[{\"ino\":\"101\",\"metadata\":{\"filename\":\"x\",\"size\":1}}],"
            "\"media\":{\"id\":\"m1\",\"metadata\":{\"title\":\"Dune\",\"subtitle\":null,\"authors\":[{\"id\":\"a1\",\"name\":\"Frank Herbert\"}],\"narrators\":[\"Scott Brick\"],"
            "\"series\":[{\"id\":\"s1\",\"name\":\"Dune Saga\",\"sequence\":\"1\"}],\"genres\":[\"Sci-Fi\"],\"description\":\"A desert planet \\u2014 and the spice.\","
            "\"authorName\":\"Frank Herbert\",\"narratorName\":\"Scott Brick\"},\"coverPath\":\"/x.jpg\",\"duration\":7200.5,\"size\":%lld,"
            "\"chapters\":[{\"id\":0,\"start\":0,\"end\":1800,\"title\":\"Book One: Dune\"},{\"id\":1,\"start\":1800,\"end\":4000,\"title\":\"Book Two: Muad'Dib\"},"
            "{\"id\":2,\"start\":4000,\"end\":7200.5,\"title\":\"Book Three: The Prophet\"}],"
            "\"audioFiles\":[{\"index\":1,\"ino\":\"101\",\"metadata\":{\"filename\":\"Dune Part One.mp3\",\"ext\":\".mp3\",\"size\":%lld}},"
            "{\"index\":2,\"ino\":\"102\",\"metadata\":{\"filename\":\"Dune Part Two.mp3\",\"ext\":\".mp3\",\"size\":%lld}}],"
            "\"tracks\":[{\"index\":1,\"startOffset\":0,\"duration\":3600,\"title\":\"Dune Part One.mp3\",\"contentUrl\":\"/api/items/b1/file/101\",\"mimeType\":\"audio/mpeg\","
            "\"metadata\":{\"filename\":\"Dune Part One.mp3\",\"ext\":\".mp3\",\"size\":%lld}},{\"index\":2,\"startOffset\":3600,\"duration\":3600.5,\"title\":\"Dune Part Two.mp3\","
            "\"contentUrl\":\"/api/items/b1/file/102\",\"mimeType\":\"audio/mpeg\",\"metadata\":{\"filename\":\"Dune Part Two.mp3\",\"ext\":\".mp3\",\"size\":%lld}}]},"
            "\"size\":%lld%s}", AV_F1 + AV_F2, AV_F1, AV_F2, AV_F1, AV_F2, AV_F1 + AV_F2, um);
    }
    if (!strcmp(path, "/api/items/p1"))
        return (size_t)snprintf(o, cap,
            "{\"id\":\"p1\",\"libraryId\":\"lib2\",\"mediaType\":\"podcast\",\"media\":{\"metadata\":{\"title\":\"Night Shift Radio\",\"author\":\"KNSR\",\"description\":\"Late shows.\"},"
            "\"episodes\":[{\"libraryItemId\":\"p1\",\"id\":\"e1\",\"index\":1,\"title\":\"Episode One\",\"duration\":1500,\"size\":%lld,\"publishedAt\":1760000000000,"
            "\"audioFile\":{\"ino\":\"201\",\"duration\":1500,\"metadata\":{\"filename\":\"ep1.mp3\",\"ext\":\".mp3\",\"size\":%lld}},"
            "\"audioTrack\":{\"index\":1,\"contentUrl\":\"/api/items/p1/file/201\",\"duration\":1500}},"
            "{\"libraryItemId\":\"p1\",\"id\":\"e2\",\"index\":2,\"title\":\"Episode Two\",\"duration\":1600,\"size\":60000,"
            "\"audioFile\":{\"ino\":\"202\",\"duration\":1600,\"metadata\":{\"filename\":\"ep2.mp3\",\"ext\":\".mp3\",\"size\":60000}}}]},\"size\":110000}", AV_E1, AV_E1);
    *status = 404;
    return (size_t)snprintf(o, cap, "Not Found");
}
static void *av_serve(void *unused)
{
    (void)unused;
    static char req[8192];
    size_t cap = 2 * 1024 * 1024;
    char *body = malloc(cap);
    for (;;)
    {
        int c = accept(av.lfd, NULL, NULL);
        if (c < 0 && errno == EINTR) continue;
        if (c < 0) break;
        size_t n = 0;
        char *hend = NULL;
        while (n + 1 < sizeof req)
        {
            ssize_t r = recv(c, req + n, sizeof req - 1 - n, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n += (size_t)r;
            req[n] = '\0';
            if (!hend) hend = strstr(req, "\r\n\r\n");
            if (hend)
            {
                const char *cl = strcasestr(req, "\r\nContent-Length:");
                size_t want = cl && cl < hend ? (size_t)atol(cl + 17) : 0;
                if (n >= (size_t)(hend + 4 - req) + want) break;
            }
        }
        req[n] = '\0';
        char method[16] = "", path[1600] = "";
        sscanf(req, "%15s %1599s", method, path);
        pthread_mutex_lock(&av.mu);
        bool slow = av.slow;
        pthread_mutex_unlock(&av.mu);
        if (slow) usleep(2500 * 1000);
        pthread_mutex_lock(&av.mu);
        av.reqs++;
        const char *a = strcasestr(req, "\r\nAuthorization: ");
        av.last_auth[0] = '\0';
        if (a) snprintf(av.last_auth, sizeof av.last_auth, "%.*s", (int)strcspn(a + 17, "\r\n"), a + 17);
        const char *ua = strcasestr(req, "\r\nUser-Agent: ");
        if (ua) snprintf(av.last_ua, sizeof av.last_ua, "%.*s", (int)strcspn(ua + 14, "\r\n"), ua + 14);
        const char *rb = hend ? hend + 4 : "";
        char *qm = strchr(path, '?');
        const char *q = qm ? qm + 1 : "";
        if (qm) *qm = '\0';
        snprintf(av.last_path, sizeof av.last_path, "%s", path);
        int status = 200;
        bool chunk = false;
        size_t len = 0;
        char head[600], date[64];
        time_t now = time(NULL) + (time_t)av.skew;
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        const char *ctype = "application/json; charset=utf-8";
        head[0] = '\0';
        int ino = 0;
        char item[16] = "";
        if (av.html) { len = (size_t)snprintf(body, cap, "<!DOCTYPE html><html>A router</html>"); ctype = "text/html"; }
        else if (sscanf(path, "/api/items/%15[^/]/file/%d", item, &ino) == 2)
        {
            /* a file: the bytes say where they are, i * 7 % 251; a Range is honoured */
            char tok[300] = "";
            sb_q(q, "token", tok, sizeof tok);
            long long total = ino == 101 ? AV_F1 : ino == 102 ? AV_F2 : ino == 201 ? AV_E1 : 0, from = 0, to = total - 1;
            if (strcmp(tok, av.access) && strcmp(tok, AV_API)) { status = 401; len = (size_t)snprintf(body, cap, "Unauthorized"); }
            else if (!total) { status = 404; len = (size_t)snprintf(body, cap, "Not Found"); }
            else
            {
                av.file_gets++;
                const char *rg = strcasestr(req, "\nRange: bytes=");
                if (rg)
                {
                    sscanf(rg + 14, "%lld-%lld", &from, &to);
                    if (to >= total) to = total - 1;
                    if (from > av.range_from_max) av.range_from_max = (int)from;
                    status = from >= total ? 416 : 206;
                }
                if (status != 416)
                {
                    len = (size_t)(to - from + 1);
                    for (size_t i = 0; i < len; i++) body[i] = (char)((from + (long long)i) * 7 % 251);
                }
                if (status == 206) snprintf(head, sizeof head, "HTTP/1.1 206 Partial Content\r\nDate: %s\r\nContent-Range: bytes %lld-%lld/%lld\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                            date, from, from + (long long)len - 1, total, len);
                ctype = "audio/mpeg";
            }
        }
        else if (!strncmp(path, "/api/items/", 11) && strstr(path, "/cover") && !strcmp(strstr(path, "/cover"), "/cover"))
        {
            memcpy(body, h_png, h_png_n);
            len = h_png_n;
            ctype = "image/png";
        }
        else len = av_body(method, path, q, rb, body, cap, &status, &chunk);
        if (chunk)
        {
            /* chunked, seven bytes a chunk, with an extension on one */
            av.chunked_sent++;
            snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nDate: %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n", date, ctype);
            sb_write_all(c, head, strlen(head));
            for (size_t at = 0; at < len; at += 7)
            {
                size_t k = len - at < 7 ? len - at : 7;
                char sz[32];
                snprintf(sz, sizeof sz, at ? "%zx\r\n" : "%zx;ext=1\r\n", k);
                sb_write_all(c, sz, strlen(sz));
                sb_write_all(c, body + at, k);
                sb_write_all(c, "\r\n", 2);
            }
            sb_write_all(c, "0\r\n\r\n", 5);
        }
        else
        {
            if (!head[0]) snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nDate: %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                   status, status == 200 ? "OK" : "Error", date, ctype, len);
            sb_write_all(c, head, strlen(head));
            sb_write_all(c, body, len);
        }
        pthread_mutex_unlock(&av.mu);
        close(c);
    }
    free(body);
    return NULL;
}
static void av_start(void)
{
    h_make_png();                         /* the covers it sends */
    sb_start();                           /* batch_sub's: its port answers, but it is not Audiobookshelf */
    if (av.up) return;
    av.lfd = sb_listen(&av.port);
    assert(!listen(av.lfd, 16));
    assert(!pthread_create(&av.thr, NULL, av_serve, NULL));
    av.up = true;
}

/* ---- helpers */
static char av_url_s[64];
static unsigned av_seq0;
static bool av_test_came(void) { return abs_st.test_seq != av_seq0; }
static bool av_idle(void) { struct sl_abs_status s; sl_abs_status(&s); return !s.job && !s.jobs && !s.dl_item[0] && !s.queued && !s.pending; }
static bool av_idle_nopend(void) { struct sl_abs_status s; sl_abs_status(&s); return !s.job && !s.jobs && !s.dl_item[0] && !s.queued; }
static struct abs_view *av_view;
static bool av_answered(void) { return av_view && !av_view->waiting && !av_view->waiting2; }
static void av_wait(bool (*done)(void), int ms)
{
    uint64_t until = h_ms() + (uint64_t)ms;
    while (h_ms() < until && !done()) { abs_poll(); frame(30); usleep(6000); }
    abs_poll();
    frame(40);
}
static int av_patches(void) { pthread_mutex_lock(&av.mu); int n = av.patches; pthread_mutex_unlock(&av.mu); return n; }
static int av_patch0;
static bool av_patched(void) { return av_patches() > av_patch0 && av_idle(); }
static void av_open_wait(void) { av_view = abs_top_view(); av_wait(av_answered, 8000); for (int i = 0; i < 6; i++) { abs_poll(); frame(60); } }
static struct mp3entry av_track0;
static int av_status0, av_books_n0;
static char av_folder0[sizeof global_settings.audiobook_folder];
static struct ab_book av_book0;
static const char *av_str0[AB_NUM_STR];
static struct ab_progress av_prog0;
static bool av_prog_on0, av_conn0, av_saved;
static void av_setup(void)
{
    av_start();
    if (!av_saved)
    {
        av_saved = true;
        av_track0 = test_track; av_status0 = test_status; av_books_n0 = fake_books_n;
        memcpy(av_folder0, global_settings.audiobook_folder, sizeof av_folder0);
        av_book0 = fake_books[0]; memcpy(av_str0, fake_book_str[0], sizeof av_str0); av_prog0 = fake_prog[0]; av_prog_on0 = fake_prog_on[0];
        av_conn0 = wf_st.connected;
    }
    strlcpy(global_settings.audiobook_folder, AV_FOLDER, sizeof global_settings.audiobook_folder);
    sl_abs_idle_ms = 1500;
    sl_abs_retry_ms = 60000;
    if (system("mkdir -p " H_ROOT "/Card")) {}
    sl_net_req_fake = NULL;               /* our own client, to the server above */
    sl_net_dl_fake = sb_dl_fake;
    snprintf(av_url_s, sizeof av_url_s, "127.0.0.1:%d", av.port);
    pthread_mutex_lock(&sbv.mu); sbv.stall = false; sbv.slice = 30000; pthread_mutex_unlock(&sbv.mu);
    pthread_mutex_lock(&av.mu); av.slow = av.expire = av.html = false; av.skew = 0; av.device[0] = '\0'; pthread_mutex_unlock(&av.mu);
    wf_st.connected = true; sl_abs_online(true);
    abs_poll();
    /* the book, as our audiobooks will see it once it is on the card */
    fake_books_n = 1;
    memset(&fake_books[0], 0, sizeof fake_books[0]);
    fake_books[0].id = 777;
    fake_books[0].nfiles = 2;
    fake_books[0].length = 600000 + 1800000;
    memset(fake_book_str[0], 0, sizeof fake_book_str[0]);
    fake_book_str[0][AB_S_PATH] = AV_DIR;
    fake_book_str[0][AB_S_TITLE] = "Dune";
    fake_book_str[0][AB_S_AUTHOR] = "Frank Herbert";
    memset(&fake_prog[0], 0, sizeof fake_prog[0]);
    fake_prog_on[0] = false;
}
static void av_restore(void)
{
    if (!av_saved) return;
    av_saved = false;
    test_track = av_track0; test_status = av_status0; fake_books_n = av_books_n0;
    memcpy(global_settings.audiobook_folder, av_folder0, sizeof av_folder0);
    fake_books[0] = av_book0; memcpy(fake_book_str[0], av_str0, sizeof av_str0); fake_prog[0] = av_prog0; fake_prog_on[0] = av_prog_on0;
    wf_st.connected = av_conn0; sl_abs_online(av_conn0);
    sl_net_dl_fake = NULL;
    memset(&abs_at, 0, sizeof abs_at);
    abs_note.text[0] = '\0';
    abs_every_s = 180;
}
static void av_reset(void)
{
    while (npages > 1) { page_pop(); frame(300); }
    if (acts_open_now) { acts_close(); frame(300); }
    sl_abs_test_forget();
    assert(!system("rm -rf " H_ROOT SL_ABS_STATE " '" H_ROOT AV_FOLDER "'"));
    sl_abs_test_forget();
    memset(&abs_st, 0, sizeof abs_st);
    sl_abs_status(&abs_st);
    memset(&abs_at, 0, sizeof abs_at);
    abs_note.text[0] = '\0';
    pthread_mutex_lock(&av.mu);
    av.nprog = 0; av.patches = av.logins = av.refreshes = av.file_gets = av.range_from_max = av.bad_auth = 0;
    av.access[0] = '\0';
    pthread_mutex_unlock(&av.mu);
}
static void av_signin(const char *user, const char *secret)
{
    while (npages > 1) { page_pop(); frame(300); }
    abs_edit_page(); frame(300);
    lv_textarea_set_text(abs_url_ta, av_url_s);
    lv_textarea_set_text(abs_user_ta, user);
    lv_textarea_set_text(abs_pass_ta, secret);
    av_seq0 = abs_st.test_seq;
    abs_save_cb(NULL);
    av_wait(av_test_came, 8000);
}
static void av_dump(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class)) printf("  [%s]\n", lv_label_get_text(o));
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) av_dump(lv_obj_get_child(o, i));
}
static lv_obj_t *av_clickable(lv_obj_t *o) { while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) o = lv_obj_get_parent(o); return o; }
static void av_tap(const char *label)
{
    lv_obj_t *o = av_clickable(find_label(pages[npages - 1], label));
    if (!o) { printf("abs: no \"%s\" on the page\n", label); av_dump(pages[npages - 1]); }
    assert(o);
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    frame(200);
}
/* the book downloaded, read by our audiobooks */
static bool av_whole(void) { struct sl_abs_book k; return sl_abs_book("b1", "", &k) && k.whole && av_idle(); }
static void av_download_b1(void)
{
    abs_item_page("b1", "Dune"); av_open_wait();
    sl_abs_download("b1", "");            /* the pill's own call: its words differ by look */
    av_wait(av_whole, 15000);
    for (int i = 0; i < 4; i++) { abs_poll(); frame(60); }
}
/* the book playing, at pos ms */
static void av_playing(uint32_t pos, bool paused)
{
    test_status = AUDIO_STATUS_PLAY | (paused ? AUDIO_STATUS_PAUSE : 0);
    strlcpy(test_track.path, AV_DIR "/01 Dune Part One.mp3", sizeof test_track.path);
    fake_prog_on[0] = true;
    fake_prog[0].id = 777;
    fake_prog[0].position = pos;
    fake_prog[0].file = pos >= 600000 ? 1 : 0;
    abs_tick();
}

static void ab_tests(void)
{
    av_setup();
    av_reset();
    /* ---- no server: the way in says so */
    {
        abs_page(); frame(300);
        assert(find_label(pages[npages - 1], "No Audiobookshelf yet."));
        page_pop(); frame(300);
        books_page(); frame(300);   /* no books here (fake_books_n 1, but the page lists the one): the row is there */
        assert(find_label(pages[npages - 1], "Connect Audiobookshelf"));
        page_pop(); frame(300);
    }
    printf("PASS: abs: with no server, Audiobookshelf says how to connect, from Audiobooks too.\n");

    /* ---- signing in: every way it can go wrong says why */
    {
        abs_edit_page(); frame(300);
        lv_textarea_set_text(abs_url_ta, "ftp://nope"); lv_textarea_set_text(abs_user_ta, "tai"); lv_textarea_set_text(abs_pass_ta, "x");
        abs_test_cb(NULL); frame(100);
        assert(has_text(pages[npages - 1], "That doesn't look like an address"));
        lv_textarea_set_text(abs_url_ta, av_url_s); lv_textarea_set_text(abs_pass_ta, "wrong");
        av_seq0 = abs_st.test_seq; abs_test_cb(NULL); av_wait(av_test_came, 8000);
        assert(abs_st.test_err == SL_ABS_E_AUTH && has_text(pages[npages - 1], "Wrong user name or password"));
        char other[64];
        snprintf(other, sizeof other, "127.0.0.1:%d", sbv.down_port);   /* nothing answers */
        lv_textarea_set_text(abs_url_ta, other);
        av_seq0 = abs_st.test_seq; abs_test_cb(NULL); av_wait(av_test_came, 30000);
        assert(abs_st.test_err == SL_ABS_E_NET);
        snprintf(other, sizeof other, "127.0.0.1:%d", sbv.port);        /* a Subsonic server: not ours */
        lv_textarea_set_text(abs_url_ta, other);
        av_seq0 = abs_st.test_seq; abs_test_cb(NULL); av_wait(av_test_came, 8000);
        assert(abs_st.test_err == SL_ABS_E_NOTABS && has_text(pages[npages - 1], "not Audiobookshelf"));
        assert(!sl_abs_server(NULL));     /* testing saves nothing */
        page_pop(); frame(300);
    }
    printf("PASS: abs: a bad address, a wrong password, no answer and a server that is not Audiobookshelf each say why; a test saves nothing.\n");
    {
        av_signin("tai", AV_PW);
        struct sl_abs_server s;
        assert(abs_st.test_saved && sl_abs_server(&s) && !s.api_token && !strcmp(s.user, "tai") && !strcmp(s.version, "2.30.0"));
        struct stat st;
        assert(h_file(SL_ABS_STATE "/server.tsv", &st) && (st.st_mode & 0777) == 0600);
        FILE *f = fopen(H_ROOT SL_ABS_STATE "/server.tsv", "r");
        char buf[4096] = "";
        size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = '\0'; fclose(f);
        assert(!strstr(buf, "pw &") && !strstr(buf, "access-") && strstr(buf, "127.0.0.1"));   /* never as it is */
        assert(av.logins >= 1 && strstr(av.last_ua, "GabagoolOS"));
        /* an API token where the password goes, with no user name */
        abs_edit_page(); frame(300);
        lv_textarea_set_text(abs_user_ta, ""); lv_textarea_set_text(abs_pass_ta, AV_API);
        av_seq0 = abs_st.test_seq; abs_save_cb(NULL); av_wait(av_test_came, 8000);
        assert(!abs_st.test_err && sl_abs_server(&s) && s.api_token && !strcmp(s.user, "tai"));
        assert(!strcmp(av.last_auth, "Bearer " AV_API));
        while (npages > 1) { page_pop(); frame(300); }
        av_signin("tai", AV_PW);          /* back to the password, for the refresh below */
        assert(sl_abs_server(&s) && !s.api_token);
    }
    printf("PASS: abs: signs in with a password (POST /login, its token kept 0600, the password never in clear) or an API token (POST /api/authorize).\n");

    /* ---- the home: continue listening (the server's), the libraries, books first */
    {
        pthread_mutex_lock(&av.mu);
        struct av_prog *p = av_prog("b1", true);
        p->cur = 600; p->dur = 7200.5; p->have = true; p->upd = av_now_ms() - 3600000;
        pthread_mutex_unlock(&av.mu);
        abs_page(); av_open_wait();
        assert(!av_view->l.err && av_view->l.n == 1 && find_label(pages[npages - 1], "Dune"));
        assert(has_text(pages[npages - 1], "Frank Herbert  \xc2\xb7  8% \xc2\xb7 1 h 50 min left"));
        assert(av_view->l2.n == 2 && !strcmp(sl_abs_s(&av_view->l2, av_view->l2.it[0].title), "Audiobooks") && av_view->l2.it[1].podcast);
        assert(av.chunked_sent >= 1);     /* the libraries came chunked */
        assert(!strncmp(av.last_auth, "Bearer access-", 14));
    }
    printf("PASS: abs: the home shows the server's Continue listening with how far, and the libraries, books before podcasts (a chunked answer).\n");
    /* ---- a library: titles a page at a time, authors, series */
    {
        av_tap("Audiobooks"); av_open_wait();
        assert(av_view->what == SL_ABS_Q_ITEMS && av_view->l.n == 40 && av_view->l.total == 45 && find_label(pages[npages - 1], "Children of Dune"));
        assert(find_label(pages[npages - 1], "More  " LV_SYMBOL_RIGHT));
        av_tap("More  " LV_SYMBOL_RIGHT); av_open_wait();
        assert(av_view->page_no == 1 && av_view->l.n == 5 && find_label(pages[npages - 1], "Book 45"));
        abs_tab_pick(1); av_open_wait();
        assert(av_view->what == SL_ABS_Q_AUTHORS && find_label(pages[npages - 1], "Frank Herbert") && has_text(pages[npages - 1], "2 books"));
        av_tap("Frank Herbert"); av_open_wait();
        assert(av_view->l.n == 2 && !strcmp(av.last_filter, "authors.YTE="));
        page_pop(); frame(300);
        abs_tab_pick(2); av_open_wait();
        assert(av_view->what == SL_ABS_Q_SERIES && find_label(pages[npages - 1], "Dune Saga") && has_text(pages[npages - 1], "2 books"));
        av_tap("Dune Saga"); av_open_wait();
        assert(av_view->l.n == 2 && !strcmp(av.last_filter, "series.czE="));
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: abs: a library pages its titles (40 of 45, then 5), and lists authors and series that filter it.\n");
    /* ---- a book's page: chapters, its length, how far */
    {
        abs_item_page("b1", "Dune"); av_open_wait();
        assert(!av_view->l.err && find_label(pages[npages - 1], "3 CHAPTERS") && find_label(pages[npages - 1], "Book Two: Muad'Dib"));
        assert(has_text(pages[npages - 1], "Read by Scott Brick") && has_text(pages[npages - 1], "A desert planet \xe2\x80\x94 and the spice."));
        assert(has_text(pages[npages - 1], "2 h") && find_label(pages[npages - 1], LV_SYMBOL_DOWNLOAD "  Download  \xc2\xb7  0.2 MB"));
        int files = 0;
        for (int i = 0; i < av_view->l.n; i++) files += av_view->l.it[i].kind == SL_ABS_FILE;
        assert(files == 2);               /* the tracks, not the audio files twice */
        av_tap("Book Two: Muad'Dib");      /* not on the card yet */
        page_pop(); frame(300);
        abs_item_page("nope", "Gone"); av_open_wait();
        assert(av_view->l.err == SL_ABS_E_NOTFOUND && find_label(pages[npages - 1], "Couldn't reach your books."));
        page_pop(); frame(300);
    }
    printf("PASS: abs: a book's page has its chapters, narrator, words and size; a missing one says so.\n");
    /* ---- the big one asks first */
    {
        abs_item_page("b2", "Children of Dune"); av_open_wait();
        av_tap(LV_SYMBOL_DOWNLOAD "  Download  \xc2\xb7  400 MB");
        assert(acts_open_now && has_text(acts, "400 MB to download") && find_label(acts, "Not now"));
        acts_close(); frame(300);
        struct sl_abs_book k;
        assert(!sl_abs_book("b2", "", &k));
        page_pop(); frame(300);
    }
    printf("PASS: abs: a book over 300 MB says its size and the card's room before it comes down.\n");

    /* ---- downloading: to Audiobooks/<author>/<title>, by Range, stopped by USB mode and carried on */
    {
        pthread_mutex_lock(&sbv.mu); sbv.slice = 20000; pthread_mutex_unlock(&sbv.mu);
        abs_item_page("b1", "Dune"); av_open_wait();
        av_tap(LV_SYMBOL_DOWNLOAD "  Download  \xc2\xb7  0.2 MB");
        struct sl_abs_status s;
        uint64_t t0 = h_ms();
        do { abs_poll(); frame(30); usleep(5000); sl_abs_status(&s); } while (h_ms() - t0 < 8000 && s.dl_got < 30000);
        assert(s.dl_item[0] && !strcmp(s.dl_item, "b1") && s.dl_files == 2);
        sl_abs_hold(true);                /* USB mode */
        sl_abs_status(&s);
        assert(!s.dl_item[0]);
        struct stat st;
        assert(h_file(AV_DIR "/01 Dune Part One.mp3.part", &st) && st.st_size > 0 && st.st_size < AV_F1);
        for (int i = 0; i < 10; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(!h_file(AV_DIR "/01 Dune Part One.mp3", &st));
        sl_abs_hold(false);
        av_wait(av_whole, 15000);
        assert(av_whole());
        assert(h_file_pattern(AV_DIR "/01 Dune Part One.mp3", AV_F1) && h_file_pattern(AV_DIR "/02 Dune Part Two.mp3", AV_F2));
        assert(av.range_from_max > 0);
        assert(h_file(AV_DIR "/metadata.json", &st) && h_file(AV_DIR "/cover.jpg", &st));
        FILE *f = fopen(H_ROOT AV_DIR "/metadata.json", "r");
        char m[2048] = "";
        size_t n = fread(m, 1, sizeof m - 1, f); m[n] = '\0'; fclose(f);
        assert(strstr(m, "\"title\": \"Dune\"") && strstr(m, "\"authors\": [\"Frank Herbert\"]") && strstr(m, "\"narrators\": [\"Scott Brick\"]"));
        assert(ab_scans >= 1);            /* our audiobooks read the folder again */
        /* and it starts where the server has it (600 s) */
        for (int i = 0; i < 100 && fake_prog[0].position != 600000; i++) { abs_poll(); frame(30); usleep(5000); }
        assert(fake_prog_on[0] && fake_prog[0].position == 600000 && fake_prog[0].file == 1 && fake_prog[0].elapsed == 0);
        for (int i = 0; i < 6; i++) { abs_poll(); frame(60); }
        assert(find_label(pages[npages - 1], LV_SYMBOL_PLAY "  Resume") && find_label(pages[npages - 1], "Remove download"));
        pthread_mutex_lock(&sbv.mu); sbv.slice = 30000; pthread_mutex_unlock(&sbv.mu);
    }
    printf("PASS: abs: a book comes down to Audiobooks/Frank Herbert/Dune (its words and cover beside), by Range, stops for USB mode and carries on.\n");
    /* ---- playing it: our audiobooks' own (30 s skips, rewind after a pause, its speed) */
    {
        ab_plays = -1;
        av_tap(LV_SYMBOL_PLAY "  Resume");
        assert(ab_plays == 0);            /* book 0, from where it is */
        if (np_open) { np_hide(0); frame(500); }
        ab_plays = -1;
        av_tap("Book Two: Muad'Dib");      /* 1800 s: the second file, 1200 s in */
        assert(ab_plays == 0 && ab_play_file == 1 && ab_play_ms == 1200000);
        if (np_open) { np_hide(0); frame(500); }
    }
    printf("PASS: abs: Play plays it as one of our audiobooks; a chapter plays from its start.\n");
    /* ---- the place goes to the server: on pause, at a chapter, every few minutes, at the end */
    {
        av_wait(av_idle, 8000);
        memset(&abs_at, 0, sizeof abs_at);
        av_patch0 = av_patches();
        av_playing(100000, false);
        av_playing(101000, false);
        assert(av_patches() == av_patch0 && abs_at.linked && abs_at.playing && abs_at.nch == 3);
        av_playing(102000, true);         /* paused */
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":102.000") && strstr(av.last_patch, "\"isFinished\":false"));
        assert(!strcmp(av.last_patch_path, "/api/me/progress/b1") && strstr(av.last_patch, "\"duration\":7200.500"));
        /* a file of its own (the second) */
        av_patch0 = av_patches();
        av_playing(1700000, false);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":1700.000"));
        /* a chapter in the file: Book Two starts at 1800 s */
        av_patch0 = av_patches();
        av_playing(1750000, false);
        assert(av_patches() == av_patch0);
        av_playing(1801000, false);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":1801.000"));
        /* every few minutes while it plays */
        av_patch0 = av_patches();
        av_playing(1802000, false);
        assert(av_patches() == av_patch0);
        abs_at.pushed_at = current_tick - (long)(abs_every_s + 1) * HZ;
        av_playing(1803000, false);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":1803.000"));
        /* the end */
        av_patch0 = av_patches();
        fake_prog[0].finished = 1;
        av_playing(2390000, false);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"isFinished\":true") && strstr(av.last_patch, "\"progress\":1.00000"));
        pthread_mutex_lock(&av.mu); struct av_prog *p = av_prog("b1", false); bool fin = p && p->fin; pthread_mutex_unlock(&av.mu);
        assert(fin);
        fake_prog[0].finished = 0;
        test_status = 0; abs_tick();
    }
    printf("PASS: abs: the place goes on pause, at a chapter, every few minutes while playing, and the end marks it finished.\n");
    /* ---- offline: kept on the card, told when Wi-Fi comes back */
    {
        av_wait(av_idle, 8000);
        wf_st.connected = false; sl_abs_online(false);
        memset(&abs_at, 0, sizeof abs_at);
        av_patch0 = av_patches();
        av_playing(200000, false);
        av_playing(205000, true);
        for (int i = 0; i < 20; i++) { abs_poll(); frame(30); usleep(5000); }
        struct sl_abs_status s;
        sl_abs_status(&s);
        struct stat st;
        assert(s.pending == 1 && av_patches() == av_patch0 && h_file(SL_ABS_STATE "/progress.tsv", &st));
        sl_abs_test_forget();             /* even after a restart */
        sl_abs_status(&s);
        wf_st.connected = true; sl_abs_online(true);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":205.000") && !h_file(SL_ABS_STATE "/progress.tsv", &st));
        test_status = 0; abs_tick();
    }
    printf("PASS: abs: offline, the place waits on the card (a restart too) and goes when Wi-Fi comes back.\n");
    /* ---- the server's newer place is taken, with where it came from */
    {
        av_wait(av_idle, 8000);
        pthread_mutex_lock(&av.mu);
        struct av_prog *p = av_prog("b1", true);
        p->cur = 4500; p->dur = 7200.5; p->fin = false; p->have = true; p->upd = av_now_ms() + 5000;
        snprintf(av.device, sizeof av.device, "Pixel 7");
        pthread_mutex_unlock(&av.mu);
        while (npages > 1) { page_pop(); frame(300); }
        abs_item_page("b1", "Dune"); av_open_wait();
        for (int i = 0; i < 20 && fake_prog[0].position != 4500000; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(fake_prog[0].position == 4500000 && fake_prog[0].file == 1 && fake_prog[0].elapsed == 3900000);
        frame(100);
        assert(find_label(pages[npages - 1], "Picked up where you left off on Pixel 7"));
        /* and with no device, the time */
        pthread_mutex_lock(&av.mu); p->cur = 4600; p->upd = av_now_ms() + 9000; av.device[0] = '\0'; pthread_mutex_unlock(&av.mu);
        page_pop(); frame(300);
        abs_item_page("b1", "Dune"); av_open_wait();
        for (int i = 0; i < 20 && fake_prog[0].position != 4600000; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(fake_prog[0].position == 4600000 && has_text(pages[npages - 1], "Picked up where you left off at "));
        /* the same place again is not news */
        uint32_t was = fake_prog[0].position;
        fake_prog[0].position = 1234;
        page_pop(); frame(300);
        abs_item_page("b1", "Dune"); av_open_wait();
        for (int i = 0; i < 10; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(fake_prog[0].position == 1234);
        fake_prog[0].position = was;
        /* ours newer than theirs (waiting offline): ours wins and goes */
        wf_st.connected = false; sl_abs_online(false);
        memset(&abs_at, 0, sizeof abs_at);
        av_playing(4700000, false); av_playing(4710000, true);
        pthread_mutex_lock(&av.mu); p->cur = 300; p->upd = av_now_ms() - 600000; pthread_mutex_unlock(&av.mu);   /* moved, but long before */
        av_patch0 = av_patches();
        wf_st.connected = true; sl_abs_online(true);
        av_wait(av_patched, 8000);
        assert(av_patches() == av_patch0 + 1 && strstr(av.last_patch, "\"currentTime\":4710.000") && fake_prog[0].position == 4710000);
        /* theirs newer than ours (ours waiting, older): theirs is taken, ours dropped */
        wf_st.connected = false; sl_abs_online(false);
        memset(&abs_at, 0, sizeof abs_at);
        av_playing(4800000, false); av_playing(4810000, true);
        pthread_mutex_lock(&av.mu); p->cur = 5000; p->upd = av_now_ms() + 60000; pthread_mutex_unlock(&av.mu);
        test_status = 0; abs_tick();
        av_patch0 = av_patches();
        wf_st.connected = true; sl_abs_online(true);
        av_wait(av_idle, 8000);
        for (int i = 0; i < 10; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(av_patches() == av_patch0 && fake_prog[0].position == 5000000);
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: abs: the server's newer place is taken at a book's page (from Pixel 7, or at its time), last write wins either way.\n");
    /* ---- Continue listening takes it too, and the token is refreshed when it runs out */
    {
        pthread_mutex_lock(&av.mu);
        struct av_prog *p = av_prog("b1", true);
        p->cur = 5100; p->upd = av_now_ms() + 120000;
        av.expire = true;                 /* the access token ran out */
        int r0 = av.refreshes;
        pthread_mutex_unlock(&av.mu);
        abs_page(); av_open_wait();
        for (int i = 0; i < 20 && fake_prog[0].position != 5100000; i++) { abs_poll(); frame(50); usleep(5000); }
        assert(fake_prog[0].position == 5100000 && !av_view->l.err);
        pthread_mutex_lock(&av.mu); assert(av.refreshes == r0 + 1); pthread_mutex_unlock(&av.mu);
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: abs: Continue listening takes the server's newer place too; a token that ran out is refreshed.\n");
    /* ---- offline: the books on the card still show and open */
    {
        wf_st.connected = false; sl_abs_online(false);
        abs_page(); av_open_wait();
        assert(av_view->l.n == 1 && find_label(pages[npages - 1], "Offline") && find_label(pages[npages - 1], "Dune"));
        av_tap("Dune"); av_open_wait();
        assert(av_view->l.offline && find_label(pages[npages - 1], "3 CHAPTERS") && find_label(pages[npages - 1], LV_SYMBOL_PLAY "  Resume"));
        while (npages > 1) { page_pop(); frame(300); }
        wf_st.connected = true; sl_abs_online(true);
    }
    printf("PASS: abs: offline, the home lists the books on the card and a book opens with its chapters and Play.\n");
    /* ---- a podcast's episode, a book of its own */
    {
        abs_page(); av_open_wait();
        av_tap("Podcasts"); av_open_wait();
        assert(av_view->l.n == 1 && find_label(pages[npages - 1], "Night Shift Radio"));
        av_tap("Night Shift Radio"); av_open_wait();
        assert(av_view->l.head.kind == SL_ABS_PODCAST && find_label(pages[npages - 1], "Episode One") && find_label(pages[npages - 1], "Episode Two"));
        av_tap("Episode One");
        struct sl_abs_book k;
        uint64_t t0 = h_ms();
        while (h_ms() - t0 < 10000 && !(sl_abs_book("p1", "e1", &k) && k.whole && av_idle())) { abs_poll(); frame(30); usleep(5000); }
        assert(sl_abs_book("p1", "e1", &k) && k.whole && h_file_pattern(AV_FOLDER "/Night Shift Radio/Episode One/ep1.mp3", AV_E1));
        sl_abs_remove("p1", "e1");
        av_wait(av_idle, 8000);
        struct stat st;
        assert(!sl_abs_book("p1", "e1", &k) && !h_file(AV_FOLDER "/Night Shift Radio", &st));
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: abs: a podcast lists its episodes; one comes down as a book of its own, and goes.\n");
    /* ---- memory: what the pages hold, and what stays after */
    {
        av_wait(av_idle, 8000);
        for (int i = 0; i < 10; i++) { abs_poll(); frame(100); }
        malloc_statistics_t m0, m1, m3;
        malloc_zone_statistics(NULL, &m0);
        abs_page(); av_open_wait();
        abs_item_page("b1", "Dune"); av_open_wait();
        abs_lib_page("lib1", "Audiobooks", false, 0, "", 0, false); av_open_wait();
        av_wait(av_idle, 8000);
        for (int i = 0; i < 6; i++) { abs_poll(); frame(60); }
        malloc_zone_statistics(NULL, &m1);
        while (npages > 1) { page_pop(); frame(300); }
        av_wait(av_idle, 8000);
        for (int i = 0; i < 10; i++) { abs_poll(); frame(100); }
        malloc_zone_statistics(NULL, &m3);
        long open = ((long)m1.size_in_use - (long)m0.size_in_use) / 1024, after = ((long)m3.size_in_use - (long)m0.size_in_use) / 1024;
        printf("abs: memory: the worker's state %u KB always; home, a book and 40 covers %ld KB; %ld KB left after they close\n",
               (unsigned)(sl_abs_test_static_bytes() / 1024), open, after);
#if !__has_feature(address_sanitizer)
        assert(after < 48);
#endif
    }
    printf("PASS: abs: memory back to where it was once the pages close.\n");
    /* ---- removing it: off the card, the place stays on the server */
    {
        abs_item_page("b1", "Dune"); av_open_wait();
        av_tap("Remove download");
        av_wait(av_idle, 8000);
        for (int i = 0; i < 6; i++) { abs_poll(); frame(60); }
        struct stat st;
        struct sl_abs_book k;
        assert(!sl_abs_book("b1", "", &k) && !h_file(AV_DIR, &st) && !h_file(AV_FOLDER "/Frank Herbert", &st));
        assert(find_label(pages[npages - 1], LV_SYMBOL_DOWNLOAD "  Download  \xc2\xb7  0.2 MB"));
        while (npages > 1) { page_pop(); frame(300); }
    }
    printf("PASS: abs: Remove download takes the book off the card.\n");
    {   /* lists on the card that point out of the audiobook folder (a server's id or address with a
         * line break once wrote such lines): read back, they are not taken, and nothing comes down */
        char p[600];
        snprintf(p, sizeof p, H_ROOT "%s", SL_ABS_STATE "/books.tsv");
        FILE *f = fopen(p, "w"); assert(f);
        fputs("ABB1\nevil\t\t/Audiobooks/../.rockbox\tEvil\tX\t1000\t10\t1\t1\t1\n"
              "evil2\t\t.rockbox/sloppy\tEvil 2\tX\t1000\t10\t1\t1\t1\n", f); fclose(f);
        snprintf(p, sizeof p, H_ROOT "%s", SL_ABS_STATE "/queue.tsv");
        f = fopen(p, "w"); assert(f);
        fputs("b1\t\t0\t1\t/api/items/b1/file/101\t/Audiobooks/../.rockbox/rockbox.r1\t10\tx\n", f); fclose(f);
        sl_abs_test_forget();
        struct sl_abs_book k[4];
        assert(sl_abs_books(k, 4) == 0);   /* read now: neither is taken */
        sl_abs_online(false); sl_abs_online(true);   /* the queue read as Wi-Fi comes back */
        av_wait(av_idle, 8000);
        struct sl_abs_status s;
        sl_abs_status(&s);
        struct stat st;
        assert(s.queued == 0 && !s.dl_files && !h_file("/.rockbox/rockbox.r1.part", &st));
        remove(p);
        snprintf(p, sizeof p, H_ROOT "%s", SL_ABS_STATE "/books.tsv"); remove(p);
        sl_abs_test_forget();
    }
    printf("PASS: abs: lists on the card pointing out of the audiobook folder or into .rockbox are not taken.\n");
    /* ---- Settings > Music servers has it beside the Subsonic servers; removing forgets the token */
    {
        sub_servers_page(); frame(300);
        assert(find_label(pages[npages - 1], "127.0.0.1") && find_label(pages[npages - 1], "Add a server"));
        page_pop(); frame(300);
        abs_edit_page(); frame(300);
        abs_forget_cb(NULL); frame(300);
        struct stat st;
        assert(!sl_abs_server(NULL) && !h_file(SL_ABS_STATE "/server.tsv", &st));
    }
    printf("PASS: abs: Settings > Music servers shows it; removing it forgets the token and password.\n");
    while (npages > 1) { page_pop(); frame(300); }
    av_wait(av_idle_nopend, 4000);
    av_reset();
    av_restore();
}

/* ---- the screens, for the audit and the shots */
static bool av_au_dirty, av_au_made;
static void av_au_reset(void)
{
    if (!av_au_dirty) return;
    av_au_dirty = false;
    if (acts_open_now) { acts_close(); frame(300); }
    pthread_mutex_lock(&sbv.mu); sbv.stall = false; pthread_mutex_unlock(&sbv.mu);
    pthread_mutex_lock(&av.mu); av.slow = false; pthread_mutex_unlock(&av.mu);
    for (int i = 0; i < 100 && !av_idle_nopend(); i++) { abs_poll(); frame(20); usleep(5000); }
    av_restore();
}
static void av_au_state(bool server)
{
    av_au_dirty = true;
    av_setup();
    if (!server) { av_reset(); av_au_made = false; return; }
    if (!av_au_made)
    {
        /* signed in, Dune on the card, part way through; the covers made */
        av_reset();
        av_signin("tai", AV_PW);
        pthread_mutex_lock(&av.mu);
        struct av_prog *p = av_prog("b1", true);
        p->cur = 2700; p->dur = 7200.5; p->have = true; p->upd = av_now_ms() - 3600000;
        pthread_mutex_unlock(&av.mu);
        av_download_b1();
        abs_lib_page("lib1", "Audiobooks", false, 0, "", 0, false); av_open_wait();
        uint64_t t0 = h_ms();
        while (h_ms() - t0 < 15000 && !av_idle()) { abs_poll(); frame(30); usleep(8000); }
        av_au_made = true;
    }
    while (npages > 1) { page_pop(); frame(300); }
    abs_poll();
}
static void av_au(int which)
{
    av_au_state(which != 0);
    switch (which)
    {
        case 0: abs_page(); break;                                        /* no server */
        case 1: abs_edit_page(); lv_textarea_set_text(abs_url_ta, "abs.example.com"); lv_textarea_set_text(abs_user_ta, "tai");
                lv_textarea_set_text(abs_pass_ta, "secret"); abs_test_say("Signing in\xe2\x80\xa6", C_SUB); break;
        case 2: abs_edit_page(); lv_textarea_set_text(abs_url_ta, av_url_s); lv_textarea_set_text(abs_user_ta, "tai");
                lv_textarea_set_text(abs_pass_ta, "wrong"); av_seq0 = abs_st.test_seq; abs_test_cb(NULL); av_wait(av_test_came, 8000); break;
        case 3: abs_page(); av_open_wait(); break;                        /* home */
        case 4: abs_lib_page("lib1", "Audiobooks", false, 0, "", 0, false); av_open_wait(); break;
        case 5: abs_lib_page("lib1", "Audiobooks", false, 1, "", 0, false); av_open_wait(); break;
        case 6: abs_lib_page("lib1", "Audiobooks", false, 2, "", 0, false); av_open_wait(); break;
        case 7: abs_item_page("b2", "Children of Dune"); av_open_wait(); break;   /* not on the card */
        case 8: abs_item_page("b1", "Dune"); av_open_wait(); break;       /* on the card */
        case 9:                                                           /* the server's place taken */
            pthread_mutex_lock(&av.mu); snprintf(av.device, sizeof av.device, "Pixel 7");
            { struct av_prog *p = av_prog("b1", true); p->cur = 3000; p->upd = av_now_ms() + 5000; }
            pthread_mutex_unlock(&av.mu);
            abs_item_page("b1", "Dune"); av_open_wait();
            for (int i = 0; i < 20 && !abs_note.text[0]; i++) { abs_poll(); frame(50); usleep(5000); }
            frame(100);
            break;
        case 10:                                                          /* coming down, its percent */
        {
            pthread_mutex_lock(&sbv.mu); sbv.slice = 20000; pthread_mutex_unlock(&sbv.mu);
            abs_item_page("p1", "Night Shift Radio"); av_open_wait();
            sl_abs_download("p1", "e1");
            struct sl_abs_status s;
            uint64_t t0 = h_ms();
            do { abs_poll(); frame(30); usleep(5000); sl_abs_status(&s); } while (h_ms() - t0 < 5000 && s.dl_got < 20000);
            pthread_mutex_lock(&sbv.mu); sbv.stall = true; pthread_mutex_unlock(&sbv.mu);
            for (int i = 0; i < 10; i++) { abs_poll(); frame(40); }
            break;
        }
        case 11: abs_item_page("p1", "Night Shift Radio"); av_open_wait(); break;
        case 12: abs_local_page(); av_open_wait(); break;
        case 13: abs_item_page("b2", "Children of Dune"); av_open_wait(); abs_download("b2", "", "Children of Dune", 400LL * 1024 * 1024); frame(300); break;
        case 14: abs_item_page("gone", "Gone"); av_open_wait(); break;    /* an error */
        case 15: pthread_mutex_lock(&av.mu); av.slow = true; pthread_mutex_unlock(&av.mu);
                 abs_lib_page("lib2", "Podcasts", true, 0, "", 0, false); frame(200); break;   /* waiting */
        case 16: wf_st.connected = false; sl_abs_online(false); abs_poll(); abs_page(); av_open_wait(); break;   /* offline */
        case 17: sub_servers_page(); frame(200); lv_obj_scroll_to_y(lv_obj_get_child(pages[npages - 1], 1), 2000, LV_ANIM_OFF); break;
        case 18: abs_edit_page(); break;                                  /* signed in: its page */
        case 19: fake_books_n = 1; books_page(); frame(200); lv_obj_scroll_to_y(lv_obj_get_child(pages[npages - 1], 1), 2000, LV_ANIM_OFF); break;
    }
}
