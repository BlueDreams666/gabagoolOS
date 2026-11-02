/* Sudoku. GPLv2+.
 *
 * Each puzzle is made here: a full grid filled at random, then numbers
 * taken out one by one while it still has only one answer, until as few are
 * left as the difficulty asks (Easy 38, Medium 32, Hard 27, Expert 24). The
 * board is one object drawing itself: the cell picked, its row, column and
 * box, the other cells with its number, notes in a little three by three,
 * and wrong answers in red. Under it, Undo, Erase, Notes and Hint, then the
 * nine numbers (one fades when all nine of it are placed).
 *
 * The clock runs only while the page is in sight and the player awake. The
 * game in progress and the best time for each difficulty are kept on the
 * card (sudoku.txt). */

#define SDK_FILE  ROCKBOX_DIR "/sloppy/sudoku.txt"
#define SDK_CELL  48
#define SDK_PAD   6
#define SDK_BOARD (9 * SDK_CELL + 2 * SDK_PAD)    /* 444 */
#define SDK_X     ((W - SDK_BOARD) / 2)
#define SDK_TOP   (SB_H + SL_HDR_H)
#define SDK_Y     (SDK_TOP + 56)
#define SDK_TOOLS_Y (SDK_Y + SDK_BOARD + SL_GUTTER)
#define SDK_KEYS_Y  (SDK_TOOLS_Y + 72)
#define SDK_UNDO  128

static const char *const sdk_diff_names[4] = { "Easy", "Medium", "Hard", "Expert" };
static const uint8_t sdk_givens[4] = { 38, 32, 27, 24 };
static struct {
    uint8_t sol[81], val[81];
    bool given[81];
    uint16_t notes[81];                    /* bit d: d is noted */
    int diff, hints, mistakes;
    uint32_t ms;                           /* time played */
    bool done, live;
} sdk;
static uint32_t sdk_best[4];               /* ms, 0 none */
static bool sdk_loaded, sdk_notes_mode, sdk_new_best;
static int sdk_sel = -1;
static struct { int8_t cell; uint8_t val; uint16_t notes; } sdk_undo[SDK_UNDO];
static int sdk_nundo;
static uint32_t sdk_rng, sdk_tick_at, sdk_saved_at;
static lv_obj_t *sdk_page_obj, *sdk_board, *sdk_clock, *sdk_diff_l, *sdk_keys[9], *sdk_notes_b, *sdk_hint_n;
static lv_obj_t *sdk_card, *sdk_card_title, *sdk_card_sub;
static lv_timer_t *sdk_timer;

