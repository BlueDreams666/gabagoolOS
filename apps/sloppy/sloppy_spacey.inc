/*
 *  Spacey Lines: a finger's trail falling into a turning tunnel. After
 *  Chris Shier's line05x (csh.bz/line/05xp.html), written again here in
 *  C for a player with no GPU.
 *
 *  The trail is a rope of 65 points, each chasing the one before it by
 *  half the gap a step, the first chasing the finger; a still finger
 *  draws it in to a dot. Its 64 segments are drawn every frame with round
 *  caps, each in a hue that turns with time, runs along the rope and
 *  jumps with the segment's speed, and thinner the faster it goes.
 *
 *  Before the trail, the last frame is drawn back onto itself twice: once
 *  zoomed in by 2 px, and again turned about the middle by pi plus the
 *  number of touches, shrunk by 8 px (an odd count) or by 32 px and a
 *  radian further (even). Old strokes spiral down into the middle. A new
 *  touch turns the tunnel the other way; a finger held in a corner fades
 *  everything towards black, a tenth a frame.
 *
 *  Both passes are one affine map each, so a frame is a single pass over
 *  the picture: per pixel a few adds in 16.16 fixed point, a bounds test
 *  and a nearest-neighbour read, as the original reads with smoothing
 *  off. The picture is half the stage each way (226 x 338), in the
 *  panel's RGB565: pixels are only moved, never mixed, but for the edges
 *  of strokes and the fade, so nothing bands, and painting has nothing
 *  to convert. It is doubled to the screen with linear smoothing straight
 *  into LVGL's buffer, as the visualizer paints; only the stage is drawn
 *  again, and where the picture covers it, not even its black card. 30
 *  frames a second at most, further apart if the work and painting take
 *  more than 3/4 of that; the trail and its colours step at the
 *  original's 60 a second whatever the rate. What frames cost goes to the
 *  log when the page closes.
 */
#define SPC_TRAIL   64          /* segments in the trail: 65 points */
#define SPC_WIDTH   12          /* a slow stroke's width, screen px; a fast one thins to 2 */
#define SPC_CORNER  20          /* px from a corner where a held finger clears */
#define SPC_KEEP    29          /* of 32: a clearing frame keeps 90 % */
#define SPC_CLEAR   30          /* frames the clear button fades over */
#define SPC_FRAME   33          /* ms between frames at least: 30 a second, as the visualizer */
#define SPC_STEP    17          /* ms a step of the trail and its colours stands for: the original's 60 Hz */
#define SPC_HINT_MS 4200        /* the hint and credit, then they fade */

static struct
{
    lv_obj_t *page, *stage, *hint;
    lv_timer_t *timer;
    uint16_t *buf[2];                    /* this picture and the last, RGB565 */
    int cur;
    int sw, sh, bw, bh;                  /* the stage in screen px; the picture, half each way */
    bool touched, down;                  /* drawn on yet; a finger on it now */
    int fx, fy;                          /* the finger, stage px */
    float px[SPC_TRAIL + 1], py[SPC_TRAIL + 1];   /* the rope, chasing it */
    int tx[SPC_TRAIL + 1], ty[SPC_TRAIL + 1];     /* and where it is drawn, rounded */
    uint32_t clicks, time, last_ms;
    float hue0;                          /* this session's colours */
    int clearing;                        /* frames of the clear button's fade left */
    uint8_t inset[SL_R_LG];              /* the rounded corners: px left unpainted, top rows down */
    struct cfm_hist work, paint;
    uint64_t paint_us, opened_us;
    uint32_t frames;
} spc;
static uint16_t spc_rows[3][W] __attribute__((aligned(4)));   /* two picture rows grown, one between */
static int spc_row_k[2];
static bool spc_hinted;                  /* shown once a session */
static bool qp_open;                     /* the control centre (sloppy_playback.inc) */

static bool spacey_on_top(void) { return spc.page && npages && pages[npages - 1] == spc.page; }
static bool spc_seen(void)
{
    return spacey_on_top() && !np_open && !qp_open && !sl_power_hides_ui() && !po_is_open();
}

static uint16_t spc_565(uint32_t c) { return (uint16_t)((c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x001f)); }
#define SPC_WIDE(v) (((v) | (uint32_t)(v) << 16) & 0x07e0f81fu)   /* green apart from red and blue, room to multiply */
#define SPC_NARROW(v) ((uint16_t)((v) | (v) >> 16))

