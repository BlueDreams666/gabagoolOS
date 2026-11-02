/* GabagoolOS pet mixtape. GPLv2+. Included by sloppy.c after the graveyard.
 *
 * The pet makes you a tape from what it has learned while listening with
 * you: its favourite artist opens side A, the other artists it has heard a
 * lot come next, and the style its species loves (found by genre in the tag
 * database) fills the rest. A pet that has not heard enough yet grabs songs
 * at random. Nothing from the Skip Graveyard makes the cut. The tape is the
 * same all day, until you ask for a new one; save it and it is a real
 * playlist in the Playlists folder. */

#define MIX_MAX 14
#define MIX_POOL 400

static int mix_tracks[MIX_MAX], mix_n;
static unsigned mix_rerolls;
static lv_obj_t *mix_page_obj, *mix_reel[2], *mix_body;
static lv_timer_t *mix_timer;
static int32_t mix_reel_angle;

static uint32_t mix_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static int ts_picks(int **out);          /* sloppy_taste.inc: what your swipes say you'll like */

static const char *mix_pet_name(void) { return pet.name[0] ? pet.name : "Your pet"; }

static bool mix_has(int t)
{
    for (int i = 0; i < mix_n; i++) if (mix_tracks[i] == t) return true;
    return false;
}

/* Buried songs never go on a tape. */
static bool mix_allowed(int t)
{
    int g = sl_grave_find(&graves, sl_track_hist_hash(t));
    return !mix_has(t) && (g < 0 || !sl_grave_buried(&graves.g[g]));
}

/* Take up to want songs from pool, in a random order. */
static void mix_take(int *pool, int n, int want, uint32_t *seed)
{
    for (int i = 0; i < n && want > 0 && mix_n < MIX_MAX; i++)
    {
        int j = i + (int)(mix_rand(seed) % (uint32_t)(n - i));
        int t = pool[j]; pool[j] = pool[i]; pool[i] = t;
        if (mix_allowed(t)) { mix_tracks[mix_n++] = t; want--; }
    }
}

static int mix_artist_tracks(const char *artist, int *out, int max)
{
    int n = 0;
    for (int a = 0; a < sl_lib.n_albums && n < max; a++)
    {
        const struct sl_album *al = &sl_lib.albums[a];
        if (!al->artist || strcasecmp(al->artist, artist)) continue;
        for (int k = 0; k < al->n_tracks && n < max; k++) out[n++] = al->tracks[k];
    }
    return n;
}

/* Songs whose genre tag falls in the style (pet_music_genre decides, as it
 * does for the dancing). Only the matching genres' files are read. */
static int mix_style_tracks(enum pet_dance style, int *out, int max)
{
    static struct tagcache_search tcs;
    int32_t seeks[24];
    int ns = 0, n = 0;
    char buf[MAX_PATH];
    if (!tagcache_is_usable() || !tagcache_search(&tcs, tag_genre)) return 0;
    while (ns < 24 && tagcache_get_next(&tcs, buf, sizeof buf))
        if (buf[0] && strcmp(buf, "<Untagged>") && pet_music_genre(buf) == style)
            seeks[ns++] = tcs.result_seek;
    tagcache_search_finish(&tcs);
    for (int i = 0; i < ns && n < max; i++)
    {
        if (!tagcache_search(&tcs, tag_filename)) break;
        tagcache_search_add_filter(&tcs, tag_genre, seeks[i]);
        while (n < max && tagcache_get_next(&tcs, buf, sizeof buf))
        {
            int t = sl_lib_find_path(buf);
            if (t >= 0) out[n++] = t;
            if ((n & 63) == 63) yield();
        }
        tagcache_search_finish(&tcs);
    }
    return n;
}

