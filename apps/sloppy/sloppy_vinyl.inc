/* Vinyl mode for Now Playing. GPLv2+. Included by sloppy.c after the lyrics.
 *
 * The cover gives way to a turntable. The record is drawn once: grooves,
 * track gaps and a sheen that stays put, like light on a real record. Only
 * the label (the cover, cut into a circle) turns, at 33 1/3 rpm, so each
 * frame repaints a 150 px square rather than the whole disc. The tone arm
 * tracks the song's progress, dropping in with a crackle on a new song.
 * Put a finger on the record and the song is under it (sloppy_scratch.h):
 * the music stops with the record, plays backwards and forwards with it,
 * at its speed, and spins up again on release. One turn is 1.8 s of music,
 * as on a real one. */
#include "pcmbuf.h"
#include "sloppy_scratch.h"

#define VNL_D      452              /* platter, and the record canvas */
#define VNL_R      219              /* record */
#define VNL_LABEL  150
#define VNL_CX     (W / 2)
#define VNL_CY     (W / 2)
#define VNL_BG     0x1b1a20
#define VNL_RPM_U  2                /* rotation units (0.1 deg) per ms: 33 1/3 rpm */
#define VNL_MS_PER_TURN 1800

static lv_obj_t *vnl, *vnl_disc, *vnl_label, *vnl_arm, *vnl_head, *vnl_base;   /* vnl_btn: sloppy_npglass.inc */
static uint16_t *vnl_disc_buf;
static uint32_t *vnl_label_buf;
static lv_image_dsc_t vnl_label_img;
static lv_timer_t *vnl_timer;
static bool vnl_label_dirty = true, vnl_dragging, vnl_arm_swing;
static int32_t vnl_angle;           /* 0.1 degrees */
static float vnl_speed;             /* units per ms, eases to VNL_RPM_U or 0 */
static float vnl_arm_deg, vnl_arm_goal;
static uint32_t vnl_last_ms, vnl_drag_ms;
static int32_t vnl_drag_prev, vnl_drag_total;
static lv_point_precise_t vnl_arm_pts[2], vnl_head_pts[2];
static const lv_point_t vnl_pivot = { W - 30, 100 };   /* below the Lyrics button */
#define VNL_ARM_LEN 262.0f
#define VNL_ARM_REST 86.0f

static bool vnl_on(void) { return global_settings.sloppy_np_vinyl == 1; }

/* ---- the needle on the song ---------------------------------------------- */
static bool vnl_needle, vnl_unpaused;
/* Put the scratch filter between pcmbuf and the mixer while the turntable
 * shows. Its ring is 1 MB, so it only exists then. */
static void vnl_needle_on(void)
{
    if (vnl_needle) { scr_leaving = false; return; }
    if (!scr_ring && !(scr_ring = malloc(SCR_RING * 4))) return;
    scr_fs = mixer_get_frequency();
    scr_held = false;
    scr_motor = is_playing();
    scr_leaving = scr_bypass = false;
    scr_reset();
    pcmbuf_set_filter(scr_filter);
    vnl_needle = true;
}
/* Leaving plays out what the ring holds first, so it never skips a beat;
 * vnl_needle_poll() unhooks the filter once it passes the song straight on. */
static void vnl_needle_off(void)
{
    if (!vnl_needle) return;
    scr_held = false;
    scr_motor = true;
    scr_leaving = true;
}
static void vnl_needle_poll(void)
{
    if (!vnl_needle || !scr_leaving) return;
    /* stopped, nothing will be heard from the ring anyway */
    if (!scr_bypass && mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_STOPPED) return;
    pcmbuf_set_filter(NULL);
    vnl_needle = false;
    free(scr_ring);
    scr_ring = NULL;
}
/* A hand on the record. Paused, the channel runs for the scratch alone. */
static void vnl_hand_down(void)
{
    if (!vnl_needle) return;
    if ((audio_status() & AUDIO_STATUS_PAUSE) &&
        mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_PAUSED)
    {
        scr_motor = false;
        mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, true);
        vnl_unpaused = true;
    }
    scr_grab();
}
/* The ring's audio trails what Rockbox counts as played. */
static unsigned long vnl_lag_ms(void) { return vnl_needle ? scr_lag_ms() : 0; }

static uint16_t vnl_rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

/* The record, once: platter rim, vinyl grooves with two quiet gaps between
 * songs, a smooth lead-in, and a fixed two-lobed sheen. */
