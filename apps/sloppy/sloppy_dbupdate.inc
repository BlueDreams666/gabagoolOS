/* ------------------------------------------------------ Update database */
/* Included by sloppy.c. The tagcache rebuilds on its own thread: it deletes
 * the database, scans the card and commits. The in-memory library stays
 * usable meanwhile. Updating gets a screen of its own on the top layer,
 * above the pages: it shows the phase and how far along it is, the music
 * keeps playing, and Hide leaves it running in the background. When the
 * commit lands, the pages under it are rebuilt around the new library (so
 * none is left pointing at freed tracks) while the screen shows the library
 * loading, and it then fades away to the new home screen. Hidden, the swap
 * waits until only the home screen is up, as before. */
enum { DBU_IDLE, DBU_POSTED, DBU_SCANNING, DBU_READY, DBU_LOADING, DBU_CHECKING };
/* Looking for changes (DBU_CHECKING): not the rebuild the Update database
 * button runs, which throws the database away and scans the card from the
 * start, but the tag database's own update. It walks the folders to scan,
 * reads only songs that are new or changed since (their file time), and
 * takes out songs gone from the card or from a folder no longer scanned.
 * The library stays usable all along and is read again only if anything
 * changed. Everything that runs by itself uses it: at start-up, after USB
 * mode, after Files and Wi-Fi transfer change the card, and pull to
 * refresh. */
static int db_check_done0, db_check_deleted0;
/* The quiet check (start-up, after USB mode) waits while music plays and
 * stops if music starts: its card reads ran for minutes at every start on
 * a test R1 (4249 songs), and as every Rockbox thread shares one OS thread
 * the player stalled with each one; Your taste, waiting for the audio
 * thread to start its clip, froze behind it (3 Nov). It runs again once
 * the music stops. */
static bool db_check_stopped, db_check_again;
static bool db_music_playing(void)
{
    int st = audio_status();
    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}
static int32_t db_check_key0[6];
/* db_check_quiet (declared in sloppy.c): at start-up and after USB mode, nothing to say if nothing changed */
static int db_update_state = DBU_IDLE;
static bool db_update_tell;             /* the load handed to the main loop says when it is done */
static long db_update_since;
static int db_update_expect;          /* entries the last full scan saw, 0 unknown */
static int db_last_entries, db_last_commit;
static long db_progress_at;           /* last tick anything moved */
#define DB_SCAN_NOTE ROCKBOX_DIR "/sloppy/scan.txt"

static lv_obj_t *dbu, *dbu_phase, *dbu_detail, *dbu_bar, *dbu_fill, *dbu_spin, *dbu_btn, *dbu_btn_txt;
static bool dbu_indeterminate;

static bool dbu_showing(void) { return dbu && !lv_obj_has_flag(dbu, LV_OBJ_FLAG_HIDDEN); }

static void dbu_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void dbu_gone(lv_anim_t *a)
{
    (void)a;
    if (!dbu) return;
    lv_obj_delete(dbu);
    dbu = dbu_phase = dbu_detail = dbu_bar = dbu_fill = dbu_spin = dbu_btn = dbu_btn_txt = NULL;
}
static void dbu_close(void)
{
    if (!dbu) return;
    lv_obj_remove_flag(dbu, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dbu);
    lv_anim_set_values(&a, lv_obj_get_style_opa(dbu, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, dbu_set_opa);
    lv_anim_set_completed_cb(&a, dbu_gone);
    lv_anim_start(&a);
}
static void dbu_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Hide while it runs; afterwards the same button closes it */
    if (db_update_state != DBU_IDLE)
        notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Still updating. The new songs appear when it is done.");
    dbu_close();
}

/* the bar: a width while the amount is known, else a sweep */
static void dbu_sweep_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void dbu_progress(int pct)
{
    int track = lv_obj_get_width(dbu_bar);
    if (pct < 0)
    {
        if (dbu_indeterminate) return;
        dbu_indeterminate = true;
        lv_obj_set_width(dbu_fill, track / 4);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dbu_fill);
        lv_anim_set_values(&a, -track / 4, track);
        lv_anim_set_duration(&a, SL_T_LOOP);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, dbu_sweep_x);
        lv_anim_start(&a);
        return;
    }
    if (dbu_indeterminate)
    {
        dbu_indeterminate = false;
        lv_anim_delete(dbu_fill, dbu_sweep_x);
        lv_obj_set_x(dbu_fill, 0);
    }
    if (pct > 100) pct = 100;
    int w = track * (pct < 2 ? 2 : pct) / 100;
    if (lv_obj_get_width(dbu_fill) != w) lv_obj_set_width(dbu_fill, w);
}

