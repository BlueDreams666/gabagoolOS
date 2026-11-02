/*
 *  Now Playing: swipe the cover for the next or the previous song, as on
 *  Spotify. The cover follows the finger with the neighbour's beside it,
 *  the next song on the right and the previous on the left; let go past a
 *  third of the way, or flick, and that song plays at once while its cover
 *  slides the rest of the way in. Short of that it springs back. The next
 *  and previous buttons play the same slide.
 *
 *  The neighbour is the song Rockbox will play (get_next_index): the
 *  playlist's own order, which shuffle has already mixed; a manual skip
 *  under repeat one goes on as repeat all; with repeat off the list has
 *  ends, and there the cover only gives a little. Its picture is the
 *  album's thumbnail, the one Now Playing shows until the full-size cover
 *  arrives; one buffer holds it (the next song's, made ready once a song
 *  has settled), handed to the cover when it lands rather than copied.
 */

#define NPS_LOCK 12                      /* px the finger moves before it is a swipe or a pull */

static struct
{
    lv_obj_t *peek, *peek_img, *peek_none;
    lv_image_dsc_t dsc;
    uint16_t *buf;
    char path[MAX_PATH];                 /* the song the peek shows */
    int album, side;                     /* side: 1 next (right), -1 previous (left), 0 none */
    bool art;
    int axis;                            /* this touch: 0 not yet, 1 the cover, 2 the sheet */
    int x0, y0, dx, last_x, vx;
    uint32_t last_at;
    bool sliding, landing;               /* landing: the slide ends on the new song */
    char skipped_from[MAX_PATH];         /* a skip on its way: the neighbours are unknown */
    char expect[MAX_PATH];               /* the song its slide showed, checked when it plays */
    uint32_t skipped_at;
    lv_timer_t *preload;
    /* the neighbour's lake, so the reflection travels with its cover */
    lv_obj_t *glow;
    uint8_t *glow_buf;
    lv_image_dsc_t glow_dsc;
    bool glow_ready;
} nps = { .album = -1 };

/* The neighbour's reflection, made when its cover is: under the glass, in
 * a buffer of its own, waiting beside the cover's own lake. */
