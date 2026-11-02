/* Snake in the spirit of Tiny Decisions. GPLv2+.
 *
 * Clean paper (white in the light theme, true black in the dark ones), a
 * soft chequered board, grey pills and round buttons, and one glossy snake.
 *
 * The rules still step on a grid (sloppy_snake_state.h). Only the picture is
 * continuous. The snake is drawn every frame as one tapered tube along a
 * path that runs through the cell centres and turns corners on quarter
 * circles, and the whole body slides along that path by how far we are
 * between two steps, so it glides and curves instead of hopping. The head
 * trails the rules by half a cell; that is what lets it round a corner
 * before the corner exists.
 *
 * The board is one RGB565 canvas drawn by hand with analytic anti-aliasing.
 * Each frame restores the background only under what was drawn last frame
 * and redraws there, so a short snake costs a few thousand pixels. */
#include "sloppy_snake_state.h"
#define SNAKE_CELL 27
#define SNAKE_BOARD (SL_SNAKE_GRID * SNAKE_CELL)       /* 432 */
#define SNAKE_BX ((W - SNAKE_BOARD) / 2)
#define SNAKE_BY (SB_H + 66 + 60)
#define SNAKE_SCORES ROCKBOX_DIR "/sloppy/snake-scores.txt"
#define SNAKE_R 10.4f                                   /* body radius */
#define SNAKE_RMAX 16.0f                                /* head + gulp, with room */
#define SNAKE_PRIMS (2 * SL_SNAKE_MAX + 8)
#define SNAKE_BITS 18
#define SNAKE_POP_MS 380

#define SNK_LIGHT (sl_theme() == 1)
#define SNK_ZULE sl_zune()                   /* black, square, hot pink */
#define SNK_PAPER (SNK_LIGHT ? 0xffffff : 0x000000)
#define SNK_INK (SNK_LIGHT ? 0x1d1d1f : 0xffffff)
#define SNK_GREY (SNK_LIGHT ? 0x6e6e73 : 0x9a9aa2)   /* 5:1 on the light board's white */
#define SNK_SOFT (SNK_LIGHT ? 0x5c5c63 : 0xc7c7cc)
#define SNK_FILL (SNK_LIGHT ? 0xf0f0f4 : 0x1c1c1e)
#define SNK_FILL_P (SNK_LIGHT ? 0xdedee4 : 0x2c2c2e)
#define SNK_THUMB (SNK_LIGHT ? 0xffffff : SNK_ZULE ? ZN_PINK : 0x3a3a3c)
#define SNK_TILE_A (SNK_LIGHT ? 0xf5f5f8 : SNK_ZULE ? 0x070707 : 0x141416)
#define SNK_TILE_B (SNK_LIGHT ? 0xececf1 : SNK_ZULE ? 0x101010 : 0x1b1b1e)
#define SNK_BLUE (SNK_ZULE ? ZN_PINK : 0x3478f6)
#define SNK_GOLD 0xf5b82e
#define SNK_GREEN 0x28b463
#define SNK_PINK 0xe8577a

struct snk_prim {
    bool arc;
    float ax, ay, bx, by;           /* ends; for an arc, its first and last point */
    float dx, dy, il2;              /* segment: b - a and 1 / |b - a|^2 */
    float cx, cy, uax, uay, ubx, uby, sweep, sg;   /* arc: centre, end directions */
    float p0, p1;                   /* path parameter at a and at b, in cells */
    int x0, y0, x1, y1;             /* pixel box, clipped */
};

static struct sl_snake_state snake_state;
static uint16_t snake_bg_px[SNAKE_BOARD * SNAKE_BOARD];
static uint16_t snake_px[SNAKE_BOARD * SNAKE_BOARD];
static struct snk_prim snk_prim[SNAKE_PRIMS];
static int snk_nprim;

static lv_obj_t *snake_page_obj, *snake_board, *snake_overlay, *snake_overlay_label, *snake_overlay_sub;
static lv_obj_t *snake_again, *snake_score, *snake_best_label, *snake_pause_label, *snake_plus;
static lv_obj_t *snake_mode_buttons[2], *snake_mode_labels[2], *snake_thumb;
static lv_timer_t *snake_timer;
static int snake_best[2];
static bool snake_scores_loaded, snake_scores_dirty, snake_paused;
static uint32_t snake_level_banner_until;
static bool snake_started;              /* waits for the first swipe or arrow */
static int snk_seg_w = 100;               /* each half of the Classic | Levels pill */
static int snake_speed = SL_SNAKE_NORMAL; /* Relaxed, Normal or Fast: on the cards, kept with the bests */
static lv_obj_t *snake_speed_row, *snake_speed_chips[3], *snake_flash;
static uint32_t snk_gold_at, snk_ring_at;
static float snk_ring_x, snk_ring_y;
static bool snk_ring_gold, snk_eaten_gold;
#define SNK_GOLD_MS 6000                  /* a golden apple waits this long, then turns red */
static void snk_flash_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void snk_flash_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

/* the picture's own state */
static uint32_t snk_step_at, snk_pause_at, snk_die_at, snk_food_at, snk_gulp_at, snk_burst_at;
static uint32_t snk_pop_at, snk_card_in, snk_card_out, snk_bump_at, snk_blink_at, snk_tongue_at;
static uint32_t snk_last, snk_seed;
static bool snk_fresh = true, snk_grew = true, snk_pending, snk_bg_dirty = true, snk_new_best;
static int snk_trail[2][2];             /* the cells the tail left behind */
static int snk_shown = -1, snk_eaten_x, snk_eaten_y, snk_baked, snk_card_key = -1, snk_card_level;
static int snk_dirty[4], snk_prev[4];   /* x0 y0 x1 y1, exclusive ends */
static float snk_ph, snk_pt, snk_len_px, snk_head_x, snk_head_y, snk_fx = 1, snk_fy;
static float snk_bit_x[SNAKE_BITS], snk_bit_y[SNAKE_BITS], snk_bit_vx[SNAKE_BITS], snk_bit_vy[SNAKE_BITS];
static float snk_bit_r[SNAKE_BITS];
static uint32_t snk_bit_col[SNAKE_BITS];
static bool snk_bits_live;

static uint32_t snk_now(void) { return lv_tick_get(); }
static float snk_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float snk_smooth(float v) { v = snk_clamp01(v); return v * v * (3 - 2 * v); }
static float snk_back(float v)          /* ease out with a little overshoot */
{
    v = snk_clamp01(v) - 1;
    return 1 + v * v * (2.7f * v + 1.7f);
}
static uint32_t snk_rand(void)
{
    snk_seed = snk_seed * 1664525u + 1013904223u;
    return snk_seed >> 8;
}

static void snake_scores_load(void)
{
    if (snake_scores_loaded) return;
    snake_scores_loaded = true;
    int fd = open(SNAKE_SCORES, O_RDONLY);
    if (fd < 0) return;
    char b[64];
    ssize_t got = read(fd, b, sizeof b - 1);
    close(fd);
    if (got <= 0) return;
    b[got] = 0;
    int classic, levels, speed = SL_SNAKE_NORMAL;
    if ((sscanf(b, "SNAKE2 %d %d %d", &classic, &levels, &speed) == 3 || sscanf(b, "SNAKE1 %d %d", &classic, &levels) == 2) &&
        classic >= 0 && levels >= 0 && classic <= 1000000 && levels <= 1000000)
    {
        snake_best[0] = classic; snake_best[1] = levels;
        snake_speed = speed >= 0 && speed <= 2 ? speed : SL_SNAKE_NORMAL;
    }
}
static void snake_scores_save(void)
{
    if (!snake_scores_dirty) return;
    char b[64];
    int len = snprintf(b, sizeof b, "SNAKE2 %d %d %d\n", snake_best[0], snake_best[1], snake_speed);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(SNAKE_SCORES ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool okay = write(fd, b, len) == len;
    if (okay) okay = fsync(fd) == 0;   /* a power cut must not leave an empty file */
    if (close(fd) < 0) okay = false;
    if (!okay || rename(SNAKE_SCORES ".tmp", SNAKE_SCORES) < 0)
    { remove(SNAKE_SCORES ".tmp"); return; }
    snake_scores_dirty = false;
}

/* ---- pixels -------------------------------------------------------------- */
static inline uint16_t snk_565(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
/* straight alpha over whatever is already there, a in 0..1 */
static inline void snk_blend(uint16_t *buf, int x, int y, float r, float g, float b, float a)
{
    uint16_t *p = &buf[y * SNAKE_BOARD + x];
    if (a >= 0.996f) { *p = snk_565((int)r, (int)g, (int)b); return; }
    if (a <= 0.004f) return;
    int dr = (*p >> 11) << 3, dg = (*p >> 5 & 63) << 2, db = (*p & 31) << 3;
    dr |= dr >> 5; dg |= dg >> 6; db |= db >> 5;
    *p = snk_565((int)(dr + (r - dr) * a), (int)(dg + (g - dg) * a), (int)(db + (b - db) * a));
}
static void snk_hex(uint32_t c, float *r, float *g, float *b)
{
    *r = c >> 16 & 255; *g = c >> 8 & 255; *b = c & 255;
}
/* clip a float box to the board and add it to this frame's dirty area */
static bool snk_box(float fx0, float fy0, float fx1, float fy1, int *x0, int *y0, int *x1, int *y1, bool mark)
{
    *x0 = (int)floorf(fx0); *y0 = (int)floorf(fy0);
    *x1 = (int)ceilf(fx1) + 1; *y1 = (int)ceilf(fy1) + 1;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > SNAKE_BOARD) *x1 = SNAKE_BOARD;
    if (*y1 > SNAKE_BOARD) *y1 = SNAKE_BOARD;
    if (*x0 >= *x1 || *y0 >= *y1) return false;
    if (mark)
    {
        if (snk_dirty[0] >= snk_dirty[2]) { snk_dirty[0] = *x0; snk_dirty[1] = *y0; snk_dirty[2] = *x1; snk_dirty[3] = *y1; }
        else
        {
            if (*x0 < snk_dirty[0]) snk_dirty[0] = *x0;
            if (*y0 < snk_dirty[1]) snk_dirty[1] = *y0;
            if (*x1 > snk_dirty[2]) snk_dirty[2] = *x1;
            if (*y1 > snk_dirty[3]) snk_dirty[3] = *y1;
        }
    }
    return true;
}
static float snk_len2(float x, float y) { return sqrtf(x * x + y * y); }

/* a soft round blob, or a crisp dot when soft is below 1 */
static void snk_dot(uint16_t *buf, float cx, float cy, float r, float soft, uint32_t col, float opa)
{
    int x0, y0, x1, y1;
    if (!snk_box(cx - r - soft - 1, cy - r - soft - 1, cx + r + soft + 1, cy + r + soft + 1,
                 &x0, &y0, &x1, &y1, buf == snake_px)) return;
    float cr, cg, cb;
    snk_hex(col, &cr, &cg, &cb);
    float w = soft < 1 ? 1 : soft;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float d = snk_len2(x + 0.5f - cx, y + 0.5f - cy) - r;
            float a = soft < 1 ? snk_clamp01(0.5f - d) : snk_smooth(0.5f - d / w);
            if (a > 0) snk_blend(buf, x, y, cr, cg, cb, a * opa);
        }
}

