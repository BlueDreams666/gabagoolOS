/* Flappy Bird. GPLv2+.
 *
 * The game everyone knows: tap (or press Play or Next) and the bird flaps
 * up; let go and it falls. Fly through the gaps in the pipes; each pair
 * passed is a point. Hit a pipe or the ground and it is over: the score,
 * the best, a medal at 10, 20, 30 and 40, and Play to go again at once.
 * Day or night and the bird's colour are drawn at random every round.
 *
 * The rules are the original's, measured in art pixels (the world is
 * 160 x 267 of them, three screen pixels each; the original's is 144 x 256,
 * so its numbers carry over as they are):
 *   gravity 450 px/s^2, a flap sets the fall speed to -135 px/s, the fall
 *   tops out at 150 px/s; the pipes come at 60 px/s, 72 px apart, each with
 *   a gap of 50 px set at random between 20 % and 56 % of the sky's height.
 * At 60 frames a second that is one art pixel a frame, as the original.
 * The bird points up 25 degrees while it climbs and noses down as it falls.
 *
 * The rules step at a fixed 120 times a second whatever the frame rate,
 * so a slow frame never lets the bird through a pipe. A tap flaps in the
 * event that brings it, never waiting for a frame. A hit is a drawn pixel
 * of the bird, turned as it is drawn, on a pipe or the ground.
 *
 * The screen is one canvas. Each frame redraws only what changed: the
 * columns of a pipe whose colour moved, the ground's stripes, the bird,
 * the score. The still scenery (day and night) is drawn once and kept.
 *
 * Locking the player, pulling a panel down or leaving pauses a round;
 * Play carries on. The best score is kept on the card (flappy.txt). */

#include "sloppy_flappy_art.inc"

#define FL_FILE    ROCKBOX_DIR "/sloppy/flappy.txt"
#define FL_STEP    (1.0f / 120)
#define FL_GRAVITY 450.0f                   /* art px/s^2 */
#define FL_FLAP    (-135.0f)                /* art px/s */
#define FL_TERMINAL 150.0f
#define FL_SPEED   60.0f                    /* the pipes, art px/s */
#define FL_SPACING 72                       /* art px from pipe to pipe */
#define FL_GAP     50
#define FL_GAP_LO  (FB_GROUND / 5)          /* the gap's top, between these */
#define FL_GAP_HI  (FB_GROUND / 5 + FB_GROUND * 3 / 5 - FL_GAP)
#define FL_BIRD_X  48                       /* the bird's middle, art px */
#define FL_PIPES   4
#define FL_TILT_UP (-25.0f)                 /* degrees; down is positive */
#define FL_SCORE_Y 22                       /* the score's top, art px */
#define FL_PAUSE_X 14                       /* the pause button, screen px */
#define FL_PAUSE_Y (SB_H + 12)
#define FL_TITLE_Y 58
#define FL_PANEL_X ((FB_LW - FB_PANEL_W) / 2)
#define FL_PANEL_Y 92
#define FL_PLAY_Y  160
#define FL_TXT_MAX (232 * 48)

enum { FL_READY, FL_FLY, FL_FALL, FL_OVER, FL_PAUSE };

/* words drawn as pixel art: a font's letters at their own size, each
 * pixel then blown up like the rest; 0 clear, 1 edge, 2 rim, 3 face */
struct fl_text { int w, h, s; uint8_t m[FL_TXT_MAX]; };
struct fl_pipe { int x, gap; bool scored; };   /* world art px */

static struct {
    lv_obj_t *page, *hdr, *canvas, *flash, *pause_btn, *play_btn;
    lv_timer_t *timer;
    int state;
    float y, vy, rot;                        /* the bird: its middle (art px), speed, turn (degrees) */
    double cam;                              /* how far the world has scrolled (art px) */
    float t_acc, wing_t;
    int wing, colour, night, score, shown, medal;
    bool new_best, play_down, pause_down;
    uint32_t last, t0, died_at, over_at, hint_gone_at;
    bool hit_pipe;
    int swooshed;                            /* Game Over's swooshes played so far */
    struct fl_pipe pipe[FL_PIPES];
    /* what is on the screen now, to redraw only what changes */
    int d_cam;
    struct fb_rect d_bird;                   /* the bird as last drawn: where, and its pose */
    int d_cy, d_wing_c;
    float d_ang;
    int d_wing, d_score, d_pause;
    bool d_valid;
} fl;
static int fl_best;
static bool fl_loaded;
static uint32_t fl_rng = 2463534242u;
static uint16_t fl_px[FB_SW * FB_SH];                  /* the canvas */
static uint16_t fl_bg[2][FB_SW * FB_SH];               /* day and night, drawn once */
static bool fl_bg_made[2], fl_art_made;
static uint16_t fl_strip[FB_STRIPE_H * FB_S * FB_STRIPE_SW];
static struct fl_text fl_t_ready, fl_t_over, fl_t_paused, fl_t_medal, fl_t_score, fl_t_best, fl_t_new;

static uint32_t fl_rand(void) { fl_rng ^= fl_rng << 13; fl_rng ^= fl_rng >> 17; fl_rng ^= fl_rng << 5; return fl_rng; }
static void fl_load(void)
{
    if (fl_loaded) return;
    fl_loaded = true;
    char b[32];
    int fd = open(FL_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got > 0) { b[got] = 0; int v; if (sscanf(b, "FLB1 %d", &v) == 1 && v >= 0) fl_best = v; }
}
static void fl_save(void)
{
    char b[32];
    int n = snprintf(b, sizeof b, "FLB1 %d\n", fl_best);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(FL_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, n) == n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(FL_FILE ".tmp", FL_FILE) < 0) remove(FL_FILE ".tmp");
}

/* ------------------------------------------------------- pixel words */
/* Latin letters in capitals (the panel's words), as the original's are;
 * other scripts as they are */
static void fl_upper(const char *s, char *o, size_t n)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 2 < n; p++)
    {
        if (*p >= 'a' && *p <= 'z') o[k++] = (char)(*p - 32);
        else if (*p == 0xc3 && p[1] >= 0xa0 && p[1] <= 0xbe && p[1] != 0xb7) { o[k++] = (char)*p; o[k++] = (char)(p[1] - 0x20); p++; }
        else o[k++] = (char)*p;
    }
    o[k] = 0;
}
/* the words in font f, their letters' pixels kept where they are more
 * than half covered; 'ring' adds a white rim then a dark edge around */
