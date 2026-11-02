/* The pet hears the music player: it dances on the beat in a style picked by
 * the song's genre at any hour (music beats bedtime), dozes off to it only
 * when its energy is spent, and wonders where the music went when it is
 * paused. It keeps what it heard in its save: minutes, songs, and a learned
 * favorite artist, and each species has a style it loves. Listening is banked
 * and paid out as Fun, or Energy while it dozed, when the pet is opened.
 * Times are LVGL ticks. GPLv2+. */
enum pet_dance { PET_DANCE_NONE, PET_DANCE_BOP, PET_DANCE_HEADBANG, PET_DANCE_GROOVE,
                 PET_DANCE_SWAY, PET_DANCE_LULLED };

#define PET_MUSIC_NOTES 5

static struct {
    uint32_t last, beat, paused_at, song_at, quiet_at;
    uint32_t listen_ms;                 /* toward the next banked minute */
    uint32_t key;                       /* hash of the playing file's path */
    int avg, level, interval;           /* peak 0..1023, meter 0..8, ms per beat */
    int odd;                            /* onsets in a row off the learned tempo */
    unsigned beats;
    bool playing, paused, loved, favorite;
    enum pet_dance genre;
    char artist[SL_PET_ARTIST_SIZE];
    struct pcm_peaks peaks;
} pet_mus = { .interval = 500 };

static struct { int x, y, age; } pet_notes[PET_MUSIC_NOTES];
static bool quiz_clip_active(void);

/* The style each species can't resist: Bun bops, Blob sways, Bot grooves to
 * electronic, Slop headbangs, and Spaghet loves a pop song. */
static const uint8_t pet_music_loves[SL_PET_SPECIES_COUNT] = {
    PET_DANCE_NONE, PET_DANCE_BOP, PET_DANCE_SWAY, PET_DANCE_GROOVE,
    PET_DANCE_HEADBANG, PET_DANCE_BOP };
static const char *const pet_music_style_name[] = {
    "", "POP", "ROCK", "DANCE MUSIC", "SLOW SONGS", "" };

static int pet_music_hour(void)
{
    struct tm *clock = get_time();
    return clock ? clock->tm_hour : 12;
}
static bool pet_music_late(void) { int h = pet_music_hour(); return h >= 23 || h < 6; }

static bool pet_music_has(const char *text, const char *const *words)
{
    char low[48];
    int n = 0;
    for (; text && text[n] && n < (int)sizeof low - 1; n++)
        low[n] = text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n];
    low[n] = 0;
    for (; *words; words++) if (strstr(low, *words)) return true;
    return false;
}

static enum pet_dance pet_music_genre(const char *genre)
{
    static const char *const loud[] = { "metal", "rock", "punk", "grunge", "hardcore",
                                        "emo", "thrash", NULL };
    static const char *const groove[] = { "hip", "rap", "funk", "disco", "dance", "house",
                                          "techno", "electr", "edm", "r&b", "rnb", "soul",
                                          "reggae", "trap", "dubstep", "drum", NULL };
    static const char *const soft[] = { "jazz", "classical", "ambient", "folk", "acoustic",
                                        "blues", "lo-fi", "lofi", "chill", "soundtrack",
                                        "piano", "new age", "lullab", NULL };
    return pet_music_has(genre, loud) ? PET_DANCE_HEADBANG :
           pet_music_has(genre, groove) ? PET_DANCE_GROOVE :
           pet_music_has(genre, soft) ? PET_DANCE_SWAY : PET_DANCE_BOP;
}

/* Remember artists by their tag, skipping placeholders that say nothing. */
static void pet_music_artist(const struct mp3entry *id3)
{
    const char *a = id3 ? (id3->artist && id3->artist[0] ? id3->artist : id3->albumartist) : NULL;
    pet_mus.artist[0] = 0;
    if (!a || !a[0] || !strncasecmp(a, "unknown", 7) || !strncasecmp(a, "various", 7) ||
        !strncasecmp(a, "<untagged", 9)) return;
    for (const char *c = a; *c; c++) if ((unsigned char)*c < 32) return;
    strlcpy(pet_mus.artist, a, sizeof pet_mus.artist);
}

