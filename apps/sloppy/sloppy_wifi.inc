/* Wi-Fi in GabagoolOS, and what it brings: music transfer from a computer,
 * the clock from the internet, AirPlay and DLNA. The radio work happens in
 * sloppy_net.c on a worker thread, one job at a time, as for Bluetooth. */

#define WF_NAME "GabagoolOS R1"             /* how phones and computers see the player */
static void rm_forget_cb(lv_event_t *e);  /* sloppy_remote.inc */

enum { WF_JOB_NONE, WF_JOB_STATUS, WF_JOB_POWER, WF_JOB_SCAN, WF_JOB_CONNECT, WF_JOB_FORGET,
       WF_JOB_NTP, WF_JOB_SHARE, WF_JOB_AIRPLAY, WF_JOB_DLNA, WF_JOB_GET, WF_JOB_LOCALSEND };
struct wf_result {
    int job;
    bool ok;
    char error[72];
    struct sl_wifi_status st;
    struct sl_wifi_ap aps[SL_WIFI_MAX];
    int count;
};
static struct wf_result wf_res;
static bool wf_done, wf_busy, wf_scan_requested;
static int wf_job;
static bool wf_arg_on;
static char wf_arg_ssid[33], wf_arg_psk[64];
static bool wf_arg_has_psk, wf_arg_hidden;
/* a plain download: into a buffer its asker holds, handed back when done */
static char wf_get_url[1024], *wf_get_buf;
static size_t wf_get_cap;
static int wf_get_len;
static void (*wf_get_done)(bool ok, const char *text, int len);
/* a join asked for while another job runs; it starts when that one ends */
static char wf_join_ssid[33], wf_join_psk[64];
static bool wf_join_pending, wf_join_has_psk, wf_join_hidden;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
static pthread_mutex_t wf_lock = PTHREAD_MUTEX_INITIALIZER;
#define WF_LOCK() pthread_mutex_lock(&wf_lock)
#define WF_UNLOCK() pthread_mutex_unlock(&wf_lock)
#else
#define WF_LOCK() ((void)0)                /* the worker runs in place */
#define WF_UNLOCK() ((void)0)
#endif
static lv_timer_t *wf_job_timer, *wf_tick_timer;

static struct sl_wifi_status wf_st;
static struct sl_wifi_ap wf_aps[SL_WIFI_MAX];
static int wf_count;
static bool wf_scanned, wf_ntp_done, wf_share_on, wf_airplay_on, wf_dlna_on;
static bool wf_resting;                 /* radio off while unused, see wf_rest_tick */
static uint32_t wf_next_status, wf_ntp_at, wf_power_retry_at, wf_ls_retry_at;
static unsigned wf_power_failures;
static lv_obj_t *wf_page_obj, *wf_body, *wf_share_page_obj, *wf_share_label;
static void wf_rebuild(void);
static void wf_tick_update(void);
static bool wf_begin(int job);
static void wf_join_next(void);
/* sloppy_receive.inc: the transfer page's insides, and LocalSend */
static bool rx_ls_up;
static char rx_alias[64];                 /* LocalSend's name for us, copied for the worker */
static bool rx_ls_wanted(void);
static void rx_page_build(lv_obj_t *p, lv_obj_t *body);
static void rx_poll(void);

static void *wf_worker(void *unused)
{
    (void)unused;
    struct wf_result r;
    memset(&r, 0, sizeof r);
    r.job = wf_job;
    switch (r.job)
    {
        case WF_JOB_POWER:
            if (!wf_arg_on) { sl_net_airplay_stop(); sl_net_dlna_stop(); sl_share_web_stop(); sl_net_share_stop(); sl_share_ls_stop(); }
            r.ok = sl_net_wifi_power(wf_arg_on);
            break;
        case WF_JOB_SCAN: r.count = sl_net_wifi_scan(r.aps, SL_WIFI_MAX); r.ok = r.count >= 0; break;
        case WF_JOB_CONNECT: r.ok = sl_net_wifi_connect_ex(wf_arg_ssid, wf_arg_has_psk ? wf_arg_psk : NULL, wf_arg_hidden); break;
        case WF_JOB_FORGET: r.ok = sl_net_wifi_forget(wf_arg_ssid); break;
        case WF_JOB_NTP: r.ok = sl_net_ntp(); break;
        case WF_JOB_GET:
            wf_get_len = sl_net_get(wf_get_url, wf_get_buf, wf_get_cap);
            r.ok = wf_get_len > 0;
            break;
        case WF_JOB_SHARE:
            /* ours, with its progress; HiBy's page only if ours cannot have the port */
            if (wf_arg_on)
            {
                r.ok = sl_share_web_start();
                if (!r.ok) { sl_net_share_stop(); r.ok = sl_share_web_start(); }
                if (!r.ok) r.ok = sl_net_share_start();
            }
            else { sl_share_web_stop(); sl_net_share_stop(); r.ok = true; }
            break;
        case WF_JOB_LOCALSEND:
            if (wf_arg_on) r.ok = sl_share_ls_start(rx_alias); else { sl_share_ls_stop(); r.ok = true; }
            break;
        case WF_JOB_AIRPLAY:
            if (wf_arg_on) r.ok = sl_net_airplay_start(WF_NAME); else { sl_net_airplay_stop(); r.ok = true; }
            break;
        case WF_JOB_DLNA:
            if (wf_arg_on) r.ok = sl_net_dlna_start(WF_NAME); else { sl_net_dlna_stop(); r.ok = true; }
            break;
        default: r.ok = true; break;
    }
    if (!r.ok && (r.job == WF_JOB_POWER || r.job == WF_JOB_SCAN || r.job == WF_JOB_CONNECT))
        snprintf(r.error, sizeof r.error, "%s", sl_net_wifi_last_error());
    memset(wf_arg_psk, 0, sizeof wf_arg_psk);
    sl_net_wifi_status(&r.st);
    WF_LOCK();
    wf_res = r;
    wf_done = true;
    WF_UNLOCK();
    return NULL;
}

