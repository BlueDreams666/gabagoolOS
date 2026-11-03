/* Bluetooth in GabagoolOS: Rockbox keeps the radio and audio routing. */

#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
#define sl_bt_status hiby_bt_status
#define sl_bt_devices hiby_bt_devices
#define sl_bt_power hiby_bt_power
#define sl_bt_connect hiby_bt_connect
#define sl_bt_disconnect hiby_bt_disconnect
#define sl_bt_forget hiby_bt_forget
#define sl_bt_scan_start hiby_bt_scan_start
#define sl_bt_scan_finish hiby_bt_scan_finish
#define sl_bt_codecs hiby_bt_codecs
#define sl_bt_codec hiby_bt_codec
#define sl_bt_auto_route hiby_bt_auto_route
#define sl_bt_player_name hiby_bt_player_name
#define sl_bt_set_player_name hiby_bt_set_player_name
#define sl_bt_rename hiby_bt_rename
#define sl_bt_pair_question hiby_bt_pair_question
#define sl_bt_pair_answer hiby_bt_pair_answer
#define sl_bt_last_error hiby_bt_last_error
#define sl_bt_last_device hiby_bt_last_device
#define sl_bt_reconnect hiby_bt_reconnect
#define sl_bt_sbc_quality hiby_bt_sbc_quality
#define sl_bt_set_sbc_quality hiby_bt_set_sbc_quality
#define sl_bt_suspended_now bt_is_suspended_fast
#define sl_bt_aap_status hiby_bt_aap_status
#define sl_bt_aap_noise hiby_bt_aap_noise
bool bt_is_suspended_fast(void);
#else
static bool sl_bt_demo_on = true;
static int sl_bt_demo_index = -1;
static struct hiby_bt_device sl_bt_demo_devices[] = {
    { "00:1A:7D:DA:71:13", "Studio headphones", true },
    { "00:1A:7D:DA:71:14", "Pocket speakers", true },
    { "00:1A:7D:DA:71:15", "Nearby headphones", false }
};
/* the simulator's names, pairing questions, misses and hangs: the harness
 * sets these to play through every state the real stack can be in */
static char sl_bt_demo_name[HIBY_BT_ALIAS_LEN], sl_bt_demo_last[18];
static int sl_bt_demo_sbc, sl_bt_demo_err, sl_bt_demo_rc_misses, sl_bt_demo_ask_kind;
static struct hiby_bt_ask sl_bt_demo_ask;
static bool sl_bt_demo_answered, sl_bt_demo_answer_yes, sl_bt_demo_hang, sl_bt_demo_suspended;
static char sl_bt_demo_answer_code[20];
static uint32_t sl_bt_demo_delay_ms;     /* every job takes this long */
static char sl_bt_demo_codec[HIBY_BT_CODEC_NAME_LEN] = "AAC";   /* what the headphones play with */
#define SL_BT_DEMO_CODEC 1               /* the harness sets the two above */
static int sl_bt_demo_codec_n = 2;       /* how many of SBC, AAC, aptX, aptX-HD, LDAC they offer */
static int sl_bt_demo_fail;              /* HIBY_BT_ERR_*: the next connects fail so */
static void sl_bt_status(struct hiby_bt_status *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled = sl_bt_demo_on;
    s->connected = s->routed = sl_bt_demo_index >= 0 && sl_bt_demo_on;
    if (s->connected)
    {
        strlcpy(s->mac, sl_bt_demo_devices[sl_bt_demo_index].mac, sizeof s->mac);
        strlcpy(s->name, sl_bt_demo_devices[sl_bt_demo_index].name, sizeof s->name);
        strlcpy(s->codec, sl_bt_demo_codec, sizeof s->codec);
    }
}
static int sl_bt_devices(struct hiby_bt_device *d, int max)
{
    int n = max < 3 ? max : 3;
    memcpy(d, sl_bt_demo_devices, n * sizeof *d);
    return n;
}
static bool sl_bt_power(bool on) { sl_bt_demo_on = on; if (!on) sl_bt_demo_index = -1; return true; }
static bool sl_bt_demo_answer_ok(void);
static bool sl_bt_connect(const struct hiby_bt_device *d)
{
    sl_bt_demo_err = HIBY_BT_ERR_NONE;
    if (sl_bt_demo_fail) { sl_bt_demo_err = sl_bt_demo_fail; return false; }   /* the harness's pretend failure */
    if (!d->paired && !sl_bt_demo_answer_ok()) return false;
    for (int i = 0; i < 3; i++)
        if (!strcmp(d->mac, sl_bt_demo_devices[i].mac))
        {
            sl_bt_demo_index = i; sl_bt_demo_devices[i].paired = true;
            strlcpy(sl_bt_demo_last, d->mac, sizeof sl_bt_demo_last);
            return true;
        }
    return false;
}
static bool sl_bt_disconnect(void) { sl_bt_demo_index = -1; return true; }
static bool sl_bt_forget(const char *mac)
{ for (int i = 0; i < 3; i++) if (!strcmp(mac, sl_bt_demo_devices[i].mac)) { sl_bt_demo_devices[i].paired = false; if (sl_bt_demo_index == i) sl_bt_demo_index = -1; return true; } return false; }
static bool sl_bt_scan_start(void) { return true; }
static int sl_bt_scan_finish(struct hiby_bt_device *d, int max) { return d ? sl_bt_devices(d, max) : 0; }
static int sl_bt_codecs(const char *mac, char codecs[][HIBY_BT_CODEC_NAME_LEN], int max)
{
    static const char *const names[] = { "SBC", "AAC", "aptX", "aptX-HD", "LDAC" };
    (void)mac;
    int n = sl_bt_demo_codec_n < max ? sl_bt_demo_codec_n : max;
    for (int i = 0; i < n; i++) strlcpy(codecs[i], names[i], HIBY_BT_CODEC_NAME_LEN);
    return n;
}
static bool sl_bt_codec(const char *mac, const char *codec) { (void)mac; strlcpy(sl_bt_demo_codec, codec, sizeof sl_bt_demo_codec); return true; }
static bool sl_bt_auto_route(void) { return sl_bt_demo_on && sl_bt_demo_index >= 0; }
static void sl_bt_player_name(char *out, int size) { strlcpy(out, sl_bt_demo_name, size); }
static bool sl_bt_set_player_name(const char *name) { strlcpy(sl_bt_demo_name, name, sizeof sl_bt_demo_name); return true; }
static bool sl_bt_rename(const char *mac, const char *name)
{
    for (int i = 0; i < 3; i++)
        if (!strcmp(mac, sl_bt_demo_devices[i].mac))
            strlcpy(sl_bt_demo_devices[i].name, name[0] ? name : i == 0 ? "Studio headphones" : i == 1 ? "Pocket speakers" : "Nearby headphones",
                    sizeof sl_bt_demo_devices[i].name);
    return true;
}
static bool sl_bt_pair_question(struct hiby_bt_ask *a) { *a = sl_bt_demo_ask; return a->kind != HIBY_BT_ASK_NONE; }
static void sl_bt_pair_answer(unsigned serial, bool yes, const char *code)
{
    if (serial != sl_bt_demo_ask.serial) return;
    sl_bt_demo_answered = true; sl_bt_demo_answer_yes = yes;
    strlcpy(sl_bt_demo_answer_code, code ? code : "", sizeof sl_bt_demo_answer_code);
}
static int sl_bt_last_error(void) { return sl_bt_demo_err; }
static bool sl_bt_last_device(char mac[18]) { strlcpy(mac, sl_bt_demo_last, 18); return mac[0]; }
static int sl_bt_reconnect(void)
{
    if (!sl_bt_demo_on || !sl_bt_demo_last[0]) return -1;
    if (sl_bt_demo_rc_misses > 0) { sl_bt_demo_rc_misses--; sl_bt_demo_err = HIBY_BT_ERR_NOT_FOUND; return 0; }
    for (int i = 0; i < 3; i++) if (!strcmp(sl_bt_demo_last, sl_bt_demo_devices[i].mac)) sl_bt_demo_index = i;
    sl_bt_demo_err = HIBY_BT_ERR_NONE;
    return 1;
}
static int sl_bt_sbc_quality(void) { return sl_bt_demo_sbc; }
static bool sl_bt_set_sbc_quality(int q) { sl_bt_demo_sbc = q; return true; }
static bool sl_bt_suspended_now(void) { return sl_bt_demo_suspended; }
static struct hiby_bt_aap sl_bt_demo_aap = { .left = -1, .right = -1, .box = -1 };
static void sl_bt_aap_status(struct hiby_bt_aap *a) { *a = sl_bt_demo_aap; }
static bool sl_bt_aap_noise(int mode) { if (!sl_bt_demo_aap.up) return false; sl_bt_demo_aap.noise = mode; return true; }
#endif

/* Receive from phone: the player as Bluetooth headphones. The session
 * runs on a thread in hiby_bluetooth.c; the UI reads its snapshot. */
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
#define sl_btrx_start hiby_btrx_start
#define sl_btrx_stop hiby_btrx_stop
#define sl_btrx_running hiby_btrx_running
#define sl_btrx_get hiby_btrx_status
#define sl_btrx_control hiby_btrx_control
#define sl_btrx_take_event hiby_btrx_take_event
#define sl_btrx_read hiby_btrx_read
#define sl_btrx_buffered hiby_btrx_buffered
#define sl_btrx_idle_ms hiby_btrx_idle_ms
#define sl_btrx_codec hiby_btrx_codec
#else
/* The simulator's pretend phone: it connects a few seconds after the
 * player becomes visible. The harness turns that off (sl_btrx_demo_auto)
 * and drives it by hand, audio included (sl_btrx_demo_push). */
static struct hiby_btrx_status sl_btrx_demo;
static bool sl_btrx_demo_running, sl_btrx_demo_auto = true, sl_btrx_demo_fail;
static uint32_t sl_btrx_demo_since, sl_btrx_demo_last;
static char sl_btrx_demo_ctl[12];
static int16_t sl_btrx_demo_ring[8192 * 2];
static uint32_t sl_btrx_demo_head, sl_btrx_demo_tail;
static bool sl_btrx_demo_said;
static bool sl_btrx_start(void)
{
    memset(&sl_btrx_demo, 0, sizeof sl_btrx_demo);
    sl_btrx_demo.volume = -1;
    sl_btrx_demo_since = sl_ms();
    sl_btrx_demo_said = false;
    sl_btrx_demo_running = !sl_btrx_demo_fail;
    sl_btrx_demo.state = sl_btrx_demo_fail ? HIBY_BTRX_OFF : HIBY_BTRX_STARTING;
    if (sl_btrx_demo_fail) strlcpy(sl_btrx_demo.error, "Bluetooth could not switch to receiving", sizeof sl_btrx_demo.error);
    return true;
}
static void sl_btrx_stop(void) { sl_btrx_demo.state = HIBY_BTRX_OFF; sl_btrx_demo_running = false; }
static bool sl_btrx_running(void) { return sl_btrx_demo_running; }
static void sl_btrx_get(struct hiby_btrx_status *s)
{
    if (sl_btrx_demo.state == HIBY_BTRX_STARTING) sl_btrx_demo.state = HIBY_BTRX_WAITING;
    if (sl_btrx_demo_auto && sl_btrx_demo.state == HIBY_BTRX_WAITING && sl_ms() - sl_btrx_demo_since > 4000)
    {
        sl_btrx_demo.state = HIBY_BTRX_CONNECTED;
        sl_btrx_demo.connects++;
        strlcpy(sl_btrx_demo.mac, "AA:BB:CC:DD:EE:01", sizeof sl_btrx_demo.mac);
        strlcpy(sl_btrx_demo.name, "Demo phone", sizeof sl_btrx_demo.name);
        strlcpy(sl_btrx_demo.codec, "AAC", sizeof sl_btrx_demo.codec);
        sl_btrx_demo.rate = 44100;
        sl_btrx_demo.channels = 2;
    }
    *s = sl_btrx_demo;
}
static void sl_btrx_control(const char *m) { strlcpy(sl_btrx_demo_ctl, m, sizeof sl_btrx_demo_ctl); }
/* the pretend phone takes AAC and SBC, and keeps AAC over LDAC */
static void sl_btrx_codec(const char *c)
{
    bool takes = !strcmp(c, "AAC") || !strcmp(c, "SBC");
    if (takes) strlcpy(sl_btrx_demo.codec, c, sizeof sl_btrx_demo.codec);
    strlcpy(sl_btrx_demo.codec_note, takes ? "" : "kept", sizeof sl_btrx_demo.codec_note);
}
static bool sl_btrx_take_event(char *line, int size)
{
    if (sl_btrx_demo_said || !sl_btrx_demo_running) return false;
    sl_btrx_demo_said = true;
    strlcpy(line, "start: receive from phone (simulator)", size);
    return true;
}
static void sl_btrx_demo_push(const int16_t *frames, int n)
{
    for (int i = 0; i < n && sl_btrx_demo_head - sl_btrx_demo_tail < 8192; i++, sl_btrx_demo_head++)
    {
        sl_btrx_demo_ring[(sl_btrx_demo_head & 8191) * 2] = frames[i * 2];
        sl_btrx_demo_ring[(sl_btrx_demo_head & 8191) * 2 + 1] = frames[i * 2 + 1];
    }
    sl_btrx_demo_last = sl_ms();
}
static int sl_btrx_read(int16_t *dst, int frames)
{
    int n = 0;
    for (; n < frames && sl_btrx_demo_tail != sl_btrx_demo_head; n++, sl_btrx_demo_tail++)
    {
        dst[n * 2] = sl_btrx_demo_ring[(sl_btrx_demo_tail & 8191) * 2];
        dst[n * 2 + 1] = sl_btrx_demo_ring[(sl_btrx_demo_tail & 8191) * 2 + 1];
    }
    return n;
}
static int sl_btrx_buffered(void) { return (int)(sl_btrx_demo_head - sl_btrx_demo_tail); }
static unsigned sl_btrx_idle_ms(void) { return sl_btrx_demo_last ? sl_ms() - sl_btrx_demo_last : 60000; }
#endif
static bool sl_btrx_on;                  /* a session runs, or is putting Bluetooth back */
static void sl_btrx_card_cb(lv_event_t *e);

