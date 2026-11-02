/* GabagoolOS listening history, smart playlists and Wrapped. GPLv2+.
 * Included by sloppy.c after the quiz (clips are not music listening).
 * hist_poll() runs with the 250 ms UI tick and appends a record to
 * plays.dat when a song counts as played. The For you page reads the log
 * once and builds Recently added, Most played, Recently played, Not played
 * yet and Forgotten favorites from it, plus a yearly Wrapped page. */
#include "sloppy_history_state.h"
#include "sloppy_grave_state.h"

#define HIST_FILE ROCKBOX_DIR "/sloppy/plays.dat"
#define HIST_LIST_MAX 100

static struct sl_hist_tracker hist_tracker;
static void grave_skipped(uint32_t hash);
static void grave_played(uint32_t hash);
static void grave_started(uint32_t hash);
static void grave_flush(bool now);
static void grave_page(void);

static uint32_t hist_clock(void)
{
    struct tm *clock = get_time();
    if (!clock) return 0;
    struct tm copy = *clock;
    time_t value = mktime(&copy);
    return value > 0 ? (uint32_t)value : 0;
}

#define HIST_REC ((off_t)sizeof(struct sl_play))

static void hist_append(const struct sl_play *p)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(HIST_FILE, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return;
    uint32_t magic = 0;
    off_t size = lseek(fd, 0, SEEK_END);
    bool ok = size >= 0;
    if (ok && size >= 4)
        ok = lseek(fd, 0, SEEK_SET) == 0 && read(fd, &magic, sizeof magic) == (ssize_t)sizeof magic;
    if (ok && (size < 4 || magic != SL_HIST_MAGIC))
    {
        /* new, or not a log we can trust: start it over */
        magic = SL_HIST_MAGIC;
        ok = ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0 &&
             write(fd, &magic, sizeof magic) == (ssize_t)sizeof magic;
        size = 4;
    }
    /* A power cut mid-write leaves part of a record: drop it, or every
     * later record would be read shifted. */
    off_t whole = 4 + (size - 4) / HIST_REC * HIST_REC;
    if (ok && whole != size) ok = ftruncate(fd, whole) == 0;
    if (ok && lseek(fd, whole, SEEK_SET) == whole)
        write(fd, p, sizeof *p);
    close(fd);
}

/* The songs heard last, newest first, as library tracks, each with when
 * it was heard: for Up next's history, grouped by day. Read once from the
 * end of the log (so it outlasts a restart), then kept as songs count. */
#define HIST_RECENT 40
static int hist_recent_t[HIST_RECENT], hist_recent_n = -1;
static uint32_t hist_recent_w[HIST_RECENT];
static char hist_cur_path[MAX_PATH];      /* the song the tracker follows */
static void hist_recent_push(int t, uint32_t when)
{
    if (t < 0 || hist_recent_n < 0) return;
    int n = hist_recent_n;
    for (int i = 0; i < n; i++)
        if (hist_recent_t[i] == t)
        {
            memmove(&hist_recent_t[i], &hist_recent_t[i + 1], (size_t)(n - i - 1) * sizeof(int));
            memmove(&hist_recent_w[i], &hist_recent_w[i + 1], (size_t)(n - i - 1) * sizeof(uint32_t));
            n--;
            break;
        }
    if (n == HIST_RECENT) n--;
    memmove(&hist_recent_t[1], &hist_recent_t[0], (size_t)n * sizeof(int));
    memmove(&hist_recent_w[1], &hist_recent_w[0], (size_t)n * sizeof(uint32_t));
    hist_recent_t[0] = t;
    hist_recent_w[0] = when;
    hist_recent_n = n + 1;
}
static int hist_recent_when(int *out, uint32_t *when, int max)
{
    if (hist_recent_n < 0)
    {
        hist_recent_n = 0;
        struct sl_play p[HIST_RECENT];
        int fd = open(HIST_FILE, O_RDONLY), got = 0;
        uint32_t magic = 0;
        off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
        if (size >= 4 + HIST_REC && lseek(fd, 0, SEEK_SET) == 0 &&
            read(fd, &magic, sizeof magic) == (ssize_t)sizeof magic && magic == SL_HIST_MAGIC)
        {
            off_t records = (size - 4) / HIST_REC, from = records > HIST_RECENT ? records - HIST_RECENT : 0;
            if (lseek(fd, 4 + from * HIST_REC, SEEK_SET) == 4 + from * HIST_REC)
            {
                ssize_t r = read(fd, p, (size_t)(records - from) * sizeof p[0]);
                got = r > 0 ? (int)(r / HIST_REC) : 0;
            }
        }
        if (fd >= 0) close(fd);
        /* oldest first, so the pushes leave the newest on top */
        int track[HIST_RECENT], left = got;
        for (int i = 0; i < got; i++) track[i] = -1;
        for (int t = 0; t < sl_lib.n_tracks && left; t++)
        {
            uint32_t h = sl_track_hist_hash(t);
            for (int i = 0; i < got; i++) if (track[i] < 0 && p[i].path_hash == h) { track[i] = t; left--; }
            if ((t & 1023) == 1023) yield();
        }
        for (int i = 0; i < got; i++) hist_recent_push(track[i], p[i].when);
    }
    int n = hist_recent_n < max ? hist_recent_n : max;
    memcpy(out, hist_recent_t, (size_t)n * sizeof *out);
    if (when) memcpy(when, hist_recent_w, (size_t)n * sizeof *when);
    return n;
}

