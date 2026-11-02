/* 24 Points: make 24 from four cards with + - x and /. GPLv2+.
 *
 * Pick a card, an operation, then another card: the two become one card
 * holding the answer (a fraction when it is one) where the second was.
 * Down to one card: 24 wins the deal and the next one comes in. Every deal
 * can be made (each is checked before it is dealt). Undo goes back a step,
 * Start over goes back to the four, Hint shows a first step that leads to
 * 24, and New deal moves on (and ends the streak). Deals solved, the streak
 * and the best streak are kept on the card (points.txt). */

#define PT_FILE  ROCKBOX_DIR "/sloppy/points.txt"
#define PT_TOP   (SB_H + SL_HDR_H)
#define PT_CW    206
#define PT_CH    172
#define PT_GAP   16
#define PT_X     ((W - 2 * PT_CW - PT_GAP) / 2)
#define PT_Y     (PT_TOP + 92)
#define PT_MSG_Y (PT_Y + 2 * PT_CH + PT_GAP + SL_SP_S)
#define PT_OPS_Y (PT_MSG_Y + 32)
#define PT_TOOLS_Y (PT_OPS_Y + 84 + SL_GUTTER)

struct pt_val { int n, d; bool live; };      /* n / d, d > 0, in lowest terms */
static struct {
    struct pt_val c[4];
    int deal[4];
    int sel, op;                             /* the card and the operation picked, -1 none */
    struct pt_val hist[3][4];
    int nhist;
    bool won, hinted;
} pt;
static uint32_t pt_solved, pt_streak, pt_best, pt_rng;
static bool pt_loaded;
static lv_obj_t *pt_page_obj, *pt_cards[4], *pt_card_l[4], *pt_ops[4], *pt_solved_l, *pt_streak_l, *pt_best_l, *pt_msg;
static lv_timer_t *pt_next_timer;
static const char *const pt_op_sym[4] = { "+", "\xe2\x80\x93", "\xc3\x97", "\xc3\xb7" };   /* + - x /, the minus an en dash (the face has no U+2212) */

static uint32_t pt_rand(void) { pt_rng = pt_rng * 1664525u + 1013904223u; return pt_rng >> 8; }
static int pt_gcd(int a, int b) { if (a < 0) a = -a; if (b < 0) b = -b; while (b) { int t = a % b; a = b; b = t; } return a ? a : 1; }
static struct pt_val pt_make(int n, int d)
{
    if (d < 0) { n = -n; d = -d; }
    int g = pt_gcd(n, d);
    return (struct pt_val){ n / g, d / g, true };
}
/* a op b; false for a division by nothing */
static bool pt_apply(struct pt_val a, struct pt_val b, int op, struct pt_val *out)
{
    switch (op)
    {
    case 0: *out = pt_make(a.n * b.d + b.n * a.d, a.d * b.d); return true;
    case 1: *out = pt_make(a.n * b.d - b.n * a.d, a.d * b.d); return true;
    case 2: *out = pt_make(a.n * b.n, a.d * b.d); return true;
    default: if (!b.n) return false; *out = pt_make(a.n * b.d, a.d * b.n); return true;
    }
}
/* can these make 24? first: the first step of a way (a, op, b) */
static bool pt_solve(const struct pt_val *v, int n, int *fa, int *fop, int *fb)
{
    if (n == 1) return v[0].n == 24 && v[0].d == 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
        {
            if (i == j) continue;
            for (int op = 0; op < 4; op++)
            {
                if ((op == 0 || op == 2) && j < i) continue;   /* + and x: one order is enough */
                struct pt_val r, rest[4];
                if (!pt_apply(v[i], v[j], op, &r)) continue;
                int m = 0;
                for (int k = 0; k < n; k++) if (k != i && k != j) rest[m++] = v[k];
                rest[m++] = r;
                if (pt_solve(rest, m, NULL, NULL, NULL))
                {
                    if (fa) { *fa = i; *fop = op; *fb = j; }
                    return true;
                }
            }
        }
    return false;
}
static void pt_deal(void)
{
    for (int tries = 0; tries < 400; tries++)
    {
        struct pt_val v[4];
        for (int k = 0; k < 4; k++) { pt.deal[k] = 1 + (int)(pt_rand() % 13); v[k] = pt_make(pt.deal[k], 1); }
        if (pt_solve(v, 4, NULL, NULL, NULL)) break;
    }
    for (int k = 0; k < 4; k++) pt.c[k] = pt_make(pt.deal[k], 1);
    pt.sel = pt.op = -1;
    pt.nhist = 0;
    pt.won = pt.hinted = false;
}
static int pt_live(void) { int n = 0; for (int k = 0; k < 4; k++) n += pt.c[k].live; return n; }

