/* The phone remote, on the interface's side: the network thread hands over
 * one /api request at a time (sl_net_remote_take) and this answers it in
 * JSON on the interface's own thread, the only one that may touch the
 * player. Turned on from the Wi-Fi page, for this session only: anyone on
 * the same Wi-Fi can open the page while it is on. The page is an app a
 * phone can keep on its home screen (sloppy_remote_page.h, made from
 * sloppy-dev/remote/remote.html). Included by sloppy.c after the search
 * and taste code it uses.
 *
 * What the page asks, all GET:
 *   /api/now                     what plays (polled; cheap: no card access)
 *   /api/hello                   its words in the player's language, the accent
 *   /api/cmd?c=toggle|next|prev|up|down|love|shuffle|repeat|shot
 *   /api/seek?ms=  /api/vol?v=   a place in the song, a volume (0-100)
 *   /api/queue                   what plays after this one
 *   /api/jump?k=&r=              play the k-th of them (r: the list it saw)
 *   /api/search?q=  /api/play?t=&m=now|next|queue
 *   /api/update                  the software update's state, in a line of the player's words
 * and POST /update, an update file as the body (sloppy_net.c takes it to the card).
 * All but /api/hello only from a paired phone: sloppy_net.c answers
 * /pair itself, and this shows the code it makes (rm_pair_poll). */
#ifdef HAVE_ALBUMART
#include "recorder/albumart.h"
#endif
static lv_timer_t *rm_timer;
static char rm_out[24 * 1024];
static unsigned rm_cost_us, rm_cost_max_us, rm_cost_n;   /* the last answer's time on this thread, for the tests */
static uint64_t rm_cost_sum_us;
static lv_obj_t *rm_code;                /* the Wi-Fi page's code to scan, while it is open */

/* the page's words: each is looked up in the player's language */
static const char *const rm_words[] = {
    "Nothing playing", "Play", "Pause", "Previous song", "Next song", "Shuffle", "Repeat", "Repeat one",
    "Love", "Volume", "Position", "Up next", "Nothing up next", "Queued", "Search",
    "Songs, artists, albums", "Nothing found", "Play next", "Playing next", "Add to queue", "Added to queue",
    "Take screenshot", "Not connected", "Keep it on your home screen", "Tap Share, then Add to Home Screen.",
    "Open the browser menu, then Add to Home screen.", "Done", "Software update", "Send an update file",
    "Pair this phone", "Type the code the player shows.", "Pair", "That code is not right.",
    "Too many tries. Wait a minute, then try again.", "That code ran out. Type the new one.",
};

/* s as a JSON string, quotes included; control bytes escaped, UTF-8 kept */
/* fmt at p, the buffer ending at end (its last byte): p never goes past
 * it, however long what is written, so a cut answer stays in bounds */
static char *rm_fmt(char *p, char *end, const char *fmt, ...)
{
    if (p >= end) return p;
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(p, (size_t)(end - p), fmt, a);
    va_end(a);
    return k < 0 ? p : k < end - p ? p + k : end - 1;
}
static char *rm_str(char *p, char *end, const char *s)
{
    if (p < end) *p++ = '"';
    for (; s && *s && p + 7 < end; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c < 0x20) p = rm_fmt(p, end, "\\u%04x", c);
        else *p++ = (char)c;
    }
    if (p < end) *p++ = '"';
    *p = '\0';
    return p;
}
/* s written out in at most cap bytes, so an entry of a few always fits its own buffer */
static char *rm_str_max(char *p, char *end, const char *s, size_t cap)
{
    return rm_str(p, end - p > (ptrdiff_t)cap ? p + cap : end, s);
}
/* one list entry, made whole in e (up to eend), added to the answer only if it fits with room to close it */
static bool rm_add(char **p, char *end, const char *e, const char *eend)
{
    size_t k = (size_t)(eend - e);
    if (*p + k + 3 >= end) return false;
    memcpy(*p, e, k);
    *p += k;
    **p = '\0';
    return true;
}
static unsigned rm_hash(unsigned h, const char *s)
{
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}
/* the cover of what plays: found once a song, its bytes left where they are
 * for the server thread to send. A tag's picture first, as the covers
 * everywhere else, then a cover file in the folder. */
