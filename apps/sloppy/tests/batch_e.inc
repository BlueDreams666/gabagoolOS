/* Gate 1 batch E: online. The status bar's codec colour, radio sections and
 * its empty states, the transfer page's QR code and uploads, LocalSend,
 * Last.fm, batch lyrics and the cover fetcher. Included from ui_test.c
 * after ui_audit.inc, whose helpers it uses.
 *
 *   e_tests()                         in the suite
 *   SLOPPY_E_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after   the screenshots */

/* headphones on the line with codec, the volume on them or not */
static void e_bt_connect(const char *codec, bool volume_on_bt) {
#ifdef SL_BT_DEMO_CODEC
    strlcpy(sl_bt_demo_codec, codec, sizeof sl_bt_demo_codec);
    sl_bt_demo_codec_n = 5;
#else
    (void)codec;
#endif
    sl_bt_demo_on = true; sl_bt_demo_index = 0;
    sl_bt_begin(SL_BT_JOB_REFRESH, ""); for (int i = 0; i < 10; i++) frame(100);
    global_status.sloppy_volume_on_bt = volume_on_bt ? 1 : 0;
    statusbar_retheme(); statusbar_update(); frame(100);
}
static void e_bt_done(void) {
    sl_bt_demo_index = -1; global_status.sloppy_volume_on_bt = 0;
    sl_bt_begin(SL_BT_JOB_REFRESH, ""); for (int i = 0; i < 10; i++) frame(100);
#ifdef SL_BT_DEMO_CODEC
    strlcpy(sl_bt_demo_codec, "AAC", sizeof sl_bt_demo_codec); sl_bt_demo_codec_n = 2;
#endif
    statusbar_update(); frame(100);
}

static void e_codec_tests(void) {
#ifdef SL_BT_DEMO_CODEC
    int th0 = global_settings.sloppy_theme;
    global_settings.sloppy_theme = 0; appearance_reload((void *)2); frame(700);
    while (npages > 1) { page_pop(); frame(300); }
    /* over the dark home: LDAC is gold, on the volume's glyph and nowhere else */
    e_bt_connect("LDAC", true);
    assert(!strncmp(lv_label_get_text(sb_vol), LV_SYMBOL_BLUETOOTH, strlen(LV_SYMBOL_BLUETOOTH)));
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_vol, 0), lv_color_hex(bt_codec_tint("LDAC", false))));
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_vol_n, 0), lv_obj_get_style_text_color(sb_clock, 0)));
    assert(lv_obj_has_flag(sb_bt, LV_OBJ_FLAG_HIDDEN));   /* the volume already says Bluetooth */
    /* the level sits a space after its glyph, as when it was one label */
    lv_obj_update_layout(sb);
    int32_t space = lv_text_get_width(" ", 1, &ss_semibold_21, 0);
    assert(lv_obj_get_x(sb_vol_n) == lv_obj_get_x(sb_vol) + lv_obj_get_width(sb_vol) + space);
    assert(lv_obj_get_y(sb_vol_n) == lv_obj_get_y(sb_vol));
    /* every codec its own colour; SBC keeps the ink */
    static const char *const names[] = { "LDAC", "aptX-HD", "aptX Adaptive", "aptX", "AAC" };
    for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++)
        if (i != j && !(i == 1 && j == 2) && !(i == 2 && j == 1)) assert(bt_codec_tint(names[i], false) != bt_codec_tint(names[j], false));
    assert(!bt_codec_tint("SBC", false) && !bt_codec_tint("", true) && !bt_codec_tint(NULL, true));
    e_bt_connect("SBC", true);
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_vol, 0), lv_obj_get_style_text_color(sb_clock, 0)));
    /* the volume on the jack: the glyph by the battery takes the colour */
    e_bt_connect("aptX-HD", false);
    assert(!lv_obj_has_flag(sb_bt, LV_OBJ_FLAG_HIDDEN));
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_bt, 0), lv_color_hex(bt_codec_tint("aptX-HD", false))));
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_vol, 0), lv_obj_get_style_text_color(sb_clock, 0)));
    lv_obj_update_layout(sb);
    assert(lv_obj_get_x(sb_bt) + lv_obj_get_width(sb_bt) + 10 == lv_obj_get_x(sb_batt));
    /* a light page: the deep tone, which reads on white */
    global_settings.sloppy_theme = 1; appearance_reload((void *)2); frame(700);
    sl_bt_page(); frame(500); statusbar_retheme(); statusbar_update(); frame(100);
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_bt, 0), lv_color_hex(bt_codec_tint("aptX-HD", true))));
    /* the codec changed from the Bluetooth page: the bar follows */
    lv_obj_t *row = lv_obj_get_parent(find_label(sl_bt_body, "LDAC"));
    assert(row); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < 10; i++) frame(100);
    statusbar_update(); frame(100);
    assert(!strcmp(sl_bt_state.codec, "LDAC"));
    assert(lv_color_eq(lv_obj_get_style_text_color(sb_bt, 0), lv_color_hex(bt_codec_tint("LDAC", true))));
    while (npages > 1) { page_pop(); frame(300); }
    e_bt_done();
    assert(lv_obj_has_flag(sb_bt, LV_OBJ_FLAG_HIDDEN));
    global_settings.sloppy_theme = th0; appearance_reload((void *)2); frame(700);
    printf("PASS: status bar: the Bluetooth glyph wears its codec (LDAC, aptX HD, aptX, AAC; SBC plain), light and dark, and follows a change.\n");
#endif
}

/* the pretend radio on and joined, or off; wf_st as its next job will say */
static void e_wifi(bool on) {
    for (int i = 0; i < 50 && wf_busy; i++) frame(100);
    sl_net_wifi_power(on);
    if (on) sl_net_wifi_connect("Home network", NULL);
    sl_net_wifi_status(&wf_st);
    global_settings.sloppy_wifi = on;
    wf_tick_update();
}
/* ---- radio: radio.txt's headings, lines written by hand, the empty states ---- */
extern const char *sl_net_demo_fail;
static const char e_radio_txt[] =
    "# my stations\n"
    "Morning Wire\thttp://example.org/wire.mp3\t\tNews\n"
    "\n[Rock]\n"
    "Riff City | http://example.org/riff\n"
    "  http://rock.example.net:8000/stream\n"
    "[Jazz]\r\n"
    "Blue Note FM\thttp://example.org/blue\t\tGB \xc2\xb7 jazz\n"
    "[]\n"
    "Late Talk\thttp://example.org/talk\n";
static void e_radio_file(void) {
    fake_put(ST_SAVED, e_radio_txt, sizeof e_radio_txt - 1);
    st_saved_read = false; st_nsaved = 0; st_saved_load();
}
static void e_radio_tests(void) {
#ifdef ST_GROUPS
    bool conn0 = wf_st.connected;
    e_radio_file();
    assert(st_nsaved == 5);
    assert(!strcmp(st_saved[0].name, "Morning Wire") && !st_saved[0].group[0] && !strcmp(st_saved[0].about, "News"));
    assert(!strcmp(st_saved[1].name, "Riff City") && !strcmp(st_saved[1].url, "http://example.org/riff") && !strcmp(st_saved[1].group, "Rock"));
    assert(!strcmp(st_saved[2].name, "rock.example.net") && !strcmp(st_saved[2].url, "http://rock.example.net:8000/stream"));
    assert(!strcmp(st_saved[3].group, "Jazz") && !strcmp(st_saved[3].about, "GB \xc2\xb7 jazz"));
    assert(!strcmp(st_saved[4].name, "Late Talk") && !st_saved[4].group[0]);
    /* the page: those under no heading as SAVED, then ROCK, then JAZZ */
    e_wifi(true); free(st_list); st_list = NULL; st_n = 0; st_kind = ST_TOP;
    webradio_page(); for (int i = 0; i < 30 && st_loading; i++) frame(100);
    lv_obj_t *sv = find_label(pages[npages - 1], "SAVED"), *rk = find_label(pages[npages - 1], "ROCK"), *jz = find_label(pages[npages - 1], "JAZZ");
    assert(sv && rk && jz);
    lv_obj_update_layout(pages[npages - 1]);
    lv_obj_t *late = find_label(pages[npages - 1], "Late Talk"), *riff = find_label(pages[npages - 1], "Riff City");
    assert(lv_obj_get_y(sv) < lv_obj_get_y(rk) && lv_obj_get_y(rk) < lv_obj_get_y(jz));
    lv_area_t a1, a2; lv_obj_get_coords(late, &a1); lv_obj_get_coords(riff, &a2); assert(a1.y1 < a2.y1);
    /* written back: the headings kept, a new station on top under none */
    st_now = st_saved[1]; strlcpy(st_now.url, "http://example.org/new", sizeof st_now.url); strlcpy(st_now.name, "New One", sizeof st_now.name);
    st_save_cb(NULL); frame(200);
    st_saved_read = false; st_nsaved = 0; st_saved_load();
    assert(st_nsaved == 6 && !strcmp(st_saved[0].name, "New One") && !st_saved[0].group[0]);
    assert(!strcmp(st_saved[2].name, "Late Talk") && !strcmp(st_saved[3].group, "Rock") && !strcmp(st_saved[5].group, "Jazz"));
    assert(fake_has(ST_SAVED, "\n[Rock]\nRiff City\t") && fake_has(ST_SAVED, "\n[Jazz]\nBlue Note FM\t"));
    while (npages > 1) { page_pop(); frame(300); }
    /* no Wi-Fi: the one empty state, with the way to Wi-Fi */
    e_wifi(false); free(st_list); st_list = NULL; st_n = 0;
    webradio_page(); frame(300);
    assert(find_label(pages[npages - 1], "Not connected to Wi-Fi") && sl_empty_last);
    lv_obj_t *go = find_label(pages[npages - 1], LV_SYMBOL_WIFI "  Wi-Fi");
    while (go && !lv_obj_has_flag(go, LV_OBJ_FLAG_CLICKABLE)) go = lv_obj_get_parent(go);
    assert(go); int n0 = npages; lv_obj_send_event(go, LV_EVENT_CLICKED, NULL); frame(400);
    assert(npages == n0 + 1 && pages[npages - 1] == wf_page_obj);
    while (npages > 1) { page_pop(); frame(300); }
    /* the directory not answering, then answering on Try again */
    for (int i = 0; i < 50 && wf_busy; i++) frame(100);   /* the Wi-Fi page's own job ends first */
    frame(300);
    e_wifi(true); sl_net_demo_fail = "radio-browser";
    webradio_page(); for (int i = 0; i < 30 && st_loading; i++) frame(100); frame(300);
    assert(st_failed && find_label(pages[npages - 1], "The station list didn't answer"));
    sl_net_demo_fail = NULL;
    lv_obj_t *again = find_label(pages[npages - 1], LV_SYMBOL_REFRESH "  Try again");
    while (again && !lv_obj_has_flag(again, LV_OBJ_FLAG_CLICKABLE)) again = lv_obj_get_parent(again);
    assert(again); lv_obj_send_event(again, LV_EVENT_CLICKED, NULL); for (int i = 0; i < 30 && st_loading; i++) frame(100); frame(300);
    assert(!st_failed && st_n == 4 && !find_label(pages[npages - 1], "The station list didn't answer"));
    /* a search with nothing back */
    strlcpy(st_query, "nothing", sizeof st_query); st_kind = ST_FOUND; st_load(ST_FIND_URL "nothing");
    for (int i = 0; i < 30 && st_loading; i++) frame(100); frame(300);
    assert(!st_n && find_label(pages[npages - 1], "No stations by that name"));
    while (npages > 1) { page_pop(); frame(300); }
    st_kind = ST_NEAR; free(st_list); st_list = NULL; st_n = 0;
    {   /* a line by hand with no name and no scheme: named by the address (it read NULL + 3 and crashed) */
        static const char bare[] = "\texample.org/live\n";
        fake_put(ST_SAVED, bare, sizeof bare - 1); st_saved_read = false; st_nsaved = 0; st_saved_load();
        assert(st_nsaved == 1 && !strcmp(st_saved[0].name, "example.org") && !strcmp(st_saved[0].url, "example.org/live"));
    }
    remove(ST_SAVED); st_saved_read = false; st_nsaved = 0;
    e_wifi(conn0);
    printf("PASS: internet radio: radio.txt's [headings] group saved stations and survive a save; lines by hand; empty states for no Wi-Fi, no answer (Try again) and no results.\n");
#endif
}