/* Bluetooth and the jack keep their own volume. How loud a Bluetooth level
 * is depends on the headphones' own amplifier, so one shared level was too
 * quiet on one and, after headphones dropped, far too loud in wired
 * earphones. Each output gets back the level it had; one never set starts
 * no louder than half, or the level now if that is lower. Checked every
 * loop pass: cheap, and the route itself sends no level (hiby_bt_volume_by_ui). */
#define SL_BT_FIRST_VOLUME_PCT 50
static const char *sl_bt_routed_mac(void)
{
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
    const char *hiby_pcm_get_bt_mac(void);
    return hiby_pcm_get_bt_mac();
#else
    return sl_bt_demo_on && sl_bt_demo_index >= 0 ? sl_bt_demo_devices[sl_bt_demo_index].mac : NULL;
#endif
}
static bool sl_volume_valid(int v)
{
    return v >= sound_min(SOUND_VOLUME) && v <= sound_max(SOUND_VOLUME);
}
/* Each output keeps its own maximum and button step (Settings > Sound):
 * the jack's in sloppy_jack_limit and sloppy_volume_step, Bluetooth's in
 * sloppy_bt_limit and sloppy_bt_step, the USB DAC's in sloppy_dac_limit
 * and sloppy_dac_step. volume_limit, which Rockbox clamps every level
 * with, is the one of the output in use (sloppy_volume_on_bt). */
enum { SL_OUT_JACK, SL_OUT_BT, SL_OUT_DAC };
static int sl_out_limit_pct(int out)
{
    int p = out == SL_OUT_DAC ? global_settings.sloppy_dac_limit :
            out == SL_OUT_BT ? global_settings.sloppy_bt_limit : global_settings.sloppy_jack_limit;
    return p >= 10 && p <= 100 ? p : 100;
}
static void sl_out_limit_apply(void)
{
    global_settings.volume_limit = volume_from_pct(sl_out_limit_pct(global_status.sloppy_volume_on_bt));
}
static int sl_out_step(int out)
{
    int s = out == SL_OUT_DAC ? global_settings.sloppy_dac_step :
            out == SL_OUT_BT ? global_settings.sloppy_bt_step : global_settings.sloppy_volume_step;
    return s >= 1 && s <= 10 ? s : 1;
}
static int sl_vol_step(void) { return sl_out_step(global_status.sloppy_volume_on_bt); }
/* once, at start: a jack maximum set before outputs kept their own */
static void sl_out_volume_startup(void)
{
    if (global_settings.sloppy_jack_limit < 10 || global_settings.sloppy_jack_limit > 100)
    {
        int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME), v = global_settings.volume_limit;
        int p = mx > mn ? ((v - mn) * 100 + (mx - mn) / 2) / (mx - mn) : 100;
        global_settings.sloppy_jack_limit = p < 10 ? 10 : p > 100 ? 100 : p;
    }
    sl_out_limit_apply();
}
/* The player as a computer's USB DAC (Settings > Time and power > USB):
 * the computer sends its own full level, so that session keeps a level of
 * its own, and the first one ever starts no louder than -30 dB (R11, the
 * "deafening over USB-C" reports). One record: the gadget cannot tell one
 * computer from another. */
#define SL_DAC_FIRST_DB (-30)
static bool sl_usb_dac_demo;              /* the harness's USB DAC session */
static bool sl_usb_dac_on(void)
{
#if defined(HAVE_HOST_USB_AUDIO) && !defined(SIMULATOR)
    return usb_audio_get_active();
#else
    return sl_usb_dac_demo;
#endif
}
static int *sl_out_level(int out)
{
    return out == SL_OUT_DAC ? &global_status.sloppy_dac_volume :
           out == SL_OUT_BT ? &global_status.sloppy_bt_volume : &global_status.sloppy_jack_volume;
}
/* an output never used yet: no louder than the level now, nor than half
 * on Bluetooth, nor than -30 dB as a USB DAC */
static int sl_out_first_level(int out)
{
    int v = global_status.volume;
    if (out == SL_OUT_BT)
        v = MIN(v, volume_from_pct(SL_BT_FIRST_VOLUME_PCT));
    else if (out == SL_OUT_DAC)
    {
        int unit = sound_numdecimals(SOUND_VOLUME) ? 10 : 1;
        v = MIN(v, MAX(SL_DAC_FIRST_DB * unit, sound_min(SOUND_VOLUME)));
    }
    return MIN(v, volume_from_pct(sl_out_limit_pct(out)));
}
static void sl_bt_volume_follow(void)
{
    static char seen_mac[18];
    const char *mac = sl_bt_routed_mac();
    int out = sl_usb_dac_on() ? SL_OUT_DAC : mac ? SL_OUT_BT : SL_OUT_JACK;
    int was = global_status.sloppy_volume_on_bt;
    if (was < SL_OUT_JACK || was > SL_OUT_DAC) was = SL_OUT_JACK;
    if (out == was)
    {
        /* straight from one pair of headphones to another: same level,
         * but the new pair has to be told it */
        if (out == SL_OUT_BT && strcmp(mac, seen_mac))
        {
            strlcpy(seen_mac, mac, sizeof seen_mac);
            setvol();
        }
        return;
    }
    *sl_out_level(was) = global_status.volume;
    int want = *sl_out_level(out);
    if (!sl_volume_valid(want))
        want = sl_out_first_level(out);
    static const char *const names[] = { "the jack", "Bluetooth", "the USB DAC" };
    sl_log("volume: now %s, level %d", names[out], want);
    global_status.volume = want;
    global_status.sloppy_volume_on_bt = out;
    sl_out_limit_apply();                /* its own maximum */
    strlcpy(seen_mac, out == SL_OUT_BT ? mac : "", sizeof seen_mac);
    setvol();                            /* Bluetooth: sent to the headphones now */
    statusbar_update();
}
/* At power-on the jack plays first; Bluetooth and the USB DAC take their
 * level back when they start. */
static void sl_bt_volume_startup(void)
{
    int was = global_status.sloppy_volume_on_bt;
    if (was == SL_OUT_JACK) return;
    if (was == SL_OUT_BT || was == SL_OUT_DAC) *sl_out_level(was) = global_status.volume;
    if (sl_volume_valid(global_status.sloppy_jack_volume))
        global_status.volume = global_status.sloppy_jack_volume;
    else
        global_status.volume = MIN(global_status.volume, volume_from_pct(SL_BT_FIRST_VOLUME_PCT));
    global_status.sloppy_volume_on_bt = SL_OUT_JACK;
    sl_out_limit_apply();
    setvol();
}

static lv_obj_t *sl_bt_page_obj, *sl_bt_body, *sl_bt_busy_label, *sl_bt_scan_label;
static lv_timer_t *sl_bt_scan_timer, *sl_bt_refresh_timer;
static struct hiby_bt_status sl_bt_state;
static bool sl_bt_known;                 /* sl_bt_state came from the stack, not a guess */
static lv_obj_t *sl_bt_switch;
static lv_timer_t *sl_bt_watch_timer;
static struct hiby_bt_device sl_bt_found[HIBY_BT_MAX_DEVICES];
static int sl_bt_count;
static bool sl_bt_scanning, sl_bt_busy;
static uint32_t sl_bt_scan_until;
static char sl_bt_codecs_found[8][HIBY_BT_CODEC_NAME_LEN];
static int sl_bt_codec_count;
static char sl_bt_forget_mac[18], sl_bt_forget_name[HIBY_BT_NAME_LEN];
enum { SL_BT_JOB_NONE, SL_BT_JOB_REFRESH, SL_BT_JOB_POWER, SL_BT_JOB_CONNECT,
       SL_BT_JOB_DISCONNECT, SL_BT_JOB_FORGET, SL_BT_JOB_CODEC,
       SL_BT_JOB_SCAN, SL_BT_JOB_SCAN_FINISH, SL_BT_JOB_AUTO,
       SL_BT_JOB_RECONNECT, SL_BT_JOB_RENAME, SL_BT_JOB_NAME, SL_BT_JOB_SBC };
static int sl_bt_job;
static bool sl_bt_job_power, sl_bt_auto_wanted;
static struct hiby_bt_device sl_bt_job_device;
static char sl_bt_job_mac[18], sl_bt_job_codec[HIBY_BT_CODEC_NAME_LEN], sl_bt_job_name[HIBY_BT_ALIAS_LEN];
static int sl_bt_job_sbc;
/* the watchdog: a job past its time is let go (its late answer ignored)
 * and the page says so, instead of waiting on the stack forever */
static uint32_t sl_bt_job_started;
static unsigned sl_bt_job_gen;
static bool sl_bt_quiet_power;           /* the power-on at start: no toast */
/* why the last connect to a device failed, shown on its row */
static char sl_bt_err_mac[18];
static int sl_bt_err_code;
static lv_timer_t *sl_bt_job_timer;
static void sl_bt_rebuild(void);
static void sl_bt_begin(int job, const char *message);
static void sl_bt_scan_tick(lv_timer_t *timer);
/* The worker owns all blocking BlueZ/BlueALSA calls. Only the LVGL thread
 * touches objects and the visible snapshot. One operation runs at a time. */
