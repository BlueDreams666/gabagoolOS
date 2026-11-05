/* ================================================================
 *  Milestone 2: song actions, queue, Up Next, crossfade, EQ,
 *  quick panel.
 * ================================================================ */

static void anim_prop(lv_obj_t *o, lv_anim_exec_xcb_t cb, int from, int to, int ms,
                      lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    a.parameter.bezier3 = (lv_anim_bezier3_para_t){ LV_BEZIER_VAL_FLOAT(0.32), LV_BEZIER_VAL_FLOAT(0.72),
                                                    LV_BEZIER_VAL_FLOAT(0.0), LV_BEZIER_VAL_FLOAT(1.0) };
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
/* Restyling redraws the object even when the value is the same: an
 * animation's start value and its first tick often are. */
static void set_ty(void *o, int32_t v) { if (lv_obj_get_style_translate_y(o, 0) != v) lv_obj_set_style_translate_y(o, v, 0); }
static void set_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void hide_cb(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    press_motion(b);
    lv_obj_set_style_transform_scale(b, 224, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, size / 2, 0);
    if (sym) lv_obj_center(text(b, sym, &sl_sym_24, 0xffffff));
    else
        for (int i = -1; i <= 1; i++)   /* no glyph: three dots, "more" */
        {
            lv_obj_t *d = box(b);
            lv_obj_set_size(d, 5, 5);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_align(d, LV_ALIGN_CENTER, 0, i * 9);
        }
    on_click(b, cb, ud);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(b, 8);     /* small targets: a near miss still lands */
    return b;
}

/* ------------------------------------------------------ queue actions */
static bool something_playing(void) { return (audio_status() & AUDIO_STATUS_PLAY) != 0; }
static bool uq_open;
static void uq_rebuild_soon(void);

static void q_play_next(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    if (!something_playing()) { if (play_list(&t, 1, 0, false)) np_show_auto(); return; }
    char path[MAX_PATH];
    sl_log("play next %s", sl_track_path(t, path, sizeof path));
    int rc = q_insert(path, true, true);
    rc < 0 ? toast_warn("Could not queue this song") : toast("Plays next");
    if (uq_open) uq_rebuild_soon();
}
static void q_add(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    if (!something_playing()) { if (play_list(&t, 1, 0, false)) np_show_auto(); return; }
    char path[MAX_PATH];
    sl_log("add to queue %s", sl_track_path(t, path, sizeof path));
    int rc = q_insert(path, false, true);
    rc < 0 ? toast_warn("Could not queue this song") : toast("Added to queue");
    if (uq_open) uq_rebuild_soon();
}

/* --------------------------------------------------------- song sheet */
static lv_obj_t *acts_scrim, *acts;
static int acts_track = -1;
static bool acts_open_now;

static void acts_close_ms(int ms)
{
    if (!acts_open_now) return;
    acts_open_now = false;
    anim_prop(acts, set_ty, lv_obj_get_style_translate_y(acts, 0), 520, ms, hide_cb);
    anim_prop(acts_scrim, set_opa, lv_obj_get_style_bg_opa(acts_scrim, 0), 0, ms, hide_cb);
}
static void acts_close(void) { acts_close_ms(SL_T_PAGE); }
static void acts_scrim_cb(lv_event_t *e) { (void)e; acts_close(); }
static void artist_page(int k0);
static void pl_open(int track);
static void pl_open_many(const int *tracks, int n);
static void sr_start(int seed);
static void sr_stop(void);
static bool sr_on(void);
static const char *sr_seed_title(void);
static bool sr_artist_usable(const char *a);
/* Song info: what the library knows of a song, and its file: each a row
 * of one card (set_group), the name small over its value */
static void song_info_line(lv_obj_t *g, const char *what, const char *value)
{
    if (!value || !value[0]) return;
    lv_obj_t *r = box(g), *in = box(r);   /* the words padded inside, the hairline at the row's own edge */
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_size(in, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(in, SL_GUTTER, 0);
    lv_obj_set_style_pad_hor(in, SL_INSET, 0);
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_COLUMN);
    text(in, what, &ss_regular_16, C_SUB);
    lv_obj_t *v = text(in, value, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(v, lv_pct(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    set_sep(g, r, SL_INSET);
}
static void song_info_page(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    const struct sl_track *tr = &sl_lib.tracks[t];
    const struct sl_album *al = &sl_lib.albums[tr->album];
    lv_obj_t *p, *body = page_new("Song info", &p);
    lv_obj_t *hd = box(body);
    lv_obj_set_size(hd, lv_pct(100), 96);
    lv_obj_t *c = cover(hd, tr->album, SL_ART_ROW, false);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *t1 = text(hd, tr->title, &ss_semibold_23, C_TEXT);
    lv_obj_set_pos(t1, 80, 16);
    lv_obj_set_width(t1, W - 28 - 90);
    label_lines(t1, 1);
    lv_obj_t *t2 = text(hd, sl_track_artist(t), &ss_regular_18, C_SUB);
    lv_obj_set_pos(t2, 80, 52);
    lv_obj_set_width(t2, W - 28 - 90);
    label_lines(t2, 1);
    char b[96];
    lv_obj_t *g = set_group(body, NULL);
    song_info_line(g, sl_tr("Album"), al->title);
    if (strcmp(sl_track_artist(t), al->artist)) song_info_line(g, sl_tr("Album artist"), al->artist);
    if (tr->num > 0)
    {
        if (tr->disc > 0) snprintf(b, sizeof b, "%d \xc2\xb7 %s %d", tr->num, sl_tr("disc"), tr->disc);
        else snprintf(b, sizeof b, "%d", tr->num);
        song_info_line(g, sl_tr("Track"), b);
    }
    if (al->year) { snprintf(b, sizeof b, "%d", al->year); song_info_line(g, sl_tr("Year"), b); }
    song_info_line(g, sl_tr("Genre"), al->genre);
    fmt_time(b, sizeof b, tr->len);
    song_info_line(g, sl_tr("Length"), b);
    char path[MAX_PATH];
    sl_track_path(t, path, sizeof path);
    const char *ext = tr->file ? strrchr(tr->file, '.') : NULL;
    if (ext && ext[1])
    {
        int n = 0;
        for (const char *q = ext + 1; *q && n < 8; q++) b[n++] = *q >= 'a' && *q <= 'z' ? *q - 32 : *q;
        b[n] = '\0';
        int bits = tr->lossless ? np_bits(path) : 0;
        if (bits) snprintf(b + n, sizeof b - n, " \xc2\xb7 %d-bit", bits);
        if (tr->lossless) { strlcat(b, " \xc2\xb7 ", sizeof b); strlcat(b, sl_tr("lossless"), sizeof b); }
        song_info_line(g, sl_tr("Format"), b);
    }
    int fd = path[0] ? open(path, O_RDONLY) : -1;
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0) close(fd);
    if (size > 0)
    {
        snprintf(b, sizeof b, "%.1f MB", (double)size / (1024.0 * 1024.0));
        song_info_line(g, sl_tr("Size"), b);
    }
    if (tr->added)
    {
        /* the file's time as the database keeps it, seconds since 1970 */
        static const char *const mon[] = { "January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December" };
        time_t at = (time_t)tr->added;
        struct tm tm;
        if (gmtime_r(&at, &tm) && tm.tm_year > 90 && tm.tm_mon >= 0 && tm.tm_mon < 12)
        {
            snprintf(b, sizeof b, "%d %s %d", tm.tm_mday, sl_tr(mon[tm.tm_mon]), tm.tm_year + 1900);
            song_info_line(g, sl_tr("Added"), b);
        }
    }
    song_info_line(g, sl_tr("File"), path);
    page_push(p);
}
static void acts_btn_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int t = acts_track;
    acts_close();
    if (t < 0 || t >= sl_lib.n_tracks) return;
    int a = sl_lib.tracks[t].album;
    switch (k)
    {
        case 0: q_play_next(t); break;
        case 1: q_add(t); break;
        case 4: pl_open(t); break;
        case 5: sr_start(t); break;
        case 6:
        {
            char b[96];
            ts_hide_artist(sl_lib.albums[a].artist);
            snprintf(b, sizeof b, "%s won't play in Radio, Shuffle or For you", sl_lib.albums[a].artist);
            toast(b);
            break;
        }
        case 2: if (np_open) np_hide(0); if (!page_top_is(PG_ALBUM | a)) album_page(a); break;
        case 7: if (np_open) np_hide(0); song_info_page(t); break;
        case 8: if (np_open) np_hide(0); play_settings_page(); break;
        case 3:
            if (np_open) np_hide(0);
            /* the song's own artist (the first it names), as Artists lists them */
            {
                int id = tg_first_artist_of(t);
                if (id >= 0) { if (!page_top_is(PG_TARTIST | id)) tartist_page(id); break; }
            }
            for (int j = 0; j < sl_lib.n_albums; j++)
                if (!strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[j]].artist, sl_lib.albums[a].artist))
                { if (!page_top_is(PG_ARTIST | j)) artist_page(j); break; }
            break;
    }
}
static void acts_row_cb(const char *sym, const char *label, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *r = box(acts);
    lv_obj_set_size(r, lv_pct(100), 64);
    lv_obj_set_style_radius(r, SL_R_XL - SL_FLOAT, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_TEXT), LV_STATE_PRESSED);   /* seen on a white sheet too */
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_t *i = text(r, sym, &sl_sym_24, 0xffffff);
    sl_align(i, LV_ALIGN_LEFT_MID, 16, 0);   /* from the leading edge: the right in Hebrew and Arabic */
    char low[64];
    lv_obj_t *l = text(r, sl_zune() ? sl_lower(sl_tr(label), low, sizeof low) : label, &ss_semibold_23, 0xffffff);
    /* up to the sheet's edge: a long one ends in an ellipsis (Largest, Spanish) */
    lv_obj_set_size(l, W - 20 - 2 * 10 - 62 - 16, lv_font_get_line_height(&ss_semibold_23));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);   /* beside its icon, in any script */
    sl_align(l, LV_ALIGN_LEFT_MID, 62, 0);
    on_click(r, cb, (void *)ud);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
}
static void acts_row(const char *sym, const char *label, int k) { acts_row_cb(sym, label, acts_btn_cb, k); }
/* the sheet, made once and emptied for each use */
static void acts_begin(void)
{
    if (!acts)
    {
        acts_scrim = box(lv_layer_top());
        lv_obj_set_size(acts_scrim, W, H);
        lv_obj_set_style_bg_color(acts_scrim, lv_color_black(), 0);
        on_click(acts_scrim, acts_scrim_cb, NULL);
        acts = box(lv_layer_top());
        lv_obj_set_width(acts, W - 20);
        lv_obj_set_height(acts, LV_SIZE_CONTENT);
        lv_obj_align(acts, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_radius(acts, SL_R_XL, 0);
        lv_obj_set_style_bg_opa(acts, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(acts, 10, 0);
        lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(acts, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
        lv_obj_remove_flag(acts, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_clean(acts);
    /* Zune: a black sheet, square, its actions in lowercase */
    lv_obj_set_style_bg_color(acts, lv_color_hex(C_SHEET), 0);   /* the theme as it is now */
    lv_obj_set_style_radius(acts, sl_zune() ? 0 : SL_R_XL, 0);
}
static void acts_show(void)
{
    lv_obj_move_foreground(acts_scrim);
    lv_obj_move_foreground(acts);
    lv_obj_remove_flag(acts_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(acts, LV_OBJ_FLAG_HIDDEN);
    /* a sheet's small head (a count, a name, Sort by) at the reading edge */
    for (uint32_t i = 0; sl_ui_rtl() && i < lv_obj_get_child_count(acts); i++)
    {
        lv_obj_t *c = lv_obj_get_child(acts, (int32_t)i);
        if (!lv_obj_check_type(c, &lv_label_class)) continue;
        lv_obj_set_width(c, lv_pct(100));
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_RIGHT, 0);
    }
    acts_open_now = true;
    sl_doing_is("a sheet", NULL);
    anim_prop(acts_scrim, set_opa, 0, 110, SL_T_PAGE, NULL);
    anim_prop(acts, set_ty, 520, 0, SL_T_PAGE, NULL);
}
/* Pulled down, a sheet follows the finger (touch_read's G_SHEET), its
 * shade with it, and closes on a long enough pull or a flick, as Up Next
 * does; a slider, a drag handle or a list scrolled down keeps its own. */
static bool acts_sheet_grab(int x, int y)
{
    if (!acts_open_now || lv_anim_get(acts, set_ty)) return false;
    lv_obj_t *hit = lv_indev_search_obj(lv_layer_top(), &(lv_point_t){ x, y });   /* the sheet is over the screen */
    for (lv_obj_t *o = hit; o; o = lv_obj_get_parent(o))
    {
        if (lv_obj_check_type(o, &lv_slider_class) ||
            (lv_obj_has_flag(o, LV_OBJ_FLAG_PRESS_LOCK) && lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) ||
            lv_obj_get_scroll_y(o) > 0)
            return false;
        if (o == acts) return true;
    }
    return false;
}
static void acts_sheet_drag(int dy)
{
    lv_anim_delete(acts, set_ty);
    lv_anim_delete(acts_scrim, set_opa);
    if (dy < 0) dy = -(int)(sqrtf((float)-dy) * 3);   /* up, it gives a little, like a rubber band */
    lv_obj_set_style_translate_y(acts, dy, 0);
    int h = lv_obj_get_height(acts) + 10;
    lv_obj_set_style_bg_opa(acts_scrim, (lv_opa_t)(110 * LV_CLAMP(0, h - dy, h) / h), 0);
}
static void acts_sheet_release(int dy, int vy)
{
    int from = lv_obj_get_style_translate_y(acts, 0), h = lv_obj_get_height(acts) + 10;
    if (dy > h / 4 || (vy > 700 && dy > 40))
    {
        /* the faster the flick, the shorter the rest of the trip, 120 to 300 ms */
        int speed = vy > 1400 ? vy : 1400, ms = (520 - from) * 1000 / speed;
        acts_close_ms(ms < 120 ? 120 : ms > 300 ? 300 : ms);
    }
    else
    {
        anim_prop(acts, set_ty, from, 0, SL_T_PAGE, NULL);
        anim_prop(acts_scrim, set_opa, lv_obj_get_style_bg_opa(acts_scrim, 0), 110, SL_T_PAGE, NULL);
    }
}
/* a sheet's head: a cover, a title and a line under it, then a hairline */
static void acts_head(int album, const char *title, const char *line)
{
    lv_obj_t *hd = box(acts);
    lv_obj_set_size(hd, lv_pct(100), 84);
    lv_obj_t *c = cover(hd, album, SL_ART_ROW, false);
    sl_align(c, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *t1 = text(hd, title, &ss_semibold_23, 0xffffff);
    lv_obj_set_width(t1, W - 130);
    label_lines(t1, 1);
    sl_place(t1, 80, 12);
    lv_obj_t *t2 = text(hd, line, &ss_regular_16, C_SUB);
    lv_obj_set_width(t2, W - 130);
    label_lines(t2, 1);
    sl_place(t2, 80, 12 + lv_font_get_line_height(&ss_semibold_23) + 4);   /* 44 at Standard; under the title at any size */
    lv_obj_set_height(hd, LV_MAX(84, 12 + lv_font_get_line_height(&ss_semibold_23) + 4 + lv_font_get_line_height(&ss_regular_16) + 12));
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);   /* the hairline of every panel */
    lv_obj_set_style_bg_opa(sep, sl_zune() ? LV_OPA_COVER : 36, 0);
}
static bool acts_np;                     /* opened by Now Playing's dots: Playback's settings at the end */
static void acts_open(int t)
{
    bool np_more = acts_np;
    acts_np = false;
    if (t < 0 || t >= sl_lib.n_tracks) return;
    acts_track = t;
    acts_begin();
    const struct sl_track *tr = &sl_lib.tracks[t];
    char b[160];
    snprintf(b, sizeof b, "%s · %s", sl_track_artist(t), sl_lib.albums[tr->album].title);
    acts_head(tr->album, tr->title, b);
    acts_row(LV_SYMBOL_NEXT, "Play next", 0);
    acts_row(LV_SYMBOL_LIST, "Add to queue", 1);
    acts_row(LV_SYMBOL_PLUS, "Add to playlist", 4);
    acts_row(LV_SYMBOL_SHUFFLE, "Radio from this song", 5);
    acts_row(LV_SYMBOL_IMAGE, "Go to album", 2);
    acts_row(LV_SYMBOL_AUDIO, "Go to artist", 3);
    acts_row(LV_SYMBOL_FILE, "Song info", 7);
    if (sr_artist_usable(sl_lib.albums[tr->album].artist)) acts_row(LV_SYMBOL_EYE_CLOSE, "Don't play this artist", 6);
    if (np_more) acts_row(LV_SYMBOL_SETTINGS, "Playback", 8);
    acts_show();
}
static void acts_open_np(int t) { acts_np = true; acts_open(t); }

/* ------------------------------------------- album and artist sheets */
/* Hold an album's cover (#137) or an artist's row: play, shuffle, queue or
 * keep it in a playlist without opening it (#114); Select starts picking
 * there, with this one picked. */
static void q_add_many(const int *l, int n);
static void sel_start(int kind, void *view, lv_obj_t *page, int first);
static void sel_paint_artist(lv_obj_t *r, bool on);
static void sel_refresh_async(void *p);
static void cover_edge(lv_obj_t *art, int w);
static lv_obj_t *top_body(void) { return lv_obj_get_child(pages[npages - 1], 1); }
static void album_act_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int a = (int)(v >> 3), k = (int)(v & 7);
    acts_close();
    if (a < 0 || a >= sl_lib.n_albums) return;
    const struct sl_album *al = &sl_lib.albums[a];
    switch (k)
    {
        case 0: case 1: if (play_album(a, 0, k == 1)) np_show_auto(); break;
        case 2: q_add_many(al->tracks, al->n_tracks); break;
        case 3: pl_open_many(al->tracks, al->n_tracks); break;
        case 4:
            for (int j = 0; j < sl_lib.n_albums; j++)
                if (!strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[j]].artist, al->artist))
                {
                    /* held on the artist's own page (theirs from Artists, too): already there */
                    intptr_t top = npages ? (intptr_t)lv_obj_get_user_data(pages[npages - 1]) : 0;
                    bool there = top == (PG_ARTIST | j) || ((top & ~0xffffff) == PG_TARTIST && tg_ready() &&
                                 (top & 0xffffff) < tgx.n && !strcasecmp(TG_NAME(top & 0xffffff), al->artist));
                    if (!there) artist_page(j);
                    break;
                }
            break;
        case 5:
            if (album_view && lv_obj_get_parent(album_view->body) == pages[npages - 1])
                sel_start(SEL_ALBUMS, album_view, pages[npages - 1], a);
            break;
        case 6:
        {
            bool on = !fav_has(a);
            fav_set(a, on);
            toast(on ? "Added to Favourite albums" : "Removed from Favourite albums");
            break;
        }
    }
}
static void album_acts_open(int a, bool pick)
{
    if (a < 0 || a >= sl_lib.n_albums) return;
    const struct sl_album *al = &sl_lib.albums[a];
    acts_begin();
    char b[160], sg[40];
    sl_count(sg, sizeof sg, al->n_tracks, "%s song", "%s songs");
    if (al->year) snprintf(b, sizeof b, "%s · %d · %s", al->artist, al->year, sg);
    else snprintf(b, sizeof b, "%s · %s", al->artist, sg);
    acts_head(a, al->title, b);
    intptr_t v = (intptr_t)a << 3;
    acts_row_cb(LV_SYMBOL_PLAY, "Play", album_act_cb, v | 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", album_act_cb, v | 1);
    acts_row_cb(LV_SYMBOL_LIST, "Add to queue", album_act_cb, v | 2);
    acts_row_cb(LV_SYMBOL_PLUS, "Add to playlist", album_act_cb, v | 3);
    acts_row_cb(LV_SYMBOL_AUDIO, "Go to artist", album_act_cb, v | 4);
    acts_row_cb("", fav_has(a) ? "Remove from favourites" : "Add to favourites", album_act_cb, v | 6);
    lv_obj_t *heart = sl_icon(lv_obj_get_child(acts, -1), IC_HEART, 24, C_TEXT, C_SHEET);   /* a heart, as the album page's */
    sl_align(heart, LV_ALIGN_LEFT_MID, 16, 0);
    if (pick) acts_row_cb(LV_SYMBOL_OK, "Select", album_act_cb, v | 5);
    acts_show();
}
static void album_long_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_active());
    if (sel_on()) return;                /* picking: a hold is a tap */
    album_acts_open((int)(v >> 1), v & 1);
}
static void artist_act_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int k0 = (int)(v >> 3), k = (int)(v & 7), *l, n;
    acts_close();
    if (k0 < 0 || k0 >= sl_lib.n_albums) return;
    if (k == 5) { sel_start(SEL_ARTISTS, top_body(), pages[npages - 1], k0); return; }
    n = albums_tracks(sl_lib.albums_by_artist + k0, artist_span(k0), &l);
    if (k <= 1) { if (n && play_list(l, n, 0, k == 1)) np_show_auto(); }
    else if (k == 2) q_add_many(l, n);
    else if (n) pl_open_many(l, n);
    free(l);
}
static void artist_acts_open(int k0)
{
    int n = artist_span(k0), songs = 0;
    if (!n) return;
    for (int i = 0; i < n; i++) songs += sl_lib.albums[sl_lib.albums_by_artist[k0 + i]].n_tracks;
    acts_begin();
    char b[96], a1[40], a2[40];
    snprintf(b, sizeof b, "%s · %s", sl_count(a1, sizeof a1, n, "%s album", "%s albums"), sl_count(a2, sizeof a2, songs, "%s song", "%s songs"));
    int first = sl_lib.albums_by_artist[k0];
    acts_head(first, sl_lib.albums[first].artist, b);
    intptr_t v = (intptr_t)k0 << 3;
    acts_row_cb(LV_SYMBOL_PLAY, "Play", artist_act_cb, v | 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", artist_act_cb, v | 1);
    acts_row_cb(LV_SYMBOL_LIST, "Add to queue", artist_act_cb, v | 2);
    acts_row_cb(LV_SYMBOL_PLUS, "Add to playlist", artist_act_cb, v | 3);
    acts_row_cb(LV_SYMBOL_OK, "Select", artist_act_cb, v | 5);
    acts_show();
}
static void artist_long_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    lv_indev_wait_release(lv_indev_active());
    if (sel_on()) return;
    artist_acts_open((int)(intptr_t)lv_obj_get_user_data(r) - 1);
}

/* ------------------------------------------------------- the play menu */
/* Songs, Albums and Artists: Play and Shuffle in the header, and beside
 * them "more": play in order, shuffle, add one at random, go to the song
 * playing, and Select. */
static int pm_kind;
static lv_obj_t *pm_page;
/* a quick wash of the accent where the eye should land, fading out */
static void flash_bg(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void flash_edge(void *o, int32_t v)
{
    lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0);
    if (v) return;
    cover_edge(o, 0);                             /* a cover: no edge of its own */
    if (sel_kind_on(SEL_ALBUMS) && album_view) lv_async_call(sel_refresh_async, album_view);   /* unless picked */
}
static void flash_card(void *o, int32_t v)
{
    lv_obj_set_style_outline_opa(o, (lv_opa_t)v, 0);
    if (!v) sel_paint_artist(o, sel_has((int)(intptr_t)lv_obj_get_user_data(o) - 1));   /* back to picked or not */
}
/* where the eye should land: a row washed in the accent, or a cover's or
 * card's edge lit (inside it, so nothing clips it), fading out */
enum { FLASH_ROW, FLASH_COVER, FLASH_CARD };
static void sl_flash(lv_obj_t *o, int how)
{
    if (!o) return;
    lv_anim_exec_xcb_t cb = how == FLASH_ROW ? flash_bg : how == FLASH_COVER ? flash_edge : flash_card;
    int from = how == FLASH_ROW ? LV_OPA_40 : LV_OPA_COVER;
    if (how == FLASH_ROW) lv_obj_set_style_bg_color(o, lv_color_hex(C_ACCENT), 0);
    else if (how == FLASH_COVER) cover_edge(o, 3);
    else
    {
        lv_obj_set_style_outline_color(o, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_outline_width(o, 3, 0);
        lv_obj_set_style_outline_pad(o, -3, 0);
    }
    cb(o, from);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, SL_T_PAGE);
    lv_anim_set_duration(&a, 4 * SL_T_SLOW);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}
static struct song_list_view *pm_songs(lv_obj_t *page)
{
    lv_obj_t *body = lv_obj_get_child(page, 1);
    for (int i = 0; i < 8; i++) if (song_views[i] && song_views[i]->body == body) return song_views[i];
    return NULL;
}
/* scroll so y (in the body's content) sits mid-screen */
static void pm_scroll_mid(lv_obj_t *body, int y, int h)
{
    lv_obj_update_layout(body);
    int to = y - (lv_obj_get_height(body) - h) / 2;
    lv_obj_scroll_to_y(body, LV_MAX(0, to), LV_ANIM_OFF);
    lv_obj_update_layout(body);
}
static int pm_now(void) { return something_playing() && np_love_track >= 0 && np_love_track < sl_lib.n_tracks ? np_love_track : -1; }
static void pm_go_now(void)
{
    int t = pm_now();
    if (t < 0 || !pm_page) return;
    int a = sl_lib.tracks[t].album;
    if (pm_kind == SEL_SONGS)
    {
        struct song_list_view *v = pm_songs(pm_page);
        int k = -1;
        for (int i = 0; v && i < v->count && k < 0; i++) if (v->tracks[i] == t) k = i;
        if (k < 0) return;
        pm_scroll_mid(v->body, lv_obj_get_y(v->host) + song_row_y(v, k), 82);
        if (v->indexes[k % 14] == k) sl_flash(v->rows[k % 14], FLASH_ROW);
    }
    else if (pm_kind == SEL_ALBUMS)
    {
        struct album_grid_view *g = album_view;
        const int *order = albums_order();
        int k = -1;
        for (int i = 0; g && i < sl_lib.n_albums && k < 0; i++) if (order[i] == a) k = i;
        if (k < 0) return;
        pm_scroll_mid(g->body, lv_obj_get_y(g->host) + (k / av_cols()) * av_row(), av_row());
        int slot = k % av_slots();
        if (g->indexes[slot] == k && g->cells[slot]) sl_flash(lv_obj_get_child(g->cells[slot], 0), FLASH_COVER);
    }
    else
    {
        int k0 = -1;
        for (int j = 0; j < sl_lib.n_albums && k0 < 0; j++) if (sl_lib.albums_by_artist[j] == a) k0 = j;
        while (k0 > 0 && !strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k0 - 1]].artist, sl_lib.albums[a].artist)) k0--;
        if (k0 < 0 || !artists_view) return;
        int i = -1;                      /* its row in the list */
        for (int j = 0; j < artists_view->count && i < 0; j++) if (artists_view->list[j] == k0) i = j;
        if (i < 0) return;
        pm_scroll_mid(artists_view->body, lv_obj_get_y(artists_view->host) + i * artists_view->pitch, artists_view->pitch - 8);
        row_view_bind(artists_view);
        int slot = i % RV_SLOTS;
        if (artists_view->indexes[slot] == i && artists_view->rows[slot]) sl_flash(artists_view->rows[slot], FLASH_CARD);
    }
}
/* every song of the page, in its order: the list as shown, the albums as
 * the grid has them, or the artists A to Z; the caller frees it */
static int pm_tracks(int kind, lv_obj_t *page, int **out)
{
    if (kind == SEL_SONGS)
    {
        struct song_list_view *v = pm_songs(page);
        *out = v && v->count ? malloc((size_t)v->count * sizeof **out) : NULL;
        if (!*out) return 0;
        memcpy(*out, v->tracks, (size_t)v->count * sizeof **out);
        return v->count;
    }
    return albums_tracks(kind == SEL_ALBUMS ? albums_order() : sl_lib.albums_by_artist, sl_lib.n_albums, out);
}
static void pm_play_cb(lv_event_t *e)
{
    lv_obj_t *page = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)));
    int *l, n = pm_tracks((int)(intptr_t)lv_event_get_user_data(e), page, &l);
    if (n && play_list(l, n, 0, false)) np_show_auto();
    free(l);
}
static void pm_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    if (!pm_page || pm_page != pages[npages - 1]) return;
    int *l, n;
    switch (k)
    {
        case 0:
            n = pm_tracks(pm_kind, pm_page, &l);
            if (n && play_list(l, n, 0, false)) np_show_auto();
            free(l);
            break;
        case 1: action(ACT_SHUFFLE); break;
        case 2:
            if (pm_kind == SEL_ALBUMS && sl_lib.n_albums)
            {
                const struct sl_album *al = &sl_lib.albums[rand() % sl_lib.n_albums];
                q_add_many(al->tracks, al->n_tracks);
            }
            else if (sl_lib.n_tracks) q_add(rand() % sl_lib.n_tracks);
            break;
        case 3: pm_go_now(); break;
        case 4:
            if (pm_kind == SEL_SONGS) sel_start(SEL_SONGS, pm_songs(pm_page), pm_page, -1);
            else if (pm_kind == SEL_ALBUMS) { if (album_view) sel_start(SEL_ALBUMS, album_view, pm_page, -1); }
            else sel_start(SEL_ARTISTS, lv_obj_get_child(pm_page, 1), pm_page, -1);
            break;
    }
}
static void play_menu_cb(lv_event_t *e)
{
    pm_kind = (int)(intptr_t)lv_event_get_user_data(e);
    pm_page = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_current_target(e)));
    acts_begin();
    char b[64];
    if (pm_kind == SEL_SONGS) sl_count(b, sizeof b, sl_lib.n_tracks, "%s song", "%s songs");
    else if (pm_kind == SEL_ALBUMS) sl_count(b, sizeof b, sl_lib.n_albums, "%s album", "%s albums");
    else
    {
        int n = 0;
        for (int k = 0; k < sl_lib.n_albums; k += artist_span(k)) n++;
        sl_count(b, sizeof b, n, "%s artist", "%s artists");
    }
    lv_obj_t *hd = text(acts, b, &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_all(hd, 12, 0);
    acts_row_cb(LV_SYMBOL_PLAY, "Play in order", pm_row_cb, 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", pm_row_cb, 1);
    acts_row_cb(LV_SYMBOL_PLUS, pm_kind == SEL_ALBUMS ? "Add a random album" : "Add a random song", pm_row_cb, 2);
    if (pm_now() >= 0) acts_row_cb(LV_SYMBOL_GPS, "Go to current song", pm_row_cb, 3);
    acts_row_cb(LV_SYMBOL_OK, "Select", pm_row_cb, 4);
    acts_show();
}
/* a header's "more": three dots, as on a song's row turned flat */
static lv_obj_t *more_button(lv_obj_t *page, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *more = header_button(page, "", cb, ud);
    for (int i = -1; i <= 1; i++)
    {
        lv_obj_t *d = box(more);
        lv_obj_set_size(d, 5, 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_align(d, LV_ALIGN_CENTER, i * 9, 0);
    }
    return more;
}
static void play_menu_button(lv_obj_t *page, int kind)
{
    more_button(page, play_menu_cb, (void *)(intptr_t)kind);
    if (sl_zune()) return;               /* Zule: the big title keeps the header; the menu has both */
    header_button(page, LV_SYMBOL_SHUFFLE, tile_cb, (void *)(intptr_t)ACT_SHUFFLE);
    header_button(page, LV_SYMBOL_PLAY, pm_play_cb, (void *)(intptr_t)kind);
}

/* "Sort by": the orders a list has, a tick by the one it is in */
static void (*sort_pick)(int);
static void sort_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    void (*pick)(int) = sort_pick;
    acts_close();
    if (pick) pick(k);
}
/* the same sheet for any short choice: a title, and a tick by cur (-1 none) */
static void pick_sheet(const char *title, const char *const *names, int n, int cur, void (*pick)(int))
{
    sort_pick = pick;
    acts_begin();
    lv_obj_t *hd = text(acts, title, &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_all(hd, 12, 0);
    for (int k = 0; k < n; k++)
    {
        lv_obj_t *r = box(acts);
        lv_obj_set_size(r, lv_pct(100), 64);
        lv_obj_set_style_radius(r, SL_R_XL - SL_FLOAT, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
        char low[64];
        lv_obj_t *l = text(r, sl_zune() ? sl_lower(sl_tr(names[k]), low, sizeof low) : names[k], &ss_semibold_23,
                           k == cur ? sl_accent_on(C_SHEET, 45) : 0xffffff);   /* the accent, readable on the sheet */
        sl_align(l, LV_ALIGN_LEFT_MID, 16, 0);
        if (k == cur) sl_align(text(r, LV_SYMBOL_OK, &sl_sym_24, sl_accent_on(C_SHEET, 45)), LV_ALIGN_RIGHT_MID, -16, 0);
        on_click(r, sort_row_cb, (void *)(intptr_t)k);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    acts_show();
}
static void sort_sheet(const char *const *names, int n, int cur, void (*pick)(int))
{
    pick_sheet("Sort by", names, n, cur, pick);
}
static void kebab_cb(lv_event_t *e)
{
    acts_open((int)(intptr_t)lv_event_get_user_data(e));
}
static void sel_begin(lv_obj_t *row);
static void row_long_cb(lv_event_t *e)
{
    if (sw_busy()) return;              /* a held swipe is not a long press */
    lv_obj_t *r = lv_event_get_current_target(e);
    /* a song in a list: hold starts picking songs (the dots keep the sheet) */
    if (lv_obj_get_user_data(lv_obj_get_parent(r))) { if (!sel_on()) sel_begin(r); }
    else acts_open((int)(intptr_t)lv_obj_get_user_data(r));
    lv_indev_wait_release(lv_indev_active());
}

/* ------------------------------------------------------------ Up Next */
static lv_obj_t *uq, *uq_list;
static bool uq_dragging;
static int uq_snapshot_amount, uq_snapshot_current;
static char uq_snapshot_path[MAX_PATH];
#define UQ_QMAX 1000                    /* queued songs shown; more are still queued and play */
static int uq_queue_indexes[UQ_QMAX], uq_queue_count;
static lv_obj_t *uq_queue_rows[UQ_QMAX], *uq_drag_row;
static int uq_queue_k, uq_queue_run;    /* the next queued row to make, and how many are queued */
static int uq_next_indexes[60], uq_next_count;
static lv_obj_t *uq_next_rows[60];
/* Each row reads its file name and cover from the card: 60 at once held
 * the open 605 ms on the R1 with a 4248-song list. The sheet opens with
 * the first few next-up rows; the rest follow while it sits still, or as
 * the list nears its end. */
#define UQ_FIRST 10
static int uq_next_k, uq_next_ahead;     /* the next row's place in play order, and the last */
static uint32_t uq_scrolled_at;
static int uq_drag_group;
static lv_timer_t *uq_rebuild_timer;
static void uq_build(void);

static int cur_index(void) { return q_cur(); }

static void uq_close_to(int from, int ms);
static void uq_close(void)
{
    uq_close_to(lv_obj_get_style_translate_y(uq, 0), 240);
}
static void uq_done_cb(lv_event_t *e) { (void)e; uq_close(); }
static void uq_radio_stop_cb(lv_event_t *e)
{
    (void)e;
    sr_stop();
    toast("Radio stopped");
    uq_rebuild_soon();
}
static void uq_rebuild_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (sw_busy()) { lv_timer_resume(t); return; }   /* let a swiped row settle first */
    if (uq_open && !uq_dragging) uq_build();
}
static void uq_rebuild_soon(void)
{
    if (!uq_rebuild_timer) uq_rebuild_timer = lv_timer_create(uq_rebuild_cb, 100, NULL);
    lv_timer_reset(uq_rebuild_timer);
    lv_timer_resume(uq_rebuild_timer);
}
static bool uq_snapshot_valid(void)
{
    struct playlist_track_info info;
    return playlist_amount() == uq_snapshot_amount && cur_index() == uq_snapshot_current &&
        playlist_get_track_info(NULL, uq_snapshot_current, &info) >= 0 &&
        !strcmp(info.filename, uq_snapshot_path);
}
static void uq_refresh_if_changed(void)
{
    if (!uq_dragging && !uq_snapshot_valid()) uq_rebuild_soon();
}

/* A skip back puts the song that was playing in front of the queue: after
 * any track or list change, gather the queue again so it plays next. */
static void q_poll(void)
{
    static int last_cur = -1, last_amount = -1;
    if (!something_playing() || quiz_clip_active()) return;
    if (cur_index() == last_cur && playlist_amount() == last_amount) return;
    int moved = q_tidy();
    if (moved)
    {
        sl_log("queue: gathered %d after a track change", moved);
        if (uq_open) uq_rebuild_soon();
    }
    last_cur = cur_index();
    last_amount = playlist_amount();
}

static void uq_clear_cb(lv_event_t *e)
{
    (void)e;
    if (!uq_snapshot_valid()) { uq_rebuild_soon(); return; }
    int cur = cur_index(), removed = 0;
    bool failed = false;
    /* Absolute descending indexes stay valid as entries are removed, even
     * when the play order wraps through index zero. */
    for (int idx = playlist_amount() - 1; idx >= 0; idx--)
    {
        int attr = idx != cur ? playlist_get_track_attr(NULL, idx) : -1;
        if (attr >= 0 && (attr & PLAYLIST_ATTR_QUEUED))
        {
            if (playlist_delete(NULL, idx) < 0) { failed = true; break; }
            removed++;
        }
        if ((idx & 15) == 0) yield();
    }
    sl_log("queue cleared (%d)", removed);
    failed ? toast_warn("Could not finish clearing queue") : toast("Queue cleared");
    uq_rebuild_soon();
}
static void uq_jump_cb(lv_event_t *e)
{
    if (!uq_snapshot_valid()) { uq_rebuild_soon(); return; }
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int steps = q_jump_steps(k);
    if (steps > 0) audio_skip(steps, 0);
    uq_rebuild_soon();
}
static void uq_remove_cb(lv_event_t *e)
{
    if (!uq_snapshot_valid()) { uq_rebuild_soon(); return; }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int result = playlist_delete(NULL, idx);
    sl_log("queue remove %d, result %d, %d left", idx, result, playlist_amount());
    if (result < 0) toast_warn("Could not remove this song");
    uq_rebuild_soon();
}

/* dragging a queued row by its handle reorders the queue */
#define UQ_ROW_H 92
static int uq_drag_y0;
static int uq_drag_scroll_y0, uq_drag_pointer_y;
static int uq_drag_slot, uq_drag_target;
static lv_timer_t *uq_settle_timer, *uq_scroll_timer;
static void uq_settle_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    uq_dragging = false;
    if (uq_open) uq_build();
}
static void uq_shift_neighbours(void)
{
    int count = uq_drag_group ? uq_next_count : uq_queue_count;
    lv_obj_t **rows = uq_drag_group ? uq_next_rows : uq_queue_rows;
    for (int i = 0; i < count; i++)
    {
        if (i == uq_drag_slot) continue;
        int offset = 0;
        if (uq_drag_target > uq_drag_slot && i > uq_drag_slot && i <= uq_drag_target)
            offset = -UQ_ROW_H;
        else if (uq_drag_target < uq_drag_slot && i >= uq_drag_target && i < uq_drag_slot)
            offset = UQ_ROW_H;
        lv_obj_t *row = rows[i];
        int from = lv_obj_get_style_translate_y(row, 0);
        if (from != offset) anim_prop(row, set_ty, from, offset, SL_T_FAST, NULL);
    }
}
static void uq_drag_update(void)
{
    lv_area_t area;
    lv_obj_get_coords(uq_list, &area);
    int scroll = lv_obj_get_scroll_y(uq_list);
    int edge = 70, step = 14;
    if (uq_drag_pointer_y < area.y1 + edge)
        lv_obj_scroll_to_y(uq_list, scroll - step, LV_ANIM_OFF);
    else if (uq_drag_pointer_y > area.y2 - edge)
        lv_obj_scroll_to_y(uq_list, scroll + step, LV_ANIM_OFF);

    int count = uq_drag_group ? uq_next_count : uq_queue_count;
    int dy = uq_drag_pointer_y - uq_drag_y0 +
             lv_obj_get_scroll_y(uq_list) - uq_drag_scroll_y0;
    int min_y = -uq_drag_slot * UQ_ROW_H;
    int max_y = (count - 1 - uq_drag_slot) * UQ_ROW_H;
    if (dy < min_y) dy = min_y;
    if (dy > max_y) dy = max_y;
    lv_obj_set_style_translate_y(uq_drag_row, dy, 0);
    int steps = dy >= 0 ? (dy + UQ_ROW_H / 2) / UQ_ROW_H :
                          -(-dy + UQ_ROW_H / 2) / UQ_ROW_H;
    int target = uq_drag_slot + steps;
    if (target != uq_drag_target)
    {
        uq_drag_target = target;
        uq_shift_neighbours();
    }
}
static void uq_scroll_cb(lv_timer_t *timer)
{
    if (!uq_dragging) { lv_timer_pause(timer); return; }
    uq_drag_update();
}
static void uq_handle_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *h = lv_event_get_current_target(e);
    lv_obj_t *row = lv_obj_get_parent(h);
    lv_point_t p;
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED)
    {
        /* The last drop is still settling and its rebuild would delete this
         * row mid-drag, leaving the list unscrollable. */
        if (uq_dragging) return;
        if (!uq_snapshot_valid()) { uq_rebuild_soon(); return; }
        int from = (int)(intptr_t)lv_obj_get_user_data(row);
        int slot = 0;
        while (slot < uq_queue_count && uq_queue_indexes[slot] != from) slot++;
        uq_drag_group = 0;
        if (slot == uq_queue_count)
        {
            slot = 0;
            while (slot < uq_next_count && uq_next_indexes[slot] != from) slot++;
            if (slot == uq_next_count) return;
            uq_drag_group = 1;
        }
        uq_dragging = true;
        uq_drag_row = row;
        uq_drag_slot = uq_drag_target = slot;
        uq_drag_y0 = p.y;
        uq_drag_pointer_y = p.y;
        lv_obj_scroll_to_y(uq_list, lv_obj_get_scroll_y(uq_list), LV_ANIM_OFF);
        uq_drag_scroll_y0 = lv_obj_get_scroll_y(uq_list);
        lv_obj_remove_flag(uq_list, LV_OBJ_FLAG_SCROLLABLE);
        if (!uq_scroll_timer) uq_scroll_timer = lv_timer_create(uq_scroll_cb, 30, NULL);
        lv_timer_reset(uq_scroll_timer);
        lv_timer_resume(uq_scroll_timer);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD_P), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        sl_elevate(row, SL_E3, 0);   /* lifted off the list under the finger */
    }
    else if (c == LV_EVENT_PRESSING && uq_dragging && row == uq_drag_row)
    {
        uq_drag_pointer_y = p.y;
        uq_drag_update();
    }
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        if (!uq_dragging || row != uq_drag_row) return;
        if (uq_scroll_timer) lv_timer_pause(uq_scroll_timer);
        lv_obj_add_flag(uq_list, LV_OBJ_FLAG_SCROLLABLE);
        bool moved = c == LV_EVENT_RELEASED && uq_drag_target != uq_drag_slot && uq_snapshot_valid();
        if (moved)
        {
            int *indexes = uq_drag_group ? uq_next_indexes : uq_queue_indexes;
            int from = indexes[uq_drag_slot];
            int to = indexes[uq_drag_target];
            int result = q_move(from, to);
            sl_log("queue reorder %d -> %d, result %d", from, to, result);
            if (result < 0)
            { toast_warn("Could not move this song"); moved = false; }
        }
        if (!moved) uq_drag_target = uq_drag_slot;
        uq_shift_neighbours();
        int final_y = (uq_drag_target - uq_drag_slot) * UQ_ROW_H;
        anim_prop(row, set_ty, lv_obj_get_style_translate_y(row, 0), final_y, SL_T_FAST, NULL);
        if (!uq_settle_timer) uq_settle_timer = lv_timer_create(uq_settle_cb, 190, NULL);
        lv_timer_reset(uq_settle_timer);
        lv_timer_resume(uq_settle_timer);
    }
}

