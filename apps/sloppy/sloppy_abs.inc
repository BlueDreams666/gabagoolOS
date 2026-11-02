/* Audiobookshelf: Music > Audiobooks > Audiobookshelf browses your server
 * (its libraries, books first, then podcasts; continue listening; a
 * library's titles, authors and series; a book with its chapters, or a
 * podcast with its episodes), brings a book down to the card (Audiobooks/
 * <author>/<title>) to play as our own audiobooks do (30 s skips, rewind
 * after a pause, the book's speed), keeps it there until you remove it,
 * and keeps your place the same on both sides: ours goes to the server on
 * pause, at each chapter, every few minutes while playing, at the end and
 * when Wi-Fi comes back; the server's, when it moved after ours (another
 * phone, the web), is taken when a book or Continue listening opens, with
 * a word on where it came from. The requests, downloads and covers are
 * sloppy_absjob.inc's worker; this page asks and looks. Settings > Music
 * servers signs in. Included by sloppy.c after the music server. */
#include "sloppy_abs.h"
#define ABS_TINT 0xc9842b                 /* Audiobookshelf's own: its icon, a book with no cover */

static struct sl_abs_status abs_st;
static unsigned abs_every_s = 180;        /* while playing, the place goes this often */
static void abs_edit_page(void);
static void abs_item_page(const char *id, const char *title);
static bool abs_wifi_up(void) { return wf_online(); }