/* a music server's songs are told to it too (sloppy_subsonic.inc) */
static void sub_started(const struct mp3entry *id3, uint32_t hash);
static void sub_counted(const struct sl_play *p, uint32_t len_ms);
/* With the UI tick: log the song that just finished, if it counted. */
static void hist_poll(void)
{
    struct mp3entry *id3 = audio_current_track();
    int status = audio_status();
    bool music = id3 && (status & AUDIO_STATUS_PLAY) && !quiz_clip_active();
    uint32_t hash = music ? sl_hist_hash(id3->path) : 0;
    bool playing = music && !(status & AUDIO_STATUS_PAUSE);
    uint32_t elapsed = music ? (uint32_t)visible_elapsed(id3) : 0;
    /* mktime can read the time zone file: only ask when a play starts */
    bool starts = sl_hist_starts(&hist_tracker, hash, elapsed);
    uint32_t now = starts ? hist_clock() : 0;
    if (sl_hist_is_skip(&hist_tracker, hash)) grave_skipped(hist_tracker.hash);
    struct sl_play p;
    uint32_t len = hist_tracker.len;      /* the song a new one ends */
    if (sl_hist_step(&hist_tracker, hash, playing, elapsed, music ? (uint32_t)id3->length : 0, now, &p))
    {
        hist_append(&p);
        if (hist_cur_path[0] && sl_hist_hash(hist_cur_path) == p.path_hash) hist_recent_push(sl_lib_find_path(hist_cur_path), p.when);
        grave_played(p.path_hash);
        scr_counted(&p, len);
        sub_counted(&p, len);
    }
    if (starts && hash) { grave_started(hash); scr_started(id3, hash, elapsed); sub_started(id3, hash); strlcpy(hist_cur_path, id3->path, sizeof hist_cur_path); }
    grave_flush(false);
}

/* Power off or leaving GabagoolOS: the song playing now may already count. */
static void hist_flush(void)
{
    struct sl_play p;
    uint32_t len = hist_tracker.len;
    if (sl_hist_finish(&hist_tracker, &p)) { hist_append(&p); grave_played(p.path_hash); scr_counted(&p, len); sub_counted(&p, len); }
    grave_flush(true);
    sl_net_scrobble_flush(1000);          /* the listen is on the card before the power goes */
}

/* ------------------------------------------------------------ reading */
struct hist_view {
    struct sl_play *plays;  /* in time order */
    int n;
    int *track;             /* library track of each play, or -1 */
    uint16_t *count;        /* per library track */
    uint32_t *last, *ms;    /* per library track */
    uint32_t first_when;
};
struct hist_key { uint32_t hash; int track; };
static int hist_key_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct hist_key *)a)->hash, y = ((const struct hist_key *)b)->hash;
    return (x > y) - (x < y);
}

static void hist_view_free(struct hist_view *v)
{
    if (!v) return;
    free(v->plays); free(v->track); free(v->count); free(v->last); free(v->ms);
    free(v);
}

/* Read the log and match it to the library. NULL when out of memory. */
static struct hist_view *hist_view_load(void)
{
    struct hist_view *v = calloc(1, sizeof *v);
    int n = sl_lib.n_tracks;
    if (!v) return NULL;
    v->count = calloc(n ? n : 1, sizeof *v->count);
    v->last = calloc(n ? n : 1, sizeof *v->last);
    v->ms = calloc(n ? n : 1, sizeof *v->ms);
    if (!v->count || !v->last || !v->ms) { hist_view_free(v); return NULL; }

    int fd = open(HIST_FILE, O_RDONLY);
    uint32_t magic = 0;
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (size >= 4 + HIST_REC && lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, &magic, sizeof magic) == (ssize_t)sizeof magic && magic == SL_HIST_MAGIC)
    {
        /* only the newest SL_HIST_MAX plays, read in one go */
        off_t records = (size - 4) / HIST_REC;
        off_t skip = records > SL_HIST_MAX ? records - SL_HIST_MAX : 0;
        int want = (int)(records - skip);
        v->plays = malloc(want * sizeof *v->plays);
        if (v->plays && lseek(fd, 4 + skip * HIST_REC, SEEK_SET) == 4 + skip * HIST_REC)
        {
            ssize_t got = read(fd, v->plays, want * sizeof *v->plays);
            v->n = got > 0 ? (int)(got / HIST_REC) : 0;
        }
        close(fd);
        fd = -1;
        if (records > 2 * SL_HIST_MAX && v->n == want)
        {
            /* keep the file bounded: rewrite it with the plays we keep */
            int out = open(HIST_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            bool ok = out >= 0 && write(out, &magic, sizeof magic) == (ssize_t)sizeof magic &&
                      write(out, v->plays, v->n * sizeof *v->plays) == (ssize_t)(v->n * sizeof *v->plays) &&
                      fsync(out) == 0;
            if (out >= 0 && close(out) < 0) ok = false;
            if (!ok || rename(HIST_FILE ".tmp", HIST_FILE) < 0) remove(HIST_FILE ".tmp");
            else sl_log("history: trimmed %ld old plays", (long)skip);
        }
    }
    if (fd >= 0) close(fd);
    /* a record torn by a power cut before this fix, or a clock gone wild,
     * must not add days of listening */
    for (int i = 0; i < v->n; i++)
        if (v->plays[i].ms > 3u * 3600u * 1000u) v->plays[i].ms = 3u * 3600u * 1000u;
    if (!v->n) return v;

    struct hist_key *keys = malloc((n ? n : 1) * sizeof *keys);
    v->track = malloc(v->n * sizeof *v->track);
    if (!keys || !v->track) { free(keys); hist_view_free(v); return NULL; }
    for (int i = 0; i < n; i++) { keys[i].hash = sl_track_hist_hash(i); keys[i].track = i; }
    qsort(keys, n, sizeof *keys, hist_key_cmp);
    for (int i = 0; i < v->n; i++)
    {
        const struct sl_play *p = &v->plays[i];
        struct hist_key want = { p->path_hash, 0 };
        const struct hist_key *k = n ? bsearch(&want, keys, n, sizeof *keys, hist_key_cmp) : NULL;
        int t = k ? k->track : -1;
        v->track[i] = t;
        if (t >= 0)
        {
            if (v->count[t] < 0xffff) v->count[t]++;
            if (p->when > v->last[t]) v->last[t] = p->when;
            v->ms[t] += p->ms;
        }
        if (p->when >= SL_HIST_VALID_CLOCK && (!v->first_when || p->when < v->first_when))
            v->first_when = p->when;
        if ((i & 4095) == 4095) yield();
    }
    free(keys);
    return v;
}

