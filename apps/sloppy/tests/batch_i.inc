/* Batch I (firmware, codecs and the Arcade): included by ui_test.c after
 * ui_audit.inc.
 *   SLOPPY_I_TESTS=1                                    the tests alone
 *   SLOPPY_I_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after   the screenshots */

/* RGB565 as the Game Boy core makes it from a palette's 0xRRGGBB (8 bits
 * to 5, green widened to 6 by its top bit) */
static uint16_t i_gb565(uint32_t rgb) {
    unsigned r = (rgb >> 19) & 31, g = (rgb >> 11) & 31, b = (rgb >> 3) & 31;
    return (uint16_t)(r << 11 | ((g << 1) | (g >> 4)) << 5 | b);
}
/* every pixel of the game's picture is one of palette k's colours */
static bool i_picture_in(int k) {
    uint16_t c[16]; int n = 0;
    for (int set = 0; set < 4; set++) for (int s = 0; s < 4; s++) c[n++] = i_gb565(gb_palette_rgb(k, set, s));
    const uint16_t *pic = gb_picture();
    for (int i = 0; i < GB_W * GB_H; i++) {
        bool hit = false;
        for (int j = 0; j < n && !hit; j++) hit = pic[i] == c[j];
        if (!hit) return false;
    }
    return true;
}
static void i_arcade_tests(void) {
    while (npages > 1) { page_pop(); frame(300); }
    int pal0 = arc_opt.pal, fx0 = arc_opt.fx;
    b9_arcade_game(true);
    assert(arc.page && arc.sheet && arc.paused && acts_open_now && !gb_color());
    assert(has_text(acts, "Colours") && has_text(acts, "Screen effect"));
    /* Colours: eighteen swatches, ours then the Game Boy Color's, the one in use ringed */
    lv_obj_t *colours = lv_obj_get_parent(find_label(acts, "Colours: Classic") ? find_label(acts, "Colours: Classic") : find_label(acts, "Colours: Pocket"));
    arc_opt.pal = 0; gb_palette(0);
    lv_obj_send_event(colours, LV_EVENT_CLICKED, NULL); frame(400);
    assert(acts_open_now && arc.paused && find_label(acts, "GABAGOOLOS") && find_label(acts, "GAME BOY COLOR") && find_label(acts, "Reversed"));
    for (int k = 0; k < GB_PALETTES; k++) assert(arc_sw[k] && lv_obj_get_style_border_opa(arc_sw[k], 0) == (k == 0 ? LV_OPA_COVER : LV_OPA_TRANSP));
    {   /* all of the sheet on the screen, its swatches four to a row */
        lv_area_t a, b; lv_obj_get_coords(acts, &a); assert(a.y1 >= 0 && a.y2 < H);
        lv_obj_get_coords(arc_sw[0], &a); lv_obj_get_coords(arc_sw[3], &b); assert(a.y1 == b.y1 && abs(b.x2 - a.x1) > 3 * ARC_SW_W - 8);
        lv_obj_get_coords(arc_sw[4], &b); assert(b.y1 > a.y2);
    }
    /* a Game Boy Color palette: the picture behind the sheet changes at once, still paused */
    uint32_t f0 = arc.frames;
    int up_a = 7;                                   /* Up + A: red, its sprites green and blue */
    assert(!strcmp(arc_pal_names[up_a], "Red") && gb_palette_rgb(up_a, 2, 1) != gb_palette_rgb(up_a, 0, 1));
    lv_obj_send_event(lv_obj_get_parent(arc_sw[up_a]), LV_EVENT_CLICKED, NULL); frame(100);
    assert(arc_opt.pal == up_a && i_picture_in(up_a) && !i_picture_in(0));
    assert(lv_obj_get_style_border_opa(arc_sw[up_a], 0) == LV_OPA_COVER && lv_obj_get_style_border_opa(arc_sw[0], 0) == LV_OPA_TRANSP);
    for (int i = 0; i < 10; i++) frame(17);
    assert(arc.frames == f0 && acts_open_now);
    /* each of the eighteen paints the picture in its own colours */
    for (int k = 0; k < GB_PALETTES; k++) {
        lv_obj_send_event(lv_obj_get_parent(arc_sw[k]), LV_EVENT_CLICKED, NULL);
        assert(i_picture_in(k));
    }
    lv_obj_send_event(lv_obj_get_parent(arc_sw[up_a]), LV_EVENT_CLICKED, NULL); frame(100);
    /* Done: back to the paused menu, which names it */
    arc_sw_done_cb(NULL); frame(400);
    assert(acts_open_now && find_label(acts, "Colours: Red") && find_label(acts, "Screen effect: Off"));
    /* the screen's effect: Off, the LCD's pixel grid, scanlines, and round again */
    lv_obj_t *fx = lv_obj_get_parent(find_label(acts, "Screen effect: Off"));
    uint16_t base = arc.px[1 * ARC_PIC_W + 1];      /* inside the first pixel: never darkened */
    const uint16_t *pic = gb_picture();
    lv_obj_send_event(fx, LV_EVENT_CLICKED, NULL); frame(17);
    assert(arc_opt.fx == ARC_FX_GRID && find_label(acts, "Screen effect: Pixel grid"));
    assert(arc.px[1 * ARC_PIC_W + 1] == base && arc.px[1 * ARC_PIC_W + 2] == arc_dim(pic[0]) && arc.px[2 * ARC_PIC_W] == arc_dim(pic[0]));
    lv_obj_send_event(fx, LV_EVENT_CLICKED, NULL); frame(17);
    assert(arc_opt.fx == ARC_FX_SCAN && find_label(acts, "Screen effect: Scanlines"));
    assert(arc.px[1 * ARC_PIC_W + 2] == pic[0] && arc.px[2 * ARC_PIC_W] == arc_half(pic[0]) && arc.px[2 * ARC_PIC_W + 2] == arc_half(pic[0]));
    /* the choices are kept */
    arc_cfg_save();
    {   char b[256] = ""; int f = open(ARC_CFG, O_RDONLY); assert(f >= 0); ssize_t n = read(f, b, sizeof b - 1); sim_close(f);
        assert(n > 0); b[n] = 0; char want[32]; snprintf(want, sizeof want, "ARCADE1 %d %d ", up_a, ARC_FX_SCAN); assert(!strncmp(b, want, strlen(want))); }
    lv_obj_send_event(fx, LV_EVENT_CLICKED, NULL); frame(17);
    assert(arc_opt.fx == ARC_FX_NONE && arc.px[2 * ARC_PIC_W] == pic[0]);
    /* the blit's cost in each look, at the R1's estimate */
    for (int m = 0; m < ARC_FX_N; m++) {
        arc_opt.fx = m;
        uint64_t t0 = sl_perf_now_us(); for (int i = 0; i < 300; i++) arc_blit();
        double ms = (sl_perf_now_us() - t0) / 300000.0;
        printf("arcade 3x scale, %s: %.3f ms a frame on this Mac (%.1f ms est. on the R1)\n", arc_fx_names[m], ms, ms * PF_K);
    }
    arc_opt.fx = ARC_FX_NONE;
    /* a Game Boy Color cartridge has its own colours: no Colours row */
    arc_menu_close(); frame(400); page_pop(); frame(400);
    arc_play(0); frame(200);
    assert(gb_color()); arc_screen_cb(NULL); frame(400);
    assert(acts_open_now && !has_text(acts, "Colours") && has_text(acts, "Screen effect"));
    arc_menu_close(); frame(400);
    while (npages > 1) { page_pop(); frame(300); }
    arc_opt.pal = pal0; arc_opt.fx = fx0; gb_palette(pal0); arc_cfg_save();
    printf("PASS: Arcade colours and screen effects: eighteen palettes (ours and the Game Boy Color's twelve, sprites coloured apart), "
           "shown at once while paused, the pixel grid and scanlines, kept; none for a Game Boy Color cartridge.\n");
}
/* NOS in Sound > DAC: a switch above the filter, which hides while NOS is on */
static lv_obj_t *i_nos_switch(void) {
    lv_obj_t *l = find_label(pages[npages - 1], "NOS");
    if (!l) return NULL;
    lv_obj_t *r = lv_obj_get_parent(l);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++)
        if (lv_obj_check_type(lv_obj_get_child(r, (int32_t)i), &lv_switch_class)) return lv_obj_get_child(r, (int32_t)i);
    return NULL;
}
static void i_nos_tests(void) {
    while (npages > 1) { page_pop(); frame(300); }
    int nos0 = global_settings.sloppy_dac_nos;
    global_settings.sloppy_dac_nos = 0;
    sound_page(); frame(300);
    lv_obj_t *sw = i_nos_switch();
    assert(sw && !lv_obj_has_state(sw, LV_STATE_CHECKED) && find_label(pages[npages - 1], "Turns off the DAC's oversampling filter"));
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    assert(dac_filter_row && !lv_obj_has_flag(dac_filter_row, LV_OBJ_FLAG_HIDDEN) && find_label(dac_filter_row, "Super slow"));
    {   lv_area_t a, b; lv_obj_get_coords(sw, &a); lv_obj_get_coords(dac_filter_row, &b); assert(a.y2 < b.y1); }
#else
    assert(!dac_filter_row);   /* this build's DAC has no filter control: the switch stands alone */
#endif
    {   /* flipping it moves nothing under the finger */
        lv_area_t a, a2; lv_obj_get_coords(sw, &a);
        lv_obj_send_event(lv_obj_get_parent(sw), LV_EVENT_CLICKED, NULL); frame(100);
        lv_obj_get_coords(sw, &a2); assert(a2.y1 == a.y1);
    }
    assert(global_settings.sloppy_dac_nos == 1 && lv_obj_has_state(sw, LV_STATE_CHECKED));
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    assert(lv_obj_has_flag(dac_filter_row, LV_OBJ_FLAG_HIDDEN));
#endif
    /* kept: the page again says so */
    page_pop(); frame(300); sound_page(); frame(300);
    sw = i_nos_switch();
    assert(sw && lv_obj_has_state(sw, LV_STATE_CHECKED));
    lv_obj_send_event(lv_obj_get_parent(sw), LV_EVENT_CLICKED, NULL); frame(100);
    assert(global_settings.sloppy_dac_nos == 0 && !lv_obj_has_state(sw, LV_STATE_CHECKED));
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    assert(!lv_obj_has_flag(dac_filter_row, LV_OBJ_FLAG_HIDDEN));
#endif
    /* the settings search finds it under the DAC's filter */
    while (npages > 1) { page_pop(); frame(300); }
    settings_search_page(); frame(300); sq_add("N"); sq_add("O"); sq_add("S"); frame(300);
    assert(find_label(pages[npages - 1], "DAC filter"));
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_dac_nos = nos0;
    printf("PASS: NOS in Sound > DAC: a switch where the driver has NOS_EN (the filter hidden while it is on, where there is one), kept, found by search.\n");
}
/* Your boot logo: a picture in Bootlogo, fitted once after a boot, then on the splash */
static void i_put(const char *path, int bytes) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0);
    static char junk[4096]; memset(junk, 0x5a, sizeof junk);
    while (bytes > 0) { int n = bytes < (int)sizeof junk ? bytes : (int)sizeof junk; assert(write(fd, junk, n) == n); bytes -= n; }
    sim_close(fd);
}
static bool i_exists(const char *p) { int fd = open(p, O_RDONLY); if (fd < 0) return false; sim_close(fd); return true; }
static void i_bootlogo_after_boot(void) {
    lv_timer_create(bootlogo_tick, 10, NULL);
    for (int i = 0; i < 40; i++) frame(20);
}
static void i_splash_gone(void) { splash_fade_out(); for (int i = 0; i < 40; i++) frame(20); }
static bool i_stamp_has(const char *s) {
    char b[600] = ""; int fd = open(BOOTLOGO_STAMP, O_RDONLY); if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof b - 1); sim_close(fd); if (n > 0) b[n] = 0; return strstr(b, s) != NULL;
}
#define I_LOGO FAKE_CARD "/Bootlogo"
static void i_bootlogo_tests(void) {
    bootlogo_root = FAKE_CARD;
    remove(I_LOGO "/My logo.png"); remove(I_LOGO "/._My logo.png"); remove(I_LOGO "/zz.jpg"); remove(I_LOGO "/notes.txt");
    remove(BOOTLOGO_CACHE); remove(BOOTLOGO_STAMP);
    /* none: the wordmark */
    splash_build(); frame(0);
    assert(!bootlogo_px && splash_word && !lv_obj_check_type(splash_word, &lv_image_class));
    i_splash_gone();
    /* a picture: fitted after the boot, the first by name (a Mac's ._ shadow and other files passed by) */
    mkdir(I_LOGO);
    i_put(I_LOGO "/zz.jpg", 2000); i_put(I_LOGO "/My logo.png", 3000); i_put(I_LOGO "/._My logo.png", 400); i_put(I_LOGO "/notes.txt", 10);
    splash_build(); frame(0);
    assert(!bootlogo_px && !lv_obj_check_type(splash_word, &lv_image_class));   /* not fitted yet: the wordmark this time */
    i_splash_gone();
    i_bootlogo_after_boot();
    assert(!strcmp(test_wall_in, I_LOGO "/My logo.png") && i_exists(BOOTLOGO_CACHE) && i_stamp_has(I_LOGO "/My logo.png 3000 "));
    /* the next start: the picture, in the middle, nothing decoded */
    splash_build(); frame(0);
    assert(bootlogo_px && lv_obj_check_type(splash_word, &lv_image_class) && bootlogo_dsc.header.w == 300 && bootlogo_dsc.header.h == 150);
    {   lv_area_t a; lv_obj_update_layout(splash_word); lv_obj_get_coords(splash_word, &a);
        assert(a.x1 == (W - 300) / 2 && a.y1 == (H - 150) / 2); }
    i_splash_gone();
    assert(!bootlogo_px);                /* let go with the splash */
    /* the same picture: nothing to make */
    test_wall_in[0] = 0; i_bootlogo_after_boot(); assert(!test_wall_in[0]);
    /* changed (another size): made again, the stamp says which */
    i_put(I_LOGO "/My logo.png", 5000);
    splash_build(); frame(0); assert(!bootlogo_px); i_splash_gone();   /* not this one any more */
    i_bootlogo_after_boot();
    assert(!strcmp(test_wall_in, I_LOGO "/My logo.png") && i_stamp_has(I_LOGO "/My logo.png 5000 "));
    /* emptied: forgotten, the wordmark back */
    remove(I_LOGO "/My logo.png"); remove(I_LOGO "/._My logo.png"); remove(I_LOGO "/zz.jpg"); remove(I_LOGO "/notes.txt");
    i_bootlogo_after_boot();
    assert(!i_exists(BOOTLOGO_CACHE) && !i_exists(BOOTLOGO_STAMP));
    splash_build(); frame(0); assert(!bootlogo_px && !lv_obj_check_type(splash_word, &lv_image_class)); i_splash_gone();
    bootlogo_root = "";
    printf("PASS: boot logo: the first picture in Bootlogo fitted after a boot, on the next splash in the middle, made again when it changes, the wordmark back when the folder empties.\n");
}
/* The plan's "NES title" was Super Mario Land, a Game Boy cartridge the
 * Playroom once opened in classic Rockbox from Games/mario.gb: that file at
 * the top of Games is in the Arcade's list and plays here */
