/*
 *  Pull to refresh, on the home screen (cards or tiles). Pull the page
 *  down past its top: a ring draws itself in the gap as it opens, closing
 *  with a tick once the pull is far enough, and the words under it say
 *  what letting go will do. Let go there and the gap holds open while the
 *  ring spins; then the home screen is built again (new covers, the tiles
 *  coming in afresh) and the music library is scanned for new songs in the
 *  background, as Settings > Update library does with its screen hidden.
 *  Off unless Settings > Customization > Home layout turns it on.
 */

#define PTR_ARM   64                     /* overscroll, px, that will refresh */
#define PTR_GAP   76                     /* the gap held open while it spins */
#define PTR_RING  34

static void home_page(void);
static void db_update_begin(bool show);
static bool mt_busy(void);

static struct
{
    lv_obj_t *page, *body, *ring, *words;
    lv_timer_t *timer;
    float progress;                      /* 0..1 of the pull, 1 while spinning */
    int spin;                            /* the spinning arc's angle */
    bool armed, refreshing;
} ptr;

static void ptr_ring_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_obj_get_style_text_color(o, 0);
    d.width = 3;
    d.rounded = 1;
    d.center.x = (a.x1 + a.x2) / 2;
    d.center.y = (a.y1 + a.y2) / 2;
    d.radius = (uint16_t)(lv_area_get_width(&a) / 2);
    d.opa = 60;                          /* the track */
    d.start_angle = 0; d.end_angle = 360;
    lv_draw_arc(lv_event_get_layer(e), &d);
    d.opa = LV_OPA_COVER;
    if (ptr.refreshing)
    {
        d.start_angle = ptr.spin; d.end_angle = ptr.spin + 100;
    }
    else
    {
        int sweep = (int)(ptr.progress * 360);
        if (sweep < 2) return;
        d.start_angle = 270; d.end_angle = 270 + (sweep > 359 ? 359 : sweep);
    }
    lv_draw_arc(lv_event_get_layer(e), &d);
}

/* The ring and the words, centred in the gap the pull has opened. */
static void ptr_show(int gap)
{
    if (!ptr.ring) return;
    bool seen = gap > 6 || ptr.refreshing;
    if (!seen)
    {
        lv_obj_add_flag(ptr.ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ptr.words, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(ptr.ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ptr.words, LV_OBJ_FLAG_HIDDEN);
    float p = gap / (float)PTR_ARM;
    ptr.progress = ptr.refreshing ? 1 : p > 1 ? 1 : p;
    int y = SB_H + gap / 2 - PTR_RING / 2 - 10;
    lv_obj_set_y(ptr.ring, y < SB_H - PTR_RING ? SB_H - PTR_RING : y);
    lv_obj_set_y(ptr.words, lv_obj_get_y(ptr.ring) + PTR_RING + 6);
    lv_opa_t opa = (lv_opa_t)(ptr.progress * 255);
    lv_obj_set_style_opa(ptr.ring, opa, 0);
    lv_obj_set_style_opa(ptr.words, gap > PTR_ARM / 2 || ptr.refreshing ? opa : 0, 0);
    const char *say = ptr.refreshing ? "Refreshing" : ptr.armed ? "Release to refresh" : "Pull to refresh";
    if (strcmp(lv_label_get_text(ptr.words), sl_tr(say))) lv_label_set_text(ptr.words, sl_tr(say));
    lv_obj_invalidate(ptr.ring);
}

static void ptr_rebuild(void *u)
{
    (void)u;
    if (npages != 1 || !ptr.page || pages[0] != ptr.page) { ptr.refreshing = false; return; }
    /* the whole home screen again: deleting it drops ptr's objects too */
    lv_obj_delete(ptr.page);
    npages = 0;
    home_icon_small = false;
    home_settings_icon = NULL;
    home_page();
    if (mini) lv_obj_move_foreground(mini);
    db_update_begin(false);              /* says so in a toast */
}

static void ptr_spin(lv_timer_t *t)
{
    ptr.spin = (ptr.spin + 24) % 360;
    lv_obj_invalidate(ptr.ring);
    if (lv_timer_get_user_data(t) && lv_tick_elaps((uint32_t)(uintptr_t)lv_timer_get_user_data(t)) > 750)
    {
        lv_timer_delete(t);
        ptr.timer = NULL;
        lv_async_call(ptr_rebuild, NULL);
    }
}

static void ptr_begin(void)
{
    ptr.armed = false;
    ptr.refreshing = true;
    sfx_play(SFX_UI_DROP);
    /* hold the gap open: the elastic pull settles on the padding instead */
    lv_obj_set_style_pad_top(ptr.body, lv_obj_get_style_pad_top(ptr.body, 0) + PTR_GAP, 0);
    lv_obj_scroll_to_y(ptr.body, 0, LV_ANIM_ON);
    ptr_show(PTR_GAP);
    ptr.timer = lv_timer_create(ptr_spin, 16, (void *)(uintptr_t)(lv_tick_get() | 1));
}

static void ptr_scroll(lv_event_t *e)
{
    (void)e;
    if (ptr.refreshing || npages != 1) return;
    /* off (Settings > Customization > Home layout), or tiles being edited:
     * the page only springs back */
    if (!global_settings.sloppy_pull_refresh || mt_busy())
    {
        if (ptr.armed || ptr.progress > 0)
        {
            ptr.armed = false;
            ptr.progress = 0;
            ptr_show(0);
        }
        return;
    }
    int over = -lv_obj_get_scroll_y(ptr.body);
    /* the finger itself: scroll events also come from layout and the
     * elastic settling, with no input device behind them */
    bool touching = touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED;
    if (touching)
    {
        bool armed = over >= PTR_ARM;
        if (armed != ptr.armed)
        {
            ptr.armed = armed;
            if (armed) sfx_play(SFX_UI_TICK);
        }
    }
    else if (ptr.armed) { ptr_begin(); return; }
    ptr_show(over > 0 ? over : 0);
}

static void ptr_deleted(lv_event_t *e)
{
    (void)e;
    if (ptr.timer) lv_timer_delete(ptr.timer);
    memset(&ptr, 0, sizeof ptr);
}

/* Give a home page its pull to refresh; ink is the colour of type on it. */
static void ptr_attach(lv_obj_t *page, lv_obj_t *body, uint32_t ink)
{
    if (ptr.timer) lv_timer_delete(ptr.timer);
    memset(&ptr, 0, sizeof ptr);
    ptr.page = page;
    ptr.body = body;
    ptr.ring = box(page);
    lv_obj_set_size(ptr.ring, PTR_RING, PTR_RING);
    lv_obj_set_x(ptr.ring, (W - PTR_RING) / 2);
    lv_obj_set_style_text_color(ptr.ring, lv_color_hex(ink), 0);
    lv_obj_add_event_cb(ptr.ring, ptr_ring_draw, LV_EVENT_DRAW_MAIN, NULL);
    ptr.words = text(page, "", &ss_semibold_18, ink);
    lv_obj_set_style_text_color(ptr.words, lv_color_hex(ink), 0);
    lv_obj_set_width(ptr.words, W);
    lv_obj_set_style_text_align(ptr.words, LV_TEXT_ALIGN_CENTER, 0);
    ptr_show(0);
    lv_obj_add_event_cb(body, ptr_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(body, ptr_scroll, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(page, ptr_deleted, LV_EVENT_DELETE, NULL);
}
