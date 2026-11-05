/*
 *  Your taste, for For you and Radio:
 *
 *   - songs you swiped: Your taste deals a few songs from the card as cards,
 *     each playing from its middle; swipe right for one you like, left for
 *     one you don't, up for one you love. For you deals them once by
 *     itself; Your taste deals more, each card picked by what the swipes
 *     so far say you like (the taste model, below). Radio leans towards the
 *     artists you liked and away from the ones you passed, Loved takes the
 *     songs you liked, and For you > Picked for you lists the ones the
 *     model rates highest.
 *   - artists you never want to hear ("Don't play this artist" on a song's
 *     sheet): left out of Radio, Shuffle all and the smart playlists. For
 *     you > Hidden artists lets them back.
 *   - Loved: your Spotify Liked Songs found on the card (the computer
 *     helper's liked.tsv, see sloppy_spotify.inc), the songs you liked
 *     here, then what you play most.
 *
 *  Both lists are text files under /.rockbox/sloppy, kept by file and by
 *  name so a rebuilt library keeps them: taste.txt ("+path" liked, "*path"
 *  loved, "-path" passed, or "#skipped", oldest first) and
 *  hidden-artists.txt (an artist a line).
 *
 *  The snippets are the music quiz's clips (sloppy_quiz.inc): the song you
 *  were playing stops, and comes back where it was when you leave.
 */

#define TS_TASTE ROCKBOX_DIR "/sloppy/taste.txt"
#define TS_HIDDEN ROCKBOX_DIR "/sloppy/hidden-artists.txt"
#define TS_HIDDEN_MAX 64
#define TS_RATED_MAX 400
#define TS_DECK 15                       /* cards dealt at a time */
#define TS_LOVED_MAX 500

static struct
{
    bool loaded, asked;                  /* asked: taste.txt exists */
    char hidden[TS_HIDDEN_MAX][SP_FIELD]; /* as the album artist reads */
    int n_hidden;
    char (*rated)[MAX_PATH];             /* "+" or "-" and the song's path */
    int n_rated;
    int on_card;                         /* the first this many are taste.txt as it is on the card; -1 it must be written whole */
    /* per album: bit 0 hidden artist */
    uint8_t *flag;
    int flag_n, gen, flag_gen, hgen;     /* hgen: the hidden list's changes */
    const struct sl_album *flag_lib;
} ts;

static void ts_make_room(void);
static void ts_load(void)
{
    if (ts.loaded) return;
    ts.loaded = true;
    char line[MAX_PATH + 4];
    int fd = open(TS_HIDDEN, O_RDONLY);
    if (fd >= 0)
    {
        while (ts.n_hidden < TS_HIDDEN_MAX && read_line(fd, line, sizeof line) > 0)
            if (line[0] && line[0] != '#') strlcpy(ts.hidden[ts.n_hidden++], line, SP_FIELD);
        close(fd);
    }
    ts.rated = malloc(TS_RATED_MAX * sizeof *ts.rated);
    fd = open(TS_TASTE, O_RDONLY);
    if (fd >= 0)
    {
        ts.asked = true;
        int lines = 0;
        while (ts.rated && read_line(fd, line, sizeof line) > 0)
            if ((line[0] == '+' || line[0] == '-' || line[0] == '*') && line[1] == '/')
            {
                if (ts.n_rated == TS_RATED_MAX) ts_make_room();   /* a file grown past the cap by appending: the newest kept */
                strlcpy(ts.rated[ts.n_rated++], line, MAX_PATH);
                lines++;
            }
        /* a power cut mid-append leaves a last line with no end: the next
         * swipe would be glued onto it, so the file is written again whole */
        char end = '\n';
        if (lseek(fd, -1, SEEK_END) >= 0 && read(fd, &end, 1) != 1) end = '\n';
        close(fd);
        ts.on_card = lines == ts.n_rated && end == '\n' ? ts.n_rated : -1;
    }
}

static bool ts_save_file(const char *path, bool taste)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = true;
    char line[MAX_PATH + 4];
    int n = taste ? (ts.n_rated ? ts.n_rated : 1) : ts.n_hidden;
    for (int i = 0; ok && i < n; i++)
    {
        int len = !taste ? snprintf(line, sizeof line, "%s\n", ts.hidden[i])
                : ts.n_rated ? snprintf(line, sizeof line, "%s\n", ts.rated[i])
                : snprintf(line, sizeof line, "#skipped\n");
        ok = write(fd, line, len) == len;
    }
    if (ok) ok = fsync(fd) == 0;   /* on the card before the rename: no empty file after a power cut */
    if (close(fd) < 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) { remove(tmp); toast_warn("Could not save that"); }
    if (ok && taste) ts.on_card = ts.n_rated;
    return ok;
}
/* The swipes onto the card as they are made, so leaving half way (or the
 * player going off) keeps every one: the new ones added to the end of
 * taste.txt, or the whole of it written again after an undo or a trim. */
static void ts_flush(void)
{
    if (!ts.rated || ts.on_card == ts.n_rated) return;
    if (ts.on_card < 0 || ts.on_card > ts.n_rated) { ts_save_file(TS_TASTE, true); return; }
    int fd = open(TS_TASTE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    bool ok = fd >= 0;
    char line[MAX_PATH + 4];
    for (int i = ts.on_card; ok && i < ts.n_rated; i++)
    {
        int len = snprintf(line, sizeof line, "%s\n", ts.rated[i]);
        ok = write(fd, line, len) == len;
    }
    if (fd >= 0 && close(fd) < 0) ok = false;
    if (ok) ts.on_card = ts.n_rated;
    else ts_save_file(TS_TASTE, true);   /* half a line may be there: all of it again */
}

static bool ts_same_artist(const char *a, const char *b)
{
    char x[SP_FIELD], y[SP_FIELD];
    sp_artist_norm(a, strlen(a), x, sizeof x);
    sp_artist_norm(b, strlen(b), y, sizeof y);
    return x[0] && !strcmp(x, y);
}

/* The rating of track t: 1 liked (or loved), -1 passed, 0 not swiped. */
static int ts_rating(int t)
{
    char p[MAX_PATH];
    if (!ts.n_rated) return 0;
    sl_track_path(t, p, sizeof p);
    for (int i = 0; i < ts.n_rated; i++)
        if (!strcmp(ts.rated[i] + 1, p)) return ts.rated[i][0] == '-' ? -1 : 1;
    return 0;
}

/* Loved from Now Playing: the song joins the swipes' yes pile, or leaves
 * the list altogether (a plain unlove is not a pass). */
static void ts_love(int t, bool love)
{
    ts_load();
    if (!ts.rated || t < 0 || t >= sl_lib.n_tracks) return;
    char p[MAX_PATH];
    sl_track_path(t, p, sizeof p);
    int i = 0;
    while (i < ts.n_rated && strcmp(ts.rated[i] + 1, p)) i++;
    if (love)
    {
        if (i == ts.n_rated) { if (ts.n_rated >= TS_RATED_MAX) return; ts.n_rated++; }
        else if (ts.rated[i][0] == '*') return;      /* loved already: stays loved */
        snprintf(ts.rated[i], MAX_PATH, "+%s", p);
    }
    else if (i < ts.n_rated)
        memmove(ts.rated[i], ts.rated[i + 1], (ts.n_rated-- - i - 1) * sizeof ts.rated[0]);
    else return;
    ts.gen++;
    ts_save_file(TS_TASTE, true);
}

/* ------------------------------------------------- the taste model */
/* What you like, learned from every swipe. A song is a few things at once:
 * itself, its artist, its album artist (when that is someone else), its
 * album, its genre and its decade. Each of those keeps two counts, likes
 * and passes, and a guess at how much you like it, (likes + 1) / (likes +
 * passes + 2): a coin that starts fair. One pass for an artist pulls the
 * guess from 1/2 to 1/3, three pull it to 1/5, so their other songs come up
 * far less, but one bad song bans no one. A love counts twice. A newer
 * swipe counts more: one TM_HALF swipes old counts half. What you play and
 * what you skip in ordinary listening count too, much more weakly.
 *
 * A song's score adds its parts' guesses up as log-odds, each drawn from
 * around itself as widely as it is unsure (Thompson sampling), so what you
 * have not heard yet still gets a look in. The next card is the best of
 * forty candidates: at first drawn from the whole library, then more and
 * more from near the songs you liked (their artist's other albums, their
 * genre's, found in two sorted lists of the albums), up to three in four
 * once twenty are liked. A card now and then goes to the least known
 * candidate instead, so it keeps finding new music: one in four at first,
 * fewer as it learns (one in ten by a hundred swipes, one in sixteen at
 * four hundred). Nothing is saved but taste.txt, a line a swipe as it is
 * made: the counts are worked out again from it, by file path, so a
 * rebuilt library keeps them. */
enum { TM_SONG, TM_ARTIST, TM_ALBUM_ARTIST, TM_ALBUM, TM_GENRE, TM_DECADE, TM_KINDS };
/* why a card was dealt, for the line on it */
enum { TM_WHY_NONE, TM_WHY_NEW, TM_WHY_ARTIST, TM_WHY_ALBUM, TM_WHY_GENRE, TM_WHY_DECADE, TM_WHY_OTHER };
static const float tm_weight[TM_KINDS] = { 1.0f, 1.0f, 0.5f, 0.6f, 0.7f, 0.35f };
static const uint8_t tm_why_of[TM_KINDS] = { TM_WHY_OTHER, TM_WHY_ARTIST, TM_WHY_ARTIST, TM_WHY_ALBUM, TM_WHY_GENRE, TM_WHY_DECADE };
#define TM_CAP 8192                      /* features: a power of two, three quarters used at most */
#define TM_HALF 120                      /* swipes after which a swipe counts half */
#define TM_HIST_MAX 2.0f                 /* the most what you play or skip can count for a feature */
#define TM_NOISE 0.6                     /* how far a guess is drawn from itself, of its spread */
#define TM_POOL 40                       /* candidates for each card */
#define TM_NEAR_MAX 30                   /* of them at most from near a song you liked */

struct tm_feat { uint32_t key; float yes, no; };
static struct
{
    struct tm_feat *f;
    int n;
    uint32_t (*al)[TM_KINDS];            /* each album's keys (its artist under TM_ARTIST) */
    int *by[2];                          /* the albums in order of their artist's key, and of their genre's */
    int al_n, gen, lib_n;
    const struct sl_track *lib;
    double scale;                        /* the newest swipe's weight: older ones weigh less beside it */
    int swipes;
    bool ok;
} tm;

static uint32_t tm_key(int kind, const char *s)
{
    if (!s || !s[0]) return 0;
    uint32_t h = 2166136261u ^ ((uint32_t)kind * 0x9e3779b9u);
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}
static uint32_t tm_artist_key(int kind, const char *a)
{
    if (!sr_artist_usable(a)) return 0;
    char n[SP_FIELD];
    sp_artist_norm(a, strlen(a), n, sizeof n);
    return tm_key(kind, n);
}
/* lower case, the ends trimmed; "<Untagged>" is no genre */
static void tm_lower(const char *in, char *out, size_t n)
{
    size_t k = 0;
    while (*in == ' ') in++;
    for (; *in && k + 1 < n; in++) out[k++] = (char)tolower((unsigned char)*in);
    while (k && out[k - 1] == ' ') k--;
    out[k] = '\0';
}
static void tm_album_keys(int a, uint32_t *key)
{
    const struct sl_album *al = &sl_lib.albums[a];
    char b[SP_FIELD + 16];
    memset(key, 0, TM_KINDS * sizeof *key);
    key[TM_ARTIST] = tm_artist_key(TM_ARTIST, al->artist);
    key[TM_ALBUM_ARTIST] = tm_artist_key(TM_ALBUM_ARTIST, al->artist);
    if (al->title && al->title[0])
    {
        int k = snprintf(b, sizeof b, "%08x/", (unsigned)key[TM_ALBUM_ARTIST]);
        tm_lower(al->title, b + k, sizeof b - k);
        key[TM_ALBUM] = tm_key(TM_ALBUM, b);
    }
    if (al->genre)
    {
        tm_lower(al->genre, b, sizeof b);
        if (strncmp(b, "<untagged", 9)) key[TM_GENRE] = tm_key(TM_GENRE, b);
    }
    if (al->year > 0)
    {
        snprintf(b, sizeof b, "%d", al->year / 10 * 10);
        key[TM_DECADE] = tm_key(TM_DECADE, b);
    }
}
static uint32_t tm_song_key(uint32_t path_hash) { return (path_hash ^ 0x5bd1e995u) | 1; }
/* Track t's keys. The album artist counts on its own only when the song
 * has an artist of its own (a guest, a compilation). */
static void tm_keys(int t, uint32_t *key)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    memcpy(key, tm.al[tr->album], TM_KINDS * sizeof *key);
    key[TM_SONG] = tm_song_key(sl_track_hist_hash(t));
    if (tr->artist) key[TM_ARTIST] = tm_artist_key(TM_ARTIST, tr->artist);
    else key[TM_ALBUM_ARTIST] = 0;
}
static struct tm_feat *tm_find(uint32_t key, bool make)
{
    if (!key || !tm.f) return NULL;
    for (unsigned i = key & (TM_CAP - 1);; i = (i + 1) & (TM_CAP - 1))
    {
        struct tm_feat *f = &tm.f[i];
        if (f->key == key) return f;
        if (f->key) continue;
        if (!make || tm.n >= TM_CAP * 3 / 4) return NULL;
        f->key = key;
        tm.n++;
        return f;
    }
}
/* Counts onto each of track t's features, but the song itself without
 * song (what you play says little about one song you play anyway). */
static void tm_add(int t, float yes, float no, bool song)
{
    uint32_t key[TM_KINDS];
    tm_keys(t, key);
    for (int k = song ? 0 : 1; k < TM_KINDS; k++)
    {
        struct tm_feat *f = tm_find(key[k], yes > 0 || no > 0);
        if (!f) continue;
        f->yes = f->yes + yes > 0 ? f->yes + yes : 0;
        f->no = f->no + no > 0 ? f->no + no : 0;
    }
}
/* One swipe, way '+', '*' or '-', in; or taken back out (the newest). */
static void tm_swipe(int t, char way, bool out)
{
    if (!tm.ok || t < 0 || t >= sl_lib.n_tracks) return;
    if (!out) tm.scale *= 1.0057929410678534;   /* 2^(1/TM_HALF) */
    float w = (float)tm.scale * (way == '*' ? 2 : 1) * (out ? -1 : 1);
    if (way == '-') tm_add(t, 0, w, true);
    else tm_add(t, w, 0, true);
    if (out) tm.scale /= 1.0057929410678534;
    tm.swipes += out ? -1 : 1;
}
static int tm_by_kind;
static int tm_by_cmp(const void *a, const void *b)
{
    uint32_t x = tm.al[*(const int *)a][tm_by_kind], y = tm.al[*(const int *)b][tm_by_kind];
    return x != y ? (x > y) - (x < y) : *(const int *)a - *(const int *)b;
}
static struct sl_graves graves;          /* sloppy_grave.inc */
static void graves_load(void);
static int tm_grave_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct sl_grave *)a)->hash, y = ((const struct sl_grave *)b)->hash;
    return (x > y) - (x < y);
}
/* The model for the swipes and library as they are: worked out again when
 * either changed some other way than a swipe here. False without memory. */
