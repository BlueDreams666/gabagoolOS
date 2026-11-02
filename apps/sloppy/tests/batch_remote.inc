/* P41 (#55): the phone remote as an app (sloppy_remote.inc, sloppy_net.c's
 * server, the page from sloppy-dev/remote). Included from ui_test.c after
 * batch_h.inc, whose helpers it uses.
 *   rm_tests()              in the suite
 *   SLOPPY_REMOTE_TESTS=1   the tests alone
 *   audit screen wifi-remote-code, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=wifi-remote ./ui-test
 *   SLOPPY_REMOTE_SERVE=<s> keeps the remote up that long with the player
 *   running, for a browser on this Mac (http://127.0.0.1:4400) */
#include "../sloppy_remote_page.h"           /* the page as the server has it */
#define RMT_DIR "/RemoteTest"
static char rmt_buf[1 << 20];            /* a whole answer: the 512 icon fits */

static uint64_t rmt_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
/* a GET of any size, as a phone's browser waits for it (http_get gives up
 * after 300 frames, which pass in no time here); the body and its length */
static int rmt_get(const char *path, char **body)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), got = 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(SL_REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a));
    char req[320];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s\r\n", path, rm_cookie);
    send(s, req, (size_t)n, 0);
    for (uint64_t until = rmt_ms() + 5000; rmt_ms() < until && got < (int)sizeof rmt_buf - 1; )
    {
        ssize_t r = recv(s, rmt_buf + got, sizeof rmt_buf - 1 - (size_t)got, MSG_DONTWAIT);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;
        frame(5);
        usleep(1000);
    }
    rmt_buf[got] = '\0';
    close(s);
    char *b = strstr(rmt_buf, "\r\n\r\n");
    *body = b ? b + 4 : rmt_buf;
    return b ? got - (int)(b + 4 - rmt_buf) : -1;
}
/* a request of our own making; its answer's status code */
static int rmt_status(const char *req)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), got = 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(SL_REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a));
    send(s, req, strlen(req), 0);
    char b[64];
    for (uint64_t until = rmt_ms() + 5000; rmt_ms() < until && got < 20; )
    {
        ssize_t r = recv(s, b + got, sizeof b - 1 - (size_t)got, MSG_DONTWAIT);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;
        frame(5);
        usleep(1000);
    }
    b[got] = '\0';
    close(s);
    return got > 12 ? atoi(b + 9) : -1;
}
/* ---- a strict little JSON reader: true when s is one JSON value and nothing else */
static const char *rmt_ws(const char *s) { while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++; return s; }
static const char *rmt_val(const char *s, int depth);
static const char *rmt_strv(const char *s)
{
    if (*s++ != '"') return NULL;
    for (; *s != '"'; s++)
    {
        if ((unsigned char)*s < 0x20) return NULL;
        if (*s == '\\')
        {
            s++;
            if (*s == 'u') { for (int i = 1; i <= 4; i++) if (!isxdigit((unsigned char)s[i])) return NULL; s += 4; }
            else if (!strchr("\"\\/bfnrt", *s) || !*s) return NULL;
        }
    }
    return s + 1;
}
static const char *rmt_val(const char *s, int depth)
{
    s = rmt_ws(s);
    if (depth > 32) return NULL;
    if (*s == '{' || *s == '[')
    {
        char close = *s == '{' ? '}' : ']';
        bool obj = *s == '{';
        s = rmt_ws(s + 1);
        if (*s == close) return s + 1;
        for (;;)
        {
            if (obj) { s = rmt_strv(rmt_ws(s)); if (!s) return NULL; s = rmt_ws(s); if (*s++ != ':') return NULL; }
            s = rmt_val(s, depth + 1);
            if (!s) return NULL;
            s = rmt_ws(s);
            if (*s == ',') { s++; continue; }
            return *s == close ? s + 1 : NULL;
        }
    }
    if (*s == '"') return rmt_strv(s);
    if (!strncmp(s, "true", 4)) return s + 4;
    if (!strncmp(s, "false", 5)) return s + 5;
    if (!strncmp(s, "null", 4)) return s + 4;
    char *e;
    strtod(s, &e);
    return e > s ? e : NULL;
}
static bool rmt_json(const char *s) { const char *e = rmt_val(s, 0); return e && !*rmt_ws(e); }
static long rmt_num(const char *json, const char *key)
{
    char k[48]; snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    return p ? strtol(p + strlen(k), NULL, 10) : -999;
}
static unsigned long rmt_unum(const char *json, const char *key)
{
    char k[48]; snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    return p ? strtoul(p + strlen(k), NULL, 10) : 0;
}
/* a PNG read here, chunk by chunk (their CRCs checked), inflated and
 * unfiltered: 8-bit RGB only, as the icons are. 0 when it is one. */