struct sl_bt_result {
    struct hiby_bt_status state;
    struct hiby_bt_device devices[HIBY_BT_MAX_DEVICES];
    char codecs[8][HIBY_BT_CODEC_NAME_LEN];
    int count, codec_count, job, err, rc;
    unsigned gen;
    bool ok;
};
static struct sl_bt_result sl_bt_result;
static bool sl_bt_job_done;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
static pthread_mutex_t sl_bt_job_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void *sl_bt_worker(void *unused)
{
    (void)unused;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    hiby_bt_worker_thread();             /* no Rockbox kernel calls from here */
#endif
    struct sl_bt_result result;
    memset(&result, 0, sizeof result);
    result.job = sl_bt_job;
    result.gen = sl_bt_job_gen;
    switch (result.job)
    {
        case SL_BT_JOB_RECONNECT: result.rc = sl_bt_reconnect(); result.ok = result.rc > 0; break;
        case SL_BT_JOB_RENAME: result.ok = sl_bt_rename(sl_bt_job_mac, sl_bt_job_name); break;
        case SL_BT_JOB_NAME: result.ok = sl_bt_set_player_name(sl_bt_job_name); break;
        case SL_BT_JOB_SBC: result.ok = sl_bt_set_sbc_quality(sl_bt_job_sbc); break;
        case SL_BT_JOB_POWER: result.ok = sl_bt_power(sl_bt_job_power); break;
        case SL_BT_JOB_CONNECT: result.ok = sl_bt_connect(&sl_bt_job_device); break;
        case SL_BT_JOB_DISCONNECT: result.ok = sl_bt_disconnect(); break;
        case SL_BT_JOB_FORGET: result.ok = sl_bt_forget(sl_bt_job_mac); break;
        case SL_BT_JOB_CODEC: result.ok = sl_bt_codec(sl_bt_job_mac, sl_bt_job_codec); break;
        case SL_BT_JOB_AUTO: result.ok = sl_bt_auto_route(); break;
        case SL_BT_JOB_SCAN: result.ok = sl_bt_scan_start(); break;
        case SL_BT_JOB_SCAN_FINISH:
            result.count = sl_bt_scan_finish(result.devices, HIBY_BT_MAX_DEVICES);
            result.ok = true;
            break;
        default: result.ok = true; break;
    }
    if (result.job == SL_BT_JOB_CONNECT || result.job == SL_BT_JOB_RECONNECT)
        result.err = result.ok ? HIBY_BT_ERR_NONE : sl_bt_last_error();
    if (result.job != SL_BT_JOB_SCAN && result.job != SL_BT_JOB_SCAN_FINISH)
        sl_bt_status(&result.state);
    if (result.job != SL_BT_JOB_SCAN && result.job != SL_BT_JOB_SCAN_FINISH)
        result.count = sl_bt_devices(result.devices, HIBY_BT_MAX_DEVICES);
    if (result.job != SL_BT_JOB_SCAN && result.job != SL_BT_JOB_SCAN_FINISH &&
        result.state.connected)
        result.codec_count = sl_bt_codecs(result.state.mac, result.codecs, 8);
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_mutex_lock(&sl_bt_job_lock);
#endif
    if (result.gen == sl_bt_job_gen)     /* let go by the watchdog: nobody waits for it now */
    {
        sl_bt_result = result;
        sl_bt_job_done = true;
    }
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_mutex_unlock(&sl_bt_job_lock);
#endif
    return NULL;
}

static void sl_bt_refresh_cb(lv_timer_t *timer)
{
    lv_timer_pause(timer);
    if (sl_bt_page_obj) sl_bt_rebuild();
}
static void sl_bt_refresh_soon(void)
{
    if (!sl_bt_refresh_timer)
        sl_bt_refresh_timer = lv_timer_create(sl_bt_refresh_cb, 40, NULL);
    lv_timer_reset(sl_bt_refresh_timer);
    lv_timer_resume(sl_bt_refresh_timer);
}
/* A new audio route needs playback restarted, which only a Rockbox thread
 * may do: pause here, resume a moment later from a timer. */
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
static void sl_bt_resume_cb(lv_timer_t *timer)
{
    (void)timer;                         /* one shot: LVGL deletes it */
    /* not if play/pause was pressed in the meantime */
    if (sl_bt_resume_due && (audio_status() & AUDIO_STATUS_PAUSE)) sl_resume();   /* a book: a few seconds back */
    sl_bt_resume_due = false;
}
#endif
static void sl_bt_kick_audio(void)
{
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    if (hiby_bt_take_audio_pause())
    {
        hiby_bt_take_audio_kick();      /* asked for here: the job's toast says why */
        if (is_playing()) audio_pause();
        return;
    }
    if (!hiby_bt_take_audio_kick()) return;
    int st = audio_status();
    if (!(st & AUDIO_STATUS_PLAY) || (st & AUDIO_STATUS_PAUSE)) return;
    audio_pause();
    sl_bt_resume_due = true;
    lv_timer_set_repeat_count(lv_timer_create(sl_bt_resume_cb, 250, NULL), 1);
#endif
}
static void sl_bt_ask_poll(void);
static void sl_bt_rc_result(const struct sl_bt_result *r, bool was_routed);
static void sl_bt_rc_arm(const char *why);
static void sl_bt_rc_disarm(const char *why);
static const char *sl_bt_err_text(int err, const char *name);
#if !(defined(HIBY_LINUX) && !defined(SIMULATOR))
static void sl_bt_demo_step(void);
#endif
/* how long a job may take before the page stops waiting on it: a pairing
 * with questions and two connect tries, a power-on with the AIC firmware */
static uint32_t sl_bt_job_limit(int job)
{
    return job == SL_BT_JOB_CONNECT ? 180000 : job == SL_BT_JOB_POWER ? 75000 :
           job == SL_BT_JOB_SBC ? 60000 : 45000;
}
static void sl_bt_job_cb(lv_timer_t *timer)
{
    struct sl_bt_result result;
#if !(defined(HIBY_LINUX) && !defined(SIMULATOR))
    sl_bt_demo_step();                   /* the simulator's stack, as slow or stuck as asked */
#endif
    sl_bt_ask_poll();                    /* a passkey or PIN: its page */
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_mutex_lock(&sl_bt_job_lock);
#endif
    bool done = sl_bt_job_done;
    if (done) { result = sl_bt_result; sl_bt_job_done = false; }
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_mutex_unlock(&sl_bt_job_lock);
#endif
    if (!done)
    {
        if (sl_bt_busy && lv_tick_elaps(sl_bt_job_started) > sl_bt_job_limit(sl_bt_job))
        {
            /* the stack never answered: stop waiting, say so, and leave
             * the worker to end on its own deadline (its answer is not
             * taken: the generation moved on) */
            sl_log("bluetooth: job %d gave no answer in %lu s, let go", sl_bt_job,
                   (unsigned long)(sl_bt_job_limit(sl_bt_job) / 1000));
            sl_bt_job_gen++;
            int job = sl_bt_job;
            sl_bt_job = SL_BT_JOB_NONE;
            sl_bt_busy = false;
            sl_bt_known = false;
            lv_timer_pause(timer);
            sl_bt_ask_poll();
            if (job != SL_BT_JOB_REFRESH && job != SL_BT_JOB_RECONNECT)
                notify_ex(NS_BT, NOTE_WARN, 0, NA_BT, NULL, sl_bt_err_text(HIBY_BT_ERR_HUNG, ""));
            if (job == SL_BT_JOB_RECONNECT)
            {
                struct sl_bt_result miss = { .job = SL_BT_JOB_RECONNECT, .rc = 0 };
                sl_bt_rc_result(&miss, false);
            }
            if (sl_bt_page_obj) sl_bt_refresh_soon();
            if (qp_is_visible()) qp_refresh();
        }
        return;
    }
    lv_timer_pause(timer);
    sl_bt_ask_poll();
    bool was_routed = sl_bt_known && sl_bt_state.routed;
    sl_bt_kick_audio();
    /* what the stack said, so a failure toast can be told apart on the card */
    if (result.job != SL_BT_JOB_REFRESH)
        sl_log("bluetooth: job %d %s; on %d, connected %d, routed %d, %s %s, %d devices",
               result.job, result.ok ? "ok" : "FAILED", result.state.enabled, result.state.connected,
               result.state.routed, result.state.mac[0] ? result.state.mac : "-",
               result.state.codec[0] ? result.state.codec : "-", result.count);
    /* A quiet re-check only redraws the page when something changed. */
    bool changed = true;
    if (result.job == SL_BT_JOB_REFRESH && sl_bt_known)
        changed = memcmp(&sl_bt_state, &result.state, sizeof result.state) ||
                  sl_bt_count != result.count ||
                  memcmp(sl_bt_found, result.devices, sl_bt_count * sizeof sl_bt_found[0]);
    const char *message = NULL;
    switch (result.job)
    {
        case SL_BT_JOB_POWER:
            if (!sl_bt_job_power && sl_bt_scanning)
            {
                sl_bt_scanning = false;
                if (sl_bt_scan_timer) lv_timer_pause(sl_bt_scan_timer);
            }
            message = result.ok ? (sl_bt_job_power ? "Bluetooth is on" : "Bluetooth is off") :
                (sl_bt_job_power ? "Bluetooth radio did not start. Retry or reboot the player." :
                                   "Bluetooth did not turn off. Please retry.");
            if (sl_bt_quiet_power && result.ok) message = NULL;   /* at start: nothing to say */
            sl_bt_quiet_power = false;
            if (result.ok && sl_bt_job_power) sl_bt_rc_arm("Bluetooth on");
            else if (result.ok) sl_bt_rc_disarm("Bluetooth off");
            break;
        case SL_BT_JOB_CONNECT:
            if (result.ok)
            {
                message = "Headphones connected";
                sl_bt_err_mac[0] = '\0';
                sl_bt_rc_disarm("connected");
            }
            else
            {
                /* why, in words, and on the device's row */
                strlcpy(sl_bt_err_mac, sl_bt_job_device.mac, sizeof sl_bt_err_mac);
                sl_bt_err_code = result.err ? result.err : HIBY_BT_ERR_NOT_FOUND;
                message = sl_bt_err_text(sl_bt_err_code, sl_bt_job_device.name);
            }
            break;
        case SL_BT_JOB_DISCONNECT:
            message = result.ok ? "Disconnected" : "Could not disconnect";
            sl_bt_rc_disarm("let go");   /* asked for: not reconnected behind the user's back */
            break;
        case SL_BT_JOB_FORGET:
            if (sl_bt_page_obj) page_bring_back(sl_bt_page_obj);
            message = result.ok ? "Device forgotten" : "Could not forget device";
            break;
        case SL_BT_JOB_RECONNECT:
            sl_bt_rc_result(&result, was_routed);
            if (result.ok && result.state.routed && !was_routed)
            {
                static char back_on[HIBY_BT_NAME_LEN + 16];
                snprintf(back_on, sizeof back_on, "%s %s", sl_tr("Playing on"), result.state.name);
                message = back_on;
            }
            break;
        case SL_BT_JOB_RENAME:
            message = result.ok ? "Name saved" : "Could not save the name";
            break;
        case SL_BT_JOB_NAME:
            message = result.ok ? "Name saved" : "Saved. Bluetooth takes it when it next starts.";
            break;
        case SL_BT_JOB_SBC:
            message = result.ok ? "SBC quality set" : "Could not restart Bluetooth audio. Please retry.";
            break;
        case SL_BT_JOB_CODEC:
            message = result.ok ? "Audio codec changed" : "Could not change codec";
            break;
        case SL_BT_JOB_SCAN:
            if (result.ok)
            {
                sl_bt_scanning = true;
                sl_bt_scan_until = lv_tick_get() + 15000;
                if (!sl_bt_scan_timer) sl_bt_scan_timer = lv_timer_create(sl_bt_scan_tick, 250, NULL);
                lv_timer_reset(sl_bt_scan_timer);
                lv_timer_resume(sl_bt_scan_timer);
            }
            else message = "Could not start scanning";
            break;
        case SL_BT_JOB_SCAN_FINISH:
            sl_bt_scanning = false;
            if (sl_bt_scan_timer) lv_timer_pause(sl_bt_scan_timer);
            message = result.count ? NULL : "No named devices found. Put headphones in pairing mode and scan again.";
            break;
        case SL_BT_JOB_AUTO:
            /* headphones that came back by themselves: say where the
             * sound went, once, and nothing when it was there already */
            if (result.ok && result.state.routed && !was_routed)
            {
                static char playing_on[HIBY_BT_NAME_LEN + 16];
                snprintf(playing_on, sizeof playing_on, "%s %s", sl_tr("Playing on"), result.state.name);
                message = playing_on;
            }
            break;
    }
    if (result.job != SL_BT_JOB_SCAN)
    {
        if (result.job != SL_BT_JOB_SCAN_FINISH) { sl_bt_state = result.state; sl_bt_known = true; }
        sl_bt_count = result.count < 0 ? 0 : result.count;
        if (sl_bt_count > HIBY_BT_MAX_DEVICES) sl_bt_count = HIBY_BT_MAX_DEVICES;
        memcpy(sl_bt_found, result.devices, sl_bt_count * sizeof sl_bt_found[0]);
        if (result.job != SL_BT_JOB_SCAN_FINISH)
        {
            sl_bt_codec_count = result.codec_count;
            memcpy(sl_bt_codecs_found, result.codecs, sizeof sl_bt_codecs_found);
        }
    }
    sl_bt_job = SL_BT_JOB_NONE;
    sl_bt_busy = false;
    bool warn = !strncmp(message ? message : "", "Could not", 9) || (result.job == SL_BT_JOB_CONNECT && !result.ok);
    if (message) notify_ex(NS_BT, warn ? NOTE_WARN : NOTE_INFO, 0, NA_BT, NULL, message);
    if (changed) sl_bt_refresh_soon();
    else if (sl_bt_switch) lv_obj_remove_state(sl_bt_switch, LV_STATE_DISABLED);
    if (qp_is_visible()) qp_refresh();   /* its tile in the control centre */
    if (sl_bt_auto_wanted) { sl_bt_auto_wanted = false; sl_bt_begin(SL_BT_JOB_AUTO, "Connecting audio…"); }
}
static void sl_bt_begin(int job, const char *message)
{
    if (sl_bt_busy || sl_net_wireless_unsupported()) return;   /* an AIC8800 unit: nothing to drive */
    /* receiving owns BlueALSA and the radio; a quiet look is still fine */
    if (sl_btrx_on && job != SL_BT_JOB_REFRESH)
    {
        if (job != SL_BT_JOB_AUTO && job != SL_BT_JOB_RECONNECT) toast_warn("Stop receiving from your phone first");
        if (sl_bt_switch) sl_bt_refresh_soon();   /* the switch shows the truth again */
        return;
    }
    sl_bt_busy = true;
    sl_bt_job = job;
    sl_bt_job_started = lv_tick_get();
    sl_bt_job_gen++;
    if (sl_bt_busy_label && job != SL_BT_JOB_REFRESH && message[0]) lv_label_set_text(sl_bt_busy_label, message);
    if (sl_bt_switch) lv_obj_add_state(sl_bt_switch, LV_STATE_DISABLED);
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    pthread_t thread;
    if (pthread_create(&thread, NULL, sl_bt_worker, NULL) == 0)
        pthread_detach(thread);
    else
    {
        sl_bt_busy = false;
        sl_bt_job = SL_BT_JOB_NONE;
        toast_warn("Could not start Bluetooth task. Please retry.");
        return;
    }
#else
    /* the simulator's stack answers at once, unless the harness made it
     * slow, stuck or asking: then the job timer runs it (sl_bt_demo_step) */
    if (!sl_bt_demo_hang && !sl_bt_demo_delay_ms && !(job == SL_BT_JOB_CONNECT && sl_bt_demo_ask_kind))
        sl_bt_worker(NULL);
#endif
    if (!sl_bt_job_timer) sl_bt_job_timer = lv_timer_create(sl_bt_job_cb, 100, NULL);
    lv_timer_reset(sl_bt_job_timer);
    lv_timer_resume(sl_bt_job_timer);
}

