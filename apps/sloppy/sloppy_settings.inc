/* Settings: its home page, the words every Settings page is built from, and
 * the pages for sound, buttons, the control centre, time and power, and
 * lyrics colour. Most of these drive Rockbox's own settings; the rest are
 * GabagoolOS settings in settings_list.c. Included from sloppy.c. */

/* ------------------------------------------------------------ vocabulary */
/* Every Settings page is built from these and nothing else, so one page
 * reads like the next:
 *   set_group    small capitals over a rounded card that holds rows
 *   set_row      icon, name, the value now and a chevron: opens a page
 *   set_link     the same for a page opened by a callback (set_link_sub:
 *                with a line under the name); in Tiles mode all three are tiles
 *   set_action   the same row, doing something of its own when tapped
 *   set_check    one choice of several, ticked when it is the one
 *   set_danger   a row that resets or forgets, in red, no chevron
 *   opt_switch   a name, a line on it if it needs one, and a switch
 *   opt_slider   a name, its value and a slider under them
 *   opt_label    a name over the choices of opt_chips
 *   set_text     words inside a card: a readout, an explanation
 *   opt_note     words under a card, on what the rows above it do
 * One row height, one inset, one icon size, one hairline between rows,
 * one press. Zule keeps its own look: capitals on black, no panels. */
/* Big icons (Customization > Settings layout): bigger icons, and rows and
 * tiles grown to keep the same air around them. Zule keeps its own look:
 * capitals with no icons beside them, so nothing to grow. */
static bool set_big(void) { return global_settings.sloppy_big_icons == 1 && !sl_zune(); }
#define SET_ICON    (set_big() ? 48 : 36)    /* the icon square: lit_square rounds it to SL_R_SM */
#define SET_PAD_V   (SL_GUTTER + SL_SP_XS)   /* 18: a row's air above and below its words */
#define SET_ROW_H   (SET_ICON + 2 * SET_PAD_V)   /* 72 (84 big): a row at Standard, taller when its words are */
#define SET_INNER   (W - 2 * SL_GUTTER - 2)  /* a card's width inside its hairline */
#define SET_TEXT_X  (SL_INSET + SET_ICON + SL_GUTTER)   /* a row's words, after its icon */
#define SET_SWITCH_W 64                      /* style_switch's track */
#define SET_RED     (sl_theme() == 1 ? 0xc8282e : 0xff6b6e)   /* destructive words: 4.5:1 on every card */

typedef void (*opt_apply_fn)(int value);

/* Hebrew and Arabic read from the right: a row is laid out mirrored */
static bool set_rtl(void) { return global_settings.sloppy_language == 4 || global_settings.sloppy_language == 5; }
/* x is from the leading edge of the card's inside */
static void set_place(lv_obj_t *o, int x, int y, int w)
{
    /* aligned, not set: in a right-to-left box a plain position is mirrored again */
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, set_rtl() ? SET_INNER - x - w : x, y);
}
static void set_align(lv_obj_t *l)
{
    lv_obj_set_style_text_align(l, set_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
}
/* the height of s set in f at width w, wrapped */
static int set_text_h(const char *s, const lv_font_t *f, int w)
{
    lv_point_t z;
    lv_text_get_size(&z, s, f, 0, 0, w, LV_TEXT_FLAG_NONE);
    return z.y + (sl_has_arabic(s) ? sl_line_h(f) - lv_font_get_line_height(f) : 0);
}
static int set_text_w(const char *s, const lv_font_t *f)
{
    return lv_text_get_width(s, strlen(s), f, 0);
}
/* Capitals for a header, Latin-1 letters too (é to É), so "Réglages"
 * never reads "RéGLAGES": sl_upper does both now */
static const char *set_caps(const char *s, char *buf, size_t n) { return sl_upper(s, buf, n); }
/* A hairline between two rows, from the words on, as the cards' edge. A
 * group remembers (bit 0 of its user data) when the next row belongs to
 * the one before it (a name over its chips) and goes without. */
static void set_sep(lv_obj_t *g, lv_obj_t *r, int x)
{
    intptr_t st = (intptr_t)lv_obj_get_user_data(g);
    lv_obj_set_user_data(g, (void *)(st & ~(intptr_t)1));
    if (sl_zune() || lv_obj_get_child_count(g) < 2 || (st & 1)) return;
    lv_obj_t *rule = box(r);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(rule, SET_INNER - x, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, set_rtl() ? 0 : x, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_bg_opa(rule, 40, 0);
}
static void set_join_next(lv_obj_t *g)
{
    lv_obj_set_user_data(g, (void *)((intptr_t)lv_obj_get_user_data(g) | 1));
}
/* every row: the full width of its card, pressed in the card's pressed
 * colour (the card clips the corners of the first and last) */
static lv_obj_t *set_row_box(lv_obj_t *g, int h)
{
    lv_obj_t *r = box(g);
    lv_obj_set_size(r, lv_pct(100), h);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 0, 0);
    return r;
}

/* The small capitals over a group, in line with the words in its card. */
static lv_obj_t *set_header(lv_obj_t *body, const char *label)
{
    char up[128];
    const char *s = set_caps(sl_tr(label), up, sizeof up);
    lv_obj_t *l = text(body, s, &ss_semibold_18, C_DIM);
    lv_obj_set_style_text_letter_space(l, sl_has_arabic(s) ? 0 : 1, 0);   /* Arabic letters join */
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    lv_obj_set_style_pad_top(l, SL_GUTTER + SL_SP_XS, 0);
    lv_obj_set_style_pad_bottom(l, SL_SP_S, 0);
    lv_obj_set_style_pad_hor(l, sl_zune() ? SL_SP_XS : SL_INSET + 1, 0);   /* + the card's hairline */
    sl_label_deep(l);
    return l;
}
/* A header (when given) and the rounded card its rows go in. */
static lv_obj_t *set_group(lv_obj_t *body, const char *label)
{
    if (label) set_header(body, label);
    lv_obj_t *g = box(body);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(g, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    sl_elevate(g, SL_E1, 0);
    lv_obj_set_style_clip_corner(g, true, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    if (!label) lv_obj_set_style_margin_top(g, SL_SP_S, 0);
    return g;
}
/* Words under a card, on what its rows do. */
static void opt_note(lv_obj_t *body, const char *s)
{
    lv_obj_t *n = text(body, s, &ss_regular_16, C_SUB);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    set_align(n);
    lv_obj_set_style_pad_top(n, SL_SP_S, 0);
    lv_obj_set_style_pad_hor(n, sl_zune() ? SL_SP_XS : SL_INSET + 1, 0);
    sl_label_deep(n);
    /* under an empty state: centred with it, not hanging off its edge */
    int32_t k = lv_obj_get_index(n);
    if (k > 0 && sl_empty_last && lv_obj_get_child(body, k - 1) == sl_empty_last)
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
}
/* A note on centred content above it (a pairing code): centred too */
static void opt_note_centred(lv_obj_t *body, const char *s)
{
    opt_note(body, s);
    lv_obj_set_style_text_align(lv_obj_get_child(body, -1), LV_TEXT_ALIGN_CENTER, 0);
}
/* A group, and words under it on what its rows do. */
static lv_obj_t *opt_card(lv_obj_t *body, const char *title, const char *note)
{
    lv_obj_t *c = set_group(body, title);
    if (note) opt_note(body, note);
    return c;
}
/* Words inside a card: a readout or an explanation. Returns the label. */
static lv_obj_t *set_text(lv_obj_t *g, const char *s, uint32_t color)
{
    int w = SET_INNER - 2 * SL_INSET;
    lv_obj_t *r = box(g);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, SL_INSET, 0);
    lv_obj_set_style_pad_ver(r, SET_PAD_V, 0);
    lv_obj_t *l = text(r, s, &ss_regular_18, color);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    sl_label_deep(l);
    uint32_t kids = lv_obj_get_child_count(r);
    set_sep(g, r, SL_INSET);
    /* the hairline on the card's other rules, as opt_chips's: the row's
     * padding had moved it 20 in and 18 down, onto the words */
    if (lv_obj_get_child_count(r) > kids)
        lv_obj_align(lv_obj_get_child(r, -1), LV_ALIGN_TOP_LEFT, (set_rtl() ? 0 : SL_INSET) - SL_INSET, -SET_PAD_V);
    return l;
}

/* A rounded tile holding a symbol, lit from above. */
static lv_obj_t *set_icon(lv_obj_t *parent, const char *sym, uint32_t tint, int size)
{
    lv_obj_t *ic = lit_square(parent, tint, size);
    /* the symbol about half the square: 18 in 36, 24 in 48, 28 in 56, 32 in 72 */
    lv_obj_center(text(ic, sym, size > 60 ? &sl_sym_32 : size > 50 ? &sl_sym_28
                                : size > 44 ? &sl_sym_24 : size > 40 ? &sl_sym_22
                                : &sl_sym_18, 0xfefefe));
    return ic;
}
static int set_chevron_w(void)
{
    static int w;
    if (!w) w = set_text_w(LV_SYMBOL_RIGHT, &sl_sym_16);
    return w;
}
/* The row that opens something. Children in order: name, value (when
 * given), icon, chevron; screen_sleep_row_track() finds the value second.
 * Zule: the name in capitals at the edge, the value under it, nothing else. */
static lv_obj_t *set_action(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                            lv_event_cb_t cb, void *ud)
{
    if (sl_zune())
    {
        char up[96];
        int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = t2 ? lv_font_get_line_height(&ss_regular_16) : 0;
        int rh = LV_MAX(t2 ? SET_ROW_H + 2 * SL_SP_S : SET_ROW_H, lh1 + lh2 + 2 * SET_PAD_V), ty = (rh - lh1 - lh2) / 2;
        lv_obj_t *r = set_row_box(g, rh);
        lv_obj_t *a = text(r, sl_upper(sl_tr(t1), up, sizeof up), &ss_semibold_23, C_TEXT);
        lv_obj_set_pos(a, 6, ty);
        lv_obj_set_size(a, SET_INNER - 12, lh1);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        if (t2)
        {
            lv_obj_t *b = text(r, t2, &ss_regular_16, C_SUB);
            lv_obj_set_pos(b, 6, ty + lh1);
            lv_obj_set_size(b, SET_INNER - 12, lh2);
            lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        }
        if (cb) on_click(r, cb, ud);
        return r;
    }
    const char *s2 = t2 ? sl_tr(t2) : NULL;
    int tx = sym ? SET_TEXT_X : SL_INSET;
    int end = SL_INSET + set_chevron_w() + SL_SP_S;         /* what the chevron takes from the far edge */
    int room = SET_INNER - tx - end;
    int vw = s2 && s2[0] ? LV_MIN(set_text_w(s2, &ss_regular_18), room * 45 / 100) : 0;
    int lw = room - (vw ? vw + SL_GUTTER : 0);
    int lh = lv_font_get_line_height(&ss_semibold_21), vh = lv_font_get_line_height(&ss_regular_18);
    int h = LV_MAX(SET_ROW_H, sl_line_h(&ss_semibold_21) + 2 * SET_PAD_V);
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, t1, &ss_semibold_21, C_TEXT);
    lv_obj_set_size(a, lw, lh);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, (h - lh) / 2, lw);
    if (t2)
    {
        lv_obj_t *b = text(r, t2, &ss_regular_18, C_SUB);
        lv_obj_set_size(b, vw, vh);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(b, set_rtl() ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
        sl_label_deep(b);
        set_place(b, SET_INNER - end - vw, (h - vh) / 2, vw);
    }
    if (sym) set_place(set_icon(r, sym, tint, SET_ICON), SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    lv_obj_t *ch = text(r, set_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    lv_obj_align(ch, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    set_sep(g, r, tx);
    if (cb) on_click(r, cb, ud);
    return r;
}
/* Tiles mode reaches past the hubs: on every page under Settings a row
 * that only opens another page (set_row, set_link, set_link_sub) is a
 * tile, in the grid, sizes and corner value of Settings' own tiles, over
 * its group's card; the rows that set something stay in the card under
 * the tiles, full width. Zule never has tiles. */
static bool set_tiles_on(void) { return global_settings.sloppy_settings_tiles == 1 && !sl_zune(); }
#define SET_TILE_ONLY 0x1000000u   /* or'd into a tint: the icon is the tile's, the row goes without one */
static lv_obj_t *set_link_tile(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                               lv_event_cb_t cb, void *ud);
/* A way on to another page: set_action's row, or a tile in Tiles mode.
 * Children either way: name, value, then the rest. */
static lv_obj_t *set_link(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                          lv_event_cb_t cb, void *ud)
{
    if (set_tiles_on()) return set_link_tile(g, sym, tint, t1, t2, cb, ud);
    return set_action(g, tint & SET_TILE_ONLY ? NULL : sym, tint & 0xffffff, t1, t2, cb, ud);
}
/* The same with a line under the row's name (set_action_sub); a tile has
 * no room for a line, so it shows the value in its corner (NULL: none). */
static lv_obj_t *set_link_sub(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *line,
                              const char *value, lv_event_cb_t cb, void *ud)
{
    if (set_tiles_on()) return set_link_tile(g, sym, tint, t1, value, cb, ud);
    return set_action_sub(g, tint & SET_TILE_ONLY ? NULL : sym, tint & 0xffffff, t1, line, cb, ud);
}
static lv_obj_t *set_row(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2, intptr_t ud)
{
    return set_link(g, sym, tint, t1, t2, card_row_cb, (void *)ud);
}
/* The row that opens something, with a line under its name where a value
 * at the end would not fit (a device, a network, what a row does).
 * Children: name, line, icon (when given), chevron. */
static lv_obj_t *set_action_sub(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                                lv_event_cb_t cb, void *ud)
{
    if (sl_zune()) return set_action(g, sym, tint, t1, t2, cb, ud);
    int tx = sym ? SET_TEXT_X : SL_INSET;
    int lw = SET_INNER - tx - SL_INSET - set_chevron_w() - SL_SP_S;
    int lh1 = lv_font_get_line_height(&ss_semibold_21), lh2 = lv_font_get_line_height(&ss_regular_16);
    int deep = sl_line_h(&ss_regular_16) - lh2;
    int h = LV_MAX(SET_ROW_H + SL_GUTTER, lh1 + lh2 + deep + 2 * SET_PAD_V), y = (h - lh1 - lh2 - deep) / 2;
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, t1, &ss_semibold_21, C_TEXT);
    lv_obj_set_size(a, lw, lh1);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, y, lw);
    lv_obj_t *b = text(r, t2 ? t2 : "", &ss_regular_16, C_SUB);
    lv_obj_set_size(b, lw, lh2);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    set_align(b);
    sl_label_deep(b);
    set_place(b, tx, y + lh1, lw);
    if (sym) set_place(set_icon(r, sym, tint, SET_ICON), SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    lv_obj_t *ch = text(r, set_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    lv_obj_align(ch, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    set_sep(g, r, tx);
    if (cb) on_click(r, cb, ud);
    return r;
}
/* A row's own small round button at its far end in place of the chevron
 * (child chevron): forget a device or a network. Its last child. */
static lv_obj_t *set_row_button(lv_obj_t *r, int chevron, const char *sym, lv_event_cb_t cb, void *ud)
{
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, chevron), LV_OBJ_FLAG_HIDDEN);   /* Zule draws none */
    /* the name and its line end before the button, which is wider than the chevron */
    int d = SL_TAP - 2 + SL_GUTTER - SL_INSET - set_chevron_w();   /* and SL_SP_S clear of it */
    for (int i = 0; i < 2 && !sl_zune(); i++)
    {
        lv_obj_t *l = lv_obj_get_child(r, i);
        lv_obj_set_width(l, lv_obj_get_style_width(l, 0) - d);
        if (set_rtl()) lv_obj_align(l, LV_ALIGN_TOP_LEFT, lv_obj_get_style_x(l, 0) + d, lv_obj_get_style_y(l, 0));
    }
    /* Zule: its capitals run the row's width, and end before the button too */
    for (int i = 0; i < 2 && sl_zune() && i < (int)lv_obj_get_child_count(r); i++)
    {
        lv_obj_t *l = lv_obj_get_child(r, i);
        int w = SET_INNER - SL_GUTTER - (SL_TAP - 2) - SL_SP_S - 6;
        lv_obj_set_width(l, w);
        if (set_rtl()) lv_obj_set_x(l, SET_INNER - 6 - w);
    }
    lv_obj_t *x = box(r);
    lv_obj_set_size(x, SL_TAP - 2, SL_TAP - 2);
    lv_obj_align(x, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_GUTTER : -SL_GUTTER, 0);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_mix(lv_color_hex(C_TEXT), lv_color_hex(C_CARD_P), 40), LV_STATE_PRESSED);
    press_motion(x);
    lv_obj_center(text(x, sym, &sl_sym_18, C_SUB));
    on_click(x, cb, ud);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_EVENT_BUBBLE);
    return x;
}
/* A one-line text field as the first row of a group (a password, a
 * token): the card is the field, the accent cursor shows where typing goes. */
static lv_obj_t *set_field(lv_obj_t *g)
{
    lv_obj_t *ta = lv_textarea_create(g);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, &ss_semibold_23, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_border_width(ta, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_pad_hor(ta, SL_INSET, 0);
    lv_obj_set_style_pad_ver(ta, SL_GUTTER, 0);   /* a field keeps its 14: three of them clear the keyboard at Largest */
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT_UI), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    return ta;
}
/* A page that asks before something big: what will happen in a card
 * under its header, then the way back first and the way on below it (in
 * red when it cannot be undone). */
static void set_confirm(lv_obj_t *body, const char *header, const char *what, const char *keep, const char *keep_sub,
                        lv_event_cb_t keep_cb, const char *go, const char *go_sub, lv_event_cb_t go_cb, bool danger)
{
    lv_obj_t *g = set_group(body, header);
    set_text(g, what, C_TEXT);
    g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action_sub(g, LV_SYMBOL_LEFT, 0x66738a, keep, keep_sub, keep_cb, NULL);
    lv_obj_t *r = set_action_sub(g, danger ? LV_SYMBOL_TRASH : LV_SYMBOL_RIGHT, danger ? 0xe5484d : 0x2d7ff0, go, go_sub,
                                 go_cb, NULL);
    if (danger) lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(SET_RED), 0);
}
/* A row that only says something: no chevron, no press. */
static lv_obj_t *set_info(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2, bool sub)
{
    lv_obj_t *r = sub ? set_action_sub(g, sym, tint, t1, t2, NULL, NULL) : set_action(g, sym, tint, t1, t2, NULL, NULL);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, (t2 || sub ? 2 : 1) + (sym ? 1 : 0)), LV_OBJ_FLAG_HIDDEN);   /* the chevron */
    return r;
}
/* A row that resets or forgets: red words, its icon, no chevron. */
static lv_obj_t *set_danger(lv_obj_t *g, const char *sym, const char *t1, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = set_action(g, sym, 0xe5484d, t1, NULL, cb, ud);
    lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(sl_zune() ? 0xff6b6e : SET_RED), 0);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, sym ? 2 : 1), LV_OBJ_FLAG_HIDDEN);   /* the chevron */
    return r;
}
/* One choice of several. Children: name, tick, then the icon when there
 * is one. set_check_mark() moves the tick. */
