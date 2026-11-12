/* Chess for GabagoolOS: play the computer at five strengths or pass the
 * player to a friend. The board takes its colour from the accent, the
 * pieces are an ivory and ebony set drawn from distance fields, and every
 * move glides. Drag a piece or tap it and tap where it goes; the game is
 * saved after every move. GPLv2+. */
#include <pthread.h>
#include "sloppy_chess_state.h"
#include "sloppy_chess_art.h"

#define CHESS_FILE ROCKBOX_DIR "/sloppy/chess.dat"
#define CHESS_TEMP ROCKBOX_DIR "/sloppy/chess.tmp"
#define CHESS_SQ 58
#define CHESS_PX (CHESS_SQ * 8)
#define CHESS_BX ((W - CHESS_PX) / 2)
#define CHESS_STRIP_Y (SB_H + 66)
#define CHESS_TOPBAR_Y (CHESS_STRIP_Y + 42)
#define CHESS_BOARD_Y (CHESS_TOPBAR_Y + 62)
#define CHESS_BOTBAR_Y (CHESS_BOARD_Y + CHESS_PX + 6)
#define CHESS_TOOLS_Y (CHESS_BOTBAR_Y + 64)
#define CHESS_SMALL 22
#define CHESS_MID 44
#define CHESS_BIG 96
#define CHESS_BRASS 0xf2c14e
#define CHESS_DRAG_LIFT 26   /* a dragged piece rides above the finger */
/* the game keeps its layout at every text size */
#define CH_R16 (*sl_fonts[0][0])
#define CH_R18 (*sl_fonts[0][1])
#define CH_S18 (*sl_fonts[0][2])
#define CH_S21 (*sl_fonts[0][3])
#define CH_S26 (*sl_fonts[0][5])

static const char *const chess_level_names[CH_LEVELS] = { "Beginner", "Easy", "Medium", "Hard", "Master" };
static const char *const chess_level_blurbs[CH_LEVELS] = {
    "Plays on a whim and leaves pieces hanging.",
    "Looks a move ahead. Takes what you give it.",
    "Sees simple tactics. A fair game.",
    "Calculates deep lines. Plays to win.",
    "Thinks for longer and punishes every slip.",
};

static struct sl_chess_game chess;
static bool chess_loaded;
static char chess_san[CH_MAX_PLIES][8];

/* ---------------------------------------------------------------- art */
/* RGB565A8 sprites: [0..5] white pawn..king, [6..11] black */
static uint8_t chess_spr[12][CHESS_SQ * CHESS_SQ * 3] __attribute__((aligned(4)));
static uint8_t chess_small[12][CHESS_SMALL * CHESS_SMALL * 3] __attribute__((aligned(4)));
static uint8_t chess_mid[2][CHESS_MID * CHESS_MID * 3] __attribute__((aligned(4)));
static uint8_t chess_big[4][CHESS_BIG * CHESS_BIG * 3] __attribute__((aligned(4)));   /* kings: standing, then lying */
static lv_image_dsc_t chess_mid_dsc[2], chess_big_dsc[4], chess_spr_dsc[12];
static bool chess_art_ready;

static int chess_spr_index(int8_t piece) { return (piece > 0 ? piece : -piece) - 1 + (piece < 0 ? 6 : 0); }

static void chess_dsc(lv_image_dsc_t *d, uint8_t *data, int s)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.w = d->header.h = (uint32_t)s;
    d->header.stride = (uint32_t)s * 2;
    d->data = data;
    d->data_size = (uint32_t)s * (uint32_t)s * 3;
}

/* The pieces are the same for a build, and drawing them is the slow part
 * of opening Chess: 0.66 s on the R1 each time, as they are given back
 * when it closes (a test R1's log, Nov 12). Drawn once, they are kept on
 * the card and read back after: the build's name and a sum of the bytes
 * head the file, and anything else in it (another build's, a file cut
 * short, a bad byte) has them drawn and kept again. */
#define CHESS_ART_FILE ROCKBOX_DIR "/sloppy/chess-art.bin"
extern const char rbversion[];
struct chess_art_head { char magic[8]; char build[40]; uint32_t sum; };
static const struct { uint8_t *at; size_t n; } chess_art_parts[4] = {
    { chess_spr[0], sizeof chess_spr }, { chess_small[0], sizeof chess_small },
    { chess_mid[0], sizeof chess_mid }, { chess_big[0], sizeof chess_big } };
static void chess_art_head_now(struct chess_art_head *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->magic, "GCHESS1", 8);
    snprintf(h->build, sizeof h->build, "%s", rbversion);
    uint32_t sum = 2166136261u;
    for (int k = 0; k < 4; k++)
        for (size_t i = 0; i < chess_art_parts[k].n; i++) sum = (sum ^ chess_art_parts[k].at[i]) * 16777619u;
    h->sum = sum;
}
static bool chess_art_load(void)
{
    int fd = open(CHESS_ART_FILE, O_RDONLY);
    if (fd < 0) return false;
    struct chess_art_head h, now;
    bool ok = read(fd, &h, sizeof h) == (ssize_t)sizeof h;
    for (int k = 0; ok && k < 4; k++) ok = read(fd, chess_art_parts[k].at, chess_art_parts[k].n) == (ssize_t)chess_art_parts[k].n;
    char more;
    ok = ok && read(fd, &more, 1) == 0;
    close(fd);
    if (!ok) return false;
    chess_art_head_now(&now);
    return !memcmp(&h, &now, sizeof h);
}
static void chess_art_save(void)
{
    struct chess_art_head h;
    chess_art_head_now(&h);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(CHESS_ART_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, &h, sizeof h) == (ssize_t)sizeof h;
    for (int k = 0; ok && k < 4; k++) ok = write(fd, chess_art_parts[k].at, chess_art_parts[k].n) == (ssize_t)chess_art_parts[k].n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(CHESS_ART_FILE ".tmp", CHESS_ART_FILE) < 0) remove(CHESS_ART_FILE ".tmp");
}
static void chess_art_init(void)
{
    if (chess_art_ready) return;
    uint32_t t0 = sl_ms();
    bool kept = chess_art_load(), ok = true;
    if (!kept)
    {
        for (int t = 1; t <= 6; t++)
        {
            ok &= sl_chess_art_render(t, CHESS_SQ, chess_spr[t - 1], chess_spr[t + 5]);
            ok &= sl_chess_art_render(t, CHESS_SMALL, chess_small[t - 1], chess_small[t + 5]);
        }
        ok &= sl_chess_art_render(CH_K, CHESS_MID, chess_mid[0], chess_mid[1]);
        ok &= sl_chess_art_render(CH_K, CHESS_BIG, chess_big[0], chess_big[1]);
        ok &= sl_chess_art_render_turned(CH_K, CHESS_BIG, chess_big[2], chess_big[3], 1.5708f);
        if (ok) chess_art_save();
    }
    for (int i = 0; i < 12; i++) chess_dsc(&chess_spr_dsc[i], chess_spr[i], CHESS_SQ);
    for (int i = 0; i < 2; i++) chess_dsc(&chess_mid_dsc[i], chess_mid[i], CHESS_MID);
    for (int i = 0; i < 4; i++) chess_dsc(&chess_big_dsc[i], chess_big[i], CHESS_BIG);
    chess_art_ready = ok;
    sl_log("chess: pieces %s in %lu ms", kept ? "read from the card" : "drawn", (unsigned long)(sl_ms() - t0));
}

/* -------------------------------------------------------------- board */
static uint16_t chess_buf[CHESS_PX * CHESS_PX] __attribute__((aligned(4)));
static uint16_t chess_cell[64];          /* each screen cell's colour, lit from the top left */
static uint32_t chess_ink_light, chess_ink_dark;   /* coordinates on light / dark cells */
static uint16_t chess_last565, chess_pick565;      /* the last move, the piece in hand */
static lv_obj_t *chess_page_obj, *chess_canvas, *chess_coord[16];
static struct sl_chess_pos chess_shown;  /* the position on the board */
static int chess_view;                   /* plies shown: chess.plies when live */
static bool chess_flipped;
static uint64_t chess_hidden;            /* cells whose piece a flyer draws instead */
static uint64_t chess_dirty;
static int chess_selected = -1, chess_hover = -1;
static struct sl_chess_move chess_targets[32];
static int chess_ntargets;
static bool chess_promo_open;
static struct sl_chess_move chess_pending;
static const int8_t chess_promo_types[4] = { CH_Q, CH_N, CH_R, CH_B };
static bool chess_thinking;

static uint16_t chess_565(uint32_t c) { return lv_color_to_u16(lv_color_hex(c)); }

static uint32_t chess_scale_rgb(uint32_t c, int permille)
{
    int r = (int)(c >> 16 & 255) * permille / 1000, g = (int)(c >> 8 & 255) * permille / 1000, b = (int)(c & 255) * permille / 1000;
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* The board is the accent's own hue: a pale cream of it and a deep,
 * quiet shade of it, so every GabagoolOS has its own set. A grey accent
 * falls back to walnut. */
static void chess_palette(void)
{
    uint32_t c = sl_accent();
    double r = (c >> 16 & 255) / 255.0, g = (c >> 8 & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn, h = 0.42;
    if (d > 0.0001 && mx > 0 && d / mx > 0.15)
    {
        h = mx == r ? fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        if (h < 0) h += 6;
    }
    uint32_t light = sl_hsv(h, 0.14, 0.9), dark = sl_hsv(h, 0.4, 0.58);
    chess_last565 = chess_565(sl_hsv(h, 0.52, 0.8));
    chess_pick565 = chess_565(sl_hsv(h, 0.62, 0.92));
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++)
        {
            /* a soft light falls from the top left corner */
            int lit = 1030 - (sx + sy) * 60 / 14;
            chess_cell[sy * 8 + sx] = chess_565(chess_scale_rgb((sx + sy) & 1 ? dark : light, lit));
        }
    chess_ink_light = sl_hsv(h, 0.42, 0.5);
    chess_ink_dark = sl_hsv(h, 0.1, 0.94);
}

static inline uint16_t chess_blend(uint16_t fg, uint16_t bg, unsigned a)
{
    uint32_t a5 = (a + 4) >> 3;
    uint32_t f = (fg | (uint32_t)fg << 16) & 0x07e0f81fu, k = (bg | (uint32_t)bg << 16) & 0x07e0f81fu;
    uint32_t m = ((((f - k) * a5) >> 5) + k) & 0x07e0f81fu;
    return (uint16_t)(m | m >> 16);
}

static void chess_fill(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) chess_buf[yy * CHESS_PX + xx] = c;
}