static char rm_cov_track[MAX_PATH];
static unsigned rm_cov_key;
static void rm_cover_find(const struct mp3entry *id3)
{
    if (!id3) { if (rm_cov_track[0]) { rm_cov_track[0] = '\0'; rm_cov_key = 0; sl_net_remote_cover("", 0, 0, 0, 0); } return; }
    if (!strcmp(rm_cov_track, id3->path)) return;
    strlcpy(rm_cov_track, id3->path, sizeof rm_cov_track);
    char src[MAX_PATH] = "";
    long off = 0, size = -1;
    int kind = 0;
#ifdef HAVE_ALBUMART
    if (id3->has_embedded_albumart && !(id3->albumart.type & AA_FLAG_VORBIS_BASE64))
    {
        int ty = id3->albumart.type & AA_CLEAR_FLAGS_MASK;
        if (ty == AA_TYPE_JPG || ty == AA_TYPE_PNG)
        {
            strlcpy(src, id3->path, sizeof src);
            off = id3->albumart.pos;
            size = id3->albumart.size;
            kind = (ty == AA_TYPE_PNG ? SL_REMOTE_COVER_PNG : SL_REMOTE_COVER_JPEG) |
                   (id3->albumart.type & AA_FLAG_ID3_UNSYNC ? SL_REMOTE_COVER_UNSYNC : 0);
        }
    }
    if (!kind && search_albumart_files(id3, "", src, sizeof src))
    {
        size_t n = strlen(src);
        const char *ext = n > 4 ? src + n - 4 : "";
        if (!strcasecmp(ext, ".png")) kind = SL_REMOTE_COVER_PNG;
        else if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, "jpeg")) kind = SL_REMOTE_COVER_JPEG;
        off = 0;
        size = -1;
    }
#endif
    rm_cov_key = kind ? (rm_hash(rm_hash(2166136261u, src), id3->path) ^ (unsigned)off) | 1u : 0;
    sl_net_remote_cover(kind ? src : "", off, size, kind, rm_cov_key);
}
/* what plays after this one changes this; the queue's own moves bump it too */
static unsigned rm_qbump;
static unsigned rm_qrev(void)
{
    if (!something_playing()) return 0;
    unsigned h = 2166136261u;
    int v[] = { q_cur(), playlist_amount(), global_settings.playlist_shuffle, global_settings.repeat_mode, q_run(), (int)rm_qbump };
    for (unsigned i = 0; i < ARRAYLEN(v); i++) h = (h ^ (unsigned)v[i]) * 16777619u;
    return h | 1u;
}
/* the page asks again for its words and colours when this moves */
static unsigned rm_cfg(void)
{
    return (((unsigned)global_settings.sloppy_language * 31u + (unsigned)sl_accent()) * 31u +
            (unsigned)global_settings.sloppy_love_star) | 1u;
}
/* what plays, as the page shows it */
static void rm_now(char *b, size_t n)
{
    char *p = b, *end = b + n - 1;
    struct mp3entry *id3 = audio_current_track();
    char name[64];
    long cs = 0, cl = 0;
    int t = id3 ? sl_lib_find_path_at(id3->path, (long)id3->elapsed) : -1;
    if (id3) cue_now(id3, &cs, &cl);
    rm_cover_find(id3);
    sl_net_remote_tint(sl_accent(), sl_ink_on(sl_accent()));   /* the icon, in the accent of now */
    long at = id3 ? (long)visible_elapsed(id3) - cs : 0;
    p = rm_fmt(p, end, "{\"title\":");
    p = rm_str(p, end, id3 ? track_title(id3, name, sizeof name) : "");
    p = rm_fmt(p, end, ",\"artist\":");
    p = rm_str(p, end, id3 ? track_artist(id3) : "");
    p = rm_fmt(p, end, ",\"album\":");
    p = rm_str(p, end, t >= 0 ? sl_lib.albums[sl_lib.tracks[t].album].title : id3 && id3->album ? id3->album : "");
    rm_fmt(p, end,
             ",\"playing\":%s,\"pos\":%ld,\"len\":%ld,\"vol\":%d,\"loved\":%s,\"shuffle\":%s,\"repeat\":%d,"
             "\"cover\":%u,\"qrev\":%u,\"cfg\":%u,\"accent\":\"#%06lx\",\"shot\":%u}",
             is_playing() ? "true" : "false", at < 0 ? 0 : at, cl < 0 ? 0 : cl, volume_pct(),
             t >= 0 && ts_rating(t) == 1 ? "true" : "false", global_settings.playlist_shuffle ? "true" : "false",
             global_settings.repeat_mode == REPEAT_ONE ? 2 : global_settings.repeat_mode == REPEAT_ALL ? 1 : 0,
             rm_cov_key, rm_qrev(), rm_cfg(), (unsigned long)(C_ACCENT & 0xffffff), sl_shot_seq());
}
/* its words and the accent for both of its looks: as on our own cards,
 * lifted (or deepened) until they read on the surface */
