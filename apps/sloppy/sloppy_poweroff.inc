/*
 *  Slide to power off: hold the power button and the screen frosts over,
 *  whatever is on it (the lock screen too), with a liquid glass slider on
 *  top. Slide the knob across to turn the player off; Cancel, a press of
 *  the power button or half a minute untouched puts things back.
 *
 *  The glass is Control Center's (qp_glass_capture): the screen shrunk,
 *  blurred and tinted, grown back once, so the backdrop is a still
 *  picture. On it the track is a pane of lighter glass with a bright rim,
 *  a sheen across its top and a shadow along its bottom; the words have a
 *  highlight sweeping through them. The knob is a drop of brighter glass:
 *  dragged fast it stretches along the track and narrows, and let go short
 *  of the end it springs back, stretched by its speed, round again at rest.
 *
 *  Holding the button on for SL_POWER_FORCE_MS still turns the player off
 *  without any of this, for a touch screen that has stopped answering.
 */

#define PO_TRACK_W  432
#define PO_TRACK_H  96                   /* thick enough to take a thumb and look it */
#define PO_KNOB     84
#define PO_INSET    6
#define PO_TRAVEL   (PO_TRACK_W - PO_KNOB - 2 * PO_INSET)
#define PO_IDLE_MS  30000
#define PO_M        16                   /* the panes' shadows, round them */
#define PO_KM       8                    /* the knob's */
#define PO_GROW     10                   /* held, the knob swells by this */
#define PO_X        ((W - PO_TRACK_W) / 2)
#define PO_Y        132
#define PO_CANCEL   78
#define PO_CX       ((W - PO_CANCEL) / 2)
#define PO_CY       (H - 150 - PO_CANCEL)
#define PO_STRIPS   8                    /* the dark, in strips across */
#define PO_DARK_STEP 2                   /* the dark shown moves at most this a frame */

static struct
{
    lv_obj_t *root, *track, *knob, *words, *shine, *shine_words, *lens, *glint, *dim, *cancel, *cancel_label, *rim;
    lv_obj_t *sharp, *frost;             /* the screen as it was, and frosted */
    lv_timer_t *timer;
    int x, grab, w;                      /* knob: travelled, where it was taken, width */
    int grow, lens_opa, kx, ky, kw, kh;  /* held: swelling, how much glass; where it is */
    int dim0, dark;                      /* the dim at rest; the dark the slide adds to it */
    bool predim;                         /* the dim at rest is in the frost itself (po_offer) */
    int fade, words_opa;                 /* how much of it there is, coming and going; the words' */
    lv_obj_t *strip[PO_STRIPS];          /* the dim, in strips: each frame brings one to the dark */
    int strip_dark[PO_STRIPS], shown, next_strip;   /* each strip's; the dark they are being brought to */
    lv_timer_t *sweep;
    struct sl_lens_cache lens_cache;     /* the knob's shape, while it keeps it */
    uint8_t *track_buf, *words_buf, *cancel_buf, *knob_buf;   /* the glass, drawn */
    uint8_t *track_show;                 /* the track as shown: with alpha, over the real backdrop */
    uint8_t *rim_buf;                    /* its rim alone, faded out along the slide */
    int32_t last_x, vx;                  /* pointer x last frame, its speed (px/s) */
    int32_t spring_v, spring_speed;      /* let go: the speed it went back from, and goes at now */
    int spring_w;                        /* and the stretch it had */
    uint32_t last_at, touched_at;
    bool dragging, going, off;           /* off: let go at the end; it is turning off */
    bool lens_dirty;                     /* the knob moved: its lens is drawn again before the frame */
    bool heavy;                          /* this frame redraws the track already: no strip with it */
    uint32_t frames, swept;              /* frames drawn; at the sweep's last look */
} po;

static bool po_is_open(void) { return po.root != NULL; }
static void po_refr_start(lv_event_t *e);
static bool po_sweep_step(void);

