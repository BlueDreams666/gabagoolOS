/*
 *  List home: a Zune HD style start screen (Customization > Home layout >
 *  List). The places are words, big and lowercase, one under another,
 *  set close to the left edge with room around them; a press lights a word
 *  hot pink and nudges it along. Over them, while something plays, a line
 *  says what it is. Below them, as a second, dimmer heading, the apps.
 *  A thin ribbon in the spectrum analyzer's Zune colours (violet through
 *  magenta and pink to orange and gold) runs under the status bar.
 *
 *  Everything moves by opacity and translation only (a scaled object is
 *  drawn into a layer on every frame on the R1): the words slide in from
 *  the right one after another, as the Zune's menu did.
 */

#define ZN_M     22                      /* the words' left margin */

static const uint32_t zn_ribbon[] = { 0x5a24d8, 0xb21ba0, 0xf2287a, 0xff6b2c, 0xffcd52 };

static struct
{
    lv_obj_t *page, *np, *np_title, *np_artist;
    lv_timer_t *timer;
    char np_path[MAX_PATH];
    uint32_t ink;
    int shown;                           /* rows slid in so far, for the stagger */
} zn;

static void zn_set_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void zn_slide_in(lv_obj_t *o)
{
    unsigned delay = home_intro_ms + (unsigned)(zn.shown++ < 10 ? zn.shown * 45 : 450);
    lv_obj_set_style_translate_x(o, 56, 0);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, 56, 0);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_delay(&a, delay);
    motion_decel(&a);
    lv_anim_set_exec_cb(&a, zn_set_tx);
    lv_anim_start(&a);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, home_tile_fade);
    lv_anim_start(&a);
}

/* A row that lights pink and moves along a little while pressed; its
 * labels take the row's text colour, so they light with it. */