/* hsl(h, 100 %, 50 %), 0xRRGGBB */
static uint32_t spc_hue(float h)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float s = h / 60.0f;
    int i = (int)s;
    uint32_t up = (uint32_t)((s - i) * 255.0f + 0.5f), dn = 255 - up;
    switch (i)
    {
        case 0:  return 0xff0000 | up << 8;
        case 1:  return dn << 16 | 0x00ff00;
        case 2:  return 0x00ff00 | up;
        case 3:  return dn << 8 | 0x0000ff;
        case 4:  return up << 16 | 0x0000ff;
        default: return 0xff0000 | dn;
    }
}

/* ------------------------------------------------------------ the tunnel */
#define SPC_FX(v) ((int32_t)lroundf((v) * 65536.0f))
/* The last frame into the next: zoomed in by 2 px, and, where the turned
 * and shrunk copy lands, that copy instead (read from the zoomed one, as
 * the original draws it second). One pass, nearest neighbour. */
static void spc_feedback(const uint16_t *src, uint16_t *dst)
{
    const int bw = spc.bw, bh = spc.bh;
    bool odd = spc.clicks & 1;
    float s = odd ? -8.0f : -32.0f, th = 3.14159265f + (float)spc.clicks + (odd ? 0 : 1);
    float kx = (float)spc.sw / (spc.sw + 2), ky = (float)spc.sh / (spc.sh + 2);
    float mx = (float)spc.sw / (spc.sw + s), my = (float)spc.sh / (spc.sh + s);
    float c = cosf(th), sn = sinf(th);
    /* the copy: u = mid + diag(mx, my) R(-th) (p - mid); read at the zoom of u */
    float a00 = mx * c, a01 = mx * sn, a10 = -my * sn, a11 = my * c;
    float cx = bw * 0.5f, cy = bh * 0.5f, dx0 = 0.5f - cx;
    int32_t ux_x = SPC_FX(a00), uy_x = SPC_FX(a10);
    int32_t fx_x = SPC_FX(kx * a00), fy_x = SPC_FX(ky * a10), zx_x = SPC_FX(kx);
    uint32_t uw = (uint32_t)bw << 16, uh = (uint32_t)bh << 16;
    for (int y = 0; y < bh; y++)
    {
        float dy = y + 0.5f - cy;
        float u = a00 * dx0 + a01 * dy, v = a10 * dx0 + a11 * dy;
        int32_t ux = SPC_FX(cx + u), uy = SPC_FX(cy + v);
        int32_t fx = SPC_FX(cx + kx * u), fy = SPC_FX(cy + ky * v);
        int32_t zx = SPC_FX(cx + kx * dx0);
        const uint16_t *zrow = src + (int)(cy + ky * dy) * bw;
        uint16_t *d = dst + y * bw;
        for (int x = 0; x < bw; x++)
        {
            if ((uint32_t)ux < uw && (uint32_t)uy < uh) d[x] = src[(fy >> 16) * bw + (fx >> 16)];
            else d[x] = zrow[zx >> 16];
            ux += ux_x; uy += uy_x; fx += fx_x; fy += fy_x; zx += zx_x;
        }
    }
}

/* towards black, a tenth */
static void spc_fade(uint16_t *p, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint32_t v = SPC_WIDE(p[i]) * SPC_KEEP >> 5 & 0x07e0f81fu;
        p[i] = SPC_NARROW(v);
    }
}

/* ------------------------------------------------------------ the trail */
static void spc_follow(void)
{
    float tx = (float)spc.fx, ty = (float)spc.fy;
    for (int i = 0; i <= SPC_TRAIL; i++)
    {
        spc.px[i] += (tx - spc.px[i]) * 0.5f;
        spc.py[i] += (ty - spc.py[i]) * 0.5f;
        spc.tx[i] = (int)lroundf(spc.px[i]);
        spc.ty[i] = (int)lroundf(spc.py[i]);
        tx = (float)spc.tx[i];
        ty = (float)spc.ty[i];
    }
}