static bool tm_ready(void)
{
    ts_load();
    if (tm.ok && tm.gen == ts.gen && tm.lib == sl_lib.tracks && tm.lib_n == sl_lib.n_tracks && tm.al_n == sl_lib.n_albums)
        return true;
    int na = sl_lib.n_albums, nt = sl_lib.n_tracks;
    if (!tm.f) tm.f = malloc(TM_CAP * sizeof *tm.f);
    free(tm.al);
    tm.al = malloc((na ? na : 1) * sizeof *tm.al);
    tm.ok = false;
    if (!tm.f || !tm.al) return false;
    memset(tm.f, 0, TM_CAP * sizeof *tm.f);
    tm.n = 0;
    for (int a = 0; a < na; a++)
    {
        tm_album_keys(a, tm.al[a]);
        if ((a & 511) == 511) yield();
    }
    for (int j = 0; j < 2; j++)
    {
        free(tm.by[j]);
        tm.by[j] = malloc((na ? na : 1) * sizeof *tm.by[j]);
        if (!tm.by[j]) continue;          /* no memory: near songs from their own album only */
        for (int a = 0; a < na; a++) tm.by[j][a] = a;
        tm_by_kind = j ? TM_GENRE : TM_ARTIST;
        qsort(tm.by[j], na, sizeof *tm.by[j], tm_by_cmp);
    }
    tm.al_n = na; tm.lib = sl_lib.tracks; tm.lib_n = nt; tm.gen = ts.gen;
    tm.scale = 1; tm.swipes = 0;
    tm.ok = true;
    /* what you play: a little towards its artist, album, genre and decade */
    struct hist_view *v = hist_view_load();
    if (v)
    {
        for (int t = 0; t < nt; t++)
        {
            if (v->count[t]) tm_add(t, 0.2f * (v->count[t] < 5 ? v->count[t] : 5), 0, false);
            if ((t & 1023) == 1023) yield();
        }
        hist_view_free(v);
    }
    /* what you skip: the Skip Graveyard's counts, against the song and the rest */
    graves_load();
    if (graves.n)
    {
        static struct sl_grave sorted[SL_GRAVE_MAX];
        int n = graves.n < SL_GRAVE_MAX ? graves.n : SL_GRAVE_MAX;
        memcpy(sorted, graves.g, n * sizeof *sorted);
        qsort(sorted, n, sizeof *sorted, tm_grave_cmp);
        for (int t = 0; t < nt; t++)
        {
            struct sl_grave want = { .hash = sl_track_hist_hash(t) };
            const struct sl_grave *g = bsearch(&want, sorted, n, sizeof *sorted, tm_grave_cmp);
            if (g && g->skips) tm_add(t, 0, 0.35f * (g->skips < 4 ? g->skips : 4), true);
            if ((t & 1023) == 1023) yield();
        }
    }
    for (int i = 0; i < TM_CAP; i++)
    {
        if (tm.f[i].yes > TM_HIST_MAX) tm.f[i].yes = TM_HIST_MAX;
        if (tm.f[i].no > TM_HIST_MAX) tm.f[i].no = TM_HIST_MAX;
    }
    /* the swipes, oldest first; one for a song since gone still ages the rest */
    for (int i = 0; i < ts.n_rated; i++)
    {
        int t = sl_lib_find_path(ts.rated[i] + 1);
        if (t >= 0) tm_swipe(t, ts.rated[i][0], false);
        else { tm.scale *= 1.0057929410678534; tm.swipes++; }
    }
    sl_log("taste: model of %d swipes, %d features", tm.swipes, tm.n);
    return true;
}
/* A feature's counts as they weigh now. */
static void tm_counts(uint32_t key, double *a, double *b)
{
    const struct tm_feat *f = tm_find(key, false);
    *a = f ? f->yes / tm.scale : 0;
    *b = f ? f->no / tm.scale : 0;
}
/* How much you like a feature, 0 to 1, and whether anything is known. */
static double tm_guess(uint32_t key, double *known)
{
    double a, b;
    tm_counts(key, &a, &b);
    if (known) *known = a + b;
    return (a + 1) / (a + b + 2);
}
static double tm_gauss(unsigned *r)
{
    *r = *r * 1103515245u + 12345u;
    double u1 = ((*r >> 8) + 1.0) / 16777217.0;
    *r = *r * 1103515245u + 12345u;
    double u2 = (*r >> 8) / 16777216.0;
    return sqrt(-2 * log(u1)) * cos(6.283185307179586 * u2);
}
/* Track t's score: its parts' guesses as log-odds, added up. With rng each
 * guess is drawn from around itself. why: the part that did the most for
 * it; known: how much is known of it at all. */
static double tm_score(int t, unsigned *rng, int *why, double *known)
{
    uint32_t key[TM_KINDS];
    tm_keys(t, key);
    double s = 0, best = 0.3, ev = 0;
    int w = TM_WHY_NONE;
    for (int k = 0; k < TM_KINDS; k++)
    {
        if (!key[k]) continue;
        double a, b;
        tm_counts(key[k], &a, &b);
        double m = (a + 1) / (a + b + 2), p = m;
        if (rng)
        {
            p += TM_NOISE * sqrt(m * (1 - m) / (a + b + 3)) * tm_gauss(rng);
            p = p < 0.03 ? 0.03 : p > 0.97 ? 0.97 : p;
        }
        s += tm_weight[k] * log(p / (1 - p));
        ev += a + b;
        double c = tm_weight[k] * log(m / (1 - m));
        if (c > best) { best = c; w = tm_why_of[k]; }
    }
    if (why) *why = w;
    if (known) *known = ev;
    return s;
}

/* Per album, bit 0: by an artist you hid. Worked out again when the list
 * or the library changes. */
static const uint8_t *ts_flags(void)
{
    ts_load();
    int na = sl_lib.n_albums;
    if (ts.flag && ts.flag_n == na && ts.flag_gen == ts.hgen && ts.flag_lib == sl_lib.albums) return ts.flag;
    free(ts.flag);
    ts.flag = calloc(na ? na : 1, 1);
    if (!ts.flag) return NULL;
    ts.flag_n = na; ts.flag_gen = ts.hgen; ts.flag_lib = sl_lib.albums;
    if (!ts.n_hidden) return ts.flag;
    static char hid[TS_HIDDEN_MAX][SP_FIELD];
    for (int i = 0; i < ts.n_hidden; i++) sp_artist_norm(ts.hidden[i], strlen(ts.hidden[i]), hid[i], SP_FIELD);
    for (int a = 0; a < na; a++)
    {
        const struct sl_album *al = &sl_lib.albums[a];
        if (!al->artist) continue;
        char me[SP_FIELD];
        sp_artist_norm(al->artist, strlen(al->artist), me, sizeof me);
        if (!me[0]) continue;
        for (int i = 0; i < ts.n_hidden; i++)
            if (!strcmp(hid[i], me)) { ts.flag[a] |= 1; break; }
        if ((a & 511) == 511) yield();
    }
    return ts.flag;
}

static bool ts_blocked_album(int a)
{
    const uint8_t *f = a >= 0 && a < sl_lib.n_albums ? ts_flags() : NULL;
    return f && (f[a] & 1);
}
static bool ts_blocked_track(int t)
{
    return t >= 0 && t < sl_lib.n_tracks && ts_blocked_album(sl_lib.tracks[t].album);
}
/* The model's guess for an album's artist: liked above 0.6, passed below
 * 0.4 (one swipe either way is enough; one of each is neither). */
static double ts_artist_guess(int a)
{
    if (a < 0 || a >= sl_lib.n_albums || !tm_ready()) return 0.5;
    double known, g = tm_guess(tm.al[a][TM_ARTIST], &known);
    return known >= 0.5 ? g : 0.5;
}
static bool ts_liked_album(int a) { return ts_artist_guess(a) > 0.6; }
static bool ts_passed_album(int a) { return ts_artist_guess(a) < 0.4; }

static void ts_hide_artist(const char *artist)
{
    ts_load();
    if (!artist || !artist[0] || ts.n_hidden >= TS_HIDDEN_MAX) return;
    for (int i = 0; i < ts.n_hidden; i++) if (ts_same_artist(ts.hidden[i], artist)) return;
    strlcpy(ts.hidden[ts.n_hidden++], artist, SP_FIELD);
    ts.gen++; ts.hgen++;
    ts_save_file(TS_HIDDEN, false);
    sl_log("taste: hid an artist (%d hidden)", ts.n_hidden);
}
static void ts_show_artist(int i)
{
    if (i < 0 || i >= ts.n_hidden) return;
    memmove(ts.hidden[i], ts.hidden[i + 1], (size_t)(ts.n_hidden - i - 1) * sizeof ts.hidden[0]);
    ts.n_hidden--;
    ts.gen++; ts.hgen++;
    ts_save_file(TS_HIDDEN, false);
}

/* Shuffle all and the like: the list without hidden artists, malloc'd. */
static int *ts_unhidden(const int *tracks, int n, int *out_n)
{
    int *l = malloc((n ? n : 1) * sizeof *l), k = 0;
    if (l) for (int i = 0; i < n; i++) if (!ts_blocked_track(tracks[i])) l[k++] = tracks[i];
    *out_n = k;
    return l;
}

/* ------------------------------------------------------------- Loved */
static int ts_loved(int **out)
{
    ts_load();
    int nt = sl_lib.n_tracks, n = 0;
    int *list = malloc(TS_LOVED_MAX * sizeof *list);
    uint32_t *seen = calloc((nt + 31) / 32 + 1, sizeof *seen);
    *out = list;
    if (!list || !seen) { free(seen); return 0; }
#define TS_ADD(t) do { int t_ = (t); if (t_ >= 0 && t_ < nt && n < TS_LOVED_MAX && !(seen[t_ / 32] & (1u << (t_ % 32))) \
                         && !ts_blocked_track(t_)) { seen[t_ / 32] |= 1u << (t_ % 32); list[n++] = t_; } } while (0)
    /* Spotify's Liked Songs, as the computer copied them */
    int fd = open(SPO_DIR "/liked.tsv", O_RDONLY);
    if (fd >= 0 && sp_build_lib())
    {
        char line[SP_FIELD * 3 + 64];
        struct sp_song s;
        int read = 0;
        while (n < TS_LOVED_MAX && read_line(fd, line, sizeof line) > 0)
        {
            if (!sp_parse_line(line, &s)) continue;
            TS_ADD(sp_match(&s, spo.lib, spo.lib_n));
            if ((++read & 31) == 0) yield();
        }
    }
    if (fd >= 0) close(fd);
    /* the songs you swiped up, then right, newest first */
    for (int way = 0; way < 2; way++)
        for (int i = ts.n_rated - 1; i >= 0; i--)
            if (ts.rated[i][0] == (way ? '+' : '*')) TS_ADD(sl_lib_find_path(ts.rated[i] + 1));
    /* what you play most */
    struct hist_view *v = hist_view_load();
    if (v)
    {
        int *most = NULL;
        int m = smart_build(v, SMART_MOST, hist_clock(), &most, NULL);
        for (int i = 0; i < m && i < 60; i++) if (v->count[most[i]] >= 3) TS_ADD(most[i]);
        free(most);
        hist_view_free(v);
    }
#undef TS_ADD
    free(seen);
    return n;
}
static void loved_page(void)
{
    int *list = NULL;
    int n = ts_loved(&list);
    sl_log("loved: %d songs", n);
    char b[32];
    snprintf(b, sizeof b, "%d SONGS", n);
    tracks_page("Loved", "Your Spotify likes, your swipes, your favourites", IC_HEART, "Nothing loved yet", "Swipe right on songs under "
                "Your taste, like them on Spotify or play your favourites, and they collect here.", list, n, b);
}

/* --------------------------------------------------- Picked for you */
/* Songs not swiped yet, best first by the model's plain guess (no noise),
 * at most three an artist; none of them hidden, buried or short. */
#define TS_PICKS 60
struct ts_pick { float score; int t; };
static int ts_pick_cmp(const void *a, const void *b)
{
    float x = ((const struct ts_pick *)a)->score, y = ((const struct ts_pick *)b)->score;
    return (x < y) - (x > y);
}
static int ts_picks(int **out)
{
    *out = NULL;
    if (!tm_ready() || !tm.swipes) return 0;
    int nt = sl_lib.n_tracks, n = 0;
    struct ts_pick *all = malloc((nt ? nt : 1) * sizeof *all);
    uint8_t *rated = calloc(nt ? nt : 1, 1);
    if (!all || !rated) { free(all); free(rated); return 0; }
    for (int i = 0; i < ts.n_rated; i++)
    {
        int t = sl_lib_find_path(ts.rated[i] + 1);
        if (t >= 0) rated[t] = 1;
    }
    graves_load();
    /* an album nothing good is known of is passed over whole: only a song
     * with an artist of its own could still score there */
    uint8_t *dull = calloc(sl_lib.n_albums ? sl_lib.n_albums : 1, 1);
    if (dull)
        for (int a = 0; a < sl_lib.n_albums; a++)
        {
            double sa = 0;
            for (int k = TM_ARTIST; k < TM_KINDS; k++)
                if (tm.al[a][k] && k != TM_ALBUM_ARTIST)
                {
                    double g = tm_guess(tm.al[a][k], NULL);
                    sa += tm_weight[k] * log(g / (1 - g));
                }
            dull[a] = sa <= 0.2;
        }
    for (int t = 0; t < nt; t++)
    {
        const struct sl_track *tr = &sl_lib.tracks[t];
        if ((t & 1023) == 1023) yield();
        if (dull && dull[tr->album] && !tr->artist) continue;
        if (rated[t] || !tr->file || (tr->len && tr->len < 30000) || ts_blocked_track(t)) continue;
        double known, s = tm_score(t, NULL, NULL, &known);
        if (known < 0.5 || s <= 0.2) continue;   /* only what the swipes say something good about */
        all[n].score = (float)s;
        all[n++].t = t;
    }
    qsort(all, n, sizeof *all, ts_pick_cmp);
    int *list = malloc(TS_PICKS * sizeof *list), k = 0;
    static uint32_t by[TS_PICKS];
    if (list)
        for (int i = 0; i < n && k < TS_PICKS; i++)
        {
            int t = all[i].t, same = 0;
            uint32_t key[TM_KINDS];
            tm_keys(t, key);
            int g = sl_grave_find(&graves, sl_track_hist_hash(t));
            if (g >= 0 && sl_grave_buried(&graves.g[g])) continue;
            for (int j = 0; j < k; j++) same += key[TM_ARTIST] && by[j] == key[TM_ARTIST];
            if (same >= 3) continue;
            by[k] = key[TM_ARTIST];
            list[k++] = t;
        }
    free(all); free(rated); free(dull);
    *out = list;
    return k;
}
/* Your mix: the newest songs you liked, and between them twice as many the
 * model picks that you never swiped, shuffled. A malloc'd list. */
