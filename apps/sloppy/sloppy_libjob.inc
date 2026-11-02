/* Lyrics and covers for the whole library, the interface's side: the page
 * in Settings > Library, and the feeding. sloppy_libjob.c fetches on its
 * own thread; this hands it songs (or albums) a few at a time with the UI
 * tick, so the library is never copied whole, and never waits on it.
 * Included from sloppy.c after Scrobbling. */
#include "sloppy_libjob.h"

static int lb_kind = -1;                 /* the job being fed; -1 none */
static int lb_next, lb_total;            /* the next index to hand over, of how many */
static int lb_art_album = -1;            /* covers: an album the cover thread is looking at first */
static uint32_t lb_art_since;
static unsigned lb_found_seq;
static struct sl_libjob_status lb_st;
static lv_obj_t *lb_page_obj, *lb_line[SL_LIBJOB_KINDS], *lb_bar[SL_LIBJOB_KINDS], *lb_btn[SL_LIBJOB_KINDS];

static int lb_pos_load(int kind) { return sl_libjob_saved(kind); }

/* what a card says: how far, what was found, or why it waits */
static void lb_refresh(void);
static void lb_start(int kind)
{
    if (lb_kind >= 0 || sl_libjob_busy()) { toast(lb_kind == kind ? "Already running" : "One at a time: the other job is running"); return; }
    if (sl_net_wireless_unsupported()) { toast_warn("Wi-Fi isn't supported on this R1 yet"); return; }
    if (lib_state != SL_LIB_READY) { toast("Your library is still loading"); return; }
    int total = kind == SL_LIBJOB_LYRICS ? sl_lib.n_tracks : sl_lib.n_albums;
    if (!total) { toast("No music in the library yet"); return; }
    int pos = lb_pos_load(kind);
    if (pos >= total) pos = 0;
    sl_libjob_online(wf_st.connected);
    if (!sl_libjob_start(kind, total, pos)) { toast_warn("Could not start"); return; }
    lb_kind = kind;
    lb_next = pos;
    lb_total = total;
    lb_art_album = -1;
    lb_found_seq = 0;
    sl_log("libjob: %s started at %d of %d", kind == SL_LIBJOB_LYRICS ? "lyrics" : "covers", pos, total);
    if (!wf_st.connected) toast("It starts when Wi-Fi connects");
    sl_libjob_status(&lb_st);
    lb_refresh();
}
static void lb_btn_cb(lv_event_t *e)
{
    int kind = (int)(intptr_t)lv_event_get_user_data(e);
    if (lb_kind == kind) { sl_libjob_stop(); toast("Stopping after this one"); lb_refresh(); return; }
    lb_start(kind);
}

