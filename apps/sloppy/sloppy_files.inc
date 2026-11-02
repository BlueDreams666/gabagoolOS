/* ---------------------------------------------------------------- Files */
/* The card as it is: every folder and file, folders first, each in natural
 * order (#164), in one list that only ever holds the rows on screen, so a
 * folder of thousands opens as fast as one of ten. A song shows its album's
 * cover when the library knows it; every other kind has its own tile. A tap
 * plays a song (the folder from there, in this order), opens a folder, a
 * cue sheet as its tracks, a playlist, a picture or a text file. A hold
 * offers the rest: queue, playlist, rename, move, copy, and Select, which
 * picks several (#37) and puts the picking bar at the foot.
 *
 * Nothing here ever replaces a file: a name that is taken is refused, a
 * move or copy onto one asks first and only ever keeps both or skips, and
 * a copy writes a hidden ".part" file and gives it its name once it is
 * whole, so a pulled card never leaves half a song under a real name.
 * Moving or renaming music asks the library to update in the background,
 * so the songs are found at their new places.
 *
 * Deleting is off unless Settings > Library > Library options allows it.
 * Then a held entry and the picking bar offer Delete, which always asks,
 * names what goes, says when what is playing is among it (it stops
 * first), and never touches .rockbox or the card itself. Folders go file
 * by file on a page with Stop, as a copy does. Included by sloppy.c. */

#define FM_SLOTS      14                 /* rows made at once: a screen and an overscan */
#define FM_ROW        82                 /* the Songs list's pitch: a 78 px row and its gap */
#define FM_ROW_H      78
#define FM_TILE       58                 /* a cover's size in a row (SL_ART_ROW) */
#define FM_GATHER_MAX 5000               /* songs one pick adds at once; said when reached */
#define FM_COPY_MAX   20000              /* files and folders one copy may hold */
#define FM_DEPTH      16                 /* folders inside folders, walked at most */
#define FM_TEXT_MAX   (16 * 1024)        /* what the text viewer shows of a file */
#define FM_PIC_MAX    (16 * 1024 * 1024) /* a bigger picture is not opened */
#define FM_CUE_MAX    (64 * 1024)
#define FM_SLICE      (256 * 1024)       /* read a frame while a picture loads */

enum { FK_DIR, FK_AUDIO, FK_CUE, FK_LIST, FK_PIC, FK_TEXT, FK_BOOK, FK_OTHER };
struct fm_entry { uint32_t name; int32_t track; long long size; uint8_t kind; };
struct fm_view {
    char path[MAX_PATH];
    char *pool;                          /* every name, one after another */
    size_t pool_n, pool_cap;
    struct fm_entry *e;
    int n, cap, folders;
    bool dest;                           /* where a move or copy goes: folders only */
    bool partial;                        /* memory ran out while reading: some are missing */
    lv_obj_t *page, *body, *host, *rows[FM_SLOTS];
    int idx[FM_SLOTS];
    bool binding;
};
static struct fm_view *fm_views[MAX_PAGES];
static void fm_drop_sources(struct fm_view *v);

static const char *fm_name(const struct fm_view *v, int k) { return v->pool + v->e[k].name; }
static bool fm_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t d = strlen(dir);
    int len = snprintf(out, n, "%s%s%s", dir, d && dir[d - 1] == '/' ? "" : "/", name);
    return len > 0 && len < (int)n;
}
static const char *fm_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}
static void fm_parent(char *out, size_t n, const char *path)
{
    strlcpy(out, path, n);
    char *s = strrchr(out, '/');
    if (s == out) s[1] = '\0';
    else if (s) *s = '\0';
}
static bool fm_exists(const char *path) { return file_exists(path) || dir_exists(path); }

static int fm_kind(const char *name, bool dir)
{
    if (dir) return FK_DIR;
    if (sl_audio_name(name)) return FK_AUDIO;
    if (sl_ext_in(name, "|cue|")) return FK_CUE;
    if (sl_ext_in(name, "|m3u|m3u8|")) return FK_LIST;
    if (sl_ext_in(name, "|jpg|jpeg|png|")) return FK_PIC;
    if (sl_ext_in(name, "|epub|")) return FK_BOOK;
    if (sl_ext_in(name, "|txt|log|lrc|nfo|md|ini|cfg|csv|json|")) return FK_TEXT;
    return FK_OTHER;
}
static const char *fm_symbol(int kind)
{
    static const char *const s[] = { LV_SYMBOL_DIRECTORY, LV_SYMBOL_AUDIO, LV_SYMBOL_AUDIO, LV_SYMBOL_LIST,
                                     LV_SYMBOL_IMAGE, LV_SYMBOL_FILE, LV_SYMBOL_FILE, LV_SYMBOL_FILE };
    return s[kind];
}
static uint32_t fm_tint(int kind)
{
    static const uint32_t t[] = { 0x6d83ba, 0xb5568a, 0x8a4fc9, SW_PLAYLIST_COLOR, 0x3d8a7c, 0x738b7d, 0xb0773a, 0x6a7280 };
    return t[kind];
}
/* 30.4 MB, the decimal as the language writes it */
static void fm_size(char *b, size_t n, long long bytes)
{
    int lang = global_settings.sloppy_language;
    char dec = (lang >= 1 && lang <= 3) || (lang >= SL_LANG_IT && lang <= SL_LANG_RU) ? ',' : '.';
    if (bytes < 1024) { snprintf(b, n, "%lld B", bytes); return; }
    if (bytes < 1024 * 1024) { snprintf(b, n, "%lld KB", (bytes + 512) / 1024); return; }
    long long tenths = bytes < 1024LL * 1024 * 1024 ? bytes * 10 / (1024 * 1024) : bytes * 10 / (1024LL * 1024 * 1024);
    snprintf(b, n, "%lld%c%lld %s", tenths / 10, dec, tenths % 10, bytes < 1024LL * 1024 * 1024 ? "MB" : "GB");
}
/* what a row says under its name */
static void fm_line(char *b, size_t n, const struct fm_view *v, int k)
{
    const struct fm_entry *en = &v->e[k];
    if (en->kind == FK_DIR) { strlcpy(b, sl_tr("Folder"), n); return; }
    if (en->kind == FK_CUE) { strlcpy(b, sl_tr("Cue sheet"), n); return; }
    if (en->kind == FK_LIST) { strlcpy(b, sl_tr("Playlist"), n); return; }
    char ext[8] = "", sz[24];
    const char *x = strrchr(fm_name(v, k), '.');
    if (x && x != fm_name(v, k) && strlen(x + 1) < sizeof ext)
        for (int i = 0; x[i + 1]; i++) ext[i] = (char)toupper((unsigned char)x[i + 1]), ext[i + 1] = '\0';
    fm_size(sz, sizeof sz, en->size);
    if (ext[0]) snprintf(b, n, "%s \xc2\xb7 %s", ext, sz);
    else strlcpy(b, sz, n);
}

/* ------------------------------------------------------------ reading */
static bool fm_push(struct fm_view *v, const char *name, int kind, long long size)
{
    size_t len = strlen(name) + 1;
    if (v->pool_n + len > v->pool_cap)
    {
        size_t cap = v->pool_cap ? v->pool_cap * 2 : 4096;
        while (cap < v->pool_n + len) cap *= 2;
        char *p = realloc(v->pool, cap);
        if (!p) return false;
        v->pool = p;
        v->pool_cap = cap;
    }
    if (v->n == v->cap)
    {
        int cap = v->cap ? v->cap * 2 : 64;
        struct fm_entry *e = realloc(v->e, (size_t)cap * sizeof *e);
        if (!e) return false;
        v->e = e;
        v->cap = cap;
    }
    memcpy(v->pool + v->pool_n, name, len);
    v->e[v->n].name = (uint32_t)v->pool_n;
    v->e[v->n].track = -2;               /* looked up in the library when its row is made */
    v->e[v->n].size = size;
    v->e[v->n].kind = (uint8_t)kind;
    v->n++;
    v->pool_n += len;
    return true;
}
static const char *fm_sort_pool;
static int fm_cmp(const void *a, const void *b)
{
    const struct fm_entry *x = a, *y = b;
    if ((x->kind == FK_DIR) != (y->kind == FK_DIR)) return x->kind == FK_DIR ? -1 : 1;
    return sl_natcmp(fm_sort_pool + x->name, fm_sort_pool + y->name);
}
/* what a card keeps for itself, not the music's */
static bool fm_system_name(const char *n)
{
    return !strcasecmp(n, "System Volume Information") || !strcasecmp(n, "$RECYCLE.BIN") || !strcasecmp(n, "LOST.DIR");
}
/* The folder's entries, folders first, each in natural order. False: the
 * folder could not be opened. */
static bool fm_read(struct fm_view *v, const char *path, bool folders_only)
{
    v->n = v->folders = 0;
    v->pool_n = 0;
    v->partial = false;
    DIR *d = opendir(path);
    if (!d) return false;
    struct DIRENT *en;
    int seen = 0;
    while ((en = readdir(d)) != NULL)
    {
        const char *nm = en->d_name;
        if (nm[0] == '.' || fm_system_name(nm)) continue;
        struct dirinfo info = dir_get_info(d, en);
        bool dir = (info.attribute & ATTR_DIRECTORY) != 0;
        if (folders_only && !dir) continue;
        if (!fm_push(v, nm, fm_kind(nm, dir), dir ? 0 : (long long)info.size)) { v->partial = true; break; }
        if ((++seen & 255) == 0) yield();
    }
    closedir(d);
    fm_sort_pool = v->pool;
    if (v->n > 1) qsort(v->e, (size_t)v->n, sizeof *v->e, fm_cmp);
    while (v->folders < v->n && v->e[v->folders].kind == FK_DIR) v->folders++;
    return true;
}
static void fm_view_free_lists(struct fm_view *v)
{
    free(v->pool);
    free(v->e);
    v->pool = NULL;
    v->e = NULL;
    v->n = v->cap = v->folders = 0;
    v->pool_n = v->pool_cap = 0;
}

/* ------------------------------------------------------------ paths gathered */
/* the songs a pick stands for: files as they are, folders walked */
struct fm_paths { char *pool; size_t n, cap; uint32_t *off; int count, offcap; bool capped; };
static bool fm_paths_add(struct fm_paths *ps, const char *path)
{
    if (ps->count >= FM_GATHER_MAX) { ps->capped = true; return false; }
    size_t len = strlen(path) + 1;
    if (ps->n + len > ps->cap)
    {
        size_t cap = ps->cap ? ps->cap * 2 : 8192;
        while (cap < ps->n + len) cap *= 2;
        char *p = realloc(ps->pool, cap);
        if (!p) { ps->capped = true; return false; }
        ps->pool = p;
        ps->cap = cap;
    }
    if (ps->count == ps->offcap)
    {
        int cap = ps->offcap ? ps->offcap * 2 : 64;
        uint32_t *o = realloc(ps->off, (size_t)cap * sizeof *o);
        if (!o) { ps->capped = true; return false; }
        ps->off = o;
        ps->offcap = cap;
    }
    memcpy(ps->pool + ps->n, path, len);
    ps->off[ps->count++] = (uint32_t)ps->n;
    ps->n += len;
    return true;
}
static const char *fm_paths_at(const struct fm_paths *ps, int i) { return ps->pool + ps->off[i]; }
static void fm_paths_free(struct fm_paths *ps)
{
    free(ps->pool);
    free(ps->off);
    memset(ps, 0, sizeof *ps);
}
/* to the Add to playlist sheet, which takes the list over */
static void pl_open_paths(struct fm_paths *ps, const char *title)
{
    pl_open_raw(ps->pool, ps->off, ps->count, title);
    memset(ps, 0, sizeof *ps);
}
/* a folder's songs in the order Files shows them, then its folders' */
static void fm_gather_dir(struct fm_paths *ps, const char *path, int depth)
{
    struct fm_view t = { 0 };
    if (!fm_read(&t, path, false)) return;
    char full[MAX_PATH];
    for (int k = t.folders; k < t.n && !ps->capped; k++)
        if (t.e[k].kind == FK_AUDIO && fm_join(full, sizeof full, path, fm_name(&t, k))) fm_paths_add(ps, full);
    for (int k = 0; k < t.folders && !ps->capped && depth < FM_DEPTH; k++)
        if (fm_join(full, sizeof full, path, fm_name(&t, k))) fm_gather_dir(ps, full, depth + 1);
    fm_view_free_lists(&t);
}
static void fm_gather_entry(struct fm_paths *ps, const struct fm_view *v, int k)
{
    char full[MAX_PATH];
    if (!fm_join(full, sizeof full, v->path, fm_name(v, k))) return;
    if (v->e[k].kind == FK_DIR) fm_gather_dir(ps, full, 0);
    else if (v->e[k].kind == FK_AUDIO) fm_paths_add(ps, full);
}

