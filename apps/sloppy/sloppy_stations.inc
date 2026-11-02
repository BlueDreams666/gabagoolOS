/* Internet radio: stations from radio-browser.info (the open directory),
 * the most listened first or found by name; MP3, AAC, FLAC, Ogg Vorbis, Ogg
 * Opus and HLS streams, decoded by the network layer (sl_net_radio_*; video
 * left out of the lists) and played through the mixer the way
 * AirPlay is, resampled to whatever rate the mixer runs at. Music and the
 * radio take turns: starting one stops the other. Included by sloppy.c
 * after the Wi-Fi code it shares the mixer channel with. */
#define ST_API      "https://all.api.radio-browser.info/json/stations/"
#define ST_Q        "hidebroken=true&order=clickcount&reverse=true&limit=50"
#define ST_TOP_URL  ST_API "topclick/50?hidebroken=true"
#define ST_FIND_URL ST_API "search?" ST_Q "&name="
/* the country the player is in, from its address: plain HTTP, two letters back */
#define ST_WHERE_URL "http://ip-api.com/line/?fields=countryCode"
#define ST_SAVED    ROCKBOX_DIR "/sloppy/radio.txt"
#define ST_MAX 40
#define ST_SAVED_MAX 48
#define ST_GROUPS 1                       /* the harness checks these */
/* group: the heading it is under in radio.txt ("[Rock]"), "" none */
struct st_station { char name[64], url[256], alt[256], about[64], group[32]; };
static struct st_station *st_list;
static int st_n;
static bool st_loading, st_failed;     /* failed: the directory did not answer */
static char st_on_name[64], st_on_about[64];
static lv_obj_t *st_page_obj, *st_state_label, *st_find_ta;
static lv_timer_t *st_timer;
static bool st_active, st_told;
/* what the list shows: near you, the most listened, a kind of music, or a search */
enum { ST_NEAR, ST_TOP, ST_TAGS, ST_FOUND = -1 };
static const char *const st_kinds[] = { "Near you", "Most listened", "Jazz", "Lo-fi", "Classical", "Rock", "Pop",
                                        "Hip-hop", "Electronic", "Chill", "News" };
static const char *const st_tags[] = { NULL, NULL, "jazz", "lofi", "classical", "rock", "pop",
                                       "hip-hop", "electronic", "chillout", "news" };
static int st_kind = ST_NEAR;
static char st_cc[4], st_query[64];
static struct st_station st_now;          /* the one on air: the list may change under it */
static int st_try;                        /* which of its addresses is being tried */
static int st_redial;                     /* times it dropped and was dialled again */
static struct st_station st_saved[ST_SAVED_MAX];
static int st_nsaved;
static bool st_saved_read;

/* --- into the mixer, as AirPlay's ap_get_more */
#define ST_OUT 1024
static int16_t st_out[ST_OUT * 2], st_in[ST_OUT * 2 * 5];
static int16_t st_prev[2], st_next[2];
static uint32_t st_pos, st_step;
static void st_get_more(const void **start, size_t *size)
{
    uint32_t pops = (uint32_t)(((uint64_t)st_pos + (uint64_t)st_step * (ST_OUT - 1)) >> 16);
    if (pops > ST_OUT * 5) pops = ST_OUT * 5;
    size_t got = sl_net_radio_read(st_in, pops), used = 0;
    for (int i = 0; i < ST_OUT; i++)
    {
        while (st_pos >= 65536)
        {
            st_prev[0] = st_next[0];
            st_prev[1] = st_next[1];
            if (used < got) { st_next[0] = st_in[used * 2]; st_next[1] = st_in[used * 2 + 1]; used++; }
            else st_next[0] = st_next[1] = 0;            /* the station is late: silence, not noise */
            st_pos -= 65536;
        }
        for (int c = 0; c < 2; c++)
            st_out[i * 2 + c] = (int16_t)(st_prev[c] + (((int32_t)(st_next[c] - st_prev[c]) * (int32_t)st_pos) >> 16));
        st_pos += st_step;
    }
    *start = st_out;
    *size = sizeof st_out;
}
static bool st_on(void) { return st_on_name[0] != '\0'; }

