/* GabagoolOS car mode (PS18). GPLv2+. Included by sloppy.c after the
 * screensaver, whose cover scaling it shares.
 *
 * For a player that lives on a car's USB port. Hiby-R1-Mod's car mode
 * resumes playback at boot and offers to shut down when the charger goes;
 * this does that and what it leaves out:
 *  - power comes (the car starts, or the player starts on the car's
 *    power): 3 s later, while the engine settles, the music plays where it
 *    stopped, and a screen of big controls comes up and stays lit;
 *  - power goes: the music pauses at once, and the player turns itself off
 *    30 s later unless "Stay on" is tapped (or the power comes back).
 * Each part can be turned off on its own (Time and power > Car mode). */

#define CAR_ON      1                     /* sloppy_car: bit 0 car mode; the rest turn a part off */
#define CAR_NO_PLAY 2
#define CAR_NO_BIG  4
#define CAR_NO_OFF  8
#define CAR_RESUME_MS 3000u
#define CAR_OFF_S 30
static bool car_on(void) { return (global_settings.sloppy_car & CAR_ON) != 0; }
static bool car_part(int no) { return car_on() && !(global_settings.sloppy_car & no); }

static struct {
    lv_obj_t *root, *img, *corner[4], *title, *artist, *pp, *vol;
    lv_obj_t *off, *off_count;            /* the countdown when the power went */
    lv_timer_t *timer;
    lv_image_dsc_t dsc;
    uint16_t *px;
    uint32_t art_sum, resume_at, off_at;
    char path[MAX_PATH];
} car;

static bool car_shown(void) { return car.root && !lv_obj_has_flag(car.root, LV_OBJ_FLAG_HIDDEN); }
static bool car_off_shown(void) { return car.off && !lv_obj_has_flag(car.off, LV_OBJ_FLAG_HIDDEN); }

/* the big screen's words, cover, play button and volume */
static void car_refresh(bool force)
{
    if (!car_shown()) return;
    struct mp3entry *id3 = audio_current_track();
    char b[64];
    if (force || (id3 && strcmp(car.path, id3->path)) || (!id3 && car.path[0]))
    {
        strlcpy(car.path, id3 ? id3->path : "", sizeof car.path);
        label_if_changed(car.title, id3 ? track_title(id3, b, sizeof b) : sl_tr("Nothing playing"));
        label_if_changed(car.artist, id3 ? track_artist(id3) : "");
    }
    uint32_t sum = sv_has_art() ? sv_art_sum() : 0;
    if (force || sum != car.art_sum)
    {
        car.art_sum = sum;
        bool art = sum && sl_cover_scaled(&car.px, &car.dsc, 240);
        if (art) lv_image_set_src(car.img, &car.dsc);
        lv_obj_update_flag(car.img, LV_OBJ_FLAG_HIDDEN, !art);
    }
    const char *pp = is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    label_if_changed(lv_obj_get_child(car.pp, 0), pp);
    snprintf(b, sizeof b, "%d", volume_pct());
    label_if_changed(car.vol, b);
}
static void car_hide(void)
{
    if (!car_shown()) return;
    lv_obj_add_flag(car.root, LV_OBJ_FLAG_HIDDEN);
    if (car.px) { lv_image_set_src(car.img, NULL); lv_image_cache_drop(&car.dsc); free(car.px); car.px = NULL; }
    car.art_sum = 0;
}
static void car_btn_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    switch (k)
    {
    case 0: car_hide(); return;
    case 1: np_skip(-1); break;
    case 2: toggle_play(); break;
    case 3: np_skip(1); break;
    case 4: case 5:
        global_status.volume = volume_from_pct(LV_CLAMP(0, volume_pct() + (k == 5 ? 5 : -5), 100));
        setvol();
        statusbar_update();
        break;
    }
    sfx_play(SFX_TAP);
    car_refresh(false);
}
/* a round button of the big screen: the play button white, the rest on the card */
static lv_obj_t *car_btn(lv_obj_t *parent, const char *sym, int size, const lv_font_t *f, bool primary, int k)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT_UI : C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT_UI : C_CARD_P), LV_STATE_PRESSED);
    press_motion(b);
    lv_obj_set_style_transform_scale(b, 230, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, size / 2, 0);
    lv_obj_t *l = text(b, sym, f, primary ? sl_ink_on(C_ACCENT_UI) : C_TEXT);
    lv_obj_center(l);
    on_click(b, car_btn_cb, (void *)(intptr_t)k);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}