static void chess_tint(int x, int y, int w, int h, uint16_t c, unsigned a)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
        {
            uint16_t *p = &chess_buf[yy * CHESS_PX + xx];
            *p = chess_blend(c, *p, a);
        }
}

/* A sprite with its own alpha, times opa, clipped to the board. */
static void chess_blit(const uint8_t *spr, int s, int x, int y, unsigned opa)
{
    const uint16_t *col = (const uint16_t *)spr;
    const uint8_t *al = spr + s * s * 2;
    int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + s > CHESS_PX ? CHESS_PX - x : s, y1 = y + s > CHESS_PX ? CHESS_PX - y : s;
    for (int yy = y0; yy < y1; yy++)
    {
        uint16_t *dst = &chess_buf[(y + yy) * CHESS_PX + x];
        for (int xx = x0; xx < x1; xx++)
        {
            unsigned a = al[yy * s + xx];
            if (!a) continue;
            if (opa < 255) a = a * opa / 255;
            dst[xx] = a > 250 ? col[yy * s + xx] : chess_blend(col[yy * s + xx], dst[xx], a);
        }
    }
}

/* A soft-edged disc (ring when width > 0) inside cell (x, y). */
static void chess_disc(int x, int y, float radius, float width, uint16_t c, unsigned alpha)
{
    float cx = CHESS_SQ / 2.0f, cy = CHESS_SQ / 2.0f;
    for (int yy = 0; yy < CHESS_SQ; yy++)
        for (int xx = 0; xx < CHESS_SQ; xx++)
        {
            float d = sqrtf((xx + 0.5f - cx) * (xx + 0.5f - cx) + (yy + 0.5f - cy) * (yy + 0.5f - cy));
            float cov = width > 0 ? ca_clamp01(width / 2 + 0.5f - fabsf(d - radius)) : ca_clamp01(radius + 0.5f - d);
            if (cov <= 0) continue;
            uint16_t *p = &chess_buf[(y + yy) * CHESS_PX + x + xx];
            *p = chess_blend(c, *p, (unsigned)(cov * alpha));
        }
}

static int chess_cell_of(int sq)
{
    int f = CH_FILE(sq), r = CH_RANK(sq);
    return chess_flipped ? r * 8 + (7 - f) : (7 - r) * 8 + f;
}

static int chess_sq_of(int cell)
{
    int sx = cell & 7, sy = cell >> 3;
    return chess_flipped ? CH_SQ(7 - sx, sy) : CH_SQ(sx, 7 - sy);
}

static bool chess_target(int sq, const struct sl_chess_move **out)
{
    for (int i = 0; i < chess_ntargets; i++)
        if (chess_targets[i].to == sq) { if (out) *out = &chess_targets[i]; return true; }
    return false;
}

static int chess_promo_slot(int sq)
{
    if (!chess_promo_open) return -1;
    int dir = chess_pending.to >= CH_SQ(0, 7) ? -16 : 16;
    for (int i = 0; i < 4; i++)
        if (sq == chess_pending.to + dir * i) return i;
    return -1;
}

static struct { int8_t piece; int from, to; } chess_drag;   /* to: finger cell coords */
static bool chess_dragging;
static int chess_drag_x, chess_drag_y;   /* sprite top left, board pixels */

static void chess_draw_cell(int cell)
{
    int sq = chess_sq_of(cell), x = (cell & 7) * CHESS_SQ, y = (cell >> 3) * CHESS_SQ;
    const struct sl_chess_pos *p = &chess_shown;
    const struct sl_chess_move *last = chess_view ? &chess.moves[chess_view - 1] : NULL;
    bool dark = ((cell & 7) + (cell >> 3)) & 1;
    uint16_t base = chess_cell[cell];
    if (last && (sq == last->from || sq == last->to)) base = chess_blend(chess_last565, base, dark ? 150 : 170);
    if (sq == chess_selected) base = chess_blend(chess_pick565, base, 215);
    chess_fill(x, y, CHESS_SQ, CHESS_SQ, base);
    int8_t pc = p->board[sq];
    /* check: a red bloom under the king */
    if (pc && (pc > 0 ? pc : -pc) == CH_K && (pc > 0) == (p->side > 0) && sl_chess_in_check(p, p->side))
        for (int yy = 0; yy < CHESS_SQ; yy++)
            for (int xx = 0; xx < CHESS_SQ; xx++)
            {
                float dx = xx + 0.5f - CHESS_SQ / 2.0f, dy = yy + 0.5f - CHESS_SQ / 2.0f;
                float t = 1 - sqrtf(dx * dx + dy * dy) / (CHESS_SQ * 0.68f);
                if (t <= 0) continue;
                uint16_t *px = &chess_buf[(y + yy) * CHESS_PX + x + xx];
                *px = chess_blend(chess_565(0xff3b30), *px, (unsigned)(235 * t * t));
            }
    if (chess_dragging && sq == chess_hover)
        chess_disc(x, y, CHESS_SQ * 0.47f, 4, chess_565(0xffffff), 200);
    const struct sl_chess_move *t = NULL;
    bool target = chess_target(sq, &t);
    if (pc && !(chess_hidden >> cell & 1))
        chess_blit(chess_spr[chess_spr_index(pc)], CHESS_SQ, x, y, chess_dragging && sq == chess_drag.from ? 70 : 255);
    if (target && (t->flags & CHF_CAPTURE))
        chess_disc(x, y, CHESS_SQ * 0.44f, 5, chess_565(0x2b2116), 95);
    else if (target)
        chess_disc(x, y, CHESS_SQ * 0.15f, 0, chess_565(0x2b2116), 80);
    if (chess_promo_open)
    {
        int slot = chess_promo_slot(sq);
        if (slot < 0) chess_tint(x, y, CHESS_SQ, CHESS_SQ, 0, 150);
        else
        {
            chess_tint(x, y, CHESS_SQ, CHESS_SQ, 0, 60);
            chess_disc(x, y, CHESS_SQ * 0.48f, 0, chess_565(0xfaf6ee), 255);
            chess_disc(x, y, CHESS_SQ * 0.48f, 3, chess_565(CHESS_BRASS), 255);
            int8_t side = chess.pos.side;
            chess_blit(chess_spr[chess_spr_index((int8_t)(side * chess_promo_types[slot]))], CHESS_SQ, x, y, 255);
        }
    }
}

/* Pieces in flight: the move animation, or a drag snapping back. */
struct chess_flyer { int8_t piece; int16_t x0, y0, x1, y1; uint8_t a0, a1; };
static struct chess_flyer chess_fly[3];
static int chess_nfly, chess_fly_t;      /* t: 0..1024 */
static int chess_fly_ms;
static bool chess_fly_on;

static void chess_dirty_rect(int x, int y, int w, int h)
{
    int cx0 = x < 0 ? 0 : x / CHESS_SQ, cy0 = y < 0 ? 0 : y / CHESS_SQ;
    int cx1 = (x + w - 1) / CHESS_SQ, cy1 = (y + h - 1) / CHESS_SQ;
    if (cx1 > 7) cx1 = 7;
    if (cy1 > 7) cy1 = 7;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) chess_dirty |= 1ull << (cy * 8 + cx);
}

static void chess_dirty_sq(int sq) { if (sq >= 0 && CH_ONBOARD(sq)) chess_dirty |= 1ull << chess_cell_of(sq); }

static void chess_fly_pos(const struct chess_flyer *f, int *x, int *y, unsigned *a)
{
    *x = f->x0 + (f->x1 - f->x0) * chess_fly_t / 1024;
    *y = f->y0 + (f->y1 - f->y0) * chess_fly_t / 1024;
    *a = (unsigned)(f->a0 + (f->a1 - f->a0) * chess_fly_t / 1024);
}

static void chess_mark_moving(void)
{
    for (int i = 0; i < chess_nfly && chess_fly_on; i++)
    {
        int x, y; unsigned a;
        chess_fly_pos(&chess_fly[i], &x, &y, &a);
        chess_dirty_rect(x, y, CHESS_SQ, CHESS_SQ);
    }
    if (chess_dragging) chess_dirty_rect(chess_drag_x, chess_drag_y, CHESS_SQ, CHESS_SQ);
}

/* Rounded corners like the cards around the board, painted into the
 * buffer in the page's colour: LVGL's clip_corner would mask the whole
 * board on every frame of a move. */
#define CHESS_RADIUS 14
static void chess_round(uint64_t cells)
{
    static const uint8_t corner_cells[4] = { 0, 7, 56, 63 };
    uint16_t bg = chess_565(C_BG);
    for (int k = 0; k < 4; k++)
    {
        if (!(cells >> corner_cells[k] & 1)) continue;
        int right = k & 1, bottom = k >> 1;
        for (int yy = 0; yy < CHESS_RADIUS; yy++)
            for (int xx = 0; xx < CHESS_RADIUS; xx++)
            {
                float dx = CHESS_RADIUS - xx - 0.5f, dy = CHESS_RADIUS - yy - 0.5f;
                float out = ca_clamp01(sqrtf(dx * dx + dy * dy) - CHESS_RADIUS + 0.5f);
                if (out <= 0) continue;
                int x = right ? CHESS_PX - 1 - xx : xx, y = bottom ? CHESS_PX - 1 - yy : yy;
                uint16_t *px = &chess_buf[y * CHESS_PX + x];
                *px = chess_blend(bg, *px, (unsigned)(out * 255));
            }
    }
}

/* Redraw the dirty cells and whatever moves over them; tell LVGL only
 * about that rectangle. */
