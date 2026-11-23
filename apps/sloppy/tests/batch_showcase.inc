/* SLOPPY_SHOWCASE=<covers dir> [SLOPPY_SHOWCASE_OUT=<dir>]: the shots on the
 * GitHub front page, with real songs, artists and sleeves in place of the
 * audit library's made-up ones. The covers are not in the repo: <dir> holds
 * 00.jpg.. (the albums below, in order: NN, two digits), pod.jpg (the podcast's) and
 * sub.jpg (the music server's album). Writes <out>/<name>.ppm and stops. */
static const struct sc_album { const char *title, *artist, *genre; int year, n; const char *songs[8]; int secs[8], nums[8]; } sc_albums[] = {
    { "England Made Me", "Black Box Recorder", "Pop", 1998, 8,
      { "Girl Singing in the Wreckage", "England Made Me", "New Baby Boom", "It's Only the End of the World", "Ideal Home",
        "Child Psychology", "I. C. One Female", "Up Town Top Ranking" }, { 162, 240, 130, 321, 159, 248, 139, 237 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "Bored", "Billie Eilish", "Pop", 2017, 1, { "Bored" }, { 180 }, { 1 } },   /* the single */
    { "Did you know that there's a tunnel under Ocean Blvd", "Lana Del Rey", "Pop", 2023, 8,
      { "The Grants", "Did you know that there's a tunnel under Ocean Blvd", "Sweet", "A&W", "Candy Necklace", "Kintsugi",
        "Fingertips", "Paris, Texas" }, { 295, 285, 215, 434, 315, 379, 348, 207 }, { 1, 2, 3, 4, 6, 8, 9, 10 } },
    { "Chemtrails Over the Country Club", "Lana Del Rey", "Pop", 2021, 8,
      { "White Dress", "Chemtrails Over the Country Club", "Tulsa Jesus Freak", "Let Me Love You Like a Woman", "Wild at Heart",
        "Dark but Just a Game", "Not All Who Wander Are Lost", "Yosemite" }, { 333, 271, 215, 200, 246, 235, 247, 304 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "Blue Banisters", "Lana Del Rey", "Pop", 2021, 8,
      { "Text Book", "Blue Banisters", "Arcadia", "Black Bathing Suit", "If You Lie Down With Me", "Beautiful", "Violets for Roses",
        "Dealer" }, { 303, 293, 263, 318, 265, 215, 255, 274 }, { 1, 2, 3, 5, 6, 7, 8, 9 } },
    { "Chocolate and Cheese", "Ween", "Rock", 1994, 8,
      { "Take Me Away", "Spinal Meningitis (Got Me Down)", "Freedom of '76", "I Can't Put My Finger on It", "A Tear for Eddie",
        "Roses Are Free", "Baby Bitch", "Voodoo Lady" }, { 180, 172, 170, 167, 289, 274, 184, 228 }, { 1, 2, 3, 4, 5, 6, 7, 10 } },
    { "It's Not Me, It's You", "Lily Allen", "Pop", 2009, 8,
      { "Everyone's at It", "The Fear", "Not Fair", "22", "I Could Say", "Back to the Start", "Never Gonna Happen",
        "Who'd Have Known" }, { 278, 207, 201, 186, 244, 254, 207, 230 }, { 1, 2, 3, 4, 5, 6, 7, 9 } },
    { "Loose", "Nelly Furtado", "Pop", 2006, 8,   /* the US CD, its interludes left out */
      { "Afraid", "Maneater", "Promiscuous", "Glow", "Showtime", "No Hay Igual", "Say It Right", "Do It" },
      { 219, 260, 244, 244, 246, 217, 225, 223 }, { 1, 2, 4, 5, 6, 8, 10, 11 } },
    { "Hounds of Love", "Kate Bush", "Pop", 1985, 8,
      { "Running Up That Hill (A Deal with God)", "Hounds of Love", "The Big Sky", "Mother Stands for Comfort", "Cloudbusting",
        "And Dream of Sheep", "Under Ice", "Waking the Witch" }, { 300, 182, 281, 186, 304, 166, 141, 259 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "OK Computer", "Radiohead", "Rock", 1997, 8,
      { "Airbag", "Paranoid Android", "Subterranean Homesick Alien", "Exit Music (for a Film)", "Let Down", "Karma Police",
        "Fitter Happier", "Electioneering" }, { 284, 383, 267, 264, 299, 261, 117, 230 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "Rumours", "Fleetwood Mac", "Rock", 1977, 8,
      { "Second Hand News", "Dreams", "Never Going Back Again", "Don't Stop", "Go Your Own Way", "Songbird", "The Chain",
        "You Make Loving Fun" }, { 163, 254, 122, 191, 218, 200, 268, 211 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
    { "When We All Fall Asleep, Where Do We Go?", "Billie Eilish", "Pop", 2019, 8,
      { "bad guy", "xanny", "you should see me in a crown", "all the good girls go to hell", "wish you were gay",
        "when the party's over", "8", "my strange addiction" }, { 194, 244, 181, 169, 222, 196, 173, 180 }, { 2, 3, 4, 5, 6, 7, 8, 9 } },
    { "The Facts of Life", "Black Box Recorder", "Pop", 2000, 7,
      { "The Art of Driving", "Weekend", "The English Motorway System", "May Queen", "Sex Life", "French Rock'n'Roll", "The Facts of Life" },
      { 265, 147, 280, 219, 176, 181, 277 }, { 1, 2, 3, 4, 5, 6, 7 } },
    { "So Tonight That I Might See", "Mazzy Star", "Rock", 1993, 6,
      { "Fade Into You", "Bells Ring", "Mary of Silence", "Five String Serenade", "Blue Light", "She's My Baby" },
      { 295, 272, 362, 264, 310, 265 }, { 1, 2, 3, 4, 5, 6 } },
    { "Bloom", "Beach House", "Pop", 2012, 6,
      { "Myth", "Wild", "Lazuli", "Other People", "The Hours", "Troublemaker" },
      { 258, 298, 301, 264, 251, 295 }, { 1, 2, 3, 4, 5, 6 } },
    { "Cry", "Cigarettes After Sex", "Pop", 2019, 6,
      { "Don't Let Me Go", "Kiss It Off Me", "Heavenly", "You're the Only Good Thing in My Life", "Touch", "Hentai" },
      { 263, 270, 286, 275, 293, 285 }, { 1, 2, 3, 4, 5, 6 } },
    { "Immunity", "Clairo", "Pop", 2019, 6,
      { "Alewife", "Impossible", "Closer to You", "North", "Bags", "Softly" },
      { 213, 230, 184, 213, 260, 185 }, { 1, 2, 3, 4, 5, 6 } },
    { "Punisher", "Phoebe Bridgers", "Rock", 2020, 6,
      { "DVD Menu", "Garden Song", "Kyoto", "Punisher", "Halloween", "Chinese Satellite" },
      { 69, 219, 184, 189, 271, 217 }, { 1, 2, 3, 4, 5, 6 } },
    { "Be the Cowboy", "Mitski", "Rock", 2018, 6,
      { "Geyser", "Why Didn't You Stop Me?", "Old Friend", "A Pearl", "Lonesome Love", "Remember My Name" },
      { 143, 141, 112, 156, 110, 135 }, { 1, 2, 3, 4, 5, 6 } },
    { "Titanic Rising", "Weyes Blood", "Pop", 2019, 6,
      { "A Lot's Gonna Change", "Andromeda", "Everyday", "Something to Believe", "Titanic Rising", "Movies" },
      { 262, 280, 307, 286, 96, 354 }, { 1, 2, 3, 4, 5, 6 } },
    { "Dummy", "Portishead", "Electronic", 1994, 6,
      { "Mysterons", "Sour Times", "Strangers", "It Could Be Sweet", "Wandering Star", "It's a Fire" },
      { 306, 254, 238, 259, 296, 227 }, { 1, 2, 3, 4, 5, 6 } },
    { "Carrie & Lowell", "Sufjan Stevens", "Folk", 2015, 6,
      { "Death With Dignity", "Should Have Known Better", "All of Me Wants All of You", "Drawn to the Blood", "Fourth of July", "The Only Thing" },
      { 239, 305, 221, 196, 281, 194 }, { 1, 2, 3, 4, 5, 6 } },
    { "Disintegration", "The Cure", "Rock", 1989, 6,
      { "Plainsong", "Pictures of You", "Closedown", "Lovesong", "Last Dance", "Lullaby" },
      { 315, 448, 260, 210, 286, 251 }, { 1, 2, 3, 4, 5, 6 } },
    { "Doolittle", "Pixies", "Rock", 1989, 6,
      { "Debaser", "Tame", "Wave of Mutilation", "I Bleed", "Here Comes Your Man", "Dead" },
      { 172, 115, 124, 154, 201, 141 }, { 1, 2, 3, 4, 5, 6 } },
    { "Either/Or", "Elliott Smith", "Folk", 1997, 6,
      { "Speed Trials", "Alameda", "Ballad of Big Nothing", "Between the Bars", "Pictures of Me", "No Name no.5" },
      { 182, 223, 168, 141, 226, 223 }, { 1, 2, 3, 4, 5, 6 } },
    { "Melodrama", "Lorde", "Pop", 2017, 6,
      { "Green Light", "Sober", "Homemade Dynamite", "The Louvre", "Liability", "Hard Feelings/Loveless" },
      { 234, 197, 189, 271, 171, 367 }, { 1, 2, 3, 4, 5, 6 } },
    { "Mezzanine", "Massive Attack", "Electronic", 1998, 6,
      { "Angel", "Risingson", "Teardrop", "Inertia Creeps", "Exchange", "Dissolved Girl" },
      { 380, 298, 330, 356, 251, 366 }, { 1, 2, 3, 4, 5, 6 } },
    { "Souvlaki", "Slowdive", "Rock", 1993, 6,
      { "Alison", "Machine Gun", "40 Days", "Sing", "Here She Comes", "Souvlaki Space Station" },
      { 232, 268, 196, 291, 140, 359 }, { 1, 2, 3, 4, 5, 6 } },
    { "Heaven or Las Vegas", "Cocteau Twins", "Pop", 1990, 6,
      { "Cherry-Coloured Funk", "Pitch the Baby", "Iceblink Luck", "Fifty-Fifty Clown", "Heaven or Las Vegas", "I Wear Your Ring" },
      { 192, 196, 198, 195, 296, 220 }, { 1, 2, 3, 4, 5, 6 } },
    { "Fetch the Bolt Cutters", "Fiona Apple", "Rock", 2020, 6,
      { "I Want You to Love Me", "Shameika", "Fetch the Bolt Cutters", "Under the Table", "Relay", "Rack of His" },
      { 237, 248, 298, 201, 289, 222 }, { 1, 2, 3, 4, 5, 6 } },
    /* last, so the earlier covers keep their numbers; one song, so the Songs
     * pages still find a run of rows clear of the songs the shots play */
    { "Good Girl Gone Bad: Reloaded", "Rihanna", "Pop", 2008, 1, { "Umbrella" }, { 275 }, { 1 } },
    { "Hit Me Hard and Soft", "Billie Eilish", "Pop", 2024, 8,
      { "SKINNY", "LUNCH", "CHIHIRO", "BIRDS OF A FEATHER", "WILDFLOWER", "THE GREATEST", "L'AMOUR DE MA VIE", "THE DINER" },
      { 219, 180, 303, 210, 261, 293, 333, 186 }, { 1, 2, 3, 4, 5, 6, 7, 8 } },
};
#define SC_NA ((int)(sizeof sc_albums / sizeof sc_albums[0]))   /* at most SC_N, and 8 songs each */

static unsigned char *sc_read(const char *dir, const char *name, size_t *n)
{
    char p[700]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = len > 0 ? malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f); *n = b ? (size_t)len : 0;
    return b;
}
static void sc_load(const char *dir)
{
    for (int a = 0; a < SC_NA; a++)
    {
        char nm[16]; snprintf(nm, sizeof nm, "%02d.jpg", a);
        size_t n; unsigned char *b = sc_read(dir, nm, &n); int c;
        if (!b) { fprintf(stderr, "showcase: no %s/%s\n", dir, nm); continue; }
        sc_art[a].rgb = stbi_load_from_memory(b, (int)n, &sc_art[a].w, &sc_art[a].h, &c, 3); free(b);
        if (!sc_art[a].rgb) continue;
        /* its colour: the mean, the vivid pixels counting most, as a wash under the cover */
        double r = 0, g = 0, bl = 0, wsum = 0;
        for (long i = 0; i < (long)sc_art[a].w * sc_art[a].h; i++)
        {
            const unsigned char *p = sc_art[a].rgb + i * 3;
            int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            double w = 0.05 + (mx ? (double)(mx - mn) / mx : 0) * mx / 255.0;
            r += p[0] * w; g += p[1] * w; bl += p[2] * w; wsum += w;
        }
        lv_color_hsv_t hsv = lv_color_rgb_to_hsv((uint8_t)(r / wsum), (uint8_t)(g / wsum), (uint8_t)(bl / wsum));
        sc_art[a].hue = hsv.h; sc_art[a].sat = hsv.s < 15 ? 15 : hsv.s > 85 ? 85 : hsv.s;
    }
    sc_pod_jpg = sc_read(dir, "pod.jpg", &sc_pod_n);
    sc_sub_jpg = sc_read(dir, "sub.jpg", &sc_sub_n);
}
/* the library: the albums above, in arrays of its own (the audit's hold 13) */
static struct sl_album sc_al[SC_N];
static struct sl_track sc_tr[SC_N * 8];
static char sc_paths[SC_N * 8][32];
static int sc_atr[SC_N][8], sc_by_title[SC_N], sc_by_artist[SC_N], sc_songs[SC_N * 8], sc_rank[SC_N * 8];
static void sc_library(void)
{
    int n = 0, nart = 0;
    for (int a = 0; a < SC_NA; a++)
    {
        const struct sc_album *s = &sc_albums[a];
        sc_al[a] = (struct sl_album){ s->title, s->artist, s->year, s->n, (a * 37) % 360, sc_atr[a], s->genre };
        for (int k = 0; k < s->n; k++, n++)
        {
            sc_atr[a][k] = n; snprintf(sc_paths[n], sizeof sc_paths[n], "/Music/a%d/t%d.flac", a, k);
            sc_tr[n] = (struct sl_track){ sc_paths[n], s->songs[k], a, s->nums[k], 1, s->secs[k] * 1000L, (uint32_t)n, k % 3 == 0, NULL }; tsplit(&sc_tr[n]);
        }
        bool seen = false;
        for (int b = 0; b < a; b++) seen |= !strcmp(sc_albums[b].artist, s->artist);
        nart += !seen;
    }
    sl_lib.tracks = sc_tr; sl_lib.n_tracks = n; sl_lib.albums = sc_al; sl_lib.n_albums = SC_NA; sl_lib.n_artists = nart;
    int na = SC_NA;
    for (int i = 0; i < na; i++) sc_by_title[i] = sc_by_artist[i] = i;
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(sc_al[sc_by_title[j - 1]].title, sc_al[sc_by_title[j]].title) > 0; j--) { int x = sc_by_title[j]; sc_by_title[j] = sc_by_title[j - 1]; sc_by_title[j - 1] = x; }
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(sc_al[sc_by_artist[j - 1]].artist, sc_al[sc_by_artist[j]].artist) > 0; j--) { int x = sc_by_artist[j]; sc_by_artist[j] = sc_by_artist[j - 1]; sc_by_artist[j - 1] = x; }
    for (int i = 0; i < n; i++) sc_songs[i] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sl_sort_cmp(sc_tr[sc_songs[j - 1]].title, sc_tr[sc_songs[j]].title) > 0; j--) { int x = sc_songs[j]; sc_songs[j] = sc_songs[j - 1]; sc_songs[j - 1] = x; }
    for (int i = 0; i < n; i++) sc_rank[sc_songs[i]] = i;   /* its row in Songs, A to Z */
    sl_lib.albums_by_title = sc_by_title; sl_lib.albums_by_artist = sc_by_artist; sl_lib.songs_by_title = sc_songs;
    sl_lib.album_hash = NULL; sl_lib.album_hash_cap = 0; sl_lib.path_hash = NULL; sl_lib.path_hash_cap = 0;
    lib_epoch++;
}
/* the song called this */
static int sc_song(const char *title)
{
    for (int i = 0; i < sl_lib.n_tracks; i++) if (!strcmp(sl_lib.tracks[i].title, title)) return i;
    fprintf(stderr, "showcase: no song %s\n", title); assert(0); return -1;
}
/* playing: that song, its album queued around it */
static void sc_play(const char *title)
{
    const struct sl_track *t = &sl_lib.tracks[sc_song(title)];
    const struct sl_album *al = &sc_al[t->album];
    test_track.title = (char *)t->title; test_track.artist = (char *)al->artist; test_track.album = (char *)al->title;
    strlcpy(test_track.path, TPT(t), sizeof test_track.path);
    test_track.length = t->len; test_track.elapsed = t->len * 3 / 10;
    queued = al->n_tracks; started = 0;
    for (int i = 0; i < al->n_tracks; i++) { queue_tracks[i] = al->tracks[i]; queue_flags[i] = false; if (&sl_lib.tracks[al->tracks[i]] == t) started = i; }
}
/* the page's scrolling body: the scrollable one that scrolls furthest */
static lv_obj_t *sc_scroller(lv_obj_t *o, lv_obj_t *best)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(o) > 0 &&
        (!best || lv_obj_get_scroll_bottom(o) > lv_obj_get_scroll_bottom(best))) best = o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) best = sc_scroller(lv_obj_get_child(o, i), best);
    return best;
}
/* the lyrics picker's three LRCLIB answers come from sloppy_net.c's pretend
 * internet: read as the playing song's (titles only: no words are shown) */
