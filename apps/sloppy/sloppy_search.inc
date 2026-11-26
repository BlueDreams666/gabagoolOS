/* GabagoolOS search. GPLv2+. Included by sloppy.c after the history.
 * Letters come from the keyboard (sloppy_kb.inc: typed, glided, in either
 * of the two chosen languages) or from a letter drawn with a finger.
 * Results narrow as each letter is added: artists, albums, then songs (a
 * bounded list that plays from the tapped row). */
#include "sloppy_search_state.h"
#include "sloppy_swipe_type.h"

#define SQ_BAR_H 72
#define SQ_KB_H KB_H
#define SQ_SONGS_MAX 300

/* A magnifying glass, drawn: a ring and a stubby handle. The symbol font
 * has no search glyph, and the eye that stood in for it read as "view". */
static lv_obj_t *sq_glass(lv_obj_t *parent, uint32_t color)
{
    static const lv_point_precise_t handle[] = { { 15, 15 }, { 21, 21 } };
    lv_obj_t *g = box(parent);
    lv_obj_set_size(g, 24, 24);
    lv_obj_t *ring = box(g);
    lv_obj_set_size(ring, 18, 18);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_t *h = lv_line_create(g);
    lv_line_set_points(h, handle, 2);
    lv_obj_set_style_line_width(h, 4, 0);
    lv_obj_set_style_line_rounded(h, true, 0);
    lv_obj_set_style_line_color(h, lv_color_hex(color), 0);
    return g;
}

static lv_obj_t *sq_page, *sq_body, *sq_ta, *sq_kb, *sq_pad, *sq_pad_box;
static struct kb *sq_kbd;              /* the keyboard (sloppy_kb.inc); sq_kb is its root */
static bool sq_drawing;
static lv_obj_t *sq_ink[24];           /* one line object per drawn stroke */
static lv_point_precise_t *sq_ink_pts[24];
static int sq_ink_n[24];
static char sq_text[SL_Q_MAX];
static bool sq_kb_shown;
static lv_timer_t *sq_timer;           /* debounced search, and letter recognition */
static uint32_t sq_search_at, sq_recognize_at;
static struct sl_pt *sq_pts;           /* the strokes being drawn */
static int sq_npts, sq_stroke;
static struct sl_p_template *sq_templates;
static int sq_ntemplates;

/* Search covers music, or (Settings' search bubble) the settings themselves */
enum { SQ_SCOPE_MUSIC, SQ_SCOPE_SETTINGS };
static int sq_scope;

static bool search_hides_mini(void)
{
    return sq_page && sq_kb_shown && npages && pages[npages - 1] == sq_page;
}

/* ------------------------------------------------------------ results */
static int *sq_scores;
static int sq_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (sq_scores[x] != sq_scores[y]) return sq_scores[y] - sq_scores[x];
    return strcasecmp(sl_lib.tracks[x].title, sl_lib.tracks[y].title);
}

