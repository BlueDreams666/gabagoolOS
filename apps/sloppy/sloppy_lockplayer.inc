/* GabagoolOS lock screen Now Playing card. GPLv2+.
 * Included by sloppy.c after the Now Playing sheet and the mini player.
 * The lock screen owns every touch while locked (sloppy_power.inc), so LVGL
 * never sees them: the power filter asks lp_hit() which control a contact
 * started on, feeds a slider its drag through lp_drag(), and calls
 * lp_activate() when the finger lifts (a button only if it did not move). */

static void statusbar_update(void);
static int volume_pct(void);

enum { LP_BIG, LP_SMALL };
#define LP_ART_BIG   LCD_WIDTH  /* edge to edge in the middle, like a wallpaper */
#define LP_ART_SMALL  96
#define LP_CARD_W    (LCD_WIDTH - 40)

static lv_obj_t *lp_card, *lp_art, *lp_art_img, *lp_art_note, *lp_title, *lp_artist;
static lv_obj_t *lp_track, *lp_fill, *lp_t0, *lp_t1, *lp_btn[3];
static lv_obj_t *lp_vol_track, *lp_vol_fill, *lp_vol_icon[2];
static lv_obj_t *lp_date, *lp_clock;    /* iPhone-style time over the wallpaper */
static int lp_held = -1;                /* the control under the finger */
static long lp_scrub_ms = -1;           /* a song position being dragged to */
static int lp_style = -1;                /* the layout lp_card was built for */
static bool lp_art_ready;
static bool lp_image_wallpaper;         /* the lock wallpaper lp_theme() last saw */
static char lp_last_path[MAX_PATH];
static uint16_t *lp_art_buf;             /* LP_ART_BIG squared, allocated on first use */
static lv_image_dsc_t lp_art_dsc;

static int lp_layout(void)
{
    return global_settings.sloppy_lock_player == LP_SMALL ? LP_SMALL : LP_BIG;
}

static int lp_art_size(void) { return lp_style == LP_SMALL ? LP_ART_SMALL : LP_ART_BIG; }

/* Box-filter the Now Playing art (a centered square of it) to the card. */
static void lp_art_scale(const lv_image_dsc_t *src)
{
    int d = lp_art_size();
    int sw = src->header.w, sh = src->header.h, stride = src->header.stride / 2;
    int s = sw < sh ? sw : sh, ox = (sw - s) / 2, oy = (sh - s) / 2;
    const uint16_t *px = (const uint16_t *)src->data;
    if (!lp_art_buf) lp_art_buf = malloc(LP_ART_BIG * LP_ART_BIG * 2);
    if (!lp_art_buf) return;
    for (int y = 0; y < d; y++)
    {
        int y0 = oy + y * s / d, y1 = oy + (y + 1) * s / d;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < d; x++)
        {
            int x0 = ox + x * s / d, x1 = ox + (x + 1) * s / d;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, n++)
                {
                    uint16_t c = px[sy * stride + sx];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31;
                }
            lp_art_buf[y * d + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
    lp_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lp_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    lp_art_dsc.header.w = lp_art_dsc.header.h = d;
    lp_art_dsc.header.stride = d * 2;
    lp_art_dsc.data = (const uint8_t *)lp_art_buf;
    lp_art_dsc.data_size = (uint32_t)d * d * 2;
    lv_image_cache_drop(&lp_art_dsc);
    lv_image_set_src(lp_art_img, &lp_art_dsc);
}

static void lp_set_art(struct mp3entry *id3)
{
    np_set_art(id3);                    /* loads the playing track's art into np_img */
    lp_art_ready = !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) &&
                   (lp_art_buf || (lp_art_buf = malloc(LP_ART_BIG * LP_ART_BIG * 2)));
    if (lp_art_ready)
        lp_art_scale(&np_img);
    hidden_if_changed(lp_art_img, !lp_art_ready);
    hidden_if_changed(lp_art_note, lp_art_ready);
    bg_if_changed(lp_art, lv_obj_get_style_bg_color(np_art, 0));
}

static lv_obj_t *lp_button(int k, const char *symbol, int size, const lv_font_t *font)
{
    lv_obj_t *b = box(lp_card);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_center(text(b, symbol, font, 0xffffff));
    lp_btn[k] = b;
    return b;
}

