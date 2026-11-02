/* The Bluetooth backend (apps/hiby_bluetooth.c) and the Bluetooth volume
 * sender (hibylinux_codec.c) against pretend BlueZ and BlueALSA tools and a
 * pretend sys_server. Run by run-bt-tests.sh, which copies the sources next
 * to the stubs and puts the fake tools on PATH. Mocks prove the state and
 * the races, not the radio. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hiby_bluetooth.c"
#include "pcm-alsa-hiby.c"
/* separate files on the device; one here, where the declarations differ */
#define bt_bluealsa_change_volume bt_bluealsa_change_volume_unused
#include "hibylinux_codec.c"

volatile long current_tick;
struct test_settings global_settings;
static int playing, paused, pauses, resumes, jack, switches;
static char routed_to[128];

unsigned int rb_test_sleep(unsigned int ticks) { usleep(ticks * 10000); return 0; }
int audio_status(void) { return (playing ? AUDIO_STATUS_PLAY : 0) | (paused ? AUDIO_STATUS_PAUSE : 0); }
void audio_pause(void) { if (playing) paused = 1; pauses++; }
void audio_resume(void) { paused = 0; resumes++; }
bool headphones_inserted(void) { return jack; }
int hiby_has_valid_output(void) { return jack ? 2 : 0; }
bool button_add_input_device(int i) { (void)i; return true; }
void button_remove_input_device(int i) { (void)i; }
bool pcm_is_initialized(void) { return true; }
int pcm_alsa_switch_playback_device(const char *device)
{
    switches++;
    snprintf(routed_to, sizeof routed_to, "%s", device);
    return 0;
}
void alsa_controls_init(const char *name) { (void)name; }
void alsa_controls_close(void) {}
void alsa_controls_set_bool(const char *name, bool value) { (void)name; (void)value; }
void alsa_controls_set_ints(const char *name, int count, long *values) { (void)name; (void)count; (void)values; }
bool alsa_controls_get_int_range(const char *name, long *min, long *max) { (void)name; *min = *max = 0; return false; }
bool alsa_has_control(const char *name) { (void)name; return false; }

/* sys_server: answers each ABSVOL after a delay, or never */
static pthread_mutex_t srv_lock = PTHREAD_MUTEX_INITIALIZER;
static int srv_fd, srv_count, srv_last = -1, srv_delay_ms = 40;
static bool srv_answer = true;
static char srv_last_cmd[96];
static void *server(void *arg)
{
    (void)arg;
    for (;;)
    {
        int c = accept(srv_fd, NULL, NULL);
        if (c < 0) continue;
        char b[96] = "";
        ssize_t n = recv(c, b, sizeof b - 1, 0);
        if (n > 0)
        {
            b[n] = '\0';
            pthread_mutex_lock(&srv_lock);
            srv_count++;
            snprintf(srv_last_cmd, sizeof srv_last_cmd, "%s", b);
            const char *sp = strrchr(b, ' ');
            srv_last = sp ? atoi(sp + 1) : -1;
            int delay = srv_delay_ms;
            bool answer = srv_answer;
            pthread_mutex_unlock(&srv_lock);
            usleep(delay * 1000);
            if (answer) send(c, "OK", 2, 0);
            else usleep(400000);          /* past the client's 300 ms */
        }
        close(c);
    }
    return NULL;
}
static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static void wait_sends(void) { usleep(900000); }
static void state(const char *file, bool on)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("BT_STATE"), file);
    if (on) { FILE *f = fopen(p, "w"); assert(f); fclose(f); }
    else unlink(p);
}
static void state_int(const char *file, int v)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("BT_STATE"), file);
    FILE *f = fopen(p, "w"); assert(f); fprintf(f, "%d\n", v); fclose(f);
}

static const struct hiby_bt_device phones = { "24:58:7C:8A:10:20", "Studio headphones", true };

