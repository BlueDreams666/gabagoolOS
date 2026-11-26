/* Clicker: a tally counter. GPLv2+.
 *
 * One big number on a calm page with a soft accent glow behind it. A tap
 * anywhere counts one, and so does either volume key, which stops changing
 * the volume while the page is open; the keys keep counting with the
 * player locked or dark, without lighting it. Plus and minus sit at the
 * bottom, a red Reset at the top asks first. Each count pops the number
 * and pulses the glow; that is the only motion.
 *
 * The count is saved to clicker.txt ("SLK1 n") a moment after the last
 * change, when the page closes, and before USB mode or power off. */

LV_FONT_DECLARE(sl_count_184)
LV_FONT_DECLARE(sl_count_136)

#define CK_FILE ROCKBOX_DIR "/sloppy/clicker.txt"
#define CK_MAX 9999999u                     /* seven digits */
#define CK_SAVE_MS 800                      /* quiet this long: saved */
#define CK_BODY_Y (SB_H + 66)               /* the tap surface, under the header */
#define CK_NUM_Y 360                        /* screen y of the number's centre */
#define CK_GLOW 400                         /* the glow's diameter */
#define CK_BLOOM 320                        /* and the brighter middle a count lights */
#define CK_BTN 84                           /* the round plus and minus */
#define CK_BTN_Y (H - 76 - CK_BTN)
#define CK_SHEET_H 252
#define CK_RED (sl_theme() == 1 ? 0xd93036 : 0xe5484d)
/* the round controls and the sheet: a surface that shows on every theme,
 * including true black */
#define CK_FILL (sl_theme() == 1 ? 0xffffff : sl_theme() == 2 ? 0x1c1c1e : 0x1b2430)
#define CK_FILL_P (sl_theme() == 1 ? 0xdfe6ed : sl_theme() == 2 ? 0x2c2c2e : 0x2a3544)

static uint32_t clicker_count;
static bool clicker_loaded;
static lv_obj_t *clicker_page_obj, *clicker_box, *clicker_num, *clicker_glow, *clicker_bloom, *clicker_reset;
static lv_obj_t *clicker_scrim, *clicker_sheet;
static lv_timer_t *clicker_save_timer;
static bool clicker_dirty;
static uint16_t *clicker_glow_px;          /* the glow at rest, then brighter */
static lv_image_dsc_t clicker_glow_dsc[2];

static void clicker_load(void)
{
    if (clicker_loaded) return;
    clicker_loaded = true;
    char b[32];
    int fd = open(CK_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 6 || strncmp(b, "SLK1 ", 5)) return;
    b[got] = 0;
    unsigned long n = strtoul(b + 5, NULL, 10);
    clicker_count = n > CK_MAX ? CK_MAX : (uint32_t)n;
}
static void clicker_save(void)
{
    char b[32];
    int len = snprintf(b, sizeof b, "SLK1 %lu\n", (unsigned long)clicker_count);
    clicker_dirty = false;
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(CK_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;   /* a power cut must not leave an empty file */
    if (close(fd) < 0) ok = false;
    if (!ok || rename(CK_FILE ".tmp", CK_FILE) < 0) remove(CK_FILE ".tmp");
}
/* before USB mode and power off: a count still waiting is written now */
static void clicker_flush(void)
{
    if (clicker_dirty) clicker_save();
    if (clicker_save_timer) lv_timer_pause(clicker_save_timer);
}
static void clicker_save_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (clicker_dirty) clicker_save();
}

static bool clicker_on_top(void)
{
    return clicker_page_obj && npages && pages[npages - 1] == clicker_page_obj;
}

/* The digits get smaller only when they must: four at full size, then six,
 * then seven. The box holds just the digits' height, so the pop scales a
 * small layer, not the font's tall line. */
