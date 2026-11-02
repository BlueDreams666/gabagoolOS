/* Podcasts: the shows followed as a grid of their pictures, what is being
 * listened to, each show's episodes (full titles, heard or not, on the
 * card or not), finding shows in the directory or by a feed's address,
 * OPML in and out, and the downloads. The feeds, the directory and the
 * downloads are sloppy_podjob.inc's worker; this page only asks and
 * looks. An episode plays from the card where it was left, at its show's
 * speed, a little back after a pause (Playback > Rewind after a pause),
 * and the skip keys move 30 seconds as they do in a book. Included by
 * sloppy.c after the Settings components. */
#include "sloppy_podcast.h"
#define POD_PER_PAGE 40                   /* episode rows a page: a show of 300 pages through them */
static void fm_acts_red(void);            /* the sheet's last row in red (sloppy_files.inc) */
#define POD_TINT 0x8e5cf0                 /* Podcasts' own colour: its icon, a show with no picture */

static struct sl_pod_status pod_st;        /* as last seen by pod_poll */
static lv_obj_t *pod_page_obj, *pod_show_obj, *pod_dl_obj, *pod_found_obj, *pod_prev_obj, *pod_url_ta, *pod_find_ta;
static bool pod_want_found, pod_want_prev, pod_want_follow;   /* a page waits on the worker's answer */
/* the worker's counts when each was asked for: the answer is the next one, however
 * soon it comes (pod_st is only ever moved on by pod_poll, so nothing is missed) */
static unsigned pod_found_from, pod_prev_from, pod_done_from;
static struct sl_pod_status pod_status_now(void) { struct sl_pod_status s; sl_pod_status(&s); return s; }
static char pod_find_q[96];

/* ---- pictures: the worker's squares, read by a drive of our own that
 * opens the card as the C library sees it (the worker wrote them so) */
static void *pod_fs_open(lv_fs_drv_t *d, const char *path, lv_fs_mode_t mode)
{
    (void)d;
    char real[512];
    if (mode != LV_FS_MODE_RD || snprintf(real, sizeof real, "%s%s", sl_net_card_root(), path) >= (int)sizeof real) return NULL;
    return fopen(real, "rb");
}
static lv_fs_res_t pod_fs_close(lv_fs_drv_t *d, void *f) { (void)d; fclose(f); return LV_FS_RES_OK; }
static lv_fs_res_t pod_fs_read(lv_fs_drv_t *d, void *f, void *buf, uint32_t n, uint32_t *br)
{
    (void)d;
    *br = (uint32_t)fread(buf, 1, n, f);
    return ferror(f) ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}
static lv_fs_res_t pod_fs_seek(lv_fs_drv_t *d, void *f, uint32_t pos, lv_fs_whence_t w)
{
    (void)d;
    return fseek(f, (long)pos, w == LV_FS_SEEK_CUR ? SEEK_CUR : w == LV_FS_SEEK_END ? SEEK_END : SEEK_SET) ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}
static lv_fs_res_t pod_fs_tell(lv_fs_drv_t *d, void *f, uint32_t *pos)
{
    (void)d;
    long p = ftell(f);
    if (p < 0) return LV_FS_RES_FS_ERR;
    *pos = (uint32_t)p;
    return LV_FS_RES_OK;
}
static void pod_fs_init(void)
{
    static lv_fs_drv_t drv;
    static bool done;
    if (done) return;
    done = true;
    lv_fs_drv_init(&drv);
    drv.letter = 'P';
    drv.open_cb = pod_fs_open;
    drv.close_cb = pod_fs_close;
    drv.read_cb = pod_fs_read;
    drv.seek_cb = pod_fs_seek;
    drv.tell_cb = pod_fs_tell;
    lv_fs_drv_register(&drv);
}
/* a picture leaves LVGL's image cache with its page: a show's picture is
 * 94 KB decoded, and the cache would keep it after the page has gone */
static void pod_art_gone(lv_event_t *e)
{
    lv_obj_t *img = lv_event_get_current_target(e);
    const void *src = lv_image_get_src(img);
    if (src) lv_image_cache_drop(src);
}
/* a show's own colour: its picture's, or one from its name */
static uint32_t pod_tint(const struct sl_pod_show *s)
{
    if (s && s->tint) return s->tint;
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)((s ? s->id : 0) % 360), 55, 62);
    return lv_color_to_u32(c) & 0xffffff;
}
/* A show's square: its picture when the worker has made it, else its
 * colour with the podcasts mark, as an album with no cover */
static lv_obj_t *pod_art(lv_obj_t *parent, const struct sl_pod_show *s, int size)
{
    uint32_t tint = pod_tint(s);
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : size > 100 ? SL_R_MD : SL_R_SM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_mix(lv_color_black(), lv_color_hex(tint), 150), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    if (s && s->art)
    {
        pod_fs_init();
        char name[96], src[100];
        sl_pod_art_name(name, sizeof name, s->id, size > SL_POD_ART_ROW ? SL_POD_ART_BIG : SL_POD_ART_ROW);
        snprintf(src, sizeof src, "P:%s", name);
        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, src);
        lv_obj_add_event_cb(img, pod_art_gone, LV_EVENT_DELETE, NULL);
        int px = size > SL_POD_ART_ROW ? SL_POD_ART_BIG : SL_POD_ART_ROW;
        if (px != size) lv_image_set_scale(img, (uint32_t)(256 * size / px));
        lv_obj_center(img);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
        /* LVGL's software drawing ignores an image's own radius: the box
         * clips it (a layer only for the strips its corners are in) */
        lv_obj_set_style_clip_corner(c, true, 0);
    }
    else
    {
        lv_obj_t *n = sl_icon(c, IC_PODCAST, size * 3 / 8, lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(tint), 190)) & 0xffffff, tint);
        lv_obj_center(n);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(n, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    return c;
}

/* ---- words */
/* "21 Oct", or "21 Oct 2024" from another year */
static void pod_date(char *b, size_t n, uint32_t t)
{
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    b[0] = '\0';
    if (!t) return;
    time_t at = (time_t)t, now = time(NULL);
    struct tm tm, tn;
    if (!gmtime_r(&at, &tm) || !gmtime_r(&now, &tn) || tm.tm_mon < 0 || tm.tm_mon > 11) return;
    if (global_settings.sloppy_language == SL_LANG_ZH || global_settings.sloppy_language == SL_LANG_JA)   /* Chinese, Japanese: 10月21日, 2024年10月21日 */
    {
        if (tm.tm_year == tn.tm_year) snprintf(b, n, "%s%d\xe6\x97\xa5", sl_tr(mon[tm.tm_mon]), tm.tm_mday);
        else snprintf(b, n, "%d\xe5\xb9\xb4%s%d\xe6\x97\xa5", tm.tm_year + 1900, sl_tr(mon[tm.tm_mon]), tm.tm_mday);
    }
    else if (tm.tm_year == tn.tm_year) snprintf(b, n, "%d %s", tm.tm_mday, sl_tr(mon[tm.tm_mon]));
    else snprintf(b, n, "%d %s %d", tm.tm_mday, sl_tr(mon[tm.tm_mon]), tm.tm_year + 1900);
}
/* "54 min", "1 h 12 min" */
static void pod_dur(char *b, size_t n, uint32_t secs)
{
    uint32_t min = (secs + 30) / 60;
    if (!secs) b[0] = '\0';
    else if (min >= 60 && min % 60) snprintf(b, n, sl_tr("%lu h %lu min"), (unsigned long)(min / 60), (unsigned long)(min % 60));
    else if (min >= 60) snprintf(b, n, sl_tr("%lu h"), (unsigned long)(min / 60));
    else snprintf(b, n, sl_tr("%lu min"), (unsigned long)(min ? min : 1));
}
static void pod_join(char *b, size_t n, const char *s)
{
    if (!s || !s[0]) return;
    size_t k = strlen(b);
    snprintf(b + k, n - k, "%s%s", k ? " \xc2\xb7 " : "", s);
}
static void pod_size(char *b, size_t n, long long bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) snprintf(b, n, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 10LL * 1024 * 1024) snprintf(b, n, "%lld MB", (bytes + 512 * 1024) / (1024 * 1024));
    else snprintf(b, n, "%.1f MB", (double)bytes / (1024.0 * 1024));
}

