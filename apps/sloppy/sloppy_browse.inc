/* Compact music and app destinations. GPLv2+. */
#define SL_BROWSE_PAGE 72

struct sl_browse_target { char path[MAX_PATH]; int offset; bool directory; };
static void sl_files_page(const char *path, int offset);
static void sl_saved_playlists_page(int offset);
static void sp_page(void);
/* picking (sloppy_playback.inc), which the Playlists page does too */
static int sel_count;
static bool sel_toggle(int k);
static void sel_end(void);
static void sel_start(int kind, void *view, lv_obj_t *page, int first);
static void sel_paint_artist(lv_obj_t *r, bool on);
static void sel_artists_repaint(lv_obj_t *body, bool marks);
static lv_obj_t *top_body(void);
static void q_add_many(const int *l, int n);
static lv_obj_t *more_button(lv_obj_t *page, lv_event_cb_t cb, void *ud);
static void sl_spotify_row_cb(lv_event_t *e) { (void)e; sp_page(); }

/* An app: a row of the card of apps under the last header (one is made
 * when the body's last child is not that card), its picture in a Settings
 * icon, as Library's and Settings' rows are */
static void sl_app_group_mark(lv_event_t *e) { (void)e; }
static lv_obj_t *sl_app_row(lv_obj_t *body, const char *title, const char *subtitle,
                            int action_id, int icon, uint32_t color)
{
    uint32_t n = lv_obj_get_child_count(body);
    lv_obj_t *g = n ? lv_obj_get_child(body, (int32_t)n - 1) : NULL;
    if (!g || !lv_obj_get_event_count(g) || lv_event_dsc_get_cb(lv_obj_get_event_dsc(g, 0)) != sl_app_group_mark)
    {
        g = set_group(body, NULL);
        lv_obj_add_event_cb(g, sl_app_group_mark, LV_EVENT_DELETE, NULL);
    }
    lv_obj_t *row = set_action_sub(g, "", color, title, subtitle, card_row_cb, (void *)(intptr_t)action_id);
    if (!sl_zune())
    {
        lv_obj_t *sq = lv_obj_get_child(row, 2);   /* the row's icon square: the app's picture in it */
        lv_obj_center(sl_icon(sq, icon, lv_obj_get_style_width(sq, 0) * 7 / 10, 0xffffff, color));
    }
    return row;
}

static const char *sl_playlist_dir(void)
{
    return global_settings.playlist_catalog_dir[0] ?
           (const char *)global_settings.playlist_catalog_dir : PLAYLIST_CATALOG_DEFAULT_DIR;
}

static bool sl_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && !strcasecmp(name + n - s, suffix);
}

/* Every file type Rockbox has a decoder for, as lib/rbcodec/metadata/
 * metadata.c lists them (the suite reads that table and checks this one
 * has them all): trackers, chiptunes, MIDI, DSD and WavPack too, not just
 * the usual ten. */
#define SL_AUDIO_EXTS "|mp3|mp2|mp1|mpa|flac|ogg|oga|opus|spx|wav|at3|w64|aif|aiff|au|snd|m4a|m4b|mp4|aac" \
                      "|ape|mac|wv|mpc|tta|shn|wma|wmv|asf|a52|ac3|rm|ra|rmvb|oma|aa3|dsf|dff|iso" \
                      "|mod|sid|spc|nsf|nsfe|sap|cmc|cm3|cmr|cms|dmc|dlt|mpt|mpd|rmt|tmc|tm8|tm2" \
                      "|ay|vtx|gbs|hes|sgc|vgm|vgz|kss|adx|mmf|vox|mid|midi|rmi|kar|"
/* name's extension, lowercased, is one of list's ("|a|b|") */
static bool sl_ext_in(const char *name, const char *list)
{
    const char *x = strrchr(name, '.');
    if (!x || x == name) return false;
    size_t n = strlen(++x);
    if (!n || n > 6) return false;
    char low[10];
    low[0] = '|';
    for (size_t i = 0; i < n; i++) low[i + 1] = (char)tolower((unsigned char)x[i]);
    low[n + 1] = '|';
    low[n + 2] = '\0';
    return strstr(list, low) != NULL;
}
static bool sl_audio_name(const char *name) { return sl_ext_in(name, SL_AUDIO_EXTS); }