static lv_obj_t *zn_row(lv_obj_t *body, int k)
{
    lv_obj_t *r = box(body);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(r, ZN_M, 0);
    lv_obj_set_style_pad_right(r, ZN_M, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(zn.ink), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(ZN_PINK), LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(r, 10, LV_STATE_PRESSED);
    press_motion(r);
    on_click(r, tile_cb, (void *)(intptr_t)k);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}
static lv_obj_t *zn_label(lv_obj_t *parent, const char *s, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    if (sl_has_arabic(s)) lv_obj_set_style_pad_bottom(l, sl_line_h(f) - lv_font_get_line_height(f), 0);   /* Arabic hangs lower than the row allowed */
    return l;
}

/* One of the places: a word, big */
static void zn_place(lv_obj_t *body, const char *name, int k)
{
    char low[64];
    const char *s = sl_lower(sl_tr(name), low, sizeof low);
    lv_obj_t *r = zn_row(body, k);
    lv_obj_set_style_pad_ver(r, 4, 0);
    lv_obj_t *l = zn_label(r, s, sl_zune_font(s, &ss_regular_60));
    lv_obj_set_style_text_line_space(l, -6, 0);
    zn_slide_in(r);
}
/* An app: its name, lowercase, a size under the places */
static void zn_app(lv_obj_t *body, const char *name, const char *note, int k)
{
    (void)note;
    char low[64];
    const char *t = sl_lower(sl_tr(name), low, sizeof low);
    lv_obj_t *r = zn_row(body, k);
    lv_obj_set_style_pad_ver(r, 6, 0);
    zn_label(r, t, sl_zune_font(t, &ss_regular_34));
    zn_slide_in(r);
}

static void zn_heading(lv_obj_t *body, const char *name);
/* history: the albums played last, small square covers in a row, as on
 * the Zune HD's start screen */
static void zn_history(lv_obj_t *body)
{
    int albums[3], na = hist_recent_albums(albums, 3);
    if (!na) return;
    zn_heading(body, "History");
    lv_obj_t *row = box(body);
    lv_obj_set_size(row, lv_pct(100), SL_ART_SMALL);
    lv_obj_set_style_pad_left(row, ZN_M, 0);
    lv_obj_set_style_margin_top(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    for (int i = 0; i < na; i++)
    {
        lv_obj_t *c = cover(row, albums[i], SL_ART_SMALL, false);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_translate_y(c, 4, LV_STATE_PRESSED);
        on_click(c, album_open_cb, (void *)(intptr_t)albums[i]);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    zn_slide_in(row);
}
/* A heading: the Zune's big, dim, lowercase kind */
static void zn_heading(lv_obj_t *body, const char *name)
{
    char low[64];
    const char *s = sl_lower(sl_tr(name), low, sizeof low);
    lv_obj_t *h = zn_label(body, s, sl_zune_font(s, &ss_regular_60));
    lv_obj_set_style_text_color(h, lv_color_hex(zn.ink), 0);
    lv_obj_set_style_text_opa(h, sl_theme() == 1 ? 140 : 90, 0);   /* faint, and still 3:1 on the light page */
    lv_obj_set_style_pad_left(h, ZN_M - 3, 0);
    lv_obj_set_style_margin_top(h, 26, 0);
    zn_slide_in(h);
}

/* What is playing, over the words; gone when nothing is */
static void zn_np_update(void)
{
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    const char *path = id3 ? id3->path : "";
    if (!strcmp(path, zn.np_path)) return;
    strlcpy(zn.np_path, path, sizeof zn.np_path);
    hidden_if_changed(zn.np, !id3);
    if (!id3) return;
    const char *slash = strrchr(id3->path, '/');
    lv_label_set_text(zn.np_title, id3->title && *id3->title ? id3->title : slash ? slash + 1 : id3->path);
    lv_label_set_text(zn.np_artist, id3->artist && *id3->artist ? id3->artist : "");
}
static void zn_tick(lv_timer_t *t)
{
    (void)t;
    if (npages == 1 && !overlay_visible()) zn_np_update();
}
static void zn_np_cb(lv_event_t *e) { (void)e; np_show(); }
static void zn_deleted(lv_event_t *e)
{
    (void)e;
    if (zn.timer) lv_timer_delete(zn.timer);
    memset(&zn, 0, sizeof zn);
}

static void zune_home_page(void)
{
    memset(&zn, 0, sizeof zn);
    zn.ink = wallpaper_id(false) >= 5 ? 0xffffff : C_TEXT;   /* on the photos, white like the status bar */
    lv_obj_t *p = zn.page = box(ui_screen());
    lv_obj_add_style(p, &st_page, 0);
    lv_obj_set_size(p, W, H);
    lv_obj_add_event_cb(p, sl_wallpaper_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);

    /* the ribbon, under the status bar: four short gradients end to end */
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *b = box(p);
        lv_obj_set_pos(b, i * W / 4, SB_H);
        lv_obj_set_size(b, (i + 1) * W / 4 - i * W / 4, 3);
        lv_obj_set_style_bg_color(b, lv_color_hex(zn_ribbon[i]), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(zn_ribbon[i + 1]), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }

    lv_obj_t *body = lv_obj_create(p);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, SB_H + 3);
    lv_obj_set_size(body, W, H - SB_H - 3);
    lv_obj_add_event_cb(body, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_top(body, 14, 0);
    lv_obj_set_style_pad_bottom(body, 130, 0);   /* the last app clears the mini player */
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    /* now playing: the small pink words, then the song, then who */
    zn.np = box(body);
    lv_obj_set_size(zn.np, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(zn.np, ZN_M, 0);
    lv_obj_set_style_pad_right(zn.np, ZN_M, 0);
    lv_obj_set_style_pad_bottom(zn.np, 10, 0);
    lv_obj_set_flex_flow(zn.np, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_color(zn.np, lv_color_hex(zn.ink), 0);
    lv_obj_set_style_text_color(zn.np, lv_color_hex(ZN_PINK), LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(zn.np, 10, LV_STATE_PRESSED);
    press_motion(zn.np);
    char low[64];
    lv_obj_t *kick = zn_label(zn.np, sl_lower(sl_tr("Now playing"), low, sizeof low), &ss_semibold_18);
    lv_obj_set_style_text_color(kick, lv_color_hex(ZN_PINK), 0);
    zn.np_title = zn_label(zn.np, "", &ss_semibold_26);
    zn.np_artist = zn_label(zn.np, "", &ss_regular_18);
    lv_obj_set_style_text_opa(zn.np_artist, sl_theme() == 1 ? 190 : 160, 0);   /* 4.5:1 on the light page */
    on_click(zn.np, zn_np_cb, NULL);
    lv_obj_remove_flag(zn.np, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(zn.np, LV_OBJ_FLAG_HIDDEN);
    zn_np_update();
    zn_slide_in(zn.np);

    /* the places: the cards home's six, in its order */
    zn_place(body, "Music", ACT_MUSIC);
    zn_place(body, "Playlists", ACT_PLAYLISTS);
    zn_place(body, "Up next", ACT_NOW);
    zn_place(body, "Playroom", ACT_GAMES);
    zn_place(body, "Bluetooth", ACT_BLUETOOTH);
    zn_place(body, "Settings", ACT_SYSTEM);

    zn_history(body);
    zn_heading(body, "Apps");
    zn_app(body, "Cover Flow", "Your albums, flipping by", ACT_COVERFLOW);
    zn_app(body, "Files", "Every folder on the card", ACT_FILES);
    zn_app(body, "Tamagotchi", "A little friend, always growing", ACT_PET);
    zn_app(body, "Ragdoll", "Grab, toss and watch it tumble", ACT_RAGDOLL);
    zn_app(body, "Snake", "Classic walls or level challenge", ACT_SNAKE);
    zn_app(body, "2048", "Slide the tiles, join the twins", ACT_2048);
    zn_app(body, "Sudoku", "Four levels, with notes and hints", ACT_SUDOKU);
    zn_app(body, "24 Points", "Make 24 from four cards", ACT_POINTS);
    zn_app(body, "Flappy Bird", "Tap to flap through the pipes", ACT_FLAPPY);
    zn_app(body, "Sloppy FM", "Tap to tune and grow your station", ACT_RADIO);
    zn_app(body, "Freaky Spoon", "Bounce up with a very strange spoon", ACT_SPOON);
    zn_app(body, "Music Bip Bop Smarty", "Music, movies and TV trivia", ACT_QUIZ);
    zn_app(body, "Flip a Coin", "A quick call for heads or tails", ACT_COIN);
    zn_app(body, "Magic 8 Ball", "Ask a question and tap the ball", ACT_MAGIC);
    zn_app(body, "Chess", "Play the computer or a friend", ACT_CHESS);
    zn_app(body, "Clicker", "Count anything, one tap at a time", ACT_CLICKER);
    zn_app(body, "Arcade", "Game Boy games from your card, and Doom", ACT_ARCADE);
    zn_app(body, "Spectrum", NULL, ACT_SPECTRUM);
    zn_app(body, "Visualizer", NULL, ACT_VISUALIZER);
    zn_app(body, "Spacey Lines", NULL, ACT_SPACEY);

    zn.timer = lv_timer_create(zn_tick, 1000, NULL);
    lv_obj_add_event_cb(p, zn_deleted, LV_EVENT_DELETE, NULL);
    ptr_attach(p, body, zn.ink);
    pages[npages++] = p;
}
