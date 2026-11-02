/* SLOPPY_SHOWCASE=<covers dir> [SLOPPY_SHOWCASE_OUT=<dir>]: the shots on the
 * GitHub front page, with real songs, artists and sleeves in place of the
 * audit library's made-up ones. The covers are not in the repo: <dir> holds
 * 00.jpg..09.jpg (the albums below, in order), pod.jpg (the podcast's) and
 * sub.jpg (the music server's album). Writes <out>/<name>.ppm and stops. */
static const struct sc_album { const char *title, *artist, *genre; int year, first; const char *songs[8]; int secs[8], nums[8]; } sc_albums[10] = {
    { "England Made Me", "Black Box Recorder", "Pop", 1998, 1,
      { "Girl Singing in the Wreckage", "England Made Me", "New Baby Boom", "It's Only the End of the World", "Ideal Home",
        "Child Psychology", "I. C. One Female", "Up Town Top Ranking" }, { 162, 240, 130, 321, 159, 248, 139, 237 } },
    { "When We All Fall Asleep, Where Do We Go?", "Billie Eilish", "Pop", 2019, 2,
      { "bad guy", "xanny", "you should see me in a crown", "all the good girls go to hell", "wish you were gay",
        "when the party's over", "8", "my strange addiction" }, { 194, 244, 181, 169, 222, 196, 173, 180 } },
    { "Born to Die", "Lana Del Rey", "Pop", 2012, 1,
      { "Born to Die", "Off to the Races", "Blue Jeans", "Video Games", "Diet Mountain Dew", "National Anthem", "Dark Paradise", "Radio" },
      { 286, 300, 210, 282, 223, 231, 244, 214 } },
    { "Random Access Memories", "Daft Punk", "Electronic", 2013, 1,
      { "Give Life Back to Music", "The Game of Love", "Giorgio by Moroder", "Within", "Instant Crush", "Lose Yourself to Dance",
        "Touch", "Get Lucky" }, { 274, 321, 544, 228, 337, 353, 498, 367 } },
    { "Rumours", "Fleetwood Mac", "Rock", 1977, 1,
      { "Second Hand News", "Dreams", "Never Going Back Again", "Don't Stop", "Go Your Own Way", "Songbird", "The Chain",
        "You Make Loving Fun" }, { 163, 254, 122, 191, 218, 200, 268, 211 } },
    { "OK Computer", "Radiohead", "Rock", 1997, 1,
      { "Airbag", "Paranoid Android", "Subterranean Homesick Alien", "Exit Music (for a Film)", "Let Down", "Karma Police",
        "Fitter Happier", "Electioneering" }, { 284, 383, 267, 264, 299, 261, 117, 230 } },
    { "Hounds of Love", "Kate Bush", "Pop", 1985, 1,
      { "Running Up That Hill (A Deal with God)", "Hounds of Love", "The Big Sky", "Mother Stands for Comfort", "Cloudbusting",
        "And Dream of Sheep", "Under Ice", "Waking the Witch" }, { 300, 182, 281, 186, 304, 166, 141, 259 } },
    { "Currents", "Tame Impala", "Psychedelic", 2015, 1,
      { "Let It Happen", "Nangs", "The Moment", "Yes I'm Changing", "Eventually", "Gossip", "The Less I Know the Better", "Past Life" },
      { 467, 107, 255, 270, 318, 55, 216, 228 } },
    { "Blonde", "Frank Ocean", "R&B", 2016, 1,
      { "Nikes", "Ivy", "Pink + White", "Be Yourself", "Solo", "Skyline To", "Self Control", "Good Guy" },
      { 314, 249, 184, 86, 257, 184, 249, 66 } },
    { "Loose", "Nelly Furtado", "Pop", 2006, 1,   /* the US CD, its interludes left out */
      { "Afraid", "Maneater", "Promiscuous", "Glow", "Showtime", "No Hay Igual", "Say It Right", "Do It" },
      { 219, 260, 244, 244, 246, 217, 225, 223 }, { 1, 2, 4, 5, 6, 8, 10, 11 } },
};
#define SC_PLAYING 5   /* Child Psychology, England Made Me's sixth */
#define SC_NELLY 9, 6   /* now-playing-nelly: Say It Right, from Loose */

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
    for (int a = 0; a < 10; a++)
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
/* the audit library (au_library), its ten albums renamed */
static void sc_library(void)
{
    for (int a = 0; a < 10; a++)
    {
        const struct sc_album *s = &sc_albums[a];
        au_al[a].title = s->title; au_al[a].artist = s->artist; au_al[a].genre = s->genre; au_al[a].year = s->year;
        for (int k = 0; k < 8; k++)
        {
            struct sl_track *t = &sl_lib.tracks[au_al[a].tracks[k]];
            t->title = s->songs[k]; t->num = s->nums[k] ? s->nums[k] : s->first + k; t->len = s->secs[k] * 1000L; t->artist = NULL;
        }
    }
    int na = sl_lib.n_albums, n = sl_lib.n_tracks;
    for (int i = 0; i < na; i++) au_by_title[i] = au_by_artist[i] = i;
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(au_al[au_by_title[j - 1]].title, au_al[au_by_title[j]].title) > 0; j--) { int x = au_by_title[j]; au_by_title[j] = au_by_title[j - 1]; au_by_title[j - 1] = x; }
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(au_al[au_by_artist[j - 1]].artist, au_al[au_by_artist[j]].artist) > 0; j--) { int x = au_by_artist[j]; au_by_artist[j] = au_by_artist[j - 1]; au_by_artist[j - 1] = x; }
    for (int i = 0; i < n; i++) au_songs[i] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sl_sort_cmp(au_tr[au_songs[j - 1]].title, au_tr[au_songs[j]].title) > 0; j--) { int x = au_songs[j]; au_songs[j] = au_songs[j - 1]; au_songs[j - 1] = x; }
    lib_epoch++;
}
/* playing: album a's song k, the album queued around it */
static void sc_play(int a, int k)
{
    const struct sl_track *t = &sl_lib.tracks[au_al[a].tracks[k]];
    test_track.title = (char *)t->title; test_track.artist = (char *)au_al[a].artist; test_track.album = (char *)au_al[a].title;
    strlcpy(test_track.path, TPT(t), sizeof test_track.path);
    test_track.length = t->len; test_track.elapsed = t->len * 3 / 10;
    queued = 8; started = k; for (int i = 0; i < 8; i++) { queue_tracks[i] = au_al[a].tracks[i]; queue_flags[i] = false; }
}
/* the lyrics picker's three LRCLIB answers come from sloppy_net.c's pretend
 * internet: read as Child Psychology's (titles only: no words are shown) */