/* ==== Receive from phone (hiby_btrx_*) ==== */
static bool state_has(const char *file, const char *needle)
{
    char p[512], b[512] = "";
    snprintf(p, sizeof p, "%s/%s", getenv("BT_STATE"), file);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    size_t n = fread(b, 1, sizeof b - 1, f);
    b[n] = '\0';
    fclose(f);
    return strstr(b, needle) != NULL;
}
static char rx_events[64][128];
static int rx_nevents;
static void rx_drain(void)
{
    char l[128];
    while (hiby_btrx_take_event(l, sizeof l))
    {
        printf("    rx: %s\n", l);
        if (rx_nevents < 64) snprintf(rx_events[rx_nevents++], sizeof rx_events[0], "%s", l);
    }
}
static bool rx_logged(const char *needle)
{
    for (int i = 0; i < rx_nevents; i++) if (strstr(rx_events[i], needle)) return true;
    return false;
}
static struct hiby_btrx_status rx;
/* the session reaches a state (or, for OFF, ends) within ms */
static bool rx_until(int want, int ms)
{
    for (long until = now_ms() + ms; now_ms() < until; usleep(20000))
    {
        rx_drain();
        hiby_btrx_status(&rx);
        if (want == HIBY_BTRX_OFF ? !hiby_btrx_running() : rx.state == want) { rx_drain(); hiby_btrx_status(&rx); return true; }
    }
    return false;
}
static bool rx_wait(bool (*ok)(void), int ms)
{
    for (long until = now_ms() + ms; now_ms() < until; usleep(20000)) { rx_drain(); hiby_btrx_status(&rx); if (ok()) return true; }
    return false;
}
static bool rx_buffered(void) { return hiby_btrx_buffered() > 4800; }
static bool rx_idle(void) { return hiby_btrx_idle_ms() > 1200; }
static bool rx_flowing(void) { return hiby_btrx_idle_ms() < 200 && hiby_btrx_buffered() > 0; }
static bool rx_titled(void) { return !strcmp(rx.title, "Gabagool Groove") && !strcmp(rx.player, "playing"); }
static bool rx_volume64(void) { return rx.volume == 64 && rx_logged("the phone set 64/127"); }
static bool rx_first(void) { return rx_logged("audio: first PCM"); }
static bool rx_answered(void) { return state_has("answered", "yes"); }
static bool rx_paused_sent(void) { return state_has("ctl", "Pause") && rx_logged("control: Pause sent"); }
static void rx_reset_state(void)
{
    static const char *const files[] = { "no_sink", "no_bluealsa", "no_discoverable", "phone", "paused", "title",
        "artist", "volume", "pair_request", "asked", "answered", "trusted", "phone_dropped", "ctl", "open_fails",
        "arecord", "format", "agent_said", "discoverable", "pairable", "alias" };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) state(files[i], false);
    rx_nevents = 0;
}
/* the phone's audio out of the ring: left counts up, right is its negative */
static void rx_check_audio(void)
{
    static int16_t b[2048 * 2];
    int got = hiby_btrx_read(b, 2048);
    assert(got > 1000);
    for (int i = 0; i < got; i++)
    {
        assert(b[i * 2] == -b[i * 2 + 1]);
        if (i) assert(b[i * 2] == ((b[i * 2 - 2] + 1) & 0x3fff));
    }
}

