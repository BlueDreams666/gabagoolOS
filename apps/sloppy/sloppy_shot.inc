/* Screenshots, on the interface's side. Three ways in: Power held and
 * Volume up pressed (or Volume up held and Power pressed, Sonix's order),
 * the control centre's Screenshot tile, and the phone remote's button.
 *
 * The screen is copied as it is (768 KB, about a millisecond) before
 * anything of this file is drawn; a white flash then fades over
 * SL_T_PAGE, the shutter sounds, and sloppy_png.c makes the PNG and writes
 * it to /Screenshots on its own low-priority thread, so the interface
 * never waits. When it is on the card a card rises where every toast
 * rises (sl_overlay_place): a thumbnail of the shot, "Screenshot saved",
 * and a tap opens it. Included by sloppy.c after the panel and the
 * notifications it uses. */
#include "sloppy_png.h"

#define SHOT_TH_H   48                   /* the card's thumbnail: the screen's shape */
#define SHOT_TH_W   (SHOT_TH_H * W / H)
#define SHOT_CARD_H (SHOT_TH_H + 2 * SL_SP_S)
#define SHOT_VIEW_W (W / 2)              /* the viewer's picture: half size, every pixel a 2 x 2 average */
#define SHOT_VIEW_H (H / 2)

static uint16_t shot_thumb_px[SHOT_TH_W * SHOT_TH_H];
static lv_image_dsc_t shot_thumb_dsc;
static char shot_last[256];              /* the last one saved, as Rockbox names it */
static lv_timer_t *shot_timer;
static unsigned shot_taken;              /* screenshots asked for, for the tests */
static struct
{
    lv_obj_t *obj, *thumb, *title, *body;
    lv_timer_t *timer;
    lv_point_t press;
    bool up, flicked;
    int8_t dir;
} sc;

/* the screen's own pixels, area-averaged into the thumbnail */
static void shot_thumb_make(const uint16_t *px)
{
    for (int y = 0; y < SHOT_TH_H; y++)
        for (int x = 0; x < SHOT_TH_W; x++)
        {
            int x0 = x * W / SHOT_TH_W, x1 = (x + 1) * W / SHOT_TH_W, y0 = y * H / SHOT_TH_H, y1 = (y + 1) * H / SHOT_TH_H;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += 2)          /* every other row and column: plenty at this size */
                for (int xx = x0; xx < x1; xx += 2)
                {
                    unsigned p = px[yy * W + xx];
                    r += p >> 11; g += (p >> 5) & 63; b += p & 31; n++;
                }
            shot_thumb_px[y * SHOT_TH_W + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n));
        }
    shot_thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    shot_thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    shot_thumb_dsc.header.w = SHOT_TH_W;
    shot_thumb_dsc.header.h = SHOT_TH_H;
    shot_thumb_dsc.header.stride = SHOT_TH_W * 2;
    shot_thumb_dsc.data = (const uint8_t *)shot_thumb_px;
    shot_thumb_dsc.data_size = sizeof shot_thumb_px;
}

