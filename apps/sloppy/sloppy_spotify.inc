/*
 *  Spotify playlists, matched to the songs on the card.
 *
 *  The player has no TLS, so it never talks to Spotify itself. A small
 *  script on a computer (tools/sloppy-spotify/spotify_playlists.py) signs
 *  in to the owner's own Spotify app, reads the playlists they choose and
 *  writes one file per playlist into /.rockbox/sloppy/spotify: the songs'
 *  title, artists, album and length, in order. No sign-in, token or audio
 *  ever reaches the card. Here each song is looked for on the card
 *  (sloppy_spotify_match.h) and the ones found become a normal playlist.
 *
 *  Saving and refreshing: the playlist it writes starts with a
 *  #SLOPPY-SPOTIFY:<id> line. Saving again replaces that file (whatever it
 *  has been renamed to), so a refresh never makes a second copy. A playlist
 *  of the same name without that line is someone's own and is left alone:
 *  the import gets "(Spotify)" on its name instead. Songs added to an
 *  imported playlist by hand go when it is refreshed; the page says so.
 */

#define SPO_DIR ROCKBOX_DIR "/sloppy/spotify"
#define SPO_MAX 2000                     /* songs read from one playlist */
#define SPO_ROWS 120                     /* rows drawn per list */

static struct
{
    struct sp_local *lib;               /* the card, ready to match against */
    int lib_n;
    int *match;                         /* per playlist song: track or -1 */
    char (*label)[96];                  /* per song: "Title · Artist" */
    int n, found;
    char id[64], name[96], file[MAX_PATH], exported[32];
} spo;

static void sp_free(void)
{
    free(spo.lib); free(spo.match); free(spo.label);
    spo.lib = NULL; spo.match = NULL; spo.label = NULL;
    spo.lib_n = spo.n = spo.found = 0;
}
static void sp_page_deleted(lv_event_t *e) { (void)e; sp_free(); }

static bool sp_build_lib(void)
{
    if (spo.lib && spo.lib_n == sl_lib.n_tracks) return true;
    free(spo.lib);
    spo.lib_n = 0;
    spo.lib = malloc((sl_lib.n_tracks ? sl_lib.n_tracks : 1) * sizeof *spo.lib);
    if (!spo.lib) return false;
    for (int i = 0; i < sl_lib.n_tracks; i++)
    {
        const struct sl_track *t = &sl_lib.tracks[i];
        const struct sl_album *a = &sl_lib.albums[t->album];
        spo.lib[i] = (struct sp_local){ t->title, a->artist, a->title, t->len, sp_title_key(t->title) };
        if ((i & 511) == 511) yield();
    }
    spo.lib_n = sl_lib.n_tracks;
    return true;
}

/* The header lines of an exported file: #id, #name, #exported. */
static void sp_header(char *line, char *id, size_t idn, char *name, size_t namen, char *when, size_t whenn)
{
    char *tab = strchr(line, '\t');
    if (line[0] != '#' || !tab) return;
    *tab++ = '\0';
    char *end = tab + strcspn(tab, "\r\n");
    *end = '\0';
    if (!strcmp(line, "#id") && id) strlcpy(id, tab, idn);
    else if (!strcmp(line, "#name") && name) strlcpy(name, tab, namen);
    else if (!strcmp(line, "#exported") && when) strlcpy(when, tab, whenn);
}