/* ------------------------------------------------------ smart lists */
enum { SMART_ADDED, SMART_MOST, SMART_RECENT, SMART_UNPLAYED, SMART_FORGOTTEN, SMART_COUNT };
static const char *const smart_titles[SMART_COUNT] = {
    "Recently added", "Most played", "Recently played", "Not played yet", "Forgotten favorites" };
static const char *const smart_blurbs[SMART_COUNT] = {
    "The newest files on your card",
    "Your heaviest rotation",
    "What you heard last",
    "Songs waiting for their first listen",
    "Played a lot, but not in six months" };
/* empty: what is missing, then how it fills */
static const char *const smart_empty_what[SMART_COUNT] = {
    "No file dates yet", "Nothing played yet", "Nothing played yet", "You've heard everything", "Nothing forgotten yet" };
static const char *const smart_empty_how[SMART_COUNT] = {
    "Update the database and look again.",
    "Play some music and your favorites collect here.",
    "Songs you play show here, the latest first.",
    "Every song in your library has been played. Impressive.",
    "Songs you played three or more times show up here once you have not heard them for six months." };
static const uint8_t smart_empty_icon[SMART_COUNT] = { IC_SONGS, IC_SONGS, IC_CLOCK, IC_SONGS, IC_CLOCK };
static const uint32_t smart_colors[SMART_COUNT] = { 0x2d8ff0, 0xe8408a, 0x34b84e, 0x7975ae, 0xf0602f };

static const struct hist_view *smart_sort_view;
static int smart_sort_kind;
static int smart_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    const struct hist_view *v = smart_sort_view;
    uint32_t kx, ky;
    switch (smart_sort_kind)
    {
        case SMART_MOST: case SMART_FORGOTTEN:
            if (v->count[x] != v->count[y]) return v->count[x] < v->count[y] ? 1 : -1;
            kx = v->ms[x]; ky = v->ms[y]; break;
        case SMART_RECENT: kx = v->last[x]; ky = v->last[y]; break;
        default: kx = sl_lib.tracks[x].added; ky = sl_lib.tracks[y].added; break;
    }
    if (kx != ky) return kx < ky ? 1 : -1;              /* larger first */
    return x - y;
}

/* The songs of one smart playlist, best first, capped. *out is malloc'd;
 * *total (if given) gets the count before the cap. */
static int smart_build(const struct hist_view *v, int kind, uint32_t now, int **out, int *total)
{
    int n = sl_lib.n_tracks, count = 0;
    int *list = malloc((n ? n : 1) * sizeof *list);
    *out = list;
    if (!list) return 0;
    bool clock_ok = now >= SL_HIST_VALID_CLOCK;
    for (int t = 0; t < n; t++)
    {
        bool take;
        switch (kind)
        {
            case SMART_ADDED: take = sl_lib.tracks[t].added > 0; break;
            case SMART_MOST: take = v->count[t] > 0; break;
            case SMART_RECENT: take = v->last[t] > 0; break;
            case SMART_UNPLAYED: take = v->count[t] == 0; break;
            default:
                /* plays logged with an unset or wrong clock can't prove a song was forgotten */
                take = clock_ok && v->count[t] >= 3 && v->last[t] >= SL_HIST_VALID_CLOCK &&
                       v->last[t] < now && now - v->last[t] > 182u * 86400u;
                break;
        }
        if (take && !ts_blocked_track(t)) list[count++] = t;
    }
    smart_sort_view = v;
    smart_sort_kind = kind;
    if (kind != SMART_UNPLAYED || count <= 2000)
        qsort(list, count, sizeof *list, smart_cmp);
    int max = kind == SMART_UNPLAYED ? 500 : HIST_LIST_MAX;
    if (total) *total = count;
    return count < max ? count : max;
}

static void smart_play_cb(lv_event_t *e)
{
    struct song_list_view *v = lv_event_get_user_data(e);
    bool shuffle = lv_obj_get_index(lv_event_get_current_target(e)) == 1;
    if (v && play_list(v->tracks, v->count, 0, shuffle)) np_show_auto();
}