/* ---- the grid of shows */
static void pod_show_page(uint32_t id);
static void pod_cell_cb(lv_event_t *e) { pod_show_page((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }
/* the count of episodes not heard, on the picture's corner */
static void pod_badge(lv_obj_t *art, int n)
{
    if (n <= 0) return;
    char b[16];
    snprintf(b, sizeof b, "%d", n > 99 ? 99 : n);
    lv_obj_t *d = box(art);
    lv_obj_set_size(d, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_min_width(d, 30, 0);
    lv_obj_set_style_pad_hor(d, 9, 0);
    lv_obj_set_style_radius(d, sl_zune() ? 0 : SL_R_FULL, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    sl_elevate(d, SL_E1, 0);
    uint32_t ink = sl_accent_ink() == 0xffffff ? 0xfefefe : sl_accent_ink();
    lv_obj_t *l = text(d, b, &ss_semibold_18, ink);
    lv_obj_center(l);
    sl_align(d, LV_ALIGN_TOP_RIGHT, -8, 8);
}
static void pod_grid(lv_obj_t *body, const struct sl_pod_show *shows, int n)
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
    for (int k = 0; k < n; k++)
    {
        const struct sl_pod_show *s = &shows[k];
        lv_obj_t *cell = box(g);
        /* the whole name, on two lines at most: one line wasted most of them */
        int th = set_text_h(s->title, &ss_semibold_18, cw) > lh1 ? 2 * lh1 : lh1;
        lv_obj_set_size(cell, cw, cw + 8 + th + 2 + lh2 + 6);
        lv_obj_set_style_transform_pivot_x(cell, cw / 2, 0);
        lv_obj_set_style_transform_pivot_y(cell, cw / 2, 0);
        press_motion(cell);
        lv_obj_set_style_transform_scale(cell, 246, LV_STATE_PRESSED);
        lv_obj_t *a = pod_art(cell, s, cw);
        sl_elevate(a, sl_zune() ? SL_E0 : SL_E1, 0);
        pod_badge(a, s->n_unheard);
        lv_obj_t *t = text(cell, s->title, &ss_semibold_18, 0xffffff);
        lv_obj_set_size(t, cw, th);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        set_align(t);
        sl_label_deep(t);
        lv_obj_set_pos(t, 0, cw + 8);
        char b[96], c[48];
        b[0] = '\0';
        if (s->n_unheard) { sl_count(c, sizeof c, s->n_unheard, "%s not heard", "%s not heard"); pod_join(b, sizeof b, c); }
        else { sl_count(c, sizeof c, s->n_eps, "%s episode", "%s episodes"); pod_join(b, sizeof b, c); }
        if (s->n_dl) { sl_count(c, sizeof c, s->n_dl, "%s on the card", "%s on the card"); pod_join(b, sizeof b, c); }
        lv_obj_t *l = text(cell, b, &ss_regular_16, C_SUB);
        lv_obj_set_size(l, cw, lh2);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        set_align(l);
        sl_label_deep(l);
        lv_obj_set_pos(l, 0, cw + 8 + th + 2);
        on_click(cell, pod_cell_cb, (void *)(uintptr_t)s->id);
    }
}

/* ---- what is being listened to: the latest few, each a tap from where it was */
static void pod_play_progress(const struct sl_pod_progress *p);
static struct sl_pod_progress pod_cont[3];
static int pod_ncont;
static void pod_cont_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < pod_ncont) pod_play_progress(&pod_cont[i]);
}
/* "23 min left" */
static void pod_left(char *b, size_t n, uint32_t pos_ms, uint32_t dur_ms)
{
    b[0] = '\0';
    if (!dur_ms || pos_ms >= dur_ms) return;
    char d[40];
    pod_dur(d, sizeof d, (dur_ms - pos_ms) / 1000);
    snprintf(b, n, "%s %s", d, sl_tr("left"));
}
static lv_obj_t *pod_cont_row(lv_obj_t *g, int i)
{
    const struct sl_pod_progress *p = &pod_cont[i];
    struct sl_pod_show s;
    if (!sl_pod_show(p->show, &s)) memset(&s, 0, sizeof s);
    char line[200], left[48];
    snprintf(line, sizeof line, "%s", s.title);
    pod_left(left, sizeof left, p->pos_ms, p->dur_ms);
    pod_join(line, sizeof line, left);
    int art = SL_POD_ART_ROW, tx = SL_INSET + art + SL_GUTTER;
    int lw = SET_INNER - tx - SL_INSET - set_chevron_w() - SL_SP_S;
    int lh1 = lv_font_get_line_height(&ss_semibold_21), lh2 = lv_font_get_line_height(&ss_regular_16);
    int bar_h = 4, h = LV_MAX(art + 2 * SET_PAD_V, lh1 + lh2 + SL_SP_S + bar_h + 2 * SET_PAD_V);
    lv_obj_t *r = set_row_box(g, h);
    lv_obj_t *a = text(r, p->title, &ss_semibold_21, C_TEXT);
    lv_obj_set_size(a, lw, lh1);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    int y = (h - lh1 - lh2 - SL_SP_S - bar_h) / 2;
    set_place(a, tx, y, lw);
    lv_obj_t *b = text(r, line, &ss_regular_16, C_SUB);
    lv_obj_set_size(b, lw, lh2);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    set_align(b);
    sl_label_deep(b);
    set_place(b, tx, y + lh1, lw);
    /* how far: a hairline bar, the part heard in the accent */
    lv_obj_t *bar = lv_bar_create(r);
    lv_obj_set_size(bar, lw, bar_h);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, p->dur_ms ? (int32_t)((uint64_t)p->pos_ms * 1000 / p->dur_ms) : 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, sl_theme() == 1 ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_base_dir(bar, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    set_place(bar, tx, y + lh1 + lh2 + SL_SP_S, lw);
    lv_obj_t *c = pod_art(r, &s, art);
    set_place(c, SL_INSET, (h - art) / 2, art);
    lv_obj_t *ch = text(r, set_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    lv_obj_align(ch, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    set_sep(g, r, tx);
    on_click(r, pod_cont_cb, (void *)(intptr_t)i);
    return r;
}

/* ---- the Podcasts page */
static void pod_find_page(void);
static void pod_url_page(void);
static void pod_opml_page(void);
static void pod_downloads_page(void);
static void pod_find_cb(lv_event_t *e) { (void)e; pod_find_page(); }
static void pod_url_cb(lv_event_t *e) { (void)e; pod_url_page(); }
static void pod_opml_cb(lv_event_t *e) { (void)e; pod_opml_page(); }
static void pod_downloads_cb(lv_event_t *e) { (void)e; pod_downloads_page(); }
static bool pod_need_wifi(void)
{
    if (wf_online()) return true;
    toast("Connect to Wi-Fi first");
    return false;
}
static void pod_refresh_all_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_need_wifi()) return;
    if (sl_pod_refresh(0)) toast("Looking for new episodes");
    else toast_warn("Could not look for new episodes");
}
static void pod_export_cb(lv_event_t *e)
{
    (void)e;
    char path[128], b[200];
    if (!sl_pod_opml_export(path, sizeof path)) { toast_warn("Could not save the list"); return; }
    snprintf(b, sizeof b, "%s %s", sl_tr("Saved to"), path);
    toast(b);
}
/* the downloads under way, as the page's line says them */
static void pod_dl_words(char *b, size_t n)
{
    b[0] = '\0';
    int waiting = pod_st.queued + (pod_st.dl_show ? 1 : 0);
    if (!waiting) return;
    sl_count(b, n, waiting, "%s coming", "%s coming");
    if (pod_st.dl_show && pod_st.dl_total > 0)
    {
        char p[24];
        snprintf(p, sizeof p, "%d%%", (int)(pod_st.dl_got * 100 / pod_st.dl_total));
        pod_join(b, n, p);
    }
    else if (!pod_st.online) pod_join(b, n, sl_tr("Waiting for Wi-Fi"));
}
static lv_obj_t *pod_dl_row, *pod_page_body;
static void pod_page_deleted(lv_event_t *e) { (void)e; pod_page_obj = pod_page_body = NULL; pod_dl_row = NULL; }
/* a page's words again, where it is scrolled to: the worker changed something under it */
static void pod_refill(lv_obj_t *body, void (*fill)(lv_obj_t *))
{
    if (!body) return;
    int32_t y = lv_obj_get_scroll_y(body);
    lv_obj_clean(body);
    fill(body);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_y(body, y, LV_ANIM_OFF);
}
static void pod_page_fill(lv_obj_t *body)
{
    pod_dl_row = NULL;
    int n = 0;
    struct sl_pod_show *shows = sl_pod_shows_copy(&n);
    if (!n)
    {
        lv_obj_t *c = sl_empty(body, IC_PODCAST, 96, "No podcasts yet.",
                               "Find a show in the directory, add one by its feed's address, or bring your list from another app.", 48);
        sl_empty_action(c, LV_SYMBOL_PLUS "  Find a podcast", pod_find_cb, NULL);
        lv_obj_t *g = set_group(body, "Other ways");
        set_link_sub(g, LV_SYMBOL_EDIT, 0x2d7ff0, "Add by address", "Any podcast feed: RSS or Atom", NULL, pod_url_cb, NULL);
        set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x3aa856, "Import a list", "An OPML file from another podcast app", NULL, pod_opml_cb, NULL);
        free(shows);
        return;
    }
    /* what is being listened to */
    pod_ncont = sl_pod_progress_list(pod_cont, 3);
    if (pod_ncont)
    {
        lv_obj_t *g = set_group(body, "Continue listening");
        for (int i = 0; i < pod_ncont; i++) pod_cont_row(g, i);
    }
    char b[96];
    sl_count(b, sizeof b, n, "%s SHOW", "%s SHOWS");
    lv_obj_t *hd = section(body, sl_case(b));
    (void)hd;
    pod_grid(body, shows, n);
    free(shows);
    lv_obj_t *g = set_group(body, "Library");
    char dl[64];
    pod_dl_words(dl, sizeof dl);
    pod_dl_row = set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x0aa9c7, "Downloads", dl[0] ? dl : sl_tr("On the card, and coming"), dl[0] ? dl : NULL,
                              pod_downloads_cb, NULL);
    set_link_sub(g, LV_SYMBOL_PLUS, POD_TINT, "Find a podcast", "The directory of every show", NULL, pod_find_cb, NULL);
    set_link_sub(g, LV_SYMBOL_EDIT, 0x2d7ff0, "Add by address", "Any podcast feed: RSS or Atom", NULL, pod_url_cb, NULL);
    set_link_sub(g, LV_SYMBOL_COPY, 0x3aa856, "Import or export a list", "OPML, for other podcast apps", NULL, pod_opml_cb, NULL);
    lv_obj_t *r = set_action(g, LV_SYMBOL_REFRESH, 0xeb743b, "Look for new episodes", NULL, pod_refresh_all_cb, NULL);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, 2), LV_OBJ_FLAG_HIDDEN);   /* does, does not open: no chevron */
    opt_note(body, "Episodes download to the Podcasts folder on the card, a folder for each show.");
}
static void podcasts_page(void)
{
    pod_fs_init();
    if (page_bring_back(pod_page_obj)) return;   /* once on the stack */
    lv_obj_t *p, *body = page_new("Podcasts", &p);
    pod_page_obj = p;
    pod_page_body = body;
    lv_obj_add_event_cb(p, pod_page_deleted, LV_EVENT_DELETE, NULL);
    header_button(p, LV_SYMBOL_PLUS, pod_find_cb, NULL);
    pod_page_fill(body);
    page_push(p);
}

