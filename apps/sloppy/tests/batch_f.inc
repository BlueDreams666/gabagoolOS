/* Gate 1 batch F: personalisation and system. Now Playing's album line,
 * the clock's place, rounded corners, the screensaver, the screen turned
 * round, erasing GabagoolOS's data, the battery log, car mode and the
 * phone keypad. Included from ui_test.c after batch_e.inc, whose helpers
 * (and ui_audit.inc's) it uses.
 *
 *   f_tests()                                            in the suite
 *   SLOPPY_F_TESTS=1                                     batch F alone
 *   SLOPPY_F_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after   the screenshots
 *
 * SL_F_BASE (defined only when building the tree before batch F, for the
 * "before" shots) leaves out everything that names batch F's code. */

static bool f_au_dirty;
static void f_shot(const char *dir, const char *name, const char *look, const char *tag) {
    char p[600]; frame(0); snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, name, look, tag); shot_now(p);
}
/* the box of an object on the screen */
static lv_area_t f_box(lv_obj_t *o) { lv_area_t a; lv_obj_update_layout(o); lv_obj_get_coords(o, &a); return a; }
static void f_np_open(void) {
    test_status = AUDIO_STATUS_PLAY; sl_track_path(0,test_track.path,sizeof test_track.path);
    test_track.title = "Blue Hours"; test_track.artist = "Mira Sol"; test_track.album = "Blue Hours (Deluxe)";
    test_track.length = 240000; test_track.elapsed = 72000;
    np_show(); frame(500); np_update(true); frame(300);
}

#ifndef SL_F_BASE
/* ---------------------------------------------------------- R2: the album line */
static void f_np_album_tests(void) {
    int was = global_settings.sloppy_np_album;
    for (int style = 0; style < 3; style++) {
        global_settings.sloppy_np_style = style;
        global_settings.sloppy_np_album = 0; now_playing_retheme(); f_np_open();
        assert(lv_obj_has_flag(np_album, LV_OBJ_FLAG_HIDDEN));
        lv_area_t a0 = f_box(np_artist), s0 = f_box(np_seek), v0 = f_box(np_vol), t0 = f_box(np_title);
        global_settings.sloppy_np_album = 1; now_playing_retheme(); frame(300);
        assert(!lv_obj_has_flag(np_album, LV_OBJ_FLAG_HIDDEN));
        assert(!strcmp(lv_label_get_text(np_album), "Blue Hours (Deluxe)"));
        lv_area_t t1 = f_box(np_title), a1 = f_box(np_artist), b1 = f_box(np_album), s1 = f_box(np_seek), v1 = f_box(np_vol);
        /* the title stays; the artist closes up 2; the album under it, clear of the seek bar's knob */
        assert(t1.y1 == t0.y1 && a1.y1 == a0.y1 - 2 && s1.y1 == s0.y1 + 12 && v1.y1 == v0.y1 + 12);
        assert(b1.y1 >= a1.y2 && b1.y2 + 4 < s1.y1 - 6);   /* the knob swells 6 above the track */
        assert(v1.y2 < H - 20);                            /* the volume still well clear of the edge */
        /* the album as the tag says; the library's when the tag has none */
        test_track.album = NULL; np_last_path[0] = '\0'; np_update(true); frame(100);
        assert(!strcmp(lv_label_get_text(np_album), sl_lib.albums[sl_lib.tracks[0].album].title));
        global_settings.sloppy_np_album = 0; now_playing_retheme(); frame(300);
        lv_area_t a2 = f_box(np_artist), s2 = f_box(np_seek);
        assert(a2.y1 == a0.y1 && s2.y1 == s0.y1 && lv_obj_has_flag(np_album, LV_OBJ_FLAG_HIDDEN));
        np_hide(0); frame(400);
    }
    global_settings.sloppy_np_style = 0; global_settings.sloppy_np_album = was; now_playing_retheme();
    /* the switch is on Customization > Now Playing, and Settings search finds it */
    np_style_page(); frame(400);
    assert(find_label(pages[npages - 1], sl_tr("Show the album")));
    while (npages > 1) { page_pop(); frame(300); }
    bool found = false;
    for (unsigned i = 0; i < ARRAYLEN(sq_settings); i++) if (!strcmp(sq_settings[i].name, "Show the album")) found = sq_settings[i].code == 0x20016;
    assert(found);
    printf("PASS: Now Playing: Title, Album and Artist as an option, in Glass, Classic and Centred; the controls move down 12, nothing overlaps.\n");
}

/* ---------------------------------------------------------- P61: the clock's place */
static bool f_overlap(lv_obj_t *a, lv_obj_t *b) {
    if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t x = f_box(a), y = f_box(b), r;
    return lv_area_intersect(&r, &x, &y);
}
static void f_clock_tests(void) {
    int was = global_settings.sloppy_clock_pos;
    while (npages > 1) { page_pop(); frame(300); }
    /* everything on the bar at once: playing, Wi-Fi, the percentage, charging */
    test_status = AUDIO_STATUS_PLAY; global_settings.battery_display = 1;
    bool conn0 = wf_st.connected; wf_st.connected = true;
    for (int pos = 0; pos < 3; pos++) {
        global_settings.sloppy_clock_pos = pos; statusbar_update(); frame(100);
        lv_area_t c = f_box(sb_clock), b = f_box(sb_batt);
        if (pos == 0) assert(c.x1 == 96);
        if (pos == 1) assert(LV_ABS((c.x1 + c.x2) / 2 - W / 2) <= 1);
        if (pos == 2) assert(c.x2 == W - 16 - 1 && b.x2 == c.x1 - 12 - 1);
        else assert(b.x2 == W - 16 - 1);
        lv_obj_t *all[] = { sb_vol, sb_vol_n, sb_clock, sb_batt, sb_radio };
        for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) assert(!f_overlap(all[i], all[j]));
        /* the clock and battery stay on the bar's middle line */
        assert(LV_ABS((c.y1 + c.y2) - (b.y1 + b.y2)) <= 1);
    }
    /* a wider time, at the right: the battery follows it */
    global_settings.sloppy_clock_pos = 2; label_if_changed(sb_clock, "12:58"); sb_place(); frame(50);
    lv_area_t c = f_box(sb_clock), b = f_box(sb_batt);
    assert(c.x2 == W - 16 - 1 && b.x2 == c.x1 - 12 - 1);
    wf_st.connected = conn0; global_settings.battery_display = 0;
    global_settings.sloppy_clock_pos = was; statusbar_update(); frame(100);
    time_power_page(); frame(400);
    assert(find_label(pages[npages - 1], sl_tr("Clock position")) && find_label(pages[npages - 1], sl_tr("Centre")));
    while (npages > 1) { page_pop(); frame(300); }
    printf("PASS: status bar: the clock at the left, in the middle or at the right, the battery and the rest kept clear of it.\n");
}