/* The last few albums played, newest first, each once */
static int hist_recent_albums(int *albums, int max)
{
    struct hist_view *v = hist_view_load();
    if (!v) return 0;
    int *list = NULL;
    int n = smart_build(v, SMART_RECENT, hist_clock(), &list, NULL);
    hist_view_free(v);
    int na = 0;
    for (int i = 0; i < n && na < max; i++)
    {
        int a = list[i] >= 0 && list[i] < sl_lib.n_tracks ? sl_lib.tracks[list[i]].album : -1;
        bool seen = a < 0;
        for (int j = 0; j < na && !seen; j++) seen = albums[j] == a;
        if (!seen) albums[na++] = a;
    }
    free(list);
    return na;
}

/* A list of songs with Play and Shuffle over it; takes list (malloc'd). */
static void tracks_page(const char *title, const char *blurb, int icon, const char *what, const char *how, int *list, int n,
                        const char *heading)
{
    lv_obj_t *p, *body = page_new(title, &p);
    if (blurb)
    {
        lv_obj_t *b = text(body, blurb, &ss_regular_18, C_SUB);
        lv_obj_set_style_pad_left(b, 6, 0);
        lv_obj_set_width(b, lv_pct(100));               /* wraps: a long one ran off the screen */
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    }
    if (!n)
    {
        free(list);
        sl_empty(body, icon, 96, what, how, 40);   /* under the blurb */
        page_push(p);
        return;
    }
    lv_obj_t *pills = box(body);
    lv_obj_set_size(pills, lv_pct(100), 72);
    lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pills, 12, 0);
    lv_obj_t *play = pill(pills, LV_SYMBOL_PLAY "  Play", true, 0);
    lv_obj_t *shuf = pill(pills, LV_SYMBOL_SHUFFLE "  Shuffle", false, 0);
    section(body, heading);
    struct song_list_view *view = song_list(body, list, n, SL_LIST_CUSTOM);
    if (!view) { free(list); page_push(p); return; }
    view->owned = list;
    /* the pills play this list: swap pill_cb for ours */
    lv_obj_t *buttons[2] = { play, shuf };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_remove_event_cb(buttons[i], pill_cb);
        lv_obj_add_event_cb(buttons[i], smart_play_cb, LV_EVENT_CLICKED, view);
    }
    page_push(p);
}

static void smart_list_page(int kind)
{
    struct hist_view *v = hist_view_load();
    if (!v) { toast_warn("Not enough memory for your history"); return; }
    int *list = NULL;
    int n = smart_build(v, kind, hist_clock(), &list, NULL);
    uint32_t since = v->first_when;
    hist_view_free(v);
    sl_log("smart list %d: %d songs", kind, n);
    char b[80];
    if (kind != SMART_ADDED && since)
    {
        int y, m, d;
        static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        sl_hist_date(since, &y, &m, &d, NULL);
        snprintf(b, sizeof b, "%d SONGS · COUNTING SINCE %d %s %d", n, d, months[m - 1], y);
    }
    else snprintf(b, sizeof b, "%d SONGS", n);
    tracks_page(smart_titles[kind], smart_blurbs[kind], smart_empty_icon[kind], smart_empty_what[kind], smart_empty_how[kind], list, n, b);
}

static void smart_row_cb(lv_event_t *e) { smart_list_page((int)(intptr_t)lv_event_get_user_data(e)); }

/* ----------------------------------------------------------- Wrapped */

struct wrapped {
    uint64_t ms;
    int plays, songs, artists;
    uint64_t month_ms[12];
    uint32_t hour_plays[24];
    int top_artist[5], top_song[5];      /* artist: index into albums_by_artist */
    uint64_t top_artist_ms[5];
    int top_song_plays[5];
    int years[16], n_years;              /* years with plays, newest first */
};

/* albums_by_artist position of the first album of each album's artist */
static int *wrapped_artist_ids(void)
{
    int *id = malloc((sl_lib.n_albums ? sl_lib.n_albums : 1) * sizeof *id);
    if (!id) return NULL;
    int current = 0;
    for (int k = 0; k < sl_lib.n_albums; k++)
    {
        if (k && strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k]].artist,
                            sl_lib.albums[sl_lib.albums_by_artist[k - 1]].artist))
            current = k;
        id[sl_lib.albums_by_artist[k]] = current;
    }
    return id;
}

static void wrapped_top(uint64_t value, int who, uint64_t *vals, int *ids, int n)
{
    if (!value) return;
    for (int i = 0; i < n; i++)
        if (ids[i] < 0 || value > vals[i])
        {
            memmove(&vals[i + 1], &vals[i], (n - 1 - i) * sizeof *vals);
            memmove(&ids[i + 1], &ids[i], (n - 1 - i) * sizeof *ids);
            vals[i] = value; ids[i] = who;
            return;
        }
}