/* ---- a show's page */
static struct sl_pod_show pod_sh;          /* the show on its page */
static struct sl_pod_eps pod_eps;          /* its episodes, newest first */
static int *pod_vis, pod_nvis;             /* the ones the filter shows, as indexes into pod_eps */
static int pod_filter, pod_start;          /* 0 all, 1 not heard, 2 on the card; the page of rows */
static lv_obj_t *pod_rows;                 /* the rows' box, filled again as things change */
static void pod_rows_fill(void);
static void pod_vis_make(void)
{
    free(pod_vis);
    pod_vis = malloc(((size_t)pod_eps.n + 1) * sizeof *pod_vis);
    pod_nvis = 0;
    for (int i = 0; pod_vis && i < pod_eps.n; i++)
    {
        const struct sl_pod_ep *p = &pod_eps.ep[i];
        if (pod_filter == 1 && (p->flags & SL_POD_HEARD)) continue;
        if (pod_filter == 2 && p->dl == SL_POD_DL_NONE) continue;
        pod_vis[pod_nvis++] = i;
    }
}
static bool pod_ep_playing(const struct sl_pod_ep *p)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    char path[400];
    return id3 && p->file[0] && sl_pod_ep_path(pod_sh.id, p, path, sizeof path) && !strcmp(path, id3->path);
}
/* the line under an episode's title: when, how long, and where it stands */
static void pod_ep_line(const struct sl_pod_ep *p, char *b, size_t n)
{
    char d[48];
    b[0] = '\0';
    pod_date(d, sizeof d, p->date);
    pod_join(b, n, d);
    struct sl_pod_progress pr;
    bool part = !(p->flags & SL_POD_HEARD) && sl_pod_progress_get(pod_sh.id, p->key, &pr) && pr.pos_ms > 0;
    if (part) pod_left(d, sizeof d, pr.pos_ms, pr.dur_ms ? pr.dur_ms : p->dur * 1000);
    else pod_dur(d, sizeof d, p->dur);
    pod_join(b, n, d);
    if (p->dl == SL_POD_DL_ACTIVE)
    {
        if (pod_st.dl_total > 0 && pod_st.dl_key == p->key) snprintf(d, sizeof d, "%s %d%%", sl_tr("Downloading"), (int)(pod_st.dl_got * 100 / pod_st.dl_total));
        else snprintf(d, sizeof d, "%s", sl_tr("Downloading"));
        pod_join(b, n, d);
    }
    else if (p->dl == SL_POD_DL_QUEUED) pod_join(b, n, sl_tr("Waiting to download"));
    else if (p->dl == SL_POD_DL_DONE) pod_join(b, n, sl_tr("On the card"));
    if (p->flags & SL_POD_HEARD) pod_join(b, n, sl_tr("Heard"));
}
static void pod_ep_sheet(int i);
static void pod_play_eps(const int *idx, int n);
static void pod_dl_one(int i);
static void pod_ep_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (sel_kind_on(SEL_EPISODES))
    {
        lv_event_stop_processing(e);
        if (g_dragged && lv_indev_active() == touch) return;
        if (sel_toggle(i)) pod_rows_fill();
        return;
    }
    if (i < 0 || i >= pod_eps.n) return;
    if (pod_eps.ep[i].dl == SL_POD_DL_DONE) pod_play_eps(&i, 1);
    else pod_ep_sheet(i);
}
static void pod_ep_long_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!sel_on() && pod_show_obj) sel_start(SEL_EPISODES, &pod_eps, pod_show_obj, i);
}
static void pod_ep_more_cb(lv_event_t *e) { pod_ep_sheet((int)(intptr_t)lv_event_get_user_data(e)); }
static void pod_ep_get_cb(lv_event_t *e) { pod_dl_one((int)(intptr_t)lv_event_get_user_data(e)); }
/* One episode: a dot for one not heard yet (the bars while it plays), its
 * whole title however long, a line under it, and its button: download,
 * or more */
