/* Scrobbling: what you play goes to ListenBrainz and Last.fm over Wi-Fi,
 * and to the Audioscrobbler log (/.scrobbler.log) a computer can upload.
 * Listening history decides when a play counts (sloppy_history.inc calls
 * scr_counted); sloppy_net.c keeps the queue, the token and the requests
 * on its own thread, so nothing here waits on the card or the network.
 * Last.fm signs in with a phone (a QR code) or a name and password; the
 * session it gives never runs out, and when Last.fm ends it the page and a
 * notification say so while the songs wait.
 * Included from sloppy.c after the Wi-Fi page, whose keyboard it uses. */
#include "sloppy_history_state.h"

static struct { uint32_t hash; struct sl_listen l; } scr_song;   /* the song that started last */
static struct sl_scrobble_status scr_st;
static bool scr_on;                        /* the worker has started */
static lv_obj_t *scr_page_obj, *scr_token_row, *scr_test_row, *scr_forget_row, *scr_lines[3];
static lv_obj_t *scr_lfm_row, *scr_lfm_line, *scr_lfm_problem, *scr_lfm_out_row;
static lv_obj_t *scr_tok_ta;

static bool scr_any(void) { return global_settings.sloppy_listenbrainz || global_settings.sloppy_scrobble_log || global_settings.sloppy_lastfm; }
static int scr_bits(void)
{
    return !!global_settings.sloppy_listenbrainz | !!global_settings.sloppy_scrobble_log << 1 | !!global_settings.sloppy_lastfm << 2;
}

static int scr_applied = -1;               /* the settings the worker has, as bits */
static void scr_apply(void)
{
    if (scr_any() || scr_page_obj) { sl_net_scrobble_start(); scr_on = true; }
    sl_net_scrobble_config(global_settings.sloppy_listenbrainz, global_settings.sloppy_scrobble_log, global_settings.sloppy_lastfm);
    scr_applied = scr_bits();
}

/* From hist_poll when a song starts: keep its tags, which are gone by the
 * time the play is logged (the next song has begun), and tell
 * ListenBrainz what is playing. */
static void scr_started(const struct mp3entry *id3, uint32_t hash, uint32_t elapsed)
{
    scr_song.hash = 0;
    if (!scr_any() || !id3) return;
    struct sl_listen *l = &scr_song.l;
    memset(l, 0, sizeof *l);
    sl_scrob_clean(l->artist, sizeof l->artist, id3->artist && id3->artist[0] ? id3->artist : id3->albumartist);
    sl_scrob_clean(l->title, sizeof l->title, id3->title);
    sl_scrob_clean(l->album, sizeof l->album, id3->album);
    if (id3->mb_track_id && sl_scrob_uuid(id3->mb_track_id)) strlcpy(l->mbid, id3->mb_track_id, sizeof l->mbid);
    l->track = id3->tracknum > 0 ? id3->tracknum : 0;
    l->secs = (uint32_t)(id3->length / 1000);
    scr_song.hash = hash;
    /* a song begun, not one picked up halfway (after USB mode, say) */
    if ((global_settings.sloppy_listenbrainz || global_settings.sloppy_lastfm) && elapsed < 10000 && l->artist[0] && l->title[0])
        sl_net_scrobble_now(l);
}

/* From hist_poll and hist_flush: a play history counted. It is a listen
 * when it also counts for Last.fm; len_ms is the song's length. */
static void scr_counted(const struct sl_play *p, uint32_t len_ms)
{
    if (!scr_any() || !scr_song.hash || p->path_hash != scr_song.hash) return;
    scr_song.hash = 0;
    if (!sl_scrob_counts(p->ms, len_ms)) return;
    struct sl_listen *l = &scr_song.l;
    l->when = p->when;
    if (len_ms / 1000 > l->secs) l->secs = len_ms / 1000;
    if (!l->artist[0] || !l->title[0]) { sl_log("scrobble: no artist or title tag, not scrobbled"); return; }
    if (l->when < SL_HIST_VALID_CLOCK) { sl_log("scrobble: the clock is not set, not scrobbled"); return; }
    if (!sl_net_scrobble_add(l)) sl_log("scrobble: the worker is behind, a listen was lost");
}

