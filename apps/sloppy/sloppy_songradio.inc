/*
 *  Radio from this song: the song, then an endless run of songs from the
 *  card picked around it. No audio analysis and nothing from the internet:
 *  only tags and the listening log, so it is as good as both are.
 *
 *  What counts as "around" a song, per album:
 *   - the same artist (album artist; Various Artists and blanks never match)
 *   - artists heard in the same sittings as this one (sloppy_history.inc's
 *     log: plays within an hour of a play of this artist), more the more often
 *   - a year within two of the song's
 *  A library too small or too untagged for that to give a few dozen songs
 *  is topped up with everything else, lightly, so radio still plays.
 *
 *  Picks never repeat a song in one radio session (until every song in the
 *  pool has played), the same artist does not play twice running, and the
 *  seed's own artist gets at most one pick in three. The pool is worked out
 *  once, when radio starts; while it plays, a pick is a walk over the
 *  album weights, nothing more.
 *
 *  The queue: radio starts like playing any song (sloppy_queue.h carries
 *  the songs you queued over, and they play first, straight after the
 *  seed). Up next shows what radio picked under its own heading, with
 *  Stop radio: that stops new picks; the ones already listed stay. Playing
 *  anything else ends radio too.
 */

#define SR_TOP_FILE ROCKBOX_DIR "/sloppy/spotify/top-artists.txt"
#define SR_TOP_MAX 50
#define SR_START 10                     /* the seed and the first picks */
#define SR_MORE 5                       /* added when two are left */
#define SR_POOL_MIN 30                  /* fewer songs than this: top up */

static struct
{
    bool active;
    int seed;                           /* track index */
    char seed_title[64];
    uint8_t *weight;                    /* per album, 0 = not in the pool */
    uint32_t *played;                   /* per track bit: picked this session */
    int *artist;                        /* per album, wrapped_artist_ids() */
    int seed_artist;                    /* -1 when the seed has no usable artist */
    int last_artist, since_seed_artist, picked, pool_tracks;
    unsigned rng;
} sr;

static bool sr_on(void) { return sr.active; }
static const char *sr_seed_title(void) { return sr.seed_title; }

static unsigned sr_rand(void)
{
    sr.rng = sr.rng * 1103515245u + 12345u;
    return sr.rng >> 8;
}

/* An album artist that says who it is: not empty, not a compilation. */
static bool sr_artist_usable(const char *a)
{
    return a && a[0] && strcasecmp(a, "Various Artists") && strcasecmp(a, "Various") &&
           strcasecmp(a, "VA") && strncasecmp(a, "<Untagged", 9) && strcasecmp(a, "Unknown Artist") &&
           strcasecmp(a, "Unknown");
}

static void sr_free(void)
{
    free(sr.weight); free(sr.played); free(sr.artist);
    sr.weight = NULL; sr.played = NULL; sr.artist = NULL;
}

/* Radio outlives a power-off: the seed's path is kept here while it runs,
 * and the first playback after start-up carries on from it (sr_resume).
 * Not radio.txt: that is internet radio's saved stations */
#define SR_SAVE ROCKBOX_DIR "/sloppy/songradio.txt"
static bool sr_resume_checked;

static void sr_stop(void)
{
    if (!sr.active) return;
    sr.active = false;
    remove(SR_SAVE);
    sr_free();
    sl_log("radio: stopped after %d picks", sr.picked);
}