/* c (spread wide) over *p, a of 32 */
static void spc_blend(uint16_t *p, uint32_t c, uint32_t a)
{
    uint32_t v = (SPC_WIDE(*p) * (32 - a) + c * a) >> 5 & 0x07e0f81fu;
    *p = SPC_NARROW(v);
}

/* A segment with round caps, r px about it, its edge smoothed, in picture px. */
static void spc_capsule(uint16_t *pic, float x0, float y0, float x1, float y1, float r, uint32_t rgb)
{
    int xa = (int)floorf(fminf(x0, x1) - r - 1), xb = (int)ceilf(fmaxf(x0, x1) + r + 1);
    int ya = (int)floorf(fminf(y0, y1) - r - 1), yb = (int)ceilf(fmaxf(y0, y1) + r + 1);
    xa = xa < 0 ? 0 : xa; ya = ya < 0 ? 0 : ya;
    xb = xb > spc.bw ? spc.bw : xb; yb = yb > spc.bh ? spc.bh : yb;
    float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy, inv = ll > 1e-6f ? 1.0f / ll : 0;
    float out = (r + 0.5f) * (r + 0.5f), in = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : -1;
    uint16_t c = spc_565(rgb);
    uint32_t cw = SPC_WIDE(c);
    for (int y = ya; y < yb; y++)
    {
        uint16_t *row = pic + y * spc.bw;
        float qy = y + 0.5f - y0;
        for (int x = xa; x < xb; x++)
        {
            float qx = x + 0.5f - x0, t = (qx * dx + qy * dy) * inv;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = qx - t * dx, ey = qy - t * dy, d2 = ex * ex + ey * ey;
            if (d2 >= out) continue;
            if (d2 <= in) row[x] = c;
            else spc_blend(&row[x], cw, (uint32_t)((r + 0.5f - sqrtf(d2)) * 32.0f));
        }
    }
}

/* the rope, tail first, so the newest segment lies on top */
static void spc_draw_trail(uint16_t *pic)
{
    for (int i = SPC_TRAIL; i > 0; i--)
    {
        int a = spc.tx[i] - spc.tx[i - 1], b = spc.ty[i] - spc.ty[i - 1];
        a = a ? a : 1;
        b = b ? b : 1;
        float speed = sqrtf((float)(a * a + b * b)) / 2;
        float hue = -(float)spc.time / 9 + i * 2 + speed * 6 + spc.hue0;
        float w = SPC_WIDTH - speed / 2;
        w = w < 2 ? 2 : w;
        spc_capsule(pic, spc.tx[i - 1] * 0.5f, spc.ty[i - 1] * 0.5f, spc.tx[i] * 0.5f, spc.ty[i] * 0.5f,
                    w / 4, spc_hue(hue));
    }
}

/* ------------------------------------------------------------ a frame */
static bool spc_in_corner(void)
{
    int x = spc.fx, y = spc.fy, r = spc.sw - 1 - SPC_CORNER, b = spc.sh - 1 - SPC_CORNER;
    return (x <= SPC_CORNER || x >= r) && (y <= SPC_CORNER || y >= b);
}

/* The stage again, as seven areas: three bands inside its rounded edge,
 * which it covers (nothing under them is drawn), and the four corners. */
static void spc_invalidate(void)
{
    lv_area_t o, a;
    lv_obj_get_coords(spc.stage, &o);
    const int r = SL_R_LG;
    const lv_area_t band[3] = { { o.x1 + r, o.y1, o.x2 - r, o.y1 + r - 1 }, { o.x1, o.y1 + r, o.x2, o.y2 - r },
                                { o.x1 + r, o.y2 - r + 1, o.x2 - r, o.y2 } };
    for (int i = 0; i < 3; i++) lv_obj_invalidate_area(spc.stage, &band[i]);
    for (int i = 0; i < 4; i++)
    {
        a.x1 = i & 1 ? o.x2 - r + 1 : o.x1; a.x2 = a.x1 + r - 1;
        a.y1 = i & 2 ? o.y2 - r + 1 : o.y1; a.y2 = a.y1 + r - 1;
        lv_obj_invalidate_area(spc.stage, &a);
    }
}