static void vnl_draw_disc(void)
{
    const float c = VNL_D / 2.0f;
    uint16_t bg = vnl_rgb(0x1b, 0x1a, 0x20);
    static int8_t groove[VNL_R * 4];
    for (int i = 0; i < VNL_R * 4; i++)
        groove[i] = (int8_t)lroundf(4.0f * sinf(i * 0.575f) + 2.0f * sinf(i * 0.0925f));
    for (int y = 0; y < VNL_D; y++)
    {
        uint16_t *row = vnl_disc_buf + y * VNL_D;
        float fy = y + 0.5f - c;
        for (int x = 0; x < VNL_D; x++)
        {
            float fx = x + 0.5f - c, r = sqrtf(fx * fx + fy * fy);
            if (r > c) { row[x] = bg; continue; }
            /* angles without atan2: cos(th + a) and cos(2 (th - b)) from
             * the pixel's own offsets, and the grooves from a table by
             * quarter pixel of radius */
            if (r > VNL_R + 2)               /* the brushed platter edge */
            {
                int v = 96 + (int)(40 * (fx * 0.6967f - fy * 0.7174f) / r);
                row[x] = vnl_rgb(v, v + 2, v + 8);
                continue;
            }
            if (r > VNL_R - 1) { row[x] = vnl_rgb(40, 40, 46); continue; }
            float v = 17;
            bool smooth = r > 204 || (r > 148 && r < 152) || (r > 116 && r < 119) || r < 84;
            if (!smooth) v += groove[(int)(r * 4)];
            float r2 = r * r + 0.001f;
            float s = ((fx * fx - fy * fy) * 0.0707f + 2 * fx * fy * 0.9975f) / r2;
            if (s > 0) { s *= s; s *= s; s *= s; v += (smooth ? 30 : 46) * s; }
            row[x] = vnl_rgb((int)v, (int)v, (int)(v + 5));
        }
        if ((y & 63) == 63) yield();
    }
}

/* The label: the cover cut into a circle, with a rim and a spindle hole.
 * Without a cover, a plain label in the song's colour. */
static void vnl_draw_label(void)
{
    vnl_label_dirty = false;
    bool art = np_art_img && !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data;
    lv_color_t plain = lv_obj_get_style_bg_color(np_art, 0);
    const float c = VNL_LABEL / 2.0f;
    int aw = art ? (int)np_img.header.w : 1, ah = art ? (int)np_img.header.h : 1;
    int stride = art ? (int)np_img.header.stride / 2 : 0;
    const uint16_t *src = art ? (const uint16_t *)np_img.data : NULL;
    for (int y = 0; y < VNL_LABEL; y++)
    {
        uint32_t *row = vnl_label_buf + y * VNL_LABEL;
        float fy = y + 0.5f - c;
        for (int x = 0; x < VNL_LABEL; x++)
        {
            float fx = x + 0.5f - c, r = sqrtf(fx * fx + fy * fy);
            float edge = c - r;                        /* anti-aliased rim */
            if (edge <= 0) { row[x] = 0; continue; }
            int red, green, blue;
            if (r < 4.5f) { red = green = blue = 12; }
            else if (r > c - 4) { red = 34; green = 32; blue = 38; }
            else if (src)
            {
                uint16_t p = src[(y * ah / VNL_LABEL) * stride + x * aw / VNL_LABEL];
                red = (p >> 11) << 3; green = ((p >> 5) & 63) << 2; blue = (p & 31) << 3;
            }
            else
            {
                red = plain.red; green = plain.green; blue = plain.blue;
                if ((int)r % 14 == 0) { red = red * 3 / 4; green = green * 3 / 4; blue = blue * 3 / 4; }
            }
            uint32_t a = edge >= 1 ? 255 : (uint32_t)(edge * 255);
            row[x] = a << 24 | (uint32_t)red << 16 | (uint32_t)green << 8 | (uint32_t)blue;
        }
    }
    lv_image_cache_drop(&vnl_label_img);
    lv_image_set_src(vnl_label, &vnl_label_img);
    lv_obj_invalidate(vnl_label);
}

/* Where the needle sits for an arm angle (degrees, screen space). */
static float vnl_needle_r(float deg)
{
    float a = deg * 3.14159265f / 180;
    float nx = vnl_pivot.x + VNL_ARM_LEN * cosf(a) - VNL_CX, ny = vnl_pivot.y + VNL_ARM_LEN * sinf(a) - VNL_CY;
    return sqrtf(nx * nx + ny * ny);
}

/* The arm angle that puts the needle at radius r (the arm swings inward as
 * the angle grows). */