static const char *const sc_lrclib[][2] = {
    { "Night Drive \xc2\xb7 Mira Sol", "Child Psychology \xc2\xb7 Black Box Recorder" },
    { "Night Drive (Live) \xc2\xb7 Mira Sol", "Child Psychology \xc2\xb7 Black Box Recorder" },
    { "Blue Hours \xc2\xb7 4:00", "England Made Me \xc2\xb7 4:08" }, { "Live at the Harbor \xc2\xb7 4:22", "4:08" }, { "4:01", "4:10" },
};
static void sc_relabel(lv_obj_t *o)
{
    if (lv_obj_check_type(o, &lv_label_class))
        for (unsigned i = 0; i < sizeof sc_lrclib / sizeof sc_lrclib[0]; i++)
            if (!strcmp(lv_label_get_text(o), sc_lrclib[i][0])) { lv_label_set_text(o, sc_lrclib[i][1]); break; }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) sc_relabel(lv_obj_get_child(o, i));
}
static void showcase_shots(const char *covers, const char *out)
{
    static const struct { const char *file, *screen; int look; } shots[] = {
        { "control-centre-dark", "control-centre", 0 },   /* before the equalizer's shot turns it on */
        { "tiles-home-dark", "home", 2 }, { "cards-home-dark", "home", 0 }, { "list-home-zule", "home", 6 },
        { "music-zule", "music", 6 }, { "now-playing-dark", "now-playing", 0 }, { "now-playing-nelly-dark", "now-playing", 0 }, { "album-dark", "album", 0 },
        { "eq-autoeq-dark", "settings-eq-autoeq", 0 }, { "taste-dark", "taste", 0 }, { "waveform-dark", "now-playing-waveform", 0 },
        { "podcast-show-dark", "podcast-show", 0 }, { "subsonic-album-dark", "subsonic-album", 0 },
        { "ebook-reader-dark", "ebook-reader-sepia", 0 },
        { "all-rail-dark", "songs", 0 }, { "tiles-home-light", "home", 3 },
        { "settings-dark", "settings", 0 }, { "lyrics-picker-dark", "lyrics-picker", 0 }, { "arcade-dark", "arcade-list", 0 }, { "coverflow-dark", "coverflow", 0 },
    };
    char p[700]; snprintf(p, sizeof p, "mkdir -p '%s'", out); system(p);
    sc_load(covers); sc_on = true; test_real_art = true;
    if (!np_img_buf) np_img_buf = calloc(W * W, 2);   /* Now Playing's cover, as the suite sets it up later */
    au_library(); sc_library();
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++)
    {
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(shots[i].file, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        au_apply(shots[i].look, 0, 0);
        if (strstr(shots[i].file, "nelly")) sc_play(SC_NELLY); else sc_play(0, SC_PLAYING);
        global_settings.sloppy_cf_vertical = !strcmp(shots[i].screen, "coverflow");   /* upright, as the page's phone is */
        if (!strcmp(shots[i].screen, "home")) au_open_home(AU_HOME);
        else { test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0; au_open(au_find(shots[i].screen)); au_settle(); }
        if (!strncmp(shots[i].screen, "now-playing", 11)) { np_glow_refresh(true); frame(300); }   /* the glass, made from this cover, not the last one */
        if (!strcmp(shots[i].screen, "lyrics-picker")) { sc_relabel(lv_screen_active()); sc_relabel(lv_layer_top()); frame(100); }
        frame(0);
        snprintf(p, sizeof p, "%s/%s.ppm", out, shots[i].file); shot_now(p);
        au_reset(); au_empty(false);
    }
    global_settings.sloppy_cf_vertical = 0;
    au_library_done();
}