static void chess_flush(void)
{
    if (!chess_canvas || !chess_dirty) return;
    chess_mark_moving();
    uint64_t corners = chess_dirty;
    int x0 = 8, y0 = 8, x1 = -1, y1 = -1;
    for (int cell = 0; cell < 64; cell++)
        if (chess_dirty >> cell & 1)
        {
            chess_draw_cell(cell);
            int cx = cell & 7, cy = cell >> 3;
            if (cx < x0) x0 = cx;
            if (cy < y0) y0 = cy;
            if (cx > x1) x1 = cx;
            if (cy > y1) y1 = cy;
        }
    chess_dirty = 0;
    for (int i = 0; i < chess_nfly && chess_fly_on; i++)
    {
        int x, y; unsigned a;
        chess_fly_pos(&chess_fly[i], &x, &y, &a);
        if (a) chess_blit(chess_spr[chess_spr_index(chess_fly[i].piece)], CHESS_SQ, x, y, a);
    }
    if (chess_dragging) chess_blit(chess_spr[chess_spr_index(chess_drag.piece)], CHESS_SQ, chess_drag_x, chess_drag_y, 255);
    chess_round(corners);
    lv_area_t at;
    lv_obj_get_coords(chess_canvas, &at);
    lv_area_t a = { at.x1 + x0 * CHESS_SQ, at.y1 + y0 * CHESS_SQ,
                    at.x1 + (x1 + 1) * CHESS_SQ - 1, at.y1 + (y1 + 1) * CHESS_SQ - 1 };
    lv_obj_invalidate_area(chess_canvas, &a);
}

static void chess_coords_update(void)
{
    for (int i = 0; i < 16 && chess_coord[0]; i++)
    {
        char s[2] = { 0, 0 };
        int cell = i < 8 ? 56 + i : (i - 8) * 8;
        s[0] = i < 8 ? (char)('a' + (chess_flipped ? 7 - i : i)) : (char)('8' - (chess_flipped ? 7 - (i - 8) : i - 8));
        lv_label_set_text(chess_coord[i], s);
        bool dark = ((cell & 7) + (cell >> 3)) & 1;
        lv_obj_set_style_text_color(chess_coord[i], lv_color_hex(dark ? chess_ink_dark : chess_ink_light), 0);
    }
}

static void chess_redraw(void)
{
    chess_dirty = ~0ull;
    chess_flush();
}

/* ------------------------------------------------------- the position */
static struct sl_chess_game chess_scratch;   /* for replaying the record */

static void chess_show(int view)
{
    if (view < 0) view = 0;
    if (view > chess.plies) view = chess.plies;
    chess_view = view;
    if (view == chess.plies) { chess_shown = chess.pos; return; }
    chess_scratch = chess;
    while (chess_scratch.plies > view) sl_chess_takeback(&chess_scratch);
    chess_shown = chess_scratch.pos;
}

static void chess_san_rebuild(void)
{
    chess_scratch = chess;
    while (chess_scratch.plies) sl_chess_takeback(&chess_scratch);
    for (int i = 0; i < chess.plies; i++)
    {
        sl_chess_san(&chess_scratch.pos, &chess.moves[i], chess_san[i]);
        sl_chess_make(&chess_scratch.pos, &chess.moves[i], &chess_scratch.undo[i]);
    }
}

static bool chess_live(void) { return chess_view == chess.plies; }

static bool chess_save(void)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    chess.checksum = sl_chess_game_checksum(&chess);
    int fd = open(CHESS_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool okay = write(fd, &chess, sizeof chess) == (ssize_t)sizeof chess;
    if (okay) okay = fsync(fd) == 0;   /* a power cut must not leave an empty save */
    if (close(fd) < 0) okay = false;
    if (!okay || rename(CHESS_TEMP, CHESS_FILE) < 0) { remove(CHESS_TEMP); return false; }
    return true;
}

static void chess_load(void)
{
    if (chess_loaded) return;
    chess_loaded = true;
    int fd = open(CHESS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        ssize_t got = read(fd, &chess, sizeof chess);
        close(fd);
        if (got == (ssize_t)sizeof chess && (sl_chess_game_valid(&chess) || sl_chess_game_upgrade(&chess)))
        {
            chess_san_rebuild();
            return;
        }
        sl_log("chess: ignoring unreadable save");
    }
    sl_chess_new(&chess, 1, 2, 1);
    chess.plies = 0;
}

static bool chess_in_progress(void)
{
    return sl_chess_game_valid(&chess) && chess.plies && sl_chess_game_status(&chess) == CH_PLAYING;
}

/* ----------------------------------------------------- bars and strip */
static lv_obj_t *chess_bar[2], *chess_bar_avatar[2], *chess_bar_name[2], *chess_bar_sub[2];
static lv_obj_t *chess_bar_cap[2], *chess_bar_adv[2], *chess_bar_pill[2], *chess_bar_pill_text[2];
static uint16_t chess_cap_buf[2][200 * CHESS_SMALL] __attribute__((aligned(4)));
static lv_obj_t *chess_strip, *chess_strip_hint, *chess_strip_lbl[CH_MAX_PLIES], *chess_strip_num[CH_MAX_PLIES / 2 + 1];
static int chess_strip_n, chess_strip_end[CH_MAX_PLIES + 1];
static lv_obj_t *chess_btn[5];
enum { CHESS_BTN_NEW, CHESS_BTN_UNDO, CHESS_BTN_FLIP, CHESS_BTN_BACK, CHESS_BTN_FWD };
static int chess_dots;

static int chess_bar_side(int bar) { int bottom = chess_flipped ? -1 : 1; return bar ? -bottom : bottom; }

static const char *chess_side_label(int side)
{
    if (chess.mode) return side == chess.human ? "You" : "Computer";
    return side > 0 ? "White" : "Black";
}

static int chess_material(const struct sl_chess_pos *p, int side)
{
    int m = 0;
    for (int sq = 0; sq < 128; sq++)
    {
        if (!CH_ONBOARD(sq)) { sq += 7; continue; }
        int8_t pc = (int8_t)(p->board[sq] * side);
        if (pc > 0 && pc < CH_K) m += ch_value[pc];
    }
    return m;
}

/* The pieces this side has taken, small and overlapping by kind. */
static void chess_draw_captures(int bar)
{
    int side = chess_bar_side(bar);
    uint16_t *buf = chess_cap_buf[bar];
    uint16_t bg = chess_565(C_CARD);
    for (int i = 0; i < 200 * CHESS_SMALL; i++) buf[i] = bg;
    int count[7] = { 0 };
    for (int sq = 0; sq < 128; sq++)
    {
        if (!CH_ONBOARD(sq)) { sq += 7; continue; }
        int8_t pc = (int8_t)(chess_shown.board[sq] * -side);
        if (pc > 0) count[pc]++;
    }
    static const int start[7] = { 0, 8, 2, 2, 2, 1, 1 };
    int x = 0;
    for (int t = CH_P; t <= CH_Q; t++)
    {
        int gone = start[t] - count[t];
        for (int k = 0; k < gone && x + CHESS_SMALL <= 200; k++)
        {
            const uint8_t *spr = chess_small[chess_spr_index((int8_t)(-side * t))];
            const uint16_t *col = (const uint16_t *)spr;
            const uint8_t *al = spr + CHESS_SMALL * CHESS_SMALL * 2;
            /* ebony on a dark card: a faint rim so it does not sink in */
            if (side > 0 && sl_theme() != 1)
                for (int yy = 1; yy < CHESS_SMALL - 1; yy++)
                    for (int xx = 1; xx < CHESS_SMALL - 1; xx++)
                    {
                        unsigned a = al[(yy - 1) * CHESS_SMALL + xx] | al[(yy + 1) * CHESS_SMALL + xx] |
                                     al[yy * CHESS_SMALL + xx - 1] | al[yy * CHESS_SMALL + xx + 1];
                        if (a > 120 && al[yy * CHESS_SMALL + xx] < 200)
                            buf[yy * 200 + x + xx] = chess_blend(chess_565(0x9aa6b4), buf[yy * 200 + x + xx], 150);
                    }
            for (int yy = 0; yy < CHESS_SMALL; yy++)
                for (int xx = 0; xx < CHESS_SMALL; xx++)
                {
                    unsigned a = al[yy * CHESS_SMALL + xx];
                    if (a) buf[yy * 200 + x + xx] = chess_blend(col[yy * CHESS_SMALL + xx], buf[yy * 200 + x + xx], a);
                }
            x += 10;
        }
        if (gone > 0) x += 14;
    }
    lv_obj_invalidate(chess_bar_cap[bar]);
    lv_obj_set_x(chess_bar_adv[bar], 60 + (x ? x : 0) + (x ? 4 : 0));
    int adv = chess_material(&chess_shown, side) - chess_material(&chess_shown, -side);
    char s[16] = "";
    if (adv >= 100) snprintf(s, sizeof s, "+%d", adv / 100);
    lv_label_set_text(chess_bar_adv[bar], s);
}

