/* Gate 1 batch H: the big builds. Podcasts first. Included from ui_test.c
 * after ui_audit.inc, whose helpers it uses.
 *   h_tests()                                       in the suite
 *   SLOPPY_H_TESTS=1                                the tests alone
 *   SLOPPY_H_SHOTS=<dir> SLOPPY_AUDIT_TAG=now      the screenshots (SLOPPY_AUDIT_ONLY=podcasts ...) */

#define H_ROOT SIM_ROOT ""
#define H_POD "/Card/Podcasts"               /* the episodes, inside the sandboxed card: the queue takes them */
static uint64_t h_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void h_wait(bool (*done)(void), int ms)
{
    uint64_t until = h_ms() + (uint64_t)ms;
    while (h_ms() < until && !done()) { pod_poll(); frame(40); usleep(8000); }
    pod_poll();
    frame(40);
}
static bool h_file(const char *rb, struct stat *st) { char p[600]; snprintf(p, sizeof p, H_ROOT "%s", rb); return stat(p, st) == 0; }
static bool h_has(const char *rb, const char *w)
{
    char p[600]; snprintf(p, sizeof p, H_ROOT "%s", rb);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    static char b[256 * 1024]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return strstr(b, w) != NULL;
}

/* ---- the internet the podcasts see */
static int h_feed_gets, h_search_gets, h_art_gets, h_slow_ms, h_dl_polls;
static char h_last_url[512];
static bool h_html_feed;                        /* the next feed is a web page, not a feed */
static int h_feed_n = 48;                       /* episodes in history.xml */
static const char *const h_long_title =
    "Episode 48: The Quiet Architecture of Everything We Left Behind, a conversation about cities, rivers and the people who mapped them";
/* SLOPPY_SHOWCASE: Song Exploder's own episode names, newest first */
static const char *const sc_eps[] = { "Allison Russell - The Returner", "DJ Shadow - Mutual Slump", "Elvis Costello - Alison",
    "Olivia Rodrigo - The Cure", "Natalie Merchant - Sister Tilly", "Paula Cole - I Don't Want to Wait", "Charli XCX - Camera",
    "Mitski - Your Best American Girl", "Suki Waterhouse - Back in Love", "Key Change: Questlove on Prince",
    "Metric - Victim of Luck", "Hayley Kiyoko - Girls Like Girls" };