static lv_obj_t *pod_ep_row(lv_obj_t *parent, int i)
{
    const struct sl_pod_ep *p = &pod_eps.ep[i];
    bool heard = p->flags & SL_POD_HEARD, now = pod_ep_playing(p), picking = sel_kind_on(SEL_EPISODES);
    int lead = sl_zune() ? 10 : SL_ROW_NUM_X - 12, tw = W - 2 * SL_GUTTER - lead - SL_ROW_END;
    int lh2 = lv_font_get_line_height(&ss_regular_18);
    int th = set_text_h(p->title, &ss_semibold_21, tw);
    int maxh = 4 * lv_font_get_line_height(&ss_semibold_21);   /* a title of a paragraph: four lines, then dots */
    if (th > maxh) th = maxh;
    int h = LV_MAX(SL_ROW_H, th + lh2 + 2 * SL_GUTTER), ty = (h - th - lh2) / 2;
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_size(r, lv_pct(100), h);
    if (now) sl_align(now_bars(r, C_ACCENT), LV_ALIGN_LEFT_MID, 8, 0);
    else if (!heard && !sl_zune())
    {
        lv_obj_t *d = box(r);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        sl_align(d, LV_ALIGN_TOP_LEFT, (lead - 10) / 2, ty + (lv_font_get_line_height(&ss_semibold_21) - 10) / 2);
    }
    lv_obj_t *t1 = text(r, p->title, &ss_semibold_21, now ? C_ACCENT_TXT : heard ? C_SUB : 0xffffff);
    lv_obj_set_size(t1, tw, th);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    set_align(t1);
    sl_label_deep(t1);
    sl_place(t1, lead, ty);
    char b[200];
    pod_ep_line(p, b, sizeof b);
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    lv_obj_set_size(t2, tw, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    set_align(t2);
    sl_label_deep(t2);
    sl_place(t2, lead, ty + th);
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    on_click(r, pod_ep_cb, (void *)(intptr_t)i);
    lv_obj_add_event_cb(r, pod_ep_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    if (picking)
    {
        sl_align(sel_ring(r), LV_ALIGN_RIGHT_MID, -14, 0);
        sel_paint_row(r, sel_has(i));
    }
    else
    {
        bool get = p->dl == SL_POD_DL_NONE;
        lv_obj_t *more = icon_btn(r, get ? LV_SYMBOL_DOWNLOAD : NULL, SL_ROW_MORE_W, get ? pod_ep_get_cb : pod_ep_more_cb, (void *)(intptr_t)i);
        sl_align(more, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN, 0);
    }
    return r;
}
static void pod_page_cb(lv_event_t *e)
{
    pod_start = (int)(intptr_t)lv_event_get_user_data(e);
    pod_rows_fill();
    if (pod_rows) lv_obj_scroll_to_view(pod_rows, LV_ANIM_OFF);
}
static void pod_rows_fill(void)
{
    if (!pod_rows) return;
    lv_obj_clean(pod_rows);
    if (pod_start >= pod_nvis) pod_start = 0;
    int end = pod_start + POD_PER_PAGE < pod_nvis ? pod_start + POD_PER_PAGE : pod_nvis;
    for (int k = pod_start; k < end; k++) pod_ep_row(pod_rows, pod_vis[k]);
    if (!pod_nvis)
    {
        lv_obj_t *c = sl_empty(pod_rows, IC_PODCAST, 72, pod_filter == 2 ? "Nothing on the card yet." : pod_filter == 1 ? "All heard." : "No episodes yet.",
                               pod_filter == 2 ? "Download an episode with the arrow beside it." : pod_filter == 1 ? "Every episode here has been heard." :
                               "The feed has none, or could not be read. Look again later.", 24);
        (void)c;
    }
    if (pod_start || end < pod_nvis)
    {
        lv_obj_t *bar = sl_pager(pod_rows);
        if (pod_start) sl_pager_button(bar, LV_SYMBOL_LEFT "  Newer", false, pod_page_cb, pod_start - POD_PER_PAGE);
        if (end < pod_nvis) sl_pager_button(bar, "Older  " LV_SYMBOL_RIGHT, true, pod_page_cb, end);
    }
}
/* the list again from the file: a download or a mark changed it */
static void pod_refill(lv_obj_t *body, void (*fill)(lv_obj_t *));
static lv_obj_t *pod_show_body;
static void pod_show_fill(lv_obj_t *body);
static void pod_reload(void)
{
    if (!pod_sh.id || !pod_show_obj) return;
    struct sl_pod_eps e;
    if (!sl_pod_eps_load(pod_sh.id, &e)) return;
    sl_pod_eps_free(&pod_eps);
    pod_eps = e;
    sl_pod_show(pod_sh.id, &pod_sh);
    pod_vis_make();
    if (pod_show_body) pod_refill(pod_show_body, pod_show_fill);
}
static void pod_filter_pick(int k)
{
    if (k == pod_filter) return;
    pod_filter = k;
    pod_start = 0;
    pod_vis_make();
    pod_refill(pod_show_body, pod_show_fill);   /* the count row says the filter */
}
static void pod_filter_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[] = { "All episodes", "Not heard", "On the card" };
    pick_sheet("Show", names, 3, pod_filter, pod_filter_pick);
}
/* ---- playing */
static struct sl_pod_progress pod_resume_pr;
/* an episode's place: where it was left, a little back when it has been a while */
static uint32_t pod_where(uint32_t show, const struct sl_pod_ep *p)
{
    struct sl_pod_progress pr;
    /* a place is kept only until it is heard to the end: one there is being listened to (again) */
    if (!sl_pod_progress_get(show, p->key, &pr)) return 0;
    uint32_t pos = pr.pos_ms, now = (uint32_t)time(NULL);
    if (global_settings.sloppy_book_rewind && pr.when && now > pr.when)
    {
        uint32_t away = now - pr.when > 4000000u ? 4000000000u : (now - pr.when) * 1000u;
        long back = sl_book_rewind_ms(away);
        pos = pos > (uint32_t)back ? pos - (uint32_t)back : 0;
    }
    return pos;
}
/* several of a show's episodes, the first from where it was left */
static void pod_play_eps(const int *idx, int n)
{
    char dir[400], path[400];
    int first = -1;
    for (int k = 0; k < n && first < 0; k++) if (pod_eps.ep[idx[k]].dl == SL_POD_DL_DONE) first = idx[k];
    if (first < 0) { toast("Download it first: it plays from the card"); return; }
    snprintf(dir, sizeof dir, "%s/%s", sl_pod_folder, pod_sh.folder);
    if (playlist_create(dir, NULL) < 0) { toast_warn("Could not play this episode"); return; }
    int queued = 0;
    for (int k = 0; k < n; k++)
    {
        const struct sl_pod_ep *p = &pod_eps.ep[idx[k]];
        if (p->dl != SL_POD_DL_DONE || !sl_pod_ep_path(pod_sh.id, p, path, sizeof path)) continue;
        if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, false) >= 0) queued++;
    }
    playlist_sync(NULL);
    if (!queued) { toast_warn("Could not play this episode"); return; }
    playlist_start(0, pod_where(pod_sh.id, &pod_eps.ep[first]), 0);
    np_show_auto();
}
static void pod_play_progress(const struct sl_pod_progress *p)
{
    struct sl_pod_show s;
    struct sl_pod_eps e;
    if (!sl_pod_show(p->show, &s) || !sl_pod_eps_load(p->show, &e)) { toast_warn("Could not play this episode"); return; }
    int at = -1;
    for (int i = 0; i < e.n && at < 0; i++) if (e.ep[i].key == p->key) at = i;
    if (at < 0 || e.ep[at].dl != SL_POD_DL_DONE)
    {
        sl_pod_eps_free(&e);
        toast_warn("This episode is no longer on the card");
        return;
    }
    /* played as its show's page would: the show's list in hand */
    sl_pod_eps_free(&pod_eps);
    pod_eps = e;
    pod_sh = s;
    pod_vis_make();
    pod_play_eps(&at, 1);
    if (!pod_show_obj) { sl_pod_eps_free(&pod_eps); free(pod_vis); pod_vis = NULL; pod_nvis = 0; }   /* no page holds them */
}
static void pod_dl_one(int i)
{
    if (i < 0 || i >= pod_eps.n) return;
    if (!sl_pod_download(pod_sh.id, &pod_eps.ep[i].key, 1)) { toast("Already on the card or on its way"); return; }
    toast(wf_online() ? "Downloading" : "It downloads when Wi-Fi is on");
    pod_reload();
}
/* the episode's sheet: what it can do now */
static int pod_sheet_i;
enum { PA_PLAY, PA_GET, PA_CANCEL, PA_DELETE, PA_HEARD, PA_UNHEARD, PA_SELECT };
static void pod_sheet_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e), i = pod_sheet_i;
    acts_close();
    if (i < 0 || i >= pod_eps.n) return;
    uint32_t key = pod_eps.ep[i].key;
    switch (k)
    {
        case PA_PLAY: pod_play_eps(&i, 1); break;
        case PA_GET: pod_dl_one(i); break;
        case PA_CANCEL: sl_pod_cancel(pod_sh.id, key); toast("Download stopped"); pod_reload(); break;
        case PA_DELETE:
            if (pod_ep_playing(&pod_eps.ep[i])) audio_stop();   /* not from under the player */
            if (sl_pod_delete(pod_sh.id, &key, 1)) toast("Download deleted");
            break;
        case PA_HEARD: case PA_UNHEARD:
            if (!sl_pod_mark(pod_sh.id, &key, 1, k == PA_HEARD)) { toast_warn("Could not save that"); break; }
            if (k == PA_HEARD) sl_pod_finished(pod_sh.id, key);
            pod_reload();
            break;
        case PA_SELECT: if (pod_show_obj) sel_start(SEL_EPISODES, &pod_eps, pod_show_obj, i); break;
    }
}
static void pod_ep_sheet(int i)
{
    if (i < 0 || i >= pod_eps.n) return;
    const struct sl_pod_ep *p = &pod_eps.ep[i];
    pod_sheet_i = i;
    acts_begin();
    /* the head: the whole title, and its line */
    lv_obj_t *hd = box(acts);
    lv_obj_set_size(hd, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(hd, 6, 0);
    lv_obj_set_style_pad_bottom(hd, 12, 0);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = text(hd, p->title, &ss_semibold_23, 0xffffff);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(t, 4 * lv_font_get_line_height(&ss_semibold_23), 0);
    set_align(t);
    char b[200];
    pod_ep_line(p, b, sizeof b);
    lv_obj_t *l = text(hd, b, &ss_regular_16, C_SUB);
    lv_obj_set_width(l, lv_pct(100));
    label_lines(l, 1);
    set_align(l);
    lv_obj_t *sep = box(acts);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);
    lv_obj_set_style_bg_opa(sep, sl_zune() ? LV_OPA_COVER : 36, 0);
    struct sl_pod_progress pr;
    bool part = !(p->flags & SL_POD_HEARD) && sl_pod_progress_get(pod_sh.id, p->key, &pr) && pr.pos_ms > 0;
    if (p->dl == SL_POD_DL_DONE) acts_row_cb(LV_SYMBOL_PLAY, part ? "Resume" : "Play", pod_sheet_cb, PA_PLAY);
    else if (p->dl == SL_POD_DL_NONE) acts_row_cb(LV_SYMBOL_DOWNLOAD, "Download", pod_sheet_cb, PA_GET);
    else acts_row_cb(LV_SYMBOL_STOP, "Stop downloading", pod_sheet_cb, PA_CANCEL);
    if (p->flags & SL_POD_HEARD) acts_row_cb(LV_SYMBOL_REFRESH, "Mark as not heard", pod_sheet_cb, PA_UNHEARD);
    else acts_row_cb(LV_SYMBOL_OK, "Mark as heard", pod_sheet_cb, PA_HEARD);
    acts_row_cb(LV_SYMBOL_LIST, "Select", pod_sheet_cb, PA_SELECT);
    if (p->dl == SL_POD_DL_DONE)
    {
        acts_row_cb(LV_SYMBOL_TRASH, "Delete download", pod_sheet_cb, PA_DELETE);
        fm_acts_red();
    }
    acts_show();
}
/* ---- picking several (the bar at the foot, Select all on it) */
static int pod_sel_total(void) { return pod_eps.n; }
static void pod_sel_repaint(void *view, bool marks) { (void)view; (void)marks; pod_rows_fill(); }
static void pod_sel_act(int which)
{
    int *idx = malloc(((size_t)pod_eps.n + 1) * sizeof *idx), n = 0;
    uint32_t *keys = malloc(((size_t)pod_eps.n + 1) * sizeof *keys);
    if (!idx || !keys) { free(idx); free(keys); return; }
    /* in the page's order, newest first */
    for (int i = 0; i < pod_eps.n; i++) if (sel_has(i)) { idx[n] = i; keys[n] = pod_eps.ep[i].key; n++; }
    char b[96];
    switch (which)
    {
        case 0: pod_play_eps(idx, n); break;
        case 1:
        {
            int k = sl_pod_download(pod_sh.id, keys, n);
            snprintf(b, sizeof b, sl_tr(k == 1 ? "%d episode to download" : "%d episodes to download"), k);
            toast(k ? b : sl_tr("Already on the card or on its way"));
            break;
        }
        case 2:
        {
            int on = 0;
            for (int k = 0; k < n; k++) if (pod_eps.ep[idx[k]].dl == SL_POD_DL_DONE) { if (pod_ep_playing(&pod_eps.ep[idx[k]])) audio_stop(); keys[on++] = keys[k]; }
            if (on && sl_pod_delete(pod_sh.id, keys, on))
            {
                snprintf(b, sizeof b, sl_tr(on == 1 ? "%d download deleted" : "%d downloads deleted"), on);
                toast(b);
            }
            else toast("None of these is on the card");
            break;
        }
        case 3: case 4:
            if (sl_pod_mark(pod_sh.id, keys, n, which == 3))
            {
                if (which == 3) for (int k = 0; k < n; k++) sl_pod_finished(pod_sh.id, keys[k]);
                snprintf(b, sizeof b, sl_tr(which == 3 ? (n == 1 ? "%d marked as heard" : "%d marked as heard")
                                                       : (n == 1 ? "%d marked as not heard" : "%d marked as not heard")), n);
                toast(b);
            }
            break;
    }
    free(idx);
    free(keys);
    sel_end();
    lv_async_call((lv_async_cb_t)pod_reload, NULL);
}
/* ---- this show: its speed, all heard, following no more */
static const int pod_speeds[] = { 75, 100, 125, 150, 175, 200, 250 };
static void pod_speed_apply(int pct);
static bool pod_now(void);
static uint32_t pod_np_show;
static void pod_speed_chip(int i)
{
    if (i < 0 || i >= (int)ARRAYLEN(pod_speeds)) return;
    if (!sl_pod_set_speed(pod_sh.id, pod_speeds[i])) { toast_warn("Could not save that"); return; }
    pod_sh.speed = (uint16_t)pod_speeds[i];
    if (pod_now() && pod_np_show == pod_sh.id) pod_speed_apply(pod_speeds[i]);   /* heard now */
}
static void pod_all_heard_cb(lv_event_t *e)
{
    (void)e;
    uint32_t *keys = malloc(((size_t)pod_eps.n + 1) * sizeof *keys);
    int n = 0;
    for (int i = 0; keys && i < pod_eps.n; i++) if (!(pod_eps.ep[i].flags & SL_POD_HEARD)) keys[n++] = pod_eps.ep[i].key;
    if (n && sl_pod_mark(pod_sh.id, keys, n, true)) toast("All marked as heard");
    else if (!n) toast("Every episode here has been heard");
    free(keys);
    pod_reload();
}
static void pod_refresh_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_need_wifi()) return;
    if (sl_pod_refresh(pod_sh.id)) toast("Looking for new episodes");
}
static void pod_play_latest_cb(lv_event_t *e)
{
    (void)e;
    /* the newest not heard on the card, or (all heard) the newest on the card */
    int pick = -1;
    for (int i = 0; i < pod_eps.n && pick < 0; i++)
        if (pod_eps.ep[i].dl == SL_POD_DL_DONE && !(pod_eps.ep[i].flags & SL_POD_HEARD)) pick = i;
    for (int i = 0; i < pod_eps.n && pick < 0; i++) if (pod_eps.ep[i].dl == SL_POD_DL_DONE) pick = i;
    if (pick >= 0) { pod_play_eps(&pick, 1); return; }
    if (pod_eps.n) pod_dl_one(0);         /* nothing on the card: the newest comes */
}
static void pod_unfollow_go(bool files)
{
    uint32_t id = pod_sh.id;
    char b[200];
    snprintf(b, sizeof b, "%s %s", sl_tr("No longer following"), pod_sh.title);
    if (!sl_pod_unfollow(id, files)) { toast_warn("Could not save that"); return; }
    toast(b);
    pod_sh.id = 0;
    /* back to the Podcasts page, made again without it */
    pod_refill(pod_page_body, pod_page_fill);
    if (pod_page_obj) page_bring_back(pod_page_obj);
    else page_pop();
}
static void pod_unfollow_keep_cb(lv_event_t *e) { (void)e; pod_unfollow_go(false); }
static void pod_unfollow_all_cb(lv_event_t *e) { (void)e; pod_unfollow_go(true); }
static void pod_unfollow_back_cb(lv_event_t *e) { (void)e; page_pop(); }
static void pod_unfollow_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("Unfollow", &p);
    char what[320];
    snprintf(what, sizeof what, sl_tr("%s goes from your podcasts. Its downloads can stay on the card, in the Podcasts folder, or go with it."), pod_sh.title);
    lv_obj_t *g = set_group(body, pod_sh.title);
    set_text(g, what, C_TEXT);
    g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action_sub(g, LV_SYMBOL_LEFT, 0x66738a, "Keep following", "Nothing changes", pod_unfollow_back_cb, NULL);
    set_action_sub(g, LV_SYMBOL_CLOSE, 0x2d7ff0, "Unfollow", "Keep its downloads on the card", pod_unfollow_keep_cb, NULL);
    lv_obj_t *r = set_action_sub(g, LV_SYMBOL_TRASH, 0xe5484d, "Unfollow and delete", "Its downloads go too", pod_unfollow_all_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(r, 0), lv_color_hex(SET_RED), 0);
    page_push(p);
}
static void pod_show_deleted(lv_event_t *e)
{
    (void)e;
    pod_show_obj = pod_show_body = NULL;
    pod_rows = NULL;
    /* its episodes go with it (a show of 300 holds about 60 KB) */
    sl_pod_eps_free(&pod_eps);
    free(pod_vis);
    pod_vis = NULL;
    pod_nvis = 0;
}
static void pod_show_fill(lv_obj_t *body);
/* the show as a hero, as an album is: its picture large on a wash of its
 * colour, its name, who makes it and how many, the way to play, then
 * the episodes newest first */