static void wf_job_cb(lv_timer_t *timer)
{
    WF_LOCK();
    bool done = wf_done;
    static struct wf_result r;
    if (done) { r = wf_res; wf_done = false; }
    WF_UNLOCK();
    if (!done) return;
    lv_timer_pause(timer);
    bool was_connected = wf_st.connected;
    bool changed = memcmp(&wf_st, &r.st, sizeof wf_st) != 0;   /* the status structs are zeroed first */
    wf_st = r.st;
    sl_net_scrobble_online(wf_st.connected);
    sl_libjob_online(wf_st.connected);
    sl_pod_online(wf_st.connected);
    sl_sub_online(wf_st.connected);
    sl_abs_online(wf_st.connected);
    if (wf_st.connected && !was_connected) lyr_online();   /* lyrics that were waiting for it */
    wf_busy = false;
    wf_job = WF_JOB_NONE;
    const char *msg = NULL;
    switch (r.job)
    {
        case WF_JOB_POWER:
            if (!wf_arg_on) wf_airplay_on = wf_dlna_on = wf_share_on = rx_ls_up = false;
            if (wf_arg_on && !r.ok && sl_net_wifi_radio_missing()) {
                /* An older boot menu detached the radio, and only a boot
                 * through a newer one brings it back: retrying cannot help. */
                wf_power_failures = 0;
                wf_power_retry_at = 0;
                msg = "Wi-Fi needs the latest boot menu update";
            } else if (wf_arg_on && !r.ok) {
                /* Keep the saved request on. A failed boot must not silently
                 * disable Wi-Fi on every later boot. Retry on the worker. */
                wf_power_failures++;
                /* a radio that keeps failing is retried less and less:
                 * each try runs the stock scripts and waits on the chip */
                wf_power_retry_at = sl_ms() + (wf_power_failures == 1 ? 5000 :
                                               wf_power_failures == 2 ? 15000 :
                                               wf_power_failures <= 4 ? 60000 : 600000);
            } else { wf_power_failures = 0; wf_power_retry_at = 0; }
            if (!wf_arg_on && !r.ok) {
                global_settings.sloppy_wifi = r.st.on;
                settings_save();
            }
            if (!global_settings.sloppy_wifi) { wf_airplay_on = wf_dlna_on = wf_share_on = rx_ls_up = false; wf_count = 0; wf_scanned = false; wf_scan_requested = false; }
            if (!r.ok && !wf_arg_on && wf_resting) wf_resting = false;   /* stays on; no fuss */
            else if (!r.ok && !wf_arg_on) msg = "Wi-Fi did not turn off";
            else if (!r.ok && wf_power_failures == 1) msg = "Wi-Fi did not start. Retrying automatically.";
            break;
        case WF_JOB_SCAN:
            if (r.ok) {
                wf_count = r.count;
                memcpy(wf_aps, r.aps, sizeof wf_aps);
                wf_scanned = true;
                if (!r.count) msg = "No networks found";
            } else msg = "Could not scan. Please retry.";
            break;
        case WF_JOB_CONNECT:
            /* the reason is one of sl_net_wifi_connect's, in the lang table */
            msg = r.ok ? "Connected" : r.error[0] ? r.error : "Could not join the network";
            break;
        case WF_JOB_FORGET:
            msg = r.ok ? "Network forgotten" : "Could not forget network";
            for (int i = 0; r.ok && i < wf_count; i++)
                if (!strcmp(wf_aps[i].ssid, wf_arg_ssid)) wf_aps[i].saved = false;
            break;
        case WF_JOB_GET:
        {
            void (*done)(bool, const char *, int) = wf_get_done;
            char *buf = wf_get_buf;
            wf_get_done = NULL;
            wf_get_buf = NULL;
            if (done) done(r.ok, buf, r.ok ? wf_get_len : 0);
            free(buf);
            break;
        }
        case WF_JOB_NTP:
            wf_ntp_done = r.ok;
            wf_ntp_at = sl_ms() + 5 * 60 * 1000;   /* no answer: try again in 5 minutes */
            if (r.ok) { sl_log("clock set from the internet"); statusbar_update(); }
            break;
        case WF_JOB_SHARE:
            wf_share_on = r.ok && wf_arg_on;   /* what came over joins the library as each transfer ends (rx_poll) */
            if (wf_arg_on && !r.ok) msg = "Could not start music transfer";
            break;
        case WF_JOB_LOCALSEND:
            rx_ls_up = r.ok && wf_arg_on;
            if (wf_arg_on && !r.ok)
            {
                msg = "LocalSend did not start";
                wf_ls_retry_at = sl_ms() + 30000;   /* a port that will not open is tried again later, not every second */
            }
            break;
        case WF_JOB_AIRPLAY:
            wf_airplay_on = r.ok && wf_arg_on;
            if (wf_arg_on && !r.ok) {
                global_settings.sloppy_airplay = 0;
                settings_save();
                msg = "AirPlay did not start";
            }
            break;
        case WF_JOB_DLNA:
            wf_dlna_on = r.ok && wf_arg_on;
            if (wf_arg_on && !r.ok) {
                global_settings.sloppy_dlna = 0;
                settings_save();
                msg = "DLNA did not start";
            }
            break;
    }
    if (!wf_st.connected && was_connected) { wf_ntp_done = false; wf_ntp_at = 0; }
    if (!r.ok && r.error[0]) sl_log("wifi job %d failed: %s", r.job, r.error);
    if (!r.ok && r.job == WF_JOB_POWER && sl_net_wifi_detail()[0])
    {
        /* one line per kernel line, so the log stays readable */
        char d[320];
        strlcpy(d, sl_net_wifi_detail(), sizeof d);
        for (char *line = strtok(d, "\n"); line; line = strtok(NULL, "\n"))
            sl_log("wifi:   %s", line);
    }
    /* fixed words, or the network layer's error: nothing that names a network */
    if (msg) notify_ex(NS_WIFI, r.ok ? NOTE_INFO : NOTE_WARN, 0, NA_WIFI, NULL, msg);
    /* background checks redraw the page only when they found something new */
    bool joining = wf_join_pending;
    if (joining && !wf_st.on) {
        wf_join_pending = false;
        memset(wf_join_psk, 0, sizeof wf_join_psk);
        toast("Turn on Wi-Fi first");
    }
    wf_join_next();                       /* a queued join goes before a scan */
    if (wf_page_obj && (changed || joining || (r.job != WF_JOB_STATUS && r.job != WF_JOB_NTP &&
                                               r.job != WF_JOB_GET))) wf_rebuild();
    wf_tick_update();
    if (wf_scan_requested && wf_st.on && !wf_busy) {
        wf_scan_requested = false;
        wf_begin(WF_JOB_SCAN);
    }
}