/* "Last sent: 5 min ago" */
static void scr_ago(char *b, size_t n, uint32_t at)
{
    uint32_t now = (uint32_t)time(NULL), d = now > at ? now - at : 0;
    if (d < 60) snprintf(b, n, "%s: %s", sl_tr("Last sent"), sl_tr("now"));
    else if (d < 3600) snprintf(b, n, "%s: %u %s", sl_tr("Last sent"), (unsigned)(d / 60), sl_tr("min ago"));
    else if (d < 86400) snprintf(b, n, "%s: %u %s", sl_tr("Last sent"), (unsigned)(d / 3600), sl_tr("h ago"));
    else snprintf(b, n, "%s: %u %s", sl_tr("Last sent"), (unsigned)(d / 86400), sl_tr("d ago"));
}

static void scr_refresh(void)
{
    if (!scr_page_obj) return;
    const struct sl_scrobble_status *s = &scr_st;
    char b[128];
    set_link_text(scr_token_row, sl_tr(!s->has_token ? "Not set" : s->token_bad ? "Not accepted, enter it again" : "Saved"),
                  sl_tr(!s->has_token ? "Not set" : s->token_bad ? "Not accepted" : "Saved"));
    if (s->test == SL_SCROB_GOOD) snprintf(b, sizeof b, "%s %s", sl_tr("Signed in as"), s->user);
    else snprintf(b, sizeof b, "%s", sl_tr(s->test == SL_SCROB_TESTING ? "Checking..." :
                  s->test == SL_SCROB_BAD ? "Not accepted" :
                  s->test == SL_SCROB_NO_ANSWER && s->error[0] ? s->error : "Check the token with ListenBrainz"));
    label_if_changed(lv_obj_get_child(scr_test_row, 1), b);
    hidden_if_changed(scr_forget_row, !s->has_token);
    hidden_if_changed(lv_obj_get_parent(scr_forget_row), !s->has_token);   /* its card too */
    if (s->queued) snprintf(b, sizeof b, "%s: %d", sl_tr("Waiting to send"), s->queued);
    else snprintf(b, sizeof b, "%s", sl_tr("Nothing waiting to send"));
    label_if_changed(scr_lines[0], b);
    if (s->last_sent) scr_ago(b, sizeof b, s->last_sent);
    else snprintf(b, sizeof b, "%s", sl_tr("Nothing sent yet"));
    label_if_changed(scr_lines[1], b);
    const char *problem = s->error[0] ? s->error :
                          global_settings.sloppy_listenbrainz && s->queued && s->has_token && !wf_st.connected ?
                          "Waiting for Wi-Fi" : "";
    label_if_changed(scr_lines[2], sl_tr(problem));
    hidden_if_changed(scr_lines[2], !problem[0]);
    hidden_if_changed(lv_obj_get_parent(scr_lines[2]), !problem[0]);
    /* Last.fm */
    const char *acct = s->lfm_keyless ? "Not in this build" : s->lfm == SL_LFM_IN ? s->lfm_user :
                       s->lfm == SL_LFM_LOST ? "Sign in again" : "Not signed in";
    set_link_text(scr_lfm_row, s->lfm == SL_LFM_IN ? s->lfm_user : sl_tr(acct), s->lfm == SL_LFM_IN ? s->lfm_user : sl_tr(acct));
    if (s->lfm_queued) snprintf(b, sizeof b, "%s: %d", sl_tr("Waiting to send"), s->lfm_queued);
    else if (s->lfm_last_sent) scr_ago(b, sizeof b, s->lfm_last_sent);
    else snprintf(b, sizeof b, "%s", sl_tr("Nothing waiting to send"));
    label_if_changed(scr_lfm_line, b);
    const char *lp = s->lfm == SL_LFM_LOST ? "Last.fm signed this player out. Sign in again; your songs wait on the player." :
                     s->lfm_error[0] && s->lfm == SL_LFM_IN ? s->lfm_error :
                     global_settings.sloppy_lastfm && s->lfm == SL_LFM_OUT && !s->lfm_keyless && s->lfm_queued ? "Sign in to send what waits" :
                     global_settings.sloppy_lastfm && s->lfm == SL_LFM_IN && s->lfm_queued && !wf_st.connected ? "Waiting for Wi-Fi" : "";
    label_if_changed(scr_lfm_problem, sl_tr(lp));
    hidden_if_changed(lv_obj_get_parent(scr_lfm_problem), !lp[0]);
    hidden_if_changed(scr_lfm_out_row, s->lfm == SL_LFM_OUT);
    hidden_if_changed(lv_obj_get_parent(scr_lfm_out_row), s->lfm == SL_LFM_OUT);   /* its card too */
}