#define TS_MIX_LIKED 30
static int ts_mix(int **out)
{
    int *picks = NULL, np = ts_picks(&picks), nl = 0;
    int liked[TS_MIX_LIKED];
    for (int i = ts.n_rated - 1; i >= 0 && nl < TS_MIX_LIKED; i--)
    {
        if (ts.rated[i][0] == '-') continue;
        int t = sl_lib_find_path(ts.rated[i] + 1);
        if (t >= 0 && !ts_blocked_track(t)) liked[nl++] = t;
    }
    int *mix = malloc((np + nl ? np + nl : 1) * sizeof *mix), n = 0;
    if (mix)
        for (int i = 0, j = 0; i < nl || j < np; )
        {
            if (i < nl) mix[n++] = liked[i++];
            for (int k = 0; k < 2 && j < np; k++) mix[n++] = picks[j++];
        }
    free(picks);
    *out = mix;
    return n;
}
static void picks_page(void)
{
    int *list = NULL;
    int n = ts_picks(&list);
    sl_log("picked for you: %d songs", n);
    char b[32];
    snprintf(b, sizeof b, "%d SONGS", n);
    tracks_page("Picked for you", "Songs your swipes say you'll like", IC_FORYOU, "Nothing picked yet",
                "Swipe a few songs under Your taste and the ones it thinks you'll like show up here.", list, n, b);
}

/* Forget every swipe: the deck starts from nothing again. */
static void taste_reset_go_cb(lv_event_t *e)
{
    (void)e;
    ts_load();
    ts.n_rated = 0;
    ts.gen++;
    ts.asked = true;
    ts_save_file(TS_TASTE, true);
    sl_log("taste: started over");
    page_pop();
    toast("Your taste starts over");
}
static void taste_reset_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
static void taste_reset_page(void)
{
    lv_obj_t *p, *body = page_new("Start over?", &p);
    set_confirm(body, "YOUR TASTE",
        "This forgets every song you swiped, so the cards, Radio and Picked for you start learning again. "
        "Loved keeps your Spotify likes and favourites, and Hidden artists stay hidden.",
        "Keep my taste", "Go back without changing anything", taste_reset_keep_cb,
        "Start over", "Forget every swipe now", taste_reset_go_cb, true);
    page_push(p);
}

/* ------------------------------------------------- the swipe deck */
/* The cards are a stack: the song on top and the two after it under it,
 * smaller and dimmer, a strip of each showing below. The top card follows
 * the finger, turning round a point under it, and the ones under it come
 * up as it goes. Let go far enough, or with a flick, and it flies off the
 * way it was thrown; otherwise it springs back.
 *
 * Each card on screen is a picture of itself (RGB565 plus alpha, made by
 * LVGL from the real card, and again if its cover comes later): turning a
 * picture is one transform a frame, where turning the live card drew it
 * into a layer first, every frame. At rest the top card is not turned at
 * all and draws as a plain copy. The card under it is drawn from copies at
 * four steps of its size, made one a tick while the stack sits still, so a
 * drag never scales anything; the one at the bottom only ever shows as a
 * strip, so it is a plain shape (its picture is made, hidden, for when it
 * comes up). Nothing heavy waits for the frame a card lands in: the next
 * card is picked and pictured on the ticks after. */
#define TD_CARD_W 360
/* tall enough for the cover and both lines at any text size, with a clear
 * bottom edge: that edge is what shows of the cards underneath */
static int td_card_h(void)
{
    int h = 22 + SL_ART_GRID + 16 + lv_font_get_line_height(&ss_semibold_26) + 4
            + lv_font_get_line_height(&ss_regular_18) + 6 + lv_font_get_line_height(&ss_regular_16) + 2 * 11 + 8;
    return h > 350 ? h : 350;
}
#define TD_CARD_H td_card_h()
#define TD_TOP (SB_H + 80)
#define TD_STACK 3                       /* cards on screen */
#define TD_PEEK 11                       /* px of each card under showing below the one above */
#define TD_PIVOT (TD_CARD_H + 60)        /* a card turns round a point this far down from its top */
#define TD_FLING 110                     /* px across that rate a card */
#define TD_UP 130                        /* px up that love it */
#define TD_FLICK 700                     /* px/s: a flick rates it from shorter */
#define TD_RISE 200                      /* px across by which the next card has come up */
#define TD_OFF 470                       /* px across: off the screen, turned */
#define TD_LIFT 8                        /* held: 3 % bigger */
#define TD_BTN_Y (TD_TOP + TD_CARD_H + 2 * TD_PEEK + 24)
/* the lifted card's shadow: elevation 3's (SL_E3) blur and strength, a
 * soft card-shaped box a little below the card, not turned with it. Only
 * its edges can show, so it is three strips (left, right, bottom), made
 * once a page: a few thousand pixels a frame, where a shadow box cost LVGL
 * a fresh blur every frame. */
#define TD_SH_BLUR 14                    /* px each side of the edge it fades over */
#define TD_SH_DY 8                       /* px down */
#define TD_SH_OPA 110
#define TD_STEPS 4                       /* the card under, ready at 240, 244, 248 and 252 of 256 */

struct td_slot
{
    lv_obj_t *src, *cover, *img;         /* the real card (hidden), its cover, its picture */
    lv_draw_buf_t *buf;                  /* NULL without memory: the real card shows instead */
    int card;                            /* its place in the deck */
    uint32_t art;                        /* the cover's children when pictured: more, its art came */
    const lv_draw_buf_t *shown;          /* its picture, a step of it or td.turn */
    lv_color_t wash;                     /* the page under it, in its cover's colour */
};

static struct
{
    lv_obj_t *page, *stamp[3], *btn[4], *count, *hint, *ghost;   /* stamps: pass, like, love; buttons: pass, like, undo, love */
    lv_draw_buf_t *stamp_buf[3];
    lv_draw_buf_t *stamp_turn;           /* the one stamp showing, turned and sized: made when its step moves */
    int stamp_key, stamp_ox, stamp_oy;
    lv_draw_buf_t *step[TD_STEPS];       /* the card under the top one at each step of its size, */
    int step_card;                       /* made one a tick while the stack sits still */
    uint8_t step_ok;                     /* bit j: step[j] holds step_card */
    lv_draw_buf_t *next0;                /* the card after it at the first step, ready for when it comes up */
    int next0_card;
    lv_obj_t *shadow[3];                 /* left, right, bottom strips */
    lv_draw_buf_t *shadow_buf[3];
    int ghost_in;                        /* the bottom card arriving, of 256 */
    lv_draw_buf_t *turn;                 /* the top card turned, at its last step of turn */
    int turn_card, turn_rot, turn_scale, turn_ox, turn_oy;
    bool turn_fine;                      /* the turned copy bilinear throughout */
    lv_draw_buf_t *turn_back;            /* the next turned copy, made over two frames while the last one shows */
    int back_card, back_rot, back_scale, back_row;   /* back_row: made down to here, -1 none under way */
    int refine_row;                      /* made fine down to here, a band a tick */
    int still_x, still_y, still_ticks;   /* where the held card was a tick ago, and for how many */
    lv_obj_t *wash_img;                  /* the cover's colour at the top of the page, fading down */
    lv_draw_buf_t *wash_buf;
    lv_color_t wash_cur;
    int wash_step;                       /* of TD_WASH_STEPS made so far */
    struct td_slot slot[TD_STACK];       /* card i is in slot i % TD_STACK */
    int deck[TS_DECK], n, at, liked, dealt;   /* dealt: cards picked so far, each as it comes up */
    char way[TS_DECK];                   /* '+', '*' or '-': where undo brings it from */
    uint8_t why[TS_DECK];                /* TM_WHY_*: the line on the card */
    uint32_t akey[TS_DECK];              /* its artist, to keep the deck varied */
    uint16_t kept;                       /* bit i: card i's rating went in taste.txt */
    bool first;                          /* For you dealt it: For you follows */
    bool held, dragging, flying, past;   /* past: beyond TD_FLING, for the tick */
    bool returning;                      /* undo bringing a card back: no stamp, no grabbing */
    int x, y, lift;                      /* the top card */
    int fx, fy, flift, tx, ty;           /* the move under way: from, to (lift goes to 0) */
    int bscale[4];                       /* the buttons' press */
    int bsc[4], bfill[4];                /* as drawn: size of 256, fill of 255 */
    uint32_t ink[4];
    int rot_sign;                        /* -1: held low on the card, so it turns the other way */
    lv_color_t wash_from, wash_to;       /* the page's colour, from the cover on top */
    unsigned pending;                    /* bit k: slot k waits for its picture */
    lv_point_t down;
    lv_timer_t *timer;
    unsigned rng;
} td;
/* What was playing when the deck came up: it is paused while the deck is
 * open, so the cards' clips are the only audio, and plays on after (it
 * froze on a swipe over playing music, 3 Nov: with the music stopped, the
 * player has the memory and the time for the deck). */
static bool td_resume;
static long td_test_kb;                  /* the harness: free memory as the R1 would have it, 0 the real one */

/* May track t be the next card? Half a minute long or more, not hidden,
 * not swiped, not in the deck, and its artist not on either of the last
 * two cards nor twice in the deck already. */
static bool td_can(int t, uint32_t artist)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    if ((tr->len && tr->len < 30000) || !tr->file || ts_blocked_track(t) || ts_rating(t)) return false;
    int same = 0;
    for (int i = 0; i < td.dealt; i++)
    {
        if (td.deck[i] == t) return false;
        if (artist && td.akey[i] == artist && (++same >= 2 || i >= td.dealt - 2)) return false;
    }
    return true;
}
static unsigned td_rand(void) { td.rng = td.rng * 1103515245u + 12345u; return td.rng >> 8; }
/* A song near one you liked, the newer likes more often: another album by
 * its artist (two times in three) or in its genre, or its own album when
 * there is none. -1: none. */
static int td_near(int likes)
{
    if (!likes || !sl_lib.n_albums) return -1;
    unsigned r1 = td_rand() % (unsigned)likes, r2 = td_rand() % (unsigned)likes;
    int k = (int)(r1 < r2 ? r1 : r2);    /* counted from the newest */
    for (int i = ts.n_rated - 1; i >= 0; i--)
    {
        if (ts.rated[i][0] == '-' || k--) continue;
        int t = sl_lib_find_path(ts.rated[i] + 1);
        if (t < 0) return -1;
        int a = sl_lib.tracks[t].album, j = td_rand() % 3 ? 0 : 1;
        uint32_t key = tm.al[a][j ? TM_GENRE : TM_ARTIST];
        const int *by = tm.by[j];
        if (key && by)
        {
            int lo = 0, hi = sl_lib.n_albums;   /* the first album with this key */
            while (lo < hi) { int m = (lo + hi) / 2; if (tm.al[by[m]][j ? TM_GENRE : TM_ARTIST] < key) lo = m + 1; else hi = m; }
            int n = 0;
            while (lo + n < sl_lib.n_albums && tm.al[by[lo + n]][j ? TM_GENRE : TM_ARTIST] == key) n++;
            if (n > 1)
            {
                int b = by[lo + (int)(td_rand() % (unsigned)n)];
                if (b == a) b = by[lo + (int)(td_rand() % (unsigned)n)];
                a = b;
            }
        }
        const struct sl_album *al = &sl_lib.albums[a];
        return al->n_tracks && al->tracks ? al->tracks[td_rand() % (unsigned)al->n_tracks] : -1;
    }
    return -1;
}
/* The next card: the best of a few dozen candidates by the model, now and
 * then the least known instead. -1 when the library has nothing left. */
static int td_pick(uint8_t *why, uint32_t *artist)
{
    int nt = sl_lib.n_tracks, cand[TM_POOL], nc = 0;
    uint32_t ak[TM_POOL];
    if (!nt || !tm_ready()) return -1;
    int likes = 0;
    for (int i = 0; i < ts.n_rated; i++) likes += ts.rated[i][0] != '-';
    int near = likes * 3 / 2 < TM_NEAR_MAX ? likes * 3 / 2 : TM_NEAR_MAX;
    for (int tries = 0; nc < TM_POOL && tries < TM_POOL * 4; tries++)
    {
        int t = tries < near * 2 && nc < near ? td_near(likes) : (int)(td_rand() % (unsigned)nt);
        if (t < 0 || t >= nt) continue;
        bool dup = false;
        for (int i = 0; i < nc && !dup; i++) dup = cand[i] == t;
        uint32_t key[TM_KINDS];
        tm_keys(t, key);
        if (dup || !td_can(t, key[TM_ARTIST])) continue;
        ak[nc] = key[TM_ARTIST];
        cand[nc++] = t;
    }
    if (!nc)                             /* a small library, nearly all swiped: whatever is left */
        for (int i = 0, s = (int)(td_rand() % (unsigned)nt); i < nt && !nc; i++)
        {
            int t = (s + i) % nt;
            uint32_t key[TM_KINDS];
            tm_keys(t, key);
            if (td_can(t, key[TM_ARTIST])) { ak[0] = key[TM_ARTIST]; cand[nc++] = t; }
        }
    if (!nc) return -1;
    /* the least known now and then: one in four at first, fewer as it learns */
    bool explore = (int)(td_rand() % 1000) < 50 + 200 * 30 / (30 + tm.swipes);
    int best = 0, w = TM_WHY_NONE;
    double top = -1e9;
    for (int i = 0; i < nc; i++)
    {
        int wi;
        double known, sc = tm_score(cand[i], explore ? NULL : &td.rng, &wi, &known);
        if (explore) sc = -known + (td_rand() & 255) / 4096.0;   /* the least known, ties at random */
        if (sc > top) { top = sc; best = i; w = wi; }
    }
    double known;
    tm_score(cand[best], NULL, NULL, &known);
    *why = explore || known < 0.5 ? TM_WHY_NEW : w ? w : TM_WHY_OTHER;
    *artist = ak[best];
    return cand[best];
}
/* Cards picked up to card upto (not past the deck): each picked as late as
 * it can be, so it knows the swipes just made. */
static void td_deal_to(int upto)
{
    while (td.dealt < upto && td.dealt < td.n)
    {
        uint8_t why;
        uint32_t artist;
        int t = td_pick(&why, &artist);
        if (t < 0) { td.n = td.dealt; break; }
        td.deck[td.dealt] = t;
        td.why[td.dealt] = why;
        td.akey[td.dealt++] = artist;
    }
}
/* A fresh deck: its first cards. */
static void td_deal(void)
{
    td.n = TS_DECK;
    td.at = td.dealt = 0;
    td.kept = 0;
    td.step_card = td.turn_card = td.next0_card = -1;
    td_deal_to(TD_STACK);
}

static void td_finish(void);
static lv_color_t td_wash_for(int album);