/* ---- rules ---- */
static uint32_t sdk_rand(void) { sdk_rng = sdk_rng * 1664525u + 1013904223u; return sdk_rng >> 8; }
static uint16_t sdk_cands(const uint8_t *g, int i)
{
    int r = i / 9, c = i % 9, br = r / 3 * 3, bc = c / 3 * 3;
    uint16_t used = 0;
    for (int k = 0; k < 9; k++)
    {
        used |= 1u << g[r * 9 + k];
        used |= 1u << g[k * 9 + c];
        used |= 1u << g[(br + k / 3) * 9 + bc + k % 3];
    }
    return (uint16_t)(~used & 0x3fe);
}
/* answers to g, counted up to limit; fill: the first one found is left in g */
static int sdk_solve(uint8_t *g, int limit, bool shuffle)
{
    int best = -1, bestn = 10;
    uint16_t bm = 0;
    for (int i = 0; i < 81; i++)
    {
        if (g[i]) continue;
        uint16_t m = sdk_cands(g, i);
        int n = __builtin_popcount(m);
        if (!n) return 0;
        if (n < bestn) { best = i; bestn = n; bm = m; if (n == 1) break; }
    }
    if (best < 0) return 1;
    int digits[9], nd = 0;
    for (int d = 1; d <= 9; d++) if (bm & (1u << d)) digits[nd++] = d;
    if (shuffle)
        for (int k = nd - 1; k > 0; k--) { int j = sdk_rand() % (k + 1), t = digits[k]; digits[k] = digits[j]; digits[j] = t; }
    int total = 0;
    for (int k = 0; k < nd && total < limit; k++)
    {
        g[best] = (uint8_t)digits[k];
        total += sdk_solve(g, limit - total, shuffle);
        if (total >= limit && limit == 1) return total;   /* filling: keep this one */
    }
    g[best] = 0;
    return total;
}
static void sdk_make(int diff)
{
    uint8_t full[81] = { 0 }, g[81];
    sdk_solve(full, 1, true);              /* a full grid, at random */
    memcpy(g, full, 81);
    int order[81];
    for (int i = 0; i < 81; i++) order[i] = i;
    for (int k = 80; k > 0; k--) { int j = sdk_rand() % (k + 1), t = order[k]; order[k] = order[j]; order[j] = t; }
    int left = 81;
    for (int k = 0; k < 81 && left > sdk_givens[diff]; k++)
    {
        int i = order[k];
        uint8_t keep = g[i];
        g[i] = 0;
        uint8_t t[81];
        memcpy(t, g, 81);
        if (sdk_solve(t, 2, false) == 1) left--;
        else g[i] = keep;                  /* two answers without it: it stays */
    }
    memset(&sdk, 0, sizeof sdk);
    memcpy(sdk.sol, full, 81);
    for (int i = 0; i < 81; i++) { sdk.given[i] = g[i] != 0; sdk.val[i] = g[i]; }
    sdk.diff = diff;
    sdk.live = true;
    sdk_nundo = 0;
    sdk_sel = -1;
}
static bool sdk_wrong(int i) { return sdk.val[i] && !sdk.given[i] && sdk.val[i] != sdk.sol[i]; }
static int sdk_placed(int d) { int n = 0; for (int i = 0; i < 81; i++) n += sdk.val[i] == d && !sdk_wrong(i); return n; }
static bool sdk_solved(void) { for (int i = 0; i < 81; i++) if (sdk.val[i] != sdk.sol[i]) return false; return true; }
static bool sdk_peer(int a, int b)
{
    return a / 9 == b / 9 || a % 9 == b % 9 || (a / 27 == b / 27 && a % 9 / 3 == b % 9 / 3);
}

/* ---- the card ---- */
static void sdk_save(void)
{
    char b[600];
    int n = snprintf(b, sizeof b, "SDK1 %d %u %d %d %d %u %u %u %u\n", sdk.diff, (unsigned)sdk.ms, sdk.hints, sdk.mistakes,
                     sdk.live && !sdk.done, (unsigned)sdk_best[0], (unsigned)sdk_best[1], (unsigned)sdk_best[2], (unsigned)sdk_best[3]);
    for (int i = 0; i < 81; i++) b[n++] = (char)('0' + sdk.sol[i]);
    b[n++] = '\n';
    for (int i = 0; i < 81; i++) b[n++] = sdk.given[i] ? (char)('A' + sdk.val[i]) : (char)('0' + sdk.val[i]);
    b[n++] = '\n';
    for (int i = 0; i < 81; i++) n += snprintf(b + n, sizeof b - n, "%03x", sdk.notes[i]);
    b[n++] = '\n';
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(SDK_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, n) == n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(SDK_FILE ".tmp", SDK_FILE) < 0) remove(SDK_FILE ".tmp");
    sdk_saved_at = sl_ms();
}
static void sdk_load(void)
{
    if (sdk_loaded) return;
    sdk_loaded = true;
    sdk_rng = sl_ms() * 2246822519u + 7;
    static char b[700];
    int fd = open(SDK_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got <= 0) return;
    b[got] = 0;
    int diff, hints, mistakes, live;
    unsigned ms, b0, b1, b2, b3;
    if (sscanf(b, "SDK1 %d %u %d %d %d %u %u %u %u", &diff, &ms, &hints, &mistakes, &live, &b0, &b1, &b2, &b3) != 9) return;
    sdk_best[0] = b0; sdk_best[1] = b1; sdk_best[2] = b2; sdk_best[3] = b3;
    char *l1 = strchr(b, '\n'), *l2 = l1 ? strchr(l1 + 1, '\n') : NULL, *l3 = l2 ? strchr(l2 + 1, '\n') : NULL;
    if (!live || !l3 || strlen(l3 + 1) < 243 || diff < 0 || diff > 3) return;
    for (int i = 0; i < 81; i++)
    {
        char s = l1[1 + i], v = l2[1 + i];
        if (s < '1' || s > '9') return;
        sdk.sol[i] = (uint8_t)(s - '0');
        sdk.given[i] = v >= 'A';
        sdk.val[i] = (uint8_t)(sdk.given[i] ? v - 'A' : v - '0');
        if (sdk.val[i] > 9) return;
        char h[4] = { l3[1 + 3 * i], l3[2 + 3 * i], l3[3 + 3 * i], 0 };
        sdk.notes[i] = (uint16_t)(strtoul(h, NULL, 16) & 0x3fe);
    }
    sdk.diff = diff; sdk.ms = ms; sdk.hints = hints; sdk.mistakes = mistakes;
    sdk.live = true;
}