/* the next few, with the UI tick */
static void lb_feed(void)
{
    int total = lb_kind == SL_LIBJOB_LYRICS ? sl_lib.n_tracks : sl_lib.n_albums;
    if (lib_state != SL_LIB_READY) return;   /* being read again: the arrays are not ours to walk */
    if (total != lb_total) { lb_total = total; if (lb_next > total) lb_next = total; }
    for (int k = 0; k < 8 && lb_next < lb_total && sl_libjob_want(); k++)
    {
        struct sl_libjob_item it;
        memset(&it, 0, sizeof it);
        it.index = lb_next;
        if (lb_kind == SL_LIBJOB_LYRICS)
        {
            int t = sl_lib.songs_by_title[lb_next];
            const struct sl_track *tr = &sl_lib.tracks[t];
            sl_track_path(t, it.path, sizeof it.path);
            if (!tr->cue)                    /* a cue sheet's track shares its file: no .lrc of its own */
            {
                strlcpy(it.artist, sl_track_artist(t), sizeof it.artist);
                strlcpy(it.title, tr->title ? tr->title : "", sizeof it.title);
                strlcpy(it.album, sl_lib.albums[tr->album].title ? sl_lib.albums[tr->album].title : "", sizeof it.album);
            }
            it.secs = (unsigned)(tr->len / 1000);
        }
        else
        {
            int a = sl_lib.albums_by_title[lb_next];
            const struct sl_album *al = &sl_lib.albums[a];
            int state = sl_art_state(a);
            /* not looked at yet: the cover thread looks first (embedded art counts), a while at most */
            if (state < 0 && (lb_art_album != a || sl_ms() - lb_art_since < 20000))
            {
                if (lb_art_album != a) { lb_art_album = a; lb_art_since = sl_ms(); sl_art_prefer(a); }
                break;
            }
            it.index = a;
            it.had = state == 1;
            if (al->n_tracks > 0) sl_track_path(al->tracks[0], it.path, sizeof it.path);
            strlcpy(it.artist, al->artist ? al->artist : "", sizeof it.artist);
            strlcpy(it.album, al->title ? al->title : "", sizeof it.album);
        }
        sl_libjob_give(&it);
        lb_next++;
    }
    if (lb_next >= lb_total) sl_libjob_fed_all();
}
static void lb_poll(void)
{
    if (lb_kind < 0) return;
    lb_feed();
    struct sl_libjob_status s;
    sl_libjob_status(&s);
    /* a cover put in its folder: the cover thread makes it */
    if (s.kind == SL_LIBJOB_COVERS && s.found_seq != lb_found_seq)
    {
        lb_found_seq = s.found_seq;
        if (s.last_found >= 0 && s.last_found < sl_lib.n_albums) sl_art_again(s.last_found);
    }
    bool moved = s.seq != lb_st.seq || s.state != lb_st.state;
    lb_st = s;
    if ((s.state == SL_LIBJOB_DONE || s.state == SL_LIBJOB_STOPPED || s.state == SL_LIBJOB_FAILED) && !sl_libjob_busy())
    {
        char m[128], n[24];
        sl_fmt_count(n, sizeof n, s.found);
        if (s.state == SL_LIBJOB_FAILED) snprintf(m, sizeof m, "%s", sl_tr(s.why));
        else if (s.kind == SL_LIBJOB_LYRICS) snprintf(m, sizeof m, sl_tr(s.found == 1 ? "Lyrics found for %s song" : "Lyrics found for %s songs"), n);
        else snprintf(m, sizeof m, sl_tr(s.found == 1 ? "%s cover found" : "%s covers found"), n);
        if (s.state != SL_LIBJOB_STOPPED || s.found)
            notify_ex(NS_LIBRARY, s.state == SL_LIBJOB_FAILED ? NOTE_WARN : NOTE_INFO, 0, NA_NONE, "libjob", m);
        sl_log("libjob: %s %s: %d found, %d had, %d missing, %d failed, at %d of %d%s%s", s.kind == SL_LIBJOB_LYRICS ? "lyrics" : "covers",
               s.state == SL_LIBJOB_DONE ? "done" : s.state == SL_LIBJOB_FAILED ? "failed" : "stopped",
               s.found, s.had, s.missing, s.failed, s.fed, s.total, s.why[0] ? ": " : "", s.why);
        lb_kind = -1;
        moved = true;
    }
    if (moved) lb_refresh();
}

