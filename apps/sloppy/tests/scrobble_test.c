/* Scrobbling tests: the formats, the queue on a real disk and the worker
 * against a fake ListenBrainz.
 * cc -std=gnu99 -DSLOPPY_NET_TEST -fsanitize=address,undefined scrobble_test.c -lpthread */
#include <assert.h>
#include "../sloppy_net.c"

static char root[256];
static int requests, answer = 200;
static char last_url[128], last_auth[128], last_body[32768];
static const char *bad_word;          /* a body naming it gets a 400 */

static int fake(const char *url, const char *auth, const char *body, char *out, size_t cap)
{
    requests++;
    snprintf(last_url, sizeof last_url, "%s", url);
    snprintf(last_auth, sizeof last_auth, "%s", auth);
    snprintf(last_body, sizeof last_body, "%s", body ? body : "");
    if (strstr(url, "validate-token"))
    {
        snprintf(out, cap, strstr(auth, "good") ? "{\"code\":200,\"message\":\"Token valid.\",\"valid\":true,\"user_name\":\"tai\"}"
                                                : "{\"code\":200,\"message\":\"Token invalid.\",\"valid\":false}");
        return 200;
    }
    if (bad_word && body && strstr(body, bad_word)) return 400;
    snprintf(out, cap, "{\"status\":\"ok\"}");
    return answer;
}

static char *slurp(const char *name)
{
    static char b[1 << 20];
    char p[300];
    snprintf(p, sizeof p, "%s%s", root, name);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
    if (f) fclose(f);
    b[n] = '\0';
    return b;
}

static int lines(const char *s) { int n = 0; for (; *s; s++) n += *s == '\n'; return n; }

static struct sl_listen song(int i, const char *title)
{
    struct sl_listen l;
    memset(&l, 0, sizeof l);
    l.when = 1790000000u + (uint32_t)i * 300;
    l.secs = 200 + (uint32_t)i;
    l.track = i % 12;
    snprintf(l.artist, sizeof l.artist, "Artist %d", i);
    snprintf(l.title, sizeof l.title, "%s", title);
    snprintf(l.album, sizeof l.album, "Album");
    return l;
}

/* every log line so far, checked for the token */
static void drain(const char *secret)
{
    char m[128];
    while (sl_net_scrobble_message(m, sizeof m))
    {
        printf("  log: scrobble: %s\n", m);
        assert(!strstr(m, secret));
    }
}

static void run(void) { scr.kick = true; sl_net_scrobble_poll(); }