/* ---- the board, drawn ---- */
static uint32_t sdk_red(void) { return sl_theme() == 1 ? 0xc8282e : 0xff6b6e; }
static void sdk_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(sdk_board, &a);
    int x0 = a.x1 + SDK_PAD, y0 = a.y1 + SDK_PAD;
    uint32_t acc = sl_zune() ? ZN_PINK : C_ACCENT_UI;
    int seld = sdk_sel >= 0 ? sdk.val[sdk_sel] : 0;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    for (int i = 0; i < 81; i++)
    {
        int cx = x0 + (i % 9) * SDK_CELL, cy = y0 + (i / 9) * SDK_CELL;
        lv_opa_t o = 0;
        lv_color_t c = lv_color_hex(acc);
        if (i == sdk_sel) o = sl_theme() == 1 ? 90 : 120;
        else if (seld && sdk.val[i] == seld) o = sl_theme() == 1 ? 50 : 70;
        else if (sdk_sel >= 0 && sdk_peer(i, sdk_sel)) { c = lv_color_hex(C_TEXT); o = sl_theme() == 1 ? 14 : 18; }
        if (sdk_wrong(i) && i != sdk_sel) { c = lv_color_hex(sdk_red()); o = 40; }
        if (!o) continue;
        r.bg_color = c;
        r.bg_opa = o;
        r.radius = 0;
        lv_area_t ca = { cx, cy, cx + SDK_CELL - 1, cy + SDK_CELL - 1 };
        lv_draw_rect(layer, &r, &ca);
    }
    /* lines: hairlines between cells, firmer ones round the boxes */
    for (int k = 1; k < 9; k++)
    {
        bool box3 = k % 3 == 0;
        r.bg_color = lv_color_hex(box3 ? C_SUB : C_DIM);
        r.bg_opa = box3 ? (sl_theme() == 1 ? 150 : 170) : (sl_theme() == 1 ? 60 : 80);
        int w = box3 ? 2 : 1;
        lv_area_t v = { x0 + k * SDK_CELL - w / 2, y0, x0 + k * SDK_CELL - w / 2 + w - 1, y0 + 9 * SDK_CELL - 1 };
        lv_draw_rect(layer, &r, &v);
        lv_area_t h = { x0, y0 + k * SDK_CELL - w / 2, x0 + 9 * SDK_CELL - 1, y0 + k * SDK_CELL - w / 2 + w - 1 };
        lv_draw_rect(layer, &r, &h);
    }
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.align = LV_TEXT_ALIGN_CENTER;
    char s[2] = { 0, 0 };
    t.text = s;
    t.text_local = 1;
    for (int i = 0; i < 81; i++)
    {
        int cx = x0 + (i % 9) * SDK_CELL, cy = y0 + (i / 9) * SDK_CELL;
        if (sdk.val[i])
        {
            s[0] = (char)('0' + sdk.val[i]);
            t.font = sdk.given[i] ? &ss_semibold_30 : &ss_regular_34;
            t.color = lv_color_hex(sdk.given[i] ? C_TEXT : sdk_wrong(i) ? sdk_red() : sl_accent_on(C_CARD, 45));
            int lh = lv_font_get_line_height(t.font);
            lv_area_t la = { cx, cy + (SDK_CELL - lh) / 2, cx + SDK_CELL - 1, cy + (SDK_CELL - lh) / 2 + lh - 1 };
            lv_draw_label(layer, &t, &la);
        }
        else if (sdk.notes[i])
        {
            t.font = &sl_sym_14;
            t.color = lv_color_hex(C_SUB);
            for (int d = 1; d <= 9; d++)
            {
                if (!(sdk.notes[i] & (1u << d))) continue;
                s[0] = (char)('0' + d);
                int sx = cx + 1 + (d - 1) % 3 * 15, sy = cy + 1 + (d - 1) / 3 * 15;
                lv_area_t la = { sx, sy, sx + 15, sy + 15 };
                lv_draw_label(layer, &t, &la);
            }
        }
    }
}