static void car_timer_cb(lv_timer_t *t);
/* The big screen: under the status bar, the cover, the song, three
 * buttons a thumb finds without looking, the volume. Built each time it
 * comes up (it is rare), in the theme of the moment. */
static void car_show(void)
{
    if (car.root) { lv_obj_delete(car.root); car.root = NULL; car.px = NULL; }
    car.root = box(lv_layer_top());
    lv_obj_set_size(car.root, W, H - SB_H);
    lv_obj_set_pos(car.root, 0, SB_H);
    lv_obj_set_style_bg_color(car.root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(car.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(car.root, LV_OBJ_FLAG_CLICKABLE);    /* nothing under it takes a touch */
    lv_obj_remove_flag(car.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *down = car_btn(car.root, LV_SYMBOL_DOWN, 56, &sl_sym_24, false, 0);
    lv_obj_set_pos(down, SL_GUTTER, SL_SP_S);
    car.img = lv_image_create(car.root);
    lv_obj_align(car.img, LV_ALIGN_TOP_MID, 0, 24);
    for (int k = 0; k < 4; k++)
    {
        car.corner[k] = lv_image_create(car.img);
        lv_image_set_src(car.corner[k], sl_corner_dsc(k));
        lv_obj_align(car.corner[k], k == 0 ? LV_ALIGN_TOP_LEFT : k == 1 ? LV_ALIGN_TOP_RIGHT :
                                    k == 2 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        /* the masks are black: on a light page, the page's own colour */
        lv_obj_set_style_image_recolor(car.corner[k], lv_color_hex(C_BG), 0);
        lv_obj_set_style_image_recolor_opa(car.corner[k], LV_OPA_COVER, 0);
    }
    car.title = text(car.root, "", &ss_semibold_38, C_TEXT);
    car.artist = text(car.root, "", &ss_regular_26, C_SUB);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = i ? car.artist : car.title;
        lv_obj_set_width(l, W - 2 * SL_INSET);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(l, 1);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 24 + 240 + SL_INSET + i * (lv_font_get_line_height(&ss_semibold_38) + SL_SP_XS));
    }
    lv_obj_t *row = box(car.root);
    lv_obj_set_size(row, W - 2 * SL_GUTTER, 148);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 400);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    car_btn(row, LV_SYMBOL_PREV, 116, &sl_sym_48, false, 1);
    car.pp = car_btn(row, LV_SYMBOL_PLAY, 148, &sl_sym_48, true, 2);
    car_btn(row, LV_SYMBOL_NEXT, 116, &sl_sym_48, false, 3);
    lv_obj_t *vrow = box(car.root);
    lv_obj_set_size(vrow, W - 2 * SL_GUTTER, 96);
    lv_obj_align(vrow, LV_ALIGN_TOP_MID, 0, 400 + 148 + SL_INSET);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    car_btn(vrow, LV_SYMBOL_VOLUME_MID, 96, &sl_sym_32, false, 4);
    car.vol = text(vrow, "", &ss_semibold_38, C_TEXT);
    lv_obj_set_width(car.vol, 96);
    lv_obj_set_style_text_align(car.vol, LV_TEXT_ALIGN_CENTER, 0);
    car_btn(vrow, LV_SYMBOL_VOLUME_MAX, 96, &sl_sym_32, false, 5);
    if (!car.timer) car.timer = lv_timer_create(car_timer_cb, 1000, NULL);
    car.path[0] = '\0';
    car_refresh(true);
    lv_obj_move_foreground(car.root);
    if (car.off) lv_obj_move_foreground(car.off);
}

/* the power went: pause now, off in 30 s unless asked to stay */
static void car_off_cb(lv_event_t *e)
{
    bool stay = (intptr_t)lv_event_get_user_data(e) == 0;
    lv_obj_add_flag(car.off, LV_OBJ_FLAG_HIDDEN);
    car.off_at = 0;
    if (!stay) { sl_log("car: turned off by hand"); sl_poweroff(); }
    else sl_log("car: staying on");
}
static void car_off_show(void)
{
    if (!car.off)
    {
        car.off = box(lv_layer_top());
        lv_obj_set_size(car.off, W, H);
        lv_obj_set_style_bg_color(car.off, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(car.off, LV_OPA_70, 0);
        lv_obj_add_flag(car.off, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *card = box(car.off);
        lv_obj_set_size(card, W - 2 * SL_FLOAT, LV_SIZE_CONTENT);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(card, sl_zune() ? 0 : SL_R_XL, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        sl_elevate(card, SL_E3, 0);
        lv_obj_set_style_pad_all(card, SL_INSET, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, SL_GUTTER, 0);
        lv_obj_t *t = text(card, "The car's power is off", &ss_semibold_30, C_TEXT);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        car.off_count = text(card, "", &ss_regular_21, C_SUB);
        lv_obj_set_width(car.off_count, lv_pct(100));
        lv_obj_set_style_text_align(car.off_count, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(car.off_count, LV_LABEL_LONG_WRAP);
        for (int i = 0; i < 2; i++)
        {
            lv_obj_t *b = box(card);
            lv_obj_set_size(b, lv_pct(100), SL_PILL_H + SL_SP_S * 2);
            lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_PILL, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(i ? C_CARD_P : C_ACCENT_UI), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            press_motion(b);
            lv_obj_set_style_transform_scale(b, 245, LV_STATE_PRESSED);
            lv_obj_t *l = text(b, i ? "Turn off now" : "Stay on", &ss_semibold_23, i ? C_TEXT : sl_ink_on(C_ACCENT_UI));
            lv_obj_center(l);
            on_click(b, car_off_cb, (void *)(intptr_t)i);
        }
    }
    lv_obj_remove_flag(car.off, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(car.off);
}
static void car_off_count(void)
{
    int left = car.off_at ? (int)((int32_t)(car.off_at - sl_ms()) + 999) / 1000 : 0;
    char b[96];
    snprintf(b, sizeof b, sl_tr("The music is paused. The player turns off in %d s."), LV_MAX(0, left));
    if (car.off_count) label_if_changed(car.off_count, b);
}

static void car_timer_cb(lv_timer_t *t)
{
    (void)t;
    car_refresh(false);
    if (car.off_at)
    {
        car_off_count();
        if ((int32_t)(sl_ms() - car.off_at) >= 0)
        {
            car.off_at = 0;
            sl_log("car: no power for %d s, turning off", CAR_OFF_S);
            sl_poweroff();
        }
    }
    if (car.resume_at && (int32_t)(sl_ms() - car.resume_at) >= 0)
    {
        car.resume_at = 0;
        if (!is_playing()) { sl_log("car: power came, playing"); toggle_play(); }
        car_refresh(false);
    }
    if (!car_shown() && !car.off_at && !car.resume_at) lv_timer_pause(car.timer);
}
static void car_timer_go(void)
{
    if (!car.timer) car.timer = lv_timer_create(car_timer_cb, 1000, NULL);
    lv_timer_resume(car.timer);
}

/* From the status tick: the charger came or went (first: whether one was
 * in when the player started) */
static void car_power(bool charging, bool first)
{
    if (!car_on()) return;
    if (charging)
    {
        car.off_at = 0;
        if (car_off_shown()) lv_obj_add_flag(car.off, LV_OBJ_FLAG_HIDDEN);
        if (car_part(CAR_NO_PLAY) && !is_playing()) car.resume_at = sl_ms() + (first ? 1000u : CAR_RESUME_MS);
        if (car_part(CAR_NO_BIG))
        {
            if (sl_power_running && sl_power.mode != SL_POWER_ACTIVE)
            {
                sl_power_state_set(&sl_power, SL_POWER_ACTIVE, sl_ms());
                sl_power_apply();
            }
            car_show();
        }
        car_timer_go();
        sl_log("car: power came");
        return;
    }
    if (first) return;
    car.resume_at = 0;
    if (is_playing()) { audio_pause(); sl_paused_at = sl_ms(); }
    car_hide();
    if (car_part(CAR_NO_OFF))
    {
        car.off_at = sl_ms() + CAR_OFF_S * 1000u;
        if (sl_power_running && sl_power.mode != SL_POWER_ACTIVE)
        {
            sl_power_state_set(&sl_power, SL_POWER_ACTIVE, sl_ms());   /* lit, so the question is seen */
            sl_power_apply();
        }
        car_off_show();
        car_off_count();
        car_timer_go();
    }
    sl_log("car: power went");
}
/* from the power tick: lit while on the car's power with the big screen */
static bool car_keeps_lit(void)
{
#if CONFIG_CHARGING
    return car_part(CAR_NO_BIG) && charger_inserted();
#else
    return false;
#endif
}

/* ------------------------------------------------------------ Settings */
static const char *set_v_car(void) { return car_on() ? "On" : "Off"; }
static lv_obj_t *car_parts_box;
static void car_parts_deleted(lv_event_t *e) { (void)e; car_parts_box = NULL; }
static void car_set_bit(int bit, bool on)
{
    global_settings.sloppy_car = on ? global_settings.sloppy_car | bit : global_settings.sloppy_car & ~bit;
}
static void car_set_on(int on)
{
    car_set_bit(CAR_ON, on);
    if (car_parts_box) lv_obj_update_flag(car_parts_box, LV_OBJ_FLAG_HIDDEN, !on);
    if (!on) { car_hide(); car.resume_at = 0; }
}
static void car_set_play(int on) { car_set_bit(CAR_NO_PLAY, !on); }
static void car_set_big(int on) { car_set_bit(CAR_NO_BIG, !on); }
static void car_set_off(int on) { car_set_bit(CAR_NO_OFF, !on); }
static void car_open_cb(lv_event_t *e) { (void)e; car_show(); }
static void car_page(void)
{
    lv_obj_t *p, *body = page_new("Car mode", &p);
    set_switch_ex(set_group(body, NULL), LV_SYMBOL_CHARGE, 0x2d7ff0, "Car mode",
                  "For a player on a car's USB power", car_on(), car_set_on);
    car_parts_box = box(body);
    lv_obj_set_size(car_parts_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(car_parts_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(car_parts_box, car_parts_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(car_parts_box, "When the power comes");
    set_switch(g, "Play", "Where it stopped, 3 s after the car starts", !(global_settings.sloppy_car & CAR_NO_PLAY), car_set_play);
    set_switch(g, "Big controls", "Large buttons, lit while the car powers it", !(global_settings.sloppy_car & CAR_NO_BIG), car_set_big);
    g = set_group(car_parts_box, "When the power goes");
    set_switch(g, "Turn off", "The music pauses; the player turns off 30 s later unless you stay", !(global_settings.sloppy_car & CAR_NO_OFF), car_set_off);
    g = set_group(car_parts_box, NULL);
    set_action(g, LV_SYMBOL_PLAY, 0xe8408a, "Open the big controls", NULL, car_open_cb, NULL);
    lv_obj_update_flag(car_parts_box, LV_OBJ_FLAG_HIDDEN, !car_on());
    page_push(p);
}