/* a stone block for Levels: a rounded square, lit from the top */
static void snk_block(uint16_t *buf, int cx_cell, int cy_cell, float sc)
{
    if (sc <= 0.02f) return;
    float cx = cx_cell * SNAKE_CELL + SNAKE_CELL / 2.0f, cy = cy_cell * SNAKE_CELL + SNAKE_CELL / 2.0f;
    float hs = 11.5f * sc, rr = 6.5f * sc;
    int x0, y0, x1, y1;
    if (!snk_box(cx - hs - 4, cy - hs - 3, cx + hs + 4, cy + hs + 7, &x0, &y0, &x1, &y1, buf == snake_px)) return;
    bool light = SNK_LIGHT;
    float tr, tg, tb, br, bg, bb;
    snk_hex(light ? 0xd8d8de : 0x55555a, &tr, &tg, &tb);
    snk_hex(light ? 0xa9a9b1 : 0x38383c, &br, &bg, &bb);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            /* shadow, dropped a little */
            float sx = fabsf(px) - (hs - rr), sy = fabsf(py - 3.5f * sc) - (hs - rr);
            float sd = snk_len2(sx > 0 ? sx : 0, sy > 0 ? sy : 0) + fminf(fmaxf(sx, sy), 0) - rr;
            float sa = snk_smooth((3.5f - sd) / 6) * (light ? 0.20f : 0.55f);
            if (sa > 0) snk_blend(buf, x, y, 0, 0, 0, sa);
            float qx = fabsf(px) - (hs - rr), qy = fabsf(py) - (hs - rr);
            float d = snk_len2(qx > 0 ? qx : 0, qy > 0 ? qy : 0) + fminf(fmaxf(qx, qy), 0) - rr;
            float a = snk_clamp01(0.5f - d);
            if (a <= 0) continue;
            float v = snk_clamp01((py + hs) / (2 * hs));
            float lift = d > -2.2f && py < 0 ? 18 * snk_clamp01((d + 2.2f) / 1.6f) * snk_clamp01(-py / hs) : 0;
            snk_blend(buf, x, y, tr + (br - tr) * v + lift, tg + (bg - tg) * v + lift,
                      tb + (bb - tb) * v + lift, a);
        }
}

static void snk_paint_bg(void)
{
    float pr, pg, pb, ar, ag, ab, br, bg, bb;
    snk_hex(SNK_PAPER, &pr, &pg, &pb);
    snk_hex(SNK_TILE_A, &ar, &ag, &ab);
    snk_hex(SNK_TILE_B, &br, &bg, &bb);
    const float rad = 22, half = SNAKE_BOARD / 2.0f;
    for (int y = 0; y < SNAKE_BOARD; y++)
        for (int x = 0; x < SNAKE_BOARD; x++)
        {
            float qx = fabsf(x + 0.5f - half) - (half - rad), qy = fabsf(y + 0.5f - half) - (half - rad);
            float d = snk_len2(qx > 0 ? qx : 0, qy > 0 ? qy : 0) + fminf(fmaxf(qx, qy), 0) - rad;
            float a = snk_clamp01(0.5f - d);
            bool odd = ((x / SNAKE_CELL) + (y / SNAKE_CELL)) & 1;
            float r = odd ? br : ar, g = odd ? bg : ag, b = odd ? bb : ab;
            snake_bg_px[y * SNAKE_BOARD + x] = snk_565((int)(pr + (r - pr) * a), (int)(pg + (g - pg) * a),
                                                      (int)(pb + (b - pb) * a));
        }
    for (int i = 0; i < snk_baked && i < snake_state.obstacle_count; i++)
        snk_block(snake_bg_px, snake_state.obstacle_x[i], snake_state.obstacle_y[i], 1);
}

/* ---- the path ------------------------------------------------------------ */
/* cell k of the path, counted from the head: -1 is where the head is going,
 * len and len + 1 are the cells the tail has just left */
static void snk_cell(int k, float *x, float *y)
{
    const struct sl_snake_state *s = &snake_state;
    int cx, cy;
    if (k < 0) { cx = s->x[0] + s->dx; cy = s->y[0] + s->dy; }
    else if (k < s->len) { cx = s->x[k]; cy = s->y[k]; }
    else if (k - s->len < 2) { cx = snk_trail[k - s->len][0]; cy = snk_trail[k - s->len][1]; }
    else
    {
        int j = k - s->len - 1;
        cx = snk_trail[1][0] + (snk_trail[1][0] - snk_trail[0][0]) * j;
        cy = snk_trail[1][1] + (snk_trail[1][1] - snk_trail[0][1]) * j;
    }
    *x = cx * SNAKE_CELL + SNAKE_CELL / 2.0f;
    *y = cy * SNAKE_CELL + SNAKE_CELL / 2.0f;
}
/* the piece of path around cell k runs from the edge it shares with k - 1
 * to the edge it shares with k + 1: straight, or a quarter circle */
static bool snk_piece(int k, float *ax, float *ay, float *bx, float *by, float *cx, float *cy)
{
    float px, py, x, y, nx, ny;
    snk_cell(k - 1, &px, &py);
    snk_cell(k, &x, &y);
    snk_cell(k + 1, &nx, &ny);
    *ax = (px + x) / 2; *ay = (py + y) / 2;
    *bx = (x + nx) / 2; *by = (y + ny) / 2;
    *cx = *ax + *bx - x; *cy = *ay + *by - y;
    return fabsf((px - x) * (ny - y) - (py - y) * (nx - x)) > 0.5f;   /* a turn */
}
static void snk_piece_at(int k, float f, float *x, float *y)
{
    float ax, ay, bx, by, cx, cy;
    if (snk_piece(k, &ax, &ay, &bx, &by, &cx, &cy))
    {
        float c = cosf(f * 1.5707963f), s = sinf(f * 1.5707963f);
        *x = cx + (ax - cx) * c + (bx - cx) * s;
        *y = cy + (ay - cy) * c + (by - cy) * s;
    }
    else { *x = ax + (bx - ax) * f; *y = ay + (by - ay) * f; }
}
/* p counts cells from the head: cell k's centre is at p = k */
static void snk_path_at(float p, float *x, float *y)
{
    int k = (int)floorf(p + 0.5f);
    snk_piece_at(k, p - (k - 0.5f), x, y);
}

