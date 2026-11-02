/* GabagoolOS screensaver (P64, R3). GPLv2+. Included by sloppy.c after
 * Now Playing and Settings.
 *
 * While music plays, the moment the screen would go to sleep it shows the
 * song instead: a floating cover, the cover square across the screen, or
 * a clock. It lies over the lock screen, on black, a little dimmer; a
 * touch or a key puts it away and leaves the lock screen, as waking a dark
 * screen does. When the music stops it waits 15 s, as the lock screen
 * does, and the screen goes dark.
 *
 * Nothing stays put long enough to mark the panel: the floating cover
 * drifts a pixel at a time and turns at the edges, the square cover
 * steps round a small ring every 30 s, the clock moves every minute.
 * Cheap while music plays: the floating cover moves a pixel a second and
 * only its own square is drawn again; the others draw every 30 s, once a
 * minute or on a new song. The floating cover is the Now Playing cover halved once,
 * 115 KB while it shows; nothing is kept when it is away. */

#define SV_FLOAT 240                      /* the floating cover's side */
#define SV_TEXT_GAP SL_GUTTER             /* cover to words */
static struct {
    lv_obj_t *root, *group, *img, *corner[4], *title, *artist, *clock, *date;
    lv_timer_t *timer;
    lv_image_dsc_t dsc;
    uint16_t *px;                         /* the floating cover, halved */
    uint32_t art_sum;                     /* which cover it was made from */
    char path[MAX_PATH];                  /* the song shown */
    int style, x, y, dx, dy, ring, min;
    uint32_t moved_at, stopped_at;
} sv;
static uint32_t sv_steps;                 /* moves made, for the tests */

static int sv_style(void) { int s = global_settings.sloppy_saver; return s >= 1 && s <= 3 ? s : 0; }
static bool sv_shown(void) { return sv.root && !lv_obj_has_flag(sv.root, LV_OBJ_FLAG_HIDDEN); }
static bool sv_has_art(void) { return np_img_buf && np_img.data && np_art_img && !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN); }

/* a cheap look at the cover: a new one, or the full-size one after the thumbnail */
static uint32_t sv_art_sum(void)
{
    if (!sv_has_art()) return 0;
    const uint16_t *p = (const uint16_t *)np_img.data;
    int n = (int)(np_img.header.stride / 2) * (int)np_img.header.h;
    uint32_t s = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < 64; i++) s = (s ^ p[(int)((int64_t)n * i / 64)]) * 16777619u;
    return s;
}
/* the Now Playing cover at side x side in *px (made when missing), each
 * pixel the average of its block, described by *dsc */