/* ---- play ---- */
static void sdk_show(void)
{
    if (!sdk_page_obj) return;
    lv_obj_invalidate(sdk_board);
    for (int d = 1; d <= 9; d++)
        lv_obj_set_style_opa(sdk_keys[d - 1], sdk_placed(d) >= 9 ? LV_OPA_30 : LV_OPA_COVER, 0);
    if (sdk_notes_mode) lv_obj_add_state(sdk_notes_b, LV_STATE_CHECKED);
    else lv_obj_remove_state(sdk_notes_b, LV_STATE_CHECKED);
    lv_label_set_text_fmt(sdk_hint_n, "%d", sdk.hints);
    if (sdk.hints) lv_obj_remove_flag(lv_obj_get_parent(sdk_hint_n), LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(lv_obj_get_parent(sdk_hint_n), LV_OBJ_FLAG_HIDDEN);
}
static void sdk_clock_show(void)
{
    if (!sdk_clock) return;
    unsigned s = sdk.ms / 1000;
    if (s >= 3600) lv_label_set_text_fmt(sdk_clock, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else lv_label_set_text_fmt(sdk_clock, "%u:%02u", s / 60, s % 60);
}
static void sdk_fmt_time(char *b, size_t n, uint32_t ms)
{
    unsigned s = ms / 1000;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
}
static void sdk_card_show(bool on);
static void sdk_check_done(void)
{
    if (!sdk_solved()) return;
    sdk.done = true;
    sdk_new_best = !sdk_best[sdk.diff] || sdk.ms < sdk_best[sdk.diff];
    if (sdk_new_best && !sdk.hints) sdk_best[sdk.diff] = sdk.ms;
    else sdk_new_best = false;             /* a time with hints is not a record */
    sdk_sel = -1;
    sdk_save();
    sfx_play(SFX_WIN);
    sdk_card_show(true);
}
static void sdk_push_undo(int i)
{
    if (sdk_nundo == SDK_UNDO) { memmove(sdk_undo, sdk_undo + 1, sizeof sdk_undo[0] * (SDK_UNDO - 1)); sdk_nundo--; }
    sdk_undo[sdk_nundo].cell = (int8_t)i;
    sdk_undo[sdk_nundo].val = sdk.val[i];
    sdk_undo[sdk_nundo].notes = sdk.notes[i];
    sdk_nundo++;
}
static void sdk_put(int d)
{
    if (sdk.done || sdk_sel < 0 || sdk.given[sdk_sel]) { sfx_play(SFX_TAP); return; }
    int i = sdk_sel;
    if (sdk_notes_mode)
    {
        if (sdk.val[i]) return;
        sdk_push_undo(i);
        sdk.notes[i] ^= (uint16_t)(1u << d);
        sfx_play(SFX_UI_TICK);
    }
    else
    {
        if (sdk.val[i] == d) return;
        sdk_push_undo(i);
        sdk.val[i] = (uint8_t)d;
        sdk.notes[i] = 0;
        if (d != sdk.sol[i]) { sdk.mistakes++; sfx_play(SFX_ERROR); }
        else
        {
            /* the number is settled: its note goes from the cells it sees */
            for (int k = 0; k < 81; k++) if (k != i && sdk_peer(k, i)) sdk.notes[k] &= (uint16_t)~(1u << d);
            sfx_play(SFX_SUDOKU_PLACE);
        }
    }
    sdk_show();
    sdk_check_done();
    if (sl_ms() - sdk_saved_at > 2000) sdk_save();
}
static void sdk_key_cb(lv_event_t *e) { sdk_put((int)(intptr_t)lv_event_get_user_data(e)); }
static void sdk_board_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(sdk_board, &a);
    int c = (p.x - a.x1 - SDK_PAD) / SDK_CELL, r = (p.y - a.y1 - SDK_PAD) / SDK_CELL;
    if (c < 0 || c > 8 || r < 0 || r > 8 || sdk.done) return;
    sdk_sel = r * 9 + c;
    sdk_show();
}
static void sdk_undo_cb(lv_event_t *e)
{
    (void)e;
    if (!sdk_nundo || sdk.done) return;
    sdk_nundo--;
    int i = sdk_undo[sdk_nundo].cell;
    sdk.val[i] = sdk_undo[sdk_nundo].val;
    sdk.notes[i] = sdk_undo[sdk_nundo].notes;
    sdk_sel = i;
    sfx_play(SFX_UI_BACK);
    sdk_show();
}
static void sdk_erase_cb(lv_event_t *e)
{
    (void)e;
    if (sdk_sel < 0 || sdk.given[sdk_sel] || sdk.done || (!sdk.val[sdk_sel] && !sdk.notes[sdk_sel])) return;
    sdk_push_undo(sdk_sel);
    sdk.val[sdk_sel] = 0;
    sdk.notes[sdk_sel] = 0;
    sfx_play(SFX_UI_REMOVE);
    sdk_show();
}
static void sdk_notes_cb(lv_event_t *e) { (void)e; sdk_notes_mode = !sdk_notes_mode; sfx_play(SFX_TAP); sdk_show(); }
/* the answer for the cell picked; with none picked (or it right already),
 * an empty one */
static void sdk_hint_cb(lv_event_t *e)
{
    (void)e;
    if (sdk.done) return;
    int i = sdk_sel;
    if (i < 0 || sdk.given[i] || sdk.val[i] == sdk.sol[i])
    {
        int empty[81], n = 0;
        for (int k = 0; k < 81; k++) if (sdk.val[k] != sdk.sol[k]) empty[n++] = k;
        if (!n) return;
        i = empty[sdk_rand() % n];
    }
    sdk_push_undo(i);
    sdk.val[i] = sdk.sol[i];
    sdk.notes[i] = 0;
    sdk.hints++;
    sdk_sel = i;
    for (int k = 0; k < 81; k++) if (k != i && sdk_peer(k, i)) sdk.notes[k] &= (uint16_t)~(1u << sdk.sol[i]);
    sfx_play(SFX_SUDOKU_PLACE);
    sdk_show();
    sdk_check_done();
}
static bool sdk_on_top(void) { return sdk_page_obj && npages && pages[npages - 1] == sdk_page_obj; }
static void sdk_tick(lv_timer_t *tm)
{
    (void)tm;
    uint32_t now = lv_tick_get();
    bool running = sdk_on_top() && !sdk.done && !sl_power_hides_ui() && !overlay_visible();
    if (running && sdk_tick_at) sdk.ms += now - sdk_tick_at;
    sdk_tick_at = running ? now : 0;
    sdk_clock_show();
}
static void sdk_new(int diff)
{
    sdk_make(diff);
    sdk_notes_mode = false;
    sdk_card_show(false);
    if (sdk_diff_l) lv_label_set_text(sdk_diff_l, sl_tr(sdk_diff_names[diff]));
    sdk_tick_at = 0;
    sdk_clock_show();
    sdk_show();
    sdk_save();
}
static void sdk_pick_diff(int k) { sfx_play(SFX_START); sdk_new(k); }
static void sdk_new_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[4] = { "Easy", "Medium", "Hard", "Expert" };
    sort_sheet(names, 4, sdk.diff, sdk_pick_diff);
    /* the sheet's head says what this one is for */
    lv_obj_t *hd = lv_obj_get_child(acts, 0);
    if (hd) lv_label_set_text(hd, sl_tr("New game"));
}