static void po_close_now(void)
{
    if (!po.root) return;
    if (po.timer) lv_timer_delete(po.timer);
    if (po.sweep) lv_timer_delete(po.sweep);
    lv_display_remove_event_cb_with_user_data(disp, po_refr_start, NULL);
    bool off = po.off || po_power_off;
    lv_anim_delete(po.root, NULL);
    lv_anim_delete(po.knob, NULL);
    lv_anim_delete(po.shine, NULL);
    lv_obj_delete(po.root);
    free(po.track_buf); free(po.words_buf); free(po.cancel_buf); free(po.knob_buf); free(po.track_show); free(po.rim_buf);
    sl_lens_cache_free(&po.lens_cache);
    memset(&po, 0, sizeof po);
    qp_glass_at = 0;                     /* Control Center takes its own next time */
    if (!off) nbanner_resume();          /* what came while it was up */
}

/* the dim over the frosted screen, with the dark of the slide, as much as
 * there is of it */
static lv_opa_t po_dim_opa(int dark)
{
    int total = po.dim0 + (255 - po.dim0) * dark / 255;
    if (po.predim && dark)
    {
        /* only the slide's dark: the rest is in the frost. LVGL mixes in 32
         * steps, so this step is picked to land where the one dark of
         * old did: (32 - in frost) x (32 - this) = 32 x (32 - all of it) */
        int qt = (total + 4) >> 3, q0 = (po.dim0 + 4) >> 3;
        int qx = 32 - ((32 - qt) * 32 + (32 - q0) / 2) / (32 - q0);
        total = qx <= 0 ? 0 : qx >= 32 ? 255 : qx * 8;
    }
    else if (po.predim) total = 0;
    return (lv_opa_t)(total * po.fade / 255);
}
/* Cancel, the one thing over the dim, fades with the dark in steps of 16 */
static void po_cancel_fade(void)
{
    int v = (255 - (po.shown + 8) / 16 * 16) * po.fade / 255;
    if (lv_obj_get_style_opa(po.cancel, 0) == v) return;
    lv_obj_set_style_opa(po.cancel, (lv_opa_t)v, 0);
    lv_obj_set_style_opa(po.cancel_label, (lv_opa_t)v, 0);
}
/* Coming and going, each part fades by itself: one layer the size of the
 * screen made every frame of it twice as slow as the R1 can draw. Coming,
 * the frost fades in over the screen as it was, still, so the page under
 * it is not drawn at all; going, over the page as it is now. */
static void po_set_fade(void *o, int32_t v)
{
    (void)o;
    po.fade = v;
    if (po.frost) lv_obj_set_style_image_opa(po.frost, (lv_opa_t)v, 0);
    for (int i = 0; i < PO_STRIPS; i++) lv_obj_set_style_bg_opa(po.strip[i], po_dim_opa(po.strip_dark[i]), 0);
    lv_obj_set_style_opa(po.track, (lv_opa_t)v, 0);
    po_cancel_fade();
}
static void po_gone_async(void *u) { (void)u; po_close_now(); }
static void po_gone(lv_anim_t *a) { (void)a; lv_async_call(po_gone_async, NULL); }   /* not from inside its own animation */
static void po_close(void)
{
    if (!po.root || po.going || po.off) return;   /* a finished slide is not taken back */
    po.going = true;
    if (po.sharp) lv_obj_add_flag(po.sharp, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, po.root);
    lv_anim_set_exec_cb(&a, po_set_fade);
    lv_anim_set_values(&a, po.fade, 0);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, po_gone);
    lv_anim_start(&a);
}

/* Held, the knob turns to clear glass, as a slider's does: the track and
 * its words under it a little larger, bent in round its rim. */
static void po_lens_draw(void)
{
    if (!po.knob_buf || !po.lens_opa) return;
    int tw = PO_TRACK_W + 2 * PO_M, th = PO_TRACK_H + 2 * PO_M;
    struct sl_lens_src src = { (const uint16_t *)po.track_buf, tw, tw, th, PO_X - PO_M, PO_Y - PO_M, 256,
                               (const uint16_t *)po.words_buf, po.words_opa };
    const struct sl_lens_look *k = &sl_lens_clear;
    int cw = po.kw + 2 * PO_KM, ch = po.kh + 2 * PO_KM;
    sl_lens_moved(&po.lens_cache, &src, PO_X + po.kx - PO_KM, PO_Y + po.ky - PO_KM, cw, ch, PO_KM, po.kh / 2.0f, k,
                  (uint16_t *)po.knob_buf, po.knob_buf + (size_t)cw * ch * 2);
    lv_canvas_set_buffer(po.lens, po.knob_buf, cw, ch, LV_COLOR_FORMAT_RGB565A8);
    lv_obj_set_pos(po.lens, -PO_KM, -PO_KM);
    lv_obj_invalidate(po.lens);
}
/* once a frame, however many times the knob was placed in it (the springs
 * and the glass each place it) */