static void pt_load(void)
{
    if (pt_loaded) return;
    pt_loaded = true;
    pt_rng = sl_ms() * 2654435761u + 3;
    int fd = open(PT_FILE, O_RDONLY);
    char b[64];
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got <= 0) return;
    b[got] = 0;
    unsigned s, st, best;
    if (sscanf(b, "PTS1 %u %u %u", &s, &st, &best) == 3) { pt_solved = s; pt_streak = st; pt_best = best; }
}
static void pt_save(void)
{
    char b[64];
    int n = snprintf(b, sizeof b, "PTS1 %u %u %u\n", (unsigned)pt_solved, (unsigned)pt_streak, (unsigned)pt_best);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(PT_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, n) == n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(PT_FILE ".tmp", PT_FILE) < 0) remove(PT_FILE ".tmp");
}

/* ---- looks ---- */
static uint32_t pt_face(void) { return sl_zune() ? 0x141414 : sl_theme() == 1 ? 0xffffff : sl_theme() == 2 ? 0x1c1c1e : 0x22303e; }
static void pt_show(void)
{
    if (!pt_page_obj) return;
    uint32_t acc = sl_zune() ? ZN_PINK : C_ACCENT_UI;
    for (int k = 0; k < 4; k++)
    {
        lv_obj_t *c = pt_cards[k];
        if (!pt.c[k].live) { lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
        char b[24];
        if (pt.c[k].d == 1) snprintf(b, sizeof b, "%d", pt.c[k].n);
        else snprintf(b, sizeof b, "%d/%d", pt.c[k].n, pt.c[k].d);
        lv_label_set_text(pt_card_l[k], b);
        lv_obj_set_style_text_font(pt_card_l[k], strlen(b) <= 2 ? &ss_regular_60 : strlen(b) <= 4 ? &ss_semibold_38 : &ss_semibold_26, 0);
        bool sel = k == pt.sel, win = pt.won;
        lv_obj_set_style_bg_color(c, lv_color_hex(win ? acc : sel ? lv_color_to_u32(lv_color_mix(lv_color_hex(acc), lv_color_hex(pt_face()), 90)) & 0xffffff : pt_face()), 0);
        lv_obj_set_style_border_width(c, sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(acc), 0);
        lv_obj_set_style_text_color(pt_card_l[k], lv_color_hex(win ? sl_ink_on(acc) : C_TEXT), 0);
    }
    for (int o = 0; o < 4; o++)
    {
        bool on = o == pt.op;
        lv_obj_set_style_bg_color(pt_ops[o], lv_color_hex(on ? acc : C_CARD), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(pt_ops[o], 0), lv_color_hex(on ? sl_ink_on(acc) : C_TEXT), 0);
    }
    lv_label_set_text_fmt(pt_solved_l, "%u", (unsigned)pt_solved);
    lv_label_set_text_fmt(pt_streak_l, "%u", (unsigned)pt_streak);
    lv_label_set_text_fmt(pt_best_l, "%u", (unsigned)pt_best);
}
static void pt_say(const char *s)
{
    lv_label_set_text(pt_msg, s ? sl_tr(s) : "");
}
static void pt_tx_cb(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void pt_shake_done(lv_anim_t *a) { lv_obj_set_style_translate_x(a->var, 0, 0); }
static void pt_scale_cb(void *o, int32_t v) { lv_obj_set_style_transform_scale(o, v, 0); }
/* a card arriving or just made: it grows in with a little overshoot */
static void pt_pop(lv_obj_t *o, int delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, pt_scale_cb);
    lv_anim_set_values(&a, 200, 256);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_obj_set_style_transform_scale(o, delay ? 200 : 256, 0);
    lv_anim_start(&a);
}
static void pt_shake(lv_obj_t *o)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, pt_tx_cb);
    lv_anim_set_values(&a, -10, 10);
    lv_anim_set_duration(&a, 45);
    lv_anim_set_playback_duration(&a, 45);
    lv_anim_set_repeat_count(&a, 2);
    lv_anim_set_ready_cb(&a, pt_shake_done);
    lv_anim_start(&a);
}
static void pt_next_cb(lv_timer_t *t)
{
    (void)t;
    if (pt_next_timer) lv_timer_delete(pt_next_timer);   /* New deal calls this with none waiting */
    pt_next_timer = NULL;
    pt_deal();
    pt_say(NULL);
    pt_show();
    for (int k = 0; k < 4; k++) pt_pop(pt_cards[k], 60 * k);
}
static void pt_card_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (pt.won || !pt.c[k].live) return;
    if (pt.sel < 0 || pt.op < 0 || pt.sel == k)
    {
        pt.sel = pt.sel == k ? -1 : k;      /* pick it, or put it back */
        if (pt.sel < 0) pt.op = -1;
        sfx_play(SFX_TAP);
        pt_show();
        return;
    }
    struct pt_val r;
    if (!pt_apply(pt.c[pt.sel], pt.c[k], pt.op, &r))
    {
        pt_say("Nothing divides by 0");
        sfx_play(SFX_ERROR);
        pt_shake(pt_cards[k]);
        return;
    }
    memcpy(pt.hist[pt.nhist++], pt.c, sizeof pt.c);
    pt.c[pt.sel].live = false;
    pt.c[k] = r;
    pt.sel = k;                             /* the answer is picked: carry on from it */
    pt.op = -1;
    pt_say(NULL);
    pt_pop(pt_cards[k], 0);
    if (pt_live() == 1)
    {
        pt.sel = -1;
        if (r.n == 24 && r.d == 1)
        {
            pt.won = true;
            if (!pt.hinted) { pt_solved++; pt_streak++; if (pt_streak > pt_best) pt_best = pt_streak; }
            pt_save();
            pt_say(pt.hinted ? "24! With a hint, so the streak waits" : "24!");
            sfx_play(SFX_WIN);
            pt_next_timer = lv_timer_create(pt_next_cb, 1300, NULL);
        }
        else
        {
            pt_say("Not 24. Undo, or start over");
            sfx_play(SFX_LOSE);
            pt_shake(pt_cards[k]);
        }
    }
    else sfx_play(SFX_POINTS_JOIN);
    pt_show();
}
static void pt_op_cb(lv_event_t *e)
{
    int o = (int)(intptr_t)lv_event_get_user_data(e);
    if (pt.won) return;
    if (pt.sel < 0) { pt_say("Pick a card first"); sfx_play(SFX_TAP); return; }
    pt.op = pt.op == o ? -1 : o;
    sfx_play(SFX_TAP);
    pt_show();
}
static void pt_undo_cb(lv_event_t *e)
{
    (void)e;
    if (pt.won || !pt.nhist) return;
    memcpy(pt.c, pt.hist[--pt.nhist], sizeof pt.c);
    pt.sel = pt.op = -1;
    pt_say(NULL);
    sfx_play(SFX_UI_BACK);
    pt_show();
}
static void pt_reset_cb(lv_event_t *e)
{
    (void)e;
    if (pt.won || !pt.nhist) return;
    memcpy(pt.c, pt.hist[0], sizeof pt.c);
    pt.nhist = 0;
    pt.sel = pt.op = -1;
    pt_say(NULL);
    sfx_play(SFX_UI_BACK);
    pt_show();
}
static void pt_hint_cb(lv_event_t *e)
{
    (void)e;
    if (pt.won) return;
    struct pt_val v[4];
    int idx[4], n = 0;
    for (int k = 0; k < 4; k++) if (pt.c[k].live) { idx[n] = k; v[n++] = pt.c[k]; }
    int a, op, b;
    if (!pt_solve(v, n, &a, &op, &b)) { pt_say("No way to 24 from here. Undo, or start over"); sfx_play(SFX_ERROR); return; }
    pt.hinted = true;
    pt.sel = idx[a];
    pt.op = op;
    char s[64], x[16], y[16];
    snprintf(x, sizeof x, v[a].d == 1 ? "%d" : "%d/%d", v[a].n, v[a].d);
    snprintf(y, sizeof y, v[b].d == 1 ? "%d" : "%d/%d", v[b].n, v[b].d);
    snprintf(s, sizeof s, sl_tr("Try %s %s %s"), x, pt_op_sym[op], y);
    lv_label_set_text(pt_msg, s);
    sfx_play(SFX_TAP);
    pt_show();
}
static void pt_new_cb(lv_event_t *e)
{
    (void)e;
    if (pt_next_timer) { lv_timer_delete(pt_next_timer); pt_next_timer = NULL; }
    if (!pt.won) { pt_streak = 0; pt_save(); }   /* a deal left unsolved ends the run */
    sfx_play(SFX_START);
    pt_next_cb(NULL);
}