/* The field key of one directory entry, between obj and end: a string
 * with its escapes undone, or a number as written. */
static bool st_field(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(obj, k);
    out[0] = '\0';
    if (!p || p >= end) return false;
    p += strlen(k);
    while (*p == ' ') p++;
    size_t n = 0;
    if (*p != '"')
    {
        while (p < end && *p && *p != ',' && *p != '}' && n + 1 < cap) out[n++] = *p++;
        out[n] = '\0';
        return n > 0;
    }
    for (p++; p < end && *p && *p != '"' && n + 1 < cap; p++)
    {
        if (*p != '\\') { out[n++] = *p; continue; }
        p++;
        if (*p == 'u' && p + 4 < end && n + 4 < cap)
        {
            unsigned v = (unsigned)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
            if (v < 0x80) out[n++] = (char)v;   /* as UTF-8: "Caf\u00e9" is Café */
            else if (v < 0x800) { out[n++] = (char)(0xc0 | v >> 6); out[n++] = (char)(0x80 | (v & 0x3f)); }
            else { out[n++] = (char)(0xe0 | v >> 12); out[n++] = (char)(0x80 | ((v >> 6) & 0x3f)); out[n++] = (char)(0x80 | (v & 0x3f)); }
            p += 4;
        }
        else if (*p) out[n++] = *p == 'n' ? ' ' : *p;
    }
    out[n] = '\0';
    return n > 0;
}
/* the directory's answer as a list of stations */
static void st_parse(const char *text)
{
    free(st_list);
    st_list = calloc(ST_MAX, sizeof *st_list);
    st_n = 0;
    for (const char *p = strstr(text, "\"changeuuid\""); st_list && p && st_n < ST_MAX; )
    {
        const char *next = strstr(p + 1, "\"changeuuid\""), *end = next ? next : p + strlen(p);
        struct st_station *s = &st_list[st_n];
        char tags[96], kbps[16], cc[8], codec[24] = "", hls[4] = "";
        st_field(p, end, "name", s->name, sizeof s->name);
        if (!st_field(p, end, "url_resolved", s->url, sizeof s->url)) st_field(p, end, "url", s->url, sizeof s->url);
        st_field(p, end, "url", s->alt, sizeof s->alt);   /* the station's own address: tried if the resolved one fails */
        st_field(p, end, "tags", tags, sizeof tags);
        st_field(p, end, "bitrate", kbps, sizeof kbps);
        st_field(p, end, "countrycode", cc, sizeof cc);
        st_field(p, end, "codec", codec, sizeof codec);
        st_field(p, end, "hls", hls, sizeof hls);
        /* what the radio can't play stays out of the list */
        if (sb_has(codec, "h.26") || sb_has(codec, "mp4v")) { p = next; continue; }
        char *comma = strchr(tags, ',');
        if (comma) *comma = '\0';
        /* the format when it is not plain MP3: AAC, FLAC, Ogg, Opus, HLS */
        const char *fmt = hls[0] == '1' ? "HLS" : !strncasecmp(codec, "AAC", 3) ? "AAC" : !strcasecmp(codec, "FLAC") ? "FLAC" :
                          !strcasecmp(codec, "OGG") ? "Ogg" : sb_has(codec, "opus") ? "Opus" : "";
        snprintf(s->about, sizeof s->about, "%s%s%s%s%s%s%s kbps", cc, cc[0] && tags[0] ? " \xc2\xb7 " : "", tags,
                 cc[0] || tags[0] ? " \xc2\xb7 " : "", fmt, fmt[0] ? " " : "", atoi(kbps) > 0 ? kbps : "128");
        if (s->name[0] && s->url[0]) st_n++;
        p = next;
    }
}

/* A station's n-th address to try: the resolved one, then its own; an
 * HTTPS one first as plain HTTP, which the player reads itself (wget's TLS
 * does not speak to every server). False past the last. */