static int sc_rel;   /* the song it is */
static void sc_relabel(lv_obj_t *o)
{
    const struct sl_track *tr = &sl_lib.tracks[sc_rel];
    if (lv_obj_check_type(o, &lv_label_class))
    {
        const char *t = lv_label_get_text(o);
        char b[200], len[16];
        snprintf(len, sizeof len, "%ld:%02ld", tr->len / 60000, tr->len / 1000 % 60);
        if (!strcmp(t, "Night Drive \xc2\xb7 Mira Sol") || !strcmp(t, "Night Drive (Live) \xc2\xb7 Mira Sol"))
        { snprintf(b, sizeof b, "%s \xc2\xb7 %s", tr->title, sl_lib.albums[tr->album].artist); lv_label_set_text(o, b); }
        else if (!strcmp(t, "Blue Hours \xc2\xb7 4:00")) { snprintf(b, sizeof b, "%s \xc2\xb7 %s", sl_lib.albums[tr->album].title, len); lv_label_set_text(o, b); }
        else if (!strcmp(t, "Live at the Harbor \xc2\xb7 4:22") || !strcmp(t, "4:01")) lv_label_set_text(o, len);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) sc_relabel(lv_obj_get_child(o, i));
}
/* the songs a shot shows, by their labels: none may show in two shots */
static signed char sc_shown_in[SC_N * 8];   /* the shot, +1; 0 none */
static int sc_dups;
static void sc_mark(int t, int shot, const char *const *names);
static void sc_seen(lv_obj_t *o, int shot, const char *const *names)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(o, 0) < LV_OPA_MIN) return;   /* nor anything in it */
    if (lv_obj_check_type(o, &lv_label_class) && lv_obj_is_visible(o))
    {
        const char *t = lv_label_get_text(o);
        if (strspn(t, "0123456789") == strlen(t)) return;   /* a track number, not the song "8" */
        for (int i = 0; i < sl_lib.n_tracks; i++)
        {
            size_t l = strlen(sc_tr[i].title);
            if (strncmp(t, sc_tr[i].title, l) || (t[l] && strncmp(t + l, " \xc2\xb7", 3))) continue;
            bool album = false;   /* a title that is its album's too, on that album's page */
            for (int a = 0; a < SC_NA; a++) album |= !strcmp(t, sc_al[a].title);
            if (album && strcmp(names[shot], "album-dark") && strcmp(names[shot], "coverflow-dark")) album = false;
            if (album) continue;
            sc_mark(i, shot, names);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) sc_seen(lv_obj_get_child(o, i), shot, names);
}
/* a song for a shot to play, from an album no other shot plays: one past the
 * rows the Songs shots show, shown nowhere else, not the card's album */
