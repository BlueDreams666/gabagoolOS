/* Audiobooks: the Audiobook Mod's library (apps/audiobooks) in this
 * interface. The book being listened to is a card of its own, to pick up
 * where it was left (the mod rewinds a little after a long pause and puts
 * the book's own speed back); below it, the books a page at a time, each
 * with how far through it is. The mod is started the first time the page
 * opens, not at boot, so a player without books pays nothing for it.
 * Included by sloppy.c after the shared helpers. */
#include "audiobooks/abdb.h"
#include "audiobooks/audiobooks.h"
#define BK_PER_PAGE 40
/* the Settings components (sloppy_settings.inc), for the book page's own settings */
static lv_obj_t *set_header(lv_obj_t *body, const char *label);
static lv_obj_t *set_group(lv_obj_t *body, const char *label);
static lv_obj_t *set_action(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                            lv_event_cb_t cb, void *ud);
static lv_obj_t *set_action_sub(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                                lv_event_cb_t cb, void *ud);
static lv_obj_t *set_row_button(lv_obj_t *r, int chevron, const char *sym, lv_event_cb_t cb, void *ud);
static lv_obj_t *set_text(lv_obj_t *g, const char *s, uint32_t color);
static void opt_label(lv_obj_t *c, const char *s);
static void opt_note(lv_obj_t *body, const char *s);
static lv_obj_t *bk_page_obj;
static int *bk_order, bk_n, bk_current_start;

static int bk_cmp_title(const void *a, const void *b)
{
    return strcasecmp(ab_db_str(ab_db_book(*(const int *)a), AB_S_TITLE),
                      ab_db_str(ab_db_book(*(const int *)b), AB_S_TITLE));
}
/* "6 h 20 min", "45 min" */
static void bk_dur(char *b, size_t n, uint32_t ms)
{
    uint32_t min = (ms + 30000) / 60000;
    if (min >= 60 && !(min % 60)) snprintf(b, n, sl_tr("%lu h"), (unsigned long)(min / 60));
    else if (min >= 60) snprintf(b, n, sl_tr("%lu h %lu min"), (unsigned long)(min / 60), (unsigned long)(min % 60));
    else snprintf(b, n, sl_tr("%lu min"), (unsigned long)min);
}
/* where the book is: "Finished", "43% · 2 h 14 min left", or its length */
static void bk_state(char *b, size_t n, const struct ab_book *bk)
{
    const struct ab_progress *p = ab_progress_get(bk->id);
    char d[48];
    if (p && p->finished) { strlcpy(b, sl_tr("Finished"), n); return; }
    if (!p || !p->position) { bk_dur(b, n, bk->length); return; }
    bk_dur(d, sizeof d, bk->length > p->position ? bk->length - p->position : 0);
    snprintf(b, n, "%d%% \xc2\xb7 %s %s", ab_book_permille(bk, p) / 10, d, sl_tr("left"));
}
/* each book its own colour, from its id, as albums have theirs */
static uint32_t bk_tint(const struct ab_book *bk)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(bk->id % 360), 55, 62);
    return lv_color_to_u32(c) & 0xffffff;
}
static void bk_play(int book, bool over)
{
    if (book < 0 || book >= ab_db_count()) return;
    if (audiobooks_play(book, over)) np_show_auto();
    else toast_warn("Could not play this book");
}
static void bk_row_cb(lv_event_t *e) { bk_play((int)(intptr_t)lv_event_get_user_data(e), false); }
static void bk_over_cb(lv_event_t *e) { bk_play((int)(intptr_t)lv_event_get_user_data(e), true); }

/* ---- covers: each book's own picture, made into squares on the card by
 * the cover worker (sloppy_libjob.c) the first time the book shows; until
 * then, and for a book without one, its own colour with the books mark */
static void pod_fs_init(void);              /* sloppy_podcast.inc: the card as the C library sees it, drive P */
static void pod_art_gone(lv_event_t *e);    /* and its pictures leave LVGL's cache with their page */
static uint32_t bk_art_key(const struct ab_book *bk)
{
    uint32_t h = 2166136261u ^ bk->id;
    for (const char *c = ab_db_str(bk, AB_S_COVER); *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    h = (h ^ bk->cover_pos) * 16777619u;
    h = (h ^ bk->cover_size) * 16777619u;
    return h ? h : 1;
}
static int bk_art_flags(const struct ab_book *bk)
{
    return (bk->cover_type & AA_FLAG_ID3_UNSYNC ? SL_BOOK_ART_UNSYNC : 0) | (bk->cover_type & AA_FLAG_VORBIS_BASE64 ? SL_BOOK_ART_BASE64 : 0);
}
#define BK_PEND 48
static struct { lv_obj_t *box; int book, size; uint32_t key; } bk_pend[BK_PEND];
static int bk_npend;
static lv_timer_t *bk_art_timer;
static lv_obj_t *bk_hero;                   /* the book being heard's card: it takes its cover's colour */
static uint32_t bk_hero_key;
/* the card's ground: the cover's colour (or the book's) washed down into the card */
static void bk_hero_ground(lv_obj_t *c, uint32_t tint)
{
    lv_obj_set_style_bg_color(c, lv_color_mix(lv_color_hex(tint), lv_color_hex(C_CARD), sl_theme() == 1 ? 70 : 110), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
}
static void bk_art_img(lv_obj_t *c, uint32_t key, int size, bool fade)
{
    pod_fs_init();
    int px = size > SL_BOOK_ART_ROW ? SL_BOOK_ART_BIG : SL_BOOK_ART_ROW;
    char name[96], src[104];
    sl_book_art_name(name, sizeof name, key, px);
    snprintf(src, sizeof src, "P:%s", name);
    lv_obj_delete(lv_obj_get_child(c, 0));  /* the mark */
    lv_obj_t *img = lv_image_create(c);
    lv_obj_move_to_index(img, 0);           /* under a progress bar or a tick */
    lv_image_set_src(img, src);
    lv_obj_add_event_cb(img, pod_art_gone, LV_EVENT_DELETE, NULL);
    if (px != size) lv_image_set_scale(img, (uint32_t)(256 * size / px));
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_clip_corner(c, true, 0);   /* software drawing ignores an image's radius: the box clips it */
    if (fade) lv_obj_fade_in(img, SL_T_PAGE, 0);
}
static void bk_art_tick(lv_timer_t *t);
static void bk_art_unpend(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_current_target(e);
    for (int i = 0; i < bk_npend; )
        if (bk_pend[i].box == c) bk_pend[i] = bk_pend[--bk_npend]; else i++;
}
/* a book's square, size across: its cover when made, else its colour and
 * the mark while the worker makes it (the mark is always child 0) */
static lv_obj_t *bk_art(lv_obj_t *parent, int book, int size)
{
    const struct ab_book *bk = ab_db_book(book);
    uint32_t tint = bk_tint(bk), key = bk_art_key(bk);
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : size > 100 ? SL_R_MD : SL_R_SM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_lighten(lv_color_hex(tint), 30), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_mix(lv_color_black(), lv_color_hex(tint), 120), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_t *n = sl_icon(c, IC_LIBRARY, size * 5 / 12, lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(tint), 200)) & 0xffffff, tint);
    lv_obj_center(n);
    int st = ab_db_str(bk, AB_S_COVER)[0] ? sl_book_art_state(key) : -1;
    if (st > 0) bk_art_img(c, key, size, false);
    else if (!st && bk_npend < BK_PEND)
    {
        bk_pend[bk_npend].box = c; bk_pend[bk_npend].book = book; bk_pend[bk_npend].size = size; bk_pend[bk_npend].key = key;
        bk_npend++;
        lv_obj_add_event_cb(c, bk_art_unpend, LV_EVENT_DELETE, NULL);
        if (!bk_art_timer) bk_art_timer = lv_timer_create(bk_art_tick, 80, NULL);
        lv_timer_resume(bk_art_timer);
    }
    return c;
}
/* the worker's answers, and the next book for it */
static void bk_art_tick(lv_timer_t *t)
{
    uint32_t key = 0;
    int r = sl_book_art_poll(&key);
    if (!r) return;                         /* still making one */
    if (r > 0 || r == -1)
        for (int i = 0; i < bk_npend; )
        {
            if (bk_pend[i].key != key) { i++; continue; }
            lv_obj_t *c = bk_pend[i].box;
            lv_obj_remove_event_cb(c, bk_art_unpend);
            int size = bk_pend[i].size;
            bk_pend[i] = bk_pend[--bk_npend];
            if (r > 0) bk_art_img(c, key, size, true);
        }
    if (r > 0 && bk_hero && key == bk_hero_key)
    {
        uint32_t tint = sl_book_art_tint(key);
        if (tint) bk_hero_ground(bk_hero, tint);
    }
    while (bk_npend)
    {
        const struct ab_book *bk = ab_db_book(bk_pend[0].book);
        if (bk && bk_art_key(bk) == bk_pend[0].key && !sl_book_art_state(bk_pend[0].key))
        {
            sl_book_art_start(bk_pend[0].key, ab_db_str(bk, AB_S_COVER), bk->cover_pos, bk->cover_size, bk_art_flags(bk));
            return;                         /* its answer comes on a later tick */
        }
        lv_obj_remove_event_cb(bk_pend[0].box, bk_art_unpend);   /* gone from the index, or made meanwhile */
        bk_pend[0] = bk_pend[--bk_npend];
    }
    lv_timer_pause(t);
}
/* how far through, along a cover's foot */
static void bk_art_bar(lv_obj_t *c, int book, int size)
{
    const struct ab_book *bk = ab_db_book(book);
    const struct ab_progress *p = ab_progress_get(bk->id);
    int pm = p && !p->finished ? ab_book_permille(bk, p) : 0;
    if (p && p->finished)
    {   /* finished: a tick in its corner, the mark (while there is no picture) out of its way */
        lv_obj_t *m = lv_obj_get_child(c, 0);
        if (m && !lv_obj_check_type(m, &lv_image_class))
            lv_obj_align(m, sl_ui_rtl() ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, sl_ui_rtl() ? -size / 8 : size / 8, size / 8);
        int d = size > 100 ? 30 : 20;
        lv_obj_t *k = box(c);
        lv_obj_set_size(k, d, d);
        lv_obj_set_style_radius(k, SL_R_FULL, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(0x2fae5f), 0);
        lv_obj_set_style_border_color(k, lv_color_white(), 0);
        lv_obj_set_style_border_width(k, 2, 0);
        lv_obj_align(k, sl_ui_rtl() ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, sl_ui_rtl() ? 4 : -4, -4);
        lv_obj_center(text_light(k, LV_SYMBOL_OK, size > 100 ? &sl_sym_16 : &sl_sym_14));
        return;
    }
    if (!pm) return;
    int h = size > 100 ? 6 : 4;
    lv_obj_t *track = box(c);
    lv_obj_set_size(track, size, h);
    lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_t *fill = box(track);
    lv_obj_set_size(fill, LV_MAX(h, size * pm / 1000), h);
    lv_obj_align(fill, sl_ui_rtl() ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(C_ACCENT), 0);
}