static bool wf_online(void) { return wf_st.connected; }
/* Fetch url on the worker; done gets its text on this thread, then it is freed */
static bool wf_fetch(const char *url, size_t cap, void (*done)(bool ok, const char *text, int len))
{
    if (wf_busy) return false;
    wf_get_buf = malloc(cap);
    if (!wf_get_buf) return false;
    strlcpy(wf_get_url, url, sizeof wf_get_url);
    wf_get_cap = cap;
    wf_get_done = done;
    if (wf_begin(WF_JOB_GET)) return true;
    free(wf_get_buf);
    wf_get_buf = NULL;
    wf_get_done = NULL;
    return false;
}
static bool wf_begin(int job)
{
    if (wf_busy || sl_net_wireless_unsupported()) return false;   /* an AIC8800 unit: no radio to ask */
    wf_busy = true;
    wf_job = job;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_t t;
    if (pthread_create(&t, NULL, wf_worker, NULL)) { wf_busy = false; wf_job = WF_JOB_NONE; return false; }
    pthread_detach(t);
#else
    wf_worker(NULL);                      /* the pretend radio answers at once */
#endif
    if (!wf_job_timer) wf_job_timer = lv_timer_create(wf_job_cb, 100, NULL);
    lv_timer_reset(wf_job_timer);
    lv_timer_resume(wf_job_timer);
    return true;
}

/* ------------------------------------------------------------ AirPlay audio */
/* The stream is 44.1 kHz; the mixer runs at whatever playback last set. */
#define AP_OUT 1024
static int16_t ap_out[AP_OUT * 2], ap_in[AP_OUT * 2 * 5];
static int16_t ap_prev[2], ap_next[2];
static uint32_t ap_pos, ap_step;
static bool ap_active, ap_held_off;

static void ap_get_more(const void **start, size_t *size)
{
    uint32_t pops = (uint32_t)(((uint64_t)ap_pos + (uint64_t)ap_step * (AP_OUT - 1)) >> 16);
    if (pops > AP_OUT * 5) pops = AP_OUT * 5;
    size_t got = sl_net_airplay_read(ap_in, pops), used = 0;
    for (int i = 0; i < AP_OUT; i++)
    {
        while (ap_pos >= 65536)
        {
            ap_prev[0] = ap_next[0];
            ap_prev[1] = ap_next[1];
            if (used < got) { ap_next[0] = ap_in[used * 2]; ap_next[1] = ap_in[used * 2 + 1]; used++; }
            else ap_next[0] = ap_next[1] = 0;            /* ran dry: silence */
            ap_pos -= 65536;
        }
        for (int c = 0; c < 2; c++)
            ap_out[i * 2 + c] = (int16_t)(ap_prev[c] + (((int32_t)(ap_next[c] - ap_prev[c]) * (int32_t)ap_pos) >> 16));
        ap_pos += ap_step;
    }
    *start = ap_out;
    *size = sizeof ap_out;
}

static void ap_poll(void)
{
    if (!wf_airplay_on) { if (ap_active) { mixer_channel_stop(PCM_MIXER_CHAN_VOICE); ap_active = false; } return; }
    uint32_t idle = sl_net_airplay_idle_ms();
    if (!ap_active && idle < 500 && !ap_held_off && !sl_btrx_active)   /* a phone over Bluetooth has the channel */
    {
        if (is_playing()) audio_pause();          /* the phone takes over */
        uint32_t fs = mixer_get_frequency();
        if (fs < 8000) fs = 44100;
        ap_step = (uint32_t)((44100ull << 16) / fs);
        ap_pos = 65536;
        ap_prev[0] = ap_prev[1] = ap_next[0] = ap_next[1] = 0;
        const void *start;
        size_t size;
        ap_get_more(&start, &size);
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, MIX_AMP_UNITY);
        mixer_channel_play_data(PCM_MIXER_CHAN_VOICE, ap_get_more, start, size);
        ap_active = true;
        sl_log("airplay: stream started at %lu Hz", (unsigned long)fs);
        notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NOW_PLAYING, "cast", "Playing from AirPlay");
    }
    else if (ap_active && (idle > 3000 || is_playing()))
    {
        /* the phone stopped, or someone pressed play here */
        mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
        ap_active = false;
        ap_held_off = idle <= 3000;
        sl_log("airplay: stream ended");
    }
    if (ap_held_off && idle > 3000) ap_held_off = false;
    if (ap_active) reset_poweroff_timer();   /* playing: Auto power off waits, as it does for songs */
}

/* ------------------------------------------------------------------ DLNA */
static char dl_cast_path[128];

static bool dl_is_current(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    return id3 && dl_cast_path[0] && strstr(id3->path, dl_cast_path + 1);   /* allow a root prefix */
}

static void dl_poll(void)
{
    if (!wf_dlna_on) return;
    struct sl_dlna_cmd c;
    while (sl_net_dlna_poll(&c))
    {
        switch (c.type)
        {
            case SL_DLNA_LOAD:
                strlcpy(dl_cast_path, c.path, sizeof dl_cast_path);
                if (sl_new_list(NULL) < 0)
                { toast_warn("Could not play what was sent"); break; }
                if (playlist_insert_track(NULL, c.path, PLAYLIST_INSERT_LAST, false, true) < 0)
                { sl_abort_list(); toast_warn("Could not play what was sent"); break; }
                playlist_sync(NULL);
                sl_start_list(0);
                if (!c.value) audio_pause();
                sl_log("dlna: playing %s", c.path);
                notify_ex(NS_MUSIC, NOTE_INFO, NOTE_PRIVATE, NA_NOW_PLAYING, "cast",   /* a song sent from elsewhere: not kept on the card */
                          c.title[0] ? c.title : "Playing from DLNA");
                break;
            case SL_DLNA_PLAY: if (dl_is_current() && (audio_status() & AUDIO_STATUS_PAUSE)) audio_resume(); break;
            case SL_DLNA_PAUSE: if (dl_is_current() && is_playing()) audio_pause(); break;
            case SL_DLNA_STOP: if (dl_is_current()) audio_stop(); break;
            case SL_DLNA_SEEK:
                if (dl_is_current())
                {
                    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
                    audio_ff_rewind(c.value);
                }
                break;
            case SL_DLNA_VOLUME:
                global_status.volume = vol_from_pct((int)c.value);
                setvol();
                statusbar_update();
                break;
        }
    }
    struct sl_dlna_state st = { 0, 0, 0, volume_pct() };
    if (dl_is_current())
    {
        struct mp3entry *id3 = audio_current_track();
        st.playing = (audio_status() & AUDIO_STATUS_PAUSE) ? 2 : 1;
        st.elapsed_ms = (long)visible_elapsed(id3);
        st.length_ms = (long)id3->length;
    }
    sl_net_dlna_report(&st);
}