/* ---------------------------------------------------------- P62: rounded corners */
static void f_corner_tests(void) {
    int th0 = global_settings.sloppy_theme;
    global_settings.sloppy_theme = 1; appearance_reload((void *)2); frame(700);   /* light: the corners show black on white */
    while (npages > 1) { page_pop(); frame(300); }
    theme_page(); frame(500);                      /* a page that holds still */
    global_settings.sloppy_round_corners = 0; sl_corners_apply(); lv_obj_invalidate(lv_screen_active()); frame(100);
    static uint16_t before[W * H];
    memcpy(before, pixels, sizeof before);
    global_settings.sloppy_round_corners = 1; sl_corners_apply(); lv_obj_invalidate(lv_screen_active()); frame(100);
    for (int k = 0; k < 4; k++) assert(sl_corner[k] && !lv_obj_has_flag(sl_corner[k], LV_OBJ_FLAG_HIDDEN));
    const int R = SL_R_SCREEN;
    /* the very corner black, the edge's middle and a point just inside the arc as the page has them */
    int cx[4] = { 0, W - 1, 0, W - 1 }, cy[4] = { 0, 0, H - 1, H - 1 };
    for (int k = 0; k < 4; k++) {
        assert(pixels[cy[k] * W + cx[k]] == 0);
        int mx = k & 1 ? W - 1 - R / 4 : R / 4, my = k & 2 ? H - 1 - R / 4 : R / 4;   /* in the arc's cut: black */
        assert(pixels[my * W + mx] == 0);
    }
    /* only the corners changed */
    int changed = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if (pixels[y * W + x] != before[y * W + x]) {
            changed++;
            assert((x < R || x >= W - R) && (y < R || y >= H - R));
        }
    assert(changed > 4 * R);
    global_settings.sloppy_round_corners = 0; sl_corners_apply(); lv_obj_invalidate(lv_screen_active()); frame(100);
    for (int k = 0; k < 4; k++) assert(lv_obj_has_flag(sl_corner[k], LV_OBJ_FLAG_HIDDEN));
    assert(!memcmp(before, pixels, sizeof before));   /* off: the screen as it was */
    assert(find_label(pages[npages - 1], sl_tr("Rounded corners")));
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_theme = th0; appearance_reload((void *)2); frame(700);
    printf("PASS: rounded corners: the four corners masked smooth and black over every page, nothing else touched; off again leaves the screen as it was.\n");
}

/* ---------------------------------------------------------- PS22: the battery log */
static int f_fake_size(const char *p) { int fd = open(p, O_RDONLY); if (fd < 0) return -1; int n = (int)lseek(fd, 0, SEEK_END); sim_close(fd); return n; }   /* close is POSIX in the tests */
/* a day: off the charger at 100 % for 10 h down to 40, 2 h charging to 90, 1 h off to 84 */
static void f_bl_day(long now) {
    static char b[200000]; int n = snprintf(b, sizeof b, "time,level,mv,charging,screen,playing\n");
    long t = now - 13 * 3600;
    for (int m = 0; m <= 13 * 60; m++, t += 60) {
        int level, c = 0;
        if (m <= 600) level = 100 - m * 60 / 600;
        else if (m <= 720) { level = 40 + (m - 600) * 50 / 120; c = 1; }
        else level = 90 - (m - 720) * 6 / 60;
        n += snprintf(b + n, sizeof b - n, "%ld,%d,%d,%d,%d,%d\n", t, level, 3500 + level * 7, c, m % 7 == 0, 1);
    }
    remove(BL_PREV); fake_put(BL_FILE, b, n);
}
static void f_batlog_tests(void) {
    int was = global_settings.sloppy_battery_log;
    remove(BL_FILE); remove(BL_PREV);
    /* on by default: the first line a little after the start, then one a minute, never two at once */
    global_settings.sloppy_battery_log = 1;
    bl_write();
    assert(fake_has(BL_FILE, "time,level,mv,charging,screen,playing\n") && fake_has(BL_FILE, ",83,4012,"));
    uint32_t w0 = bl_writes;
    bl_tick(); bl_tick(); bl_tick();
    assert(bl_writes == w0);                     /* each call costs a comparison, no write */
    /* timed: a line costs the UI almost nothing */
    uint64_t t0 = sl_perf_now_us();
    for (int i = 0; i < 100; i++) bl_write();
    uint64_t per = (sl_perf_now_us() - t0) / 100;
    t0 = sl_perf_now_us();
    for (int i = 0; i < 100000; i++) bl_tick();
    uint64_t tick_ns = (sl_perf_now_us() - t0) * 1000 / 100000;
    printf("battery log: a line %llu us, the tick's check %llu ns (once per status tick)\n", (unsigned long long)per, (unsigned long long)tick_ns);
    /* USB mode: the card is the computer's, nothing written */
    log_held = true; w0 = bl_writes; bl_write(); assert(bl_writes == w0); log_held = false;
    /* full: started again, the last kept beside it */
    static char big[BL_MAX_BYTES + 100]; memset(big, '#', sizeof big); big[sizeof big - 1] = '\n';
    fake_put(BL_FILE, big, sizeof big);
    bl_write();
    assert(f_fake_size(BL_PREV) == (int)sizeof big && f_fake_size(BL_FILE) < 200 && fake_has(BL_FILE, ",83,4012,"));
    /* off: nothing */
    global_settings.sloppy_battery_log = 0; w0 = bl_writes;
    bl_tick(); assert(bl_writes == w0);
    /* the graph's reading: the last day, the drain while off the charger */
    long now = (long)time(NULL);
    f_bl_day(now);
    struct bl_pt *pt = malloc(BL_MAX_PTS * sizeof *pt);
    int n = bl_read(pt, now);
    assert(n == 13 * 60 + 1 && pt[0].level == 100 && pt[n - 1].level == 84 && (pt[n - 1].flags & BL_PLAYING));
    int run_s = 0, drop = 0, rate = bl_rate_x10(pt, n, &run_s, &drop);
    assert(rate == 61 && drop == 6 && run_s == 3540);   /* 90 % at the unplugging, 84 % 59 min later */
    /* a garbled line and the clock set back are survived */
    fake_put(BL_FILE, "time,level\nzz\n100,50,3800,0,1,1\n50,49,3790,0,1,1\n", 46);
    assert(bl_read(pt, 200) == 1);
    free(pt);
    f_bl_day(now);
    global_settings.sloppy_battery_log = 1;
    time_power_page(); frame(500);
    char want[160]; snprintf(want, sizeof want, sl_tr("About %d.%d%% an hour lately: %d h %02d min left at %d%%"), 6, 1, 13, 46, 84);
    assert(find_label(pages[npages - 1], want) && find_label(pages[npages - 1], sl_tr("Keep a battery log")));
    while (npages > 1) { page_pop(); frame(300); }
    /* charging now */
    { char b[200]; long t = now - 120; int k = snprintf(b, sizeof b, "%ld,60,3900,1,1,0\n%ld,61,3910,1,1,0\n", t, t + 60); fake_put(BL_FILE, b, k); }
    time_power_page(); frame(500);
    snprintf(want, sizeof want, sl_tr("Charging, %d%% now"), 61);
    assert(find_label(pages[npages - 1], want));
    while (npages > 1) { page_pop(); frame(300); }
    remove(BL_FILE); remove(BL_PREV);
    time_power_page(); frame(500);
    assert(find_label(pages[npages - 1], sl_tr("No history yet. A reading is kept every minute.")));
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_battery_log = was;
    printf("PASS: battery log: a line a minute on the card (none in USB mode, none when off, started again past 512 KB), the last day drawn in Time and power with the drain and the time left.\n");
}