/* ---- music in over Wi-Fi: the transfer page's server and LocalSend, a
 * client built here from their specs (plain sockets, no dependencies) ---- */
#include <netinet/in.h>
#include <arpa/inet.h>
#define E_RX_DIR "/tmp/sloppy-ui-test-root/Music/Received"
static int e_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    assert(!connect(fd, (struct sockaddr *)&a, sizeof a));
    return fd;
}
static void e_sendall(int fd, const void *b, size_t n) { const char *p = b; while (n) { ssize_t w = send(fd, p, n, 0); assert(w > 0); p += w; n -= (size_t)w; } }
/* the answer: its status, its body into out */
static int e_answer(int fd, char *out, size_t cap) {
    static char r[200000]; size_t got = 0; ssize_t k;
    while (got + 1 < sizeof r && (k = recv(fd, r + got, sizeof r - 1 - got, 0)) > 0) got += (size_t)k;
    r[got] = '\0'; close(fd);
    char *sp = strchr(r, ' '), *body = strstr(r, "\r\n\r\n");
    if (out && cap) snprintf(out, cap, "%s", body ? body + 4 : "");
    return sp ? atoi(sp + 1) : -1;
}
static int e_req(int port, const char *method, const char *path, const void *body, size_t len, char *out, size_t cap) {
    int fd = e_connect(port); char h[2048];
    int n = snprintf(h, sizeof h, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", method, path, len);
    e_sendall(fd, h, (size_t)n); if (len) e_sendall(fd, body, len);
    return e_answer(fd, out, cap);
}
static bool e_file_is(const char *rel, const void *want, size_t n) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", E_RX_DIR, rel);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    static char b[300000]; size_t got = fread(b, 1, sizeof b, f); fclose(f);
    return got == n && !memcmp(b, want, n);
}
static bool e_exists(const char *rel) { char p[512]; snprintf(p, sizeof p, "%s/%s", E_RX_DIR, rel); return !access(p, F_OK); }
static int e_files_in(const char *dir) {   /* every file under dir */
    char c[600]; snprintf(c, sizeof c, "find '%s' -type f 2>/dev/null | wc -l", dir);
    FILE *p = popen(c, "r"); int n = 0; if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); } return n;
}
static void e_rx_frames(int n) { for (int i = 0; i < n; i++) { rx_poll(); frame(50); usleep(5000); } }
/* a request on its own thread: prepare-upload waits for the owner */
struct e_bg { int port; const char *method, *path; char body[8192]; size_t len; char out[8192]; volatile int status; volatile bool done; };
static void *e_bg_run(void *v) { struct e_bg *b = v; b->status = e_req(b->port, b->method, b->path, b->body, b->len, b->out, sizeof b->out); b->done = true; return NULL; }
static void e_bg_start(struct e_bg *b, pthread_t *t) { b->done = false; b->status = 0; assert(!pthread_create(t, NULL, e_bg_run, b)); }
/* SHA-256 of a buffer, the sender's way: the reference vector is checked first */
static void e_sha(const void *d, size_t n, char hex[65]) {   /* the system's shasum: the receiver must agree with it */
    FILE *f = fopen("/tmp/sloppy-e-sha.bin", "wb"); assert(f); if (n) fwrite(d, 1, n, f); fclose(f);
    FILE *p = popen("shasum -a 256 /tmp/sloppy-e-sha.bin", "r"); assert(p); assert(fscanf(p, "%64s", hex) == 1); pclose(p);
}
static bool e_json_str(const char *j, const char *key, char *out, size_t cap) {
    char k[96]; snprintf(k, sizeof k, "\"%s\":\"", key); const char *p = strstr(j, k); if (!p) return false;
    p += strlen(k); size_t n = strcspn(p, "\""); snprintf(out, cap, "%.*s", (int)n, p); return true;
}
/* one of the last few notifications says s */
static bool e_note_has(const char *w) {
    for (int i = 0; i < nh.n && i < 4; i++) if (strstr(nh.list[i].title, w) || strstr(nh.list[i].body, w)) return true;
    return false;
}
static uint64_t e_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

