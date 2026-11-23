/* Music Bip Bop Smarty: queue-safe clips and library questions. GPLv2+. */
enum { QUIZ_SONG, QUIZ_ARTIST, QUIZ_ALBUM, QUIZ_YEAR, QUIZ_MOVIE, QUIZ_TV, QUIZ_MUSIC_FACT, QUIZ_KIND_COUNT };
enum { QUIZ_MODE_MUSIC, QUIZ_MODE_MOVIES, QUIZ_MODE_TV, QUIZ_MODE_MIXED };
struct quiz_trivia_item { int kind; const char *question; const char *choices[4]; int answer; };
#define QUIZ_TRIVIA_MAX_PER_MODE 256
static const struct quiz_trivia_item quiz_trivia[] = {
#include "sloppy_quiz_trivia.inc"
};
static struct {
    int mode, kind, round, score, streak, answer, track, clip_index, trivia_index;
    int asked_songs[512], asked_song_count;
    int asked_music_kind[512], asked_music_key[512], asked_music_count;
    uint64_t asked_movie[QUIZ_TRIVIA_MAX_PER_MODE / 64];
    uint64_t asked_tv[QUIZ_TRIVIA_MAX_PER_MODE / 64];
    uint32_t question_deadline;
    uint32_t elimination_deadline;
    int eliminated_count, choice_count;
    int elimination_order[4];
    bool answered;
    uint32_t rng;
    char choices[5][96];
    int choice_album[5];              /* album behind each choice, for covers */
    char fact_question[240];          /* fits two whole tags: see quiz_tag_ok */
    /* The game show around the questions: rounds of eight, three strikes,
     * cash, the Wheel of Chance and a bonus round after every clear. */
    int stage, qnum, strikes, phase, best_streak, correct, round_cash, shown_cash;
    uint32_t react_total, auto_at, disc_stop;
    uint8_t pip[8];
    bool recorded;
    int wheel_rot, wheel_from, wheel_to, wheel_seg, wager, bonus_n, bonus_answer, bonus_used[8];
    uint32_t wheel_start, bonus_deadline, bonus_wait;
    bool wheel_spinning, bonus_won;
    char bonus_prompt[180], bonus_big[120], bonus_opt[2][96];
    struct {
        bool active;
        int amount, index, sample_index;
        unsigned long elapsed, offset;
        uint32_t deadline, queue_hash;
        int resume_index;               /* global_status resume point before the clip */
        uint32_t resume_crc32, resume_elapsed, resume_offset;
        bool was_paused, was_playing;
        /* The clip counts from when it is heard, not from when it was asked
         * for: loading and seeking a song takes a moment, longer on a card. */
        bool audible, hushed;
        uint32_t asked_at, heard_elapsed, restore_at;
        bool heard_seen;
        char path[MAX_PATH];
    } clip;
    bool no_clip;                       /* the last clip never played: ask something else */
} quiz;
static lv_obj_t *quiz_page_obj, *quiz_question, *quiz_score, *quiz_choices[5], *quiz_next_button;
static lv_obj_t *quiz_stage, *quiz_mode_label, *quiz_feedback, *quiz_progress, *quiz_glyph;
static lv_obj_t *quiz_timer_label, *quiz_round_label, *quiz_pips, *quiz_cover, *quiz_choice_cover[5];
static lv_obj_t *quiz_fx, *quiz_overlay, *quiz_wheel, *quiz_wheel_title, *quiz_wheel_desc, *quiz_wheel_btn;
static lv_obj_t *quiz_wager_grid, *quiz_bonus_bar, *quiz_bonus_count, *quiz_bonus_card;
static lv_obj_t *quiz_bonus_prompt, *quiz_bonus_big, *quiz_bonus_btn[2];
static lv_timer_t *quiz_timer;
static const char *quiz_value(int kind, int track);

static uint32_t quiz_rand(void)
{ quiz.rng = quiz.rng * 1664525u + 1013904223u; return quiz.rng; }
static bool quiz_has_text(const char *value, const char *needle)
{
    if (!value || !needle) return false;
    size_t n = strlen(needle);
    for (const char *p = value; *p; p++)
        if (!strncasecmp(p, needle, n)) return true;
    return false;
}
static bool quiz_tag_ok(const char *value)
{
    if (!value || !value[0]) return false;
    /* Every tag is quoted whole, in a question or on an answer. */
    if (strlen(value) >= sizeof quiz.choices[0]) return false;
    bool visible = false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
    {
        /* Reject ASCII controls, DEL and C1 controls. C1 controls arrive in
         * UTF-8 as C2 80..C2 9F; bare 0x80..0xBF bytes are continuation bytes
         * inside ordinary letters (every Hebrew letter uses them). */
        if (*p < 0x20 || *p == 0x7f) return false;
        if (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) return false;
        if (*p != ' ') visible = true;
    }
    if (!visible) return false;
    if (strstr(value, "\xef\xbf\xbd") || quiz_has_text(value, "various artists") ||
        quiz_has_text(value, "unknown artist") || quiz_has_text(value, "untitled") ||
        !strcasecmp(value, "unknown") || !strcasecmp(value, "n/a") || !strcasecmp(value, "none")) return false;
    return true;
}
static bool quiz_track_ok(int index)
{
    if (index < 0 || index >= sl_lib.n_tracks) return false;
    const struct sl_track *track = &sl_lib.tracks[index];
    if (track->album < 0 || track->album >= sl_lib.n_albums || !track->file || !track->file[0]) return false;
    const struct sl_album *album = &sl_lib.albums[track->album];
    return quiz_tag_ok(track->title) && quiz_tag_ok(album->artist) && quiz_tag_ok(album->title);
}
static int quiz_music_key(int kind, int track)
{
    if (track < 0 || track >= sl_lib.n_tracks) return -1;
    return (kind == QUIZ_ARTIST || kind == QUIZ_YEAR) ? sl_lib.tracks[track].album : track;
}
static bool quiz_music_seen(int kind, int key)
{
    for (int i = 0; i < quiz.asked_music_count; i++)
        if (quiz.asked_music_kind[i] == kind && quiz.asked_music_key[i] == key) return true;
    return false;
}
static void quiz_remember_music(int kind, int track)
{
    int key = quiz_music_key(kind, track);
    if (key < 0 || quiz.asked_music_count >= (int)(sizeof quiz.asked_music_key / sizeof quiz.asked_music_key[0]) ||
        quiz_music_seen(kind, key)) return;
    int i = quiz.asked_music_count++;
    quiz.asked_music_kind[i] = kind;
    quiz.asked_music_key[i] = key;
}
static bool quiz_same_path_at(int index, const char *path)
{
    struct playlist_track_info info;
    return index >= 0 && index < playlist_amount() &&
           playlist_get_track_info(NULL, index, &info) >= 0 && !strcmp(info.filename, path);
}
/* A fingerprint of the entries around the clip (the song it interrupted and
 * what follows), used to tell whether the user changed the queue during the
 * clip. Only a few entries: each file name read is card I/O on the device,
 * and hashing a 10,000-song queue twice per question froze the UI. The
 * restore also checks the amount and the clip's own path. */
static uint32_t quiz_queue_hash(int skip)
{
    uint32_t hash = 2166136261u;
    int amount = playlist_amount();
    int from = skip - 2 < 0 ? 0 : skip - 2, to = skip + 3 > amount ? amount : skip + 3;
    for (int i = from; i < to; i++)
    {
        if (i == skip) continue;
        struct playlist_track_info info;
        if (playlist_get_track_info(NULL, i, &info) < 0) return 0;
        const unsigned char *p = (const unsigned char *)info.filename;
        while (*p) { hash = (hash ^ *p++) * 16777619u; }
        hash = (hash ^ 0xff) * 16777619u;
    }
    return hash;
}
static bool quiz_clip_active(void) { return quiz.clip.active; }
/* Rockbox loads (and decodes) a track's cover before its audio for every
 * art slot held. During a clip nobody shows that cover - the quiz has its
 * own thumbnail - so the slot is let go for the clip and taken back before
 * the user's own music is restored. On the R1 a clip took 1.7 to 3.7 s to
 * sound, one never did (the device log, 2026-10-13). */
static bool quiz_aa_released;
static void quiz_aa_release(void)
{
    if (quiz_aa_released || aa_slot < 0) return;
    playback_release_aa_slot(aa_slot);
    quiz_aa_released = true;
}
static void quiz_aa_reclaim(void)
{
    if (!quiz_aa_released) return;
    struct dim d = { W, W };
    aa_slot = playback_claim_aa_slot(&d);
    quiz_aa_released = false;
}

static void quiz_restore_clip(void)
{
    if (!quiz.clip.active) return;
    quiz.clip.active = false;
    quiz_aa_reclaim();
    int status = audio_status();
    bool queue_unchanged = playlist_amount() == quiz.clip.amount + 1 &&
                           quiz_queue_hash(quiz.clip.sample_index) == quiz.clip.queue_hash &&
                           quiz_same_path_at(quiz.clip.sample_index, quiz.clip.path);
    if (!queue_unchanged)
    { sl_log("quiz: queue changed during clip, leaving playback alone"); return; }
    bool still_on_clip = cur_index() == quiz.clip.sample_index &&
                         (quiz.clip.hushed || (status & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)));
    /* Stop before touching the queue: with crossfade enabled Rockbox would
     * otherwise blend the clip into the restored song for seconds. */
    /* each step timed: leaving Your taste held the screen for a second
     * here on the R1 (22 Nov), and which step is not known yet */
    uint32_t t0 = sl_ms();
    if (still_on_clip && !quiz.clip.hushed)
        audio_stop();
    uint32_t t1 = sl_ms();
    playlist_delete(NULL, quiz.clip.sample_index);
    uint32_t t2 = sl_ms();
    if (!still_on_clip)
    { sl_log("quiz: user moved on during clip; removed temporary item only"); return; }
    if (!quiz.clip.was_playing)
    {
        /* audio_stop() saved the clip as the resume point. Put the user's
         * back, or the next Play would restart the queue from track 1. */
        global_status.resume_index = quiz.clip.resume_index;
        global_status.resume_crc32 = quiz.clip.resume_crc32;
        global_status.resume_elapsed = quiz.clip.resume_elapsed;
        global_status.resume_offset = quiz.clip.resume_offset;
        status_save(true);
        sl_log("quiz: stopped standalone clip and restored stopped library");
        return;
    }
    /* Resume by time only. The saved byte offset is the decoder's read
     * position, which runs seconds ahead of what was audible (more with
     * crossfade), and the MP3 codec prefers it over the elapsed time. */
    playlist_start(quiz.clip.index, quiz.clip.elapsed, 0);
    if (quiz.clip.was_paused) audio_pause();
    sl_log("quiz: restored track %d at %lu ms (clock %lu); stop %u ms, remove %u ms, start %u ms", quiz.clip.index,
           quiz.clip.elapsed, (unsigned long)sl_ms(), (unsigned)(t1 - t0), (unsigned)(t2 - t1), (unsigned)(sl_ms() - t2));
}
/* Silence the clip now (an answer, or its 3 s are up) and bring the user's
 * song back a moment later, once the answer's sound has played: restarting
 * a song can switch the output sample rate, which resets the mixer and cuts
 * off any sound effect half way (a crackle). */
static void quiz_hush_clip(void)
{
    if (!quiz.clip.active || quiz.clip.hushed) return;
    if (cur_index() == quiz.clip.sample_index && (audio_status() & AUDIO_STATUS_PLAY) &&
        quiz_same_path_at(quiz.clip.sample_index, quiz.clip.path))
    {
        audio_stop();
        quiz.clip.hushed = true;
    }
    uint32_t now = sl_ms();
    int32_t sound_left = (int32_t)(sfx_playing_until - now);
    quiz.clip.restore_at = now + (sound_left > 0 ? (uint32_t)sound_left + 60 : 0);
    if (!quiz.clip.hushed) quiz_restore_clip();     /* the user already moved on */
}
/* Has the clip started to sound? Its song has to be current and its time
 * moving, and sound has to be reaching the output: the time can move while
 * the decoder fills the buffer and the mixer still plays nothing, which ran
 * the clock on silence. A passage quiet enough to show no level counts once
 * its time has moved well on. */
static bool quiz_clip_heard(void)
{
    struct mp3entry *id3 = audio_current_track();
    int status = audio_status();
    if (!id3 || !(status & AUDIO_STATUS_PLAY) || (status & AUDIO_STATUS_PAUSE) ||
        strcmp(id3->path, quiz.clip.path)) return false;
    if (!quiz.clip.heard_seen)
    {
        quiz.clip.heard_seen = true;
        quiz.clip.heard_elapsed = id3->elapsed;
        return false;
    }
    if (id3->elapsed <= quiz.clip.heard_elapsed || id3->elapsed - quiz.clip.heard_elapsed < 40) return false;
    struct pcm_peaks peaks;
    mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);
    return MAX(peaks.left, peaks.right) > 64 || id3->elapsed - quiz.clip.heard_elapsed >= 600;
}
/* Bluetooth headphones play what the mixer sends a little later */
#define QUIZ_BT_LAG_MS 250
static bool quiz_find_clip_track(void)
{
    struct mp3entry *current = audio_current_track();
    if (!sl_lib.n_tracks) return false;
    if ((audio_status() & AUDIO_STATUS_PLAY) && current &&
        !quiz_same_path_at(cur_index(), current->path)) return false;
    int first = (int)(quiz_rand() % sl_lib.n_tracks);
    for (int tries = 0; tries < sl_lib.n_tracks; tries++)
    {
        int t = (first + tries) % sl_lib.n_tracks;
        const char *title = quiz_value(QUIZ_SONG, t);
        /* a clip needs 3 s of song; a shorter one would run into the next track */
        char path[MAX_PATH];
        if (!quiz_track_ok(t) || !title || !title[0] || !sl_lib.tracks[t].file ||
            (sl_lib.tracks[t].len > 0 && sl_lib.tracks[t].len < 4000) ||
            (current && !strcasecmp(sl_track_path(t, path, sizeof path), current->path))) continue;
        bool seen = false;
        for (int i = 0; i < quiz.asked_song_count; i++)
            if (quiz.asked_songs[i] >= 0 && quiz.asked_songs[i] < sl_lib.n_tracks &&
                !strcasecmp(quiz_value(QUIZ_SONG, quiz.asked_songs[i]), title)) { seen = true; break; }
        if (seen) continue;
        quiz.track = t;
        quiz.clip_index = -1;
        return true;
    }
    return false;
}
static bool clip_insert(int track, int position);
static bool quiz_clip_begin(void)
{
    if (quiz.kind != QUIZ_SONG || quiz.track < 0 || quiz.track >= sl_lib.n_tracks) return false;
    quiz_restore_clip();
    struct mp3entry *id3 = audio_current_track();
    int status = audio_status();
    bool was_playing = (status & AUDIO_STATUS_PLAY) != 0;
    if (was_playing && !id3)
    { toast_warn("Playback changed. Try again."); return false; }
    int index = cur_index();
    if (was_playing && !quiz_same_path_at(index, id3->path))
    { toast_warn("Playback changed. Try again."); return false; }
    int original_amount = playlist_amount();
    /* After boot the saved queue is only loaded on the first Play. Load it
     * now: playlist_create would truncate its control file and lose it. */
    if (!was_playing && original_amount == 0 && playlist_resume() != -1)
    {
        original_amount = playlist_amount();
        index = cur_index();
    }
    if (!was_playing && original_amount == 0 && playlist_create(NULL, NULL) < 0)
    { toast_warn("Could not start the song clip"); return false; }
    /* A clip taken out while stopped leaves Rockbox's playing index on the
     * gap, at the end of a one-song list: "after the playing song" was then
     * past the end. In the list, and the clip after it, by number. */
    if (index >= original_amount) index = original_amount ? original_amount - 1 : 0;
    if (index < 0) index = 0;
    quiz.clip.amount = original_amount;
    quiz.clip.index = index;
    /* A track skip can briefly expose bogus codec position data. Never feed
     * that position back into playlist_start when the clip is restored. */
    bool valid_position = was_playing && id3->length && id3->elapsed <= id3->length + 2000UL;
    quiz.clip.elapsed = valid_position ? visible_elapsed(id3) : 0;
    quiz.clip.offset = valid_position ? id3->offset : 0;
    quiz.clip.was_paused = (status & AUDIO_STATUS_PAUSE) != 0;
    quiz.clip.was_playing = was_playing;
    quiz.clip.resume_index = global_status.resume_index;
    quiz.clip.resume_crc32 = global_status.resume_crc32;
    quiz.clip.resume_elapsed = global_status.resume_elapsed;
    quiz.clip.resume_offset = global_status.resume_offset;
    return clip_insert(quiz.track, original_amount ? index + 1 : 0);
}
/* The clip itself: track's middle, at position in the list. */
static bool clip_insert(int track, int position)
{
    /* No sync: the playing song stops and playlist_start() below writes the
     * control file anyway, so a flush and fsync here only delay the clip. */
    char path[MAX_PATH];
    sl_track_path(track, path, sizeof path);
    int inserted = playlist_insert_track(NULL, path, position, false, false);
    if (inserted < 0)
    { toast_warn("Could not start the song clip"); return false; }
    quiz.clip.sample_index = inserted;
    quiz.clip.queue_hash = quiz_queue_hash(inserted);
    strlcpy(quiz.clip.path, path, sizeof quiz.clip.path);
    quiz.clip.active = true;
    quiz.clip.audible = quiz.clip.hushed = quiz.clip.heard_seen = false;
    quiz.clip.asked_at = sl_ms();
    quiz.clip.deadline = 0;             /* 3 s from when it is heard */
    long length = sl_lib.tracks[track].len;
    unsigned long start = length > 6000 ? (unsigned long)(length / 2) : 0;
    /* Start reading the file near the middle, not at its first byte:
     * buffering from 0 read half a 30 MB FLAC off the card before the clip
     * could sound. MP3, FLAC and Ogg Vorbis resync to the next frame from
     * a byte offset; other formats keep a plain seek by time. */
    unsigned long offset = 0;
    if (start && length > 0 && (sl_suffix(path, ".mp3") || sl_suffix(path, ".flac") || sl_suffix(path, ".ogg")))
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            off_t size = lseek(fd, 0, SEEK_END);
            close(fd);
            if (size > 0) offset = (unsigned long)((uint64_t)size * start / (unsigned long)length);
        }
    }
    if (quiz.clip.was_playing) audio_stop(); /* no crossfade into the clip */
    quiz_aa_release();
    playlist_start(inserted, start, offset);
    sl_log("quiz: library track %d, middle clip from %lu ms (byte %lu) at %lu: %s",
           track, start, offset, (unsigned long)sl_ms(), path);
    return true;
}
/* Another clip straight after this one (Your taste's cards): the queue and
 * the place in it were kept when the first began, so only the clip changes
 * and the song you were playing doesn't come back in between. */