/* ------------------------------------------------------------- resting */
/* The 2.4 GHz radio is one of the bigger draws on a 1150 mAh battery. With
 * the screen dark for a while and nothing using the network (AirPlay,
 * DLNA and LocalSend stay reachable while they are on), the radio powers down; it comes
 * back when the screen does. The Wi-Fi setting itself never changes. */
#define WF_REST_AFTER_MS (3u * 60u * 1000u)
static uint32_t wf_dark_since;
static bool dl_is_current(void);
static bool sleep_streams_on(void);
/* true: the tick has nothing else to do */
static bool wf_rest_tick(void)
{
    static bool was_dark;
    uint32_t now = sl_ms();
    if (!global_settings.sloppy_wifi) { wf_resting = was_dark = false; return false; }
    if (wallpaper_screen_dark)
    {
        if (!was_dark) { was_dark = true; wf_dark_since = now; }
        bool idle = wf_st.on && !wf_busy && !wf_airplay_on && !wf_dlna_on && !wf_share_on && !rx_ls_up &&
                    !ap_active && !dl_is_current() &&
                    /* internet radio stopped three minutes into a dark screen, and a
                     * podcast, server or book download was cut the same way */
                    !sleep_streams_on() && !sl_pod_busy() && !sl_sub_busy() && !sl_abs_busy() && !sl_ota_busy();
        if (idle && !wf_resting && now - wf_dark_since >= WF_REST_AFTER_MS)
        {
            wf_arg_on = false;
            if (wf_begin(WF_JOB_POWER))
            {
                wf_resting = true;
                sl_log("wifi: resting after %u min with the screen off", WF_REST_AFTER_MS / 60000);
            }
        }
        return true;                    /* no status checks nobody will see */
    }
    was_dark = false;
    if (wf_resting && !wf_busy)
    {
        wf_resting = false;
        wf_power_failures = 0;
        wf_arg_on = true;
        wf_begin(WF_JOB_POWER);
        sl_log("wifi: back on with the screen");
        return true;
    }
    return wf_resting;
}

/* --------------------------------------------------------------- the tick */
/* Every 250 ms while AirPlay or DLNA runs, once a second while Wi-Fi is on
 * without them, and not at all with Wi-Fi off: an idle player sleeps. */
static void wf_tick(lv_timer_t *timer)
{
    (void)timer;
    ap_poll();
    dl_poll();
    if (sl_net_wireless_unsupported()) { lv_timer_pause(wf_tick_timer); return; }
    if (!global_settings.sloppy_wifi) { wf_tick_update(); return; }   /* pauses once the audio has stopped */
    if (wf_rest_tick()) return;
    if (wf_busy) return;
    uint32_t now = sl_ms();
    if (!wf_st.on && wf_power_failures && (int32_t)(now - wf_power_retry_at) >= 0)
    {
        wf_arg_on = true;
        if (!wf_begin(WF_JOB_POWER)) wf_power_retry_at = now + 5000;
        return;
    }
    if ((int32_t)(now - wf_next_status) >= 0)
    {
        /* each check runs wpa_cli; look often only while someone is watching */
        wf_next_status = now + (wf_page_obj ? 4000 : wf_st.connected ? 30000 : 10000);
        wf_begin(WF_JOB_STATUS);
        return;
    }
    if (!wf_st.connected) return;
    if (global_settings.sloppy_auto_time && !wf_ntp_done && (int32_t)(now - wf_ntp_at) >= 0)
    { wf_arg_on = true; wf_begin(WF_JOB_NTP); return; }
    if (!!global_settings.sloppy_airplay != wf_airplay_on) { wf_arg_on = global_settings.sloppy_airplay; wf_begin(WF_JOB_AIRPLAY); return; }
    if (!!global_settings.sloppy_dlna != wf_dlna_on) { wf_arg_on = global_settings.sloppy_dlna; wf_begin(WF_JOB_DLNA); return; }
    if (wf_share_page_obj && !wf_share_on) { wf_arg_on = true; wf_begin(WF_JOB_SHARE); return; }
    if (!wf_share_page_obj && wf_share_on) { wf_arg_on = false; wf_begin(WF_JOB_SHARE); return; }
    if (rx_ls_wanted() != rx_ls_up && !(rx_ls_wanted() && (int32_t)(now - wf_ls_retry_at) < 0))
    {
        wf_arg_on = rx_ls_wanted();
        strlcpy(rx_alias, sl_bt_shown_player_name(), sizeof rx_alias);
        sl_share_allow_any(global_settings.sloppy_receive_any);
        wf_begin(WF_JOB_LOCALSEND);
        return;
    }
}

static void wf_tick_update(void)
{
    bool audio = wf_airplay_on || wf_dlna_on || ap_active;
    if (!global_settings.sloppy_wifi && !audio && !wf_busy)
    {
        if (wf_tick_timer) lv_timer_pause(wf_tick_timer);
        return;
    }
    if (!wf_tick_timer) wf_tick_timer = lv_timer_create(wf_tick, 1000, NULL);
    lv_timer_set_period(wf_tick_timer, audio ? 250 : 1000);
    lv_timer_resume(wf_tick_timer);
}
static void wf_tick_start(void) { wf_tick_update(); }

/* ---------------------------------------------------------------- the page */
static void wf_power_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    if (wf_busy) { lv_obj_set_state(lv_event_get_current_target(e), LV_STATE_CHECKED, !on); return; }
    global_settings.sloppy_wifi = on;
    settings_save();
    wf_power_failures = 0;
    wf_power_retry_at = 0;
    wf_arg_on = on;
    if (!wf_begin(WF_JOB_POWER)) {
        global_settings.sloppy_wifi = !on;
        settings_save();
        lv_obj_set_state(lv_event_get_current_target(e), LV_STATE_CHECKED, !on);
        toast_warn("Could not start Wi-Fi task");
        return;
    }
    wf_tick_start();
    if (on) wf_scan_requested = true;
}

