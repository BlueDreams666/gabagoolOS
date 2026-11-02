/* E-books: the Books shelf (Music > Books) and the reader. The book is
 * read on sloppy_epub.c's worker, a chapter at a time; here its
 * paragraphs are cut into pages, a few lines a frame so no frame waits,
 * and only the page on the screen is made of labels. A page turns by a
 * tap at either edge, a swipe, or the side keys; a Hebrew or Arabic book
 * turns from the right. Three papers (light, sepia, dark), four text
 * sizes, the contents to jump by, and where each book was left, kept on
 * the card. Included by sloppy.c after the podcasts (their P: drive shows
 * the covers). */
#include "src/misc/lv_text_private.h"
#include "src/misc/lv_bidi_private.h"

/* ---------------------------------------------------------------- papers */
enum { RD_LIGHT, RD_SEPIA, RD_DARK };
static const char *const rd_theme_names[3] = { "Light", "Sepia", "Dark" };
static uint32_t rd_paper(int t) { return t == RD_LIGHT ? 0xfbfaf7 : t == RD_SEPIA ? 0xf3e8d0 : sl_theme() == 2 ? 0x000000 : 0x131518; }
static uint32_t rd_ink(int t) { return t == RD_LIGHT ? 0x1f2328 : t == RD_SEPIA ? 0x43331f : 0xdedad2; }
static uint32_t rd_sub(int t) { return t == RD_LIGHT ? 0x5b626b : t == RD_SEPIA ? 0x6b5639 : 0x9c9890; }

/* ---------------------------------------------------------------- state */
#define RD_MX (SL_INSET + SL_SP_S)            /* paper edge to words */
#define RD_TW (W - 2 * RD_MX)
/* the progress line and where in the book, under it at any text size */
#define RD_FOOT (SL_SP_S + 3 + SL_SP_S + sl_line_h(&ss_regular_16) + SL_SP_XS)
#define RD_SWIPE 40
struct rd_pos { int para; uint32_t off; };
static struct {
    lv_obj_t *page, *body, *stage, *leaf[2], *foot, *track, *fill, *where, *count, *state;
    lv_timer_t *tick;
    int cur;                                  /* the leaf showing */
    struct sl_epub *book;
    struct sl_epub_chap *chap;
    unsigned ticket;
    int spine;                                /* the part being read */
    struct rd_pos *pg;                        /* where each page of it starts */
    int npg, cap_pg, at;                      /* pages known, the one shown (-1 none yet) */
    bool laid;                                /* every page of it known */
    struct rd_pos lay;                        /* where the next page starts */
    struct rd_pos want;                       /* to show: the page holding this */
    bool want_last;                           /* or its last page (coming back a part) */
    uint32_t want_frag;                       /* or where the contents pointed */
    int theme, size;
    int ph;                                   /* the page's height */
    bool mini_room;                           /* laid out above the mini player */
    bool rtl;
    bool anyway;                              /* shown though its script has no letters here */
    bool asking;                              /* the note on its script is up: nothing laid out under it */
    bool turning;
    bool loading;                             /* the book or a part of it still coming */
    int px, py;                               /* where a touch began */
    bool pressed;
    uint32_t saved_ms;
    char path[300];
    lv_obj_t *sw[3], *size_l, *minus, *plus;  /* the sheet's */
} rd = { .at = -1 };
static char rd_buf[6144];                     /* one fragment of a page's words */

static const lv_font_t *rd_font(int kind)
{
    int s = rd.size < 0 ? 0 : rd.size > SL_TEXT_STEPS - 1 ? SL_TEXT_STEPS - 1 : rd.size;
    switch (kind)
    {
        case SL_EP_H1: return sl_fonts[s][5];
        case SL_EP_H2: return sl_fonts[s][4];
        case SL_EP_H3: return sl_fonts[s][3];
        case -1: return sl_fonts[s][0];       /* a note of ours */
        default: return sl_fonts[s][1];
    }
}
static int rd_space(const lv_font_t *f, int kind) { return lv_font_get_line_height(f) / (kind >= SL_EP_H1 && kind <= SL_EP_H3 ? 6 : 3); }
static int rd_line(const lv_font_t *f, int kind) { return lv_font_get_line_height(f) + rd_space(f, kind); }
static int rd_body_line(void) { return rd_line(rd_font(SL_EP_P), SL_EP_P); }

/* the chapter's paragraphs, and ours after them: a note where it was cut, or empty, or missing */
static int rd_np(void)
{
    if (!rd.chap) return 0;
    return rd.chap->np + (rd.chap->cut || rd.chap->missing || !rd.chap->np ? 1 : 0);
}
static const char *rd_para(int i, int *kind, int *flags)
{
    const struct sl_epub_chap *c = rd.chap;
    if (i < c->np) { *kind = c->p[i].kind; *flags = c->p[i].flags; return c->text + c->p[i].off; }
    *kind = -1;
    *flags = 0;
    return sl_tr(c->missing ? "This part of the book is missing or damaged." :
                 c->cut ? "The rest of this chapter is too long to show here." : "This part of the book has no words.");
}
static int rd_cmp(struct rd_pos a, struct rd_pos b) { return a.para != b.para ? (a.para < b.para ? -1 : 1) : a.off < b.off ? -1 : a.off > b.off; }

/* ---------------------------------------------------------------- a page */
/* One fragment of a paragraph's lines: a label of exactly the lines laid
 * out, so the label never wraps them otherwise */
static void rd_frag(lv_obj_t *host, const char *s, const lv_font_t *f, int kind, int flags, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(host);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(l, s);                  /* the book's words: never translated */
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_line_space(l, rd_space(f, kind), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kind == -1 || (flags & SL_EPF_EM) ? rd_sub(rd.theme) : rd_ink(rd.theme)), 0);
    if (kind == -1) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    sl_label_deep(l);
}
/* From at, as many lines as the page holds: their labels into host (NULL:
 * only measured). The same steps both ways, so a page drawn is the page
 * counted. Returns where the next page starts. */