static bool clip_swap(int track)
{
    if (track < 0 || track >= sl_lib.n_tracks) return false;
    if (!quiz.clip.active || quiz.clip.hushed || playlist_amount() != quiz.clip.amount + 1 ||
        !quiz_same_path_at(quiz.clip.sample_index, quiz.clip.path))
    {
        quiz_restore_clip();
        quiz.kind = QUIZ_SONG;
        quiz.track = track;
        return quiz_clip_begin();
    }
    audio_stop();
    playlist_delete(NULL, quiz.clip.sample_index);
    quiz.clip.audible = quiz.clip.hushed = quiz.clip.heard_seen = false;
    quiz.clip.asked_at = sl_ms();
    /* where the last clip was, by number: the delete leaves the playing
     * index on the gap, so "after the playing song" landed one further on,
     * past the end when your list was one song, and the slot left between
     * was read as the playlist file "/" ("Error accessing playlist file",
     * 2 s each, a test R1 2026-11-08) */
    if (clip_insert(track, quiz.clip.sample_index)) return true;
    quiz.clip.active = false;           /* the clip is gone: the queue is as it was */
    quiz_aa_reclaim();
    if (quiz.clip.was_playing) playlist_start(quiz.clip.index, quiz.clip.elapsed, 0);
    return false;
}
static bool quiz_unique(const char *candidate, int count)
{
    if (!candidate || !candidate[0]) return false;
    for (int i = 0; i < count; i++) if (!strcasecmp(quiz.choices[i], candidate)) return false;
    return true;
}
static const char *quiz_value(int kind, int track)
{
    if (track < 0 || track >= sl_lib.n_tracks) return NULL;
    const struct sl_track *t = &sl_lib.tracks[track];
    if (t->album < 0 || t->album >= sl_lib.n_albums) return NULL;
    const struct sl_album *album = &sl_lib.albums[t->album];
    return kind == QUIZ_SONG ? t->title : kind == QUIZ_ARTIST ? album->artist : album->title;
}
/* An unasked movie or TV prompt, marked as asked; -1 when none are left. */
static int quiz_take_trivia(int kind)
{
    int indexes[QUIZ_TRIVIA_MAX_PER_MODE], total = 0;
    uint64_t *asked = kind == QUIZ_MOVIE ? quiz.asked_movie : quiz.asked_tv;
    for (int i = 0; i < (int)(sizeof quiz_trivia / sizeof quiz_trivia[0]); i++)
        if (quiz_trivia[i].kind == kind)
        {
            if (total >= QUIZ_TRIVIA_MAX_PER_MODE) return -1;
            indexes[total++] = i;
        }
    if (!total) return -1;
    int first = quiz_rand() % total;
    for (int tries = 0; tries < total; tries++)
    {
        int slot = (first + tries) % total;
        uint64_t bit = (uint64_t)1 << (slot & 63);
        if (asked[slot >> 6] & bit) continue;
        asked[slot >> 6] |= bit;
        return indexes[slot];
    }
    return -1;
}
static bool quiz_prepare_kind(int kind)
{
    for (int i = 0; i < 5; i++) quiz.choice_album[i] = -1;
    if (kind == QUIZ_MOVIE || kind == QUIZ_TV)
    {
        int index = quiz_take_trivia(kind);
        if (index >= 0)
        {
            const struct quiz_trivia_item *item = &quiz_trivia[index];
            quiz.trivia_index = index;
            quiz.answer = item->answer;
            quiz.choice_count = 4;
            for (int i = 0; i < 4; i++)
                strlcpy(quiz.choices[i], item->choices[i] ? item->choices[i] : "", sizeof quiz.choices[i]);
            for (int i = 3; i > 0; i--)
            {
                int other = quiz_rand() % (i + 1);
                char saved[sizeof quiz.choices[0]];
                strlcpy(saved, quiz.choices[i], sizeof saved);
                strlcpy(quiz.choices[i], quiz.choices[other], sizeof quiz.choices[i]);
                strlcpy(quiz.choices[other], saved, sizeof quiz.choices[other]);
                if (quiz.answer == i) quiz.answer = other;
                else if (quiz.answer == other) quiz.answer = i;
            }
            return true;
        }
        return false;
    }
    if (!sl_lib.n_tracks || !sl_lib.n_albums) return false;
    if (kind == QUIZ_SONG && (quiz.no_clip || !quiz_find_clip_track())) return false;
    if (kind != QUIZ_SONG)
    {
        int first = quiz_rand() % sl_lib.n_tracks;
        bool found = false;
        for (int i = 0; i < sl_lib.n_tracks; i++)
        {
            int candidate = (first + i) % sl_lib.n_tracks;
            if (!quiz_track_ok(candidate)) continue;
            if (kind == QUIZ_YEAR && (sl_lib.albums[sl_lib.tracks[candidate].album].year < 1900 ||
                                      sl_lib.albums[sl_lib.tracks[candidate].album].year > 2100)) continue;
            const struct sl_album *ca = &sl_lib.albums[sl_lib.tracks[candidate].album];
            /* "Who made the album Nirvana?" answers itself. */
            if (kind == QUIZ_ARTIST && (quiz_has_text(ca->title, ca->artist) || quiz_has_text(ca->artist, ca->title))) continue;
            int key = quiz_music_key(kind, candidate);
            if (quiz_music_seen(kind, key)) continue;
            quiz.track = candidate;
            found = true;
            break;
        }
        if (!found) return false;
    }
    if (quiz.track < 0 || quiz.track >= sl_lib.n_tracks) return false;
    const struct sl_track *t = &sl_lib.tracks[quiz.track];
    if (t->album < 0 || t->album >= sl_lib.n_albums) return false;
    const struct sl_album *album = &sl_lib.albums[t->album];
    if (kind == QUIZ_MUSIC_FACT)
    {
        if (!t->title || !t->title[0] || !album->artist || !album->artist[0]) return false;
        bool truth = (quiz_rand() & 1) == 0;
        const char *claim_artist = album->artist;
        if (!truth)
        {
            int start = quiz_rand() % sl_lib.n_tracks;
            claim_artist = NULL;
            for (int tries = 0; tries < sl_lib.n_tracks; tries++)
            {
                int other_index = (start + tries) % sl_lib.n_tracks;
                const struct sl_track *other = &sl_lib.tracks[other_index];
                if (!quiz_track_ok(other_index)) continue;
                const char *candidate = sl_lib.albums[other->album].artist;
                if (candidate && candidate[0] && strcasecmp(candidate, album->artist))
                { claim_artist = candidate; break; }
            }
            if (!claim_artist) return false;
        }
        snprintf(quiz.fact_question, sizeof quiz.fact_question,
                 "True or false: %s is by %s?", t->title, claim_artist);
        strlcpy(quiz.choices[0], "True", sizeof quiz.choices[0]);
        strlcpy(quiz.choices[1], "False", sizeof quiz.choices[1]);
        quiz.choices[2][0] = quiz.choices[3][0] = '\0';
        quiz.choice_count = 2;
        quiz.answer = truth ? 0 : 1;
        quiz_remember_music(kind, quiz.track);
        return true;
    }
    if (kind == QUIZ_YEAR)
    {
        int year = sl_lib.albums[t->album].year;
        if (year < 1900 || year > 2100) return false;
        static const int differences[] = { -2, -1, 1, 2 };
        int correct_position = quiz_rand() % 4;
        for (int i = 0; i < 4; i++)
            snprintf(quiz.choices[i], sizeof quiz.choices[i], "%d", year + differences[i]);
        snprintf(quiz.choices[correct_position], sizeof quiz.choices[correct_position], "%d", year);
        quiz.choice_count = 4;
        quiz.answer = correct_position;
        quiz_remember_music(kind, quiz.track);
        return true;
    }
    const char *correct = quiz_value(kind, quiz.track);
    if (!correct || !correct[0]) return false;
    strlcpy(quiz.choices[0], correct, sizeof quiz.choices[0]);
    if (kind == QUIZ_ALBUM) quiz.choice_album[0] = t->album;
    int count = 1;
    int start = quiz_rand() % sl_lib.n_tracks;
    int wanted = kind == QUIZ_SONG ? 5 : 4;
    for (int tries = 0; tries < sl_lib.n_tracks && count < wanted; tries++)
    {
        int candidate = (start + tries) % sl_lib.n_tracks;
        if (!quiz_track_ok(candidate)) continue;
        const char *value = quiz_value(kind, candidate);
        char short_value[sizeof quiz.choices[0]];
        strlcpy(short_value, value ? value : "", sizeof short_value);
        if (quiz_unique(short_value, count))
        {
            if (kind == QUIZ_ALBUM) quiz.choice_album[count] = sl_lib.tracks[candidate].album;
            strlcpy(quiz.choices[count++], short_value, sizeof quiz.choices[0]);
        }
    }
    if (count < wanted) return false;
    quiz.choice_count = wanted;
    quiz.answer = quiz_rand() % wanted;
    char saved[sizeof quiz.choices[0]];
    strlcpy(saved, quiz.choices[quiz.answer], sizeof saved);
    strlcpy(quiz.choices[quiz.answer], quiz.choices[0], sizeof quiz.choices[0]);
    strlcpy(quiz.choices[0], saved, sizeof quiz.choices[0]);
    int saved_album = quiz.choice_album[quiz.answer];
    quiz.choice_album[quiz.answer] = quiz.choice_album[0];
    quiz.choice_album[0] = saved_album;
    if (kind == QUIZ_SONG && quiz.asked_song_count < (int)(sizeof quiz.asked_songs / sizeof quiz.asked_songs[0]))
        quiz.asked_songs[quiz.asked_song_count++] = quiz.track;
    if (kind != QUIZ_SONG) quiz_remember_music(kind, quiz.track);
    return true;
}
/* ---- The show. Styled after Apple's iQuiz for the click-wheel iPod: a
 * deep-space screen with cyan, magenta and lime neon; five rounds of eight
 * questions; three strikes in a round ends the game; answers pay cash, more
 * for speed and streaks; every cleared round spins the Wheel of Chance for a
 * wagered bonus round (Lightning true/false or This or That). Bests are
 * kept in quiz.txt. */
#define QUIZ_ROUNDS    5
#define QUIZ_PER_ROUND 8
#define QUIZ_STRIKES   3
#define QUIZ_BONUS     6
#define QUIZ_STATS     ROCKBOX_DIR "/sloppy/quiz.txt"
#define QUIZ_STAGE_H   192
/* body top pad, HUD, lights and stage with their gaps, and a bottom margin */
#define QUIZ_CHOICE_ROOM (H - SB_H - 66 - 4 - 44 - 24 - QUIZ_STAGE_H - 3 * 8 - 14)
#define QUIZ_Q_TOP     40    /* the question's box beside the record, */
#define QUIZ_Q_H       102   /* clear of NEXT under it */
#define QUIZ_ROW_MIN   56    /* the smallest answer row: its letter and a margin */
enum { QP_PLAY, QP_WHEEL, QP_BONUS, QP_BONUS_DONE, QP_OVER };
enum { QB_LIGHTNING, QB_SORT };
#define QC_BASE    0x050a1a
#define QC_DEEP    0x0b1638
#define QC_PANEL   0x0d1a3a
#define QC_LINE    0x22336a
#define QC_CYAN    0x22d3ee
#define QC_MAGENTA 0xd946ef
#define QC_LIME    0x84cc16
#define QC_AMBER   0xfbbf24
#define QC_RED     0xf43f5e
#define QC_WHITE   0xfafcff   /* text() turns pure white into the theme's text colour */
#define QC_DIM     0x8a9cc6
/* The wheel after Tiny Decisions: flat slices and a hub that carries the
 * pointer, coloured in the quiz's own neon (magenta round to cyan) and set
 * in navy with a cyan rim. The lime slice is the x5 jackpot. */
static const struct { uint8_t type, mult; uint32_t color; } quiz_wheel_segs[8] = {
    { QB_LIGHTNING, 2, 0xc026d3 }, { QB_SORT, 2, 0x9333ea }, { QB_LIGHTNING, 3, 0x6d4aed },
    { QB_SORT, 2, 0x3b6cf0 }, { QB_LIGHTNING, 2, 0x0e8fd8 }, { QB_SORT, 3, 0x0aa9c7 },
    { QB_LIGHTNING, 5, 0x6fae16 }, { QB_SORT, 2, 0xdb2f9c },
};
static struct { int best_cash, best_streak, best_react, games, wins, best_stage; bool loaded; } quiz_stats;
static int quiz_tick_second;          /* last countdown second that ticked */
static uint32_t quiz_fx_at;
static int quiz_fx_kind, quiz_wheel_last;
static void quiz_game_over(int how);
static void quiz_round_clear(void);
static lv_obj_t *quiz_wheel_face;

static void quiz_stats_load(void)
{
    if (quiz_stats.loaded) return;
    quiz_stats.loaded = true;
    char b[96];
    int fd = open(QUIZ_STATS, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 5 || strncmp(b, "SLQ1 ", 5)) return;
    b[got] = 0;
    int *v[] = { &quiz_stats.best_cash, &quiz_stats.best_streak, &quiz_stats.best_react,
                 &quiz_stats.games, &quiz_stats.wins, &quiz_stats.best_stage };
    char *p = b + 5;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
    {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p) break;
        *v[i] = n < 0 ? 0 : n > 99999999 ? 99999999 : (int)n;
        p = end;
    }
}
static void quiz_stats_save(void)
{
    char b[96];
    int len = snprintf(b, sizeof b, "SLQ1 %d %d %d %d %d %d\n", quiz_stats.best_cash, quiz_stats.best_streak,
                       quiz_stats.best_react, quiz_stats.games, quiz_stats.wins, quiz_stats.best_stage);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(QUIZ_STATS ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;   /* a power cut must not leave an empty file */
    if (close(fd) < 0) ok = false;
    if (!ok || rename(QUIZ_STATS ".tmp", QUIZ_STATS) < 0) remove(QUIZ_STATS ".tmp");
}
/* Average time to a right answer; 0 means none yet, so never below 1 ms. */
static int quiz_avg_react(void)
{
    int react = quiz.correct ? (int)(quiz.react_total / quiz.correct) : 0;
    return quiz.correct && react < 1 ? 1 : react;
}
/* Counts a finished (or abandoned) game once; returns which bests it beat:
 * 1 cash, 2 streak, 4 reaction time. */
static unsigned quiz_record(bool champion)
{
    if (quiz.recorded || !quiz.round) return 0;
    quiz.recorded = true;
    quiz_stats_load();
    unsigned beaten = 0;
    quiz_stats.games++;
    if (champion) quiz_stats.wins++;
    if (quiz.score > quiz_stats.best_cash) { quiz_stats.best_cash = quiz.score; beaten |= 1; }
    if (quiz.best_streak > quiz_stats.best_streak) { quiz_stats.best_streak = quiz.best_streak; beaten |= 2; }
    int react = quiz_avg_react();
    if (quiz.correct >= 5 && (!quiz_stats.best_react || react < quiz_stats.best_react))
    { quiz_stats.best_react = react; beaten |= 4; }
    if (quiz.stage > quiz_stats.best_stage) quiz_stats.best_stage = quiz.stage;
    quiz_stats_save();
    return beaten;
}

static void quiz_money(char *b, size_t n, int v)
{
    char digits[16];
    snprintf(digits, sizeof digits, "%d", v < 0 ? -v : v);
    size_t o = 0, len = strlen(digits);
    if (v < 0 && o + 1 < n) b[o++] = '-';
    if (o + 1 < n) b[o++] = '$';
    for (size_t i = 0; i < len && o + 1 < n; i++)
    {
        if (i && (len - i) % 3 == 0 && o + 2 < n) b[o++] = ',';
        b[o++] = digits[i];
    }
    b[o] = 0;
}
/* Base pay rises each round; speed adds up to four times the base and a
 * streak adds ten per cent per answer in a row, up to double. */
static int quiz_payout(uint32_t remaining)
{
    int base = quiz.stage * 100;
    int speed = (int)((uint64_t)base * 4 * (remaining > 10000 ? 10000 : remaining) / 10000);
    int tenths = 10 + (quiz.streak < 10 ? quiz.streak : 10);
    return ((base + speed) * tenths / 10 + 5) / 10 * 10;
}
static uint32_t quiz_kind_color(int kind)
{
    static const uint32_t c[] = { QC_CYAN, QC_MAGENTA, QC_LIME, QC_AMBER, QC_MAGENTA, QC_CYAN, QC_LIME };
    return kind >= 0 && kind < (int)(sizeof c / sizeof c[0]) ? c[kind] : QC_CYAN;
}

/* ---- drawing helpers */
static void quiz_fill(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t c, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(c);
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}
static void quiz_circle(lv_layer_t *layer, int cx, int cy, int r, uint32_t c, lv_opa_t opa)
{ quiz_fill(layer, cx - r, cy - r, cx + r, cy + r, c, opa, LV_RADIUS_CIRCLE); }
static void quiz_ring(lv_layer_t *layer, int cx, int cy, int r, int w, uint32_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_width = w;
    d.border_color = lv_color_hex(c);
    d.border_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &d, &a);
}
static void quiz_arc(lv_layer_t *layer, int cx, int cy, int r, int w, int a0, int a1, uint32_t c, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = r;
    d.width = w;
    d.start_angle = ((a0 % 360) + 360) % 360;
    d.end_angle = ((a1 % 360) + 360) % 360;
    d.color = lv_color_hex(c);
    d.opa = opa;
    lv_draw_arc(layer, &d);
}
static void quiz_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int w, uint32_t c, lv_opa_t opa)
{
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = lv_color_hex(c);
    l.opa = opa;
    l.width = w;
    l.round_start = l.round_end = 1;
    l.p1.x = x1; l.p1.y = y1; l.p2.x = x2; l.p2.y = y2;
    lv_draw_line(layer, &l);
}
static void quiz_draw_text(lv_layer_t *layer, const char *s, const lv_font_t *f, uint32_t c, lv_opa_t opa,
                           int x1, int y1, int x2, int y2)
{
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = f;
    l.color = lv_color_hex(c);
    l.opa = opa;
    l.text = s;
    l.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_label(layer, &l, &a);
}
/* Neon type: the label drawn a few times around itself, faintly, before
 * the label itself. */