static bool sl_cover_scaled(uint16_t **px, lv_image_dsc_t *dsc, int side)
{
    if (!*px) *px = malloc((size_t)side * side * 2);
    if (!*px) return false;
    uint16_t *dst = *px;
    const uint16_t *src = (const uint16_t *)np_img.data;
    int sw = np_img.header.w, sh = np_img.header.h, st = np_img.header.stride / 2;
    for (int y = 0; y < side; y++)
    {
        int y0 = y * sh / side, y1 = LV_MAX(y0 + 1, (y + 1) * sh / side);
        for (int x = 0; x < side; x++)
        {
            int x0 = x * sw / side, x1 = LV_MAX(x0 + 1, (x + 1) * sw / side);
            unsigned r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                {
                    uint16_t p = src[yy * st + xx];
                    r += p >> 11; g += (p >> 5) & 63; b += p & 31; k++;
                }
            dst[y * side + x] = (uint16_t)((r / k) << 11 | (g / k) << 5 | (b / k));
        }
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = dsc->header.h = side;
    dsc->header.stride = side * 2;
    dsc->data = (const uint8_t *)dst;
    dsc->data_size = (uint32_t)side * side * 2;
    lv_image_cache_drop(dsc);
    return true;
}
static bool sv_scale(void) { return sl_cover_scaled(&sv.px, &sv.dsc, SV_FLOAT); }

/* where the group sits: the floating cover where it has drifted to, the
 * square cover on its ring, the clock where this minute put it */
static void sv_place(void)
{
    lv_obj_update_layout(sv.group);
    int gw = lv_obj_get_width(sv.group), gh = lv_obj_get_height(sv.group);
    int x = sv.x, y = sv.y;
    if (sv.style == 2)
    {
        static const int8_t ring[8][2] = { { 0, 0 }, { 6, -4 }, { 8, 2 }, { 4, 7 }, { -3, 8 }, { -8, 3 }, { -7, -4 }, { -2, -8 } };
        x = (W - gw) / 2 + ring[sv.ring & 7][0];
        y = (H - gh) / 2 + ring[sv.ring & 7][1];
    }
    if (lv_obj_get_x(sv.group) != x || lv_obj_get_y(sv.group) != y) lv_obj_set_pos(sv.group, x, y);
}
/* a place for the clock, a new one each minute, never the last */
static void sv_clock_spot(void)
{
    lv_obj_update_layout(sv.group);
    int gw = lv_obj_get_width(sv.group), gh = lv_obj_get_height(sv.group);
    int rx = LV_MAX(1, W - gw - 2 * SL_GUTTER), ry = LV_MAX(1, H - gh - 2 * 60);
    static uint32_t n;
    uint32_t r = ((uint32_t)sv.min + ++n * 40503u) * 2654435761u ^ (uint32_t)sl_ms();
    int x = SL_GUTTER + (int)(r % (uint32_t)rx), y = 60 + (int)((r >> 12) % (uint32_t)ry);
    if (x == sv.x && y == sv.y) y = 60 + (y - 60 + ry / 2 + 1) % ry;   /* never where it was */
    sv.x = x;
    sv.y = y;
}

/* the song's words, and the cover when it changed */
static void sv_refresh(bool force)
{
    struct mp3entry *id3 = audio_current_track();
    if (!sv.root) return;
    if (id3 && (force || strcmp(sv.path, id3->path)))
    {
        char name[64];
        strlcpy(sv.path, id3->path, sizeof sv.path);
        label_if_changed(sv.title, track_title(id3, name, sizeof name));
        label_if_changed(sv.artist, track_artist(id3));
    }
    bool art = sv.style != 3 && sv_has_art();
    uint32_t sum = art ? sv_art_sum() : 0;
    if (force || sum != sv.art_sum)
    {
        sv.art_sum = sum;
        if (art && sv.style == 1 && sv_scale()) lv_image_set_src(sv.img, &sv.dsc);
        else if (art && sv.style == 2) { lv_image_cache_drop(&np_img); lv_image_set_src(sv.img, &np_img); }
        lv_obj_update_flag(sv.img, LV_OBJ_FLAG_HIDDEN, !art);
        for (int k = 0; k < 4; k++) lv_obj_update_flag(sv.corner[k], LV_OBJ_FLAG_HIDDEN, !art || sv.style != 1);
        /* no cover: the clock, so there is always something to see */
        lv_obj_update_flag(sv.clock, LV_OBJ_FLAG_HIDDEN, art);
        lv_obj_update_flag(sv.date, LV_OBJ_FLAG_HIDDEN, art);
    }
    if (!lv_obj_has_flag(sv.clock, LV_OBJ_FLAG_HIDDEN))
    {
        struct tm *tm = get_time();
        char b[40];
        clock_text(b, sizeof b, tm);
        label_if_changed(sv.clock, b);
        static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December" };
        if (tm->tm_wday >= 0 && tm->tm_wday < 7 && tm->tm_mon >= 0 && tm->tm_mon < 12)
            snprintf(b, sizeof b, "%s %d %s", sl_tr(days[tm->tm_wday]), tm->tm_mday, sl_tr(months[tm->tm_mon]));
        else b[0] = '\0';
        label_if_changed(sv.date, sl_case(b));
    }
}

static void sv_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!sv_shown()) return;
    sv_refresh(false);
    uint32_t now = sl_ms();
    struct tm *tm = get_time();
    int minute = tm->tm_hour * 60 + tm->tm_min;
    bool floating = sv.style == 1 && !lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN);
    if (floating)
    {
        /* a pixel each way each step, turning at the screen's edges */
        lv_obj_update_layout(sv.group);
        int gw = lv_obj_get_width(sv.group), gh = lv_obj_get_height(sv.group);
        if (sv.x + sv.dx < SL_GUTTER || sv.x + sv.dx > W - gw - SL_GUTTER) sv.dx = -sv.dx;
        if (sv.y + sv.dy < 40 || sv.y + sv.dy > H - gh - 40) sv.dy = -sv.dy;
        sv.x += sv.dx;
        sv.y += sv.dy;
        sv_steps++;
    }
    else if (sv.style == 2 && !lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN))
    {
        if (now - sv.moved_at >= 30000) { sv.moved_at = now; sv.ring++; sv_steps++; }
    }
    else if (minute != sv.min)
    {
        sv.min = minute;
        sv_clock_spot();
        sv_steps++;
    }
    sv_place();
}

