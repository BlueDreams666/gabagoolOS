/* GabagoolOS synced lyrics in Now Playing. GPLv2+.
 * Included by sloppy.c before np_build. An .lrc file next to the song
 * ("song.mp3" -> "song.lrc") is read when the track changes; a Lyrics
 * button over the cover opens a panel that scrolls with the song, karaoke
 * style, and a tap on a timed line seeks there. Plain (untimed) lyrics
 * scroll freely.
 *
 * Timed lyrics read like Spotify's: the line being sung is bigger and lit,
 * held in the middle of the panel; the rest are smaller and dimmed. A line
 * lights at its own timestamp and stays lit until the next one: no timing
 * is made up between them. A finger scrolling the panel is left alone;
 * four seconds after it lets go the panel glides back to the line. */
#include "sloppy_lyrics_state.h"
#include "sloppy_taglyrics.h"

static struct sl_lrc lyr;
static char *lyr_text;                 /* UTF-8 buffer the lines point into */
static char lyr_path[MAX_PATH];        /* track the lyrics belong to */
static lv_obj_t *lyr_btn, *lyr_panel, *lyr_list;
static lv_obj_t *lyr_rows[SL_LRC_MAX_LINES];
static lv_timer_t *lyr_timer;
static int lyr_cur = -2;               /* highlighted line; -2 = none drawn */
static bool lyr_want;                  /* keep the panel open for the next song */
static uint32_t lyr_hold_until;        /* the user scrolled: don't follow */

static bool lyr_visible(void)
{
    return lyr_panel && !lv_obj_has_flag(lyr_panel, LV_OBJ_FLAG_HIDDEN);
}
/* the lane the lines scroll in: under the buttons, down to the cover's
 * foot (lyr_layout moves it for the status bar and Centred) */
static int lyr_top = LYR_TOP0, lyr_bottom = W;

/* Settings > Customization > Lyrics: four sizes, on top of Text size. The
 * line being sung two steps up the ladder from the rest. Standard at the
 * standard Text size is what lyrics always were: 23 and 30. */
static const char *const lyr_size_names[] = { "Small", "Standard", "Large", "Extra large" };
static const lv_font_t *lyr_font(bool current, bool synced)
{
    const lv_font_t *const ladder[] = { sl_fonts[0][2], sl_fonts[0][3], sl_fonts[0][4], sl_fonts[0][5],
                                        &ss_semibold_30, &ss_semibold_34, &ss_semibold_38 };
    int size = global_settings.sloppy_lyrics_size;
    if (size < 0 || size > 3) size = 1;
    int o = 2 + size - 1 + sl_text_step;
    o = o < 0 ? 0 : o > 6 ? 6 : o;
    int cur = o + 2 > 6 ? 6 : o + 2;
    if (o >= cur) o = cur - 1;
    if (!synced) return ladder[o + 1 > 6 ? 6 : o + 1];   /* plain lyrics: one size, a step up from the dim lines */
    return ladder[current ? cur : o];
}

static bool sl_reading_lyrics(void) { return np_open && lyr_visible() && lyr.n > 0 && is_playing(); }

static void lyr_free(void)
{
    free(lyr_text);
    lyr_text = NULL;
    lyr.n = 0;
    lyr.synced = false;
}

/* Read the song's lyrics into lyr: "<song>.lrc", else "<song>.txt", else
 * the ones in its tags. Returns false when there are none. */
static bool lyr_read(const char *track)
{
    char path[MAX_PATH];
    int fd = -1;
    for (int k = 0; k < 4 && fd < 0; k++)
        if (sl_lrc_path(track, path, sizeof path, k & 1))
        {
            if (k >= 2) memcpy(path + strlen(path) - 4, k & 1 ? ".TXT" : ".txt", 4);
            fd = open(path, O_RDONLY);
        }
    unsigned char *raw = malloc(SL_LRC_MAX_BYTES + 1);
    ssize_t size = 0, n = 1;
    if (fd >= 0)
    {
        while (raw && size <= SL_LRC_MAX_BYTES &&
               (n = read(fd, raw + size, SL_LRC_MAX_BYTES + 1 - size)) > 0)
            size += n;
        close(fd);
    }
    else if (raw && (fd = open(track, O_RDONLY)) >= 0)
    {
        size = (ssize_t)sl_tag_lyrics(fd, raw, SL_LRC_MAX_BYTES + 1);
        close(fd);
    }
    bool ok = raw && n >= 0 && size > 0 && size <= SL_LRC_MAX_BYTES;   /* too big: skip it */
    if (ok)
    {
        /* single-byte text grows to at most 3 bytes a character in UTF-8 */
        size_t cap = (size_t)size * 3 + 4;
        lyr_text = malloc(cap);
        ok = lyr_text != NULL;
        if (ok)
        {
            sl_lrc_to_utf8(raw, (size_t)size, (unsigned char *)lyr_text, cap);
            sl_lrc_parse(lyr_text, &lyr);
            ok = lyr.n > 0;
        }
    }
    free(raw);
    if (!ok) lyr_free();
    return ok;
}