static void quiz_glow_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) { lv_event_set_ext_draw_size(e, 3); return; }
    lv_obj_t *l = lv_event_get_current_target(e);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    lv_obj_init_draw_label_dsc(l, LV_PART_MAIN, &d);
    d.text = lv_label_get_text(l);
    d.color = lv_color_hex((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    d.opa = 70;
    lv_area_t a;
    lv_obj_get_content_coords(l, &a);
    static const int8_t off[8][2] = { {-2,0}, {2,0}, {0,-2}, {0,2}, {-1,-1}, {1,1}, {-1,1}, {1,-1} };
    for (int i = 0; i < 8; i++)
    {
        lv_area_t b = a;
        lv_area_move(&b, off[i][0], off[i][1]);
        lv_draw_label(lv_event_get_layer(e), &d, &b);
    }
}
static lv_obj_t *quiz_glow(lv_obj_t *label, uint32_t color)
{
    lv_obj_add_event_cb(label, quiz_glow_cb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(uintptr_t)color);
    lv_obj_add_event_cb(label, quiz_glow_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(uintptr_t)color);
    lv_obj_refresh_ext_draw_size(label);
    return label;
}
static void quiz_anim_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void quiz_anim_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void quiz_anim_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
/* The iPod's "hardware wipe": things slide in from the right. */
static void quiz_slide_in(lv_obj_t *o, int delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, quiz_anim_tx);
    lv_anim_set_values(&a, 56, 0);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    quiz_anim_tx(o, 56);
    lv_anim_start(&a);
}
static void quiz_style_panel(lv_obj_t *o, uint32_t border, int radius)
{
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(QC_PANEL), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x081026), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_opa(o, 150, 0);
}
/* A glowing pill button; top 0 makes a quiet outlined one. */
static lv_obj_t *quiz_button(lv_obj_t *parent, const char *label, int w, int h,
                             uint32_t top, uint32_t bottom, uint32_t ink, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    if (top)
    {
        lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(b, lv_color_hex(top), 0);
        lv_obj_set_style_shadow_width(b, 18, 0);
        lv_obj_set_style_shadow_opa(b, 90, 0);
    }
    else
    {
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(QC_LINE), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(QC_PANEL), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_translate_y(b, 2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(top ? bottom : QC_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_21, ink);
    lv_obj_center(l);
    on_click(b, cb, ud);
    return b;
}
/* "+$450" rising off an answer. */
static void quiz_float(const char *s, uint32_t color, lv_obj_t *near)
{
    if (!quiz_page_obj || !near) return;
    lv_obj_t *l = quiz_glow(text(quiz_page_obj, s, &sl_sym_32, color), color);
    lv_obj_update_layout(l);
    lv_obj_align_to(l, near, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_exec_cb(&a, quiz_anim_ty);
    lv_anim_set_values(&a, 0, -70);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, quiz_anim_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 450);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_completed_cb(&a, lv_obj_delete_anim_completed_cb);
    lv_anim_start(&a);
}

/* A new round announces itself over the stage. */
static void quiz_banner(const char *s)
{
    if (!quiz_page_obj || !quiz_stage) return;
    lv_obj_t *l = quiz_glow(text(quiz_page_obj, s, &sl_sym_48, QC_WHITE), QC_CYAN);
    lv_obj_update_layout(l);
    lv_obj_align_to(l, quiz_stage, LV_ALIGN_TOP_MID, 0, -8);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_exec_cb(&a, quiz_anim_ty);
    lv_anim_set_values(&a, 30, -10);
    lv_anim_set_duration(&a, 1300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, quiz_anim_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 800);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_completed_cb(&a, lv_obj_delete_anim_completed_cb);
    lv_anim_start(&a);
}

/* ---- particles: sparkles for right answers, confetti for wins, a puff of
 * dust where an eliminated answer was. */
static void quiz_fx_draw(lv_event_t *e)
{
    uint32_t t = lv_tick_get() - quiz_fx_at;
    if (t > 1100) return;
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    static const uint32_t palette[] = { QC_CYAN, QC_MAGENTA, QC_LIME, 0xffffff, QC_AMBER };
    float s = t / 1000.0f;
    lv_opa_t opa = (lv_opa_t)(255 - t * 255 / 1100);
    int n = quiz_fx_kind == 2 ? 12 : quiz_fx_kind == 1 ? 48 : 30;
    for (int i = 0; i < n; i++)
    {
        uint32_t h = (uint32_t)(i + 1) * 2654435761u ^ quiz_fx_at * 40503u;
        float ang = (i * 360.0f / n + (h % 23)) * 0.0174533f;
        float speed = quiz_fx_kind == 2 ? 40 + h % 50 : 150 + h % (quiz_fx_kind == 1 ? 240 : 150);
        int x = cx + (int)(cosf(ang) * speed * s);
        int y = cy + (int)(sinf(ang) * speed * s + (quiz_fx_kind == 2 ? -30 : 260) * s * s);
        if (quiz_fx_kind == 2) { quiz_circle(layer, x, y, 4 + (int)(s * 10), QC_DIM, opa / 3); continue; }
        uint32_t c = palette[(h >> 8) % 5];
        int size = 2 + (h >> 12) % 4;
        if (quiz_fx_kind == 1 && i % 2) quiz_fill(layer, x - 3, y - 5, x + 3, y + 5, c, opa, 1);
        else quiz_circle(layer, x, y, size, c, opa);
        if (i % 3 == 0)   /* a twinkle */
        {
            quiz_fill(layer, x - size * 3, y, x + size * 3, y, 0xffffff, opa / 2, 0);
            quiz_fill(layer, x, y - size * 3, x, y + size * 3, 0xffffff, opa / 2, 0);
        }
    }
}
static void quiz_burst(lv_obj_t *near, int kind)
{
    if (!quiz_fx || !near) return;
    lv_obj_update_layout(near);
    lv_area_t a;
    lv_obj_get_coords(near, &a);
    lv_area_t page;
    lv_obj_get_coords(quiz_page_obj, &page);
    lv_obj_set_pos(quiz_fx, (a.x1 + a.x2) / 2 - 190 - page.x1, (a.y1 + a.y2) / 2 - 190 - page.y1);
    lv_obj_remove_flag(quiz_fx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quiz_fx);
    quiz_fx_at = lv_tick_get();
    quiz_fx_kind = kind;
    lv_obj_invalidate(quiz_fx);
}

/* An album sleeve at any size. Thumbnails exist only at the art cache's
 * sizes, so the nearest one is scaled to fit; until it has been made the
 * sleeve shows the album's own colours and a note. */
static void quiz_art_scale_cb(lv_event_t *e)
{
    lv_obj_t *child = lv_event_get_param(e);
    if (child && lv_obj_check_type(child, &lv_image_class))
        lv_image_set_scale(child, (uint32_t)(uintptr_t)lv_event_get_user_data(e));
}
static lv_obj_t *quiz_sleeve(lv_obj_t *parent, int album, int size)
{
    const struct sl_album *al = &sl_lib.albums[album];
    int thumb = size <= SL_ART_ROW ? SL_ART_ROW : SL_ART_PAGE;
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, SL_R_SM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hsv_to_rgb(al->hue, 55, 58), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hsv_to_rgb((al->hue + 70) % 360, 70, 20), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(c, size / 5, 0);
    lv_obj_set_style_shadow_offset_y(c, size / 16, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(c, 170, 0);
    lv_obj_t *note = text(c, LV_SYMBOL_AUDIO, size > 70 ? &sl_sym_32 : &sl_sym_20, QC_WHITE);
    lv_obj_set_style_text_opa(note, 150, 0);
    lv_obj_center(note);
    lv_obj_add_event_cb(c, quiz_art_scale_cb, LV_EVENT_CHILD_CREATED, (void *)(uintptr_t)(256 * size / thumb));
    sl_art_apply(c, album, thumb);
    return c;
}

/* ---- the spinning record for song clips, or a neon medallion */
static void quiz_disc_draw(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) { lv_event_set_ext_draw_size(e, 26); return; }
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = lv_area_get_width(&a) / 2 - 2;
    uint32_t now = lv_tick_get(), accent = quiz_kind_color(quiz.kind);
    if (quiz.kind == QUIZ_SONG)
    {
        if (quiz.clip.active && quiz.clip.audible && !quiz.clip.hushed)   /* sound rings while the clip plays */
            for (int k = 0; k < 2; k++)
            {
                int grow = (int)((now / 6 + k * 12) % 24);
                quiz_ring(layer, cx, cy, r + grow, 2, QC_CYAN, (lv_opa_t)(170 - grow * 7));
            }
        quiz_circle(layer, cx, cy, r, 0x07070c, LV_OPA_COVER);
        for (int rr = r - 5; rr > r * 45 / 100; rr -= 5) quiz_ring(layer, cx, cy, rr, 1, 0x262838, LV_OPA_COVER);
        int ang = (int)(((quiz.answered ? quiz.disc_stop : now) / 5) % 360);
        for (int k = 0; k < 3; k++)   /* the light catching the grooves */
        {
            quiz_arc(layer, cx, cy, r - 3, r / 2, ang + k * 6, ang + 36 - k * 6, 0xffffff, 12);
            quiz_arc(layer, cx, cy, r - 3, r / 2, ang + 180 + k * 6, ang + 216 - k * 6, 0xffffff, 12);
        }
        int lr = r * 36 / 100;
        quiz_circle(layer, cx, cy, lr, 0xe11d48, LV_OPA_COVER);
        quiz_ring(layer, cx, cy, lr - 6, 1, 0xfda4af, 150);
        quiz_arc(layer, cx, cy, lr - 3, 5, ang + 90, ang + 150, 0xffffff, 110);
        quiz_circle(layer, cx, cy, 4, QC_BASE, LV_OPA_COVER);
        return;
    }
    if (quiz_cover && !lv_obj_has_flag(quiz_cover, LV_OBJ_FLAG_HIDDEN))
    {   /* the sleeve with its record sliding out behind it */
        int vx = a.x1 + 82, vr = 42;
        quiz_circle(layer, vx, cy, vr, 0x07070c, LV_OPA_COVER);
        for (int rr = vr - 4; rr > 18; rr -= 4) quiz_ring(layer, vx, cy, rr, 1, 0x2c2e40, LV_OPA_COVER);
        quiz_arc(layer, vx, cy, vr - 2, vr / 2, 290, 350, 0xffffff, 30);
        quiz_circle(layer, vx, cy, 14, accent, LV_OPA_COVER);
        quiz_circle(layer, vx, cy, 3, QC_BASE, LV_OPA_COVER);
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_mix(lv_color_hex(accent), lv_color_hex(QC_BASE), 90);
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_mix(lv_color_hex(accent), lv_color_hex(QC_BASE), 110);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(QC_BASE);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    d.border_width = 3;
    d.border_color = lv_color_hex(accent);
    d.border_opa = 230;
    d.shadow_width = 22;
    d.shadow_color = lv_color_hex(accent);
    d.shadow_opa = 110;
    lv_area_t c = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &d, &c);
    quiz_ring(layer, cx, cy, r - 9, 1, accent, 90);
    if (quiz.kind == QUIZ_TV)
    {
        quiz_line(layer, cx - 14, cy - 34, cx, cy - 20, 3, 0xffffff, LV_OPA_COVER);
        quiz_line(layer, cx + 14, cy - 34, cx, cy - 20, 3, 0xffffff, LV_OPA_COVER);
        quiz_fill(layer, cx - 32, cy - 20, cx + 32, cy + 24, 0xffffff, LV_OPA_COVER, 9);
        quiz_fill(layer, cx - 26, cy - 14, cx + 16, cy + 18, QC_DEEP, LV_OPA_COVER, 6);
        quiz_circle(layer, cx + 24, cy - 6, 3, accent, LV_OPA_COVER);
        quiz_circle(layer, cx + 24, cy + 6, 3, accent, LV_OPA_COVER);
        return;
    }
    const char *glyph = quiz.kind == QUIZ_MOVIE ? LV_SYMBOL_VIDEO : quiz.kind == QUIZ_MUSIC_FACT ? "?" : LV_SYMBOL_AUDIO;
    quiz_draw_text(layer, glyph, &sl_sym_48, 0xffffff, LV_OPA_COVER, cx - r, cy - 26, cx + r, cy + 30);
}

/* ---- eight question lights, a streak badge and three strike marks */
static void quiz_pips_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cy = (a.y1 + a.y2) / 2;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < QUIZ_PER_ROUND; i++)
    {
        int cx = a.x1 + 9 + i * 23;
        bool current = i == quiz.qnum - 1 && !quiz.answered && quiz.phase == QP_PLAY;
        if (quiz.pip[i] == 1) { quiz_circle(layer, cx, cy, 9, QC_LIME, 70); quiz_circle(layer, cx, cy, 6, QC_LIME, LV_OPA_COVER); }
        else if (quiz.pip[i] == 2) quiz_circle(layer, cx, cy, 6, QC_RED, LV_OPA_COVER);
        else if (current)
        {
            int pulse = (int)(now / 4 % 250);
            quiz_circle(layer, cx, cy, 9, QC_CYAN, (lv_opa_t)(pulse < 125 ? pulse : 250 - pulse));
            quiz_circle(layer, cx, cy, 6, QC_CYAN, LV_OPA_COVER);
        }
        else quiz_ring(layer, cx, cy, 6, 2, QC_LINE, LV_OPA_COVER);
    }
    for (int i = 0; i < QUIZ_STRIKES; i++)
    {
        int cx = a.x2 - 12 - (QUIZ_STRIKES - 1 - i) * 30;
        uint32_t c = i < quiz.strikes ? QC_RED : 0x1f2b52;
        if (i < quiz.strikes) quiz_circle(layer, cx, cy, 11, QC_RED, 50);
        quiz_line(layer, cx - 7, cy - 7, cx + 7, cy + 7, 4, c, LV_OPA_COVER);
        quiz_line(layer, cx + 7, cy - 7, cx - 7, cy + 7, 4, c, LV_OPA_COVER);
    }
    if (quiz.streak >= 2)
    {
        char b[24];
        int tenths = 10 + (quiz.streak < 10 ? quiz.streak : 10);
        snprintf(b, sizeof b, "%d IN A ROW  x%d.%d", quiz.streak, tenths / 10, tenths % 10);
        int x1 = a.x1 + 190, x2 = a.x2 - 96;
        quiz_fill(layer, x1, cy - 12, x2, cy + 12, QC_AMBER, 40, 12);
        quiz_draw_text(layer, b, &ss_semibold_18, QC_AMBER, LV_OPA_COVER, x1, cy - 11, x2, cy + 12);
    }
}