/* Read one exported playlist and look for every song on the card. */
static bool sp_load(const char *file)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0) { toast_warn("Could not read that playlist"); return false; }
    if (!sp_build_lib()) { close(fd); toast_warn("Not enough memory"); return false; }
    free(spo.match); free(spo.label);
    spo.match = malloc(SPO_MAX * sizeof *spo.match);
    spo.label = malloc(SPO_MAX * sizeof *spo.label);
    if (!spo.match || !spo.label) { close(fd); toast_warn("Not enough memory"); return false; }
    spo.n = spo.found = 0;
    spo.id[0] = spo.exported[0] = '\0';
    strlcpy(spo.file, file, sizeof spo.file);
    char line[SP_FIELD * 3 + 64];
    struct sp_song s;
    while (spo.n < SPO_MAX && read_line(fd, line, sizeof line) > 0)
    {
        if (line[0] == '#') { sp_header(line, spo.id, sizeof spo.id, spo.name, sizeof spo.name, spo.exported, sizeof spo.exported); continue; }
        if (!sp_parse_line(line, &s)) continue;
        int t = sp_match(&s, spo.lib, spo.lib_n);
        spo.match[spo.n] = t;
        char *semi = strchr(s.artists, ';');
        if (semi) *semi = '\0';            /* the first artist is enough on a row */
        snprintf(spo.label[spo.n], sizeof spo.label[0], "%s \xc2\xb7 %s", s.title, s.artists);
        pl_utf8_trim(spo.label[spo.n]);    /* a long row is cut between characters */
        if (t >= 0) spo.found++;
        if ((++spo.n & 31) == 0) yield();
    }
    close(fd);
    sl_log("spotify: %s, %d of %d songs on the card", spo.id, spo.found, spo.n);
    return true;
}

/* The playlist file this import wrote before (by its marker line), if any. */
static bool sp_find_marked(const char *id, char *out, size_t size)
{
    const char *dirpath = sl_playlist_dir();
    DIR *dir = opendir(dirpath);
    if (!dir) return false;
    char want[96], line[128];
    snprintf(want, sizeof want, "#SLOPPY-SPOTIFY:%s", id);
    bool found = false;
    struct DIRENT *entry;
    while (!found && (entry = readdir(dir)) != NULL)
    {
        if (!sl_suffix(entry->d_name, ".m3u8")) continue;
        char full[MAX_PATH];
        int n = snprintf(full, sizeof full, "%s%s%s", dirpath, dirpath[strlen(dirpath) - 1] == '/' ? "" : "/", entry->d_name);
        if (n < 0 || n >= (int)sizeof full) continue;
        int fd = open(full, O_RDONLY);
        if (fd < 0) continue;
        for (int k = 0; k < 3 && read_line(fd, line, sizeof line) > 0; k++)
            if (!strncmp(line, want, strlen(want)) && (line[strlen(want)] == '\0' || line[strlen(want)] == ' '))
            { found = true; strlcpy(out, full, size); break; }
        close(fd);
    }
    closedir(dir);
    return found;
}

/* Where a first save goes: the playlist's name, or "(Spotify)" after it
 * when someone's own playlist already has that name. */
static bool sp_new_path(char *out, size_t size)
{
    char clean[96], name[112];
    pl_clean_name(clean, sizeof clean, spo.name[0] ? spo.name : "Spotify playlist");
    if (!clean[0]) strlcpy(clean, "Spotify playlist", sizeof clean);
    for (int k = 0; k < 10; k++)
    {
        if (!k) strlcpy(name, clean, sizeof name);
        else if (k == 1) snprintf(name, sizeof name, "%s (Spotify)", clean);
        else snprintf(name, sizeof name, "%s (Spotify %d)", clean, k);
        if (!pl_path_for(out, size, name)) return false;
        if (!pl_exists(out)) return true;
    }
    return false;
}

