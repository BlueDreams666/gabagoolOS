/* GabagoolOS display ownership and lock screen. GPLv2+.
 * Included by sloppy.c after sl_ms() and the font declarations. */
#include "sloppy_power_state.h"

static struct sl_power_state sl_power;
static void sv_tick(void);                /* the screensaver (sloppy_saver.inc) */
static bool sv_shown(void);
static void sv_touch_down(void);
static bool car_keeps_lit(void);          /* car mode (sloppy_car.inc) */
static bool sl_power_running;
static bool sl_power_suspended;
static bool sl_power_wake_pending;
static int sl_power_applied_mode = -1, sl_power_applied_level = -1;
static lv_obj_t *sl_power_overlay;

static bool sl_power_is_locked(void)
{
    return sl_power.mode >= SL_POWER_LOCKED;
}

/* The lock screen (or a dark panel) covers the interface: animations and
 * games underneath are not seen. */
static bool arc_playing(void);           /* sloppy_arcade.inc */
static bool sl_power_hides_ui(void)
{
    return sl_power_running && !sl_power_suspended && sl_power.mode >= SL_POWER_LOCKED;
}

static void sl_power_overlay_set_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

static void sl_power_overlay_gone(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(a->var, 0);
}

/* Slide the lock screen from where the finger left it: up and away to
 * reveal the home screen, or back down when the swipe fell short. */
static void sl_power_overlay_slide(bool away)
{
    int from = lv_obj_get_style_y(sl_power_overlay, 0), to = away ? -LCD_HEIGHT : 0;
    lv_anim_delete(sl_power_overlay, sl_power_overlay_set_y);
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sl_power_overlay);
    lv_anim_set_values(&a, from, to);
    int distance = from > to ? from - to : to - from;
    lv_anim_set_duration(&a, SL_T_SPAN(distance, LCD_HEIGHT));
    lv_anim_set_exec_cb(&a, sl_power_overlay_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (away)
        lv_anim_set_completed_cb(&a, sl_power_overlay_gone);
    lv_anim_start(&a);
}

static void sl_power_overlay_drag(int dy)
{
    if (!sl_power_overlay || lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN))
        return;
    lv_anim_delete(sl_power_overlay, sl_power_overlay_set_y);
    lv_obj_set_y(sl_power_overlay, dy > 0 ? -dy : 0);
}

static bool sl_power_image_wallpaper(void)
{
    return global_settings.sloppy_lock_wallpaper >= 5 &&
           global_settings.sloppy_lock_wallpaper <= 9;   /* the photos and your picture */
}

/* the Now Playing card (sloppy_lockplayer.inc) and its controls */
enum { LP_PREV, LP_PLAY, LP_NEXT, LP_SONG, LP_SEEK, LP_VOLUME };
static void lp_refresh(void);
static void lp_drag(int k, int x);
static void lp_cancel(int k, int volume);
static void lp_theme(bool image_wallpaper);
static int lp_hit(int x, int y);
static void lp_press(int k, bool on);
static void lp_activate(int k);

