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
static void set_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void set_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void hide_cb(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_center(text(b, sym, &lv_font_montserrat_24, 0xffffff));
    on_click(b, cb, ud);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}

/* ------------------------------------------------------ queue actions */
static bool something_playing(void) { return (audio_status() & AUDIO_STATUS_PLAY) != 0; }

static void q_play_next(int t)
{
    if (!something_playing()) { play_list(&t, 1, 0, false); np_show(); return; }
    sl_log("play next %s", sl_lib.tracks[t].path);
    playlist_insert_track(NULL, sl_lib.tracks[t].path, PLAYLIST_INSERT_FIRST, true, true);
    toast("Plays next");
}
static void q_add(int t)
{
    if (!something_playing()) { play_list(&t, 1, 0, false); np_show(); return; }
    sl_log("add to queue %s", sl_lib.tracks[t].path);
    playlist_insert_track(NULL, sl_lib.tracks[t].path, PLAYLIST_INSERT, true, true);
    toast("Added to queue");
}
static void q_add_album(int a)
{
    const struct sl_album *al = &sl_lib.albums[a];
    if (!something_playing()) { play_list(al->tracks, al->n_tracks, 0, false); np_show(); return; }
    for (int i = 0; i < al->n_tracks; i++)
        playlist_insert_track(NULL, sl_lib.tracks[al->tracks[i]].path, PLAYLIST_INSERT, true, false);
    playlist_sync(NULL);
    char b[48];
    snprintf(b, sizeof b, "Added %d songs to the queue", al->n_tracks);
    toast(b);
}

/* --------------------------------------------------------- song sheet */
static lv_obj_t *acts_scrim, *acts;
static int acts_track = -1;
static bool acts_open_now;