/* the book page's card: its bar and state line, kept up to date when the
 * book is marked finished or not */
static lv_obj_t *bk_card_bar, *bk_card_state;
static void bk_hero_gone(lv_event_t *e) { if (lv_event_get_target(e) == bk_hero) bk_hero = NULL; }
/* the book being listened to: its cover beside its name, on the cover's
 * own colour, how far in, and the way back in */
#define BK_COVER 132
static void bk_card(lv_obj_t *body, int book, const char *label)
{
    const struct ab_book *bk = ab_db_book(book);
    const struct ab_progress *p = ab_progress_get(bk->id);
    bool rtl = sl_ui_rtl();
    uint32_t key = bk_art_key(bk), tint = sl_book_art_tint(key);
    if (label) set_header(body, label);
    lv_obj_t *c = box(body);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    bk_hero_ground(c, tint ? tint : bk_tint(bk));
    bk_hero = c; bk_hero_key = key;
    lv_obj_add_event_cb(c, bk_hero_gone, LV_EVENT_DELETE, NULL);
    hairline(c);
    sl_elevate(c, SL_E1, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_margin_bottom(c, 6, 0);
    int iw = W - 2 * SL_GUTTER - 36, tx = BK_COVER + 16, tw = iw - tx;
    lv_obj_t *art = bk_art(c, book, BK_COVER);
    sl_elevate(art, SL_E2, 0);
    lv_obj_align(art, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *t = text(c, ab_db_str(bk, AB_S_TITLE), &ss_semibold_26, 0xffffff);
    lv_obj_set_width(t, tw);
    /* two lines only when it needs them, so the author sits right under it */
    const char *ts = lv_label_get_text(t);
    label_lines(t, lv_text_get_width(ts, strlen(ts), &ss_semibold_26, 0) > tw ? 2 : 1);
    lv_obj_set_style_text_align(t, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(t, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, rtl ? -tx : tx, 0);
    lv_obj_t *a = text(c, ab_db_str(bk, AB_S_AUTHOR), &ss_regular_18, C_SUB);
    lv_obj_set_width(a, tw);
    label_lines(a, 1);
    lv_obj_set_style_text_align(a, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align_to(a, t, rtl ? LV_ALIGN_OUT_BOTTOM_RIGHT : LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    /* how far through, at the cover's foot: the words over a rounded bar,
     * the part listened to in the accent */
    char b[64];
    bk_state(b, sizeof b, bk);
    lv_obj_t *s = text(c, b, &ss_regular_16, C_SUB);
    lv_obj_set_width(s, tw);
    label_lines(s, 1);
    lv_obj_set_style_text_align(s, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    /* at the cover's foot, or under the name where a larger text size takes more */
    lv_obj_update_layout(c);
    int sh = lv_font_get_line_height(lv_obj_get_style_text_font(s, 0));
    int sy = LV_MAX(BK_COVER - 8 - 10 - sh, lv_obj_get_y2(a) + SL_SP_S);
    lv_obj_align(s, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, rtl ? -tx : tx, sy);
    lv_obj_t *bar = lv_bar_create(c);
    lv_obj_set_size(bar, tw, 8);
    lv_obj_align(bar, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, rtl ? -tx : tx, sy + sh + 10);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, ab_book_permille(bk, p), LV_ANIM_OFF);
    if (rtl) lv_obj_set_style_base_dir(bar, LV_BASE_DIR_RTL, 0);
    lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, sl_theme() == 1 ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    bk_card_bar = label ? NULL : bar;
    bk_card_state = label ? NULL : s;
    lv_obj_t *row = box(c);
    lv_obj_set_size(row, iw, 2 * SL_R_PILL);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, LV_MAX(BK_COVER, sy + sh + 18) + 18);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    if (rtl) lv_obj_set_style_base_dir(row, LV_BASE_DIR_RTL, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *go = mn_pill(row, p && p->position && !p->finished ? LV_SYMBOL_PLAY "  Resume" : LV_SYMBOL_PLAY "  Play",
                           10, 2 * SL_R_PILL, true, bk_row_cb, (void *)(intptr_t)book);
    lv_obj_set_flex_grow(go, 2);
    lv_obj_t *over = mn_pill(row, "Start over", 10, 2 * SL_R_PILL, false, bk_over_cb, (void *)(intptr_t)book);
    lv_obj_set_flex_grow(over, 1);
    lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);    /* they stretch: sink about their own middles */
    lv_obj_set_style_transform_pivot_x(over, lv_pct(50), 0);
}

/* A book's own page: its card, what it is about, and its chapters (its
 * files, in play order), the one it is at marked; a tap plays from one. */
static int bk_shown;
static void bk_chapter_cb(lv_event_t *e)
{
    int f = (int)(intptr_t)lv_event_get_user_data(e);
    if (audiobooks_play_at(bk_shown, f, 0)) np_show_auto();
    else toast_warn("Could not play this book");
}

/* ---- bookmarks: the Audiobook Mod's own, in the book folder's .bmark
 * (the file Rockbox's bookmark menu and the mod's Bookmarks page read).
 * Each line is a Rockbox bookmark: the chapter (the book's file, its
 * place in the playlist the mod makes) and the ms into it, with the
 * bookmark's title as an optional token (BM_TITLE, apps/bookmark.c) - the
 * fields stock 2.0 keeps for m4b (path, ms, chapter, title). A single-file
 * book shares its folder's file with the books beside it: each line is
 * matched to its book by file name, and the others are kept as they are. */
#define BK_BM_MAX 64
#define BK_BM_LINE 350                  /* apps/bookmark.c's MAX_BOOKMARK_SIZE */
#define BK_BM_TITLE 0x04                /* apps/bookmark.c's BM_TITLE */
struct bk_bm { uint16_t file; uint32_t ms; char title[64]; };
static struct bk_bm bk_bms[BK_BM_MAX];
static int bk_nbm;
static lv_obj_t *bk_bm_group, *bk_fin_row;

/* the .bmark of the folder the book's playlist is made from, as ab_ui.c names it */
static void bk_bm_path(const struct ab_book *bk, char *out, size_t n)
{
    char dir[MAX_PATH];
    strlcpy(dir, ab_db_str(bk, AB_S_PATH), sizeof dir);
    if (bk->single)
    {
        char *slash = strrchr(dir, '/');
        if (slash) slash[slash == dir ? 1 : 0] = '\0';
    }
    if (!strcmp(dir, "/")) strlcpy(out, "/root_dir.bmark", n);
    else snprintf(out, n, "%s.bmark", dir);
}
static const char *bk_base(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}
/* a line's chapter of this book (its file among names), -1 when it is another's */
static int bk_bm_parse(const char *line, const char **names, int n, struct bk_bm *out)
{
    const char *s = line;
    int flags = 0, idx;
    bool fresh = *s == '>';
    if (fresh) { flags = atoi(++s); s = strchr(s, ';'); if (!s) return -1; s++; }
    long tok[8];
    int ntok = 6 + !fresh + !!(flags & 1) + !!(flags & 2);   /* index, offset, seed, (old), time, repeat, shuffle, pitch, speed */
    for (int i = 0; i < ntok && i < 8; i++) { tok[i] = atol(s); s = strchr(s, ';'); if (!s) return -1; s++; }
    idx = (int)tok[0];
    long ms = tok[fresh ? 3 : 4];
    out->title[0] = '\0';
    if (flags & BK_BM_TITLE)
    {
        const char *e = strchr(s, ';');
        if (!e) return -1;
        size_t len = (size_t)(e - s) < sizeof out->title - 1 ? (size_t)(e - s) : sizeof out->title - 1;
        memcpy(out->title, s, len);
        out->title[len] = '\0';
        s = e + 1;
    }
    const char *file = strchr(s, ';');              /* past the playlist's name */
    if (!file) return -1;
    file++;
    for (int f = 0; f < n; f++)
        if (!strcmp(file, names[f]) || !strcmp(bk_base(file), bk_base(names[f])))
        {
            out->file = (uint16_t)f;
            out->ms = ms < 0 ? 0 : (uint32_t)ms;
            (void)idx;
            return f;
        }
    return -1;
}
static int bk_bm_cmp(const void *a, const void *b)
{
    const struct bk_bm *x = a, *y = b;
    if (x->file != y->file) return x->file < y->file ? -1 : 1;
    return x->ms < y->ms ? -1 : x->ms > y->ms;
}
/* this book's bookmarks, in the order they come in the book */
static void bk_bm_load(int book)
{
    const struct ab_book *bk = ab_db_book(book);
    bk_nbm = 0;
    if (!bk) return;
    char path[MAX_PATH], line[BK_BM_LINE];
    bk_bm_path(bk, path, sizeof path);
    const char **names = NULL;
    int n = ab_db_list_files(ab_db_str(bk, AB_S_PATH), bk->single, &names);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    while (bk_nbm < BK_BM_MAX && read_line(fd, line, sizeof line) > 0)
        if (bk_bm_parse(line, names, n, &bk_bms[bk_nbm]) >= 0) bk_nbm++;
    close(fd);
    qsort(bk_bms, bk_nbm, sizeof *bk_bms, bk_bm_cmp);
}
static bool bk_put(int fd, const char *fmt, ...)
{
    char b[BK_BM_LINE + 2];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof b) return false;
    return write(fd, b, n) == n;
}
/* writes the file again: the other books' lines as they were, then this
 * book's bookmarks. Nothing left: no file. */
static bool bk_bm_save(int book)
{
    const struct ab_book *bk = ab_db_book(book);
    if (!bk) return false;
    char path[MAX_PATH], tmp[MAX_PATH + 8], line[BK_BM_LINE], dir[MAX_PATH];
    bk_bm_path(bk, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    const char **names = NULL;
    int n = ab_db_list_files(ab_db_str(bk, AB_S_PATH), bk->single, &names);
    /* the playlist the mod makes: the book's folder, or the folder its one file is in */
    strlcpy(dir, ab_db_str(bk, AB_S_PATH), sizeof dir);
    if (bk->single) { char *slash = strrchr(dir, '/'); if (slash) slash[1] = '\0'; }
    else strlcat(dir, "/", sizeof dir);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) return false;
    bool ok = true;
    int kept = 0;
    int in = open(path, O_RDONLY);
    if (in >= 0)
    {
        struct bk_bm b;
        while (ok && read_line(in, line, sizeof line) > 0)
        {
            if (!line[0] || bk_bm_parse(line, names, n, &b) >= 0) continue;   /* this book's: written below */
            ok = bk_put(out, "%s\n", line);
            kept++;
        }
        close(in);
    }
    for (int i = 0; ok && i < bk_nbm; i++)
    {
        const struct ab_progress *p = ab_progress_get(bk->id);
        int f = bk_bms[i].file < n ? bk_bms[i].file : 0;
        /* flags: pitch, speed, title; offset 0 starts the chapter at the time */
        ok = bk_put(out, ">%d;%d;0;0;%lu;0;0;%ld;%ld;%s;%s;%s\n", 1 | 2 | BK_BM_TITLE, f, (unsigned long)bk_bms[i].ms,
                      (long)(p && p->pitch ? p->pitch : PITCH_SPEED_100), (long)(p && p->speed ? p->speed : PITCH_SPEED_100),
                      bk_bms[i].title, dir, bk_base(names[f]));
    }
    if (ok) ok = fsync(out) == 0;   /* on the card before the rename */
    if (close(out) != 0) ok = false;
    if (ok && !kept && !bk_nbm) { remove(tmp); remove(path); return true; }
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}
/* where the book is now: the chapter playing and the ms into it, or
 * where it was left */
static void bk_where(int book, int *file, uint32_t *ms)
{
    const struct ab_book *bk = ab_db_book(book);
    struct mp3entry *id3 = audio_current_track();
    int f = 0;
    *file = 0; *ms = 0;
    if (id3 && audiobooks_is_book(id3->path) && ab_db_find_file(id3->path, &f) == book)
    {
        *file = f; *ms = (uint32_t)id3->elapsed;
        return;
    }
    const struct ab_progress *p = bk ? ab_progress_get(bk->id) : NULL;
    if (p && !p->finished) { *file = p->file; *ms = p->elapsed; }
}
/* a chapter's name: its file without the folder and the extension */
static void bk_chapter_name(const char *path, char *out, size_t n)
{
    strlcpy(out, bk_base(path), n);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}
/* how a bookmark reads: the chapter and the time in it, or for a book of
 * one file the time and how far through */
static void bk_bm_words(const struct ab_book *bk, int file, uint32_t ms, const char *title, char *t1, size_t n1,
                        char *t2, size_t n2)
{
    char tm[24];
    fmt_time(tm, sizeof tm, (long)ms);
    if (bk->single || bk->nfiles <= 1)
    {
        strlcpy(t1, tm, n1);
        snprintf(t2, n2, "%d%%", bk->length ? (int)((uint64_t)ms * 100 / bk->length) : 0);
        return;
    }
    if (title && title[0]) strlcpy(t1, title, n1);
    else snprintf(t1, n1, "%d", file + 1);
    strlcpy(t2, tm, n2);
}
static void bk_bm_fill(void);
static void bk_bm_jump_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= bk_nbm) return;
    if (audiobooks_play_at(bk_shown, bk_bms[i].file, bk_bms[i].ms)) np_show_auto();
    else toast_warn("Could not play this book");
}
static void bk_bm_delete_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= bk_nbm) return;
    struct bk_bm gone = bk_bms[i];
    memmove(&bk_bms[i], &bk_bms[i + 1], (bk_nbm - i - 1) * sizeof *bk_bms);
    bk_nbm--;
    if (!bk_bm_save(bk_shown))
    {
        memmove(&bk_bms[i + 1], &bk_bms[i], (bk_nbm - i) * sizeof *bk_bms);
        bk_bms[i] = gone;
        bk_nbm++;
        toast_warn("Could not save the bookmarks");
        return;
    }
    lv_async_call((lv_async_cb_t)bk_bm_fill, NULL);   /* the row that was tapped goes after its event */
    toast("Bookmark deleted");
}
static void bk_bm_add_cb(lv_event_t *e)
{
    (void)e;
    const struct ab_book *bk = ab_db_book(bk_shown);
    if (!bk) return;
    if (bk_nbm >= BK_BM_MAX) { toast_warn("A book keeps 64 bookmarks. Delete one to add another."); return; }
    int f;
    uint32_t ms;
    bk_where(bk_shown, &f, &ms);
    const char **names = NULL;
    int n = ab_db_list_files(ab_db_str(bk, AB_S_PATH), bk->single, &names);
    if (n <= 0) { toast_warn("Could not save the bookmarks"); return; }
    if (f >= n) f = n - 1;
    for (int i = 0; i < bk_nbm; i++)        /* the same place twice is once */
        if (bk_bms[i].file == f && bk_bms[i].ms / 1000 == ms / 1000) { toast("Already bookmarked"); return; }
    struct bk_bm *b = &bk_bms[bk_nbm++];
    b->file = (uint16_t)f;
    b->ms = ms;
    bk_chapter_name(names[f], b->title, sizeof b->title);
    for (char *c = b->title; *c; c++) if (*c == ';' || *c == '\n' || *c == '\r') *c = ',';
    qsort(bk_bms, bk_nbm, sizeof *bk_bms, bk_bm_cmp);
    if (!bk_bm_save(bk_shown)) { bk_bm_load(bk_shown); toast_warn("Could not save the bookmarks"); return; }
    lv_async_call((lv_async_cb_t)bk_bm_fill, NULL);
    toast("Bookmark added");
}
/* the Bookmarks card's rows: add one where the book is, then each */
static void bk_bm_fill(void)
{
    lv_obj_t *g = bk_bm_group;
    const struct ab_book *bk = ab_db_book(bk_shown);
    if (!g || !bk) return;
    lv_obj_clean(g);
    lv_obj_set_user_data(g, NULL);
    char t1[96], t2[64], where[96];
    int f;
    uint32_t ms;
    bk_where(bk_shown, &f, &ms);
    const char **names = NULL;
    int n = ab_db_list_files(ab_db_str(bk, AB_S_PATH), bk->single, &names);
    char ch[64] = "";
    if (f < n) bk_chapter_name(names[f], ch, sizeof ch);
    bk_bm_words(bk, f, ms, ch, t1, sizeof t1, t2, sizeof t2);
    if (bk->single || bk->nfiles <= 1) strlcpy(where, t1, sizeof where);
    else snprintf(where, sizeof where, "%s \xc2\xb7 %s", t1, t2);
    set_action_sub(g, LV_SYMBOL_PLUS, 0x2d7ff0, "Add a bookmark", where, bk_bm_add_cb, NULL);
    if (!bk_nbm) set_text(g, sl_tr("No bookmarks yet. Add one where you are, and come back to it with a tap."), C_SUB);
    for (int i = 0; i < bk_nbm; i++)
    {
        bk_bm_words(bk, bk_bms[i].file, bk_bms[i].ms, bk_bms[i].title, t1, sizeof t1, t2, sizeof t2);
        lv_obj_t *r = set_action_sub(g, NULL, 0, t1, t2, bk_bm_jump_cb, (void *)(intptr_t)i);
        set_row_button(r, 2, LV_SYMBOL_TRASH, bk_bm_delete_cb, (void *)(intptr_t)i);
    }
}