static void pod_show_page(uint32_t id)
{
    if (!sl_pod_show(id, &pod_sh)) return;
    sl_pod_eps_free(&pod_eps);
    if (!sl_pod_eps_load(id, &pod_eps)) memset(&pod_eps, 0, sizeof pod_eps);
    pod_start = 0;
    pod_filter = 0;
    pod_vis_make();
    lv_obj_t *body = page_new("", NULL);
    lv_obj_t *p = lv_obj_get_parent(body);
    pod_show_obj = p;
    pod_show_body = body;
    lv_obj_add_event_cb(p, pod_show_deleted, LV_EVENT_DELETE, NULL);
    if (!sl_zune())
    {
        lv_color_t tint = lv_color_hex(pod_tint(&pod_sh));
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
    pod_show_fill(body);
    page_push(p);
}
static void pod_show_fill(lv_obj_t *body)
{
    pod_rows = NULL;
    lv_obj_t *head = box(body);
    lv_obj_set_size(head, W, LV_SIZE_CONTENT);
    lv_obj_set_style_translate_x(head, -SL_GUTTER, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(head, 6, 0);
    lv_obj_set_style_pad_bottom(head, 18, 0);
    lv_obj_set_style_pad_hor(head, 28, 0);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_t *c = pod_art(head, &pod_sh, SL_POD_ART_BIG);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    sl_elevate(c, sl_zune() ? SL_E0 : SL_E3, 0);
    lv_obj_t *t = text(head, pod_sh.title, sl_zune() ? sl_zune_font(pod_sh.title, &ss_regular_34) : &ss_bold_28, 0xffffff);
    lv_obj_set_width(t, W - 56);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    note_lines(t, pod_sh.title, W - 56, 2);
    sl_label_deep(t);
    if (pod_sh.author[0])
    {
        lv_obj_t *ar = text(head, pod_sh.author, &ss_semibold_18, C_SUB);
        lv_obj_set_width(ar, W - 56);
        lv_obj_set_style_text_align(ar, LV_TEXT_ALIGN_CENTER, 0);
        label_lines(ar, 1);
    }
    char b[160], c1[48];
    b[0] = '\0';
    sl_count(c1, sizeof c1, pod_sh.n_eps, "%s episode", "%s episodes");
    pod_join(b, sizeof b, c1);
    if (pod_sh.n_unheard) { sl_count(c1, sizeof c1, pod_sh.n_unheard, "%s not heard", "%s not heard"); pod_join(b, sizeof b, c1); }
    if (pod_sh.n_dl) { sl_count(c1, sizeof c1, pod_sh.n_dl, "%s on the card", "%s on the card"); pod_join(b, sizeof b, c1); }
    lv_obj_t *m = text(head, b, &ss_regular_16, C_SUB);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    /* play the newest not heard (or bring it), and look for new ones */
    lv_obj_t *pl = box(body);
    lv_obj_set_size(pl, lv_pct(100), 72);
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pl, 12, 0);
    lv_obj_set_style_pad_hor(pl, 6, 0);
    lv_obj_set_style_pad_top(pl, 4, 0);
    bool any = false;
    for (int i = 0; i < pod_eps.n && !any; i++) any = pod_eps.ep[i].dl == SL_POD_DL_DONE;
    lv_obj_t *go = mn_pill(pl, any ? LV_SYMBOL_PLAY "  Play" : LV_SYMBOL_DOWNLOAD "  Get the latest", 10, SL_PILL_H, true, pod_play_latest_cb, NULL);
    lv_obj_set_flex_grow(go, 1);
    lv_obj_t *rf = mn_pill(pl, LV_SYMBOL_REFRESH "  Refresh", 10, SL_PILL_H, false, pod_refresh_cb, NULL);
    lv_obj_set_flex_grow(rf, 1);
    lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_x(rf, lv_pct(50), 0);
    /* the episodes: the count, and which ones */
    static const char *const shown[] = { "All episodes", "Not heard", "On the card" };
    sl_count(b, sizeof b, pod_nvis, "%s EPISODE", "%s EPISODES");
    count_row(body, sl_case(b), shown[pod_filter], pod_filter_cb);
    pod_rows = box(body);
    lv_obj_set_size(pod_rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pod_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pod_rows, SL_SP_XS, 0);
    pod_rows_fill();
    /* this show: its speed, all heard, unfollow */
    lv_obj_t *g = set_group(body, "This show");
    opt_label(g, "Playback speed");
    static const char *const sp_names[] = { "0.75x", "1x", "1.25x", "1.5x", "1.75x", "2x", "2.5x" };
    int sel = 1, now = pod_sh.speed ? pod_sh.speed : 100;
    for (int i = 0; i < (int)ARRAYLEN(pod_speeds); i++) if (pod_speeds[i] == now) sel = i;
    opt_chips(g, sp_names, ARRAYLEN(pod_speeds), sel, pod_speed_chip);
    lv_obj_t *r = set_action(g, LV_SYMBOL_OK, 0x3aa856, "Mark all as heard", NULL, pod_all_heard_cb, NULL);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, 2), LV_OBJ_FLAG_HIDDEN);
    opt_note(body, "The speed is this show's own. Music keeps its own speed.");
    g = set_group(body, NULL);
    set_danger(g, LV_SYMBOL_CLOSE, "Unfollow", pod_unfollow_cb, NULL);
}