/* ---- words */
static const char *abs_err_words(int err)
{
    switch (err)
    {
        case SL_ABS_E_NET: return "The server didn't answer. Check the address, and that it is on and reachable from this network.";
        case SL_ABS_E_AUTH: return "Wrong user name or password, or the API token is no longer valid.";
        case SL_ABS_E_NOTABS: return "Something answers there, but not Audiobookshelf. Check the port and the path.";
        case SL_ABS_E_NOTFOUND: return "It's no longer on the server.";
        case SL_ABS_E_NOMEM: return "Not enough memory for all of that.";
        case SL_ABS_E_OFFLINE: return "You're offline. Books on the card still play.";
        case SL_ABS_E_NOSERVER: return "Sign in to your Audiobookshelf first.";
        case SL_ABS_E_FULL: return "There isn't enough room on the card for this book.";
        case SL_ABS_E_ADDRESS: return "That doesn't look like an address. Try abs.example.com or 192.168.1.5:13378.";
        case SL_ABS_E_HTTPS: return "A secure connection to the server couldn't be made. Try its http:// address on your home network.";
        case SL_ABS_E_ELSEWHERE: return "The server sent us to another address. Type that address instead.";
        case SL_ABS_E_NOFILES: return "This item has no audio to play.";
        case SL_ABS_E_HOLD: return "Not while the player is in USB mode.";
        default: return "The server couldn't do that.";
    }
}
/* where an item is: "Finished", "43% · 2 h 14 min left", or its length */
static void abs_state(char *b, size_t n, const struct sl_abs_item *it)
{
    char d[48];
    b[0] = '\0';
    if (it->finished) { strlcpy(b, sl_tr("Finished"), n); return; }
    if (it->permille && it->dur_ms)
    {
        uint32_t at = it->cur_ms ? it->cur_ms : (uint32_t)((uint64_t)it->dur_ms * it->permille / 1000);
        bk_dur(d, sizeof d, it->dur_ms > at ? it->dur_ms - at : 0);
        snprintf(b, n, "%d%% \xc2\xb7 %s %s", it->permille / 10, d, sl_tr("left"));
        return;
    }
    if (it->dur_ms) bk_dur(b, n, it->dur_ms);
}
/* "authors.<base64 of the id>": Audiobookshelf's filters */
static void abs_filter(char *out, size_t n, const char *what, const char *id)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = (size_t)snprintf(out, n, "%s.", what), len = strlen(id);
    for (size_t i = 0; i < len && k + 5 < n; i += 3)
    {
        uint32_t v = (uint32_t)(unsigned char)id[i] << 16 | (i + 1 < len ? (uint32_t)(unsigned char)id[i + 1] << 8 : 0) |
                     (i + 2 < len ? (unsigned char)id[i + 2] : 0);
        out[k++] = t[v >> 18 & 63];
        out[k++] = t[v >> 12 & 63];
        out[k++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[k++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[k] = '\0';
}

/* ---- covers: the worker's squares through Podcasts' drive */
struct abs_pend { lv_obj_t *box; char item[48]; int size; };
static struct abs_pend abs_pend[40];
static int abs_npend;
static void abs_art_img(lv_obj_t *c, const char *item, int size)
{
    char name[96], src[100];
    int px = size > SL_ABS_ART_ROW ? SL_ABS_ART_BIG : SL_ABS_ART_ROW;
    sl_abs_art_name(name, sizeof name, item, px);
    snprintf(src, sizeof src, "P:%s", name);
    lv_obj_t *img = lv_image_create(c);
    lv_image_set_src(img, src);
    lv_obj_add_event_cb(img, pod_art_gone, LV_EVENT_DELETE, NULL);
    if (px != size) lv_image_set_scale(img, (uint32_t)(256 * size / px));
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_move_to_index(img, 1);         /* over the glyph, under a bar or a badge */
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_add_flag(lv_obj_get_child(c, 0), LV_OBJ_FLAG_HIDDEN);
}
static void abs_art_gone(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_current_target(e);
    for (int i = 0; i < abs_npend; i++)
        if (abs_pend[i].box == c) { abs_pend[i] = abs_pend[--abs_npend]; break; }
}
static lv_obj_t *abs_art(lv_obj_t *parent, const char *item, const char *name, int size, bool podcast)
{
    uint32_t tint = sub_tint_of(name && name[0] ? name : item);
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : size > 100 ? SL_R_MD : SL_R_SM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_mix(lv_color_black(), lv_color_hex(tint), 150), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_t *g = sl_icon(c, podcast ? IC_PODCAST : IC_LIBRARY, size * 3 / 8,
                          lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(tint), 190)) & 0xffffff, tint);
    lv_obj_center(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (!item || !item[0]) return c;
    pod_fs_init();
    if (sl_abs_art_have(item)) abs_art_img(c, item, size);
    else if (abs_st.online && abs_npend < (int)ARRAYLEN(abs_pend))
    {
        struct abs_pend *p = &abs_pend[abs_npend++];
        p->box = c;
        snprintf(p->item, sizeof p->item, "%s", item);
        p->size = size;
        lv_obj_add_event_cb(c, abs_art_gone, LV_EVENT_DELETE, NULL);
        sl_abs_art_want(item);
    }
    return c;
}
static void abs_art_landed(void)
{
    for (int i = 0; i < abs_npend; )
    {
        if (sl_abs_art_have(abs_pend[i].item))
        {
            struct abs_pend p = abs_pend[i];
            abs_pend[i] = abs_pend[--abs_npend];
            lv_obj_remove_event_cb(p.box, abs_art_gone);
            abs_art_img(p.box, p.item, p.size);
        }
        else i++;
    }
}
/* how far, as a thin bar along a cover's foot */
static void abs_bar(lv_obj_t *art, int size, const struct sl_abs_item *it)
{
    if (!it->permille && !it->finished) return;
    int inset = size > 100 ? 12 : 6, h = size > 100 ? 6 : 4;
    lv_obj_t *tr = box(art);
    lv_obj_set_size(tr, size - 2 * inset, h);
    lv_obj_set_style_radius(tr, sl_zune() ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tr, LV_OPA_50, 0);
    lv_obj_align(tr, LV_ALIGN_BOTTOM_MID, 0, -inset);
    lv_obj_t *fill = box(tr);
    int pm = it->finished ? 1000 : it->permille;
    lv_obj_set_size(fill, LV_MAX(h, (size - 2 * inset) * pm / 1000), h);
    lv_obj_set_style_radius(fill, sl_zune() ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(it->finished ? 0x3aa856 : C_ACCENT), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    sl_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
}
/* on the card: a tick in the cover's corner */
static void abs_badge(lv_obj_t *art, int size, bool whole)
{
    lv_obj_t *d = box(art);
    int s = size > 100 ? 30 : 22;
    lv_obj_set_size(d, s, s);
    lv_obj_set_style_radius(d, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(whole ? C_ACCENT : 0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(d, whole ? LV_OPA_COVER : LV_OPA_70, 0);
    sl_elevate(d, SL_E1, 0);
    uint32_t ink = whole ? (sl_accent_ink() == 0xffffff ? 0xfefefe : sl_accent_ink()) : 0xfefefe;
    lv_obj_t *l = text(d, whole ? LV_SYMBOL_OK : LV_SYMBOL_DOWNLOAD, &sl_sym_16, ink);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    lv_obj_center(l);
    sl_align(d, LV_ALIGN_TOP_RIGHT, size > 100 ? -8 : -3, size > 100 ? 8 : 3);
}

/* ---- the pages: each a view of the worker's answers */
enum { AV_HOME = 100, AV_LIB };
struct abs_view {
    lv_obj_t *page, *body;
    int what, page_no, tab;               /* the request; a library's page; Titles, Authors, Series */
    unsigned req, req2;
    bool waiting, waiting2, podcast;
    char arg[64], arg2[160], title[96];
    struct sl_abs_list l, l2;             /* the home's: continue listening, the libraries */
    struct abs_view *next;
};
static struct abs_view *abs_views;
static lv_obj_t *abs_prog_lbl;            /* the book coming down, its percent */
static char abs_prog_item[100];
static bool abs_prog_pill;               /* it is the book page's Download pill */
static struct { char item[48], episode[48], text[200]; } abs_note;   /* the last place taken from the server */
static void abs_view_fill(struct abs_view *v);
static void abs_view_deleted(lv_event_t *e)
{
    struct abs_view *v = lv_event_get_user_data(e);
    for (struct abs_view **p = &abs_views; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; break; }
    if (v->waiting) sl_abs_forget_req(v->req);
    if (v->waiting2) sl_abs_forget_req(v->req2);
    if (v->what == SL_ABS_Q_ITEM && !strcmp(abs_note.item, v->arg)) abs_note.text[0] = abs_note.item[0] = '\0';   /* said once */
    sl_abs_list_free(&v->l);
    sl_abs_list_free(&v->l2);
    abs_prog_lbl = NULL;
    lv_free(v);
}
static void abs_ask(struct abs_view *v)
{
    if (v->waiting) sl_abs_forget_req(v->req);
    if (v->waiting2) sl_abs_forget_req(v->req2);
    v->waiting2 = false;
    if (v->what == AV_HOME)
    {
        struct sl_abs_status now;
        sl_abs_status(&now);              /* Wi-Fi as it is this moment */
        v->req = sl_abs_ask(now.online ? SL_ABS_Q_CONTINUE : SL_ABS_Q_LOCAL, "", "", 0);
        v->req2 = now.online ? sl_abs_ask(SL_ABS_Q_LIBRARIES, "", "", 0) : 0;
        v->waiting2 = v->req2 != 0;
    }
    else v->req = sl_abs_ask(v->what, v->arg, v->arg2, v->page_no);
    v->waiting = v->req != 0;
    if (!v->waiting) { sl_abs_list_free(&v->l); v->l.err = SL_ABS_E_NOMEM; }
}
static struct abs_view *abs_view_new(int what, const char *arg, const char *arg2, const char *title, lv_obj_t **pp)
{
    struct abs_view *v = lv_malloc(sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->what = what;
    snprintf(v->arg, sizeof v->arg, "%s", arg ? arg : "");
    snprintf(v->arg2, sizeof v->arg2, "%s", arg2 ? arg2 : "");
    snprintf(v->title, sizeof v->title, "%s", title ? title : "");
    lv_obj_t *p;
    v->body = page_new(v->title, &p);
    v->page = p;
    lv_obj_add_event_cb(p, abs_view_deleted, LV_EVENT_DELETE, v);
    v->next = abs_views;
    abs_views = v;
    if (pp) *pp = p;
    return v;
}
static void abs_refill(struct abs_view *v)
{
    int32_t y = lv_obj_get_scroll_y(v->body);
    abs_prog_lbl = NULL;
    lv_obj_clean(v->body);
    abs_view_fill(v);
    lv_obj_update_layout(v->body);
    lv_obj_scroll_to_y(v->body, y, LV_ANIM_OFF);
}
static struct abs_view *abs_view_of(lv_obj_t *o)
{
    for (struct abs_view *v = abs_views; v; v = v->next)
        for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p)) if (p == v->page) return v;
    return NULL;
}
static struct abs_view *abs_top_view(void)
{
    if (!npages) return NULL;
    for (struct abs_view *v = abs_views; v; v = v->next) if (v->page == pages[npages - 1]) return v;
    return NULL;
}
static void abs_retry_cb(lv_event_t *e)
{
    struct abs_view *v = lv_event_get_user_data(e);
    abs_ask(v);
    abs_refill(v);
}
static void abs_error(struct abs_view *v, int err, const char *why)
{
    char b[200];
    const char *how = abs_err_words(err);
    if (err == SL_ABS_E_SERVER && why && why[0]) { snprintf(b, sizeof b, "%s %s", sl_tr("The server said:"), why); how = b; }
    lv_obj_t *c = sl_empty(v->body, err == SL_ABS_E_OFFLINE ? IC_WIFI : IC_SERVER, 88,
                           err == SL_ABS_E_OFFLINE ? "Not on the card." : "Couldn't reach your books.", how, 56);
    if (err != SL_ABS_E_OFFLINE && err != SL_ABS_E_NOSERVER) sl_empty_action(c, LV_SYMBOL_REFRESH "  Try again", abs_retry_cb, v);
}

/* ---- rows and cells */
static void abs_open_item(const struct sl_abs_list *l, int k)
{
    const struct sl_abs_item *it = &l->it[k];
    abs_item_page(sl_abs_s(l, it->id), sl_abs_s(l, it->title));
}
static void abs_item_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    intptr_t u = (intptr_t)lv_event_get_user_data(e);
    if (!v) return;
    const struct sl_abs_list *l = u & 0x10000 ? &v->l2 : &v->l;
    int k = (int)(u & 0xffff);
    if (k < 0 || k >= l->n) return;
    abs_open_item(l, k);
}
/* a book's or a podcast's row: its cover (and how far along it), the title, who, where it is */
static lv_obj_t *abs_row(lv_obj_t *parent, const struct sl_abs_list *l, int k, intptr_t ud)
{
    const struct sl_abs_item *it = &l->it[k];
    const char *t = sl_abs_s(l, it->title);
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    int rh = sub_row_h();
    lv_obj_set_size(r, lv_pct(100), rh);
    bool pod = it->kind == SL_ABS_PODCAST || it->podcast;
    lv_obj_t *a = abs_art(r, sl_abs_s(l, it->id), t, SL_ART_ROW, pod);
    abs_bar(a, SL_ART_ROW, it);
    sl_align(a, LV_ALIGN_LEFT_MID, SL_ROW_ART_X, 0);
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (rh - sl_line_h(&ss_semibold_23) - sl_line_h(&ss_regular_18)) / 2 + SL_SP_XS / 2);
    int tw = W - 2 * SL_GUTTER - SL_ROW_TXT_X - SL_ROW_END - 30;
    /* a podcast in Continue listening: its episode is the title */
    const char *ep_t = sl_abs_s(l, it->desc);
    bool ep_line = pod && it->episode && ep_t[0];
    lv_obj_t *t1 = text(r, ep_line ? ep_t : t, &ss_semibold_23, 0xffffff);
    lv_obj_set_size(t1, tw, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    set_align(t1);
    sl_label_deep(t1);
    sl_place(t1, SL_ROW_TXT_X, ty);
    char b[200], s[80];
    b[0] = '\0';
    sub_join(b, sizeof b, ep_line ? t : sl_abs_s(l, it->author));
    abs_state(s, sizeof s, it);
    sub_join(b, sizeof b, s);
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    set_align(t2);
    sl_label_deep(t2);
    sl_place(t2, SL_ROW_TXT_X, ty + lh1);
    if (it->local)
    {
        lv_obj_t *m = text(r, it->local == 1 ? LV_SYMBOL_OK : LV_SYMBOL_DOWNLOAD, &sl_sym_16, it->local == 1 ? C_ACCENT_TXT : C_DIM);
        sl_align(m, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN - 30, 0);
    }
    lv_obj_t *ch = text(r, sl_ui_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    sl_align(ch, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN - 6, 0);
    on_click(r, abs_item_cb, (void *)ud);
    return r;
}
static lv_obj_t *abs_rows_box(lv_obj_t *body)
{
    lv_obj_t *rows = box(body);
    lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rows, SL_SP_XS, 0);
    return rows;
}
/* a library's books, two covers a row */
static void abs_grid(struct abs_view *v, lv_obj_t *body)
{
    lv_obj_t *g = box(body);
    lv_obj_set_width(g, lv_pct(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, 16, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(g, LV_BASE_DIR_RTL, 0);
    int cw = (W - 2 * SL_GUTTER - SL_GUTTER) / 2;
    int lh1 = lv_font_get_line_height(&ss_semibold_18), lh2 = lv_font_get_line_height(&ss_regular_16);
    for (int k = 0; k < v->l.n; k++)
    {
        const struct sl_abs_item *it = &v->l.it[k];
        const char *t = sl_abs_s(&v->l, it->title);
        lv_obj_t *cell = box(g);
        lv_obj_set_size(cell, cw, cw + 8 + lh1 + 2 + lh2 + 6);
        lv_obj_set_style_transform_pivot_x(cell, cw / 2, 0);
        lv_obj_set_style_transform_pivot_y(cell, cw / 2, 0);
        press_motion(cell);
        lv_obj_set_style_transform_scale(cell, 246, LV_STATE_PRESSED);
        lv_obj_t *a = abs_art(cell, sl_abs_s(&v->l, it->id), t, cw, it->kind == SL_ABS_PODCAST);
        sl_elevate(a, sl_zune() ? SL_E0 : SL_E1, 0);
        abs_bar(a, cw, it);
        if (it->local) abs_badge(a, cw, it->local == 1);
        lv_obj_t *l1 = text(cell, t, &ss_semibold_18, 0xffffff);
        lv_obj_set_size(l1, cw, lh1);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        set_align(l1);
        sl_label_deep(l1);
        lv_obj_set_pos(l1, 0, cw + 8);
        char b[160], s[64];
        b[0] = '\0';
        sub_join(b, sizeof b, sl_abs_s(&v->l, it->author));
        if (it->finished || it->permille) { abs_state(s, sizeof s, it); sub_join(b, sizeof b, s); }
        lv_obj_t *l2 = text(cell, b, &ss_regular_16, C_SUB);
        lv_obj_set_size(l2, cw, lh2);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        set_align(l2);
        sl_label_deep(l2);
        lv_obj_set_pos(l2, 0, cw + 8 + lh1 + 2);
        on_click(cell, abs_item_cb, (void *)(intptr_t)k);
    }
}

/* ---- the home: continue listening, the libraries, what is on the card */
static void abs_lib_page(const char *id, const char *name, bool podcast, int tab, const char *filter, int page_no, bool replace);
static void abs_local_page(void);
static void abs_lib_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || k < 0 || k >= v->l2.n) return;
    const struct sl_abs_item *it = &v->l2.it[k];
    abs_lib_page(sl_abs_s(&v->l2, it->id), sl_abs_s(&v->l2, it->title), it->podcast, 0, "", 0, false);
}
static void abs_link_cb(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e))
    {
        case 0: abs_local_page(); break;
        case 1: sub_servers_page(); break;
        case 2: abs_edit_page(); break;
    }
}
static void abs_loading(lv_obj_t *body) { sub_loading(body, "Asking the server"); }
static void abs_home_fill(struct abs_view *v)
{
    lv_obj_t *body = v->body;
    struct sl_abs_server s;
    if (!sl_abs_server(&s))
    {
        lv_obj_t *c = sl_empty(body, IC_LIBRARY, 96, "No Audiobookshelf yet.",
                               "Sign in to your Audiobookshelf server to bring its books to the card. Where you are in each stays the same on your phone and here.", 48);
        sl_empty_action(c, LV_SYMBOL_PLUS "  Sign in", abs_link_cb, (void *)2);
        return;
    }
    bool off = !abs_st.online;
    if (off)
    {
        lv_obj_t *g = set_group(body, NULL);
        set_info(g, LV_SYMBOL_WIFI, 0x66738a, "Offline", "Books on the card still play", true);
    }
    char hd[48];
    snprintf(hd, sizeof hd, "%s", sl_case(sl_tr(off ? "ON THE CARD" : "CONTINUE LISTENING")));
    section(body, hd);
    if (v->waiting) abs_loading(body);
    else if (v->l.err) abs_error(v, v->l.err, v->l.why);
    else if (!v->l.n) sl_empty(body, IC_LIBRARY, 64, off ? "Nothing on the card yet." : "Nothing started yet.",
                               off ? "Books you download stay here, to play without Wi-Fi." : "Books you're listening to on any device show here.", 12);
    else
    {
        lv_obj_t *rows = abs_rows_box(body);
        for (int i = 0; i < v->l.n && i < 6; i++) abs_row(rows, &v->l, i, i);
    }
    if (!off)
    {
        lv_obj_t *g = set_group(body, "Libraries");
        if (v->waiting2) set_info(g, LV_SYMBOL_REFRESH, 0x66738a, "Asking the server", NULL, false);
        else if (v->l2.err) set_info(g, LV_SYMBOL_WARNING, SET_RED, "Couldn't reach your books.", abs_err_words(v->l2.err), true);
        else if (!v->l2.n) set_info(g, LV_SYMBOL_LIST, 0x66738a, "No libraries yet.", NULL, false);
        for (int i = 0; !v->waiting2 && i < v->l2.n; i++)
        {
            const struct sl_abs_item *it = &v->l2.it[i];
            set_link(g, it->podcast ? LV_SYMBOL_AUDIO : LV_SYMBOL_LIST, it->podcast ? 0x8b6fd6 : ABS_TINT, sl_abs_s(&v->l2, it->title),
                     NULL, abs_lib_cb, (void *)(intptr_t)i);
        }
    }
    lv_obj_t *g = set_group(body, "On the card");
    char line[120], c[40];
    int n = sl_abs_books(NULL, 0);
    line[0] = '\0';
    sl_count(c, sizeof c, n, "%s book", "%s books");
    sub_join(line, sizeof line, c);
    if (abs_st.dl_item[0] && abs_st.dl_total > 0)
    {
        snprintf(c, sizeof c, "%s %d%%", sl_tr("Downloading"), (int)(abs_st.dl_got * 100 / abs_st.dl_total));
        sub_join(line, sizeof line, c);
    }
    else if (abs_st.queued && off) sub_join(line, sizeof line, sl_tr("Waiting for Wi-Fi"));
    set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x0aa9c7, "Downloaded", line, NULL, abs_link_cb, (void *)0);
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s", s.user, s.url);
    set_link_sub(g, LV_SYMBOL_SETTINGS, 0x66738a, "Music servers", line, NULL, abs_link_cb, (void *)1);
}
static lv_obj_t *abs_home_obj;
static void abs_home_deleted(lv_event_t *e) { if (lv_event_get_current_target(e) == abs_home_obj) abs_home_obj = NULL; }
static void abs_page(void)
{
    pod_fs_init();
    if (page_bring_back(abs_home_obj)) return;
    struct sl_abs_server s;
    bool have = sl_abs_server(&s);
    lv_obj_t *p;
    struct abs_view *v = abs_view_new(AV_HOME, "", "", "Audiobookshelf", &p);
    if (!v) return;
    abs_home_obj = p;
    lv_obj_add_event_cb(p, abs_home_deleted, LV_EVENT_DELETE, NULL);
    if (have)
    {
        header_button(p, LV_SYMBOL_SETTINGS, abs_link_cb, (void *)1);
        abs_ask(v);
    }
    abs_view_fill(v);
    page_push(p);
}

