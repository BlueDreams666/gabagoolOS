/* GabagoolOS: erase GabagoolOS's own data (P68). GPLv2+. Included by
 * sloppy.c after Settings.
 *
 * Settings > Advanced > Erase GabagoolOS data lists what goes and what
 * stays, then marks the card and restarts. At the start, before anything
 * of ours is read, the whole .rockbox/sloppy folder is set aside in one
 * rename (font packs and the Gabagool clip are moved back: they came with
 * the player or from its owner), the settings go back to how they came,
 * and the folder set aside is deleted on a thread of its own
 * (sl_net_erase_start), so the player is in use at once even with
 * thousands of cover thumbnails. Nothing outside .rockbox/sloppy is
 * touched: music, playlists, pictures, books, games, Wi-Fi networks and
 * Bluetooth pairings all stay. */

static const char *er_root = ROCKBOX_DIR;   /* the tests point it at their own card */
static const char *const er_keep[] = { "fonts", "gabagool.wav", "gabagool.vid" };

/* At the start: the marked card's folder set aside and the settings reset.
 * True when it was done. */
static bool er_apply(void)
{
    char mark[MAX_PATH], from[MAX_PATH], to[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    snprintf(mark, sizeof mark, "%s/sloppy-erase", er_root);
    if (!file_exists(mark)) return false;
    snprintf(from, sizeof from, "%s/sloppy", er_root);
    int i;
    for (i = 0; i < 10; i++)
    {
        snprintf(to, sizeof to, "%s/sloppy-erased%d", er_root, i);
        if (!dir_exists(to)) break;
    }
    if (i == 10 || (dir_exists(from) && rename(from, to) != 0))
        return false;                    /* nothing erased: the mark stays, to try again */
    mkdir(from);
    for (unsigned k = 0; k < ARRAYLEN(er_keep); k++)
    {
        snprintf(a, sizeof a, "%s/%s", to, er_keep[k]);
        snprintf(b, sizeof b, "%s/%s", from, er_keep[k]);
        rename(a, b);
    }
    settings_reset();
    settings_save();
    settings_apply(true);
    remove(mark);
    return true;
}
/* the start: erase when asked, and go on deleting what was set aside */
static void er_boot(void)
{
    bool erased = er_apply();
    if (erased && log_fd >= 0) { close(log_fd); log_fd = -1; }   /* the log begins again in the new folder */
    bool deleting = sl_net_erase_start();
    if (erased) sl_log("erase: GabagoolOS data erased; the settings are as they came");
    if (deleting) sl_log("erase: deleting what was set aside, on a thread of its own");
}

static void er_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
static void er_go_cb(lv_event_t *e)
{
    (void)e;
    char mark[MAX_PATH];
    snprintf(mark, sizeof mark, "%s/sloppy-erase", er_root);
    int fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = fd >= 0 && write(fd, "1", 1) == 1;
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (!ok)
    {
        remove(mark);
        toast_warn("Could not erase. Check the card.");
        return;
    }
    sl_log("erase: asked for; restarting");
#if defined(HIBY_R1) && !defined(SIMULATOR)
    sl_log_flush();
    system_reboot();                     /* no saving on the way out: all of it is going */
#else
    /* the simulator does not restart: erase now, as the start would */
    er_apply();
    sl_net_erase_start();
    touchscreen_set_mode(TOUCHSCREEN_POINT);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    if (sl_power_running) { sl_power_take_display(); sl_power_apply(); }
    appearance_reload_soon(true);
    toast("GabagoolOS data erased");
#endif
}

/* One thing that goes or stays: its icon, its name, and every word of
 * what it holds (wrapped, as a switch's line is: a list to read in full,
 * not to tap) */
static void er_row(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2)
{
    bool icon = !sl_zune();
    int tx = icon ? SET_TEXT_X : SL_INSET, w = SET_INNER - tx - SL_INSET;
    const char *s1 = sl_tr(t1), *s2 = sl_tr(t2);
    int h1 = set_text_h(s1, &ss_semibold_21, w), h2 = set_text_h(s2, &ss_regular_16, w);
    int body = h1 + SL_SP_XS + h2, h = LV_MAX(SET_ROW_H, body + 2 * SET_PAD_V), y = (h - body) / 2;
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = text(r, t1, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    sl_label_deep(l);
    set_place(l, tx, y, w);
    lv_obj_t *n = text(r, t2, &ss_regular_16, C_SUB);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    set_align(n);
    sl_label_deep(n);
    set_place(n, tx, y + h1 + SL_SP_XS, w);
    if (icon) set_place(set_icon(r, sym, tint, SET_ICON), SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    set_sep(g, r, tx);
}

/* Erase GabagoolOS data? What goes, what stays, then the two ways */
static void er_page(void)
{
    lv_obj_t *p, *body = page_new("Erase GabagoolOS data?", &p);
    lv_obj_t *g = set_group(body, "Erased");
    er_row(g, LV_SYMBOL_SETTINGS, 0x66738a, "Settings", "Every choice in Settings, your tiles and shortcuts");
    er_row(g, LV_SYMBOL_REFRESH, 0x2f8de4, "Library and covers", "The library's list and the cover thumbnails, made again from your music");
    er_row(g, LV_SYMBOL_LIST, 0x4f7fe0, "History", "Plays, Most played, Wrapped, skips and notifications");
    er_row(g, LV_SYMBOL_AUDIO, 0xe8408a, "Your taste", "Loved songs, favourite albums, hidden artists and saved stations");
    er_row(g, LV_SYMBOL_PLAY, 0x8b6fd6, "Games and your pet", "Scores, saved games and the pet");
    er_row(g, LV_SYMBOL_UPLOAD, 0xb8743a, "Sign-ins", "Last.fm, ListenBrainz and Spotify");
    er_row(g, LV_SYMBOL_VOLUME_MAX, 0xf0602f, "Sound", "EQ presets and the headphone profile");
    er_row(g, LV_SYMBOL_EDIT, 0xe89a1c, "Logs", "The player's log and the battery history");
    g = set_group(body, "Kept");
    er_row(g, LV_SYMBOL_DIRECTORY, 0xe89a1c, "Your files", "Music, playlists, pictures, books, games and everything else on the card");
    er_row(g, LV_SYMBOL_WIFI, 0x0aa9c7, "Wi-Fi and Bluetooth", "Saved networks and paired devices, which HiBy's own firmware shares");
    opt_note(body, "The player restarts and opens as it did the first time.");
    g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action_sub(g, LV_SYMBOL_LEFT, 0x66738a, "Keep everything", "Go back without erasing anything", er_keep_cb, NULL);
    lv_obj_t *r = set_action_sub(g, LV_SYMBOL_TRASH, 0xe5484d, "Erase and restart", "This cannot be undone", er_go_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(SET_RED), 0);
    page_push(p);
}
static void er_open_cb(lv_event_t *e) { (void)e; er_page(); }
