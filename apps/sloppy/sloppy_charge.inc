/*
 *  Plugging in: a drop of black glass swells out of the USB port at the
 *  bottom edge, rises with a lime bolt inside it, wobbles to a stop like
 *  something liquid, shows the charge, and sinks back into the port.
 *
 *  One animation drives it all (cb_step, milliseconds since the plug):
 *  the drop rises on a damped spring, stretches tall while it moves fast
 *  and squashes when it stops; a smaller droplet trails it out of the port
 *  and is swallowed. Translation, opacity and one scaled object, so it
 *  stays cheap on the R1's software renderer. Touches pass through it.
 */

#define CB_D        156                  /* the drop's diameter */
#define CB_REST     (H - 196)            /* its centre once risen */
#define CB_LIME     0x7cff4f
#define CB_RISE_MS  620
#define CB_SINK_AT  1220
#define CB_END_MS   1700

static struct
{
    lv_obj_t *root, *scrim, *drop, *trail, *bolt, *pct;
    int level, rest;                     /* the drop's centre once risen */
} cb;

/* a damped spring let go from rest: 0 to 1, over by about 6% a little
 * past half way, settled by the end (exp, not expf: newer than the R1's
 * glibc) */
static float cb_spring(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return 1 - (float)exp(-5.0 * t) * (cosf(5.5f * t) + 0.909f * sinf(5.5f * t));
}

static void cb_gone(lv_anim_t *a)
{
    (void)a;
    lv_obj_delete(cb.root);
    cb.root = NULL;
}

static void cb_step(lv_anim_t *a, int32_t ms)
{
    (void)a;
    const int from = H + CB_D / 2 + 8;   /* below the edge, in the port */
    float y, stretch;                    /* centre, and how tall (+) or wide (-) */
    if (ms < CB_SINK_AT)
    {
        float t = ms / (float)CB_RISE_MS, p = cb_spring(t);
        float v = (cb_spring(t + 0.02f) - p) / 0.02f;   /* speed, in travels per rise */
        y = from + (cb.rest - from) * p;
        if (ms >= CB_RISE_MS)
        {
            /* risen: a slow bob, as if afloat */
            y += 3.0f * sinf((ms - CB_RISE_MS) / 260.0f);
            v = 0;
        }
        stretch = v > 0 ? v * 0.10f : v * 0.06f;
        if (ms > CB_RISE_MS * 4 / 10 && ms < CB_RISE_MS * 16 / 10)   /* the wobble after it stops */
            stretch -= 0.07f * sinf((ms - CB_RISE_MS * 4 / 10) / 70.0f) *
                       (float)exp(-(ms - CB_RISE_MS * 4 / 10) / 220.0);
    }
    else
    {
        float t = (ms - CB_SINK_AT) / (float)(CB_END_MS - CB_SINK_AT);
        float p = t * t * t;             /* eases in: drops back fast */
        y = cb.rest + (from - cb.rest) * p;
        stretch = 0.16f * t;
    }
    if (stretch > 0.22f) stretch = 0.22f;
    if (stretch < -0.14f) stretch = -0.14f;
    int sy = (int)(256 * (1 + stretch)), sx = (int)(256 * (1 - stretch * 0.8f));
    lv_obj_set_style_transform_scale_x(cb.drop, sx, 0);
    lv_obj_set_style_transform_scale_y(cb.drop, sy, 0);
    lv_obj_set_y(cb.drop, (int)y - CB_D / 2);

    /* the droplet: pulled out of the port behind the drop, then swallowed */
    int td = 46;
    float tp = cb_spring((ms - 90) / (float)(CB_RISE_MS + 120));
    float ty = from + ((y + CB_D / 2 - td / 2 - 6) - from) * tp;
    int topa = ms < 90 ? 0 : ms < 520 ? 255 : ms < 760 ? 255 - (ms - 520) * 255 / 240 : 0;
    if (ms >= CB_SINK_AT) topa = 0;
    lv_obj_set_y(cb.trail, (int)ty - td / 2);
    lv_obj_set_style_opa(cb.trail, (lv_opa_t)topa, 0);

    /* the bolt strikes once the drop has nearly arrived; the charge follows */
    int bolt = ms < 300 ? 0 : ms < 420 ? (ms - 300) * 255 / 120 : 255;
    lv_obj_set_style_opa(cb.bolt, (lv_opa_t)bolt, 0);
    lv_obj_set_style_translate_y(cb.bolt, ms < 420 ? (420 - ms) / 12 : 0, 0);
    int pct = ms < 520 ? 0 : ms < 700 ? (ms - 520) * 255 / 180 : 255;
    lv_obj_set_style_opa(cb.pct, (lv_opa_t)pct, 0);

    int scrim = ms < 300 ? ms * 255 / 300 : ms < CB_SINK_AT ? 255 :
                255 - (ms - CB_SINK_AT) * 255 / (CB_END_MS - CB_SINK_AT);
    lv_obj_set_style_opa(cb.scrim, (lv_opa_t)(scrim < 0 ? 0 : scrim), 0);
}

/* A glassy black disc: dark at the bottom, a rim of light, the window
 * highlight across its top and the bolt's green caught in its base. */
static lv_obj_t *cb_glass(lv_obj_t *parent, int d)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x23262c), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x030304), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xdfe9e2), 0);
    lv_obj_set_style_border_opa(o, 70, 0);
    lv_obj_set_style_transform_pivot_x(o, d / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, d / 2, 0);
    return o;
}