static int fl_text_try(struct fl_text *t, const char *s, const lv_font_t *f, int ring)
{
    lv_obj_t *l = lv_label_create(fl.page);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_update_layout(l);
    lv_draw_buf_t *b = lv_snapshot_take(l, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(l);
    if (!b) return -1;
    int bw = b->header.w, bh = b->header.h, pad = ring, x0 = bw, x1 = -1, y0 = bh, y1 = -1;
    for (int y = 0; y < bh; y++)
    {
        const uint8_t *row = b->data + y * b->header.stride;
        for (int x = 0; x < bw; x++)
            if (row[x * 4 + 3] >= 128) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    }
    int w = x1 < x0 ? 0 : x1 - x0 + 1 + 2 * pad, h = y1 < y0 ? 0 : y1 - y0 + 1 + 2 * pad;
    if (w <= 0 || w * h > FL_TXT_MAX) { lv_draw_buf_destroy(b); return w <= 0 ? 0 : -1; }
    t->w = w; t->h = h;
    memset(t->m, 0, (size_t)(w * h));
    for (int y = y0; y <= y1; y++)
    {
        const uint8_t *row = b->data + y * b->header.stride;
        for (int x = x0; x <= x1; x++)
            if (row[x * 4 + 3] >= 128) t->m[(y - y0 + pad) * w + x - x0 + pad] = 3;
    }
    lv_draw_buf_destroy(b);
    /* a white rim, then a dark edge, each a pixel all round */
    for (int pass = 0; pass < (ring ? 2 : 0); pass++)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                if (t->m[y * w + x]) continue;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        uint8_t v = xx >= 0 && yy >= 0 && xx < w && yy < h ? t->m[yy * w + xx] : 0;
                        if (v >= 2 && v <= 3) t->m[y * w + x] = 5;   /* 5: new this pass */
                    }
            }
        for (int i = 0; i < w * h; i++) if (t->m[i] == 5) t->m[i] = (uint8_t)(pass ? 1 : 2);
    }
    return w;
}
/* a title: the biggest of these fonts that fits at three times, or at two */
static void fl_text_make(struct fl_text *t, const char *s, bool title)
{
    const lv_font_t *const big[] = { &ss_semibold_23, &ss_semibold_21, &ss_semibold_18 };
    char up[96];
    if (!title) { fl_upper(s, up, sizeof up); s = up; }
    t->w = t->h = 0;
    if (title)
    {
        for (int s3 = 3; s3 >= 2; s3--)
            for (unsigned i = 0; i < ARRAYLEN(big); i++)
                if (fl_text_try(t, s, big[i], 2) > 0 && t->w * s3 <= FB_SW - 24) { t->s = s3; return; }
    }
    else if (fl_text_try(t, s, &ss_semibold_18, 0) > 0 && t->w * 2 <= 160) { t->s = 2; return; }
    else if (fl_text_try(t, s, &ss_regular_16, 0) > 0) { t->s = 2; return; }
    t->s = 2;
}
static void fl_text_draw(uint16_t *px, const struct fb_rect *clip, const struct fl_text *t, int sx, int sy,
                         uint32_t face, uint32_t rim, uint32_t edge, int opa)
{
    if (!t->w) return;
    struct fb_rect r = { sx, sy, sx + t->w * t->s, sy + t->h * t->s }, k;
    if (!fb_rect_and(&k, &r, clip)) return;
    uint16_t c[4] = { 0, FB_RGB(edge), FB_RGB(rim), FB_RGB(face) };
    for (int y = k.y0; y < k.y1; y++)
    {
        const uint8_t *row = t->m + (y - sy) / t->s * t->w;
        for (int x = k.x0; x < k.x1; x++)
        {
            uint8_t v = row[(x - sx) / t->s];
            if (v) px[y * FB_SW + x] = opa >= 256 ? c[v] : fb_blend(px[y * FB_SW + x], c[v], opa);
        }
    }
}

/* ----------------------------------------------------- what is drawn */
static int fl_cam_px(void) { return (int)floor(fl.cam * FB_S); }
static int fl_pipe_sx(const struct fl_pipe *p, int cam) { return p->x * FB_S - cam; }
static const uint16_t (*fl_bird_map(void))[FB_BIRD_W] { return (const uint16_t (*)[FB_BIRD_W])fb_bird_px[fl.colour][fl.wing]; }
static float fl_bird_y(void)
{
    float y = fl.y;
    if (fl.state == FL_READY) y += 4 * sinf((lv_tick_get() - fl.t0) * 6.2831853f / 1000);   /* bobbing, waiting */
    return y;
}
/* the bird now: its middle (screen px) and turn (radians) */
#define FL_BIRD_CX (FL_BIRD_X * FB_S + FB_S / 2.0f)
static float fl_bird_cy(void) { return fl_bird_y() * FB_S; }
static float fl_bird_ang(void) { return fl.rot * 0.01745329f; }
static struct fb_bird_img fl_img, fl_hit_img;   /* as drawn this frame; as tested this step */
static void fl_bird_now(struct fb_bird_img *b) { fb_bird_render(b, FL_BIRD_CX, fl_bird_cy(), fl_bird_ang(), fl_bird_map()); }
/* the overlays: which, and how far in (state, title, panel, count, medal) */
struct fl_ov { int8_t which; int16_t title_y; int16_t title_opa; int16_t panel_y; int16_t count; int8_t glint; bool medal, play, fresh, down; };
static struct fl_ov fl_ov_now, fl_ov_drawn;
static int fl_ease_out(int t, int d, int a, int b) { if (t >= d) return b; float u = 1 - (float)t / d; return a + (int)((b - a) * (1 - u * u * u)); }
static void fl_ov_compute(struct fl_ov *o, uint32_t now)
{
    memset(o, 0, sizeof *o);
    o->which = (int8_t)(fl.state == FL_READY ? 1 : fl.state == FL_OVER ? 2 : fl.state == FL_PAUSE ? 3 : 0);
    if (fl.state == FL_FLY && fl.hint_gone_at && now - fl.hint_gone_at < 200)
    {
        o->which = 1;                    /* Get Ready fading as the bird sets off */
        o->title_opa = (int16_t)(256 - (now - fl.hint_gone_at) * 256 / 200);
    }
    else if (o->which) o->title_opa = 256;
    o->title_y = FL_TITLE_Y * FB_S;
    if (o->which == 2)
    {
        uint32_t t = now - fl.over_at;
        o->title_opa = (int16_t)(t < 250 ? 0 : t < 450 ? (t - 250) * 256 / 200 : 256);
        o->title_y = (int16_t)(t < 250 ? FL_TITLE_Y * FB_S - 18 : fl_ease_out((int)t - 250, 200, FL_TITLE_Y * FB_S - 18, FL_TITLE_Y * FB_S));
        o->panel_y = (int16_t)(t < 600 ? FB_SH : fl_ease_out((int)t - 600, 350, FB_SH, FL_PANEL_Y * FB_S));
        int span = fl.score * 40 > 1000 ? 1000 : fl.score * 40;
        o->count = (int16_t)(t < 950 ? 0 : span <= 0 || t >= 950u + span ? fl.score : (int)((t - 950) * fl.score / span));
        bool done = t >= 950u + span;
        o->medal = done && fl.medal >= 0;
        o->fresh = done && fl.new_best;
        o->play = t >= 950;
        o->glint = (int8_t)(o->medal ? ((t - 950 - span) / 90) % 14 : -1);
    }
    if (o->which == 3) o->play = true;
    o->down = o->play && fl.play_down;
}
/* where the overlays draw (screen px): the title (with the hint), the
 * panel, the Play button */