static void po_refr_start(lv_event_t *e)
{
    (void)e;
    po.frames++;
    if (po.sweep && !po.heavy && !lv_timer_get_paused(po.sweep) && !po_sweep_step()) lv_timer_pause(po.sweep);
    po.heavy = false;
    if (!po.lens_dirty) return;
    po.lens_dirty = false;
    po_lens_draw();
}

/* The dark covers the whole screen, and the R1 cannot draw a whole screen
 * in a frame (about 55 ms with the glass): changed at once, every step of
 * it held the knob still for three frames. So the dim is in strips across,
 * and each frame brings the next strip down to the dark shown, which
 * moves towards the knob's at most PO_DARK_STEP a frame: a frame draws a
 * strip and the knob, and no two strips are more than a sweep's worth of
 * steps apart (14 of 255), too little to see. All the way dark takes two
 * seconds. */
static bool po_sweep_step(void)
{
    int d = po.dark - po.shown;
    po.shown += d > PO_DARK_STEP ? PO_DARK_STEP : d < -PO_DARK_STEP ? -PO_DARK_STEP : d;
    po_cancel_fade();
    for (int n = 0; n < PO_STRIPS; n++)
    {
        int i = po.next_strip;
        po.next_strip = (i + 1) % PO_STRIPS;
        if (po.strip_dark[i] == po.shown) continue;
        po.strip_dark[i] = po.shown;
        lv_obj_set_style_bg_opa(po.strip[i], po_dim_opa(po.shown), 0);
        return true;
    }
    return po.shown != po.dark;
}
/* The step is taken as a frame starts (po_refr_start), unless the frame
 * already redraws the track (a step of the words or the rim): one or the
 * other, not both. With nothing else drawing, the timer takes it. */
static void po_sweep(lv_timer_t *t)
{
    if (po.frames != po.swept) { po.swept = po.frames; return; }
    if (!po_sweep_step()) lv_timer_pause(t);   /* all of it there */
}

/* The knob at x along the track, w wide: the words fade as it covers them,
 * the track's rim fades out over the whole way, and everything but the
 * slider goes dark with it, as on the phone, so at the end only the slider
 * is lit; let go short and it all comes back.
 * The dark is the dim under the slider made deeper, not a second layer
 * to blend. */