static bool wrapped_compute(const struct hist_view *v, int year, struct wrapped *w)
{
    memset(w, 0, sizeof *w);
    for (int i = 0; i < 5; i++) w->top_artist[i] = w->top_song[i] = -1;
    int *artist_of_album = wrapped_artist_ids();
    uint64_t *artist_ms = calloc(sl_lib.n_albums ? sl_lib.n_albums : 1, sizeof *artist_ms);
    uint16_t *song_plays = calloc(sl_lib.n_tracks ? sl_lib.n_tracks : 1, sizeof *song_plays);
    if (!artist_of_album || !artist_ms || !song_plays)
    { free(artist_of_album); free(artist_ms); free(song_plays); return false; }
    for (int i = 0; i < v->n; i++)
    {
        const struct sl_play *p = &v->plays[i];
        if (p->when < SL_HIST_VALID_CLOCK) continue;
        int y, m, h;
        sl_hist_date(p->when, &y, &m, NULL, &h);
        bool known = false;
        for (int k = 0; k < w->n_years; k++) known |= w->years[k] == y;
        if (!known && w->n_years < 16) w->years[w->n_years++] = y;
        if (y != year) continue;
        w->ms += p->ms;
        w->plays++;
        w->month_ms[m - 1] += p->ms;
        w->hour_plays[h]++;
        int t = v->track[i];
        if (t < 0) continue;
        if (!song_plays[t]) w->songs++;
        if (song_plays[t] < 0xffff) song_plays[t]++;
        artist_ms[artist_of_album[sl_lib.tracks[t].album]] += p->ms;
    }
    bool have_year = false;                        /* the asked-for year, even with no plays yet */
    for (int k = 0; k < w->n_years; k++) have_year |= w->years[k] == year;
    if (!have_year && w->n_years < 16) w->years[w->n_years++] = year;
    for (int i = 0; i < w->n_years; i++)          /* newest first */
        for (int j = i + 1; j < w->n_years; j++)
            if (w->years[j] > w->years[i]) { int s = w->years[i]; w->years[i] = w->years[j]; w->years[j] = s; }
    for (int a = 0; a < sl_lib.n_albums; a++)
        if (artist_ms[a]) { w->artists++; wrapped_top(artist_ms[a], a, w->top_artist_ms, w->top_artist, 5); }
    uint64_t song_vals[5] = { 0 };
    for (int t = 0; t < sl_lib.n_tracks; t++)
        wrapped_top(song_plays[t], t, song_vals, w->top_song, 5);
    for (int i = 0; i < 5; i++) w->top_song_plays[i] = (int)song_vals[i];
    free(artist_of_album); free(artist_ms); free(song_plays);
    return true;
}

static lv_obj_t *wrapped_card(lv_obj_t *body, uint32_t c1, uint32_t c2, int height)
{
    lv_obj_t *c = box(body);
    lv_obj_set_size(c, lv_pct(100), height);
    lv_obj_set_style_radius(c, SL_R_LG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(c1), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_darken(lv_color_hex(c2), 45), 0);   /* deeper where the words are: white reads 4.5:1 */
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_margin_bottom(c, 12, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    return c;
}

static lv_obj_t *wrapped_line(lv_obj_t *parent, const char *s, const lv_font_t *f, int y, uint32_t color)
{
    lv_obj_t *l = text(parent, s, f, color);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);   /* on colour, whatever the theme */
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    sl_label_deep(l);
    /* under the line before it when the text is large, and the card as tall as its lines */
    int below = (int)(intptr_t)lv_obj_get_user_data(parent);
    if (y < below) y = below;
    lv_obj_set_pos(l, 0, y);
    lv_obj_update_layout(parent);
    below = y + LV_MAX(lv_font_get_line_height(f), lv_obj_get_height(l)) + 2;
    lv_obj_set_user_data(parent, (void *)(intptr_t)below);
    int h = lv_obj_get_style_height(parent, LV_PART_MAIN);
    if (h > 0 && !LV_COORD_IS_SPEC(h) && h < below + 40) lv_obj_set_height(parent, below + 40);
    return l;
}

/* "1  Name  ....  value" on a Wrapped card; the name keeps its own direction */
static lv_obj_t *wrapped_rank(lv_obj_t *card, int i, const char *name, const char *value,
                              const lv_font_t *font, uint32_t soft)
{
    char n[4];
    snprintf(n, sizeof n, "%d", i + 1);
    int y = 50 + i * 58;
    lv_obj_t *num = wrapped_line(card, n, font, y, 0xffffff);
    lv_obj_set_width(num, 30);
    lv_obj_t *l = wrapped_line(card, name, font, y, 0xffffff);
    lv_obj_set_x(l, 34);
    lv_obj_set_width(l, lv_pct(70));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *r = wrapped_line(card, value, &ss_regular_18, y + 6, soft);
    lv_obj_set_width(r, lv_pct(26));
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, y + 6);
    return l;
}

static void wrapped_page_year(int year, bool replace);
static int wrapped_year;
static void wrapped_year_cb(lv_event_t *e) { wrapped_page_year((int)(intptr_t)lv_event_get_user_data(e), true); }

static void wrapped_song_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t >= 0 && t < sl_lib.n_tracks && play_list(&t, 1, 0, false)) np_show_auto();
}

static const char *wrapped_when(const struct wrapped *w)
{
    uint32_t part[4] = { 0 };   /* night, morning, afternoon, evening */
    for (int h = 0; h < 24; h++) part[h < 5 ? 0 : h < 12 ? 1 : h < 18 ? 2 : h < 23 ? 3 : 0] += w->hour_plays[h];
    int best = 0;
    for (int i = 1; i < 4; i++) if (part[i] > part[best]) best = i;
    static const char *const says[4] = {
        "Night owl. Most of your listening happens after 11 PM.",
        "Early bird. Mornings are when the music comes on.",
        "Afternoon groover. Your peak is between noon and 6 PM.",
        "Evening unwinder. Most songs play between 6 and 11 PM." };
    return says[best];
}