/* With the UI tick: the worker's log lines, its toasts, the page. */
static void lfm_page_refresh(void);
static void scr_poll(void)
{
    if (!scr_on) return;
    /* Reset all settings turns both off behind the page's back */
    if (scr_applied != scr_bits()) scr_apply();
    sl_net_scrobble_poll();
    char m[128];
    while (sl_net_scrobble_message(m, sizeof m)) sl_log("scrobble: %s", m);
    struct sl_scrobble_status s;
    sl_net_scrobble_status(&s);
    if (s.test_seq != scr_st.test_seq)
    {
        if (s.test == SL_SCROB_GOOD)
        {
            /* the first sentence is the title */
            char t[128];
            snprintf(t, sizeof t, "%s. %s %s", sl_tr("Token works"), sl_tr("Signed in as"), s.user);
            notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "scrobble", t);
        }
        else if (s.test == SL_SCROB_BAD) notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "scrobble", "ListenBrainz did not accept the token");
        else notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "scrobble", s.error[0] ? s.error : "ListenBrainz did not answer");
    }
    else if (s.fail_seq != scr_st.fail_seq)
        notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "scrobble", s.token_bad ?
                  "ListenBrainz did not accept the token. Enter it again in Scrobbling." :
                  "ListenBrainz is not answering. Songs wait on the player.");
    if (s.lfm_fail_seq != scr_st.lfm_fail_seq)
        notify_ex(NS_MUSIC, NOTE_WARN, 0, NA_NONE, "lastfm", s.lfm == SL_LFM_LOST ?
                  "Last.fm signed this player out. Sign in again in Scrobbling; your songs wait on the player." :
                  s.lfm_error[0] ? s.lfm_error : "Last.fm is not answering. Songs wait on the player.");
    if (s.lfm_auth_seq != scr_st.lfm_auth_seq && s.lfm_auth == SL_LFM_AUTH_OK)
    {
        char t[128];
        snprintf(t, sizeof t, "%s. %s %s", sl_tr("Signed in to Last.fm"), sl_tr("Signed in as"), s.lfm_user);
        notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "lastfm", t);
    }
    bool changed = memcmp(&s, &scr_st, sizeof s) != 0;
    bool auth_moved = s.lfm_auth_seq != scr_st.lfm_auth_seq;
    scr_st = s;
    if (changed) scr_refresh();
    if (auth_moved) lfm_page_refresh();
}