static struct rd_pos rd_fill(struct rd_pos at, lv_obj_t *host)
{
    int y = 0, np = rd_np(), bl = rd_body_line();
    bool first = true;
    int pi = at.para;
    uint32_t off = at.off;
    while (pi < np)
    {
        int kind, flags;
        const char *s = rd_para(pi, &kind, &flags);
        bool head = kind >= SL_EP_H1 && kind <= SL_EP_H3;
        int pf = pi > 0 && pi - 1 < rd.chap->np ? rd.chap->p[pi - 1].flags : 0;
        int gap = first ? 0 : head ? bl : (flags & SL_EPF_LI) && (pf & SL_EPF_LI) ? bl / 6 : bl / 2;   /* a list's items stay together */
        if (kind == SL_EP_PIC || kind == SL_EP_RULE)
        {
            if (!first && y + gap + bl > rd.ph) break;
            y += gap;
            if (host && kind == SL_EP_RULE)
            {
                lv_obj_t *r = box(host);
                lv_obj_set_size(r, 64, 1);
                lv_obj_set_pos(r, (RD_TW - 64) / 2, y + bl / 2);
                lv_obj_set_style_bg_color(r, lv_color_hex(rd_sub(rd.theme)), 0);
                lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
            }
            else if (host)
            {
                /* a picture: a quiet line where it was, with its words when it has them */
                const lv_font_t *f = rd_font(-1);
                lv_obj_t *row = box(host);
                lv_obj_set_size(row, RD_TW, bl);
                lv_obj_set_pos(row, 0, y);
                lv_obj_t *ic = text(row, LV_SYMBOL_IMAGE, &sl_sym_18, rd_sub(rd.theme));
                lv_obj_set_style_text_color(ic, lv_color_hex(rd_sub(rd.theme)), 0);
                lv_obj_align(ic, rd.rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
                lv_obj_t *l = lv_label_create(row);
                lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
                lv_label_set_text(l, s[0] ? s : sl_tr("Picture"));
                lv_obj_set_style_text_font(l, f, 0);
                lv_obj_set_style_text_color(l, lv_color_hex(rd_sub(rd.theme)), 0);
                lv_obj_set_size(l, RD_TW - 30, lv_font_get_line_height(f));
                lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(l, rd.rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_align(l, rd.rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, rd.rtl ? -30 : 30, 0);
                sl_label_deep(l);
            }
            y += bl;
            pi++; off = 0; first = false;
            continue;
        }
        const lv_font_t *f = rd_font(kind);
        int lh = rd_line(f, kind);
        int indent = (flags & SL_EPF_QUOTE) ? SL_INSET : 0, w = RD_TW - indent;
        if (!first && y + gap + lh + (head ? bl : 0) > rd.ph) break;   /* a heading keeps a line under it */
        y += gap;
        bool rtl_p = host && lv_bidi_detect_base_dir(s) == LV_BASE_DIR_RTL;
        int x = rtl_p ? 0 : indent, y0 = y;
        size_t bn = 0;
        uint32_t len = (uint32_t)strlen(s);
        while (off < len)
        {
            if (y + lh > rd.ph && y > 0)
            {
                if (host && bn) { rd_buf[bn] = '\0'; rd_frag(host, rd_buf, f, kind, flags, x, y0, w); }
                return (struct rd_pos){ pi, off };
            }
            int32_t used;
            uint32_t n = lv_text_get_next_line(s + off, f, 0, w, &used, LV_TEXT_FLAG_NONE);
            if (!n) n = 1;
            if (off + n > len) n = len - off;
            if (host)
            {
                uint32_t k = n;
                while (k && (s[off + k - 1] == ' ' || s[off + k - 1] == '\n')) k--;
                /* one line longer than the buffer (zero-width letters never wrap): cut at a letter */
                if (k > sizeof rd_buf - 2)
                {
                    k = sizeof rd_buf - 2;
                    while (k && ((unsigned char)s[off + k] & 0xc0) == 0x80) k--;
                }
                if (bn + k + 2 >= sizeof rd_buf)
                {
                    rd_buf[bn] = '\0';
                    rd_frag(host, rd_buf, f, kind, flags, x, y0, w);
                    bn = 0;
                    y0 = y;
                }
                if (bn) rd_buf[bn++] = '\n';
                memcpy(rd_buf + bn, s + off, k);
                bn += k;
            }
            off += n;
            y += lh;
            first = false;
        }
        if (host && bn) { rd_buf[bn] = '\0'; rd_frag(host, rd_buf, f, kind, flags, x, y0, w); }
        pi++; off = 0; first = false;
    }
    return (struct rd_pos){ pi, pi < np ? off : 0 };
}

/* ---------------------------------------------------------------- where in the book */
static uint16_t rd_permille(void)
{
    if (!rd.book || !rd.book->total || !rd.chap) return 0;
    uint64_t at = rd.book->spine_at[rd.spine];
    uint32_t u = rd.book->ent[rd.book->spine[rd.spine]].usize;
    if (u > SL_EPUB_XHTML_CAP || !u) u = u ? SL_EPUB_XHTML_CAP : 1;
    if (rd.at >= 0 && rd.chap->n && rd.at < rd.npg)
    {
        struct rd_pos p = rd.pg[rd.at];
        uint64_t b = p.para < rd.chap->np ? rd.chap->p[p.para].off + p.off : rd.chap->n;
        if (rd.laid && rd.at == rd.npg - 1) b = rd.chap->n;   /* the part's last page: all of it read */
        at += b * u / rd.chap->n;
    }
    uint64_t pm = at * 1000 / rd.book->total;
    return (uint16_t)(pm > 1000 ? 1000 : pm);
}
static void rd_remember(void)
{
    if (!rd.book || !rd.chap || rd.at < 0 || rd.at >= rd.npg) return;
    struct sl_epub_pos p = { rd.spine, rd.pg[rd.at].para, rd.pg[rd.at].off, rd_permille(), (uint32_t)time(NULL) };
    sl_epub_pos_set(rd.path, &p);
}
/* the contents line the page shown is under: a line pointing inside this
 * part counts once the page has reached its paragraph */
static int rd_toc_here(void)
{
    const struct sl_epub *b = rd.book;
    int best = -1;
    int para = rd.chap && rd.at >= 0 && rd.at < rd.npg ? rd.pg[rd.at].para : 0;
    for (int k = 0; b && k < b->ntoc; k++)
    {
        const struct sl_epub_toc *t = &b->toc[k];
        if (t->spine > rd.spine) continue;
        if (t->spine == rd.spine && t->frag && rd.chap && sl_epub_anchor(rd.chap, t->frag) > para) continue;
        if (best < 0 || t->spine >= b->toc[best].spine) best = k;
    }
    return best;
}
static void rd_foot_update(void)
{
    if (!rd.foot) return;
    int pm = rd_permille();
    lv_obj_set_width(rd.fill, LV_MAX(6, RD_TW * pm / 1000));
    int k = rd_toc_here();
    char b[200];
    if (k >= 0) snprintf(b, sizeof b, "%s", sl_epub_toc_title(rd.book, k));
    else snprintf(b, sizeof b, "%s", rd.book ? rd.book->title : "");
    if (sl_zune()) { char low[200]; sl_lower(b, low, sizeof low); label_if_changed(rd.where, low); }
    else label_if_changed(rd.where, b);
    if (rd.laid) snprintf(b, sizeof b, sl_tr("%d of %d"), rd.at + 1, rd.npg);
    else snprintf(b, sizeof b, "%d", rd.at + 1);
    label_if_changed(rd.count, b);
    /* the part's name has the room the count leaves it */
    int cw = lv_text_get_width(b, (uint32_t)strlen(b), &ss_regular_16, 0) + 2;
    lv_obj_set_width(rd.count, cw);
    lv_obj_set_width(rd.where, LV_MAX(40, RD_TW - cw - SL_INSET));
}

/* ---------------------------------------------------------------- showing a page */
static void rd_leaf_done(lv_anim_t *a)
{
    lv_obj_t *o = a->var;
    if (o != rd.leaf[rd.cur]) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); lv_obj_clean(o); }
    rd.turning = false;
}
/* page k into the other leaf, slid in from dir (1 the next, -1 the one before, 0 at once) */
static void rd_show(int k, int dir)
{
    if (!rd.stage || k < 0 || k >= rd.npg) return;
    if (rd.turning)
    {
        /* a turn still sliding: it lands now */
        for (int i = 0; i < 2; i++) { lv_anim_delete(rd.leaf[i], NULL); lv_obj_set_x(rd.leaf[i], 0); }
        lv_obj_add_flag(rd.leaf[!rd.cur], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(rd.leaf[!rd.cur]);
        rd.turning = false;
    }
    if (rd.state) { lv_obj_delete(rd.state); rd.state = NULL; }
    rd.loading = false;
    lv_obj_t *old = rd.leaf[rd.cur], *nw = rd.leaf[!rd.cur];
    lv_obj_clean(nw);
    rd_fill(rd.pg[k], nw);
    lv_obj_remove_flag(nw, LV_OBJ_FLAG_HIDDEN);
    rd.cur = !rd.cur;
    rd.at = k;
    /* a book that turns from the right slides the other way */
    int sx = rd.rtl ? -dir : dir;
    if (dir && !lv_obj_has_flag(old, LV_OBJ_FLAG_HIDDEN))
    {
        rd.turning = true;
        anim_x(nw, sx * W, 0, SL_T_PAGE, NULL);
        anim_x(old, 0, -sx * W, SL_T_PAGE, rd_leaf_done);
    }
    else
    {
        lv_obj_set_x(nw, 0);
        lv_obj_add_flag(old, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(old);
    }
    rd_foot_update();
    rd_remember();
}
/* the state in the stage's middle: what is wrong, or that it is coming */
static void rd_state(int icon, const char *what, const char *how)
{
    if (!rd.stage) return;
    if (rd.state) lv_obj_delete(rd.state);
    for (int i = 0; i < 2; i++) { lv_obj_add_flag(rd.leaf[i], LV_OBJ_FLAG_HIDDEN); lv_obj_clean(rd.leaf[i]); }
    rd.loading = false;
    rd.state = sl_empty(rd.stage, icon, 96, what, how, 56);   /* as the shelf's own empty state sits */
    lv_obj_set_width(rd.state, W);
    if (rd.foot) lv_obj_add_flag(rd.foot, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------- the paper */
static void rd_ink_tree(lv_obj_t *o, uint32_t ink)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, lv_color_hex(ink), 0);
        else if (lv_obj_check_type(c, &lv_line_class)) lv_obj_set_style_line_color(c, lv_color_hex(ink), 0);
        else rd_ink_tree(c, ink);
    }
}
static void rd_paint(void)
{
    if (!rd.page) return;
    uint32_t paper = rd_paper(rd.theme), ink = rd_ink(rd.theme);
    lv_obj_set_style_bg_color(rd.page, lv_color_hex(paper), 0);
    lv_obj_set_style_bg_opa(rd.page, LV_OPA_COVER, 0);
    rd_ink_tree(lv_obj_get_child(rd.page, 0), ink);
    if (rd.foot)
    {
        lv_obj_set_style_bg_color(rd.track, lv_color_hex(ink), 0);
        lv_obj_set_style_text_color(rd.where, lv_color_hex(rd_sub(rd.theme)), 0);
        lv_obj_set_style_text_color(rd.count, lv_color_hex(rd_sub(rd.theme)), 0);
    }
    if (npages && pages[npages - 1] == rd.page) statusbar_retheme();
}
static void rd_unpaint(void)
{
    /* an error or loading state: the page as every other page is */
    if (!rd.page) return;
    lv_obj_set_style_bg_color(rd.page, lv_color_hex(C_BG), 0);
    rd_ink_tree(lv_obj_get_child(rd.page, 0), C_TEXT);
    if (npages && pages[npages - 1] == rd.page) statusbar_retheme();
}

/* ---------------------------------------------------------------- laying a part out */
static bool rd_mini_on(void) { return (audio_status() & AUDIO_STATUS_PLAY) != 0; }
static void rd_geometry(void)
{
    rd.mini_room = rd_mini_on();
    int bh = H - SB_H - SL_HDR_H;
    int reserve = rd.mini_room ? 82 + SL_FLOAT + SL_SP_S : SL_SP_S;
    int foot_y = bh - reserve - RD_FOOT;
    rd.ph = foot_y - SL_SP_S - SL_SP_XS;
    for (int i = 0; i < 2; i++) lv_obj_set_size(rd.leaf[i], W, rd.ph + SL_SP_XS);
    lv_obj_set_y(rd.foot, foot_y);
}
static void rd_lay_reset(void)
{
    rd.npg = 0;
    rd.laid = false;
    rd.lay = (struct rd_pos){ 0, 0 };
    rd.at = -1;
}
static bool rd_pg_push(struct rd_pos p)
{
    if (rd.npg >= rd.cap_pg)
    {
        int nc = rd.cap_pg ? rd.cap_pg * 2 : 64;
        struct rd_pos *np = realloc(rd.pg, (size_t)nc * sizeof *np);
        if (!np) return false;
        rd.pg = np;
        rd.cap_pg = nc;
    }
    rd.pg[rd.npg++] = p;
    return true;
}
/* the page holding want, when it is known yet (-1 not yet) */
static int rd_find(struct rd_pos want)
{
    int k = -1;
    for (int i = 0; i < rd.npg && rd_cmp(rd.pg[i], want) <= 0; i++) k = i;
    if (k < 0) return rd.npg ? 0 : -1;
    if (k + 1 < rd.npg || rd.laid || rd_cmp(rd.lay, want) > 0) return k;
    return -1;
}
/* is the page wanted known yet: shown, then */
static void rd_try_show(void)
{
    if (rd.at >= 0 || !rd.npg) return;
    int k = rd.want_last ? (rd.laid ? rd.npg - 1 : -1) : rd_find(rd.want);
    if (k >= 0) rd_show(k, 0);
}
/* a slice of the laying out: pages for at most a few ms */
static void rd_lay_step(void)
{
    if (!rd.chap || rd.laid || rd.asking) return;
    uint32_t until = sl_ms() + 6;
    int np = rd_np();
    do
    {
        if (rd.lay.para >= np) { rd.laid = true; break; }
        struct rd_pos next = rd_fill(rd.lay, NULL);
        if (!rd_pg_push(rd.lay)) { rd.laid = true; break; }
        if (rd_cmp(next, rd.lay) <= 0) next = (struct rd_pos){ rd.lay.para + 1, 0 };   /* never stuck on one */
        rd.lay = next;
        if (rd.lay.para >= np) rd.laid = true;
    } while (!rd.laid && (int32_t)(sl_ms() - until) < 0);
    rd_try_show();
    if (rd.laid) rd_foot_update();
}
/* again from the page shown (a new text size, the mini player came or went) */
static void rd_relayout(void)
{
    if (!rd.chap) return;
    if (rd.at >= 0 && rd.at < rd.npg) { rd.want = rd.pg[rd.at]; rd.want_last = false; }
    rd_geometry();
    rd_lay_reset();
    rd_lay_step();
}
/* the letters this book's words need, and whether the fonts have them */
static bool rd_script_missing(void)
{
    const struct sl_epub_chap *c = rd.chap;
    if (!c || !c->n || rd.anyway) return false;
    const lv_font_t *f = rd_font(SL_EP_P);
    int all = 0, miss = 0;
    uint32_t i = 0;
    while (i < c->n && all < 1500)
    {
        uint32_t cp = lv_text_encoded_next(c->text, &i);
        if (!cp) continue;
        if (cp < 0x80) { if (cp > ' ') all++; continue; }
        all++;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(f, &g, cp, 0) || g.is_placeholder) miss++;
    }
    return all >= 20 && miss * 5 > all;
}
static void rd_anyway_cb(lv_event_t *e) { (void)e; rd.anyway = true; rd.asking = false; rd_paint(); lv_obj_remove_flag(rd.foot, LV_OBJ_FLAG_HIDDEN); rd_relayout(); }
static void rd_chapter_came(struct sl_epub_chap *c)
{
    sl_epub_chap_free(rd.chap);
    rd.chap = c;
    rd.spine = c->spine;
    if (rd.want_frag)
    {
        rd.want = (struct rd_pos){ sl_epub_anchor(c, rd.want_frag), 0 };
        rd.want_frag = 0;
    }
    if (rd.want.para >= rd_np()) rd.want = (struct rd_pos){ 0, 0 };
    if (rd_script_missing())
    {
        lv_obj_t *s;
        rd_unpaint();
        rd_state(IC_LIBRARY, "This book is in a script gabagoolOS can't draw yet.",
                 "Its letters aren't in the player's fonts. Books in Latin, Cyrillic, Hebrew, Arabic, Thai, Chinese, Japanese and Korean letters read here.");
        s = rd.state;
        rd.asking = true;
        sl_empty_action(s, LV_SYMBOL_EYE_OPEN "  Show it anyway", rd_anyway_cb, NULL);
        return;
    }
    rd_paint();
    lv_obj_remove_flag(rd.foot, LV_OBJ_FLAG_HIDDEN);
    rd_geometry();
    rd_lay_reset();
    rd_lay_step();
}
/* part sp, at want (or its end) */
static void rd_goto(int sp, struct rd_pos want, bool last, uint32_t frag)
{
    if (!rd.book || sp < 0 || sp >= rd.book->nspine) return;
    rd.want = want;
    rd.want_last = last;
    rd.want_frag = frag;
    if (rd.chap && rd.chap->spine == sp)
    {
        if (frag) { rd.want = (struct rd_pos){ sl_epub_anchor(rd.chap, frag), 0 }; rd.want_frag = 0; }
        int k = last ? (rd.laid ? rd.npg - 1 : -1) : rd_find(rd.want);
        if (k >= 0) { rd_show(k, k > rd.at ? 1 : k < rd.at ? -1 : 0); return; }
        rd_lay_reset();
        rd_lay_step();
        return;
    }
    /* one part in memory: the one going is let go first */
    sl_epub_chap_free(rd.chap);
    rd.chap = NULL;
    rd_lay_reset();
    rd.spine = sp;
    rd.ticket = sl_epub_chapter_start(rd.book, sp);
}

