/* Music in over Wi-Fi, the interface's side: the transfer page with its QR
 * code and its progress, LocalSend's "Accept N songs from ...?" sheet, and
 * the library update when a transfer ends. sloppy_share.c serves both on
 * its own threads; this reads its state with the UI tick and never waits.
 * Included from sloppy.c after the Wi-Fi page. */
#include "sloppy_qr.h"
#include "sloppy_share.h"

#define RX_WEB_PORT 4399                  /* as phones and computers see it */
static struct sl_rx_status rx_st;
static bool rx_ls_up;                     /* LocalSend serving, as its last job said */
static bool rx_sheet;                     /* the ask is on screen */
static lv_obj_t *rx_card, *rx_title, *rx_bar, *rx_line;   /* the transfer page's progress */

/* ------------------------------------------------------------ a QR code */
/* drawn, not stored as pixels: a dark square for each run of dark modules */
static void qr_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    const struct sl_qr *q = lv_obj_get_user_data(o);
    if (!q) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    int32_t m = lv_area_get_width(&a) / q->size, x0 = a.x1 + (lv_area_get_width(&a) - m * q->size) / 2,
            y0 = a.y1 + (lv_area_get_height(&a) - m * q->size) / 2;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(0x0d1117);
    d.bg_opa = LV_OPA_COVER;
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; )
        {
            if (!sl_qr_dark(q, x, y)) { x++; continue; }
            int x1 = x;
            while (x < q->size && sl_qr_dark(q, x, y)) x++;
            lv_area_t r = { x0 + x1 * m, y0 + y * m, x0 + x * m - 1, y0 + (y + 1) * m - 1 };
            lv_draw_rect(layer, &d, &r);
        }
}
static void qr_free_cb(lv_event_t *e) { free(lv_obj_get_user_data(lv_event_get_current_target(e))); }
/* text as a QR code on a white tile about size px a side, its quiet
 * zone inside; NULL when the text is too long for one */
static lv_obj_t *qr_tile(lv_obj_t *parent, const char *txt, int size)
{
    struct sl_qr *q = malloc(sizeof *q);
    if (!q || !sl_qr_make(q, txt)) { free(q); return NULL; }
    int m = size / (q->size + 8);         /* four modules of white round it, as scanners want */
    if (m < 2) m = 2;
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, (q->size + 8) * m, (q->size + 8) * m);
    lv_obj_set_style_pad_all(o, 4 * m, 0);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, sl_zune() ? 0 : SL_R_MD, 0);   /* Zule is square */
    lv_obj_set_user_data(o, q);
    lv_obj_add_event_cb(o, qr_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, qr_free_cb, LV_EVENT_DELETE, NULL);
    return o;
}