static void h_rss(char *out, size_t cap, const char *show, int n)
{
    size_t k = 0;
    bool sc = sc_on && !strcmp(show, "history");
    k += (size_t)snprintf(out + k, cap - k,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- a comment > with a bracket -->\n"
        "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\"><channel>\n"
        "<title>%s</title>\n<itunes:author>%s</itunes:author>\n"
        "<itunes:image href=\"https://art.test/%s/3000x3000.png\"/>\n"
        "<image><url>https://art.test/%s/old.png</url><title>Not this</title></image>\n"
        "<description><![CDATA[<p>Notes with <b>tags</b> & ampersands]]></description>\n",
        sc ? "Song Exploder" : !strcmp(show, "history") ? "Long History &amp; Co" : !strcmp(show, "hebrew") ? "\xd7\xa9\xd7\x99\xd7\x97\xd7\x95\xd7\xaa" : "Night Shift",
        sc ? "Hrishikesh Hirway" : "Mira Sol &amp; Friends", show, show);
    for (int i = n; i >= 1 && k + 2048 < cap; i--)
    {
        char title[300];
        if (sc) snprintf(title, sizeof title, "%s", n - i < (int)(sizeof sc_eps / sizeof sc_eps[0]) ? sc_eps[n - i] : "Song Exploder");
        else if (i == n && !strcmp(show, "history")) snprintf(title, sizeof title, "<![CDATA[%s]]>", h_long_title);
        else if (i == n - 1) snprintf(title, sizeof title, "Ep. %d: Caf&#233; &lt;live&gt; &#x2014; %s", i, show);
        else if (i == n - 2 && !strcmp(show, "history")) snprintf(title, sizeof title, "\xd7\xa4\xd7\xa8\xd7\xa7 %d", i);
        else snprintf(title, sizeof title, "Episode %d of %s", i, show);
        /* a week apart, the newest on 21 Oct 2026 */
        time_t at = (time_t)1792576800 - (time_t)(n - i) * 7 * 86400;
        struct tm tm; gmtime_r(&at, &tm);
        char date[64]; strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S +0000", &tm);
        k += (size_t)snprintf(out + k, cap - k,
            "<item>\n<title>%s</title>\n<guid isPermaLink=\"false\">%s-%d</guid>\n<pubDate>%s</pubDate>\n"
            "<itunes:duration>%s</itunes:duration>\n<description>Show notes for %d, long and ignored.</description>\n"
            "<enclosure url=\"https://cdn.test/%s/%d.mp3?src=feed\" length=\"%d\" type=\"audio/mpeg\"/>\n</item>\n",
            title, show, i, date, i % 3 == 0 ? "1:02:03" : i % 3 == 1 ? "45:10" : "2400", i, show, i, 100000 + i * 1000);
    }
    snprintf(out + k, cap - k, "</channel></rss>\n");
}
static unsigned char *h_png;
static size_t h_png_n;
unsigned lodepng_encode24(unsigned char **out, size_t *outsize, const unsigned char *image, unsigned w, unsigned h);
static void h_make_png(void)
{
    if (h_png) return;
    static unsigned char rgb[300 * 300 * 3];
    for (int y = 0; y < 300; y++) for (int x = 0; x < 300; x++)
    {
        unsigned char *p = rgb + (y * 300 + x) * 3;
        /* a sunset: orange to violet, a pale disc */
        int dx = x - 150, dy = y - 120;
        bool sun = dx * dx + dy * dy < 50 * 50;
        p[0] = (unsigned char)(sun ? 255 : 240 - y / 3);
        p[1] = (unsigned char)(sun ? 214 : 120 - y / 4);
        p[2] = (unsigned char)(sun ? 150 : 60 + y / 3);
    }
    assert(!lodepng_encode24(&h_png, &h_png_n, rgb, 300, 300) && h_png_n > 100);
}
static int h_fake(const char *url, char *out, size_t cap, const char *path)
{
    (void)path;
    snprintf(h_last_url, sizeof h_last_url, "%s", url);
    if (h_slow_ms) usleep((useconds_t)h_slow_ms * 1000);
    if (strstr(url, "itunes.apple.com/search"))
    {
        __atomic_add_fetch(&h_search_gets, 1, __ATOMIC_SEQ_CST);
        if (strstr(url, "term=nothing")) { snprintf(out, cap, "{\"resultCount\":0,\"results\":[]}"); return 200; }
        if (strstr(url, "term=down")) return -1;
        snprintf(out, cap,
            "{\"resultCount\":3,\"results\":[\n"
            "{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionName\":\"Long History & Co\",\"artistName\":\"Mira Sol & Friends\","
            "\"feedUrl\":\"https://feeds.test/history.xml\",\"artworkUrl100\":\"https://art.test/history/100x100bb.jpg\","
            "\"artworkUrl600\":\"https://art.test/history/600x600bb.jpg\",\"primaryGenreName\":\"History\",\"trackCount\":48,"
            "\"genres\":[\"History\",\"Podcasts\"],\"note\":\"a brace } in a string {\"},\n"
            "{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionName\":\"Night Shift\",\"artistName\":\"Kite Radio\","
            "\"feedUrl\":\"https://feeds.test/night.xml\",\"artworkUrl600\":\"https://art.test/night/600x600bb.jpg\","
            "\"primaryGenreName\":\"Music\",\"trackCount\":12},\n"
            "{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionName\":\"No Feed Here\",\"artistName\":\"Nobody\"}\n]}");
        return 200;
    }
    if (strstr(url, "feeds.test/"))
    {
        __atomic_add_fetch(&h_feed_gets, 1, __ATOMIC_SEQ_CST);
        if (h_html_feed || strstr(url, "page.html")) { snprintf(out, cap, "<!DOCTYPE html><html><head><title>A blog</title></head><body>Hi</body></html>"); return 200; }
        if (strstr(url, "missing")) return 404;
        if (strstr(url, "atom.xml"))
        {
            snprintf(out, cap, "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Atom Cast</title>"
                     "<author><name>Ada</name></author>"
                     "<entry><title type=\"html\">First &amp;amp; best</title><id>urn:a:1</id><published>2026-10-20T12:00:00+02:00</published>"
                     "<link rel=\"alternate\" href=\"https://atom.test/1\"/><link rel=\"enclosure\" type=\"audio/mpeg\" length=\"5000\" href=\"https://cdn.test/atom/1.mp3\"/></entry>"
                     "</feed>");
            return 200;
        }
        const char *show = strstr(url, "history") ? "history" : strstr(url, "hebrew") ? "hebrew" : "night";
        h_rss(out, cap, show, !strcmp(show, "history") ? h_feed_n : 12);
        return 200;
    }
    if (strstr(url, "art.test/"))
    {
        __atomic_add_fetch(&h_art_gets, 1, __ATOMIC_SEQ_CST);
        if (strstr(url, "3000x3000")) return 404;   /* a feed's own huge picture: not found here */
        if (sc_pod_jpg && sc_pod_n <= cap) { memcpy(out, sc_pod_jpg, sc_pod_n); sl_net_http_fake_len = sc_pod_n; return 200; }
        h_make_png();
        if (h_png_n > cap) return -1;
        memcpy(out, h_png, h_png_n);
        sl_net_http_fake_len = h_png_n;
        return 200;
    }
    return -1;
}
/* downloads: each poll adds a slice; "gone" is not on the web; a byte's value says where it is in the file */
static long long h_dl_size(const char *url)
{
    const char *s = strstr(url, "cdn.test/");
    int n = 0;
    if (s && (s = strchr(s + 9, '/'))) n = atoi(s + 1);
    return 100000 + n * 1000;
}
static int h_dl_slice = 40000;
static bool h_dl_hold_after;                    /* the test stops it after a slice */
static int h_dl_fake(const char *url, const char *path, long long have)
{
    __atomic_add_fetch(&h_dl_polls, 1, __ATOMIC_SEQ_CST);
    if (strstr(url, "gone")) return 404;
    long long total = h_dl_size(url);
    if (have >= total) return 206;
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    long long n = total - have < h_dl_slice ? total - have : h_dl_slice;
    for (long long i = 0; i < n; i++) fputc((int)((have + i) * 7 % 251), f);
    fclose(f);
    return 0;
}
static bool h_file_pattern(const char *rb, long long size)
{
    char p[600]; snprintf(p, sizeof p, H_ROOT "%s", rb);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    long long i = 0; int c;
    while ((c = fgetc(f)) != EOF) { if (c != (int)(i * 7 % 251)) { fclose(f); return false; } i++; }
    fclose(f);
    return i == size;
}

/* ---- waiting on the worker */
static unsigned h_seq0;
static bool h_found_came(void) { return pod_st.found_seq != h_seq0; }
static bool h_prev_came(void) { return pod_st.preview_seq != h_seq0; }
static bool h_done_came(void) { return pod_st.done_seq != h_seq0; }
static bool h_dl_idle(void) { struct sl_pod_status s; sl_pod_status(&s); return !s.dl_show && !s.queued; }
static bool h_worker_idle(void) { struct sl_pod_status s; sl_pod_status(&s); return !s.job && !s.jobs && !s.dl_show && !s.queued; }