static void sp_result_page(void);
static void sp_save_cb(lv_event_t *e)
{
    (void)e;
    if (!spo.match || !spo.id[0]) return;
    if (!spo.found) { toast("None of these songs are on this player"); return; }
    char path[MAX_PATH], tmp[MAX_PATH + 8];
    bool update = sp_find_marked(spo.id, path, sizeof path);
    if (!update && !sp_new_path(path, sizeof path)) { toast_warn("Could not name the playlist"); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { toast_warn("Could not save the playlist. Check card space."); return; }
    char head[200];
    int hn = snprintf(head, sizeof head, "#EXTM3U\n#SLOPPY-SPOTIFY:%s imported from Spotify; refreshing replaces this file\n", spo.id);
    bool okay = hn > 0 && hn < (int)sizeof head && pl_write(out, head, hn);
    char lines[MAX_PATH + 200];
    for (int i = 0; okay && i < spo.n; i++)
    {
        if (spo.match[i] < 0) continue;
        int m = pl_song_lines(lines, sizeof lines, spo.match[i], true);
        okay = m > 0 && pl_write(out, lines, m);
    }
    if (okay) okay = fsync(out) == 0;
    if (close(out) < 0) okay = false;
    if (okay) okay = rename(tmp, path) >= 0;
    if (!okay) { remove(tmp); toast_warn("Could not save the playlist. Check card space."); return; }
    sl_log("spotify: %s %s, %d songs", update ? "refreshed" : "saved", path, spo.found);
    toast(update ? "Playlist updated" : "Saved to Playlists");
    if (pl_entries) pl_load();          /* Add to playlist sees it */
}

static void sp_section_rows(lv_obj_t *body, bool found)
{
    int shown = 0, all = found ? spo.found : spo.n - spo.found;
    for (int i = 0; i < spo.n && shown < SPO_ROWS; i++)
    {
        if ((spo.match[i] >= 0) != found) continue;
        lv_obj_t *l = text(body, spo.label[i], &ss_regular_18, found ? C_TEXT : C_DIM);
        lv_obj_set_width(l, lv_pct(100));
        label_lines(l, 1);
        lv_obj_set_style_pad_ver(l, 6, 0);
        lv_obj_set_style_pad_left(l, 6, 0);
        shown++;
    }
    if (all > shown)
    {
        char b[48];
        snprintf(b, sizeof b, "%d more", all - shown);
        lv_obj_t *l = text(body, b, &ss_regular_18, C_SUB);
        lv_obj_set_style_pad_left(l, 6, 0);
    }
}

static void sp_result_page(void)
{
    lv_obj_t *p, *body = page_new(spo.name[0] ? spo.name : "Spotify playlist", &p);
    char b[160], marked[MAX_PATH];
    bool update = sp_find_marked(spo.id, marked, sizeof marked);
    snprintf(b, sizeof b, "%d of %d songs are on this player", spo.found, spo.n);
    lv_obj_t *sum = text(body, b, &ss_semibold_23, C_TEXT);
    lv_obj_set_style_pad_left(sum, 6, 0);
    snprintf(b, sizeof b, "From Spotify, open.spotify.com/playlist/%s%s%s", spo.id,
             spo.exported[0] ? "\nRead on " : "", spo.exported);
    lv_obj_t *src = text(body, b, &ss_regular_16, C_SUB);
    lv_obj_set_width(src, lv_pct(100));
    lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_left(src, 6, 0);
    lv_obj_t *bar = box(body);
    lv_obj_set_size(bar, lv_pct(100), 60);
    lv_obj_align(txt_btn(bar, update ? "Update playlist" : "Save as playlist", sp_save_cb), LV_ALIGN_LEFT_MID, 0, 0);
    if (update)
    {
        lv_obj_t *note = text(body, "Updating replaces the saved copy, songs added to it here included.",
                              &ss_regular_16, C_DIM);
        lv_obj_set_width(note, lv_pct(100));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_left(note, 6, 0);
    }
    if (spo.found) { section(body, "ON THIS PLAYER"); sp_section_rows(body, true); }
    if (spo.found < spo.n) { section(body, "NOT ON THIS PLAYER"); sp_section_rows(body, false); }
    page_push(p);
}

static void sp_open_cb(lv_event_t *e)
{
    struct sl_browse_target *t = lv_event_get_user_data(e);
    spo.name[0] = '\0';
    if (sp_load(t->path)) sp_result_page();
}

static void sp_forget_cb(lv_event_t *e)
{
    (void)e;
    DIR *dir = opendir(SPO_DIR);
    int n = 0;
    if (dir)
    {
        struct DIRENT *entry;
        char full[MAX_PATH];
        while ((entry = readdir(dir)) != NULL)
            if ((sl_suffix(entry->d_name, ".tsv") || !strcmp(entry->d_name, "top-artists.txt")) &&
                snprintf(full, sizeof full, SPO_DIR "/%s", entry->d_name) < (int)sizeof full && remove(full) == 0)
                n++;
        closedir(dir);
    }
    sl_log("spotify: removed %d exported playlists", n);
    toast("Spotify playlist lists removed");
    page_pop();
}

/* a list on the card as Settings' row: its name, how many songs and when */
static bool sp_list_row(lv_obj_t *g, const char *name, const char *sub, const char *path)
{
    struct sl_browse_target *target = malloc(sizeof *target);
    if (!target) return false;
    strlcpy(target->path, path, sizeof target->path);
    target->offset = 0;
    target->directory = false;
    lv_obj_t *row = set_action_sub(g, LV_SYMBOL_LIST, 0x1db954, name, sub, sp_open_cb, target);
    lv_obj_add_event_cb(row, sl_target_free, LV_EVENT_DELETE, target);
    return true;
}

static void sp_page(void)
{
    lv_obj_t *p, *body = page_new("Spotify playlists", &p);
    lv_obj_add_event_cb(p, sp_page_deleted, LV_EVENT_DELETE, NULL);
    DIR *dir = opendir(SPO_DIR);
    int shown = 0;
    lv_obj_t *g = NULL;
    if (dir)
    {
        struct DIRENT *entry;
        while ((entry = readdir(dir)) != NULL && shown < 100)
        {
            if (entry->d_name[0] == '.' || !sl_suffix(entry->d_name, ".tsv")) continue;
            char full[MAX_PATH], name[96] = "", when[32] = "", line[256];
            if (snprintf(full, sizeof full, SPO_DIR "/%s", entry->d_name) >= (int)sizeof full) continue;
            int fd = open(full, O_RDONLY), songs = 0;
            if (fd < 0) continue;
            while (read_line(fd, line, sizeof line) > 0)
                if (line[0] == '#') sp_header(line, NULL, 0, name, sizeof name, when, sizeof when);
                else if (line[0]) songs++;
            close(fd);
            char sub[80];
            snprintf(sub, sizeof sub, "%d songs%s%s", songs, when[0] ? ", read " : "", when);
            if (!shown) g = set_group(body, "FROM SPOTIFY");
            if (!sp_list_row(g, name[0] ? name : entry->d_name, sub, full)) break;
            shown++;
        }
        closedir(dir);
    }
    /* the top artists, if the helper copied them: how many are here */
    int fd = open(SPO_DIR "/top-artists.txt", O_RDONLY);
    if (fd >= 0)
    {
        char line[SP_FIELD], me[SP_FIELD], their[SP_FIELD];
        int all = 0, here = 0;
        while (read_line(fd, line, sizeof line) > 0)
        {
            if (!line[0] || line[0] == '#') continue;
            all++;
            sp_artist_norm(line, strlen(line), their, sizeof their);
            for (int a = 0; a < sl_lib.n_albums; a++)
            {
                const char *ar = sl_lib.albums[a].artist;
                if (!ar) continue;
                sp_artist_norm(ar, strlen(ar), me, sizeof me);
                if (their[0] && !strcmp(me, their)) { here++; break; }
            }
            if ((all & 7) == 0) yield();
        }
        close(fd);
        char b[96];
        snprintf(b, sizeof b, "%d of your %d Spotify top artists are on this player. Radio leans towards them.", here, all);
        opt_note(body, b);
    }
    if (!shown && fd < 0)
        lv_obj_set_style_pad_bottom(sl_empty(body, IC_PLAYLISTS, 96, "No Spotify playlists on this player yet",
                 "The player can't sign in to Spotify itself. On a computer, run "
                 "tools/sloppy-spotify/spotify_playlists.py from GabagoolOS with the card "
                 "connected: it signs in to your Spotify, lets you pick playlists you "
                 "made or collaborate on, and copies their song lists here. "
                 "No music comes from Spotify.", 56), SL_SP_XS, 0);   /* Arabic's marks under its last line */
    else
        set_danger(set_group(body, NULL), LV_SYMBOL_TRASH, "Remove these lists", sp_forget_cb, NULL);
    page_push(p);
}