/* ---- finding a show: the directory (iTunes Search: no account, no key) */
static struct sl_pod_found *pod_found;
static int pod_nfound;
static void pod_found_page(void);
static lv_obj_t *pod_find_note;
static void pod_find_go_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_find_ta) return;
    strlcpy(pod_find_q, sl_ta_text(pod_find_ta), sizeof pod_find_q);
    pl_utf8_trim(pod_find_q);
    if (!pod_find_q[0]) return;
    if (!pod_need_wifi()) return;
    pod_found_from = pod_status_now().found_seq;
    if (!sl_pod_search(pod_find_q)) { toast_warn("Could not search now"); return; }
    pod_want_found = true;
    /* the note under the field says so, where the eyes are */
    if (pod_find_note) lv_label_set_text(pod_find_note, sl_tr("Searching Apple's podcast directory\xe2\x80\xa6"));
}
static void pod_find_deleted(lv_event_t *e) { (void)e; pod_find_ta = pod_find_note = NULL; pod_want_found = false; }
static void pod_find_page(void)
{
    lv_obj_t *p, *body = page_new("Find a podcast", &p);
    lv_obj_add_event_cb(p, pod_find_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    pod_find_ta = set_field(set_group(body, "Name or topic"));
    lv_textarea_set_max_length(pod_find_ta, 80);
    sl_ta_hint(pod_find_ta, sl_tr("History, comedy, a show's name"));
    lv_obj_set_style_text_color(pod_find_ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    if (set_rtl()) lv_obj_set_style_text_align(pod_find_ta, LV_TEXT_ALIGN_RIGHT, 0);
    opt_note(body, "Searches Apple's podcast directory over Wi-Fi. No account needed.");
    pod_find_note = lv_obj_get_child(body, -1);
    wf_kb_build(p, pod_find_ta, KB_SEARCH, pod_find_go_cb, "Search");
    page_push(p);
}
static void pod_preview_page(int k);
static void pod_found_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= pod_nfound) return;
    uint32_t id = sl_pod_feed_id(pod_found[k].feed);
    if (sl_pod_show(id, NULL)) { pod_show_page(id); return; }   /* followed already: its page */
    if (!pod_need_wifi()) return;
    pod_prev_from = pod_status_now().preview_seq;
    if (!sl_pod_preview(pod_found[k].feed)) { toast_warn("Could not read the feed now"); return; }
    pod_want_prev = true;
    pod_preview_page(k);
}
static void pod_found_deleted(lv_event_t *e) { (void)e; pod_found_obj = NULL; free(pod_found); pod_found = NULL; pod_nfound = 0; }
static void pod_found_page(void)
{
    free(pod_found);
    pod_found = calloc(SL_POD_MAX_FOUND, sizeof *pod_found);
    pod_nfound = pod_found ? sl_pod_found_take(pod_found, SL_POD_MAX_FOUND) : 0;
    lv_obj_t *p, *body = page_new(pod_find_q, &p);
    pod_found_obj = p;
    lv_obj_add_event_cb(p, pod_found_deleted, LV_EVENT_DELETE, NULL);
    if (!pod_nfound)
    {
        sl_empty(body, IC_PODCAST, 96, pod_st.found_n < 0 ? "The directory didn't answer." : "Nothing by that name.",
                 pod_st.found_n < 0 ? "Check Wi-Fi and try again." : "Try fewer words, or the show's exact name.", 56);
        page_push(p);
        return;
    }
    char b[48];
    sl_count(b, sizeof b, pod_nfound, "%s SHOW", "%s SHOWS");
    lv_obj_t *g = set_group(body, b);
    for (int k = 0; k < pod_nfound; k++)
    {
        const struct sl_pod_found *f = &pod_found[k];
        char line[200], c[48];
        line[0] = '\0';
        pod_join(line, sizeof line, f->author);
        pod_join(line, sizeof line, f->genre);
        if (f->count > 0) { sl_count(c, sizeof c, f->count, "%s episode", "%s episodes"); pod_join(line, sizeof line, c); }
        /* followed already: a tick, and its page; else a plus, and a look first */
        bool have = sl_pod_following(f->feed);
        set_action_sub(g, have ? LV_SYMBOL_OK : LV_SYMBOL_PLUS, have ? 0x3aa856 : POD_TINT, f->title, line, pod_found_cb, (void *)(intptr_t)k);
    }
    opt_note(body, "Tap a show to see its episodes before following it.");
    page_push(p);
}

/* ---- a show before following it: its episodes, and Follow */
static int pod_prev_k = -1;
static struct sl_pod_show pod_prev_sh;
static lv_obj_t *pod_prev_body;
static void pod_follow_found_cb(lv_event_t *e)
{
    (void)e;
    int k = pod_prev_k;
    if (k < 0 || k >= pod_nfound) return;
    if (!pod_need_wifi()) return;
    pod_done_from = pod_status_now().done_seq;
    if (!sl_pod_follow(pod_found[k].feed, pod_found[k].title, pod_found[k].author, pod_found[k].art))
    { toast_warn("Could not follow this show"); return; }
    pod_want_follow = true;               /* "Now following" comes when it is saved */
}
static void pod_prev_deleted(lv_event_t *e)
{
    (void)e;
    pod_prev_obj = NULL;
    pod_prev_body = NULL;
    pod_want_prev = false;
    if (!pod_want_follow) sl_pod_preview_forget();   /* a follow under way still reads it */
}
/* the episodes read, under the show's words (filled when they come) */
static void pod_prev_fill(void)
{
    if (!pod_prev_body) return;
    struct sl_pod_eps e;
    lv_obj_t *wait = lv_obj_get_child(pod_prev_body, -1);
    if (wait && lv_obj_get_user_data(wait) == (void *)1) lv_obj_delete(wait);
    if (pod_st.preview_ok <= 0 || !sl_pod_preview_take(&pod_prev_sh, &e))
    {
        sl_empty(pod_prev_body, IC_PODCAST, 72, pod_st.preview_ok < 0 ? "The feed didn't answer." : "This feed has no episodes.",
                 pod_st.preview_ok < 0 ? "Check Wi-Fi and try again." : "Its address may have moved. Try again later.", 24);
        return;
    }
    char b[48];
    sl_count(b, sizeof b, e.n, "%s EPISODE", "%s EPISODES");
    lv_obj_t *g = set_group(pod_prev_body, b);
    for (int i = 0; i < e.n && i < 12; i++)
    {
        char line[96], d[48];
        line[0] = '\0';
        pod_date(d, sizeof d, e.ep[i].date);
        pod_join(line, sizeof line, d);
        pod_dur(d, sizeof d, e.ep[i].dur);
        pod_join(line, sizeof line, d);
        set_info(g, NULL, 0, e.ep[i].title, line, true);
    }
    if (e.n > 12) opt_note(pod_prev_body, "Follow the show to see every episode and download them.");
    sl_pod_eps_free(&e);
}
static void pod_preview_page(int k)
{
    pod_prev_k = k;
    const struct sl_pod_found *f = &pod_found[k];
    lv_obj_t *p, *body = page_new("", &p);
    pod_prev_obj = p;
    pod_prev_body = body;
    lv_obj_add_event_cb(p, pod_prev_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *head = box(body);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_set_style_pad_bottom(head, 12, 0);
    struct sl_pod_show s;
    memset(&s, 0, sizeof s);
    s.id = sl_pod_feed_id(f->feed);
    lv_obj_t *c = pod_art(head, &s, 120);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    lv_obj_t *t = text(head, f->title, &ss_bold_28, 0xffffff);
    lv_obj_set_width(t, W - 56);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    note_lines(t, f->title, W - 56, 2);
    char line[160];
    line[0] = '\0';
    pod_join(line, sizeof line, f->author);
    pod_join(line, sizeof line, f->genre);
    lv_obj_t *a = text(head, line, &ss_semibold_18, C_SUB);
    lv_obj_set_width(a, W - 56);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(a, 1);
    lv_obj_t *pl = box(body);
    lv_obj_set_size(pl, lv_pct(100), 72);
    lv_obj_set_style_pad_hor(pl, 6, 0);
    lv_obj_set_style_pad_top(pl, 4, 0);
    lv_obj_t *go = mn_pill(pl, LV_SYMBOL_PLUS "  Follow", W - 2 * SL_GUTTER - 12, SL_PILL_H, true, pod_follow_found_cb, NULL);
    lv_obj_center(go);
    /* waiting for the feed */
    lv_obj_t *w = text(body, "Reading the feed", &ss_regular_18, C_SUB);
    lv_obj_set_width(w, lv_pct(100));
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(w, SL_INSET, 0);
    lv_obj_set_user_data(w, (void *)1);
    page_push(p);
}

/* ---- by a feed's address */
static void pod_url_go_cb(lv_event_t *e)
{
    (void)e;
    if (!pod_url_ta) return;
    char url[400];
    strlcpy(url, sl_ta_text(pod_url_ta), sizeof url);
    /* typed without its scheme: https is meant */
    if (url[0] && strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))
    {
        char t[400];
        snprintf(t, sizeof t, "https://%s", url);
        strlcpy(url, t, sizeof url);
    }
    if (!strchr(url + 8, '.')) { toast("That doesn't look like a web address"); return; }
    if (sl_pod_following(url)) { toast("You follow this show already"); return; }
    if (!pod_need_wifi()) return;
    pod_done_from = pod_status_now().done_seq;
    if (!sl_pod_follow(url, NULL, NULL, NULL)) { toast_warn("Could not follow this show"); return; }
    pod_want_follow = true;
    toast("Reading the feed");
}
static void pod_url_deleted(lv_event_t *e) { (void)e; pod_url_ta = NULL; }
static void pod_url_page(void)
{
    lv_obj_t *p, *body = page_new("Add by address", &p);
    lv_obj_add_event_cb(p, pod_url_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    pod_url_ta = set_field(set_group(body, "Feed address"));
    lv_textarea_set_max_length(pod_url_ta, 380);
    sl_ta_hint(pod_url_ta, "feeds.example.com/show.xml");
    lv_obj_set_style_text_color(pod_url_ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_base_dir(pod_url_ta, LV_BASE_DIR_LTR, 0);   /* an address reads left to right in any language */
    opt_note(body, "The show's RSS feed, as its website or another podcast app gives it.");
    wf_kb_build(p, pod_url_ta, KB_TEXT, pod_url_go_cb, "Follow");
    page_push(p);
}

/* ---- OPML: other apps' lists in, ours out */
static char (*pod_opml)[96];
static int pod_nopml;
static void pod_opml_go_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= pod_nopml) return;
    if (!pod_need_wifi()) return;
    pod_done_from = pod_status_now().done_seq;
    if (!sl_pod_opml_import(pod_opml[k])) { toast_warn("Could not read that file"); return; }
    pod_want_follow = true;
    toast("Following the shows in the list");
}
static void pod_opml_deleted(lv_event_t *e) { (void)e; free(pod_opml); pod_opml = NULL; pod_nopml = 0; }
static void pod_opml_page(void)
{
    lv_obj_t *p, *body = page_new("Import or export", &p);
    lv_obj_add_event_cb(p, pod_opml_deleted, LV_EVENT_DELETE, NULL);
    free(pod_opml);
    pod_opml = calloc(24, sizeof *pod_opml);
    pod_nopml = pod_opml ? sl_pod_opml_list(pod_opml, 24) : 0;
    lv_obj_t *g = set_group(body, "Import");
    if (!pod_nopml) set_text(g, sl_tr("No OPML file on the card. Export your list from your old podcast app, then put the file at the top of the card or in the Podcasts folder."), C_SUB);
    for (int k = 0; k < pod_nopml; k++)
    {
        const char *slash = strrchr(pod_opml[k], '/');
        set_action_sub(g, LV_SYMBOL_FILE, 0x3aa856, slash ? slash + 1 : pod_opml[k], pod_opml[k], pod_opml_go_cb, (void *)(intptr_t)k);
    }
    opt_note(body, "Each show in the file is followed once; ones you follow already are left as they are.");
    g = set_group(body, "Export");
    lv_obj_t *r = set_action_sub(g, LV_SYMBOL_UPLOAD, 0x2d7ff0, "Save your list", "Podcasts.opml, in the Podcasts folder", pod_export_cb, NULL);
    if (!sl_zune()) lv_obj_add_flag(lv_obj_get_child(r, 3), LV_OBJ_FLAG_HIDDEN);   /* does, does not open */
    page_push(p);
}