static void rx_tests(void)
{
    hiby_bt_worker_thread();
    state("powered", true);
    state("bluealsa", false);
    btrx_wait_ms = 1500;

    /* BlueALSA will not run as a sink: back to source mode, and said why */
    rx_reset_state();
    state("no_sink", true);
    assert(hiby_btrx_start() && hiby_btrx_running());
    assert(rx_until(HIBY_BTRX_OFF, 20000));
    assert(!strcmp(rx.error, "Bluetooth could not switch to receiving"));
    assert(state_has("bluealsa", "-p a2dp-source --a2dp-volume") && !state_has("discoverable", "on"));
    assert(rx_logged("did not answer") && rx_logged("restore: source mode back"));
    printf("PASS: receiver: a sink BlueALSA that will not start restores source mode.\n");

    /* BlueALSA missing altogether: the restore fails too, and says so */
    rx_reset_state();
    state("no_bluealsa", true);
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_OFF, 30000));
    assert(!strcmp(rx.error, "Bluetooth could not switch to receiving") && rx_logged("restore: FAILED"));
    printf("PASS: receiver: BlueALSA missing is reported, not hung on.\n");

    /* the controller will not become visible */
    rx_reset_state();
    state("no_discoverable", true);
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_OFF, 30000));
    assert(!strcmp(rx.error, "The player could not be made visible"));
    assert(state_has("alias", "HiBy R1") && state_has("bluealsa", "a2dp-source"));
    printf("PASS: receiver: not visible -> name and source mode restored.\n");

    /* nobody comes: visible for the wait, then everything as it was */
    rx_reset_state();
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_WAITING, 20000));
    assert(state_has("bluealsa", "-p a2dp-sink --a2dp-volume") && state_has("discoverable", "on"));
    assert(state_has("alias", "GabagoolOS R1") && state_has("agent_said", "agent NoInputNoOutput") &&
           state_has("agent_said", "default-agent") && state_has("pairable", "on"));
    assert(rx_until(HIBY_BTRX_OFF, 20000) && !strcmp(rx.error, "No phone connected"));
    assert(state_has("discoverable", "off") && state_has("alias", "HiBy R1") && state_has("bluealsa", "a2dp-source"));
    assert(rx_logged("visible as 'GabagoolOS R1'") && rx_logged("no phone in 1 s"));
    printf("PASS: receiver: timeout restores name, visibility and source mode.\n");

    /* the whole session: headphones playing first, then a phone pairs,
     * connects, plays, pauses, is paused from here, and goes */
    rx_reset_state();
    bt_dropped_mac[0] = '\0';
    state_int("fail", 0);
    assert(hiby_bt_connect(&phones) && hiby_pcm_get_bt_mac());
    hiby_bt_take_audio_kick();
    btrx_wait_ms = 30000;
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_WAITING, 20000));
    assert(!hiby_pcm_get_bt_mac() && strstr(routed_to, "plughw") && !state_has("removed", "24:58"));
    assert(rx_logged("headphones 24_58_7C_8A_10_20 let go") || rx_logged("headphones 24:58:7C:8A:10:20 let go"));
    state("pair_request", true);
    assert(rx_wait(rx_answered, 5000) && rx_logged("pairing request"));
    state("title", true); state("artist", true);
    char p[512];
    snprintf(p, sizeof p, "%s/title", getenv("BT_STATE")); { FILE *f = fopen(p, "w"); fputs("Gabagool Groove", f); fclose(f); }
    snprintf(p, sizeof p, "%s/artist", getenv("BT_STATE")); { FILE *f = fopen(p, "w"); fputs("The Sopranos", f); fclose(f); }
    state("phone", true);
    assert(rx_until(HIBY_BTRX_CONNECTED, 5000));
    assert(!strcmp(rx.mac, "AA:BB:CC:DD:EE:01") && !strcmp(rx.name, "Tai's iPhone") && rx.connects == 1);
    assert(rx.rate == 48000 && rx.channels == 2 && !strcmp(rx.codec, "AAC"));
    assert(rx_wait(rx_buffered, 5000));
    assert(state_has("trusted", "AA:BB:CC:DD:EE:01"));
    rx_check_audio();
    assert(rx_wait(rx_first, 3000) && rx_logged("connected: AA:BB:CC:DD:EE:01 (Tai's iPhone), codec AAC, S16_LE, 2 ch, 48000 Hz"));
    assert(rx_wait(rx_titled, 5000) && !strcmp(rx.artist, "The Sopranos"));
    state_int("volume", 64);
    assert(rx_wait(rx_volume64, 8000));
    state("paused", true);
    assert(rx_wait(rx_idle, 5000));
    assert(rx_until(HIBY_BTRX_CONNECTED, 10) && hiby_btrx_running());   /* a pause is not a disconnect */
    state("paused", false);
    assert(rx_wait(rx_flowing, 5000));
    hiby_btrx_control("Pause");
    assert(rx_wait(rx_paused_sent, 5000));
    state("phone", false);                 /* walked out of range */
    assert(rx_until(HIBY_BTRX_OFF, 10000) && !strcmp(rx.error, "Phone disconnected"));
    assert(!state_has("phone_dropped", "AA") && state_has("bluealsa", "a2dp-source") &&
           state_has("discoverable", "off") && state_has("alias", "HiBy R1"));
    assert(!state_has("removed", "24:58"));  /* the headphones are still paired */
    printf("PASS: receiver: pair, connect, audio, track, volume, pause, control, disconnect.\n");

    /* stopped from the player: the phone is let go, source mode back */
    rx_reset_state();
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_WAITING, 20000));
    state("phone", true);
    assert(rx_until(HIBY_BTRX_CONNECTED, 5000) && rx.connects == 1);
    hiby_btrx_stop();
    assert(rx_until(HIBY_BTRX_OFF, 10000) && !rx.error[0]);
    assert(state_has("phone_dropped", "AA:BB:CC:DD:EE:01") && state_has("bluealsa", "a2dp-source"));
    assert(rx_logged("stop: asked to") && rx_logged("restore: source mode back"));
    printf("PASS: receiver: Stop lets the phone go and restores source mode.\n");

    /* "bluealsa-cli open" refused: the audio comes in through arecord,
     * and a 32-bit stream is read from its top 16 bits */
    rx_reset_state();
    state("open_fails", true);
    snprintf(p, sizeof p, "%s/format", getenv("BT_STATE")); { FILE *f = fopen(p, "w"); fputs("S32_LE", f); fclose(f); }
    assert(hiby_btrx_start() && rx_until(HIBY_BTRX_WAITING, 20000));
    state("phone", true);
    assert(rx_until(HIBY_BTRX_CONNECTED, 5000));
    assert(rx_wait(rx_buffered, 8000));
    rx_check_audio();
    assert(state_has("arecord", "-D bluealsa:DEV=AA:BB:CC:DD:EE:01,PROFILE=a2dp -t raw -f S32_LE -c 2 -r 48000"));
    assert(rx_logged("reading with arecord"));
    hiby_btrx_stop();
    assert(rx_until(HIBY_BTRX_OFF, 10000));
    printf("PASS: receiver: arecord fallback, S32_LE read right.\n");

    /* the ring never lets the delay grow past half a second */
    btrx_head = BTRX_RING - 10; btrx_tail = 0;
    static int16_t b[64];
    assert(hiby_btrx_read(b, 32) == 32 && btrx_head - btrx_tail == BTRX_RING / 4 - 32);
    btrx_head = btrx_tail = 0;
    printf("PASS: receiver: latency is trimmed, not left to grow.\n");
    rx_reset_state();
}