static void snk_prim_box(struct snk_prim *g, float ex, float ey)
{
    float mx0 = fminf(fminf(g->ax, g->bx), ex), my0 = fminf(fminf(g->ay, g->by), ey);
    float mx1 = fmaxf(fmaxf(g->ax, g->bx), ex), my1 = fmaxf(fmaxf(g->ay, g->by), ey);
    snk_box(mx0 - SNAKE_RMAX, my0 - SNAKE_RMAX, mx1 + SNAKE_RMAX, my1 + SNAKE_RMAX + 6,
            &g->x0, &g->y0, &g->x1, &g->y1, false);
}
static void snk_build_path(void)
{
    snk_nprim = 0;
    float ph = snk_ph, pt = snk_pt;
    int k0 = (int)floorf(ph + 0.5f), k1 = (int)floorf(pt + 0.5f);
    if (k1 > k0 && pt - (k1 - 0.5f) < 1e-4f) k1--;
    for (int k = k0; k <= k1 && snk_nprim < SNAKE_PRIMS; k++)
    {
        float base = k - 0.5f;
        float fa = snk_clamp01(ph - base), fb = snk_clamp01(pt - base);
        if (fb < fa) continue;
        float ax, ay, bx, by, cx, cy;
        bool turn = snk_piece(k, &ax, &ay, &bx, &by, &cx, &cy);
        struct snk_prim *g = &snk_prim[snk_nprim];
        if (!turn)
        {
            float sx = ax + (bx - ax) * fa, sy = ay + (by - ay) * fa;
            float ex = ax + (bx - ax) * fb, ey = ay + (by - ay) * fb;
            struct snk_prim *prev = snk_nprim ? &snk_prim[snk_nprim - 1] : NULL;
            /* straight runs are one long segment */
            if (prev && !prev->arc && fabsf(prev->bx - sx) < 0.01f && fabsf(prev->by - sy) < 0.01f &&
                fabsf((prev->bx - prev->ax) * (ey - sy) - (prev->by - prev->ay) * (ex - sx)) < 0.01f)
            {
                prev->bx = ex; prev->by = ey; prev->p1 = base + fb;
                g = prev;
            }
            else
            {
                *g = (struct snk_prim){ .arc = false, .ax = sx, .ay = sy, .bx = ex, .by = ey,
                                        .p0 = base + fa, .p1 = base + fb };
                snk_nprim++;
            }
            g->dx = g->bx - g->ax; g->dy = g->by - g->ay;
            float l2 = g->dx * g->dx + g->dy * g->dy;
            g->il2 = l2 > 1e-6f ? 1 / l2 : 0;
            snk_prim_box(g, g->ax, g->ay);
        }
        else
        {
            float va = fa * 1.5707963f, vb = fb * 1.5707963f;
            float ux = (ax - cx) / (SNAKE_CELL / 2.0f), uy = (ay - cy) / (SNAKE_CELL / 2.0f);
            float wx = (bx - cx) / (SNAKE_CELL / 2.0f), wy = (by - cy) / (SNAKE_CELL / 2.0f);
            *g = (struct snk_prim){ .arc = true, .cx = cx, .cy = cy, .p0 = base + fa, .p1 = base + fb };
            g->uax = ux * cosf(va) + wx * sinf(va); g->uay = uy * cosf(va) + wy * sinf(va);
            g->ubx = ux * cosf(vb) + wx * sinf(vb); g->uby = uy * cosf(vb) + wy * sinf(vb);
            g->sweep = vb - va;
            g->sg = ux * wy - uy * wx > 0 ? 1 : -1;
            g->ax = cx + g->uax * SNAKE_CELL / 2; g->ay = cy + g->uay * SNAKE_CELL / 2;
            g->bx = cx + g->ubx * SNAKE_CELL / 2; g->by = cy + g->uby * SNAKE_CELL / 2;
            float mx, my;
            snk_piece_at(k, (fa + fb) / 2, &mx, &my);
            snk_prim_box(g, mx, my);
            snk_nprim++;
        }
    }
}
/* distance to a piece of path, the nearest point on it and its parameter */
static inline float snk_prim_d(const struct snk_prim *g, float px, float py, float *qx, float *qy, float *prm)
{
    if (!g->arc)
    {
        float pax = px - g->ax, pay = py - g->ay;
        float h = snk_clamp01((pax * g->dx + pay * g->dy) * g->il2);
        *qx = g->ax + g->dx * h; *qy = g->ay + g->dy * h;
        *prm = g->p0 + (g->p1 - g->p0) * h;
        return snk_len2(px - *qx, py - *qy);
    }
    float vx = px - g->cx, vy = py - g->cy, l = snk_len2(vx, vy);
    if (g->sweep > 1e-4f && l > 1e-4f &&
        g->sg * (g->uax * vy - g->uay * vx) >= 0 && g->sg * (vx * g->uby - vy * g->ubx) >= 0)
    {
        float ang = atan2f(g->sg * (g->uax * vy - g->uay * vx), g->uax * vx + g->uay * vy);
        *prm = g->p0 + (g->p1 - g->p0) * snk_clamp01(ang / g->sweep);
        *qx = g->cx + vx / l * (SNAKE_CELL / 2.0f); *qy = g->cy + vy / l * (SNAKE_CELL / 2.0f);
        return fabsf(l - SNAKE_CELL / 2.0f);
    }
    float da = snk_len2(px - g->ax, py - g->ay), db = snk_len2(px - g->bx, py - g->by);
    if (da <= db) { *qx = g->ax; *qy = g->ay; *prm = g->p0; return da; }
    *qx = g->bx; *qy = g->by; *prm = g->p1;
    return db;
}
/* the body tapers from a round head to a fine tail and swells where a
 * swallowed apple is on its way down */
static float snk_radius(float s, float gulp_s, float gulp_amp)
{
    float r = SNAKE_R + 1.8f * fmaxf(0, 1 - s / 22);
    float taper = fminf(snk_len_px * 0.6f, 74);
    float into = s - (snk_len_px - taper);
    if (into > 0) { float t = into / taper; r -= 5.4f * t * t; }
    if (gulp_amp > 0)
    {
        float z = (s - gulp_s) / 8.5f;
        if (z > -3 && z < 3) r += gulp_amp * exp(-z * z);
    }
    return r;
}
static bool snk_owner(int i, float px, float py, float d)
{
    for (int j = i - 2; j <= i + 2; j++)
    {
        if (j == i || j < 0 || j >= snk_nprim) continue;
        float qx, qy, prm;
        float dj = snk_prim_d(&snk_prim[j], px, py, &qx, &qy, &prm);
        if (dj < d || (dj == d && j < i)) return false;
    }
    return true;
}

/* ---- the apple ----------------------------------------------------------- */
static float snk_apple_sdf(float x, float y, float sc)
{
    float r = 7.3f * sc;
    float d1 = snk_len2(x + 2.7f * sc, y - 0.8f * sc) - r, d2 = snk_len2(x - 2.7f * sc, y - 0.8f * sc) - r;
    float k = 2.6f * sc, h = snk_clamp01(0.5f + 0.5f * (d2 - d1) / k);
    return d2 + (d1 - d2) * h - k * h * (1 - h);
}
static bool snk_gold_now;                /* the apple being drawn is the golden one */
static void snk_apple(float cx, float cy, float sc, float lift, float opa)
{
    if (sc <= 0.02f || opa <= 0) return;
    int x0, y0, x1, y1;
    if (!snk_box(cx - 13 * sc, cy - 16 * sc - lift, cx + 13 * sc, cy + 14 * sc, &x0, &y0, &x1, &y1, true)) return;
    float shadow = SNK_LIGHT ? 0.20f : 0.5f;
    float ay = cy - lift;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float px = x + 0.5f, py = y + 0.5f;
            /* its shadow stays on the board and shrinks as it bobs up */
            float sw = (8.5f - lift * 0.8f) * sc, sh = 3.2f * sc;
            float sd = (snk_len2((px - cx) / sw, (py - cy - 9.5f * sc) / sh) - 1) * sh;
            float sa = snk_smooth(0.5f - sd / 3) * shadow * opa * (1 - lift * 0.08f);
            if (sa > 0) snk_blend(snake_px, x, y, 0, 0, 0, sa);
            float lx = px - cx, ly = py - ay;
            /* stem */
            float sx = lx - 0.3f * sc, sy = ly + 5 * sc, vx = 0.9f * sc, vy = -5.2f * sc;
            float t = snk_clamp01((sx * vx + sy * vy) / (vx * vx + vy * vy));
            float ds = snk_len2(sx - vx * t, sy - vy * t) - 1.1f * sc;
            float a = snk_clamp01(0.5f - ds);
            if (a > 0) snk_blend(snake_px, x, y, 142, 92, 48, a * opa);
            /* body */
            float d = snk_apple_sdf(lx, ly, sc);
            a = snk_clamp01(0.5f - d);
            if (a > 0)
            {
                float R = 9.2f * sc, nx = lx / R, ny = (ly - 0.8f * sc) / R;
                float n2 = nx * nx + ny * ny;
                float nz = n2 < 1 ? sqrtf(1 - n2) : 0;
                float lam = fmaxf(0, -0.40f * nx - 0.60f * ny + 0.69f * nz);
                float ndh = fmaxf(0, -0.22f * nx - 0.33f * ny + 0.92f * nz);
                float sp = ndh * ndh; sp *= sp; sp *= sp; sp *= sp; sp *= sp;
                float k = 0.50f + 0.62f * lam;
                float ar = 255, ag = snk_gold_now ? 186 : 66, ab = snk_gold_now ? 46 : 72;
                snk_blend(snake_px, x, y, ar * k + 255 * sp * 0.8f, ag * k + 255 * sp * 0.8f,
                          ab * k + 255 * sp * 0.8f, a * opa);
            }
            /* leaf, tilted up to the right */
            float qx = lx - 3.9f * sc, qy = ly + 9.6f * sc, c = 0.866f, s = -0.5f;
            float ex = qx * c + qy * s, ey = -qx * s + qy * c;
            float dl = (snk_len2(ex / (4.6f * sc), ey / (2.1f * sc)) - 1) * 2.1f * sc;
            a = snk_clamp01(0.5f - dl);
            if (a > 0)
            {
                float v = snk_clamp01(0.5f + ey / (4.2f * sc));
                snk_blend(snake_px, x, y, 72 - 30 * v, 214 - 50 * v, 104 - 30 * v, a * opa);
            }
        }
}