/* ---------------------------------------------------------- P68: erase GabagoolOS data */
static void f_mkdirs(const char *p) {   /* every folder down to p, on the test card */
    char d[MAX_PATH]; strlcpy(d, p, sizeof d);
    for (char *s = d + 1; *s; s++) if (*s == '/') { *s = '\0'; if (!sim_dir_exists(d)) mkdir(d); *s = '/'; }
    if (!sim_dir_exists(d)) mkdir(d);
}
static void f_erase_tests(void) {
    /* a card with GabagoolOS's folder, what came with the player, and the owner's files */
    const char *R = FAKE_CARD "/.rockbox";
    f_mkdirs(FAKE_CARD "/.rockbox/sloppy/thumbs/g0001"); f_mkdirs(FAKE_CARD "/.rockbox/sloppy/fonts");
    f_mkdirs(FAKE_CARD "/Playlists"); f_mkdirs(FAKE_CARD "/Music/Erase Test");
    fake_put(FAKE_CARD "/.rockbox/sloppy/taste.txt", "loved", 5);
    fake_put(FAKE_CARD "/.rockbox/sloppy/plays.dat", "plays", 5);
    fake_put(FAKE_CARD "/.rockbox/sloppy/lastfm.session", "key", 3);
    fake_put(FAKE_CARD "/.rockbox/sloppy/thumbs/g0001/1.bin", "px", 2);
    fake_put(FAKE_CARD "/.rockbox/sloppy/fonts/cjk16.slf", "font", 4);
    fake_put(FAKE_CARD "/.rockbox/sloppy/gabagool.wav", "ova", 3);
    fake_put(FAKE_CARD "/Playlists/Mine.m3u8", "x.flac", 6);
    fake_put(FAKE_CARD "/Music/Erase Test/01.flac", "fLaC", 4);
    fake_put(FAKE_CARD "/.rockbox/config.cfg", "theme: 1", 8);
    /* asked from Advanced: the page lists what goes and what stays */
    while (npages > 1) { page_pop(); frame(300); }
    advanced_page(); frame(400);
    lv_obj_t *row = lv_obj_get_parent(find_label(pages[npages - 1], sl_tr("Erase GabagoolOS data")));
    assert(row); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); frame(500);
    lv_obj_t *pg = pages[npages - 1];
    static const char *const listed[] = { "Settings", "Library and covers", "History", "Your taste", "Games and your pet",
        "Sign-ins", "Sound", "Logs", "Your files", "Wi-Fi and Bluetooth", "Keep everything", "Erase and restart" };
    for (unsigned i = 0; i < ARRAYLEN(listed); i++) assert(find_label(pg, sl_tr(listed[i])));
    /* Keep everything: nothing marked */
    const char *was = er_root; er_root = R;
    lv_obj_send_event(lv_obj_get_parent(find_label(pg, sl_tr("Keep everything"))), LV_EVENT_CLICKED, NULL); frame(400);
    assert(npages == 2 && !sim_file_exists(FAKE_CARD "/.rockbox/sloppy-erase"));
    lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); frame(500);
    /* Erase: the mark, then (the R1 restarts here) the start sets the folder aside */
    int resets = settings_reset_calls, saves = settings_save_calls;
    lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], sl_tr("Erase and restart"))), LV_EVENT_CLICKED, NULL);
    frame(800);
    assert(settings_reset_calls == resets + 1 && settings_save_calls >= saves + 1);
    assert(!sim_file_exists(FAKE_CARD "/.rockbox/sloppy-erase"));
    assert(sim_dir_exists(FAKE_CARD "/.rockbox/sloppy") && sim_dir_exists(FAKE_CARD "/.rockbox/sloppy-erased0"));
    assert(!sim_file_exists(FAKE_CARD "/.rockbox/sloppy/taste.txt") && !sim_file_exists(FAKE_CARD "/.rockbox/sloppy/plays.dat"));
    assert(!sim_file_exists(FAKE_CARD "/.rockbox/sloppy/lastfm.session") && !sim_dir_exists(FAKE_CARD "/.rockbox/sloppy/thumbs"));
    assert(sim_file_exists(FAKE_CARD "/.rockbox/sloppy-erased0/taste.txt"));
    /* kept: what came with the player, the owner's clip and files */
    assert(fake_equals(FAKE_CARD "/.rockbox/sloppy/fonts/cjk16.slf", "font") && fake_equals(FAKE_CARD "/.rockbox/sloppy/gabagool.wav", "ova"));
    assert(fake_equals(FAKE_CARD "/Playlists/Mine.m3u8", "x.flac") && fake_equals(FAKE_CARD "/Music/Erase Test/01.flac", "fLaC"));
    /* no mark: a start does nothing */
    resets = settings_reset_calls;
    assert(!er_apply() && settings_reset_calls == resets);
    /* a second erase sets aside under the next name */
    fake_put(FAKE_CARD "/.rockbox/sloppy/taste.txt", "again", 5);
    fake_put(FAKE_CARD "/.rockbox/sloppy-erase", "1", 1);
    assert(er_apply() && sim_file_exists(FAKE_CARD "/.rockbox/sloppy-erased1/taste.txt") && sim_file_exists(FAKE_CARD "/.rockbox/sloppy/fonts/cjk16.slf"));
    er_root = was;
    while (npages > 1) { page_pop(); frame(300); }
    /* the set-aside folder is deleted on a thread, on the real card (thousands of thumbnails) */
    char base[600], p[700];
    snprintf(base, sizeof base, "%s/.rockbox/sloppy-erased4", sl_net_card_root());
    snprintf(p, sizeof p, "mkdir -p '%s/thumbs/g1' '%s/thumbs/g2' && for i in $(seq 1 600); do : > '%s/thumbs/g1/'$i.bin; : > '%s/thumbs/g2/'$i.bin; done && : > '%s/taste.txt'",
             base, base, base, base, base);
    assert(system(p) == 0);
    int before = sl_net_erase_count();
    uint64_t t0 = sl_perf_now_us();
    assert(sl_net_erase_start());
    uint32_t worst = 0;
    for (int i = 0; i < 3000 && sl_net_erase_busy(); i++) { uint64_t f0 = sl_perf_now_us(); frame(10); uint32_t d = (uint32_t)(sl_perf_now_us() - f0); if (d > worst) worst = d; usleep(1000); }
    assert(!sl_net_erase_busy());
    struct stat st;
    assert(stat(base, &st) != 0 && sl_net_erase_count() - before == 1201);
    printf("erase: 1,201 files deleted on the thread in %llu ms; worst UI frame meanwhile %u us\n",
           (unsigned long long)((sl_perf_now_us() - t0) / 1000), worst);
    assert(!sl_net_erase_start());   /* nothing left */
    printf("PASS: erase GabagoolOS data: the page lists what goes and what stays; .rockbox/sloppy set aside at the start (font packs and the Gabagool clip kept), settings reset, music, playlists and files untouched, the rest deleted on a thread.\n");
}