/* ---- downloads: what is coming, and everything on the card */
struct pod_dl_ref { uint32_t show, key; uint32_t date; char title[160], show_title[96], path[400]; };
static struct pod_dl_ref *pod_dls;
static int pod_ndls;
static lv_obj_t *pod_dl_now_bar, *pod_dl_now_line;
static void pod_dl_del_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= pod_ndls) return;
    struct mp3entry *id3 = audio_current_track();
    if (id3 && !strcmp(id3->path, pod_dls[k].path)) audio_stop();
    if (sl_pod_delete(pod_dls[k].show, &pod_dls[k].key, 1)) toast("Download deleted");
}
/* a tap plays it, from where it was left */
static void pod_dl_play_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= pod_ndls) return;
    struct sl_pod_progress p;
    memset(&p, 0, sizeof p);
    p.show = pod_dls[k].show;
    p.key = pod_dls[k].key;
    pod_play_progress(&p);
}
static void pod_dl_cancel_cb(lv_event_t *e)
{
    uint32_t key = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    sl_pod_cancel(pod_st.dl_show, key);
}
static lv_obj_t *pod_dl_body;
static void pod_dl_deleted(lv_event_t *e) { (void)e; pod_dl_obj = pod_dl_body = NULL; pod_dl_now_bar = pod_dl_now_line = NULL; free(pod_dls); pod_dls = NULL; pod_ndls = 0; }
static void pod_dl_now_text(char *b, size_t n)
{
    b[0] = '\0';
    char x[48];
    if (pod_st.dl_total > 0) { char g[24], t[24]; pod_size(g, sizeof g, pod_st.dl_got); pod_size(t, sizeof t, pod_st.dl_total); snprintf(x, sizeof x, "%s / %s", g, t); }
    else pod_size(x, sizeof x, pod_st.dl_got);
    pod_join(b, n, x);
    if (pod_st.queued) { sl_count(x, sizeof x, pod_st.queued, "%s more waiting", "%s more waiting"); pod_join(b, n, x); }
}
static int pod_dl_cmp(const void *a, const void *b)
{
    const struct pod_dl_ref *x = a, *y = b;
    return x->date < y->date ? 1 : x->date > y->date ? -1 : 0;
}
static void pod_downloads_fill(lv_obj_t *body)
{
    pod_dl_now_bar = pod_dl_now_line = NULL;
    /* coming down now */
    if (pod_st.dl_show)
    {
        struct sl_pod_show s;
        struct sl_pod_eps e;
        char title[160] = "";
        if (sl_pod_show(pod_st.dl_show, &s) && sl_pod_eps_load(pod_st.dl_show, &e))
        {
            for (int i = 0; i < e.n; i++) if (e.ep[i].key == pod_st.dl_key) snprintf(title, sizeof title, "%s", e.ep[i].title);
            sl_pod_eps_free(&e);
        }
        lv_obj_t *g = set_group(body, "Downloading");
        char b[160];
        pod_dl_now_text(b, sizeof b);
        lv_obj_t *r = set_action_sub(g, NULL, 0, title, b, NULL, NULL);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        pod_dl_now_line = lv_obj_get_child(r, 1);
        set_row_button(r, 2, LV_SYMBOL_CLOSE, pod_dl_cancel_cb, (void *)(uintptr_t)pod_st.dl_key);
        lv_obj_t *bar = lv_bar_create(r);
        lv_obj_set_size(bar, SET_INNER - 2 * SL_INSET, 4);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, pod_st.dl_total > 0 ? (int32_t)(pod_st.dl_got * 1000 / pod_st.dl_total) : 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, SL_R_FULL, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, sl_theme() == 1 ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_base_dir(bar, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -SL_SP_S);
        pod_dl_now_bar = bar;
        if (!pod_st.online) opt_note(body, "Waiting for Wi-Fi. Downloads carry on where they stopped.");
    }
    else if (pod_st.queued)
    {
        lv_obj_t *g = set_group(body, "Downloading");
        char b[96];
        sl_count(b, sizeof b, pod_st.queued, "%s waiting", "%s waiting");
        set_info(g, LV_SYMBOL_DOWNLOAD, 0x0aa9c7, b, pod_st.online ? sl_tr("Starting") : sl_tr("Waiting for Wi-Fi"), true);
    }
    /* everything on the card, newest first: a show's file each */
    free(pod_dls);
    pod_dls = NULL;
    pod_ndls = 0;
    int ns = 0, cap = 0;
    struct sl_pod_show *shows = sl_pod_shows_copy(&ns);
    long long total = 0;
    for (int i = 0; i < ns; i++) cap += shows[i].n_dl;
    if (cap) pod_dls = calloc((size_t)cap + 8, sizeof *pod_dls);
    for (int i = 0; pod_dls && i < ns; i++)
    {
        if (!shows[i].n_dl) continue;
        struct sl_pod_eps e;
        if (!sl_pod_eps_load(shows[i].id, &e)) continue;
        for (int k = 0; k < e.n && pod_ndls < cap + 8; k++)
        {
            if (e.ep[k].dl != SL_POD_DL_DONE) continue;
            struct pod_dl_ref *d = &pod_dls[pod_ndls++];
            d->show = shows[i].id; d->key = e.ep[k].key; d->date = e.ep[k].date;
            snprintf(d->title, sizeof d->title, "%s", e.ep[k].title);
            snprintf(d->show_title, sizeof d->show_title, "%s", shows[i].title);
            sl_pod_ep_path(shows[i].id, &e.ep[k], d->path, sizeof d->path);
            total += e.ep[k].bytes;
        }
        sl_pod_eps_free(&e);
    }
    free(shows);
    if (pod_ndls > 1) qsort(pod_dls, (size_t)pod_ndls, sizeof *pod_dls, pod_dl_cmp);
    if (!pod_ndls && !pod_st.dl_show && !pod_st.queued)
        sl_empty(body, IC_PODCAST, 96, "Nothing downloaded yet.", "Episodes you download show here, the newest first, to play without Wi-Fi.", 56);
    else if (pod_ndls)
    {
        char b[64];
        sl_count(b, sizeof b, pod_ndls, "%s ON THE CARD", "%s ON THE CARD");
        lv_obj_t *g = set_group(body, b);
        for (int k = 0; k < pod_ndls && k < 200; k++)
        {
            char line[200], d[48];
            snprintf(line, sizeof line, "%s", pod_dls[k].show_title);
            pod_date(d, sizeof d, pod_dls[k].date);
            pod_join(line, sizeof line, d);
            lv_obj_t *r = set_action_sub(g, NULL, 0, pod_dls[k].title, line, pod_dl_play_cb, (void *)(intptr_t)k);
            set_row_button(r, 2, LV_SYMBOL_TRASH, pod_dl_del_cb, (void *)(intptr_t)k);
        }
        if (total > 0)
        {
            char sz[32], note[96];
            pod_size(sz, sizeof sz, total);
            snprintf(note, sizeof note, sl_tr("%s on the card, in the Podcasts folder."), sz);
            opt_note(body, note);
        }
    }
}
static void pod_downloads_page(void)
{
    lv_obj_t *p, *body = page_new("Downloads", &p);
    pod_dl_obj = p;
    pod_dl_body = body;
    lv_obj_add_event_cb(p, pod_dl_deleted, LV_EVENT_DELETE, NULL);
    pod_downloads_fill(body);
    page_push(p);
}