/* ---- the snake ----------------------------------------------------------- */
static void snk_body_colour(float t, float *r, float *g, float *b)
{
    if (SNK_ZULE)
    {
        /* Zule: hot pink at the head to a pale rose at the tail */
        *r = 242 + (255 - 242) * t;
        *g = 40 + (150 - 40) * t;
        *b = 122 + (196 - 122) * t;
        return;
    }
    /* spring green at the head to a minty teal at the tail */
    *r = 64 + (46 - 64) * t;
    *g = 208 + (190 - 208) * t;
    *b = 96 + (200 - 96) * t;
}
static void snk_draw_snake(uint32_t now)
{
    float gulp_s = 0, gulp_amp = 0;
    if (snk_gulp_at)
    {
        float t = (now - snk_gulp_at) / (float)sl_snake_period(&snake_state);
        gulp_s = 14 + t * SNAKE_CELL * 1.6f;
        if (gulp_s > snk_len_px + 20) snk_gulp_at = 0;
        else gulp_amp = 3.4f * snk_clamp01(t * 3) * (1 - snk_clamp01((gulp_s - snk_len_px * 0.7f) / (snk_len_px * 0.3f + 1)));
    }
    float dead = snake_state.alive || snake_state.won ? 0 : snk_clamp01((now - snk_die_at - 120) / 500.0f);
    float shadow = SNK_LIGHT ? 0.17f : 0.45f;
    /* shadows first, so no body pixel is ever darkened by a neighbour's */
    for (int i = 0; i < snk_nprim; i++)
    {
        const struct snk_prim *g = &snk_prim[i];
        int y0 = g->y0 + 3, y1 = g->y1 + 3;
        if (y1 > SNAKE_BOARD) y1 = SNAKE_BOARD;
        int x0, x1, yy0, yy1;
        if (!snk_box(g->x0, y0, g->x1 - 1, y1 - 1, &x0, &yy0, &x1, &yy1, true)) continue;
        for (int y = yy0; y < yy1; y++)
            for (int x = x0; x < x1; x++)
            {
                float px = x + 0.5f, py = y + 0.5f - 4, qx, qy, prm;
                float d = snk_prim_d(g, px, py, &qx, &qy, &prm);
                if (d > SNAKE_RMAX + 3) continue;
                float r = snk_radius((prm - snk_ph) * SNAKE_CELL, gulp_s, gulp_amp);
                float a = snk_smooth((r + 3 - d) / 7) * shadow;
                if (a <= 0 || !snk_owner(i, px, py, d)) continue;
                snk_blend(snake_px, x, y, 0, 0, 0, a);
            }
    }
    for (int i = 0; i < snk_nprim; i++)
    {
        const struct snk_prim *g = &snk_prim[i];
        int x0, y0, x1, y1;
        if (!snk_box(g->x0, g->y0, g->x1 - 1, g->y1 - 1, &x0, &y0, &x1, &y1, true)) continue;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                float px = x + 0.5f, py = y + 0.5f, qx, qy, prm;
                float d = snk_prim_d(g, px, py, &qx, &qy, &prm);
                if (d > SNAKE_RMAX) continue;
                float s = (prm - snk_ph) * SNAKE_CELL;
                float r = snk_radius(s, gulp_s, gulp_amp);
                float cov = snk_clamp01(r - d + 0.5f);
                if (cov <= 0 || !snk_owner(i, px, py, d)) continue;
                float nx = (px - qx) / r, ny = (py - qy) / r, n2 = nx * nx + ny * ny;
                if (n2 > 1) { float l = sqrtf(n2); nx /= l; ny /= l; n2 = 1; }
                float nz = sqrtf(1 - n2);
                float lam = fmaxf(0, -0.40f * nx - 0.60f * ny + 0.69f * nz);
                float ndh = fmaxf(0, -0.22f * nx - 0.33f * ny + 0.92f * nz);
                float sp = ndh * ndh; sp *= sp; sp *= sp; sp *= sp; sp *= sp;
                float cr, cg, cb;
                snk_body_colour(snk_len_px > 1 ? s / snk_len_px : 0, &cr, &cg, &cb);
                /* pale spots down the back, carried along with the body */
                if (s > 20)
                {
                    float m = fmodf(s, SNAKE_CELL) - SNAKE_CELL / 2.0f;
                    float spot = 3.4f * r / SNAKE_R - snk_len2(m, d);
                    float k = snk_clamp01(spot + 0.5f) * 0.38f;
                    cr += (236 - cr) * k; cg += (255 - cg) * k; cb += (214 - cb) * k;
                }
                float k = 0.46f + 0.66f * lam;
                cr *= k; cg *= k; cb *= k;
                cr += 255 * sp * 0.55f; cg += 255 * sp * 0.55f; cb += 255 * sp * 0.55f;
                if (dead > 0)
                {
                    /* the colour drains from the head back to the tail */
                    float tpos = snk_len_px > 1 ? s / snk_len_px : 0;
                    float dk = snk_clamp01(dead * 1.9f - tpos * 0.9f);
                    float grey = cr * 0.3f + cg * 0.59f + cb * 0.11f;
                    grey = grey * 0.8f + 30;
                    cr += (grey - cr) * dk * 0.75f; cg += (grey - cg) * dk * 0.75f; cb += (grey - cb) * dk * 0.75f;
                }
                snk_blend(snake_px, x, y, cr, cg, cb, cov);
            }
    }
}

static void snk_line(float ax, float ay, float bx, float by, float w, uint32_t col)
{
    int x0, y0, x1, y1;
    if (!snk_box(fminf(ax, bx) - w - 1, fminf(ay, by) - w - 1, fmaxf(ax, bx) + w + 1, fmaxf(ay, by) + w + 1,
                 &x0, &y0, &x1, &y1, true)) return;
    float cr, cg, cb, dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    snk_hex(col, &cr, &cg, &cb);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float h = l2 > 0 ? snk_clamp01((px * dx + py * dy) / l2) : 0;
            float a = snk_clamp01(w + 0.5f - snk_len2(px - dx * h, py - dy * h));
            if (a > 0) snk_blend(snake_px, x, y, cr, cg, cb, a);
        }
}

static void snk_draw_face(uint32_t now, float look_x, float look_y)
{
    float hx = snk_head_x, hy = snk_head_y, fx = snk_fx, fy = snk_fy, nx = -fy, ny = fx;
    bool alive = snake_state.alive || snake_state.won;
    /* a flick of the tongue now and then, more often near the apple */
    if (alive && snk_tongue_at && now >= snk_tongue_at)
    {
        uint32_t t = now - snk_tongue_at;
        if (t < 260)
        {
            float e = sinf(t / 260.0f * 3.14159f) * 8.5f;
            float bx = hx + fx * 11.5f, by = hy + fy * 11.5f, tx = bx + fx * e, ty = by + fy * e;
            snk_line(bx, by, tx, ty, 0.9f, 0xff3b6b);
            float fk = e / 8.5f * 3.2f;
            snk_line(tx, ty, tx + (fx * 0.8f + nx * 0.6f) * fk, ty + (fy * 0.8f + ny * 0.6f) * fk, 0.75f, 0xff3b6b);
            snk_line(tx, ty, tx + (fx * 0.8f - nx * 0.6f) * fk, ty + (fy * 0.8f - ny * 0.6f) * fk, 0.75f, 0xff3b6b);
        }
        else
        {
            float d = snk_len2(look_x - hx, look_y - hy);
            snk_tongue_at = now + (d < SNAKE_CELL * 2.5f ? 700 : 2200) + snk_rand() % 2400;
        }
    }
    /* eyes: they follow the apple and blink every few seconds */
    bool blink = alive && snk_blink_at && now >= snk_blink_at && now - snk_blink_at < 130;
    if (snk_blink_at && now - snk_blink_at >= 130 && now >= snk_blink_at)
        snk_blink_at = now + 2200 + snk_rand() % 3200;
    float lx = look_x - hx, ly = look_y - hy, ll = snk_len2(lx, ly);
    if (ll > 1) { lx /= ll; ly /= ll; } else { lx = fx; ly = fy; }
    for (int side = -1; side <= 1; side += 2)
    {
        float ex = hx + fx * 3.4f + nx * 5.5f * side, ey = hy + fy * 3.4f + ny * 5.5f * side;
        if (!alive)
        {
            snk_dot(snake_px, ex, ey, 4.4f, 0, 0xffffff, 1);
            snk_line(ex - 2.1f, ey - 2.1f, ex + 2.1f, ey + 2.1f, 0.9f, 0x2c2c2e);
            snk_line(ex - 2.1f, ey + 2.1f, ex + 2.1f, ey - 2.1f, 0.9f, 0x2c2c2e);
        }
        else if (blink)
            snk_line(ex - fx * 3.4f, ey - fy * 3.4f, ex + fx * 3.4f, ey + fy * 3.4f, 1.0f, 0x1f5e33);
        else
        {
            snk_dot(snake_px, ex, ey, 4.4f, 0, 0xffffff, 1);
            float px = ex + lx * 1.5f, py = ey + ly * 1.5f;
            snk_dot(snake_px, px, py, 2.5f, 0, 0x1d1d1f, 1);
            snk_dot(snake_px, px - 0.9f, py - 1.0f, 0.9f, 0, 0xffffff, 1);
        }
    }
}

/* ---- confetti -------------------------------------------------------------- */
static void snk_burst(float x, float y, int n, float power)
{
    static const uint32_t cols[] = { 0xff375f, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff, 0xbf5af2 };
    snk_burst_at = snk_now();
    snk_bits_live = true;
    for (int i = 0; i < SNAKE_BITS; i++)
    {
        if (i >= n) { snk_bit_r[i] = 0; continue; }
        float a = (i + (snk_rand() % 100) / 100.0f) * 6.2831853f / n;
        float v = power * (70 + snk_rand() % 110);
        snk_bit_x[i] = x; snk_bit_y[i] = y;
        snk_bit_vx[i] = cosf(a) * v; snk_bit_vy[i] = sinf(a) * v - 60 * power;
        snk_bit_r[i] = 1.8f + (snk_rand() % 100) / 55.0f;
        snk_bit_col[i] = cols[(i + snk_rand() % 2) % 6];
    }
}
static void snk_draw_bits(uint32_t now, float dt)
{
    if (!snk_bits_live) return;
    uint32_t t = now - snk_burst_at;
    if (t > 900) { snk_bits_live = false; return; }
    float fade = 1 - snk_clamp01((t - 450) / 450.0f);
    for (int i = 0; i < SNAKE_BITS; i++)
    {
        if (snk_bit_r[i] <= 0) continue;
        snk_bit_vy[i] += 420 * dt;
        snk_bit_vx[i] *= 1 - 1.6f * dt;
        snk_bit_x[i] += snk_bit_vx[i] * dt;
        snk_bit_y[i] += snk_bit_vy[i] * dt;
        snk_dot(snake_px, snk_bit_x[i], snk_bit_y[i], snk_bit_r[i], 0, snk_bit_col[i], fade);
    }
}