static float vnl_arm_for(float r)
{
    float lo = VNL_ARM_REST, hi = 150;
    for (int i = 0; i < 24; i++)
    {
        float mid = (lo + hi) / 2;
        if (vnl_needle_r(mid) > r) lo = mid; else hi = mid;
    }
    return lo;
}

static void vnl_place_arm(float deg)
{
    float a = deg * 3.14159265f / 180, ca = cosf(a), sa = sinf(a);
    vnl_arm_pts[0] = (lv_point_precise_t){ vnl_pivot.x - 30 * ca, vnl_pivot.y - 30 * sa };
    vnl_arm_pts[1] = (lv_point_precise_t){ vnl_pivot.x + (VNL_ARM_LEN - 30) * ca, vnl_pivot.y + (VNL_ARM_LEN - 30) * sa };
    /* the headshell bends off the arm toward the record's centre */
    float b = a + 0.38f;
    vnl_head_pts[0] = vnl_arm_pts[1];
    vnl_head_pts[1] = (lv_point_precise_t){ vnl_arm_pts[1].x + 40 * cosf(b), vnl_arm_pts[1].y + 40 * sinf(b) };
    lv_line_set_points(vnl_arm, vnl_arm_pts, 2);
    lv_line_set_points(vnl_head, vnl_head_pts, 2);
}

/* Where the arm should be: off the record when stopped, else from the
 * lead-in (r 204) to the run-out (r 88) with the song's progress. */
static void vnl_goal(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!(audio_status() & AUDIO_STATUS_PLAY) || !id3) { vnl_arm_goal = VNL_ARM_REST; return; }
    float u = id3->length ? (float)visible_elapsed(id3) / id3->length : 0;
    if (u > 1) u = 1;
    vnl_arm_goal = vnl_arm_for(204 - 116 * u);
}

static void vnl_tick(lv_timer_t *t)
{
    uint32_t now = sl_ms();
    uint32_t dt = now - vnl_last_ms;
    vnl_last_ms = now;
    if (!np_open) { vnl_needle_off(); lv_timer_pause(t); return; }  /* np_show wakes it */
    if (!vnl) return;
    if (vnl_needle)
    {
        scr_motor = is_playing();
        scr_fs = mixer_get_frequency();
        /* paused again once a scratch while paused has come to rest */
        if (vnl_unpaused && !vnl_dragging && scr_still)
        {
            vnl_unpaused = false;
            if (audio_status() & AUDIO_STATUS_PAUSE)
                mixer_channel_play_pause(PCM_MIXER_CHAN_PLAYBACK, false);
        }
    }
    if (dt > 100) dt = 100;
    if (vnl_label_dirty) vnl_draw_label();
    if (!vnl_dragging)
    {
        float want = is_playing() ? VNL_RPM_U : 0;     /* spin up quickly, coast down */
        vnl_speed += (want - vnl_speed) * (want > vnl_speed ? 0.18f : 0.06f);
        if (vnl_speed < 0.01f && !want) vnl_speed = 0;
        int32_t step = (int32_t)lroundf(vnl_speed * dt);
        if (step)
        {
            vnl_angle = (vnl_angle + step) % 3600;
            lv_image_set_rotation(vnl_label, vnl_angle);
        }
    }
    /* The arm creeps a fraction of a degree a second while a song plays:
     * move it when that adds up to a visible step, and swing it smoothly
     * when it has somewhere to go (a new song, a seek, a stop). */
    vnl_goal();
    float d = vnl_arm_goal - vnl_arm_deg;
    if (d > 3 || d < -3) vnl_arm_swing = true;
    if (vnl_arm_swing)
    {
        vnl_arm_deg += d * (d > 3 || d < -3 ? 0.16f : 0.4f);
        if (d < 0.1f && d > -0.1f) { vnl_arm_deg = vnl_arm_goal; vnl_arm_swing = false; }
        vnl_place_arm(vnl_arm_deg);
    }
    else if (d > 0.4f || d < -0.4f) { vnl_arm_deg = vnl_arm_goal; vnl_place_arm(vnl_arm_deg); }
}

/* ---- scratching ---------------------------------------------------------- */
static int32_t vnl_touch_angle(bool *on_record)
{
    lv_point_t p = { 0, 0 };
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(vnl, &a);
    float fx = p.x - (a.x1 + VNL_CX), fy = p.y - (a.y1 + VNL_CY);
    if (on_record) *on_record = fx * fx + fy * fy <= (float)VNL_R * VNL_R;
    return (int32_t)lroundf(atan2f(fy, fx) * 1800 / 3.14159265f);
}