static void e_share_tests(void) {
    char out[8192], hex[65];
    /* the pieces: names, paths, the checksum */
    e_sha("abc", 3, hex); assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    static char big[200000]; for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + (i >> 9));
    assert(sl_share_is_music("a.FLAC") && sl_share_is_music("cover.jpg") && sl_share_is_music("x.cue") && !sl_share_is_music("notes.txt") && !sl_share_is_music("noext"));
    char cp[256];
    sl_share_clean_path(cp, sizeof cp, "../../etc/passwd"); assert(!strcmp(cp, "etc/passwd"));
    sl_share_clean_path(cp, sizeof cp, "/Album: Live?/01 \"One\".flac"); assert(!strcmp(cp, "Album_ Live_/01 _One_.flac"));
    sl_share_clean_path(cp, sizeof cp, "a\\b\\..\\c. /d.mp3"); assert(!strcmp(cp, "a/b/c/d.mp3"));
    sl_share_clean_path(cp, sizeof cp, "../.."); assert(!cp[0]);
    sl_share_clean_path(cp, sizeof cp, "Caf\xc3\xa9/\xd7\xa9\xd7\x99\xd7\xa8.mp3"); assert(!strcmp(cp, "Caf\xc3\xa9/\xd7\xa9\xd7\x99\xd7\xa8.mp3"));
    assert(!system("rm -rf '" E_RX_DIR "'"));
    bool conn0 = wf_st.connected; e_wifi(true);
    int th0 = global_settings.sloppy_theme;
    global_settings.sloppy_theme = 0; appearance_reload((void *)2); frame(700);
    while (npages > 1) { page_pop(); frame(300); }

    int io0 = sl_share_io_s; sl_share_io_s = 2;   /* a silent client is let go after 2 s here */
    /* ---- the transfer page: its server starts with the page, on the worker */
    wf_share_cb(NULL); for (int i = 0; i < 30 && (wf_busy || !wf_share_on); i++) frame(100);
    assert(wf_share_on && wf_share_page_obj && rx_card && lv_obj_has_flag(rx_card, LV_OBJ_FLAG_HIDDEN));
    int wp = sl_share_bound_port(false); assert(wp > 0);
    assert(e_req(wp, "GET", "/", NULL, 0, out, sizeof out) == 200 && strstr(out, "Drop songs or folders here") && strstr(out, "webkitdirectory"));
    assert(e_req(wp, "GET", "/api/info", NULL, 0, out, sizeof out) == 200 && strstr(out, "\"free\":"));
    assert(e_req(wp, "POST", "/api/begin?files=4&bytes=400200", NULL, 0, out, sizeof out) == 200);
    db_update_state = DBU_IDLE;
    assert(e_req(wp, "POST", "/api/upload?path=Blue%20Hours%2F01%20Night%20Drive.flac&size=200000", big, 200000, out, sizeof out) == 200);
    assert(e_file_is("Blue Hours/01 Night Drive.flac", big, 200000));
    /* the same name again: both kept */
    assert(e_req(wp, "POST", "/api/upload?path=Blue%20Hours%2F01%20Night%20Drive.flac&size=100", big + 5, 100, out, sizeof out) == 200);
    assert(e_file_is("Blue Hours/01 Night Drive.flac", big, 200000) && e_file_is("Blue Hours/01 Night Drive (2).flac", big + 5, 100));
    /* not a song: refused, nothing written */
    assert(e_req(wp, "POST", "/api/upload?path=notes.txt&size=5", "hello", 5, out, sizeof out) == 415 && !e_exists("notes.txt"));
    /* chunked, as some clients send, and half of one on the R1's progress */
    {
        int fd = e_connect(wp);
        const char *h = "POST /api/upload?path=02%20Harbor.mp3&size=100 HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
        e_sendall(fd, h, strlen(h));
        e_sendall(fd, "32\r\n", 4); e_sendall(fd, big, 50); e_sendall(fd, "\r\n", 2);
        e_rx_frames(10);
        assert(rx_st.state == SL_RX_RECEIVING && !lv_obj_has_flag(rx_card, LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_label_get_text(rx_title), "Receiving 3 of 4") && strstr(lv_label_get_text(rx_line), "02 Harbor.mp3"));
        e_sendall(fd, "32\r\n", 4); e_sendall(fd, big + 50, 50); e_sendall(fd, "\r\n0\r\n\r\n", 7);
        assert(e_answer(fd, out, sizeof out) == 200 && e_file_is("02 Harbor.mp3", big, 100));
    }
    /* a client that connects and says nothing: the page and the player go on */
    {
        int idle = e_connect(wp);
        uint64_t t0 = e_ms(), worst = 0;
        for (int i = 0; i < 20; i++) { uint64_t f0 = e_ms(); rx_poll(); frame(50); if (e_ms() - f0 > worst) worst = e_ms() - f0; }
        assert(e_req(wp, "GET", "/api/info", NULL, 0, out, sizeof out) == 200);   /* another is served beside it */
        printf("receive: a silent client connected: worst UI frame %u ms over %u ms; another request served\n", (unsigned)worst, (unsigned)(e_ms() - t0));
        assert(worst < 100);
        /* and it is let go when it says nothing for the timeout */
        char c1; ssize_t r = recv(idle, &c1, 1, 0);
        printf("receive: the silent client was let go after %u ms\n", (unsigned)(e_ms() - t0));
        assert(r <= 0 && e_ms() - t0 < 4000);
        close(idle);
    }
    assert(e_req(wp, "POST", "/api/end", NULL, 0, out, sizeof out) == 200);
    e_rx_frames(10);
    /* the end: said, the library updated, the result left on the page */
    assert(rx_st.state == SL_RX_IDLE && db_update_state != DBU_IDLE && !lv_obj_has_flag(rx_card, LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(rx_title), "3 files received"));
    assert(e_note_has("3 files received"));
    while (npages > 1) { page_pop(); frame(300); }
    for (int i = 0; i < 40 && (wf_busy || wf_share_on); i++) frame(250);
    assert(!wf_share_on && !sl_share_bound_port(false));

    /* ---- LocalSend: the switch, then discovery, an ask, the files ---- */
    db_update_state = DBU_IDLE;
    global_settings.sloppy_localsend = 1; sl_bt_demo_name[0] = '\0';
    for (int i = 0; i < 40 && !rx_ls_up; i++) frame(250);
    int lp = sl_share_bound_port(true); assert(rx_ls_up && lp > 0);
    {   /* multicast's announcement, sent straight to it: it answers who it is */
        int u = socket(AF_INET, SOCK_DGRAM, 0); assert(u >= 0);
        struct timeval tv = { 3, 0 }; setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)lp) }; inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        const char *ann = "{\"alias\":\"Tai's Phone\",\"version\":\"2.1\",\"deviceModel\":\"iPhone\",\"deviceType\":\"mobile\","
                          "\"fingerprint\":\"sender-fp-1\",\"port\":53317,\"protocol\":\"https\",\"download\":false,\"announce\":true}";
        assert(sendto(u, ann, strlen(ann), 0, (struct sockaddr *)&to, sizeof to) > 0);
        char r[1024]; ssize_t n = recv(u, r, sizeof r - 1, 0); assert(n > 0); r[n] = '\0';
        char alias[64], proto[16]; assert(e_json_str(r, "alias", alias, sizeof alias) && e_json_str(r, "protocol", proto, sizeof proto));
        assert(!strcmp(alias, sl_bt_shown_player_name()) && !strcmp(proto, "http") && strstr(r, "\"announce\":false"));
        close(u);
    }
    assert(e_req(lp, "POST", "/api/localsend/v2/register", "{\"alias\":\"x\"}", 13, out, sizeof out) == 200 && strstr(out, "\"deviceModel\":\"HiBy R1\""));
    assert(e_req(lp, "GET", "/api/localsend/v2/info", NULL, 0, out, sizeof out) == 200 && strstr(out, "\"fingerprint\":"));
    {   /* a prepare nested 200,000 deep, from anyone on the network: refused, the
         * connection thread's stack intact (it ran out, and the player went down) */
        static char deep[400100]; size_t n = (size_t)sprintf(deep, "{\"info\":{\"a\":");
        for (int i = 0; i < 200000; i++) deep[n++] = '[';
        for (int i = 0; i < 200000; i++) deep[n++] = ']';
        n += (size_t)sprintf(deep + n, "}}");
        int st = e_req(lp, "POST", "/api/localsend/v2/prepare-upload", deep, n, out, sizeof out);
        assert(st >= 400 && !rx_sheet);
        assert(e_req(lp, "GET", "/api/localsend/v2/info", NULL, 0, out, sizeof out) == 200);
        printf("receive: a prepare nested 200,000 deep: %d\n", st);
    }
    /* prepare: two songs (one with its checksum) and a text file */
    char sha[65]; e_sha(big, 150000, sha);
    static struct e_bg bg; pthread_t t;
    bg.port = lp; bg.method = "POST"; bg.path = "/api/localsend/v2/prepare-upload";
    bg.len = (size_t)snprintf(bg.body, sizeof bg.body,
        "{\"info\":{\"alias\":\"Tai\\u2019s Phone\",\"version\":\"2.1\",\"deviceModel\":\"iPhone\",\"deviceType\":\"mobile\",\"fingerprint\":\"f\",\"port\":53317,\"protocol\":\"https\",\"download\":false},"
        "\"files\":{\"a1\":{\"id\":\"a1\",\"fileName\":\"03 Lighthouse.flac\",\"size\":150000,\"fileType\":\"audio/flac\",\"sha256\":\"%s\",\"preview\":null},"
        "\"b2\":{\"id\":\"b2\",\"fileName\":\"04 Tide.m4a\",\"size\":1000,\"fileType\":\"audio/mp4\",\"sha256\":null,\"metadata\":{\"modified\":\"2026-01-01T12:00:00Z\"}},"
        "\"c3\":{\"id\":\"c3\",\"fileName\":\"todo.txt\",\"size\":10,\"fileType\":\"text/plain\"}}}", sha);
    e_bg_start(&bg, &t);
    for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
    assert(rx_sheet && acts_open_now && rx_st.state == SL_RX_ASK && rx_st.files == 2 && rx_st.refused == 1);
    assert(find_label(acts, "Accept 2 songs from Tai\xe2\x80\x99s Phone?") && find_label(acts, "Accept") && find_label(acts, "Decline"));
    lv_obj_t *yes = find_label(acts, "Accept"); while (yes && !lv_obj_has_flag(yes, LV_OBJ_FLAG_CLICKABLE)) yes = lv_obj_get_parent(yes);
    lv_obj_send_event(yes, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < 200 && !bg.done; i++) usleep(10000);
    pthread_join(t, NULL);
    char sid[40], tok1[40], tok2[40], q[256];
    assert(bg.status == 200 && e_json_str(bg.out, "sessionId", sid, sizeof sid) && e_json_str(bg.out, "a1", tok1, sizeof tok1) && e_json_str(bg.out, "b2", tok2, sizeof tok2));
    assert(!strstr(bg.out, "\"c3\""));   /* the text file was not taken */
    /* a wrong token, then the right ones; the second song's checksum is wrong on purpose */
    snprintf(q, sizeof q, "/api/localsend/v2/upload?sessionId=%s&fileId=a1&token=nope", sid);
    assert(e_req(lp, "POST", q, big, 150000, out, sizeof out) == 403 && !e_exists("03 Lighthouse.flac"));
    snprintf(q, sizeof q, "/api/localsend/v2/upload?sessionId=%s&fileId=a1&token=%s", sid, tok1);
    assert(e_req(lp, "POST", q, big, 150000, out, sizeof out) == 200 && e_file_is("03 Lighthouse.flac", big, 150000));
    e_rx_frames(4); assert(rx_st.state == SL_RX_RECEIVING && rx_st.done == 1);
    snprintf(q, sizeof q, "/api/localsend/v2/upload?sessionId=%s&fileId=b2&token=%s", sid, tok2);
    assert(e_req(lp, "POST", q, big, 1000, out, sizeof out) == 200 && e_file_is("04 Tide.m4a", big, 1000));
    e_rx_frames(6);
    assert(rx_st.state == SL_RX_IDLE && db_update_state != DBU_IDLE && e_note_has("2 files received") && e_note_has("Tai\xe2\x80\x99s Phone"));
    db_update_state = DBU_IDLE;
    /* a checksum that does not match: refused, removed */
    bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Laptop\"},\"files\":{\"z\":{\"id\":\"z\",\"fileName\":\"05 Wrong.flac\",\"size\":100,"
        "\"sha256\":\"%s\"}}}", sha);
    e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
    assert(rx_sheet && find_label(acts, "Accept 1 song from Laptop?"));
    { lv_obj_t *y = find_label(acts, "Accept"); while (y && !lv_obj_has_flag(y, LV_OBJ_FLAG_CLICKABLE)) y = lv_obj_get_parent(y); lv_obj_send_event(y, LV_EVENT_CLICKED, NULL); }
    pthread_join(t, NULL); assert(bg.status == 200 && e_json_str(bg.out, "sessionId", sid, sizeof sid) && e_json_str(bg.out, "z", tok1, sizeof tok1));
    snprintf(q, sizeof q, "/api/localsend/v2/upload?sessionId=%s&fileId=z&token=%s", sid, tok1);
    assert(e_req(lp, "POST", q, big + 1, 100, out, sizeof out) == 422 && !e_exists("05 Wrong.flac"));
    e_rx_frames(6); assert(rx_st.state == SL_RX_IDLE);
    /* declined: 403 and nothing written */
    bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Stranger\"},\"files\":{\"s\":{\"id\":\"s\",\"fileName\":\"06 Spam.mp3\",\"size\":10}}}");
    e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
    { lv_obj_t *n = find_label(acts, "Decline"); while (n && !lv_obj_has_flag(n, LV_OBJ_FLAG_CLICKABLE)) n = lv_obj_get_parent(n); lv_obj_send_event(n, LV_EVENT_CLICKED, NULL); }
    pthread_join(t, NULL); assert(bg.status == 403); e_rx_frames(4); assert(rx_st.state == SL_RX_IDLE && !rx_sheet);
    /* the sheet sent away by a tap beside it is a no too */
    e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
    acts_scrim_cb(NULL); e_rx_frames(4); pthread_join(t, NULL); assert(bg.status == 403 && !rx_sheet);
    /* only text files: refused without asking */
    bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Stranger\"},\"files\":{\"t\":{\"id\":\"t\",\"fileName\":\"a.pdf\",\"size\":10}}}");
    assert(e_req(lp, "POST", bg.path, bg.body, bg.len, out, sizeof out) == 403);
    e_rx_frames(4); assert(!rx_sheet && e_note_has("Only songs are received"));
    /* cancelled half way, and USB mode half way: the half files go */
    for (int pass = 0; pass < 2; pass++)
    {
        bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Laptop\"},\"files\":{\"h\":{\"id\":\"h\",\"fileName\":\"07 Half.flac\",\"size\":200000}}}");
        e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
        sl_share_answer(true); pthread_join(t, NULL);
        assert(bg.status == 200 && e_json_str(bg.out, "sessionId", sid, sizeof sid) && e_json_str(bg.out, "h", tok1, sizeof tok1));
        int fd = e_connect(lp); char h[512];
        int n = snprintf(h, sizeof h, "POST /api/localsend/v2/upload?sessionId=%s&fileId=h&token=%s HTTP/1.1\r\nHost: x\r\nContent-Length: 200000\r\n\r\n", sid, tok1);
        e_sendall(fd, h, (size_t)n); e_sendall(fd, big, 60000);
        for (int i = 0; i < 100 && !e_exists("07 Half.flac"); i++) usleep(10000);
        assert(e_exists("07 Half.flac"));
        e_rx_frames(2);
        if (!pass) { snprintf(q, sizeof q, "/api/localsend/v2/cancel?sessionId=%s", sid); assert(e_req(lp, "POST", q, NULL, 0, out, sizeof out) == 200); }
        else sl_share_hold(true);
        send(fd, big, 1000, 0);           /* more comes: the writer sees the session gone */
        int st = e_answer(fd, out, sizeof out);
        for (int i = 0; i < 100 && e_exists("07 Half.flac"); i++) usleep(10000);
        assert(!e_exists("07 Half.flac") && (st == 409 || st == 503 || st == -1));
        if (pass) {   /* while held, nothing is taken */
            assert(e_req(lp, "POST", bg.path, bg.body, bg.len, out, sizeof out) == 503);
            sl_share_hold(false);
        }
        e_rx_frames(4); assert(rx_st.state == SL_RX_IDLE && !rx_sheet);
        assert(nh.n && strstr(nh.list[0].title, pass ? "USB mode stopped it" : "Cancelled by the sender"));
    }
    {   /* ids of 95 quotes, each escaped twice over in the answer: it ran past its
         * buffer (heap overflow, once Accept was tapped) */
        char qid[200]; size_t k = 0; for (int i = 0; i < 95; i++) { qid[k++] = '\\'; qid[k++] = '"'; } qid[k] = '\0';
        bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Laptop\"},\"files\":{"
            "\"q1\":{\"id\":\"%s\",\"fileName\":\"08 Q.flac\",\"size\":10},\"q2\":{\"id\":\"%s1\",\"fileName\":\"09 Q.flac\",\"size\":10}}}", qid, qid);
        e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
        sl_share_answer(true); pthread_join(t, NULL);
        assert(bg.status == 200 && e_json_str(bg.out, "sessionId", sid, sizeof sid) && strstr(bg.out, "}}"));
        snprintf(q, sizeof q, "/api/localsend/v2/cancel?sessionId=%s", sid); assert(e_req(lp, "POST", q, NULL, 0, out, sizeof out) == 200);
        e_rx_frames(4); assert(rx_st.state == SL_RX_IDLE && !rx_sheet);
    }
    /* only one at a time: a second sender while the first is asked waits its turn */
    e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
    assert(e_req(lp, "POST", bg.path, bg.body, bg.len, out, sizeof out) == 409);
    sl_share_answer(false); pthread_join(t, NULL); e_rx_frames(4);
    assert(e_files_in(E_RX_DIR) == 5);    /* 3 from the page (one name twice), 2 from LocalSend */
    /* off with the switch, and with Wi-Fi */
    global_settings.sloppy_localsend = 0;
    for (int i = 0; i < 40 && rx_ls_up; i++) frame(250);
    assert(!rx_ls_up && !sl_share_bound_port(true));
    while (npages > 1) { page_pop(); frame(300); }
    db_update_state = DBU_IDLE;
    e_wifi(conn0);
    sl_share_io_s = io0;
    global_settings.sloppy_theme = th0; appearance_reload((void *)2); frame(700);
    printf("PASS: music in over Wi-Fi: the transfer page (folders, keep both, songs only, chunked, progress on the player, a silent client) "
           "and LocalSend (discovery, ask, accept, decline, checksum, cancel, USB mode, one at a time), the library updated after.\n");
}