static uint32_t rmt_crc(const unsigned char *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}
static unsigned rmt_be(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int rmt_decode(unsigned char **out, unsigned *w, unsigned *h, const unsigned char *in, size_t n)
{
    static unsigned char idat[1 << 20];
    size_t il = 0, i = 8;
    bool end = false;
    *out = NULL;
    if (n < 8 || memcmp(in, "\x89PNG\r\n\x1a\n", 8)) return 1;
    while (i + 12 <= n && !end)
    {
        unsigned len = rmt_be(in + i);
        const unsigned char *t = in + i + 4;
        if (i + 12 + len > n || rmt_crc(t, len + 4) != rmt_be(t + 4 + len)) return 2;
        if (!memcmp(t, "IHDR", 4)) { *w = rmt_be(t + 4); *h = rmt_be(t + 8); if (t[12] != 8 || t[13] != 2 || t[16]) return 3; }
        else if (!memcmp(t, "IDAT", 4)) { if (il + len > sizeof idat) return 4; memcpy(idat + il, t + 4, len); il += len; }
        else if (!memcmp(t, "IEND", 4)) end = true;
        i += 12 + len;
    }
    size_t row = (size_t)*w * 3, need = (row + 1) * *h, got = 0;
    unsigned char *raw = NULL;
    if (!end || il < 2 || sl_inflate(idat + 2, il - 2, need, &raw, &got) != 0 || got != need) { free(raw); return 5; }
    unsigned char *px = malloc(row * *h);
    for (unsigned y = 0; y < *h; y++)
    {
        int f = raw[y * (row + 1)];
        const unsigned char *src = raw + y * (row + 1) + 1;
        unsigned char *d = px + y * row, *up = y ? d - row : NULL;
        for (size_t x = 0; x < row; x++)
        {
            int a = x >= 3 ? d[x - 3] : 0, b = up ? up[x] : 0, c = x >= 3 && up ? up[x - 3] : 0, pr;
            int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
            pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            d[x] = (unsigned char)(src[x] + (f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? pr : 0));
        }
    }
    free(raw);
    *out = px;
    return 0;
}
/* a PNG that decodes, w x h; its middle and corner pixels */
static void rmt_png(const char *path, int size, uint32_t *mid, uint32_t *corner)
{
    char *b;
    int n = rmt_get(path, &b);
    if (n <= 8) printf("remote: %s gave %d: %.200s\n", path, n, rmt_buf);
    assert(n > 8 && strstr(rmt_buf, "Content-Type: image/png") && !memcmp(b, "\x89PNG\r\n\x1a\n", 8));
    unsigned char *px = NULL;
    unsigned w = 0, h = 0;
    assert(!rmt_decode(&px, &w, &h, (const unsigned char *)b, (size_t)n) && w == (unsigned)size && h == (unsigned)size);
    unsigned char *m = px + ((size_t)(h / 2) * w + w / 2) * 3, *c = px;
    *mid = (uint32_t)m[0] << 16 | m[1] << 8 | m[2];
    *corner = (uint32_t)c[0] << 16 | c[1] << 8 | c[2];
    printf("remote: %s %dx%d, %d bytes\n", path, size, size, n);
    free(px);
}
static int rmt_near(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int s = 0; s < 24; s += 8) { int x = (int)(a >> s & 255) - (int)(b >> s & 255); d += x < 0 ? -x : x; }
    return d;
}
static void rmt_write(const char *rb, const void *data, size_t n)
{
    char p[600];
    snprintf(p, sizeof p, H_ROOT "%s", rb);
    FILE *f = fopen(p, "wb");
    assert(f && fwrite(data, 1, n, f) == n);
    fclose(f);
}

/* pairing: a phone the player does not know gets the page, its words and
 * /pair, nothing else; the code it must type shows on the player; five
 * wrong codes and pairing waits; Forget sends every phone back to pairing */