static void clicker_show(void)
{
    char b[12];
    snprintf(b, sizeof b, "%lu", (unsigned long)clicker_count);
    size_t n = strlen(b);
    const lv_font_t *f = n <= 4 ? &sl_count_184 : n <= 6 ? &sl_count_136 : &ss_regular_104;
    lv_font_glyph_dsc_t g;
    lv_font_get_glyph_dsc(f, &g, '0', 0);
    int lh = lv_font_get_line_height(f), top = lh - f->base_line - g.ofs_y - g.box_h;
    int bw = lv_text_get_width(b, n, f, 0) + 24, bh = g.box_h + 16;   /* as small as the digits */
    lv_label_set_text(clicker_num, b);
    lv_obj_set_style_text_font(clicker_num, f, 0);
    lv_obj_set_size(clicker_num, bw, lh);
    lv_obj_set_pos(clicker_num, 0, 8 - top);
    lv_obj_set_size(clicker_box, bw, bh);
    lv_obj_set_pos(clicker_box, (W - bw) / 2, CK_NUM_Y - CK_BODY_Y - bh / 2);
    lv_obj_set_style_transform_pivot_x(clicker_box, bw / 2, 0);
    lv_obj_set_style_transform_pivot_y(clicker_box, bh / 2, 0);
    if (clicker_reset)
    {
        lv_obj_set_style_opa(clicker_reset, clicker_count ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_state(clicker_reset, LV_STATE_DISABLED, !clicker_count);   /* nothing to reset: inactive, and says so */
    }
}

static void clicker_scale_cb(void *o, int32_t v) { lv_obj_set_style_transform_scale(o, v, 0); }
static void clicker_glow_cb(void *o, int32_t v) { lv_obj_set_style_image_opa(o, (lv_opa_t)v, 0); }
static void clicker_anim(void *o, lv_anim_exec_xcb_t cb, int from, int to, int ms, lv_anim_path_cb_t path)
{
    lv_anim_delete(o, cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

/* by one (or back to zero): the number, the save, the sound and the pop */
static void clicker_add(int d, bool key)
{
    uint32_t was = clicker_count;
    if (d > 0 && clicker_count < CK_MAX) clicker_count++;
    else if (d < 0 && clicker_count) clicker_count--;
    else if (d == 0) clicker_count = 0;
    bool hidden = sl_power_hides_ui();
    /* locked or dark, a key is a quiet interface tick (Interface sounds) */
    if (d == 0) sfx_play(SFX_UI_REMOVE);
    else if (hidden && key) sfx_play(SFX_UI_TICK);
    else if (d > 0) sfx_play(SFX_TAP);
    else sfx_play_ex(SFX_TAP, 72, 80);    /* minus: lower and softer */
    if (clicker_count == was && d != 0) return;
    clicker_dirty = true;
    if (d == 0) clicker_save();
    else if (clicker_save_timer)
    {
        lv_timer_reset(clicker_save_timer);
        lv_timer_resume(clicker_save_timer);
    }
    if (!clicker_num) return;
    clicker_show();
    if (hidden)
    {
        /* nothing is seen: no motion, just the new number for later */
        lv_anim_delete(clicker_box, clicker_scale_cb);
        lv_anim_delete(clicker_glow, clicker_glow_cb);
        lv_anim_delete(clicker_bloom, clicker_glow_cb);
        lv_obj_set_style_transform_scale(clicker_box, 256, 0);
        lv_obj_set_style_image_opa(clicker_glow, LV_OPA_COVER, 0);
        lv_obj_set_style_image_opa(clicker_bloom, LV_OPA_TRANSP, 0);
        return;
    }
    /* up pops out and the glow blooms; down dips in and the glow dims;
     * zero shrinks it and the glow comes back from nothing */
    clicker_anim(clicker_box, clicker_scale_cb, d > 0 ? 292 : d < 0 ? 228 : 200, 256,
                 d == 0 ? 420 : 300, lv_anim_path_overshoot);
    if (d > 0) clicker_anim(clicker_bloom, clicker_glow_cb, LV_OPA_COVER, LV_OPA_TRANSP, 520, lv_anim_path_ease_out);
    else
    {
        lv_anim_delete(clicker_bloom, clicker_glow_cb);
        lv_obj_set_style_image_opa(clicker_bloom, LV_OPA_TRANSP, 0);
        clicker_anim(clicker_glow, clicker_glow_cb, d < 0 ? 110 : 0, LV_OPA_COVER, d < 0 ? 420 : 700,
                     lv_anim_path_ease_out);
    }
}

/* Either volume key counts one while the page is on top, locked and dark
 * included; nothing reaches the volume. sloppy.c asks before the lock
 * screen's key filter, which would wake a dark screen to the lock screen.
 * A key still keeps a lit screen awake and lifts a dim one, as any key. */
static bool clicker_key(long btn)
{
    int key = btn & ~(BUTTON_REL | BUTTON_REPEAT);
    if ((key != BUTTON_UP && key != BUTTON_DOWN) || !clicker_on_top()) return false;
    sl_vol_keys_seen(btn);                 /* its volume keys still count as let go (Power's screenshot) */
    if (sl_power_running && !sl_power_suspended)
    {
        sl_power.ignored_keys &= ~(uint32_t)key;
        if (sl_power.mode == SL_POWER_DIM) sl_power_state_set(&sl_power, SL_POWER_ACTIVE, sl_ms());
        else if (sl_power.mode != SL_POWER_DARK) sl_power.activity_at = sl_ms();
        sl_power_apply();
    }
    if (!(btn & (BUTTON_REL | BUTTON_REPEAT))) clicker_add(1, true);   /* a press, not a hold */
    return true;
}

static void clicker_tap_cb(lv_event_t *e) { (void)e; clicker_add(1, false); }
static void clicker_minus_cb(lv_event_t *e) { (void)e; clicker_add(-1, false); }

/* ---- Reset: a sheet that asks first ------------------------------------ */
static void clicker_sheet_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void clicker_sheet_show(bool in)
{
    if (in) lv_obj_remove_flag(clicker_scrim, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(clicker_scrim, LV_OBJ_FLAG_HIDDEN);
    clicker_anim(clicker_sheet, clicker_sheet_y, lv_obj_get_y(clicker_sheet), in ? H - CK_SHEET_H : H + 20,
                 in ? 300 : 220, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
}
static bool clicker_sheet_open(void) { return clicker_scrim && !lv_obj_has_flag(clicker_scrim, LV_OBJ_FLAG_HIDDEN); }
static void clicker_reset_cb(lv_event_t *e)
{
    (void)e;
    if (!clicker_count || clicker_sheet_open()) return;
    clicker_sheet_show(true);
    sfx_play(SFX_UI_PANEL_OPEN);
}
static void clicker_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (!clicker_sheet_open()) return;
    clicker_sheet_show(false);
    sfx_play(SFX_UI_PANEL_CLOSE);
}
static void clicker_clear_cb(lv_event_t *e)
{
    (void)e;
    if (!clicker_sheet_open()) return;
    clicker_sheet_show(false);
    clicker_add(0, false);
}
static lv_obj_t *clicker_pill(lv_obj_t *parent, const char *s, uint32_t bg, uint32_t bg_p, uint32_t ink, int y,
                              lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, W - 48, 58);
    lv_obj_set_pos(b, 24, y);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg_p), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *l = text(b, s, &ss_semibold_23, ink);
    lv_obj_center(l);
    on_click(b, cb, NULL);
    return b;
}
static void clicker_build_sheet(lv_obj_t *p)
{
    clicker_scrim = box(p);
    lv_obj_set_size(clicker_scrim, W, H);
    lv_obj_set_style_bg_color(clicker_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(clicker_scrim, LV_OPA_40, 0);
    lv_obj_remove_flag(clicker_scrim, LV_OBJ_FLAG_EVENT_BUBBLE);
    on_click(clicker_scrim, clicker_cancel_cb, NULL);
    lv_obj_add_flag(clicker_scrim, LV_OBJ_FLAG_HIDDEN);

    clicker_sheet = box(p);
    lv_obj_set_size(clicker_sheet, W, CK_SHEET_H + 30);
    lv_obj_set_pos(clicker_sheet, 0, H + 20);
    lv_obj_set_style_radius(clicker_sheet, SL_R_XL, 0);
    lv_obj_set_style_bg_color(clicker_sheet, lv_color_hex(CK_FILL), 0);
    lv_obj_set_style_bg_opa(clicker_sheet, LV_OPA_COVER, 0);
    sl_elevate(clicker_sheet, SL_E4, 0);
    lv_obj_remove_flag(clicker_sheet, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(clicker_sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* taps stop here */
    lv_obj_t *grab = box(clicker_sheet);
    lv_obj_set_size(grab, 44, 5);
    lv_obj_set_pos(grab, (W - 44) / 2, 9);
    lv_obj_set_style_radius(grab, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(grab, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_t *t = text(clicker_sheet, "Reset to zero?", &ss_semibold_26, C_TEXT);
    lv_obj_set_width(t, W - 48);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 24, 30);
    clicker_pill(clicker_sheet, "Reset", CK_RED, sl_theme() == 1 ? 0xb4232a : 0xc73a3f, 0xfffffe, 90, clicker_clear_cb);
    clicker_pill(clicker_sheet, "Cancel", CK_FILL_P, sl_theme() == 1 ? 0xcfd8e1 : 0x3a3a3e, C_TEXT, 160,
                 clicker_cancel_cb);
}

/* ---- the page ------------------------------------------------------------ */
/* A soft round glow in the accent, painted once over the page colour as
 * two RGB565 pictures: the glow at rest, and a smaller bloom, the same
 * glow with a brighter middle, that a count fades out of. The page is flat
 * behind them, so they are opaque copies, and an ordered dither keeps the
 * fall-off from banding (a tinted alpha map showed rings). */
static int clicker_falloff(int i, int j, int size)           /* (1 - d^2)^2, of 256 */
{
    int dx = 2 * i + 1 - size, dy = 2 * j + 1 - size;
    int d2 = (dx * dx + dy * dy) * 256 / (size * size);
    return d2 >= 256 ? 0 : (256 - d2) * (256 - d2) / 256;
}
static bool clicker_glow_paint(void)
{
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    static const int size[2] = { CK_GLOW, CK_BLOOM }, off = (CK_GLOW - CK_BLOOM) / 2;   /* a multiple of 4: one dither */
    clicker_glow_px = malloc((CK_GLOW * CK_GLOW + CK_BLOOM * CK_BLOOM) * sizeof(uint16_t));
    if (!clicker_glow_px) return false;
    uint32_t bg = C_BG, acc = sl_accent();
    bool light = sl_theme() == 1;
    int rest = light ? 60 : 80, bloom = light ? 90 : 135;   /* of 256 */
    uint16_t *px = clicker_glow_px;
    for (int k = 0; k < 2; k++)
    {
        for (int j = 0; j < size[k]; j++)
            for (int i = 0; i < size[k]; i++)
            {
                int w = k ? clicker_falloff(i + off, j + off, CK_GLOW) * rest / 256 +
                            clicker_falloff(i, j, CK_BLOOM) * bloom / 256
                          : clicker_falloff(i, j, CK_GLOW) * rest / 256;
                int b = bayer[(j & 3) * 4 + (i & 3)], c[3];
                for (int n = 0; n < 3; n++)
                {
                    /* from the page colour as LVGL draws it (cut to 5 or 6
                     * bits), so the edges meet the page without a seam */
                    int from = bg >> (16 - 8 * n) & 255, to = acc >> (16 - 8 * n) & 255;
                    c[n] = (from & (n == 1 ? 0xfc : 0xf8)) * 256 + (to - from) * w;   /* x 256 */
                    c[n] = (c[n] + b * (n == 1 ? 64 : 128)) >> (n == 1 ? 10 : 11);
                    if (c[n] < 0) c[n] = 0;
                    if (c[n] > (n == 1 ? 63 : 31)) c[n] = n == 1 ? 63 : 31;
                }
                *px++ = (uint16_t)(c[0] << 11 | c[1] << 5 | c[2]);
            }
        clicker_glow_dsc[k] = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                        .w = size[k], .h = size[k], .stride = size[k] * 2 },
            .data_size = size[k] * size[k] * 2, .data = (const uint8_t *)(px - size[k] * size[k]) };
    }
    return true;
}

/* a round button with a plus or minus drawn in bars */
static lv_obj_t *clicker_button(lv_obj_t *p, int cx, bool plus, uint32_t bg, uint32_t bg_p, uint32_t ink,
                                lv_event_cb_t cb)
{
    lv_obj_t *b = box(p);
    lv_obj_set_size(b, CK_BTN, CK_BTN);
    lv_obj_set_pos(b, cx - CK_BTN / 2, CK_BTN_Y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg_p), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(b, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, CK_BTN / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, CK_BTN / 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    press_motion(b);
    for (int k = 0; k < (plus ? 2 : 1); k++)
    {
        lv_obj_t *bar = box(b);
        lv_obj_set_size(bar, k ? 4 : 30, k ? 30 : 4);
        lv_obj_set_style_radius(bar, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(ink), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_center(bar);
    }
    on_click(b, cb, NULL);
    return b;
}

static void clicker_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != clicker_page_obj) return;
    clicker_flush();
    if (clicker_save_timer) { lv_timer_delete(clicker_save_timer); clicker_save_timer = NULL; }
    clicker_page_obj = clicker_box = clicker_num = clicker_glow = clicker_bloom = clicker_reset = NULL;
    clicker_scrim = clicker_sheet = NULL;
    for (int k = 0; k < 2; k++) lv_image_cache_drop(&clicker_glow_dsc[k]);   /* the next page paints anew */
    free(clicker_glow_px);
    clicker_glow_px = NULL;
#ifdef BUTTON_WAKE_SWALLOWS
    button_set_wake_swallows(!global_settings.sloppy_keys_dark);
#endif
}

static void clicker_page(void)
{
    if (page_bring_back(clicker_page_obj)) return;
    clicker_load();
    lv_obj_t *p, *body = page_new("Clicker", &p);
    clicker_page_obj = p;
    lv_obj_add_event_cb(p, clicker_deleted, LV_EVENT_DELETE, NULL);
    /* nothing but the back arrow up top: the number says what this is */
    lv_obj_t *hdr = lv_obj_get_child(p, 0);
    lv_obj_delete(lv_obj_get_child(hdr, -1));

    /* the body is the tap surface, from under the header to the bottom */
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    on_click(body, clicker_tap_cb, NULL);
    bool glow = clicker_glow_paint();
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *g = glow ? lv_image_create(body) : box(body);   /* no memory for it: no glow */
        if (glow) lv_image_set_src(g, &clicker_glow_dsc[k]);
        int d = k ? CK_BLOOM : CK_GLOW;
        lv_obj_set_pos(g, (W - d) / 2, CK_NUM_Y - CK_BODY_Y - d / 2);
        lv_obj_set_style_image_opa(g, k ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
        if (k) clicker_bloom = g;
        else clicker_glow = g;
    }
    clicker_box = box(body);
    clicker_num = lv_label_create(clicker_box);
    lv_obj_set_style_text_color(clicker_num, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(clicker_num, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_base_dir(clicker_num, LV_BASE_DIR_LTR, 0);   /* digits read left to right everywhere */
    lv_label_set_long_mode(clicker_num, LV_LABEL_LONG_CLIP);

    /* Reset: small and red, top right */
    clicker_reset = box(hdr);
    lv_obj_set_size(clicker_reset, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(clicker_reset, 18, 0);
    lv_obj_set_style_radius(clicker_reset, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(clicker_reset, lv_color_hex(CK_RED), 0);
    lv_obj_set_style_bg_opa(clicker_reset, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(clicker_reset, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_base_dir(clicker_reset, LV_BASE_DIR_LTR, 0);   /* stays top right in RTL */
    lv_obj_set_ext_click_area(clicker_reset, (SL_TAP - 40) / 2);   /* a finger-sized tap */
    lv_obj_t *rl = text(clicker_reset, "Reset", &ss_semibold_18, CK_RED);
    lv_obj_center(rl);
    lv_obj_align(clicker_reset, LV_ALIGN_RIGHT_MID, -16, 0);
    on_click(clicker_reset, clicker_reset_cb, NULL);

    uint32_t acc = sl_accent();
    clicker_button(p, W / 2 - 78, false, CK_FILL, CK_FILL_P, C_TEXT, clicker_minus_cb);
    lv_obj_t *plus = clicker_button(p, W / 2 + 78, true, acc, lv_color_to_u32(lv_color_darken(lv_color_hex(acc), 40)) & 0xffffff, sl_accent_ink(), clicker_tap_cb);
    if (sl_theme() != 1)                   /* a soft light under it, in the dark */
    {
        lv_obj_set_style_shadow_color(plus, lv_color_hex(acc), 0);
        lv_obj_set_style_shadow_width(plus, 26, 0);
        lv_obj_set_style_shadow_offset_y(plus, 6, 0);
        lv_obj_set_style_shadow_opa(plus, LV_OPA_30, 0);
    }
    clicker_build_sheet(p);
    clicker_show();

    clicker_save_timer = lv_timer_create(clicker_save_cb, CK_SAVE_MS, NULL);
    lv_timer_pause(clicker_save_timer);
#ifdef BUTTON_WAKE_SWALLOWS
    /* a key on a dark screen must count, not only wake it */
    button_set_wake_swallows(false);
#endif
    page_push(p);
}

/* The tile's back face: the count. */
static bool clicker_tile_back(struct mt_slot *s)
{
    lv_obj_t *b = s->back;
    char n[12];
    clicker_load();
    snprintf(n, sizeof n, "%lu", (unsigned long)clicker_count);
    lv_obj_clean(b);
    if (s->size == SZ_SMALL)
    {
        lv_obj_t *l = mt_label(b, n, strlen(n) <= 4 ? &ss_regular_34 : &ss_semibold_18, LV_OPA_COVER);
        lv_obj_set_style_base_dir(l, LV_BASE_DIR_LTR, 0);
        lv_obj_center(l);
        return true;
    }
    lv_obj_t *l = mt_label(b, n, &ss_regular_60, LV_OPA_COVER);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_LTR, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, -14);
    mt_title(b, mt_defs[s->id].name);
    return true;
}