static void po_place(int x, int w)
{
    po.x = x < 0 ? 0 : x > PO_TRAVEL ? PO_TRAVEL : x;
    po.w = w;
    int ww = w + 2 * po.grow, h = PO_KNOB + po.grow - (w - PO_KNOB) / 3;  /* held, a capsule; stretched, it narrows */
    if (ww != po.kw || h != po.kh)
    {
        lv_obj_set_size(po.knob, ww, h);
        lv_obj_set_style_radius(po.knob, SL_R_FULL, 0);
    }
    /* it stretches from its middle, but never past either end of the track */
    int left = PO_INSET + po.x - (ww - PO_KNOB) / 2;
    if (left < PO_INSET - po.grow / 2) left = PO_INSET - po.grow / 2;
    if (left + ww > PO_TRACK_W - PO_INSET + po.grow / 2) left = PO_TRACK_W - PO_INSET + po.grow / 2 - ww;
    po.kx = left; po.ky = (PO_TRACK_H - h) / 2; po.kw = ww; po.kh = h;
    lv_obj_set_pos(po.knob, left, po.ky);
    /* the words and the rim span the track: each change to them redraws
     * all of it, so they too fade in 16 steps, set only when they change.
     * Their own text and image opacity, not the object's: that drew each
     * through a layer of its own, every frame. */
    int fade = 255 - po.x * 255 * 2 / PO_TRAVEL;
    fade = fade <= 0 ? 0 : (fade + 8) / 16 * 16 > 255 ? 255 : (fade + 8) / 16 * 16;
    if (po.words_opa != fade)
    {
        po.heavy = true;
        po.words_opa = fade;
        lv_obj_set_style_text_opa(po.words, (lv_opa_t)(150 * fade / 255), 0);
        lv_obj_set_style_text_opa(po.shine_words, (lv_opa_t)fade, 0);
        lv_obj_update_flag(po.shine, LV_OBJ_FLAG_HIDDEN, fade == 0);   /* its sweep redraws nothing */
    }
    if (po.rim)
    {
        int rim = 255 - 255 * po.x / PO_TRAVEL;
        rim = (rim + 8) / 16 * 16 > 255 ? 255 : (rim + 8) / 16 * 16;
        if (lv_obj_get_style_image_opa(po.rim, 0) != rim)
        {
            po.heavy = true;
            lv_obj_set_style_image_opa(po.rim, (lv_opa_t)rim, 0);
            lv_obj_update_flag(po.rim, LV_OBJ_FLAG_HIDDEN, rim == 0);
        }
    }
    int dark = 236 * po.x / PO_TRAVEL;
    if (po.sweep && dark != po.dark)
    {
        po.dark = dark;
        lv_timer_resume(po.sweep);
    }
    po.lens_dirty = true;
}