/* ---------------------------------------------------------------- turning */
static void rd_turn(int dir)
{
    if (!rd.chap || rd.at < 0) return;
    if (dir > 0)
    {
        if (rd.at + 1 < rd.npg) { rd_show(rd.at + 1, 1); return; }
        if (!rd.laid) { rd_lay_step(); if (rd.at + 1 < rd.npg) rd_show(rd.at + 1, 1); return; }
        if (rd.spine + 1 < rd.book->nspine) { rd_goto(rd.spine + 1, (struct rd_pos){ 0, 0 }, false, 0); return; }
        toast("That's the end of the book");
        return;
    }
    if (rd.at > 0) { rd_show(rd.at - 1, -1); return; }
    if (rd.spine > 0) rd_goto(rd.spine - 1, (struct rd_pos){ 0, 0 }, true, 0);
}
static void rd_stage_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || on_covered_page(rd.stage)) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    if (code == LV_EVENT_PRESSED) { rd.px = pt.x; rd.py = pt.y; rd.pressed = true; return; }
    if (code == LV_EVENT_PRESS_LOST) { rd.pressed = false; return; }
    if (code != LV_EVENT_RELEASED || !rd.pressed) return;
    rd.pressed = false;
    int dx = pt.x - rd.px, dy = pt.y - rd.py;
    /* a book from the right: its next page comes from the left, swiped rightwards */
    if (abs(dx) > RD_SWIPE && abs(dx) > abs(dy)) { rd_turn((dx < 0) != rd.rtl ? 1 : -1); return; }
    if (abs(dx) > 16 || abs(dy) > 16) return;   /* a drag that went nowhere */
    /* a tap: the third at the back edge turns back, the rest on */
    bool back = rd.rtl ? pt.x >= W * 2 / 3 : pt.x < W / 3;
    rd_turn(back ? -1 : 1);
}
/* the side keys: down the next page, up the one before */
static bool rd_key(int b, bool rel, bool repeat)
{
    (void)repeat;
    if (!rd.page || !npages || pages[npages - 1] != rd.page || (b != BUTTON_UP && b != BUTTON_DOWN)) return false;
    if (acts_open_now) return false;           /* the sheet over it: the volume, as anywhere */
    if (!rel) rd_turn(b == BUTTON_DOWN ? 1 : -1);
    return true;
}