/* Settings > Customization > Lyrics colour: the line being sung */
static const char *const lyr_color_names[] = { "White", "Accent", "Gold", "Mint", "Pink", "Sky" };
static const char *lyr_color_now(void)
{
    int c = global_settings.sloppy_lyrics_color;
    return c >= 0 && c < 6 ? lyr_color_names[c] : lyr_color_names[0];
}
static uint32_t lyr_color(int i)
{
    static const uint32_t c[] = { 0xffffff, 0, 0xf5c542, 0x6ee7b7, 0xf472b6, 0x7dd3fc };
    if (i < 0 || i >= (int)ARRAYLEN(c)) i = 0;
    return i == 1 ? (uint32_t)C_ACCENT : c[i];
}

#define LYR_LIST_TOP lyr_top
#define LYR_LIST_H   (lyr_bottom - lyr_top)
#define LYR_DIM      150                /* the lines not being sung */

static void lyr_set_opa(void *o, int32_t v) { lv_obj_set_style_text_opa(o, (lv_opa_t)v, 0); }

static void lyr_style_row(int i, bool current)
{
    if (i < 0 || i >= lyr.n || !lyr_rows[i]) return;
    lv_obj_t *l = lyr_rows[i];
    lv_anim_delete(l, lyr_set_opa);
    lv_obj_set_style_text_color(l, lv_color_hex(current ? lyr_color(global_settings.sloppy_lyrics_color) : 0x8f98aa), 0);
    lv_obj_set_style_text_opa(l, current || !lyr.synced ? LV_OPA_COVER : LYR_DIM, 0);
    if (lyr.synced) lv_obj_set_style_text_font(l, lyr_font(current, true), 0);
}

/* Bring line i to the middle of the panel. */
static void lyr_scroll_to(int i, bool anim)
{
    if (i < 0 || i >= lyr.n || !lyr_rows[i]) return;
    int y = lv_obj_get_style_pad_top(lyr_list, 0) + lv_obj_get_y(lyr_rows[i]) +
            lv_obj_get_height(lyr_rows[i]) / 2 - LYR_LIST_H / 2;
    lv_obj_scroll_to_y(lyr_list, y < 0 ? 0 : y, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

/* A finger on the panel, or just lifted from it: the scrolling is its. */
static bool lyr_held(void)
{
    if (touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED && lv_indev_get_scroll_obj(touch) == lyr_list)
        return true;
    return (int32_t)(sl_ms() - lyr_hold_until) < 0;
}

/* Follow the song: highlight the line being sung and keep it in view. */
static void lyr_follow(bool force)
{
    struct mp3entry *id3 = audio_current_track();
    if (!lyr_visible() || !lyr.synced || !id3) return;
    /* a little lead: a line should light up as it starts, not after */
    int i = sl_lrc_find(&lyr, (uint32_t)visible_elapsed(id3) + 150);
    if (i == lyr_cur && !force) return;
    /* The lit line is set bigger and the last one smaller, so lines move.
     * The scroll shifts by as much as the new line moved: what is on
     * screen stays put, and the glide to the middle starts from there. */
    lv_obj_t *anchor = lyr_rows[i < 0 ? 0 : i];
    int was = anchor ? lv_obj_get_y(anchor) : 0;
    lyr_style_row(lyr_cur, false);
    lyr_style_row(i, true);
    lyr_cur = i;
    lv_obj_update_layout(lyr_list);
    int moved = anchor ? lv_obj_get_y(anchor) - was : 0;
    if (moved && !force) lv_obj_scroll_to_y(lyr_list, lv_obj_get_scroll_y(lyr_list) + moved, LV_ANIM_OFF);
    if (i >= 0 && !force)
    {
        /* the new line brightens in rather than blinking on */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, lyr_rows[i]);
        lv_anim_set_exec_cb(&a, lyr_set_opa);
        lv_anim_set_values(&a, LYR_DIM, LV_OPA_COVER);
        lv_anim_set_duration(&a, SL_T_PAGE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    if (force || !lyr_held())
        lyr_scroll_to(i < 0 ? 0 : i, !force);
}

static void lyr_tick(lv_timer_t *t)
{
    if (!lyr_visible()) { lv_timer_pause(t); return; }
    if (!np_open) return;               /* keep the timer: Now Playing may come back to this song */
    lyr_follow(false);
    /* the hold ran out with the panel scrolled away: back to the line,
     * even if it has not changed since */
    if (lyr.synced && lyr_hold_until && !lyr_held())
    {
        lyr_hold_until = 0;
        lyr_scroll_to(lyr_cur < 0 ? 0 : lyr_cur, true);
    }
}

static void lyr_line_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!lyr.synced || i < 0 || i >= lyr.n || !audio_current_track()) return;
    if (!(audio_status() & AUDIO_STATUS_PAUSE))
        audio_pre_ff_rewind();
    audio_ff_rewind(lyr.lines[i].ms);
    lyr_hold_until = 0;
    sl_log("lyrics: seek to line %d (%lu ms)", i, (unsigned long)lyr.lines[i].ms);
}

static void lyr_scroll_cb(lv_event_t *e)
{
    (void)e;
    /* a finger or its fling, not our follow: kept up while it moves. Our
     * own scroll can run inside a touch too (the Lyrics button opening
     * the panel), so the touch must be the one scrolling the list. */
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_scroll_obj(in) == lyr_list) lyr_hold_until = sl_ms() + 4000;
}