/* the control centre shortcut */
static void qp_wifi_toggle(void)
{
    if (sl_net_wireless_unsupported()) { toast_warn("Wi-Fi isn't supported on this R1 yet"); return; }
    if (wf_busy) { toast_warn("Wi-Fi is busy. Please retry."); return; }
    global_settings.sloppy_wifi = !global_settings.sloppy_wifi;
    wf_power_failures = 0;
    wf_power_retry_at = 0;
    wf_arg_on = global_settings.sloppy_wifi;
    if (!wf_begin(WF_JOB_POWER)) {
        global_settings.sloppy_wifi = !global_settings.sloppy_wifi;
        toast_warn("Could not start Wi-Fi task");
        return;
    }
    settings_save();
    wf_tick_start();
}

static void wf_scan_cb(lv_event_t *e)
{
    (void)e;
    if (wf_busy) {
        if (wf_job == WF_JOB_POWER || wf_job == WF_JOB_STATUS) {
            wf_scan_requested = true;
            toast("Scan will start when Wi-Fi is ready");
        } else toast_warn("Wi-Fi is busy. Please retry.");
        return;
    }
    if (!wf_st.on) {
        toast(!global_settings.sloppy_wifi ? "Turn on Wi-Fi first" :
              sl_net_wifi_radio_missing() ? "Wi-Fi needs the latest boot menu update" :
              "Wi-Fi is unavailable. Toggle it off and on.");
        return;
    }
    wf_begin(WF_JOB_SCAN);
}

/* Start the queued join if the worker is free; the password moves into the
 * worker's argument only now, so a job already running never sees it. */
static void wf_join_next(void)
{
    if (!wf_join_pending || wf_busy) return;
    wf_join_pending = false;
    strlcpy(wf_arg_ssid, wf_join_ssid, sizeof wf_arg_ssid);
    wf_arg_has_psk = wf_join_has_psk;
    wf_arg_hidden = wf_join_hidden;
    memcpy(wf_arg_psk, wf_join_psk, sizeof wf_arg_psk);
    memset(wf_join_psk, 0, sizeof wf_join_psk);
    if (!wf_begin(WF_JOB_CONNECT)) {
        memset(wf_arg_psk, 0, sizeof wf_arg_psk);
        toast_warn("Could not start Wi-Fi task");
    }
}

/* the rows are rebuilt after their own click event, never during it */
static void wf_rebuild_async(void *unused) { (void)unused; wf_rebuild(); }

static void wf_password_page(const char *ssid);
static void wf_other_page(void);
static void wf_join_ex(const char *ssid, const char *psk, bool hidden)
{
    if (wf_job == WF_JOB_CONNECT || wf_join_pending) { toast("Already joining a network"); return; }
    strlcpy(wf_join_ssid, ssid, sizeof wf_join_ssid);
    wf_join_hidden = hidden;
    wf_join_has_psk = psk != NULL;
    strlcpy(wf_join_psk, psk ? psk : "", sizeof wf_join_psk);
    wf_join_pending = true;
    wf_join_next();                       /* or when the running job ends */
    lv_async_call(wf_rebuild_async, NULL);
}
static void wf_join(const char *ssid, const char *psk) { wf_join_ex(ssid, psk, false); }
static void wf_other_cb(lv_event_t *e) { (void)e; wf_other_page(); }

static void wf_ap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= wf_count) return;
    const struct sl_wifi_ap *ap = &wf_aps[i];
    if (wf_st.connected && !strcmp(wf_st.ssid, ap->ssid)) return;
    if (ap->secure && !ap->saved) wf_password_page(ap->ssid);
    else wf_join(ap->ssid, NULL);
}

static void wf_forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= wf_count) return;
    if (wf_busy || wf_join_pending) { toast_warn("Wi-Fi is busy. Please retry."); return; }
    strlcpy(wf_arg_ssid, wf_aps[i].ssid, sizeof wf_arg_ssid);
    wf_begin(WF_JOB_FORGET);
}

static void wf_share_cb(lv_event_t *e);
static void wf_airplay_set(int on) { global_settings.sloppy_airplay = on; }
static void wf_auto_time_set(int on) { global_settings.sloppy_auto_time = on; wf_ntp_done = false; }
static void wf_dlna_set(int on) { global_settings.sloppy_dlna = on; }

static void wf_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == wf_page_obj) {
        wf_page_obj = wf_body = NULL;
        wf_scan_requested = false;
    }
}

static const char *wf_bars(int dbm)
{
    return dbm > -55 ? "Excellent signal" : dbm > -67 ? "Good signal" : dbm > -77 ? "Fair signal" : "Weak signal";
}

