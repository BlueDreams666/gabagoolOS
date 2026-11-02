/* Settings > Software update, the interface's side of sloppy_ota.c: what
 * this build is, an update found on the card or sent from the phone
 * remote, its notes, Install, the progress, Restart to update, the errors
 * in plain words and Previous version. The checks run on the engine's
 * thread; this reads its state with the UI tick and never waits but for
 * the swap itself, on the way to a restart. Included from sloppy.c before
 * the remote, which answers /api/update with these words. */
#include "sloppy_ota.h"

static lv_obj_t *up_page_obj, *up_box, *up_bar, *up_line, *up_title;
static unsigned up_seq;                   /* the status the page shows */
static int up_shape = -1;                 /* its state and why: a change rebuilds the page */
static struct sl_ota_status up_st;
static char up_noticed[16];               /* the version the quiet notice was for */
static uint32_t up_since_ms;              /* the first tick: a new version is kept after SL_OTA_HEALTHY_MS */
static bool up_restarting;

static const char *up_lang_code(void)
{
    return sl_lang_code(global_settings.sloppy_language);
}
static void up_mb(char *b, size_t n, uint64_t bytes)
{
    if (bytes < 10ull << 20) snprintf(b, n, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes & 0xfffff) * 10 >> 20));
    else snprintf(b, n, "%u MB", (unsigned)((bytes + (1u << 19)) >> 20));
}
static bool up_battery_ok(void)
{
#if CONFIG_CHARGING
    if (charger_inserted()) return true;
#endif
    return battery_level() < 0 || battery_level() >= 30;
}
/* what went wrong, as a title and a line; %s is a version or a size */
static void up_why_words(const struct sl_ota_status *s, char *t, size_t tn, char *l, size_t ln)
{
    char mb[24];
    switch (s->why)
    {
        case SL_OTA_E_NOKEY:
            snprintf(t, tn, "%s", sl_tr("This build can't check updates"));
            snprintf(l, ln, "%s", sl_tr("It was made without the release key, so it can't tell a real update from a fake one. It installs none."));
            break;
        case SL_OTA_E_NOCRYPTO:
            snprintf(t, tn, "%s", sl_tr("This player can't check updates"));
            snprintf(l, ln, "%s", sl_tr("Its system has no way to check an update's signature, so nothing is installed."));
            break;
        case SL_OTA_E_OLDER:
            snprintf(t, tn, sl_tr("GabagoolOS %s is older than this one"), s->version);
            snprintf(l, ln, sl_tr("Nothing changed. This player has %s."), SL_VERSION);
            break;
        case SL_OTA_E_MINFROM:
            snprintf(t, tn, sl_tr("This update needs GabagoolOS %s first"), s->min_from);
            snprintf(l, ln, sl_tr("Install %s, then this one. Nothing changed."), s->min_from);
            break;
        case SL_OTA_E_DAMAGED:
            snprintf(t, tn, "%s", sl_tr("The update file is damaged"));
            snprintf(l, ln, "%s", sl_tr("Copy it to the card again. Nothing changed."));
            break;
        case SL_OTA_E_SPACE:
            up_mb(mb, sizeof mb, s->need);
            snprintf(t, tn, "%s", sl_tr("Not enough space on the card"));
            snprintf(l, ln, sl_tr("Free %s more, then try again. Nothing changed."), mb);
            break;
        case SL_OTA_E_USB:
            snprintf(t, tn, "%s", sl_tr("Stopped for USB mode"));
            snprintf(l, ln, "%s", sl_tr("Nothing changed. Try again once the player is back."));
            break;
        case SL_OTA_E_CARD:
            snprintf(t, tn, "%s", sl_tr("Couldn't write to the card"));
            snprintf(l, ln, "%s", sl_tr("Check the card, then try again. Nothing changed."));
            break;
        case SL_OTA_E_PATH:
            snprintf(t, tn, "%s", sl_tr("This update wants to change files it may not touch"));
            snprintf(l, ln, "%s", sl_tr("It wasn't installed. Nothing changed."));
            break;
        default:
            snprintf(t, tn, "%s", sl_tr("This file isn't a GabagoolOS update"));
            snprintf(l, ln, "%s", sl_tr("Nothing changed."));
            break;
    }
}
/* the state in one line, for the phone remote */
static void up_line_words(const struct sl_ota_status *s, char *b, size_t n)
{
    char t[160], l[200], a[24], c[24];
    if (!s->key) { up_why_words(&(struct sl_ota_status){ .why = SL_OTA_E_NOKEY }, t, sizeof t, l, sizeof l); snprintf(b, n, "%s", t); return; }
    switch (s->state)
    {
        case SL_OTA_LOOKING: snprintf(b, n, "%s", sl_tr("Looking for an update\xe2\x80\xa6")); break;
        case SL_OTA_RECEIVING:
            up_mb(a, sizeof a, s->rx); up_mb(c, sizeof c, s->rx_total);
            snprintf(b, n, sl_tr("Receiving %s of %s"), a, c);
            break;
        case SL_OTA_FOUND: snprintf(b, n, sl_tr("GabagoolOS %s is ready to install on the player"), s->version); break;
        case SL_OTA_WORKING: snprintf(b, n, "%s", sl_tr("Checking the update\xe2\x80\xa6")); break;
        case SL_OTA_READY: snprintf(b, n, sl_tr("GabagoolOS %s is checked. Restart the player to finish."), s->version); break;
        case SL_OTA_REJECTED: up_why_words(s, t, sizeof t, l, sizeof l); snprintf(b, n, "%s. %s", t, l); break;
        default: snprintf(b, n, sl_tr("GabagoolOS %s. No update on the card."), SL_VERSION); break;
    }
}