/* A thin track with an accent fill, like the Now Playing sheet's. */
static lv_obj_t *lp_bar(int x, int y, int w, lv_obj_t **fill)
{
    lv_obj_t *t = box(lp_card);
    lv_obj_set_size(t, w, 6);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_20, 0);
    *fill = box(t);
    lv_obj_set_size(*fill, 0, 6);
    lv_obj_set_style_radius(*fill, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
    return t;
}

/* True while the big layout's art fills the middle of the lock screen. */
static bool lp_art_up(void)
{
    return lp_style == LP_BIG && lp_card && !lv_obj_has_flag(lp_card, LV_OBJ_FLAG_HIDDEN);
}

static void lp_build(lv_obj_t *overlay)
{
    if (lp_art && lv_obj_get_parent(lp_art) == overlay) lv_obj_delete(lp_art);
    lp_art = NULL;
    if (lp_card) lv_obj_delete(lp_card);
    lp_style = lp_layout();
    lp_last_path[0] = 0;
    bool big = lp_style == LP_BIG;
    int art = lp_art_size(), inner = LP_CARD_W - 40;

    lp_card = box(overlay);
    lv_obj_set_size(lp_card, LP_CARD_W, big ? 256 : 312);
    lv_obj_align(lp_card, LV_ALIGN_BOTTOM_MID, 0, -60);   /* above the unlock bar */
    lv_obj_set_style_radius(lp_card, SL_R_XL, 0);
    lv_obj_set_style_bg_opa(lp_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lp_card, 20, 0);
    lv_obj_add_flag(lp_card, LV_OBJ_FLAG_HIDDEN);

    /* Big: the art spans the screen's middle behind the clock and the card,
     * just above the wallpaper (child 0 is the unlock bar). Small: in the card. */
    lp_art = box(big ? overlay : lp_card);
    lv_obj_set_size(lp_art, art, art);
    lv_obj_set_style_bg_opa(lp_art, LV_OPA_COVER, 0);
    if (big)
    {
        lv_obj_center(lp_art);
        lv_obj_move_to_index(lp_art, 1);
        lv_obj_add_flag(lp_art, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_set_style_radius(lp_art, SL_R_MD, 0);
        lv_obj_set_style_clip_corner(lp_art, true, 0);
        lv_obj_align(lp_art, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    lp_art_img = lv_image_create(lp_art);
    lv_obj_center(lp_art_img);
    lv_obj_add_flag(lp_art_img, LV_OBJ_FLAG_HIDDEN);
    lp_art_note = text(lp_art, LV_SYMBOL_AUDIO, big ? &sl_sym_48 : &sl_sym_32,
                       0xffffff);
    lv_obj_set_style_text_opa(lp_art_note, LV_OPA_60, 0);
    lv_obj_center(lp_art_note);
    int tx = big ? 0 : art + 16, tw = big ? inner : inner - art - 16;
    lp_title = text(lp_card, "", &ss_semibold_23, 0xffffff);
    lp_artist = text(lp_card, "", &ss_regular_18, 0xffffff);
    lv_obj_set_width(lp_title, tw);
    lv_obj_set_width(lp_artist, tw);
    label_lines(lp_title, 1);
    label_lines(lp_artist, 1);
    if (big)
    {
        lv_obj_set_style_text_align(lp_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(lp_artist, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_set_pos(lp_title, tx, big ? 0 : 14);
    lv_obj_set_pos(lp_artist, tx, big ? 32 : 50);

    int bar_y = big ? 70 : art + 20;
    lp_track = lp_bar(0, bar_y, inner, &lp_fill);
    lp_t0 = text(lp_card, "", &ss_regular_16, 0xffffff);
    lp_t1 = text(lp_card, "", &ss_regular_16, 0xffffff);
    lv_obj_set_pos(lp_t0, 0, bar_y + 12);
    lv_obj_align(lp_t1, LV_ALIGN_TOP_RIGHT, 0, bar_y + 12);

    int row = bar_y + 36, gap = big ? 116 : 104;
    lv_obj_align(lp_button(0, LV_SYMBOL_PREV, 64, &sl_sym_28),
                 LV_ALIGN_TOP_MID, -gap, row + 4);
    lv_obj_align(lp_button(1, LV_SYMBOL_PLAY, 72, &sl_sym_40),
                 LV_ALIGN_TOP_MID, 0, row);
    lv_obj_align(lp_button(2, LV_SYMBOL_NEXT, 64, &sl_sym_28),
                 LV_ALIGN_TOP_MID, gap, row + 4);

    int vol_y = row + 100;              /* the centre line of the volume row */
    lp_vol_icon[0] = text(lp_card, LV_SYMBOL_VOLUME_MID, &sl_sym_20, 0xffffff);
    lv_obj_align(lp_vol_icon[0], LV_ALIGN_TOP_LEFT, 0, vol_y - 10);
    lp_vol_icon[1] = text(lp_card, LV_SYMBOL_VOLUME_MAX, &sl_sym_20, 0xffffff);
    lv_obj_align(lp_vol_icon[1], LV_ALIGN_TOP_RIGHT, 0, vol_y - 10);
    lp_vol_track = lp_bar(40, vol_y - 3, inner - 80, &lp_vol_fill);
}

/* The date, then the time in large figures, centered at the top. */
static void lp_clock_build(lv_obj_t *overlay)
{
    lp_date = text(overlay, "", &ss_semibold_21, 0xffffff);
    lv_obj_align(lp_date, LV_ALIGN_TOP_MID, 0, 62);
    lp_clock = text(overlay, "", &ss_semibold_104, 0xffffff);
    lv_obj_set_style_text_letter_space(lp_clock, -2, 0);
    lv_obj_align(lp_clock, LV_ALIGN_TOP_MID, 0, 90);
}

static void lp_clock_update(void)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May",
        "June", "July", "August", "September", "October", "November", "December" };
    struct tm *tm = get_time();
    char b[40];
    int hour = tm->tm_hour;
    if (global_settings.timeformat)
    {
        hour %= 12;
        snprintf(b, sizeof b, "%d:%02d", hour ? hour : 12, tm->tm_min);
    }
    else
        snprintf(b, sizeof b, "%02d:%02d", hour, tm->tm_min);
    label_if_changed(lp_clock, b);
    if (tm->tm_wday >= 0 && tm->tm_wday < 7 && tm->tm_mon >= 0 && tm->tm_mon < 12)
        snprintf(b, sizeof b, "%s %d %s", sl_tr(days[tm->tm_wday]), tm->tm_mday, sl_tr(months[tm->tm_mon]));
    else
        b[0] = 0;
    label_if_changed(lp_date, sl_case(b));
    /* the figures' tops a little under the date, whatever size the date's
     * type is: the big font's line starts well above its digits */
    const lv_font_t *df = lv_obj_get_style_text_font(lp_date, 0), *cf = lv_obj_get_style_text_font(lp_clock, 0);
    lv_font_glyph_dsc_t g;
    int top = 0;
    if (lv_font_get_glyph_dsc(cf, &g, '0', 0))
        top = (lv_font_get_line_height(cf) - cf->base_line) - (g.box_h + g.ofs_y);
    int y = 62 + lv_font_get_line_height(df) + 8 - top;
    if (lv_obj_get_y_aligned(lp_clock) != y) lv_obj_align(lp_clock, LV_ALIGN_TOP_MID, 0, y);
}

/* Colors follow the lock wallpaper: glass over pictures, a card otherwise. */
static void lp_theme(bool image_wallpaper)
{
    lp_image_wallpaper = image_wallpaper;
    bool photo = image_wallpaper;         /* behind the clock: the big art starts below it */
    image_wallpaper |= lp_art_up();     /* the big art is a picture behind the card */
    if (lp_clock)
    {
        /* Zune: the time in the lighter face, and the card square */
        lv_obj_set_style_text_font(lp_clock, sl_zune() ? &ss_regular_104 : &ss_semibold_104, 0);
        uint32_t ink = photo ? 0xffffff : C_TEXT;   /* a white clock vanished on the light theme */
        lv_obj_set_style_text_color(lp_clock, lv_color_hex(ink), 0);
        lv_obj_set_style_text_color(lp_date, lv_color_hex(ink), 0);
        lv_obj_set_style_text_opa(lp_clock, LV_OPA_90, 0);
        lv_obj_set_style_text_opa(lp_date, LV_OPA_80, 0);
    }
    if (!lp_card) return;
    lv_obj_set_style_radius(lp_card, sl_zune() ? 0 : SL_R_XL, 0);
    lv_obj_set_style_bg_color(lp_card, image_wallpaper ? lv_color_black() : lv_color_hex(C_CARD), 0);
    /* glass over a photo; over the art alone it is denser, as it also
     * crosses the page under the art (grey on the light theme) */
    lv_obj_set_style_bg_opa(lp_card, photo ? LV_OPA_50 : image_wallpaper ? LV_OPA_70 : LV_OPA_COVER, 0);
    uint32_t ink = image_wallpaper ? 0xffffff : C_TEXT;
    uint32_t sub = image_wallpaper ? 0xc7d8df : C_SUB;
    lv_obj_set_style_text_color(lp_title, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(lp_artist, lv_color_hex(sub), 0);
    lv_obj_set_style_text_color(lp_t0, lv_color_hex(sub), 0);
    lv_obj_set_style_text_color(lp_t1, lv_color_hex(sub), 0);
    lv_obj_set_style_bg_color(lp_track, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(lp_fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_color(lp_vol_track, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(lp_vol_fill, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(lp_vol_icon[0], lv_color_hex(sub), 0);
    lv_obj_set_style_text_color(lp_vol_icon[1], lv_color_hex(sub), 0);
    for (int k = 0; k < 3; k++)
    {
        lv_obj_set_style_text_color(lv_obj_get_child(lp_btn[k], 0), lv_color_hex(ink), 0);
        lv_obj_set_style_bg_color(lp_btn[k], lv_color_hex(ink), LV_STATE_PRESSED);
    }
}

static void lp_bar_set(lv_obj_t *track, lv_obj_t *fill, unsigned long v, unsigned long max)
{
    int w = lv_obj_get_style_width(track, 0);
    int32_t x = max ? (int32_t)((uint64_t)w * (v < max ? v : max) / max) : 0;
    if (lv_obj_get_style_width(fill, 0) != x)
        lv_obj_set_width(fill, x);
}

static void lp_show_time(const struct mp3entry *id3, unsigned long elapsed)
{
    lp_bar_set(lp_track, lp_fill, elapsed, id3->length);
    char b[20];
    fmt_time(b, sizeof b, elapsed);
    label_if_changed(lp_t0, b);
    b[0] = '-';
    fmt_time(b + 1, sizeof b - 1, id3->length > elapsed ? (long)(id3->length - elapsed) : 0);
    label_if_changed(lp_t1, b);
}

/* The lock screen is up with the player card on it: its volume bar says
 * the level, so the side volume bar stays away. */
static bool lp_volume_shown(void)
{
    return sl_power_overlay && !lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN) &&
           !wallpaper_screen_dark && lp_card && !lv_obj_has_flag(lp_card, LV_OBJ_FLAG_HIDDEN);
}

/* Show the card while a track is loaded (playing or paused) and keep it current. */
static void lp_refresh(void)
{
    if (!sl_power_overlay || lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN) ||
        wallpaper_screen_dark)
        return;
    if (!lp_clock) lp_clock_build(sl_power_overlay);
    if (!lp_card || lp_style != lp_layout())
    {
        lp_build(sl_power_overlay);
        lp_theme(sl_power_image_wallpaper());
    }
    lp_clock_update();
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    bool show = id3 != NULL;
    if (show == lv_obj_has_flag(lp_card, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_update_flag(lp_card, LV_OBJ_FLAG_HIDDEN, !show);
        if (lp_style == LP_BIG)
            lv_obj_update_flag(lp_art, LV_OBJ_FLAG_HIDDEN, !show);
        lp_theme(lp_image_wallpaper);
    }
    if (!show) return;

    static int lp_last_tags = -1;        /* the file's own tags came after the song started */
    int tags = (id3->title && id3->title[0]) | (id3->artist && id3->artist[0]) << 1;
    if (strcmp(lp_last_path, id3->path) || tags != lp_last_tags)
    {
        lp_last_tags = tags;
        strlcpy(lp_last_path, id3->path, sizeof lp_last_path);
        char name[64];
        label_if_changed(lp_title, track_title(id3, name, sizeof name));
        label_if_changed(lp_artist, track_artist(id3));
        lp_set_art(id3);
    }
    else if (!lp_art_ready || np_art_stale())
        lp_set_art(id3);                /* art may arrive after the track starts, or the song before's came first */

    if (lp_scrub_ms < 0)
        lp_show_time(id3, visible_elapsed(id3));
    if (lp_held != LP_VOLUME)
        lp_bar_set(lp_vol_track, lp_vol_fill, volume_pct(), 100);
    label_if_changed(lv_obj_get_child(lp_btn[1], 0), is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static bool lp_over(lv_obj_t *o, int x, int y, int grow_x, int grow_y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return x >= a.x1 - grow_x && x <= a.x2 + grow_x && y >= a.y1 - grow_y && y <= a.y2 + grow_y;
}

/* Which LP_ control is under a point, or -1. The thin sliders get a
 * finger-sized band around them; the song's title and artist are one. */
static int lp_hit(int x, int y)
{
    if (!lp_card || lv_obj_has_flag(lp_card, LV_OBJ_FLAG_HIDDEN)) return -1;
    for (int k = LP_PREV; k <= LP_NEXT; k++)
        if (lp_over(lp_btn[k], x, y, 12, 12)) return k;
    if (lp_over(lp_title, x, y, 0, 8) || lp_over(lp_artist, x, y, 0, 8)) return LP_SONG;
    struct mp3entry *id3 = audio_current_track();
    if (id3 && id3->length && lp_over(lp_track, x, y, 12, 22)) return LP_SEEK;
    if (lp_over(lp_vol_track, x, y, 40, 22)) return LP_VOLUME;
    return -1;
}

static void lp_press(int k, bool on)
{
    if (k < 0 || !lp_card) return;
    lp_held = on ? k : -1;
    if (k <= LP_NEXT)
    {
        if (on) lv_obj_add_state(lp_btn[k], LV_STATE_PRESSED);
        else lv_obj_remove_state(lp_btn[k], LV_STATE_PRESSED);
    }
    else if (k == LP_SONG)              /* the song's words dim under the finger */
    {
        lv_obj_set_style_opa(lp_title, on ? LV_OPA_60 : LV_OPA_COVER, 0);
        lv_obj_set_style_opa(lp_artist, on ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
    else                                /* a held slider thickens */
    {
        lv_obj_t *track = k == LP_SEEK ? lp_track : lp_vol_track;
        lv_obj_set_height(track, on ? 10 : 6);
        lv_obj_set_height(lv_obj_get_child(track, 0), on ? 10 : 6);
        lv_obj_set_y(track, lv_obj_get_style_y(track, 0) + (on ? -2 : 2));
    }
}

/* A slider let go of without being used: put back what it previewed. */
static void lp_cancel(int k, int volume)
{
    lp_press(k, false);
    lp_scrub_ms = -1;
    if (k == LP_VOLUME && global_status.volume != volume)
    {
        global_status.volume = volume;
        setvol();
        statusbar_update();
    }
    lp_refresh();
}

/* The finger at x along a slider: scrub the song, or set the volume live. */
static void lp_drag(int k, int x)
{
    lv_obj_t *track = k == LP_SEEK ? lp_track : lp_vol_track;
    lv_area_t a;
    lv_obj_get_coords(track, &a);
    int w = lv_area_get_width(&a), at = x - a.x1;
    at = at < 0 ? 0 : at > w ? w : at;
    if (k == LP_SEEK)
    {
        struct mp3entry *id3 = audio_current_track();
        if (!id3 || !id3->length) return;
        lp_scrub_ms = (long)((uint64_t)id3->length * at / w);
        lp_show_time(id3, lp_scrub_ms);
    }
    else
    {
        int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME);
        int v = volume_snap(mn + (mx - mn) * at / w);
        if (v != global_status.volume)
        {
            global_status.volume = v;
            setvol();
            statusbar_update();
        }
        lp_bar_set(lp_vol_track, lp_vol_fill, at, w);
    }
}

static void lp_activate(int k)
{
    if (k == LP_SEEK)
    {
        struct mp3entry *id3 = audio_current_track();
        if (id3 && lp_scrub_ms >= 0)
        {
            if (!(audio_status() & AUDIO_STATUS_PAUSE))
                audio_pre_ff_rewind();
            audio_ff_rewind(lp_scrub_ms);
        }
        lp_scrub_ms = -1;
        return;
    }
    if (k == LP_VOLUME) return;         /* already applied while dragging */
    if (k == LP_SONG)
    {
        /* the song tapped: unlocked, straight into Now Playing, as an iPhone does */
        sl_power_state_set(&sl_power, SL_POWER_ACTIVE, sl_ms());
        sl_power_apply();
        if (!np_open) np_show();
        return;
    }
    if (k == LP_PREV) sl_prev();
    else if (k == LP_PLAY) toggle_play();
    else if (k == LP_NEXT) sl_next();
    else return;
    sfx_play(SFX_TAP);
    lp_refresh();
}