static void nps_glow_make(void)
{
    nps.glow_ready = false;
    if (!np_glass() || !np_glow || !np_glow_buf) return;   /* no glass, or no room for even one */
    if (sl_bt_streaming() && !nps.glow_buf) return;          /* the Bluetooth encoder's memory first */
    if (!nps.glow_buf)
    {
        long kb = sl_mem_available_kb();     /* the same bar as the cover's own glass */
        if ((kb >= 0 && kb < NG_MIN_FREE_KB) || !(nps.glow_buf = malloc((size_t)W * NG_ROWS * 3)))
        {
            sl_log("np: no room for the neighbour's reflection (%ld KiB free)", kb);
            return;
        }
    }
    if (!nps.glow)
    {
        nps.glow = lv_image_create(lv_obj_get_parent(np_glow));
        lv_obj_set_pos(nps.glow, 0, NG_TOP);
        lv_obj_add_flag(nps.glow, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_move_to_index(nps.glow, lv_obj_get_index(np_glow) + 1);
    }
    if (nps.art) ng_paint(nps.glow_buf, nps.buf, W, W, W, lv_color_black());
    else ng_paint(nps.glow_buf, NULL, 0, 0, 0, lv_obj_get_style_bg_color(nps.peek, 0));
    ng_show(nps.glow, &nps.glow_dsc, nps.glow_buf);
    nps.glow_ready = true;
}

/* The playlist index a skip by step plays, or -1 at an end. */
static int np_neighbour(int step)
{
    int n = playlist_amount(), cur = q_cur();
    if (n <= 0 || cur < 0) return -1;
    int mode = global_settings.repeat_mode;
    if (mode != REPEAT_OFF && !(mode == REPEAT_SHUFFLE && n > 1))
        return ((cur + step) % n + n) % n;   /* round: all, one (a skip leaves it), a one-song shuffle */
    int r = q_rot(cur) + step;
    return r < 0 || r >= n ? -1 : (r + playlist_get_first_index(NULL)) % n;
}

/* Until the skip just made has changed the song, the neighbours are the
 * old song's: no peek, and the buttons skip without a slide. */
static bool nps_pending(void)
{
    if (!nps.skipped_from[0]) return false;
    struct mp3entry *id3 = audio_current_track();
    if ((id3 && strcmp(id3->path, nps.skipped_from)) || lv_tick_elaps(nps.skipped_at) > 1500)
    {
        if (id3 && nps.expect[0] && strcmp(id3->path, nps.expect))
            sl_log("np: the slide showed %s, but %s plays", nps.expect, id3->path);
        nps.skipped_from[0] = nps.expect[0] = '\0';
    }
    return nps.skipped_from[0] != '\0';
}

static lv_color_t nps_hue(const char *s)
{
    unsigned hue = 0;
    for (; *s; s++) hue = hue * 31 + (unsigned char)*s;
    return lv_color_hsv_to_rgb(hue % 360, 55, 45);
}

/* Put playlist index idx in the peek: its album's picture, or the colour
 * Now Playing gives a song without one. False if the song can't be read. */
static bool nps_load(int idx)
{
    struct playlist_track_info info;
    if (idx < 0 || playlist_get_track_info(NULL, idx, &info) < 0) return false;
    if (!strcmp(info.filename, nps.path)) return true;   /* ready already */
    strlcpy(nps.path, info.filename, sizeof nps.path);
    int t = sl_lib_find_path(info.filename);
    nps.album = t >= 0 ? sl_lib.tracks[t].album : -1;
    if (!nps.buf) nps.buf = malloc((size_t)W * W * 2);
    nps.art = nps.buf && nps.album >= 0 && sl_art_load_square(nps.album, W, nps.buf);
    if (nps.art)
    {
        nps.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        nps.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        nps.dsc.header.w = nps.dsc.header.h = W;
        nps.dsc.header.stride = W * 2;
        nps.dsc.data = (const uint8_t *)nps.buf;
        nps.dsc.data_size = (uint32_t)W * W * 2;
        lv_image_cache_drop(&nps.dsc);
        lv_image_set_src(nps.peek_img, &nps.dsc);
        lv_obj_set_style_bg_color(nps.peek, lv_color_black(), 0);
    }
    else
    {
        lv_color_t bg = nps_hue(nps.album >= 0 ? sl_lib.albums[nps.album].title : info.filename);
        lv_obj_set_style_bg_color(nps.peek, bg, 0);
        lv_obj_set_style_bg_color(nps.peek_none, bg, 0);
    }
    hidden_if_changed(nps.peek_img, !nps.art);
    hidden_if_changed(nps.peek_none, nps.art);
    nps_glow_make();
    return true;
}

/* The cover dx along, and the peek beside it on its side */
static void nps_set_x(void *o, int32_t dx)
{
    (void)o;
    nps.dx = dx;
    lv_obj_set_style_translate_x(np_art, dx, 0);
    hidden_if_changed(nps.peek, !nps.side || !dx);
    if (nps.side) lv_obj_set_style_translate_x(nps.peek, dx + nps.side * W, 0);
    /* each cover's reflection under it, moving with it: live, not after */
    if (nps.glow_ready && np_glow)
    {
        lv_obj_set_style_translate_x(np_glow, dx, 0);
        hidden_if_changed(nps.glow, !nps.side || !dx);
        if (nps.side) lv_obj_set_style_translate_x(nps.glow, dx + nps.side * W, 0);
    }
    else if (np_glow && lv_obj_get_style_translate_x(np_glow, 0)) lv_obj_set_style_translate_x(np_glow, 0, 0);
}

static void np_glow_soon(void);
/* The slide is over: the peek's picture becomes the cover's (the buffers
 * change places), and the cover is back where it was. */
static void nps_land(lv_anim_t *a)
{
    (void)a;
    nps.sliding = nps.landing = false;
    if (nps.side && nps.art)
    {
        uint16_t *was = np_img_buf;
        np_img_buf = nps.buf;
        nps.buf = was;
        np_img = nps.dsc;
        np_img.data = (const uint8_t *)np_img_buf;
        lv_image_cache_drop(&np_img);
        lv_image_set_src(np_art_img, &np_img);
        hidden_if_changed(np_art_img, false);
        hidden_if_changed(np_art_none, true);
        lv_obj_set_style_bg_color(np_art, lv_color_black(), 0);
        np_img_album = nps.album;
        np_img_provisional = true;           /* the full-size cover replaces it when it comes */
        np_glow_soon();
    }
    else if (nps.side)
    {
        lv_color_t bg = lv_obj_get_style_bg_color(nps.peek, 0);
        hidden_if_changed(np_art_img, true);
        lv_obj_set_style_bg_color(np_art, bg, 0);
        lv_obj_set_style_bg_color(np_art_none, bg, 0);
        hidden_if_changed(np_art_none, false);
        np_img_album = -1;
        np_glow_soon();
    }
    if (nps.side && nps.glow_ready)
    {
        /* the reflection that slid in is the cover's now: the buffers change places */
        uint8_t *was = np_glow_buf;
        np_glow_buf = nps.glow_buf;
        nps.glow_buf = was;
        ng_show(np_glow, &np_glow_img, np_glow_buf);
        nps.glow_ready = false;
    }
    nps.path[0] = '\0';                      /* the buffer holds the old cover now */
    nps.side = 0;
    nps_set_x(NULL, 0);
    if (nps.glow) lv_obj_add_flag(nps.glow, LV_OBJ_FLAG_HIDDEN);
    if (np_glow) lv_obj_set_style_translate_x(np_glow, 0, 0);
}

static void nps_slide(int from, int to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, np_art);
    lv_anim_set_exec_cb(&a, nps_set_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SL_T_SPAN((int)abs(to - from), W));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void nps_back(lv_anim_t *a)
{
    (void)a;
    nps.sliding = false;
    nps.side = 0;
    nps_set_x(NULL, 0);
    if (nps.glow) lv_obj_add_flag(nps.glow, LV_OBJ_FLAG_HIDDEN);
    if (np_glow) lv_obj_set_style_translate_x(np_glow, 0, 0);
}

/* A slide that is still going: finish it now (a new touch, a new skip) */
static void nps_finish(void)
{
    if (!nps.sliding) return;
    lv_anim_delete(np_art, nps_set_x);
    if (nps.landing) nps_land(NULL); else nps_back(NULL);
}

/* Play the neighbour on this side now; its cover slides in after. */
static void nps_go(int side)
{
    struct mp3entry *id3 = audio_current_track();
    strlcpy(nps.skipped_from, id3 ? id3->path : "", sizeof nps.skipped_from);
    nps.skipped_at = lv_tick_get();
    sl_log("np: %s by %s to %s", side > 0 ? "next" : "previous", nps.axis == 1 ? "swipe" : "button", nps.path);
    strlcpy(nps.expect, nps.path, sizeof nps.expect);
    if (side > 0) sl_next(); else sl_prev_song();
    nps.sliding = nps.landing = true;
    nps_slide(nps.dx, -side * W, nps_land);
}

static int cue_now(const struct mp3entry *id3, long *start, long *len);
/* a cue sheet's tracks share their file and its cover: no cover to slide in */
static bool nps_can(void) { return np_open && !vnl_on() && !lyr_visible() && nps.peek && !np_centred() && cue_now(audio_current_track(), NULL, NULL) < 0; }   /* Centred: the buttons only */

/* The next and previous buttons: the same slide, when it can show the
 * right song; otherwise the plain skip (previous keeps its restart). */
static void np_skip(int side)
{
    nps_finish();
    struct mp3entry *id3 = audio_current_track();
    bool restart = side < 0 && id3 && id3->elapsed > 3000;
    int idx = restart || !nps_can() || nps_pending() ? -1 : np_neighbour(side);
    if (idx < 0 || !nps_load(idx))
    {
        if (side > 0) sl_next(); else sl_prev();
        return;
    }
    nps.side = side;
    nps.dx = 0;
    nps.axis = 0;
    nps_go(side);
}

/* The touch on the sheet: is it a swipe of the cover? True while it is,
 * so the sheet's own pull leaves it alone. */
static bool nps_drag(lv_event_code_t c, lv_point_t p)
{
    if (c == LV_EVENT_PRESSED)
    {
        nps_finish();
        nps.axis = 0;
        nps.x0 = nps.last_x = p.x;
        nps.y0 = p.y;
        nps.vx = 0;
        nps.last_at = lv_tick_get();
        return false;
    }
    int dx = p.x - nps.x0, dy = p.y - nps.y0;
    if (c == LV_EVENT_PRESSING)
    {
        if (!nps.axis && (abs(dx) > NPS_LOCK || abs(dy) > NPS_LOCK))
        {
            nps.axis = abs(dx) > abs(dy) && nps.y0 < W && nps_can() ? 1 : 2;
            if (nps.axis == 1) { lv_anim_delete(np, NULL); lv_obj_set_y(np, 0); }   /* the pull it began, undone */
        }
        if (nps.axis != 1) return false;
        uint32_t dt = lv_tick_elaps(nps.last_at);
        if (dt) nps.vx = (nps.vx + (p.x - nps.last_x) * 1000 / (int)dt) / 2;
        nps.last_x = p.x;
        nps.last_at = lv_tick_get();
        int side = dx < 0 ? 1 : -1;
        if (dx && side != nps.side)
        {
            int idx = nps_pending() ? -1 : np_neighbour(side);
            nps.side = idx >= 0 && nps_load(idx) ? side : 0;
            if (nps.side && !nps.glow_ready) nps_glow_make();   /* not ready ahead: now, so it still travels */
        }
        /* one to one with a song to bring in; stiff at an end */
        int x = !nps.side ? dx / 4 : dx > W ? W : dx < -W ? -W : dx;
        lv_anim_delete(np_art, nps_set_x);
        nps_set_x(NULL, x);
        return true;
    }
    if (nps.axis != 1) return false;
    /* let go: past a third, or a flick its way; a contact taken away goes back */
    bool go = c == LV_EVENT_RELEASED && nps.side && (dx < 0) == (nps.side > 0) &&
              (abs(dx) > W / 3 || (abs(dx) > 40 && abs(nps.vx) > 500 && (nps.vx < 0) == (dx < 0)));
    if (go) nps_go(nps.side);
    else { nps.sliding = true; nps_slide(nps.dx, 0, nps_back); }
    nps.axis = 0;
    return true;
}

/* A song has settled with the sheet open: get the next one's cover ready,
 * a moment later, so a swipe or the button starts with it. */
static void nps_preload_cb(lv_timer_t *t)
{
    (void)t;
    nps.preload = NULL;
    if (!nps_can() || nps.axis || nps.sliding || nps_pending()) return;
    int idx = np_neighbour(1);
    if (idx >= 0 && nps_load(idx) && !nps.glow_ready) nps_glow_make();   /* a cover loaded before the glass was */
}
static void nps_track_changed(void)
{
    if (!np_open) return;
    if (nps.preload) lv_timer_reset(nps.preload);
    else
    {
        nps.preload = lv_timer_create(nps_preload_cb, 600, NULL);
        lv_timer_set_repeat_count(nps.preload, 1);
    }
}

/* The sheet is shut: the peek's buffer goes back */
static void nps_release(void)
{
    nps_finish();
    if (nps.preload) { lv_timer_delete(nps.preload); nps.preload = NULL; }
    if (nps.peek_img) lv_image_set_src(nps.peek_img, NULL);
    lv_image_cache_drop(&nps.dsc);
    free(nps.buf);
    nps.buf = NULL;
    nps.path[0] = '\0';
    if (nps.glow) { lv_obj_add_flag(nps.glow, LV_OBJ_FLAG_HIDDEN); lv_image_set_src(nps.glow, NULL); }
    lv_image_cache_drop(&nps.glow_dsc);
    free(nps.glow_buf);
    nps.glow_buf = NULL;
    nps.glow_ready = false;
}

static void nps_build(lv_obj_t *parent)
{
    nps.peek = box(parent);
    lv_obj_set_size(nps.peek, W, W);
    lv_obj_set_style_bg_opa(nps.peek, LV_OPA_COVER, 0);
    lv_obj_add_flag(nps.peek, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_move_to_index(nps.peek, lv_obj_get_index(np_art) + 1);
    nps.peek_img = lv_image_create(nps.peek);
    lv_obj_center(nps.peek_img);
    lv_obj_add_flag(nps.peek_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    nps.peek_none = sl_icon(nps.peek, IC_ALBUMS, 176, 0xffffff, 0x000000);
    lv_obj_center(nps.peek_none);
    lv_obj_set_style_text_color(nps.peek_none, lv_color_hex(0xb4b4b4), 0);
}