static void h_pod_reset(void)
{
    sl_pod_test_forget();
    assert(!system("rm -rf " H_ROOT "/.rockbox/sloppy/podcasts " H_ROOT H_POD " " H_ROOT "/Card/*.opml"));
    sl_pod_test_forget();
    memset(&pod_st, 0, sizeof pod_st);
    sl_pod_status(&pod_st);
    sl_pod_eps_free(&pod_eps);
    memset(&pod_sh, 0, sizeof pod_sh);
}
static void h_pod_setup(void)
{
    snprintf(sl_pod_folder, sizeof sl_pod_folder, "%s", H_POD);
    sl_pod_idle_ms = 1500;
    if (system("mkdir -p " H_ROOT "/Card")) {}
    sl_net_http_fake = h_fake;
    sl_net_dl_fake = h_dl_fake;
    sim_mkdir(FAKE_CARD);
}
static lv_obj_t *h_top(void) { return pages[npages - 1]; }
static void h_click(lv_obj_t *o) { assert(o); lv_obj_send_event(o, LV_EVENT_CLICKED, NULL); frame(80); }
/* the row whose label is s (any case: Zule's are capitals), anywhere under o */
static lv_obj_t *h_label_ci(lv_obj_t *o, const char *s)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcasecmp(lv_label_get_text(o), s)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) { lv_obj_t *f = h_label_ci(lv_obj_get_child(o, i), s); if (f) return f; }
    return NULL;
}
static bool h_text_ci(lv_obj_t *o, const char *s)
{
    if (lv_obj_check_type(o, &lv_label_class) && strcasestr(lv_label_get_text(o), s)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) if (h_text_ci(lv_obj_get_child(o, i), s)) return true;
    return false;
}
static lv_obj_t *h_row(lv_obj_t *o, const char *s)
{
    lv_obj_t *l = h_label_ci(o, s);
    return l ? lv_obj_get_parent(l) : NULL;
}

/* a show followed through the directory, as a person would: search, look, follow */
static uint32_t h_follow_history(void)
{
    while (npages > 1) { page_pop(); frame(300); }
    podcasts_page(); frame(300);
    pod_find_page(); frame(300);
    lv_textarea_set_text(pod_find_ta, "history");
    h_seq0 = pod_st.found_seq;
    pod_find_go_cb(NULL);
    h_wait(h_found_came, 8000);
    assert(pod_found_obj && h_top() == pod_found_obj && pod_nfound == 2);   /* the one with no feed is left out */
    assert(h_label_ci(pod_found_obj, "Long History & Co") && h_text_ci(pod_found_obj, "Mira Sol & Friends"));
    int feeds0 = h_feed_gets;
    h_seq0 = pod_st.preview_seq;
    h_click(h_row(pod_found_obj, "Long History & Co"));
    /* "Reading the feed" (by its mark: Arabic is shaped on the label) */
    assert(pod_prev_obj && h_top() == pod_prev_obj && pod_want_prev && lv_obj_get_user_data(lv_obj_get_child(pod_prev_body, -1)) == (void *)1);
    h_wait(h_prev_came, 8000);
    char n48[48];
    sl_count(n48, sizeof n48, 48, "%s EPISODE", "%s EPISODES");
    assert(h_feed_gets == feeds0 + 1 && (global_settings.sloppy_language == 5 || h_text_ci(pod_prev_obj, n48)) && (sc_on || h_text_ci(pod_prev_obj, "Episode 48: The Quiet")));   /* showcase: real names */
    assert(lv_obj_get_user_data(lv_obj_get_child(pod_prev_body, -1)) != (void *)1);
    h_seq0 = pod_st.done_seq;
    pod_follow_found_cb(NULL);
    h_wait(h_done_came, 8000);
    h_wait(h_worker_idle, 8000);
    assert(h_feed_gets == feeds0 + 1);         /* the preview's read was enough */
    uint32_t id = sl_pod_feed_id("https://feeds.test/history.xml");
    assert(pod_show_obj && h_top() == pod_show_obj && pod_sh.id == id);
    return id;
}