/* ---------------------------------------------------------- P64/R3: the screensaver */
/* music playing with a cover, Now Playing put away, the screen as it would be at its sleep time */
static void f_sv_ready(int style, bool art) {
    if (!np_img_buf) np_img_buf = calloc(W * W, 2);   /* as the player has from its start */
    bool ra = test_real_art; test_real_art = art;
    np_img_album = -1; np_last_path[0] = '\0';
    f_np_open(); np_hide(0); frame(400);
    test_real_art = ra;
    global_settings.sloppy_saver = style;
    sl_power_running = true; sl_power_suspended = false;
    sl_power_state_init(&sl_power, sl_ms()); sl_power.sleep_ms = sl_power_sleep_ms();
}
static void f_sv_idle(void) { sl_power.activity_at = sl_ms() - sl_power.sleep_ms - 200; sl_power_tick(); frame(300); }
static void f_saver_tests(void) {
    bool run0 = sl_power_running; int was = global_settings.sloppy_saver;
    while (npages > 1) { page_pop(); frame(300); }
    /* off: the screen goes dark at its time, as before */
    f_sv_ready(0, true); f_sv_idle();
    assert(sl_power.mode == SL_POWER_DARK && !sv_shown());
    /* floating cover: where it would sleep, the screensaver over the lock screen, dimmer */
    f_sv_ready(1, true); f_sv_idle();
    assert(sv_shown() && sl_power.mode == SL_POWER_LOCKED && sl_power.saver && sv.px && !lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(sv.clock, LV_OBJ_FLAG_HIDDEN) && !strcmp(lv_label_get_text(sv.title), "Blue Hours"));
    /* it never goes dark while the music plays */
    sl_power.activity_at = sl_ms() - 10 * 60000; sl_power_tick();
    assert(sl_power.mode == SL_POWER_LOCKED && sv_shown());
    /* it drifts a pixel a second and stays on the screen, turning at the edges, for an hour */
    lv_area_t g0 = f_box(sv.group);
    uint32_t s0 = sv_steps;
    for (int i = 0; i < 3600; i++) {
        sv_timer_cb(NULL);
        if (i == 0) { lv_area_t g1 = f_box(sv.group); assert(LV_ABS(g1.x1 - g0.x1) == 1 && LV_ABS(g1.y1 - g0.y1) == 1); }
        lv_area_t g = f_box(sv.group);
        assert(g.x1 >= SL_GUTTER && g.x2 < W - SL_GUTTER + 1 && g.y1 >= 40 && g.y2 < H - 40 + 1);
    }
    assert(sv_steps - s0 == 3600);
    /* a touch: the lock screen, the contact kept (no unlock, no tap through) */
    sl_power_filter_touch(true, 240, 600);
    assert(!sv_shown() && sl_power.mode == SL_POWER_LOCKED && !sl_power.saver && sl_power.swallow_touch && !sv.px);
    sl_power_filter_touch(false, 240, 300);
    assert(sl_power.mode == SL_POWER_LOCKED);
    /* a volume key: the lock screen with the volume over it */
    f_sv_ready(1, true); f_sv_idle(); assert(sv_shown());
    sl_power_filter_button(BUTTON_UP); sl_power_tick(); frame(100);
    assert(!sv_shown() && sl_power.mode == SL_POWER_LOCKED);
    sl_power_filter_button(BUTTON_UP | BUTTON_REL);
    /* the music stops: 15 s of it, then dark */
    f_sv_ready(1, true); f_sv_idle(); assert(sv_shown());
    test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE; sl_power_tick();
    assert(sv_shown() && sv.stopped_at);
    sv.stopped_at = sl_ms() - SL_POWER_WAKE_MS - 1; sl_power_tick(); sl_power_tick(); frame(100);
    assert(!sv_shown() && sl_power.mode == SL_POWER_DARK);
    test_status = AUDIO_STATUS_PLAY;
    /* square cover: the Now Playing cover itself, round a small ring every 30 s */
    f_sv_ready(2, true); f_sv_idle();
    assert(sv_shown() && lv_image_get_src(sv.img) == &np_img && !sv.px);
    lv_area_t q0 = f_box(sv.group);
    sv.moved_at = sl_ms() - 30001; sv_timer_cb(NULL);
    lv_area_t q1 = f_box(sv.group);
    assert((q1.x1 != q0.x1 || q1.y1 != q0.y1) && LV_ABS(q1.x1 - q0.x1) <= 8 && LV_ABS(q1.y1 - q0.y1) <= 8);
    sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600);
    /* the clock: it moves when the minute does */
    f_sv_ready(3, true); f_sv_idle();
    assert(sv_shown() && !lv_obj_has_flag(sv.clock, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN));
    lv_area_t c0 = f_box(sv.group);
    sv.min = -1; sv_timer_cb(NULL);
    lv_area_t c1 = f_box(sv.group);
    assert(c1.x1 != c0.x1 || c1.y1 != c0.y1);
    assert(c1.x1 >= SL_GUTTER && c1.x2 < W && c1.y1 >= 60 && c1.y2 < H - 60 + 1);
    sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600);
    /* a cover chosen but none there: the clock instead */
    f_sv_ready(1, false); f_sv_idle();
    assert(sv_shown() && !lv_obj_has_flag(sv.clock, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN));
    sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600);
    /* nothing playing: no screensaver */
    f_sv_ready(1, true); test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE; f_sv_idle();
    assert(!sv_shown() && sl_power.mode == SL_POWER_DARK);
    test_status = AUDIO_STATUS_PLAY;
    /* its cost while it shows: 10 s of each, every frame drawn, as the R1 would */
    static const char *const nm[] = { "", "saver-float", "saver-square", "saver-clock" };
    for (int st = 1; st <= 3; st++) {
        f_sv_ready(st, true); f_sv_idle(); assert(sv_shown());
        pf_begin(nm[st]);
        uint64_t t0 = sl_perf_now_us();
        for (int i = 0; i < 40; i++) { if (st != 1 && i == 20) { sv.moved_at = 0; sv.min = -1; } pf_frame(250); }
        (void)t0;
        double work = 0; for (int i = 0; i < pf.n; i++) if (pf.px[i]) work += (pf.us[i] - pf.flush_us[i]) * PF_K / 1000.0 + pf.px[i] * (3.5 + 3.6 + 1.7) / 297600.0;
        int frames = 0; for (int i = 0; i < pf.n; i++) frames += pf.px[i] != 0;
        printf("screensaver %s: %d frames drawn in 10 s, R1 est %.1f ms of drawing = %.2f%% of a core\n", nm[st] + 6, frames, work, work / 100.0);
        sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600);
    }
    /* Customization: its page, the hub's row, Settings search */
    saver_page(); frame(400);
    assert(find_label(pages[npages - 1], sl_tr("Floating cover")) && find_label(pages[npages - 1], sl_tr("Square cover")));
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_saver = was; sl_power_running = run0; sl_power_state_init(&sl_power, sl_ms());
    printf("PASS: screensaver: floating cover, square cover or clock where the screen would sleep with music on, never dark while it plays, always moving and on the screen, a touch or key back to the lock screen, dark 15 s after the music stops.\n");
}