/* ==== names, PIN pairing, reconnecting, SBC quality, a hung bluetoothctl ==== */
static volatile int asked_kinds, answer_wrong;
static char asked_code[20];
static void *answerer(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    for (int i = 0; i < 4000; i++)
    {
        struct hiby_bt_ask a;
        if (hiby_bt_pair_question(&a) && a.serial != seen)
        {
            seen = a.serial;
            asked_kinds |= 1 << a.kind;
            snprintf(asked_code, sizeof asked_code, "%s", a.code);
            if (a.kind == HIBY_BT_ASK_CONFIRM) hiby_bt_pair_answer(a.serial, !answer_wrong, NULL);
            else if (a.kind == HIBY_BT_ASK_PIN) hiby_bt_pair_answer(a.serial, true, answer_wrong ? "9999" : "1234");
        }
        usleep(10000);
    }
    return NULL;
}
static void file_put(const char *file, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", getenv("BT_STATE"), file);
    FILE *f = fopen(p, "w"); assert(f); fputs(text, f); fclose(f);
}
static void pair_tests(void)
{
    struct hiby_bt_device car = { "24:58:7C:8A:10:20", "Car stereo", false };
    pthread_t ans;
    pthread_create(&ans, NULL, answerer, NULL);
    state_int("fail", 0);
    /* a car showing a passkey: it is shown here, Pair says yes */
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    file_put("pair_mode", "confirm");
    assert(hiby_bt_connect(&car) && (asked_kinds & 1 << HIBY_BT_ASK_CONFIRM) && !strcmp(asked_code, "123456"));
    assert(state_has("agent_said", "agent KeyboardDisplay") && state_has("agent_said", "yes"));
    printf("PASS: pairing: a passkey to compare is shown (123456) and confirmed.\n");
    /* turned down here: no pairing, and why */
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0'; answer_wrong = 1;
    assert(!hiby_bt_connect(&car) && hiby_bt_last_error() == HIBY_BT_ERR_CANCELED);
    /* a PIN to type (#168): typed, and a wrong one fails as such */
    file_put("pair_mode", "pin"); file_put("pin_expect", "1234");
    assert(!hiby_bt_connect(&car) && hiby_bt_last_error() == HIBY_BT_ERR_PAIR_FAILED);
    answer_wrong = 0;
    assert(hiby_bt_connect(&car) && (asked_kinds & 1 << HIBY_BT_ASK_PIN));
    printf("PASS: pairing: a PIN is asked for and typed; a wrong one says so; No cancels.\n");
    /* a passkey to type on the other side: shown, nothing asked */
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    file_put("pair_mode", "display");
    assert(hiby_bt_connect(&car) && (asked_kinds & 1 << HIBY_BT_ASK_SHOW_PASSKEY));
    /* earbuds that kept an old pairing: our keys go, a second try pairs */
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    file_put("pair_mode", "stale"); state("removed", false);
    assert(hiby_bt_connect(&car) && state_has("removed", car.mac));
    printf("PASS: pairing: a passkey to type there is shown; stale keys are removed and paired again.\n");
    state("pair_mode", false); state("removed", false);

    /* the player's own name and a device's, kept and given back to BlueZ */
    assert(hiby_bt_set_player_name("  Tai's R1\n") && state_has("alias", "Tai's R1"));
    char n[64]; hiby_bt_player_name(n, sizeof n); assert(!strcmp(n, "Tai's R1"));
    file_put("alias", "HiBy R1\n");                 /* a reboot: bt_init set HiBy's name again */
    bt_cfg.loaded = false; bt_ready_done = false;
    hiby_bt_disconnect(); hiby_bt_take_audio_pause();
    assert(hiby_bt_power(true) && state_has("alias", "Tai's R1"));
    assert(hiby_bt_rename(phones.mac, "Kitchen; rm -rf /") && state_has("dbus_set", "variant:string:Kitchen; rm -rf /"));
    struct hiby_bt_device list[8];
    int count = hiby_bt_devices(list, 8);
    assert(count >= 1 && !strcmp(list[0].name, "Kitchen; rm -rf /"));
    bt_cfg.loaded = false;                           /* from the card again */
    count = hiby_bt_devices(list, 8);
    assert(!strcmp(list[0].name, "Kitchen; rm -rf /"));
    assert(hiby_bt_rename(phones.mac, "") && state_has("dbus_set", "variant:string:\n"));
    count = hiby_bt_devices(list, 8);
    assert(!strcmp(list[0].name, "Studio headphones"));
    printf("PASS: names: the player's and a device's kept on the card, given to BlueZ at power-on, no shell.\n");

    /* reconnecting: the headphones routed last are tried again */
    assert(hiby_bt_connect(&phones));
    char last[18]; assert(hiby_bt_last_device(last) && !strcmp(last, phones.mac));
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    state("connected", false);
    state_int("fail", 2);
    assert(hiby_bt_reconnect() == 0 && hiby_bt_last_error() == HIBY_BT_ERR_NOT_FOUND);   /* out of range */
    state_int("fail", 0);
    assert(hiby_bt_reconnect() == 1 && hiby_pcm_get_bt_mac());                              /* back */
    assert(hiby_bt_reconnect() == 1);                                                        /* nothing to do */
    printf("PASS: reconnect: out of range is a plain miss, back in range it routes.\n");

    /* a bluetoothctl that never answers: the try ends on its deadline */
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    state("connected", false); state("hang", true);
    long t0 = now_ms();
    assert(hiby_bt_reconnect() == 0);
    long took = now_ms() - t0;
    state("hang", false);
    printf("hung bluetoothctl: reconnect gave up after %ld ms\n", took);
    assert(took >= 11000 && took < 16000);
    printf("PASS: a hung bluetoothctl is killed at its deadline (%ld ms), nothing waits forever.\n", took);

    /* SBC XQ: BlueALSA restarted with it, and back */
    state("bluealsa", true); file_put("bluealsa", "-p a2dp-source --a2dp-volume\n");
    assert(hiby_bt_set_sbc_quality(1) && state_has("bluealsa", "--sbc-quality=xq"));
    assert(hiby_bt_set_sbc_quality(2) && state_has("bluealsa", "--sbc-quality=xq+"));
    assert(hiby_bt_set_sbc_quality(0) && !state_has("bluealsa", "--sbc-quality"));
    printf("PASS: SBC quality: BlueALSA restarted with --sbc-quality=xq / xq+, and without.\n");
    hiby_bt_set_player_name("");                     /* the receiver's tests expect its own name */

    /* AirPods (AAP): LibrePods' sample packets, as the AirPods send them */
    static const uint8_t battery[] = { 0x04,0x00,0x04,0x00,0x04,0x00,0x03, 0x02,0x01,0x64,0x02,0x01,
                                       0x04,0x01,0x63,0x01,0x01, 0x08,0x01,0x11,0x02,0x01 };
    static const uint8_t anc[] = { 0x04,0x00,0x04,0x00,0x09,0x00,0x0d,0x02,0x00,0x00,0x00 };
    static const uint8_t junk[] = { 0x04,0x00,0x04,0x00,0x04,0x00,0x09, 0x02 };   /* says 9, carries none */
    struct hiby_bt_aap ap;
    hiby_bt_aap_parse(junk, sizeof junk);
    hiby_bt_aap_parse(battery, sizeof battery);
    hiby_bt_aap_parse(anc, sizeof anc);
    hiby_bt_aap_status(&ap);
    assert(ap.up && ap.right == 100 && !ap.right_charging && ap.left == 99 && ap.left_charging && ap.box == 17 && ap.noise == 2);
    assert(!hiby_bt_aap_noise(3));                   /* no AAP link here: said, not sent */
    printf("PASS: AirPods: battery (L 99%% charging, R 100%%, case 17%%) and noise control read from AAP packets.\n");
    pthread_join(ans, NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);    /* a crash keeps what was said */
    /* The receiver blocks SIGPIPE on its own thread, which is enough on
     * Linux (the signal goes to the writing thread). macOS sends it to the
     * process, to any thread that has it unblocked: ignore it here. */
    signal(SIGPIPE, SIG_IGN);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", HIBY_SYS_SERVER_SOCKET);
    unlink(a.sun_path);
    srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(srv_fd >= 0 && bind(srv_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(srv_fd, 16) == 0);
    pthread_t st;
    pthread_create(&st, NULL, server, NULL);
    audiohw_preinit();
    audiohw_set_volume(-400, -400);    /* before any route: nothing is sent */
    usleep(100000);
    assert(srv_count == 0);

    hiby_bt_worker_thread();           /* as GabagoolOS's worker */
    state("powered", true);

    /* a connect that routes reports success, and status says routed (the
     * colon / underscore spellings of the address used to never match) */
    playing = 1;
    assert(hiby_bt_connect(&phones));
    assert(strstr(routed_to, "bluealsa:DEV=24:58:7C:8A:10:20"));
    struct hiby_bt_status s;
    hiby_bt_status(&s);
    assert(s.connected && s.routed && !strcmp(s.mac, "24:58:7C:8A:10:20"));
    assert(!strcmp(s.name, "Studio headphones") && !strcmp(s.codec, "AAC"));
    assert(hiby_bt_take_audio_kick() && !hiby_bt_take_audio_pause());
    /* and the headphones are sent the player's volume at once */
    wait_sends();
    int step = (300 * 127 + 300) / 600;
    assert(srv_count == 1 && srv_last == step);
    assert(!strncmp(srv_last_cmd, "BT:ABSVOL:24_58_7C_8A_10_20 ", 28));
    printf("PASS: connect routes and reports routed, volume synced on connect (step %d).\n", step);

    /* a drag: 120 volume changes in a row never wait for sys_server, and
     * only the newest few are sent, ending where the finger stopped */
    srv_count = 0;
    long from = now_ms();
    for (int v = -700; v <= -100; v += 5) audiohw_set_volume(v, v);
    long took = now_ms() - from;
    wait_sends();
    printf("drag: 121 changes in %ld ms, %d sent, last step %d\n", took, srv_count, srv_last);
    assert(took < 60 && srv_count >= 1 && srv_count <= 12 && srv_last == 127);
    /* sys_server not answering: still never waited on, and retried at most
     * twice before the next change takes over */
    pthread_mutex_lock(&srv_lock); srv_answer = false; srv_count = 0; pthread_mutex_unlock(&srv_lock);
    from = now_ms();
    audiohw_set_volume(-300, -300);
    took = now_ms() - from;
    usleep(2500000);
    printf("silent sys_server: call took %ld ms, %d tries\n", took, srv_count);
    assert(took < 20 && srv_count == 3);
    pthread_mutex_lock(&srv_lock); srv_answer = true; srv_count = 0; pthread_mutex_unlock(&srv_lock);
    audiohw_set_volume(-300, -300);    /* not taken last time: sent again */
    wait_sends();
    assert(srv_count == 1);
    printf("PASS: volume drag coalesced and off-thread, silent sys_server bounded.\n");

    /* every half-dB press moves the headphones, never by more than two */
    for (int v = -700; v < -100; v += 5)
    {
        int d = hiby_volume_to_absvol_step(v + 5) - hiby_volume_to_absvol_step(v);
        assert(d >= 1 && d <= 2);
    }
    assert(hiby_volume_to_absvol_step(-700) == 0 && hiby_volume_to_absvol_step(-100) == 127);
    /* up, then straight back down while the up is still being sent: the
     * down reaches the headphones too (it used to look like no change) */
    int back = hiby_volume_to_absvol_step(-300);
    pthread_mutex_lock(&srv_lock); srv_delay_ms = 200; srv_count = 0; pthread_mutex_unlock(&srv_lock);
    audiohw_set_volume(-295, -295);
    usleep(50000);                     /* the up is out, not answered yet */
    audiohw_set_volume(-300, -300);
    wait_sends();
    printf("reversal in flight: %d sent, last step %d (want %d)\n", srv_count, srv_last, back);
    assert(srv_count == 2 && srv_last == back);
    pthread_mutex_lock(&srv_lock); srv_delay_ms = 40; pthread_mutex_unlock(&srv_lock);
    printf("PASS: one rounding, every press reaches the headphones, reversals kept.\n");

    /* a disconnect with nothing in the jack pauses (the UI thread does it);
     * with wired headphones in, the song carries on through them */
    assert(hiby_bt_disconnect() && strstr(routed_to, "plughw"));
    assert(hiby_bt_take_audio_pause() && !hiby_bt_take_audio_kick() && !hiby_pcm_get_bt_mac());
    srv_count = 0;
    audiohw_set_volume(-250, -250);    /* nothing routed: nothing sent */
    wait_sends();
    assert(srv_count == 0);
    /* the user let it go: it linking straight back is not routed to */
    state("connected", true);
    assert(!hiby_bt_auto_route() && !hiby_pcm_get_bt_mac());
    /* asked for again, it is; with GabagoolOS picking the Bluetooth level the
     * route itself sends nothing (no blip at the jack's level) */
    hiby_bt_volume_by_ui = true;
    srv_count = 0;
    assert(hiby_bt_connect(&phones) && hiby_pcm_get_bt_mac());
    wait_sends();
    assert(srv_count == 0);
    audiohw_set_volume(-550, -550);    /* the UI sets Bluetooth's own level */
    wait_sends();
    assert(srv_count == 1 && srv_last == hiby_volume_to_absvol_step(-550));
    hiby_bt_volume_by_ui = false;
    printf("PASS: GabagoolOS route sends only the level the UI picks.\n");
    hiby_bt_take_audio_kick();
    jack = 1;
    assert(hiby_bt_disconnect());
    assert(!hiby_bt_take_audio_pause() && hiby_bt_take_audio_kick());
    jack = 0;
    printf("PASS: disconnect pauses with an empty jack, keeps playing on wired, no re-route fight.\n");

    /* headphones switched on by themselves (not the dropped one) route */
    bt_dropped_mac[0] = '\0';
    state("connected", true);
    assert(hiby_bt_auto_route());
    hiby_bt_status(&s);
    assert(s.routed);
    /* routed but BlueALSA silent: the address shown is still BlueZ's */
    state("connected", false);
    hiby_bt_status(&s);
    assert(s.connected && !strcmp(s.mac, "24:58:7C:8A:10:20"));
    printf("PASS: auto route, status address while BlueALSA is silent.\n");

    /* a first page turned away is tried once more; two are given up on */
    hiby_bt_disconnect();
    hiby_bt_take_audio_pause();
    bt_dropped_mac[0] = '\0';
    state_int("fail", 1);
    from = now_ms();
    assert(hiby_bt_connect(&phones));
    printf("connect retry: %ld ms\n", now_ms() - from);
    hiby_bt_disconnect();
    state_int("fail", 2);
    assert(!hiby_bt_connect(&phones) && !hiby_pcm_get_bt_mac());
    printf("PASS: one bounded connect retry.\n");

    /* BlueALSA answering empty now and then, while the route plays: a
     * connect that routed says so (it said "Could not pair or route audio"
     * over working headphones) */
    state_int("fail", 0);
    int ok_count = 0;
    for (int i = 0; i < 4; i++)
    {
        hiby_bt_disconnect();
        hiby_bt_take_audio_pause();
        bt_dropped_mac[0] = '\0';
        state_int("flaky", i);
        bool ok = hiby_bt_connect(&phones);
        bool routed = hiby_pcm_get_bt_mac() != NULL;
        printf("flaky BlueALSA connect %d: reported %s, routed %d\n", i, ok ? "ok" : "FAILED", routed);
        assert(ok == routed);
        ok_count += ok;
    }
    state("flaky", false);
    assert(ok_count > 0);
    /* and the route is a connected device at once: the stale "not
     * connected" from before the connect bounced every connect back to
     * the jack through the button tick */
    bt_connected_cache = false;
    hiby_bt_disconnect(); hiby_bt_take_audio_pause(); bt_dropped_mac[0] = '\0';
    bt_connected_cache = false;
    assert(hiby_bt_connect(&phones) && bt_connected_cache && bt_is_connected_fast());
    printf("PASS: a connect that routes reports success while BlueALSA is flaky.\n");

    /* the classic menus run on a Rockbox thread: the pause happens there */
    state_int("fail", 0);
    assert(hiby_bt_connect(&phones));
    bt_off_kernel = false;
    playing = 1; paused = 0; pauses = 0;
    bt_disconnect();
    assert(paused && pauses == 1 && !hiby_bt_take_audio_pause());
    printf("PASS: Rockbox-thread disconnect pauses directly.\n");
    bt_off_kernel = true;
    pair_tests();
    rx_tests();
    return 0;
}