/* ---------------------------------------------------------- the token */
static void scr_tok_save_cb(lv_event_t *e)
{
    (void)e;
    char t[64];
    strlcpy(t, lv_textarea_get_text(scr_tok_ta), sizeof t);
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') t[--n] = '\0';
    bool ok = n >= 8;
    for (size_t i = 0; ok && i < n; i++)
        ok = (t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '-';
    if (!ok) { memset(t, 0, sizeof t); toast("That does not look like a token"); return; }
    sl_net_scrobble_token(t);
    memset(t, 0, sizeof t);
    lv_textarea_set_text(scr_tok_ta, "");
    if (wf_st.connected) sl_net_scrobble_test();   /* says who it belongs to */
    else toast("Token saved");
    page_pop();
}

static void scr_tok_show_cb(lv_event_t *e)
{
    bool show = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(scr_tok_ta, !show);
}

static void scr_token_page(void)
{
    lv_obj_t *p, *body = page_new("User token", &p);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *g = set_group(body, "LISTENBRAINZ TOKEN");
    scr_tok_ta = set_field(g);
    lv_textarea_set_password_mode(scr_tok_ta, true);
    lv_textarea_set_max_length(scr_tok_ta, 63);
    lv_obj_t *show = opt_switch(g, "Show token", false, NULL);
    lv_obj_add_event_cb(show, scr_tok_show_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_SAVE, 0xeb743b, "Save token", NULL, scr_tok_save_cb, NULL);
    wf_kb_build(p, scr_tok_ta, KB_PASSWORD, scr_tok_save_cb, "Save");
    page_push(p);
}

/* ---------------------------------------------------------- Last.fm sign-in */
static lv_obj_t *lfm_page_obj, *lfm_body, *lfm_pw_page, *lfm_user_ta, *lfm_pass_ta;
static void lfm_page_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != lfm_page_obj) return;
    lfm_page_obj = lfm_body = NULL;
    if (scr_st.lfm_auth == SL_LFM_AUTH_TOKEN || scr_st.lfm_auth == SL_LFM_AUTH_WAIT) sl_net_lastfm_signin_cancel();
}
static void lfm_again_cb(lv_event_t *e)
{
    (void)e;
    if (!wf_st.connected) { toast("Join a Wi-Fi network first"); return; }
    sl_net_lastfm_signin_phone();
    scr_poll();
}
static void lfm_pw_page_open(lv_event_t *e);
/* the sign-in: a code to scan, what is happening under it, a password instead */
static void lfm_page_refresh(void)
{
    if (!lfm_body) return;
    lv_obj_clean(lfm_body);
    const struct sl_scrobble_status *s = &scr_st;
    lv_obj_t *g = set_group(lfm_body, NULL);
    lv_obj_set_style_pad_all(g, SL_INSET, 0);
    lv_obj_set_style_pad_row(g, SL_SP_S, 0);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const char *head, *line;
    if (s->lfm_auth == SL_LFM_AUTH_WAIT && s->lfm_url[0])
    {
        qr_tile(g, s->lfm_url, 232);
        head = "Scan with your phone";
        line = "Sign in to Last.fm there and allow GabagoolOS. This page carries on by itself.";
    }
    else if (s->lfm_auth == SL_LFM_AUTH_OK || (s->lfm == SL_LFM_IN && s->lfm_auth == SL_LFM_AUTH_IDLE))
    {
        lv_obj_set_style_margin_bottom(sl_icon(g, IC_SONGS, 72, C_DIM, C_CARD), SL_SP_S, 0);
        head = "Signed in to Last.fm";
        line = s->lfm_user;
    }
    else if (s->lfm_auth == SL_LFM_AUTH_FAILED)
    {
        lv_obj_set_style_margin_bottom(sl_icon(g, IC_WIFI, 72, C_DIM, C_CARD), SL_SP_S, 0);
        head = s->lfm_error[0] ? s->lfm_error : "Last.fm did not answer";
        line = "Check Wi-Fi, then try again.";
    }
    else
    {
        lv_obj_set_style_margin_bottom(sl_icon(g, IC_SONGS, 72, C_DIM, C_CARD), SL_SP_S, 0);
        head = "Asking Last.fm\xe2\x80\xa6";
        line = "A code to scan comes up in a moment.";
    }
    lv_obj_t *t = text(g, head, &ss_semibold_23, C_TEXT);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    sl_label_deep(t);
    lv_obj_t *l = text(g, line, &ss_regular_16, C_SUB);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    sl_label_deep(l);
    if (s->lfm_auth == SL_LFM_AUTH_FAILED) sl_empty_action(g, LV_SYMBOL_REFRESH "  Try again", lfm_again_cb, NULL);
    if (s->lfm_auth != SL_LFM_AUTH_OK && s->lfm != SL_LFM_IN)
    {
        g = set_group(lfm_body, NULL);
        set_link(g, LV_SYMBOL_KEYBOARD, 0x66738a, "Sign in with a password instead", NULL, lfm_pw_page_open, NULL);
        opt_note(lfm_body, "The password goes to Last.fm once and is not kept. The player keeps only the sign-in Last.fm gives it, which does not run out.");
    }
}
static void lfm_page(void)
{
    if (page_bring_back(lfm_page_obj)) return;
    lv_obj_t *p;
    lfm_body = page_new("Last.fm", &p);
    lfm_page_obj = p;
    lv_obj_add_event_cb(p, lfm_page_deleted, LV_EVENT_DELETE, NULL);
    if (scr_st.lfm != SL_LFM_IN || scr_st.lfm_auth != SL_LFM_AUTH_IDLE)
    {
        if (wf_st.connected) sl_net_lastfm_signin_phone();
        else { sl_net_lastfm_signin_cancel(); scr_st.lfm_auth = SL_LFM_AUTH_FAILED; snprintf(scr_st.lfm_error, sizeof scr_st.lfm_error, "Join a Wi-Fi network first"); }
    }
    scr_poll();
    lfm_page_refresh();
    page_push(p);
}
/* with a name and a password, on the Wi-Fi keyboard */
static void lfm_pw_focus(lv_obj_t *ta)
{
    lv_obj_t *other = ta == lfm_user_ta ? lfm_pass_ta : lfm_user_ta;
    if (other) lv_obj_remove_state(other, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (lfm_pw_page) kb_retarget(kb_of_page(lfm_pw_page), ta);
}
static void lfm_pw_field_cb(lv_event_t *e) { lfm_pw_focus(lv_event_get_current_target(e)); }
static void lfm_pw_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == lfm_pw_page) lfm_pw_page = lfm_user_ta = lfm_pass_ta = NULL;
}
static void lfm_pw_go_cb(lv_event_t *e)
{
    (void)e;
    const char *u = lv_textarea_get_text(lfm_user_ta), *pw = lv_textarea_get_text(lfm_pass_ta);
    if (!u[0]) { toast("Type your Last.fm name"); lfm_pw_focus(lfm_user_ta); return; }
    if (!pw[0]) { lfm_pw_focus(lfm_pass_ta); return; }
    if (!wf_st.connected) { toast("Join a Wi-Fi network first"); return; }
    sl_net_lastfm_signin_password(u, pw);
    lv_textarea_set_text(lfm_pass_ta, "");
    page_pop();
    scr_poll();
}
static lv_obj_t *lfm_pw_field(lv_obj_t *g, const char *hint)
{
    lv_obj_t *ta = set_field(g);
    lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    sl_ta_hint(ta, sl_tr(hint));
    lv_obj_set_style_text_color(ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    if (set_rtl()) lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ta, lfm_pw_field_cb, LV_EVENT_PRESSED, NULL);
    return ta;
}
static void lfm_pw_page_open(lv_event_t *e)
{
    (void)e;
    if (page_bring_back(lfm_pw_page)) return;
    if (lfm_page_obj) sl_net_lastfm_signin_cancel();   /* one way at a time */
    lv_obj_t *p, *body = page_new("Last.fm password", &p);
    lfm_pw_page = p;
    lv_obj_add_event_cb(p, lfm_pw_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_bottom(body, WF_KB_H + SL_GUTTER, 0);
    lv_obj_t *g = set_group(body, "LAST.FM NAME");
    lfm_user_ta = lfm_pw_field(g, "Your Last.fm name");
    lv_textarea_set_max_length(lfm_user_ta, 60);
    g = set_group(body, "PASSWORD");
    lfm_pass_ta = lfm_pw_field(g, "Password");
    lv_textarea_set_password_mode(lfm_pass_ta, true);
    lv_textarea_set_max_length(lfm_pass_ta, 120);
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_OK, 0xd51007, "Sign in", NULL, lfm_pw_go_cb, NULL);
    wf_kb_build(p, lfm_user_ta, KB_PASSWORD, lfm_pw_go_cb, "Sign in");
    lfm_pw_focus(lfm_user_ta);
    page_push(p);
}
static void scr_lfm_set(int on) { global_settings.sloppy_lastfm = on; scr_apply(); scr_refresh(); }
static void scr_lfm_cb(lv_event_t *e)
{
    (void)e;
    if (scr_st.lfm_keyless) { toast("This build has no Last.fm key"); return; }
    lfm_page();
}
static void scr_lfm_out_cb(lv_event_t *e)
{
    (void)e;
    sl_net_lastfm_signout();
    toast("Signed out of Last.fm");
}