static void i_mario_tests(void) {
    while (npages > 1) { page_pop(); frame(300); }
    b9_arcade_card(true);
    static uint8_t rom[32768];
    gbt_make(rom, "MARIO", 0);
    int fd = open(FAKE_CARD "/Games/mario.gb", O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0);
    assert(write(fd, rom, sizeof rom) == (ssize_t)sizeof rom); sim_close(fd);
    arcade_page(); frame(300);
    int k = -1;
    for (int i = 0; i < arc_n; i++) if (!strcmp(arc_roms[i].path, FAKE_CARD "/Games/mario.gb")) k = i;
    assert(k >= 0 && find_label(pages[npages - 1], arc_roms[k].name));
    arc_play(k); frame(100);
    for (int i = 0; i < 20; i++) frame(17);
    assert(arc.page && gb_running() && !gb_color() && arc.frames > 10);
    while (npages > 1) { page_pop(); frame(300); }
    remove(FAKE_CARD "/Games/mario.gb");
    printf("PASS: Games/mario.gb, where the old Playroom looked for Super Mario Land, is in the Arcade's list and plays there.\n");
}
static void i_tests(void) {
    i_arcade_tests();
    i_mario_tests();
    i_nos_tests();
    i_bootlogo_tests();
}

/* ---- screenshots ---- */
static void i_shot(const char *dir, const char *name, const char *look, const char *tag) {
    char p[600]; frame(0); snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, name, look, tag); shot_now(p);
}
static void i_shots(const char *dir, const char *tag) {
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (int look = 0; look < 2; look++) {
        const char *lk = look ? "light" : "dark";
        au_apply(look, 0, 0);
        int pal0 = arc_opt.pal, fx0 = arc_opt.fx;
        arc_opt.pal = 0; arc_opt.fx = ARC_FX_NONE;
        b9_arcade_game(true); au_settle();
        i_shot(dir, "arcade-menu", lk, tag);
        arc_pal_cb(NULL); au_settle();
        i_shot(dir, "arcade-colours", lk, tag);
        lv_obj_send_event(lv_obj_get_parent(arc_sw[7]), LV_EVENT_CLICKED, NULL); au_settle();
        i_shot(dir, "arcade-colours-red", lk, tag);
        arc_menu_close(); au_settle();
        static const char *const fxn[ARC_FX_N] = { "sharp", "grid", "scanlines" };
        for (int m = 0; m < ARC_FX_N; m++) {
            arc_opt.fx = m; arc_blit(); frame(17);
            snprintf(p, sizeof p, "arcade-game-red-%s", fxn[m]); i_shot(dir, p, lk, tag);
        }
        arc_opt.pal = 11; gb_palette(11); gb_frame(0, true); arc_opt.fx = ARC_FX_NONE; arc_blit(); frame(17);
        i_shot(dir, "arcade-game-grey", lk, tag);
        while (npages > 1) { page_pop(); frame(300); }
        arc_opt.pal = pal0; arc_opt.fx = fx0; gb_palette(pal0);
        au_reset();
        /* Sound > DAC, NOS off then on */
        au_apply(look, 0, 0);
        int nos0 = global_settings.sloppy_dac_nos;
        for (int on = 0; on < 2; on++) {
            global_settings.sloppy_dac_nos = on;
            sound_page(); au_settle();
            lv_obj_t *dac = find_label(pages[npages - 1], "NOS");
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_area_t la, ba; lv_obj_get_coords(dac, &la); lv_obj_get_coords(b, &ba);
            lv_obj_scroll_to_y(b, lv_obj_get_scroll_y(b) + la.y1 - ba.y1 - 120, LV_ANIM_OFF); frame(300);
            i_shot(dir, on ? "settings-sound-dac-nos-on" : "settings-sound-dac", lk, tag);
            while (npages > 1) { page_pop(); frame(300); }
        }
        global_settings.sloppy_dac_nos = nos0;
        au_reset();
        /* the splash: the wordmark, then your boot logo */
        au_apply(look, 0, 0);
        remove(BOOTLOGO_CACHE); remove(BOOTLOGO_STAMP);
        splash_build(); frame(0); i_shot(dir, "splash-wordmark", lk, tag); i_splash_gone();
        bootlogo_root = FAKE_CARD; mkdir(I_LOGO); i_put(I_LOGO "/My logo.png", 3000); i_bootlogo_after_boot();
        splash_build(); frame(0); i_shot(dir, "splash-bootlogo", lk, tag); i_splash_gone();
        remove(I_LOGO "/My logo.png"); i_bootlogo_after_boot(); bootlogo_root = "";
        au_reset();
    }
    au_library_done();
}
