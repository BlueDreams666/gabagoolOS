/* A music server: Music > Server browses a Subsonic, OpenSubsonic or
 * Navidrome server (artists, albums by newest, recent, random or starred,
 * playlists, search, starred) and plays from it through the card: a song
 * comes down first (the next few while it plays), then Rockbox plays the
 * file. What came down stays as a cache, browsed here with no Wi-Fi;
 * albums and playlists can be kept offline for good. Plays go back to the
 * server (scrobble) through the listening history's hooks. The requests,
 * downloads and covers are sloppy_subjob.inc's worker; this page only asks
 * and looks. Settings > Music servers adds a server, tests it and sets the
 * quality and the cache's size. Included by sloppy.c after Podcasts. */
#include "sloppy_subsonic.h"
#define SUB_TINT 0x2f9e8f                 /* the server's own colour: its icon, an album with no cover */
#define SUB_ARTISTS_PER 60                /* artist rows a page */

static struct sl_sub_status sub_st;       /* as last seen by sub_poll */
static void sub_poll(void);
static bool sub_need_server(void);
static bool sub_wifi_up(void) { return wf_online(); }

/* ---- words */
static const char *sub_err_words(int err)
{
    switch (err)
    {
        case SL_SUB_E_NET: return "The server didn't answer. Check the address, and that it is on and reachable from this network.";
        case SL_SUB_E_AUTH: return "Wrong user name or password.";
        case SL_SUB_E_NOTSUB: return "Something answers there, but not a Subsonic server. Check the port and the path.";
        case SL_SUB_E_TOKEN: return "This server needs the password itself. Use its https:// address to sign in.";
        case SL_SUB_E_NOTFOUND: return "It's no longer on the server.";
        case SL_SUB_E_NOMEM: return "Not enough memory for all of that.";
        case SL_SUB_E_OFFLINE: return "You're offline. What's on the card still plays.";
        case SL_SUB_E_NOSERVER: return "Add a server first.";
        case SL_SUB_E_FULL: return "The room for downloaded songs is full of songs kept offline.";
        case SL_SUB_E_ADDRESS: return "That doesn't look like an address. Try music.example.com or 192.168.1.5:4533.";
        default: return "The server couldn't do that.";
    }
}
/* as the room is set: 2 GB is 2 x 1024 MB */
static void sub_size(char *b, size_t n, long long bytes)
{
    const long long mb = 1024 * 1024, gb = 1024 * mb;
    if (bytes >= gb)
    {
        long long t = (bytes * 10 + gb / 2) / gb;
        if (t % 10) snprintf(b, n, "%lld.%lld GB", t / 10, t % 10); else snprintf(b, n, "%lld GB", t / 10);
    }
    else if (bytes >= 10 * mb) snprintf(b, n, "%lld MB", (bytes + mb / 2) / mb);
    else if (bytes >= mb / 10) { long long t = (bytes * 10 + mb / 2) / mb; snprintf(b, n, "%lld.%lld MB", t / 10, t % 10); }
    else snprintf(b, n, "%lld KB", (bytes + 1023) / 1024);
}
static void sub_join(char *b, size_t n, const char *s)
{
    if (!s || !s[0]) return;
    if (b[0]) strlcat(b, "  \xc2\xb7  ", n);
    strlcat(b, s, n);
}
static void sub_mins(char *b, size_t n, uint32_t secs)
{
    char m[32];
    if (secs >= 3600) snprintf(m, sizeof m, sl_tr("%u h %u min"), (unsigned)(secs / 3600), (unsigned)(secs % 3600 / 60));
    else sl_count(m, sizeof m, (int)((secs + 59) / 60), "%s min", "%s min");
    snprintf(b, n, "%s", m);
}

/* ---- covers: the worker's squares through Podcasts' drive (the card as the C library sees it) */
static void pod_fs_init(void);
static void pod_art_gone(lv_event_t *e);
struct sub_pend { lv_obj_t *box; char cover[48]; int size; };
static struct sub_pend sub_pend[40];
static int sub_npend;
static void sub_art_img(lv_obj_t *c, const char *cover, int size)
{
    char name[96], src[100];
    int px = size > SL_SUB_ART_ROW ? SL_SUB_ART_BIG : SL_SUB_ART_ROW;
    sl_sub_art_name(name, sizeof name, sl_sub_active(), cover, px);
    snprintf(src, sizeof src, "P:%s", name);
    lv_obj_t *img = lv_image_create(c);
    lv_image_set_src(img, src);
    lv_obj_add_event_cb(img, pod_art_gone, LV_EVENT_DELETE, NULL);
    if (px != size) lv_image_set_scale(img, (uint32_t)(256 * size / px));
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_clip_corner(c, true, 0);
    /* the glyph that stood in goes */
    for (uint32_t i = 0; i < lv_obj_get_child_count(c); i++)
        if (lv_obj_get_child(c, i) != img) lv_obj_add_flag(lv_obj_get_child(c, i), LV_OBJ_FLAG_HIDDEN);
}
static void sub_art_gone(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_current_target(e);
    for (int i = 0; i < sub_npend; i++)
        if (sub_pend[i].box == c) { sub_pend[i] = sub_pend[--sub_npend]; break; }
}
/* its colour from its name, as an album with no cover */
static uint32_t sub_tint_of(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(h % 360), 45, 58);
    return lv_color_to_u32(c) & 0xffffff;
}
static lv_obj_t *sub_art(lv_obj_t *parent, const char *cover, const char *name, int size, int kind)
{
    uint32_t tint = sub_tint_of(name && name[0] ? name : cover);
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : kind == SL_SUB_ARTIST ? LV_RADIUS_CIRCLE : size > 100 ? SL_R_MD : SL_R_SM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_mix(lv_color_black(), lv_color_hex(tint), 150), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    int ic = kind == SL_SUB_ARTIST ? IC_ARTISTS : kind == SL_SUB_PLAYLIST ? IC_PLAYLISTS : IC_ALBUMS;
    lv_obj_t *g = sl_icon(c, ic, size * 3 / 8, lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(tint), 190)) & 0xffffff, tint);
    lv_obj_center(g);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (!cover || !cover[0]) return c;
    pod_fs_init();
    if (sl_sub_art_have(cover)) sub_art_img(c, cover, size);
    else if (sub_st.online && sub_npend < (int)ARRAYLEN(sub_pend))
    {
        /* asked for; it comes in when the worker has made it */
        struct sub_pend *p = &sub_pend[sub_npend++];
        p->box = c;
        snprintf(p->cover, sizeof p->cover, "%s", cover);
        p->size = size;
        lv_obj_add_event_cb(c, sub_art_gone, LV_EVENT_DELETE, NULL);
        sl_sub_art_want(cover);
    }
    return c;
}
static void sub_art_landed(void)
{
    for (int i = 0; i < sub_npend; )
    {
        if (sl_sub_art_have(sub_pend[i].cover))
        {
            struct sub_pend p = sub_pend[i];
            sub_pend[i] = sub_pend[--sub_npend];
            lv_obj_remove_event_cb(p.box, sub_art_gone);
            sub_art_img(p.box, p.cover, p.size);
        }
        else i++;
    }
}

