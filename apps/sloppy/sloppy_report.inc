/* GabagoolOS internals. GPLv2+. */
/* ------------------------------------------------------- bug reports */
/* Nothing here leaves the player by itself: a report is a file the user
 * makes (Settings > Advanced, or the phone remote's button) and posts if
 * they want to. Reports go public on GitHub issues, so every line is
 * scrubbed: no Bluetooth or Wi-Fi addresses, network names, accounts,
 * tokens, keys or server addresses. File names stay, they say what played. */

/* What was last asked for, so a slow moment says what it was part of. */
static char sl_doing[64] = "starting";
static void sl_doing_is(const char *what, const char *detail)
{
    if (detail && detail[0]) snprintf(sl_doing, sizeof sl_doing, "%s: %s", what, detail);
    else snprintf(sl_doing, sizeof sl_doing, "%s", what);
}

/* Record lag: for a day, every main-loop pass over 50 ms, every wait the
 * music's threads overran and every audio underrun is logged with what
 * was happening, then it turns itself off. The end time is kept on the
 * card, so a restart (or a freeze and a reset) keeps it on. */
#define LAG_FILE ROCKBOX_DIR "/sloppy/lag-until"
#define LAG_MS 50
#define LAG_DAY (24 * 60 * 60)
static long lag_until;                   /* seconds since 1970, 0: off */
static long lag_test_now;                /* the harness: the clock, 0 the real one */
static long lag_now(void) { return lag_test_now ? lag_test_now : (long)time(NULL); }
static void lag_load(void)
{
    char b[24] = "";
    int fd = open(LAG_FILE, O_RDONLY);
    if (fd < 0) return;
    int n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n > 0) { b[n] = '\0'; lag_until = atol(b); }
}
static void lag_set(int on)
{
    lag_until = on ? lag_now() + LAG_DAY : 0;
    if (!on) { remove(LAG_FILE); sl_log("lag: recording off"); return; }
    char b[24];
    int n = snprintf(b, sizeof b, "%ld\n", lag_until);
    int fd = open(LAG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { write(fd, b, n); close(fd); }
    sl_log("lag: recording for a day, every pass over %d ms and every underrun", LAG_MS);
}
static bool lag_on(void)
{
    if (!lag_until) return false;
    if (lag_now() < lag_until) return true;
    sl_log("lag: a day is up");
    lag_set(0);
    return false;
}
/* a few lines a second at most: a slow screen should not fill the card */
static bool lag_room(void)
{
    static uint32_t from;
    static int lines, held;
    uint32_t now = sl_ms();
    if ((int32_t)(now - from) >= 10000)
    {
        if (held) sl_log("lag: %d more not written", held);
        from = now; lines = held = 0;
    }
    if (lines >= 30) { held++; return false; }
    lines++;
    return true;
}
static const char *lag_audio_now(char *b, size_t n)
{
    int st = audio_status();
    snprintf(b, n, "%s%s", !(st & AUDIO_STATUS_PLAY) ? "stopped" : (st & AUDIO_STATUS_PAUSE) ? "paused" : "playing",
             (st & AUDIO_STATUS_PLAY) ? (sl_out_is_bt() ? " over Bluetooth" : " on the jack") : "");
    return b;
}
/* a main-loop pass: how long, and the step that took most of it */
static void lag_pass(uint32_t ms, const char *step, uint32_t step_ms)
{
    if (ms <= LAG_MS || !lag_on() || !lag_room()) return;
    char a[32];
    sl_log("lag: %u ms pass, %u ms in %s, doing %s; %s, %ld KiB free", (unsigned)ms, (unsigned)step_ms, step,
           sl_doing, lag_audio_now(a, sizeof a), sl_mem_available_kb());
}
/* a wait the player's other threads (the music, the database) overran */
static void lag_wait(uint32_t asked_ms, uint32_t slept_ms)
{
    if (slept_ms <= asked_ms + LAG_MS || !lag_on() || !lag_room()) return;
    char a[32];
    sl_log("lag: asked to wait %u ms, the music's threads held on for %u, doing %s; %s", (unsigned)asked_ms,
           (unsigned)slept_ms, sl_doing, lag_audio_now(a, sizeof a));
}
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
unsigned int pcm_alsa_get_rate(void);
unsigned int pcm_alsa_get_xruns(void);
#else
static unsigned lag_test_xruns;
static unsigned int pcm_alsa_get_xruns(void) { return lag_test_xruns; }
static unsigned int pcm_alsa_get_rate(void) { return 44100; }
#endif
static unsigned lag_xruns_seen;
static void lag_audio(void)
{
    unsigned x = pcm_alsa_get_xruns();
    if (x == lag_xruns_seen) return;
    unsigned d = x - lag_xruns_seen;
    lag_xruns_seen = x;
    if (!lag_on() || !lag_room()) return;
    char a[32];
    /* the decoded music low: the decoder was starved here (the UI, the card);
     * not low: the output or the Bluetooth link lost it on the way */
    sl_log("lag: %u audio underrun%s (%u in all), %u Hz, music buffer %s, doing %s; %s, %ld KiB free", d, d == 1 ? "" : "s",
           x, pcm_alsa_get_rate(), pcmbuf_is_lowdata() ? "low" : "not low", sl_doing, lag_audio_now(a, sizeof a),
           sl_mem_available_kb());
}

/* ------------------------------------------------------- the scrubber */
static bool rp_hex(char c) { return isxdigit((unsigned char)c); }
static bool rp_word(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'; }
/* one line, in place: what could name a person, a place or an account goes */
static void sl_report_scrub(char *s, size_t cap)
{
    char out[1024];
    size_t o = 0;
    static const char *const keys[] = { "password", "passwd", "psk", "token", "secret", "apikey", "api_key", "key",
                                        "ssid", "user", "username", "session", "sk", "name", "email" };
    /* Bluetooth, Wi-Fi and accounts: what they quote is a device's or a network's name */
    bool quoted = !strncmp(s, "bluetooth", 9) || !strncmp(s, "btrx", 4) || !strncmp(s, "wifi", 4) ||
                  !strncmp(s, "wireless", 8) || !strncmp(s, "scrobble", 8) || !strncmp(s, "subsonic", 8) ||
                  !strncmp(s, "remote", 6) || !strncmp(s, "dlna", 4) || strstr(s, "] bluetooth") ||
                  strstr(s, "] btrx") || strstr(s, "] wifi") || strstr(s, "] scrobble") || strstr(s, "] subsonic");
    for (size_t i = 0; s[i] && o + 16 < sizeof out; )
    {
        const char *p = s + i;
        /* an address: aa:bb:cc:dd:ee:ff, or BlueZ's dev_AA_BB_... */
        bool mac = true;
        for (int k = 0; k < 17 && mac; k++)
            mac = k % 3 == 2 ? (p[k] == ':' || p[k] == '_' || p[k] == '-') : rp_hex(p[k]);
        if (mac && !rp_hex(p[17]) && (i == 0 || !rp_hex(p[-1])))
        {
            o += snprintf(out + o, sizeof out - o, "xx:xx:xx:xx:xx:xx");
            i += 17;
            continue;
        }
        /* a web address, a server's or a station's: its host and path go */
        if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8))
        {
            o += snprintf(out + o, sizeof out - o, "<address>");
            while (s[i] && s[i] != ' ' && s[i] != ')' && s[i] != '"' && s[i] != '\'' && s[i] != ',') i++;
            continue;
        }
        /* an e-mail address */
        if (rp_word(*p) && (i == 0 || !rp_word(p[-1])))
        {
            size_t j = i;
            while (rp_word(s[j])) j++;
            if (s[j] == '@' && rp_word(s[j + 1]))
            {
                for (j++; rp_word(s[j]); j++);
                o += snprintf(out + o, sizeof out - o, "<email>");
                i = j;
                continue;
            }
        }
        /* an IPv4 address */
        if (isdigit((unsigned char)*p) && (i == 0 || !rp_word(p[-1])))
        {
            int dots = 0;
            size_t j = i;
            while (isdigit((unsigned char)s[j]) || (s[j] == '.' && isdigit((unsigned char)s[j + 1]))) dots += s[j++] == '.';
            if (dots == 3 && !rp_word(s[j]))
            {
                o += snprintf(out + o, sizeof out - o, "x.x.x.x");
                i = j;
                continue;
            }
        }
        /* key=value, key: value, "key":"value" */
        if (isalpha((unsigned char)*p) && (i == 0 || !rp_word(p[-1])))
        {
            size_t j = i;
            while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
            size_t kl = j - i;
            bool secret = false;
            for (unsigned k = 0; k < ARRAYLEN(keys) && !secret; k++)
                secret = strlen(keys[k]) == kl && !strncasecmp(p, keys[k], kl);
            size_t v = j;
            if (secret && s[v] == '"') v++;
            if (secret && (s[v] == '=' || s[v] == ':'))
            {
                v++;
                while (s[v] == ' ' || s[v] == '"' || s[v] == '\'') v++;
                if (s[v])
                {
                    memcpy(out + o, p, v - i);
                    o += v - i;
                    o += snprintf(out + o, sizeof out - o, "<hidden>");
                    while (s[v] && s[v] != ' ' && s[v] != ',' && s[v] != '&' && s[v] != '"' && s[v] != '\'' && s[v] != ')') v++;
                    i = v;
                    continue;
                }
            }
        }
        /* a quoted name, on the lines that name devices and networks */
        if (quoted && (*p == '\'' || *p == '"'))
        {
            const char *e = strchr(p + 1, *p);
            if (e)
            {
                o += snprintf(out + o, sizeof out - o, "%c<name>%c", *p, *p);
                i = e - s + 1;
                continue;
            }
        }
        out[o++] = s[i++];
    }
    out[o] = '\0';
    strlcpy(s, out, cap);
}