/* ---------------------------------------------------------------- the worker's answers */
static void rd_fail(int err)
{
    static const char *const why[] = {
        [SL_EPUB_ERR_OPEN] = "The file could not be read.",
        [SL_EPUB_ERR_ZIP] = "It isn't a whole EPUB file. It may have been cut short while it was copied.",
        [SL_EPUB_ERR_OPF] = "Its list of contents is missing or damaged.",
        [SL_EPUB_ERR_EMPTY] = "It has no chapters to show.",
        [SL_EPUB_ERR_MEM] = "There isn't enough memory free to open it.",
        [SL_EPUB_ERR_DRM] = "It is locked with DRM, so only the shop's own app can open it.",
        [SL_EPUB_ERR_HELD] = "The card is in use by the computer.",
    };
    rd_unpaint();
    rd_state(IC_LIBRARY, "This book can't be opened.", err > 0 && err <= SL_EPUB_ERR_HELD ? why[err] : why[SL_EPUB_ERR_OPEN]);
    lv_obj_t *n = text(rd.state, rd.path, &ss_regular_16, C_DIM);   /* which file, for finding it */
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(n, SL_INSET, 0);
}
/* every letter of s in f (a title in a script we cannot draw keeps the file's name) */
static bool rd_drawable(const char *s, const lv_font_t *f)
{
    uint32_t i = 0, n = (uint32_t)strlen(s);
    while (i < n)
    {
        uint32_t cp = lv_text_encoded_next(s, &i);
        lv_font_glyph_dsc_t g;
        if (cp > ' ' && (!lv_font_get_glyph_dsc(f, &g, cp, 0) || g.is_placeholder)) return false;
    }
    return true;
}
/* The page's title: the book's own, once it has been read. Zule sets it
 * as its pages set a title the big face cannot: one line, clear of the
 * buttons (a book's long name ran under them at 104 px). */