static void wf_rebuild(void)
{
    if (!wf_page_obj) return;
    int scroll = lv_obj_get_scroll_y(wf_body);
    lv_obj_clean(wf_body);
    if (sl_net_wireless_unsupported()) { sl_wireless_gone(wf_body, IC_WIFI, "Wi-Fi isn't supported on this R1 yet"); return; }

    char line[96];
    const char *state;
    if (wf_job == WF_JOB_POWER) state = global_settings.sloppy_wifi ? "Turning on..." : "Turning off...";
    else if (wf_job == WF_JOB_CONNECT || wf_join_pending)
    {
        snprintf(line, sizeof line, "%s %s", sl_tr("Joining"), wf_join_pending ? wf_join_ssid : wf_arg_ssid);
        state = line;
    }
    else if (wf_st.connected) { snprintf(line, sizeof line, "%s · %s", wf_st.ssid, wf_st.ip); state = line; }
    else if (global_settings.sloppy_wifi && !wf_st.on) state = "Wi-Fi is unavailable";
    else if (global_settings.sloppy_wifi) state = "Not connected";
    else state = "Off";
    /* the switch, and what Wi-Fi is doing, in Settings' own rows */
    lv_obj_t *g = set_group(wf_body, NULL);
    lv_obj_t *power = set_switch_ex(g, LV_SYMBOL_WIFI, 0x0aa9c7, "Wi-Fi", state, global_settings.sloppy_wifi, NULL);
    label_lines(lv_obj_get_child(lv_obj_get_parent(power), 1), 1);
    lv_obj_add_event_cb(power, wf_power_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (!global_settings.sloppy_wifi)
        opt_note(wf_body, "Wi-Fi brings internet radio, AirPlay, the phone remote, scrobbling and music transfer.");
    else
    {
        g = set_group(wf_body, "NETWORKS");
        set_action(g, LV_SYMBOL_REFRESH, 0x0aa9c7, wf_job == WF_JOB_SCAN ? "Looking for networks..." : "Scan for networks",
                   NULL, wf_scan_cb, NULL);
        for (int i = 0; i < wf_count; i++)
        {
            bool here = wf_st.connected && !strcmp(wf_st.ssid, wf_aps[i].ssid);
            char sub[80];
            snprintf(sub, sizeof sub, "%s · %s", here ? sl_tr("Connected") : wf_aps[i].saved ? sl_tr("Saved") :
                     wf_aps[i].secure ? sl_tr("Password") : sl_tr("Open"), sl_tr(wf_bars(wf_aps[i].signal)));
            lv_obj_t *row = set_action_sub(g, LV_SYMBOL_WIFI,
                                           here ? C_ACCENT_UI : wf_aps[i].saved ? 0x0aa9c7 : 0x66738a,
                                           wf_aps[i].ssid, sub, wf_ap_cb, (void *)(intptr_t)i);
            if (here) lv_obj_set_style_text_color(lv_obj_get_child(row, 1), lv_color_hex(C_ACCENT_TXT), 0);
            if (wf_aps[i].saved) set_row_button(row, 3, LV_SYMBOL_CLOSE, wf_forget_cb, (void *)(intptr_t)i);
        }
        /* a network the scan cannot see, or one out of the list: by its name */
        set_link(g, LV_SYMBOL_PLUS, 0x66738a, "Other network", NULL, wf_other_cb, NULL);

        lv_obj_t *c = opt_card(wf_body, "WIRELESS MUSIC", "Shows up as " WF_NAME ".");
        opt_switch(c, "AirPlay", global_settings.sloppy_airplay, wf_airplay_set);
        opt_switch(c, "DLNA", global_settings.sloppy_dlna, wf_dlna_set);
        /* the phone remote: a web page, so any phone works and nothing is installed */
        char note[96];
        snprintf(note, sizeof note, "%s http://%s:%d", sl_tr("On a phone on this Wi-Fi, open"), wf_st.ip, SL_REMOTE_PORT);
        lv_obj_t *rc = opt_card(wf_body, "Phone remote", note);
        opt_switch(rc, "Remote", sl_net_remote_on(), rm_set);
        {   /* the code to scan, while it is on */
            char url[48];
            snprintf(url, sizeof url, "http://%s:%d", wf_st.ip, SL_REMOTE_PORT);
            rm_code_row(rc, url);
        }
        /* the phones that typed the player's code; forgotten, each must pair again */
        set_action(rc, LV_SYMBOL_CLOSE, 0x66738a, "Forget paired phones", NULL, rm_forget_cb, NULL);
        g = set_group(wf_body, "FILES");
        set_link(g, LV_SYMBOL_UPLOAD, 0x3aa856, "Transfer music", NULL, wf_share_cb, NULL);
    }
    lv_obj_update_layout(wf_body);
    lv_obj_scroll_to_y(wf_body, scroll, LV_ANIM_OFF);
}

static void sl_wifi_page(void)
{
    if (page_bring_back(wf_page_obj)) return;   /* once on the stack, as Bluetooth */
    lv_obj_t *p, *body = page_new("Wi-Fi", &p);
    wf_page_obj = p;
    wf_body = body;
    lv_obj_add_event_cb(p, wf_deleted, LV_EVENT_DELETE, NULL);
    wf_rebuild();
    page_push(p);
    wf_tick_start();
    wf_next_status = sl_ms();
    /* always look again: the last list may be from somewhere else; it
     * stays on screen until the new one is in */
    if (global_settings.sloppy_wifi) {
        if (!wf_busy && wf_st.on) wf_begin(WF_JOB_SCAN);
        else wf_scan_requested = true;
    }
}

/* ------------------------------------------------------------ the password */
static lv_obj_t *wf_pw_ta;
static char wf_pw_ssid[33];

static void wf_pw_join_cb(lv_event_t *e)
{
    (void)e;
    const char *psk = lv_textarea_get_text(wf_pw_ta);
    if (strlen(psk) < 8) { toast("Wi-Fi passwords have at least 8 characters"); return; }
    wf_join(wf_pw_ssid, psk);
    page_pop();
}
static void wf_pw_show_cb(lv_event_t *e)
{
    bool show = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(wf_pw_ta, !show);
}

/* The keyboard (sloppy_kb.inc) for any one-line field: typing goes to
 * ta, the wide key at the bottom right says enter_label and runs enter.
 * The pet's name page uses it too. */
static struct kb *kb_of_page(lv_obj_t *p);
static void wf_kb_build(lv_obj_t *p, lv_obj_t *ta, int kind, lv_event_cb_t enter, const char *enter_label)
{
    kb_attach(p, ta, kind, enter, enter_label);
}
/* A page typing on the keyboard: its keys run to the bottom edge, where
 * the mini player would float, so the mini stays away while it is on top */
static bool kb_on_top(void)
{
    return npages && kb_of_page(pages[npages - 1]) != NULL;
}
/* numbers to type (an EQ band's values): the keyboard opens on its digits */
/* the page on top's keyboard, or the one just built (its page not pushed yet) */
static struct kb *wf_kb_now(void)
{
    struct kb *kb = npages ? kb_of_page(pages[npages - 1]) : NULL;
    return kb ? kb : kb_live;
}
static void wf_kb_digits(void) { kb_digits(wf_kb_now()); }
static void wf_kb_target(lv_obj_t *ta) { kb_retarget(wf_kb_now(), ta); }

static void wf_password_page(const char *ssid)
{
    strlcpy(wf_pw_ssid, ssid, sizeof wf_pw_ssid);
    lv_obj_t *p, *body = page_new(ssid, &p);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *g = set_group(body, "PASSWORD");
    wf_pw_ta = set_field(g);
    lv_textarea_set_password_mode(wf_pw_ta, true);
    lv_textarea_set_max_length(wf_pw_ta, 63);
    lv_obj_t *show = opt_switch(g, "Show password", false, NULL);
    lv_obj_add_event_cb(show, wf_pw_show_cb, LV_EVENT_VALUE_CHANGED, NULL);
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_WIFI, 0x0aa9c7, "Join network", NULL, wf_pw_join_cb, NULL);
    wf_kb_build(p, wf_pw_ta, KB_PASSWORD, wf_pw_join_cb, "Join");
    page_push(p);
}