static void wrapped_page_year(int year, bool replace)
{
    struct hist_view *v = hist_view_load();
    struct wrapped *w = malloc(sizeof *w);
    if (!v || !w || !wrapped_compute(v, year, w))
    { hist_view_free(v); free(w); toast_warn("Not enough memory for Wrapped"); return; }
    uint32_t since = v->first_when;
    hist_view_free(v);
    bool newer = year > wrapped_year;              /* slide the way the years go */
    wrapped_year = year;
    char b[120], title[24];
    snprintf(title, sizeof title, "Wrapped %d", year);
    lv_obj_t *p, *body = page_new(title, &p);
    lv_obj_set_style_pad_row(body, 0, 0);

    if (w->n_years > 1)
    {
        lv_obj_t *bar = sl_pager(body);
        for (int i = 0; i < w->n_years; i++)
            if (w->years[i] == year)
            {
                if (i + 1 < w->n_years)
                {
                    /* an earlier year behind you: left in English, right in RTL */
                    if (sl_ui_rtl()) snprintf(b, sizeof b, "%d  " LV_SYMBOL_RIGHT, w->years[i + 1]);
                    else snprintf(b, sizeof b, LV_SYMBOL_LEFT "  %d", w->years[i + 1]);
                    sl_pager_button(bar, b, false, wrapped_year_cb, w->years[i + 1]);
                }
                if (i > 0)
                {
                    if (sl_ui_rtl()) snprintf(b, sizeof b, LV_SYMBOL_LEFT "  %d", w->years[i - 1]);
                    else snprintf(b, sizeof b, "%d  " LV_SYMBOL_RIGHT, w->years[i - 1]);
                    sl_pager_button(bar, b, true, wrapped_year_cb, w->years[i - 1]);
                }
            }
    }

    if (!w->plays)
    {
        lv_obj_t *c = wrapped_card(body, 0x6a3de8, 0xe8408a, 220);
        lv_obj_t *t = wrapped_line(c, "Your year in music", &ss_semibold_26, 0, 0xffffff);
        lv_obj_t *l = wrapped_line(c, "Play some music! Wrapped fills in as you listen. A song counts after "
                                      "30 seconds.", &ss_regular_18, 50, 0xf2eefc);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        /* nothing to show yet: the two lines centred in the card, as an
         * empty state is, not stuck in its top corner */
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_update_layout(c);
        int used = lv_obj_get_y(l) + lv_obj_get_height(l), room = lv_obj_get_content_height(c);
        if (room > used)
        {
            lv_obj_set_y(t, lv_obj_get_y(t) + (room - used) / 2);
            lv_obj_set_y(l, lv_obj_get_y(l) + (room - used) / 2);
        }
        free(w);
        if (replace) page_replace(p, newer); else page_push(p);
        return;
    }

    /* hero: listening time */
    lv_obj_t *c = wrapped_card(body, 0x6a3de8, 0xe8408a, 230);
    wrapped_line(c, "You listened for", &ss_semibold_23, 0, 0xf2eefc);
    unsigned minutes = (unsigned)(w->ms / 60000u);
    if (minutes >= 120) snprintf(b, sizeof b, "%u hours", minutes / 60);
    else snprintf(b, sizeof b, "%u minutes", minutes);
    /* as written: "15 minutes" is also the sleep timer's, whose words the
     * 56 px face has no letters for in Hebrew, Arabic or Chinese */
    lv_label_set_text(wrapped_line(c, b, &ss_bold_56, 36, 0xffffff), b);
    snprintf(b, sizeof b, "%d play%s · %d song%s · %d artist%s", w->plays, w->plays == 1 ? "" : "s",
             w->songs, w->songs == 1 ? "" : "s", w->artists, w->artists == 1 ? "" : "s");
    wrapped_line(c, b, &ss_semibold_21, 120, 0xffffff);
    if (minutes >= 2880)
        snprintf(b, sizeof b, "That is %u full days of nonstop music.", minutes / 1440);
    else if (minutes >= 90)
        snprintf(b, sizeof b, "About %u albums back to back.", minutes / 45);
    else
        snprintf(b, sizeof b, "About one album's worth. A good start.");
    wrapped_line(c, b, &ss_regular_18, 158, 0xf2eefc);

    /* top artists */
    if (w->top_artist[0] >= 0)
    {
        int rows = 0;
        while (rows < 5 && w->top_artist[rows] >= 0) rows++;
        lv_obj_t *a = wrapped_card(body, 0x0e8fd8, 0x0aa9c7, 84 + 58 * rows);
        wrapped_line(a, "Top artists", &ss_semibold_26, 0, 0xffffff);
        for (int i = 0; i < rows; i++)
        {
            const char *name = sl_lib.albums[sl_lib.albums_by_artist[w->top_artist[i]]].artist;
            unsigned mins = (unsigned)(w->top_artist_ms[i] / 60000u);
            if (mins >= 60) snprintf(b, sizeof b, "%u h %02u m", mins / 60, mins % 60);
            else snprintf(b, sizeof b, "%u min", mins ? mins : 1);
            wrapped_rank(a, i, name, b, &ss_semibold_23, 0xe6f7ff);
        }
    }

    /* top songs, tap to play */
    if (w->top_song[0] >= 0)
    {
        int rows = 0;
        while (rows < 5 && w->top_song[rows] >= 0) rows++;
        lv_obj_t *s = wrapped_card(body, 0x1f9d55, 0x6fae16, 84 + 58 * rows);
        wrapped_line(s, "Top songs", &ss_semibold_26, 0, 0xffffff);
        for (int i = 0; i < rows; i++)
        {
            int t = w->top_song[i];
            snprintf(b, sizeof b, "%d play%s", w->top_song_plays[i], w->top_song_plays[i] == 1 ? "" : "s");
            lv_obj_t *l = wrapped_rank(s, i, sl_lib.tracks[t].title, b, &ss_semibold_23, 0xeaffd9);
            on_click(l, wrapped_song_cb, (void *)(intptr_t)t);
            lv_obj_set_ext_click_area(l, 12);
        }
    }

    /* months */
    lv_obj_t *m = wrapped_card(body, 0xdb2f9c, 0xf0602f, 250);
    wrapped_line(m, "Your months", &ss_semibold_26, 0, 0xffffff);
    uint64_t top = 1;
    int best = 0;
    for (int i = 0; i < 12; i++) if (w->month_ms[i] > top) { top = w->month_ms[i]; best = i; }
    static const char *const letters = "JFMAMJJASOND";
    int bw = 26, gap = (W - 28 - 40 - 12 * bw) / 11;
    for (int i = 0; i < 12; i++)
    {
        int h = (int)(w->month_ms[i] * 120 / top);
        lv_obj_t *bar = box(m);
        lv_obj_set_size(bar, bw, h > 4 ? h : 4);
        lv_obj_set_pos(bar, i * (bw + gap), 50 + 120 - (h > 4 ? h : 4));
        lv_obj_set_style_radius(bar, SL_R_XS, 0);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, i == best ? LV_OPA_COVER : LV_OPA_50, 0);
        char ch[2] = { letters[i], 0 };
        lv_obj_t *l = wrapped_line(m, ch, &ss_semibold_18, 176, 0xffffff);
        lv_obj_set_width(l, bw);
        lv_obj_set_x(l, i * (bw + gap));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* when you listen */
    lv_obj_t *t = wrapped_card(body, 0x3b6cf0, 0x7975ae, 150);
    wrapped_line(t, "When you listen", &ss_semibold_26, 0, 0xffffff);
    lv_obj_t *l = wrapped_line(t, wrapped_when(w), &ss_semibold_21, 48, 0xeef1ff);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);

    if (since)
    {
        int y, mo, d;
        static const char *const months[] = { "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December" };
        sl_hist_date(since, &y, &mo, &d, NULL);
        snprintf(b, sizeof b, "Counting since %d %s %d", d, months[mo - 1], y);
        lv_obj_t *f = text(body, b, &ss_regular_18, C_DIM);
        lv_obj_set_style_pad_all(f, 8, 0);
    }
    free(w);
    if (replace) page_replace(p, newer); else page_push(p);
}