static void vnl_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    struct mp3entry *id3 = audio_current_track();
    if (code == LV_EVENT_PRESSED)
    {
        bool on = false;
        vnl_drag_prev = vnl_touch_angle(&on);
        vnl_dragging = on && id3;
        vnl_drag_total = 0;
        vnl_drag_ms = sl_ms();
        if (vnl_dragging) { vnl_speed = 0; vnl_hand_down(); }
        else np_drag_cb(e);                    /* off the record: the sheet's own drag */
    }
    else if (!vnl_dragging)
    {
        if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
            np_drag_cb(e);
    }
    else if (code == LV_EVENT_PRESSING && vnl_dragging)
    {
        int32_t a = vnl_touch_angle(NULL), d = a - vnl_drag_prev;
        if (d > 1800) d -= 3600;
        if (d < -1800) d += 3600;
        uint32_t now = sl_ms(), dt = now - vnl_drag_ms;
        vnl_drag_prev = a;
        vnl_drag_ms = now;
        vnl_drag_total += d;
        vnl_angle = ((vnl_angle + d) % 3600 + 3600) % 3600;
        lv_image_set_rotation(vnl_label, vnl_angle);
        /* the needle goes where the record went: a turn is 1.8 s */
        if (vnl_needle) scr_turn((int32_t)((int64_t)d * VNL_MS_PER_TURN * scr_fs / 3600000), dt);
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && vnl_dragging)
    {
        vnl_dragging = false;
        long ms = (long)vnl_drag_total * VNL_MS_PER_TURN / 3600;
        if (vnl_needle)
        {
            scr_release();
            /* Wound back further than the groove the ring holds: go there
             * the ordinary way. */
            uint32_t past = scr_past_groove();
            if (!id3 || past < scr_fs / 4) return;
            ms = -(long)vnl_lag_ms() - (long)((uint64_t)past * 1000 / scr_fs);
        }
        if (!id3 || (ms < 150 && ms > -150)) return;
        long to = (long)visible_elapsed(id3) + ms;
        long end = id3->length > 1000 ? (long)id3->length - 1000 : 0;
        if (to < 0) to = 0;
        if (to > end) to = end;
        sl_log("vinyl: scratched %ld ms", ms);
        if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
        audio_ff_rewind(to);
    }
}

/* ---- on and off ------------------------------------------------------------ */
static void vnl_btn_style(void)
{
    if (!vnl_btn) return;
    lv_obj_set_style_bg_color(vnl_btn, lv_color_hex(vnl_on() ? C_ACCENT : 0x000000), 0);
    lv_obj_set_style_bg_opa(vnl_btn, vnl_on() ? LV_OPA_COVER : LV_OPA_30, 0);
    ng_buttons();                        /* glass over the cover, or not */
}

static void vnl_free(void)
{
    vnl_needle_off();
    if (vnl_timer) { lv_timer_delete(vnl_timer); vnl_timer = NULL; }
    if (vnl) { lv_obj_delete(vnl); vnl = NULL; }
    free(vnl_disc_buf); vnl_disc_buf = NULL;
    free(vnl_label_buf); vnl_label_buf = NULL;
    vnl_disc = vnl_label = vnl_arm = vnl_head = vnl_base = NULL;
}