static bool sc_album_used[SC_N], sc_song_used[SC_N * 8];
static int sc_row0;   /* the Songs pages' first row: the top page shows rows 0-6, the four from here */
static bool sc_listed(int t) { return sc_rank[t] < 8 || (sc_rank[t] >= sc_row0 - 1 && sc_rank[t] < sc_row0 + 32); }
static void sc_mark(int t, int shot, const char *const *names)
{
    if (sc_shown_in[t] && sc_shown_in[t] != shot + 1)
    { fprintf(stderr, "showcase: \"%s\" in %s and %s\n", sc_tr[t].title, names[sc_shown_in[t] - 1], names[shot]); sc_dups++; }
    sc_shown_in[t] = (signed char)(shot + 1);
}
static int sc_pick(int not_album, int *next)
{
    for (int tries = 0; tries < SC_NA; tries++)
    {
        int a = (*next)++ % SC_NA;
        if (sc_album_used[a] || a == not_album) continue;
        for (int k = sc_al[a].n_tracks - 1; k >= 0; k--)
        {
            int t = sc_al[a].tracks[k];
            if (sc_listed(t) || sc_song_used[t]) continue;
            sc_album_used[a] = sc_song_used[t] = true;
            return t;
        }
    }
    /* every album playing already (the feature shots came on top): a song not
     * yet shown, from any album but the one on the screen */
    for (int a = 0; a < SC_NA; a++)
    {
        if (a == not_album) continue;
        for (int k = sc_al[a].n_tracks - 1; k >= 0; k--)
        {
            int t = sc_al[a].tracks[k];
            if (sc_listed(t) || sc_song_used[t]) continue;
            sc_song_used[t] = true;
            return t;
        }
    }
    fprintf(stderr, "showcase: no song left to play\n"); assert(0); return -1;
}
static void showcase_shots(const char *covers, const char *out)
{
    /* every song on one screen only, and each playing one from an album of
     * its own (NULL: picked); the five taste cards one song each; the Songs
     * list at the top, then four pages from row sc_row0, eight rows (82 apart)
     * further each time: six whole rows a page, and the rows cut at a page's
     * foot and head shown on that page alone */
    static const struct { const char *file, *screen; int look; const char *play, *card; int page; } shots[] = {
        { "all-rail-dark", "songs", 0 },
        { "all-rail-p1", "songs", 0, NULL, NULL, 1 }, { "all-rail-p2", "songs", 0, NULL, NULL, 2 },
        { "all-rail-p3", "songs", 0, NULL, NULL, 3 }, { "all-rail-p4", "songs", 0, NULL, NULL, 4 },
        { "control-centre-dark", "control-centre", 0 },   /* before the equalizer's shot turns it on */
        { "tiles-home-dark", "home", 2 }, { "cards-home-dark", "home", 0 }, { "list-home-zule", "home", 6 },
        { "music-zule", "music", 6 },
        { "now-playing-dark", "now-playing", 0, "Child Psychology" }, { "now-playing-nelly-dark", "now-playing", 0, "Say It Right" }, { "now-playing-umbrella-dark", "now-playing", 0, "Umbrella" },
        { "album-hmhas-dark", "album", 0, "BIRDS OF A FEATHER" },
        /* the front page's main features: the queue, crossfade, Cover Flow held sideways, the games */
        { "queue-dark", "queue", 0, "The Grants" },
        { "crossfade-dark", "settings-playback", 0, "Teardrop" },
        { "coverflow-landscape-dark", "coverflow", 0, "Here Comes Your Man" },
        { "playroom-dark", "playroom", 0, "Karma Police" }, { "chess-dark", "chess", 0, "Let Down" },
        { "2048-dark", "2048", 0, "Green Light" },
        { "album-dark", "album", 0 },
        { "eq-autoeq-dark", "settings-eq-autoeq", 0 },
        { "taste-dark", "taste", 0, NULL, "Kintsugi" },
        { "taste-1-dark", "taste", 0, NULL, "Bored" }, { "taste-2-dark", "taste", 0, NULL, "Dealer" },
        { "taste-3-dark", "taste", 0, NULL, "The Facts of Life" }, { "taste-4-dark", "taste", 0, NULL, "Baby Bitch" },
        { "taste-5-dark", "taste", 0, NULL, "Who'd Have Known" },
        { "waveform-dark", "now-playing-waveform", 0 },
        { "podcast-show-dark", "podcast-show", 0 }, { "subsonic-album-dark", "subsonic-album", 0 },
        { "ebook-reader-dark", "ebook-reader-sepia", 0 },
        { "tiles-home-light", "home", 3 }, { "settings-dark", "settings", 0 },
        { "lyrics-picker-dark", "lyrics-picker", 0 }, { "arcade-dark", "arcade-list", 0 },
        { "coverflow-dark", "coverflow", 0 },
    };
    enum { NS = sizeof shots / sizeof shots[0] };
    const char *names[NS];
    for (int i = 0; i < NS; i++) names[i] = shots[i].file;
    char p[700]; snprintf(p, sizeof p, "mkdir -p '%s'", out); system(p);
    sc_load(covers); sc_on = true; test_real_art = true;
    if (!np_img_buf) np_img_buf = calloc(W * W, 2);   /* Now Playing's cover, as the suite sets it up later */
    au_library(); sc_library();
    /* the songs asked for, and the album page's first rows, kept for their shots */
    int play[NS], next = 1;
    for (int i = 0; i < NS; i++)
    {
        /* the hero's Now Playing (Umbrella) may show in Songs too: next to it is
         * where it belongs, and the Songs pages had no run clear of it */
        /* (and the main features' shots, made on top of the rest) */
        if (shots[i].play && (strstr(" now-playing-umbrella-dark album-hmhas-dark queue-dark crossfade-dark coverflow-landscape-dark playroom-dark chess-dark 2048-dark ",
                                     shots[i].file) != NULL))
            sc_album_used[sc_tr[sc_song(shots[i].play)].album] = true;
        else if (shots[i].play) { int t = sc_song(shots[i].play); sc_album_used[sc_tr[t].album] = sc_song_used[t] = true; }
        if (shots[i].card) sc_song_used[sc_song(shots[i].card)] = true;
    }
    for (int k = 0; k < 4; k++) sc_song_used[sc_al[0].tracks[k]] = true;
    /* the Songs pages: a run of rows with none of those in it */
    for (sc_row0 = 9; sc_row0 + 32 < sl_lib.n_tracks; sc_row0++)
    {
        bool clear = true;
        for (int t = 0; t < sl_lib.n_tracks; t++) clear &= !(sc_song_used[t] && sc_listed(t));
        if (clear) break;
    }
    for (int t = 0; t < sl_lib.n_tracks; t++) assert(!(sc_song_used[t] && sc_listed(t)));
    for (int i = 0; i < NS; i++)
        play[i] = !strcmp(shots[i].screen, "coverflow") && !shots[i].play ? sc_al[0].tracks[0] :
                  shots[i].play ? sc_song(shots[i].play) : sc_pick(shots[i].card ? sc_tr[sc_song(shots[i].card)].album : -1, &next);
    for (int i = 0; i < NS; i++)
    {
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(shots[i].file, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        au_apply(shots[i].look, 0, 0);
        sc_play(sc_tr[play[i]].title);
        /* upright, as the page's phone is; the landscape one as Cover Flow is held */
        global_settings.sloppy_cf_vertical = !strcmp(shots[i].screen, "coverflow") && !strstr(shots[i].file, "landscape");
        if (!strcmp(shots[i].screen, "home")) au_open_home(AU_HOME);
        else
        {
            if (!strcmp(shots[i].file, "crossfade-dark")) xf_apply(6, true);   /* on: six seconds, skips too */
            test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0; au_open(au_find(shots[i].screen)); au_settle();
        }
        if (!strcmp(shots[i].file, "chess-dark"))   /* a game under way, not the new-game sheet */
        {
            chess_start(0, 0, 1); chess_sheet_close_now(false); frame(300);
            /* the Italian: 1. e4 e5 2. Nf3 Nc6 3. Bc4 Nf6, white to move */
            static const int mv[][4] = { {4,1,4,3}, {4,6,4,4}, {6,0,5,2}, {1,7,2,5}, {5,0,2,3}, {6,7,5,5} };
            for (int k = 0; k < 6; k++)
            {
                struct sl_chess_move all[256];
                int n = sl_chess_legal(&chess.pos, all, false), found = -1;
                for (int j = 0; j < n; j++)
                    if (all[j].from == CH_SQ(mv[k][0], mv[k][1]) && all[j].to == CH_SQ(mv[k][2], mv[k][3])) found = j;
                assert(found >= 0);
                chess_play_move(&all[found], false); frame(200);
            }
            au_settle();
        }
        if (!strcmp(shots[i].file, "album-hmhas-dark"))   /* the hero's album page: its own album, not the first */
        { page_pop(); frame(400); album_page(sc_tr[play[i]].album); au_settle(); }
        if (!strncmp(shots[i].screen, "now-playing", 11)) { np_glow_refresh(true); frame(300); }   /* the glass, made from this cover, not the last one */
        if (!strcmp(shots[i].screen, "lyrics-picker")) { sc_rel = play[i]; sc_play(sc_tr[play[i]].title); sc_relabel(lv_screen_active()); sc_relabel(lv_layer_top()); frame(100); }
        if (shots[i].card && td.n)   /* the top card: this song */
        {
            td.deck[td.at] = sc_song(shots[i].card); td.why[td.at] = TM_WHY_NEW;
            td_stack_build(false); au_settle();
        }
        if (shots[i].page)
        {
            lv_obj_t *b = sc_scroller(pages[npages - 1], NULL);
            if (b) { lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 82 * (sc_row0 + 8 * (shots[i].page - 1)), LV_ANIM_OFF); au_settle(); }
        }
        if (!np_open && strcmp(shots[i].screen, "coverflow") && lv_obj_has_flag(mini, LV_OBJ_FLAG_HIDDEN)) { mini_update(); au_settle(); }   /* the podcast and server pages' setup left it hidden */
        frame(0);
        sc_seen(lv_screen_active(), i, names); sc_seen(lv_layer_top(), i, names);
        if (shots[i].card) sc_mark(sc_song(shots[i].card), i, names);   /* drawn as a picture: no label to find */
        printf("showcase: %-22s %s - %s\n", shots[i].file, test_track.title, test_track.artist);
        if (!strcmp(shots[i].file, "crossfade-dark")) xf_apply(0, false);
        snprintf(p, sizeof p, "%s/%s.ppm", out, shots[i].file); shot_now(p);
        au_reset(); au_empty(false);
    }
    printf("showcase: %d songs shown in two shots\n", sc_dups);
    global_settings.sloppy_cf_vertical = 0;
    au_library_done();
}