static void sdk_card_show(bool on)
{
    if (!sdk_card) return;
    if (!on) { lv_obj_add_flag(sdk_card, LV_OBJ_FLAG_HIDDEN); return; }
    char t[32], b[160];
    sdk_fmt_time(t, sizeof t, sdk.ms);
    lv_label_set_text(sdk_card_title, sl_tr(sdk_new_best ? "New best time!" : "Solved!"));
    lv_obj_set_style_text_color(sdk_card_title, lv_color_hex(sdk_new_best ? sl_accent_on(C_SHEET, 45) : C_TEXT), 0);
    if (sdk.hints) snprintf(b, sizeof b, sl_tr("%s in %s, %d hints"), sl_tr(sdk_diff_names[sdk.diff]), t, sdk.hints);
    else snprintf(b, sizeof b, sl_tr("%s in %s"), sl_tr(sdk_diff_names[sdk.diff]), t);
    lv_label_set_text(sdk_card_sub, b);
    lv_obj_remove_flag(sdk_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sdk_card);
    anim_prop(sdk_card, gm_opa_cb, 0, 255, SL_T_PAGE, NULL);
}
static void sdk_again_cb(lv_event_t *e) { (void)e; sdk_new(sdk.diff); }

/* ---- the page ---- */
static void sdk_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sdk_page_obj) return;
    if (sdk_timer) { lv_timer_delete(sdk_timer); sdk_timer = NULL; }
    sdk_save();
    sdk_page_obj = sdk_board = sdk_clock = sdk_diff_l = sdk_card = NULL;
}
/* a tool under a game's board: its symbol over its name (Sudoku, 24 Points) */
static lv_obj_t *gm_tool(lv_obj_t *p, int x, int y, int w, const char *sym, const char *name, lv_event_cb_t cb)
{
    lv_obj_t *b = box(p);
    lv_obj_set_size(b, w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, 64, 0);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_pad_ver(b, SL_SP_XS, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_MD, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_hex(sl_zune() ? ZN_PINK : C_ACCENT_UI), lv_color_hex(C_BG), 70), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    text(b, sym, &sl_sym_22, C_TEXT);
    lv_obj_t *l = text(b, name, &ss_regular_16, C_SUB);
    lv_obj_set_width(l, w - 4);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    on_click(b, cb, NULL);
    return b;
}
static lv_obj_t *sdk_tool(lv_obj_t *p, int k, const char *sym, const char *name, lv_event_cb_t cb)
{
    int w = SDK_BOARD / 4;
    return gm_tool(p, SDK_X + (sl_ui_rtl() ? 3 - k : k) * w + SL_SP_XS, SDK_TOOLS_Y, w - SL_SP_S, sym, name, cb);
}
/* a game's card over its board: a title, a line, then its buttons */
static lv_obj_t *gm_card(lv_obj_t *p, int w, int y, lv_obj_t **title, lv_obj_t **sub)
{
    lv_obj_t *c = box(p);
    lv_obj_set_width(c, w);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, SL_SP_S, 0);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_SHEET), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    sl_elevate(c, SL_E3, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    *title = text(c, "", &ss_bold_28, C_TEXT);
    lv_obj_set_width(*title, lv_pct(100));
    lv_label_set_long_mode(*title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(*title, LV_TEXT_ALIGN_CENTER, 0);
    *sub = text(c, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(*sub, lv_pct(100));
    lv_label_set_long_mode(*sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(*sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}
static void sudoku_page(void)
{
    if (page_bring_back(sdk_page_obj)) return;
    sdk_load();
    lv_obj_t *p, *body = page_new("Sudoku", &p);
    sdk_page_obj = p;
    lv_obj_add_event_cb(p, sdk_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    if (!sdk.live || sdk.done) sdk_make(sdk.live ? sdk.diff : 0);
    /* the difficulty (a tap starts a new game), and the clock */
    lv_obj_t *d = box(p);
    lv_obj_set_size(d, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(d, SL_GUTTER, 0);
    lv_obj_set_style_radius(d, sl_zune() ? 0 : SL_R_PILL, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    hairline(d);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(d, SL_SP_S, 0);
    sdk_diff_l = text(d, sdk_diff_names[sdk.diff], &ss_semibold_18, C_TEXT);
    text(d, LV_SYMBOL_DOWN, &sl_sym_14, C_SUB);
    sl_align(d, LV_ALIGN_TOP_LEFT, SDK_X, SDK_TOP + SL_SP_S);
    lv_obj_set_ext_click_area(d, 4);
    on_click(d, sdk_new_cb, NULL);
    sdk_clock = text(p, "0:00", &ss_semibold_23, C_TEXT);
    sl_align(sdk_clock, LV_ALIGN_TOP_RIGHT, -SDK_X, SDK_TOP + SL_SP_S + 6);

    sdk_board = box(p);
    lv_obj_set_size(sdk_board, SDK_BOARD, SDK_BOARD);
    lv_obj_set_pos(sdk_board, SDK_X, SDK_Y);
    lv_obj_set_style_radius(sdk_board, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(sdk_board, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(sdk_board, LV_OPA_COVER, 0);
    sl_elevate(sdk_board, SL_E1, 0);
    lv_obj_add_flag(sdk_board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_add_event_cb(sdk_board, sdk_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(sdk_board, sdk_board_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *t0 = sdk_tool(p, 0, LV_SYMBOL_LEFT, "Undo", sdk_undo_cb);
    lv_obj_t *t1 = sdk_tool(p, 1, LV_SYMBOL_BACKSPACE, "Erase", sdk_erase_cb);
    sdk_notes_b = sdk_tool(p, 2, LV_SYMBOL_EDIT, "Notes", sdk_notes_cb);
    lv_obj_t *h = sdk_tool(p, 3, LV_SYMBOL_EYE_OPEN, "Hint", sdk_hint_cb);
    /* hints used, a small badge on its corner once there are any */
    lv_obj_t *badge = box(h);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_min_width(badge, 22, 0);
    lv_obj_set_style_pad_hor(badge, 6, 0);
    lv_obj_set_style_radius(badge, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(sl_zune() ? ZN_PINK : C_ACCENT_UI), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 20, 0);
    sdk_hint_n = text(badge, "0", &sl_sym_14, sl_ink_on(sl_zune() ? ZN_PINK : C_ACCENT_UI));
    lv_obj_center(sdk_hint_n);

    /* the nine numbers, under the tools however tall their words made them */
    lv_obj_update_layout(p);
    int keys_y = SDK_KEYS_Y;
    lv_obj_t *tools[4] = { t0, t1, sdk_notes_b, h };
    for (int k = 0; k < 4; k++)
    {
        lv_area_t ta;
        lv_obj_get_coords(tools[k], &ta);
        keys_y = LV_MAX(keys_y, ta.y2 + 1 + SL_SP_S);
    }
    int kh = LV_MIN(72, H - SL_SP_S - keys_y);
    int kw = 46, gap = (SDK_BOARD - 9 * kw) / 8;
    for (int k = 0; k < 9; k++)
    {
        lv_obj_t *b = box(p);
        lv_obj_set_size(b, kw, kh);
        lv_obj_set_pos(b, SDK_X + k * (kw + gap) + (SDK_BOARD - 9 * kw - 8 * gap) / 2, keys_y);
        lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_MD, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        hairline(b);
        press_motion(b);
        char s[2] = { (char)('1' + k), 0 };
        lv_obj_center(text(b, s, &ss_semibold_30, sl_accent_on(C_CARD, 45)));
        on_click(b, sdk_key_cb, (void *)(intptr_t)(k + 1));
        sdk_keys[k] = b;
    }

    sdk_card = gm_card(p, 340, SDK_Y + SDK_BOARD / 2 - 100, &sdk_card_title, &sdk_card_sub);
    mn_pill(sdk_card, "Play again", 280, SL_PILL_H, true, sdk_again_cb, NULL);
    mn_pill(sdk_card, "Another difficulty", 280, SL_PILL_H, false, sdk_new_cb, NULL);

    sdk_timer = lv_timer_create(sdk_tick, 250, NULL);
    sdk_tick_at = 0;
    sdk_clock_show();
    sdk_show();
    page_push(p);
}
