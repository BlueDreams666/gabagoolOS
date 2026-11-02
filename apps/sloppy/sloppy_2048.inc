/* 2048: slide every tile one way; two of a kind join and add up. GPLv2+.
 *
 * Swipe anywhere under the header, or use the keys the other games steer
 * with (volume up and down, Play for left, Next for right). Tiles slide in
 * 110 ms, then the joined ones pop and the new one grows in; a swipe in the
 * middle of a slide finishes it first, so fast play never drops a move.
 * One step of undo. The game in progress and the best score are kept on
 * the card (2048.txt), so leaving never loses a board.
 *
 * The tiles climb our own ramp: quiet surfaces for 2 and 4, then the
 * accent's family from teal to gold for 2048, and black with gold beyond. */

#define T48_FILE  ROCKBOX_DIR "/sloppy/2048.txt"
#define T48_N     4
#define T48_CELL  96
#define T48_GAP   12
#define T48_BOARD (T48_N * T48_CELL + (T48_N + 1) * T48_GAP)   /* 444 */
#define T48_X     ((W - T48_BOARD) / 2)
#define T48_TOP   (SB_H + SL_HDR_H)
#define T48_Y     (T48_TOP + 96)
#define T48_SLIDE_MS 110

struct t48 { uint8_t v[16]; uint32_t score; bool won, kept; };   /* v: the power of two, 0 empty */
static struct t48 t48, t48_prev;
static bool t48_can_undo, t48_loaded, t48_over;
static uint32_t t48_best, t48_rng, t48_saved_at;
static lv_obj_t *t48_page_obj, *t48_board, *t48_tiles[16], *t48_moving[16], *t48_score_l, *t48_best_l, *t48_undo_b;
static lv_obj_t *t48_card, *t48_card_title, *t48_card_sub, *t48_card_go, *t48_card_keep;
static lv_timer_t *t48_slide_timer;
static int8_t t48_from[16];               /* where each tile of this move came from (-1: none) */
static int8_t t48_second[16];             /* and the one that joined it */
static bool t48_joined[16];
static int t48_born = -1;
static int t48_queued = -1;               /* a swipe that came during a slide */

static uint32_t t48_rand(void) { t48_rng = t48_rng * 1664525u + 1013904223u; return t48_rng >> 8; }
static void t48_spawn(struct t48 *g)
{
    int empty[16], n = 0;
    for (int i = 0; i < 16; i++) if (!g->v[i]) empty[n++] = i;
    if (!n) { t48_born = -1; return; }
    int at = empty[t48_rand() % n];
    g->v[at] = t48_rand() % 10 ? 1 : 2;   /* a 4 one time in ten */
    t48_born = at;
}
/* one move: 0 up, 1 right, 2 down, 3 left. Fills t48_from and t48_joined;
 * false when nothing would move. */
static bool t48_slide(struct t48 *g, int dir)
{
    bool moved = false;
    memset(t48_from, -1, sizeof t48_from);
    memset(t48_joined, 0, sizeof t48_joined);
    int8_t from2[16];
    memset(from2, -1, sizeof from2);
    for (int line = 0; line < T48_N; line++)
    {
        int idx[T48_N];
        for (int k = 0; k < T48_N; k++)        /* k = 0 is the end the tiles slide toward */
        {
            int r, c;
            if (dir == 0) { r = k; c = line; }
            else if (dir == 2) { r = T48_N - 1 - k; c = line; }
            else if (dir == 3) { r = line; c = k; }
            else { r = line; c = T48_N - 1 - k; }
            idx[k] = r * T48_N + c;
        }
        int out = 0;
        int last = -1;                          /* the place a tile could still join */
        uint8_t nv[T48_N] = { 0 };
        int8_t src[T48_N] = { -1, -1, -1, -1 }, src2[T48_N] = { -1, -1, -1, -1 };
        for (int k = 0; k < T48_N; k++)
        {
            uint8_t v = g->v[idx[k]];
            if (!v) continue;
            if (last >= 0 && nv[last] == v)
            {
                nv[last]++;
                src2[last] = (int8_t)idx[k];
                g->score += 1u << nv[last];
                if (nv[last] == 11 && !g->won) g->won = true;   /* 2048 */
                last = -1;
            }
            else
            {
                nv[out] = v;
                src[out] = (int8_t)idx[k];
                last = out++;
            }
        }
        for (int k = 0; k < T48_N; k++)
        {
            if (g->v[idx[k]] != nv[k]) moved = true;
            if (src[k] >= 0 && src[k] != idx[k]) moved = true;
            g->v[idx[k]] = nv[k];
            t48_from[idx[k]] = src[k];
            from2[idx[k]] = src2[k];
            t48_joined[idx[k]] = src2[k] >= 0;
        }
    }
    /* the second tile of each join slides too: noted where the first was */
    memcpy(t48_second, from2, sizeof from2);
    return moved;
}
static bool t48_stuck(const struct t48 *g)
{
    for (int i = 0; i < 16; i++)
    {
        if (!g->v[i]) return false;
        if (i % T48_N < T48_N - 1 && g->v[i] == g->v[i + 1]) return false;
        if (i < 12 && g->v[i] == g->v[i + T48_N]) return false;
    }
    return true;
}