/* Natural order (#164): a run of digits is one number, so "Track 2" comes
 * before "Track 10" and "Disc 9" before "Disc 10"; letters without case.
 * Equal by that, fewer leading zeros first, then plain bytes, so no two
 * names ever tie. */
static int sl_natcmp(const char *a, const char *b)
{
    const char *a0 = a, *b0 = b;
    int tie = 0;
    while (*a && *b)
    {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (isdigit(ca) && isdigit(cb))
        {
            const char *sa = a, *sb = b;
            while (*sa == '0') sa++;
            while (*sb == '0') sb++;
            const char *ea = sa, *eb = sb;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - sa != eb - sb) return ea - sa < eb - sb ? -1 : 1;
            int c = strncmp(sa, sb, (size_t)(ea - sa));
            if (c) return c;
            if (!tie && (sa - a) != (sb - b)) tie = (sa - a) < (sb - b) ? -1 : 1;
            a = ea;
            b = eb;
            continue;
        }
        int la = tolower(ca), lb = tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        a++;
        b++;
    }
    if (*a || *b) return *a ? 1 : -1;
    return tie ? tie : strcmp(a0, b0);
}

static void sl_target_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static lv_obj_t *sl_target_row(lv_obj_t *body, const char *title, const char *subtitle,
                               const char *path, int offset, bool directory, lv_event_cb_t cb)
{
    struct sl_browse_target *target = malloc(sizeof *target);
    if (!target) return NULL;
    strlcpy(target->path, path, sizeof target->path);
    target->offset = offset;
    target->directory = directory;
    lv_obj_t *row = card_row(body, title, subtitle, ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(row, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(row, cb, target);
    lv_obj_add_event_cb(row, sl_target_free, LV_EVENT_DELETE, target);
    return row;
}
/* The same, as a row of a card of rows (set_group) */
static lv_obj_t *sl_target_set_row(lv_obj_t *g, const char *title, const char *subtitle,
                                   const char *path, int offset, bool directory, lv_event_cb_t cb)
{
    struct sl_browse_target *target = malloc(sizeof *target);
    if (!target) return NULL;
    strlcpy(target->path, path, sizeof target->path);
    target->offset = offset;
    target->directory = directory;
    lv_obj_t *row = subtitle ? set_action_sub(g, NULL, 0, title, subtitle, cb, target) : set_action(g, NULL, 0, title, NULL, cb, target);
    lv_obj_add_event_cb(row, sl_target_free, LV_EVENT_DELETE, target);
    return row;
}

/* Play a saved playlist from its file. */
static void sl_play_saved(const char *path)
{
    sl_log("playlist tap: %s", path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { toast_warn("Playlist file is unavailable"); return; }
    char first;
    if (read(fd, &first, 1) != 1) { close(fd); toast("Playlist file is empty"); return; }
    close(fd);
    if (sl_new_list(NULL) < 0) { toast_warn("Could not create playlist"); return; }
    int inserted = playlist_insert_playlist_ex(NULL, path, PLAYLIST_INSERT_LAST, false, false);
    sl_log("playlist insert result: %d tracks: %d", inserted, playlist_amount());
    if (inserted < 0 || playlist_amount() == 0)
    { sl_abort_list(); toast_warn("Could not load this playlist"); sl_log("playlist load failed: %s", path); return; }
    playlist_sync(NULL);
    sl_start_list(0);
    sl_log("playlist play: %s", path);
    np_show_auto();
}
/* ---- a saved playlist, opened: its songs as a list, Play and Shuffle on
 * top, Rename and Delete under it. Its lines are matched to the library
 * (relative to the playlist's folder, Windows slashes and a UTF-8 mark
 * allowed); a song not in the library is counted, and Play then hands the
 * file to Rockbox, which plays whatever it can find. */
#define PLS_MAX 5000
static char pls_path[MAX_PATH], pls_name[96];
static int *pls_tracks, pls_n, pls_missing;
static lv_obj_t *pls_page_obj, *pls_name_ta;
static lv_obj_t *mn_pill(lv_obj_t *parent, const char *label, int w, int h, bool primary, lv_event_cb_t cb, void *ud);
static void wf_kb_build(lv_obj_t *p, lv_obj_t *ta, int kind, lv_event_cb_t enter, const char *enter_label);
static void sl_saved_playlists_page_at(int offset, bool replace);

static int pls_read_gone;                /* of the last read's missing ones, how many aren't on the card at all */
static int pls_read(const char *path, int **out, int *missing)
{
    *out = NULL;
    *missing = 0;
    pls_read_gone = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int *list = malloc(PLS_MAX * sizeof *list), n = 0;
    char dir[MAX_PATH], line[MAX_PATH], full[MAX_PATH];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    /* PLS_MAX lines looked up, found or not: a Windows list of C:\ paths,
     * or a big binary file named .m3u8, cost a card lookup a line on the
     * screen's thread and froze it for as long as it was long */
    for (bool first = true; list && n + *missing < PLS_MAX && read_line(fd, line, sizeof line) > 0; first = false)
    {
        char *l = line;
        if (first && (unsigned char)l[0] == 0xef && (unsigned char)l[1] == 0xbb && (unsigned char)l[2] == 0xbf) l += 3;
        while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == '#') continue;
        for (char *c = l; *c; c++) if (*c == '\\') *c = '/';
        if (l[0] == '/') strlcpy(full, l, sizeof full);
        else snprintf(full, sizeof full, "%s/%s", dir, l);
        int t = sl_lib_find_path(full);
        if (t >= 0) list[n++] = t;
        else { (*missing)++; if (!file_exists(full)) pls_read_gone++; }   /* deleted, or never there */
    }
    close(fd);
    *out = list;
    return n;
}
static void pls_play(bool shuffle)
{
    if (pls_n && play_list(pls_tracks, pls_n, 0, shuffle)) np_show_auto();
    else if (!pls_n) sl_play_saved(pls_path);      /* nothing matched: Rockbox's own loader */
}
static void pls_play_cb(lv_event_t *e) { pls_play((intptr_t)lv_event_get_user_data(e) != 0); }
static void pls_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != pls_page_obj) return;
    pls_page_obj = NULL;
    free(pls_tracks);
    pls_tracks = NULL;
    pls_n = 0;
}
static void pls_rename_page(lv_event_t *e);
static void pls_delete_page(lv_event_t *e);
static void pls_page(const char *path)
{
    int *tracks, missing, n = pls_read(path, &tracks, &missing), gone = pls_read_gone;
    if (n < 0) { toast_warn("Playlist file is unavailable"); return; }
    if (!n && !missing) { free(tracks); toast("Playlist file is empty"); return; }
    strlcpy(pls_path, path, sizeof pls_path);
    const char *base = strrchr(path, '/');
    strlcpy(pls_name, base ? base + 1 : path, sizeof pls_name);
    char *dot = strrchr(pls_name, '.');
    if (dot && dot != pls_name) *dot = '\0';
    lv_obj_t *p, *body = page_new(pls_name, &p);
    free(pls_tracks);
    pls_tracks = tracks;
    pls_n = n;
    pls_missing = missing;
    pls_page_obj = p;
    lv_obj_add_event_cb(p, pls_deleted, LV_EVENT_DELETE, NULL);
    long long ms = 0;   /* 32 bits on the R1: past 596 hours it went negative */
    for (int i = 0; i < n; i++) ms += sl_lib.tracks[tracks[i]].len;
    char b[96], s1[40], s2[32];
    if (n) snprintf(b, sizeof b, "%s \xc2\xb7 %s", sl_count(s1, sizeof s1, n, "%s SONG", "%s SONGS"),
                    sl_count(s2, sizeof s2, (int)(ms / 60000), "%s MIN", "%s MIN"));
    else sl_count(b, sizeof b, missing, "%s SONG", "%s SONGS");
    section(body, b);
    lv_obj_t *row = box(body);
    lv_obj_set_size(row, lv_pct(100), 2 * SL_R_PILL + 8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_t *go = mn_pill(row, LV_SYMBOL_PLAY "  Play", 10, 2 * SL_R_PILL, true, pls_play_cb, (void *)0);
    lv_obj_set_flex_grow(go, 1);
    lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
    if (n > 1)
    {
        lv_obj_t *sh = mn_pill(row, LV_SYMBOL_SHUFFLE "  Shuffle", 10, 2 * SL_R_PILL, false, pls_play_cb, (void *)1);
        lv_obj_set_flex_grow(sh, 1);
        lv_obj_set_style_transform_pivot_x(sh, lv_pct(50), 0);
    }
    for (int pass = 0; pass < 2; pass++)
    {
        /* the songs not in the library that are still on the card, then
         * those that aren't (deleted in Files, or moved away): marked missing */
        int k = pass ? gone : missing - gone;
        if (!k) continue;
        char m[256];
        if (pass) snprintf(m, sizeof m, sl_tr(k == 1 ? "%d song here is no longer on the card." : "%d songs here are no longer on the card."), k);
        else snprintf(m, sizeof m, sl_tr(k == 1 ? "%d song here is not in your library: Play still finds it on the card."
                                                : "%d songs here are not in your library: Play still finds them on the card."), k);
        lv_obj_t *note = text(body, m, &ss_regular_16, C_SUB);
        lv_obj_set_width(note, lv_pct(100));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_hor(note, 6, 0);
        lv_obj_set_style_pad_bottom(note, 6, 0);
    }
    if (n) song_list(body, pls_tracks, n, SL_LIST_CUSTOM);   /* a tap plays from that song, a hold picks songs */
    section(body, "THIS PLAYLIST");
    lv_obj_t *rn = card_row(body, "Rename", NULL, ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(rn, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(rn, pls_rename_page, NULL);
    lv_obj_t *dl = card_row(body, "Delete", NULL, ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(dl, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(dl, pls_delete_page, NULL);
    page_push(p);
}
/* After a rename or a delete: back to the list of playlists, read again */
static void pls_back_to_list(void)
{
    while (npages > 1 && pls_page_obj && pages[npages - 1] != pls_page_obj) page_pop();
    if (npages > 1) page_pop();
    sl_saved_playlists_page_at(0, true);
}
static void pls_rename_go_cb(lv_event_t *e)
{
    (void)e;
    const char *name = pls_name_ta ? sl_ta_text(pls_name_ta) : "";
    if (!name[0] || strchr(name, '/')) { toast("Type a name without slashes"); return; }
    char to[MAX_PATH], dir[MAX_PATH];
    strlcpy(dir, pls_path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    const char *ext = strrchr(pls_path, '.');
    snprintf(to, sizeof to, "%s/%s%s", dir, name, ext ? ext : ".m3u8");
    if (file_exists(to)) { toast("A playlist has that name already"); return; }
    if (rename(pls_path, to) != 0) { toast_warn("Could not rename the playlist"); return; }
    toast("Renamed");
    pls_back_to_list();
}
static void pls_name_deleted(lv_event_t *e) { (void)e; pls_name_ta = NULL; }
static void pls_rename_page(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("Rename", &p);
    lv_obj_add_event_cb(p, pls_name_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    section(body, "PLAYLIST NAME");
    pls_name_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(pls_name_ta, true);
    lv_textarea_set_max_length(pls_name_ta, 60);
    lv_textarea_set_text(pls_name_ta, pls_name);
    lv_obj_set_width(pls_name_ta, lv_pct(100));
    lv_obj_set_style_text_font(pls_name_ta, &ss_semibold_23, 0);
    lv_obj_set_style_bg_color(pls_name_ta, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(pls_name_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(pls_name_ta, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(pls_name_ta, SL_R_MD, 0);
    lv_obj_add_state(pls_name_ta, LV_STATE_FOCUSED);
    wf_kb_build(p, pls_name_ta, KB_NAME, pls_rename_go_cb, "Save");
    page_push(p);
}
static void pls_delete_go_cb(lv_event_t *e)
{
    (void)e;
    if (remove(pls_path) != 0) { toast_warn("Could not delete the playlist"); return; }
    toast("Playlist deleted");
    pls_back_to_list();
}
static void pls_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
static void pls_delete_page(lv_event_t *e)
{
    (void)e;
    char t[128];
    snprintf(t, sizeof t, "%s %s?", sl_tr("Delete"), pls_name);
    lv_obj_t *p, *body = page_new(t, &p);
    lv_obj_t *info = text(body, "The playlist goes. The songs in it stay on the card and in your library.", &ss_regular_18, C_SUB);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(info, 6, 0);
    lv_obj_set_style_pad_bottom(info, 10, 0);
    lv_obj_t *keep = card_row(body, "Keep it", NULL, ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(keep, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(keep, pls_keep_cb, NULL);
    lv_obj_t *del = card_row(body, "Delete the playlist", NULL, ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(del, card_row_cb, (void *)(intptr_t)ACT_SOON);
    lv_obj_set_style_text_color(lv_obj_get_child(del, 0), lv_color_hex(0xe5534b), 0);
    on_click(del, pls_delete_go_cb, NULL);
    page_push(p);
}
static void sl_saved_row_cb(lv_event_t *e)
{
    struct sl_browse_target *t = lv_event_get_user_data(e);
    lv_obj_t *r = lv_event_get_current_target(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
    if (sel_kind_on(SEL_PLAYLISTS))        /* picking: a tap marks it */
    {
        if (k >= 0 && sel_toggle(k)) sel_paint_artist(r, sel_has(k));
        return;
    }
    pls_page(t->path);
}

/* Each row's song count, one playlist a frame, so the list opens at once */
#define PLS_ROWS 72
static lv_obj_t *pls_count_rows[PLS_ROWS];
static char pls_count_paths[PLS_ROWS][MAX_PATH];
static int pls_count_n, pls_count_at;
static lv_timer_t *pls_count_timer;
static lv_obj_t *pls_list_obj, *pls_rows;   /* the page, and the card its playlists are rows of */
static void pls_count_cb(lv_timer_t *t)
{
    if (!pls_list_obj || pls_count_at >= pls_count_n) { lv_timer_pause(t); return; }
    int k = pls_count_at++;
    int fd = open(pls_count_paths[k], O_RDONLY), n = 0;
    char line[MAX_PATH];
    for (long lines = 0; fd >= 0 && lines < 20000 && read_line(fd, line, sizeof line) > 0; lines++)
    {
        const char *l = line;
        if (!lines && (unsigned char)l[0] == 0xef && (unsigned char)l[1] == 0xbb && (unsigned char)l[2] == 0xbf) l += 3;
        while (*l == ' ' || *l == '\t') l++;
        if (*l && *l != '#') n++;
    }
    if (fd >= 0) close(fd);
    char b[48];
    sl_count(b, sizeof b, n, "%s song", "%s songs");
    lv_obj_t *sub = lv_obj_get_child(pls_count_rows[k], 1);
    if (sub && lv_obj_check_type(sub, &lv_label_class)) lv_label_set_text(sub, b);
}
static void pls_list_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != pls_list_obj) return;
    pls_list_obj = pls_rows = NULL;
    pls_count_n = pls_count_at = 0;
    if (pls_count_timer) lv_timer_pause(pls_count_timer);
}
/* every saved playlist's file name, in natural order: no cap; the page
 * shows them a page at a time and picking can take them all */
static char **pls_all;
static int pls_all_n;
static void pls_all_free(void)
{
    for (int i = 0; i < pls_all_n; i++) free(pls_all[i]);
    free(pls_all);
    pls_all = NULL;
    pls_all_n = 0;
}
static int pls_cmp_name(const void *a, const void *b) { return sl_natcmp(*(char *const *)a, *(char *const *)b); }
/* the playlist folder's .m3u and .m3u8 files into a fresh list; the caller frees it */
static int pls_list_names(char ***out)
{
    int n = 0, cap = 0;
    char **l = NULL;
    DIR *dir = opendir(sl_playlist_dir());
    if (dir)
    {
        struct DIRENT *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_name[0] == '.' || (!sl_suffix(entry->d_name, ".m3u") && !sl_suffix(entry->d_name, ".m3u8"))) continue;
            if (n == cap)
            {
                char **g = realloc(l, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *g);
                if (!g) break;
                l = g;
            }
            if (!(l[n] = strdup(entry->d_name))) break;
            n++;
        }
        closedir(dir);
    }
    if (n > 1) qsort(l, (size_t)n, sizeof *l, pls_cmp_name);
    *out = l;
    return n;
}
static bool pls_full(char *out, size_t n, const char *name)
{
    const char *d = sl_playlist_dir();
    int len = snprintf(out, n, "%s%s%s", d, d[strlen(d) - 1] == '/' ? "" : "/", name);
    return len > 0 && len < (int)n;
}

static void sl_saved_more_cb(lv_event_t *e)
{
    struct sl_browse_target *t = lv_event_get_user_data(e);
    if (sel_on()) sel_end();
    sl_saved_playlists_page(t->offset);
}
/* the picked playlists' songs, one after another; the caller frees them */
static int pls_sel_tracks(int **out)
{
    int n = 0, cap = 0, *all = NULL;
    for (int i = 0; i < pls_all_n; i++)
    {
        char full[MAX_PATH];
        int *t, missing, k;
        if (!sel_has(i) || !pls_full(full, sizeof full, pls_all[i]) || (k = pls_read(full, &t, &missing)) < 0) continue;
        if (n + k > cap)
        {
            int *g = realloc(all, (size_t)(cap = n + k + 64) * sizeof *g);
            if (!g) { free(t); break; }
            all = g;
        }
        memcpy(all + n, t, (size_t)k * sizeof *t);
        n += k;
        free(t);
    }
    *out = all;
    return n;
}
static int pls_sel_total(void) { return pls_all_n; }
static void pls_sel_repaint(void *body, bool marks) { sel_artists_repaint(body, marks); }   /* rows marked as an artist's card is */
static void pls_del_go_cb(lv_event_t *e)
{
    (void)e;
    int gone = 0, failed = 0;
    for (int i = 0; i < pls_all_n; i++)
    {
        char full[MAX_PATH];
        if (!sel_has(i) || !pls_full(full, sizeof full, pls_all[i])) continue;
        if (remove(full) == 0) { gone++; sl_log("playlist deleted: %s", full); }
        else failed++;
    }
    char b[96];
    if (failed) toast_warn("Could not delete every playlist");
    else { snprintf(b, sizeof b, sl_tr(gone == 1 ? "Deleted %d playlist" : "Deleted %d playlists"), gone); toast(b); }
    sel_end();
    page_pop();
    sl_saved_playlists_page_at(0, true);
}
static void pls_del_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
/* the picked ones go, after a yes: what goes is listed, the songs stay */
static void pls_del_page(void)
{
    char t[64], b[96];
    snprintf(t, sizeof t, sl_tr(sel_count == 1 ? "Delete %d playlist?" : "Delete %d playlists?"), sel_count);
    lv_obj_t *p, *body = page_new(t, &p);
    lv_obj_t *g = set_group(body, NULL);
    set_text(g, "The playlists go. The songs in them stay on the card and in your library.", C_SUB);
    int shown = 0;
    for (int i = 0; i < pls_all_n && shown < 5; i++)
    {
        if (!sel_has(i)) continue;
        char nm[MAX_PATH];
        strlcpy(nm, pls_all[i], sizeof nm);
        char *dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = '\0';
        lv_obj_t *r = set_info(g, LV_SYMBOL_LIST, 0x7357e0, "", NULL, false);
        lv_label_set_text(lv_obj_get_child(r, 0), nm);
        shown++;
    }
    if (sel_count > shown)
    {
        snprintf(b, sizeof b, sl_tr("and %d more"), sel_count - shown);
        set_text(g, b, C_SUB);
    }
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_LEFT, 0x6a7280, "Keep them", NULL, pls_del_keep_cb, NULL);
    snprintf(b, sizeof b, sl_tr(sel_count == 1 ? "Delete %d playlist" : "Delete %d playlists"), sel_count);
    set_danger(g, LV_SYMBOL_TRASH, b, pls_del_go_cb, NULL);
    page_push(p);
}
static void pls_sel_act(int which)
{
    if (which == 2) { if (sel_count) pls_del_page(); return; }
    int *l, n = pls_sel_tracks(&l);
    sel_end();
    if (!n) toast("No songs here");
    else if (which == 0) { if (play_list(l, n, 0, false)) np_show_auto(); }
    else q_add_many(l, n);
    free(l);
}
/* a playlist's sheet: play, shuffle or queue it without opening it */
static int pls_acts_k;
static void pls_act_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e), k = pls_acts_k;
    acts_close();
    if (k < 0 || k >= pls_all_n) return;
    if (act == 3) { sel_start(SEL_PLAYLISTS, pls_rows, pages[npages - 1], k); return; }
    char full[MAX_PATH];
    int *t, missing, n;
    if (!pls_full(full, sizeof full, pls_all[k]) || (n = pls_read(full, &t, &missing)) < 0) { toast_warn("Playlist file is unavailable"); return; }
    if (!n) toast("No songs here");
    else if (act == 2) q_add_many(t, n);
    else if (play_list(t, n, 0, act == 1)) np_show_auto();
    free(t);
}
static void pls_long_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
    lv_indev_wait_release(lv_indev_active());
    if (sel_on() || k < 0 || k >= pls_all_n) return;
    pls_acts_k = k;
    acts_begin();
    char nm[MAX_PATH];
    strlcpy(nm, pls_all[k], sizeof nm);
    char *dot = strrchr(nm, '.');
    if (dot && dot != nm) *dot = '\0';
    lv_obj_t *hd = text(acts, "", &ss_semibold_18, C_SUB);
    lv_label_set_text(hd, nm);
    lv_obj_set_width(hd, lv_pct(100));
    lv_label_set_long_mode(hd, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_all(hd, 12, 0);
    acts_row_cb(LV_SYMBOL_PLAY, "Play", pls_act_cb, 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", pls_act_cb, 1);
    acts_row_cb(LV_SYMBOL_LIST, "Add to queue", pls_act_cb, 2);
    acts_row_cb(LV_SYMBOL_OK, "Select", pls_act_cb, 3);
    acts_show();
}
static void pls_menu_row_cb(lv_event_t *e)
{
    (void)e;
    acts_close();
    if (pls_all_n && pls_list_obj == pages[npages - 1]) sel_start(SEL_PLAYLISTS, pls_rows, pages[npages - 1], -1);
}
static void pls_menu_cb(lv_event_t *e)
{
    (void)e;
    acts_begin();
    char b[48];
    lv_obj_t *hd = text(acts, sl_count(b, sizeof b, pls_all_n, "%s playlist", "%s playlists"), &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_all(hd, 12, 0);
    acts_row_cb(LV_SYMBOL_OK, "Select", pls_menu_row_cb, 0);
    acts_show();
}

static void sl_saved_playlists_page(int offset) { sl_saved_playlists_page_at(offset, false); }
static void sl_saved_playlists_page_at(int offset, bool replace)
{
    const char *dirpath = sl_playlist_dir();
    if (sel_kind_on(SEL_PLAYLISTS)) sel_end();
    lv_obj_t *p, *body = page_new("Playlists", &p);
    pls_list_obj = p;
    pls_count_n = pls_count_at = 0;
    lv_obj_add_event_cb(p, pls_list_deleted, LV_EVENT_DELETE, NULL);
    /* every name first, so the list is in order, then a page of them */
    pls_all_free();
    pls_all_n = pls_list_names(&pls_all);
    if (pls_all_n) more_button(p, pls_menu_cb, NULL);
    char head[64];
    if (pls_all_n > SL_BROWSE_PAGE)
    {
        /* a page of many: which ones these are, so none seem missing */
        char a[16], z[16], all[16];
        snprintf(head, sizeof head, sl_tr("%s TO %s OF %s"), sl_fmt_count(a, sizeof a, offset + 1),
                 sl_fmt_count(z, sizeof z, LV_MIN(offset + SL_BROWSE_PAGE, pls_all_n)), sl_fmt_count(all, sizeof all, pls_all_n));
    }
    lv_obj_t *g = pls_rows = set_group(body, pls_all_n > SL_BROWSE_PAGE ? head : "YOUR SAVED PLAYLISTS");
    int shown = 0;
    for (int i = offset; i < pls_all_n && shown < SL_BROWSE_PAGE; i++)
    {
        const char *name = pls_all[i];
        char full[MAX_PATH];
        if (!pls_full(full, sizeof full, name)) continue;
        char shown_name[MAX_PATH];           /* "mix", not "mix.m3u8" */
        strlcpy(shown_name, name, sizeof shown_name);
        char *dot = strrchr(shown_name, '.');
        if (dot && dot != shown_name) *dot = '\0';
        lv_obj_t *r = sl_target_set_row(g, "", "Playlist", full, 0, false, sl_saved_row_cb);
        if (!r) break;
        lv_label_set_text(lv_obj_get_child(r, 0), sl_zune() ? sl_upper(shown_name, full, sizeof full) : shown_name);   /* its name: never translated */
        lv_obj_set_user_data(r, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(r, pls_long_cb, LV_EVENT_LONG_PRESSED, NULL);
        if (pls_count_n < PLS_ROWS)
        {
            pls_count_rows[pls_count_n] = r;
            pls_full(pls_count_paths[pls_count_n++], MAX_PATH, name);
        }
        shown++;
    }
    if (pls_count_n)
    {
        if (!pls_count_timer) pls_count_timer = lv_timer_create(pls_count_cb, 30, NULL);
        lv_timer_resume(pls_count_timer);
    }
    if (pls_all_n > offset + shown)
        sl_target_set_row(g, "More playlists", "Show the next page", dirpath,
                          offset + shown, true, sl_saved_more_cb);
    if (!shown && !offset)
    {
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);   /* no card of nothing: its header and a note */
        opt_note(body, "No saved playlists found. Rockbox playlists in the Playlists folder will appear here.");
    }
    if (!offset)                         /* a way on to another page, in its own card */
        sl_target_set_row(set_group(body, NULL), "Spotify playlists", NULL,
                          ROCKBOX_DIR "/sloppy/spotify", 0, true, sl_spotify_row_cb);
    if (replace) page_replace(p, false); else page_push(p);
}

static void sl_games_page(void)
{
    lv_obj_t *p, *body = page_new("Playroom", &p);
    set_header(body, "TAKE A LITTLE BREAK");
    sl_app_row(body, "Tamagotchi", "A little friend, always growing", ACT_PET, IC_PET, 0xb57c8f);
    sl_app_row(body, "Ragdoll", "Grab, toss and watch it tumble", ACT_RAGDOLL, IC_RAGDOLL, 0x6199a1);
    sl_app_row(body, "Snake", "Classic walls or level challenge", ACT_SNAKE, IC_SNAKE, 0x6aa87c);
    sl_app_row(body, "2048", "Slide the tiles, join the twins", ACT_2048, IC_2048, 0xd9893b);
    sl_app_row(body, "Sudoku", "Four levels, with notes and hints", ACT_SUDOKU, IC_SUDOKU, 0x4f7fe0);
    sl_app_row(body, "24 Points", "Make 24 from four cards", ACT_POINTS, IC_POINTS, 0xc2574a);
    sl_app_row(body, "Flappy Bird", "Tap to flap through the pipes", ACT_FLAPPY, IC_FLAPPY, 0x4fb8c4);
    sl_app_row(body, "Sloppy FM", "Tap to tune and grow your station", ACT_RADIO, IC_RADIO, 0x6b9eaa);
    sl_app_row(body, "Freaky Spoon", "Bounce up with a very strange spoon", ACT_SPOON, IC_SPOON, 0x947da7);
    sl_app_row(body, "Music Bip Bop Smarty", "Music, movies and TV trivia", ACT_QUIZ, IC_QUIZ, 0x738b7d);
    sl_app_row(body, "Flip a Coin", "A quick call for heads or tails", ACT_COIN, IC_COIN, 0xba8957);
    sl_app_row(body, "Magic 8 Ball", "Ask a question and tap the ball", ACT_MAGIC, IC_MAGIC, 0x6d83ba);
    sl_app_row(body, "Chess", "Play the computer or a friend", ACT_CHESS, IC_CHESS, 0x8a6a4a);
    sl_app_row(body, "Clicker", "Count anything, one tap at a time", ACT_CLICKER, IC_CLICKER, 0x4e9e8f);
    sl_app_row(body, "Metronome", "Keep time, 30 to 250 beats a minute", ACT_METRONOME, IC_METRONOME, 0xc4794a);
    sl_app_row(body, "Arcade", "Game Boy games from your card, and Doom", ACT_ARCADE, IC_CARTRIDGE, 0x7a5bd6);
    sl_app_row(body, "Spectrum", "Full screen, sideways", ACT_SPECTRUM, IC_SPECTRUM, 0x8a4fc9);
    sl_app_row(body, "Visualizer", "Your music, painting the screen", ACT_VISUALIZER, IC_VISUALIZER, 0x4b6fd6);
    sl_app_row(body, "Spacey Lines", "Draw into a spinning tunnel", ACT_SPACEY, IC_SPACEY, 0xb5568a);
    page_push(p);
}