static lv_obj_t *set_check(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, bool on,
                           lv_event_cb_t cb, void *ud)
{
    int tx = sym ? SET_TEXT_X : SL_INSET, tick = set_text_w(LV_SYMBOL_OK, &sl_sym_18);
    int lw = SET_INNER - tx - SL_INSET - tick - SL_SP_S;
    const lv_font_t *f = &ss_semibold_21;
    int lh = set_text_h(sl_tr(t1), f, lw), h = LV_MAX(SET_ROW_H, lh + 2 * SET_PAD_V);
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, t1, f, C_TEXT);
    lv_obj_set_width(a, lw);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, (h - lh) / 2, lw);
    lv_obj_t *t = text(r, LV_SYMBOL_OK, &sl_sym_18, C_ACCENT_TXT);
    lv_obj_align(t, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    lv_obj_update_flag(t, LV_OBJ_FLAG_HIDDEN, !on);
    if (sym) set_place(set_icon(r, sym, tint, SET_ICON), SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    set_sep(g, r, tx);
    if (cb) on_click(r, cb, ud);
    return r;
}
/* One choice of several that is easier seen than named (a theme, a
 * wallpaper, a layout): set_check with a picture where the icon goes and a
 * line under the name. Children: name, tick, picture, line (when given);
 * the caller draws in the picture, SET_PIC_W by SET_PIC_H. */
static lv_obj_t *set_pick(lv_obj_t *g, const char *t1, const char *t2, bool on, lv_event_cb_t cb, void *ud)
{
    int tx = SL_INSET + SET_PIC_W + SL_GUTTER, tick = set_text_w(LV_SYMBOL_OK, &sl_sym_18);
    int lw = SET_INNER - tx - SL_INSET - tick - SL_SP_S;
    const char *s2 = t2 ? sl_tr(t2) : NULL;
    int h1 = set_text_h(sl_tr(t1), &ss_semibold_21, lw), h2 = s2 ? set_text_h(s2, &ss_regular_16, lw) : 0;
    int words = h1 + (s2 ? SL_SP_XS + h2 : 0);
    int h = LV_MAX(SET_PIC_H + 2 * SET_PAD_V, words + 2 * SET_PAD_V), y = (h - words) / 2;
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, t1, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(a, lw);
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, y, lw);
    lv_obj_t *t = text(r, LV_SYMBOL_OK, &sl_sym_18, C_ACCENT_TXT);
    lv_obj_align(t, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    lv_obj_update_flag(t, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_t *pic = box(r);
    lv_obj_set_size(pic, SET_PIC_W, SET_PIC_H);
    set_place(pic, SL_INSET, (h - SET_PIC_H) / 2, SET_PIC_W);
    if (s2)
    {
        lv_obj_t *n = text(r, t2, &ss_regular_16, C_SUB);
        lv_obj_set_width(n, lw);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        set_align(n);
        sl_label_deep(n);
        set_place(n, tx, y + h1 + SL_SP_XS, lw);
    }
    set_sep(g, r, tx);
    if (cb) on_click(r, cb, ud);
    return r;
}
/* the little screen most pictures are drawn on: the page's own ground,
 * rounded as an icon, its hairline */
static lv_obj_t *set_pic_screen(lv_obj_t *pic, int w, int h, uint32_t bg)
{
    lv_obj_t *s = box(pic);
    lv_obj_set_size(s, w, h);
    lv_obj_center(s);
    lv_obj_set_style_radius(s, SL_R_SM, 0);
    lv_obj_set_style_clip_corner(s, true, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(s, 40, 0);
    lv_obj_set_style_border_width(s, 1, 0);
    lv_obj_set_style_border_post(s, true, 0);   /* over what is drawn in it */
    return s;
}
static void set_check_mark(lv_obj_t *chosen)
{
    lv_obj_t *g = lv_obj_get_parent(chosen);
    for (uint32_t i = 0; i < lv_obj_get_child_count(g); i++)
    {
        lv_obj_t *r = lv_obj_get_child(g, i);
        if (lv_obj_get_child_count(r) > 1) lv_obj_update_flag(lv_obj_get_child(r, 1), LV_OBJ_FLAG_HIDDEN, r != chosen);
    }
}

static void opt_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target(e);
    opt_apply_fn apply = (opt_apply_fn)lv_event_get_user_data(e);
    if (!apply) return;                    /* the caller's own handler does the rest */
    apply(lv_obj_has_state(sw, LV_STATE_CHECKED));
    if (settings_save() != 0) toast_warn("Could not save settings");
}
static void opt_row_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_user_data(e);
    if (lv_obj_has_state(sw, LV_STATE_DISABLED)) return;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}
/* A name, a line on it when given, and a switch; a tap anywhere on the row
 * flips it (the switch is 5 mm wide on the R1's 311 ppi panel). Children:
 * name, line (when given), switch. Returns the switch. */
static lv_obj_t *set_switch_ex(lv_obj_t *g, const char *sym, uint32_t tint, const char *label, const char *note,
                               bool on, opt_apply_fn apply)
{
    int tx = sym && !sl_zune() ? SET_TEXT_X : SL_INSET;
    int w = SET_INNER - tx - SL_INSET - SET_SWITCH_W - SL_GUTTER;
    const char *s1 = sl_tr(label), *s2 = note ? sl_tr(note) : NULL;
    int h1 = set_text_h(s1, &ss_semibold_21, w), h2 = s2 ? set_text_h(s2, &ss_regular_16, w) : 0;
    int body = h1 + (s2 ? SL_SP_XS + h2 : 0), h = LV_MAX(SET_ROW_H, body + 2 * SET_PAD_V), y = (h - body) / 2;
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *l = text(r, label, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    sl_label_deep(l);
    set_place(l, tx, y, w);
    if (s2)
    {
        lv_obj_t *n = text(r, note, &ss_regular_16, C_SUB);
        lv_obj_set_width(n, w);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        set_align(n);
        sl_label_deep(n);
        set_place(n, tx, y + h1 + SL_SP_XS, w);
    }
    lv_obj_t *sw = lv_switch_create(r);
    style_switch(sw);
    set_place(sw, SET_INNER - SL_INSET - SET_SWITCH_W, (h - lv_obj_get_style_height(sw, 0)) / 2, SET_SWITCH_W);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, opt_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)apply);
    if (sym && !sl_zune()) set_place(set_icon(r, sym, tint, SET_ICON), SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    set_sep(g, r, tx);
    on_click(r, opt_row_toggle_cb, sw);
    return sw;
}
static lv_obj_t *set_switch(lv_obj_t *g, const char *label, const char *note, bool on, opt_apply_fn apply)
{
    return set_switch_ex(g, NULL, 0, label, note, on, apply);
}
static lv_obj_t *opt_switch(lv_obj_t *c, const char *label, bool on, opt_apply_fn apply)
{
    return set_switch(c, label, NULL, on, apply);
}

static void opt_chips_mark(lv_obj_t *row, int sel)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
    {
        lv_obj_t *chip = lv_obj_get_child(row, i);
        if (!lv_obj_has_flag(chip, LV_OBJ_FLAG_CLICKABLE)) continue;   /* the hairline */
        bool on = (int)i == sel;
        lv_obj_set_style_bg_color(chip, lv_color_hex(on ? C_ACCENT_UI : C_CARD_P), 0);
        lv_obj_set_style_bg_color(chip, lv_color_mix(lv_color_hex(C_TEXT), lv_color_hex(on ? C_ACCENT_UI : C_CARD_P), 40),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), lv_color_hex(on ? sl_ink_on(C_ACCENT_UI) : C_TEXT), 0);
    }
}
static void opt_chip_cb(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_current_target(e);
    lv_obj_t *row = lv_obj_get_parent(chip);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    opt_apply_fn apply = (opt_apply_fn)lv_obj_get_user_data(row);
    opt_chips_mark(row, i);
    apply(i);
    if (settings_save() != 0) toast_warn("Could not save settings");
}
/* A name over a row of choices (opt_chips next) */
static void opt_label(lv_obj_t *c, const char *s)
{
    int w = SET_INNER - 2 * SL_INSET, h1 = set_text_h(sl_tr(s), &ss_semibold_21, w);
    lv_obj_t *r = box(c);
    lv_obj_set_size(r, lv_pct(100), SET_PAD_V + h1);
    lv_obj_t *l = text(r, s, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    sl_label_deep(l);
    set_place(l, SL_INSET, SET_PAD_V, w);
    set_sep(c, r, SL_INSET);
    set_join_next(c);
}
/* A row of choices, one chosen, wrapping when they do not fit; apply gets
 * the chosen index. Children: the chips in order. */
static lv_obj_t *opt_chips(lv_obj_t *c, const char *const *names, int n, int sel, opt_apply_fn apply)
{
    bool joined = (intptr_t)lv_obj_get_user_data(c) & 1;
    lv_obj_t *row = box(c);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(row, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_pad_column(row, SL_SP_S, 0);
    lv_obj_set_style_pad_row(row, SL_SP_S, 0);
    lv_obj_set_style_pad_hor(row, SL_INSET, 0);
    lv_obj_set_style_pad_top(row, joined ? SL_SP_S + SL_SP_XS : SET_PAD_V, 0);
    lv_obj_set_style_pad_bottom(row, SET_PAD_V, 0);
    lv_obj_set_user_data(row, (void *)apply);
    int ch = LV_MAX(42, sl_line_h(&ss_semibold_18) + 2 * SL_SP_S);
    for (int i = 0; i < n; i++)
    {
        lv_obj_t *chip = box(row);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, ch);
        lv_obj_set_style_max_width(chip, SET_INNER - 2 * SL_INSET, 0);
        lv_obj_set_style_pad_hor(chip, SL_GUTTER + 2, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        press_motion(chip);
        lv_obj_t *l = text(chip, names[i], &ss_semibold_18, C_TEXT);
        lv_obj_center(l);
        sl_label_deep(l);
        on_click(chip, opt_chip_cb, (void *)(intptr_t)i);
        lv_obj_set_ext_click_area(chip, SL_SP_S / 2);   /* the gaps between chips count too */
    }
    opt_chips_mark(row, sel);
    uint32_t kids = lv_obj_get_child_count(row);
    set_sep(c, row, SL_INSET);
    /* the hairline lines up with the card's other rules: the row's padding
     * would have moved it in and down by its own inset */
    if (lv_obj_get_child_count(row) > kids)
        lv_obj_align(lv_obj_get_child(row, -1), LV_ALIGN_TOP_LEFT, (set_rtl() ? 0 : SL_INSET) - SL_INSET,
                     -lv_obj_get_style_pad_top(row, 0));
    return row;
}

typedef void (*opt_format_fn)(char *b, int n, int value);
struct opt_slider { opt_apply_fn apply; opt_format_fn format; };

static void opt_slider_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_current_target(e);
    const struct opt_slider *o = lv_event_get_user_data(e);
    int v = lv_slider_get_value(s);
    char b[24];
    o->format(b, sizeof b, v);
    label_if_changed(lv_obj_get_child(lv_obj_get_parent(s), 1), b);
    o->apply(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && settings_save() != 0)
        toast_warn("Could not save settings");
}
/* A name and its value over a slider. The track runs along the words'
 * column; the knob at either end stays inside the card. Children: name,
 * value, slider. Returns the row. */
#define SET_VALUE_W 132                      /* room the value keeps at the far end */
static lv_obj_t *set_slider_row(lv_obj_t *c, const char *label, const char *value, uint32_t value_color)
{
    int vw = SET_VALUE_W, lw = SET_INNER - 2 * SL_INSET - vw - SL_SP_S;
    int h1 = set_text_h(sl_tr(label), &ss_semibold_21, lw);
    int ty = SET_PAD_V + h1 + SL_GUTTER;     /* the knob's top */
    lv_obj_t *r = box(c);
    lv_obj_set_size(r, lv_pct(100), ty + SL_KNOB + SET_PAD_V);
    lv_obj_t *l = text(r, label, &ss_semibold_21, C_TEXT);
    lv_obj_set_width(l, lw);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    set_align(l);
    sl_label_deep(l);
    set_place(l, SL_INSET, SET_PAD_V, lw);
    int lh = lv_font_get_line_height(&ss_semibold_21), vh = lv_font_get_line_height(&ss_semibold_18);
    lv_obj_t *v = text(r, value, &ss_semibold_18, value_color);
    lv_obj_set_size(v, vw, vh);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, set_rtl() ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    set_place(v, SET_INNER - SL_INSET - vw, SET_PAD_V + lh - vh - (lh - vh) / 4, vw);   /* on the name's baseline */
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_size(s, SET_INNER - 2 * SL_INSET, SL_TRACK_H);
    lv_obj_set_pos(s, SL_INSET, ty + (SL_KNOB - SL_TRACK_H) / 2);
    style_slider(s);
    set_sep(c, r, SL_INSET);
    return r;
}
static lv_obj_t *opt_slider(lv_obj_t *c, const char *label, int min, int max, int value,
                            const struct opt_slider *o)
{
    char b[24];
    o->format(b, sizeof b, value);
    lv_obj_t *r = set_slider_row(c, label, b, C_ACCENT_TXT);
    lv_obj_t *s = lv_obj_get_child(r, 2);
    lv_slider_set_range(s, min, max);
    if (min < 0 && max > 0) lv_slider_set_mode(s, LV_SLIDER_MODE_SYMMETRICAL);   /* either way of 0: filled from 0 */
    lv_slider_set_value(s, value < min ? min : value > max ? max : value, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, opt_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)o);
    lv_obj_add_event_cb(s, opt_slider_cb, LV_EVENT_RELEASED, (void *)o);
    return r;
}

/* ---------------------------------------------------------- settings home */
/* One table: the list and the tiles are both drawn from it, so the two
 * layouts always hold the same things in the same order. The value is
 * what the thing is set to now, where that says something. */