int main(void)
{
    /* ---- the rules */
    assert(sl_scrob_counts(120000, 240000) && !sl_scrob_counts(119999, 240000));
    assert(sl_scrob_counts(240000, 3600000) && !sl_scrob_counts(239000, 3600000));
    assert(!sl_scrob_counts(29000, 29000) && sl_scrob_counts(15000, 30000));

    /* ---- one field */
    char f[16];
    sl_scrob_clean(f, sizeof f, "  a\tb\nc  ");
    assert(!strcmp(f, "a b c"));
    sl_scrob_clean(f, sizeof f, "x\xff" "y\xc3");      /* not UTF-8 */
    assert(!strcmp(f, "x?y?"));
    sl_scrob_clean(f, 6, "ab\xc3\xa9\xc3\xa9");         /* "abéé": é never cut in half */
    assert(!strcmp(f, "ab\xc3\xa9"));
    sl_scrob_clean(f, sizeof f, NULL);
    assert(!f[0]);
    assert(sl_scrob_uuid("62c2e20a-559e-422f-a44c-9afa7882f0c4") && !sl_scrob_uuid("62c2e20a?-559e-422f-a44c-9afa7882f0c4"));

    /* ---- the Audioscrobbler line, byte for byte */
    struct sl_listen l = { 1790346600u, 365, 1, "Metallica", "Enter Sandman", "Metallica", "62c2e20a-559e-422f-a44c-9afa7882f0c4" };
    char b[SCR_LINE];
    assert(sl_scrob_log_line(b, sizeof b, &l));
    assert(!strcmp(b, "Metallica\tMetallica\tEnter Sandman\t1\t365\tL\t1790346600\t62c2e20a-559e-422f-a44c-9afa7882f0c4\n"));
    struct sl_listen u = { 1790346600u, 312, 0, "Portishead", "Cowboys", "", "" };
    assert(sl_scrob_log_line(b, sizeof b, &u) && !strcmp(b, "Portishead\t\tCowboys\t\t312\tL\t1790346600\t\n"));
    /* the queue line and back */
    struct sl_listen back;
    assert(sl_scrob_queue_line(b, sizeof b, &l));
    assert(!strcmp(b, "1790346600\t365\t1\tMetallica\tEnter Sandman\tMetallica\t62c2e20a-559e-422f-a44c-9afa7882f0c4\n"));
    assert(sl_scrob_parse_line(b, &back) && !memcmp(&back, &l, sizeof l));
    assert(!sl_scrob_parse_line("1790346600\t365", &back) && !sl_scrob_parse_line("garbage", &back));
    /* the JSON, with quotes in a title */
    struct sl_listen q = { 1790346600u, 200, 0, "A \"B\"", "C\\D", "", "" };
    char j[1024];
    assert(sl_scrob_json(j, sizeof j, "single", &q, 1));
    assert(!strcmp(j, "{\"listen_type\":\"single\",\"payload\":[{\"listened_at\":1790346600,\"track_metadata\":"
                      "{\"artist_name\":\"A \\\"B\\\"\",\"track_name\":\"C\\\\D\",\"additional_info\":{\"media_player\":"
                      "\"GabagoolOS\",\"submission_client\":\"GabagoolOS\",\"submission_client_version\":\"1.0\","
                      "\"duration_ms\":200000}}}]}"));
    assert(sl_scrob_json(j, sizeof j, "playing_now", &l, 1) && !strstr(j, "listened_at") && strstr(j, "\"recording_mbid\""));
    assert(!sl_scrob_json(j, 40, "single", &l, 1));                /* does not fit: nothing */
    char user[16];
    assert(sl_scrob_json_true("{\"valid\": true}", "valid") && !sl_scrob_json_true("{\"valid\":false}", "valid"));
    assert(sl_scrob_json_text("{\"user_name\": \"t\\\"ai\"}", "user_name", user, sizeof user) && !strcmp(user, "t\"ai"));
    /* what busybox wget prints */
    assert(scr_status("note: TLS certificate validation not implemented\n  HTTP/1.1 401 UNAUTHORIZED\n"
                      "wget: server returned error: HTTP/1.1 401 UNAUTHORIZED\n") == 401);
    assert(scr_status("wget: bad address 'api.listenbrainz.org'\n") == 0);
    scr_reason("note: TLS certificate validation not implemented\nwget: bad address 'x'\n", user, sizeof user);
    assert(!strncmp(user, "wget: bad addr", 14));
    assert(sl_scrob_backoff(1) == 30 && sl_scrob_backoff(2) == 60 && sl_scrob_backoff(9) == 1800);
    printf("PASS: rules, fields, Audioscrobbler line, queue line, JSON, wget status\n");

    /* ---- the worker, on a disk */
    snprintf(root, sizeof root, "/tmp/sloppy-scrobble-test-%d", (int)getpid());
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/.rockbox", root, root);
    assert(!system(cmd));
    setenv("SL_NET_ROOT", root, 1);
    sl_net_scrobble_fake = fake;
    const char *secret = "good-0f3a9e2c-token";
    sl_net_scrobble_start();
    sl_net_scrobble_config(true, true);
    sl_net_scrobble_token(secret);
    run();
    assert(!strcmp(slurp(SCR_TOKEN), "good-0f3a9e2c-token\n"));
    /* three plays offline: on the card, nothing sent */
    for (int i = 0; i < 3; i++) { struct sl_listen s = song(i, "Song"); assert(sl_net_scrobble_add(&s)); }
    run();
    struct sl_scrobble_status st;
    sl_net_scrobble_status(&st);
    assert(st.queued == 3 && st.has_token && !requests);
    assert(lines(slurp(SCR_QUEUE)) == 3);
    char *log = slurp(SCR_LOG);
    assert(!strncmp(log, SL_SCROB_LOG_HEADER, strlen(SL_SCROB_LOG_HEADER)) && lines(log) == 4 + 3);
    assert(strstr(log, "Artist 1\tAlbum\tSong\t1\t201\tL\t1790000300\t\n"));
    drain(secret);
    /* Wi-Fi: one import of all three, the queue empties */
    sl_net_scrobble_online(true);
    run();
    sl_net_scrobble_status(&st);
    assert(requests == 1 && st.queued == 0 && st.sent == 3 && st.last_sent);
    assert(strstr(last_url, "submit-listens") && !strcmp(last_auth, "Authorization: Token good-0f3a9e2c-token"));
    assert(strstr(last_body, "\"listen_type\":\"import\"") && strstr(last_body, "Artist 2"));
    assert(!slurp(SCR_QUEUE)[0]);
    drain(secret);
    printf("PASS: offline queue, Audioscrobbler log, batch sent on Wi-Fi\n");

    /* playing now: no time, sent once, never queued */
    struct sl_listen np = song(9, "Now");
    sl_net_scrobble_now(&np);
    run();
    assert(requests == 2 && strstr(last_body, "playing_now") && !strstr(last_body, "listened_at"));
    run();
    assert(requests == 2);
    drain(secret);

    /* the token check */
    sl_net_scrobble_test();
    run();
    sl_net_scrobble_status(&st);
    assert(st.test == SL_SCROB_GOOD && !strcmp(st.user, "tai") && st.test_seq == 1);
    drain(secret);

    /* ---- partial acceptance: one bad listen in five */
    sl_net_scrobble_online(false);
    const char *titles[] = { "One", "Two", "BAD three", "Four", "Five" };
    for (int i = 0; i < 5; i++) { struct sl_listen s = song(10 + i, titles[i]); sl_net_scrobble_add(&s); }
    run();
    sl_net_scrobble_status(&st);
    assert(st.queued == 5);
    bad_word = "BAD";
    sl_net_scrobble_online(true);
    int before = requests;
    for (int i = 0; i < 8 && st.queued; i++) { run(); sl_net_scrobble_status(&st); }
    /* five refused, then One, Two, BAD (dropped), then Four and Five together */
    assert(st.queued == 0 && requests - before == 5 && st.sent == 3 + 4);
    assert(strstr(last_body, "import") && strstr(last_body, "Four") && strstr(last_body, "Five"));
    bad_word = NULL;
    drain(secret);
    printf("PASS: playing now, token check, a refused listen dropped and the rest sent\n");

    /* ---- failures back off, five in a row tell the user once */
    answer = -1;
    struct sl_listen s1 = song(20, "Later");
    sl_net_scrobble_add(&s1);
    before = requests;
    run();
    sl_net_scrobble_status(&st);
    assert(requests == before + 1 && st.queued == 1 && sw.failures == 1 && (int32_t)(sw.next_ms - net_ms()) > 25000);
    run();
    assert(requests == before + 1);                         /* waiting */
    unsigned fails = st.fail_seq;
    for (int i = 0; i < 6; i++) { sw.next_ms = net_ms(); run(); }
    sl_net_scrobble_status(&st);
    assert(sw.failures == 7 && st.fail_seq == fails + 1 && st.queued == 1 && st.error[0]);
    answer = 200;
    sw.next_ms = net_ms();
    run();
    sl_net_scrobble_status(&st);
    assert(st.queued == 0 && !st.error[0] && !sw.failures);
    drain(secret);

    /* ---- 401: sending stops until a new token */
    answer = 401;
    struct sl_listen s2 = song(21, "Refused");
    sl_net_scrobble_add(&s2);
    run();
    sl_net_scrobble_status(&st);
    assert(st.token_bad && st.queued == 1 && st.fail_seq == fails + 2);
    before = requests;
    sw.next_ms = net_ms();
    run();
    assert(requests == before);
    answer = 200;
    sl_net_scrobble_token(secret);
    run();
    sl_net_scrobble_status(&st);
    assert(!st.token_bad && st.queued == 0);
    drain(secret);
    printf("PASS: backoff, one warning for a long outage, a refused token stops sending\n");

    /* ---- USB mode: nothing written, it all lands afterwards */
    sl_net_scrobble_online(false);
    sl_net_scrobble_hold(true);
    struct sl_listen s3 = song(22, "During USB");
    sl_net_scrobble_add(&s3);
    run();
    assert(!slurp(SCR_QUEUE)[0] && !strstr(slurp(SCR_LOG), "During USB"));
    assert(sl_net_scrobble_flush(100));                      /* the card is the computer's: no wait */
    sl_net_scrobble_hold(false);
    run();
    assert(lines(slurp(SCR_QUEUE)) == 1 && strstr(slurp(SCR_LOG), "During USB"));
    /* the computer deleted the log after uploading it: a new one, with its header */
    char p[300];
    snprintf(p, sizeof p, "%s%s", root, SCR_LOG);
    unlink(p);
    struct sl_listen s4 = song(23, "After upload");
    sl_net_scrobble_add(&s4);
    run();
    log = slurp(SCR_LOG);
    assert(!strncmp(log, SL_SCROB_LOG_HEADER, strlen(SL_SCROB_LOG_HEADER)) && lines(log) == 5);

    /* ---- a line torn by a power cut: ended, then dropped as no listen */
    snprintf(p, sizeof p, "%s%s", root, SCR_QUEUE);
    FILE *qf = fopen(p, "w");
    fputs("1790000000\t200\t1\tTorn", qf);
    fclose(qf);
    sl_net_scrobble_hold(true);
    sl_net_scrobble_hold(false);                             /* reload: the computer changed the queue */
    struct sl_listen s5 = song(24, "Whole");
    sl_net_scrobble_add(&s5);
    run();
    assert(!strcmp(slurp(SCR_QUEUE), "1790000000\t200\t1\tTorn\n1790007200\t224\t0\tArtist 24\tWhole\tAlbum\t\n"));
    sl_net_scrobble_online(true);
    run();
    sl_net_scrobble_status(&st);
    drain(secret);
    assert(st.queued == 0 && strstr(last_body, "Whole") && !strstr(last_body, "Torn"));
    drain(secret);

    /* ---- the cap: the oldest go */
    sl_net_scrobble_online(false);
    qf = fopen(p, "w");
    for (int i = 0; i < SCR_CAP - 1; i++)
    {
        struct sl_listen s = song(100 + i, "Old");
        sl_scrob_queue_line(b, sizeof b, &s);
        fputs(b, qf);
    }
    fclose(qf);
    sl_net_scrobble_hold(true);
    sl_net_scrobble_hold(false);
    for (int i = 0; i < 3; i++) { struct sl_listen s = song(5000 + i, "New"); sl_net_scrobble_add(&s); }
    run();
    sl_net_scrobble_status(&st);
    char *qs = slurp(SCR_QUEUE);
    assert(st.queued == SCR_CAP && lines(qs) == SCR_CAP);
    assert(strstr(qs, "Artist 102\t") == qs + strlen("1790030600\t302\t6\t") && strstr(qs, "Artist 5002"));
    drain(secret);
    /* sending them all takes a hundred requests of twenty */
    sl_net_scrobble_online(true);
    before = requests;
    for (int i = 0; i < 200 && st.queued; i++) { run(); sl_net_scrobble_status(&st); }
    assert(st.queued == 0 && requests - before == SCR_CAP / SCR_BATCH);
    drain(secret);
    printf("PASS: USB mode, a deleted log, a torn line, the cap of %d\n", SCR_CAP);

    /* ---- forgetting the token removes the file and stops everything */
    sl_net_scrobble_token("");
    run();
    sl_net_scrobble_status(&st);
    assert(!st.has_token && access(p, F_OK) == 0);
    snprintf(p, sizeof p, "%s%s", root, SCR_TOKEN);
    assert(access(p, F_OK) != 0);
    drain(secret);
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(!system(cmd));
    printf("PASS: forgetting the token\n");
    return 0;
}