static void mix_record(void)
{
    pet_load();
    graves_load();
    mix_n = 0;
    int *pool = malloc(MIX_POOL * sizeof *pool);
    if (!pool || !sl_lib.n_tracks) { free(pool); return; }
    uint32_t seed = hist_clock() / 86400u * 2654435761u + pet.species * 7919u + mix_rerolls * 104729u;
    const char *fav = sl_pet_favorite(&pet);
    int n;
    if (fav) { n = mix_artist_tracks(fav, pool, MIX_POOL); mix_take(pool, n, 4, &seed); }
    for (int i = 0; i < SL_PET_TASTE; i++)
    {
        const struct sl_pet_taste *ts = &pet.taste[i];
        if (!ts->artist[0] || ts->minutes < 3 || (fav && !strcmp(fav, ts->artist))) continue;
        n = mix_artist_tracks(ts->artist, pool, MIX_POOL);
        mix_take(pool, n, 2, &seed);
    }
    /* and a few of what your swipes under Your taste say you'll like */
    int *picks = NULL;
    n = ts_picks(&picks);
    if (picks) mix_take(picks, n < 20 ? n : 20, 3, &seed);
    free(picks);
    int opener = mix_n ? 1 : 0;              /* the favourite stays in front */
    enum pet_dance style = (enum pet_dance)pet_music_loves[pet.species];
    if (style != PET_DANCE_NONE)
    {
        n = mix_style_tracks(style, pool, MIX_POOL);
        mix_take(pool, n, MIX_MAX, &seed);
    }
    /* not enough taste yet: whatever it can grab */
    for (int tries = 0; mix_n < MIX_MAX && tries < MIX_MAX * 8; tries++)
    {
        int t = (int)(mix_rand(&seed) % (uint32_t)sl_lib.n_tracks);
        if (mix_allowed(t)) mix_tracks[mix_n++] = t;
    }
    for (int i = mix_n - 1; i > opener; i--)   /* shuffle into a mix */
    {
        int j = opener + (int)(mix_rand(&seed) % (uint32_t)(i - opener + 1));
        int t = mix_tracks[i]; mix_tracks[i] = mix_tracks[j]; mix_tracks[j] = t;
    }
    free(pool);
    sl_log("mixtape: %d songs (favorite %s)", mix_n, fav ? fav : "none");
}

/* ---- the cassette ---------------------------------------------------------- */
static lv_obj_t *mix_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *mix_reel_make(lv_obj_t *parent, int cx, int cy)
{
    lv_obj_t *r = mix_rect(parent, cx - 34, cy - 34, 68, 68, LV_RADIUS_CIRCLE, 0xefe9dc);
    lv_obj_set_style_border_width(r, 4, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x5a4636), 0);
    lv_obj_set_style_transform_pivot_x(r, 34, 0);
    lv_obj_set_style_transform_pivot_y(r, 34, 0);
    for (int k = 0; k < 3; k++)              /* the sprocket's three teeth */
    {
        static const int8_t tx[] = { 0, 12, -12 }, ty[] = { -13, 7, 7 };
        mix_rect(r, 34 - 4 - 4 + tx[k], 34 - 4 - 4 + ty[k], 8, 8, 2, 0x3a2e24);
    }
    mix_rect(r, 34 - 4 - 7, 34 - 4 - 7, 14, 14, LV_RADIUS_CIRCLE, 0x3a2e24);
    return r;
}

static bool mix_playing_tape(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!is_playing() || !id3) return false;
    int t = sl_lib_find_path(id3->path);
    return t >= 0 && mix_has(t);
}

static void mix_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!mix_page_obj || pages[npages - 1] != mix_page_obj || !mix_playing_tape()) return;
    mix_reel_angle = (mix_reel_angle + 150) % 3600;
    for (int i = 0; i < 2; i++) lv_obj_set_style_transform_rotation(mix_reel[i], mix_reel_angle, 0);
}