static bool scr_any(void);
static lv_obj_t *brand_mark(lv_obj_t *parent, const lv_font_t *f, int drop);
static const char *set_v_bluetooth(void)
{
    if (sl_net_wireless_unsupported()) return "Not supported";
    if (!sl_bt_known) return NULL;
    if (sl_bt_state.routed && sl_bt_state.name[0]) return sl_bt_state.name;
    return sl_bt_state.enabled ? "On" : "Off";
}
static const char *set_v_scrobble(void) { return scr_any() ? "On" : "Off"; }
static const char *set_v_servers(void)
{
    static struct sl_sub_server s;
    static struct sl_abs_server a;
    return sl_sub_server(0, &s) ? s.name : sl_abs_server(&a) ? a.name : "Off";   /* the one in use, else Audiobookshelf's */
}
static const char *set_v_sounds(void)
{
    static char b[8];
    if (!global_settings.sloppy_sfx_volume) return "Off";
    snprintf(b, sizeof b, "%d%%", global_settings.sloppy_sfx_volume);
    return b;
}
static const char *set_v_theme(void)
{
    static const char *const themes[] = { "Dark", "Light", "True dark", "Zule" };
    int th = global_settings.sloppy_theme;
    return themes[th >= 0 && th < 4 ? th : 0];
}
static const char *set_v_notes(void) { return notes_on() ? "On" : "Off"; }
static const char *set_v_library(void)
{
    static char b[40];
    char n[16];
    snprintf(b, sizeof b, "%s %s", sl_fmt_count(n, sizeof n, sl_lib.n_tracks), sl_tr_count(sl_lib.n_tracks, "song", "songs"));
    return b;
}
static const char *set_v_update(void);
struct set_entry { const char *sym; uint32_t tint; const char *name; const char *(*value)(void); intptr_t act; };
struct set_section { const char *label; uint8_t first, n; };
static const struct set_entry set_entries[] = {
    { LV_SYMBOL_BLUETOOTH, 0x2d7ff0, "Bluetooth", set_v_bluetooth, ACT_BLUETOOTH },
    { LV_SYMBOL_WIFI, 0x0aa9c7, "Wi-Fi", wf_row_text, 0x20012 },
    { LV_SYMBOL_UPLOAD, 0xeb743b, "Scrobbling", set_v_scrobble, 0x2001b },
    { LV_SYMBOL_DRIVE, 0x2f9e8f, "Music servers", set_v_servers, 0x2002a },
    { LV_SYMBOL_PLAY, 0xe8408a, "Playback", NULL, 0x20002 },
    { LV_SYMBOL_VOLUME_MAX, 0xf0602f, "Sound", NULL, 0x2000d },
    { LV_SYMBOL_BELL, 0x8b6fd6, "Sound effects", set_v_sounds, 0x2000a },
    { LV_SYMBOL_IMAGE, 0xd93fa0, "Customization", set_v_theme, 0x20003 },
    { LV_SYMBOL_EYE_CLOSE, 0x3aa856, "Screen sleep", screen_sleep_row_text, 0x2000c },
    { LV_SYMBOL_KEYBOARD, 0xb8894f, "Buttons", NULL, 0x2000e },
    { LV_SYMBOL_BARS, 0x1fb39a, "Notification Center", set_v_notes, 0x2000f },
    { LV_SYMBOL_POWER, 0xe89a1c, "Time and power", NULL, 0x20010 },
    { LV_SYMBOL_AUDIO, 0x4f7fe0, "Library", set_v_library, ACT_LIBRARY },
    { LV_SYMBOL_REFRESH, 0x2f9e8f, "Software update", set_v_update, 0x2002b },
    { LV_SYMBOL_SETTINGS, 0x66738a, "Advanced", NULL, 0x20017 },
};
static const struct set_section set_sections[] = {
    { "CONNECTIONS", 0, 4 }, { "SOUND", 4, 3 }, { "DISPLAY AND CONTROLS", 7, 3 }, { "DEVICE", 10, 5 },
};
/* Customization and Library are hubs too, mostly ways on to other pages:
 * drawn from tables as Settings is, so the Tiles layout carries through */
static const char *set_v_home(void)
{
    static const char *const homes[] = { "Cards", "Tiles", "List" };
    int hs = global_settings.sloppy_home_style;
    return homes[hs >= 0 && hs < 3 ? hs : 0];
}
static const char *set_v_set_layout(void) { return global_settings.sloppy_settings_tiles == 1 ? "Tiles" : "List"; }
static const char *set_v_icon(void) { return global_settings.sloppy_skull_icon ? "Angular skull" : "Classic cog"; }
static const char *set_v_lock_player(void) { return global_settings.sloppy_lock_player ? "Small cover" : "Big cover"; }   /* the names on its page */
static const char *set_v_np(void) { return global_settings.sloppy_np_style == 3 ? "Waveform" : global_settings.sloppy_np_style == 2 ? "Centred" : global_settings.sloppy_np_style ? "Classic" : "Glass"; }
static const char *set_v_cf(void) { return global_settings.sloppy_cf_vertical ? "Vertical" : "Horizontal"; }
static const char *lang_name_now(void);
static const char *set_v_text_size(void) { return text_size_names[sl_text_step]; }
static const char *kb_v_now(void);
static const char *lyr_v_now(void);
static const char *set_v_saver(void);
static const struct set_entry set_custom_entries[] = {
    { LV_SYMBOL_EYE_OPEN, 0x6c63ff, "Theme", set_v_theme, 0x20004 },
    { LV_SYMBOL_TINT, 0xf0602f, "Accent color", NULL, 0x20005 },
    { LV_SYMBOL_HOME, 0x1fb39a, "Home layout", set_v_home, 0x20009 },
    { LV_SYMBOL_LIST, 0x4f7fe0, "Settings layout", set_v_set_layout, 0x2001d },
    { LV_SYMBOL_SETTINGS, 0x66738a, "Settings icon", set_v_icon, 0x20008 },
    { LV_SYMBOL_IMAGE, 0xd93fa0, "Home wallpaper", NULL, 0x20006 },
    { LV_SYMBOL_IMAGE, 0x2d7ff0, "Lock wallpaper", NULL, 0x20007 },
    { LV_SYMBOL_PLAY, 0xe8408a, "Lock screen player", set_v_lock_player, 0x2000b },
    { LV_SYMBOL_AUDIO, 0x2f8de4, "Now Playing", set_v_np, 0x20016 },
    { LV_SYMBOL_IMAGE, 0xb8743a, "Cover Flow", set_v_cf, 0x2001a },
    { LV_SYMBOL_EYE_CLOSE, 0x3aa856, "Screensaver", set_v_saver, 0x20028 },
    { SL_SYM_GLOBE, 0x0aa9c7, "Language", lang_name_now, 0x20013 },
    { LV_SYMBOL_EDIT, 0xe89a1c, "Text size", set_v_text_size, 0x20014 },
    { LV_SYMBOL_KEYBOARD, 0x4f7fe0, "Keyboards", kb_v_now, 0x20020 },
    { LV_SYMBOL_AUDIO, 0x8b6fd6, "Lyrics", lyr_v_now, 0x20011 },
};
static const struct set_section set_custom_sections[] = {
    { "APPEARANCE", 0, 5 }, { "WALLPAPERS", 5, 6 }, { "TEXT", 11, 4 },
};
static const char *set_v_count(int n)
{
    static char b[4][16];
    static int k;
    k = (k + 1) & 3;                     /* a few at once: one table is drawn at a time */
    return sl_fmt_count(b[k], sizeof b[k], n);
}
static const char *set_v_songs(void) { return set_v_count(sl_lib.n_tracks); }
static const char *set_v_albums(void) { return set_v_count(sl_lib.n_albums); }
static const char *set_v_artists(void) { return set_v_count(tg_n_artists()); }
static const char *set_v_album_artists(void) { return set_v_count(sl_lib.n_artists); }
static const char *lb_v_row(void);   /* sloppy_libjob.inc: where a lyrics or covers job is */
static const struct set_entry set_library_entries[] = {
    { LV_SYMBOL_AUDIO, 0x4f7fe0, "All songs", set_v_songs, ACT_ALL },
    { LV_SYMBOL_IMAGE, 0xe8408a, "Albums", set_v_albums, ACT_ALBUMS },
    { LV_SYMBOL_EYE_OPEN, 0x8b6fd6, "Artists", set_v_artists, ACT_ARTISTS },
    { LV_SYMBOL_EYE_OPEN, 0x1fb39a, "Album artists", set_v_album_artists, ACT_ALBUMARTISTS },
    { LV_SYMBOL_DIRECTORY, 0xe89a1c, "Files", NULL, ACT_FILES },
    { LV_SYMBOL_SETTINGS, 0x66738a, "Library options", NULL, 0x2001e },
    { LV_SYMBOL_DOWNLOAD, 0x8b6fd6, "Lyrics and covers", lb_v_row, 0x20025 },
    { LV_SYMBOL_REFRESH, 0x3aa856, "Update database", NULL, ACT_REBUILD },
};
static const struct set_section set_library_sections[] = { { "EXPLORE", 0, 5 }, { "OPTIONS", 5, 1 }, { "MAINTENANCE", 6, 2 } };

/* Settings as tiles: the Cards home's own cards (home_card), two across:
 * the icon at the leading top, the name under it and what it is set to
 * under the name, every tile in a grid as tall as the next. The first of
 * an odd group spans the row, so every group closes square: a wide tile
 * lies down, its icon beside its words. One across (all lying down) when
 * the words would not fit two. Children: name, value (when given), icon,
 * as set_row's, for screen_sleep_row_track(). */
#define SET_TILE_ICON (set_big() ? 72 : 56)   /* a tile's icon; Big icons: as the rows' grow, by about a third */
#define SET_TILE_PAD  SL_INSET               /* 20: the tile's edge to its icon and to its words, both ways */
#define SET_TILE_GAP  (SL_SP_S + SL_SP_XS)   /* 12: icon to name, as the Cards home's cards */
#define SET_TILE_WIDE_X (SET_TILE_PAD + SET_TILE_ICON + SL_GUTTER)   /* a wide tile's words, after its icon */
static int set_tile_across;
static bool set_words_fit(const char *s, const lv_font_t *f, int w)
{
    char word[64];
    while (*s)
    {
        int n = 0;
        while (*s == ' ') s++;
        while (s[n] && s[n] != ' ' && n < (int)sizeof word - 1) { word[n] = s[n]; n++; }
        word[n] = '\0';
        if (n && set_text_w(word, f) > w) return false;
        s += n;
    }
    return true;
}
static void set_tiles_measure(const struct set_entry *entries, int n)
{
    int w = HOME_CARD_W - 2 - 2 * SL_INSET;            /* a tile's words, inside its hairline */
    set_tile_across = 2;
    for (int i = 0; i < n; i++)
        if (!set_words_fit(sl_tr(entries[i].name), &ss_semibold_21, w)) { set_tile_across = 1; break; }
}
static lv_obj_t *set_tiles(lv_obj_t *body, const char *label)
{
    set_header(body, label);
    lv_obj_t *g = box(body);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(g, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, SL_GUTTER, 0);
    return g;
}
/* a tile's colour, a breath of it on a ground */
#define SET_TINT (sl_theme() == 1 ? 18 : 13)   /* of 255: a pastel in Light, a hint in the darks */
static lv_color_t set_tint_on(uint32_t tint, uint32_t ground, int k) { return lv_color_mix(lv_color_hex(tint), lv_color_hex(ground), k); }
/* A page a tile opens wears the tile's colour: its ground the same breath
 * of it the tile lays on its card, the square it came from opened out.
 * Its rows below are Settings' rows as in the List layout. */
static void set_wash(lv_obj_t *p, uint32_t tint)
{
    lv_obj_set_style_bg_color(p, set_tint_on(tint, C_BG, SET_TINT), 0);
}
/* Settings' own pages wear it; the music pages Library's tiles open keep their own ground */
static void set_tile_cb(lv_event_t *e)
{
    const struct set_entry *se = lv_event_get_user_data(e);
    if (se->act >= 0x20000 || se->act == ACT_BLUETOOTH || se->act == ACT_LIBRARY) page_wash_next = se->tint | 1u << 24;
    card_row_open(se->act);
    page_wash_next = 0;                  /* nothing was pushed: no later page takes it */
}
/* a tile's name whole: a dotted label's text has the dots in it, so the
 * name keeps a copy of its words for laying the tile out again */
static void set_tile_name_free(lv_event_t *e) { lv_free(lv_obj_get_user_data(lv_event_get_current_target(e))); }
static const char *set_tile_name(lv_obj_t *t) { return lv_obj_get_user_data(lv_obj_get_child(t, 0)); }
/* a tile's ground: its icon's colour, a breath of it laid flat over the
 * card (a gradient this faint steps in bands on the 16-bit panel); under
 * the finger the breath doubles, so the tile lights in its own colour */
static void set_tile_skin(lv_obj_t *t, uint32_t tint)
{
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_style_bg_color(t, set_tint_on(tint, C_CARD, SET_TINT), 0);
    lv_obj_set_style_bg_color(t, set_tint_on(tint, C_CARD, SET_TINT * 3), LV_STATE_PRESSED);
}
/* a tile w wide with room for lines of name: its children (name, the value
 * when there is one, icon) put in their places. Positions are inside the
 * hairline, so the words keep the group headers' inset. */
static void set_tile_place(lv_obj_t *t, int w, int lines)
{
    bool rtl = set_rtl(), wide = w > HOME_CARD_W;
    int lh = lv_font_get_line_height(&ss_semibold_21), deep = sl_line_h(&ss_semibold_21) - lh;
    int vh = lv_font_get_line_height(&ss_regular_16);
    int n = (int)lv_obj_get_child_count(t), iw = w - 2;
    lv_obj_t *a = lv_obj_get_child(t, 0), *b = n == 3 ? lv_obj_get_child(t, 1) : NULL, *ic = lv_obj_get_child(t, n - 1);
    bool shows = b && *lv_label_get_text(b);
    int tx = wide ? SET_TILE_WIDE_X : SET_TILE_PAD, tw = iw - tx - SL_INSET;
    int own = set_text_h(set_tile_name(t), &ss_semibold_21, tw) > lh * 3 / 2 ? lh * 2 : lh;
    int h, y;
    if (wide)
    {
        /* lying down: the icon, and the name over its value centred on it */
        h = 2 + 2 * SET_TILE_PAD + LV_MAX(SET_TILE_ICON, own + vh + deep);   /* alone on its row: its own lines */
        y = (h - 2 - own - (shows ? vh : 0)) / 2;
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, rtl ? iw - SET_TILE_PAD - SET_TILE_ICON : SET_TILE_PAD, (h - 2 - SET_TILE_ICON) / 2);
    }
    else
    {
        /* standing: the icon, the name under it, the value under the name;
         * room for a value on every tile, so every tile with a one-line
         * name is one height, with or without a value */
        h = 2 + 2 * SET_TILE_PAD + SET_TILE_ICON + SET_TILE_GAP + lh * lines + vh + deep;
        y = SET_TILE_PAD + SET_TILE_ICON + SET_TILE_GAP;
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, rtl ? iw - SET_TILE_PAD - SET_TILE_ICON : SET_TILE_PAD, SET_TILE_PAD);
    }
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_transform_pivot_x(t, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(t, h / 2, 0);
    lv_obj_set_size(a, tw, own);
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, rtl ? iw - tx - tw : tx, y);
    if (b)
    {
        lv_obj_set_size(b, tw, vh);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, rtl ? iw - tx - tw : tx, y + own);
    }
}
/* a tile's name and value, then its icon */
static void set_tile_words(lv_obj_t *t, const char *name, const char *value, const char *sym, uint32_t tint)
{
    lv_obj_t *a = text(t, name, &ss_semibold_21, C_TEXT);
    lv_obj_set_user_data(a, lv_strdup(lv_label_get_text(a)));
    lv_obj_add_event_cb(a, set_tile_name_free, LV_EVENT_DELETE, NULL);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    if (value)
    {
        lv_obj_t *b = text(t, value, &ss_regular_16, C_SUB);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        set_align(b);
        sl_label_deep(b);
    }
    set_icon(t, sym, tint, SET_TILE_ICON);
}
/* a grid's tiles in their places: across 2 or 1 (0: two unless a word
 * will not fit), the first full width when an odd count goes two across,
 * and each pair as tall as the one whose name takes more lines */
static void set_grid_lay(lv_obj_t *grid, int across)
{
    int n = (int)lv_obj_get_child_count(grid), lh = lv_font_get_line_height(&ss_semibold_21);
    int w = HOME_CARD_W - 2 - 2 * SL_INSET;
    for (int i = 0; i < n && !across; i++)
        if (!set_words_fit(set_tile_name(lv_obj_get_child(grid, i)), &ss_semibold_21, w)) across = 1;
    if (!across) across = 2;
    int odd = across == 2 && n % 2;
    for (int i = 0; i < n; i++)
    {
        if (across == 1 || (i == 0 && odd)) { set_tile_place(lv_obj_get_child(grid, i), W - 2 * SL_GUTTER, 1); continue; }
        int k = i + ((i - odd) % 2 ? -1 : 1), lines = 1;   /* k: the other of the pair */
        for (int j = 0; j < 2; j++)
        {
            int m = j ? k : i;
            if (m < n && set_text_h(set_tile_name(lv_obj_get_child(grid, m)), &ss_semibold_21, w) > lh * 3 / 2) lines = 2;
        }
        set_tile_place(lv_obj_get_child(grid, i), HOME_CARD_W, lines);
    }
}
static lv_obj_t *set_tile(lv_obj_t *g, const struct set_entry *e)
{
    lv_obj_t *t = home_card(g, HOME_CARD_W);
    set_tile_skin(t, e->tint);
    set_tile_words(t, e->name, e->value ? e->value() : NULL, e->sym, e->tint);
    on_click(t, set_tile_cb, (void *)e);
    return t;
}

/* ---- the tiles on the pages under Settings. A group's tiles go in a
 * grid over its card (the card keeps the rows that set something, and
 * hides while it has none); the card remembers its grid in its user data
 * beside set_join_next's bit. Each tile added lays its grid out again by
 * the hub's rules: two across unless a word will not fit, every tile as
 * tall as the one with the most lines, the first full width when the
 * count is odd. */