static bool st_url(const struct st_station *s, int n, char *out)
{
    char all[4][256];
    int k = 0;
    const char *src[2] = { s->url, s->alt };
    for (int i = 0; i < 2; i++)
        for (int pass = 0; pass < 2; pass++)
        {
            char u[256];
            if (!src[i][0]) continue;
            bool tls = !strncmp(src[i], "https://", 8);
            if (pass == 0 && tls) snprintf(u, sizeof u, "http://%s", src[i] + 8);
            else if (pass == 1) strlcpy(u, src[i], sizeof u);
            else continue;
            bool seen = false;
            for (int j = 0; j < k; j++) seen |= !strcmp(all[j], u);
            if (!seen && k < 4) strlcpy(all[k++], u, sizeof all[0]);
        }
    if (n < 0 || n >= k) return false;
    strlcpy(out, all[n], 256);
    return true;
}

/* Saved stations, a line each in radio.txt: name, address, its own
 * address, about, tab between. The file may be written by hand on a
 * computer: "[Rock]" starts a heading the lines under it go under ("[]"
 * ends it), "#" starts a comment, a line may be just a name and an
 * address (a tab or " | " between), or only an address. Headings stay in
 * the order they come; stations under none come first. */
static char st_saved_buf[ST_SAVED_MAX * 700];   /* the file, read or being written */
static void st_trim(char *t)
{
    size_t n = strlen(t), i = 0;
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\r')) t[--n] = '\0';
    while (t[i] == ' ') i++;
    if (i) memmove(t, t + i, n - i + 1);
}
static void st_saved_line(char *line, const char *group)
{
    char *f[4] = { line, NULL, NULL, NULL };
    for (int i = 1; i < 4 && f[i - 1]; i++)
    {
        char *tab = strchr(f[i - 1], '\t');
        if (tab) { *tab = '\0'; f[i] = tab + 1; }
    }
    if (!f[1])                          /* by hand: "Name | address" */
    {
        char *bar = strstr(line, " | ");
        if (bar) { *bar = '\0'; f[1] = bar + 3; }
    }
    for (int i = 0; i < 4; i++) if (f[i]) st_trim(f[i]);
    if (!f[1] && (!strncasecmp(f[0], "http://", 7) || !strncasecmp(f[0], "https://", 8))) { f[1] = f[0]; f[0] = NULL; }
    if (!f[1] || !f[1][0]) return;
    struct st_station *st = &st_saved[st_nsaved++];
    memset(st, 0, sizeof *st);
    if (f[0] && f[0][0]) strlcpy(st->name, f[0], sizeof st->name);
    else
    {
        /* only an address: its host names it */
        const char *h = strstr(f[1], "://");
        h = h ? h + 3 : f[1];           /* "\texample" by hand: no scheme, and NULL + 3 was read */
        snprintf(st->name, sizeof st->name, "%.*s", (int)strcspn(h, ":/?"), h);
    }
    strlcpy(st->url, f[1], sizeof st->url);
    if (f[2]) strlcpy(st->alt, f[2], sizeof st->alt);
    if (f[3]) strlcpy(st->about, f[3], sizeof st->about);
    strlcpy(st->group, group, sizeof st->group);
}
static void st_saved_load(void)
{
    if (st_saved_read) return;
    st_saved_read = true;
    st_nsaved = 0;
    char *b = st_saved_buf;
    int fd = open(ST_SAVED, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof st_saved_buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (got <= 0) return;
    b[got] = '\0';
    char group[32] = "";
    char *line = strncmp(b, "SLR1\n", 5) ? b : b + 5;
    if (!strncmp(line, "\xef\xbb\xbf", 3)) line += 3;   /* a byte-order mark a text editor left */
    for (char *next; *line && st_nsaved < ST_SAVED_MAX; line = next)
    {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = '\0';
        char *t = line;
        while (*t == ' ') t++;
        if (!*t || *t == '#' || *t == '\r') continue;
        if (*t == '[')
        {
            char *e = strrchr(t, ']');
            if (!e) continue;
            *e = '\0';
            strlcpy(group, t + 1, sizeof group);
            st_trim(group);
            continue;
        }
        st_saved_line(t, group);
    }
}
static void st_saved_write(void)
{
    char *b = st_saved_buf;
    const size_t cap = sizeof st_saved_buf;
    int len = snprintf(b, cap, "SLR1\n");
    /* those under no heading, then each heading's in the order they come */
    for (int g = -1; g < st_nsaved; g++)
    {
        const char *group = g < 0 ? "" : st_saved[g].group;
        if (g >= 0)
        {
            bool first = group[0];
            for (int k = 0; first && k < g; k++) if (!strcmp(st_saved[k].group, group)) first = false;
            if (!first) continue;
            if (len < (int)cap) len += snprintf(b + len, cap - len, "[%s]\n", group);
        }
        for (int i = 0; i < st_nsaved && len < (int)cap; i++)
            if (!strcmp(st_saved[i].group, group))
                len += snprintf(b + len, cap - len, "%s\t%s\t%s\t%s\n",
                                st_saved[i].name, st_saved[i].url, st_saved[i].alt, st_saved[i].about);
    }
    if (len >= (int)cap) len = (int)cap - 1;
    int fd = open(ST_SAVED ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(ST_SAVED ".tmp", ST_SAVED) < 0) remove(ST_SAVED ".tmp");
}
static int st_saved_find(const char *url)
{
    for (int i = 0; i < st_nsaved; i++) if (!strcmp(st_saved[i].url, url)) return i;
    return -1;
}

static void webradio_page_at(bool replace);
static void st_refresh(void) { if (st_page_obj && npages && pages[npages - 1] == st_page_obj) webradio_page_at(true); }
static void st_end(const char *why)
{
    if (st_active) { mixer_channel_stop(PCM_MIXER_CHAN_VOICE); st_active = false; }
    sl_net_radio_stop();
    st_on_name[0] = '\0';
    if (st_timer) lv_timer_pause(st_timer);
    if (why) toast(why);
    mini_update();
    st_refresh();
}
/* The sleep timer's minutes are up: the radio and AirPlay stop too.
 * AirPlay waits for the phone to pause before it may play again. */
static bool sleep_streams_on(void) { return st_active || ap_active; }
static void sleep_stop_streams(void)
{
    if (st_on()) st_end(NULL);
    if (ap_active)
    {
        mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
        ap_active = false;
        ap_held_off = true;
    }
}
static void st_tick(lv_timer_t *t)
{
    (void)t;
    int s = sl_net_radio_state();
    if (st_active) reset_poweroff_timer();   /* playing: Auto power off waits, as it does for songs */
    if (st_active && is_playing()) { st_end("Radio off: your music is playing"); return; }
    if (s == SL_RADIO_FAILED)
    {
        char why[200], url[256];
        sl_net_radio_why(why, sizeof why);
        sl_log("radio: %s %s at %s (%s)", st_on_name, st_active ? "dropped out" : "did not answer",
               st_url(&st_now, st_try, url) ? url : "?", why);
        /* a station that played and dropped: dialled again, a few times */
        if (st_active && st_redial < 3 && st_url(&st_now, st_try, url))
        {
            mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
            st_active = false;
            st_redial++;
            if (sl_net_radio_start(url)) { sl_log("radio: dialling %s again (%d)", st_on_name, st_redial); return; }
        }
        /* the next address it has: its own after the resolved one, plain HTTP after HTTPS */
        if (!st_active && st_url(&st_now, st_try + 1, url) && sl_net_radio_start(url)) { st_try++; return; }
        st_end(st_active ? "The station dropped out" : "The station did not answer");
        return;
    }
    if (s == SL_RADIO_ENDED)
    {
        char url[256];
        if (st_active && st_redial < 3 && st_url(&st_now, st_try, url))
        {
            mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
            st_active = false;
            st_redial++;
            if (sl_net_radio_start(url)) { sl_log("radio: %s ended, dialling again (%d)", st_on_name, st_redial); return; }
        }
        st_end("The station stopped");
        return;
    }
    unsigned rate = sl_net_radio_rate();
    if (s == SL_RADIO_PLAYING && !st_active && rate && sl_net_radio_buffered() >= rate / 2)
    {
        if (ap_active) { mixer_channel_stop(PCM_MIXER_CHAN_VOICE); ap_active = false; }   /* one stream at a time */
        uint32_t fs = mixer_get_frequency();
        if (fs < 8000) fs = 44100;
        st_step = (uint32_t)(((uint64_t)rate << 16) / fs);
        st_pos = 65536;
        st_prev[0] = st_prev[1] = st_next[0] = st_next[1] = 0;
        const void *start;
        size_t size;
        st_get_more(&start, &size);
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, MIX_AMP_UNITY);
        mixer_channel_play_data(PCM_MIXER_CHAN_VOICE, st_get_more, start, size);
        st_active = true;
        sl_log("radio: %s at %u Hz", st_on_name, rate);
    }
    if (st_state_label)
    {
        char b[96], w[40];
        if (s == SL_RADIO_PLAYING && !st_active && rate)
        {
            /* how much of the half second it waits for has come */
            size_t have = sl_net_radio_buffered(), want = rate / 2;
            snprintf(w, sizeof w, "%s %d%%", sl_tr("Buffering"), (int)(have >= want ? 99 : have * 100 / want));
        }
        else snprintf(w, sizeof w, "%s", s == SL_RADIO_CONNECTING ? sl_tr("Tuning in") : sl_tr("On air"));
        char song[128];
        sl_net_radio_title(song, sizeof song);
        if (st_active && song[0]) snprintf(b, sizeof b, "%s", song);   /* what is playing, when the station says */
        else snprintf(b, sizeof b, "%s \xc2\xb7 %s", w, st_on_about);
        if (strcmp(lv_label_get_text(st_state_label), b)) lv_label_set_text(st_state_label, b);
    }
}
static void st_start(const struct st_station *s)
{
    char url[256];
    if (st_active) { mixer_channel_stop(PCM_MIXER_CHAN_VOICE); st_active = false; }
    if (is_playing()) audio_pause();      /* the radio takes over from the music */
    st_now = *s;
    st_try = 0;
    st_redial = 0;
    while (st_url(&st_now, st_try, url) && !sl_net_radio_start(url)) st_try++;
    if (!st_url(&st_now, st_try, url)) { st_on_name[0] = '\0'; toast_warn("The station did not answer"); return; }
    strlcpy(st_on_name, s->name, sizeof st_on_name);
    strlcpy(st_on_about, s->about, sizeof st_on_about);
    if (!st_timer) st_timer = lv_timer_create(st_tick, 100, NULL);
    lv_timer_resume(st_timer);
    mini_update();
    st_refresh();
}
static void st_play(int k) { if (k >= 0 && st_list && k < st_n) st_start(&st_list[k]); }
static void st_row_cb(lv_event_t *e) { st_play((int)(intptr_t)lv_event_get_user_data(e)); }
static void st_saved_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k >= 0 && k < st_nsaved) st_start(&st_saved[k]);
}
static void st_stop_cb(lv_event_t *e) { (void)e; st_end(NULL); }