static void rd_title(const char *s)
{
    lv_obj_t *hdr = lv_obj_get_child(rd.page, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++)
    {
        lv_obj_t *t = lv_obj_get_child(hdr, (int32_t)i);
        if (!lv_obj_check_type(t, &lv_label_class)) continue;
        char low[200], was[200];
        if (!s) { snprintf(was, sizeof was, "%s", lv_label_get_text(t)); s = was; }
        const char *w = sl_zune() ? sl_lower(s, low, sizeof low) : s;
        if (sl_zune())
        {
            lv_obj_set_style_text_font(t, &ss_semibold_30, 0);
            lv_obj_set_size(t, W - 58 - 2 * 58 - SL_SP_S, lv_font_get_line_height(&ss_semibold_30));   /* clear of the two buttons */
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, 58, 0);
        }
        if (rd_drawable(w, lv_obj_get_style_text_font(t, LV_PART_MAIN))) lv_label_set_text(t, w);
        break;
    }
}
static void rd_tick_cb(lv_timer_t *t)
{
    (void)t;
    unsigned ticket;
    struct sl_epub *b;
    struct sl_epub_chap *c;
    int err;
    int kind = sl_epub_poll(&ticket, &b, &c, &err);
    if (kind && ticket != rd.ticket) { sl_epub_free(b); sl_epub_chap_free(c); kind = 0; }
    if (kind == SL_EPUB_J_OPEN)
    {
        if (!b) { rd_fail(err); return; }
        rd.book = b;
        rd.rtl = b->rtl;
        rd_title(b->title);
        struct sl_epub_pos p;
        int sp = 0;
        if (sl_epub_pos_get(rd.path, &p) && p.spine >= 0 && p.spine < b->nspine) { sp = p.spine; rd.want = (struct rd_pos){ p.para, p.off }; }
        rd.spine = sp;
        rd.ticket = sl_epub_chapter_start(b, sp);
    }
    else if (kind == SL_EPUB_J_CHAP) { if (c) rd_chapter_came(c); else { rd_fail(SL_EPUB_ERR_MEM); return; } }
    if (rd.chap && !rd.laid) rd_lay_step();
    if (rd.chap && rd.at >= 0 && rd_mini_on() != rd.mini_room && !rd.turning) rd_relayout();
    /* where it is, on the card now and then (and when it closes) */
    if (sl_ms() - rd.saved_ms > 5000) { rd.saved_ms = sl_ms(); sl_epub_save(false); }
}
static void bs_refresh(void);
static void rd_close(void)
{
    rd_remember();
    sl_epub_save(false);
    sl_epub_drop();
    if (rd.tick) lv_timer_delete(rd.tick);
    sl_epub_chap_free(rd.chap);
    sl_epub_release(rd.book);
    free(rd.pg);
    memset(&rd, 0, sizeof rd);
    rd.at = -1;
    bs_refresh();                            /* the shelf under it: where the book is now */
}
static void rd_page_deleted(lv_event_t *e) { if (lv_event_get_target(e) == rd.page) rd_close(); }
/* before USB mode and power off */
static void rd_flush(void) { if (rd.page) rd_remember(); sl_epub_save(true); }

/* ---------------------------------------------------------------- the sheet */
static void rd_contents_page(void);
static void rd_sheet_paint(void)
{
    for (int i = 0; i < 3; i++)
    {
        if (!rd.sw[i]) continue;
        lv_obj_set_style_border_color(rd.sw[i], lv_color_hex(i == rd.theme ? C_ACCENT : C_SUB), 0);
        lv_obj_set_style_border_opa(rd.sw[i], i == rd.theme ? LV_OPA_COVER : 60, 0);
        lv_obj_set_style_border_width(rd.sw[i], i == rd.theme ? 3 : 1, 0);
    }
    if (rd.size_l) lv_label_set_text(rd.size_l, sl_case(text_size_names[rd.size]));
    if (rd.minus) lv_obj_set_style_opa(rd.minus, rd.size > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    if (rd.plus) lv_obj_set_style_opa(rd.plus, rd.size < SL_TEXT_STEPS - 1 ? LV_OPA_COVER : LV_OPA_40, 0);
}
static void rd_theme_cb(lv_event_t *e)
{
    rd.theme = (int)(intptr_t)lv_event_get_user_data(e);
    sl_epub_prefs_set(rd.theme, rd.size);
    rd_sheet_paint();
    if (rd.chap && !rd.state) { rd_paint(); if (rd.at >= 0) { int k = rd.at; rd.at = -1; rd_show(k, 0); } }
}
static void rd_size_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int s = rd.size + d;
    if (s < 0 || s >= SL_TEXT_STEPS) return;
    rd.size = s;
    sl_epub_prefs_set(rd.theme, rd.size);
    rd_sheet_paint();
    if (!rd.state) rd_relayout();
}
static void rd_sheet_gone(lv_event_t *e) { (void)e; rd.sw[0] = rd.sw[1] = rd.sw[2] = rd.size_l = rd.minus = rd.plus = NULL; }
static void rd_contents_cb(lv_event_t *e) { (void)e; acts_close(); rd_contents_page(); }
static lv_obj_t *rd_round(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, SL_PILL_H, SL_PILL_H);
    lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_center(text(b, sym, &sl_sym_24, 0xffffff));
    on_click(b, cb, (void *)ud);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}