static const char set_grid_tag;
static bool set_is_tile(lv_obj_t *r) { return lv_obj_get_user_data(lv_obj_get_parent(r)) == (void *)&set_grid_tag; }
static lv_obj_t *set_grid_of(lv_obj_t *g) { return (lv_obj_t *)((intptr_t)lv_obj_get_user_data(g) & ~(intptr_t)1); }
static void set_grid_card_cb(lv_event_t *e) { lv_obj_remove_flag(lv_event_get_current_target(e), LV_OBJ_FLAG_HIDDEN); }
static lv_obj_t *set_link_grid(lv_obj_t *g)
{
    lv_obj_t *grid = set_grid_of(g);
    if (grid) return grid;
    grid = box(lv_obj_get_parent(g));
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(grid, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_pad_column(grid, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(grid, SL_GUTTER, 0);
    lv_obj_set_style_margin_top(grid, lv_obj_get_style_margin_top(g, 0), 0);   /* a group with no header: its breath */
    lv_obj_move_to_index(grid, lv_obj_get_index(g));    /* under the header, over the card */
    lv_obj_set_user_data(grid, (void *)&set_grid_tag);
    lv_obj_set_user_data(g, (void *)((intptr_t)grid | ((intptr_t)lv_obj_get_user_data(g) & 1)));
    lv_obj_set_style_margin_top(g, SL_GUTTER, 0);       /* a row's gap under the tiles */
    if (!lv_obj_get_child_count(g)) lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g, set_grid_card_cb, LV_EVENT_CHILD_CREATED, NULL);
    return grid;
}
static void set_grid_settle(lv_obj_t *grid) { set_grid_lay(grid, 0); }
/* the page a tile opens wears the tile's colour, as from Settings' own:
 * set for the push, cleared after it whether a page came or not */
static void set_link_from_cb(lv_event_t *e) { page_wash_next = (uint32_t)(uintptr_t)lv_event_get_user_data(e) | 1u << 24; }
static void set_link_wash_cb(lv_event_t *e) { (void)e; page_wash_next = 0; }
static lv_obj_t *set_link_tile(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                               lv_event_cb_t cb, void *ud)
{
    lv_obj_t *grid = set_link_grid(g);
    tint &= 0xffffff;
    lv_obj_t *t = home_card(grid, HOME_CARD_W);
    set_tile_skin(t, tint);
    set_tile_words(t, t1, t2 ? t2 : "", sym ? sym : LV_SYMBOL_RIGHT, tint);   /* a value always: set_link_text finds it */
    on_click(t, set_link_from_cb, (void *)(uintptr_t)tint);   /* after the drag guard: a drag sets nothing */
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, ud);
    lv_obj_add_event_cb(t, set_link_wash_cb, LV_EVENT_CLICKED, NULL);
    set_grid_settle(grid);
    return t;
}
/* a way on's line (a row) or value (a tile), when it changes */
static void set_link_text(lv_obj_t *r, const char *line, const char *value)
{
    const char *s = set_is_tile(r) ? value : line;
    lv_obj_t *l = lv_obj_get_child(r, 1);
    if (!strcmp(lv_label_get_text(l), s ? s : "")) return;
    bool was = *lv_label_get_text(l);
    lv_label_set_text(l, s ? s : "");
    if (set_is_tile(r) && was != (s && *s)) set_grid_settle(lv_obj_get_parent(r));   /* a wide tile centres its words again */
}
/* a group emptied to be filled again: its tiles go too */
static void set_group_clean(lv_obj_t *g)
{
    lv_obj_clean(g);
    if (set_grid_of(g)) lv_obj_clean(set_grid_of(g));
}

/* A hub as rows or as tiles, as Settings layout says (Zule: always rows) */
static void set_hub(lv_obj_t *body, const struct set_section *sec, int nsec, const struct set_entry *entries, int n)
{
    bool tiles = global_settings.sloppy_settings_tiles == 1 && !sl_zune();
    if (tiles) set_tiles_measure(entries, n);
    for (int s = 0; s < nsec; s++)
    {
        lv_obj_t *g = tiles ? set_tiles(body, sec[s].label) : set_group(body, sec[s].label);
        for (int i = 0; i < sec[s].n; i++)
        {
            const struct set_entry *e = &entries[sec[s].first + i];
            lv_obj_t *r = tiles ? set_tile(g, e)
                                : set_row(g, e->sym, e->tint, e->name, e->value ? e->value() : NULL, e->act);
            if (e->act == 0x2000c) screen_sleep_row_track(r);
        }
        if (tiles) set_grid_lay(g, set_tile_across);   /* every group of the hub two across, or all one */
    }
}

/* The top of Settings: whose player this is. The GabagoolOS mark on its
 * own dark, warm card (in every theme, as the Wi-Fi and Bluetooth heroes),
 * the player and the update under it, then the battery and the library in
 * three columns. Tapping it opens the library. */
#define SET_HERO_INK  0xfff1dc               /* the mark's cream */
#define SET_HERO_SUB  0xd6c6b2               /* 9:1 on the card */
/* "GABAGOOLOS · GAME ON THEN UPDATE": the name in capitals */
static const char *sl_update_footer(void)
{
    static char b[96];
    if (!b[0])
    {
        snprintf(b, sizeof b, "GABAGOOLOS \xc2\xb7 %s UPDATE", SL_UPDATE_NAME);
        for (char *c = b; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    return b;
}
static void set_hero(lv_obj_t *body)
{
    lv_obj_t *h = box(body);
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(h, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x231915), 0);   /* flat: a dark gradient bands on the 16-bit panel */
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x40291f), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(h, lv_color_hex(0xff6a1f), 0);
    lv_obj_set_style_border_opa(h, 40, 0);
    lv_obj_set_style_border_width(h, sl_zune() ? 0 : 1, 0);
    lv_obj_set_style_pad_top(h, SL_INSET - SL_SP_XS, 0);
    lv_obj_set_style_pad_bottom(h, SL_INSET, 0);
    lv_obj_set_style_pad_hor(h, SL_INSET, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    press_motion(h);
    press_sink(h, W - 2 * SL_GUTTER, 160);
    brand_mark(h, &sl_brand_34, 3);
    lv_obj_t *d = text(h, "HiBy R1 \xc2\xb7 " SL_UPDATE_NAME, &ss_regular_18, SET_HERO_SUB);
    lv_obj_set_style_text_color(d, lv_color_hex(SET_HERO_SUB), 0);
    lv_obj_set_width(d, lv_pct(100));
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(d, 1);
    /* three equal columns, the number over its word, so a big library
     * never pushes a word off the card */
    lv_obj_t *stats = box(h);
    lv_obj_set_size(stats, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(stats, SL_GUTTER, 0);
    lv_obj_set_style_pad_top(stats, SL_GUTTER, 0);
    lv_obj_set_style_border_side(stats, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(stats, 1, 0);
    lv_obj_set_style_border_color(stats, lv_color_hex(SET_HERO_INK), 0);
    lv_obj_set_style_border_opa(stats, 30, 0);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_base_dir(stats, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    char n[3][16];
    const char *what[3] = { "battery", sl_tr_count(sl_lib.n_tracks, "song", "songs"), sl_tr_count(sl_lib.n_albums, "album", "albums") };
    snprintf(n[0], sizeof n[0], "%d%%", battery_level() < 0 ? 0 : battery_level());
    sl_fmt_count(n[1], sizeof n[1], sl_lib.n_tracks);
    sl_fmt_count(n[2], sizeof n[2], sl_lib.n_albums);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *col = box(stats);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *num = text_light(col, n[i], &ss_semibold_23);
        lv_obj_set_style_text_color(num, lv_color_hex(SET_HERO_INK), 0);
        lv_obj_set_width(num, lv_pct(100));
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(num, 1);
        lv_obj_set_style_pad_bottom(col, sl_line_h(&ss_regular_16) - lv_font_get_line_height(&ss_regular_16), 0);   /* Arabic descenders */
        lv_obj_t *word = text(col, what[i], &ss_regular_16, SET_HERO_SUB);
        lv_obj_set_style_text_color(word, lv_color_hex(SET_HERO_SUB), 0);
        lv_obj_set_width(word, lv_pct(100));
        lv_obj_set_style_text_align(word, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(word, 1);
    }
    on_click(h, card_row_cb, (void *)(intptr_t)ACT_LIBRARY);
}

/* Developer options stay hidden until the version line at the bottom of
 * Settings is tapped, then for the rest of the session. */
static bool dev_unlocked;
static void dev_overlay(int on)
{
    global_settings.sloppy_dev_overlay = on;
    devhud_show(on);
}
static void dev_reveal_cb(lv_event_t *e)
{
    lv_obj_t *dev = lv_event_get_user_data(e);
    if (!lv_obj_has_flag(dev, LV_OBJ_FLAG_HIDDEN)) return;
    dev_unlocked = true;
    lv_obj_remove_flag(dev, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(dev);
    lv_obj_scroll_to_view_recursive(dev, LV_ANIM_ON);
    toast("Developer options unlocked");
}
static void reset_all_settings_open_cb(lv_event_t *e);
/* The rows almost nobody needs twice: out of the way, in one place. */
static void er_open_cb(lv_event_t *e);
static void advanced_page(void)
{
    lv_obj_t *p, *body = page_new("Advanced", &p);
    lv_obj_t *g = set_group(body, "OTHER SYSTEMS");
    set_row(g, LV_SYMBOL_HOME, 0x66738a, "Open Classic Rockbox", NULL, ACT_CLASSIC);
    set_row(g, LV_SYMBOL_POWER, 0x8b6fd6, "Dual boot screen", NULL, 0x20015);
    /* what cannot be undone: last, in its own card, as on every page */
    g = set_group(body, NULL);
    set_danger(g, LV_SYMBOL_TRASH, "Reset all settings", reset_all_settings_open_cb, NULL);
    set_danger(g, LV_SYMBOL_TRASH, "Erase GabagoolOS data", er_open_cb, NULL);
    opt_note(body, "Reset puts every setting back. Erase also clears history, taste, games and caches. Your music and files always stay.");
    page_push(p);
}

/* Settings: search, whose player this is, then everything there is to
 * set, as a list (one row each, its value at the end) or as tiles. The
 * rarely used things wait under Advanced. */
static void system_page(void)
{
    lv_obj_t *p, *body = page_new("Settings", &p);
    settings_search_pill(body);
    set_hero(body);
    set_hub(body, set_sections, ARRAYLEN(set_sections), set_entries, ARRAYLEN(set_entries));
    lv_obj_t *foot = text(body, sl_update_footer(), &ss_semibold_18, C_DIM);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(foot, 2, 0);
    lv_obj_set_style_pad_top(foot, SL_INSET + SL_SP_XS, 0);
    lv_obj_set_style_pad_bottom(foot, SL_SP_S, 0);
    lv_obj_add_flag(foot, LV_OBJ_FLAG_CLICKABLE);
    /* a section title and its card, hidden together */
    lv_obj_t *dev = box(body);
    lv_obj_set_size(dev, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dev, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *g = set_group(dev, "DEVELOPER");
    set_switch(g, "Performance overlay", "FPS, frame times, CPU and memory up top", global_settings.sloppy_dev_overlay, dev_overlay);
    if (!dev_unlocked && !global_settings.sloppy_dev_overlay) lv_obj_add_flag(dev, LV_OBJ_FLAG_HIDDEN);
    on_click(foot, dev_reveal_cb, dev);
    page_push(p);
}

/* Customization: each row says what it is set to now, nothing more */
static void appearance_page(void)
{
    lv_obj_t *p, *body = page_new("Customization", &p);
    set_hub(body, set_custom_sections, ARRAYLEN(set_custom_sections), set_custom_entries, ARRAYLEN(set_custom_entries));
    page_push(p);
}
/* Library: what is on the player, then keeping it current */
/* Library options: how the album grids look */
static void lo_badges(int on) { global_settings.sloppy_album_badges = on; }
static void lo_artist(int on) { global_settings.sloppy_album_no_artist = !on; }
static void lo_delete(int on) { global_settings.sloppy_files_delete = on; }
/* Artists and genres split or kept whole: the lists are made again the
 * next time they open. Join compilations and Ignore The, A and An read
 * the library again as the page closes (db_update_reload, as after an
 * update: the pages under it are built again around the new library). */
static bool lo_reread;
static void tp_db_auto(int on) { global_settings.sloppy_db_auto = on; }
static void lo_layout(int v) { global_settings.sloppy_album_layout = v; }
static const char *lo_v_folders(void);
static void lo_split_artists(int on) { global_settings.sloppy_artists_whole = !on; tag_pages_drop(); }
static void lo_split_genres(int on) { global_settings.sloppy_genres_whole = !on; tag_pages_drop(); }
static void lo_join(int on) { global_settings.sloppy_albums_apart = !on; lo_reread = true; }
static void lo_articles(int on) { global_settings.sloppy_sort_articles = !on; lo_reread = true; }
static void db_options_reread(void *unused);
static void lo_page_deleted(lv_event_t *e)
{
    (void)e;
    if (!lo_reread) return;
    lo_reread = false;
    lv_async_call(db_options_reread, NULL);
}
static const char *lo_v_seps(void)
{
    static char b[64];
    int n = 0;
    b[0] = '\0';
    for (int i = 0; i < TG_SEPS; i++)
        if (!(global_settings.sloppy_split_off & tg_seps[i].bit))
            n += snprintf(b + n, sizeof b - n, "%s%s", n ? "  " : "", tg_seps[i].name);
    return n ? b : sl_tr("Nothing");
}
static void library_options_page(void)
{
    lv_obj_t *p, *body = page_new("Library options", &p);
    lv_obj_add_event_cb(p, lo_page_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *c = opt_card(body, "Album covers", NULL);
    set_switch(c, "Quality badges", "Lossy, CD, Hi-Res or DSD on each cover", global_settings.sloppy_album_badges, lo_badges);
    set_switch(c, "Show artist", "Under each album's title", !global_settings.sloppy_album_no_artist, lo_artist);
    opt_note(body, "Hi-Res is more than 16 bits or 48 kHz, read from each album's first song.");
    c = opt_card(body, "Album page", NULL);
    static const char *const layouts[] = { "Cover and colour", "Big cover" };
    opt_chips(c, layouts, 2, global_settings.sloppy_album_layout == 1, lo_layout);
    opt_note(body, "Big cover shows an album's cover across the page, its words beneath.");
    c = opt_card(body, "Artists and genres", NULL);
    set_switch(c, "Split artists", "\"Artist feat. Guest\" lists the song under each of them",
               !global_settings.sloppy_artists_whole, lo_split_artists);
    set_action_sub(c, NULL, 0, "Split on", lo_v_seps(), card_row_cb, (void *)(intptr_t)0x20022);
    set_action_sub(c, NULL, 0, "Keep together", "Band names that only look like two artists", card_row_cb, (void *)(intptr_t)0x20023);
    set_switch(c, "Split genres", "\"Rock; Pop\" counts for Rock and for Pop", !global_settings.sloppy_genres_whole, lo_split_genres);
    opt_note(body, "Artists lists everyone a song names. Album artists keeps each album under the artist it's credited to.");
    c = opt_card(body, "Albums and order", NULL);
    set_switch(c, "Join compilations", "Songs of one album in one folder, by different artists, make one album by Various Artists",
               !global_settings.sloppy_albums_apart, lo_join);
    set_switch(c, "Ignore The, A and An", "The Beatles under B", !global_settings.sloppy_sort_articles, lo_articles);
    opt_note(body, "These two read your library again when you leave this page. The music keeps playing.");
    c = opt_card(body, "Scanning", NULL);
    set_action_sub(c, NULL, 0, "Folders to scan", lo_v_folders(), card_row_cb, (void *)(intptr_t)0x20024);
    set_switch(c, "Look for changes at start-up", "New, changed and removed songs, without scanning everything again",
               global_settings.sloppy_db_auto, tp_db_auto);
    opt_note(body, "Changes are also picked up after USB mode and after Files or Wi-Fi transfer add music. Update database scans everything from the start.");
    /* off unless asked for: Files can then delete, and asks every time */
    c = opt_card(body, "Files", NULL);
    set_switch(c, "Allow deleting files", "Delete in Files, on a held file and when picking",
               global_settings.sloppy_files_delete, lo_delete);
    opt_note(body, "Every delete asks first and can't be undone. GabagoolOS's own folder, .rockbox, is never deleted.");
    page_push(p);
}
/* Library options > Folders to scan: each folder at the top of the card,
 * scanned or left out (the tag database's exclude list, and its Audiobooks
 * switch for the audiobook folder). Leaving the page looks for changes:
 * a folder left out is taken out of the library, one let back in is read. */
static bool lo_folders_changed;
static int lo_cmp_name(const void *a, const void *b) { return sl_sort_cmp(*(const char *const *)a, *(const char *const *)b); }
static bool lo_is_books(const char *path)
{
    const char *b = global_settings.audiobook_folder;
    size_t n = strlen(b);
    while (n && b[n - 1] == '/') n--;
    return n && !strncasecmp(path, b, n) && !path[n];
}
static bool lo_folder_scanned(const char *path)
{
    if (lo_is_books(path)) return !global_settings.db_exclude_audiobooks;
    return !tagcache_folder_listed(path);
}
static const char *lo_v_folders(void)
{
    static char b[48];
    int out = 0;
    for (const char *p = global_settings.db_exclude_folders; *p; p++) if (p == global_settings.db_exclude_folders || p[-1] == ':') out++;
    if (!out) return sl_tr("Every folder");
    return sl_count(b, sizeof b, out, "%s left out", "%s left out");
}
static void lo_folder_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target(e);
    const char *path = lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (lo_is_books(path)) global_settings.db_exclude_audiobooks = !on;
    else if (!tagcache_set_folder_excluded(path, !on))
    {
        if (on) lv_obj_remove_state(sw, LV_STATE_CHECKED); else lv_obj_add_state(sw, LV_STATE_CHECKED);
        toast_warn("That's as many folders as can be left out");
        return;
    }
    if (settings_save() != 0) toast_warn("Could not save settings");
    lo_folders_changed = true;
}
static void lo_folder_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }
static void lo_folders_deleted(lv_event_t *e)
{
    (void)e;
    if (!lo_folders_changed) return;
    lo_folders_changed = false;
    db_update_begin(false);              /* looks for changes, and says so */
}
#define LO_FOLDERS_MAX 64
static void lo_folders_page(void)
{
    lv_obj_t *p, *body = page_new("Folders to scan", &p);
    lv_obj_add_event_cb(p, lo_folders_deleted, LV_EVENT_DELETE, NULL);
    /* the folders at the top of the card, A to Z, and any left out deeper down */
    char *names[LO_FOLDERS_MAX];
    int n = 0;
    DIR *d = opendir("/");
    struct dirent *en;
    while (d && (en = readdir(d)) != NULL && n < LO_FOLDERS_MAX)
    {
        if (en->d_name[0] == '.' || !(dir_get_info(d, en).attribute & ATTR_DIRECTORY)) continue;
        if (!strcasecmp((char *)en->d_name, "System Volume Information") || !strcasecmp((char *)en->d_name, "LOST.DIR")
            || en->d_name[0] == '$') continue;
        size_t len = strlen((char *)en->d_name) + 2;
        if ((names[n] = lv_malloc(len))) { snprintf(names[n], len, "/%s", en->d_name); n++; }
    }
    if (d) closedir(d);
    if (n > 1) qsort(names, (size_t)n, sizeof *names, lo_cmp_name);
    for (const char *q = global_settings.db_exclude_folders; *q && n < LO_FOLDERS_MAX; )
    {
        const char *end = strchr(q, ':');
        size_t len = end ? (size_t)(end - q) : strlen(q);
        bool seen = false;
        for (int i = 0; i < n && !seen; i++) seen = strlen(names[i]) == len && !strncasecmp(names[i], q, len);
        if (len && !seen && (names[n] = lv_malloc(len + 1))) { memcpy(names[n], q, len); names[n][len] = '\0'; n++; }
        if (!end) break;
        q = end + 1;
    }
    if (!n) sl_empty(body, IC_FILES, 96, "No folders on the card", "Songs at the top of the card are always scanned.", 40);
    else
    {
        lv_obj_t *g = set_group(body, "Scan these folders");
        for (int i = 0; i < n; i++)
        {
            /* how much of the library each holds now */
            size_t len = strlen(names[i]);
            int songs = 0;
            for (int t = 0; t < sl_lib.n_tracks; t++)
            {
                /* the folder is a whole part of the song's (its file's name has no '/') */
                const char *d = sl_lib.tracks[t].file ? sl_lib.dirs[sl_lib.tracks[t].dir] : "";
                if (!strncasecmp(d, names[i], len) && d[len] == '/') songs++;
            }
            char line[48];
            bool on = lo_folder_scanned(names[i]);
            if (!on) strlcpy(line, sl_tr(lo_is_books(names[i]) ? "In Audiobooks instead" : "Left out"), sizeof line);
            else sl_count(line, sizeof line, songs, "%s song in the library", "%s songs in the library");
            lv_obj_t *sw = set_switch(g, names[i] + 1, line, on, NULL);
            lv_obj_add_event_cb(sw, lo_folder_cb, LV_EVENT_VALUE_CHANGED, names[i]);
            lv_obj_add_event_cb(sw, lo_folder_free, LV_EVENT_DELETE, names[i]);
        }
    }
    opt_note(body, "A folder turned off leaves the library when you leave this page, and comes back when it's turned on again. Its files stay on the card.");
    page_push(p);
}

/* Library options > Split on: each way of naming several artists */
static void lo_sep_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target(e);
    int bit = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) global_settings.sloppy_split_off &= ~bit;
    else global_settings.sloppy_split_off |= bit;
    tag_pages_drop();
    if (settings_save() != 0) toast_warn("Could not save settings");
}
static void lo_seps_page(void)
{
    lv_obj_t *p, *body = page_new("Split on", &p);
    lv_obj_t *g = set_group(body, "Split artists on");
    for (int i = 0; i < TG_SEPS; i++)
    {
        lv_obj_t *sw = set_switch(g, tg_seps[i].example, tg_seps[i].note, !(global_settings.sloppy_split_off & tg_seps[i].bit), NULL);
        lv_obj_add_event_cb(sw, lo_sep_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)tg_seps[i].bit);
    }
    opt_note(body, "Names on Keep together, and well-known ones like Simon & Garfunkel, always stay whole.");
    page_push(p);
}
/* Library options > Keep together: names kept whole, and every name in the
 * library that was split where a band's name could look the same */
#define LO_KEEP_ROWS 400
static void lo_keep_free(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }
static void lo_keep_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target(e);
    const char *name = lv_event_get_user_data(e);
    tg_keep_set(name, lv_obj_has_state(sw, LV_STATE_CHECKED));
    tag_pages_drop();
}
static void lo_keep_page(void)
{
    lv_obj_t *p, *body = page_new("Keep together", &p);
    tg_keep_load();
    /* the library's names that split on &, a comma, a slash, x or vs.,
     * each once, A to Z */
    int cap = sl_lib.n_albums + sl_lib.n_tracks, n = 0;
    const char **names = malloc(((size_t)cap + 1) * sizeof *names);
    for (int pass = 0; names && pass < 2; pass++)
        for (int i = 0; i < (pass ? sl_lib.n_tracks : sl_lib.n_albums); i++)
        {
            const char *s = pass ? sl_lib.tracks[i].artist : sl_lib.albums[i].artist;
            struct tg_piece pc[TG_PIECES];
            bool amb;
            if (!s || tg_split(s, false, pc, TG_PIECES, &amb) < 2 || !amb) continue;
            names[n++] = s;
        }
    if (names && n > 1)
    {
        qsort(names, (size_t)n, sizeof *names, lo_cmp_name);
        int m = 1;
        for (int i = 1; i < n; i++) if (strcasecmp(names[i], names[m - 1])) names[m++] = names[i];
        n = m;
    }
    lv_obj_t *g;
    if (tg_keep_n)
    {
        g = set_group(body, "Kept whole");
        /* the card's list is copied: a switch turned off takes the name off it */
        for (int i = 0; i < tg_keep_n; i++)
        {
            char *copy = lv_strdup(tg_keep_user[i]);
            lv_obj_t *sw = set_switch(g, copy, NULL, true, NULL);
            lv_obj_add_event_cb(sw, lo_keep_cb, LV_EVENT_VALUE_CHANGED, copy);
            lv_obj_add_event_cb(sw, lo_keep_free, LV_EVENT_DELETE, copy);
        }
    }
    if (n)
    {
        char b[64];
        g = set_group(body, sl_count(b, sizeof b, n, "%s NAME SPLIT IN YOUR LIBRARY", "%s NAMES SPLIT IN YOUR LIBRARY"));
        for (int i = 0; i < n && i < LO_KEEP_ROWS; i++)
        {
            struct tg_piece pc[TG_PIECES];
            char line[160];
            int k = tg_split(names[i], false, pc, TG_PIECES, NULL), at = 0;
            for (int j = 0; j < k && at < (int)sizeof line - 8; j++)
                at += snprintf(line + at, sizeof line - at, "%s%.*s", j ? " \xc2\xb7 " : "", pc[j].len, pc[j].p);
            lv_obj_t *sw = set_switch(g, names[i], line, false, NULL);
            lv_obj_add_event_cb(sw, lo_keep_cb, LV_EVENT_VALUE_CHANGED, (void *)names[i]);
        }
        if (n > LO_KEEP_ROWS) set_text(g, sl_tr("The first 400, A to Z."), C_SUB);
    }
    if (!n && !tg_keep_n)
        sl_empty(body, IC_ARTISTS, 96, "Nothing to keep together", "No name in your library was split where a band's name could look the same.", 40);
    opt_note(body, "Simon & Garfunkel, Earth, Wind & Fire and other well-known names always stay whole. A name turned on here counts as one artist.");
    free(names);
    page_push(p);
}
static void sl_library_page(void)
{
    lv_obj_t *p, *body = page_new("Your library", &p);
    set_hub(body, set_library_sections, ARRAYLEN(set_library_sections), set_library_entries, ARRAYLEN(set_library_entries));
    page_push(p);
}

/* Customization > Settings layout */
static void set_layout_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice == global_settings.sloppy_settings_tiles) return;
    global_settings.sloppy_settings_tiles = choice;
    appearance_changed();                  /* Settings again, in the new layout */
}
/* Big icons: Settings again at the new size, this page with it */
static void set_big_icons_apply(int on)
{
    global_settings.sloppy_big_icons = on ? 1 : 0;
    appearance_changed();
}
/* a little Settings: rows in a card, or squares two by two */
static void set_layout_sketch(lv_obj_t *pic, bool tiles)
{
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, C_BG);
    static const uint32_t tints[] = { 0x2d7ff0, 0x0aa9c7, 0xe8408a, 0xf0602f };
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *c = box(s);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, SL_R_MINI, 0);
        lv_obj_t *ic = box(c);
        lv_obj_set_size(ic, 10, 10);
        lv_obj_set_style_radius(ic, SL_R_MINI, 0);
        lv_obj_set_style_bg_color(ic, lv_color_hex(tints[i]), 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_t *ln = box(c);
        lv_obj_set_style_bg_color(ln, lv_color_hex(C_SUB), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_60, 0);
        lv_obj_set_style_radius(ln, SL_R_FULL, 0);
        if (tiles)
        {
            lv_obj_set_size(c, 28, 40);
            lv_obj_set_pos(c, 6 + i % 2 * 32, 8 + i / 2 * 46);
            lv_obj_set_pos(ic, 4, 4);
            lv_obj_set_size(ln, 16, 3);
            lv_obj_set_pos(ln, 4, 31);
        }
        else
        {
            lv_obj_set_size(c, 60, 20);
            lv_obj_set_pos(c, 5, 8 + i * 22);
            lv_obj_set_pos(ic, 4, 5);
            lv_obj_set_size(ln, 32, 3);
            lv_obj_set_pos(ln, 19, 9);
        }
    }
}
static void settings_layout_page(void)
{
    static const char *const names[] = { "List", "Tiles" };
    static const char *const notes[] = { "Grouped rows, each with what it is set to",
                                         "Big squares, two across" };
    lv_obj_t *p, *body = page_new("Settings layout", &p);
    int choice = global_settings.sloppy_settings_tiles == 1 ? 1 : 0;
    lv_obj_t *g = set_group(body, "Layout");
    for (int i = 0; i < 2; i++)
        set_layout_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == choice, set_layout_cb, (void *)(intptr_t)i), 2),
                          i == 1);
    set_switch(set_group(body, "Size"), "Big icons", "Larger icons and rows in Settings", set_big(), set_big_icons_apply);
    page_push(p);
}