static void sq_artist_cb(lv_event_t *e) { artist_page((int)(intptr_t)lv_event_get_user_data(e)); }
static void sq_album_cb(lv_event_t *e) { album_page((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *sq_result_row_in(lv_obj_t *parent, const char *t1, const char *t2, int album, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_size(r, lv_pct(100), 78);
    lv_obj_t *c;
    if (album >= 0) c = cover(r, album, 58, false);
    else
    {
        /* a playlist: its tile, rounded as a cover is */
        c = lit_square(r, SW_PLAYLIST_COLOR, 58);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_center(text(c, LV_SYMBOL_LIST, &sl_sym_28, 0xfefefe));
    }
    sl_align(c, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *a = text(r, t1, &ss_semibold_23, 0xffffff);
    sl_place(a, 80, 10);
    lv_obj_set_size(a, W - 28 - 120, 29);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_t *b = text(r, t2, &ss_regular_18, C_SUB);
    sl_place(b, 80, 42);
    lv_obj_set_size(b, W - 28 - 120, 24);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    lv_obj_t *ch = text(r, sl_chevron(), &sl_sym_22, 0xffffff);
    sl_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
    on_click(r, cb, (void *)ud);
    return r;
}
static lv_obj_t *sq_result_row(const char *t1, const char *t2, int album, lv_event_cb_t cb, intptr_t ud)
{
    return sq_result_row_in(sq_body, t1, t2, album, cb, ud);
}

/* "See all": every artist or album the last search found, past the four
 * it shows (an artist is its first index in albums_by_artist) */
#define SQ_ALL_MAX 120               /* a grid of covers, not a virtual one: kept small for the R1 */
static int *sq_all_artists, *sq_all_albums, sq_n_all_artists, sq_n_all_albums;
static void sq_artist_line(int k, char *b, size_t n)
{
    int albums_n = 0;
    const char *name = sl_lib.albums[sl_lib.albums_by_artist[k]].artist;
    while (k + albums_n < sl_lib.n_albums && !strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k + albums_n]].artist, name))
        albums_n++;
    char a[40];
    snprintf(b, n, "%s \xc2\xb7 %s", sl_tr("Artist"), sl_count(a, sizeof a, albums_n, "%s album", "%s albums"));
}
static void sq_see_all_cb(lv_event_t *e)
{
    bool artists = (intptr_t)lv_event_get_user_data(e) == 0;
    int n = artists ? sq_n_all_artists : sq_n_all_albums;
    lv_obj_t *p, *body = page_new(artists ? "Artists" : "Albums", &p);
    char b[96];
    snprintf(b, sizeof b, "\xe2\x80\x9c%s\xe2\x80\x9d \xc2\xb7 %d", sq_text, n);
    section(body, b);
    if (artists)
        for (int i = 0; i < n; i++)
        {
            int k = sq_all_artists[i];
            sq_artist_line(k, b, sizeof b);
            sq_result_row_in(body, sl_lib.albums[sl_lib.albums_by_artist[k]].artist, b, sl_lib.albums_by_artist[k], sq_artist_cb, k);
        }
    else album_grid(body, sq_all_albums, n);
    page_push(p);
}
static void sq_see_all_row(int n, bool artists)
{
    char b[96];
    snprintf(b, sizeof b, "%s %d %s", sl_tr("See all"), n, artists ? sl_tr_count(n, "artist", "artists") : sl_tr_count(n, "album", "albums"));
    lv_obj_t *r = box(sq_body);
    lv_obj_set_size(r, lv_pct(100), 52);
    lv_obj_t *l = text(r, b, &ss_semibold_21, C_ACCENT);
    sl_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    sl_align(text(r, sl_chevron(), &sl_sym_22, C_ACCENT), LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, SL_R_MD, 0);
    on_click(r, sq_see_all_cb, (void *)(intptr_t)(artists ? 0 : 1));
}

/* ---------------------------------------------------- settings index */
struct sq_setting { const char *name, *where, *words; intptr_t code; };
static const struct sq_setting sq_settings[] = {
    { "Bluetooth", "Connections", "headphones pair speaker audio output codec", ACT_BLUETOOTH },
    { "Wi-Fi", "Connections", "internet network airplay dlna transfer clock", 0x20012 },
    { "Scrobbling", "Connections", "listenbrainz last.fm lastfm scrobble scrobbler log token listens", 0x2001b },
    { "Send to Last.fm", "Scrobbling", "last.fm lastfm scrobble sign in account qr", 0x2001b },
    { "Software update", "Device", "update upgrade firmware version new install gbupdate previous rollback undo ota", 0x2002b },
    { "Music servers", "Connections", "subsonic opensubsonic navidrome airsonic gonic server stream offline download audiobookshelf abs audiobook sync", 0x2002a },
    { "Receive with LocalSend", "Wi-Fi", "localsend airdrop send phone receive transfer qr code upload", 0x20026 },
    { "Transfer music", "Wi-Fi", "upload browser computer drag drop folder qr code", 0x20026 },
    { "Crossfade", "Playback", "fade gap gapless skip", 0x20002 },
    { "Equalizer", "Playback", "eq bands presets bass treble peq parametric preamp autoeq apo hiby json", 0x20001 },
    { "Play through albums", "Playback", "next album folders now playing opens", 0x20002 },
    { "Volume", "Sound", "maximum limit loudness usb dac computer", 0x2000d },
    { "Volume button step", "Sound", "keys increment step db", 0x2000d },
    { "Power-on volume", "Sound", "boot start fixed", 0x2000d },
    { "Gain", "Sound", "low high output digital maximum dsd", 0x2000d },
    { "Tone", "Sound", "bass treble balance", 0x2000d },
    { "Tone tuning", "Sound", "mseb warm warmer bright brighter temperature deep bass texture thickness vocals sibilance air hiby", 0x2001f },
    { "ReplayGain", "Sound", "loudness normalize preamp clipping", 0x2000d },
    { "Playback speed", "Sound", "faster slower pitch audiobook", 0x2000d },
    { "Crossfeed", "Sound", "stereo width soundfield", 0x2000d },
    { "Mono", "Sound", "mono one channel single ear left right stereo", 0x2000d },
    { "DAC filter", "Sound", "dither hi-res sharp slow nos non-oversampling oversampling", 0x2000d },
    { "Sound effects", "Sound", "interface taps clicks game notification", 0x2000a },
    { "Theme", "Customization", "dark light true black zule oled", 0x20004 },
    { "Rounded corners", "Theme", "screen corners round rounded square edges phone", 0x20004 },
    { "Accent color", "Customization", "colour wheel tint", 0x20005 },
    { "Home layout", "Customization", "cards tiles list pull refresh", 0x20009 },
    { "Settings icon", "Customization", "cog skull", 0x20008 },
    { "Settings layout", "Customization", "list tiles grid boxes squares rows", 0x2001d },
    { "Big icons", "Customization", "large larger bigger size icons rows tiles settings", 0x2001d },
    { "Quality badges", "Library options", "hi-res lossless lossy cd dsd flac badge covers", 0x2001e },
    { "Show artist", "Library options", "album grid artist name covers", 0x2001e },
    { "Home wallpaper", "Customization", "background picture photo screenshot your own custom", 0x20006 },
    { "Lock wallpaper", "Customization", "background lock screen picture photo screenshot your own custom", 0x20007 },
    { "Lock screen player", "Customization", "cover art big small", 0x2000b },
    { "Now Playing style", "Customization", "glass classic black panel centred centered blur", 0x20016 },
    { "Status bar", "Customization", "now playing clock battery volume time", 0x20016 },
    { "Show the album", "Now Playing", "album line title artist record song lines three", 0x20016 },
    { "Loved songs", "Customization", "heart star favourite favorite love", 0x20016 },
    { "Cover Flow", "Customization", "vertical horizontal upright sideways albums", 0x2001a },
    { "USB mode screen", "Customization", "usb computer dog gabagool classic plug picture", 0x2002c },
    { "Lock screen", "Customization", "lock screen off disable swipe unlock no lock wake straight", 0x2000b },
    { "Screensaver", "Customization", "screen saver idle floating cover square album clock burn in always on display", 0x20028 },
    { "Language", "Customization", "english spanish french german hebrew arabic chinese", 0x20013 },
    { "Text size", "Customization", "font large bigger smaller", 0x20014 },
    { "Keyboards", "Customization", "keyboard layout globe second language qwertz azerty pinyin hebrew arabic russian", 0x20020 },
    { "Swipe typing", "Keyboards", "glide keyboard gesture type", 0x20020 },
    { "Phone keypad (T9)", "Keyboards", "t9 phone keypad numbers multi tap multitap predictive old nokia twelve keys", 0x20020 },
    { "Lyrics color", "Customization", "colour sung line", 0x20011 },
    { "Lyrics size", "Customization", "lyrics text bigger smaller font large", 0x20011 },
    { "Screen sleep", "Display", "timeout display off lock", 0x2000c },
    { "Brightness", "Display", "dim darker night dark screen backlight light", 0x20021 },
    { "Buttons", "Controls", "play key next key hold previous", 0x2000e },
    { "Headset controls", "Controls", "remote bluetooth wired", 0x2000e },
    { "Double tap to wake", "Controls", "touch tap twice wake screen off dark keys", 0x2000e },
    { "Upside down", "Buttons", "rotate rotation turn 180 flip upside down orientation left handed keys other side", 0x2000e },
    { "Notification Center", "Controls", "shortcuts control centre quick", 0x2000f },
    { "Notifications", "Controls", "toasts banners history alerts messages", 0x2000f },
    { "Clock", "Time and power", "time 24-hour automatic", 0x20010 },
    { "Clock position", "Time and power", "status bar clock left centre center middle right time place", 0x20010 },
    { "Time zone", "Time and power", "timezone city", 0x20010 },
    { "Auto power off", "Time and power", "shutdown idle sleep timer", 0x20010 },
    { "Battery", "Time and power", "percentage charge care limit led", 0x20010 },
    { "Car mode", "Time and power", "car vehicle drive driving usb power auto play resume turn off big buttons", 0x20029 },
    { "Battery history", "Time and power", "battery log graph drain hours left voltage history usage", 0x20010 },
    { "USB", "Time and power", "dac transfer files charge adb debugging", 0x20010 },
    { "Library", "Device", "songs albums artists music", ACT_LIBRARY },
    { "Update database", "Library", "scan rescan new music", ACT_REBUILD },
    { "Lyrics and covers", "Library", "lyrics lrclib lrc words covers cover art album art musicbrainz download fetch", 0x20025 },
    { "Spotify playlists", "Playlists", "spotify import match top artists", 0x20018 },
    { "Open Classic Rockbox", "Advanced", "rockbox classic menus", ACT_CLASSIC },
    { "Dual boot screen", "Advanced", "boot menu hiby tools", 0x20015 },
    { "Reset all settings", "Advanced", "defaults factory restore", 0x20019 },
    { "Erase GabagoolOS data", "Advanced", "factory reset erase wipe clear history taste cache start fresh new", 0x20027 },
};
static void sq_setting_cb(lv_event_t *e) { card_row_open(sq_settings[(intptr_t)lv_event_get_user_data(e)].code); }
/* a result wears the icon of the Settings row it is under */
static const struct set_entry *sq_setting_entry(const struct sq_setting *st)
{
    for (unsigned k = 0; k < ARRAYLEN(set_entries); k++)
        if (set_entries[k].act == st->code) return &set_entries[k];
    for (unsigned k = 0; k < ARRAYLEN(set_entries); k++)
        if (!strcmp(set_entries[k].name, st->where)) return &set_entries[k];
    return &set_entries[ARRAYLEN(set_entries) - 1];   /* Advanced's cog */
}
static void sq_run_settings(char *words[], int nw)
{
    int shown = 0;
    lv_obj_t *g = NULL;
    char name[128], rest[192];
    for (int i = 0; i < (int)ARRAYLEN(sq_settings); i++)
    {
        const struct sq_setting *st = &sq_settings[i];
        sl_q_fold(sl_tr(st->name), name, sizeof name);
        char both[160];
        snprintf(both, sizeof both, "%s %s", st->words, st->name);
        sl_q_fold(both, rest, sizeof rest);
        if (nw && !sl_q_score(words, nw, name, rest, "")) continue;
        if (!g) g = set_group(sq_body, NULL);
        const struct set_entry *se = sq_setting_entry(st);
        set_action_sub(g, se->sym, se->tint, st->name, sl_tr(st->where), sq_setting_cb, (void *)(intptr_t)i);
        shown++;
    }
    if (!shown)
    {
        char b[64];
        snprintf(b, sizeof b, "Nothing matches \"%s\"", sq_text);
        opt_note(sq_body, b);
    }
}

/* playlists by name: the folder read once a search page, kept while it is open */
static char **sq_pls;
static int sq_pls_n = -1;
static void sq_pls_free(void)
{
    for (int i = 0; i < sq_pls_n; i++) free(sq_pls[i]);
    free(sq_pls);
    sq_pls = NULL;
    sq_pls_n = -1;
}
static void sq_playlist_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char full[MAX_PATH];
    if (i >= 0 && i < sq_pls_n && pls_full(full, sizeof full, sq_pls[i])) pls_page(full);
}
#define SQ_PLS_SHOWN 6
static void sq_run_playlists(char *words[], int nw)
{
    if (sq_pls_n < 0) sq_pls_n = pls_list_names(&sq_pls);
    int hit[SQ_PLS_SHOWN], n = 0, all = 0;
    char f[256], nm[MAX_PATH];
    for (int i = 0; i < sq_pls_n; i++)
    {
        strlcpy(nm, sq_pls[i], sizeof nm);
        char *dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = '\0';
        sl_q_fold(nm, f, sizeof f);
        if (!sl_q_score(words, nw, f, "", "")) continue;
        if (n < SQ_PLS_SHOWN) hit[n++] = i;
        all++;
    }
    if (!n) return;
    char b[64], num[16];
    if (all > n) snprintf(b, sizeof b, sl_tr("PLAYLISTS · %d OF %s"), n, sl_fmt_count(num, sizeof num, all));
    else snprintf(b, sizeof b, sl_tr("PLAYLISTS · %d"), n);
    section(sq_body, b);
    for (int k = 0; k < n; k++)
    {
        strlcpy(nm, sq_pls[hit[k]], sizeof nm);
        char *dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = '\0';
        lv_obj_t *r = sq_result_row("", "Playlist", -1, sq_playlist_cb, hit[k]);
        lv_label_set_text(lv_obj_get_child(r, 1), nm);   /* its name: never translated */
    }
}
static void sq_run(void)
{
    uint32_t started = sl_ms();
    lv_obj_clean(sq_body);
    char q[SL_Q_MAX], *words[6];
    sl_q_fold(sq_text, q, sizeof q);
    int nw = sl_q_words(q, words);
    if (sq_scope == SQ_SCOPE_SETTINGS) { sq_run_settings(words, nw); return; }
    if (!nw) return;
    int n = sl_lib.n_tracks;
    int *scores = calloc(n ? n : 1, sizeof *scores);
    int *songs = malloc((n ? n : 1) * sizeof *songs);
    int *album_hit = calloc(sl_lib.n_albums ? sl_lib.n_albums : 1, sizeof *album_hit);
    if (!scores || !songs || !album_hit)
    { free(scores); free(songs); free(album_hit); toast_warn("Not enough memory to search"); return; }

    /* tracks of an album usually sit together: fold its names only when
     * the album changes */
    char t[256], ar[256], al[256];
    int count = 0, folded_album = -1;
    for (int i = 0; i < n; i++)
    {
        const struct sl_track *tr = &sl_lib.tracks[i];
        sl_q_fold(tr->title, t, sizeof t);
        if (tr->album != folded_album)
        {
            folded_album = tr->album;
            sl_q_fold(sl_lib.albums[tr->album].artist, ar, sizeof ar);
            sl_q_fold(sl_lib.albums[tr->album].title, al, sizeof al);
        }
        int s;
        if (tr->artist)                     /* its own artist finds it too: a guest, a compilation */
        {
            char own[256];
            sl_q_fold(tr->artist, own, sizeof own);
            s = sl_q_score(words, nw, t, own, al);
            int as_album = sl_q_score(words, nw, t, ar, al);
            if (as_album > s) s = as_album;
        }
        else s = sl_q_score(words, nw, t, ar, al);
        if (s) { scores[i] = s; songs[count++] = i; }
        if ((i & 511) == 511) yield();
    }
    /* artists and albums whose own names match (ar and al are reused) */
    /* four of each are shown; up to SQ_ALL_MAX kept for "See all" */
    free(sq_all_artists);
    free(sq_all_albums);
    sq_all_artists = malloc(SQ_ALL_MAX * sizeof *sq_all_artists);
    sq_all_albums = malloc(SQ_ALL_MAX * sizeof *sq_all_albums);
    sq_n_all_artists = sq_n_all_albums = 0;
    int artist_k[4], n_artists = 0, albums[4], n_albums = 0, all_artists = 0, all_albums = 0;
    for (int k = 0; k < sl_lib.n_albums; k++)
    {
        const struct sl_album *a = &sl_lib.albums[sl_lib.albums_by_artist[k]];
        if (k && !strcasecmp(a->artist, sl_lib.albums[sl_lib.albums_by_artist[k - 1]].artist)) continue;
        sl_q_fold(a->artist, ar, sizeof ar);
        if (!sl_q_score(words, nw, ar, "", "")) continue;
        if (n_artists < 4) artist_k[n_artists++] = k;
        if (sq_all_artists && sq_n_all_artists < SQ_ALL_MAX) sq_all_artists[sq_n_all_artists++] = k;
        all_artists++;
    }
    for (int k = 0; k < sl_lib.n_albums; k++)
    {
        int a = sl_lib.albums_by_title[k];
        sl_q_fold(sl_lib.albums[a].title, al, sizeof al);
        if (!sl_q_score(words, nw, al, "", "") || album_hit[a]) continue;
        album_hit[a] = 1;
        if (n_albums < 4) albums[n_albums++] = a;
        if (sq_all_albums && sq_n_all_albums < SQ_ALL_MAX) sq_all_albums[sq_n_all_albums++] = a;
        all_albums++;
    }
    free(album_hit);
    sq_scores = scores;
    if (count > 1) qsort(songs, count, sizeof *songs, sq_cmp);
    free(scores);

    char b[96];
    if (n_artists)
    {
        section(sq_body, "ARTISTS");
        for (int i = 0; i < n_artists; i++)
        {
            int k = artist_k[i];
            sq_artist_line(k, b, sizeof b);
            sq_result_row(sl_lib.albums[sl_lib.albums_by_artist[k]].artist, b, sl_lib.albums_by_artist[k], sq_artist_cb, k);
        }
        if (all_artists > n_artists && sq_n_all_artists) sq_see_all_row(all_artists, true);
    }
    if (n_albums)
    {
        section(sq_body, "ALBUMS");
        for (int i = 0; i < n_albums; i++)
            sq_result_row(sl_lib.albums[albums[i]].title, sl_lib.albums[albums[i]].artist, albums[i],
                          sq_album_cb, albums[i]);
        if (all_albums > n_albums && sq_n_all_albums) sq_see_all_row(all_albums, false);
    }
    int pls_before = (int)lv_obj_get_child_count(sq_body);
    sq_run_playlists(words, nw);
    bool any_pls = (int)lv_obj_get_child_count(sq_body) > pls_before;
    if (count)
    {
        int shown = count < SQ_SONGS_MAX ? count : SQ_SONGS_MAX;
        if (count > shown) snprintf(b, sizeof b, sl_tr("SONGS · BEST %d OF %d"), shown, count);
        else snprintf(b, sizeof b, sl_tr("SONGS · %d"), count);
        section(sq_body, b);
        int *list = realloc(songs, shown * sizeof *songs);
        if (!list) list = songs;
        songs = NULL;
        struct song_list_view *v = song_list(sq_body, list, shown, SL_LIST_CUSTOM);
        if (v) v->owned = list; else free(list);
    }
    free(songs);
    if (!count && !n_artists && !n_albums && !any_pls)
    {
        snprintf(b, sizeof b, "Nothing matches \"%s\"", sq_text);
        lv_obj_t *e = text(sq_body, b, &ss_regular_18, C_SUB);
        lv_obj_set_style_pad_all(e, 10, 0);
    }
    lv_obj_scroll_to_y(sq_body, 0, LV_ANIM_OFF);
    sl_log("search \"%s\": %d songs, %d artists, %d albums in %lu ms", sq_text, count, n_artists, n_albums,
           (unsigned long)(sl_ms() - started));
}