static void rd_sheet_label(const char *s)
{
    lv_obj_t *l = text(acts, sl_case(s), &ss_regular_16, C_SUB);
    lv_obj_set_style_pad_hor(l, SL_SP_S, 0);
    lv_obj_set_style_pad_top(l, SL_SP_S, 0);
}
static void rd_sheet(void)
{
    acts_begin();
    rd_sheet_label("Theme");
    lv_obj_t *row = box(acts);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, SL_SP_S, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(row, LV_BASE_DIR_RTL, 0);
    int sw = (W - 20 - 20 - 2 * SL_GUTTER) / 3;
    int lh = lv_font_get_line_height(&ss_semibold_26), lh2 = lv_font_get_line_height(&ss_regular_16);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *s = box(row);
        rd.sw[i] = s;
        lv_obj_set_size(s, sw, lh + lh2 + 2 * SL_GUTTER);
        lv_obj_set_style_radius(s, sl_zune() ? 0 : SL_R_MD, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(rd_paper(i)), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        press_motion(s);
        lv_obj_set_style_transform_pivot_x(s, sw / 2, 0);
        lv_obj_set_style_transform_scale(s, 246, LV_STATE_PRESSED);
        lv_obj_t *a = lv_label_create(s);
        lv_label_set_text(a, "Aa");
        lv_obj_set_style_text_font(a, &ss_semibold_26, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(rd_ink(i)), 0);
        lv_obj_align(a, LV_ALIGN_TOP_MID, 0, SL_GUTTER - 4);
        lv_obj_t *n = text(s, sl_case(rd_theme_names[i]), &ss_regular_16, rd_sub(i));
        lv_obj_set_style_text_color(n, lv_color_hex(rd_sub(i)), 0);
        lv_obj_set_width(n, sw - 2 * SL_SP_S);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -(SL_GUTTER - 4));
        on_click(s, rd_theme_cb, (void *)(intptr_t)i);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    rd_sheet_label("Text size");
    lv_obj_t *sz = box(acts);
    lv_obj_set_size(sz, lv_pct(100), SL_PILL_H + 2 * SL_SP_S);
    rd.minus = rd_round(sz, LV_SYMBOL_MINUS, rd_size_cb, -1);
    sl_align(rd.minus, LV_ALIGN_LEFT_MID, SL_SP_S, 0);
    rd.plus = rd_round(sz, LV_SYMBOL_PLUS, rd_size_cb, 1);
    sl_align(rd.plus, LV_ALIGN_RIGHT_MID, -SL_SP_S, 0);
    rd.size_l = text(sz, "", &ss_semibold_21, C_TEXT);
    lv_obj_set_width(rd.size_l, W - 20 - 20 - 2 * (SL_PILL_H + 2 * SL_SP_S));
    lv_label_set_long_mode(rd.size_l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(rd.size_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(rd.size_l);
    lv_obj_add_event_cb(sz, rd_sheet_gone, LV_EVENT_DELETE, NULL);
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);
    lv_obj_set_style_bg_opa(sep, sl_zune() ? LV_OPA_COVER : 36, 0);
    acts_row_cb(LV_SYMBOL_LIST, "Contents", rd_contents_cb, 0);
    rd_sheet_paint();
    acts_show();
}
static void rd_sheet_cb(lv_event_t *e) { (void)e; if (rd.book) rd_sheet(); }

/* ---------------------------------------------------------------- the contents */
#define RD_TOC_PER_PAGE 60
static lv_obj_t *rd_toc_obj;
static void rd_toc_deleted(lv_event_t *e) { if (lv_event_get_target(e) == rd_toc_obj) rd_toc_obj = NULL; }
static void rd_toc_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!rd.book) return;
    if (rd_toc_obj) page_pop();
    if (k >= 0x10000) rd_goto(k - 0x10000, (struct rd_pos){ 0, 0 }, false, 0);   /* a part, in a book with no contents */
    else if (k >= 0 && k < rd.book->ntoc)
    {
        const struct sl_epub_toc *t = &rd.book->toc[k];
        rd_goto(t->spine, (struct rd_pos){ 0, 0 }, false, t->frag);
    }
}
static void rd_contents_at(int start, bool replace);
static void rd_toc_page_cb(lv_event_t *e) { rd_contents_at((int)(intptr_t)lv_event_get_user_data(e), true); }
static void rd_contents_at(int start, bool replace)
{
    if (!rd.book) return;
    lv_obj_t *p, *body = page_new("Contents", &p);
    rd_toc_obj = p;
    lv_obj_add_event_cb(p, rd_toc_deleted, LV_EVENT_DELETE, NULL);
    const struct sl_epub *b = rd.book;
    bool parts = !b->ntoc;
    int n = parts ? b->nspine : b->ntoc, here = parts ? rd.spine : rd_toc_here();
    if (start < 0) start = here > 0 ? here / RD_TOC_PER_PAGE * RD_TOC_PER_PAGE : 0;
    int end = start + RD_TOC_PER_PAGE < n ? start + RD_TOC_PER_PAGE : n;
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_t *mark = NULL;
    for (int k = start; k < end; k++)
    {
        char t[200];
        if (parts) snprintf(t, sizeof t, sl_tr("Part %d"), k + 1);
        else snprintf(t, sizeof t, "%s", sl_epub_toc_title(b, k));
        lv_obj_t *r = set_action(g, NULL, 0, t, k == here ? sl_tr("Reading") : NULL, rd_toc_cb,
                                 (void *)(intptr_t)(parts ? 0x10000 + k : k));
        /* a line inside another sits in a little, as the book nests them */
        int d = parts ? 0 : b->toc[k].depth;
        lv_obj_t *nm = lv_obj_get_child(r, 0);
        if (d && nm && lv_obj_check_type(nm, &lv_label_class))
        {
            if (sl_ui_rtl()) lv_obj_set_style_pad_right(nm, SL_INSET * (d > 2 ? 2 : d), 0);
            else lv_obj_set_style_pad_left(nm, SL_INSET * (d > 2 ? 2 : d), 0);
        }
        if (k == here) { mark = r; lv_obj_t *v = lv_obj_get_child(r, 1); if (v && lv_obj_check_type(v, &lv_label_class)) lv_obj_set_style_text_color(v, lv_color_hex(C_ACCENT_TXT), 0); }
    }
    if (parts) opt_note(body, "This book has no table of contents: its parts are listed in order.");
    if (start || end < n)
    {
        lv_obj_t *bar = sl_pager(body);
        if (start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Previous", false, rd_toc_page_cb, start - RD_TOC_PER_PAGE);
        if (end < n) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, rd_toc_page_cb, end);
    }
    if (replace) page_replace(p, true); else page_push(p);
    if (mark) { lv_obj_update_layout(body); lv_obj_scroll_to_view_recursive(mark, LV_ANIM_OFF); }
}
static void rd_contents_page(void) { rd_contents_at(-1, false); }
static void rd_contents_cb2(lv_event_t *e) { (void)e; if (rd.book) rd_contents_page(); }