static void mix_cassette(lv_obj_t *body)
{
    int w = W - 28, h = 292;
    uint32_t shell = pet_shell_color();
    lv_obj_t *c = mix_rect(body, 0, 0, w, h, 22, shell);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x2a2320), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_40, 0);
    pet_screw(c, 10, 10); pet_screw(c, w - 26, 10);
    pet_screw(c, 10, h - 26); pet_screw(c, w - 26, h - 26);

    /* the label, written on by hand (a slight tilt and a felt-tip blue) */
    lv_obj_t *label = mix_rect(c, 30, 20, w - 60, 172, 10, 0xf6ecd2);
    mix_rect(label, 0, 0, w - 60, 26, 10, 0xe24e3b);
    mix_rect(label, 0, 14, w - 60, 12, 0, 0xe24e3b);
    lv_obj_t *side = text(label, "A", &ss_bold_28, 0xf6ecd2);
    lv_obj_set_style_text_color(side, lv_color_hex(0x1f3a8a), 0);
    lv_obj_set_pos(side, 12, 30);
    char b[64];
    snprintf(b, sizeof b, "%s's mix", mix_pet_name());
    lv_obj_t *title = text(label, b, &ss_semibold_26, 0x1f3a8a);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1f3a8a), 0);
    lv_obj_set_width(title, w - 130);
    label_lines(title, 1);
    lv_obj_set_pos(title, 44, 34);
    lv_obj_set_style_transform_rotation(title, -18, 0);
    int y, m, d;
    uint32_t now = hist_clock();
    sl_hist_date(now >= SL_HIST_VALID_CLOCK ? now : SL_HIST_VALID_CLOCK, &y, &m, &d, NULL);
    snprintf(b, sizeof b, "for you, with love  %d/%d", d, m);
    lv_obj_t *sub = text(label, b, &ss_regular_18, 0x3b4a7a);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x3b4a7a), 0);
    lv_obj_set_pos(sub, 46, 72);
    lv_obj_set_style_transform_rotation(sub, -12, 0);

    /* the window with its two reels and the tape between them */
    lv_obj_t *win = mix_rect(label, (w - 60) / 2 - 150, 100, 300, 64, 32, 0x1b1a1f);
    mix_rect(win, 70, 22, 160, 20, 4, 0x4a3426);
    mix_reel[0] = mix_reel_make(win, 64, 32);
    mix_reel[1] = mix_reel_make(win, 236, 32);
    /* the bottom edge, with its guide holes */
    lv_obj_t *lip = mix_rect(c, w / 2 - 130, h - 64, 260, 58, 12, 0x2a2320);
    lv_obj_set_style_bg_opa(lip, LV_OPA_30, 0);
    for (int k = 0; k < 4; k++)
        mix_rect(lip, 30 + k * 62 + (k > 1 ? 20 : 0), 20, 16, 16, LV_RADIUS_CIRCLE, 0x1b1a1f);
}

/* ---- the page ------------------------------------------------------------ */
static void mix_fill(void);

static void mix_play_cb(lv_event_t *e)
{
    (void)e;
    sfx_play(SFX_TAPE_CLICK);
    if (play_list(mix_tracks, mix_n, 0, false)) np_show_auto();
}

static void mix_save_cb(lv_event_t *e)
{
    (void)e;
    char name[PL_NAME_MAX + 1], raw[64], path[MAX_PATH], tmp[MAX_PATH + 8], line[MAX_PATH + 220];
    snprintf(raw, sizeof raw, "%s's Mixtape", mix_pet_name());
    pl_clean_name(name, sizeof name, raw);
    if (!name[0] || !pl_path_for(path, sizeof path, name)) { toast_warn("Could not name the playlist"); return; }
    /* a tape saved before keeps its songs: this one gets the next number */
    char base[PL_NAME_MAX + 1];
    strlcpy(base, name, sizeof base);
    for (int k = 2; k < 1000 && pl_exists(path); k++)
    {
        snprintf(name, sizeof name, "%.*s %d", PL_NAME_MAX - 5, base, k);
        if (!pl_path_for(path, sizeof path, name)) { toast_warn("Could not name the playlist"); return; }
    }
    if (pl_exists(path)) { toast_warn("Could not name the playlist"); return; }
    mkdir(sl_playlist_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool okay = fd >= 0 && pl_write(fd, "#EXTM3U\n", 8);
    for (int i = 0; okay && i < mix_n; i++)
    {
        int n = pl_song_lines(line, sizeof line, mix_tracks[i], true);
        okay = n > 0 && pl_write(fd, line, n);
    }
    if (okay) okay = fsync(fd) == 0;
    if (fd >= 0 && close(fd) < 0) okay = false;
    if (okay) okay = rename(tmp, path) >= 0;
    if (!okay) { remove(tmp); toast_warn("Could not save the tape. Check card space."); return; }
    sl_log("mixtape saved: %s", path);
    sfx_play(SFX_TAPE_CLICK);
    snprintf(line, sizeof line, "Saved to Playlists as %s", name);
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_PLAYLISTS, NULL, line);
}

static void mix_new_cb(lv_event_t *e)
{
    (void)e;
    mix_rerolls++;
    mix_record();
    sfx_play(SFX_TAPE_CLICK);
    mix_fill();
}

static void mix_song_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < mix_n && play_list(mix_tracks, mix_n, i, false)) np_show_auto();
}