/* ------------------------------------------------------------ the transfer page */
static void rx_fmt_mb(char *b, size_t n, uint64_t bytes)
{
    if (bytes < 10ull << 20) snprintf(b, n, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes & 0xfffff) * 10 >> 20));
    else snprintf(b, n, "%u MB", (unsigned)(bytes >> 20));
}
static void rx_page_update(const struct sl_rx_status *s)
{
    if (!rx_card) return;
    bool show = s->state == SL_RX_RECEIVING || ((s->state == SL_RX_DONE || s->state == SL_RX_ENDED) && (s->done || s->failed));
    hidden_if_changed(rx_card, !show);
    if (!show) return;
    char t[96], l[160], a[24], b[24];
    if (s->state == SL_RX_RECEIVING)
    {
        int n = s->done + s->failed + 1;
        if (s->files > 0) snprintf(t, sizeof t, sl_tr("Receiving %d of %d"), n > s->files ? s->files : n, s->files);
        else snprintf(t, sizeof t, "%s", sl_tr("Receiving"));
        rx_fmt_mb(a, sizeof a, s->bytes);
        rx_fmt_mb(b, sizeof b, s->total);
        if (s->total) snprintf(l, sizeof l, "%s \xc2\xb7 %s / %s", s->current, a, b);
        else snprintf(l, sizeof l, "%s \xc2\xb7 %s", s->current, a);
    }
    else
    {
        snprintf(t, sizeof t, sl_tr(s->done == 1 ? "%d file received" : "%d files received"), s->done);
        if (s->failed) snprintf(l, sizeof l, sl_tr("%d did not arrive"), s->failed);
        else snprintf(l, sizeof l, "%s", sl_tr("In Music/Received. The library is updating."));
    }
    label_if_changed(rx_title, t);
    label_if_changed(rx_line, l);
    int v = s->state != SL_RX_RECEIVING ? 1000 : s->total ? (int)(s->bytes * 1000 / s->total) : 0;
    if (lv_bar_get_value(rx_bar) != v) lv_bar_set_value(rx_bar, v > 1000 ? 1000 : v, LV_ANIM_OFF);
}
static void rx_page_deleted(lv_event_t *e)
{
    (void)e;                              /* the page's own (wf_share_deleted has let go of it first) */
    rx_card = rx_title = rx_bar = rx_line = NULL;
}
static void rx_ls_set(int on);
static void rx_any_set(int on) { global_settings.sloppy_receive_any = on; sl_share_allow_any(on); }
/* the page: the code to scan, the address under it, the progress, LocalSend */
static void rx_page_build(lv_obj_t *p, lv_obj_t *body)
{
    char url[48];
    snprintf(url, sizeof url, "http://%s:%d", wf_st.ip, RX_WEB_PORT);
    lv_obj_add_event_cb(p, rx_page_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, NULL);
    lv_obj_set_style_pad_all(g, SL_INSET, 0);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g, SL_SP_S, 0);
    qr_tile(g, url, 216);
    wf_share_label = text(g, url, &ss_bold_28, C_ACCENT_TXT);
    lv_obj_set_style_margin_top(wf_share_label, SL_SP_XS, 0);
    lv_obj_t *n = text(g, "Scan with a phone, or open this address in a browser on the same Wi-Fi. Drop songs or whole folders on the page.",
                       &ss_regular_16, C_SUB);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    sl_label_deep(n);
    /* what is coming in */
    rx_card = set_group(body, NULL);
    lv_obj_set_style_pad_all(rx_card, SL_INSET, 0);
    lv_obj_set_style_pad_row(rx_card, SL_SP_S, 0);
    rx_title = text(rx_card, "", &ss_semibold_21, C_TEXT);
    set_align(rx_title);
    lv_obj_set_width(rx_title, lv_pct(100));
    rx_bar = lv_bar_create(rx_card);
    lv_obj_set_size(rx_bar, lv_pct(100), 8);
    lv_bar_set_range(rx_bar, 0, 1000);
    lv_obj_set_style_radius(rx_bar, SL_R_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(rx_bar, SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(rx_bar, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rx_bar, sl_theme() == 1 ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rx_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    if (set_rtl()) lv_obj_set_style_base_dir(rx_bar, LV_BASE_DIR_RTL, 0);
    rx_line = text(rx_card, "", &ss_regular_16, C_SUB);
    lv_obj_set_width(rx_line, lv_pct(100));
    label_lines(rx_line, 1);
    set_align(rx_line);
    lv_obj_add_flag(rx_card, LV_OBJ_FLAG_HIDDEN);
    /* LocalSend, and what is taken */
    g = set_group(body, "LOCALSEND");
    char note[160];
    snprintf(note, sizeof note, sl_tr("Shows up in the LocalSend app as %s. You're asked before anything comes in."), sl_bt_shown_player_name());
    set_switch_ex(g, LV_SYMBOL_DOWNLOAD, 0x2d7ff0, "Receive with LocalSend", note, global_settings.sloppy_localsend, rx_ls_set);
    set_switch(g, "Other files too", "Not only songs and their covers, cue sheets and lyrics.", global_settings.sloppy_receive_any, rx_any_set);
    opt_note(body, "Everything lands in Music/Received. Nothing on the player is replaced, and new songs join the library when a transfer ends.");
    sl_share_status(&rx_st);
    if (rx_st.state == SL_RX_RECEIVING) rx_page_update(&rx_st);
}

/* ------------------------------------------------------------ LocalSend's ask */
static void rx_ask_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    rx_sheet = false;
    acts_close();
    sl_share_answer(k == 1);
}
static void rx_ask(void)
{
    char t[128], l[160], mb[24];
    int n = rx_st.files;
    if (rx_st.songs == n) snprintf(t, sizeof t, sl_tr(n == 1 ? "Accept %d song from %s?" : "Accept %d songs from %s?"), n, rx_st.from);
    else snprintf(t, sizeof t, sl_tr(n == 1 ? "Accept %d file from %s?" : "Accept %d files from %s?"), n, rx_st.from);
    rx_fmt_mb(mb, sizeof mb, rx_st.total);
    snprintf(l, sizeof l, "%s \xc2\xb7 %s", mb, sl_tr("Into Music/Received"));
    /* a dark screen lights for it, on the lock screen as for a charger */
    if (sl_power_running && sl_power.mode == SL_POWER_DARK) sl_power_cable_wake();
    acts_begin();
    lyr_sheet_head(t, l);
    acts_row_cb(LV_SYMBOL_DOWNLOAD, "Accept", rx_ask_cb, 1);
    acts_row_cb(LV_SYMBOL_CLOSE, "Decline", rx_ask_cb, 0);
    acts_show();
    rx_sheet = true;
}

/* ------------------------------------------------------------ with the UI tick */
static void rx_ls_set(int on)
{
    global_settings.sloppy_localsend = on;
    settings_save();
}
/* is LocalSend wanted now: the switch, and a network to be found on */
static bool rx_ls_wanted(void) { return global_settings.sloppy_localsend && wf_st.connected && !sl_net_wireless_unsupported(); }
static void rx_poll(void)
{
    if (!wf_share_on && !rx_ls_up && rx_st.state == SL_RX_IDLE) return;
    struct sl_rx_status s;
    sl_share_status(&s);
    bool moved = s.seq != rx_st.seq;
    rx_st = s;
    /* the ask, once; gone (scrim tapped, timed out) is a no */
    if (s.state == SL_RX_ASK && !rx_sheet && moved) rx_ask();
    else if (rx_sheet && s.state != SL_RX_ASK) { rx_sheet = false; acts_close(); }
    else if (rx_sheet && !acts_open_now) { rx_sheet = false; sl_share_answer(false); }
    if (s.state == SL_RX_DONE || s.state == SL_RX_ENDED)
    {
        char m[160];
        if (s.done)
        {
            snprintf(m, sizeof m, sl_tr(s.done == 1 ? "%d file received" : "%d files received"), s.done);
            if (s.kind == SL_RX_LOCALSEND && s.from[0]) { strlcat(m, " \xc2\xb7 ", sizeof m); strlcat(m, s.from, sizeof m); }
            notify_ex(NS_WIFI, NOTE_INFO, 0, NA_NONE, "receive", m);
            db_update_begin(false);           /* they join the library */
        }
        else if (s.why[0] && strcmp(s.why, "Declined")) notify_ex(NS_WIFI, NOTE_WARN, 0, NA_NONE, "receive", s.why);   /* a no said here needs no telling */
        sl_log("receive: %s session ended: %d landed, %d failed, %d refused, %u KiB%s%s",
               s.kind == SL_RX_LOCALSEND ? "LocalSend" : "transfer page", s.done, s.failed, s.refused,
               (unsigned)(s.bytes >> 10), s.why[0] ? ": " : "", s.why);
        rx_page_update(&s);                /* the page keeps the result showing */
        sl_share_seen();
        rx_st.state = SL_RX_IDLE;
        return;
    }
    if (s.state == SL_RX_RECEIVING) rx_page_update(&s);
}
