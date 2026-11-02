/*
 *  Notifications: everything GabagoolOS has to tell you, in one place.
 *
 *  A notification has a title and, when the message runs to a second
 *  sentence, a line of detail ("Could not save the playlist" / "Check card
 *  space."), where it came from (Music, Wi-Fi, your pet...), whether it is a
 *  failure, and sometimes somewhere to go (the Wi-Fi page, Now Playing).
 *  notify() keeps it in the history the Notification Center lists, newest
 *  first, at most NOTE_MAX, and shows it as a banner across the top: a
 *  pane of dark glass with its source's mark, that drops in, stays a moment
 *  and lifts away. A tap opens where it leads, if it leads anywhere, or
 *  sends it away; a flick up sends it away. Either way it stays in the
 *  history, unread, until it is seen there or cleared.
 *
 *  One banner at a time. A newer message takes it over at once (most often
 *  it answers what was just touched), except over a failure that has been
 *  up less than NOTE_WARN_MIN: then the newest waits in the one place behind
 *  it, and anything newer still replaces that. So a burst of routine words
 *  never makes an error wait, nor buries it, and nothing piles up.
 *
 *  Repeats: the same words again within a few seconds count up (x2) rather
 *  than add a line. Something ongoing (the library scan, the battery, the
 *  shuffle switch) passes a key, and its one entry is brought up to date
 *  and to the top instead. Distinct events are never merged.
 *
 *  Read: an entry is unread until its row has been in sight in the open
 *  Notification Center for a second (or its banner was tapped through to
 *  where it leads). Rows scrolled out of sight stay unread.
 *
 *  toast(), toast_warn() and toast_private() are the callers' names: the
 *  source is the file they are in. toast_private's words can name something
 *  of yours (a network, a song sent to the player) and stay off the card.
 *  The history is saved a little after it changes, and at power off, in
 *  the usual way (a temporary file, synced, renamed).
 */

#define NOTE_MAX      40
#define NOTE_TITLE    80
#define NOTE_BODY     100
#define NOTE_KEY      16
#define NOTE_FILE     ROCKBOX_DIR "/sloppy/notes.txt"
#define NOTE_SHOW_MS  2600               /* a banner */
#define NOTE_WARN_MS  4500               /* a failure's */
#define NOTE_WARN_MIN 2000               /* a failure stays at least this, whatever comes */
#define NOTE_REPEAT_S 5                  /* the same words within this count up */
#define NOTE_SEEN_MS  1000               /* in sight this long in the center: read */

enum { NOTE_INFO, NOTE_WARN };
enum { NS_SYSTEM, NS_MUSIC, NS_LIBRARY, NS_PET, NS_RADIO, NS_WIFI, NS_BT, NS_GAMES, NS_SETTINGS, NS_COUNT };
enum { NA_NONE, NA_NOW_PLAYING, NA_MUSIC, NA_PLAYLISTS, NA_PET, NA_RADIO, NA_WIFI, NA_BT, NA_UPDATE };
#define NOTE_PRIVATE 1

struct note
{
    char title[NOTE_TITLE];
    char body[NOTE_BODY];
    char key[NOTE_KEY];                  /* an ongoing event's; empty for one-off events */
    uint8_t kind, src, action;
    bool priv;                           /* not written to the card */
    bool read;
    uint16_t count;                      /* the same, again, at once */
    uint32_t at;                         /* seconds, from the player's clock */
    uint32_t id;
    uint32_t seen_at;                    /* tick it came in sight in the center; 0 out of it */
};
static struct
{
    struct note list[NOTE_MAX];          /* newest first */
    int n;
    uint32_t next_id;
    bool loaded, dirty;
    lv_timer_t *save_timer;
    uint32_t sound_at;
} nh;
static struct
{
    lv_obj_t *obj, *icon, *glyph, *title, *body, *chev;
    lv_timer_t *timer;
    uint32_t id;                         /* the note it shows; 0 none */
    uint32_t shown_at;
    uint32_t next;                       /* the one waiting behind a failure; 0 none */
    lv_point_t press;
    bool flicked;
    int8_t dir;                          /* where it sits: +1 the bottom, -1 the top; it leaves that way */
} nb;
#define banner_obj nb.obj

static const struct { const char *name; const char *glyph; uint32_t color; } note_srcs[NS_COUNT] = {
    [NS_SYSTEM]   = { "GabagoolOS", LV_SYMBOL_HOME, 0x5b6b82 },
    [NS_MUSIC]    = { "Music", LV_SYMBOL_AUDIO, 0xe8408a },
    [NS_LIBRARY]  = { "Library", LV_SYMBOL_DIRECTORY, 0x2f8de4 },
    [NS_PET]      = { "Sloppy Gotchi", LV_SYMBOL_EYE_OPEN, 0x5f9e3a },
    [NS_RADIO]    = { "Sloppy FM", LV_SYMBOL_VOLUME_MAX, 0xe8762c },
    [NS_WIFI]     = { "Wi-Fi", LV_SYMBOL_WIFI, 0x2f8de4 },
    [NS_BT]       = { "Bluetooth", LV_SYMBOL_BLUETOOTH, 0x3a62d8 },
    [NS_GAMES]    = { "Playroom", LV_SYMBOL_PLAY, 0x8b6fd6 },
    [NS_SETTINGS] = { "Settings", LV_SYMBOL_SETTINGS, 0x66738a },
};
#define NOTE_WARN_COLOR 0xff9f0a

static bool qp_is_visible(void);
static void qp_notes_refresh(void);
static void hidden_if_changed(lv_obj_t *o, bool hidden);
static void note_open(int action);
static void pop_resume(void);