static struct fb_rect fl_ov_title_r(const struct fl_ov *o)
{
    struct fb_rect r = { 0, 0, 0, 0 };
    if (!o->which) return r;
    const struct fl_text *t = o->which == 1 ? &fl_t_ready : o->which == 2 ? &fl_t_over : &fl_t_paused;
    r.x0 = (FB_SW - t->w * t->s) / 2; r.x1 = r.x0 + t->w * t->s;
    r.y0 = o->title_y; r.y1 = o->title_y + t->h * t->s;
    return r;
}
/* Get Ready's hint: the arrow, the hand and its marks */
static struct fb_rect fl_ov_hint_r(const struct fl_ov *o)
{
    struct fb_rect r = { 0, 0, 0, 0 };
    if (o->which == 1) { r.x0 = 72 * FB_S; r.y0 = 104 * FB_S; r.x1 = 91 * FB_S; r.y1 = 138 * FB_S; }
    return r;
}
static struct fb_rect fl_ov_panel_r(const struct fl_ov *o)
{
    struct fb_rect r = { 0, 0, 0, 0 };
    if (o->which != 2 || o->panel_y >= FB_SH) return r;
    r.x0 = FL_PANEL_X * FB_S; r.x1 = r.x0 + FB_PANEL_W * FB_S; r.y0 = o->panel_y; r.y1 = o->panel_y + FB_PANEL_H * FB_S;
    return r;
}
static struct fb_rect fl_ov_play_r(const struct fl_ov *o)
{
    struct fb_rect r = { 0, 0, 0, 0 };
    if (!o->play) return r;
    r.x0 = (FB_LW - fb_play.w) / 2 * FB_S; r.x1 = r.x0 + fb_play.w * FB_S; r.y0 = FL_PLAY_Y * FB_S; r.y1 = r.y0 + (fb_play.h + 1) * FB_S;
    return r;
}
static void fl_ov_draw(uint16_t *px, const struct fb_rect *clip, const struct fl_ov *o)
{
    if (!o->which) return;
    const struct fl_text *title = o->which == 1 ? &fl_t_ready : o->which == 2 ? &fl_t_over : &fl_t_paused;
    uint32_t face = o->which == 1 ? 0x63c64a : o->which == 2 ? 0xf6a03a : 0xffffff;
    if (o->title_opa > 0)
        fl_text_draw(px, clip, title, (FB_SW - title->w * title->s) / 2, o->title_y, face, 0xffffff, fb_ink, o->title_opa);
    if (o->which == 1)
    {
        /* the hint: an arrow up over a hand that taps, marks either side */
        int opa = o->title_opa;
        fb_sprite_draw(px, clip, &fb_arrow, 77 * FB_S, 106 * FB_S, FB_S, opa);
        fb_sprite_draw(px, clip, &fb_hand, 76 * FB_S, 120 * FB_S, FB_S, opa);
        static const int8_t marks[6][2] = { { 75, 120 }, { 74, 119 }, { 73, 118 }, { 84, 120 }, { 85, 119 }, { 86, 118 } };
        for (int i = 0; i < 6; i++)
        {
            struct fb_rect m = { marks[i][0] * FB_S, marks[i][1] * FB_S, marks[i][0] * FB_S + FB_S, marks[i][1] * FB_S + FB_S }, k;
            if (fb_rect_and(&k, &m, clip))
                for (int y = k.y0; y < k.y1; y++)
                    for (int x = k.x0; x < k.x1; x++) px[y * FB_SW + x] = fb_blend(px[y * FB_SW + x], 0xffff, opa > 255 ? 255 : opa);
        }
        return;
    }
    if (o->which == 2 && o->panel_y < FB_SH)
    {
        int ox = FL_PANEL_X * FB_S, oy = o->panel_y;
        struct fb_rect k;
        fb_map_draw(px, clip, fb_panel_px, FB_PANEL_W, FB_PANEL_H, ox, oy, FB_S, 256);
        /* the words, at two: Medal over the dish, Score and Best on the right */
        uint32_t word = 0xe0702c, lift = 0xfbf6cf;
        int right = ox + 107 * FB_S;
        const struct fl_text *mt = &fl_t_medal;
        fl_text_draw(px, clip, mt, ox + 27 * FB_S - mt->w * mt->s / 2, oy + 6 * FB_S + 1, lift, lift, lift, 256);
        fl_text_draw(px, clip, mt, ox + 27 * FB_S - mt->w * mt->s / 2, oy + 6 * FB_S, word, word, word, 256);
        fl_text_draw(px, clip, &fl_t_score, right - fl_t_score.w * fl_t_score.s, oy + 6 * FB_S + 1, lift, lift, lift, 256);
        fl_text_draw(px, clip, &fl_t_score, right - fl_t_score.w * fl_t_score.s, oy + 6 * FB_S, word, word, word, 256);
        fl_text_draw(px, clip, &fl_t_best, right - fl_t_best.w * fl_t_best.s, oy + 30 * FB_S + 1, lift, lift, lift, 256);
        fl_text_draw(px, clip, &fl_t_best, right - fl_t_best.w * fl_t_best.s, oy + 30 * FB_S, word, word, word, 256);
        int best = o->fresh || o->count > fl_best ? (o->count > fl_best ? o->count : fl_best) : fl_best;
        int sw = fb_number_w(false, o->count) * FB_S, bw = fb_number_w(false, best) * FB_S;
        fb_number_draw(px, clip, false, o->count, right - sw + FB_S, oy + 16 * FB_S, 256);
        fb_number_draw(px, clip, false, best, right - bw + FB_S, oy + 40 * FB_S, 256);
        if (o->fresh)
        {
            /* NEW: a red tag before the best */
            const struct fl_text *nt = &fl_t_new;
            int tw = nt->w * nt->s + 4 * FB_S, th = 9 * FB_S, tx = right - bw - tw - FB_S, ty = oy + 40 * FB_S + FB_S;
            if (tx < ox + 48 * FB_S) tx = ox + 48 * FB_S;
            struct fb_rect tr = { tx, ty, tx + tw, ty + th };
            if (fb_rect_and(&k, &tr, clip))
                for (int y = k.y0; y < k.y1; y++)
                    for (int x = k.x0; x < k.x1; x++)
                    {
                        bool corner = (x - tx < FB_S || tx + tw - 1 - x < FB_S) && (y - ty < FB_S || ty + th - 1 - y < FB_S);
                        if (!corner) px[y * FB_SW + x] = FB_RGB(0xe2402f);
                    }
            fl_text_draw(px, clip, nt, tx + 2 * FB_S, ty + (th - nt->h * nt->s) / 2, 0xffffff, 0xffffff, 0xffffff, 256);
        }
        if (o->medal)
        {
            int mx = ox + 16 * FB_S, my = oy + 21 * FB_S;
            fb_map_draw(px, clip, &fb_medal_px[fl.medal][0][0], FB_MEDAL, FB_MEDAL, mx, my, FB_S, 256);
            if (o->glint >= 0 && o->glint < 5)
            {
                /* a glint, somewhere on the medal each time */
                uint32_t h = fb_hash((uint32_t)(fl.over_at / 7 + (lv_tick_get() - fl.over_at) / 1260));
                int gx = mx + (4 + (int)(h % 12)) * FB_S, gy = my + (4 + (int)((h >> 8) % 12)) * FB_S;
                for (int y = 0; y < 5; y++)
                    for (int x = 0; x < 5; x++)
                        if (fb_glint[o->glint][y][x] == 'W') fb_block(px, clip, gx + x * FB_S, gy + y * FB_S, FB_S, 0xffff);
            }
        }
    }
    if (o->play)
    {
        int bx = (FB_LW - fb_play.w) / 2 * FB_S, by = FL_PLAY_Y * FB_S + (fl.play_down ? FB_S : 0);
        fb_sprite_draw(px, clip, &fb_play, bx, by, FB_S, 256);
    }
}
static struct fb_rect fl_score_rect(int n)
{
    int w = fb_number_w(true, n) * FB_S;
    struct fb_rect r = { (FB_SW - w) / 2, FL_SCORE_Y * FB_S, (FB_SW - w) / 2 + w, FL_SCORE_Y * FB_S + 17 * FB_S };
    return r;
}
static bool fl_score_shown(void) { return fl.state == FL_FLY || fl.state == FL_FALL || fl.state == FL_PAUSE; }
static bool fl_pause_shown(void) { return fl.state == FL_FLY; }