/* ---- Last.fm direct: a fake Last.fm that checks every signature ---- */
#include "../sloppy_lastfm_state.h"
static int lff_req, lff_scrobbled, lff_now, lff_polls, lff_approve_after = 2, lff_lost, lff_bad_sig;
static char lff_last[8192], lff_method[40];
static int lff_param(const char *body, const char *key, char *out, size_t cap) {
    char k[48]; snprintf(k, sizeof k, "%s=", key);
    for (const char *p = body; p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
        if (!strncmp(p, k, strlen(k))) { sl_net_query_param(p, key, out, cap); return 1; }
    if (cap) out[0] = '\0';
    return 0;
}
static int lff(const char *url, const char *auth, const char *body, char *out, size_t cap) {
    (void)auth;
    if (!strstr(url, "ws.audioscrobbler.com")) return lbf(url, auth, body, out, cap);
    lff_req++;
    snprintf(lff_last, sizeof lff_last, "%s", body ? body : "");
    /* the signature, as Last.fm checks it: every pair but api_sig and format, sorted, then the secret */
    static struct sl_lfm_param ps[200]; static char vals[200][300]; int n = 0; char sig[40] = "";
    for (const char *p = body; p && *p && n < 200; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        size_t kl = strcspn(p, "="); char key[32]; snprintf(key, sizeof key, "%.*s", (int)kl, p);
        char raw[1200]; snprintf(raw, sizeof raw, "%.*s", (int)strcspn(p, "&"), p);
        char v[300]; sl_net_query_param(raw, key, v, sizeof v);
        /* keys come encoded: artist%5B0%5D */
        char dk[32]; size_t o = 0; for (size_t i = 0; key[i] && o + 1 < sizeof dk; i++) {
            if (key[i] == '%' && key[i + 1] && key[i + 2]) { char h[3] = { key[i + 1], key[i + 2], 0 }; dk[o++] = (char)strtol(h, NULL, 16); i += 2; }
            else dk[o++] = key[i]; } dk[o] = '\0';
        if (!strcmp(dk, "api_sig")) { snprintf(sig, sizeof sig, "%s", v); continue; }
        if (!strcmp(dk, "format")) continue;
        snprintf(ps[n].k, sizeof ps[n].k, "%s", dk); snprintf(vals[n], sizeof vals[n], "%s", v); ps[n].v = vals[n]; n++;
    }
    char want[33]; sl_lfm_sign(ps, n, "testsecret", want);
    if (strcmp(want, sig)) { lff_bad_sig++; snprintf(out, cap, "{\"error\":13,\"message\":\"Invalid method signature supplied\"}"); return 403; }
    lff_param(body, "method", lff_method, sizeof lff_method);
    char v[200];
    if (!strcmp(lff_method, "auth.getToken")) { snprintf(out, cap, "{\"token\":\"tok0123456789abcdefTOKEN\"}"); return 200; }
    if (!strcmp(lff_method, "auth.getSession")) {
        if (++lff_polls <= lff_approve_after) { snprintf(out, cap, "{\"error\":14,\"message\":\"Unauthorized Token - This token has not been issued\"}"); return 403; }
        snprintf(out, cap, "{\"session\":{\"name\":\"tai\",\"key\":\"sk0123456789secretSESSION\",\"subscriber\":0}}"); return 200;
    }
    if (!strcmp(lff_method, "auth.getMobileSession")) {
        lff_param(body, "password", v, sizeof v);
        if (strcmp(v, "right pass")) { snprintf(out, cap, "{\"error\":4,\"message\":\"Authentication Failed\"}"); return 403; }
        snprintf(out, cap, "{\"session\":{\"name\":\"tai\",\"key\":\"sk0123456789secretSESSION\",\"subscriber\":0}}"); return 200;
    }
    lff_param(body, "sk", v, sizeof v);
    if (lff_lost || strcmp(v, "sk0123456789secretSESSION")) { snprintf(out, cap, "{\"error\":9,\"message\":\"Invalid session key - Please re-authenticate\"}"); return 403; }
    if (!strcmp(lff_method, "track.updateNowPlaying")) { lff_now++; snprintf(out, cap, "{\"nowplaying\":{}}"); return 200; }
    if (!strcmp(lff_method, "track.scrobble")) {
        int k = 0; char key[16]; for (;; k++) { snprintf(key, sizeof key, "artist%%5B%d%%5D", k); if (!lff_param(body, key, v, sizeof v)) break; }
        lff_scrobbled += k;
        snprintf(out, cap, "{\"scrobbles\":{\"scrobble\":[],\"@attr\":{\"ignored\":0,\"accepted\":%d}}}", k); return 200;
    }
    snprintf(out, cap, "{\"error\":3,\"message\":\"Invalid Method\"}"); return 400;
}
static void e_lfm_play(const char *path, const char *title, unsigned long ms) { lbf_play(path, title, ms); scr_poll(); frame(100); }

static void e_lastfm_tests(void) {
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.tsv /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session"
                   " && mkdir -p /tmp/sloppy-ui-test-root/.rockbox/sloppy"));
    const char *k0 = sl_lastfm_key, *s0 = sl_lastfm_secret; unsigned poll0 = sl_lastfm_poll_ms;
    sl_lastfm_key = "testkey"; sl_lastfm_secret = "testsecret"; sl_lastfm_poll_ms = 0;
    sl_net_scrobble_fake = lff;
    while (npages > 1) { page_pop(); frame(300); }
    int st0 = test_status; struct mp3entry tt0 = test_track; bool conn0 = wf_st.connected;
    wf_st.connected = true; sl_net_scrobble_online(true);
    sl_net_scrobble_hold(false);   /* reloads the files: no session */
    scr_page(); scr_poll(); frame(400);
    assert(scr_page_obj && has_text(scr_page_obj, "Send to Last.fm") && has_text(scr_page_obj, "Not signed in"));
    assert(lv_obj_has_flag(scr_lfm_out_row, LV_OBJ_FLAG_HIDDEN));
    /* on, signed out: what plays waits for a sign-in */
    lv_obj_send_event(lv_obj_get_parent(find_label(scr_page_obj, "Send to Last.fm")), LV_EVENT_CLICKED, NULL); frame(300);
    assert(global_settings.sloppy_lastfm == 1);
    test_status = AUDIO_STATUS_PLAY; test_track.artist = "Mira Sol"; test_track.albumartist = NULL; test_track.album = "Night Bus";
    test_track.tracknum = 4; test_track.length = 200000; test_track.mb_track_id = NULL;
    int r0 = lff_req;
    e_lfm_play("/Music/lfm-a.flac", "Harbor Lights", 120000);
    e_lfm_play("/Music/lfm-b.flac", "Tide Tables", 0);
    assert(lff_req == r0 && scr_st.lfm_queued == 1 && scr_st.lfm == SL_LFM_OUT);
    assert(has_text(scr_page_obj, "Waiting to send: 1") && has_text(scr_page_obj, "Sign in to send what waits"));
    /* the phone way: a code to scan, polled until the phone allows it */
    lv_obj_send_event(scr_lfm_row, LV_EVENT_CLICKED, NULL); frame(300); scr_poll(); frame(300);
    assert(lfm_page_obj && pages[npages - 1] == lfm_page_obj && scr_st.lfm_auth == SL_LFM_AUTH_WAIT);
    assert(!strcmp(scr_st.lfm_url, "https://www.last.fm/api/auth/?api_key=testkey&token=tok0123456789abcdefTOKEN"));
    assert(has_text(lfm_page_obj, "Scan with your phone") && has_text(lfm_page_obj, "Sign in with a password instead"));
    lff_polls = 0;
    for (int i = 0; i < 2; i++) { scr_poll(); frame(100); }
    assert(scr_st.lfm_auth == SL_LFM_AUTH_WAIT && lff_polls == 2);   /* not allowed yet: asked again, nothing else */
    scr_poll(); frame(300);
    assert(scr_st.lfm == SL_LFM_IN && scr_st.lfm_auth == SL_LFM_AUTH_OK && !strcmp(scr_st.lfm_user, "tai"));
    assert(has_text(lfm_page_obj, "Signed in to Last.fm") && e_note_has("Signed in to Last.fm"));
    scr_poll(); frame(100);
    assert(lff_scrobbled == 1 && strstr(lff_last, "track%5B0%5D=Harbor%20Lights") && strstr(lff_last, "trackNumber%5B0%5D=4") && !lff_bad_sig);
    assert(scr_st.lfm_queued == 0);
    {   FILE *f = fopen("/tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session", "r"); char b[128] = ""; assert(f && fgets(b, sizeof b, f)); fclose(f);
        assert(!strcmp(b, "tai\tsk0123456789secretSESSION\n"));
        struct stat sb; assert(!stat("/tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session", &sb) && (sb.st_mode & 077) == 0);
        assert(access("/tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session.tmp", F_OK)); }   /* written aside and renamed: none left */
    page_pop(); frame(400);
    assert(has_text(scr_page_obj, "tai") && !lv_obj_has_flag(scr_lfm_out_row, LV_OBJ_FLAG_HIDDEN));
    /* what plays now goes as it starts, then as it counts */
    int now0 = lff_now;
    e_lfm_play("/Music/lfm-c.flac", "Lighthouse", 0);
    assert(lff_now == now0 + 1 && strstr(lff_last, "track.updateNowPlaying"));
    e_lfm_play("/Music/lfm-c.flac", "Lighthouse", 130000);
    e_lfm_play("/Music/lfm-d.flac", "Undertow", 0);
    assert(lff_scrobbled == 2 && scr_st.lfm_queued == 0 && has_text(scr_page_obj, "Last sent: now"));
    /* Last.fm ends the session: said loudly, and nothing that waits is lost */
    lff_lost = 1;
    e_lfm_play("/Music/lfm-d.flac", "Undertow", 130000);
    e_lfm_play("/Music/lfm-e.flac", "Breakwater", 125000);
    e_lfm_play("/Music/lfm-f.flac", "Gulls", 0);
    assert(scr_st.lfm == SL_LFM_LOST && scr_st.lfm_queued == 2);
    assert(e_note_has("Last.fm signed this player out") && has_text(scr_page_obj, "Sign in again"));
    int req_lost = lff_req;
    e_lfm_play("/Music/lfm-f.flac", "Gulls", 130000); e_lfm_play("/Music/lfm-g.flac", "Pier", 0);
    assert(lff_req == req_lost && scr_st.lfm_queued == 3);   /* no requests that would fail, the songs kept */
    /* signed in again, with a password: a wrong one first */
    lff_lost = 0; lff_polls = 0; lff_approve_after = 1000;   /* the phone never allows it this time */
    lv_obj_send_event(scr_lfm_row, LV_EVENT_CLICKED, NULL); frame(300); scr_poll(); frame(300);
    lv_obj_send_event(lv_obj_get_parent(find_label(lfm_page_obj, "Sign in with a password instead")), LV_EVENT_CLICKED, NULL); frame(400);
    assert(lfm_pw_page && pages[npages - 1] == lfm_pw_page && lfm_user_ta && lfm_pass_ta);
    lv_textarea_set_text(lfm_user_ta, "tai"); lv_textarea_set_text(lfm_pass_ta, "wrong");
    lfm_pw_go_cb(NULL); scr_poll(); frame(300);
    assert(scr_st.lfm_auth == SL_LFM_AUTH_FAILED && !strcmp(scr_st.lfm_error, "Last.fm did not accept that name and password"));
    assert(pages[npages - 1] == lfm_page_obj && has_text(lfm_page_obj, "Last.fm did not accept that name and password"));
    lfm_pw_page_open(NULL); frame(400);
    lv_textarea_set_text(lfm_user_ta, "tai"); lv_textarea_set_text(lfm_pass_ta, "right pass");
    lfm_pw_go_cb(NULL); scr_poll(); frame(300); scr_poll(); frame(300);
    assert(scr_st.lfm == SL_LFM_IN && scr_st.lfm_queued == 0 && lff_scrobbled == 5 && !lff_bad_sig);
    while (npages > 1 && pages[npages - 1] != scr_page_obj) { page_pop(); frame(300); }
    /* signed out: the file goes */
    lv_obj_send_event(scr_lfm_out_row, LV_EVENT_CLICKED, NULL); scr_poll(); frame(300);
    assert(scr_st.lfm == SL_LFM_OUT && access("/tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session", F_OK) != 0);
    assert(has_text(scr_page_obj, "Not signed in") && lv_obj_has_flag(scr_lfm_out_row, LV_OBJ_FLAG_HIDDEN));
    /* never in the log, never on screen */
    assert(!lbf_log_has("sk0123456789secretSESSION") && !lbf_log_has("right pass") && !lbf_log_has("tok0123456789") && lbf_log_has("Last.fm: 1 scrobbled"));
    assert(!has_text(lv_screen_active(), "sk0123456789") && !has_text(lv_layer_top(), "sk0123456789"));
    /* a build without the key says so and asks nothing */
    sl_lastfm_key = ""; scr_poll(); sl_net_scrobble_online(false); sl_net_scrobble_online(true); scr_poll(); frame(300);
    assert(scr_st.lfm_keyless && has_text(scr_page_obj, "Not in this build"));
    sl_lastfm_key = "testkey"; sl_net_scrobble_online(false); sl_net_scrobble_online(true); scr_poll();
    test_status = 0; hist_poll(); test_track = tt0; test_status = st0;
    global_settings.sloppy_lastfm = 0; scr_apply();
    wf_st.connected = conn0; sl_net_scrobble_online(conn0); sl_net_scrobble_fake = NULL;
    sl_lastfm_key = k0; sl_lastfm_secret = s0; sl_lastfm_poll_ms = poll0; lff_approve_after = 2;
    while (npages > 1) { page_pop(); frame(300); }
    printf("PASS: Last.fm direct: signed requests checked by a fake Last.fm, phone sign-in (QR) and password, now playing, "
           "a queue kept while signed out and when Last.fm ends the session (said loudly), sign out; no session in the log.\n");
}