static void rm_hello(char *b, size_t n)
{
    int lang = global_settings.sloppy_language;
    char *p = b, *end = b + n - 1;
    uint32_t d = sl_accent_on(0x18202b, 30), dt = sl_accent_on(0x18202b, 45),
             l = sl_accent_on(0xffffff, 30), lt = sl_accent_on(0xffffff, 45);
    sl_net_remote_tint(sl_accent(), sl_ink_on(sl_accent()));
    p = rm_fmt(p, end,
                  "{\"lang\":\"%s\",\"dir\":\"%s\",\"star\":%s,\"cfg\":%u,"
                  "\"acc\":{\"d\":\"#%06lx\",\"dt\":\"#%06lx\",\"di\":\"#%06lx\",\"l\":\"#%06lx\",\"lt\":\"#%06lx\",\"li\":\"#%06lx\"},\"w\":{",
                  sl_lang_code(lang), lang == 4 || lang == 5 ? "rtl" : "ltr",
                  global_settings.sloppy_love_star ? "true" : "false", rm_cfg(),
                  (unsigned long)d, (unsigned long)dt, (unsigned long)sl_ink_on(d),
                  (unsigned long)l, (unsigned long)lt, (unsigned long)sl_ink_on(l));
    for (unsigned i = 0; i < ARRAYLEN(rm_words) && p + 400 < end; i++)
    {
        if (i) *p++ = ',';
        p = rm_str(p, end, rm_words[i]);
        *p++ = ':';
        p = rm_str(p, end, sl_tr(rm_words[i]));
    }
    rm_fmt(p, end, "}}");
}
/* what plays after this one, as Up next shows it: the queue first */
#define RM_QUEUE_MAX 30
static void rm_queue(char *b, size_t n)
{
    char *p = b, *end = b + n - 1, title[160];
    int ahead = something_playing() ? q_ahead() : 0, shown = 0;
    p = rm_fmt(p, end, "{\"rev\":%u,\"more\":%d,\"items\":[", rm_qrev(), ahead > RM_QUEUE_MAX ? ahead - RM_QUEUE_MAX : 0);
    for (int k = 1; k <= ahead && shown < RM_QUEUE_MAX; k++)
    {
        struct playlist_track_info info;
        int idx = q_at(k);
        if (playlist_get_track_info(NULL, idx, &info) < 0) break;
        int t = sl_lib_find_path(info.filename);
        if (t >= 0 && sl_lib.tracks[t].title) strlcpy(title, sl_lib.tracks[t].title, sizeof title);
        else
        {
            const char *s = strrchr(info.filename, '/');
            strlcpy(title, s ? s + 1 : info.filename, sizeof title);
            char *dot = strrchr(title, '.');
            if (dot && dot != title) *dot = '\0';
        }
        char one[1200], *e = one, *ee = one + sizeof one - 1;
        e = rm_fmt(e, ee, "%s{\"k\":%d,\"q\":%s,\"title\":", shown ? "," : "", k, q_is_queued(idx) ? "true" : "false");
        e = rm_str_max(e, ee, title, 500);
        e = rm_fmt(e, ee, ",\"artist\":");
        e = rm_str_max(e, ee, t >= 0 ? sl_track_artist(t) : "", 500);
        e = rm_fmt(e, ee, "}");
        if (!rm_add(&p, end, one, e)) break;
        shown++;
    }
    rm_fmt(p, end, "]}");
}
static void rm_cmd(const char *c)
{
    if (!strcmp(c, "toggle")) toggle_play();
    else if (!strcmp(c, "next")) np_skip(1);
    else if (!strcmp(c, "prev")) np_skip(-1);
    else if (!strcmp(c, "up")) volume_key(1);
    else if (!strcmp(c, "down")) volume_key(-1);
    else if (!strcmp(c, "shuffle")) { if (something_playing()) np_toggle_shuffle(); }
    else if (!strcmp(c, "repeat")) np_cycle_repeat();
    else if (!strcmp(c, "shot")) sl_screenshot();   /* the screen as it is; the page shows it once it is saved */
    else if (!strcmp(c, "love"))
    {
        struct mp3entry *id3 = audio_current_track();
        int t = id3 ? sl_lib_find_path_at(id3->path, (long)id3->elapsed) : -1;
        if (t >= 0) ts_love(t, ts_rating(t) != 1);
        np_love_paint();
    }
}
static void rm_seek(const char *query)
{
    char v[16];
    struct mp3entry *id3 = audio_current_track();
    if (!sl_net_query_param(query, "ms", v, sizeof v) || !id3 || !id3->length) return;
    long cs, cl, ms = atol(v);
    cue_now(id3, &cs, &cl);
    if (ms < 0) ms = 0;
    if (ms > cl - 500) ms = cl > 500 ? cl - 500 : 0;   /* not past the end: that would be a skip */
    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
    audio_ff_rewind(cs + ms);
}
static void rm_vol(const char *query)
{
    char v[8];
    if (!sl_net_query_param(query, "v", v, sizeof v) || !v[0]) return;
    int volume = volume_from_pct(LV_CLAMP(0, atoi(v), 100));
    if (volume != global_status.volume) { global_status.volume = volume; setvol(); statusbar_update(); }
    hud_show();                          /* the player shows the level the phone set (and makes the bar the first time) */
}
static void rm_jump(const char *query)
{
    char k[8], r[16];
    sl_net_query_param(query, "k", k, sizeof k);
    sl_net_query_param(query, "r", r, sizeof r);
    int steps = (unsigned)strtoul(r, NULL, 10) == rm_qrev() && atoi(k) > 0 && atoi(k) <= q_ahead() ? q_jump_steps(atoi(k)) : -1;
    if (steps > 0) { audio_skip(steps, 0); rm_qbump++; }
    if (uq_open) uq_rebuild_soon();
}
static int *rm_scores;
static int rm_by_score(const void *a, const void *b) { return rm_scores[*(const int *)b] - rm_scores[*(const int *)a]; }
/* songs for a search, ranked as the player's own search ranks them */
static void rm_search(const char *query, char *b, size_t n)
{
    char *p = b, *end = b + n - 1;
    char q[SL_Q_MAX], *words[6], t[256], ar[256], al[256];
    sl_q_fold(query, q, sizeof q);
    int nw = sl_q_words(q, words), nt = sl_lib.n_tracks, count = 0;
    int *songs = nw ? malloc((nt ? nt : 1) * sizeof *songs) : NULL;
    rm_scores = nw ? calloc(nt ? nt : 1, sizeof *rm_scores) : NULL;
    for (int i = 0; songs && rm_scores && i < nt; i++)
    {
        const struct sl_track *tr = &sl_lib.tracks[i];
        sl_q_fold(tr->title, t, sizeof t);
        sl_q_fold(sl_track_artist(i), ar, sizeof ar);
        sl_q_fold(sl_lib.albums[tr->album].title, al, sizeof al);
        int s = sl_q_score(words, nw, t, ar, al);
        if (s) { rm_scores[i] = s; songs[count++] = i; }
    }
    if (count) qsort(songs, count, sizeof *songs, rm_by_score);
    p = rm_fmt(p, end, "{\"songs\":[");
    for (int k = 0; k < count && k < 50; k++)
    {
        const struct sl_track *tr = &sl_lib.tracks[songs[k]];
        char one[1700], *e = one, *ee = one + sizeof one - 1;
        e = rm_fmt(e, ee, "%s{\"t\":%d,\"title\":", k ? "," : "", songs[k]);
        e = rm_str_max(e, ee, tr->title, 500);
        e = rm_fmt(e, ee, ",\"artist\":");
        e = rm_str_max(e, ee, sl_track_artist(songs[k]), 500);
        e = rm_fmt(e, ee, ",\"album\":");
        e = rm_str_max(e, ee, sl_lib.albums[tr->album].title, 500);
        e = rm_fmt(e, ee, "}");
        if (!rm_add(&p, end, one, e)) break;   /* the answer is full: the rest left out, the list still whole */
    }
    rm_fmt(p, end, "]}");
    free(songs);
    free(rm_scores);
    rm_scores = NULL;
}
static void rm_play(const char *query)
{
    char v[16], m[16];
    sl_net_query_param(query, "t", v, sizeof v);
    sl_net_query_param(query, "m", m, sizeof m);
    int t = atoi(v);
    if (!v[0] || t < 0 || t >= sl_lib.n_tracks) return;
    if (!strcmp(m, "next")) q_play_next(t);
    else if (!strcmp(m, "queue")) q_add(t);
    else if (play_list(&t, 1, 0, false)) np_show_auto();
    rm_qbump++;
}
/* the update, as the page shows it under its button */
static void rm_update(char *b, size_t n)
{
    static const char *const names[] = { "none", "looking", "found", "rejected", "working", "ready", "receiving" };
    struct sl_ota_status s;
    char line[400];
    sl_ota_status(&s);
    up_line_words(&s, line, sizeof line);
    char *p = b, *end = b + n - 1;
    p = rm_fmt(p, end, "{\"state\":\"%s\",\"key\":%s,\"busy\":%s,\"version\":",
                  names[s.state >= 0 && s.state <= SL_OTA_RECEIVING ? s.state : 0], s.key ? "true" : "false",
                  s.state == SL_OTA_LOOKING || s.state == SL_OTA_WORKING || s.state == SL_OTA_RECEIVING ? "true" : "false");
    p = rm_str(p, end, s.version);
    p = rm_fmt(p, end, ",\"line\":");
    p = rm_str(p, end, line);
    rm_fmt(p, end, "}");
}
static uint64_t rm_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* ---- on the player: a phone asks to pair, and its code waits on a page
 * of its own until the phone sends it back, it runs out, or Don't pair */