/* ---- the pages: each a view of one answer */
enum { SV_HOME = 100, SV_FIND };
struct sub_view {
    lv_obj_t *page, *body;
    int what, offset, start;              /* the request; a page of albums; a page of artist rows */
    unsigned req;
    bool waiting;
    char arg[160], title[96];
    struct sl_sub_list l;
    struct sub_view *next;
};
static struct sub_view *sub_views;
static lv_obj_t *sub_prog_lbl;            /* the song coming down, its percent */
static char sub_prog_id[48];
static char sub_np_id[48];                /* the server's song playing now, "" none */
static void sub_view_fill(struct sub_view *v);
static struct sub_view *sub_sheet_v;      /* the page a song's sheet is about */
static void sub_view_deleted(lv_event_t *e)
{
    struct sub_view *v = lv_event_get_user_data(e);
    for (struct sub_view **p = &sub_views; *p; p = &(*p)->next)
        if (*p == v) { *p = v->next; break; }
    if (v->waiting) sl_sub_forget_req(v->req);
    sl_sub_list_free(&v->l);
    sub_prog_lbl = NULL;                  /* it may have been on this page; the next tick finds it again */
    if (sub_sheet_v == v) sub_sheet_v = NULL;
    lv_free(v);
}
static void sub_ask(struct sub_view *v)
{
    if (v->waiting) sl_sub_forget_req(v->req);
    /* the home's peek: the newest from the server, or what is on the card */
    int what = v->what == SV_HOME ? (sub_st.online ? SL_SUB_Q_ALBUMS : SL_SUB_Q_OFFLINE) : v->what;
    v->req = sl_sub_ask(what, v->arg, v->offset);
    v->waiting = v->req != 0;
    if (!v->waiting) { sl_sub_list_free(&v->l); v->l.err = SL_SUB_E_NOMEM; }
}
static struct sub_view *sub_view_new(int what, const char *arg, const char *title, bool hero)
{
    struct sub_view *v = lv_malloc(sizeof *v);
    if (!v) return NULL;
    memset(v, 0, sizeof *v);
    v->what = what;
    snprintf(v->arg, sizeof v->arg, "%s", arg ? arg : "");
    snprintf(v->title, sizeof v->title, "%s", title ? title : "");
    lv_obj_t *p;
    v->body = page_new(hero ? "" : v->title, &p);
    v->page = p;
    lv_obj_add_event_cb(p, sub_view_deleted, LV_EVENT_DELETE, v);
    v->next = sub_views;
    sub_views = v;
    return v;
}
/* the words again where it is scrolled to: something under it changed */
static void sub_refill(struct sub_view *v)
{
    int32_t y = lv_obj_get_scroll_y(v->body);
    sub_prog_lbl = NULL;
    lv_obj_clean(v->body);
    sub_view_fill(v);
    lv_obj_update_layout(v->body);
    lv_obj_scroll_to_y(v->body, y, LV_ANIM_OFF);
}
/* waiting: a ring turning, and what for */
static void sub_loading(lv_obj_t *body, const char *what)
{
    lv_obj_t *c = box(body);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(c, 72, 0);
    lv_obj_set_style_pad_row(c, SL_INSET, 0);
    lv_obj_set_style_pad_bottom(c, SL_SP_S, 0);   /* Arabic's marks below the line stay inside */
    lv_obj_t *sp = lv_spinner_create(c);
    lv_spinner_set_anim_params(sp, 1200, 70);
    lv_obj_set_size(sp, 56, 56);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(sp, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_CARD_P), LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = text(c, what, &ss_regular_18, C_SUB);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(c, (void *)1);  /* the tests know it by this */
}
static void sub_retry_cb(lv_event_t *e)
{
    struct sub_view *v = lv_event_get_user_data(e);
    sub_ask(v);
    sub_refill(v);
}
static void sub_error(struct sub_view *v, int err, const char *why)
{
    char b[200];
    const char *how = sub_err_words(err);
    if (err == SL_SUB_E_SERVER && why && why[0]) { snprintf(b, sizeof b, "%s %s", sl_tr("The server said:"), why); how = b; }
    lv_obj_t *c = sl_empty(v->body, err == SL_SUB_E_OFFLINE ? IC_WIFI : IC_SERVER, 88,
                           err == SL_SUB_E_OFFLINE ? "Not on the card." : "Couldn't reach your music.", how, 56);
    if (err != SL_SUB_E_OFFLINE && err != SL_SUB_E_NOSERVER) sl_empty_action(c, LV_SYMBOL_REFRESH "  Try again", sub_retry_cb, v);
}