/* --------------------------------------------------------- the input */
static void sq_changed(void)
{
    sq_search_at = lv_tick_get() + 140;          /* let a quick run of taps finish */
    if (sq_timer) lv_timer_resume(sq_timer);
}
/* the field's words, as the results read them */
static void sq_ta_cb(lv_event_t *e)
{
    (void)e;
    if (!sq_ta) return;
    strlcpy(sq_text, sl_ta_text(sq_ta), sizeof sq_text);
    pl_utf8_trim(sq_text);
    sq_changed();
}

static bool sq_add(const char *s)
{
    size_t n = strlen(sq_text), k = strlen(s);
    if (!sq_ta || n + k >= sizeof sq_text - 1) return false;
    if (!strcmp(s, " ") && (!n || sq_text[n - 1] == ' ')) return false;
    lv_textarea_set_cursor_pos(sq_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(sq_ta, s);
    sfx_play(SFX_TAP);
    return true;
}

static void sq_backspace(void)
{
    if (!sq_ta || !sq_text[0]) return;
    lv_textarea_set_cursor_pos(sq_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_delete_char(sq_ta);
    sfx_play(SFX_UI_BACK);
}

static void sq_clear_cb(lv_event_t *e)
{
    (void)e;
    if (!sq_text[0] || !sq_ta) return;
    lv_textarea_set_text(sq_ta, "");
    sfx_play(SFX_UI_BACK);
    if (sq_kbd) kb_bar_clear(sq_kbd);
}

/* The words a glide can become, beside the keyboard's dictionary: the
 * library's titles, artists and albums (or the settings' names), read a
 * few hundred tracks per tick while the page is open, so a first glide
 * never waits for all of it. */
static bool sq_kb_more(struct kb *kb)
{
    if (kb->voc_more == 0)
        for (int i = 0; i < (int)ARRAYLEN(sq_settings); i++)
        {
            st_vocab_add_w(&kb->geo, &kb->voc, sl_tr(sq_settings[i].name), 40);
            st_vocab_add_w(&kb->geo, &kb->voc, sq_settings[i].words, 40);
        }
    if (sq_scope != SQ_SCOPE_MUSIC) return false;
    int at = kb->voc_more, end = at + 300, last_album = -1;
    for (; at < sl_lib.n_tracks && at < end; at++)
    {
        const struct sl_track *t = &sl_lib.tracks[at];
        /* a word in the library counts as much as a common word, so names glide */
        if (!st_vocab_add_w(&kb->geo, &kb->voc, t->title, 60)) return false;   /* full */
        if (t->album != last_album)
        {
            last_album = t->album;
            st_vocab_add_w(&kb->geo, &kb->voc, sl_lib.albums[t->album].title, 60);
            st_vocab_add_w(&kb->geo, &kb->voc, sl_lib.albums[t->album].artist, 60);
        }
    }
    kb->voc_more = at;
    return at < sl_lib.n_tracks;
}

static void sq_ink_clear(void)
{
    for (int i = 0; i < 24; i++)
    {
        if (sq_ink[i]) lv_obj_delete(sq_ink[i]);
        sq_ink[i] = NULL;
        free(sq_ink_pts[i]);
        sq_ink_pts[i] = NULL;
        sq_ink_n[i] = 0;
    }
    sq_npts = sq_stroke = 0;
}

static void sq_recognize(void)
{
    if (!sq_templates)
    {
        sq_templates = malloc(SL_P_MAX_TEMPLATES * sizeof *sq_templates);
        if (sq_templates) sq_ntemplates = sl_p_templates(sq_templates, SL_P_MAX_TEMPLATES);
    }
    char best[3];
    uint32_t started = sl_ms();
    int found = sq_templates && sq_npts >= 2 ? sl_p_recognize(sq_pts, sq_npts, sq_templates, sq_ntemplates, best) : 0;
    sl_log("scribble: %d points, %d strokes -> %c (%lu ms)", sq_npts, sq_stroke, found ? best[0] : '?',
           (unsigned long)(sl_ms() - started));
    sq_ink_clear();
    if (!found) return;
    char one[2] = { best[0], 0 };
    if (!sq_add(one)) return;
    /* the runners-up in the keyboard's bar, to fix a misread with one tap */
    if (!sq_kbd) return;
    kb_bar_clear(sq_kbd);
    for (int i = 1; i < found && sq_kbd->nsug < 3; i++)
        if (best[i] >= 'A' && best[i] <= 'Z') { sq_kbd->sug[sq_kbd->nsug][0] = best[i]; sq_kbd->sug[sq_kbd->nsug++][1] = '\0'; }
    sq_kbd->sug_letter = sq_kbd->nsug > 0;
    kb_bar_show(sq_kbd);
}

static void sq_pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !sq_pts) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t a;
    lv_obj_get_coords(sq_pad, &a);
    int x = pt.x - a.x1, y = pt.y - a.y1;
    if (code == LV_EVENT_PRESSED)
    {
        if (sq_stroke >= 24) return;
        sq_ink_pts[sq_stroke] = malloc(128 * sizeof(lv_point_precise_t));
        sq_ink[sq_stroke] = lv_line_create(sq_pad);
        lv_obj_set_style_line_width(sq_ink[sq_stroke], 8, 0);
        lv_obj_set_style_line_rounded(sq_ink[sq_stroke], true, 0);
        lv_obj_set_style_line_color(sq_ink[sq_stroke], lv_color_hex(C_ACCENT), 0);
        sq_ink_n[sq_stroke] = 0;
        sq_recognize_at = 0;
        if (sq_kbd) kb_bar_clear(sq_kbd);
    }
    if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && sq_stroke < 24)
    {
        if (sq_npts && sq_pts[sq_npts - 1].stroke == sq_stroke &&
            abs((int)sq_pts[sq_npts - 1].x - x) + abs((int)sq_pts[sq_npts - 1].y - y) < 3) return;
        if (sq_npts < SL_P_MAX_IN)
        {
            sq_pts[sq_npts].x = (float)x; sq_pts[sq_npts].y = (float)y; sq_pts[sq_npts].stroke = sq_stroke;
            sq_npts++;
        }
        int k = sq_ink_n[sq_stroke];
        if (sq_ink_pts[sq_stroke] && k < 128)
        {
            sq_ink_pts[sq_stroke][k].x = x; sq_ink_pts[sq_stroke][k].y = y;
            sq_ink_n[sq_stroke] = k + 1;
            lv_line_set_points(sq_ink[sq_stroke], sq_ink_pts[sq_stroke], k + 1);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (sq_stroke < 24) sq_stroke++;
        sq_recognize_at = lv_tick_get() + 650;    /* time for the next stroke of the letter */
        if (sq_timer) lv_timer_resume(sq_timer);
    }
}

