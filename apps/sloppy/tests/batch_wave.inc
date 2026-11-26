/* P19: Now Playing's waveform seek bar. Included from ui_test.c after
 * batch_h.inc.
 *   wave_tests()          in the suite
 *   SLOPPY_WAVE_TESTS=1   the tests alone
 *   audit screens now-playing-waveform*, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=now-playing-waveform ./ui-test */

/* a song's shape, for the screens: a quiet intro, verses, a loud chorus, a
 * break, the last chorus, a fade */
static uint8_t wave_shape(int c)
{
    static const float sec[] = { -26, -12, -9, -4, -3, -15, -11, -3, -2, -8, -22 };
    float t = (float)c * 11 / WV_N;
    int i = (int)t;
    float f = t - i, db = sec[i] + (i < 10 ? (sec[i + 1] - sec[i]) * f * f : 0);
    unsigned r = (unsigned)c * 2654435761u;
    db += (float)((r >> 13) % 70) / 10.0f - 3.5f;   /* a beat's ups and downs */
    if (db > 0) db = 0;
    return wv_level((int)(32768.0f * powf(10.0f, db / 20.0f)));
}
static bool wave_au_album;
static void wave_au(int which)
{
    /* 0 the shape known (cached), 40 % played; 1 a first play, filling in;
     * 2 with the album line and the status bar; 3 the colours from the cover */
    au_np_set(3, which == 2, 1, 0, which == 3);
    if (which == 2) { wave_au_album = true; global_settings.sloppy_np_album = 1; now_playing_retheme(); }
    test_track.elapsed = test_track.length * 2 / 5;
    np_show(); frame(300);
    wv_poll();
    if (!wv) return;
    for (int c = 0; c < WV_N; c++) wv->col[c] = which == 1 && c > WV_N * 2 / 5 ? 0 : wave_shape(c);
    wv->changed = false;                 /* drawn for the screen, not kept */
    wv->grow = 256;
    np_update(false);
    lv_obj_invalidate(np_seek);
}
static void wave_au_reset(void)
{
    if (wave_au_album) { wave_au_album = false; global_settings.sloppy_np_album = 0; now_playing_retheme(); }
}

/* one quarter second of playback: a buffer at amplitude a through the hook, the clock on */
static void wave_step(float a)
{
    float f = 440;
    test_feed(1, &f, &a);
    test_track.elapsed += 250;
    wv_poll();
    if (np_open) np_update(false);      /* as periodic does, a quarter second apart */
}
static void wave_file(char *p, int n, const char *path, int cue) { wv_path(p, n, wv_hash(path, cue, 2166136261u)); }