/* Where a message came from, by the file that said it */
static int note_src(const char *file)
{
    static const struct { const char *part; int src; } map[] = {
        { "wifi", NS_WIFI }, { "bluetooth", NS_BT }, { "pet", NS_PET }, { "radio", NS_RADIO },
        { "dbupdate", NS_LIBRARY }, { "quiz", NS_GAMES }, { "chess", NS_GAMES }, { "snake", NS_GAMES },
        { "spoon", NS_GAMES }, { "ragdoll", NS_GAMES }, { "chance", NS_GAMES },
        { "settings", NS_SETTINGS }, { "appearance", NS_SETTINGS },
        { "playback", NS_MUSIC }, { "browse", NS_MUSIC }, { "swipe", NS_MUSIC }, { "mixtape", NS_MUSIC },
        { "history", NS_MUSIC }, { "coverflow", NS_MUSIC }, { "search", NS_MUSIC }, { "vinyl", NS_MUSIC },
        { "grave", NS_MUSIC }, { "lyrics", NS_MUSIC }, { "visualizer", NS_MUSIC },
    };
    const char *base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : file ? file : "";
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strstr(base, map[i].part)) return map[i].src;
    return NS_SYSTEM;
}

/* The player's clock in seconds: days and time of day, without the time
 * zone file mktime reads. Only differences are shown, so local is fine. */