/* ---------------------------------------------------------- PS18: car mode */
static void f_tap(lv_obj_t *o) { lv_obj_send_event(o, LV_EVENT_CLICKED, NULL); frame(100); }
static void f_car_tests(void) {
    int was = global_settings.sloppy_car; bool run0 = sl_power_running, ch0 = test_charging;
    while (npages > 1) { page_pop(); frame(300); }
    sl_power_running = true; sl_power_state_init(&sl_power, sl_ms());
    f_np_open(); np_hide(0); frame(400);
    /* off: power coming and going changes nothing */
    global_settings.sloppy_car = 0;
    test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE;
    car_power(true, false); assert(!car_shown() && !car.resume_at);
    car_power(false, false); assert(!car_off_shown() && !car.off_at);
    /* on: power comes, the big controls at once, the music 3 s later where it stopped */
    global_settings.sloppy_car = CAR_ON;
    sl_power_state_set(&sl_power, SL_POWER_DARK, sl_ms()); sl_power_apply();
    test_charging = true; car_power(true, false); frame(300);
    assert(car_shown() && car.resume_at && sl_power.mode == SL_POWER_ACTIVE && !is_playing());
    assert(!strcmp(lv_label_get_text(car.title), "Blue Hours"));
    car.resume_at = sl_ms() - 1; car_timer_cb(NULL);
    assert(is_playing() && !car.resume_at);
    /* lit while the car powers it: never dims or sleeps */
    sl_power.activity_at = sl_ms() - 10 * 60000; sl_power_tick();
    assert(sl_power.mode == SL_POWER_ACTIVE);
    /* the buttons: play/pause, the volume a step of 5 either way */
    f_tap(car.pp); assert(!is_playing() && !strcmp(lv_label_get_text(lv_obj_get_child(car.pp, 0)), LV_SYMBOL_PLAY));
    f_tap(car.pp); assert(is_playing());
    int v0 = volume_pct();
    f_tap(lv_obj_get_child(lv_obj_get_parent(car.vol), 2));
    assert(volume_pct() == LV_MIN(100, v0 + 5) || volume_pct() >= v0);
    f_tap(lv_obj_get_child(lv_obj_get_parent(car.vol), 0));
    /* the big screen's size: every button a big target */
    lv_area_t pp = f_box(car.pp); assert(lv_area_get_width(&pp) >= 140);
    /* the power goes: paused at once, the question, off in 30 s */
    test_charging = false; car_power(false, false); frame(300);
    assert(!is_playing() && !car_shown() && car_off_shown() && car.off_at);
    assert(find_label(car.off, sl_tr("Stay on")) && find_label(car.off, sl_tr("Turn off now")));
    char want[96]; snprintf(want, sizeof want, sl_tr("The music is paused. The player turns off in %d s."), 30);
    assert(find_label(car.off, want));
    /* the power back before then: no turning off, the music again */
    test_charging = true; car_power(true, false); frame(100);
    assert(!car_off_shown() && !car.off_at && car.resume_at);
    car.resume_at = sl_ms() - 1; car_timer_cb(NULL); assert(is_playing());
    /* gone again; Stay on keeps it on */
    test_charging = false; car_power(false, false);
    f_tap(lv_obj_get_parent(find_label(car.off, sl_tr("Stay on"))));
    assert(!car_off_shown() && !car.off_at);
    /* gone and nobody there: off after the 30 s */
    int offs = poweroffs;
    car_power(false, false); car.off_at = sl_ms() - 1; car_timer_cb(NULL);
    assert(poweroffs == offs + 1 && !car.off_at);
    bye_cancel(); frame(300);
    if (car_off_shown()) lv_obj_add_flag(car.off, LV_OBJ_FLAG_HIDDEN);
    /* each part can be turned off: no playing, no big screen, no turning off */
    global_settings.sloppy_car = CAR_ON | CAR_NO_PLAY | CAR_NO_BIG | CAR_NO_OFF;
    test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE; test_charging = true;
    car_power(true, false); assert(!car_shown() && !car.resume_at);
    test_status = AUDIO_STATUS_PLAY; test_charging = false;
    car_power(false, false); assert(!car_off_shown() && !is_playing());
    test_status = AUDIO_STATUS_PLAY;
    /* the page: the switch, its parts hidden while off, the way to the big screen */
    global_settings.sloppy_car = 0;
    time_power_page(); frame(400);
    lv_obj_t *row = lv_obj_get_parent(find_label(pages[npages - 1], sl_tr("Car mode")));
    assert(row); f_tap(row); frame(400);
    assert(car_parts_box && lv_obj_has_flag(car_parts_box, LV_OBJ_FLAG_HIDDEN));
    car_set_on(1); assert(!lv_obj_has_flag(car_parts_box, LV_OBJ_FLAG_HIDDEN));
    f_tap(lv_obj_get_parent(find_label(pages[npages - 1], sl_tr("Open the big controls"))));
    assert(car_shown()); f_tap(lv_obj_get_child(car.root, 0)); assert(!car_shown() && !car.px);
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_car = was; test_charging = ch0; sl_power_running = run0; sl_power_state_init(&sl_power, sl_ms());
    printf("PASS: car mode: power in, big controls and the music 3 s later; lit on the car's power; power out, paused and off after 30 s unless Stay on or the power returns; each part can be turned off.\n");
}