static void sq_tick(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();
    bool waiting = false;
    if (sq_recognize_at)
    {
        if ((int32_t)(now - sq_recognize_at) >= 0) { sq_recognize_at = 0; sq_recognize(); }
        else waiting = true;
    }
    if (sq_search_at)
    {
        if ((int32_t)(now - sq_search_at) >= 0) { sq_search_at = 0; sq_run(); }
        else waiting = true;
    }
    if (!waiting) lv_timer_pause(t);
}

static void sq_layout(void)
{
    int top = SB_H + 66 + SQ_BAR_H;
    int bottom = sq_kb_shown ? H - SQ_KB_H : H;
    lv_obj_set_pos(sq_body, 0, top);
    lv_obj_set_size(sq_body, W, bottom - top);
    lv_obj_set_style_pad_bottom(sq_body, sq_kb_shown ? 16 : 110, 0);
    lv_obj_update_flag(sq_kb, LV_OBJ_FLAG_HIDDEN, !sq_kb_shown);
    if (sq_kb_shown && mini) lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);
    if (!sq_kb_shown && mini && !np_open) mini_update();
}

static void sq_show_kb(bool on)
{
    if (sq_kb_shown == on) return;
    sq_kb_shown = on;
    sq_layout();
}
static void sq_bar_cb(lv_event_t *e) { (void)e; sq_show_kb(true); }
static void sq_hide_cb(lv_event_t *e) { (void)e; sq_show_kb(false); }
static void sq_scroll_cb(lv_event_t *e)
{
    (void)e;
    if (lv_indev_active()) sq_show_kb(false);   /* browsing results: give them the room */
}