/* ---- listening: the episode playing keeps its place, its show's speed */
static char pod_np_path[400], pod_np_title[160];
static uint32_t pod_np_key, pod_np_len, pod_np_at;
static uint32_t pod_save_ms, pod_flush_ms;
static bool pod_speed_on, pod_was_paused;
static int32_t pod_music_speed;
static bool pod_now(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    return id3 && sl_pod_is_episode(id3->path);
}
static void pod_speed_apply(int pct)
{
    if (!pod_speed_on) { pod_music_speed = dsp_get_timestretch(); pod_speed_on = true; }
    if (!global_settings.timestretch_enabled)
    {
        global_settings.timestretch_enabled = true;
        dsp_timestretch_enable(true);
    }
    dsp_set_timestretch((int32_t)(pct ? pct : 100) * PITCH_SPEED_PRECISION);
}
static void pod_speed_restore(void)
{
    if (!pod_speed_on) return;
    pod_speed_on = false;
    dsp_set_timestretch(pod_music_speed ? pod_music_speed : PITCH_SPEED_100);
}
static void pod_left_episode(void)
{
    if (!pod_np_show) return;
    /* heard to its last minute: heard, its place forgotten */
    if (pod_np_len && pod_np_at + 60000 >= pod_np_len) sl_pod_finished(pod_np_show, pod_np_key);
    else
    {
        /* where it was left, at once: the next tick may never come (a stop, a song) */
        if (pod_np_at > 0) sl_pod_progress_set(pod_np_show, pod_np_key, pod_np_at, pod_np_len, pod_np_path, pod_np_title);
        sl_pod_progress_flush();
    }
    pod_np_show = pod_np_key = 0;
    pod_np_path[0] = '\0';
    if (pod_show_obj) lv_async_call((lv_async_cb_t)pod_reload, NULL);
}
static void pod_tick(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    bool ep = id3 && sl_pod_is_episode(id3->path);
    if (!ep || strcmp(id3->path, pod_np_path))
    {
        pod_left_episode();
        if (ep)
        {
            strlcpy(pod_np_path, id3->path, sizeof pod_np_path);
            uint32_t s = 0, k = 0;
            if (sl_pod_which(id3->path, &s, &k, pod_np_title, sizeof pod_np_title))
            {
                struct sl_pod_show sh;
                pod_np_show = s;
                pod_np_key = k;
                pod_save_ms = pod_flush_ms = sl_ms();
                if (sl_pod_show(s, &sh)) pod_speed_apply(sh.speed);
            }
        }
        else pod_speed_restore();
    }
    if (!pod_np_show || !id3) return;
    pod_np_at = (uint32_t)id3->elapsed;
    if (id3->length) pod_np_len = (uint32_t)id3->length;
    bool paused = (audio_status() & AUDIO_STATUS_PAUSE) != 0;
    uint32_t now = sl_ms();
    if (now - pod_save_ms >= 5000 || paused != pod_was_paused)
    {
        pod_save_ms = now;
        char title[160];
        sl_pod_progress_set(pod_np_show, pod_np_key, pod_np_at, pod_np_len, id3->path,
                            pod_np_title[0] ? pod_np_title : track_title(id3, title, sizeof title));
    }
    /* on the card every half a minute, and at once when paused */
    if (now - pod_flush_ms >= 30000 || (paused && !pod_was_paused)) { pod_flush_ms = now; sl_pod_progress_flush(); }
    pod_was_paused = paused;
}

/* ---- the worker's news */
static void pod_poll(void)
{
    sl_pod_poll();
    pod_tick();
    struct sl_pod_status s;
    sl_pod_status(&s);
    if (s.seq == pod_st.seq) return;
    struct sl_pod_status was = pod_st;
    pod_st = s;
    if (pod_want_found && s.found_seq != pod_found_from)
    {
        pod_want_found = false;
        if (pod_find_note) lv_label_set_text(pod_find_note, sl_tr("Searches Apple's podcast directory over Wi-Fi. No account needed."));
        if (pod_find_ta || (npages && pod_find_q[0])) pod_found_page();
    }
    if (pod_want_prev && s.preview_seq != pod_prev_from) { pod_want_prev = false; pod_prev_fill(); }
    if (s.done_seq != was.done_seq)
    {
        char b[240];
        if (s.done_job == SL_POD_JOB_FOLLOW && pod_want_follow && s.done_seq != pod_done_from)
        {
            pod_want_follow = false;
            if (s.done_ok)
            {
                /* the first show: its folder stays out of the music library (Library > Folders to scan lets it in) */
                if (sl_pod_show_count() == 1 && !tagcache_folder_listed(sl_pod_folder))
                {
                    tagcache_set_folder_excluded(sl_pod_folder, true);
                    settings_save();
                }
                snprintf(b, sizeof b, "%s %s", sl_tr("Now following"), s.done_title);
                notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "podcast", b);
                uint32_t id = 0;
                int n = 0;
                struct sl_pod_show *all = sl_pod_shows_copy(&n);
                for (int i = 0; i < n && !id; i++) if (!strcmp(all[i].title, s.done_title)) id = all[i].id;
                free(all);
                /* the show's own page, over the search that found it */
                if (id) pod_show_page(id);
            }
            else notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "podcast", sl_tr(s.why));
        }
        else if (s.done_job == SL_POD_JOB_OPML && pod_want_follow && s.done_seq != pod_done_from)
        {
            pod_want_follow = false;
            if (s.done_ok) snprintf(b, sizeof b, sl_tr(s.done_count == 1 ? "%d show added" : "%d shows added"), s.done_count);
            else snprintf(b, sizeof b, "%s", sl_tr(s.why));
            notify_ex(NS_MUSIC, s.done_ok ? NOTE_INFO : NOTE_WARN, 0, NA_NONE, "podcast", b);
        }
        else if (s.done_job == SL_POD_JOB_REFRESH && (pod_page_obj || pod_show_obj))
        {
            if (!s.done_ok) toast_warn(s.why);
            else if (s.done_count > 0)
            {
                snprintf(b, sizeof b, sl_tr(s.done_count == 1 ? "%d new episode" : "%d new episodes"), s.done_count);
                toast(b);
            }
            else toast("No new episodes");
        }
        else if (s.done_job == SL_POD_JOB_DELETE && pod_dl_body) pod_refill(pod_dl_body, pod_downloads_fill);
    }
    /* the pages showing what moved */
    if (s.eps_seq != was.eps_seq && pod_show_obj && (s.eps_show == pod_sh.id || !s.eps_show) && !sel_kind_on(SEL_EPISODES)) pod_reload();
    else if (pod_show_obj && s.dl_show == pod_sh.id && s.dl_got != was.dl_got && pod_rows && !sel_kind_on(SEL_EPISODES))
    {
        /* the row coming down says how far, in place */
        for (uint32_t i = 0; i < lv_obj_get_child_count(pod_rows); i++)
        {
            lv_obj_t *r = lv_obj_get_child(pod_rows, (int32_t)i);
            int k = (int)(intptr_t)lv_obj_get_user_data(r);
            if (k < 0 || k >= pod_eps.n || pod_eps.ep[k].key != s.dl_key) continue;
            char b[200];
            pod_ep_line(&pod_eps.ep[k], b, sizeof b);
            for (uint32_t j = 0; j < lv_obj_get_child_count(r); j++)
            {
                lv_obj_t *c = lv_obj_get_child(r, (int32_t)j);
                if (lv_obj_check_type(c, &lv_label_class) && lv_obj_get_style_text_font(c, 0) == &ss_regular_18) label_if_changed(c, b);
            }
        }
    }
    if (pod_dl_body)
    {
        if (s.dl_seq != was.dl_seq || (bool)s.dl_show != (bool)was.dl_show) pod_refill(pod_dl_body, pod_downloads_fill);
        else if (pod_dl_now_bar && s.dl_got != was.dl_got)
        {
            lv_bar_set_value(pod_dl_now_bar, s.dl_total > 0 ? (int32_t)(s.dl_got * 1000 / s.dl_total) : 0, LV_ANIM_ON);
            char b[160];
            pod_dl_now_text(b, sizeof b);
            if (pod_dl_now_line) label_if_changed(pod_dl_now_line, b);
        }
    }
    if (pod_dl_row && (s.dl_got != was.dl_got || s.queued != was.queued || s.dl_seq != was.dl_seq))
    {
        char dl[64];
        pod_dl_words(dl, sizeof dl);
        set_link_text(pod_dl_row, dl[0] ? dl : sl_tr("On the card, and coming"), dl[0] ? dl : NULL);
    }
    if (s.shows_seq != was.shows_seq && pod_page_body) pod_refill(pod_page_body, pod_page_fill);
    if (s.dl_seq != was.dl_seq && !s.dl_ok && s.why[0] && strcmp(s.why, was.why))
        notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "podcast", sl_tr(s.why));
}