static lv_obj_t *uq_row(int idx, int k, const char *path, bool now)
{
    int t = sl_lib_find_path(path);
    lv_obj_t *r = box(uq_list);
    lv_obj_set_size(r, lv_pct(100), UQ_ROW_H - 4);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_set_user_data(r, (void *)(intptr_t)idx);
    if (now)
    {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_20, 0);
    }
    else
    {
        lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
        on_click(r, uq_jump_cb, (void *)(intptr_t)k);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    /* laid out from the leading edge: in Hebrew and Arabic the cover is at
     * the right and the × and handle at the left */
    if (t >= 0)
    {
        lv_obj_t *c = cover(r, sl_lib.tracks[t].album, SL_ART_ROW, false);
        sl_align(c, LV_ALIGN_LEFT_MID, 8, 0);
    }
    const char *title = t >= 0 ? sl_lib.tracks[t].title : strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    lv_obj_t *t1 = text(r, title, &ss_semibold_26, 0xffffff);
    sl_place(t1, 78, 10);
    lv_obj_set_width(t1, W - 28 - 78 - (now ? 12 : 110));
    label_lines(t1, 1);
    const char *artist = t >= 0 ? sl_track_artist(t) : "";
    lv_obj_t *t2 = text(r, artist, &ss_semibold_21, C_SUB);
    sl_place(t2, 78, 49);
    lv_obj_set_width(t2, W - 28 - 78 - (now ? 12 : 110));
    label_lines(t2, 1);
    /* a title starts by its cover whatever its script (the All list's rule) */
    if (!sl_ui_rtl())
    {
        lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_LEFT, 0);
    }
    if (!now)
    {
        lv_obj_t *x = icon_btn(r, LV_SYMBOL_CLOSE, 48, uq_remove_cb, (void *)(intptr_t)idx);
        sl_align(x, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_t *h = box(r);
        lv_obj_set_size(h, 52, UQ_ROW_H - 4);
        sl_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_center(text(h, LV_SYMBOL_BARS, &sl_sym_24, C_DIM));   /* a fixed pale grey vanished on light */
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(h, uq_handle_cb, LV_EVENT_ALL, NULL);
    }
    sw_attach(r, t);
    return r;
}

static bool uq_snapshot_valid(void);
static void uq_more(int n)
{
    if (!uq_open || uq_dragging || !uq_snapshot_valid()) return;
    struct playlist_track_info info;
    /* the queue's rows first, each after the last one made */
    for (; uq_queue_k <= uq_queue_run && uq_queue_count < UQ_QMAX && uq_queue_count > 0 && n > 0; uq_queue_k++)
    {
        int idx = (uq_snapshot_current + uq_queue_k) % uq_snapshot_amount;
        if (playlist_get_track_info(NULL, idx, &info) < 0) continue;
        int at = (int)lv_obj_get_index(uq_queue_rows[uq_queue_count - 1]) + 1;
        lv_obj_t *r = uq_row(idx, uq_queue_k, info.filename, false);
        lv_obj_move_to_index(r, at);
        uq_queue_indexes[uq_queue_count] = idx;
        uq_queue_rows[uq_queue_count++] = r;
        n--;
    }
    if (uq_next_k > uq_next_ahead || uq_next_count >= 60) return;
    for (; uq_next_k <= uq_next_ahead && uq_next_count < 60 && n > 0; uq_next_k++)
    {
        int idx = (uq_snapshot_current + uq_next_k) % uq_snapshot_amount;
        if (playlist_get_track_info(NULL, idx, &info) < 0) continue;
        uq_next_indexes[uq_next_count] = idx;
        uq_next_rows[uq_next_count++] = uq_row(idx, uq_next_k, info.filename, false);
        n--;
    }
}
static void uq_more_tick(lv_timer_t *t)
{
    (void)t;
    /* still, and not coasting from a flick: a little at a time */
    if (g_state == G_IDLE && !lv_anim_count_running() && lv_tick_elaps(uq_scrolled_at) > 300) uq_more(2);
}
static void uq_more_scroll(lv_event_t *e)
{
    (void)e;
    uq_scrolled_at = lv_tick_get();
    if (lv_obj_get_scroll_bottom(uq_list) < 2 * UQ_ROW_H) uq_more(3);
}
static void uq_build_rows(void);
static lv_obj_t *txt_btn(lv_obj_t *parent, const char *label, lv_event_cb_t cb);
static void uq_build(void)
{
    uint32_t started = sl_ms();
    /* A rebuild replaces every row, so no drag can survive it: reset the
     * drag state so a lost release can't strand the list unscrollable. */
    uq_dragging = false;
    if (uq_settle_timer) lv_timer_pause(uq_settle_timer);
    if (uq_list) lv_obj_add_flag(uq_list, LV_OBJ_FLAG_SCROLLABLE);
    uq_build_rows();
    /* Rows that were translated or shadowed during a drag can leave pixels
     * outside the rebuilt rows' areas; repaint the whole sheet once. */
    if (uq) lv_obj_invalidate(uq);
    sl_log("up next built: %d tracks, %d queued rows, %d next rows, %lu ms",
           playlist_amount(), uq_queue_count, uq_next_count, (unsigned long)(sl_ms() - started));
}
#include "sloppy_history_state.h"     /* SL_HIST_VALID_CLOCK */
static int hist_recent_when(int *out, uint32_t *when, int max);   /* sloppy_history.inc */
static uint32_t hist_clock(void);
static int uq_open_scroll;
static void uq_earlier_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t >= 0 && t < sl_lib.n_tracks) { q_play_next(t); }
}
/* hold a song heard earlier: its sheet (Play next, Add to queue, ...) */
static void uq_earlier_long_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_active());
    if (!uq_dragging) acts_open(t);
}
/* A song heard earlier dragged by its handle into the queue: it lands
 * where the line shows, between two queued songs, at the queue's end, or
 * first when nothing is queued. Dropped anywhere else it goes back. */
static lv_obj_t *uq_hist_row, *uq_hist_mark;
static int uq_hist_t, uq_hist_at = -1;   /* the song, and where it would go: 0..queue count, -1 nowhere */
static void uq_hist_update(int py)
{
    lv_area_t area;
    lv_obj_get_coords(uq_list, &area);
    int scroll = lv_obj_get_scroll_y(uq_list);
    if (py < area.y1 + 70) lv_obj_scroll_to_y(uq_list, scroll - 14, LV_ANIM_OFF);
    else if (py > area.y2 - 70) lv_obj_scroll_to_y(uq_list, scroll + 14, LV_ANIM_OFF);
    lv_obj_set_style_translate_y(uq_hist_row, py - uq_drag_y0 + lv_obj_get_scroll_y(uq_list) - uq_drag_scroll_y0, 0);
    /* where it would go: before the first queued song whose middle is under the finger */
    int at = -1, line_y = 0;
    lv_obj_t *nowrow = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(uq_list) && !nowrow; i++)
    {
        lv_obj_t *c = lv_obj_get_child(uq_list, i);
        if (lv_obj_get_user_data(c) == (void *)(intptr_t)uq_snapshot_current && lv_obj_get_height(c) >= UQ_ROW_H - 8) nowrow = c;
    }
    lv_area_t na;
    if (nowrow) lv_obj_get_coords(nowrow, &na);
    if (nowrow && py > na.y2 - UQ_ROW_H / 2)
    {
        at = uq_queue_count;
        if (uq_queue_count) { lv_area_t la; lv_obj_get_coords(uq_queue_rows[uq_queue_count - 1], &la); line_y = la.y2 + 2; }
        else line_y = na.y2 + 2;
        for (int i = 0; i < uq_queue_count; i++)
        {
            lv_area_t a;
            lv_obj_get_coords(uq_queue_rows[i], &a);
            if (py < (a.y1 + a.y2) / 2) { at = i; line_y = a.y1 - 2; break; }
        }
    }
    uq_hist_at = at;
    if (at < 0) { lv_obj_add_flag(uq_hist_mark, LV_OBJ_FLAG_HIDDEN); return; }
    lv_area_t la;
    lv_obj_get_coords(uq_list, &la);
    lv_obj_remove_flag(uq_hist_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(uq_hist_mark, line_y - la.y1 + lv_obj_get_scroll_y(uq_list) - 2);
}
static void uq_hist_handle_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target(e));
    lv_point_t p;
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED)
    {
        if (uq_dragging) return;
        if (!uq_snapshot_valid()) { uq_rebuild_soon(); return; }
        uq_dragging = true;
        uq_hist_row = row;
        uq_hist_t = (int)(intptr_t)lv_event_get_user_data(e);
        uq_hist_at = -1;
        uq_drag_y0 = p.y;
        uq_drag_scroll_y0 = lv_obj_get_scroll_y(uq_list);
        lv_obj_remove_flag(uq_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD_P), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        sl_elevate(row, SL_E3, 0);
        /* over the rows below it as it goes: lifted out of the list, a
         * space as tall left where it was so nothing moves */
        int x = lv_obj_get_x(row), y = lv_obj_get_y(row), at = (int)lv_obj_get_index(row);
        lv_obj_t *gap = box(uq_list);
        lv_obj_set_size(gap, lv_pct(100), lv_obj_get_height(row));
        lv_obj_move_to_index(gap, at);
        lv_obj_add_flag(row, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(row, x, y);
        lv_obj_move_foreground(row);
        uq_hist_mark = box(uq_list);
        lv_obj_add_flag(uq_hist_mark, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(uq_hist_mark, lv_pct(100), 4);
        lv_obj_set_style_radius(uq_hist_mark, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(uq_hist_mark, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(uq_hist_mark, LV_OPA_COVER, 0);
    }
    else if (c == LV_EVENT_PRESSING && uq_dragging && row == uq_hist_row) uq_hist_update(p.y);
    else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && uq_dragging && row == uq_hist_row)
    {
        lv_obj_add_flag(uq_list, LV_OBJ_FLAG_SCROLLABLE);
        int at = c == LV_EVENT_RELEASED ? uq_hist_at : -1, t = uq_hist_t;
        uq_dragging = false;
        uq_hist_row = NULL;
        if (at >= 0 && t >= 0 && t < sl_lib.n_tracks && uq_snapshot_valid())
        {
            /* on the queue's end, then where it was dropped */
            int count = uq_queue_count, before = at < count ? uq_queue_indexes[at] : -1;
            char path[MAX_PATH];
            int idx = q_insert(sl_track_path(t, path, sizeof path), false, true);
            if (idx >= 0 && before >= 0) q_move(idx, before);
            sl_log("queue: %s from history at %d of %d", idx >= 0 ? "dropped" : "could not drop", at, count);
            if (idx < 0) toast_warn("Could not queue this song");
            else sfx_play(SFX_UI_ON);
        }
        uq_build();                      /* the row back in its place, and the queue as it is now */
    }
}
/* the heading over a day's songs: Today, Yesterday, or the weekday and date */
static void uq_day_label(char *b, size_t n, uint32_t when, uint32_t now)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May",
        "June", "July", "August", "September", "October", "November", "December" };
    time_t tw = (time_t)when, tn = (time_t)now;
    struct tm a, d;
    localtime_r(&tw, &a);
    localtime_r(&tn, &d);
    long da = (long)(a.tm_year) * 400 + a.tm_yday, dn = (long)(d.tm_year) * 400 + d.tm_yday;
    char up[64];
    if (!when || when < SL_HIST_VALID_CLOCK) strlcpy(b, sl_tr("EARLIER"), n);
    else if (da == dn) strlcpy(b, sl_tr("TODAY"), n);
    else if (da == dn - 1 || (d.tm_yday == 0 && a.tm_year == d.tm_year - 1 && a.tm_mon == 11 && a.tm_mday == 31)) strlcpy(b, sl_tr("YESTERDAY"), n);
    else
    {
        snprintf(up, sizeof up, "%s %d %s", sl_tr(days[a.tm_wday % 7]), a.tm_mday, sl_tr(months[a.tm_mon % 12]));
        strlcpy(b, sl_upper(up, up, sizeof up), n);
    }
}
#define UQ_EARLIER 16
static void uq_build_rows(void)
{
    int previous_scroll = lv_obj_get_scroll_y(uq_list), prev_queue = uq_queue_count;
    /* a rebuild while open keeps the place: the rows down to it and a
     * screenful more, not all that were there (60 held the R1 ~600 ms on
     * each song change); the rest follow as on open */
    int want = UQ_FIRST;
    if (uq_open && uq_next_count > want)
    {
        want += previous_scroll / UQ_ROW_H;
        if (want > uq_next_count) want = uq_next_count;
    }
    lv_obj_clean(uq_list);
    uq_hist_mark = NULL;
    q_tidy();
    int amount = playlist_amount(), cur = cur_index();
    uq_snapshot_amount = amount;
    uq_snapshot_current = cur;
    uq_queue_count = 0;
    uq_next_count = 0;
    uq_snapshot_path[0] = '\0';
    struct playlist_track_info info;
    /* what played before, by day, newest nearest: a tap plays it again
     * next, a hold gives its sheet, its handle drags it into the queue */
    int earlier[UQ_EARLIER], n_earlier, shown_earlier = 0;
    uint32_t when[UQ_EARLIER], now = hist_clock();
    n_earlier = hist_recent_when(earlier, when, UQ_EARLIER);
    char day[64] = "";
    for (int k = n_earlier - 1; k >= 0; k--)
    {
        int t = earlier[k];
        if (t < 0 || t >= sl_lib.n_tracks || (amount > 0 && playlist_get_track_info(NULL, cur, &info) >= 0 &&
                                               sl_track_path_is(t, info.filename))) continue;
        char head[64];
        uq_day_label(head, sizeof head, when[k], now);
        if (strcmp(head, day)) { strlcpy(day, head, sizeof day); section(uq_list, day); }
        shown_earlier++;
        lv_obj_t *r = box(uq_list);
        lv_obj_set_size(r, lv_pct(100), 64);
        lv_obj_add_style(r, &st_row, 0);
        lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
        lv_obj_set_style_opa(r, 170, 0);   /* gone by: a little faded */
        lv_obj_t *c = cover(r, sl_lib.tracks[t].album, 46, false);
        sl_align(c, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_t *a = text(r, sl_lib.tracks[t].title, &ss_semibold_21, C_TEXT);
        sl_place(a, 66, 8);
        lv_obj_set_width(a, W - 28 - 66 - 60);
        label_lines(a, 1);
        lv_obj_t *b2 = text(r, sl_track_artist(t), &ss_regular_16, C_SUB);
        sl_place(b2, 66, 36);
        lv_obj_set_width(b2, W - 28 - 66 - 60);
        label_lines(b2, 1);
        if (!sl_ui_rtl())
        {
            lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_align(b2, LV_TEXT_ALIGN_LEFT, 0);
        }
        on_click(r, uq_earlier_cb, (void *)(intptr_t)t);
        lv_obj_add_event_cb(r, uq_earlier_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)t);
        lv_obj_t *h = box(r);
        lv_obj_set_size(h, 52, 64);
        sl_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_center(text(h, LV_SYMBOL_BARS, &sl_sym_24, C_DIM));
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(h, uq_hist_handle_cb, LV_EVENT_ALL, (void *)(intptr_t)t);
    }
    lv_obj_t *s = section(uq_list, "NOW PLAYING");
    (void)s;
    if (amount > 0 && playlist_get_track_info(NULL, cur, &info) >= 0)
    {
        strlcpy(uq_snapshot_path, info.filename, sizeof uq_snapshot_path);
        uq_row(cur, 0, info.filename, true);
    }

    /* Rows in play order: the queued run (sloppy_queue.h keeps it whole,
     * straight after this song), then what follows it. The flags come from
     * memory; a file name is read (card I/O without dircache) only for the
     * rows that are shown. */
    int queued = q_run(), ahead = q_ahead();
    char b[64];
    /* nothing queued and nothing after: one empty state, not two headings
     * over two "nothing"s; nothing queued but more to come: no heading */
    bool empty = !queued && ahead <= 0 && !sr_on();
    if (empty)
        sl_empty(uq_list, IC_UPNEXT, 64, "Nothing up next",
                 "Songs you queue will play after this one.", 40);
    if (queued)
    {
        snprintf(b, sizeof b, sl_tr("IN YOUR QUEUE · %d"), queued);
        section(uq_list, b);
    }
    /* the first of the queue now, the rest as the list nears them (each
     * row reads its file name from the card), as Next up's do */
    int want_q = UQ_FIRST * 2;
    if (uq_open && prev_queue > want_q) want_q = LV_MIN(prev_queue, UQ_QMAX);   /* a rebuild keeps what was there */
    uq_queue_count = 0;
    int kq;
    for (kq = 1; kq <= queued && uq_queue_count < want_q; kq++)
    {
        int idx = (cur + kq) % amount;
        if (playlist_get_track_info(NULL, idx, &info) >= 0)
        {
            uq_queue_indexes[uq_queue_count] = idx;
            uq_queue_rows[uq_queue_count++] = uq_row(idx, kq, info.filename, false);
        }
    }
    uq_queue_k = kq;
    uq_queue_run = queued;
    if (queued > UQ_QMAX)
    {
        char more[64], num[16];
        snprintf(more, sizeof more, sl_tr("The first %s queued songs are shown"), sl_fmt_count(num, sizeof num, UQ_QMAX));
        lv_obj_t *m = text(uq_list, more, &ss_regular_18, C_DIM);
        lv_obj_set_style_pad_all(m, 10, 0);
    }
    if (sr_on())
    {
        /* what radio picked, and the way out of it */
        lv_obj_t *hr = box(uq_list);
        lv_obj_set_size(hr, lv_pct(100), 56);
        lv_obj_t *h = section(hr, "RADIO");
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *from = text(hr, sr_seed_title(), &ss_regular_18, C_SUB);
        lv_obj_set_width(from, W - 28 - 90 - 150);
        label_lines(from, 1);
        lv_obj_align(from, LV_ALIGN_LEFT_MID, 82, 2);
        lv_obj_align(txt_btn(hr, "Stop radio", uq_radio_stop_cb), LV_ALIGN_RIGHT_MID, 0, 0);
    }
    else if (!empty)
        section(uq_list, "NEXT UP");
    int rest = 0, k;
    for (k = queued + 1; k <= ahead && rest < want; k++)
    {
        int idx = (cur + k) % amount;
        if (playlist_get_track_info(NULL, idx, &info) >= 0)
        {
            uq_next_indexes[uq_next_count] = idx;
            uq_next_rows[uq_next_count++] = uq_row(idx, k, info.filename, false);
            rest++;
        }
    }
    uq_next_k = k;
    uq_next_ahead = ahead;
    if (!rest && !empty)
    {
        /* with songs queued, those are what comes next: say so, not "nothing" */
        lv_obj_t *e = text(uq_list, queued ? "Nothing after your queue." : "Nothing after this song.", &ss_regular_18, C_DIM);
        lv_obj_set_style_pad_all(e, 10, 0);
    }
    lv_obj_t *clr = lv_obj_get_child(lv_obj_get_child(uq, 1), 1);
    if (queued) lv_obj_remove_flag(clr, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(clr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(uq_list);
    /* opened: Now playing at the top, Earlier just above it, a pull away */
    uq_open_scroll = !uq_open && shown_earlier ? lv_obj_get_y(s) : 0;   /* applied once it shows */
    lv_obj_scroll_to_y(uq_list, previous_scroll, LV_ANIM_OFF);
}

/* Pulling the sheet down, from its header anywhere, or from the list once
 * it is scrolled to the top: the sheet follows the finger (touch_read's
 * G_SHEET), and closes on a long enough pull or a flick. A drag handle
 * keeps reordering, and a sideways swipe stays a row swipe. */
#define UQ_TOP 48
static bool uq_sheet_grab(lv_obj_t *hit)
{
    if (!uq_open || uq_dragging || !hit) return false;
    bool inside = false;
    for (lv_obj_t *o = hit; o; o = lv_obj_get_parent(o))
    {
        if (o == uq) { inside = true; break; }
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_PRESS_LOCK) && lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1))
            return false;              /* a drag handle (a row is only PRESS_LOCK, for swipes) */
    }
    if (!inside) return false;
    return g_y0 < UQ_TOP + 88 || lv_obj_get_scroll_y(uq_list) <= 0;
}
static void uq_sheet_drag(int dy)
{
    lv_anim_delete(uq, set_ty);
    /* past the top it gives a little, like a rubber band */
    if (dy < 0) dy = -(int)(sqrtf((float)-dy) * 3);
    lv_obj_set_style_translate_y(uq, dy, 0);
}
static void uq_close_to(int from, int ms)
{
    if (!uq_open) return;
    uq_open = false;
    lv_anim_delete(uq, set_ty);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, uq);
    lv_anim_set_values(&a, from, H - UQ_TOP);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, set_ty);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, hide_cb);
    lv_anim_start(&a);
}
static void uq_sheet_release(int dy, int vy)
{
    int from = lv_obj_get_style_translate_y(uq, 0);
    int height = H - UQ_TOP;
    /* a quarter of the sheet, or a flick that has really moved it */
    if (dy > height / 4 || (vy > 700 && dy > 40))
    {
        /* keep the finger's speed: the faster the flick, the shorter the
         * rest of the trip, between 120 and 300 ms */
        int left = height - from, speed = vy > 1400 ? vy : 1400;
        int ms = left * 1000 / speed;
        uq_close_to(from, ms < 120 ? 120 : ms > 300 ? 300 : ms);
    }
    else
        anim_prop(uq, set_ty, from, 0, SL_T_PAGE, NULL);
}

static lv_obj_t *txt_btn(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_min_width(b, SL_TAP + SL_SP_S, 0);   /* a short word ("تم") is still a finger's width */
    lv_obj_set_style_radius(b, SL_R_MD, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_21, C_ACCENT_TXT);   /* the accent as words: readable on light */
    lv_obj_center(l);
    on_click(b, cb, NULL);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}