/* the screen inside r, from the scenery up */
static void fl_compose(const struct fb_rect *r)
{
    struct fb_rect all = { 0, 0, FB_SW, FB_SH }, k;
    if (!fb_rect_and(&k, r, &all)) return;
    const uint16_t *bg = fl_bg[fl.night];
    for (int y = k.y0; y < k.y1; y++) memcpy(fl_px + y * FB_SW + k.x0, bg + y * FB_SW + k.x0, (size_t)(k.x1 - k.x0) * 2);
    int cam = fl_cam_px(), ground = FB_GROUND * FB_S;
    if (fl.state != FL_READY)
        for (int i = 0; i < FL_PIPES; i++)
            fb_pipe_draw(fl_px, &k, fl_pipe_sx(&fl.pipe[i], cam), fl.pipe[i].gap * FB_S, (fl.pipe[i].gap + FL_GAP) * FB_S, ground);
    /* the stripes, slid by the scroll */
    int sy0 = FB_STRIPE_Y * FB_S, sy1 = sy0 + FB_STRIPE_H * FB_S, off = cam % (FB_STRIPE * FB_S);
    for (int y = k.y0 > sy0 ? k.y0 : sy0; y < k.y1 && y < sy1; y++)
        memcpy(fl_px + y * FB_SW + k.x0, fl_strip + (y - sy0) * FB_STRIPE_SW + off + k.x0, (size_t)(k.x1 - k.x0) * 2);
    if (fl_pause_shown())
        fb_sprite_draw(fl_px, &k, &fb_pause, FL_PAUSE_X, FL_PAUSE_Y + (fl.pause_down ? FB_S : 0), FB_S, 256);
    fb_bird_draw(fl_px, &k, &fl_img);
    if (fl_score_shown())
    {
        struct fb_rect s = fl_score_rect(fl.score);
        fb_number_draw(fl_px, &k, true, fl.score, s.x0, s.y0, 256);
    }
    fl_ov_draw(fl_px, &k, &fl_ov_now);
}