static void acts_close(void)
{
    if (!acts_open_now) return;
    acts_open_now = false;
    anim_prop(acts, set_ty, 0, 520, 320, hide_cb);
    anim_prop(acts_scrim, set_opa, 110, 0, 320, hide_cb);
}
static void acts_scrim_cb(lv_event_t *e) { (void)e; acts_close(); }
static void artist_page(int k0);
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
        case 2: if (np_open) np_hide(0); album_page(a); break;
        case 3:
            if (np_open) np_hide(0);
            for (int j = 0; j < sl_lib.n_albums; j++)
                if (!strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[j]].artist, sl_lib.albums[a].artist))
                { artist_page(j); break; }
            break;
    }
}
static void acts_row(const char *sym, const char *label, int k)
{
    lv_obj_t *r = box(acts);
    lv_obj_set_size(r, lv_pct(100), 64);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_t *i = text(r, sym, &lv_font_montserrat_24, 0xffffff);
    lv_obj_align(i, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *l = text(r, label, &ss_semibold_23, 0xffffff);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 62, 0);
    on_click(r, acts_btn_cb, (void *)(intptr_t)k);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
}
static void acts_open(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    acts_track = t;
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
        lv_obj_set_style_radius(acts, 26, 0);
        lv_obj_set_style_bg_color(acts, lv_color_hex(0x262c44), 0);
        lv_obj_set_style_bg_opa(acts, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(acts, 10, 0);
        lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(acts, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps */
        lv_obj_remove_flag(acts, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_clean(acts);
    const struct sl_track *tr = &sl_lib.tracks[t];
    lv_obj_t *hd = box(acts);
    lv_obj_set_size(hd, lv_pct(100), 84);
    lv_obj_t *c = cover(hd, tr->album, SL_ART_ROW, false);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_t *t1 = text(hd, tr->title, &ss_semibold_23, 0xffffff);
    lv_obj_set_pos(t1, 80, 12);
    lv_obj_set_width(t1, W - 130);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    char b[160];
    snprintf(b, sizeof b, "%s · %s", sl_lib.albums[tr->album].artist, sl_lib.albums[tr->album].title);
    lv_obj_t *t2 = text(hd, b, &ss_regular_16, C_SUB);
    lv_obj_set_pos(t2, 80, 44);
    lv_obj_set_width(t2, W - 130);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x3a4160), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    acts_row(LV_SYMBOL_NEXT, "Play next", 0);
    acts_row(LV_SYMBOL_PLUS, "Add to queue", 1);
    acts_row(LV_SYMBOL_IMAGE, "Go to album", 2);
    acts_row(LV_SYMBOL_AUDIO, "Go to artist", 3);

    lv_obj_move_foreground(acts_scrim);
    lv_obj_move_foreground(acts);
    lv_obj_remove_flag(acts_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(acts, LV_OBJ_FLAG_HIDDEN);
    acts_open_now = true;
    anim_prop(acts_scrim, set_opa, 0, 110, 300, NULL);
    anim_prop(acts, set_ty, 520, 0, 420, NULL);
}
static void kebab_cb(lv_event_t *e)
{
    acts_open((int)(intptr_t)lv_event_get_user_data(e));
}
static void row_long_cb(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    acts_open((int)(intptr_t)lv_obj_get_user_data(r));
    lv_indev_reset(lv_indev_active(), NULL);   /* no click after the long press */
}

/* ------------------------------------------------------------ Up Next */
static lv_obj_t *uq, *uq_list;
static bool uq_open;
static void uq_build(void);

static int cur_index(void) { int i = 0; playlist_get_resume_info(&i); return i; }

static void uq_close(void)
{
    if (!uq_open) return;
    uq_open = false;
    anim_prop(uq, set_ty, lv_obj_get_style_translate_y(uq, 0), H, 360, hide_cb);
}
static void uq_done_cb(lv_event_t *e) { (void)e; uq_close(); }
static void uq_rebuild_cb(lv_timer_t *t) { lv_timer_delete(t); if (uq_open) uq_build(); }
static void uq_rebuild_soon(void) { lv_timer_create(uq_rebuild_cb, 250, NULL); }

static void uq_clear_cb(lv_event_t *e)
{
    (void)e;
    int amount = playlist_amount(), cur = cur_index(), removed = 0;
    for (int k = amount - 1; k >= 1; k--)
    {
        struct playlist_track_info info;
        int idx = (cur + k) % amount;
        if (playlist_get_track_info(NULL, idx, &info) >= 0 && (info.attr & PLAYLIST_ATTR_QUEUED))
        {
            playlist_delete(NULL, idx);
            removed++;
        }
    }
    sl_log("queue cleared (%d)", removed);
    toast("Queue cleared");
    uq_build();
}
static void uq_jump_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    sl_log("up next: jump %d ahead", k);
    audio_skip(k, 0);
    uq_rebuild_soon();
}
static void uq_remove_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    sl_log("up next: remove index %d", idx);
    playlist_delete(NULL, idx);
    uq_build();
}

/* dragging a queued row by its handle reorders the queue */
#define UQ_ROW_H 76
static int uq_drag_y0;
static void uq_handle_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *h = lv_event_get_current_target(e);
    lv_obj_t *row = lv_obj_get_parent(h);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED)
    {
        uq_drag_y0 = p.y;
        lv_obj_move_foreground(row);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x323a5c), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(row, 24, 0);
        lv_obj_set_style_shadow_opa(row, LV_OPA_60, 0);
    }
    else if (c == LV_EVENT_PRESSING)
        lv_obj_set_style_translate_y(row, p.y - uq_drag_y0, 0);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        int steps = (p.y - uq_drag_y0 + (p.y > uq_drag_y0 ? UQ_ROW_H / 2 : -UQ_ROW_H / 2)) / UQ_ROW_H;
        int from = (int)(intptr_t)lv_obj_get_user_data(row);
        int amount = playlist_amount();
        if (steps && amount > 0)
        {
            int to = (from + steps + amount) % amount;
            sl_log("up next: move %d -> %d", from, to);
            playlist_move(NULL, from, to);
        }
        uq_build();
    }
}