static void spc_tick(lv_timer_t *t)
{
    (void)t;
    if (!spc_seen()) { spc.last_ms = 0; return; }
    uint32_t now = lv_tick_get();
    int steps = spc.last_ms ? (int)((now - spc.last_ms + SPC_STEP / 2) / SPC_STEP) : 1;
    steps = steps < 1 ? 1 : steps > 4 ? 4 : steps;
    spc.last_ms = now;
    if (!spc.touched && !spc.clearing) return;   /* black and still: nothing to draw */
    uint64_t t0 = sl_perf_now_us();
    if (spc.paint_us)                    /* the last frame's painting, all its strips */
    {
        cfm_add(&spc.paint, (uint32_t)spc.paint_us);
        spc.paint_us = 0;
    }
    if (spc.touched)
        for (int i = 0; i < steps; i++) { spc_follow(); spc.time++; }
    const uint16_t *src = spc.buf[spc.cur];
    uint16_t *dst = spc.buf[spc.cur ^ 1];
    spc_feedback(src, dst);
    spc.cur ^= 1;
    if ((spc.down && spc_in_corner()) || spc.clearing) spc_fade(dst, spc.bw * spc.bh);
    if (spc.clearing && !--spc.clearing)
    {
        memset(spc.buf[0], 0, (size_t)spc.bw * spc.bh * 2);   /* the last of it gone, not a dim ghost */
        memset(spc.buf[1], 0, (size_t)spc.bw * spc.bh * 2);
    }
    if (spc.touched) spc_draw_trail(dst);
    spc_invalidate();
    uint64_t t1 = sl_perf_now_us();
    cfm_add(&spc.work, (uint32_t)(t1 - t0));
    spc.frames++;
    /* no closer than the work and its painting take, and a third again */
    uint32_t paint = spc.paint.n ? (uint32_t)(spc.paint.sum / spc.paint.n) : 0;
    uint32_t period = ((uint32_t)(t1 - t0) + paint) / 1000 * 4 / 3;
    lv_timer_set_period(spc.timer, period < SPC_FRAME ? SPC_FRAME : period < 100 ? period : 100);
}

/* ------------------------------------------------------------ painting */
#define SPC_AVG(a, b, m) (((a) & (b)) + ((((a) ^ (b)) & (m)) >> 1))   /* RGB565 halfway */

/* picture row k at stage width: each pixel, then halfway to the next */
static const uint16_t *spc_row(int k)
{
    uint16_t *o = spc_rows[k & 1];
    if (spc_row_k[k & 1] == k) return o;
    spc_row_k[k & 1] = k;
    const uint16_t *s = spc.buf[spc.cur] + k * spc.bw;
    uint32_t *o32 = (uint32_t *)o, c = s[0];
    for (int x = 0; x < spc.bw - 1; x++)
    {
        uint32_t n = s[x + 1];
        o32[x] = c | SPC_AVG(c, n, 0xf7deu) << 16;
        c = n;
    }
    o32[spc.bw - 1] = c | c << 16;
    return o;
}
/* stage row y: a picture row, or halfway between two */
static const uint16_t *spc_screen_row(int y)
{
    int k = y >> 1;
    const uint16_t *r = spc_row(k);
    if (!(y & 1) || k + 1 >= spc.bh) return r;
    const uint32_t *a = (const uint32_t *)r, *b = (const uint32_t *)spc_row(k + 1);
    uint32_t *o = (uint32_t *)spc_rows[2];
    for (int x = 0; x < spc.bw; x++) o[x] = SPC_AVG(a[x], b[x], 0xf7def7deu);
    return spc_rows[2];
}

/* The picture, straight into LVGL's buffer, as much as the clip shows,
 * inside the stage's rounded corners. Asked first (user data 1): a clip
 * all inside them is painted whole, and the card's black under it is not
 * drawn at all. Otherwise after the card, which smooths the corners. */