/* ---- rows and cells */
static void sub_album_page(const char *id, const char *title);
static void sub_artist_page(const char *id, const char *name);
static void sub_playlist_page(const char *id, const char *name);
static void sub_play(struct sub_view *v, int at, bool shuffle);
static void sub_song_sheet(struct sub_view *v, int i);
static struct sub_view *sub_view_of(lv_obj_t *o)
{
    for (struct sub_view *v = sub_views; v; v = v->next)
        for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p)) if (p == v->page) return v;
    return NULL;
}
static void sub_item_cb(lv_event_t *e)
{
    struct sub_view *v = sub_view_of(lv_event_get_current_target(e));
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!v || i < 0 || i >= v->l.n) return;
    const struct sl_sub_item *it = &v->l.it[i];
    const char *id = sl_sub_s(&v->l, it->id), *t = sl_sub_s(&v->l, it->title);
    switch (it->kind)
    {
        case SL_SUB_ALBUM: sub_album_page(id, t); break;
        case SL_SUB_ARTIST: sub_artist_page(id, t); break;
        case SL_SUB_PLAYLIST: sub_playlist_page(id, t); break;
        case SL_SUB_SONG: sub_play(v, i, false); break;
    }
}
static void sub_more_cb(lv_event_t *e)
{
    struct sub_view *v = sub_view_of(lv_event_get_current_target(e));
    if (v) sub_song_sheet(v, (int)(intptr_t)lv_event_get_user_data(e));
}
/* a mark on a cover: on the card (a tick), kept offline (an arrow down) */
static void sub_badge(lv_obj_t *art, const struct sl_sub_item *it)
{
    if (!it->cached && !it->pinned) return;
    lv_obj_t *d = box(art);
    lv_obj_set_size(d, 30, 30);
    lv_obj_set_style_radius(d, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(it->pinned ? C_ACCENT : 0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(d, it->pinned ? LV_OPA_COVER : LV_OPA_70, 0);
    sl_elevate(d, SL_E1, 0);
    uint32_t ink = it->pinned ? (sl_accent_ink() == 0xffffff ? 0xfefefe : sl_accent_ink()) : 0xfefefe;
    lv_obj_t *l = text(d, it->pinned ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_OK, &sl_sym_16, ink);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    lv_obj_center(l);
    sl_align(d, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
}
static void sub_grid(struct sub_view *v, lv_obj_t *body, int from, int to)
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
    for (int k = from; k < to && k < v->l.n; k++)
    {
        const struct sl_sub_item *it = &v->l.it[k];
        if (it->kind != SL_SUB_ALBUM && it->kind != SL_SUB_PLAYLIST) continue;
        const char *t = sl_sub_s(&v->l, it->title);
        lv_obj_t *cell = box(g);
        lv_obj_set_size(cell, cw, cw + 8 + lh1 + 2 + lh2 + 6);
        lv_obj_set_style_transform_pivot_x(cell, cw / 2, 0);
        lv_obj_set_style_transform_pivot_y(cell, cw / 2, 0);
        press_motion(cell);
        lv_obj_set_style_transform_scale(cell, 246, LV_STATE_PRESSED);
        lv_obj_t *a = sub_art(cell, sl_sub_s(&v->l, it->cover), t, cw, it->kind);
        sl_elevate(a, sl_zune() ? SL_E0 : SL_E1, 0);
        sub_badge(a, it);
        lv_obj_t *l1 = text(cell, t, &ss_semibold_18, 0xffffff);
        lv_obj_set_size(l1, cw, lh1);
        lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
        set_align(l1);
        sl_label_deep(l1);
        lv_obj_set_pos(l1, 0, cw + 8);
        char b[140], c[40];
        b[0] = '\0';
        if (v->what == SL_SUB_Q_OFFLINE)
        {
            sl_count(c, sizeof c, it->count, "%s song", "%s songs");
            sub_join(b, sizeof b, c);
            sub_size(c, sizeof c, it->size);
            sub_join(b, sizeof b, c);
        }
        else
        {
            sub_join(b, sizeof b, sl_sub_s(&v->l, it->artist));
            if (it->year) { snprintf(c, sizeof c, "%d", it->year); sub_join(b, sizeof b, c); }
        }
        lv_obj_t *l2 = text(cell, b, &ss_regular_16, C_SUB);
        lv_obj_set_size(l2, cw, lh2);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
        set_align(l2);
        sl_label_deep(l2);
        lv_obj_set_pos(l2, 0, cw + 8 + lh1 + 2);
        on_click(cell, sub_item_cb, (void *)(intptr_t)k);
    }
}
/* a row's height: SL_ROW_H, or more where a script's letters hang lower than the Latin line (Arabic, the larger sizes) */
static int sub_row_h(void) { return LV_MAX(SL_ROW_H, sl_line_h(&ss_semibold_23) + sl_line_h(&ss_regular_18) + 2 * SL_SP_XS); }
/* an artist's or a playlist's row: its picture, its name, a line under it */
static lv_obj_t *sub_row(struct sub_view *v, lv_obj_t *parent, int k)
{
    const struct sl_sub_item *it = &v->l.it[k];
    const char *t = sl_sub_s(&v->l, it->title);
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    int rh = sub_row_h();
    lv_obj_set_size(r, lv_pct(100), rh);
    lv_obj_t *a = sub_art(r, sl_sub_s(&v->l, it->cover), t, SL_ART_ROW, it->kind);
    sl_align(a, LV_ALIGN_LEFT_MID, SL_ROW_ART_X, 0);
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (rh - sl_line_h(&ss_semibold_23) - sl_line_h(&ss_regular_18)) / 2 + SL_SP_XS / 2), tw = W - 2 * SL_GUTTER - SL_ROW_TXT_X - SL_ROW_END;
    lv_obj_t *t1 = text(r, t, &ss_semibold_23, 0xffffff);
    lv_obj_set_size(t1, tw, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    set_align(t1);
    sl_label_deep(t1);
    sl_place(t1, SL_ROW_TXT_X, ty);
    char b[140], c[48];
    b[0] = '\0';
    if (it->kind == SL_SUB_ARTIST) { if (it->count) { sl_count(c, sizeof c, it->count, "%s album", "%s albums"); sub_join(b, sizeof b, c); } }
    else
    {
        sl_count(c, sizeof c, it->count, "%s song", "%s songs");
        sub_join(b, sizeof b, c);
        if (it->dur) { sub_mins(c, sizeof c, it->dur); sub_join(b, sizeof b, c); }
        if (it->pinned) sub_join(b, sizeof b, sl_tr("Kept offline"));
    }
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    set_align(t2);
    sl_label_deep(t2);
    sl_place(t2, SL_ROW_TXT_X, ty + lh1);
    lv_obj_t *ch = text(r, sl_ui_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    sl_align(ch, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN - 14, 0);
    on_click(r, sub_item_cb, (void *)(intptr_t)k);
    return r;
}
/* a song's row: its number on an album's page (its cover elsewhere), title, who and how long; on the card or coming */
static lv_obj_t *sub_song_row(struct sub_view *v, lv_obj_t *parent, int k, bool numbered)
{
    const struct sl_sub_item *it = &v->l.it[k];
    const char *id = sl_sub_s(&v->l, it->id);
    bool now = sub_np_id[0] && !strcmp(id, sub_np_id);
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    int rh = sub_row_h();
    lv_obj_set_size(r, lv_pct(100), rh);
    int lead = numbered ? SL_ROW_NUM_X : SL_ROW_TXT_X;
    if (numbered)
    {
        if (now) sl_align(now_bars(r, C_ACCENT), LV_ALIGN_LEFT_MID, 10, 0);
        else
        {
            char n[8];
            snprintf(n, sizeof n, "%d", it->track ? it->track : k + 1);
            lv_obj_t *l = text(r, n, &ss_regular_18, C_DIM);
            lv_obj_set_width(l, 40);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            sl_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }
    else
    {
        lv_obj_t *a = sub_art(r, sl_sub_s(&v->l, it->cover), sl_sub_s(&v->l, it->album), SL_ART_ROW, SL_SUB_SONG);
        sl_align(a, LV_ALIGN_LEFT_MID, SL_ROW_ART_X, 0);
        if (now) lv_obj_center(now_bars(a, 0xffffff));
    }
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = LV_MAX(0, (rh - sl_line_h(&ss_semibold_23) - sl_line_h(&ss_regular_18)) / 2 + SL_SP_XS / 2), tw = W - 2 * SL_GUTTER - lead - SL_ROW_END - 34;
    lv_obj_t *t1 = text(r, sl_sub_s(&v->l, it->title), &ss_semibold_23, now ? C_ACCENT_TXT : 0xffffff);
    lv_obj_set_size(t1, tw, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    if (!sl_ui_rtl()) lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_LEFT, 0);
    sl_place(t1, lead, ty);
    char b[160], c[24];
    b[0] = '\0';
    bool album = v->l.head.kind == SL_SUB_ALBUM;
    const char *ar = sl_sub_s(&v->l, it->artist);
    if (!album || strcasecmp(ar, sl_sub_s(&v->l, v->l.head.artist))) sub_join(b, sizeof b, ar);
    if (it->dur) { fmt_time(c, sizeof c, (long)it->dur * 1000); sub_join(b, sizeof b, c); }
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    if (!sl_ui_rtl()) lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_LEFT, 0);
    sl_place(t2, lead, ty + lh1);
    /* where it is: coming down (its percent), on the card (a tick), kept (an arrow) */
    bool coming = sub_st.dl_id[0] && !strcmp(sub_st.dl_id, id);
    if (coming || it->cached)
    {
        lv_obj_t *m;
        if (coming)
        {
            m = text(r, "", &ss_semibold_18, C_ACCENT_TXT);
            sub_prog_lbl = m;
            snprintf(sub_prog_id, sizeof sub_prog_id, "%s", id);
            char pc[16];
            snprintf(pc, sizeof pc, "%d%%", sub_st.dl_total > 0 ? (int)(sub_st.dl_got * 100 / sub_st.dl_total) : 0);
            lv_label_set_text(m, pc);
        }
        else m = text(r, it->pinned ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_OK, &sl_sym_16, it->pinned ? C_ACCENT_TXT : C_DIM);
        sl_align(m, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN - SL_ROW_MORE_W - 4, 0);
    }
    lv_obj_t *more = icon_btn(r, NULL, SL_ROW_MORE_W, sub_more_cb, (void *)(intptr_t)k);
    sl_align(more, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN, 0);
    on_click(r, sub_item_cb, (void *)(intptr_t)k);
    lv_obj_add_event_cb(r, sub_more_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
    return r;
}
static lv_obj_t *sub_rows_box(lv_obj_t *body)
{
    lv_obj_t *rows = box(body);
    lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rows, SL_SP_XS, 0);
    return rows;
}

/* ---- an album's or a playlist's page: the cover as a hero, Play and Shuffle, kept offline or not */
static void sub_play_cb(lv_event_t *e)
{
    struct sub_view *v = sub_view_of(lv_event_get_current_target(e));
    if (v) sub_play(v, -1, (intptr_t)lv_event_get_user_data(e) != 0);
}
static struct sub_view *sub_top_view(void)
{
    if (!npages) return NULL;
    for (struct sub_view *v = sub_views; v; v = v->next) if (v->page == pages[npages - 1]) return v;
    return NULL;
}
static void sub_keep_apply(int on)
{
    struct sub_view *v = sub_top_view();
    if (!v || v->l.err) return;
    if (!sl_sub_pin(&v->l, on)) { toast_warn("Couldn't change that now"); return; }
    if (on) toast(sub_wifi_up() ? "Downloading to keep offline" : "It downloads when Wi-Fi is on");
    else toast("No longer kept offline");
    v->l.head.pinned = (uint8_t)on;
}
static void sub_forget_cb(lv_event_t *e)
{
    struct sub_view *v = sub_view_of(lv_event_get_current_target(e));
    if (!v) return;
    sl_sub_forget(sl_sub_s(&v->l, v->l.head.id));
    toast("Removed from the card");
}
static void sub_head_wash(lv_obj_t *p, lv_obj_t *body, const char *name)
{
    if (sl_zune()) return;
    lv_color_t tint = lv_color_hex(sub_tint_of(name));
    lv_color_hsv_t hsv = lv_color_to_hsv(tint);
    lv_color_t wash = sl_theme() == 1 ? lv_color_hsv_to_rgb(hsv.h, 4 + hsv.s * 26 / 100, 94)
                                      : lv_color_hsv_to_rgb(hsv.h, 6 + hsv.s * 52 / 100, sl_theme() == 2 ? 24 : 36);
    lv_obj_set_style_bg_color(p, wash, 0);
    lv_obj_set_style_bg_grad_color(p, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(p, 36, 0);
    lv_obj_set_style_bg_grad_stop(p, 180, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(p); i++)
    {
        lv_obj_t *o = lv_obj_get_child(p, i);
        if (o == body) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        else lv_obj_set_style_bg_color(o, wash, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        sl_elevate(o, SL_E0, 0);
    }
}
static void sub_hero(struct sub_view *v)
{
    lv_obj_t *body = v->body;
    const struct sl_sub_item *h = &v->l.head;
    const char *title = h->title ? sl_sub_s(&v->l, h->title) : v->title;
    lv_obj_t *head = box(body);
    lv_obj_set_size(head, W, LV_SIZE_CONTENT);
    lv_obj_set_style_translate_x(head, -SL_GUTTER, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(head, 6, 0);
    lv_obj_set_style_pad_bottom(head, 18, 0);
    lv_obj_set_style_pad_hor(head, 28, 0);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_t *c = sub_art(head, sl_sub_s(&v->l, h->cover), title, SL_SUB_ART_BIG, v->what == SL_SUB_Q_PLAYLIST ? SL_SUB_PLAYLIST : SL_SUB_ALBUM);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    sl_elevate(c, sl_zune() ? SL_E0 : SL_E3, 0);
    lv_obj_t *t = text(head, title, sl_zune() ? sl_zune_font(title, &ss_regular_34) : &ss_bold_28, 0xffffff);
    lv_obj_set_width(t, W - 56);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    note_lines(t, title, W - 56, 2);
    sl_label_deep(t);
    const char *who = sl_sub_s(&v->l, h->artist);
    if (who[0])
    {
        lv_obj_t *ar = text(head, who, &ss_semibold_18, C_SUB);
        lv_obj_set_width(ar, W - 56);
        lv_obj_set_style_text_align(ar, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(ar, 1);
    }
    char b[160], c1[48];
    b[0] = '\0';
    if (h->year) { snprintf(c1, sizeof c1, "%d", h->year); sub_join(b, sizeof b, c1); }
    int on_card = 0;
    uint32_t dur = 0;
    for (int i = 0; i < v->l.n; i++) { on_card += v->l.it[i].cached; dur += v->l.it[i].dur; }
    sl_count(c1, sizeof c1, v->l.n, "%s song", "%s songs");
    sub_join(b, sizeof b, c1);
    if (dur) { sub_mins(c1, sizeof c1, dur); sub_join(b, sizeof b, c1); }
    lv_obj_t *m = text(head, b, &ss_regular_16, C_SUB);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *pl = box(body);
    lv_obj_set_size(pl, lv_pct(100), 72);
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pl, 12, 0);
    lv_obj_set_style_pad_hor(pl, 6, 0);
    lv_obj_set_style_pad_top(pl, 4, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(pl, LV_BASE_DIR_RTL, 0);
    lv_obj_t *go = mn_pill(pl, LV_SYMBOL_PLAY "  Play", 10, SL_PILL_H, true, sub_play_cb, (void *)0);
    lv_obj_set_flex_grow(go, 1);
    lv_obj_t *sh = mn_pill(pl, LV_SYMBOL_SHUFFLE "  Shuffle", 10, SL_PILL_H, false, sub_play_cb, (void *)1);
    lv_obj_set_flex_grow(sh, 1);
    lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_x(sh, lv_pct(50), 0);
    (void)on_card;
}
static void sub_keep_group(struct sub_view *v)
{
    int on_card = 0;
    for (int i = 0; i < v->l.n; i++) on_card += v->l.it[i].cached;
    lv_obj_t *g = set_group(v->body, "On the card");
    char line[96];
    if (on_card == v->l.n && v->l.n) snprintf(line, sizeof line, "%s", sl_tr("Every song is on the card"));
    else snprintf(line, sizeof line, sl_tr("%d of %d songs on the card"), on_card, v->l.n);
    set_switch(g, "Keep offline", line, v->l.head.pinned, sub_keep_apply);
    if (on_card && !v->l.head.pinned && v->what == SL_SUB_Q_ALBUM) set_danger(g, LV_SYMBOL_TRASH, "Remove from the card", sub_forget_cb, NULL);
}
static void sub_songs_fill(struct sub_view *v)
{
    if (v->l.offline)
    {
        lv_obj_t *n = text(v->body, "Offline: the songs on the card", &ss_semibold_18, C_SUB);
        lv_obj_set_width(n, lv_pct(100));
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_bottom(n, SL_SP_S, 0);
    }
    lv_obj_t *rows = sub_rows_box(v->body);
    bool numbered = v->what == SL_SUB_Q_ALBUM;
    int disc = -1;
    bool discs = false;
    for (int i = 1; i < v->l.n && numbered; i++) discs |= v->l.it[i].disc != v->l.it[0].disc;
    for (int i = 0; i < v->l.n; i++)
    {
        if (discs && v->l.it[i].disc != disc)
        {
            disc = v->l.it[i].disc;
            char b[32];
            snprintf(b, sizeof b, "%s %d", sl_tr("DISC"), disc);
            section(rows, b);
        }
        sub_song_row(v, rows, i, numbered);
    }
    if (!v->l.n) sl_empty(v->body, IC_SONGS, 72, "No songs here.", "The server has none in it yet.", 24);
    sub_keep_group(v);
}

/* ---- the server's home: the ways in, the newest albums, what is on the card */
static void sub_servers_page(void);
static void sub_find_page(void);
static void sub_list_page(int what, const char *arg, const char *title);
static void sub_offline_page(void);
static void sub_link_open(intptr_t k)
{
    switch (k)
    {
        case 0: sub_list_page(SL_SUB_Q_ARTISTS, "", "Artists"); break;
        case 1: sub_list_page(SL_SUB_Q_ALBUMS, "newest", "Albums"); break;
        case 2: sub_list_page(SL_SUB_Q_PLAYLISTS, "", "Playlists"); break;
        case 3: sub_list_page(SL_SUB_Q_STARRED, "", "Starred"); break;
        case 4: sub_find_page(); break;
        case 5: sub_offline_page(); break;
        case 6: sub_servers_page(); break;
    }
}
static void sub_link_cb(lv_event_t *e) { sub_link_open((intptr_t)lv_event_get_user_data(e)); }
static void sub_dl_words(char *b, size_t n)
{
    b[0] = '\0';
    int waiting = sub_st.queued + (sub_st.dl_id[0] ? 1 : 0);
    if (!waiting) return;
    sl_count(b, n, waiting, "%s coming", "%s coming");
    if (sub_st.dl_id[0] && sub_st.dl_total > 0)
    {
        char p[24];
        snprintf(p, sizeof p, "%d%%", (int)(sub_st.dl_got * 100 / sub_st.dl_total));
        sub_join(b, n, p);
    }
    else if (!sub_st.online) sub_join(b, n, sl_tr("Waiting for Wi-Fi"));
}
static void sub_card_words(char *b, size_t n)
{
    long long used, limit;
    int songs, albums;
    sl_sub_usage(&used, &limit, &songs, &albums);
    char c[40], u[24], l[24];
    b[0] = '\0';
    sl_count(c, sizeof c, songs, "%s song", "%s songs");
    sub_join(b, n, c);
    sub_size(u, sizeof u, used);
    sub_size(l, sizeof l, limit);
    snprintf(c, sizeof c, "%s / %s", u, l);
    sub_join(b, n, c);
}
static void sub_home_fill(struct sub_view *v)
{
    lv_obj_t *body = v->body;
    struct sl_sub_server s;
    if (!sl_sub_server(0, &s))
    {
        lv_obj_t *c = sl_empty(body, IC_SERVER, 96, "No music server yet.",
                               "Add your Navidrome, Subsonic or other OpenSubsonic server to play its music here. Songs you play stay on the card for when there's no Wi-Fi.", 48);
        sl_empty_action(c, LV_SYMBOL_PLUS "  Add a server", sub_link_cb, (void *)6);
        return;
    }
    bool off = !sub_st.online;
    if (off)
    {
        lv_obj_t *g = set_group(body, NULL);
        set_info(g, LV_SYMBOL_WIFI, 0x66738a, "Offline", "Albums on the card still play", true);
    }
    lv_obj_t *g = set_group(body, "Browse");
    set_link(g, LV_SYMBOL_AUDIO, 0xf0872e, "Artists", NULL, sub_link_cb, (void *)0);
    set_link(g, LV_SYMBOL_IMAGE, 0x34b84e, "Albums", NULL, sub_link_cb, (void *)1);
    set_link(g, LV_SYMBOL_LIST, 0x697da5, "Playlists", NULL, sub_link_cb, (void *)2);
    set_link(g, LV_SYMBOL_OK, 0xe8a317, "Starred", NULL, sub_link_cb, (void *)3);
    set_link(g, LV_SYMBOL_KEYBOARD, 0x2d8ff0, "Search", NULL, sub_link_cb, (void *)4);
    /* the newest albums, a peek (the worker's answer, or the card's when offline) */
    char hd[48];
    snprintf(hd, sizeof hd, "%s", sl_case(sl_tr(off ? "ON THE CARD" : "NEWEST")));
    section(body, hd);
    if (v->waiting) sub_loading(body, "Asking the server");
    else if (v->l.err) sub_error(v, v->l.err, v->l.why);
    else if (!v->l.n) sl_empty(body, IC_ALBUMS, 64, off ? "Nothing on the card yet." : "No albums yet.",
                               off ? "Songs you play from the server stay here, to play without Wi-Fi." : "The server has no music in it yet.", 12);
    else sub_grid(v, body, 0, 4);
    g = set_group(body, "On the card");
    char dl[80], card[96];
    sub_dl_words(dl, sizeof dl);
    sub_card_words(card, sizeof card);
    set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x0aa9c7, "Downloaded", dl[0] ? dl : card, NULL, sub_link_cb, (void *)5);
    char line[200];
    snprintf(line, sizeof line, "%s  \xc2\xb7  %s", s.user, s.url);
    set_link_sub(g, LV_SYMBOL_SETTINGS, 0x66738a, "Music servers", line, NULL, sub_link_cb, (void *)6);
}
static lv_obj_t *sub_home_obj;
static void sub_home_deleted(lv_event_t *e) { if (lv_event_get_current_target(e) == sub_home_obj) sub_home_obj = NULL; }
static void subsonic_page(void)
{
    pod_fs_init();
    if (page_bring_back(sub_home_obj)) return;
    struct sl_sub_server s;
    bool have = sl_sub_server(0, &s);
    struct sub_view *v = sub_view_new(SV_HOME, "newest", have ? s.name : "Server", false);
    if (!v) return;
    sub_home_obj = v->page;
    lv_obj_add_event_cb(v->page, sub_home_deleted, LV_EVENT_DELETE, NULL);
    if (have)
    {
        header_button(v->page, LV_SYMBOL_SETTINGS, sub_link_cb, (void *)6);
        sub_ask(v);
    }
    sub_view_fill(v);
    page_push(v->page);
}

/* ---- lists: artists, albums (by order), playlists, starred, search, an artist's albums */
static const char *const sub_orders[] = { "newest", "recent", "random", "starred", "alphabeticalByName" };
static const char *const sub_order_names[] = { "Newest", "Recently played", "Random", "Starred", "A to Z" };
static void sub_order_pick(int k)
{
    struct sub_view *v = sub_top_view();
    if (!v || k < 0 || k >= (int)ARRAYLEN(sub_orders)) return;
    snprintf(v->arg, sizeof v->arg, "%s", sub_orders[k]);
    v->offset = 0;
    sub_ask(v);
    sub_refill(v);
}
static void sub_pager_cb(lv_event_t *e)
{
    struct sub_view *v = sub_view_of(lv_event_get_current_target(e));
    if (!v) return;
    int to = (int)(intptr_t)lv_event_get_user_data(e);
    if (v->what == SL_SUB_Q_ALBUMS) { v->offset = to; sub_ask(v); }
    else v->start = to;
    sub_refill(v);
    lv_obj_scroll_to_y(v->body, 0, LV_ANIM_OFF);
}
static void sub_section(lv_obj_t *body, int n, const char *one, const char *many)
{
    char b[64];
    sl_count(b, sizeof b, n, one, many);
    section(body, sl_case(b));
}
static void sub_results(struct sub_view *v)
{
    int na = 0, nal = 0, ns = 0;
    for (int i = 0; i < v->l.n; i++)
    {
        int k = v->l.it[i].kind;
        na += k == SL_SUB_ARTIST; nal += k == SL_SUB_ALBUM; ns += k == SL_SUB_SONG;
    }
    if (!v->l.n)
    {
        sl_empty(v->body, v->what == SL_SUB_Q_STARRED ? IC_STAR : IC_SERVER, 88,
                 v->what == SL_SUB_Q_STARRED ? "Nothing starred yet." : "Nothing by that name.",
                 v->what == SL_SUB_Q_STARRED ? "Star a song from its menu, here or in the server's own app." : "Try fewer words, or part of a name.", 56);
        return;
    }
    if (na)
    {
        sub_section(v->body, na, "%s ARTIST", "%s ARTISTS");
        lv_obj_t *rows = sub_rows_box(v->body);
        for (int i = 0; i < v->l.n; i++) if (v->l.it[i].kind == SL_SUB_ARTIST) sub_row(v, rows, i);
    }
    if (nal)
    {
        sub_section(v->body, nal, "%s ALBUM", "%s ALBUMS");
        sub_grid(v, v->body, 0, v->l.n);
    }
    if (ns)
    {
        sub_section(v->body, ns, "%s SONG", "%s SONGS");
        lv_obj_t *rows = sub_rows_box(v->body);
        for (int i = 0; i < v->l.n; i++) if (v->l.it[i].kind == SL_SUB_SONG) sub_song_row(v, rows, i, false);
    }
}
/* what is on the card: how full, what is coming, the albums */
static void sub_clear_cb(lv_event_t *e);
static void sub_offline_fill(struct sub_view *v)
{
    lv_obj_t *body = v->body;
    long long used, limit;
    int songs, albums;
    sl_sub_usage(&used, &limit, &songs, &albums);
    lv_obj_t *card = opt_card(body, NULL, NULL);
    char b[120], u[24], l[24];
    sub_size(u, sizeof u, used);
    sub_size(l, sizeof l, limit);
    snprintf(b, sizeof b, sl_tr("%s of %s"), u, l);
    lv_obj_t *t = text(card, b, &ss_semibold_23, C_TEXT);
    set_align(t);
    lv_obj_set_width(t, lv_pct(100));
    /* the room as a bar, round at both ends */
    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, lv_pct(100), 10);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, limit > 0 ? (int32_t)LV_MIN(1000, used * 1000 / limit) : 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, SL_R_FULL, 0);
    lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_margin_ver(bar, SL_SP_S, 0);
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(bar, LV_BASE_DIR_RTL, 0);
    char c[48];
    b[0] = '\0';
    sl_count(c, sizeof c, songs, "%s song", "%s songs");
    sub_join(b, sizeof b, c);
    sl_count(c, sizeof c, albums, "%s album", "%s albums");
    sub_join(b, sizeof b, c);
    char dl[80];
    sub_dl_words(dl, sizeof dl);
    sub_join(b, sizeof b, dl);
    lv_obj_t *s = text(card, b, &ss_regular_16, C_SUB);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_bottom(s, sl_line_h(&ss_regular_16) - lv_font_get_line_height(&ss_regular_16), 0);   /* Arabic's marks stay inside */
    set_align(s);
    if (sub_st.dl_id[0])
    {
        snprintf(b, sizeof b, "%s %s", sl_tr("Downloading"), sub_st.dl_title);
        lv_obj_t *d = text(card, b, &ss_regular_16, C_ACCENT_TXT);
        lv_obj_set_size(d, lv_pct(100), lv_font_get_line_height(&ss_regular_16));
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        set_align(d);
    }
    if (v->waiting) { sub_loading(body, "Looking at the card"); return; }
    if (!v->l.n)
    {
        sl_empty(body, IC_ALBUMS, 72, "Nothing on the card yet.", "Songs you play from the server stay here, to play without Wi-Fi.", 24);
        return;
    }
    sub_section(body, v->l.n, "%s ALBUM", "%s ALBUMS");
    sub_grid(v, body, 0, v->l.n);
    lv_obj_t *g = set_group(body, NULL);
    set_danger(g, LV_SYMBOL_TRASH, "Remove songs not kept offline", sub_clear_cb, NULL);
    opt_note(body, "The least recently played go first when the room runs out. Albums and playlists kept offline always stay.");
}
static void sub_clear_cb(lv_event_t *e)
{
    (void)e;
    sl_sub_clear();
    toast("Removed from the card");
}
static void sub_view_fill(struct sub_view *v)
{
    sub_prog_lbl = NULL;
    if (v->what == SV_HOME) { sub_home_fill(v); return; }
    if (v->what == SL_SUB_Q_OFFLINE) { sub_offline_fill(v); return; }
    bool hero = v->what == SL_SUB_Q_ALBUM || v->what == SL_SUB_Q_PLAYLIST;
    if (v->what == SL_SUB_Q_ALBUMS)
    {
        int sel = 0;
        for (int i = 0; i < (int)ARRAYLEN(sub_orders); i++) if (!strcmp(v->arg, sub_orders[i])) sel = i;
        lv_obj_t *g = set_group(v->body, NULL);
        opt_chips(g, sub_order_names, ARRAYLEN(sub_orders), sel, sub_order_pick);
    }
    if (v->waiting) { sub_loading(v->body, "Asking the server"); return; }
    if (v->l.err) { sub_error(v, v->l.err, v->l.why); return; }
    switch (v->what)
    {
        case SL_SUB_Q_ALBUM: case SL_SUB_Q_PLAYLIST:
            (void)hero;
            sub_hero(v);
            sub_songs_fill(v);
            break;
        case SL_SUB_Q_ALBUMS:
            if (!v->l.n) { sl_empty(v->body, IC_ALBUMS, 88, "No albums here.", v->offset ? "That was the last page." : "None in this order yet.", 40); }
            sub_grid(v, v->body, 0, v->l.n);
            if (v->offset || v->l.more)
            {
                lv_obj_t *bar = sl_pager(v->body);
                if (v->offset) sl_pager_button(bar, LV_SYMBOL_LEFT "  Back", false, sub_pager_cb, v->offset - SL_SUB_PAGE_N);
                if (v->l.more) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, sub_pager_cb, v->offset + SL_SUB_PAGE_N);
            }
            break;
        case SL_SUB_Q_ARTIST:
        {
            sub_section(v->body, v->l.n, "%s ALBUM", "%s ALBUMS");
            sub_grid(v, v->body, 0, v->l.n);
            if (!v->l.n) sl_empty(v->body, IC_ALBUMS, 72, "No albums here.", "The server has none by them.", 24);
            break;
        }
        case SL_SUB_Q_ARTISTS: case SL_SUB_Q_PLAYLISTS:
        {
            if (!v->l.n)
            {
                bool pl = v->what == SL_SUB_Q_PLAYLISTS;
                sl_empty(v->body, pl ? IC_PLAYLISTS : IC_ARTISTS, 88, pl ? "No playlists yet." : "No artists yet.",
                         pl ? "Make one in the server's own app: it shows here." : "The server has no music in it yet.", 56);
                break;
            }
            if (v->start >= v->l.n) v->start = 0;
            int end = v->start + SUB_ARTISTS_PER < v->l.n ? v->start + SUB_ARTISTS_PER : v->l.n;
            if (v->what == SL_SUB_Q_ARTISTS) sub_section(v->body, v->l.n, "%s ARTIST", "%s ARTISTS");
            else sub_section(v->body, v->l.n, "%s PLAYLIST", "%s PLAYLISTS");
            lv_obj_t *rows = sub_rows_box(v->body);
            for (int i = v->start; i < end; i++) sub_row(v, rows, i);
            if (v->start || end < v->l.n)
            {
                lv_obj_t *bar = sl_pager(v->body);
                if (v->start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Back", false, sub_pager_cb, v->start - SUB_ARTISTS_PER);
                if (end < v->l.n) sl_pager_button(bar, "More  " LV_SYMBOL_RIGHT, true, sub_pager_cb, end);
            }
            break;
        }
        default: sub_results(v); break;
    }
}
static void sub_list_page(int what, const char *arg, const char *title)
{
    if (!sub_need_server()) return;
    struct sub_view *v = sub_view_new(what, arg, title, false);
    if (!v) return;
    sub_ask(v);
    sub_view_fill(v);
    page_push(v->page);
}
static void sub_album_page(const char *id, const char *title)
{
    struct sub_view *v = sub_view_new(SL_SUB_Q_ALBUM, id, title, true);
    if (!v) return;
    sub_head_wash(v->page, v->body, title);
    sub_ask(v);
    sub_view_fill(v);
    page_push(v->page);
}
static void sub_playlist_page(const char *id, const char *name)
{
    struct sub_view *v = sub_view_new(SL_SUB_Q_PLAYLIST, id, name, true);
    if (!v) return;
    sub_head_wash(v->page, v->body, name);
    sub_ask(v);
    sub_view_fill(v);
    page_push(v->page);
}
static void sub_artist_page(const char *id, const char *name) { sub_list_page(SL_SUB_Q_ARTIST, id, name); }
static void sub_offline_page(void)
{
    struct sub_view *v = sub_view_new(SL_SUB_Q_OFFLINE, "", "Downloaded", false);
    if (!v) return;
    sub_ask(v);
    sub_view_fill(v);
    page_push(v->page);
}

/* ---- search: the words, then their answers */
static lv_obj_t *sub_find_ta;
static void sub_find_go_cb(lv_event_t *e)
{
    (void)e;
    if (!sub_find_ta) return;
    char q[96];
    strlcpy(q, sl_ta_text(sub_find_ta), sizeof q);
    pl_utf8_trim(q);
    if (!q[0]) return;
    if (!sub_wifi_up()) { toast("Connect to Wi-Fi first"); return; }
    sub_list_page(SL_SUB_Q_SEARCH, q, q);
}
static lv_obj_t *sub_find_obj;
static void sub_find_deleted(lv_event_t *e) { if (lv_event_get_current_target(e) == sub_find_obj) sub_find_obj = sub_find_ta = NULL; }
static void sub_find_page(void)
{
    if (!sub_need_server()) return;
    lv_obj_t *p, *body = page_new("Search", &p);
    sub_find_obj = p;
    lv_obj_add_event_cb(p, sub_find_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    sub_find_ta = set_field(set_group(body, "Artist, album or song"));
    lv_textarea_set_max_length(sub_find_ta, 80);
    sl_ta_hint(sub_find_ta, sl_tr("A name, or part of one"));
    lv_obj_set_style_text_color(sub_find_ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    if (set_rtl()) lv_obj_set_style_text_align(sub_find_ta, LV_TEXT_ALIGN_RIGHT, 0);
    opt_note(body, "Searches your server over Wi-Fi.");
    wf_kb_build(p, sub_find_ta, KB_SEARCH, sub_find_go_cb, "Search");
    page_push(p);
}

/* ---- a song's sheet: play, star, its album */
static int sub_sheet_i;
static void sub_sheet_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    struct sub_view *v = sub_sheet_v;
    int i = sub_sheet_i;
    acts_close();
    if (!v || i < 0 || i >= v->l.n) return;
    struct sl_sub_item *it = &v->l.it[i];
    switch (k)
    {
        case 0: sub_play(v, i, false); break;
        case 1: case 2:
            if (!sub_wifi_up()) { toast("Connect to Wi-Fi first"); break; }
            if (!sl_sub_star(SL_SUB_SONG, sl_sub_s(&v->l, it->id), k == 1)) { toast_warn("Couldn't change that now"); break; }
            it->starred = k == 1;
            toast(k == 1 ? "Starred" : "No longer starred");
            break;
        case 3:
        {
            char id[48], t[96];
            snprintf(id, sizeof id, "%s", sl_sub_s(&v->l, it->album_id));
            snprintf(t, sizeof t, "%s", sl_sub_s(&v->l, it->album));
            sub_album_page(id, t);
            break;
        }
    }
}
static void sub_song_sheet(struct sub_view *v, int i)
{
    if (i < 0 || i >= v->l.n) return;
    const struct sl_sub_item *it = &v->l.it[i];
    sub_sheet_v = v;
    sub_sheet_i = i;
    acts_begin();
    lv_obj_t *t = text(acts, sl_sub_s(&v->l, it->title), &ss_semibold_23, 0xffffff);
    lv_obj_set_size(t, lv_pct(100), lv_font_get_line_height(&ss_semibold_23));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_hor(t, 16, 0);
    lv_obj_set_style_pad_top(t, 8, 0);
    char b[200];
    b[0] = '\0';
    sub_join(b, sizeof b, sl_sub_s(&v->l, it->artist));
    sub_join(b, sizeof b, sl_sub_s(&v->l, it->album));
    lv_obj_t *s = text(acts, b, &ss_regular_16, C_SUB);
    lv_obj_set_size(s, lv_pct(100), lv_font_get_line_height(&ss_regular_16) + 8);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_hor(s, 16, 0);
    acts_row_cb(LV_SYMBOL_PLAY, "Play", sub_sheet_cb, 0);
    if (sub_st.online) acts_row_cb(LV_SYMBOL_OK, it->starred ? "Unstar" : "Star", sub_sheet_cb, it->starred ? 2 : 1);
    if (v->what != SL_SUB_Q_ALBUM && it->album_id) acts_row_cb(LV_SYMBOL_IMAGE, "Go to album", sub_sheet_cb, 3);
    acts_show();
}

/* ---- playing: the first song comes down, plays, and the next few come while it does */
static struct {
    unsigned id;                          /* the worker's session, 0 none */
    struct sl_sub_list l;                 /* the songs, a copy */
    int *order, n;                        /* their order (indexes into l) */
    int cursor;                           /* the next in order to go into the playlist */
    int fetched;                          /* in order, asked for up to here */
    int *pl_at, pl_n;                     /* the playlist's songs, as places in order */
    bool ended;                           /* the playlist ran out before the next was here */
    bool seen;                            /* one of ours has played */
    uint32_t foreign_ms;                  /* since something else has been playing; 0 not */
    uint32_t asked_ms;
    unsigned dl_seq0;
    char path[300];
} sub_ses;
static void sub_ses_end(void)
{
    sl_sub_list_free(&sub_ses.l);
    free(sub_ses.order);
    free(sub_ses.pl_at);
    memset(&sub_ses, 0, sizeof sub_ses);
}
static bool sub_ses_ours(const struct mp3entry *id3)
{
    return id3 && sl_sub_is_cached_path(id3->path);
}
static void sub_ses_fetch(int upto)
{
    if (upto > sub_ses.n) upto = sub_ses.n;
    if (sub_ses.fetched >= upto) return;
    sl_sub_fetch(&sub_ses.l, sub_ses.order + sub_ses.fetched, upto - sub_ses.fetched, sub_ses.id);
    sub_ses.fetched = upto;
    sub_ses.asked_ms = sl_ms();
    sub_ses.dl_seq0 = sub_st.dl_seq;
}
static void sub_play(struct sub_view *v, int at, bool shuffle)
{
    int songs = 0;
    for (int i = 0; i < v->l.n; i++) songs += v->l.it[i].kind == SL_SUB_SONG;
    if (!songs) return;
    sub_ses_end();
    if (!sl_sub_list_dup(&v->l, &sub_ses.l)) { toast_warn("Not enough memory for all of that."); return; }
    sub_ses.order = malloc((size_t)songs * sizeof *sub_ses.order);
    sub_ses.pl_at = malloc((size_t)songs * sizeof *sub_ses.pl_at);
    if (!sub_ses.order || !sub_ses.pl_at) { sub_ses_end(); toast_warn("Not enough memory for all of that."); return; }
    /* from the one tapped to the end (a list of songs from search: just that one) */
    bool one = v->what == SL_SUB_Q_SEARCH || v->what == SL_SUB_Q_STARRED;
    for (int i = at < 0 ? 0 : at; i < v->l.n; i++)
        if (v->l.it[i].kind == SL_SUB_SONG) { sub_ses.order[sub_ses.n++] = i; if (one && at >= 0) break; }
    if (shuffle)
        for (int i = sub_ses.n - 1; i > 0; i--)
        {
            int j = (int)((unsigned)rand() % (unsigned)(i + 1)), t = sub_ses.order[i];
            sub_ses.order[i] = sub_ses.order[j];
            sub_ses.order[j] = t;
        }
    /* offline: only what is here plays */
    if (!sub_st.online)
    {
        int k = 0;
        for (int i = 0; i < sub_ses.n; i++)
            if (sl_sub_cached(sub_ses.l.server, sl_sub_s(&sub_ses.l, sub_ses.l.it[sub_ses.order[i]].id), NULL, 0)) sub_ses.order[k++] = sub_ses.order[i];
        sub_ses.n = k;
        if (!k) { sub_ses_end(); toast("You're offline. What's on the card still plays."); return; }
    }
    sub_ses.id = sl_sub_session_new();
    sub_ses_fetch(1 + SL_SUB_AHEAD);
    sub_poll();
    if (!sub_ses.pl_n) toast("Getting the song");
}
/* each tick: the songs that came go into the playlist, in order; the next few are asked for */
static void sub_ses_tick(void)
{
    if (!sub_ses.id) return;
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    bool ours = sub_ses_ours(id3);
    /* something else plays now, for a while (Rockbox takes a moment to start ours): the play is over */
    if (sub_ses.pl_n && id3 && !ours)
    {
        if (!sub_ses.foreign_ms) sub_ses.foreign_ms = sl_ms() | 1;
        else if (sl_ms() - sub_ses.foreign_ms > 4000) { sub_ses_end(); sl_sub_session_new(); return; }
    }
    else sub_ses.foreign_ms = 0;
    if (ours) sub_ses.seen = true;
    int cur = ours ? playlist_get_display_index() - 1 : -1;
    if (cur >= 0 && cur < sub_ses.pl_n) sub_ses_fetch(sub_ses.pl_at[cur] + 1 + SL_SUB_AHEAD);   /* it and the next few */
    if (sub_ses.seen && !(audio_status() & AUDIO_STATUS_PLAY)) sub_ses.ended = true;
    while (sub_ses.cursor < sub_ses.n)
    {
        const struct sl_sub_item *it = &sub_ses.l.it[sub_ses.order[sub_ses.cursor]];
        char path[300];
        if (!sl_sub_cached(sub_ses.l.server, sl_sub_s(&sub_ses.l, it->id), path, sizeof path))
        {
            /* not coming (gone from the server, no room, offline): the next one instead */
            bool coming = sub_st.queued || sub_st.dl_id[0];
            if (sub_ses.fetched > sub_ses.cursor && !coming && sub_st.dl_seq != sub_ses.dl_seq0 && sl_ms() - sub_ses.asked_ms > 500)
            { sub_ses.cursor++; sub_ses_fetch(sub_ses.cursor + 1 + SL_SUB_AHEAD); continue; }
            if (sub_ses.fetched <= sub_ses.cursor) sub_ses_fetch(sub_ses.cursor + 1 + SL_SUB_AHEAD);
            break;
        }
        if (!sub_ses.pl_n)
        {
            if (playlist_create(sl_sub_folder, NULL) < 0 || playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
            { toast_warn("Couldn't play this song"); sub_ses_end(); return; }
            sub_ses.pl_at[sub_ses.pl_n++] = sub_ses.cursor++;
            playlist_start(0, 0, 0);
            sub_ses.foreign_ms = 0;
            sl_sub_touch(path);
            if (sub_top_view()) np_show_auto();
            continue;
        }
        bool at_end = cur == sub_ses.pl_n - 1;
        if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0) break;
        sub_ses.pl_at[sub_ses.pl_n++] = sub_ses.cursor++;
        if (sub_ses.ended) { sub_ses.ended = false; playlist_start(sub_ses.pl_n - 1, 0, 0); }
        else if (at_end && (audio_status() & AUDIO_STATUS_PLAY)) audio_flush_and_reload_tracks();   /* Rockbox had nothing after it */
    }
}

/* ---- the listening history's hooks: a play is told to the server */
static struct { uint32_t hash; char path[300]; } sub_heard;
static void sub_started(const struct mp3entry *id3, uint32_t hash)
{
    sub_heard.hash = 0;
    if (!id3 || !sl_sub_is_cached_path(id3->path)) return;
    sub_heard.hash = hash;
    strlcpy(sub_heard.path, id3->path, sizeof sub_heard.path);
    sl_sub_touch(id3->path);
    sl_sub_scrobble(id3->path, (uint32_t)time(NULL), false);   /* playing now */
}
static void sub_counted(const struct sl_play *p, uint32_t len_ms)
{
    if (!sub_heard.hash || p->path_hash != sub_heard.hash) return;
    sub_heard.hash = 0;
    if (sl_scrob_counts(p->ms, len_ms)) sl_sub_scrobble(sub_heard.path, p->when, true);
}

/* ---- Settings > Music servers: the servers, the quality, the room on the card */
static lv_obj_t *sub_srv_obj, *sub_edit_obj, *sub_url_ta, *sub_user_ta, *sub_pass_ta, *sub_test_line;
static uint32_t sub_edit_id;
static bool sub_want_test, sub_test_save;
static unsigned sub_test_from;
static void sub_edit_page(uint32_t id);
static void sub_srv_fill(lv_obj_t *body);
static lv_obj_t *sub_srv_body;
static void sub_refill_srv(void *unused);
static void sub_srv_cb(lv_event_t *e) { sub_edit_page((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }
static void sub_quality_pick(int k) { struct sl_sub_prefs p; sl_sub_prefs(&p); p.quality = k; sl_sub_prefs_set(&p); }
static const int sub_gbs[] = { 1, 2, 4, 8, 16 };
static void sub_room_pick(int k)
{
    struct sl_sub_prefs p;
    sl_sub_prefs(&p);
    p.limit_gb = sub_gbs[k];
    sl_sub_prefs_set(&p);
    if (sub_srv_body) lv_async_call((lv_async_cb_t)sub_refill_srv, NULL);
}
static void sub_srv_clear_cb(lv_event_t *e) { (void)e; sl_sub_clear(); toast("Removed from the card"); }
static void sub_srv_fill(lv_obj_t *body)
{
    struct sl_sub_server s[SL_SUB_SERVERS];
    int n = sl_sub_servers(s, SL_SUB_SERVERS);
    uint32_t act = sl_sub_active();
    lv_obj_t *g = set_group(body, "Servers");
    for (int i = 0; i < n; i++)
    {
        char line[260];
        snprintf(line, sizeof line, "%s  \xc2\xb7  %s", s[i].user, s[i].url);
        set_link_sub(g, s[i].id == act ? LV_SYMBOL_OK : LV_SYMBOL_DRIVE, s[i].id == act ? 0x3aa856 : 0x66738a, s[i].name, line,
                     s[i].id == act ? sl_tr("In use") : NULL, sub_srv_cb, (void *)(uintptr_t)s[i].id);
    }
    if (n < SL_SUB_SERVERS) set_link_sub(g, LV_SYMBOL_PLUS, SUB_TINT, "Add a server", "Navidrome, Subsonic, Airsonic, Gonic and more", NULL, sub_srv_cb, (void *)0);
    struct sl_sub_prefs p;
    sl_sub_prefs(&p);
    g = set_group(body, "Quality");
    static const char *const q[] = { "Original", "MP3 320", "Opus 128" };
    opt_chips(g, q, 3, p.quality, sub_quality_pick);
    opt_note(body, "Original is the file as it is on the server. MP3 and Opus are made smaller by the server as the song comes; Opus needs a server that can make it.");
    g = set_group(body, "Room on the card");
    static const char *const gb[] = { "1 GB", "2 GB", "4 GB", "8 GB", "16 GB" };
    int sel = 1;
    for (int i = 0; i < (int)ARRAYLEN(sub_gbs); i++) if (sub_gbs[i] == p.limit_gb) sel = i;
    opt_chips(g, gb, ARRAYLEN(sub_gbs), sel, sub_room_pick);
    char card[96];
    sub_card_words(card, sizeof card);
    set_info(g, LV_SYMBOL_DOWNLOAD, 0x08869f, "Downloaded", card, true);   /* a darker teal: no tap, so its icon needs 3:1 */
    set_danger(g, LV_SYMBOL_TRASH, "Remove songs not kept offline", sub_srv_clear_cb, NULL);
    opt_note(body, "Songs come down to the Subsonic folder on the card and play from there. They stay out of the music library, so nothing shows twice.");
    abs_srv_group(body);                  /* Audiobookshelf's, beside them (sloppy_abs.inc) */
}
static void sub_refill_srv(void *unused)
{
    (void)unused;
    if (!sub_srv_body) return;
    int32_t y = lv_obj_get_scroll_y(sub_srv_body);
    lv_obj_clean(sub_srv_body);
    sub_srv_fill(sub_srv_body);
    lv_obj_update_layout(sub_srv_body);
    lv_obj_scroll_to_y(sub_srv_body, y, LV_ANIM_OFF);
}
static void sub_srv_deleted(lv_event_t *e) { if (lv_event_get_current_target(e) == sub_srv_obj) sub_srv_obj = sub_srv_body = NULL; }
static void sub_servers_page(void)
{
    if (page_bring_back(sub_srv_obj)) return;
    lv_obj_t *p, *body = page_new("Music servers", &p);
    sub_srv_obj = p;
    sub_srv_body = body;
    lv_obj_add_event_cb(p, sub_srv_deleted, LV_EVENT_DELETE, NULL);
    sub_srv_fill(body);
    page_push(p);
}
/* a server's address, name and password, on the Wi-Fi keyboard */
static void sub_edit_focus(lv_obj_t *ta)
{
    lv_obj_t *all[3] = { sub_url_ta, sub_user_ta, sub_pass_ta };
    for (int i = 0; i < 3; i++) if (all[i] && all[i] != ta) lv_obj_remove_state(all[i], LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (sub_edit_obj) kb_retarget(kb_of_page(sub_edit_obj), ta);
}
static void sub_edit_field_cb(lv_event_t *e) { sub_edit_focus(lv_event_get_current_target(e)); }
static void sub_edit_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sub_edit_obj) return;   /* an older one, gone late */
    if (sub_pass_ta) lv_textarea_set_text(sub_pass_ta, "");
    sub_edit_obj = sub_url_ta = sub_user_ta = sub_pass_ta = sub_test_line = NULL;
    sub_want_test = false;
}
static lv_obj_t *sub_edit_field(lv_obj_t *g, const char *hint)
{
    lv_obj_t *ta = set_field(g);
    lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    sl_ta_hint(ta, sl_tr(hint));
    lv_obj_set_style_text_color(ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ta, sub_edit_field_cb, LV_EVENT_PRESSED, NULL);
    return ta;
}
static void sub_test_say(const char *s, uint32_t ink)
{
    if (!sub_test_line) return;
    lv_label_set_text(sub_test_line, sl_tr(s));
    lv_obj_set_style_text_color(sub_test_line, lv_color_hex(ink), 0);
}
static void sub_edit_go(bool save)
{
    if (!sub_url_ta) return;
    const char *u = sl_ta_text(sub_url_ta), *n = sl_ta_text(sub_user_ta), *pw = sl_ta_text(sub_pass_ta);
    if (!u[0]) { toast("Type the server's address"); sub_edit_focus(sub_url_ta); return; }
    if (!n[0]) { toast("Type your user name"); sub_edit_focus(sub_user_ta); return; }
    if (!pw[0] && !sub_edit_id) { sub_edit_focus(sub_pass_ta); return; }
    if (!sub_wifi_up()) { toast("Join a Wi-Fi network first"); return; }
    char fixed[192];
    if (!sl_sub_url_fix(u, fixed, sizeof fixed)) { sub_test_say(sub_err_words(SL_SUB_E_ADDRESS), SET_RED); return; }
    sub_test_from = sub_st.test_seq;
    if (!sl_sub_signin(u, n, pw, sub_edit_id, save)) { toast_warn("Couldn't try now"); return; }
    sub_want_test = true;
    sub_test_save = save;
    sub_test_say("Signing in\xe2\x80\xa6", C_SUB);
}
static void sub_test_cb(lv_event_t *e) { (void)e; sub_edit_go(false); }
static void sub_save_cb(lv_event_t *e) { (void)e; sub_edit_go(true); }
static void sub_remove_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_sub_remove(sub_edit_id)) { toast_warn("Couldn't change that now"); return; }
    toast("Server removed");
    page_pop();
}
static void sub_use_cb(lv_event_t *e) { (void)e; if (sl_sub_use(sub_edit_id)) { toast("In use now"); page_pop(); } }
static void sub_edit_page(uint32_t id)
{
    if (page_bring_back(sub_edit_obj)) return;
    struct sl_sub_server s;
    memset(&s, 0, sizeof s);
    if (id && !sl_sub_server(id, &s)) id = 0;
    sub_edit_id = id;
    lv_obj_t *p, *body = page_new(id ? s.name : "Add a server", &p);
    sub_edit_obj = p;
    lv_obj_add_event_cb(p, sub_edit_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_bottom(body, WF_KB_H + SL_GUTTER, 0);
    lv_obj_t *g = set_group(body, "ADDRESS");
    sub_url_ta = sub_edit_field(g, "music.example.com or 192.168.1.5:4533");
    lv_textarea_set_max_length(sub_url_ta, 180);
    g = set_group(body, "USER NAME");
    sub_user_ta = sub_edit_field(g, "Your user name");
    lv_textarea_set_max_length(sub_user_ta, 60);
    g = set_group(body, "PASSWORD OR API KEY");
    sub_pass_ta = sub_edit_field(g, id ? "Saved. Type to change it" : "Password");
    lv_textarea_set_password_mode(sub_pass_ta, true);
    lv_textarea_set_max_length(sub_pass_ta, 150);
    if (id) { lv_textarea_set_text(sub_url_ta, s.url); lv_textarea_set_text(sub_user_ta, s.user); }
    sub_test_line = text(body, "", &ss_regular_18, C_SUB);
    lv_obj_set_width(sub_test_line, lv_pct(100));
    lv_label_set_long_mode(sub_test_line, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(sub_test_line, SL_INSET, 0);
    lv_obj_set_style_pad_bottom(sub_test_line, SL_SP_S, 0);
    set_align(sub_test_line);
    if (id)
    {
        char b[160];
        snprintf(b, sizeof b, "%s %s%s%s%s", s.name, s.version, s.open ? "  \xc2\xb7  OpenSubsonic" : "", s.api_key ? "  \xc2\xb7  " : "", s.api_key ? sl_tr("API key") : "");
        lv_label_set_text(sub_test_line, b);
    }
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_OK, 0x3aa856, id ? "Save" : "Sign in", NULL, sub_save_cb, NULL);
    set_action(g, LV_SYMBOL_REFRESH, 0x2d7ff0, "Test connection", NULL, sub_test_cb, NULL);
    if (id && id != sl_sub_active()) set_action(g, LV_SYMBOL_PLAY, SUB_TINT, "Use this server", NULL, sub_use_cb, NULL);
    if (id) set_danger(g, LV_SYMBOL_TRASH, "Remove this server", sub_remove_cb, NULL);
    opt_note(body, "The password stays on the player. Each request sends a one-time token made from it, never the password. Over https the server's certificate isn't checked.");
    wf_kb_build(p, sub_url_ta, KB_PASSWORD, sub_save_cb, id ? "Save" : "Sign in");
    sub_edit_focus(id ? sub_pass_ta : sub_url_ta);
    page_push(p);
}
static bool sub_need_server(void)
{
    if (sl_sub_server(0, NULL)) return true;
    toast("Add a server first.");
    return false;
}

/* ---- the worker's news */
static void sub_poll(void)
{
    sl_sub_poll();
    struct sl_sub_status s;
    sl_sub_status(&s);
    /* the song playing now, if it is the server's */
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    char np[48] = "";
    static char np_path[300];
    static char np_was[48];
    if (id3 && sl_sub_is_cached_path(id3->path))
    {
        if (strcmp(np_path, id3->path)) { strlcpy(np_path, id3->path, sizeof np_path); if (!sl_sub_which(id3->path, NULL, np_was, sizeof np_was)) np_was[0] = '\0'; }
        snprintf(np, sizeof np, "%s", np_was);
    }
    else np_path[0] = '\0';
    bool np_moved = strcmp(np, sub_np_id) != 0;
    snprintf(sub_np_id, sizeof sub_np_id, "%s", np);
    if (s.seq == sub_st.seq && !np_moved) { sub_ses_tick(); return; }
    struct sl_sub_status was = sub_st;
    sub_st = s;
    sub_ses_tick();
    if (s.art_seq != was.art_seq) sub_art_landed();
    /* the song coming down, its percent */
    if (sub_prog_lbl && !strcmp(s.dl_id, sub_prog_id))
    {
        char pc[16];
        snprintf(pc, sizeof pc, "%d%%", s.dl_total > 0 ? (int)(s.dl_got * 100 / s.dl_total) : 0);
        lv_label_set_text(sub_prog_lbl, pc);
    }
    /* answers */
    bool card = s.cache_seq != was.cache_seq || strcmp(s.dl_id, was.dl_id) || s.online != was.online || np_moved;
    for (struct sub_view *v = sub_views; v; v = v->next)
    {
        bool fill = false;
        if (v->waiting && s.list_seq != was.list_seq)
        {
            struct sl_sub_list l;
            if (sl_sub_take(v->req, &l)) { sl_sub_list_free(&v->l); v->l = l; v->waiting = false; fill = true; }
        }
        if (!fill && card && !v->waiting && v->what != SV_HOME) fill = true;
        if (!fill && v->what == SV_HOME && (s.online != was.online || s.cache_seq != was.cache_seq || s.servers_seq != was.servers_seq))
        {
            /* the peek again: from the server, or the card's when offline */
            struct sl_sub_server srv;
            if (sl_sub_server(0, &srv)) { v->what = SV_HOME; sub_ask(v); }
            fill = true;
        }
        if (fill) sub_refill(v);
    }
    if (sub_srv_body && (s.servers_seq != was.servers_seq || s.cache_seq != was.cache_seq)) sub_refill_srv(NULL);
    /* a sign-in's answer */
    if (sub_want_test && s.test_seq != sub_test_from)
    {
        sub_want_test = false;
        if (!s.test_err)
        {
            char b[200];
            snprintf(b, sizeof b, "%s %s%s%s%s", sl_tr("Connected:"), s.test_kind[0] ? s.test_kind : "Subsonic", s.test_version[0] ? " " : "", s.test_version,
                     s.test_open ? "  \xc2\xb7  OpenSubsonic" : "");
            sub_test_say(b, 0x3aa856);
            if (s.test_saved)
            {
                /* the first server: its folder stays out of the music library (Library > Folders to scan lets it in) */
                struct sl_sub_server all[SL_SUB_SERVERS];
                if (sl_sub_servers(all, SL_SUB_SERVERS) == 1 && !tagcache_folder_listed(sl_sub_folder))
                {
                    tagcache_set_folder_excluded(sl_sub_folder, true);
                    settings_save();
                }
                toast("Signed in");
                if (sub_edit_obj && npages && pages[npages - 1] == sub_edit_obj) page_pop();
                if (sub_home_obj)
                {
                    for (struct sub_view *v = sub_views; v; v = v->next)
                        if (v->page == sub_home_obj)
                        {
                            struct sl_sub_server srv;
                            if (sl_sub_server(0, &srv))
                            {
                                lv_obj_t *hl = lv_obj_get_child(v->page, 0);
                                (void)hl;
                                snprintf(v->title, sizeof v->title, "%s", srv.name);
                            }
                            sub_ask(v);
                            sub_refill(v);
                        }
                }
            }
        }
        else
        {
            char b[220];
            if (s.test_err == SL_SUB_E_SERVER && s.test_why[0]) snprintf(b, sizeof b, "%s %s", sl_tr("The server said:"), s.test_why);
            else snprintf(b, sizeof b, "%s", sl_tr(sub_err_words(s.test_err)));
            sub_test_say(b, SET_RED);
        }
    }
    if (s.dl_seq != was.dl_seq && s.dl_err == SL_SUB_E_FULL) toast_warn("The room for downloaded songs is full of songs kept offline.");
}