/* ---------------------------------------------------------------- opening a book */
static void rd_open(const char *path)
{
    /* one book open at a time, as every page that keeps its state does: the open one comes back */
    if (rd.page) { page_bring_back(rd.page); return; }
    const char *base = strrchr(path, '/');
    char name[160];
    snprintf(name, sizeof name, "%s", base ? base + 1 : path);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = '\0';
    /* the shelf knows its title already: the page has it from the start */
    struct sl_epub_book *shelf = NULL;
    int ns = sl_epub_shelf(&shelf);
    for (int i = 0; i < ns; i++) if (!strcmp(shelf[i].path, path) && !shelf[i].bad) { snprintf(name, sizeof name, "%s", shelf[i].title); break; }
    free(shelf);
    lv_obj_t *p, *body = page_new(name, &p);
    memset(&rd, 0, sizeof rd);
    rd.at = -1;
    snprintf(rd.path, sizeof rd.path, "%s", path);
    rd.page = p;
    rd.body = body;
    int th, sz;
    sl_epub_prefs(&th, &sz);
    rd.theme = th >= 0 ? th : sl_theme() == 1 ? RD_LIGHT : RD_DARK;
    rd.size = sz >= 0 ? sz : sl_text_step > 0 ? sl_text_step : 1;
    lv_obj_add_event_cb(p, rd_page_deleted, LV_EVENT_DELETE, NULL);
    header_button(p, LV_SYMBOL_SETTINGS, rd_sheet_cb, NULL);
    header_button(p, LV_SYMBOL_LIST, rd_contents_cb2, NULL);
    if (sl_zune()) rd_title(NULL);           /* the small title from the start: no jump when the book's comes */
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(body, 0, 0);
    rd.stage = box(body);
    lv_obj_set_size(rd.stage, W, H - SB_H - SL_HDR_H);
    lv_obj_add_flag(rd.stage, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_3);   /* swipes turn pages; back from the edge only */
    lv_obj_remove_flag(rd.stage, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(rd.stage, rd_stage_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(rd.stage, rd_stage_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(rd.stage, rd_stage_cb, LV_EVENT_PRESS_LOST, NULL);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = box(rd.stage);
        lv_obj_set_style_pad_hor(l, RD_MX, 0);
        lv_obj_set_style_pad_top(l, SL_SP_S, 0);
        lv_obj_set_y(l, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        rd.leaf[i] = l;
    }
    rd.foot = box(rd.stage);
    lv_obj_set_size(rd.foot, W, RD_FOOT);
    rd.track = box(rd.foot);
    lv_obj_set_size(rd.track, RD_TW, 3);
    lv_obj_set_pos(rd.track, RD_MX, SL_SP_S);
    lv_obj_set_style_radius(rd.track, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(rd.track, 40, 0);
    rd.fill = box(rd.foot);
    lv_obj_set_size(rd.fill, 6, 3);
    lv_obj_set_style_radius(rd.fill, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(rd.fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(rd.fill, LV_OPA_COVER, 0);
    sl_align(rd.fill, LV_ALIGN_TOP_LEFT, RD_MX, SL_SP_S);
    int lh = sl_line_h(&ss_regular_16);
    rd.count = text(rd.foot, "", &ss_regular_16, C_SUB);
    lv_obj_set_size(rd.count, 120, lh);
    lv_label_set_long_mode(rd.count, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(rd.count, sl_ui_rtl() ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    sl_align(rd.count, LV_ALIGN_TOP_RIGHT, -RD_MX, SL_SP_S + 3 + SL_SP_S);
    rd.where = text(rd.foot, "", &ss_regular_16, C_SUB);
    lv_obj_set_size(rd.where, RD_TW - 120 - SL_SP_S, lh);
    lv_label_set_long_mode(rd.where, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(rd.where, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    sl_align(rd.where, LV_ALIGN_TOP_LEFT, RD_MX, SL_SP_S + 3 + SL_SP_S);
    sl_label_deep(rd.where);
    lv_obj_add_flag(rd.foot, LV_OBJ_FLAG_HIDDEN);
    rd_geometry();
    rd_state(IC_LIBRARY, "Opening the book", "");
    rd.loading = true;
    rd.tick = lv_timer_create(rd_tick_cb, 16, NULL);
    rd.ticket = sl_epub_open_start(path);
    page_push(p);
}

/* ---------------------------------------------------------------- the shelf */
#define BS_PER_PAGE 24
static lv_obj_t *bs_page_obj, *bs_body;
static struct sl_epub_book *bs_books;
static int bs_n, bs_start;
static unsigned bs_seq;
static lv_timer_t *bs_timer;
static void bs_fs(void) { pod_fs_init(); }
static void bs_open_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k >= 0 && k < bs_n) rd_open(bs_books[k].path);
}
static uint32_t bs_tint(uint32_t hash)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(hash % 360), 45, 52);
    return lv_color_to_u32(c) & 0xffffff;
}
/* a book's cover: its own picture when it has one, else one made from its title */
static lv_obj_t *bs_cover(lv_obj_t *parent, const struct sl_epub_book *bk)
{
    int w = SL_EPUB_COVER_W, h = SL_EPUB_COVER_H;
    uint32_t tint = bs_tint(bk->hash);
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_mix(lv_color_black(), lv_color_hex(tint), 120), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    if (bk->cover && !bk->bad)
    {
        bs_fs();
        char name[96], src[100];
        sl_epub_cover_name(name, sizeof name, bk->hash);
        snprintf(src, sizeof src, "P:%s", name);
        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, src);
        lv_obj_add_event_cb(img, pod_art_gone, LV_EVENT_DELETE, NULL);
        lv_obj_center(img);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
        return c;
    }
    /* the made one: the title in white on its colour, a rule over the author */
    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_base_dir(t, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(t, bk->title);
    lv_obj_set_style_text_font(t, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(t, w - 2 * SL_GUTTER);
    label_lines(t, 4);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, SL_INSET + SL_SP_S);
    lv_obj_t *r = box(c);
    lv_obj_set_size(r, 28, 2);
    lv_obj_set_style_bg_color(r, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_60, 0);
    lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 0, -(SL_INSET + lv_font_get_line_height(&ss_regular_16) + SL_SP_S));
    if (bk->bad)
    {
        lv_obj_t *i = text(c, LV_SYMBOL_WARNING, &sl_sym_24, 0xffffff);
        lv_obj_set_style_text_color(i, lv_color_white(), 0);
        lv_obj_align(i, LV_ALIGN_BOTTOM_MID, 0, -SL_INSET + 4);
    }
    else if (bk->author[0])
    {
        lv_obj_t *a = lv_label_create(c);
        lv_obj_set_style_base_dir(a, LV_BASE_DIR_AUTO, 0);
        lv_label_set_text(a, bk->author);
        lv_obj_set_style_text_font(a, &ss_regular_16, 0);
        lv_obj_set_style_text_color(a, lv_color_white(), 0);
        lv_obj_set_style_text_opa(a, 220, 0);
        lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(a, w - 2 * SL_SP_S);
        label_lines(a, 1);
        lv_obj_align(a, LV_ALIGN_BOTTOM_MID, 0, -SL_INSET + 4);
    }
    return c;
}
/* "43%", "Finished", "New" */
static void bs_state(char *b, size_t n, const struct sl_epub_book *bk)
{
    struct sl_epub_pos p;
    if (bk->bad) snprintf(b, n, "%s", sl_tr("Can't be opened"));
    else if (!sl_epub_pos_get(bk->path, &p)) snprintf(b, n, "%s", sl_tr("New"));
    else if (p.permille >= 995) snprintf(b, n, "%s", sl_tr("Finished"));
    else snprintf(b, n, "%d%%", (p.permille + 5) / 10 ? (p.permille + 5) / 10 : 1);
}
static void bs_bar(lv_obj_t *parent, int w, int permille, int x, int y)
{
    lv_obj_t *t = box(parent);
    lv_obj_set_size(t, w, 4);
    lv_obj_set_style_radius(t, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_bg_opa(t, 50, 0);
    sl_align(t, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t *f = box(t);
    lv_obj_set_size(f, LV_MAX(4, w * permille / 1000), 4);
    lv_obj_set_style_radius(f, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    sl_align(f, LV_ALIGN_LEFT_MID, 0, 0);
}
/* the book read last, a tap from where it was */
static void bs_continue(lv_obj_t *body, int k)
{
    const struct sl_epub_book *bk = &bs_books[k];
    struct sl_epub_pos p;
    if (!sl_epub_pos_get(bk->path, &p)) return;
    lv_obj_t *g = set_group(body, "Continue reading");
    lv_obj_t *r = box(g);
    int ch = SL_EPUB_COVER_H + 2 * SL_GUTTER;
    lv_obj_set_size(r, lv_pct(100), ch);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *c = bs_cover(r, bk);
    sl_elevate(c, sl_zune() ? SL_E0 : SL_E2, 0);
    sl_align(c, LV_ALIGN_LEFT_MID, SL_GUTTER, 0);
    int tx = SL_GUTTER + SL_EPUB_COVER_W + SL_INSET, tw = W - 2 * SL_GUTTER - tx - SL_INSET;
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int th = set_text_h(bk->title, &ss_semibold_23, tw) > lh1 ? 2 * lh1 : lh1;
    lv_obj_t *t = text(r, bk->title, &ss_semibold_23, C_TEXT);
    lv_obj_set_size(t, tw, th);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    set_align(t);
    sl_label_deep(t);
    sl_place(t, tx, SL_GUTTER + SL_SP_S);
    int y = SL_GUTTER + SL_SP_S + th + SL_SP_XS;
    if (bk->author[0])
    {
        lv_obj_t *a = text(r, bk->author, &ss_regular_18, C_SUB);
        lv_obj_set_size(a, tw, lh2);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        set_align(a);
        sl_label_deep(a);
        sl_place(a, tx, y);
    }
    char b[48];
    bs_state(b, sizeof b, bk);
    lv_obj_t *s = text(r, b, &ss_semibold_18, C_ACCENT_TXT);
    lv_obj_set_width(s, tw);
    set_align(s);
    sl_place(s, tx, ch - SL_GUTTER - SL_SP_S - 4 - SL_SP_S - lv_font_get_line_height(&ss_semibold_18));
    bs_bar(r, tw, p.permille, tx, ch - SL_GUTTER - SL_SP_S - 4);
    on_click(r, bs_open_cb, (void *)(intptr_t)k);
}
static void bs_fill(lv_obj_t *body);
static void bs_page_cb(lv_event_t *e)
{
    bs_start = (int)(intptr_t)lv_event_get_user_data(e);
    if (!bs_body) return;
    lv_obj_clean(bs_body);
    bs_fill(bs_body);
    lv_obj_scroll_to_y(bs_body, 0, LV_ANIM_OFF);
}
static void bs_scan_cb(lv_event_t *e)
{
    (void)e;
    sl_epub_shelf_scan();
    if (bs_body) { lv_obj_clean(bs_body); bs_fill(bs_body); }
}
static void bs_fill(lv_obj_t *body)
{
    free(bs_books);
    bs_n = sl_epub_shelf(&bs_books);
    if (bs_start >= bs_n) bs_start = 0;
    if (!bs_n)
    {
        if (sl_epub_shelf_scanning())
            sl_empty(body, IC_LIBRARY, 96, "Looking for books", "Every .epub in the Books folder, and a few other usual places.", 56);
        else
        {
            lv_obj_t *c = sl_empty(body, IC_LIBRARY, 96, "No books on the card yet.",
                                   "Copy .epub files into a folder named Books on the card, then look again.", 56);
            sl_empty_action(c, LV_SYMBOL_REFRESH "  Look for books", bs_scan_cb, NULL);
        }
        return;
    }
    char last[256];
    if (!bs_start && sl_epub_pos_last(last, sizeof last))
        for (int k = 0; k < bs_n; k++) if (!strcmp(bs_books[k].path, last) && !bs_books[k].bad) { bs_continue(body, k); break; }
    char b[64];
    int end = bs_start + BS_PER_PAGE < bs_n ? bs_start + BS_PER_PAGE : bs_n;
    if (bs_n > BS_PER_PAGE) snprintf(b, sizeof b, sl_tr("BOOKS %d\xe2\x80\x93%d OF %d"), bs_start + 1, end, bs_n);
    else snprintf(b, sizeof b, sl_tr(bs_n == 1 ? "%d BOOK" : "%d BOOKS"), bs_n);
    section(body, sl_case(b));
    lv_obj_t *g = box(body);
    lv_obj_set_width(g, lv_pct(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, SL_INSET, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(g, LV_BASE_DIR_RTL, 0);
    int cw = SL_EPUB_COVER_W, lh1 = lv_font_get_line_height(&ss_semibold_18), lh2 = lv_font_get_line_height(&ss_regular_16);
    for (int k = bs_start; k < end; k++)
    {
        const struct sl_epub_book *bk = &bs_books[k];
        lv_obj_t *cell = box(g);
        lv_obj_set_size(cell, cw, SL_EPUB_COVER_H + SL_SP_S + 2 * lh1 + 2 + lh2 + SL_SP_XS);
        lv_obj_set_style_transform_pivot_x(cell, cw / 2, 0);
        lv_obj_set_style_transform_pivot_y(cell, SL_EPUB_COVER_H / 2, 0);
        press_motion(cell);
        lv_obj_set_style_transform_scale(cell, 246, LV_STATE_PRESSED);
        lv_obj_t *c = bs_cover(cell, bk);
        sl_elevate(c, sl_zune() ? SL_E0 : SL_E1, 0);
        lv_obj_t *t = text(cell, bk->title, &ss_semibold_18, 0xffffff);
        lv_obj_set_size(t, cw, 2 * lh1);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        set_align(t);
        sl_label_deep(t);
        lv_obj_set_pos(t, 0, SL_EPUB_COVER_H + SL_SP_S);
        char st[48];
        bs_state(st, sizeof st, bk);
        lv_obj_t *l = text(cell, st, &ss_regular_16, C_SUB);
        lv_obj_set_size(l, cw, lh2);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        set_align(l);
        sl_label_deep(l);
        lv_obj_set_pos(l, 0, SL_EPUB_COVER_H + SL_SP_S + 2 * lh1 + 2);
        on_click(cell, bs_open_cb, (void *)(intptr_t)k);
    }
    if (bs_start || end < bs_n)
    {
        lv_obj_t *bar = sl_pager(body);
        if (bs_start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Previous", false, bs_page_cb, bs_start - BS_PER_PAGE);
        if (end < bs_n) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, bs_page_cb, end);
    }
    opt_note(body, "Books come from the Books folder on the card. Copy .epub files there to add more.");
}
/* the worker found more, or a book closed with a new place: the shelf again */
static void bs_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!bs_body) return;
    unsigned s = sl_epub_shelf_seq();
    bool scanning = sl_epub_shelf_scanning();
    static bool was_scanning;
    if (s != bs_seq || (was_scanning && !scanning && !bs_n))
    {
        bs_seq = s;
        int32_t y = lv_obj_get_scroll_y(bs_body);
        lv_obj_clean(bs_body);
        bs_fill(bs_body);
        lv_obj_update_layout(bs_body);
        lv_obj_scroll_to_y(bs_body, y, LV_ANIM_OFF);
    }
    was_scanning = scanning;
}
static void bs_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != bs_page_obj) return;
    bs_page_obj = bs_body = NULL;
    if (bs_timer) lv_timer_delete(bs_timer);
    bs_timer = NULL;
    free(bs_books);
    bs_books = NULL;
    bs_n = 0;
}
/* back on top (a book closed over it): its places may have moved */
static void bs_refresh(void)
{
    if (!bs_body || !npages || pages[npages - 1] != bs_page_obj) return;
    int32_t y = lv_obj_get_scroll_y(bs_body);
    lv_obj_clean(bs_body);
    bs_fill(bs_body);
    lv_obj_update_layout(bs_body);
    lv_obj_scroll_to_y(bs_body, y, LV_ANIM_OFF);
}
static void ebooks_page(void)
{
    if (page_bring_back(bs_page_obj)) { bs_refresh(); return; }
    lv_obj_t *p, *body = page_new("Books", &p);
    bs_page_obj = p;
    bs_body = body;
    bs_start = 0;
    lv_obj_add_event_cb(p, bs_deleted, LV_EVENT_DELETE, NULL);
    header_button(p, LV_SYMBOL_REFRESH, bs_scan_cb, NULL);
    sl_scroll_indicator(body);
    sl_epub_shelf_scan();
    bs_seq = sl_epub_shelf_seq();
    bs_fill(body);
    bs_timer = lv_timer_create(bs_tick_cb, 250, NULL);
    page_push(p);
}