/* ---- a library: its titles (a page at a time), its authors, its series */
static void abs_tab_pick(int k)
{
    struct abs_view *v = abs_top_view();
    if (!v || v->what == SL_ABS_Q_ITEM || k == v->tab) return;
    char id[64], t[96];
    snprintf(id, sizeof id, "%s", v->arg);
    snprintf(t, sizeof t, "%s", v->title);
    abs_lib_page(id, t, false, k, "", 0, true);
}
static void abs_pager_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    if (!v) return;
    char id[64], t[96], f[160];
    snprintf(id, sizeof id, "%s", v->arg);
    snprintf(t, sizeof t, "%s", v->title);
    snprintf(f, sizeof f, "%s", v->arg2);
    abs_lib_page(id, t, v->podcast, v->tab, f, (int)(intptr_t)lv_event_get_user_data(e), true);
}
static void abs_group_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || k < 0 || k >= v->l.n) return;
    const struct sl_abs_item *it = &v->l.it[k];
    char f[160], id[64];
    abs_filter(f, sizeof f, it->kind == SL_ABS_AUTHOR ? "authors" : "series", sl_abs_s(&v->l, it->id));
    snprintf(id, sizeof id, "%s", v->arg);
    abs_lib_page(id, sl_abs_s(&v->l, it->title), false, 0, f, 0, false);
}
static void abs_lib_fill(struct abs_view *v)
{
    lv_obj_t *body = v->body;
    if (!v->podcast && !v->arg2[0])
    {
        lv_obj_t *g = set_group(body, NULL);
        static const char *const tabs[] = { "Titles", "Authors", "Series" };
        opt_chips(g, tabs, 3, v->tab, abs_tab_pick);
    }
    if (v->waiting) { abs_loading(body); return; }
    if (v->l.err) { abs_error(v, v->l.err, v->l.why); return; }
    if (!v->l.n)
    {
        sl_empty(body, IC_LIBRARY, 72, v->tab == 1 ? "No authors here." : v->tab == 2 ? "No series here." : "Nothing in this library yet.",
                 "Add books on the server and they show here.", 24);
        return;
    }
    if (v->tab == 0)
    {
        int total = v->l.total > v->l.n ? v->l.total : v->l.n + v->page_no * SL_ABS_PAGE_N, start = v->page_no * SL_ABS_PAGE_N;
        char b[80];
        if (total > SL_ABS_PAGE_N) snprintf(b, sizeof b, sl_tr("BOOKS %d\xe2\x80\x93%d OF %d"), start + 1, start + v->l.n, total);
        else snprintf(b, sizeof b, sl_tr(v->l.n == 1 ? "%d BOOK" : "%d BOOKS"), v->l.n);
        section(body, b);
        abs_grid(v, body);
        if (v->page_no || start + v->l.n < total)
        {
            lv_obj_t *bar = sl_pager(body);
            if (v->page_no) sl_pager_button(bar, LV_SYMBOL_LEFT "  Previous", false, abs_pager_cb, v->page_no - 1);
            if (start + v->l.n < total) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, abs_pager_cb, v->page_no + 1);
        }
        return;
    }
    lv_obj_t *g = set_group(body, NULL);
    for (int i = 0; i < v->l.n; i++)
    {
        const struct sl_abs_item *it = &v->l.it[i];
        char c[40];
        sl_count(c, sizeof c, it->count, "%s book", "%s books");
        set_link_sub(g, v->tab == 1 ? LV_SYMBOL_EDIT : LV_SYMBOL_COPY, v->tab == 1 ? 0xeb743b : 0x8b6fd6, sl_abs_s(&v->l, it->title),
                     it->count ? c : NULL, NULL, abs_group_cb, (void *)(intptr_t)i);
    }
}
static void abs_lib_page(const char *id, const char *name, bool podcast, int tab, const char *filter, int page_no, bool replace)
{
    int what = tab == 1 ? SL_ABS_Q_AUTHORS : tab == 2 ? SL_ABS_Q_SERIES : SL_ABS_Q_ITEMS;
    lv_obj_t *p;
    struct abs_view *v = abs_view_new(what, id, filter, name, &p);
    if (!v) return;
    v->tab = tab;
    v->page_no = page_no;
    v->podcast = podcast;
    abs_ask(v);
    abs_view_fill(v);
    if (replace) page_replace(p, true); else page_push(p);
}