/* ------------------------------------------------- joining by hand (#80) */
/* A network the scan does not list (a hidden one, or one just out of
 * range): its name, whether it has a password, the password. The
 * supplicant asks for it by name (scan_ssid), which is what a hidden
 * network answers. The keyboard types into whichever field was touched. */
static lv_obj_t *wf_ot_page_obj, *wf_ot_name, *wf_ot_pw, *wf_ot_pw_head, *wf_ot_pw_group;
static int wf_ot_secure = 1;
static void wf_ot_focus(lv_obj_t *ta)
{
    lv_obj_t *other = ta == wf_ot_name ? wf_ot_pw : wf_ot_name;
    if (other) lv_obj_remove_state(other, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (wf_ot_page_obj) kb_retarget(kb_of_page(wf_ot_page_obj), ta);
}
static void wf_ot_field_cb(lv_event_t *e) { wf_ot_focus(lv_event_get_current_target(e)); }
static void wf_ot_secure_set(int i)
{
    wf_ot_secure = i;
    lv_obj_update_flag(wf_ot_pw_head, LV_OBJ_FLAG_HIDDEN, !i);
    lv_obj_update_flag(wf_ot_pw_group, LV_OBJ_FLAG_HIDDEN, !i);
    wf_ot_focus(i && lv_textarea_get_text(wf_ot_name)[0] ? wf_ot_pw : wf_ot_name);
}
static void wf_ot_join_cb(lv_event_t *e)
{
    (void)e;
    char name[33];
    const char *typed = lv_textarea_get_text(wf_ot_name);
    if (!typed[0]) { toast("Type the network's name"); wf_ot_focus(wf_ot_name); return; }
    if (strlen(typed) > 32) { toast("Network names are at most 32 bytes"); return; }
    strlcpy(name, typed, sizeof name);
    const char *psk = wf_ot_secure ? lv_textarea_get_text(wf_ot_pw) : NULL;
    if (psk && strlen(psk) < 8) { toast("Wi-Fi passwords have at least 8 characters"); wf_ot_focus(wf_ot_pw); return; }
    if (!global_settings.sloppy_wifi || !wf_st.on) { toast("Turn on Wi-Fi first"); return; }
    wf_join_ex(name, psk, true);
    page_pop();
}
static void wf_ot_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != wf_ot_page_obj) return;
    wf_ot_page_obj = wf_ot_name = wf_ot_pw = wf_ot_pw_head = wf_ot_pw_group = NULL;
}
/* the eye at the field's end shows the password, and hides it again */
static void wf_ot_show_cb(lv_event_t *e)
{
    bool show = lv_textarea_get_password_mode(wf_ot_pw);
    lv_textarea_set_password_mode(wf_ot_pw, !show);
    lv_label_set_text(lv_obj_get_child(lv_event_get_current_target(e), 0), show ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    wf_ot_focus(wf_ot_pw);
}
static lv_obj_t *wf_ot_field(lv_obj_t *g, const char *hint)
{
    lv_obj_t *ta = set_field(g);
    lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    sl_ta_hint(ta, sl_tr(hint));
    /* the field's own face, so the hint sits where the words will; only its colour is quieter */
    lv_obj_set_style_text_color(ta, lv_color_hex(C_SUB), LV_PART_TEXTAREA_PLACEHOLDER);
    if (set_rtl()) lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ta, wf_ot_field_cb, LV_EVENT_PRESSED, NULL);
    return ta;
}
static void wf_other_page(void)
{
    if (sl_net_wireless_unsupported() || page_bring_back(wf_ot_page_obj)) return;
    lv_obj_t *p, *body = page_new("Other network", &p);
    wf_ot_page_obj = p;
    lv_obj_add_event_cb(p, wf_ot_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_bottom(body, WF_KB_H + SL_GUTTER, 0);   /* everything scrolls clear of the keyboard */
    lv_obj_t *g = set_group(body, "NETWORK");
    wf_ot_name = wf_ot_field(g, "Network name");
    lv_textarea_set_max_length(wf_ot_name, 32);
    static const char *const kinds[] = { "Open", "Password" };
    opt_chips(g, kinds, 2, wf_ot_secure, wf_ot_secure_set);
    wf_ot_pw_head = set_header(body, "PASSWORD");
    wf_ot_pw_group = g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, 0, 0);
    wf_ot_pw = wf_ot_field(g, "At least 8 characters");
    lv_textarea_set_password_mode(wf_ot_pw, true);
    lv_textarea_set_max_length(wf_ot_pw, 63);
    /* Show password as the field's own eye, not a row: the Join row stays above the keyboard */
    lv_obj_set_style_pad_right(wf_ot_pw, set_rtl() ? SL_INSET : SL_GUTTER + SL_TAP, 0);
    lv_obj_set_style_pad_left(wf_ot_pw, set_rtl() ? SL_GUTTER + SL_TAP : SL_INSET, 0);
    lv_obj_t *eye = box(wf_ot_pw);
    lv_obj_add_flag(eye, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(eye, SL_TAP - 2, SL_TAP - 2);
    lv_obj_align(eye, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? -SL_TAP : SL_TAP, 0);   /* SL_GUTTER from the card's edge, as a row's own button */
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye, lv_color_mix(lv_color_hex(C_TEXT), lv_color_hex(C_CARD_P), 40), LV_STATE_PRESSED);
    press_motion(eye);
    lv_obj_center(text(eye, LV_SYMBOL_EYE_OPEN, &sl_sym_18, C_SUB));
    on_click(eye, wf_ot_show_cb, NULL);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_EVENT_BUBBLE);
    g = set_group(body, NULL);
    set_action(g, LV_SYMBOL_WIFI, 0x0aa9c7, "Join network", NULL, wf_ot_join_cb, NULL);
    wf_kb_build(p, wf_ot_name, KB_PASSWORD, wf_ot_join_cb, "Join");
    wf_ot_secure_set(wf_ot_secure);
    wf_ot_focus(wf_ot_name);
    page_push(p);
}

/* ---------------------------------------------------------- music transfer */
/* the control centre's Transfer tile: lit while the page serves, with its address */
static bool wf_transfer_on(const char **ip)
{
    if (ip) *ip = wf_st.ip;
    return wf_share_on && wf_share_page_obj && wf_st.connected;
}
static void wf_share_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) == wf_share_page_obj) wf_share_page_obj = wf_share_label = NULL;
}