static void pet_music_upper(char *dst, const char *src, size_t size)
{
    size_t n = 0;
    for (; src[n] && n + 1 < size; n++)
        dst[n] = src[n] >= 'a' && src[n] <= 'z' ? src[n] - 32 : src[n];
    dst[n] = 0;
}

/* Learn the tempo from the gap between onsets. A gap of about two beats is
 * a missed one; four off-tempo gaps in a row mean the song changed pace. */
static void pet_music_learn(int gap)
{
    int beat = pet_mus.interval;
    if (gap > beat * 17 / 10 && gap < beat * 23 / 10) gap /= 2;
    if (gap >= beat * 7 / 10 && gap <= beat * 14 / 10)
    {
        pet_mus.interval = (beat * 3 + gap) / 4;
        pet_mus.odd = 0;
    }
    else if (++pet_mus.odd >= 4)
    {
        pet_mus.interval = gap < 300 ? 300 : gap > 1000 ? 1000 : gap;
        pet_mus.odd = 0;
    }
}

/* Poll the player about 20 times a second. Beat detection compares the
 * playback channel's peak against its running average and learns the tempo
 * from the gaps; silent or unreadable output (a muted simulator, a quiet
 * intro) falls back to a steady 120 BPM so the pet never freezes mid-song. */
static void pet_music_poll(void)
{
    uint32_t now = lv_tick_get(), dt = now - pet_mus.last;
    if (pet_mus.last && dt < 40) return;
    if (!pet_mus.last || dt > 2000) dt = 0;
    pet_mus.last = now;
    int st = audio_status();
    bool paused = (st & AUDIO_STATUS_PLAY) && (st & AUDIO_STATUS_PAUSE);
    if (paused && !pet_mus.paused) pet_mus.paused_at = now;
    pet_mus.paused = paused;
    /* A quiz snippet is a question, not a song to dance to. */
    if (!(st & AUDIO_STATUS_PLAY) || paused || quiz_clip_active())
    {
        pet_mus.playing = false;
        pet_mus.level = pet_mus.avg = 0;
        return;
    }
    pet_load();
    struct mp3entry *id3 = audio_current_track();
    uint32_t key = id3 ? sl_pet_bytes_checksum(id3->path, strlen(id3->path)) : 0;
    if (!pet_mus.playing) pet_mus.beat = pet_mus.quiet_at = now;
    pet_mus.playing = true;
    if (key != pet_mus.key)
    {
        pet_mus.key = key;
        pet_mus.song_at = now;
        pet_mus.genre = pet_music_genre(id3 ? id3->genre_string : NULL);
        pet_music_artist(id3);
        sl_pet_heard_song(&pet);
        const char *fav = sl_pet_favorite(&pet);
        pet_mus.favorite = fav && pet_mus.artist[0] && !strcmp(fav, pet_mus.artist);
        pet_mus.loved = pet_mus.favorite ||
                        (pet.species < SL_PET_SPECIES_COUNT &&
                         pet_music_loves[pet.species] == pet_mus.genre);
    }
    pet_mus.listen_ms += dt;
    if (pet_mus.listen_ms >= 60000)
    {
        pet_mus.listen_ms -= 60000;
        sl_pet_listen(&pet, pet_mus.artist, pet.energy < 20, pet_mus.loved);
        if (pet.music_minutes % 5 == 0 && !pet_save()) sl_log("pet: could not save listening");
    }

    mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &pet_mus.peaks);
    int peak = (int)(MAX(pet_mus.peaks.left, pet_mus.peaks.right) >> 5);
    if (peak > 1023) peak = 1023;
    if (peak > 24) pet_mus.quiet_at = now;
    bool heard = now - pet_mus.quiet_at < 1500;
    int since = (int)(now - pet_mus.beat);
    int refractory = pet_mus.interval * 3 / 5 < 280 ? 280 : pet_mus.interval * 3 / 5;
    bool onset = heard && peak > pet_mus.avg * 13 / 10 + 16 && since >= refractory;
    bool beat = onset || since >= (heard ? pet_mus.interval * 2 : 500);
    pet_mus.avg = (pet_mus.avg * 7 + peak) / 8;
    int meter = heard ? (peak * 9 / 512 > 8 ? 8 : peak * 9 / 512) : 4;
    pet_mus.level = meter > pet_mus.level ? meter : pet_mus.level > 0 ? pet_mus.level - 1 : 0;
    if (beat)
    {
        if (onset) pet_music_learn(since);
        if (!heard) pet_mus.interval = 500;
        pet_mus.beat = now;
        pet_mus.beats++;
    }
}