static void cf_close(void);
static void sl_mem_on_dark(void);
static void sl_power_overlay_show(bool show)
{
    if (show) cf_close();                   /* the lock screen is portrait */
    if (!show)
    {
        if (!sl_power_overlay || lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN))
            return;
        if (sl_power_running)
            sl_power_overlay_slide(true);
        else
        {
            lv_anim_delete(sl_power_overlay, sl_power_overlay_set_y);
            lv_obj_set_y(sl_power_overlay, 0);
            lv_obj_add_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!sl_power_overlay)
    {
        sl_power_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(sl_power_overlay);
        lv_obj_set_pos(sl_power_overlay, 0, 0);
        lv_obj_set_size(sl_power_overlay, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_style_bg_color(sl_power_overlay, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(sl_power_overlay, LV_OPA_COVER, 0);
        lv_obj_remove_flag(sl_power_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(sl_power_overlay, sl_wallpaper_draw, LV_EVENT_DRAW_MAIN, (void *)1);
        wallpaper_lock_obj = sl_power_overlay;
        lv_obj_t *bar = lv_obj_create(sl_power_overlay);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 108, 6);
        lv_obj_set_style_radius(bar, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -32);
    }
    lv_obj_set_style_bg_color(sl_power_overlay, lv_color_hex(C_BG), 0);
    bool image_wallpaper = sl_power_image_wallpaper();
    lv_obj_set_style_bg_color(lv_obj_get_child(sl_power_overlay, 0),
                              lv_color_hex(C_ACCENT), 0);
    lv_anim_delete(sl_power_overlay, sl_power_overlay_set_y);
    lv_obj_set_y(sl_power_overlay, 0);
    lv_obj_invalidate(sl_power_overlay);
    lv_obj_remove_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(sl_power_overlay, -1);
    lp_theme(image_wallpaper);
    lp_refresh();
}

/* The R1's LCD sleeps a moment after the backlight goes off, and the touch
 * panel sleeps with it: a tap then reaches nothing and only a key wakes the
 * screen. Tap to wake keeps the LCD from sleeping while GabagoolOS runs. */
static void sl_power_tap_wake_apply(void)
{
#ifdef HAVE_LCD_SLEEP_SETTING
    lcd_set_sleep_after_backlight_off(global_settings.sloppy_tap_wake ? -1
                                      : global_settings.lcd_sleep_after_backlight_off);
#endif
}

static void sl_power_take_display(void)
{
    /* Raw LVGL input bypasses action.c, which normally refreshes Rockbox's
     * backlight timer. Disable both native timers for this UI session so
     * they cannot expire behind our idle policy (including while charging).
     * Keeping native timeout == 0 also ensures the target sends raw touch
     * data while our screen is dark; we swallow that wake contact ourselves. */
    backlight_set_timeout(0);
#if CONFIG_CHARGING
    backlight_set_timeout_plugged(0);
#endif
    sl_power_tap_wake_apply();
#ifdef HAVE_BACKLIGHT_DIM_IDLE
    backlight_set_dim_brightness(0);
    backlight_set_off_timeout(0);
#endif
#if CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING
    /* waking shows the lock screen at once, not a black panel fading up */
    backlight_set_fade_in(false);
#endif
    sl_power_applied_mode = -1;
    sl_power_applied_level = -1;
}

/* With the backlight off nothing is visible, so stop LVGL from redrawing:
 * timers, the mini player and games kept rendering and flushing to a dark
 * panel. LVGL counts enable/disable calls, so change it only on a flip. */
/* Below brightness 1, for the dark (PS24, #126): the backlight cannot go
 * lower, so the brightness sliders run SL_NIGHT_STEPS further, into a
 * black veil over everything. Each step lets through 4/5 of the light of
 * the one before; the darkest, about a sixth of brightness 1. It takes no
 * touches, and a screenshot is taken without it. */
#define SL_NIGHT_STEPS 8
static lv_obj_t *sl_night_veil;
static int sl_night_level(void)
{
    int n = global_settings.sloppy_night_dim;
    return n > 0 && n <= SL_NIGHT_STEPS ? n : 0;
}
static void sl_night_apply(void)
{
    static const uint8_t opa[SL_NIGHT_STEPS + 1] = { 0, 51, 92, 125, 150, 171, 189, 202, 212 };
    int n = sl_night_level();
    if (!n && !sl_night_veil) return;
    if (!sl_night_veil)
    {
        sl_night_veil = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(sl_night_veil);
        lv_obj_set_size(sl_night_veil, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_style_bg_color(sl_night_veil, lv_color_black(), 0);
        lv_obj_remove_flag(sl_night_veil, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_style_bg_opa(sl_night_veil, opa[n], 0);
    lv_obj_update_flag(sl_night_veil, LV_OBJ_FLAG_HIDDEN, !n);
    lv_obj_move_foreground(sl_night_veil);
}
/* Rounded screen corners (Customization > Theme, P62): four little black
 * quarter rings over everything, made once, smoothed at their edge. A
 * corner of 24 px is all it draws over; nothing under it moves. Kept out
 * of screenshots, as the veil is. */
static lv_obj_t *sl_corner[4];
/* corner k's mask (0 top left, 1 top right, 2 bottom left, 3 bottom right),
 * made the first time it is asked for; the screensaver's cover wears them too */
static const lv_image_dsc_t *sl_corner_dsc(int k)
{
    static uint32_t px[4][SL_R_SCREEN * SL_R_SCREEN];
    static lv_image_dsc_t dsc[4];
    if (!dsc[0].data)
    {
        const int R = SL_R_SCREEN;
        for (int c = 0; c < 4; c++)
        {
            for (int y = 0; y < R; y++)
                for (int x = 0; x < R; x++)
                {
                    /* the share of the pixel outside the circle, 4 x 4 samples */
                    int out = 0;
                    for (int sy = 0; sy < 4; sy++)
                        for (int sx = 0; sx < 4; sx++)
                        {
                            float fx = R - (x + (sx + 0.5f) / 4), fy = R - (y + (sy + 0.5f) / 4);
                            out += fx * fx + fy * fy > (float)(R * R);
                        }
                    /* corner c: 0 top left, 1 top right, 2 bottom left, 3 bottom right */
                    int tx = c & 1 ? R - 1 - x : x, ty = c & 2 ? R - 1 - y : y;
                    px[c][ty * R + tx] = (uint32_t)(out * 255 / 16) << 24;
                }
            dsc[c].header.magic = LV_IMAGE_HEADER_MAGIC;
            dsc[c].header.cf = LV_COLOR_FORMAT_ARGB8888;
            dsc[c].header.w = dsc[c].header.h = R;
            dsc[c].header.stride = R * 4;
            dsc[c].data = (const uint8_t *)px[c];
            dsc[c].data_size = sizeof px[c];
        }
    }
    return k >= 0 && k < 4 ? &dsc[k] : NULL;
}
static void sl_corners_apply(void)
{
    bool on = global_settings.sloppy_round_corners == 1;
    if (!on && !sl_corner[0]) return;
    if (!sl_corner[0])
        for (int k = 0; k < 4; k++)
        {
            sl_corner[k] = lv_image_create(lv_layer_sys());
            lv_image_set_src(sl_corner[k], sl_corner_dsc(k));
            lv_obj_remove_flag(sl_corner[k], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(sl_corner[k], k & 1 ? LCD_WIDTH - SL_R_SCREEN : 0, k & 2 ? LCD_HEIGHT - SL_R_SCREEN : 0);
        }
    for (int k = 0; k < 4; k++)
    {
        lv_obj_update_flag(sl_corner[k], LV_OBJ_FLAG_HIDDEN, !on);
        lv_obj_move_foreground(sl_corner[k]);
    }
}
static bool sl_corners_shown(void) { return sl_corner[0] && !lv_obj_has_flag(sl_corner[0], LV_OBJ_FLAG_HIDDEN); }
static void sl_corners_show(bool on)
{
    for (int k = 0; k < 4 && sl_corner[0]; k++) lv_obj_update_flag(sl_corner[k], LV_OBJ_FLAG_HIDDEN, !on);
}
/* the sliders' value: brightness, or below 1 the veil's step */
static int sl_bright_value(void)
{
    return sl_night_level() ? MIN_BRIGHTNESS_SETTING - sl_night_level() : global_settings.brightness;
}
static void sl_bright_set(int v)
{
    global_settings.sloppy_night_dim = v < MIN_BRIGHTNESS_SETTING ? MIN_BRIGHTNESS_SETTING - v : 0;
    global_settings.brightness = MAX(v, MIN_BRIGHTNESS_SETTING);
    backlight_set_brightness(global_settings.brightness);
    sl_night_apply();
}

static void sl_power_set_drawing(bool on)
{
    static bool off;
    if (off == !on) return;
    off = !on;
    lv_display_enable_invalidation(NULL, on);
    if (on) lv_obj_invalidate(lv_screen_active());
}

/* What woke a dark screen, and when it went dark: for the log, so a tap
 * that stopped waking it (minutes in, Tap to wake on) shows up there */
static const char *sl_power_by = "something";
static uint32_t sl_power_dark_ms;
static void sl_power_apply(void)
{
    if (!sl_power_running || sl_power_suspended)
        return;
    wallpaper_screen_dark = sl_power.mode == SL_POWER_DARK;
    sl_power_set_drawing(!wallpaper_screen_dark);
    int level = global_settings.brightness;
    if (sl_power.mode == SL_POWER_DIM || (sl_power.saver && sl_power.mode == SL_POWER_LOCKED))
        level = (level * 65 + 50) / 100; /* a slight, relative dim; the screensaver keeps it */
    if (level < MIN_BRIGHTNESS_SETTING) level = MIN_BRIGHTNESS_SETTING;
    if (level > MAX_BRIGHTNESS_SETTING) level = MAX_BRIGHTNESS_SETTING;

    if (sl_power_applied_mode != (int)sl_power.mode)
    {
        /* an unlock that does not stick shows up here as a quick relock */
        static const char *const names[] = { "active", "dim", "locked", "dark" };
        bool was_dark = sl_power_applied_mode == SL_POWER_DARK;
        if (was_dark)
            sl_log("power: dark -> %s, woken by %s after %u s dark", names[sl_power.mode], sl_power_by,
                   (unsigned)((sl_ms() - sl_power_dark_ms) / 1000));
        else if (sl_power_applied_mode >= 0)
            sl_log("power: %s -> %s", names[sl_power_applied_mode], names[sl_power.mode]);
        if (sl_power.mode == SL_POWER_DARK) sl_power_dark_ms = sl_ms();
        sl_power_by = "something";
        bool was_locked = sl_power_applied_mode >= SL_POWER_LOCKED;
        if (sl_power_applied_mode >= 0 && was_locked != sl_power_is_locked())
            sfx_play(was_locked ? SFX_UI_UNLOCK : SFX_UI_LOCK);
        sl_power_overlay_show(sl_power_is_locked());
        if (sl_power.mode == SL_POWER_DARK)
        {
            sl_power_wake_pending = false;
            sl_mem_on_dark();
            /* Prevent caption backlighting, physical keys, and background
             * notifications from relighting a locked player. Input arrives
             * normally and is explicitly handled by our filters. */
            backlight_on_ignore(true, 0);
            backlight_off();
        }
        else
        {
            backlight_on_ignore(false, 0);
            if (was_dark)
                sl_power_wake_pending = true;
            else
                backlight_wake();
        }
        sl_power_applied_mode = sl_power.mode;
    }
    if (sl_power.mode != SL_POWER_DARK && sl_power_applied_level != level)
    {
        backlight_set_brightness(level);
        sl_power_applied_level = level;
    }
}

static void sl_power_restore_display(void)
{
    /* Restore the current user settings, so changes made during this
     * session are honored when the classic UI takes ownership again. */
    sl_power_set_drawing(true);
    sl_power_wake_pending = false;
    backlight_on_ignore(false, 0);
    backlight_set_brightness(global_settings.brightness);
#ifdef HAVE_BACKLIGHT_DIM_IDLE
    backlight_set_dim_brightness(global_settings.dim_level);
    backlight_set_off_timeout(global_settings.backlight_off_timeout);
#endif
#if CONFIG_BACKLIGHT_FADING == BACKLIGHT_FADING_SW_SETTING
    backlight_set_fade_in(global_settings.backlight_fade_in);
#endif
    backlight_set_timeout(global_settings.backlight_timeout);
#if CONFIG_CHARGING
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
#endif
#ifdef HAVE_LCD_SLEEP_SETTING
    lcd_set_sleep_after_backlight_off(global_settings.lcd_sleep_after_backlight_off);
#endif
    backlight_wake();
}

static void sl_power_begin(void)
{
    sl_power_state_init(&sl_power, sl_ms());
    sl_power_running = true;
    sl_power_suspended = false;
    sl_power_take_display();
    sl_power_apply();
    sl_night_apply();
    sl_corners_apply();
}

static void sl_power_lock(const char *why)
{
    if (!sl_power_running) return;
    sl_log("power: lock from %s", why);
    sl_power_state_set(&sl_power, SL_POWER_DARK, sl_ms());
    sl_power_apply();
}

/* A charger plugged in while the screen is dark lights it on the lock
 * screen, so the charge bubble is seen (#130); it goes dark again after
 * the lock screen's usual 15 s. */
static void sl_power_cable_wake(void)
{
    if (!sl_power_running || sl_power_suspended || sl_power.mode != SL_POWER_DARK) return;
    sl_power_by = "the charger";
    sl_power_state_set(&sl_power, SL_POWER_LOCKED, sl_ms());
    sl_power_apply();
}

static void sl_power_end(void)
{
    sl_power_running = false;
    sl_power_overlay_show(false);
    sl_power_restore_display();
}

/* Classic USB/shutdown dialogs temporarily own the display and input. */
static void sl_power_suspend(void)
{
    if (!sl_power_running) return;
    sl_power_suspended = true;
    sl_power_restore_display();
}

static void sl_power_resume(void)
{
    if (!sl_power_running) return;
    bool locked = sl_power_is_locked();
    sl_power_state_init(&sl_power, sl_ms());
    if (locked)
        sl_power.mode = SL_POWER_LOCKED;
    /* A finger held over a system dialog may not activate the page below. */
    sl_power.touch_down = (button_status() & BUTTON_TOUCHSCREEN) != 0;
    sl_power.swallow_touch = sl_power.touch_down;
    sl_power_suspended = false;
    sl_power_take_display();
    sl_power_apply();
}

/* Power off after the shutdown chime, so it is heard before the audio
 * system goes down; the goodbye screen plays over it (sloppy.c). */
static void bye_play(bool chime);
static void notes_flush(void);
static void sl_poweroff(void)
{
    radio_flush();
    hist_flush();
    notes_flush();
    clicker_flush();
    rd_flush();
    bye_play(sfx_play(SFX_SYS_SHUTDOWN));
    sys_poweroff();
}

/* Slide to power off (sloppy_poweroff.inc). */
static void po_offer(void);
static bool po_is_open(void);
static void po_close(void);
static void po_close_now(void);
static bool po_power_off;               /* slid all the way: off, from the main loop */

static uint32_t sl_power_sleep_ms(void)
{
    static const uint32_t choices[] = { 30000u, 60000u, 120000u };
    int i = global_settings.sloppy_sleep;
    return choices[i >= 0 && i < 3 ? i : 1];
}

static bool sl_keys_locked;               /* Volume down + Power: sl_keys_lock_toggle */
static void sl_keys_note(const char *msg);   /* a notice (sloppy.c, after sloppy_notify.inc) */
static void sl_power_tick(void)
{
    if (po_power_off)
    {
        po_power_off = false;
        po_close_now();                  /* the goodbye screen plays without it */
        sl_poweroff();
        return;
    }
    if (!sl_power_running || sl_power_suspended) return;
    sl_power.sleep_ms = sl_power_sleep_ms();
    /* reading lyrics along with a song: the screen stays on (a lock by
     * hand still darkens it) */
    if (sl_power.mode == SL_POWER_ACTIVE && (sl_reading_lyrics() || arc_playing())) sl_power.activity_at = sl_ms();   /* and a game being played */
    if (sl_power.mode == SL_POWER_ACTIVE && car_keeps_lit()) sl_power.activity_at = sl_ms();   /* car mode on the car's power */
    static bool forced;
    if (!sl_power.power_down) forced = false;   /* armed again for the next hold */
    if (sl_power.power_down && !forced && !sl_keys_locked && sl_ms() - sl_power.power_at >= SL_POWER_FORCE_MS)
    {
        forced = true;                   /* still held: a touch screen may not answer */
        sl_power.power_fired = true;     /* its release offers nothing */
        po_close_now();
        sl_poweroff();
        return;
    }
    sv_tick();                           /* music playing: the screensaver where the screen would sleep */
    if (sl_power_state_tick(&sl_power, sl_ms()))
    {
        if (sl_keys_locked)
            sl_keys_note("Buttons are locked. Hold Volume down and press Power.");
        else po_offer();
    }
    sl_power_apply();
}

static int wallpaper_id(bool lock);
static void wallpaper_nexus_tap_at(int x, int y);
/* The Nexus wallpaper answers touches on the lock screen too: the lock
 * screen takes every contact before LVGL, so it passes them on here. A
 * burst where the finger lands, and another each time a drag has gone a
 * little way, in the overlay's own coordinates (it follows the finger up). */
static void sl_power_nexus_touch(bool start, int x, int y)
{
    static int bx, by;
    int dx = x - bx, dy = y - by;
    if (!sl_power_overlay || wallpaper_id(true) != 8) return;
    if (!start && dx * dx + dy * dy < 90 * 90) return;
    bx = x; by = y;
    lv_area_t a;
    lv_obj_get_coords(sl_power_overlay, &a);
    wallpaper_nexus_tap_at(x - a.x1, y - a.y1);
}

static bool sl_power_filter_touch(bool down, int x, int y)
{
    if (!sl_power_running || sl_power_suspended) return false;
    if (po_is_open()) return false;      /* the power off slider takes every touch */
    static int last_x, last_y;
    static int control = -1;            /* a Now Playing control under the finger */
    /* A contact that starts on a slider is not the slider's until it moves
     * sideways: going up first, it is an unlock swipe after all (the
     * sliders sit where a thumb starts one), and what it previewed is put
     * back. */
    static bool slider_pending;
    static int slider_volume;
    /* what one contact delivered: a swipe logged as 0 px tells a frozen
     * reading (one position, many samples) from a reader that stopped */
    static int samples, y_lo, y_hi;
    if (down && !sl_power.touch_down) { samples = 0; y_lo = y_hi = y; }
    if (down)
    {
        last_x = x; last_y = y; samples++;
        if (y < y_lo) y_lo = y;
        if (y > y_hi) y_hi = y;
    }
    bool was_down = sl_power.touch_down;
    /* Release samples may contain zeroed coordinates on the target. */
    bool dragging = sl_power.touch_down && sl_power.mode == SL_POWER_LOCKED &&
                    !sl_power.swallow_touch;
    /* the screensaver is lit, so a touch on it always wakes */
    sl_power.no_tap_wake = !global_settings.sloppy_tap_wake && !sv_shown();
    if (down && !sl_power.touch_down && sv_shown()) sv_touch_down();   /* a touch puts the screensaver away */
    sl_power_by = "a tap";
    bool consume = sl_power_state_touch(&sl_power, down, last_x, last_y, sl_ms());
    bool live = sl_power.mode == SL_POWER_LOCKED && !sl_power.swallow_touch;
    if (down)
    {
        reset_poweroff_timer();
        if (!was_down && live)
        {
            lp_press(control = lp_hit(x, y), true);
            slider_pending = control >= LP_SEEK;
            slider_volume = global_status.volume;
        }
        int dx = x - sl_power.touch_x, dy = sl_power.touch_y - y;
        if (slider_pending && dy >= 12 && dy > (dx < 0 ? -dx : dx))
        {
            lp_cancel(control, slider_volume);
            control = -1;
            slider_pending = false;
        }
        else if (slider_pending && (dx >= 10 || dx <= -10))
        {
            /* the slider keeps the rest of the contact: no unlock, no tap */
            slider_pending = false;
            sl_power.swallow_touch = true;
        }
        if (control >= LP_SEEK)
            lp_drag(control, x);
        else if (control >= 0 && (dx > 24 || dx < -24 || dy > 24 || dy < -24))
        {
            lp_press(control, false);   /* a swipe after all */
            control = -1;
        }
        /* The lock screen follows the finger up; a button press holds still. */
        if (live && control < 0)
            sl_power_overlay_drag(dy);
        if (live && control < 0)
            sl_power_nexus_touch(!was_down, x, y);
    }
    sl_power_apply();
    if (!down)
    {
        int tapped = control;
        lp_press(control, false);
        control = -1;
        slider_pending = false;
        if (tapped >= LP_SEEK)
            lp_activate(tapped);        /* commit the new song position */
        else if (dragging && sl_power_is_locked() && sl_power_overlay)
        {
            sl_log("lock: swipe up %d px, sideways %d px, %u ms, %d samples, y %d..%d: stays locked",
                   sl_power.touch_y - last_y, last_x - sl_power.touch_x,
                   (unsigned)(sl_ms() - sl_power.touch_at), samples, y_lo, y_hi);
            sl_power_overlay_slide(false);
            lp_activate(tapped);
        }
    }
    return consume;
}

/* Volume down held, then Power (or Power held, then Volume down): the
 * player's own keys lock, a hold switch for a pocket, until the same again.
 * Volume, Play and Next do nothing and Power's hold offers no slider; a
 * press of Power still lights or darkens the screen, and a headset's or
 * Bluetooth's controls still work. Not kept over a restart. */
static void sl_keys_lock_toggle(void)
{
    sl_keys_locked = !sl_keys_locked;
    sl_log("keys: %s", sl_keys_locked ? "locked" : "unlocked");
    sl_keys_note(sl_keys_locked ? "Buttons locked" : "Buttons unlocked");
}
static bool sl_power_filter_button(long btn)
{
    if (!sl_power_running || sl_power_suspended) return false;
    bool released = (btn & BUTTON_REL) != 0;
    bool repeated = (btn & BUTTON_REPEAT) != 0;
    unsigned key = btn & ~(BUTTON_REL | BUTTON_REPEAT);
    /* Power held, then volume up: a screenshot (a PNG in /Screenshots,
     * sloppy_shot.inc), and the power key's release does nothing more */
    /* (Rockbox may report the two held as one code, both bits set) */
    /* Volume up held, then Power: the same, in Sonix's order. The power
     * press is taken as a hold already used, so its release does nothing */
    static bool up_down, down_down, down_combo;
    if (key == BUTTON_UP) up_down = !released;
    if (key == BUTTON_DOWN) down_down = !released;
    if (key == BUTTON_POWER && down_down && !released && !repeated && !sl_power.power_down && !po_is_open())
    {
        sl_power.power_down = sl_power.power_fired = true;
        sl_power.power_at = sl_ms();
        down_combo = true;
        sl_keys_lock_toggle();
        return true;
    }
    if ((key == BUTTON_DOWN && sl_power.power_down) || key == (BUTTON_DOWN | BUTTON_POWER))
    {
        if (!released && !repeated)
        {
            sl_power.power_fired = true;
            down_combo = true;
            sl_keys_lock_toggle();
        }
        return true;
    }
    if (key == BUTTON_DOWN && released && down_combo) { down_combo = false; return true; }   /* the combo's own let-go */
    if (sl_keys_locked && (key == BUTTON_UP || key == BUTTON_DOWN || key == BUTTON_LEFT || key == BUTTON_RIGHT))
    {
        if (!released && !repeated && sl_power.mode < SL_POWER_DARK)
            sl_keys_note("Buttons are locked. Hold Volume down and press Power.");
        return true;
    }
    if (key == BUTTON_POWER && up_down && !released && !repeated && !sl_power.power_down &&
        sl_power.mode < SL_POWER_LOCKED && !po_is_open())
    {
        sl_power.power_down = sl_power.power_fired = true;
        sl_power.power_at = sl_ms();
        sl_screenshot();
        return true;
    }
    if (((key == BUTTON_UP && sl_power.power_down) || key == (BUTTON_UP | BUTTON_POWER)) && sl_power.mode < SL_POWER_LOCKED)
    {
        if (!released && !repeated)
        {
            sl_power.power_fired = true;
            sl_screenshot();
        }
        return true;
    }
    if (key == BUTTON_POWER)
    {
        /* with the slider up, a press of its own puts it away; the hold
         * that brought it up ends as usual */
        static bool po_press;
        if (po_is_open() && !released && !repeated && !sl_power.power_down)
        {
            po_press = true;
            sl_power.power_down = sl_power.power_fired = true;
            sl_power.power_at = sl_ms();
            return true;
        }
        if (po_press && released)
        {
            po_press = false;
            sl_power.power_down = false;
            if (sl_ms() - sl_power.power_at < SL_POWER_HOLD_MS) po_close();
            return true;
        }
        sl_power_by = "the power key";
        if (sl_power_state_power(&sl_power, released, repeated, sl_ms()))
        {
            if (sl_keys_locked)
                sl_keys_note("Buttons are locked. Hold Volume down and press Power.");
            else po_offer();
        }
        sl_power_apply();
        return true;
    }
    /* Hiby maps Bluetooth controls separately, without BUTTON_REMOTE.
     * A headset remains usable with the player locked in a pocket. */
    if (key == BUTTON_PLAY || key == BUTTON_NEXT || key == BUTTON_PREV)
        return false;
    /* Settings > Buttons: volume and track keys work while the screen is
     * off or locked, and leave it that way */
    if (global_settings.sloppy_keys_dark && sl_power.mode >= SL_POWER_LOCKED)
        return false;
    /* With the lock screen showing, the volume keys work as usual and show
     * the volume; it stays lit while they do. A key that woke a dark
     * screen does nothing more until it is let go. */
    if ((key == BUTTON_UP || key == BUTTON_DOWN) && sl_power.mode == SL_POWER_LOCKED &&
        !(sl_power.ignored_keys & key))
    {
        sl_power.activity_at = sl_ms();
        sl_power.saver = false;          /* the screensaver goes; the volume shows over the lock screen */
        return false;
    }
    sl_power_by = "a key";
    bool consume = sl_power_state_key(&sl_power, key, released, sl_ms());
    sl_power_apply();
    return consume;
}