static void wf_share_cb(lv_event_t *e)
{
    (void)e;
    if (page_bring_back(wf_share_page_obj)) return;
    if (sl_net_wireless_unsupported()) { toast_warn("Wi-Fi isn't supported on this R1 yet"); return; }
    if (!wf_st.connected) { toast("Join a Wi-Fi network first"); return; }
    lv_obj_t *p, *body = page_new("Transfer music", &p);
    wf_share_page_obj = p;
    lv_obj_add_event_cb(p, wf_share_deleted, LV_EVENT_DELETE, NULL);
    rx_page_build(p, body);
    page_push(p);
}

/* -------------------------------------------------------------- time zones */
static const char *const tz_zones[] = {
    "Pacific/Honolulu", "America/Anchorage", "America/Los_Angeles", "America/Phoenix", "America/Denver",
    "America/Chicago", "America/Mexico_City", "America/New_York", "America/Bogota", "America/Lima",
    "America/Caracas", "America/Halifax", "America/Santiago", "America/St_Johns", "America/Sao_Paulo",
    "America/Argentina/Buenos_Aires", "Atlantic/Azores", "UTC", "Europe/London", "Europe/Dublin",
    "Europe/Lisbon", "Africa/Lagos", "Europe/Paris", "Europe/Berlin", "Europe/Madrid", "Europe/Rome",
    "Europe/Amsterdam", "Europe/Stockholm", "Europe/Warsaw", "Africa/Johannesburg", "Africa/Cairo",
    "Asia/Jerusalem", "Europe/Athens", "Europe/Helsinki", "Europe/Kiev", "Europe/Istanbul", "Africa/Nairobi",
    "Europe/Moscow", "Asia/Dubai", "Asia/Tehran", "Asia/Karachi", "Asia/Kolkata", "Asia/Kathmandu",
    "Asia/Dhaka", "Asia/Bangkok", "Asia/Jakarta", "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Singapore",
    "Asia/Taipei", "Australia/Perth", "Asia/Seoul", "Asia/Tokyo", "Australia/Adelaide", "Australia/Brisbane",
    "Australia/Sydney", "Pacific/Auckland",
};
static lv_obj_t *tz_row;

static void tz_apply(void)
{
    const char *z = (const char *)global_settings.sloppy_timezone;
    if (z[0]) setenv("TZ", z, 1);
    else unsetenv("TZ");
    tzset();
}

/* "London (UTC+1)" for a zone, as it is right now */
static void tz_label(char *b, int n, const char *zone)
{
    const char *slash = strrchr(zone, '/');
    char city[40];
    strlcpy(city, slash ? slash + 1 : zone, sizeof city);
    for (char *c = city; *c; c++) if (*c == '_') *c = ' ';
    time_t now = time(NULL);
    setenv("TZ", zone, 1);
    tzset();
    struct tm lt = *localtime(&now);
    long off = lt.tm_gmtoff / 60;
    if (off % 60) snprintf(b, n, "%s (UTC%+ld:%02ld)", city, off / 60, labs(off % 60));
    else snprintf(b, n, "%s (UTC%+ld)", city, off / 60);
    if (!strcmp(zone, "UTC")) strlcpy(b, "UTC", n);
}

static const char *tz_row_text(void)
{
    static char b[64];
    if (!global_settings.sloppy_timezone[0]) return "As set on the player";
    tz_label(b, sizeof b, (const char *)global_settings.sloppy_timezone);
    tz_apply();
    return b;
}

static void tz_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy((char *)global_settings.sloppy_timezone, tz_zones[i], sizeof global_settings.sloppy_timezone);
    tz_apply();
    if (settings_save() != 0) toast_warn("Could not save settings");
    statusbar_update();
    if (tz_row) lv_label_set_text(lv_obj_get_child(tz_row, 1), tz_row_text());
    page_pop();
}

static void tz_row_deleted(lv_event_t *e) { (void)e; tz_row = NULL; }

static void tz_page(void)
{
    lv_obj_t *p, *body = page_new("Time zone", &p);
    char b[64];
    lv_obj_t *g = set_group(body, "City");
    for (int i = 0; i < (int)ARRAYLEN(tz_zones); i++)
    {
        tz_label(b, sizeof b, tz_zones[i]);
        set_check(g, NULL, 0, b, !strcmp(tz_zones[i], (const char *)global_settings.sloppy_timezone),
                  tz_pick_cb, (void *)(intptr_t)i);
    }
    tz_apply();                               /* labelling changed TZ; put it back */
    page_push(p);
}
static void tz_open_cb(lv_event_t *e) { (void)e; tz_page(); }

/* Settings > Time and power: the clock group (returned, for the rest of
 * the clock's switches) with the time zone row in it */
static lv_obj_t *tz_card(lv_obj_t *body)
{
    lv_obj_t *c = opt_card(body, "Clock", NULL);
    opt_switch(c, "Set time automatically", global_settings.sloppy_auto_time, wf_auto_time_set);
    tz_row = set_link_sub(c, LV_SYMBOL_GPS, 0xe89a1c | SET_TILE_ONLY, "Time zone", tz_row_text(), tz_row_text(), tz_open_cb, NULL);
    lv_obj_add_event_cb(tz_row, tz_row_deleted, LV_EVENT_DELETE, NULL);
    return c;
}

/* --------------------------------------------------------------- start-up */
static void wifi_startup(void)
{
    tz_apply();
    char probe[200];
    sl_net_wireless_probe(probe, sizeof probe);
    sl_log("wireless: %s", probe);   /* an owner's log says which chip their R1 has */
    if (sl_net_wireless_unsupported()) return;   /* nothing to bind: see the Wi-Fi page */
    wf_tick_start();
    if (global_settings.sloppy_wifi) {
        wf_arg_on = true;
        if (!wf_begin(WF_JOB_POWER)) {
            wf_power_failures = 1;
            wf_power_retry_at = sl_ms() + 5000;
        }
    }
}

/* the Settings row */
static const char *wf_row_text(void)
{
    static char b[64];
    if (sl_net_wireless_unsupported()) return "Not supported";
    if (!global_settings.sloppy_wifi) return "Off";
    if (!wf_st.connected) return "Not connected";
    snprintf(b, sizeof b, "%s", wf_st.ssid);
    return b;
}