/* ------------------------------------------------------------ playing */
static bool fm_play_paths(const struct fm_paths *ps, int start, bool shuffle)
{
    if (!ps->count) { toast("No songs here"); return false; }
    if (start < 0 || start >= ps->count) start = 0;
    sl_log("files: play %d songs from %d%s: %s", ps->count, start, shuffle ? " shuffled" : "", fm_paths_at(ps, start));
    if (sl_new_list(NULL) < 0) { toast_warn("Could not create playlist"); return false; }
    for (int i = 0; i < ps->count; i++)
    {
        if (playlist_insert_track(NULL, fm_paths_at(ps, i), PLAYLIST_INSERT_LAST, false, false) < 0)
        {
            sl_log("files: stopped adding at %d", i);
            sl_abort_list();
            toast_warn("Could not load all songs. Check card space.");
            return false;
        }
        if ((i & 15) == 15) yield();
    }
    playlist_sync(NULL);
    if (shuffle)
    {
        unsigned seed = sl_shuffle_seed();
        srand(seed);
        start = playlist_shuffle(seed, rand() % ps->count);
    }
    sl_start_list(start);
    np_show_auto();
    return true;
}
/* onto the end of the queue (or next), or played when nothing is */
static void fm_queue_paths(const struct fm_paths *ps, bool next)
{
    if (!ps->count) { toast("No songs here"); return; }
    if (!something_playing()) { fm_play_paths(ps, 0, false); return; }
    int ok = 0;
    for (int i = next ? ps->count - 1 : 0; next ? i >= 0 : i < ps->count; i += next ? -1 : 1)   /* Play next: last first, so they play in order */
        if (q_insert(fm_paths_at(ps, i), next, true) >= 0) ok++;
    char b[128], num[16];
    if (next && ps->count == 1) strlcpy(b, sl_tr("Plays next"), sizeof b);
    else if (ps->capped) snprintf(b, sizeof b, sl_tr("Added the first %s songs"), sl_fmt_count(num, sizeof num, ok));
    else snprintf(b, sizeof b, sl_tr(ok == 1 ? "Added %d song to the queue" : "Added %d songs to the queue"), ok);
    ok == ps->count ? toast(b) : toast_warn("Could not queue every song");
    if (uq_open) uq_rebuild_soon();
}
/* the folder from this song on, in the order shown */
static void fm_play_from(struct fm_view *v, int k)
{
    struct fm_paths ps = { 0 };
    int start = 0;
    char full[MAX_PATH];
    for (int i = v->folders; i < v->n; i++)
    {
        if (v->e[i].kind != FK_AUDIO || !fm_join(full, sizeof full, v->path, fm_name(v, i))) continue;
        if (i == k) start = ps.count;
        if (!fm_paths_add(&ps, full) && i <= k) start = 0;
    }
    if (fm_join(full, sizeof full, v->path, fm_name(v, k)) && !file_exists(full)) toast_warn("File is unavailable");
    else fm_play_paths(&ps, start, false);
    fm_paths_free(&ps);
}

/* ------------------------------------------------------------ the list */
static void fm_row_cb(lv_event_t *e);
static void fm_long_cb(lv_event_t *e);
static int fm_head_h(void) { return lv_font_get_line_height(&ss_semibold_18) + 20; }
static int fm_row_y(const struct fm_view *v, int k)
{
    int y = fm_head_h() + k * FM_ROW;
    if (v->folders && v->folders < v->n && k >= v->folders) y += fm_head_h();
    return y;
}
/* a row's tile: the album's cover for a song the library has, else the
 * kind's symbol on its colour, rounded as a cover is */
static lv_obj_t *fm_tile(lv_obj_t *parent, struct fm_view *v, int k, int size)
{
    struct fm_entry *en = &v->e[k];
    if (en->track == -2)
    {
        char full[MAX_PATH];
        en->track = en->kind == FK_AUDIO && fm_join(full, sizeof full, v->path, fm_name(v, k)) ? sl_lib_find_path(full) : -1;
    }
    if (en->track >= 0 && en->track < sl_lib.n_tracks) return cover(parent, sl_lib.tracks[en->track].album, size, false);
    lv_obj_t *t = lit_square(parent, fm_tint(en->kind), size);
    lv_obj_set_style_radius(t, sl_zune() ? 0 : SL_R_SM, 0);
    lv_obj_center(text(t, fm_symbol(en->kind), size > 50 ? &sl_sym_28 : &sl_sym_22, 0xfefefe));
    return t;
}
static lv_obj_t *fm_row(struct fm_view *v, int k)
{
    lv_obj_t *r = box(v->host);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_size(r, lv_pct(100), FM_ROW_H);
    sl_align(fm_tile(r, v, k, FM_TILE), LV_ALIGN_LEFT_MID, 6, 0);
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (FM_ROW_H - lh1 - lh2) / 2), tw = W - 2 * SL_GUTTER - 80 - 56;
    lv_obj_t *t1 = text(r, "", &ss_semibold_23, 0xffffff);
    lv_label_set_text(t1, fm_name(v, k));        /* a name as it is: never translated */
    lv_obj_set_size(t1, tw, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    sl_place(t1, 80, ty);
    /* read the way the interface reads, from by the tile: "05 שיר.mp3"
     * stayed whole at the start, not turned into "mp3.שיר 05" at the far end */
    lv_obj_set_style_base_dir(t1, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_text_align(t1, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    char line[64];
    fm_line(line, sizeof line, v, k);
    lv_obj_t *t2 = text(r, line, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    sl_place(t2, 80, ty + lh1);
    if (sel_kind_on(SEL_FILES) && sel_view == v)
    {
        sl_align(sel_ring(r), LV_ALIGN_RIGHT_MID, -14, 0);
        sel_paint_row(r, sel_has(k));
    }
    else if (v->e[k].kind == FK_DIR) sl_align(text(r, sl_chevron(), &sl_sym_18, C_DIM), LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_user_data(r, (void *)(intptr_t)k);
    on_click(r, fm_row_cb, v);
    if (!v->dest) lv_obj_add_event_cb(r, fm_long_cb, LV_EVENT_LONG_PRESSED, v);
    lv_obj_set_pos(r, 0, fm_row_y(v, k));
    return r;
}
static void fm_bind(struct fm_view *v)
{
    if (v->binding || !v->host || !v->n) return;
    v->binding = true;
    int y = lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host);
    int first = y / FM_ROW - 2;
    if (first > v->n - FM_SLOTS) first = v->n - FM_SLOTS;
    if (first < 0) first = 0;
    for (int k = first; k < v->n && k < first + FM_SLOTS; k++)
    {
        int slot = k % FM_SLOTS;
        if (v->idx[slot] == k && v->rows[slot]) continue;
        if (v->rows[slot]) lv_obj_delete(v->rows[slot]);
        v->rows[slot] = fm_row(v, k);
        v->idx[slot] = k;
    }
    v->binding = false;
}
static void fm_scroll_cb(lv_event_t *e) { fm_bind(lv_event_get_user_data(e)); }
/* the rows made again (picking began or ended) */
static void fm_rebind_async(void *p)
{
    struct fm_view *v = p;
    bool live = false;
    for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] == v) live = true;
    if (!live) return;
    for (int s = 0; s < FM_SLOTS; s++)
    {
        if (v->rows[s]) lv_obj_delete(v->rows[s]);
        v->rows[s] = NULL;
        v->idx[s] = -1;
    }
    fm_bind(v);
}
static lv_obj_t *fm_head(lv_obj_t *host, const char *s, int y)
{
    lv_obj_t *l = section(host, s);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_y(l, y);
    return l;
}
/* the designed empty folder: a tile, a line, and what to do about it */
static void fm_empty(struct fm_view *v)
{
    lv_obj_t *c = box(v->body);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(c, 96, 0);
    lv_obj_set_style_pad_bottom(c, SL_GUTTER, 0);   /* room under the last line for Arabic's marks */
    lv_obj_set_style_pad_row(c, SL_SP_S, 0);
    lv_obj_t *t = lit_square(c, fm_tint(FK_DIR), 72);
    lv_obj_set_style_radius(t, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_center(text(t, LV_SYMBOL_DIRECTORY, &sl_sym_28, 0xfefefe));
    lv_obj_set_style_margin_bottom(t, SL_SP_S, 0);
    lv_obj_t *h = text(c, v->dest ? "No folders in here" : "This folder is empty", &ss_semibold_23, 0xffffff);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *s = text(c, v->dest ? "Put it here, or make a new folder with +." :
                                    "Copy music here in USB mode, or with Wi-Fi transfer.", &ss_regular_18, C_SUB);
    lv_obj_set_width(s, W - 2 * SL_GUTTER - 2 * SL_INSET);
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    sl_label_deep(h);                    /* Arabic's marks, below the line, kept */
    sl_label_deep(s);
}
/* the list in the page's body, made again from v's entries */
static void fm_build(struct fm_view *v)
{
    for (int s = 0; s < FM_SLOTS; s++) { v->rows[s] = NULL; v->idx[s] = -1; }
    lv_obj_clean(v->body);
    v->host = NULL;
    if (!v->n) { fm_empty(v); return; }
    v->host = box(v->body);
    int files = v->n - v->folders, hh = fm_head_h();
    lv_obj_set_size(v->host, lv_pct(100), fm_row_y(v, v->n - 1) + FM_ROW_H);
    char b[64];
    if (v->folders) fm_head(v->host, sl_count(b, sizeof b, v->folders, "%s FOLDER", "%s FOLDERS"), 0);
    if (files) fm_head(v->host, sl_count(b, sizeof b, files, "%s FILE", "%s FILES"), v->folders ? fm_row_y(v, v->folders) - hh : 0);
    if (v->partial)
    {
        lv_obj_t *n = text(v->body, "Not enough memory to list everything here", &ss_regular_16, C_SUB);
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_all(n, SL_SP_S, 0);
    }
    lv_obj_update_layout(v->body);
    fm_bind(v);
}
static void fm_page_deleted(lv_event_t *e)
{
    struct fm_view *v = lv_event_get_user_data(e);
    if (lv_event_get_target(e) != v->page) return;
    for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] == v) fm_views[i] = NULL;
    lv_async_call_cancel(fm_rebind_async, v);
    sel_forget(v);
    if (v->dest) lv_async_call(sel_mini_async, NULL);   /* its bar gone: the mini player back */
    fm_view_free_lists(v);
    free(v);
}
/* read again and shown again, where it was (a rename, a move, a copy) */
static void fm_reload(struct fm_view *v)
{
    if (!v) return;
    int y = lv_obj_get_scroll_y(v->body);
    if (!fm_read(v, v->path, v->dest)) { toast_warn("Folder is unavailable"); return; }
    if (v->dest) fm_drop_sources(v);
    if (sel_view == v) sel_end();
    fm_build(v);
    lv_obj_scroll_to_y(v->body, LV_MIN(y, (int)lv_obj_get_scroll_bottom(v->body) + y), LV_ANIM_OFF);
    fm_bind(v);
}
static struct fm_view *fm_find(const char *path)
{
    for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && !fm_views[i]->dest && !strcmp(fm_views[i]->path, path)) return fm_views[i];
    return NULL;
}
/* the row named name, scrolled to and lit (after a rename or a new folder) */
static void fm_show_name(struct fm_view *v, const char *name)
{
    if (!v || !v->host) return;
    for (int k = 0; k < v->n; k++)
    {
        if (strcmp(fm_name(v, k), name)) continue;
        lv_obj_update_layout(v->body);
        int y = lv_obj_get_y(v->host) + fm_row_y(v, k) - (lv_obj_get_height(v->body) - FM_ROW_H) / 2;
        lv_obj_scroll_to_y(v->body, LV_MAX(0, y), LV_ANIM_OFF);
        fm_bind(v);
        for (int s = 0; s < FM_SLOTS; s++) if (v->idx[s] == k && v->rows[s]) sl_flash(v->rows[s], FLASH_ROW);
        return;
    }
}
static void fm_menu_cb(lv_event_t *e);
static void fm_dest_new_cb(lv_event_t *e);
static void fm_dest_cancel_cb(lv_event_t *e);
static void fm_dest_bar(struct fm_view *v);
static struct fm_view *fm_open(const char *path, bool dest)
{
    struct fm_view *v = calloc(1, sizeof *v);
    if (!v) { toast_warn("Not enough memory to show more files"); return NULL; }
    strlcpy(v->path, path, sizeof v->path);
    v->dest = dest;
    if (!fm_read(v, path, dest))
    {
        free(v);
        toast_warn("Folder is unavailable");
        sl_log("folder open failed: %s", path);
        return NULL;
    }
    if (dest) fm_drop_sources(v);
    const char *name = strcmp(path, "/") ? fm_base(path) : "Files";
    lv_obj_t *p, *body = page_new(name, &p);
    if (strcmp(path, "/"))                    /* a folder's own name, never translated */
    {
        lv_obj_t *hdr = lv_obj_get_child(p, 0), *t = lv_obj_get_child(hdr, -1);
        char low[MAX_PATH];
        if (t && lv_obj_check_type(t, &lv_label_class))
            lv_label_set_text(t, sl_zune() ? sl_lower(name, low, sizeof low) : name);
    }
    v->page = p;
    v->body = body;
    lv_obj_set_layout(body, LV_LAYOUT_NONE);   /* the rows place themselves */
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    sl_scroll_indicator(body);
    lv_obj_add_event_cb(body, fm_scroll_cb, LV_EVENT_SCROLL, v);
    lv_obj_add_event_cb(p, fm_page_deleted, LV_EVENT_DELETE, v);
    for (int i = 0; i < MAX_PAGES; i++) if (!fm_views[i]) { fm_views[i] = v; break; }
    if (dest)
    {
        header_button(p, LV_SYMBOL_CLOSE, fm_dest_cancel_cb, NULL);
        header_button(p, LV_SYMBOL_PLUS, fm_dest_new_cb, v);
    }
    else more_button(p, fm_menu_cb, v);
    fm_build(v);
    if (dest) fm_dest_bar(v);
    if (npages >= MAX_PAGES) page_replace(p, true);   /* deep in the card: the stack is full, this one takes the top */
    else page_push(p);
    return v;
}
static void sl_files_page(const char *path, int offset)
{
    struct fm_view *v = fm_open(path, false);
    if (!v || offset <= 0 || offset >= v->n) return;
    lv_obj_update_layout(v->body);
    lv_obj_scroll_to_y(v->body, lv_obj_get_y(v->host) + fm_row_y(v, offset), LV_ANIM_OFF);
    fm_bind(v);
}