/* ---- one frame of the board ------------------------------------------------ */
static float snk_u(uint32_t now)
{
    if (snk_fresh) return 1;
    uint32_t at = snake_paused ? snk_pause_at : now;
    return snk_clamp01((int32_t)(at - snk_step_at) / (float)sl_snake_period(&snake_state));
}
static void snake_draw(uint32_t now)
{
    if (!snake_board) return;
    float dt = snk_last ? (now - snk_last) / 1000.0f : 0;
    if (dt > 0.05f) dt = 0.05f;
    snk_last = now;
    if (snk_bg_dirty)
    {
        snk_paint_bg();
        snk_bg_dirty = false;
        snk_prev[0] = snk_prev[1] = 0; snk_prev[2] = snk_prev[3] = SNAKE_BOARD;
    }
    /* put the board back under last frame's drawing */
    for (int y = snk_prev[1]; y < snk_prev[3]; y++)
        memcpy(&snake_px[y * SNAKE_BOARD + snk_prev[0]], &snake_bg_px[y * SNAKE_BOARD + snk_prev[0]],
               (snk_prev[2] - snk_prev[0]) * sizeof(uint16_t));
    snk_dirty[0] = snk_dirty[1] = snk_dirty[2] = snk_dirty[3] = 0;

    /* where the body is along its path this frame */
    const struct sl_snake_state *s = &snake_state;
    float u = snk_u(now);
    snk_ph = 1.5f - u;
    if (!s->alive && !s->won)       /* lunge into whatever it hit */
        snk_ph = 0.5f - 0.4f * snk_back((now - snk_die_at) / 170.0f);
    snk_pt = snk_grew ? s->len - 0.5f : snk_ph + s->len - 1;
    snk_len_px = (snk_pt - snk_ph) * SNAKE_CELL;
    snk_path_at(snk_ph, &snk_head_x, &snk_head_y);
    float bx, by;
    snk_path_at(snk_ph + 0.04f, &bx, &by);
    float fl = snk_len2(snk_head_x - bx, snk_head_y - by);
    if (fl > 1e-3f) { snk_fx = (snk_head_x - bx) / fl; snk_fy = (snk_head_y - by) / fl; }
    snk_build_path();

    /* Levels walls that have just arrived drop in one after another */
    if (snk_baked < s->obstacle_count)
    {
        bool done = true;
        for (int i = snk_baked; i < s->obstacle_count; i++)
        {
            int32_t t = (int32_t)(now - snk_pop_at) - (i - snk_baked) * 55;
            if (t < SNAKE_POP_MS) done = false;
            snk_block(snake_px, s->obstacle_x[i], s->obstacle_y[i], t <= 0 ? 0 : snk_back(t / (float)SNAKE_POP_MS));
        }
        if (done) { snk_baked = s->obstacle_count; snk_bg_dirty = true; }
    }

    /* the apple the snake is about to swallow, and the next one */
    float apple_x = -100, apple_y = -100;
    float bob = 1.6f + 1.6f * sinf(now * 0.0052f);
    if (snk_pending)
    {
        float ex = snk_eaten_x * SNAKE_CELL + SNAKE_CELL / 2.0f, ey = snk_eaten_y * SNAKE_CELL + SNAKE_CELL / 2.0f;
        float d = snk_len2(ex - snk_head_x, ey - snk_head_y);
        if (d < SNAKE_CELL * 0.62f || (!s->alive && !s->won) || snk_fresh)
        {
            snk_pending = false;
            snk_burst(ex, ey, snk_eaten_gold ? SNAKE_BITS : 12, snk_eaten_gold ? 1.5f : 1);
            snk_ring_at = now; snk_ring_x = ex; snk_ring_y = ey; snk_ring_gold = snk_eaten_gold;
            snk_gulp_at = now;
            snk_food_at = now;
            snk_bump_at = now;
        }
        else
        {
            snk_gold_now = snk_eaten_gold;
            snk_apple(ex, ey, 1 - 0.25f * snk_clamp01(1 - d / SNAKE_CELL), bob, 1);
            snk_gold_now = false;
            apple_x = ex; apple_y = ey;
        }
    }
    if (!snk_pending && s->food_x >= 0)
    {
        float fx = s->food_x * SNAKE_CELL + SNAKE_CELL / 2.0f, fy = s->food_y * SNAKE_CELL + SNAKE_CELL / 2.0f;
        uint32_t t = now - snk_food_at;
        if (t < 520)            /* a ripple where it lands */
        {
            float k = t / 520.0f;
            int x0, y0, x1, y1;
            float rr = 5 + 20 * k;
            if (snk_box(fx - rr - 2, fy - rr - 2, fx + rr + 2, fy + rr + 2, &x0, &y0, &x1, &y1, true))
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                    {
                        float d = fabsf(snk_len2(x + 0.5f - fx, y + 0.5f - fy) - rr);
                        float a = snk_clamp01(1.3f - d) * 0.45f * (1 - k);
                        if (a > 0) snk_blend(snake_px, x, y, 255, 69, 72, a);
                    }
        }
        snk_gold_now = s->food_gold;
        snk_apple(fx, fy, snk_back(t / 360.0f), bob, 1);
        snk_gold_now = false;
        if (s->food_gold && snk_gold_at)
        {
            /* its clock: a gold ring round it, running out */
            float left = 1 - snk_clamp01((int32_t)((snake_paused ? snk_pause_at : now) - snk_gold_at) / (float)SNK_GOLD_MS);
            int x0, y0, x1, y1;
            const float rr = 15.5f;
            if (snk_box(fx - rr - 2, fy - rr - 2, fx + rr + 2, fy + rr + 2, &x0, &y0, &x1, &y1, true))
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                    {
                        float px = x + 0.5f - fx, py = y + 0.5f - fy;
                        float d = fabsf(snk_len2(px, py) - rr);
                        float ang = atan2f(px, -py) / 6.2831853f;   /* 0 at twelve, clockwise */
                        if (ang < 0) ang += 1;
                        float a = snk_clamp01(1.6f - d) * (ang <= left ? 0.9f : 0.18f);
                        if (a > 0) snk_blend(snake_px, x, y, 245, 184, 46, a);
                    }
        }
        apple_x = fx; apple_y = fy;
    }
    if (apple_x < -50) { apple_x = snk_head_x + snk_fx * 40; apple_y = snk_head_y + snk_fy * 40; }

    snk_draw_snake(now);
    snk_draw_face(now, apple_x, apple_y);
    snk_draw_bits(now, dt);
    if (snk_ring_at && now - snk_ring_at < 380)
    {
        /* the swallow: a ring goes out from where the apple was */
        float k = (now - snk_ring_at) / 380.0f, rr = 8 + 30 * snk_smooth(k);
        int x0, y0, x1, y1;
        float cr = snk_ring_gold ? 245 : 255, cg = snk_ring_gold ? 184 : 255, cb = snk_ring_gold ? 46 : 255;
        if (SNK_LIGHT && !snk_ring_gold) { cr = 52; cg = 120; cb = 246; }
        if (snk_box(snk_ring_x - rr - 3, snk_ring_y - rr - 3, snk_ring_x + rr + 3, snk_ring_y + rr + 3, &x0, &y0, &x1, &y1, true))
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++)
                {
                    float d = fabsf(snk_len2(x + 0.5f - snk_ring_x, y + 0.5f - snk_ring_y) - rr);
                    float a = snk_clamp01((2.6f - 1.6f * k) - d) * 0.7f * (1 - k);
                    if (a > 0) snk_blend(snake_px, x, y, cr, cg, cb, a);
                }
    }

    /* hand LVGL just the part that changed */
    int u0 = snk_dirty[0] < snk_dirty[2] ? snk_dirty[0] : SNAKE_BOARD, v0 = snk_dirty[1] < snk_dirty[3] ? snk_dirty[1] : SNAKE_BOARD;
    int u1 = snk_dirty[0] < snk_dirty[2] ? snk_dirty[2] : 0, v1 = snk_dirty[1] < snk_dirty[3] ? snk_dirty[3] : 0;
    if (snk_prev[0] < snk_prev[2])
    {
        if (snk_prev[0] < u0) u0 = snk_prev[0];
        if (snk_prev[1] < v0) v0 = snk_prev[1];
        if (snk_prev[2] > u1) u1 = snk_prev[2];
        if (snk_prev[3] > v1) v1 = snk_prev[3];
    }
    memcpy(snk_prev, snk_dirty, sizeof snk_prev);
    if (u0 < u1 && v0 < v1)
    {
        lv_area_t a;
        lv_obj_get_coords(snake_board, &a);
        a.x2 = a.x1 + u1 - 1; a.y2 = a.y1 + v1 - 1;
        a.x1 += u0; a.y1 += v0;
        lv_obj_invalidate_area(snake_board, &a);
    }
}