/* The button tick saw headphones the stack connected by itself. Routing to
 * them waits on BlueALSA for seconds, which on the UI thread (Rockbox's own
 * handler) froze the screen and starved the decoder: run it on the worker,
 * after whatever job is running. */
static void sl_bt_auto_request(void)
{
    if (sl_bt_busy || sl_bt_scanning) sl_bt_auto_wanted = true;
    else sl_bt_begin(SL_BT_JOB_AUTO, "Connecting audio…");
}

/* ------------------------------------------------------- honest errors */
/* What went wrong, in words a listener can act on (R9: Sonix's most
 * repeated complaint is Bluetooth failing with no reason given). */
static const char *sl_bt_err_text(int err, const char *name)
{
    static char said[200];
    const char *f;
    switch (err)
    {
        case HIBY_BT_ERR_RADIO: f = "Bluetooth radio did not start. Turn it off and on, or restart the player."; break;
        case HIBY_BT_ERR_PAIR_REJECTED: f = "%s turned the pairing down. Put it in pairing mode and try again."; break;
        case HIBY_BT_ERR_PAIR_FAILED: f = "Pairing with %s failed. Check the code, or reset it and pair again."; break;
        case HIBY_BT_ERR_PAIR_TIMEOUT: f = "Pairing with %s timed out. Try again."; break;
        case HIBY_BT_ERR_CANCELED: f = "Pairing canceled"; break;
        case HIBY_BT_ERR_NO_AUDIO: f = "%s connected, but no audio came through. Try again, or forget it and pair again."; break;
        case HIBY_BT_ERR_HUNG: f = "Bluetooth stopped answering. Turn it off and on, or restart the player."; break;
        default: f = "%s didn't answer. Check it's on and nearby, then try again."; break;
    }
    snprintf(said, sizeof said, sl_tr(f), name && name[0] ? name : sl_tr("The device"));
    return said;
}
/* the same, short, on the device's row */
static const char *sl_bt_err_short(int err)
{
    switch (err)
    {
        case HIBY_BT_ERR_PAIR_REJECTED: case HIBY_BT_ERR_PAIR_FAILED: case HIBY_BT_ERR_PAIR_TIMEOUT:
            return "Pairing failed · tap to try again";
        case HIBY_BT_ERR_CANCELED: return "Not paired · tap to pair";
        case HIBY_BT_ERR_NO_AUDIO: return "No audio came through · tap to retry";
        default: return "Didn't answer · tap to try again";
    }
}

/* ------------------------------------------------- the simulator's stack */
#if !(defined(HIBY_LINUX) && !defined(SIMULATOR))
static uint32_t sl_bt_demo_ask_at;
static bool sl_bt_demo_answer_ok(void)
{
    if (!sl_bt_demo_ask_kind) return true;
    int k = sl_bt_demo_ask_kind;
    bool ok = k == HIBY_BT_ASK_SHOW_PASSKEY || k == HIBY_BT_ASK_SHOW_PIN ||
              (sl_bt_demo_answered && sl_bt_demo_answer_yes &&
               (k == HIBY_BT_ASK_CONFIRM || !strcmp(sl_bt_demo_answer_code, "1234")));
    if (!ok) sl_bt_demo_err = sl_bt_demo_answered && !sl_bt_demo_answer_yes ? HIBY_BT_ERR_CANCELED : HIBY_BT_ERR_PAIR_FAILED;
    return ok;
}
/* A job runs when the pretend stack is ready: never while it "hangs",
 * after sl_bt_demo_delay_ms, and a pairing only once its question has
 * been answered (or shown long enough, for a code typed over there). */
static void sl_bt_demo_step(void)
{
    if (!sl_bt_busy || sl_bt_job_done || sl_bt_demo_hang) return;
    if (lv_tick_elaps(sl_bt_job_started) < sl_bt_demo_delay_ms) return;
    if (sl_bt_job == SL_BT_JOB_CONNECT && !sl_bt_job_device.paired && sl_bt_demo_ask_kind)
    {
        if (sl_bt_demo_ask.kind == HIBY_BT_ASK_NONE && !sl_bt_demo_answered)
        {
            sl_bt_demo_ask.kind = sl_bt_demo_ask_kind;
            sl_bt_demo_ask.serial++;
            strlcpy(sl_bt_demo_ask.mac, sl_bt_job_device.mac, sizeof sl_bt_demo_ask.mac);
            strlcpy(sl_bt_demo_ask.name, sl_bt_job_device.name, sizeof sl_bt_demo_ask.name);
            strlcpy(sl_bt_demo_ask.code, sl_bt_demo_ask_kind == HIBY_BT_ASK_CONFIRM ? "123456" :
                    sl_bt_demo_ask_kind == HIBY_BT_ASK_SHOW_PASSKEY ? "654321" :
                    sl_bt_demo_ask_kind == HIBY_BT_ASK_SHOW_PIN ? "0000" : "", sizeof sl_bt_demo_ask.code);
            sl_bt_demo_ask_at = lv_tick_get();
            return;
        }
        bool shown = sl_bt_demo_ask_kind == HIBY_BT_ASK_SHOW_PASSKEY || sl_bt_demo_ask_kind == HIBY_BT_ASK_SHOW_PIN;
        if (!sl_bt_demo_answered && !(shown && lv_tick_elaps(sl_bt_demo_ask_at) > 1500)) return;
    }
    sl_bt_worker(NULL);
    sl_bt_demo_ask.kind = HIBY_BT_ASK_NONE;
    sl_bt_demo_answered = false;
}
#endif

/* ------------------------------------------------------------ pairing */
/* A passkey to compare, a PIN or passkey to type, or one to type on the
 * other device (#168): a page of its own while the pairing waits. */