/* ---- the book's own speed: what the mod keeps for it and puts back
 * whenever it plays (music keeps its own) */
static const int bk_speeds[] = { 75, 100, 125, 150, 175, 200, 250 };
static int bk_speed_pct(const struct ab_progress *p)
{
    int32_t v = p && p->speed ? p->speed : PITCH_SPEED_100;
    return (int)((v + PITCH_SPEED_PRECISION / 2) / PITCH_SPEED_PRECISION);
}
static bool bk_is_playing(int book)
{
    struct mp3entry *id3 = audio_current_track();
    int f;
    return id3 && audiobooks_is_book(id3->path) && ab_db_find_file(id3->path, &f) == book;
}
static void bk_speed_chip(int i)
{
    const struct ab_book *bk = ab_db_book(bk_shown);
    if (!bk || i < 0 || i >= (int)ARRAYLEN(bk_speeds)) return;
    struct ab_progress *p = ab_progress_touch(bk->id);
    p->speed = (int32_t)bk_speeds[i] * PITCH_SPEED_PRECISION;
    if (bk_is_playing(bk_shown))             /* heard now; the watcher keeps it */
    {
        if (!global_settings.timestretch_enabled)
        {
            global_settings.timestretch_enabled = true;
            dsp_timestretch_enable(true);
        }
        dsp_set_timestretch(p->speed);
    }
    ab_progress_dirty();
    ab_progress_save();
}