static lv_obj_t *uq_row(int idx, int k, const char *path, bool queued, bool now)
{
    int t = sl_lib_find_path(path);
    lv_obj_t *r = box(uq_list);
    lv_obj_set_size(r, lv_pct(100), UQ_ROW_H - 4);
    lv_obj_set_style_radius(r, 14, 0);
    lv_obj_set_user_data(r, (void *)(intptr_t)idx);
    if (now)
    {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_20, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(r, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
        on_click(r, uq_jump_cb, (void *)(intptr_t)k);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    if (t >= 0)
    {
        lv_obj_t *c = cover(r, sl_lib.tracks[t].album, SL_ART_ROW, false);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, 8, 0);
    }
    const char *title = t >= 0 ? sl_lib.tracks[t].title : strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    lv_obj_t *t1 = text(r, title, &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(t1, 78, 10);
    lv_obj_set_width(t1, W - 28 - 78 - (queued ? 110 : 12));
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    lv_obj_t *t2 = text(r, t >= 0 ? sl_lib.albums[sl_lib.tracks[t].album].artist : "", &ss_regular_16, C_SUB);
    lv_obj_set_pos(t2, 78, 40);
    lv_obj_set_width(t2, W - 28 - 78 - (queued ? 110 : 12));
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    if (queued)
    {
        lv_obj_t *x = icon_btn(r, LV_SYMBOL_CLOSE, 48, uq_remove_cb, (void *)(intptr_t)idx);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_t *h = box(r);
        lv_obj_set_size(h, 52, UQ_ROW_H - 4);
        lv_obj_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_center(text(h, LV_SYMBOL_BARS, &lv_font_montserrat_24, 0xaab3c4));
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(h, uq_handle_cb, LV_EVENT_ALL, NULL);
    }
    return r;
}

static void uq_build(void)
{
    lv_obj_clean(uq_list);
    int amount = playlist_amount(), cur = cur_index();
    struct playlist_track_info info;
    lv_obj_t *s = section(uq_list, "NOW PLAYING");
    (void)s;
    if (amount > 0 && playlist_get_track_info(NULL, cur, &info) >= 0)
        uq_row(cur, 0, info.filename, false, true);

    /* queued songs first, in play order, then the rest of the list */
    int queued = 0;
    for (int k = 1; k < amount; k++)
    {
        int idx = (cur + k) % amount;
        if (idx < cur && global_settings.repeat_mode != REPEAT_ALL) break;
        if (playlist_get_track_info(NULL, idx, &info) >= 0 && (info.attr & PLAYLIST_ATTR_QUEUED))
            queued++;
    }
    char b[64];
    snprintf(b, sizeof b, "IN YOUR QUEUE · %d", queued);
    section(uq_list, b);
    if (!queued)
    {
        lv_obj_t *e = text(uq_list, "Nothing queued. Hold a song, or tap its  " LV_SYMBOL_LIST "  button, then Play next or Add to queue.", &ss_regular_18, C_DIM);
        lv_obj_set_width(e, lv_pct(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_all(e, 10, 0);
    }
    int shown = 0;
    for (int k = 1; k < amount && shown < 400; k++)
    {
        int idx = (cur + k) % amount;
        if (idx < cur && global_settings.repeat_mode != REPEAT_ALL) break;
        if (playlist_get_track_info(NULL, idx, &info) >= 0 && (info.attr & PLAYLIST_ATTR_QUEUED))
        { uq_row(idx, k, info.filename, true, false); shown++; }
    }
    section(uq_list, "NEXT UP");
    int rest = 0;
    for (int k = 1; k < amount && rest < 60; k++)
    {
        int idx = (cur + k) % amount;
        if (idx < cur && global_settings.repeat_mode != REPEAT_ALL) break;
        if (playlist_get_track_info(NULL, idx, &info) >= 0 && !(info.attr & PLAYLIST_ATTR_QUEUED))
        { uq_row(idx, k, info.filename, false, false); rest++; }
    }
    if (!rest)
    {
        lv_obj_t *e = text(uq_list, "Nothing after this song.", &ss_regular_18, C_DIM);
        lv_obj_set_style_pad_all(e, 10, 0);
    }
    lv_obj_t *clr = lv_obj_get_child(lv_obj_get_child(uq, 1), 1);
    if (queued) lv_obj_remove_flag(clr, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(clr, LV_OBJ_FLAG_HIDDEN);
}

static void uq_hdr_drag_cb(lv_event_t *e)
{
    static int y0;
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED) y0 = p.y;
    else if (c == LV_EVENT_PRESSING && p.y > y0) lv_obj_set_style_translate_y(uq, p.y - y0, 0);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        if (p.y - y0 > 120) uq_close();
        else anim_prop(uq, set_ty, lv_obj_get_style_translate_y(uq, 0), 0, 260, NULL);
    }
}

static lv_obj_t *txt_btn(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_21, C_ACCENT);
    lv_obj_center(l);
    on_click(b, cb, NULL);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}

static void uq_show(void)
{
    if (!something_playing()) return;
    if (!uq)
    {
        uq = box(lv_screen_active());
        lv_obj_set_size(uq, W, H - 48);
        lv_obj_set_pos(uq, 0, 48);
        lv_obj_set_style_radius(uq, 26, 0);
        lv_obj_set_style_bg_color(uq, lv_color_hex(0x1d2238), 0);
        lv_obj_set_style_bg_opa(uq, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(uq, 40, 0);
        lv_obj_set_style_shadow_opa(uq, LV_OPA_60, 0);
        lv_obj_add_flag(uq, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
        lv_obj_remove_flag(uq, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *grab = box(uq);                     /* child 0 */
        lv_obj_set_size(grab, 56, 6);
        lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_radius(grab, 3, 0);
        lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(grab, LV_OPA_40, 0);

        lv_obj_t *hd = box(uq);                       /* child 1 */
        lv_obj_set_size(hd, W, 64);
        lv_obj_set_pos(hd, 0, 20);
        lv_obj_add_flag(hd, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(hd, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(hd, uq_hdr_drag_cb, LV_EVENT_ALL, NULL);
        lv_obj_t *t = text(hd, "Up next", &ss_bold_28, 0xffffff);    /* hd child 0 */
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
    }
    sl_log("up next open");
    uq_build();
    lv_obj_move_foreground(uq);
    lv_obj_remove_flag(uq, LV_OBJ_FLAG_HIDDEN);
    uq_open = true;
    anim_prop(uq, set_ty, H, 0, 440, NULL);
}
static void uq_btn_cb(lv_event_t *e) { (void)e; uq_show(); }

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

static lv_obj_t *xf_val, *xf_desc, *xf_slider, *xf_switch;
static void xf_labels(int n)
{
    char b[16];
    if (n) snprintf(b, sizeof b, "%d s", n); else strlcpy(b, "Off", sizeof b);
    lv_label_set_text(xf_val, b);
    char d[96];
    if (n) snprintf(d, sizeof d, "The last %d seconds of each song blend into the next.", n);
    else strlcpy(d, "Songs play back to back with no overlap.", sizeof d);
    lv_label_set_text(xf_desc, d);
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
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 9, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s, LV_OPA_50, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 18);
}
static void style_switch(lv_obj_t *sw)
{
    lv_obj_set_size(sw, 64, 36);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x5d6677), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
}

/* --------------------------------------------------------- equalizer */
static const char *eq_names[EQ_NUM_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const int eq_freqs[EQ_NUM_BANDS] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const struct { const char *name; signed char g[EQ_NUM_BANDS]; } eq_presets[] = {
    { "Flat",   { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "Bass",   { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 } },
    { "Vocal",  { -2, -2, -1, 1, 3, 4, 3, 1, 0, -1 } },
    { "Warm",   { 3, 3, 2, 1, 0, -1, -1, -2, -2, -3 } },
    { "Bright", { -1, -1, 0, 0, 0, 1, 2, 3, 4, 4 } },
};
static lv_obj_t *eq_sl[EQ_NUM_BANDS], *eq_vl[EQ_NUM_BANDS], *eq_chips[5], *eq_sw;

static void eq_apply_all(void)
{
    int maxg = 0;
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        global_settings.eq_band_settings[i].cutoff = eq_freqs[i];
        if (global_settings.eq_band_settings[i].q <= 0)
            global_settings.eq_band_settings[i].q = (i == 0 || i == EQ_NUM_BANDS - 1) ? 7 : 10;
        dsp_set_eq_coefs(i, &global_settings.eq_band_settings[i]);
        if (global_settings.eq_band_settings[i].gain > maxg) maxg = global_settings.eq_band_settings[i].gain;
    }
    global_settings.eq_precut = maxg;          /* keep boosted bands from clipping */
    dsp_set_eq_precut(global_settings.eq_precut);
    dsp_eq_enable(global_settings.eq_enabled);
}
static void eq_mark_chip(void)
{
    for (unsigned p = 0; p < 5; p++)
    {
        bool same = true;
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            if (global_settings.eq_band_settings[i].gain != eq_presets[p].g[i] * 10) same = false;
        lv_obj_set_style_bg_color(eq_chips[p], lv_color_hex(same ? C_ACCENT : C_CARD), 0);
    }
}
static void eq_value_label(int i)
{
    char b[8];
    int g = global_settings.eq_band_settings[i].gain / 10;
    snprintf(b, sizeof b, g > 0 ? "+%d" : "%d", g);
    lv_label_set_text(eq_vl[i], b);
}
static void eq_slider_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.eq_band_settings[i].gain = lv_slider_get_value(eq_sl[i]) * 10;
    eq_value_label(i);
    eq_apply_all();
    eq_mark_chip();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void eq_preset_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        global_settings.eq_band_settings[i].gain = eq_presets[p].g[i] * 10;
        lv_slider_set_value(eq_sl[i], eq_presets[p].g[i], LV_ANIM_ON);
        eq_value_label(i);
    }
    eq_apply_all();
    eq_mark_chip();
    settings_save();
    sl_log("eq preset %s", eq_presets[p].name);
}
static void eq_switch_cb(lv_event_t *e)
{
    (void)e;
    global_settings.eq_enabled = lv_obj_has_state(eq_sw, LV_STATE_CHECKED);
    eq_apply_all();
    settings_save();
}
static void eq_page(void)
{
    lv_obj_t *p, *body = page_new("Equalizer", &p);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    eq_sw = lv_switch_create(p);
    style_switch(eq_sw);
    lv_obj_align(eq_sw, LV_ALIGN_TOP_RIGHT, -20, SB_H + 15);
    if (global_settings.eq_enabled) lv_obj_add_state(eq_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(eq_sw, eq_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(eq_sw, LV_OBJ_FLAG_USER_1);

    lv_obj_t *chips = box(body);
    lv_obj_set_size(chips, lv_pct(100), 50);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 8, 0);
    for (unsigned k = 0; k < 5; k++)
    {
        lv_obj_t *c = box(chips);
        lv_obj_set_height(c, 42);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_style_radius(c, 21, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_center(text(c, eq_presets[k].name, &ss_semibold_18, 0xffffff));
        on_click(c, eq_preset_cb, (void *)(intptr_t)k);
        eq_chips[k] = c;
    }
    lv_obj_t *bands = box(body);
    lv_obj_set_size(bands, lv_pct(100), H - SB_H - 66 - 50 - 150);
    lv_obj_set_style_radius(bands, 20, 0);
    lv_obj_set_style_bg_color(bands, lv_color_hex(0x262c40), 0);
    lv_obj_set_style_bg_opa(bands, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bands, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bands, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(bands, 10, 0);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        lv_obj_t *col = box(bands);
        lv_obj_set_size(col, 40, lv_pct(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        eq_vl[i] = text(col, "", &ss_semibold_18, 0xdfe5ee);
        lv_obj_t *s = lv_slider_create(col);
        lv_obj_set_width(s, 6);
        lv_obj_set_flex_grow(s, 1);
        lv_obj_set_style_margin_ver(s, 12, 0);
        lv_slider_set_range(s, -12, 12);
        lv_slider_set_mode(s, LV_SLIDER_MODE_SYMMETRICAL);
        style_slider(s);
        lv_obj_set_ext_click_area(s, 16);
        lv_slider_set_value(s, global_settings.eq_band_settings[i].gain / 10, LV_ANIM_OFF);
        lv_obj_add_event_cb(s, eq_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, eq_slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        eq_sl[i] = s;
        text(col, eq_names[i], &ss_regular_16, 0xc3cad8);
        eq_value_label(i);
    }
    lv_obj_t *note = text(body, "Hz  ·  ±12 dB  ·  drag a band or pick a preset", &ss_regular_16, C_DIM);
    lv_obj_set_width(note, lv_pct(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(note, 8, 0);
    eq_mark_chip();
    page_push(p);
}

/* ------------------------------------------------------- play settings */
static void settings_row_cb(lv_event_t *e);
static void play_settings_page(void)
{
    lv_obj_t *p, *body = page_new("Play", &p);
    /* crossfade card */
    lv_obj_t *c = box(body);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_t *top = box(c);
    lv_obj_set_size(top, lv_pct(100), 36);
    lv_obj_align(text(top, "Crossfade", &ss_semibold_26, 0xffffff), LV_ALIGN_LEFT_MID, 0, 0);
    xf_val = text(top, "", &ss_semibold_23, 0xffffff);
    lv_obj_align(xf_val, LV_ALIGN_RIGHT_MID, 0, 0);
    xf_desc = text(c, "", &ss_regular_18, 0xd0d6de);
    lv_obj_set_width(xf_desc, lv_pct(100));
    lv_label_set_long_mode(xf_desc, LV_LABEL_LONG_WRAP);
    xf_slider = lv_slider_create(c);
    lv_obj_set_size(xf_slider, lv_pct(96), 8);
    lv_obj_set_style_margin_ver(xf_slider, 18, 0);
    lv_obj_set_style_margin_left(xf_slider, 8, 0);
    lv_slider_set_range(xf_slider, 0, 12);
    style_slider(xf_slider);
    lv_slider_set_value(xf_slider, xf_seconds(), LV_ANIM_OFF);
    lv_obj_add_event_cb(xf_slider, xf_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(xf_slider, xf_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *ticks = box(c);
    lv_obj_set_size(ticks, lv_pct(100), 22);
    const char *tk[] = { "Off", "3 s", "6 s", "9 s", "12 s" };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *t = text(ticks, tk[i], &ss_regular_16, 0xaab3c4);
        lv_obj_align(t, i == 0 ? LV_ALIGN_LEFT_MID : i == 4 ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                     i == 0 || i == 4 ? 0 : (W - 64) * i / 4 - 14, 0);
    }
    lv_obj_t *sr = box(c);
    lv_obj_set_size(sr, lv_pct(100), 48);
    lv_obj_align(text(sr, "Also when I skip a song", &ss_regular_18, 0xe2e7ef), LV_ALIGN_LEFT_MID, 0, 0);
    xf_switch = lv_switch_create(sr);
    style_switch(xf_switch);
    lv_obj_align(xf_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (xf_on_skip()) lv_obj_add_state(xf_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(xf_switch, xf_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    xf_labels(xf_seconds());

    card_row(body, "Equalizer", global_settings.eq_enabled ? "On" : "Off", 0x20001);
    page_push(p);
}

/* ------------------------------------------------------------ quick panel */
static lv_obj_t *qp, *qp_clock, *qp_date, *qp_bright, *qp_vol, *qp_np_t, *qp_np_a, *qp_np_pp;
static lv_obj_t *qp_tiles[8], *qp_tile_lbl[8];
static bool qp_open;
static int sleep_choice;
static const int sleep_minutes[] = { 0, 15, 30, 60 };

static const char *qp_tile_label(int k, char *b, int n)
{
    switch (k)
    {
        case 0: return "Shuffle";
        case 1: return global_settings.repeat_mode == REPEAT_ONE ? "Repeat one" :
                       global_settings.repeat_mode == REPEAT_ALL ? "Repeat all" : "Repeat";
        case 2: if (xf_seconds()) { snprintf(b, n, "Fade %d s", xf_seconds()); return b; } return "Crossfade";
        case 3: return "EQ";
        case 4: if (sleep_choice) { snprintf(b, n, "%d min", sleep_minutes[sleep_choice]); return b; } return "Sleep";
        case 5: return "Lock";
        case 6: return "Settings";
        default: return "Rockbox";
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
        default: return false;
    }
}
static void qp_refresh(void)
{
    if (!qp) return;
    char b[32];
    for (int k = 0; k < 8; k++)
    {
        lv_obj_set_style_bg_color(lv_obj_get_child(qp_tiles[k], 0), lv_color_hex(qp_tile_on(k) ? C_ACCENT : 0x3a4160), 0);
        lv_label_set_text(qp_tile_lbl[k], qp_tile_label(k, b, sizeof b));
    }
    struct tm *tm = get_time();
    snprintf(b, sizeof b, "%02d:%02d", tm->tm_hour, tm->tm_min);
    lv_label_set_text(qp_clock, b);
    snprintf(b, sizeof b, "%d%%", battery_level());
    lv_label_set_text(qp_date, b);
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    lv_label_set_text(qp_np_t, id3 && id3->title ? id3->title : "Nothing playing");
    lv_label_set_text(qp_np_a, id3 ? (id3->albumartist ? id3->albumartist : id3->artist ? id3->artist : "") : "");
    lv_label_set_text(lv_obj_get_child(qp_np_pp, 0), is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (!lv_slider_is_dragged(qp_vol)) lv_slider_set_value(qp_vol, volume_pct(), LV_ANIM_OFF);
}
static void qp_close_to(int from)
{
    qp_open = false;
    anim_prop(qp, set_ty, from, -H, 340, hide_cb);
}
static void qp_close(void) { if (qp_open) qp_close_to(lv_obj_get_style_translate_y(qp, 0)); }
static void qp_tile_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    switch (k)
    {
        case 0: np_btn_cb(e == NULL ? NULL : e); break;   /* replaced below */
        default: break;
    }
}
static void qp_tile_press(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    sl_log("quick panel tile %d", k);
    switch (k)
    {
        case 0:
            global_settings.playlist_shuffle = !global_settings.playlist_shuffle;
            if (global_settings.playlist_shuffle) playlist_randomise(NULL, current_tick, true);
            else playlist_sort(NULL, true);
            break;
        case 1:
            global_settings.repeat_mode = global_settings.repeat_mode == REPEAT_OFF ? REPEAT_ALL :
                                          global_settings.repeat_mode == REPEAT_ALL ? REPEAT_ONE : REPEAT_OFF;
            audio_flush_and_reload_tracks();
            break;
        case 2: xf_apply(xf_seconds() ? 0 : xf_last, xf_on_skip() || !xf_seconds()); break;
        case 3: global_settings.eq_enabled = !global_settings.eq_enabled; eq_apply_all(); break;
        case 4:
            sleep_choice = (sleep_choice + 1) % 4;
            set_sleeptimer_duration(sleep_minutes[sleep_choice]);
            break;
        case 5: qp_close(); backlight_off(); return;
        case 6: qp_close(); play_settings_page(); return;
        case 7: qp_close(); want_classic = true; return;
    }
    settings_save();
    qp_refresh();
}
static void qp_bright_cb(lv_event_t *e)
{
    (void)e;
    int v = lv_slider_get_value(qp_bright);
    global_settings.brightness = v;
    backlight_set_brightness(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}
static void qp_vol_cb(lv_event_t *e)
{
    (void)e;
    int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME);
    global_status.volume = mn + (mx - mn) * lv_slider_get_value(qp_vol) / 100;
    setvol();
    statusbar_update();
}
static void qp_np_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == 1) audio_prev(); else if (k == 2) toggle_play(); else if (k == 3) audio_next();
    else if (k == 0 && something_playing()) { qp_close(); np_show(); }
    lv_timer_t *t = lv_timer_create((lv_timer_cb_t)qp_refresh, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}
static void qp_drag_cb(lv_event_t *e)
{
    static int y0;
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED) y0 = p.y;
    else if (c == LV_EVENT_PRESSING && p.y < y0) lv_obj_set_style_translate_y(qp, p.y - y0, 0);
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        int dy = p.y - y0;
        if (dy < -100) qp_close_to(dy);
        else if (dy < 0) anim_prop(qp, set_ty, dy, 0, 260, NULL);
        else if (dy < 8 && p.y > H - 120) qp_close();   /* tap on the empty bottom */
    }
}
static void qp_build(void)
{
    qp = box(lv_layer_top());
    lv_obj_set_size(qp, W, H);
    lv_obj_set_style_bg_color(qp, lv_color_hex(0x121628), 0);
    lv_obj_set_style_bg_opa(qp, 245, 0);
    lv_obj_set_style_pad_hor(qp, 14, 0);
    lv_obj_add_flag(qp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(qp, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(qp, qp_drag_cb, LV_EVENT_ALL, NULL);

    qp_clock = text(qp, "", &ss_bold_28, 0xffffff);
    lv_obj_set_style_text_font(qp_clock, &ss_bold_64, 0);
    lv_obj_set_pos(qp_clock, 10, 18);
    qp_date = text(qp, "", &ss_semibold_21, C_SUB);
    lv_obj_align(qp_date, LV_ALIGN_TOP_RIGHT, -10, 44);

    lv_obj_t *tp = box(qp);
    lv_obj_set_size(tp, W - 28, 236);
    lv_obj_set_pos(tp, 0, 108);
    lv_obj_set_style_radius(tp, 26, 0);
    lv_obj_set_style_bg_color(tp, lv_color_hex(0x232a42), 0);
    lv_obj_set_style_bg_opa(tp, LV_OPA_COVER, 0);
    static const char *syms[8] = { LV_SYMBOL_SHUFFLE, LV_SYMBOL_LOOP, LV_SYMBOL_SHUFFLE, LV_SYMBOL_EQUALIZER_ICON,
                                   LV_SYMBOL_POWER, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_SETTINGS, LV_SYMBOL_HOME };
    for (int k = 0; k < 8; k++)
    {
        lv_obj_t *t = box(tp);
        lv_obj_set_size(t, (W - 28) / 4, 112);
        lv_obj_set_pos(t, (k % 4) * (W - 28) / 4, 6 + (k / 4) * 114);
        lv_obj_t *cc = box(t);                           /* child 0: circle */
        lv_obj_set_size(cc, 72, 72);
        lv_obj_align(cc, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(cc, 36, 0);
        lv_obj_set_style_bg_opa(cc, LV_OPA_COVER, 0);
        lv_obj_center(text(cc, syms[k], &lv_font_montserrat_28, 0xffffff));
        qp_tile_lbl[k] = text(t, "", &ss_semibold_18, 0xdbe1ea);
        lv_obj_align(qp_tile_lbl[k], LV_ALIGN_BOTTOM_MID, 0, -4);
        on_click(t, qp_tile_press, (void *)(intptr_t)k);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_transform_scale(cc, 235, LV_STATE_PRESSED);
        qp_tiles[k] = t;
    }

    lv_obj_t *sp = box(qp);
    lv_obj_set_size(sp, W - 28, 140);
    lv_obj_set_pos(sp, 0, 356);
    lv_obj_set_style_radius(sp, 26, 0);
    lv_obj_set_style_bg_color(sp, lv_color_hex(0x232a42), 0);
    lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
    lv_obj_t *sun = text(sp, LV_SYMBOL_IMAGE, &lv_font_montserrat_24, 0xffffff);
    lv_obj_set_pos(sun, 20, 26);
    qp_bright = lv_slider_create(sp);
    lv_obj_set_size(qp_bright, W - 130, 10);
    lv_obj_set_pos(qp_bright, 70, 34);
    lv_slider_set_range(qp_bright, MIN_BRIGHTNESS_SETTING, MAX_BRIGHTNESS_SETTING);
    style_slider(qp_bright);
    lv_slider_set_value(qp_bright, global_settings.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(qp_bright, qp_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(qp_bright, qp_bright_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *spk = text(sp, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_24, 0xffffff);
    lv_obj_set_pos(spk, 20, 92);
    qp_vol = lv_slider_create(sp);
    lv_obj_set_size(qp_vol, W - 130, 10);
    lv_obj_set_pos(qp_vol, 70, 100);
    lv_slider_set_range(qp_vol, 0, 100);
    style_slider(qp_vol);
    lv_obj_add_event_cb(qp_vol, qp_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *np_c = box(qp);
    lv_obj_set_size(np_c, W - 28, 104);
    lv_obj_set_pos(np_c, 0, 508);
    lv_obj_set_style_radius(np_c, 26, 0);
    lv_obj_set_style_bg_color(np_c, lv_color_hex(0x232a42), 0);
    lv_obj_set_style_bg_opa(np_c, LV_OPA_COVER, 0);
    on_click(np_c, qp_np_cb, (void *)0);
    lv_obj_remove_flag(np_c, LV_OBJ_FLAG_EVENT_BUBBLE);
    qp_np_t = text(np_c, "", &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(qp_np_t, 20, 22);
    lv_obj_set_width(qp_np_t, W - 240);
    lv_label_set_long_mode(qp_np_t, LV_LABEL_LONG_DOT);
    qp_np_a = text(np_c, "", &ss_regular_16, C_SUB);
    lv_obj_set_pos(qp_np_a, 20, 54);
    lv_obj_set_width(qp_np_a, W - 240);
    lv_label_set_long_mode(qp_np_a, LV_LABEL_LONG_DOT);
    lv_obj_t *nx = icon_btn(np_c, LV_SYMBOL_NEXT, 52, qp_np_cb, (void *)3);
    lv_obj_align(nx, LV_ALIGN_RIGHT_MID, -12, 0);
    qp_np_pp = icon_btn(np_c, LV_SYMBOL_PLAY, 60, qp_np_cb, (void *)2);
    lv_obj_set_style_bg_color(qp_np_pp, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qp_np_pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(qp_np_pp, 0), lv_color_hex(C_ACCENT), 0);
    lv_obj_align(qp_np_pp, LV_ALIGN_RIGHT_MID, -72, 0);
    lv_obj_t *pv = icon_btn(np_c, LV_SYMBOL_PREV, 52, qp_np_cb, (void *)1);
    lv_obj_align(pv, LV_ALIGN_RIGHT_MID, -140, 0);

    lv_obj_t *grab = box(qp);
    lv_obj_set_size(grab, 64, 6);
    lv_obj_align(grab, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_50, 0);
    (void)qp_tile_cb;
}
static void qp_drag_to(int dy)
{
    if (!qp) qp_build();
    if (!qp_open && lv_obj_has_flag(qp, LV_OBJ_FLAG_HIDDEN))
    {
        qp_refresh();
        lv_obj_remove_flag(qp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(qp);
    }
    lv_anim_delete(qp, NULL);
    if (dy > H) dy = H;
    lv_obj_set_style_translate_y(qp, dy - H, 0);
}
static void qp_release(int dy, int vy)
{
    if (dy > H / 3 || vy > 500)
    {
        qp_open = true;
        anim_prop(qp, set_ty, dy - H, 0, 120 + 300 * (H - dy) / H, NULL);
        sl_log("quick panel open");
    }
    else qp_close_to(dy - H);
}