static lv_obj_t *mix_button(lv_obj_t *parent, const char *label, bool primary, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, lv_pct(100), 68);
    lv_obj_set_style_radius(b, SL_R_LG, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? 0xe24e3b : C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_23, 0xffffff);
    if (primary) lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    on_click(b, cb, NULL);
    return b;
}

static void mix_fill(void)
{
    lv_obj_clean(mix_body);
    mix_reel[0] = mix_reel[1] = NULL;
    mix_cassette(mix_body);
    char b[160];
    const char *fav = sl_pet_favorite(&pet);
    enum pet_dance style = (enum pet_dance)pet_music_loves[pet.species];
    const char *loves = style != PET_DANCE_NONE ? pet_music_style_name[style] : NULL;
    if (fav && loves)
        snprintf(b, sizeof b, "%s picked these. It opens with %s, its favorite, and it's mad about %s.",
                 mix_pet_name(), fav, loves);
    else if (loves)
        snprintf(b, sizeof b, "%s is still learning your taste, so it packed the tape with %s. "
                 "Play music together and it will pick a favorite.", mix_pet_name(), loves);
    else
        snprintf(b, sizeof b, "%s grabbed whatever it could reach. Play music together to teach it your taste.",
                 mix_pet_name());
    lv_obj_t *note = text(mix_body, b, &ss_regular_18, C_SUB);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_ver(note, 6, 0);
    if (!mix_n)
    {
        lv_obj_t *e = text(mix_body, "No songs to record yet. Add music to the library first.", &ss_regular_18, C_DIM);
        lv_obj_set_width(e, lv_pct(100));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        return;
    }
    mix_button(mix_body, LV_SYMBOL_PLAY "  PLAY THE TAPE", true, mix_play_cb);
    mix_button(mix_body, "SAVE AS A PLAYLIST", false, mix_save_cb);
    mix_button(mix_body, "RECORD A NEW ONE", false, mix_new_cb);
    int half = (mix_n + 1) / 2;
    for (int i = 0; i < mix_n; i++)
    {
        if (i == 0 || i == half) section(mix_body, i ? "SIDE B" : "SIDE A");
        const struct sl_track *tr = &sl_lib.tracks[mix_tracks[i]];
        lv_obj_t *r = box(mix_body);
        lv_obj_set_size(r, lv_pct(100), 64);
        lv_obj_set_style_radius(r, SL_R_MD, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        snprintf(b, sizeof b, "%c%d", i < half ? 'A' : 'B', (i < half ? i : i - half) + 1);
        lv_obj_t *num = text(r, b, &ss_semibold_21, 0xe24e3b);
        lv_obj_set_style_text_color(num, lv_color_hex(0xe24e3b), 0);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_t *t1 = text(r, tr->title, &ss_semibold_21, 0xffffff);
        lv_obj_set_pos(t1, 60, 6);
        lv_obj_set_width(t1, W - 28 - 70);
        label_lines(t1, 1);
        lv_obj_t *t2 = text(r, tr->artist ? tr->artist : sl_lib.albums[tr->album].artist, &ss_regular_16, C_SUB);
        lv_obj_set_pos(t2, 60, 36);
        lv_obj_set_width(t2, W - 28 - 70);
        label_lines(t2, 1);
        on_click(r, mix_song_cb, (void *)(intptr_t)i);
    }
}

static void mix_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != mix_page_obj) return;
    if (mix_timer) { lv_timer_delete(mix_timer); mix_timer = NULL; }
    mix_page_obj = mix_body = mix_reel[0] = mix_reel[1] = NULL;
}

static void mix_page(void)
{
    if (page_bring_back(mix_page_obj)) return;
    pet_load();
    if (pet.species == SL_PET_UNCHOSEN || sl_pet_is_egg(&pet) || pet.dead)
    { toast("Your pet makes tapes once it has hatched."); return; }
    mix_record();           /* the same tape all day, and never stale after a rescan */
    char b[64];
    snprintf(b, sizeof b, "%s's Mixtape", mix_pet_name());
    lv_obj_t *p;
    mix_body = page_new(b, &p);
    mix_page_obj = p;
    lv_obj_add_event_cb(p, mix_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(mix_body, 10, 0);
    mix_fill();
    mix_timer = lv_timer_create(mix_tick, 50, NULL);
    sfx_play(SFX_TAPE_CLICK);
    page_push(p);
}