static void dbu_build(void)
{
    if (dbu)
    {
        lv_anim_delete(dbu, dbu_set_opa);          /* reopened while fading */
        lv_obj_set_style_opa(dbu, LV_OPA_COVER, 0);
        lv_obj_add_flag(dbu, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    dbu = box(lv_layer_top());
    lv_obj_set_size(dbu, W, H);
    lv_obj_add_style(dbu, &st_page, 0);
    lv_obj_add_flag(dbu, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* swallow taps, no back-swipe */
    lv_obj_remove_flag(dbu, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* a record being read: a slow ring around a disc */
    lv_obj_t *disc = box(dbu);
    lv_obj_set_size(disc, 150, 150);
    lv_obj_align(disc, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0x0c0f16), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(0x2a3444), 0);
    lv_obj_set_style_border_width(disc, 10, 0);
    lv_obj_t *label = box(disc);
    lv_obj_set_size(label, 46, 46);
    lv_obj_center(label);
    lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    dbu_spin = lv_spinner_create(dbu);
    lv_spinner_set_anim_params(dbu_spin, 1600, 70);
    lv_obj_set_size(dbu_spin, 186, 186);
    lv_obj_align(dbu_spin, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_arc_width(dbu_spin, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(dbu_spin, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(dbu_spin, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_arc_color(dbu_spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_remove_flag(dbu_spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = text(dbu, "Now updating", &ss_bold_28, C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 350);
    lv_obj_t *s = text(dbu, "please hold on boi", &ss_regular_21, C_ACCENT);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 392);

    dbu_phase = text(dbu, "Getting started", &ss_semibold_21, C_TEXT);
    lv_obj_align(dbu_phase, LV_ALIGN_TOP_MID, 0, 462);
    dbu_bar = box(dbu);
    lv_obj_set_size(dbu_bar, W - 100, 8);
    lv_obj_align(dbu_bar, LV_ALIGN_TOP_MID, 0, 500);
    lv_obj_set_style_radius(dbu_bar, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(dbu_bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(dbu_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(dbu_bar, true, 0);
    dbu_fill = box(dbu_bar);
    lv_obj_set_size(dbu_fill, 0, 8);
    lv_obj_set_style_radius(dbu_fill, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(dbu_fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(dbu_fill, LV_OPA_COVER, 0);
    dbu_indeterminate = false;
    dbu_detail = text(dbu, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(dbu_detail, W - 80);
    lv_obj_set_style_text_align(dbu_detail, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(dbu_detail, 2);
    lv_obj_align(dbu_detail, LV_ALIGN_TOP_MID, 0, 524);

    lv_obj_t *note = text(dbu, LV_SYMBOL_AUDIO "  Your music keeps playing", &ss_regular_18, C_DIM);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -150);
    dbu_btn = box(dbu);
    lv_obj_set_size(dbu_btn, 220, 60);
    lv_obj_align(dbu_btn, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_radius(dbu_btn, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(dbu_btn, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(dbu_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dbu_btn, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    dbu_btn_txt = text(dbu_btn, "Hide", &ss_semibold_23, C_TEXT);
    lv_obj_center(dbu_btn_txt);
    on_click(dbu_btn, dbu_btn_cb, NULL);
    lv_obj_remove_flag(dbu_btn, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (sb) lv_obj_move_foreground(sb);   /* the time and battery stay up */
    lv_obj_set_style_opa(dbu, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dbu);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, dbu_set_opa);
    lv_anim_start(&a);
}

/* The end of the road, good or bad: stop the spinner and say so. */
static void dbu_finish(const char *phase, const char *detail, bool ok)
{
    if (!dbu) return;
    label_if_changed(dbu_phase, phase);
    label_if_changed(dbu_detail, detail);
    lv_obj_add_flag(dbu_spin, LV_OBJ_FLAG_HIDDEN);
    dbu_progress(ok ? 100 : 0);
    lv_obj_set_style_text_color(dbu_phase, lv_color_hex(ok ? C_TEXT : 0xff6b6b), 0);
    label_if_changed(dbu_btn_txt, ok ? "Done" : "Close");
}

/* What the tagcache thread is up to, on the screen. */
static void dbu_refresh(void)
{
    if (!dbu_showing()) return;
    struct tagcache_stat *st = tagcache_get_stat();
    char b[128];
    if (db_update_state == DBU_POSTED || (db_update_state == DBU_SCANNING && !st->commit_step))
    {
        label_if_changed(dbu_phase, "Scanning your music");
        const char *cur = (const char *)st->curentry, *leaf = NULL;
        if (cur)
        {
            /* the folder, not the file: it changes slowly enough to read */
            const char *end = strrchr(cur, '/');
            for (const char *p = end ? end - 1 : NULL; p && p >= cur; p--)
                if (*p == '/') { leaf = p + 1; break; }
        }
        int n = st->processed_entries;
        if (leaf && strchr(leaf, '/'))
            snprintf(b, sizeof b, "%d items checked\n%.*s", n, (int)(strchr(leaf, '/') - leaf), leaf);
        else
            snprintf(b, sizeof b, n ? "%d items checked" : "Looking through the card", n);
        label_if_changed(dbu_detail, b);
        /* a card seen before gives a fair idea of the total */
        dbu_progress(db_update_expect > 0 ? n * 90 / db_update_expect : -1);
    }
    else if (db_update_state == DBU_SCANNING)
    {
        int max = tagcache_get_max_commit_step();
        label_if_changed(dbu_phase, "Saving your library");
        snprintf(b, sizeof b, "Step %d of %d", st->commit_step, max > 0 ? max : st->commit_step);
        label_if_changed(dbu_detail, b);
        dbu_progress(max > 0 ? 90 + st->commit_step * 5 / max : -1);
    }
    else if (db_update_state == DBU_CHECKING)
    {
        label_if_changed(dbu_phase, "Looking for changes");
        snprintf(b, sizeof b, st->processed_entries ? "%d items checked" : "Looking through the card", st->processed_entries);
        label_if_changed(dbu_detail, b);
        dbu_progress(-1);
    }
    else if (db_update_state == DBU_READY)
    {
        label_if_changed(dbu_phase, "Saving your library");
        label_if_changed(dbu_detail, "Nearly there");
        dbu_progress(95);
    }
    else if (db_update_state == DBU_LOADING)
    {
        label_if_changed(dbu_phase, "Loading it in");
        if (sl_lib.scan_total > 0)
            snprintf(b, sizeof b, "%d of %d songs", sl_lib.scan_done, sl_lib.scan_total);
        else
            snprintf(b, sizeof b, "%d songs", sl_lib.scan_done);
        label_if_changed(dbu_detail, b);
        dbu_progress(sl_lib.scan_total > 0 ? 95 + sl_lib.scan_done * 5 / sl_lib.scan_total : -1);
    }
    /* Long quiet stretches happen on a big card; say it is still going. */
    if (db_update_state != DBU_LOADING && TIME_AFTER(current_tick, db_progress_at + 45 * HZ))
        label_if_changed(dbu_detail, "Still going. Big folders take a while.");
}

/* Settings > Time and power > Update at start-up: once the library is up
 * and the player has settled after booting. And after USB mode, whatever
 * the setting: music copied over is in the library without asking. USB
 * mode restarts the interface, so its note lives in /tmp, which the
 * restart keeps and a power off clears. */
static void db_auto_poll(void)
{
    static bool done;
    if (lib_state != SL_LIB_READY || db_update_state != DBU_IDLE || current_tick < 3 * HZ ||
        !tagcache_is_usable()) return;
    /* the note also after the start-up update: a read USB mode stopped
     * leaves it for when the library is up again */
    bool usb = access(DB_USB_NOTE, F_OK) == 0;
    if (!usb && !db_check_again && (done || !global_settings.sloppy_db_auto || current_tick < 20 * HZ)) return;
    if (db_music_playing() || td.page) return;   /* nor while Your taste is open */
    done = true;
    db_check_again = false;
    unlink(DB_USB_NOTE);
    sl_log(usb ? "database update after USB mode" : "database update at start-up");
    db_check_quiet = true;
    db_update_begin(false);
}
static void db_check_begin(void)
{
    db_update_state = DBU_CHECKING;
    db_update_since = db_progress_at = current_tick;
    db_last_entries = db_last_commit = -1;
    db_check_done0 = tagcache_updates_done;
    db_check_deleted0 = tagcache_entries_deleted;
    if (!sl_lib_db_key(db_check_key0)) memset(db_check_key0, 0, sizeof db_check_key0);
    tagcache_update();                   /* only queues the job */
    sl_log("database: looking for changes%s", db_check_quiet ? " (quietly)" : "");
    if (!db_check_quiet) notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Looking for new music. Keep listening.");
}
static void db_update_begin(bool show)
{
    if (db_update_state != DBU_IDLE)
    {
        if (show) { dbu_build(); dbu_refresh(); }   /* back to the update screen */
        else notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Already updating your library");
        return;
    }
    if (lib_state == SL_LIB_LOADING) { db_check_quiet = false; toast("Your library is still loading"); return; }
    /* by itself: what changed, if the database is there to update */
    if (!show && tagcache_is_usable() && lib_state == SL_LIB_READY) { db_check_begin(); return; }
    /* By itself, never the rebuild, which deletes the database before it
     * scans: back from USB mode before the library had loaded, a test R1
     * rebuilt 27436 entries, was switched off and had no database (3 Nov).
     * It waits until the database is up (db_auto_poll); a missing one gets
     * the first scan, and Update library still rebuilds. */
    if (!show)
    {
        db_check_again = true;
        sl_log("database: the check waits for the database to be up");
        return;
    }
    db_check_quiet = false;
    tagcache_rebuild();                  /* only queues the job; returns false */
    db_update_state = DBU_POSTED;
    db_update_since = db_progress_at = current_tick;
    db_last_entries = db_last_commit = -1;
    int fd = open(DB_SCAN_NOTE, O_RDONLY);
    char note[16] = "";
    if (fd >= 0) { read(fd, note, sizeof note - 1); close(fd); }
    db_update_expect = atoi(note);
    sl_log("database update requested (last scan %d entries)", db_update_expect);
    if (show) { dbu_build(); dbu_refresh(); }
    else notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Updating your library. Keep listening.");
}
static void db_update_start(void) { db_update_begin(true); }

static bool db_update_home_idle(void)
{
    int pos;
    return npages == 1 && !np_open && !qp_open && !uq_open && !cf_is_open() &&
           !pet_care_locked && !appearance_reload_pending && !lib_banner &&
           !(button_status_wdata(&pos) & BUTTON_TOUCHSCREEN);
}

/* Swap in the new library: rebuild the pages around it. */
static void db_update_reload(void)
{
    sl_log("database update done: reloading the library");
    hist_recent_n = -1;                  /* track numbers change: Up next's Earlier reads the log again */
    if (np_open) np_hide(0);
    if (uq_open) uq_close();
    if (qp_open) qp_close();
    acts_close();
    if (cf_is_open()) cf_close();
    while (npages > 0)
    {
        lv_obj_t *p = pages[--npages];
        lv_anim_delete(p, NULL);
        lv_obj_delete(p);
    }
    home_settings_icon = NULL;
    lib_options_apply();
    lib_epoch++;
    lib_state = lib_begin();
    sl_art_invalidate_cache();
    sl_art_reset();
    cf_art_flush();
    np_img_album = -1;
    np_img_provisional = false;
    np_last_path[0] = '\0';
    home_page();
    if (mini) lv_obj_move_foreground(mini);
    if (dbu) lv_obj_move_foreground(dbu);
    if (sb) lv_obj_move_foreground(sb);
}

/* Library options changed how the library is read (Join compilations,
 * Ignore The, A and An): read it again, as after an update. An update or
 * a read already going reads it the new way when it gets there. */
static void db_options_reread(void *unused)
{
    (void)unused;
    if (db_update_state != DBU_IDLE || lib_state == SL_LIB_LOADING) return;
    sl_log("library options changed: reading the library again");
    db_update_reload();
    if (lib_state == SL_LIB_LOADING) { banner_show(); db_update_tell = true; }
    else if (lib_state == SL_LIB_READY) notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_MUSIC, "library", "Library updated");
}
static void db_update_poll(void)
{
    struct tagcache_stat *st = tagcache_get_stat();
    if (st->processed_entries != db_last_entries || st->commit_step != db_last_commit)
    {
        db_last_entries = st->processed_entries;
        db_last_commit = st->commit_step;
        db_progress_at = current_tick;
    }
    switch (db_update_state)
    {
        case DBU_POSTED:     /* wait for the tagcache thread to drop the old db */
            if (!tagcache_is_usable() || TIME_AFTER(current_tick, db_update_since + 15 * HZ))
                db_update_state = DBU_SCANNING;
            break;
        case DBU_SCANNING:
            if (st->commit_delayed)
            {
                db_update_state = DBU_IDLE;
                if (dbu_showing())
                    dbu_finish("Almost done", "The songs are scanned. Restart the player to finish.", false);
                else
                    notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Library scanned. Restart the player to finish.");
            }
            else if (tagcache_is_usable() && st->commit_step == 0)
            {
                db_update_state = DBU_READY;
                /* remember the size of the card for the next bar */
                int fd = open(DB_SCAN_NOTE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd >= 0)
                {
                    char note[16];
                    int n = snprintf(note, sizeof note, "%d\n", st->processed_entries);
                    write(fd, note, n);
                    close(fd);
                }
            }
            break;
        case DBU_READY:
            if (lib_state != SL_LIB_READY && lib_state != SL_LIB_FAILED)
            {
                db_update_state = DBU_IDLE;   /* the first-scan path owns it */
                break;
            }
            /* on the update screen the pages under it can be rebuilt now;
             * hidden, wait for the home screen and no finger down */
            if (!dbu_showing() && !db_update_home_idle()) break;
            if (dbu_showing() && (pet_care_locked || appearance_reload_pending)) break;
            db_update_reload();
            if (dbu_showing() && lib_state == SL_LIB_LOADING)
                db_update_state = DBU_LOADING;   /* the screen shows the load */
            else
            {
                db_update_state = DBU_IDLE;
                if (lib_state == SL_LIB_LOADING) { banner_show(); db_update_tell = true; }
                else if (lib_state == SL_LIB_READY) notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_MUSIC, "library", "Library updated");
                else notify_ex(NS_LIBRARY, NOTE_WARN, 0, NA_NONE, "library", "Library could not load. Check card space and restart.");
                if (dbu_showing())
                {
                    if (lib_state == SL_LIB_FAILED)
                        dbu_finish("Could not load it", "Check the card has free space, then restart.", false);
                    else
                        dbu_close();
                }
            }
            break;
        case DBU_CHECKING:
            if (db_check_quiet && !db_check_stopped && tagcache_updates_done == db_check_done0 &&
                (db_music_playing() || td.page))
            {
                tagcache_stop_scan();
                db_check_stopped = true;
                sl_log("database: the check stops while music plays, again after");
            }
            if (tagcache_updates_done == db_check_done0)
            {
                if (st->commit_delayed)        /* found new songs, but they can't be saved until a restart */
                {
                    db_update_state = DBU_IDLE;
                    notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Library scanned. Restart the player to finish.");
                }
                break;
            }
            if (db_check_stopped)
            {
                db_check_stopped = false;
                db_check_again = true;
            }
            {
                int32_t key[6];
                bool changed = tagcache_entries_deleted != db_check_deleted0 || !sl_lib_db_key(key)
                            || memcmp(key, db_check_key0, sizeof key);
                sl_log("database: %s (%d taken out)", changed ? "changes found" : "no changes",
                       tagcache_entries_deleted - db_check_deleted0);
                if (!changed)
                {
                    db_update_state = DBU_IDLE;
                    if (!db_check_quiet) notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_NONE, "library", "Your library is up to date");
                    if (dbu_showing()) dbu_finish("Up to date", "Nothing new on the card.", true);
                    db_check_quiet = false;
                    break;
                }
                sl_lib_cache_drop();           /* a removal alone leaves the key as it was */
                db_update_state = DBU_READY;   /* read again, as after an update */
                db_check_quiet = false;
            }
            break;
        case DBU_LOADING:              /* the main loop steps the load */
            if (lib_state == SL_LIB_LOADING) break;
            db_update_state = DBU_IDLE;
            if (lib_state == SL_LIB_READY)
            {
                char b[48];
                snprintf(b, sizeof b, "Library updated · %d songs", sl_lib.n_tracks);
                notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_MUSIC, "library", b);
                dbu_close();
            }
            else
                dbu_finish("Could not load it", "Check the card has free space, then restart.", false);
            break;
    }
    dbu_refresh();
}
