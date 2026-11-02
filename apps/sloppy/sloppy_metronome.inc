/* Metronome: a tempo from 30 to 250 beats a minute, set with - and +, by
 * tapping it in, or by name. The bar's beats light in a row, the first a
 * little louder and higher. The beat runs on LVGL's tick and catches up
 * from the time it should have sounded, so a slow frame never drags the
 * tempo. Included by sloppy.c after the shared helpers. */
#define MN_MIN 30
#define MN_MAX 250
#define MN_DOT 24
static lv_obj_t *mn_page_obj, *mn_bpm_label, *mn_name_label, *mn_dots_row, *mn_go, *mn_go_label;
static lv_obj_t *mn_sigs[4];
static lv_timer_t *mn_timer;
static int mn_bpm = 120, mn_beats = 4, mn_beat, mn_played;
static bool mn_running;
static uint32_t mn_next, mn_taps[4];
static int mn_n_taps;

static const char *mn_tempo_name(int bpm)
{
    return bpm < 60 ? "Largo" : bpm < 76 ? "Adagio" : bpm < 108 ? "Andante" : bpm < 120 ? "Moderato"
         : bpm < 168 ? "Allegro" : "Presto";
}
static void mn_show_bpm(void)
{
    char b[8];
    snprintf(b, sizeof b, "%d", mn_bpm);
    lv_label_set_text(mn_bpm_label, b);
    lv_label_set_text(mn_name_label, sl_case(mn_tempo_name(mn_bpm)));
}
static void mn_set_bpm(int bpm)
{
    mn_bpm = bpm < MN_MIN ? MN_MIN : bpm > MN_MAX ? MN_MAX : bpm;
    mn_show_bpm();
}
static void mn_dot_scale(void *o, int32_t v) { lv_obj_set_style_transform_scale(o, v, 0); }
/* the bar's dots: the beat just played in the accent, swelling and settling */
static void mn_light(int beat)
{
    for (int i = 0; i < (int)lv_obj_get_child_count(mn_dots_row); i++)
    {
        lv_obj_t *d = lv_obj_get_child(mn_dots_row, i);
        bool on = i == beat;
        lv_obj_set_style_bg_color(d, lv_color_hex(on ? C_ACCENT : C_CARD_P), 0);
        if (!on) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, d);
        lv_anim_set_values(&a, i ? 320 : 380, 256);
        lv_anim_set_duration(&a, SL_T_PAGE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, mn_dot_scale);
        lv_anim_start(&a);
    }
}
static void mn_build_dots(void)
{
    lv_obj_clean(mn_dots_row);
    for (int i = 0; i < mn_beats; i++)
    {
        lv_obj_t *d = box(mn_dots_row);
        lv_obj_set_size(d, MN_DOT, MN_DOT);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_CARD_P), 0);
        lv_obj_set_style_transform_pivot_x(d, MN_DOT / 2, 0);
        lv_obj_set_style_transform_pivot_y(d, MN_DOT / 2, 0);
        if (!i)                              /* the bar's first beat wears a ring */
        {
            lv_obj_set_style_border_width(d, 2, 0);
            lv_obj_set_style_border_color(d, lv_color_hex(C_ACCENT), 0);
        }
    }
}
static void mn_tick(lv_timer_t *t)
{
    (void)t;
    if (!mn_running) return;
    uint32_t now = lv_tick_get(), step = 60000u / (unsigned)mn_bpm;
    if ((int32_t)(now - mn_next) < 0) return;
    if (now - mn_next > step) mn_next = now;   /* a long stall: start the bar over, never a burst */
    sfx_play_ex(SFX_TAP, mn_beat ? 100 : 140, mn_beat ? 70 : 100);
    mn_light(mn_beat);
    mn_played++;
    mn_beat = (mn_beat + 1) % mn_beats;
    mn_next += step;
}
static void mn_paint_go(void)
{
    lv_label_set_text(mn_go_label, sl_tr(mn_running ? LV_SYMBOL_PAUSE "  Stop" : LV_SYMBOL_PLAY "  Start"));
}
static void mn_go_cb(lv_event_t *e)
{
    (void)e;
    mn_running = !mn_running;
    if (mn_running)
    {
        mn_beat = 0;
        mn_next = lv_tick_get();
        lv_timer_resume(mn_timer);
        mn_tick(NULL);                       /* the first beat on the tap, not a frame later */
    }
    else
    {
        lv_timer_pause(mn_timer);
        mn_light(-1);
    }
    mn_paint_go();
}
static void mn_step_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED_REPEAT) d *= 5;   /* held: in fives */
    mn_set_bpm(mn_bpm + d);
}
/* Tap tempo: the last few taps, averaged; a pause of two seconds starts over */
static void mn_tap_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = lv_tick_get();
    if (mn_n_taps && now - mn_taps[mn_n_taps - 1] > 2000) mn_n_taps = 0;
    if (mn_n_taps == 4) { memmove(mn_taps, mn_taps + 1, 3 * sizeof mn_taps[0]); mn_n_taps = 3; }
    mn_taps[mn_n_taps++] = now;
    if (mn_n_taps >= 2)
    {
        uint32_t span = mn_taps[mn_n_taps - 1] - mn_taps[0];
        if (span) mn_set_bpm((int)((60000u * (unsigned)(mn_n_taps - 1) + span / 2) / span));
    }
    sfx_play_ex(SFX_TAP, 120, 60);
}
static const int mn_sig_beats[4] = { 2, 3, 4, 6 };
static void mn_paint_sigs(void)
{
    for (int i = 0; i < 4; i++)
    {
        bool on = mn_sig_beats[i] == mn_beats;
        lv_obj_set_style_bg_color(mn_sigs[i], lv_color_hex(on ? C_ACCENT : C_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(mn_sigs[i], 0), lv_color_hex(on ? sl_accent_ink() : C_TEXT), 0);
    }
}
static void mn_sig_cb(lv_event_t *e)
{
    mn_beats = (int)(intptr_t)lv_event_get_user_data(e);
    mn_beat = 0;
    mn_build_dots();
    mn_paint_sigs();
}
/* a round button with a symbol, the size the music controls use */
static lv_obj_t *mn_round(lv_obj_t *parent, const char *sym, int d)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 72, 72);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    hairline(b);
    press_motion(b);
    press_sink(b, 72, 72);
    lv_obj_center(text(b, sym, &sl_sym_28, 0xffffff));
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, click_drag_guard, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, mn_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    lv_obj_add_event_cb(b, mn_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)d);
    return b;
}
/* a pill: filled in the accent for the main action, a card for the rest */
static lv_obj_t *mn_pill(lv_obj_t *parent, const char *label, int w, int h, bool primary, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CARD), 0);
    lv_obj_set_style_bg_color(b, primary ? lv_color_darken(lv_color_hex(C_ACCENT), 50) : lv_color_hex(C_CARD_P),
                              LV_STATE_PRESSED);
    if (!primary) hairline(b);
    press_motion(b);
    press_sink(b, w, h);
    lv_obj_t *l = text(b, sl_case(label), &ss_semibold_23, primary ? sl_accent_ink() : 0xffffff);
    sl_fit(l);                           /* inside the pill at any text size */
    lv_obj_center(l);
    on_click(b, cb, ud);
    return b;
}
static void mn_deleted(lv_event_t *e)
{
    (void)e;
    if (mn_timer) lv_timer_delete(mn_timer);
    mn_timer = NULL;
    mn_running = false;
    mn_page_obj = NULL;
}
static void metronome_page(void)
{
    if (page_bring_back(mn_page_obj)) return;
    lv_obj_t *p, *body = page_new("Metronome", &p);
    mn_page_obj = p;
    lv_obj_add_event_cb(p, mn_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);
    int iw = W - 28;                         /* the body's width inside its 14 px gutters */

    mn_name_label = text(body, "", &ss_semibold_21, C_SUB);
    lv_obj_set_width(mn_name_label, iw);
    lv_obj_set_style_text_align(mn_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mn_name_label, 0, 14);
    mn_bpm_label = lv_label_create(body);
    lv_obj_set_style_text_font(mn_bpm_label, &sl_count_136, 0);
    lv_obj_set_style_text_color(mn_bpm_label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(mn_bpm_label, iw);
    lv_obj_set_style_text_align(mn_bpm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mn_bpm_label, 0, 44);
    lv_obj_t *unit = text(body, sl_zune() ? "beats a minute" : "BEATS A MINUTE", &ss_semibold_18, C_DIM);
    lv_obj_set_style_text_letter_space(unit, 1, 0);
    lv_obj_set_width(unit, iw);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(unit, 0, 196);

    mn_dots_row = box(body);
    lv_obj_set_size(mn_dots_row, iw, MN_DOT + 16);
    lv_obj_set_pos(mn_dots_row, 0, 238);
    lv_obj_set_flex_flow(mn_dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mn_dots_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mn_dots_row, 18, 0);
    mn_build_dots();

    lv_obj_t *row = box(body);
    lv_obj_set_size(row, iw, 80);
    lv_obj_set_pos(row, 0, 300);
    lv_obj_align(mn_round(row, LV_SYMBOL_MINUS, -1), LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_align(mn_pill(row, "Tap", 196, 72, false, mn_tap_cb, NULL), LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(mn_round(row, LV_SYMBOL_PLUS, 1), LV_ALIGN_RIGHT_MID, -20, 0);

    static const char *const sig_names[4] = { "2/4", "3/4", "4/4", "6/8" };
    int sw = (iw - 3 * 10) / 4;
    for (int i = 0; i < 4; i++)
    {
        mn_sigs[i] = mn_pill(body, sig_names[i], sw, 52, false, mn_sig_cb, (void *)(intptr_t)mn_sig_beats[i]);
        lv_obj_set_pos(mn_sigs[i], i * (sw + 10), 404);
    }
    mn_paint_sigs();

    mn_go = mn_pill(body, "", iw, 2 * SL_R_PILL + 8, true, mn_go_cb, NULL);
    lv_obj_set_pos(mn_go, 0, 486);
    mn_go_label = lv_obj_get_child(mn_go, 0);
    mn_paint_go();

    mn_timer = lv_timer_create(mn_tick, 5, NULL);
    lv_timer_pause(mn_timer);
    mn_running = false;
    mn_show_bpm();
    page_push(p);
}