/* ------------------------------------------------------------ names */
/* rename and new folder: a name typed, checked, then done or refused */
static struct { struct fm_view *v; int k; bool folder; char ext[16]; } fm_nm;
static lv_obj_t *fm_nm_ta;
static void fm_nm_deleted(lv_event_t *e) { (void)e; fm_nm_ta = NULL; }
/* the typed name as the card can take it; false (and said) when it cannot */
static bool fm_clean(char *out, size_t n, const char *typed)
{
    pl_clean_name(out, n, typed);
    if (!out[0]) { toast("Type a name"); return false; }
    if (strcmp(out, typed))
    {
        for (const char *c = typed; *c; c++)
            if ((unsigned char)*c < 0x20 || strchr("/\\:*?\"<>|", *c)) { toast("A name can't have / \\ : * ? \" < > or |"); return false; }
    }
    return true;
}
static void fm_nm_go_cb(lv_event_t *e)
{
    (void)e;
    struct fm_view *v = fm_nm.v;
    bool live = false;
    for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] == v) live = true;
    if (!fm_nm_ta || !live) return;
    char base[200], name[220], to[MAX_PATH];
    if (!fm_clean(base, sizeof base, sl_ta_text(fm_nm_ta))) return;
    snprintf(name, sizeof name, "%s%s", base, fm_nm.ext);
    if (!fm_join(to, sizeof to, v->path, name)) { toast("That name is too long"); return; }
    if (fm_nm.k < 0)
    {
        if (fm_exists(to)) { toast("Something here has that name already"); return; }
        if (mkdir(to) != 0) { toast_warn("Could not make the folder"); return; }
        sl_log("files: new folder %s", to);
        toast("Folder made");
    }
    else
    {
        char from[MAX_PATH];
        const char *old = fm_name(v, fm_nm.k);
        if (!strcmp(old, name)) { page_pop(); return; }
        if (!fm_join(from, sizeof from, v->path, old)) return;
        /* taken, unless only its capitals change (the card sees one name) */
        if (strcasecmp(old, name) && fm_exists(to)) { toast("Something here has that name already"); return; }
        bool music = v->e[fm_nm.k].kind == FK_DIR || v->e[fm_nm.k].kind == FK_AUDIO;
        if (rename(from, to) != 0) { toast_warn("Could not rename it"); return; }
        sl_log("files: renamed %s to %s", from, to);
        toast("Renamed");
        if (music) db_update_begin(false);   /* the library finds them by their new names */
    }
    page_pop();
    fm_reload(v);
    fm_show_name(v, name);
}
static void fm_name_page(struct fm_view *v, int k)
{
    fm_nm.v = v;
    fm_nm.k = k;
    fm_nm.ext[0] = '\0';
    char start[200];
    if (k < 0)
    {
        strlcpy(start, sl_tr("New folder"), sizeof start);
        char t[MAX_PATH];
        for (int n = 2; n < 100 && fm_join(t, sizeof t, v->path, start) && fm_exists(t); n++)
            snprintf(start, sizeof start, "%s %d", sl_tr("New folder"), n);
    }
    else
    {
        strlcpy(start, fm_name(v, k), sizeof start);
        char *dot = strrchr(start, '.');
        /* a file keeps its type: the name is edited, the extension kept */
        if (v->e[k].kind != FK_DIR && dot && dot != start && strlen(dot) < sizeof fm_nm.ext)
        {
            strlcpy(fm_nm.ext, dot, sizeof fm_nm.ext);
            *dot = '\0';
        }
    }
    lv_obj_t *p, *body = page_new(k < 0 ? "New folder" : "Rename", &p);
    lv_obj_add_event_cb(p, fm_nm_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    section(body, k < 0 ? "FOLDER NAME" : "NAME");
    fm_nm_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(fm_nm_ta, true);
    lv_textarea_set_max_length(fm_nm_ta, 120);
    lv_textarea_set_text(fm_nm_ta, start);
    lv_obj_set_width(fm_nm_ta, lv_pct(100));
    lv_obj_set_style_text_font(fm_nm_ta, &ss_semibold_23, 0);
    lv_obj_set_style_bg_color(fm_nm_ta, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(fm_nm_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(fm_nm_ta, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(fm_nm_ta, SL_R_MD, 0);
    lv_obj_add_state(fm_nm_ta, LV_STATE_FOCUSED);
    if (fm_nm.ext[0])
    {
        char b[64];
        snprintf(b, sizeof b, sl_tr("Stays a %s file"), fm_nm.ext + 1);
        lv_obj_t *n = text(body, b, &ss_regular_16, C_SUB);
        lv_obj_set_style_pad_hor(n, 6, 0);
        lv_obj_set_style_pad_top(n, SL_SP_S, 0);
    }
    wf_kb_build(p, fm_nm_ta, KB_NAME, fm_nm_go_cb, k < 0 ? "Create" : "Save");
    page_push(p);
}

/* ------------------------------------------------------------ move and copy */
enum { FM_MOVE, FM_COPY };
enum { FM_ASK, FM_KEEP_BOTH, FM_SKIP };
static struct {
    int kind, n;
    char **src;
    lv_obj_t *origin;                    /* the Files page it started on */
    char from[MAX_PATH];                 /* that page's folder */
    char dest[MAX_PATH];
} fm_op;
static void fm_op_clear(void)
{
    for (int i = 0; i < fm_op.n; i++) free(fm_op.src[i]);
    free(fm_op.src);
    fm_op.src = NULL;
    fm_op.n = 0;
}
static bool fm_in_op(const char *path)
{
    for (int i = 0; i < fm_op.n; i++) if (!strcmp(fm_op.src[i], path)) return true;
    return false;
}
/* a destination never offers a folder that is itself going */
static void fm_drop_sources(struct fm_view *v)
{
    char full[MAX_PATH];
    int o = 0;
    for (int k = 0; k < v->n; k++)
        if (!fm_join(full, sizeof full, v->path, fm_name(v, k)) || !fm_in_op(full)) v->e[o++] = v->e[k];
    v->n = v->folders = o;
}
/* where name would go in dir without replacing anything: "name 2.ext" on */
static bool fm_free_name(char *out, size_t n, const char *dir, const char *name, bool dir_kind)
{
    if (!fm_join(out, n, dir, name)) return false;
    if (!fm_exists(out)) return true;
    char stem[200], ext[24] = "", alt[230];
    strlcpy(stem, name, sizeof stem);
    char *dot = dir_kind ? NULL : strrchr(stem, '.');
    if (dot && dot != stem && strlen(dot) < sizeof ext) { strlcpy(ext, dot, sizeof ext); *dot = '\0'; }
    for (int k = 2; k < 1000; k++)
    {
        snprintf(alt, sizeof alt, "%s %d%s", stem, k, ext);
        if (!fm_join(out, n, dir, alt)) return false;
        if (!fm_exists(out)) return true;
    }
    return false;
}
static void fm_dest_page(const char *path);
static void fm_op_start(int kind, struct fm_view *v, const int *ks, int n)
{
    fm_op_clear();
    fm_op.src = calloc((size_t)(n ? n : 1), sizeof *fm_op.src);
    if (!fm_op.src) { toast_warn("Not enough memory"); return; }
    char full[MAX_PATH];
    for (int i = 0; i < n; i++)
        if (fm_join(full, sizeof full, v->path, fm_name(v, ks[i])) && (fm_op.src[fm_op.n] = strdup(full))) fm_op.n++;
    fm_op.kind = kind;
    fm_op.origin = v->page;
    strlcpy(fm_op.from, v->path, sizeof fm_op.from);
    fm_dest_page(v->path);
}
/* back to the Files page the move or copy started on, read again */
static void fm_op_home(void)
{
    while (npages > 1 && pages[npages - 1] != fm_op.origin)
    {
        bool found = false;
        for (int i = 0; i < npages; i++) if (pages[i] == fm_op.origin) found = true;
        if (!found) break;
        page_pop();
    }
    fm_reload(fm_find(fm_op.from));
}
static void fm_copy_begin(int mode);
static void fm_move_run(int mode)
{
    int moved = 0, failed = 0, skipped = 0;
    bool music = false;
    char to[MAX_PATH];
    for (int i = 0; i < fm_op.n; i++)
    {
        const char *src = fm_op.src[i];
        bool dir = dir_exists(src);
        if (!fm_join(to, sizeof to, fm_op.dest, fm_base(src))) { failed++; continue; }
        if (fm_exists(to))
        {
            if (mode != FM_KEEP_BOTH || !fm_free_name(to, sizeof to, fm_op.dest, fm_base(src), dir)) { skipped++; continue; }
        }
        if (rename(src, to) != 0) { failed++; sl_log("files: move failed %s -> %s", src, to); continue; }
        sl_log("files: moved %s -> %s", src, to);
        moved++;
        if (dir || sl_audio_name(src)) music = true;
    }
    char b[96];
    if (failed) snprintf(b, sizeof b, sl_tr(failed == 1 ? "Could not move %d item" : "Could not move %d items"), failed);
    else if (skipped && !moved) strlcpy(b, sl_tr("Nothing moved"), sizeof b);
    else snprintf(b, sizeof b, sl_tr(moved == 1 ? "Moved %d item" : "Moved %d items"), moved);
    failed ? toast_warn(b) : toast(b);
    fm_op_clear();
    fm_op_home();
    if (music) db_update_begin(false);
}
static void fm_op_run(int mode)
{
    if (fm_op.kind == FM_MOVE) fm_move_run(mode);
    else fm_copy_begin(mode);
}
static void fm_conflict_cb(lv_event_t *e)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    page_pop();
    if (mode == FM_ASK) return;          /* Cancel: back to choosing a folder */
    fm_op_run(mode);
}
/* some are already there: keep both or skip them; nothing is replaced */
static void fm_conflict_page(int n)
{
    lv_obj_t *p, *body = page_new("Already there", &p);
    char b[300];
    const char *where = strcmp(fm_op.dest, "/") ? fm_base(fm_op.dest) : sl_tr("Files");
    if (n == 1) snprintf(b, sizeof b, sl_tr("One of these is already in %s. Nothing there is replaced."), where);
    else snprintf(b, sizeof b, sl_tr("%d of these are already in %s. Nothing there is replaced."), n, where);
    lv_obj_t *g = set_group(body, NULL);
    set_text(g, b, C_TEXT);
    g = set_group(body, NULL);
    set_action_sub(g, LV_SYMBOL_COPY, 0x4e9e8f, "Keep both", "The new ones get a number", fm_conflict_cb, (void *)FM_KEEP_BOTH);
    set_action_sub(g, LV_SYMBOL_NEXT, 0x6a7280, "Skip them", "Only the others go", fm_conflict_cb, (void *)FM_SKIP);
    set_action(g, LV_SYMBOL_CLOSE, 0x6a7280, "Cancel", NULL, fm_conflict_cb, (void *)FM_ASK);
    page_push(p);
}
static void fm_here_cb(lv_event_t *e)
{
    struct fm_view *v = lv_event_get_user_data(e);
    strlcpy(fm_op.dest, v->path, sizeof fm_op.dest);
    int clash = 0, here = 0;
    char to[MAX_PATH], parent[MAX_PATH];
    size_t dl = strlen(fm_op.dest);
    for (int i = 0; i < fm_op.n; i++)
    {
        const char *src = fm_op.src[i];
        size_t sl = strlen(src);
        /* a folder into itself, or into one of its own */
        if (dir_exists(src) && !strncmp(fm_op.dest, src, sl) && (dl == sl || fm_op.dest[sl] == '/'))
        { toast("A folder can't go inside itself"); return; }
        fm_parent(parent, sizeof parent, src);
        if (fm_op.kind == FM_MOVE && !strcmp(parent, fm_op.dest)) { here++; continue; }
        if (fm_join(to, sizeof to, fm_op.dest, fm_base(src)) && fm_exists(to)) clash++;
    }
    if (here == fm_op.n) { toast("They're in this folder already"); return; }
    if (clash) fm_conflict_page(clash);
    else fm_op_run(FM_ASK);
}
static void fm_dest_cancel_cb(lv_event_t *e)
{
    (void)e;
    fm_op_clear();
    while (npages > 1 && pages[npages - 1] != fm_op.origin) page_pop();
}
static void fm_dest_new_cb(lv_event_t *e) { fm_name_page(lv_event_get_user_data(e), -1); }
/* the foot of a destination: what goes, and the button that sends it here */
static void fm_dest_bar(struct fm_view *v)
{
    lv_obj_t *bar = box(v->page);
    lv_obj_set_size(bar, W - 2 * SL_FLOAT, 82);   /* where the mini player and the picking bar sit */
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -SL_FLOAT);
    lv_obj_set_style_radius(bar, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    hairline(bar);
    sl_elevate(bar, SL_E3, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    char b[64];
    sl_count(b, sizeof b, fm_op.n, "%s item", "%s items");
    int pw = 170;
    lv_obj_t *l = text(bar, sl_case(b), &ss_semibold_21, 0xffffff);
    lv_obj_set_width(l, W - 2 * SL_FLOAT - 2 * SL_INSET - pw - SL_SP_S);
    label_lines(l, 1);
    sl_align(l, LV_ALIGN_LEFT_MID, SL_INSET, 0);
    lv_obj_t *go = mn_pill(bar, fm_op.kind == FM_MOVE ? "Move here" : "Copy here", pw, 2 * SL_R_PILL - 4, true, fm_here_cb, v);
    sl_align(go, LV_ALIGN_RIGHT_MID, -(82 - (2 * SL_R_PILL - 4)) / 2, 0);
    lv_obj_set_style_pad_bottom(v->body, 82 + 2 * SL_FLOAT + SL_SP_S, 0);
}
static void fm_dest_page(const char *path) { fm_open(path, true); }
/* a destination's own bar holds the foot: the mini player steps aside */
static bool fm_dest_on_top(void)
{
    for (int i = 0; npages && i < MAX_PAGES; i++)
        if (fm_views[i] && fm_views[i]->dest && fm_views[i]->page == pages[npages - 1]) return true;
    return false;
}

/* the copy itself: a list of folders to make and files to copy, run a few
 * milliseconds a frame, each file into a hidden .part, named when whole */
struct fm_cp_item { uint32_t src, dst; long long size; bool dir; };
static struct {
    struct fm_cp_item *it;
    int n, cap, at, copied, files;
    char *pool;
    size_t pn, pcap;
    int in, out;
    char part[MAX_PATH];
    long long total, done, file_done;
    unsigned char *buf;
    bool running, music, failed;
    lv_timer_t *timer;
    lv_obj_t *page, *count, *name, *fill, *bytes;
} cp = { .in = -1, .out = -1 };
static uint32_t cp_str(const char *s)
{
    size_t len = strlen(s) + 1;
    if (cp.pn + len > cp.pcap)
    {
        size_t cap = cp.pcap ? cp.pcap * 2 : 16384;
        while (cap < cp.pn + len) cap *= 2;
        char *p = realloc(cp.pool, cap);
        if (!p) return UINT32_MAX;
        cp.pool = p;
        cp.pcap = cap;
    }
    memcpy(cp.pool + cp.pn, s, len);
    cp.pn += len;
    return (uint32_t)(cp.pn - len);
}
static bool cp_add(const char *src, const char *dst, long long size, bool dir)
{
    if (cp.n >= FM_COPY_MAX) return false;
    if (cp.n == cp.cap)
    {
        int cap = cp.cap ? cp.cap * 2 : 64;
        struct fm_cp_item *it = realloc(cp.it, (size_t)cap * sizeof *it);
        if (!it) return false;
        cp.it = it;
        cp.cap = cap;
    }
    uint32_t s = cp_str(src), d = cp_str(dst);
    if (s == UINT32_MAX || d == UINT32_MAX) return false;
    cp.it[cp.n++] = (struct fm_cp_item){ s, d, size, dir };
    if (!dir) { cp.total += size; cp.files++; }
    if (dir || sl_audio_name(src)) cp.music = true;
    return true;
}
/* a folder's contents under dst, folders made before what goes in them */
static bool cp_walk(const char *src, const char *dst, int depth)
{
    if (depth > FM_DEPTH) return false;
    struct fm_view t = { 0 };
    if (!fm_read(&t, src, false) || t.partial) { fm_view_free_lists(&t); return false; }
    bool ok = true;
    char s[MAX_PATH], d[MAX_PATH];
    for (int k = 0; k < t.n && ok; k++)
    {
        ok = fm_join(s, sizeof s, src, fm_name(&t, k)) && fm_join(d, sizeof d, dst, fm_name(&t, k)) &&
             cp_add(s, d, t.e[k].size, t.e[k].kind == FK_DIR);
        if (ok && t.e[k].kind == FK_DIR) ok = cp_walk(s, d, depth + 1);
    }
    fm_view_free_lists(&t);
    return ok;
}
static void cp_close_files(bool keep)
{
    if (cp.in >= 0) close(cp.in);
    if (cp.out >= 0) close(cp.out);
    if (cp.out >= 0 && !keep) remove(cp.part);
    cp.in = cp.out = -1;
}
static void cp_ui(void)
{
    if (!cp.page) return;
    char b[96], d[24], t[24];
    snprintf(b, sizeof b, sl_tr("Copying %d of %d"), LV_MIN(cp.copied + 1, LV_MAX(cp.files, 1)), LV_MAX(cp.files, 1));
    label_if_changed(cp.count, b);
    if (cp.at < cp.n) label_if_changed(cp.name, fm_base(cp.pool + cp.it[cp.at].src));
    fm_size(d, sizeof d, cp.done);
    fm_size(t, sizeof t, cp.total);
    snprintf(b, sizeof b, sl_tr("%s of %s"), d, t);
    label_if_changed(cp.bytes, b);
    int full = W - 2 * SL_GUTTER - 2 * SL_INSET - 2;
    lv_obj_set_width(cp.fill, LV_MAX(8, (int)(cp.total ? (long long)full * cp.done / cp.total : full)));
}
static void cp_finish(const char *why)
{
    bool music = cp.music && cp.copied, failed = cp.failed;
    int copied = cp.copied, files = cp.files;
    cp_close_files(false);
    if (cp.timer) lv_timer_pause(cp.timer);
    free(cp.it); free(cp.pool); free(cp.buf);
    cp.it = NULL; cp.pool = NULL; cp.buf = NULL;
    cp.n = cp.cap = cp.at = 0; cp.pn = cp.pcap = 0;
    cp.running = false;
    sl_log("files: copy %s, %d of %d files", why, copied, files);
    char b[96];
    if (failed) toast_warn("Could not copy everything. Check card space.");
    else if (!strcmp(why, "stopped"))
    {
        snprintf(b, sizeof b, sl_tr("Copy stopped: %d of %d copied"), copied, files);
        toast(b);
    }
    else
    {
        snprintf(b, sizeof b, sl_tr(copied == 1 ? "Copied %d file" : "Copied %d files"), copied);
        toast(b);
    }
    if (cp.page) { lv_obj_t *p = cp.page; cp.page = NULL; if (npages > 1 && pages[npages - 1] == p) page_pop(); }
    fm_op_home();
    if (music) db_update_begin(false);
}
static void cp_tick(lv_timer_t *t)
{
    (void)t;
    if (!cp.running) return;
    uint32_t until = sl_ms() + 12;
    while ((int32_t)(sl_ms() - until) < 0)
    {
        if (cp.out < 0)
        {
            if (cp.at >= cp.n) { cp_finish("done"); return; }
            struct fm_cp_item *it = &cp.it[cp.at];
            const char *src = cp.pool + it->src, *dst = cp.pool + it->dst;
            if (it->dir)
            {
                if (mkdir(dst) != 0 && !dir_exists(dst)) { cp.failed = true; cp_finish("failed"); return; }
                cp.at++;
                continue;
            }
            char dir[MAX_PATH];
            fm_parent(dir, sizeof dir, dst);
            snprintf(cp.part, sizeof cp.part, "%s/.%s.part", strcmp(dir, "/") ? dir : "", fm_base(dst));
            remove(cp.part);                     /* one of ours, left by a copy that never finished */
            cp.in = open(src, O_RDONLY);
            cp.out = cp.in >= 0 ? open(cp.part, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0666) : -1;
            if (cp.in < 0 || cp.out < 0) { sl_log("files: copy could not open %s", src); cp_close_files(false); cp.failed = true; cp_finish("failed"); return; }
            cp.file_done = 0;
            cp_ui();
        }
        ssize_t r = read(cp.in, cp.buf, 64 * 1024);
        if (r < 0 || (r > 0 && write(cp.out, cp.buf, (size_t)r) != r))
        { cp_close_files(false); cp.failed = true; cp_finish("failed"); return; }
        if (r > 0) { cp.done += r; cp.file_done += r; continue; }
        /* whole: named, unless something took the name meanwhile */
        bool ok = fsync(cp.out) == 0;
        close(cp.in); cp.in = -1;
        if (close(cp.out) < 0) ok = false;
        cp.out = -1;
        const char *dst = cp.pool + cp.it[cp.at].dst;
        char final[MAX_PATH], dir[MAX_PATH];
        fm_parent(dir, sizeof dir, dst);
        if (ok) ok = fm_free_name(final, sizeof final, dir, fm_base(dst), false) && rename(cp.part, final) == 0;
        if (!ok) { remove(cp.part); cp.failed = true; cp_finish("failed"); return; }
        cp.copied++;
        cp.at++;
    }
    cp_ui();
}
static void cp_stop_cb(lv_event_t *e) { (void)e; if (cp.running) cp_finish("stopped"); }
/* A long job's page (a copy, a delete): what is at, its name, a bar, a
 * line under it, and Stop. Pushed; returns the page. */
static lv_obj_t *fm_progress_page(const char *title, lv_obj_t **count, lv_obj_t **name, lv_obj_t **fill, lv_obj_t **line,
                                  lv_event_cb_t stop_cb, lv_event_cb_t deleted_cb)
{
    lv_obj_t *p, *body = page_new(title, &p);
    lv_obj_add_event_cb(p, deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_t *c = box(g);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, SL_INSET, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);   /* one job, centred over its Stop */
    lv_obj_set_style_pad_row(c, SL_SP_S, 0);
    *count = text(c, "", &ss_semibold_23, 0xffffff);
    *name = text(c, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(*name, lv_pct(100));
    lv_obj_set_style_text_align(*name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    lv_obj_t *track = box(c);
    lv_obj_set_size(track, lv_pct(100), 8);
    lv_obj_set_style_radius(track, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(track, SL_SP_S, 0);
    *fill = box(track);
    lv_obj_set_size(*fill, 8, 8);
    lv_obj_set_style_radius(*fill, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(*fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    sl_align(*fill, LV_ALIGN_LEFT_MID, 0, 0);
    *line = text(c, "", &ss_regular_16, C_SUB);
    lv_obj_t *row = box(body);
    lv_obj_set_size(row, lv_pct(100), 2 * SL_R_PILL + 2 * SL_GUTTER);
    lv_obj_t *stop = mn_pill(row, "Stop", W - 2 * SL_GUTTER, 2 * SL_R_PILL, false, stop_cb, NULL);
    lv_obj_center(stop);
    page_push(p);
    return p;
}
static void cp_page_deleted(lv_event_t *e) { (void)e; cp.page = NULL; }
/* USB mode: the card goes to the computer; a copy lets go of it first */
static void fd_usb_release(void);
static void fm_usb_release(void)
{
    if (cp.running) cp_finish("stopped");
    fd_usb_release();
}
static void fm_copy_begin(int mode)
{
    if (cp.running) { toast("A copy is still running"); return; }
    cp.total = cp.done = 0;
    cp.files = cp.copied = 0;
    cp.music = cp.failed = false;
    bool ok = true;
    char to[MAX_PATH];
    for (int i = 0; i < fm_op.n && ok; i++)
    {
        const char *src = fm_op.src[i];
        bool dir = dir_exists(src);
        if (!fm_join(to, sizeof to, fm_op.dest, fm_base(src))) continue;
        if (fm_exists(to) && (mode != FM_KEEP_BOTH || !fm_free_name(to, sizeof to, fm_op.dest, fm_base(src), dir))) continue;
        struct fm_view one = { 0 };
        long long size = 0;
        if (!dir)
        {
            /* its size, from its folder's listing */
            char parent[MAX_PATH];
            fm_parent(parent, sizeof parent, src);
            if (fm_read(&one, parent, false))
                for (int k = 0; k < one.n; k++) if (!strcmp(fm_name(&one, k), fm_base(src))) size = one.e[k].size;
            fm_view_free_lists(&one);
        }
        ok = cp_add(src, to, size, dir) && (!dir || cp_walk(src, to, 0));
    }
    fm_op_clear();
    if (!ok)
    {
        free(cp.it); free(cp.pool);
        cp.it = NULL; cp.pool = NULL; cp.n = cp.cap = 0; cp.pn = cp.pcap = 0;
        toast_warn("Too many files to copy at once");
        return;
    }
    if (!cp.n) { toast("Nothing copied"); fm_op_home(); return; }
    cp.buf = malloc(64 * 1024);
    if (!cp.buf) { toast_warn("Not enough memory"); return; }
    cp.at = 0;
    cp.running = true;
    sl_log("files: copying %d files, %lld bytes, to %s", cp.files, cp.total, fm_op.dest);
    /* the progress, on a page of its own over the destination */
    cp.page = fm_progress_page("Copying", &cp.count, &cp.name, &cp.fill, &cp.bytes, cp_stop_cb, cp_page_deleted);
    cp_ui();
    if (!cp.timer) cp.timer = lv_timer_create(cp_tick, 1, NULL);
    lv_timer_resume(cp.timer);
}


/* ------------------------------------------------------------ delete */
#define FM_DEL_SHOWN 5                   /* names a several-item delete lists; then "and N more" */
static bool fm_del_allowed(void) { return global_settings.sloppy_files_delete != 0; }
/* never: the card itself, a path that climbs out of it, or GabagoolOS's own folder */
static bool fm_del_refused(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    if (n < 2 || path[0] != '/') return true;
    if (strstr(path, "/../") || (n >= 3 && !strcmp(path + n - 3, "/..")) || strstr(path, "//")) return true;
    return !strncasecmp(path, "/.rockbox", 9) && (path[9] == '\0' || path[9] == '/');   /* FAT: any case */
}
/* path is top, or inside the folder top */
static bool fm_under(const char *path, const char *top)
{
    size_t n = strlen(top);
    return !strncmp(path, top, n) && (path[n] == '\0' || path[n] == '/');
}
static struct {
    char **src;                          /* what was picked, whole paths */
    int n;
    lv_obj_t *origin;                    /* the Files page it started on */
    char from[MAX_PATH];
    /* the run: every file, then each folder after what is in it */
    uint32_t *it;                        /* each one's path: an offset into pool */
    bool *dir;                           /* and whether it is a folder */
    int items, cap, at, gone, failed;
    char *pool;
    size_t pn, pcap;
    bool running, music, stopped;
    lv_timer_t *timer;
    lv_obj_t *page, *count, *name, *fill, *line;
} fdl = { 0 };
static void fd_clear_src(void)
{
    for (int i = 0; i < fdl.n; i++) free(fdl.src[i]);
    free(fdl.src);
    fdl.src = NULL;
    fdl.n = 0;
}
static void fd_clear_run(void)
{
    free(fdl.it); free(fdl.dir); free(fdl.pool);
    fdl.it = NULL; fdl.dir = NULL; fdl.pool = NULL;
    fdl.items = fdl.cap = fdl.at = 0;
    fdl.pn = fdl.pcap = 0;
}
static bool fd_add(const char *path, bool dir)
{
    if (fdl.items >= FM_COPY_MAX) return false;
    if (fdl.items == fdl.cap)
    {
        int cap = fdl.cap ? fdl.cap * 2 : 64;
        uint32_t *it = realloc(fdl.it, (size_t)cap * sizeof *it);
        if (!it) return false;
        fdl.it = it;
        bool *d = realloc(fdl.dir, (size_t)cap * sizeof *d);
        if (!d) return false;
        fdl.dir = d;
        fdl.cap = cap;
    }
    size_t len = strlen(path) + 1;
    if (fdl.pn + len > fdl.pcap)
    {
        size_t cap = fdl.pcap ? fdl.pcap * 2 : 16384;
        while (cap < fdl.pn + len) cap *= 2;
        char *p = realloc(fdl.pool, cap);
        if (!p) return false;
        fdl.pool = p;
        fdl.pcap = cap;
    }
    memcpy(fdl.pool + fdl.pn, path, len);
    fdl.it[fdl.items] = (uint32_t)fdl.pn;
    fdl.dir[fdl.items++] = dir;
    fdl.pn += len;
    return true;
}
/* a folder's contents, hidden ones too (a folder only goes when empty),
 * each folder after what is in it */
static bool fd_walk(const char *path, int depth)
{
    if (depth > FM_DEPTH) return false;
    DIR *d = opendir(path);
    if (!d) return false;
    struct DIRENT *en;
    bool ok = true;
    char full[MAX_PATH];
    int seen = 0;
    while (ok && (en = readdir(d)) != NULL)
    {
        const char *nm = en->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        bool dir = (dir_get_info(d, en).attribute & ATTR_DIRECTORY) != 0;
        ok = fm_join(full, sizeof full, path, nm) && (!dir || fd_walk(full, depth + 1)) && fd_add(full, dir);
        if ((++seen & 255) == 0) yield();
    }
    closedir(d);
    return ok;
}
/* the song playing, when one is and it is among what goes */
static bool fd_hits_playing(char **src, int n)
{
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    for (int i = 0; id3 && id3->path[0] && i < n; i++)
        if (fm_under(id3->path, src[i])) return true;
    return false;
}
/* the queue lets go of what is no longer on the card */
static int fd_queue_drop(void)
{
    int dropped = 0;
    struct playlist_track_info info;
    for (int idx = playlist_amount() - 1; idx >= 0; idx--)
    {
        if (playlist_get_track_info(NULL, idx, &info) < 0) continue;
        bool hit = false;
        for (int i = 0; i < fdl.n && !hit; i++) hit = fm_under(info.filename, fdl.src[i]);
        if (hit && !file_exists(info.filename) && playlist_delete(NULL, idx) >= 0) dropped++;
        if ((idx & 15) == 0) yield();
    }
    if (dropped) { sl_log("files: %d gone from the queue", dropped); if (uq_open) uq_rebuild_soon(); }
    return dropped;
}
static void fd_ui(void)
{
    if (!fdl.page) return;
    char b[96];
    snprintf(b, sizeof b, sl_tr("Deleting %d of %d"), LV_MIN(fdl.at + 1, LV_MAX(fdl.items, 1)), LV_MAX(fdl.items, 1));
    label_if_changed(fdl.count, b);
    if (fdl.at < fdl.items) label_if_changed(fdl.name, fm_base(fdl.pool + fdl.it[fdl.at]));
    int full = W - 2 * SL_GUTTER - 2 * SL_INSET - 2;
    lv_obj_set_width(fdl.fill, LV_MAX(8, fdl.items ? full * fdl.at / fdl.items : full));
}
static void fd_finish(void)
{
    int gone = fdl.gone, failed = fdl.failed, items = fdl.items;
    bool music = fdl.music, stopped = fdl.stopped;
    if (fdl.timer) lv_timer_pause(fdl.timer);
    fdl.running = false;
    sl_log("files: delete %s, %d of %d gone, %d failed", stopped ? "stopped" : "done", gone, items, failed);
    /* what was picked and is gone now: the count the toast says */
    int picked = 0;
    for (int i = 0; i < fdl.n; i++) if (!fm_exists(fdl.src[i])) picked++;
    fd_queue_drop();
    char b[96];
    if (failed) toast_warn("Could not delete everything");
    else if (stopped)
    {
        snprintf(b, sizeof b, sl_tr("Delete stopped: %d of %d deleted"), gone, items);
        toast(b);
    }
    else
    {
        snprintf(b, sizeof b, sl_tr(picked == 1 ? "Deleted %d item" : "Deleted %d items"), picked);
        toast(b);
    }
    fd_clear_run();
    fd_clear_src();
    if (fdl.page) { lv_obj_t *p = fdl.page; fdl.page = NULL; if (npages > 1 && pages[npages - 1] == p) page_pop(); }
    while (npages > 1 && pages[npages - 1] != fdl.origin)
    {
        bool found = false;
        for (int i = 0; i < npages; i++) if (pages[i] == fdl.origin) found = true;
        if (!found) break;
        page_pop();
    }
    fm_reload(fm_find(fdl.from));
    if (music) db_update_begin(false);
}
/* a few milliseconds of deleting a frame, or all of it at once (budget
 * 0); at most max items (0: no limit) */
static void fd_run(uint32_t budget, int max)
{
    uint32_t until = sl_ms() + budget;
    for (int done = 0; fdl.running && fdl.at < fdl.items && (!budget || (int32_t)(sl_ms() - until) < 0) && (!max || done < max); done++)
    {
        const char *path = fdl.pool + fdl.it[fdl.at];
        bool dir = fdl.dir[fdl.at];
        if ((dir ? rmdir(path) : remove(path)) == 0)
        {
            fdl.gone++;
            if (!dir && sl_audio_name(path)) fdl.music = true;
        }
        else { fdl.failed++; sl_log("files: could not delete %s", path); }
        fdl.at++;
    }
    if (fdl.at >= fdl.items) { fd_finish(); return; }
    fd_ui();
}
static void fd_tick(lv_timer_t *t) { (void)t; if (fdl.running) fd_run(12, 0); }
static void fd_stop_cb(lv_event_t *e) { (void)e; if (fdl.running) { fdl.stopped = true; fd_finish(); } }
static void fd_page_deleted(lv_event_t *e) { (void)e; fdl.page = NULL; }
static void fd_go_cb(lv_event_t *e)
{
    (void)e;
    if (fdl.running || cp.running) { toast("Wait for the copy or delete running now"); return; }
    if (!fdl.n) { page_pop(); return; }
    page_pop();                          /* the question */
    fd_clear_run();
    bool folders = false, ok = true;
    for (int i = 0; i < fdl.n && ok; i++)
    {
        bool dir = dir_exists(fdl.src[i]);
        folders |= dir;
        ok = (!dir || fd_walk(fdl.src[i], 0)) && fd_add(fdl.src[i], dir);
    }
    if (!ok) { fd_clear_run(); fd_clear_src(); toast_warn("Too many files to delete at once"); return; }
    /* the song playing among them: it stops first, so nothing holds the file */
    if (fd_hits_playing(fdl.src, fdl.n)) { sl_log("files: the playing song is being deleted: stopped"); audio_stop(); }
    fdl.at = fdl.gone = fdl.failed = 0;
    fdl.music = fdl.stopped = false;
    fdl.running = true;
    sl_log("files: deleting %d items under %d picked", fdl.items, fdl.n);
    if (!folders) { fd_run(0, 0); return; }   /* files alone: at once */
    fdl.page = fm_progress_page("Deleting", &fdl.count, &fdl.name, &fdl.fill, &fdl.line, fd_stop_cb, fd_page_deleted);
    lv_obj_add_flag(fdl.line, LV_OBJ_FLAG_HIDDEN);
    fd_ui();
    if (!fdl.timer) fdl.timer = lv_timer_create(fd_tick, 1, NULL);
    lv_timer_resume(fdl.timer);
}
static void fd_keep_cb(lv_event_t *e) { (void)e; fd_clear_src(); page_pop(); }
/* USB mode: the card goes to the computer; a delete lets go of it first */
static void fd_usb_release(void)
{
    if (fdl.running) { fdl.stopped = true; fd_finish(); }
}
/* the question: what goes, by name; that it can't be undone; that what is
 * playing stops first when it is among it. A red Delete and a Cancel. */
static void fm_delete_ask(struct fm_view *v, const int *ks, int n)
{
    if (!fm_del_allowed() || !n) return;
    if (fdl.running || cp.running) { toast("Wait for the copy or delete running now"); return; }
    fd_clear_src();
    fdl.src = calloc((size_t)n, sizeof *fdl.src);
    if (!fdl.src) { toast_warn("Not enough memory"); return; }
    char full[MAX_PATH];
    for (int i = 0; i < n; i++)
    {
        if (!fm_join(full, sizeof full, v->path, fm_name(v, ks[i])) || fm_del_refused(full))
        {
            sl_log("files: refused to delete %s", full);
            fd_clear_src();
            toast_warn("GabagoolOS's own folder can't be deleted");
            return;
        }
        if ((fdl.src[fdl.n] = strdup(full))) fdl.n++;
    }
    fdl.origin = v->page;
    strlcpy(fdl.from, v->path, sizeof fdl.from);
    char t[MAX_PATH + 32], b[96];
    if (n == 1) snprintf(t, sizeof t, sl_tr("Delete \"%s\"?"), fm_name(v, ks[0]));
    else snprintf(t, sizeof t, sl_tr("Delete %d items?"), n);
    lv_obj_t *p, *body = page_new(t, &p);
    lv_obj_t *g = set_group(body, NULL);
    set_text(g, n == 1 ? "This removes it from the card and can't be undone."
                       : "This removes them from the card and can't be undone.", C_TEXT);
    for (int i = 0; i < n && i < FM_DEL_SHOWN; i++)
    {
        int k = ks[i];
        char line[64];
        fm_line(line, sizeof line, v, k);
        lv_obj_t *r = set_info(g, fm_symbol(v->e[k].kind), fm_tint(v->e[k].kind), "", line, true);
        lv_label_set_text(lv_obj_get_child(r, 0), fm_name(v, k));
    }
    if (n > FM_DEL_SHOWN)
    {
        snprintf(b, sizeof b, sl_tr("and %d more"), n - FM_DEL_SHOWN);
        set_text(g, b, C_SUB);
    }
    if (fd_hits_playing(fdl.src, fdl.n))
        set_info(g, LV_SYMBOL_WARNING, 0xf0602f, "Playing now", "The song playing is in this. It stops first.", true);
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_LEFT, 0x6a7280, "Cancel", NULL, fd_keep_cb, NULL);
    set_danger(g, LV_SYMBOL_TRASH, "Delete", fd_go_cb, NULL);
    page_push(p);
}

/* ------------------------------------------------------------ the sheets */
enum { FA_PLAY, FA_SHUFFLE, FA_NEXT, FA_QUEUE, FA_PLAYLIST, FA_RENAME, FA_MOVE, FA_COPY, FA_SELECT, FA_OPEN,
       FA_NEWFOLDER, FA_PLAYALL, FA_SHUFFLEALL, FA_DELETE };
static struct fm_view *fm_acts_view;
static bool fm_live(struct fm_view *v)
{
    for (int i = 0; v && i < MAX_PAGES; i++) if (fm_views[i] == v) return true;
    return false;
}
static void fm_open_entry(struct fm_view *v, int k);
/* what to do with the picked entries ks (or the one held) */
static void fm_do(struct fm_view *v, int act, const int *ks, int n)
{
    struct fm_paths ps = { 0 };
    switch (act)
    {
        case FA_PLAY: case FA_SHUFFLE: case FA_NEXT: case FA_QUEUE: case FA_PLAYLIST: case FA_PLAYALL: case FA_SHUFFLEALL:
            if (act == FA_PLAYALL || act == FA_SHUFFLEALL) { for (int k = v->folders; k < v->n; k++) if (v->e[k].kind == FK_AUDIO) fm_gather_entry(&ps, v, k); }
            else for (int i = 0; i < n && !ps.capped; i++) fm_gather_entry(&ps, v, ks[i]);
            if (!ps.count) { toast("No songs here"); break; }
            if (act == FA_PLAYLIST)
            {
                char title[MAX_PATH];
                if (n == 1) strlcpy(title, fm_name(v, ks[0]), sizeof title);
                else sl_count(title, sizeof title, n, "%s item", "%s items");
                pl_open_paths(&ps, title);      /* the sheet takes the list */
                return;
            }
            if (act == FA_QUEUE || act == FA_NEXT) fm_queue_paths(&ps, act == FA_NEXT);
            else fm_play_paths(&ps, 0, act == FA_SHUFFLE || act == FA_SHUFFLEALL);
            if (ps.capped && act != FA_QUEUE)
            {
                char b[96], num[16];
                snprintf(b, sizeof b, sl_tr("Added the first %s songs"), sl_fmt_count(num, sizeof num, ps.count));
                toast(b);
            }
            break;
        case FA_RENAME: if (n == 1) fm_name_page(v, ks[0]); break;
        case FA_NEWFOLDER: fm_name_page(v, -1); break;
        case FA_MOVE: case FA_COPY: fm_op_start(act == FA_MOVE ? FM_MOVE : FM_COPY, v, ks, n); break;
        case FA_SELECT: sel_start(SEL_FILES, v, v->page, n ? ks[0] : -1); break;
        case FA_OPEN: if (n == 1) fm_open_entry(v, ks[0]); break;
        case FA_DELETE: fm_delete_ask(v, ks, n); break;
    }
    fm_paths_free(&ps);
}
static void fm_act_cb(lv_event_t *e)
{
    intptr_t u = (intptr_t)lv_event_get_user_data(e);
    int k = (int)(u >> 5), act = (int)(u & 31);
    struct fm_view *v = fm_acts_view;
    acts_close();
    if (!fm_live(v) || k >= v->n) return;
    if (k < 0) fm_do(v, act, NULL, 0);
    else fm_do(v, act, &k, 1);
}
static void fm_acts_row(int k, int act, const char *sym, const char *label)
{
    acts_row_cb(sym, label, fm_act_cb, ((intptr_t)k << 5) | act);
}
/* the sheet's last row, Delete, in the red Settings' danger rows wear */
static void fm_acts_red(void)
{
    lv_obj_t *r = lv_obj_get_child(acts, -1);
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_text_color(lv_obj_get_child(r, i), lv_color_hex(sl_zune() ? 0xff6b6e : SET_RED), 0);
}
static void fm_acts_danger(int k)
{
    fm_acts_row(k, FA_DELETE, LV_SYMBOL_TRASH, "Delete");
    fm_acts_red();
}
/* a held entry's sheet: its tile, name and line, then what it can do */
static void fm_acts(struct fm_view *v, int k)
{
    fm_acts_view = v;
    acts_begin();
    lv_obj_t *hd = box(acts);
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_16);
    lv_obj_set_size(hd, lv_pct(100), LV_MAX(84, 12 + lh1 + 4 + lh2 + 12));
    sl_align(fm_tile(hd, v, k, SL_ART_ROW), LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *t1 = text(hd, "", &ss_semibold_23, 0xffffff);
    lv_label_set_text(t1, fm_name(v, k));
    lv_obj_set_width(t1, W - 130);
    label_lines(t1, 1);
    sl_place(t1, 80, 12);
    char line[64];
    fm_line(line, sizeof line, v, k);
    lv_obj_t *t2 = text(hd, line, &ss_regular_16, C_SUB);
    lv_obj_set_width(t2, W - 130);
    label_lines(t2, 1);
    sl_place(t2, 80, 12 + lh1 + 4);
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);
    lv_obj_set_style_bg_opa(sep, sl_zune() ? LV_OPA_COVER : 36, 0);
    int kind = v->e[k].kind;
    if (kind == FK_DIR)
    {
        fm_acts_row(k, FA_PLAY, LV_SYMBOL_PLAY, "Play");
        fm_acts_row(k, FA_SHUFFLE, LV_SYMBOL_SHUFFLE, "Shuffle");
        fm_acts_row(k, FA_QUEUE, LV_SYMBOL_LIST, "Add to queue");
        fm_acts_row(k, FA_PLAYLIST, LV_SYMBOL_PLUS, "Add to playlist");
    }
    else if (kind == FK_AUDIO)
    {
        fm_acts_row(k, FA_NEXT, LV_SYMBOL_NEXT, "Play next");
        fm_acts_row(k, FA_QUEUE, LV_SYMBOL_LIST, "Add to queue");
        fm_acts_row(k, FA_PLAYLIST, LV_SYMBOL_PLUS, "Add to playlist");
    }
    else if (kind != FK_OTHER) fm_acts_row(k, FA_OPEN, LV_SYMBOL_EYE_OPEN, "Open");
    fm_acts_row(k, FA_RENAME, LV_SYMBOL_EDIT, "Rename");
    fm_acts_row(k, FA_MOVE, LV_SYMBOL_DIRECTORY, "Move to…");
    fm_acts_row(k, FA_COPY, LV_SYMBOL_COPY, "Copy to…");
    fm_acts_row(k, FA_SELECT, LV_SYMBOL_OK, "Select");
    if (fm_del_allowed()) fm_acts_danger(k);
    acts_show();
}
/* the header's "more": the folder's own actions */
static void fm_menu_cb(lv_event_t *e)
{
    struct fm_view *v = lv_event_get_user_data(e);
    if (!fm_live(v)) return;
    fm_acts_view = v;
    acts_begin();
    char b[96], f1[40], f2[40];
    snprintf(b, sizeof b, "%s \xc2\xb7 %s", sl_count(f1, sizeof f1, v->folders, "%s folder", "%s folders"),
             sl_count(f2, sizeof f2, v->n - v->folders, "%s file", "%s files"));
    lv_obj_t *hd = text(acts, b, &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_all(hd, 12, 0);
    bool songs = false;
    for (int k = v->folders; k < v->n && !songs; k++) songs = v->e[k].kind == FK_AUDIO;
    if (songs)
    {
        fm_acts_row(-1, FA_PLAYALL, LV_SYMBOL_PLAY, "Play all");
        fm_acts_row(-1, FA_SHUFFLEALL, LV_SYMBOL_SHUFFLE, "Shuffle");
    }
    fm_acts_row(-1, FA_NEWFOLDER, LV_SYMBOL_PLUS, "New folder");
    if (v->n) fm_acts_row(-1, FA_SELECT, LV_SYMBOL_OK, "Select");
    acts_show();
}
static void fm_long_cb(lv_event_t *e)
{
    struct fm_view *v = lv_event_get_user_data(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    lv_indev_wait_release(lv_indev_active());
    if (sel_on() || k < 0 || k >= v->n) return;   /* picking: a hold is a tap */
    fm_acts(v, k);
}

/* the picking bar's work, for files (sloppy_playback.inc calls these) */
static int fm_sel_list(int **out)
{
    struct fm_view *v = sel_view;
    int *l = malloc((size_t)(sel_count ? sel_count : 1) * sizeof *l), n = 0;
    for (int k = 0; l && v && k < v->n && n < sel_count; k++) if (sel_has(k)) l[n++] = k;
    *out = l;
    return l ? n : 0;
}
static void fm_sel_do(int act)
{
    struct fm_view *v = sel_view;
    int *l, n = fm_sel_list(&l);
    sel_end();
    if (n && fm_live(v)) fm_do(v, act, l, n);
    free(l);
}
static void fm_sel_more_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    if (sel_kind_on(SEL_FILES)) fm_sel_do(act);
}
static void fm_sel_more(void)
{
    acts_begin();
    char b[48];
    lv_obj_t *hd = text(acts, sl_count(b, sizeof b, sel_count, "%s item", "%s items"), &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_all(hd, 12, 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", fm_sel_more_cb, FA_SHUFFLE);
    if (sel_count == 1) acts_row_cb(LV_SYMBOL_EDIT, "Rename", fm_sel_more_cb, FA_RENAME);
    acts_row_cb(LV_SYMBOL_DIRECTORY, "Move to…", fm_sel_more_cb, FA_MOVE);
    acts_row_cb(LV_SYMBOL_COPY, "Copy to…", fm_sel_more_cb, FA_COPY);
    if (fm_del_allowed())
    {
        acts_row_cb(LV_SYMBOL_TRASH, "Delete", fm_sel_more_cb, FA_DELETE);
        fm_acts_red();
    }
    acts_show();
}

/* ------------------------------------------------------------ text */
/* A text file, the first 16 KB of it, in paragraphs (each its own label,
 * so no one label lays out the whole file). Not UTF-8: read as Latin-1. */
static void fm_to_utf8(char *out, size_t n, const char *in, size_t len)
{
    bool utf8 = true;
    for (size_t i = 0; i < len && utf8; )
    {
        unsigned char c = (unsigned char)in[i];
        int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0) { utf8 = false; break; }
        for (int j = 1; j <= extra; j++)
            if (i + j >= len || ((unsigned char)in[i + j] & 0xc0) != 0x80) { utf8 = i + j >= len; if (!utf8) break; }
        i += (size_t)extra + 1;
    }
    size_t o = 0;
    for (size_t i = 0; i < len && o + 3 < n; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') continue;
        if (c == '\t') { out[o++] = ' '; out[o++] = ' '; continue; }
        if (c < 0x20 && c != '\n') continue;
        if (utf8 || c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
    }
    out[o] = '\0';
    if (utf8) pl_utf8_trim(out);         /* cut at 16 KB: never mid-character */
}
static lv_obj_t *fm_text_pg;             /* the text open, for the side keys */
static void fm_text_deleted(lv_event_t *e) { if (lv_event_get_target(e) == fm_text_pg) fm_text_pg = NULL; }
static void fm_text_page(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { toast_warn("File is unavailable"); return; }
    char *raw = malloc(FM_TEXT_MAX + 1), *txt = malloc(FM_TEXT_MAX * 2 + 4);
    ssize_t got = raw ? read(fd, raw, FM_TEXT_MAX + 1) : -1;
    close(fd);
    if (!raw || !txt || got < 0) { free(raw); free(txt); toast_warn("Could not open the file"); return; }
    bool more = got > FM_TEXT_MAX;
    size_t len = (size_t)(more ? FM_TEXT_MAX : got);
    size_t skip = len >= 3 && (unsigned char)raw[0] == 0xef && (unsigned char)raw[1] == 0xbb && (unsigned char)raw[2] == 0xbf ? 3 : 0;
    fm_to_utf8(txt, FM_TEXT_MAX * 2 + 4, raw + skip, len - skip);
    free(raw);
    lv_obj_t *p, *body = page_new(fm_base(path), &p);
    lv_obj_t *t = lv_obj_get_child(lv_obj_get_child(p, 0), -1);
    char low[MAX_PATH];
    if (t && lv_obj_check_type(t, &lv_label_class)) lv_label_set_text(t, sl_zune() ? sl_lower(fm_base(path), low, sizeof low) : fm_base(path));
    sl_scroll_indicator(body);
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_t *c = box(g);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, SL_INSET, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    if (!txt[0])
    {
        lv_obj_t *l = text(c, "This file is empty", &ss_regular_18, C_SUB);
        lv_obj_set_width(l, lv_pct(100));
    }
    /* paragraphs of up to ~1.5 KB, split at a line's end */
    for (char *s = txt; *s; )
    {
        size_t n = strlen(s);
        char *cut = s + n;
        if (n > 1536)
        {
            cut = s + 1536;
            while (cut > s + 512 && *cut != '\n') cut--;
            if (*cut != '\n') { cut = s + 1536; while (cut > s && ((unsigned char)*cut & 0xc0) == 0x80) cut--; }
        }
        char keep = *cut;
        *cut = '\0';
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
        lv_label_set_text(l, s);             /* a file's words: never translated */
        lv_obj_set_style_text_font(l, &ss_regular_18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_line_space(l, 4, 0);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        *cut = keep;
        s = keep == '\n' ? cut + 1 : cut;
    }
    free(txt);
    if (more) opt_note(body, "Showing the first 16 KB of this file");
    fm_text_pg = p;
    lv_obj_add_event_cb(p, fm_text_deleted, LV_EVENT_DELETE, NULL);
    page_push(p);
}

/* ------------------------------------------------------------ pictures */
/* A JPEG or PNG: read a slice a frame, decoded on a thread (sloppy_art.c),
 * then shown as the screenshot viewer shows a shot: rounded, edged, raised. */
#define FM_PIC_W (W - 2 * SL_GUTTER)
#define FM_PIC_H (H - SB_H - SL_HDR_H - 220)
static struct { int fd; unsigned char *buf; long size, got; lv_timer_t *timer; lv_obj_t *page, *frame, *spin, *info;
                uint16_t *px; lv_image_dsc_t dsc; char path[MAX_PATH]; bool decoding;
                struct fm_view *view; int k; lv_obj_t *title, *name; } pic = { .fd = -1 };
static void pic_stop_reading(void)
{
    if (pic.fd >= 0) close(pic.fd);
    pic.fd = -1;
    free(pic.buf);
    pic.buf = NULL;
}
static void pic_fail(const char *why)
{
    pic_stop_reading();
    pic.decoding = false;
    if (pic.timer) lv_timer_pause(pic.timer);
    if (!pic.frame) return;
    lv_obj_clean(pic.frame);
    pic.spin = NULL;
    lv_obj_t *l = text(pic.frame, why, &ss_regular_18, C_SUB);
    lv_obj_set_width(l, FM_PIC_W - 2 * SL_INSET);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}
static void pic_tick(lv_timer_t *t)
{
    (void)t;
    if (!pic.page) { pic_stop_reading(); lv_timer_pause(pic.timer); return; }
    if (pic.fd >= 0)
    {
        ssize_t r = read(pic.fd, pic.buf + pic.got, (size_t)LV_MIN((long)FM_SLICE, pic.size - pic.got));
        if (r <= 0) { pic_fail("Could not read this picture"); return; }
        pic.got += r;
        if (pic.got < pic.size) return;
        close(pic.fd);
        pic.fd = -1;
        unsigned char *b = pic.buf;
        pic.buf = NULL;
        int rc = sl_pic_start(b, (int)pic.size, FM_PIC_W, FM_PIC_H);
        if (rc == SL_PIC_TOO_BIG) { pic_fail("This picture is too big to open here"); return; }
        if (rc != SL_PIC_STARTED) { pic_fail(rc == SL_PIC_BUSY ? "Another picture is still opening" : "GabagoolOS can't read this picture"); return; }
        pic.decoding = true;
        return;
    }
    if (!pic.decoding) { lv_timer_pause(pic.timer); return; }
    uint16_t *px;
    int w, h, iw, ih, rc = sl_pic_poll(&px, &w, &h, &iw, &ih);
    if (!rc) return;
    pic.decoding = false;
    lv_timer_pause(pic.timer);
    if (rc < 0) { pic_fail("GabagoolOS can't read this picture"); return; }
    free(pic.px);
    pic.px = px;
    pic.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    pic.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    pic.dsc.header.w = (uint32_t)w;
    pic.dsc.header.h = (uint32_t)h;
    pic.dsc.header.stride = (uint32_t)w * 2;
    pic.dsc.data = (const uint8_t *)px;
    pic.dsc.data_size = (uint32_t)(w * h * 2);
    lv_obj_clean(pic.frame);
    pic.spin = NULL;
    lv_obj_set_size(pic.frame, w, h);
    lv_obj_set_height(lv_obj_get_parent(pic.frame), h + SL_SP_S + SL_GUTTER);   /* the details right under it */
    lv_obj_t *img = lv_image_create(pic.frame);
    lv_image_set_src(img, &pic.dsc);
    lv_obj_center(img);
    lv_obj_set_style_opa(img, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(img, SL_T_PAGE, 0);
    char b[96], sz[24];
    fm_size(sz, sizeof sz, pic.size);
    snprintf(b, sizeof b, "%d \xc3\x97 %d \xc2\xb7 %s", iw, ih, sz);
    if (pic.info) lv_label_set_text(pic.info, b);
}
static void pic_page_deleted(lv_event_t *e)
{
    (void)e;
    pic.page = pic.frame = pic.spin = pic.info = pic.title = pic.name = NULL;
    pic.view = NULL;
    pic_stop_reading();
    free(pic.px);
    pic.px = NULL;
}
static void pic_spinner(void)
{
    pic.spin = lv_spinner_create(pic.frame);
    lv_obj_set_size(pic.spin, 44, 44);
    lv_obj_set_style_arc_width(pic.spin, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(pic.spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pic.spin, lv_color_hex(C_CARD_P), LV_PART_MAIN);
    lv_obj_set_style_arc_color(pic.spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(pic.spin, true, LV_PART_INDICATOR);
    lv_obj_center(pic.spin);
}
static void fm_pic_page(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { toast_warn("File is unavailable"); return; }
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || size > FM_PIC_MAX) { close(fd); toast(size > 0 ? "This picture is too big to open here" : "This file is empty"); return; }
    if (pic.page) { close(fd); return; }
    pic_stop_reading();
    pic.buf = malloc((size_t)size);
    if (!pic.buf) { close(fd); toast_warn("Not enough memory"); return; }
    pic.fd = fd;
    pic.size = size;
    pic.got = 0;
    pic.decoding = false;
    strlcpy(pic.path, path, sizeof pic.path);
    lv_obj_t *p, *body = page_new(fm_base(path), &p);
    lv_obj_t *t = lv_obj_get_child(lv_obj_get_child(p, 0), -1);
    char low[MAX_PATH];
    if (t && lv_obj_check_type(t, &lv_label_class)) lv_label_set_text(t, sl_zune() ? sl_lower(fm_base(path), low, sizeof low) : fm_base(path));
    pic.title = t && lv_obj_check_type(t, &lv_label_class) ? t : NULL;
    pic.view = NULL;
    pic.page = p;
    lv_obj_add_event_cb(p, pic_page_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *wrap = box(body);
    lv_obj_set_size(wrap, lv_pct(100), FM_PIC_W * 3 / 4 + SL_SP_S + SL_GUTTER);
    pic.frame = box(wrap);
    lv_obj_set_size(pic.frame, FM_PIC_W, FM_PIC_W * 3 / 4);
    lv_obj_align(pic.frame, LV_ALIGN_TOP_MID, 0, SL_SP_S);
    lv_obj_set_style_radius(pic.frame, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_clip_corner(pic.frame, true, 0);
    lv_obj_set_style_bg_color(pic.frame, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(pic.frame, LV_OPA_COVER, 0);
    sl_elevate(pic.frame, SL_E3, 0);
    lv_obj_set_style_border_color(pic.frame, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(pic.frame, 60, 0);
    lv_obj_set_style_border_width(pic.frame, 1, 0);
    lv_obj_set_style_border_post(pic.frame, true, 0);
    pic_spinner();
    lv_obj_t *g = set_group(body, NULL);
    char sz[24];
    fm_size(sz, sizeof sz, size);
    lv_obj_t *r = set_info(g, LV_SYMBOL_IMAGE, fm_tint(FK_PIC), fm_base(path), sz, true);
    lv_label_set_text(lv_obj_get_child(r, 0), fm_base(path));
    pic.name = lv_obj_get_child(r, 0);
    pic.info = lv_obj_get_child(r, 1);
    set_action_sub(g, LV_SYMBOL_IMAGE, 0x3d8fd6, "Set as wallpaper", "Home screen, lock screen or both", wallpaper_set_cb, pic.path);
    page_push(p);
    if (!pic.timer) pic.timer = lv_timer_create(pic_tick, 15, NULL);
    lv_timer_resume(pic.timer);
}

/* The side keys in a viewer (RD #5), as a reader's: in a picture, Volume
 * up shows the folder's picture before it and Volume down or Next the one
 * after, in the same page; in a text, Volume up and down turn its pages.
 * Elsewhere they are the volume and the next song as ever. */
static void pic_step(int dir)
{
    struct fm_view *v = pic.view;
    if (!pic.page || !pic.frame || pic.decoding || !fm_live(v)) return;   /* one opening at a time */
    int k = pic.k;
    do k += dir; while (k >= 0 && k < v->n && v->e[k].kind != FK_PIC);
    if (k < 0 || k >= v->n) return;      /* the first or last: it stays */
    char full[MAX_PATH];
    if (!fm_join(full, sizeof full, v->path, fm_name(v, k))) return;
    int fd = open(full, O_RDONLY);
    if (fd < 0) { toast_warn("File is unavailable"); return; }
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0 || size > FM_PIC_MAX) { close(fd); toast(size > 0 ? "This picture is too big to open here" : "This file is empty"); return; }
    unsigned char *buf = malloc((size_t)size);
    if (!buf) { close(fd); toast_warn("Not enough memory"); return; }
    pic_stop_reading();
    lv_obj_clean(pic.frame);             /* the last picture's image, before its pixels go */
    free(pic.px);
    pic.px = NULL;
    pic.k = k;
    pic.buf = buf;
    pic.fd = fd;
    pic.size = size;
    pic.got = 0;
    strlcpy(pic.path, full, sizeof pic.path);
    lv_obj_set_size(pic.frame, FM_PIC_W, FM_PIC_W * 3 / 4);
    lv_obj_set_height(lv_obj_get_parent(pic.frame), FM_PIC_W * 3 / 4 + SL_SP_S + SL_GUTTER);
    pic_spinner();
    char low[MAX_PATH], sz[24];
    if (pic.title) lv_label_set_text(pic.title, sl_zune() ? sl_lower(fm_base(full), low, sizeof low) : fm_base(full));
    if (pic.name) lv_label_set_text(pic.name, fm_base(full));
    fm_size(sz, sizeof sz, size);
    if (pic.info) lv_label_set_text(pic.info, sz);
    lv_timer_resume(pic.timer);
}
static bool fm_key(int b, bool rel, bool repeat)
{
    if (!npages) return false;
    lv_obj_t *top = pages[npages - 1];
    if (top == pic.page && pic.page && (b == BUTTON_UP || b == BUTTON_DOWN || b == BUTTON_RIGHT))
    {
        if (!rel && !repeat) pic_step(b == BUTTON_UP ? -1 : 1);
        return true;
    }
    if (top == fm_text_pg && fm_text_pg && (b == BUTTON_UP || b == BUTTON_DOWN))
    {
        if (!rel)
        {
            lv_obj_t *body = lv_obj_get_child(top, 1);
            int32_t step = lv_obj_get_height(body) - 2 * lv_font_get_line_height(&ss_regular_18);
            lv_obj_scroll_by_bounded(body, 0, b == BUTTON_UP ? step : -step, LV_ANIM_ON);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ cue sheets */
/* A cue sheet as its tracks (#163, the core reads cue sheets): each one
 * plays the album's file from its start. A sheet naming more than one
 * file plays them in turn. The file it names may have been converted
 * since (a .wav become a .flac): the same name with any audio ending in
 * the same folder is taken. */
#define CUE_TRACKS 99
#define CUE_FILES 8
static struct {
    char title[128], perf[96], dir[MAX_PATH], files[CUE_FILES][MAX_PATH];
    int nfiles, n;
    bool missing;
    struct { char title[96], perf[96]; uint32_t ms; uint8_t file; } t[CUE_TRACKS];
} cue;
static void cue_unquote(char *out, size_t n, const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '"')
    {
        s++;
        const char *e = strrchr(s, '"');
        size_t len = e && e >= s ? (size_t)(e - s) : strlen(s);
        if (len >= n) len = n - 1;
        memcpy(out, s, len);
        out[len] = '\0';
        return;
    }
    strlcpy(out, s, n);
    char *sp = strpbrk(out, " \t");      /* FILE name.wav WAVE: the name is the first word */
    if (sp) *sp = '\0';
}
/* the file a FILE line names, found in the sheet's folder */
static bool cue_find_file(char *out, size_t n, const char *dir, const char *name)
{
    if (fm_join(out, n, dir, fm_base(name)) && file_exists(out)) return true;
    char stem[200];
    strlcpy(stem, fm_base(name), sizeof stem);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    struct fm_view t = { 0 };
    bool found = false;
    if (fm_read(&t, dir, false))
        for (int k = t.folders; k < t.n && !found; k++)
        {
            const char *nm = fm_name(&t, k);
            size_t sl = strlen(stem);
            if (t.e[k].kind == FK_AUDIO && !strncasecmp(nm, stem, sl) && nm[sl] == '.') found = fm_join(out, n, dir, nm);
        }
    fm_view_free_lists(&t);
    return found;
}
static bool cue_parse(const char *path)
{
    memset(&cue, 0, sizeof cue);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char *raw = malloc(FM_CUE_MAX + 1), *txt = malloc(FM_CUE_MAX * 2 + 4);
    ssize_t got = raw && txt ? read(fd, raw, FM_CUE_MAX) : -1;
    close(fd);
    if (got < 0) { free(raw); free(txt); return false; }
    size_t skip = got >= 3 && (unsigned char)raw[0] == 0xef && (unsigned char)raw[1] == 0xbb && (unsigned char)raw[2] == 0xbf ? 3 : 0;
    fm_to_utf8(txt, FM_CUE_MAX * 2 + 4, raw + skip, (size_t)got - skip);
    free(raw);
    fm_parent(cue.dir, sizeof cue.dir, path);
    int file = -1;
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n"))
    {
        while (*line == ' ' || *line == '\t') line++;
        char word[16];
        int i = 0;
        while (line[i] && line[i] != ' ' && i < 15) { word[i] = (char)toupper((unsigned char)line[i]); i++; }
        word[i] = '\0';
        const char *rest = line + i;
        bool in_track = cue.n > 0;
        if (!strcmp(word, "FILE") && cue.nfiles < CUE_FILES)
        {
            char name[MAX_PATH];
            cue_unquote(name, sizeof name, rest);
            if (!cue_find_file(cue.files[cue.nfiles], MAX_PATH, cue.dir, name))
            {
                cue.missing = true;
                fm_join(cue.files[cue.nfiles], MAX_PATH, cue.dir, fm_base(name));
            }
            file = cue.nfiles++;
        }
        else if (!strcmp(word, "TRACK") && cue.n < CUE_TRACKS && file >= 0) { cue.t[cue.n].file = (uint8_t)file; cue.n++; }
        else if (!strcmp(word, "TITLE")) cue_unquote(in_track ? cue.t[cue.n - 1].title : cue.title, in_track ? sizeof cue.t[0].title : sizeof cue.title, rest);
        else if (!strcmp(word, "PERFORMER")) cue_unquote(in_track ? cue.t[cue.n - 1].perf : cue.perf, in_track ? sizeof cue.t[0].perf : sizeof cue.perf, rest);
        else if (!strcmp(word, "INDEX") && in_track)
        {
            int idx = 0, m = 0, s = 0, f = 0;
            if (sscanf(rest, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1)
                cue.t[cue.n - 1].ms = (uint32_t)(m * 60000 + s * 1000 + f * 1000 / 75);
        }
    }
    free(txt);
    return cue.n > 0;
}
static void cue_play(int k, bool shuffle)
{
    if (k < 0 || k >= cue.n) return;
    if (cue.missing) { toast_warn("The audio this cue sheet names isn't in its folder"); return; }
    if (sl_new_list(NULL) < 0) { toast_warn("Could not create playlist"); return; }
    for (int i = 0; i < cue.nfiles; i++)
        if (playlist_insert_track(NULL, cue.files[i], PLAYLIST_INSERT_LAST, false, false) < 0)
        { sl_abort_list(); toast_warn("Could not play this file"); return; }
    playlist_sync(NULL);
    if (shuffle && cue.nfiles > 1) { unsigned seed = sl_shuffle_seed(); srand(seed); playlist_shuffle(seed, 0); }
    sl_start_list_at(cue.t[k].file, shuffle ? 0 : cue.t[k].ms);
    sl_log("cue: %s track %d at %u ms", cue.files[cue.t[k].file], k + 1, (unsigned)cue.t[k].ms);
    np_show_auto();
}
static void cue_row_cb(lv_event_t *e) { cue_play((int)(intptr_t)lv_event_get_user_data(e), false); }
static void cue_pill_cb(lv_event_t *e) { cue_play(0, (intptr_t)lv_event_get_user_data(e) != 0); }
static void fm_cue_page(const char *path)
{
    if (!cue_parse(path)) { toast_warn("GabagoolOS can't read this cue sheet"); return; }
    char title[128];
    strlcpy(title, cue.title[0] ? cue.title : fm_base(path), sizeof title);
    lv_obj_t *p, *body = page_new(title, &p);
    lv_obj_t *ht = lv_obj_get_child(lv_obj_get_child(p, 0), -1);
    char low[160];
    if (ht && lv_obj_check_type(ht, &lv_label_class)) lv_label_set_text(ht, sl_zune() ? sl_lower(title, low, sizeof low) : title);
    sl_scroll_indicator(body);
    char b[200], n1[40];
    sl_count(n1, sizeof n1, cue.n, "%s TRACK", "%s TRACKS");
    if (cue.perf[0]) snprintf(b, sizeof b, "%s \xc2\xb7 %s", cue.perf, n1);
    else strlcpy(b, n1, sizeof b);
    lv_obj_t *sec = section(body, "");
    lv_label_set_text(sec, sl_zune() ? sl_upper(b, low, sizeof low) : b);
    if (cue.missing)
    {
        lv_obj_t *g = set_group(body, NULL);
        char m[300];
        snprintf(m, sizeof m, sl_tr("The audio this cue sheet names, %s, isn't in its folder."), fm_base(cue.files[0]));
        set_text(g, m, C_SUB);
    }
    else
    {
        lv_obj_t *row = box(body);
        lv_obj_set_size(row, lv_pct(100), 2 * SL_R_PILL + 8);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_style_pad_bottom(row, 8, 0);
        lv_obj_t *go = mn_pill(row, LV_SYMBOL_PLAY "  Play", 10, 2 * SL_R_PILL, true, cue_pill_cb, (void *)0);
        lv_obj_set_flex_grow(go, 1);
        lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
    }
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (FM_ROW_H - lh1 - lh2) / 2);
    for (int k = 0; k < cue.n; k++)
    {
        lv_obj_t *r = box(body);
        lv_obj_add_style(r, &st_row, 0);
        lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
        lv_obj_set_size(r, lv_pct(100), FM_ROW_H);
        char num[8];
        snprintf(num, sizeof num, "%d", k + 1);
        lv_obj_t *nl = text(r, num, &ss_regular_18, C_DIM);
        lv_obj_set_width(nl, 40);
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
        sl_align(nl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *t1 = text(r, "", &ss_semibold_23, 0xffffff);
        char nt[40];
        snprintf(nt, sizeof nt, "%s %d", sl_tr("Track"), k + 1);
        lv_label_set_text(t1, cue.t[k].title[0] ? cue.t[k].title : nt);
        lv_obj_set_size(t1, W - 2 * SL_GUTTER - 52 - SL_INSET, lh1);
        lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
        sl_place(t1, 52, ty);
        char tm[16], line[160];
        fmt_time(tm, sizeof tm, (long)cue.t[k].ms);
        const char *who = cue.t[k].perf[0] ? cue.t[k].perf : cue.perf;
        if (who[0]) snprintf(line, sizeof line, "%s \xc2\xb7 %s", who, tm);
        else strlcpy(line, tm, sizeof line);
        lv_obj_t *t2 = text(r, "", &ss_regular_18, C_SUB);
        lv_label_set_text(t2, line);
        lv_obj_set_size(t2, W - 2 * SL_GUTTER - 52 - SL_INSET, lh2);
        lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
        sl_place(t2, 52, ty + lh1);
        if (cue.missing) lv_obj_set_style_opa(r, LV_OPA_50, 0);
        else on_click(r, cue_row_cb, (void *)(intptr_t)k);
    }
    page_push(p);
}

static void fm_open_entry(struct fm_view *v, int k)
{
    char full[MAX_PATH];
    if (!fm_join(full, sizeof full, v->path, fm_name(v, k))) { toast_warn("That name is too long"); return; }
    switch (v->e[k].kind)
    {
        case FK_DIR: if (v->dest) fm_dest_page(full); else sl_files_page(full, 0); break;
        case FK_AUDIO: fm_play_from(v, k); break;
        case FK_CUE: fm_cue_page(full); break;
        case FK_LIST: pls_page(full); break;
        case FK_PIC: fm_pic_page(full); if (pic.page && !pic.view) { pic.view = v; pic.k = k; } break;
        case FK_TEXT: fm_text_page(full); break;
        case FK_BOOK: rd_open(full); break;
        default: fm_acts(v, k); break;   /* nothing opens it here: what can be done with it */
    }
}
static void fm_row_cb(lv_event_t *e)
{
    struct fm_view *v = lv_event_get_user_data(e);
    lv_obj_t *r = lv_event_get_current_target(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(r);
    if (k < 0 || k >= v->n) return;
    if (sel_kind_on(SEL_FILES) && sel_view == v)
    {
        if (sel_toggle(k)) sel_paint_row(r, sel_has(k));
        return;
    }
    fm_open_entry(v, k);
}

/* the picking bar's hooks (sloppy_playback.inc) */
static bool fm_rebind_view(void *p)
{
    for (int i = 0; p && i < MAX_PAGES; i++)
        if (fm_views[i] == p) { fm_rebind_async(p); return true; }
    return false;
}
static int fm_sel_total(void) { struct fm_view *v = sel_view; return fm_live(v) ? v->n : 0; }
static void fm_sel_act(int which)
{
    static const int acts_of[] = { FA_PLAY, FA_QUEUE, FA_PLAYLIST };
    if (which == 3) fm_sel_more();
    else fm_sel_do(acts_of[which]);
}