/* Glass coming over the white drop, and the drop swelling, as it is held */
static void po_set_glass(void *o, int32_t v)
{
    (void)o;
    po.lens_opa = v;
    po.grow = PO_GROW * v / 255;
    lv_obj_set_style_bg_opa(po.knob, (lv_opa_t)(242 * (255 - v) / 255), 0);
    lv_obj_set_style_shadow_opa(po.knob, (lv_opa_t)(48 * (255 - v) / 255), 0);
    lv_obj_set_style_bg_main_opa(po.glint, (lv_opa_t)(200 * (255 - v) / 255), 0);
    lv_obj_update_flag(po.glint, LV_OBJ_FLAG_HIDDEN, v == 255);
    if (po.lens)
    {
        lv_obj_set_style_image_opa(po.lens, (lv_opa_t)v, 0);
        lv_obj_update_flag(po.lens, LV_OBJ_FLAG_HIDDEN, v == 0);
    }
    po_place(po.x, po.w);
}
static void po_glass(bool on)
{
    if (!po.knob_buf) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, po.lens);
    lv_anim_set_exec_cb(&a, po_set_glass);
    lv_anim_set_values(&a, po.lens_opa, on ? 255 : 0);
    lv_anim_set_duration(&a, on ? SL_T_FAST : SL_T_PAGE);
    lv_anim_set_path_cb(&a, on ? lv_anim_path_ease_out : lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void po_spring_x(void *o, int32_t v) { (void)o; po_place(v, po.w); }

/* Let go short of the end, the knob goes back as a spring takes it:
 * critically damped, from the speed the finger had back towards the
 * start (none on towards the end: where it was let go decides), so it
 * neither jumps at the start nor rings at the end, and never goes past
 * the start. It stretches with its own speed as it did with the finger's,
 * the stretch it was let go with easing off, and is round at rest. */
#define PO_SPRING    18.0f               /* per second */
#define PO_SPRING_MS 480                 /* by then within half a pixel */
static int po_stretch(int32_t speed)
{
    speed = speed < 0 ? -speed : speed;
    return speed / 45 > 26 ? 26 : speed / 45;
}
static void po_spring_back(lv_anim_t *a, int32_t v)
{
    (void)v;
    float t = a->act_time / 1000.0f, x0 = (float)a->start_value, c = po.spring_v + PO_SPRING * x0;
    float e = (float)exp(-PO_SPRING * t), x = (x0 + c * t) * e;   /* not expf: newer than the R1's glibc */
    po.spring_speed = (int32_t)((c - PO_SPRING * (x0 + c * t)) * e);
    int stretch = po_stretch(po.spring_speed), held = (int)(po.spring_w * e + 0.5f);
    if (a->act_time >= (int32_t)a->duration) x = 0, po.spring_speed = 0, stretch = held = 0;
    po_place((int)(x + 0.5f), PO_KNOB + (held > stretch ? held : stretch));
}

static void po_shine_x(void *o, int32_t v)
{
    lv_obj_set_x(o, v);
    lv_obj_set_x(po.shine_words, -v);    /* the bright copy stays over the words */
}

/* Turned off from the main loop (sl_power_tick), not from in here: the
 * goodbye screen draws with lv_timer_handler, which does nothing when
 * called from inside its own timers, so it stood still for its whole
 * chime. The slider stays up until then, so nothing shows in between. */
static void po_off_async(void *u)
{
    (void)u;
    po_power_off = true;
}
static void po_off(lv_anim_t *a) { (void)a; lv_async_call(po_off_async, NULL); }

static void po_knob_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt = { 0, 0 };
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &pt);
    if (po.off || po.going) return;      /* gliding to the end, or going away: nothing more to say */
    po.touched_at = lv_tick_get();
    sl_power.activity_at = sl_ms();      /* sliding keeps the screen awake */
    if (code == LV_EVENT_PRESSED)
    {
        lv_anim_delete(po.knob, NULL);
        po.dragging = true;
        po.grab = pt.x - po.x;
        po.last_x = pt.x;
        po.last_at = lv_tick_get();
        po.vx = po.spring_speed;         /* caught going back: its stretch eases off, not at once */
        po.spring_speed = 0;
        po_glass(true);
        sfx_play(SFX_UI_PICKUP);
        if (!sl_perf.name) sl_perf_begin("power slide");   /* the drag's frames, in sloppy.log */
    }
    else if (code == LV_EVENT_PRESSING && po.dragging)
    {
        uint32_t dt = lv_tick_elaps(po.last_at);
        if (dt)
        {
            /* smoothed by time, not by reading: the reads come unevenly */
            int32_t v = (pt.x - po.last_x) * 1000 / (int32_t)dt;
            po.vx += (v - po.vx) * (int32_t)dt / ((int32_t)dt + 24);
        }
        po.last_x = pt.x;
        po.last_at = lv_tick_get();
        po_place(pt.x - po.grab, PO_KNOB + po_stretch(po.vx));
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && po.dragging)
    {
        po.dragging = false;
        if (sl_perf.name && !strcmp(sl_perf.name, "power slide")) sl_perf_end();
        /* only a finger let go at the end turns it off: a contact taken
         * away (PRESS_LOST) goes back like an early release */
        bool off = code == LV_EVENT_RELEASED && po.x >= PO_TRAVEL * 9 / 10;
        po.off = off;
        if (!off) po_glass(false);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, po.knob);
        lv_anim_set_values(&a, po.x, off ? PO_TRAVEL : 0);
        if (off)
        {
            lv_anim_set_exec_cb(&a, po_spring_x);
            lv_anim_set_duration(&a, SL_T_FAST);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_completed_cb(&a, po_off);
        }
        else
        {
            /* flung back faster than it could stop at the start, it stops there */
            int32_t floor_v = -(int32_t)(PO_SPRING * po.x);
            po.spring_v = po.vx < floor_v ? floor_v : po.vx > 0 ? 0 : po.vx;
            po.spring_w = po.w - PO_KNOB;
            lv_anim_set_custom_exec_cb(&a, po_spring_back);
            lv_anim_set_duration(&a, PO_SPRING_MS);
        }
        po.vx = 0;
        lv_anim_start(&a);
        sfx_play(off ? SFX_UI_LOCK : SFX_UI_DROP);
    }
}

static void po_cancel_cb(lv_event_t *e) { (void)e; sfx_play(SFX_UI_BACK); po_close(); }

static void po_tick(lv_timer_t *t)
{
    (void)t;
    if (sl_power.mode == SL_POWER_DARK || lv_tick_elaps(po.touched_at) > PO_IDLE_MS) po_close();
}

/* The screen, sharp or frosted, hides all under it while it is all there:
 * LVGL draws from it, not from the page below (no image says it covers) */