/* ---- words around the board ------------------------------------------------ */
static void snake_ui(uint32_t now)
{
    if (!snake_page_obj) return;
    const struct sl_snake_state *s = &snake_state;
    int mode = s->levels ? 1 : 0;
    int shown = s->score | snake_best[mode] << 16 | snake_paused << 30 | (s->alive && snake_started) << 31;
    if (shown != snk_shown)             /* labels only change when their words do */
    {
        snk_shown = shown;
        lv_label_set_text_fmt(snake_score, "%d", s->score);
        lv_label_set_text_fmt(snake_best_label, sl_tr("Best %d"), snake_best[mode]);
        lv_label_set_text(snake_pause_label, snake_paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        if (s->alive && snake_started) lv_obj_remove_state(lv_obj_get_parent(snake_pause_label), LV_STATE_DISABLED);
        else lv_obj_add_state(lv_obj_get_parent(snake_pause_label), LV_STATE_DISABLED);
    }

    bool banner = snake_level_banner_until && (int32_t)(snake_level_banner_until - sl_ms()) > 0;
    int key = s->won ? 5 : !s->alive ? 4 : snake_paused ? 2 : banner ? 3 : !snake_started ? 1 : 0;
    if (key == snk_card_key && !(key == 3 && snk_card_level != s->level)) return;
    if (key == 0)
    {
        if (snk_card_key > 0) snk_card_out = now;
        snk_card_key = 0;
        return;
    }
    snk_card_key = key;
    snk_card_level = s->level;
    snk_card_out = 0;
    snk_card_in = now + (key == 4 || key == 5 ? 650 : 0);
    uint32_t title_col = SNK_INK;
    char sub[80];
    const char *title;
    if (key == 1)
    {
        title = s->levels ? "Levels" : "Classic";
        snprintf(sub, sizeof sub, "%s", sl_tr(s->levels ? "New walls every 10 points. Gold apples count 3.\nSwipe to start"
                                                         : "Eat the apples, miss the walls. Gold ones count 3.\nSwipe to start"));
    }
    else if (key == 2) { title = "Paused"; snprintf(sub, sizeof sub, "%s", sl_tr("Tap play to keep going")); }
    else if (key == 3)
    {
        static char lv[48];
        snprintf(lv, sizeof lv, sl_tr("Level %d"), s->level);
        title = lv;
        snprintf(sub, sizeof sub, "%s", sl_tr("New walls ahead"));
        title_col = SNK_BLUE;
    }
    else
    {
        title = key == 5 ? "You win!" : snk_new_best ? "New best!" : "Game over";
        if (key == 5 || snk_new_best) title_col = key == 5 ? SNK_GREEN : SNK_PINK;
        snprintf(sub, sizeof sub, sl_tr(s->score == 1 ? "%d point" : "%d points"), s->score);
    }
    if (key == 1 || key >= 4) lv_obj_remove_flag(snake_speed_row, LV_OBJ_FLAG_HIDDEN);   /* the pace, chosen before a game */
    else lv_obj_add_flag(snake_speed_row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(snake_overlay_label, sl_tr(title));
    lv_obj_set_style_text_color(snake_overlay_label, lv_color_hex(title_col), 0);
    lv_label_set_text(snake_overlay_sub, sub);
    if (key >= 4) lv_obj_remove_flag(snake_again, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(snake_again, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(snake_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(snake_overlay);
}
/* fades, the card sliding in, the score bumping, the crash shake */
static void snake_animate(uint32_t now)
{
    if (snk_card_key > 0)
    {
        float k = snk_smooth((int32_t)(now - snk_card_in) / 240.0f);
        lv_obj_set_style_opa(snake_overlay, (lv_opa_t)(255 * k), 0);
        lv_obj_set_style_translate_y(snake_overlay, (int32_t)(16 * (1 - k)), 0);
    }
    else if (snk_card_out)
    {
        float k = 1 - snk_clamp01((now - snk_card_out) / 160.0f);
        lv_obj_set_style_opa(snake_overlay, (lv_opa_t)(255 * k), 0);
        if (k <= 0) { lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN); snk_card_out = 0; }
    }
    uint32_t t = now - snk_bump_at;
    int scale = 256;
    if (snk_bump_at && t < 320) scale = 256 + (int)(80 * sinf(t / 320.0f * 3.14159f) * (1 - t / 320.0f));
    lv_obj_set_style_transform_scale(snake_score, scale, 0);
    if (snk_bump_at && t < 700)
    {
        float k = t / 700.0f;
        lv_obj_remove_flag(snake_plus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(snake_plus, SNAKE_BX + (int)(snk_eaten_x * SNAKE_CELL + SNAKE_CELL / 2) - 16,
                       SNAKE_BY + snk_eaten_y * SNAKE_CELL - 12 - (int)(30 * snk_smooth(k)));
        lv_obj_set_style_opa(snake_plus, (lv_opa_t)(255 * (1 - k * k)), 0);
    }
    else lv_obj_add_flag(snake_plus, LV_OBJ_FLAG_HIDDEN);
    int shake = 0;
    if (!snake_state.alive && !snake_state.won && snk_die_at)
    {
        int32_t d = (int32_t)(now - snk_die_at) - 120;
        if (d > 0 && d < 480) shake = (int)(7 * sinf(d * 0.075f) * exp(-d / 140.0f));
    }
    lv_obj_set_style_translate_x(snake_board, shake, 0);
}

/* ---- rules ------------------------------------------------------------------ */
/* the board as it was before the last step, for a turn that re-steers it */
static struct sl_snake_state snk_before;
static int snk_before_trail[2][2];
static bool snk_before_grew, snk_last_plain;
static void snake_advance(uint32_t now)
{
    struct sl_snake_state *s = &snake_state;
    int old_level = s->level, old_len = s->len, tx = s->x[s->len - 1], ty = s->y[s->len - 1];
    snk_before = *s;
    memcpy(snk_before_trail, snk_trail, sizeof snk_trail);
    snk_before_grew = snk_grew;
    enum sl_snake_result result = sl_snake_step(s);
    snk_last_plain = result == SL_SNAKE_MOVED && s->level == old_level;
    snk_fresh = false;
    if (result != SL_SNAKE_DEAD)
    {
        snk_grew = s->len > old_len;
        if (!snk_grew)
        {
            snk_trail[1][0] = snk_trail[0][0]; snk_trail[1][1] = snk_trail[0][1];
            snk_trail[0][0] = tx; snk_trail[0][1] = ty;
        }
    }
    bool was_gold = snk_before.food_gold;
    if ((result == SL_SNAKE_ATE || result == SL_SNAKE_WON) && s->score)
    {
        if (snk_pending) { snk_burst(snk_eaten_x * SNAKE_CELL + 13.5f, snk_eaten_y * SNAKE_CELL + 13.5f, 12, 1); snk_food_at = now; }
        snk_pending = true;
        snk_eaten_gold = was_gold;
        snk_eaten_x = s->x[0]; snk_eaten_y = s->y[0];
        char plus[8];
        snprintf(plus, sizeof plus, "+%d", s->score - snk_before.score);
        lv_label_set_text(snake_plus, plus);
        lv_obj_set_style_text_color(snake_plus, lv_color_hex(was_gold ? SNK_GOLD : SNK_ZULE ? ZN_PINK : SNK_GREEN), 0);
        if (s->food_gold) snk_gold_at = now;   /* the next one is golden: its clock starts */
    }
    if (s->level > old_level) { snake_level_banner_until = sl_ms() + 900; snk_pop_at = now + 250; }
    if (result == SL_SNAKE_ATE) sfx_play_vary(was_gold ? SFX_SNAKE_GOLD : SFX_SNAKE_EAT);
    else if (result == SL_SNAKE_DEAD)
    {
        sfx_play(SFX_SNAKE_DIE);
        snk_die_at = now;
        /* the board flashes red and fades back */
        if (snake_flash)
        {
            lv_obj_remove_flag(snake_flash, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(snake_flash);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, snake_flash);
            lv_anim_set_exec_cb(&a, snk_flash_cb);
            lv_anim_set_values(&a, 150, 0);
            lv_anim_set_duration(&a, 520);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_completed_cb(&a, snk_flash_done);
            lv_anim_start(&a);
        }
    }
    else if (result == SL_SNAKE_WON)
    {
        sfx_play(SFX_WIN);
        snk_burst(SNAKE_BOARD / 2.0f, SNAKE_BOARD / 2.0f, SNAKE_BITS, 2.2f);
    }
    int mode = s->levels ? 1 : 0;
    if (s->score > snake_best[mode])
    {
        snake_best[mode] = s->score;
        snake_scores_dirty = true;
        snk_new_best = true;
    }
    if (!s->alive) snake_scores_save();
}
static bool snake_on_top(void) { return snake_page_obj && npages && pages[npages - 1] == snake_page_obj; }
static bool snake_running(void)
{
    return snake_page_obj && npages && pages[npages - 1] == snake_page_obj &&
           !lv_obj_has_flag(snake_page_obj, LV_OBJ_FLAG_HIDDEN) && snake_state.alive &&
           snake_started && !snake_paused;
}
/* one step of the rules, now (the UI harness steps the game with it) */
static void __attribute__((unused)) snake_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!snake_running()) return;
    if (snake_level_banner_until && (int32_t)(snake_level_banner_until - sl_ms()) > 0) return;
    snake_level_banner_until = 0;
    uint32_t now = snk_now();
    snake_advance(now);
    snk_step_at = now;
    snake_ui(now);
}
static void snake_frame(lv_timer_t *timer)
{
    if (!snake_page_obj || !npages || pages[npages - 1] != snake_page_obj) return;
    uint32_t now = snk_now();
    /* Locking the player or pulling down a panel pauses the game, rather
     * than letting the snake run into a wall unseen. */
    if (snake_running() && (sl_power_hides_ui() || overlay_visible()))
    {
        snake_paused = true;
        snk_pause_at = now;
    }
    if (snake_running())
    {
        if (snake_level_banner_until)
        {
            if ((int32_t)(snake_level_banner_until - sl_ms()) <= 0)
            {
                snake_level_banner_until = 0;
                snake_advance(now);
                snk_step_at = now;
            }
        }
        else if ((int32_t)(now - snk_step_at) >= sl_snake_period(&snake_state))
        {
            snake_advance(now);
            snk_step_at += sl_snake_period(&snake_state);
            if ((int32_t)(now - snk_step_at) >= sl_snake_period(&snake_state)) snk_step_at = now;
        }
    }
    if (snake_running() && snake_state.food_gold && snk_gold_at && (int32_t)(now - snk_gold_at) >= SNK_GOLD_MS)
    {
        snake_state.food_gold = false;     /* too slow: an ordinary apple again */
        snk_food_at = now;
    }
    snake_ui(now);
    snake_draw(now);
    snake_animate(now);
    /* a calm board only needs half the frames */
    bool busy = snake_running() || snk_bits_live || snk_pending || snk_card_out || (snk_ring_at && now - snk_ring_at < 400) ||
                (int32_t)(now - snk_die_at) < 1200 || (int32_t)(now - snk_card_in) < 300 ||
                snk_baked < snake_state.obstacle_count;
    if (timer) lv_timer_set_period(timer, busy ? 16 : 33);
}

static void snake_reset(bool levels)
{
    snake_scores_save();
    uint32_t now = snk_now();
    sl_snake_init_speed(&snake_state, levels, sl_ms() ^ snk_seed, snake_speed);
    snk_gold_at = snk_ring_at = 0;
    snake_started = snake_paused = false;
    snake_level_banner_until = 0;
    snk_fresh = snk_grew = true;
    snk_pending = snk_new_best = false;
    snk_trail[0][0] = snake_state.x[snake_state.len - 1] - 1; snk_trail[0][1] = snake_state.y[0];
    snk_trail[1][0] = snk_trail[0][0] - 1; snk_trail[1][1] = snake_state.y[0];
    snk_baked = 0;
    snk_bg_dirty = true;
    snk_food_at = now;
    snk_gulp_at = snk_bump_at = snk_die_at = 0;
    snk_bits_live = false;
    snk_card_key = -1;
    snk_blink_at = now + 1400;
    snk_tongue_at = now + 900;
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_text_color(snake_mode_labels[i], lv_color_hex((int)levels == i ? (SNK_ZULE ? 0xffffff : SNK_INK) : SNK_GREY), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, snake_thumb);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(snake_thumb), levels ? 4 + snk_seg_w : 4);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    snake_ui(now);
    snake_draw(now);
}
/* The first swipe starts the game straight away, with a step. After that a
 * turn is never left waiting a whole step: one that comes just after a step
 * re-steers that step (the head has hardly left the cell's centre, so the
 * corner still looks right), and one that comes late in a step takes the
 * next step at once. In between it waits for the next centre, as a grid
 * snake must. Reversing onto itself is still refused (sl_snake_turn). */
static void snake_steer(int dx, int dy)
{
    struct sl_snake_state *s = &snake_state;
    if (!s->alive || snake_paused) return;
    uint32_t now = snk_now();
    bool was_straight = s->next_dx == s->dx && s->next_dy == s->dy;
    int ndx = s->next_dx, ndy = s->next_dy;
    sl_snake_turn(s, dx, dy);
    if (!snake_started)
    {
        snake_started = true;
        sfx_play(SFX_TAP);
        snake_advance(now);
        snk_step_at = now;
        snake_ui(now);
        return;
    }
    if (!snake_running() || snake_level_banner_until || snk_fresh ||
        !was_straight || (s->next_dx == ndx && s->next_dy == ndy)) return;
    int period = sl_snake_period(s), since = (int32_t)(now - snk_step_at);
    if (since < period / 4 && snk_last_plain)
    {
        struct sl_snake_state redo = snk_before;
        sl_snake_turn(&redo, dx, dy);
        struct sl_snake_state trial = redo;
        if (redo.next_dx == dx && redo.next_dy == dy && sl_snake_step(&trial) == SL_SNAKE_MOVED)
        {
            snake_state = redo;
            memcpy(snk_trail, snk_before_trail, sizeof snk_trail);
            snk_grew = snk_before_grew;
            snake_advance(snk_step_at);          /* the same moment, a new corner */
            snake_ui(now);
            return;
        }
    }
    if (since >= period * 3 / 5)
    {
        snake_advance(now);
        snk_step_at = now;
        snake_ui(now);
    }
}

/* Swipes, read by touch_read (G_GAME) at the touch rate anywhere below the
 * header, so they never have to start on the board. Each couple of
 * millimetres of travel in a clear direction steers, and the anchor moves
 * with the finger: up, right, down in one stroke is three turns. */
#define SNK_SWIPE_PX 16
static int snk_ax, snk_ay, snk_swipe_dir;
static bool snake_takes_swipe(int y)
{
    return snake_page_obj && npages && pages[npages - 1] == snake_page_obj &&
           !lv_obj_has_flag(snake_page_obj, LV_OBJ_FLAG_HIDDEN) && y >= SB_H + 66 &&
           snake_state.alive && !snake_paused;
}
static void snake_swipe_begin(int x, int y) { snk_ax = x; snk_ay = y; snk_swipe_dir = -1; }
static void snake_swipe_move(int x, int y)
{
    int dx = x - snk_ax, dy = y - snk_ay, adx = abs(dx), ady = abs(dy);
    if ((adx > ady ? adx : ady) < SNK_SWIPE_PX) return;
    if (adx * 2 < ady * 3 && ady * 2 < adx * 3) return;       /* diagonal: wait for a clear lead */
    int dir = adx > ady ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0);
    static const int sx[] = {0, 1, 0, -1}, sy[] = {-1, 0, 1, 0};
    if (dir != snk_swipe_dir) snake_steer(sx[dir], sy[dir]);
    snk_swipe_dir = dir;
    snk_ax = x; snk_ay = y;
}
static void snake_pause_cb(lv_event_t *e)
{
    (void)e;
    if (!snake_state.alive || !snake_started) return;
    uint32_t now = snk_now();
    snake_paused = !snake_paused;
    if (snake_paused) snk_pause_at = now;
    else
    {
        snk_step_at += now - snk_pause_at;
        if (snk_gold_at) snk_gold_at += now - snk_pause_at;   /* a pause stops the golden apple's clock too */
    }
    sfx_play(SFX_TAP);
    snake_ui(now);
}
static void snake_mode_cb(lv_event_t *e)
{
    bool levels = (intptr_t)lv_event_get_user_data(e) != 0;
    if (levels != snake_state.levels) sfx_play(SFX_TAP);
    snake_reset(levels);
}
static void snake_restart_cb(lv_event_t *e) { (void)e; sfx_play(SFX_START); snake_reset(snake_state.levels); }
/* the pace chips: the one in use filled */
static void snake_speed_show(void)
{
    static const int speed_of[3] = { SL_SNAKE_RELAXED, SL_SNAKE_NORMAL, SL_SNAKE_FAST };
    for (int i = 0; i < 3 && snake_speed_chips[i]; i++)
    {
        bool on = speed_of[i] == snake_speed;
        lv_obj_set_style_bg_color(snake_speed_chips[i], lv_color_hex(on ? SNK_BLUE : SNK_FILL), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(snake_speed_chips[i], 0), lv_color_hex(on ? 0xffffff : SNK_SOFT), 0);
    }
}
static void snake_speed_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == snake_speed) return;
    snake_speed = k;
    snake_scores_dirty = true;
    snake_scores_save();
    sfx_play(SFX_TAP);
    if (!snake_started || !snake_state.alive) snake_reset(snake_state.levels);   /* the game waiting, or over: a fresh one at this pace */
    snake_speed_show();
}
static bool snake_key(int button)
{
    if (button == BUTTON_UP) snake_steer(0, -1);
    else if (button == BUTTON_DOWN) snake_steer(0, 1);
    else if (button == BUTTON_LEFT) snake_steer(-1, 0);
    else if (button == BUTTON_RIGHT) snake_steer(1, 0);
    else return false;
    return true;
}
static void snake_deleted(lv_event_t *e)
{
    (void)e;
    snake_scores_save();
    if (snake_timer) { lv_timer_delete(snake_timer); snake_timer = NULL; }
    if (snake_thumb) lv_anim_delete(snake_thumb, NULL);
    snake_page_obj = snake_board = snake_score = snake_best_label = snake_pause_label = NULL;
    snake_overlay = snake_overlay_label = snake_overlay_sub = snake_again = snake_plus = snake_thumb = NULL;
    snake_mode_buttons[0] = snake_mode_buttons[1] = NULL;
    snake_mode_labels[0] = snake_mode_labels[1] = NULL;
    snake_speed_row = snake_flash = NULL;
    for (int i = 0; i < 3; i++) snake_speed_chips[i] = NULL;
    snk_bits_live = snk_pending = false;
    snk_last = 0;
    sl_give_back(snake_bg_px, sizeof snake_bg_px);   /* 728 KB, painted again on the next open */
    sl_give_back(snake_px, sizeof snake_px);
    snk_bg_dirty = true;
    statusbar_retheme();
}
/* a round grey button, the way Tiny Decisions draws its secondary actions */
static lv_obj_t *snake_round(lv_obj_t *parent, const char *symbol, int x, int y, int size)
{
    lv_obj_t *button = box(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(SNK_FILL), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(SNK_FILL_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_transform_scale(button, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(button, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(button, size / 2, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(text(button, symbol, &sl_sym_22, SNK_SOFT));
    return button;
}
static void snake_page(void)
{
    if (page_bring_back(snake_page_obj)) return;
    lv_obj_t *p, *body = page_new("Snake", &p);
    snake_page_obj = p;
    snake_scores_load();
    snk_seed ^= sl_ms() * 2654435761u;
    lv_obj_add_event_cb(p, snake_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_color(p, lv_color_hex(SNK_PAPER), 0);
    lv_obj_t *header = lv_obj_get_child(p, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(header); i++)
    {
        lv_obj_t *item = lv_obj_get_child(header, i);
        if (!lv_obj_check_type(item, &lv_label_class) && lv_obj_get_child_count(item))
            item = lv_obj_get_child(item, 0);
        if (lv_obj_check_type(item, &lv_label_class))
            lv_obj_set_style_text_color(item, lv_color_hex(SNK_INK), 0);
    }
    statusbar_ink(SNK_INK);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_NONE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);        /* everything is placed by hand */

    /* a segmented pill: Classic | Levels, with a sliding white thumb; each
     * half as wide as the longer of the two words needs (Largest, German) */
    const char *mode_names[2] = { "Classic", "Levels" };
    int tw = 0;
    for (int i = 0; i < 2; i++)
    {
        const char *w = sl_tr(mode_names[i]);
        int x = lv_text_get_width(w, strlen(w), &ss_semibold_18, 0);
        if (x > tw) tw = x;
    }
    snk_seg_w = LV_MAX(100, tw + 2 * SL_GUTTER);
    if (snk_seg_w > (SNAKE_BOARD - 128 - 8) / 2) snk_seg_w = (SNAKE_BOARD - 128 - 8) / 2;   /* and clear of the score */
    lv_obj_t *seg = box(p);
    lv_obj_set_pos(seg, SNAKE_BX, SB_H + 66 + 6);
    lv_obj_set_size(seg, 2 * snk_seg_w + 8, 42);
    lv_obj_set_style_radius(seg, SNK_ZULE ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(seg, lv_color_hex(SNK_FILL), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    snake_thumb = box(seg);
    lv_obj_set_size(snake_thumb, snk_seg_w, 34);
    lv_obj_set_pos(snake_thumb, 4, 4);
    lv_obj_set_style_radius(snake_thumb, SNK_ZULE ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(snake_thumb, lv_color_hex(SNK_THUMB), 0);
    lv_obj_set_style_bg_opa(snake_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(snake_thumb, 10, 0);
    lv_obj_set_style_shadow_offset_y(snake_thumb, 2, 0);
    lv_obj_set_style_shadow_opa(snake_thumb, SNK_LIGHT ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *button = box(seg);
        snake_mode_buttons[i] = button;
        lv_obj_set_size(button, snk_seg_w, 42);
        lv_obj_set_pos(button, i ? 4 + snk_seg_w : 4, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(button, (SL_TAP - 42 + 1) / 2);   /* a finger-sized tap */
        snake_mode_labels[i] = text(button, mode_names[i], &ss_semibold_18, SNK_GREY);
        lv_obj_set_width(snake_mode_labels[i], snk_seg_w - SL_SP_S);
        lv_label_set_long_mode(snake_mode_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(snake_mode_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(snake_mode_labels[i]);
        on_click(button, snake_mode_cb, (void *)(intptr_t)i);
    }
    /* the score, big, and the best under it */
    snake_score = text(p, "0", &ss_bold_28, SNK_INK);
    lv_obj_set_style_text_align(snake_score, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(snake_score, 120);
    lv_obj_set_pos(snake_score, W - SNAKE_BX - 120, SB_H + 66 - 2);
    lv_obj_set_style_transform_pivot_x(snake_score, 100, 0);
    lv_obj_set_style_transform_pivot_y(snake_score, 18, 0);
    snake_best_label = text(p, "", &ss_regular_16, SNK_GREY);
    lv_obj_set_style_text_align(snake_best_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(snake_best_label, 140);
    lv_obj_set_pos(snake_best_label, W - SNAKE_BX - 140, SB_H + 66 + 32);

    /* the board: one canvas, swiped anywhere */
    snake_board = lv_canvas_create(p);
    lv_canvas_set_buffer(snake_board, snake_px, SNAKE_BOARD, SNAKE_BOARD, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(snake_board, SNAKE_BX, SNAKE_BY);
    lv_obj_add_flag(snake_board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe */
    lv_obj_remove_flag(snake_board, LV_OBJ_FLAG_EVENT_BUBBLE);
    snake_plus = text(p, "+1", &ss_semibold_21, SNK_GREEN);
    lv_obj_add_flag(snake_plus, LV_OBJ_FLAG_HIDDEN);
    /* the crash: the board flashes red */
    snake_flash = box(p);
    lv_obj_set_size(snake_flash, SNAKE_BOARD, SNAKE_BOARD);
    lv_obj_set_pos(snake_flash, SNAKE_BX, SNAKE_BY);
    lv_obj_set_style_radius(snake_flash, 22, 0);
    lv_obj_set_style_bg_color(snake_flash, lv_color_hex(0xff2d3a), 0);
    lv_obj_set_style_bg_opa(snake_flash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(snake_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(snake_flash, LV_OBJ_FLAG_HIDDEN);

    /* a floating card for Ready, Paused, Level and Game over */
    snake_overlay = box(p);
    lv_obj_set_width(snake_overlay, 340);
    lv_obj_set_height(snake_overlay, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(snake_overlay, 18, 0);
    lv_obj_set_style_pad_row(snake_overlay, 4, 0);
    lv_obj_set_style_radius(snake_overlay, SL_R_XL, 0);
    lv_obj_set_style_bg_color(snake_overlay, lv_color_hex(SNK_LIGHT ? 0xffffff : 0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(snake_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(snake_overlay, 36, 0);
    lv_obj_set_style_shadow_offset_y(snake_overlay, 10, 0);
    lv_obj_set_style_shadow_opa(snake_overlay, SNK_LIGHT ? LV_OPA_20 : LV_OPA_60, 0);
    lv_obj_set_flex_flow(snake_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(snake_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* anchored low on the board, clear of the snake's starting row */
    lv_obj_align(snake_overlay, LV_ALIGN_BOTTOM_MID, 0, -(H - SNAKE_BY - SNAKE_BOARD + 18));
    snake_overlay_label = text(snake_overlay, "", &ss_bold_28, SNK_INK);
    snake_overlay_sub = text(snake_overlay, "", &ss_regular_18, SNK_GREY);
    lv_obj_set_width(snake_overlay_sub, lv_pct(100));
    lv_obj_set_style_text_align(snake_overlay_sub, LV_TEXT_ALIGN_CENTER, 0);
    /* the pace, picked on the cards before a game: Relaxed, Normal, Fast */
    snake_speed_row = box(snake_overlay);
    lv_obj_set_size(snake_speed_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(snake_speed_row, SL_SP_S, 0);
    lv_obj_set_flex_flow(snake_speed_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(snake_speed_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(snake_speed_row, SL_SP_S, 0);
    static const char *const speed_names[3] = { "Relaxed", "Normal", "Fast" };
    static const int speed_of[3] = { SL_SNAKE_RELAXED, SL_SNAKE_NORMAL, SL_SNAKE_FAST };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *c = box(snake_speed_row);
        lv_obj_set_size(c, 10, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(c, SL_TAP, 0);
        lv_obj_set_style_pad_ver(c, SL_SP_XS, 0);
        lv_obj_set_flex_grow(c, 1);                /* a third of the card each, the words fitted in */
        lv_obj_set_style_radius(c, SNK_ZULE ? 0 : SL_TAP / 2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = text(c, speed_names[i], &ss_semibold_18, SNK_INK);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_hor(c, SL_SP_XS, 0);
        lv_obj_center(l);
        on_click(c, snake_speed_cb, (void *)(intptr_t)speed_of[i]);
        snake_speed_chips[i] = c;
    }
    snake_speed_show();
    snake_again = box(snake_overlay);
    lv_obj_set_size(snake_again, LV_SIZE_CONTENT, 52);   /* as wide as its words need (Largest, Spanish) */
    lv_obj_set_style_min_width(snake_again, 178, 0);
    lv_obj_set_style_pad_hor(snake_again, SL_INSET, 0);
    lv_obj_set_style_margin_top(snake_again, 10, 0);
    lv_obj_set_style_radius(snake_again, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(snake_again, lv_color_hex(SNK_BLUE), 0);
    lv_obj_set_style_bg_color(snake_again, lv_color_hex(0x2a63d4), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(snake_again, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(snake_again, lv_color_hex(SNK_BLUE), 0);
    lv_obj_set_style_shadow_width(snake_again, 16, 0);
    lv_obj_set_style_shadow_offset_y(snake_again, 5, 0);
    lv_obj_set_style_shadow_opa(snake_again, LV_OPA_30, 0);
    lv_obj_set_style_transform_scale(snake_again, 244, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(snake_again, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(snake_again, 26, 0);
    lv_obj_add_flag(snake_again, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(text(snake_again, "Play again", &ss_semibold_21, 0xfffffe));
    on_click(snake_again, snake_restart_cb, NULL);
    lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);

    /* the bottom: pause and new game; steering is by swipe, anywhere */
    int top = SNAKE_BY + SNAKE_BOARD + 22, sz = 60;
    lv_obj_t *pause = snake_round(p, LV_SYMBOL_PAUSE, W / 2 - sz - 20, top, sz);
    snake_pause_label = lv_obj_get_child(pause, 0);
    on_click(pause, snake_pause_cb, NULL);
    lv_obj_t *restart = snake_round(p, LV_SYMBOL_REFRESH, W / 2 + 20, top, sz);
    on_click(restart, snake_restart_cb, NULL);
    lv_obj_t *hint = text(p, "Swipe anywhere to steer", &ss_regular_16, SNK_GREY);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, top + sz + 16);

    snk_prev[0] = snk_prev[1] = snk_prev[2] = snk_prev[3] = 0;
    snk_last = 0;
    snk_shown = -1;
    snake_reset(false);
    lv_obj_set_x(snake_thumb, 4);
    snake_draw(snk_now());   /* the board as it slides in, not a given-back (black) one */
    snake_timer = lv_timer_create(snake_frame, 16, NULL);
    page_push(p);
}