/* One label per line, built when the panel opens for a song. */
static void lyr_fill(void)
{
    lv_obj_clean(lyr_list);
    memset(lyr_rows, 0, sizeof lyr_rows);
    lyr_cur = -2;
    /* room to bring the first line to the middle; plain lyrics start higher */
    lv_obj_set_style_pad_top(lyr_list, lyr.synced ? LYR_LIST_H / 2 : W / 3, 0);
    for (int i = 0; i < lyr.n; i++)
    {
        const char *s = lyr.lines[i].text;
        lv_obj_t *l = text(lyr_list, *s ? s : lyr.synced ? "\xc2\xb7  \xc2\xb7  \xc2\xb7" : " ",
                           lyr_font(false, lyr.synced), 0x8f98aa);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_ver(l, lyr.synced ? 9 : 7, 0);
        if (lyr.synced)
        {
            on_click(l, lyr_line_cb, (void *)(intptr_t)i);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        lyr_rows[i] = l;
        lyr_style_row(i, false);
    }
    lv_obj_update_layout(lyr_list);
    lv_obj_scroll_to_y(lyr_list, 0, LV_ANIM_OFF);
}

/* Behind the lines: the cover, blurred and darkened, so nothing on it shows
 * through (the record placeholder and the title did, N4). Made once a
 * cover, only while the lyrics are open; with Centred, its own screen
 * backdrop already is that. The lines fade in and out at the lane's ends
 * through strips cut from the same picture, not black bands. Short of
 * memory: the old dark veil. */
#define LYR_FADE 40
/* On the glass the backdrop reaches a little past the cover's foot, over
 * the lake's bright waterline, and melts into the lake where it has
 * already darkened under the song's title. */
#define LYR_BD_X 24
#define LYR_BD_H (W + LYR_BD_X)
#define LYR_BD_C (NC_Y + 14 + NC_S + 66)   /* Centred: down past the cover's shadow */
#define LYR_BD_MAX (LYR_BD_C > LYR_BD_H ? LYR_BD_C : LYR_BD_H)
static uint16_t *lyr_bd_px;              /* W x LYR_BD_MAX at most */
static uint8_t *lyr_fade_px;             /* the two fades, RGB565A8, W x LYR_FADE each */
static lv_image_dsc_t lyr_bd_dsc, lyr_fade_dsc[2];
static lv_obj_t *lyr_bd, *lyr_fade_img[2], *lyr_fade_box[2];
static uint32_t lyr_bd_key;
static const uint16_t *lyr_bd_src;       /* what the lane shows: lyr_bd_px, Centred's screen, or none */
static const uint16_t *lyr_backdrop(void) { return lyr_visible() ? lyr_bd_src : NULL; }
static void lyr_fade_make(int k, const uint16_t *src)
{
    uint8_t *buf = lyr_fade_px + (size_t)k * W * LYR_FADE * 3;
    uint16_t *rgb = (uint16_t *)buf;
    uint8_t *alpha = buf + (size_t)W * LYR_FADE * 2;
    int y0 = k ? lyr_bottom - LYR_FADE : lyr_top;
    for (int y = 0; y < LYR_FADE; y++)
    {
        int t = (k ? y + 1 : LYR_FADE - y) * 256 / LYR_FADE;   /* 256 at the lane's very end */
        int a = t * t * (768 - 2 * t) >> 16;
        int sy = y0 + y < 0 ? 0 : y0 + y >= H ? H - 1 : y0 + y;
        memcpy(rgb + y * W, src + (size_t)sy * W, W * 2);
        memset(alpha + y * W, a > 255 ? 255 : a, W);
    }
    lyr_fade_dsc[k].header.magic = LV_IMAGE_HEADER_MAGIC;
    lyr_fade_dsc[k].header.cf = LV_COLOR_FORMAT_RGB565A8;
    lyr_fade_dsc[k].header.w = W;
    lyr_fade_dsc[k].header.h = LYR_FADE;
    lyr_fade_dsc[k].header.stride = W * 2;
    lyr_fade_dsc[k].data = buf;
    lyr_fade_dsc[k].data_size = (uint32_t)W * LYR_FADE * 3;
    lv_image_cache_drop(&lyr_fade_dsc[k]);
    lv_image_set_src(lyr_fade_img[k], &lyr_fade_dsc[k]);
    lv_obj_set_pos(lyr_fade_img[k], 0, y0);
    lv_obj_invalidate(lyr_fade_img[k]);
}
static void lyr_bd_sync(bool force)
{
    if (!lyr_panel || !lyr_visible()) return;
    const uint16_t *src = NULL;
    {
        bool c = np_centred();
        uint32_t key = ng_key() ^ (np_glass() ? 0x40000000u : 0) ^ (c ? 0x20000000u : 0) ^ (uint32_t)lyr_bottom << 20;
        int bh = c ? LYR_BD_C : LYR_BD_H;   /* Centred: past the cover's shadow, where its own screen takes over */
        if (!lyr_bd_px)
        {
            long kb = sl_mem_available_kb();
            if (kb < 0 || kb >= NG_MIN_FREE_KB) lyr_bd_px = malloc((size_t)W * LYR_BD_MAX * 2);
            force = true;
        }
        if (lyr_bd_px && (force || key != lyr_bd_key))
        {
            lyr_bd_key = key;
            bool pic = !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data && np_img.header.w;
            /* on the glass: the last rows melt into the lake's first one, under the panel's foot */
            const uint16_t *foot = np_glass() && np_glow_buf && !lv_obj_has_flag(np_glow, LV_OBJ_FLAG_HIDDEN) ?
                                   (const uint16_t *)np_glow_buf + (size_t)(NG_FADE + LYR_BD_X) * W :
                                   c && nc_bd_px ? nc_bd_px + (size_t)bh * W : NULL;
            if (pic)
                nb_paint(lyr_bd_px, W, bh, c ? H : LYR_BD_H, (const uint16_t *)np_img.data, np_img.header.w, np_img.header.h,
                         np_img.header.stride / 2, lv_color_black(), c ? 84 : 80, foot);
            else
                nb_paint(lyr_bd_px, W, bh, c ? H : LYR_BD_H, NULL, 0, 0, 0, lv_obj_get_style_bg_color(np_art, 0), 80, foot);
            lyr_bd_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            lyr_bd_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            lyr_bd_dsc.header.w = W;
            lyr_bd_dsc.header.h = bh;
            lyr_bd_dsc.header.stride = W * 2;
            lyr_bd_dsc.data = (const uint8_t *)lyr_bd_px;
            lyr_bd_dsc.data_size = (uint32_t)W * bh * 2;
            lv_image_cache_drop(&lyr_bd_dsc);
            lv_image_set_src(lyr_bd, &lyr_bd_dsc);
            lv_obj_invalidate(lyr_bd);
        }
        src = lyr_bd_px;
    }
    if (src && !lyr_fade_px) lyr_fade_px = malloc((size_t)2 * W * LYR_FADE * 3);
    if (!lyr_fade_px) src = NULL;
    lyr_bd_src = src;
    lv_obj_set_style_bg_opa(lyr_panel, src ? LV_OPA_TRANSP : 205, 0);
    lv_obj_set_height(lyr_panel, src && (np_glass() || np_centred()) ? (int)lyr_bd_dsc.header.h : lyr_bottom);
    lv_obj_update_flag(lyr_bd, LV_OBJ_FLAG_HIDDEN, !src);
    for (int k = 0; k < 2; k++)
    {
        if (src) lyr_fade_make(k, src);
        lv_obj_update_flag(lyr_fade_img[k], LV_OBJ_FLAG_HIDDEN, !src);
        lv_obj_update_flag(lyr_fade_box[k], LV_OBJ_FLAG_HIDDEN, src != NULL);
    }
}
static void lyr_bd_free(void)
{
    if (lyr_bd) lv_image_set_src(lyr_bd, NULL);
    for (int k = 0; k < 2; k++) if (lyr_fade_img[k]) lv_image_set_src(lyr_fade_img[k], NULL);
    lv_image_cache_drop(&lyr_bd_dsc);
    lv_image_cache_drop(&lyr_fade_dsc[0]);
    lv_image_cache_drop(&lyr_fade_dsc[1]);
    free(lyr_bd_px); free(lyr_fade_px);
    lyr_bd_px = NULL; lyr_fade_px = NULL; lyr_bd_src = NULL;
}
/* the lane between top and bottom (np_layout) */
static void lyr_layout(int top, int bottom)
{
    if (!lyr_panel || (top == lyr_top && bottom == lyr_bottom && lv_obj_get_height(lyr_panel) == bottom)) return;
    lyr_top = top;
    lyr_bottom = bottom;
    lv_obj_set_size(lyr_panel, W, bottom);
    lv_obj_set_size(lyr_list, W, bottom - top);
    lv_obj_set_pos(lyr_list, 0, top);
    lv_obj_set_style_pad_bottom(lyr_list, (bottom - top) / 2, 0);
    lv_obj_set_pos(lyr_fade_box[0], 0, top);
    lv_obj_set_pos(lyr_fade_box[1], 0, bottom - LYR_FADE);
    if (lyr_visible()) { lyr_fill(); lyr_follow(true); lyr_bd_sync(false); }
}

static void lyr_show(bool on)
{
    if (!lyr_panel) return;
    if (on && !lyr.n) on = false;
    if (on == lyr_visible()) return;
    if (on)
    {
        lyr_fill();
        lv_obj_remove_flag(lyr_panel, LV_OBJ_FLAG_HIDDEN);
        lyr_bd_sync(false);
        lyr_hold_until = 0;
        lyr_follow(true);
        if (lyr.synced)
        {
            if (!lyr_timer) lyr_timer = lv_timer_create(lyr_tick, 100, NULL);
            lv_timer_resume(lyr_timer);
        }
    }
    else
    {
        lv_obj_add_flag(lyr_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(lyr_list);
        memset(lyr_rows, 0, sizeof lyr_rows);
        if (lyr_timer) lv_timer_pause(lyr_timer);
        lyr_bd_src = NULL;
    }
    if (!vnl_on()) hidden_if_changed(np_art, on && np_centred());   /* Centred: the lines over the blur, the cover put away */
    ng_buttons();                        /* the lenses bend what is under them now */
    lv_obj_set_style_bg_color(lyr_btn, lv_color_hex(on ? C_ACCENT : 0x000000), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(lyr_btn, 0), lv_color_hex(on ? sl_accent_ink() : 0xffffff), 0);   /* readable on any accent */
    lv_obj_set_style_bg_opa(lyr_btn, on ? LV_OPA_COVER : LV_OPA_30, 0);
}

/* No .lrc on the card: ask LRCLIB over Wi-Fi, by the song's tags. Every
 * version it knows comes back (the album cut, a live one, words without
 * timing); one that plainly matches is taken at once, otherwise a sheet
 * lists them, each marked Synced or Text only. Hold the Lyrics button to
 * choose again. The file lands next to the song, so it is there offline
 * from then on. With no Wi-Fi the button asks to turn it on, and looks
 * once it connects. */
static bool lyr_can_search(const struct mp3entry *id3)
{
    return id3 && id3->title && id3->title[0] && id3->artist && id3->artist[0] && !sl_net_wireless_unsupported();
}
static bool lyr_can_fetch(const struct mp3entry *id3) { return lyr_can_search(id3) && wf_online(); }
#define LYR_FOUND 6
static struct lyr_found { char title[96], artist[96], album[96]; int secs; bool synced; char *text; } lyr_found[LYR_FOUND];
static int lyr_n_found;
static char lyr_found_for[MAX_PATH];     /* the song they were looked up for */
static bool lyr_wait_wifi;               /* asked to turn Wi-Fi on: look once it connects */
static void lyr_found_free(void)
{
    for (int i = 0; i < lyr_n_found; i++) free(lyr_found[i].text);
    lyr_n_found = 0;
}
/* a URL's query part: letters and digits as they are, the rest %XX */
static void lyr_url_part(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; s && *s && n + 4 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.')
            out[n++] = (char)c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = '\0';
}
/* "key": a string (its escapes undone, at most cap - 1 bytes; NULL cap 0
 * just measures it), a number, or null, inside one result [o, end) */
static const char *lyr_json_at(const char *o, const char *end, const char *key)
{
    char k[32];
    snprintf(k, sizeof k, "\"%s\"", key);
    size_t kl = strlen(k);
    for (const char *p = o; p + kl < end; p++)
        if (!memcmp(p, k, kl))
        {
            p += kl;
            while (p < end && (*p == ' ' || *p == ':')) p++;
            return p < end ? p : NULL;
        }
    return NULL;
}
static size_t lyr_json_str(const char *o, const char *end, const char *key, char *out, size_t cap)
{
    const char *p = lyr_json_at(o, end, key);
    if (!p || *p != '"') { if (cap) out[0] = '\0'; return 0; }
    size_t n = 0;
    for (p++; p < end && *p != '"'; p++)
    {
        char c = *p;
        unsigned v = 0;
        if (c == '\\' && p + 1 < end)
        {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = ' ';
            else if (c == 'r') continue;
            else if (c == 'u' && p + 4 < end)
            {
                for (int i = 1; i <= 4; i++) { char h = p[i] | 32; v = v * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h - '0'); }
                p += 4;
                char u[3];
                int m = v < 0x80 ? 1 : v < 0x800 ? 2 : 3;
                if (m == 1) u[0] = (char)v;
                else if (m == 2) { u[0] = (char)(0xc0 | v >> 6); u[1] = (char)(0x80 | (v & 0x3f)); }
                else { u[0] = (char)(0xe0 | v >> 12); u[1] = (char)(0x80 | ((v >> 6) & 0x3f)); u[2] = (char)(0x80 | (v & 0x3f)); }
                for (int i = 0; i < m; i++, n++) if (cap && n + 1 < cap) out[n] = u[i];
                continue;
            }
        }
        if (cap && n + 1 < cap) out[n] = c;
        n++;
    }
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}
/* the chosen one, written next to the song and shown */
static void lyr_take(int i)
{
    struct mp3entry *id3 = audio_current_track();
    char path[MAX_PATH];
    if (i < 0 || i >= lyr_n_found || !id3 || strcmp(id3->path, lyr_found_for) || !sl_lrc_path(id3->path, path, sizeof path, false))
    { lyr_found_free(); return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    size_t len = strlen(lyr_found[i].text);
    bool ok = fd >= 0 && write(fd, lyr_found[i].text, len) == (ssize_t)len;
    if (fd >= 0) close(fd);
    lyr_found_free();
    if (!ok) { toast_warn("Could not save the lyrics"); return; }
    toast("Lyrics saved next to the song");
    lyr_path[0] = '\0';
    lyr_want = true;
    lyr_track(id3->path);
}
static void acts_begin(void);
static void acts_show(void);
static void acts_close(void);
static void acts_row_cb(const char *sym, const char *label, lv_event_cb_t cb, intptr_t ud);
static lv_obj_t *acts;
static void lyr_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    lyr_take(i);
}
/* a sheet's head without a cover: a title and a line, then the hairline */
static void lyr_sheet_head(const char *title, const char *line)
{
    lv_obj_t *hd = box(acts);
    lv_obj_set_size(hd, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(hd, 16, 0);
    lv_obj_set_style_pad_ver(hd, 12, 0);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hd, SL_SP_XS, 0);
    lv_obj_t *t = text(hd, title, &ss_semibold_23, 0xffffff);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_t *l = text(hd, line, &ss_regular_16, C_SUB);
    lv_obj_set_width(l, lv_pct(100));
    if (sl_ui_rtl()) { lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0); }
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);
    lv_obj_set_style_bg_opa(sep, sl_zune() ? LV_OPA_COVER : 36, 0);
}
static void lyr_pick_sheet(void)
{
    char line[64];
    acts_begin();
    lyr_sheet_head("Choose lyrics", sl_count(line, sizeof line, lyr_n_found, "%s version on LRCLIB", "%s versions on LRCLIB"));
    for (int i = 0; i < lyr_n_found; i++)
    {
        const struct lyr_found *f = &lyr_found[i];
        lv_obj_t *r = box(acts);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(r, 64, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_set_style_pad_ver(r, 10, 0);
        lv_obj_set_style_radius(r, SL_R_XL - SL_FLOAT, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
        /* the badge first, so the words know their room */
        lv_obj_t *pill = box(r);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_pad_hor(pill, 12, 0);
        lv_obj_set_style_radius(pill, SL_R_FULL, 0);
        /* synced: a filled accent badge; words only: its outline, quieter */
        lv_obj_set_style_bg_opa(pill, f->synced ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_width(pill, f->synced ? 0 : 1, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(C_SUB), 0);
        lv_obj_t *pl = text(pill, f->synced ? "Synced" : "Text only", &ss_semibold_18, f->synced ? sl_accent_ink() : C_SUB);
        lv_obj_center(pl);
        sl_align(pill, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_update_layout(pill);
        int room = W - 20 - 2 * 10 - 32 - lv_obj_get_width(pill) - 12;
        lv_obj_t *t = text(r, "", &ss_semibold_21, 0xffffff);
        char b[200];
        snprintf(b, sizeof b, "%s \xc2\xb7 %s", f->title, f->artist);
        lv_label_set_text(t, b);
        lv_obj_set_width(t, room);
        label_lines(t, 1);
        sl_place(t, 0, 0);
        char m[160], d[16];
        snprintf(d, sizeof d, "%d:%02d", f->secs / 60, f->secs % 60);
        if (f->album[0]) snprintf(m, sizeof m, "%s \xc2\xb7 %s", f->album, d);
        else strlcpy(m, d, sizeof m);
        lv_obj_t *l = text(r, "", &ss_regular_16, C_SUB);
        lv_label_set_text(l, m);
        lv_obj_set_width(l, room);
        label_lines(l, 1);
        sl_place(l, 0, lv_font_get_line_height(&ss_semibold_21) + 2);
        on_click(r, lyr_pick_cb, (void *)(intptr_t)i);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    acts_show();
}
/* LRCLIB's answer: each result with words in it, the synced ones first,
 * then the nearest in length to the song */
static int lyr_found_cmp(const void *a, const void *b)
{
    const struct lyr_found *x = a, *y = b;
    if (x->synced != y->synced) return x->synced ? -1 : 1;
    struct mp3entry *id3 = audio_current_track();
    int len = id3 ? (int)(id3->length / 1000) : 0;
    return abs(x->secs - len) - abs(y->secs - len);
}
static void lyr_search_done(bool ok, const char *json, int n)
{
    struct mp3entry *id3 = audio_current_track();
    lyr_found_free();
    if (!ok || !json || !id3 || strcmp(id3->path, lyr_found_for)) { if (!ok) toast("No lyrics found online"); return; }
    const char *end = json + n;
    for (const char *p = json; p < end && lyr_n_found < LYR_FOUND; )
    {
        p = memchr(p, '{', (size_t)(end - p));
        if (!p) break;
        /* the result's end: its closing brace, strings skipped (one cut short ends the list) */
        const char *q = p + 1;
        int depth = 1;
        for (; q < end && depth; q++)
        {
            if (*q == '"') { for (q++; q < end && *q != '"'; q++) if (*q == '\\') q++; }
            else if (*q == '{') depth++;
            else if (*q == '}') depth--;
        }
        if (depth) break;
        struct lyr_found *f = &lyr_found[lyr_n_found];
        const char *key = "syncedLyrics";
        size_t len = lyr_json_str(p, q, key, NULL, 0);
        if (!len) { key = "plainLyrics"; len = lyr_json_str(p, q, key, NULL, 0); }
        if (len && (f->text = malloc(len + 1)))
        {
            lyr_json_str(p, q, key, f->text, len + 1);
            f->synced = key[0] == 's';
            lyr_json_str(p, q, "trackName", f->title, sizeof f->title);
            lyr_json_str(p, q, "artistName", f->artist, sizeof f->artist);
            lyr_json_str(p, q, "albumName", f->album, sizeof f->album);
            const char *d = lyr_json_at(p, q, "duration");
            f->secs = d ? atoi(d) : 0;
            lyr_n_found++;
        }
        p = q;
    }
    if (!lyr_n_found) { toast("No lyrics found online"); return; }
    qsort(lyr_found, lyr_n_found, sizeof lyr_found[0], lyr_found_cmp);
    /* one synced version the song's own length, and no other near it: plainly the one */
    int len = (int)(id3->length / 1000), near = 0;
    for (int i = 0; i < lyr_n_found; i++) if (lyr_found[i].synced && abs(lyr_found[i].secs - len) <= 2) near++;
    if (near == 1 && lyr_found[0].synced && abs(lyr_found[0].secs - len) <= 2 && !lyr.n) lyr_take(0);
    else lyr_pick_sheet();
}
static void lyr_search(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!lyr_can_fetch(id3)) return;
    char a[300], t[300], url[700];
    lyr_url_part(a, sizeof a, id3->artist);
    lyr_url_part(t, sizeof t, id3->title);
    snprintf(url, sizeof url, "https://lrclib.net/api/search?track_name=%s&artist_name=%s", t, a);
    strlcpy(lyr_found_for, id3->path, sizeof lyr_found_for);
    if (!wf_fetch(url, 128 * 1024, lyr_search_done)) { toast("Wi-Fi is busy. Try again in a moment"); return; }
    toast("Looking for lyrics online");
}
/* Wi-Fi connected: the search it was turned on for */
static void lyr_online(void)
{
    if (!lyr_wait_wifi) return;
    lyr_wait_wifi = false;
    struct mp3entry *id3 = audio_current_track();
    if (np_open && id3 && !strcmp(id3->path, lyr_found_for) && !lyr.n) lyr_search();
}
static void qp_wifi_toggle(void);
static void sl_wifi_page(void);
static void lyr_wifi_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    if (k == 0) return;                  /* not now */
    struct mp3entry *id3 = audio_current_track();
    strlcpy(lyr_found_for, id3 ? id3->path : "", sizeof lyr_found_for);
    lyr_wait_wifi = true;
    if (k == 1) { if (!global_settings.sloppy_wifi) qp_wifi_toggle(); toast("Lyrics will be looked up once Wi-Fi connects"); }
    else { np_hide(0); sl_wifi_page(); }
}
/* the sheet that asks: on, with a network it knows; or on the way to choosing one */
static void lyr_wifi_ask(void)
{
    acts_begin();
    lyr_sheet_head("Turn on Wi-Fi?", "Lyrics are looked up on LRCLIB, over Wi-Fi.");
    if (!global_settings.sloppy_wifi) acts_row_cb(LV_SYMBOL_WIFI, "Turn on Wi-Fi", lyr_wifi_cb, 1);
    else acts_row_cb(LV_SYMBOL_WIFI, "Choose a network", lyr_wifi_cb, 2);
    acts_row_cb(LV_SYMBOL_CLOSE, "Not now", lyr_wifi_cb, 0);
    acts_show();
}
static void lyr_fetch(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!lyr_can_search(id3)) return;
    if (!wf_online()) { lyr_wifi_ask(); return; }
    lyr_search();
}
static void lyr_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!lyr.n) { lyr_fetch(); return; }    /* the button offers a search when the card has none */
    lyr_want = !lyr_visible();
    lyr_show(lyr_want);
}
/* held: look again, and choose */
static void lyr_btn_long_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_wait_release(lv_indev_active());
    lyr_fetch();
}