/* ------------------------------------------------------------ pieces */
static lv_obj_t *up_label(lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t color, bool wrap)
{
    lv_obj_t *l = text(parent, s, f, color);
    lv_obj_set_width(l, lv_pct(100));
    if (wrap) lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    else label_lines(l, 1);
    set_align(l);
    sl_label_deep(l);
    return l;
}
static lv_obj_t *up_card(lv_obj_t *body, const char *header)
{
    lv_obj_t *g = set_group(body, header);
    lv_obj_set_style_pad_all(g, SL_INSET, 0);
    lv_obj_set_style_pad_row(g, SL_SP_S, 0);
    return g;
}
/* this build: the name, the version, the update's name as Settings has it */
static void up_version_card(lv_obj_t *body, const char *state, uint32_t color)
{
    lv_obj_t *g = up_card(body, NULL);
    lv_obj_t *row = box(g);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, SL_GUTTER, 0);
    lv_obj_set_style_base_dir(row, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    if (!sl_zune()) set_icon(row, LV_SYMBOL_REFRESH, 0x2f9e8f, SET_ICON + SL_SP_S);   /* Zule draws no row icons */
    lv_obj_t *col = box(row);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    char v[48];
    snprintf(v, sizeof v, "GabagoolOS %s", SL_VERSION);
    up_label(col, v, &ss_semibold_23, C_TEXT, false);
    up_label(col, "HiBy R1 \xc2\xb7 " SL_UPDATE_NAME, &ss_regular_16, C_SUB, false);
    if (state) up_label(g, state, &ss_regular_18, color, true);
}
static void up_pill(lv_obj_t *parent, const char *label, bool primary, lv_event_cb_t cb)
{
    lv_obj_t *p = mn_pill(parent, label, W - 2 * SL_GUTTER - 2 * SL_INSET, SL_PILL_H, primary, cb, NULL);
    lv_obj_set_style_margin_top(p, SL_SP_XS, 0);
}
static void up_bar_make(lv_obj_t *g, int value)
{
    up_bar = lv_bar_create(g);
    lv_obj_set_size(up_bar, lv_pct(100), 8);
    lv_bar_set_range(up_bar, 0, 1000);
    lv_bar_set_value(up_bar, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(up_bar, SL_R_FULL, LV_PART_MAIN);
    lv_obj_set_style_radius(up_bar, SL_R_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(up_bar, lv_color_hex(C_CARD_P), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(up_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(up_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_margin_ver(up_bar, SL_SP_XS, 0);
    if (set_rtl()) lv_obj_set_style_base_dir(up_bar, LV_BASE_DIR_RTL, 0);
}
static void up_notes(lv_obj_t *body, const char *notes)
{
    if (!notes[0]) return;
    lv_obj_t *g = up_card(body, "What's new");
    up_label(g, notes, &ss_regular_18, C_TEXT, true);
}
/* the progress, in place: no rebuild for each piece */
static void up_progress(void)
{
    if (!up_bar || !up_line) return;
    char a[24], b[24], l[96];
    uint64_t done = up_st.state == SL_OTA_RECEIVING ? up_st.rx : up_st.done,
             total = up_st.state == SL_OTA_RECEIVING ? up_st.rx_total : up_st.total;
    if (up_st.state == SL_OTA_WORKING && up_st.phase == SL_OTA_PH_COMPARE)
        snprintf(l, sizeof l, sl_tr("%d of %d files"), (int)done, (int)total);
    else
    {
        up_mb(a, sizeof a, done); up_mb(b, sizeof b, total);
        snprintf(l, sizeof l, sl_tr("%s of %s"), a, b);
    }
    label_if_changed(up_line, l);
    if (up_title && up_st.state == SL_OTA_WORKING)
        label_if_changed(up_title, sl_tr(up_st.phase == SL_OTA_PH_UNPACK ? "Unpacking the new files" :
                                         up_st.phase == SL_OTA_PH_COMPARE ? "Comparing with the card" : "Checking the update"));
    int v = total ? (int)(done * 1000 / total) : 0;
    if (v > 1000) v = 1000;
    if (lv_bar_get_value(up_bar) != v) lv_bar_set_value(up_bar, v, LV_ANIM_OFF);
}

/* ------------------------------------------------------------ the actions */
static void up_install_cb(lv_event_t *e) { (void)e; if (!sl_ota_install()) toast_warn("The update could not start"); }
static void up_cancel_cb(lv_event_t *e) { (void)e; sl_ota_cancel(); }
static void up_again_cb(lv_event_t *e) { (void)e; sl_ota_scan(); }
/* on the way out: what plays paused and its place saved, then the swap */
static void up_leave(int (*swap)(void))
{
    if (audio_status() & AUDIO_STATUS_PLAY) audio_pause();
    settings_save();
    hist_flush();
    notes_flush();
    int why = swap();
    if (why)
    {
        sl_log("update: the swap did not happen (%d)", why);
        toast_warn("Nothing changed. Try again.");
        sl_ota_scan();
        return;
    }
    up_restarting = true;
    sl_log_flush();
    system_reboot();
}
static void up_restart_cb(lv_event_t *e)
{
    (void)e;
    if (!up_battery_ok()) { toast_warn("Charge to 30% first, or plug in a charger."); return; }
    up_leave(sl_ota_swap);
}
static void up_revert_go_cb(lv_event_t *e) { (void)e; up_leave(sl_ota_revert); }
static void up_revert_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
static void up_revert_page(void)
{
    lv_obj_t *p, *body = page_new("Previous version", &p);
    char what[240], go[64];
    snprintf(what, sizeof what, sl_tr("The player restarts on GabagoolOS %s. Your settings, music, playlists and saves stay as they are."),
             up_st.prev_version);
    snprintf(go, sizeof go, sl_tr("Go back to %s"), up_st.prev_version);
    set_confirm(body, "RESTART", what, "Stay on this version", "Keep listening", up_revert_keep_cb, go, "Restart now",
                up_revert_go_cb, false);
    page_push(p);
}
static void up_revert_cb(lv_event_t *e) { (void)e; up_revert_page(); }

/* ------------------------------------------------------------ the page */
static void up_fill(void)
{
    if (!up_box) return;
    lv_obj_clean(up_box);
    up_bar = up_line = up_title = NULL;
    lv_obj_t *body = up_box;
    const struct sl_ota_status *s = &up_st;
    char t[160], l[240], b[200], a[24];
    if (!s->key || (s->state == SL_OTA_REJECTED && (s->why == SL_OTA_E_NOKEY || s->why == SL_OTA_E_NOCRYPTO)))
    {
        up_version_card(body, NULL, C_SUB);
        up_why_words(&(struct sl_ota_status){ .why = s->key ? s->why : SL_OTA_E_NOKEY }, t, sizeof t, l, sizeof l);
        sl_empty(body, IC_LOCK, 80, t, l, SL_INSET);
    }
    else switch (s->state)
    {
        case SL_OTA_FOUND:
        {
            up_version_card(body, NULL, C_SUB);
            lv_obj_t *g = up_card(body, s->file[0] ? "Found on the card" : "Sent from the phone remote");
            snprintf(b, sizeof b, "GabagoolOS %s", s->version);
            up_label(g, b, &ss_semibold_23, C_TEXT, false);
            up_mb(a, sizeof a, s->size);
            if (s->file[0]) snprintf(l, sizeof l, "%s \xc2\xb7 %s", a, s->file);
            else snprintf(l, sizeof l, "%s", a);
            up_label(g, l, &ss_regular_16, C_SUB, false);
            up_pill(g, "Install", true, up_install_cb);
            up_notes(body, s->notes);
            if (s->boot_menu) opt_note(body, "It also brings a new boot menu, which is not installed from here. Update the boot menu with HiBy's updater when the notes say so.");
            break;
        }
        case SL_OTA_WORKING:
        case SL_OTA_RECEIVING:
        {
            up_version_card(body, NULL, C_SUB);
            lv_obj_t *g = up_card(body, NULL);
            up_title = up_label(g, s->state == SL_OTA_RECEIVING ? "Receiving from the phone remote" : "Checking the update",
                                &ss_semibold_21, C_TEXT, false);
            up_bar_make(g, 0);
            up_line = up_label(g, "", &ss_regular_16, C_SUB, false);
            if (s->state == SL_OTA_WORKING) up_pill(g, "Cancel", false, up_cancel_cb);
            up_progress();
            opt_note(body, "Nothing on the player changes until you restart. You can keep listening.");
            break;
        }
        case SL_OTA_READY:
        {
            up_version_card(body, NULL, C_SUB);
            lv_obj_t *g = up_card(body, NULL);
            lv_obj_t *row = box(g);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, SL_SP_S, 0);
            lv_obj_set_style_pad_bottom(row, sl_line_h(&ss_semibold_21) - lv_font_get_line_height(&ss_semibold_21), 0);   /* Arabic descenders */
            lv_obj_set_style_base_dir(row, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
            text(row, LV_SYMBOL_OK, &sl_sym_22, C_ACCENT_TXT);
            lv_obj_t *v = text(row, "Verified", &ss_semibold_21, C_ACCENT_TXT);
            lv_obj_set_flex_grow(v, 1);
            set_align(v);
            sl_label_deep(v);                 /* Arabic's marks below stay in the row */
            snprintf(b, sizeof b, sl_tr("GabagoolOS %s is checked and ready."), s->version);
            up_label(g, b, &ss_semibold_23, C_TEXT, true);
            sl_count(l, sizeof l, s->changed, "%s file changes. Your settings and music stay.", "%s files change. Your settings and music stay.");
            up_label(g, l, &ss_regular_16, C_SUB, true);
            up_pill(g, "Restart to update", true, up_restart_cb);
            if (!up_battery_ok()) up_label(g, "Charge to 30% first, or plug in a charger.", &ss_regular_16, C_ACCENT_TXT, true);
            up_notes(body, s->notes);
            opt_note(body, "If the new version doesn't start, hold Volume Down while the player starts, then Tools > Run script > GabagoolOS - undo update.sh.");
            break;
        }
        case SL_OTA_REJECTED:
        {
            up_version_card(body, NULL, C_SUB);
            up_why_words(s, t, sizeof t, l, sizeof l);
            lv_obj_t *c = sl_empty(body, IC_G_WARNING, 80, t, l, SL_INSET);
            sl_empty_action(c, "Look again", up_again_cb, NULL);
            if (s->file[0]) { snprintf(b, sizeof b, "%s: %s", sl_tr("File"), s->file); opt_note(body, b); }
            break;
        }
        case SL_OTA_LOOKING:
            up_version_card(body, "Looking for an update\xe2\x80\xa6", C_SUB);
            break;
        default:
        {
            if (s->trial || s->notes[0])
            {
                snprintf(b, sizeof b, sl_tr("Updated to GabagoolOS %s"), SL_VERSION);
                up_version_card(body, b, C_ACCENT_TXT);
                up_notes(body, s->notes);
                break;
            }
            up_version_card(body, NULL, C_SUB);
            lv_obj_t *c = sl_empty(body, IC_G_DOWNLOAD, 80, "No update on the card",
                                   "Copy the .gbupdate file to the top of the card, or send it from the phone remote (Wi-Fi > Phone remote).", SL_INSET);
            sl_empty_action(c, "Look again", up_again_cb, NULL);
            break;
        }
    }
    if (s->prev && s->state != SL_OTA_WORKING)
    {
        lv_obj_t *g = set_group(body, "Previous version");
        snprintf(b, sizeof b, sl_tr("Go back to %s"), s->prev_version);
        set_action(g, LV_SYMBOL_LOOP, 0x66738a, b, NULL, up_revert_cb, NULL);
    }
    opt_note(body, "Updates only replace GabagoolOS's own program files. Your settings, music, playlists, saves and covers are never touched.");
}
static int up_shape_of(const struct sl_ota_status *s)
{
    return s->key << 12 | s->prev << 11 | (s->trial || s->notes[0]) << 10 | s->state << 5 | s->why;
}
static void up_page_deleted(lv_event_t *e) { (void)e; up_page_obj = up_box = up_bar = up_line = up_title = NULL; }
static void update_page(void)
{
    lv_obj_t *p, *body = page_new("Software update", &p);
    up_page_obj = p;
    lv_obj_add_event_cb(p, up_page_deleted, LV_EVENT_DELETE, NULL);
    up_box = box(body);
    lv_obj_set_size(up_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(up_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(up_box, SL_SP_S, 0);   /* the last note's Arabic marks stay inside */
    sl_ota_lang(up_lang_code());
    sl_ota_status(&up_st);
    if (up_st.state != SL_OTA_WORKING && up_st.state != SL_OTA_RECEIVING && up_st.state != SL_OTA_READY) sl_ota_scan();
    sl_ota_status(&up_st);
    up_seq = up_st.seq;
    up_shape = up_shape_of(&up_st);
    up_fill();
    page_push(p);
}
static const char *set_v_update(void)
{
    if (up_st.state == SL_OTA_FOUND || up_st.state == SL_OTA_READY) return "Update ready";
    return SL_VERSION;
}

/* ------------------------------------------------------------ with the UI tick */
static void up_poll(void)
{
    if (!up_since_ms) up_since_ms = sl_ms() | 1;
    struct sl_ota_status s;
    sl_ota_status(&s);
    /* a start that found a swap half done: once home is up, the right build */
    if (s.restart && !up_restarting && sl_ms() - up_since_ms > 2500)
    {
        up_restarting = true;
        toast("Finishing the update\xe2\x80\xa6");
        settings_save();
        sl_log_flush();
        system_reboot();
    }
    if (s.trial && sl_ms() - up_since_ms > SL_OTA_HEALTHY_MS) sl_ota_healthy();
    if (s.updated_to[0])
    {
        char b[160];
        snprintf(b, sizeof b, sl_tr("Updated to GabagoolOS %s. See what's new in Settings > Software update."), s.updated_to);
        notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_UPDATE, "update", b);
        sl_ota_seen();
    }
    /* a new update found: said once, quietly */
    if ((s.state == SL_OTA_FOUND || s.state == SL_OTA_READY) && strcmp(up_noticed, s.version))
    {
        snprintf(up_noticed, sizeof up_noticed, "%s", s.version);
        if (s.state == SL_OTA_FOUND && !up_page_obj)   /* on the page it shows itself */
            notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_UPDATE, "update", "An update is ready to install. Open Settings > Software update.");
    }
    if (s.seq == up_seq) return;
    up_seq = s.seq;
    up_st = s;
    if (!up_box) return;
    int shape = up_shape_of(&s);
    if (shape != up_shape) { up_shape = shape; up_fill(); }
    else up_progress();
}
/* at start: the card's top looked at once things have settled */
static void up_boot_scan(lv_timer_t *t)
{
    lv_timer_delete(t);
    sl_ota_lang(up_lang_code());
    sl_ota_scan();
}