static void h_pod_tests(void)
{
    struct mp3entry track0 = test_track;  /* the suite's track, as the screens after these expect it */
    int status0 = test_status;
    h_pod_setup();
    h_pod_reset();
    /* ---- the pieces */
    assert(sl_pod_rfc822("Tue, 21 Oct 2026 10:00:00 +0000") == 1792576800u);
    assert(sl_pod_rfc822("Tue, 21 Oct 2026 06:00:00 EDT") == 1792576800u);
    assert(sl_pod_rfc822("21 Oct 2026 11:00:00 +0100") == 1792576800u);
    assert(sl_pod_rfc822("Tue, 21 Oct 26 10:00:00 GMT") == 1792576800u);
    assert(sl_pod_rfc822("2026-10-21T12:00:00+02:00") == 1792576800u && sl_pod_rfc822("2026-10-21T10:00:00Z") == 1792576800u);
    assert(sl_pod_rfc822("2026-10-21T10:00:00.250Z") == 1792576800u && sl_pod_rfc822("2024-03-01") == 1709251200u);
    assert(!sl_pod_rfc822("yesterday") && !sl_pod_rfc822("") && !sl_pod_rfc822(NULL));
    assert(sl_pod_duration("1:02:03") == 3723 && sl_pod_duration("62:03") == 3723 && sl_pod_duration("3723") == 3723 && !sl_pod_duration("soon"));
    char b[400];
    sl_pod_file_name(b, sizeof b, 1792576800u, "What? A/B: \"test\"  ", "https://x.test/y/ep.m4a?z=1.mp3");
    assert(!strcmp(b, "2026-10-21 What A B test.m4a"));
    sl_pod_file_name(b, sizeof b, 0, "", "https://x.test/play?id=7");
    assert(!strcmp(b, "Episode.mp3"));
    sl_pod_clean_name(b, 12, "\xd7\xa9\xd7\x99\xd7\x97\xd7\x95\xd7\xaa ...");   /* never half a letter, no dots at the end */
    assert(!strcmp(b, "\xd7\xa9\xd7\x99\xd7\x97\xd7\x95\xd7\xaa"));
    assert(sl_pod_feed_id("https://Feeds.test/x.xml/") == sl_pod_feed_id("http://feeds.test/x.xml"));
    /* the parser: the same items however the bytes arrive (a byte at a time too) */
    static char rss[300000];
    h_rss(rss, sizeof rss, "history", 48);
    struct sl_pod_show ws, bs;
    struct sl_pod_eps whole, bytes;
    assert(sl_pod_test_parse(rss, strlen(rss), &ws, &whole) && sl_pod_test_parse(rss, 1, &bs, &bytes));
    assert(whole.n == 48 && bytes.n == 48 && !strcmp(ws.title, "Long History & Co") && !strcmp(bs.title, ws.title));
    assert(!strcmp(ws.author, "Mira Sol & Friends") && !strcmp(ws.image, "https://art.test/history/3000x3000.png"));
    for (int i = 0; i < 48; i++)
        assert(whole.ep[i].key == bytes.ep[i].key && !strcmp(whole.ep[i].title, bytes.ep[i].title) && !strcmp(whole.ep[i].url, bytes.ep[i].url));
    assert(!strcmp(whole.ep[0].title, h_long_title) && whole.ep[0].date == 1792576800u && whole.ep[0].dur == 3723 && whole.ep[0].bytes == 148000);
    assert(!strcmp(whole.ep[1].title, "Ep. 47: Caf\xc3\xa9 <live> \xe2\x80\x94 history") && whole.ep[1].dur == 2400);
    assert(!strcmp(whole.ep[0].url, "https://cdn.test/history/48.mp3?src=feed") && whole.ep[0].key == sl_pod_test_key("history-48"));
    sl_pod_eps_free(&whole); sl_pod_eps_free(&bytes);
    {   /* Atom, and a web page that is no feed */
        static char out[4096]; h_fake("https://feeds.test/atom.xml", out, sizeof out, NULL);
        struct sl_pod_show s; struct sl_pod_eps e;
        assert(sl_pod_test_parse(out, 5, &s, &e) && e.n == 1 && !strcmp(s.title, "Atom Cast") && !strcmp(s.author, "Ada"));
        assert(!strcmp(e.ep[0].url, "https://cdn.test/atom/1.mp3") && e.ep[0].date == 1792576800u - 24 * 3600 && !strcmp(e.ep[0].title, "First &amp; best"));
        sl_pod_eps_free(&e);
        h_fake("https://feeds.test/page.html", out, sizeof out, NULL);
        assert(!sl_pod_test_parse(out, 64, &s, &e) && e.n == 0);
        sl_pod_eps_free(&e);
    }
    {   /* the directory's answer: braces inside strings, a result with no feed left out */
        static char out[8192]; h_fake("https://itunes.apple.com/search?term=history", out, sizeof out, NULL);
        struct sl_pod_found f[8];
        assert(sl_pod_test_search(out, f, 8) == 2 && !strcmp(f[1].title, "Night Shift") && f[0].count == 48 && !strcmp(f[0].genre, "History"));
        assert(!strcmp(f[0].art, "https://art.test/history/600x600bb.jpg"));
        sl_pod_test_art_smaller(f[0].art);
        assert(!strcmp(f[0].art, "https://art.test/history/300x300bb.jpg"));
    }
    printf("PASS: podcasts' pieces: feed dates (RFC 822 and ISO), lengths, file names, a feed read a byte at a time, Atom, the directory's answer.\n");

    /* ---- no shows: the empty page says what to do */
    bool conn0 = wf_st.connected;
    wf_st.connected = false; sl_pod_online(false);
    while (npages > 1) { page_pop(); frame(300); }
    podcasts_page(); frame(300);
    assert(pod_page_obj && has_text(pod_page_obj, "No podcasts yet.") && has_text(pod_page_obj, "Find a podcast") && has_text(pod_page_obj, "Import a list"));
    pod_find_page(); frame(300);
    lv_textarea_set_text(pod_find_ta, "history");
    int s0 = h_search_gets;
    pod_find_go_cb(NULL); frame(200);
    assert(h_search_gets == s0 && !pod_want_found);   /* no Wi-Fi: nothing asked */
    wf_st.connected = true; sl_pod_online(true);

    /* ---- found, looked at, followed: one read of the feed, the picture from the directory */
    uint32_t id = h_follow_history();
    struct sl_pod_show sh;
    assert(sl_pod_show(id, &sh) && !strcmp(sh.title, "Long History & Co") && !strcmp(sh.folder, "Long History & Co") && sh.n_eps == 48);
    assert(sh.n_unheard == 3 && sh.art && sh.tint);   /* the three newest are news; the rest of the years are not */
    assert(strstr(h_last_url, "art.test") || h_art_gets > 0);
    struct stat st;
    char rb[300];
    sl_pod_art_name(rb, sizeof rb, id, SL_POD_ART_BIG); assert(h_file(rb, &st) && st.st_size == 219 * 219 * 2 + 12);
    sl_pod_art_name(rb, sizeof rb, id, SL_POD_ART_ROW); assert(h_file(rb, &st) && st.st_size == 58 * 58 * 2 + 12);
    assert(h_has(SL_POD_STATE "/shows.tsv", "https://feeds.test/history.xml") && h_has(SL_POD_STATE "/shows.tsv", "https://art.test/history/600x600bb.jpg"));
    /* the show's page: forty rows a page, the long title whole, the unheard dotted */
    assert(pod_rows && lv_obj_get_child_count(pod_rows) == POD_PER_PAGE + 1);   /* and the pager */
    assert(has_text(pod_show_obj, "48 EPISODES") && has_text(pod_show_obj, "Older"));
    lv_obj_t *lr = h_row(pod_rows, h_long_title);
    assert(lr && lv_obj_get_height(lr) > SL_ROW_H);
    {
        lv_obj_t *t = find_label(pod_rows, h_long_title);
        lv_obj_update_layout(t);
        assert(lv_obj_get_height(t) >= 2 * lv_font_get_line_height(&ss_semibold_21));   /* wrapped, not cut to a line */
    }
    assert(has_text(pod_rows, "21 Oct \xc2\xb7 1 h 2 min"));
    /* ---- a download: the arrow, the part coming in slices, the file whole and in place */
    h_dl_slice = 40000;
    int k0 = 0;
    assert(pod_eps.ep[k0].key == sl_pod_test_key("history-48"));
    pod_dl_one(k0);
    frame(100);
    h_wait(h_dl_idle, 10000);
    pod_poll();
    char name[200], path[400];
    sl_pod_file_name(name, sizeof name, 1792576800u, h_long_title, "https://cdn.test/history/48.mp3?src=feed");
    snprintf(path, sizeof path, H_POD "/Long History & Co/%s", name);
    assert(h_file_pattern(path, 148000));
    snprintf(rb, sizeof rb, "%s.part", path); assert(!h_file(rb, &st));
    for (int i = 0; i < 20; i++) frame(50);
    assert(pod_eps.ep[0].dl == SL_POD_DL_DONE && has_text(pod_rows, "On the card"));
    assert(sl_pod_show(id, &sh) && sh.n_dl == 1);
    /* ---- USB mode half way: wget stops at once, the part stays, and it carries on from there after */
    h_dl_slice = 20000;
    int k1 = 3;
    uint32_t key1 = pod_eps.ep[k1].key;
    assert(sl_pod_download(id, &key1, 1) == 1);
    uint64_t t0 = h_ms();
    while (h_ms() - t0 < 5000) { pod_poll(); if (pod_st.dl_key == key1 && pod_st.dl_got >= 40000) break; frame(40); usleep(8000); }
    sl_pod_hold(true);
    int polls = h_dl_polls;
    char name1[200], part1[400];
    sl_pod_file_name(name1, sizeof name1, pod_eps.ep[k1].date, pod_eps.ep[k1].title, pod_eps.ep[k1].url);
    snprintf(part1, sizeof part1, H_POD "/Long History & Co/%s.part", name1);
    assert(h_file(part1, &st) && st.st_size >= 40000 && st.st_size < h_dl_size(pod_eps.ep[k1].url));
    off_t held = st.st_size;
    for (int i = 0; i < 20; i++) { frame(40); usleep(8000); }
    assert(h_dl_polls == polls && h_file(part1, &st) && st.st_size == held);   /* nothing touched the card */
    pod_poll();
    assert(pod_st.hold && !pod_st.dl_show && pod_st.queued == 1);
    sl_pod_hold(false);
    h_wait(h_dl_idle, 10000);
    snprintf(path, sizeof path, H_POD "/Long History & Co/%s", name1);
    assert(h_file_pattern(path, h_dl_size(pod_eps.ep[k1].url)) && !h_file(part1, &st));
    printf("podcasts: a download stopped by USB mode at %lld bytes carried on from there, the file whole\n", (long long)held);
    /* gone from the web: not tried again */
    {
        for (int i = 0; i < 10; i++) frame(50);
        pod_poll();
        h_dl_slice = 40000;
    }
    /* ---- picking several: Select all on the bar, then download, mark, delete */
    pod_reload(); frame(200);
    lv_obj_send_event(h_row(pod_rows, h_long_title), LV_EVENT_LONG_PRESSED, NULL); frame(300);
    assert(sel_kind_on(SEL_EPISODES) && sel_count == 1 && sel_has(0) && has_text(sel_bar, "1 episode"));
    sel_all_cb(NULL); frame(200);
    assert(sel_count == 48 && has_text(sel_bar, "48 episodes") && has_text(sel_bar, "Deselect all"));
    sel_all_cb(NULL); frame(200);
    assert(!sel_on());
    lv_obj_send_event(h_row(pod_rows, h_long_title), LV_EVENT_LONG_PRESSED, NULL); frame(300);
    for (int i = 4; i < 7; i++) assert(sel_toggle(i));
    assert(sel_count == 4);
    pod_sel_act(1); frame(300);                 /* download: the long one is on the card already */
    h_wait(h_dl_idle, 15000);
    for (int i = 0; i < 10; i++) frame(50);
    assert(sl_pod_show(id, &sh) && sh.n_dl == 5);
    lv_obj_send_event(h_row(pod_rows, h_long_title), LV_EVENT_LONG_PRESSED, NULL); frame(300);
    for (int i = 1; i < 7; i++) if (i != 3) sel_toggle(i);
    pod_sel_act(3); frame(300);                 /* heard */
    for (int i = 0; i < 10; i++) frame(50);
    assert(sl_pod_show(id, &sh) && sh.n_unheard == 0);
    lv_obj_send_event(h_row(pod_rows, "Episode 44 of history"), LV_EVENT_LONG_PRESSED, NULL); frame(300);
    sel_toggle(5);
    assert(sel_count == 2);
    pod_sel_act(2); frame(300);                 /* their downloads go */
    h_wait(h_worker_idle, 8000);
    for (int i = 0; i < 10; i++) frame(50);
    assert(sl_pod_show(id, &sh) && sh.n_dl == 3 && pod_eps.ep[4].dl == SL_POD_DL_NONE && pod_eps.ep[5].dl == SL_POD_DL_NONE);
    printf("PASS: podcasts: followed from the directory (one feed read), the picture made, full titles, a download in slices, "
           "stopped for USB mode and carried on, several picked: downloaded, marked heard, deleted.\n");
    /* ---- listening: where it was, its show's speed, back a little after a pause, heard at the end */
    uint32_t key0 = pod_eps.ep[0].key;
    assert(sl_pod_mark(id, &key0, 1, false));
    pod_reload(); frame(100);
    int sp0 = (int)test_speed, rw0 = global_settings.sloppy_book_rewind;
    global_settings.sloppy_book_rewind = 1;
    pod_speed_chip(3);                          /* 1.5x */
    assert(sl_pod_show(id, &sh) && sh.speed == 150);
    started = -1;
    h_click(h_row(pod_rows, h_long_title));     /* on the card: it plays */
    assert(started == 0 && last_start_elapsed == 0);
    char ep_path[400];
    assert(sl_pod_ep_path(id, &pod_eps.ep[0], ep_path, sizeof ep_path) && sl_pod_is_episode(ep_path));
    strlcpy(test_track.path, ep_path, sizeof test_track.path);
    test_track.length = 3723000; test_track.elapsed = 600000; test_status = AUDIO_STATUS_PLAY;
    frame(300); pod_poll();
    assert(pod_np_show == id && pod_np_key == key0 && test_speed == 150 * PITCH_SPEED_PRECISION);
    assert(sl_book_now());                      /* the keys skip 30 s, as in a book */
    test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE; sl_paused_at = sl_ms() - 70000;
    pod_poll(); frame(100);
    assert(h_has(SL_POD_STATE "/progress.tsv", "\t600000\t"));   /* on the card at the pause */
    test_track.elapsed = 600000;
    toggle_play();                              /* back after seventy seconds: ten seconds back */
    assert(test_track.elapsed == 590000 && !(test_status & AUDIO_STATUS_PAUSE));
    /* Continue listening on the Podcasts page */
    page_bring_back(pod_page_obj); frame(400);
    pod_refill(pod_page_body, pod_page_fill); frame(200);
    assert(has_text(pod_page_obj, "CONTINUE LISTENING") || has_text(pod_page_obj, "Continue listening"));
    assert(has_text(pod_page_obj, "Episode 48: The Quiet"));
    /* music again: its own speed */
    strlcpy(test_track.path, "/Music/song.mp3", sizeof test_track.path);
    pod_poll();
    assert(!pod_np_show && test_speed == sp0);
    /* heard to its last minute: heard, and gone from Continue listening */
    strlcpy(test_track.path, ep_path, sizeof test_track.path);
    pod_poll();
    test_track.elapsed = 3700000; pod_poll();
    test_status = 0; pod_poll(); frame(100);
    struct sl_pod_progress pr;
    assert(!sl_pod_progress_get(id, key0, &pr) && sl_pod_show(id, &sh) && sh.n_unheard == 0);
    /* played from the Continue listening row: where it was left */
    test_status = AUDIO_STATUS_PLAY; test_track.elapsed = 1200000; pod_poll(); sl_pod_progress_flush();
    test_status = 0; test_track.elapsed = 1200000;
    strlcpy(test_track.path, "/Music/song.mp3", sizeof test_track.path); pod_poll();
    global_settings.sloppy_book_rewind = 0;
    struct sl_pod_progress cont;
    assert(sl_pod_progress_list(&cont, 1) == 1 && cont.pos_ms == 1200000);
    started = -1; pod_play_progress(&cont);
    assert(started == 0 && last_start_elapsed == 1200000);
    global_settings.sloppy_book_rewind = rw0;
    test_status = 0;
    printf("PASS: podcasts: an episode plays from the card where it was left, at its show's 1.5x, 10 s back after a minute's pause, "
           "music gets its speed back, heard at the end, Continue listening.\n");
    /* ---- OPML out and in */
    char opml[128];
    assert(sl_pod_opml_export(opml, sizeof opml) && !strcmp(opml, H_POD "/Podcasts.opml"));
    assert(h_has(H_POD "/Podcasts.opml", "xmlUrl=\"https://feeds.test/history.xml\"") && h_has(H_POD "/Podcasts.opml", "text=\"Long History &amp; Co\""));
    {   /* another app's list at the card's top: two shows, one followed already */
        FILE *f = fopen(H_ROOT "/Card/other-app.opml", "w"); assert(f);
        fprintf(f, "<?xml version=\"1.0\"?><opml version=\"1.0\"><body><outline text=\"Feeds\">"
                   "<outline type=\"rss\" text=\"Night Shift\" xmlUrl=\"https://feeds.test/night.xml\"/>"
                   "<outline type=\"rss\" text=\"Long History\" xmlUrl=\"http://feeds.test/history.xml/\"/>"
                   "<outline type=\"rss\" text=\"Gone\" xmlUrl=\"https://feeds.test/missing.xml\"/>"
                   "</outline></body></opml>\n");
        fclose(f);
    }
    snprintf(sl_pod_folder, sizeof sl_pod_folder, "%s", H_POD);
    char names[8][96];
    int nopml = sl_pod_opml_list(names, 8);
    assert(nopml == 2);
    h_seq0 = pod_st.done_seq;
    assert(sl_pod_opml_import("/Card/other-app.opml"));
    pod_want_follow = true;
    h_wait(h_done_came, 10000);
    h_wait(h_worker_idle, 10000);
    assert(pod_st.done_job == SL_POD_JOB_OPML && pod_st.done_ok && pod_st.done_count == 1);
    struct sl_pod_show all[4];
    assert(sl_pod_shows(all, 4) == 2);
    printf("PASS: podcasts: OPML written with every feed; another app's list followed once each, a dead feed skipped.\n");
    /* ---- by address: not a feed, then a feed */
    pod_url_page(); frame(300);
    lv_textarea_set_text(pod_url_ta, "feeds.test/page.html");
    h_seq0 = pod_st.done_seq;
    pod_url_go_cb(NULL);
    h_wait(h_done_came, 8000);
    assert(!pod_st.done_ok && !strcmp(pod_st.why, "That address is not a podcast feed") && e_note_has("That address is not a podcast feed"));
    lv_textarea_set_text(pod_url_ta, "https://feeds.test/atom.xml");
    h_seq0 = pod_st.done_seq;
    pod_url_go_cb(NULL);
    h_wait(h_done_came, 8000);
    h_wait(h_worker_idle, 8000);
    assert(pod_st.done_ok && sl_pod_following("https://feeds.test/atom.xml") && pod_show_obj && !strcmp(pod_sh.title, "Atom Cast"));
    /* ---- a server that hangs: the interface keeps its pace */
    h_slow_ms = 2500;
    assert(sl_pod_refresh(0));
    uint64_t worst = 0, w0 = h_ms();
    while (h_ms() - w0 < 1500) { uint64_t f0 = h_ms(); frame(16); if (h_ms() - f0 > worst) worst = h_ms() - f0; usleep(4000); }
    printf("podcasts: a feed that takes 2.5 s: worst UI frame %u ms meanwhile\n", (unsigned)worst);
    assert(worst < 100);
    h_slow_ms = 0;
    h_wait(h_worker_idle, 15000);
    /* ---- memory: what the pages hold while open, and what stays after (the R1 has 4-5 MB free) */
    {
        while (npages > 1) { page_pop(); frame(300); }
        h_wait(h_worker_idle, 8000);
        for (int i = 0; i < 10; i++) frame(100);
        malloc_statistics_t m0, m1, m2, m3;
        malloc_zone_statistics(NULL, &m0);
        podcasts_page(); frame(400);
        malloc_zone_statistics(NULL, &m1);
        pod_show_page(id); frame(400);
        malloc_zone_statistics(NULL, &m2);
        while (npages > 1) { page_pop(); frame(300); }
        for (int i = 0; i < 10; i++) frame(100);
        malloc_zone_statistics(NULL, &m3);
        long grid = ((long)m1.size_in_use - (long)m0.size_in_use) / 1024, show = ((long)m2.size_in_use - (long)m1.size_in_use) / 1024;
        long after = ((long)m3.size_in_use - (long)m0.size_in_use) / 1024;
        printf("podcasts: memory: the worker's state %u KB always; the Podcasts page (3 shows, art) %ld KB, a show's page (48 episodes) %ld KB more; "
               "%ld KB left after both close\n", (unsigned)(sl_pod_test_static_bytes() / 1024), grid, show, after);
#if !__has_feature(address_sanitizer)
        assert(after < 48);
#endif
    }
    /* ---- unfollow: keep the files, or not */
    while (npages > 1) { page_pop(); frame(300); }
    podcasts_page(); frame(300);
    pod_show_page(id); frame(300);
    pod_unfollow_cb(NULL); frame(300);
    assert(has_text(h_top(), "Unfollow and delete") && has_text(h_top(), "Keep following"));
    int dls = sh.n_dl;
    assert(dls == 3);
    pod_unfollow_all_cb(NULL); frame(400);
    if (h_file(H_POD "/Long History & Co", &st)) { if (system("ls -la '" H_ROOT H_POD "/Long History & Co'")) {} }
    assert(!sl_pod_show(id, NULL));
    assert(!h_file(H_POD "/Long History & Co", &st));
    assert(h_top() == pod_page_obj);
    sl_pod_art_name(rb, sizeof rb, id, SL_POD_ART_BIG); assert(!h_file(rb, &st));
    printf("PASS: podcasts: by address (a web page refused, Atom followed), a slow server never holds the UI, unfollowed with its files.\n");
    while (npages > 1) { page_pop(); frame(300); }
    if (np_open) { np_hide(0); frame(500); }
    test_track = track0; test_status = status0;
    wf_st.connected = conn0; sl_pod_online(conn0);
    sl_net_http_fake = NULL; sl_net_dl_fake = NULL;
}