static void uq_show(void)
{
    if (!something_playing() || uq_open) return;
    if (!uq)
    {
        uq = box(ui_screen());
        lv_obj_set_size(uq, W, H - 48);
        lv_obj_set_pos(uq, 0, 48);
        lv_obj_set_style_radius(uq, SL_R_LG, 0);
        lv_obj_set_style_bg_color(uq, lv_color_hex(0x1b2a3a), 0);
        lv_obj_set_style_bg_opa(uq, LV_OPA_COVER, 0);
        sl_elevate(uq, SL_E4, 0);
        lv_obj_add_flag(uq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_remove_flag(uq, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *grab = box(uq);                     /* child 0 */
        lv_obj_set_size(grab, 56, 6);
        lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_radius(grab, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(grab, LV_OPA_40, 0);

        lv_obj_t *hd = box(uq);                       /* child 1 */
        lv_obj_set_size(hd, W, 64);
        lv_obj_set_pos(hd, 0, 20);
        lv_obj_add_flag(hd, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(hd, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *t = text(hd, "Up next", &ss_semibold_30, 0xffffff);    /* hd child 0 */
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *clr = txt_btn(hd, "Clear queue", uq_clear_cb);   /* hd child 1 */
        lv_obj_align(clr, LV_ALIGN_RIGHT_MID, -96, 0);
        lv_obj_t *dn = txt_btn(hd, "Done", uq_done_cb);
        lv_obj_align(dn, LV_ALIGN_RIGHT_MID, -12, 0);

        uq_list = lv_obj_create(uq);
        lv_obj_remove_style_all(uq_list);
        lv_obj_set_pos(uq_list, 0, 88);
        lv_obj_set_size(uq_list, W, H - 48 - 88);
        lv_obj_set_style_pad_hor(uq_list, 14, 0);
        lv_obj_set_style_pad_bottom(uq_list, 30, 0);
        lv_obj_set_flex_flow(uq_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(uq_list, 4, 0);
        lv_obj_set_scroll_dir(uq_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(uq_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(uq_list, uq_more_scroll, LV_EVENT_SCROLL, NULL);
        lv_timer_create(uq_more_tick, 60, NULL);
    }
    /* the theme, each time (the sheet is built once): Zune's is black,
     * square, its title big and lowercase */
    /* light: a white sheet, as its rows are drawn in the theme's ink */
    lv_obj_set_style_bg_color(uq, lv_color_hex(sl_zune() ? 0x050505 : sl_theme() == 1 ? C_CARD : 0x1b2a3a), 0);
    lv_obj_set_style_radius(uq, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(uq, 0), sl_theme() == 1 ? lv_color_hex(C_DIM) : lv_color_white(), 0);
    {
        lv_obj_t *t = lv_obj_get_child(lv_obj_get_child(uq, 1), 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
        char low[48];
        label_if_changed(t, sl_zune() ? sl_lower(sl_tr("Up next"), low, sizeof low) : sl_tr("Up next"));
        lv_obj_set_style_text_font(t, sl_zune() ? &ss_regular_34 : &ss_semibold_30, 0);
        for (int i = 1; i <= 2; i++)          /* Clear queue and Done take the accent now */
            lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(uq, 1), i), 0),
                                        lv_color_hex(sl_theme() == 1 ? C_ACCENT_TXT : C_ACCENT), 0);   /* readable on the white sheet */
        /* the language now (the sheet outlives a change of it), and the
         * header from the leading edge: mirrored in Hebrew and Arabic */
        lv_obj_t *hd = lv_obj_get_child(uq, 1);
        label_if_changed(lv_obj_get_child(lv_obj_get_child(hd, 1), 0), sl_tr("Clear queue"));
        label_if_changed(lv_obj_get_child(lv_obj_get_child(hd, 2), 0), sl_tr("Done"));
        sl_align(t, LV_ALIGN_LEFT_MID, 22, 0);
        sl_align(lv_obj_get_child(hd, 1), LV_ALIGN_RIGHT_MID, -96, 0);
        sl_align(lv_obj_get_child(hd, 2), LV_ALIGN_RIGHT_MID, -12, 0);
    }
    sl_log("up next open");
    /* reopened while still sliding away: rise from where it is */
    int from = lv_obj_has_flag(uq, LV_OBJ_FLAG_HIDDEN) ? H : lv_obj_get_style_translate_y(uq, 0);
    uq_build();
    lv_obj_move_foreground(uq);
    lv_obj_remove_flag(uq, LV_OBJ_FLAG_HIDDEN);
    if (uq_open_scroll) { lv_obj_update_layout(uq_list); lv_obj_scroll_to_y(uq_list, uq_open_scroll, LV_ANIM_OFF); }
    uq_open = true;
    anim_prop(uq, set_ty, from, 0, SL_T_PAGE, NULL);
}

/* --------------------------------------------------------- crossfade */
static int xf_seconds(void)
{
    return global_settings.crossfade == CROSSFADE_ENABLE_OFF ? 0 : global_settings.crossfade_fade_out_duration;
}
static bool xf_on_skip(void)
{
    return global_settings.crossfade == CROSSFADE_ENABLE_ALWAYS ||
           global_settings.crossfade == CROSSFADE_ENABLE_MANSKIP ||
           global_settings.crossfade == CROSSFADE_ENABLE_SHUFFLE_OR_MANSKIP;
}
static int xf_last = 4;
static void xf_apply(int secs, bool on_skip)
{
    if (secs < 0) secs = 0;
    if (secs > 12) secs = 12;
    if (secs) xf_last = secs;
    global_settings.crossfade = secs == 0 ? CROSSFADE_ENABLE_OFF
                              : on_skip ? CROSSFADE_ENABLE_ALWAYS : CROSSFADE_ENABLE_AUTOSKIP;
    if (secs)
    {
        global_settings.crossfade_fade_in_delay = 0;
        global_settings.crossfade_fade_out_delay = 0;
        global_settings.crossfade_fade_in_duration = secs;
        global_settings.crossfade_fade_out_duration = secs;
        global_settings.crossfade_fade_out_mixmode = 0;
    }
    audio_set_crossfade(global_settings.crossfade);
    settings_save();
    sl_log("crossfade %ds%s", secs, on_skip ? " (also on skip)" : "");
}

static lv_obj_t *xf_val, *xf_desc, *xf_slider, *xf_switch, *xf_graph;
static lv_obj_t *xf_graph_owner;
static lv_timer_t *xf_graph_timer;
static uint32_t xf_graph_started;
static int xf_shown;                 /* shown fade, seconds * 100, eases to the slider */
#define XF_GRAPH_H   132
#define XF_SPAN      26              /* seconds of music across the preview */

/* The incoming song's colour: a warm coral, or a soft blue when the accent
 * itself is warm, so the two songs never look alike. */
static uint32_t xf_incoming_color(void)
{
    uint32_t c = C_ACCENT;
    lv_color_hsv_t h = lv_color_rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
    return h.s > 20 && (h.h < 60 || h.h > 320) ? 0x7aa7ff : 0xff9f6e;
}
static int xf_noise(int i, int song)
{
    unsigned x = (unsigned)(i * 374761393 + song * 668265263);
    x = (x ^ (x >> 13)) * 1274126177u;
    return (int)((x ^ (x >> 16)) % 1000);
}
static void xf_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

/* Two songs as mirrored waveforms. The outgoing one (accent) fades out and
 * the incoming one fades in across the crossfade window with equal-power
 * curves; the playhead sweeps through and lights what it has passed. */
static void xf_graph_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    uint32_t out_c = C_ACCENT, in_c = xf_incoming_color();
    bool light = sl_theme() == 1;
    xf_rect(layer, a.x1, a.y1, a.x2, a.y2, light ? 0xe9eef3 : sl_theme() == 2 ? 0x0b0b0d : 0x0f1823, LV_OPA_COVER, 18);

    int pad = 14, x0 = a.x1 + pad, iw = w - 2 * pad, cy = a.y1 + h / 2 + 8;
    float fade = xf_shown / 100.0f;
    int fw = (int)(fade / XF_SPAN * iw), mid = x0 + iw / 2;
    int fs = mid - fw / 2, fe = mid + fw / 2;
    uint32_t t = sl_ms() - xf_graph_started;
    int play = x0 + (int)((t % 4200) * (uint32_t)iw / 4200);

    /* the crossfade window, with its length on a small pill */
    char b[12];
    if (fw > 2)
    {
        xf_rect(layer, fs, a.y1 + 30, fe, a.y2 - 10, light ? 0x182735 : 0xffffff, light ? 14 : 16, 10);
        snprintf(b, sizeof b, "%d s", (xf_shown + 50) / 100);
    }
    else
    {
        xf_rect(layer, mid - 1, a.y1 + 30, mid, a.y2 - 10, light ? 0x182735 : 0xffffff, 70, 0);
        strlcpy(b, "Cut", sizeof b);
    }
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = &ss_semibold_18;
    l.color = lv_color_hex(C_TEXT);
    l.text = b;
    l.align = LV_TEXT_ALIGN_CENTER;
    xf_rect(layer, mid - 30, a.y1 + 6, mid + 30, a.y1 + 28, light ? 0xffffff : 0x26313c, LV_OPA_COVER, 11);
    lv_area_t la = { mid - 30, a.y1 + 7, mid + 30, a.y1 + 29 };
    lv_draw_label(layer, &l, &la);

    /* bars: 4 px wide, 2 px apart, gently breathing */
    int maxh = (a.y2 - 10 - (a.y1 + 34)) / 2;
    for (int x = x0, i = 0; x + 4 <= x0 + iw; x += 6, i++)
    {
        float u = fw > 2 ? (x + 2 - fs) / (float)fw : (x + 2 < mid ? -1.0f : 2.0f);
        float ga = u <= 0 ? 1 : u >= 1 ? 0 : cosf(u * 1.5707963f);
        float gb = u <= 0 ? 0 : u >= 1 ? 1 : sinf(u * 1.5707963f);
        bool played = x + 2 <= play;
        for (int song = 0; song < 2; song++)
        {
            float g = song ? gb : ga;
            if (g < 0.03f) continue;
            float amp = 0.30f + 0.70f * (xf_noise(i, song) * 0.6f + xf_noise(i + 1, song) * 0.4f) / 1000.0f;
            amp *= 0.88f + 0.12f * sinf(t / 260.0f + i * 0.7f + song * 2.0f);
            int bh = (int)(amp * g * maxh);
            if (bh < 2) bh = 2;
            lv_opa_t opa = (lv_opa_t)((played ? 235 : 110) * (song && ga > 0.03f ? 0.85f : 1.0f));
            xf_rect(layer, x, cy - bh, x + 3, cy + bh, song ? in_c : out_c, opa, 2);
        }
    }

    /* playhead with a soft glow */
    lv_draw_rect_dsc_t g;
    lv_draw_rect_dsc_init(&g);
    g.bg_color = lv_color_hex(C_TEXT);
    g.bg_opa = 230;
    g.radius = 1;
    g.shadow_width = 14;
    g.shadow_color = lv_color_hex(C_ACCENT);
    g.shadow_opa = 150;
    lv_area_t pa = { play - 1, a.y1 + 34, play, a.y2 - 12 };
    lv_draw_rect(layer, &g, &pa);
    xf_rect(layer, play - 5, a.y1 + 30, play + 4, a.y1 + 39, C_TEXT, LV_OPA_COVER, 5);
}

static void xf_graph_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!xf_graph || !xf_slider) return;
    int target = lv_slider_get_value(xf_slider) * 100;
    int diff = target - xf_shown;               /* ease out, then land exactly */
    if (diff > 12 || diff < -12) xf_shown += diff / 4 + (diff > 0 ? 3 : -3);
    else xf_shown = target;
    lv_obj_invalidate(xf_graph);
}

static void xf_graph_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != xf_graph_owner) return;
    if (xf_graph_timer) { lv_timer_delete(xf_graph_timer); xf_graph_timer = NULL; }
    xf_graph_owner = xf_graph = xf_slider = xf_switch = xf_val = xf_desc = NULL;
}
static void xf_labels(int n)
{
    char b[16];
    if (n) snprintf(b, sizeof b, "%d s", n); else strlcpy(b, sl_tr("Off"), sizeof b);
    lv_label_set_text(xf_val, b);
    char d[200];
    if (n) snprintf(d, sizeof d, sl_tr("The last %d seconds of each song blend into the next."), n);
    else strlcpy(d, sl_tr("Songs play back to back with no overlap."), sizeof d);
    lv_label_set_text(xf_desc, d);
    if (xf_graph) lv_obj_invalidate(xf_graph);
}
static void xf_slider_cb(lv_event_t *e)
{
    int n = lv_slider_get_value(xf_slider);
    xf_labels(n);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        xf_apply(n, lv_obj_has_state(xf_switch, LV_STATE_CHECKED));
}
static void xf_switch_cb(lv_event_t *e)
{
    (void)e;
    xf_apply(lv_slider_get_value(xf_slider), lv_obj_has_state(xf_switch, LV_STATE_CHECKED));
}
static void style_slider(lv_obj_t *s)
{
    /* the track: white over glass and dark cards, ink over light ones */
    bool light = sl_theme() == 1;
    lv_obj_set_style_bg_color(s, light ? lv_color_hex(C_DIM) : lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, light ? LV_OPA_40 : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_ACCENT_UI), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    int h = lv_obj_get_style_height(s, 0);
    sl_slider_knob(s, h, h > SL_TRACK_H);   /* the control centre's thicker tracks take the large knob */
}
static void switch_sound_cb(lv_event_t *e)
{
    sfx_play(lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED) ? SFX_UI_ON : SFX_UI_OFF);
}
static void style_switch(lv_obj_t *sw)
{
    lv_obj_add_event_cb(sw, switch_sound_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(sw, 64, 36);
    lv_obj_set_style_bg_color(sw, lv_color_hex(sl_theme() == 1 ? 0xb9c4ce : 0x3a4656), LV_PART_MAIN);
    /* off, the track alone was under 2:1 against its card: outline it */
    lv_obj_set_style_border_width(sw, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, lv_color_hex(C_ACCENT_UI), LV_PART_MAIN | LV_STATE_CHECKED);   /* same outline, in the fill */
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT_UI), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sw, (SL_TAP - 36) / 2);   /* the 36 px track takes a finger-sized tap */
}

/* --------------------------------------------------------- equalizer */
/* A ten-band octave graphic equalizer on Rockbox's DSP: a low shelf, eight
 * peaking bands and a high shelf. The bands sit on the ISO octave centres.
 * Peaking bands use Q 1.41, the bandwidth of one octave, so a band moves its
 * own octave and neighbours do not pile up. The shelves turn over at the
 * band edge (44 Hz, 11.3 kHz), so the 31 Hz and 16 kHz bands reach their
 * gain where their label says. The graph draws the response the DSP applies,
 * computed from the same RBJ filter formulas. */
#define EQ_MAX_DB 12
static const char *eq_names[EQ_NUM_BANDS] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const int eq_centres[EQ_NUM_BANDS] = { 31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const int eq_cutoffs[EQ_NUM_BANDS] = { 44, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 11314 };
#define EQ_Q_PEAK  14                       /* tenths: Q 1.41, one octave */
#define EQ_Q_SHELF 7                        /* Q 0.71: no overshoot */
#define EQ_PRESETS 8
static const struct { const char *name; signed char g[EQ_NUM_BANDS]; } eq_presets[EQ_PRESETS] = {
    { "Flat",     {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 } },
    { "Bass",     {  6,  5,  3,  1,  0,  0,  0,  0,  0,  0 } },
    { "Warm",     {  3,  2,  1,  1,  0,  0, -1, -1, -2, -2 } },
    { "Vocal",    { -3, -2, -1,  1,  3,  3,  2,  1,  0, -1 } },
    { "Bright",   {  0,  0,  0,  0,  0,  1,  2,  3,  4,  4 } },
    { "Rock",     {  4,  3,  1, -1, -2, -1,  1,  2,  3,  4 } },
    { "Loudness", {  5,  4,  2,  0, -1,  0,  0,  1,  3,  4 } },   /* ears lose lows and highs when quiet */
    { "Speech",   { -6, -4, -2,  1,  3,  4,  3,  1, -1, -3 } },   /* podcasts: less rumble and hiss */
};
static lv_obj_t *eq_page_obj;
static lv_obj_t *eq_graph, *eq_dots[EQ_NUM_BANDS], *eq_chips[EQ_PRESETS], *eq_sw, *eq_read_f, *eq_read_g;
static lv_obj_t *eq_ticks[10];              /* Hz under a profile's graph */
static lv_obj_t *eq_steps[2];               /* the readout's - and + */
static lv_obj_t *eq_pre_row, *eq_level_line;  /* the Level card's preamp and its line */
static void eq_level_refresh(void);
static int set_text_h(const char *s, const lv_font_t *f, int w);
static lv_obj_t *eq_labels[EQ_NUM_BANDS], *eq_prof_row, *eq_chips_box, *eq_pq_row, *eq_presets_head;
/* curves of your own, saved by name: a line each, "name<TAB>ten gains in dB" */
#define EQ_MINE_FILE ROCKBOX_DIR "/sloppy/eq-presets.txt"
#define EQ_MINE_MAX 8
static struct { char name[24]; signed char g[EQ_NUM_BANDS]; } eq_mine[EQ_MINE_MAX];
static int eq_n_mine;
static bool eq_mine_read;
static lv_obj_t *eq_mine_chips[EQ_MINE_MAX], *eq_name_ta;
static int eq_sel = 4;                      /* the band the readout and -/+ adjust */

static int eq_gain(int i) { return global_settings.eq_band_settings[i].gain / 10; }

/* A headphone profile from AutoEq: each band keeps the frequency and Q the
 * profile gave it, and the graph shows that curve. Without one the bands
 * sit on the octaves above. The profile's name lives in a file of its own,
 * so the bands' own settings carry the rest. */
#define EQ_PROFILE_FILE ROCKBOX_DIR "/sloppy/eq-profile.txt"
#define EQ_AUTOEQ_DIR   "/AutoEq"
static char eq_profile[64];
static bool eq_profile_read;
static void eq_profile_load(void)
{
    if (eq_profile_read) return;
    eq_profile_read = true;
    int fd = open(EQ_PROFILE_FILE, O_RDONLY);
    if (fd < 0) return;
    if (read_line(fd, eq_profile, sizeof eq_profile) <= 0) eq_profile[0] = '\0';
    close(fd);
}
static void eq_profile_save(const char *name)
{
    strlcpy(eq_profile, name, sizeof eq_profile);
    eq_profile_read = true;
    if (!name[0]) { remove(EQ_PROFILE_FILE); return; }
    mkdir(ROCKBOX_DIR "/sloppy");
    /* through a .tmp: emptied by a power cut, the bands would quietly
     * turn back into octaves */
    int fd = open(EQ_PROFILE_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, name, strlen(name)) == (ssize_t)strlen(name) && fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(EQ_PROFILE_FILE ".tmp", EQ_PROFILE_FILE) < 0) { remove(EQ_PROFILE_FILE ".tmp"); sl_log("eq profile: not saved"); }
}
static bool eq_parametric(void) { eq_profile_load(); return eq_profile[0] != '\0'; }
static bool eq_shelf(int i) { return i == 0 || i == EQ_NUM_BANDS - 1; }
static int eq_band_hz(int i) { return eq_parametric() ? global_settings.eq_band_settings[i].cutoff : eq_cutoffs[i]; }
static int eq_band_q(int i)
{
    return eq_parametric() ? global_settings.eq_band_settings[i].q : eq_shelf(i) ? EQ_Q_SHELF : EQ_Q_PEAK;
}
/* where a band's dot sits: its octave, or the profile's own frequency */
static int eq_dot_hz(int i) { return eq_parametric() ? global_settings.eq_band_settings[i].cutoff : eq_centres[i]; }
/* the filter a band is: its own type, or (the octave bands, and every
 * EQ from before types) its place's: a low shelf first, a high shelf last */
static int eq_kind(int i) { return eq_band_kind(i, &global_settings.eq_band_settings[i]); }
static bool eq_pass(int i) { return eq_kind_is_pass(eq_kind(i)); }
static const char *const eq_type_names[EQ_TYPE_COUNT] = { "", "Peak", "Low shelf", "High shelf", "Low pass", "High pass" };
static float eq_response(float f);

/* What the DSP gets: the user's bands with Tone's filters on top (Tone
 * tuning, below). The user's own stay in global_settings, as saved. */
static struct eq_band_setting eq_applied[EQ_NUM_BANDS];
static int eq_applied_precut;               /* tenths of a dB cut; below 0 a gain */
static int eq_auto_cut;                     /* tenths auto level took off past the preamp */
static int eq_over;                         /* tenths the loudest boost passes the preamp's room: clipping, when auto level is off */
static void tone_eq_bands(struct eq_band_setting *t, bool para);
static float eq_response_of(const struct eq_band_setting *b, bool user, float f);
struct eq_bq { float b0, b1, b2, a1, a2; };     /* a band's biquad, normalised */
static int eq_curve(const struct eq_band_setting *bs, bool user, struct eq_bq *out);
static float eq_curve_db(const struct eq_bq *f, int n, float hz);
static bool tone_texture_on(void) { return global_settings.pbe > 0; }

static void eq_apply_all(void)
{
    bool para = eq_parametric(), on = global_settings.eq_enabled;
    struct eq_band_setting tone[EQ_NUM_BANDS];
    tone_eq_bands(tone, para);
    int maxg = -1000;
    bool any = false;
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        struct eq_band_setting *b = &global_settings.eq_band_settings[i], *a = &eq_applied[i];
        if (!para)
        {
            b->cutoff = eq_cutoffs[i];
            b->q = eq_shelf(i) ? EQ_Q_SHELF : EQ_Q_PEAK;
            b->type = EQ_TYPE_AUTO;             /* the octave bands are the fixed layout */
        }
        *a = *b;
        if (!on) { a->gain = 0; a->type = EQ_TYPE_AUTO; }   /* off: a pass filter rests too */
        /* the octave bands take Tone's gains where they stand; a profile
         * lends Tone the bands it leaves flat */
        if (!para) a->gain += tone[i].gain;
        else if (tone[i].gain) *a = tone[i];
        a->gain = a->gain > 200 ? 200 : a->gain < -200 ? -200 : a->gain;
        dsp_set_eq_coefs(i, a);
        if (eq_band_active(i, a))
        {
            any = true;
            if (!eq_kind_is_pass(eq_band_kind(i, a)) && a->gain > maxg) maxg = a->gain;
        }
    }
    /* overlapping bands can pass any one band's boost, and a sharp shelf or
     * pass filter rings past its own: the curve's highest point, an
     * eightieth of a decade apart, and around each band a 48th of an
     * octave apart, where a Q 20 resonance hides between the others */
    if (any)
    {
        static float oct48[49];
        if (!oct48[0]) for (int j = 0; j < 49; j++) oct48[j] = (float)pow(2.0, (j - 24) / 48.0);
        struct eq_bq cq[EQ_NUM_BANDS];
        int nq = eq_curve(eq_applied, false, cq);
        float top = -300;
        for (int k = 0; k <= 240; k++)
        {
            float r = eq_curve_db(cq, nq, 20.0f * (float)pow(10.0, k / 80.0));
            if (r > top) top = r;
        }
        for (int i = 0; i < EQ_NUM_BANDS; i++)
        {
            if (!eq_band_active(i, &eq_applied[i])) continue;
            for (int j = 0; j < 49; j++)
            {
                float f = eq_applied[i].cutoff * oct48[j];
                if (f < 20 || f > 20000) continue;
                float r = eq_curve_db(cq, nq, f);
                if (r > top) top = r;
            }
        }
        int peak = (int)ceilf(top * 10);
        if (peak > maxg) maxg = peak;
    }
    if (!any) maxg = 0;
    if (tone_texture_on()) maxg += 30;      /* bass texture's own EQ lifts about 3 dB */
    /* the preamp is the user's; auto level takes it lower, never higher,
     * so nothing boosted clips. The switch off: the preamp as set */
    int pre = on ? global_settings.sloppy_eq_preamp : 0, gain = pre;
    eq_over = maxg + pre;
    if (global_settings.sloppy_eq_auto_level && -maxg < gain) gain = -maxg;
    eq_auto_cut = pre - gain;
    eq_applied_precut = -gain;
    global_settings.eq_precut = eq_applied_precut < 0 ? 0 : eq_applied_precut;   /* Rockbox's own setting cuts only */
    dsp_set_eq_precut(eq_applied_precut);
    dsp_eq_enable(any || eq_applied_precut != 0);
}

/* The response at 44.1 kHz, from the RBJ biquads eq.c uses: each band's
 * filter made once (eq_curve), then read at any frequency (eq_curve_db). */
static int eq_curve(const struct eq_band_setting *bs, bool user, struct eq_bq *out)
{
    const float fs = 44100.0f, pi = 3.14159265f;
    int n = 0;
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        if (!eq_band_active(i, &bs[i])) continue;     /* eq.c leaves a flat band out */
        int kind = eq_band_kind(i, &bs[i]);
        int hz = user ? eq_band_hz(i) : bs[i].cutoff, qq = user ? eq_band_q(i) : bs[i].q;
        qq = qq < 1 ? 1 : qq > EQ_Q_TYPED_MAX ? EQ_Q_TYPED_MAX : qq;
        float w0 = 2.0f * pi * hz / fs;
        if (w0 > 0.98f * pi) w0 = 0.98f * pi;         /* eq.c keeps a filter below 0.49 fs */
        float A = (float)pow(10.0, bs[i].gain / 400.0), cw = cosf(w0), q = qq / 10.0f;
        float al = sinf(w0) / (2 * q), sa = 2 * sqrtf(A) * al;
        float b0, b1, b2, a0, a1, a2;
        switch (kind)
        {
            case EQ_TYPE_LOW_SHELF:
                b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - sa); a0 = (A + 1) + (A - 1) * cw + sa;
                a1 = -2 * ((A - 1) + (A + 1) * cw);     a2 = (A + 1) + (A - 1) * cw - sa;
                break;
            case EQ_TYPE_HIGH_SHELF:
                b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - sa); a0 = (A + 1) - (A - 1) * cw + sa;
                a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sa;
                break;
            case EQ_TYPE_LOW_PASS:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
                break;
            case EQ_TYPE_HIGH_PASS:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al;
                break;
            default:
                b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A;
                a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
                break;
        }
        struct eq_bq *f = &out[n++];
        f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0; f->a1 = a1 / a0; f->a2 = a2 / a0;
    }
    return n;
}
static float eq_curve_db(const struct eq_bq *f, int n, float hz)
{
    const float w = 2.0f * 3.14159265f * hz / 44100.0f;
    const float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
    float m = 1;
    for (int k = 0; k < n; k++, f++)
    {
        float nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = f->b1 * s1 + f->b2 * s2;
        float dr = 1 + f->a1 * c1 + f->a2 * c2, di = f->a1 * s1 + f->a2 * s2;
        m *= (nr * nr + ni * ni) / (dr * dr + di * di);
    }
    return m > 1e-30f ? 10.0f * log10f(m) : -300.0f;
}
static float eq_response_of(const struct eq_band_setting *bs, bool user, float f)
{
    struct eq_bq q[EQ_NUM_BANDS];
    return eq_curve_db(q, eq_curve(bs, user, q), f);
}
/* the user's own curve, as the graph shows it */
static float eq_response(float f) { return eq_response_of(global_settings.eq_band_settings, true, f); }

/* Graph geometry, relative to eq_graph: 20 Hz to 20 kHz across, +-12 dB. */
#define EQ_GX 18
#define EQ_GY 18
#define EQ_GW (lv_obj_get_width(eq_graph) - 2 * EQ_GX)
#define EQ_GH (lv_obj_get_height(eq_graph) - EQ_GY - 40)
static int eq_x(float f) { return EQ_GX + (int)(EQ_GW * log10f(f / 20.0f) / 3.0f); }
static int eq_y(float db)
{
    if (db > EQ_MAX_DB + 2) db = EQ_MAX_DB + 2;
    if (db < -EQ_MAX_DB - 0.5f) db = -EQ_MAX_DB - 0.5f;   /* a pass filter's fall stops above the Hz labels */
    return EQ_GY + (int)(EQ_GH * (EQ_MAX_DB - db) / (2.0f * EQ_MAX_DB));
}

static void eq_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(eq_graph, &a);
    int ox = a.x1, oy = a.y1, x0 = ox + EQ_GX, x1 = ox + EQ_GX + EQ_GW;
    uint32_t ink = C_TEXT, acc = global_settings.eq_enabled ? C_ACCENT : C_DIM;
    for (int db = -EQ_MAX_DB; db <= EQ_MAX_DB; db += 6)     /* the dB grid, 0 dB stronger */
        wallpaper_line(layer, x0, oy + eq_y(db), x1, oy + eq_y(db), ink, db ? 22 : 60, 1);
    for (int d = 100; eq_parametric() && d <= 10000; d *= 10)   /* a profile's bands sit anywhere: the decades */
        wallpaper_line(layer, ox + eq_x(d), oy + EQ_GY, ox + eq_x(d), oy + EQ_GY + EQ_GH, ink, 22, 1);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        wallpaper_line(layer, ox + eq_x(eq_dot_hz(i)), oy + EQ_GY, ox + eq_x(eq_dot_hz(i)),
                       oy + EQ_GY + EQ_GH, ink, i == eq_sel ? 50 : 14, 1);
    /* the response: a soft fill to 0 dB, then the line. Each band's own
     * frequency is a point of the line too, so a Q 20 notch narrower than
     * the step shows its full depth */
    struct eq_bq cq[EQ_NUM_BANDS];
    int nq = eq_curve(global_settings.eq_band_settings, true, cq);
    int zero = oy + eq_y(0), px = -1, py = 0;
    for (int x = x0; x <= x1; x += 3)
    {
        for (int i = 0; px >= 0 && i < EQ_NUM_BANDS; i++)
        {
            int hz = eq_band_hz(i), bx = ox + eq_x(hz);
            if (bx <= px || bx >= x || !eq_band_active(i, &global_settings.eq_band_settings[i])) continue;
            int by = oy + eq_y(eq_curve_db(cq, nq, hz));
            wallpaper_line(layer, px, py, bx, by, acc, LV_OPA_COVER, 3);
            px = bx; py = by;
        }
        float f = 20.0f * (float)pow(10.0, 3.0f * (x - x0) / (float)EQ_GW);
        int y = oy + eq_y(eq_curve_db(cq, nq, f));
        if (y != zero)
            wallpaper_rect(layer, x - 1, y < zero ? y : zero, x + 1, y < zero ? zero : y, acc, 46, 0);
        if (px >= 0) wallpaper_line(layer, px, py, x, y, acc, LV_OPA_COVER, 3);
        px = x; py = y;
    }
}
/* where a band's dot sits: at its gain; a pass filter, which has none,
 * on its own curve at its frequency (-3 dB at Q 0.7) */
static float eq_dot_db(int i)
{
    const struct eq_band_setting *b = &global_settings.eq_band_settings[i];
    if (!eq_pass(i)) return b->gain / 10.0f;
    struct eq_band_setting one[EQ_NUM_BANDS];
    memset(one, 0, sizeof one);
    one[i] = *b;
    return eq_response_of(one, false, (float)b->cutoff);
}

static void eq_readout(void)
{
    char b[40];
    if (eq_parametric())
    {
        /* a profile's band: its own frequency and width, its gain to a tenth */
        const struct eq_band_setting *s = &global_settings.eq_band_settings[eq_sel];
        int g = s->gain, a = g < 0 ? -g : g, kind = eq_kind(eq_sel);
        char t[80];
        if (eq_kind_is_pass(kind))
        {
            /* a pass filter has no gain: its kind where the gain was */
            snprintf(t, sizeof t, "%d Hz \xc2\xb7 Q %d.%d", s->cutoff, s->q / 10, s->q % 10);
            lv_label_set_text(eq_read_f, t);
            lv_label_set_text(eq_read_g, sl_tr(eq_type_names[kind]));
            return;
        }
        /* right to left: the numbers laid left to right, the kind at the
         * right, where the line is read from */
        if (set_rtl()) snprintf(t, sizeof t, "%d Hz \xc2\xb7 Q %d.%d \xc2\xb7 %s", s->cutoff, s->q / 10, s->q % 10, sl_tr(eq_type_names[kind]));
        else snprintf(t, sizeof t, "%s \xc2\xb7 %d Hz \xc2\xb7 Q %d.%d", sl_tr(eq_type_names[kind]), s->cutoff, s->q / 10, s->q % 10);
        lv_obj_set_style_base_dir(eq_read_f, LV_BASE_DIR_LTR, 0);
        lv_label_set_text(eq_read_f, t);
        snprintf(b, sizeof b, "%s%d.%d dB", g > 0 ? "+" : g < 0 ? "-" : "", a / 10, a % 10);
        lv_label_set_text(eq_read_g, b);
        return;
    }
    int g = global_settings.eq_band_settings[eq_sel].gain, a = g < 0 ? -g : g;
    snprintf(b, sizeof b, "%s Hz", eq_names[eq_sel]);
    lv_label_set_text(eq_read_f, b);
    if (a % 10) snprintf(b, sizeof b, "%s%d.%d dB", g > 0 ? "+" : "-", a / 10, a % 10);   /* typed to a tenth */
    else snprintf(b, sizeof b, g > 0 ? "+%d dB" : g < 0 ? "-%d dB" : "%d dB", a / 10);
    lv_label_set_text(eq_read_g, b);
}
static void eq_refresh(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        lv_obj_t *d = eq_dots[i];
        bool sel = i == eq_sel;
        lv_obj_set_size(d, sel ? 26 : 18, sel ? 26 : 18);
        lv_obj_set_style_radius(d, SL_R_FULL, 0);
        lv_obj_set_style_border_width(d, sel ? 4 : 0, 0);
        lv_obj_set_pos(d, eq_x(eq_dot_hz(i)) - (sel ? 13 : 9), eq_y(eq_dot_db(i)) - (sel ? 13 : 9));
        lv_obj_set_style_bg_color(d, lv_color_hex(sel ? 0xffffff : (global_settings.eq_enabled ? C_ACCENT : C_DIM)), 0);
        /* a profile's bands can crowd: their names would pile up, the readout says each */
        if (eq_labels[i]) hidden_if_changed(eq_labels[i], eq_parametric());
    }
    for (int i = 0; i < 10; i++) if (eq_ticks[i]) hidden_if_changed(eq_ticks[i], !eq_parametric());
    if (eq_prof_row)
        set_link_text(eq_prof_row, eq_parametric() ? eq_profile : sl_tr("AutoEq profiles from the card"), eq_parametric() ? eq_profile : NULL);
    /* a profile's bands are moved by hand where the presets were */
    if (eq_chips_box)
    {
        hidden_if_changed(eq_chips_box, eq_parametric());
        hidden_if_changed(lv_obj_get_parent(eq_chips_box), eq_parametric());   /* its card, */
        hidden_if_changed(eq_presets_head, eq_parametric());                  /* and the card's header */
    }
    if (eq_pq_row) hidden_if_changed(eq_pq_row, !eq_parametric());
    for (int p = 0; p < EQ_PRESETS + eq_n_mine; p++)
    {
        lv_obj_t *chip = p < EQ_PRESETS ? eq_chips[p] : eq_mine_chips[p - EQ_PRESETS];
        if (!chip) continue;
        bool same = true;
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            if (eq_gain(i) != (p < EQ_PRESETS ? eq_presets[p].g[i] : eq_mine[p - EQ_PRESETS].g[i])) same = false;
        lv_obj_set_style_bg_color(chip, lv_color_hex(same ? C_ACCENT : C_CARD_P), 0);   /* as opt_chips */
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), lv_color_hex(same ? sl_accent_ink() : C_TEXT), 0);
    }
    for (int k = 0; k < 2; k++)                 /* a pass filter's gain is not there to step */
        if (eq_steps[k])
        {
            if (eq_parametric() && eq_pass(eq_sel)) lv_obj_add_state(eq_steps[k], LV_STATE_DISABLED);
            else lv_obj_remove_state(eq_steps[k], LV_STATE_DISABLED);
        }
    eq_readout();
    eq_level_refresh();
    lv_obj_invalidate(eq_graph);
}
/* Shaping the sound means wanting to hear it: an edit turns the EQ on. */
static void eq_edited(void)
{
    if (!global_settings.eq_enabled)
    {
        global_settings.eq_enabled = true;
        lv_obj_add_state(eq_sw, LV_STATE_CHECKED);
    }
    eq_apply_all();
    eq_refresh();
}
static void eq_set(int band, int db)
{
    if (db > EQ_MAX_DB) db = EQ_MAX_DB;
    if (db < -EQ_MAX_DB) db = -EQ_MAX_DB;
    eq_sel = band;
    if (eq_gain(band) == db || eq_pass(band)) { eq_refresh(); return; }   /* a pass filter has no gain to set */
    global_settings.eq_band_settings[band].gain = db * 10;
    eq_edited();
}
/* Touch anywhere on the graph: the nearest band follows the finger up and down. */
static void eq_graph_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED) { settings_save(); return; }
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(eq_graph, &a);
    int x = pt.x - a.x1, y = pt.y - a.y1;
    if (code == LV_EVENT_PRESSED)
    {
        int best = 0;
        for (int i = 1; i < EQ_NUM_BANDS; i++)
            if (abs(eq_x(eq_dot_hz(i)) - x) < abs(eq_x(eq_dot_hz(best)) - x)) best = i;
        eq_sel = best;
    }
    float db = EQ_MAX_DB - (y - EQ_GY) * 2.0f * EQ_MAX_DB / EQ_GH;
    eq_set(eq_sel, (int)lroundf(db));
}
static void eq_step_cb(lv_event_t *e)
{
    eq_set(eq_sel, eq_gain(eq_sel) + (int)(intptr_t)lv_event_get_user_data(e));
    settings_save();
}
static void eq_preset_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (eq_parametric()) { eq_profile_save(""); global_settings.sloppy_eq_preamp = 0; }   /* a preset is the octave bands again, at their own level */
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        global_settings.eq_band_settings[i].gain = eq_presets[p].g[i] * 10;
    eq_edited();
    settings_save();
    sl_log("eq preset %s", eq_presets[p].name);
}
static void eq_mine_load(void)
{
    if (eq_mine_read) return;
    eq_mine_read = true;
    eq_n_mine = 0;
    char b[EQ_MINE_MAX * 80];
    int fd = open(EQ_MINE_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got <= 0) return;
    b[got] = '\0';
    for (char *line = b, *next; *line && eq_n_mine < EQ_MINE_MAX; line = next)
    {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = '\0';
        char *tab = strchr(line, '\t');
        if (!tab || tab == line) continue;
        *tab = '\0';
        strlcpy(eq_mine[eq_n_mine].name, line, sizeof eq_mine[0].name);
        char *q = tab + 1;
        int i = 0;
        for (; i < EQ_NUM_BANDS; i++)
        {
            char *end;
            long v = strtol(q, &end, 10);
            if (end == q) break;
            eq_mine[eq_n_mine].g[i] = (signed char)(v > EQ_MAX_DB ? EQ_MAX_DB : v < -EQ_MAX_DB ? -EQ_MAX_DB : v);
            q = end;
        }
        if (i == EQ_NUM_BANDS) eq_n_mine++;
    }
}
static void eq_mine_write(void)
{
    char b[EQ_MINE_MAX * 80];
    int len = 0;
    for (int k = 0; k < eq_n_mine; k++)
    {
        len += snprintf(b + len, sizeof b - len, "%s\t", eq_mine[k].name);
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            len += snprintf(b + len, sizeof b - len, "%d%s", eq_mine[k].g[i], i + 1 < EQ_NUM_BANDS ? " " : "\n");
    }
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(EQ_MINE_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { toast_warn("Could not save the preset. Check the card."); return; }
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(EQ_MINE_FILE ".tmp", EQ_MINE_FILE) < 0) { remove(EQ_MINE_FILE ".tmp"); toast_warn("Could not save the preset. Check the card."); }
}
static void eq_chips_fill(void);
static void eq_mine_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= eq_n_mine) return;
    if (eq_parametric()) { eq_profile_save(""); global_settings.sloppy_eq_preamp = 0; }
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        global_settings.eq_band_settings[i].gain = eq_mine[k].g[i] * 10;
    eq_edited();
    settings_save();
    sl_log("eq preset of your own: %s", eq_mine[k].name);
}
/* held: gone */
static void eq_mine_long_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= eq_n_mine) return;
    char b[128];
    snprintf(b, sizeof b, sl_tr("Removed %s"), eq_mine[k].name);
    memmove(&eq_mine[k], &eq_mine[k + 1], (size_t)(eq_n_mine - k - 1) * sizeof eq_mine[0]);
    eq_n_mine--;
    eq_mine_write();
    eq_chips_fill();
    eq_refresh();
    toast(b);
}
static void eq_name_deleted(lv_event_t *e) { (void)e; eq_name_ta = NULL; }
static void eq_save_go_cb(lv_event_t *e)
{
    (void)e;
    if (!eq_name_ta) return;
    char name[24];
    strlcpy(name, sl_ta_text(eq_name_ta), sizeof name);
    pl_utf8_trim(name);
    for (char *c = name; *c; c++) if (*c == '\t' || *c == '\n') *c = ' ';
    if (!name[0]) return;
    int k = 0;
    while (k < eq_n_mine && strcasecmp(eq_mine[k].name, name)) k++;   /* the same name: replaced */
    if (k == eq_n_mine)
    {
        if (eq_n_mine == EQ_MINE_MAX) { toast_warn("Eight presets saved. Hold one to remove it."); return; }
        eq_n_mine++;
    }
    strlcpy(eq_mine[k].name, name, sizeof eq_mine[k].name);
    for (int i = 0; i < EQ_NUM_BANDS; i++) eq_mine[k].g[i] = (signed char)eq_gain(i);
    eq_mine_write();
    page_pop();
    eq_chips_fill();
    eq_refresh();
    toast("Preset saved");
}
static void eq_save_page(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("Save preset", &p);
    lv_obj_add_event_cb(p, eq_name_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    eq_name_ta = set_field(set_group(body, "NAME"));
    lv_textarea_set_max_length(eq_name_ta, 20);
    wf_kb_build(p, eq_name_ta, KB_NAME, eq_save_go_cb, "Save");
    page_push(p);
}
static lv_obj_t *eq_chip(lv_obj_t *box_, const char *name, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *c = box(box_);
    lv_obj_set_size(c, (W - 2 * SL_GUTTER - 2 - 2 * SL_GUTTER - 3 * SL_SP_S) / 4, 44);   /* four across in its card */
    lv_obj_set_style_radius(c, SL_R_FULL, 0);
    lv_obj_t *l = text(c, name, &ss_semibold_18, 0xffffff);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    sl_fit(l);                           /* one line in the chip at any text size */
    lv_obj_center(l);
    on_click(c, cb, (void *)ud);
    return c;
}
/* the built-in presets, yours, and a chip to save the curve as one */
static void eq_chips_fill(void)
{
    if (!eq_chips_box) return;
    eq_mine_load();
    lv_obj_clean(eq_chips_box);
    for (int k = 0; k < EQ_PRESETS; k++) eq_chips[k] = eq_chip(eq_chips_box, eq_presets[k].name, eq_preset_cb, k);
    memset(eq_mine_chips, 0, sizeof eq_mine_chips);
    for (int k = 0; k < eq_n_mine; k++)
    {
        eq_mine_chips[k] = eq_chip(eq_chips_box, eq_mine[k].name, eq_mine_cb, k);
        lv_obj_add_event_cb(eq_mine_chips[k], eq_mine_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
    }
    lv_obj_t *save = eq_chip(eq_chips_box, LV_SYMBOL_PLUS " Save", eq_save_page, 0);
    lv_obj_set_style_border_color(save, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(save, 2, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(save, 0), lv_color_hex(sl_accent_on(C_BG, 45)), 0);
}
static void eq_switch_cb(lv_event_t *e)
{
    (void)e;
    global_settings.eq_enabled = lv_obj_has_state(eq_sw, LV_STATE_CHECKED);
    eq_apply_all();
    eq_refresh();
    settings_save();
}
static lv_obj_t *eq_step_button(lv_obj_t *parent, const char *sym, int step)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 64, 64);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(b, 30, 0);
    lv_obj_set_style_bg_opa(b, 70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_center(text(b, sym, &sl_sym_28, 0xffffff));
    on_click(b, eq_step_cb, (void *)(intptr_t)step);
    return b;
}
/* A profile's band, by hand: its frequency a sixth of an octave a step,
 * its width a tenth of Q */
static void eq_pq_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    struct eq_band_setting *b = &global_settings.eq_band_settings[eq_sel];
    if (k == 0 || k == 1)
    {
        int hz = (int)lround(b->cutoff * pow(2.0, (k ? 1 : -1) / 6.0));   /* double: the R1's glibc has no new powf */
        if (hz == b->cutoff) hz += k ? 1 : -1;
        b->cutoff = hz < 20 ? 20 : hz > 20000 ? 20000 : hz;
    }
    else
    {
        /* a tenth at a time while wide, coarser as it narrows toward Q 20 */
        int down = k == 2 && b->q > 1 ? b->q - 1 : b->q, at = k == 3 ? b->q : down;
        int st = at < 20 ? 1 : at < 50 ? 2 : at < 100 ? 5 : 10;
        int q = b->q + (k == 3 ? st : -st);
        b->q = q < 1 ? 1 : q > EQ_Q_TYPED_MAX ? EQ_Q_TYPED_MAX : q;
    }
    eq_edited();
    settings_save();
}
static lv_obj_t *eq_pq_pair(lv_obj_t *row, const char *label, int k, lv_align_t align)
{
    lv_obj_t *g = box(row);
    lv_obj_set_size(g, (W - 2 * SL_GUTTER - 2 - 2 * SL_GUTTER - SL_SP_S) / 2, 48);   /* two across in the bands' card */
    lv_obj_align(g, align, 0, 0);
    lv_obj_set_style_radius(g, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(g, 30, 0);
    lv_obj_center(text(g, label, &ss_semibold_18, 0xffffff));
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *b = box(g);
        lv_obj_set_size(b, 48, 48);
        lv_obj_align(b, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, 50, LV_STATE_PRESSED);
        press_motion(b);
        lv_obj_center(text(b, i ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, &sl_sym_18, 0xffffff));
        on_click(b, eq_pq_cb, (void *)(intptr_t)(k + i));
    }
    return g;
}
static void eq_page_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != eq_page_obj) return;
    eq_page_obj = NULL;
    eq_prof_row = NULL;
    eq_sw = NULL;                           /* a profile fetched after the page closed must not touch it */
    eq_chips_box = eq_pq_row = eq_presets_head = NULL;
    eq_pre_row = eq_level_line = NULL;
    eq_steps[0] = eq_steps[1] = NULL;
    memset(eq_labels, 0, sizeof eq_labels);
    memset(eq_ticks, 0, sizeof eq_ticks);
}