/* ------------------------------------------------------------ the flash */
static void shot_flash_gone(lv_anim_t *a) { lv_obj_delete((lv_obj_t *)a->var); }
static void shot_flash_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void shot_flash(void)
{
    lv_obj_t *f = box(lv_layer_top());
    lv_obj_set_size(f, W, H);
    lv_obj_set_style_bg_color(f, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_60, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, f);
    lv_anim_set_exec_cb(&a, shot_flash_opa);
    lv_anim_set_values(&a, LV_OPA_60, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, shot_flash_gone);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------- the card */
static void shot_view_page(const char *path);
static void shot_card_hide(void);
static void shot_card_gone(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(sc.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(sc.obj, 0, 0);
}
static void shot_card_timer_cb(lv_timer_t *t) { (void)t; if (!po_is_open()) shot_card_hide(); }
static void shot_card_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) { sc.press = p; sc.flicked = false; }
    else if (c == LV_EVENT_PRESSING && !sc.flicked && (p.y - sc.press.y) * sc.dir > 12) { sc.flicked = true; shot_card_hide(); }
    else if (c == LV_EVENT_CLICKED && !sc.flicked)
    {
        shot_card_hide();
        if (shot_last[0]) shot_view_page(shot_last);
    }
}
static void shot_card_build(void)
{
    sc.obj = box(lv_layer_top());
    lv_obj_set_size(sc.obj, LV_SIZE_CONTENT, SHOT_CARD_H);
    lv_obj_set_style_pad_all(sc.obj, SL_SP_S, 0);
    lv_obj_set_style_pad_right(sc.obj, SL_INSET, 0);
    lv_obj_set_style_pad_column(sc.obj, SL_GUTTER - 2, 0);
    lv_obj_set_flex_flow(sc.obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc.obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(sc.obj, 248, 0);
    lv_obj_set_style_bg_grad_dir(sc.obj, LV_GRAD_DIR_VER, 0);
    sl_elevate(sc.obj, SL_E3, 0);   /* a banner, as the notifications' */
    lv_obj_add_flag(sc.obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(sc.obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(sc.obj, shot_card_event, LV_EVENT_ALL, NULL);
    /* the thumbnail, its corners concentric with the card's, a hairline
     * so a dark screen still has an edge on the dark card */
    lv_obj_t *frame = box(sc.obj);
    lv_obj_set_size(frame, SHOT_TH_W, SHOT_TH_H);
    lv_obj_set_style_radius(frame, SL_R_SM - SL_SP_S, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_white(), 0);
    lv_obj_set_style_border_opa(frame, 60, 0);
    lv_obj_set_style_border_post(frame, true, 0);
    sc.thumb = lv_image_create(frame);
    lv_obj_remove_flag(sc.thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *words = box(sc.obj);
    lv_obj_set_size(words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    sc.title = text(words, "Screenshot saved", &ss_semibold_18, 0xf7f8fc);
    lv_obj_set_style_text_color(sc.title, lv_color_hex(0xf7f8fc), 0);   /* white in every theme, as a toast */
    sc.body = text(words, "Tap to view", &ss_regular_16, 0xb9bfcc);
    lv_obj_set_style_text_color(sc.body, lv_color_hex(0xb9bfcc), 0);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = i ? sc.body : sc.title;
        lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
}
/* the toast's look: dark candy glass with the accent's glow, or Zule's
 * black band with its pink edge */
static void shot_card_style(void)
{
    bool zn = sl_zune();
    lv_obj_set_style_radius(sc.obj, zn ? 0 : SL_R_SM, 0);
    lv_obj_set_style_bg_color(sc.obj, lv_color_hex(zn ? 0x000000 : 0x1b1d26), 0);
    lv_obj_set_style_bg_grad_color(sc.obj, lv_color_hex(zn ? 0x000000 : 0x262935), 0);
    lv_obj_set_style_border_side(sc.obj, zn ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_width(sc.obj, zn ? 4 : 1, 0);
    lv_obj_set_style_border_color(sc.obj, lv_color_hex(zn ? ZN_PINK : C_ACCENT), 0);
    lv_obj_set_style_border_opa(sc.obj, zn ? LV_OPA_COVER : 110, 0);
    lv_obj_set_style_shadow_color(sc.obj, lv_color_hex(zn ? 0x000000 : C_ACCENT), 0);
    lv_obj_set_style_radius(lv_obj_get_parent(sc.thumb), zn ? 0 : SL_R_SM - SL_SP_S, 0);
    lv_obj_set_style_base_dir(sc.obj, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
}
static void shot_card_show(void)
{
    if (!sc.obj) shot_card_build();
    pop_hide_now();                      /* one thing at the toast's place */
    shot_card_style();
    /* the words' own width, as the toast measures its own, held to what fits beside the thumbnail */
    const char *t = sl_tr("Screenshot saved"), *b = sl_tr("Tap to view");
    lv_point_t st, sb;
    lv_text_get_size(&st, t, &ss_semibold_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&sb, b, &ss_regular_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = LV_MIN(LV_MAX(st.x, sb.x), W - 2 * SL_GUTTER - SL_SP_S - SHOT_TH_W - (SL_GUTTER - 2) - SL_INSET);
    lv_obj_set_width(sc.title, w);
    lv_obj_set_width(sc.body, w);
    lv_label_set_text(sc.title, t);
    lv_label_set_text(sc.body, b);
    lv_image_set_src(sc.thumb, &shot_thumb_dsc);
    lv_obj_remove_flag(sc.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sc.obj);
    lv_anim_delete(sc.obj, NULL);
    sc.dir = (int8_t)sl_overlay_place(sc.obj, 10);   /* never over a control */
    /* in from its edge, past its place, and settles: the toast's own bounce */
    pop_anim(sc.obj, pop_set_y, 76 * sc.dir, 0, 460, 0, lv_anim_path_overshoot, NULL);
    pop_anim(sc.obj, pop_set_opa, LV_OPA_TRANSP, LV_OPA_COVER, 160, 0, lv_anim_path_ease_out, NULL);
    sc.up = true;
    /* as long as a failure's toast stays: time to reach for it */
    if (!sc.timer) sc.timer = lv_timer_create(shot_card_timer_cb, POP_WARN_MS, NULL);
    lv_timer_reset(sc.timer);
    lv_timer_resume(sc.timer);
}
static void shot_card_hide(void)
{
    if (!sc.obj || !sc.up) return;
    sc.up = false;
    if (sc.timer) lv_timer_pause(sc.timer);
    lv_anim_delete(sc.obj, NULL);
    pop_anim(sc.obj, pop_set_y, lv_obj_get_style_translate_y(sc.obj, 0), 76 * sc.dir, 260, 0, lv_anim_path_ease_in, NULL);
    pop_anim(sc.obj, pop_set_opa, lv_obj_get_style_opa(sc.obj, 0), LV_OPA_TRANSP, 260, 0, lv_anim_path_ease_in, shot_card_gone);
}

/* ------------------------------------------------------------- taking one */
static void shot_poll_cb(lv_timer_t *t)
{
    char path[256];
    int s = sl_shot_poll(path, sizeof path);
    if (s == SL_SHOT_BUSY || s == SL_SHOT_IDLE) return;
    lv_timer_pause(t);
    if (s == SL_SHOT_SAVED)
    {
        strlcpy(shot_last, path, sizeof shot_last);
        sl_log("screenshot saved: %s", path);
        shot_card_show();
    }
    else
    {
        sl_log("screenshot not saved: %s", s == SL_SHOT_FULL ? "card full" : "write failed");
        toast_warn(s == SL_SHOT_FULL ? "Card full. Screenshot not saved." : "Screenshot not saved");
    }
}
static void sl_screenshot(void)
{
    shot_taken++;
    if (log_held) { toast_warn("Card busy. Screenshot not saved."); return; }   /* USB mode: the computer has it */
    if (sl_shot_busy()) { toast("Still saving the last screenshot"); return; }
    uint16_t *px = malloc((size_t)W * H * 2);
    if (!px) { toast_warn("Screenshot not saved"); return; }
    if (sc.up)                           /* the last one's card is ours, not the page's: out of the shot */
    {
        sc.up = false;
        if (sc.timer) lv_timer_pause(sc.timer);
        lv_anim_delete(sc.obj, NULL);
        lv_obj_add_flag(sc.obj, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(disp);
    }
    bool veil = sl_night_veil && !lv_obj_has_flag(sl_night_veil, LV_OBJ_FLAG_HIDDEN);
    if (veil)                            /* the night veil is the room's, not the picture's */
    {
        lv_obj_add_flag(sl_night_veil, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(disp);
    }
    bool corners = sl_corners_shown();   /* and the rounded corners the panel's */
    if (corners)
    {
        sl_corners_show(false);
        lv_refr_now(disp);
    }
    const uint16_t *screen = SL_SCREEN_PIXELS();
    for (int y = 0; y < H; y++) memcpy(&px[y * W], &screen[y * SL_SCREEN_STRIDE], W * 2);
    if (veil) lv_obj_remove_flag(sl_night_veil, LV_OBJ_FLAG_HIDDEN);
    if (corners) sl_corners_show(true);
    shot_thumb_make(px);
    char name[64];
    struct tm *tm = get_time();
    snprintf(name, sizeof name, "gabagoolOS %04d-%02d-%02d at %02d.%02d.%02d", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (!sl_shot_start(px, W, H, name)) { toast_warn("Screenshot not saved"); return; }
    shot_card_hide();
    shot_flash();
    sfx_play(SFX_UI_SHUTTER);
    sl_log("screenshot taken");
    if (!shot_timer) shot_timer = lv_timer_create(shot_poll_cb, 100, NULL);
    lv_timer_reset(shot_timer);
    lv_timer_resume(shot_timer);
}
/* the control centre's tile: the panel goes first, with its own motion;
 * the shot waits for the page under it to be drawn again */
static void shot_after_panel(void)
{
    lv_refr_now(disp);
    lv_refr_now(disp);
    sl_screenshot();
}

/* ------------------------------------------------------------ the viewer */
/* Each viewer keeps its own picture: two can be open at once (the card
 * tapped twice, or a shot taken over a viewer), and closing the top one
 * must not take the pixels the one under it draws */
struct shot_view { uint16_t *px; lv_image_dsc_t dsc; char path[256]; };
static uint16_t *shot_view_px;          /* the newest viewer's, NULL with none open */
static char shot_view_path[256];        /* the newest viewer's picture */
static void shot_view_deleted(lv_event_t *e)
{
    struct shot_view *sv = lv_event_get_user_data(e);
    if (shot_view_px == sv->px) shot_view_px = NULL;
    free(sv->px);
    free(sv);
}
static void shot_delete_cb(lv_event_t *e)
{
    const struct shot_view *sv = e ? lv_event_get_user_data(e) : NULL;
    const char *path = sv ? sv->path : shot_view_path;
    if (!sl_shot_delete(path)) { toast_warn("Could not delete the screenshot"); return; }
    sl_log("screenshot deleted: %s", path);
    if (!strcmp(path, shot_last)) shot_last[0] = '\0';
    page_pop();
    toast("Screenshot deleted");
}
static void shot_view_page(const char *path)
{
    uint16_t *px;
    int w, h;
    struct shot_view *sv = calloc(1, sizeof *sv);
    if (!sv || !sl_png_load565(path, SHOT_VIEW_W, SHOT_VIEW_H, &px, &w, &h)) { free(sv); toast_warn("Could not open the screenshot"); return; }
    strlcpy(sv->path, path, sizeof sv->path);
    strlcpy(shot_view_path, path, sizeof shot_view_path);
    lv_obj_t *p, *body = page_new("Screenshot", &p);
    sv->px = shot_view_px = px;
    lv_obj_add_event_cb(p, shot_view_deleted, LV_EVENT_DELETE, sv);
    sv->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    sv->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    sv->dsc.header.w = (uint32_t)w;
    sv->dsc.header.h = (uint32_t)h;
    sv->dsc.header.stride = (uint32_t)w * 2;
    sv->dsc.data = (const uint8_t *)px;
    sv->dsc.data_size = (uint32_t)(w * h * 2);
    /* the picture floats over the page as a panel does: rounded, a hairline, its shadow */
    lv_obj_t *wrap = box(body);                   /* centres it in the page's column */
    lv_obj_set_size(wrap, lv_pct(100), h + SL_SP_S + SL_GUTTER);
    lv_obj_t *frame = box(wrap);
    lv_obj_set_size(frame, w, h);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, SL_SP_S);
    lv_obj_set_style_radius(frame, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    sl_elevate(frame, SL_E3, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(frame, 60, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_post(frame, true, 0);
    lv_obj_t *img = lv_image_create(frame);
    lv_image_set_src(img, &sv->dsc);
    lv_obj_center(img);
    /* its name says when it was taken: that, without the prefix and the extension */
    char when[96];
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    if (!strncmp(file, "gabagoolOS ", 11)) file += 11;
    strlcpy(when, file, sizeof when);
    char *dot = strrchr(when, '.');
    if (dot && !strcmp(dot, ".png")) *dot = '\0';
    lv_obj_t *g = set_group(body, NULL);
    set_info(g, LV_SYMBOL_IMAGE, 0x8b6fd6, when, "In Screenshots, at the top of the card", true);
    set_action_sub(g, LV_SYMBOL_IMAGE, 0x3d8fd6, "Set as wallpaper", "Home screen, lock screen or both", wallpaper_set_cb, sv->path);
    set_danger(g, LV_SYMBOL_TRASH, "Delete", shot_delete_cb, sv);
    page_push(p);
}