/* ---- the card ---- */
static void t48_load(void)
{
    if (t48_loaded) return;
    t48_loaded = true;
    t48_rng = sl_ms() * 2654435761u + 1;
    int fd = open(T48_FILE, O_RDONLY);
    char b[160];
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got > 0)
    {
        b[got] = 0;
        unsigned best, score, won, kept, v[16];
        if (sscanf(b, "T48A %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", &best, &score, &won, &kept,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13],
                   &v[14], &v[15]) == 20)
        {
            t48_best = best;
            t48.score = score; t48.won = won; t48.kept = kept;
            bool any = false;
            for (int i = 0; i < 16; i++) { t48.v[i] = v[i] <= 17 ? (uint8_t)v[i] : 0; any |= t48.v[i] != 0; }
            if (any) return;
        }
    }
    memset(&t48, 0, sizeof t48);
    t48_spawn(&t48);
    t48_spawn(&t48);
}
static void t48_save(void)
{
    char b[160];
    int n = snprintf(b, sizeof b, "T48A %u %u %u %u", (unsigned)t48_best, (unsigned)t48.score, t48.won, t48.kept);
    for (int i = 0; i < 16; i++) n += snprintf(b + n, sizeof b - n, " %u", t48.v[i]);
    n += snprintf(b + n, sizeof b - n, "\n");
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(T48_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, n) == n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(T48_FILE ".tmp", T48_FILE) < 0) remove(T48_FILE ".tmp");
    t48_saved_at = sl_ms();
}