/* ------------------------------------------------- the dirty places */
#define FL_DIRTY 24
static struct fb_rect fl_dirty[FL_DIRTY];
static int fl_ndirty;
static void fl_dirty_add(struct fb_rect r)
{
    struct fb_rect all = { 0, 0, FB_SW, FB_SH };
    if (!fb_rect_and(&r, &r, &all)) return;
    if (fl_ndirty == FL_DIRTY)
    {
        /* full: the new one joins whichever it grows least */
        int best = 0; long cost = LONG_MAX;
        for (int i = 0; i < fl_ndirty; i++)
        {
            struct fb_rect *d = &fl_dirty[i];
            long u = (long)((r.x1 > d->x1 ? r.x1 : d->x1) - (r.x0 < d->x0 ? r.x0 : d->x0)) *
                     ((r.y1 > d->y1 ? r.y1 : d->y1) - (r.y0 < d->y0 ? r.y0 : d->y0)) - (long)(d->x1 - d->x0) * (d->y1 - d->y0);
            if (u < cost) { cost = u; best = i; }
        }
        struct fb_rect *d = &fl_dirty[best];
        if (r.x0 < d->x0) d->x0 = r.x0;
        if (r.y0 < d->y0) d->y0 = r.y0;
        if (r.x1 > d->x1) d->x1 = r.x1;
        if (r.y1 > d->y1) d->y1 = r.y1;
        return;
    }
    fl_dirty[fl_ndirty++] = r;
}
static struct fb_rect fl_union(struct fb_rect a, struct fb_rect b)
{
    if (a.x0 >= a.x1) return b;
    if (b.x0 >= b.x1) return a;
    struct fb_rect r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
/* a pipe's colour at column k of its lip box (-1 where there is none):
 * body rows, or lip rows */
static int fl_pipe_col(int k, bool lip)
{
    if (k < 0 || k >= FB_LIP_W * FB_S) return -1;
    if (lip) return (int)(fb_lip_cols[k / FB_S] & 0xffffff);
    k -= FB_S;
    return k < 0 || k >= FB_BODY_W * FB_S ? -1 : (int)(fb_body_cols[k / FB_S] & 0xffffff);
}
/* a pipe moved from screen x a to b: the columns that changed colour, as
 * runs, each redrawn above and below the gap */
static void fl_pipe_dirty(const struct fl_pipe *p, int a, int b)
{
    int x0 = a < b ? a : b, x1 = (a > b ? a : b) + FB_LIP_W * FB_S;
    int top = p->gap * FB_S, bot = (p->gap + FL_GAP) * FB_S, ground = FB_GROUND * FB_S;
    if (x1 <= 0 || x0 >= FB_SW || a == b) return;
    int run = -1, last = -100;
    for (int x = x0; x <= x1; x++)
    {
        bool ch = x < x1 && (fl_pipe_col(x - a, false) != fl_pipe_col(x - b, false) || fl_pipe_col(x - a, true) != fl_pipe_col(x - b, true));
        if (ch)
        {
            if (run >= 0 && x - last > 6)   /* a gap of a few pixels joins two runs */
            {
                fl_dirty_add((struct fb_rect){ run, 0, last + 1, top });
                fl_dirty_add((struct fb_rect){ run, bot, last + 1, ground });
                run = -1;
            }
            if (run < 0) run = x;
            last = x;
        }
    }
    if (run >= 0)
    {
        fl_dirty_add((struct fb_rect){ run, 0, last + 1, top });
        fl_dirty_add((struct fb_rect){ run, bot, last + 1, ground });
    }
}
static void fl_invalidate(const struct fb_rect *r)
{
    lv_area_t a = { r->x0, r->y0, r->x1 - 1, r->y1 - 1 };
    lv_obj_invalidate_area(fl.canvas, &a);
}
static void fl_redraw_all(void)
{
    struct fb_rect all = { 0, 0, FB_SW, FB_SH };
    fl_ov_compute(&fl_ov_now, lv_tick_get());
    fl_bird_now(&fl_img);
    fl_compose(&all);
    fl_ov_drawn = fl_ov_now;
    fl.d_cam = fl_cam_px();
    fl.d_bird = fl_img.r;
    fl.d_cy = (int)lroundf(fl_bird_cy());
    fl.d_ang = fl_bird_ang();
    fl.d_wing = fl.wing;
    fl.d_wing_c = fl.colour;
    fl.d_score = fl_score_shown() ? fl.score : -1;
    fl.d_pause = fl_pause_shown() ? 1 + fl.pause_down : 0;
    fl.d_valid = true;
    if (fl.canvas) lv_obj_invalidate(fl.canvas);
}
/* what moved since the last frame, redrawn */
static void fl_draw(void)
{
    if (!fl.d_valid) { fl_redraw_all(); return; }
    uint32_t now = lv_tick_get();
    fl_ndirty = 0;
    int cam = fl_cam_px();
    if (cam != fl.d_cam)
    {
        if (fl.state != FL_READY)
            for (int i = 0; i < FL_PIPES; i++)
            {
                int b = fl_pipe_sx(&fl.pipe[i], cam), a = b + (cam - fl.d_cam);
                fl_pipe_dirty(&fl.pipe[i], a, b);
            }
        fl_dirty_add((struct fb_rect){ 0, FB_STRIPE_Y * FB_S, FB_SW, (FB_STRIPE_Y + FB_STRIPE_H) * FB_S });
    }
    /* the bird: drawn once into its image when it moved, turned or beat its wings */
    int cy = (int)lroundf(fl_bird_cy());
    float ang = fl_bird_ang();
    bool bird = cy != fl.d_cy || ang != fl.d_ang || fl.wing != fl.d_wing || fl.colour != fl.d_wing_c;
    if (bird)
    {
        fl_bird_now(&fl_img);
        fl_dirty_add(fl_union(fl_img.r, fl.d_bird));
    }
    int sc = fl_score_shown() ? fl.score : -1;
    if (sc != fl.d_score)
    {
        struct fb_rect z = { 0, 0, 0, 0 };
        fl_dirty_add(fl_union(fl.d_score >= 0 ? fl_score_rect(fl.d_score) : z, sc >= 0 ? fl_score_rect(sc) : z));
    }
    fl_ov_compute(&fl_ov_now, now);
    const struct fl_ov *o = &fl_ov_now, *d = &fl_ov_drawn;
    bool every = o->which != d->which;
    if (every || o->title_y != d->title_y || o->title_opa != d->title_opa)
    {
        fl_dirty_add(fl_union(fl_ov_title_r(o), fl_ov_title_r(d)));
        fl_dirty_add(fl_union(fl_ov_hint_r(o), fl_ov_hint_r(d)));
    }
    if (every || o->panel_y != d->panel_y || o->count != d->count || o->glint != d->glint || o->medal != d->medal || o->fresh != d->fresh)
    {
        struct fb_rect a = fl_ov_panel_r(o), b = fl_ov_panel_r(d);
        if (a.x0 < a.x1 && b.x0 < b.x1 && a.y0 != b.y0)   /* the panel rising: one band */
            fl_dirty_add(fl_union(a, b));
        else { if (a.x0 < a.x1) fl_dirty_add(a); if (b.x0 < b.x1 && (b.y0 != a.y0 || a.x0 >= a.x1)) fl_dirty_add(b); }
    }
    if (every || o->play != d->play || o->down != d->down)
        fl_dirty_add(fl_union(fl_ov_play_r(o), fl_ov_play_r(d)));
    /* the pause button: shown in flight, pressed in a step */
    int pz = fl_pause_shown() ? 1 + fl.pause_down : 0;
    if (pz != fl.d_pause)
        fl_dirty_add((struct fb_rect){ FL_PAUSE_X, FL_PAUSE_Y, FL_PAUSE_X + fb_pause.w * FB_S, FL_PAUSE_Y + (fb_pause.h + 1) * FB_S });
    fl.d_pause = pz;
    for (int i = 0; i < fl_ndirty; i++) { fl_compose(&fl_dirty[i]); fl_invalidate(&fl_dirty[i]); }
    fl_ov_drawn = fl_ov_now;
    fl.d_cam = cam;
    fl.d_bird = fl_img.r;
    fl.d_cy = cy;
    fl.d_ang = ang;
    fl.d_wing = fl.wing;
    fl.d_wing_c = fl.colour;
    fl.d_score = sc;
}

/* ----------------------------------------------------------- the rules */
static void fl_header_show(bool on);
static void fl_pipe_place(struct fl_pipe *p, int x)
{
    p->x = x;
    p->gap = FL_GAP_LO + (int)(fl_rand() % (FL_GAP_HI - FL_GAP_LO + 1));
    p->scored = false;
}
static void fl_scene_make(int night)
{
    if (fl_bg_made[night]) return;
    static uint32_t art[FB_LW * FB_LH];
    fb_scene_build(art, night);
    fb_scene_blow_up(fl_bg[night], art);
    fl_bg_made[night] = true;
}
static void fl_ink(void)
{
    /* the status bar and the back button in the sky's ink */
    uint32_t ink = fl.night ? 0xffffff : 0x163b40;
    lv_obj_t *hdr = fl.hdr;
    if (hdr && lv_obj_get_child_count(hdr))
    {
        lv_obj_t *b = lv_obj_get_child(hdr, 0);
        for (uint32_t i = 0; i < lv_obj_get_child_count(b); i++)
        {
            lv_obj_t *c = lv_obj_get_child(b, i);
            lv_obj_set_style_text_color(c, lv_color_hex(ink), 0);
            lv_obj_set_style_line_color(c, lv_color_hex(ink), 0);
        }
    }
    statusbar_ink(ink);
    if (npages && pages[npages - 1] == fl.page) sb_ink_pending = false;   /* ours already: not the next page's */
}
static void fl_reset(void)
{
    fl.state = FL_READY;
    fl.y = FB_GROUND * 0.47f;
    fl.vy = 0;
    fl.rot = 0;
    fl.t_acc = 0;
    fl.score = 0;
    fl.new_best = false;
    fl.hit_pipe = false;
    fl.died_at = fl.over_at = fl.hint_gone_at = 0;
    fl.play_down = fl.pause_down = false;
    fl.t0 = lv_tick_get();
    int night = (int)(fl_rand() >> 7) & 1, colour = (int)((fl_rand() >> 9) % 3);
    fl.colour = colour;
    if (night != fl.night || !fl_bg_made[night]) { fl.night = night; fl_scene_make(night); if (fl.page) fl_ink(); }
    fl.d_valid = false;
    fl_header_show(true);
    if (fl.play_btn) lv_obj_add_flag(fl.play_btn, LV_OBJ_FLAG_HIDDEN);
    if (fl.pause_btn) lv_obj_add_flag(fl.pause_btn, LV_OBJ_FLAG_HIDDEN);
}
static void fl_start(void)
{
    fl.state = FL_FLY;
    fl.hint_gone_at = lv_tick_get();
    int x = (int)fl.cam + FB_LW + 30;
    for (int i = 0; i < FL_PIPES; i++) fl_pipe_place(&fl.pipe[i], x + i * FL_SPACING);
    fl_header_show(false);
    lv_obj_remove_flag(fl.pause_btn, LV_OBJ_FLAG_HIDDEN);
}
static void fl_flap(void)
{
    if (!fl.page) return;
    if (fl.state == FL_PAUSE) return;
    if (fl.state == FL_READY) fl_start();
    if (fl.state != FL_FLY) return;
    if (fl.y < -2 * FB_BIRD_H) return;      /* far over the top: no more lift */
    fl.vy = FL_FLAP;
    sfx_play_vary(SFX_FB_WING);
}
static void fl_over(void)
{
    fl.state = FL_OVER;
    fl.over_at = lv_tick_get();
    fl.swooshed = 0;
    fl.medal = fb_medal_for(fl.score);
    if (fl.score > fl_best) { fl_best = fl.score; fl.new_best = true; fl_save(); }
    fl_header_show(true);
}
static void fl_die(bool pipe)
{
    if (fl.state != FL_FLY) return;
    fl.state = FL_FALL;
    fl.died_at = lv_tick_get();
    fl.hit_pipe = pipe;
    if (fl.vy < 0) fl.vy = 0;
    lv_obj_add_flag(fl.pause_btn, LV_OBJ_FLAG_HIDDEN);
    sfx_play(SFX_FB_HIT);
    /* the flash: white, then gone */
    lv_obj_remove_flag(fl.flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(fl.flash, LV_OPA_COVER, 0);
}
/* any drawn pixel of the bird in a pipe, or on the ground: the boxes
 * first, the bird drawn to test only when it is near one */
static bool fl_bird_in(const struct fb_rect *boxes, int n)
{
    struct fb_rect r = fb_bird_bounds(FL_BIRD_CX, fl_bird_cy(), fl_bird_ang()), k;
    bool near = false;
    for (int i = 0; i < n && !near; i++) near = fb_rect_and(&k, &r, &boxes[i]);
    if (!near) return false;
    fl_bird_now(&fl_hit_img);
    for (int i = 0; i < n; i++) if (fb_bird_hits(&fl_hit_img, &boxes[i])) return true;
    return false;
}
static bool fl_hits_pipe(void)
{
    int cam = fl_cam_px(), lw = FB_LIP_W * FB_S, lh = FB_LIP_H * FB_S;
    struct fb_rect boxes[FL_PIPES * 4];
    for (int i = 0; i < FL_PIPES; i++)
    {
        int sx = fl_pipe_sx(&fl.pipe[i], cam), top = fl.pipe[i].gap * FB_S, bot = (fl.pipe[i].gap + FL_GAP) * FB_S;
        struct fb_rect *b = &boxes[i * 4];
        b[0] = (struct fb_rect){ sx + FB_S, -4000, sx + lw - FB_S, top - lh };
        b[1] = (struct fb_rect){ sx, top - lh, sx + lw, top };
        b[2] = (struct fb_rect){ sx, bot, sx + lw, bot + lh };
        b[3] = (struct fb_rect){ sx + FB_S, bot + lh, sx + lw - FB_S, FB_GROUND * FB_S };
    }
    return fl_bird_in(boxes, FL_PIPES * 4);
}
static bool fl_on_ground(void)
{
    struct fb_rect g = { 0, FB_GROUND * FB_S, FB_SW, FB_SH + 400 };
    return fl_bird_in(&g, 1);
}
static void fl_step(void)
{
    if (fl.state == FL_FLY || fl.state == FL_FALL)
    {
        fl.vy += FL_GRAVITY * FL_STEP;
        if (fl.vy > FL_TERMINAL) fl.vy = FL_TERMINAL;
        fl.y += fl.vy * FL_STEP;
        /* the turn: up while climbing, then down the faster it falls */
        float want = fl.state == FL_FALL ? 90 : fl.vy < 40 ? FL_TILT_UP : FL_TILT_UP + (fl.vy - 40) / (FL_TERMINAL - 40) * (90 - FL_TILT_UP);
        float rate = want < fl.rot ? 1200 : 600;
        if (want < fl.rot) { fl.rot -= rate * FL_STEP; if (fl.rot < want) fl.rot = want; }
        else { fl.rot += rate * FL_STEP; if (fl.rot > want) fl.rot = want; }
    }
    if (fl.state == FL_READY || fl.state == FL_FLY)
    {
        fl.cam += FL_SPEED * FL_STEP;
        fl.wing_t += FL_STEP;
    }
    if (fl.state == FL_FLY)
    {
        int cam = fl_cam_px();
        float far = 0;
        for (int i = 0; i < FL_PIPES; i++) if (fl.pipe[i].x > far) far = fl.pipe[i].x;
        for (int i = 0; i < FL_PIPES; i++)
        {
            struct fl_pipe *p = &fl.pipe[i];
            if (fl_pipe_sx(p, cam) + FB_LIP_W * FB_S < 0) { fl_pipe_place(p, (int)far + FL_SPACING); far = p->x; }
            if (!p->scored && p->x * FB_S + FB_LIP_W * FB_S / 2 - cam <= FL_BIRD_X * FB_S + FB_S / 2)
            {
                p->scored = true;
                fl.score++;
                sfx_play(SFX_FB_POINT);
            }
        }
    }
    /* the wings beat while it flies, and stop when it dives or falls */
    if (fl.state == FL_READY || (fl.state == FL_FLY && fl.rot < 70))
    {
        static const int8_t beat[4] = { 0, 1, 2, 1 };
        fl.wing = beat[(int)(fl.wing_t / 0.09f) & 3];
    }
    else fl.wing = 1;
    if (fl.state == FL_FLY || fl.state == FL_FALL)
    {
        if (fl.state == FL_FLY && fl_hits_pipe()) fl_die(true);
        if (fl_on_ground())
        {
            /* down: stood on the ground's top, not in it */
            while (fl_on_ground()) fl.y -= 1.0f / FB_S;
            fl.vy = 0;
            if (fl.state == FL_FLY) fl_die(false);
            if (fl.state == FL_FALL) fl_over();
        }
    }
}
static bool fl_on_top(void) { return fl.page && npages && pages[npages - 1] == fl.page; }
static void fl_pause(void)
{
    fl.state = FL_PAUSE;
    lv_obj_add_flag(fl.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fl.play_btn, LV_OBJ_FLAG_HIDDEN);
    fl_header_show(true);
}
static void fl_resume(void)
{
    fl.state = FL_FLY;
    fl.last = lv_tick_get();
    fl.play_down = false;
    lv_obj_add_flag(fl.play_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fl.pause_btn, LV_OBJ_FLAG_HIDDEN);
    fl_header_show(false);
    sfx_play(SFX_FB_SWOOSH);
}
static void fl_tick(lv_timer_t *tm)
{
    (void)tm;
    if (!fl.page) return;
    uint32_t now = lv_tick_get();
    if ((!fl_on_top() || sl_power_hides_ui() || overlay_visible()) && (fl.state == FL_FLY || fl.state == FL_FALL))
        { if (fl.state == FL_FLY) fl_pause(); }
    float dt = (now - fl.last) / 1000.0f;
    fl.last = now;
    if (dt > 0.1f) dt = 0.1f;
    if (fl.state != FL_PAUSE)
    {
        fl.t_acc += dt;
        while (fl.t_acc >= FL_STEP) { fl_step(); fl.t_acc -= FL_STEP; }
    }
    /* the die sound as it drops from a pipe; the swooshes of Game Over */
    if (fl.state == FL_FALL && fl.hit_pipe && fl.died_at && now - fl.died_at >= 280) { sfx_play(SFX_FB_DIE); fl.hit_pipe = false; }
    if (fl.state == FL_OVER)
    {
        uint32_t t = now - fl.over_at;
        int stage = t >= 600 ? 2 : t >= 250 ? 1 : 0;
        if (stage > fl.swooshed) { fl.swooshed = stage; sfx_play(SFX_FB_SWOOSH); }
        if (t >= 950 && lv_obj_has_flag(fl.play_btn, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(fl.play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (fl.died_at && !lv_obj_has_flag(fl.flash, LV_OBJ_FLAG_HIDDEN))
    {
        uint32_t t = now - fl.died_at;
        if (t >= 70) lv_obj_add_flag(fl.flash, LV_OBJ_FLAG_HIDDEN);   /* white, then gone: a blend across the screen would cost frames */
    }
    fl_draw();
}

/* ---------------------------------------------------------- the input */
static void fl_canvas_cb(lv_event_t *e)
{
    (void)e;
    if (fl.state == FL_PAUSE) { fl_resume(); return; }
    fl_flap();
}
static void fl_again(void)
{
    sfx_play(SFX_FB_SWOOSH);
    fl_reset();
    fl_redraw_all();
}
static void fl_play_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    bool down = c == LV_EVENT_PRESSED;
    if (down != fl.play_down) { fl.play_down = down; fl_draw(); }
    if (c != LV_EVENT_CLICKED) return;
    fl.play_down = false;
    if (fl.state == FL_PAUSE) fl_resume();
    else if (fl.state == FL_OVER) fl_again();
}
static void fl_pause_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    bool down = c == LV_EVENT_PRESSED;
    if (down != fl.pause_down) { fl.pause_down = down; fl_draw(); }
    if (c == LV_EVENT_CLICKED && fl.state == FL_FLY) { fl.pause_down = false; fl_pause(); }
}
/* Play and Next: flap; after a round, go again; paused, carry on. Held, a
 * key flaps once. */
static bool fl_key(int btn)
{
    int b = btn & ~(BUTTON_REL | BUTTON_REPEAT);
    if (!fl_on_top() || (b != BUTTON_LEFT && b != BUTTON_RIGHT)) return false;
    if (btn & (BUTTON_REL | BUTTON_REPEAT)) return true;
    if (fl.state == FL_PAUSE) fl_resume();
    else if (fl.state == FL_OVER) { if (lv_tick_get() - fl.over_at >= 950) fl_again(); }
    else fl_flap();
    return true;
}

/* ----------------------------------------------------------- the page */
static void fl_header_show(bool on)
{
    lv_obj_t *hdr = fl.hdr;
    if (!hdr) return;
    if (on) lv_obj_remove_flag(hdr, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(hdr, LV_OBJ_FLAG_HIDDEN);
}
static void fl_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != fl.page) return;
    if (fl.timer) lv_timer_delete(fl.timer);
    memset(&fl, 0, sizeof fl);
    /* 2.2 MB: the skies are made again for the next open, the canvas redrawn */
    sl_give_back(fl_px, sizeof fl_px);
    sl_give_back(fl_bg, sizeof fl_bg);
    fl_bg_made[0] = fl_bg_made[1] = false;
    statusbar_retheme();
}
static lv_obj_t *fl_hit_box(lv_obj_t *p, int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *o = box(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(o, cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(o, cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(o, cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}
static void flappy_page(void)
{
    if (page_bring_back(fl.page)) return;
    fl_load();
    fl_rng ^= lv_tick_get() * 2654435761u;
    if (!fl_rng) fl_rng = 1;
    if (!fl_art_made)
    {
        fb_bird_build();
        fb_medal_build();
        fb_pipe_build();
        fb_stripes_build(fl_strip);
        fb_sprites_build();
        fl_art_made = true;
    }
    lv_obj_t *p, *body = page_new("Flappy Bird", &p);
    memset(&fl, 0, sizeof fl);
    fl.page = p;
    fl.night = -1;
    lv_obj_add_event_cb(p, fl_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    /* the header keeps only its back button: the sky is the page */
    lv_obj_t *hdr = fl.hdr = lv_obj_get_child(p, 0);
    for (uint32_t i = npages > 0 ? 1 : 0; i < lv_obj_get_child_count(hdr); i++) lv_obj_add_flag(lv_obj_get_child(hdr, i), LV_OBJ_FLAG_HIDDEN);

    fl.canvas = lv_canvas_create(p);
    lv_canvas_set_buffer(fl.canvas, fl_px, FB_SW, FB_SH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(fl.canvas, 0, 0);
    lv_obj_add_flag(fl.canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe over the sky */
    lv_obj_add_event_cb(fl.canvas, fl_canvas_cb, LV_EVENT_PRESSED, NULL);

    /* the words, in the language, as pixel art */
    fl_text_make(&fl_t_ready, sl_tr("Get Ready!"), true);
    fl_text_make(&fl_t_over, sl_tr("Game Over"), true);
    fl_text_make(&fl_t_paused, sl_tr("Paused"), true);
    fl_text_make(&fl_t_medal, sl_tr("Medal"), false);
    fl_text_make(&fl_t_score, sl_tr("Score"), false);
    fl_text_make(&fl_t_best, sl_tr("Best"), false);
    fl_text_make(&fl_t_new, sl_tr("New"), false);

    /* the buttons drawn on the canvas take their taps here */
    fl.pause_btn = fl_hit_box(p, 0, SB_H, 72, SL_HDR_H, fl_pause_cb);
    fl.play_btn = fl_hit_box(p, (FB_LW - fb_play.w) / 2 * FB_S - 6, FL_PLAY_Y * FB_S - 6, fb_play.w * FB_S + 12, fb_play.h * FB_S + 12, fl_play_cb);
    fl.flash = box(p);
    lv_obj_set_size(fl.flash, W, H);
    lv_obj_set_style_bg_color(fl.flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(fl.flash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(fl.flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(fl.flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(hdr);           /* the back button over the sky */

    fl_reset();
    fl.last = lv_tick_get();
    fl_redraw_all();
    fl.timer = lv_timer_create(fl_tick, 16, NULL);
    page_push(p);
}