/* ---- lyrics and covers for the library: a fake LRCLIB, MusicBrainz and Cover Art Archive ---- */
#define E_LIB_ROOT "/tmp/sloppy-ui-test-root/Music/E-Lib"
static pthread_mutex_t ljf_lock = PTHREAD_MUTEX_INITIALIZER;
static int ljf_n, ljf_slow_ms;
static char ljf_urls[64][400];
static uint64_t ljf_at[64];
static int ljf(const char *url, char *out, size_t cap, const char *path) {
    pthread_mutex_lock(&ljf_lock);
    int k = ljf_n < 64 ? ljf_n++ : 63;
    snprintf(ljf_urls[k], sizeof ljf_urls[k], "%s", url); ljf_at[k] = e_ms();
    int slow = ljf_slow_ms;
    pthread_mutex_unlock(&ljf_lock);
    if (slow) usleep((useconds_t)slow * 1000);   /* a server that takes its time */
    if (strstr(url, "lrclib.net/api/get")) {
        if (strstr(url, "track_name=Night%20Drive")) { snprintf(out, cap, "  HTTP/1.1 200 OK\n{\"id\":1,\"trackName\":\"Night Drive\",\"instrumental\":false,"
            "\"plainLyrics\":\"Streetlights hum\",\"syncedLyrics\":\"[00:01.00]Streetlights hum\\n[00:05.00]The harbor holds its breath \\u2764\"}"); return 200; }
        if (strstr(url, "track_name=Harbor")) { snprintf(out, cap, "{\"plainLyrics\":\"Only words\\nNo times\",\"syncedLyrics\":null}"); return 200; }
        if (strstr(url, "track_name=Interlude")) { snprintf(out, cap, "{\"instrumental\":true,\"plainLyrics\":null,\"syncedLyrics\":null}"); return 200; }
        snprintf(out, cap, "{\"code\":404,\"name\":\"TrackNotFound\"}"); return 404;
    }
    if (strstr(url, "musicbrainz.org/ws/2/release-group/")) {
        if (strstr(url, "Blue%20Hours")) { snprintf(out, cap, "{\"created\":\"x\",\"count\":1,\"offset\":0,\"release-groups\":[{\"id\":\"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\","
            "\"type-id\":\"x\",\"score\":100,\"title\":\"Blue Hours\"}]}"); return 200; }
        if (strstr(url, "Maybe")) { snprintf(out, cap, "{\"release-groups\":[{\"id\":\"11111111-2222-3333-4444-555555555555\",\"score\":42}]}"); return 200; }
        snprintf(out, cap, "{\"created\":\"x\",\"count\":0,\"offset\":0,\"release-groups\":[]}"); return 200;
    }
    if (strstr(url, "coverartarchive.org/release-group/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0/front-500") && path) {
        FILE *f = fopen(path, "wb"); if (!f) return -1;
        unsigned char jpg[2048] = { 0xff, 0xd8, 0xff, 0xe0 }; jpg[2046] = 0xff; jpg[2047] = 0xd9;
        fwrite(jpg, 1, sizeof jpg, f); fclose(f); return 200;
    }
    return 404;
}
static struct sl_lib e_lib_saved;
static int e_lib_state_saved;
static char *e_lib_paths[8];
static void e_lib_make(void) {
    e_lib_saved = sl_lib; e_lib_state_saved = lib_state;
    static struct sl_track tr[8]; static struct sl_album al[4]; static int st[8], at[4], tl[4][3];
    static const char *const titles[8] = { "Night Drive", "Harbor", "Interlude", "Nowhere Song", "Already Here", "Blue Rain", "Lost Track", "Gone" };
    static const char *const albums[4] = { "Blue Hours", "Has Art", "Unknown Album", "Folder Kept" };
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", E_LIB_ROOT, E_LIB_ROOT); assert(!system(cmd));
    for (int a = 0; a < 4; a++) {
        al[a] = (struct sl_album){ albums[a], "Mira Sol", 2026, 2, 200, tl[a], NULL };
        at[a] = a;
        char d[300]; snprintf(d, sizeof d, "mkdir -p '%s/%s'", E_LIB_ROOT, albums[a]); assert(!system(d));
    }
    for (int i = 0; i < 8; i++) {
        int a = i / 2;
        char p[200]; snprintf(p, sizeof p, "/Music/E-Lib/%s/%02d %s.flac", albums[a], i + 1, titles[i]);
        free(e_lib_paths[i]); e_lib_paths[i] = strdup(p);
        tr[i] = (struct sl_track){ e_lib_paths[i], titles[i], a, i % 2 + 1, 1, 200000 + i * 1000, 0, true, NULL, 0, 0, 0 }; tsplit(&tr[i]);
        tl[a][i % 2] = i; st[i] = i;
        char real[400]; snprintf(real, sizeof real, "/tmp/sloppy-ui-test-root%s", p);
        FILE *f = fopen(real, "wb"); assert(f); fputs("x", f); fclose(f);
    }
    /* one song with its words already; one folder with its own cover */
    FILE *f = fopen(E_LIB_ROOT "/Unknown Album/05 Already Here.lrc", "wb"); assert(f); fputs("[00:01.00]mine", f); fclose(f);
    f = fopen(E_LIB_ROOT "/Folder Kept/Folder.JPG", "wb"); assert(f); fputs("jpg", f); fclose(f);
    sl_lib.tracks = tr; sl_lib.n_tracks = 8; sl_lib.albums = al; sl_lib.n_albums = 4;
    sl_lib.songs_by_title = st; sl_lib.albums_by_title = at;
    lib_state = SL_LIB_READY;
}
static void e_lib_restore(void) { sl_lib = e_lib_saved; lib_state = e_lib_state_saved; }
static void e_job_wait(int ms) {
    uint64_t until = e_ms() + (uint64_t)ms;
    while (e_ms() < until && (lb_kind >= 0 || sl_libjob_busy())) { lb_poll(); frame(50); usleep(10000); }
    lb_poll();
}
static bool e_file_has(const char *p, const char *w) {
    FILE *f = fopen(p, "rb"); if (!f) return false; static char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return strstr(b, w) != NULL;
}

static void e_libjob_tests(void) {
    assert(!strcmp(SL_LIBJOB_UA, "GabagoolOS/1.1 ( https://github.com/BlueDreams666/gabagoolOS )"));
    char b[512];
    assert(sl_libjob_lrc_path("/Music/a.b/song.flac", b, sizeof b, ".lrc") && !strcmp(b, "/Music/a.b/song.lrc"));
    assert(sl_libjob_lrc_path("/Music/a.b/noext", b, sizeof b, ".lrc") && !strcmp(b, "/Music/a.b/noext.lrc"));
    assert(sl_libjob_json_str("{\"x\":\"a\\\"b\\u00e9\\ud83d\\ude00\"}", "x", b, sizeof b) && !strcmp(b, "a\"b\xc3\xa9\xf0\x9f\x98\x80"));
    assert(!sl_libjob_json_str("{\"x\":null}", "x", b, sizeof b));
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.miss /tmp/sloppy-ui-test-root/.rockbox/sloppy/covers.miss "
                   "/tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.pos /tmp/sloppy-ui-test-root/.rockbox/sloppy/covers.pos"));
    int ps = fake_find(ROCKBOX_DIR "/sloppy/lyrics.pos"); (void)ps;
    bool conn0 = wf_st.connected;
    e_lib_make();
    sl_net_http_fake = ljf;
    e_wifi(true); sl_libjob_online(true);
    while (npages > 1) { page_pop(); frame(300); }
    /* Settings > Library > Lyrics and covers */
    libjob_page(); frame(400);
    assert(lb_page_obj && has_text(lb_page_obj, "Get lyrics") && has_text(lb_page_obj, "Get covers"));
    /* lyrics, at LRCLIB's pace: the synced words, the plain ones, an instrumental, ones it does not know */
    ljf_n = 0;
    lv_obj_send_event(lb_btn[SL_LIBJOB_LYRICS], LV_EVENT_CLICKED, NULL);
    assert(lb_kind == SL_LIBJOB_LYRICS && has_text(lb_page_obj, "Stop"));
    e_job_wait(30000);
    assert(lb_kind < 0 && lb_st.state == SL_LIBJOB_DONE && lb_st.fed == 8 && lb_st.found == 2 && lb_st.had == 1 && lb_st.missing == 5 && !lb_st.failed);
    assert(e_file_has(E_LIB_ROOT "/Blue Hours/01 Night Drive.lrc", "[00:05.00]The harbor holds its breath \xe2\x9d\xa4"));
    assert(e_file_has(E_LIB_ROOT "/Blue Hours/02 Harbor.lrc", "Only words\nNo times"));
    assert(e_file_has(E_LIB_ROOT "/Unknown Album/05 Already Here.lrc", "mine") && access(E_LIB_ROOT "/Has Art/03 Interlude.lrc", F_OK));
    assert(ljf_n == 7);                    /* every song but the one with words already */
    uint64_t gap = 1000000;
    for (int i = 1; i < ljf_n; i++) if (ljf_at[i] - ljf_at[i - 1] < gap) gap = ljf_at[i] - ljf_at[i - 1];
    printf("libjob: LRCLIB asked %d times, at least %u ms apart\n", ljf_n, (unsigned)gap);
    assert(gap >= 990);
    assert(e_note_has("Lyrics found for 2 songs") && has_text(lb_page_obj, "Get lyrics"));
    /* again: what LRCLIB did not have is not asked for twice; what is on the card is skipped */
    ljf_n = 0; sl_libjob_lrclib_ms = 20;
    lb_start(SL_LIBJOB_LYRICS); e_job_wait(10000);
    assert(lb_st.state == SL_LIBJOB_DONE && ljf_n == 0 && lb_st.had == 3 && lb_st.missing == 5 && lb_st.found == 0);
    /* stopped half way, then carried on from there */
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.miss '" E_LIB_ROOT "/Blue Hours/01 Night Drive.lrc' '" E_LIB_ROOT "/Blue Hours/02 Harbor.lrc'"));
    ljf_n = 0; sl_libjob_lrclib_ms = 300;
    lb_start(SL_LIBJOB_LYRICS);
    for (int i = 0; i < 400 && lb_st.fed < 3; i++) { lb_poll(); frame(20); usleep(5000); }
    lv_obj_send_event(lb_btn[SL_LIBJOB_LYRICS], LV_EVENT_CLICKED, NULL);   /* Stop */
    e_job_wait(10000);
    int stopped_at = lb_st.fed;
    assert(lb_st.state == SL_LIBJOB_STOPPED && stopped_at >= 3 && stopped_at < 8 && lb_pos_load(SL_LIBJOB_LYRICS) == stopped_at);
    frame(300); assert(has_text(lb_page_obj, "Carry on"));
    int asked = ljf_n;
    lb_start(SL_LIBJOB_LYRICS); e_job_wait(10000);
    assert(lb_st.state == SL_LIBJOB_DONE && lb_st.fed == 8 && ljf_n - asked == 8 - stopped_at - (stopped_at <= 4 ? 1 : 0) && lb_pos_load(SL_LIBJOB_LYRICS) == 0);
    /* a server that does not answer for seconds: the player goes on, and Stop is at once */
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.miss /tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.pos '" E_LIB_ROOT "/Blue Hours/01 Night Drive.lrc'"));
    ljf_slow_ms = 3000; ljf_n = 0; sl_libjob_lrclib_ms = 20;
    lb_start(SL_LIBJOB_LYRICS);
    uint64_t worst = 0, t0 = e_ms();
    while (e_ms() - t0 < 1500) { uint64_t f0 = e_ms(); lb_poll(); frame(16); if (e_ms() - f0 > worst) worst = e_ms() - f0; usleep(4000); }
    assert(ljf_n == 1 && lb_kind == SL_LIBJOB_LYRICS);   /* one request in flight, the UI ticking */
    uint64_t s0 = e_ms(); sl_libjob_stop(); uint64_t stop_ms = e_ms() - s0;
    e_job_wait(8000);
    printf("libjob: an unresponsive server: worst UI frame %u ms while waiting, Stop took %u ms, the job ended %u ms later\n",
           (unsigned)worst, (unsigned)stop_ms, (unsigned)(e_ms() - s0));
    assert(worst < 100 && stop_ms < 20 && lb_kind < 0 && lb_st.state == SL_LIBJOB_STOPPED);
    ljf_slow_ms = 0;
    /* no Wi-Fi: it waits, and goes on when it is back; USB mode the same */
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lyrics.pos"));
    wf_st.connected = false; ljf_n = 0;
    lb_start(SL_LIBJOB_LYRICS);
    for (int i = 0; i < 40; i++) { lb_poll(); frame(20); usleep(5000); }
    assert(lb_st.state == SL_LIBJOB_WAITING && ljf_n == 0 && has_text(lb_page_obj, "Waiting for Wi-Fi"));
    sl_libjob_hold(true); wf_st.connected = true; sl_libjob_online(true);
    for (int i = 0; i < 40; i++) { lb_poll(); frame(20); usleep(5000); }
    assert(lb_st.state == SL_LIBJOB_WAITING && ljf_n == 0 && !strcmp(lb_st.why, "USB mode"));
    sl_libjob_hold(false);
    e_job_wait(10000);
    assert(lb_st.state == SL_LIBJOB_DONE && ljf_n > 0);
    /* covers: from MusicBrainz and the Cover Art Archive, for albums with none */
    test_art_state[0] = 0; test_art_state[1] = 1; test_art_state[2] = 0; test_art_state[3] = 0; test_art_again = -1;
    ljf_n = 0;
    lv_obj_send_event(lb_btn[SL_LIBJOB_COVERS], LV_EVENT_CLICKED, NULL);
    e_job_wait(30000);
    assert(lb_st.state == SL_LIBJOB_DONE && lb_st.kind == SL_LIBJOB_COVERS && lb_st.found == 1 && lb_st.had == 2 && lb_st.missing == 1);
    assert(test_art_again == 0);           /* the cover thread is told to look again */
    {   struct stat sb; assert(!stat(E_LIB_ROOT "/Blue Hours/cover.jpg", &sb) && sb.st_size == 2048); }
    assert(access(E_LIB_ROOT "/Unknown Album/cover.jpg", F_OK) && access(E_LIB_ROOT "/Folder Kept/cover.jpg", F_OK));
    assert(ljf_n == 3 && strstr(ljf_urls[0], "musicbrainz.org/ws/2/release-group/?query=releasegroup%3A%22Blue%20Hours%22%20AND%20artist%3A%22Mira%20Sol%22"));
    gap = 1000000; for (int i = 1; i < ljf_n; i++) if (ljf_at[i] - ljf_at[i - 1] < gap) gap = ljf_at[i] - ljf_at[i - 1];
    printf("libjob: MusicBrainz and the Cover Art Archive asked %d times, at least %u ms apart\n", ljf_n, (unsigned)gap);
    assert(gap >= 1090);
    assert(e_note_has("1 cover found"));
    while (npages > 1) { page_pop(); frame(300); }
    memset(test_art_state, 0, sizeof test_art_state);
    sl_libjob_lrclib_ms = SL_LIBJOB_LRCLIB_MS; sl_libjob_mb_ms = SL_LIBJOB_MB_MS;
    sl_net_http_fake = NULL;
    e_lib_restore(); e_wifi(conn0);
    printf("PASS: lyrics and covers for the library: LRCLIB at a request a second, .lrc beside each song, misses remembered, stopped and carried on, "
           "an unresponsive server never holds the UI, waits for Wi-Fi and USB mode; covers via MusicBrainz at its pace, cover.jpg in the folder.\n");
}