static void fmt_db(char *b, int n, int v) { snprintf(b, n, v > 0 ? "+%d dB" : "%d dB", v); }
static void fmt_pct(char *b, int n, int v) { snprintf(b, n, "%d%%", v); }

/* ------------------------------------------------------------------ sound */
#ifdef AUDIOHW_HAVE_BASS
static void snd_bass(int v) { global_settings.bass = v; tone_shelves_apply(); }   /* with Temperature on top */
static const struct opt_slider snd_bass_o = { snd_bass, fmt_db };
#endif
#ifdef AUDIOHW_HAVE_TREBLE
static void snd_treble(int v) { global_settings.treble = v; tone_shelves_apply(); }
static const struct opt_slider snd_treble_o = { snd_treble, fmt_db };
#endif
static void snd_balance(int v) { global_settings.balance = v; sound_set(SOUND_BALANCE, v); }
static void fmt_balance(char *b, int n, int v)
{
    if (v) snprintf(b, n, "%s %d", v < 0 ? "Left" : "Right", v < 0 ? -v : v);
    else strlcpy(b, "Center", n);
}
static const struct opt_slider snd_balance_o = { snd_balance, fmt_balance };
/* The DSP applies a width only in its custom channel mode: the slider
 * picks that mode as it leaves 100 % and plain stereo as it comes back.
 * Mono (R14) is the DSP's own mono mode; the modes only Classic Rockbox
 * offers (one side, karaoke) are left as they are. */
static lv_obj_t *snd_width_row;
static bool snd_channels_ours(void)
{
    int c = global_settings.channel_config;
    return c == SOUND_CHAN_STEREO || c == SOUND_CHAN_CUSTOM || c == SOUND_CHAN_MONO;
}
static void snd_channels_apply(bool mono)
{
    if (!snd_channels_ours()) return;
    global_settings.channel_config = mono ? SOUND_CHAN_MONO :
                                     global_settings.stereo_width != 100 ? SOUND_CHAN_CUSTOM : SOUND_CHAN_STEREO;
    sound_set(SOUND_CHANNELS, global_settings.channel_config);
}
static void snd_width(int v)
{
    global_settings.stereo_width = v;
    sound_set(SOUND_STEREO_WIDTH, v);
    snd_channels_apply(global_settings.channel_config == SOUND_CHAN_MONO);
}
static const struct opt_slider snd_width_o = { snd_width, fmt_pct };
static void snd_mono(int on)
{
    if (!snd_channels_ours()) global_settings.channel_config = SOUND_CHAN_STEREO;   /* the switch takes over */
    snd_channels_apply(on);
    if (snd_width_row) lv_obj_update_flag(snd_width_row, LV_OBJ_FLAG_HIDDEN, on);   /* one channel has no width */
}
static void snd_width_row_deleted(lv_event_t *e) { (void)e; snd_width_row = NULL; }
static void snd_crossfeed(int on) { global_settings.crossfeed = on; dsp_set_crossfeed_type(on); }

static void rg_type(int i)
{
    static const int types[] = { REPLAYGAIN_OFF, REPLAYGAIN_TRACK, REPLAYGAIN_ALBUM, REPLAYGAIN_SHUFFLE };
    global_settings.replaygain_settings.type = types[i];
    replaygain_update();
}
static int rg_choice(void)
{
    switch (global_settings.replaygain_settings.type)
    {
        case REPLAYGAIN_TRACK: return 1;
        case REPLAYGAIN_ALBUM: return 2;
        case REPLAYGAIN_SHUFFLE: return 3;
        default: return 0;
    }
}
static void rg_noclip(int on) { global_settings.replaygain_settings.noclip = on; replaygain_update(); }
static void rg_preamp(int v) { global_settings.replaygain_settings.preamp = v * 10; replaygain_update(); }
static const struct opt_slider rg_preamp_o = { rg_preamp, fmt_db };

/* playback speed, kept at the same pitch */
static const int speed_steps[] = { 75, 100, 125, 150, 200 };
static int speed_pct(void)
{
    return (int)((dsp_get_timestretch() + PITCH_SPEED_PRECISION / 2) / PITCH_SPEED_PRECISION);
}
static void speed_set(int pct)
{
    if (!global_settings.timestretch_enabled)
    {
        global_settings.timestretch_enabled = true;
        dsp_timestretch_enable(true);
    }
    dsp_set_timestretch((int32_t)pct * PITCH_SPEED_PRECISION);
    global_status.resume_speed = (int32_t)pct * PITCH_SPEED_PRECISION;   /* settings_startup restores it */
    status_save(true);
}
static void speed_next(void)
{
    int now = speed_pct(), i = 0;
    while (i < (int)ARRAYLEN(speed_steps) && speed_steps[i] <= now) i++;
    speed_set(speed_steps[i % ARRAYLEN(speed_steps)]);
}
static void speed_chip(int i) { speed_set(speed_steps[i]); }

/* volume: the limit and a fixed level at power-on, as a share of the range */
static int vol_from_pct(int p) { return volume_from_pct(p); }
/* The Volume card sets one output at a time: the one playing when it
 * opens, or the other when its chip is picked. */
static int vol_edit_out;                 /* SL_OUT_JACK, SL_OUT_BT or SL_OUT_DAC */
static lv_obj_t *vol_limit_row, *vol_step_row;
static void vol_limit(int p)
{
    if (vol_edit_out == SL_OUT_DAC) global_settings.sloppy_dac_limit = p;
    else if (vol_edit_out == SL_OUT_BT) global_settings.sloppy_bt_limit = p;
    else global_settings.sloppy_jack_limit = p;
    sl_out_limit_apply();                /* only the output in use changes what plays */
    setvol();
    statusbar_update();
}
static const struct opt_slider vol_limit_o = { vol_limit, fmt_pct };
static void vol_boot(int p) { global_settings.sloppy_boot_volume = p; }
/* each step is half a decibel; the classic perceptual mode counts its own steps */
static void vol_step(int p)
{
    if (vol_edit_out == SL_OUT_DAC) global_settings.sloppy_dac_step = p;
    else if (vol_edit_out == SL_OUT_BT) global_settings.sloppy_bt_step = p;
    else global_settings.sloppy_volume_step = p;
}
static void fmt_vol_step(char *b, int n, int v)
{
#ifdef HAVE_PERCEPTUAL_VOLUME
    if (global_settings.volume_adjust_mode == VOLUME_ADJUST_PERCEPTUAL)
    {
        sl_count(b, n, v, "%s step", "%s steps");
        return;
    }
#endif
    /* the scale's own unit: tenths of a dB on the R1, whole dB elsewhere */
    int x = v * sound_steps(SOUND_VOLUME), unit = sound_numdecimals(SOUND_VOLUME) ? 10 : 1;
    if (x % unit) snprintf(b, n, "%d.%d dB", x / unit, x % unit);
    else snprintf(b, n, "%d dB", x / unit);
}
static const struct opt_slider vol_step_o = { vol_step, fmt_vol_step };
static const struct opt_slider vol_boot_o = { vol_boot, fmt_pct };
static lv_obj_t *vol_boot_row;
static void vol_boot_fixed(int on)
{
    global_settings.sloppy_boot_volume = on ? volume_pct() : -1;
    if (vol_boot_row)
    {
        lv_obj_update_flag(vol_boot_row, LV_OBJ_FLAG_HIDDEN, !on);
        if (on)
        {
            char b[24];
            fmt_pct(b, sizeof b, volume_pct());
            lv_label_set_text(lv_obj_get_child(vol_boot_row, 1), b);
            lv_slider_set_value(lv_obj_get_child(vol_boot_row, 2), volume_pct(), LV_ANIM_OFF);
        }
    }
}
static void vol_boot_row_deleted(lv_event_t *e) { (void)e; vol_boot_row = NULL; }
static void vol_rows_deleted(lv_event_t *e) { (void)e; vol_limit_row = vol_step_row = NULL; }
static int vol_edit_step(void) { return sl_out_step(vol_edit_out); }
/* the sliders show the chosen output's own values */
static void vol_edit_pick(int i)
{
    vol_edit_out = i;
    char b[24];
    lv_obj_t *rows[2] = { vol_limit_row, vol_step_row };
    int vals[2] = { sl_out_limit_pct(vol_edit_out), vol_edit_step() };
    for (int k = 0; k < 2; k++)
    {
        if (!rows[k]) continue;
        if (k) fmt_vol_step(b, sizeof b, vals[k]); else fmt_pct(b, sizeof b, vals[k]);
        lv_label_set_text(lv_obj_get_child(rows[k], 1), b);
        lv_slider_set_value(lv_obj_get_child(rows[k], 2), vals[k], LV_ANIM_OFF);
    }
}