static void quiz_update_score(void)
{
    char money[20], round[16];
    quiz_money(money, sizeof money, quiz.shown_cash);
    lv_label_set_text(quiz_score, money);
    snprintf(round, sizeof round, "ROUND %d", quiz.stage ? quiz.stage : 1);
    if (quiz_round_label) lv_label_set_text(quiz_round_label, round);
    if (quiz_pips) lv_obj_invalidate(quiz_pips);
}
/* The cash counter rolls up like a till; it rings when it gets there. */
static void quiz_cash_roll(void)
{
    if (quiz.shown_cash == quiz.score || !quiz_score) return;
    int diff = quiz.score - quiz.shown_cash, step = diff / 6;
    if (step > -10 && step < 10) step = diff > 0 ? (diff < 10 ? diff : 10) : (diff > -10 ? diff : -10);
    quiz.shown_cash += step;
    quiz_update_score();
    if (quiz.shown_cash == quiz.score && diff > 0) sfx_play_ex(SFX_QUIZ_CASH, 100, 80);
}

static void quiz_choice_look(int i, int look)   /* 0 idle, 1 right, 2 wrong, 3 dimmed */
{
    static const uint32_t top[] = { QC_PANEL, 0x65a30d, 0xbe185d, QC_PANEL };
    static const uint32_t bottom[] = { 0x0a1430, 0x3f6212, 0x831843, 0x0a1430 };
    static const uint32_t edge[] = { QC_LINE, QC_LIME, QC_MAGENTA, QC_LINE };
    lv_obj_t *c = quiz_choices[i];
    lv_obj_set_style_bg_color(c, lv_color_hex(top[look]), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(bottom[look]), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(edge[look]), 0);
    lv_obj_set_style_shadow_opa(c, look == 1 || look == 2 ? 130 : 0, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(edge[look]), 0);
    lv_obj_set_style_bg_opa(c, look == 3 ? 150 : LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(c, look == 3 ? 130 : LV_OPA_COVER, 0);
    lv_obj_t *letter = lv_obj_get_child(c, 1);
    lv_obj_set_style_bg_color(letter, lv_color_hex(look == 1 || look == 2 ? 0xffffff : 0x13244f), 0);
    lv_obj_set_style_bg_opa(letter, look == 1 || look == 2 ? 60 : LV_OPA_COVER, 0);
}

/* The first of the fonts that sets s within max_h at this width, else the
 * last; the height it takes in *h. */
static const lv_font_t *quiz_fit(const char *s, int width, int max_h, const lv_font_t *const *fonts, int n, int *h)
{
    lv_point_t size;
    int i = 0;
    for (;; i++)
    {
        lv_text_get_size(&size, s, fonts[i], 0, 0, width, LV_TEXT_FLAG_NONE);
        if (size.y <= max_h || i == n - 1) break;
    }
    *h = size.y;
    return fonts[i];
}
/* Wraps l in the biggest type that fits max_h; past the smallest it ends
 * in dots on its last whole line. LONG_DOT cuts the label's own text, so
 * it is measured wrapped. */
static void quiz_fit_label(lv_obj_t *l, const char *s, int width, int max_h, const lv_font_t *const *fonts, int n)
{
    int h;
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, s);
    const lv_font_t *f = quiz_fit(lv_label_get_text(l), width, max_h, fonts, n, &h);
    if (h > max_h) h = max_h / lv_font_get_line_height(f) * lv_font_get_line_height(f);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_height(l, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}
/* The record and the question start under the mode and the timer, which
 * grow with the Text size. */
static int quiz_q_top(void) { return LV_MAX(QUIZ_Q_TOP, 9 + lv_font_get_line_height(&ss_semibold_21) + 2); }
/* The question is always shown whole: in the biggest type beside the
 * record, or one size down; past that the stage grows into the answers'
 * room as far as their smallest rows allow, in smaller type only when it
 * must. Returns how far the stage grew. */
static int quiz_set_question(const char *s, int choices)
{
    const lv_font_t *fonts[] = { &ss_semibold_26, &ss_semibold_21, &ss_regular_18, &ss_regular_16,
                                 sl_fonts[0][0] };   /* the smallest, whatever the Text size */
    int width = lv_obj_get_style_width(quiz_question, 0), h, top = quiz_q_top();
    int room = QUIZ_Q_H + QUIZ_CHOICE_ROOM - (top - QUIZ_Q_TOP) -
               (choices ? choices * QUIZ_ROW_MIN + 8 * (choices - 1) : 0);
    lv_label_set_text(quiz_question, s);
    const char *t = lv_label_get_text(quiz_question);   /* as drawn: Arabic letters joined */
    const lv_font_t *f = quiz_fit(t, width, QUIZ_Q_H, fonts, 1, &h);
    if (h > QUIZ_Q_H) f = quiz_fit(t, width, room, fonts + 1, 4, &h);
    lv_obj_set_style_text_font(quiz_question, f, 0);
    lv_obj_set_y(quiz_question, top + (h < QUIZ_Q_H ? (QUIZ_Q_H - h) / 2 : 0));
    int grow = top - QUIZ_Q_TOP + (h > QUIZ_Q_H ? h - QUIZ_Q_H : 0);
    lv_obj_set_height(quiz_stage, QUIZ_STAGE_H + grow);
    return grow;
}

static void quiz_next(void)
{
    quiz_restore_clip();
    if (quiz.phase != QP_PLAY) return;
    if (quiz.round && quiz.strikes >= QUIZ_STRIKES) { quiz_game_over(0); return; }
    if (quiz.qnum >= QUIZ_PER_ROUND) { quiz_round_clear(); return; }
    quiz.answered = false;
    quiz.auto_at = 0;
    quiz.eliminated_count = 0;
    static const int music_kinds[] = { QUIZ_ARTIST, QUIZ_SONG, QUIZ_ALBUM, QUIZ_YEAR, QUIZ_MUSIC_FACT };
    static const int movie_kinds[] = { QUIZ_MOVIE };
    static const int tv_kinds[] = { QUIZ_TV };
    static const int mixed_kinds[] = { QUIZ_SONG, QUIZ_ARTIST, QUIZ_ALBUM, QUIZ_YEAR, QUIZ_MOVIE, QUIZ_TV, QUIZ_MUSIC_FACT };
    const int *kinds = quiz.mode == QUIZ_MODE_MUSIC ? music_kinds :
                       quiz.mode == QUIZ_MODE_MOVIES ? movie_kinds :
                       quiz.mode == QUIZ_MODE_TV ? tv_kinds : mixed_kinds;
    int count = quiz.mode == QUIZ_MODE_MUSIC ? (int)(sizeof music_kinds / sizeof music_kinds[0]) :
                quiz.mode == QUIZ_MODE_MIXED ? QUIZ_KIND_COUNT : 1;
    int preferred = quiz.mode == QUIZ_MODE_MUSIC ? quiz.round % count :
                    quiz.round ? quiz_rand() % count : 0;
    bool ready = false;
    for (int attempt = 0; attempt < count && !ready; attempt++)
    {
        int kind = kinds[(preferred + attempt) % count];
        if (quiz_prepare_kind(kind))
        {
            quiz.kind = kind;
            if (kind != QUIZ_SONG || quiz_clip_begin()) ready = true;
        }
    }
    if (!ready)
    {
        quiz.answered = true;
        quiz.question_deadline = 0;
        quiz_set_question(quiz.round ? "You've answered every available question!" :
                          quiz.mode == QUIZ_MODE_MUSIC ? "Add tagged songs to play My music." : "No questions available.", 0);
        lv_label_set_text(quiz_feedback, quiz.round ? "Back to modes for another game." :
                          quiz.mode == QUIZ_MODE_MUSIC ? "Music questions need readable song tags." :
                          "Try another mode or add music.");
        lv_obj_set_width(quiz_progress, 0);
        if (quiz_timer_label) lv_label_set_text(quiz_timer_label, "");
        for (int i = 0; i < 5; i++) lv_obj_add_flag(quiz_choices[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(quiz_next_button, LV_OBJ_FLAG_HIDDEN);
        if (quiz.round) quiz_game_over(2);
        return;
    }
    quiz.no_clip = false;
    quiz.round++;
    quiz.qnum++;
    /* A clip question's ten seconds start when the clip is heard */
    quiz.question_deadline = quiz.clip.active ? 0 : sl_ms() + 10000;
    char question[sizeof quiz.fact_question];
    if (quiz.kind == QUIZ_MOVIE || quiz.kind == QUIZ_TV)
        strlcpy(question, quiz_trivia[quiz.trivia_index].question, sizeof question);
    else if (quiz.kind == QUIZ_MUSIC_FACT)
        strlcpy(question, quiz.fact_question, sizeof question);
    else
    {
        const struct sl_track *track = &sl_lib.tracks[quiz.track];
        const struct sl_album *album = &sl_lib.albums[track->album];
        if (quiz.kind == QUIZ_SONG) strlcpy(question, "Which song is in this clip?", sizeof question);
        else if (quiz.kind == QUIZ_ARTIST) snprintf(question, sizeof question, "Who made the album %s?", album->title);
        else if (quiz.kind == QUIZ_ALBUM) snprintf(question, sizeof question, "Which album includes %s?", track->title);
        else snprintf(question, sizeof question, "In what year was %s released?", album->title);
    }
    int grow = quiz_set_question(question, quiz.choice_count);
    static const char *modes[] = { "LISTEN CLOSE", "NAME THE ARTIST", "NAME THE ALBUM", "WHAT YEAR?",
                                   "AT THE MOVIES", "ON THE TELLY", "TRUE OR FALSE" };
    uint32_t accent = quiz_kind_color(quiz.kind);
    lv_label_set_text(quiz_mode_label, modes[quiz.kind]);
    lv_obj_set_style_text_color(quiz_mode_label, lv_color_hex(accent), 0);
    lv_obj_set_style_border_color(quiz_stage, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_color(quiz_stage, lv_color_hex(accent), 0);
    /* The album art is the clue when asking who made it or when. */
    if (quiz_cover) { lv_obj_delete(quiz_cover); quiz_cover = NULL; }
    if ((quiz.kind == QUIZ_ARTIST || quiz.kind == QUIZ_YEAR) && quiz_glyph)
    {
        quiz_cover = quiz_sleeve(quiz_glyph, sl_lib.tracks[quiz.track].album, 90);
        lv_obj_align(quiz_cover, LV_ALIGN_LEFT_MID, -4, 0);
    }
    if (quiz_glyph) lv_obj_invalidate(quiz_glyph);
    char hint[64];
    int top_pay = quiz_payout(10000);
    char money[20];
    quiz_money(money, sizeof money, top_pay);
    if (quiz.kind == QUIZ_SONG) snprintf(hint, sizeof hint, "3-second clip. Up to %s.", money);
    else snprintf(hint, sizeof hint, "Fast answers pay up to %s.", money);
    lv_label_set_text(quiz_feedback, hint);
    lv_obj_set_style_text_color(quiz_feedback, lv_color_hex(QC_DIM), 0);
    lv_obj_set_width(quiz_progress, lv_pct(100));
    /* The answers share everything below the stage: bigger targets with
     * fewer choices, up to a comfortable maximum. */
    int row_h = (QUIZ_CHOICE_ROOM - grow - 8 * (quiz.choice_count - 1)) / (quiz.choice_count ? quiz.choice_count : 1);
    if (row_h > 96) row_h = 96;
    /* A long answer takes a second line in smaller type before it ends in dots. */
    const lv_font_t *answer_fonts[] = { &ss_semibold_26, &ss_semibold_21, &ss_regular_18, &ss_regular_16 };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_set_height(quiz_choices[i], row_h);
        if (quiz_choice_cover[i]) { lv_obj_delete(quiz_choice_cover[i]); quiz_choice_cover[i] = NULL; }
        lv_obj_t *letter = lv_obj_get_child(quiz_choices[i], 1), *answer = lv_obj_get_child(quiz_choices[i], 0);
        quiz_fit_label(answer, i < quiz.choice_count ? quiz.choices[i] : "", lv_obj_get_style_width(answer, 0),
                       row_h - 6, answer_fonts, 4);
        quiz_choice_look(i, 0);
        lv_obj_set_style_opa(quiz_choices[i], LV_OPA_COVER, 0);
        if (i < quiz.choice_count && quiz.choices[i][0])
        {
            lv_obj_remove_flag(quiz_choices[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(quiz_choices[i], LV_OBJ_FLAG_CLICKABLE);
            quiz_slide_in(quiz_choices[i], 40 + i * 35);
        }
        else
        {
            lv_obj_add_flag(quiz_choices[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(quiz_choices[i], LV_OBJ_FLAG_CLICKABLE);
        }
        /* Album questions show each album's cover in place of its letter. */
        bool art = quiz.kind == QUIZ_ALBUM && quiz.choice_album[i] >= 0 && quiz.choice_album[i] < sl_lib.n_albums;
        if (art)
        {
            quiz_choice_cover[i] = quiz_sleeve(quiz_choices[i], quiz.choice_album[i], SL_ART_ROW);
            lv_obj_align(quiz_choice_cover[i], LV_ALIGN_LEFT_MID, 8, 0);
        }
        if (art) lv_obj_add_flag(letter, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(letter, LV_OBJ_FLAG_HIDDEN);
    }
    /* The stage stays put and its record and question wipe in: moving the
     * whole glowing panel under the answers' wipe dropped frames. */
    quiz_slide_in(quiz_glyph, 0);
    quiz_slide_in(quiz_question, 0);
    if (quiz.kind == QUIZ_SONG)
    {
        int n = 0;
        for (int i = 0; i < quiz.choice_count; i++) if (i != quiz.answer) quiz.elimination_order[n++] = i;
        for (int i = n - 1; i > 0; i--)
        {
            int j = quiz_rand() % (i + 1), t = quiz.elimination_order[i];
            quiz.elimination_order[i] = quiz.elimination_order[j];
            quiz.elimination_order[j] = t;
        }
    }
    lv_obj_add_flag(quiz_next_button, LV_OBJ_FLAG_HIDDEN);
    quiz_update_score();
    quiz_tick_second = 0;
    /* A song clip is its own cue; a chime over it would only get in the way. */
    if (!quiz.clip.active) sfx_play(SFX_QUIZ_QUESTION);
}

/* Settles the question: picked is a row, or -1 when time ran out. */
static void quiz_settle(int picked)
{
    quiz.answered = true;
    quiz.disc_stop = lv_tick_get();
    uint32_t now = sl_ms();
    uint32_t remaining = !quiz.question_deadline ? 10000 :        /* before the clock started */
                         quiz.question_deadline > now ? quiz.question_deadline - now : 0;
    bool right = picked == quiz.answer;
    if (quiz.qnum >= 1 && quiz.qnum <= QUIZ_PER_ROUND) quiz.pip[quiz.qnum - 1] = right ? 1 : 2;
    char msg[64], money[20];
    if (right)
    {
        int gain = quiz_payout(remaining);
        quiz.score += gain;
        quiz.round_cash += gain;
        quiz.streak++;
        if (quiz.streak > quiz.best_streak) quiz.best_streak = quiz.streak;
        quiz.correct++;
        quiz.react_total += 10000 - (remaining > 10000 ? 10000 : remaining);
        sfx_play(quiz.streak >= 3 ? SFX_QUIZ_STREAK : SFX_QUIZ_RIGHT);
        quiz_money(money, sizeof money, gain);
        if (quiz.streak >= 3) snprintf(msg, sizeof msg, "%d in a row! +%s", quiz.streak, money);
        else snprintf(msg, sizeof msg, "Correct! +%s", money);
        char plus[24];
        snprintf(plus, sizeof plus, "+%s", money);
        quiz_float(plus, QC_LIME, quiz_choices[picked]);
        quiz_burst(quiz_choices[picked], 0);
        quiz.auto_at = now + 1500;
        if (!quiz.auto_at) quiz.auto_at = 1;
    }
    else
    {
        quiz.streak = 0;
        quiz.strikes++;
        sfx_play(picked < 0 ? SFX_QUIZ_TIMEUP : SFX_QUIZ_WRONG);
        if (quiz.strikes >= QUIZ_STRIKES) strlcpy(msg, "Strike three. You're out!", sizeof msg);
        else snprintf(msg, sizeof msg, "%s Strike %d. Answer in green.", picked < 0 ? "Time!" : "Nope!", quiz.strikes);
    }
    for (int i = 0; i < quiz.choice_count; i++)
    {
        if (lv_obj_get_style_opa(quiz_choices[i], 0) == LV_OPA_TRANSP) continue;
        quiz_choice_look(i, i == quiz.answer ? 1 : i == picked ? 2 : 3);
    }
    lv_label_set_text(quiz_feedback, msg);
    lv_obj_set_style_text_color(quiz_feedback, lv_color_hex(right ? QC_LIME : QC_RED), 0);
    lv_label_set_text(lv_obj_get_child(quiz_next_button, 0),
                      quiz.strikes >= QUIZ_STRIKES ? "RESULTS " LV_SYMBOL_RIGHT :
                      quiz.qnum >= QUIZ_PER_ROUND ? "BONUS " LV_SYMBOL_RIGHT : "NEXT " LV_SYMBOL_RIGHT);
    lv_obj_remove_flag(quiz_next_button, LV_OBJ_FLAG_HIDDEN);
    quiz_update_score();
    quiz_hush_clip();                  /* after the answer's sound has started */
}
static void quiz_answer_cb(lv_event_t *e)
{
    if (quiz.answered || quiz.phase != QP_PLAY) return;
    int picked = (int)(intptr_t)lv_event_get_user_data(e);
    if (picked < 0 || picked >= quiz.choice_count ||
        !lv_obj_has_flag(quiz_choices[picked], LV_OBJ_FLAG_CLICKABLE)) return;
    quiz_settle(picked);
}
static void quiz_next_cb(lv_event_t *e) { (void)e; quiz_next(); }

/* ---- overlays: the wheel, the bonus round and the results */
static lv_obj_t *quiz_overlay_open(uint32_t top, uint32_t bottom)
{
    lv_obj_clean(quiz_overlay);
    quiz_wheel = quiz_wheel_title = quiz_wheel_desc = quiz_wheel_btn = quiz_wager_grid = quiz_wheel_face = NULL;
    quiz_bonus_bar = quiz_bonus_count = quiz_bonus_card = quiz_bonus_prompt = quiz_bonus_big = NULL;
    quiz_bonus_btn[0] = quiz_bonus_btn[1] = NULL;
    lv_obj_set_style_bg_color(quiz_overlay, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(quiz_overlay, lv_color_hex(bottom), 0);
    lv_obj_remove_flag(quiz_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quiz_overlay);
    if (quiz_fx) lv_obj_move_foreground(quiz_fx);
    lv_obj_scroll_to_y(quiz_overlay, 0, LV_ANIM_OFF);
    lv_obj_fade_in(quiz_overlay, 220, 0);
    return quiz_overlay;
}
static lv_obj_t *quiz_heading(lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t color, bool glow)
{
    lv_obj_t *l = text(parent, s, f, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (glow) quiz_glow(l, color);
    return l;
}

static int quiz_wheel_seg_at(int rot)
{
    int local = ((270 - rot) % 360 + 360) % 360;
    return local / 45;
}
static void quiz_wheel_name(int seg, char *b, size_t n)
{
    int mult = quiz_wheel_segs[seg].mult;
    snprintf(b, n, "%s \xc3\x97%d", mult == 5 ? "Jackpot" : quiz_wheel_segs[seg].type == QB_LIGHTNING ?
             "Lightning" : "This or That", mult);
}
static void quiz_wheel_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = lv_area_get_width(&a) / 2 - 12;
    uint32_t now = lv_tick_get();
    int rot = quiz.wheel_rot;
    bool landed = quiz.wheel_seg >= 0;
    quiz_circle(layer, cx, cy + 5, r + 10, 0x000000, 90);
    quiz_circle(layer, cx, cy, r + 8, 0x14224a, LV_OPA_COVER);
    quiz_ring(layer, cx, cy, r + 9, 2, QC_CYAN, 150);
    for (int i = 0; i < 8; i++)
    {
        int a0 = rot + i * 45;
        quiz_arc(layer, cx, cy, r, r, a0, a0 + 45, quiz_wheel_segs[i].color, LV_OPA_COVER);
        /* once it stops, the other slices step back */
        if (landed && quiz.wheel_seg != i) quiz_arc(layer, cx, cy, r, r, a0, a0 + 45, QC_BASE, 150);
    }
    if (landed)
    {
        int a0 = rot + quiz.wheel_seg * 45, pulse = (int)(now / 6 % 100);
        quiz_arc(layer, cx, cy, r + 8, 8, a0, a0 + 45, quiz_wheel_segs[quiz.wheel_seg].color,
                 (lv_opa_t)(140 + (pulse < 50 ? pulse : 100 - pulse) * 2));
    }
    for (int i = 0; i < 8; i++)
    {
        float m = (rot + i * 45 + 22.5f) * 0.0174533f;
        float c = cosf(m), sn = sinf(m);
        lv_opa_t opa = landed && quiz.wheel_seg != i ? 110 : LV_OPA_COVER;
        int lx = cx + (int)(c * r * 0.68f), ly = cy + (int)(sn * r * 0.68f);
        char b[8];
        snprintf(b, sizeof b, "\xc3\x97%d", quiz_wheel_segs[i].mult);
        quiz_draw_text(layer, b, &ss_bold_28, 0xffffff, opa, lx - 32, ly - 16, lx + 32, ly + 18);
        int sx = cx + (int)(c * r * 0.42f), sy = cy + (int)(sn * r * 0.42f);
        quiz_draw_text(layer, quiz_wheel_segs[i].type == QB_LIGHTNING ? LV_SYMBOL_CHARGE : LV_SYMBOL_SHUFFLE,
                       &sl_sym_16, 0xffffff, opa > 200 ? 220 : 120, sx - 12, sy - 9, sx + 12, sy + 11);
        if (quiz_wheel_segs[i].mult == 5)   /* two little sparkles on the jackpot */
            for (int k = -1; k <= 1; k += 2)
            {
                int px = cx + (int)(cosf(m + k * 0.2f) * r * 0.88f), py = cy + (int)(sinf(m + k * 0.2f) * r * 0.88f);
                quiz_fill(layer, px - 5, py - 1, px + 5, py + 1, 0xffffff, opa, 1);
                quiz_fill(layer, px - 1, py - 5, px + 1, py + 5, 0xffffff, opa, 1);
            }
    }
    /* the white hub with its pointer, as on the phone app */
    int hr = r * 24 / 100;
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_opa = 50;
    d.bg_color = lv_color_hex(0x000000);
    d.p[0].x = cx - 13; d.p[0].y = cy - hr + 8;
    d.p[1].x = cx + 13; d.p[1].y = cy - hr + 8;
    d.p[2].x = cx;      d.p[2].y = cy - hr - 20;
    lv_draw_triangle(layer, &d);
    quiz_circle(layer, cx, cy + 3, hr + 2, 0x000000, 45);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = lv_color_hex(QC_CYAN);
    d.p[0].y = d.p[1].y = cy - hr + 6;
    d.p[2].y = cy - hr - 22;
    lv_draw_triangle(layer, &d);
    quiz_circle(layer, cx, cy, hr + 3, QC_CYAN, LV_OPA_COVER);
    quiz_circle(layer, cx, cy, hr, QC_PANEL, LV_OPA_COVER);
    quiz_ring(layer, cx, cy, hr * 62 / 100, 5, QC_CYAN, 110);
}
/* A little face that ponders, watches the wheel go round, then beams. */
static void quiz_face_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2 + 2, r = 21;
    quiz_circle(layer, cx, cy, r, 0xffc93c, LV_OPA_COVER);
    quiz_arc(layer, cx, cy, r, 6, 20, 160, 0xf2a531, 150);
    quiz_arc(layer, cx - 4, cy - 4, r - 8, 3, 210, 260, 0xffffff, 120);
    uint32_t ink = 0x5a3b16;
    if (quiz.wheel_seg >= 0)
    {   /* happy: closed eyes, a grin and rosy cheeks */
        quiz_arc(layer, cx - 8, cy - 1, 5, 3, 200, 340, ink, LV_OPA_COVER);
        quiz_arc(layer, cx + 8, cy - 1, 5, 3, 200, 340, ink, LV_OPA_COVER);
        quiz_arc(layer, cx, cy + 3, 9, 3, 20, 160, ink, LV_OPA_COVER);
        quiz_circle(layer, cx - 14, cy + 6, 4, 0xff7a8a, 140);
        quiz_circle(layer, cx + 14, cy + 6, 4, 0xff7a8a, 140);
        return;
    }
    int dx = 3, dy = -3;
    if (quiz.wheel_spinning)
    {   /* the eyes follow the wheel round */
        float t = quiz.wheel_rot * 0.0174533f;
        dx = (int)(cosf(t) * 3);
        dy = (int)(sinf(t) * 2);
    }
    quiz_fill(layer, cx - 10 + dx, cy - 8 + dy, cx - 5 + dx, cy + dy, ink, LV_OPA_COVER, 3);
    quiz_fill(layer, cx + 5 + dx, cy - 8 + dy, cx + 10 + dx, cy + dy, ink, LV_OPA_COVER, 3);
    if (quiz.wheel_spinning) quiz_circle(layer, cx, cy + 10, 4, ink, LV_OPA_COVER);
    else
    {   /* hmm: a raised brow and a sideways mouth */
        quiz_line(layer, cx + 4, cy - 14, cx + 12, cy - 16, 2, ink, LV_OPA_COVER);
        quiz_line(layer, cx - 3, cy + 10, cx + 7, cy + 8, 3, ink, LV_OPA_COVER);
    }
}

static void quiz_bonus_start(void);
static void quiz_next_stage(void);
static void quiz_wager_cb(lv_event_t *e)
{
    if (quiz.phase != QP_WHEEL || quiz.wheel_seg < 0) return;
    int pct = (int)(intptr_t)lv_event_get_user_data(e);
    quiz.wager = quiz.score * pct / 100 / 10 * 10;
    if (pct && !quiz.wager) quiz.wager = quiz.score;
    sfx_play(SFX_TAP);
    if (quiz.wager > 0) quiz_bonus_start();
    else quiz_next_stage();
}
static void quiz_wheel_landed(void)
{
    quiz.wheel_spinning = false;
    quiz.wheel_seg = quiz_wheel_seg_at(quiz.wheel_rot);
    bool zap = quiz_wheel_segs[quiz.wheel_seg].type == QB_LIGHTNING;
    int mult = quiz_wheel_segs[quiz.wheel_seg].mult;
    char b[48];
    quiz_wheel_name(quiz.wheel_seg, b, sizeof b);
    lv_label_set_text(quiz_wheel_title, b);
    lv_obj_set_style_text_color(quiz_wheel_title, lv_color_hex(quiz_wheel_segs[quiz.wheel_seg].color), 0);
    snprintf(b, sizeof b, "%s Bet to win it \xc3\x97%d.", zap ? "True or false, fast!" : "Pick the right one!", mult);
    lv_label_set_text(quiz_wheel_desc, b);
    if (quiz_wheel_face) lv_obj_invalidate(quiz_wheel_face);
    lv_obj_remove_flag(quiz_wager_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(quiz_wager_grid, 250, 0);
    static const int pct[] = { 0, 25, 50, 100 };
    for (int i = 0; i < 4; i++)
    {
        char money[20];
        int amount = quiz.score * pct[i] / 100 / 10 * 10;
        if (pct[i] && !amount) amount = quiz.score;
        quiz_money(money, sizeof money, pct[i] ? amount : quiz.score);
        snprintf(b, sizeof b, "%s", pct[i] ? money : "keep it safe");
        lv_label_set_text(lv_obj_get_child(lv_obj_get_child(quiz_wager_grid, i), 1), b);
    }
    sfx_play(mult == 5 ? SFX_QUIZ_STREAK : SFX_QUIZ_CASH);
}
static void quiz_wheel_spin_cb(lv_event_t *e)
{
    (void)e;
    if (quiz.phase != QP_WHEEL || quiz.wheel_spinning || quiz.wheel_seg >= 0) return;
    static const uint8_t weight[] = { 0, 0, 4, 2, 0, 1 };   /* by multiplier */
    int total = 0;
    for (int i = 0; i < 8; i++) total += weight[quiz_wheel_segs[i].mult];
    int roll = quiz_rand() % total, seg = 0;
    for (int i = 0; i < 8; i++)
    {
        roll -= weight[quiz_wheel_segs[i].mult];
        if (roll < 0) { seg = i; break; }
    }
    int target = ((270 - (seg * 45 + 22) + (int)(quiz_rand() % 25) - 12) % 360 + 360) % 360;
    quiz.wheel_from = quiz.wheel_rot;
    quiz.wheel_to = quiz.wheel_rot - quiz.wheel_rot % 360 + 360 * 5 + target;
    quiz.wheel_start = lv_tick_get();
    quiz.wheel_spinning = true;
    quiz_wheel_last = quiz_wheel_seg_at(quiz.wheel_rot);
    lv_obj_add_flag(quiz_wheel_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(quiz_wheel_desc, "Round and round it goes...");
    lv_label_set_text(quiz_wheel_title, "???");
    sfx_play(SFX_TAP);
}
/* Only the breathing rim of the winning slice changes once the wheel has
 * landed: redraw its bounding box, not all 16 slices and labels. */
static void quiz_wheel_invalidate_rim(void)
{
    lv_area_t a;
    lv_obj_get_coords(quiz_wheel, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = lv_area_get_width(&a) / 2 - 12;
    int a0 = quiz.wheel_rot + quiz.wheel_seg * 45;
    lv_area_t box = { cx, cy, cx, cy };
    bool first = true;
    for (int k = 0; k <= 9; k++)            /* the outer edge every 5 degrees, and the inner corners */
    {
        float m = (a0 + k * 5) * 0.0174533f;
        for (int rr = r - 2; rr <= r + 10; rr += 12)
        {
            int x = cx + (int)(cosf(m) * rr), y = cy + (int)(sinf(m) * rr);
            if (first) { box.x1 = box.x2 = x; box.y1 = box.y2 = y; first = false; }
            if (x < box.x1) box.x1 = x;
            if (x > box.x2) box.x2 = x;
            if (y < box.y1) box.y1 = y;
            if (y > box.y2) box.y2 = y;
        }
    }
    box.x1 -= 4; box.y1 -= 4; box.x2 += 4; box.y2 += 4;
    lv_obj_invalidate_area(quiz_wheel, &box);
}
static void quiz_wheel_step(uint32_t now)
{
    static uint32_t blink;
    if (!quiz_wheel) return;
    if (!quiz.wheel_spinning)
    {   /* the winning slice's edge breathes */
        if (quiz.wheel_seg >= 0 && now / 60 != blink) { blink = now / 60; quiz_wheel_invalidate_rim(); }
        return;
    }
    const uint32_t spin = 3400;
    uint32_t t = now - quiz.wheel_start;
    if (t >= spin) quiz.wheel_rot = quiz.wheel_to;
    else
    {
        float p = 1.0f - t / (float)spin;
        quiz.wheel_rot = quiz.wheel_from + (int)((quiz.wheel_to - quiz.wheel_from) * (1.0f - p * p * p));
    }
    int seg = quiz_wheel_seg_at(quiz.wheel_rot);
    if (seg != quiz_wheel_last)
    {   /* the slice under the pointer shows above the wheel as it passes */
        char b[32];
        quiz_wheel_last = seg;
        quiz_wheel_name(seg, b, sizeof b);
        lv_label_set_text(quiz_wheel_title, b);
        lv_obj_set_style_text_color(quiz_wheel_title, lv_color_hex(quiz_wheel_segs[seg].color), 0);
        sfx_play_vary(SFX_QUIZ_TICK);
    }
    lv_obj_invalidate(quiz_wheel);
    if (quiz_wheel_face) lv_obj_invalidate(quiz_wheel_face);
    if (t >= spin) quiz_wheel_landed();
}
static void quiz_round_clear(void)
{
    quiz.phase = QP_WHEEL;
    quiz.answered = true;
    quiz.question_deadline = 0;
    quiz.wheel_seg = -1;
    quiz.wheel_spinning = false;
    lv_obj_t *o = quiz_overlay_open(0x0c1224, 0x020308);
    char b[64], money[20];
    lv_obj_t *card = box(o);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    quiz_style_panel(card, QC_LINE, 24);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    quiz_money(money, sizeof money, quiz.round_cash);
    snprintf(b, sizeof b, "ROUND %d DONE  \xc2\xb7  +%s", quiz.stage, money);
    lv_obj_t *head = quiz_heading(card, b, &ss_semibold_18, QC_DIM, false);
    lv_obj_set_style_text_letter_space(head, 1, 0);
    quiz_wheel_face = box(card);
    lv_obj_set_size(quiz_wheel_face, 60, 50);
    lv_obj_add_event_cb(quiz_wheel_face, quiz_face_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *pill = box(card);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, 18, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(QC_CYAN), 0);
    lv_obj_set_style_bg_opa(pill, 40, 0);
    lv_obj_set_style_pad_hor(pill, 16, 0);
    lv_obj_set_style_pad_ver(pill, 6, 0);
    quiz_wheel_desc = text(pill, "What's my bonus round?", &ss_semibold_18, QC_CYAN);
    quiz_wheel_title = quiz_heading(card, "???", &ss_bold_28, QC_WHITE, false);
    quiz_wheel = box(card);
    lv_obj_set_size(quiz_wheel, 300, 300);
    lv_obj_add_event_cb(quiz_wheel, quiz_wheel_draw, LV_EVENT_DRAW_MAIN, NULL);
    on_click(quiz_wheel, quiz_wheel_spin_cb, NULL);
    quiz_wheel_btn = quiz_button(o, "SPIN THE WHEEL", 300, 58, QC_CYAN, 0x0284c7, QC_BASE, quiz_wheel_spin_cb, NULL);
    quiz_wager_grid = box(o);
    lv_obj_set_size(quiz_wager_grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(quiz_wager_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(quiz_wager_grid, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);   /* the first at the reading edge */
    lv_obj_set_style_pad_column(quiz_wager_grid, 10, 0);
    lv_obj_set_style_pad_row(quiz_wager_grid, 10, 0);
    lv_obj_add_flag(quiz_wager_grid, LV_OBJ_FLAG_HIDDEN);
    static const char *names[] = { "Pass", "Bet 25%", "Bet half", "All in!" };
    static const int pct[] = { 0, 25, 50, 100 };
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *w = box(quiz_wager_grid);
        bool all = i == 3;
        lv_obj_set_size(w, (W - 28 - 10) / 2, 64);
        quiz_style_panel(w, all ? QC_MAGENTA : i ? QC_CYAN : QC_LINE, 18);
        if (all)
        {
            lv_obj_set_style_bg_color(w, lv_color_hex(0xc026d3), 0);
            lv_obj_set_style_bg_grad_color(w, lv_color_hex(0x86198f), 0);
            lv_obj_set_style_shadow_color(w, lv_color_hex(QC_MAGENTA), 0);
            lv_obj_set_style_shadow_width(w, 18, 0);
            lv_obj_set_style_shadow_opa(w, 90, 0);
        }
        lv_obj_set_style_bg_color(w, lv_color_hex(all ? 0x86198f : QC_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(w, 2, LV_STATE_PRESSED);
        lv_obj_t *l = text(w, names[i], &ss_semibold_21, QC_WHITE);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *v = text(w, "", &ss_regular_16, all ? 0xf5d0fe : QC_DIM);
        lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -8);
        on_click(w, quiz_wager_cb, (void *)(intptr_t)pct[i]);
    }
    sfx_play(SFX_QUIZ_CASH);
}

/* ---- the bonus round: six quick calls. Lightning shows a statement to
 * call true or false; This or That offers two answers. */
static bool quiz_bonus_music(bool zap)
{
    int n = sl_lib.n_tracks;
    if (!n) return false;
    for (int tries = 0; tries < 60; tries++)
    {
        int t = quiz_rand() % n;
        if (!quiz_track_ok(t)) continue;
        bool used = false;
        for (int i = 0; i < quiz.bonus_n && i < 8; i++) if (quiz.bonus_used[i] == t) used = true;
        if (used) continue;
        const struct sl_track *track = &sl_lib.tracks[t];
        const struct sl_album *album = &sl_lib.albums[track->album];
        const char *other_artist = NULL, *other_album = NULL;
        int other_year = 0;
        int start = quiz_rand() % n;
        for (int k = 0; k < n; k++)
        {
            int c = (start + k) % n;
            if (!quiz_track_ok(c)) continue;
            const struct sl_album *oa = &sl_lib.albums[sl_lib.tracks[c].album];
            if (!other_artist && strcasecmp(oa->artist, album->artist)) other_artist = oa->artist;
            if (!other_album && strcasecmp(oa->title, album->title) && oa->year >= 1900 && oa->year <= 2100 &&
                oa->year != album->year) { other_album = oa->title; other_year = oa->year; }
            if (other_artist && other_album) break;   /* the rest of the library can't add anything */
        }
        if (!other_artist) return false;
        bool dated = album->year >= 1900 && album->year <= 2100;
        int variant = quiz_rand() % 3;
        if (variant == 1 && (!dated || (!zap && !other_album))) variant = 0;
        bool truth = quiz_rand() & 1;
        quiz.bonus_big[0] = 0;
        if (variant == 0)
        {
            if (zap)
            {
                snprintf(quiz.bonus_prompt, sizeof quiz.bonus_prompt, "\"%s\" is by", track->title);
                strlcpy(quiz.bonus_big, truth ? album->artist : other_artist, sizeof quiz.bonus_big);
            }
            else
            {
                strlcpy(quiz.bonus_prompt, "Who made this one?", sizeof quiz.bonus_prompt);
                strlcpy(quiz.bonus_big, track->title, sizeof quiz.bonus_big);
                strlcpy(quiz.bonus_opt[truth], album->artist, sizeof quiz.bonus_opt[0]);
                strlcpy(quiz.bonus_opt[!truth], other_artist, sizeof quiz.bonus_opt[0]);
            }
        }
        else if (variant == 1)
        {
            if (zap)
            {
                static const int miss[] = { -3, -2, -1, 1, 2, 3 };
                snprintf(quiz.bonus_prompt, sizeof quiz.bonus_prompt, "The album %s came out in", album->title);
                snprintf(quiz.bonus_big, sizeof quiz.bonus_big, "%d", truth ? album->year : album->year + miss[quiz_rand() % 6]);
            }
            else
            {
                strlcpy(quiz.bonus_prompt, "Which album came out first?", sizeof quiz.bonus_prompt);
                strlcpy(quiz.bonus_opt[truth], album->year < other_year ? album->title : other_album, sizeof quiz.bonus_opt[0]);
                strlcpy(quiz.bonus_opt[!truth], album->year < other_year ? other_album : album->title, sizeof quiz.bonus_opt[0]);
            }
        }
        else
        {
            const char *wrong_album = NULL;
            for (int k = 0; k < n && !wrong_album; k++)
            {
                int c = (start + k) % n;
                const char *title = quiz_track_ok(c) ? sl_lib.albums[sl_lib.tracks[c].album].title : NULL;
                if (title && strcasecmp(title, album->title)) wrong_album = title;
            }
            if (!wrong_album) continue;
            snprintf(quiz.bonus_prompt, sizeof quiz.bonus_prompt, "\"%s\" is on the album", track->title);
            if (zap) strlcpy(quiz.bonus_big, truth ? album->title : wrong_album, sizeof quiz.bonus_big);
            else
            {
                strlcpy(quiz.bonus_opt[truth], album->title, sizeof quiz.bonus_opt[0]);
                strlcpy(quiz.bonus_opt[!truth], wrong_album, sizeof quiz.bonus_opt[0]);
            }
        }
        if (zap)
        {
            strlcpy(quiz.bonus_opt[0], "TRUE", sizeof quiz.bonus_opt[0]);
            strlcpy(quiz.bonus_opt[1], "FALSE", sizeof quiz.bonus_opt[1]);
            quiz.bonus_answer = truth ? 0 : 1;
        }
        else quiz.bonus_answer = truth;
        if (quiz.bonus_n < 8) quiz.bonus_used[quiz.bonus_n] = t;
        return true;
    }
    return false;
}
static bool quiz_bonus_item(bool zap)
{
    bool music = quiz.mode == QUIZ_MODE_MUSIC || (quiz.mode == QUIZ_MODE_MIXED && (quiz_rand() & 1));
    if (music && quiz_bonus_music(zap)) return true;
    int kind = quiz.mode == QUIZ_MODE_TV ? QUIZ_TV : quiz.mode == QUIZ_MODE_MOVIES ? QUIZ_MOVIE :
               (quiz_rand() & 1) ? QUIZ_MOVIE : QUIZ_TV;
    int index = quiz_take_trivia(kind);
    if (index < 0) index = quiz_take_trivia(kind == QUIZ_MOVIE ? QUIZ_TV : QUIZ_MOVIE);
    if (index < 0) return !music && quiz_bonus_music(zap);
    const struct quiz_trivia_item *item = &quiz_trivia[index];
    int wrong = (item->answer + 1 + quiz_rand() % 3) % 4;
    bool truth = quiz_rand() & 1;
    strlcpy(quiz.bonus_prompt, item->question, sizeof quiz.bonus_prompt);
    if (zap)
    {
        strlcpy(quiz.bonus_big, item->choices[truth ? item->answer : wrong], sizeof quiz.bonus_big);
        strlcpy(quiz.bonus_opt[0], "TRUE", sizeof quiz.bonus_opt[0]);
        strlcpy(quiz.bonus_opt[1], "FALSE", sizeof quiz.bonus_opt[1]);
        quiz.bonus_answer = truth ? 0 : 1;
    }
    else
    {
        quiz.bonus_big[0] = 0;
        strlcpy(quiz.bonus_opt[truth], item->choices[item->answer], sizeof quiz.bonus_opt[0]);
        strlcpy(quiz.bonus_opt[!truth], item->choices[wrong], sizeof quiz.bonus_opt[0]);
        quiz.bonus_answer = truth;
    }
    return true;
}
static bool quiz_bonus_zap(void) { return quiz.wheel_seg >= 0 && quiz_wheel_segs[quiz.wheel_seg].type == QB_LIGHTNING; }
static void quiz_bonus_btn_look(int i, int look)   /* 0 idle, 1 right, 2 wrong */
{
    bool zap = quiz_bonus_zap();
    uint32_t top = look == 1 ? 0x22c55e : look == 2 ? 0xe11d48 : zap ? (i ? 0x3b0a45 : 0x22c55e) : 0x0c3a4d;
    uint32_t bottom = look == 1 ? 0x15803d : look == 2 ? 0x9f1239 : zap ? (i ? 0x2a0631 : 0x15803d) : 0x072433;
    uint32_t edge = look == 1 ? 0xbef264 : look == 2 ? 0xfda4af : zap ? (i ? QC_MAGENTA : 0x86efac) : QC_CYAN;
    lv_obj_t *b = quiz_bonus_btn[i];
    lv_obj_set_style_bg_color(b, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(bottom), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(edge), 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(edge), 0);
    lv_obj_set_style_shadow_opa(b, look ? 160 : 60, 0);
}
static void quiz_bonus_finish(int result);
#define QUIZ_BONUS_CARD_H   244
#define QUIZ_BONUS_CARD_MAX 330   /* still clear of the buttons at the largest Text size */
static void quiz_bonus_item_next(void)
{
    bool zap = quiz_bonus_zap();
    if (quiz.bonus_n >= QUIZ_BONUS) { quiz_bonus_finish(1); return; }
    if (!quiz_bonus_item(zap)) { quiz_bonus_finish(quiz.bonus_n ? 1 : -1); return; }
    quiz.bonus_n++;
    char b[16];
    snprintf(b, sizeof b, "%d / %d", quiz.bonus_n, QUIZ_BONUS);
    lv_label_set_text(quiz_bonus_count, b);
    /* The prompt and the claim under it are shown whole: smaller type,
     * then a taller card. */
    const lv_font_t *bigs[] = { &ss_bold_28, &ss_semibold_26, &ss_semibold_21, &ss_regular_18 };
    const lv_font_t *prompts[] = { &ss_semibold_21, &ss_regular_18, &ss_regular_16, sl_fonts[0][0] };
    const lv_font_t *opts[] = { zap ? &ss_bold_28 : &ss_semibold_21, &ss_regular_18, &ss_regular_16, sl_fonts[0][0] };
    int width = W - 28 - 2 * 18 - 2 * 2, big_h = 0, prompt_h;
    if (quiz.bonus_big[0])
    {
        lv_label_set_text(quiz_bonus_big, quiz.bonus_big);
        lv_obj_set_style_text_font(quiz_bonus_big, quiz_fit(lv_label_get_text(quiz_bonus_big), width,
                                   2 * lv_font_get_line_height(&ss_bold_28), bigs, 4, &big_h), 0);
        big_h += 12;
        lv_obj_remove_flag(quiz_bonus_big, LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(quiz_bonus_big, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(quiz_bonus_prompt, quiz.bonus_prompt);
    lv_obj_set_style_text_font(quiz_bonus_prompt, quiz_fit(lv_label_get_text(quiz_bonus_prompt), width,
                               QUIZ_BONUS_CARD_MAX - 40 - big_h, prompts, 4, &prompt_h), 0);
    lv_obj_set_height(quiz_bonus_card, LV_MAX(QUIZ_BONUS_CARD_H, 40 + prompt_h + big_h));
    for (int i = 0; i < 2; i++)
    {
        quiz_fit_label(lv_obj_get_child(quiz_bonus_btn[i], 0), quiz.bonus_opt[i],
                       (W - 28 - 14) / 2 - 2 * 12 - 2 * 2, 84, opts, 4);
        quiz_bonus_btn_look(i, 0);
    }
    quiz.bonus_deadline = lv_tick_get() + (zap ? 3000 : 4000);
    quiz.bonus_wait = 0;
    lv_obj_set_width(quiz_bonus_bar, lv_pct(100));
    quiz_slide_in(quiz_bonus_card, 0);
}
static void quiz_bonus_pick(int pick)   /* -1 when too slow */
{
    if (quiz.phase != QP_BONUS || quiz.bonus_wait) return;
    quiz.bonus_won = pick == quiz.bonus_answer;
    if (pick >= 0) quiz_bonus_btn_look(pick, quiz.bonus_won ? 1 : 2);
    if (!quiz.bonus_won) quiz_bonus_btn_look(quiz.bonus_answer, 1);
    if (quiz.bonus_won) sfx_play_ex(SFX_QUIZ_RIGHT, 100 + quiz.bonus_n * 8, 90);
    else sfx_play(pick < 0 ? SFX_QUIZ_TIMEUP : SFX_QUIZ_WRONG);
    lv_label_set_text(quiz_bonus_count, quiz.bonus_won ? "YES!" : pick < 0 ? "TOO SLOW!" : "NOPE!");
    quiz.bonus_wait = lv_tick_get() + (quiz.bonus_won ? 320 : 1100);
    if (!quiz.bonus_wait) quiz.bonus_wait = 1;
}
static void quiz_bonus_cb(lv_event_t *e) { quiz_bonus_pick((int)(intptr_t)lv_event_get_user_data(e)); }
static void quiz_bonus_step(uint32_t now)
{
    if (quiz.bonus_wait)
    {
        if ((int32_t)(now - quiz.bonus_wait) < 0) return;
        quiz.bonus_wait = 0;
        if (quiz.bonus_won) quiz_bonus_item_next();
        else quiz_bonus_finish(0);
        return;
    }
    uint32_t total = quiz_bonus_zap() ? 3000 : 4000;
    int32_t left = (int32_t)(quiz.bonus_deadline - now);
    if (left <= 0) { quiz_bonus_pick(-1); return; }
    int32_t bar_w = lv_pct((int)((uint32_t)left * 100 / total));
    if (lv_obj_get_style_width(quiz_bonus_bar, 0) != bar_w) lv_obj_set_width(quiz_bonus_bar, bar_w);
    /* the neon edge pulses in four steps, which keeps redraws cheap */
    static uint32_t pulse;
    if (now / 120 != pulse)
    {
        pulse = now / 120;
        lv_obj_set_style_border_opa(quiz_bonus_card, (lv_opa_t)(90 + 40 * (pulse % 4)), 0);
    }
}
static void quiz_bonus_start(void)
{
    bool zap = quiz_bonus_zap();
    int mult = quiz_wheel_segs[quiz.wheel_seg].mult;
    uint32_t accent = zap ? QC_MAGENTA : QC_CYAN;
    quiz.phase = QP_BONUS;
    quiz.bonus_n = 0;
    for (int i = 0; i < 8; i++) quiz.bonus_used[i] = -1;
    lv_obj_t *o = quiz_overlay_open(zap ? 0x4a0d57 : 0x063a4a, zap ? 0x12021c : 0x020f1a);
    quiz_heading(o, zap ? "LIGHTNING ROUND" : "THIS OR THAT", &sl_sym_32, zap ? 0xf0abfc : 0x67e8f9, true);
    char b[64], money[20];
    quiz_money(money, sizeof money, quiz.wager);
    snprintf(b, sizeof b, "WAGER %s  ·  WIN x%d", money, mult);
    quiz_heading(o, b, &ss_semibold_18, QC_WHITE, false);
    lv_obj_t *row = box(o);
    lv_obj_set_size(row, lv_pct(100), 26);
    quiz_bonus_count = text(row, "", &ss_semibold_21, accent);
    lv_obj_align(quiz_bonus_count, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *track = box(row);
    lv_obj_set_size(track, 330, 10);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(track, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(track, 110, 0);
    quiz_bonus_bar = box(track);
    lv_obj_set_size(quiz_bonus_bar, lv_pct(100), 10);
    lv_obj_set_style_radius(quiz_bonus_bar, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(quiz_bonus_bar, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(quiz_bonus_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(quiz_bonus_bar, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_width(quiz_bonus_bar, 12, 0);
    lv_obj_set_style_shadow_opa(quiz_bonus_bar, 160, 0);
    quiz_bonus_card = box(o);
    lv_obj_set_size(quiz_bonus_card, lv_pct(100), QUIZ_BONUS_CARD_H);
    lv_obj_set_style_radius(quiz_bonus_card, 24, 0);
    lv_obj_set_style_bg_color(quiz_bonus_card, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(quiz_bonus_card, 14, 0);
    lv_obj_set_style_border_width(quiz_bonus_card, 2, 0);
    lv_obj_set_style_border_color(quiz_bonus_card, lv_color_hex(accent), 0);
    lv_obj_set_style_pad_all(quiz_bonus_card, 18, 0);
    lv_obj_set_flex_flow(quiz_bonus_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(quiz_bonus_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(quiz_bonus_card, 12, 0);
    quiz_bonus_prompt = quiz_heading(quiz_bonus_card, "", &ss_semibold_21, zap ? 0xf5d0fe : 0xcffafe, false);
    quiz_bonus_big = quiz_heading(quiz_bonus_card, "", &ss_bold_28, QC_WHITE, true);
    lv_obj_t *buttons = box(o);
    lv_obj_set_size(buttons, lv_pct(100), 104);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *btn = box(buttons);
        quiz_bonus_btn[i] = btn;
        lv_obj_set_size(btn, (W - 28 - 14) / 2, 100);
        lv_obj_align(btn, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(btn, zap ? SL_R_FULL : 22, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_shadow_width(btn, 20, 0);
        lv_obj_set_style_pad_hor(btn, 12, 0);
        lv_obj_set_style_translate_y(btn, 3, LV_STATE_PRESSED);
        lv_obj_t *l = text(btn, "", zap ? &ss_bold_28 : &ss_semibold_21, QC_WHITE);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        on_click(btn, quiz_bonus_cb, (void *)(intptr_t)i);
    }
    quiz_heading(o, zap ? "One slip and the wager is gone." : "Pick the right one before the bar runs out.",
                 &ss_regular_16, QC_DIM, false);
    sfx_play(zap ? SFX_QUIZ_ZAP : SFX_QUIZ_QUESTION);
    quiz_bonus_item_next();
}
static void quiz_continue_cb(lv_event_t *e) { (void)e; quiz_next_stage(); }
static void quiz_bonus_finish(int result)   /* 1 won, 0 lost, -1 nothing to ask */
{
    int mult = quiz_wheel_segs[quiz.wheel_seg].mult;
    int delta = result > 0 ? quiz.wager * (mult - 1) : result == 0 ? -quiz.wager : 0;
    quiz.score += delta;
    if (quiz.score < 0) quiz.score = 0;
    quiz.phase = QP_BONUS_DONE;
    lv_obj_t *o = quiz_overlay_open(result > 0 ? 0x0d2a10 : result == 0 ? 0x2a0716 : QC_DEEP, 0x02040c);
    lv_obj_t *title = quiz_heading(o, result > 0 ? "PERFECT!" : result == 0 ? "BUSTED!" : "NO BONUS",
                                   &sl_sym_48, result > 0 ? QC_LIME : result == 0 ? QC_MAGENTA : QC_DIM, true);
    char money[20], b[64];
    quiz_money(money, sizeof money, delta < 0 ? -delta : delta);
    snprintf(b, sizeof b, "%s%s", delta > 0 ? "+" : delta < 0 ? "-" : "", money);
    quiz_heading(o, b, &sl_sym_48, result > 0 ? QC_WHITE : QC_RED, true);
    quiz_heading(o, result > 0 ? "Six for six. The wheel pays out." :
                    result == 0 ? "The wager is gone. Chin up." : "Out of bonus questions this time.",
                 &ss_regular_18, QC_DIM, false);
    quiz_money(money, sizeof money, quiz.score);
    snprintf(b, sizeof b, "BANKED  %s", money);
    quiz_heading(o, b, &ss_semibold_26, QC_CYAN, true);
    lv_obj_t *gap = box(o);
    lv_obj_set_size(gap, 10, 40);
    if (quiz.stage >= QUIZ_ROUNDS) strlcpy(b, "FINAL RESULTS " LV_SYMBOL_RIGHT, sizeof b);
    else snprintf(b, sizeof b, "ON TO ROUND %d " LV_SYMBOL_RIGHT, quiz.stage + 1);
    quiz_button(o, b, 320, 60, QC_CYAN, 0x0284c7, QC_BASE, quiz_continue_cb, NULL);
    sfx_play(result > 0 ? SFX_WIN : result == 0 ? SFX_LOSE : SFX_TAP);
    if (result > 0) quiz_burst(title, 1);
}
static void quiz_next_stage(void)
{
    if (quiz.stage >= QUIZ_ROUNDS) { quiz_game_over(1); return; }
    quiz.stage++;
    quiz.qnum = quiz.strikes = quiz.round_cash = 0;
    memset(quiz.pip, 0, sizeof quiz.pip);
    quiz.phase = QP_PLAY;
    quiz.wager = 0;
    lv_obj_add_flag(quiz_overlay, LV_OBJ_FLAG_HIDDEN);
    quiz_next();
    char b[16];
    snprintf(b, sizeof b, quiz.stage == QUIZ_ROUNDS ? "FINAL ROUND" : "ROUND %d", quiz.stage);
    if (quiz.phase == QP_PLAY) quiz_banner(b);
}

/* ---- results */
static void quiz_start(void);
static void quiz_again_cb(lv_event_t *e) { (void)e; quiz_start(); }
static void quiz_menu_cb(lv_event_t *e) { (void)e; page_pop(); }
static void quiz_stat_row(lv_obj_t *card, const char *name, const char *value, bool best)
{
    lv_obj_t *row = box(card);
    lv_obj_set_size(row, lv_pct(100), 34);
    lv_obj_t *label = text(row, name, &ss_regular_18, QC_DIM);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = text(row, value, &ss_semibold_21, QC_WHITE);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    if (best)
    {
        lv_obj_t *badge = box(row);
        lv_obj_set_size(badge, 96, 24);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -(lv_text_get_width(value, strlen(value), &ss_semibold_21, 0) + 12), 0);
        lv_obj_set_style_radius(badge, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(QC_LIME), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_center(text(badge, "NEW BEST", &ss_semibold_18, QC_BASE));
    }
}
static void quiz_game_over(int how)   /* 0 struck out, 1 cleared every round, 2 out of questions */
{
    quiz_restore_clip();
    quiz.phase = QP_OVER;
    quiz.answered = true;
    quiz.question_deadline = 0;
    unsigned beaten = quiz_record(how != 0);
    lv_obj_t *o = quiz_overlay_open(how == 1 ? 0x0b2a2e : QC_DEEP, 0x02040c);
    uint32_t c = how == 1 ? QC_LIME : how == 2 ? QC_CYAN : QC_MAGENTA;
    lv_obj_t *title = quiz_heading(o, how == 1 ? "CHAMPION!" : how == 2 ? "ALL ANSWERED!" : "GAME OVER",
                                   &sl_sym_48, c, true);
    quiz_heading(o, "FINAL WINNINGS", &ss_semibold_18, QC_DIM, false);
    char b[40], money[20];
    quiz_money(money, sizeof money, quiz.score);
    quiz_heading(o, money, &sl_sym_48, QC_CYAN, true);
    lv_obj_t *card = box(o);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    quiz_style_panel(card, QC_LINE, SL_R_LG);
    lv_obj_set_style_pad_hor(card, 18, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    int cleared = how == 1 ? QUIZ_ROUNDS : quiz.stage - 1;
    snprintf(b, sizeof b, "%d of %d", cleared < 0 ? 0 : cleared, QUIZ_ROUNDS);
    quiz_stat_row(card, "Rounds cleared", b, false);
    snprintf(b, sizeof b, "%d", quiz.correct);
    quiz_stat_row(card, "Right answers", b, false);
    snprintf(b, sizeof b, "%d", quiz.best_streak);
    quiz_stat_row(card, "Best streak", b, beaten & 2);
    int react = quiz_avg_react();
    if (react) snprintf(b, sizeof b, "%d.%ds", react / 1000, react % 1000 / 100);
    else strlcpy(b, "-", sizeof b);
    quiz_stat_row(card, "Average answer", b, beaten & 4);
    quiz_money(money, sizeof money, quiz_stats.best_cash);
    quiz_stat_row(card, "High score", money, beaten & 1);
    lv_obj_t *row = box(o);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_t *again = quiz_button(row, "PLAY AGAIN", 250, 60, QC_CYAN, 0x0284c7, QC_BASE, quiz_again_cb, NULL);
    lv_obj_align(again, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *menu = quiz_button(row, "MENU", 180, 60, 0, 0, QC_WHITE, quiz_menu_cb, NULL);
    lv_obj_align(menu, LV_ALIGN_RIGHT_MID, 0, 0);
    sfx_play(how == 0 && !beaten ? SFX_LOSE : SFX_QUIZ_APPLAUSE);
    if (how || beaten) quiz_burst(title, 1);
}

static void quiz_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = sl_ms(), tick = lv_tick_get();
    quiz_cash_roll();
    if (quiz_fx && !lv_obj_has_flag(quiz_fx, LV_OBJ_FLAG_HIDDEN))
    {
        if (tick - quiz_fx_at > 1150) lv_obj_add_flag(quiz_fx, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_invalidate(quiz_fx);
    }
    if (quiz.phase == QP_WHEEL) { quiz_wheel_step(tick); return; }
    if (quiz.phase == QP_BONUS) { quiz_bonus_step(tick); return; }
    if (quiz.phase != QP_PLAY) return;
    if (quiz.clip.active && !quiz.clip.audible && !quiz.clip.hushed)
    {
        if (quiz_clip_heard())
        {
            /* heard: now the clip's 3 s and the question's 10 s run */
            uint32_t lag = global_status.sloppy_volume_on_bt == 1 ? QUIZ_BT_LAG_MS : 0;
            quiz.clip.audible = true;
            quiz.clip.deadline = now + lag + 3000;
            if (!quiz.answered) quiz.question_deadline = now + lag + 10000;
            sl_log("quiz: clip heard %lu ms after it was asked for", (unsigned long)(now - quiz.clip.asked_at));
        }
        else if (now - quiz.clip.asked_at > 4500 && !quiz.answered)
        {
            /* It never started (a broken file, a slow card): ask something
             * else rather than run a clock on silence. */
            sl_log("quiz: clip never played, asking something else");
            quiz_restore_clip();
            quiz.no_clip = true;
            quiz.round--;
            quiz.qnum--;
            quiz_next();
            return;
        }
    }
    if (quiz.clip.active && quiz.clip.audible && !quiz.clip.hushed &&
        (int32_t)(now - quiz.clip.deadline) >= 0)
        quiz_hush_clip();
    if (quiz.clip.active && quiz.clip.hushed && (int32_t)(now - quiz.clip.restore_at) >= 0)
        quiz_restore_clip();
    if (!quiz.answered && quiz.question_deadline && (int32_t)(now - quiz.question_deadline) >= 0)
        quiz_settle(-1);
    if (quiz.answered && quiz.auto_at && (int32_t)(now - quiz.auto_at) >= 0)
    { quiz.auto_at = 0; quiz_next(); return; }
    if (quiz_glyph && quiz.kind == QUIZ_SONG && (!quiz.answered || quiz.clip.active)) lv_obj_invalidate(quiz_glyph);
    if (quiz_pips && !quiz.answered && quiz.qnum >= 1 && quiz.qnum <= QUIZ_PER_ROUND)
    {   /* only the current light pulses */
        lv_area_t a;
        lv_obj_get_coords(quiz_pips, &a);
        a.x1 += 9 + (quiz.qnum - 1) * 23 - 11;
        a.x2 = a.x1 + 22;
        lv_obj_invalidate_area(quiz_pips, &a);
    }
    if (!quiz.answered && quiz_timer_label && !quiz.question_deadline && quiz.clip.active)
        label_if_changed(quiz_timer_label, "...");      /* tuning in */
    if (!quiz.answered && quiz_timer_label && quiz.question_deadline)
    {
        uint32_t remaining = quiz.question_deadline - now;
        int seconds = (int)((remaining + 999) / 1000);
        if (quiz.kind == QUIZ_SONG)
        {
            int elapsed = 10000 - (int)remaining;
            int due = elapsed / 2000;
            while (quiz.eliminated_count < due && quiz.eliminated_count < 4)
            {
                int row = quiz.elimination_order[quiz.eliminated_count++];
                /* An invisible choice still occupies its flex row, so the
                 * remaining answers never move under the player's finger. */
                lv_obj_remove_flag(quiz_choices[row], LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_opa(quiz_choices[row], LV_OPA_TRANSP, 0);
                quiz_burst(quiz_choices[row], 2);
                sfx_play(SFX_QUIZ_POP);
            }
        }
        /* The last three seconds tick. */
        if (seconds <= 3 && seconds > 0 && seconds != quiz_tick_second)
        {
            quiz_tick_second = seconds;
            sfx_play_ex(SFX_QUIZ_TICK, seconds == 1 ? 150 : 100, 100);
        }
        int32_t bar_w = lv_pct((int)(remaining * 100 / 10000));
        if (lv_obj_get_style_width(quiz_progress, 0) != bar_w) lv_obj_set_width(quiz_progress, bar_w);
        static int shown_seconds;
        if (seconds != shown_seconds || quiz_tick_second == 0)
        {
            shown_seconds = seconds;
            char time_text[8];
            snprintf(time_text, sizeof time_text, "%ds", seconds);
            lv_label_set_text(quiz_timer_label, time_text);
            uint32_t bar = seconds <= 3 ? QC_RED : QC_CYAN;
            lv_obj_set_style_bg_color(quiz_progress, lv_color_hex(bar), 0);
            lv_obj_set_style_shadow_color(quiz_progress, lv_color_hex(bar), 0);
            lv_obj_set_style_text_color(quiz_timer_label, lv_color_hex(seconds <= 3 ? QC_RED : QC_WHITE), 0);
        }
    }
}
static void quiz_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != quiz_page_obj) return;
    quiz_restore_clip();
    quiz_record(false);   /* a game walked away from still counts */
    quiz_active = false;
    mini_update();
    if (quiz_timer) { lv_timer_delete(quiz_timer); quiz_timer = NULL; }
    quiz_page_obj = quiz_question = quiz_score = quiz_next_button = NULL;
    quiz_timer_label = quiz_round_label = quiz_pips = quiz_cover = quiz_fx = quiz_overlay = NULL;
    quiz_stage = quiz_mode_label = quiz_feedback = quiz_progress = quiz_glyph = NULL;
    quiz_wheel = quiz_wheel_title = quiz_wheel_desc = quiz_wheel_btn = quiz_wager_grid = quiz_wheel_face = NULL;
    quiz_bonus_bar = quiz_bonus_count = quiz_bonus_card = quiz_bonus_prompt = quiz_bonus_big = NULL;
    quiz_bonus_btn[0] = quiz_bonus_btn[1] = NULL;
    for (int i = 0; i < 5; i++) quiz_choice_cover[i] = NULL;
}
/* A fresh game in the current page: also PLAY AGAIN. */
static void quiz_start(void)
{
    int mode = quiz.mode;
    quiz_restore_clip();
    if (quiz.round && !quiz.recorded) quiz_record(false);
    memset(&quiz, 0, sizeof quiz);
    quiz.mode = mode;
    quiz.rng = (sl_ms() ^ lv_tick_get() * 2654435761u) | 1;
    quiz.clip_index = -1;
    quiz.stage = 1;
    quiz.phase = QP_PLAY;
    if (quiz_overlay) lv_obj_add_flag(quiz_overlay, LV_OBJ_FLAG_HIDDEN);
    quiz_next();
}
/* The pages here paint deep space whatever the theme, so the shared header
 * gets light ink. */
static void quiz_page_style(lv_obj_t *p)
{
    lv_obj_set_style_bg_color(p, lv_color_hex(QC_BASE), 0);
    lv_obj_set_style_bg_grad_color(p, lv_color_hex(QC_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
    lv_obj_t *hdr = lv_obj_get_child(p, 0);
    for (uint32_t i = 0; hdr && i < lv_obj_get_child_count(hdr); i++)
    {
        lv_obj_t *c = lv_obj_get_child(hdr, i);
        if (!lv_obj_check_type(c, &lv_label_class) && lv_obj_get_child_count(c)) c = lv_obj_get_child(c, 0);
        if (lv_obj_check_type(c, &lv_label_class))
            lv_obj_set_style_text_color(c, lv_color_hex(QC_WHITE), 0);
    }
}
static void quiz_game_page(int mode)
{
    if (quiz_timer) { quiz_restore_clip(); lv_timer_delete(quiz_timer); quiz_timer = NULL; }
    lv_obj_t *p, *body = page_new("Music Bip Bop Smarty", &p);
    quiz_page_obj = p;
    memset(&quiz, 0, sizeof quiz);
    quiz.mode = mode;
    quiz.rng = sl_ms() ? sl_ms() : 1;
    quiz.clip_index = -1;
    quiz.stage = 1;
    quiz_active = true;
    mini_update();
    quiz_cover = NULL;
    for (int i = 0; i < 5; i++) quiz_choice_cover[i] = NULL;
    lv_obj_add_event_cb(p, quiz_deleted, LV_EVENT_DELETE, NULL);
    quiz_page_style(p);
    lv_obj_set_style_pad_bottom(body, 14, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);   /* a round fits on the screen */

    /* HUD: round, the depleting timer and the cash */
    lv_obj_t *hud = box(body);
    lv_obj_set_size(hud, lv_pct(100), 44);
    quiz_style_panel(hud, QC_LINE, SL_R_MD);
    quiz_round_label = text(hud, "ROUND 1", &ss_semibold_18, QC_WHITE);
    lv_obj_set_style_text_letter_space(quiz_round_label, 1, 0);
    lv_obj_align(quiz_round_label, LV_ALIGN_LEFT_MID, 14, 0);
    /* the clock's bar starts after the round, which grows with the Text size */
    lv_point_t round_size;
    lv_text_get_size(&round_size, "ROUND 5", &ss_semibold_18, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int track_x = LV_MAX(110, 14 + round_size.x + 14);
    lv_obj_t *track = box(hud);
    lv_obj_set_size(track, 180 - (track_x - 110), 10);
    lv_obj_align(track, LV_ALIGN_LEFT_MID, track_x, 0);
    lv_obj_set_style_radius(track, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x14224a), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    quiz_progress = box(track);
    lv_obj_set_size(quiz_progress, lv_pct(100), 10);
    lv_obj_set_style_radius(quiz_progress, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(quiz_progress, lv_color_hex(QC_CYAN), 0);
    lv_obj_set_style_bg_opa(quiz_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(quiz_progress, lv_color_hex(QC_CYAN), 0);
    lv_obj_set_style_shadow_width(quiz_progress, 12, 0);
    lv_obj_set_style_shadow_opa(quiz_progress, 170, 0);
    quiz_score = quiz_glow(text(hud, "$0", &ss_semibold_23, QC_CYAN), QC_CYAN);
    lv_obj_align(quiz_score, LV_ALIGN_RIGHT_MID, -14, 0);

    quiz_pips = box(body);
    lv_obj_set_size(quiz_pips, lv_pct(100), 24);
    lv_obj_add_event_cb(quiz_pips, quiz_pips_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* the stage: record or medallion, the question, feedback and NEXT */
    quiz_stage = box(body);
    lv_obj_set_size(quiz_stage, lv_pct(100), QUIZ_STAGE_H);
    quiz_style_panel(quiz_stage, QC_CYAN, 22);
    lv_obj_set_style_bg_color(quiz_stage, lv_color_hex(0x0f1f47), 0);
    lv_obj_set_style_shadow_width(quiz_stage, 22, 0);
    lv_obj_set_style_shadow_opa(quiz_stage, 70, 0);
    quiz_mode_label = text(quiz_stage, "", &ss_semibold_18, QC_CYAN);
    lv_obj_set_style_text_letter_space(quiz_mode_label, 2, 0);
    lv_obj_set_pos(quiz_mode_label, 16, 11);
    quiz_timer_label = text(quiz_stage, "10s", &ss_semibold_21, QC_WHITE);
    lv_obj_align(quiz_timer_label, LV_ALIGN_TOP_RIGHT, -16, 9);
    quiz_glyph = box(quiz_stage);
    lv_obj_set_size(quiz_glyph, 108, 108);
    lv_obj_set_pos(quiz_glyph, 16, quiz_q_top() - 3);
    lv_obj_add_event_cb(quiz_glyph, quiz_disc_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(quiz_glyph, quiz_disc_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(quiz_glyph);
    quiz_question = text(quiz_stage, "", &ss_semibold_26, QC_WHITE);
    lv_obj_set_width(quiz_question, W - 28 - 140 - 16);
    lv_label_set_long_mode(quiz_question, LV_LABEL_LONG_WRAP);   /* never clipped: see quiz_set_question */
    lv_obj_set_pos(quiz_question, 140, quiz_q_top());
    quiz_feedback = text(quiz_stage, "", &ss_regular_16, QC_DIM);
    lv_obj_set_width(quiz_feedback, W - 28 - 32 - 132);
    label_lines(quiz_feedback, 1);
    lv_obj_align(quiz_feedback, LV_ALIGN_BOTTOM_LEFT, 16, -13);
    quiz_next_button = quiz_button(quiz_stage, "NEXT " LV_SYMBOL_RIGHT, 124, 38, QC_CYAN, 0x0284c7, QC_BASE,
                                   quiz_next_cb, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(quiz_next_button, 0), &ss_semibold_18, 0);
    lv_obj_align(quiz_next_button, LV_ALIGN_BOTTOM_RIGHT, -12, -9);

    for (int i = 0; i < 5; i++)
    {
        quiz_choices[i] = box(body);
        lv_obj_t *c = quiz_choices[i];
        lv_obj_set_size(c, lv_pct(100), 72);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        /* the click-wheel highlight: a cyan gradient bar under the finger */
        lv_obj_set_style_bg_color(c, lv_color_hex(QC_CYAN), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_color(c, lv_color_hex(0x0284c7), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(c, lv_color_hex(0xa5f3fc), LV_STATE_PRESSED);
        lv_obj_t *answer = text(c, "", &ss_semibold_26, QC_WHITE);
        lv_obj_set_width(answer, W - 28 - 80 - 18);
        lv_obj_align(answer, LV_ALIGN_LEFT_MID, 80, 0);
        lv_obj_t *letter = box(c);
        lv_obj_set_size(letter, 48, 48);
        lv_obj_align(letter, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_radius(letter, 13, 0);
        lv_obj_set_style_border_width(letter, 1, 0);
        lv_obj_set_style_border_color(letter, lv_color_hex(QC_CYAN), 0);
        lv_obj_set_style_border_opa(letter, 120, 0);
        char marker[2] = { (char)('A' + i), 0 };
        lv_obj_center(text(letter, marker, &ss_semibold_26, QC_CYAN));
        quiz_choice_look(i, 0);
        on_click(c, quiz_answer_cb, (void *)(intptr_t)i);
    }

    quiz_overlay = box(p);
    lv_obj_set_pos(quiz_overlay, 0, SB_H + 66);
    lv_obj_set_size(quiz_overlay, W, H - SB_H - 66);
    lv_obj_add_flag(quiz_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(quiz_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(quiz_overlay, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(quiz_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(quiz_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(quiz_overlay, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_hor(quiz_overlay, 14, 0);
    lv_obj_set_style_pad_ver(quiz_overlay, 12, 0);
    lv_obj_set_flex_flow(quiz_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(quiz_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(quiz_overlay, 10, 0);
    lv_obj_add_flag(quiz_overlay, LV_OBJ_FLAG_HIDDEN);

    quiz_fx = box(p);
    lv_obj_set_size(quiz_fx, 380, 380);
    lv_obj_add_event_cb(quiz_fx, quiz_fx_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(quiz_fx, LV_OBJ_FLAG_HIDDEN);

    quiz_timer = lv_timer_create(quiz_tick, 33, NULL);
    quiz_next();
    page_push(p);
}

/* ---- the menu: a title card with a live equaliser, the player's record,
 * and a tile per mode. The only motion that lasts is the equaliser: one
 * 30 fps timer over a small strip, which idles when the menu is covered. */
#define QUIZ_EQ_BARS 9
static lv_obj_t *quiz_menu_page, *quiz_eq[QUIZ_EQ_BARS];
static lv_timer_t *quiz_eq_timer;
static void quiz_mode_cb(lv_event_t *e)
{
    quiz_game_page((int)(intptr_t)lv_event_get_user_data(e));
}
static void quiz_menu_press_cb(lv_event_t *e) { (void)e; sfx_play(SFX_TAP); }
static void quiz_eq_tick(lv_timer_t *t)
{
    bool shown = quiz_menu_page && npages && pages[npages - 1] == quiz_menu_page &&
                 !sl_power_hides_ui() && !overlay_visible();
    lv_timer_set_period(t, shown ? 33 : 500);
    if (!shown) return;
    /* a few slow sines per bar read as music, not noise; faster with a song on */
    float k = lv_tick_get() * (is_playing() ? 0.0062f : 0.0036f);
    for (int i = 0; i < QUIZ_EQ_BARS; i++)
    {
        float v = 0.55f + 0.25f * sinf(k * (1.0f + i * 0.23f) + i * 1.7f) +
                  0.2f * sinf(k * 2.3f + i * 0.9f);
        int h = 8 + (int)(v * 34);
        if (lv_obj_get_height(quiz_eq[i]) != h) lv_obj_set_height(quiz_eq[i], h);
    }
}
static bool quiz_menu_shown(void)
{
    return quiz_menu_page && npages && pages[npages - 1] == quiz_menu_page;
}
static void quiz_menu_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != quiz_menu_page) return;
    if (quiz_eq_timer) { lv_timer_delete(quiz_eq_timer); quiz_eq_timer = NULL; }
    quiz_menu_page = NULL;
}
static void quiz_logo_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2 - 14;
    for (int r = 88; r > 20; r -= 22) quiz_circle(layer, cx, cy, r, QC_CYAN, 9);
    quiz_ring(layer, cx, cy, 118, 1, QC_MAGENTA, 50);
    quiz_ring(layer, cx, cy, 86, 1, QC_CYAN, 60);
    static const int16_t stars[][3] = { {-190, -60, 2}, {-150, 50, 1}, {-110, -78, 1}, {160, -64, 2},
                                       {196, 30, 1}, {120, 70, 2}, {-200, 20, 1}, {70, -84, 1} };
    for (unsigned i = 0; i < sizeof stars / sizeof stars[0]; i++)
    {
        int x = cx + stars[i][0], y = cy + stars[i][1], s = stars[i][2] * 4;
        quiz_fill(layer, x - s, y, x + s, y, 0xffffff, 160, 0);
        quiz_fill(layer, x, y - s, x, y + s, 0xffffff, 160, 0);
    }
}
/* entrance: rise and fade in, one after another */
static void quiz_rise_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void quiz_rise_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void quiz_rise(lv_obj_t *o, int delay)
{
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, 26, 0);
    lv_anim_set_exec_cb(&a, quiz_rise_y);
    lv_anim_start(&a);
    lv_anim_set_duration(&a, 240);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, quiz_rise_opa);
    lv_anim_start(&a);
}
static void quiz_mode_tile(lv_obj_t *grid, const char *title, const char *detail, const char *symbol,
                           int mode, uint32_t from, uint32_t to)
{
    static lv_style_transition_dsc_t press;
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0 };
    lv_style_transition_dsc_init(&press, props, lv_anim_path_ease_out, 110, 0, NULL);
    int w = (W - 28 - 12) / 2;
    /* the caption under the heading, the tile as tall as both (158 at Standard) */
    int hy = 80, cy = hy + lv_font_get_line_height(&ss_semibold_23) + 4;
    int th = LV_MAX(158, cy + 2 * lv_font_get_line_height(&ss_regular_16) + 8);
    lv_obj_t *item = box(grid);
    lv_obj_set_size(item, w, th);
    lv_obj_set_style_radius(item, SL_R_XL, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(from), 0);
    lv_obj_set_style_bg_grad_color(item, lv_color_darken(lv_color_hex(to), 50), 0);   /* deeper where the caption sits */
    lv_obj_set_style_bg_grad_dir(item, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_color(item, lv_color_white(), 0);
    lv_obj_set_style_border_opa(item, 50, 0);
    lv_obj_set_style_shadow_color(item, lv_color_hex(from), 0);
    lv_obj_set_style_shadow_width(item, 22, 0);
    lv_obj_set_style_shadow_opa(item, 70, 0);
    lv_obj_set_style_shadow_offset_y(item, 6, 0);
    /* pressed: it gives under the finger, and comes back when let go */
    lv_obj_set_style_transform_pivot_x(item, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(item, th / 2, 0);
    lv_obj_set_style_transform_scale_x(item, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(item, 236, LV_STATE_PRESSED);
    lv_obj_set_style_transition(item, &press, 0);
    lv_obj_set_style_transition(item, &press, LV_STATE_PRESSED);
    lv_obj_t *badge = box(item);
    lv_obj_set_size(badge, 52, 52);
    sl_align(badge, LV_ALIGN_TOP_LEFT, 16, 16);   /* at the reading edge, in Hebrew and Arabic the right */
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(badge, 45, 0);
    lv_obj_center(text(badge, symbol, &sl_sym_24, QC_WHITE));
    lv_obj_t *heading = text(item, title, &ss_semibold_23, QC_WHITE);
    sl_place(heading, 16, hy);
    lv_obj_set_width(heading, w - 28);
    label_lines(heading, 1);
    lv_obj_t *caption = text(item, detail, &ss_regular_16, QC_WHITE);
    lv_obj_set_style_text_opa(caption, LV_OPA_COVER, 0);
    sl_place(caption, 16, cy);
    lv_obj_set_width(caption, w - 28);
    label_lines(caption, 2);
    lv_obj_add_event_cb(item, quiz_menu_press_cb, LV_EVENT_PRESSED, NULL);
    on_click(item, quiz_mode_cb, (void *)(intptr_t)mode);
    quiz_rise(item, 160 + mode * 60);
}
static void quiz_menu_stat(lv_obj_t *strip, const char *value, const char *name)
{
    lv_obj_t *cell = box(strip);
    lv_obj_set_size(cell, lv_pct(25), lv_pct(100));
    lv_obj_t *v = text(cell, value, &ss_semibold_21, QC_CYAN);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *n = text(cell, name, &ss_regular_16, QC_DIM);
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -9);
    sl_fit(n);                           /* a long word steps down a size inside its quarter */
}
static void quiz_page(void)
{
    if (page_bring_back(quiz_menu_page)) return;
    quiz_stats_load();
    lv_obj_t *p, *body = page_new("Music Bip Bop Smarty", &p);
    quiz_page_style(p);
    quiz_menu_page = p;
    lv_obj_add_event_cb(p, quiz_menu_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(body, 12, 0);
    /* a game's front door: it all fits, no mini player, nothing to scroll */
    lv_obj_set_style_pad_bottom(body, 14, 0);
    if (sl_text_step < 2) lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);   /* larger text: it scrolls to the last cards */
    lv_obj_t *logo = box(body);
    lv_obj_set_size(logo, lv_pct(100), 200);
    lv_obj_add_event_cb(logo, quiz_logo_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *kicker = text(logo, "BIP BOP", &ss_semibold_21, QC_MAGENTA);
    lv_obj_set_style_text_letter_space(kicker, 8, 0);
    quiz_glow(kicker, QC_MAGENTA);
    lv_obj_align(kicker, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t *name = quiz_glow(text(logo, "Smarty", &sl_sym_48, QC_WHITE), QC_CYAN);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -16);
    /* the equaliser, bars growing up from a shared floor */
    lv_obj_t *eq = box(logo);
    lv_obj_set_size(eq, QUIZ_EQ_BARS * 14, 44);
    lv_obj_align(eq, LV_ALIGN_BOTTOM_MID, 0, -40);
    for (int i = 0; i < QUIZ_EQ_BARS; i++)
    {
        quiz_eq[i] = box(eq);
        lv_obj_set_size(quiz_eq[i], 8, 12);
        lv_obj_align(quiz_eq[i], LV_ALIGN_BOTTOM_LEFT, i * 14 + 3, 0);
        lv_obj_set_style_radius(quiz_eq[i], SL_R_FULL, 0);
        lv_obj_set_style_bg_color(quiz_eq[i], lv_color_hex(i % 3 == 1 ? QC_MAGENTA : QC_CYAN), 0);
        lv_obj_set_style_bg_opa(quiz_eq[i], LV_OPA_COVER, 0);
    }
    lv_obj_t *tag = text(logo, "THE POCKET GAME SHOW", &ss_semibold_18, QC_CYAN);
    lv_obj_set_style_text_letter_space(tag, 3, 0);
    lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_t *strip = box(body);
    /* tall enough for the number over its name at every text size (70 at Standard) */
    lv_obj_set_size(strip, lv_pct(100), LV_MAX(70, 10 + lv_font_get_line_height(&ss_semibold_21) + 2 +
                                                  lv_font_get_line_height(&ss_regular_16) + 9));
    quiz_style_panel(strip, QC_LINE, 18);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_base_dir(strip, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);   /* the first at the reading edge */
    char money[20], streak[12], fast[12], games[12];
    quiz_money(money, sizeof money, quiz_stats.best_cash);
    snprintf(streak, sizeof streak, "%d", quiz_stats.best_streak);
    if (quiz_stats.best_react)
        snprintf(fast, sizeof fast, "%d.%ds", quiz_stats.best_react / 1000, quiz_stats.best_react % 1000 / 100);
    else strlcpy(fast, "-", sizeof fast);
    snprintf(games, sizeof games, "%d", quiz_stats.games);
    quiz_menu_stat(strip, money, "high score");
    quiz_menu_stat(strip, streak, "best streak");
    quiz_menu_stat(strip, fast, "fastest avg");
    quiz_menu_stat(strip, games, "games");
    quiz_rise(strip, 80);

    lv_obj_t *rules = text(body, "5 rounds · 8 questions · 3 strikes and you're out", &ss_regular_16, QC_DIM);
    lv_obj_set_width(rules, lv_pct(100));
    lv_obj_set_style_text_align(rules, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *grid = box(body);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_base_dir(grid, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);   /* the first at the reading edge */
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_style_pad_bottom(grid, 10, 0);
    char movies[64], tv[64];
    int n_movies = 0, n_tv = 0;
    for (unsigned i = 0; i < sizeof quiz_trivia / sizeof quiz_trivia[0]; i++)
    { n_movies += quiz_trivia[i].kind == QUIZ_MOVIE; n_tv += quiz_trivia[i].kind == QUIZ_TV; }
    snprintf(movies, sizeof movies, sl_tr("%d big-screen questions"), n_movies);
    snprintf(tv, sizeof tv, sl_tr("%d small-screen questions"), n_tv);
    quiz_mode_tile(grid, "Music Quiz", "Clips and questions from your library", LV_SYMBOL_AUDIO,
                   QUIZ_MODE_MUSIC, 0x06b6d4, 0x1d4ed8);
    quiz_mode_tile(grid, "Movie Trivia", movies, LV_SYMBOL_VIDEO, QUIZ_MODE_MOVIES, 0xd946ef, 0x6d28d9);
    quiz_mode_tile(grid, "TV Trivia", tv, LV_SYMBOL_IMAGE, QUIZ_MODE_TV, 0xf59e0b, 0xdc2626);
    quiz_mode_tile(grid, "Surprise Me", "A bit of everything", LV_SYMBOL_SHUFFLE, QUIZ_MODE_MIXED,
                   0x84cc16, 0x0f766e);
    quiz_eq_timer = lv_timer_create(quiz_eq_tick, 33, NULL);
    quiz_eq_tick(quiz_eq_timer);
    page_push(p);
}