/* ------------------------------------------------------- the report */
extern const char rbversion[];           /* the build: its commit and day */
static bool rp_put(int fd, const char *s) { return write(fd, s, strlen(s)) == (ssize_t)strlen(s); }
/* the last `tail` bytes of a file, line by line through the scrubber */
static void rp_file(int fd, const char *title, const char *path, long tail)
{
    char head[160];
    snprintf(head, sizeof head, "\n----- %s -----\n", title);
    rp_put(fd, head);
    int in = open(path, O_RDONLY);
    if (in < 0) { rp_put(fd, "(none)\n"); return; }
    long size = lseek(in, 0, SEEK_END);
    long from = size > tail ? size - tail : 0;
    lseek(in, from, SEEK_SET);
    static char buf[4096], line[1024];
    size_t ln = 0;
    bool skip = from > 0;                /* a line cut in two at the start: from the next */
    int n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        for (int i = 0; i < n; i++)
        {
            char c = buf[i];
            if (c != '\n' && ln + 1 < sizeof line) { line[ln++] = c; continue; }
            if (c != '\n') continue;     /* an over-long line: its end goes */
            line[ln] = '\0';
            ln = 0;
            if (skip) { skip = false; continue; }
            sl_report_scrub(line, sizeof line);
            rp_put(fd, line);
            rp_put(fd, "\n");
        }
    if (ln && !skip) { line[ln] = '\0'; sl_report_scrub(line, sizeof line); rp_put(fd, line); rp_put(fd, "\n"); }
    close(in);
}
#ifndef SL_CARD_ROOT
#define SL_CARD_ROOT ""   /* the card is the R1's / (sloppy.c says so too, further down) */
#endif
/* Makes the report on the card's top and returns its path (in `path`). */
static bool sl_report_write(char *path, size_t n)
{
    struct tm *tm = get_time();
    snprintf(path, n, SL_CARD_ROOT "/gabagool-report-%04d-%02d-%02d-%02d%02d.txt", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { sl_log("report: %s not made, errno %d", path, errno); sloppy_card_error(errno); return false; }
    char b[512], a[32];
    unsigned long peak, rss = sl_perf_rss_kb(&peak);
    snprintf(b, sizeof b,
             "GabagoolOS bug report\n"
             "Version: %s (%s)\n"
             "Build: %s\n"
             "Player: HiBy R1, chip %s, wireless %s\n"
             "Made: %04d-%02d-%02d %02d:%02d, up %lu min\n"
             "Memory: %ld KiB free, the player %lu KiB (peak %lu), %ld KiB kept for the music\n"
             "Audio: %s, %u Hz, %u underruns since start\n"
             "Record lag: %s\n"
             "Doing: %s\n",
             SL_VERSION, SL_UPDATE_NAME, rbversion,
             sl_net_soc_name(sl_net_soc_id()), sl_net_wireless_aic() ? "AIC8800" : "Broadcom",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
             (unsigned long)(current_tick / HZ / 60),
             sl_mem_available_kb(), rss, peak, sl_mem_floor_kb(),
             lag_audio_now(a, sizeof a), pcm_alsa_get_rate(), pcm_alsa_get_xruns(),
             lag_until ? "on" : "off", sl_doing);
    sl_report_scrub(b, sizeof b);
    rp_put(fd, b);
    rp_file(fd, "crash.log (crashes and freezes, newest last)", ROCKBOX_DIR "/crash.log", 16 * 1024);
    rp_file(fd, "sloppy.prev2.log (two starts before)", ROCKBOX_DIR "/sloppy/sloppy.prev2.log", 32 * 1024);
    rp_file(fd, "sloppy.prev.log (the start before)", ROCKBOX_DIR "/sloppy/sloppy.prev.log", 64 * 1024);
    rp_file(fd, "sloppy.log (this start)", ROCKBOX_DIR "/sloppy/sloppy.log", 96 * 1024);
    rp_put(fd, "\n----- end -----\n");
    close(fd);
    sl_log("report: made %s", path);
    return true;
}
static void report_make_cb(lv_event_t *e)
{
    (void)e;
    char path[MAX_PATH], b[160];
    if (!sl_report_write(path, sizeof path)) { toast_warn("The bug report could not be saved"); return; }
    const char *name = strrchr(path, '/');
    snprintf(b, sizeof b, sl_tr("Bug report saved on the card: %s"), name ? name + 1 : path);
    toast(b);
}