static bool pt_on_top(void) { return pt_page_obj && npages && pages[npages - 1] == pt_page_obj; }
static void pt_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != pt_page_obj) return;
    if (pt_next_timer) { lv_timer_delete(pt_next_timer); pt_next_timer = NULL; }
    for (int k = 0; k < 4; k++) lv_anim_delete(pt_cards[k], NULL);
    pt_save();
    pt_page_obj = NULL;
}
static void points_page(void)
{
    if (page_bring_back(pt_page_obj)) return;
    pt_load();
    lv_obj_t *p, *body = page_new("24 Points", &p);
    pt_page_obj = p;
    lv_obj_add_event_cb(p, pt_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    pt_deal();
    /* three numbers across the top: solved, streak, best */
    static const char *const names[3] = { "Solved", "Streak", "Best streak" };
    lv_obj_t **vals[3] = { &pt_solved_l, &pt_streak_l, &pt_best_l };
    int cw = (2 * PT_CW + PT_GAP - 2 * SL_SP_S) / 3;
    int chips_end = 0;
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *c = gm_chip(p, PT_X + (sl_ui_rtl() ? 2 - i : i) * (cw + SL_SP_S), PT_TOP + 12, cw, names[i], vals[i]);
        lv_obj_update_layout(c);
        chips_end = LV_MAX(chips_end, lv_obj_get_y(c) + lv_obj_get_height(c));
    }
    /* larger words make the chips taller: the cards start under them and
     * give the height back, so the rest stays where it is */
    int dy = LV_MAX(0, chips_end + SL_SP_S - PT_Y), ch = PT_CH - (dy + 1) / 2;
    /* the four cards */
    for (int k = 0; k < 4; k++)
    {
        lv_obj_t *c = box(p);
        lv_obj_set_size(c, PT_CW, ch);
        lv_obj_set_pos(c, PT_X + (k % 2) * (PT_CW + PT_GAP), PT_Y + dy + (k / 2) * (ch + PT_GAP));
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        sl_elevate(c, SL_E2, 0);
        lv_obj_set_style_transform_pivot_x(c, PT_CW / 2, 0);
        lv_obj_set_style_transform_pivot_y(c, ch / 2, 0);
        press_motion(c);
        pt_card_l[k] = text(c, "", &ss_regular_60, C_TEXT);
        lv_obj_center(pt_card_l[k]);
        on_click(c, pt_card_cb, (void *)(intptr_t)k);
        pt_cards[k] = c;
    }
    /* the four operations */
    int ow = 84, og = (2 * PT_CW + PT_GAP - 4 * ow) / 3;
    for (int o = 0; o < 4; o++)
    {
        lv_obj_t *b = box(p);
        lv_obj_set_size(b, ow, ow);
        lv_obj_set_pos(b, PT_X + o * (ow + og), PT_OPS_Y);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        hairline(b);
        press_motion(b);
        lv_obj_center(text(b, pt_op_sym[o], &ss_semibold_38, C_TEXT));
        on_click(b, pt_op_cb, (void *)(intptr_t)o);
        pt_ops[o] = b;
    }
    pt_msg = text(p, "", &ss_semibold_18, C_SUB);
    lv_obj_set_width(pt_msg, 2 * PT_CW + PT_GAP);
    lv_label_set_long_mode(pt_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pt_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(pt_msg, PT_X, PT_MSG_Y);
    /* and what to do about a deal */
    int tw = (2 * PT_CW + PT_GAP) / 4;
    static const char *const tsym[4] = { LV_SYMBOL_LEFT, LV_SYMBOL_REFRESH, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_SHUFFLE };
    static const char *const tname[4] = { "Undo", "Start over", "Hint", "New deal" };
    lv_event_cb_t tcb[4] = { pt_undo_cb, pt_reset_cb, pt_hint_cb, pt_new_cb };
    for (int k = 0; k < 4; k++)
        gm_tool(p, PT_X + (sl_ui_rtl() ? 3 - k : k) * tw + SL_SP_XS, PT_TOOLS_Y, tw - SL_SP_S, tsym[k], tname[k], tcb[k]);
    pt_show();
    page_push(p);
}