static int wrapped_this_year(void)
{
    uint32_t now = hist_clock();
    int y = 2026;
    if (now >= SL_HIST_VALID_CLOCK) sl_hist_date(now, &y, NULL, NULL, NULL);
    return y;
}
static void wrapped_page(void) { wrapped_page_year(wrapped_this_year(), false); }
static void wrapped_open_cb(lv_event_t *e) { (void)e; wrapped_page(); }
static void grave_open_cb(lv_event_t *e) { (void)e; grave_page(); }
static void mix_page(void);
static void mix_open_cb(lv_event_t *e) { (void)e; mix_page(); }

/* ----------------------------------------------------------- For you */
/* One of For you's rows: a coloured icon, a name and what it is. */
static void fy_row(lv_obj_t *body, const char *sym_s, uint32_t color, const char *title, const char *sub,
                   lv_event_cb_t cb, int ud)
{
    lv_obj_t *r = box(body);
    lv_obj_add_style(r, &st_card, 0);
    lv_obj_add_style(r, &st_card_pr, LV_STATE_PRESSED);
    /* the subtitle under the title by their heights (54 and 96 at Standard) */
    int t2y = 16 + lv_font_get_line_height(&ss_semibold_26) + 7;
    lv_obj_set_size(r, lv_pct(100), sub ? LV_MAX(96, t2y + lv_font_get_line_height(&ss_regular_18) + 20) : 84);
    lv_obj_set_style_margin_bottom(r, 8, 0);
    lv_obj_t *ic = box(r);
    lv_obj_set_size(ic, 64, 64);
    sl_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(ic, SL_R_ICON(64), 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_t *sym = text(ic, sym_s, &sl_sym_28, 0xffffff);
    lv_obj_set_style_text_color(sym, lv_color_white(), 0);
    lv_obj_center(sym);
    lv_obj_t *t1 = text(r, title, &ss_semibold_26, 0xffffff);
    lv_obj_set_width(t1, W - 28 - 94 - 44);   /* clear of the chevron: "Añadidas recientemente" ran under it */
    label_lines(t1, 1);
    if (sub)
    {
        sl_place(t1, 94, 16);
        lv_obj_t *t2 = text(r, sub, &ss_regular_18, C_SUB);
        sl_place(t2, 94, t2y);
        lv_obj_set_size(t2, W - 28 - 94 - 44, 24);
        lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    }
    else
    {
        sl_align(t1, LV_ALIGN_LEFT_MID, 94, 0);
        if (sl_ui_rtl()) lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_t *ch = text(r, sl_chevron(), &sl_sym_22, 0xffffff);
    sl_align(ch, LV_ALIGN_RIGHT_MID, -18, 0);
    on_click(r, cb, (void *)(intptr_t)ud);
}
static void picks_page(void);           /* sloppy_taste.inc */
static void taste_reset_page(void);
static void foryou_open_cb(lv_event_t *e)
{
    switch ((int)(intptr_t)lv_event_get_user_data(e))
    {
        case 0: loved_page(); break;
        case 1: taste_page(false); break;
        case 3: picks_page(); break;
        case 4: taste_reset_page(); break;
        default: hidden_page(); break;
    }
}
static void foryou_page(void)
{
    if (ts_first_visit()) { taste_page(true); return; }   /* a deck to swipe, once */
    struct hist_view *v = hist_view_load();
    if (!v) { toast_warn("Not enough memory for your history"); return; }
    uint32_t now = hist_clock();
    int counts[SMART_COUNT], totals[SMART_COUNT];
    for (int k = 0; k < SMART_COUNT; k++)
    {
        int *list = NULL;
        counts[k] = smart_build(v, k, now, &list, &totals[k]);
        free(list);
    }
    uint64_t ms = 0;
    int year = wrapped_this_year();
    for (int i = 0; i < v->n; i++)
    {
        int y = 0;
        if (v->plays[i].when >= SL_HIST_VALID_CLOCK) sl_hist_date(v->plays[i].when, &y, NULL, NULL, NULL);
        if (y == year) ms += v->plays[i].ms;
    }
    hist_view_free(v);

    lv_obj_t *p, *body = page_new("For you", &p);
    char b[80];
    lv_obj_t *hero = wrapped_card(body, 0x6a3de8, 0xe8408a, 150);
    snprintf(b, sizeof b, "Your %d Wrapped", year);
    wrapped_line(hero, b, &ss_bold_28, 4, 0xffffff);
    unsigned minutes = (unsigned)(ms / 60000u);
    if (minutes >= 120) snprintf(b, sizeof b, "%u hours of music so far", minutes / 60);
    else if (minutes) snprintf(b, sizeof b, "%u minutes of music so far", minutes);
    else snprintf(b, sizeof b, "Starts counting with your next song");
    wrapped_line(hero, b, &ss_semibold_21, 52, 0xf2eefc);
    wrapped_line(hero, "Tap to see your top artists and songs", &ss_regular_18, 84, 0xf2eefc);
    on_click(hero, wrapped_open_cb, NULL);
    lv_obj_set_style_bg_opa(hero, LV_OPA_80, LV_STATE_PRESSED);

    section(body, "SMART PLAYLISTS");
    static const char *const icons[SMART_COUNT] = {
        LV_SYMBOL_DOWNLOAD, LV_SYMBOL_UP, LV_SYMBOL_LOOP, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_REFRESH };
    fy_row(body, LV_SYMBOL_AUDIO, 0xd6336c, "Loved", "Spotify likes, your swipes, favourites", foryou_open_cb, 0);
    for (int k = 0; k < SMART_COUNT; k++)
    {
        if (totals[k] > counts[k])
            snprintf(b, sizeof b, "Top %d · %s", counts[k], smart_blurbs[k]);
        else if (counts[k])
            snprintf(b, sizeof b, "%d song%s · %s", counts[k], counts[k] == 1 ? "" : "s", smart_blurbs[k]);
        else snprintf(b, sizeof b, "Empty for now · %s", smart_blurbs[k]);
        fy_row(body, icons[k], smart_colors[k], smart_titles[k], b, smart_row_cb, k);
    }
    lv_obj_t *mt = wrapped_card(body, 0xe24e3b, 0x9c2f5a, 110);
    wrapped_line(mt, "Your pet's mixtape", &ss_bold_28, 4, 0xffffff);
    wrapped_line(mt, "A tape it made from what you play together", &ss_regular_18, 50, 0xfbe3dc);
    on_click(mt, mix_open_cb, NULL);
    lv_obj_set_style_bg_opa(mt, LV_OPA_80, LV_STATE_PRESSED);
    section(body, "AFTER DARK");
    lv_obj_t *gy = wrapped_card(body, 0x2a2250, 0x15111f, 110);
    wrapped_line(gy, "Skip Graveyard", &ss_bold_28, 4, 0xece6ff);
    wrapped_line(gy, "Where the songs you skip go to rest", &ss_regular_18, 50, 0xb3a9dc);
    on_click(gy, grave_open_cb, NULL);
    lv_obj_set_style_bg_opa(gy, LV_OPA_80, LV_STATE_PRESSED);
    section(body, "YOUR TASTE");
    fy_row(body, LV_SYMBOL_SHUFFLE, 0x2fb38a, "Your taste", "Swipe songs to teach Radio and Loved", foryou_open_cb, 1);
    fy_row(body, LV_SYMBOL_OK, 0x7357e0, "Picked for you", "Songs your swipes say you'll like", foryou_open_cb, 3);
    fy_row(body, LV_SYMBOL_EYE_CLOSE, 0x5a6478, "Hidden artists", NULL, foryou_open_cb, 2);
    fy_row(body, LV_SYMBOL_REFRESH, 0xe5484d, "Start over", "Forget every song you swiped", foryou_open_cb, 4);
    page_push(p);
}