/* ---- the screens, for the audit and the shots */
/* what a podcast screen changed, back as the next screen expects it: Wi-Fi, the fakes, picking, the sheet */
static bool h_au_dirty, h_au_conn0, h_track_saved;
static struct mp3entry h_track0;
static void h_au_reset(void)
{
    if (h_track_saved) { if (np_open) { np_hide(0); frame(400); } test_track = h_track0; h_track_saved = false; }
    if (!h_au_dirty) return;
    h_au_dirty = false;
    if (sel_on()) sel_end();
    if (acts_open_now) { acts_close(); frame(300); }
    for (int i = 0; i < 100 && sl_pod_busy(); i++) { pod_poll(); frame(20); usleep(5000); }   /* nothing of ours still running */
    wf_st.connected = h_au_conn0; sl_pod_online(h_au_conn0);
    sl_net_http_fake = NULL; sl_net_dl_fake = NULL;
}
static void h_au_pod_state(int which)
{
    if (!h_au_dirty) { h_au_dirty = true; h_au_conn0 = wf_st.connected; }
    h_pod_setup();
    static bool made;
    if (!made || which == 0)
    {
        h_pod_reset();
        made = false;
    }
    wf_st.connected = true; sl_pod_online(true);
    if (which && !made)
    {
        /* two shows: one with its picture and three on the card, one with none */
        h_dl_slice = 1000000;
        uint32_t id = h_follow_history();
        while (npages > 1) { page_pop(); frame(300); }
        assert(sl_pod_follow("https://feeds.test/hebrew.xml", NULL, NULL, NULL));
        h_wait(h_worker_idle, 8000);
        struct sl_pod_eps e;
        assert(sl_pod_eps_load(id, &e));
        uint32_t keys[3] = { e.ep[0].key, e.ep[1].key, e.ep[4].key };
        sl_pod_eps_free(&e);
        sl_pod_download(id, keys, 3);
        h_wait(h_dl_idle, 8000);
        sl_pod_progress_set(id, keys[1], 1500000, 2710000, "", "Ep. 47: Caf\xc3\xa9 <live> \xe2\x80\x94 history");
        sl_pod_progress_flush();
        made = true;
    }
    while (npages > 1) { page_pop(); frame(300); }
    pod_poll();
}
static void h_au_pod(int which)
{
    h_au_pod_state(which);
    uint32_t id = sl_pod_feed_id("https://feeds.test/history.xml");
    switch (which)
    {
        case 0: podcasts_page(); break;
        case 1: podcasts_page(); break;
        case 2: podcasts_page(); frame(300); pod_show_page(id); break;
        case 3: podcasts_page(); frame(300); pod_show_page(id); frame(300);
                lv_obj_send_event(lv_obj_get_child(pod_rows, 0), LV_EVENT_LONG_PRESSED, NULL); frame(200);   /* the newest; its title may end in dots at Largest */
                assert(sel_kind_on(SEL_EPISODES)); sel_toggle(2); sel_toggle(4); break;
        case 4: podcasts_page(); frame(300); pod_show_page(id); frame(300); pod_ep_sheet(1); break;
        case 5: pod_find_page(); lv_textarea_set_text(pod_find_ta, "history"); break;
        case 6: pod_find_page(); frame(200); lv_textarea_set_text(pod_find_ta, "history");
                h_seq0 = pod_st.found_seq; pod_find_go_cb(NULL); h_wait(h_found_came, 5000); break;
        case 7: pod_find_page(); frame(200); lv_textarea_set_text(pod_find_ta, "night");
                h_seq0 = pod_st.found_seq; pod_find_go_cb(NULL); h_wait(h_found_came, 5000); frame(200);
                h_seq0 = pod_st.preview_seq; h_click(h_row(pod_found_obj, "Night Shift")); h_wait(h_prev_came, 5000); break;
        case 8: pod_url_page(); break;
        case 9: pod_opml_page(); break;
        case 10: pod_downloads_page(); break;
        case 11: podcasts_page(); frame(300); pod_show_page(id); frame(300); pod_unfollow_cb(NULL); break;
        case 12: podcasts_page(); frame(300); pod_show_page(sl_pod_feed_id("https://feeds.test/hebrew.xml")); break;
        case 13: pod_find_page(); frame(200); lv_textarea_set_text(pod_find_ta, "nothing");
                 h_seq0 = pod_st.found_seq; pod_find_go_cb(NULL); h_wait(h_found_came, 5000); break;
        case 14:   /* an episode playing: Now Playing's skip buttons move 30 seconds */
        {
            podcasts_page(); frame(300); pod_show_page(id); frame(300);
            int k = 0;
            while (k < pod_eps.n && pod_eps.ep[k].dl != SL_POD_DL_DONE) k++;
            if (k < pod_eps.n)
            {
                h_track0 = test_track; h_track_saved = true;
                sl_pod_ep_path(id, &pod_eps.ep[k], test_track.path, sizeof test_track.path);
                static char title[160];
                strlcpy(title, pod_eps.ep[k].title, sizeof title);
                test_track.title = title;
                test_track.length = 3723000; test_track.elapsed = 1500000;
                pod_play_eps(&k, 1); frame(500);
            }
            break;
        }
        case 15: music_page(); frame(300); lv_obj_scroll_to_y(lv_obj_get_child(h_top(), 1), LV_COORD_MAX, LV_ANIM_OFF); break;
    }
}
static void h_shot(const char *dir, const char *name, const char *look, const char *tag)
{
    char p[600]; frame(0); snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, name, look, tag); shot_now(p);
}
static void h_shots(const char *dir, const char *tag)
{
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); if (system(p)) {}
    const char *only = getenv("SLOPPY_AUDIT_ONLY");
    au_library();
    static const char *const names[] = { "podcasts-empty", "podcasts", "podcast-show", "podcast-select", "podcast-sheet",
                                         "podcast-find", "podcast-results", "podcast-preview", "podcast-address", "podcast-opml",
                                         "podcast-downloads", "podcast-unfollow", "podcast-show-hebrew", "podcast-results-none",
                                         "podcast-now-playing", "music-podcasts" };
    for (int look = 0; look < 4; look++)
    {
        static const char *const lk[] = { "dark", "light", "zule", "tiles" };
        for (int w = 0; w < (int)(sizeof names / sizeof names[0]); w++)
        {
            if (only && !strstr(names[w], only) && strcmp(only, "podcasts")) continue;
            /* cards dark, cards light, Zule, and Settings as tiles */
            au_apply(look == 2 ? 6 : look == 3 ? 0 : look, 0, 0);
            int tiles0 = global_settings.sloppy_settings_tiles;
            if (look == 3) global_settings.sloppy_settings_tiles = 1;
            global_settings.sloppy_notifications = 0;
            h_au_pod(w);
            au_settle();
            h_shot(dir, names[w], lk[look], tag);
            if (w == 2 || w == 1)
            {
                /* the rest of it */
                lv_obj_t *b = lv_obj_get_child(h_top(), 1);
                lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF); au_settle();
                char nm[64]; snprintf(nm, sizeof nm, "%s-end", names[w]);
                h_shot(dir, nm, lk[look], tag);
            }
            if (sel_on()) sel_end();
            if (acts_open_now) acts_close();
            frame(300);
            while (npages > 1) { page_pop(); frame(300); }
            global_settings.sloppy_settings_tiles = tiles0;
            au_reset();
            test_status = 0;
        }
    }
}
static void h_tests(void)
{
    h_pod_tests();
}