static void sv_build(void)
{
    sv.root = box(lv_layer_top());
    lv_obj_set_size(sv.root, W, H);
    lv_obj_set_style_bg_color(sv.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sv.root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(sv.root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    sv.group = box(sv.root);
    lv_obj_set_size(sv.group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sv.group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sv.group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    sv.clock = text_light(sv.group, "", &ss_semibold_104);
    lv_obj_set_style_text_letter_space(sv.clock, -2, 0);
    lv_obj_set_style_text_opa(sv.clock, LV_OPA_90, 0);
    sv.date = text_light(sv.group, "", &ss_semibold_21);
    lv_obj_set_style_text_opa(sv.date, LV_OPA_80, 0);
    lv_obj_set_style_pad_bottom(sv.date, SL_INSET, 0);
    sv.img = lv_image_create(sv.group);
    lv_obj_set_style_margin_bottom(sv.img, SV_TEXT_GAP, 0);
    for (int k = 0; k < 4; k++)
    {
        sv.corner[k] = lv_image_create(sv.img);
        lv_image_set_src(sv.corner[k], sl_corner_dsc(k));
        lv_obj_add_flag(sv.corner[k], LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(sv.corner[k], k == 0 ? LV_ALIGN_TOP_LEFT : k == 1 ? LV_ALIGN_TOP_RIGHT :
                                   k == 2 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
    sv.title = text_light(sv.group, "", &ss_semibold_26);
    sv.artist = text_light(sv.group, "", &ss_regular_21);
    lv_obj_set_style_text_opa(sv.artist, LV_OPA_70, 0);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = i ? sv.artist : sv.title;
        lv_obj_set_width(l, W - 2 * SL_INSET - 2 * SL_GUTTER);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(l, 1);
    }
    lv_obj_add_flag(sv.root, LV_OBJ_FLAG_HIDDEN);
    sv.timer = lv_timer_create(sv_timer_cb, 1000, NULL);
    lv_timer_pause(sv.timer);
}

static void sv_show(void)
{
    if (!sv.root) sv_build();
    sv.style = sv_style();
    /* the words as wide as what is over them */
    int ww = sv.style == 1 ? SV_FLOAT + 2 * SL_GUTTER : sv.style == 2 ? W - 2 * SL_INSET : 320;
    lv_obj_set_width(sv.title, ww);
    lv_obj_set_width(sv.artist, ww);
    sv.path[0] = '\0';
    sv_refresh(true);
    sv.dx = 1; sv.dy = 1; sv.ring = 0; sv.moved_at = sl_ms();
    struct tm *tm = get_time();
    sv.min = tm->tm_hour * 60 + tm->tm_min;
    lv_obj_update_layout(sv.group);
    sv.x = (W - lv_obj_get_width(sv.group)) / 2;
    sv.y = (H - lv_obj_get_height(sv.group)) / 2;
    if (sv.style == 3 || lv_obj_has_flag(sv.img, LV_OBJ_FLAG_HIDDEN)) sv_clock_spot();
    sv_place();
    lv_obj_remove_flag(sv.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sv.root);
    /* once a second: a pixel of drift, a new song, the minute */
    lv_timer_set_period(sv.timer, 1000);
    lv_timer_resume(sv.timer);
    sv.stopped_at = 0;
    sl_log("screensaver: %s", sv.style == 1 ? "floating cover" : sv.style == 2 ? "square cover" : "clock");
}
static void sv_hide(void)
{
    if (!sv_shown()) return;
    lv_obj_add_flag(sv.root, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(sv.timer);
    if (sv.px) { lv_image_set_src(sv.img, NULL); lv_image_cache_drop(&sv.dsc); free(sv.px); sv.px = NULL; }
    sv.art_sum = 0;
}

/* From the power tick, before it decides to sleep: the moment the screen
 * would go dark with music playing, the screensaver instead. */
static void sv_tick(void)
{
    uint32_t now = sl_ms();
    if (sv_shown())
    {
        /* a touch, a key or the power key ended it */
        if (!sl_power.saver || sl_power.mode != SL_POWER_LOCKED) { sl_power.saver = false; sv_hide(); return; }
        if (sv_style() != sv.style) { sv_hide(); sl_power.saver = false; return; }
        /* the music stopped: the lock screen's 15 s, then dark */
        if (!is_playing())
        {
            if (!sv.stopped_at) sv.stopped_at = now ? now : 1;
            else if (now - sv.stopped_at >= SL_POWER_WAKE_MS) { sl_power.saver = false; sl_power.activity_at = now - SL_POWER_WAKE_MS; }
        }
        else sv.stopped_at = 0;
        return;
    }
    if (!sv_style() || !is_playing() || sl_power.mode > SL_POWER_DIM || sl_power.touch_down || sl_power.power_down || po_is_open())
        return;
    uint32_t sleep = sl_power.sleep_ms ? sl_power.sleep_ms : SL_POWER_LOCK_MS;
    if (now - sl_power.activity_at + 100 < sleep) return;
    sl_power_state_set(&sl_power, SL_POWER_LOCKED, now);
    sl_power.saver = true;
    sl_power_apply();                    /* the lock screen under it, a little dimmer */
    sv_show();
}
/* a touch on it: as on a dark screen, it wakes the lock screen and goes */
static void sv_touch_down(void)
{
    sl_power.saver = false;
    sl_power.mode = SL_POWER_DARK;        /* sl_power_state_touch wakes it to the lock screen and keeps the contact */
    sv_hide();
}

/* ------------------------------------------------------------ Settings */
static const char *const sv_names[] = { "Off", "Floating cover", "Square cover", "Clock" };
static const char *set_v_saver(void) { return sv_names[sv_style()]; }
static void sv_sketch(lv_obj_t *pic, int style)
{
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, 0x000000);
    lv_color_t accent = lv_color_hex(C_ACCENT);
    if (style == 1 || style == 2)
    {
        int side = style == 1 ? 34 : SET_PIC_W - 2;
        lv_obj_t *art = box(s);
        lv_obj_set_size(art, side, side);
        lv_obj_set_pos(art, style == 1 ? 12 : 0, style == 1 ? 22 : 18);
        lv_obj_set_style_radius(art, style == 1 ? SL_R_XS : 0, 0);
        lv_obj_set_style_bg_color(art, lv_color_mix(lv_color_white(), accent, 70), 0);
        lv_obj_set_style_bg_grad_color(art, accent, 0);
        lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
        for (int i = 0; i < 2; i++)
        {
            lv_obj_t *l = box(s);
            int w = style == 1 ? (i ? 18 : 28) : (i ? 26 : 40);
            lv_obj_set_size(l, w, 3);
            lv_obj_set_pos(l, style == 1 ? 12 + (34 - w) / 2 : (SET_PIC_W - w) / 2, (style == 1 ? 62 : 82) + i * 6);
            lv_obj_set_style_radius(l, SL_R_FULL, 0);
            lv_obj_set_style_bg_color(l, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(l, i ? LV_OPA_50 : LV_OPA_80, 0);
        }
    }
    else if (style == 3)
    {
        lv_obj_t *t = text_light(s, "9:41", &ss_semibold_21);
        lv_obj_align(t, LV_ALIGN_CENTER, 4, -8);
        lv_obj_t *l = box(s);
        lv_obj_set_size(l, 30, 3);
        lv_obj_align(l, LV_ALIGN_CENTER, 4, 12);
        lv_obj_set_style_radius(l, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(l, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
    }
}
static void sv_pick_cb(lv_event_t *e)
{
    global_settings.sloppy_saver = (int)(intptr_t)lv_event_get_user_data(e);
    set_check_mark(lv_event_get_current_target(e));
    if (settings_save() != 0) toast_warn("Could not save settings");
}
static void saver_page(void)
{
    static const char *const notes[] = { "The screen goes dark as usual",
                                         "Your cover drifts slowly round the screen",
                                         "Your cover across the screen, the song under it",
                                         "The time, the date and the song" };
    lv_obj_t *p, *body = page_new("Screensaver", &p);
    lv_obj_t *g = set_group(body, "Style");
    for (int i = 0; i < 4; i++)
        sv_sketch(lv_obj_get_child(set_pick(g, sv_names[i], notes[i], i == sv_style(), sv_pick_cb, (void *)(intptr_t)i), 2), i);
    opt_note(body, "While music plays, it shows when the screen would go to sleep, a little dimmer, and keeps moving so nothing marks the screen. A touch or a key brings back the lock screen.");
    page_push(p);
}