static void rmt_get_as(const char *path, const char *cookie, int want)
{
    char rq[400];
    snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s\r\n", path, cookie);
    int got = rmt_status(rq);
    if (got != want) printf("FAIL: %s answered %d, not %d (%s)\n", path, got, want, cookie);
    assert(got == want);
}
static void rm_pair_tests(void)
{
    char code[8], wrong[8], path[40], cookie[64], file[MAX_PATH];
    unsigned serial, s0;
    char *b;
    snprintf(file, sizeof file, "%s/.rockbox/sloppy/remote-phones", sim_root_dir);
    sl_net_remote_forget();
    assert(sl_net_remote_paired() == 0 && access(file, F_OK));
    rm_cookie[0] = '\0';

    /* unknown: the page and its words; every command, the cover, the screenshot and an update refused */
    rmt_get_as("/", "", 200);
    rmt_get_as("/manifest.webmanifest", "", 200);
    rmt_get_as("/api/hello", "", 200);
    static const char *const shut[] = { "/api/now", "/api/cmd?c=next", "/api/cmd?c=shot", "/api/vol?v=0", "/api/seek?ms=0",
                                        "/api/search?q=a", "/api/play?t=0&m=now", "/api/queue", "/api/jump?k=0&r=0",
                                        "/api/update", "/cover?k=1", "/shot.png" };
    int nexts = audio_nexts;
    for (unsigned i = 0; i < ARRAYLEN(shut); i++) rmt_get_as(shut[i], "", 401);
    assert(audio_nexts == nexts);
    assert(rmt_status("POST /update HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\n") == 401);
    rmt_get_as("/api/now", "Cookie: gbtok=0123456789abcdef0123456789abcdef\r\n", 401);   /* a made-up token */
    rmt_get_as("/api/now", "Cookie: gbtok=\r\n", 401);

    /* the phone asks: the player shows six digits on a page of its own; asking again keeps them */
    assert(!sl_net_remote_pair_code(code, sizeof code, &serial));
    rmt_get_as("/pair", "", 202);
    assert(sl_net_remote_pair_code(code, sizeof code, &s0) && strlen(code) == 6 && strspn(code, "0123456789") == 6);
    frame(300);
    assert(rm_pair_obj && pages[npages - 1] == rm_pair_obj && find_label(rm_pair_obj, code));
    assert(find_label(rm_pair_obj, "Type this code on your phone to let it control the player."));
    shot("remote-pair.ppm");
    rmt_get_as("/pair", "", 202);
    { char c2[8]; assert(sl_net_remote_pair_code(c2, sizeof c2, &serial) && serial == s0 && !strcmp(c2, code)); }

    /* a wrong code, then the right one: a token in a cookie no script and no other site gets */
    snprintf(wrong, sizeof wrong, "%06d", (atoi(code) + 1) % 1000000);
    snprintf(path, sizeof path, "/pair?code=%s", wrong);
    rmt_get_as(path, "", 403);
    snprintf(path, sizeof path, "/pair?code=%s", code);
    rmt_get(path, &b);
    assert(strstr(rmt_buf, "200 OK") && strstr(rmt_buf, "; HttpOnly; SameSite=Strict\r\n"));
    char *c = strstr(rmt_buf, "Set-Cookie: gbtok=");
    assert(c && strspn(c + 18, "0123456789abcdef") == 32);
    snprintf(cookie, sizeof cookie, "Cookie: %.38s\r\n", c + 12);
    rmt_get_as(path, "", 410);           /* used up */
    frame(300);
    assert(!rm_pair_obj && sl_net_remote_paired() == 1);
    {   /* kept on the card, for after a restart */
        char line[64] = "";
        FILE *f = fopen(file, "r");
        assert(f && fgets(line, sizeof line, f) && !strncmp(line, c + 18, 32));
        fclose(f);
    }
    rmt_get_as("/api/now", cookie, 200);
    {   /* among other cookies too, as a browser sends them */
        char two[96];
        snprintf(two, sizeof two, "Cookie: hint=1; %s", cookie + 8);
        rmt_get_as("/api/now", two, 200);
    }
    rmt_get_as("/cover?k=1", cookie, 404);   /* let in; no cover playing */

    /* five wrong codes: the code is gone and pairing waits, the right code too; the paired phone goes on */
    rmt_get_as("/pair", "", 202);
    assert(sl_net_remote_pair_code(code, sizeof code, &serial) && serial != s0);
    snprintf(wrong, sizeof wrong, "%06d", (atoi(code) + 1) % 1000000);
    snprintf(path, sizeof path, "/pair?code=%s", wrong);
    for (int i = 0; i < 4; i++) rmt_get_as(path, "", 403);
    rmt_get_as(path, "", 429);
    snprintf(path, sizeof path, "/pair?code=%s", code);
    rmt_get_as(path, "", 429);
    rmt_get_as("/pair", "", 429);
    assert(!sl_net_remote_pair_code(code, sizeof code, &serial));
    frame(300);
    assert(!rm_pair_obj && sl_net_remote_paired() == 1);
    rmt_get_as("/api/now", cookie, 200);

    /* Forget (the Wi-Fi page's Phone remote card): the phone is refused, the card's list gone, pairing open again */
    rm_forget_cb(NULL);
    assert(sl_net_remote_paired() == 0 && access(file, F_OK));
    rmt_get_as("/api/now", cookie, 401);
    rmt_get_as("/api/cmd?c=next", cookie, 401);
    assert(audio_nexts == nexts);

    /* Don't pair, on the player: the code it showed is no good */
    rmt_get_as("/pair", "", 202);
    assert(sl_net_remote_pair_code(code, sizeof code, &serial));
    frame(300);
    assert(rm_pair_obj);
    rm_pair_no_cb(NULL);
    frame(300);
    assert(!rm_pair_obj && !sl_net_remote_pair_code(wrong, sizeof wrong, &serial));
    snprintf(path, sizeof path, "/pair?code=%s", code);
    rmt_get_as(path, "", 410);

    rm_test_pair();                      /* paired again, for the rest */
    assert(sl_net_remote_paired() == 1);
    printf("PASS: phone remote pairing: unpaired refused, the code on the player, a token, five tries, Forget, Don't pair.\n");
}
static void rm_tests(void)
{
    char *b;
    int n;
    struct mp3entry keep = test_track;
    int status0 = test_status, vol0 = global_status.volume, shuf0 = global_settings.playlist_shuffle,
        rep0 = global_settings.repeat_mode, lang0 = global_settings.sloppy_language;
    rm_set(1);
    assert(sl_net_remote_on());
    pop_hide_now();
    rm_pair_tests();

    /* the page: an app's tags, everything inline, nothing from the internet */
    n = rmt_get("/", &b);
    assert(n > 0 && strstr(rmt_buf, "200 OK") && strstr(rmt_buf, "text/html"));
    assert(n == (int)sizeof sl_remote_html - 1);
    assert(strstr(b, "<link rel=\"manifest\" href=\"/manifest.webmanifest\">"));
    assert(strstr(b, "<link rel=\"apple-touch-icon\" href=\"/apple-touch-icon.png\">"));
    assert(strstr(b, "name=\"apple-mobile-web-app-capable\" content=\"yes\""));
    assert(strstr(b, "name=\"viewport\"") && strstr(b, "prefers-color-scheme"));
    assert(!strstr(b, "http://") && !strstr(b, "https://") && !strstr(b, "<script src") && !strstr(b, "@import"));
    assert(!strstr(b, "serviceWorker"));   /* none: phones only run one from HTTPS, and the remote is plain HTTP */
    printf("remote: the page is %d bytes, inline CSS and JS\n", n);
    /* every word it shows is one the player translates */
    {
        static const char *const keys[] = { "data-t=\"", "data-a=\"", "data-p=\"", "t(\"" };
        int found = 0;
        for (unsigned k = 0; k < ARRAYLEN(keys); k++)
            for (const char *p = strstr(b, keys[k]); p; p = strstr(p + 1, keys[k]))
            {
                if (k == 3 && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) continue;   /* get(", not t(" */
                const char *s = p + strlen(keys[k]), *e = strchr(s, '"');
                char w[160];
                assert(e && e - s < (int)sizeof w);
                memcpy(w, s, (size_t)(e - s)); w[e - s] = '\0';
                bool in = false;
                for (unsigned i = 0; i < ARRAYLEN(rm_words); i++) in |= !strcmp(rm_words[i], w);
                if (!in) printf("remote: \"%s\" is not in rm_words\n", w);
                assert(in);
                found++;
            }
        assert(found >= 20);
        for (unsigned i = 0; i < ARRAYLEN(rm_words); i++)
        {
            for (int l = 1; l <= 6; l++) { if (!sl_tr_find(rm_words[i], l)) printf("remote: no %d for %s\n", l, rm_words[i]); assert(sl_tr_find(rm_words[i], l)); }   /* a row in every table (French "Pause" is "Pause") */
        }
    }

    /* the manifest */
    n = rmt_get("/manifest.webmanifest", &b);
    assert(n > 0 && strstr(rmt_buf, "Content-Type: application/manifest+json"));
    assert(rmt_json(b));
    assert(strstr(b, "\"name\":\"GabagoolOS Remote\"") && strstr(b, "\"short_name\":\"GabagoolOS\""));
    assert(strstr(b, "\"display\":\"standalone\"") && strstr(b, "\"start_url\":\"/"));
    assert(strstr(b, "\"theme_color\":\"#0d1219\"") && strstr(b, "\"background_color\":\"#0d1219\""));
    assert(strstr(b, "\"src\":\"/icon-192.png\",\"sizes\":\"192x192\"") && strstr(b, "\"src\":\"/icon-512.png\",\"sizes\":\"512x512\""));
    assert(strstr(b, "\"purpose\":\"maskable\""));

    { uint64_t t0 = rmt_ms(); unsigned char *pp; size_t pn = sl_net_remote_icon(512, &pp); printf("remote: icon 512 made in %llu ms, %zu bytes\n", (unsigned long long)(rmt_ms() - t0), pn); free(pp); }
    /* the icons: real PNGs of their sizes, the accent's disc on the night blue, its ink in the middle */
    {
        uint32_t mid, corner, acc = sl_accent(), ink = sl_ink_on(acc);
        uint64_t t0 = rmt_ms();
        rmt_png("/icon-512.png", 512, &mid, &corner);
        printf("remote: the 512 icon drawn and sent in %llu ms\n", (unsigned long long)(rmt_ms() - t0));
        printf("remote: icon corner %06x middle %06x (ink %06x)\n", corner, mid, ink);
        assert(rmt_near(corner, 0x1b2533) < 24 && rmt_near(mid, ink) < 40);
        rmt_png("/icon-192.png", 192, &mid, &corner);
        assert(rmt_near(mid, ink) < 40);
        rmt_png("/apple-touch-icon.png", 180, &mid, &corner);
        rmt_png("/favicon.ico", 48, &mid, &corner);
        /* a point on the disc, off the play mark: the accent */
        unsigned char *png;
        size_t pn = sl_net_remote_icon(200, &png);
        unsigned char *px = NULL; unsigned w = 0, h = 0;
        assert(pn && !rmt_decode(&px, &w, &h, png, pn));
        unsigned char *q = px + ((size_t)70 * w + 100) * 3;   /* above the mark, inside the disc (r 60) */
        assert(rmt_near((uint32_t)q[0] << 16 | q[1] << 8 | q[2], acc) < 60);
        free(px); free(png);
    }

    /* what plays; the page's words and colours */
    test_status = AUDIO_STATUS_PLAY;
    n = rmt_get("/api/now", &b);
    assert(n > 0 && rmt_json(b) && strstr(b, "\"title\":") && strstr(b, "\"album\":") && strstr(b, "\"shuffle\":") &&
           strstr(b, "\"repeat\":") && strstr(b, "\"cover\":") && strstr(b, "\"qrev\":") && strstr(b, "\"cfg\":"));
    n = rmt_get("/api/hello", &b);
    assert(n > 0 && rmt_json(b) && strstr(b, "\"lang\":\"en\"") && strstr(b, "\"dir\":\"ltr\"") && strstr(b, "\"Up next\":\"Up next\""));
    {
        unsigned c0 = (unsigned)rmt_unum(b, "cfg");
        global_settings.sloppy_language = 4;   /* Hebrew: its words, right to left, and the page asks again */
        n = rmt_get("/api/hello", &b);
        assert(rmt_json(b) && strstr(b, "\"lang\":\"he\"") && strstr(b, "\"dir\":\"rtl\"") && strstr(b, "\"Pause\":\"\xd7\x94\xd7\xa9\xd7\x94\xd7\x99\xd7\x94\""));
        assert((unsigned)rmt_unum(b, "cfg") != c0);
        global_settings.sloppy_language = 6;
        n = rmt_get("/api/hello", &b);
        assert(rmt_json(b) && strstr(b, "\"lang\":\"zh\"") && strstr(b, "\"Pause\":\"\xe6\x9a\x82\xe5\x81\x9c\""));
        global_settings.sloppy_language = lang0;
    }

    /* the controls reach the player */
    {
        static int list[6];
        for (int i = 0; i < 6; i++) list[i] = i;
        assert(play_list(list, 6, 0, false));
        frame(100);
        test_status = AUDIO_STATUS_PLAY;
        sl_track_path(0,test_track.path,sizeof test_track.path);
        test_track.title = (char *)sl_lib.tracks[0].title;
        test_track.length = 200000;
        test_track.elapsed = 1000;
        n = rmt_get("/api/cmd?c=toggle", &b);
        assert(rmt_json(b) && (test_status & AUDIO_STATUS_PAUSE) && strstr(b, "\"playing\":false"));
        n = rmt_get("/api/cmd?c=toggle", &b);
        assert(!(test_status & AUDIO_STATUS_PAUSE) && strstr(b, "\"playing\":true"));
        int nexts = audio_nexts;
        rmt_get("/api/cmd?c=next", &b);
        assert(audio_nexts == nexts + 1);
        n = rmt_get("/api/seek?ms=61000", &b);
        assert(test_track.elapsed == 61000 && rmt_json(b));
        rmt_get("/api/seek?ms=999999", &b);
        assert(test_track.elapsed == 199500);   /* not past the end */
        n = rmt_get("/api/vol?v=40", &b);
        assert(volume_pct() == 40 && rmt_num(b, "vol") == 40);
        int sh = global_settings.playlist_shuffle;
        rmt_get("/api/cmd?c=shuffle", &b);
        assert(global_settings.playlist_shuffle == !sh && strstr(b, sh ? "\"shuffle\":false" : "\"shuffle\":true"));
        rmt_get("/api/cmd?c=shuffle", &b);
        assert(global_settings.playlist_shuffle == sh);
        global_settings.repeat_mode = REPEAT_OFF;
        rmt_get("/api/cmd?c=repeat", &b); assert(global_settings.repeat_mode == REPEAT_ALL && rmt_num(b, "repeat") == 1);
        rmt_get("/api/cmd?c=repeat", &b); assert(global_settings.repeat_mode == REPEAT_ONE && rmt_num(b, "repeat") == 2);
        rmt_get("/api/cmd?c=repeat", &b); assert(global_settings.repeat_mode == REPEAT_OFF && rmt_num(b, "repeat") == 0);
        bool loved = ts_rating(0) == 1;
        rmt_get("/api/cmd?c=love", &b);
        assert((ts_rating(0) == 1) == !loved && strstr(b, loved ? "\"loved\":false" : "\"loved\":true"));
        rmt_get("/api/cmd?c=love", &b);
        assert((ts_rating(0) == 1) == loved);
        pop_hide_now();
        nbanner_hide_now();

        /* Up next, and a tap on one of them */
        n = rmt_get("/api/queue", &b);
        assert(n > 0 && rmt_json(b) && strstr(b, "\"items\":[{\"k\":1,"));
        unsigned rev = (unsigned)rmt_unum(b, "rev");
        int at = q_cur();
        char u[64];
        snprintf(u, sizeof u, "/api/jump?k=2&r=%u", rev + 1);   /* a list it no longer shows: nothing */
        rmt_get(u, &b);
        assert(q_cur() == at);
        snprintf(u, sizeof u, "/api/jump?k=2&r=%u", rev);
        rmt_get(u, &b);
        assert(q_cur() == at + 2 && (unsigned)rmt_unum(b, "qrev") != rev);

        /* only the player's own page, at its address: another site's page (a link, a form, a script)
         * and a name that is someone else's (DNS rebinding) are turned away */
        char rq[320];
        snprintf(rq, sizeof rq, "GET /api/now HTTP/1.1\r\nHost: 127.0.0.1:4400\r\n%s\r\n", rm_cookie);
        assert(rmt_status(rq) == 200);
        snprintf(rq, sizeof rq, "GET /api/now HTTP/1.1\r\nHost: 192.168.1.20\r\nOrigin: http://192.168.1.20\r\nSec-Fetch-Site: same-origin\r\n%s\r\n", rm_cookie);
        assert(rmt_status(rq) == 200);
        assert(rmt_status("GET /api/now HTTP/1.1\r\nHost: evil.example:4400\r\n\r\n") == 403);
        assert(rmt_status("GET /api/cmd?c=next HTTP/1.1\r\nHost: 127.0.0.1\r\nSec-Fetch-Site: cross-site\r\n\r\n") == 403);
        assert(rmt_status("GET /api/cmd?c=next HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.example\r\n\r\n") == 403);
        assert(rmt_status("POST /update HTTP/1.1\r\nHost: evil.example\r\nContent-Length: 10\r\n\r\n") == 421);
        /* search, then Play next and Add to queue */
        n = rmt_get("/api/search?q=night", &b);
        assert(rmt_json(b) && strstr(b, "\"songs\":[{\"t\":") && strstr(b, "\"album\":"));
        {   /* songs whose title and artist are 700 quote marks each (twice as long written out, so an entry runs over the buffer's end):
             * the answer stops at its buffer, still whole JSON, never written past it */
            static char qt[1100];
            memset(qt, 0, sizeof qt); memcpy(qt, "zzqq ", 5); memset(qt + 5, '"', 700);
            const char *keep_t[40], *keep_a[40], *keep_al[40];
            int nk = sl_lib.n_tracks < 40 ? sl_lib.n_tracks : 40;
            for (int i = 0; i < nk; i++)
            {
                keep_t[i] = sl_lib.tracks[i].title; keep_a[i] = sl_lib.tracks[i].artist; keep_al[i] = sl_lib.albums[sl_lib.tracks[i].album].title;
                sl_lib.tracks[i].title = qt; sl_lib.tracks[i].artist = qt;
            }
            int got = rmt_get("/api/search?q=zzqq", &b);
            for (int i = nk - 1; i >= 0; i--)
            {
                sl_lib.tracks[i].title = keep_t[i]; sl_lib.tracks[i].artist = keep_a[i]; sl_lib.albums[sl_lib.tracks[i].album].title = keep_al[i];
            }
            assert(nk >= 6 && got > 0 && rmt_json(b) && strstr(b, "\"songs\":[{") && strlen(b) < sizeof rm_out);
        }
        n = rmt_get("/api/search?q=night", &b);
        int t = (int)rmt_num(b, "t"), amount = playlist_amount();
        snprintf(u, sizeof u, "/api/play?t=%d&m=next", t);
        rmt_get(u, &b);
        assert(playlist_amount() == amount + 1);
        n = rmt_get("/api/queue", &b);
        assert(strstr(b, "\"items\":[{\"k\":1,\"q\":true,"));
        snprintf(u, sizeof u, "/api/play?t=%d&m=queue", t);
        rmt_get(u, &b);
        assert(playlist_amount() == amount + 2);
    }

    /* the cover: a tag's picture, unsynchronised as ID3 keeps it, sent undone; then a folder's */
    {
        char d[700];
        snprintf(d, sizeof d, "rm -rf '" H_ROOT RMT_DIR "' && mkdir -p '" H_ROOT RMT_DIR "'");
        assert(system(d) == 0);
        static const unsigned char tagged[] = "ID3-and-more\xff\xd8\xff\x00\xe0JFIF\xff\x00\x00tail\xff\xd9" "after";
        static const unsigned char want[] = "\xff\xd8\xff\xe0JFIF\xff\x00tail\xff\xd9";
        rmt_write(RMT_DIR "/song.mp3", tagged, sizeof tagged - 1);
        strlcpy(test_track.path, RMT_DIR "/song.mp3", sizeof test_track.path);
        test_track.has_embedded_albumart = true;
        test_track.albumart.type = AA_TYPE_JPG | AA_FLAG_ID3_UNSYNC;
        test_track.albumart.pos = 12;
        test_track.albumart.size = (int)(sizeof tagged - 1 - 12 - 5);
        rmt_get("/api/now", &b);
        unsigned key = (unsigned)rmt_unum(b, "cover");
        assert(key);
        char u[64];
        snprintf(u, sizeof u, "/cover?k=%u", key);
        n = rmt_get(u, &b);
        assert(n == (int)sizeof want - 1 && !memcmp(b, want, sizeof want - 1) && strstr(rmt_buf, "image/jpeg") && strstr(rmt_buf, "immutable"));
        snprintf(u, sizeof u, "/cover?k=%u", key + 2);   /* an old one */
        n = rmt_get(u, &b);
        assert(strstr(rmt_buf, "404"));
        /* no picture in the tag: the folder's cover, as it is */
        static unsigned char png[3000];
        memcpy(png, "\x89PNG\r\n\x1a\n", 8);
        for (int i = 8; i < (int)sizeof png; i++) png[i] = (unsigned char)(i * 7);
        rmt_write(RMT_DIR "/cover.png", png, sizeof png);
        rmt_write(RMT_DIR "/other.mp3", "x", 1);
        strlcpy(test_track.path, RMT_DIR "/other.mp3", sizeof test_track.path);
        test_track.has_embedded_albumart = false;
        test_folder_cover = RMT_DIR "/cover.png";
        rmt_get("/api/now", &b);
        unsigned k2 = (unsigned)rmt_unum(b, "cover");
        assert(k2 && k2 != key);
        snprintf(u, sizeof u, "/cover?k=%u", k2);
        n = rmt_get(u, &b);
        assert(n == (int)sizeof png && !memcmp(b, png, sizeof png) && strstr(rmt_buf, "image/png"));
        /* none at all: no key, and the page draws its own */
        test_folder_cover = NULL;
        strlcpy(test_track.path, RMT_DIR "/song.mp3", sizeof test_track.path);
        test_track.albumart.type = AA_TYPE_BMP;
        test_track.has_embedded_albumart = true;
        rm_cov_track[0] = '\0';
        rmt_get("/api/now", &b);
        assert(rmt_unum(b, "cover") == 0);
        test_track.has_embedded_albumart = false;
    }

    /* a browser's connection opened ahead and left silent must not hold up the next ask */
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(SL_REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(!connect(s, (struct sockaddr *)&a, sizeof a));
        uint64_t t0 = rmt_ms();
        n = rmt_get("/api/now", &b);
        uint64_t took = rmt_ms() - t0;
        printf("remote: an ask beside a silent connection answered in %llu ms\n", (unsigned long long)took);
        assert(n > 0 && rmt_json(b) && took < 1000);
        close(s);
    }

    /* the cost: what a poll takes on the interface thread, and all of it */
    {
        rm_cost_sum_us = 0; rm_cost_n = 0; rm_cost_max_us = 0;
        unsigned served = sl_net_remote_served;
        uint64_t t0 = rmt_ms();
        for (int i = 0; i < 50; i++) { n = rmt_get("/api/now", &b); assert(n > 0 && n < 1024); }
        uint64_t wall = rmt_ms() - t0;
        assert(sl_net_remote_served == served + 50 && rm_cost_n == 50);
        unsigned avg = (unsigned)(rm_cost_sum_us / rm_cost_n);
        printf("remote: a poll costs the interface %u us (most %u us), %d bytes, %.1f ms a round trip here\n",
               avg, rm_cost_max_us, n, wall / 50.0);
        assert(avg < 2000);
        rm_cost_sum_us = 0; rm_cost_n = 0; rm_cost_max_us = 0;
        rmt_get("/api/queue", &b);
        printf("remote: Up next costs %u us\n", rm_cost_max_us);
        assert(rm_cost_max_us < 20000);
    }

    /* the Wi-Fi page: the code to scan while it is on, gone when it is off */
    {
        bool c0 = wf_st.connected;
        wf_st.connected = true;
        global_settings.sloppy_wifi = 1;
        strlcpy(wf_st.ip, "192.168.1.23", sizeof wf_st.ip);
        while (npages > 1) { page_pop(); frame(300); }
        sl_wifi_page();
        frame(400);
        assert(rm_code && !lv_obj_has_flag(rm_code, LV_OBJ_FLAG_HIDDEN));
        assert(find_label(rm_code, "Scan with your phone's camera. Add the page to your home screen to keep it as an app."));
        assert(find_label(pages[npages - 1], "Forget paired phones"));
        rm_set(0);
        assert(lv_obj_has_flag(rm_code, LV_OBJ_FLAG_HIDDEN));
        rm_set(1);
        pop_hide_now();
        assert(!lv_obj_has_flag(rm_code, LV_OBJ_FLAG_HIDDEN));
        while (npages > 1) { page_pop(); frame(300); }
        assert(!rm_code);
        wf_st.connected = c0;
    }

    rm_set(0);
    assert(!sl_net_remote_on());
    pop_hide_now();
    test_track = keep;
    test_status = status0;
    global_status.volume = vol0;
    global_settings.playlist_shuffle = shuf0;
    global_settings.repeat_mode = rep0;
    global_settings.sloppy_language = lang0;
    printf("PASS: phone remote as an app: page, manifest, icons, words, controls, Up next, search, cover, cost.\n");
}