static void cb_highlights(lv_obj_t *drop)
{
    /* light from the window: a thin crescent under the top of the rim,
     * fading before it reaches the middle */
    lv_obj_t *hi = box(drop);
    lv_obj_set_size(hi, CB_D * 58 / 100, CB_D * 20 / 100);
    lv_obj_align(hi, LV_ALIGN_TOP_MID, 0, CB_D * 5 / 100);
    lv_obj_set_style_radius(hi, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hi, lv_color_white(), 0);
    lv_obj_set_style_bg_main_opa(hi, 72, 0);
    lv_obj_set_style_bg_grad_color(hi, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_opa(hi, 0, 0);
    lv_obj_set_style_bg_main_stop(hi, 0, 0);
    lv_obj_set_style_bg_grad_stop(hi, 170, 0);
    lv_obj_set_style_bg_grad_dir(hi, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(hi, LV_OPA_COVER, 0);
    /* a hard glint on the left shoulder */
    lv_obj_t *glint = box(drop);
    lv_obj_set_size(glint, 10, 10);
    lv_obj_set_pos(glint, CB_D * 19 / 100, CB_D * 22 / 100);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(glint, 200, 0);
    /* the bolt's light, caught in the glass at the bottom */
    lv_obj_t *lo = box(drop);
    lv_obj_set_size(lo, CB_D * 66 / 100, CB_D * 26 / 100);
    lv_obj_align(lo, LV_ALIGN_BOTTOM_MID, 0, -CB_D * 4 / 100);
    lv_obj_set_style_radius(lo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lo, lv_color_hex(CB_LIME), 0);
    lv_obj_set_style_bg_main_opa(lo, 0, 0);
    lv_obj_set_style_bg_grad_color(lo, lv_color_hex(CB_LIME), 0);
    lv_obj_set_style_bg_grad_opa(lo, 64, 0);
    lv_obj_set_style_bg_grad_dir(lo, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(lo, LV_OPA_COVER, 0);
}

/* A bolt drawn as three fat strokes, like a marker, over a darker copy
 * knocked down and right: the same hard shadow as the wordmark. */
static lv_obj_t *cb_bolt(lv_obj_t *parent)
{
    static const lv_point_precise_t pts[] = { { 42, 4 }, { 14, 46 }, { 44, 40 }, { 18, 82 } };
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 64, 92);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *l = lv_line_create(b);
        lv_line_set_points(l, pts, 4);
        lv_obj_set_pos(l, k ? 0 : 4, k ? 0 : 5);
        lv_obj_set_style_line_width(l, 11, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(k ? CB_LIME : 0x1d5e12), 0);
    }
    return b;
}

static void charge_bubble_show(int level)
{
    if (cb.root || wallpaper_screen_dark || cf_is_open() || po_is_open()) return;
    cb.level = level;
    /* on the lock screen (a charger woke it), above the player there, not on its controls */
    cb.rest = CB_REST;
    if (sl_power_overlay && !lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN) && lp_card &&
        !lv_obj_has_flag(lp_card, LV_OBJ_FLAG_HIDDEN))
    {
        lv_area_t a;
        lv_obj_update_layout(lp_card);
        lv_obj_get_coords(lp_card, &a);
        if (a.y1 > 0) cb.rest = LV_MIN(CB_REST, a.y1 - CB_D / 2 - SL_GUTTER);
    }
    cb.root = box(lv_layer_top());
    lv_obj_set_size(cb.root, W, H);
    lv_obj_add_flag(cb.root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    /* a dusk over the bottom of the screen for the drop to rise through */
    cb.scrim = box(cb.root);
    lv_obj_set_size(cb.scrim, W, 340);
    lv_obj_align(cb.scrim, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(cb.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(cb.scrim, 0, 0);
    lv_obj_set_style_bg_grad_color(cb.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_opa(cb.scrim, 150, 0);
    lv_obj_set_style_bg_grad_dir(cb.scrim, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cb.scrim, LV_OPA_COVER, 0);

    cb.trail = cb_glass(cb.root, 46);
    lv_obj_set_x(cb.trail, (W - 46) / 2);
    cb.drop = cb_glass(cb.root, CB_D);
    lv_obj_set_x(cb.drop, (W - CB_D) / 2);
    /* the glow it gives off, lime, close in */
    lv_obj_set_style_shadow_color(cb.drop, lv_color_hex(CB_LIME), 0);
    lv_obj_set_style_shadow_width(cb.drop, 44, 0);
    lv_obj_set_style_shadow_opa(cb.drop, 80, 0);
    cb_highlights(cb.drop);
    cb.bolt = cb_bolt(cb.drop);
    lv_obj_align(cb.bolt, LV_ALIGN_CENTER, 2, -12);
    char b[8];
    snprintf(b, sizeof b, "%d%%", level < 0 ? 0 : level > 100 ? 100 : level);
    cb.pct = lv_label_create(cb.drop);
    lv_label_set_text(cb.pct, level < 0 ? "" : b);
    lv_obj_set_style_text_font(cb.pct, &ss_semibold_21, 0);
    lv_obj_set_style_text_color(cb.pct, lv_color_hex(0xeaf7e4), 0);
    lv_obj_align(cb.pct, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cb.root);
    lv_anim_set_values(&a, 0, CB_END_MS);
    lv_anim_set_duration(&a, CB_END_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_custom_exec_cb(&a, cb_step);
    lv_anim_set_completed_cb(&a, cb_gone);
    lv_anim_start(&a);
}

/* Unplugged mid-rise: let it sink straight away. */
static void charge_bubble_unplugged(void)
{
    if (!cb.root) return;
    lv_anim_t *a = lv_anim_get(cb.root, NULL);
    if (a && a->act_time < CB_SINK_AT) a->act_time = CB_SINK_AT;
}