static lv_obj_t *sl_bt_ask_page_obj, *sl_bt_ask_ta;
static struct hiby_bt_ask sl_bt_ask_now;
static unsigned sl_bt_ask_shown;
static void sl_bt_ask_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sl_bt_ask_page_obj) return;
    /* closed by Back: a question left unanswered is a No */
    if (sl_bt_ask_now.kind == HIBY_BT_ASK_CONFIRM || sl_bt_ask_now.kind == HIBY_BT_ASK_PIN ||
        sl_bt_ask_now.kind == HIBY_BT_ASK_PASSKEY)
        sl_bt_pair_answer(sl_bt_ask_now.serial, false, NULL);
    sl_bt_ask_page_obj = sl_bt_ask_ta = NULL;
    sl_bt_ask_now.kind = HIBY_BT_ASK_NONE;
}
static void sl_bt_ask_close(void)
{
    if (!sl_bt_ask_page_obj) return;
    sl_bt_ask_now.kind = HIBY_BT_ASK_NONE;   /* answered or over: Back is no No now */
    if (npages && pages[npages - 1] == sl_bt_ask_page_obj) page_pop();
}
static void sl_bt_ask_yes_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_bt_ask_page_obj) return;
    const char *code = sl_bt_ask_ta ? lv_textarea_get_text(sl_bt_ask_ta) : NULL;
    if (sl_bt_ask_ta && (!code || !code[0])) { toast("Type the code first"); return; }
    sl_log("bluetooth: pairing answered yes");
    sl_bt_pair_answer(sl_bt_ask_now.serial, true, code);
    sl_bt_ask_close();
}
static void sl_bt_ask_no_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_bt_ask_page_obj) return;
    sl_log("bluetooth: pairing answered no");
    sl_bt_pair_answer(sl_bt_ask_now.serial, false, NULL);
    sl_bt_ask_close();
}
static void wf_kb_build(lv_obj_t *p, lv_obj_t *ta, int kind, lv_event_cb_t enter, const char *enter_label);
static void wf_kb_digits(void);
/* the code itself, large, centred in its card: the special part here */
static void sl_bt_code_row(lv_obj_t *g, const char *code)
{
    lv_obj_t *r = box(g);
    lv_obj_set_size(r, lv_pct(100), 56 + 2 * (SL_GUTTER + SL_SP_XS) + SL_GUTTER);   /* the code, a row's air above and below */
    lv_obj_t *l = text(r, code, &ss_bold_56, C_TEXT);
    lv_obj_set_style_text_letter_space(l, 6, 0);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_LTR, 0);     /* digits read left to right in every language */
    lv_obj_center(l);
}
static void sl_bt_pair_page(const struct hiby_bt_ask *a)
{
    sl_bt_ask_now = *a;
    char line[200];
    const char *who = a->name[0] ? a->name : a->mac;
    bool confirm = a->kind == HIBY_BT_ASK_CONFIRM, type = a->kind == HIBY_BT_ASK_PIN || a->kind == HIBY_BT_ASK_PASSKEY;
    lv_obj_t *page, *body = page_new("Pairing", &page);
    sl_bt_ask_page_obj = page;
    sl_bt_ask_ta = NULL;
    lv_obj_add_event_cb(page, sl_bt_ask_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, type ? (a->kind == HIBY_BT_ASK_PIN ? "PIN" : "PASSKEY") :
                                  confirm ? "PAIRING CODE" : "TYPE THIS CODE");
    if (type)
    {
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        sl_bt_ask_ta = set_field(g);
        lv_textarea_set_max_length(sl_bt_ask_ta, a->kind == HIBY_BT_ASK_PIN ? 16 : 6);
        lv_textarea_set_accepted_chars(sl_bt_ask_ta, a->kind == HIBY_BT_ASK_PASSKEY ? "0123456789" : NULL);
        snprintf(line, sizeof line, sl_tr(a->kind == HIBY_BT_ASK_PIN ?
                 "Type the PIN %s shows, or the one in its manual (often 0000 or 1234)." :
                 "Type the six digits %s shows."), who);
    }
    else
    {
        sl_bt_code_row(g, a->code);
        snprintf(line, sizeof line, sl_tr(confirm ? "Check that %s shows the same code, then pair." :
                 "Type this code on %s, then press its Enter or OK key."), who);
    }
    if (type) opt_note(body, line);       /* under a field: at its edge */
    else opt_note_centred(body, line);    /* under the code: centred with it */
    if (confirm || type)
    {
        g = set_group(body, NULL);
        lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
        set_action(g, LV_SYMBOL_OK, 0x2d7ff0, "Pair", NULL, sl_bt_ask_yes_cb, NULL);
        /* a code to compare: No beside Yes; a code to type: Back is No
         * (the keyboard covers anything lower, as on the Wi-Fi password) */
        if (confirm) set_action(g, LV_SYMBOL_CLOSE, 0x66738a, "Don't pair", NULL, sl_bt_ask_no_cb, NULL);
    }
    if (type)
    {
        wf_kb_build(page, sl_bt_ask_ta, KB_NAME, sl_bt_ask_yes_cb, "Pair");
        wf_kb_digits();
    }
    page_push(page);
}
static void sl_bt_ask_poll(void)
{
    struct hiby_bt_ask a;
    bool any = sl_bt_pair_question(&a);
    if (any && a.serial != sl_bt_ask_shown)
    {
        sl_bt_ask_shown = a.serial;
        sl_log("bluetooth: pairing asks (kind %d)", a.kind);
        sl_bt_ask_close();
        sl_bt_pair_page(&a);
    }
    else if (!any && sl_bt_ask_page_obj)
        sl_bt_ask_close();               /* answered, or the pairing is over */
}

/* ------------------------------------------------------ reconnecting */
/* The last headphones are asked for again after the player starts, after
 * the screen wakes, after Bluetooth comes on, and when they drop out of
 * range: at 2, 5, 10, 20, 40 and 60 s, then every 2 minutes, for 15
 * minutes. A disconnect asked for here, or turning Bluetooth off, stops
 * it. Each try is a job on the worker; nothing waits on the UI thread. */
static const uint16_t sl_bt_rc_wait_s[] = { 2, 5, 10, 20, 40, 60 };
#define SL_BT_RC_LATER_S 120
#define SL_BT_RC_GIVE_UP_MS (15 * 60 * 1000)
static struct { bool armed; int tries; uint32_t next, since; } sl_bt_rc;
static lv_timer_t *sl_bt_rc_timer;
static void sl_bt_rc_tick(lv_timer_t *t);
static void sl_bt_rc_arm(const char *why)
{
    char mac[18];
    if (sl_net_wireless_unsupported() || sl_btrx_on || !sl_bt_last_device(mac)) return;
    if (sl_bt_routed_mac()) return;
    uint32_t now = lv_tick_get();
    if (!sl_bt_rc.armed) sl_log("bluetooth: reconnecting to %s (%s)", mac, why);
    sl_bt_rc.armed = true;
    sl_bt_rc.tries = 0;
    sl_bt_rc.since = now;
    sl_bt_rc.next = now + sl_bt_rc_wait_s[0] * 1000;
    if (!sl_bt_rc_timer) sl_bt_rc_timer = lv_timer_create(sl_bt_rc_tick, 1000, NULL);
    lv_timer_resume(sl_bt_rc_timer);
    if (sl_bt_page_obj) sl_bt_refresh_soon();
}
static void sl_bt_rc_disarm(const char *why)
{
    if (sl_bt_rc.armed) sl_log("bluetooth: reconnecting stopped (%s) after %d tries", why, sl_bt_rc.tries);
    sl_bt_rc.armed = false;
    if (sl_bt_rc_timer) lv_timer_pause(sl_bt_rc_timer);
    if (sl_bt_page_obj) sl_bt_refresh_soon();
}
static void sl_bt_rc_tick(lv_timer_t *t)
{
    (void)t;
    if (!sl_bt_rc.armed) { lv_timer_pause(t); return; }
    if (sl_bt_routed_mac()) { sl_bt_rc_disarm("connected"); return; }
    uint32_t now = lv_tick_get();
    if ((int32_t)(now - sl_bt_rc.next) < 0) return;
    if (now - sl_bt_rc.since > SL_BT_RC_GIVE_UP_MS) { sl_bt_rc_disarm("15 minutes without them"); return; }
    if (sl_bt_busy || sl_bt_scanning || sl_btrx_on) { sl_bt_rc.next = now + 1000; return; }
    sl_bt_begin(SL_BT_JOB_RECONNECT, "");
}
static void sl_bt_rc_result(const struct sl_bt_result *r, bool was_routed)
{
    (void)was_routed;
    if (r->rc > 0) { sl_bt_rc_disarm("connected"); return; }
    if (r->rc < 0) { sl_bt_rc_disarm("nothing to try"); return; }
    if (!sl_bt_rc.armed) return;
    sl_bt_rc.tries++;
    int wait = sl_bt_rc.tries < (int)(sizeof sl_bt_rc_wait_s / sizeof sl_bt_rc_wait_s[0]) ?
               sl_bt_rc_wait_s[sl_bt_rc.tries] : SL_BT_RC_LATER_S;
    sl_bt_rc.next = lv_tick_get() + wait * 1000;
    sl_log("bluetooth: reconnect try %d missed, next in %d s", sl_bt_rc.tries, wait);
}
/* the screen woke: headphones switched on meanwhile are looked for */
static void sl_bt_on_wake(void)
{
    if (!sl_bt_rc.armed && sl_bt_known && sl_bt_state.enabled && !sl_bt_routed_mac()) sl_bt_rc_arm("wake");
}
/* the headphones dropped out (out of range, flat, switched off) */
static void sl_bt_on_lost(void) { sl_bt_rc_arm("link lost"); }
/* GabagoolOS started: Bluetooth left on comes back on, and the last
 * headphones are looked for. bt_init leaves the adapter powered off. */
static void sl_bt_startup(void)
{
    if (sl_net_wireless_unsupported() || sl_bt_suspended_now()) return;
    sl_log("bluetooth: left on, powering on and reconnecting");
    sl_bt_job_power = true;
    sl_bt_quiet_power = true;
    sl_bt_begin(SL_BT_JOB_POWER, "");
}

/* ---------------------------------------------- a device, names, SBC */
static lv_obj_t *sl_bt_name_ta;
static char sl_bt_name_mac[18];          /* "" for the player's own name */
static void sl_bt_name_save_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_bt_name_ta) return;
    if (sl_bt_busy) { toast("Bluetooth is busy. Try again in a moment."); return; }
    strlcpy(sl_bt_job_name, sl_ta_text(sl_bt_name_ta), sizeof sl_bt_job_name);
    pl_utf8_trim(sl_bt_job_name);
    strlcpy(sl_bt_job_mac, sl_bt_name_mac, sizeof sl_bt_job_mac);
    bool device = sl_bt_name_mac[0] != '\0';
    /* shown at once; the stack is told on the worker */
    for (int i = 0; device && i < sl_bt_count; i++)
        if (!strcasecmp(sl_bt_found[i].mac, sl_bt_name_mac) && sl_bt_job_name[0])
            strlcpy(sl_bt_found[i].name, sl_bt_job_name, sizeof sl_bt_found[i].name);
    sl_bt_begin(device ? SL_BT_JOB_RENAME : SL_BT_JOB_NAME, "Saving the name…");
    if (sl_bt_page_obj) page_bring_back(sl_bt_page_obj);
    else page_pop();
}
static void sl_bt_name_deleted(lv_event_t *e) { (void)e; sl_bt_name_ta = NULL; }
static void sl_bt_name_page(const char *mac, const char *now)
{
    strlcpy(sl_bt_name_mac, mac ? mac : "", sizeof sl_bt_name_mac);
    lv_obj_t *p, *body = page_new(mac ? "Rename" : "Player name", &p);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p, sl_bt_name_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, "NAME");
    sl_bt_name_ta = set_field(g);
    lv_textarea_set_max_length(sl_bt_name_ta, HIBY_BT_ALIAS_LEN - 1);
    lv_textarea_set_text(sl_bt_name_ta, now ? now : "");
    opt_note(body, mac ? "Shown on this player. Leave it empty to use the name the device gives itself." :
                         "What phones, cars and other players see when they look for Bluetooth, here and while you receive from your phone.");
    g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action(g, LV_SYMBOL_OK, 0x2d7ff0, "Save", NULL, sl_bt_name_save_cb, NULL);
    wf_kb_build(p, sl_bt_name_ta, KB_NAME, sl_bt_name_save_cb, "Save");
    page_push(p);
}
static void sl_bt_player_name_cb(lv_event_t *e)
{
    (void)e;
    char name[HIBY_BT_ALIAS_LEN];
    sl_bt_player_name(name, sizeof name);
    sl_bt_name_page(NULL, name);
}
static const char *sl_bt_shown_player_name(void)
{
    static char name[HIBY_BT_ALIAS_LEN];
    sl_bt_player_name(name, sizeof name);
    return name[0] ? name : "HiBy R1";
}