static void po_cover_cb(lv_event_t *e)
{
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if (info->res != LV_COVER_RES_MASKED && lv_obj_get_style_image_opa(lv_event_get_current_target(e), 0) >= LV_OPA_MAX)
        info->res = LV_COVER_RES_COVER;
}

/* Hold the power button: offer to turn off. */
static void po_offer(void)
{
    if (po.root) return;
    cf_close();                          /* the sheet is portrait: Cover Flow and Spectrum close */
    qp_glass_capture();                  /* what is on screen now, frosted */
    if (qp_glass_ok) qp_glass_grow(H);
    sfx_play(SFX_UI_PANEL_OPEN);
    po.root = box(lv_layer_sys());
    lv_obj_set_size(po.root, W, H);
    lv_obj_add_flag(po.root, LV_OBJ_FLAG_CLICKABLE);   /* nothing under it takes a touch */
    lv_obj_remove_flag(po.root, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (qp_glass_ok)
    {
        lv_image_set_src(po.sharp = lv_image_create(po.root), &qp_sharp_img);
        lv_image_set_src(po.frost = lv_image_create(po.root), &qp_glass_img);
        lv_obj_add_event_cb(po.sharp, po_cover_cb, LV_EVENT_COVER_CHECK, NULL);
        lv_obj_add_event_cb(po.frost, po_cover_cb, LV_EVENT_COVER_CHECK, NULL);
    }
    lv_obj_t *dim = po.dim = box(po.root);   /* no memory for glass: plain dark */
    lv_obj_set_size(dim, W, H);
    po.dim0 = qp_glass_ok ? 96 : 210;
    po.predim = qp_glass_ok;             /* the dim at rest goes into the frost: see the end */
    for (int i = 0; i < PO_STRIPS; i++)
    {
        lv_obj_t *st = po.strip[i] = box(dim);
        lv_obj_set_size(st, W, H / PO_STRIPS);
        lv_obj_set_pos(st, 0, i * (H / PO_STRIPS));
        lv_obj_set_style_bg_color(st, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(st, po.predim ? LV_OPA_TRANSP : (lv_opa_t)po.dim0, 0);
    }
    po.sweep = lv_timer_create(po_sweep, LV_DEF_REFR_PERIOD, NULL);
    lv_timer_pause(po.sweep);
    lv_display_add_event_cb(disp, po_refr_start, LV_EVENT_REFR_START, NULL);

    /* the glass drawn from the frosted screen behind, scrim and all; with
     * no memory for that, the plain pane */
    struct sl_lens_src back = { qp_glass_px, W, W, H, 0, 0, 256 - 96 * 256 / 255, NULL, 0 };
    int tw = PO_TRACK_W + 2 * PO_M, th = PO_TRACK_H + 2 * PO_M;
    if (qp_glass_ok)
    {
        po.track_buf = malloc((size_t)tw * th * 2);
        po.track_show = malloc((size_t)tw * th * 3);
        if (!po.track_buf || !po.track_show) { free(po.track_buf); free(po.track_show); po.track_buf = po.track_show = NULL; }
    }
    if (po.track_buf)
    {
        po.track = box(po.root);
        lv_obj_set_size(po.track, PO_TRACK_W, PO_TRACK_H);
        sl_overflow(po.track, PO_M);
        struct sl_lens_look k = sl_lens_pane;
        k.lift = -0.24f;                 /* smoked, as the phone's: the white knob is the light */
        /* The rim is drawn apart, to fade along the slide: the pane without
         * it, then over it what the rim changes. Without memory for that,
         * one pane, rim and all. */
        struct sl_lens_look bare = k;
        bare.rim = bare.inner = 0;
        po.rim_buf = malloc((size_t)tw * th * 3);
        /* shown with alpha over the real backdrop; and flat, backdrop and
         * all, for the knob's lens to look through: without the rim, so the
         * lens shows none of it at the end */
        lv_obj_set_pos(sl_lens_canvas(po.track, &back, PO_X, PO_Y, PO_TRACK_W, PO_TRACK_H, PO_M,
                                      PO_TRACK_H / 2.0f, po.rim_buf ? &bare : &k, po.track_show, true), -PO_M, -PO_M);
        if (po.rim_buf)
        {
            po.rim = sl_lens_canvas(po.track, &back, PO_X, PO_Y, PO_TRACK_W, PO_TRACK_H, PO_M,
                                    PO_TRACK_H / 2.0f, &k, po.rim_buf, true);
            const uint16_t *base = (const uint16_t *)po.track_show;
            uint16_t *rgb = (uint16_t *)po.rim_buf;
            uint8_t *alpha = po.rim_buf + (size_t)tw * th * 2;
            for (int i = 0; i < tw * th; i++)
                if (rgb[i] == base[i]) alpha[i] = 0;   /* not the rim: the pane shows */
            lv_obj_set_pos(po.rim, -PO_M, -PO_M);
        }
        sl_lens(&back, PO_X - PO_M, PO_Y - PO_M, tw, th, PO_M, PO_TRACK_H / 2.0f, po.rim_buf ? &bare : &k,
                (uint16_t *)po.track_buf, NULL);
    }
    else po.track = sl_glass_pane(po.root, PO_TRACK_W, PO_TRACK_H, 38);
    lv_obj_set_pos(po.track, PO_X, PO_Y);

    /* the words, and a band of brighter words sweeping through them */
    const char *say = sl_tr("slide to power off");
    int wx = PO_INSET + PO_KNOB + 10, ww = PO_TRACK_W - wx - 12;
    po.words = lv_label_create(po.track);
    lv_label_set_text(po.words, say);
    lv_obj_set_style_text_font(po.words, &ss_semibold_26, 0);
    lv_obj_set_style_text_color(po.words, lv_color_white(), 0);
    lv_obj_set_style_text_align(po.words, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(po.words, ww);
    lv_obj_align(po.words, LV_ALIGN_LEFT_MID, wx, 0);
    label_lines(po.words, 1);
    lv_obj_update_layout(po.words);
    po.shine = box(po.words);                /* clips the bright copy to itself */
    lv_obj_set_size(po.shine, 64, lv_obj_get_height(po.words));
    lv_obj_add_flag(po.shine, LV_OBJ_FLAG_FLOATING);
    po.shine_words = lv_label_create(po.shine);
    lv_label_set_text(po.shine_words, say);
    lv_obj_set_style_text_font(po.shine_words, &ss_semibold_26, 0);
    lv_obj_set_style_text_color(po.shine_words, lv_color_white(), 0);
    lv_obj_set_style_text_align(po.shine_words, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(po.shine_words, ww);
    label_lines(po.shine_words, 1);
    /* the track with its words, for the knob's lens to look through */
    if (po.track_buf && (po.words_buf = malloc((size_t)tw * th * 2)) != NULL)
    {
        memcpy(po.words_buf, po.track_buf, (size_t)tw * th * 2);
        lv_obj_t *c = lv_canvas_create(po.root);
        lv_canvas_set_buffer(c, po.words_buf, tw, th, LV_COLOR_FORMAT_RGB565);
        lv_layer_t layer;
        lv_canvas_init_layer(c, &layer);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = say;
        d.font = &ss_semibold_26;
        d.color = lv_color_white();
        d.opa = 150;
        d.align = LV_TEXT_ALIGN_CENTER;
        int ly = PO_M + lv_obj_get_y(po.words);
        lv_area_t at = { PO_M + wx, ly, PO_M + wx + ww - 1, ly + lv_obj_get_height(po.words) - 1 };
        lv_draw_label(&layer, &d, &at);
        lv_canvas_finish_layer(c, &layer);
        lv_obj_delete(c);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, po.shine);
    lv_anim_set_exec_cb(&a, po_shine_x);
    lv_anim_set_values(&a, -80, ww + 16);
    lv_anim_set_duration(&a, 2 * SL_T_LOOP);
    lv_anim_set_repeat_delay(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* the knob: a drop of brighter glass with the power symbol in it */
    po.knob = box(po.track);
    lv_obj_set_style_bg_color(po.knob, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(po.knob, lv_color_hex(0xdfe4ea), 0);
    lv_obj_set_style_bg_grad_dir(po.knob, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(po.knob, 242, 0);
    lv_obj_set_style_shadow_width(po.knob, 16, 0);
    lv_obj_set_style_shadow_opa(po.knob, 48, 0);
    lv_obj_set_style_shadow_offset_y(po.knob, 3, 0);
    lv_obj_add_flag(po.knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(po.knob, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(po.knob, 14);
    lv_obj_add_event_cb(po.knob, po_knob_event, LV_EVENT_ALL, NULL);
    if (po.words_buf)                        /* held: a lens, drawn over the drop */
    {
        po.knob_buf = malloc((size_t)(PO_KNOB + 26 + 2 * PO_GROW + 2 * PO_KM) * (PO_KNOB + PO_GROW + 2 * PO_KM) * 3);
        if (po.knob_buf)
        {
            po.lens = lv_canvas_create(po.knob);
            lv_obj_remove_flag(po.lens, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(po.lens, LV_OBJ_FLAG_HIDDEN);
            sl_overflow(po.knob, PO_KM);    /* its size has the swelling in it; the shadow adds its own */
        }
    }
    lv_obj_t *glint = po.glint = box(po.knob);   /* where the light catches the drop */
    lv_obj_set_size(glint, 38, 15);
    lv_obj_set_style_radius(glint, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(glint, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(glint, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_dir(glint, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(glint, 200, 0);
    lv_obj_set_style_bg_grad_opa(glint, 0, 0);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_t *icon = lv_label_create(po.knob);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(icon, &sl_sym_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xff3b30), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 1);
    po_place(0, PO_KNOB);

    /* Cancel: a round pane of glass at the bottom, as on the phone */
    lv_obj_t *cancel;
    int cm = PO_CANCEL + 2 * PO_M;
    if (po.track_buf && (po.cancel_buf = malloc((size_t)cm * cm * 3)) != NULL)
    {
        cancel = box(po.root);
        lv_obj_set_size(cancel, PO_CANCEL, PO_CANCEL);
        lv_obj_set_style_radius(cancel, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
        sl_overflow(cancel, PO_M);
        struct sl_lens_look k = sl_lens_pane;
        k.lift = -0.24f;                 /* smoked, as the phone's: the white knob is the light */
        lv_obj_set_pos(sl_lens_canvas(cancel, &back, PO_CX, PO_CY, PO_CANCEL, PO_CANCEL, PO_M,
                                      PO_CANCEL / 2.0f, &k, po.cancel_buf, true), -PO_M, -PO_M);
    }
    else cancel = sl_glass_pane(po.root, PO_CANCEL, PO_CANCEL, 46);
    lv_obj_set_pos(cancel, PO_CX, PO_CY);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cancel, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_opa(cancel, 90, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(cancel, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(cancel, 39, 0);
    lv_obj_set_style_transform_pivot_y(cancel, 39, 0);
    press_motion(cancel);
    lv_obj_add_event_cb(cancel, po_cancel_cb, LV_EVENT_CLICKED, NULL);
    po.cancel = cancel;
    lv_obj_t *x = lv_label_create(cancel);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(x, &sl_sym_28, 0);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);
    lv_obj_center(x);
    lv_obj_t *cl = po.cancel_label = text(po.root, "Cancel", &ss_regular_18, 0xf2f2f2);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_align_to(cl, cancel, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_anim_init(&a);
    lv_anim_set_var(&a, po.root);
    lv_anim_set_exec_cb(&a, po_set_fade);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    po.touched_at = lv_tick_get();
    sl_power.activity_at = sl_ms();
    lv_obj_move_foreground(po.track);
    po.timer = lv_timer_create(po_tick, 500, NULL);
    if (po.predim)
    {
        /* The dim at rest put into the frost once, as the dark strips drew
         * it (LVGL's own mix, so it looks the same), now the glass above
         * has been made from the frost as it was: coming and going, each
         * frame then blends the frost alone, not the frost and then a
         * screen of dark over it. The strips keep only the dark the slide
         * adds. Control Center takes its own glass next time (po_close_now). */
        for (int i = 0; i < W * H; i++) qp_glass_px[i] = lv_color_16_16_mix(0, qp_glass_px[i], (uint8_t)po.dim0);
        lv_image_cache_drop(&qp_glass_img);
    }
}