static lv_obj_t *rm_pair_obj;
static unsigned rm_pair_shown;
static int rm_pair_n = -1;
static void rm_pair_close(void)
{
    if (rm_pair_obj && npages && pages[npages - 1] == rm_pair_obj) { page_pop(); rm_pair_obj = NULL; }   /* gone as it slides away */
}
static void rm_pair_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != rm_pair_obj) return;
    rm_pair_obj = NULL;
    sl_net_remote_pair_cancel();          /* Back is Don't pair; typed or run out, there is none left anyway */
}
static void rm_pair_no_cb(lv_event_t *e) { (void)e; sl_net_remote_pair_cancel(); rm_pair_close(); }
static void rm_pair_page(const char *code)
{
    lv_obj_t *page, *body = page_new("Phone remote", &page);
    rm_pair_obj = page;
    lv_obj_add_event_cb(page, rm_pair_deleted, LV_EVENT_DELETE, NULL);
    sl_bt_code_row(set_group(body, "PAIRING CODE"), code);
    opt_note_centred(body, "Type this code on your phone to let it control the player.");
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action(g, LV_SYMBOL_CLOSE, 0x66738a, "Don't pair", NULL, rm_pair_no_cb, NULL);
    page_push(page);
}
static void rm_pair_poll(void)
{
    char code[8];
    unsigned serial;
    bool on = sl_net_remote_pair_code(code, sizeof code, &serial);
    if (on && serial != rm_pair_shown)
    {
        rm_pair_shown = serial;
        rm_pair_close();
        rm_pair_page(code);
    }
    else if (!on && rm_pair_obj) rm_pair_close();
    int n = sl_net_remote_paired();
    if (rm_pair_n >= 0 && n > rm_pair_n) toast("Phone paired");
    rm_pair_n = n;
}
static void rm_forget_cb(lv_event_t *e)
{
    (void)e;
    sl_net_remote_forget();
    rm_pair_n = 0;
    toast("Paired phones forgotten");
}
static void rm_tick(lv_timer_t *timer)
{
    (void)timer;
    char path[64], query[256], v[128];
    rm_pair_poll();
    if (!sl_net_remote_take(path, sizeof path, query, sizeof query)) return;
    uint64_t t0 = rm_us();
    if (!strcmp(path, "/api/search"))
    {
        sl_net_query_param(query, "q", v, sizeof v);
        rm_search(v, rm_out, sizeof rm_out);
    }
    else if (!strcmp(path, "/api/hello")) rm_hello(rm_out, sizeof rm_out);
    else if (!strcmp(path, "/api/queue")) rm_queue(rm_out, sizeof rm_out);
    else if (!strcmp(path, "/api/update")) rm_update(rm_out, sizeof rm_out);
    else
    {
        if (!strcmp(path, "/api/cmd") && sl_net_query_param(query, "c", v, sizeof v)) rm_cmd(v);
        else if (!strcmp(path, "/api/play")) rm_play(query);
        else if (!strcmp(path, "/api/seek")) rm_seek(query);
        else if (!strcmp(path, "/api/vol")) rm_vol(query);
        else if (!strcmp(path, "/api/jump")) rm_jump(query);
        rm_now(rm_out, sizeof rm_out);
    }
    rm_cost_us = (unsigned)(rm_us() - t0);
    if (rm_cost_us > rm_cost_max_us) rm_cost_max_us = rm_cost_us;
    rm_cost_sum_us += rm_cost_us;
    rm_cost_n++;
    sl_net_remote_answer(rm_out);
}
static void rm_set(int on)
{
    if (on && !sl_net_remote_start()) { toast_warn("The remote did not start"); return; }
    if (!on) { sl_net_remote_stop(); sl_net_remote_pair_cancel(); rm_pair_close(); }
    if (on) sl_net_remote_tint(sl_accent(), sl_ink_on(sl_accent()));
    if (on && !rm_timer) rm_timer = lv_timer_create(rm_tick, 50, NULL);
    if (rm_timer) { if (on) lv_timer_resume(rm_timer); else lv_timer_pause(rm_timer); }
    rm_cov_track[0] = '\0';               /* found again, on the next ask */
    if (rm_code) hidden_if_changed(rm_code, !on);
    if (on) toast("Remote on: open the address on your phone");
}

/* ---- on the player: the code to scan, inside the Wi-Fi page's Phone remote card */
static void rm_code_deleted(lv_event_t *e) { (void)e; rm_code = NULL; }
static void rm_code_row(lv_obj_t *card, const char *url)
{
    lv_obj_t *r = box(card);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, SL_INSET, 0);
    lv_obj_set_style_pad_ver(r, SL_INSET, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(r, SL_GUTTER, 0);
    qr_tile(r, url, 200);
    lv_obj_t *a = text(r, url, &ss_bold_28, C_ACCENT_TXT);   /* as the transfer page writes its own */
    lv_obj_set_style_margin_top(a, -SL_SP_S, 0);
    lv_obj_t *n = text(r, "Scan with your phone's camera. Add the page to your home screen to keep it as an app.",
                       &ss_regular_16, C_SUB);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    sl_label_deep(n);
    set_sep(card, r, SL_INSET);
    rm_code = r;
    lv_obj_add_event_cb(r, rm_code_deleted, LV_EVENT_DELETE, NULL);
    if (!sl_net_remote_on()) lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
}