/* "-3.45" as tenths, rounded: -35. A number AutoEq wrote, nothing more. */
static int eq_tenths(const char *s)
{
    int sign = 1, v = 0, frac = 0, digits = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',')
        for (s++; *s >= '0' && *s <= '9'; s++)
            if (digits++ < 2) frac = frac * 10 + (*s - '0');
    if (digits == 1) frac *= 10;
    return sign * (v * 10 + (frac + 5) / 10);
}
/* ---- PEQ files: AutoEq's ParametricEQ.txt, Equalizer APO's config and
 * the HiBy Music app's JSON preset, read into one list of filters, then
 * placed on the ten bands, any filter on any band. Past ten, the quietest
 * are left out. */
#define EQ_IMPORT_MAX 32
struct eq_filter { int type, hz, gain, q; };     /* an EQ_TYPE_*; gain and Q in tenths */
struct eq_import
{
    struct eq_filter f[EQ_IMPORT_MAX];
    int n, unsupported, eased;              /* filters read; band pass, notch and the like; Q over 20 */
    int preamp;                             /* tenths */
    bool has_preamp, graphic_only;
    char name[64];
};
/* "-3.45" or "1000.0" as a whole number of the unit (Hz) */
static int eq_whole(const char *s) { return (int)lround(strtod(s, NULL)); }
static bool eq_num_at(const char *s) { while (*s == ' ' || *s == '\t') s++; return (*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.'; }
/* Q from an octave bandwidth, as APO's "BW Oct 1.0" */
static int eq_q_from_bw(double oct) { double p = pow(2.0, oct); return oct > 0 ? (int)lround(10.0 * sqrt(p) / (p - 1)) : 0; }
static int eq_kind_of(const char *t, size_t n)
{
    char k[16];
    if (n >= sizeof k) n = sizeof k - 1;
    for (size_t i = 0; i < n; i++) k[i] = (char)((t[i] >= 'a' && t[i] <= 'z') ? t[i] - 32 : t[i]);
    k[n] = '\0';
    static const char *const peak[] = { "PK", "PEQ", "PEAK", "PEAKING", "BELL", "MODAL", "0" };
    static const char *const low[] = { "LSC", "LS", "LOWSHELF", "LOW_SHELF", "LSQ", "1" };
    static const char *const high[] = { "HSC", "HS", "HIGHSHELF", "HIGH_SHELF", "HSQ", "2" };
    static const char *const lowpass[] = { "LP", "LPQ", "LPF", "LOWPASS", "LOW_PASS" };
    static const char *const highpass[] = { "HP", "HPQ", "HPF", "HIGHPASS", "HIGH_PASS" };
    for (unsigned i = 0; i < ARRAYLEN(peak); i++) if (!strcmp(k, peak[i])) return EQ_TYPE_PEAK;
    for (unsigned i = 0; i < ARRAYLEN(low); i++) if (!strcmp(k, low[i])) return EQ_TYPE_LOW_SHELF;
    for (unsigned i = 0; i < ARRAYLEN(high); i++) if (!strcmp(k, high[i])) return EQ_TYPE_HIGH_SHELF;
    for (unsigned i = 0; i < ARRAYLEN(lowpass); i++) if (!strcmp(k, lowpass[i])) return EQ_TYPE_LOW_PASS;
    for (unsigned i = 0; i < ARRAYLEN(highpass); i++) if (!strcmp(k, highpass[i])) return EQ_TYPE_HIGH_PASS;
    return 0;                               /* band pass, notch, all pass: none of the EQ's */
}
static void eq_add_filter(struct eq_import *im, int kind, int hz, int gain, int q)
{
    if (!kind) { im->unsupported++; return; }
    if (im->n >= EQ_IMPORT_MAX || hz <= 0) return;
    if (q <= 0) q = kind == EQ_TYPE_PEAK ? EQ_Q_PEAK : EQ_Q_SHELF;   /* a pass filter without Q: Butterworth's 0.71 */
    if (q > EQ_Q_TYPED_MAX) { q = EQ_Q_TYPED_MAX; im->eased++; }
    struct eq_filter *f = &im->f[im->n++];
    f->type = kind;
    f->hz = hz < 20 ? 20 : hz > 22000 ? 22000 : hz;
    f->gain = eq_kind_is_pass(kind) ? 0 :
              gain < -EQ_MAX_DB * 10 ? -EQ_MAX_DB * 10 : gain > EQ_MAX_DB * 10 ? EQ_MAX_DB * 10 : gain;
    f->q = q;
}
/* AutoEq and APO: "Preamp: -6.4 dB" and "Filter 3: ON PK Fc 1300 Hz Gain 2.8 dB Q 1.41" */
static void eq_parse_line(struct eq_import *im, char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!strncasecmp(line, "Preamp:", 7))
    {
        if (eq_num_at(line + 7)) { im->preamp = eq_tenths(line + 7); im->has_preamp = true; }
        return;
    }
    if (!strncasecmp(line, "GraphicEQ:", 10)) { im->graphic_only = true; return; }
    if (strncasecmp(line, "Filter", 6)) return;
    char *t = strchr(line, ':');
    if (!t) return;
    char *tok[24];
    int nt = 0;
    for (char *q = strtok(t + 1, " \t"); q && nt < 24; q = strtok(NULL, " \t")) tok[nt++] = q;
    if (nt < 2 || strcasecmp(tok[0], "ON")) return;        /* OFF: not wanted, not a failure */
    int kind = eq_kind_of(tok[1], strlen(tok[1]));
    int hz = 0, gain = 0, q = 0;
    for (int i = 2; i + 1 < nt; i++)
    {
        if (!strcasecmp(tok[i], "Fc")) hz = eq_whole(tok[i + 1]);
        else if (!strcasecmp(tok[i], "Gain")) gain = eq_tenths(tok[i + 1]);
        else if (!strcasecmp(tok[i], "Q")) q = eq_tenths(tok[i + 1]);
        else if (!strcasecmp(tok[i], "BW") && i + 2 < nt && !strcasecmp(tok[i + 1], "Oct")) q = eq_q_from_bw(strtod(tok[i + 2], NULL));
    }
    if (!kind) { im->unsupported++; return; }
    if (hz > 0) eq_add_filter(im, kind, hz, gain, q);
}
/* The HiBy Music app's preset: { "name": ..., "pre_amp": -3.5, "bands": [ { "filter": "PK", "on": true,
 * "freq": 1000, "gain": -2.5, "Q": 1.41 }, ... ] }. Returns why not, or NULL. */
static const char *eq_parse_json(struct eq_import *im, const char *text, const char *end)
{
    const char *bands = lyr_json_at(text, end, "bands");
    if (!bands || *bands != '[') return "That JSON has no EQ bands in it";
    const char *p = bands + 1, *close = NULL;
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
        if (p < end && *p == ']') { close = p; break; }
        if (p >= end || *p != '{') return "That file isn't a HiBy Music EQ preset";
        const char *o = p;
        int depth = 0;
        bool str = false;
        for (; p < end; p++)
        {
            if (str) { if (*p == '\\') p++; else if (*p == '"') str = false; continue; }
            if (*p == '"') str = true;
            else if (*p == '{') depth++;
            else if (*p == '}' && --depth == 0) break;
        }
        if (p >= end) return "That file isn't a HiBy Music EQ preset";
        const char *oe = ++p, *v;
        v = lyr_json_at(o, oe, "on");
        if (!v) v = lyr_json_at(o, oe, "enabled");
        if (v && (*v == 'f' || *v == '0')) continue;
        int kind = 0;
        char k[16];
        if (lyr_json_str(o, oe, "filter", k, sizeof k) || lyr_json_str(o, oe, "type", k, sizeof k)) kind = eq_kind_of(k, strlen(k));
        else if ((v = lyr_json_at(o, oe, "filter")) || (v = lyr_json_at(o, oe, "type")))
            kind = eq_kind_of(v, strspn(v, "0123456789"));
        else kind = EQ_TYPE_PEAK;
        const char *fv = lyr_json_at(o, oe, "freq");
        if (!fv) fv = lyr_json_at(o, oe, "fc");
        if (!fv) fv = lyr_json_at(o, oe, "frequency");
        const char *gv = lyr_json_at(o, oe, "gain"), *qv = lyr_json_at(o, oe, "Q");
        if (!qv) qv = lyr_json_at(o, oe, "q");
        if (!fv || !eq_num_at(fv)) return "A band in that file has no frequency";
        if (!kind) { im->unsupported++; continue; }
        eq_add_filter(im, kind, eq_whole(fv), gv && eq_num_at(gv) ? eq_tenths(gv) : 0, qv && eq_num_at(qv) ? eq_tenths(qv) : 0);
    }
    if (!close) return "That file isn't a HiBy Music EQ preset";
    /* the preset's own name and preamp sit outside its bands */
    const char *pv = lyr_json_at(text, bands, "pre_amp");
    if (!pv) pv = lyr_json_at(close, end, "pre_amp");
    if (!pv) pv = lyr_json_at(text, bands, "preamp");
    if (!pv) pv = lyr_json_at(close, end, "preamp");
    if (pv && eq_num_at(pv)) { im->preamp = eq_tenths(pv); im->has_preamp = true; }
    if (!lyr_json_str(text, bands, "name", im->name, sizeof im->name)) lyr_json_str(close, end, "name", im->name, sizeof im->name);
    return NULL;
}
/* any of the three formats; NULL when filters came out of it */
static const char *eq_parse(struct eq_import *im, char *text, int len)
{
    memset(im, 0, sizeof *im);
    char *p = text, *end = text + len;
    if (len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;      /* a byte-order mark */
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p < end && *p == '{')
    {
        const char *why = eq_parse_json(im, p, end);
        if (why) return why;
    }
    else
        for (char *line = p, *next; line < end && *line; line = next)
        {
            next = line + strcspn(line, "\r\n");
            if (*next) *next++ = '\0';
            if (*line != '#') eq_parse_line(im, line);
        }
    if (im->n) return NULL;
    if (im->graphic_only) return "Equalizer APO's GraphicEQ isn't read. Use its Filter lines";
    if (im->unsupported) return "No filters the EQ has in that file: it has peaks, shelves and low and high pass";
    return "No filters in that file";
}
/* the filters on the bands: returns how many fit (all of them, up to ten) */
static int eq_place(const struct eq_import *im, struct eq_band_setting *b, bool *used)
{
    int order[EQ_IMPORT_MAX], n = 0;
    for (int i = 0; i < im->n; i++) order[n++] = i;
    /* what is heard most first, so what is left out is what is heard
     * least: pass filters (they have no gain, and change the most), then
     * the loudest */
    #define EQ_WEIGHT(f) (eq_kind_is_pass((f)->type) ? 10000 : abs((f)->gain))
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && EQ_WEIGHT(&im->f[order[j]]) > EQ_WEIGHT(&im->f[order[j - 1]]); j--)
        { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    #undef EQ_WEIGHT
    int fit = n < EQ_NUM_BANDS ? n : EQ_NUM_BANDS, nl = 0, nh = 0, np = 0, nx = 0;
    for (int k = 0; k < fit; k++)
    {
        int t = im->f[order[k]].type;
        if (t == EQ_TYPE_LOW_SHELF) nl++;
        else if (t == EQ_TYPE_HIGH_SHELF) nh++;
        else if (t == EQ_TYPE_PEAK) np++;
        else nx++;
    }
    /* low to high across the bands, as the graph reads */
    for (int i = 1; i < fit; i++)
        for (int j = i; j > 0 && im->f[order[j]].hz < im->f[order[j - 1]].hz; j--)
        { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    /* A file the fixed layout holds (a low shelf, a high shelf, eight
     * peaks at most) goes where it always did: the shelves at the ends,
     * the peaks between, untyped, on the filters it had. Anything else:
     * each band its own type, the first and last left free while there's
     * room (Tone's shelves borrow them). */
    bool fixed = nl <= 1 && nh <= 1 && nx == 0 && np <= EQ_NUM_BANDS - 2;
    int at = fixed ? 1 : fit <= EQ_NUM_BANDS - 2 ? 1 : 0;
    for (int k = 0; k < fit; k++)
    {
        const struct eq_filter *f = &im->f[order[k]];
        int band = at;
        if (fixed && f->type == EQ_TYPE_LOW_SHELF) band = 0;
        else if (fixed && f->type == EQ_TYPE_HIGH_SHELF) band = EQ_NUM_BANDS - 1;
        else at++;
        b[band].cutoff = f->hz; b[band].gain = f->gain; b[band].q = f->q;
        b[band].type = fixed ? EQ_TYPE_AUTO : f->type;
        used[band] = true;
    }
    return fit;
}
/* Read a profile from the card and tune the EQ to it. */
static bool eq_autoeq_load(const char *path, const char *name)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { toast_warn("Could not read the profile"); return false; }
    char *text = malloc(32 * 1024);
    int len = 0;
    ssize_t got;
    while (text && len < 32 * 1024 - 1 && (got = read(fd, text + len, 32 * 1024 - 1 - len)) > 0) len += (int)got;
    close(fd);
    if (!text) { toast_warn("Could not read the profile"); return false; }
    text[len] = '\0';
    struct eq_import *im = malloc(sizeof *im);
    const char *why = im ? eq_parse(im, text, len) : "Could not read the profile";
    free(text);
    if (why) { toast_warn(why); free(im); sl_log("eq profile %s: %s", name, why); return false; }
    struct eq_band_setting b[EQ_NUM_BANDS];
    bool used[EQ_NUM_BANDS] = { false };
    int fit = eq_place(im, b, used);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        struct eq_band_setting *s = &global_settings.eq_band_settings[i];
        if (used[i]) *s = b[i];
        else { s->cutoff = eq_cutoffs[i]; s->q = eq_shelf(i) ? EQ_Q_SHELF : EQ_Q_PEAK; s->gain = 0; s->type = EQ_TYPE_AUTO; }
    }
    /* the file's preamp is the EQ's now; auto level still lowers it when a boost would clip */
    int pre = im->has_preamp ? im->preamp : 0;
    global_settings.sloppy_eq_preamp = pre < -150 ? -150 : pre > 60 ? 60 : pre;
    eq_profile_save(im->name[0] ? im->name : name);
    global_settings.eq_enabled = true;
    if (eq_sw) lv_obj_add_state(eq_sw, LV_STATE_CHECKED);
    eq_apply_all();
    settings_save();
    if (eq_pre_row) eq_level_refresh();
    sl_log("eq profile %s: %d of %d filters, %d of no kind the EQ has, preamp %d, %d eased", name, fit, im->n, im->unsupported, pre, im->eased);
    char m[128];
    if (fit < im->n) snprintf(m, sizeof m, sl_tr("%d of %d filters fit the EQ"), fit, im->n);   /* more than ten */
    else if (im->unsupported) strlcpy(m, "Tuned. Band pass, notch and all-pass filters were left out", sizeof m);
    else if (im->eased) strlcpy(m, "Tuned. Filters sharper than Q 20 were widened to 20", sizeof m);
    else strlcpy(m, "Tuned to your headphones", sizeof m);
    free(im);
    toast(m);
    return true;
}
#define EQ_AF_MAX 48
#define EQ_PEQ_DIR "/peq"                   /* where HiBy's own firmware looks */
struct eq_af { char file[80]; bool peq; };
static struct eq_af *eq_af_names;
/* a file's name without " ParametricEQ" and its extension */
static void eq_af_title(const struct eq_af *f, char *name, size_t cap)
{
    strlcpy(name, f->file, cap);
    char *cut = strstr(name, " ParametricEQ");
    if (!cut) cut = strrchr(name, '.');
    if (cut) *cut = '\0';
}
static void eq_af_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    char path[MAX_PATH];
    if (!eq_af_names) return;
    if (k < 0)                              /* back to the octave bands */
    {
        eq_profile_save("");
        global_settings.sloppy_eq_preamp = 0;   /* the profile's preamp went with it */
        eq_apply_all();
        settings_save();
        toast("Graphic EQ");
    }
    else
    {
        snprintf(path, sizeof path, "%s/%s", eq_af_names[k].peq ? EQ_PEQ_DIR : EQ_AUTOEQ_DIR, eq_af_names[k].file);
        char name[64];
        eq_af_title(&eq_af_names[k], name, sizeof name);
        if (!eq_autoeq_load(path, name)) return;
    }
    page_pop();
    if (eq_page_obj) eq_refresh();
}
static void eq_af_deleted(lv_event_t *e) { (void)e; free(eq_af_names); eq_af_names = NULL; }
static int eq_af_cmp(const void *a, const void *b) { return strcasecmp(((const struct eq_af *)a)->file, ((const struct eq_af *)b)->file); }
/* ---- AutoEq by name, over Wi-Fi: AutoEq's own index of every pair it has
 * measured comes once and stays on the card; a search reads it here, and
 * the chosen pair's ParametricEQ.txt joins the others in /AutoEq. */
#define EQ_INDEX_FILE  ROCKBOX_DIR "/sloppy/autoeq-index.md"
#define EQ_INDEX_URL   "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/INDEX.md"
#define EQ_RESULTS_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/"
#define EQ_FOUND_MAX   30
struct eq_found { char name[64], path[192], by[48]; };
static struct eq_found *eq_found;
static int eq_found_n, eq_fetching = -1;
static lv_obj_t *eq_find_ta;

static void eq_lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }
/* the index's "- [Name](./path) by source" lines whose name holds the query */
static bool eq_index_search(const char *query)
{
    int fd = open(EQ_INDEX_FILE, O_RDONLY);
    if (fd < 0) return false;
    free(eq_found);
    eq_found = calloc(EQ_FOUND_MAX, sizeof *eq_found);
    eq_found_n = 0;
    char q[64], line[480], low[96];
    strlcpy(q, query, sizeof q);
    eq_lower(q);
    for (int k = 0; eq_found && eq_found_n < EQ_FOUND_MAX && read_line(fd, line, sizeof line) > 0; k++)
    {
        if (strncmp(line, "- [", 3)) continue;
        char *ne = strstr(line, "]("), *pe = ne ? strstr(ne, ") by ") : NULL;
        if (!ne || !pe || ne - line - 3 >= (int)sizeof low) continue;
        memcpy(low, line + 3, ne - line - 3);
        low[ne - line - 3] = '\0';
        eq_lower(low);
        if (!strstr(low, q)) continue;
        struct eq_found *f = &eq_found[eq_found_n++];
        snprintf(f->name, sizeof f->name, "%.*s", (int)(ne - line - 3), line + 3);
        snprintf(f->path, sizeof f->path, "%.*s", (int)(pe - ne - 4), ne + 4);   /* without "](./" */
        snprintf(f->by, sizeof f->by, "%s", pe + 5);
        if ((k & 255) == 255) yield();
    }
    close(fd);
    return true;
}
/* a name as a URL path takes it */
static void eq_url_part(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 4 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.')
            out[n++] = (char)c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = '\0';
}
static void eq_pop_to_eq(void)
{
    while (npages > 1 && eq_page_obj && pages[npages - 1] != eq_page_obj) page_pop();
}
static void eq_prof_done(bool ok, const char *text, int len)
{
    int k = eq_fetching;
    eq_fetching = -1;
    if (!ok || k < 0 || !eq_found || k >= eq_found_n) { toast_warn("Could not reach AutoEq"); return; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, EQ_AUTOEQ_DIR "/%s ParametricEQ.txt", eq_found[k].name);
    mkdir(EQ_AUTOEQ_DIR);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool saved = fd >= 0 && write(fd, text, len) == len;
    if (fd >= 0) close(fd);
    if (!saved) { toast_warn("Could not save the profile"); return; }
    if (eq_autoeq_load(path, eq_found[k].name)) { eq_pop_to_eq(); if (eq_page_obj) eq_refresh(); }
}
static void eq_found_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || !eq_found || k >= eq_found_n || eq_fetching >= 0) return;
    char file[160], url[sizeof EQ_RESULTS_URL + 512], name[96];
    snprintf(name, sizeof name, "%s ParametricEQ.txt", eq_found[k].name);
    eq_url_part(file, sizeof file, name);
    snprintf(url, sizeof url, EQ_RESULTS_URL "%s/%s", eq_found[k].path, file);
    eq_fetching = k;
    if (!wf_fetch(url, 16 * 1024, eq_prof_done)) { eq_fetching = -1; toast("Wi-Fi is busy. Try again in a moment"); return; }
    toast("Getting the profile");
}
static void eq_found_page(const char *query)
{
    char t[80];
    snprintf(t, sizeof t, "\xe2\x80\x9c%s\xe2\x80\x9d", query);
    lv_obj_t *p, *body = page_new(t, &p);
    char b[64];
    snprintf(b, sizeof b, sl_tr(eq_found_n == 1 ? "%d MATCH" : "%d MATCHES"), eq_found_n);
    lv_obj_t *g = set_group(body, b);
    for (int k = 0; k < eq_found_n; k++)
    {
        char by[64];
        snprintf(by, sizeof by, "%s %s", sl_tr("Measured by"), eq_found[k].by);
        set_action_sub(g, LV_SYMBOL_AUDIO, 0x4f7fe0, eq_found[k].name, by, eq_found_cb, (void *)(intptr_t)k);
    }
    if (!eq_found_n)
    {
        lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
        opt_note(body, "AutoEq has nothing by that name. Try fewer words, or the maker's name.");
    }
    page_push(p);
}
static char eq_find_q[64];
static void eq_index_done(bool ok, const char *text, int len)
{
    if (!ok || !strstr(text, "- [")) { toast_warn("Could not reach AutoEq"); return; }
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(EQ_INDEX_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool saved = fd >= 0 && write(fd, text, len) == len;
    if (fd >= 0) close(fd);
    if (!saved) { toast_warn("Could not save AutoEq's list"); return; }
    if (eq_index_search(eq_find_q)) eq_found_page(eq_find_q);
}
static void eq_find_go_cb(lv_event_t *e)
{
    (void)e;
    if (!eq_find_ta) return;
    strlcpy(eq_find_q, sl_ta_text(eq_find_ta), sizeof eq_find_q);
    pl_utf8_trim(eq_find_q);
    if (!eq_find_q[0]) return;
    if (eq_index_search(eq_find_q)) { eq_found_page(eq_find_q); return; }   /* the list is on the card */
    if (!wf_online()) { toast("Connect to Wi-Fi first"); return; }
    if (!wf_fetch(EQ_INDEX_URL, 1536 * 1024, eq_index_done)) { toast("Wi-Fi is busy. Try again in a moment"); return; }
    toast("Getting AutoEq's list, once");
}
static void eq_find_deleted(lv_event_t *e) { (void)e; eq_find_ta = NULL; }
static void eq_find_page(void)
{
    lv_obj_t *p, *body = page_new("Find headphones", &p);
    lv_obj_add_event_cb(p, eq_find_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    eq_find_ta = set_field(set_group(body, "MAKER AND MODEL"));
    lv_textarea_set_max_length(eq_find_ta, 60);
    sl_ta_hint(eq_find_ta, sl_tr("Sennheiser HD 650"));
    lv_obj_set_style_text_color(eq_find_ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    if (set_rtl()) lv_obj_set_style_text_align(eq_find_ta, LV_TEXT_ALIGN_RIGHT, 0);
    opt_note(body, "AutoEq has measured thousands of headphones. Their list comes over Wi-Fi once, then searches work offline.");
    wf_kb_build(p, eq_find_ta, KB_TEXT, eq_find_go_cb, "Search");
    page_push(p);
}
static void eq_find_open_cb(lv_event_t *e) { (void)e; eq_find_page(); }

/* AutoEq's files on the card, one row a pair of headphones */
static void eq_profiles_page(void)
{
    lv_obj_t *p, *body = page_new("Headphones", &p);
    free(eq_af_names);
    eq_af_names = calloc(EQ_AF_MAX, sizeof *eq_af_names);
    lv_obj_add_event_cb(p, eq_af_deleted, LV_EVENT_DELETE, NULL);
    int n = 0;
    for (int dir = 0; dir < 2 && eq_af_names; dir++)
    {
        DIR *d = opendir(dir ? EQ_PEQ_DIR : EQ_AUTOEQ_DIR);
        struct dirent *ent;
        while (d && n < EQ_AF_MAX && (ent = readdir(d)) != NULL)
        {
            if (ent->d_name[0] == '.' || (!sl_suffix(ent->d_name, ".txt") && !sl_suffix(ent->d_name, ".json"))) continue;
            strlcpy(eq_af_names[n].file, ent->d_name, sizeof eq_af_names[0].file);
            eq_af_names[n++].peq = dir;
        }
        if (d) closedir(d);
    }
    if (n) qsort(eq_af_names, n, sizeof eq_af_names[0], eq_af_cmp);
    if (eq_parametric())
    {
        lv_obj_t *g = set_group(body, "IN USE");
        set_action_sub(g, SL_SYM_EQ, 0x1fb39a, eq_profile, "Tap to go back to the graphic EQ", eq_af_cb, (void *)(intptr_t)-1);
    }
    /* finding one first, then the ones on the card */
    lv_obj_t *g = set_group(body, "Profiles");
    set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x0aa9c7, "Find your headphones", "Thousands of AutoEq profiles, over Wi-Fi", NULL,
                 eq_find_open_cb, NULL);
    for (int k = 0; k < n; k++)
    {
        char name[64];
        eq_af_title(&eq_af_names[k], name, sizeof name);
        set_action_sub(g, LV_SYMBOL_AUDIO, 0x4f7fe0, name,
                       sl_suffix(eq_af_names[k].file, ".json") ? "HiBy Music preset" : "Tune the EQ to these",
                       eq_af_cb, (void *)(intptr_t)k);
    }
    if (!n)
        sl_empty(body, IC_HEADPHONES, 96, "No headphone profiles yet",
                 "Copy a ParametricEQ.txt from AutoEq, an Equalizer APO config or a HiBy Music preset into "
                 "a folder named AutoEq or peq on the card, then come back.", 40);
    page_push(p);
}
/* ---- the preamp: the user's level, from -15 to +6 dB, half a dB a step.
 * Auto level (on unless turned off) takes it lower, never higher, when a
 * boost would clip; the line under it says by how much. */
static void eq_db_text(char *b, int n, int tenths)
{
    int a = tenths < 0 ? -tenths : tenths;
    if (a % 10) snprintf(b, n, "%s%d.%d dB", tenths > 0 ? "+" : "-", a / 10, a % 10);
    else snprintf(b, n, tenths > 0 ? "+%d dB" : tenths < 0 ? "-%d dB" : "%d dB", a / 10);
}
static void eq_level_refresh(void)
{
    if (!eq_pre_row) return;
    char b[160], v[24];
    eq_db_text(v, sizeof v, global_settings.sloppy_eq_preamp);
    label_if_changed(lv_obj_get_child(eq_pre_row, 1), v);
    lv_obj_t *s = lv_obj_get_child(eq_pre_row, 2);
    int want = (global_settings.sloppy_eq_preamp + (global_settings.sloppy_eq_preamp < 0 ? -2 : 2)) / 5;
    if (lv_slider_get_value(s) != want && !lv_obj_has_state(s, LV_STATE_PRESSED)) lv_slider_set_value(s, want, LV_ANIM_OFF);
    strlcpy(b, sl_tr("Nothing boosted can clip"), sizeof b);
    if (global_settings.eq_enabled && global_settings.sloppy_eq_auto_level && eq_auto_cut > 0)
    {
        eq_db_text(v, sizeof v, -eq_applied_precut);
        snprintf(b, sizeof b, sl_tr("Playing at %s, so boosts don't clip"), v);
    }
    else if (global_settings.eq_enabled && !global_settings.sloppy_eq_auto_level && eq_over > 0)
    {
        eq_db_text(v, sizeof v, eq_over);
        snprintf(b, sizeof b, sl_tr("Loud songs can clip: boosts pass the preamp by %s"), v);
    }
    if (eq_level_line) label_if_changed(eq_level_line, b);   /* always there, its room kept: nothing below moves as a band does */
}
static void eq_pre_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_current_target(e)) * 5;
    if (v != global_settings.sloppy_eq_preamp)
    {
        global_settings.sloppy_eq_preamp = v;
        eq_apply_all();
    }
    eq_level_refresh();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void eq_auto_level(int on)
{
    global_settings.sloppy_eq_auto_level = on;
    eq_apply_all();
    eq_level_refresh();
}
static void eq_level_card(lv_obj_t *body)
{
    lv_obj_t *c = set_group(body, "Level");
    char v[24];
    eq_db_text(v, sizeof v, global_settings.sloppy_eq_preamp);
    eq_pre_row = set_slider_row(c, "Preamp", v, C_ACCENT_TXT);
    lv_obj_t *s = lv_obj_get_child(eq_pre_row, 2);
    lv_slider_set_range(s, -30, 12);
    lv_slider_set_mode(s, LV_SLIDER_MODE_SYMMETRICAL);   /* filled from 0 dB, either way */
    lv_obj_add_event_cb(s, eq_pre_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, eq_pre_cb, LV_EVENT_RELEASED, NULL);
    set_switch(c, "Auto level", "Lowers the preamp further whenever a boost would clip",
               global_settings.sloppy_eq_auto_level, eq_auto_level);
    eq_level_line = set_text(c, "", C_SUB);
    {   /* as tall as its longest saying, so a change of words never moves the page */
        static const char *const says[] = { "Nothing boosted can clip", "Playing at %s, so boosts don't clip",
                                             "Loud songs can clip: boosts pass the preamp by %s" };
        int w = W - 2 * SL_GUTTER - 2 - 2 * SL_INSET, h = 0;
        for (unsigned i = 0; i < ARRAYLEN(says); i++)
        {
            char t[160];
            snprintf(t, sizeof t, sl_tr(says[i]), "-12.5 dB");
            int th = set_text_h(t, lv_obj_get_style_text_font(eq_level_line, 0), w);
            if (th > h) h = th;
        }
        lv_obj_set_height(eq_level_line, h);
    }
    eq_level_refresh();
}

/* ---- a band's values, typed: the gain to a tenth of a dB, and with a
 * profile its frequency and width too. The keyboard opens on its digits. */
static lv_obj_t *eq_ty_f, *eq_ty_g, *eq_ty_q, *eq_ty_page_obj, *eq_ty_gain_card;
static int eq_ty_type;                      /* the band page's type, applied with Done */
static void wf_kb_digits(void);
static void wf_kb_target(lv_obj_t *ta);
static void eq_ty_focus(lv_obj_t *ta)
{
    lv_obj_t *all[3] = { eq_ty_f, eq_ty_g, eq_ty_q };
    for (int i = 0; i < 3; i++) if (all[i] && all[i] != ta) lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    wf_kb_target(ta);
}
static void eq_ty_field_cb(lv_event_t *e) { eq_ty_focus(lv_event_get_current_target(e)); }
static void eq_ty_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != eq_ty_page_obj) return;
    eq_ty_page_obj = eq_ty_f = eq_ty_g = eq_ty_q = eq_ty_gain_card = NULL;
}
/* the type chips: a pass filter has no gain, so its field goes */
static void eq_ty_show_gain(void)
{
    if (!eq_ty_gain_card) return;
    bool pass = eq_kind_is_pass(eq_ty_type);
    hidden_if_changed(eq_ty_gain_card, pass);
    lv_obj_t *head = lv_obj_get_sibling(eq_ty_gain_card, -1);   /* its header */
    if (head) hidden_if_changed(head, pass);
    if (pass && eq_ty_f && lv_obj_has_state(eq_ty_g, LV_STATE_FOCUSED)) eq_ty_focus(eq_ty_f);
}
static void eq_ty_type_set(int k)
{
    eq_ty_type = k + EQ_TYPE_PEAK;
    eq_ty_show_gain();
}
static lv_obj_t *eq_ty_field(lv_obj_t *body, const char *head, int tenths, bool whole)
{
    lv_obj_t *ta = set_field(set_group(body, head));
    lv_textarea_set_accepted_chars(ta, "0123456789.,-+");
    lv_textarea_set_max_length(ta, 8);
    char b[16];
    int a = tenths < 0 ? -tenths : tenths;
    if (whole) snprintf(b, sizeof b, "%d", tenths);
    else snprintf(b, sizeof b, "%s%d.%d", tenths < 0 ? "-" : "", a / 10, a % 10);
    lv_textarea_set_text(ta, b);
    if (set_rtl()) lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, eq_ty_field_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}