static lv_obj_t *vnl_line(lv_obj_t *parent, int width, uint32_t color)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static bool vnl_build(lv_obj_t *np_parent)
{
    vnl_disc_buf = malloc(VNL_D * VNL_D * sizeof *vnl_disc_buf);
    vnl_label_buf = malloc(VNL_LABEL * VNL_LABEL * sizeof *vnl_label_buf);
    if (!vnl_disc_buf || !vnl_label_buf) { vnl_free(); return false; }
    vnl = box(np_parent);
    lv_obj_set_size(vnl, W, W);
    lv_obj_set_style_bg_color(vnl, lv_color_hex(VNL_BG), 0);
    lv_obj_set_style_bg_opa(vnl, LV_OPA_COVER, 0);
    lv_obj_move_to_index(vnl, lv_obj_get_index(np_art) + 1);

    vnl_draw_disc();
    vnl_disc = lv_canvas_create(vnl);
    lv_canvas_set_buffer(vnl_disc, vnl_disc_buf, VNL_D, VNL_D, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(vnl_disc, VNL_CX - VNL_D / 2, VNL_CY - VNL_D / 2);
    lv_obj_add_flag(vnl_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe */
    lv_obj_remove_flag(vnl_disc, LV_OBJ_FLAG_EVENT_BUBBLE);                  /* nor sheet drag */
    lv_obj_add_event_cb(vnl_disc, vnl_touch_cb, LV_EVENT_ALL, NULL);

    vnl_label_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    vnl_label_img.header.cf = LV_COLOR_FORMAT_ARGB8888;
    vnl_label_img.header.w = VNL_LABEL;
    vnl_label_img.header.h = VNL_LABEL;
    vnl_label_img.header.stride = VNL_LABEL * 4;
    vnl_label_img.data = (const uint8_t *)vnl_label_buf;
    vnl_label_img.data_size = VNL_LABEL * VNL_LABEL * 4;
    vnl_label = lv_image_create(vnl);
    lv_obj_set_pos(vnl_label, VNL_CX - VNL_LABEL / 2, VNL_CY - VNL_LABEL / 2);
    lv_obj_set_size(vnl_label, VNL_LABEL, VNL_LABEL);
    lv_image_set_pivot(vnl_label, VNL_LABEL / 2, VNL_LABEL / 2);
    lv_obj_remove_flag(vnl_label, LV_OBJ_FLAG_CLICKABLE);
    vnl_draw_label();
    lv_image_set_rotation(vnl_label, vnl_angle);

    vnl_base = box(vnl);                 /* the arm's pivot */
    lv_obj_set_size(vnl_base, 58, 58);
    lv_obj_set_pos(vnl_base, vnl_pivot.x - 29, vnl_pivot.y - 29);
    lv_obj_set_style_radius(vnl_base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(vnl_base, lv_color_hex(0x3a3b43), 0);
    lv_obj_set_style_bg_opa(vnl_base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vnl_base, 6, 0);
    lv_obj_set_style_border_color(vnl_base, lv_color_hex(0x8c909c), 0);
    vnl_arm = vnl_line(vnl, 9, 0xd3d6de);
    vnl_head = vnl_line(vnl, 16, 0x6e727e);
    vnl_arm_deg = VNL_ARM_REST;
    vnl_place_arm(vnl_arm_deg);
    vnl_last_ms = sl_ms();
    vnl_timer = lv_timer_create(vnl_tick, 33, NULL);
    if (np_open) vnl_needle_on();
    return true;
}

static void vnl_apply(lv_obj_t *np_parent, bool drop)
{
    vnl_btn_style();
    if (vnl_on())
    {
        if (!vnl && !vnl_build(np_parent))
        {
            global_settings.sloppy_np_vinyl = 0;
            vnl_btn_style();
            toast_warn("Not enough memory for the turntable");
            return;
        }
        lv_obj_add_flag(np_art, LV_OBJ_FLAG_HIDDEN);
        if (drop && is_playing()) sfx_play(SFX_VINYL_DROP);
    }
    else
    {
        vnl_free();
        lv_obj_remove_flag(np_art, LV_OBJ_FLAG_HIDDEN);
    }
}

static void vnl_btn_cb(lv_event_t *e)
{
    (void)e;
    global_settings.sloppy_np_vinyl = vnl_on() ? 0 : 1;
    settings_save();
    vnl_arm_deg = VNL_ARM_REST;           /* the needle drops in from its rest */
    vnl_apply(np, true);
    now_playing_retheme();               /* Centred keeps the square for the turntable */
}

static void vnl_wake(void)
{
    if (!vnl_timer) return;
    vnl_needle_on();
    vnl_last_ms = sl_ms();
    lv_timer_resume(vnl_timer);
}

/* From np_update: a new song lifts the arm and drops it again. */
static void vnl_track_changed(void)
{
    vnl_label_dirty = true;
    if (!vnl) return;
    vnl_arm_deg = VNL_ARM_REST;
    vnl_place_arm(vnl_arm_deg);
    if (np_open && is_playing()) sfx_play(SFX_VINYL_DROP);
}

static void vnl_build_button(lv_obj_t *parent)
{
    vnl_btn = box(parent);
    lv_obj_set_size(vnl_btn, 56, 56);
    lv_obj_set_pos(vnl_btn, 72, 10);
    lv_obj_set_style_radius(vnl_btn, SL_R_FULL, 0);
    lv_obj_t *disc = box(vnl_btn);           /* a little record for an icon */
    lv_obj_set_size(disc, 32, 32);
    lv_obj_center(disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disc, 2, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(0xe8e8ee), 0);
    lv_obj_t *dot = box(disc);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xf0602f), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    on_click(vnl_btn, vnl_btn_cb, NULL);
    lv_obj_remove_flag(vnl_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    vnl_btn_style();
}