/* ---- radio: FLAC (native and Ogg), Ogg Vorbis, Ogg Opus, HLS ---- */
static void e_copy(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb"); assert(in && out);
    char b[8192]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out); fclose(in); fclose(out);
}
/* plays path until it ends; its frames into pcm (up to cap); the state it ended in */
static int e_radio_play(const char *path, int16_t *pcm, size_t cap, size_t *got, unsigned *rate) {
    *got = 0; *rate = 0;
    if (!sl_net_radio_start(path)) return SL_RADIO_FAILED;
    uint64_t t0 = e_ms();
    while (e_ms() - t0 < 8000) {
        size_t n = sl_net_radio_read(pcm + *got * 2, cap - *got < 4096 ? cap - *got : 4096);
        *got += n;
        int st = sl_net_radio_state();
        if ((st == SL_RADIO_ENDED || st == SL_RADIO_FAILED) && !sl_net_radio_buffered()) break;
        if (*got >= cap) break;
        if (!n) usleep(2000);
    }
    *rate = sl_net_radio_rate();
    int st = sl_net_radio_state();
    char why[256]; sl_net_radio_why(why, sizeof why);
    printf("radio %s: %s, %zu frames at %u Hz (%s)\n", strrchr(path, '/') + 1, st == SL_RADIO_ENDED ? "played to its end" : st == SL_RADIO_PLAYING ? "playing" : "failed", *got, *rate, why);
    sl_net_radio_stop();
    return st;
}
/* the tone the FLAC vectors were made from (sloppy-dev: 440 Hz and 554 Hz, a whisper of noise on the left) */
static int e_tone_l(int i) { return (int)(6000 * sin(2 * M_PI * 440 * i / 44100.0) + 300 * sin(i * 0.37)); }
static int e_tone_r(int i) { return (int)(6000 * sin(2 * M_PI * 554 * i / 44100.0)); }
/* a minimal MPEG-TS of an ADTS file: PAT, PMT, then PES packets */
static void e_ts_put(FILE *f, int pid, bool start, const unsigned char *d, size_t n, unsigned *cc) {
    while (n || start) {
        unsigned char p[188]; memset(p, 0xff, sizeof p);
        p[0] = 0x47; p[1] = (unsigned char)((start ? 0x40 : 0) | (pid >> 8)); p[2] = (unsigned char)pid;
        size_t room = 184, take = n < room ? n : room;
        if (take < room) {   /* stuffing in an adaptation field */
            size_t af = room - take;
            p[3] = (unsigned char)(0x30 | (*cc & 15));
            p[4] = (unsigned char)(af - 1);
            if (af > 1) p[5] = 0;
            memcpy(p + 4 + af, d, take);
        } else { p[3] = (unsigned char)(0x10 | (*cc & 15)); memcpy(p + 4, d, take); }
        (*cc)++;
        fwrite(p, 1, 188, f);
        d += take; n -= take; start = false;
    }
}
static void e_make_ts(const char *aac, size_t from, size_t len, const char *out) {
    FILE *in = fopen(aac, "rb"); assert(in); static unsigned char all[200000]; size_t n = fread(all, 1, sizeof all, in); fclose(in);
    if (from + len > n) len = n - from;
    FILE *f = fopen(out, "wb"); assert(f);
    unsigned cc0 = 0, cc1 = 0, cc2 = 0;
    static const unsigned char pat[] = { 0, 0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x01, 0xf0, 0x00, 0, 0, 0, 0 };
    static const unsigned char pmt[] = { 0, 0x02, 0xb0, 0x12, 0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00, 0xf0, 0x00, 0x0f, 0xe1, 0x00, 0xf0, 0x00, 0, 0, 0, 0 };
    e_ts_put(f, 0, true, pat, sizeof pat, &cc0);
    e_ts_put(f, 0x1000, true, pmt, sizeof pmt, &cc1);
    for (size_t at = from; at < from + len; ) {
        size_t take = from + len - at < 2000 ? from + len - at : 2000;
        unsigned char pes[2100] = { 0, 0, 1, 0xc0, 0, 0, 0x80, 0x80, 5, 0x21, 0, 1, 0, 1 };
        size_t pl = 3 + 5 + take; pes[4] = (unsigned char)(pl >> 8); pes[5] = (unsigned char)pl;
        memcpy(pes + 14, all + at, take);
        e_ts_put(f, 0x100, true, pes, 14 + take, &cc2);
        at += take;
    }
    fclose(f);
}
/* where each ADTS frame of the file starts, so segments cut between them */
static size_t e_adts_cut(const char *aac, size_t near) {
    FILE *in = fopen(aac, "rb"); static unsigned char all[200000]; size_t n = fread(all, 1, sizeof all, in); fclose(in);
    size_t at = 0;
    while (at + 7 < n && at < near) { size_t fl = ((size_t)(all[at + 3] & 3) << 11) | ((size_t)all[at + 4] << 3) | (all[at + 5] >> 5); if (!fl) break; at += fl; }
    return at;
}
static void e_radio_format_tests(void) {
    static int16_t pcm[2 * 400000];
    size_t got; unsigned rate;
    /* native FLAC and FLAC in Ogg: every sample as encoded (lossless) */
    e_copy("sloppyos/apps/sloppy/tests/tone.flac", "/tmp/sloppy-e.flac");
    e_copy("sloppyos/apps/sloppy/tests/tone-flac.ogg", "/tmp/sloppy-e-flac.ogg");
    const char *flacs[2] = { "/tmp/sloppy-e.flac", "/tmp/sloppy-e-flac.ogg" };
    for (int k = 0; k < 2; k++) {
        int st = e_radio_play(flacs[k], pcm, 400000, &got, &rate);
        assert(st == SL_RADIO_ENDED && rate == 44100 && got >= 44100);
        int off = 0;
        for (size_t i = 0; i < 44100; i++) if (abs(pcm[i * 2] - e_tone_l((int)i)) > 1 || abs(pcm[i * 2 + 1] - e_tone_r((int)i)) > 1) off++;
        assert(off == 0);
    }
    /* Ogg Vorbis: a sample from this Mac when given (not in git: the vectors here can't be made without an encoder) */
    const char *vs = getenv("SLOPPY_VORBIS_SAMPLE");
    if (vs) {
        e_copy(vs, "/tmp/sloppy-e-vorbis.ogg");
        int st = e_radio_play("/tmp/sloppy-e-vorbis.ogg", pcm, 400000, &got, &rate);
        assert((st == SL_RADIO_ENDED || st == SL_RADIO_PLAYING) && rate >= 8000 && got > 1000);
        FILE *o = fopen("/tmp/sloppy-e-vorbis.raw", "wb"); if (o) { fwrite(pcm, 4, got, o); fclose(o); }
    }
    /* HLS: a playlist of MPEG-TS segments, one packed ADTS segment with its ID3 tag; on demand (ENDLIST) */
    const char *aac = "sloppyos/apps/sloppy/tests/tone.aac";
    assert(!system("rm -rf /tmp/sloppy-e-hls && mkdir -p /tmp/sloppy-e-hls/media"));
    size_t c1 = e_adts_cut(aac, 8000), c2 = e_adts_cut(aac, 16000), c3 = e_adts_cut(aac, 200000);
    e_make_ts(aac, 0, c1, "/tmp/sloppy-e-hls/media/seg0.ts");
    e_make_ts(aac, c1, c2 - c1, "/tmp/sloppy-e-hls/media/seg1.ts");
    {   FILE *in = fopen(aac, "rb"); static unsigned char all[200000]; size_t n = fread(all, 1, sizeof all, in); fclose(in);
        FILE *f = fopen("/tmp/sloppy-e-hls/media/seg2.aac", "wb");
        static const unsigned char id3[] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 10, 'P', 'R', 'I', 'V', 0, 0, 0, 0, 0, 0 };
        fwrite(id3, 1, sizeof id3, f); fwrite(all + c2, 1, (c3 < n ? c3 : n) - c2, f); fclose(f); }
    FILE *m = fopen("/tmp/sloppy-e-hls/media/live.m3u8", "w");
    fputs("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:7\n#EXTINF:0.5,\nseg0.ts\n#EXTINF:0.5,\nseg1.ts\n#EXTINF:1.0,\nseg2.aac\n#EXT-X-ENDLIST\n", m); fclose(m);
    m = fopen("/tmp/sloppy-e-hls/master.m3u8", "w");
    fputs("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=96000,CODECS=\"mp4a.40.2\"\nmedia/live.m3u8\n"
          "#EXT-X-STREAM-INF:BANDWIDTH=5000000,CODECS=\"avc1.4d401f,mp4a.40.2\"\nvideo/big.m3u8\n", m); fclose(m);
    int st = e_radio_play("/tmp/sloppy-e-hls/master.m3u8", pcm, 400000, &got, &rate);
    assert(st == SL_RADIO_ENDED && rate >= 32000 && got > (size_t)rate * 2);   /* through all three segments */
    size_t whole = got;
    double sum = 0; for (size_t i = 0; i < got; i++) sum += (double)pcm[i * 2] * pcm[i * 2];
    assert(sqrt(sum / (double)got) > 1000);   /* sound, not silence */
    /* live: the playlist grows while it plays; the new segment is fetched, the old ones are not again */
    m = fopen("/tmp/sloppy-e-hls/media/grow.m3u8", "w");
    fputs("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:0.5,\nseg0.ts\n#EXTINF:0.5,\nseg1.ts\n", m); fclose(m);
    assert(sl_net_radio_start("/tmp/sloppy-e-hls/media/grow.m3u8"));
    got = 0;
    uint64_t t0 = e_ms();
    bool grown = false;
    while (e_ms() - t0 < 8000) {
        got += sl_net_radio_read(pcm + got * 2, 400000 - got < 4096 ? 400000 - got : 4096);
        if (!grown && e_ms() - t0 > 300) {
            m = fopen("/tmp/sloppy-e-hls/media/grow.m3u8.new", "w");
            fputs("#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:101\n#EXTINF:0.5,\nseg1.ts\n#EXTINF:1.0,\nseg2.aac\n#EXT-X-ENDLIST\n", m); fclose(m);
            assert(!system("mv -f /tmp/sloppy-e-hls/media/grow.m3u8.new /tmp/sloppy-e-hls/media/grow.m3u8"));   /* rename() here is Rockbox's */
            grown = true;
        }
        if (sl_net_radio_state() == SL_RADIO_ENDED && !sl_net_radio_buffered()) break;
        usleep(2000);
    }
    size_t live_frames = got;
    st = sl_net_radio_state(); sl_net_radio_stop();
    printf("radio grow.m3u8: %zu frames after the playlist grew (the whole tone is %zu)\n", live_frames, whole);
    assert(st == SL_RADIO_ENDED && live_frames + 4096 >= whole && live_frames <= whole + 4096);   /* the first two and the one added, each once */
    /* the ones it can't: said by name */
    m = fopen("/tmp/sloppy-e-hls/locked.m3u8", "w");
    fputs("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:6,\nmedia/seg0.ts\n", m); fclose(m);
    st = e_radio_play("/tmp/sloppy-e-hls/locked.m3u8", pcm, 400000, &got, &rate);
    char why[256]; sl_net_radio_why(why, sizeof why);
    assert(st == SL_RADIO_FAILED && strstr(why, "encrypted HLS"));
    /* Ogg Opus: macOS's encoder made the vector (make_opus_vector.py); the
     * decode against the tone it came from, and against Apple's decoder */
    e_copy("sloppyos/apps/sloppy/tests/tone-opus.ogg", "/tmp/sloppy-e-opus.ogg");
    st = e_radio_play("/tmp/sloppy-e-opus.ogg", pcm, 400000, &got, &rate);
    assert(st == SL_RADIO_ENDED && rate == 48000 && got == 76 * 960 - 312);   /* every packet, the pre-skip left out */
    {   FILE *raw = fopen("/tmp/sloppy-e-opus.raw", "wb"); if (raw) { fwrite(pcm, 4, got, raw); fclose(raw); } }
    /* each side against its tone: the sine at its frequency fitted (any delay
     * or phase the encoder adds), what is left over is the error */
    double snr_tone = 1e9, snr_apple = 0, sig, err;
    for (int c = 0; c < 2; c++) {
        double f = c ? 660 : 440, ss = 0, sc = 0, cc = 0, ys = 0, yc = 0, yy = 0;
        for (size_t i = 2000; i < 70000; i++) {
            double sn = sin(2 * M_PI * f * i / 48000.0), cs = cos(2 * M_PI * f * i / 48000.0), y = pcm[i * 2 + c];
            ss += sn * sn; cc += cs * cs; sc += sn * cs; ys += y * sn; yc += y * cs; yy += y * y;
        }
        double det = ss * cc - sc * sc, A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det, fit = A * ys + B * yc;
        double snr = 10 * log10(fit / (yy - fit)), amp = hypot(A, B);
        assert(fabs(amp - (c ? 9000 : 12000)) < (c ? 9000 : 12000) * 0.05);
        if (snr < snr_tone) snr_tone = snr;
    }
    assert(snr_tone > 30);
    if (!system("command -v afconvert >/dev/null 2>&1 && afconvert -f WAVE -d LEI16@48000 /tmp/sloppy-e-opus.ogg /tmp/sloppy-e-opus.wav")) {
        FILE *w = fopen("/tmp/sloppy-e-opus.wav", "rb"); assert(w);
        static unsigned char wav[44 + 72000 * 4 + 4096]; size_t wn = fread(wav, 1, sizeof wav, w); fclose(w);
        size_t at = 12; while (at + 8 < wn && memcmp(wav + at, "data", 4)) at += 8 + (wav[at + 4] | wav[at + 5] << 8 | wav[at + 6] << 16);
        const int16_t *ref = (const int16_t *)(wav + at + 8); size_t rn = (wn - at - 8) / 4;
        sig = err = 0;
        for (size_t i = 0; i < rn && i < got; i++)
            for (int c = 0; c < 2; c++) { double a = ref[i * 2 + c], b = pcm[i * 2 + c]; sig += a * a; err += (a - b) * (a - b); }
        snr_apple = 10 * log10(sig / (err > 0 ? err : 1e-9));
        printf("radio tone-opus.ogg: %.1f dB against the tone, %.1f dB against Apple's decoder (%zu frames)\n", snr_tone, snr_apple, rn);
        assert(rn == 72000 && snr_apple > 60);
    }
    /* more than two channels: said by name */
    FILE *o = fopen("/tmp/sloppy-e-opus6.ogg", "wb");
    static const unsigned char opus6[] = { 'O','g','g','S',0,2, 0,0,0,0,0,0,0,0, 1,0,0,0, 0,0,0,0, 0,0,0,0, 1, 27,
        'O','p','u','s','H','e','a','d',1,6,0x38,1,0x80,0xbb,0,0,0,0,1, 4,2, 0,4,1,2,3,5 };
    fwrite(opus6, 1, sizeof opus6, o); fclose(o);
    st = e_radio_play("/tmp/sloppy-e-opus6.ogg", pcm, 400000, &got, &rate);
    sl_net_radio_why(why, sizeof why);
    assert(st == SL_RADIO_FAILED && strstr(why, "Ogg Opus with 6 channels"));
    printf("PASS: internet radio formats: FLAC and Ogg FLAC sample for sample, HLS (master playlist, MPEG-TS and packed AAC segments, a live playlist that grows), "
           "Ogg Opus (%.0f dB against the tone it was made from, %.0f dB against Apple's decoder), encrypted HLS and 6-channel Opus said by name%s.\n",
           snr_tone, snr_apple, vs ? ", an Ogg Vorbis sample decoded" : "");
}