/* SLOPPY_REMOTE_SERVE=<seconds>: the remote up, the player running, for a real browser */
static void rm_serve(int secs)
{
    rm_set(1);
    pop_hide_now();
    static int list[8];
    for (int i = 0; i < 8; i++) list[i] = i;
    play_list(list, 8, 0, false);
    test_status = AUDIO_STATUS_PLAY;
    sl_track_path(0,test_track.path,sizeof test_track.path);
    test_track.title = (char *)sl_lib.tracks[0].title;
    test_track.artist = (char *)sl_track_artist(0);
    test_track.length = 245000;
    test_track.elapsed = 83000;
    test_track.has_embedded_albumart = false;
    if (getenv("SLOPPY_REMOTE_COVER")) test_folder_cover = getenv("SLOPPY_REMOTE_COVER");   /* a Rockbox path under the test card */
    if (getenv("SLOPPY_REMOTE_LANG")) global_settings.sloppy_language = atoi(getenv("SLOPPY_REMOTE_LANG"));   /* 4 Hebrew, 6 Chinese */
    if (getenv("SLOPPY_REMOTE_ACCENT")) global_settings.sloppy_accent_color = (int)strtol(getenv("SLOPPY_REMOTE_ACCENT"), NULL, 16);
    printf("remote: serving on http://127.0.0.1:%d for %d s\n", SL_REMOTE_PORT, secs);
    fflush(stdout);
    uint64_t end = rmt_ms() + (uint64_t)secs * 1000;
    unsigned said = 0;
    while (rmt_ms() < end)
    {
        frame(20);
        char code[8];
        unsigned serial;
        if (sl_net_remote_pair_code(code, sizeof code, &serial) && serial != said) { said = serial; printf("remote: pairing code %s\n", code); fflush(stdout); }
        usleep(10000);
        if (!(test_status & AUDIO_STATUS_PAUSE)) test_track.elapsed += 30;
        if (test_track.elapsed >= test_track.length) test_track.elapsed = 0;
    }
    rm_set(0);
}

/* the audit's screen: the Wi-Fi page with the remote on, its code in view */
static bool rm_au_dirty, rm_au_conn0;
static int rm_au_wifi0;
static void rm_au(int which)
{
    (void)which;
    if (!rm_au_dirty) { rm_au_conn0 = wf_st.connected; rm_au_wifi0 = global_settings.sloppy_wifi; }
    rm_au_dirty = true;
    e_wifi(true);                        /* the pretend radio's network, and its address */
    rm_set(1);
    pop_hide_now();
    sl_wifi_page();
    frame(300);
    if (rm_code) lv_obj_scroll_to_view_recursive(rm_code, LV_ANIM_OFF);
}
static void rm_au_reset(void)
{
    if (!rm_au_dirty) return;
    rm_au_dirty = false;
    rm_set(0);
    pop_hide_now();
    e_wifi(rm_au_conn0);
    global_settings.sloppy_wifi = rm_au_wifi0;
}