/* Weigh every album against the seed. False when out of memory. */
static bool sr_build_pool(int seed)
{
    int na = sl_lib.n_albums, nt = sl_lib.n_tracks;
    sr.weight = calloc(na ? na : 1, 1);
    sr.played = calloc((nt + 31) / 32 + 1, sizeof *sr.played);
    sr.artist = wrapped_artist_ids();
    if (!sr.weight || !sr.played || !sr.artist) return false;
    const struct sl_album *sa = &sl_lib.albums[sl_lib.tracks[seed].album];
    sr.seed_artist = sr_artist_usable(sa->artist) ? sr.artist[sl_lib.tracks[seed].album] : -1;

    /* sittings: artists heard within an hour of the seed's artist */
    uint16_t *co = NULL;
    int co_max = 0;
    struct hist_view *v = sr.seed_artist >= 0 ? hist_view_load() : NULL;
    if (v && v->n && v->track && (co = calloc(na ? na : 1, sizeof *co)))
    {
        for (int i = 0; i < v->n; i++)
        {
            int t = v->track[i];
            if (t < 0 || sr.artist[sl_lib.tracks[t].album] != sr.seed_artist) continue;
            for (int j = i - 8 > 0 ? i - 8 : 0; j < v->n && j <= i + 8; j++)
            {
                int u = v->track[j];
                if (u < 0 || j == i) continue;
                uint32_t a = v->plays[i].when, b = v->plays[j].when, d = a > b ? a - b : b - a;
                if (d > 3600) continue;
                int id = sr.artist[sl_lib.tracks[u].album];
                if (id == sr.seed_artist || !sr_artist_usable(sl_lib.albums[sl_lib.tracks[u].album].artist)) continue;
                if (co[id] < 0xffff) co[id]++;
                if (co[id] > co_max) co_max = co[id];
            }
            if ((i & 1023) == 1023) yield();
        }
    }
    hist_view_free(v);

    /* Spotify top artists, squashed as the playlist matcher squashes them */
    static char top[SR_TOP_MAX][SP_FIELD];
    int ntop = 0;
    int fd = open(SR_TOP_FILE, O_RDONLY);
    if (fd >= 0)
    {
        char line[SP_FIELD];
        while (ntop < SR_TOP_MAX && read_line(fd, line, sizeof line) > 0)
            if (line[0] && line[0] != '#')
            {
                sp_artist_norm(line, strlen(line), top[ntop], sizeof top[0]);
                if (top[ntop][0]) ntop++;
            }
        close(fd);
    }

    int pool = 0, liked = 0;
    for (int a = 0; a < na; a++)
    {
        const struct sl_album *al = &sl_lib.albums[a];
        int w = 0;
        if (ntop && sr_artist_usable(al->artist))
        {
            char me[SP_FIELD];
            sp_artist_norm(al->artist, strlen(al->artist), me, sizeof me);
            for (int i = 0; i < ntop; i++)
                if (!strcmp(me, top[i])) { w += 3; liked++; break; }
        }
        if (sr.seed_artist >= 0 && sr.artist[a] == sr.seed_artist) w += 6;
        else if (co && co_max && co[sr.artist[a]]) w += 1 + 4 * co[sr.artist[a]] / co_max;
        if (sa->year > 0 && al->year > 0 && abs(al->year - sa->year) <= 2) w += 1;
        if (sa->genre && al->genre && !strcasecmp(sa->genre, al->genre)) w += 2;   /* the same kind of music */
        if (w && ts_liked_album(a)) w += 2;                                        /* artists you swiped right */
        if (w > 1 && ts_passed_album(a)) w -= 1;                                   /* and left */
        if (ts_blocked_album(a)) w = 0;                                            /* never: hidden */
        sr.weight[a] = (uint8_t)w;
        if (w) pool += al->n_tracks;
    }
    free(co);
    /* the seed itself is never picked again */
    sr.played[seed / 32] |= 1u << (seed % 32);
    if (pool - 1 < SR_POOL_MIN)
    {
        for (int a = 0; a < na; a++)
            if (!sr.weight[a] && !ts_blocked_album(a)) { sr.weight[a] = 1; pool += sl_lib.albums[a].n_tracks; }
        sl_log("radio: few related songs, topped up with the whole library");
    }
    sr.pool_tracks = pool;
    if (ntop) sl_log("radio: %d Spotify top artists, %d albums by them here", ntop, liked);
    return true;
}

/* One pick, or -1 when the library has nothing else. */
static int sr_pick(void)
{
    for (int round = 0; round < 2; round++)
    {
        unsigned total = 0;
        for (int a = 0; a < sl_lib.n_albums; a++) total += sr.weight[a];
        if (!total) return -1;
        for (int tries = 0; tries < 40; tries++)
        {
            unsigned r = sr_rand() % total;
            int a = 0;
            for (; a < sl_lib.n_albums; a++)
            {
                if (r < sr.weight[a]) break;
                r -= sr.weight[a];
            }
            if (a >= sl_lib.n_albums) continue;
            const struct sl_album *al = &sl_lib.albums[a];
            if (al->n_tracks <= 0) continue;
            int t = al->tracks[sr_rand() % al->n_tracks];
            if (sr.played[t / 32] & (1u << (t % 32))) continue;
            bool same_artist = sr.artist[a] == sr.last_artist && sr_artist_usable(al->artist);
            bool seed_artist = sr.seed_artist >= 0 && sr.artist[a] == sr.seed_artist;
            /* variety first; after many tries take what there is */
            if (tries < 30 && (same_artist || (seed_artist && sr.since_seed_artist < 2))) continue;
            sr.played[t / 32] |= 1u << (t % 32);
            sr.last_artist = sr.artist[a];
            sr.since_seed_artist = seed_artist ? 0 : sr.since_seed_artist + 1;
            sr.picked++;
            return t;
        }
        /* everything near was heard: start the session's memory over */
        memset(sr.played, 0, ((sl_lib.n_tracks + 31) / 32 + 1) * sizeof *sr.played);
        sr.played[sr.seed / 32] |= 1u << (sr.seed % 32);
    }
    return -1;
}