/* An app's big buffers go back to the system when its page closes (on the
 * R1 they stayed the player's: Spoon, Snake and the coin took 1.8 MB for
 * good and the player froze out of memory, Tai's log of 2 Nov), and each
 * app draws them again when it opens, before it slides in. */
static size_t e_nonzero(const void *p, size_t n) {
    const uint8_t *b = p; size_t k = 0;
    for (size_t i = 4096; i + 4096 < n; i++) k += b[i] != 0;   /* the whole pages inside */
    return k;
}
static void e_give_back_tests(void) {
    while (npages > 1) { page_pop(); frame(300); }
    struct { const char *name; void (*open)(void); const void *buf; size_t size; } app[] = {
        { "Snake", snake_page, snake_px, sizeof snake_px },
        { "Spoon", spoon_page, spn_px, sizeof spn_px },
        { "Flip a Coin", coin_page, coin_shadow_px, sizeof coin_shadow_px },
        { "Flappy Bird", flappy_page, fl_px, sizeof fl_px },
        { "Chess", chess_page, chess_buf, sizeof chess_buf },
    };
    for (size_t i = 0; i < sizeof app / sizeof app[0]; i++) {
        for (int round = 0; round < 2; round++) {
            app[i].open();
            size_t drawn = e_nonzero(app[i].buf, app[i].size);   /* before the first frame: no black slide-in */
            frame(400);
            assert(drawn > (app[i].size - 8192) / 16);   /* the coin's shadow is mostly alpha */
            page_pop(); frame(600);
            assert(npages == 1 && e_nonzero(app[i].buf, app[i].size) == 0);
        }
        printf("given back on close: %s, %zu KB\n", app[i].name, app[i].size / 1024);
    }
    assert(e_nonzero(snake_bg_px, sizeof snake_bg_px) == 0 && e_nonzero(fl_bg, sizeof fl_bg) == 0);
    assert(!fl_bg_made[0] && !fl_bg_made[1] && coin_art_ready);   /* the faces stay: warmed at start-up */
}
static void e_tests(void) {
    e_give_back_tests();
    e_codec_tests();
    e_radio_tests();
    e_share_tests();
    e_lastfm_tests();
    e_libjob_tests();
    e_radio_format_tests();
}

/* ---- the audit's screens ---- */
static bool e_au_dirty, e_au_conn0;
static void e_au_radio(int which) {
#ifdef ST_GROUPS
    if (!e_au_dirty) e_au_conn0 = wf_st.connected;
    e_au_dirty = true;
    if (which == 0) e_radio_file();
    free(st_list); st_list = NULL; st_n = 0; st_kind = which == 3 ? ST_FOUND : ST_TOP;
    e_wifi(which != 1);
    if (which == 2) sl_net_demo_fail = "radio-browser";
    if (which == 3) { strlcpy(st_query, "nothing", sizeof st_query); st_load(ST_FIND_URL "nothing"); for (int i = 0; i < 30 && st_loading; i++) frame(100); }
    webradio_page(); for (int i = 0; i < 30 && st_loading; i++) frame(100);
    e_wifi(which != 1); webradio_page_at(true);
    sl_net_demo_fail = NULL;
#else
    (void)which; webradio_page();
#endif
}
/* the transfer page in the middle of a folder and after it, and
 * LocalSend's ask, as the state says them (no server needed) */
static void e_au_receive(int which) {
    if (!e_au_dirty) e_au_conn0 = wf_st.connected;
    e_au_dirty = true;
    e_wifi(true);
    struct sl_rx_status s;
    memset(&s, 0, sizeof s);
    s.kind = which == 2 ? SL_RX_LOCALSEND : SL_RX_WEB;
    s.files = 12; s.songs = 12; s.done = which == 1 ? 12 : 4;
    s.bytes = which == 1 ? 402653184 : 151000000; s.total = 402653184;
    snprintf(s.current, sizeof s.current, "05 The Extraordinarily Long-Named Closing Track (Live at the Harbor).flac");
    snprintf(s.from, sizeof s.from, "Tai's iPhone 15 Pro Max");
    if (which == 2) { s.state = SL_RX_ASK; rx_st = s; au_open_home(AU_HOME); rx_ask(); return; }
    wf_share_cb(NULL); frame(300);
    s.state = which == 1 ? SL_RX_DONE : SL_RX_RECEIVING;
    rx_page_update(&s);
}
/* Last.fm's pages, through the fake Last.fm: 0 the code to scan, 1 the
 * password page, 2 the Scrobbling page signed in, 3 after Last.fm ended
 * the session with songs waiting */