static const char *const sl_bt_sbc_names[] = { "Standard", "XQ", "XQ+" };
static lv_obj_t *sl_bt_sbc_rows[3];
static void sl_bt_sbc_cb(lv_event_t *e)
{
    int q = (int)(intptr_t)lv_event_get_user_data(e);
    if (sl_bt_busy) { toast("Bluetooth is busy. Try again in a moment."); return; }
    if (q == sl_bt_sbc_quality()) return;
    if (sl_bt_sbc_rows[q]) set_check_mark(sl_bt_sbc_rows[q]);
    sl_bt_job_sbc = q;
    sl_bt_begin(SL_BT_JOB_SBC, "Restarting Bluetooth audio…");
}
static void sl_bt_sbc_deleted(lv_event_t *e) { (void)e; memset(sl_bt_sbc_rows, 0, sizeof sl_bt_sbc_rows); }
static void sl_bt_sbc_page(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("SBC quality", &p);
    lv_obj_add_event_cb(p, sl_bt_sbc_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_t *g = set_group(body, "QUALITY");
    static const char *const rows[] = { "Standard", "XQ · up to 452 kb/s", "XQ+ · up to 551 kb/s" };
    int q = sl_bt_sbc_quality();
    for (int i = 0; i < 3; i++)
        sl_bt_sbc_rows[i] = set_check(g, NULL, 0, rows[i], i == q, sl_bt_sbc_cb, (void *)(intptr_t)i);
    opt_note(body, "For headphones and speakers that use SBC (not AAC, aptX or LDAC). Ones that can't take XQ play at standard quality. Bluetooth audio restarts for a moment when you change it.");
    page_push(p);
}

static int sl_bt_device_index;
static void sl_bt_device_rename_cb(lv_event_t *e)
{
    (void)e;
    if (sl_bt_device_index < 0 || sl_bt_device_index >= sl_bt_count) return;
    sl_bt_name_page(sl_bt_found[sl_bt_device_index].mac, sl_bt_found[sl_bt_device_index].name);
}
static void sl_bt_forget_cb(lv_event_t *e);
static void sl_bt_disconnect_cb(lv_event_t *e);
/* a paired device: its name, rename it, let it go, forget it */
static void sl_bt_device_page_cb(lv_event_t *e)
{
    if (sl_bt_busy || sl_bt_scanning) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= sl_bt_count) return;
    sl_bt_device_index = i;
    const struct hiby_bt_device *d = &sl_bt_found[i];
    bool linked = sl_bt_state.connected && !strcasecmp(sl_bt_state.mac, d->mac);
    lv_obj_t *p, *body = page_new(d->name, &p);
    lv_obj_t *g = set_group(body, "DEVICE");
    char line[96];
    snprintf(line, sizeof line, "%s · %s", d->mac, sl_tr(linked ? "Connected" : "Paired"));
    set_info(g, LV_SYMBOL_BLUETOOTH, 0x2d7ff0, d->name, line, true);
    set_action(g, LV_SYMBOL_EDIT, 0xf0602f, "Rename", NULL, sl_bt_device_rename_cb, NULL);
    if (linked) set_action(g, LV_SYMBOL_CLOSE, 0x66738a, "Disconnect", NULL, sl_bt_disconnect_cb, NULL);
    g = set_group(body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_danger(g, LV_SYMBOL_TRASH, "Forget device", sl_bt_forget_cb, (void *)(intptr_t)i);
    opt_note(body, "Forgetting removes the pairing. To use it again, put it in pairing mode and scan.");
    page_push(p);
}

static void sl_bt_power_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_current_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (sl_bt_busy || !sl_bt_known)
    {
        /* not now: show the truth again rather than a switch that lies */
        if (sl_bt_state.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(sw, LV_STATE_CHECKED);
        return;
    }
    sl_bt_job_power = on;
    sl_bt_begin(SL_BT_JOB_POWER, on ? "Turning on Bluetooth…" : "Turning off Bluetooth…");
}

static void sl_bt_device_cb(lv_event_t *e)
{
    if (sl_bt_busy || sl_bt_scanning) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= sl_bt_count) return;
    if (sl_bt_state.routed && !strcasecmp(sl_bt_state.mac, sl_bt_found[index].mac))
    {
        sl_bt_begin(SL_BT_JOB_DISCONNECT, "Disconnecting…");
        return;
    }
    sl_bt_job_device = sl_bt_found[index];
    sl_bt_begin(SL_BT_JOB_CONNECT,
                sl_bt_job_device.paired ? "Connecting…" : "Pairing and connecting…");
}

static void sl_bt_disconnect_cb(lv_event_t *e)
{
    (void)e;
    if (sl_bt_busy) return;
    sl_bt_begin(SL_BT_JOB_DISCONNECT, "Disconnecting…");
}

static void sl_bt_forget_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (sl_bt_busy) return;
    strlcpy(sl_bt_job_mac, sl_bt_forget_mac, sizeof sl_bt_job_mac);
    sl_bt_begin(SL_BT_JOB_FORGET, "Forgetting device…");
}

static void sl_bt_forget_keep_cb(lv_event_t *e) { (void)e; page_pop(); }
static void sl_bt_forget_cb(lv_event_t *e)
{
    if (sl_bt_busy || sl_bt_scanning) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= sl_bt_count) return;
    strlcpy(sl_bt_forget_mac, sl_bt_found[index].mac, sizeof sl_bt_forget_mac);
    strlcpy(sl_bt_forget_name, sl_bt_found[index].name, sizeof sl_bt_forget_name);
    lv_obj_t *page, *body = page_new("Forget device?", &page);
    set_confirm(body, "REMOVE PAIRING", sl_bt_forget_name, "Keep it", "You can pair this device again later.",
                sl_bt_forget_keep_cb, "Forget device", "Remove it from this player", sl_bt_forget_confirm_cb, true);
    page_push(page);
}

static void sl_bt_codec_cb(lv_event_t *e)
{
    if (sl_bt_busy) return;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= sl_bt_codec_count) return;
    strlcpy(sl_bt_job_mac, sl_bt_state.mac, sizeof sl_bt_job_mac);
    strlcpy(sl_bt_job_codec, sl_bt_codecs_found[index], sizeof sl_bt_job_codec);
    sl_bt_begin(SL_BT_JOB_CODEC, "Changing audio codec…");
}

static void sl_bt_scan_tick(lv_timer_t *timer)
{
    if (!sl_bt_scanning) { lv_timer_pause(timer); return; }
    uint32_t now = lv_tick_get();
    if ((int32_t)(now - sl_bt_scan_until) >= 0)
    {
        if (sl_bt_busy) return;
        lv_timer_pause(timer);
        sl_bt_begin(SL_BT_JOB_SCAN_FINISH, "Finishing device scan…");
        return;
    }
    if (sl_bt_scan_label)
    {
        char message[48];
        snprintf(message, sizeof message, "Looking nearby · %lus left",
                 (unsigned long)((sl_bt_scan_until - now + 999) / 1000));
        lv_label_set_text(sl_bt_scan_label, message);
    }
}

static void sl_bt_scan_cb(lv_event_t *e)
{
    (void)e;
    if (sl_bt_busy || sl_bt_scanning) return;
    sl_bt_begin(SL_BT_JOB_SCAN, "Starting device scan…");
}

/* While the page is open, look again every few seconds: headphones that
 * connect, drop out or run flat change the state without telling us. */
static struct hiby_bt_aap sl_bt_aap_shown;
static const char *const sl_bt_noise_names[] = { "Off", "Noise cancellation", "Transparency", "Adaptive" };
static void sl_bt_noise_set(int i)
{
    sl_log("bluetooth: AirPods noise control %d asked for", i + 1);
    if (!sl_bt_aap_noise(i + 1)) toast_warn("The AirPods did not take it. Try again.");
}
/* AirPods: battery and noise control, as the AirPods report them (AAP) */
static void sl_bt_aap_rows(lv_obj_t *g)
{
    struct hiby_bt_aap a;
    sl_bt_aap_status(&a);
    sl_bt_aap_shown = a;
    if (!a.up) return;
    char line[96] = "";
    int n = 0;
    const struct { int level; bool charging; const char *what; } parts[] = {
        { a.left, a.left_charging, "Left" }, { a.right, a.right_charging, "Right" }, { a.box, a.box_charging, "Case" } };
    for (int i = 0; i < 3; i++)
        if (parts[i].level >= 0)
            n += snprintf(line + n, sizeof line - n, "%s%s %d%%%s%s", n ? " · " : "", sl_tr(parts[i].what), parts[i].level,
                          parts[i].charging ? " " : "", parts[i].charging ? sl_tr("charging") : "");
    set_info(g, LV_SYMBOL_BATTERY_FULL, 0x23804a, "Battery", n ? line : sl_tr("Not reported yet"), true);
}
static void sl_bt_watch_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!sl_bt_page_obj || pages[npages - 1] != sl_bt_page_obj || sl_power_hides_ui()) return;
    struct hiby_bt_aap a;
    sl_bt_aap_status(&a);
    if (memcmp(&a, &sl_bt_aap_shown, sizeof a)) sl_bt_refresh_soon();   /* AirPods said something new */
    if (!sl_bt_busy && !sl_bt_scanning) sl_bt_begin(SL_BT_JOB_REFRESH, "");
}
static void sl_bt_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sl_bt_page_obj) return;
    sl_bt_page_obj = sl_bt_body = sl_bt_busy_label = sl_bt_scan_label = sl_bt_switch = NULL;
    if (sl_bt_refresh_timer) lv_timer_pause(sl_bt_refresh_timer);
    if (sl_bt_watch_timer) lv_timer_pause(sl_bt_watch_timer);
}

static void sl_bt_rebuild(void)
{
    if (!sl_bt_page_obj || !sl_bt_body) return;
    int previous_scroll = lv_obj_get_scroll_y(sl_bt_body);
    lv_obj_clean(sl_bt_body);
    sl_bt_busy_label = sl_bt_scan_label = NULL;
    sl_bt_switch = NULL;
    if (sl_net_wireless_unsupported()) { sl_wireless_gone(sl_bt_body, IC_BLUETOOTH, "Bluetooth isn't supported on this R1 yet"); return; }

    /* the switch, and what Bluetooth is doing, in Settings' own rows */
    lv_obj_t *g = set_group(sl_bt_body, NULL);
    lv_obj_t *power = set_switch_ex(g, LV_SYMBOL_BLUETOOTH, 0x2d7ff0, "Bluetooth",
        sl_btrx_on ? "Receiving from your phone" :
        !sl_bt_known ? "Checking Bluetooth…" :
        sl_bt_scanning ? "Looking for nearby devices" :
        sl_bt_state.connected ? "Connected and ready to listen" :
        sl_bt_rc.armed && sl_bt_state.enabled ? "Looking for your headphones" :
        sl_bt_state.suspended ? "Radio is sleeping" :
        sl_bt_state.enabled ? "Ready to connect" : "Turn on to find headphones", sl_bt_state.enabled, NULL);
    sl_bt_switch = power;
    sl_bt_busy_label = lv_obj_get_child(lv_obj_get_parent(power), 1);
    label_lines(sl_bt_busy_label, 1);
    if (sl_bt_busy || !sl_bt_known) lv_obj_add_state(power, LV_STATE_DISABLED);
    lv_obj_add_event_cb(power, sl_bt_power_cb, LV_EVENT_VALUE_CHANGED, NULL);

    g = set_group(sl_bt_body, "AUDIO OUTPUT");
    set_info(g, sl_bt_state.routed ? LV_SYMBOL_AUDIO : LV_SYMBOL_VOLUME_MAX, sl_bt_state.routed ? 0x2d7ff0 : 0xf0602f,
             sl_bt_state.routed ? sl_bt_state.name : "This player", NULL, false);

    if (sl_bt_state.connected)
    {
        g = set_group(sl_bt_body, "CONNECTED");
        char subtitle[120];
        snprintf(subtitle, sizeof subtitle, "%s · %s", sl_bt_state.mac,
                 sl_bt_state.codec[0] ? sl_bt_state.codec : "Bluetooth audio");
        set_info(g, LV_SYMBOL_BLUETOOTH, 0x2d7ff0, sl_bt_state.name, subtitle, true);   /* white reads on it: the accent may be pale */
        sl_bt_aap_rows(g);
        set_action(g, LV_SYMBOL_CLOSE, 0x66738a, "Disconnect", NULL, sl_bt_disconnect_cb, NULL);
        if (sl_bt_aap_shown.up)
        {
            g = set_group(sl_bt_body, "NOISE CONTROL");
            opt_chips(g, sl_bt_noise_names, 4, sl_bt_aap_shown.noise >= 1 ? sl_bt_aap_shown.noise - 1 : -1, sl_bt_noise_set);
        }
    }

    g = set_group(sl_bt_body, "DEVICES");
    lv_obj_t *scan = set_action(g, LV_SYMBOL_REFRESH, 0x2d7ff0, sl_bt_scanning ? "Looking nearby · 15s left" :
                                "Scan for devices", NULL, sl_bt_scanning ? NULL : sl_bt_scan_cb, NULL);
    sl_bt_scan_label = lv_obj_get_child(scan, 0);
    if (sl_bt_scanning && !sl_zune()) lv_obj_add_flag(lv_obj_get_child(scan, 2), LV_OBJ_FLAG_HIDDEN);   /* no chevron while it looks */

    for (int i = 0; i < sl_bt_count; i++)
    {
        bool linked = sl_bt_state.connected && !strcasecmp(sl_bt_state.mac, sl_bt_found[i].mac);
        bool active = linked && sl_bt_state.routed;
        char last[18] = "";
        bool failed = !active && !strcasecmp(sl_bt_err_mac, sl_bt_found[i].mac);
        bool looking = !linked && sl_bt_rc.armed && sl_bt_last_device(last) && !strcasecmp(last, sl_bt_found[i].mac);
        lv_obj_t *row = set_action_sub(g, LV_SYMBOL_BLUETOOTH,
                                       active ? C_ACCENT_UI : sl_bt_found[i].paired ? 0x2d7ff0 : 0x66738a,
                                       sl_bt_found[i].name,
                                       active ? "Connected · tap to disconnect" :
                                       linked ? "Connected · tap to play here" :
                                       looking ? "Reconnecting · tap to connect now" :
                                       failed ? sl_bt_err_short(sl_bt_err_code) :
                                       sl_bt_found[i].paired ? "Paired · tap to connect" :
                                       "Nearby · tap to pair", sl_bt_device_cb, (void *)(intptr_t)i);
        /* its own page: rename, disconnect, forget */
        if (sl_bt_found[i].paired) set_row_button(row, 3, LV_SYMBOL_SETTINGS, sl_bt_device_page_cb, (void *)(intptr_t)i);
    }
    if (!sl_bt_count)
        opt_note(sl_bt_body, sl_bt_state.enabled ? "No devices yet. Start a scan to find your headphones." :
                                                   "Turn on Bluetooth to see your devices.");

    if (sl_bt_state.connected && sl_bt_codec_count > 0)
    {
        g = set_group(sl_bt_body, "AUDIO CODEC");
        for (int i = 0; i < sl_bt_codec_count; i++)
        {
            bool current = !strcasecmp(sl_bt_state.codec, sl_bt_codecs_found[i]);
            /* each in the colour the status bar gives it */
            uint32_t tint = bt_codec_tint(sl_bt_codecs_found[i], true);
            set_check(g, LV_SYMBOL_BLUETOOTH, tint ? tint : 0x66738a, sl_bt_codecs_found[i], current,
                      current ? NULL : sl_bt_codec_cb, (void *)(intptr_t)i);
        }
    }
    g = set_group(sl_bt_body, "THIS PLAYER");
    set_action(g, LV_SYMBOL_EDIT, 0xf0602f, "Name", sl_bt_shown_player_name(), sl_bt_player_name_cb, NULL);
    set_action(g, LV_SYMBOL_AUDIO, 0x2d7ff0, "SBC quality", sl_bt_sbc_names[sl_bt_sbc_quality() % 3], sl_bt_sbc_page, NULL);
    g = set_group(sl_bt_body, "PHONE AUDIO");
    set_link_sub(g, LV_SYMBOL_DOWNLOAD, 0x8b6fd6, "Receive from phone",
                 sl_btrx_on ? "Receiving · tap to open" : "Play your phone through this player", sl_btrx_on ? "On" : NULL,
                 sl_btrx_card_cb, NULL);
    lv_obj_update_layout(sl_bt_body);
    lv_obj_scroll_to_y(sl_bt_body, previous_scroll, LV_ANIM_OFF);
}