/* ---- finished, or not: the mod's own mark, as reaching the end sets it */
static void bk_finished_cb(lv_event_t *e)
{
    (void)e;
    const struct ab_book *bk = ab_db_book(bk_shown);
    if (!bk) return;
    struct ab_progress *p = ab_progress_touch(bk->id);
    p->finished = !p->finished;
    if (!p->started) p->started = ab_now();
    p->last_played = ab_now();
    ab_progress_dirty();
    ab_progress_save();
    if (bk_fin_row)
    {
        const char *w = sl_tr(p->finished ? "Mark as not finished" : "Mark as finished");
        char up[96];
        lv_label_set_text(lv_obj_get_child(bk_fin_row, 0), sl_zune() ? sl_upper(w, up, sizeof up) : w);
    }
    if (bk_card_bar) lv_bar_set_value(bk_card_bar, ab_book_permille(bk, p), LV_ANIM_ON);
    if (bk_card_state) { char b[64]; bk_state(b, sizeof b, bk); lv_label_set_text(bk_card_state, b); }
    toast(p->finished ? "Marked as finished" : "Marked as not finished");
}
static void bk_page_deleted(lv_event_t *e)
{
    (void)e;
    bk_bm_group = bk_fin_row = bk_card_bar = bk_card_state = NULL;
}