/* What the pet does with the music right now, before care, petting or
 * mania get their say. Music keeps it up past bedtime; only an empty
 * battery lets the song lull it to sleep. */
static enum pet_dance pet_music_dance(void)
{
    if (!pet_mus.playing) return PET_DANCE_NONE;
    if (pet.energy < 20) return PET_DANCE_LULLED;
    return pet_mus.genre;
}

static bool pet_music_wondering(void)
{
    return pet_mus.paused && lv_tick_get() - pet_mus.paused_at < 60000;
}

/* Between songs the pet hums its favorite: six seconds every half minute. */
static bool pet_music_humming(void)
{
    return !pet_mus.playing && !pet_music_wondering() && !pet.dead &&
           pet.energy >= 20 && !pet_music_late() && sl_pet_favorite(&pet) &&
           lv_tick_get() / 6000 % 5 == 4;
}

/* 0 on the beat, rising to 999 as the next one nears. */
static int pet_music_phase(void)
{
    uint32_t since = lv_tick_get() - pet_mus.beat;
    return since > 999 ? 999 : (int)since;
}

static const char *pet_music_caption(enum sl_pet_stage stage)
{
    static char line[48];
    if (pet.dead || pet.species == SL_PET_UNCHOSEN) return NULL;
    if (pet_mus.playing && lv_tick_get() - pet_mus.song_at < 2500)
        return stage == SL_PET_EGG ? "THE EGG WIGGLES" : "OOH, NEW SONG!";
    if (pet_music_wondering()) return "WHERE'D THE MUSIC GO?";
    if (stage == SL_PET_EGG) return pet_mus.playing ? "THE EGG IS LISTENING" : NULL;
    if (pet_music_humming())
    {
        char name[SL_PET_ARTIST_SIZE];
        pet_music_upper(name, sl_pet_favorite(&pet), sizeof name);
        snprintf(line, sizeof line, "HUMMING %s", name);
        return line;
    }
    enum pet_dance dance = pet_music_dance();
    if (dance == PET_DANCE_LULLED) return "LULLED TO SLEEP";
    if (dance != PET_DANCE_NONE && pet_mus.favorite)
    {
        char name[SL_PET_ARTIST_SIZE];
        pet_music_upper(name, pet_mus.artist, sizeof name);
        snprintf(line, sizeof line, "FAVORITE! %s", name);
        return line;
    }
    if (dance != PET_DANCE_NONE && pet_mus.loved && lv_tick_get() / 5000 % 2)
        return "LOVES THIS SONG!";
    switch (dance) {
    case PET_DANCE_BOP: return "BOPPING ALONG";
    case PET_DANCE_HEADBANG: return "HEADBANGING!";
    case PET_DANCE_GROOVE: return "GROOVING";
    case PET_DANCE_SWAY: return "SWAYING ALONG";
    default: return NULL;
    }
}

