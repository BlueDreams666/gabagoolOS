/* ---- the screen audit: every page, sheet and overlay, in every look, read
 * object by object for words that overlap, are cut off, sit under the
 * status bar, the mini player or a screen corner, are too small to tap, or
 * are too faint to read.
 *
 *   SLOPPY_AUDIT=<dir> ./ui-test    the whole matrix; <dir>/audit.tsv and a
 *                                   marked screenshot of each new finding
 *                                   in <dir>/shots, then stop
 *   (no env)                        the suite runs au_gate(): a smaller
 *                                   matrix that must find nothing
 *
 * One line per finding in audit.tsv:
 *   screen  look  rule  object-path  "text"  rect  other-rect  detail
 * rules: a overlap, b clipped, c chrome (status bar, mini player, corner),
 *        d tap target under 44 px, e contrast.
 *
 * What is allowed on purpose (each one counted in the summary):
 *  A1 an object over its own ancestor or descendant (words on their own
 *     card or button, a badge inside its icon, a title on its cover)
 *  A2 anything over a cover, image or canvas (art is not text or a control)
 *  A3 everything under a full-screen surface (a page, a sheet's scrim, the
 *     control centre's glass) is out of sight and not read
 *  A4 a marquee label (LONG_SCROLL*) may run past its box
 *  A5 a LONG_DOT label shows an ellipsis where it stops
 *  A6 Zule's pivot title (the 104 px face) runs off the right edge
 *  A7 content in a scrolling list may pass under something floating over
 *     it (the mini player, a bar, a toast) on the side it can still scroll
 *     towards; at the end of the list it must clear it (checked)
 *  A8 words fading in or out (opacity under 25 %) are not read
 *  A9 a control drawn as a full-screen catcher (the page, a scrim) has no
 *     size rule
 *  A10 game boards draw their own world: contrast and overlap inside a
 *     game's canvas or image are the game's art
 *  A11 a small control inside a tappable row or card: the row takes the tap
 *  A12 lyric lines that are not being sung are dimmed on purpose; the line
 *      being sung is read
 *  A13 an icon on a button or row that also has its words is decoration
 *      (WCAG 1.4.11: 3:1 only for graphics needed to understand the control)
 *  A14 a toast, banner or screenshot card may pass over list content (it leaves in seconds
 *      and a flick sends it away); never over a fixed control
 *  A15 an inactive control (disabled state) may be faint (WCAG 1.4.3 exempts it)
 *  A16 the GabagoolOS logotype (brand_mark, Settings' card): its letters
 *      touch and its red drop layer sits under the cream face by design; the
 *      face itself is read like any words (WCAG 1.4.3 exempts logotypes)
 *  A17 a count badge on its own button's glyph (Now Playing's repeat-one
 *      "1"): a disc of 24 px or less with a mark of two letters at most,
 *      set on the corner of the icon beside it by design
 *
 * Deferred contrast findings, real and printed apart (not fixed in this pass):
 *  D1 words and icons on the control centre's glass: the backdrop moves
 *  D3 white words on Zule's pink and on the accent preview's swatch
 */
#include "src/misc/lv_event_private.h"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_draw_private.h"
/* batch E's screens (batch_e.inc) */
static void e_au_radio(int which);
static void e_au_reset(void);
static void e_au_receive(int which);
static void e_au_lastfm(int which);
static void e_au_libjob(int which);
/* batch F's (batch_f.inc) */
static void f_au_reset(void);
static void f_au_np_album(void);
static void f_au_batlog(void);
static void f_au_erase(void);
static void f_au_saver(int style);
static void f_au_car(int which);
/* batch H's (batch_h.inc) */
static void h_au_pod(int which);
static void h_au_reset(void);
static void wave_au(int which);
static void wave_au_reset(void);
/* the e-book reader's (batch_epub.inc) */
static void ep_au(int which);
/* the music server's (batch_sub.inc) */
static void sb_au(int which);
static void sb_au_reset(void);
/* Audiobookshelf's (batch_abs.inc) */
static void av_au(int which);
static void av_au_reset(void);
static void ep_au_reset(void);
static void rm_au(int which);
static void rm_au_reset(void);
/* software updates' (batch_ota.inc) */
static void ot_au(int which);
static void ot_au_reset(void);

#define AU_MAX 6000
#define AU_GATE_STRICT 1       /* 1 once the findings are fixed: the suite fails on any new one */
#define AU_MIN_TAP 44
#define AU_CORNER_R 24          /* the R1 panel's rounded corners (an estimate, see the report) */
enum { AU_LABEL = 1, AU_ICON = 2, AU_CTRL = 4, AU_SURF = 8, AU_ART = 16 };
struct au_item {
    lv_obj_t *o;
    int order, kind, opa;
    lv_area_t box, vis, txt;     /* txt: the words' own extent (labels, icons), clipped to view */
    bool chrome;                 /* the status bar or the mini player */
    bool large;                  /* large text for contrast */
    int surf;                    /* order of the opaque surface it sits on (itself, or an ancestor); -1 the screen */
    bool hidden;                 /* wholly under a later opaque surface of the page: not on screen */
    bool buried;                 /* wholly under something floating (the mini player, a toast): a finding of its own */
    lv_area_t surf_vis;
};
static struct au_item au_it[AU_MAX];
static int au_n, au_order, au_floor;
static int au_sw, au_sh;          /* the display as LVGL sees it (Cover Flow turns it) */
static FILE *au_tsv;
static const char *au_dir;
static char au_screen[64], au_look[64];
static int au_rule_n[5], au_total, au_allow[18], au_defer[4];
static long au_glyphs, au_glyph_missing;   /* letters read for a glyph, and those with none */
static long au_syms, au_sym_foreign;      /* symbols on screen, and those not drawn by sl_sym */
static bool au_shot_due;
static int au_screen_findings;
static struct { lv_area_t a; char rule; } au_marks[64];
static int au_nmarks;
/* first sightings: one marked screenshot per (screen, rule, text) */
#define AU_SEEN 8192
static uint32_t au_seen[AU_SEEN];
static int au_nseen;
static uint32_t au_hash(const char *s, uint32_t h) { while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static bool au_first(uint32_t h) {
    for (int i = 0; i < au_nseen; i++) if (au_seen[i] == h) return false;
    if (au_nseen < AU_SEEN) au_seen[au_nseen++] = h;
    return true;
}

static bool au_floats(lv_obj_t *o);
static bool au_area_ok(const lv_area_t *a) { return a->x2 >= a->x1 && a->y2 >= a->y1; }
static bool au_and(lv_area_t *r, const lv_area_t *a, const lv_area_t *b) {
    r->x1 = LV_MAX(a->x1, b->x1); r->y1 = LV_MAX(a->y1, b->y1);
    r->x2 = LV_MIN(a->x2, b->x2); r->y2 = LV_MIN(a->y2, b->y2);
    return au_area_ok(r);
}
static bool au_inside(const lv_area_t *in, const lv_area_t *out) {
    return in->x1 >= out->x1 && in->y1 >= out->y1 && in->x2 <= out->x2 && in->y2 <= out->y2;
}
static bool au_is_anc(lv_obj_t *a, lv_obj_t *o) { for (o = lv_obj_get_parent(o); o; o = lv_obj_get_parent(o)) if (o == a) return true; return false; }

static bool au_symbol_only(const char *s) {
    bool any = false;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ') { s++; continue; }
        if ((c == 0xee || c == 0xef) && ((unsigned char)s[1] & 0xc0) == 0x80) {   /* U+E000..U+FFFF: the symbol fonts (sl_sym_<px>) */
            uint32_t cp = ((c & 0x0f) << 12) | (((unsigned char)s[1] & 0x3f) << 6) | ((unsigned char)s[2] & 0x3f);
            if (cp >= 0xe000 && cp <= 0xf8ff) { s += 3; any = true; continue; }
        }
        return false;
    }
    return any;
}
static bool au_has_click_cb(lv_obj_t *o) {
    uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        uint32_t f = d->filter & ~LV_EVENT_PREPROCESS;
        if (f == LV_EVENT_CLICKED || f == LV_EVENT_SHORT_CLICKED || f == LV_EVENT_PRESSED || f == LV_EVENT_RELEASED ||
            f == LV_EVENT_LONG_PRESSED || f == LV_EVENT_ALL || f == LV_EVENT_VALUE_CHANGED) return true;
    }
    return false;
}
static bool au_is_sl_icon(lv_obj_t *o) {
    uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++) if (lv_obj_get_event_dsc(o, i)->cb == sl_icon_draw) return true;
    return false;
}
static bool au_is_art(lv_obj_t *o) {
    return lv_obj_check_type(o, &lv_image_class) || lv_obj_check_type(o, &lv_canvas_class);
}
static bool au_is_ctrl_class(lv_obj_t *o) {
    return lv_obj_check_type(o, &lv_button_class) || lv_obj_check_type(o, &lv_slider_class) ||
           lv_obj_check_type(o, &lv_switch_class) || lv_obj_check_type(o, &lv_checkbox_class) ||
           lv_obj_check_type(o, &lv_textarea_class) || lv_obj_check_type(o, &lv_buttonmatrix_class) ||
           lv_obj_check_type(o, &lv_roller_class) || lv_obj_check_type(o, &lv_dropdown_class) ||
           lv_obj_check_type(o, &lv_arc_class);
}
static bool au_semibold(const lv_font_t *f) {
    for (int s = 0; s < SL_TEXT_STEPS; s++) for (int k = 2; k < 6; k++) if (sl_fonts[s][k] == f) return true;
    return f == &ss_semibold_30 || f == &ss_semibold_34 || f == &ss_semibold_38 || f == &ss_bold_28 || f == &ss_bold_56;
}
static bool au_in_chrome(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) if (o == sb || o == mini) return true;
    return false;
}
/* Playroom boards: a page whose game draws its own world */
static bool au_in_game_art(lv_obj_t *o) {
    for (; o; o = lv_obj_get_parent(o)) if (au_is_art(o)) return true;
    return false;
}

/* A2: a generated cover (a square with the album's gradient) prints the
 * title as part of the picture; it is repeated, readable, under it */
static bool au_on_cover(lv_obj_t *o) {
    lv_obj_t *p = lv_obj_get_parent(o);
    return p && lv_obj_get_style_bg_grad_dir(p, LV_PART_MAIN) == LV_GRAD_DIR_VER && lv_obj_get_width(p) == lv_obj_get_height(p) &&
           lv_obj_get_width(p) >= 48;
}
/* the words' extent inside a label, as LVGL lays them out: across, the
 * aligned line widths; down, the glyphs' own ink from the first line's top
 * to the last shown line's bottom (a line box is taller than its letters) */
static void au_ink(const char *t, const lv_font_t *f, int *above, int *below) {
    *above = *below = 0; uint32_t i = 0;
    while (t[i]) {
        uint32_t letter = lv_text_encoded_next(t, &i), j = i, next = t[i] ? lv_text_encoded_next(t, &j) : 0;
        if (letter == '\n' || letter == ' ') continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, letter, next) || !g.box_h) continue;
        *above = LV_MAX(*above, g.box_h + g.ofs_y); *below = LV_MAX(*below, -g.ofs_y);
    }
}
static void au_label_text(lv_obj_t *l, lv_area_t *full, int *ink_bottom_all, int *line_ink_h) {
    lv_area_t c; lv_obj_get_content_coords(l, &c);
    const char *t = lv_label_get_text(l);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    int ls = lv_obj_get_style_text_letter_space(l, LV_PART_MAIN), lns = lv_obj_get_style_text_line_space(l, LV_PART_MAIN);
    int cw = lv_area_get_width(&c), ch = lv_area_get_height(&c);
    lv_label_long_mode_t lm = lv_label_get_long_mode(l);
    int max_w = (lm == LV_LABEL_LONG_SCROLL || lm == LV_LABEL_LONG_SCROLL_CIRCULAR) ? LV_COORD_MAX : cw;
    if (lv_obj_get_style_width(l, LV_PART_MAIN) == LV_SIZE_CONTENT) max_w = LV_COORD_MAX;
    lv_point_t z; lv_text_get_size(&z, t, f, ls, lns, max_w, LV_TEXT_FLAG_NONE);
    int lh = lv_font_get_line_height(f), pitch = lh + lns;
    int lines = LV_MAX(1, (z.y + lns + pitch / 2) / pitch);
    int shown = LV_MAX(1, LV_MIN(lines, (ch + lns) / pitch));
    int above, below; au_ink(t, f, &above, &below);
    if (!above && !below) { above = lh - f->base_line; below = f->base_line; }
    int base0 = c.y1 + lh - f->base_line;
    int w = LV_MIN(z.x, cw);
    lv_text_align_t al = lv_obj_calculate_style_text_align(l, LV_PART_MAIN, t);
    full->y1 = base0 - above;
    full->y2 = base0 + (shown - 1) * pitch + below - 1;
    if (al == LV_TEXT_ALIGN_CENTER) full->x1 = c.x1 + (cw - w) / 2;
    else if (al == LV_TEXT_ALIGN_RIGHT) full->x1 = c.x2 - w + 1;
    else full->x1 = c.x1;
    full->x2 = full->x1 + w - 1;
    *ink_bottom_all = base0 + (lines - 1) * pitch + below - 1;
    *line_ink_h = lh - f->base_line + below;   /* from the box top to one line's lowest ink */
}

static int au_eff_bg_opa(lv_obj_t *o) {
    return lv_obj_get_style_bg_opa(o, LV_PART_MAIN);
}
static void au_walk(lv_obj_t *o, const lv_area_t *clip, int opa, int surf, const lv_area_t *surf_vis) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    opa = opa * lv_obj_get_style_opa(o, LV_PART_MAIN) / 255;
    opa = opa * lv_obj_get_style_opa_layered(o, LV_PART_MAIN) / 255;
    if (opa < 8) return;
    lv_area_t box, vis; lv_obj_get_coords(o, &box);
    bool shown = au_and(&vis, &box, clip);
    int ord = au_order++;
    if (au_n < AU_MAX) {
        struct au_item *it = &au_it[au_n];
        memset(it, 0, sizeof *it);
        it->o = o; it->order = ord; it->opa = opa; it->box = box; it->vis = vis; it->surf = surf; it->surf_vis = *surf_vis;
        it->chrome = au_in_chrome(o);
        if (lv_obj_check_type(o, &lv_label_class)) {
            const char *t = lv_label_get_text(o);
            bool blank = true; for (const char *s = t; *s; s++) if (*s != ' ' && *s != '\n') blank = false;
            if (!blank) {
                int inkb, lih; lv_area_t full; au_label_text(o, &full, &inkb, &lih);
                it->kind = au_symbol_only(t) ? AU_ICON : AU_LABEL;
                it->opa = it->opa * lv_obj_get_style_text_opa(o, LV_PART_MAIN) / 255;   /* faint words fade like faint objects (A8) */
                lv_obj_t *pp = lv_obj_get_parent(o);   /* A2: the note on a placeholder cover is the cover's art */
                if (it->kind == AU_ICON && pp && lv_obj_get_child_count(pp) == 1 && au_eff_bg_opa(pp) >= 200 &&
                    lv_obj_get_width(pp) >= 40 && LV_ABS(lv_obj_get_width(pp) - lv_obj_get_height(pp)) <= lv_obj_get_width(pp) / 10)
                    it->kind = AU_ART;
                if (!au_and(&it->txt, &full, &vis)) it->kind = 0;
                const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
                int lh = lv_font_get_line_height(f);
                it->large = lh >= 31 || (au_semibold(f) && lh >= 22);
            }
        } else if (au_is_sl_icon(o)) {
            it->kind = AU_ICON; it->txt = vis;
            lv_obj_t *pp = lv_obj_get_parent(o);   /* A2: the drawn placeholder on a big empty cover is the cover */
            if (pp && lv_obj_get_width(pp) >= 100 && LV_ABS(lv_obj_get_width(pp) - lv_obj_get_height(pp)) <= lv_obj_get_width(pp) / 10 &&
                au_eff_bg_opa(pp) >= 200 && lv_obj_get_child_count(pp) <= 3) it->kind = AU_ART;
        } else if (au_is_art(o)) it->kind = AU_ART;
        bool clickable = lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(o, LV_STATE_DISABLED);
        if (clickable && (au_has_click_cb(o) || au_is_ctrl_class(o))) it->kind |= AU_CTRL;
        if (!lv_obj_check_type(o, &lv_label_class) && au_eff_bg_opa(o) >= 200 && opa >= 200 &&
            lv_area_get_size(&box) >= 400) { it->kind |= AU_SURF; it->surf = ord; it->surf_vis = vis; }
        /* the floor: a full-screen surface or catcher hides all before it, and
         * so does a sheet (full width, up from the bottom, over half the screen) */
        bool wide = shown && lv_area_get_width(&vis) >= au_sw * 95 / 100 && o != lv_layer_top() && o != lv_layer_sys();
        bool full = wide && lv_area_get_height(&vis) >= au_sh * 95 / 100;
        bool sheet = wide && vis.y2 >= au_sh - 2 && lv_area_get_height(&vis) >= au_sh * 55 / 100;
        if ((full && (au_eff_bg_opa(o) >= 60 || clickable) && opa >= 128) || (sheet && au_eff_bg_opa(o) >= 200 && opa >= 200))
            au_floor = ord;
        if (it->kind && shown) au_n++;
    }
    if (!shown && !lv_obj_has_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) return;
    lv_area_t cc = lv_obj_has_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE) ? *clip : vis;
    int csurf = surf; lv_area_t csv = *surf_vis;
    if (!lv_obj_check_type(o, &lv_label_class) && au_eff_bg_opa(o) >= 200 && opa >= 200 && lv_area_get_size(&box) >= 400) { csurf = ord; csv = vis; }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) au_walk(lv_obj_get_child(o, i), &cc, opa, csurf, &csv);
}
static void au_collect(void) {
    au_n = 0; au_order = 0; au_floor = -1;
    lv_display_t *d = lv_display_get_default();
    au_sw = lv_display_get_horizontal_resolution(d); au_sh = lv_display_get_vertical_resolution(d);
    lv_area_t scr = { 0, 0, au_sw - 1, au_sh - 1 };
    lv_obj_update_layout(lv_screen_active());
    au_walk(lv_screen_active(), &scr, 255, -1, &scr);
    au_walk(lv_layer_top(), &scr, 255, -1, &scr);
    au_walk(lv_layer_sys(), &scr, 255, -1, &scr);
}

/* a short path: root, then each child's index and the class */
static void au_path(lv_obj_t *o, char *out, int cap) {
    lv_obj_t *chain[32]; int n = 0;
    for (lv_obj_t *p = o; p && n < 32; p = lv_obj_get_parent(p)) chain[n++] = p;
    lv_obj_t *root = chain[n - 1];
    int k = snprintf(out, cap, "%s", root == lv_layer_top() ? "top" : root == lv_layer_sys() ? "sys" : "scr");
    for (int i = n - 2; i >= 0 && k < cap - 16; i--) {
        lv_obj_t *c = chain[i];
        const char *cls = lv_obj_check_type(c, &lv_label_class) ? "L" : lv_obj_check_type(c, &lv_image_class) ? "I" :
                          lv_obj_check_type(c, &lv_slider_class) ? "S" : lv_obj_check_type(c, &lv_textarea_class) ? "T" : "";
        k += snprintf(out + k, cap - k, "/%d%s", (int)lv_obj_get_index(c), cls);
    }
    if (o == sb || au_is_anc(sb, o)) strlcat(out, " (status bar)", cap);
    if (o == mini || au_is_anc(mini, o)) strlcat(out, " (mini)", cap);
}
static void au_words(lv_obj_t *o, char *out, int cap) {
    out[0] = 0;
    const char *t = NULL;
    if (lv_obj_check_type(o, &lv_label_class)) t = lv_label_get_text(o);
    else {   /* a control: its first words */
        lv_obj_t *st[64]; int n = 0; st[n++] = o;
        while (n && !t) { lv_obj_t *c = st[--n];
            if (lv_obj_check_type(c, &lv_label_class) && lv_label_get_text(c)[0] && !au_symbol_only(lv_label_get_text(c))) { t = lv_label_get_text(c); break; }
            for (int i = (int)lv_obj_get_child_count(c) - 1; i >= 0 && n < 64; i--) st[n++] = lv_obj_get_child(c, i); }
        if (!t) t = au_is_sl_icon(o) ? "(icon)" : "";
    }
    int k = 0;
    for (const char *s = t; *s && k < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (k > 56 && (c & 0xc0) != 0x80) { strcpy(out + k, "..."); k += 3; break; }
        out[k++] = (c == '\n' || c == '\t') ? ' ' : (char)c;
    }
    out[k] = 0;
    if (au_symbol_only(t)) snprintf(out, cap, "(symbol)");
}
static void au_report2(char rule, lv_obj_t *o, const lv_area_t *a, const lv_area_t *b, const char *detail);
static void au_report(char rule, lv_obj_t *o, const lv_area_t *a, const lv_area_t *b, const char *detail) {
    au_report2(rule, o, a, b, detail);
}
/* the same, with the other object's rect in its column */
static void au_report2(char rule, lv_obj_t *o, const lv_area_t *a, const lv_area_t *b, const char *detail) {
    char path[200], words[96];
    au_path(o, path, sizeof path); au_words(o, words, sizeof words);
    au_rule_n[rule - 'a']++; au_total++; au_screen_findings++;
    if (au_tsv)
    {   char other[64] = "-";
        if (b) snprintf(other, sizeof other, "%d,%d,%d,%d", (int)b->x1, (int)b->y1, (int)b->x2, (int)b->y2);
        fprintf(au_tsv, "%s\t%s\t%c\t%s\t\"%s\"\t%d,%d,%d,%d\t%s\t%s\n", au_screen, au_look, rule, path, words,
                (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2, other, detail); }
    if (au_nmarks < 63) { au_marks[au_nmarks].a = *a; au_marks[au_nmarks++].rule = rule; if (b) { au_marks[au_nmarks].a = *b; au_marks[au_nmarks++].rule = rule; } }
    char sig[400]; snprintf(sig, sizeof sig, "%s|%c|%s|%s", au_screen, rule, words, detail);
    if (au_first(au_hash(sig, 2166136261u))) au_shot_due = true;
}

/* A17: b is a count badge's mark on the button whose glyph is o */
static bool au_badge(lv_obj_t *b, lv_obj_t *o) {
    if (!lv_obj_check_type(b, &lv_label_class) || strlen(lv_label_get_text(b)) > 2) return false;
    lv_obj_t *disc = lv_obj_get_parent(b);
    return disc && lv_obj_get_width(disc) <= 24 && lv_obj_get_height(disc) <= 24 &&
           lv_obj_get_parent(disc) && lv_obj_get_parent(disc) == lv_obj_get_parent(o);
}
/* A17 too: disc is the badge itself, drawn over the glyph o beside it */
static bool au_badge_disc(lv_obj_t *disc, lv_obj_t *o) {
    lv_obj_t *mark = lv_obj_get_child_count(disc) == 1 ? lv_obj_get_child(disc, 0) : NULL;
    return mark && au_badge(mark, o);
}
/* the nearest ancestor that scrolls along an axis */
static lv_obj_t *au_scroller(lv_obj_t *o, lv_dir_t dir) {
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p))
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) && (lv_obj_get_scroll_dir(p) & dir)) return p;
    return NULL;
}
/* A7: o sits in a list that can still scroll it out from under r */
static bool au_scrolls_clear(lv_obj_t *o, lv_obj_t *over, const lv_area_t *r) {
    lv_obj_t *s = au_scroller(o, LV_DIR_VER);
    if (!s || over == s || au_is_anc(s, over)) return false;
    lv_area_t sa; lv_obj_get_coords(s, &sa);
    int mid = (sa.y1 + sa.y2) / 2;
    if ((r->y1 + r->y2) / 2 >= mid) return lv_obj_get_scroll_bottom(s) > 0;
    return lv_obj_get_scroll_top(s) > 0;
}

/* rule b: the words fit their box and every box around it */
/* A16: a letter of the GabagoolOS mark, and its red drop layer */
static bool au_logo(lv_obj_t *o) {
    if (!lv_obj_check_type(o, &lv_label_class)) return false;
    const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    return f == &sl_brand_34 || f == &sl_brand_50;
}
static bool au_logo_drop(lv_obj_t *o) {
    return au_logo(o) && (lv_color_to_u32(lv_obj_get_style_text_color(o, LV_PART_MAIN)) & 0xffffff) == 0xb3261e;
}
static void au_rule_b(struct au_item *it) {
    lv_obj_t *l = it->o;
    if (!lv_obj_check_type(l, &lv_label_class) || it->kind & AU_ICON) return;
    lv_label_long_mode_t lm = lv_label_get_long_mode(l);
    lv_area_t full; int inkb, lih; au_label_text(l, &full, &inkb, &lih);
    lv_area_t c; lv_obj_get_content_coords(l, &c);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    if (sl_zune() && f == &ss_regular_104) { au_allow[6]++; return; }   /* A6 */
    /* LVGL draws a label's letters a quarter line past its box (its extra
     * draw area), cut only by the boxes around it; a marquee is cut to its box */
    int ext = LV_MAX(lv_font_get_line_height(f) / 4, lv_obj_get_ext_draw_size(l));
    bool marquee = lm == LV_LABEL_LONG_SCROLL || lm == LV_LABEL_LONG_SCROLL_CIRCULAR;
    if (marquee) au_allow[4]++;
    else {
        int limit = it->box.y2 + ext, bottom = lm == LV_LABEL_LONG_DOT ? full.y2 : inkb;
        if (bottom > limit + 1) {
            char d[96]; snprintf(d, sizeof d, "letters cut at the bottom of the box: %d px", bottom - limit);
            au_report('b', l, &full, NULL, d);
        }
        else if (lm == LV_LABEL_LONG_DOT && inkb > c.y2 + 1) au_allow[5]++;
        if (full.y1 < it->box.y1 - ext - 1) {
            char d[96]; snprintf(d, sizeof d, "letters cut at the top of the box: %d px", (int)(it->box.y1 - ext - full.y1));
            au_report('b', l, &full, NULL, d);
        }
    }
    /* the boxes around it */
    lv_area_t drawn = it->box;
    if (!marquee) lv_area_increase(&drawn, ext, ext);
    lv_area_t want; if (!au_and(&want, &full, &drawn)) return;
    lv_area_t cut = want;
    for (lv_obj_t *p = lv_obj_get_parent(l); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
        lv_area_t pa; lv_obj_get_coords(p, &pa);
        if (au_inside(&cut, &pa)) continue;
        bool hor = cut.x1 < pa.x1 || cut.x2 > pa.x2, ver = cut.y1 < pa.y1 || cut.y2 > pa.y2;
        lv_obj_t *sh = au_scroller(l, LV_DIR_HOR), *sv = au_scroller(l, LV_DIR_VER);
        bool scrolled = (!hor || (sh && (sh == p || au_is_anc(p, sh)))) && (!ver || (sv && (sv == p || au_is_anc(p, sv))));
        if (scrolled) { au_allow[7]++; return; }
        lv_area_t in; if (!au_and(&in, &cut, &pa)) return;   /* wholly outside: not on screen */
        char d[96]; snprintf(d, sizeof d, "cut by a %s box that does not scroll (%dx%d)", hor && ver ? "smaller" : hor ? "narrower" : "shorter",
                             (int)lv_area_get_width(&pa), (int)lv_area_get_height(&pa));
        au_report2('b', l, &want, &pa, d);
        return;
    }
    lv_area_t scr = { 0, 0, au_sw - 1, au_sh - 1 };
    if (!au_inside(&cut, &scr) && !au_scroller(l, LV_DIR_HOR)) au_report2('b', l, &cut, &scr, "runs off the screen");
}

/* the words' own surface: the nearest ancestor painted one solid colour,
 * nothing between (a gradient, art or glass: read the pixels instead) */
static bool au_solid_bg(lv_obj_t *o, uint32_t *out) {
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        if (au_is_art(p) || au_is_sl_icon(p)) return false;
        if (lv_obj_get_style_opa(p, LV_PART_MAIN) < 250) return false;
        int bo = lv_obj_get_style_bg_opa(p, LV_PART_MAIN);
        if (bo == 0) continue;
        if (bo < 250 || lv_obj_get_style_bg_grad_dir(p, LV_PART_MAIN) != LV_GRAD_DIR_NONE ||
            lv_obj_get_event_count(p) && p == lv_screen_active()) return false;
        for (uint32_t k = 0; k < lv_obj_get_event_count(p); k++)   /* drawn by hand (a wallpaper, a board) */
            if ((lv_obj_get_event_dsc(p, k)->filter & ~LV_EVENT_PREPROCESS) == LV_EVENT_DRAW_MAIN) return false;
        *out = lv_color_to_u32(lv_obj_get_style_bg_color(p, LV_PART_MAIN)) & 0xffffff;
        return true;
    }
    return false;
}
/* A13: an icon on a button or row that also has its words is decoration
 * (WCAG 1.4.11 asks 3:1 only of graphics needed to understand the control) */
static bool au_has_words(lv_obj_t *o) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(o, &lv_label_class)) { const char *t = lv_label_get_text(o); return t[0] && !au_symbol_only(t); }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) if (au_has_words(lv_obj_get_child(o, i))) return true;
    return false;
}
static bool au_icon_labelled(lv_obj_t *o) {
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p))
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE) && au_has_click_cb(p)) return lv_obj_get_width(p) < au_sw * 9 / 10 + 40 && au_has_words(p);
    return false;
}
/* rule e: WCAG contrast, the words against what is under them */
static uint32_t au_px(int x, int y) {
    int px = x, py = y;
    if (au_sw > au_sh) { px = y; py = H - 1 - x; }   /* Cover Flow is landscape */
    if (px < 0 || py < 0 || px >= W || py >= H) return 0;
    unsigned p = pixels[py * W + px];
    return ((((p >> 11) & 31) * 255 / 31) << 16) | ((((p >> 5) & 63) * 255 / 63) << 8) | ((p & 31) * 255 / 31);
}
static double au_lin(int c) { double v = c / 255.0; return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double au_lum(uint32_t c) { return 0.2126 * au_lin((c >> 16) & 255) + 0.7152 * au_lin((c >> 8) & 255) + 0.0722 * au_lin(c & 255); }
static uint32_t au_mix(uint32_t fg, uint32_t bg, int opa) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) r |= ((((fg >> s) & 255) * opa + ((bg >> s) & 255) * (255 - opa)) / 255) << s;
    return r;
}
static uint32_t au_bg_mode(const lv_area_t *a) {
    static uint16_t keys[4096]; static int counts[4096]; int n = 0;
    int step = lv_area_get_size(a) > 20000 ? 3 : 1;
    for (int y = a->y1; y <= a->y2; y += step) for (int x = a->x1; x <= a->x2; x += step) {
        uint32_t c = au_px(x, y); uint16_t k = (uint16_t)(((c >> 20) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 4) & 15));
        int i = 0; while (i < n && keys[i] != k) i++;
        if (i == n) { if (n == 4096) continue; keys[n] = k; counts[n++] = 0; }
        counts[i]++;
    }
    int best = 0; for (int i = 1; i < n; i++) if (counts[i] > counts[best]) best = i;
    /* back to a colour: the average of the pixels in the winning bucket */
    long sr = 0, sg = 0, sbb = 0, cnt = 0;
    for (int y = a->y1; y <= a->y2; y += step) for (int x = a->x1; x <= a->x2; x += step) {
        uint32_t c = au_px(x, y); uint16_t k = (uint16_t)(((c >> 20) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 4) & 15));
        if (n && k == keys[best]) { sr += (c >> 16) & 255; sg += (c >> 8) & 255; sbb += c & 255; cnt++; }
    }
    if (!cnt) return 0;
    return (uint32_t)((sr / cnt) << 16 | (sg / cnt) << 8 | (sbb / cnt));
}
static uint32_t au_bg_ring(const lv_area_t *outer, const lv_area_t *inner) {
    static uint16_t keys[1024]; static int counts[1024]; static long sum[1024][3]; int n = 0;
    for (int y = outer->y1; y <= outer->y2; y++) for (int x = outer->x1; x <= outer->x2; x++) {
        if (x >= inner->x1 && x <= inner->x2 && y >= inner->y1 && y <= inner->y2) continue;
        if (x < 0 || y < 0 || x >= au_sw || y >= au_sh) continue;
        uint32_t c = au_px(x, y); uint16_t k = (uint16_t)(((c >> 20) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 4) & 15));
        int i = 0; while (i < n && keys[i] != k) i++;
        if (i == n) { if (n == 1024) continue; keys[n] = k; counts[n] = 0; sum[n][0] = sum[n][1] = sum[n][2] = 0; n++; }
        counts[i]++; sum[i][0] += (c >> 16) & 255; sum[i][1] += (c >> 8) & 255; sum[i][2] += c & 255;
    }
    if (!n) return au_bg_mode(inner);
    int b = 0; for (int i = 1; i < n; i++) if (counts[i] > counts[b]) b = i;
    return (uint32_t)((sum[b][0] / counts[b]) << 16 | (sum[b][1] / counts[b]) << 8 | (sum[b][2] / counts[b]));
}
static void au_rule_e(struct au_item *it) {
    lv_obj_t *o = it->o;
    if (au_in_game_art(o)) { au_allow[10]++; return; }
    if (au_on_cover(o)) { au_allow[2]++; return; }
    if ((lyr_panel && au_is_anc(lyr_panel, o) && (lyr_cur < 0 || o != lyr_rows[lyr_cur])) ||
        (o && (o == lyr_sample[0] || o == lyr_sample[2]))) { au_allow[12]++; return; }   /* A12, and the Lyrics page's preview of them */
    if (it->kind & AU_ICON && au_icon_labelled(o)) { au_allow[13]++; return; }   /* A13 */
    if (au_logo_drop(o)) { au_allow[16]++; return; }                                  /* A16 */
    for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p))   /* A15: an inactive control (WCAG exempts it) */
        if (lv_obj_has_state(p, LV_STATE_DISABLED)) { au_allow[15]++; return; }
    /* deferred, real, and counted apart (see AUDIT-2026-10-21.md) */
    if (qp && (o == qp || au_is_anc(qp, o))) { au_defer[1]++; return; }            /* D1 the control centre's glass */
    uint32_t solid;
    if ((au_solid_bg(o, &solid) && solid == ZN_PINK) || (accent_preview && (o == accent_preview || au_is_anc(accent_preview, o)))) { au_defer[3]++; return; }   /* D3 */
    uint32_t fg = lv_color_to_u32(lv_obj_get_style_text_color(o, LV_PART_MAIN)) & 0xffffff;
    int opa = it->opa;   /* the object's and the words' own opacity (au_walk) */
    if (opa < 64) { au_allow[8]++; return; }
    /* what is under the words: a ring just outside them, so a solid glyph
     * is not taken for its own background; clipped to the surface it sits on */
    lv_area_t ring = { it->txt.x1 - 4, it->txt.y1 - 4, it->txt.x2 + 4, it->txt.y2 + 4 }, keep;
    if (au_and(&keep, &ring, &it->surf_vis)) ring = keep;
    uint32_t bg;
    bool over_art = false;   /* drawn over a sibling's art or surface (a game's stage): read the pixels */
    lv_area_t ov;
    for (int j = 0; j < au_n && !over_art; j++)
        if (au_it[j].order < it->order && au_it[j].kind & (AU_ART | AU_SURF) && !au_is_anc(au_it[j].o, o) &&
            au_and(&ov, &it->txt, &au_it[j].vis)) over_art = true;
    if (over_art || !au_solid_bg(o, &bg)) bg = au_bg_ring(&ring, &it->txt);
    if (over_art) {   /* A10: a game's own words over its board (a zone name, a score popping up) */
        for (int j = 0; j < au_n; j++)
            if (au_it[j].order < it->order && lv_obj_check_type(au_it[j].o, &lv_canvas_class) && au_and(&ov, &it->txt, &au_it[j].vis)) { au_allow[10]++; return; }
    }
    uint32_t seen = au_mix(fg, bg, opa);
    double l1 = au_lum(seen), l2 = au_lum(bg), r = (LV_MAX(l1, l2) + 0.05) / (LV_MIN(l1, l2) + 0.05);
    double need = (it->kind & AU_ICON) || it->large ? 3.0 : 4.5;
    if (r + 0.005 < need) {
        char d[96]; snprintf(d, sizeof d, "contrast %.2f:1 (needs %.1f) text #%06x on #%06x", r, need, (unsigned)seen, (unsigned)bg);
        au_report('e', o, &it->txt, NULL, d);
    }
}

/* rule d: something to tap is at least 44 px each way */
static bool au_draws(lv_obj_t *o) {
    if (au_eff_bg_opa(o) > 0 || lv_obj_get_style_border_width(o, LV_PART_MAIN) > 0 || au_is_sl_icon(o) ||
        lv_obj_check_type(o, &lv_label_class) || lv_obj_check_type(o, &lv_image_class) || au_is_ctrl_class(o)) return true;
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) { lv_obj_t *c = lv_obj_get_child(o, i); if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && au_draws(c)) return true; }
    return false;
}
static void au_rule_d(struct au_item *it) {
    lv_obj_t *o = it->o;
    int pad = o->spec_attr ? o->spec_attr->ext_click_pad : 0;
    int w = lv_area_get_width(&it->box) + 2 * pad, h = lv_area_get_height(&it->box) + 2 * pad;
    if (lv_obj_check_type(o, &lv_slider_class)) { w = LV_MAX(w, 44); h += 2 * lv_obj_get_style_pad_top(o, LV_PART_KNOB) + 8; }
    if (w >= au_sw * 9 / 10 && h >= au_sh * 9 / 10) { au_allow[9]++; return; }
    if (!au_draws(o)) return;
    if (w < AU_MIN_TAP || h < AU_MIN_TAP) {
        for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p))   /* A11: the row around it takes the tap */
            if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE) && au_has_click_cb(p) && lv_obj_get_width(p) >= AU_MIN_TAP &&
                lv_obj_get_height(p) >= AU_MIN_TAP && lv_obj_get_width(p) < au_sw * 9 / 10 + 40 && lv_obj_get_height(p) < au_sh / 2) { au_allow[11]++; return; }
        char d[64]; snprintf(d, sizeof d, "tap target %dx%d px", w, h);
        au_report('d', o, &it->box, NULL, d);
    }
}

/* rules a and c between two things on screen */
static bool au_hit(const lv_area_t *a, const lv_area_t *b, lv_area_t *r) {
    return au_and(r, a, b) && lv_area_get_width(r) >= 3 && lv_area_get_height(r) >= 3;
}
static void au_corner(struct au_item *it) {
    const lv_area_t *t = &it->txt; int R = AU_CORNER_R;
    lv_obj_t *sv = au_scroller(it->o, LV_DIR_VER);   /* A7: list content passes the corner as it scrolls */
    if (sv && (lv_obj_get_scroll_bottom(sv) > 0 || lv_obj_get_scroll_top(sv) > 0)) return;
    int cx[4] = { R, au_sw - 1 - R, R, au_sw - 1 - R }, cy[4] = { R, R, au_sh - 1 - R, au_sh - 1 - R };
    int px[4] = { t->x1, t->x2, t->x1, t->x2 }, py[4] = { t->y1, t->y1, t->y2, t->y2 };
    for (int k = 0; k < 4; k++) {
        bool in_sq = (k % 2 ? px[k] > cx[k] : px[k] < cx[k]) && (k < 2 ? py[k] < cy[k] : py[k] > cy[k]);
        if (in_sq && (px[k] - cx[k]) * (px[k] - cx[k]) + (py[k] - cy[k]) * (py[k] - cy[k]) > R * R) {
            au_report('c', it->o, t, NULL, "in a rounded screen corner"); return; }
    }
}
/* a later opaque surface that y is not on, and that hides the place r */
static bool au_hidden_by_surface(const struct au_item *lo, const struct au_item *hi, const lv_area_t *r) {
    return hi->surf > lo->surf && hi->surf > lo->order && au_inside(r, &hi->surf_vis);
}
static void au_pairs(bool end_pass, lv_obj_t *only_in) {
    for (int i = 0; i < au_n; i++) {
        struct au_item *x = &au_it[i];
        if (x->order < au_floor || !(x->kind & (AU_LABEL | AU_ICON)) || x->hidden || x->opa < 64) continue;
        if (only_in && !au_is_anc(only_in, x->o)) continue;
        if (!end_pass && x->kind & AU_LABEL && !(sl_zune() && lv_obj_get_style_text_font(x->o, LV_PART_MAIN) == &ss_regular_104)) au_corner(x);
        lv_obj_t *cover_seen = NULL;
        for (int j = 0; j < au_n; j++) {
            if (i == j) continue;
            struct au_item *y = &au_it[j];
            if (y->order < au_floor || y->hidden || y->buried || y->opa < 64) continue;
            if (cover_seen && au_is_anc(cover_seen, y->o)) continue;   /* one report per thing on top */
            if (au_is_anc(y->o, x->o) || au_is_anc(x->o, y->o)) { if (!end_pass) au_allow[1]++; continue; }   /* A1 */
            lv_area_t r;
            if (y->kind == AU_ART) { if (au_hit(&x->txt, &y->vis, &r)) au_allow[2]++; continue; }   /* A2 */
            bool chrome = y->chrome && !x->chrome;
            bool zule_title = sl_zune() && ((x->kind & AU_LABEL && lv_obj_get_style_text_font(x->o, LV_PART_MAIN) == &ss_regular_104) ||
                                            (y->kind & AU_LABEL && lv_obj_get_style_text_font(y->o, LV_PART_MAIN) == &ss_regular_104));
            /* words on words, words on an icon */
            if (y->kind & (AU_LABEL | AU_ICON) && !(x->kind & AU_ICON && y->kind & AU_ICON)) {
                if (j < i && y->kind & AU_LABEL && x->kind & AU_LABEL) continue;   /* each pair once */
                if (!au_hit(&x->txt, &y->txt, &r)) continue;
                if (zule_title) { au_allow[6]++; continue; }
                if (au_logo(x->o) && au_logo(y->o)) { au_allow[16]++; continue; }   /* A16 */
                if (au_badge(x->o, y->o) || au_badge(y->o, x->o)) { au_allow[17]++; continue; }   /* A17 */
                if (au_hidden_by_surface(x, y, &r) || au_hidden_by_surface(y, x, &r)) continue;   /* reported as covered */
                if (au_in_game_art(x->o) || au_in_game_art(y->o)) { au_allow[10]++; continue; }
                if (au_scrolls_clear(x->o, y->o, &r) || au_scrolls_clear(y->o, x->o, &r)) { au_allow[7]++; continue; }
                if (end_pass && !chrome && !y->chrome) continue;
                char d[160], w2[64]; au_words(y->o, w2, sizeof w2);
                snprintf(d, sizeof d, "%s \"%s\"", chrome ? "meets the status bar or mini player:" : y->kind & AU_ICON ? "meets icon" : "meets", w2);
                au_report2(chrome || x->chrome ? 'c' : 'a', x->o, &x->txt, &y->txt, d);
                continue;
            }
            /* a surface drawn later over the words (a floating bar, the mini player, a toast) */
            if (y->kind & AU_SURF && y->order > x->order && y->order > x->surf && au_hit(&x->txt, &y->vis, &r)) {
                /* A14: a toast or banner may pass over list content (it leaves in seconds, a flick sends
                 * it away); never over a fixed control or words that cannot scroll out from under it */
                if ((y->o == pop.obj || y->o == nb.obj || y->o == sc.obj || (pop.obj && au_is_anc(pop.obj, y->o)) || (nb.obj && au_is_anc(nb.obj, y->o)) || (sc.obj && au_is_anc(sc.obj, y->o))) && au_scroller(x->o, LV_DIR_VER) && !au_is_anc(au_scroller(x->o, LV_DIR_VER), y->o)) { au_allow[14]++; continue; }
                if (au_inside(&x->txt, &y->vis) && !au_floats(y->o)) continue;   /* drawn over and wholly hidden: not shown */
                if (zule_title) { au_allow[6]++; continue; }
                if (au_badge_disc(y->o, x->o)) { au_allow[17]++; continue; }   /* A17 */
                if (au_scrolls_clear(x->o, y->o, &r)) { au_allow[7]++; continue; }
                char d[160], w2[64]; au_words(y->o, w2, sizeof w2);
                snprintf(d, sizeof d, "%s%s \"%s\"", end_pass ? "at the end of the list, " : "", y->chrome ? "under the mini player" : "covered by", w2);
                au_report2(y->chrome ? 'c' : 'a', x->o, &x->txt, &y->vis, d);
                cover_seen = y->o;
                continue;
            }
            /* words running onto a control they are not part of */
            if (!end_pass && y->kind & AU_CTRL && x->kind & AU_LABEL && y->order < x->order && !(y->kind & AU_LABEL) &&
                au_eff_bg_opa(y->o) >= 50 && au_hit(&x->txt, &y->vis, &r)) {
                if (lv_area_get_width(&y->vis) >= au_sw * 9 / 10 && lv_area_get_height(&y->vis) >= au_sh * 9 / 10) continue;
                if (x->surf > y->order && au_inside(&r, &x->surf_vis)) continue;   /* on its own surface, over the control */
                if (au_scrolls_clear(x->o, y->o, &r)) { au_allow[7]++; continue; }
                char d[160], w2[64]; au_words(y->o, w2, sizeof w2);
                snprintf(d, sizeof d, "runs onto the control \"%s\"", w2);
                au_report2('a', x->o, &x->txt, &y->vis, d);
            }
        }
    }
}

static void au_mark_and_save(void) {
    if (!au_dir || !au_shot_due) return;
    static uint16_t keep[W * H];
    memcpy(keep, pixels, sizeof keep);
    for (int m = 0; m < au_nmarks; m++) {
        lv_area_t a = au_marks[m].a;
        uint16_t col = au_marks[m].rule == 'e' ? 0xffe0 : au_marks[m].rule == 'd' ? 0x07ff : 0xf81f;
        for (int t = 0; t < 2; t++) {
            for (int x = a.x1 - t; x <= a.x2 + t; x++) {
                int y1 = a.y1 - t, y2 = a.y2 + t;
                if (au_sw > au_sh) continue;
                if (x >= 0 && x < W) { if (y1 >= 0 && y1 < H) pixels[y1 * W + x] = col; if (y2 >= 0 && y2 < H) pixels[y2 * W + x] = col; }
            }
            for (int y = a.y1 - t; y <= a.y2 + t; y++) {
                int x1 = a.x1 - t, x2 = a.x2 + t;
                if (au_sw > au_sh) continue;
                if (y >= 0 && y < H) { if (x1 >= 0 && x1 < W) pixels[y * W + x1] = col; if (x2 >= 0 && x2 < W) pixels[y * W + x2] = col; }
            }
        }
    }
    char nm[600]; snprintf(nm, sizeof nm, "%s/shots/%s__%s.ppm", au_dir, au_screen, au_look);
    shot_now(nm);
    memcpy(pixels, keep, sizeof keep);
}

static bool au_floats(lv_obj_t *o) {
    return lv_obj_get_parent(o) == lv_layer_top() || o == mini || lv_obj_has_flag(o, LV_OBJ_FLAG_FLOATING);
}
/* what a later opaque surface hides completely is not on screen */
static void au_occlusion(void) {
    for (int i = 0; i < au_n; i++) {
        struct au_item *x = &au_it[i];
        const lv_area_t *r = x->kind & (AU_LABEL | AU_ICON) ? &x->txt : &x->vis;
        x->hidden = x->buried = false;
        for (int j = 0; j < au_n && !x->hidden; j++) {
            struct au_item *y = &au_it[j];
            if (!(y->kind & AU_SURF) || y->order <= x->order || y->order <= x->surf || au_is_anc(x->o, y->o)) continue;
            if (!au_inside(r, &y->vis)) continue;
            if (au_floats(y->o)) x->buried = true; else x->hidden = true;
        }
    }
}
static bool au_under_float(const struct au_item *x) {
    lv_area_t r, near = { x->txt.x1 - 4, x->txt.y1 - 4, x->txt.x2 + 4, x->txt.y2 + 4 };   /* the ring read for its background */
    for (int j = 0; j < au_n; j++) {
        const struct au_item *y = &au_it[j];
        /* with its shadow: words in the mini player's shade are darkened with what is under them */
        int e = au_floats(y->o) ? lv_obj_get_ext_draw_size(y->o) : 0;
        lv_area_t shade = { y->vis.x1 - e, y->vis.y1 - e, y->vis.x2 + e, y->vis.y2 + e };
        if (y->kind & AU_SURF && y->order > x->order && y->order > x->surf && au_floats(y->o) && !au_is_anc(y->o, x->o) &&
            au_and(&r, &near, &shade)) return true;
    }
    return false;
}
/* every rule on what the screen shows now */
/* rule c, the mini player: while it shows, nothing to tap is fixed under
 * it (a list may run under it if it scrolls a player's height or more) */
static lv_obj_t *au_ctrl_under_mini(lv_obj_t *o, const lv_area_t *m) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    lv_area_t a; lv_obj_get_coords(o, &a);
    if (a.y2 < m->y1 || a.y1 > m->y2) return NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_top(o) + lv_obj_get_scroll_bottom(o) >= m->y2 - m->y1 + 1 + SL_FLOAT) return NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_area_get_height(&a) < au_sh / 2 &&
        a.x2 >= m->x1 && a.x1 <= m->x2) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) { lv_obj_t *c = au_ctrl_under_mini(lv_obj_get_child(o, i), m); if (c) return c; }
    return NULL;
}
static int au_mini_checked;   /* screens read with the mini player up */
static void au_rule_mini(void) {
    /* the screen as it settles: the player looks again every 250 ms
     * (periodic_ui), not on each change inside a page */
    if (mini && npages >= 2) mini_update();
    if (!mini || lv_obj_has_flag(mini, LV_OBJ_FLAG_HIDDEN) || npages < 2) return;
    au_mini_checked++;
    lv_area_t m; lv_obj_get_coords(mini, &m);
    lv_obj_t *p = pages[npages - 1];
    for (uint32_t i = 0; i < lv_obj_get_child_count(p); i++) {
        lv_obj_t *c = au_ctrl_under_mini(lv_obj_get_child(p, i), &m);
        if (c) { lv_area_t a; lv_obj_get_coords(c, &a); au_report('c', c, &a, &m, "a control fixed under the mini player"); return; }
    }
}
static void au_check(const char *screen) {
    snprintf(au_screen, sizeof au_screen, "%s", screen);
    au_shot_due = false; au_nmarks = 0; au_screen_findings = 0;
    frame(0);
    au_collect();
    au_occlusion();
    for (int i = 0; i < au_n; i++) {
        struct au_item *it = &au_it[i];
        if (it->order < au_floor) { au_allow[3]++; continue; }
        if (it->hidden) { au_allow[3]++; continue; }
        if (it->buried) continue;   /* read by the pairs, as covered */
        if (it->opa < 64) { au_allow[8]++; continue; }
        if (it->kind & (AU_LABEL | AU_ICON)) au_rule_b(it);
        if (it->kind & (AU_LABEL | AU_ICON) && !au_under_float(it)) au_rule_e(it);   /* partly under a toast: read as covered, not for contrast */
        if (it->kind & AU_CTRL) au_rule_d(it);
    }
    au_pairs(false, NULL);
    au_rule_mini();
    /* SLOPPY_AUDIT_WORDS=<file>: every visible label's words, for finding text left in English */
    if (getenv("SLOPPY_AUDIT_WORDS")) {
        static FILE *wf; if (!wf) wf = fopen(getenv("SLOPPY_AUDIT_WORDS"), "w");
        for (int i = 0; wf && i < au_n; i++) {
            struct au_item *it = &au_it[i];
            if (!(it->kind & AU_LABEL) || it->hidden || it->opa < 64 || !lv_obj_check_type(it->o, &lv_label_class)) continue;
            const char *t = lv_label_get_text(it->o); if (!t || !t[0]) continue;
            fprintf(wf, "%s\t%s\t", au_screen, au_look);
            for (; *t; t++) fputc(*t == '\n' ? '|' : *t == '\t' ? ' ' : *t, wf);
            fputc('\n', wf);
        }
        if (wf) fflush(wf);
    }
    /* every letter on screen has its glyph: no box for one a font lacks
     * (Chinese, Cyrillic: read from the card's letter packs) */
    for (int i = 0; i < au_n; i++) {
        struct au_item *it = &au_it[i];
        if (!(it->kind & AU_LABEL) || it->hidden || it->opa < 64 || !lv_obj_check_type(it->o, &lv_label_class)) continue;
        const lv_font_t *f = lv_obj_get_style_text_font(it->o, LV_PART_MAIN);
        const char *t = lv_label_get_text(it->o); uint32_t c;
        while (t && (c = st_utf8_next(&t))) {
            if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0xad || (c >= 0x2000 && c <= 0x200f) || (c >= 0x2028 && c <= 0x202f) || (c >= 0x2060 && c <= 0x206f) ||
                (c >= 0xfe00 && c <= 0xfe0f) || c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb) || (c >= 0xe0000 && c <= 0xe01ef)) continue;   /* nothing to draw */
            lv_font_glyph_dsc_t g; bool ok = lv_font_get_glyph_dsc(f, &g, c, 0);
            if (!ok || g.is_placeholder) {
                if (au_glyph_missing++ < 20) printf("audit: no glyph for U+%04X in \"%s\" (%s, %s)\n", (unsigned)c, lv_label_get_text(it->o), au_screen, au_look);
            }
            au_glyphs++;
        }
    }
    /* every symbol on screen is one of ours (sl_sym_<px>), in a row of words
     * or alone: none left to Montserrat or to a font with no chain to ours */
    for (int i = 0; i < au_n; i++) {
        struct au_item *it = &au_it[i];
        if (it->hidden || it->opa < 64 || !lv_obj_check_type(it->o, &lv_label_class)) continue;
        const lv_font_t *f = lv_obj_get_style_text_font(it->o, LV_PART_MAIN);
        const char *t = lv_label_get_text(it->o); uint32_t c;
        while (t && (c = st_utf8_next(&t))) {
            if (c < 0xe000 || c > 0xf8ff) continue;
            lv_font_glyph_dsc_t g; bool ok = lv_font_get_glyph_dsc(f, &g, c, 0);
            au_syms++;
            if (!ok || !g.resolved_font || g.resolved_font->get_glyph_dsc != sym_dsc)
                if (au_sym_foreign++ < 20) printf("audit: symbol U+%04X not drawn by sl_sym in \"%s\" (%s, %s)\n", (unsigned)c, lv_label_get_text(it->o), au_screen, au_look);
        }
    }
    /* the end of each list: its last words clear what floats over it */
    lv_obj_t *sc[16]; int ns = 0;
    for (int i = 0; i < au_n && ns < 16; i++) {
        lv_obj_t *s = au_it[i].order >= au_floor ? au_scroller(au_it[i].o, LV_DIR_VER) : NULL;
        if (!s || lv_obj_get_scroll_bottom(s) <= 0) continue;
        bool dup = false; for (int k = 0; k < ns; k++) if (sc[k] == s) dup = true;
        if (!dup) sc[ns++] = s;
    }
    au_mark_and_save();
    for (int k = 0; k < ns; k++) {
        lv_obj_t *s = sc[k];
        int y0 = lv_obj_get_scroll_y(s), down = lv_obj_get_scroll_bottom(s);
        if (down > 200000) continue;
        lv_obj_scroll_by(s, 0, -down, LV_ANIM_OFF);
        frame(50); frame(50);
        au_nmarks = 0; au_shot_due = false;
        char nm[80]; snprintf(nm, sizeof nm, "%s@end", screen);
        snprintf(au_screen, sizeof au_screen, "%s", nm);
        au_collect();
        au_occlusion();
        au_pairs(true, s);
        au_mark_and_save();
        snprintf(au_screen, sizeof au_screen, "%s", screen);
        lv_obj_scroll_to_y(s, y0, LV_ANIM_OFF); frame(50);
    }
}

/* ------------------------------------------------------------ the walk */
static const struct { const char *name; int theme, home; } au_looks[] = {
    { "cards-dark", 0, 0 }, { "cards-light", 1, 0 }, { "tiles-dark", 0, 1 }, { "tiles-light", 1, 1 },
    { "list-dark", 0, 2 }, { "list-light", 1, 2 }, { "zule", 3, 2 },
};
static const char *const au_lang_names[SL_LANG_LAST + 1] = { "en", "es", "fr", "de", "he", "ar", "zh", "it", "nl", "pt", "pl", "cs", "ru", "ja" };
static const char *const au_step_names[] = { "std", "large", "larger", "largest" };

static void au_settle(void) { for (int i = 0; i < 6; i++) frame(200); }
/* the audit library: ten albums of eight songs; for the screen audit and
 * the script shots (au_scripts) one each in Korean, Japanese and Thai of
 * four more, their letters from the card's packs */
enum { AU_NA = 13, AU_NT = 10 * 8 + 3 * 4 };
static bool au_scripts;
/* Library options' badges, on covers of every kind: 24/96 and 16/44.1 FLAC
 * headers, a lossy album and a DSD one; off again, as they were */
static bool au_badged;
static struct sl_track au_tr[AU_NT];
static char au_paths[AU_NT][32];
static void au_badges(bool on) {
    global_settings.sloppy_album_badges = on; global_settings.sloppy_album_no_artist = 0;
    album_q_lib = NULL;   /* read again */
    if (on && !au_badged) {
        static const unsigned char q[2][4] = { { 0x17, 0x70, 0x01, 0x70 }, { 0x0a, 0xc4, 0x40, 0xf0 } };   /* 96 kHz 24 bit, 44.1 kHz 16 bit */
        for (int a = 0; a < 2; a++) {
            unsigned char h[36] = "fLaC"; h[4] = 0; h[18] = q[a][0]; h[19] = q[a][1]; h[20] = q[a][2]; h[21] = q[a][3];
            int fd = open(TPT(&au_tr[a * 8]), O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd >= 0) { write(fd, h, sizeof h); sim_close(fd); }
        }
        au_tr[16].lossless = false; strlcpy(au_paths[32] + strlen(au_paths[32]) - 5, ".dsf", 6);
    }
    else if (!on && au_badged) { au_tr[16].lossless = true; strlcpy(au_paths[32] + strlen(au_paths[32]) - 4, "flac", 5); }
    au_badged = on;
}
/* the play menu, opened from the header's "more" (its first button) */
static void au_play_menu(void (*open_page)(void)) {
    open_page(); frame(500);
    lv_obj_t *hdr = lv_obj_get_child(pages[npages - 1], 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++)
        if (lv_obj_has_flag(lv_obj_get_child(hdr, i), LV_OBJ_FLAG_USER_4)) { lv_obj_send_event(lv_obj_get_child(hdr, i), LV_EVENT_CLICKED, NULL); break; }
    frame(400);
}
/* Go to current song on each page: a song far down the lists (the one
 * playing is put back after) */
static bool b4_shooting;
static void au_go_now(void (*open_page)(void), int kind) {
    int was = np_love_track;
    np_love_track = sl_lib.songs_by_title[60];
    open_page(); frame(500);
    pm_kind = kind; pm_page = pages[npages - 1];
    pm_go_now(); frame(450);   /* the flash at its height */
    if (!b4_shooting) for (int i = 0; i < 20; i++) frame(200);   /* the audit: the A-Z rail, woken by the jump, at rest again */
    np_love_track = was;
}
/* two albums picked, the second by a tap; two artists */
static void au_pick_albums(void) {
    albums_page(); frame(500);
    sel_start(SEL_ALBUMS, album_view, pages[npages - 1], sl_lib.albums_by_title[0]);
    frame(300); sel_toggle(sl_lib.albums_by_title[3]); lv_async_call(sel_refresh_async, album_view); frame(400);
}
static void au_pick_artists(void) {
    artists_page(); frame(500);
    sel_start(SEL_ARTISTS, top_body(), pages[npages - 1], 0);
    frame(300); sel_toggle(artist_span(0)); sel_artists_repaint(top_body(), true); frame(400);
}
/* batch 5: Now Playing's other layouts and states, and the lyrics' sheets */
static bool au_np_changed, au_conn0;
static int au_wifi0;
static const char *au_title0;
static unsigned long au_len0;
static void au_np_set(int style, int status, int size, int star, int ink) {
    au_np_changed = true; au_wifi0 = global_settings.sloppy_wifi; au_conn0 = wf_st.connected;
    au_title0 = test_track.title; au_len0 = test_track.length;
    global_settings.sloppy_np_style = style; global_settings.sloppy_np_status = status; global_settings.sloppy_lyrics_size = size;
    global_settings.sloppy_love_star = star; global_settings.sloppy_np_cover_ink = ink;
    now_playing_retheme();
}
static void au_np_lyrics(void) {
    np_show(); frame(500); np_update(true); frame(100);
    if (!lyr_visible() && lyr_btn && !lv_obj_has_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN)) lv_obj_send_event(lyr_btn, LV_EVENT_CLICKED, NULL);
    frame(300);
}
static void au_lyrics_sheet(bool wifi) {
    au_np_set(0, 0, 1, 0, 0);
    test_track.title = "Night Drive"; test_track.length = 200000;
    global_settings.sloppy_wifi = 0; wf_st.connected = !wifi; lyr_path[0] = '\0';
    np_show(); frame(500); np_update(true); frame(100);
    if (lyr_visible()) lyr_show(false);
    lyr_found_free(); lyr_free();
    lv_obj_send_event(lyr_btn, LV_EVENT_CLICKED, NULL); for (int i = 0; i < 4; i++) frame(200);
}
static char au_del_track[MAX_PATH];        /* the song "playing" before a delete screen said otherwise */
static void au_bt_reset(void);
static bool au_guested, au_faved;   /* batch C screens: a guest on one song, two favourite albums */
/* Sloppy FM on another tab; Now Playing with Repeat one (its "1" badge) */
static void au_fm_tab(int t) { radio_tab = t; radio_page(); }
static void au_np_repeat_one(void) { global_settings.repeat_mode = REPEAT_ONE; np_show(); }
static void au_reset(void) {
    radio_tab = RT_STUDIO; global_settings.repeat_mode = REPEAT_OFF;
    if (eq_parametric()) {   /* a headphone profile an audit screen loaded: the octave bands again */
        eq_profile_save("");
        for (int i = 0; i < EQ_NUM_BANDS; i++) global_settings.eq_band_settings[i].gain = 0;
        global_settings.sloppy_eq_preamp = 0;
        eq_apply_all();
    }
    if (tone_any()) {        /* Tone tuning an audit screen set */
        for (int k = 0; k < TN_N; k++) { if (k == TN_TEXTURE) global_settings.pbe = 0; else *tone_var(k) = 0; }
        tone_apply_all();
    }
    if (cf_is_open()) { cf_close(); frame(400); }
    if (spa.active) { spa_close(); frame(400); }
    if (vz.active) { vz_close(); frame(400); }
    if (pl_visible()) { pl_close(); frame(300); }
    if (acts_open_now) { acts_close(); frame(300); }
    if (sel_on()) { sel_end(); frame(100); }
    fm_op_clear();
    if (fdl.running) { lv_timer_resume(fdl.timer); fdl.stopped = true; fd_finish(); frame(300); }
    fd_clear_src();
    global_settings.sloppy_files_delete = 0;
    if (au_del_track[0]) { strlcpy(test_track.path, au_del_track, sizeof test_track.path); au_del_track[0] = '\0'; }
    if (global_settings.sloppy_qp_tiles[0] && !getenv("SLOPPY_CC_TILES")) { global_settings.sloppy_qp_tiles[0] = '\0'; quick_panel_rebuild(); }
    if (sel_bar) { sel_end(); frame(100); }
    if (uq_open) { uq_close(); frame(400); }
    if (np_open) { np_hide(0); frame(400); }
    if (qp_open) { qp_close(); frame(400); }
    if (po_is_open()) po_close_now();
    if (sl_power_overlay && !lv_obj_has_flag(sl_power_overlay, LV_OBJ_FLAG_HIDDEN)) sl_power_overlay_show(false);
    pet_care_locked = false;
    sl_net_soc_fake = -1;
    sl_net_aic_fake = -2;
    au_bt_reset();
    e_au_reset();
    f_au_reset();
    h_au_reset();
    wave_au_reset();
    ep_au_reset();
    rm_au_reset();
    ot_au_reset();
    sb_au_reset();
    av_au_reset();
    if (sc.up) { shot_card_hide(); frame(300); }
    global_settings.sloppy_settings_tiles = 0;
    global_settings.sloppy_big_icons = 0;
    global_settings.sloppy_years_decades = 0;
    global_settings.sloppy_album_layout = 0;
    if (au_guested) { au_tr[17].artist = au_tr[18].artist = NULL; au_guested = false; lib_epoch++; }
    if (au_faved) { remove(FAV_FILE); fav_n = -1; free(fav_buf); fav_buf = NULL; au_faved = false; }
    if (global_settings.sloppy_album_badges || global_settings.sloppy_album_no_artist || au_badged) au_badges(false);
    while (npages > 1) { page_pop(); frame(300); }
    pop_hide_now(); nbanner_hide_now();
    if (hud) lv_obj_add_flag(hud, LV_OBJ_FLAG_HIDDEN);
    if (au_np_changed) {   /* batch 5's Now Playing states, back as they were */
        lyr_want = false; lyr_show(false); lyr_found_free(); lyr_wait_wifi = false;
        global_settings.sloppy_np_style = 0; global_settings.sloppy_np_status = 0; global_settings.sloppy_lyrics_size = 1;
        global_settings.sloppy_love_star = 0; global_settings.sloppy_np_cover_ink = 0; global_settings.sloppy_wifi = au_wifi0;
        wf_st.connected = au_conn0; test_track.title = au_title0; test_track.length = au_len0; lyr_path[0] = '\0';
        now_playing_retheme(); au_np_changed = false;
    }
    frame(400);
}

/* a library with long names, other scripts and many albums */
static struct sl_album au_al[AU_NA];
static int au_atr[AU_NA][8], au_by_title[AU_NA], au_by_artist[AU_NA], au_songs[AU_NT];
static struct sl_lib au_saved_lib;
static void au_library(void) {
    static const char *const at[10] = {
        "Blue Hours", "The Quiet Architecture of Everything We Left Behind (Deluxe Anniversary Edition)", "Neon Harbor",
        "\xd7\x99\xd7\x95\xd7\x9d \xd7\x97\xd7\x93\xd7\xa9", "Kite", "Greatest Hits 1999-2024",
        "Supercalifragilisticexpialidociously-Unbreakable-Hyphenated-Title", "Lumen",
        "\xd8\xb9\xd9\x88\xd8\xaf\xd8\xa9", "After the Rain" };
    static const char *const aa[10] = {
        "Mira Sol", "Mira Sol & The Extraordinarily Long-Named Midnight Orchestra", "Neon Harbor",
        "\xd7\xa9\xd7\x9c\xd7\x9e\xd7\x94 \xd7\x90\xd7\xa8\xd7\xa6\xd7\x99", "Kite", "Various Artists",
        "Zw\xc3\xb6lf \xc3\x9c" "berl\xc3\xa4ngen Kammerorchester D\xc3\xbcsseldorf", "Lumen",
        "\xd9\x81\xd9\x8a\xd8\xb1\xd9\x88\xd8\xb2", "The Long Weekend" };
    static const int ay[10] = { 2026, 2019, 2011, 1998, 1975, 2003, 2020, 2020, 1972, 2015 };
    static const char *const ag[10] = { "Pop", "Ambient", "Electronic", "Pop", "Rock", "Pop", "Klassik", "Ambient", "Arabic", "Jazz" };
    static const char *const tt[8] = {
        "Night Drive", "An Exceptionally Long Song Title That Keeps Going Well Past Any Reasonable Width (Extended Club Mix)",
        "After the Rain", "\xd7\xa8\xd7\x92\xd7\xa2\xd7\x99\xd7\x9d", "Glass Avenue", "Track 2", "Track 10",
        "Ohne Titel \xe2\x80\x94 Unbenanntes St\xc3\xbc" "ck f\xc3\xbcr Streichquartett Nr. 14" };
    /* Korean (two syllables outside KS X 1001, and one title written apart in
     * jamo, as a Mac names files), Japanese (kana, ・, and ga as ka and its
     * sound mark) and Thai (tones over vowels, over tall letters, before sara
     * am, and yo ying over a lower vowel) */
    static const char *const xt[3] = { "\xeb\xb0\xa4\xed\x95\x98\xeb\x8a\x98\xec\x9d\x98 \xeb\xb3\x84", "\xe5\xa4\x9c\xe6\x98\x8e\xe3\x81\x91\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xad\xe3\x83\x87\xe3\x82\xa3\xe3\x83\xbc",
        "\xe0\xb8\x97\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\x87\xe0\xb8\x9f\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb8\xaa\xe0\xb8\xb5\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb2\xe0\xb8\xa1" };
    static const char *const xa[3] = { "\xec\x9c\xa4\xed\x95\x98\xeb\x8a\x98", "\xe3\x83\x9f\xe3\x83\x8a\xe3\x83\x88\xe3\x83\xbb\xe3\x82\xbd\xe3\x83\xa9",
        "\xe0\xb8\x99\xe0\xb8\xa0\xe0\xb8\xb2 \xe0\xb8\xa8\xe0\xb8\xa3\xe0\xb8\xb5\xe0\xb8\xaa\xe0\xb8\xb8\xe0\xb8\x82" };
    static const char *const xs[3][4] = {
        { "\xec\xb2\xab\xeb\x88\x88\xec\x9d\xb4 \xec\x98\xa4\xeb\xa9\xb4", "\xea\xb7\xb8\xeb\x8c\x80\xec\x97\x90\xea\xb2\x8c",
          "\xeb\x98\xa0\xeb\xb0\xa9\xea\xb0\x81\xed\x95\x98\xec\x99\x80 \xed\x96\x8f",
          "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab\xe1\x84\x80\xe1\x85\xb3\xe1\x86\xaf \xe1\x84\x91\xe1\x85\xa1\xe1\x84\x8b\xe1\x85\xb5\xe1\x86\xaf" },
        { "\xe3\x81\x95\xe3\x82\x88\xe3\x81\xaa\xe3\x82\x89\xe3\x81\xae\xe5\x90\x91\xe3\x81\x93\xe3\x81\x86\xe5\x81\xb4", "\xe3\x82\xab\xe3\x82\x99\xe3\x83\xa9\xe3\x82\xb9\xe3\x81\xae\xe8\xa1\x97",
          "\xe6\x98\x9f\xe3\x81\xae\xe3\x83\xaf\xe3\x83\xab\xe3\x83\x84\xe3\x80\x9c\xe3\x83\xaa\xe3\x83\x97\xe3\x83\xa9\xe3\x82\xa4\xe3\x80\x9c", "\xe3\x81\x82\xe3\x81\x84\xe3\x81\xae\xe3\x81\x86\xe3\x81\x9f" },
        { "\xe0\xb8\x9b\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x9e\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xa2\xe0\xb9\x8c\xe0\xb8\xa2\xe0\xb8\xb2\xe0\xb8\xa1\xe0\xb9\x80\xe0\xb8\x8a\xe0\xb9\x89\xe0\xb8\xb2",
          "\xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3\xe0\xb8\x95\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb9\x84\xe0\xb8\xab\xe0\xb8\xa5\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb8\x99",
          "\xe0\xb8\x81\xe0\xb8\x95\xe0\xb8\xb1\xe0\xb8\x8d\xe0\xb8\x8d\xe0\xb8\xb9\xe0\xb8\xa3\xe0\xb8\xb9\xe0\xb9\x89\xe0\xb8\x84\xe0\xb8\xb8\xe0\xb8\x93",
          "\xe0\xb8\x9d\xe0\xb8\xb1\xe0\xb9\x88\xe0\xb8\x87\xe0\xb8\x9f\xe0\xb9\x89\xe0\xb8\xb2" } };
    static const int xy[3] = { 2024, 2022, 2021 };
    static const char *const xg[3] = { "K-Pop", "J-Pop", "Thai Pop" };
    au_saved_lib = sl_lib;
    int n = 0;
    int na = au_scripts ? AU_NA : 10;
    for (int a = 0; a < na; a++) {
        int nk = a < 10 ? 8 : 4;
        au_al[a] = a < 10 ? (struct sl_album){ at[a], aa[a], ay[a], 8, (a * 37) % 360, au_atr[a], ag[a] }
                          : (struct sl_album){ xt[a - 10], xa[a - 10], xy[a - 10], 4, (a * 37) % 360, au_atr[a], xg[a - 10] };
        for (int k = 0; k < nk; k++, n++) {
            au_atr[a][k] = n; snprintf(au_paths[n], sizeof au_paths[n], "/Music/a%d/t%d.flac", a, k);
            au_tr[n] = (struct sl_track){ au_paths[n], a < 10 ? tt[(k + a) % 8] : xs[a - 10][k], a, k + 1, 1, 185000 + k * 31000, (uint32_t)n, k % 3 == 0, NULL }; tsplit(&au_tr[n]);
        }
    }
    sl_lib.tracks = au_tr; sl_lib.n_tracks = n; sl_lib.albums = au_al; sl_lib.n_albums = na; sl_lib.n_artists = na;
    for (int i = 0; i < na; i++) au_by_title[i] = au_by_artist[i] = i;
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(au_al[au_by_title[j - 1]].title, au_al[au_by_title[j]].title) > 0; j--) { int x = au_by_title[j]; au_by_title[j] = au_by_title[j - 1]; au_by_title[j - 1] = x; }
    for (int i = 1; i < na; i++) for (int j = i; j > 0 && sl_sort_cmp(au_al[au_by_artist[j - 1]].artist, au_al[au_by_artist[j]].artist) > 0; j--) { int x = au_by_artist[j]; au_by_artist[j] = au_by_artist[j - 1]; au_by_artist[j - 1] = x; }
    for (int i = 0; i < n; i++) au_songs[i] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sl_sort_cmp(au_tr[au_songs[j - 1]].title, au_tr[au_songs[j]].title) > 0; j--) { int x = au_songs[j]; au_songs[j] = au_songs[j - 1]; au_songs[j - 1] = x; }
    sl_lib.albums_by_title = au_by_title; sl_lib.albums_by_artist = au_by_artist; sl_lib.songs_by_title = au_songs;
    sl_lib.album_hash = NULL; sl_lib.album_hash_cap = 0; sl_lib.path_hash = NULL; sl_lib.path_hash_cap = 0;
    /* playing: the long one, with more queued */
    test_track.title = au_tr[9].title; test_track.artist = au_al[1].artist; test_track.album = au_al[1].title;
    strlcpy(test_track.path, TPT(&au_tr[9]), sizeof test_track.path);
    test_track.length = 240000; test_track.elapsed = 72000;
    test_status = AUDIO_STATUS_PLAY;
    queued = 6; started = 1; for (int i = 0; i < 6; i++) { queue_tracks[i] = 8 + i; queue_flags[i] = false; }
    {   /* lyrics for it */
        const char *lrc = "[00:10.00]First light on the water, and a line long enough to wrap twice on the narrow screen\n"
                          "[00:20.00]\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d\n"
                          "[00:30.00]\xeb\x84\x88\xec\x9d\x98 \xeb\xaa\xa9\xec\x86\x8c\xeb\xa6\xac\xea\xb0\x80 \xeb\x93\xa4\xeb\xa0\xa4\n"   /* Korean, Japanese, Thai */
                          "[00:40.00]\xe5\xa4\x9c\xe6\x98\x8e\xe3\x81\x91\xe3\x81\xbe\xe3\x81\xa7\xe3\x80\x81\xe3\x81\x82\xe3\x81\xa8\xe5\xb0\x91\xe3\x81\x97\n"
                          "[00:50.00]\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x99\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\xb5\xe0\xb9\x83\xe0\xb8\x84\xe0\xb8\xa3\n[01:00]Last verse line\n";
        char p[64]; snprintf(p, sizeof p, "/Music/a1/t1.lrc");
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd >= 0) { write(fd, lrc, strlen(lrc)); sim_close(fd); }
    }
    {   /* a playlist with a long name */
        char p[300]; snprintf(p, sizeof p, "%s/A Very Long Playlist Name For Late Night Drives Through The City.m3u8", sl_playlist_dir());
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { char b[200]; int k = snprintf(b, sizeof b, "#EXTM3U\n%s\n%s\n", TPT(&au_tr[1]), TPT(&au_tr[9])); write(fd, b, k); sim_close(fd); }
    }
    {   /* audiobooks */
        static const char *const ti[3] = { "The Hobbit, or There and Back Again (Unabridged, Read by the Author)", "Dune", "\xd7\x94\xd7\xa1\xd7\xa4\xd7\xa8" };
        static const char *const au[3] = { "J. R. R. Tolkien", "Frank Herbert", "\xd7\x90\xd7\x9e\xd7\x95\xd7\xa1 \xd7\xa2\xd7\x95\xd7\x96" };
        fake_books_n = 3;
        for (int i = 0; i < 3; i++) { fake_books[i].id = 11 * (i + 1); fake_books[i].length = 3600000u * (5 + i * 3); fake_book_str[i][AB_S_TITLE] = ti[i];
            fake_book_str[i][AB_S_AUTHOR] = au[i]; fake_prog[i].id = fake_books[i].id; fake_prog_on[i] = i < 2; }
        fake_prog[0].position = fake_books[0].length * 43 / 100; fake_books[0].nfiles = 3; fake_prog[0].file = 1;
        /* batch 8: where they are, a series each for two, one finished, and bookmarks in the first */
        static const char *const pa[3] = { "/Audiobooks/The Hobbit", "/Audiobooks/Dune", "/Audiobooks/\xd7\x94\xd7\xa1\xd7\xa4\xd7\xa8" };
        for (int i = 0; i < 3; i++) { fake_book_str[i][AB_S_PATH] = pa[i]; fake_books[i].nfiles = 3; }
        fake_book_str[0][AB_S_SERIES] = "The Legendarium of Middle-earth: Collected Tales and Histories"; fake_book_str[0][AB_S_SERIES_NO] = "1";
        fake_book_str[1][AB_S_SERIES] = "Dune Chronicles"; fake_book_str[1][AB_S_SERIES_NO] = "1";
        fake_prog[1].finished = 1; fake_prog[1].last_played = 50; fake_prog[0].last_played = 100;
        const char *bm = ">7;0;0;0;95000;0;0;10000;10000;01 Prologue;/Audiobooks/The Hobbit/;01 Prologue.mp3\n"
                         ">7;1;0;0;754000;0;0;10000;10000;02 Arrakis;/Audiobooks/The Hobbit/;02 Arrakis.mp3\n"
                         ">7;2;0;0;3541000;0;0;10000;10000;03 The Desert;/Audiobooks/The Hobbit/;03 The Desert.mp3\n";
        int fd = open("/Audiobooks/The Hobbit.bmark", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { write(fd, bm, strlen(bm)); sim_close(fd); }
    }
}
/* the book page with its bookmarks at the top of the screen */
static void au_book_bookmarks(void) {
    bk_book_page(0); frame(400);
    lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
    lv_obj_update_layout(b);
    lv_obj_scroll_to_y(b, lv_obj_get_y(bk_bm_group) - 64, LV_ANIM_OFF);
}
static void au_library_done(void) { sl_lib = au_saved_lib; }

/* the home screens, for each layout */
enum { AU_HOME, AU_HOME_SCROLLED, AU_HOME_IDLE, AU_HOME_N };
static const char *const au_home_names[AU_HOME_N] = { "home", "home-scrolled", "home-idle" };
static bool au_open_home(int k) {
    while (npages > 1) { page_pop(); frame(300); }
    if (k == AU_HOME_IDLE) { test_status = 0; mini_update(); }
    else { test_status = AUDIO_STATUS_PLAY; mini_update(); }
    appearance_reload((void *)2);
    au_settle();
    if (k == AU_HOME_SCROLLED) {
        lv_obj_t *s = NULL;
        for (int i = 0; i < 4 && !s; i++) { /* the home's scrolling body */
            lv_obj_t *p = pages[0];
            for (uint32_t c = 0; c < lv_obj_get_child_count(p); c++) { lv_obj_t *o = lv_obj_get_child(p, c);
                if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(o) > 0) { s = o; break; } }
        }
        if (!s) return false;
        lv_obj_scroll_by(s, 0, -(lv_obj_get_scroll_bottom(s) / 2 + 300), LV_ANIM_OFF); frame(100); frame(100);
    }
    return true;
}

/* every other screen: its name, and how to open it */
/* batch 9's screens, opened as its shots open them (defined at the end) */
static void b9_arcade_card(bool roms);
static void b9_arcade_game(bool menu);
static void b9_arcade_colours(void);
static void b9_2048(void);
static void b9_sudoku(void);
static void b9_points(void);
static void b9_flappy(int which);
static void b9_snake_mid(void);
static void b9_snake_death(void);
#define AU_SCREENS(X) \
    X("music", music_page()) \
    X("songs", songs_page()) \
    X("songs-sort-sheet", (songs_page(), frame(500), songs_sort_cb(NULL))) \
    X("cover-flow-short-sheet", (cf_short_ask(5000), frame(500))) \
    X("songs-picking", au_picking()) \
    X("songs-toast", (songs_page(), frame(500), au_toast())) \
    X("albums", albums_page()) \
    X("albums-sort-sheet", (albums_page(), frame(500), albums_sort_cb(NULL))) \
    X("albums-recent", (global_settings.sloppy_albums_sort = 3, albums_page(), global_settings.sloppy_albums_sort = 0)) \
    X("albums-badges", (au_badges(true), albums_page())) \
    X("albums-no-artist", (global_settings.sloppy_album_no_artist = 1, albums_page())) \
    X("albums-picking", au_pick_albums()) \
    X("albums-current", au_go_now(albums_page, SEL_ALBUMS)) \
    X("album-sheet", (albums_page(), frame(500), album_acts_open(1, true))) \
    X("play-menu", au_play_menu(songs_page)) \
    X("play-menu-albums", au_play_menu(albums_page)) \
    X("play-menu-artists", au_play_menu(artists_page)) \
    X("songs-current", au_go_now(songs_page, SEL_SONGS)) \
    X("album-long", album_page(1)) \
    X("album", album_page(0)) \
    X("album-hebrew", album_page(3)) \
    X("artists", artists_page()) \
    X("artists-picking", au_pick_artists()) \
    X("artists-current", au_go_now(artists_page, SEL_ARTISTS)) \
    X("artist-sheet", (artists_page(), frame(500), artist_acts_open(au_artist_k(1)))) \
    X("artist-badges", (au_badges(true), artist_page(au_artist_k(1)))) \
    X("artist-long", artist_page(au_artist_k(1))) \
    X("tartists", tartists_page()) \
    X("tartist", au_tartist(false)) \
    X("tartist-appears", au_tartist(true)) \
    X("tartist-sheet", (tartists_page(), frame(500), au_tartist_sheet())) \
    X("genres", genres_page()) \
    X("genre-albums", (genres_page(), frame(400), group_page(0x30000))) \
    X("years", years_page()) \
    X("year-albums", (years_page(), frame(400), group_page(0x40000 | 2019))) \
    X("decades", (global_settings.sloppy_years_decades = 1, years_page())) \
    X("decade-albums", (global_settings.sloppy_years_decades = 1, years_page(), frame(400), group_page(0x50000 | 2010))) \
    X("favourite-albums-empty", group_page(0x60000)) \
    X("favourite-albums", (au_favs(), group_page(0x60000))) \
    X("recently-played", group_page(0x70000)) \
    X("album-favourite", (au_favs(), album_page(1))) \
    X("album-big", (global_settings.sloppy_album_layout = 1, album_page(1))) \
    X("album-big-long", (global_settings.sloppy_album_layout = 1, album_page(1), frame(300), lv_obj_scroll_to_y(top_body(), 700, LV_ANIM_OFF))) \
    X("split-on", lo_seps_page()) \
    X("folders-to-scan", lo_folders_page()) \
    X("keep-together", (au_guest(), lo_keep_page())) \
    X("keep-together-empty", lo_keep_page()) \
    X("loved", loved_page()) \
    X("for-you", foryou_page()) \
    X("most-played", smart_list_page(SMART_MOST)) \
    X("wrapped", wrapped_page()) \
    X("hidden-artists", hidden_page()) \
    X("taste", taste_page(false)) \
    X("taste-picks", picks_page()) \
    X("taste-reset", taste_reset_page()) \
    X("playlists", sl_saved_playlists_page(0)) \
    X("playlist-open", au_open_playlist()) \
    X("files", sl_files_page("/", 0)) \
    X("files-folder", au_files_folder()) \
    X("files-empty", (au_card(), sl_files_page(AU_KITE "/Scans", 0))) \
    X("files-cue", (au_card(), fm_cue_page(AU_LIVE "/Live at the Harbor.cue"))) \
    X("files-text", (au_card(), fm_text_page(AU_KITE "/notes.txt"))) \
    X("files-picture", (au_card(), fm_pic_page(AU_KITE "/cover.jpg"))) \
    X("files-sheet", au_files_sheet()) \
    X("files-sheet-delete", au_files_sheet_delete()) \
    X("files-pick-delete", au_files_pick_delete()) \
    X("files-delete-one", au_files_delete_one()) \
    X("files-delete-many", au_files_delete_many()) \
    X("files-delete-progress", au_files_delete_progress()) \
    X("files-folder-sheet", au_files_folder_sheet()) \
    X("files-pick", au_files_pick()) \
    X("files-move", au_files_dest()) \
    X("files-conflict", au_files_conflict()) \
    X("files-rename", au_files_rename()) \
    X("playlists-pick", au_playlists_pick()) \
    X("playlists-delete", au_playlists_delete()) \
    X("playlist-replace", au_playlist_replace()) \
    X("search-playlists", (search_page(), frame(400), sq_add("m"), sq_add("i"), sq_add("x"))) \
    X("audiobooks", (fake_books_n = 3, books_page())) \
    X("audiobook", (fake_books_n = 3, bk_book_page(0))) \
    X("audiobooks-empty", (fake_books_n = 0, books_page())) \
    X("audiobook-bookmarks", (fake_books_n = 3, au_book_bookmarks())) \
    X("audiobooks-authors", (fake_books_n = 3, bk_keys_page(AB_S_AUTHOR))) \
    X("audiobooks-author", (fake_books_n = 3, bk_collect(AB_S_AUTHOR), bk_group_page(AB_S_AUTHOR, 0))) \
    X("audiobooks-series", (fake_books_n = 3, bk_keys_page(AB_S_SERIES))) \
    X("audiobooks-finished", (fake_books_n = 3, bk_view_page(BK_V_FINISHED))) \
    X("audiobooks-library", (fake_books_n = 3, bk_library_at(0, false))) \
    X("internet-radio", webradio_page()) \
    X("internet-radio-sections", e_au_radio(0)) \
    X("internet-radio-offline", e_au_radio(1)) \
    X("internet-radio-failed", e_au_radio(2)) \
    X("internet-radio-none", e_au_radio(3)) \
    X("podcasts-empty", h_au_pod(0)) \
    X("podcasts", h_au_pod(1)) \
    X("podcast-show", h_au_pod(2)) \
    X("podcast-select", h_au_pod(3)) \
    X("podcast-sheet", h_au_pod(4)) \
    X("podcast-find", h_au_pod(5)) \
    X("podcast-results", h_au_pod(6)) \
    X("podcast-preview", h_au_pod(7)) \
    X("podcast-address", h_au_pod(8)) \
    X("podcast-opml", h_au_pod(9)) \
    X("podcast-downloads", h_au_pod(10)) \
    X("podcast-unfollow", h_au_pod(11)) \
    X("podcast-show-hebrew", h_au_pod(12)) \
    X("podcast-results-none", h_au_pod(13)) \
    X("ebook-shelf", ep_au(0)) \
    X("ebook-shelf-empty", ep_au(1)) \
    X("ebook-reader-light", ep_au(2)) \
    X("ebook-reader-sepia", ep_au(3)) \
    X("ebook-reader-dark", ep_au(4)) \
    X("ebook-contents", ep_au(5)) \
    X("ebook-settings", ep_au(6)) \
    X("ebook-error", ep_au(7)) \
    X("ebook-hebrew", ep_au(8)) \
    X("ebook-script", ep_au(9)) \
    X("ebook-chapter-cut", ep_au(10)) \
    X("subsonic-none", sb_au(0)) \
    X("subsonic-signing-in", sb_au(1)) \
    X("subsonic-wrong-password", sb_au(2)) \
    X("subsonic-home", sb_au(3)) \
    X("subsonic-albums", sb_au(4)) \
    X("subsonic-artists", sb_au(5)) \
    X("subsonic-album", sb_au(6)) \
    X("subsonic-downloading", sb_au(7)) \
    X("subsonic-offline", sb_au(8)) \
    X("subsonic-downloaded", sb_au(9)) \
    X("subsonic-search", sb_au(10)) \
    X("subsonic-search-none", sb_au(11)) \
    X("subsonic-settings", sb_au(12)) \
    X("subsonic-sheet", sb_au(13)) \
    X("subsonic-playlist", sb_au(14)) \
    X("subsonic-kept-offline", sb_au(15)) \
    X("subsonic-find", sb_au(16)) \
    X("subsonic-waiting", sb_au(17)) \
    X("subsonic-error", sb_au(18)) \
    X("subsonic-server", sb_au(19)) \
    X("abs-none", av_au(0)) \
    X("abs-signing-in", av_au(1)) \
    X("abs-wrong-password", av_au(2)) \
    X("abs-home", av_au(3)) \
    X("abs-library", av_au(4)) \
    X("abs-authors", av_au(5)) \
    X("abs-series", av_au(6)) \
    X("abs-book", av_au(7)) \
    X("abs-book-on-card", av_au(8)) \
    X("abs-picked-up", av_au(9)) \
    X("abs-downloading", av_au(10)) \
    X("abs-podcast", av_au(11)) \
    X("abs-downloaded", av_au(12)) \
    X("abs-big-download", av_au(13)) \
    X("abs-error", av_au(14)) \
    X("abs-waiting", av_au(15)) \
    X("abs-offline", av_au(16)) \
    X("abs-settings", av_au(17)) \
    X("abs-signed-in", av_au(18)) \
    X("abs-audiobooks", av_au(19)) \
    X("search", search_page()) \
    X("search-results", (search_page(), frame(400), sq_add("N"), sq_add("I"))) \
    X("search-123", (search_page(), frame(400), kb_tap_act(sq_kbd, KA_MODE))) \
    X("search-symbols", (search_page(), frame(400), kb_tap_act(sq_kbd, KA_MODE), kb_tap_act(sq_kbd, KA_SYMS))) \
    X("search-draw", (search_page(), frame(400), kb_tap_act(sq_kbd, KA_DRAW))) \
    X("search-other-keyboard", (search_page(), frame(400), kb_tap_act(sq_kbd, KA_GLOBE))) \
    X("search-glided", (search_page(), frame(800), kb_switch(sq_kbd, KB_EN), frame(400), kb_glide(sq_kbd, "night", true))) \
    X("keyboard-suggestions", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_EN), lv_textarea_set_text(kb_now()->ta, "Late nig"), kb_changed(kb_now()))) \
    X("keyboard-pinyin", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_ZH), kb_type(kb_now(), "nihao"))) \
    X("keyboard-kana", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_JA), kb_type(kb_now(), "ongaku"))) \
    X("keyboard-arabic", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_AR))) \
    X("keyboard-russian", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_RU))) \
    X("keyboard-german", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_DE))) \
    X("keyboard-t9", (eq_save_page(NULL), frame(400), kb_switch(kb_now(), KB_T9))) \
    X("settings-keyboards", keyboards_page()) \
    X("settings-keyboards-pick", kbs_pick_page(1)) \
    X("library", sl_library_page()) \
    X("settings-library-options", library_options_page()) \
    X("settings-library-options-delete", (global_settings.sloppy_files_delete = 1, library_options_page())) \
    X("music-arrange", mu_arrange_page()) \
    X("song-info", song_info_page(9)) \
    X("now-playing", np_show()) \
    X("lyrics", au_lyrics()) \
    X("now-playing-centred", (au_np_set(2, 0, 1, 0, 1), np_show())) \
    X("now-playing-status", (au_np_set(0, 1, 1, 1, 1), np_show())) \
    X("now-playing-album", f_au_np_album()) \
    X("now-playing-waveform", wave_au(0)) \
    X("now-playing-waveform-filling", wave_au(1)) \
    X("now-playing-waveform-album", wave_au(2)) \
    X("now-playing-waveform-ink", wave_au(3)) \
    X("settings-battery-history", f_au_batlog()) \
    X("erase-data", f_au_erase()) \
    X("screensaver-page", f_au_saver(0)) \
    X("screensaver-floating", f_au_saver(1)) \
    X("screensaver-square", f_au_saver(2)) \
    X("screensaver-clock", f_au_saver(3)) \
    X("car-controls", f_au_car(0)) \
    X("car-power-gone", f_au_car(1)) \
    X("settings-car-mode", f_au_car(2)) \
    X("lyrics-centred", (au_np_set(2, 1, 1, 0, 0), au_np_lyrics())) \
    X("lyrics-xl", (au_np_set(0, 0, 3, 0, 0), au_np_lyrics())) \
    X("lyrics-picker", au_lyrics_sheet(false)) \
    X("lyrics-wifi", au_lyrics_sheet(true)) \
    X("queue", (np_show(), frame(500), uq_show())) \
    X("control-centre", ccx_open()) \
    X("control-centre-grown", (ccx_open(), ccx_grow())) \
    X("control-centre-grown-page-2", au_cc_page_2()) \
    X("control-centre-grown-16", au_cc_sixteen()) \
    X("song-sheet", acts_open(9)) \
    X("now-playing-more", (np_show(), frame(500), np_open && np_love_track >= 0 ? acts_open_np(np_love_track) : acts_open_np(9))) \
    X("playlist-sheet", pl_open(9)) \
    X("playlist-new", au_playlist_new()) \
    X("toast-home", au_toast()) \
    X("banner-home", au_banner()) \
    X("banner-page", (songs_page(), frame(500), au_banner())) \
    X("volume-hud", au_hud()) \
    X("lock", sl_power_overlay_show(true)) \
    X("settings", system_page()) \
    X("settings-tiles", (global_settings.sloppy_settings_tiles = 1, system_page())) \
    X("settings-layout", settings_layout_page()) \
    X("settings-big", (global_settings.sloppy_big_icons = 1, system_page())) \
    X("settings-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, system_page())) \
    X("settings-layout-big", (global_settings.sloppy_big_icons = 1, settings_layout_page())) \
    X("settings-sound-big", (global_settings.sloppy_big_icons = 1, sound_page())) \
    X("settings-customization-big", (global_settings.sloppy_big_icons = 1, appearance_page())) \
    X("settings-customization-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, appearance_page())) \
    X("settings-screen-sleep-big", (global_settings.sloppy_big_icons = 1, screen_sleep_page())) \
    X("bluetooth-big", (global_settings.sloppy_big_icons = 1, sl_bt_page())) \
    X("settings-advanced-tiles", (global_settings.sloppy_settings_tiles = 1, advanced_page())) \
    X("settings-advanced-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, advanced_page())) \
    X("settings-sound-tiles", (global_settings.sloppy_settings_tiles = 1, sound_page())) \
    X("settings-sound-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, sound_page())) \
    X("settings-playback-tiles", (global_settings.sloppy_settings_tiles = 1, play_settings_page())) \
    X("settings-playback-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, play_settings_page())) \
    X("wifi-tiles", (global_settings.sloppy_settings_tiles = 1, sl_wifi_page())) \
    X("wifi-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, sl_wifi_page())) \
    X("bluetooth-tiles", (global_settings.sloppy_settings_tiles = 1, sl_bt_page())) \
    X("bluetooth-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, sl_bt_page())) \
    X("settings-scrobble-tiles", (global_settings.sloppy_settings_tiles = 1, scr_page())) \
    X("settings-scrobble-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, scr_page())) \
    X("settings-time-power-tiles", (global_settings.sloppy_settings_tiles = 1, time_power_page())) \
    X("settings-time-power-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, time_power_page())) \
    X("settings-eq-tiles", (global_settings.sloppy_settings_tiles = 1, eq_page())) \
    X("settings-eq-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, eq_page())) \
    X("settings-eq-profiles-tiles", (global_settings.sloppy_settings_tiles = 1, eq_profiles_page())) \
    X("settings-eq-profiles-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, eq_profiles_page())) \
    X("settings-keyboards-tiles", (global_settings.sloppy_settings_tiles = 1, keyboards_page())) \
    X("settings-keyboards-tiles-big", (global_settings.sloppy_big_icons = 1, global_settings.sloppy_settings_tiles = 1, keyboards_page())) \
    X("settings-sound", sound_page()) \
    X("settings-playback", play_settings_page()) \
    X("settings-eq", eq_page()) \
    X("settings-eq-profiles", eq_profiles_page()) \
    X("settings-eq-autoeq", au_eq_profile()) \
    X("settings-eq-band", au_eq_band(false)) \
    X("settings-eq-band-profile", au_eq_band(true)) \
    X("settings-eq-peq", au_eq_peq()) \
    X("settings-eq-peq-shelf", au_eq_peq_shelf()) \
    X("settings-eq-band-peq", au_eq_band_peq()) \
    X("settings-tone", au_tone(false)) \
    X("settings-tone-profile", au_tone(true)) \
    X("settings-eq-find", eq_find_page()) \
    X("settings-buttons", buttons_page()) \
    X("settings-shortcuts", shortcuts_page()) \
    X("settings-time-power", time_power_page()) \
    X("settings-time-zones", tz_page()) \
    X("settings-lyrics-colour", lyrics_color_page()) \
    X("settings-language", language_page()) \
    X("settings-text-size", text_size_page()) \
    X("settings-advanced", advanced_page()) \
    X("settings-sounds", sounds_page()) \
    X("settings-customization", appearance_page()) \
    X("settings-icon", settings_icon_page()) \
    X("settings-theme", theme_page()) \
    X("settings-home-style", home_style_page()) \
    X("settings-np-style", np_style_page()) \
    X("settings-coverflow-style", cf_style_page()) \
    X("settings-lock-player", lock_player_page()) \
    X("settings-screen-sleep", screen_sleep_page()) \
    X("settings-accent", accent_page()) \
    X("settings-wallpaper", wallpaper_page(false)) \
    X("settings-lock-wallpaper", wallpaper_page(true)) \
    X("settings-sleep", qp_sleep_page()) \
    X("settings-display", qp_display_page()) \
    X("settings-search", settings_search_page()) \
    X("settings-scrobble", scr_page()) \
    X("lyrics-and-covers", e_au_libjob(0)) \
    X("lyrics-and-covers-running", e_au_libjob(1)) \
    X("lyrics-and-covers-waiting", e_au_libjob(2)) \
    X("lyrics-and-covers-failed", e_au_libjob(3)) \
    X("lastfm-signin", e_au_lastfm(0)) \
    X("lastfm-password", e_au_lastfm(1)) \
    X("settings-scrobble-lastfm-in", e_au_lastfm(2)) \
    X("settings-scrobble-lastfm-lost", e_au_lastfm(3)) \
    X("settings-scrobble-token", scr_token_page()) \
    X("settings-spotify", sp_page()) \
    X("settings-reset", reset_all_settings_page()) \
    X("settings-classic", classic_confirm_page()) \
    X("settings-dual-boot", dual_boot_confirm_page()) \
    X("settings-receive", sl_btrx_page()) \
    X("settings-receive-connected", au_btrx_connected()) \
    X("bluetooth-connected", (sl_bt_demo_index = 0, sl_bt_page())) \
    X("bluetooth-codecs", (strlcpy(sl_bt_demo_codec, "LDAC", sizeof sl_bt_demo_codec), sl_bt_demo_codec_n = 5, sl_bt_demo_index = 0, \
                           global_status.sloppy_volume_on_bt = 1, sl_bt_page())) \
    X("bt-device", au_bt_device()) \
    X("bt-rename", (sl_bt_page(), frame(300), sl_bt_name_page(sl_bt_demo_devices[0].mac, sl_bt_demo_devices[0].name))) \
    X("bt-player-name", (strlcpy(sl_bt_demo_name, "Sam's R1", sizeof sl_bt_demo_name), sl_bt_player_name_cb(NULL))) \
    X("bt-sbc", (sl_bt_demo_sbc = 1, sl_bt_sbc_page(NULL))) \
    X("bt-pair-confirm", au_bt_ask(HIBY_BT_ASK_CONFIRM, "123456")) \
    X("bt-pair-pin", au_bt_ask(HIBY_BT_ASK_PIN, "")) \
    X("bt-pair-show", au_bt_ask(HIBY_BT_ASK_SHOW_PASSKEY, "654321")) \
    X("bt-error", au_bt_error()) \
    X("bt-reconnecting", au_bt_reconnecting()) \
    X("settings-receive-codec", au_btrx_codec()) \
    X("bt-airpods", au_bt_airpods()) \
    X("settings-eq-save", eq_save_page(NULL)) \
    X("settings-eq-found", au_eq_found()) \
    X("settings-bt-forget", au_bt_forget()) \
    X("wifi", sl_wifi_page()) \
    X("wifi-password", wf_password_page("Home Network 5G Extended")) \
    X("wifi-other", (wf_ot_secure = 1, wf_other_page())) \
    X("wifi-other-open", (wf_ot_secure = 0, wf_other_page(), lv_textarea_set_text(wf_ot_name, "Attic Studio (hidden)"))) \
    X("wifi-transfer", (wf_st.connected ? wf_share_cb(NULL) : (void)0)) \
    X("wifi-transfer-receiving", e_au_receive(0)) \
    X("wifi-transfer-done", e_au_receive(1)) \
    X("localsend-ask", e_au_receive(2)) \
    X("bluetooth", sl_bt_page()) \
    X("screenshot-card", au_shot(false)) \
    X("screenshot-viewer", au_shot(true)) \
    X("wifi-remote-code", rm_au(0)) \
    X("update-none", ot_au(0)) \
    X("update-no-key", ot_au(1)) \
    X("update-found", ot_au(2)) \
    X("update-installing", ot_au(3)) \
    X("update-ready", ot_au(4)) \
    X("update-not-ours", ot_au(5)) \
    X("update-older", ot_au(6)) \
    X("update-needs-first", ot_au(7)) \
    X("update-damaged", ot_au(8)) \
    X("update-no-space", ot_au(9)) \
    X("update-usb-mode", ot_au(10)) \
    X("update-receiving", ot_au(11)) \
    X("update-updated", ot_au(12)) \
    X("update-no-crypto", ot_au(13)) \
    X("update-from-remote", ot_au(14)) \
    X("update-previous-confirm", ot_au(15)) \
    X("wifi-aic", (sl_net_soc_fake = 0x4a00, sl_wifi_page())) \
    X("bluetooth-aic", (sl_net_soc_fake = 0x4900, sl_bt_page())) \
    X("playroom", sl_games_page()) \
    X("pet", pet_page()) \
    X("ragdoll", ragdoll_page()) \
    X("snake", snake_page()) \
    X("sloppy-fm", radio_page()) \
    X("sloppy-fm-syndicate", au_fm_tab(RT_SYNDICATE)) \
    X("now-playing-repeat-one", au_np_repeat_one()) \
    X("freaky-spoon", spoon_page()) \
    X("quiz", quiz_page()) \
    X("coin", coin_page()) \
    X("magic-8-ball", magic_page()) \
    X("chess", chess_page()) \
    X("clicker", clicker_page()) \
    X("metronome", metronome_page()) \
    X("spacey-lines", spacey_page()) \
    X("arcade", arcade_page()) \
    X("arcade-empty", (b9_arcade_card(false), arcade_page())) \
    X("arcade-list", (b9_arcade_card(true), arcade_page())) \
    X("arcade-game", b9_arcade_game(false)) \
    X("arcade-menu", b9_arcade_game(true)) \
    X("arcade-colours", b9_arcade_colours()) \
    X("2048", b9_2048()) \
    X("sudoku", b9_sudoku()) \
    X("24-points", b9_points()) \
    X("flappy", b9_flappy(0)) \
    X("flappy-flying", b9_flappy(1)) \
    X("flappy-paused", b9_flappy(2)) \
    X("flappy-over", b9_flappy(3)) \
    X("snake-mid", b9_snake_mid()) \
    X("snake-death", b9_snake_death()) \
    X("graveyard", grave_page()) \
    X("coverflow", cf_open()) \
    X("spectrum", spa_open()) \
    X("visualizer", vz_open()) \
    X("empty-music", (au_empty(true), music_page())) \
    X("empty-songs", (au_empty(true), songs_page())) \
    X("empty-albums", (au_empty(true), albums_page()))

/* batch C: a guest on one song, two favourite albums; both undone by au_reset */
static void au_guest(void) { au_tr[17].artist = "Lumen feat. Kite & Mira Sol"; au_tr[18].artist = "Kite x Neon Harbor"; au_guested = true; lib_epoch++; }
static void au_tartist(bool appears) {
    au_guest();
    int id = tg_find(appears ? "Lumen" : "Mira Sol");
    if (id >= 0) tartist_page(id);
}
static void au_tartist_sheet(void) {
    lv_obj_t *l = find_label(pages[npages - 1], "Kite");
    if (l) lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_LONG_PRESSED, NULL);
}
static void au_favs(void) { remove(FAV_FILE); fav_n = -1; fav_set(1, true); fav_set(4, true); au_faved = true; }
static int au_artist_k(int album) { for (int k = 0; k < sl_lib.n_albums; k++) if (sl_lib.albums_by_artist[k] == album) return k; return 0; }
/* The harness's clock stands still, so every shot has one name and past 99
 * of it a shot can't be saved (and the wait for its card runs out, ~3 s a
 * shot): the folder is emptied before it gets there */
static void au_shot_room(void) {
    char d[600]; snprintf(d, sizeof d, "%s/Screenshots", sim_root_dir);
    DIR *dir = opendir(d); int n = 0;
    if (dir) { while (readdir(dir)) n++; closedir(dir); }
    if (n > 90) { char c[700]; snprintf(c, sizeof c, "rm -rf '%s'", d); system(c); }
}
/* a screenshot of what is there, its card risen; or that shot open in the viewer */
static void au_shot(bool view) {
    char d[600]; snprintf(d, sizeof d, "mkdir -p '%s'", sim_root_dir); system(d);
    au_shot_room();
    sl_screenshot();
    for (int i = 0; i < 300 && !sc.up; i++) { usleep(10000); frame(20); }
    frame(600);
    if (view && shot_last[0]) { shot_card_hide(); frame(300); shot_view_page(shot_last); }
}
/* AutoEq's matches for a search, as they come back */
/* the EQ with a ten-filter AutoEq profile in it: a low shelf, eight peaks, a high shelf */
static void au_eq_profile(void) {
    static const char af[] = "Preamp: -6.1 dB\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\nFilter 2: ON PK Fc 62 Hz Gain -1.9 dB Q 0.39\n"
        "Filter 3: ON PK Fc 180 Hz Gain -3.6 dB Q 0.71\nFilter 4: ON PK Fc 1240 Hz Gain 2.2 dB Q 1.47\n"
        "Filter 5: ON PK Fc 2900 Hz Gain -3.4 dB Q 2.45\nFilter 6: ON PK Fc 4400 Hz Gain 4.1 dB Q 3.10\n"
        "Filter 7: ON PK Fc 6100 Hz Gain -4.6 dB Q 4.20\nFilter 8: ON PK Fc 8300 Hz Gain 2.8 dB Q 2.90\n"
        "Filter 9: ON PK Fc 13000 Hz Gain -1.5 dB Q 1.20\nFilter 10: ON HSC Fc 10000 Hz Gain -2.5 dB Q 0.70\n";
    int fd = open("/AutoEq/Studio Reference ParametricEQ.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { if (write(fd, af, sizeof af - 1) < 0) {} sim_close(fd); }
    eq_autoeq_load("/AutoEq/Studio Reference ParametricEQ.txt", "Studio Reference");
    sim_remove("/AutoEq/Studio Reference ParametricEQ.txt");
    eq_sel = 3;
    eq_page();
}
/* a PEQ file that uses the whole filter set: a high pass for rumble, a Q 20
 * notch, shelves, peaks and a low pass, as HiBy's app and APO write them */
static void au_eq_peq(void) {
    static const char af[] = "Preamp: -4.0 dB\n"
        "Filter 1: ON HP Fc 30 Hz Q 0.71\nFilter 2: ON LSC Fc 105 Hz Gain 4.0 dB Q 0.70\n"
        "Filter 3: ON PK Fc 1000 Hz Gain -12.0 dB Q 20.00\nFilter 4: ON PK Fc 3000 Hz Gain 3.0 dB Q 2.00\n"
        "Filter 5: ON PK Fc 6000 Hz Gain -4.0 dB Q 4.00\nFilter 6: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.70\n"
        "Filter 7: ON LP Fc 18000 Hz Q 0.71\n";
    mkdir("/peq");
    int fd = open("/peq/Notch and Pass.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { if (write(fd, af, sizeof af - 1) < 0) {} sim_close(fd); }
    eq_autoeq_load("/peq/Notch and Pass.txt", "Notch and Pass");
    sim_remove("/peq/Notch and Pass.txt");
    eq_sel = 0;
    for (int i = 0; i < EQ_NUM_BANDS; i++)      /* the notch, wherever it was placed */
        if (global_settings.eq_band_settings[i].cutoff == 1000 && global_settings.eq_band_settings[i].gain) eq_sel = i;
    eq_page();
}
/* the same, its high shelf chosen: the longest kind in the readout */
static void au_eq_peq_shelf(void) {
    au_eq_peq();
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        if (global_settings.eq_band_settings[i].type == EQ_TYPE_HIGH_SHELF) eq_sel = i;
    eq_refresh();
}
static void au_eq_band_peq(void) {
    au_eq_peq();
    frame(300); pop_hide_now(); nbanner_hide_now();
    eq_band_page(NULL);
}
/* Tone tuning, set the way someone warming up a bright pair would */
static void au_tone_set(void) {
    global_settings.sloppy_tone_temp = -2; global_settings.sloppy_tone_deep = 3; global_settings.pbe = 25;
    global_settings.sloppy_tone_vocal = 2; global_settings.sloppy_tone_air = 2;
    tone_apply_all();
}
static void au_tone(bool profile) {
    au_tone_set();
    if (profile) { au_eq_profile(); frame(300); page_pop(); frame(300); pop_hide_now(); nbanner_hide_now(); }
    tone_page();
}
/* a band's values, typed */
static void au_eq_band(bool profile) {
    if (profile) au_eq_profile(); else { eq_sel = 4; eq_page(); }
    frame(300); pop_hide_now(); nbanner_hide_now();   /* the profile's toast has gone by the time anyone types */
    eq_band_page(NULL);
}
static void au_eq_found(void) {
    free(eq_found); eq_found = calloc(EQ_FOUND_MAX, sizeof *eq_found); eq_found_n = 2;
    strlcpy(eq_found[0].name, "Sennheiser HD 650", sizeof eq_found[0].name); strlcpy(eq_found[0].by, "oratory1990", sizeof eq_found[0].by);
    strlcpy(eq_found[1].name, "Sennheiser HD 650 (sample 2)", sizeof eq_found[1].name); strlcpy(eq_found[1].by, "Rtings", sizeof eq_found[1].by);
    eq_found_page("HD 650");
}
/* Receive from phone with a phone playing (the simulator's pretend phone) */
static void au_btrx_connected(void) {
    sl_btrx_demo_auto = false;
    sl_btrx_begin(); frame(100);
    sl_btrx_demo.state = HIBY_BTRX_CONNECTED; sl_btrx_demo.connects = 1;
    strlcpy(sl_btrx_demo.mac, "AA:BB:CC:DD:EE:01", sizeof sl_btrx_demo.mac);
    strlcpy(sl_btrx_demo.name, "Sam's iPhone", sizeof sl_btrx_demo.name);
    strlcpy(sl_btrx_demo.codec, "AAC", sizeof sl_btrx_demo.codec); sl_btrx_demo.rate = 48000; sl_btrx_demo.channels = 2;
    strlcpy(sl_btrx_demo.title, "Gabagool Groove", sizeof sl_btrx_demo.title);
    strlcpy(sl_btrx_demo.artist, "The Sopranos", sizeof sl_btrx_demo.artist);
    strlcpy(sl_btrx_demo.player, "paused", sizeof sl_btrx_demo.player); sl_btrx_demo.volume = 64;
    frame(200);
    sl_btrx_page(); frame(200);
}
/* the Bluetooth screens' pretend state, back as it was */
static void au_bt_reset(void) {
    if (sl_btrx_on) { sl_btrx_stop(); frame(200); }
    sl_btrx_demo_auto = true;
    sl_bt_demo_index = -1; sl_bt_demo_on = true;
    strlcpy(sl_bt_demo_codec, "AAC", sizeof sl_bt_demo_codec); sl_bt_demo_codec_n = 2; global_status.sloppy_volume_on_bt = 0;
    sl_bt_demo_name[0] = '\0'; sl_bt_demo_sbc = 0; sl_bt_demo_last[0] = '\0';
    sl_bt_err_mac[0] = '\0'; sl_bt_rc_disarm("audit"); sl_bt_demo_rc_misses = 0;
    sl_bt_demo_ask.kind = HIBY_BT_ASK_NONE;
    memset(&sl_btrx_demo, 0, sizeof sl_btrx_demo);
    sl_bt_demo_aap = (struct hiby_bt_aap){ .left = -1, .right = -1, .box = -1 };
    strlcpy(sl_bt_demo_devices[1].name, "Pocket speakers", sizeof sl_bt_demo_devices[1].name);
}
/* a paired device's own page (Rename, Forget) */
static void au_bt_device(void) {
    sl_bt_demo_index = 0; sl_bt_page(); frame(300); frame(100); frame(100);
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(o, sl_bt_device_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    lv_obj_delete(o);
}
/* the pairing page as each kind of question shows it */
static void au_bt_ask(int kind, const char *code) {
    struct hiby_bt_ask a = { .kind = kind, .serial = 9000 + kind };
    strlcpy(a.mac, "00:1A:7D:DA:71:15", sizeof a.mac);
    strlcpy(a.name, "Volkswagen Golf Radio", sizeof a.name);
    strlcpy(a.code, code, sizeof a.code);
    sl_bt_pair_page(&a);
}
/* a connect that failed: the reason on the row */
static void au_bt_error(void) {
    strlcpy(sl_bt_err_mac, sl_bt_demo_devices[1].mac, sizeof sl_bt_err_mac);
    sl_bt_err_code = HIBY_BT_ERR_NOT_FOUND;
    sl_bt_page();
}
/* the last headphones looked for after they dropped out */
static void au_bt_reconnecting(void) {
    strlcpy(sl_bt_demo_last, sl_bt_demo_devices[0].mac, sizeof sl_bt_demo_last);
    sl_bt_demo_rc_misses = 1000;
    sl_bt_rc_arm("audit");
    sl_bt_page();
}
/* AirPods connected: battery and noise control */
static void au_bt_airpods(void) {
    strlcpy(sl_bt_demo_devices[1].name, "Sam's AirPods Pro", sizeof sl_bt_demo_devices[1].name);
    sl_bt_demo_aap = (struct hiby_bt_aap){ .up = true, .left = 99, .right = 100, .box = 17, .left_charging = true, .noise = 2 };
    sl_bt_demo_index = 1; sl_bt_page();
}
/* receiving, with the codecs the phone and the player share */
static void au_btrx_codec(void) {
    au_btrx_connected();
    sl_btrx_demo.codec_count = 4;
    strlcpy(sl_btrx_demo.codecs[0], "SBC", 16); strlcpy(sl_btrx_demo.codecs[1], "AAC", 16);
    strlcpy(sl_btrx_demo.codecs[2], "aptX", 16); strlcpy(sl_btrx_demo.codecs[3], "LDAC", 16);
    strlcpy(sl_btrx_demo.codec_note, "kept", sizeof sl_btrx_demo.codec_note);
    frame(200);
    lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
    lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF);
}
/* Bluetooth's Forget device? page, for a paired pair of headphones */
static void au_bt_forget(void) {
    memset(&sl_bt_found[0], 0, sizeof sl_bt_found[0]);
    strlcpy(sl_bt_found[0].name, "Sennheiser Momentum 4", sizeof sl_bt_found[0].name);
    strlcpy(sl_bt_found[0].mac, "AA:BB:CC:DD:EE:01", sizeof sl_bt_found[0].mac);
    sl_bt_found[0].paired = true; sl_bt_count = 1;
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(o, sl_bt_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    lv_obj_delete(o);
    sl_bt_count = 0;
}
static void au_toast(void) {
    global_settings.sloppy_notifications = 0;
    toast("Added 12 songs to A Very Long Playlist Name For Late Night Drives");
    frame(0); frame(700);
}
static void au_banner(void) {
    global_settings.sloppy_notifications = 1;
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "audit", "Radio from An Exceptionally Long Song Title That Keeps Going");
    frame(400);
}
static void au_hud(void) {
    int s = sl_power_running; sl_power_running = false;
    global_status.volume = volume_from_pct(50); volume_key(0); frame(0); frame(2100);
    sl_power_running = s;
}
static void au_lyrics(void) {
    np_show(); frame(500);
    np_update(true); frame(100);
    if (lyr_btn && !lv_obj_has_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN)) lv_obj_send_event(lyr_btn, LV_EVENT_CLICKED, NULL);
    frame(300);
}
static void au_playlist_new(void) {
    pl_open(9); frame(500);
    lv_obj_t *l = pl_sheet ? find_label(pl_sheet, sl_tr("New playlist")) : NULL;
    if (l) { lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_CLICKED, NULL); frame(300); }
}
/* the sandboxed card (/Card, in memory): an album folder whose numbers
 * sort wrong as words, a live album as one file and its cue sheet, a cover,
 * notes and a folder of scans; folders made in no particular order */
#define AU_KITE FAKE_CARD "/Music/Kite - Paper Kite (1975)"
#define AU_LIVE FAKE_CARD "/Music/Live at the Harbor"
static void au_card(void) {
    fake_card_reset();
    mkdir(FAKE_CARD "/Music"); mkdir(FAKE_CARD "/Podcasts"); mkdir(AU_LIVE); mkdir(AU_KITE); mkdir(AU_KITE "/Scans");
    static const char *const kite[] = { "10 Undertow.flac", "2 Paper Kite.flac", "1 Morning Wire.flac", "11 Last Light.flac",
        "3 Harbor Lights.flac", "9 Weekend.flac", "4 Glass Avenue.flac", "cover.jpg", "notes.txt" };
    char p[300], b[4096];
    for (unsigned i = 0; i < sizeof kite / sizeof kite[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", AU_KITE, kite[i]);
        size_t n = strstr(kite[i], ".flac") ? 2048 + 97 * i : strstr(kite[i], ".jpg") ? 1800 : 0;
        if (!n) n = (size_t)snprintf(b, sizeof b, "Kite, Paper Kite (1975)\n\nRecorded in one week at Harbor Studio.\nTracks 5 to 8 are on the second side of the tape and are not here yet.\n\nMastered from the original reels.\n");
        else memset(b, 0x5a, n);
        fake_put(p, b, n);
    }
    memset(b, 0x5a, 4000); fake_put(AU_LIVE "/Live at the Harbor.flac", b, 4000);
    const char *cue = "REM GENRE Rock\nPERFORMER \"Kite\"\nTITLE \"Live at the Harbor\"\nFILE \"Live at the Harbor.flac\" WAVE\n"
        "  TRACK 01 AUDIO\n    TITLE \"Intro\"\n    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n    TITLE \"Paper Kite\"\n    INDEX 01 01:12:30\n"
        "  TRACK 03 AUDIO\n    TITLE \"Harbor Lights\"\n    PERFORMER \"Kite with Mira Sol\"\n    INDEX 00 05:01:00\n    INDEX 01 05:03:00\n"
        "  TRACK 04 AUDIO\n    TITLE \"Undertow (Extended, and Then Some More)\"\n    INDEX 01 09:40:12\n";
    fake_put(AU_LIVE "/Live at the Harbor.cue", cue, strlen(cue));
    fake_put(AU_LIVE "/cover.jpg", b, 1800);
    fake_put(FAKE_CARD "/Podcasts/Episode 12.mp3", b, 3000);
    fake_put(FAKE_CARD "/Podcasts/Episode 2.mp3", b, 3000);
}
static void au_files_folder(void) { au_card(); sl_files_page(AU_KITE, 0); }
static struct fm_view *au_kite(void) { au_files_folder(); frame(400); return fm_find(AU_KITE); }
static void au_files_pick(void) { struct fm_view *v = au_kite(); sel_start(SEL_FILES, v, v->page, 1); sel_toggle(2); frame(100); }
static void au_files_dest(void) { struct fm_view *v = au_kite(); int k = 8; fm_op_start(FM_MOVE, v, &k, 1); }
static void au_files_conflict(void) { struct fm_view *v = au_kite(); int k = 9; fm_op_start(FM_COPY, v, &k, 1); frame(400); strlcpy(fm_op.dest, AU_KITE, sizeof fm_op.dest); fm_conflict_page(1); }
static void au_files_rename(void) { struct fm_view *v = au_kite(); fm_name_page(v, 1); }
static void au_files_sheet(void) { struct fm_view *v = au_kite(); fm_acts(v, 1); }
/* deleting (allowed for the screen; au_reset puts it back off) */
static void au_files_sheet_delete(void) { global_settings.sloppy_files_delete = 1; au_files_sheet(); }
static void au_files_pick_delete(void) { global_settings.sloppy_files_delete = 1; au_files_pick(); fm_sel_more(); }
static void au_files_delete_one(void) { global_settings.sloppy_files_delete = 1; struct fm_view *v = au_kite(); frame(400); int k = 1; fm_delete_ask(v, &k, 1); }
static void au_files_delete_many(void) {
    global_settings.sloppy_files_delete = 1;
    struct fm_view *v = au_kite(); frame(400);
    strlcpy(au_del_track, test_track.path, sizeof au_del_track);
    test_status = AUDIO_STATUS_PLAY; strlcpy(test_track.path, AU_KITE "/2 Paper Kite.flac", sizeof test_track.path);
    int ks[] = { 0, 1, 2, 3, 4, 5, 6 };   /* Scans, then six songs: five named, "and 2 more", the playing one among them */
    fm_delete_ask(v, ks, 7);
}
static void au_files_delete_progress(void) {
    global_settings.sloppy_files_delete = 1;
    struct fm_view *v = au_kite(); frame(400);
    fake_put(AU_KITE "/Scans/Front.jpg", "x", 1); fake_put(AU_KITE "/Scans/Back.jpg", "x", 1); fake_put(AU_KITE "/Scans/Booklet 1.jpg", "x", 1);
    int k = 0; fm_delete_ask(v, &k, 1); frame(400);
    fd_go_cb(NULL); lv_timer_pause(fdl.timer); fd_run(0, 2);   /* held there, halfway */
}
static void au_cc_page_2(void) { ccx_open(); ccx_grow(); qx_page_go(1); for (int i = 0; i < 8; i++) frame(60); }
static void au_cc_sixteen(void) {
    strlcpy((char *)global_settings.sloppy_qp_tiles, "0123456789abcdef.", sizeof global_settings.sloppy_qp_tiles);
    quick_panel_rebuild(); ccx_open(); ccx_grow();
}
static void au_files_folder_sheet(void) { struct fm_view *v = au_kite(); fm_acts(v, 0); }
static void au_playlists_pick(void) { sl_saved_playlists_page(0); frame(400); sel_start(SEL_PLAYLISTS, top_body(), pages[npages - 1], 0); }
static void au_playlists_delete(void) { au_playlists_pick(); frame(200); pls_del_page(); }
static void au_playlist_replace(void) {
    pl_open(9); frame(300);
    snprintf(pl_replace_path, sizeof pl_replace_path, "%s/Road trip.m3u8", sl_playlist_dir());
    strlcpy(pl_replace_name, "Road trip", sizeof pl_replace_name);
    pl_replace_view();
}
static void au_open_playlist(void) {
    char p[300]; snprintf(p, sizeof p, "%s/A Very Long Playlist Name For Late Night Drives Through The City.m3u8", sl_playlist_dir());
    pls_page(p);
}
static void au_picking(void) {
    songs_page(); frame(500);
    lv_obj_t *sb2 = lv_obj_get_child(pages[npages - 1], 1), *host = NULL, *r0 = NULL, *r1 = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(sb2); i++) if (lv_obj_get_user_data(lv_obj_get_child(sb2, i))) host = lv_obj_get_child(sb2, i);
    for (uint32_t i = 0; host && i < lv_obj_get_child_count(host); i++) { lv_obj_t *c = lv_obj_get_child(host, i); if (lv_obj_get_y(c) == 0) r0 = c; }
    if (!r0) return;
    lv_obj_send_event(r0, LV_EVENT_LONG_PRESSED, NULL); frame(300);
    for (uint32_t i = 0; i < lv_obj_get_child_count(host); i++) { lv_obj_t *c = lv_obj_get_child(host, i); if (lv_obj_get_y(c) > 0 && lv_obj_get_y(c) < 200) r1 = c; }
    if (r1) lv_obj_send_event(r1, LV_EVENT_CLICKED, NULL);
    frame(400);
}
static int au_empty_saved_n, au_empty_saved_a;
static bool au_emptied;
static void au_empty(bool on) {
    if (on && !au_emptied) { au_empty_saved_n = sl_lib.n_tracks; au_empty_saved_a = sl_lib.n_albums; sl_lib.n_tracks = sl_lib.n_albums = 0; au_emptied = true; }
    else if (!on && au_emptied) { sl_lib.n_tracks = au_empty_saved_n; sl_lib.n_albums = au_empty_saved_a; au_emptied = false; }
}

#define AU_NAME(n, call) n,
static const char *const au_names[] = { AU_SCREENS(AU_NAME) };
#undef AU_NAME
#define AU_N_SCREENS ((int)(sizeof au_names / sizeof au_names[0]))
static void au_open(int k) {
    int i = 0;
#define AU_CASE(n, call) if (i++ == k) { call; return; }
    AU_SCREENS(AU_CASE)
#undef AU_CASE
}

static void au_apply(int look, int step, int lang) {
    global_settings.sloppy_theme = au_looks[look].theme; global_settings.sloppy_home_style = au_looks[look].home;
    global_settings.sloppy_language = lang; sl_text_step = step; global_settings.sloppy_text_size = step;
    au_reset();
    appearance_reload((void *)2); frame(700);
    while (npages > 1) { page_pop(); frame(300); }
}
static void au_visit(const char *name, int k, bool home) {
    char nm[64];
    if (home) { if (!au_open_home(k)) return; snprintf(nm, sizeof nm, "%s", name); }
    else {
        test_status = AUDIO_STATUS_PLAY; mini_update();
        global_settings.sloppy_notifications = 0;
        au_open(k); au_settle(); snprintf(nm, sizeof nm, "%s", name);
    }
    au_check(nm);
    au_reset();
    au_empty(false);
}
/* the matrix: looks x text sizes x languages; screens other than home
 * depend only on the theme, so they run in dark, light and Zule */
static void au_run(const char *dir, const int (*combos)[3], int ncombos) {
    au_dir = dir;
    if (dir) {
        char p[600]; snprintf(p, sizeof p, "mkdir -p '%s/shots'", dir); system(p);
        snprintf(p, sizeof p, "%s/audit.tsv", dir); au_tsv = fopen(p, "w");
        if (au_tsv) fprintf(au_tsv, "screen\tlook\trule\tpath\ttext\trect\tother\tdetail\n");
    }
    int th0 = global_settings.sloppy_theme, hs0 = global_settings.sloppy_home_style, lang0 = global_settings.sloppy_language, step0 = sl_text_step;
    int st0 = test_status, notes0 = global_settings.sloppy_notifications, books0 = fake_books_n;
    memset(au_rule_n, 0, sizeof au_rule_n); memset(au_allow, 0, sizeof au_allow); memset(au_defer, 0, sizeof au_defer); au_total = 0; au_nseen = 0;
    au_glyphs = au_glyph_missing = 0;
    au_syms = au_sym_foreign = 0;
    au_scripts = true; au_library(); au_scripts = false;   /* with the Korean, Japanese and Thai albums */
    int visits = 0;
    for (int c = 0; c < ncombos; c++) {
        int look = combos[c][0], step = combos[c][1], lang = combos[c][2];
        bool others = au_looks[look].home == (au_looks[look].theme == 3 ? 2 : 0);   /* cards-*, zule: the rest of the OS once per theme */
        au_apply(look, step, lang);
        for (int h = 0; h < AU_HOME_N; h++) {
            snprintf(au_look, sizeof au_look, "%s-%s-%s", au_looks[look].name, au_step_names[step], au_lang_names[lang]);
            au_visit(au_home_names[h], h, true); visits++;
        }
        if (!others) continue;
        snprintf(au_look, sizeof au_look, "%s-%s-%s", au_looks[look].theme == 3 ? "zule" : au_looks[look].theme ? "light" : "dark",
                 au_step_names[step], au_lang_names[lang]);
        for (int k = 0; k < AU_N_SCREENS; k++) {
            if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(au_names[k], getenv("SLOPPY_AUDIT_ONLY"))) continue;
            if (getenv("SLOPPY_AUDIT_LOG")) fprintf(stderr, "audit %s %s\n", au_look, au_names[k]);
            au_visit(au_names[k], k, false); visits++;
        }
    }
    au_library_done();
    global_settings.sloppy_theme = th0; global_settings.sloppy_home_style = hs0; global_settings.sloppy_language = lang0;
    sl_text_step = step0; global_settings.sloppy_text_size = step0; test_status = st0; global_settings.sloppy_notifications = notes0; fake_books_n = books0;
    appearance_reload((void *)2); frame(700); while (npages > 1) { page_pop(); frame(300); } mini_update(); frame(300);
    printf("audit: %d screens visited in %d looks; %d findings: a overlap %d, b clipped %d, c chrome %d, d tap %d, e contrast %d\n",
           visits, ncombos, au_total, au_rule_n[0], au_rule_n[1], au_rule_n[2], au_rule_n[3], au_rule_n[4]);
    printf("audit: allowed A1 own card %d, A2 over art %d, A3 under a full-screen surface %d, A4 marquee %d, A5 ellipsis %d, A6 Zule title %d, "
           "A7 scrolls clear %d, A8 fading %d, A9 catcher %d, A10 game art %d, A11 row takes the tap %d, "
           "A12 lyric lines not being sung %d, A13 icon beside its words %d, A14 toast over list content %d, A15 inactive control %d, "
           "A16 logotype %d, A17 count badge on its glyph %d\n",
           au_allow[1], au_allow[2], au_allow[3], au_allow[4], au_allow[5], au_allow[6], au_allow[7], au_allow[8], au_allow[9], au_allow[10], au_allow[11],
           au_allow[12], au_allow[13], au_allow[14], au_allow[15], au_allow[16], au_allow[17]);
    printf("audit: deferred contrast (real, not fixed here) D1 control centre glass %d, D3 white on Zule pink or the accent preview %d\n",
           au_defer[1], au_defer[3]);
    printf("audit: %d screens read with music playing and the mini player up\n", au_mini_checked);
    printf("audit: %ld letters on screen, %ld without a glyph\n", au_glyphs, au_glyph_missing);
    printf("audit: %ld symbols on screen, %ld not drawn by sl_sym\n", au_syms, au_sym_foreign);
    if (au_tsv) { fclose(au_tsv); au_tsv = NULL; }
}
/* the whole matrix: 7 looks x 4 sizes x 14 languages */
static void au_full(const char *dir) {
    ng_force_small = getenv("SLOPPY_AUDIT_SMALL_GLASS") != NULL;   /* =1: the backdrops as memory short of the floor draws them */
    static int combos[7 * 4 * (SL_LANG_LAST + 1)][3]; int n = 0;
    const char *q = getenv("SLOPPY_AUDIT_LOOKS");   /* e.g. "0,0,0;1,3,4": look, size, language */
    if (q) { int a, b, c, k; while (n < (int)ARRAYLEN(combos) && sscanf(q, "%d,%d,%d%n", &a, &b, &c, &k) == 3) {
        combos[n][0] = a; combos[n][1] = b; combos[n][2] = c; n++; q += k; if (*q == ';') q++; else break; } }
    else for (int look = 0; look < 7; look++) for (int step = 0; step < 4; step++) for (int lang = 0; lang <= SL_LANG_LAST; lang++) {
        combos[n][0] = look; combos[n][1] = step; combos[n][2] = lang; n++; }
    au_run(dir, (const int (*)[3])combos, n);
}
/* the suite's gate: a look of each kind, the extremes of size, RTL and a long language */
static void au_gate(void) {
    static const int combos[][3] = {
        { 0, 0, 0 }, { 1, 3, 4 }, { 6, 2, 3 }, { 1, 0, 5 }, { 0, 3, 1 },
        { 2, 0, 0 }, { 3, 3, 5 }, { 4, 3, 3 }, { 5, 0, 4 }, { 1, 3, 6 }, { 6, 0, 6 },
        { 0, 3, SL_LANG_RU }, { 6, 3, SL_LANG_PL }, { 2, 3, SL_LANG_JA },   /* the longest Cyrillic and Latin, CJK at the biggest */
    };
    au_run(getenv("SLOPPY_AUDIT_GATE_DIR"), combos, (int)(sizeof combos / sizeof combos[0]));
}
/* SLOPPY_AUDIT_SNAP=<dir> SLOPPY_AUDIT_TAG=before|after: clean shots of the
 * screens the audit's fixes touch, light and dark, as the audit opens them */
static int au_find(const char *name) { for (int k = 0; k < AU_N_SCREENS; k++) if (!strcmp(au_names[k], name)) return k; return -1; }
static void au_snap(const char *dir, const char *tag) {
    static const struct { const char *file, *screen; int home, step, lang; } shots[] = {
        { "cards-home", "home", 0, 0, 0 }, { "cards-home-idle", "home-idle", 0, 0, 0 }, { "cards-home-apps", "home-scrolled", 0, 0, 0 },
        { "cards-home-largest", "home", 0, 3, 0 }, { "cards-home-hebrew", "home", 0, 0, 4 },
        { "all-list-toast", "songs-toast", -1, 0, 0 }, { "all-list-banner", "banner-page", -1, 0, 0 }, { "home-toast", "toast-home", 0, 0, 0 },
        { "sort-sheet", "songs-sort-sheet", -1, 0, 0 }, { "song-sheet", "song-sheet", -1, 0, 0 }, { "playlist-sheet", "playlist-sheet", -1, 0, 0 },
        { "all-list", "songs", -1, 0, 0 }, { "albums", "albums", -1, 0, 0 },
        { "artists-largest", "artists", -1, 3, 0 }, { "library-largest", "library", -1, 3, 0 }, { "playroom-larger-arabic", "playroom", -1, 2, 5 },
        { "music-hebrew", "music", -1, 0, 4 }, { "albums-hebrew", "albums", -1, 0, 4 },
        { "wifi-password", "wifi-password", -1, 0, 0 }, { "settings-eq", "settings-eq", -1, 0, 0 },
        { "coin", "coin", -1, 0, 0 }, { "quiz", "quiz", -1, 0, 0 }, { "snake", "snake", -1, 0, 0 }, { "sloppy-fm", "sloppy-fm", -1, 0, 0 },
        { "graveyard", "graveyard", -1, 0, 0 }, { "magic-8-ball", "magic-8-ball", -1, 0, 0 }, { "lock", "lock", -1, 0, 0 },
        { "now-playing", "now-playing", -1, 0, 0 }, { "search", "search", -1, 0, 0 }, { "genre-albums", "genre-albums", -1, 0, 0 },
        { "queue", "queue", -1, 0, 0 }, { "bluetooth", "bluetooth", -1, 0, 0 }, { "wifi", "wifi", -1, 0, 0 },
        { "settings-time-power", "settings-time-power", -1, 0, 0 }, { "songs-picking", "songs-picking", -1, 0, 0 },
        { "now-playing-zule", "now-playing", -2, 0, 0 }, { "audiobook", "audiobook", -1, 0, 0 }, { "clicker", "clicker", -1, 0, 0 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(shots[i].file, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        for (int th = 0; th < 2; th++) {
            if (shots[i].home == -2 && th) continue;
            int look = shots[i].home == -2 ? 6 : (shots[i].home > 0 ? shots[i].home * 2 : 0) + th;
            au_apply(look, shots[i].step, shots[i].lang);
            int k = au_find(shots[i].screen);
            if (!strcmp(shots[i].screen, "home") || !strcmp(shots[i].screen, "home-idle") || !strcmp(shots[i].screen, "home-scrolled"))
                au_open_home(!strcmp(shots[i].screen, "home") ? AU_HOME : !strcmp(shots[i].screen, "home-idle") ? AU_HOME_IDLE : AU_HOME_SCROLLED);
            else if (k >= 0) { test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0; au_open(k); au_settle(); }
            frame(0);
            snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, shots[i].file, shots[i].home == -2 ? "zule" : th ? "light" : "dark", tag);
            shot_now(p);
            au_reset(); au_empty(false);
        }
    }
    au_library_done();
}
/* SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: every audit
 * screen, as the audit opens it, unmarked, in the looks the centring pass
 * reads: dark, light, Zule, Largest, Hebrew, Arabic, Chinese (SLOPPY_CENTER_LOOKS
 * "look,size,lang;..." to pick others; SLOPPY_AUDIT_ONLY to pick screens) */
static void center_shots(const char *dir, const char *tag) {
    static const struct { const char *name; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "dark-largest", 0, 3, 0 },
        { "light-he", 1, 0, 4 }, { "dark-ar-largest", 0, 3, 5 }, { "dark-zh", 0, 0, 6 },
    };
    const char *want = getenv("SLOPPY_CENTER_LOOKS");
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned l = 0; l < sizeof looks / sizeof looks[0]; l++) {
        if (want && !strstr(want, looks[l].name)) continue;
        au_apply(looks[l].look, looks[l].step, getenv("SLOPPY_CENTER_LANG") ? atoi(getenv("SLOPPY_CENTER_LANG")) : looks[l].lang);   /* =<n>: every look in that language */
        for (int h = 0; h < AU_HOME_N; h++) {
            if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(au_home_names[h], getenv("SLOPPY_AUDIT_ONLY"))) continue;
            if (!au_open_home(h)) continue;
            frame(0); snprintf(p, sizeof p, "%s/%s__%s-%s.ppm", dir, au_home_names[h], looks[l].name, tag); shot_now(p);
            au_reset(); au_empty(false);
        }
        for (int k = 0; k < AU_N_SCREENS; k++) {
            if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(au_names[k], getenv("SLOPPY_AUDIT_ONLY"))) continue;
            test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            au_open(k); au_settle(); frame(0);
            snprintf(p, sizeof p, "%s/%s__%s-%s.ppm", dir, au_names[k], looks[l].name, tag); shot_now(p);
            au_reset(); au_empty(false);
        }
    }
    au_library_done();
}
/* SLOPPY_SET_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: Settings and its
 * sub-pages, light and dark, as the audit opens them; each layout of the
 * main page when there is more than one */
static int set_shots_layouts(void) { return 2; }
static const char *set_shots_layout(int i) { global_settings.sloppy_settings_tiles = i; return i ? "-tiles" : ""; }
static void set_shots(const char *dir, const char *tag) {
    static const struct { const char *file, *screen; int step, lang, scroll; } shots[] = {
        { "settings", "settings", 0, 0, 0 }, { "settings-bottom", "settings", 0, 0, 1 },
        { "settings-largest", "settings", 3, 0, 0 }, { "settings-hebrew", "settings", 0, 4, 0 }, { "settings-arabic-larger", "settings", 2, 5, 0 },
        { "sound", "settings-sound", 0, 0, 0 }, { "sound-bottom", "settings-sound", 0, 0, 1 }, { "sound-largest", "settings-sound", 3, 0, 0 },
        { "bluetooth", "bluetooth", 0, 0, 0 }, { "wifi", "wifi", 0, 0, 0 },
        { "customization", "settings-customization", 0, 0, 0 }, { "customization-bottom", "settings-customization", 0, 0, 1 },
        { "display", "settings-display", 0, 0, 0 }, { "time-power", "settings-time-power", 0, 0, 0 },
        { "playback", "settings-playback", 0, 0, 0 }, { "buttons", "settings-buttons", 0, 0, 0 },
        { "advanced", "settings-advanced", 0, 0, 0 }, { "language", "settings-language", 0, 0, 0 },
        { "notification-center", "settings-shortcuts", 0, 0, 0 }, { "scrobbling", "settings-scrobble", 0, 0, 0 },
        { "library", "library", 0, 0, 0 }, { "text-size", "settings-text-size", 0, 0, 0 },
        { "search-results", "", 0, 0, 0 }, { "sound-hebrew", "settings-sound", 0, 4, 0 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (int lay = 0; lay < set_shots_layouts(); lay++)
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        if (lay && strcmp(shots[i].screen, "settings")) continue;   /* the layouts differ on the main page only */
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(shots[i].file, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        for (int th = 0; th < 2; th++) {
            au_apply(th, shots[i].step, shots[i].lang);
            set_shots_layout(lay);
            test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            if (!shots[i].screen[0]) { settings_search_page(); au_settle(); sq_add("V"); sq_add("O"); sq_add("L"); frame(300); }
            else au_open(au_find(shots[i].screen));
            au_settle();
            if (shots[i].scroll) {
                lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
                lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
            }
            frame(0);
            snprintf(p, sizeof p, "%s/%s%s-%s-%s.ppm", dir, shots[i].file, set_shots_layout(lay), th ? "light" : "dark", tag);
            shot_now(p);
            au_reset(); au_empty(false);
        }
    }
    au_library_done();
}
/* SLOPPY_MUSIC_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the Music menu in
 * every look, size and script, the rows whose chevrons mirror in RTL, and
 * screens whose words were translated in that pass */
struct au_mshot { const char *screen, *variant; int look, step, lang, scroll; };
static void au_mshots(const char *dir, const char *tag, const struct au_mshot *shots, unsigned n);
static void music_shots(const char *dir, const char *tag) {
    static const struct au_mshot shots[] = {
        { "music", "dark", 0, 0, 0, 0 }, { "music", "light", 1, 0, 0, 0 }, { "music", "zule", 6, 0, 0, 0 },
        { "music", "largest-dark", 0, 3, 0, 0 }, { "music", "largest-light", 1, 3, 0, 0 }, { "music", "largest-zule", 6, 3, 0, 0 },
        { "music", "he-dark", 0, 0, 4, 0 }, { "music", "he-light", 1, 0, 4, 0 }, { "music", "ar-dark", 0, 0, 5, 0 },
        { "music", "ar-largest-light", 1, 3, 5, 0 }, { "music", "he-zule", 6, 0, 4, 0 },
        { "music", "es-dark", 0, 0, 1, 0 }, { "music", "de-light", 1, 0, 3, 0 }, { "music", "bottom-dark", 0, 0, 0, 1 },
        { "music", "bottom-light", 1, 0, 0, 1 }, { "music", "bottom-largest-dark", 0, 3, 0, 1 },
        { "library", "he-dark", 0, 0, 4, 0 }, { "playroom", "ar-dark", 0, 0, 5, 0 }, { "genres", "he-light", 1, 0, 4, 0 },
        { "for-you", "ar-light", 1, 0, 5, 0 }, { "wifi", "he-dark", 0, 0, 4, 0 }, { "playlists", "ar-dark", 0, 0, 5, 0 },
        { "settings-eq-profiles", "he-dark", 0, 0, 4, 0 }, { "playlist-open", "he-light", 1, 0, 4, 0 },
        { "songs-picking", "es-dark", 0, 0, 1, 0 }, { "songs-picking", "de-light", 1, 0, 3, 0 },
        { "songs-picking", "he-dark", 0, 0, 4, 0 }, { "songs-picking", "ar-light", 1, 0, 5, 0 },
        { "audiobooks", "de-dark", 0, 0, 3, 0 }, { "internet-radio", "es-dark", 0, 0, 1, 0 },
        { "playroom", "he-light", 1, 2, 4, 0 }, { "metronome", "de-dark", 0, 0, 3, 0 }, { "song-info", "es-light", 1, 0, 1, 0 },
        { "queue", "ar-dark", 0, 0, 5, 0 }, { "album", "de-dark", 0, 0, 3, 0 }, { "albums", "es-light", 1, 0, 1, 0 },
        { "settings-eq", "es-dark", 0, 0, 1, 0 }, { "bluetooth", "he-dark", 0, 0, 4, 0 }, { "spacey-lines", "de-dark", 0, 0, 3, 0 },
        { "albums", "zule", 6, 0, 0, 0 }, { "playlists", "zule", 6, 0, 0, 0 }, { "library", "zule-largest", 6, 3, 0, 0 },
        { "home", "he-dark", 0, 0, 4, 0 }, { "home", "ar-light", 1, 0, 5, 0 }, { "home", "es-dark", 0, 0, 1, 0 },
        { "home-scrolled", "he-dark", 0, 0, 4, 0 }, { "control-centre", "he-dark", 0, 0, 4, 0 }, { "search-results", "he-dark", 0, 0, 4, 0 },
        { "artists", "ar-dark", 0, 0, 5, 0 }, { "songs", "he-dark", 0, 0, 4, 0 }, { "wrapped", "he-dark", 0, 0, 4, 0 },
        { "music-arrange", "es-dark", 0, 0, 1, 0 }, { "years", "ar-dark", 0, 0, 5, 0 }, { "search", "de-dark", 0, 0, 3, 0 },
    };
    au_mshots(dir, tag, shots, sizeof shots / sizeof shots[0]);
}
/* SLOPPY_A1_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: Gate 1 batch A, the
 * rows that mirror in Hebrew and Arabic (and a Hebrew title in an English
 * list), and the screens whose words were translated */
static void a1_shots(const char *dir, const char *tag) {
    static const struct au_mshot shots[] = {
        { "songs", "he-dark", 0, 0, 4, 0 }, { "songs", "ar-light", 1, 0, 5, 0 }, { "songs", "dark", 0, 0, 0, 0 },
        { "album", "he-dark", 0, 0, 4, 0 }, { "album", "ar-light", 1, 0, 5, 0 },
        { "queue", "he-dark", 0, 0, 4, 0 }, { "queue", "ar-light", 1, 0, 5, 0 }, { "queue", "light", 1, 0, 0, 0 },
        { "now-playing-status", "he-dark", 0, 0, 4, 0 }, { "now-playing-status", "ar-light", 1, 0, 5, 0 },
        { "now-playing-status", "light", 1, 0, 0, 0 }, { "search-results", "he-light", 1, 0, 4, 0 },
        { "lyrics", "es-dark", 0, 0, 1, 0 }, { "lyrics", "zh-light", 1, 0, 6, 0 },
        { "chess", "es-dark", 0, 0, 1, 0 }, { "chess", "he-light", 1, 0, 4, 0 }, { "coin", "fr-dark", 0, 0, 2, 0 },
        { "magic-8-ball", "de-dark", 0, 0, 3, 0 }, { "quiz", "he-dark", 0, 0, 4, 0 }, { "ragdoll", "ar-light", 1, 0, 5, 0 },
        { "sloppy-fm", "zh-dark", 0, 0, 6, 0 }, { "sloppy-fm", "de-dark", 0, 0, 3, 0 }, { "graveyard", "es-light", 1, 0, 1, 0 },
        { "pet", "fr-dark", 0, 0, 2, 0 }, { "freaky-spoon", "de-dark", 0, 0, 3, 0 }, { "playlist-new", "es-dark", 0, 0, 1, 0 },
        { "playlist-sheet", "he-dark", 0, 0, 4, 0 }, { "settings-time-power", "de-light", 1, 0, 3, 0 },
        { "spectrum", "fr-dark", 0, 0, 2, 0 }, { "coverflow", "es-dark", 0, 0, 1, 0 }, { "files", "zh-dark", 0, 0, 6, 0 },
        { "settings-accent", "fr-light", 1, 0, 2, 0 },
    };
    au_mshots(dir, tag, shots, sizeof shots / sizeof shots[0]);
}
/* SLOPPY_D_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: gate 1 batch D's
 * screens in dark and light: Settings > Sound (its Volume card and its
 * Soundfield card), the control centre and Display & lock (brightness),
 * the lock screen, the screenshot and picture viewers, the wallpaper page.
 * scroll 1 here: the Soundfield card in view. */
static void d_shots(const char *dir, const char *tag) {
    static const struct au_mshot shots[] = {
        { "settings-sound", "dark", 0, 0, 0, 0 }, { "settings-sound", "light", 1, 0, 0, 0 },
        { "settings-sound", "soundfield-dark", 0, 0, 0, 2 }, { "settings-sound", "soundfield-light", 1, 0, 0, 2 },
        { "control-centre", "dark", 0, 0, 0, 0 }, { "control-centre", "light", 1, 0, 0, 0 },
        { "settings-display", "dark", 0, 0, 0, 0 }, { "settings-display", "light", 1, 0, 0, 0 },
        { "lock", "dark", 0, 0, 0, 0 }, { "lock", "light", 1, 0, 0, 0 },
        { "screenshot-viewer", "dark", 0, 0, 0, 0 }, { "screenshot-viewer", "light", 1, 0, 0, 0 },
        { "files-picture", "dark", 0, 0, 0, 0 }, { "files-picture", "light", 1, 0, 0, 0 },
        { "settings-wallpaper", "dark", 0, 0, 0, 0 }, { "settings-wallpaper", "light", 1, 0, 0, 0 },
    };
    au_mshots(dir, tag, shots, sizeof shots / sizeof shots[0]);
    if (strcmp(tag, "after")) return;
    /* the new states, dark and light: Mono on, the wallpaper sheet, Display
     * & lock in the night range (veil on), a charger waking a dark screen */
    char p[600];
    au_library();
    for (int look = 0; look < 2; look++) {
        const char *lk = look ? "light" : "dark";
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr("extras", getenv("SLOPPY_AUDIT_ONLY"))) break;
        au_apply(look, 0, 0); test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        snd_mono(1); sound_page(); au_settle();
        lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1), *l = find_label(pages[npages - 1], "SOUNDFIELD");
        if (l) { lv_obj_update_layout(b); lv_area_t la, ba; lv_obj_get_coords(l, &la); lv_obj_get_coords(b, &ba);
                 lv_obj_scroll_to_y(b, lv_obj_get_scroll_y(b) + la.y1 - ba.y1 - 16, LV_ANIM_OFF); frame(300); }
        snprintf(p, sizeof p, "%s/mono-on-%s-after.ppm", dir, lk); shot_now(p);
        snd_mono(0); au_reset();
        au_shot(true); au_settle(); wallpaper_set_offer(shot_view_path); frame(600);
        snprintf(p, sizeof p, "%s/wallpaper-set-sheet-%s-after.ppm", dir, lk); shot_now(p);
        acts_close(); frame(400); au_reset();
        int b0 = global_settings.brightness; sl_bright_set(MIN_BRIGHTNESS_SETTING - 4); qp_display_page(); au_settle();
        snprintf(p, sizeof p, "%s/night-display-page-%s-after.ppm", dir, lk); shot_now(p);
        global_settings.sloppy_night_dim = 0; global_settings.brightness = b0; sl_night_apply(); au_reset();
        bool run = sl_power_running; sl_power_running = true;
        power_sounds(60, false); for (int i = 0; i < 40 && cb.root; i++) frame(100);
        sl_power.mode = SL_POWER_DARK; sl_power_apply(); power_sounds(60, true); frame(0); frame(700);
        snprintf(p, sizeof p, "%s/charge-wake-%s-after.ppm", dir, lk); shot_now(p);
        sl_power.mode = SL_POWER_ACTIVE; sl_power_apply(); for (int i = 0; i < 20; i++) frame(100); sl_power_running = run;
        au_reset(); au_empty(false);
    }
    au_library_done();
}
static void au_mshots(const char *dir, const char *tag, const struct au_mshot *shots, unsigned n) {
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < n; i++) {
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(shots[i].screen, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        int k = au_find(shots[i].screen);
        bool home = !strcmp(shots[i].screen, "home") || !strcmp(shots[i].screen, "home-scrolled");
        if (k < 0 && !home) continue;
        au_apply(shots[i].look, shots[i].step, shots[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        if (home) au_open_home(!strcmp(shots[i].screen, "home") ? AU_HOME : AU_HOME_SCROLLED);
        else { au_open(k); au_settle(); }
        if (shots[i].scroll == 2) {   /* the Soundfield card in view */
            lv_obj_t *l = find_label(pages[npages - 1], "SOUNDFIELD");
            if (!l) l = find_label(pages[npages - 1], "Soundfield");
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            if (l) {
                lv_obj_update_layout(b); lv_area_t la, ba; lv_obj_get_coords(l, &la); lv_obj_get_coords(b, &ba);
                lv_obj_scroll_to_y(b, lv_obj_get_scroll_y(b) + la.y1 - ba.y1 - 16, LV_ANIM_OFF); frame(300);
            }
        } else if (shots[i].scroll) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, shots[i].screen, shots[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
    }
    au_library_done();
}
/* SLOPPY_B2_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 2's screens:
 * Wi-Fi (on, joined), joining by hand, the transfer page, Bluetooth, the
 * control centre folded and grown, its shortcut list, the screenshot toast
 * and viewer, and the pages a newer wireless chip gets. A screen this build
 * lacks is skipped. */
static void b2_wifi(bool on) {
    while (wf_busy) frame(100);
    global_settings.sloppy_wifi = on; wf_arg_on = on; wf_begin(WF_JOB_POWER);
    for (int i = 0; i < 50 && wf_busy; i++) frame(100);
    if (!on) return;
    wf_begin(WF_JOB_SCAN); for (int i = 0; i < 50 && wf_busy; i++) frame(100);
    wf_join("Home network", NULL); for (int i = 0; i < 50 && (wf_busy || wf_join_pending); i++) frame(100);
    for (int i = 0; i < 30; i++) frame(200);   /* "Connected" has come and gone */
}
static void b2_shot_toast(void) {
    au_open_home(AU_HOME);
    au_shot_room();
    sl_screenshot();
    for (int i = 0; i < 300 && !sc.up; i++) { usleep(10000); frame(20); }
    frame(600);
}
/* the panel with Screenshot and Transfer chosen, Transfer serving: its address on the tile */
static void b2_cc_tiles(void) {
    strlcpy((char *)global_settings.sloppy_qp_tiles, "012345gh", sizeof global_settings.sloppy_qp_tiles);
    quick_panel_rebuild();
    wf_share_cb(NULL); for (int i = 0; i < 30 && (wf_busy || !wf_share_on); i++) frame(100);
    frame(400); ccx_open(); frame(600);
}
static void b2_shots(const char *dir, const char *tag) {
    static const struct { const char *screen, *variant; int look, step, lang, wifi; } shots[] = {
        { "wifi", "dark", 0, 0, 0, 1 }, { "wifi", "light", 1, 0, 0, 1 }, { "wifi", "zule", 6, 0, 0, 1 },
        { "wifi", "largest-dark", 0, 3, 0, 1 }, { "wifi", "he-dark", 0, 0, 4, 1 }, { "wifi", "ar-light", 1, 0, 5, 1 },
        { "wifi-bottom", "dark", 0, 0, 0, 1 }, { "wifi-bottom", "light", 1, 0, 0, 1 }, { "wifi-bottom", "zule", 6, 0, 0, 1 },
        { "wifi-other", "dark", 0, 0, 0, 1 }, { "wifi-other", "light", 1, 0, 0, 1 }, { "wifi-other", "zule", 6, 0, 0, 1 },
        { "wifi-other", "largest-dark", 0, 3, 0, 1 }, { "wifi-other", "he-dark", 0, 0, 4, 1 }, { "wifi-other", "ar-light", 1, 0, 5, 1 },
        { "wifi-other-open", "dark", 0, 0, 0, 1 }, { "wifi-other-open", "light", 1, 0, 0, 1 },
        { "wifi-password", "dark", 0, 0, 0, 1 }, { "wifi-password", "light", 1, 0, 0, 1 },
        { "wifi-transfer", "dark", 0, 0, 0, 1 }, { "wifi-transfer", "light", 1, 0, 0, 1 }, { "wifi-transfer", "zule", 6, 0, 0, 1 },
        { "wifi-aic", "dark", 0, 0, 0, 1 }, { "wifi-aic", "light", 1, 0, 0, 1 }, { "wifi-aic", "zule", 6, 0, 0, 1 },
        { "wifi-aic", "largest-dark", 0, 3, 0, 1 }, { "wifi-aic", "he-dark", 0, 0, 4, 1 }, { "wifi-aic", "ar-light", 1, 0, 5, 1 },
        { "bluetooth", "dark", 0, 0, 0, 0 }, { "bluetooth", "light", 1, 0, 0, 0 }, { "bluetooth", "zule", 6, 0, 0, 0 },
        { "bluetooth-aic", "dark", 0, 0, 0, 0 }, { "bluetooth-aic", "light", 1, 0, 0, 0 }, { "bluetooth-aic", "zule", 6, 0, 0, 0 },
        { "bluetooth-aic", "largest-dark", 0, 3, 0, 0 }, { "bluetooth-aic", "he-dark", 0, 0, 4, 0 }, { "bluetooth-aic", "ar-light", 1, 0, 5, 0 },
        { "control-centre", "dark", 0, 0, 0, 1 }, { "control-centre", "light", 1, 0, 0, 1 }, { "control-centre", "zule", 6, 0, 0, 1 },
        { "control-centre-grown", "dark", 0, 0, 0, 1 }, { "control-centre-grown", "light", 1, 0, 0, 1 },
        { "control-centre-grown", "zule", 6, 0, 0, 1 }, { "control-centre-grown", "largest-dark", 0, 3, 0, 1 },
        { "control-centre-grown", "he-dark", 0, 0, 4, 1 }, { "control-centre-grown", "ar-light", 1, 0, 5, 1 },
        { "control-centre-grown", "de-dark", 0, 0, 3, 1 },
        { "control-centre-tiles", "dark", 0, 0, 0, 1 }, { "control-centre-tiles", "light", 1, 0, 0, 1 },
        { "control-centre-tiles", "zule", 6, 0, 0, 1 }, { "control-centre-tiles", "he-dark", 0, 0, 4, 1 },
        { "settings-shortcuts", "dark", 0, 0, 0, 0 }, { "settings-shortcuts", "light", 1, 0, 0, 0 },
        { "screenshot-toast", "dark", 0, 0, 0, 0 }, { "screenshot-toast", "light", 1, 0, 0, 0 }, { "screenshot-toast", "zule", 6, 0, 0, 0 },
        { "screenshot-toast", "largest-dark", 0, 3, 0, 0 }, { "screenshot-toast", "he-dark", 0, 0, 4, 0 }, { "screenshot-toast", "ar-light", 1, 0, 5, 0 },
        { "screenshot-flash", "dark", 0, 0, 0, 0 },
        { "screenshot-viewer", "dark", 0, 0, 0, 0 }, { "screenshot-viewer", "light", 1, 0, 0, 0 }, { "screenshot-viewer", "zule", 6, 0, 0, 0 },
        { "screenshot-viewer", "he-dark", 0, 0, 4, 0 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        const char *s = shots[i].screen;
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        bool bottom = !strcmp(s, "wifi-bottom"), toast_shot = !strcmp(s, "screenshot-toast"), tiles = !strcmp(s, "control-centre-tiles");
        int k = au_find(bottom ? "wifi" : s);
        if (k < 0 && !toast_shot && !tiles) continue;
        au_apply(shots[i].look, shots[i].step, shots[i].lang);
        b2_wifi(shots[i].wifi);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        if (toast_shot) b2_shot_toast();
        else if (tiles) b2_cc_tiles();
        else if (strstr(s, "-current")) au_open(k);   /* the flash at its height */
        else { au_open(k); au_settle(); }
        if (bottom) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, shots[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
        if (tiles) { global_settings.sloppy_qp_tiles[0] = '\0'; quick_panel_rebuild(); }
    }
    b2_wifi(false);
    au_library_done();
}

/* SLOPPY_B3_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 3's screens:
 * Now Playing (shuffle on, repeat one), the same with a book playing, and
 * the control centre with the sleep timer running. The Settings pages it
 * touches are shot whole by SLOPPY_SC_SHOTS. */
static void b3_shots(const char *dir, const char *tag) {
    static const struct { const char *screen, *variant; int look, step, lang; } shots[] = {
        { "now-playing", "dark", 0, 0, 0 }, { "now-playing", "light", 1, 0, 0 }, { "now-playing", "zule", 6, 0, 0 },
        { "now-playing", "largest-dark", 0, 3, 0 }, { "now-playing", "he-dark", 0, 0, 4 }, { "now-playing", "ar-light", 1, 0, 5 },
        { "now-playing-classic", "dark", 0, 0, 0 }, { "now-playing-classic", "light", 1, 0, 0 },
        { "now-playing-book", "dark", 0, 0, 0 }, { "now-playing-book", "light", 1, 0, 0 },
        { "control-centre", "dark", 0, 0, 0 }, { "control-centre", "light", 1, 0, 0 }, { "control-centre", "zule", 6, 0, 0 },
        { "control-centre", "he-dark", 0, 0, 4 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        const char *s = shots[i].screen;
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        bool classic = !strcmp(s, "now-playing-classic"), book = !strcmp(s, "now-playing-book");
        int k = au_find(classic || book ? "now-playing" : s);
        if (k < 0) continue;
        global_settings.sloppy_np_style = classic;
        au_apply(shots[i].look, shots[i].step, shots[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        global_settings.playlist_shuffle = true; global_settings.repeat_mode = REPEAT_ONE;
        if (!strcmp(s, "control-centre")) sleep_set(2);
        au_open(k); au_settle();
        if (book) { np_book_buttons(true); frame(200); }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, shots[i].variant, tag);
        shot_now(p);
        if (book) np_book_buttons(false);
        sleep_set(0);
        global_settings.playlist_shuffle = false; global_settings.repeat_mode = REPEAT_OFF; global_settings.sloppy_np_style = 0;
        au_reset(); au_empty(false);
    }
    au_library_done();
}

/* SLOPPY_SC_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: every page Settings
 * reaches, each as long as it is: the body scrolled a screen at a time,
 * each frame written with its scroll (sc-stitch.py joins them). No mini
 * player, so the page's own bottom shows. SLOPPY_SC_VARIANTS picks the
 * looks ("dark,light,true-dark,zule,largest-dark,he-dark,tiles-dark",
 * the first two by default). */
static const char *const sc_pages[] = {
    "settings", "settings-search", "bluetooth", "bluetooth-aic", "settings-bt-forget", "settings-receive",
    "wifi", "wifi-aic", "wifi-password", "wifi-other", "wifi-transfer", "settings-scrobble", "settings-scrobble-token",
    "settings-playback", "settings-eq", "settings-eq-autoeq", "settings-eq-band", "settings-eq-band-profile", "settings-eq-peq", "settings-eq-band-peq", "settings-tone", "settings-tone-profile", "settings-eq-profiles", "settings-eq-find", "settings-eq-save", "settings-eq-found",
    "settings-sound", "settings-sounds", "settings-customization", "settings-theme", "settings-accent",
    "settings-home-style", "settings-layout", "settings-icon", "settings-wallpaper", "settings-lock-wallpaper",
    "settings-lock-player", "settings-np-style", "settings-coverflow-style", "settings-language", "settings-text-size",
    "settings-keyboards", "settings-lyrics-colour", "settings-screen-sleep", "settings-buttons", "settings-shortcuts", "settings-time-power",
    "settings-time-zones", "library", "settings-advanced", "settings-reset", "settings-classic", "settings-dual-boot",
    "settings-spotify", "settings-sleep", "settings-display", "settings-library-options",
};
static void sc_shots(const char *dir, const char *tag) {
    static const struct { const char *name; int theme, step, lang, tiles, big; } looks[] = {
        { "big-dark", 0, 0, 0, 0, 1 }, { "big-light", 1, 0, 0, 0, 1 }, { "big-zule", 3, 0, 0, 0, 1 },
        { "big-largest-dark", 0, 3, 0, 0, 1 }, { "big-largest-light", 1, 3, 0, 0, 1 }, { "big-he-dark", 0, 0, 4, 0, 1 },
        { "big-tiles-dark", 0, 0, 0, 1, 1 }, { "big-tiles-light", 1, 0, 0, 1, 1 },
        { "dark", 0, 0, 0, 0 }, { "light", 1, 0, 0, 0 }, { "true-dark", 2, 0, 0, 0 }, { "zule", 3, 0, 0, 0 },
        { "largest-dark", 0, 3, 0, 0 }, { "largest-light", 1, 3, 0, 0 }, { "he-dark", 0, 0, 4, 0 }, { "he-light", 1, 0, 4, 0 },
        { "ar-dark", 0, 0, 5, 0 }, { "ar-light", 1, 0, 5, 0 },
        { "tiles-dark", 0, 0, 0, 1 }, { "tiles-light", 1, 0, 0, 1 },
        { "tiles-zule", 3, 0, 0, 1 }, { "tiles-largest-dark", 0, 3, 0, 1 }, { "tiles-he-dark", 0, 0, 4, 1 },
        { "big-tiles-zule", 3, 0, 0, 1, 1 }, { "big-tiles-largest-dark", 0, 3, 0, 1, 1 }, { "big-tiles-he-dark", 0, 0, 4, 1, 1 },
        { "tiles-true-dark", 2, 0, 0, 1, 0 }, { "tiles-largest-light", 1, 3, 0, 1, 0 }, { "tiles-he-light", 1, 0, 4, 1, 0 },
        { "tiles-ar-dark", 0, 0, 5, 1, 0 }, { "tiles-ar-light", 1, 0, 5, 1, 0 }, { "tiles-zh-dark", 0, 0, 6, 1, 0 }, { "tiles-zh-light", 1, 0, 6, 1, 0 },
        { "big-tiles-ar-dark", 0, 0, 5, 1, 1 }, { "big-tiles-zh-dark", 0, 0, 6, 1, 1 },
    };
    const char *want = getenv("SLOPPY_SC_VARIANTS") ? getenv("SLOPPY_SC_VARIANTS") : "dark,light";
    char p[700]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    b2_wifi(true);
    for (unsigned v = 0; v < sizeof looks / sizeof looks[0]; v++) {
        char key[40]; snprintf(key, sizeof key, ",%s,", looks[v].name);
        char w2[700]; snprintf(w2, sizeof w2, ",%s,", want);
        if (!strstr(w2, key)) continue;
        /* SLOPPY_SC_PAGES="a,b": these pages instead of Settings' */
        static char pl[1024]; static const char *pv[64]; unsigned np = 0;
        if (getenv("SLOPPY_SC_PAGES")) { strlcpy(pl, getenv("SLOPPY_SC_PAGES"), sizeof pl); for (char *t = strtok(pl, ","); t && np < 64; t = strtok(NULL, ",")) pv[np++] = t; }
        else for (unsigned i = 0; i < sizeof sc_pages / sizeof sc_pages[0] && np < 64; i++) pv[np++] = sc_pages[i];
        for (unsigned i = 0; i < np; i++) {
            const char *s = pv[i];
            if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
            int k = au_find(s);
            if (k < 0) continue;
            global_settings.sloppy_theme = looks[v].theme;
            global_settings.sloppy_home_style = looks[v].theme == 3 ? 2 : 0;
            global_settings.sloppy_language = looks[v].lang; sl_text_step = looks[v].step; global_settings.sloppy_text_size = looks[v].step;
            au_reset();
            global_settings.sloppy_settings_tiles = looks[v].tiles;
            global_settings.sloppy_big_icons = looks[v].big;
            appearance_reload((void *)2); frame(700);
            while (npages > 1) { page_pop(); frame(300); }
            test_status = 0; mini_update(); global_settings.sloppy_notifications = 0;
            /* Tiles: the hub, and the pages its tiles open, opened by a tap on the tile */
            static const char *const tile_of[][3] = {
                { "bluetooth", "Bluetooth", NULL }, { "wifi", "Wi-Fi", NULL }, { "settings-scrobble", "Scrobbling", NULL },
                { "settings-playback", "Playback", NULL }, { "settings-sound", "Sound", NULL }, { "settings-sounds", "Sound effects", NULL },
                { "settings-customization", "Customization", NULL }, { "settings-screen-sleep", "Screen sleep", NULL },
                { "settings-buttons", "Buttons", NULL }, { "settings-shortcuts", "Notification Center", NULL },
                { "settings-time-power", "Time and power", NULL }, { "library", "Library", NULL }, { "settings-advanced", "Advanced", NULL },
                { "settings-theme", "Customization", "Theme" }, { "settings-accent", "Customization", "Accent color" },
                { "settings-keyboards", "Customization", "Keyboards" }, { "settings-eq", "Playback", "Equalizer" },
                { "settings-time-zones", "Time and power", "Time zone" }, { "settings-library-options", "Library", "Library options" },
                { "settings-tone", "Sound", "Tone tuning" },
            };
            int t = -1;
            for (unsigned j = 0; j < sizeof tile_of / sizeof tile_of[0]; j++) if (!strcmp(tile_of[j][0], s)) t = (int)j;
            /* the rest (and every page in Zule, which has no tiles) as the List layout opens them */
            if (looks[v].tiles && t >= 0 && looks[v].theme != 3) {
                system_page(); au_settle();
                for (int step = 1; step < 3 && tile_of[t][step]; step++) {
                    lv_obj_t *l = find_label(pages[npages - 1], sl_tr(tile_of[t][step]));
                    if (l) { lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_CLICKED, NULL); au_settle(); }
                    else { while (npages > 1) { page_pop(); frame(300); } au_open(k); break; }
                }
            }
            else if (!strcmp(s, "settings-search")) { settings_search_page(); au_settle(); sq_add("V"); sq_add("O"); sq_add("L"); frame(300); }
            else { au_open(k); }
            au_settle();
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b);
            bool scrolls = lv_obj_has_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            snprintf(p, sizeof p, "%s/%s-%s-%s.txt", dir, s, looks[v].name, tag);
            FILE *list = fopen(p, "w");
            for (int n = 0; n < 12; n++) {
                int y = lv_obj_get_scroll_y(b);
                frame(0);
                snprintf(p, sizeof p, "%s/%s-%s-%s-%02d.ppm", dir, s, looks[v].name, tag, n);
                shot_now(p);
                if (list) fprintf(list, "%s %d %d\n", p, y, SB_H + SL_HDR_H);
                if (!scrolls || lv_obj_get_scroll_bottom(b) <= 0) break;
                lv_obj_scroll_by(b, 0, -LV_MIN(560, lv_obj_get_scroll_bottom(b)), LV_ANIM_OFF);
                frame(100); frame(100);
            }
            if (list) fclose(list);
            au_reset(); au_empty(false);
        }
    }
    b2_wifi(false);
    au_library_done();
}

/* SLOPPY_SHEET_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the bottom sheets
 * (song, album, artist, play menu, Sort by, a file, the Arcade's menu) in
 * English, Hebrew and Arabic */
static void sheet_shots(const char *dir, const char *tag) {
    static const struct { const char *name; int look, lang; } looks[] = { { "dark", 0, 0 }, { "he-dark", 0, 4 }, { "ar-light", 1, 5 } };
    static const char *const screens[] = { "song-sheet", "album-sheet", "artist-sheet", "play-menu", "songs-sort-sheet", "files-sheet", "files-folder-sheet" };
    char p[700]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned v = 0; v < sizeof looks / sizeof looks[0]; v++) {
        au_apply(looks[v].look, 0, looks[v].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        for (unsigned i = 0; i < sizeof screens / sizeof screens[0]; i++) {
            int k = au_find(screens[i]);
            if (k < 0) continue;
            au_open(k); au_settle(); frame(0);
            snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, screens[i], looks[v].name, tag); shot_now(p);
            au_reset(); au_empty(false);
        }
        /* the Arcade's menu over a game */
        static uint8_t rom[32768];
        arc_root = FAKE_CARD;
        mkdir(FAKE_CARD "/Games"); mkdir(FAKE_CARD "/Games/GB");
        gbt_make(rom, "SLOPPYTEST", 0);
        int fd = open(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { if (write(fd, rom, sizeof rom) != (ssize_t)sizeof rom) fprintf(stderr, "sheet_shots: short write\n"); sim_close(fd); }
        arcade_page(); frame(300);
        for (int k = 0; k < arc_n; k++) if (!strcmp(arc_roms[k].name, "Test Cart")) { arc_play(k); break; }
        for (int i = 0; i < 30; i++) frame(17);
        if (arc.page) {
            arc_screen_cb(NULL); frame(300);
            snprintf(p, sizeof p, "%s/arcade-menu-%s-%s.ppm", dir, looks[v].name, tag); shot_now(p);
            acts_close(); frame(300);
        }
        au_reset(); au_empty(false);
    }
    au_library_done();
}

/* SLOPPY_TM_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: Settings tiles in
 * motion, Dark and Light: a tile held down (Wi-Fi), and the Sound page
 * sliding in from its tile, 60 and 140 ms in, then settled */
static void tm_shots(const char *dir, const char *tag) {
    static const struct { const char *name; int theme; } looks[] = { { "tiles-dark", 0 }, { "tiles-light", 1 } };
    char p[700]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned v = 0; v < 2; v++) {
        global_settings.sloppy_theme = looks[v].theme; global_settings.sloppy_home_style = 0;
        global_settings.sloppy_language = 0; sl_text_step = 0; global_settings.sloppy_text_size = 0;
        au_reset();
        global_settings.sloppy_settings_tiles = 1; global_settings.sloppy_big_icons = 0;
        appearance_reload((void *)2); frame(700);
        while (npages > 1) { page_pop(); frame(300); }
        test_status = 0; mini_update(); global_settings.sloppy_notifications = 0;
        system_page(); au_settle();
        lv_obj_t *l = find_label(pages[npages - 1], "Wi-Fi");
        if (l) {
            lv_obj_t *t = lv_obj_get_parent(l);
            lv_obj_add_state(t, LV_STATE_PRESSED); frame(100); frame(100);
            snprintf(p, sizeof p, "%s/press-%s-%s.ppm", dir, looks[v].name, tag); shot_now(p);
            lv_obj_remove_state(t, LV_STATE_PRESSED); frame(300); frame(300);
        }
        l = find_label(pages[npages - 1], "Sound");
        if (l) {
            lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_CLICKED, NULL);
            static const int at[3] = { 60, 80, 600 };
            for (int i = 0; i < 3; i++) {
                frame(at[i]);
                snprintf(p, sizeof p, "%s/slide-%d-%s-%s.ppm", dir, i, looks[v].name, tag); shot_now(p);
            }
        }
        au_reset(); au_empty(false);
    }
    au_library_done();
}

/* SLOPPY_B4_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 4's screens:
 * Albums, an album, Songs and Artists as they open; the album sheet, the
 * play menu and picking where this build has them; Sound and Buttons in
 * Hebrew and Arabic; and the Album Artist evidence (b4_library): Artists,
 * one artist's albums and a compilation kept whole. A screen this build
 * lacks is skipped. */
static struct sl_track b4_tr[24];
static struct sl_album b4_al[5];
static int b4_atr[5][6], b4_by_title[5], b4_by_artist[5], b4_songs[24];
static struct sl_lib b4_keep;
/* a compilation of five artists, two of whom have albums of their own,
 * and one song that is on an album and on the compilation both */
static void b4_library(void) {
    static const char *const at[5] = { "Summer Hits 2026", "Blue Hours", "Glass Avenue", "Neon Harbor", "Kite" };
    static const char *const aa[5] = { "Various Artists", "Mira Sol", "Mira Sol", "Neon Harbor", "Kite" };
    static const int ay[5] = { 2026, 2026, 2019, 2011, 1975 }, an[5] = { 6, 5, 4, 5, 4 };
    static const char *const ag[5] = { "Pop", "Pop", "Pop", "Electronic", "Rock" };
    static const char *const ct[6] = { "Night Drive", "Harbor Lights", "Paper Kite", "Undertow", "After the Rain", "Weekend" };
    static const char *const cart[6] = { "Mira Sol", "Neon Harbor", "Kite", "Lumen", "Mira Sol", "The Long Weekend" };
    static const char *const ot[4][5] = {
        { "Night Drive", "Blue Hours", "After the Rain", "Small Hours", "Runaway" },
        { "Glass Avenue", "Silent City", "Neon Sky", "High Voltage", NULL },
        { "Harbor Lights", "Tidal", "Night Drive (feat. Mira Sol)", "Lanterns", "Drift" },
        { "Paper Kite", "Updraft", "String", "Weather", NULL } };
    static char paths[24][32];
    b4_keep = sl_lib;
    int n = 0;
    for (int a = 0; a < 5; a++) {
        b4_al[a] = (struct sl_album){ at[a], aa[a], ay[a], an[a], (a * 67 + 20) % 360, b4_atr[a], ag[a] };
        for (int k = 0; k < an[a]; k++, n++) {
            b4_atr[a][k] = n; snprintf(paths[n], sizeof paths[n], "/Music/b%d/t%d.flac", a, k);
            b4_tr[n] = (struct sl_track){ paths[n], a ? ot[a - 1][k] : ct[k], a, k + 1, 1, 185000 + k * 23000, (uint32_t)(1000 + n), true,
                                          a ? NULL : cart[k] }; tsplit(&b4_tr[n]);
        }
    }
    sl_lib.tracks = b4_tr; sl_lib.n_tracks = n; sl_lib.albums = b4_al; sl_lib.n_albums = 5; sl_lib.n_artists = 4;
    for (int i = 0; i < 5; i++) b4_by_title[i] = b4_by_artist[i] = i;
    for (int i = 1; i < 5; i++) for (int j = i; j > 0 && sl_sort_cmp(b4_al[b4_by_title[j - 1]].title, b4_al[b4_by_title[j]].title) > 0; j--) { int x = b4_by_title[j]; b4_by_title[j] = b4_by_title[j - 1]; b4_by_title[j - 1] = x; }
    for (int i = 1; i < 5; i++) for (int j = i; j > 0 && sl_sort_cmp(b4_al[b4_by_artist[j - 1]].artist, b4_al[b4_by_artist[j]].artist) > 0; j--) { int x = b4_by_artist[j]; b4_by_artist[j] = b4_by_artist[j - 1]; b4_by_artist[j - 1] = x; }
    for (int i = 0; i < n; i++) b4_songs[i] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && sl_sort_cmp(b4_tr[b4_songs[j - 1]].title, b4_tr[b4_songs[j]].title) > 0; j--) { int x = b4_songs[j]; b4_songs[j] = b4_songs[j - 1]; b4_songs[j - 1] = x; }
    sl_lib.albums_by_title = b4_by_title; sl_lib.albums_by_artist = b4_by_artist; sl_lib.songs_by_title = b4_songs;
    sl_lib.album_hash = NULL; sl_lib.album_hash_cap = 0; sl_lib.path_hash = NULL; sl_lib.path_hash_cap = 0;
}
static void b4_library_done(void) { sl_lib = b4_keep; }
static void b4_shots(const char *dir, const char *tag) {
    static const struct { const char *screen, *variant; int look, step, lang; } shots[] = {
        { "albums", "dark", 0, 0, 0 }, { "albums", "light", 1, 0, 0 }, { "albums", "zule", 6, 0, 0 },
        { "albums", "largest-dark", 0, 3, 0 }, { "albums", "he-dark", 0, 0, 4 }, { "albums", "ar-light", 1, 0, 5 },
        { "album", "dark", 0, 0, 0 }, { "album", "light", 1, 0, 0 }, { "album", "zule", 6, 0, 0 },
        { "album", "largest-dark", 0, 3, 0 }, { "album", "he-dark", 0, 0, 4 }, { "album", "ar-light", 1, 0, 5 },
        { "songs", "dark", 0, 0, 0 }, { "songs", "light", 1, 0, 0 }, { "songs", "zule", 6, 0, 0 }, { "songs", "he-dark", 0, 0, 4 },
        { "artists", "dark", 0, 0, 0 }, { "artists", "light", 1, 0, 0 }, { "artists", "zule", 6, 0, 0 }, { "artists", "ar-light", 1, 0, 5 },
        { "album-sheet", "dark", 0, 0, 0 }, { "album-sheet", "light", 1, 0, 0 }, { "album-sheet", "zule", 6, 0, 0 },
        { "album-sheet", "largest-dark", 0, 3, 0 }, { "album-sheet", "he-dark", 0, 0, 4 }, { "album-sheet", "ar-light", 1, 0, 5 },
        { "artist-sheet", "dark", 0, 0, 0 }, { "artist-sheet", "light", 1, 0, 0 },
        { "play-menu", "dark", 0, 0, 0 }, { "play-menu", "light", 1, 0, 0 }, { "play-menu", "zule", 6, 0, 0 },
        { "play-menu", "largest-dark", 0, 3, 0 }, { "play-menu", "he-dark", 0, 0, 4 }, { "play-menu", "ar-light", 1, 0, 5 },
        { "albums-sort-sheet", "dark", 0, 0, 0 }, { "albums-sort-sheet", "light", 1, 0, 0 },
        { "albums-picking", "dark", 0, 0, 0 }, { "albums-picking", "light", 1, 0, 0 }, { "albums-picking", "zule", 6, 0, 0 },
        { "albums-picking", "largest-dark", 0, 3, 0 }, { "albums-picking", "he-dark", 0, 0, 4 }, { "albums-picking", "ar-light", 1, 0, 5 },
        { "artists-picking", "dark", 0, 0, 0 }, { "artists-picking", "light", 1, 0, 0 }, { "artists-picking", "zule", 6, 0, 0 },
        { "artists-picking", "he-dark", 0, 0, 4 },
        { "albums-badges", "dark", 0, 0, 0 }, { "albums-badges", "light", 1, 0, 0 }, { "albums-badges", "zule", 6, 0, 0 },
        { "albums-no-artist", "dark", 0, 0, 0 }, { "albums-no-artist", "light", 1, 0, 0 },
        { "songs-current", "dark", 0, 0, 0 }, { "songs-current", "light", 1, 0, 0 },
        { "albums-current", "dark", 0, 0, 0 }, { "artists-current", "light", 1, 0, 0 },
        { "albums-recent", "dark", 0, 0, 0 }, { "albums-recent", "largest-dark", 0, 3, 1 },
        { "artist-badges", "dark", 0, 0, 0 },
        { "library", "dark", 0, 0, 0 }, { "library", "light", 1, 0, 0 },
        { "settings-library-options", "dark", 0, 0, 0 }, { "settings-library-options", "light", 1, 0, 0 },
        { "settings-library-options", "zule", 6, 0, 0 }, { "settings-library-options", "largest-dark", 0, 3, 0 },
        { "settings-library-options", "he-dark", 0, 0, 4 }, { "settings-library-options", "ar-light", 1, 0, 5 },
        { "settings-sound", "he-dark", 0, 0, 4 }, { "settings-sound", "ar-light", 1, 0, 5 },
        { "settings-sound-bottom", "he-dark", 0, 0, 4 }, { "settings-sound-bottom", "ar-light", 1, 0, 5 },
        { "settings-buttons", "he-dark", 0, 0, 4 }, { "settings-buttons", "ar-light", 1, 0, 5 },
        { "settings-buttons-bottom", "he-dark", 0, 0, 4 }, { "settings-buttons-bottom", "ar-light", 1, 0, 5 },
        { "evidence-artists", "dark", 0, 0, 0 }, { "evidence-artists", "light", 1, 0, 0 },
        { "evidence-artist", "dark", 0, 0, 0 }, { "evidence-artist", "light", 1, 0, 0 },
        { "evidence-compilation", "dark", 0, 0, 0 }, { "evidence-compilation", "light", 1, 0, 0 },
        { "evidence-compilation-bottom", "dark", 0, 0, 0 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    b4_shooting = true;
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        const char *s = shots[i].screen;
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        bool ev = !strncmp(s, "evidence-", 9), bottom = strstr(s, "-bottom") != NULL;
        char base[64]; strlcpy(base, s, sizeof base);
        if (bottom) *strstr(base, "-bottom") = '\0';
        int k = au_find(base);
        if (k < 0 && !ev) continue;
        au_apply(shots[i].look, shots[i].step, shots[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        if (ev) {
            b4_library(); test_status = 0; mini_update();
            if (!strcmp(base, "evidence-artists")) artists_page();
            else if (!strcmp(base, "evidence-artist")) { for (int j = 0; j < sl_lib.n_albums; j++) if (!strcmp(sl_lib.albums[sl_lib.albums_by_artist[j]].artist, "Mira Sol")) { artist_page(j); break; } }
            else album_page(0);
            au_settle();
        }
        else if (strstr(s, "-current")) au_open(k);   /* the flash at its height */
        else { au_open(k); au_settle(); }
        if (bottom) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, shots[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
        if (ev) { b4_library_done(); test_status = AUDIO_STATUS_PLAY; }
    }
    au_library_done();
}

/* Batch 4 in the suite: the album and artist sheets, the play menu, picking
 * albums and artists, Recently added, the quality badges and the Album
 * Artist library (a compilation kept whole, artists listing albums). */
static lv_obj_t *b4_row(lv_obj_t *in, const char *label) { lv_obj_t *l = find_label(in, sl_tr(label)); return l ? lv_obj_get_parent(l) : NULL; }
static void b4_tap(lv_obj_t *o) { assert(o); lv_obj_send_event(o, LV_EVENT_CLICKED, NULL); frame(400); }
static void b4_tests(void) {
    au_library();
    test_status = AUDIO_STATUS_PLAY; mini_update();
    while (npages > 1) { page_pop(); frame(300); }
    int n0 = npages;
    /* 11: hold a cover: its sheet; Play plays the album; Add to queue adds every song */
    albums_page(); frame(500);
    struct album_grid_view *g = album_view;
    int a0 = sl_lib.albums_by_title[0], a1 = sl_lib.albums_by_title[1];
    assert(g && g->cells[0] && g->indexes[0] == 0);
    lv_obj_send_event(g->cells[0], LV_EVENT_LONG_PRESSED, NULL); frame(400);
    assert(acts_open_now && find_label(acts, sl_lib.albums[a0].title) && b4_row(acts, "Add to playlist") && b4_row(acts, "Select"));
    b4_tap(b4_row(acts, "Play"));
    if (np_open) { np_hide(0); frame(400); }
    assert(!acts_open_now && queued == sl_lib.albums[a0].n_tracks && started == 0);
    lv_obj_send_event(g->cells[0], LV_EVENT_LONG_PRESSED, NULL); frame(400);
    b4_tap(b4_row(acts, "Add to queue"));
    assert(queued == 2 * sl_lib.albums[a0].n_tracks);
    /* Add to playlist: the playlist sheet for all its songs */
    lv_obj_send_event(g->cells[0], LV_EVENT_LONG_PRESSED, NULL); frame(400);
    b4_tap(b4_row(acts, "Add to playlist"));
    assert(pl_visible() && pl_nmany == sl_lib.albums[a0].n_tracks); pl_close(); frame(300);
    /* Select: picking albums, this one picked; a tap picks another and opens nothing */
    lv_obj_send_event(g->cells[0], LV_EVENT_LONG_PRESSED, NULL); frame(400);
    b4_tap(b4_row(acts, "Select"));
    assert(sel_kind_on(SEL_ALBUMS) && sel_count == 1 && sel_has(a0) && find_label(sel_bar, "1 album"));
    b4_tap(g->cells[1]);
    assert(sel_count == 2 && sel_has(a1) && npages == n0 + 1 && find_label(sel_bar, "2 albums"));
    {   int *l, n = sel_list(&l);   /* both albums' songs, in the grid's order */
        assert(n == sl_lib.albums[a0].n_tracks + sl_lib.albums[a1].n_tracks && l[0] == sl_lib.albums[a0].tracks[0]
               && l[n - 1] == sl_lib.albums[a1].tracks[sl_lib.albums[a1].n_tracks - 1]); free(l); }
    sel_all_cb(NULL); frame(300); assert(sel_count == sl_lib.n_albums && has_text(sel_bar, "Deselect all"));
    sel_play_cb(NULL); frame(400);
    if (np_open) { np_hide(0); frame(400); }
    assert(!sel_on() && queued >= sl_lib.n_tracks && queue_tracks[0] == sl_lib.albums[a0].tracks[0]);   /* the songs queued before carry over */
    /* the album page: + puts the whole album in a playlist */
    album_page(a1); frame(500);
    {   lv_obj_t *hdr = lv_obj_get_child(pages[npages - 1], 0), *plus = NULL;
        for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++) if (find_label(lv_obj_get_child(hdr, i), LV_SYMBOL_PLUS)) plus = lv_obj_get_child(hdr, i);
        b4_tap(plus); assert(pl_visible() && pl_nmany == sl_lib.albums[a1].n_tracks); pl_close(); frame(300); }
    page_pop(); frame(400);
    page_pop(); frame(400);   /* and Albums */
    /* 12: the play menu: in order plays every album's songs; a random album
     * goes onto the queue; Go to current song scrolls to it */
    au_play_menu(albums_page);
    assert(acts_open_now && b4_row(acts, "Play in order") && b4_row(acts, "Add a random album") && b4_row(acts, "Go to current song"));
    b4_tap(b4_row(acts, "Play in order"));
    if (np_open) { np_hide(0); frame(400); }
    assert(queued >= sl_lib.n_tracks && queue_tracks[0] == sl_lib.albums[a0].tracks[0]);
    int q0 = queued; page_pop(); frame(400);
    au_play_menu(albums_page);
    b4_tap(b4_row(acts, "Add a random album"));
    assert(queued == q0 + 8);
    page_pop(); frame(400);
    {   int was = np_love_track; np_love_track = sl_lib.songs_by_title[70];
        au_play_menu(songs_page);
        assert(b4_row(acts, "Add a random song"));
        lv_obj_t *body = top_body(); assert(lv_obj_get_scroll_y(body) == 0);
        b4_tap(b4_row(acts, "Go to current song"));
        assert(lv_obj_get_scroll_y(body) > 70 * 82 - 800);
        struct song_list_view *v = pm_songs(pages[npages - 1]);
        assert(v && v->indexes[70 % 14] == 70 && lv_obj_get_style_bg_opa(v->rows[70 % 14], 0) > 0);   /* lit */
        frame(2000); assert(lv_obj_get_style_bg_opa(v->rows[70 % 14], 0) == LV_OPA_TRANSP);           /* and gone */
        int q = queued; acts_close(); frame(300); page_pop(); frame(400); au_play_menu(songs_page);
        b4_tap(b4_row(acts, "Add a random song")); assert(queued == q + 1);
        page_pop(); frame(400);
        test_status = 0; au_play_menu(songs_page); assert(!b4_row(acts, "Go to current song"));   /* nothing playing */
        acts_close(); frame(300); page_pop(); frame(400); test_status = AUDIO_STATUS_PLAY;
        np_love_track = was; }
    /* picking artists from the play menu's Select; Select all is every artist */
    au_play_menu(artists_page);
    b4_tap(b4_row(acts, "Select"));
    assert(sel_kind_on(SEL_ARTISTS) && sel_count == 0);
    {   lv_obj_t *body = artists_view->host, *r0 = NULL, *r1 = NULL;   /* the rows made so far */
        for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) { lv_obj_t *r = lv_obj_get_child(body, i);
            if ((intptr_t)lv_obj_get_user_data(r) == 1) r0 = r; else if (!r1 && (intptr_t)lv_obj_get_user_data(r) > 1) r1 = r; }
        b4_tap(r0); b4_tap(r1);
        assert(sel_count == 2 && npages == n0 + 1 && find_label(sel_bar, "2 artists"));
        int *l, n = sel_list(&l), want = 0, k1 = (int)(intptr_t)lv_obj_get_user_data(r1) - 1;
        for (int i = 0; i < artist_span(0); i++) want += sl_lib.albums[sl_lib.albums_by_artist[i]].n_tracks;
        for (int i = 0; i < artist_span(k1); i++) want += sl_lib.albums[sl_lib.albums_by_artist[k1 + i]].n_tracks;
        assert(n == want); free(l); }
    sel_all_cb(NULL); frame(300); assert(sel_count == sel_total() && sel_count == 10);
    sel_all_cb(NULL); frame(300); assert(!sel_on());
    /* the artist sheet: hold a row */
    {   lv_obj_t *body = artists_view->host, *r0 = NULL;
        for (uint32_t i = 0; i < lv_obj_get_child_count(body) && !r0; i++) if ((intptr_t)lv_obj_get_user_data(lv_obj_get_child(body, i)) == 1) r0 = lv_obj_get_child(body, i);
        lv_obj_send_event(r0, LV_EVENT_LONG_PRESSED, NULL); frame(400);
        assert(acts_open_now && b4_row(acts, "Shuffle") && b4_row(acts, "Select"));
        b4_tap(b4_row(acts, "Add to queue")); assert(!acts_open_now); }
    page_pop(); frame(400);
    /* 13: Recently added: the album with the newest song first; the chip says so */
    albums_sort = 3; albums_page(); frame(500);
    assert(albums_order()[0] == 9 && albums_order()[9] == 0 && find_label(pages[npages - 1], "Recently added"));
    page_pop(); frame(400); albums_sort = 0;
    /* the badges, from each album's first song */
    au_badges(true);
    assert(album_quality(0) == AQ_HIRES && album_quality(1) == AQ_CD && album_quality(2) == AQ_LOSSY && album_quality(4) == AQ_DSD
           && album_quality(3) == AQ_LOSSLESS);
    albums_page(); frame(500); assert(find_label(pages[npages - 1], "Hi-Res")); page_pop(); frame(400);
    au_badges(false); assert(!find_label(lv_screen_active(), "Hi-Res"));
    /* Show artist off: titles alone, the rows shorter */
    int row = av_row();
    global_settings.sloppy_album_no_artist = 1; albums_page(); frame(500);
    assert(av_row() < row && !find_label(pages[npages - 1], "The Long Weekend"));
    page_pop(); frame(400); global_settings.sloppy_album_no_artist = 0;
    au_library_done();
    /* 14: the Album Artist library: Artists lists album artists with their
     * albums; a compilation stays one album of six, each song its own artist */
    b4_library();
    artists_page(); frame(500);
    assert(find_label(pages[npages - 1], "Mira Sol") && find_label(pages[npages - 1], "2 albums \xc2\xb7 9 songs")
           && find_label(pages[npages - 1], "Various Artists") && find_label(pages[npages - 1], "1 album \xc2\xb7 6 songs")
           && !find_label(pages[npages - 1], "Lumen") && !find_label(pages[npages - 1], "The Long Weekend"));
    page_pop(); frame(400);
    for (int j = 0; j < sl_lib.n_albums; j++) if (!strcmp(sl_lib.albums[sl_lib.albums_by_artist[j]].artist, "Mira Sol")) { artist_page(j); break; }
    frame(500); assert(find_label(pages[npages - 1], "Blue Hours") && find_label(pages[npages - 1], "Glass Avenue")); page_pop(); frame(400);
    album_page(0); frame(500);
    assert(find_label(pages[npages - 1], "Summer Hits 2026") && sl_lib.albums[0].n_tracks == 6
           && !strcmp(sl_track_artist(sl_lib.albums[0].tracks[1]), "Neon Harbor") && !strcmp(sl_track_artist(sl_lib.albums[0].tracks[0]), "Mira Sol")
           && find_label(pages[npages - 1], "Neon Harbor  \xc2\xb7  3:28  \xc2\xb7  FLAC"));   /* each song says whose it is */
    album_page(1); frame(500); assert(find_label(pages[npages - 1], "3:05  \xc2\xb7  FLAC")); page_pop(); frame(400);   /* an artist's own: no name */
    page_pop(); frame(400);
    b4_library_done();
    while (npages > 1) { page_pop(); frame(300); }
    printf("PASS: albums and artists: sheets, play menu, picking, Recently added, badges, the Album Artist library.\n");
}

/* Batch C: artists and genres one by one. Every way of naming several
 * artists splits, band names stay whole, the settings turn each off; the
 * Artists list from the songs (a guest on a compilation is there, Various
 * Artists isn't); an artist's page with their albums and the ones they're
 * on; Play plays only their songs; genres split; decades; favourites. */
static int c_split(const char *s, bool genre, const char *want) {
    struct tg_piece pc[TG_PIECES]; char got[256] = ""; int at = 0;
    int n = tg_split(s, genre, pc, TG_PIECES, NULL);
    for (int i = 0; i < n; i++) at += snprintf(got + at, sizeof got - at, "%s%.*s", i ? "|" : "", pc[i].len, pc[i].p);
    if (strcmp(got, want)) { fprintf(stderr, "split \"%s\": got \"%s\", want \"%s\"\n", s, got, want); assert(false); }
    return n;
}
static void c_tags_tests(void) {
    global_settings.sloppy_artists_whole = global_settings.sloppy_genres_whole = global_settings.sloppy_split_off = 0;
    c_split("Mira Sol", false, "Mira Sol");
    c_split("Mira Sol feat. Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol ft. Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol Featuring Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol (feat. Lumen)", false, "Mira Sol|Lumen");
    c_split("Mira Sol [with Lumen & Kite]", false, "Mira Sol|Lumen|Kite");
    c_split("Mira Sol & Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol, Lumen & Kite", false, "Mira Sol|Lumen|Kite");
    c_split("Mira Sol, Lumen, & Kite", false, "Mira Sol|Lumen|Kite");
    c_split("Mira Sol; Lumen;Kite", false, "Mira Sol|Lumen|Kite");
    c_split("Mira Sol / Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol/Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol x Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol vs. Lumen", false, "Mira Sol|Lumen");
    c_split("Mira Sol & Mira Sol", false, "Mira Sol");                       /* each once */
    c_split("Simon & Garfunkel", false, "Simon & Garfunkel");                 /* a band */
    c_split("Earth, Wind & Fire", false, "Earth, Wind & Fire");
    c_split("Simon & Garfunkel feat. Lumen", false, "Simon & Garfunkel|Lumen");
    c_split("Tom Petty & the Heartbreakers", false, "Tom Petty & the Heartbreakers");
    c_split("Tyler, The Creator", false, "Tyler, The Creator");
    c_split("AC/DC", false, "AC/DC");
    c_split("Malcolm X", false, "Malcolm X");
    c_split("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d & \xe4\xb8\x96\xe7\x95\x8c", false, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d|\xe4\xb8\x96\xe7\x95\x8c");   /* any script */
    c_split("Rock; Pop", true, "Rock|Pop");
    c_split("Rock/Pop", true, "Rock|Pop");
    c_split("Rock, Pop / Jazz", true, "Rock|Pop|Jazz");
    c_split("Hip-Hop/Rap", true, "Hip-Hop/Rap");
    c_split("Drum & Bass", true, "Drum & Bass");
    c_split("R&B", true, "R&B");
    bool amb;
    struct tg_piece pc[TG_PIECES];
    assert(tg_split("Mira Sol feat. Lumen", false, pc, TG_PIECES, &amb) == 2 && !amb);   /* feat. is never a band name */
    assert(tg_split("Mira Sol & Lumen", false, pc, TG_PIECES, &amb) == 2 && amb);
    /* the settings: one way off, or the whole of it */
    global_settings.sloppy_split_off = TG_AMP; c_split("Mira Sol & Lumen feat. Kite", false, "Mira Sol & Lumen|Kite");
    global_settings.sloppy_split_off = 0; global_settings.sloppy_artists_whole = 1;
    c_split("Mira Sol & Lumen feat. Kite", false, "Mira Sol & Lumen feat. Kite");
    global_settings.sloppy_artists_whole = 0; global_settings.sloppy_genres_whole = 1;
    c_split("Rock; Pop", true, "Rock; Pop"); global_settings.sloppy_genres_whole = 0;
    /* Keep together: a name picked stays whole, and off it again */
    remove(TG_KEEP_FILE); tg_keep_n = -1;
    tg_keep_set("Mira Sol & Lumen", true); c_split("Mira Sol & Lumen", false, "Mira Sol & Lumen");
    tg_keep_n = -1; c_split("Mira Sol & Lumen", false, "Mira Sol & Lumen");   /* read back from the card */
    tg_keep_set("Mira Sol & Lumen", false); c_split("Mira Sol & Lumen", false, "Mira Sol|Lumen");
    remove(TG_KEEP_FILE); tg_keep_n = -1;

    b4_library(); queued = 0;   /* a queue of these songs only */
    while (npages > 1) { page_pop(); frame(300); }
    /* a guest on the compilation, credited with Mira Sol */
    const char *was = b4_tr[3].artist; b4_tr[3].artist = "Lumen feat. Mira Sol";
    const char *g0 = b4_al[0].genre; b4_al[0].genre = "Pop; Dance";
    assert(tg_ready() && tgx.listed == 5);
    static const char *const want[5] = { "Kite", "The Long Weekend", "Lumen", "Mira Sol", "Neon Harbor" };   /* The under L */
    for (int i = 0; i < 5; i++) assert(!strcmp(TG_NAME(tgx.order[i]), want[i]));
    int mira = tg_find("mira sol"), lumen = tg_find("Lumen");
    assert(mira >= 0 && lumen >= 0 && tg_find("Various Artists") < 0 || tgx.first[tg_find("Various Artists") + 1] == tgx.first[tg_find("Various Artists")]);
    assert(tg_albums_of(mira) == 3 && tgx.mine[mira] == 2 && tgx.songs[mira] == 12);   /* two of theirs, on the compilation three times */
    assert(tgx.alb[tgx.first[mira]] == 2 && tgx.alb[tgx.first[mira] + 1] == 1 && tgx.alb[tgx.first[mira] + 2] == 0);   /* oldest first, then the one they're on */
    {   int *l, n = tg_artist_tracks(mira, &l);
        assert(n == 12); for (int i = 0; i < n; i++) assert(tg_track_by(l[i], mira));
        assert(l[n - 1] == b4_al[0].tracks[4]); free(l); }
    assert(tgx.songs[lumen] == 1 && tgx.mine[lumen] == 0);
    tartists_page(); frame(500);
    assert(find_label(pages[npages - 1], "5 ARTISTS") && find_label(pages[npages - 1], "Lumen")
           && find_label(pages[npages - 1], "3 albums \xc2\xb7 12 songs") && !find_label(pages[npages - 1], "Various Artists"));
    shot("c-artists.ppm");
    {   lv_obj_t *r = lv_obj_get_parent(find_label(pages[npages - 1], "Mira Sol")); int depth = npages;
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); frame(500);
        assert(npages == depth + 1); assert(find_label(pages[npages - 1], "ALBUMS")); assert(find_label(pages[npages - 1], "APPEARS ON"));
        assert(find_label(pages[npages - 1], "Summer Hits 2026")); assert(find_label(pages[npages - 1], "3 albums \xc2\xb7 12 songs \xc2\xb7 45 min"));
        shot("c-artist.ppm");
        int q = queued; lv_obj_t *play = lv_obj_get_parent(find_label(pages[npages - 1], LV_SYMBOL_PLAY "  Play"));
        lv_obj_send_event(play, LV_EVENT_CLICKED, NULL); frame(400); if (np_open) { np_hide(0); frame(400); }
        assert(queued == q + 12);                            /* only their songs */
        page_pop(); frame(400); }
    {   lv_obj_t *r = lv_obj_get_parent(find_label(pages[npages - 1], "Lumen"));   /* hold: its sheet */
        lv_obj_send_event(r, LV_EVENT_LONG_PRESSED, NULL); frame(400);
        assert(acts_open_now && b4_row(acts, "Add to queue")); int q = queued;
        b4_tap(b4_row(acts, "Add to queue")); assert(queued == q + 1); }
    page_pop(); frame(400);
    /* a song's Go to artist: the first artist it names, on their own page */
    {   int depth = npages; acts_open(3); frame(300); b4_tap(b4_row(acts, "Go to artist"));
        assert(npages == depth + 1 && find_label(lv_obj_get_child(pages[npages - 1], 0), "Lumen") && find_label(pages[npages - 1], "APPEARS ON"));
        page_pop(); frame(400); }
    /* split off: the guest's tag is one name */
    global_settings.sloppy_artists_whole = 1;
    assert(tg_ready() && tg_find("Lumen feat. Mira Sol") >= 0 && tg_find("Lumen") < 0 && tgx.songs[tg_find("mira sol")] == 11);
    global_settings.sloppy_artists_whole = 0;
    /* genres one by one: Pop; Dance under each */
    genres_page(); frame(500);
    assert(find_label(pages[npages - 1], "4 GENRES") && find_label(pages[npages - 1], "Dance") && find_label(pages[npages - 1], "3 albums"));
    shot("c-genres.ppm");
    {   int k = 0; while (k < tgx.ng && strcmp(TG_GNAME(k), "Dance")) k++;
        assert(k < tgx.ng); group_page(0x30000 | k); frame(500);
        assert(find_label(pages[npages - 1], "Summer Hits 2026") && find_label(pages[npages - 1], "1 ALBUM")); page_pop(); frame(400); }
    page_pop(); frame(400);
    /* decades: 2020s, 2010s, 1970s; a decade's albums */
    global_settings.sloppy_years_decades = 1; years_page(); frame(500);
    assert(find_label(pages[npages - 1], "3 DECADES") && find_label(pages[npages - 1], "2020s") && find_label(pages[npages - 1], "1970s")
           && find_label(pages[npages - 1], "By decade"));
    shot("c-decades.ppm");
    lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "2010s")), LV_EVENT_CLICKED, NULL); frame(500);
    assert(find_label(pages[npages - 1], "Glass Avenue") && find_label(pages[npages - 1], "Neon Harbor") && find_label(pages[npages - 1], "2 ALBUMS"));
    page_pop(); frame(400); page_pop(); frame(400);
    years_page_at(false); frame(300); years_view_pick(0); frame(400);
    assert(!global_settings.sloppy_years_decades && find_label(pages[npages - 1], "5 YEARS") == NULL && find_label(pages[npages - 1], "4 YEARS"));
    page_pop(); frame(400);
    /* favourites: the heart on an album's page, kept on the card by name */
    remove(FAV_FILE); fav_n = -1; free(fav_buf); fav_buf = NULL;
    group_page(0x60000); frame(500); assert(find_label(pages[npages - 1], "No favourite albums yet")); shot("c-favourites-empty.ppm"); page_pop(); frame(400);
    album_page(2); frame(500);
    {   lv_obj_t *hdr = lv_obj_get_child(pages[npages - 1], 0), *heart = NULL;
        for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++) { lv_obj_t *c = lv_obj_get_child(hdr, i);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_USER_4) && lv_obj_get_child_count(c) == 2) heart = c; }
        assert(heart && !fav_has(2)); lv_obj_send_event(heart, LV_EVENT_CLICKED, NULL); frame(300); assert(fav_has(2));
        shot("c-album-favourite.ppm"); }
    page_pop(); frame(400);
    album_acts_open(4, false); frame(400); b4_tap(b4_row(acts, "Add to favourites")); assert(fav_has(4));
    fav_n = -1; free(fav_buf); fav_buf = NULL; assert(fav_has(2) && fav_has(4) && !fav_has(0));   /* from the card */
    group_page(0x60000); frame(500);
    assert(find_label(pages[npages - 1], "2 ALBUMS") && find_label(pages[npages - 1], "Kite") && find_label(pages[npages - 1], "Glass Avenue"));
    {   int *l, n = group_albums(0x60000, &l); assert(n == 2 && l[0] == 4 && l[1] == 2); free(l); }   /* newest first */
    shot("c-favourites.ppm");
    page_pop(); frame(400);
    album_acts_open(4, false); frame(400); b4_tap(b4_row(acts, "Remove from favourites")); assert(!fav_has(4));
    remove(FAV_FILE); fav_n = -1; free(fav_buf); fav_buf = NULL;
    /* Library options: the new cards; Split on; Keep together offers a name split on & */
    const char *was5 = b4_tr[5].artist; b4_tr[5].artist = "The Long Weekend & Kite";
    library_options_page(); frame(500);
    assert(find_label(pages[npages - 1], "Split artists") && find_label(pages[npages - 1], "Join compilations")
           && find_label(pages[npages - 1], "Ignore The, A and An"));
    lv_obj_scroll_to_y(top_body(), 380, LV_ANIM_OFF); frame(200); shot("c-library-options.ppm");
    lo_seps_page(); frame(500); assert(find_label(pages[npages - 1], "Artist vs. Artist")); shot("c-split-on.ppm"); page_pop(); frame(400);
    lo_keep_page(); frame(500); assert(find_label(pages[npages - 1], "The Long Weekend & Kite")); shot("c-keep-together.ppm");
    {   lv_obj_t *r = lv_obj_get_parent(find_label(pages[npages - 1], "The Long Weekend & Kite"));
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL); frame(300);   /* kept whole: one artist now */
        assert(tg_kept("The Long Weekend & Kite") && tg_ready() && tg_find("The Long Weekend & Kite") >= 0); }
    page_pop(); frame(400);
    /* Join compilations off reads the library again as the page closes */
    {   int dbu = db_update_state; db_update_state = DBU_IDLE;   /* (an update going would read it the new way itself) */
        int begins = test_lib_begins; lv_obj_t *sw = NULL, *row = lv_obj_get_parent(find_label(pages[npages - 1], "Join compilations"));
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) if (lv_obj_check_type(lv_obj_get_child(row, i), &lv_switch_class)) sw = lv_obj_get_child(row, i);
        assert(sw && lv_obj_has_state(sw, LV_STATE_CHECKED)); lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); frame(200);
        assert(global_settings.sloppy_albums_apart == 1 && test_lib_begins == begins);
        page_pop(); frame(600);
        assert(test_lib_begins == begins + 1 && sl_lib_keep_apart);
        global_settings.sloppy_albums_apart = 0; lib_options_apply(); db_update_state = dbu; }
    remove(TG_KEEP_FILE); tg_keep_n = -1; tg_keep_gen++;
    b4_tr[5].artist = was5;
    b4_tr[3].artist = was; b4_al[0].genre = g0;
    b4_library_done();
    while (npages > 1) { page_pop(); frame(300); }
    printf("PASS: artists and genres one by one: 36 tags split or kept whole, the Artists list from the songs, an artist's own albums and the ones they're on, Play plays theirs; genres split; decades; favourite albums.\n");
}

/* Batch C at scale: 10,000 albums, 70,000 songs, one in five with a guest
 * ("feat."), every tenth album two genres. The artists and genres are
 * found in one pass (timed); the Artists list, an artist's page and a
 * genre of 3,334 albums scroll end to end with a bounded number of
 * objects; a scrolled frame is timed as the song list's swipe is. */
static double c_ms(struct timespec a, struct timespec b) { return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }
static double c_scroll_us(lv_obj_t *body, int frames) {
    struct timespec a, b; double total = 0;
    for (int i = 0; i < frames; i++) {
        lv_obj_scroll_by(body, 0, -61, LV_ANIM_OFF);
        clock_gettime(CLOCK_MONOTONIC, &a); lv_timer_handler(); lv_refr_now(disp); clock_gettime(CLOCK_MONOTONIC, &b);
        total += c_ms(a, b) * 1000;
    }
    return total / frames;
}
enum { C_NA = 10000, C_PER = 7, C_NT = C_NA * C_PER };
static struct sl_lib c_keep;
static struct sl_track *c_tr; static struct sl_album *c_al;
static int *c_atr, *c_by_title, *c_by_artist, *c_songs;
static char (*c_names)[48], (*c_titles)[24], (*c_guest)[64];
static void c_scale_lib(void) {
    enum { NA = C_NA, PER = C_PER, NT = C_NT };
    c_keep = sl_lib;
    c_tr = calloc(NT, sizeof *c_tr); c_al = calloc(NA, sizeof *c_al);
    c_atr = malloc(NT * sizeof *c_atr); c_by_title = malloc(NA * sizeof *c_by_title); c_by_artist = malloc(NA * sizeof *c_by_artist); c_songs = malloc(NT * sizeof *c_songs);
    c_names = malloc((size_t)NA * 48); c_titles = malloc((size_t)NA * 24); c_guest = malloc((size_t)NT / 5 * 64);
    assert(c_tr && c_al && c_atr && c_by_title && c_by_artist && c_songs && c_names && c_titles && c_guest);
    static const char *const genres[] = { "Rock", "Pop", "Jazz" };
    for (int a = 0; a < NA; a++) {
        snprintf(c_names[a], 48, "Artist %04d", a / 2); snprintf(c_titles[a], 24, "Album %05d", a);
        c_al[a] = (struct sl_album){ c_titles[a], c_names[a], 1960 + a % 66, PER, a % 360, c_atr + a * PER, a % 10 ? genres[a % 3] : "Jazz; Electronic" };
        c_by_title[a] = c_by_artist[a] = a;
        for (int k = 0; k < PER; k++) {
            int t = a * PER + k; c_atr[t] = t; c_songs[t] = t;
            const char *own = NULL;
            if (t % 5 == 0) { snprintf(c_guest[t / 5], 64, "%s feat. Guest %03d", c_names[a], t % 997); own = c_guest[t / 5]; }
            c_tr[t] = (struct sl_track){ "/Music/x.flac", c_titles[a], a, k + 1, 1, 200000, (uint32_t)t, true, own }; tsplit(&c_tr[t]);
        }
    }
    sl_lib.tracks = c_tr; sl_lib.n_tracks = NT; sl_lib.albums = c_al; sl_lib.n_albums = NA; sl_lib.n_artists = NA / 2;
    sl_lib.albums_by_title = c_by_title; sl_lib.albums_by_artist = c_by_artist; sl_lib.songs_by_title = c_songs;
    sl_lib.album_hash = NULL; sl_lib.album_hash_cap = 0; sl_lib.path_hash = NULL; sl_lib.path_hash_cap = 0;
    lib_epoch++;
}
static void c_scale_done(void) {
    sl_lib = c_keep; lib_epoch++;
    free(c_tr); free(c_al); free(c_atr); free(c_by_title); free(c_by_artist); free(c_songs); free(c_names); free(c_titles); free(c_guest);
}
static void c_scale_tests(void) {
    enum { NA = C_NA, NT = C_NT };
    c_scale_lib();
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); assert(tg_ready()); clock_gettime(CLOCK_MONOTONIC, &b);
    double build = c_ms(a, b);
    if (tgx.listed != NA / 2 + 997 || tgx.ng != 4) fprintf(stderr, "scale: %d artists %d genres\n", tgx.listed, tgx.ng);
    assert(tgx.listed == NA / 2 + 997 && tgx.ng == 4);
    {   int g = 0; while (strcmp(TG_GNAME(g), "Jazz")) g++;
        assert(tgx.gfirst[g + 1] - tgx.gfirst[g] == 3334 + 1000 - 334); }   /* a third of the rest, and every tenth */
    while (npages > 1) { page_pop(); frame(300); }
    tartists_page(); frame(500);
    lv_obj_t *body = top_body(); double us = c_scroll_us(body, 120);
    assert(objects(pages[npages - 1]) < 350);
    lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF); frame(300);
    assert(objects(pages[npages - 1]) < 350 && find_label(pages[npages - 1], "Guest 996"));
    page_pop(); frame(400);
    int jazz = 0; while (strcmp(TG_GNAME(jazz), "Jazz")) jazz++;
    group_page(0x30000 | jazz); frame(500);
    body = top_body(); double gus = c_scroll_us(body, 120);
    assert(objects(pages[npages - 1]) < 300);
    lv_obj_scroll_to_y(body, LV_COORD_MAX, LV_ANIM_OFF); frame(300);
    assert(objects(pages[npages - 1]) < 300 && find_label(pages[npages - 1], "Album 09998"));   /* the last of them */
    page_pop(); frame(400);
    artists_page(); frame(500); body = top_body(); double aus = c_scroll_us(body, 120);
    assert(objects(pages[npages - 1]) < 350); page_pop(); frame(400);
    /* the same measure on the lists that were already made as they scroll, for comparison */
    albums_page(); frame(500); double bus = c_scroll_us(top_body(), 120); page_pop(); frame(400);
    songs_page(); frame(500); double sus = c_scroll_us(top_body(), 120); page_pop(); frame(400);
    printf("batch C scale: %d albums, %d songs: artists and genres found in %.1f ms (host); scrolled frame %.0f us Artists, %.0f us a genre of %d albums, %.0f us Album artists (Albums %.0f us, Songs %.0f us)\n",
           NA, NT, build, us, gus, tgx.gfirst[jazz + 1] - tgx.gfirst[jazz], aus, bus, sus);
    c_scale_done();
    printf("PASS: 10,000 albums: Artists, Album artists and a genre's albums scroll end to end as one list each, bounded.\n");
}

/* SLOPPY_FONT_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: Korean, Japanese
 * and Thai in the library, on Now Playing and in lyrics, dark and light
 * (the audit library's last three albums) */
static void xf_play(int t, const char *lrc, uint32_t elapsed) {
    int a = au_tr[t].album;
    test_track.title = au_tr[t].title; test_track.artist = au_al[a].artist; test_track.album = au_al[a].title;
    strlcpy(test_track.path, TPT(&au_tr[t]), sizeof test_track.path);
    test_track.length = 240000; test_track.elapsed = elapsed; test_status = AUDIO_STATUS_PLAY;
    char p[64]; strlcpy(p, TPT(&au_tr[t]), sizeof p); strcpy(strrchr(p, '.'), ".lrc");
    if (lrc) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd >= 0) { write(fd, lrc, strlen(lrc)); sim_close(fd); } }
    else sim_remove(p);
}
static void xf_shots(const char *dir, const char *tag) {
    static const char *const lrc =
        "[00:05.00]\xeb\xb0\xa4\xed\x95\x98\xeb\x8a\x98\xec\x9d\x98 \xeb\xb3\x84\xec\x9d\x84 \xec\x84\xb8\xeb\x8b\xa4\xea\xb0\x80\n"                 /* 밤하늘의 별을 세다가 */
        "[00:10.00]\xec\x9e\xa0\xeb\x93\xa4\xec\xa7\x80 \xeb\xaa\xbb\xed\x95\x9c \xeb\xb0\xa4\n"                                                      /* 잠들지 못한 밤 */
        "[00:15.00]\xe5\xa4\x9c\xe6\x98\x8e\xe3\x81\x91\xe3\x81\xbe\xe3\x81\xa7\xe3\x80\x81\xe3\x81\x82\xe3\x81\xa8\xe5\xb0\x91\xe3\x81\x97\n"        /* 夜明けまで、あと少し */
        "[00:20.00]\xe3\x82\xac\xe3\x83\xa9\xe3\x82\xb9\xe3\x81\xae\xe8\xa1\x97\xe3\x82\x92\xe6\xad\xa9\xe3\x81\x8f\n"                               /* ガラスの街を歩く */
        "[00:25.00]\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x99\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\xb5\xe0\xb9\x83\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb9\x80\xe0\xb8\x82\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb9\x83\xe0\xb8\x88\n"   /* ที่นี่ไม่มีใครเข้าใจ */
        "[00:30.00]\xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3\xe0\xb8\x95\xe0\xb8\xb2\xe0\xb8\x9f\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb8\x9d\xe0\xb8\xb1\xe0\xb9\x88\xe0\xb8\x87\xe0\xb8\x9b\xe0\xb8\xb5\xe0\xb9\x88\n"   /* น้ำตาฟ้าฝั่งปี่ */
        "[00:35.00]\xe0\xb8\x81\xe0\xb8\x95\xe0\xb8\xb1\xe0\xb8\x8d\xe0\xb8\x8d\xe0\xb8\xb9\xe0\xb8\xa3\xe0\xb8\xb9\xe0\xb9\x89\xe0\xb8\x84\xe0\xb8\xb8\xe0\xb8\x93\n"   /* กตัญญูรู้คุณ */
        "[00:40.00]Night drive, slow and blue\n";
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_scripts = true; au_library(); au_scripts = false;
    for (int look = 0; look < 2; look++) {
        const char *ln = look ? "light" : "dark";
        au_apply(look, 0, 0); global_settings.sloppy_notifications = 0;
        xf_play(88, NULL, 47000); mini_update(); frame(300);
        /* the All list at the Korean, Japanese and Thai songs */
        songs_page(); frame(500);
        struct song_list_view *v = pm_songs(pages[npages - 1]);
        if (v) { int k = 0; while (k < v->count && v->tracks[k] < 80) k++;
                 if (k < v->count) { lv_obj_scroll_to_y(v->body, lv_obj_get_y(v->host) + k * SL_ROW_PITCH - 2 * SL_ROW_PITCH, LV_ANIM_OFF); frame(300); } }
        snprintf(p, sizeof p, "%s/songs-%s-%s.ppm", dir, ln, tag); shot(p);
        page_pop(); frame(400);
        for (int a = 10; a < 13; a++) {
            static const char *const sc[3] = { "ko", "ja", "th" };
            album_page(a); frame(500);
            snprintf(p, sizeof p, "%s/album-%s-%s-%s.ppm", dir, sc[a - 10], ln, tag); shot(p);
            page_pop(); frame(400);
            xf_play(a == 10 ? 82 : a == 11 ? 85 : 88, NULL, 47000);
            np_show(); frame(500); np_update(true); au_settle(); np_update(false); frame(100);
            snprintf(p, sizeof p, "%s/np-%s-%s-%s.ppm", dir, sc[a - 10], ln, tag); shot(p);
            np_hide(0); frame(500);
        }
        /* lyrics in all three, a Thai song playing, the Thai line sung */
        xf_play(89, lrc, 26000);
        np_show(); frame(500); np_update(true); frame(100);
        if (lyr_btn && !lv_obj_has_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN)) lv_obj_send_event(lyr_btn, LV_EVENT_CLICKED, NULL);
        au_settle();
        snprintf(p, sizeof p, "%s/lyrics-%s-%s.ppm", dir, ln, tag); shot(p);
        np_hide(0); frame(500);
        xf_play(89, NULL, 0);
    }
    au_library_done();
}

/* ---- script packs ---- */
/* the code a drawn glyph was read under: a letter, or a Thai mark's key for where it stands */
static uint16_t xf_code(const lv_font_glyph_dsc_t *g) {
    const struct xf_pack *p = &xf_packs[g->gid.index >> 16]; uint16_t v;
    assert(p->mem); memcpy(&v, p->cp + (g->gid.index & 0xffff) * 2, 2); return v;
}
/* letters as a label draws them: each one's width with the next, then drawn with none; both the same */
static void xf_run(const lv_font_t *f, const uint32_t *s, int n, lv_font_glyph_dsc_t *out) {
    for (int i = 0; i < n; i++) {
        lv_font_glyph_dsc_t w;
        bool ok = lv_font_get_glyph_dsc(f, &w, s[i], i + 1 < n ? s[i + 1] : 0); assert(ok && !w.is_placeholder);
        ok = lv_font_get_glyph_dsc(f, &out[i], s[i], 0); assert(ok && !out[i].is_placeholder);
        assert(w.gid.index == out[i].gid.index && w.adv_w == out[i].adv_w && w.ofs_x == out[i].ofs_x);
    }
}
#define XF_THKEY(ctx, c) (0xe000u | (ctx) << 7 | ((c) - 0xe00))
static void xf_script_tests(void) {
    for (int i = 0; i < XF_PACKS; i++) { free(xf_packs[i].mem); xf_packs[i].mem = NULL; xf_packs[i].tried = false; }
    {   /* a damaged pack (a letter's bitmap past the end) is refused, not drawn from beyond it */
        struct xf_pack *e = &xf_packs[XF_PACKS - 1];
        char path[MAX_PATH]; snprintf(path, sizeof path, ROCKBOX_DIR "/sloppy/fonts/%s.slf", e->name);
        int fd = open(path, O_RDONLY); assert(fd >= 0);
        static uint8_t pk[1 << 21]; ssize_t n = read(fd, pk, sizeof pk); close(fd); assert(n > 20 && n < (ssize_t)sizeof pk);
        uint32_t cnt; memcpy(&cnt, pk + 12, 4);
        uint32_t off0; memcpy(&off0, pk + 20 + (size_t)cnt * 2, 4);
        uint32_t bad = 0x7ffffff0; memcpy(pk + 20 + (size_t)cnt * 2, &bad, 4);
        fake_put(path, pk, (size_t)n);
        assert(!xf_load(e) && !e->mem);
        memcpy(pk + 20 + (size_t)cnt * 2, &off0, 4); fake_put(path, pk, (size_t)n);
        e->tried = false; assert(xf_load(e) && e->mem);
        free(e->mem); e->mem = NULL; e->tried = false;
        printf("PASS: a damaged letter pack is refused whole; the pack itself loads\n");
    }
    xf_bytes = 0;
    const lv_font_t *const px[XF_SIZES] = { &sl_xf_16, &sl_xf_18, &sl_xf_21, &sl_xf_23, &sl_xf_26, &sl_xf_28, &sl_xf_30 };
    lv_font_glyph_dsc_t g, r[8];
    /* Korean: a common syllable reads the small pack only, a rare one the other */
    assert(lv_font_get_glyph_dsc(&ss_semibold_23, &g, 0xd55c, 0) && !g.is_placeholder && g.resolved_font == &sl_xf_23);   /* 한 */
    assert(g.gid.index >> 16 == XF_KOR + 1 && xf_packs[XF_KOR + 1].mem && !xf_packs[XF_KORX + 1].mem);
    unsigned long k0 = xf_bytes;
    lv_font_get_glyph_dsc(&sl_xf_16, &g, 0xd55c, 0); lv_font_get_glyph_dsc(&sl_xf_26, &g, 0xd55c, 0);
    unsigned long kor = xf_bytes;
    assert(lv_font_get_glyph_dsc(&sl_xf_23, &g, 0xb620, 0) && g.gid.index >> 16 == XF_KORX + 1);                          /* 똠, outside KS X 1001 */
    unsigned long korx1 = xf_bytes - kor;
    lv_font_get_glyph_dsc(&sl_xf_16, &g, 0xb620, 0); lv_font_get_glyph_dsc(&sl_xf_26, &g, 0xb620, 0);
    unsigned long korx = xf_bytes - kor;
    assert(xf_packs[XF_KOR].n == 2350 + 94);
    unsigned long t0 = xf_bytes;
    for (int si = 0; si < XF_SIZES; si++) lv_font_get_glyph_dsc(px[si], &g, 0xe01, 0);
    unsigned long th = xf_bytes - t0;
    /* every modern syllable, jamo, kana and Thai letter, at every size */
    for (int si = 0; si < XF_SIZES; si++) {
        for (uint32_t c = 0xac00; c <= 0xd7a3; c++) { bool ok = lv_font_get_glyph_dsc(px[si], &g, c, 0); assert(ok && !g.is_placeholder && g.box_w); }
        for (uint32_t c = 0x3131; c <= 0x318e; c++) { bool ok = lv_font_get_glyph_dsc(px[si], &g, c, 0); assert(ok && !g.is_placeholder); }
        for (uint32_t c = 0x3041; c <= 0x30ff; c++) if (c < 0x3097 || c > 0x3098) { bool ok = lv_font_get_glyph_dsc(px[si], &g, c, 0); assert(ok && !g.is_placeholder); }
        for (uint32_t c = 0xff61; c <= 0xff9f; c++) { bool ok = lv_font_get_glyph_dsc(px[si], &g, c, 0); assert(ok && !g.is_placeholder); }
        for (uint32_t c = 0xe01; c <= 0xe5b; c++) if (c <= 0xe3a || c >= 0xe3f) { bool ok = lv_font_get_glyph_dsc(px[si], &g, c, 0); assert(ok && !g.is_placeholder); }
    }
    assert(lv_font_get_glyph_dsc(&sl_xf_23, &g, 0x3000, 0) && !g.is_placeholder && g.adv_w && !g.box_w);   /* the ideographic space */
    /* Hangul a Mac wrote apart: 한글, and 가 */
    {   static const uint32_t s[] = { 0x1112, 0x1161, 0x11ab, 0x1100, 0x1173, 0x11af }; xf_run(&sl_xf_23, s, 6, r);
        lv_font_glyph_dsc_t h, gl; lv_font_get_glyph_dsc(&sl_xf_23, &h, 0xd55c, 0); lv_font_get_glyph_dsc(&sl_xf_23, &gl, 0xae00, 0);
        assert(!r[0].adv_w && !r[0].box_w && !r[1].adv_w && !r[1].box_w && r[2].gid.index == h.gid.index && r[2].adv_w == h.adv_w);
        assert(!r[3].adv_w && !r[4].adv_w && r[5].gid.index == gl.gid.index); }
    {   static const uint32_t s[] = { 0x1100, 0x1161 }; xf_run(&sl_xf_23, s, 2, r); assert(!r[0].adv_w && xf_code(&r[1]) == 0xac00); }
    {   static const uint32_t s[] = { 0x1100 }; xf_run(&sl_xf_23, s, 1, r); assert(xf_code(&r[0]) == 0x3131); }
    /* kana and a sound mark apart: ガ, パ; a mark after no kana stands on its own */
    {   static const uint32_t s[] = { 0x30ab, 0x3099, 0x30cf, 0x309a, 0x4e2d, 0x3099 }; xf_run(&sl_xf_23, s, 6, r);
        assert(xf_code(&r[0]) == 0x30ac && !r[1].adv_w && !r[1].box_w && xf_code(&r[2]) == 0x30d1 && !r[3].adv_w && xf_code(&r[5]) == 0x309b && r[5].adv_w); }
    /* Thai: each mark as a shaper places it */
    {   static const uint32_t s[] = { 0xe01, 0xe48 }; xf_run(&sl_xf_23, s, 2, r); assert(xf_code(&r[1]) == 0xe48 && !r[1].adv_w && r[1].ofs_x < 0); }
    {   static const uint32_t s[] = { 0xe1b, 0xe48 }; xf_run(&sl_xf_23, s, 2, r); assert(xf_code(&r[1]) == XF_THKEY(1, 0xe48)); }               /* ป่ */
    {   static const uint32_t s[] = { 0xe17, 0xe35, 0xe48 }; xf_run(&sl_xf_23, s, 3, r); assert(xf_code(&r[1]) == 0xe35 && xf_code(&r[2]) == XF_THKEY(6, 0xe48)); }   /* ที่ */
    {   static const uint32_t s[] = { 0xe1b, 0xe35, 0xe48 }; xf_run(&sl_xf_23, s, 3, r); assert(xf_code(&r[1]) == XF_THKEY(1, 0xe35) && xf_code(&r[2]) == XF_THKEY(7, 0xe48)); }   /* ปี่ */
    {   static const uint32_t s[] = { 0xe1d, 0xe31, 0xe48, 0xe07 }; xf_run(&sl_xf_23, s, 4, r); assert(xf_code(&r[2]) == XF_THKEY(3, 0xe48) && xf_code(&r[3]) == 0xe07); }   /* ฝั่ง */
    {   static const uint32_t s[] = { 0xe19, 0xe49, 0xe33 }; xf_run(&sl_xf_23, s, 3, r); assert(xf_code(&r[1]) == XF_THKEY(12, 0xe49) && xf_code(&r[2]) == 0xe33); }   /* น้ำ */
    {   static const uint32_t s[] = { 0xe0d, 0xe39 }; xf_run(&sl_xf_23, s, 2, r); assert(xf_code(&r[0]) == XF_THKEY(20, 0xe0d) && xf_code(&r[1]) == XF_THKEY(16, 0xe39)); }   /* ญู */
    {   static const uint32_t s[] = { 0xe0d, 0xe32 }; xf_run(&sl_xf_23, s, 2, r); assert(xf_code(&r[0]) == 0xe0d); }                            /* ญา keeps its foot */
    /* the tallest stack, a tone over a vowel over a tall letter, inside the line its words sit on */
    {   const lv_font_t *const prim[XF_SIZES] = { &ss_regular_16, &ss_semibold_18, &ss_semibold_21, &ss_semibold_23, &ss_semibold_26, &ss_bold_28, &ss_semibold_30 };
        static const uint32_t s[] = { 0xe1b, 0xe35, 0xe48 };
        for (int si = 0; si < XF_SIZES; si++) {
            xf_run(px[si], s, 3, r);
            int room = prim[si]->line_height - prim[si]->base_line, top = 0;
            for (int i = 0; i < 3; i++) if (r[i].ofs_y + r[i].box_h > top) top = r[i].ofs_y + r[i].box_h;
            if (top > room) fprintf(stderr, "thai: %d px stack %d over a %d px line\n", xf_px[si], top, room);
            assert(top <= room);
        } }
    {   uint8_t *buf = malloc(64 * 64); lv_draw_buf_t db = { 0 }; db.data = buf;
        static const uint32_t s[] = { 0xe1b, 0xe35, 0xe48 }; xf_run(&sl_xf_30, s, 3, r);
        assert(xf_glyph_bitmap(&r[2], &db) == &db); free(buf); }
    printf("PASS: Korean, Japanese and Thai from the card: every modern Hangul syllable, jamo, kana and Thai letter at all seven sizes; "
           "jamo and kana a Mac writes apart drawn as one letter; Thai marks in the shaper's places, the tallest stack inside the line. "
           "In memory once shown: Korean %lu KB at one size, %lu KB at all three (+%lu KB, %lu KB for a syllable outside KS X 1001), Thai %lu KB at all seven.\n",
           k0 / 1024, kor / 1024, korx1 / 1024, korx / 1024, th / 1024);
}
/* ---- end script packs ---- */

/* R5: names that break naive code, on the sandboxed card and in the
 * library: emoji, accents, Hebrew, Arabic, Chinese, Japanese, Korean,
 * quotes, #, %, &, ;, brackets, a leading space, a trailing dot, and one
 * as long as a name gets. Each lists, plays from the folder (the queue
 * holds the very song), survives a playlist written and read back, and
 * plays from a cue sheet that names it. */
static void c_unicode_tests(void) {
    static const char *const names[] = {
        "Sunrise \xf0\x9f\x8c\x85", "Caf\xc3\xa9 \xc3\xa0 la cr\xc3\xa8me", "Beyonc\xc3\xa9 & \xc3\x91" "and\xc3\xba", "\xc3\x98resund \xc3\x85ngstr\xc3\xb6m",
        "\xd7\xa9\xd7\x99\xd7\xa8 \xd7\xa2\xd7\xa8\xd7\xa9", "\xd8\xa3\xd8\xba\xd9\x86\xd9\x8a\xd8\xa9 \xd8\xa7\xd9\x84\xd9\x84\xd9\x8a\xd9\x84",
        "\xe5\xa4\x9c\xe3\x81\xae\xe6\xad\x8c", "\xe5\x91\xa8\xe6\x9d\xb0\xe4\xbc\xa6", "\xec\x95\x88\xeb\x85\x95",
        "Don't 'Stop' Now", "\xe2\x80\x9c" "Curly" "\xe2\x80\x9d \xe2\x80\x98Quotes\xe2\x80\x99", "Track #2 at 50%", "Rock & Roll; Part 2",
        "Song (feat. Guest) [2024] {Edit}", "+ Plus = Equals ~Tilde~ `Back`", " Leading space", "Trailing dot.",
        "\xe3\x83\x9f\xe3\x83\x8a\xe3\x83\x88\xe3\x83\xbb\xe3\x82\xab\xe3\x82\x99\xe3\x83\xa9\xe3\x82\xb9", "\xe0\xb8\x9b\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x9e\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xa2\xe0\xb9\x8c \xe0\xb8\x99\xe0\xb9\x89\xe0\xb8\xb3\xe0\xb8\x95\xe0\xb8\xb2",
        "Long name xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx end",
    };
    enum { N = sizeof names / sizeof names[0] };
    static char paths[N][MAX_PATH];
    static struct sl_track tr[N]; static struct sl_album al[1]; static int atr[N], by[1] = { 0 }, songs[N];
    struct sl_lib keep = sl_lib;
    fake_card_reset(); mkdir(FAKE_CARD); mkdir(FAKE_CARD "/Unicode test");
    for (int i = 0; i < N; i++) {
        snprintf(paths[i], sizeof paths[i], FAKE_CARD "/Unicode test/%02d %s.mp3", i + 1, names[i]);
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0); write(fd, "ID3", 3); sim_close(fd);
        tr[i] = (struct sl_track){ paths[i], names[i], 0, i + 1, 1, 200000, (uint32_t)i, false, NULL }; tsplit(&tr[i]);
        atr[i] = i; songs[i] = i;
    }
    al[0] = (struct sl_album){ "\xc3\x9cnic\xc3\xb8" "d\xc3\xa9 #1 & 100%", "Unicode Artist", 2026, N, 40, atr, "Pop" };
    sl_lib.tracks = tr; sl_lib.n_tracks = N; sl_lib.albums = al; sl_lib.n_albums = 1; sl_lib.n_artists = 1;
    sl_lib.albums_by_title = sl_lib.albums_by_artist = by; sl_lib.songs_by_title = songs;
    sl_lib.album_hash = NULL; sl_lib.album_hash_cap = 0; sl_lib.path_hash = NULL; sl_lib.path_hash_cap = 0;
    /* the harness finds songs by path through the library's own index */
    sl_lib.path_hash_cap = 64; static int ph[64]; sl_lib.path_hash = ph;
    for (int i = 0; i < 64; i++) ph[i] = -1;
    for (int i = 0; i < N; i++) { unsigned h = 2166136261u; for (const char *p = paths[i]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
        for (int j = 0; ; j++) { int slot = (h + j) & 63; if (ph[slot] < 0) { ph[slot] = i; break; } } }
    for (int i = 0; i < N; i++) assert(sl_lib_find_path(paths[i]) == i);
    lib_epoch++; queued = 0; started = -1;
    while (npages > 1) { page_pop(); frame(300); }
    /* listed: each title on the album page, every letter with a glyph but emoji's */
    album_page(0); frame(500); shot("c-unicode-album.ppm");
    long missing = 0, emoji = 0;   /* no emoji font: counted, not hidden */
    {   lv_obj_t *body = top_body();
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
        for (int pass = 0; pass < 4; pass++) {
            for (int i = 0; i < N; i++) { lv_obj_t *l = find_label(pages[npages - 1], names[i]); (void)l; }
            lv_obj_scroll_by(body, 0, -600, LV_ANIM_OFF); frame(200);
        }
    }
    {   /* every song's title on its row, its letters drawn (the fonts' and the card's packs; an emoji has none) */
        lv_obj_t *l;
        for (int i = 0; i < N; i++) {
            lv_obj_t *page = pages[npages - 1];
            songs_page(); frame(500);
            struct song_list_view *v = pm_songs(pages[npages - 1]); assert(v);
            int k = 0; while (k < v->count && v->tracks[k] != i) k++;   /* its row, in the order the list is in */
            assert(k < v->count);
            lv_obj_scroll_to_y(v->body, lv_obj_get_y(v->host) + k * SL_ROW_PITCH, LV_ANIM_OFF); frame(200);
            {   char shaped[400];   /* Arabic is shaped into the letters' joined forms, as the label shows it */
                if (lv_text_ap_calc_bytes_count(names[i]) < (int)sizeof shaped) lv_text_ap_proc(names[i], shaped); else strlcpy(shaped, names[i], sizeof shaped);
                l = find_label(pages[npages - 1], shaped); if (!l) l = find_label(pages[npages - 1], names[i]); }
            if (!l) { char pre[40]; snprintf(pre, sizeof pre, "%.14s", names[i]);   /* ASCII names only reach here: a long one ends in dots */ if (!has_text(pages[npages - 1], pre)) { fprintf(stderr, "unicode: row %d \"%s\" not shown\n", i, names[i]);
                for (uint32_t c = 0; c < lv_obj_get_child_count(v->host); c++) { lv_obj_t *r = lv_obj_get_child(v->host, c); if ((intptr_t)lv_obj_get_user_data(r) == i) for (uint32_t j = 0; j < lv_obj_get_child_count(r); j++) if (lv_obj_check_type(lv_obj_get_child(r, j), &lv_label_class)) fprintf(stderr, "  label: [%s]\n", lv_label_get_text(lv_obj_get_child(r, j))); } } assert(has_text(pages[npages - 1], pre)); }
            for (const char *t = names[i]; *t; ) {
                uint32_t c = st_utf8_next(&t); lv_font_glyph_dsc_t g;
                bool ok = c < 0x20 || (lv_font_get_glyph_dsc(&ss_semibold_23, &g, c, 0) && !g.is_placeholder);
                if (!ok) { if (c >= 0x1f000) emoji++; else { missing++; printf("unicode: no glyph for U+%04X in \"%s\"\n", (unsigned)c, names[i]); } }
            }
            page_pop(); frame(300); (void)page;
        }
    }
    page_pop(); frame(300);
    assert(!missing);
    /* played from the folder: Files lists every name, and a tap on the
     * first queues the folder, each the very song */
    sl_files_page(FAKE_CARD "/Unicode test", 0); frame(500); shot("c-unicode-files.ppm");
    struct fm_view *fv = NULL; for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && fm_views[i]->page == pages[npages - 1]) fv = fm_views[i]; if (fv && fv->n != N) { fprintf(stderr, "unicode: Files lists %d of %d\n", fv->n, N); for (int k = 0; k < fv->n; k++) fprintf(stderr, "  [%s]\n", fm_name(fv, k)); }
    assert(fv && fv->n == N);
    fm_play_from(fv, fv->folders); frame(300); if (np_open) { np_hide(0); frame(400); }
    assert(queued == N);
    {   bool seen[N] = { false };
        for (int i = 0; i < queued; i++) { assert(queue_tracks[i] >= 0 && queue_tracks[i] < N && !seen[queue_tracks[i]]); seen[queue_tracks[i]] = true; } }
    page_pop(); frame(300);
    /* a playlist of them all, written and read back */
    {   int all[N]; for (int i = 0; i < N; i++) all[i] = i;
        free(pl_many); pl_many = malloc(sizeof all); memcpy(pl_many, all, sizeof all); pl_nmany = N; pl_track = 0; pl_npaths = 0;
        const char *pp = FAKE_CARD "/Unicode test/All of them #1 & 100%.m3u8";
        assert(pl_write_new(pp));
        int *l = NULL, miss = 0, n = pls_read(pp, &l, &miss);
        assert(n == N && !miss); for (int i = 0; i < N; i++) assert(l[i] == i);
        free(l); free(pl_many); pl_many = NULL; pl_nmany = 0; pl_track = -1; }
    /* a cue sheet naming one of them: its tracks, and the second plays that very file from 3:00 */
    {   char cue_txt[1200]; int k = snprintf(cue_txt, sizeof cue_txt,
            "PERFORMER \"%s\"\nTITLE \"Live at the \xc3\x96lympia #2\"\nFILE \"%02d %s.mp3\" MP3\n"
            "  TRACK 01 AUDIO\n    TITLE \"%s\"\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    TITLE \"%s\"\n    INDEX 01 03:00:00\n",
            names[4], 3, names[2], names[9], names[11]);
        int fd = open(FAKE_CARD "/Unicode test/live.cue", O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0); write(fd, cue_txt, k); sim_close(fd);
        fm_cue_page(FAKE_CARD "/Unicode test/live.cue"); frame(500); shot("c-unicode-cue.ppm");
        assert(cue.n == 2 && !cue.missing && !strcmp(cue.files[0], paths[2]) && !strcmp(cue.t[0].title, names[9]) && !strcmp(cue.perf, names[4]));
        assert(find_label(pages[npages - 1], names[11]));
        cue_play(1, false); frame(300); if (np_open) { np_hide(0); frame(400); }
        assert(queued == 1 && queue_tracks[0] == 2);
        page_pop(); frame(300); }
    printf("PASS: %d names with emoji, accents, Hebrew, Arabic, CJK, Korean, Japanese, Thai, quotes, #, %%, &, ; and a %d-byte one: listed (%ld emoji letters without a glyph, nothing else), played from their folder, through a playlist and from a cue sheet.\n",
           N, (int)strlen(names[N - 1]), emoji);
    fake_card_reset(); queued = 0;
    sl_lib = keep; lib_epoch++;
    while (npages > 1) { page_pop(); frame(300); }
}

/* F2: an album in one file with its cue sheet, as the library reads it:
 * three songs sharing the file. The album plays the file once, from the
 * track picked; Now Playing names the track playing and times it alone;
 * next and previous step through the sheet's tracks inside the file. */
static void c_cue_tests(void) {
    static struct sl_track tr[4]; static struct sl_album al[2]; static int atr0[3] = { 0, 1, 2 }, atr1[1] = { 3 }, by[2] = { 0, 1 }, songs[4] = { 0, 1, 2, 3 };
    struct sl_lib keep = sl_lib;
    static const char *img = "/Music/Live/Live in Paris.flac";
    tr[0] = (struct sl_track){ img, "Ouverture", 0, 1, 1, 300000, 1, true, NULL, 0, 0, 1 };
    tr[1] = (struct sl_track){ img, "Deux", 0, 2, 1, 450000, 1, true, "Guest", 300000, 0, 2 };
    tr[2] = (struct sl_track){ img, "Trois", 0, 3, 1, 600000, 1, true, NULL, 750000, 0, 3 };
    tr[3] = (struct sl_track){ "/Music/Other/one.flac", "Another", 1, 1, 1, 200000, 2, true, NULL, 0, 0, 0 };
    for (int i = 0; i < 4; i++) tsplit(&tr[i]);   /* the sheet's three share one folder and one name */
    al[0] = (struct sl_album){ "Live in Paris", "S\xc3\xa9verine", 2024, 3, 30, atr0, "Pop" };
    al[1] = (struct sl_album){ "Other", "Kite", 2020, 1, 90, atr1, "Rock" };
    sl_lib.tracks = tr; sl_lib.n_tracks = 4; sl_lib.albums = al; sl_lib.n_albums = 2; sl_lib.n_artists = 2;
    sl_lib.albums_by_title = sl_lib.albums_by_artist = by; sl_lib.songs_by_title = songs; lib_epoch++;
    while (npages > 1) { page_pop(); frame(300); }
    album_page(0); frame(500); shot("c-cue-album.ppm");
    assert(find_label(pages[npages - 1], "Deux") && find_label(pages[npages - 1], "Trois"));
    page_pop(); frame(300);
    /* the whole album: one entry, from the start */
    assert(play_album(0, 1, false)); if (np_open) { np_hide(0); frame(300); }
    assert(queued == 1 && queue_tracks[0] == 0 && started == 0 && last_start_elapsed == 300000);   /* from Deux, 5:00 into the file */
    /* the album and another: the file once, then the other */
    {   int l[4] = { 0, 1, 2, 3 }; assert(play_list(l, 4, 3, false)); assert(queued == 2 && started == 1 && last_start_elapsed == 0); }
    /* Now Playing: Deux, by Guest, timed on its own */
    test_status = AUDIO_STATUS_PLAY; strlcpy(test_track.path, img, sizeof test_track.path);
    test_track.title = "Live in Paris (the file's tag)"; test_track.artist = "S\xc3\xa9verine"; test_track.length = 1350000; test_track.elapsed = 360000;
    np_show(); frame(500); np_update(true); frame(100); shot("c-cue-np.ppm");
    assert(!strcmp(lv_label_get_text(np_title), "Deux") && has_text(np_artist, "Guest") && np_love_track == 1);
    assert(!strcmp(lv_label_get_text(np_t0), "1:00") && !strcmp(lv_label_get_text(np_t1), "7:30"));
    /* next: Trois, in the same file; previous after 3 s: Deux's start again; then Ouverture */
    int seeks = audio_seeks; sl_next(); assert(audio_seeks == seeks + 1 && test_track.elapsed == 750000);
    frame(100); np_update(false); assert(!strcmp(lv_label_get_text(np_title), "Trois"));
    test_track.elapsed = 760000; sl_prev(); assert(test_track.elapsed == 750000);   /* within 3 s of its start: back to it ... */
    test_track.elapsed = 751000; sl_prev(); assert(test_track.elapsed == 300000);   /* ... and again: the track before */
    mini_update(); assert(!strcmp(lv_label_get_text(mini_title), "Deux"));
    np_hide(0); frame(400);
    test_track.title = NULL; test_track.artist = NULL; test_status = 0; queued = 0;
    sl_lib = keep; lib_epoch++;
    printf("PASS: an album in one file with its cue sheet: its tracks as songs, the file played once from the track picked, Now Playing on the track, next and previous inside the file.\n");
}

/* P12: the second album page, Big cover: the cover across the page, the
 * thumbnail scaled up at once, the cover's own pixels when the cover
 * thread has them, the words under it from its edge */
static void c_big_cover_tests(void) {
    bool ra = test_real_art; test_real_art = true;
    b4_library();
    while (npages > 1) { page_pop(); frame(300); }
    global_settings.sloppy_album_layout = 1;
    album_page(2); frame(100);
    lv_obj_t *head = lv_obj_get_child(top_body(), 0), *c = lv_obj_get_child(head, 0);
    assert(lv_obj_get_width(c) == W - 2 * SL_GUTTER && test_big_album == 2);
    for (int i = 0; i < 12; i++) frame(250);
    lv_obj_t *img = lv_obj_get_child(c, -1);
    assert(lv_obj_check_type(img, &lv_image_class) && lv_obj_get_width(img) == 452 && lv_obj_get_style_opa(img, 0) == LV_OPA_COVER);
    shot("c-album-big.ppm");
    lv_area_t ta; lv_obj_get_coords(lv_obj_get_child(head, 1), &ta); lv_area_t ca; lv_obj_get_coords(c, &ca);
    assert(ta.x1 == ca.x1 && ta.y1 > ca.y2);                /* the title under the cover, from its edge */
    page_pop(); frame(400);
    global_settings.sloppy_album_layout = 0;
    album_page(2); frame(300); assert(lv_obj_get_width(lv_obj_get_child(lv_obj_get_child(top_body(), 0), 0)) == SL_ART_GRID); page_pop(); frame(400);
    b4_library_done(); test_real_art = ra;
    printf("PASS: Big cover: the album's cover across its page, made sharp by the cover thread, the words under it.\n");
}

/* SLOPPY_B5_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 5's screens,
 * on real-looking covers (test_real_art): Now Playing, its lyrics and the
 * album page, each look on a different kind of cover (a sunset, stripes,
 * a dark sleeve, a pale one, a busy one, soft blobs), so the frost and the
 * lyrics are judged on light and dark, busy and plain art. Screens this
 * build lacks are skipped. */
static void b5_play(int a) {
    int t = a * 8 + 1;
    test_track.title = au_tr[t].title; test_track.artist = au_al[a].artist; test_track.album = au_al[a].title;
    strlcpy(test_track.path, TPT(&au_tr[t]), sizeof test_track.path);
    test_track.length = 240000; test_track.elapsed = 47000;
    static const char *const lrc =
        "[00:10.00]Streetlights hum a borrowed tune\n[00:18.00]The harbor holds its breath\n"
        "[00:26.00]We drove until the morning came\n[00:34.00]And every sign said west\n"
        "[00:42.00]Night drive, slow and blue\n[00:50.00]I keep the window down for you\n"
        "[00:58.00]The radio forgets our names\n[01:06.00]But the road remembers\n[01:14.00]Night drive, slow and blue\n";
    char p[64]; snprintf(p, sizeof p, "/Music/a%d/t1.lrc", a);
    if (a != 1) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); if (fd >= 0) { write(fd, lrc, strlen(lrc)); sim_close(fd); } }
}
static void b5_np(void) { np_show(); frame(500); np_update(true); au_settle(); np_update(false); frame(100); }
static void b5_lyrics_btn(void) { if (lyr_btn && !lv_obj_has_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN)) lv_obj_send_event(lyr_btn, LV_EVENT_CLICKED, NULL); frame(400); }
static bool b5_open(const char *s, int a) {
    /* the main loop's np_update, which the harness has not: the glass for a thumbnail comes 700 ms after it */
    if (!strncmp(s, "now-playing", 11)) { b5_np(); return true; }
    if (!strcmp(s, "lyrics-picker") || !strcmp(s, "lyrics-wifi")) {
        bool wifi = !strcmp(s, "lyrics-wifi");
        char p[64]; snprintf(p, sizeof p, "/Music/a%d/t1.lrc", a); sim_remove(p);
        test_track.title = "Night Drive"; test_track.artist = "Mira Sol"; test_track.length = 200000;
        global_settings.sloppy_wifi = 0; wf_st.connected = !wifi; lyr_path[0] = '\0';
        b5_np(); b5_lyrics_btn(); au_settle();
        if (!wifi && !acts_open_now) fprintf(stderr, "b5: no picker (found %d, url %s)\n", lyr_n_found, sl_net_demo_url);
        return true;
    }
    if (!strncmp(s, "lyrics", 6)) { b5_np(); if (!lyr_visible()) b5_lyrics_btn(); au_settle(); np_update(false); frame(100); return true; }
    if (!strcmp(s, "album")) { album_page(a); au_settle(); return true; }
    if (!strncmp(s, "settings-np", 11)) { np_style_page(); au_settle(); return true; }
    if (!strncmp(s, "settings-lyrics", 15)) { lyrics_color_page(); au_settle(); return true; }
    return false;
}
static void b5_shots(const char *dir, const char *tag) {
    static const struct { const char *screen, *variant; int look, step, lang, album; } shots[] = {
        { "now-playing", "dark", 0, 0, 0, 0 }, { "now-playing", "light", 1, 0, 0, 3 }, { "now-playing", "zule", 6, 0, 0, 2 },
        { "now-playing", "largest-dark", 0, 3, 0, 4 }, { "now-playing", "he-dark", 0, 0, 4, 1 }, { "now-playing", "ar-light", 1, 0, 5, 5 },
        { "now-playing-classic", "dark", 0, 0, 0, 0 }, { "now-playing-classic", "light", 1, 0, 0, 3 },
        { "lyrics", "dark", 0, 0, 0, 0 }, { "lyrics", "light", 1, 0, 0, 3 }, { "lyrics", "zule", 6, 0, 0, 2 },
        { "lyrics", "largest-dark", 0, 3, 0, 4 }, { "lyrics", "he-dark", 0, 0, 4, 1 }, { "lyrics", "ar-light", 1, 0, 5, 5 },
        { "album", "dark", 0, 0, 0, 0 }, { "album", "light", 1, 0, 0, 3 }, { "album", "zule", 6, 0, 0, 2 },
        { "album", "largest-dark", 0, 3, 0, 4 }, { "album", "he-dark", 0, 0, 4, 1 }, { "album", "ar-light", 1, 0, 5, 5 },
        { "now-playing-centred", "dark", 0, 0, 0, 0 }, { "now-playing-centred", "light", 1, 0, 0, 3 },
        { "now-playing-centred", "busy-dark", 0, 0, 0, 4 }, { "now-playing-centred", "largest-dark", 0, 3, 0, 5 },
        { "now-playing-centred", "he-dark", 0, 0, 4, 1 }, { "now-playing-centred", "ar-light", 1, 0, 5, 2 },
        { "lyrics-centred", "dark", 0, 0, 0, 0 }, { "lyrics-centred", "light", 1, 0, 0, 3 }, { "lyrics-centred", "he-dark", 0, 0, 4, 1 },
        { "now-playing-status", "dark", 0, 0, 0, 0 }, { "now-playing-status", "light", 1, 0, 0, 3 }, { "now-playing-status", "zule", 6, 0, 0, 2 },
        { "now-playing-status", "largest-dark", 0, 3, 0, 4 }, { "now-playing-status", "ar-light", 1, 0, 5, 5 },
        { "now-playing-ink", "dark", 0, 0, 0, 0 }, { "now-playing-ink", "light", 1, 0, 0, 1 }, { "now-playing-ink", "busy-dark", 0, 0, 0, 4 },
        { "now-playing-star", "dark", 0, 0, 0, 5 }, { "now-playing-star", "light", 1, 0, 0, 1 },
        { "lyrics-large", "dark", 0, 0, 0, 0 }, { "lyrics-small", "dark", 0, 0, 0, 0 }, { "lyrics-large", "largest-dark", 0, 3, 0, 4 },
        { "lyrics-status", "dark", 0, 0, 0, 0 },
        { "lyrics-picker", "dark", 0, 0, 0, 0 }, { "lyrics-picker", "light", 1, 0, 0, 3 }, { "lyrics-picker", "zule", 6, 0, 0, 2 },
        { "lyrics-picker", "largest-dark", 0, 3, 0, 4 }, { "lyrics-picker", "he-dark", 0, 0, 4, 1 }, { "lyrics-picker", "ar-light", 1, 0, 5, 5 },
        { "lyrics-wifi", "dark", 0, 0, 0, 0 }, { "lyrics-wifi", "light", 1, 0, 0, 3 }, { "lyrics-wifi", "ar-light", 1, 0, 5, 5 },
        { "settings-np", "dark", 0, 0, 0, 0 }, { "settings-np", "light", 1, 0, 0, 0 }, { "settings-np", "zule", 6, 0, 0, 0 },
        { "settings-np", "largest-dark", 0, 3, 0, 0 }, { "settings-np", "he-dark", 0, 0, 4, 0 }, { "settings-np", "ar-light", 1, 0, 5, 0 },
        { "settings-np-bottom", "dark", 0, 0, 0, 0 }, { "settings-np-bottom", "light", 1, 0, 0, 0 },
        { "settings-np-bottom", "largest-dark", 0, 3, 0, 0 }, { "settings-np-bottom", "he-dark", 0, 0, 4, 0 },
        { "settings-lyrics", "dark", 0, 0, 0, 0 }, { "settings-lyrics", "light", 1, 0, 0, 0 }, { "settings-lyrics", "zule", 6, 0, 0, 0 },
        { "settings-lyrics", "largest-dark", 0, 3, 0, 0 }, { "settings-lyrics", "he-dark", 0, 0, 4, 0 }, { "settings-lyrics", "ar-light", 1, 0, 5, 0 },
        { "settings-lyrics-xl", "dark", 0, 0, 0, 0 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    test_real_art = true; b4_shooting = true;
    if (!np_img_buf) np_img_buf = calloc(W * W, 2);   /* as sloppy_main makes it */
    for (unsigned i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        const char *s = shots[i].screen;
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        global_settings.sloppy_np_style = !strcmp(s, "now-playing-classic") ? 1 : strstr(s, "-centred") ? 2 : 0;
        global_settings.sloppy_np_status = strstr(s, "-status") != NULL;
        global_settings.sloppy_np_cover_ink = strstr(s, "-ink") != NULL;
        global_settings.sloppy_love_star = strstr(s, "-star") != NULL;
        global_settings.sloppy_lyrics_size = strstr(s, "-large") ? 2 : strstr(s, "-small") ? 0 : strstr(s, "-xl") ? 3 : 1;
        bool b5_bottom = strstr(s, "-bottom") != NULL;
        lyr_want = false; lyr_show(false);   /* each shot opens the lyrics itself */
        au_apply(shots[i].look, shots[i].step, shots[i].lang);
        b5_play(shots[i].album);
        if (global_settings.sloppy_love_star) ts_love(sl_lib_find_path(test_track.path), true);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        if (!b5_open(s, shots[i].album)) continue;
        if (b5_bottom) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, shots[i].variant, tag);
        shot_now(p);
        if (global_settings.sloppy_love_star) ts_love(sl_lib_find_path(test_track.path), false);
        global_settings.sloppy_np_style = 0; global_settings.sloppy_np_status = 0; global_settings.sloppy_np_cover_ink = 0;
        global_settings.sloppy_love_star = 0; global_settings.sloppy_lyrics_size = 1; wf_st.connected = false;
        au_reset(); au_empty(false);
    }
    test_real_art = false; b4_shooting = false;
    au_library_done();
}
/* SLOPPY_KB_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: every keyboard, in
 * dark, light and Zule, at Largest, in Hebrew, Arabic and Chinese */
static bool kb_open_shot(const char *s) {
    struct kb *kb;
    if (!strncmp(s, "search", 6)) {
        search_page(); for (int i = 0; i < 12; i++) frame(100);
        kb = sq_kbd; if (!kb) return false;
        if (!strcmp(s, "search")) return true;
        if (!strcmp(s, "search-hebrew")) { kb_switch(kb, KB_HE); frame(200); return true; }
        if (!strcmp(s, "search-arabic")) { kb_switch(kb, KB_AR); frame(200); return true; }
        if (!strcmp(s, "search-pinyin")) { kb_switch(kb, KB_ZH); frame(200); kb_type(kb, "yinyue"); frame(200); return true; }
        if (!strcmp(s, "search-123")) { kb_tap_act(kb, KA_MODE); frame(200); return true; }
        if (!strcmp(s, "search-draw")) { kb_tap_act(kb, KA_DRAW); frame(200); return true; }
        if (!strcmp(s, "search-glide") || !strcmp(s, "search-suggestions")) {
            kb_switch(kb, KB_EN); for (int i = 0; i < 12; i++) frame(100);
            kb_glide(kb, "night", !strcmp(s, "search-suggestions")); frame(100); return true; }
        return false;
    }
    if (!strcmp(s, "keyboard-text")) { wf_password_page("Home Network"); frame(400); return true; }
    if (!strcmp(s, "keyboard-symbols")) { wf_password_page("Home Network"); frame(400); kb_tap_act(kb_now(), KA_MODE); frame(100); return true; }
    if (!strcmp(s, "keyboard-name") || !strcmp(s, "keyboard-accents") || !strcmp(s, "keyboard-pinyin-candidates")) {
        eq_save_page(NULL); frame(400); kb = kb_now(); if (!kb) return false;
        lv_textarea_set_text(kb->ta, ""); kb_changed(kb); frame(100);
        if (!strcmp(s, "keyboard-name")) {
            kb_type(kb, kb->lay == KB_HE ? "\xd7\xa9\xd7\x9c\xd7\x95" : kb->lay == KB_AR ? "\xd9\x85\xd9\x88\xd8\xb3" :
                        kb->lay == KB_ZH ? "yinyue" : "Late nig");
            frame(200); return true; }
        if (!strcmp(s, "keyboard-pinyin-candidates")) { kb_switch(kb, KB_ZH); kb_type(kb, "nihao"); frame(200); return true; }
        kb_type(kb, "Caf"); int i = kb_find(kb, 'e'), x, y; kb_xy(kb, i, &x, &y);
        touch_at(x, y, true); for (int k = 0; k < 12; k++) touch_at(x, y, true);
        lv_area_t a; lv_obj_get_coords(kb->acc, &a); touch_at(a.x1 + 30, y - 70, true); touch_at(a.x1 + 30, y - 70, true);
        return true;
    }
    if (!strncmp(s, "keyboard-layout-", 16)) {
        eq_save_page(NULL); frame(400); kb = kb_now(); if (!kb) return false;
        lv_textarea_set_text(kb->ta, ""); kb_changed(kb);
        for (int k = 0; k < KB_N; k++) {
            static const char *const ids[KB_N] = { "en", "es", "fr", "de", "he", "ar", "zh", "it", "pt", "nl", "pl", "cs", "ru", "t9", "ja" };
            if (!strcmp(s + 16, ids[k])) { kb_switch(kb, k); frame(200); return true; }
        }
        return false;
    }
    if (!strcmp(s, "playlist-new")) { au_playlist_new(); frame(300); return true; }
    if (!strcmp(s, "library-cjk")) {   /* Chinese, Japanese and Russian titles in the All list */
        static const char *const t[3] = { "\xe6\x99\xb4\xe5\xa4\xa9 \xc2\xb7 \xe5\x91\xa8\xe6\x9d\xb0\xe5\x80\xab",
            "\xe5\xa4\x9c\xe3\x81\xab\xe9\xa7\x86\xe3\x81\x91\xe3\x82\x8b", "\xd0\x9a\xd1\x83\xd0\xba\xd0\xbb\xd0\xb0 \xd0\x9a\xd0\xbe\xd0\xbb\xd0\xb4\xd1\x83\xd0\xbd\xd1\x8c\xd1\x8f" };
        static const char *saved[3];
        for (int i = 0; i < 3; i++) { saved[i] = sl_lib.tracks[au_songs[i]].title; sl_lib.tracks[au_songs[i]].title = t[i]; }
        songs_page(); frame(500);
        for (int i = 0; i < 3; i++) sl_lib.tracks[au_songs[i]].title = saved[i];
        return true;
    }
    if (!strcmp(s, "settings-keyboards")) { keyboards_page(); frame(300); return true; }
    return false;
}
static void kb_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } vs[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 },
        { "he-dark", 0, 0, 4 }, { "ar-light", 1, 0, 5 }, { "zh-dark", 0, 0, 6 }, { "zh-light", 1, 0, 6 },
    };
    static const struct { const char *name; int nv; } screens[] = {
        { "search", 8 }, { "search-hebrew", 8 }, { "search-arabic", 8 }, { "search-pinyin", 8 }, { "search-123", 8 },
        { "search-draw", 8 }, { "search-glide", 8 }, { "search-suggestions", 8 }, { "keyboard-text", 8 },
        { "keyboard-symbols", 8 }, { "keyboard-name", 8 }, { "keyboard-accents", 3 }, { "keyboard-pinyin-candidates", 8 },
        { "playlist-new", 8 }, { "settings-keyboards", 8 }, { "library-cjk", 8 },
        { "keyboard-layout-en", 3 }, { "keyboard-layout-es", 3 }, { "keyboard-layout-fr", 3 }, { "keyboard-layout-de", 3 },
        { "keyboard-layout-it", 3 }, { "keyboard-layout-pt", 3 }, { "keyboard-layout-nl", 3 }, { "keyboard-layout-pl", 3 },
        { "keyboard-layout-cs", 3 }, { "keyboard-layout-ru", 3 }, { "keyboard-layout-he", 3 }, { "keyboard-layout-ar", 3 },
        { "keyboard-layout-zh", 3 }, { "keyboard-layout-ja", 3 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned s = 0; s < sizeof screens / sizeof screens[0]; s++)
        for (int v = 0; v < screens[s].nv; v++) {
            if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(screens[s].name, getenv("SLOPPY_AUDIT_ONLY"))) continue;
            if (vs[v].lang > SL_LANG_LAST) continue;
            au_apply(vs[v].look, vs[v].step, vs[v].lang);
            test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
            if (!kb_open_shot(screens[s].name)) { au_reset(); continue; }
            frame(0);
            snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, screens[s].name, vs[v].variant, tag);
            shot_now(p);
            t_down = false; frame(100);
            au_reset();
        }
    au_library_done();
}

/* ---- glide accuracy: a fixed list of words a language, synthetic finger
 * paths through their keys (jittered, corners cut, as a fingertip glides),
 * read by the decoder as it was before the keyboards batch (st_old.h: the
 * old search grid, the words of the library and the settings' names) and
 * as it is now (the keyboard's own layout and dictionary). */
#include "st_old.h"
static const struct { int lay, lang; const char *words; } ga_lists[] = {
    { KB_EN, 0, "night love dance heart music summer dream river light world forever never always together beautiful remember tonight morning shadow fire ocean sweet happy little people think should their about would" },
    { KB_ES, 1, "noche amor coraz\xc3\xb3n vida canci\xc3\xb3n m\xc3\xbasica tiempo siempre nunca ciudad mundo verano fuego cielo bailar cantar sue\xc3\xb1o luna estrella hermosa ma\xc3\xb1" "ana mujer gente camino tierra libertad gracias porque cuando tambi\xc3\xa9n" },
    { KB_FR, 2, "nuit amour c\xc5\x93ur vie chanson musique temps toujours jamais ville monde \xc3\xa9t\xc3\xa9 feu ciel danser chanter r\xc3\xaave lune \xc3\xa9toile belle demain femme gens chemin terre libert\xc3\xa9 merci comme quand aussi" },
    { KB_DE, 3, "nacht liebe herz leben lied musik zeit immer nie stadt welt sommer feuer himmel tanzen singen traum mond stern sch\xc3\xb6n morgen frau leute weg erde freiheit danke weil wenn auch" },
    { KB_IT, 0, "notte amore cuore vita canzone musica tempo sempre mai citt\xc3\xa0 mondo estate fuoco cielo ballare cantare sogno luna stella bella" },
    { KB_PT, 0, "noite amor cora\xc3\xa7\xc3\xa3o vida can\xc3\xa7\xc3\xa3o m\xc3\xbasica tempo sempre nunca cidade mundo ver\xc3\xa3o fogo c\xc3\xa9u dan\xc3\xa7" "ar cantar sonho lua estrela saudade" },
    { KB_NL, 0, "nacht liefde hart leven liedje muziek tijd altijd nooit stad wereld zomer vuur hemel dansen zingen droom maan ster mooi" },
    { KB_PL, 0, "noc mi\xc5\x82o\xc5\x9b\xc4\x87 serce \xc5\xbc" "ycie piosenka muzyka czas zawsze nigdy miasto \xc5\x9bwiat lato ogie\xc5\x84 niebo ta\xc5\x84" "czy\xc4\x87 \xc5\x9bpiewa\xc4\x87 sen ksi\xc4\x99\xc5\xbc" "yc gwiazda pi\xc4\x99kna" },
    { KB_CS, 0, "noc l\xc3\xa1ska srdce \xc5\xbeivot p\xc3\xad" "se\xc5\x88 hudba \xc4\x8d" "as v\xc5\xbe" "dy nikdy m\xc4\x9bsto sv\xc4\x9bt l\xc3\xa9to ohe\xc5\x88 nebe tan\xc4\x8dit zp\xc3\xadvat sen m\xc4\x9bs\xc3\xad" "c hv\xc4\x9bzda kr\xc3\xa1sn\xc3\xa1" },
    { KB_RU, 0, "\xd0\xbd\xd0\xbe\xd1\x87\xd1\x8c \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xbe\xd0\xb2\xd1\x8c \xd1\x81\xd0\xb5\xd1\x80\xd0\xb4\xd1\x86\xd0\xb5 \xd0\xb6\xd0\xb8\xd0\xb7\xd0\xbd\xd1\x8c \xd0\xbf\xd0\xb5\xd1\x81\xd0\xbd\xd1\x8f \xd0\xbc\xd1\x83\xd0\xb7\xd1\x8b\xd0\xba\xd0\xb0 \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f \xd0\xb2\xd1\x81\xd0\xb5\xd0\xb3\xd0\xb4\xd0\xb0 \xd0\xbd\xd0\xb8\xd0\xba\xd0\xbe\xd0\xb3\xd0\xb4\xd0\xb0 \xd0\xb3\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb4 \xd0\xbc\xd0\xb8\xd1\x80 \xd0\xbb\xd0\xb5\xd1\x82\xd0\xbe \xd0\xbe\xd0\xb3\xd0\xbe\xd0\xbd\xd1\x8c \xd0\xbd\xd0\xb5\xd0\xb1\xd0\xbe \xd1\x82\xd0\xb0\xd0\xbd\xd1\x86\xd0\xb5\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbf\xd0\xb5\xd1\x82\xd1\x8c \xd1\x81\xd0\xbe\xd0\xbd \xd0\xbb\xd1\x83\xd0\xbd\xd0\xb0 \xd0\xb7\xd0\xb2\xd0\xb5\xd0\xb7\xd0\xb4\xd0\xb0 \xd0\xba\xd1\x80\xd0\xb0\xd1\x81\xd0\xb8\xd0\xb2\xd0\xb0\xd1\x8f" },
    { KB_HE, 4, "\xd7\x9c\xd7\x99\xd7\x9c\xd7\x94 \xd7\x90\xd7\x94\xd7\x91\xd7\x94 \xd7\x9c\xd7\x91 \xd7\x97\xd7\x99\xd7\x99\xd7\x9d \xd7\xa9\xd7\x99\xd7\xa8 \xd7\x9e\xd7\x95\xd7\x96\xd7\x99\xd7\xa7\xd7\x94 \xd7\x96\xd7\x9e\xd7\x9f \xd7\xaa\xd7\x9e\xd7\x99\xd7\x93 \xd7\xa2\xd7\x99\xd7\xa8 \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d \xd7\xa7\xd7\x99\xd7\xa5 \xd7\x90\xd7\xa9 \xd7\xa9\xd7\x9e\xd7\x99\xd7\x99\xd7\x9d \xd7\x9c\xd7\xa8\xd7\xa7\xd7\x95\xd7\x93 \xd7\x9c\xd7\xa9\xd7\x99\xd7\xa8 \xd7\x97\xd7\x9c\xd7\x95\xd7\x9d \xd7\x99\xd7\xa8\xd7\x97 \xd7\x9b\xd7\x95\xd7\x9b\xd7\x91 \xd7\x99\xd7\xa4\xd7\x94 \xd7\x9e\xd7\x97\xd7\xa8 \xd7\x90\xd7\x99\xd7\xa9\xd7\x94 \xd7\x90\xd7\xa0\xd7\xa9\xd7\x99\xd7\x9d \xd7\x93\xd7\xa8\xd7\x9a \xd7\x90\xd7\x93\xd7\x9e\xd7\x94 \xd7\x97\xd7\x95\xd7\xa4\xd7\xa9 \xd7\xaa\xd7\x95\xd7\x93\xd7\x94 \xd7\x9e\xd7\xaa\xd7\x99 \xd7\x92\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x94\xd7\x92\xd7\x93\xd7\xa8\xd7\x95\xd7\xaa" },
    { KB_AR, 5, "\xd9\x84\xd9\x8a\xd9\x84 \xd8\xad\xd8\xa8 \xd9\x82\xd9\x84\xd8\xa8 \xd8\xad\xd9\x8a\xd8\xa7\xd8\xa9 \xd8\xa3\xd8\xba\xd9\x86\xd9\x8a\xd8\xa9 \xd9\x85\xd9\x88\xd8\xb3\xd9\x8a\xd9\x82\xd9\x89 \xd9\x88\xd9\x82\xd8\xaa \xd8\xaf\xd8\xa7\xd8\xa6\xd9\x85\xd8\xa7 \xd9\x85\xd8\xaf\xd9\x8a\xd9\x86\xd8\xa9 \xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd8\xb5\xd9\x8a\xd9\x81 \xd9\x86\xd8\xa7\xd8\xb1 \xd8\xb3\xd9\x85\xd8\xa7\xd8\xa1 \xd8\xb1\xd9\x82\xd8\xb5 \xd8\xad\xd9\x84\xd9\x85 \xd9\x82\xd9\x85\xd8\xb1 \xd9\x86\xd8\xac\xd9\x85\xd8\xa9 \xd8\xac\xd9\x85\xd9\x8a\xd9\x84 \xd8\xba\xd8\xaf\xd8\xa7 \xd8\xa7\xd9\x85\xd8\xb1\xd8\xa3\xd8\xa9 \xd9\x86\xd8\xa7\xd8\xb3 \xd8\xb7\xd8\xb1\xd9\x8a\xd9\x82 \xd8\xa3\xd8\xb1\xd8\xb6 \xd8\xad\xd8\xb1\xd9\x8a\xd8\xa9 \xd8\xb4\xd9\x83\xd8\xb1\xd8\xa7 \xd9\x84\xd9\x83\xd9\x86 \xd9\x85\xd8\xaa\xd9\x89 \xd8\xb5\xd8\xaf\xd9\x8a\xd9\x82 \xd8\xa8\xd8\xad\xd8\xb1 \xd8\xa7\xd9\x84\xd8\xad\xd8\xa8" },
};
static unsigned ga_rng;
static float ga_j(float amp) { ga_rng = ga_rng * 1103515245u + 12345u; return ((ga_rng >> 8) % 1000 / 1000.0f - 0.5f) * 2 * amp; }
/* a fingertip through these key centres: each corner missed by up to wob,
 * cut a little short, sampled every few pixels with a tremor */
static int ga_finger(const float *kx, const float *ky, int nk, float *px, float *py, float wob)
{
    float cx[ST_WORD_MAX], cy[ST_WORD_MAX];
    for (int i = 0; i < nk; i++) { cx[i] = kx[i] + ga_j(wob); cy[i] = ky[i] + ga_j(wob); }
    for (int i = 1; i + 1 < nk; i++) {   /* corners cut short, by up to a third of a key */
        float mx = (cx[i - 1] + cx[i + 1]) / 2 - cx[i], my = (cy[i - 1] + cy[i + 1]) / 2 - cy[i], d = hypotf(mx, my), cut = d * 0.15f;
        if (cut > wob) cut = wob;
        if (d > 0) { cx[i] += mx / d * cut; cy[i] += my / d * cut; } }
    int n = 0;
    px[n] = cx[0]; py[n++] = cy[0];
    for (int i = 1; i < nk; i++) {
        float d = hypotf(cx[i] - cx[i - 1], cy[i] - cy[i - 1]);
        int steps = (int)(d / 8) + 1;
        for (int s = 1; s <= steps && n < KB_PATH_MAX; s++) {
            px[n] = cx[i - 1] + (cx[i] - cx[i - 1]) * s / steps + ga_j(1.5f);
            py[n] = cy[i - 1] + (cy[i] - cy[i - 1]) * s / steps + ga_j(1.5f); n++; }
    }
    if (nk == 1 || n < 2) { px[n] = cx[0] + 1; py[n++] = cy[0]; }
    return n;
}
/* the search grid as it was: QWERTY or Hebrew in rows of 40 px keys, 6 px apart */
static bool ga_old_layout(int lay, struct sto_layout *l)
{
    static const char *const latin[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    const char *const *rows;
    const char *heb[3] = { kb_defs[KB_HE].rows[0], kb_defs[KB_HE].rows[1], kb_defs[KB_HE].rows[2] };
    if (lay == KB_AR || lay == KB_RU) return false;          /* no keyboard for them */
    rows = lay == KB_HE ? heb : latin;
    l->n = 0; l->key_w = 40;
    for (int r = 0; r < 3; r++) {
        int n = 0; for (const char *s = rows[r]; *s; st_utf8_next(&s)) n++;
        float row_w = n * 40 + (n - 1) * 6, x0 = 12 + (456 - row_w) / 2 + 20;
        const char *s = rows[r]; uint32_t c;
        for (int i = 0; (c = st_utf8_next(&s)); i++) l->k[l->n++] = (struct sto_key){ c, x0 + i * 46, H - 332 + 72 + r * 64 + 29 };
    }
    return true;
}
static void glide_accuracy(bool strict)
{
    au_library();
    int lang0 = global_settings.sloppy_language;
    eq_save_page(NULL); frame(400);
    struct kb *kb = kb_now(); assert(kb);
    static float px[KB_PATH_MAX], py[KB_PATH_MAX];
    int sum_b = 0, sum_a = 0, sum_a3 = 0, sum_n = 0;
    uint32_t worst_us = 0, total_us = 0; int decodes = 0;
    printf("glide accuracy (top pick / in the bar's three), %s:\n", "6 glides a word, finger off the key centres by up to 15-35 % of a key");
    for (unsigned li = 0; li < ARRAYLEN(ga_lists); li++) {
        int lay = ga_lists[li].lay;
        kb_switch(kb, lay); frame(50);
        assert(kb->voc.w && kb->voc.n > 100);
        kb_layout_geo(kb);
        /* before: the old grid, the library's words and the settings' names in the language */
        struct sto_layout ol; bool have_old = ga_old_layout(lay, &ol);
        static struct sto_word ow[4000]; static char oa[60000]; static int oh[8000];
        struct sto_vocab ov = { ow, 0, 4000, oa, 0, sizeof oa, oh, 8000 };
        if (have_old) {
            for (int i = 0; i < 8000; i++) oh[i] = -1;
            global_settings.sloppy_language = ga_lists[li].lang;
            for (int i = 0; i < (int)ARRAYLEN(sq_settings); i++) { sto_vocab_add(&ol, &ov, sl_tr(sq_settings[i].name)); sto_vocab_add(&ol, &ov, sq_settings[i].words); }
            for (int t = 0; t < sl_lib.n_tracks; t++) { sto_vocab_add(&ol, &ov, sl_lib.tracks[t].title);
                sto_vocab_add(&ol, &ov, sl_lib.albums[sl_lib.tracks[t].album].title); sto_vocab_add(&ol, &ov, sl_lib.albums[sl_lib.tracks[t].album].artist); }
            global_settings.sloppy_language = lang0;
        }
        int nb = 0, na = 0, na3 = 0, n = 0;
        const char *list = ga_lists[li].words;
        FILE *dump = NULL;
        if (getenv("GA_DUMP")) {
            char fn[600]; snprintf(fn, sizeof fn, "%s/lay%u.txt", getenv("GA_DUMP"), li); dump = fopen(fn, "w");
            fprintf(dump, "%g %g %d\n", kb->geo.key_w, kb->geo.key_h, kb->geo.n);
            for (int i = 0; i < kb->geo.n; i++) fprintf(dump, "%u %g %g\n", kb->geo.k[i].cp, kb->geo.k[i].x, kb->geo.k[i].y);
            fprintf(dump, "%d\n", kb->voc.n);
            for (int i = 0; i < kb->voc.n; i++) fprintf(dump, "%s %d\n", kb->voc.arena + kb->voc.w[i].off, kb->voc.w[i].count);
        }
        char w[ST_WORD_MAX * 4];
        while (*list) {
            int k = 0; while (*list && *list != ' ') w[k++] = *list++; w[k] = 0; while (*list == ' ') list++;
            for (int trial = 0; trial < 6; trial++) {
                float wob = kb->geo.key_w * (0.15f + 0.1f * (trial % 3));
                ga_rng = 7919u * (unsigned)(li * 977 + n * 13 + trial + 1);
                int keys[ST_WORD_MAX]; float kx[ST_WORD_MAX], ky[ST_WORD_MAX];
                int nk = st_word_keys(&kb->geo, w, keys); assert(nk > 0);
                for (int i = 0; i < nk; i++) { kx[i] = kb->geo.k[keys[i]].x; ky[i] = kb->geo.k[keys[i]].y; }
                int m = ga_finger(kx, ky, nk, px, py, wob);
                if (dump) { fprintf(dump, "%s %d", w, m); for (int i = 0; i < m; i++) fprintf(dump, " %.1f %.1f", px[i], py[i]); fprintf(dump, "\n"); }
                struct st_match best[ST_BEST];
                uint32_t t0 = sl_ms();
                struct timespec a0, a1; clock_gettime(CLOCK_MONOTONIC, &a0);
                int got = st_decode(&kb->geo, &kb->voc, px, py, m, best);
                clock_gettime(CLOCK_MONOTONIC, &a1); (void)t0;
                uint32_t us = (uint32_t)((a1.tv_sec - a0.tv_sec) * 1000000 + (a1.tv_nsec - a0.tv_nsec) / 1000);
                total_us += us; decodes++; if (us > worst_us) worst_us = us;
                if (got && !strcmp(kb->voc.arena + kb->voc.w[best[0].word].off, w)) na++;
                else if (getenv("GA_DBG") && li == (unsigned)atoi(getenv("GA_DBG"))) {
                    fprintf(stderr, "%s (wob %.0f):", w, wob);
                    for (int j = 0; j < got && j < 4; j++) fprintf(stderr, " %s %.2f", kb->voc.arena + kb->voc.w[best[j].word].off, best[j].score);
                    fprintf(stderr, "\n"); }
                for (int j = 0; j < got && j < 3; j++) if (!strcmp(kb->voc.arena + kb->voc.w[best[j].word].off, w)) { na3++; break; }
                if (have_old) {
                    /* the same glide on the old grid: its keys, its geometry */
                    float ox[ST_WORD_MAX], oy[ST_WORD_MAX]; int on = 0; bool ok = true;
                    const char *q = w; uint32_t c;
                    while ((c = st_utf8_next(&q))) { int kk = sto_key_of(&ol, st_lower(c)); if (kk < 0) { ok = false; break; }
                        if (on && ox[on - 1] == ol.k[kk].x && oy[on - 1] == ol.k[kk].y) continue; ox[on] = ol.k[kk].x; oy[on++] = ol.k[kk].y; }
                    if (ok) {
                        ga_rng = 7919u * (unsigned)(li * 977 + n * 13 + trial + 1);
                        int om = ga_finger(ox, oy, on, px, py, 40 * (0.15f + 0.1f * (trial % 3)));
                        struct sto_match ob[STO_BEST];
                        int og = sto_decode(&ol, &ov, px, py, om, ob);
                        if (og && !strcmp(oa + ow[ob[0].word].off, w)) nb++;
                    }
                }
                n++;
            }
        }
        if (dump) fclose(dump);
        printf("  %-10s %3d glides: before %3d%%%s, after %3d%% first, %3d%% in the bar\n", kb_defs[lay].name, n,
               nb * 100 / n, have_old ? "" : " (no keyboard)", na * 100 / n, na3 * 100 / n);
        sum_b += nb; sum_a += na; sum_a3 += na3; sum_n += n;
        if (strict) assert(na * 100 >= n * 80 && na3 * 100 >= n * 92);
    }
    printf("  all        %3d glides: before %3d%%, after %3d%% first, %3d%% in the bar; decode %u us average, %u us worst (Mac)\n",
           sum_n, sum_b * 100 / sum_n, sum_a * 100 / sum_n, sum_a3 * 100 / sum_n, decodes ? total_us / decodes : 0, worst_us);
    page_pop(); frame(400);
    au_library_done();
}

/* SLOPPY_KB_PERF=1: how long a keyboard takes to open, to read its words,
 * to decode a glide and to offer hanzi; what the letter packs cost. Mac
 * times, and the R1 by the harness's measured ratio (PF_K) */
static double kbp_us(struct timespec a, struct timespec b) { return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3; }
static void kb_perf(void)
{
    struct timespec a, b;
    au_library();
    double open_us = 0, voc_us = 0, page_us = 0;
    for (int r = 0; r < 20; r++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        eq_save_page(NULL);
        clock_gettime(CLOCK_MONOTONIC, &b); page_us += kbp_us(a, b);
        struct kb *kb = kb_live;
        clock_gettime(CLOCK_MONOTONIC, &a);
        kb_build_keys(kb);
        clock_gettime(CLOCK_MONOTONIC, &b); open_us += kbp_us(a, b);
        lv_obj_update_layout(kb->root);
        clock_gettime(CLOCK_MONOTONIC, &a);
        kb_voc_begin(kb);
        clock_gettime(CLOCK_MONOTONIC, &b); voc_us += kbp_us(a, b);
        frame(0); page_pop(); frame(400);
    }
    printf("keyboard perf (Mac; R1 at x%.0f):\n", PF_K);
    printf("  a page with its keyboard: %.0f us (R1 ~%.0f ms); the keys alone %.0f us (R1 ~%.0f ms)\n",
           page_us / 20, page_us / 20 * PF_K / 1000, open_us / 20, open_us / 20 * PF_K / 1000);
    printf("  its dictionary read (English, %d words): %.0f us (R1 ~%.0f ms), in the first glide or tap\n",
           1399, voc_us / 20, voc_us / 20 * PF_K / 1000);
    /* first frame of the search page, keyboard up */
    clock_gettime(CLOCK_MONOTONIC, &a);
    search_page(); frame(0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("  Search, open to its first frame: %.0f us (R1 ~%.0f ms)\n", kbp_us(a, b), kbp_us(a, b) * PF_K / 1000);
    for (int i = 0; i < 20; i++) frame(100);
    struct kb *kb = sq_kbd;
    kb_layout_geo(kb);
    static float px[KB_PATH_MAX], py[KB_PATH_MAX];
    int keys[ST_WORD_MAX]; float kx[ST_WORD_MAX], ky[ST_WORD_MAX];
    const char *words[] = { "night", "beautiful", "remember", "together", "music" };
    double dec = 0, worst = 0; int nd = 0;
    for (int w = 0; w < 5; w++) {
        int nk = st_word_keys(&kb->geo, words[w], keys);
        for (int i = 0; i < nk; i++) { kx[i] = kb->geo.k[keys[i]].x; ky[i] = kb->geo.k[keys[i]].y; }
        for (int t = 0; t < 10; t++) {
            ga_rng = 99u + (unsigned)(w * 31 + t);
            int m = ga_finger(kx, ky, nk, px, py, 10);
            struct st_match best[ST_BEST];
            clock_gettime(CLOCK_MONOTONIC, &a);
            st_decode(&kb->geo, &kb->voc, px, py, m, best);
            clock_gettime(CLOCK_MONOTONIC, &b);
            double u = kbp_us(a, b); dec += u; nd++; if (u > worst) worst = u;
        }
    }
    printf("  a glide decoded against %d words (dictionary and library): %.0f us average, %.0f us worst (R1 ~%.0f ms, worst ~%.0f ms)\n",
           kb->voc.n, dec / nd, worst, dec / nd * PF_K / 1000, worst * PF_K / 1000);
    printf("  words in memory: %d KB for search, the dictionary alone %d KB\n",
           (int)((kb->voc.n * sizeof *kb->voc.w + kb->voc.used) / 1024), (int)((1399 * sizeof *kb->voc.w + sizeof kb_dict_en) / 1024));
    page_pop(); frame(400);
    /* pinyin */
    eq_save_page(NULL); frame(400); kb = kb_live; kb_switch(kb, KB_ZH);
    const char *ins[] = { "nihao", "zjl", "yinyueh", "shi", "zhongguoren" };
    double pyt = 0; int npy = 0;
    for (int i = 0; i < 5; i++) for (int r = 0; r < 20; r++) {
        strlcpy(kb->py_buf, ins[i], sizeof kb->py_buf);
        clock_gettime(CLOCK_MONOTONIC, &a); kb_py_candidates(kb); clock_gettime(CLOCK_MONOTONIC, &b);
        pyt += kbp_us(a, b); npy++;
    }
    printf("  pinyin candidates: %.0f us (R1 ~%.1f ms)\n", pyt / npy, pyt / npy * PF_K / 1000);
    kb->py_buf[0] = 0; page_pop(); frame(400);
    /* the letter packs: loaded on first need */
    for (int i = 0; i < XF_PACKS; i++) { free(xf_packs[i].mem); xf_packs[i].mem = NULL; xf_packs[i].tried = false; }
    xf_bytes = 0;
    lv_font_glyph_dsc_t g;
    for (int si = 0; si < XF_SIZES; si++) {
        const lv_font_t *f = si == 0 ? &sl_xf_16 : si == 1 ? &sl_xf_18 : si == 2 ? &sl_xf_21 : si == 3 ? &sl_xf_23 : si == 4 ? &sl_xf_26 : si == 5 ? &sl_xf_28 : &sl_xf_30;
        clock_gettime(CLOCK_MONOTONIC, &a);
        lv_font_get_glyph_dsc(f, &g, 0x4e2d, 0);       /* 中 */
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (kbp_us(a, b) > 1000) printf("  first hanzi at %d px: pack read in %.1f ms (Mac)\n", xf_px[si], kbp_us(a, b) / 1000);
    }
    printf("  Chinese packs in memory once every size is used: %lu KB (simplified)\n", xf_bytes / 1024);
    for (int si = 0; si < 3; si++) lv_font_get_glyph_dsc(si == 0 ? &sl_xf_16 : si == 1 ? &sl_xf_21 : &sl_xf_26, &g, 0x5011, 0);   /* 們, traditional */
    printf("  and with traditional: %lu KB; Cyrillic adds %lu KB at all seven sizes\n", xf_bytes / 1024,
           (unsigned long)(5979 + 7166 + 9446 + 11136 + 13276 + 15064 + 17012) / 1024);
    /* a glyph drawn from a pack */
    uint8_t *buf = malloc(64 * 64); lv_draw_buf_t db = { 0 }; db.data = buf;
    lv_font_get_glyph_dsc(&sl_xf_21, &g, 0x97f3, 0);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 1000; i++) xf_glyph_bitmap(&g, &db);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("  a hanzi unpacked to draw: %.2f us (R1 ~%.0f us)\n", kbp_us(a, b) / 1000, kbp_us(a, b) / 1000 * PF_K);
    free(buf);
    au_library_done();
}

/* SLOPPY_CC_PROOF=<dir> SLOPPY_AUDIT_TAG=<tag> [SLOPPY_CC_TILES=<setting>]: the control centre
 * folded, half grown, grown, folded back and mid pull, in seven looks, for byte compares
 * against another build (the same routine, nothing else: the before-batch-2
 * build ran it with no SLOPPY_CC_TILES, its sixteen kinds) */
static void cc_proof(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 },
        { "he-dark", 0, 0, 4 }, { "ar-light", 1, 0, 5 }, { "de-dark", 0, 0, 3 },
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++) {
        au_apply(looks[i].look, looks[i].step, looks[i].lang);
        if (getenv("SLOPPY_CC_TILES")) {
            strlcpy((char *)global_settings.sloppy_qp_tiles, getenv("SLOPPY_CC_TILES"), sizeof global_settings.sloppy_qp_tiles);
            quick_panel_rebuild();
        }
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        frame(600);
        for (int st = 0; st < 5; st++) {
            static const char *const names[] = { "folded", "half", "grown", "collapsed", "pulling" };
            if (st == 0) ccx_open();
            else if (st == 1) { qx_set(QX_ROW); frame(300); }
            else if (st == 2) { qx_set(0); frame(300); ccx_grow(); }
            else if (st == 3) { ccx_drag(240, 500, 300); assert(qx_px == 0); }
            else { qp_close(); ccx_settle(); frame(600);
                   touch_at(240, 4, true); for (int y = 20; y <= 320; y += 30) touch_at(240, y, true); }
            frame(0);
            snprintf(p, sizeof p, "%s/cc-%s-%s-%s.ppm", dir, names[st], looks[i].variant, tag);
            shot_now(p);
        }
        touch_at(240, 320, false); ccx_settle(); qp_close(); ccx_settle(); frame(600);
        au_reset(); au_empty(false);
    }
    au_library_done();
}

/* SLOPPY_B16_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the control
 * centre grown (every kind: two pages; sixteen chosen: one), Settings'
 * shortcuts, Library options, Files' hold sheet and picking sheet, and
 * the delete pages, in five looks. Built with B16_BEFORE for the build
 * before the change: the screens it lacks are left out. */
static void b16_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 }, { "he-dark", 0, 0, 4 },
    };
    static const char *const screens[] = {
        "cc-grown", "cc-grown-page-2", "cc-grown-16", "settings-shortcuts", "library-options", "library-options-on",
        "files-sheet", "files-pick-more", "files-delete-one", "files-delete-many", "files-delete-progress",
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++)
    for (unsigned j = 0; j < sizeof screens / sizeof screens[0]; j++) {
        const char *s = screens[j];
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
#ifdef B16_BEFORE
        if (!strcmp(s, "cc-grown-page-2") || !strcmp(s, "library-options-on") || !strncmp(s, "files-delete", 12)) continue;
#endif
        au_apply(looks[i].look, looks[i].step, looks[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
#ifndef B16_BEFORE
        global_settings.sloppy_files_delete = !strncmp(s, "files-", 6) || !strcmp(s, "library-options-on");
#endif
        if (!strcmp(s, "cc-grown")) { ccx_open(); ccx_grow(); }
#ifndef B16_BEFORE
        else if (!strcmp(s, "cc-grown-page-2")) au_cc_page_2();
        else if (!strcmp(s, "files-delete-one")) au_files_delete_one();
        else if (!strcmp(s, "files-delete-many")) au_files_delete_many();
        else if (!strcmp(s, "files-delete-progress")) au_files_delete_progress();
#endif
        else if (!strcmp(s, "cc-grown-16")) {
            strlcpy((char *)global_settings.sloppy_qp_tiles, "0123456789abcdef.", sizeof global_settings.sloppy_qp_tiles);
            quick_panel_rebuild(); ccx_open(); ccx_grow();
        }
        else if (!strcmp(s, "settings-shortcuts")) shortcuts_page();
        else if (!strncmp(s, "library-options", 15)) library_options_page();
        else if (!strcmp(s, "files-sheet")) au_files_sheet();
        else if (!strcmp(s, "files-pick-more")) { au_files_pick(); fm_sel_more(); }
        for (int f = 0; f < 6; f++) frame(120);
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, looks[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
        if (global_settings.sloppy_qp_tiles[0]) { global_settings.sloppy_qp_tiles[0] = '\0'; quick_panel_rebuild(); }
    }
    au_library_done();
}

/* Batch 8: the audiobook pages. SLOPPY_B8_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after.
 * B8_BEFORE skips the pages the batch adds. */
static void b8_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 }, { "he-dark", 0, 0, 4 },
    };
    static const char *const screens[] = {
        "audiobooks", "audiobooks-end", "audiobook", "audiobook-end",
#ifndef B8_BEFORE
        "audiobooks-authors", "audiobooks-author", "audiobooks-series", "audiobooks-finished", "audiobook-bookmarks",
#endif
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++)
    for (unsigned j = 0; j < sizeof screens / sizeof screens[0]; j++) {
        const char *s = screens[j];
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        au_apply(looks[i].look, looks[i].step, looks[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        bool end = strstr(s, "-end") != NULL;
        if (!strncmp(s, "audiobooks", 10) && (end || !s[10])) books_page();
        else if (!strncmp(s, "audiobook", 9) && (end || !s[9])) bk_book_page(0);
#ifndef B8_BEFORE
        else au_open(au_find(s));
#endif
        au_settle();
        if (end) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, looks[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
    }
    au_library_done();
}
/* The audit's books their covers: a picture each in its folder on the card
 * as the worker reads it (the C library's, under the card root), painted:
 * a sky, a sun and hills, each book its own colours */
static void bk_test_covers(void) {
    static const unsigned char pal[3][3][3] = {
        { { 18, 58, 46 }, { 222, 190, 98 }, { 9, 30, 24 } },     /* a green dusk, a gold sun */
        { { 70, 34, 110 }, { 255, 150, 170 }, { 30, 14, 52 } },  /* violet, a pink sun */
        { { 20, 52, 120 }, { 150, 220, 255 }, { 8, 22, 58 } },   /* deep blue, a pale moon */
    };
    static const char *const dir[3] = { "/Audiobooks/The Hobbit", "/Audiobooks/Dune", "/Audiobooks/Sefer" };
    enum { S = 300 };
    unsigned char *rgb = malloc(S * S * 3);
    for (int b = 0; rgb && b < 3; b++) {
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                unsigned char *p = rgb + (y * S + x) * 3;
                int t = y * 255 / S, dx = x - S * 2 / 3, dy = y - S * 2 / 5;
                for (int c = 0; c < 3; c++) p[c] = (unsigned char)((pal[b][0][c] * (255 - t) + pal[b][2][c] * t) / 255);
                if (dx * dx + dy * dy < (S / 6) * (S / 6)) for (int c = 0; c < 3; c++) p[c] = pal[b][1][c];
                int hill = S * 3 / 4 - (int)(S / 10 * sinf(x * 6.2832f / S * (1.5f + b * 0.5f)));
                if (y > hill) for (int c = 0; c < 3; c++) p[c] = (unsigned char)(pal[b][2][c] / 2);
            }
        unsigned char *png = NULL;
        size_t n = sl_png_encode_rgb(rgb, S, S, &png);
        char real[600];
        snprintf(real, sizeof real, "mkdir -p '%s%s'", sim_root_dir, dir[b]); system(real);
        snprintf(real, sizeof real, "%s%s/cover.png", sim_root_dir, dir[b]);
        FILE *f = n ? fopen(real, "wb") : NULL;
        if (f) { fwrite(png, 1, n, f); fclose(f); }
        free(png);
        static char cov[3][80];
        snprintf(cov[b], sizeof cov[b], "%s/cover.png", dir[b]);
        fake_book_str[b][AB_S_COVER] = cov[b];
    }
    free(rgb);
}
/* SLOPPY_BKH_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the Audiobooks home
 * (its top and its end), with nothing on the card, in Dark, Light, True
 * dark and Zule, and in Arabic and Hebrew */
static void bkh_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int theme, home, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "truedark", 2, 0, 0 }, { "zule", 3, 2, 0 }, { "ar-dark", 0, 0, 5 }, { "he-light", 1, 0, 4 },
    };
    static const char *const screens[] = { "home", "home-end", "empty", "library" };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    bk_test_covers();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++)
    for (unsigned j = 0; j < sizeof screens / sizeof screens[0]; j++) {
        const char *s = screens[j];
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        global_settings.sloppy_theme = looks[i].theme; global_settings.sloppy_home_style = looks[i].home;
        global_settings.sloppy_language = looks[i].lang; sl_text_step = 0; global_settings.sloppy_text_size = 0;
        au_reset(); appearance_reload((void *)2); frame(700);
        while (npages > 1) { page_pop(); frame(300); }
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        int n0 = fake_books_n;
        if (!strcmp(s, "empty")) fake_books_n = 0;
        if (!strcmp(s, "library")) bk_library_at(0, false); else books_page();
        for (int k = 0; k < 20; k++) frame(100);   /* the covers come in */
        if (strstr(s, "-end")) {
            lv_obj_t *b = lv_obj_get_child(pages[npages - 1], 1);
            lv_obj_update_layout(b); lv_obj_scroll_to_y(b, 100000, LV_ANIM_OFF); frame(300);
        }
        frame(0);
        snprintf(p, sizeof p, "%s/audiobooks-%s-%s-%s.ppm", dir, s, looks[i].variant, tag);
        shot_now(p);
        fake_books_n = n0;
        au_reset(); au_empty(false);
    }
    global_settings.sloppy_theme = 0; global_settings.sloppy_home_style = 0; global_settings.sloppy_language = 0;
    au_library_done();
}
/* SLOPPY_B9_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 9's games (the
 * Playroom, Snake fresh, mid-game and at its death, the Arcade), light, dark,
 * Zule, Largest and Hebrew */
static void b9_snake_mid(void) {
    snake_page(); frame(200);
    struct sl_snake_state *st = &snake_state;
    snake_steer(1, 0);
    static const int turns[][2] = { { 1, 0 }, { 1, 0 }, { 1, 0 }, { 0, -1 }, { 0, -1 }, { 0, -1 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 }, { 0, 1 } };
    for (unsigned i = 0; i < sizeof turns / sizeof turns[0]; i++) {
        sl_snake_turn(st, turns[i][0], turns[i][1]);
        st->food_x = st->x[0] + st->next_dx; st->food_y = st->y[0] + st->next_dy;
        snake_tick(NULL); frame(20);
    }
    st->food_x = 4; st->food_y = 11; st->food_gold = true; snk_gold_at = snk_now() - 2000;
    for (int i = 0; i < 16; i++) frame(40);   /* the swallow, then the golden apple popping in */
}
static void b9_snake_death(void) {
    b9_snake_mid();
    struct sl_snake_state *st = &snake_state;
    while (st->alive) { snake_tick(NULL); frame(16); }
    for (int i = 0; i < 70; i++) frame(16);
}
static void b9_arcade_card(bool roms) {
    arc_root = FAKE_CARD; arc_last[0] = 0;
    static uint8_t rom[32768];
    mkdir(FAKE_CARD "/Games"); mkdir(FAKE_CARD "/Games/GB"); mkdir(FAKE_CARD "/Games/GBC");
    const char *names[] = { FAKE_CARD "/Games/GB/Test Cart (World).gb", FAKE_CARD "/Games/GBC/Color Test.gbc",
                            FAKE_CARD "/Games/GB/Homebrew Puzzle.gb" };
    for (int i = 0; i < 3; i++) {
        if (!roms) { remove(names[i]); continue; }
        gbt_make(rom, "TEST", i == 1);
        int fd = open(names[i], O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { if (write(fd, rom, sizeof rom) < 0) {} sim_close(fd); }
    }
}
static void b9_arcade_game(bool menu) {
    b9_arcade_card(true); arcade_page(); frame(200);
    arc_play(1); frame(100);
    for (int i = 0; i < 40; i++) frame(17);
    arc.touch = GB_RIGHT; arc.keys = GB_A; arc_show_keys(); frame(17);
    if (menu) { arc_screen_cb(NULL); frame(400); }
}
static void b9_arcade_colours(void) { b9_arcade_game(true); arc_pal_cb(NULL); frame(400); }
static void b9_2048(void) {
    t48_page(); frame(100);
    static const uint8_t v[16] = { 1, 2, 3, 0, 4, 5, 6, 1, 7, 8, 9, 2, 10, 11, 3, 1 };
    memcpy(t48.v, v, 16); t48.score = 12960; t48_best = 20480; t48_over = false;
    memset(t48_joined, 0, sizeof t48_joined); t48_render(false);
}
static void b9_sudoku(void) {
    sdk_loaded = true; sdk_rng = 99; sdk_make(1); sdk.ms = 3 * 60000 + 27000;
    sudoku_page(); frame(100);
    int filled = 0;
    for (int i = 0; i < 81 && filled < 6; i++) if (!sdk.given[i]) { sdk.val[i] = filled == 3 ? (sdk.sol[i] % 9) + 1 : sdk.sol[i]; filled++; }
    for (int i = 80; i > 40; i--) if (!sdk.val[i]) { sdk.notes[i] = (1u << 2) | (1u << 5) | (1u << 9); break; }
    for (int i = 30; i < 81; i++) if (!sdk.given[i] && !sdk.val[i]) { sdk_sel = i; break; }
    sdk_show(); sdk_clock_show();
}
static void b9_points(void) {
    pt_loaded = true; pt_solved = 14; pt_streak = 3; pt_best = 9;
    points_page(); frame(100);
    static const int d[4] = { 3, 8, 8, 1 };
    for (int k = 0; k < 4; k++) { pt.deal[k] = d[k]; pt.c[k] = pt_make(d[k], 1); }
    pt.sel = 1; pt.op = 2; pt.nhist = 0; pt.won = false; pt_show();
}
/* Flappy Bird in a set look (day or night, a bird), at one of its moments:
 * 0 Get Ready, 1 flying through a gap, 2 paused, 3 Game Over with a gold
 * medal and a new best; 4 a close pass (the bird a few pixels under a lip) */
static void b9_flappy_look(int night, int colour) {
    fl.night = night; fl_scene_make(night); fl_ink(); fl.colour = colour; fl.d_valid = false;
}
static void b9_flappy_at(int which, int night, int colour) {
    fl_loaded = true; fl_best = 23;
    flappy_page(); frame(400);
    b9_flappy_look(night, colour); frame(16);
    if (which == 0) { for (int i = 0; i < 20; i++) frame(16); return; }
    fl_flap();
    for (int i = 0; i < 150; i++) {   /* far enough for pipes on the screen, the gaps kept where it flies */
        for (int k = 0; k < FL_PIPES; k++) if (fl_pipe_sx(&fl.pipe[k], fl_cam_px()) >= FB_SW) fl.pipe[k].gap = 64 + 7 * k;
        if (fl.y > 96) fl_flap();
        frame(16);
    }
    fl.score = 7;
    if (which == 4) {
        /* a pipe's lip right over the bird: its top drawn row three pixels under the lip */
        struct fl_pipe *p = &fl.pipe[0];
        for (int k = 1; k < FL_PIPES; k++) if (fl.pipe[k].x < p->x) p = &fl.pipe[k];
        p->x = (fl_cam_px() + FL_BIRD_X * FB_S) / FB_S - FB_LIP_W / 2 + 4;
        fl.rot = FL_TILT_UP; fl.vy = -40; fl.y = p->gap + 7.0f;
        fl.d_valid = false; fl_draw();
        return;
    }
    if (which == 2) { lv_obj_send_event(fl.pause_btn, LV_EVENT_CLICKED, NULL); frame(16); frame(16); return; }
    if (which == 3) {
        fl.score = 31; fl_die(true);
        for (int i = 0; i < 260; i++) frame(16);
        return;
    }
    fl.vy = -80; fl.rot = FL_TILT_UP; fl.d_valid = false; frame(16);
}
static void b9_flappy(int which) { b9_flappy_at(which, which == 1 || which == 3, which % 3); }
/* a label by its words, whatever their case (Zule writes in lower case) */
static lv_obj_t *flappy_find(lv_obj_t *o, const char *s) {
    if (lv_obj_check_type(o, &lv_label_class) && !strcasecmp(lv_label_get_text(o), s)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) { lv_obj_t *f = flappy_find(lv_obj_get_child(o, i), s); if (f) return f; }
    return NULL;
}
/* SLOPPY_FLAPPY_SHOTS=<dir>: the game's moments, day and night, and its
 * place in the Playroom */
static void flappy_shots(const char *dir) {
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    static const struct { const char *name; int which, night, colour; } m[] = {
        { "get-ready-day", 0, 0, 0 }, { "get-ready-night", 0, 1, 2 }, { "mid-flight-day", 1, 0, 1 }, { "mid-flight-night", 1, 1, 0 },
        { "close-pass-day", 4, 0, 0 }, { "close-pass-night", 4, 1, 1 }, { "paused-day", 2, 0, 2 },
        { "game-over-medal-day", 3, 0, 0 }, { "game-over-medal-night", 3, 1, 2 },
    };
    au_library();
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(m[i].name, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        au_apply(0, 0, 0);
        b9_flappy_at(m[i].which, m[i].night, m[i].colour);
        frame(0);
        snprintf(p, sizeof p, "%s/%s.ppm", dir, m[i].name); shot_now(p);
        au_reset();
    }
    /* its place in the Playroom and on the home lists: the row scrolled into view */
    static const struct { const char *variant; int look, lang; } looks[] = {
        { "dark", 0, 0 }, { "light", 1, 0 }, { "list-light", 5, 0 }, { "zule", 6, 0 }, { "he", 0, 4 }, { "zh", 0, 6 } };
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++) {
        for (int where = 0; where < 2; where++) {
            au_apply(looks[i].look, 0, looks[i].lang);
            if (where) { sl_games_page(); au_settle(); }
            else { appearance_reload((void *)2); au_settle(); }
            lv_obj_t *row = flappy_find(pages[npages - 1], sl_tr("Flappy Bird"));
            if (!row) { au_reset(); continue; }
            lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF); frame(100); frame(100);
            snprintf(p, sizeof p, "%s/%s-%s.ppm", dir, where ? "playroom" : "home", looks[i].variant); shot_now(p);
            au_reset();
        }
        if (i >= 4) {   /* the words in Hebrew and Chinese, as pixel art */
            au_apply(looks[i].look, 0, looks[i].lang);
            b9_flappy_at(3, 0, 0); frame(0);
            snprintf(p, sizeof p, "%s/game-over-%s.ppm", dir, looks[i].variant); shot_now(p);
            au_reset();
            au_apply(looks[i].look, 0, looks[i].lang);
            b9_flappy_at(0, 1, 1); frame(0);
            snprintf(p, sizeof p, "%s/get-ready-%s.ppm", dir, looks[i].variant); shot_now(p);
            au_reset();
        }
    }
    au_library_done();
}
static bool b9_open(const char *s) {
    if (!strcmp(s, "playroom")) sl_games_page();
    else if (!strcmp(s, "snake")) snake_page();
    else if (!strcmp(s, "snake-mid")) { b9_snake_mid(); return true; }
    else if (!strcmp(s, "snake-death")) { b9_snake_death(); return true; }
    else if (!strcmp(s, "arcade")) arcade_page();
    else if (!strcmp(s, "arcade-empty")) { b9_arcade_card(false); arcade_page(); }
    else if (!strcmp(s, "arcade-list")) { b9_arcade_card(true); arcade_page(); }
    else if (!strcmp(s, "arcade-game")) { b9_arcade_game(false); return true; }
    else if (!strcmp(s, "arcade-menu")) { b9_arcade_game(true); return true; }
    else if (!strcmp(s, "arcade-colours")) { b9_arcade_colours(); return true; }
    else if (!strcmp(s, "2048")) { b9_2048(); au_settle(); return true; }
    else if (!strcmp(s, "sudoku")) { b9_sudoku(); au_settle(); return true; }
    else if (!strcmp(s, "points")) { b9_points(); au_settle(); return true; }
    else if (!strcmp(s, "flappy")) { b9_flappy(0); return true; }
    else if (!strcmp(s, "flappy-flying")) { b9_flappy(1); return true; }
    else if (!strcmp(s, "flappy-over")) { b9_flappy(3); return true; }
    else return false;
    au_settle();
    return true;
}
static void b9_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 }, { "he-dark", 0, 0, 4 },
    };
    static const char *const screens[] = {
        "playroom", "snake", "snake-mid", "snake-death",
#ifndef B9_BEFORE
        "arcade-empty", "arcade-list", "arcade-game", "arcade-menu", "2048", "sudoku", "points", "flappy", "flappy-flying", "flappy-over",
#else
        "arcade",
#endif
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++)
    for (unsigned j = 0; j < sizeof screens / sizeof screens[0]; j++) {
        const char *s = screens[j];
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        au_apply(looks[i].look, looks[i].step, looks[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        if (!b9_open(s)) { au_reset(); continue; }
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, looks[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
    }
    au_library_done();
}

/* SLOPPY_B13_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: batch 13's
 * Bluetooth screens in Dark, Light, Zule, Largest and Hebrew. Screens a
 * build does not have yet are skipped (the before shots). */
static void b13_shots(const char *dir, const char *tag) {
    static const struct { const char *variant; int look, step, lang; } looks[] = {
        { "dark", 0, 0, 0 }, { "light", 1, 0, 0 }, { "zule", 6, 0, 0 }, { "largest-dark", 0, 3, 0 }, { "he-dark", 0, 0, 4 },
    };
    static const char *const screens[] = {
        "bluetooth", "bluetooth-connected", "settings-bt-forget", "settings-receive", "settings-receive-connected",
        "bt-device", "bt-rename", "bt-player-name", "bt-sbc", "bt-pair-confirm", "bt-pair-pin", "bt-pair-show",
        "bt-error", "bt-reconnecting", "bt-airpods", "settings-receive-codec",
    };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++)
    for (unsigned j = 0; j < sizeof screens / sizeof screens[0]; j++) {
        const char *s = screens[j];
        if (getenv("SLOPPY_AUDIT_ONLY") && !strstr(s, getenv("SLOPPY_AUDIT_ONLY"))) continue;
        int k = au_find(s);
        if (k < 0) continue;
        au_apply(looks[i].look, looks[i].step, looks[i].lang);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        au_open(k); au_settle();
        pop_hide_now(); nbanner_hide_now();   /* a toast from the pretend stack would cover the page */
        frame(0);
        snprintf(p, sizeof p, "%s/%s-%s-%s.ppm", dir, s, looks[i].variant, tag);
        shot_now(p);
        au_reset(); au_empty(false);
    }
    au_library_done();
}
/* Colour emoji: a sequence is one picture, as a phone draws it, its parts
 * never on their own; at every text size and in a right to left line */
static void em_walk(const lv_font_t *f, const char *s, int *pics, int *drawn, int *w) {
    uint32_t i = 0;
    *pics = *drawn = *w = 0;
    while (s[i]) {   /* as a label asks: a letter's width with the next, then the letter drawn; never a marker (U+200D) */
        uint32_t c = lv_text_encoded_next(s, &i), j = i, nx = s[j] ? lv_text_encoded_next(s, &j) : 0;
        lv_font_glyph_dsc_t g;
        if (lv_text_is_marker(c)) continue;
        if (lv_font_get_glyph_dsc(f, &g, c, nx)) *w += g.adv_w;
        if (lv_font_get_glyph_dsc(f, &g, c, 0) && g.box_w && g.box_h) { (*drawn)++; if (g.format == LV_FONT_GLYPH_FORMAT_IMAGE) (*pics)++; }
    }
}
static void emoji_tests(void) {
    static const struct { const char *s; int pics, drawn; } seqs[] = {
        { "🧛‍♂️", 1, 1 }, { "👍🏽", 1, 1 }, { "🇮🇱", 1, 1 }, { "🇮🇱🇺🇸", 2, 2 }, { "❤️‍🔥", 1, 1 }, { "👩🏿‍💻", 1, 1 },
        { "🏴󠁧󠁢󠁳󠁣󠁴󠁿", 1, 1 }, { "🎧️", 1, 1 }, { "🧛‍🦖", 1, 1 }, { "1️⃣", 0, 1 }, { "#️⃣", 0, 1 }, { "🏽", 0, 0 },
        { "Satanpodz🧛‍♂️", 1, 10 }, { "😀😀", 2, 2 }, { "🙂", 1, 1 }, { "♂", -1, 1 },
    };
    const lv_font_t *fonts[] = { &ss_regular_16, &ss_regular_18, &ss_regular_21, &ss_semibold_23, &ss_regular_26, &ss_semibold_30 };
    for (unsigned k = 0; k < ARRAYLEN(fonts); k++)
        for (unsigned i = 0; i < ARRAYLEN(seqs); i++) {
            int pics, drawn, w;
            em_walk(fonts[k], seqs[i].s, &pics, &drawn, &w);
            if ((seqs[i].pics >= 0 && pics != seqs[i].pics) || drawn != seqs[i].drawn)
                printf("emoji: \"%s\" at %d px: %d pictures, %d drawn\n", seqs[i].s, fonts[k]->line_height, pics, drawn);
            assert((seqs[i].pics < 0 || pics == seqs[i].pics) && drawn == seqs[i].drawn);
            if (seqs[i].pics == 1 && drawn == 1) assert(w >= 16 && w <= 28);   /* one picture's room */
        }
    {   /* ★ ✓ ▶ on their own are drawn as they always were; with U+FE0F, the picture */
        int pics, drawn, w; lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(&sl_sym_20, &g, 0x25b6, 0) && !g.is_placeholder) { em_walk(&ss_regular_21, "▶", &pics, &drawn, &w); assert(!pics && drawn == 1); }
        em_walk(&ss_regular_21, "▶️", &pics, &drawn, &w); assert(pics == 1 && drawn == 1);
    }
    {   /* a right to left line keeps a sequence in its order: 🇮🇱 is not 🇱🇮 */
        static const char *const rtl[] = { "שלום 🇮🇱", "שיר 🧛‍♂️ 👍🏽", "אוזניות 1️⃣ 🇮🇱🇺🇸" };
        static const int want[] = { 1, 2, 2 };
        for (unsigned i = 0; i < ARRAYLEN(rtl); i++) {
            char out[96]; lv_bidi_process(rtl[i], out, LV_BASE_DIR_RTL);
            assert(strstr(out, "🇮🇱") || !strstr(rtl[i], "🇮🇱"));
            assert(!strstr(out, "🇱🇮") && (!strstr(rtl[i], "🧛‍♂️") || strstr(out, "🧛‍♂️")) && (!strstr(rtl[i], "👍🏽") || strstr(out, "👍🏽")));
            int pics, drawn, w; em_walk(&ss_regular_21, out, &pics, &drawn, &w); assert(pics == want[i]);
        }
    }
    /* a Bluetooth name with them, at every text size and in Hebrew: drawn, read by the audit */
    char name0[sizeof sl_bt_demo_devices[0].name]; strlcpy(name0, sl_bt_demo_devices[0].name, sizeof name0);
    static const struct { int step, lang; const char *name, *shot; } looks[] = {
        { 0, 0, "Satanpodz🧛‍♂️ 👍🏽🇮🇱", "emoji-bt-standard" }, { 1, 0, "Satanpodz🧛‍♂️ 👍🏽🇮🇱", "emoji-bt-large" },
        { 2, 0, "Satanpodz🧛‍♂️ 👍🏽🇮🇱", "emoji-bt-larger" }, { 3, 0, "Satanpodz🧛‍♂️ 👍🏽🇮🇱", "emoji-bt-largest" },
        { 0, 4, "אוזניות 🧛‍♂️ 🇮🇱 ❤️‍🔥", "emoji-bt-hebrew" }, { 3, 4, "אוזניות 🧛‍♂️ 🇮🇱 ❤️‍🔥", "emoji-bt-hebrew-largest" },
    };
    int th0 = global_settings.sloppy_theme, hs0 = global_settings.sloppy_home_style, lang0 = global_settings.sloppy_language, step0 = sl_text_step;
    for (unsigned i = 0; i < ARRAYLEN(looks); i++) {
        strlcpy(sl_bt_demo_devices[0].name, looks[i].name, sizeof sl_bt_demo_devices[0].name);
        au_apply(0, looks[i].step, looks[i].lang);
        sl_bt_page(); sl_bt_demo_index = 0; frame(4100); frame(100); frame(400);
        assert(find_label(sl_bt_body, looks[i].name));
        char shot[64]; snprintf(shot, sizeof shot, "%s.ppm", looks[i].shot); shot_now(shot);
        au_check(looks[i].shot);
        if (au_screen_findings) printf("emoji: %s: %d audit findings\n", looks[i].shot, au_screen_findings);
        assert(!au_screen_findings);
        while (npages > 1) { page_pop(); frame(300); }
    }
    strlcpy(sl_bt_demo_devices[0].name, name0, sizeof name0); sl_bt_demo_index = -1;
    global_settings.sloppy_theme = th0; global_settings.sloppy_home_style = hs0; global_settings.sloppy_language = lang0;
    sl_text_step = step0; global_settings.sloppy_text_size = step0; appearance_reload((void *)2); frame(700);
    printf("PASS: colour emoji: sequences (ZWJ, skin tones, flags, tags, keycaps) one picture or their first emoji, never their parts, at %d sizes; kept in order right to left; a Bluetooth name at four text sizes and in Hebrew, the audit clean; %d KB of slots, nothing else held.\n",
           (int)ARRAYLEN(fonts), (int)(XE_SLOTS * sizeof(struct xe_slot) / 1024));
}
/* Never a box: a letter no font has is drawn as its nearest (a dash, a space,
 * a quote, ASCII for fullwidth), as nothing when it is a control or format
 * letter, else as a thin gap */
#if LV_USE_FONT_PLACEHOLDER
#error "a letter without a glyph must never draw a box"
#endif
/* a name listed whole, or cut short with "..." where the row has no room for it */
static lv_obj_t *tofu_find(lv_obj_t *o, const char *s) {
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        size_t n = strlen(t);
        if (!strcmp(t, s) || (n > 3 && !strcmp(t + n - 3, "...") && !strncmp(t, s, n - 3))) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) { lv_obj_t *f = tofu_find(lv_obj_get_child(o, i), s); if (f) return f; }
    return NULL;
}
static void tofu_tests(void) {
    static const uint32_t near[] = { 0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2212, 0xfe58, 0xfe63, 0xff0d,
        0x2018, 0x2019, 0x201b, 0x201c, 0x201f, 0x2032, 0x2033, 0x2035, 0x2039, 0x203a, 0x02bc, 0x2026, 0x2024,
        0x2044, 0x2215, 0x2030, 0xff01, 0xff21, 0xff41, 0xff5e };
    static const uint32_t spaces[] = { 0xa0, 0x2000, 0x2002, 0x2003, 0x2007, 0x2009, 0x200a, 0x202f, 0x205f };
    static const uint32_t none[] = { 0x200b, 0x200c, 0x200e, 0x2060, 0xfeff, 0x7f, 0x85, 0x9f, 0xfe00, 0xe0001, 0xfff9 };
    const lv_font_t *fonts[] = { &ss_regular_16, &ss_regular_18, &ss_regular_21, &ss_semibold_23, &ss_regular_26, &ss_semibold_30 };
    lv_font_glyph_dsc_t g;
    for (unsigned k = 0; k < ARRAYLEN(fonts); k++) {
        const lv_font_t *f = fonts[k];
        for (unsigned i = 0; i < ARRAYLEN(near); i++) {
            bool ok = lv_font_get_glyph_dsc(f, &g, near[i], 0);
            if (!ok || g.is_placeholder || !g.box_w) printf("tofu: U+%04X at %d px has no near letter\n", (unsigned)near[i], f->line_height);
            assert(ok && !g.is_placeholder && g.box_w && g.adv_w);
        }
        for (unsigned i = 0; i < ARRAYLEN(spaces); i++) {
            assert(lv_font_get_glyph_width(f, spaces[i], 0) == lv_font_get_glyph_width(f, ' ', 0));
        }
        for (unsigned i = 0; i < ARRAYLEN(none); i++) {
            lv_font_get_glyph_dsc(f, &g, none[i], 0);
            assert(lv_font_get_glyph_width(f, none[i], 0) == 0 && (!g.box_w || !g.box_h));
        }
        /* none at all (an Egyptian hieroglyph): a thin gap, nothing drawn */
        assert(!lv_font_get_glyph_dsc(f, &g, 0x13000, 0) && !g.box_w && g.adv_w > 0 && g.adv_w <= f->line_height / 4);
    }
    /* names with them, at every text size and in Hebrew: drawn, read by the audit */
    static const char *const names[3][2] = {   /* on devices 0 and 2: 1 keeps a name an earlier test gave it */
        { "Rock‐n‑Roll ‒ Live ― Take−2 ﹘﹣－", "A B C D E F G H" },
        { "‛Rock′ ‟Roll″ ‹Mix› 1⁄2 ‰", "Zero​Width⁠Joined﻿Here‎\xc2\x85!" },
        { "Ａｂｃ １２３！ （Ｍｉｘ）", "‘One’ … “Two” «Three» • ·" } };
    char saved[3][sizeof sl_bt_demo_devices[0].name];
    for (int d = 0; d < 3; d++) strlcpy(saved[d], sl_bt_demo_devices[d].name, sizeof saved[d]);
    bool paired2 = sl_bt_demo_devices[2].paired; sl_bt_demo_devices[2].paired = true;   /* all three listed without a scan */
    static const struct { int step, lang; const char *shot; } looks[] = {
        { 0, 0, "tofu-bt-standard" }, { 1, 0, "tofu-bt-large" }, { 2, 0, "tofu-bt-larger" }, { 3, 0, "tofu-bt-largest" },
        { 0, 4, "tofu-bt-hebrew" }, { 3, 4, "tofu-bt-hebrew-largest" } };
    int th0 = global_settings.sloppy_theme, hs0 = global_settings.sloppy_home_style, lang0 = global_settings.sloppy_language, step0 = sl_text_step;
    for (unsigned i = 0; i < ARRAYLEN(looks); i++)
        for (int n = 0; n < 3; n++) {
            for (int d = 0; d < 2; d++) strlcpy(sl_bt_demo_devices[d * 2].name, names[n][d], sizeof sl_bt_demo_devices[d * 2].name);
            au_apply(0, looks[i].step, looks[i].lang);
            sl_bt_page(); sl_bt_demo_index = 0; frame(4100); frame(100); frame(400);
            for (int d = 0; d < 2; d++) assert(tofu_find(sl_bt_body, names[n][d]));
            char shot[64]; snprintf(shot, sizeof shot, "%s-%d", looks[i].shot, n + 1);
            char ppm[72]; snprintf(ppm, sizeof ppm, "%s.ppm", shot); shot_now(ppm);
            au_check(shot);
            if (au_screen_findings) printf("tofu: %s: %d audit findings\n", shot, au_screen_findings);
            assert(!au_screen_findings);
            while (npages > 1) { page_pop(); frame(300); }
        }
    for (int d = 0; d < 3; d++) strlcpy(sl_bt_demo_devices[d].name, saved[d], sizeof saved[d]);
    sl_bt_demo_devices[2].paired = paired2; sl_bt_demo_index = -1;
    global_settings.sloppy_theme = th0; global_settings.sloppy_home_style = hs0; global_settings.sloppy_language = lang0;
    sl_text_step = step0; global_settings.sloppy_text_size = step0; appearance_reload((void *)2); frame(700);
    printf("PASS: never a box: %d dashes, quotes and fullwidth letters drawn as their nearest, %d spaces as a space, %d control and format letters as nothing, a letter no font has as a thin gap, at %d sizes; names with them at four text sizes and in Hebrew, the audit clean.\n",
           (int)ARRAYLEN(near), (int)ARRAYLEN(spaces), (int)ARRAYLEN(none), (int)ARRAYLEN(fonts));
}