/* The playing track changed (or Now Playing opened): load its lyrics. */
static void lyr_track(const char *path)
{
    if (!lyr_btn || !strcmp(path, lyr_path)) return;
    strlcpy(lyr_path, path, sizeof lyr_path);
    bool was_open = lyr_visible();
    if (was_open) lyr_show(false);
    lyr_free();
    bool have = lyr_read(path);
    if (have) sl_log("lyrics: %d %s lines", lyr.n, lyr.synced ? "timed" : "plain");
    /* no file: the button stays when Wi-Fi could find one */
    hidden_if_changed(lyr_btn, !have && !lyr_can_search(audio_current_track()));
    if (have && (was_open || lyr_want)) lyr_show(true);
}

static void lyr_build(lv_obj_t *parent)
{
    lyr_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(lyr_panel);
    lv_obj_set_size(lyr_panel, W, W);
    lv_obj_set_style_bg_color(lyr_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lyr_panel, 205, 0);
    lv_obj_remove_flag(lyr_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lyr_panel, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lyr_bd = lv_image_create(lyr_panel);
    lv_obj_add_flag(lyr_bd, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);

    lyr_list = lv_obj_create(lyr_panel);
    lv_obj_remove_style_all(lyr_list);
    lv_obj_set_size(lyr_list, W, LYR_LIST_H);
    lv_obj_set_pos(lyr_list, 0, LYR_LIST_TOP);
    /* the lines fade into the panel at its top and foot instead of being
     * cut: two strips over the list, the panel's black thinning to nothing */
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *fade = lyr_fade_box[k] = box(lyr_panel);
        lv_obj_set_size(fade, W, 40);
        lv_obj_set_pos(fade, 0, k ? W - 40 : LYR_LIST_TOP);
        lv_obj_set_style_bg_color(fade, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(fade, k ? 0 : 205, 0);
        lv_obj_set_style_bg_grad_opa(fade, k ? 205 : 0, 0);
        lv_obj_move_foreground(fade);
        lyr_fade_img[k] = lv_image_create(lyr_panel);
        lv_obj_add_flag(lyr_fade_img[k], LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(lyr_fade_img[k], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_pad_hor(lyr_list, 28, 0);
    /* room to bring the last line to the middle (lyr_fill sets the top) */
    lv_obj_set_style_pad_bottom(lyr_list, LYR_LIST_H / 2, 0);
    lv_obj_set_flex_flow(lyr_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(lyr_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lyr_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(lyr_list, LV_OBJ_FLAG_USER_1);          /* no back-swipe */
    lv_obj_add_event_cb(lyr_list, lyr_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(lyr_list, lyr_scroll_cb, LV_EVENT_SCROLL, NULL);

    lyr_btn = box(parent);
    lv_obj_set_size(lyr_btn, 104, 56);
    lv_obj_set_pos(lyr_btn, W - 112, 10);
    lv_obj_set_style_radius(lyr_btn, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(lyr_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lyr_btn, LV_OPA_30, 0);
    lv_obj_center(text(lyr_btn, "Lyrics", &ss_semibold_21, 0xffffff));
    lv_obj_set_style_text_color(lv_obj_get_child(lyr_btn, 0), lv_color_white(), 0);
    on_click(lyr_btn, lyr_btn_cb, NULL);
    lv_obj_add_event_cb(lyr_btn, lyr_btn_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_remove_flag(lyr_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN);
}