static void spc_paint_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_buf_t *db = layer->draw_buf;
    lv_area_t obj, a = layer->_clip_area;
    lv_obj_get_coords(spc.stage, &obj);
    bool first = lv_event_get_user_data(e) != NULL;
    const lv_area_t *c = &layer->_clip_area;   /* in a band clear of the corners */
    bool inside = c->x1 >= obj.x1 && c->x2 <= obj.x2 && c->y1 >= obj.y1 && c->y2 <= obj.y2 &&
                  ((c->x1 >= obj.x1 + SL_R_LG && c->x2 <= obj.x2 - SL_R_LG) ||
                   (c->y1 >= obj.y1 + SL_R_LG && c->y2 <= obj.y2 - SL_R_LG));
    if (first != inside) return;
    if (first) lv_event_stop_processing(e);
    const lv_area_t *in[2] = { &layer->buf_area, &obj };
    for (int i = 0; i < 2; i++)
    {
        a.x1 = LV_MAX(a.x1, in[i]->x1); a.y1 = LV_MAX(a.y1, in[i]->y1);
        a.x2 = LV_MIN(a.x2, in[i]->x2); a.y2 = LV_MIN(a.y2, in[i]->y2);
    }
    if (!spc.buf[0] || !db || layer->color_format != LV_COLOR_FORMAT_RGB565 || a.x1 > a.x2 || a.y1 > a.y2)
        return;
    uint64_t t0 = sl_perf_now_us();
    spc_row_k[0] = spc_row_k[1] = -1;
    uint8_t *d = db->data + (a.y1 - layer->buf_area.y1) * db->header.stride;
    for (int y = a.y1; y <= a.y2; y++, d += db->header.stride)
    {
        int k = y - obj.y1, edge = k < SL_R_LG ? k : spc.sh - 1 - k;
        int in_px = edge < SL_R_LG ? spc.inset[edge] : 0;
        int x1 = LV_MAX(a.x1, obj.x1 + in_px), x2 = LV_MIN(a.x2, obj.x2 - in_px);
        if (x1 > x2) continue;
        const uint16_t *r = spc_screen_row(k);
        memcpy(d + (x1 - layer->buf_area.x1) * 2, r + (x1 - obj.x1), (size_t)(x2 - x1 + 1) * 2);
    }
    spc.paint_us += sl_perf_now_us() - t0;   /* a frame's strips add up; the next tick counts them */
}

/* ------------------------------------------------------------ touch */
static void spc_hint_hidden(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void spc_hint_out(uint32_t ms, uint32_t delay)
{
    if (!spc.hint || lv_obj_has_flag(spc.hint, LV_OBJ_FLAG_HIDDEN)) return;
    lv_anim_delete(spc.hint, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, spc.hint);
    lv_anim_set_values(&a, lv_obj_get_style_opa(spc.hint, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, spa_set_opa);
    lv_anim_set_completed_cb(&a, spc_hint_hidden);
    lv_anim_start(&a);
}

static void spc_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        spc.down = false;
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    lv_area_t obj;
    lv_obj_get_coords(spc.stage, &obj);
    int x = pt.x - obj.x1, y = pt.y - obj.y1;   /* the gutter round it counts as its edge */
    spc.fx = x < 0 ? 0 : x >= spc.sw ? spc.sw - 1 : x;
    spc.fy = y < 0 ? 0 : y >= spc.sh ? spc.sh - 1 : y;
    if (code != LV_EVENT_PRESSED) return;
    spc.down = true;
    spc.clicks++;                        /* each touch turns the tunnel */
    spc.clearing = 0;
    if (!spc.touched)                    /* the rope starts gathered under the finger */
    {
        for (int i = 0; i <= SPC_TRAIL; i++)
        {
            spc.px[i] = (float)(spc.tx[i] = spc.fx);
            spc.py[i] = (float)(spc.ty[i] = spc.fy);
        }
        spc.touched = true;
    }
    spc_hint_out(SL_T_FAST, 0);
}

/* the header's clear: everything fades out and the pen lifts */
static void spc_clear_cb(lv_event_t *e)
{
    (void)e;
    if (!spc.touched && !spc.clearing) return;
    spc.touched = false;
    spc.clearing = SPC_CLEAR;
}

/* ------------------------------------------------------------ open, close */
static void spc_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != spc.page) return;
    if (spc.timer) lv_timer_delete(spc.timer);
    free(spc.buf[0]);
    free(spc.buf[1]);
    uint32_t secs = (uint32_t)((sl_perf_now_us() - spc.opened_us) / 1000000);
    if (spc.work.n)
        sl_log("spacey lines: %lu frames in %lu s at %d x %d; work avg %lu p95 %lu max %lu us, paint avg %lu p95 %lu max %lu us",
               (unsigned long)spc.frames, (unsigned long)secs, spc.bw, spc.bh,
               (unsigned long)(spc.work.sum / spc.work.n), (unsigned long)cfm_pct(&spc.work, 95),
               (unsigned long)spc.work.max, (unsigned long)(spc.paint.n ? spc.paint.sum / spc.paint.n : 0),
               (unsigned long)cfm_pct(&spc.paint, 95), (unsigned long)spc.paint.max);
    memset(&spc, 0, sizeof spc);
}