/* output level on the R1's own headphone jack */
static void out_gain(int i) { global_settings.sloppy_gain = i; setvol(); }
static void out_digital_max(int on) { global_settings.sloppy_digital_max = on; setvol(); }
static void out_dsd_gain(int on) { global_settings.sloppy_dsd_gain = on; }
static void out_dither(int on) { global_settings.dithering_enabled = on; dsp_dither_enable(on); }

/* The CS43131's own digital filter, set through the driver's "Digital
 * Filter" control. Only the responses the driver accepts are offered. */
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
static int dac_filters(void)
{
#ifdef SIMULATOR
    return 5;
#else
    return hiby_dac_filter_count();
#endif
}
static void dac_filter(int i) { global_settings.roll_off = i; sound_set(SOUND_FILTER_ROLL_OFF, i); }
#endif
/* NOS: the CS43131 without its oversampling filter, where the driver has
 * the control (HiBy's 2.0 driver; 1.6's has none, and then no switch).
 * On, the filter choice does nothing, so it hides, as width does in Mono. */
static lv_obj_t *dac_filter_row;
static void dac_filter_row_deleted(lv_event_t *e) { (void)e; dac_filter_row = NULL; }
static bool dac_has_nos(void)
{
#ifdef SIMULATOR
    return true;
#else
    return hiby_dac_has_nos();
#endif
}
static void dac_nos(int on)
{
    global_settings.sloppy_dac_nos = on;
#ifndef SIMULATOR
    hiby_dac_set_nos(on);
#endif
    if (dac_filter_row) lv_obj_update_flag(dac_filter_row, LV_OBJ_FLAG_HIDDEN, on);
}

/* ------------------------------------------------------------ signal path */
/* What the music goes through, from the file to the jack, as it is now. */
static void fmt_khz(char *b, int n, unsigned long hz)
{
    if (hz % 1000) snprintf(b, n, "%lu.%lu kHz", hz / 1000, hz % 1000 / 100);
    else snprintf(b, n, "%lu kHz", hz / 1000);
}
static void signal_path_text(char *b, int n)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    if (!id3 || !id3->frequency)
    {
        strlcpy(b, "Nothing playing. Each song opens the DAC at its own sample rate, "
                   "from 44.1 up to 192 kHz, so nothing is converted on the way.", n);
        return;
    }
    char src[20], out[20];
    unsigned long rate = mixer_get_frequency();
    fmt_khz(src, sizeof src, id3->frequency);
    fmt_khz(out, sizeof out, rate);
    const char *codec = get_codec_string(id3->codectype);
    int len;
    /* each sentence in the language, its numbers and names put in after */
    const char *what = codec ? codec : sl_tr("Music");
    if (id3->codectype == AFMT_DSD)
        len = snprintf(b, n, sl_tr("DSD, converted to PCM at %s."), out);
    else if (id3->frequency == rate)
        len = snprintf(b, n, sl_tr("%s at %s. The DAC runs at %s too: no resampling."), what, src, out);
    else if (id3->frequency > rate)
        len = snprintf(b, n, sl_tr("%s at %s, resampled down to %s, the highest rate "
                                   "the player's audio path takes."), what, src, out);
    else
        len = snprintf(b, n, sl_tr("%s at %s, resampled to %s."), what, src, out);
    if (len < 0 || len >= n) return;
    b[len++] = '\n';
    b[len] = '\0';
    if (sl_bt_known && sl_bt_state.routed)
    {
        if (sl_bt_state.codec[0]) snprintf(b + len, n - len, sl_tr("Then over Bluetooth as %s."), sl_bt_state.codec);
        else strlcpy(b + len, sl_tr("Then over Bluetooth."), n - len);
    }
    else
        strlcpy(b + len, sl_tr("Then 16-bit into the CS43131, which sets the volume "
                               "itself and drives the headphone jack."), n - len);
}
static void signal_path_tick(lv_timer_t *t)
{
    char b[512];
    signal_path_text(b, sizeof b);
    label_if_changed(lv_timer_get_user_data(t), b);
}
static void signal_path_deleted(lv_event_t *e) { lv_timer_delete(lv_event_get_user_data(e)); }
static void signal_path_card(lv_obj_t *body)
{
    lv_obj_t *c = set_group(body, "Signal path");
    char b[512];
    signal_path_text(b, sizeof b);
    lv_obj_t *l = set_text(c, b, C_TEXT);
    /* only while the page is open, and once a second: songs change rarely */
    lv_timer_t *t = lv_timer_create(signal_path_tick, 1000, l);
    lv_obj_add_event_cb(l, signal_path_deleted, LV_EVENT_DELETE, t);
}

static void tone_snd_row_deleted(lv_event_t *e) { (void)e; tone_snd_row = NULL; }
static void sound_page(void)
{
    lv_obj_t *p, *body = page_new("Sound", &p);
    /* what is changed most first; how the sound gets to the DAC last */
    lv_obj_t *c;
    c = opt_card(body, "Volume", NULL);
    vol_edit_out = global_status.sloppy_volume_on_bt;
#ifdef HAVE_HOST_USB_AUDIO
    static const char *const out_names[] = { "Headphone jack", "Bluetooth", "USB DAC" };
#else
    static const char *const out_names[] = { "Headphone jack", "Bluetooth" };
#endif
    if (vol_edit_out < 0 || vol_edit_out >= (int)ARRAYLEN(out_names)) vol_edit_out = SL_OUT_JACK;
    opt_chips(c, out_names, ARRAYLEN(out_names), vol_edit_out, vol_edit_pick);
    vol_limit_row = opt_slider(c, "Maximum volume", 10, 100, sl_out_limit_pct(vol_edit_out), &vol_limit_o);
    vol_step_row = opt_slider(c, "Volume button step", 1, 10, vol_edit_step(), &vol_step_o);
    lv_obj_add_event_cb(vol_limit_row, vol_rows_deleted, LV_EVENT_DELETE, NULL);
#ifdef HAVE_HOST_USB_AUDIO
    opt_note(body, "Each output keeps its own level, maximum and button step, and the player changes over as Bluetooth headphones come and go. As a USB DAC it starts at least 30 dB below full scale.");
#else
    opt_note(body, "Each output keeps its own maximum and button step, and the player changes over as Bluetooth headphones come and go.");
#endif
    c = opt_card(body, "Power-on", NULL);
    bool fixed = global_settings.sloppy_boot_volume >= 0;
    opt_switch(c, "Fixed power-on volume", fixed, vol_boot_fixed);
    vol_boot_row = opt_slider(c, "Power-on volume", 0, 100, fixed ? global_settings.sloppy_boot_volume : volume_pct(), &vol_boot_o);
    lv_obj_add_event_cb(vol_boot_row, vol_boot_row_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_update_flag(vol_boot_row, LV_OBJ_FLAG_HIDDEN, !fixed);

    c = opt_card(body, "Output", NULL);
    static const char *const gain_names[] = { "Low gain", "High gain" };
    opt_chips(c, gain_names, 2, global_settings.sloppy_gain ? 1 : 0, out_gain);
    opt_switch(c, "Maximum digital volume", global_settings.sloppy_digital_max, out_digital_max);
    opt_switch(c, "DSD gain compensation (+6 dB)", global_settings.sloppy_dsd_gain, out_dsd_gain);

    c = opt_card(body, "Tone", NULL);
    tone_snd_row = set_row(c, LV_SYMBOL_AUDIO, 0xf0602f, "Tone tuning", tone_row_text(), 0x2001f);
    lv_obj_add_event_cb(tone_snd_row, tone_snd_row_deleted, LV_EVENT_DELETE, NULL);
#ifdef AUDIOHW_HAVE_BASS
    opt_slider(c, "Bass", sound_min(SOUND_BASS), sound_max(SOUND_BASS), global_settings.bass, &snd_bass_o);
#endif
#ifdef AUDIOHW_HAVE_TREBLE
    opt_slider(c, "Treble", sound_min(SOUND_TREBLE), sound_max(SOUND_TREBLE), global_settings.treble, &snd_treble_o);
#endif
    opt_slider(c, "Balance", sound_min(SOUND_BALANCE), sound_max(SOUND_BALANCE), global_settings.balance, &snd_balance_o);


    c = opt_card(body, "ReplayGain", NULL);
    static const char *const rg_names[] = { "Off", "Track", "Album", "Smart" };
    opt_chips(c, rg_names, 4, rg_choice(), rg_type);
    opt_slider(c, "Preamp", -12, 12, global_settings.replaygain_settings.preamp / 10, &rg_preamp_o);
    opt_switch(c, "Prevent clipping", global_settings.replaygain_settings.noclip, rg_noclip);

    c = opt_card(body, "Playback speed", NULL);
    static const char *const speed_names[] = { "0.75x", "1x", "1.25x", "1.5x", "2x" };
    int sel = 1;
    for (int i = 0; i < (int)ARRAYLEN(speed_steps); i++)
        if (speed_steps[i] == speed_pct()) sel = i;
    opt_chips(c, speed_names, ARRAYLEN(speed_steps), sel, speed_chip);

    c = opt_card(body, "Soundfield", NULL);
    opt_switch(c, "Mono", global_settings.channel_config == SOUND_CHAN_MONO, snd_mono);
    snd_width_row = opt_slider(c, "Stereo width", sound_min(SOUND_STEREO_WIDTH), sound_max(SOUND_STEREO_WIDTH),
                               global_settings.stereo_width, &snd_width_o);
    lv_obj_add_event_cb(snd_width_row, snd_width_row_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_update_flag(snd_width_row, LV_OBJ_FLAG_HIDDEN, global_settings.channel_config == SOUND_CHAN_MONO);
    opt_switch(c, "Crossfeed", global_settings.crossfeed != 0, snd_crossfeed);

    /* the DAC: NOS and its filter (each when the driver has it) and dither, together */
    c = opt_card(body, "DAC", NULL);
    bool nos = dac_has_nos();
    if (nos) set_switch(c, "NOS", "Turns off the DAC's oversampling filter", global_settings.sloppy_dac_nos, dac_nos);
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
    int filters = dac_filters();
    if (filters > 1)
    {
        static const char *const filter_names[] = { "Sharp", "Slow", "Short sharp", "Short slow", "Super slow" };
        int on = global_settings.roll_off;
        dac_filter_row = opt_chips(c, filter_names, filters, on >= 0 && on < filters ? on : 0, dac_filter);
        lv_obj_add_event_cb(dac_filter_row, dac_filter_row_deleted, LV_EVENT_DELETE, NULL);
        lv_obj_update_flag(dac_filter_row, LV_OBJ_FLAG_HIDDEN, nos && global_settings.sloppy_dac_nos);
    }
#endif
    opt_switch(c, "Dither to 16-bit", global_settings.dithering_enabled, out_dither);

    signal_path_card(body);
    page_push(p);
}

/* -------------------------------------------------------------- play order */
static void po_albums(int on) { global_settings.sloppy_album_through = on; }
static void po_folders(int on) { global_settings.next_folder = on ? FOLDER_ADVANCE_NEXT : FOLDER_ADVANCE_OFF; }
static void po_auto_np(int on) { global_settings.sloppy_auto_np = on; }

/* the second card of Settings > Playback */
static void play_order_card(lv_obj_t *body)
{
    lv_obj_t *c = opt_card(body, "What plays next", NULL);
    opt_switch(c, "Play through albums", global_settings.sloppy_album_through, po_albums);
    opt_switch(c, "Play through folders", global_settings.next_folder != FOLDER_ADVANCE_OFF, po_folders);
    opt_switch(c, "Open Now Playing when a song starts", global_settings.sloppy_auto_np, po_auto_np);
}

/* ----------------------------------------------------------------- buttons */
static void btn_play_key(int i) { global_settings.sloppy_play_key = i; }
static void btn_remote(int on) { global_settings.sloppy_remote_keys = on; }
static void btn_next_hold(int i) { global_settings.sloppy_next_hold = i; }
static void btn_play_hold(int i) { global_settings.sloppy_play_hold = i; }
static void btn_play_double(int i) { global_settings.sloppy_play_double = i; }
static void btn_next_double(int i) { global_settings.sloppy_next_double = i; }
static void btn_book_next(int i) { global_settings.sloppy_book_next = i; }
static void btn_tap_wake(int on) { global_settings.sloppy_tap_wake = on; sl_power_tap_wake_apply(); }
static void btn_dark(int on)
{
    global_settings.sloppy_keys_dark = on;
#ifdef BUTTON_WAKE_SWALLOWS
    button_set_wake_swallows(!on);
#endif
}

/* Each key: a press, a second press and a hold, each a row of choices.
 * The chips' names say what happens (Settings > Buttons, #41 #64 #147). */
static void sl_flip_apply(bool on);
static void btn_flip(int on) { global_settings.sloppy_flip = on; sl_flip_apply(on); }
static void buttons_page(void)
{
    lv_obj_t *p, *body = page_new("Buttons", &p);
    lv_obj_t *c = opt_card(body, "Play key", NULL);
    static const char *const names[] = { "Play/pause", "Next song", "Previous song", "Now Playing" };
    opt_label(c, "Press");
    opt_chips(c, names, 4, global_settings.sloppy_play_key, btn_play_key);
    static const char *const play_twice[] = { "Nothing", "Next song", "Previous song", "Now Playing" };
    opt_label(c, "Press twice");
    opt_chips(c, play_twice, 4, global_settings.sloppy_play_double, btn_play_double);
    static const char *const play_hold[] = { "Nothing", "Seek back" };
    opt_label(c, "Hold");
    opt_chips(c, play_hold, 2, global_settings.sloppy_play_hold == 1, btn_play_hold);

    c = opt_card(body, "Next key", NULL);
    static const char *const book_names[] = { "30 s ahead", "30 s back" };
    opt_label(c, "Press, in a book");
    opt_chips(c, book_names, 2, global_settings.sloppy_book_next == 1, btn_book_next);
    static const char *const next_twice[] = { "Nothing", "Previous song", "Play/pause" };
    opt_label(c, "Press twice");
    opt_chips(c, next_twice, 3, global_settings.sloppy_next_double, btn_next_double);
    static const char *const hold_names[] = { "Nothing", "Previous song", "Seek ahead" };
    opt_label(c, "Hold");
    int hold = global_settings.sloppy_next_hold;
    opt_chips(c, hold_names, 3, hold >= 0 && hold < 3 ? hold : 0, btn_next_hold);
    opt_note(body, "Hold to seek: the longer you hold, the bigger each step. "
                   "A key with a press-twice choice waits a quarter of a second after a press for the second. "
                   "In a book, next and previous skip 30 seconds, on headsets too.");

    c = opt_card(body, "Remote controls", NULL);
    opt_switch(c, "Headset and Bluetooth controls", global_settings.sloppy_remote_keys, btn_remote);

    c = opt_card(body, "Screen off", NULL);
    opt_switch(c, "Keys work with the screen off", global_settings.sloppy_keys_dark, btn_dark);
    opt_switch(c, "Tap to wake", global_settings.sloppy_tap_wake, btn_tap_wake);

    c = opt_card(body, "Orientation", "Turns the screen round for the keys on the other side. The volume keys swap, so the upper one is still louder.");
    opt_switch(c, "Upside down", global_settings.sloppy_flip == 1, btn_flip);
    page_push(p);
}

/* ---------------------------------------------------------- drag to order */
/* A card of rows put in order by dragging each by its handle (as the
 * queue's rows are): the row lifts and follows the finger, the others make
 * room, the page scrolls on near an edge. Let go over another place and
 * moved(a, b) is told; settled, the card is filled again in its order. */
#define SDL_MAX 18
struct sdl
{
    lv_obj_t *body, *in, *rows[SDL_MAX], *drag;
    int n, row_h, slot, target, y0, sy0, py;
    lv_timer_t *scroll;
    void (*moved)(struct sdl *d, int a, int b);
    void (*fill)(void);
};
static void sdl_shift(struct sdl *d)
{
    for (int i = 0; i < d->n; i++)
    {
        if (i == d->slot) continue;
        int to = d->target > d->slot && i > d->slot && i <= d->target ? -d->row_h :
                 d->target < d->slot && i >= d->target && i < d->slot ? d->row_h : 0;
        int from = lv_obj_get_style_translate_y(d->rows[i], 0);
        if (from != to) anim_prop(d->rows[i], set_ty, from, to, SL_T_FAST, NULL);
    }
}
static void sdl_update(struct sdl *d)
{
    lv_area_t a;
    lv_obj_get_coords(d->body, &a);
    if (d->py < a.y1 + 70) lv_obj_scroll_by_bounded(d->body, 0, 14, LV_ANIM_OFF);   /* near an edge: the page scrolls on */
    else if (d->py > a.y2 - 70) lv_obj_scroll_by_bounded(d->body, 0, -14, LV_ANIM_OFF);
    int dy = d->py - d->y0 + lv_obj_get_scroll_y(d->body) - d->sy0;
    dy = LV_CLAMP(-d->slot * d->row_h, dy, (d->n - 1 - d->slot) * d->row_h);
    lv_obj_set_style_translate_y(d->drag, dy, 0);
    int t = d->slot + (dy >= 0 ? (dy + d->row_h / 2) / d->row_h : -((-dy + d->row_h / 2) / d->row_h));
    if (t != d->target) { d->target = t; sdl_shift(d); }
}
static void sdl_scroll_cb(lv_timer_t *t)
{
    struct sdl *d = lv_timer_get_user_data(t);
    if (!d->drag) { lv_timer_pause(t); return; }
    sdl_update(d);
}
static void sdl_dropped(lv_timer_t *t)
{
    struct sdl *d = lv_timer_get_user_data(t);
    d->drag = NULL;
    if (d->in) d->fill();
}
static void sdl_handle_cb(lv_event_t *e)
{
    struct sdl *d = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target(e));
    lv_point_t p;
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &p);
    if (c == LV_EVENT_PRESSED && !d->drag)
    {
        int slot = 0;
        while (slot < d->n && d->rows[slot] != row) slot++;
        if (slot == d->n) return;
        d->drag = row;
        d->slot = d->target = slot;
        d->y0 = d->py = p.y;
        d->sy0 = lv_obj_get_scroll_y(d->body);
        lv_obj_remove_flag(d->body, LV_OBJ_FLAG_SCROLLABLE);
        /* the rows stay where they are while it is drawn over them: the
         * column is held in place, the lifted row brought to the front */
        lv_obj_update_layout(d->in);
        for (int i = 0; i < d->n; i++) lv_obj_set_pos(d->rows[i], lv_obj_get_x(d->rows[i]), lv_obj_get_y(d->rows[i]));
        lv_obj_set_layout(d->in, LV_LAYOUT_NONE);
        lv_obj_move_foreground(row);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD_P), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        if (!d->scroll) d->scroll = lv_timer_create(sdl_scroll_cb, 30, d);
        lv_timer_reset(d->scroll);
        lv_timer_resume(d->scroll);
    }
    else if (c == LV_EVENT_PRESSING && d->drag == row) { d->py = p.y; sdl_update(d); }
    else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && d->drag == row)
    {
        lv_obj_add_flag(d->body, LV_OBJ_FLAG_SCROLLABLE);
        if (c == LV_EVENT_RELEASED && d->target != d->slot) d->moved(d, d->slot, d->target);
        else d->target = d->slot;
        sdl_shift(d);
        anim_prop(row, set_ty, lv_obj_get_style_translate_y(row, 0), (d->target - d->slot) * d->row_h, SL_T_FAST, NULL);
        lv_timer_t *t = lv_timer_create(sdl_dropped, SL_T_FAST + 20, d);   /* settled: drawn again in order */
        lv_timer_set_repeat_count(t, 1);
    }
}
/* the handle at a row's end, 52 wide */
static void sdl_handle(lv_obj_t *r, struct sdl *d)
{
    lv_obj_t *hd = box(r);
    lv_obj_set_size(hd, 52, d->row_h);
    sl_align(hd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_center(text(hd, LV_SYMBOL_BARS, &sl_sym_24, C_DIM));
    lv_obj_add_flag(hd, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(hd, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(hd, sdl_handle_cb, LV_EVENT_ALL, d);
}
/* the card emptied to be filled again, held in place no more */
static void sdl_clean(struct sdl *d)
{
    memset(d->rows, 0, sizeof d->rows);
    set_group_clean(d->in);
    lv_obj_set_flex_flow(d->in, LV_FLEX_FLOW_COLUMN);
}
static void sdl_gone(struct sdl *d)
{
    if (d->scroll) lv_timer_pause(d->scroll);
    d->body = d->in = d->drag = NULL;
    memset(d->rows, 0, sizeof d->rows);
}

/* ---------------------------------------------------------- control centre */
/* The shortcuts in the panel, arranged here: those in it, in order, each
 * moved by its handle or taken out with its cross; under them the rest,
 * each put back at the end with its plus; and the usual order again. The
 * first eight are the folded panel's. The setting is written whole, ending
 * in '.' (qp_fn_parse). */
static struct
{
    struct sdl d;
    lv_obj_t *more;
    int fn[QP_FNS];
} scc;
static void scc_fill(void);
static void scc_save(const int *fn, int n)
{
    char s[QP_FNS + 2];
    int k = 0;
    for (int i = 0; i < n; i++) s[k++] = qp_fn_keys[fn[i]];
    s[k++] = '.';
    s[k] = '\0';
    strlcpy((char *)global_settings.sloppy_qp_tiles, s, sizeof global_settings.sloppy_qp_tiles);
    quick_panel_rebuild();
    if (settings_save() != 0) toast_warn("Could not save settings");
}
/* Shortcut f moved from place a to place b in the panel */
static void scc_move(struct sdl *d, int a, int b)
{
    if (a < 0 || a >= d->n || b < 0 || b >= d->n || a == b) return;
    int f = scc.fn[a];
    if (a < b) memmove(&scc.fn[a], &scc.fn[a + 1], (size_t)(b - a) * sizeof scc.fn[0]);
    else memmove(&scc.fn[b + 1], &scc.fn[b], (size_t)(a - b) * sizeof scc.fn[0]);
    scc.fn[b] = f;
    scc_save(scc.fn, d->n);
}
static void scc_remove_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e), k = 0;
    if (scc.d.drag) return;
    if (scc.d.n == 1) { toast("Keep at least one shortcut"); return; }
    for (int i = 0; i < scc.d.n; i++) if (scc.fn[i] != f) scc.fn[k++] = scc.fn[i];
    scc.d.n = k;
    scc_save(scc.fn, scc.d.n);
    scc_fill();
}
static void scc_add_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (scc.d.drag || scc.d.n >= QP_FNS) return;
    scc.fn[scc.d.n++] = f;
    scc_save(scc.fn, scc.d.n);
    scc_fill();
}
static void scc_reset_cb(lv_event_t *e)
{
    (void)e;
    if (scc.d.drag) return;
    strlcpy((char *)global_settings.sloppy_qp_tiles, "01234567", sizeof global_settings.sloppy_qp_tiles);
    quick_panel_rebuild();
    if (settings_save() != 0) toast_warn("Could not save settings");
    scc_fill();
    toast("Shortcuts back in the usual order");
}
/* a shortcut's icon, as the control centre draws it */
static lv_obj_t *scc_icon(lv_obj_t *r, int f, int h)
{
    lv_obj_t *ic = set_icon(r, qp_fn_syms[f], qp_fn_tints[f], SET_ICON);
    set_place(ic, SL_INSET, (h - SET_ICON) / 2, SET_ICON);
    if (f == 14) lv_obj_add_event_cb(ic, qx_dark_icon_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    return ic;
}
static void scc_in_row(lv_obj_t *g, int place, int f)
{
    int h = scc.d.row_h, lw = SET_INNER - SET_TEXT_X - 52 - 48 - 2 * SL_SP_S;
    lv_obj_t *r = scc.d.rows[place] = set_row_box(g, h);
    lv_obj_t *a = text(r, qp_fn_names[f], &ss_semibold_21, C_TEXT);
    lv_obj_set_width(a, lw);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    set_place(a, SET_TEXT_X, (h - lv_font_get_line_height(&ss_semibold_21)) / 2, lw);
    scc_icon(r, f, h);
    lv_obj_t *x = icon_btn(r, LV_SYMBOL_CLOSE, 48, scc_remove_cb, (void *)(intptr_t)f);
    sl_align(x, LV_ALIGN_RIGHT_MID, -52, 0);
    sdl_handle(r, &scc.d);
    on_click(r, scc_remove_cb, (void *)(intptr_t)f);   /* a tap takes it out, as it always did */
    set_sep(g, r, SET_TEXT_X);
}
static void scc_more_row(lv_obj_t *g, int f)
{
    int h = scc.d.row_h, lw = SET_INNER - SET_TEXT_X - 48 - 2 * SL_SP_S;
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, qp_fn_names[f], &ss_semibold_21, C_TEXT);
    lv_obj_set_width(a, lw);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    set_place(a, SET_TEXT_X, (h - lv_font_get_line_height(&ss_semibold_21)) / 2, lw);
    scc_icon(r, f, h);
    lv_obj_t *plus = icon_btn(r, LV_SYMBOL_PLUS, 48, scc_add_cb, (void *)(intptr_t)f);
    sl_align(plus, LV_ALIGN_RIGHT_MID, -SL_SP_S, 0);
    on_click(r, scc_add_cb, (void *)(intptr_t)f);
    set_sep(g, r, SET_TEXT_X);
}
static void scc_fill(void)
{
    qp_fn_parse(scc.fn, &scc.d.n);       /* without a '.', every one is in: the eight, then the rest */
    sdl_clean(&scc.d);
    set_group_clean(scc.more);
    for (int i = 0; i < scc.d.n; i++) scc_in_row(scc.d.in, i, scc.fn[i]);
    unsigned in = 0;
    for (int i = 0; i < scc.d.n; i++) in |= 1u << scc.fn[i];
    int left = 0;
    for (int f = 0; f < QP_FNS; f++) if (!(in & 1u << f)) { scc_more_row(scc.more, f); left++; }
    lv_obj_update_flag(scc.more, LV_OBJ_FLAG_HIDDEN, !left);
    lv_obj_update_flag(lv_obj_get_child(scc.d.body, lv_obj_get_index(scc.more) - 1), LV_OBJ_FLAG_HIDDEN, !left);   /* its header */
}
static void scc_gone(lv_event_t *e)
{
    (void)e;
    sdl_gone(&scc.d);
    scc.more = NULL;
}