static void sr_start(int seed)
{
    if (seed < 0 || seed >= sl_lib.n_tracks) return;
    sr_stop();
    memset(&sr, 0, sizeof sr);
    sr.rng = (unsigned)current_tick * 2654435761u ^ (unsigned)seed * 40503u ^ hist_clock();
    sr.last_artist = sr.seed_artist = -1;
    sr.since_seed_artist = 2;
    if (!sr_build_pool(seed)) { sr_free(); toast_warn("Not enough memory for radio"); return; }
    sr.seed = seed;
    /* the songs you queued play first (play_list carries them over): radio
     * must not pick them again */
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        struct playlist_track_info info;
        for (int k = 1, run = q_run(); k <= run; k++)
            if (playlist_get_track_info(NULL, q_at(k), &info) >= 0)
            {
                int t = sl_lib_find_path(info.filename);
                if (t >= 0) sr.played[t / 32] |= 1u << (t % 32);
            }
    }
    int list[SR_START], n = 0;
    list[n++] = seed;
    while (n < SR_START)
    {
        int t = sr_pick();
        if (t < 0) break;
        list[n++] = t;
    }
    if (n < 2) { sr_free(); toast("No other songs for radio yet"); return; }
    if (!play_list(list, n, 0, false)) { sr_free(); return; }
    sr.active = true;                   /* play_list ends any radio before */
    sr_resume_checked = true;           /* this session's radio, not last one's */
    strlcpy(sr.seed_title, sl_lib.tracks[seed].title, sizeof sr.seed_title);
    int fd = open(SR_SAVE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        char path[MAX_PATH];
        sl_track_path(seed, path, sizeof path);
        write(fd, path, strlen(path));
        write(fd, "\n", 1);
        close(fd);
    }
    sl_log("radio: from \"%s\", %d songs in the pool, artist %s", sr.seed_title, sr.pool_tracks,
           sr.seed_artist >= 0 ? "known" : "unknown");
    char b[96];
    snprintf(b, sizeof b, "Radio from %s", sr.seed_title);
    toast(b);
    if (uq_open) uq_rebuild_soon();
}

/* Back on after a power-off: the list that resumed is still radio's when
 * it starts with the seed. What is in it counts as already picked. */
static void sr_resume(void)
{
    sr_resume_checked = true;
    char path[MAX_PATH];
    int fd = open(SR_SAVE, O_RDONLY);
    if (fd < 0) return;
    int got = read_line(fd, path, sizeof path);
    close(fd);
    struct playlist_track_info info;
    int seed = got > 0 ? sl_lib_find_path(path) : -1;
    if (seed < 0 || playlist_amount() < 1 ||
        playlist_get_track_info(NULL, playlist_get_first_index(NULL), &info) < 0 || strcmp(info.filename, path))
    {
        remove(SR_SAVE);                /* something else plays now */
        return;
    }
    memset(&sr, 0, sizeof sr);
    sr.rng = (unsigned)current_tick * 2654435761u ^ (unsigned)seed * 40503u ^ hist_clock();
    sr.last_artist = sr.seed_artist = -1;
    sr.since_seed_artist = 2;
    if (!sr_build_pool(seed)) { sr_free(); return; }
    sr.seed = seed;
    int n = playlist_amount();
    for (int i = 0; i < n; i++)
    {
        if (playlist_get_track_info(NULL, i, &info) < 0) continue;
        int t = sl_lib_find_path(info.filename);
        if (t >= 0) sr.played[t / 32] |= 1u << (t % 32);
        if ((i & 15) == 15) yield();
    }
    sr.active = true;
    strlcpy(sr.seed_title, sl_lib.tracks[seed].title, sizeof sr.seed_title);
    sl_log("radio: carried on from \"%s\" after start-up, %d songs listed", sr.seed_title, n);
}

/* With the UI tick: two songs left, add more. */
static void sr_poll(void)
{
    if (!sr_resume_checked && !sr.active && lib_state == SL_LIB_READY && (audio_status() & AUDIO_STATUS_PLAY))
        sr_resume();
    if (!sr.active || quiz_clip_active() || !(audio_status() & AUDIO_STATUS_PLAY)) return;
    if (playlist_get_display_index() < playlist_amount() - 1) return;
    int added = 0;
    char path[MAX_PATH];
    for (int i = 0; i < SR_MORE; i++)
    {
        int t = sr_pick();
        if (t < 0) break;
        if (playlist_insert_track(NULL, sl_track_path(t, path, sizeof path), PLAYLIST_INSERT_LAST, false, false) < 0) break;
        added++;
    }
    playlist_sync(NULL);
    if (!added) { sl_log("radio: nothing more to add"); sr_stop(); return; }
    if (uq_open) uq_rebuild_soon();
}