static void spacey_page(void)
{
    if (page_bring_back(spc.page)) return;
    int top = SB_H + SL_HDR_H + SL_SP_XS, sw = W - 2 * SL_GUTTER, sh = (H - top - SL_GUTTER) & ~1;
    size_t n = (size_t)(sw / 2) * (sh / 2);
    uint16_t *a = calloc(n, 2), *b = calloc(n, 2);
    if (!a || !b)
    {
        free(a); free(b);
        toast_warn("Not enough memory for Spacey Lines");
        return;
    }
    struct cfm_hist keep = { .shift = 9 };   /* 0.5 ms bins */
    memset(&spc, 0, sizeof spc);
    spc.work = spc.paint = keep;
    spc.buf[0] = a; spc.buf[1] = b;
    spc.sw = sw; spc.sh = sh; spc.bw = sw / 2; spc.bh = sh / 2;
    spc.hue0 = (float)(lv_rand(0, 3599)) / 10 + 1;
    spc.opened_us = sl_perf_now_us();
    for (int k = 0; k < SL_R_LG; k++)    /* inside the card's corner, and a pixel more for its smoothed edge */
    {
        float dy = SL_R_LG - k - 0.5f;
        spc.inset[k] = (uint8_t)(ceilf(SL_R_LG - sqrtf(SL_R_LG * SL_R_LG - dy * dy)) + 1);
    }

    lv_obj_t *p;
    lv_obj_t *body = page_new("Spacey Lines", &p);
    spc.page = p;
    lv_obj_add_event_cb(p, spc_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    header_button(p, LV_SYMBOL_TRASH, spc_clear_cb, NULL);

    /* the stage: a black card, the gutter round it, down to the foot */
    spc.stage = box(p);
    lv_obj_set_pos(spc.stage, SL_GUTTER, top);
    lv_obj_set_size(spc.stage, sw, sh);
    lv_obj_set_style_radius(spc.stage, SL_R_LG, 0);
    lv_obj_set_style_bg_color(spc.stage, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(spc.stage, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(spc.stage, false, 0);
    lv_obj_add_flag(spc.stage, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_USER_3);   /* draws sideways: back only from the edge */
    lv_obj_remove_flag(spc.stage, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(spc.stage, SL_GUTTER);
    lv_obj_add_event_cb(spc.stage, spc_paint_cb, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, (void *)1);
    lv_obj_add_event_cb(spc.stage, spc_paint_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(spc.stage, spc_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(spc.stage, spc_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(spc.stage, spc_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(spc.stage, spc_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    /* the first time a session: how to play, and whose idea it was */
    if (!spc_hinted)
    {
        spc_hinted = true;
        lv_obj_t *h = spc.hint = box(spc.stage);
        lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_pad_all(h, SL_INSET, 0);
        lv_obj_set_style_pad_row(h, SL_SP_XS, 0);
        lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        const struct { const char *s; const lv_font_t *f; lv_opa_t opa; } line[] = {
            { "Draw with one finger", &ss_semibold_21, LV_OPA_COVER },
            { "Tap to turn the tunnel. Hold a corner to clear.", &ss_regular_18, 190 },
            { "after line05x by Chris Shier", &ss_regular_16, 120 },
        };
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *l = text(h, line[i].s, line[i].f, 0xffffff);
            lv_obj_set_style_text_color(l, lv_color_white(), 0);   /* on the black stage in every theme */
            lv_obj_set_style_text_opa(l, line[i].opa, 0);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(l, lv_pct(100));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            if (i == 2) lv_obj_set_style_margin_top(l, SL_SP_S, 0);
        }
        spc_hint_out(SL_T_SLOW * 2, SPC_HINT_MS);
    }

    spc.timer = lv_timer_create(spc_tick, SPC_FRAME, NULL);
    page_push(p);
    mini_update();                       /* the mini player goes now, not on its next round: it sat over the stage */
}