/* Notifications on: a history in the Notification Center, and banners.
 * Off: toasts that pop up and go. Either way, the switch says so in the
 * style it just chose. */
static void notes_switch(int on)
{
    global_settings.sloppy_notifications = on;
    if (on) pop_hide_now();
    else nbanner_hide_now();
    qp_notes_refresh();
    if (on) notify_ex(NS_SETTINGS, NOTE_INFO, 0, NA_NONE, "notes", "Notifications on. They wait for you in the Notification Center.");
    else notify_ex(NS_SETTINGS, NOTE_INFO, 0, NA_NONE, "notes", "Toasts it is. Messages pop up here and go.");
}
static void shortcuts_page(void)
{
    lv_obj_t *p, *body = page_new("Notification Center", &p);
    lv_obj_t *nc = set_group(body, "Notifications");
    set_switch(nc, "Notification history", "Off: messages pop up as a little toast at the top, then go. "
               "On: they also wait in the Notification Center until you clear them.", notes_on(), notes_switch);
    scc.d.body = body;
    scc.d.row_h = LV_MAX(SET_ROW_H, lv_font_get_line_height(&ss_semibold_21) + 2 * SET_PAD_V);
    scc.d.drag = NULL;
    scc.d.moved = scc_move;
    scc.d.fill = scc_fill;
    scc.d.in = set_group(body, "IN THE CONTROL CENTER");
    opt_note(body, "The first 8 are there when it opens, the rest when you pull it further, 16 to a page. "
                   "Drag one by its handle to move it. Touch and hold one there for its settings.");
    scc.more = set_group(body, "MORE SHORTCUTS");
    lv_obj_add_event_cb(scc.d.in, scc_gone, LV_EVENT_DELETE, NULL);
    scc_fill();
    set_action(set_group(body, NULL), LV_SYMBOL_REFRESH, 0x66738a, "Reset to the usual order", NULL, scc_reset_cb, NULL);
    page_push(p);
}

/* ---------------------------------------------------------- arrange music */
/* Music's ways in, put in order as the control centre's shortcuts are: the
 * tiles among the tiles, the cards among the cards, each by its handle; a
 * tap or the eye hides one from Music, or shows it again. */
static struct sdl mua[2];               /* the tiles, the cards */
static void mua_fill(void);
static void mua_move(struct sdl *d, int a, int b)
{
    int k = (int)(d - mua), at[MU_N], n = 0;
    for (int i = 0; i < MU_N; i++) if (mu_defs[mu_order[i]].tile == !k) at[n++] = i;
    if (a < 0 || a >= n || b < 0 || b >= n || a == b) return;
    uint8_t id = mu_order[at[a]];
    if (a < b) for (int i = a; i < b; i++) mu_order[at[i]] = mu_order[at[i + 1]];
    else for (int i = a; i > b; i--) mu_order[at[i]] = mu_order[at[i - 1]];
    mu_order[at[b]] = id;
    mu_save();
    mu_dirty = true;
}
static void mua_eye_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (mua[0].drag || mua[1].drag) return;
    mu_hidden[id] = !mu_hidden[id];
    mu_save();
    mu_dirty = true;
    mua_fill();
}
static void mua_row(struct sdl *d, int id)
{
    int h = d->row_h, lw = SET_INNER - SL_INSET - 52 - 48 - 2 * SL_SP_S;
    lv_obj_t *r = d->rows[d->n++] = set_row_box(d->in, h);
    lv_obj_t *a = text(r, mu_defs[id].name, &ss_semibold_21, mu_hidden[id] ? C_SUB : C_TEXT);
    lv_obj_set_width(a, lw);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    set_place(a, SL_INSET, (h - lv_font_get_line_height(&ss_semibold_21)) / 2, lw);
    lv_obj_t *eye = icon_btn(r, mu_hidden[id] ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN, 48, mua_eye_cb, (void *)(intptr_t)id);
    sl_align(eye, LV_ALIGN_RIGHT_MID, -52, 0);
    sdl_handle(r, d);
    on_click(r, mua_eye_cb, (void *)(intptr_t)id);
    set_sep(d->in, r, SL_INSET);
}
static void mua_fill(void)
{
    for (int k = 0; k < 2; k++)
    {
        sdl_clean(&mua[k]);
        mua[k].n = 0;
        for (int i = 0; i < MU_N; i++) if (mu_defs[mu_order[i]].tile == !k) mua_row(&mua[k], mu_order[i]);
    }
}
static void mua_gone(lv_event_t *e)
{
    (void)e;
    sdl_gone(&mua[0]);
    sdl_gone(&mua[1]);
    if (mu_dirty) lv_async_call(mu_refresh_async, NULL);
}
static void mu_arrange_page(void)
{
    mu_load();
    lv_obj_t *p, *body = page_new("Arrange Music", &p);
    lv_obj_add_event_cb(p, mua_gone, LV_EVENT_DELETE, NULL);
    opt_note(body, "Drag one by its handle to move it, a tile among the tiles. Tap one to hide it from Music, or to show it again.");
    for (int k = 0; k < 2; k++)
    {
        mua[k].body = body;
        mua[k].row_h = LV_MAX(SET_ROW_H, lv_font_get_line_height(&ss_semibold_21) + 2 * SET_PAD_V);
        mua[k].drag = NULL;
        mua[k].moved = mua_move;
        mua[k].fill = mua_fill;
        mua[k].in = set_group(body, k ? "MORE WAYS IN" : "TILES");
    }
    mua_fill();
    page_push(p);
}

/* ---------------------------------------------------------- time and power */
static void tp_clock(int h24) { global_settings.timeformat = h24 ? 0 : 1; statusbar_update(); }
static void tp_clock_pos(int pos) { global_settings.sloppy_clock_pos = pos; statusbar_update(); }
static void tp_battery(int on) { global_settings.battery_display = on; statusbar_update(); }
static const int tp_off_minutes[] = { 0, 5, 10, 15, 30, 60 };
static void tp_poweroff(int i)
{
    global_settings.poweroff = tp_off_minutes[i];
    set_poweroff_timeout(global_settings.poweroff);
}
#ifdef HAVE_GENERAL_PURPOSE_LED
static void tp_led(int on) { global_settings.use_led_indicators = on; led_hw_set_mode(on); }
#endif
#ifdef HAVE_HIBY_LINUX_POWER_CHARGE_LIMIT
/* Battery care: the charger's top voltage. "charge limit v" counts 1 for
 * 4.40 V (HiBy's own), 2 for 4.35 V, 3 for 4.20 V. Full writes 4.40 V
 * rather than leaving the setting off, because the charger keeps the
 * last limit written until the battery is disconnected. */