static void bk_book_page(int book)
{
    const struct ab_book *bk = ab_db_book(book);
    if (!bk) return;
    bk_shown = book;
    lv_obj_t *p, *body = page_new("", &p);
    bk_card(body, book, NULL);
    char b[160];
    const char *narr = ab_db_str(bk, AB_S_NARRATOR), *series = ab_db_str(bk, AB_S_SERIES);
    if (series[0])
    {
        const char *no = ab_db_str(bk, AB_S_SERIES_NO);
        char nb[48];
        snprintf(nb, sizeof nb, sl_tr("Book %s"), no);
        snprintf(b, sizeof b, no[0] ? "%s \xc2\xb7 %s" : "%s", series, nb);
        lv_obj_t *s = text(body, b, &ss_semibold_18, C_SUB);
        lv_obj_set_width(s, lv_pct(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_hor(s, sl_zune() ? SL_SP_XS : SL_INSET + 1, 0);   /* in line with the headers */
        lv_obj_set_style_text_align(s, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_top(s, SL_SP_S, 0);
    }
    if (narr[0])
    {
        snprintf(b, sizeof b, "%s %s", sl_tr("Read by"), narr);
        lv_obj_t *s = text(body, b, &ss_regular_18, C_SUB);
        lv_obj_set_width(s, lv_pct(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_hor(s, sl_zune() ? SL_SP_XS : SL_INSET + 1, 0);
        lv_obj_set_style_text_align(s, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
    const char *desc = ab_db_str(bk, AB_S_DESC);
    if (desc[0])
    {
        lv_obj_t *d = text(body, desc, &ss_regular_18, C_TEXT);
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_height(d, 6 * lv_font_get_line_height(&ss_regular_18), 0);
        lv_obj_set_style_pad_hor(d, sl_zune() ? SL_SP_XS : SL_INSET + 1, 0);
        lv_obj_set_style_text_align(d, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_ver(d, SL_SP_S, 0);
    }
    lv_obj_add_event_cb(p, bk_page_deleted, LV_EVENT_DELETE, NULL);
    /* bookmarks: one where the book is, a tap to go back to each */
    bk_bm_load(book);
    bk_bm_group = set_group(body, "Bookmarks");
    bk_bm_fill();
    /* this book: its speed, and finished or not */
    const struct ab_progress *pr = ab_progress_get(bk->id);
    lv_obj_t *g = set_group(body, "This book");
    opt_label(g, "Playback speed");
    static const char *const sp_names[] = { "0.75x", "1x", "1.25x", "1.5x", "1.75x", "2x", "2.5x" };
    int sel = -1, now = bk_speed_pct(pr);
    for (int i = 0; i < (int)ARRAYLEN(bk_speeds); i++) if (bk_speeds[i] == now) sel = i;
    opt_chips(g, sp_names, ARRAYLEN(bk_speeds), sel, bk_speed_chip);
    bk_fin_row = set_action(g, LV_SYMBOL_OK, 0x3aa856, pr && pr->finished ? "Mark as not finished" : "Mark as finished",
                            NULL, bk_finished_cb, NULL);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(bk_fin_row, 2), LV_OBJ_FLAG_HIDDEN);   /* a switch of a row: no chevron */
    opt_note(body, "The speed is this book's own. Music keeps its own speed.");
    const char **names = NULL;
    int n = ab_db_list_files(ab_db_str(bk, AB_S_PATH), bk->single, &names);
    const uint32_t *len = ab_db_lengths(bk);
    if (n > 1)
    {
        snprintf(b, sizeof b, "%d %s", n, sl_tr("CHAPTERS"));
        lv_obj_t *cg = set_group(body, b);
        int shown = n < 200 ? n : 200;     /* a row each: a book of a thousand files shows its first two hundred */
        for (int f = 0; f < shown; f++)
        {
            const char *slash = strrchr(names[f], '/');
            char nm[96];
            strlcpy(nm, slash ? slash + 1 : names[f], sizeof nm);
            char *dot = strrchr(nm, '.');
            if (dot) *dot = '\0';
            char d[48] = "";
            if (len && f < bk->nfiles) bk_dur(d, sizeof d, len[f]);
            bool here = pr && !pr->finished && pr->file == f;
            if (here) snprintf(b, sizeof b, "%s \xc2\xb7 %s", sl_tr("You are here"), d);
            lv_obj_t *r = set_action_sub(cg, NULL, 0, nm, here ? b : d, bk_chapter_cb, (void *)(intptr_t)f);
            if (here) lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(C_ACCENT_TXT), 0);
        }
    }
    page_push(p);
}
static void bk_open_cb(lv_event_t *e) { bk_book_page((int)(intptr_t)lv_event_get_user_data(e)); }

/* A Hebrew or Arabic name in an LTR interface reads from the right, its
 * line with it, as the library's rows do (card_row) */
static void bk_row_dir(lv_obj_t *r, const char *t1)
{
    if (sl_zune() || sl_ui_rtl() || !sl_rtl(t1)) return;
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = lv_obj_get_child(r, i);
        lv_obj_set_style_base_dir(l, LV_BASE_DIR_RTL, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
}
/* the Settings rows the book rows are made of (sloppy_settings.inc) */
static bool set_rtl(void);
static void set_place(lv_obj_t *o, int x, int y, int w);
static void set_align(lv_obj_t *l);
static void set_sep(lv_obj_t *g, lv_obj_t *r, int x);
static lv_obj_t *set_row_box(lv_obj_t *g, int h);
static int set_chevron_w(void);
#define BK_ROW_INNER (W - 2 * SL_GUTTER - 2)   /* SET_INNER: a card's width inside its hairline */
#define BK_ROW_PAD   (SL_GUTTER + SL_SP_XS)    /* SET_PAD_V: a row's air above and below its words */
/* one book in the list: its cover (how far in along its foot), its name,
 * and who wrote it and where it is; lead (a series' "Book 2") goes before
 * that line. Children: name, line, cover, chevron. */
static void bk_row_lead(lv_obj_t *g, int book, const char *lead)
{
    const struct ab_book *bk = ab_db_book(book);
    char b[96];
    bk_state(b, sizeof b, bk);
    const char *author = ab_db_str(bk, AB_S_AUTHOR);
    char sub[200];
    if (lead) snprintf(sub, sizeof sub, "%s \xc2\xb7 %s", lead, b);
    else if (author[0]) snprintf(sub, sizeof sub, "%s \xc2\xb7 %s", author, b);
    else strlcpy(sub, b, sizeof sub);
    int art = SL_ART_ROW, tx = SL_INSET + art + SL_GUTTER;
    int lw = BK_ROW_INNER - tx - SL_INSET - set_chevron_w() - SL_SP_S;
    int lh1 = lv_font_get_line_height(&ss_semibold_21), lh2 = lv_font_get_line_height(&ss_regular_16);
    int deep = sl_line_h(&ss_regular_16) - lh2;
    int h = LV_MAX(art + 2 * SL_GUTTER, lh1 + lh2 + deep + 2 * BK_ROW_PAD), y = (h - lh1 - lh2 - deep) / 2;
    lv_obj_t *r = set_row_box(g, h);
    const char *title = ab_db_str(bk, AB_S_TITLE);
    lv_obj_t *a = text(r, title, &ss_semibold_21, C_TEXT);
    lv_obj_set_size(a, lw, lh1);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, y, lw);
    lv_obj_t *s = text(r, sub, &ss_regular_16, C_SUB);
    lv_obj_set_size(s, lw, lh2);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    set_align(s);
    sl_label_deep(s);
    set_place(s, tx, y + lh1, lw);
    lv_obj_t *c = bk_art(r, book, art);
    bk_art_bar(c, book, art);
    set_place(c, SL_INSET, (h - art) / 2, art);
    lv_obj_t *ch = text(r, set_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    lv_obj_align(ch, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    set_sep(g, r, tx);
    on_click(r, bk_open_cb, (void *)(intptr_t)book);   /* its page */
    bk_row_dir(r, title);
}
static void bk_row(lv_obj_t *g, int book) { bk_row_lead(g, book, NULL); }

/* ---- other ways in: what's being listened to, by author, by series, finished */
static const char **bk_keys;               /* the authors or series, pointers into the index */
static int bk_nkeys;
static int bk_cmp_key(const void *a, const void *b) { return sl_sort_cmp(*(const char *const *)a, *(const char *const *)b); }
/* every author (AB_S_AUTHOR) or series (AB_S_SERIES) once, sorted */
static void bk_collect(enum ab_str which)
{
    int n = ab_db_count();
    free(bk_keys);
    bk_keys = malloc((n ? n : 1) * sizeof *bk_keys);
    bk_nkeys = 0;
    for (int i = 0; bk_keys && i < n; i++)
    {
        const char *k = ab_db_str(ab_db_book(i), which);
        if (!k[0]) continue;
        int j = 0;
        while (j < bk_nkeys && strcasecmp(bk_keys[j], k)) j++;
        if (j == bk_nkeys) bk_keys[bk_nkeys++] = k;
    }
    if (bk_nkeys) qsort(bk_keys, bk_nkeys, sizeof *bk_keys, bk_cmp_key);
}
static int bk_count_with(enum ab_str which, const char *k)
{
    int n = 0;
    for (int i = 0; i < ab_db_count(); i++) if (!strcasecmp(ab_db_str(ab_db_book(i), which), k)) n++;
    return n;
}
/* a series in its own order (book 1, 2, 2.5 ...), then by title */
static int bk_cmp_series(const void *a, const void *b)
{
    const struct ab_book *x = ab_db_book(*(const int *)a), *y = ab_db_book(*(const int *)b);
    int c = strcasecmp(ab_db_str(x, AB_S_SERIES), ab_db_str(y, AB_S_SERIES));
    if (c) return c;
    double nx = atof(ab_db_str(x, AB_S_SERIES_NO)), ny = atof(ab_db_str(y, AB_S_SERIES_NO));
    if (nx != ny) return nx < ny ? -1 : 1;
    return sl_sort_cmp(ab_db_str(x, AB_S_TITLE), ab_db_str(y, AB_S_TITLE));
}
/* the latest listened to first */
static int bk_cmp_recent(const void *a, const void *b)
{
    const struct ab_progress *x = ab_progress_get(ab_db_book(*(const int *)a)->id);
    const struct ab_progress *y = ab_progress_get(ab_db_book(*(const int *)b)->id);
    uint32_t tx = x ? x->last_played : 0, ty = y ? y->last_played : 0;
    return tx < ty ? 1 : tx > ty ? -1 : 0;
}
enum { BK_V_CONTINUE, BK_V_FINISHED };
static bool bk_in_view(int view, int book)
{
    const struct ab_progress *p = ab_progress_get(ab_db_book(book)->id);
    if (view == BK_V_FINISHED) return p && p->finished;
    return p && !p->finished && p->position;
}
static int bk_view_count(int view)
{
    int n = 0;
    for (int i = 0; i < ab_db_count(); i++) if (bk_in_view(view, i)) n++;
    return n;
}
/* Continue listening, or Finished: the books, the latest first */
static void bk_view_page(int view)
{
    lv_obj_t *p, *body = page_new(view == BK_V_FINISHED ? "Finished" : "Continue listening", &p);
    int n = ab_db_count(), m = 0;
    int *o = malloc((n ? n : 1) * sizeof *o);
    for (int i = 0; o && i < n; i++) if (bk_in_view(view, i)) o[m++] = i;
    if (m) qsort(o, m, sizeof *o, bk_cmp_recent);
    if (!m)
    {
        if (view == BK_V_FINISHED)
            sl_empty(body, IC_LIBRARY, 96, "No finished books yet.", "Books you finish, or mark finished on their page, show here.", 56);
        else
            sl_empty(body, IC_LIBRARY, 96, "Nothing in progress.", "Books you've started show here, the latest first.", 56);
    }
    else
    {
        char b[48];
        snprintf(b, sizeof b, sl_tr(m == 1 ? "%d BOOK" : "%d BOOKS"), m);
        lv_obj_t *g = set_group(body, b);
        for (int k = 0; k < m && k < 200; k++) bk_row(g, o[k]);
    }
    free(o);
    page_push(p);
}
static void bk_view_cb(lv_event_t *e) { bk_view_page((int)(intptr_t)lv_event_get_user_data(e)); }
/* one author's books, or one series': a series in its order */
static void bk_group_page(enum ab_str which, int k)
{
    if (k < 0 || k >= bk_nkeys) return;
    const char *key = bk_keys[k];
    lv_obj_t *p, *body = page_new(key, &p);
    int n = ab_db_count(), m = 0;
    int *o = malloc((n ? n : 1) * sizeof *o);
    for (int i = 0; o && i < n; i++) if (!strcasecmp(ab_db_str(ab_db_book(i), which), key)) o[m++] = i;
    if (m) qsort(o, m, sizeof *o, which == AB_S_SERIES ? bk_cmp_series : bk_cmp_title);
    if (which == AB_S_AUTHOR && m > 1)
    {
        /* an author's: each series together, in its order, then the rest by title */
        for (int i = 1; i < m; i++)
            for (int j = i; j > 0; j--)
            {
                const struct ab_book *x = ab_db_book(o[j - 1]), *y = ab_db_book(o[j]);
                const char *sx = ab_db_str(x, AB_S_SERIES), *sy = ab_db_str(y, AB_S_SERIES);
                bool swap = sx[0] && sy[0] ? bk_cmp_series(&o[j - 1], &o[j]) > 0 : !sx[0] && sy[0];
                if (!swap) break;
                int t = o[j]; o[j] = o[j - 1]; o[j - 1] = t;
            }
    }
    char b[48];
    snprintf(b, sizeof b, sl_tr(m == 1 ? "%d BOOK" : "%d BOOKS"), m);
    lv_obj_t *g = set_group(body, b);
    for (int i = 0; i < m && i < 200; i++)
    {
        const char *no = ab_db_str(ab_db_book(o[i]), AB_S_SERIES_NO);
        char lead[48];
        if (which == AB_S_SERIES && no[0]) snprintf(lead, sizeof lead, sl_tr("Book %s"), no);
        bk_row_lead(g, o[i], which == AB_S_SERIES && no[0] ? lead : NULL);
    }
    free(o);
    page_push(p);
}
static void bk_author_cb(lv_event_t *e) { bk_group_page(AB_S_AUTHOR, (int)(intptr_t)lv_event_get_user_data(e)); }
static void bk_series_cb(lv_event_t *e) { bk_group_page(AB_S_SERIES, (int)(intptr_t)lv_event_get_user_data(e)); }
/* Authors, or Series: each once, with how many books */
static void bk_keys_page(enum ab_str which)
{
    lv_obj_t *p, *body = page_new(which == AB_S_SERIES ? "Series" : "Authors", &p);
    bk_collect(which);
    lv_obj_t *g = bk_nkeys ? set_group(body, NULL) : NULL;
    if (!bk_nkeys)
        sl_empty(body, IC_LIBRARY, 96, which == AB_S_SERIES ? "No series here yet." : "No authors here yet.",
                 "They come from the books' tags, or from a series folder's name.", 56);
    for (int k = 0; k < bk_nkeys && k < 500; k++)
    {
        char b[160], c[48];
        sl_count(c, sizeof c, bk_count_with(which, bk_keys[k]), "%s book", "%s books");
        if (which == AB_S_SERIES)
        {
            /* the series' author, from its first book */
            const char *au = "";
            for (int i = 0; i < ab_db_count() && !au[0]; i++)
                if (!strcasecmp(ab_db_str(ab_db_book(i), AB_S_SERIES), bk_keys[k])) au = ab_db_str(ab_db_book(i), AB_S_AUTHOR);
            if (au[0]) snprintf(b, sizeof b, "%s \xc2\xb7 %s", au, c); else strlcpy(b, c, sizeof b);
        }
        else strlcpy(b, c, sizeof b);
        bk_row_dir(set_action_sub(g, NULL, 0, bk_keys[k], b, which == AB_S_SERIES ? bk_series_cb : bk_author_cb, (void *)(intptr_t)k), bk_keys[k]);
    }
    page_push(p);
}
static void bk_keys_cb(lv_event_t *e) { bk_keys_page((enum ab_str)(intptr_t)lv_event_get_user_data(e)); }
/* ---- the ways in: four big cards two across, each a pair of glass tiles
 * in its own colour, one laid over the other, its mark on the front one */
#define BK_GLASS 52                         /* each tile of the pair */
#define BK_GLASS_OFF 16                     /* the back one, up and along */
#define BK_TILE_TINT (sl_theme() == 1 ? 18 : 13)   /* of 255: the card's breath of its colour (Settings' tiles') */
static void sl_glass_light_draw(lv_event_t *e);   /* sloppy_glass.inc: light across the top, shade at the foot */
/* the pair, (size + off) square; big: the empty state's, half again */
static lv_obj_t *bk_glass_pair(lv_obj_t *parent, int icon, uint32_t tint, int s, int off)
{
    bool rtl = sl_ui_rtl(), zule = sl_zune();
    lv_obj_t *w = box(parent);
    lv_obj_set_size(w, s + off, s + off);
    lv_obj_t *back = box(w);
    lv_obj_set_size(back, s, s);
    lv_obj_set_style_radius(back, zule ? 0 : SL_R_ICON(s), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_lighten(lv_color_hex(tint), zule ? 60 : 70), 0);
    lv_obj_set_style_opa(back, zule ? LV_OPA_COVER : sl_theme() == 1 ? 140 : 175, 0);
    if (!zule)
    {
        lv_obj_set_style_border_color(back, lv_color_white(), 0);
        lv_obj_set_style_border_opa(back, 70, 0);
        lv_obj_set_style_border_width(back, 1, 0);
    }
    lv_obj_align(back, rtl ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *front = box(w);
    lv_obj_set_size(front, s, s);
    lv_obj_set_style_radius(front, zule ? 0 : SL_R_ICON(s), 0);
    lv_obj_set_style_bg_opa(front, zule ? LV_OPA_COVER : 225, 0);   /* the back one shows through */
    lv_obj_set_style_bg_color(front, lv_color_hex(tint), 0);
    if (!zule)
    {   /* glass: a rim catching the light, lit across its top, glowing in its colour */
        lv_obj_set_style_border_color(front, lv_color_white(), 0);
        lv_obj_set_style_border_opa(front, 110, 0);
        lv_obj_set_style_border_width(front, 1, 0);
        lv_obj_set_style_shadow_width(front, 18, 0);
        lv_obj_set_style_shadow_offset_y(front, 5, 0);
        lv_obj_set_style_shadow_color(front, sl_theme() == 1 ? lv_color_darken(lv_color_hex(tint), 60) : lv_color_hex(tint), 0);
        lv_obj_set_style_shadow_opa(front, sl_theme() == 1 ? 80 : 90, 0);
        lv_obj_add_event_cb(front, sl_glass_light_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    }
    lv_obj_align(front, rtl ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_center(sl_icon(front, icon, s * 9 / 16, 0xffffff, tint));
    return w;
}
static int bk_tile_h(void)
{
    return 2 * SL_INSET + BK_GLASS + BK_GLASS_OFF + SL_SP_S + SL_SP_XS + lv_font_get_line_height(&ss_semibold_21);
}
static void bk_tile(lv_obj_t *g, int icon, const char *label, uint32_t tint, int count, lv_event_cb_t cb, void *ud, int k)
{
    bool rtl = sl_ui_rtl();
    int h = bk_tile_h();
    lv_obj_t *t = home_card(g, HOME_CARD_W);
    lv_obj_set_height(t, h);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_style_transform_pivot_y(t, h / 2, 0);
    hairline(t);
    if (!sl_zune())
    {   /* a breath of its colour over the card; under the finger, more */
        lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(tint), lv_color_hex(C_CARD), BK_TILE_TINT), 0);
        lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(tint), lv_color_hex(C_CARD), BK_TILE_TINT * 3), LV_STATE_PRESSED);
    }
    lv_obj_t *ic = bk_glass_pair(t, icon, tint, BK_GLASS, BK_GLASS_OFF);
    lv_obj_align(ic, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, rtl ? -SL_INSET : SL_INSET, SL_INSET);
    home_icon_rise(ic, (unsigned)(60 + 50 * k));   /* after its card, one after another */
    lv_obj_t *l = text(t, label, &ss_semibold_21, 0xffffff);
    lv_obj_set_width(l, HOME_CARD_W - 2 * SL_INSET);
    lv_obj_set_style_text_align(l, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    label_lines(l, 1);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, SL_INSET, -(SL_INSET - SL_SP_XS));
    if (count >= 0)
    {   /* how many, at the far top, on the front tile's middle */
        char n[16];
        static const lv_font_t *const f[SL_TEXT_STEPS] = { &ss_regular_21, &ss_regular_23, &ss_regular_26, &ss_regular_26 };
        lv_obj_t *c = text(t, sl_fmt_count(n, sizeof n, count), f[sl_text_step], C_SUB);
        int cy = SL_INSET + BK_GLASS_OFF + (BK_GLASS - lv_font_get_line_height(f[sl_text_step])) / 2;
        if (sl_zune()) lv_obj_align(c, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                                    rtl ? -(SL_INSET + BK_GLASS + BK_GLASS_OFF + SL_GUTTER) : SL_INSET + BK_GLASS + BK_GLASS_OFF + SL_GUTTER, cy);
        else lv_obj_align(c, rtl ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, rtl ? SL_INSET : -SL_INSET, cy);
    }
    on_click(t, cb, ud);
}
static void bk_library_at(int start, bool replace);
static void bk_library_cb(lv_event_t *e) { bk_library_at((int)(intptr_t)lv_event_get_user_data(e), false); }
static void bk_browse(lv_obj_t *body)
{
    lv_obj_t *g = box(body);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, SL_GUTTER, 0);
    lv_obj_set_style_margin_top(g, SL_GUTTER, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(g, LV_BASE_DIR_RTL, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the cards' press shrink, whole */
    bk_tile(g, IC_LIBRARY, "Library", 0x2f7cf6, ab_db_count(), bk_library_cb, (void *)(intptr_t)0, 0);
    bk_collect(AB_S_AUTHOR);
    bk_tile(g, IC_ARTISTS, "Authors", 0xe0479b, bk_nkeys, bk_keys_cb, (void *)(intptr_t)AB_S_AUTHOR, 1);
    bk_collect(AB_S_SERIES);
    bk_tile(g, IC_G_COPY, "Series", 0x27a960, bk_nkeys, bk_keys_cb, (void *)(intptr_t)AB_S_SERIES, 2);
    bk_tile(g, IC_G_OK, "Finished", 0x8a5cf0, bk_view_count(BK_V_FINISHED), bk_view_cb, (void *)(intptr_t)BK_V_FINISHED, 3);
}
/* a section's small capitals with a way to all of it at the far end */
static void bk_header_more(lv_obj_t *body, const char *label, const char *more, lv_event_cb_t cb, void *ud)
{
    if (!more) { set_header(body, label); return; }
    bool rtl = sl_ui_rtl();
    lv_obj_t *r = box(body);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    set_header(r, label);
    lv_obj_t *m = text(r, more, &ss_semibold_18, C_ACCENT_TXT);
    lv_obj_set_style_pad_ver(m, 14, 0);                 /* a finger's room */
    lv_obj_set_style_pad_hor(m, SL_INSET + 1, 0);
    lv_obj_align(m, rtl ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 14 - SL_SP_S);
    lv_obj_add_flag(r, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    on_click(m, cb, ud);
}

/* Library: every book by its cover, two across, a page of them at a time */
#define BK_GRID_PAGE 20
static int bk_lib_start;
static void bk_lib_page_cb(lv_event_t *e) { bk_library_at((int)(intptr_t)lv_event_get_user_data(e), true); }
static void bk_sort_all(void)
{
    int n = ab_db_count();
    free(bk_order);
    bk_order = malloc((n ? n : 1) * sizeof *bk_order);
    bk_n = bk_order ? n : 0;
    for (int i = 0; i < bk_n; i++) bk_order[i] = i;
    if (bk_n) qsort(bk_order, bk_n, sizeof *bk_order, bk_cmp_title);
}
static void bk_library_at(int start, bool replace)
{
    bk_sort_all();
    bool rtl = sl_ui_rtl();
    lv_obj_t *p, *body = page_new("Library", &p);
    lv_obj_t *g = box(body);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, SL_GUTTER + SL_SP_XS, 0);
    lv_obj_set_style_margin_top(g, SL_SP_S, 0);
    if (rtl) lv_obj_set_style_base_dir(g, LV_BASE_DIR_RTL, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int end = start + BK_GRID_PAGE < bk_n ? start + BK_GRID_PAGE : bk_n;
    for (int k = start; k < end; k++)
    {
        int book = bk_order[k];
        const struct ab_book *bk = ab_db_book(book);
        lv_obj_t *cell = box(g);
        lv_obj_set_size(cell, HOME_CARD_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, rtl ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(cell, SL_SP_XS, 0);
        lv_obj_set_style_pad_bottom(cell, SL_SP_XS, 0);
        press_sink(cell, HOME_CARD_W, HOME_CARD_W);
        press_motion(cell);
        lv_obj_t *a = bk_art(cell, book, HOME_CARD_W);
        bk_art_bar(a, book, HOME_CARD_W);
        lv_obj_set_style_margin_bottom(a, SL_SP_XS, 0);
        const char *title = ab_db_str(bk, AB_S_TITLE);
        lv_obj_t *t = text(cell, title, &ss_semibold_18, 0xffffff);
        lv_obj_set_width(t, HOME_CARD_W);
        lv_obj_set_style_text_align(t, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        label_lines(t, 1);
        char b[64];
        bk_state(b, sizeof b, bk);
        const char *au = ab_db_str(bk, AB_S_AUTHOR);
        lv_obj_t *s = text(cell, au[0] ? au : b, &ss_regular_16, C_SUB);
        lv_obj_set_width(s, HOME_CARD_W);
        lv_obj_set_style_text_align(s, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        label_lines(s, 1);
        if (!rtl && !sl_zune() && sl_rtl(title))
            for (int i = 1; i < 3; i++) lv_obj_set_style_text_align(lv_obj_get_child(cell, i), LV_TEXT_ALIGN_RIGHT, 0);
        on_click(cell, bk_open_cb, (void *)(intptr_t)book);
    }
    if (start || end < bk_n)
    {
        lv_obj_t *bar = sl_pager(body);
        if (start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Previous", false, bk_lib_page_cb, start - BK_GRID_PAGE);
        if (end < bk_n) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, bk_lib_page_cb, end);
    }
    if (replace) page_replace(p, start >= bk_lib_start); else page_push(p);
    bk_lib_start = start;
}
static void bk_page_cb(lv_event_t *e);
static void books_page_at(int start, bool replace);
/* Reading the folder again: books whose folders changed are read, the rest kept */
static void bk_scan_cb(lv_event_t *e)
{
    (void)e;
    toast("Updating your books");
    lv_refr_now(NULL);                  /* the word is up before the card is read */
    ab_lock();
    ab_scan_silent = true;
    bool ok = ab_db_scan(false, true);
    ab_scan_silent = false;
    ab_unlock();
    if (!ok) toast_warn("Could not read the Audiobooks folder");
    books_page_at(0, true);
}
static void books_page_at(int start, bool replace)
{
    audiobooks_init();                  /* once; loads the index and what was listened to */
    lv_obj_t *p, *body = page_new("Audiobooks", &p);
    bk_page_obj = p;
    bk_sort_all();
    header_button(p, LV_SYMBOL_REFRESH, bk_scan_cb, NULL);
    if (!bk_n)
    {
        /* no books: the books' own glass, what to do about it, and the
         * button that does the rest */
        lv_obj_t *c = sl_empty(body, IC_NONE, 96, "No audiobooks on the card yet.",
                               "Put each book in a folder of its own inside a folder named Audiobooks, then look again.", 48);
        lv_obj_t *ic = lv_obj_get_child(c, 0);
        lv_obj_set_size(ic, 116, 116);
        lv_obj_center(bk_glass_pair(ic, IC_LIBRARY, 0x2f7cf6, 84, 32));
        home_icon_rise(ic, 60);
        sl_empty_action(c, LV_SYMBOL_REFRESH "  Look for books", bk_scan_cb, NULL);
    }
    if (!bk_n) abs_books_entry(body);   /* or from an Audiobookshelf server */
    int last = start ? -1 : ab_progress_last_book();
    if (last >= 0)
    {
        int going = bk_view_count(BK_V_CONTINUE);
        bk_header_more(body, "CONTINUE LISTENING", going > 1 ? "See all" : NULL, bk_view_cb, (void *)(intptr_t)BK_V_CONTINUE);
        bk_card(body, last, NULL);
        bk_card_bar = bk_card_state = NULL;   /* the book page's, not this one's */
    }
    if (bk_n && !start) bk_browse(body);
    if (bk_n)
    {
        char b[80];
        int end = start + BK_PER_PAGE < bk_n ? start + BK_PER_PAGE : bk_n;
        if (bk_n > BK_PER_PAGE) snprintf(b, sizeof b, sl_tr("BOOKS %d\xe2\x80\x93%d OF %d"), start + 1, end, bk_n);
        else snprintf(b, sizeof b, sl_tr(bk_n == 1 ? "%d BOOK" : "%d BOOKS"), bk_n);
        lv_obj_t *g = set_group(body, b);
        for (int k = start; k < end; k++) bk_row(g, bk_order[k]);
        if (start || end < bk_n)
        {
            lv_obj_t *bar = sl_pager(body);
            if (start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Previous", false, bk_page_cb, start - BK_PER_PAGE);
            if (end < bk_n) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, bk_page_cb, end);
        }
    }
    if (bk_n && !start) abs_books_entry(body);
    if (replace) page_replace(p, start >= bk_current_start); else page_push(p);
    bk_current_start = start;
}
static void bk_page_cb(lv_event_t *e) { books_page_at((int)(intptr_t)lv_event_get_user_data(e), true); }
static void books_page(void) { books_page_at(0, false); }