static void sl_bt_page(void)
{
    if (page_bring_back(sl_bt_page_obj)) return;   /* once on the stack: a tap from the control centre or Now Playing goes back to it, not nowhere */
    lv_obj_t *page, *body = page_new("Bluetooth", &page);
    sl_bt_page_obj = page;
    sl_bt_body = body;
    lv_obj_add_event_cb(page, sl_bt_deleted, LV_EVENT_DELETE, NULL);
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
    /* Audio going to a device is known at once, without asking BlueZ: never
     * open on "off" while headphones play. The re-check fills in the rest. */
    if (bt_is_routed_fast() && !sl_bt_state.connected)
    {
        sl_bt_state.enabled = sl_bt_state.connected = sl_bt_state.routed = true;
        sl_bt_state.suspended = false;
        if (!sl_bt_state.name[0]) strlcpy(sl_bt_state.name, "Bluetooth audio", sizeof sl_bt_state.name);
        sl_bt_known = true;
    }
#endif
    sl_bt_rebuild();
    page_push(page);
    if (!sl_bt_busy && !sl_bt_scanning)
        sl_bt_begin(SL_BT_JOB_REFRESH, "Checking Bluetooth…");
    if (!sl_bt_watch_timer) sl_bt_watch_timer = lv_timer_create(sl_bt_watch_cb, 4000, NULL);
    lv_timer_reset(sl_bt_watch_timer);
    lv_timer_resume(sl_bt_watch_timer);
}

/* ------------------------------------------------ Receive from phone */
/* The phone's audio goes into the mixer's voice channel, as AirPlay's,
 * with the player's volume after it; resampled from the phone's rate to
 * whatever the mixer runs at. Local playback pauses while it plays. */
static bool ap_active;                   /* AirPlay has the voice channel (sloppy_wifi.inc) */
#define SL_BTRX_OUT 1024
static int16_t sl_btrx_out[SL_BTRX_OUT * 2], sl_btrx_in[SL_BTRX_OUT * 2 * 5];
static int16_t sl_btrx_prev[2], sl_btrx_next[2];
static uint32_t sl_btrx_pos, sl_btrx_step;
static bool sl_btrx_active, sl_btrx_held_off, sl_btrx_dry;
static unsigned sl_btrx_underruns;      /* gaps the audio came back from */
static int sl_btrx_volume_used = -2;
static struct hiby_btrx_status sl_btrx;
static unsigned sl_btrx_seen_connects;
static lv_timer_t *sl_btrx_timer;
static lv_obj_t *sl_btrx_page_obj, *sl_btrx_body;
static bool sl_btrx_shown_active;
static char sl_btrx_last_player[16];
static unsigned sl_btrx_heard;           /* the connection the audio last came from */
static void sl_btrx_rebuild(void);

static void sl_btrx_get_more(const void **start, size_t *size)
{
    uint32_t pops = (uint32_t)(((uint64_t)sl_btrx_pos + (uint64_t)sl_btrx_step * (SL_BTRX_OUT - 1)) >> 16);
    if (pops > SL_BTRX_OUT * 5) pops = SL_BTRX_OUT * 5;
    int got = sl_btrx_read(sl_btrx_in, (int)pops), used = 0;
    if ((uint32_t)got < pops) sl_btrx_dry = true;
    else if (sl_btrx_dry) { sl_btrx_dry = false; sl_btrx_underruns++; }
    for (int i = 0; i < SL_BTRX_OUT; i++)
    {
        while (sl_btrx_pos >= 65536)
        {
            sl_btrx_prev[0] = sl_btrx_next[0];
            sl_btrx_prev[1] = sl_btrx_next[1];
            if (used < got) { sl_btrx_next[0] = sl_btrx_in[used * 2]; sl_btrx_next[1] = sl_btrx_in[used * 2 + 1]; used++; }
            else sl_btrx_next[0] = sl_btrx_next[1] = 0;      /* ran dry: silence */
            sl_btrx_pos -= 65536;
        }
        for (int c = 0; c < 2; c++)
            sl_btrx_out[i * 2 + c] = (int16_t)(sl_btrx_prev[c] +
                (((int32_t)(sl_btrx_next[c] - sl_btrx_prev[c]) * (int32_t)sl_btrx_pos) >> 16));
        sl_btrx_pos += sl_btrx_step;
    }
    *start = sl_btrx_out;
    *size = sizeof sl_btrx_out;
}

/* The phone's own volume slider (AVRCP absolute volume, 0..127) as gain,
 * squared so it feels like the phone's steps; the player's keys stay the
 * player's own volume on top. */
static unsigned sl_btrx_amp(int volume)
{
    if (volume < 0 || volume >= 127) return MIX_AMP_UNITY;
    return (unsigned)((uint64_t)MIX_AMP_UNITY * (unsigned)(volume * volume) / (127 * 127));
}

static void sl_btrx_stream(void)
{
    bool connected = sl_btrx.state == HIBY_BTRX_CONNECTED;
    unsigned idle = sl_btrx_idle_ms();
    /* the phone pressed play after the player's own music took over */
    if (sl_btrx_held_off && strcmp(sl_btrx.player, sl_btrx_last_player) && !strcmp(sl_btrx.player, "playing"))
        sl_btrx_held_off = false;
    strlcpy(sl_btrx_last_player, sl_btrx.player, sizeof sl_btrx_last_player);
    if (!sl_btrx_active && connected && idle < 500 && sl_btrx_buffered() >= 2048 && !sl_btrx_held_off && !ap_active)
    {
        if (is_playing()) audio_pause();          /* the phone takes over */
        uint32_t fs = mixer_get_frequency();
        if (fs < 8000) fs = 44100;
        int rate = sl_btrx.rate >= 8000 ? sl_btrx.rate : 44100;
        sl_btrx_step = (uint32_t)(((uint64_t)rate << 16) / fs);
        sl_btrx_pos = 65536;
        sl_btrx_prev[0] = sl_btrx_prev[1] = sl_btrx_next[0] = sl_btrx_next[1] = 0;
        sl_btrx_dry = false;
        sl_btrx_underruns = 0;
        sl_btrx_volume_used = sl_btrx.volume;
        const void *start;
        size_t size;
        sl_btrx_get_more(&start, &size);
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, sl_btrx_amp(sl_btrx.volume));
        mixer_channel_play_data(PCM_MIXER_CHAN_VOICE, sl_btrx_get_more, start, size);
        sl_btrx_active = true;
        sl_btrx_heard = sl_btrx.connects;
        sl_log("btrx: stream started, phone %d Hz into the mixer at %lu Hz, gain %u%%", rate,
               (unsigned long)fs, sl_btrx_amp(sl_btrx.volume) * 100 / MIX_AMP_UNITY);
    }
    else if (sl_btrx_active && (!connected || idle > 3000 || is_playing()))
    {
        mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
        sl_btrx_active = false;
        sl_btrx_held_off = connected && idle <= 3000;    /* someone pressed play here */
        sl_log("btrx: stream stopped (%s), %u underruns", !connected ? "phone gone" :
               idle > 3000 ? "phone paused" : "music played here", sl_btrx_underruns);
    }
    if (sl_btrx_held_off && idle > 3000) sl_btrx_held_off = false;
    if (!sl_btrx_active) return;
    reset_poweroff_timer();               /* no Rockbox playback: keep the player awake */
    if (sl_btrx.volume != sl_btrx_volume_used)
    {
        sl_btrx_volume_used = sl_btrx.volume;
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, sl_btrx_amp(sl_btrx.volume));
        sl_log("btrx: phone volume %d/127, gain %u%%", sl_btrx.volume,
               sl_btrx_amp(sl_btrx.volume) * 100 / MIX_AMP_UNITY);
    }
}