static void chess_pill(int bar, const char *s, uint32_t bg, uint32_t ink)
{
    if (!s) { lv_obj_add_flag(chess_bar_pill[bar], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(chess_bar_pill[bar], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(chess_bar_pill[bar], lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(chess_bar_pill_text[bar], lv_color_hex(ink), 0);
    label_if_changed(chess_bar_pill_text[bar], sl_tr(s));
}

static void chess_bars_update(void)
{
    if (!chess_bar[0]) return;
    enum sl_chess_status st = sl_chess_game_status(&chess);
    for (int bar = 0; bar < 2; bar++)
    {
        int side = chess_bar_side(bar);
        lv_image_set_src(chess_bar_avatar[bar], &chess_mid_dsc[side < 0]);
        const char *name = sl_tr(chess_side_label(side));
        lv_label_set_text(chess_bar_name[bar], name);
        lv_point_t nsz;
        lv_text_get_size(&nsz, name, &CH_S18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_x(chess_bar_sub[bar], 60 + nsz.x + 8);
        lv_label_set_text(chess_bar_sub[bar], chess.mode ? sl_tr(side == chess.human ? (side > 0 ? "White" : "Black")
                                                                                   : chess_level_names[chess.level]) : "");
        chess_draw_captures(bar);
        bool to_move = chess.pos.side == side;
        bool check = to_move && sl_chess_in_check(&chess.pos, side);
        lv_obj_set_style_border_opa(chess_bar[bar], to_move && st == CH_PLAYING ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (st == CH_CHECKMATE) chess_pill(bar, to_move ? NULL : "Winner", CHESS_BRASS, 0x2a1c05);
        else if (st != CH_PLAYING) chess_pill(bar, "Draw", C_CARD_P, C_TEXT);
        else if (!to_move) chess_pill(bar, NULL, 0, 0);
        else if (chess.mode && side != chess.human)
        {
            char think[48];   /* the word in the language, then its dots */
            snprintf(think, sizeof think, "%s%.*s", sl_tr("Thinking"), chess_dots & 3, "...");
            chess_pill(bar, chess_thinking ? think : "To move", C_CARD_P, C_TEXT);
        }
        else if (check) chess_pill(bar, "Check", 0xff3b30, 0xffffff);
        else chess_pill(bar, chess.mode ? "Your move" : "To move", sl_accent(), sl_accent_ink());
    }
}

static void chess_strip_cb(lv_event_t *e);

static void chess_strip_mark(void)
{
    for (int i = 0; i < chess_strip_n; i++)
    {
        bool on = i == chess_view - 1;
        lv_obj_set_style_bg_opa(chess_strip_lbl[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(chess_strip_lbl[i], lv_color_hex(on ? sl_accent_ink() : C_TEXT), 0);
    }
    if (chess_strip_hint)
    {
        if (chess.plies) lv_obj_add_flag(chess_strip_hint, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(chess_strip_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (chess_view > 0 && chess_view <= chess_strip_n)
        lv_obj_scroll_to_view(chess_strip_lbl[chess_view - 1], LV_ANIM_ON);
}

/* The move list: append what is new, drop what was taken back. */
static void chess_strip_sync(void)
{
    if (!chess_strip) return;
    while (chess_strip_n > chess.plies)
    {
        chess_strip_n--;
        lv_obj_delete(chess_strip_lbl[chess_strip_n]);
        if (!(chess_strip_n & 1)) lv_obj_delete(chess_strip_num[chess_strip_n / 2]);
    }
    while (chess_strip_n < chess.plies)
    {
        int i = chess_strip_n, x = chess_strip_end[i] + (i ? 4 : 10);
        lv_point_t sz;
        if (!(i & 1))
        {
            char num[8];
            snprintf(num, sizeof num, "%d.", i / 2 + 1);
            lv_obj_t *n = text(chess_strip, "", &CH_R16, C_DIM);
            lv_label_set_text(n, num);
            lv_text_get_size(&sz, num, &CH_R16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_obj_set_pos(n, x + (i ? 8 : 0), 8);
            chess_strip_num[i / 2] = n;
            x += sz.x + (i ? 12 : 4);
        }
        lv_obj_t *l = text(chess_strip, "", &CH_S18, C_TEXT);
        lv_label_set_text(l, chess_san[i]);
        lv_obj_set_style_pad_hor(l, 8, 0);
        lv_obj_set_style_pad_ver(l, 4, 0);
        lv_obj_set_style_radius(l, 12, 0);
        lv_obj_set_style_bg_color(l, lv_color_hex(sl_accent()), 0);
        lv_obj_set_pos(l, x, 3);
        on_click(l, chess_strip_cb, (void *)(intptr_t)(i + 1));
        lv_text_get_size(&sz, chess_san[i], &CH_S18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        chess_strip_lbl[i] = l;
        chess_strip_end[i + 1] = x + sz.x + 16;
        chess_strip_n++;
    }
    chess_strip_mark();
}

static void chess_buttons_update(void)
{
    if (!chess_btn[0]) return;
    bool undo = chess.plies > 0 && !(chess.mode && chess.plies == 1 && chess.human < 0 && !chess_thinking);
    bool can[5] = { true, undo, true, chess_view > 0, chess_view < chess.plies };
    for (int i = 0; i < 5; i++)
        if (can[i]) lv_obj_remove_state(chess_btn[i], LV_STATE_DISABLED);
        else lv_obj_add_state(chess_btn[i], LV_STATE_DISABLED);
}

static void chess_ui_update(void)
{
    chess_bars_update();
    chess_strip_sync();
    chess_buttons_update();
}

/* --------------------------------------------------------- the opponent */
/* The computer thinks on a thread of its own, on a copy of the board: on
 * the UI thread a Hard move froze the whole player for up to 1.3 s on the
 * R1 (the watchdog logged each one), taps and music keys included. The
 * search touches nothing but its copy, its table and the clock; the page
 * polls for the answer and plays it only if the board is still the one
 * it asked about. A search the page no longer wants (undo, left, new
 * game) is told to stop and is dropped. */
static struct
{
    struct sl_chess_pos pos;             /* the worker's own copy */
    struct sl_chess_pos asked;           /* the board the answer is for */
    struct sl_chess_move best;
    uint32_t history[CH_MAX_PLIES + 1];
    int nhistory, level, depth;
    uint32_t seed, budget, started;
    bool found, busy;
    volatile bool done, abort;
} chess_job;
static uint32_t chess_think_at, chess_reply_at;

static void *chess_worker(void *arg)
{
    (void)arg;
    struct sl_chess_ask a = { .level = chess_job.level, .seed = chess_job.seed, .now_ms = sl_ms,
                              .budget_ms = chess_job.budget, .history = chess_job.history,
                              .nhistory = chess_job.nhistory, .abort = &chess_job.abort };
    chess_job.found = sl_chess_think_ask(&chess_job.pos, &a, &chess_job.best);
    chess_job.depth = a.depth;
    __atomic_store_n(&chess_job.done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void chess_think_begin(void)
{
    if (chess_job.busy) return;          /* an old search still stopping */
    chess_job.pos = chess_job.asked = chess.pos;
    int from = chess.plies - chess.pos.halfmove;
    if (from < 0) from = 0;
    chess_job.nhistory = chess.plies - from;
    memcpy(chess_job.history, &chess.hashes[from], sizeof chess_job.history[0] * (size_t)chess_job.nhistory);
    chess_job.level = chess.level;
    chess_job.seed = sl_ms() ^ (uint32_t)chess.plies * 2654435761u;
    chess_job.budget = sl_chess_level_ms(chess.level);
    chess_job.started = sl_ms();
    chess_job.done = chess_job.abort = false;
    chess_job.busy = true;
    pthread_attr_t attr;
    pthread_t th;
    bool threaded = pthread_attr_init(&attr) == 0;
    if (threaded)
    {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 512 * 1024);   /* about 100 KiB at the deepest line */
        threaded = pthread_create(&th, &attr, chess_worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!threaded) chess_worker(NULL);   /* no thread: think here, as before */
}

static void chess_think_cancel(void)
{
    chess_thinking = false;
    if (chess_job.busy) chess_job.abort = true;
}

static void chess_after_move(bool animate, const struct sl_chess_pos *before, bool backward);
static void chess_over_maybe(void);

static void chess_think_end(void)
{
    chess_job.busy = false;
    bool wanted = chess_thinking && !memcmp(&chess_job.asked, &chess.pos, sizeof chess.pos);
    sl_log("chess: computer (%s) %s in %lu ms, depth %d", chess_level_names[chess_job.level],
           wanted ? "moved" : "answer dropped", (unsigned long)(sl_ms() - chess_job.started), chess_job.depth);
    if (!wanted) return;
    chess_thinking = false;
    if (!chess_job.found) return;
    struct sl_chess_pos before = chess.pos;
    sl_chess_san(&chess.pos, &chess_job.best, chess_san[chess.plies]);
    if (sl_chess_play(&chess, &chess_job.best)) chess_after_move(true, &before, false);
}

/* ---------------------------------------------------------- animation */
static uint32_t chess_settled_at;     /* when the last flight landed */

static void chess_fly_exec(void *var, int32_t v)
{
    (void)var;
    for (int i = 0; i < chess_nfly; i++)
    {
        int x, y; unsigned a;
        chess_fly_pos(&chess_fly[i], &x, &y, &a);
        chess_dirty_rect(x, y, CHESS_SQ, CHESS_SQ);   /* where it was */
    }
    chess_fly_t = v;
    chess_flush();
}

static void chess_fly_done(lv_anim_t *a)
{
    (void)a;
    for (int i = 0; i < chess_nfly; i++)
    {
        int x, y; unsigned al;
        chess_fly_pos(&chess_fly[i], &x, &y, &al);
        chess_dirty_rect(x, y, CHESS_SQ, CHESS_SQ);
    }
    chess_fly_on = false;
    chess_nfly = 0;
    chess_dirty |= chess_hidden;
    chess_hidden = 0;
    chess_flush();
    chess_settled_at = lv_tick_get();
}

static void chess_fly_start(int ms)
{
    lv_anim_delete(&chess_fly_t, NULL);
    chess_fly_t = 0;
    chess_fly_on = true;
    chess_fly_ms = ms;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &chess_fly_t);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_exec_cb(&a, chess_fly_exec);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, chess_fly_done);
    lv_anim_start(&a);
    chess_flush();
}

static void chess_fly_stop(void)
{
    if (!chess_fly_on) return;
    lv_anim_delete(&chess_fly_t, NULL);
    chess_fly_done(NULL);
}

static void chess_cell_xy(int sq, int *x, int *y)
{
    int cell = chess_cell_of(sq);
    *x = (cell & 7) * CHESS_SQ; *y = (cell >> 3) * CHESS_SQ;
}

static void chess_fly_add(int8_t piece, int from_sq, int to_sq, unsigned a0, unsigned a1)
{
    if (!piece || chess_nfly >= 3) return;
    struct chess_flyer *f = &chess_fly[chess_nfly++];
    int x0, y0, x1, y1;
    chess_cell_xy(from_sq, &x0, &y0);
    chess_cell_xy(to_sq, &x1, &y1);
    *f = (struct chess_flyer){ piece, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (uint8_t)a0, (uint8_t)a1 };
    chess_hidden |= 1ull << chess_cell_of(to_sq);
    chess_dirty_sq(from_sq);
    chess_dirty_sq(to_sq);
}

/* Glide m between before (the position it was played from) and the
 * board now shown: forward plays it, backward takes it back. */
static void chess_animate(const struct sl_chess_pos *before, const struct sl_chess_move *m, bool backward)
{
    chess_fly_stop();
    chess_nfly = 0;
    chess_hidden = 0;
    int rank = CH_RANK(m->from), cap = m->flags & CHF_EP ? m->to + (before->side > 0 ? -16 : 16) : m->to;
    int8_t moved = before->board[m->from], taken = (m->flags & CHF_CAPTURE) ? before->board[cap] : 0;
    int rook_from = -1, rook_to = -1;
    if (m->flags & CHF_CASTLE)
    {
        rook_from = CH_FILE(m->to) == 6 ? CH_SQ(7, rank) : CH_SQ(0, rank);
        rook_to = CH_FILE(m->to) == 6 ? CH_SQ(5, rank) : CH_SQ(3, rank);
    }
    if (!backward)
    {
        if (taken) chess_fly_add(taken, cap, cap, 255, 0);
        chess_fly_add(moved, m->from, m->to, 255, 255);
        if (rook_from >= 0) chess_fly_add(before->board[rook_from], rook_from, rook_to, 255, 255);
    }
    else
    {
        if (taken) chess_fly_add(taken, cap, cap, 0, 255);
        chess_fly_add(moved, m->to, m->from, 255, 255);
        if (rook_from >= 0) chess_fly_add(before->board[rook_from], rook_to, rook_from, 255, 255);
    }
    int df = abs(CH_FILE(m->to) - CH_FILE(m->from)), dr = abs(CH_RANK(m->to) - CH_RANK(m->from));
    int dist = df > dr ? df : dr;
    chess_fly_start(150 + 14 * dist);
}

/* ------------------------------------------------------------ moving */
static int chess_pieces;
static bool chess_promoted;

static int chess_piece_count(void)
{
    int n = 0;
    for (int sq = 0; sq < 128; sq++)
        if (CH_ONBOARD(sq) && chess.pos.board[sq]) n++;
    return n;
}

static void chess_move_sound(void)
{
    int pieces = chess_piece_count();
    bool capture = pieces < chess_pieces;
    chess_pieces = pieces;
    enum sl_chess_status st = sl_chess_game_status(&chess);
    if (st == CH_CHECKMATE)
        sfx_play(chess.mode && chess.pos.side == chess.human ? SFX_LOSE : SFX_WIN);
    else if (st != CH_PLAYING) sfx_play(SFX_DRAW);
    else if (sl_chess_in_check(&chess.pos, chess.pos.side)) sfx_play(SFX_CHESS_CHECK);
    else if (chess_promoted) sfx_play(SFX_CHESS_PROMOTE);
    else sfx_play_vary(capture ? SFX_CHESS_CAPTURE : SFX_CHESS_MOVE);
    chess_promoted = false;
}

static bool chess_human_turn(void)
{
    return !chess.mode || chess.pos.side == chess.human;
}

static void chess_deselect(void)
{
    if (chess_selected >= 0) chess_dirty_sq(chess_selected);
    for (int i = 0; i < chess_ntargets; i++) chess_dirty_sq(chess_targets[i].to);
    chess_selected = -1;
    chess_ntargets = 0;
}

static uint32_t chess_over_at;
static bool chess_over_seen;
static void chess_over_hide(void);

/* After any move lands in the record: show it, save it, and hand the
 * board to whoever is next. */
static void chess_after_move(bool animate, const struct sl_chess_pos *before, bool backward)
{
    chess_deselect();
    struct sl_chess_move m = chess.moves[chess.plies - (backward ? 0 : 1)];
    chess_show(chess.plies);
    if (animate) chess_animate(before, &m, backward);
    else
    {
        chess_redraw();
        chess_settled_at = lv_tick_get();
    }
    if (!backward) chess_move_sound();
    else chess_pieces = chess_piece_count();
    if (!chess_save()) sl_log("chess: save failed");
    enum sl_chess_status st = sl_chess_game_status(&chess);
    if (chess.mode && chess.pos.side != chess.human && st == CH_PLAYING)
    {
        chess_thinking = true;
        chess_think_at = lv_tick_get() + 250;   /* let the move land first */
        chess_reply_at = lv_tick_get() + 650;   /* an instant answer feels like a machine */
    }
    if (st != CH_PLAYING) chess_over_at = lv_tick_get() + 650;
    chess_ui_update();
}

static bool chess_play_move(const struct sl_chess_move *m, bool animate)
{
    struct sl_chess_pos before = chess.pos;
    char san[8];
    sl_chess_san(&chess.pos, m, san);
    if (!sl_chess_play(&chess, m)) return false;
    memcpy(chess_san[chess.plies - 1], san, sizeof san);
    chess_after_move(animate, &before, false);
    return true;
}

static void chess_select(int sq)
{
    chess_deselect();
    int8_t pc = chess.pos.board[sq];
    if (!pc || (pc > 0) != (chess.pos.side > 0)) return;
    struct sl_chess_move all[CH_MAX_MOVES];
    int n = sl_chess_legal(&chess.pos, all, false);
    for (int i = 0; i < n && chess_ntargets < 32; i++)
        if (all[i].from == sq && (!all[i].promo || all[i].promo == CH_Q))
            chess_targets[chess_ntargets++] = all[i];
    if (!chess_ntargets) return;
    chess_selected = sq;
    chess_dirty_sq(sq);
    for (int i = 0; i < chess_ntargets; i++) chess_dirty_sq(chess_targets[i].to);
    sfx_play(SFX_CHESS_SELECT);
}

static void chess_promo_show(const struct sl_chess_move *m)
{
    chess_pending = *m;
    chess_promo_open = true;
    chess_deselect();
    chess_redraw();
}

static void chess_promo_close(void)
{
    if (!chess_promo_open) return;
    chess_promo_open = false;
    chess_redraw();
}

static void chess_promote(int type)
{
    if (!chess_promo_open) return;
    struct sl_chess_move m = chess_pending;
    chess_promo_open = false;
    m.promo = (int8_t)type;
    chess_promoted = true;
    if (!chess_play_move(&m, true)) chess_redraw();
}

static bool chess_input_ok(void)
{
    return chess_canvas && !chess_thinking && chess_human_turn() && chess_live() &&
           sl_chess_game_status(&chess) == CH_PLAYING;
}

/* Try the selected piece to sq. Returns true when that was a move. */
static bool chess_try(int sq, bool animate)
{
    const struct sl_chess_move *t;
    if (chess_selected < 0 || !chess_target(sq, &t)) return false;
    struct sl_chess_move m = *t;
    if (m.promo) { chess_promo_show(&m); return true; }
    chess_play_move(&m, animate);
    return true;
}

/* A tap on a square, as the board's press and release make it. */
static void chess_tap_square(int sq)
{
    if (!CH_ONBOARD(sq) || !chess_canvas) return;
    if (chess_promo_open)
    {
        int slot = chess_promo_slot(sq);
        if (slot >= 0) chess_promote(chess_promo_types[slot]);
        else chess_promo_close();
        return;
    }
    if (!chess_live())
    {
        chess_show(chess.plies);   /* back to the game */
        chess_redraw();
        chess_ui_update();
        return;
    }
    if (!chess_input_ok()) return;
    if (chess_try(sq, true)) return;
    int was = chess_selected;
    chess_deselect();
    if (sq != was) chess_select(sq);
    chess_flush();
}

/* -------------------------------------------------- touch on the board */
static int chess_press_sq = -1, chess_press_x, chess_press_y;
static bool chess_press_was_selected, chess_press_drag_ok;

static bool chess_board_point(int *x, int *y)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !chess_canvas) return false;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t a;
    lv_obj_get_coords(chess_canvas, &a);
    *x = pt.x - a.x1; *y = pt.y - a.y1;
    return true;
}

static int chess_point_sq(int x, int y)
{
    if (x < 0 || y < 0 || x >= CHESS_PX || y >= CHESS_PX) return -1;
    return chess_sq_of(y / CHESS_SQ * 8 + x / CHESS_SQ);
}

static void chess_drag_to(int x, int y)
{
    chess_dirty_rect(chess_drag_x, chess_drag_y, CHESS_SQ, CHESS_SQ);
    chess_drag_x = x - CHESS_SQ / 2;
    chess_drag_y = y - CHESS_SQ / 2 - CHESS_DRAG_LIFT;
    int hover = chess_point_sq(x, y - CHESS_DRAG_LIFT);
    if (hover >= 0 && !chess_target(hover, NULL)) hover = -1;
    if (hover != chess_hover) { chess_dirty_sq(chess_hover); chess_dirty_sq(hover); chess_hover = hover; }
    chess_flush();
}

static void chess_board_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int x, y;
    if (!chess_board_point(&x, &y)) return;
    if (code == LV_EVENT_PRESSED)
    {
        chess_press_sq = chess_point_sq(x, y);
        chess_press_x = x; chess_press_y = y;
        chess_press_was_selected = chess_press_sq >= 0 && chess_press_sq == chess_selected;
        chess_press_drag_ok = false;
        if (chess_press_sq < 0 || chess_promo_open || !chess_input_ok()) return;
        int8_t pc = chess.pos.board[chess_press_sq];
        if (pc && (pc > 0) == (chess.pos.side > 0))
        {
            /* a press on your own piece shows where it can go at once */
            chess_fly_stop();
            if (!chess_press_was_selected) chess_select(chess_press_sq);
            chess_press_drag_ok = chess_selected == chess_press_sq;
            chess_flush();
        }
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!chess_press_drag_ok) return;
        if (!chess_dragging && (abs(x - chess_press_x) > 8 || abs(y - chess_press_y) > 8))
        {
            chess_dragging = true;
            chess_drag.piece = chess.pos.board[chess_press_sq];
            chess_drag.from = chess_press_sq;
            chess_drag_x = x - CHESS_SQ / 2;
            chess_drag_y = y - CHESS_SQ / 2 - CHESS_DRAG_LIFT;
            chess_dirty_sq(chess_press_sq);
        }
        if (chess_dragging) chess_drag_to(x, y);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (chess_dragging)
        {
            int drop = code == LV_EVENT_RELEASED ? chess_point_sq(x, y - CHESS_DRAG_LIFT) : -1;
            int px = chess_drag_x, py = chess_drag_y;
            chess_dragging = false;
            chess_dirty_rect(px, py, CHESS_SQ, CHESS_SQ);
            chess_dirty_sq(chess_hover);
            chess_hover = -1;
            if (drop >= 0 && drop != chess_drag.from && chess_try(drop, false)) return;
            /* not a move: the piece glides home */
            chess_dirty_sq(chess_drag.from);
            chess_nfly = 1;
            int hx, hy;
            chess_cell_xy(chess_drag.from, &hx, &hy);
            chess_fly[0] = (struct chess_flyer){ chess_drag.piece, (int16_t)px, (int16_t)py, (int16_t)hx, (int16_t)hy, 255, 255 };
            chess_hidden = 1ull << chess_cell_of(chess_drag.from);
            chess_fly_start(140);
            return;
        }
        if (code != LV_EVENT_RELEASED || chess_press_sq < 0) return;
        int sq = chess_press_sq;
        chess_press_sq = -1;
        if (chess_promo_open || !chess_live() || !chess_input_ok()) { chess_tap_square(sq); return; }
        if (chess_try(sq, true)) return;
        int8_t pc = chess.pos.board[sq];
        bool own = pc && (pc > 0) == (chess.pos.side > 0);
        if (!own || chess_press_was_selected) chess_deselect();   /* a second tap puts it down */
        chess_flush();
    }
}

/* ----------------------------------------------------------- toolbar */
static void chess_sheet_open(void);

static void chess_view_step(int to)
{
    if (to < 0 || to > chess.plies || to == chess_view) return;
    chess_deselect();
    chess_promo_open = false;
    int from = chess_view;
    if (abs(to - from) == 1)
    {
        struct sl_chess_pos before;
        if (to > from) { before = chess_shown; chess_show(to); chess_animate(&before, &chess.moves[from], false); }
        else { chess_show(to); before = chess_shown; chess_animate(&before, &chess.moves[to], true); }
    }
    else { chess_show(to); chess_redraw(); }
    chess_ui_update();
}

static void chess_strip_cb(lv_event_t *e) { chess_view_step((int)(intptr_t)lv_event_get_user_data(e)); }

static void chess_undo(void)
{
    if (!chess.plies) return;
    chess_fly_stop();
    bool was_thinking = chess_thinking;
    chess_think_cancel();
    chess_promo_open = false;
    chess_dragging = false;
    if (!chess_live()) chess_show(chess.plies);
    sl_chess_takeback(&chess);
    /* Against the computer, take back its reply too so it is your turn. */
    if (chess.mode && !was_thinking && chess.plies && chess.pos.side != chess.human) sl_chess_takeback(&chess);
    struct sl_chess_pos before = chess.pos;   /* the board the last undone move was played from */
    sfx_play(SFX_CHESS_UNDO);
    chess_over_at = 0;
    chess_over_seen = false;
    chess_over_hide();
    chess_after_move(true, &before, true);
}

static void chess_flip(void)
{
    chess_fly_stop();
    chess_flipped = !chess_flipped;
    if (!chess.mode) { chess.flip = 0; chess_save(); }
    chess_coords_update();
    chess_redraw();
    chess_bars_update();
}

static void chess_tool_cb(lv_event_t *e)
{
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case CHESS_BTN_NEW: chess_sheet_open(); break;
    case CHESS_BTN_UNDO: chess_undo(); break;
    case CHESS_BTN_FLIP: chess_flip(); break;
    case CHESS_BTN_BACK: chess_view_step(chess_view - 1); break;
    case CHESS_BTN_FWD: chess_view_step(chess_view + 1); break;
    }
}

/* ------------------------------------------------------- game over */
static lv_obj_t *chess_over, *chess_over_scrim, *chess_over_title, *chess_over_sub, *chess_over_king[2];

static void chess_over_hide(void)
{
    if (chess_over) lv_obj_add_flag(chess_over, LV_OBJ_FLAG_HIDDEN);
    if (chess_over_scrim) lv_obj_add_flag(chess_over_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void chess_over_review_cb(lv_event_t *e) { (void)e; chess_over_hide(); }

static void chess_start(int mode, int level, int human);
static void chess_rematch_cb(lv_event_t *e)
{
    (void)e;
    chess_over_hide();
    chess_start(chess.mode, chess.level, chess.mode ? -chess.human : 1);
}

static void chess_over_grow(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)(v * 255 / 256), 0);
    lv_obj_set_style_transform_scale(o, 200 + v * 56 / 256, 0);
}

static void chess_over_show(void)
{
    enum sl_chess_status st = sl_chess_game_status(&chess);
    if (st == CH_PLAYING || !chess_over) return;
    chess_over_seen = true;
    static const char *const reasons[] = { "", "", "No legal move, but no check: a draw", "Fifty moves without a capture",
                                           "Neither side can checkmate", "The same position three times",
                                           "The game ran out of moves" };
    int loser = chess.pos.side;
    const char *title, *sub;
    char who[48];
    if (st == CH_CHECKMATE)
    {
        title = "Checkmate";
        if (chess.mode) sub = loser == chess.human ? "The computer wins" : "You win!";
        else { snprintf(who, sizeof who, "%s %s", sl_tr(loser > 0 ? "Black" : "White"), sl_tr("wins")); sub = who; }
    }
    else { title = st == CH_STALEMATE ? "Stalemate" : "Draw"; sub = reasons[st]; }
    lv_label_set_text(chess_over_title, sl_tr(title));
    lv_label_set_text(chess_over_sub, sub == who ? who : sl_tr(sub));
    /* the winner stands, the loser's king lies on its side */
    for (int i = 0; i < 2; i++)
    {
        int side = i ? -1 : 1;
        bool down = st == CH_CHECKMATE && side == loser;
        lv_image_set_src(chess_over_king[i], &chess_big_dsc[i + (down ? 2 : 0)]);
        lv_obj_set_style_image_opa(chess_over_king[i], down ? LV_OPA_80 : LV_OPA_COVER, 0);
    }
    lv_obj_remove_flag(chess_over_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(chess_over, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(chess_over_scrim);
    lv_obj_move_foreground(chess_over);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, chess_over);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 280);
    lv_anim_set_exec_cb(&a, chess_over_grow);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void chess_over_maybe(void)
{
    if (!chess_over_at || (int32_t)(lv_tick_get() - chess_over_at) < 0 || chess_fly_on) return;
    chess_over_at = 0;
    if (!chess_over_seen) chess_over_show();
}

/* ------------------------------------------------------------- sheet */
static lv_obj_t *chess_sheet, *chess_sheet_scrim, *chess_sheet_close, *chess_seg_thumb, *chess_seg_lbl[2];
static lv_obj_t *chess_lvl_chip[CH_LEVELS], *chess_side_chip[3], *chess_sheet_blurb, *chess_sheet_cpu;
static int chess_setup_mode = 1, chess_setup_level = 2, chess_setup_side = 1;   /* side: 0 white 1 random 2 black */
static bool chess_setup_turn = true;      /* two players: the board turns each move */
static lv_obj_t *chess_sheet_friend, *chess_turn_knob, *chess_turn_track;
static bool chess_sheet_shown;
#define CHESS_SHEET_H 440

/* x from the leading edge of a parent pw wide: mirrored in Hebrew and Arabic */
static int chess_mx(int x, int w, int pw) { return sl_ui_rtl() ? pw - x - w : x; }
static void chess_sheet_refresh(void)
{
    if (!chess_sheet) return;
    lv_anim_delete(chess_seg_thumb, NULL);
    anim_x(chess_seg_thumb, lv_obj_get_x(chess_seg_thumb), chess_mx(chess_setup_mode ? 4 : 4 + 214, 214, W - 40), 200, NULL);
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_text_color(chess_seg_lbl[i], lv_color_hex(i == !chess_setup_mode ? C_TEXT : C_SUB), 0);
    for (int i = 0; i < CH_LEVELS; i++)
    {
        bool on = i == chess_setup_level;
        lv_obj_set_style_bg_color(chess_lvl_chip[i], lv_color_hex(on ? sl_accent() : C_CARD_P), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chess_lvl_chip[i], 0), lv_color_hex(on ? sl_accent_ink() : C_TEXT), 0);
    }
    for (int i = 0; i < 3; i++)
    {
        bool on = i == chess_setup_side;
        lv_obj_set_style_border_color(chess_side_chip[i], lv_color_hex(on ? sl_accent() : C_CARD_P), 0);
        lv_obj_set_style_bg_color(chess_side_chip[i], lv_color_hex(on ? C_CARD : C_CARD_P), 0);
    }
    if (chess_setup_mode) lv_obj_remove_flag(chess_sheet_cpu, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(chess_sheet_cpu, LV_OBJ_FLAG_HIDDEN);
    if (chess_setup_mode) lv_obj_add_flag(chess_sheet_friend, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(chess_sheet_friend, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(chess_turn_track, lv_color_hex(chess_setup_turn ? sl_accent() : C_DIM), 0);
    lv_anim_delete(chess_turn_knob, NULL);
    anim_x(chess_turn_knob, lv_obj_get_x(chess_turn_knob), chess_mx(chess_setup_turn ? 54 - 30 : 4, 24, 58), 160, NULL);
    lv_obj_set_y(chess_sheet_blurb, chess_setup_mode ? 216 : 228);
    lv_label_set_text(chess_sheet_blurb, chess_setup_mode ? sl_tr(chess_level_blurbs[chess_setup_level]) :
                      sl_tr("Pass the player between moves. The board turns to face whoever is next."));
    if (chess_in_progress()) lv_obj_remove_flag(chess_sheet_close, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(chess_sheet_close, LV_OBJ_FLAG_HIDDEN);
}

static void chess_sheet_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

static void chess_sheet_slide(bool in)
{
    lv_anim_delete(chess_sheet, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, chess_sheet);
    lv_anim_set_values(&a, lv_obj_get_y(chess_sheet), in ? H - CHESS_SHEET_H : H + 20);
    lv_anim_set_duration(&a, in ? SL_T_SLOW : SL_T_PAGE);
    lv_anim_set_exec_cb(&a, chess_sheet_y);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

static void chess_sheet_open(void)
{
    if (!chess_sheet) return;
    chess_dragging = false;
    if (chess.mode) { chess_setup_mode = 1; chess_setup_level = chess.level; }
    chess_sheet_shown = true;
    chess_sheet_refresh();
    lv_obj_remove_flag(chess_sheet_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(chess_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(chess_sheet_scrim);
    lv_obj_move_foreground(chess_sheet);
    chess_sheet_slide(true);
    sfx_play(SFX_UI_PANEL_OPEN);
}

static void chess_sheet_close_now(bool sound)
{
    if (!chess_sheet || !chess_sheet_shown) return;
    chess_sheet_shown = false;
    lv_obj_add_flag(chess_sheet_scrim, LV_OBJ_FLAG_HIDDEN);
    chess_sheet_slide(false);
    if (sound) sfx_play(SFX_UI_PANEL_CLOSE);
}

static void chess_sheet_dismiss_cb(lv_event_t *e)
{
    (void)e;
    if (chess_in_progress()) chess_sheet_close_now(true);
}

static void chess_mode_cb(lv_event_t *e) { chess_setup_mode = (int)(intptr_t)lv_event_get_user_data(e); sfx_play(SFX_TAP); chess_sheet_refresh(); }
static void chess_level_cb(lv_event_t *e) { chess_setup_level = (int)(intptr_t)lv_event_get_user_data(e); sfx_play(SFX_TAP); chess_sheet_refresh(); }
static void chess_turn_cb(lv_event_t *e) { (void)e; chess_setup_turn = !chess_setup_turn; sfx_play(chess_setup_turn ? SFX_UI_ON : SFX_UI_OFF); chess_sheet_refresh(); }
static void chess_side_cb(lv_event_t *e) { chess_setup_side = (int)(intptr_t)lv_event_get_user_data(e); sfx_play(SFX_TAP); chess_sheet_refresh(); }

static void chess_reset_view(void)
{
    chess_fly_stop();
    chess_think_cancel();
    chess_deselect();
    chess_promo_open = chess_dragging = false;
    chess_hover = -1;
    chess_over_at = 0;
    chess_over_seen = false;
    chess_over_hide();
    chess_flipped = chess.mode ? chess.human < 0 : (chess.flip && chess.pos.side < 0);
    chess_show(chess.plies);
    chess_pieces = chess_piece_count();
    chess_coords_update();
}

static void chess_start(int mode, int level, int human)
{
    sl_chess_new(&chess, mode, level, mode ? human : 1);
    chess_save();
    sl_log("chess: new %s game%s", mode ? chess_level_names[level] : "two-player", mode ? (human > 0 ? " as white" : " as black") : "");
    chess_strip_sync();
    chess_reset_view();
    chess_redraw();
    sfx_play(SFX_START);
    if (chess.mode && chess.pos.side != chess.human)
    {
        chess_thinking = true;
        chess_think_at = lv_tick_get() + 500;
        chess_reply_at = chess_think_at + 400;
    }
    chess_ui_update();
}

static void chess_go_cb(lv_event_t *e)
{
    (void)e;
    int human = chess_setup_side == 0 ? 1 : chess_setup_side == 2 ? -1 : ((sl_ms() >> 3) & 1 ? 1 : -1);
    chess_sheet_close_now(false);
    chess_start(chess_setup_mode, chess_setup_level, human);
    if (!chess.mode && !chess_setup_turn) { chess.flip = 0; chess_save(); }
}

/* ------------------------------------------------------------- the tick */
static lv_timer_t *chess_timer;
static uint32_t chess_dots_at;

static void chess_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!chess_page_obj) return;
    if (chess_job.busy && __atomic_load_n(&chess_job.done, __ATOMIC_ACQUIRE) &&
        (!chess_thinking || (int32_t)(lv_tick_get() - chess_reply_at) >= 0) && !chess_fly_on && !chess_dragging)
        chess_think_end();
    if (chess_thinking && !chess_job.busy && (int32_t)(lv_tick_get() - chess_think_at) >= 0) chess_think_begin();
    if (chess_thinking && lv_tick_get() - chess_dots_at > 380)
    {
        chess_dots_at = lv_tick_get();
        chess_dots++;
        chess_bars_update();
    }
    chess_over_maybe();
    /* two players: the board turns to whoever is next once the move has landed */
    if (!chess.mode && chess.flip && !chess_fly_on && !chess_dragging && chess_live() && !chess_promo_open &&
        sl_chess_game_status(&chess) == CH_PLAYING && chess_flipped != (chess.pos.side < 0) &&
        lv_tick_get() - chess_settled_at > 350)
    {
        chess_flipped = chess.pos.side < 0;
        chess_deselect();
        chess_coords_update();
        chess_redraw();
        chess_bars_update();
    }
}

/* -------------------------------------------------------------- page */
static bool chess_on_top(void)
{
    return chess_page_obj && npages && pages[npages - 1] == chess_page_obj;
}

static void chess_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != chess_page_obj) return;
    if (chess_timer) lv_timer_delete(chess_timer);
    chess_timer = NULL;
    lv_anim_delete(&chess_fly_t, NULL);
    chess_fly_on = false;
    chess_nfly = 0;
    chess_hidden = 0;
    chess_think_cancel();
    chess_promo_open = chess_dragging = false;
    chess_save();
    chess_page_obj = chess_canvas = chess_strip = chess_strip_hint = NULL;
    chess_over = chess_over_scrim = chess_over_title = chess_over_sub = chess_over_king[0] = chess_over_king[1] = NULL;
    chess_sheet = chess_sheet_scrim = chess_sheet_close = chess_seg_thumb = chess_sheet_blurb = chess_sheet_cpu = NULL;
    chess_sheet_friend = chess_turn_knob = chess_turn_track = NULL;
    chess_seg_lbl[0] = chess_seg_lbl[1] = NULL;
    for (int i = 0; i < CH_LEVELS; i++) chess_lvl_chip[i] = NULL;
    for (int i = 0; i < 3; i++) chess_side_chip[i] = NULL;
    for (int i = 0; i < 16; i++) chess_coord[i] = NULL;
    for (int i = 0; i < 5; i++) chess_btn[i] = NULL;
    for (int i = 0; i < 2; i++)
        chess_bar[i] = chess_bar_avatar[i] = chess_bar_name[i] = chess_bar_sub[i] = chess_bar_cap[i] =
            chess_bar_adv[i] = chess_bar_pill[i] = chess_bar_pill_text[i] = NULL;
    chess_strip_n = 0;
    chess_sheet_shown = false;
    sl_give_back(chess_buf, sizeof chess_buf);   /* 420 KB, redrawn on the next open */
    /* the pieces (~0.3 MB) are drawn again on the next open (~0.3 s), and
     * the computer's table of positions starts fresh, as at start-up; not
     * while a search it was asked to drop is still writing to it */
    sl_give_back(chess_spr, sizeof chess_spr);
    sl_give_back(chess_small, sizeof chess_small);
    sl_give_back(chess_mid, sizeof chess_mid);
    sl_give_back(chess_big, sizeof chess_big);
    chess_art_ready = false;
    if (!chess_job.busy) sl_give_back(ch_tt, sizeof ch_tt);
    mini_update();
}

static lv_obj_t *chess_card(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t colour)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, radius, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

static lv_obj_t *chess_button(lv_obj_t *parent, const char *label, int x, int y, int w, int h,
                              lv_event_cb_t cb, intptr_t data, bool primary)
{
    lv_obj_t *b = chess_card(parent, x, y, w, h, h / 2 < 18 ? h / 2 : 18, primary ? sl_accent() : C_CARD);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? sl_accent_on(C_CARD, 10) : C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = text(b, label, label[0] & 0x80 ? &sl_sym_22 : &CH_S18, primary ? sl_accent_ink() : C_TEXT);
    lv_obj_center(l);
    press_motion(b);
    on_click(b, cb, (void *)data);
    return b;
}

static void chess_build_bar(lv_obj_t *p, int bar, int y)
{
    lv_obj_t *c = chess_card(p, CHESS_BX, y, CHESS_PX, 58, SL_R_LG, C_CARD);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(sl_accent()), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_TRANSP, 0);
    chess_bar[bar] = c;
    lv_obj_t *av = chess_card(c, 6, 5, 44, 44, SL_R_FULL, C_CARD_P);
    chess_bar_avatar[bar] = lv_image_create(av);
    lv_obj_center(chess_bar_avatar[bar]);
    chess_bar_name[bar] = text(c, "", &CH_S18, C_TEXT);
    lv_obj_set_pos(chess_bar_name[bar], 60, 4);
    chess_bar_sub[bar] = text(c, "", &CH_R16, C_SUB);
    lv_obj_set_y(chess_bar_sub[bar], 6);
    chess_bar_cap[bar] = lv_canvas_create(c);
    lv_canvas_set_buffer(chess_bar_cap[bar], chess_cap_buf[bar], 200, CHESS_SMALL, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(chess_bar_cap[bar], 60, 30);
    lv_obj_remove_flag(chess_bar_cap[bar], LV_OBJ_FLAG_CLICKABLE);
    chess_bar_adv[bar] = text(c, "", &CH_S18, C_SUB);
    lv_obj_set_y(chess_bar_adv[bar], 29);
    chess_bar_pill[bar] = chess_card(c, CHESS_PX - 132, 13, 122, 32, SL_R_FULL, C_CARD_P);
    chess_bar_pill_text[bar] = text(chess_bar_pill[bar], "", &CH_S18, C_TEXT);
    lv_obj_set_style_text_align(chess_bar_pill_text[bar], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(chess_bar_pill_text[bar], 122);
    lv_obj_center(chess_bar_pill_text[bar]);
}

static void chess_build_sheet(lv_obj_t *p)
{
    chess_sheet_scrim = box(p);
    lv_obj_set_pos(chess_sheet_scrim, 0, CHESS_STRIP_Y);
    lv_obj_set_size(chess_sheet_scrim, W, H - CHESS_STRIP_Y);
    lv_obj_set_style_bg_color(chess_sheet_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chess_sheet_scrim, LV_OPA_40, 0);
    on_click(chess_sheet_scrim, chess_sheet_dismiss_cb, NULL);
    lv_obj_add_flag(chess_sheet_scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *s = chess_card(p, 0, H + 20, W, CHESS_SHEET_H + 30, SL_R_XL, C_CARD);
    chess_sheet = s;
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* taps stop here */
    sl_elevate(s, SL_E4, 0);
    lv_obj_t *grab = chess_card(s, (W - 44) / 2, 9, 44, 5, SL_R_FULL, C_DIM);
    (void)grab;
    lv_obj_t *t = text(s, "New game", &CH_S26, C_TEXT);
    sl_place(t, 20, 24);
    chess_sheet_close = chess_button(s, LV_SYMBOL_CLOSE, chess_mx(W - 20 - 44, 44, W), 20, 44, 44, chess_sheet_dismiss_cb, 0, false);
    lv_obj_set_style_bg_color(chess_sheet_close, lv_color_hex(C_CARD_P), 0);

    /* Computer | Two players, a sliding thumb */
    lv_obj_t *seg = chess_card(s, 20, 76, W - 40, 46, SL_R_FULL, C_CARD_P);
    chess_seg_thumb = chess_card(seg, chess_mx(4, 214, W - 40), 4, 214, 38, SL_R_FULL, C_CARD);
    lv_obj_set_style_shadow_width(chess_seg_thumb, 8, 0);
    lv_obj_set_style_shadow_opa(chess_seg_thumb, LV_OPA_30, 0);
    static const char *const modes[] = { "Computer", "Two players" };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *b = box(seg);
        lv_obj_set_size(b, 218, 46);
        lv_obj_set_pos(b, chess_mx(i * 218 + 2, 218, W - 40), 0);
        chess_seg_lbl[i] = text(b, modes[i], &CH_S18, C_TEXT);
        lv_obj_center(chess_seg_lbl[i]);
        on_click(b, chess_mode_cb, (void *)(intptr_t)!i);
    }

    chess_sheet_cpu = box(s);
    lv_obj_set_pos(chess_sheet_cpu, 20, 134);
    lv_obj_set_size(chess_sheet_cpu, W - 40, 216);
    lv_obj_t *h1 = text(chess_sheet_cpu, "Strength", &CH_R16, C_SUB);
    sl_place(h1, 2, 0);
    int cw = (W - 40 - 4 * 6) / CH_LEVELS;
    for (int i = 0; i < CH_LEVELS; i++)
    {
        lv_obj_t *c = chess_card(chess_sheet_cpu, chess_mx(i * (cw + 6), cw, W - 40), 24, cw, 50, SL_R_MD, C_CARD_P);
        lv_obj_t *l = text(c, chess_level_names[i], &CH_R16, C_TEXT);
        lv_obj_center(l);
        press_motion(c);
        on_click(c, chess_level_cb, (void *)(intptr_t)i);
        chess_lvl_chip[i] = c;
    }
    lv_obj_t *h2 = text(chess_sheet_cpu, "Play as", &CH_R16, C_SUB);
    sl_place(h2, 2, 116);
    static const char *const sides[] = { "White", "Random", "Black" };
    int sw = (W - 40 - 2 * 8) / 3;
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *c = chess_card(chess_sheet_cpu, chess_mx(i * (sw + 8), sw, W - 40), 140, sw, 64, 18, C_CARD_P);
        lv_obj_set_style_border_width(c, 2, 0);
        if (i == 1)
        {
            for (int k = 0; k < 2; k++)
            {
                lv_obj_t *im = lv_image_create(c);
                lv_image_set_src(im, &chess_mid_dsc[k]);
                sl_align(im, LV_ALIGN_LEFT_MID, 2 + k * 14, 0);
            }
        }
        else
        {
            lv_obj_t *im = lv_image_create(c);
            lv_image_set_src(im, &chess_mid_dsc[i == 2]);
            sl_align(im, LV_ALIGN_LEFT_MID, 8, 0);
        }
        lv_obj_t *l = text(c, sides[i], &CH_S18, C_TEXT);
        sl_align(l, LV_ALIGN_LEFT_MID, i == 1 ? 62 : 56, 0);
        press_motion(c);
        on_click(c, chess_side_cb, (void *)(intptr_t)i);
        chess_side_chip[i] = c;
    }
    /* two players: whether the board turns to face whoever is next */
    chess_sheet_friend = chess_card(s, 20, 140, W - 40, 72, 20, C_CARD_P);
    lv_obj_t *tl = text(chess_sheet_friend, "Turn the board each move", &CH_S18, C_TEXT);
    sl_align(tl, LV_ALIGN_LEFT_MID, 18, 0);
    chess_turn_track = chess_card(chess_sheet_friend, chess_mx(W - 40 - 18 - 58, 58, W - 40), 22, 58, 30, 15, sl_accent());
    chess_turn_knob = chess_card(chess_turn_track, chess_mx(54 - 30, 24, 58), 3, 24, 24, 12, 0xffffff);
    press_motion(chess_sheet_friend);
    on_click(chess_sheet_friend, chess_turn_cb, NULL);
    chess_sheet_blurb = text(s, "", &CH_R16, C_SUB);
    lv_obj_set_width(chess_sheet_blurb, W - 44);
    lv_label_set_long_mode(chess_sheet_blurb, LV_LABEL_LONG_WRAP);
    sl_place(chess_sheet_blurb, 22, 216);
    chess_button(s, "Start game", 20, CHESS_SHEET_H - 82, W - 40, 58, chess_go_cb, 0, true);
}

static void chess_build_over(lv_obj_t *p)
{
    chess_over_scrim = box(p);
    lv_obj_set_pos(chess_over_scrim, CHESS_BX, CHESS_BOARD_Y);
    lv_obj_set_size(chess_over_scrim, CHESS_PX, CHESS_PX);
    lv_obj_set_style_bg_color(chess_over_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chess_over_scrim, LV_OPA_40, 0);
    lv_obj_add_flag(chess_over_scrim, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    on_click(chess_over_scrim, chess_over_review_cb, NULL);

    int cw = 384, ch = 318;
    lv_obj_t *c = chess_card(p, (W - cw) / 2, CHESS_BOARD_Y + (CHESS_PX - ch) / 2, cw, ch, 28, C_CARD);
    chess_over = c;
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_set_style_shadow_width(c, 44, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_50, 0);
    lv_obj_set_style_transform_pivot_x(c, cw / 2, 0);
    lv_obj_set_style_transform_pivot_y(c, ch / 2, 0);
    /* the two kings stand on a thin line of the board's own colour */
    lv_obj_t *floor = chess_card(c, (cw - 230) / 2, 16 + CHESS_BIG - 9, 230, 3, 2, sl_accent());
    lv_obj_set_style_bg_opa(floor, LV_OPA_60, 0);
    for (int i = 0; i < 2; i++)
    {
        chess_over_king[i] = lv_image_create(c);
        lv_image_set_src(chess_over_king[i], &chess_big_dsc[i]);
        lv_obj_set_pos(chess_over_king[i], cw / 2 - CHESS_BIG - 2 + i * (CHESS_BIG + 4), 16);
    }
    chess_over_title = text(c, "", &ss_semibold_34, C_TEXT);
    lv_obj_set_width(chess_over_title, cw);
    lv_obj_set_style_text_align(chess_over_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(chess_over_title, 0, 132);
    chess_over_sub = text(c, "", &CH_R18, C_SUB);
    lv_obj_set_width(chess_over_sub, cw - 40);
    lv_obj_set_style_text_align(chess_over_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(chess_over_sub, 20, 180);
    int bw = (cw - 40 - 12) / 2;
    lv_obj_t *rv = chess_button(c, "Review", 20, ch - 82, bw, 58, chess_over_review_cb, 0, false);
    lv_obj_set_style_bg_color(rv, lv_color_hex(C_CARD_P), 0);
    chess_button(c, "Rematch", 20 + bw + 12, ch - 82, bw, 58, chess_rematch_cb, 0, true);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
}

static void chess_page(void)
{
    if (page_bring_back(chess_page_obj)) return;
    if (!sl_app_room(1100, "Chess")) return;
    chess_load();
    chess_art_init();
    chess_palette();
    lv_obj_t *p, *body = page_new("Chess", &p);
    chess_page_obj = p;
    lv_obj_add_event_cb(p, chess_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);

    /* the moves, in notation, along the top */
    chess_strip = box(p);
    lv_obj_set_pos(chess_strip, 0, CHESS_STRIP_Y);
    lv_obj_set_size(chess_strip, W, 36);
    lv_obj_add_flag(chess_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chess_strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chess_strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_right(chess_strip, 14, 0);
    chess_strip_hint = text(chess_strip, "Drag a piece, or tap it and tap where it goes.", &CH_R16, C_DIM);
    lv_obj_set_pos(chess_strip_hint, 16, 8);
    chess_strip_n = 0;
    chess_strip_end[0] = 0;

    chess_build_bar(p, 1, CHESS_TOPBAR_Y);

    chess_canvas = lv_canvas_create(p);
    lv_canvas_set_buffer(chess_canvas, chess_buf, CHESS_PX, CHESS_PX, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(chess_canvas, CHESS_BX, CHESS_BOARD_Y);
    lv_obj_add_flag(chess_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_PRESS_LOCK);   /* no back-swipe */
    lv_obj_remove_flag(chess_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(chess_canvas, chess_board_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(chess_canvas, chess_board_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(chess_canvas, chess_board_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(chess_canvas, chess_board_cb, LV_EVENT_PRESS_LOST, NULL);
    for (int i = 0; i < 16; i++)
    {
        chess_coord[i] = text(p, "", &sl_sym_14, C_TEXT);
        if (i < 8) lv_obj_set_pos(chess_coord[i], CHESS_BX + i * CHESS_SQ + CHESS_SQ - 13, CHESS_BOARD_Y + CHESS_PX - 18);
        else lv_obj_set_pos(chess_coord[i], CHESS_BX + 3, CHESS_BOARD_Y + (i - 8) * CHESS_SQ + 2);
    }

    chess_build_bar(p, 0, CHESS_BOTBAR_Y);

    static const char *const tools[] = { "New", "Undo", "Turn", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    int aw = 58, gap = 8, tw = (CHESS_PX - 2 * aw - 4 * gap) / 3;
    for (int i = 0, x = CHESS_BX; i < 5; i++)
    {
        int w = i < 3 ? tw : aw;
        chess_btn[i] = chess_button(p, tools[i], x, CHESS_TOOLS_Y, w, 50, chess_tool_cb, i, false);
        x += w + gap;
    }

    chess_build_over(p);
    chess_build_sheet(p);

    chess_reset_view();
    chess_redraw();
    chess_ui_update();
    if (chess.mode && chess.pos.side != chess.human && sl_chess_game_status(&chess) == CH_PLAYING)
    {
        chess_thinking = true;
        chess_think_at = lv_tick_get() + 400;
        chess_reply_at = chess_think_at;
    }
    if (chess_timer) lv_timer_delete(chess_timer);
    chess_timer = lv_timer_create(chess_tick, 30, NULL);
    page_push(p);
    if (!chess_in_progress()) chess_sheet_open();
    mini_update();
}