/* ---------------------------------------------------------- P65: upside down */
static void f_flip_tests(void) {
    int was = global_settings.sloppy_flip;
    while (npages > 1) { page_pop(); frame(300); }
    /* the keys: the upper one louder, whichever way up */
    sl_flip_apply(false);
    assert(sl_flip_key(BUTTON_UP) == BUTTON_UP && sl_flip_key(BUTTON_DOWN | BUTTON_REL) == (BUTTON_DOWN | BUTTON_REL));
    sl_flip_apply(true);
    assert(sl_flip_key(BUTTON_UP) == BUTTON_DOWN && sl_flip_key(BUTTON_DOWN | BUTTON_REL) == (BUTTON_UP | BUTTON_REL));
    assert(sl_flip_key(BUTTON_DOWN | BUTTON_REPEAT) == (BUTTON_UP | BUTTON_REPEAT));
    assert(sl_flip_key(BUTTON_PLAY) == BUTTON_PLAY && sl_flip_key(BUTTON_POWER) == BUTTON_POWER && sl_flip_key(BUTTON_NONE) == BUTTON_NONE);
    /* a touch: where the finger is on the turned panel is where it lands on the page */
    buttons_page(); frame(500);
    lv_obj_t *l = find_label(pages[npages - 1], sl_tr("Upside down"));
    assert(l);
    lv_obj_t *row = lv_obj_get_parent(l);
    lv_obj_t *sw = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    lv_area_t a = f_box(row);
    lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF); frame(200); a = f_box(row);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    bool on0 = lv_obj_has_state(sw, LV_STATE_CHECKED);
    touch_at(W - 1 - cx, H - 1 - cy, true); frame(40);
    lv_point_t pt; lv_indev_get_point(touch, &pt);
    assert(pt.x == cx && pt.y == cy);          /* LVGL is told the picture's own point */
    touch_at(W - 1 - cx, H - 1 - cy, false); frame(200);
    assert(lv_obj_has_state(sw, LV_STATE_CHECKED) != on0);   /* the row under it took the tap */
    global_settings.sloppy_flip = was; sl_flip_apply(was == 1);
    while (npages > 1) { page_pop(); frame(300); }
    printf("PASS: upside down: touches turned back to the picture's points, the volume keys swapped (the upper one louder), the rest of the keys as they were.\n");
}

/* ---------------------------------------------------------- P58: the phone keypad */
static int f_t9_key(struct kb *kb, int act, uint32_t cp) {
    for (int i = 0; i < kb->nk; i++) if (kb->k[i].act == act && (!cp || kb->k[i].cp == cp)) return i;
    return -1;
}
static void f_t9_keys(struct kb *kb, const char *digits) {
    for (const char *d = digits; *d; d++) {
        int i = *d == ' ' ? f_t9_key(kb, KA_SPACE, 0) : f_t9_key(kb, KA_T9, (uint32_t)*d);
        assert(i >= 0); kb_press_key(kb, i); frame(50);
    }
}
static const char *f_ta(struct kb *kb) { return lv_textarea_get_text(kb->ta); }
static void f_t9_tests(void) {
    int t0 = global_settings.sloppy_kb_t9, f0 = global_settings.sloppy_kb_first, s0 = global_settings.sloppy_kb_second, l0 = global_settings.sloppy_language;
    global_settings.sloppy_language = 0;
    global_settings.sloppy_kb_first = KB_T9 + 1; global_settings.sloppy_kb_second = KB_EN + 1;
    while (npages > 1) { page_pop(); frame(300); }
    eq_save_page(NULL); frame(400);
    struct kb *kb = kb_now(); assert(kb && kb->lay == KB_T9 && kb_t9(kb));
    lv_textarea_set_text(kb->ta, ""); kb_set_shift(kb, 0);
    /* twelve keys: 1 to 9 three across, Delete, Shift and the T9 key beside them, then 123, globe, space, Done */
    int n9 = 0; for (int i = 0; i < kb->nk; i++) n9 += kb->k[i].act == KA_T9;
    assert(n9 == 9 && f_t9_key(kb, KA_BKSP, 0) >= 0 && f_t9_key(kb, KA_SHIFT, 0) >= 0 && f_t9_key(kb, KA_T9MODE, 0) >= 0);
    assert(f_t9_key(kb, KA_GLOBE, 0) >= 0 && f_t9_key(kb, KA_SPACE, 0) >= 0);
    for (int i = 0; i < kb->nk; i++) { lv_area_t a = f_box(kb->k[i].o); assert(a.x1 >= 0 && a.x2 < W && lv_area_get_width(&a) >= 44 && lv_area_get_height(&a) >= 44); }
    assert(!strcmp(lv_label_get_text(kb->k[f_t9_key(kb, KA_T9, '7')].l), "pqrs"));
    /* predictive: a press a letter, the commonest word in the field, the others in the bar */
    global_settings.sloppy_kb_t9 = 1; t9_mode_label(kb);
    f_t9_keys(kb, "43556"); assert(!strcmp(f_ta(kb), "hello"));
    f_t9_keys(kb, " ");     assert(!strcmp(f_ta(kb), "hello "));
    f_t9_keys(kb, "4663");  assert(!strcmp(f_ta(kb), "hello good") && kb->sug_t9 && kb->nsug >= 2);
    bool home = false; for (int i = 0; i < kb->nsug; i++) home |= !strcmp(kb->sug[i], "home");
    assert(home);
    int slot = -1; static const int which[3] = { 1, 0, 2 };
    for (int sl = 0; sl < 3; sl++) if (which[sl] < kb->nsug && !strcmp(kb->sug[which[sl]], "home")) slot = sl;
    assert(slot >= 0); kb_sug_pick(kb, slot); frame(50);
    assert(!strcmp(f_ta(kb), "hello home"));
    /* Delete takes the last key, then the word */
    f_t9_keys(kb, " 46"); assert(!strcmp(f_ta(kb), "hello home go") || strlen(f_ta(kb)) == strlen("hello home go"));
    kb_backspace(kb, false); frame(50); assert(strlen(f_ta(kb)) == strlen("hello home g"));
    kb_backspace(kb, false); frame(50); assert(!strcmp(f_ta(kb), "hello home "));
    /* Shift: the word with a capital */
    kb_set_shift(kb, 1); f_t9_keys(kb, "4663"); assert(!strcmp(f_ta(kb), "hello home Good") && kb->shift == 0);
    f_t9_keys(kb, " ");
    /* 1: punctuation, round as multi-tap */
    f_t9_keys(kb, "11"); assert(!strcmp(f_ta(kb), "hello home Good ,"));
    /* multi-tap: the T9 key, then a key pressed until its letter comes */
    lv_textarea_set_text(kb->ta, ""); kb_set_shift(kb, 0); t9_commit(kb);
    kb_press_key(kb, f_t9_key(kb, KA_T9MODE, 0)); frame(50);
    assert(global_settings.sloppy_kb_t9 == 0 && !strcmp(lv_label_get_text(kb->k[f_t9_key(kb, KA_T9MODE, 0)].l), "abc"));
    f_t9_keys(kb, "44"); assert(!strcmp(f_ta(kb), "h"));
    frame(T9_TAP_MS + 100);
    f_t9_keys(kb, "444"); assert(!strcmp(f_ta(kb), "hi"));
    frame(T9_TAP_MS + 100);
    /* held: the digit */
    int k7 = f_t9_key(kb, KA_T9, '7');
    kb->hot = k7; lv_obj_send_event(kb->keys, LV_EVENT_LONG_PRESSED, NULL); frame(50);
    assert(!strcmp(f_ta(kb), "hi7"));
    /* a password: never predictive */
    kb->kind = KB_PASSWORD; global_settings.sloppy_kb_t9 = 1;
    assert(!t9_predictive(kb)); kb->kind = KB_TEXT;
    /* the globe: back to the other keyboard */
    kb_press_key(kb, f_t9_key(kb, KA_GLOBE, 0)); frame(100); assert(kb->lay == KB_EN);
    while (npages > 1) { page_pop(); frame(300); }
    /* Settings: a keyboard to choose, its switch, Settings search */
    kbs_pick_page(0); frame(400); assert(find_label(pages[npages - 1], sl_tr("Phone keypad (T9)")));
    while (npages > 1) { page_pop(); frame(300); }
    keyboards_page(); frame(400); assert(find_label(pages[npages - 1], sl_tr("Predictive (T9)")));
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_kb_t9 = t0; global_settings.sloppy_kb_first = f0; global_settings.sloppy_kb_second = s0; global_settings.sloppy_language = l0;
    printf("PASS: phone keypad: twelve keys; predictive words from the dictionary (hello, good or home from the bar), Delete a key at a time, a capital, punctuation on 1; multi-tap round a key's letters; held, its digit; never predictive in a password.\n");
}
#endif