static void sl_btrx_tick(lv_timer_t *timer)
{
    char line[128];
    while (sl_btrx_take_event(line, sizeof line)) sl_log("btrx: %s", line);
    struct hiby_btrx_status s;
    sl_btrx_get(&s);
    bool running = sl_btrx_running();
    bool changed = memcmp(&s, &sl_btrx, sizeof s) != 0;
    sl_btrx = s;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    sl_bt_kick_audio();                  /* headphones let go at the start pause the song */
#endif
    if (s.state == HIBY_BTRX_CONNECTED && s.connects != sl_btrx_seen_connects)
    {
        static char said[HIBY_BT_NAME_LEN + 16];
        sl_btrx_seen_connects = s.connects;
        snprintf(said, sizeof said, "%s %s", s.name, sl_tr("connected"));
        notify_ex(NS_BT, NOTE_INFO, 0, NA_BT, "bluetooth", said);
    }
    if (running) sl_btrx_stream();
    else
    {
        static char said[HIBY_BT_NAME_LEN + 64];
        if (sl_btrx_active)
        {
            mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
            sl_btrx_active = false;
            sl_log("btrx: stream stopped (session over), %u underruns", sl_btrx_underruns);
        }
        sl_btrx_on = false;
        sl_btrx_held_off = false;
        lv_timer_pause(timer);
        bool failed = s.error[0] && strcmp(s.error, "Phone disconnected") && strcmp(s.error, "No phone connected");
        if (!strcmp(s.error, "Phone disconnected"))
            snprintf(said, sizeof said, "%s %s", s.name[0] ? s.name : sl_tr("Phone"), sl_tr("disconnected. Bluetooth is back to normal."));
        else if (s.error[0])
            snprintf(said, sizeof said, "%s. %s", sl_tr(s.error), sl_tr(failed ? "Bluetooth is back as it was." : "Bluetooth is back to normal."));
        else strlcpy(said, sl_tr("Stopped receiving. Bluetooth is back to normal."), sizeof said);
        sl_log("btrx: session over: %s", s.error[0] ? s.error : "stopped");
        notify_ex(NS_BT, failed ? NOTE_WARN : NOTE_INFO, 0, NA_BT, "bluetooth", said);
        changed = true;
        sl_bt_rc_arm("receive ended");   /* the headphones it let go */
        if (sl_bt_page_obj && !sl_bt_busy) sl_bt_begin(SL_BT_JOB_REFRESH, "");
        else if (sl_bt_page_obj) sl_bt_refresh_soon();
    }
    if ((changed || sl_btrx_active != sl_btrx_shown_active) && sl_btrx_page_obj) sl_btrx_rebuild();
    if (changed && qp_is_visible()) qp_refresh();
}

static void sl_btrx_begin(void)
{
    if (sl_btrx_on) return;
    /* a quiet re-check can run alongside; a connect or a scan cannot */
    if ((sl_bt_busy && sl_bt_job != SL_BT_JOB_REFRESH) || sl_bt_scanning)
    {
        toast_warn("Bluetooth is busy. Try again in a moment.");
        return;
    }
    if (!sl_btrx_start())
    {
        toast_warn("Could not start receiving. Please retry.");
        return;
    }
    sl_log("btrx: receive from phone asked for");
    sl_bt_rc_disarm("receiving from a phone");
    sl_btrx_on = true;
    sl_btrx_seen_connects = 0;
    memset(&sl_btrx, 0, sizeof sl_btrx);
    sl_btrx.state = HIBY_BTRX_STARTING;
    sl_btrx.volume = -1;
    if (!sl_btrx_timer) sl_btrx_timer = lv_timer_create(sl_btrx_tick, 100, NULL);
    lv_timer_reset(sl_btrx_timer);
    lv_timer_resume(sl_btrx_timer);
    if (sl_btrx_page_obj) sl_btrx_rebuild();
    if (sl_bt_page_obj) sl_bt_refresh_soon();
}

static void sl_btrx_start_cb(lv_event_t *e) { (void)e; sl_btrx_begin(); }
static void sl_btrx_stop_cb(lv_event_t *e)
{
    (void)e;
    if (!sl_btrx_on || sl_btrx.state == HIBY_BTRX_STOPPING) return;
    sl_log("btrx: stop asked for");
    sl_btrx_stop();
    sl_btrx.state = HIBY_BTRX_STOPPING;
    sl_btrx_rebuild();
}
static void sl_btrx_control_cb(lv_event_t *e)
{
    const char *method = lv_event_get_user_data(e);
    if (sl_btrx.state != HIBY_BTRX_CONNECTED) return;
    /* play/pause asks the phone; its audio stopping or coming back is
     * what the screen then shows */
    if (!strcmp(method, "Play") && (sl_btrx_active || !strcmp(sl_btrx.player, "playing"))) method = "Pause";
    sl_log("btrx: %s asked for", method);
    sl_btrx_control(method);
}

static void sl_btrx_codec_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (sl_btrx.state != HIBY_BTRX_CONNECTED || i < 0 || i >= sl_btrx.codec_count) return;
    sl_log("btrx: codec %s asked for", sl_btrx.codecs[i]);
    sl_btrx_codec(sl_btrx.codecs[i]);
    toast("Asking your phone to switch…");
}
static void sl_btrx_rebuild(void)
{
    if (!sl_btrx_page_obj || !sl_btrx_body) return;
    lv_obj_clean(sl_btrx_body);
    sl_btrx_shown_active = sl_btrx_active;
    const struct hiby_btrx_status *s = &sl_btrx;
    bool connected = sl_btrx_on && s->state == HIBY_BTRX_CONNECTED;
    bool stopping = sl_btrx_on && s->state == HIBY_BTRX_STOPPING;
    bool paused = connected && !sl_btrx_active && (!strcmp(s->player, "paused") ||
                                                   (sl_btrx_heard == s->connects && sl_btrx_idle_ms() > 3000));
    char line[160], visible[HIBY_BT_ALIAS_LEN + 32], seen_as[HIBY_BT_ALIAS_LEN];
    sl_bt_player_name(seen_as, sizeof seen_as);
    if (!seen_as[0]) strlcpy(seen_as, "GabagoolOS R1", sizeof seen_as);   /* what receiving calls itself unless named */
    snprintf(visible, sizeof visible, sl_tr("Visible as %s"), seen_as);

    set_header(sl_btrx_body, "PHONE AUDIO");
    /* the Bluetooth page's hero, shorter: the controls and Stop stay
     * above the mini player */
    lv_obj_t *hero = box(sl_btrx_body);
    lv_obj_set_size(hero, lv_pct(100), 132);
    lv_obj_set_style_radius(hero, SL_R_LG, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(sl_accent_on(0xffffff, 45)), 0);   /* white words read on it */
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0x17435b), 0);
    lv_obj_set_style_text_color(hero, lv_color_white(), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_t *orb = box(hero);
    lv_obj_set_size(orb, 76, 76);
    lv_obj_set_pos(orb, 20, 28);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(orb, sl_btrx_active ? LV_OPA_40 : LV_OPA_20, 0);
    lv_obj_center(text_light(orb, connected ? LV_SYMBOL_AUDIO : LV_SYMBOL_BLUETOOTH, &sl_sym_32));
    lv_obj_t *name = text_light(hero, connected ? s->name : sl_btrx_on ? "Waiting for your phone" : "Receive from phone",
                          &ss_bold_28);
    lv_obj_set_pos(name, 114, 30);
    lv_obj_set_width(name, W - 166);
    label_lines(name, 1);
    lv_obj_t *state = text_light(hero,
        stopping ? "Putting Bluetooth back…" :
        !sl_btrx_on ? "Play your phone through this player" :
        s->state == HIBY_BTRX_STARTING ? "Getting Bluetooth ready…" :
        s->state == HIBY_BTRX_WAITING ? visible :
        sl_btrx_active ? "Playing through this player" :
        paused ? "Paused on your phone" :
        sl_btrx_held_off ? "This player's music is playing" : "Connected. Play music on your phone.",
        &ss_regular_18);
    lv_obj_set_pos(state, 114, 74);
    lv_obj_set_width(state, W - 166);
    label_lines(state, 1);

    if (connected)
    {
        /* the song in Settings' rows, the phone's own controls under it */
        lv_obj_t *card = set_group(sl_btrx_body, "NOW PLAYING");
        if (s->title[0]) strlcpy(line, s->title, sizeof line);
        else snprintf(line, sizeof line, "%s %s", sl_tr("Playing from"), s->name);
        set_info(card, LV_SYMBOL_AUDIO, 0x8b6fd6, line,
                 s->artist[0] ? s->artist : sl_tr("Track details appear if the phone sends them"), true);
        int n = snprintf(line, sizeof line, "%s · %d.%d kHz", s->codec[0] ? s->codec : "Bluetooth",
                         s->rate / 1000, s->rate % 1000 / 100);
        if (s->volume >= 0)
            snprintf(line + n, sizeof line - n, " · %s %d%%", sl_tr("phone volume"), (s->volume * 100 + 63) / 127);
        set_text(card, line, C_SUB);
        lv_obj_t *row = box(card);
        lv_obj_set_size(row, lv_pct(100), 88 + 2 * SL_GUTTER);
        lv_obj_set_style_base_dir(row, LV_BASE_DIR_LTR, 0);   /* previous at the left, as on the phone */
        static const char *const methods[] = { "Previous", "Play", "Next" };
        const char *icons[] = { LV_SYMBOL_PREV, sl_btrx_active ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *b = box(row);
            int size = i == 1 ? 88 : 72;
            lv_obj_set_size(b, size, size);
            lv_obj_align(b, LV_ALIGN_CENTER, (i - 1) * 130, 0);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(i == 1 ? C_ACCENT : C_CARD_P), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_center(text(b, icons[i], i == 1 ? &sl_sym_32 : &sl_sym_24,
                               i == 1 ? sl_accent_ink() : C_TEXT));
            on_click(b, sl_btrx_control_cb, (void *)methods[i]);
        }
        /* the codec (#7): the phone picks it; asking it to switch is all a
         * receiver can do, and the page says how that went */
        if (s->codec_count > 0)
        {
            lv_obj_t *cg = set_group(sl_btrx_body, "AUDIO CODEC");
            for (int i = 0; i < s->codec_count && i < 6; i++)
            {
                bool current = !strcasecmp(s->codec, s->codecs[i]);
                set_check(cg, NULL, 0, s->codecs[i], current, current ? NULL : sl_btrx_codec_cb, (void *)(intptr_t)i);
            }
            if (s->codec_note[0])
                snprintf(line, sizeof line, sl_tr("Your phone stayed on %s. On Android, Developer options can change it."), s->codec);
            else
                strlcpy(line, sl_tr("Your phone chooses. Tap one to ask it to switch; some phones keep their own choice. iPhones only send AAC."), sizeof line);
            opt_note(sl_btrx_body, line);
        }
    }
    else if (sl_btrx_on && !stopping)
    {
        snprintf(line, sizeof line, sl_tr("1. Open Bluetooth settings.\n2. Pick %s.\n3. Play music. It comes out here."), seen_as);
        set_text(set_group(sl_btrx_body, "ON YOUR PHONE"), line, C_TEXT);
    }
    if (stopping) return;
    /* the way on, or back, as a row in its own card, as set_confirm's */
    lv_obj_t *g = set_group(sl_btrx_body, NULL);
    lv_obj_set_style_margin_top(g, SL_GUTTER + SL_SP_XS, 0);
    set_action(g, sl_btrx_on ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY, sl_btrx_on ? 0x66738a : 0x8b6fd6,
               sl_btrx_on ? "Stop receiving" : "Start receiving", NULL, sl_btrx_on ? sl_btrx_stop_cb : sl_btrx_start_cb, NULL);
    opt_note(sl_btrx_body, "Your headphones stay paired. When you stop, Bluetooth goes back to normal.");
}

static void sl_btrx_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != sl_btrx_page_obj) return;
    sl_btrx_page_obj = sl_btrx_body = NULL;
}

static void sl_btrx_page(void)
{
    if (page_bring_back(sl_btrx_page_obj)) return;
    lv_obj_t *page, *body = page_new("Receive from phone", &page);
    sl_btrx_page_obj = page;
    sl_btrx_body = body;
    lv_obj_add_event_cb(page, sl_btrx_deleted, LV_EVENT_DELETE, NULL);
    sl_btrx_rebuild();
    page_push(page);
}

static void sl_btrx_card_cb(lv_event_t *e)
{
    (void)e;
    sl_btrx_page();
    if (!sl_btrx_on) sl_btrx_begin();
}

/* Leaving for classic Rockbox: the session puts Bluetooth back first.
 * This is a Rockbox thread, so the wait sleeps (and the decoder runs). */
static void sl_btrx_shutdown(void)
{
    if (!sl_btrx_on) return;
    sl_log("btrx: leaving GabagoolOS, stopping the session");
    sl_btrx_stop();
    for (int i = 0; i < 150 && sl_btrx_running(); i++) sleep(HZ / 10);
    if (sl_btrx_timer) sl_btrx_tick(sl_btrx_timer);
}