/* ------------------------------------------------------------ the page */
static void lb_page_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != lb_page_obj) return;
    lb_page_obj = NULL;
    memset(lb_line, 0, sizeof lb_line);
    memset(lb_bar, 0, sizeof lb_bar);
    memset(lb_btn, 0, sizeof lb_btn);
}
static void lb_card_text(int kind, char *b, size_t n, int *permille)
{
    const struct sl_libjob_status *s = &lb_st;
    bool mine = s->kind == kind && (lb_kind == kind || s->state >= SL_LIBJOB_DONE);
    char x[24], y[24], z[24];
    *permille = -1;
    if (!mine || s->state == SL_LIBJOB_IDLE)
    {
        int pos = lb_pos_load(kind), total = kind == SL_LIBJOB_LYRICS ? sl_lib.n_tracks : sl_lib.n_albums;
        if (pos > 0 && pos < total)
            snprintf(b, n, sl_tr(kind == SL_LIBJOB_LYRICS ? "Stopped at %s of %s songs" : "Stopped at %s of %s albums"),
                     sl_fmt_count(x, sizeof x, pos), sl_fmt_count(y, sizeof y, total));
        else snprintf(b, n, "%s", sl_tr(kind == SL_LIBJOB_LYRICS ? "Songs that already have lyrics are skipped." :
                                                                "Albums that already have a cover are skipped."));
        return;
    }
    *permille = s->total ? (int)((int64_t)s->fed * 1000 / s->total) : 0;
    if ((s->state == SL_LIBJOB_WAITING || s->state == SL_LIBJOB_FAILED) && s->why[0]) { snprintf(b, n, "%s", sl_tr(s->why)); return; }
    snprintf(b, n, sl_tr(kind == SL_LIBJOB_LYRICS ? "%s of %s songs \xc2\xb7 %s found" : "%s of %s albums \xc2\xb7 %s found"),
             sl_fmt_count(x, sizeof x, s->fed), sl_fmt_count(y, sizeof y, s->total), sl_fmt_count(z, sizeof z, s->found));
}
static void lb_refresh(void)
{
    if (!lb_page_obj) return;
    for (int k = 0; k < SL_LIBJOB_KINDS; k++)
    {
        if (!lb_line[k]) continue;
        char b[160];
        int pm;
        lb_card_text(k, b, sizeof b, &pm);
        label_if_changed(lb_line[k], b);
        hidden_if_changed(lv_obj_get_parent(lb_bar[k]), pm < 0);
        if (pm >= 0 && lv_bar_get_value(lb_bar[k]) != pm) lv_bar_set_value(lb_bar[k], pm, LV_ANIM_OFF);
        bool running = lb_kind == k;
        int pos = lb_pos_load(k), total = k == SL_LIBJOB_LYRICS ? sl_lib.n_tracks : sl_lib.n_albums;
        const char *what = running ? "Stop" : pos > 0 && pos < total ? "Carry on" :
                           k == SL_LIBJOB_LYRICS ? "Get lyrics" : "Get covers";
        label_if_changed(lv_obj_get_child(lb_btn[k], 0), sl_tr(what));
    }
}
static void lb_card(lv_obj_t *body, int kind)
{
    lv_obj_t *g = set_group(body, kind == SL_LIBJOB_LYRICS ? "LYRICS" : "COVERS");
    set_text(g, kind == SL_LIBJOB_LYRICS ?
             "Synced words from LRCLIB for every song, saved beside it as an .lrc file, so they're there without Wi-Fi." :
             "Covers from MusicBrainz and the Cover Art Archive for albums that have none, saved as cover.jpg in the album's folder.",
             C_TEXT);
    lv_obj_t *row = box(g);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, SL_INSET, 0);
    lv_obj_set_style_pad_top(row, SL_SP_XS, 0);
    lb_bar[kind] = lv_bar_create(row);
    lv_obj_set_size(lb_bar[kind], lv_pct(100), 8);
    lv_bar_set_range(lb_bar[kind], 0, 1000);
    lv_obj_set_style_radius(lb_bar[kind], SL_R_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(lb_bar[kind], SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(lb_bar[kind], lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lb_bar[kind], sl_theme() == 1 ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lb_bar[kind], lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    if (set_rtl()) lv_obj_set_style_base_dir(lb_bar[kind], LV_BASE_DIR_RTL, 0);
    lb_line[kind] = set_text(g, "", C_SUB);
    lb_btn[kind] = set_action(g, kind == SL_LIBJOB_LYRICS ? LV_SYMBOL_AUDIO : LV_SYMBOL_IMAGE,
                              kind == SL_LIBJOB_LYRICS ? 0x8b6fd6 : 0xe8408a, "Get lyrics", NULL, lb_btn_cb, (void *)(intptr_t)kind);
}
static void libjob_page(void)
{
    if (page_bring_back(lb_page_obj)) return;
    lv_obj_t *p, *body = page_new("Lyrics and covers", &p);
    lb_page_obj = p;
    lv_obj_add_event_cb(p, lb_page_deleted, LV_EVENT_DELETE, NULL);
    lb_card(body, SL_LIBJOB_LYRICS);
    lb_card(body, SL_LIBJOB_COVERS);
    opt_note(body, "Over Wi-Fi, a little at a time, while you listen. Stop whenever you like: it carries on from there next time. Nothing on the card is replaced.");
    if (lb_kind < 0) sl_libjob_status(&lb_st);
    lb_refresh();
    page_push(p);
}
/* Settings > Library's row: where a job is */
static const char *lb_v_row(void)
{
    static char b[48];
    if (lb_kind < 0) return NULL;
    char x[24];
    snprintf(b, sizeof b, "%s%%", sl_fmt_count(x, sizeof x, lb_st.total ? (int)((int64_t)lb_st.fed * 100 / lb_st.total) : 0));
    return b;
}