/* ---- what is on the card */
static void abs_local_fill(struct abs_view *v)
{
    if (v->waiting) { abs_loading(v->body); return; }
    if (v->l.err) { abs_error(v, v->l.err, v->l.why); return; }
    if (!v->l.n)
    {
        sl_empty(v->body, IC_LIBRARY, 72, "Nothing on the card yet.", "Books you download stay here, to play without Wi-Fi.", 24);
        return;
    }
    long long size = 0, room = 0;
    for (int i = 0; i < v->l.n; i++) size += v->l.it[i].size;
    char b[120], u[24], r[24];
    sub_size(u, sizeof u, size);
    if (sl_abs_card_free(&room)) { sub_size(r, sizeof r, room); snprintf(b, sizeof b, sl_tr("%s on the card, %s free"), u, r); }
    else snprintf(b, sizeof b, "%s", u);
    section(v->body, b);
    lv_obj_t *rows = abs_rows_box(v->body);
    for (int i = 0; i < v->l.n; i++) abs_row(rows, &v->l, i, i);
}
static void abs_local_page(void)
{
    lv_obj_t *p;
    struct abs_view *v = abs_view_new(SL_ABS_Q_LOCAL, "", "", "Downloaded", &p);
    if (!v) return;
    abs_ask(v);
    abs_view_fill(v);
    page_push(p);
}

/* ---- playing a book from the card: it is our audiobooks' own */
static int abs_book_index(const struct sl_abs_book *k)
{
    size_t n = strlen(k->dir);
    for (int i = 0; i < ab_db_count(); i++)
    {
        const char *p = ab_db_str(ab_db_book(i), AB_S_PATH);
        if (!strcasecmp(p, k->dir) || (!strncasecmp(p, k->dir, n) && p[n] == '/')) return i;
    }
    return -1;
}
static void abs_rescan(void)
{
    audiobooks_init();
    ab_lock();
    ab_scan_silent = true;
    ab_db_scan(false, true);
    ab_scan_silent = false;
    ab_unlock();
}
/* the book on the card for an item, its index in our audiobooks (read again once if it is new) */
static int abs_local_index(const char *item, const char *ep, struct sl_abs_book *k)
{
    if (!sl_abs_book(item, ep, k) || !k->whole) return -1;
    audiobooks_init();
    int i = abs_book_index(k);
    if (i < 0) { abs_rescan(); i = abs_book_index(k); }
    return i;
}
/* a place in the book (ms) as its file and the ms into it */
static void abs_where(const struct ab_book *b, uint32_t at, int *file, uint32_t *ms)
{
    const uint32_t *len = ab_db_lengths(b);
    *file = 0;
    *ms = at;
    for (int i = 0; len && i + 1 < b->nfiles && *ms >= len[i]; i++) { *ms -= len[i]; *file = i + 1; }
}
static bool abs_play(const char *item, const char *ep, int64_t at_ms)
{
    struct sl_abs_book k;
    int i = abs_local_index(item, ep, &k);
    if (i < 0) { toast_warn("Could not play this book"); return false; }
    if (at_ms < 0) { bk_play(i, false); return true; }
    int file;
    uint32_t ms;
    abs_where(ab_db_book(i), (uint32_t)at_ms, &file, &ms);
    if (audiobooks_play_at(i, file, ms)) np_show_auto();
    else toast_warn("Could not play this book");
    return true;
}