static const int tp_care_limit[] = { 1, 2, 3 };
static void tp_care(int i)
{
    global_settings.hiby_charge_limit_voltage = tp_care_limit[i];
#ifndef SIMULATOR
    set_charge_limit_voltage(tp_care_limit[i]);
#endif
}
#endif
#if defined(HAVE_USB_POWER) && !defined(USB_NONE) && !defined(SIMULATOR)
/* The fourth choice makes the player a USB sound card: charge-only USB
 * with the vendor kernel's audio function on top, so the computer's sound
 * plays through the CS43131. */
static void tp_usb(int i)
{
#ifdef HAVE_HOST_USB_AUDIO
    int dac = i == 3 ? 2 : 0;           /* usb_audio: while charge-only */
    if (i == 3) i = 1;                  /* USB_MODE_CHARGE */
    global_settings.usb_audio = dac;
#endif
    global_settings.usb_mode = i;
    usb_set_mode(i);
#ifdef HAVE_HOST_USB_AUDIO
    usb_set_audio(dac);
#endif
}
#endif

static lv_obj_t *tz_card(lv_obj_t *body);
static void bl_card(lv_obj_t *body);
static const char *set_v_car(void);
static void car_page(void);
static void tp_car_cb(lv_event_t *e) { (void)e; car_page(); }
static void time_power_page(void)
{
    lv_obj_t *p, *body = page_new("Time and power", &p);
    /* one card per subject: the clock, power off, the battery, USB */
    lv_obj_t *c = tz_card(body);
    opt_switch(c, "24-hour clock", global_settings.timeformat == 0, tp_clock);
    opt_label(c, "Clock position");
    static const char *const clock_names[] = { "Left", "Centre", "Right" };
    opt_chips(c, clock_names, 3, sb_clock_pos(), tp_clock_pos);

    c = opt_card(body, "Auto power off", NULL);
    static const char *const off_names[] = { "Never", "5 min", "10 min", "15 min", "30 min", "60 min" };
    int sel = 0;
    for (int i = 0; i < (int)ARRAYLEN(tp_off_minutes); i++)
        if (tp_off_minutes[i] == global_settings.poweroff) sel = i;
    opt_chips(c, off_names, ARRAYLEN(tp_off_minutes), sel, tp_poweroff);

    c = set_group(body, "In the car");
    set_link(c, LV_SYMBOL_CHARGE, 0x2d7ff0, "Car mode", set_v_car(), tp_car_cb, NULL);

    c = opt_card(body, "Battery", NULL);
    opt_switch(c, "Battery percentage", global_settings.battery_display != 0, tp_battery);
#ifdef HAVE_GENERAL_PURPOSE_LED
    opt_switch(c, "LED indicator", global_settings.use_led_indicators, tp_led);
#endif
#ifdef HAVE_HIBY_LINUX_POWER_CHARGE_LIMIT
    opt_label(c, "Charge to");
    static const char *const care_names[] = { "Full", "95%", "80%" };
    int care = 0;
    for (int i = 0; i < (int)ARRAYLEN(tp_care_limit); i++)
        if (tp_care_limit[i] == global_settings.hiby_charge_limit_voltage) care = i;
    opt_chips(c, care_names, ARRAYLEN(tp_care_limit), care, tp_care);
#endif
    bl_card(body);


#if defined(HAVE_USB_POWER) && !defined(USB_NONE) && !defined(SIMULATOR)
#ifdef HAVE_HOST_USB_AUDIO
    c = opt_card(body, "USB", NULL);
    static const char *const usb_names[] = { "Transfer files", "Charge only", "Debugging (ADB)", "USB DAC" };
    int usb_count = 4;
#else
    c = opt_card(body, "USB", NULL);
    static const char *const usb_names[] = { "Transfer files", "Charge only", "Debugging (ADB)" };
    int usb_count = 3;
#endif
    int usb = global_settings.usb_mode;
    usb = usb >= 0 && usb < 3 ? usb : 0;
#ifdef HAVE_HOST_USB_AUDIO
    if (usb == 1 && global_settings.usb_audio == 2) usb = 3;
#endif
    opt_chips(c, usb_names, usb_count, usb, tp_usb);
#endif

    page_push(p);
}

/* ------------------------------------------------------------ lyrics */
/* Customization > Lyrics: how big the lines are (Small to Extra large, on
 * top of Text size, as stock 2.0 offers) and the sung line's colour, both
 * seen at once in a preview drawn as Now Playing draws them. */
static lv_obj_t *lyr_sample[3], *lyr_size_value;
static void lyr_sample_paint(void)
{
    for (int i = 0; i < 3; i++)
    {
        if (!lyr_sample[i]) return;
        lv_obj_set_style_text_font(lyr_sample[i], lyr_font(i == 1, true), 0);
        lv_obj_set_style_text_color(lyr_sample[i], lv_color_hex(i == 1 ? lyr_color(global_settings.sloppy_lyrics_color) : 0x8f98aa), 0);
        lv_obj_set_style_text_opa(lyr_sample[i], i == 1 ? LV_OPA_COVER : LYR_DIM, 0);
    }
    int sz = global_settings.sloppy_lyrics_size;
    if (lyr_size_value) label_if_changed(lyr_size_value, sl_tr(lyr_size_names[sz >= 0 && sz <= 3 ? sz : 1]));
}
static void lyr_sample_gone(lv_event_t *e) { (void)e; memset(lyr_sample, 0, sizeof lyr_sample); lyr_size_value = NULL; }
static void lyrics_size_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_current_target(e));
    global_settings.sloppy_lyrics_size = v;
    lyr_sample_paint();                  /* the preview follows the finger */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && settings_save() != 0) toast_warn("Could not save settings");
}
static void lyrics_color_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.sloppy_lyrics_color = choice;
    set_check_mark(lv_event_get_current_target(e));
    lyr_sample_paint();
    if (settings_save() != 0) toast_warn("Could not save settings");
}
static const char *lyr_v_now(void)
{
    static char b[64];
    int sz = global_settings.sloppy_lyrics_size;
    snprintf(b, sizeof b, "%s, %s", sl_tr(lyr_size_names[sz >= 0 && sz <= 3 ? sz : 1]), sl_tr(lyr_color_now()));
    return b;
}
/* the colours as round swatches where the icons go, each ticked when chosen */
static void lyrics_color_page(void)
{
    lv_obj_t *p, *body = page_new("Lyrics", &p);
    lv_obj_t *g = set_group(body, "PREVIEW");
    /* three lines on the dark of a blurred cover, the middle one sung; tall
     * enough for the biggest, so the slider under it stays put */
    int keep = global_settings.sloppy_lyrics_size, h = 0;
    for (int k = 0; k < 4; k++)
    {
        global_settings.sloppy_lyrics_size = k;
        int t = lv_font_get_line_height(lyr_font(true, true)) + 2 * lv_font_get_line_height(lyr_font(false, true));
        if (t > h) h = t;
    }
    global_settings.sloppy_lyrics_size = keep;
    lv_obj_t *stage = box(g);
    lv_obj_set_size(stage, lv_pct(100), h + 2 * SL_GUTTER + 2 * SL_SP_S);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(stage, lv_color_mix(lv_color_hex(C_ACCENT), lv_color_black(), 70), 0);
    lv_obj_set_style_bg_grad_color(stage, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_grad_dir(stage, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(stage, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_flex_flow(stage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stage, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stage, SL_SP_S / 2, 0);
    static const char *const words[3] = { "Streetlights hum", "Slow and blue", "Every sign said west" };
    for (int i = 0; i < 3; i++)
    {
        lyr_sample[i] = lv_label_create(stage);
        lv_label_set_text(lyr_sample[i], words[i]);   /* sample lyrics, as they would be sung */
        lv_obj_set_width(lyr_sample[i], lv_pct(100));
        lv_obj_set_style_pad_hor(lyr_sample[i], SL_INSET, 0);
        lv_obj_set_style_text_align(lyr_sample[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lyr_sample[i], LV_LABEL_LONG_DOT);
    }
    lv_obj_add_event_cb(stage, lyr_sample_gone, LV_EVENT_DELETE, NULL);
    lv_obj_t *r = set_slider_row(g, "Size", "", C_ACCENT_TXT);
    lyr_size_value = lv_obj_get_child(r, 1);
    lv_obj_t *sl = lv_obj_get_child(r, 2);
    lv_slider_set_range(sl, 0, 3);
    lv_slider_set_value(sl, keep >= 0 && keep <= 3 ? keep : 1, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, lyrics_size_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, lyrics_size_cb, LV_EVENT_RELEASED, NULL);
    lyr_sample_paint();
    opt_note(body, "Lyrics grow with Text size too.");
    int choice = global_settings.sloppy_lyrics_color;
    g = set_group(body, "Lyrics color");
    for (int i = 0; i < (int)ARRAYLEN(lyr_color_names); i++)
    {
        lv_obj_t *row = set_check(g, "", lyr_color(i), lyr_color_names[i], i == choice,
                                  lyrics_color_cb, (void *)(intptr_t)i);
        lv_obj_t *dot = lv_obj_get_child(row, 2);   /* the icon square, made a swatch */
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(lyr_color(i)), 0);
        lv_obj_set_style_bg_grad_dir(dot, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(C_DIM), 0);
    }
    page_push(p);
}

/* ------------------------------------------------------ language, text size */
static const char *const lang_names[SL_LANG_LAST + 1] = { "English", "Español", "Français", "Deutsch", "עברית", "العربية", "简体中文",
                                                         "Italiano", "Nederlands", "Português (Brasil)", "Polski", "Čeština", "Русский", "日本語" };
static const char *lang_name_now(void)
{
    int l = global_settings.sloppy_language;
    return l >= 0 && l < (int)(sizeof lang_names / sizeof lang_names[0]) ? lang_names[l] : lang_names[0];
}

static void language_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == global_settings.sloppy_language) return;
    global_settings.sloppy_language = i;
    appearance_changed();                  /* every page again, in the new language */
}

static void language_page(void)
{
    lv_obj_t *p, *body = page_new("Language", &p);
    lv_obj_t *g = set_group(body, "Menus and messages");
    for (int i = 0; i < (int)ARRAYLEN(lang_names); i++)
        set_check(g, NULL, 0, lang_names[i], i == global_settings.sloppy_language, language_cb, (void *)(intptr_t)i);
    page_push(p);
}

static void typing_swipe(int on) { global_settings.sloppy_swipe_typing = on; }
static void typing_t9(int on) { global_settings.sloppy_kb_t9 = on; }
/* ------------------------------------------------------------ keyboards */
/* a keyboard's name as Settings lists it: its own language's word for it */
static const char *kb_name(int k)
{
    return k == KB_ZH ? sl_tr("Chinese (Pinyin)") : k == KB_JA ? sl_tr("Japanese (Romaji)") : k == KB_T9 ? sl_tr("Phone keypad (T9)") : kb_defs[k].name;
}
/* the hub's value: the two, or the one when both are the same */
static const char *kb_v_now(void)
{
    static char b[96];
    int a = kb_chosen(0), c = kb_chosen(1);
    if (a == c) strlcpy(b, kb_name(a), sizeof b);
    else snprintf(b, sizeof b, "%s, %s", kb_name(a), kb_name(c));
    return b;
}
static lv_obj_t *kbs_group;             /* the Keyboards page's two rows, kept fresh from the pickers */
static void kbs_deleted(lv_event_t *e) { (void)e; kbs_group = NULL; }
static void kbs_rows(void);
static void kbs_pick_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e), slot = v >> 8, k = v & 0xff;
    if (slot) global_settings.sloppy_kb_second = k + 1; else global_settings.sloppy_kb_first = k + 1;
    set_check_mark(lv_event_get_current_target(e));
    kbs_rows();
    if (settings_save() != 0) toast_warn("Could not save settings");
}
static void kbs_pick_page(int slot)
{
    lv_obj_t *p, *body = page_new(slot ? "Second keyboard" : "First keyboard", &p);
    lv_obj_t *g = set_group(body, "KEYBOARDS");
    static const int order[KB_N] = { KB_EN, KB_ES, KB_FR, KB_DE, KB_IT, KB_PT, KB_NL, KB_PL, KB_CS, KB_RU, KB_HE, KB_AR, KB_ZH, KB_JA, KB_T9 };
    for (int i = 0; i < KB_N; i++)
        set_check(g, NULL, 0, kb_name(order[i]), order[i] == kb_chosen(slot), kbs_pick_cb, (void *)(intptr_t)(slot << 8 | order[i]));
    opt_note(body, slot ? "The globe key switches to this one. Choose your first keyboard again to type with one only."
                        : "The keyboard every field opens with.");
    page_push(p);
}
static void kbs_first_cb(lv_event_t *e) { (void)e; kbs_pick_page(0); }
static void kbs_second_cb(lv_event_t *e) { (void)e; kbs_pick_page(1); }
static void kbs_rows(void)
{
    if (!kbs_group) return;
    set_group_clean(kbs_group);
    for (int slot = 0; slot < 2; slot++)
        set_link(kbs_group, LV_SYMBOL_KEYBOARD, 0x4f7fe0 | SET_TILE_ONLY, slot ? "Second keyboard" : "First keyboard",
                 kb_name(kb_chosen(slot)), slot ? kbs_second_cb : kbs_first_cb, NULL);
}
static void keyboards_page(void)
{
    lv_obj_t *p, *body = page_new("Keyboards", &p);
    lv_obj_add_event_cb(p, kbs_deleted, LV_EVENT_DELETE, NULL);
    kbs_group = set_group(body, "YOUR KEYBOARDS");
    kbs_rows();
    opt_note(body, "The globe key switches between them, and the space bar says which one is in use. Hold a letter for its accents.");
    lv_obj_t *c = opt_card(body, "Typing", "Slide across the letters of a word without lifting your finger.");
    opt_switch(c, "Swipe typing", global_settings.sloppy_swipe_typing, typing_swipe);
    c = opt_card(body, "Phone keypad", "Each key once, and the words it could be come up; off, a key pressed until its letter comes. Hold a key for its digit.");
    opt_switch(c, "Predictive (T9)", global_settings.sloppy_kb_t9 != 0, typing_t9);
    page_push(p);
}

static lv_obj_t *text_size_sample[2], *text_size_value;
static void text_size_show(int i)
{
    lv_obj_set_style_text_font(text_size_sample[0], sl_fonts[i][5], 0);
    lv_obj_set_style_text_font(text_size_sample[1], sl_fonts[i][1], 0);
    label_if_changed(text_size_value, sl_tr(text_size_names[i]));
}
/* the sample follows the finger; the pages are rebuilt once, on release */
static void text_size_cb(lv_event_t *e)
{
    int i = lv_slider_get_value(lv_event_get_current_target(e));
    text_size_show(i);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED || i == global_settings.sloppy_text_size) return;
    global_settings.sloppy_text_size = i;
    sl_text_step = i;
    appearance_changed();
}

static void text_size_page(void)
{
    lv_obj_t *p, *body = page_new("Text size", &p);
    lv_obj_t *c = set_group(body, "PREVIEW");
    /* a fixed height, so the slider under it stays put as the sample grows */
    lv_obj_t *sample = box(c);
    lv_obj_set_size(sample, lv_pct(100), 96 + 2 * SL_GUTTER);
    lv_obj_set_style_pad_hor(sample, SL_INSET, 0);
    lv_obj_set_style_pad_ver(sample, SL_GUTTER, 0);
    lv_obj_set_flex_flow(sample, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sample, LV_FLEX_ALIGN_END, set_rtl() ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          set_rtl() ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
    text_size_sample[0] = text(sample, "Night Tide", sl_fonts[0][5], C_TEXT);
    text_size_sample[1] = text(sample, "Mira Sol", sl_fonts[0][1], C_SUB);
    lv_obj_t *r = set_slider_row(c, "Size", "", C_ACCENT_TXT);
    text_size_value = lv_obj_get_child(r, 1);
    lv_obj_t *s = lv_obj_get_child(r, 2);
    lv_slider_set_range(s, 0, SL_TEXT_STEPS - 1);
    int now = global_settings.sloppy_text_size;
    if (now < 0 || now >= SL_TEXT_STEPS) now = 0;
    lv_slider_set_value(s, now, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, text_size_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, text_size_cb, LV_EVENT_RELEASED, NULL);
    text_size_show(now);
    page_push(p);
}

/* --------------------------------------------------------------- start-up */
static void wifi_startup(void);
static bool scr_any(void);
static void scr_apply(void);
/* Applied once when GabagoolOS starts. */
static void settings_startup(void)
{
    sl_text_step = global_settings.sloppy_text_size;
    if (sl_text_step < 0 || sl_text_step >= SL_TEXT_STEPS) sl_text_step = 0;
    wifi_startup();
    if (scr_any()) scr_apply();
    sl_out_volume_startup();
    sl_bt_volume_startup();
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
    sl_bt_startup();                     /* left on: back on, and the last headphones looked for */
#endif
    tone_apply_all();                    /* Tone tuning and the EQ's preamp ride on what Rockbox set up */
    if (global_settings.sloppy_boot_volume >= 0)
    {
        global_status.volume = vol_from_pct(global_settings.sloppy_boot_volume);
        setvol();
    }
    /* the speed last chosen; Rockbox restores it only from its own menus */
    if (global_status.resume_speed != PITCH_SPEED_100 && global_settings.timestretch_enabled)
        dsp_set_timestretch(global_status.resume_speed);
#ifdef BUTTON_WAKE_SWALLOWS
    button_set_wake_swallows(!global_settings.sloppy_keys_dark);
#endif
}