/* Draw: a pad where the keys were, Delete, Space and the way back beside it */
static void sq_draw_key_cb(lv_event_t *e)
{
    const char *v = lv_event_get_user_data(e);
    if (!strcmp(v, "\x08")) sq_backspace();
    else if (!strcmp(v, " ")) sq_add(" ");
    else
    {
        sq_drawing = false;
        sq_ink_clear();
        if (sq_pad_box) { lv_obj_delete(sq_pad_box); sq_pad_box = NULL; sq_pad = NULL; }
        if (sq_kbd) { lv_obj_remove_flag(sq_kbd->keys, LV_OBJ_FLAG_HIDDEN); kb_bar_clear(sq_kbd); }
    }
}
static lv_obj_t *sq_side_key(lv_obj_t *parent, const char *label, const char *value, int y, int h)
{
    lv_obj_t *k = box(parent);
    lv_obj_set_size(k, 92, h);
    lv_obj_set_pos(k, W - KB_PAD_X - 92, y);
    lv_obj_set_style_radius(k, sl_zune() ? 0 : SL_R_SM, 0);
    kb_fn_face(k);
    lv_obj_t *l = text(k, label, &ss_semibold_18, C_TEXT);
    sl_fit(l);                           /* "Leertaste", "Espacio": inside the key */
    lv_obj_center(l);
    on_click(k, sq_draw_key_cb, (void *)value);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
    return k;
}
static void sq_draw_open(struct kb *kb)
{
    if (sq_drawing) return;
    sq_drawing = true;
    if (!sq_pts) sq_pts = malloc(SL_P_MAX_IN * sizeof *sq_pts);
    lv_obj_add_flag(kb->keys, LV_OBJ_FLAG_HIDDEN);
    kb_bar_clear(kb);
    int top = KB_TOP + KB_BAR_H, h = 4 * KB_KEY_H + 3 * KB_ROW_GAP;
    sq_pad_box = box(kb->root);
    lv_obj_set_size(sq_pad_box, W, KB_H - top);
    lv_obj_set_pos(sq_pad_box, 0, top);
    sq_pad = box(sq_pad_box);
    lv_obj_set_size(sq_pad, W - 2 * KB_PAD_X - 92 - KB_GAP, h);
    lv_obj_set_pos(sq_pad, KB_PAD_X, 0);
    lv_obj_set_style_radius(sq_pad, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(sq_pad, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(sq_pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sq_pad, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(sq_pad, 2, 0);
    lv_obj_set_style_border_opa(sq_pad, LV_OPA_50, 0);
    lv_obj_add_flag(sq_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe */
    lv_obj_remove_flag(sq_pad, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(sq_pad, sq_pad_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *hint = text(sq_pad, "Write a letter here", &ss_regular_18, C_SUB);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -SL_SP_S);
    int kh = (h - 2 * KB_ROW_GAP) / 3;
    sq_side_key(sq_pad_box, LV_SYMBOL_BACKSPACE, "\x08", 0, kh);
    sq_side_key(sq_pad_box, "Space", " ", kh + KB_ROW_GAP, kh);
    sq_side_key(sq_pad_box, kb_def_now(kb)->abc, "abc", 2 * (kh + KB_ROW_GAP), kh);
}

static void sq_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sq_page) return;
    if (sq_timer) lv_timer_delete(sq_timer);
    sq_timer = NULL;
    for (int i = 0; i < 24; i++) { free(sq_ink_pts[i]); sq_ink_pts[i] = NULL; sq_ink[i] = NULL; sq_ink_n[i] = 0; }
    free(sq_pts); sq_pts = NULL;
    free(sq_templates); sq_templates = NULL;
    sq_npts = sq_stroke = 0;
    sq_page = sq_body = sq_ta = sq_kb = sq_pad = sq_pad_box = NULL;
    sq_kbd = NULL;
    sq_drawing = false;
    sq_pls_free();
    sq_kb_shown = false;
}

static void search_page_in(int scope);
static void search_page(void) { search_page_in(SQ_SCOPE_MUSIC); }
static void settings_search_page(void) { search_page_in(SQ_SCOPE_SETTINGS); }
static void search_page_in(int scope)
{
    lv_obj_t *p;
    sq_scope = scope;
    sq_body = page_new(scope == SQ_SCOPE_SETTINGS ? "Search settings" : "Search", &p);
    sq_page = p;
    lv_obj_add_event_cb(p, sq_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(sq_body, sq_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    lv_obj_t *bar = box(p);
    lv_obj_set_size(bar, W - 28, SQ_BAR_H - 12);
    lv_obj_set_pos(bar, 14, SB_H + 66);
    lv_obj_set_style_radius(bar, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    on_click(bar, sq_bar_cb, NULL);
    lv_obj_t *icon = sq_glass(bar, C_DIM);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);
    /* the field: one line, its cursor the accent's */
    sq_ta = lv_textarea_create(bar);
    lv_textarea_set_one_line(sq_ta, true);
    lv_obj_set_size(sq_ta, W - 28 - 56 - 58, SQ_BAR_H - 12);
    lv_obj_align(sq_ta, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_set_style_text_font(sq_ta, &ss_semibold_26, 0);
    lv_obj_set_style_text_color(sq_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(sq_ta, lv_color_hex(C_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(sq_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sq_ta, 0, 0);
    lv_obj_set_style_border_width(sq_ta, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(sq_ta, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(sq_ta, SL_SP_S, 0);
    lv_obj_set_style_pad_ver(sq_ta, (SQ_BAR_H - 12 - lv_font_get_line_height(&ss_semibold_26)) / 2, 0);
    lv_obj_set_style_border_color(sq_ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(sq_ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(sq_ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(sq_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_base_dir(sq_ta, LV_BASE_DIR_AUTO, 0);
    sl_ta_hint(sq_ta, sl_tr(scope == SQ_SCOPE_SETTINGS ? "Search settings" : "Search your music"));
    if (sl_ui_rtl()) lv_obj_set_style_text_align(sq_ta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_textarea_set_max_length(sq_ta, SL_Q_MAX - 8);
    lv_obj_add_state(sq_ta, LV_STATE_FOCUSED);
    lv_obj_remove_flag(sq_ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(sq_ta, sq_ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sq_ta, sq_bar_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear = box(bar);
    lv_obj_set_size(clear, 56, 56);
    lv_obj_align(clear, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_center(text(clear, LV_SYMBOL_CLOSE, &sl_sym_22, C_DIM));
    on_click(clear, sq_clear_cb, NULL);
    lv_obj_remove_flag(clear, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* the keyboard, docked at the bottom: Draw beside its globe, a key to put it away */
    sq_text[0] = 0;
    sq_kbd = kb_attach(p, sq_ta, KB_SEARCH, sq_hide_cb, NULL);
    sq_kb = sq_kbd ? sq_kbd->root : box(p);
    if (sq_kbd)
    {
        sq_kbd->more = sq_kb_more;
        sq_kbd->on_draw = sq_draw_open;
        kb_build_keys(sq_kbd);                 /* with its Draw key */
    }
    sq_timer = lv_timer_create(sq_tick, 40, NULL);
    lv_timer_pause(sq_timer);
    sq_kb_shown = true;
    sq_layout();
    sq_run();
    page_push(p);
    if (sq_kbd)
    {
        lv_obj_update_layout(sq_kbd->keys);
        kb_voc_begin(sq_kbd);                  /* the words now, not at the first glide */
        if (kb_voc_building(sq_kbd)) lv_timer_resume(sq_kbd->timer);
    }
    if (mini) lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------ Settings' search bubble
 * A small capsule under the Settings title. Tapped, it springs open into
 * the search bar, the way an island grows: a quick overshoot from its own
 * size to the bar's, then the keyboard rises under it. Only that one small
 * object moves and resizes (no page slides past), so it stays smooth on
 * the R1's software renderer. */
#define SQB_W 180
#define SQB_H 46
static lv_obj_t *sqb;                  /* the growing bubble, on the top layer */
static lv_area_t sqb_from;
static void sqb_step(void *o, int32_t v)
{
    lv_obj_t *b = o;
    int tx = 14, ty = SB_H + 66, tw = W - 28, th = SQ_BAR_H - 12;
    int fw = lv_area_get_width(&sqb_from), fh = lv_area_get_height(&sqb_from);
    int w = fw + (tw - fw) * v / 1000, h = fh + (th - fh) * v / 1000;
    int x = sqb_from.x1 + (tx - sqb_from.x1) * v / 1000, y = sqb_from.y1 + (ty - sqb_from.y1) * v / 1000;
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
}
static void sqb_kb_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void sqb_done(lv_anim_t *a)
{
    (void)a;
    page_push_quiet = true;
    settings_search_page();
    if (sq_kb)
    {
        /* the keyboard comes up from below */
        lv_anim_t k;
        lv_anim_init(&k);
        lv_anim_set_var(&k, sq_kb);
        lv_anim_set_values(&k, SQ_KB_H, 0);
        lv_anim_set_duration(&k, SL_T_PAGE);
        lv_anim_set_path_cb(&k, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&k, sqb_kb_ty);
        lv_obj_set_style_translate_y(sq_kb, SQ_KB_H, 0);
        lv_anim_start(&k);
    }
    if (sqb) { lv_obj_delete(sqb); sqb = NULL; }
}
static void sqb_open_cb(lv_event_t *e)
{
    lv_obj_t *pill = lv_event_get_current_target(e);
    if (sqb || sq_page) return;
    lv_obj_get_coords(pill, &sqb_from);
    sqb = box(lv_layer_top());
    lv_obj_set_style_bg_color(sqb, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(sqb, LV_OPA_COVER, 0);
    lv_obj_t *ic = sq_glass(sqb, C_DIM);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
    sqb_step(sqb, 0);
    sfx_play(SFX_UI_OPEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sqb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, sqb_step);
    lv_anim_set_completed_cb(&a, sqb_done);
    lv_anim_start(&a);
}
static void settings_search_pill(lv_obj_t *body)
{
    /* the width of the squares under it, rounded as they are */
    lv_obj_t *row = box(body);
    lv_obj_set_size(row, lv_pct(100), SL_PILL_H + SL_GUTTER);
    lv_obj_t *pill = box(row);
    lv_obj_set_size(pill, lv_pct(100), SL_PILL_H);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(pill, sl_zune() ? 0 : SL_R_PILL, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(pill, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(pill, 36, 0);
    lv_obj_set_style_border_width(pill, sl_zune() ? 0 : 1, 0);
    press_motion(pill);
    lv_obj_t *ic = sq_glass(pill, C_SUB);
    lv_obj_align(ic, set_rtl() ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, set_rtl() ? -20 : 20, 0);
    lv_obj_t *l = text(pill, "Search settings", &ss_regular_21, C_SUB);
    lv_obj_align(l, set_rtl() ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, set_rtl() ? -56 : 56, 0);
    on_click(pill, sqb_open_cb, NULL);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/* Where a toast or a banner settles. One rule everywhere: it floats over
 * content, never over a control or the status bar. That is just above the
 * mini player, or above whatever holds the bottom of the screen (the
 * picking bar, a keyboard), so a page's header, its chips and its back
 * button stay in reach. Now Playing and the queue are controls down to the
 * bottom edge, so there it sits under their header, over the art. Returns
 * +1 when it sits at the bottom (it rises into place), -1 at the top. */
static int sl_overlay_place(lv_obj_t *o, int top)
{
    if (cf_is_open()) { lv_obj_align(o, LV_ALIGN_TOP_MID, 0, top); return -1; }
    if (uq_open) { lv_obj_align(o, LV_ALIGN_TOP_MID, 0, UQ_TOP + 20 + 64 + SL_SP_XS); return -1; }   /* the sheet's header */
    if (np_open) { lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 10 + BACK_D + SL_SP_S); return -1; }         /* the close button */
    int floor = H - SL_FLOAT;
    lv_area_t a;
    if (mini && !lv_obj_has_flag(mini, LV_OBJ_FLAG_HIDDEN)) { lv_obj_get_coords(mini, &a); floor = LV_MIN(floor, a.y1); }
    if (sel_bar) { lv_obj_get_coords(sel_bar, &a); floor = LV_MIN(floor, a.y1); }
    if (search_hides_mini()) floor = LV_MIN(floor, H - SQ_KB_H);
    if (kb_on_top()) floor = LV_MIN(floor, H - WF_KB_H);
    lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, floor - H - SL_SP_S);
    return 1;
}