/* Why card i was dealt, in a few words. */
static void td_why_text(int i, char *b, size_t n)
{
    int t = td.deck[i];
    const struct sl_album *al = &sl_lib.albums[sl_lib.tracks[t].album];
    switch (td.why[i])
    {
        case TM_WHY_ARTIST: snprintf(b, n, sl_tr("Because you liked %s"), sl_track_artist(t)); break;
        case TM_WHY_ALBUM:  snprintf(b, n, sl_tr("Because you liked %s"), al->title); break;
        case TM_WHY_GENRE:  snprintf(b, n, sl_tr("Because you like %s"), al->genre); break;
        case TM_WHY_DECADE: snprintf(b, n, sl_tr("More from the %ds"), al->year / 10 * 10); break;
        case TM_WHY_NEW:    strlcpy(b, sl_tr("New to you"), n); break;
        default:            strlcpy(b, sl_tr("Something different"), n); break;
    }
}
/* The real card for card i, hidden: its picture is what shows. */
static lv_obj_t *td_card_obj(int i, lv_obj_t **cover_out)
{
    int t = td.deck[i];
    const struct sl_track *tr = &sl_lib.tracks[t];
    lv_obj_t *card = box(td.page);
    lv_obj_set_size(card, TD_CARD_W, TD_CARD_H);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, TD_TOP);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_set_style_radius(card, sl_zune() ? 0 : SL_R_PILL, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *c = *cover_out = cover(card, tr->album, SL_ART_GRID, false);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t1 = text(card, tr->title, &ss_semibold_26, C_TEXT);
    lv_obj_set_width(t1, TD_CARD_W - 40);
    label_lines(t1, 1);
    lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 22 + SL_ART_GRID + 16);
    lv_obj_t *t2 = text(card, tr->artist ? tr->artist : sl_lib.albums[tr->album].artist, &ss_regular_18, C_SUB);
    lv_obj_set_width(t2, TD_CARD_W - 40);
    label_lines(t2, 1);
    lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(t2, t1, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    char why[SP_FIELD + 48];
    td_why_text(i, why, sizeof why);
    lv_obj_t *t3 = lv_label_create(card);
    lv_obj_set_style_base_dir(t3, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(t3, why);
    lv_obj_set_style_text_font(t3, &ss_regular_16, 0);
    lv_obj_set_style_text_color(t3, lv_color_hex(C_ACCENT_TXT), 0);
    lv_obj_set_width(t3, TD_CARD_W - 40);
    label_lines(t3, 1);
    lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(t3, t2, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    return card;
}

/* An object as RGB565 plus alpha, NULL without memory. */
static lv_draw_buf_t *td_snap(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    lv_draw_buf_t *argb = lv_snapshot_take(o, LV_COLOR_FORMAT_ARGB8888);
    lv_draw_buf_t *b = argb ? sl_rgb565a8_from(argb) : NULL;
    if (argb) lv_draw_buf_destroy(argb);
    return b;
}
static void td_free_buf(lv_draw_buf_t *b)
{
    if (!b) return;
    lv_image_cache_drop(b);
    lv_draw_buf_destroy(b);
}

static void td_touch_cb(lv_event_t *e);
static void td_unslot(struct td_slot *s)
{
    if (s->img && s->img != s->src) lv_obj_delete(s->img);
    if (s->src) lv_obj_delete(s->src);
    td_free_buf(s->buf);
    if (td.step_card == s->card) td.step_card = -1;
    if (td.next0_card == s->card) td.next0_card = -1;
    if (td.turn_card == s->card) td.turn_card = -1;
    memset(s, 0, sizeof *s);
}
/* What takes the touches and moves: the picture, or the real card. */
static void td_grabbable(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_USER_1);   /* no back-swipe across it */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(o, td_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(o, td_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(o, td_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(o, td_touch_cb, LV_EVENT_PRESS_LOST, NULL);
}
/* The slot's picture, made again when its cover has come. */
static void td_picture(struct td_slot *s)
{
    lv_draw_buf_t *b = td_snap(s->src);
    s->art = lv_obj_get_child_count(s->cover);
    if (!b)
    {
        if (!s->img) { s->img = s->src; td_grabbable(s->src); }
        return;
    }
    if (s->img == s->src)                /* memory again after all: the real card hides */
    {
        lv_obj_add_flag(s->src, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_event_cb(s->src, td_touch_cb);
        lv_obj_remove_flag(s->src, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_translate_x(s->src, 0, 0);
        lv_obj_set_style_translate_y(s->src, 0, 0);
        s->img = NULL;
    }
    if (!s->img)
    {
        s->img = lv_image_create(td.page);
        lv_obj_align(s->img, LV_ALIGN_TOP_MID, 0, TD_TOP);
        lv_image_set_pivot(s->img, TD_CARD_W / 2, TD_PIVOT);
        lv_obj_set_style_image_recolor(s->img, lv_color_hex(C_BG), 0);
        td_grabbable(s->img);
    }
    lv_image_set_src(s->img, b);
    td_free_buf(s->buf);
    s->buf = b;
    s->shown = b;
    if (td.step_card == s->card) td.step_card = -1;
    if (td.next0_card == s->card) td.next0_card = -1;
    if (td.turn_card == s->card) td.turn_card = -1;
}
static struct td_slot *td_slot_of(int i)
{
    struct td_slot *s = &td.slot[i % TD_STACK];
    return i >= 0 && i < td.n && s->src && s->card == i ? s : NULL;
}
/* Card i into its slot, now. */
static void td_slot_make(int i)
{
    struct td_slot *s = &td.slot[i % TD_STACK];
    td_unslot(s);
    td.pending &= ~(1u << (i % TD_STACK));
    if (i < 0 || i >= td.n) return;
    s->card = i;
    s->src = td_card_obj(i, &s->cover);
    s->wash = td_wash_for(sl_lib.tracks[td.deck[i]].album);
    td_picture(s);
}

/* The order on screen: the top card above the ones under it, the stamps
 * above all; only the top card takes a finger. */
static void td_restack(void)
{
    /* the order wanted, bottom first: moving anything in front redraws the
     * whole page (LVGL), so nothing moves when they are in it already */
    lv_obj_t *want[1 + TD_STACK + 3 + 3];
    int n = 0;
    if (td.ghost) want[n++] = td.ghost;
    for (int k = TD_STACK - 1; k >= 0; k--)
    {
        struct td_slot *s = td_slot_of(td.at + k);
        if (!s) continue;
        for (int j = 0; !k && j < 3; j++) if (td.shadow[j]) want[n++] = td.shadow[j];   /* just under the top card */
        want[n++] = s->img;
        if (k) lv_obj_remove_flag(s->img, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_add_flag(s->img, LV_OBJ_FLAG_CLICKABLE);
        if (s->img == s->src)            /* a real card can't shrink: only the top one shows */
        {
            if (k) lv_obj_add_flag(s->img, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(s->img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 3; i++) if (td.stamp[i]) want[n++] = td.stamp[i];
    int kids = (int)lv_obj_get_child_count(td.page), i = 0;
    while (i < n && kids - n + i >= 0 && lv_obj_get_child(td.page, kids - n + i) == want[i]) i++;
    if (i == n) return;
    for (int j = 0; j < n; j++) lv_obj_move_foreground(want[j]);
}

/* Restyling redraws even when the value is the same. */
static void td_set_tr(lv_obj_t *o, int x, int y)
{
    if (lv_obj_get_style_translate_x(o, 0) != x) lv_obj_set_style_translate_x(o, x, 0);
    if (lv_obj_get_style_translate_y(o, 0) != y) lv_obj_set_style_translate_y(o, y, 0);
}
static void td_set_img(lv_obj_t *o, int rot, int scale)
{
    if ((int)lv_image_get_rotation(o) != (rot + 3600) % 3600) lv_image_set_rotation(o, rot);
    if ((int)lv_image_get_scale(o) != scale) lv_image_set_scale(o, scale);
}

/* Which way the top card is going: 0 pass, 1 like, 3 love (up), -1 none;
 * and how far, of the way that rates it, in 256ths (more past the line). */
static int td_heading(int *e)
{
    int up = -td.y, ax = LV_ABS(td.x);
    bool vertical = up > ax;
    *e = 0;
    if (td.returning || (!td.x && up <= 0) || (vertical ? up : ax) <= 0) return -1;
    *e = vertical ? up * 256 / TD_UP : ax * 256 / TD_FLING;
    return vertical ? 3 : td.x > 0;
}
/* The buttons answer the drag: the one it heads for fills and grows, the
 * others give way. Each draws itself at its size (td_btn_draw), so growing
 * is a redraw of the button, where LVGL scaled it through a layer. */
static void td_buttons(void)
{
    int e, to = td_heading(&e);
    if (e > 256) e = 256;
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *b = td.btn[i];
        if (!b) continue;
        bool toward = to == i;
        int scale = i == 2 ? 256 : toward ? 256 + e * 36 / 256 : 256 - (to >= 0 ? e * 20 / 256 : 0);
        int sc = scale * td.bscale[i] / 256, fill = i == 2 || !toward ? 0 : e * 230 / 256;
        if (sc == td.bsc[i] && fill == td.bfill[i]) continue;
        td.bsc[i] = sc;
        td.bfill[i] = fill;
        lv_obj_invalidate(b);
    }
}

/* RGB565 plus alpha, scaled down into dst (its header set to the new
 * size), bilinear: the card under the top one at one step of its size. */
static void td_scale_into(const lv_draw_buf_t *src, lv_draw_buf_t *dst, int dw, int dh)
{
    static uint16_t xi[TD_CARD_W];
    static uint8_t xw[TD_CARD_W];
    int sw = src->header.w, sh = src->header.h, ss = src->header.stride / 2;
    const uint16_t *sc = (const uint16_t *)src->data;
    const uint8_t *sa = src->data + src->header.stride * sh;
    if (dw > TD_CARD_W) dw = TD_CARD_W;
    dst->header.w = dw; dst->header.h = dh; dst->header.stride = dw * 2;
    uint16_t *dc = (uint16_t *)dst->data;
    uint8_t *da = dst->data + dw * 2 * dh;
    int fx = ((sw - 1) << 16) / (dw > 1 ? dw - 1 : 1), fy = ((sh - 1) << 16) / (dh > 1 ? dh - 1 : 1);
    for (int x = 0; x < dw; x++) { xi[x] = (uint16_t)((x * fx) >> 16); xw[x] = (uint8_t)(((x * fx) >> 11) & 31); }
    /* each colour spread over a 32-bit word (g high, r and b low) so one
     * multiply mixes all three; weights in 32nds */
#define TD_SPREAD(c) (((uint32_t)(c) | (uint32_t)(c) << 16) & 0x07e0f81fu)
    for (int y = 0; y < dh; y++)
    {
        int yy = y * fy, y0 = yy >> 16, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        uint32_t wy = (yy >> 11) & 31;
        const uint16_t *r0 = sc + y0 * ss, *r1 = sc + y1 * ss;
        const uint8_t *a0 = sa + y0 * sw, *a1 = sa + y1 * sw;
        uint16_t *oc = dc + y * dw;
        uint8_t *oa = da + y * dw;
        for (int x = 0; x < dw; x++)
        {
            int x0 = xi[x], x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t wx = xw[x];
            uint32_t t = (TD_SPREAD(r0[x0]) * (32 - wx) + TD_SPREAD(r0[x1]) * wx) >> 5 & 0x07e0f81fu;
            uint32_t b = (TD_SPREAD(r1[x0]) * (32 - wx) + TD_SPREAD(r1[x1]) * wx) >> 5 & 0x07e0f81fu;
            uint32_t m = (t * (32 - wy) + b * wy) >> 5 & 0x07e0f81fu;
            oc[x] = (uint16_t)(m | m >> 16);
            uint32_t at = a0[x0] * (32 - wx) + a0[x1] * wx, ab = a1[x0] * (32 - wx) + a1[x1] * wx;
            oa[x] = (uint8_t)((at * (32 - wy) + ab * wy) >> 10);
        }
    }
#undef TD_SPREAD
}
/* The slot's image showing picture b, its pixels new or not. */
static void td_src(struct td_slot *s, lv_draw_buf_t *b, bool changed)
{
    if (s->shown == b && !changed) return;
    if (changed) lv_image_cache_drop(b);
    lv_image_set_src(s->img, b);
    s->shown = b;
}
/* The card under the top one at scale sc: its own picture at full size,
 * else the biggest step made that is no bigger than sc. The size the
 * picture shows at, or 0 when no step is made yet (LVGL scales it then). */
static int td_show(struct td_slot *s, int sc)
{
    if (sc >= 256) { td_src(s, s->buf, false); return 256; }
    int j = (sc - 240) / 4;
    if (j >= TD_STEPS) j = TD_STEPS - 1;
    while (j >= 0 && !(td.step_card == s->card && (td.step_ok & (1u << j)))) j--;
    if (j < 0) { td_src(s, s->buf, false); return 0; }
    td_src(s, td.step[j], false);
    return 240 + 4 * j;
}
/* One more step of the card under, while nothing moves. */
static bool td_step_make(void)
{
    struct td_slot *s = td_slot_of(td.at + 1);
    if (!s || !s->buf || s->img == s->src) return false;
    if (td.step_card != s->card) { td.step_card = s->card; td.step_ok = 0; }
    for (int j = 0; j < TD_STEPS; j++)
    {
        if ((td.step_ok & (1u << j)) || !td.step[j]) continue;
        int q = 240 + 4 * j;
        td_scale_into(s->buf, td.step[j], (TD_CARD_W * q + 128) / 256, (TD_CARD_H * q + 128) / 256);
        lv_image_cache_drop(td.step[j]);
        td.step_ok |= 1u << j;
        if (s->shown == td.step[j]) { s->shown = NULL; }
        return true;
    }
    struct td_slot *n = td_slot_of(td.at + 2);   /* then the card after it, at the first step */
    if (n && n->buf && n->img != n->src && td.next0 && td.next0_card != n->card)
    {
        td_scale_into(n->buf, td.next0, (TD_CARD_W * 240 + 128) / 256, (TD_CARD_H * 240 + 128) / 256);
        lv_image_cache_drop(td.next0);
        td.next0_card = n->card;
        return true;
    }
    return false;
}
/* The card after the one under has come up under: its first step is ready. */
static void td_step_next(void)
{
    if (td.next0_card != td.at + 1 || !td.next0) return;
    lv_draw_buf_t *b = td.step[0];
    td.step[0] = td.next0;
    td.next0 = b;
    td.step_card = td.at + 1;
    td.step_ok = 1;
    td.next0_card = -1;
}

#define TD_TURN_W 520                    /* room for the card turned 25 degrees and lifted */
#define TD_TURN_H 544
/* Narrow [*a, *b) to the x where lo <= (u0 + du * x) >> 16 < hi. */
static void td_span(int32_t u0, int32_t du, int lo, int hi, int *a, int *b)
{
    int64_t l = (int64_t)lo << 16, h = (int64_t)hi << 16;   /* lo <= u < hi, in 16.16 */
    int x0 = *a, x1 = *b;
    if (du == 0) { if (u0 < l || u0 >= h) *b = *a; return; }
    /* a guess from the line, then exact by stepping */
    double g0 = ((du > 0 ? l : h - 1) - (double)u0) / du, g1 = ((du > 0 ? h - 1 : l) - (double)u0) / du;
    int s0 = g0 > x0 ? (g0 < x1 ? (int)ceil(g0) : x1) : x0, s1 = g1 < x1 - 1 ? (g1 > x0 - 1 ? (int)floor(g1) + 1 : x0) : x1;
#define TD_IN(x) ((int64_t)u0 + (int64_t)du * (x) >= l && (int64_t)u0 + (int64_t)du * (x) < h)
    while (s0 > x0 && TD_IN(s0 - 1)) s0--;
    while (s0 < s1 && !TD_IN(s0)) s0++;
    while (s1 < x1 && TD_IN(s1)) s1++;
    while (s1 > s0 && !TD_IN(s1 - 1)) s1--;
#undef TD_IN
    *a = s0; *b = s1 > s0 ? s1 : s0;
}
#define TD_SPREAD(p) (((uint32_t)(p) | (uint32_t)(p) << 16) & 0x07e0f81fu)
/* Flying off, only what is still on the screen is turned (from the point it
 * turns round): it only goes further off, so what is left out never shows. */
static bool td_keep_on;
static int td_keep_x0, td_keep_x1, td_keep_y0, td_keep_y1;
/* One pixel of the card, bilinear, at 16.16 (u, v) wholly inside it. */
static inline void td_bilerp(const uint16_t *sp, const uint8_t *sa, int ss, int sw, int32_t u, int32_t v,
                             uint16_t *oc, uint8_t *oa)
{
    int iu = u >> 16, iv = v >> 16;
    uint32_t wx = (u >> 11) & 31, wy = (v >> 11) & 31;
    const uint16_t *p0 = sp + iv * ss + iu, *p1 = p0 + ss;
    const uint8_t *q = sa + iv * sw + iu;
    uint32_t t = (TD_SPREAD(p0[0]) * (32 - wx) + TD_SPREAD(p0[1]) * wx) >> 5 & 0x07e0f81fu;
    uint32_t bb = (TD_SPREAD(p1[0]) * (32 - wx) + TD_SPREAD(p1[1]) * wx) >> 5 & 0x07e0f81fu;
    uint32_t m = (t * (32 - wy) + bb * wy) >> 5 & 0x07e0f81fu;
    *oc = (uint16_t)(m | m >> 16);
    if ((q[0] & q[1] & q[sw] & q[sw + 1]) == 255) *oa = 255;   /* most of the card: solid */
    else *oa = (uint8_t)(((q[0] * (32 - wx) + q[1] * wx) * (32 - wy) + (q[sw] * (32 - wx) + q[sw + 1] * wx) * wy) >> 10);
}
/* A picture turned by rot (0.1 degree) and scaled by sc round (px, py),
 * into dst; *oxp, *oyp: where dst's top left lands, from that point. fine:
 * bilinear throughout. Not fine (while it moves): the nearest pixel inside,
 * still bilinear at its edges and round corners, so the outline stays
 * smooth and only the picture's insides are coarser while it moves; about
 * a third of the work. */
static void td_turn_into(const lv_draw_buf_t *src, lv_draw_buf_t *dst, int rot, int sc, bool fine,
                         float px, float py, int *oxp, int *oyp, int y_from, int y_to)
{
    int sw = src->header.w, sh = src->header.h, ss = src->header.stride / 2;
    const uint16_t *sp = (const uint16_t *)src->data;
    const uint8_t *sa = src->data + src->header.stride * sh;
    float an = rot * 3.14159265f / 1800, c = cosf(an), sn = sinf(an), k = sc / 256.f;
    float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
    for (int i = 0; i < 4; i++)          /* the corners, turned: the box to fill */
    {
        float cx = (i & 1 ? sw : 0) - px, cy = (i & 2 ? sh : 0) - py;
        float tx = k * (cx * c - cy * sn), ty = k * (cx * sn + cy * c);
        if (tx < x1) x1 = tx;
        if (tx > x2) x2 = tx;
        if (ty < y1) y1 = ty;
        if (ty > y2) y2 = ty;
    }
    int ox = (int)floorf(x1) - 1, oy = (int)floorf(y1) - 1;
    int dw = (int)ceilf(x2) + 1 - ox, dh = (int)ceilf(y2) + 1 - oy;
    if (dw > TD_TURN_W) dw = TD_TURN_W;
    if ((uint32_t)(dw * dh * 3) > dst->data_size) dh = (int)(dst->data_size / 3 / (uint32_t)dw);
    dst->header.w = dw; dst->header.h = dh; dst->header.stride = dw * 2;
    uint16_t *dc = (uint16_t *)dst->data;
    uint8_t *da = dst->data + dw * 2 * dh;
    /* each destination pixel's centre, back through the turn: 16.16 steps */
    int32_t du = (int32_t)(c / k * 65536), dv = (int32_t)(-sn / k * 65536);
    for (int y = y_from; y < dh && y < y_to; y++)   /* rows y_from to y_to only: the copy made fine a band at a time */
    {
        float fx = ox + 0.5f, fy = oy + y + 0.5f;
        int32_t u0 = (int32_t)(((fx * c + fy * sn) / k + px - 0.5f) * 65536);
        int32_t v0 = (int32_t)(((-fx * sn + fy * c) / k + py - 0.5f) * 65536);
        uint16_t *oc = dc + y * dw;
        uint8_t *oa = da + y * dw;
        /* where on this row the card is at all, and where wholly inside */
        int a = 0, b = dw, ia, ib;
        if (td_keep_on)
        {
            if (oy + y < td_keep_y0 || oy + y >= td_keep_y1) a = b = 0;
            if (a < td_keep_x0 - ox) a = td_keep_x0 - ox;
            if (b > td_keep_x1 - ox) b = td_keep_x1 - ox;
            if (a > dw) a = dw;
            if (b < a) b = a;
        }
        td_span(u0, du, -1, sw, &a, &b);
        td_span(v0, dv, -1, sh, &a, &b);
        if (a > b) a = b;
        ia = a; ib = b;
        td_span(u0, du, 0, sw - 1, &ia, &ib);
        td_span(v0, dv, 0, sh - 1, &ia, &ib);
        if (ia > ib) ia = ib = a;
        memset(oc, 0, (size_t)a * 2); memset(oa, 0, (size_t)a);
        memset(oc + b, 0, (size_t)(dw - b) * 2); memset(oa + b, 0, (size_t)(dw - b));
        int liv = -1;                    /* the source row the nearest pixels come from, kept while it lasts */
        const uint16_t *crow = sp;
        const uint8_t *arow = sa;
        bool solid = false;
        for (int x = a; x < b; x++)
        {
            int32_t u = u0 + du * x, v = v0 + dv * x;
            if (x >= ia && x < ib)
            {
                if (fine) { td_bilerp(sp, sa, ss, sw, u, v, oc + x, oa + x); continue; }
                /* the nearest pixel, unless it is at a round corner (a row
                 * solid at both ends is solid all along) */
                int iv = (v + 0x8000) >> 16, iu = (u + 0x8000) >> 16;
                if (iv != liv)
                {
                    liv = iv; crow = sp + iv * ss; arow = sa + iv * sw;
                    solid = (arow[0] & arow[sw - 1]) == 255;
                }
                if (solid || arow[iu] == 255) { oc[x] = crow[iu]; oa[x] = 255; }
                else td_bilerp(sp, sa, ss, sw, u, v, oc + x, oa + x);
                continue;
            }
            /* the edge: outside the card is clear, so it fades over a pixel */
            int iu = u >> 16, iv = v >> 16;
            uint32_t wx = (u >> 11) & 31, wy = (v >> 11) & 31, a00, a01, a10, a11;
            int u_0 = iu < 0 ? 0 : iu, u_1 = iu + 1 < sw ? iu + 1 : sw - 1;
            int v_0 = iv < 0 ? 0 : iv, v_1 = iv + 1 < sh ? iv + 1 : sh - 1;
            a00 = iu >= 0 && iv >= 0 ? sa[v_0 * sw + u_0] : 0; a01 = iu + 1 < sw && iv >= 0 ? sa[v_0 * sw + u_1] : 0;
            a10 = iu >= 0 && iv + 1 < sh ? sa[v_1 * sw + u_0] : 0; a11 = iu + 1 < sw && iv + 1 < sh ? sa[v_1 * sw + u_1] : 0;
            uint32_t t = (TD_SPREAD(sp[v_0 * ss + u_0]) * (32 - wx) + TD_SPREAD(sp[v_0 * ss + u_1]) * wx) >> 5 & 0x07e0f81fu;
            uint32_t bb = (TD_SPREAD(sp[v_1 * ss + u_0]) * (32 - wx) + TD_SPREAD(sp[v_1 * ss + u_1]) * wx) >> 5 & 0x07e0f81fu;
            uint32_t m = (t * (32 - wy) + bb * wy) >> 5 & 0x07e0f81fu;
            oc[x] = (uint16_t)(m | m >> 16);
            oa[x] = (uint8_t)(((a00 * (32 - wx) + a01 * wx) * (32 - wy) + (a10 * (32 - wx) + a11 * wx) * wy) >> 10);
        }
    }
    *oxp = ox;
    *oyp = oy;
}
#undef TD_SPREAD
/* The top card at rot and sc: its own picture when straight, else the
 * turned copy, made again only when the turn moves on a step (half a
 * degree): between steps the card only moves, a plain copy. The copy is the
 * quick kind while it moves and made fine once it is held still. */
static bool td_show_top(struct td_slot *s, int rot, int sc)
{
    if ((!rot && sc == 256) || !td.turn)
    {
        td_src(s, s->buf, false);
        td_set_img(s->img, rot, sc);
        td_set_tr(s->img, td.x, td.y);
        td.back_row = -1;
        return false;
    }
    bool want = td.turn_card != s->card || td.turn_rot != rot || td.turn_scale != sc, changed = false;
    td_keep_on = td.flying;
    td_keep_x0 = -(W / 2 + td.x); td_keep_x1 = W - (W / 2 + td.x);
    td_keep_y0 = -(TD_TOP + TD_PIVOT + td.y); td_keep_y1 = H - (TD_TOP + TD_PIVOT + td.y);
    if (!want) td.back_row = -1;         /* back where the copy showing is: nothing to make */
    else if (td.turn_card != s->card || s->shown != td.turn || !td.turn_back)
    {
        /* nothing turned of this card showing yet: made whole, now */
        int ox, oy;
        td_turn_into(s->buf, td.turn, rot, sc, false, TD_CARD_W / 2, TD_PIVOT, &ox, &oy, 0, TD_TURN_H);
        td.turn_ox = ox + TD_CARD_W / 2;     /* from the card's top left */
        td.turn_oy = oy + TD_PIVOT;
        td.turn_card = s->card; td.turn_rot = rot; td.turn_scale = sc;
        td.turn_fine = false;
        td.refine_row = 0;
        td.back_row = -1;
        changed = true;
    }
    else if (td.back_row < 0)
    {
        /* the next step: its top half this frame, the last one showing a frame more */
        int ox, oy;
        td.back_card = s->card; td.back_rot = rot; td.back_scale = sc;
        td_turn_into(s->buf, td.turn_back, rot, sc, false, TD_CARD_W / 2, TD_PIVOT, &ox, &oy, 0, 0);
        td.back_row = (td.turn_back->header.h + 1) / 2;
        td_turn_into(s->buf, td.turn_back, rot, sc, false, TD_CARD_W / 2, TD_PIVOT, &ox, &oy, 0, td.back_row);
    }
    else
    {
        /* its bottom half, and it shows */
        int ox, oy;
        td_turn_into(s->buf, td.turn_back, td.back_rot, td.back_scale, false, TD_CARD_W / 2, TD_PIVOT, &ox, &oy, td.back_row, TD_TURN_H);
        lv_draw_buf_t *t = td.turn;
        td.turn = td.turn_back;
        td.turn_back = t;
        td.turn_ox = ox + TD_CARD_W / 2;
        td.turn_oy = oy + TD_PIVOT;
        td.turn_card = td.back_card; td.turn_rot = td.back_rot; td.turn_scale = td.back_scale;
        td.turn_fine = false;
        td.refine_row = 0;
        td.back_row = -1;
        changed = true;
    }
    td_keep_on = false;
    td_src(s, td.turn, changed);
    td_set_img(s->img, 0, 256);
    td_set_tr(s->img, (W - TD_CARD_W) / 2 + td.turn_ox - (W / 2 - (int)td.turn->header.w / 2) + td.x, td.turn_oy + td.y);
    return changed;
}
static void td_show_flag(lv_obj_t *o, bool on)
{
    if (on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
}

/* Everything from where the top card is: the stack under it, the stamp,
 * the buttons. */
static void td_layout(void)
{
    static const int scale[TD_STACK] = { 256, 240, 224 };
    static const int dim[TD_STACK] = { 0, 80, 150 };
    /* the card under comes up as the top one goes, any way it goes */
    int ax = LV_ABS(td.x), go = ax > -td.y ? ax : -td.y, p = go >= TD_RISE ? 256 : go * 256 / TD_RISE;
    int rot = td.x * (td.rot_sign ? td.rot_sign : 1) / 2, lift = td.lift > 0 ? td.lift & ~1 : 0, top_scale = 256 + lift;
    /* lifted: its shadow under it, while it is not turned much */
    int sh = sl_zune() || td.flying || LV_ABS(rot) > 80 ? 0 : lift * TD_SH_OPA / TD_LIFT;
    if (rot > 250) rot = 250;
    if (rot < -250) rot = -250;
    /* steps of half a degree; a degree and a half flying off, too fast to see steps */
    if (td.turn) { int st = td.flying ? 15 : 5; rot = (rot >= 0 ? rot + st / 2 : rot - st / 2) / st * st; }
    for (int k = 0; k < TD_STACK; k++)
    {
        /* depth k, rising to k - 1 as the top card goes; scaled round the
         * point under the card, so the strip below stays TD_PEEK a card */
        int sc = k ? scale[k] + (scale[k - 1] - scale[k]) * p / 256 : 256;
        int ty = k ? (k * TD_PEEK + (TD_PIVOT - TD_CARD_H) * (256 - scale[k]) / 256) * (256 - p) / 256 +
                     ((k - 1) * TD_PEEK + (TD_PIVOT - TD_CARD_H) * (256 - scale[k - 1]) / 256) * p / 256 : 0;
        int d = k ? dim[k] + (dim[k - 1] - dim[k]) * p / 256 : 0;
        struct td_slot *s = td_slot_of(td.at + k);
        if (k == 2 && td.ghost)          /* the bottom card: a plain card shape */
        {
            bool on = td.at + 2 < td.n;
            td_show_flag(td.ghost, on);
            if (s && s->img != s->src) td_show_flag(s->img, false);
            if (!on) continue;
            /* only its foot: the card under always hides the rest, and
             * filling the whole card each frame cost as much as a picture */
            int w = (TD_CARD_W * sc + 128) / 256, h = (TD_CARD_H * sc + 128) / 256;
            int hg = h < TD_PEEK + SL_R_PILL + 16 ? h : TD_PEEK + SL_R_PILL + 16;
            int y = TD_TOP + ty + TD_PIVOT * (256 - sc) / 256 + h - hg;
            h = hg;
            if (lv_obj_get_width(td.ghost) != w || lv_obj_get_height(td.ghost) != h)
            {
                lv_obj_set_size(td.ghost, w, h);
                if (!sl_zune()) lv_obj_set_style_radius(td.ghost, SL_R_PILL * sc / 256, 0);
            }
            if (lv_obj_get_style_y(td.ghost, 0) != y) lv_obj_align(td.ghost, LV_ALIGN_TOP_MID, 0, y);
            int de = d + (255 - d) * (256 - td.ghost_in) / 256;
            lv_color_t c = lv_color_mix(lv_color_hex(C_BG), lv_color_hex(C_CARD), (uint8_t)(de > 255 ? 255 : de));
            if (!lv_color_eq(lv_obj_get_style_bg_color(td.ghost, 0), c)) lv_obj_set_style_bg_color(td.ghost, c, 0);
            continue;
        }
        if (!s) continue;
        if (s->img == s->src) { if (!k) td_set_tr(s->img, td.x, td.y); continue; }
        td_show_flag(s->img, true);
        if (!k)
        {
            td_show_top(s, rot, top_scale);
            for (int j = 0; j < 3; j++)
            {
                lv_obj_t *o = td.shadow[j];
                if (!o) continue;
                td_show_flag(o, sh > 0 && s->img != s->src);
                if (!sh) continue;
                lv_opa_t op = (lv_opa_t)(sh * 255 / TD_SH_OPA);
                if (lv_obj_get_style_image_opa(o, 0) != op) lv_obj_set_style_image_opa(o, op, 0);
                td_set_tr(o, (W - TD_CARD_W) / 2 + td.x + (j == 1 ? TD_CARD_W - TD_SH_BLUR : j ? TD_SH_BLUR : -TD_SH_BLUR),
                          TD_TOP + td.y + (j == 2 ? TD_CARD_H + TD_SH_DY - TD_SH_BLUR : TD_SH_DY - TD_SH_BLUR));
            }
        }
        else
        {
            /* a copy made at a step of size, drawn as it is; LVGL scales it
             * live only until the steps are made */
            int q = td_show(s, sc);
            td_set_img(s->img, 0, q ? 256 : sc);
            td_set_tr(s->img, 0, q ? ty + TD_PIVOT * (256 - q) / 256 : ty);
        }
        if (lv_obj_get_style_image_recolor_opa(s->img, 0) != d) lv_obj_set_style_image_recolor_opa(s->img, (lv_opa_t)d, 0);
    }
    /* the stamp: in from 20 px, pressed down onto the card as it comes,
     * turned with it: PASS and LIKE on its top corners, LOVE low in the middle */
    int e, to = td_heading(&e), dist = to == 3 ? -td.y : ax, line = to == 3 ? TD_UP : TD_FLING;
    int f = to < 0 ? 0 : (dist - 20) * 256 / (line - 20);
    if (f < 0) f = 0;
    if (f > 256) f = 256;
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *st = td.stamp[i];
        if (!st) continue;
        bool on = f && to == (i == 2 ? 3 : i) && td_slot_of(td.at) && td_slot_of(td.at)->img != td_slot_of(td.at)->src;
        if (!on) { if (!lv_obj_has_flag(st, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(st, LV_OBJ_FLAG_HIDDEN); continue; }
        int sw = td.stamp_buf[i]->header.w, sht = td.stamp_buf[i]->header.h;
        float lx = i == 2 ? TD_CARD_W / 2 : i ? 30 + sw / 2 : TD_CARD_W - 30 - sw / 2;
        float ly = i == 2 ? 22 + SL_ART_GRID - 34 - sht / 2 : 34 + sht / 2;
        float a = rot * 3.14159265f / 1800, c = cosf(a), sn = sinf(a), k = top_scale / 256.f;
        float vx = (lx - TD_CARD_W / 2) * k, vy = (ly - TD_PIVOT) * k;
        int sx = (W - TD_CARD_W) / 2 + td.x + TD_CARD_W / 2 + (int)lroundf(vx * c - vy * sn);
        int sy = TD_TOP + td.y + TD_PIVOT + (int)lroundf(vx * sn + vy * c);
        int srot = rot + (i == 2 ? -80 : i ? -140 : 140), ssc = ((256 + (256 - f) * 3 / 8) * top_scale / 256 + 4) & ~7;
        if (td.stamp_turn)
        {
            /* turned by us, at steps of half a degree and 1/32 of size: LVGL
             * turning it every frame cost more than the card's own copy */
            int key = i << 24 | (srot + 3600) << 10 | ssc;
            if (key != td.stamp_key)
            {
                td_turn_into(td.stamp_buf[i], td.stamp_turn, srot, ssc, true, sw / 2, sht / 2, &td.stamp_ox, &td.stamp_oy, 0, TD_TURN_H);
                lv_image_cache_drop(td.stamp_turn);
                lv_image_set_src(st, td.stamp_turn);
                td.stamp_key = key;
            }
            else if (lv_image_get_src(st) != td.stamp_turn) lv_image_set_src(st, td.stamp_turn);
            td_set_img(st, 0, 256);
            td_set_tr(st, sx + td.stamp_ox, sy + td.stamp_oy);
        }
        else
        {
            td_set_img(st, srot, ssc);
            td_set_tr(st, sx - sw / 2, sy - sht / 2);
        }
        lv_opa_t o = (lv_opa_t)(f > 255 ? 255 : f);
        if (lv_obj_get_style_image_opa(st, 0) != o) lv_obj_set_style_image_opa(st, o, 0);
        if (lv_obj_has_flag(st, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(st, LV_OBJ_FLAG_HIDDEN);
    }
    td_buttons();
}

/* "3 of 15", and undo while there is something to take back */
static void td_count(void)
{
    char b[32];
    if (td.at < td.n) snprintf(b, sizeof b, sl_tr("%d of %d"), td.at + 1, td.n);
    else b[0] = '\0';
    label_if_changed(td.count, b);
    if (td.btn[2]) lv_obj_set_style_opa(td.btn[2], td.at > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
}

/* A spring letting go: past the end by about 7 % once, then still. */
static int32_t td_spring_path(const lv_anim_t *a)
{
    double t = a->duration ? (double)a->act_time / a->duration : 1;   /* double: a float t lets GCC narrow exp to expf */
    float f = t >= 1 ? 1 : 1 - (float)(exp(-9 * t) * (cos(12 * t) + 0.75 * sin(12 * t)));   /* not expf: newer than the R1's glibc */
    return a->start_value + (int32_t)lroundf((a->end_value - a->start_value) * f);
}
static void td_motion(void *var, int32_t v)
{
    (void)var;
    td.x = td.fx + (td.tx - td.fx) * v / 1024;
    td.y = td.fy + (td.ty - td.fy) * v / 1024;
    td.lift = td.flift - td.flift * v / 1024;
    td_layout();
}
static void td_move(int x, int y, int ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&td, NULL);
    lv_anim_delete(&td.lift, NULL);
    td.fx = td.x; td.fy = td.y; td.flift = td.lift;
    td.tx = x; td.ty = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &td);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_exec_cb(&a, td_motion);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void td_lift_exec(void *var, int32_t v) { (void)var; td.lift = v; td_layout(); }

static void td_ghost_exec(void *var, int32_t v) { (void)var; td.ghost_in = v; td_layout(); }
/* a new card at the bottom of the stack: it darkens in */
static void td_ghost_arrive(void)
{
    td.ghost_in = 0;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &td.ghost_in);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, td_ghost_exec);
    lv_anim_start(&a);
}

/* The page in the top card's cover colour, as an album's page is washed
 * in its own (not on Zule, where words sit on black). */
static lv_color_t td_wash_for(int album)
{
    int hue = sl_lib.albums[album].hue, sat = 100;
    if (!sl_art_hue(album, &hue, &sat)) hue = sl_lib.albums[album].hue;
    return sl_theme() == 1 ? lv_color_hsv_to_rgb(hue, 4 + sat * 26 / 100, 94)
                           : lv_color_hsv_to_rgb(hue, 6 + sat * 52 / 100, sl_theme() == 2 ? 24 : 36);
}
/* The wash: solid to TD_WASH_TOP, then fading to the page's own colour by
 * TD_WASH_END (an album page's stops, 36 and 180 of 255), as a picture
 * filled once a change: LVGL worked the gradient out again every frame. */
#define TD_WASH_TOP (H * 36 / 255)
#define TD_WASH_END (H * 180 / 255)
static void td_wash_fill(lv_color_t c)
{
    td.wash_cur = c;
    if (!td.wash_buf) return;
    lv_color_t bg = lv_color_hex(C_BG);
    for (int y = 0; y < TD_WASH_END; y++)
    {
        int f = y < TD_WASH_TOP ? 255 : 255 - (y - TD_WASH_TOP) * 255 / (TD_WASH_END - TD_WASH_TOP);
        uint16_t px = lv_color_to_u16(lv_color_mix(c, bg, (uint8_t)f));
        uint16_t *row = (uint16_t *)(td.wash_buf->data + y * td.wash_buf->header.stride);
        if (y && px == ((uint16_t *)(td.wash_buf->data + (y - 1) * td.wash_buf->header.stride))[0])
            memcpy(row, row - td.wash_buf->header.stride / 2, W * 2);
        else for (int x = 0; x < W; x++) row[x] = px;
    }
    lv_image_cache_drop(td.wash_buf);
    if (!td.wash_img) return;
    struct td_slot *s = td_slot_of(td.at);
    if (!s || td.x || td.y || td.lift || s->img == s->src) { lv_obj_invalidate(td.wash_img); return; }
    /* the top card at rest hides the wash but at its corners: only round it
     * is drawn again, not the cards under it */
    int x0 = (W - TD_CARD_W) / 2, x1 = x0 + TD_CARD_W, y0 = TD_TOP, y1 = TD_TOP + TD_CARD_H, r = SL_R_PILL;
    const lv_area_t part[] = {
        { 0, 0, W - 1, y0 - 1 }, { 0, y0, x0 - 1, TD_WASH_END - 1 }, { x1, y0, W - 1, TD_WASH_END - 1 },
        { x0, y1 - r, x1 - 1, TD_WASH_END - 1 }, { x0, y0, x0 + r - 1, y0 + r - 1 }, { x1 - r, y0, x1 - 1, y0 + r - 1 } };
    for (unsigned i = 0; i < sizeof part / sizeof part[0]; i++)
        if (part[i].y2 >= part[i].y1) lv_obj_invalidate_area(td.wash_img, &part[i]);
}
static void td_wash_exec(void *var, int32_t v)
{
    (void)var;
    lv_color_t c = lv_color_mix(td.wash_to, td.wash_from, (uint8_t)(v >= 255 ? 255 : v));
    if (!lv_color_eq(td.wash_cur, c)) td_wash_fill(c);
}
/* The wash to a new colour: at once, or in TD_WASH_STEPS steps, one a tick
 * once the cards are still (td_tick), so it never shares a frame with a
 * card moving and pauses under a finger. */
#define TD_WASH_STEPS 4
static void td_wash(lv_color_t to, bool anim)
{
    if (sl_zune() || !td.page) return;
    td.wash_from = td.wash_cur;
    td.wash_to = to;
    td.wash_step = anim ? 0 : TD_WASH_STEPS;
    if (!anim) td_wash_exec(NULL, 255);
}
static void td_wash_top(bool anim)
{
    struct td_slot *s = td_slot_of(td.at);
    if (s) td_wash(s->wash, anim);
}
/* The snippet fades out as its card goes; the next one starts from cold,
 * at full volume. */
static void td_clip_fade(void)
{
    if (quiz_clip_active() && quiz.clip.audible && !quiz.clip.hushed) pcmbuf_fade(true, false);
}

/* Is the model up to date without being worked out again? */
static bool tm_fresh(void)
{
    return tm.ok && tm.gen == ts.gen && tm.lib == sl_lib.tracks && tm.lib_n == sl_lib.n_tracks && tm.al_n == sl_lib.n_albums;
}
/* Room for more swipes: the oldest passes go first, so Loved keeps its
 * songs (and then the oldest of any). The model is worked out again. */
static void ts_make_room(void)
{
    int drop = 50, k = 0;
    for (int i = 0; i < ts.n_rated; i++)
    {
        bool go = drop && ts.rated[i][0] == '-';
        if (go) drop--;
        else if (k != i) memcpy(ts.rated[k++], ts.rated[i], MAX_PATH);
        else k++;
    }
    if (drop) { memmove(ts.rated, ts.rated[drop], (size_t)(k - drop) * sizeof ts.rated[0]); k -= drop; }
    ts.n_rated = k;
    ts.on_card = -1;
    ts.gen++;
}
/* A swipe kept, way '+', '*' or '-': in taste.txt's list and the model. */
static bool ts_swipe(int t, char way)
{
    if (!ts.rated) return false;
    if (ts.n_rated >= TS_RATED_MAX) ts_make_room();
    bool fresh = tm_fresh();
    char p[MAX_PATH];
    snprintf(ts.rated[ts.n_rated++], MAX_PATH, "%c%s", way, sl_track_path(t, p, sizeof p));
    ts.gen++;
    if (fresh) { tm_swipe(t, way, false); tm.gen = ts.gen; }
    return true;
}
/* The newest swipe taken back, when it is track t's. */
static bool ts_unswipe(int t)
{
    if (!ts.rated || !ts.n_rated || !sl_track_path_is(t, ts.rated[ts.n_rated - 1] + 1)) return false;
    bool fresh = tm_fresh();
    char way = ts.rated[--ts.n_rated][0];
    if (ts.on_card > ts.n_rated) ts.on_card = -1;   /* it was on the card: taken out by writing it all again */
    ts.gen++;
    if (fresh) { tm_swipe(t, way, true); tm.gen = ts.gen; }
    return true;
}

/* The card thrown is gone: rate it, and the one under it is on top. */
static void td_rate(char way)
{
    int i = td.at, t = td.deck[i];
    td.way[i] = way;
    if (ts_swipe(t, way)) td.kept |= 1u << i;
    if (way != '-') td.liked++;
    struct td_slot *gone = td_slot_of(i);
    if (gone) td_unslot(gone);
    td.at++;
    td_step_next();
    td.x = td.y = td.lift = 0;
    td.rot_sign = 1;
    td.past = false;
    if (td.at >= td.dealt) td_deal_to(td.at + 2);   /* only when nothing was ready */
    if (td.at >= td.n) { td_finish(); return; }
    /* the top two now; the one coming up under them is picked, knowing this
     * swipe, and pictured when the screen is still */
    for (int k = 0; k < 2; k++) if (!td_slot_of(td.at + k) && td.at + k < td.dealt) td_slot_make(td.at + k);
    if (td.at + 2 < td.dealt) td.pending |= 1u << ((td.at + 2) % TD_STACK);
    td_ghost_arrive();
    td_restack();
    td_layout();
    td_count();
    td_wash_top(true);
    clip_swap(td.deck[td.at]);
}
static char td_threw;
static void td_flown(lv_anim_t *a)
{
    (void)a;
    td.flying = false;
    td_rate(td_threw);
}
/* Off the screen the way it was going, faster for a faster flick: to the
 * side for a like or a pass, up for a love. A button's throw starts from
 * still, so it gathers speed first. */
static void td_fly(char way, int vx, int vy)
{
    if (td.flying || td.returning || !td_slot_of(td.at)) return;
    td.flying = true;
    td.held = false;
    td_threw = way;
    if (way == '*') sfx_play_ex(SFX_UI_ON, 150, 100);   /* the like's chime, higher */
    else sfx_play(way == '+' ? SFX_UI_ON : SFX_UI_OFF);
    td_clip_fade();
    int x1, y1;
    int speed = (int)sqrtf((float)vx * vx + (float)vy * vy);
    bool button = !td.x && !td.y && !speed;
    if (way == '*')
    {
        y1 = -(TD_TOP + TD_CARD_H + 40);
        if (speed > 300 && vy < 0) x1 = td.x + (int)((long)(y1 - td.y) * vx / vy);
        else if (td.y < 0) x1 = td.x + (int)((long)(y1 - td.y) * td.x / td.y);
        else x1 = 0;
        if (x1 > 320) x1 = 320;
        if (x1 < -320) x1 = -320;
    }
    else
    {
        x1 = way == '+' ? TD_OFF : -TD_OFF;
        if (speed > 300 && vx) y1 = td.y + (int)((long)(x1 - td.x) * vy / vx);
        else if (td.x) y1 = td.y + (int)((long)(x1 - td.x) * td.y / td.x);
        else y1 = -40;                   /* a button: up a little as it goes */
        if (y1 > 320) y1 = 320;
        if (y1 < -320) y1 = -320;
    }
    int dist = (int)sqrtf((float)(x1 - td.x) * (x1 - td.x) + (float)(y1 - td.y) * (y1 - td.y));
    int ms = button ? 340 : dist * 1700 / (speed > 1500 ? speed : 1500);
    if (ms < 170) ms = 170;
    if (ms > 340) ms = 340;
    td_move(x1, y1, ms, button ? lv_anim_path_ease_in_out : lv_anim_path_ease_out, td_flown);
}
static void td_throw(bool like, int vx, int vy) { td_fly(like ? '+' : '-', vx, vy); }
static void td_spring(void)
{
    td_move(0, 0, 560, td_spring_path, NULL);
}
static bool td_past_line(void)
{
    int e, to = td_heading(&e);
    return to >= 0 && e >= 256;
}

static void td_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (td.flying || td.returning) return;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (code == LV_EVENT_PRESSED)
    {
        /* caught where it is, even springing back */
        lv_anim_delete(&td, NULL);
        td.down.x = pt.x - td.x;
        td.down.y = pt.y - td.y;
        td.held = true;
        td.dragging = td.x || td.y;
        /* held low, it turns the other way, as a card does held by its foot */
        if (!td.dragging) td.rot_sign = pt.y - TD_TOP > TD_CARD_H * 2 / 3 ? -1 : 1;
        td.past = td_past_line();
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &td.lift);
        lv_anim_set_values(&a, td.lift, TD_LIFT);
        lv_anim_set_duration(&a, SL_T_FAST);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, td_lift_exec);
        lv_anim_start(&a);
        return;
    }
    if (!td.held) return;
    int dx = pt.x - td.down.x, dy = pt.y - td.down.y;
    if (code == LV_EVENT_PRESSING)
    {
        if (!td.dragging && (dx - td.x) * (dx - td.x) + (dy - td.y) * (dy - td.y) > 8 * 8) td.dragging = true;
        if (!td.dragging) return;
        td.x = dx;
        td.y = dy;
        bool past = td_past_line();
        if (past != td.past) sfx_play_ex(SFX_UI_TICK, past ? 120 : 90, 60);   /* a soft tick at the line */
        td.past = past;
        td_layout();
        return;
    }
    /* let go: a finger that stopped before lifting has no flick. A flick
     * rates from short of the line, the way it went; one back towards the
     * middle (or down) keeps the card. */
    td.held = false;
    int vx = lv_tick_get() - g_tl > 80 ? 0 : g_vx, vy = lv_tick_get() - g_tl > 80 ? 0 : g_vy;
    int en, to = td_heading(&en);
    bool back = LV_ABS(vx) >= TD_FLICK && (vx > 0) != (td.x > 0);
    bool flick = LV_ABS(vx) >= TD_FLICK && LV_ABS(vx) >= -vy && LV_ABS(td.x) >= 24 && !back;
    bool up = vy <= -TD_FLICK && -vy > LV_ABS(vx) && -td.y >= 24, down = vy >= TD_FLICK;
    char way = 0;
    if (!td.dragging) way = 0;
    else if (up) way = '*';
    else if (flick) way = td.x > 0 ? '+' : '-';
    else if (to == 3 && en >= 256 && !down) way = '*';
    else if ((to == 0 || to == 1) && en >= 256 && !back) way = to ? '+' : '-';
    if (way) td_fly(way, vx, vy);
    else td_spring();
}

/* A button's press: in under the finger, back with a bounce. */
static void td_bscale_exec(void *var, int32_t v)
{
    td.bscale[(int *)var - td.bscale] = v;
    td_buttons();
}
static void td_btn_press_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bool down = lv_event_get_code(e) == LV_EVENT_PRESSED;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &td.bscale[i]);
    lv_anim_set_values(&a, td.bscale[i], down ? 220 : 256);
    lv_anim_set_duration(&a, down ? SL_T_TOUCH : SL_T_SLOW);
    lv_anim_set_path_cb(&a, down ? lv_anim_path_ease_out : lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, td_bscale_exec);
    lv_anim_start(&a);
}

static void td_hint_hide(void) { lv_obj_add_flag(td.hint, LV_OBJ_FLAG_HIDDEN); }
static void td_back_in(lv_anim_t *a)
{
    (void)a;
    td.returning = false;
    clip_swap(td.deck[td.at]);
}
/* Undo: the last card flies back in from the side it went, and the
 * rating it got is taken back. */
static void td_undo(void)
{
    if (td.flying || td.returning || td.held || td.at <= 0 || td.at > td.n) return;
    int i = td.at - 1, t = td.deck[i];
    if (td.kept & (1u << i)) ts_unswipe(t);
    td.kept &= ~(1u << i);
    if (td.way[i] != '-') td.liked--;
    td_clip_fade();
    if (td.at == td.n) td_hint_hide();
    lv_anim_delete(&td, NULL);
    td.at = i;
    td_slot_make(i);                     /* in the slot of the card at the bottom, which goes */
    for (int k = 1; k < TD_STACK; k++)
        if (!td_slot_of(i + k) && i + k < td.n) td_slot_make(i + k);
    /* from where it went: the side, or the top for a love */
    td.x = td.way[i] == '*' ? 0 : td.way[i] == '+' ? TD_OFF : -TD_OFF;
    td.y = td.way[i] == '*' ? -(TD_TOP + TD_CARD_H + 40) : -40;
    td.lift = 0;
    td.rot_sign = 1;
    td.returning = true;
    td_restack();
    td_layout();
    td_count();
    td_wash_top(true);
    sfx_play(SFX_UI_BACK);
    td_move(0, 0, 600, td_spring_path, td_back_in);
}
static void td_btn_cb(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i == 2) td_undo();
    else if (!td.held) td_fly(i == 3 ? '*' : i ? '+' : '-', 0, 0);
}

/* Distance from (x, y), from the middle, to a box hw x hh across its
 * halves with corners of radius r: below 0 inside. */
static float td_box_dist(float x, float y, float hw, float hh, float r)
{
    float qx = (x < 0 ? -x : x) - hw + r, qy = (y < 0 ? -y : y) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return (ox > 0 && oy > 0 ? sqrtf(ox * ox + oy * oy) : ox + oy) + (in < 0 ? in : 0) - r;
}
/* The shadow's three strips: alpha only, drawn black. */
static void td_make_shadow(lv_obj_t *p)
{
    int w = TD_CARD_W, h = TD_CARD_H, b = TD_SH_BLUR;
    const int x0[3] = { -b, w - b, b }, y0[3] = { TD_SH_DY - b, TD_SH_DY - b, h + TD_SH_DY - b };
    const int ws[3] = { 2 * b, 2 * b, w - 2 * b }, hs[3] = { h + 2 * b, h + 2 * b, 2 * b };
    for (int j = 0; j < 3; j++)
    {
        lv_draw_buf_t *d = td.shadow_buf[j] = lv_draw_buf_create(ws[j], hs[j], LV_COLOR_FORMAT_A8, 0);
        if (!d) continue;
        for (int y = 0; y < hs[j]; y++)
        {
            uint8_t *row = d->data + y * d->header.stride;
            for (int x = 0; x < ws[j]; x++)
            {
                /* the card's box, TD_SH_DY lower, faded over TD_SH_BLUR each side of its edge */
                float dd = td_box_dist(x0[j] + x + 0.5f - w / 2.f, y0[j] + y + 0.5f - TD_SH_DY - h / 2.f, w / 2.f, h / 2.f, SL_R_PILL);
                float f = (b - dd) / (2 * b);
                row[x] = (uint8_t)(f <= 0 ? 0 : f >= 1 ? TD_SH_OPA : TD_SH_OPA * f * f * (3 - 2 * f));
            }
        }
        lv_obj_t *o = td.shadow[j] = lv_image_create(p);
        lv_image_set_src(o, d);
        lv_obj_set_style_image_recolor(o, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The three stamps, made once a page: a picture each, turned with the card. */
static void td_make_stamps(void)
{
    static const char *const words[3] = { "PASS", "LIKE", "LOVE" };
    static const uint32_t inks[3] = { 0xff5a5a, 0x3fd47a, 0x3d9df2 };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *s = text(td.page, words[i], &ss_bold_28, inks[i]);
        lv_obj_set_style_text_color(s, lv_color_hex(inks[i]), 0);
        lv_obj_set_style_text_letter_space(s, 3, 0);
        lv_obj_set_style_border_width(s, 4, 0);
        lv_obj_set_style_border_color(s, lv_color_hex(inks[i]), 0);
        lv_obj_set_style_radius(s, SL_R_SM, 0);
        lv_obj_set_style_pad_hor(s, 14, 0);
        lv_obj_set_style_pad_ver(s, 3, 0);
        lv_obj_set_style_bg_color(s, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s, 90, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
        td.stamp_buf[i] = td_snap(s);
        lv_obj_delete(s);
        if (!td.stamp_buf[i]) continue;
        lv_obj_t *im = td.stamp[i] = lv_image_create(td.page);
        lv_image_set_src(im, td.stamp_buf[i]);
        lv_image_set_pivot(im, td.stamp_buf[i]->header.w / 2, td.stamp_buf[i]->header.h / 2);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
    }
    int side = 0;                        /* room for the biggest, turned and at its largest */
    for (int i = 0; i < 3; i++)
        if (td.stamp_buf[i] && (int)(td.stamp_buf[i]->header.w + td.stamp_buf[i]->header.h) * 3 / 2 + 8 > side)
            side = (td.stamp_buf[i]->header.w + td.stamp_buf[i]->header.h) * 3 / 2 + 8;
    if (side) td.stamp_turn = lv_draw_buf_create(side, side, LV_COLOR_FORMAT_RGB565A8, 0);
    td.stamp_key = -1;
}

/* The deck dealt: its first three cards on screen. */
static void td_stack_build(bool anim)
{
    td_deal_to(td.at + TD_STACK);
    for (int k = 0; k < TD_STACK; k++) td_slot_make(td.at + k);
    td.x = td.y = td.lift = 0;
    td_restack();
    td_layout();
    td_count();
    td_wash_top(anim);                   /* at once as the page arrives */
    if (td.n) clip_swap(td.deck[td.at]);
}

static void td_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != td.page) return;   /* an older deck's: td is not its any more */
    if (td.timer) lv_timer_delete(td.timer);
    lv_anim_delete(&td, NULL);
    lv_anim_delete(&td.lift, NULL);
    for (int i = 0; i < 4; i++) lv_anim_delete(&td.bscale[i], NULL);
    lv_anim_delete(&td.ghost_in, NULL);
    for (int j = 0; j < TD_STEPS; j++) td_free_buf(td.step[j]);
    td_free_buf(td.next0);
    td_free_buf(td.wash_buf);
    td_free_buf(td.turn);
    td_free_buf(td.turn_back);
    /* the objects go with the page; the pictures are ours */
    for (int k = 0; k < TD_STACK; k++) td_free_buf(td.slot[k].buf);
    for (int i = 0; i < 3; i++) td_free_buf(td.stamp_buf[i]);
    td_free_buf(td.stamp_turn);
    for (int j = 0; j < 3; j++) td_free_buf(td.shadow_buf[j]);
    /* your own song back, where it was, and playing again if it was */
    if (quiz_clip_active()) { quiz_hush_clip(); quiz_restore_clip(); }
    if (td_resume) { td_resume = false; audio_resume(); }
    if (ts.n_rated) ts.asked = true;     /* left half way: For you won't deal it again */
    if (ts.asked) { if (access(TS_TASTE, F_OK)) ts_save_file(TS_TASTE, true); else ts_flush(); }
    memset(&td, 0, sizeof td);
}
static void td_done_cb(lv_event_t *e)
{
    (void)e;
    bool first = td.first;
    ts.asked = true;
    page_pop();
    if (first) foryou_page();
}
static void td_mix_cb(lv_event_t *e)
{
    (void)e;
    int *mix = NULL, n = ts_mix(&mix);
    if (!n) { free(mix); toast("Like a few songs first"); return; }
    if (quiz_clip_active()) { quiz_hush_clip(); quiz_restore_clip(); }   /* your own song back first, then the mix over it */
    sl_log("taste: your mix, %d songs", n);
    if (play_list(mix, n, (int)(td_rand() % (unsigned)n), true)) toast("Playing your mix");
    free(mix);
}
static void td_more_cb(lv_event_t *e)
{
    (void)e;
    td_hint_hide();
    td_deal();
    if (td.n) td_stack_build(true);
    else td_finish();
}
/* Out of cards: how it went, another deck or done. */
static void td_finish(void)
{
    lv_anim_delete(&td, NULL);
    for (int k = 0; k < TD_STACK; k++) td_unslot(&td.slot[k]);
    td.pending = 0;
    td.x = td.y = td.lift = 0;
    td_layout();
    td_wash(lv_color_hex(C_BG), true);
    if (quiz_clip_active()) { quiz_hush_clip(); quiz_restore_clip(); }
    ts.asked = true;
    if (access(TS_TASTE, F_OK)) ts_save_file(TS_TASTE, true);   /* "#skipped" when nothing was swiped: asked once */
    else ts_flush();
    char b[64];
    snprintf(b, sizeof b, td.liked == 1 ? "1 song liked" : "%d songs liked", td.liked);
    lv_label_set_text(lv_obj_get_child(td.hint, 0), b);
    lv_obj_remove_flag(td.hint, LV_OBJ_FLAG_HIDDEN);
    td_count();
}
static void td_tick(lv_timer_t *t)
{
    (void)t;
    /* a snippet stops after 20 seconds of sound: enough to know */
    if (quiz_clip_active() && quiz.clip.audible && !quiz.clip.hushed && sl_ms() - quiz.clip.asked_at > 20000)
        quiz_hush_clip();
    if (!quiz.clip.audible && quiz_clip_active() && quiz_clip_heard()) quiz.clip.audible = true;
    /* held still a fifth of a second: the turned copy made fine (the moving
     * one is coarser), a third of it a tick so no frame carries it all */
    if (td.held)
    {
        if (td.back_row >= 0) td_layout();   /* a turn left half made when the finger stopped: shown */
        td.still_ticks = td.x == td.still_x && td.y == td.still_y ? td.still_ticks + 1 : 0;
        td.still_x = td.x; td.still_y = td.y;
        struct td_slot *s = td_slot_of(td.at);
        if (td.still_ticks >= 2 && !td.turn_fine && td.back_row < 0 && td.turn_card == td.at && s && s->shown == td.turn)
        {
            int ox, oy, h = td.turn->header.h, r1 = td.refine_row + (h + 2) / 3;
            td_turn_into(s->buf, td.turn, td.turn_rot, td.turn_scale, true, TD_CARD_W / 2, TD_PIVOT, &ox, &oy, td.refine_row, r1);
            lv_image_cache_drop(td.turn);
            lv_area_t a;
            lv_obj_get_coords(s->img, &a);
            lv_area_t band = { a.x1, a.y1 + td.refine_row, a.x2, a.y1 + (r1 < h ? r1 : h) - 1 };
            lv_obj_invalidate_area(s->img, &band);
            td.refine_row = r1;
            if (r1 >= h) td.turn_fine = true;
        }
        return;
    }
    /* the rest is made while the cards sit still, one piece a tick: the
     * next card picked, the pictures, the steps of the card under */
    if (td.flying || lv_anim_get(&td, NULL)) return;
    if (ts.on_card != ts.n_rated) { ts_flush(); return; }   /* the last swipe onto the card, first */
    if (td.wash_step < TD_WASH_STEPS)    /* the new card's colour first, a step a tick */
    {
        td.wash_step++;
        td_wash_exec(NULL, td.wash_step * 255 / TD_WASH_STEPS);
        return;
    }
    if (td.dealt < td.at + TD_STACK && td.dealt < td.n)
    {
        int d0 = td.dealt;
        td_deal_to(td.at + TD_STACK);
        for (int i = d0; i < td.dealt; i++) if (i >= td.at && i < td.at + TD_STACK) td.pending |= 1u << (i % TD_STACK);
        if (td.at + 2 >= td.n) td_layout();   /* fewer cards than thought: no strip for one */
        return;
    }
    for (int k = 0; k < TD_STACK; k++)
    {
        if (!(td.pending & (1u << k))) continue;
        int i = td.at + (k - td.at % TD_STACK + TD_STACK) % TD_STACK;
        td_slot_make(i);
        td_restack();
        td_layout();
        return;
    }
    for (int k = 0; k < TD_STACK; k++)   /* a cover that came after the picture */
    {
        struct td_slot *s = &td.slot[k];
        if (s->src && s->buf && lv_obj_get_child_count(s->cover) != s->art)
        {
            td_picture(s);
            return;
        }
    }
    if (td_step_make()) td_layout();
}

/* A button: its ring (filled as the drag heads for it) and its mark, at
 * its size of the moment, in a box big enough for it at its biggest. */
#define TD_BTN_BOX(size) ((size) * 5 / 4)
static void td_btn_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e), size = i >= 2 ? 54 : 76;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2 + 1) / 2, cy = (a.y1 + a.y2 + 1) / 2, r = (size * td.bsc[i] / 256 + 1) / 2;
    lv_color_t ink = lv_color_hex(td.ink[i]);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = ink;
    d.bg_opa = (lv_opa_t)td.bfill[i];
    d.border_color = ink;
    d.border_width = 2;
    d.border_opa = i == 2 ? 150 : LV_OPA_COVER;
    lv_area_t c = { cx - r, cy - r, cx + r - 1, cy + r - 1 };
    lv_draw_rect(layer, &d, &c);
    /* the mark: white as the ring fills */
    int k = td.bfill[i] * 256 / 230;
    lv_color_t mark = lv_color_mix(lv_color_white(), ink, (uint8_t)(k > 255 ? 255 : k));
    int g = (i >= 2 ? 26 : 28) * td.bsc[i] / 256;
    struct ic_pen p = { layer, cx - g / 2, cy - g / 2, g / 48.f, mark, lv_color_hex(C_BG) };
    switch (i)
    {
        case 0:                          /* pass: a cross */
            ic_line(&p, 11, 11, 37, 37, 6.5f, false);
            ic_line(&p, 37, 11, 11, 37, 6.5f, false);
            break;
        case 1:                          /* like: a tick */
            ic_line(&p, 8, 25, 19, 36, 6.5f, false);
            ic_line(&p, 19, 36, 40, 13, 6.5f, false);
            break;
        case 2:
        {   /* undo: round the clock the other way, the head at eleven o'clock */
            ic_arc(&p, 24, 25, 15, 250, 190, 5, false);
            float ang = 250 * 3.14159265f / 180, px = 24 + 17.5f * cosf(ang), py = 25 + 17.5f * sinf(ang);
            float tx = sinf(ang), ty = -cosf(ang), rx = cosf(ang), ry = sinf(ang);
            ic_tri(&p, px + tx * 10, py + ty * 10, px - tx * 2 + rx * 9, py - ty * 2 + ry * 9,
                   px - tx * 2 - rx * 9, py - ty * 2 - ry * 9, false);
            break;
        }
        default:                         /* love: the heart, or the star as Customization has it */
        {
            lv_area_t ic = { cx - g / 2, cy - g / 2 + 1, cx - g / 2 + g - 1, cy - g / 2 + g };
            sl_icon_paint(layer, &ic, IC_HEART, mark, lv_color_hex(C_BG));
            break;
        }
    }
}

static void sl_mem_release(const char *why, bool images);
static void taste_page(bool first)
{
    ts_load();
    if (page_bring_back(td.page)) return;
    {
        int st = audio_status();
        td_resume = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
        if (td_resume) audio_pause();   /* the first clip keeps the queue and where it was */
        sl_doing_is("Your taste", NULL);
        sl_mem_release("Your taste", true);   /* the glass and the image cache, before the deck's pictures */
    }
    /* the last deck still sliding away (Done, Back, then here again at
     * once) holds td's pictures: gone now, freeing them as its own, before
     * this one makes new ones (its delete came after, and freed these) */
    if (td.page) { ps_end(); if (td.page) lv_obj_delete(td.page); }
    memset(&td, 0, sizeof td);
    td.first = first;
    td.rng = (unsigned)current_tick * 2654435761u + 1;
    for (int i = 0; i < 4; i++) td.bscale[i] = 256;
    td.rot_sign = 1;
    lv_obj_t *p, *body = page_new("Your taste", &p);
    td.page = p;
    lv_obj_add_event_cb(p, td_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    td.count = text(p, "", &ss_semibold_18, C_SUB);
    lv_obj_align(td.count, LV_ALIGN_TOP_RIGHT, -22, SB_H + 24);
    {   /* the title stops before the count, as wide as it can get ("88 of 888") */
        char b[32];
        snprintf(b, sizeof b, sl_tr("%d of %d"), 88, 888);
        header_title_clear(p, W - 22 - lv_text_get_width(b, strlen(b), &ss_semibold_18, 0));
    }
    td.wash_cur = td.wash_from = td.wash_to = lv_color_hex(C_BG);
    td.wash_step = TD_WASH_STEPS;
    /* the top card's cover colour, fading out down the page: behind the
     * header, so made after the title has found its room */
    if (!sl_zune())
    {
        td.wash_buf = lv_draw_buf_create(W, TD_WASH_END, LV_COLOR_FORMAT_RGB565, 0);
        if (td.wash_buf)
        {
            td.wash_img = lv_image_create(p);
            lv_obj_move_background(td.wash_img);
            lv_obj_remove_flag(td.wash_img, LV_OBJ_FLAG_CLICKABLE);
            td_wash_fill(td.wash_cur);
            lv_image_set_src(td.wash_img, td.wash_buf);
        }
    }
    /* for a tap instead of a swipe: undo, pass, like and love in a row, the
     * two that matter most biggest, in the middle under the thumb */
    /* deeper on the light theme, where the bright ones fell under 3:1 */
    bool light = sl_theme() == 1;
    const uint32_t inks[4] = { light ? 0xd93030 : 0xff5a5a, light ? 0x1f9d55 : 0x3fd47a, 0xf2b33d, light ? 0x1f74c9 : 0x3d9df2 };
    /* the same gap, SL_INSET, between each two: 54, 76, 76 and 54 across */
    const int bx[4] = { -(SL_INSET / 2 + 38), SL_INSET / 2 + 38, -(SL_INSET * 3 / 2 + 76 + 27), SL_INSET * 3 / 2 + 76 + 27 };
    for (int i = 0; i < 4; i++)
    {
        int size = i >= 2 ? 54 : 76;
        lv_obj_t *b = td.btn[i] = box(p);
        td.ink[i] = inks[i];
        td.bsc[i] = 256;
        lv_obj_set_size(b, TD_BTN_BOX(size), TD_BTN_BOX(size));
        lv_obj_add_event_cb(b, td_btn_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
        /* under the stack, clear of the mini player */
        lv_obj_align(b, LV_ALIGN_TOP_MID, bx[i], TD_BTN_Y + (76 - TD_BTN_BOX(size)) / 2);
        on_click(b, td_btn_cb, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, td_btn_press_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, td_btn_press_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, td_btn_press_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    /* the end of a deck */
    td.hint = box(p);
    lv_obj_set_size(td.hint, W - 60, LV_SIZE_CONTENT);
    lv_obj_align(td.hint, LV_ALIGN_TOP_MID, 0, SB_H + 220);
    lv_obj_set_flex_flow(td.hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(td.hint, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(td.hint, 18, 0);
    text(td.hint, "", &ss_semibold_26, C_TEXT);
    /* what it learned, to play: the end of a deck's first choice */
    lv_obj_t *mrow = box(td.hint);
    lv_obj_set_size(mrow, lv_pct(100), 60);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_t *mix = pill(mrow, LV_SYMBOL_PLAY "  Play your mix", true, 0);
    lv_obj_remove_event_cb(mix, pill_cb);
    lv_obj_add_event_cb(mix, td_mix_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *row = box(td.hint);
    lv_obj_set_size(row, lv_pct(100), 60);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *more = pill(row, LV_SYMBOL_REFRESH "  More", false, 0);
    lv_obj_t *done = pill(row, LV_SYMBOL_OK "  Done", false, 0);
    lv_obj_remove_event_cb(more, pill_cb);
    lv_obj_remove_event_cb(done, pill_cb);
    lv_obj_add_event_cb(more, td_more_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(done, td_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(td.hint, LV_OBJ_FLAG_HIDDEN);
    td.ghost = box(p);
    lv_obj_add_style(td.ghost, &st_card, 0);
    lv_obj_set_style_radius(td.ghost, sl_zune() ? 0 : SL_R_PILL, 0);
    lv_obj_add_flag(td.ghost, LV_OBJ_FLAG_HIDDEN);
    td.ghost_in = 256;
    /* The deck's pictures are about 5.7 MB, a card ~0.5 MB: with the
     * library up and Bluetooth on the R1 had 3.7 MB free, and every swipe
     * stalled while it swapped (a device log, 3 Nov). Short of memory, the
     * ready-made steps of the card under and the second turned copy are
     * not made (each is drawn as it is needed instead), nor, shorter
     * still, the turned copy. */
    long free_kb = td_test_kb ? td_test_kb : sl_mem_available_kb();
    bool lean = free_kb >= 0 && free_kb < 16 * 1024, leaner = free_kb >= 0 && free_kb < 10 * 1024;
    if (lean)
        sl_log("taste: %ld KiB free: the deck without its ready-made steps%s", free_kb, leaner ? " or turned copies" : "");
    for (int j = 0; j < TD_STEPS && !lean; j++)
    {
        int q = 240 + 4 * j;
        td.step[j] = lv_draw_buf_create((TD_CARD_W * q + 128) / 256, (TD_CARD_H * q + 128) / 256, LV_COLOR_FORMAT_RGB565A8, 0);
    }
    if (!lean) td.next0 = lv_draw_buf_create((TD_CARD_W * 240 + 128) / 256, (TD_CARD_H * 240 + 128) / 256, LV_COLOR_FORMAT_RGB565A8, 0);
    td.step_card = td.turn_card = td.next0_card = -1;
    if (!sl_zune()) td_make_shadow(p);    /* the lifted card's shadow, shown by td_layout */
    if (!leaner) td.turn = lv_draw_buf_create(TD_TURN_W, TD_TURN_H, LV_COLOR_FORMAT_RGB565A8, 0);
    if (!lean) td.turn_back = lv_draw_buf_create(TD_TURN_W, TD_TURN_H, LV_COLOR_FORMAT_RGB565A8, 0);
    td.back_row = -1;
    td_make_stamps();
    td_deal();
    td.timer = lv_timer_create(td_tick, 50, NULL);   /* a piece of work at most each tick, each short */
    if (td.n) td_stack_build(false);
    else td_finish();
    page_push(p);
}

/* For you's first visit, with songs enough to swipe */
static bool ts_first_visit(void)
{
    ts_load();
    return !ts.asked && sl_lib.n_tracks >= 20;
}

/* ---------------------------------------------------- hidden artists */
static lv_obj_t *hidden_page_obj, *hidden_body;
static void hidden_fill(void);
static void hidden_show_cb(lv_event_t *e)
{
    ts_show_artist((int)(intptr_t)lv_event_get_user_data(e));
    hidden_fill();
}
static void hidden_fill(void)
{
    lv_obj_clean(hidden_body);
    if (!ts.n_hidden)
    {
        sl_empty(hidden_body, IC_ARTISTS, 96, "No hidden artists",
                 "\"Don't play this artist\" on a song keeps them out of Radio, Shuffle and For you.", 56);
        return;
    }
    lv_obj_t *c = opt_card(hidden_body, NULL, NULL);
    for (int i = 0; i < ts.n_hidden; i++)
    {
        lv_obj_t *r = box(c);
        lv_obj_set_size(r, lv_pct(100), 60);
        lv_obj_t *n = text(r, ts.hidden[i], &ss_semibold_21, C_TEXT);
        lv_obj_set_width(n, W - 200);
        label_lines(n, 1);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *b = pill(r, "Allow", false, 0);
        lv_obj_remove_event_cb(b, pill_cb);
        lv_obj_set_flex_grow(b, 0);
        lv_obj_set_width(b, 110);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(b, hidden_show_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}
static void hidden_deleted(lv_event_t *e) { (void)e; hidden_page_obj = hidden_body = NULL; }
static void hidden_page(void)
{
    ts_load();
    if (page_bring_back(hidden_page_obj)) return;
    lv_obj_t *p;
    hidden_body = page_new("Hidden artists", &p);
    hidden_page_obj = p;
    lv_obj_add_event_cb(p, hidden_deleted, LV_EVENT_DELETE, NULL);
    hidden_fill();
    page_push(p);
}