static void f_tests(void) {
#ifndef SL_F_BASE
    f_np_album_tests();
    f_clock_tests();
    f_corner_tests();
    f_batlog_tests();
    f_erase_tests();
    f_saver_tests();
    f_car_tests();
    f_flip_tests();
    f_t9_tests();
#endif
}

/* ---------------------------------------------------------- the audit's screens */
static void f_au_np_album(void) {
#ifndef SL_F_BASE
    f_au_dirty = true; global_settings.sloppy_np_album = 1; now_playing_retheme();
#endif
    f_np_open();
}
static void f_au_batlog(void) {
#ifndef SL_F_BASE
    f_au_dirty = true; global_settings.sloppy_battery_log = 1; f_bl_day((long)time(NULL));
    time_power_page(); frame(400);
    lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
    lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF);
#else
    time_power_page();
#endif
}
static void f_au_erase(void) {
#ifndef SL_F_BASE
    er_page();
#else
    advanced_page();
#endif
}
static bool f_au_saver_run0, f_au_saver_on;
static void f_au_saver(int style) {
#ifndef SL_F_BASE
    if (!style) { saver_page(); return; }
    f_au_saver_on = true; f_au_saver_run0 = sl_power_running;
    f_sv_ready(style, true); f_sv_idle();
#else
    (void)style; appearance_page();
#endif
}
static bool f_au_car_on;
static void f_au_car(int which) {
#ifndef SL_F_BASE
    f_au_car_on = true; global_settings.sloppy_car = CAR_ON;
    if (which == 0) car_show();
    else if (which == 1) { car.off_at = sl_ms() + 30000; car_off_show(); car_off_count(); }
    else car_page();
#else
    (void)which; time_power_page();
#endif
}
static void f_au_reset(void) {
#ifndef SL_F_BASE
    if (f_au_saver_on) {
        f_au_saver_on = false;
        sv_hide(); global_settings.sloppy_saver = 0;
        sl_power_state_init(&sl_power, sl_ms()); sl_power_overlay_show(false); sl_power_apply();
        sl_power_running = f_au_saver_run0; frame(500);
    }
#endif
#ifndef SL_F_BASE
    if (f_au_car_on) {
        f_au_car_on = false; car_hide(); car.off_at = 0;
        if (car_off_shown()) lv_obj_add_flag(car.off, LV_OBJ_FLAG_HIDDEN);
        global_settings.sloppy_car = 0;
    }
#endif
    if (!f_au_dirty) return;
    f_au_dirty = false;
#ifndef SL_F_BASE
    global_settings.sloppy_np_album = 0; now_playing_retheme();
    remove(BL_FILE);
#endif
}