static uint32_t note_clock(void)
{
    struct tm *t = get_time();
    if (!t) return 0;
    int y = t->tm_year + 1900, m = t->tm_mon + 1;
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
    long days = (long)era * 146097 + (long)yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return (uint32_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}
/* "now", "5 min ago", "2 h ago", "3 d ago" */
static const char *note_age(uint32_t at, char *b, size_t n)
{
    uint32_t now = note_clock(), d = now > at ? now - at : 0;
    if (d < 60) return sl_tr("now");
    if (d < 3600) snprintf(b, n, "%u %s", (unsigned)(d / 60), sl_tr("min ago"));
    else if (d < 86400) snprintf(b, n, "%u %s", (unsigned)(d / 3600), sl_tr("h ago"));
    else snprintf(b, n, "%u %s", (unsigned)(d / 86400), sl_tr("d ago"));
    return b;
}

/* Plain words: control characters become spaces, a cut never splits a
 * character, and no space is left at either end. */
static void note_clean(char *out, size_t n, const char *in)
{
    while (*in && (*in == ' ' || (unsigned char)*in < 0x20)) in++;
    size_t i = 0;
    for (; in[i] && i < n - 1; i++) out[i] = (unsigned char)in[i] < 0x20 ? ' ' : in[i];
    while (i > 0 && in[i] && ((unsigned char)in[i] & 0xc0) == 0x80) i--;   /* cut mid-character: before it */
    while (i > 0 && out[i - 1] == ' ') i--;
    out[i] = '\0';
}
/* "Could not save the playlist. Check card space." -> the first sentence
 * is the title (without its stop), the rest the detail. A single sentence
 * is all title. */
static void note_split(const char *msg, char *title, char *body)
{
    const char *stop = strstr(msg, ". ");
    if (!stop || !stop[2]) { note_clean(title, NOTE_TITLE, msg); body[0] = '\0'; return; }
    char first[NOTE_TITLE];
    size_t len = (size_t)(stop - msg) < sizeof first - 1 ? (size_t)(stop - msg) : sizeof first - 1;
    memcpy(first, msg, len);
    first[len] = '\0';
    note_clean(title, NOTE_TITLE, first);
    note_clean(body, NOTE_BODY, stop + 2);
}

/* ---------------------------------------------------------- the card */
/* NOT2, a line each, oldest first: kind src action read count at key TAB
 * title TAB body. NOT1 files (kind count at text) are read too. */
static void notes_save(void)
{
    static char b[16 + NOTE_MAX * (NOTE_TITLE + NOTE_BODY + NOTE_KEY + 48)];
    nh.dirty = false;
    int len = snprintf(b, sizeof b, "NOT2\n");
    for (int i = nh.n - 1; i >= 0; i--)
    {
        const struct note *e = &nh.list[i];
        if (e->priv) continue;
        len += snprintf(b + len, sizeof b - len, "%u %u %u %u %u %lu %s\t%s\t%s\n", (unsigned)e->kind,
                        (unsigned)e->src, (unsigned)e->action, (unsigned)e->read, (unsigned)e->count,
                        (unsigned long)e->at, e->key[0] ? e->key : "-", e->title, e->body);
        if (len >= (int)sizeof b) return;
    }
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(NOTE_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len && fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(NOTE_FILE ".tmp", NOTE_FILE) < 0) remove(NOTE_FILE ".tmp");
}
static void notes_save_cb(lv_timer_t *t) { (void)t; nh.save_timer = NULL; notes_save(); }
/* a moment after the last change: a burst is one write */
static void notes_dirty(void)
{
    nh.dirty = true;
    if (nh.save_timer) { lv_timer_reset(nh.save_timer); return; }
    nh.save_timer = lv_timer_create(notes_save_cb, 3000, NULL);
    lv_timer_set_repeat_count(nh.save_timer, 1);
}
/* power off, a reboot: what is waiting to be saved, now */
static void notes_flush(void)
{
    if (nh.save_timer) { lv_timer_delete(nh.save_timer); nh.save_timer = NULL; }
    if (nh.dirty) notes_save();
}
static void notes_load(void)
{
    if (nh.loaded) return;
    nh.loaded = true;
    static char b[16 + NOTE_MAX * (NOTE_TITLE + NOTE_BODY + NOTE_KEY + 48) + 1];
    int fd = open(NOTE_FILE, O_RDONLY);
    if (fd < 0) return;
    ssize_t got = read(fd, b, sizeof b - 1);
    close(fd);
    bool v2 = got >= 5 && !memcmp(b, "NOT2\n", 5);
    if (got < 5 || (!v2 && memcmp(b, "NOT1\n", 5))) return;
    b[got] = '\0';
    for (char *line = b + 5, *nl; *line && (nl = strchr(line, '\n')); line = nl + 1)
    {
        *nl = '\0';
        unsigned kind = 0, src = NS_SYSTEM, action = NA_NONE, read = 0, count;
        unsigned long at;
        char key[NOTE_KEY] = "";
        int used = 0;
        if (v2 ? sscanf(line, "%u %u %u %u %u %lu %15s %n", &kind, &src, &action, &read, &count, &at, key, &used) < 7
               : sscanf(line, "%u %u %lu %n", &kind, &count, &at, &used) < 3) continue;
        if (!used || !line[used]) continue;
        if (nh.n == NOTE_MAX) nh.n--;   /* oldest first in the file: the ones that go are older */
        memmove(&nh.list[1], &nh.list[0], sizeof nh.list[0] * (size_t)nh.n);
        struct note *e = &nh.list[0];
        memset(e, 0, sizeof *e);
        char *title = line + used, *body = v2 ? strchr(title, '\t') : NULL;
        if (body) *body++ = '\0';
        note_clean(e->title, NOTE_TITLE, title);
        note_clean(e->body, NOTE_BODY, body ? body : "");
        if (!e->title[0]) continue;
        if (strcmp(key, "-")) strlcpy(e->key, key, sizeof e->key);
        e->kind = kind == NOTE_WARN ? NOTE_WARN : NOTE_INFO;
        e->src = src < NS_COUNT ? (uint8_t)src : NS_SYSTEM;
        e->action = action <= NA_UPDATE ? (uint8_t)action : NA_NONE;
        e->read = v2 ? read != 0 : true;   /* from before there was a read state: seen */
        e->count = (uint16_t)(count ? count : 1);
        e->at = (uint32_t)at;
        e->id = ++nh.next_id;
        nh.n++;
    }
}

static struct note *note_find(uint32_t id)
{
    for (int i = 0; i < nh.n; i++) if (nh.list[i].id == id) return &nh.list[i];
    return NULL;
}
static int note_unread(void)
{
    int n = 0;
    for (int i = 0; i < nh.n; i++) n += !nh.list[i].read;
    return n;
}
/* The Notification Center is opening: what was in sight last time starts
 * again, or a row glimpsed then would read as seen at once */
static void notes_unseen(void)
{
    for (int i = 0; i < nh.n; i++) nh.list[i].seen_at = 0;
}
static void note_mark_read(struct note *e)
{
    if (!e || e->read) return;
    e->read = true;
    if (!e->priv) notes_dirty();
}
static void nbanner_hide_now(void);
/* Clear one note from the history (0: all of them) */
static void note_clear(uint32_t id)
{
    for (int i = 0; i < nh.n; )
    {
        if (id && nh.list[i].id != id) { i++; continue; }
        memmove(&nh.list[i], &nh.list[i + 1], sizeof nh.list[0] * (size_t)(nh.n - i - 1));
        nh.n--;
    }
    if (nb.next && !note_find(nb.next)) nb.next = 0;
    if (nb.id && !note_find(nb.id)) nbanner_hide_now();
    notes_dirty();
    qp_notes_refresh();
}

/* ------------------------------------------------------------ banner */
static void nbanner_set_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void nbanner_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void nbanner_move(int y0, int y1, int o0, int o1, int ms, lv_anim_completed_cb_t done)
{
    lv_anim_delete(nb.obj, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, nb.obj);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, y0, y1);
    lv_anim_set_exec_cb(&a, nbanner_set_y);
    lv_anim_start(&a);
    lv_anim_set_values(&a, o0, o1);
    lv_anim_set_exec_cb(&a, nbanner_set_opa);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void nbanner_show(uint32_t id);
static int sl_overlay_place(lv_obj_t *o, int top);
/* gone: the one waiting behind it, if it is still there and unread */
static void nbanner_gone(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(nb.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(nb.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(nb.obj, 0, 0);
    nb.id = 0;
    if (po_is_open()) return;            /* it waits for the power off slider to go */
    uint32_t next = nb.next;
    nb.next = 0;
    const struct note *e = note_find(next);
    if (e && !e->read && !qp_is_visible()) nbanner_show(next);
}
static void nbanner_hide(void)
{
    if (!nb.obj || lv_obj_has_flag(nb.obj, LV_OBJ_FLAG_HIDDEN) || !nb.id) return;   /* gone, or going */
    if (nb.timer) lv_timer_pause(nb.timer);
    nb.id = 0;
    nbanner_move(lv_obj_get_style_translate_y(nb.obj, 0), 24 * nb.dir, lv_obj_get_style_opa(nb.obj, 0), LV_OPA_TRANSP,
                 SL_T_FAST, nbanner_gone);
}
/* No banner, nor one waiting: the Notification Center is opening */
static void nbanner_hide_now(void)
{
    nb.next = 0;
    if (!nb.obj) return;
    if (nb.timer) lv_timer_pause(nb.timer);
    lv_anim_delete(nb.obj, NULL);
    lv_obj_add_flag(nb.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(nb.obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(nb.obj, 0, 0);
    nb.id = 0;
}
/* under the power off slider a banner stays where it is, unseen: it has
 * its time when the slider goes (nbanner_resume) */
static void nbanner_timer_cb(lv_timer_t *t) { (void)t; if (!po_is_open()) nbanner_hide(); }
/* A tap goes where it leads, if it leads anywhere, else sends it away; a
 * flick up sends it away. Either way it stays in the history. */
static void nbanner_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) { nb.press = p; nb.flicked = false; }
    else if (c == LV_EVENT_PRESSING && !nb.flicked && (p.y - nb.press.y) * nb.dir > 12) { nb.flicked = true; nbanner_hide(); }   /* flicked off its edge */
    else if (c == LV_EVENT_CLICKED && !nb.flicked)
    {
        struct note *n = note_find(nb.id);
        int action = n ? n->action : NA_NONE;
        if (action) note_mark_read(n);
        nbanner_hide();
        if (action) note_open(action);
    }
}
/* the source's mark: its colour, or amber with a warning sign for a failure */
static void note_mark(lv_obj_t *circle, lv_obj_t *glyph, const struct note *e)
{
    bool warn = e->kind == NOTE_WARN;
    lv_obj_set_style_bg_color(circle, lv_color_hex(warn ? NOTE_WARN_COLOR : note_srcs[e->src].color), 0);
    lv_label_set_text(glyph, warn ? LV_SYMBOL_WARNING : note_srcs[e->src].glyph);
}
/* A label's height for at most lines lines of s, set before its text: a
 * dotted label cuts its text to the height it has. */
static void note_lines(lv_obj_t *l, const char *s, int width, int lines)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, width, LV_TEXT_FLAG_NONE);
    int line = lv_font_get_line_height(f), have = (sz.y + line - 1) / line;
    lv_obj_set_height(l, (have < 1 ? 1 : have > lines ? lines : have) * line);
    lv_label_set_text(l, s);
}
#define NB_TEXT_W (W - 2 * SL_INSET - 4 - 72 - 16)
static void nbanner_fill(const struct note *e)
{
    char t[NOTE_TITLE + 16];
    if (e->count > 1) snprintf(t, sizeof t, "%s  \xc3\x97%u", e->title, (unsigned)e->count);
    else strlcpy(t, e->title, sizeof t);
    int w = NB_TEXT_W - (e->action ? 26 : 0);
    lv_obj_set_width(nb.title, w);
    lv_obj_set_width(nb.body, w);
    note_lines(nb.title, t, w, e->body[0] ? 1 : 2);
    if (e->body[0]) note_lines(nb.body, e->body, w, 1);
    hidden_if_changed(nb.body, !e->body[0]);
    hidden_if_changed(nb.chev, !e->action);
    note_mark(nb.icon, nb.glyph, e);
}
static void nbanner_build(void)
{
    /* a pane of dark glass across the top, over the status bar as the
     * phone's are: it reads on any page and in every theme */
    nb.obj = box(lv_layer_top());
    lv_obj_set_width(nb.obj, W - 2 * SL_INSET);   /* the toast's width: clear of an A-Z rail */
    lv_obj_set_height(nb.obj, LV_SIZE_CONTENT);
    lv_obj_align(nb.obj, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(nb.obj, lv_color_hex(0x16181e), 0);
    lv_obj_set_style_bg_opa(nb.obj, 245, 0);
    lv_obj_set_style_radius(nb.obj, SL_R_LG, 0);
    sl_elevate(nb.obj, SL_E3, 0);
    lv_obj_set_style_border_width(nb.obj, 1, 0);
    lv_obj_set_style_border_color(nb.obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(nb.obj, 30, 0);
    lv_obj_set_style_pad_left(nb.obj, 72, 0);
    lv_obj_set_style_pad_right(nb.obj, 16, 0);
    lv_obj_set_style_pad_ver(nb.obj, 14, 0);
    lv_obj_set_flex_flow(nb.obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nb.obj, 2, 0);
    lv_obj_set_style_min_height(nb.obj, 68, 0);
    lv_obj_set_flex_align(nb.obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(nb.obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(nb.obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(nb.obj, nbanner_event, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(nb.obj, lv_color_hex(0x262a33), LV_STATE_PRESSED);
    nb.title = text(nb.obj, "", &ss_semibold_18, 0xf5f7fc);         /* child 0: the words */
    lv_obj_set_style_text_color(nb.title, lv_color_hex(0xf5f7fc), 0);   /* white in every theme */
    lv_obj_set_style_base_dir(nb.title, LV_BASE_DIR_AUTO, 0);
    lv_label_set_long_mode(nb.title, LV_LABEL_LONG_DOT);
    nb.body = text(nb.obj, "", &ss_regular_16, 0xaab1bf);
    lv_obj_set_style_text_color(nb.body, lv_color_hex(0xb4bac6), 0);
    lv_obj_set_style_base_dir(nb.body, LV_BASE_DIR_AUTO, 0);
    lv_label_set_long_mode(nb.body, LV_LABEL_LONG_DOT);
    nb.icon = box(nb.obj);
    lv_obj_set_size(nb.icon, 40, 40);
    lv_obj_set_style_radius(nb.icon, SL_R_ICON(40), 0);
    lv_obj_set_style_bg_opa(nb.icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(nb.icon, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(nb.icon, LV_ALIGN_LEFT_MID, -58, 0);
    nb.glyph = lv_label_create(nb.icon);
    lv_obj_set_style_text_font(nb.glyph, &sl_sym_20, 0);
    lv_obj_set_style_text_color(nb.glyph, lv_color_white(), 0);
    lv_obj_center(nb.glyph);
    nb.chev = lv_label_create(nb.obj);
    lv_label_set_text(nb.chev, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(nb.chev, &sl_sym_16, 0);
    lv_obj_set_style_text_color(nb.chev, lv_color_hex(0x8a92a3), 0);
    lv_obj_add_flag(nb.chev, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(nb.chev, LV_ALIGN_RIGHT_MID, 0, 0);
}
static void nbanner_show(uint32_t id)
{
    const struct note *e = note_find(id);
    if (!e) return;
    if (!nb.obj) nbanner_build();
    /* Zune: a square black band with a pink edge, not rounded glass */
    lv_obj_set_style_radius(nb.obj, sl_zune() ? 0 : SL_R_LG, 0);
    nb.dir = (int8_t)sl_overlay_place(nb.obj, 6);   /* never over a control */
    lv_obj_set_style_bg_color(nb.obj, lv_color_hex(sl_zune() ? 0x000000 : 0x16181e), 0);
    lv_obj_set_style_border_color(nb.obj, sl_zune() ? lv_color_hex(ZN_PINK) : lv_color_white(), 0);
    lv_obj_set_style_border_opa(nb.obj, sl_zune() ? LV_OPA_COVER : 30, 0);
    lv_obj_set_style_border_side(nb.obj, sl_zune() ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_width(nb.obj, sl_zune() ? 4 : 1, 0);
    bool fresh = lv_obj_has_flag(nb.obj, LV_OBJ_FLAG_HIDDEN) || !nb.id;
    if (nb.id != id) nb.shown_at = lv_tick_get();
    nb.id = id;
    /* Hebrew and Arabic: the mark at the right, the words from it, the
     * chevron at the left pointing on */
    bool rtl = sl_ui_rtl();
    lv_obj_set_style_pad_left(nb.obj, rtl ? 16 : 72, 0);
    lv_obj_set_style_pad_right(nb.obj, rtl ? 72 : 16, 0);
    lv_obj_align(nb.icon, rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, rtl ? 58 : -58, 0);
    lv_label_set_text(nb.chev, sl_chevron());
    lv_obj_align(nb.chev, rtl ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_align(nb.title, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_AUTO, 0);
    lv_obj_set_style_text_align(nb.body, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_AUTO, 0);
    lv_obj_set_flex_align(nb.obj, LV_FLEX_ALIGN_CENTER, rtl ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    nbanner_fill(e);
    lv_obj_remove_flag(nb.obj, LV_OBJ_FLAG_HIDDEN);
    if (fresh)
        nbanner_move(lv_anim_get(nb.obj, nbanner_set_opa) ? lv_obj_get_style_translate_y(nb.obj, 0) : 24 * nb.dir, 0,
                     lv_anim_get(nb.obj, nbanner_set_opa) ? lv_obj_get_style_opa(nb.obj, 0) : LV_OPA_TRANSP,
                     LV_OPA_COVER, SL_T_PAGE, NULL);
    lv_obj_move_foreground(nb.obj);
    if (!nb.timer) nb.timer = lv_timer_create(nbanner_timer_cb, NOTE_SHOW_MS, NULL);
    lv_timer_set_period(nb.timer, e->kind == NOTE_WARN ? NOTE_WARN_MS : NOTE_SHOW_MS);
    lv_timer_reset(nb.timer);
    lv_timer_resume(nb.timer);
}
/* A new note for the banner: shown now, unless a failure is up and has not
 * had its moment yet; then it waits in the one place behind it. */
static void nbanner_offer(const struct note *e)
{
    if (po_is_open() && nb.id != e->id)
    {
        /* nothing moves under the power off slider: it waits, unless a
         * failure is waiting already */
        const struct note *waiting = note_find(nb.next);
        if (!waiting || waiting->kind != NOTE_WARN || e->kind == NOTE_WARN) nb.next = e->id;
        return;
    }
    const struct note *up = nb.id ? note_find(nb.id) : NULL;
    if (up && up != e && up->kind == NOTE_WARN && e->kind != NOTE_WARN &&
        lv_tick_elaps(nb.shown_at) < NOTE_WARN_MIN)
    {
        nb.next = e->id;
        uint32_t left = NOTE_WARN_MIN - lv_tick_elaps(nb.shown_at);
        lv_timer_set_period(nb.timer, left < 300 ? 300 : left);   /* it goes when its moment is up */
        lv_timer_reset(nb.timer);
        return;
    }
    if (nb.next == e->id) nb.next = 0;
    nbanner_show(e->id);
}

/* The power off slider went away (and the player is not turning off):
 * the banner it covered has its time again, and what came meanwhile shows */
static void nbanner_resume(void)
{
    pop_resume();
    if (nb.id && nb.timer)
    {
        const struct note *up = note_find(nb.id);
        nb.shown_at = lv_tick_get();
        lv_timer_set_period(nb.timer, up && up->kind == NOTE_WARN ? NOTE_WARN_MS : NOTE_SHOW_MS);
        lv_timer_reset(nb.timer);
    }
    const struct note *e = note_find(nb.next);
    if (!e || e->read || qp_is_visible()) { nb.next = 0; return; }
    nbanner_offer(e);
}

/* ------------------------------------------------------------ toasts
 * With the Notification Center's history off (the default), a message is a
 * toast: a little pill that drops in at the top with a bounce, its source's
 * badge hopping as three sparks fly off it, wrapped in a soft glow of the
 * source's colour. A failure is amber and gives a shake. Nothing is kept.
 * The same rules as the banner: a newer toast takes the pill over at once
 * (the same words again count up, an ongoing event's key updates it in
 * place), except over a failure that has not had NOTE_WARN_MIN; then the
 * newest waits in the one place behind it. A tap goes where it leads, or
 * sends it away; a flick up sends it away. Only translation and opacity
 * move, so the R1 never draws a scaled layer for it. */
#define POP_SHOW_MS  2400
#define POP_WARN_MS  4200
#define POP_TEXT_MAX (W - 40 - 10 - 36 - 12 - 20)
static bool notes_on(void) { return global_settings.sloppy_notifications != 0; }
struct pop_msg
{
    char title[NOTE_TITLE], body[NOTE_BODY], key[NOTE_KEY];
    uint8_t kind, src, action;
};
static struct
{
    lv_obj_t *obj, *icon, *glyph, *words, *title, *body, *badge, *spark[3];
    lv_timer_t *timer;
    struct pop_msg cur, next;
    bool up, has_next, flicked;
    uint16_t count;
    uint32_t shown_at;
    lv_point_t press;
    int8_t dir;                          /* +1 at the bottom, -1 at the top (sl_overlay_place) */
} pop;

static void pop_set_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void pop_set_x(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void pop_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
/* a shake that dies away: three swings, 9 px at first */
static void pop_set_shake(void *o, int32_t v)
{
    lv_obj_set_style_translate_x(o, (lv_trigo_sin((int16_t)(v * 1080 / 1024)) * 9 * (1024 - v) / 1024) >> 15, 0);
}
static void pop_anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, int ms, int delay,
                     lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
/* a of the way (of 256) from colour c to white */
static uint32_t pop_lift(uint32_t c, int a)
{
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r += (255 - r) * a >> 8; g += (255 - g) * a >> 8; b += (255 - b) * a >> 8;
    return r << 16 | g << 8 | b;
}
/* the source's colour; the system's own words wear your accent */
static uint32_t pop_color(const struct pop_msg *m)
{
    if (m->kind == NOTE_WARN) return NOTE_WARN_COLOR;
    if (m->src == NS_SYSTEM || m->src == NS_SETTINGS) return C_ACCENT;
    return note_srcs[m->src].color;
}

static void pop_hide(void);
static void pop_timer_cb(lv_timer_t *t) { (void)t; if (!po_is_open()) pop_hide(); }
static void pop_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) { pop.press = p; pop.flicked = false; }
    else if (c == LV_EVENT_PRESSING && !pop.flicked && (p.y - pop.press.y) * pop.dir > 12) { pop.flicked = true; pop_hide(); }   /* flicked off its edge */
    else if (c == LV_EVENT_CLICKED && !pop.flicked)
    {
        int action = pop.cur.action;
        pop_hide();
        if (action) note_open(action);
    }
}

static void pop_build(void)
{
    pop.obj = box(lv_layer_top());
    lv_obj_set_size(pop.obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(pop.obj, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_opa(pop.obj, 248, 0);
    lv_obj_set_style_pad_left(pop.obj, 10, 0);
    lv_obj_set_style_pad_right(pop.obj, 20, 0);
    lv_obj_set_style_pad_ver(pop.obj, 8, 0);
    lv_obj_set_style_min_height(pop.obj, 52, 0);
    sl_elevate(pop.obj, SL_E3, 0);   /* a banner, floating over the page */
    lv_obj_set_flex_flow(pop.obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pop.obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pop.obj, 12, 0);
    lv_obj_add_flag(pop.obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(pop.obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(pop.obj, pop_event, LV_EVENT_ALL, NULL);
    /* the badge: the source's colour, a white glyph, a glint on its top */
    pop.icon = box(pop.obj);
    lv_obj_set_size(pop.icon, 36, 36);
    lv_obj_set_style_radius(pop.icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pop.icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(pop.icon, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(pop.icon, 2, 0);
    lv_obj_set_style_border_color(pop.icon, lv_color_white(), 0);
    lv_obj_set_style_border_opa(pop.icon, 60, 0);
    pop.glyph = lv_label_create(pop.icon);
    lv_obj_set_style_text_font(pop.glyph, &sl_sym_16, 0);
    lv_obj_set_style_text_color(pop.glyph, lv_color_white(), 0);
    lv_obj_center(pop.glyph);
    pop.words = box(pop.obj);
    lv_obj_set_size(pop.words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pop.words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pop.words, 0, 0);
    pop.title = text(pop.words, "", &ss_semibold_18, 0xf7f8fc);
    lv_obj_set_style_text_color(pop.title, lv_color_hex(0xf7f8fc), 0);   /* white in every theme */
    lv_obj_set_style_base_dir(pop.title, LV_BASE_DIR_AUTO, 0);
    lv_label_set_long_mode(pop.title, LV_LABEL_LONG_DOT);
    pop.body = text(pop.words, "", &ss_regular_16, 0xb9bfcc);
    lv_obj_set_style_text_color(pop.body, lv_color_hex(0xb9bfcc), 0);
    lv_obj_set_style_base_dir(pop.body, LV_BASE_DIR_AUTO, 0);
    lv_label_set_long_mode(pop.body, LV_LABEL_LONG_DOT);
    /* x2, x3: the same words again */
    pop.badge = text(pop.obj, "", &ss_semibold_18, 0xffffff);
    lv_obj_set_style_pad_hor(pop.badge, 8, 0);
    lv_obj_set_style_pad_ver(pop.badge, 2, 0);
    lv_obj_set_style_radius(pop.badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pop.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(pop.badge, lv_color_white(), 0);
    lv_obj_add_flag(pop.badge, LV_OBJ_FLAG_HIDDEN);
    /* sparks: they sit on the badge and fly off it when it lands */
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *s = pop.spark[i] = box(pop.obj);
        lv_obj_set_size(s, i == 1 ? 5 : 7, i == 1 ? 5 : 7);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(s, 15, 15);   /* on the badge's centre */
        lv_obj_set_style_opa(s, LV_OPA_TRANSP, 0);
    }
}

/* The theme's pill: candy dark glass with the source's glow, or Zune's
 * square black band with its pink edge */
static void pop_style(const struct pop_msg *m)
{
    uint32_t c = pop_color(m);
    bool zn = sl_zune();
    lv_obj_set_style_radius(pop.obj, zn ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pop.obj, lv_color_hex(zn ? 0x000000 : 0x1b1d26), 0);
    lv_obj_set_style_bg_grad_color(pop.obj, lv_color_hex(zn ? 0x000000 : 0x262935), 0);
    lv_obj_set_style_bg_grad_dir(pop.obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_side(pop.obj, zn ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_width(pop.obj, zn ? 4 : 1, 0);
    lv_obj_set_style_border_color(pop.obj, zn ? lv_color_hex(ZN_PINK) : lv_color_hex(c), 0);
    lv_obj_set_style_border_opa(pop.obj, zn ? LV_OPA_COVER : 110, 0);
    lv_obj_set_style_shadow_color(pop.obj, lv_color_hex(zn ? 0x000000 : c), 0);
    lv_obj_set_style_radius(pop.icon, zn ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pop.icon, lv_color_hex(pop_lift(c, 70)), 0);
    lv_obj_set_style_bg_grad_color(pop.icon, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(pop.badge, lv_color_hex(c), 0);
    static const uint32_t tint[3] = { 0, 0xffffff, 0 };
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(pop.spark[i], lv_color_hex(tint[i] ? tint[i] : pop_lift(c, 40)), 0);
}
static void pop_fill(void)
{
    const struct pop_msg *m = &pop.cur;
    lv_label_set_text(pop.glyph, m->kind == NOTE_WARN ? LV_SYMBOL_WARNING : note_srcs[m->src].glyph);
    lv_point_t t, b = { 0, 0 };
    lv_text_get_size(&t, m->title, &ss_semibold_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (m->body[0]) lv_text_get_size(&b, m->body, &ss_regular_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    char n[12];
    if (pop.count > 1) snprintf(n, sizeof n, "\xc3\x97%u", (unsigned)pop.count);
    else n[0] = '\0';
    int room = POP_TEXT_MAX - (n[0] ? 50 : 0), w = t.x > b.x ? t.x : b.x;
    if (w > room) w = room;
    if (w < 60) w = 60;
    lv_obj_set_width(pop.title, w);
    lv_label_set_text(pop.title, m->title);
    lv_obj_set_width(pop.body, w);
    lv_label_set_text(pop.body, m->body);
    hidden_if_changed(pop.body, !m->body[0]);
    lv_label_set_text(pop.badge, n);
    hidden_if_changed(pop.badge, !n[0]);
    pop_style(m);
}
/* the badge hops and its sparks fly: up, up and left, down and left */
static void pop_sparkle(int delay)
{
    lv_anim_delete(pop.icon, pop_set_y);
    pop_anim(pop.icon, pop_set_y, -9, 0, 420, delay, lv_anim_path_overshoot, NULL);
    static const int8_t to[3][2] = { { 10, -20 }, { -16, -14 }, { -18, 12 } };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *s = pop.spark[i];
        lv_anim_delete(s, NULL);
        lv_obj_set_style_translate_x(s, 0, 0);
        lv_obj_set_style_translate_y(s, 0, 0);
        pop_anim(s, pop_set_x, 0, to[i][0], 520, delay + 60, lv_anim_path_ease_out, NULL);
        pop_anim(s, pop_set_y, 0, to[i][1], 520, delay + 60, lv_anim_path_ease_out, NULL);
        pop_anim(s, pop_set_opa, LV_OPA_COVER, LV_OPA_TRANSP, 520, delay + 60, lv_anim_path_ease_in, NULL);
    }
}
static void pop_show(bool fresh)
{
    if (!pop.obj) pop_build();
    shot_card_hide();
    pop_fill();
    lv_obj_remove_flag(pop.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pop.obj);
    lv_anim_delete(pop.obj, NULL);
    if (fresh)
    {
        pop.dir = (int8_t)sl_overlay_place(pop.obj, 10);   /* never over a control */
        /* comes in from its edge past its place and settles: a bounce, not a slide */
        int from = pop.up ? lv_obj_get_style_translate_y(pop.obj, 0) : 76 * pop.dir;
        pop_anim(pop.obj, pop_set_y, from, 0, 460, 0, lv_anim_path_overshoot, NULL);
        pop_anim(pop.obj, pop_set_opa, pop.up ? lv_obj_get_style_opa(pop.obj, 0) : LV_OPA_TRANSP, LV_OPA_COVER,
                 160, 0, lv_anim_path_ease_out, NULL);
        pop_sparkle(220);
    }
    else
    {
        /* the pill takes the new words with a nod */
        lv_obj_set_style_opa(pop.obj, LV_OPA_COVER, 0);
        pop_anim(pop.obj, pop_set_y, -7 * pop.dir, 0, 320, 0, lv_anim_path_overshoot, NULL);
        pop_sparkle(0);
    }
    if (pop.cur.kind == NOTE_WARN) pop_anim(pop.obj, pop_set_shake, 0, 1024, 480, fresh ? 300 : 40, lv_anim_path_linear, NULL);
    else lv_obj_set_style_translate_x(pop.obj, 0, 0);
    pop.up = true;
    pop.shown_at = lv_tick_get();
    if (!pop.timer) pop.timer = lv_timer_create(pop_timer_cb, POP_SHOW_MS, NULL);
    lv_timer_set_period(pop.timer, pop.cur.kind == NOTE_WARN ? POP_WARN_MS : POP_SHOW_MS);
    lv_timer_reset(pop.timer);
    lv_timer_resume(pop.timer);
}
static void pop_gone(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(pop.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(pop.obj, 0, 0);
    lv_obj_set_style_translate_x(pop.obj, 0, 0);
    if (!pop.has_next || po_is_open()) return;   /* under the power off slider it waits */
    pop.has_next = false;
    pop.cur = pop.next;
    pop.count = 1;
    pop_show(true);
}
static void pop_hide(void)
{
    if (!pop.obj || !pop.up) return;
    pop.up = false;
    if (pop.timer) lv_timer_pause(pop.timer);
    lv_anim_delete(pop.obj, NULL);
    pop_anim(pop.obj, pop_set_y, lv_obj_get_style_translate_y(pop.obj, 0), 76 * pop.dir, 260, 0, lv_anim_path_ease_in, NULL);
    pop_anim(pop.obj, pop_set_opa, lv_obj_get_style_opa(pop.obj, 0), LV_OPA_TRANSP, 260, 0, lv_anim_path_ease_in, pop_gone);
}
/* none up, none waiting: the history was turned on */
static void pop_hide_now(void)
{
    pop.has_next = false;
    pop.up = false;
    if (!pop.obj) return;
    if (pop.timer) lv_timer_pause(pop.timer);
    lv_anim_delete(pop.obj, NULL);
    lv_obj_add_flag(pop.obj, LV_OBJ_FLAG_HIDDEN);
}
static void pop_offer(const struct pop_msg *m)
{
    const struct pop_msg *up = pop.up ? &pop.cur : NULL;
    if (po_is_open())
    {
        if (!pop.has_next || pop.next.kind != NOTE_WARN || m->kind == NOTE_WARN) { pop.next = *m; pop.has_next = true; }
        return;
    }
    bool same_key = up && m->key[0] && !strcmp(up->key, m->key);
    bool again = up && !m->key[0] && up->kind == m->kind && up->src == m->src &&
                 !strcmp(up->title, m->title) && !strcmp(up->body, m->body);
    if (!same_key && !again && up && up->kind == NOTE_WARN && m->kind != NOTE_WARN &&
        lv_tick_elaps(pop.shown_at) < NOTE_WARN_MIN)
    {
        pop.next = *m;
        pop.has_next = true;
        uint32_t left = NOTE_WARN_MIN - lv_tick_elaps(pop.shown_at);
        lv_timer_set_period(pop.timer, left < 300 ? 300 : left);
        lv_timer_reset(pop.timer);
        return;
    }
    if (again) { if (pop.count < 999) pop.count++; }
    else pop.count = 1;
    pop.cur = *m;
    pop_show(!up);
}
/* the power off slider went: what came under it */
static void pop_resume(void)
{
    if (pop.up && pop.timer) { pop.shown_at = lv_tick_get(); lv_timer_reset(pop.timer); }
    if (!pop.has_next) return;
    pop.has_next = false;
    struct pop_msg m = pop.next;
    pop_offer(&m);
}

/* ------------------------------------------------------------ notify */
static void notify_ex(int src, int kind, unsigned flags, int action, const char *key, const char *msg)
{
    if (!msg || !*msg) return;
    notes_load();
    char title[NOTE_TITLE], body[NOTE_BODY];
    note_split(sl_tr(msg), title, body);
    if (!title[0]) return;
    if (!notes_on())
    {
        struct pop_msg m;
        memset(&m, 0, sizeof m);
        strlcpy(m.title, title, sizeof m.title);
        strlcpy(m.body, body, sizeof m.body);
        if (key) strlcpy(m.key, key, sizeof m.key);
        m.kind = (uint8_t)kind; m.src = (uint8_t)src; m.action = (uint8_t)action;
        if (!nh.sound_at || lv_tick_elaps(nh.sound_at) > 400)
        {
            nh.sound_at = lv_tick_get() ? lv_tick_get() : 1;
            sfx_play(SFX_UI_NOTIFY);
        }
        pop_offer(&m);
        return;
    }
    uint32_t now = note_clock();
    struct note *e = NULL;
    int at = -1;
    for (int i = 0; i < nh.n && key && *key; i++)
        if (!strcmp(nh.list[i].key, key)) { at = i; break; }   /* the same ongoing event: brought up to date */
    if (at < 0 && nh.n && !nh.list[0].key[0] && nh.list[0].kind == kind && nh.list[0].src == src &&
        !strcmp(nh.list[0].title, title) && !strcmp(nh.list[0].body, body) && now - nh.list[0].at <= NOTE_REPEAT_S)
    {
        e = &nh.list[0];                 /* the same words again at once: counted */
        if (e->count < 999) e->count++;
    }
    else
    {
        uint32_t keep_id = 0;
        if (at >= 0)
        {
            keep_id = nh.list[at].id;
            memmove(&nh.list[at], &nh.list[at + 1], sizeof nh.list[0] * (size_t)(nh.n - at - 1));
            nh.n--;
        }
        else if (nh.n == NOTE_MAX) nh.n--;   /* the oldest goes */
        memmove(&nh.list[1], &nh.list[0], sizeof nh.list[0] * (size_t)nh.n);
        e = &nh.list[0];
        memset(e, 0, sizeof *e);
        e->id = at >= 0 ? keep_id : ++nh.next_id;
        if (key) strlcpy(e->key, key, sizeof e->key);
        strlcpy(e->title, title, sizeof e->title);
        strlcpy(e->body, body, sizeof e->body);
        e->count = 1;
        nh.n++;
    }
    e->kind = (uint8_t)kind;
    e->src = (uint8_t)src;
    e->action = (uint8_t)action;
    e->priv = (flags & NOTE_PRIVATE) != 0;
    e->read = false;
    e->seen_at = 0;
    e->at = now;
    if (!e->priv) notes_dirty();
    /* one chime for a burst, not one a message */
    if (!nh.sound_at || lv_tick_elaps(nh.sound_at) > 400)
    {
        nh.sound_at = lv_tick_get() ? lv_tick_get() : 1;   /* 0 is never: | 1 would run ahead of the clock */
        sfx_play(SFX_UI_NOTIFY);
    }
    qp_notes_refresh();
    if (qp_is_visible()) return;         /* the Notification Center shows it */
    nbanner_offer(e);
}
static void notify_src_file(const char *file, int kind, unsigned flags, const char *msg)
{
    notify_ex(note_src(file), kind, flags, NA_NONE, NULL, msg);
}