/* ---- looks ---- */
static uint32_t t48_tile_bg(int v)
{
    static const uint32_t ramp[] = { 0, 0, 0, 0x2fa59a, 0x2a8fbf, 0x3f6fd6, 0x6a55d6, 0x9b4cc4, 0xc8458f,
                                     0xd8434f, 0xc4511f, 0xf0b53c };   /* 4.5:1 for white words from 512 up */
    if (sl_zune())
    {
        static const uint32_t pink[] = { 0, 0x262626, 0x3a3a3a, 0x5a1a38, 0x7a1d48, 0x9a2058, 0xba2468, 0xd82672,
                                         0xf2287a, 0xff4d93, 0xff74ab, 0xffa3c7 };
        return v < 12 ? pink[v] : 0xffffff;
    }
    if (v == 1) return sl_theme() == 1 ? 0xffffff : sl_theme() == 2 ? 0x1c1c1e : 0x283340;
    if (v == 2) return sl_theme() == 1 ? 0xe4e9ef : sl_theme() == 2 ? 0x2c2c2e : 0x34414f;
    return v < 12 ? ramp[v] : 0x111111;
}
static uint32_t t48_tile_ink(int v)
{
    if (v <= 2) return C_TEXT;
    if (v >= 12) return 0xf0b53c;
    if (sl_zune()) return v >= 10 ? 0x1a0010 : 0xffffff;
    return v == 11 ? 0x2a1b00 : 0xffffff;
}
static const lv_font_t *t48_font(int v)
{
    int digits = v < 4 ? 1 : v < 7 ? 2 : v < 10 ? 3 : v < 14 ? 4 : 5;
    return digits <= 2 ? &ss_semibold_38 : digits == 3 ? &ss_semibold_34 : digits == 4 ? &ss_semibold_26 : &ss_semibold_21;
}
static int t48_px(int i) { return T48_GAP + (i % T48_N) * (T48_CELL + T48_GAP); }
static int t48_py(int i) { return T48_GAP + (i / T48_N) * (T48_CELL + T48_GAP); }
static lv_obj_t *t48_tile_new(void)
{
    lv_obj_t *t = box(t48_board);
    lv_obj_set_size(t, T48_CELL, T48_CELL);
    lv_obj_set_style_radius(t, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(t, T48_CELL / 2, 0);
    lv_obj_set_style_transform_pivot_y(t, T48_CELL / 2, 0);
    lv_obj_t *l = lv_label_create(t);
    lv_obj_center(l);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    return t;
}
static void t48_paint(lv_obj_t *t, int v)
{
    lv_obj_set_style_bg_color(t, lv_color_hex(t48_tile_bg(v)), 0);
    sl_elevate(t, v >= 11 && !sl_zune() ? SL_E2 : 0, 0);
    if (v >= 11 && !sl_zune())
    {
        lv_obj_set_style_shadow_color(t, lv_color_hex(0xf0b53c), 0);   /* 2048 glows */
        lv_obj_set_style_shadow_width(t, 28, 0);
        lv_obj_set_style_shadow_opa(t, LV_OPA_50, 0);
    }
    lv_obj_t *l = lv_obj_get_child(t, 0);
    char b[12];
    snprintf(b, sizeof b, "%lu", 1ul << v);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_font(l, t48_font(v), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(t48_tile_ink(v)), 0);
    lv_obj_center(l);
}
static void t48_scale_cb(void *o, int32_t v) { lv_obj_set_style_transform_scale(o, v, 0); }
static void t48_pop(lv_obj_t *t, int from, int to, int ms, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, t48_scale_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}
static void t48_show_scores(void)
{
    if (!t48_page_obj) return;
    lv_label_set_text_fmt(t48_score_l, "%lu", (unsigned long)t48.score);
    lv_label_set_text_fmt(t48_best_l, "%lu", (unsigned long)t48_best);
    if (t48_can_undo) lv_obj_remove_state(t48_undo_b, LV_STATE_DISABLED);
    else lv_obj_add_state(t48_undo_b, LV_STATE_DISABLED);
}
static void t48_card_show(int which);   /* 0 none, 1 2048 made, 2 no moves left */
/* the board as the state has it, the tiles that joined popping and the
 * new one growing in */
static void t48_render(bool motion)
{
    for (int i = 0; i < 16; i++)
    {
        lv_obj_add_flag(t48_moving[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *t = t48_tiles[i];
        lv_anim_delete(t, t48_scale_cb);
        lv_obj_set_style_transform_scale(t, 256, 0);
        if (!t48.v[i]) { lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN); continue; }
        t48_paint(t, t48.v[i]);
        lv_obj_set_pos(t, t48_px(i), t48_py(i));
        lv_obj_remove_flag(t, LV_OBJ_FLAG_HIDDEN);
        if (!motion) continue;
        if (t48_joined[i]) t48_pop(t, 256, 300, 150, lv_anim_path_bounce);
        else if (i == t48_born) t48_pop(t, 40, 256, 160, lv_anim_path_ease_out);
    }
    t48_show_scores();
}
static void t48_move(int dir);
static void t48_slide_done(lv_timer_t *tm)
{
    (void)tm;
    if (t48_slide_timer) { lv_timer_delete(t48_slide_timer); t48_slide_timer = NULL; }
    t48_render(true);
    if (t48_over) t48_card_show(2);
    else if (t48.won && !t48.kept) t48_card_show(1);
    int q = t48_queued;
    t48_queued = -1;
    if (q >= 0) t48_move(q);
}
static void t48_x_cb(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void t48_y_cb(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void t48_glide(lv_obj_t *o, int src, int dst)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, T48_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, t48_x_cb);
    lv_anim_set_values(&a, t48_px(src), t48_px(dst));
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, t48_y_cb);
    lv_anim_set_values(&a, t48_py(src), t48_py(dst));
    lv_anim_start(&a);
}
static void t48_move(int dir)
{
    if (!t48_page_obj || t48_over || (t48_card && !lv_obj_has_flag(t48_card, LV_OBJ_FLAG_HIDDEN))) return;
    if (t48_slide_timer) { t48_queued = dir; return; }   /* the slide still going: this one next */
    struct t48 before = t48;
    if (!t48_slide(&t48, dir))
    {
        sfx_play(SFX_TAP);
        return;
    }
    t48_prev = before;
    t48_can_undo = true;
    t48_spawn(&t48);
    t48_over = t48_stuck(&t48);
    if (t48.score > t48_best) t48_best = t48.score;
    /* the old tiles slide: each from where it was to where it lands */
    int used = 0;
    for (int i = 0; i < 16; i++) lv_obj_add_flag(t48_tiles[i], LV_OBJ_FLAG_HIDDEN);
    for (int d = 0; d < 16; d++)
    {
        int srcs[2] = { t48_from[d], t48_second[d] };
        for (int k = 0; k < 2; k++)
        {
            if (srcs[k] < 0 || used >= 16) continue;
            lv_obj_t *m = t48_moving[used++];
            t48_paint(m, before.v[srcs[k]]);
            lv_obj_set_pos(m, t48_px(srcs[k]), t48_py(srcs[k]));
            lv_obj_remove_flag(m, LV_OBJ_FLAG_HIDDEN);
            if (srcs[k] != d) t48_glide(m, srcs[k], d);
        }
    }
    int top = 0;
    for (int i = 0; i < 16; i++) if (t48_joined[i] && t48.v[i] > top) top = t48.v[i];
    if (top) sfx_play_ex(SFX_TILE_MERGE, 80 + top * 8, 100);   /* bigger joins ring higher */
    else sfx_play(SFX_TILE_SLIDE);
    t48_slide_timer = lv_timer_create(t48_slide_done, T48_SLIDE_MS + 10, NULL);
    if (t48_over || sl_ms() - t48_saved_at > 1500) t48_save();
    if (t48_over) sfx_play(SFX_LOSE);
}
static void t48_new_game(void)
{
    if (t48_slide_timer) t48_slide_done(NULL);
    memset(&t48, 0, sizeof t48);
    t48_over = t48_can_undo = false;
    t48_spawn(&t48);
    int first = t48_born;
    t48_spawn(&t48);
    memset(t48_joined, 0, sizeof t48_joined);
    t48_card_show(0);
    t48_render(false);
    if (first >= 0) t48_pop(t48_tiles[first], 40, 256, 160, lv_anim_path_ease_out);
    if (t48_born >= 0) t48_pop(t48_tiles[t48_born], 40, 256, 160, lv_anim_path_ease_out);
    t48_save();
}
static void t48_new_cb(lv_event_t *e) { (void)e; sfx_play(SFX_START); t48_new_game(); }
static void t48_undo_cb(lv_event_t *e)
{
    (void)e;
    if (!t48_can_undo) return;
    if (t48_slide_timer) t48_slide_done(NULL);
    t48 = t48_prev;
    t48_can_undo = t48_over = false;
    t48_card_show(0);
    memset(t48_joined, 0, sizeof t48_joined);
    t48_render(false);
    sfx_play(SFX_UI_BACK);
}
static void t48_keep_cb(lv_event_t *e) { (void)e; t48.kept = true; t48_card_show(0); t48_save(); }

/* the card over the board: 2048 made, or no moves left */
static void t48_card_show(int which)
{
    if (!t48_card) return;
    if (!which) { lv_obj_add_flag(t48_card, LV_OBJ_FLAG_HIDDEN); return; }
    lv_label_set_text(t48_card_title, sl_tr(which == 1 ? "You made 2048!" : "No moves left"));
    char b[64];
    snprintf(b, sizeof b, sl_tr("Score %lu"), (unsigned long)t48.score);
    lv_label_set_text(t48_card_sub, b);
    if (which == 1) { lv_obj_remove_flag(t48_card_keep, LV_OBJ_FLAG_HIDDEN); sfx_play(SFX_WIN); }
    else lv_obj_add_flag(t48_card_keep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(t48_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(t48_card);
    lv_obj_set_style_opa(t48_card, LV_OPA_TRANSP, 0);
    anim_prop(t48_card, gm_opa_cb, 0, 255, SL_T_PAGE, NULL);
}

/* ---- input ---- */
static bool t48_on_top(void) { return t48_page_obj && npages && pages[npages - 1] == t48_page_obj; }
static lv_point_t t48_down;
static bool t48_swiped;
static void t48_area_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (code == LV_EVENT_PRESSED) { t48_down = p; t48_swiped = false; return; }
    if (code != LV_EVENT_PRESSING || t48_swiped) return;
    int dx = p.x - t48_down.x, dy = p.y - t48_down.y;
    if (dx * dx + dy * dy < 28 * 28) return;
    if (abs(dx) * 2 < abs(dy) * 3 && abs(dy) * 2 < abs(dx) * 3) return;   /* diagonal: wait for a clear lead */
    t48_swiped = true;                     /* one move a stroke */
    t48_move(abs(dx) > abs(dy) ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0));
}
static bool t48_key(int b, bool rel)
{
    if (!t48_on_top()) return false;
    int dir = b == BUTTON_UP ? 0 : b == BUTTON_RIGHT ? 1 : b == BUTTON_DOWN ? 2 : b == BUTTON_LEFT ? 3 : -1;
    if (dir < 0) return false;
    if (!rel) t48_move(dir);
    return true;
}

/* ---- the page ---- */
static void t48_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != t48_page_obj) return;
    if (t48_slide_timer) { lv_timer_delete(t48_slide_timer); t48_slide_timer = NULL; }
    for (int i = 0; i < 16; i++) { lv_anim_delete(t48_tiles[i], NULL); lv_anim_delete(t48_moving[i], NULL); }
    t48_save();
    t48_page_obj = t48_board = t48_card = NULL;
    statusbar_retheme();
}
/* a score in a small card: its name over its number, as tall as they need */
static lv_obj_t *gm_chip(lv_obj_t *p, int x, int y, int w, const char *name, lv_obj_t **value)
{
    lv_obj_t *c = box(p);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(c, 66, 0);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(c, SL_SP_XS, 0);
    hairline(c);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char up[48];
    lv_obj_t *n = text(c, sl_zune() ? sl_lower(sl_tr(name), up, sizeof up) : set_caps(sl_tr(name), up, sizeof up), &ss_semibold_18, C_DIM);
    lv_obj_set_width(n, w - 2 * SL_SP_S);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    *value = text(c, "0", &ss_semibold_26, C_TEXT);
    return c;
}
static lv_obj_t *t48_chip(lv_obj_t *p, int x, const char *name, lv_obj_t **value)
{
    return gm_chip(p, x, T48_TOP + 12, 132, name, value);
}
static lv_obj_t *t48_round(lv_obj_t *p, int x, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = box(p);
    lv_obj_set_size(b, 60, 60);
    lv_obj_set_pos(b, x, T48_TOP + 15);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    hairline(b);
    press_motion(b);
    lv_obj_center(text(b, sym, &sl_sym_22, C_TEXT));
    on_click(b, cb, NULL);
    return b;
}
static void t48_page(void)
{
    if (page_bring_back(t48_page_obj)) return;
    t48_load();
    lv_obj_t *p, *body = page_new("2048", &p);
    t48_page_obj = p;
    lv_obj_add_event_cb(p, t48_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    /* the whole page under the header takes swipes; what sits on it takes taps */
    lv_obj_t *area = box(p);
    lv_obj_set_size(area, W, H - T48_TOP);
    lv_obj_set_pos(area, 0, T48_TOP);
    lv_obj_add_flag(area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe: a right swipe is a move */
    lv_obj_add_event_cb(area, t48_area_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(area, t48_area_cb, LV_EVENT_PRESSING, NULL);
    bool rtl = sl_ui_rtl();
    t48_chip(p, rtl ? W - T48_X - 132 : T48_X, "Score", &t48_score_l);
    t48_chip(p, rtl ? W - T48_X - 2 * 132 - SL_SP_S : T48_X + 132 + SL_SP_S, "Best", &t48_best_l);
    t48_undo_b = t48_round(p, rtl ? T48_X + 60 + SL_GUTTER : W - T48_X - 2 * 60 - SL_GUTTER, LV_SYMBOL_LEFT, t48_undo_cb);
    t48_round(p, rtl ? T48_X : W - T48_X - 60, LV_SYMBOL_REFRESH, t48_new_cb);

    t48_board = box(p);
    lv_obj_set_size(t48_board, T48_BOARD, T48_BOARD);
    lv_obj_set_pos(t48_board, T48_X, T48_Y);
    lv_obj_set_style_radius(t48_board, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(t48_board, lv_color_hex(sl_zune() ? 0x111111 : sl_theme() == 1 ? 0xd3dae2 : sl_theme() == 2 ? 0x101012 : 0x1a222c), 0);
    lv_obj_set_style_bg_opa(t48_board, LV_OPA_COVER, 0);
    lv_obj_remove_flag(t48_board, LV_OBJ_FLAG_CLICKABLE);    /* swipes go through to the area */
    for (int i = 0; i < 16; i++)
    {
        lv_obj_t *slot = box(t48_board);                      /* the empty places */
        lv_obj_set_size(slot, T48_CELL, T48_CELL);
        lv_obj_set_pos(slot, t48_px(i), t48_py(i));
        lv_obj_set_style_radius(slot, sl_zune() ? 0 : SL_R_MD, 0);
        lv_obj_set_style_bg_color(slot, lv_color_hex(sl_zune() ? 0x1c1c1c : sl_theme() == 1 ? 0xe6ebf0 : sl_theme() == 2 ? 0x1a1a1c : 0x222c38), 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < 16; i++) { t48_tiles[i] = t48_tile_new(); lv_obj_remove_flag(t48_tiles[i], LV_OBJ_FLAG_CLICKABLE); }
    for (int i = 0; i < 16; i++) { t48_moving[i] = t48_tile_new(); lv_obj_remove_flag(t48_moving[i], LV_OBJ_FLAG_CLICKABLE); }

    lv_obj_t *hint = text(p, "Swipe to slide every tile. Two alike join.", &ss_regular_16, C_SUB);
    lv_obj_set_width(hint, T48_BOARD);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, T48_X, T48_Y + T48_BOARD + SL_GUTTER);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    /* the card: 2048 made, or no moves left */
    t48_card = box(p);
    lv_obj_set_width(t48_card, 320);
    lv_obj_set_height(t48_card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(t48_card, 20, 0);
    lv_obj_set_style_pad_row(t48_card, SL_SP_S, 0);
    lv_obj_set_style_radius(t48_card, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(t48_card, lv_color_hex(C_SHEET), 0);
    lv_obj_set_style_bg_opa(t48_card, LV_OPA_COVER, 0);
    sl_elevate(t48_card, SL_E3, 0);
    lv_obj_set_flex_flow(t48_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t48_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(t48_card, LV_ALIGN_TOP_MID, 0, T48_Y + T48_BOARD / 2 - 110);
    lv_obj_add_flag(t48_card, LV_OBJ_FLAG_CLICKABLE);
    t48_card_title = text(t48_card, "", &ss_bold_28, C_TEXT);
    t48_card_sub = text(t48_card, "", &ss_regular_18, C_SUB);
    t48_card_keep = mn_pill(t48_card, "Keep going", 260, SL_PILL_H, false, t48_keep_cb, NULL);
    t48_card_go = mn_pill(t48_card, "New game", 260, SL_PILL_H, true, t48_new_cb, NULL);
    lv_obj_add_flag(t48_card, LV_OBJ_FLAG_HIDDEN);

    memset(t48_joined, 0, sizeof t48_joined);
    t48_born = -1;
    t48_over = t48_stuck(&t48);
    t48_render(false);
    if (t48_over) t48_card_show(2);
    page_push(p);
}