/* ------------------------------------------------------------ the page */
static void scr_lb_set(int on) { global_settings.sloppy_listenbrainz = on; scr_apply(); scr_refresh(); }
static void scr_log_set(int on) { global_settings.sloppy_scrobble_log = on; scr_apply(); }
static void scr_token_cb(lv_event_t *e) { (void)e; scr_token_page(); }
static void scr_test_cb(lv_event_t *e)
{
    (void)e;
    if (!scr_st.has_token) { toast("Add your token first"); return; }
    if (!wf_st.connected) { toast("Join a Wi-Fi network first"); return; }
    sl_net_scrobble_test();
}
static void scr_forget_cb(lv_event_t *e)
{
    (void)e;
    sl_net_scrobble_token("");
    toast("Token forgotten");
}
static void scr_page_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == scr_page_obj) scr_page_obj = NULL;
}

static void scr_page(void)
{
    if (page_bring_back(scr_page_obj)) return;
    lv_obj_t *p, *body = page_new("Scrobbling", &p);
    scr_page_obj = p;
    lv_obj_add_event_cb(p, scr_page_deleted, LV_EVENT_DELETE, NULL);
    scr_apply();                           /* the worker knows the token and the queue */
    lv_obj_t *c = set_group(body, "ListenBrainz");
    set_switch_ex(c, LV_SYMBOL_UPLOAD, 0xeb743b, "Send to ListenBrainz",
                  "What you play goes to your ListenBrainz profile over Wi-Fi. Without Wi-Fi it waits on the player.",
                  global_settings.sloppy_listenbrainz, scr_lb_set);
    scr_token_row = set_link_sub(c, LV_SYMBOL_EDIT, 0x66738a, "User token", "Not set", "Not set", scr_token_cb, NULL);
    scr_test_row = set_action_sub(c, LV_SYMBOL_OK, 0x3aa856, "Test token", "Check the token with ListenBrainz", scr_test_cb, NULL);
    opt_note(body, "Find your token on listenbrainz.org: sign in, open Settings and copy the User token. "
                   "Or save it from a computer as .rockbox/sloppy/listenbrainz.token.");
    c = set_group(body, "Status");
    for (int i = 0; i < 3; i++) scr_lines[i] = set_text(c, "", i == 2 ? NOTE_WARN_COLOR : C_SUB);
    c = set_group(body, "Last.fm");
    set_switch_ex(c, LV_SYMBOL_UPLOAD, 0xd51007, "Send to Last.fm",
                  "What you play goes to your Last.fm profile over Wi-Fi. Without Wi-Fi, or signed out, it waits on the player.",
                  global_settings.sloppy_lastfm, scr_lfm_set);
    scr_lfm_row = set_link_sub(c, LV_SYMBOL_EDIT, 0x66738a, "Last.fm account", "Not signed in", "Not signed in", scr_lfm_cb, NULL);
    scr_lfm_line = set_text(c, "", C_SUB);
    scr_lfm_problem = set_text(c, "", NOTE_WARN_COLOR);
    set_switch(c, "Keep .scrobbler.log", "Each song you play is also written to .scrobbler.log at the top of the card, "
               "as Rockbox players do. Upload it to Last.fm from a computer.", global_settings.sloppy_scrobble_log, scr_log_set);
    c = set_group(body, NULL);
    scr_lfm_out_row = set_danger(c, LV_SYMBOL_CLOSE, "Sign out of Last.fm", scr_lfm_out_cb, NULL);
    c = set_group(body, NULL);
    scr_forget_row = set_danger(c, LV_SYMBOL_TRASH, "Forget token", scr_forget_cb, NULL);
    page_push(p);
    scr_poll();
    scr_refresh();
}