/* a typed number: false when it is not one */
static bool eq_ty_read(lv_obj_t *ta, int *tenths, bool whole)
{
    const char *t = lv_textarea_get_text(ta);
    if (!eq_num_at(t)) return false;
    char *end;
    char b[16];
    strlcpy(b, t, sizeof b);
    for (char *c = b; *c; c++) if (*c == ',') *c = '.';
    double v = strtod(b, &end);
    while (*end == ' ') end++;
    if (*end || end == b) return false;
    *tenths = (int)lround(whole ? v : v * 10);
    return true;
}
static void eq_ty_done_cb(lv_event_t *e)
{
    (void)e;
    struct eq_band_setting *b = &global_settings.eq_band_settings[eq_sel];
    int g = 0, hz = b->cutoff, q = b->q;
    bool para = eq_parametric(), pass = para && eq_kind_is_pass(eq_ty_type);
    if (!pass && (!eq_ty_read(eq_ty_g, &g, false) || g < -EQ_MAX_DB * 10 || g > EQ_MAX_DB * 10))
    { toast_warn("Gain runs from -12 to +12 dB"); eq_ty_focus(eq_ty_g); return; }
    if (eq_ty_f && (!eq_ty_read(eq_ty_f, &hz, true) || hz < 20 || hz > 20000))
    { toast_warn("Frequency runs from 20 to 20000 Hz"); eq_ty_focus(eq_ty_f); return; }
    if (eq_ty_q && (!eq_ty_read(eq_ty_q, &q, false) || q < 1 || q > EQ_Q_TYPED_MAX))
    { toast_warn("Width runs from Q 0.1 to 20"); eq_ty_focus(eq_ty_q); return; }
    b->gain = g;
    if (para)
    {
        b->cutoff = hz; b->q = q;
        /* the fixed layout's own kind stays the fixed layout (and its old filter) */
        if (b->type != EQ_TYPE_AUTO || eq_ty_type != eq_band_kind(eq_sel, b)) b->type = eq_ty_type;
    }
    page_pop();
    eq_edited();
    settings_save();
}
static void eq_band_page(lv_event_t *e)
{
    (void)e;
    char t[48];
    bool para = eq_parametric();
    if (para) snprintf(t, sizeof t, sl_tr("Band %d"), eq_sel + 1);
    else snprintf(t, sizeof t, sl_tr("%s Hz band"), eq_names[eq_sel]);
    lv_obj_t *p, *body = page_new(t, &p);
    eq_ty_page_obj = p;
    lv_obj_add_event_cb(p, eq_ty_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_bottom(body, WF_KB_H + SL_GUTTER, 0);   /* everything scrolls clear of the keyboard */
    const struct eq_band_setting *b = &global_settings.eq_band_settings[eq_sel];
    eq_ty_f = eq_ty_q = NULL;
    eq_ty_type = eq_kind(eq_sel);
    eq_ty_g = eq_ty_field(body, "Gain", b->gain, false);
    eq_ty_gain_card = lv_obj_get_parent(eq_ty_g);
    if (para)
    {
        eq_ty_f = eq_ty_field(body, "Frequency", b->cutoff, true);
        eq_ty_q = eq_ty_field(body, "Width", b->q, false);
        /* a profile's band can be any filter; the chips under the fields,
         * which stay clear of the keyboard */
        static const char *const kinds[] = { "Peak", "Low shelf", "High shelf", "Low pass", "High pass" };
        opt_chips(set_group(body, "Type"), kinds, 5, eq_ty_type - EQ_TYPE_PEAK, eq_ty_type_set);
    }
    opt_note(body, para ? "Gain -12 to +12 dB, frequency 20 to 20000 Hz, Q 0.1 to 20, a tenth at a time. Low and high pass filters have no gain."
                        : "From -12 to +12 dB, a tenth at a time.");
    wf_kb_build(p, eq_ty_g, KB_TEXT, eq_ty_done_cb, "Done");
    wf_kb_digits();
    eq_ty_focus(eq_ty_g);
    eq_ty_show_gain();
    page_push(p);
}
static void eq_page(void)
{
    if (page_bring_back(eq_page_obj)) return;
    lv_obj_t *p, *body = page_new("Equalizer", &p);
    eq_page_obj = p;
    lv_obj_add_event_cb(p, eq_page_deleted, LV_EVENT_DELETE, NULL);
    /* it scrolls, so its last row clears the mini player; a drag on the
     * graph moves a band, never the page (no scroll chain from it). The
     * switch leads the page in its own card, as Wi-Fi's and Bluetooth's */
    eq_sw = set_switch_ex(set_group(body, NULL), SL_SYM_EQ, 0x1fb39a, "Equalizer", NULL, global_settings.eq_enabled, NULL);
    lv_obj_add_event_cb(eq_sw, eq_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* the response graph and the chosen band, one card */
    lv_obj_t *bands = set_group(body, "Bands");
    eq_graph = box(bands);
    lv_obj_set_size(eq_graph, lv_pct(100), 340);
    lv_obj_add_flag(eq_graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eq_graph, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(eq_graph, eq_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(eq_graph, eq_graph_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(eq_graph, eq_graph_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(eq_graph, eq_graph_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_update_layout(eq_graph);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        lv_obj_t *l = eq_labels[i] = text(eq_graph, eq_names[i], &ss_regular_16, C_DIM);
        lv_obj_update_layout(l);
        lv_obj_set_pos(l, eq_x(eq_centres[i]) - lv_obj_get_width(l) / 2, EQ_GY + EQ_GH + 10);
        eq_dots[i] = box(eq_graph);
        lv_obj_set_style_bg_opa(eq_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(eq_dots[i], lv_color_hex(C_ACCENT), 0);
    }
    /* a profile's bands wander off the octaves: Hz along the bottom instead */
    static const int tick_hz[10] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    static const char *const tick_s[10] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
    for (int i = 0; i < 10; i++)
    {
        lv_obj_t *l = eq_ticks[i] = text(eq_graph, tick_s[i], &ss_regular_16, C_DIM);
        int tw = lv_text_get_width(tick_s[i], strlen(tick_s[i]), lv_obj_get_style_text_font(l, 0), 0);   /* no layout pass a label */
        int x = eq_x(tick_hz[i]) - tw / 2, gw = lv_obj_get_width(eq_graph);
        if (x < 2) x = 2;
        if (x + tw > gw - 2) x = gw - 2 - tw;
        lv_obj_set_pos(l, x, EQ_GY + EQ_GH + 10);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);   /* shown with a profile (eq_refresh) */
    }
    lv_obj_t *db = text(eq_graph, "+12", &ss_regular_16, C_DIM);
    lv_obj_set_pos(db, EQ_GX + 2, EQ_GY - 2);
    db = text(eq_graph, "-12", &ss_regular_16, C_DIM);
    lv_obj_set_pos(db, EQ_GX + 2, EQ_GY + EQ_GH - 20);

    /* the chosen band, and a fine step either way */
    int rd_h = sl_line_h(&ss_bold_28) + sl_line_h(&ss_regular_18) + 2 * SL_SP_XS;   /* the readout grows with Text size */
    lv_obj_t *row = box(bands);
    lv_obj_set_size(row, lv_pct(100), LV_MAX(84, rd_h + SL_SP_S) + SL_SP_S);
    lv_obj_set_style_pad_bottom(row, SL_SP_S, 0);
    lv_obj_align(eq_steps[0] = eq_step_button(row, LV_SYMBOL_MINUS, -1), LV_ALIGN_LEFT_MID, SL_GUTTER, 0);
    lv_obj_align(eq_steps[1] = eq_step_button(row, LV_SYMBOL_PLUS, 1), LV_ALIGN_RIGHT_MID, -SL_GUTTER, 0);
    /* the readout itself is a button: a tap types the band's values */
    lv_obj_t *rd = box(row);
    lv_obj_set_size(rd, W - 2 * SL_GUTTER - 2 - 2 * (SL_GUTTER + 64 + SL_SP_S), rd_h);
    lv_obj_align(rd, LV_ALIGN_CENTER, 0, -SL_SP_S / 2);
    lv_obj_set_style_radius(rd, SL_R_MD, 0);
    lv_obj_set_style_bg_color(rd, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(rd, 30, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(rd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rd, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    press_motion(rd);
    on_click(rd, eq_band_page, NULL);
    lv_obj_t *gl = box(rd);                 /* the gain, and a pencil: it can be typed */
    lv_obj_set_size(gl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gl, SL_SP_S, 0);
    lv_obj_set_style_base_dir(gl, LV_BASE_DIR_LTR, 0);
    lv_obj_remove_flag(gl, LV_OBJ_FLAG_CLICKABLE);
    eq_read_g = text(gl, "", &ss_bold_28, 0xffffff);
    text(gl, LV_SYMBOL_EDIT, &sl_sym_16, C_DIM);
    eq_read_f = text(rd, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(eq_read_f, lv_pct(100));  /* a long kind ("Höhen-Kuhschwanz") runs past: it scrolls */
    lv_obj_set_style_text_align(eq_read_f, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(eq_read_f, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* with a profile, its chosen band's frequency and width, under its gain */
    eq_pq_row = box(bands);
    lv_obj_set_size(eq_pq_row, lv_pct(100), 48 + SL_GUTTER);
    lv_obj_set_style_pad_hor(eq_pq_row, SL_GUTTER, 0);
    lv_obj_set_style_pad_bottom(eq_pq_row, SL_GUTTER, 0);
    /* presets, four to a row, in their own card (gone with a profile) */
    eq_presets_head = set_header(body, "Presets");
    lv_obj_t *chips = eq_chips_box = box(set_group(body, NULL));
    lv_obj_set_style_margin_top(lv_obj_get_parent(chips), 0, 0);
    lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(chips, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_pad_all(chips, SL_GUTTER, 0);
    lv_obj_set_style_pad_column(chips, SL_SP_S, 0);
    lv_obj_set_style_pad_row(chips, SL_SP_S, 0);
    eq_chips_fill();
    eq_pq_pair(eq_pq_row, "Frequency", 0, LV_ALIGN_LEFT_MID);
    eq_pq_pair(eq_pq_row, "Width", 2, LV_ALIGN_RIGHT_MID);
    eq_level_card(body);
    /* a pair of headphones' own curve, from AutoEq files on the card */
    lv_obj_t *pg = set_group(body, "AutoEq");
    eq_prof_row = set_link_sub(pg, LV_SYMBOL_AUDIO, 0x4f7fe0, "Headphone profile", "", NULL, card_row_cb, (void *)(intptr_t)0x2001c);
    eq_refresh();
    page_push(p);
}

/* ------------------------------------------------------------ tone tuning */
/* HiBy's MSEB, made from what Rockbox's DSP has: Temperature tilts the tone
 * controls' 200 Hz and 3.5 kHz shelves against each other; Bass texture is
 * perceptual bass enhancement; the rest are EQ filters. With the octave
 * bands they add to those bands' gains; with a headphone profile each takes
 * a band the profile leaves flat, or says there is none. Nothing reacts to
 * the music (no de-esser, no transient shaper), and the page says so. */
enum { TN_TEMP, TN_DEEP, TN_TEXTURE, TN_THICK, TN_VOCAL, TN_SIB, TN_AIR, TN_N };
struct tone_spec
{
    const char *name, *lo, *hi;
    int band;                               /* the EQ band kind it needs: 0 low shelf, 1 a peak, 9 high shelf; -1 none */
    int hz, q;                              /* its filter with a profile in */
    signed char oct[EQ_NUM_BANDS];          /* tenths of a dB a step on each octave band */
};
static const struct tone_spec tone_specs[TN_N] = {
    { "Temperature", "Warmer", "Brighter", -1, 0, 0, { 0 } },
    { "Deep bass", "Lighter", "Deeper", 0, 45, 7, { 10, 3 } },
    { "Bass texture", "Clean", "Textured", -1, 0, 0, { 0 } },
    { "Note thickness", "Leaner", "Thicker", 1, 300, 8, { 0, 0, 3, 10, 5 } },
    { "Vocals", "Laid back", "Forward", 1, 3200, 12, { 0, 0, 0, 0, 0, 0, 5, 10 } },
    { "Sibilance", "Smoother", "Crisper", 1, 7000, 25, { 0, 0, 0, 0, 0, 0, 0, 0, 10 } },
    { "Air", "Softer", "Airier", 9, 10000, 7, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 10 } },
};
static int *tone_var(int k)
{
    switch (k)
    {
        case TN_TEMP: return &global_settings.sloppy_tone_temp;
        case TN_DEEP: return &global_settings.sloppy_tone_deep;
        case TN_THICK: return &global_settings.sloppy_tone_thick;
        case TN_VOCAL: return &global_settings.sloppy_tone_vocal;
        case TN_SIB: return &global_settings.sloppy_tone_sib;
        case TN_AIR: return &global_settings.sloppy_tone_air;
    }
    return NULL;
}
static int tone_value(int k)
{
    if (k == TN_TEXTURE) return (global_settings.pbe + 12) / 25;   /* Rockbox's PBE strength, in quarters */
    int v = *tone_var(k);
    return v < -6 ? -6 : v > 6 ? 6 : v;
}
/* which band each EQ slider has: -1 when a profile leaves none free */
static void tone_where(int where[TN_N])
{
    bool para = eq_parametric(), free_[EQ_NUM_BANDS];
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        free_[i] = !para || !global_settings.eq_enabled || !eq_band_active(i, &global_settings.eq_band_settings[i]);
    for (int k = 0; k < TN_N; k++)
    {
        int want = tone_specs[k].band;
        where[k] = -1;
        if (want < 0) continue;
        if (!para) { where[k] = want; continue; }
        if (want != 1) { if (free_[want]) { where[k] = want; free_[want] = false; } continue; }
        for (int i = 1; i < EQ_NUM_BANDS - 1; i++)
            if (free_[i]) { where[k] = i; free_[i] = false; break; }
    }
}
static void tone_eq_bands(struct eq_band_setting *t, bool para)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        t[i].gain = 0;
        t[i].cutoff = eq_cutoffs[i];
        t[i].q = eq_shelf(i) ? EQ_Q_SHELF : EQ_Q_PEAK;
        t[i].type = EQ_TYPE_AUTO;           /* Tone's filter is its band's place: a shelf at either end */
    }
    int where[TN_N];
    tone_where(where);
    for (int k = 0; k < TN_N; k++)
    {
        int v = tone_specs[k].band >= 0 ? tone_value(k) : 0;
        if (!v || where[k] < 0) continue;
        if (!para)
            for (int i = 0; i < EQ_NUM_BANDS; i++) t[i].gain += v * tone_specs[k].oct[i];
        else
        {
            struct eq_band_setting *b = &t[where[k]];
            b->cutoff = tone_specs[k].hz; b->q = tone_specs[k].q; b->gain = v * 10;
        }
    }
}
/* Temperature on the tone controls, on top of Bass and Treble */
static void tone_shelves_apply(void)
{
    int t = tone_value(TN_TEMP);
#ifdef AUDIOHW_HAVE_BASS
    int b = global_settings.bass - t;
    sound_set(SOUND_BASS, b < sound_min(SOUND_BASS) ? sound_min(SOUND_BASS) : b > sound_max(SOUND_BASS) ? sound_max(SOUND_BASS) : b);
#endif
#ifdef AUDIOHW_HAVE_TREBLE
    int tr = global_settings.treble + t;
    sound_set(SOUND_TREBLE, tr < sound_min(SOUND_TREBLE) ? sound_min(SOUND_TREBLE) : tr > sound_max(SOUND_TREBLE) ? sound_max(SOUND_TREBLE) : tr);
#endif
    (void)t;
}
/* everything Tone does, as the player starts and after a change */
static void tone_apply_all(void)
{
    tone_shelves_apply();
    dsp_pbe_enable(global_settings.pbe);
    eq_apply_all();
}
static bool tone_any(void)
{
    for (int k = 0; k < TN_N; k++) if (tone_value(k)) return true;
    return false;
}
static const char *tone_row_text(void) { return tone_any() ? "On" : "Off"; }

/* the shape Tone adds, drawn: first-order shelves for Temperature as the
 * tone controls make them, the EQ filters as eq.c does */
static float tone_response(const struct eq_band_setting *t, float f)
{
    float db = eq_response_of(t, false, f), tl = (float)tone_value(TN_TEMP);
    if (tl)
    {
        float gl = (float)pow(10.0, -tl / 20.0), gh = (float)pow(10.0, tl / 20.0), w = f * f;
        db += 10.0f * log10f((w + gl * gl * 200.0f * 200.0f) / (w + 200.0f * 200.0f));
        db += 10.0f * log10f((gh * gh * w + 3500.0f * 3500.0f) / (w + 3500.0f * 3500.0f));
    }
    return db;
}
static lv_obj_t *tone_page_obj, *tone_graph, *tone_rows[TN_N], *tone_snd_row;
#define TONE_MAX_DB 8                       /* Tone moves a few dB: drawn large enough to see */
static void tone_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(tone_graph, &a);
    int gx = SL_INSET, gw = lv_area_get_width(&a) - 2 * SL_INSET, gy = SL_GUTTER, gh = lv_area_get_height(&a) - 2 * SL_GUTTER;
    int x0 = a.x1 + gx, x1 = x0 + gw;
    #define TONE_Y(db) (a.y1 + gy + (int)(gh * (TONE_MAX_DB - ((db) > TONE_MAX_DB ? TONE_MAX_DB : (db) < -TONE_MAX_DB ? -TONE_MAX_DB : (db))) / (2.0f * TONE_MAX_DB)))
    for (int db = -TONE_MAX_DB; db <= TONE_MAX_DB; db += 4)
        wallpaper_line(layer, x0, TONE_Y(db), x1, TONE_Y(db), C_TEXT, db ? 22 : 60, 1);
    for (int d = 100; d <= 10000; d *= 10)
    {
        int x = x0 + (int)(gw * log10f(d / 20.0f) / 3.0f);
        wallpaper_line(layer, x, a.y1 + gy, x, a.y1 + gy + gh, C_TEXT, 14, 1);
    }
    struct eq_band_setting t[EQ_NUM_BANDS];
    tone_eq_bands(t, eq_parametric());
    int zero = TONE_Y(0.0f), px = -1, py = 0;
    for (int x = x0; x <= x1; x += 3)
    {
        float f = 20.0f * (float)pow(10.0, 3.0f * (x - x0) / (float)gw);
        int y = TONE_Y(tone_response(t, f));
        if (y != zero) wallpaper_rect(layer, x - 1, y < zero ? y : zero, x + 1, y < zero ? zero : y, C_ACCENT, 46, 0);
        if (px >= 0) wallpaper_line(layer, px, py, x, y, C_ACCENT, LV_OPA_COVER, 3);
        px = x; py = y;
    }
    #undef TONE_Y
}
static void tone_value_text(int k, int v, char *b, int n)
{
    if (k == TN_TEMP)
    {
        if (!v) strlcpy(b, sl_tr("Neutral"), n);
        else snprintf(b, n, "%s %d", sl_tr(v < 0 ? "Warmer" : "Brighter"), v < 0 ? -v : v);
    }
    else if (k == TN_TEXTURE)
    {
        if (!v) strlcpy(b, sl_tr("Off"), n);
        else snprintf(b, n, "%d%%", v * 25);
    }
    else eq_db_text(b, n, v * 10);
}
static void tone_row_sync(int k)
{
    lv_obj_t *r = tone_rows[k];
    if (!r) return;
    int where[TN_N];
    tone_where(where);
    char b[40];
    bool none = tone_specs[k].band >= 0 && where[k] < 0;
    if (none) strlcpy(b, sl_tr("No free band"), sizeof b);
    else tone_value_text(k, tone_value(k), b, sizeof b);
    label_if_changed(lv_obj_get_child(r, 1), b);
    lv_obj_t *s = lv_obj_get_child(r, 2);
    if (lv_slider_get_value(s) != tone_value(k)) lv_slider_set_value(s, tone_value(k), LV_ANIM_OFF);
    if (none) lv_obj_add_state(s, LV_STATE_DISABLED);
    else lv_obj_remove_state(s, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(r, 1), lv_color_hex(none ? C_SUB : C_ACCENT_TXT), 0);
}
static void tone_changed(int k)
{
    if (k == TN_TEMP) tone_shelves_apply();
    if (k == TN_TEXTURE) dsp_pbe_enable(global_settings.pbe);
    eq_apply_all();
    tone_row_sync(k);
    if (tone_graph) lv_obj_invalidate(tone_graph);
}
static void tone_slider_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e), v = lv_slider_get_value(lv_event_get_current_target(e));
    if (v != tone_value(k))
    {
        if (k == TN_TEXTURE) global_settings.pbe = v * 25;
        else *tone_var(k) = v;
        tone_changed(k);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && settings_save() != 0) toast_warn("Could not save settings");
}
/* a slider with what each end does under it, from its centre */
static void tone_slider(lv_obj_t *c, int k)
{
    char b[40];
    tone_value_text(k, tone_value(k), b, sizeof b);
    lv_obj_t *r = tone_rows[k] = set_slider_row(c, tone_specs[k].name, b, C_ACCENT_TXT);
    lv_obj_t *s = lv_obj_get_child(r, 2);
    if (k == TN_TEXTURE) lv_slider_set_range(s, 0, 4);
    else { lv_slider_set_range(s, -6, 6); lv_slider_set_mode(s, LV_SLIDER_MODE_SYMMETRICAL); }
    lv_slider_set_value(s, tone_value(k), LV_ANIM_OFF);
    lv_obj_add_event_cb(s, tone_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
    lv_obj_add_event_cb(s, tone_slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)k);
    lv_obj_set_style_bg_opa(s, LV_OPA_40, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(s, LV_OPA_40, LV_PART_INDICATOR | LV_STATE_DISABLED);
    /* the ends, under the track's ends */
    int lh = sl_line_h(&ss_regular_16), y = lv_obj_get_style_y(s, 0) - (SL_KNOB - SL_TRACK_H) / 2 + SL_KNOB + SL_SP_XS;
    lv_obj_set_height(r, y + lh + SL_GUTTER - SL_SP_XS);
    int w = ((W - 2 * SL_GUTTER - 2) - 2 * SL_INSET - SL_SP_S) / 2;
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = text(r, i ? tone_specs[k].hi : tone_specs[k].lo, &ss_regular_16, C_SUB);
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, i ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(l, i ? (W - 2 * SL_GUTTER - 2) - SL_INSET - w : SL_INSET, y);
        sl_label_deep(l);
    }
    tone_row_sync(k);
}
static void tone_flat_cb(lv_event_t *e)
{
    (void)e;
    for (int k = 0; k < TN_N; k++)
    {
        if (k == TN_TEXTURE) global_settings.pbe = 0;
        else *tone_var(k) = 0;
    }
    tone_apply_all();
    for (int k = 0; k < TN_N; k++) tone_row_sync(k);
    if (tone_graph) lv_obj_invalidate(tone_graph);
    settings_save();
    toast("Tone is flat");
}
static void tone_page_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != tone_page_obj) return;
    tone_page_obj = tone_graph = NULL;
    memset(tone_rows, 0, sizeof tone_rows);
    if (tone_snd_row) label_if_changed(lv_obj_get_child(tone_snd_row, 1), sl_tr(tone_row_text()));
}
static void tone_page(void)
{
    if (page_bring_back(tone_page_obj)) return;
    lv_obj_t *p, *body = page_new("Tone tuning", &p);
    tone_page_obj = p;
    lv_obj_add_event_cb(p, tone_page_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, "Shape");
    tone_graph = box(g);
    lv_obj_set_size(tone_graph, lv_pct(100), 150);
    lv_obj_add_event_cb(tone_graph, tone_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    g = opt_card(body, "Warmth and bass", NULL);
    tone_slider(g, TN_TEMP);
    tone_slider(g, TN_DEEP);
    tone_slider(g, TN_TEXTURE);
    opt_note(body, "Temperature tilts the tone controls, so it adds to Bass and Treble on the Sound page. "
                   "Bass texture is Rockbox's bass enhancement, an approximation of HiBy's.");
    g = opt_card(body, "Mids and highs", NULL);
    tone_slider(g, TN_THICK);
    tone_slider(g, TN_VOCAL);
    tone_slider(g, TN_SIB);
    tone_slider(g, TN_AIR);
    int where[TN_N];
    tone_where(where);
    bool short_ = false;
    for (int k = 0; k < TN_N; k++) if (tone_specs[k].band >= 0 && where[k] < 0) short_ = true;
    if (short_)
    {
        char b[200];
        snprintf(b, sizeof b, sl_tr("%s uses the EQ bands these need. A profile with fewer filters leaves room."), eq_profile);
        opt_note(body, b);
    }
    opt_note(body, "Made with the EQ, so these are static: a real de-esser follows the music, Sibilance does not.");
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_REFRESH, 0xf0602f, "Back to flat", NULL, tone_flat_cb, NULL);
    opt_note(body, "Close to HiBy's MSEB: Temperature, Deep bass and Air. Approximate: Bass texture, Note thickness, "
                   "Vocals and Sibilance. MSEB's Impact has no match here and is left out.");
    page_push(p);
}

/* ------------------------------------------------------- play settings */
static void play_order_card(lv_obj_t *body);
static void play_book_rewind(int on) { global_settings.sloppy_book_rewind = on; }
static void play_settings_page(void)
{
    /* once on the stack (Now Playing's sheet over Settings > Playback): a
     * second copy took the crossfade graph and left the first one's dead */
    if (page_bring_back(xf_graph_owner)) return;
    lv_obj_t *p, *body = page_new("Playback", &p);
    if (xf_graph_timer) { lv_timer_delete(xf_graph_timer); xf_graph_timer = NULL; }
    xf_graph_owner = p;
    lv_obj_add_event_cb(p, xf_graph_deleted, LV_EVENT_DELETE, NULL);
    /* crossfade: a Settings group whose first row is the picture of it */
    lv_obj_t *xg = set_group(body, "Crossfade");
    lv_obj_t *c = box(xg);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, SL_INSET, 0);
    lv_obj_set_style_pad_top(c, SL_GUTTER, 0);
    lv_obj_set_style_pad_bottom(c, SL_SP_S, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_t *top = box(c);
    lv_obj_set_size(top, lv_pct(100), LV_MAX(40, sl_line_h(&ss_semibold_21)));
    lv_obj_align(text(top, "Overlap", &ss_semibold_21, 0xffffff), set_rtl() ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    /* the value at the far end in the accent, as every slider row's */
    xf_val = text(top, "", &ss_semibold_18, C_ACCENT_TXT);
    lv_obj_align(xf_val, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    xf_desc = text(c, "", &ss_regular_16, C_SUB);   /* a row's line, as set_switch's */
    lv_obj_set_width(xf_desc, lv_pct(100));
    lv_label_set_long_mode(xf_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_bottom(xf_desc, 8, 0);
    xf_graph = box(c);
    lv_obj_set_size(xf_graph, lv_pct(100), XF_GRAPH_H);
    lv_obj_add_event_cb(xf_graph, xf_graph_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *legend = box(c);
    lv_obj_set_size(legend, lv_pct(100), LV_MAX(26, sl_line_h(&ss_regular_16)));
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 8, 0);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *dot = box(legend);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(k ? xf_incoming_color() : C_ACCENT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_t *lt = text(legend, k ? "Next song" : "Current song", &ss_regular_16, C_SUB);
        if (!k) lv_obj_set_style_margin_right(lt, 14, 0);
    }
    xf_slider = lv_slider_create(c);
    lv_obj_set_size(xf_slider, lv_pct(100), SL_TRACK_H);
    lv_obj_set_style_margin_top(xf_slider, 16, 0);
    lv_obj_set_style_margin_bottom(xf_slider, 6, 0);
    lv_slider_set_range(xf_slider, 0, 12);
    style_slider(xf_slider);
    lv_slider_set_value(xf_slider, xf_seconds(), LV_ANIM_OFF);
    lv_obj_add_event_cb(xf_slider, xf_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(xf_slider, xf_slider_cb, LV_EVENT_RELEASED, NULL);
    /* ticks sit exactly under their slider values */
    int tw = W - 28 - 36;
    lv_obj_t *ticks = box(c);
    lv_obj_set_size(ticks, lv_pct(100), LV_MAX(22, sl_line_h(&ss_regular_16)));
    const char *tk[] = { "0 s", "3 s", "6 s", "9 s", "12 s" };   /* the badge says Off; a long "Off" ran into 3 s */
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *t = text(ticks, tk[i], &ss_regular_16, C_DIM);
        if (i == 0) lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
        else if (i == 4) lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);
        else lv_obj_align(t, LV_ALIGN_CENTER, tw * i / 4 - tw / 2, 0);
    }
    xf_shown = xf_seconds() * 100;
    xf_graph_started = sl_ms();
    xf_graph_timer = lv_timer_create(xf_graph_tick, 40, NULL);
    xf_switch = set_switch(xg, "Also when I skip a song", NULL, xf_on_skip(), NULL);
    lv_obj_add_event_cb(xf_switch, xf_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    xf_labels(xf_seconds());
    /* gapless, said plainly (#107): there is no switch, it is always so */
    opt_note(body, "Gapless is always on: an album made to flow, live or mixed, plays with no gap between songs, "
                   "MP3 too when the encoder noted its padding (LAME or iTunes). Crossfade overlaps songs instead.");

    lv_obj_t *g = set_group(body, "Equalizer");
    set_row(g, SL_SYM_EQ, 0x1fb39a, "Equalizer", global_settings.eq_enabled ? "On" : "Off", 0x20001);
    play_order_card(body);
    g = set_group(body, "Audiobooks and podcasts");
    set_switch(g, "Rewind after a pause", "3 seconds back after a short pause, 10 after a minute, 30 after an hour",
               global_settings.sloppy_book_rewind, play_book_rewind);
    page_push(p);
}

/* ------------------------------------------------------------ quick panel */
static lv_obj_t *qp, *qp_bd, *qp_clock, *qp_date, *qp_bright, *qp_vol, *qp_np_t, *qp_np_a, *qp_np_pp;
static lv_obj_t *qp_np_btn[3];            /* prev, play, next */
static int8_t qp_np_idle = -1;            /* 0 a song; 1 nothing playing; 2 nor any session to resume */
/* The shortcuts, chosen in Settings > Notification Center from QP_FNS
 * kinds: shuffle, repeat, crossfade, EQ, sleep, lock, settings, library,
 * ReplayGain, Bluetooth, speed, headset keys, Wi-Fi, USB mode, dark mode,
 * notifications, a screenshot and music transfer. Each is one letter in
 * the setting, 0-9 then a-h. The folded panel shows the first eight; pulled
 * further it grows to the rest (qp_fn, qp_total of them), sixteen to a page.
 * The setting lists them in order. Ending in '.', those are all the panel
 * holds (the Settings page writes it so: chosen and ordered, Sonix's 16
 * and more); without it, as written before, up to eight are the folded
 * panel's and every other kind follows in its own order. */
#define QP_FNS 18
#define QP_PAGE 16                      /* tiles to a page of the grown card: four rows */
static lv_obj_t *qp_tiles[QP_FNS], *qp_tile_lbl[QP_FNS];
static int8_t qp_tile_state[QP_FNS];
static int qp_fn[QP_FNS], qp_n, qp_total;
static const char *const qp_fn_names[QP_FNS] = {
    "Shuffle", "Repeat", "Crossfade", "Equalizer", "Sleep timer", "Lock", "Settings", "Library",
    "ReplayGain", "Bluetooth", "Playback speed", "Headset controls", "Wi-Fi", "USB mode", "Dark mode",
    "Notifications", "Screenshot", "Transfer music" };
static const char qp_fn_keys[QP_FNS + 1] = "0123456789abcdefgh";
/* each one's symbol (dark mode's moon is drawn: qx_dark_icon_draw), and
 * the colour of its icon in Settings, that of the page it belongs with */
static const char *const qp_fn_syms[QP_FNS] = { LV_SYMBOL_SHUFFLE, LV_SYMBOL_LOOP, SL_SYM_XFADE, SL_SYM_EQ,
    LV_SYMBOL_POWER, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_SETTINGS, LV_SYMBOL_DIRECTORY,
    LV_SYMBOL_VOLUME_MID, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_CHARGE, LV_SYMBOL_AUDIO,
    LV_SYMBOL_WIFI, LV_SYMBOL_USB, "", LV_SYMBOL_BELL, LV_SYMBOL_IMAGE, LV_SYMBOL_UPLOAD };
static const uint32_t qp_fn_tints[QP_FNS] = { 0xe8408a, 0x8b6fd6, 0xf0602f, 0x1fb39a,
    0x6c63ff, 0x3aa856, 0x66738a, 0x4f7fe0,
    0xf0602f, 0x2d7ff0, 0xe89a1c, 0xb8894f,
    0x0aa9c7, 0xe89a1c, 0x6c63ff, 0x1fb39a, 0xd93fa0, 0x3aa856 };
/* The panel's shortcuts, in order, into fn (QP_FNS long); *total of them.
 * Returns how many the folded panel shows. */
static int qp_fn_parse(int *fn, int *total)
{
    const unsigned char *s = global_settings.sloppy_qp_tiles;
    bool only = strchr((const char *)s, '.') != NULL;
    int n = 0;
    unsigned seen = 0;
    for (const unsigned char *c = s; *c && n < (only ? QP_FNS : 8); c++)
    {
        const char *at = *c != '.' ? strchr(qp_fn_keys, *c | (*c >= 'A' ? 32 : 0)) : NULL;
        int f = at ? (int)(at - qp_fn_keys) : -1;
        if (f < 0 || f >= QP_FNS || (seen & 1u << f)) continue;
        seen |= 1u << f;
        fn[n++] = f;
    }
    if (!n)
    {
        only = false;
        for (; n < 8; n++) { fn[n] = n; seen |= 1u << n; }
    }
    int folded = only ? LV_MIN(n, 8) : n;
    if (!only)                           /* then the rest, in their order */
        for (int f = 0; f < QP_FNS; f++)
            if (!(seen & 1u << f)) fn[n++] = f;
    *total = n;
    return folded;
}
static int speed_pct(void);
static void speed_set(int pct);
static void speed_next(void);
static void qp_wifi_toggle(void);
static void wf_share_cb(lv_event_t *e);   /* sloppy_wifi.inc: the transfer page */
static bool wf_transfer_on(const char **ip);
static void shot_after_panel(void);      /* sloppy_shot.inc */
static lv_obj_t *qp_pet, *qp_pet_canvas, *qp_pet_line, *qp_pet_name;
static lv_obj_t *qp_notes, *qp_notes_head, *qp_notes_new, *qp_notes_rows, *qp_notes_empty;
static lv_obj_t *qp_scroll;              /* everything on the panel but its handle */
#define QP_GRAB_STRIP 44                  /* the handle's band at the foot: 18 below it, 20 above */
/* Pulled further, the panel grows (qx_set): the shortcuts card by up to
 * two rows, to every shortcut it holds. The sliders ride down under it.
 * More than a page of them (QP_PAGE), the card pages sideways (qx_page_*)
 * with dots under the tiles, in a band the card grows by too. */
#define QX_ROW  114                     /* a row of tiles (QP_TILE_H) */
#define QX_DOTS 18                      /* the band the page dots sit in */
#define QX_DOT  6                       /* a dot, and the gap between two is 8 */
#define QX_TP_H 236                     /* the shortcuts card, folded */
#define QX_PUSH 300                     /* Now Playing and the notes: off the bottom */
static lv_obj_t *qp_tp, *qp_sp, *qp_np_card, *qx_edit, *qx_power, *qp_head, *qp_cap[2], *qx_dots;
static int qx_px, qx_px0;               /* how far it has grown; at the grab */
static int qx_grow;                     /* how far it grows: 2 * QX_ROW for a full page, 0 for one card's worth */
static int qx_pages, qx_page;           /* pages of the grown card; the one in sight */
static bool qp_notes_stale = true;
static uint32_t qp_notes_minute;
static bool qp_open;
static bool qp_is_visible(void)
{
    return qp && !lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN);
}
static int sleep_choice;
/* minutes; the last two stop at the end of this song, or of this album */
static const int sleep_minutes[] = { 0, 15, 30, 60, 90, 0, 0 };
enum { SLEEP_CHOICES = 7, SLEEP_SONG = 5, SLEEP_ALBUM = 6 };
static char sleep_path[MAX_PATH];        /* the song it ends after, or the album's: one of its songs */
static int sleep_album = -1;
static unsigned long sleep_seen_at, sleep_seen_len;   /* where the song was at the last poll */
static uint32_t sleep_end_ms;           /* a length's end, on sl_ms's clock */
static void sleep_stop_streams(void);    /* the radio and AirPlay (sloppy_stations.inc) */
static bool sleep_streams_on(void);
/* The last half minute of a timer in minutes eases everything down to
 * silence (pcmbuf_sleep_fade), so the stop at the end is not a cut (#38). */
#define SLEEP_FADE_MS 30000
static bool sleep_fading;
/* The timer counts its own minutes (Rockbox's would pause only songs, and
 * then start again): at the end it stops whatever plays, songs, a book,
 * the radio or AirPlay, and goes off. The length chosen is kept
 * (sloppy_sleep_timer), so the tile starts it again with one tap. */
static void sleep_set(int choice)
{
    if (choice < 0 || choice >= SLEEP_CHOICES) choice = 0;
    if (sleep_fading)                    /* changed or turned off on the way down: the level comes back */
    {
        sleep_fading = false;
        pcmbuf_sleep_fade(0, sleep_streams_on());
    }
    sleep_choice = choice;
    if (choice) global_settings.sloppy_sleep_timer = choice;
    sleep_end_ms = sl_ms() + (uint32_t)sleep_minutes[choice] * 60000u;
    if (!sleep_end_ms) sleep_end_ms = 1;
    struct mp3entry *id3 = audio_current_track();
    sleep_path[0] = '\0';
    sleep_album = -1;
    sleep_seen_at = sleep_seen_len = 0;
    if (choice >= SLEEP_SONG && id3)
    {
        int t = sl_lib_find_path(id3->path);
        strlcpy(sleep_path, id3->path, sizeof sleep_path);
        sleep_album = t >= 0 ? sl_lib.tracks[t].album : -1;
    }
}
/* Every tick: the song (or the album's last song) about to end pauses
 * instead, a moment early rather than a moment into the next one. */
static void sleep_end_poll(uint32_t tick_ms)
{
    if (sleep_choice > 0 && sleep_choice < SLEEP_SONG)
    {
        int32_t left = (int32_t)(sleep_end_ms - sl_ms());
        if (left > 0)
        {
            if (left <= SLEEP_FADE_MS && !sleep_fading)
            {
                sleep_fading = true;
                pcmbuf_sleep_fade((left * HZ + 999) / 1000, sleep_streams_on());
                sl_log("sleep timer: fading out over %d s", (int)((left + 500) / 1000));
            }
            return;
        }
        if (is_playing()) audio_pause();
        sleep_stop_streams();
        sleep_fading = false;
        pcmbuf_sleep_fade(-1, false);    /* the level comes back once the pause has muted */
        sl_log("sleep timer: %d minutes up", sleep_minutes[sleep_choice]);
        sleep_set(0);
        notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "sleep", "Sleep timer: playback stopped");
        if (qp_is_visible()) qp_refresh();
        return;
    }
    if (sleep_choice < SLEEP_SONG || !is_playing()) return;
    struct mp3entry *id3 = audio_current_track();
    if (!id3) return;
    /* the song it waited on ended between polls (a stall, a crossfade):
     * stop now rather than take it for a skip by hand */
    bool ran_out = sleep_seen_len && sleep_seen_at + tick_ms + 1500 >= sleep_seen_len;
    if (sleep_choice == SLEEP_SONG && strcmp(id3->path, sleep_path))
    {
        if (!ran_out) { sleep_set(0); return; }   /* moved on by hand: that song is over */
        audio_pause();
        sl_log("sleep timer: stopped after the song ended");
        sleep_set(0);
        notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "sleep", "Sleep timer: playback stopped");
        return;
    }
    if (sleep_choice == SLEEP_ALBUM)
    {
        int t = sl_lib_find_path(id3->path);
        if (t < 0 || sl_lib.tracks[t].album != sleep_album) { sleep_set(0); return; }
        struct mp3entry *next = audio_next_track();
        int nt = next ? sl_lib_find_path(next->path) : -1;
        const struct sl_album *al = &sl_lib.albums[sleep_album];
        bool wraps = nt >= 0 && al->n_tracks && nt == al->tracks[0] && t != al->tracks[0];   /* repeat all: round again */
        if (nt >= 0 && sl_lib.tracks[nt].album == sleep_album && !wraps) { sleep_seen_at = id3->elapsed; sleep_seen_len = id3->length; return; }
    }
    sleep_seen_at = id3->elapsed;
    sleep_seen_len = id3->length;
    if (!id3->length || id3->elapsed + tick_ms + 200 < id3->length) return;
    audio_pause();
    sl_log("sleep timer: stopped at the end of the %s", sleep_choice == SLEEP_SONG ? "song" : "album");
    sleep_set(0);
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "sleep", "Sleep timer: playback stopped");
}

static const char *qp_tile_label(int k, char *b, int n)
{
    switch (k)
    {
        case 0: return "Shuffle";
        case 1: return global_settings.repeat_mode == REPEAT_ONE ? "Repeat one" :
                       global_settings.repeat_mode == REPEAT_ALL ? "Repeat all" : "Repeat";
        case 2: if (xf_seconds()) { snprintf(b, n, "Fade %d s", xf_seconds()); return b; } return "Crossfade";
        case 3: return "EQ";
        case 4:
            if (sleep_choice == SLEEP_SONG) return "End of song";
            if (sleep_choice == SLEEP_ALBUM) return "End of album";
            if (sleep_choice) { snprintf(b, n, "%d min", sleep_minutes[sleep_choice]); return b; }
            return "Sleep";
        case 5: return "Lock";
        case 6: return "Settings";
        case 8: return "ReplayGain";
        case 9: return "Bluetooth";
        case 10:
        {
            int pct = speed_pct();           /* 0.75x, 1.25x, 1.5x, 2x as on the Sound page */
            if (pct == 100) return "Speed";
            if (pct % 100 == 0) snprintf(b, n, "%dx", pct / 100);
            else if (pct % 10 == 0) snprintf(b, n, "%d.%dx", pct / 100, pct % 100 / 10);
            else snprintf(b, n, "%d.%02dx", pct / 100, pct % 100);
            return b;
        }
        case 11: return "Headset";
        case 12: return "Wi-Fi";
        case 13: return "USB mode";
        case 14: return "Dark mode";
        case 15: return "Notifications";
        case 16: return "Screenshot";
        case 17: { const char *ip; return wf_transfer_on(&ip) ? ip : "Transfer"; }   /* the address to type */
        default: return "Library";
    }
}
static bool qp_tile_on(int k)
{
    switch (k)
    {
        case 0: return global_settings.playlist_shuffle;
        case 1: return global_settings.repeat_mode != REPEAT_OFF;
        case 2: return xf_seconds() > 0;
        case 3: return global_settings.eq_enabled;
        case 4: return sleep_choice > 0;
        case 8: return global_settings.replaygain_settings.type != REPLAYGAIN_OFF;
        case 9: return sl_bt_known && sl_bt_state.enabled;
        case 10: return speed_pct() != 100;
        case 11: return global_settings.sloppy_remote_keys;
        case 12: return global_settings.sloppy_wifi;
        case 14: return global_settings.sloppy_theme != 1;
        case 15: return notes_on();
        case 17: return wf_transfer_on(NULL);
        default: return false;
    }
}
static void label_if_changed(lv_obj_t *o, const char *value)
{
    value = sl_tr(value);
    if (strcmp(lv_label_get_text(o), value)) lv_label_set_text(o, value);
}
/* A tile's name under its circle, kept to its own cell: in the text size's
 * face when it fits; else a smaller face; else two lines of the smallest.
 * Bigger text and longer words (German, "Notifications") ran into the next
 * tile's name and down onto the circle. The cell is the row's pitch tall
 * and its circle ends at QP_TILE_TOP. Not LONG_DOT: it writes the dots
 * into the label's text, and the next refresh measured those. */
#define QP_TILE_W   ((W - 28) / 4)
#define QP_TILE_H   114
#define QP_TILE_TOP 78                  /* first row clear of the circle */
#define QP_TILE_TEXT_W (QP_TILE_W - 6)
static void qp_tile_fit(lv_obj_t *l, const char *value)
{
    value = sl_tr(value);
    if (!strcmp(lv_label_get_text(l), value)) return;
    lv_label_set_text(l, value);
    const char *shown = lv_label_get_text(l);   /* as drawn: Arabic joined */
    lv_point_t sz;
    /* the text size's face, then each smaller one, on one line */
    for (int step = sl_text_step; step >= 0; step--)
    {
        const lv_font_t *f = sl_fonts[step][2];
        lv_text_get_size(&sz, shown, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > QP_TILE_TEXT_W || lv_font_get_line_height(f) > QP_TILE_H - 6 - QP_TILE_TOP) continue;
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_line_space(l, 0, 0);
        lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
        return;
    }
    /* two lines of the smallest, centred, held to the cell */
    const lv_font_t *f = sl_fonts[0][0];
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_line_space(l, -3, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, QP_TILE_TEXT_W, 2 * lv_font_get_line_height(f) - 3);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
}
/* The last card in the control centre: a notification from your pet, with
 * its mood and that it misses you. Only once you have a living pet; the
 * save is read, never created. */
static int qp_pet_drawn = -1;           /* species and stage in the sprite; -1 = redraw */
static void qp_pet_update(void)
{
    static uint16_t sprite[48 * 48];
    static uint32_t no_file_until;
    if (!qp_pet) return;
    if (!pet_loaded)
    {
        /* this runs 4 times a second while the panel is open: don't
         * probe the card each time for a pet that isn't there */
        if (no_file_until && (int32_t)(lv_tick_get() - no_file_until) < 0) return;
        int fd = open(PET_FILE, O_RDONLY);
        if (fd < 0)
        {
            no_file_until = lv_tick_get() + 5000;
            hidden_if_changed(qp_pet, true);
            return;
        }
        close(fd);
    }
    pet_load();
    uint32_t clock = pet_clock();
    struct sl_pet_state now = pet;
    sl_pet_advance(&now, clock);
    if (now.species == SL_PET_UNCHOSEN || now.dead || !now.name[0])
    { hidden_if_changed(qp_pet, true); return; }
    hidden_if_changed(qp_pet, false);
    char b[96];
    snprintf(b, sizeof b, "%s %s", now.name, pet_mood_words[pet_mood_of(&now)]);
    label_if_changed(qp_pet_name, b);
    int stage = sl_pet_stage_at(&now, clock), key = now.species << 8 | stage;
    if (key == qp_pet_drawn) return;    /* the same sprite: don't repaint it */
    qp_pet_drawn = key;
    lv_canvas_set_buffer(qp_pet_canvas, sprite, 48, 48, LV_COLOR_FORMAT_RGB565);
    pet_render(qp_pet_canvas, sprite, 3, now.species, stage, PET_IDLE, 1);
}
static void qp_close(void);
static void qp_pet_cb(lv_event_t *e)
{
    (void)e;
    qp_close();
    if (!pet_page_obj) pet_page();
}

static uint32_t qp_glass_capture_ms;
static void qp_notes_build_rows(void);
static void qp_notes_ages(void);
static void qp_notes_seen(void);
static void qp_refresh(void)
{
    if (!qp) return;
    qp_pet_update();
    /* the history: rebuilt when it changed; its ages once a minute, in place */
    uint32_t minute = note_clock() / 60;
    if (qp_notes_stale) qp_notes_build_rows();
    else if (minute != qp_notes_minute) qp_notes_ages();
    qp_notes_seen();
    char b[32];
    for (int k = 0; k < qp_total; k++)
    {
        lv_obj_t *symbol = lv_obj_get_child(qp_tiles[k], 0);
        /* iOS 7: a faint glass circle when off, solid when on */
        bool light = sl_theme() == 1, on = qp_tile_on(qp_fn[k]);
        int8_t state = (light ? 2 : 0) | (on ? 1 : 0);
        if (qp_tile_state[k] != state)
        {
            lv_obj_set_style_bg_color(symbol, lv_color_hex(light ? 0x182735 : 0xffffff), 0);
            lv_obj_set_style_bg_opa(symbol, on ? LV_OPA_COVER : light ? 34 : 52, 0);
            if (lv_obj_get_child_count(symbol))
                lv_obj_set_style_text_color(lv_obj_get_child(symbol, 0),
                    lv_color_hex(on == light ? 0xffffff : 0x182735), 0);
            qp_tile_state[k] = state;
        }
        qp_tile_fit(qp_tile_lbl[k], qp_tile_label(qp_fn[k], b, sizeof b));
    }
    clock_text(b, sizeof b, get_time());
    label_if_changed(qp_clock, b);
    snprintf(b, sizeof b, "%d%%", battery_level());
    label_if_changed(qp_date, b);
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    char name[64];
    label_if_changed(qp_np_t, id3 ? track_title(id3, name, sizeof name) : "Nothing playing");
    label_if_changed(qp_np_a, id3 ? track_artist(id3) : "");
    /* nothing playing: no song to skip, so no skip buttons, and the words
     * sit in the middle of the card; play stays while there is a last
     * session to pick up (toggle_play resumes it) */
    if (qp_np_idle != !id3 + (!id3 && global_status.resume_index == -1))
    {
        qp_np_idle = !id3 + (!id3 && global_status.resume_index == -1);
        lv_obj_set_y(qp_np_t, qp_np_idle ? 52 - lv_obj_get_style_height(qp_np_t, 0) / 2 : 22);
        lv_obj_align(qp_np_pp, LV_ALIGN_RIGHT_MID, qp_np_idle ? -20 : -72, 0);   /* alone: at the card's inset, as the words are */
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *b = qp_np_btn[i];
            if (qp_np_idle > (i == 1 ? 1 : 0)) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
    label_if_changed(lv_obj_get_child(qp_np_pp, 0), is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (!lv_slider_is_dragged(qp_vol)) lv_slider_set_value(qp_vol, volume_pct(), LV_ANIM_OFF);
    /* set on Display & lock too */
    if (!lv_slider_is_dragged(qp_bright)) lv_slider_set_value(qp_bright, sl_bright_value(), LV_ANIM_OFF);
}
static void qx_set(int px);
static void qx_anim_cb(void *o, int32_t v);
static void (*qp_after_close)(void);
static void qp_after_close_cb(lv_timer_t *t)
{
    (void)t;                             /* one shot: LVGL deletes it */
    void (*fn)(void) = qp_after_close;
    qp_after_close = NULL;
    if (fn && !qp_is_visible()) fn();
}
static void qp_hidden_cb(lv_anim_t *a)
{
    hide_cb(a); lv_obj_invalidate(qp_bd);
    lv_anim_delete(qp_tp, qx_anim_cb);   /* it comes back folded */
    qx_set(0);
    if (qp_after_close) lv_timer_set_repeat_count(lv_timer_create(qp_after_close_cb, 0, NULL), 1);
    sl_perf_end();
    sl_perf_memory("panel closed");
}
static void qp_settled_cb(lv_anim_t *a)
{
    (void)a;
    sl_perf_end();
    sl_log("glass capture: %lu ms", (unsigned long)qp_glass_capture_ms);
    sl_perf_memory("panel open");
}
static void qp_close_to(int from)
{
    if (qp_open) sfx_play(SFX_UI_PANEL_CLOSE);
    qp_open = false;
    /* A short pull can spring back; keep its first-pull measurements. */
    if (!sl_perf.name) sl_perf_begin("panel close");
    anim_prop(qp, set_ty, from, -H, 240, qp_hidden_cb);
}
static void qp_close(void) { if (qp_open) qp_close_to(lv_obj_get_style_translate_y(qp, 0)); }
static bool qp_long_consumed;
static void qp_prepare_settings(void)
{
    qp_close();
    if (np_open) np_hide(lv_obj_get_y(np));
}
/* Settings controls (EQ sliders, crossfade graph, ...) keep live widget
 * pointers in globals. Start from home so opening a settings page from the
 * quick panel cannot leave an older copy pointing at deleted controls. */
static void qp_settings_from_home(void)
{
    qp_prepare_settings();
    while (npages > 1) lv_obj_delete(pages[--npages]);
    lv_anim_delete(pages[0], NULL);
    lv_obj_remove_flag(pages[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(pages[0], 0);
}
/* What needs the screen without the panel on it (the power sheet frosts
 * what is there; a new theme rebuilds the panel): once it has gone
 * (qp_hidden_cb), from a timer, not from inside the panel's animation. A
 * panel caught and opened again on the way forgets it. */
static void qp_close_then(void (*fn)(void))
{
    qp_close();
    qp_after_close = fn;
}
/* Dark mode: Light, or back to the dark look it left (Dark at first).
 * Leaving Zule brings the tiles home back, and returning brings its list,
 * as on the Theme page; the other homes are left as they are. */
static int qp_dark_theme;
static void qp_dark_mode_toggle(void)
{
    int was = global_settings.sloppy_theme;
    if (was != 1) qp_dark_theme = was;
    global_settings.sloppy_theme = was != 1 ? 1 : qp_dark_theme;
    if (was == 3 || global_settings.sloppy_theme == 3)
        global_settings.sloppy_home_style = global_settings.sloppy_theme == 3 ? 2 : 1;
    if (settings_save() != 0) toast_warn("Could not save customization settings");
    appearance_reload((void *)(intptr_t)2);
}
static void notes_switch(int on);
/* Started: the one thing that could still end the music first is the
 * battery (Auto power off waits while anything plays, the radio and
 * AirPlay included), so say so when the estimate is shorter. */
static void sleep_battery_check(void)
{
    int mins = sleep_choice > 0 && sleep_choice < SLEEP_SONG ? sleep_minutes[sleep_choice] : 0;
#if CONFIG_CHARGING
    if (charger_inserted()) return;
#endif
    int left = battery_time();
    if (!mins || left <= 0 || left >= mins) return;
    char b[160];
    snprintf(b, sizeof b, sl_tr("The battery may run out first: about %d min left"), left);
    toast_warn(b);
}
static void qp_sleep_select_cb(lv_event_t *e)
{
    sleep_set((int)(intptr_t)lv_event_get_user_data(e));
    if (sleep_choice) settings_save();   /* the length the tile starts with next, after a restart too */
    set_check_mark(lv_event_get_current_target(e));
    qp_refresh();
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "sleep", sleep_choice ? "Sleep timer set" : "Sleep timer off");
    sleep_battery_check();
}
/* the tile: off, or on with the length chosen last */
static void sleep_tile_tap(void)
{
    if (sleep_choice) { sleep_set(0); return; }
    int c = global_settings.sloppy_sleep_timer;
    if (c <= 0 || c >= SLEEP_CHOICES) c = 2;
    if (c >= SLEEP_SONG && !(audio_status() & AUDIO_STATUS_PLAY)) c = 2;   /* nothing playing to end: 30 minutes */
    if (c == SLEEP_ALBUM && np_book) c = SLEEP_SONG;                        /* a book: its chapter */
    int keep = global_settings.sloppy_sleep_timer;
    sleep_set(c);
    global_settings.sloppy_sleep_timer = keep;                             /* a stand-in is not a new choice */
    sleep_battery_check();
}
static void qp_sleep_page(void)
{
    lv_obj_t *p, *body = page_new("Sleep timer", &p);
    lv_obj_t *g = set_group(body, "STOP PLAYBACK AFTER");
    static const char *names[] = { "Off", "15 minutes", "30 minutes", "60 minutes", "90 minutes",
                                   "End of this song", "End of this album" };
    for (int i = 0; i < SLEEP_CHOICES; i++)
    {
        if (i >= SLEEP_SONG && !audio_current_track()) break;   /* nothing playing to end */
        if (i == SLEEP_ALBUM && np_book) break;                    /* a book: its chapter is the song */
        set_check(g, NULL, 0, i == SLEEP_SONG && np_book ? "End of this chapter" : names[i], i == sleep_choice,
                  qp_sleep_select_cb, (void *)(intptr_t)i);
    }
    opt_note(body, "Tap Sleep in the control center to start your last choice again, and touch and hold it to come here.");
    if (global_settings.poweroff > 0)
    {
        char b[200];
        snprintf(b, sizeof b, sl_tr("When the music stops, Auto power off turns the player off %d min later."),
                 global_settings.poweroff);
        opt_note(body, b);
    }
    page_push(p);
}
static void qp_display_bright_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_current_target(e);
    sl_bright_set(lv_slider_get_value(slider));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void qp_lock_now_cb(lv_event_t *e) { (void)e; sl_power_lock("Lock now"); }
static void qp_display_page(void)
{
    lv_obj_t *p, *body = page_new("Display & lock", &p);
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_t *r = set_slider_row(g, "Brightness", "", C_ACCENT_TXT);
    lv_obj_add_flag(lv_obj_get_child(r, 1), LV_OBJ_FLAG_HIDDEN);   /* the screen itself shows how bright */
    lv_obj_t *slider = lv_obj_get_child(r, 2);
    lv_slider_set_range(slider, MIN_BRIGHTNESS_SETTING - SL_NIGHT_STEPS, MAX_BRIGHTNESS_SETTING);
    lv_slider_set_value(slider, sl_bright_value(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, qp_display_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, qp_display_bright_cb, LV_EVENT_RELEASED, NULL);
    opt_note(body, "The last steps at the dark end go below the lowest backlight, for reading in bed.");
    g = set_group(body, "LOCK BEHAVIOR");
    set_action_sub(g, LV_SYMBOL_EYE_CLOSE, 0x3aa856, "Lock now", "Turn off the screen and lock", qp_lock_now_cb, NULL);
    opt_note(body, "The screen gently dims after 60 seconds, then locks. Touch to wake and swipe up to unlock.");
    page_push(p);
}
static void qp_tile_long(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    qp_long_consumed = true;
    qp_settings_from_home();
    if (k <= 2) play_settings_page();
    else if (k == 3) eq_page();
    else if (k == 4) qp_sleep_page();
    else if (k == 5) qp_display_page();
    else if (k == 7) sl_library_page();
    else if (k == 8 || k == 10) sound_page();
    else if (k == 9) { if (sl_btrx_on) sl_btrx_page(); else sl_bt_page(); }
    else if (k == 11) buttons_page();
    else if (k == 12) sl_wifi_page();
    else if (k == 13) time_power_page();
    else if (k == 14) theme_page();
    else if (k == 15 || k == 16) shortcuts_page();
    else if (k == 17) sl_wifi_page();
    else system_page();
}
static void qp_tile_pressed_cb(lv_event_t *e) { (void)e; qp_long_consumed = false; }
static void qp_tile_press(lv_event_t *e)
{
    if (qp_long_consumed) { qp_long_consumed = false; return; }
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    sl_log("quick panel tile %d", k);
    switch (k)
    {
        case 0:
            sl_set_shuffle(!global_settings.playlist_shuffle);
            break;
        case 1:
            global_settings.repeat_mode = global_settings.repeat_mode == REPEAT_OFF ? REPEAT_ALL :
                                          global_settings.repeat_mode == REPEAT_ALL ? REPEAT_ONE : REPEAT_OFF;
            audio_flush_and_reload_tracks();
            break;
        case 2: xf_apply(xf_seconds() ? 0 : xf_last, xf_on_skip() || !xf_seconds()); break;
        case 3: global_settings.eq_enabled = !global_settings.eq_enabled; eq_apply_all(); break;
        case 4:
            sleep_tile_tap();                    /* hold it for the lengths */
            break;
        case 5: qp_close(); sl_power_lock("the control centre"); return;
        case 6:
            qp_settings_from_home();
            system_page();
            return;
        case 7:
            qp_prepare_settings(); sl_library_page();
            return;
        case 8:
            global_settings.replaygain_settings.type =
                global_settings.replaygain_settings.type == REPLAYGAIN_OFF ? REPLAYGAIN_TRACK : REPLAYGAIN_OFF;
            replaygain_update();
            break;
        case 9:
            /* on or off as the Bluetooth page's switch does it, once the
             * stack has said which it is; until then, that page. While a
             * phone plays through the player, its page. */
            if (sl_net_wireless_unsupported()) { toast_warn("Bluetooth isn't supported on this R1 yet"); break; }
            if (sl_btrx_on) { qp_prepare_settings(); sl_btrx_page(); return; }
            if (sl_bt_known && !sl_bt_busy)
            {
                sl_bt_job_power = !sl_bt_state.enabled;
                sl_bt_begin(SL_BT_JOB_POWER, sl_bt_job_power ? "Turning on Bluetooth…" : "Turning off Bluetooth…");
                break;
            }
            qp_prepare_settings(); sl_bt_page();
            return;
        case 10: speed_next(); break;
        case 11: global_settings.sloppy_remote_keys = !global_settings.sloppy_remote_keys; break;
        case 12: qp_wifi_toggle(); break;
        case 13:                             /* its choices are on Time and power */
            qp_settings_from_home();
            time_power_page();
            return;
        case 14: qp_close_then(qp_dark_mode_toggle); return;
        case 15: notes_switch(!notes_on()); break;
        case 16: qp_close_then(shot_after_panel); return;   /* the page, not the panel */
        case 17:                             /* the page that keeps it running and shows the address */
            qp_prepare_settings();
            wf_share_cb(NULL);
            return;
    }
    settings_save();
    qp_refresh();
}
static void qp_bright_cb(lv_event_t *e)
{
    sl_bright_set(lv_slider_get_value(qp_bright));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void qp_vol_cb(lv_event_t *e)
{
    (void)e;
    global_status.volume = volume_from_pct(lv_slider_get_value(qp_vol));
    setvol();
    statusbar_update();
}
static void qp_refresh_timer(lv_timer_t *t) { (void)t; if (qp_open) qp_refresh(); }
static void qp_np_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == 1) sl_prev(); else if (k == 2) toggle_play(); else if (k == 3) sl_next();
    else if (k == 0 && something_playing()) { qp_close(); np_show(); }
    lv_timer_t *t = lv_timer_create(qp_refresh_timer, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}
/* The panel scrolls as one, notifications and all. A drag up puts it away
 * only once there is no more to scroll to: from the foot of the list, or
 * when it all fits. A drag that scrolled never closes it. */
static void qp_drag_cb(lv_event_t *e)
{
    static int y0, sy0;
    static bool scrolled;
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED)
    {
        y0 = p.y;
        sy0 = lv_obj_get_scroll_y(qp_scroll);
        scrolled = false;
    }
    else if (c == LV_EVENT_PRESSING)
    {
        /* LVGL names a scroll object even at the foot, where it cannot
         * move: what counts is whether the panel's content moved */
        if (lv_obj_get_scroll_y(qp_scroll) != sy0) scrolled = true;
        if (scrolled || qx_px) return;  /* grown: a drag is qx_grab's */
        if (p.y < y0 && lv_obj_get_scroll_bottom(qp_scroll) <= 0)
            lv_obj_set_style_translate_y(qp, p.y - y0, 0);
        else y0 = p.y;
    }
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        int dy = lv_obj_get_style_translate_y(qp, 0);
        lv_area_t sliders;                               /* grown, the empty bottom is under them */
        lv_obj_get_coords(qp_sp, &sliders);
        if (dy < -100) qp_close_to(dy);
        else if (dy < 0) anim_prop(qp, set_ty, dy, 0, 260, NULL);
        else if (!scrolled && p.y > (qx_px ? sliders.y2 : H - 120) && p.y - y0 < 8 &&
                 (lv_event_get_target(e) == qp || lv_event_get_target(e) == qp_scroll))
            qp_close();                                  /* tap on the empty bottom */
    }
}
/* From the bottom edge, up: the panel goes, however far down its list is.
 * The contact is the panel's from the press (touch_read, G_PANEL), so the
 * list never scrolls first. The handle widens and brightens under the
 * thumb; a flick or a long enough drag puts the panel away, a short one
 * lets it spring back, and a tap there closes it as before. */
#define QP_EDGE 56
static lv_obj_t *qp_grab;
static void qp_grab_lit(bool on)
{
    if (!qp_grab) return;
    lv_obj_set_width(qp_grab, on ? 96 : 64);
    lv_obj_set_style_bg_opa(qp_grab, on ? 235 : LV_OPA_50, 0);
}
static bool qp_edge_grab(int y)
{
    if (!qp || !qp_open || lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN) || y < H - QP_EDGE) return false;
    lv_anim_delete(qp, NULL);            /* caught mid spring: from where it is */
    qp_grab_lit(true);
    return true;
}
static void qp_edge_drag(int dy)
{
    lv_obj_set_style_translate_y(qp, dy < 0 ? dy : 0, 0);
}
static void qp_edge_release(int dy, int vy)
{
    qp_grab_lit(false);
    if (dy < -90 || (dy < -20 && vy < -500)) qp_close_to(dy < 0 ? dy : 0);
    else if (dy > -8 && dy < 8) qp_close();          /* a tap on the edge */
    else anim_prop(qp, set_ty, dy < 0 ? dy : 0, 0, 260, NULL);
}

/* --------------------------------------- the panel, pulled further
 * Android's quick settings: pulled down again from the top of its list,
 * or further in the one pull, the shortcuts card grows by up to two rows
 * to every shortcut it holds, paging sideways past sixteen. The sliders
 * ride down under it, Now Playing and the notifications step out of the
 * way, Edit and Power come up by the heading. Pushed up it folds back; a flick decides either way. A frame is
 * the card's height and a few translations over the glass already taken:
 * nothing is blurred again. */
static void qx_page_show(int page);
static void qx_set(int px)
{
    if (px < 0) px = 0;
    if (px > qx_grow) px = qx_grow;
    if (!px && qx_page) qx_page_show(0);   /* folded, the first page's rows are the panel's */
    if (!qp_tp || px == qx_px) return;
    bool grown = px > 0;
    if (grown != (qx_px > 0))
    {
        /* folded, none of it is drawn or hit, and the list scrolls again */
        lv_obj_update_flag(qx_edit, LV_OBJ_FLAG_HIDDEN, !grown);
        lv_obj_update_flag(qx_power, LV_OBJ_FLAG_HIDDEN, !grown);
        for (int k = qp_n; k < LV_MIN(8, qp_total); k++) lv_obj_update_flag(qp_tiles[k], LV_OBJ_FLAG_HIDDEN, !grown);
        lv_obj_update_flag(qp_scroll, LV_OBJ_FLAG_SCROLLABLE, !grown);
    }
    qx_px = px;
    /* LVGL would redraw the whole card, sixteen glass circles and all, for
     * a new height: only the band its foot crossed changes (its rounded
     * corners and edge with it), so only that is drawn again */
    lv_area_t band;
    lv_obj_update_layout(qp);
    lv_obj_get_coords(qp_tp, &band);
    int was = band.y2;
    lv_display_enable_invalidation(disp, false);
    lv_obj_set_height(qp_tp, QX_TP_H + px);
    lv_obj_update_layout(qp);
    lv_display_enable_invalidation(disp, true);
    lv_obj_get_coords(qp_tp, &band);
    band.y1 = LV_MIN(was, band.y2) - 28;
    band.y2 = LV_MAX(was, band.y2) + 1;
    lv_obj_invalidate_area(qp_scroll, &band);
    set_ty(qp_sp, px);
    /* stepped aside at once, not slid: sliding, everything under the
     * sliders was drawn again each frame, and the drag ran at half rate */
    set_ty(qp_np_card, grown ? QX_PUSH : 0);
    set_ty(qp_notes, grown ? QX_PUSH : 0);
    /* small things fade: a layer the size of a button, not of the panel */
    int span = qx_grow - (qx_pages > 1 ? QX_DOTS : 0);   /* as far as the rows go: the dots' band is extra */
    lv_opa_t opa = (lv_opa_t)(span > 0 ? LV_MIN(px, span) * 255 / span : 0);
    lv_obj_set_style_opa(qx_edit, opa, 0);
    lv_obj_set_style_opa(qx_power, opa, 0);
    for (int k = qp_n; k < LV_MIN(8, qp_total); k++) lv_obj_set_style_opa(qp_tiles[k], opa, 0);
    if (qx_dots)                         /* the dots ride the card's foot */
    {
        lv_obj_update_flag(qx_dots, LV_OBJ_FLAG_HIDDEN, !grown);
        lv_obj_set_y(qx_dots, 108 + QX_TP_H + px - (QX_DOTS + QX_DOT) / 2);
        lv_obj_set_style_opa(qx_dots, (lv_opa_t)(px * 255 / qx_grow), 0);
    }
}
/* ---- the grown card's pages: a sideways drag on the tiles moves them
 * under the finger, a flick or half a page turns it. The card scrolls
 * (LVGL never does: it is not scrollable, the contact is G_CCPAGE's). */
#define QX_PAGE_W (W - 28)              /* the card's width */
static int qx_page_x0;                  /* the scroll at the grab */
static bool qx_page_timed;              /* a page turn sl_perf is timing (not one a fold set off) */
static void qx_dots_mark(void)
{
    if (!qx_dots) return;
    bool light = sl_theme() == 1;
    for (int i = 0; i < qx_pages; i++)
        lv_obj_set_style_bg_opa(lv_obj_get_child(qx_dots, i),
                                i == qx_page ? (light ? 200 : 230) : (light ? 56 : 72), 0);
}
static void qx_page_scroll(void *o, int32_t x) { (void)o; lv_obj_scroll_by(qp_tp, lv_obj_get_scroll_x(qp_tp) - x, 0, LV_ANIM_OFF); }
static void qx_page_settled(lv_anim_t *a) { (void)a; if (qx_page_timed) sl_perf_end(); qx_page_timed = false; }
static void qx_page_show(int page)
{
    lv_anim_delete(qp_tp, qx_page_scroll);
    qx_page = page;
    qx_page_scroll(NULL, page * QX_PAGE_W);
    qx_dots_mark();
}
static void qx_page_go(int page)
{
    page = LV_CLAMP(0, page, qx_pages - 1);
    int from = lv_obj_get_scroll_x(qp_tp), to = page * QX_PAGE_W;
    lv_anim_delete(qp_tp, qx_page_scroll);
    qx_page = page;
    qx_dots_mark();
    if (from == to) { if (qx_page_timed) sl_perf_end(); qx_page_timed = false; return; }
    anim_prop(qp_tp, qx_page_scroll, from, to, SL_T_FAST + 160 * abs(to - from) / QX_PAGE_W, qx_page_settled);
}
/* The grown card takes a sideways drag that starts on it, when it has
 * more than one page. Not one on a slider (none are on the card). */
static bool qx_page_grab(int x, int y, int dx, int dy)
{
    if (qx_pages < 2 || !qp_open || qx_px != qx_grow || lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_style_translate_y(qp, 0) != 0 || abs(dx) <= abs(dy))
        return false;
    lv_area_t a;
    lv_obj_get_coords(qp_tp, &a);
    if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2) return false;
    lv_anim_delete(qp_tp, qx_page_scroll);   /* caught as it turns: from there */
    qx_page_x0 = lv_obj_get_scroll_x(qp_tp);
    sl_perf_begin("panel page");
    qx_page_timed = true;
    return true;
}
static void qx_page_drag(int dx)
{
    int x = qx_page_x0 - dx, max = (qx_pages - 1) * QX_PAGE_W;
    if (x < 0) x /= 3;                   /* past the first or last page: it gives, a little */
    else if (x > max) x = max + (x - max) / 3;
    qx_page_scroll(NULL, x);
}
static void qx_page_release(int dx, int vx)
{
    int page = (qx_page_x0 + QX_PAGE_W / 2) / QX_PAGE_W;
    if (vx < -400 || dx < -QX_PAGE_W / 3) page++;
    else if (vx > 400 || dx > QX_PAGE_W / 3) page--;
    qx_page_go(page);
}
static void qx_anim_cb(void *o, int32_t v) { (void)o; qx_set(v); }
static void qx_settled_cb(lv_anim_t *a) { (void)a; sl_perf_end(); }
static void qx_animate(int to)
{
    lv_anim_delete(qp_tp, qx_anim_cb);
    if (to == qx_px) { sl_perf_end(); return; }
    anim_prop(qp_tp, qx_anim_cb, qx_px, to, 120 + 160 * abs(to - qx_px) / LV_MAX(qx_grow, 1), qx_settled_cb);
}
/* The open panel takes a vertical drag: folded, one down from the top of
 * its list; grown, either way. Not one on a slider. */
static bool qx_grab(int x, int y, int dx, int dy)
{
    if (!qp_tp || !qx_grow || !qp_open || lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_style_translate_y(qp, 0) != 0 || abs(dy) <= abs(dx))
        return false;
    if (!qx_px && (dy < 0 || lv_obj_get_scroll_y(qp_scroll) > 0)) return false;
    for (lv_obj_t *o = lv_indev_search_obj(qp, &(lv_point_t){ x, y }); o; o = lv_obj_get_parent(o))
        if (lv_obj_check_type(o, &lv_slider_class)) return false;
    lv_anim_delete(qp_tp, qx_anim_cb);   /* caught as it springs: from there */
    qx_px0 = qx_px;
    if (qx_page) qx_page_go(0);          /* folding: back to the page the folded rows are on */
    sl_perf_begin(qx_px ? "panel fold" : "panel grow");
    return true;
}
static void qx_drag(int dy) { qx_set(qx_px0 + dy); }
static void qx_release(int dy, int vy)
{
    (void)dy;
    qx_animate(vy > 500 ? qx_grow : vy < -500 ? 0 : qx_px > qx_grow / 2 ? qx_grow : 0);
}
/* The one pull from the top edge: the panel's edge follows the finger,
 * then catches up with the screen's foot; pulled on from there, it grows.
 * A quick pull to mid-screen opens it folded, as before; growing it is a
 * deliberate pull most of the way down. */
#define QX_PULL_A 240                   /* the edge under the finger this far */
#define QX_PULL_B 600                   /* fully down here, then growing */
#define QX_PULL_C (H - 60)              /* fully grown */
static int qp_pull_edge(int dy)
{
    return dy <= QX_PULL_A ? dy : dy >= QX_PULL_B ? H :
           dy + (dy - QX_PULL_A) * (H - QX_PULL_B) / (QX_PULL_B - QX_PULL_A);
}
static void qp_pull(int dy)
{
    qp_drag_to(qp_pull_edge(dy));
    qx_set(dy > QX_PULL_B ? (dy - QX_PULL_B) * qx_grow / (QX_PULL_C - QX_PULL_B) : 0);
}
static void qp_pull_release(int dy, int vy, int vy_now)
{
    qp_release(qp_pull_edge(dy), vy);
    /* where it was let go, not how fast: every pull is a fast one down */
    if (qx_px) qx_animate(vy_now < -500 || qx_px <= qx_grow / 2 ? 0 : qx_grow);
}
static void qx_btn_cb(lv_event_t *e)
{
    if (lv_event_get_user_data(e)) { qp_close_then(po_offer); return; }
    qp_settings_from_home();
    shortcuts_page();
}
/* dark mode's mark: a ring, half filled */
static void qx_dark_icon_draw(lv_event_t *e)
{
    lv_obj_t *cc = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(cc, &a);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_obj_get_style_text_color(lv_obj_get_child(cc, 0), 0);
    d.center.x = (a.x1 + a.x2) / 2;
    d.center.y = (a.y1 + a.y2) / 2;
    int w = lv_area_get_width(&a);        /* 72 in the control centre, a Settings icon's 36 */
    d.radius = w * 13 / 72;
    d.width = LV_MAX(2, w * 3 / 72);
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(lv_event_get_layer(e), &d);
    d.width = d.radius;
    d.start_angle = 90;
    d.end_angle = 270;
    lv_draw_arc(lv_event_get_layer(e), &d);
}

/* ------------------------------------------------ frosted glass (iOS 7)
 * When the panel starts to come down, the screen behind it is
 * shrunk 4x, box-blurred twice (close to a gaussian of ~18 px),
 * given more colour, tinted for the theme and scaled back up with an
 * ordered dither so RGB565 does not band. A still copy of the sharp screen
 * is kept too, and a full-screen backdrop under the panel shows the frosted
 * copy where the panel is and the sharp one below it: a sheet of frosted
 * glass sliding down. Both are plain copies, no blending, and the screen
 * below the panel's edge is never redrawn while it moves. */
#define QG_S  4
#define QG_W  (W / QG_S)
#define QG_H  (H / QG_S)
static uint16_t *qp_glass_px, *qp_sharp_px;
static lv_image_dsc_t qp_glass_img, qp_sharp_img;
static bool qp_glass_ok;

#define QG_R  5                          /* two passes match the old blur's spread */
static void qg_blur_line(uint8_t *p, int n, int step)
{
    /* the line, its ends repeated r times, so the window needs no clamping */
    const int r = QG_R;
    uint8_t buf[QG_R + (QG_H > QG_W ? QG_H : QG_W) + QG_R + 1];
    for (int i = 0; i < n; i++) buf[r + i] = p[i * step];
    for (int i = 0; i < r; i++) buf[i] = buf[r];
    for (int i = 0; i <= r; i++) buf[r + n + i] = buf[r + n - 1];
    int sum = 0;
    for (int i = 0; i <= 2 * r; i++) sum += buf[i];
    for (int i = 0; i < n; i++)
    {
        p[i * step] = (uint8_t)((sum * 2979) >> 15);   /* sum / 11, exact for any 11 bytes: no divide per sample on MIPS */
        sum += buf[i + 2 * r + 1] - buf[i];
    }
}

/* Down every column at once, a row at a time: the same sums as
 * qg_blur_line(p, QG_H, QG_W), byte for byte. Walking each column alone
 * strode 120 bytes per sample through the 24 KB plane, a cache miss on
 * nearly every one: most of the 47 ms capture on the R1. */
static void qg_blur_cols(uint8_t *p)
{
    static uint8_t src[QG_W * QG_H];
    uint16_t sum[QG_W];
    const int r = QG_R;
    memcpy(src, p, sizeof src);
    for (int x = 0; x < QG_W; x++)
    {
        int s = (r + 1) * src[x];
        for (int k = 1; k <= r; k++) s += src[k * QG_W + x];
        sum[x] = (uint16_t)s;
    }
    for (int y = 0; y < QG_H; y++)
    {
        const uint8_t *add = &src[(y + r + 1 < QG_H ? y + r + 1 : QG_H - 1) * QG_W];
        const uint8_t *sub = &src[(y - r > 0 ? y - r : 0) * QG_W];
        uint8_t *out = &p[y * QG_W];
        for (int x = 0; x < QG_W; x++)
        {
            out[x] = (uint8_t)((sum[x] * 2979) >> 15);   /* sum / 11, as in qg_blur_line */
            sum[x] = (uint16_t)(sum[x] + add[x] - sub[x]);
        }
    }
}

/* The frosted copy at quarter size; it is grown to full size row by row,
 * only as far down as the panel has been pulled (qp_glass_grow). */
static uint8_t qg_ch[3][QG_W * QG_H];
static int qp_glass_rows;              /* rows of qp_glass_px grown so far */
static void qp_glass_grow(int rows)
{
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    /* The blur has already removed detail above the quarter-size grid, so
     * each sample becomes a dithered 4x4 block. Tables fold the rounding
     * and clamping into one lookup per channel. */
    static uint16_t lut_r[256 + 8], lut_g[256 + 4], lut_b[256 + 8];
    if (!lut_r[255])
    {
        for (int v = 0; v < 256 + 8; v++)
        {
            int five = v >> 3 > 31 ? 31 : v >> 3;
            lut_r[v] = (uint16_t)(five << 11);
            lut_b[v] = (uint16_t)five;
            if (v < 256 + 4) lut_g[v] = (uint16_t)((v >> 2 > 63 ? 63 : v >> 2) << 5);
        }
    }
    if (rows > H) rows = H;
    for (int cy = qp_glass_rows / QG_S; cy * QG_S < rows; cy++)
    {
        const uint8_t *cr = &qg_ch[0][cy * QG_W], *cg = &qg_ch[1][cy * QG_W], *cb = &qg_ch[2][cy * QG_W];
        for (int j = 0; j < QG_S; j++)
        {
            const uint8_t *d = bayer[j];
            uint16_t *out = &qp_glass_px[(cy * QG_S + j) * W];
            for (int cx = 0; cx < QG_W; cx++, out += QG_S)
            {
                int r = cr[cx], g = cg[cx], b = cb[cx];
                for (int i = 0; i < QG_S; i++)
                    out[i] = lut_r[r + (d[i] >> 1)] | lut_g[g + (d[i] >> 2)] | lut_b[b + (d[i] >> 1)];
            }
        }
        qp_glass_rows = (cy + 1) * QG_S;
    }
}

static unsigned long qp_glass_flushed;  /* sl_flushed_px when it was taken */
static uint32_t qp_glass_at;
static void qp_sharp_copy(const uint16_t *screen)
{
    for (int y = 0; y < H; y++)
        memcpy(&qp_sharp_px[y * W], &screen[y * SL_SCREEN_STRIDE], W * 2);
    qp_glass_flushed = sl_flushed_px;
    lv_image_cache_drop(&qp_sharp_img);
}
static void qp_glass_capture(void)
{
    uint8_t (*ch)[QG_W * QG_H] = qg_ch;
    qp_glass_ok = false;
    qp_glass_capture_ms = 0;
    /* no glass this time: the panel is solid, never clear over the page
     * (it stayed clear from the last pull and the page's text showed
     * through it, unblurred) */
    if (qp) lv_obj_set_style_bg_opa(qp, LV_OPA_COVER, 0);
    const uint16_t *screen = SL_SCREEN_PIXELS();
    if (!screen) return;
    uint32_t t0 = sl_ms();
    /* let go when memory ran short: taking 1.5 MB back on the next pull
     * pushed the music and the code out and the pull took seconds (the R1
     * log). The panel's own backdrop stands in until there is room. */
    if ((!qp_glass_px || !qp_sharp_px) &&
        !sl_mem_room((long)(!qp_glass_px + !qp_sharp_px) * W * H * 2 / 1024, "the glass"))
        return;
    if (!qp_glass_px) qp_glass_px = malloc((size_t)W * H * 2);
    if (!qp_sharp_px) qp_sharp_px = malloc((size_t)W * H * 2);
    if (!qp_glass_px || !qp_sharp_px) { sl_log("glass: no memory for it"); return; }
    qp_sharp_copy(screen);
    /* shrink: average each 4x4 block */
    for (int y = 0; y < QG_H; y++)
        for (int x = 0; x < QG_W; x++)
        {
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < QG_S; j++)
            {
                const uint16_t *s = &screen[(y * QG_S + j) * SL_SCREEN_STRIDE + x * QG_S];
                for (int i = 0; i < QG_S; i++)
                {
                    r += (s[i] >> 11) & 31; g += (s[i] >> 5) & 63; b += s[i] & 31;
                }
            }
            /* x 255/496 and 255/1008 in 16.16: no divide per pixel on MIPS */
            ch[0][y * QG_W + x] = (uint8_t)((r * 33693 + 32768) >> 16);
            ch[1][y * QG_W + x] = (uint8_t)((g * 16579 + 32768) >> 16);
            ch[2][y * QG_W + x] = (uint8_t)((b * 33693 + 32768) >> 16);
        }
    for (int c = 0; c < 3; c++)
        /* three box passes: close to a true gaussian, so bright tiles
         * behind the panel leave no hard-edged bands (two left the top row
         * of album art as a pale rectangle behind the clock) */
        for (int pass = 0; pass < 3; pass++)
        {
            for (int y = 0; y < QG_H; y++) qg_blur_line(&ch[c][y * QG_W], QG_W, 1);
            qg_blur_cols(ch[c]);
        }
    /* colour: iOS 7 lifts saturation, then lays a theme tint over it */
    uint32_t tint = sl_theme() == 1 ? 0xf2f5f8 : sl_theme() == 2 ? 0x000000 : 0x121a26;
    int keep = sl_theme() == 1 ? 118 : sl_theme() == 2 ? 158 : 150;   /* of 256 */
    int tr = (tint >> 16) & 255, tg = (tint >> 8) & 255, tb = tint & 255;
    for (int i = 0; i < QG_W * QG_H; i++)
    {
        int r = ch[0][i], g = ch[1][i], b = ch[2][i];
        int gray = (r * 77 + g * 150 + b * 29) >> 8;
        r = gray + (r - gray) * 7 / 4; g = gray + (g - gray) * 7 / 4; b = gray + (b - gray) * 7 / 4;
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        ch[0][i] = (uint8_t)((r * keep + tr * (256 - keep)) >> 8);
        ch[1][i] = (uint8_t)((g * keep + tg * (256 - keep)) >> 8);
        ch[2][i] = (uint8_t)((b * keep + tb * (256 - keep)) >> 8);
    }
    qp_glass_rows = 0;                  /* grown as the panel comes down */
    qp_glass_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    qp_glass_img.header.cf = LV_COLOR_FORMAT_RGB565;
    qp_glass_img.header.w = W;
    qp_glass_img.header.h = H;
    qp_glass_img.header.stride = W * 2;
    qp_glass_img.data = (const uint8_t *)qp_glass_px;
    qp_glass_img.data_size = (uint32_t)W * H * 2;
    qp_sharp_img = qp_glass_img;
    qp_sharp_img.data = (const uint8_t *)qp_sharp_px;
    lv_image_cache_drop(&qp_glass_img);
    lv_image_cache_drop(&qp_sharp_img);
    qp_glass_ok = true;
    qp_glass_at = sl_ms();
    qp_glass_capture_ms = qp_glass_at - t0;
    if (qp) lv_obj_set_style_bg_opa(qp, LV_OPA_TRANSP, 0);
}

/* The backdrop is two still pictures, so it is opaque: say so, and LVGL
 * (lv_refr.c) stops repainting the page under it on every frame of a pull. */
#include "src/core/lv_obj_event_private.h"
#include "src/misc/lv_area_private.h"
static bool qp_bd_on(void) { return qp && qp_glass_ok && !lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN); }
/* the backdrop, there but drawing nothing while the panel is away */
static bool qp_bd_idle(lv_obj_t *o) { return o == qp_bd && !qp_bd_on(); }
static void qp_cover_cb(lv_event_t *e)
{
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if (!qp_bd_on() || info->res == LV_COVER_RES_MASKED) return;
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    if (lv_area_is_in(info->area, &a, 0)) info->res = LV_COVER_RES_COVER;
}

/* Frosted under the panel, sharp below its edge. */
static void qp_bd_draw(lv_event_t *e)
{
    if (!qp_bd_on()) return;
    int edge = H + lv_obj_get_style_translate_y(qp, 0);   /* first row below the panel */
    qp_glass_grow(edge);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a = { 0, 0, W - 1, H - 1 }, clip = layer->_clip_area, part;
    lv_draw_image_dsc_t d;
    for (int k = 0; k < 2; k++)
    {
        lv_area_t half = { 0, k ? edge : 0, W - 1, k ? H - 1 : edge - 1 };
        if (!lv_area_intersect(&part, &clip, &half)) continue;
        layer->_clip_area = part;
        lv_draw_image_dsc_init(&d);
        d.src = k ? &qp_sharp_img : &qp_glass_img;
        lv_draw_image(layer, &d, &a);
    }
    layer->_clip_area = clip;
}

static void qp_glass_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t panel;
    lv_obj_get_coords(lv_event_get_current_target(e), &panel);
    if (!qp_glass_ok) return;
    /* Behind the clock and the header: in light themes a sheen where light
     * catches the glass; in dark ones a scrim, so white text never sits
     * on a bright patch of whatever was on screen. */
    bool light = sl_theme() == 1;
    lv_area_t top = panel;
    top.y2 = top.y1 + (light ? 260 : 200);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0].color = light ? lv_color_white() : lv_color_black();
    r.bg_grad.stops[0].opa = light ? 60 : 150;
    r.bg_grad.stops[0].frac = 0;
    r.bg_grad.stops[1].color = r.bg_grad.stops[0].color;
    r.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    r.bg_grad.stops[1].frac = 255;
    lv_draw_rect(layer, &r, &top);
}