/* ---- a book's (or a podcast's) page */
static void abs_download_go(const char *item, const char *ep)
{
    if (!sl_abs_download(item, ep)) { toast_warn("Couldn't change that now"); return; }
    toast(abs_wifi_up() ? "Downloading to the card" : "It downloads when Wi-Fi is on");
}
static struct { char item[48], ep[48]; } abs_ask_dl;
static void abs_dl_sheet_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    if (k == 1) abs_download_go(abs_ask_dl.item, abs_ask_dl.ep);
}
/* Download: at once, or after a word on its size when it is big */
static void abs_download(const char *item, const char *ep, const char *title, long long size)
{
    if (!abs_wifi_up()) { toast("Join a Wi-Fi network first"); return; }
    long long room = 0;
    bool known = sl_abs_card_free(&room);
    if (known && size > 0 && size + 64LL * 1024 * 1024 > room) { toast_warn("There isn't enough room on the card for this book."); return; }
    if (size <= (long long)SL_ABS_BIG_MB * 1024 * 1024) { abs_download_go(item, ep); return; }
    snprintf(abs_ask_dl.item, sizeof abs_ask_dl.item, "%s", item);
    snprintf(abs_ask_dl.ep, sizeof abs_ask_dl.ep, "%s", ep);
    acts_begin();
    lv_obj_t *t = text(acts, title, &ss_semibold_23, 0xffffff);
    lv_obj_set_size(t, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    sl_label_deep(t);
    lv_obj_set_style_pad_hor(t, 16, 0);
    lv_obj_set_style_pad_top(t, 8, 0);
    lv_obj_set_style_pad_bottom(t, SL_SP_XS, 0);
    char b[160], s[24], r[24];
    sub_size(s, sizeof s, size);
    if (known) { sub_size(r, sizeof r, room); snprintf(b, sizeof b, sl_tr("%s to download, %s free on the card"), s, r); }
    else snprintf(b, sizeof b, sl_tr("%s to download"), s);
    lv_obj_t *l = text(acts, b, &ss_regular_16, C_SUB);
    lv_obj_set_size(l, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(l, 16, 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    acts_row_cb(LV_SYMBOL_DOWNLOAD, "Download", abs_dl_sheet_cb, 1);
    acts_row_cb(LV_SYMBOL_CLOSE, "Not now", abs_dl_sheet_cb, 0);
    acts_show();
}
static bool abs_is_coming(const char *item, const char *ep)
{
    return abs_st.dl_item[0] && !strcmp(abs_st.dl_item, item) && !strcmp(abs_st.dl_episode, ep);
}
static void abs_pct(char *b, size_t n)
{
    snprintf(b, n, "%s %d%%", sl_tr("Downloading"), abs_st.dl_total > 0 ? (int)(abs_st.dl_got * 100 / abs_st.dl_total) : 0);
}
static void abs_main_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    if (!v || v->l.err) return;
    const struct sl_abs_item *h = &v->l.head;
    const char *id = sl_abs_s(&v->l, h->id);
    struct sl_abs_book k;
    if (sl_abs_book(id, "", &k) && k.whole) { abs_play(id, "", -1); return; }
    if (abs_is_coming(id, "") || (sl_abs_book(id, "", &k) && !k.whole)) { toast(abs_wifi_up() ? "Downloading to the card" : "It downloads when Wi-Fi is on"); return; }
    abs_download(id, "", sl_abs_s(&v->l, h->title), h->size);
}
static void abs_remove_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    if (!v) return;
    const char *ep = (const char *)lv_event_get_user_data(e);
    if (!sl_abs_remove(sl_abs_s(&v->l, v->l.head.id), ep ? ep : "")) { toast_warn("Couldn't change that now"); return; }
    toast("Removed from the card");
}
/* finished, or not: on the server too, and here when the book is on the card */
static void abs_finished_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    if (!v || v->l.err) return;
    struct sl_abs_item *h = &v->l.head;
    const char *id = sl_abs_s(&v->l, h->id);
    bool fin = !h->finished;
    uint32_t cur = fin ? h->dur_ms : 0;
    struct sl_abs_book k;
    int i = abs_local_index(id, "", &k);
    if (i >= 0)
    {
        struct ab_progress *p = ab_progress_touch(ab_db_book(i)->id);
        p->finished = fin;
        if (!fin) { p->position = 0; p->file = 0; p->elapsed = 0; }
        ab_progress_dirty();
        ab_progress_save();
    }
    sl_abs_push(id, "", cur, h->dur_ms, fin);
    h->finished = fin;
    h->permille = fin ? 1000 : 0;
    h->cur_ms = cur;
    toast(fin ? "Marked as finished" : "Marked as not finished");
    abs_refill(v);
}
static void abs_chapter_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || k < 0 || k >= v->l.n) return;
    const char *id = sl_abs_s(&v->l, v->l.head.id);
    struct sl_abs_book b;
    if (!sl_abs_book(id, "", &b) || !b.whole) { toast("Download it to play it here"); return; }
    abs_play(id, "", v->l.it[k].start_ms);
}
static void abs_episode_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || k < 0 || k >= v->l.n) return;
    const struct sl_abs_item *it = &v->l.it[k];
    const char *id = sl_abs_s(&v->l, v->l.head.id), *ep = sl_abs_s(&v->l, it->episode);
    struct sl_abs_book b;
    if (sl_abs_book(id, ep, &b) && b.whole) { abs_play(id, ep, -1); return; }
    if (sl_abs_book(id, ep, &b) || abs_is_coming(id, ep)) { toast(abs_wifi_up() ? "Downloading to the card" : "It downloads when Wi-Fi is on"); return; }
    if (v->l.offline) { toast("Join a Wi-Fi network first"); return; }
    abs_download(id, ep, sl_abs_s(&v->l, it->title), it->size);
}
static void abs_ep_remove_cb(lv_event_t *e)
{
    struct abs_view *v = abs_view_of(lv_event_get_current_target(e));
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || k < 0 || k >= v->l.n) return;
    if (!sl_abs_remove(sl_abs_s(&v->l, v->l.head.id), sl_abs_s(&v->l, v->l.it[k].episode))) { toast_warn("Couldn't change that now"); return; }
    toast("Removed from the card");
}
/* a chapter's or an episode's row: its name, when it starts or how long, where it is */
static void abs_sub_row(lv_obj_t *rows, struct abs_view *v, int k, bool chapter, int num)
{
    const struct sl_abs_item *it = &v->l.it[k];
    lv_obj_t *r = box(rows);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    int rh = sub_row_h();
    lv_obj_set_size(r, lv_pct(100), rh);
    char n[8];
    snprintf(n, sizeof n, "%d", num);
    lv_obj_t *nl = text(r, n, &ss_regular_18, C_DIM);
    lv_obj_set_width(nl, 40);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    sl_align(nl, LV_ALIGN_LEFT_MID, 0, 0);
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (rh - sl_line_h(&ss_semibold_23) - sl_line_h(&ss_regular_18)) / 2 + SL_SP_XS / 2);
    int tw = W - 2 * SL_GUTTER - SL_ROW_NUM_X - SL_ROW_END - 70;
    const char *t = sl_abs_s(&v->l, it->title);
    char tb[40];
    if (!t[0]) { snprintf(tb, sizeof tb, sl_tr("Chapter %d"), num); t = tb; }
    lv_obj_t *t1 = text(r, t, &ss_semibold_23, 0xffffff);
    lv_obj_set_size(t1, tw, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    set_align(t1);
    sl_label_deep(t1);
    sl_place(t1, SL_ROW_NUM_X, ty);
    char b[120], c[32];
    b[0] = '\0';
    if (chapter) { fmt_time(c, sizeof c, (long)it->start_ms); sub_join(b, sizeof b, c); }
    if (it->dur_ms) { bk_dur(c, sizeof c, it->dur_ms); sub_join(b, sizeof b, c); }
    if (!chapter && it->size) { sub_size(c, sizeof c, it->size); sub_join(b, sizeof b, c); }
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    set_align(t2);
    sl_label_deep(t2);
    sl_place(t2, SL_ROW_NUM_X, ty + lh1);
    if (!chapter)
    {
        const char *id = sl_abs_s(&v->l, v->l.head.id), *ep = sl_abs_s(&v->l, it->episode);
        struct sl_abs_book bk;
        bool have = sl_abs_book(id, ep, &bk), coming = abs_is_coming(id, ep);
        lv_obj_t *m;
        if (coming)
        {
            char pc[16];
            snprintf(pc, sizeof pc, "%d%%", abs_st.dl_total > 0 ? (int)(abs_st.dl_got * 100 / abs_st.dl_total) : 0);
            m = text(r, pc, &ss_semibold_18, C_ACCENT_TXT);
            abs_prog_lbl = m;
            abs_prog_pill = false;
            snprintf(abs_prog_item, sizeof abs_prog_item, "%s/%s", id, ep);
        }
        else m = text(r, have && bk.whole ? LV_SYMBOL_PLAY : LV_SYMBOL_DOWNLOAD, &sl_sym_16, have ? C_ACCENT_TXT : C_DIM);
        sl_align(m, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN - 14, 0);
        on_click(r, abs_episode_cb, (void *)(intptr_t)k);
        if (have) lv_obj_add_event_cb(r, abs_ep_remove_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
    }
    else on_click(r, abs_chapter_cb, (void *)(intptr_t)k);
}
static void abs_item_fill(struct abs_view *v)
{
    lv_obj_t *body = v->body;
    if (v->waiting) { abs_loading(body); return; }
    if (v->l.err) { abs_error(v, v->l.err, v->l.why); return; }
    const struct sl_abs_item *h = &v->l.head;
    const char *id = sl_abs_s(&v->l, h->id), *title = h->title ? sl_abs_s(&v->l, h->title) : v->title;
    bool pod = h->kind == SL_ABS_PODCAST;
    struct sl_abs_book k;
    bool have = !pod && sl_abs_book(id, "", &k), whole = have && k.whole, coming = !pod && abs_is_coming(id, "");
    /* on the card, the place is ours (the server's newer one was taken as it came) */
    struct sl_abs_item mine = *h;
    int bi = whole ? (audiobooks_init(), abs_book_index(&k)) : -1;
    const struct ab_progress *ap = bi >= 0 ? ab_progress_get(ab_db_book(bi)->id) : NULL;
    if (bi >= 0)
    {
        mine.cur_ms = ap ? ap->position : 0;
        mine.finished = ap && ap->finished;
        mine.permille = mine.dur_ms ? (uint16_t)LV_MIN(1000, (uint64_t)mine.cur_ms * 1000 / mine.dur_ms) : 0;
        h = &mine;
    }
    lv_obj_t *head = box(body);
    lv_obj_set_size(head, W, LV_SIZE_CONTENT);
    lv_obj_set_style_translate_x(head, -SL_GUTTER, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(head, 6, 0);
    lv_obj_set_style_pad_bottom(head, 14, 0);
    lv_obj_set_style_pad_hor(head, 28, 0);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_t *c = abs_art(head, id, title, SL_ABS_ART_BIG, pod);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    sl_elevate(c, sl_zune() ? SL_E0 : SL_E3, 0);
    if (!pod) abs_bar(c, SL_ABS_ART_BIG, h);
    lv_obj_t *t = text(head, title, sl_zune() ? sl_zune_font(title, &ss_regular_34) : &ss_bold_28, 0xffffff);
    lv_obj_set_width(t, W - 56);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    note_lines(t, title, W - 56, 2);
    sl_label_deep(t);
    const char *who = sl_abs_s(&v->l, h->author);
    if (who[0])
    {
        lv_obj_t *ar = text(head, who, &ss_semibold_18, C_SUB);
        lv_obj_set_width(ar, W - 56);
        lv_obj_set_style_text_align(ar, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(ar, 1);
    }
    char b[200], c1[64];
    b[0] = '\0';
    int chapters = 0, eps = 0;
    for (int i = 0; i < v->l.n; i++) { chapters += v->l.it[i].kind == SL_ABS_CHAPTER; eps += v->l.it[i].kind == SL_ABS_EPISODE; }
    if (!pod && h->dur_ms) { bk_dur(c1, sizeof c1, h->dur_ms); sub_join(b, sizeof b, c1); }
    if (chapters) { sl_count(c1, sizeof c1, chapters, "%s chapter", "%s chapters"); sub_join(b, sizeof b, c1); }
    if (eps) { sl_count(c1, sizeof c1, eps, "%s episode", "%s episodes"); sub_join(b, sizeof b, c1); }
    if (!pod && h->size) { sub_size(c1, sizeof c1, h->size); sub_join(b, sizeof b, c1); }
    if (b[0])
    {
        lv_obj_t *m = text(head, b, &ss_regular_16, C_SUB);
        lv_obj_set_width(m, W - 56);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(m, 1);
    }
    const char *nar = sl_abs_s(&v->l, h->narrator);
    if (nar[0] && !pod)
    {
        snprintf(b, sizeof b, sl_tr("Read by %s"), nar);
        lv_obj_t *m = text(head, b, &ss_regular_16, C_SUB);
        lv_obj_set_width(m, W - 56);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(m, 1);
    }
    /* the place taken from the server, said once where it shows */
    if (abs_note.text[0] && !strcmp(abs_note.item, id))
    {
        lv_obj_t *m = text(head, abs_note.text, &ss_semibold_18, C_ACCENT_TXT);
        lv_obj_set_width(m, W - 56);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_bottom(m, SL_SP_XS, 0);
    }
    if (!pod)
    {
        lv_obj_t *pl = box(body);
        lv_obj_set_size(pl, lv_pct(100), 72);
        lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_hor(pl, 6, 0);
        lv_obj_set_style_pad_top(pl, 4, 0);
        if (sl_ui_rtl()) lv_obj_set_style_base_dir(pl, LV_BASE_DIR_RTL, 0);
        char lab[80], st[64];
        if (whole) snprintf(lab, sizeof lab, LV_SYMBOL_PLAY "  %s", sl_tr(h->permille && !h->finished ? "Resume" : "Play"));
        else if (coming || have) { abs_pct(st, sizeof st); snprintf(lab, sizeof lab, LV_SYMBOL_DOWNLOAD "  %s", coming ? st : sl_tr("Waiting to download")); }
        else if (v->l.offline) snprintf(lab, sizeof lab, LV_SYMBOL_WIFI "  %s", sl_tr("Not on the card"));
        else if (h->size) { sub_size(st, sizeof st, h->size); snprintf(lab, sizeof lab, LV_SYMBOL_DOWNLOAD "  %s  \xc2\xb7  %s", sl_tr("Download"), st); }
        else snprintf(lab, sizeof lab, LV_SYMBOL_DOWNLOAD "  %s", sl_tr("Download"));
        lv_obj_t *go = mn_pill(pl, lab, 10, SL_PILL_H, whole || (!coming && !have && !v->l.offline), abs_main_cb, NULL);
        lv_obj_set_flex_grow(go, 1);
        lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
        if (coming)
        {
            abs_prog_lbl = lv_obj_get_child(go, 0);
            abs_prog_pill = true;
            snprintf(abs_prog_item, sizeof abs_prog_item, "%s/", id);
        }
        char sl[100];
        abs_state(sl, sizeof sl, h);
        lv_obj_t *g = set_group(body, "Your place");
        set_info(g, h->finished ? LV_SYMBOL_OK : LV_SYMBOL_PLAY, h->finished ? 0x3aa856 : 0x2d7ff0, h->finished ? "Finished" : h->permille ? "In progress" : "Not started",
                 sl, true);
        if (!v->l.offline) set_action(g, LV_SYMBOL_OK, 0x3aa856, h->finished ? "Mark as not finished" : "Mark as finished", NULL, abs_finished_cb, NULL);
        if (have)
        {
            g = set_group(body, "On the card");
            char line[96];
            sub_size(line, sizeof line, k.size);
            set_info(g, LV_SYMBOL_DOWNLOAD, 0x08869f, whole ? "Downloaded" : "Downloading", line, true);
            set_danger(g, LV_SYMBOL_TRASH, "Remove download", abs_remove_cb, NULL);
        }
    }
    const char *desc = sl_abs_s(&v->l, h->desc);
    if (desc[0])
    {
        lv_obj_t *g = set_group(body, "About");
        lv_obj_t *d = set_text(g, desc, C_SUB);
        (void)d;
    }
    char hd[48];
    if (chapters)
    {
        sl_count(hd, sizeof hd, chapters, "%s CHAPTER", "%s CHAPTERS");
        section(body, hd);
        lv_obj_t *rows = abs_rows_box(body);
        for (int i = 0, n = 0; i < v->l.n; i++) if (v->l.it[i].kind == SL_ABS_CHAPTER) abs_sub_row(rows, v, i, true, ++n);
    }
    if (pod)
    {
        section(body, sl_case(sl_tr("EPISODES")));
        if (!eps) sl_empty(body, IC_PODCAST, 64, "No episodes here.", "The server has none for this podcast yet.", 12);
        lv_obj_t *rows = abs_rows_box(body);
        for (int i = 0, n = 0; i < v->l.n; i++) if (v->l.it[i].kind == SL_ABS_EPISODE) abs_sub_row(rows, v, i, false, ++n);
    }
}
static void abs_view_fill(struct abs_view *v)
{
    if (v->what == AV_HOME) abs_home_fill(v);
    else if (v->what == SL_ABS_Q_ITEM) abs_item_fill(v);
    else if (v->what == SL_ABS_Q_LOCAL) abs_local_fill(v);
    else abs_lib_fill(v);
}
static void abs_item_page(const char *id, const char *title)
{
    lv_obj_t *p;
    struct abs_view *v = abs_view_new(SL_ABS_Q_ITEM, id, "", "", &p);
    if (!v) return;
    snprintf(v->title, sizeof v->title, "%s", title ? title : "");
    abs_ask(v);
    abs_view_fill(v);
    page_push(p);
}

/* ---- the place, both ways */
static struct {
    char path[300];
    bool linked, playing, paused, fin;
    struct sl_abs_book bk;
    int book, file, chap, nch;
    uint32_t pos, dur;
    long pushed_at, tick;                 /* current_tick's */
    uint32_t starts[SL_ABS_CH_MAX];
} abs_at;
static int abs_chap_of(uint32_t pos)
{
    int c = 0;
    for (int i = 0; i < abs_at.nch; i++) if (abs_at.starts[i] <= pos) c = i;
    return c;
}
static void abs_tell(void)
{
    if (!abs_at.linked) return;
    sl_abs_push(abs_at.bk.item, abs_at.bk.episode, abs_at.pos, abs_at.dur, abs_at.fin);
    abs_at.pushed_at = current_tick;
}
/* once a second: the book playing, if it is one of the server's, and the moments its place goes */
static void abs_tick(void)
{
    int status = audio_status();
    bool playing = status & AUDIO_STATUS_PLAY, paused = status & AUDIO_STATUS_PAUSE;
    struct mp3entry *id3 = playing ? audio_current_track() : NULL;
    const char *path = id3 && id3->path[0] ? id3->path : "";
    if (strcmp(path, abs_at.path))
    {
        /* another track: the old book's place goes first */
        if (abs_at.linked && abs_at.playing) abs_tell();
        strlcpy(abs_at.path, path, sizeof abs_at.path);
        bool was = abs_at.linked;
        struct sl_abs_book old = abs_at.bk;
        abs_at.linked = path[0] && sl_abs_book_at(path, &abs_at.bk);
        if (abs_at.linked && (!was || strcmp(old.item, abs_at.bk.item) || strcmp(old.episode, abs_at.bk.episode)))
        {
            audiobooks_init();
            abs_at.book = abs_book_index(&abs_at.bk);
            abs_at.nch = sl_abs_chapters(abs_at.bk.item, abs_at.bk.episode, abs_at.starts, SL_ABS_CH_MAX);
            abs_at.chap = -1;
            abs_at.file = -1;
            abs_at.fin = false;
            abs_at.pushed_at = current_tick;
        }
        if (abs_at.linked && abs_at.book < 0) abs_at.linked = false;
        abs_at.playing = false;
    }
    if (!abs_at.linked) { abs_at.playing = false; return; }
    const struct ab_book *b = ab_db_book(abs_at.book);
    struct ab_progress *p = b ? ab_progress_get(b->id) : NULL;
    if (!p) return;
    bool going = playing && !paused;
    abs_at.dur = abs_at.bk.dur_ms ? abs_at.bk.dur_ms : b->length;
    abs_at.pos = p->position;
    bool tell = false;
    int ch = abs_chap_of(abs_at.pos);
    if (abs_at.file >= 0 && p->file != abs_at.file) tell = true;               /* a file of its own: a chapter */
    if (abs_at.chap >= 0 && ch != abs_at.chap && abs_at.nch > 1) tell = true;   /* a chapter in the file */
    if (p->finished && !abs_at.fin) tell = true;                               /* the end */
    if (abs_at.playing && !going) tell = true;                                 /* paused or stopped */
    if (going && TIME_AFTER(current_tick, abs_at.pushed_at + (long)abs_every_s * HZ)) tell = true;
    abs_at.file = p->file;
    abs_at.chap = ch;
    abs_at.fin = p->finished;
    if (tell && (abs_at.playing || going || p->finished)) abs_tell();
    abs_at.playing = going;
}
/* the server's newer place, taken here (not over the book playing now: that one is told instead) */
static void abs_take_pick(const struct sl_abs_pick *pk)
{
    struct sl_abs_book k;
    if (!sl_abs_book(pk->item, pk->episode, &k) || !k.whole) return;
    if (abs_at.linked && abs_at.playing && !strcmp(abs_at.bk.item, k.item) && !strcmp(abs_at.bk.episode, k.episode)) return;
    audiobooks_init();
    int i = abs_book_index(&k);
    if (i < 0) return;
    const struct ab_book *b = ab_db_book(i);
    struct ab_progress *p = ab_progress_touch(b->id);
    int file;
    uint32_t ms;
    abs_where(b, pk->cur_ms, &file, &ms);
    p->file = (uint16_t)file;
    p->elapsed = ms;
    p->position = pk->cur_ms;
    p->finished = pk->finished;
    p->last_played = ab_now();
    ab_progress_dirty();
    ab_progress_save();
    snprintf(abs_note.item, sizeof abs_note.item, "%s", pk->item);
    snprintf(abs_note.episode, sizeof abs_note.episode, "%s", pk->episode);
    if (pk->device[0]) snprintf(abs_note.text, sizeof abs_note.text, sl_tr("Picked up where you left off on %s"), pk->device);
    else
    {
        time_t t = (time_t)(pk->updated / 1000);
        struct tm tm;
        char when[32];
        localtime_r(&t, &tm);
        time_t now = time(NULL);
        if (now - t < 20 * 3600 && t - now < 20 * 3600) snprintf(when, sizeof when, "%02d:%02d", tm.tm_hour, tm.tm_min);
        else snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        snprintf(abs_note.text, sizeof abs_note.text, sl_tr("Picked up where you left off at %s"), when);
    }
    /* said where it shows: on the book's page, or in a word */
    bool shown = false;
    for (struct abs_view *v = abs_views; v; v = v->next)
        if (v->what == SL_ABS_Q_ITEM && !strcmp(v->arg, pk->item))
        {
            shown = true;
            v->l.head.cur_ms = pk->cur_ms;
            v->l.head.finished = pk->finished;
            if (v->l.head.dur_ms) v->l.head.permille = (uint16_t)LV_MIN(1000, (uint64_t)pk->cur_ms * 1000 / v->l.head.dur_ms);
            if (!v->waiting) abs_refill(v);
        }
    if (!shown) toast(abs_note.text);
}

/* ---- Settings > Music servers: the Audiobookshelf part, and signing in */
static lv_obj_t *abs_edit_obj, *abs_url_ta, *abs_user_ta, *abs_pass_ta, *abs_test_line;
static bool abs_want_test;
static unsigned abs_test_from;
static void abs_srv_cb(lv_event_t *e) { (void)e; abs_edit_page(); }
static void abs_srv_group(lv_obj_t *body)
{
    struct sl_abs_server s;
    lv_obj_t *g = set_group(body, "Audiobookshelf");
    if (sl_abs_server(&s))
    {
        char line[260];
        snprintf(line, sizeof line, "%s  \xc2\xb7  %s", s.user, s.url);
        set_link_sub(g, LV_SYMBOL_OK, 0x3aa856, s.name, line, NULL, abs_srv_cb, NULL);
    }
    else set_link_sub(g, LV_SYMBOL_PLUS, ABS_TINT, "Sign in to Audiobookshelf", "Your audiobooks and podcasts, your place kept in step", NULL, abs_srv_cb, NULL);
}
static void abs_edit_focus(lv_obj_t *ta)
{
    lv_obj_t *all[3] = { abs_url_ta, abs_user_ta, abs_pass_ta };
    for (int i = 0; i < 3; i++) if (all[i] && all[i] != ta) lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (abs_edit_obj) kb_retarget(kb_of_page(abs_edit_obj), ta);
}
static void abs_edit_field_cb(lv_event_t *e) { abs_edit_focus(lv_event_get_current_target(e)); }
static void abs_edit_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != abs_edit_obj) return;
    if (abs_pass_ta) lv_textarea_set_text(abs_pass_ta, "");
    abs_edit_obj = abs_url_ta = abs_user_ta = abs_pass_ta = abs_test_line = NULL;
    abs_want_test = false;
}
static lv_obj_t *abs_edit_field(lv_obj_t *g, const char *hint)
{
    lv_obj_t *ta = sub_edit_field(g, hint);
    lv_obj_remove_event_cb(ta, sub_edit_field_cb);
    lv_obj_add_event_cb(ta, abs_edit_field_cb, LV_EVENT_PRESSED, NULL);
    return ta;
}
static void abs_test_say(const char *s, uint32_t ink)
{
    if (!abs_test_line) return;
    lv_label_set_text(abs_test_line, sl_tr(s));
    lv_obj_set_style_text_color(abs_test_line, lv_color_hex(ink), 0);
}
static void abs_edit_go(bool save)
{
    if (!abs_url_ta) return;
    const char *u = sl_ta_text(abs_url_ta), *n = sl_ta_text(abs_user_ta), *pw = sl_ta_text(abs_pass_ta);
    struct sl_abs_server s;
    bool have = sl_abs_server(&s);
    if (!u[0]) { toast("Type the server's address"); abs_edit_focus(abs_url_ta); return; }
    if (!pw[0] && !have) { abs_edit_focus(abs_pass_ta); return; }
    if (!abs_wifi_up()) { toast("Join a Wi-Fi network first"); return; }
    char fixed[192];
    if (!sl_abs_url_fix(u, fixed, sizeof fixed)) { abs_test_say(abs_err_words(SL_ABS_E_ADDRESS), SET_RED); return; }
    abs_test_from = abs_st.test_seq;
    if (!sl_abs_signin(u, n, pw, save)) { toast_warn("Couldn't try now"); return; }
    abs_want_test = true;
    abs_test_say("Signing in\xe2\x80\xa6", C_SUB);
}
static void abs_test_cb(lv_event_t *e) { (void)e; abs_edit_go(false); }
static void abs_save_cb(lv_event_t *e) { (void)e; abs_edit_go(true); }
static void abs_forget_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_abs_remove_server()) { toast_warn("Couldn't change that now"); return; }
    toast("Server removed");
    page_pop();
}
static void abs_edit_page(void)
{
    if (page_bring_back(abs_edit_obj)) return;
    struct sl_abs_server s;
    memset(&s, 0, sizeof s);
    bool have = sl_abs_server(&s);
    lv_obj_t *p, *body = page_new(have ? s.name : "Audiobookshelf", &p);
    abs_edit_obj = p;
    lv_obj_add_event_cb(p, abs_edit_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_bottom(body, WF_KB_H + SL_GUTTER, 0);
    lv_obj_t *g = set_group(body, "ADDRESS");
    abs_url_ta = abs_edit_field(g, "abs.example.com or 192.168.1.5:13378");
    lv_textarea_set_max_length(abs_url_ta, 180);
    g = set_group(body, "USER NAME");
    abs_user_ta = abs_edit_field(g, "Empty to sign in with an API token");
    lv_textarea_set_max_length(abs_user_ta, 60);
    g = set_group(body, "PASSWORD OR API TOKEN");
    abs_pass_ta = abs_edit_field(g, have ? "Saved. Type to change it" : "Password");
    lv_textarea_set_password_mode(abs_pass_ta, true);
    lv_textarea_set_max_length(abs_pass_ta, 600);
    if (have) { lv_textarea_set_text(abs_url_ta, s.url); if (!s.api_token) lv_textarea_set_text(abs_user_ta, s.user); }
    abs_test_line = text(body, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(abs_test_line, lv_pct(100));
    lv_label_set_long_mode(abs_test_line, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(abs_test_line, SL_INSET, 0);
    lv_obj_set_style_pad_bottom(abs_test_line, SL_SP_S, 0);
    set_align(abs_test_line);
    if (have)
    {
        char b[160];
        snprintf(b, sizeof b, "Audiobookshelf %s%s%s", s.version, s.api_token ? "  \xc2\xb7  " : "", s.api_token ? sl_tr("API token") : "");
        lv_label_set_text(abs_test_line, b);
    }
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_OK, 0x3aa856, have ? "Save" : "Sign in", NULL, abs_save_cb, NULL);
    set_action(g, LV_SYMBOL_REFRESH, 0x2d7ff0, "Test connection", NULL, abs_test_cb, NULL);
    if (have) set_danger(g, LV_SYMBOL_TRASH, "Remove this server", abs_forget_cb, NULL);
    opt_note(body, "The password stays on the player, only to sign in again when the server asks. An API token (Settings > API Keys on the server) works instead. Over https the server's certificate isn't checked.");
    wf_kb_build(p, abs_url_ta, KB_PASSWORD, abs_save_cb, have ? "Save" : "Sign in");
    abs_edit_focus(have ? abs_pass_ta : abs_url_ta);
    page_push(p);
}

/* ---- Audiobooks' way in */
static void abs_open_cb(lv_event_t *e) { (void)e; abs_page(); }
static void abs_books_entry(lv_obj_t *body)
{
    struct sl_abs_server s;
    lv_obj_t *g = set_group(body, "Audiobookshelf");
    if (sl_abs_server(&s))
    {
        char line[120], c[40];
        line[0] = '\0';
        sl_count(c, sizeof c, sl_abs_books(NULL, 0), "%s book", "%s books");
        sub_join(line, sizeof line, s.name);
        sub_join(line, sizeof line, c);
        set_link_sub(g, LV_SYMBOL_DRIVE, ABS_TINT, "Your server", line, NULL, abs_open_cb, NULL);
    }
    else set_link_sub(g, LV_SYMBOL_DRIVE, ABS_TINT, "Connect Audiobookshelf", "Your server's books here, your place in step", NULL, abs_open_cb, NULL);
}

/* ---- the worker's news */
static void abs_poll(void)
{
    /* the audiobook folder, as the setting has it now */
    char f[64];
    snprintf(f, sizeof f, "%s", global_settings.audiobook_folder);
    size_t fl = strlen(f);
    while (fl > 1 && f[fl - 1] == '/') f[--fl] = '\0';
    if (fl > 1 && strcmp(f, sl_abs_folder)) snprintf(sl_abs_folder, sizeof sl_abs_folder, "%s", f);
    sl_abs_poll();
    struct sl_abs_status s;
    sl_abs_status(&s);
    if (!abs_at.tick || TIME_AFTER(current_tick, abs_at.tick + HZ)) { abs_at.tick = current_tick; abs_tick(); }
    struct sl_abs_pick pk;
    while (sl_abs_take_pick(&pk)) abs_take_pick(&pk);
    if (s.seq == abs_st.seq) return;
    struct sl_abs_status was = abs_st;
    abs_st = s;
    if (s.art_seq != was.art_seq) abs_art_landed();
    if (s.whole_seq != was.whole_seq) abs_rescan();   /* a book came whole or went: our audiobooks read the folder again */
    /* the book coming down, its percent */
    char now[100];
    snprintf(now, sizeof now, "%s/%s", s.dl_item, s.dl_episode);
    if (abs_prog_lbl && s.dl_item[0] && !strcmp(now, abs_prog_item))
    {
        char pc[64];
        if (abs_prog_pill)
        {
            abs_pct(pc, sizeof pc);
            char lab[80];
            snprintf(lab, sizeof lab, LV_SYMBOL_DOWNLOAD "  %s", pc);
            lv_label_set_text(abs_prog_lbl, sl_case(lab));
        }
        else
        {
            snprintf(pc, sizeof pc, "%d%%", s.dl_total > 0 ? (int)(s.dl_got * 100 / s.dl_total) : 0);
            lv_label_set_text(abs_prog_lbl, pc);
        }
    }
    bool card = s.books_seq != was.books_seq || strcmp(s.dl_item, was.dl_item) || strcmp(s.dl_episode, was.dl_episode) || s.online != was.online;
    for (struct abs_view *v = abs_views; v; v = v->next)
    {
        bool fill = false;
        if (s.list_seq != was.list_seq)
        {
            struct sl_abs_list l;
            if (v->waiting && sl_abs_take(v->req, &l)) { sl_abs_list_free(&v->l); v->l = l; v->waiting = false; fill = true; }
            if (v->waiting2 && sl_abs_take(v->req2, &l)) { sl_abs_list_free(&v->l2); v->l2 = l; v->waiting2 = false; fill = true; }
        }
        if (!fill && v->what == AV_HOME && (s.online != was.online || s.server_seq != was.server_seq)) { abs_ask(v); fill = true; }
        else if (!fill && card && !v->waiting && (v->what == SL_ABS_Q_ITEM || v->what == AV_HOME)) fill = true;
        else if (!fill && v->what == SL_ABS_Q_LOCAL && s.books_seq != was.books_seq) { abs_ask(v); fill = true; }
        if (fill) abs_refill(v);
    }
    if (sub_srv_body && s.server_seq != was.server_seq) sub_refill_srv(NULL);
    if (abs_want_test && s.test_seq != abs_test_from)
    {
        abs_want_test = false;
        if (!s.test_err)
        {
            char b[120];
            snprintf(b, sizeof b, "%s Audiobookshelf %s", sl_tr("Connected:"), s.test_version);
            abs_test_say(b, 0x3aa856);
            if (s.test_saved)
            {
                toast("Signed in");
                if (abs_edit_obj && npages && pages[npages - 1] == abs_edit_obj) page_pop();
                for (struct abs_view *v = abs_views; v; v = v->next)
                    if (v->what == AV_HOME) { abs_ask(v); abs_refill(v); }
            }
        }
        else
        {
            char b[220];
            if (s.test_err == SL_ABS_E_SERVER && s.test_why[0]) snprintf(b, sizeof b, "%s %s", sl_tr("The server said:"), s.test_why);
            else snprintf(b, sizeof b, "%s", sl_tr(abs_err_words(s.test_err)));
            abs_test_say(b, SET_RED);
        }
    }
    if (s.dl_seq != was.dl_seq && s.dl_err && s.dl_err != SL_ABS_E_NET) toast_warn(abs_err_words(s.dl_err));
}