static bool e_lfm_dirty;
static void e_lfm_begin(void) {
    e_lfm_dirty = true;
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.tsv /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session"));
    sl_lastfm_key = "testkey"; sl_lastfm_secret = "testsecret"; sl_lastfm_poll_ms = 0;
    sl_net_scrobble_fake = lff; lff_lost = 0; lff_polls = 0;
    wf_st.connected = true; sl_net_scrobble_online(true); sl_net_scrobble_hold(false);
    global_settings.sloppy_lastfm = 1; scr_apply(); scr_poll();
}
static void e_lfm_end(void) {
    if (!e_lfm_dirty) return;
    e_lfm_dirty = false;
    sl_net_lastfm_signout(); global_settings.sloppy_lastfm = 0; scr_apply(); scr_poll();
    sl_net_scrobble_fake = NULL; sl_lastfm_key = ""; sl_lastfm_secret = ""; sl_lastfm_poll_ms = 4000; lff_approve_after = 2; lff_lost = 0;
    assert(!system("rm -f /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.tsv /tmp/sloppy-ui-test-root/.rockbox/sloppy/lastfm.session"));
}
static void e_au_lastfm(int which) {
    if (!e_au_dirty) e_au_conn0 = wf_st.connected;
    e_au_dirty = true;
    e_lfm_begin();
    lff_approve_after = which >= 2 ? 0 : 1000;
    if (which == 1) { scr_page(); frame(300); lfm_page(); frame(300); lfm_pw_page_open(NULL); return; }
    if (which == 0) { scr_page(); frame(300); lfm_page(); scr_poll(); frame(300); pop_hide_now(); nbanner_hide_now(); return; }
    sl_net_lastfm_signin_phone(); scr_poll(); scr_poll();
    if (which == 3) {
        struct sl_listen l; memset(&l, 0, sizeof l);
        strlcpy(l.artist, "Mira Sol", sizeof l.artist); strlcpy(l.title, "Harbor Lights", sizeof l.title); l.when = 1761200000u; l.secs = 200;
        lff_lost = 1; sl_net_scrobble_add(&l); sl_net_scrobble_add(&l); sl_net_scrobble_now(&l); scr_poll(); scr_poll();
    }
    scr_page(); scr_poll(); frame(300);
    frame(300); pop_hide_now(); nbanner_hide_now();   /* the sign-in's toast has gone by the time anyone reads the page */
}
/* the Lyrics and covers page: idle, a job a third of the way, one waiting
 * for Wi-Fi, one MusicBrainz stopped answering; the state as the page reads it */
static bool e_lb_dirty;
static void e_au_libjob(int which) {
    e_lb_dirty = true;
    libjob_page();
    if (which) {
        memset(&lb_st, 0, sizeof lb_st);
        lb_st.kind = which == 3 ? SL_LIBJOB_COVERS : SL_LIBJOB_LYRICS;
        lb_st.state = which == 1 ? SL_LIBJOB_RUNNING : which == 2 ? SL_LIBJOB_WAITING : SL_LIBJOB_FAILED;
        lb_st.fed = which == 3 ? 412 : 31204; lb_st.total = which == 3 ? 1890 : 94012; lb_st.found = which == 3 ? 37 : 12804;
        snprintf(lb_st.why, sizeof lb_st.why, "%s", which == 2 ? "Waiting for Wi-Fi" : which == 3 ? "MusicBrainz is not answering" : "");
        lb_kind = which == 3 ? -1 : lb_st.kind;
        lb_refresh();
    }
}
static void e_au_reset(void) {
    if (e_lb_dirty) { e_lb_dirty = false; lb_kind = -1; memset(&lb_st, 0, sizeof lb_st); }
    e_lfm_end();
    if (rx_sheet) { rx_sheet = false; acts_close(); frame(300); }
    memset(&rx_st, 0, sizeof rx_st);
    if (!e_au_dirty) return;
    e_au_dirty = false;
    e_wifi(e_au_conn0);
    free(st_list); st_list = NULL; st_n = 0; st_kind = ST_NEAR;
#ifdef ST_GROUPS
    st_failed = false;
#endif
    remove(ST_SAVED); st_saved_read = false; st_nsaved = 0;
}

/* ---- the screenshots ---- */
/* the page's body scrolled so l is near its top */
static void e_scroll_to(lv_obj_t *l) {
    if (!l) return;
    lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
    lv_obj_update_layout(b); lv_area_t la, ba; lv_obj_get_coords(l, &la); lv_obj_get_coords(b, &ba);
    lv_obj_scroll_to_y(b, lv_obj_get_scroll_y(b) + la.y1 - ba.y1 - 16, LV_ANIM_OFF); frame(300);
}
static void e_shot(const char *dir, const char *name, const char *look, const char *tag) {
    char p[600]; frame(0); snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, name, look, tag); shot_now(p);
}
static void e_shots(const char *dir, const char *tag) {
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    const char *only = getenv("SLOPPY_AUDIT_ONLY");
    au_library();
    for (int look = 0; look < 2; look++) {
        const char *lk = look ? "light" : "dark";
        if (!only || strstr("codec", only)) {
            /* the status bar with headphones on LDAC: home, a page, the Bluetooth page */
            au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            au_open_home(AU_HOME); e_bt_connect("LDAC", true); e_shot(dir, "statusbar-home-ldac", lk, tag);
            e_bt_connect("aptX-HD", false); e_shot(dir, "statusbar-home-aptxhd-jack", lk, tag);
            sl_bt_page(); au_settle(); e_bt_connect("LDAC", true); statusbar_retheme(); frame(100);
            e_scroll_to(find_label(pages[npages - 1], "AUDIO CODEC"));
            e_shot(dir, "bluetooth-codecs-ldac", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            e_bt_done(); au_reset();
        }
        if (!only || strstr("radio", only)) {
            au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            bool conn0 = wf_st.connected;
            e_radio_file();
            e_wifi(true); free(st_list); st_list = NULL; st_n = 0; st_kind = ST_TOP;
            webradio_page(); for (int i = 0; i < 30 && st_loading; i++) frame(100); au_settle();
            e_shot(dir, "radio-saved-sections", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            remove(ST_SAVED); st_saved_read = false; st_nsaved = 0;
            e_wifi(false); free(st_list); st_list = NULL; st_n = 0; st_kind = ST_NEAR;
            webradio_page(); au_settle(); e_shot(dir, "radio-no-wifi", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            e_wifi(true); strlcpy(st_query, "nothing", sizeof st_query); st_kind = ST_FOUND; st_load(ST_FIND_URL "nothing");
            for (int i = 0; i < 30 && st_loading; i++) frame(100);
            e_wifi(true); webradio_page(); au_settle(); e_wifi(true); webradio_page_at(true); au_settle();
            e_shot(dir, "radio-no-results", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            st_kind = ST_TOP; free(st_list); st_list = NULL; st_n = 0;
            sl_net_demo_fail = "radio-browser"; webradio_page(); for (int i = 0; i < 30 && st_loading; i++) frame(100); au_settle();
            sl_net_demo_fail = NULL; e_shot(dir, "radio-failed", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            st_kind = ST_NEAR; free(st_list); st_list = NULL; st_n = 0; e_wifi(conn0); au_reset();
        }
        if (!only || strstr("receive", only)) {
            au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            bool conn0 = wf_st.connected; e_wifi(true);
            wf_share_cb(NULL); for (int i = 0; i < 30 && (wf_busy || !wf_share_on); i++) frame(100);
            au_settle(); e_shot(dir, "transfer-page", lk, tag);
#ifdef SL_SHARE_DIR
            /* a folder coming in: the progress card, then the result */
            int wp = sl_share_bound_port(false);
            static char big[300000]; char out[512];
            assert(e_req(wp, "POST", "/api/begin?files=3&bytes=15600000", NULL, 0, out, sizeof out) == 200);
            for (int k = 0; k < 2; k++) {
                char q[160]; snprintf(q, sizeof q, "/api/upload?path=Blue%%20Hours%%2F0%d%%20Track.flac&size=300000", k + 1);
                assert(e_req(wp, "POST", q, big, sizeof big, out, sizeof out) == 200);
            }
            int fd = e_connect(wp);
            const char *h = "POST /api/upload?path=Blue%20Hours%2F03%20Lighthouse%20Keeper.flac&size=15000000 HTTP/1.1\r\nHost: x\r\nContent-Length: 15000000\r\n\r\n";
            e_sendall(fd, h, strlen(h)); for (int k = 0; k < 28; k++) e_sendall(fd, big, sizeof big);
            e_rx_frames(10); au_settle(); e_rx_frames(2);
            e_shot(dir, "transfer-receiving", lk, tag);
            for (int k = 28; k < 50; k++) e_sendall(fd, big, sizeof big);
            assert(e_answer(fd, out, sizeof out) == 200); e_rx_frames(4);
            assert(e_req(wp, "POST", "/api/end", NULL, 0, out, sizeof out) == 200);
            e_rx_frames(10); nbanner_hide_now(); pop_hide_now(); au_settle();
            e_shot(dir, "transfer-done", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            for (int i = 0; i < 40 && (wf_busy || wf_share_on); i++) frame(250);
            /* LocalSend's ask, over home */
            global_settings.sloppy_localsend = 1;
            for (int i = 0; i < 40 && !rx_ls_up; i++) frame(250);
            static struct e_bg bg; pthread_t t;
            bg.port = sl_share_bound_port(true); bg.method = "POST"; bg.path = "/api/localsend/v2/prepare-upload";
            bg.len = (size_t)snprintf(bg.body, sizeof bg.body, "{\"info\":{\"alias\":\"Tai's iPhone\"},\"files\":{"
                "\"1\":{\"id\":\"1\",\"fileName\":\"01 Night Drive.flac\",\"size\":31000000},\"2\":{\"id\":\"2\",\"fileName\":\"02 Harbor.flac\",\"size\":28000000},"
                "\"3\":{\"id\":\"3\",\"fileName\":\"03 Tide.flac\",\"size\":26500000}}}");
            au_open_home(AU_HOME);
            e_bg_start(&bg, &t); for (int i = 0; i < 100 && !rx_sheet; i++) e_rx_frames(1);
            au_settle(); e_shot(dir, "localsend-ask", lk, tag);
            sl_share_answer(false); pthread_join(t, NULL); e_rx_frames(6);
            global_settings.sloppy_localsend = 0;
            for (int i = 0; i < 40 && rx_ls_up; i++) frame(250);
            system("rm -rf '" E_RX_DIR "'");
#endif
            while (npages > 1) { page_pop(); frame(300); }
            for (int i = 0; i < 40 && (wf_busy || wf_share_on); i++) frame(250);
            e_wifi(conn0); au_reset();
        }
        if (!only || strstr("lastfm", only)) {
            au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            bool conn0 = wf_st.connected;
#ifdef SL_LFM_API
            e_au_lastfm(2); au_settle(); nbanner_hide_now(); pop_hide_now(); e_scroll_to(find_label(pages[npages - 1], "Send to Last.fm")); frame(400); e_shot(dir, "scrobble-lastfm-signed-in", lk, tag);
            au_reset();
            e_au_lastfm(3); au_settle(); nbanner_hide_now(); pop_hide_now(); e_scroll_to(find_label(pages[npages - 1], "Send to Last.fm")); frame(400); e_shot(dir, "scrobble-lastfm-lost", lk, tag);
            au_reset();
            e_au_lastfm(0); au_settle(); e_shot(dir, "lastfm-signin-qr", lk, tag);
            au_reset();
            e_au_lastfm(1); au_settle(); e_shot(dir, "lastfm-password", lk, tag);
            au_reset();
#endif
            scr_page(); au_settle(); nbanner_hide_now(); pop_hide_now(); e_scroll_to(find_label(pages[npages - 1], "Send to Last.fm")); frame(400); e_shot(dir, "scrobble-lastfm-off", lk, tag);
            au_reset(); e_wifi(conn0);
        }
        if (!only || strstr("libjob", only)) {
            au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
#ifdef SL_LIBJOB_UA
            for (int w = 0; w < 4; w++) {
                static const char *const nm[4] = { "lyrics-covers-idle", "lyrics-covers-running", "lyrics-covers-waiting", "lyrics-covers-failed" };
                e_au_libjob(w); au_settle(); if (w == 3) e_scroll_to(find_label(pages[npages - 1], "COVERS")); e_shot(dir, nm[w], lk, tag); au_reset();
            }
#endif
            sl_library_page(); au_settle(); e_scroll_to(find_label(pages[npages - 1], "Lyrics and covers")); e_shot(dir, "settings-library", lk, tag); au_reset();
        }
    }
    au_library_done();
}