/* ---------------------------------------------------------- the screenshots */
static void f_shots(const char *dir, const char *tag) {
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    const char *only = getenv("SLOPPY_AUDIT_ONLY");
    au_library();
    for (int look = 0; look < 2; look++) {
        const char *lk = look ? "light" : "dark";
        if (!only || strstr("np-album", only)) {
            static const char *const styles[] = { "glass", "classic", "centred" };
            for (int st = 0; st < 3; st++) {
                au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
                global_settings.sloppy_np_style = st; now_playing_retheme();
                f_np_open(); au_settle();
                snprintf(p, sizeof p, "np-%s", styles[st]); f_shot(dir, p, lk, tag);
#ifndef SL_F_BASE
                global_settings.sloppy_np_album = 1; now_playing_retheme(); au_settle();
                snprintf(p, sizeof p, "np-%s-album", styles[st]); f_shot(dir, p, lk, tag);
                global_settings.sloppy_np_album = 0; now_playing_retheme();
#endif
                np_hide(0); frame(400);
            }
            global_settings.sloppy_np_style = 0; now_playing_retheme();
            /* R4/R13: the status bar over the cover, blended (a shade under it), checked as it was */
            { bool ra = test_real_art; test_real_art = true; if (!np_img_buf) np_img_buf = calloc(W * W, 2);
              global_settings.sloppy_np_status = 1; np_img_album = -1; np_last_path[0] = '\0'; now_playing_retheme();
              f_np_open(); au_settle(); f_shot(dir, "np-statusbar-over-cover", lk, tag);
              np_hide(0); frame(400); global_settings.sloppy_np_status = 0; now_playing_retheme(); test_real_art = ra; }
            np_style_page(); au_settle(); f_shot(dir, "settings-now-playing", lk, tag);
            au_reset();
        }
        if (!only || strstr("corners", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0; test_status = AUDIO_STATUS_PLAY;
            au_open_home(AU_HOME); f_shot(dir, "home-corners", lk, tag);
#ifndef SL_F_BASE
            global_settings.sloppy_round_corners = 1; sl_corners_apply(); lv_obj_invalidate(lv_screen_active()); frame(100);
            f_shot(dir, "home-corners-on", lk, tag);
            f_np_open(); au_settle(); f_shot(dir, "np-corners-on", lk, tag); np_hide(0); frame(400);
            theme_page(); au_settle(); f_shot(dir, "settings-theme-corners-on", lk, tag);
            global_settings.sloppy_round_corners = 0; sl_corners_apply();
#else
            theme_page(); au_settle(); f_shot(dir, "settings-theme", lk, tag);
#endif
            au_reset();
        }
#ifndef SL_F_BASE
        if (!only || strstr("battery", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0; test_status = AUDIO_STATUS_PLAY;
            global_settings.sloppy_battery_log = 1;   /* its default */
            f_bl_day((long)time(NULL));
            time_power_page(); au_settle();
            lv_obj_t *l = find_label(pages[npages - 1], sl_tr("BATTERY HISTORY"));
            if (!l) l = find_label(pages[npages - 1], "BATTERY HISTORY");
            if (l) e_scroll_to(l);
            f_shot(dir, "settings-battery-history", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            remove(BL_FILE);
            time_power_page(); au_settle();
            l = find_label(pages[npages - 1], "BATTERY HISTORY");
            if (l) e_scroll_to(l);
            f_shot(dir, "settings-battery-history-empty", lk, tag);
            au_reset();
        }
#endif
#ifndef SL_F_BASE
        if (!only || strstr("erase", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0; test_status = AUDIO_STATUS_PLAY;
            advanced_page(); au_settle(); f_shot(dir, "settings-advanced", lk, tag);
            er_page(); au_settle(); f_shot(dir, "erase-page", lk, tag);
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF); frame(300); f_shot(dir, "erase-page-end", lk, tag);
            au_reset();
        }
#else
        if (!only || strstr("erase", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0; test_status = AUDIO_STATUS_PLAY;
            advanced_page(); au_settle(); f_shot(dir, "settings-advanced", lk, tag);
            au_reset();
        }
#endif
#ifndef SL_F_BASE
        if (!only || strstr("saver", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            static const char *const sn[] = { "", "saver-floating", "saver-square", "saver-clock" };
            for (int st = 1; st <= 3; st++) {
                f_sv_ready(st, true); f_sv_idle();
                if (st == 1) for (int i = 0; i < 40; i++) sv_timer_cb(NULL);   /* 40 s of drift */
                frame(300); f_shot(dir, sn[st], lk, tag);
                sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600);
            }
            f_sv_ready(1, false); f_sv_idle(); frame(300); f_shot(dir, "saver-no-cover", lk, tag);
            sl_power_filter_touch(true, 240, 600); sl_power_filter_touch(false, 240, 600); frame(300);
            f_shot(dir, "saver-touched-lock", lk, tag);
            global_settings.sloppy_saver = 0; sl_power_running = false; sl_power_state_init(&sl_power, sl_ms());
            sl_power_overlay_show(false); frame(500);
            saver_page(); au_settle(); f_shot(dir, "settings-screensaver", lk, tag);
            appearance_page(); au_settle(); f_shot(dir, "settings-customization", lk, tag);
            au_reset();
        }
#else
        if (!only || strstr("saver", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            appearance_page(); au_settle(); f_shot(dir, "settings-customization", lk, tag);
            au_reset();
        }
#endif
#ifndef SL_F_BASE
        if (!only || strstr("car", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            bool ra = test_real_art; test_real_art = true; if (!np_img_buf) np_img_buf = calloc(W * W, 2);
            np_img_album = -1; np_last_path[0] = '\0'; f_np_open(); np_hide(0); frame(400); test_real_art = ra;
            car_show(); au_settle(); f_shot(dir, "car-controls", lk, tag); car_hide();
            global_settings.sloppy_car = CAR_ON; car.off_at = sl_ms() + 30000; car_off_show(); car_off_count(); au_settle();
            f_shot(dir, "car-power-gone", lk, tag);
            lv_obj_add_flag(car.off, LV_OBJ_FLAG_HIDDEN); car.off_at = 0;
            car_page(); au_settle(); f_shot(dir, "settings-car-mode", lk, tag);
            global_settings.sloppy_car = 0;
            au_reset();
        }
#endif
#ifndef SL_F_BASE
        if (!only || strstr("flip", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            buttons_page(); au_settle();
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF); frame(300);
            f_shot(dir, "settings-buttons-upside-down", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            au_open_home(AU_HOME); f_shot(dir, "home-upside-down-framebuffer", lk, tag);
            au_reset();
        }
#endif
#ifndef SL_F_BASE
        if (!only || strstr("t9", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            int f0 = global_settings.sloppy_kb_first, s0 = global_settings.sloppy_kb_second, t0 = global_settings.sloppy_kb_t9;
            global_settings.sloppy_kb_first = KB_T9 + 1; global_settings.sloppy_kb_second = KB_EN + 1; global_settings.sloppy_kb_t9 = 1;
            eq_save_page(NULL); au_settle();
            struct kb *kb = kb_now();
            lv_textarea_set_text(kb->ta, ""); kb_set_shift(kb, 0);
            f_shot(dir, "keypad-t9", lk, tag);
            f_t9_keys(kb, "43556 4663"); frame(300); f_shot(dir, "keypad-t9-typing", lk, tag);
            t9_commit(kb); kb_press_key(kb, f_t9_key(kb, KA_T9MODE, 0)); frame(300); f_shot(dir, "keypad-multitap", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
            keyboards_page(); au_settle();
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF); frame(300); f_shot(dir, "settings-keyboards", lk, tag);
            global_settings.sloppy_kb_first = f0; global_settings.sloppy_kb_second = s0; global_settings.sloppy_kb_t9 = t0;
            au_reset();
        }
#else
        if (!only || strstr("t9", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            keyboards_page(); au_settle();
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_scroll_to_y(b, LV_COORD_MAX, LV_ANIM_OFF); frame(300); f_shot(dir, "settings-keyboards", lk, tag);
            au_reset();
        }
#endif
        if (!only || strstr("hairline", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            scr_page(); au_settle(); f_shot(dir, "hairline-scrobbling", lk, tag);
            au_reset();
        }
        if (!only || strstr("clock", only)) {
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            test_status = AUDIO_STATUS_PLAY; global_settings.battery_display = 1;
            au_open_home(AU_HOME);
            static const char *const pos[] = { "left", "centre", "right" };
            for (int k = 0; k < 3; k++) {
#ifndef SL_F_BASE
                global_settings.sloppy_clock_pos = k;
#else
                if (k) break;
#endif
                statusbar_update(); frame(200);
                snprintf(p, sizeof p, "statusbar-clock-%s", pos[k]); f_shot(dir, p, lk, tag);
            }
            ccx_open(); au_settle(); f_shot(dir, "cc-clock-right", lk, tag); au_reset();
            au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
            time_power_page(); au_settle(); f_shot(dir, "settings-time-power", lk, tag);
#ifndef SL_F_BASE
            global_settings.sloppy_clock_pos = 0;
#endif
            global_settings.battery_display = 0; statusbar_update();
            au_reset();
        }
    }
    au_library_done();
}