static void qp_glass(lv_obj_t *card)
{
    /* glass on glass: a lighter pane with a hairline edge (Zune: square,
     * no edge) */
    lv_obj_set_style_radius(card, sl_zune() ? 0 : SL_R_XL, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(sl_theme() == 1 ? 0x182735 : 0xffffff), 0);
    lv_obj_set_style_bg_opa(card, sl_theme() == 1 ? 14 : 26, 0);
    lv_obj_set_style_border_width(card, sl_zune() ? 0 : 1, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, sl_theme() == 1 ? 200 : 46, 0);
}
static void qp_slider_glass(lv_obj_t *s)
{
    lv_obj_set_style_bg_color(s, lv_color_hex(sl_theme() == 1 ? 0x182735 : 0xffffff), 0);
    lv_obj_set_style_bg_opa(s, sl_theme() == 1 ? 40 : 70, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(sl_theme() == 1 ? 0x182735 : 0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s, 70, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s, lv_color_black(), LV_PART_KNOB);
}
/* ------------------------------------------------- the notifications */
/* Where a notification leads. The panel goes first; a page already open
 * comes back to the top rather than opening twice. */
static void sl_wifi_page(void);
static lv_obj_t *wf_page_obj;
static lv_obj_t *up_page_obj;
static void update_page(void);
static void note_open(int where)
{
    if (qp_open) qp_close();
    switch (where)
    {
        case NA_NOW_PLAYING: np_show(); break;
        case NA_MUSIC: action(ACT_MUSIC); break;
        case NA_PLAYLISTS: sl_saved_playlists_page(0); break;
        case NA_PET: if (!page_bring_back(pet_page_obj)) pet_page(); break;
        case NA_RADIO: if (!page_bring_back(radio_page_obj)) radio_page(); break;
        case NA_WIFI: if (!page_bring_back(wf_page_obj)) sl_wifi_page(); break;
        case NA_BT:
            if (sl_btrx_on) sl_btrx_page();
            else if (!page_bring_back(sl_bt_page_obj)) sl_bt_page();
            break;
        case NA_UPDATE: if (!page_bring_back(up_page_obj)) update_page(); break;
    }
}
static void qp_note_clear_cb(lv_event_t *e)
{
    sfx_play(SFX_UI_REMOVE);
    note_clear((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}
static void qp_note_clear_all_cb(lv_event_t *e)
{
    (void)e;
    if (!nh.n) return;
    sfx_play(SFX_UI_REMOVE);
    note_clear(0);
}
static void qp_note_open_cb(lv_event_t *e)
{
    struct note *n = note_find((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    if (!n || !n->action) return;
    note_mark_read(n);
    note_open(n->action);
}
/* how many are unread, by the heading */
static void qp_notes_count(void)
{
    char b[24];
    int n = note_unread();
    if (n) snprintf(b, sizeof b, "%d %s", n, sl_zune() ? "new" : sl_tr("NEW"));
    else b[0] = '\0';
    if (strcmp(lv_label_get_text(qp_notes_new), b)) lv_label_set_text(qp_notes_new, b);
}
/* the line over a note's words: where it came from, and when */
static void qp_note_meta(char *b, size_t n, const struct note *e)
{
    char age[24];
    snprintf(b, n, "%s  \xc2\xb7  %s", sl_tr(note_srcs[e->src].name), note_age(e->at, age, sizeof age));
}
/* A row's children: 0 the unread dot, 1 the source's mark, 2 the source and
 * age, 3 the title, 4 the detail, 5 the x, 6 the chevron when it leads
 * somewhere. Only a row that leads somewhere looks pressable. */
#define QPN_TEXT_W (W - 28 - 64 - 52)
static void qp_note_row(const struct note *e)
{
    lv_obj_t *r = box(qp_notes_rows);
    lv_obj_set_user_data(r, (void *)(uintptr_t)e->id);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    qp_glass(r);
    lv_obj_set_style_radius(r, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_pad_left(r, 64, 0);
    lv_obj_set_style_pad_right(r, 52, 0);
    lv_obj_set_style_pad_ver(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 3, 0);
    lv_obj_set_style_min_height(r, 64, 0);
    lv_obj_t *dot = box(r);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(dot, -58, 2);
    lv_obj_update_flag(dot, LV_OBJ_FLAG_HIDDEN, e->read);
    lv_obj_t *icon = box(r);
    lv_obj_set_size(icon, 38, 38);
    lv_obj_set_style_radius(icon, SL_R_ICON(38), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(icon, -48, 0);
    lv_obj_t *glyph = lv_label_create(icon);
    lv_obj_set_style_text_font(glyph, &sl_sym_20, 0);
    lv_obj_set_style_text_color(glyph, lv_color_white(), 0);
    lv_obj_center(glyph);
    note_mark(icon, glyph, e);
    char b[NOTE_TITLE + 64];
    qp_note_meta(b, sizeof b, e);
    lv_obj_t *meta = lv_label_create(r);
    lv_obj_set_style_text_font(meta, &ss_regular_16, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(C_SUB), 0);
    lv_label_set_text(meta, b);
    lv_obj_set_width(meta, QPN_TEXT_W);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    /* the words as they are: not looked up for a translation again */
    if (e->count > 1) snprintf(b, sizeof b, "%s  \xc3\x97%u", e->title, (unsigned)e->count);
    else strlcpy(b, e->title, sizeof b);
    lv_obj_t *title = lv_label_create(r);
    lv_obj_set_style_base_dir(title, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_style_text_font(title, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(e->kind == NOTE_WARN && sl_theme() != 1 ? 0xffc46b : C_TEXT), 0);
    lv_obj_set_width(title, QPN_TEXT_W);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    note_lines(title, b, QPN_TEXT_W, 2);
    lv_obj_t *body = lv_label_create(r);
    lv_obj_set_style_base_dir(body, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_style_text_font(body, &ss_regular_16, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(C_SUB), 0);
    lv_obj_set_width(body, QPN_TEXT_W);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    if (e->body[0]) note_lines(body, e->body, QPN_TEXT_W, 2);
    else lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *x = icon_btn(r, LV_SYMBOL_CLOSE, 40, qp_note_clear_cb, (void *)(uintptr_t)e->id);
    lv_obj_add_flag(x, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 44, -6);
    lv_obj_set_ext_click_area(x, 8);
    if (e->action)
    {
        lv_obj_t *chev = lv_label_create(r);
        lv_label_set_text(chev, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(chev, &sl_sym_16, 0);
        lv_obj_set_style_text_color(chev, lv_color_hex(C_SUB), 0);
        lv_obj_add_flag(chev, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(chev, LV_ALIGN_BOTTOM_RIGHT, 30, 0);
        lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, 70, LV_STATE_PRESSED);
        on_click(r, qp_note_open_cb, (void *)(uintptr_t)e->id);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);   /* its drags still scroll the list */
    }
}
/* The history as rows, newest first, the place in the list kept. */
static void qp_notes_build_rows(void)
{
    if (!qp_notes) return;
    notes_load();
    qp_notes_stale = false;
    qp_notes_minute = note_clock() / 60;
    int32_t at = lv_obj_get_scroll_y(qp_scroll);
    lv_obj_clean(qp_notes_rows);
    for (int i = 0; i < nh.n; i++) qp_note_row(&nh.list[i]);
    /* with the history off, the panel ends at your pet: toasts are not kept */
    hidden_if_changed(qp_notes_head, !notes_on() || nh.n == 0);
    hidden_if_changed(qp_notes_rows, !notes_on());
    hidden_if_changed(qp_notes_empty, !notes_on() || nh.n != 0);
    qp_notes_count();
    lv_obj_update_layout(qp_scroll);
    lv_obj_scroll_to_y(qp_scroll, at, LV_ANIM_OFF);
}
/* once a minute: only the ages change, and only their labels are set */
static void qp_notes_ages(void)
{
    qp_notes_minute = note_clock() / 60;
    char b[NOTE_TITLE + 64];
    for (uint32_t i = 0; i < lv_obj_get_child_count(qp_notes_rows); i++)
    {
        lv_obj_t *r = lv_obj_get_child(qp_notes_rows, i);
        const struct note *e = note_find((uint32_t)(uintptr_t)lv_obj_get_user_data(r));
        if (!e) continue;
        qp_note_meta(b, sizeof b, e);
        lv_obj_t *meta = lv_obj_get_child(r, 2);
        if (strcmp(lv_label_get_text(meta), b)) lv_label_set_text(meta, b);
    }
}
/* Read: a row in sight, most of it, for NOTE_SEEN_MS with the panel all the
 * way open. One scrolled out of sight starts again. */
static void qp_notes_seen(void)
{
    if (!qp_notes || !qp_open || lv_obj_get_style_translate_y(qp, 0) != 0 || lv_anim_get(qp, NULL)) return;
    lv_area_t view;                     /* the part of the panel on screen */
    lv_obj_get_coords(qp_scroll, &view);
    bool changed = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(qp_notes_rows); i++)
    {
        lv_obj_t *r = lv_obj_get_child(qp_notes_rows, i);
        struct note *e = note_find((uint32_t)(uintptr_t)lv_obj_get_user_data(r));
        if (!e || e->read) continue;
        lv_area_t a;
        lv_obj_get_coords(r, &a);
        int top = a.y1 > view.y1 ? a.y1 : view.y1, bottom = a.y2 < view.y2 ? a.y2 : view.y2;
        bool in_sight = bottom - top >= lv_area_get_height(&a) * 3 / 5;
        if (!in_sight) { e->seen_at = 0; continue; }
        if (!e->seen_at) { e->seen_at = lv_tick_get() ? lv_tick_get() : 1; continue; }
        if (lv_tick_elaps(e->seen_at) < NOTE_SEEN_MS) continue;
        note_mark_read(e);
        lv_obj_add_flag(lv_obj_get_child(r, 0), LV_OBJ_FLAG_HIDDEN);
        changed = true;
    }
    if (changed) qp_notes_count();
}
/* the history changed: shown at once while the panel is out */
static void qp_notes_refresh(void)
{
    qp_notes_stale = true;
    if (qp_is_visible()) qp_notes_build_rows();
}

/* the text size's face for slot i of sl_fonts, or the biggest smaller one
 * no taller than h */
static const lv_font_t *qp_font_fit(int i, int h)
{
    for (int step = sl_text_step; step > 0; step--)
        if (lv_font_get_line_height(sl_fonts[step][i]) <= h) return sl_fonts[step][i];
    return sl_fonts[0][i];
}
static void qp_build(void)
{
    memset(qp_tile_state, -1, sizeof qp_tile_state);
    qp_bd = box(lv_layer_top());
    lv_obj_set_size(qp_bd, W, H);
    lv_obj_add_event_cb(qp_bd, qp_bd_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(qp_bd, qp_cover_cb, LV_EVENT_COVER_CHECK, NULL);
    qp = box(lv_layer_top());
    lv_obj_set_size(qp, W, H);
    lv_obj_set_style_bg_color(qp, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_color(qp, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(qp, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(qp, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(qp, 14, 0);
    lv_obj_add_flag(qp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(qp, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(qp, qp_drag_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(qp, qp_glass_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    if (qp_glass_ok) lv_obj_set_style_bg_opa(qp, LV_OPA_TRANSP, 0);
    /* the glass's lower edge catches the light as it slides */
    lv_obj_set_style_border_side(qp, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(qp, 1, 0);
    lv_obj_set_style_border_color(qp, lv_color_white(), 0);
    lv_obj_set_style_border_opa(qp, 90, 0);

    qp_scroll = lv_obj_create(qp);
    lv_obj_remove_style_all(qp_scroll);
    /* it ends above the handle's strip, so a note scrolling past never runs
     * under the handle; the last one stops where it always did */
    lv_obj_set_size(qp_scroll, lv_pct(100), H - QP_GRAB_STRIP);
    lv_obj_set_style_pad_bottom(qp_scroll, 46 - QP_GRAB_STRIP, 0);
    lv_obj_set_scroll_dir(qp_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(qp_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(qp_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(qp_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);

    qp_clock = text(qp_scroll, "", &ss_bold_28, 0xffffff);
    lv_obj_set_style_text_font(qp_clock, &sl_sym_48, 0);
    lv_obj_set_pos(qp_clock, 10, 18);
    /* the panel's own words grow with the text size only as far as they
     * fit: bigger, the battery ran into Edit and Power and the heading into
     * the card; the slider names stay as they are, or sit on the knobs */
    qp_date = text(qp_scroll, "", qp_font_fit(3, 70 - 44), C_SUB);
    lv_obj_align(qp_date, LV_ALIGN_TOP_RIGHT, -10, 44);
    qp_head = text(qp_scroll, sl_zune() ? sl_case("Quick access") : "QUICK ACCESS", sl_zune() ? &ss_regular_23 : qp_font_fit(2, 108 - 80), C_SUB);
    lv_obj_set_pos(qp_head, 10, sl_zune() ? 76 : 80);   /* Zule's big words: their tails clear the card */

    /* Edit and Power, by the heading: only on the grown panel */
    qx_edit = icon_btn(qp_scroll, LV_SYMBOL_EDIT, 36, qx_btn_cb, (void *)0);
    qx_power = icon_btn(qp_scroll, LV_SYMBOL_POWER, 36, qx_btn_cb, (void *)1);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *b = i ? qx_power : qx_edit;
        qp_glass(b);
        lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_FULL, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &sl_sym_18, 0);
        lv_obj_set_pos(b, W - 28 - 36 - (1 - i) * 46, 70);
        lv_obj_set_style_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *tp = qp_tp = box(qp_scroll);
    lv_obj_set_size(tp, W - 28, QX_TP_H);
    lv_obj_set_pos(tp, 0, 108);
    qp_glass(tp);
    const char *const *syms = qp_fn_syms;
    qx_px = 0;
    qp_n = qp_fn_parse(qp_fn, &qp_total);
    /* the grown card: the rows a page holds past the folded two, and the
     * dots' band when there is more than a page */
    qx_pages = (qp_total + QP_PAGE - 1) / QP_PAGE;
    qx_page = 0;
    qx_grow = (LV_CLAMP(2, (qp_total + 3) / 4, QP_PAGE / 4) - 2) * QX_ROW + (qx_pages > 1 ? QX_DOTS : 0);
    qx_dots = NULL;
    for (int k = 0; k < qp_total; k++)
    {
        int f = qp_fn[k], slot = k % QP_PAGE;
        lv_obj_t *t = box(tp);
        lv_obj_set_size(t, QP_TILE_W, QP_TILE_H);
        lv_obj_set_pos(t, k / QP_PAGE * QX_PAGE_W + (slot % 4) * QP_TILE_W, 6 + (slot / 4) * QP_TILE_H);
        lv_obj_t *cc = box(t);                           /* child 0: circle */
        lv_obj_set_size(cc, 72, 72);
        lv_obj_align(cc, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(cc, sl_zune() ? 0 : SL_R_FULL, 0);
        lv_obj_set_style_bg_opa(cc, 100, 0);
        lv_obj_center(text(cc, syms[f], &sl_sym_28, 0xffffff));
        if (f == 14) lv_obj_add_event_cb(cc, qx_dark_icon_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        /* fewer than 8 chosen: the rest of the first rows come with the grown card */
        if (k >= qp_n && k < 8) { lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN); lv_obj_set_style_opa(t, LV_OPA_TRANSP, 0); }
        qp_tile_lbl[k] = text(t, "", &ss_semibold_18, C_SUB);
        lv_obj_align(qp_tile_lbl[k], LV_ALIGN_BOTTOM_MID, 0, -6);
        on_click(t, qp_tile_press, (void *)(intptr_t)f);
        lv_obj_add_event_cb(t, qp_tile_pressed_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(t, qp_tile_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)f);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_transform_scale(cc, 235, LV_STATE_PRESSED);
        qp_tiles[k] = t;
    }
    if (qx_pages > 1)
    {
        /* the page dots, under the tiles: on the panel, not the card, so
         * they stay put as it pages; shown as it grows (qx_set) */
        lv_obj_set_scrollbar_mode(tp, LV_SCROLLBAR_MODE_OFF);
        qx_dots = box(qp_scroll);
        lv_obj_set_size(qx_dots, qx_pages * QX_DOT + (qx_pages - 1) * 8, QX_DOT);
        lv_obj_set_x(qx_dots, (QX_PAGE_W - lv_obj_get_style_width(qx_dots, 0)) / 2);
        for (int i = 0; i < qx_pages; i++)
        {
            lv_obj_t *d = box(qx_dots);
            lv_obj_set_size(d, QX_DOT, QX_DOT);
            lv_obj_set_pos(d, i * (QX_DOT + 8), 0);
            lv_obj_set_style_radius(d, sl_zune() ? 0 : LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(sl_theme() == 1 ? 0x182735 : 0xffffff), 0);
        }
        lv_obj_add_flag(qx_dots, LV_OBJ_FLAG_HIDDEN);
        qx_dots_mark();
    }

    lv_obj_t *sp = qp_sp = box(qp_scroll);
    lv_obj_set_size(sp, W - 28, 140);
    lv_obj_set_pos(sp, 0, 356);
    qp_glass(sp);
    lv_obj_t *sun = sl_icon(sp, IC_SUN, 28, 0xffffff, 0x000000);
    lv_obj_set_pos(sun, 18, 24);
    lv_obj_set_pos(qp_cap[0] = text(sp, sl_zune() ? sl_case("Brightness") : "BRIGHTNESS", sl_fonts[0][0], C_SUB), 70, 8);
    qp_bright = lv_slider_create(sp);
    lv_obj_set_size(qp_bright, W - 130, 10);
    lv_obj_set_pos(qp_bright, 70, 34);
    /* below 1, the night veil (sl_night_apply): the same track, a few steps longer */
    lv_slider_set_range(qp_bright, MIN_BRIGHTNESS_SETTING - SL_NIGHT_STEPS, MAX_BRIGHTNESS_SETTING);
    style_slider(qp_bright);
    qp_slider_glass(qp_bright);
    lv_slider_set_value(qp_bright, sl_bright_value(), LV_ANIM_OFF);
    lv_obj_add_event_cb(qp_bright, qp_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(qp_bright, qp_bright_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *spk = text(sp, LV_SYMBOL_VOLUME_MAX, &sl_sym_24, 0xffffff);
    lv_obj_set_pos(spk, 20, 92);
    lv_obj_set_pos(qp_cap[1] = text(sp, sl_zune() ? sl_case("Volume") : "VOLUME", sl_fonts[0][0], C_SUB), 70, 74);
    qp_vol = lv_slider_create(sp);
    lv_obj_set_size(qp_vol, W - 130, 10);
    lv_obj_set_pos(qp_vol, 70, 100);
    lv_slider_set_range(qp_vol, 0, 100);
    style_slider(qp_vol);
    qp_slider_glass(qp_vol);
    lv_obj_add_event_cb(qp_vol, qp_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *np_c = qp_np_card = box(qp_scroll);
    lv_obj_set_size(np_c, W - 28, 104);
    lv_obj_set_pos(np_c, 0, 508);
    qp_glass(np_c);
    on_click(np_c, qp_np_cb, (void *)0);
    lv_obj_remove_flag(np_c, LV_OBJ_FLAG_EVENT_BUBBLE);
    qp_np_t = text(np_c, "", &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(qp_np_t, 20, 22);
    lv_obj_set_width(qp_np_t, W - 240);
    label_lines(qp_np_t, 1);
    qp_np_a = text(np_c, "", &ss_regular_16, C_SUB);
    lv_obj_set_pos(qp_np_a, 20, 54);
    lv_obj_set_width(qp_np_a, W - 240);
    label_lines(qp_np_a, 1);
    lv_obj_t *nx = icon_btn(np_c, LV_SYMBOL_NEXT, 52, qp_np_cb, (void *)3);
    lv_obj_align(nx, LV_ALIGN_RIGHT_MID, -12, 0);
    qp_np_pp = icon_btn(np_c, LV_SYMBOL_PLAY, 60, qp_np_cb, (void *)2);
    lv_obj_set_style_bg_color(qp_np_pp, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qp_np_pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(qp_np_pp, 0), lv_color_hex(sl_accent_on(0xffffff, 30)), 0);   /* 3:1 on the white disc */
    lv_obj_align(qp_np_pp, LV_ALIGN_RIGHT_MID, -72, 0);
    lv_obj_t *pv = icon_btn(np_c, LV_SYMBOL_PREV, 52, qp_np_cb, (void *)1);
    lv_obj_align(pv, LV_ALIGN_RIGHT_MID, -140, 0);
    qp_np_btn[0] = pv; qp_np_btn[1] = qp_np_pp; qp_np_btn[2] = nx;
    qp_np_idle = -1;

    /* Notifications, from here down, as long as the list is: your pet's
     * first, when you have one, then the history, newest first. They
     * scroll with the rest of the panel. */
    qp_notes = box(qp_scroll);
    lv_obj_set_size(qp_notes, W - 28, LV_SIZE_CONTENT);
    lv_obj_set_pos(qp_notes, 0, 626);
    lv_obj_set_flex_flow(qp_notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(qp_notes, 8, 0);
    lv_obj_set_style_pad_bottom(qp_notes, 12, 0);

    /* notification: your pet misses you */
    qp_pet = box(qp_notes);
    lv_obj_set_size(qp_pet, lv_pct(100), 86);
    lv_obj_set_style_flex_grow(qp_pet, 0, 0);
    qp_glass(qp_pet);
    lv_obj_set_style_radius(qp_pet, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(qp_pet, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(qp_pet, 70, LV_STATE_PRESSED);
    on_click(qp_pet, qp_pet_cb, NULL);
    lv_obj_remove_flag(qp_pet, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *lcd = box(qp_pet);
    lv_obj_set_size(lcd, 62, 62);
    lv_obj_align(lcd, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(lcd, SL_R_MD, 0);
    lv_obj_set_style_bg_color(lcd, lv_color_hex(PET_LCD), 0);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, lv_color_hex(PET_DARK), 0);
    qp_pet_canvas = lv_canvas_create(lcd);
    qp_pet_drawn = -1;
    lv_obj_center(qp_pet_canvas);
    lv_obj_t *app = text(qp_pet, sl_zune() ? "sloppy gotchi  ·  now" : "SLOPPY GOTCHI  ·  NOW", &ss_regular_16, C_SUB);
    lv_obj_set_pos(app, 88, 10);
    qp_pet_name = text(qp_pet, "", &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(qp_pet_name, 88, 30);
    lv_obj_set_width(qp_pet_name, W - 28 - 104);
    label_lines(qp_pet_name, 1);
    qp_pet_line = text(qp_pet, "and misses you", &ss_regular_18, C_SUB);
    lv_obj_set_pos(qp_pet_line, 88, 56);
    lv_obj_add_flag(qp_pet, LV_OBJ_FLAG_HIDDEN);

    /* a heading with Clear all, the notes, and words for when there are none */
    qp_notes_head = box(qp_notes);
    lv_obj_set_size(qp_notes_head, lv_pct(100), 40);
    lv_obj_t *nt = text(qp_notes_head, sl_zune() ? sl_case("Notifications") : "NOTIFICATIONS", sl_zune() ? &ss_regular_23 : &ss_semibold_18, C_SUB);
    lv_obj_align(nt, LV_ALIGN_LEFT_MID, 10, 0);
    qp_notes_new = text(qp_notes_head, "", &ss_semibold_18, C_ACCENT);
    lv_obj_align_to(qp_notes_new, nt, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_t *clr = box(qp_notes_head);
    lv_obj_set_size(clr, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(clr, 14, 0);
    lv_obj_set_style_radius(clr, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_align(clr, LV_ALIGN_RIGHT_MID, 0, 0);
    qp_glass(clr);
    lv_obj_center(text(clr, "Clear all", &ss_semibold_18, 0xffffff));   /* the theme's ink, as the rest of the panel */
    lv_obj_set_style_bg_color(clr, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(clr, 70, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(clr, 8);
    on_click(clr, qp_note_clear_all_cb, NULL);
    lv_obj_remove_flag(clr, LV_OBJ_FLAG_EVENT_BUBBLE);
    qp_notes_rows = box(qp_notes);
    lv_obj_set_size(qp_notes_rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qp_notes_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(qp_notes_rows, 8, 0);
    /* none: a bell and the words, quietly */
    qp_notes_empty = box(qp_notes);
    lv_obj_set_size(qp_notes_empty, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(qp_notes_empty, 12, 0);
    lv_obj_set_flex_flow(qp_notes_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qp_notes_empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qp_notes_empty, 4, 0);
    lv_obj_t *bell = lv_label_create(qp_notes_empty);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bell, &sl_sym_24, 0);
    lv_obj_set_style_text_color(bell, lv_color_hex(C_SUB), 0);
    text(qp_notes_empty, "No notifications", &ss_regular_18, C_SUB);
    qp_notes_stale = true;

    lv_obj_t *grab = qp_grab = box(qp);
    lv_obj_set_size(grab, 64, 6);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(grab, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_50, 0);
}
static void quick_panel_rebuild(void)
{
    if (!qp) return;
    lv_anim_delete(qp, NULL);
    lv_obj_delete(qp);
    lv_obj_delete(qp_bd);
    qp = qp_bd = qp_grab = NULL;
    qp_open = false;
    qp_after_close = NULL;
    qp_build();
    qp_refresh();
}
/* Build the hidden controls after the home entrance has settled, so the
 * first pull does not have to allocate and lay out the whole panel. */
static void qp_warmup(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    if (!qp) { qp_build(); qp_refresh(); }
    /* the glass ahead of the first pull, with room for it only */
    if (!qp_glass_px && !qp_sharp_px && sl_mem_room(W * H * 4 / 1024, "the glass, ahead"))
    {
        qp_glass_px = malloc((size_t)W * H * 2);
        qp_sharp_px = malloc((size_t)W * H * 2);
    }
    sl_perf_memory("home warmed");
}
/* A touch at the top edge may become a pull. The capture is most of its
 * first frame (47 ms on the R1); taken while the finger travels the 12 px
 * before the pull is decided, the panel moves on the next frame. Not while
 * something animates: that frame would hitch instead. */
static void qp_glass_early(void)
{
    if (qp_open || (qp && !lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN)) || lv_anim_count_running() ||
        lv_display_get_rotation(disp) != LV_DISPLAY_ROTATION_0)
        return;
    qp_glass_capture();
}
static void qp_drag_to(int dy)
{
    if (!qp) qp_build();
    if (!qp_open && lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN))
    {
        sl_perf_begin("panel pull");
        qp_long_consumed = false;
        /* Before the panel is drawn over the screen. The glass from the
         * touch-down will do unless much was drawn since: a progress bar
         * or the clock vanish in the blur. The sharp copy, shown as it is
         * below the panel, is always the screen now. */
        unsigned long since = sl_flushed_px - qp_glass_flushed;
        if (!qp_glass_ok || since > W * H / 20 || sl_ms() - qp_glass_at > 1000)
            qp_glass_capture();
        else if (since)
        {
            const uint16_t *screen = SL_SCREEN_PIXELS();
            if (screen) qp_sharp_copy(screen); else qp_glass_capture();
        }
        nbanner_hide_now();                 /* its notifications are listed here */
        notes_unseen();
        qp_refresh();
        /* Shown where the finger is: LVGL would first invalidate it where it
         * last stood (the whole screen, a hidden object keeps old coordinates)
         * and redraw all of it in the first frame of the pull. */
        lv_display_enable_invalidation(disp, false);
        lv_obj_remove_flag(qp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(qp, dy - H, 0);
        lv_obj_scroll_to_y(qp_scroll, 0, LV_ANIM_OFF);  /* it comes down at the top */
        lv_obj_update_layout(qp);
        lv_display_enable_invalidation(disp, true);
        lv_obj_invalidate(qp);
        /* The panel and its backdrop go above everything in the top layer
         * but the developer overlay. Any reorder there redraws the whole
         * screen (LVGL invalidates the parent), so only when needed. */
        int32_t n = (int32_t)lv_obj_get_child_count(lv_layer_top()), i = lv_obj_get_index(qp) + 1;
        bool raise = lv_obj_get_index(qp_bd) + 1 != lv_obj_get_index(qp);
        for (; i < n && !raise; i++) raise = lv_obj_get_child(lv_layer_top(), i) != dev_hud.bar;
        if (raise)
        {
            lv_obj_move_foreground(qp_bd);
            lv_obj_move_foreground(qp);
            if (dev_hud.bar) lv_obj_move_foreground(dev_hud.bar);
        }
        /* Slide the live controls without adding a full-panel snapshot to
         * the first finger-driven frame. */
    }
    lv_anim_delete(qp, NULL);
    if (dy > H) dy = H;
    if (dy < 0) dy = 0;
    set_ty(qp, dy - H);                  /* fully down and growing: not redrawn for nothing */
}
/* The ease-out's last frames creep a few pixels each, and every one of them
 * repaints the whole screen under the panel: land them in one. */
static void qp_set_ty(void *o, int32_t v) { set_ty(o, v > -6 ? 0 : v); }
static void qp_release(int dy, int vy)
{
    if (dy < 0) dy = 0;
    if (dy > H) dy = H;
    if (dy > H / 3 || vy > 500)
    {
        if (!qp_open) sfx_play(SFX_UI_PANEL_OPEN);
        qp_open = true;
        qp_after_close = NULL;
        anim_prop(qp, qp_set_ty, dy - H, 0, 80 + 160 * (H - dy) / H, qp_settled_cb);
        /* Defer card logging until the animation has settled. */
    }
    else qp_close_to(dy - H);
}

static int sl_overlay_place(lv_obj_t *o, int top);   /* sloppy_search.inc: the rule for toasts and banners */
static bool overlay_visible(void)
{
    return np_open || uq_open || acts_open_now || qp_open || pl_visible() || dbu_showing() ||
           (qp && !lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN)) ||
           (uq && !lv_obj_has_flag(uq, LV_OBJ_FLAG_HIDDEN)) ||
           (acts && !lv_obj_has_flag(acts, LV_OBJ_FLAG_HIDDEN));
}

/* ------------------------------------------------------ picking songs */
/* Hold a song in a list to pick it, then tap more: each picked row is
 * marked, and a bar at the foot plays them, queues them or loves them all
 * at once. The marks are kept by track, so the list's rows can come and go
 * under them as it scrolls. Albums and artists are picked the same way,
 * from their sheet or the play menu: marked by album, or by an artist's
 * first place in albums_by_artist, and the bar acts on all their songs. */
static uint32_t *sel_bits;
static int sel_count, sel_kind, sel_n;    /* sel_n: what a mark can be, tracks or albums */
static void *sel_view;                   /* the song list, the album grid, or the Artists page's body */
static lv_obj_t *sel_bar, *sel_label, *sel_all;

static bool sel_on(void) { return sel_bar != NULL; }
static bool sel_kind_on(int kind) { return sel_bar && sel_kind == kind; }
static bool sel_picking(lv_obj_t *host) { return sel_kind_on(SEL_SONGS) && host && lv_obj_get_user_data(host) == sel_view; }
static bool sel_has(int t) { return sel_bits && t >= 0 && t < sel_n && (sel_bits[t / 32] >> (t % 32) & 1); }
static void sel_set(int t, bool on)
{
    if (on) sel_bits[t / 32] |= 1u << (t % 32);
    else sel_bits[t / 32] &= ~(1u << (t % 32));
}
static void sel_paint_row(lv_obj_t *r, bool on)
{
    lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(r, on ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++)
    {
        lv_obj_t *c = lv_obj_get_child(r, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_USER_4)) continue;
        lv_obj_set_style_bg_opa(c, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, on ? 0 : 2, 0);
        hidden_if_changed(lv_obj_get_child(c, 0), !on);
    }
}
/* the mark: a ring, filled with a tick in the accent once picked */
static lv_obj_t *sel_ring(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, 30, 30);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_DIM), 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_USER_4);
    uint32_t ink = sl_accent_ink() == 0xffffff ? 0xfefefe : sl_accent_ink();
    lv_obj_center(text(c, LV_SYMBOL_OK, &sl_sym_18, ink));
    return c;
}
static void sel_mark(lv_obj_t *r, int t)
{
    sl_align(sel_ring(r), LV_ALIGN_RIGHT_MID, -14, 0);   /* where the dots were, either side */
    sel_paint_row(r, sel_has(t));
}
/* an accent edge w px wide inside a cover, drawn over its art; the
 * padding gives back what the edge takes, so nothing in it moves */
static void cover_edge(lv_obj_t *art, int w)
{
    lv_obj_set_style_border_color(art, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_post(art, true, 0);
    lv_obj_set_style_border_width(art, w, 0);
    lv_obj_set_style_border_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(art, -w, 0);
}
/* an album: the ring in the cover's top corner, white-edged to read on any
 * art; picked, the cover wears an accent edge */
static void sel_paint_cell(lv_obj_t *art, bool on)
{
    cover_edge(art, on ? 3 : 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(art); i++)
    {
        lv_obj_t *c = lv_obj_get_child(art, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_USER_4)) continue;
        lv_obj_set_style_bg_color(c, on ? lv_color_hex(C_ACCENT) : lv_color_black(), 0);
        lv_obj_set_style_bg_opa(c, on ? LV_OPA_COVER : LV_OPA_30, 0);
        hidden_if_changed(lv_obj_get_child(c, 0), !on);
    }
}
static void sel_cell_mark(lv_obj_t *art, int a)
{
    lv_obj_t *c = sel_ring(art);
    lv_obj_set_style_border_color(c, lv_color_white(), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -8, 8);
    sel_paint_cell(art, sel_has(a));
}
/* an artist's card: the ring where its chevron was; picked, an accent edge */
static void sel_paint_artist(lv_obj_t *r, bool on)
{
    /* an outline inside the card, over its hairline: the words stay put */
    lv_obj_set_style_outline_color(r, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_outline_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(r, on ? 2 : 0, 0);
    lv_obj_set_style_outline_pad(r, -2, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++)
    {
        lv_obj_t *c = lv_obj_get_child(r, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_USER_4)) continue;
        lv_obj_set_style_bg_opa(c, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, on ? 0 : 2, 0);
        hidden_if_changed(lv_obj_get_child(c, 0), !on);
    }
}
static void sel_artist_mark(lv_obj_t *r, int k0)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++)   /* the chevron steps aside */
    {
        lv_obj_t *c = lv_obj_get_child(r, i);
        if (lv_obj_check_type(c, &lv_label_class) && !strcmp(lv_label_get_text(c), sl_chevron()))
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    sl_align(sel_ring(r), LV_ALIGN_RIGHT_MID, -14, 0);
    sel_paint_artist(r, sel_has(k0));
}
/* Files and the Playlists page pick too: sloppy_files.inc, sloppy_browse.inc */
static bool fm_rebind_view(void *p);
static int fm_sel_total(void);
static void fm_sel_act(int which);
static void pls_sel_repaint(void *body, bool marks);
static int pls_sel_total(void);
static void pls_sel_act(int which);
/* and a podcast's episodes: sloppy_podcast.inc */
static int pod_sel_total(void);
static void pod_sel_act(int which);
static void pod_sel_repaint(void *view, bool marks);
static void sel_refresh_async(void *p)
{
    if (fm_rebind_view(p)) return;
    if (p == album_view)
    {
        struct album_grid_view *g = p;
        for (int i = 0; i < ALBUM_VSLOTS; i++) g->indexes[i] = -1;   /* the cells are made again, marked or not */
        lv_obj_send_event(g->body, LV_EVENT_SCROLL, NULL);
        return;
    }
    struct song_list_view *v = p;
    for (int i = 0; i < 14; i++) v->indexes[i] = -1;   /* the rows are made again, marked or not */
    lv_obj_send_event(v->body, LV_EVENT_SCROLL, NULL);
}
/* the Artists page's rows, marked or not (its rows all stay) */
static void sel_artists_repaint(lv_obj_t *body, bool marks)
{
    if (artists_view && artists_view->body == body) body = artists_view->host;   /* Album artists: the rows made so far */
    for (uint32_t i = 0; body && i < lv_obj_get_child_count(body); i++)
    {
        lv_obj_t *r = lv_obj_get_child(body, i);
        int k = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
        if (k < 0) continue;
        bool has = false;
        for (uint32_t j = 0; j < lv_obj_get_child_count(r); j++)
        {
            lv_obj_t *c = lv_obj_get_child(r, j);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_4)) { has = true; if (!marks) { lv_obj_delete(c); j--; } }
            else if (!marks && lv_obj_check_type(c, &lv_label_class)) lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
        if (!marks) { sel_paint_artist(r, false); continue; }
        if (has) sel_paint_artist(r, sel_has(k)); else sel_artist_mark(r, k);
    }
}
static void sel_mini_async(void *p) { (void)p; mini_update(); }
static void sel_clear(void)
{
    sel_bar = sel_all = NULL;
    sel_view = NULL;
    free(sel_bits);
    sel_bits = NULL;
    sel_count = 0;
}
/* the list's page went away, and the bar with it */
static void sel_bar_deleted(lv_event_t *e)
{
    (void)e;
    sel_clear();
    lv_async_call(sel_mini_async, NULL);
}
static void sel_forget(void *view)
{
    lv_async_call_cancel(sel_refresh_async, view);
    if (sel_view == view) sel_view = NULL;
}
static void sel_end(void)
{
    if (!sel_bar) return;
    lv_obj_t *b = sel_bar;
    void *v = sel_view;
    int kind = sel_kind;
    az_sleep(lv_obj_get_parent(b), false);
    lv_obj_remove_event_cb(b, sel_bar_deleted);
    sel_clear();
    lv_obj_delete_delayed(b, 0);         /* its own button may be calling; goes with the page if that goes first */
    if (v && kind == SEL_ARTISTS) sel_artists_repaint(v, false);
    else if (v && kind == SEL_PLAYLISTS) pls_sel_repaint(v, false);
    else if (v && kind == SEL_EPISODES) pod_sel_repaint(v, false);
    else if (v) lv_async_call(sel_refresh_async, v);
    mini_update();
}
/* how many there are to pick: songs in the list, albums, or artists */
static int sel_total(void)
{
    if (sel_kind == SEL_SONGS) return sel_view ? ((struct song_list_view *)sel_view)->count : 0;
    if (sel_kind == SEL_ALBUMS) return sl_lib.n_albums;
    if (sel_kind == SEL_FILES) return fm_sel_total();
    if (sel_kind == SEL_PLAYLISTS) return pls_sel_total();
    if (sel_kind == SEL_EPISODES) return pod_sel_total();
    int n = 0;
    for (int k = 0; k < sl_lib.n_albums; k += artist_span(k)) n++;
    return n;
}
static void sel_count_label(void)
{
    char b[48];
    if (sel_kind == SEL_ALBUMS) sl_count(b, sizeof b, sel_count, "%s album", "%s albums");
    else if (sel_kind == SEL_ARTISTS) sl_count(b, sizeof b, sel_count, "%s artist", "%s artists");
    else if (sel_kind == SEL_FILES) sl_count(b, sizeof b, sel_count, "%s item", "%s items");
    else if (sel_kind == SEL_PLAYLISTS) sl_count(b, sizeof b, sel_count, "%s playlist", "%s playlists");
    else if (sel_kind == SEL_EPISODES) sl_count(b, sizeof b, sel_count, "%s episode", "%s episodes");
    else sl_count(b, sizeof b, sel_count, "%s song", "%s songs");
    lv_label_set_text(sel_label, sl_case(b));
    bool all = sel_view && sel_count >= sel_total();
    const char *w = all ? "Deselect all" : "Select all";
    label_if_changed(sel_all, sl_zune() ? sl_case(w) : sl_tr(w));
}
/* Select all: everything there is; again, none, which ends picking */
static void sel_all_cb(lv_event_t *e)
{
    (void)e;
    void *v = sel_view;
    if (!v) return;
    if (sel_count >= sel_total()) { sfx_play(SFX_UI_OFF); sel_end(); return; }
    if (sel_kind == SEL_SONGS)
    {
        struct song_list_view *l = v;
        for (int i = 0; i < l->count; i++)
            if (!sel_has(l->tracks[i])) { sel_set(l->tracks[i], true); sel_count++; }
    }
    else if (sel_kind == SEL_FILES || sel_kind == SEL_PLAYLISTS || sel_kind == SEL_EPISODES)
    {
        for (int k = 0; k < sel_n; k++) if (!sel_has(k)) { sel_set(k, true); sel_count++; }
    }
    else for (int k = 0; k < sl_lib.n_albums; k += sel_kind == SEL_ARTISTS ? artist_span(k) : 1)
        if (!sel_has(k)) { sel_set(k, true); sel_count++; }
    sfx_play(SFX_UI_ON);
    sel_count_label();
    if (sel_kind == SEL_ARTISTS) sel_artists_repaint(v, true);
    else if (sel_kind == SEL_PLAYLISTS) pls_sel_repaint(v, true);
    else if (sel_kind == SEL_EPISODES) pod_sel_repaint(v, true);
    else lv_async_call(sel_refresh_async, v);
}
/* the picked songs in the list's order (albums as the grid shows them,
 * artists as their list does, each album whole); the caller frees it */
static int sel_list(int **out)
{
    if (sel_kind != SEL_SONGS)
    {
        const int *order = sel_kind == SEL_ALBUMS ? albums_order() : sl_lib.albums_by_artist;
        int *albums = malloc((sl_lib.n_albums ? sl_lib.n_albums : 1) * sizeof *albums), n = 0;
        *out = NULL;
        if (!albums) return 0;
        for (int k = 0; k < sl_lib.n_albums; k++)
        {
            if (sel_kind == SEL_ALBUMS) { if (sel_has(order[k])) albums[n++] = order[k]; continue; }
            int span = artist_span(k);
            if (sel_has(k)) for (int i = 0; i < span; i++) albums[n++] = order[k + i];
            k += span - 1;
        }
        int total = albums_tracks(albums, n, out);
        free(albums);
        return total;
    }
    struct song_list_view *v = sel_view;
    int n = 0, *l = malloc((sel_count ? sel_count : 1) * sizeof *l);
    for (int i = 0; l && v && i < v->count && n < sel_count; i++)
        if (sel_has(v->tracks[i])) l[n++] = v->tracks[i];
    *out = l;
    return l ? n : 0;
}
static void sel_play_cb(lv_event_t *e)
{
    (void)e;
    if (sel_kind == SEL_FILES) { fm_sel_act(0); return; }
    if (sel_kind == SEL_PLAYLISTS) { pls_sel_act(0); return; }
    if (sel_kind == SEL_EPISODES) { pod_sel_act(0); return; }
    int *l, n = sel_list(&l);
    if (n && play_list(l, n, 0, false)) np_show_auto();
    free(l);
    sel_end();
}
/* songs onto the end of the queue, or played when nothing is */
static void q_add_many(const int *l, int n)
{
    if (n <= 0) return;
    if (!something_playing()) { if (play_list(l, n, 0, false)) np_show_auto(); return; }
    int ok = 0;
    char path[MAX_PATH];
    for (int i = 0; i < n; i++) if (q_insert(sl_track_path(l[i], path, sizeof path), false, true) >= 0) ok++;
    char b[128];
    snprintf(b, sizeof b, sl_tr(ok == 1 ? "Added %d song to the queue" : "Added %d songs to the queue"), ok);
    ok == n ? toast(b) : toast_warn("Could not queue every song");
    if (uq_open) uq_rebuild_soon();
}
static void sel_queue_cb(lv_event_t *e)
{
    (void)e;
    if (sel_kind == SEL_FILES) { fm_sel_act(1); return; }
    if (sel_kind == SEL_PLAYLISTS) { pls_sel_act(1); return; }
    int *l, n = sel_list(&l);
    q_add_many(l, n);
    free(l);
    sel_end();
}
static void sel_love_cb(lv_event_t *e)
{
    (void)e;
    int *l, n = sel_list(&l);
    for (int i = 0; i < n; i++) ts_love(l[i], true);
    free(l);
    char b[96];
    snprintf(b, sizeof b, sl_tr(n == 1 ? "Loved %d song" : "Loved %d songs"), n);
    toast(b);
    sel_end();
}
static void sel_playlist_cb(lv_event_t *e)
{
    (void)e;
    if (sel_kind == SEL_FILES) { fm_sel_act(2); return; }
    int *l, n = sel_list(&l);
    if (n) pl_open_many(l, n);
    free(l);
    sel_end();
}
static void sel_close_cb(lv_event_t *e) { (void)e; sel_end(); }
static void sel_more_cb(lv_event_t *e) { (void)e; if (sel_kind == SEL_FILES) fm_sel_act(3); }
static void sel_delete_cb(lv_event_t *e) { (void)e; if (sel_kind == SEL_PLAYLISTS) pls_sel_act(2); else if (sel_kind == SEL_EPISODES) pod_sel_act(2); }
static void sel_pod_get_cb(lv_event_t *e) { (void)e; if (sel_kind == SEL_EPISODES) pod_sel_act(1); }
static void sel_pod_heard_cb(lv_event_t *e) { (void)e; if (sel_kind == SEL_EPISODES) pod_sel_act(3); }
/* a round button on the bar: the accent for Play, quiet for the rest */
/* the bar's five 52 px buttons from the right, 58 apart; the count and
 * Select all have the room left of the play button */
#define SEL_PITCH 58
#define SEL_WORDS_W (W - 2 * SL_FLOAT - SL_INSET - (12 + 4 * SEL_PITCH) - 52 - SL_SP_S)
static lv_obj_t *sel_btn(const char *sym, int icon, bool primary, lv_event_cb_t cb, int x)
{
    lv_obj_t *b = box(sel_bar);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CARD_P), 0);
    lv_obj_set_style_bg_color(b, primary ? lv_color_darken(lv_color_hex(C_ACCENT), 50)
                                         : lv_color_lighten(lv_color_hex(C_CARD_P), 30), LV_STATE_PRESSED);
    press_motion(b);
    press_sink(b, 52, 52);
    uint32_t ink = primary ? (sl_accent_ink() == 0xffffff ? 0xfefefe : sl_accent_ink()) : 0xffffff;
    if (sym) lv_obj_center(text(b, sym, &sl_sym_22, ink));
    else lv_obj_center(sl_icon(b, icon, 26, ink == 0xffffff ? C_TEXT : ink, 0x000000));
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, x, 0);
    on_click(b, cb, NULL);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}
/* Picking starts: what kind, in which view, on which page; first is the
 * one already picked (-1 none, from the play menu's Select) */
static void sel_start(int kind, void *view, lv_obj_t *page, int first)
{
    if (!view || sel_on()) return;
    sel_kind = kind;
    sel_view = view;
    sel_n = kind == SEL_SONGS ? sl_lib.n_tracks : kind == SEL_FILES ? fm_sel_total() :
            kind == SEL_PLAYLISTS ? pls_sel_total() : kind == SEL_EPISODES ? pod_sel_total() : sl_lib.n_albums;
    sel_view = NULL;
    if (first >= sel_n) return;
    sel_bits = calloc(sel_n / 32 + 1, sizeof *sel_bits);
    if (!sel_bits) { toast_warn(kind == SEL_SONGS ? "Not enough memory to pick songs" : "Not enough memory to pick"); return; }
    sel_kind = kind;
    sel_view = view;
    sel_count = 0;
    sl_doing_is("picking", NULL);
    if (first >= 0) { sel_set(first, true); sel_count = 1; }
    sfx_play(SFX_UI_ON);
    /* the bar sits where the mini player does, which steps aside for it */
    sel_bar = box(page);
    lv_obj_set_size(sel_bar, W - 2 * SL_FLOAT, 82);   /* the mini player's place and shape */
    lv_obj_align(sel_bar, LV_ALIGN_BOTTOM_MID, 0, -SL_FLOAT);
    lv_obj_set_style_radius(sel_bar, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(sel_bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(sel_bar, LV_OPA_COVER, 0);
    hairline(sel_bar);
    sl_elevate(sel_bar, SL_E3, 0);
    lv_obj_add_event_cb(sel_bar, mini_scrim_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);   /* no rows under it either */
    lv_obj_add_event_cb(sel_bar, mini_scrim_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_add_flag(sel_bar, LV_OBJ_FLAG_CLICKABLE);   /* taps on the bar stay on it */
    lv_obj_remove_flag(sel_bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(sel_bar, sel_bar_deleted, LV_EVENT_DELETE, NULL);
    /* the count over Select all, both on the bar's left */
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_semibold_18);
    sel_label = text(sel_bar, "", &ss_semibold_23, 0xffffff);
    lv_obj_set_width(sel_label, SEL_WORDS_W);
    label_lines(sel_label, 1);
    lv_obj_align(sel_label, LV_ALIGN_LEFT_MID, SL_INSET, -lh2 / 2);
    sel_all = text(sel_bar, "", &ss_semibold_18, C_ACCENT_TXT);
    /* up to the play button (52 px, its right edge 252 in): a long "Select all" ran under it */
    lv_obj_set_width(sel_all, SEL_WORDS_W);
    label_lines(sel_all, 1);
    lv_obj_align(sel_all, LV_ALIGN_LEFT_MID, SL_INSET, lh1 / 2);
    lv_obj_add_flag(sel_all, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sel_all, LV_MAX(SL_SP_S, (SL_TAP - lh2 + 1) / 2));
    lv_obj_add_event_cb(sel_all, sel_all_cb, LV_EVENT_CLICKED, NULL);
    sel_btn(LV_SYMBOL_CLOSE, IC_NONE, false, sel_close_cb, -12);
    if (kind == SEL_PLAYLISTS)           /* playlists: delete them, queue or play them */
    {
        sel_btn(LV_SYMBOL_TRASH, IC_NONE, false, sel_delete_cb, -12 - SEL_PITCH);
        sel_btn(LV_SYMBOL_LIST, IC_NONE, false, sel_queue_cb, -12 - 2 * SEL_PITCH);
        sel_btn(LV_SYMBOL_PLAY, IC_NONE, true, sel_play_cb, -12 - 3 * SEL_PITCH);
    }
    else if (kind == SEL_EPISODES)       /* episodes: their downloads gone, heard, downloaded, played */
    {
        sel_btn(LV_SYMBOL_TRASH, IC_NONE, false, sel_delete_cb, -12 - SEL_PITCH);
        sel_btn(LV_SYMBOL_OK, IC_NONE, false, sel_pod_heard_cb, -12 - 2 * SEL_PITCH);
        sel_btn(LV_SYMBOL_DOWNLOAD, IC_NONE, false, sel_pod_get_cb, -12 - 3 * SEL_PITCH);
        sel_btn(LV_SYMBOL_PLAY, IC_NONE, true, sel_play_cb, -12 - 4 * SEL_PITCH);
    }
    else
    {
        /* files: more (move, copy, rename) where songs have the heart */
        if (kind == SEL_FILES) sel_btn(LV_SYMBOL_DIRECTORY, IC_NONE, false, sel_more_cb, -12 - SEL_PITCH);
        else sel_btn(NULL, IC_HEART, false, sel_love_cb, -12 - SEL_PITCH);
        sel_btn(LV_SYMBOL_LIST, IC_NONE, false, sel_queue_cb, -12 - 2 * SEL_PITCH);
        sel_btn(LV_SYMBOL_PLUS, IC_NONE, false, sel_playlist_cb, -12 - 3 * SEL_PITCH);
        sel_btn(LV_SYMBOL_PLAY, IC_NONE, true, sel_play_cb, -12 - 4 * SEL_PITCH);
    }
    sel_count_label();
    az_sleep(page, true);
    anim_prop(sel_bar, set_ty, 120, 0, SL_T_PAGE, NULL);
    mini_update();
    if (kind == SEL_ARTISTS) sel_artists_repaint(view, true);
    else if (kind == SEL_PLAYLISTS) pls_sel_repaint(view, true);
    else if (kind == SEL_EPISODES) pod_sel_repaint(view, true);
    else lv_async_call(sel_refresh_async, view);
}
static void sel_begin(lv_obj_t *row)
{
    struct song_list_view *v = lv_obj_get_user_data(lv_obj_get_parent(row));
    int t = (int)(intptr_t)lv_obj_get_user_data(row);
    if (!v || t < 0 || t >= sl_lib.n_tracks) return;
    sel_start(SEL_SONGS, v, lv_obj_get_parent(v->body), t);
}
/* a tap while picking: one more, or one fewer; none left ends it */
static bool sel_toggle(int k)
{
    if (k < 0 || k >= sel_n) return false;
    bool on = !sel_has(k);
    sel_set(k, on);
    sel_count += on ? 1 : -1;
    sfx_play(on ? SFX_UI_ON : SFX_UI_OFF);
    if (!sel_count) { sel_end(); return false; }
    sel_count_label();
    return true;
}
static void sel_tap_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    if (!sel_picking(lv_obj_get_parent(r))) return;
    lv_event_stop_processing(e);         /* picked, not played */
    if (g_dragged && lv_indev_active() == touch) return;
    int t = (int)(intptr_t)lv_obj_get_user_data(r);
    if (t >= 0 && t < sl_lib.n_tracks && sel_toggle(t)) sel_paint_row(r, sel_has(t));
}
static void sel_cell_cb(lv_event_t *e)
{
    if (!sel_kind_on(SEL_ALBUMS)) return;
    lv_event_stop_processing(e);         /* picked, not opened */
    if (g_dragged && lv_indev_active() == touch) return;
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *cell = lv_event_get_current_target(e);
    if (sel_toggle(a)) sel_paint_cell(lv_obj_get_child(cell, 0), sel_has(a));
}
static void sel_artist_cb(lv_event_t *e)
{
    if (!sel_kind_on(SEL_ARTISTS)) return;
    lv_event_stop_processing(e);
    lv_obj_t *r = lv_event_get_current_target(e);
    int k = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
    if (sel_toggle(k)) sel_paint_artist(r, sel_has(k));
}