/* The page's footnote: what the two of you have heard together. */
static const char *pet_music_memory(void)
{
    static char line[96];
    if (pet.dead || !pet.music_minutes) return NULL;
    unsigned h = pet.music_minutes / 60, m = pet.music_minutes % 60;
    const char *fav = sl_pet_favorite(&pet);
    char name[SL_PET_ARTIST_SIZE];
    int n = h ? snprintf(line, sizeof line, "%uH %02uM OF MUSIC, %u SONGS TOGETHER", h, m,
                         (unsigned)pet.songs_heard)
              : snprintf(line, sizeof line, "%u MIN OF MUSIC, %u SONGS TOGETHER", m,
                         (unsigned)pet.songs_heard);
    if (fav && n > 0 && n < (int)sizeof line)
    {
        pet_music_upper(name, fav, sizeof name);
        snprintf(line + n, sizeof line - n, "\nFAVORITE ARTIST: %s", name);
    }
    else if (pet.species < SL_PET_SPECIES_COUNT && n > 0 && n < (int)sizeof line)
        snprintf(line + n, sizeof line - n, "\nLOVES %s",
                 pet_music_style_name[pet_music_loves[pet.species]]);
    return line;
}

/* A small speaker on the shelf that thumps with the music. */
static void pet_music_speaker(void)
{
    pet_px(81, 20, 8, 10, 1);
    pet_px(82, 21, 6, 8, 2);
    pet_px(84, 22, 2, 1, 3);
    bool thump = pet_mus.playing && pet_music_phase() < 160;
    pet_disc(84, 26, thump ? 3 : 2, 1);
    pet_px(84, 26, 1, 1, 3);
    if (thump && pet_mus.level > 2)
    {
        pet_px(91, 23, 1, 5, 1);
        if (pet_mus.level > 5) pet_px(93, 21, 1, 9, 1);
    }
}

/* A disco ball drops from the ceiling for dance music, loud pop and loved
 * songs, throwing specks of light that sweep the wall. */
static void pet_music_disco(unsigned t)
{
    pet_px(53, 0, 1, 4, 1);
    pet_disc(53, 8, 4, 1);
    pet_disc(53, 8, 3, 2);
    for (int i = 0; i < 4; i++)
    {
        int fx = 51 + (int)((t / 2 + i * 3) % 5), fy = 6 + i;
        pet_px(fx, fy, 1, 1, 3);
    }
    bool flash = pet_music_phase() < 140;
    for (int i = 0; i < 5; i++)
    {
        int sx = (int)((t * 2 + i * 29 + pet_mus.beats * 7) % 124) + 2;
        int sy = 38 + (i * 7) % 18;
        if (sx > 5 && sx < 46 && sy < 37) continue;   /* not on the window */
        if (flash) pet_spark(sx, sy, 1); else pet_px(sx, sy, 1, 1, 3);
    }
}

static const char *const pet_note_glyph[7] = {
    "..#..", "..##.", "..#.#", "..#..", "###..", "###..", ".#..." };

/* Notes rise from the pet's head, one per beat (fewer when it's dozing). */
static void pet_music_notes(int x, int y, bool sleepy)
{
    static unsigned spawned;
    if (spawned != pet_mus.beats && (!sleepy || pet_mus.beats % 3 == 0))
        for (int i = 0; i < PET_MUSIC_NOTES; i++)
            if (!pet_notes[i].age)
            {
                pet_notes[i].x = x + 2 + (int)(pet_mus.beats * 11 % 28);
                pet_notes[i].y = y - 2;
                pet_notes[i].age = 1;
                break;
            }
    spawned = pet_mus.beats;
    for (int i = 0; i < PET_MUSIC_NOTES; i++)
    {
        if (!pet_notes[i].age) continue;
        int age = pet_notes[i].age++;
        int nx = pet_notes[i].x + ((age / 4) % 2 ? 1 : -1), ny = pet_notes[i].y - age;
        if (age > (sleepy ? 34 : 24) || ny < 1) { pet_notes[i].age = 0; continue; }
        pet_glyph(pet_note_glyph, 7, nx, ny, 0);
    }
}