static void wave_tests(void)
{
    int st0 = test_status, style0 = global_settings.sloppy_np_style;
    struct mp3entry t0 = test_track;
    char cache[64];
    sl_track_path(3,test_track.path,sizeof test_track.path);
    test_track.title = "Wave Song"; test_track.artist = "Mira Sol";
    test_track.length = WV_N * 1000UL;   /* a column a second */
    test_track.elapsed = 0;
    test_status = AUDIO_STATUS_PLAY;
    wave_file(cache, sizeof cache, test_track.path, -1);
    sim_remove(cache);
    {   /* the layout opens: the seek slider is the waveform's box, the hook listens */
        global_settings.sloppy_np_style = 3; now_playing_retheme();
        np_show(); frame(400);
        wv_poll();
        assert(wv && !strcmp(wv->path, test_track.path) && test_hook);
        assert(lv_obj_get_height(np_seek) == WV_H && lv_obj_get_style_opa(np_seek, LV_PART_KNOB) == LV_OPA_TRANSP);
        assert(np_glass() || sl_zune());
        for (int c = 0; c < WV_N; c++) assert(!wv->col[c]);   /* nothing heard yet */
        printf("PASS: the waveform layout opens on the glass, its slider clear, the hook listening.\n");
    }
    {   /* columns fill from the PCM as the song plays; louder is taller */
        int loads0 = wv_loads;
        for (int i = 0; i < 40 * 4; i++) wave_step(i < 20 * 4 ? 3000 : 24000);
        frame(100);
        for (int c = 0; c < 39; c++) assert(wv->col[c]);
        for (int c = 41; c < WV_N; c++) assert(!wv->col[c]);
        assert(wv->col[30] > wv->col[10] + 40 && wv_loads == loads0 && fake_find(cache) < 0);
        /* one more column: only its bar and the played strip are drawn again */
        frame(100);
        lv_display_t *d = lv_display_get_default();
        d->inv_p = 0;
        for (int i = 0; i < 4; i++) wave_step(12000);
        np_update(false);
        lv_area_t sb; lv_obj_get_coords(np_seek, &sb);
        int seen = 0;
        for (uint32_t i = 0; i < d->inv_p; i++)
        {
            lv_area_t a = d->inv_areas[i], o;
            if (d->inv_area_joined[i] || !lv_area_intersect(&o, &a, &sb)) continue;
            if (a.y1 > sb.y1 + 2) continue;      /* the time under it, its shadow's edge reaching in */
            if (lv_area_get_width(&o) > 3 * WV_PITCH) fprintf(stderr, "wave inv %d..%d x %d..%d\n", (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2);
            assert(lv_area_get_width(&o) <= 3 * WV_PITCH);   /* never the whole bar */
            seen++;
        }
        assert(seen);
        frame(100);
        printf("PASS: the waveform fills in from the PCM as the song plays, a column at a time, only those redrawn.\n");
    }
    {   /* the whole song heard: kept on the card, in its shard */
        int saves0 = wv_saves;
        while (test_track.elapsed < test_track.length) wave_step(16000);
        for (int c = 0; c < WV_N; c++) assert(wv->col[c]);
        assert(wv_saves == saves0 + 1 && fake_find(cache) >= 0 && fake_size(cache) == (long)(sizeof(struct wv_file) + WV_N));
        assert(strstr(cache, WV_DIR "/") && cache[strlen(WV_DIR) + 3] == '/');   /* waves/xx/key.wv */
        printf("PASS: a waveform heard to the end is kept on the card, in one of 256 folders.\n");
    }
    uint8_t heard[WV_N];
    memcpy(heard, wv->col, WV_N);
    {   /* a layout change gives it all back, the hook too */
        global_settings.sloppy_np_style = 0; now_playing_retheme(); frame(100);
        assert(!wv && !test_hook && lv_obj_get_height(np_seek) == NP_TRACK_H);
        assert(lv_obj_get_style_opa(np_seek, LV_PART_KNOB) == LV_OPA_COVER);
        wv_poll(); assert(!wv);
        printf("PASS: another layout frees the waveform, lets go of the hook and brings the thin seek bar back.\n");
    }
    {   /* the second play: the whole shape at once, from the card */
        np_hide(0); frame(400);
        test_track.elapsed = 0;
        int loads0 = wv_loads;
        global_settings.sloppy_np_style = 3; now_playing_retheme();
        np_show(); frame(100);
        wv_poll(); frame(400);
        assert(wv && wv_loads == loads0 + 1 && !memcmp(wv->col, heard, WV_N) && wv->grow == 256);
        /* another cue track of the same file is another song */
        assert(wv_hash(test_track.path, 0, 2166136261u) != wv_hash(test_track.path, 1, 2166136261u));
        printf("PASS: the second play shows the whole waveform at once, read back from the card.\n");
    }
    {   /* a tap on it seeks nothing (only a drag does, 25 Nov), as on the thin bar */
        frame(300);
        lv_area_t sb; lv_obj_get_coords(np_seek, &sb);
        int w = lv_area_get_width(&sb), x = sb.x1 + w / 4, y = (sb.y1 + sb.y2) / 2;
        int seeks = audio_seeks;
        touch_at(x, y, true); touch_at(x, y, true); touch_at(x, y, false); frame(100);
        assert(audio_seeks == seeks);
        (void)w;
        /* a drag: the time follows the finger, the seek waits for the release */
        seeks = audio_seeks;
        touch_at(x, y, true);
        for (int k = 1; k <= 8; k++) touch_at(x + k * 20, y, true);
        assert(lv_slider_is_dragged(np_seek) && audio_seeks == seeks);
        char b[16]; fmt_time(b, sizeof b, (long)((int64_t)lv_slider_get_value(np_seek) * test_track.length / 1000));
        assert(!strcmp(lv_label_get_text(np_t0), b));
        touch_at(x + 160, y, false); frame(100);
        assert(audio_seeks == seeks + 1 && labs((long)test_track.elapsed - (long)((int64_t)(w / 4 + 160) * test_track.length / w)) <= (long)test_track.length / 100);
        printf("PASS: a tap on the waveform seeks nothing; a drag shows its time and seeks on release.\n");
    }
    {   /* paused: nothing is added; a seek is not heard as the columns between */
        uint8_t before[WV_N];
        memset(wv->col, 0, WV_N); wv->have_last = false;
        test_status = AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE;
        for (int i = 0; i < 8; i++) wave_step(20000);
        memcpy(before, wv->col, WV_N);
        for (int c = 0; c < WV_N; c++) assert(!before[c]);
        test_status = AUDIO_STATUS_PLAY;
        test_track.elapsed = 10000; wv_poll();
        test_track.elapsed = 60000; wv_poll();   /* a jump of 50 s in a quarter second */
        for (int c = 11; c < 60; c++) assert(!wv->col[c]);
        wv->changed = false;
        printf("PASS: the waveform hears nothing while paused, and a seek leaves the columns it jumped over.\n");
    }
    np_hide(0); frame(400);
    global_settings.sloppy_np_style = style0; now_playing_retheme(); frame(100);
    assert(!wv || style0 == 3);
    sim_remove(cache);
    test_track = t0; test_status = st0;
}