static void st_list_done(bool ok, const char *text, int len)
{
    (void)len;
    st_loading = false;
    st_failed = !ok;
    if (ok) st_parse(text);
    st_refresh();
}
static void st_load(const char *url)
{
    if (st_loading || !wf_online()) return;
    if (!wf_fetch(url, 160 * 1024, st_list_done)) { toast("Wi-Fi is busy. Try again in a moment"); return; }
    st_loading = true;
    st_failed = false;
}
/* the list for st_kind; near you needs the country first */
static void st_where_done(bool ok, const char *text, int len);
static void st_load_kind(void)
{
    char url[256];
    if (st_kind == ST_NEAR && !st_cc[0])
    {
        if (st_loading || !wf_online()) return;
        if (!wf_fetch(ST_WHERE_URL, 64, st_where_done)) { toast("Wi-Fi is busy. Try again in a moment"); return; }
        st_loading = true;
        return;
    }
    if (st_kind == ST_NEAR) snprintf(url, sizeof url, ST_API "bycountrycodeexact/%s?" ST_Q, st_cc);
    else if (st_kind >= ST_TAGS) snprintf(url, sizeof url, ST_API "bytagexact/%s?" ST_Q, st_tags[st_kind]);
    else snprintf(url, sizeof url, ST_TOP_URL);
    st_load(url);
}
static void st_load_kind_async(void *unused) { (void)unused; st_load_kind(); st_refresh(); }
static void st_where_done(bool ok, const char *text, int len)
{
    (void)len;
    st_loading = false;
    if (ok && ((text[0] | 32) >= 'a' && (text[0] | 32) <= 'z') && ((text[1] | 32) >= 'a' && (text[1] | 32) <= 'z'))
    {
        st_cc[0] = (char)(text[0] & ~32);   /* as the directory writes it: GB */
        st_cc[1] = (char)(text[1] & ~32);
        st_cc[2] = '\0';
    }
    else { toast("Could not tell where you are"); st_kind = ST_TOP; }
    lv_async_call(st_load_kind_async, NULL);   /* the Wi-Fi job is still finishing */
}
static void st_kind_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == st_kind || st_loading) return;
    st_kind = k;
    free(st_list);
    st_list = NULL;
    st_n = 0;
    st_load_kind();
    st_refresh();
}
static void st_save_cb(lv_event_t *e)
{
    (void)e;
    st_saved_load();
    int i = st_saved_find(st_now.url);
    if (i >= 0)
    {
        memmove(&st_saved[i], &st_saved[i + 1], (size_t)(st_nsaved - i - 1) * sizeof st_saved[0]);
        st_nsaved--;
        toast("Removed from saved stations");
    }
    else
    {
        if (st_nsaved == ST_SAVED_MAX) st_nsaved--;    /* the oldest goes */
        memmove(&st_saved[1], &st_saved[0], (size_t)st_nsaved * sizeof st_saved[0]);
        st_saved[0] = st_now;
        st_saved[0].group[0] = '\0';
        st_nsaved++;
        toast("Station saved");
    }
    st_saved_write();
    st_refresh();
}
static void st_find_go_cb(lv_event_t *e)
{
    (void)e;
    if (!st_find_ta || !lv_textarea_get_text(st_find_ta)[0]) return;
    char q[200], url[sizeof ST_FIND_URL + 200];
    eq_url_part(q, sizeof q, sl_ta_text(st_find_ta));
    snprintf(url, sizeof url, ST_FIND_URL "%s", q);
    if (!wf_online()) { toast("Connect to Wi-Fi first"); return; }
    if (st_loading) return;
    strlcpy(st_query, sl_ta_text(st_find_ta), sizeof st_query);
    pl_utf8_trim(st_query);
    st_kind = ST_FOUND;
    st_load(url);
    page_pop();                           /* back to the stations, which fill in when they come */
}
static void st_find_deleted(lv_event_t *e) { (void)e; st_find_ta = NULL; }
static void st_find_page(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("Find a station", &p);
    lv_obj_add_event_cb(p, st_find_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    section(body, "STATION NAME");
    st_find_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(st_find_ta, true);
    lv_textarea_set_max_length(st_find_ta, 60);
    lv_obj_set_width(st_find_ta, lv_pct(100));
    lv_obj_set_style_text_font(st_find_ta, &ss_semibold_23, 0);
    lv_obj_set_style_bg_color(st_find_ta, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(st_find_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(st_find_ta, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(st_find_ta, SL_R_MD, 0);
    lv_obj_add_state(st_find_ta, LV_STATE_FOCUSED);
    wf_kb_build(p, st_find_ta, KB_TEXT, st_find_go_cb, "Search");
    page_push(p);
}

static void st_wifi_cb(lv_event_t *e) { (void)e; sl_wifi_page(); }
static void st_retry_cb(lv_event_t *e)
{
    (void)e;
    if (st_kind == ST_FOUND && st_query[0])
    {
        char q[200], url[sizeof ST_FIND_URL + 200];
        eq_url_part(q, sizeof q, st_query);
        snprintf(url, sizeof url, ST_FIND_URL "%s", q);
        st_load(url);
    }
    else st_load_kind();
    st_refresh();
}
static void st_dot_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void st_page_deleted(lv_event_t *e) { if (lv_event_get_target(e) == st_page_obj) { st_page_obj = NULL; st_state_label = NULL; } }
static void webradio_page_at(bool replace)
{
    lv_obj_t *p, *body = page_new("Internet radio", &p);
    st_saved_load();
    st_page_obj = p;
    st_state_label = NULL;
    lv_obj_add_event_cb(p, st_page_deleted, LV_EVENT_DELETE, NULL);
    /* find by name, a pill like the Music page's */
    lv_obj_t *bar = box(body);
    lv_obj_set_size(bar, lv_pct(100), 2 * SL_R_PILL);
    lv_obj_set_style_radius(bar, SL_R_PILL, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_margin_bottom(bar, 6, 0);
    hairline(bar);
    press_motion(bar);
    lv_obj_align(sq_glass(bar, C_SUB), LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_align(text(bar, "Find a station", &ss_semibold_21, C_SUB), LV_ALIGN_LEFT_MID, 56, 0);
    on_click(bar, st_find_page, NULL);
    if (st_on())
    {
        /* what is on: a card with a live dot, its state, and Stop */
        section(body, "ON AIR");
        lv_obj_t *c = box(body);
        lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
        lv_obj_set_style_bg_color(c, lv_color_mix(lv_color_hex(C_ACCENT), lv_color_hex(C_CARD), 40), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        hairline(c);
        lv_obj_set_style_pad_all(c, 18, 0);
        lv_obj_set_style_margin_bottom(c, 6, 0);
        lv_obj_t *dot = box(c);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_pos(dot, 0, 10);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dot);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
        lv_anim_set_duration(&a, SL_T_LOOP);
        lv_anim_set_playback_duration(&a, SL_T_LOOP);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, st_dot_opa);
        lv_anim_start(&a);
        lv_obj_t *t = text(c, st_on_name, &ss_semibold_26, 0xffffff);
        lv_obj_set_pos(t, 24, 0);
        lv_obj_set_width(t, W - 28 - 36 - 24);
        label_lines(t, 1);
        st_state_label = text(c, "", &ss_regular_18, C_SUB);
        lv_obj_set_pos(st_state_label, 24, 38);
        lv_obj_set_width(st_state_label, W - 28 - 36 - 24);
        label_lines(st_state_label, 1);
        int bw = (W - 28 - 36 - 12) / 2;
        lv_obj_t *stop = mn_pill(c, LV_SYMBOL_STOP "  Stop", bw, 2 * SL_R_PILL, true, st_stop_cb, NULL);
        lv_obj_set_pos(stop, 0, 76);
        bool saved = st_saved_find(st_now.url) >= 0;
        lv_obj_t *keep = mn_pill(c, saved ? LV_SYMBOL_OK "  Saved" : LV_SYMBOL_PLUS "  Save", bw, 2 * SL_R_PILL, false, st_save_cb, NULL);
        lv_obj_set_pos(keep, bw + 12, 76);
        st_tick(NULL);
    }
    /* saved: those under no heading, then each heading of radio.txt */
    for (int g = -1; g < st_nsaved; g++)
    {
        const char *group = g < 0 ? "" : st_saved[g].group;
        bool first = g < 0 || group[0];
        for (int k = 0; first && g >= 0 && k < g; k++) if (!strcmp(st_saved[k].group, group)) first = false;
        if (!first) continue;
        bool any = false;
        for (int k = 0; k < st_nsaved; k++)
        {
            if (strcmp(st_saved[k].group, group)) continue;
            if (!any)
            {
                char up[96];
                if (group[0]) section(body, sl_upper(group, up, sizeof up));
                else section(body, "SAVED");
                any = true;
            }
            /* a station written by hand says nothing about itself: its host does */
            char host[64] = "";
            const char *h = strstr(st_saved[k].url, "://");
            if (!st_saved[k].about[0] && h) snprintf(host, sizeof host, "%.*s", (int)strcspn(h + 3, ":/?"), h + 3);
            lv_obj_t *r = card_row(body, st_saved[k].name, st_saved[k].about[0] ? st_saved[k].about : host, ACT_SOON);
            lv_obj_remove_event_cb_with_user_data(r, card_row_cb, (void *)(intptr_t)ACT_SOON);
            on_click(r, st_saved_row_cb, (void *)(intptr_t)k);
        }
    }
    /* the kinds of list, a row of chips that scrolls sideways */
    lv_obj_t *chips = box(body);
    lv_obj_set_size(chips, lv_pct(100), 52);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 8, 0);
    lv_obj_set_style_margin_top(chips, 12, 0);
    lv_obj_add_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *chosen = NULL;
    for (int k = 0; k < (int)(sizeof st_kinds / sizeof st_kinds[0]); k++)
    {
        bool on = k == st_kind;
        lv_obj_t *ch = box(chips);
        lv_obj_set_size(ch, LV_SIZE_CONTENT, 44);
        lv_obj_set_style_pad_hor(ch, 18, 0);
        lv_obj_set_style_radius(ch, sl_zune() ? 0 : SL_R_FULL, 0);
        lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ch, lv_color_hex(on ? C_ACCENT : C_CARD), 0);
        lv_obj_set_style_bg_color(ch, on ? lv_color_darken(lv_color_hex(C_ACCENT), 50) : lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        if (!on) hairline(ch);
        press_motion(ch);
        lv_obj_center(text(ch, st_kinds[k], &ss_semibold_21, on ? sl_accent_ink() : 0xffffff));
        on_click(ch, st_kind_cb, (void *)(intptr_t)k);
        if (on) chosen = ch;
    }
    if (chosen) lv_obj_scroll_to_view(chosen, LV_ANIM_OFF);
    if (!replace) st_failed = false;      /* opened again: worth another try */
    if (!st_list && !st_loading && !st_failed) st_load_kind();   /* a failure waits for Try again */
    char head[96];
    if (st_kind == ST_FOUND) snprintf(head, sizeof head, "%s \xe2\x80\x9c%s\xe2\x80\x9d", sl_tr("RESULTS FOR"), st_query);
    else if (st_kind == ST_NEAR && st_cc[0]) snprintf(head, sizeof head, "%s %s", sl_tr("STATIONS IN"), st_cc);
    else sl_upper(sl_tr(st_kind >= 0 ? st_kinds[st_kind] : "Stations"), head, sizeof head);
    section(body, head);
    if (st_loading)
    {
        lv_obj_t *w = text(body, "Tuning in to the station list\xe2\x80\xa6", &ss_regular_18, C_SUB);
        lv_obj_set_style_pad_hor(w, 6, 0);
    }
    else if (!wf_online())
    {
        lv_obj_t *c = sl_empty(body, IC_WIFI, 72, "Not connected to Wi-Fi",
                               "Join a network to find stations from all over the world.", 24);
        sl_empty_action(c, LV_SYMBOL_WIFI "  Wi-Fi", st_wifi_cb, NULL);
    }
    else if (st_failed)
    {
        lv_obj_t *c = sl_empty(body, IC_RADIO, 72, "The station list didn't answer",
                               "The directory may be busy. Try again in a moment.", 24);
        sl_empty_action(c, LV_SYMBOL_REFRESH "  Try again", st_retry_cb, NULL);
    }
    else if (!st_list || !st_n)
        sl_empty(body, IC_RADIO, 72, st_kind == ST_FOUND ? "No stations by that name" : "No stations in this list",
                 st_kind == ST_FOUND ? "Check the spelling, or try a shorter name." : "Try another list above, or find one by name.", 24);
    for (int k = 0; st_list && k < st_n; k++)
    {
        lv_obj_t *r = card_row(body, st_list[k].name, st_list[k].about, ACT_SOON);
        lv_obj_remove_event_cb_with_user_data(r, card_row_cb, (void *)(intptr_t)ACT_SOON);
        on_click(r, st_row_cb, (void *)(intptr_t)k);
    }
    if (replace) page_replace(p, true); else page_push(p);
}
static void webradio_page(void) { webradio_page_at(false); }
