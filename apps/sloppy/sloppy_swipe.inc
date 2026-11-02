/* Spotify-style song swipes and the Add to playlist sheet. GPLv2+.
 *
 * Drag any song row left to add it to the queue, or right to add it to a
 * playlist. The row rides the finger over a coloured strip that names the
 * action; once it has travelled far enough the icon pops with a tick and
 * letting go commits. Short of that it springs back and nothing happens.
 * Every song list and every row in Up Next works the same way. */

#include "src/core/lv_obj_private.h"   /* row coords: slide without a relayout */

#define SW_CLAIM   12              /* sideways travel that makes a touch a swipe */
#define SW_TRIGGER 118             /* travel that arms the action */
#define SW_STRETCH 90              /* how much further the rubber band gives */
#define SW_FLING   900             /* px/s: a flick commits from 60% of the way */
#define SW_TINTS   12              /* strip colour steps on the way to armed */
#define SW_QUEUE_COLOR    0x1f9d57
#define SW_PLAYLIST_COLOR 0x7357e0

enum { SW_IDLE, SW_TRACK, SW_DRAG, SW_SETTLE };
static int sw_state = SW_IDLE;
static lv_obj_t *sw_row, *sw_reveal, *sw_badge, *sw_icon, *sw_label;
static int sw_x0, sw_y0, sw_track, sw_side;     /* side -1: queue, +1: playlist */
static int sw_offset, sw_tint, sw_vx, sw_last_x;
static uint32_t sw_last_t;
static bool sw_armed, sw_claimed;
static lv_timer_t *sw_watch;
static lv_color_t sw_under;                     /* what the page shows behind the row */
static bool sw_had_color, sw_had_opa;
static lv_style_value_t sw_old_color, sw_old_opa;

static bool sw_busy(void) { return sw_state == SW_DRAG || sw_state == SW_SETTLE; }
static void set_scale(void *o, int32_t v) { lv_obj_set_style_transform_scale(o, v, 0); }

/* the first opaque background behind the row, so it can cover the strip */
static lv_color_t sw_backdrop(lv_obj_t *o)
{
    for (o = lv_obj_get_parent(o); o; o = lv_obj_get_parent(o))
        if (lv_obj_get_style_bg_opa(o, 0) >= LV_OPA_COVER) return lv_obj_get_style_bg_color(o, 0);
    return lv_color_hex(C_BG);
}

static void sw_reveal_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) == sw_reveal) sw_reveal = sw_badge = sw_icon = sw_label = NULL;
}

/* Slide the row to 't' px from home. A translate style would relayout the
 * whole list on every finger move; moving the coords repaints just the
 * strip the row sweeps. The strip marks home, so a relayout that snaps
 * the row back is corrected on the next move. */
static void sw_shift(int t)
{
    lv_obj_t *row = sw_row;
    sw_offset = t;
    if (!row || !sw_reveal) return;
    int diff = sw_reveal->coords.x1 + t - row->coords.x1;
    if (!diff) return;
    lv_area_t dirty = row->coords;
    if (diff < 0) dirty.x1 += diff; else dirty.x2 += diff;
    row->coords.x1 += diff;
    row->coords.x2 += diff;
    lv_obj_move_children_by(row, diff, 0, false);
    lv_obj_invalidate_area(lv_obj_get_parent(row), &dirty);
}
static void sw_anim_cb(void *o, int32_t v) { if (o == sw_row) sw_shift(v); }

/* Put the row back exactly as it was built. */
static void sw_finish(void)
{
    lv_obj_t *row = sw_row;
    bool claimed = sw_claimed;
    if (row && claimed) { lv_anim_delete(row, sw_anim_cb); sw_shift(0); }
    sw_state = SW_IDLE;
    sw_row = NULL;
    sw_claimed = false;
    sw_offset = 0;
    if (sw_watch) lv_timer_pause(sw_watch);
    if (sw_reveal) lv_obj_delete(sw_reveal);
    if (!row || !claimed) return;
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);
    if (sw_had_color) lv_obj_set_local_style_prop(row, LV_STYLE_BG_COLOR, sw_old_color, 0);
    else lv_obj_remove_local_style_prop(row, LV_STYLE_BG_COLOR, 0);
    if (sw_had_opa) lv_obj_set_local_style_prop(row, LV_STYLE_BG_OPA, sw_old_opa, 0);
    else lv_obj_remove_local_style_prop(row, LV_STYLE_BG_OPA, 0);
}
static void sw_settled_cb(lv_anim_t *a) { if (a->var == sw_row) sw_finish(); }

/* The strip deepens as the row nears the trigger; only a new step repaints. */
static void sw_paint(bool force)
{
    if (!sw_reveal) return;
    int a = sw_offset < 0 ? -sw_offset : sw_offset;
    int tint = sw_armed ? SW_TINTS + 1 : LV_MIN(a * SW_TINTS / SW_TRIGGER, SW_TINTS);
    if (tint == sw_tint && !force) return;
    sw_tint = tint;
    lv_color_t full = lv_color_hex(sw_side < 0 ? SW_QUEUE_COLOR : SW_PLAYLIST_COLOR);
    lv_opa_t mix = sw_armed ? LV_OPA_COVER : 70 + tint * 130 / SW_TINTS;
    lv_obj_set_style_bg_color(sw_reveal, lv_color_mix(full, sw_under, mix), 0);
    /* text opacity inherits; a container opacity would need a layer per frame */
    lv_obj_set_style_text_opa(sw_badge, sw_armed ? LV_OPA_COVER : 110 + tint * 100 / SW_TINTS, 0);
}

static void sw_arm(bool armed)
{
    sw_armed = armed;
    sw_paint(true);
    if (armed) anim_prop(sw_icon, set_scale, 190, 300, SL_T_FAST, NULL);
    else
    {
        lv_anim_delete(sw_icon, set_scale);
        lv_obj_set_style_transform_scale(sw_icon, 230, 0);
    }
}

static void sw_set_side(int side)
{
    sw_side = side;
    lv_label_set_text(sw_icon, side < 0 ? LV_SYMBOL_LIST : LV_SYMBOL_PLUS);
    lv_label_set_text(sw_label, sl_tr(side < 0 ? "Queue" : "Playlist"));
    lv_obj_align(sw_badge, side < 0 ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    sw_arm(false);
}

static void sw_release(bool commit);
/* A touch the system took away (screen lock, a reset) never sends a
 * release; don't leave the row hanging half out. */
static void sw_watch_cb(lv_timer_t *t)
{
    (void)t;
    if (sw_state == SW_DRAG && lv_indev_get_state(touch) != LV_INDEV_STATE_PRESSED) sw_release(false);
}

/* The touch has become a swipe: lock out scrolling and lay the strip
 * behind the row, at the same place in its parent. */
static void sw_claim(lv_obj_t *row)
{
    sw_state = SW_DRAG;
    sw_claimed = true;
    sw_armed = false;
    sw_side = 0;
    sw_offset = 0;
    sw_tint = -1;
    lv_obj_remove_state(row, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN);

    sw_under = sw_backdrop(row);
    lv_color_t color = lv_obj_get_style_bg_color(row, 0);
    lv_opa_t opa = lv_obj_get_style_bg_opa(row, 0);
    sw_had_color = lv_obj_get_local_style_prop(row, LV_STYLE_BG_COLOR, &sw_old_color, 0) == LV_STYLE_RES_FOUND;
    sw_had_opa = lv_obj_get_local_style_prop(row, LV_STYLE_BG_OPA, &sw_old_opa, 0) == LV_STYLE_RES_FOUND;
    lv_obj_set_style_bg_color(row, lv_color_mix(color, sw_under, opa), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);

    lv_obj_t *parent = lv_obj_get_parent(row);
    sw_reveal = box(parent);
    lv_obj_add_flag(sw_reveal, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(sw_reveal, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_pos(sw_reveal, lv_obj_get_x(row), lv_obj_get_y(row));
    lv_obj_set_size(sw_reveal, lv_obj_get_width(row), lv_obj_get_height(row));
    lv_obj_set_style_radius(sw_reveal, LV_MAX(lv_obj_get_style_radius(row, 0), 14), 0);
    lv_obj_set_style_bg_opa(sw_reveal, LV_OPA_COVER, 0);
    lv_obj_move_to_index(sw_reveal, lv_obj_get_index(row));
    lv_obj_add_event_cb(sw_reveal, sw_reveal_deleted, LV_EVENT_DELETE, NULL);
    /* icon over a caption, pinned to the edge the row uncovers first, so
     * both are in view before the action arms */
    sw_badge = box(sw_reveal);
    lv_obj_set_size(sw_badge, SW_TRIGGER - 6, lv_pct(100));
    lv_obj_set_flex_flow(sw_badge, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sw_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sw_badge, 2, 0);
    sw_icon = lv_label_create(sw_badge);
    lv_obj_set_style_text_font(sw_icon, &sl_sym_28, 0);
    lv_obj_set_style_text_color(sw_icon, lv_color_white(), 0);
    lv_obj_set_style_transform_pivot_x(sw_icon, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(sw_icon, lv_pct(50), 0);
    sw_label = lv_label_create(sw_badge);
    lv_obj_set_style_text_font(sw_label, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(sw_label, lv_color_white(), 0);
    lv_obj_update_layout(sw_reveal);       /* the strip's coords are home from here on */

    if (!sw_watch) sw_watch = lv_timer_create(sw_watch_cb, 60, NULL);
    lv_timer_resume(sw_watch);
}

/* Linear to the trigger, then an ever stiffer band: no hard stop. */
static int sw_rubber(int dx)
{
    int a = dx < 0 ? -dx : dx;
    if (a > SW_TRIGGER)
    {
        int over = a - SW_TRIGGER;
        a = SW_TRIGGER + over * SW_STRETCH / (over + SW_STRETCH);
    }
    return dx < 0 ? -a : a;
}

static void sw_follow(int dx)
{
    int side = dx < 0 ? -1 : dx > 0 ? 1 : sw_side;
    if (side && side != sw_side) sw_set_side(side);
    sw_shift(sw_rubber(dx));
    bool armed = (sw_offset < 0 ? -sw_offset : sw_offset) >= SW_TRIGGER;
    if (armed != sw_armed)
    {
        sw_arm(armed);
        sfx_play_ex(SFX_UI_TICK, armed ? 125 : 90, 70);
    }
    else sw_paint(false);
}

static void pl_open(int track);
static void sw_queue(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    if (!something_playing())
    {
        if (play_list(&t, 1, 0, false)) toast("Nothing was playing, so it's on now");
        return;
    }
    q_add(t);
    if (uq_open) uq_rebuild_soon();
}

/* Let go: spring home, faster from nearer, and run the action if armed. */
static void sw_release(bool commit)
{
    if (sw_state != SW_DRAG) return;
    int side = sw_side, track = sw_track, a = sw_offset < 0 ? -sw_offset : sw_offset;
    sw_state = SW_SETTLE;
    if (sw_watch) lv_timer_pause(sw_watch);
    if (commit)
    {
        if (!sw_armed) sw_arm(true);
        if (sw_icon) lv_label_set_text(sw_icon, LV_SYMBOL_OK);
    }
    anim_prop(sw_row, sw_anim_cb, sw_offset, 0, SL_T_FAST + a / 2 + (commit ? SL_T_TOUCH : 0), sw_settled_cb);
    if (!commit) return;
    char path[MAX_PATH];
    sl_log("swipe %s: %s", side < 0 ? "queue" : "playlist", sl_track_path(track, path, sizeof path));
    if (side < 0) sw_queue(track);
    else pl_open(track);
}

static void sw_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_t *row = lv_event_get_current_target(e);
    if (c == LV_EVENT_DELETE)
    {
        if (row == sw_row) { sw_row = NULL; sw_finish(); }
        return;
    }
    if (c != LV_EVENT_PRESSED && c != LV_EVENT_PRESSING &&
        c != LV_EVENT_RELEASED && c != LV_EVENT_PRESS_LOST) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (c == LV_EVENT_PRESSED)
    {
        if (sw_state != SW_IDLE) sw_finish();     /* cut a spring-back short */
        sw_state = SW_TRACK;
        sw_row = row;
        sw_track = (int)(intptr_t)lv_event_get_user_data(e);
        sw_x0 = p.x;
        sw_y0 = p.y;
        return;
    }
    if (row != sw_row) return;
    uint32_t now = lv_tick_get();
    if (c == LV_EVENT_PRESSING)
    {
        if (sw_state == SW_TRACK)
        {
            int dx = p.x - sw_x0, dy = p.y - sw_y0;
            if (lv_indev_get_scroll_obj(indev) || abs(dy) > SW_CLAIM)
            { sw_state = SW_IDLE; sw_row = NULL; return; }     /* it's a scroll */
            if (abs(dx) < SW_CLAIM || abs(dx) * 2 < abs(dy) * 3) return;
            sw_claim(row);
            sw_x0 = p.x;               /* start from here: no jump */
            sw_last_x = p.x;
            sw_last_t = now;
            sw_vx = 0;
        }
        if (sw_state != SW_DRAG) return;
        uint32_t dt = now - sw_last_t;
        if (dt > 0)
        {
            sw_vx = (sw_vx + (p.x - sw_last_x) * 1000 / (int)dt) / 2;
            sw_last_x = p.x;
            sw_last_t = now;
        }
        sw_follow(p.x - sw_x0);
        return;
    }

    /* released or lost */
    if (sw_state == SW_TRACK) { sw_state = SW_IDLE; sw_row = NULL; return; }
    if (sw_state != SW_DRAG) return;
    /* sw_vx halves on every still frame, so a stop before letting go is no flick */
    int a = sw_offset < 0 ? -sw_offset : sw_offset;
    bool fling = sw_vx * sw_side > SW_FLING && a >= SW_TRIGGER * 3 / 5;
    sw_release(c == LV_EVENT_RELEASED && (sw_armed || fling));
}

/* Make a row swipeable for the given library track. */
static void sw_attach(lv_obj_t *row, int track)
{
    if (track < 0 || track >= sl_lib.n_tracks) return;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_USER_3);   /* its own sideways swipe: no back from mid screen */
    lv_obj_add_event_cb(row, sw_event_cb, LV_EVENT_ALL, (void *)(intptr_t)track);
}

/* ============================================= Add to playlist sheet */
#define PL_FIRST     40                   /* rows made as the sheet opens; the rest follow a few a tick */
#define PL_NAME_MAX  48
#define PL_SCAN_MAX  (2L * 1024 * 1024)   /* stop reading a huge playlist here */

struct pl_entry {
    char path[MAX_PATH], name[PL_NAME_MAX + 8];
    int songs;
    bool has, bom, extm3u, ascii, scanned;
};
static struct pl_entry *pl_entries;
static int pl_count, pl_cap;             /* every playlist there is: no cap */
static lv_obj_t *pl_scrim, *pl_sheet, *pl_input, **pl_subs, *pl_list;
static int pl_rows_made;
static int pl_track = -1, pl_next_scan;
static int *pl_many, pl_nmany;           /* picked songs, added together; pl_track is the first */
/* or files from Files, by path (in the library or not): pl_track is the
 * first the library has, or -1 */
static char *pl_ppool, pl_ptitle[MAX_PATH];
static uint32_t *pl_poff;
static int pl_npaths;
static char pl_replace_path[MAX_PATH], pl_replace_name[PL_NAME_MAX + 1];
static uint32_t pl_scan_from;
static lv_timer_t *pl_scan_timer;
static bool pl_open_now;

static bool pl_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return false;
    return true;
}

/* A playlist line names the song if it is the same path, or a relative
 * path that ends the song's path at a folder boundary. */
static bool pl_same(const char *line, const char *want)
{
    if (!strcmp(line, want)) return true;
    if (line[0] == '/') return false;
    while (line[0] == '.' && line[1] == '/') line += 2;
    size_t n = strlen(line), m = strlen(want);
    return n && n < m && want[m - n - 1] == '/' && !strcmp(want + m - n, line);
}

static void pl_line(struct pl_entry *en, char *s, const char *want, bool first)
{
    if (first && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf)
    { en->bom = true; s += 3; }
    while (*s == ' ' || *s == '\t') s++;
    if (first && !strncmp(s, "#EXTM3U", 7)) en->extm3u = true;
    if (!*s || *s == '#') return;
    en->songs++;
    if (!en->has && want && pl_same(s, want)) en->has = true;
}

/* One pass over the file: song count, whether it holds this song, and how
 * it is encoded, so an append can match it. */
static void pl_scan(struct pl_entry *en, const char *want)
{
    en->songs = 0;
    en->has = en->bom = en->extm3u = false;
    en->scanned = true;
    en->ascii = true;
    int fd = open(en->path, O_RDONLY);
    if (fd < 0) return;
    char buf[512], line[MAX_PATH];
    int len = 0;
    bool first = true, overlong = false;
    long total = 0;
    ssize_t n;
    while (total < PL_SCAN_MAX && (n = read(fd, buf, sizeof buf)) > 0)
    {
        total += n;
        for (ssize_t i = 0; i < n; i++)
        {
            char ch = buf[i];
            if ((unsigned char)ch >= 0x80) en->ascii = false;
            if (ch == '\n' || ch == '\r')
            {
                line[len] = '\0';
                if (!overlong) pl_line(en, line, want, first);
                else en->songs++;
                if (len || overlong) first = false;
                len = 0;
                overlong = false;
                continue;
            }
            if (len < (int)sizeof line - 1) line[len++] = ch;
            else overlong = true;
        }
    }
    if (len && !overlong) { line[len] = '\0'; pl_line(en, line, want, first); }
    close(fd);
}

/* Cut s back to a whole UTF-8 character after a byte-count truncation. */
static void pl_utf8_trim(char *s)
{
    size_t n = strlen(s), k = n;
    while (k && ((unsigned char)s[k - 1] & 0xc0) == 0x80) k--;   /* continuation bytes */
    if (!k) return;
    unsigned char lead = (unsigned char)s[k - 1];
    size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    if (n - (k - 1) < need) s[k - 1] = '\0';
}

static void pl_display_name(char *out, size_t size, const char *file)
{
    strlcpy(out, file, size);
    pl_utf8_trim(out);
    char *dot = strrchr(out, '.');
    if (dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"))) *dot = '\0';
}

static int pl_entry_cmp(const void *a, const void *b)
{
    return sl_natcmp(((const struct pl_entry *)a)->name, ((const struct pl_entry *)b)->name);
}
static void pl_load(void)
{
    pl_count = 0;
    const char *dirpath = sl_playlist_dir();
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct DIRENT *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;
        if (name[0] == '.' || (!sl_suffix(name, ".m3u") && !sl_suffix(name, ".m3u8"))) continue;
        if (pl_count == pl_cap)
        {
            int cap = pl_cap ? pl_cap * 2 : 64;
            struct pl_entry *e = realloc(pl_entries, (size_t)cap * sizeof *e);
            lv_obj_t **subs = e ? realloc(pl_subs, (size_t)cap * sizeof *subs) : NULL;
            if (e) pl_entries = e;
            if (!e || !subs) { toast_warn("Not enough memory to show every playlist"); break; }
            pl_subs = subs;
            pl_cap = cap;
        }
        struct pl_entry *en = &pl_entries[pl_count];
        memset(en, 0, sizeof *en);         /* not scanned yet: no flags */
        int length = snprintf(en->path, sizeof en->path, "%s%s%s", dirpath,
                              dirpath[strlen(dirpath) - 1] == '/' ? "" : "/", name);
        if (length < 0 || length >= (int)sizeof en->path) continue;
        pl_display_name(en->name, sizeof en->name, name);
        pl_count++;
    }
    closedir(dir);
    if (pl_count > 1) qsort(pl_entries, (size_t)pl_count, sizeof *pl_entries, pl_entry_cmp);   /* as the Playlists page has them */
    if (pl_subs) memset(pl_subs, 0, (size_t)pl_cap * sizeof *pl_subs);
    /* reading the files waits for the sheet to slide in (pl_scan_cb) */
    pl_next_scan = 0;
    pl_scan_from = lv_tick_get() + 260;
}

static bool pl_write(int fd, const char *s, size_t n) { return write(fd, s, n) == (ssize_t)n; }

/* The lines one song adds: #EXTINF when the file uses it, then the path. */
static int pl_song_lines(char *out, size_t size, int t, bool extinf)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    int n = 0;
    if (extinf)
        n = snprintf(out, size, "#EXTINF:%ld,%s - %s\n", tr->len > 0 ? tr->len / 1000 : -1,
                     tr->artist ? tr->artist : sl_lib.albums[tr->album].artist, tr->title);
    if (n < 0 || n >= (int)size) n = 0;
    char path[MAX_PATH];
    int m = snprintf(out + n, size - n, "%s\n", sl_track_path(t, path, sizeof path));
    return m < 0 || m >= (int)(size - n) ? -1 : n + m;
}

/* What is being added: one song, the picked songs, or files by path. */
static int pl_items(void) { return pl_npaths ? pl_npaths : pl_nmany > 1 ? pl_nmany : 1; }
/* (a library song's path is made in buf) */
static const char *pl_item_path(int k, char *buf, size_t size)
{
    if (pl_npaths) return pl_ppool + pl_poff[k];
    return sl_track_path(pl_nmany > 1 ? pl_many[k] : pl_track, buf, size);
}
static int pl_item_lines(char *out, size_t size, int k, bool extinf)
{
    if (pl_npaths)
    {
        int t = sl_lib_find_path(pl_ppool + pl_poff[k]);
        if (t >= 0) return pl_song_lines(out, size, t, extinf);
        int m = snprintf(out, size, "%s\n", pl_ppool + pl_poff[k]);   /* not in the library: the path alone */
        return m < 0 || m >= (int)size ? -1 : m;
    }
    return pl_song_lines(out, size, pl_nmany > 1 ? pl_many[k] : pl_track, extinf);
}

/* Copy the playlist to a temporary file with the songs on the end, then
 * swap it in, so a pulled card never leaves half a playlist. A plain .m3u
 * without a byte order mark is read in the legacy codepage; it gains the
 * mark when a song with a non-ASCII name joins, if nothing in it would be
 * misread as a result. One copy for any number of songs. */
static bool pl_append(struct pl_entry *en)
{
    if (!en->scanned) pl_scan(en, NULL);
    bool utf8 = en->bom || sl_suffix(en->path, ".m3u8"), plain = true;
    char path[MAX_PATH];
    for (int k = 0; k < pl_items() && plain; k++) plain = pl_ascii(pl_item_path(k, path, sizeof path));
    bool add_bom = !utf8 && !plain;
    if (add_bom && !en->ascii) { toast("This playlist can't hold that song's name"); return false; }
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", en->path);
    int in = open(en->path, O_RDONLY);
    if (in < 0) { toast_warn("Playlist file is unavailable"); return false; }
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); toast_warn("Could not save the playlist. Check card space."); return false; }
    bool okay = !add_bom || pl_write(out, "\xef\xbb\xbf", 3);
    char buf[512], last = '\n';
    ssize_t n = 0;
    while (okay && (n = read(in, buf, sizeof buf)) > 0)
    {
        okay = pl_write(out, buf, n);
        last = buf[n - 1];
    }
    if (n < 0) okay = false;   /* a read error would rename a truncated copy over the list */
    close(in);
    if (okay && last != '\n') okay = pl_write(out, "\n", 1);
    char lines[MAX_PATH + 200];
    for (int k = 0; okay && k < pl_items(); k++)
    {
        int m = pl_item_lines(lines, sizeof lines, k, en->extm3u);
        okay = m > 0 && pl_write(out, lines, m);
    }
    if (okay) okay = fsync(out) == 0;
    if (close(out) < 0) okay = false;
    if (okay) okay = rename(tmp, en->path) >= 0;
    if (!okay) { remove(tmp); toast_warn("Could not save the playlist. Check card space."); return false; }
    sl_log("playlist add: %d songs, %s first -> %s", pl_items(), pl_item_path(0, path, sizeof path), en->path);
    return true;
}

static bool pl_exists(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    close(fd);
    return true;
}

static bool pl_path_for(char *out, size_t size, const char *name)
{
    const char *dirpath = sl_playlist_dir();
    int n = snprintf(out, size, "%s%s%s.m3u8", dirpath, dirpath[strlen(dirpath) - 1] == '/' ? "" : "/", name);
    return n > 0 && n < (int)size;
}

/* A file name the card accepts: no path or reserved characters, no
 * leading or trailing spaces and dots. */
static void pl_clean_name(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++)
    {
        unsigned char ch = (unsigned char)*in;
        if (ch < 0x20 || strchr("/\\:*?\"<>|", ch)) continue;
        if (ch == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = ch;
    }
    out[n] = '\0';
    pl_utf8_trim(out);          /* a long name must not end mid-character */
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = '\0';
    char *s = out;
    while (*s == '.') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

/* ------------------------------------------------------------ sheet UI */
static void pl_free_items(void)
{
    free(pl_many);
    pl_many = NULL;
    pl_nmany = 0;
    free(pl_ppool);
    free(pl_poff);
    pl_ppool = NULL;
    pl_poff = NULL;
    pl_npaths = 0;
}
static void pl_close(void)
{
    if (!pl_open_now) return;
    pl_open_now = false;
    pl_input = NULL;
    pl_list = NULL;
    if (pl_scan_timer) lv_timer_pause(pl_scan_timer);
    free(pl_entries);           /* rows still hold indexes; pl_count guards them */
    free(pl_subs);
    pl_entries = NULL;
    pl_subs = NULL;
    pl_count = pl_cap = 0;
    anim_prop(pl_sheet, set_ty, lv_obj_get_style_translate_y(pl_sheet, 0), H, SL_T_PAGE, hide_cb);
    anim_prop(pl_scrim, set_opa, 110, 0, SL_T_PAGE, hide_cb);
}
static void pl_scrim_cb(lv_event_t *e) { (void)e; pl_close(); }

static void pl_list_view(void);
/* "Added to Road trip", or how many */
static void pl_added_toast(const char *name)
{
    char b[PL_NAME_MAX + 96];
    if (pl_items() > 1) snprintf(b, sizeof b, sl_tr("Added %d songs to %s"), pl_items(), name);
    else snprintf(b, sizeof b, sl_tr("Added to %s"), name);
    toast(b);
}

static void pl_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= pl_count || !pl_open_now) return;
    struct pl_entry *en = &pl_entries[i];
    if (pl_items() == 1)
    {
        char path[MAX_PATH];
        if (!en->scanned) pl_scan(en, pl_item_path(0, path, sizeof path));
        if (en->has)
        {
            char b[PL_NAME_MAX + 96];
            snprintf(b, sizeof b, sl_tr("Already in %s"), en->name);
            toast(b);
            return;
        }
    }
    if (!pl_append(en)) return;
    pl_added_toast(en->name);
    pl_close();
}

/* a new playlist file with what is being added, written whole and then
 * swapped in (over the old one, when Replace was chosen) */
static bool pl_write_new(const char *path)
{
    mkdir(sl_playlist_dir());
    char tmp[MAX_PATH + 8], lines[MAX_PATH + 220];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool okay = fd >= 0 && pl_write(fd, "#EXTM3U\n", 8);
    for (int k = 0; okay && k < pl_items(); k++)
    {
        int n = pl_item_lines(lines, sizeof lines, k, true);
        okay = n > 0 && pl_write(fd, lines, n);
    }
    if (okay) okay = fsync(fd) == 0;
    if (fd >= 0 && close(fd) < 0) okay = false;
    if (okay) okay = rename(tmp, path) >= 0;
    if (!okay) { remove(tmp); toast_warn("Could not create the playlist. Check card space."); return false; }
    char first[MAX_PATH];
    sl_log("playlist written: %s with %d songs, %s first", path, pl_items(), pl_item_path(0, first, sizeof first));
    return true;
}
static void pl_new_view(void);
static void pl_replace_view(void);
static void pl_create_cb(lv_event_t *e)
{
    (void)e;
    if (!pl_input || !pl_open_now) return;
    char name[PL_NAME_MAX + 1], path[MAX_PATH];
    pl_clean_name(name, sizeof name, sl_ta_text(pl_input));
    if (!name[0]) { toast("Give your playlist a name"); return; }
    if (!pl_path_for(path, sizeof path, name)) { toast("That name is too long"); return; }
    if (pl_exists(path))
    {
        /* taken: replace it, add to it, or pick another name (#83) */
        strlcpy(pl_replace_path, path, sizeof pl_replace_path);
        strlcpy(pl_replace_name, name, sizeof pl_replace_name);
        pl_replace_view();
        return;
    }
    if (!pl_write_new(path)) return;
    pl_added_toast(name);
    pl_close();
}
static void pl_cancel_cb(lv_event_t *e) { (void)e; pl_list_view(); }
static void pl_header(const char *title)
{
    char many[48];
    if (pl_items() > 1 && !strcmp(title, "Add to playlist"))
    {
        snprintf(many, sizeof many, "%s %d %s", sl_tr("Add"), pl_items(), sl_tr_count(pl_items(), "song", "songs"));
        title = many;
    }
    lv_obj_t *hd = box(pl_sheet);
    lv_obj_set_size(hd, lv_pct(100), 84);
    lv_obj_t *c;
    char b[MAX_PATH + 64];
    if (pl_track >= 0)
    {
        const struct sl_track *tr = &sl_lib.tracks[pl_track];
        c = cover(hd, tr->album, SL_ART_ROW, false);
        snprintf(b, sizeof b, "%s · %s", tr->title, tr->artist ? tr->artist : sl_lib.albums[tr->album].artist);
    }
    else
    {
        /* files the library does not have: the folder's tile and its name */
        c = lit_square(hd, 0x6d83ba, SL_ART_ROW);
        lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_center(text(c, LV_SYMBOL_DIRECTORY, &sl_sym_28, 0xfefefe));
        strlcpy(b, pl_ptitle, sizeof b);
    }
    if (pl_npaths && pl_track >= 0 && pl_items() > 1) strlcpy(b, pl_ptitle, sizeof b);   /* a folder: its name, not its first song */
    sl_align(c, LV_ALIGN_LEFT_MID, 6, 0);   /* from the leading edge: mirrored in Hebrew and Arabic */
    lv_obj_t *t1 = text(hd, title, &ss_semibold_23, 0xffffff);
    sl_place(t1, 80, 12);
    lv_obj_set_width(t1, W - 130);
    label_lines(t1, 1);
    lv_obj_t *t2 = text(hd, "", &ss_regular_16, C_SUB);
    lv_label_set_text(t2, b);
    sl_place(t2, 80, 12 + lv_font_get_line_height(&ss_semibold_23) + 4);   /* 44 at Standard; under the title at any size */
    lv_obj_set_width(t2, W - 130);
    label_lines(t2, 1);
    if (!sl_ui_rtl()) lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_LEFT, 0);   /* a Hebrew song starts by its cover too */
    lv_obj_set_height(hd, LV_MAX(84, 12 + lv_font_get_line_height(&ss_semibold_23) + 4 + lv_font_get_line_height(&ss_regular_16) + 12));
    lv_obj_t *sep = box(pl_sheet);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_SUB), 0);   /* the hairline of every panel */
    lv_obj_set_style_bg_opa(sep, 36, 0);
}

static lv_obj_t *pl_row(lv_obj_t *parent, const char *name, const char *sub, bool accent,
                        lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, lv_pct(100), 72);
    lv_obj_set_style_radius(r, SL_R_XL - SL_FLOAT, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_TEXT), LV_STATE_PRESSED);   /* seen on a white sheet too */
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_t *tile = box(r);
    lv_obj_set_size(tile, 52, 52);
    sl_align(tile, LV_ALIGN_LEFT_MID, 8, 0);   /* from the leading edge: mirrored in Hebrew and Arabic */
    lv_obj_set_style_radius(tile, SL_R_SM, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(accent ? SW_PLAYLIST_COLOR : 0x2c4257), 0);
    lv_obj_center(text(tile, accent ? LV_SYMBOL_PLUS : LV_SYMBOL_AUDIO, &sl_sym_24, 0xffffff));
    lv_obj_t *t1 = text(r, name, &ss_semibold_23, 0xffffff);
    sl_place(t1, 76, sub ? 9 : 21);
    lv_obj_set_width(t1, W - 20 - 20 - 90);
    label_lines(t1, 1);
    if (!sl_ui_rtl()) lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_LEFT, 0);   /* a Hebrew name starts by its tile too */
    if (sub)
    {
        lv_obj_t *t2 = text(r, sub, &ss_regular_16, C_SUB);
        sl_place(t2, 76, 42);
        lv_obj_set_width(t2, W - 20 - 20 - 90);
        label_lines(t2, 1);
    }
    on_click(r, cb, (void *)ud);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

static void pl_subtitle(int i)
{
    const struct pl_entry *en = &pl_entries[i];
    char sub[96], n[40];
    if (!en->scanned) strlcpy(sub, "…", sizeof sub);
    else snprintf(sub, sizeof sub, "%s%s%s", en->has ? sl_tr("Already added") : "", en->has ? " \xc2\xb7 " : "",
                  sl_count(n, sizeof n, en->songs, "%s song", "%s songs"));
    lv_label_set_text(pl_subs[i], sub);
    if (en->has) lv_obj_set_style_text_color(pl_subs[i], lv_color_hex(C_ACCENT), 0);
}
/* rows up to `upto`: the first PL_FIRST at once, the rest a few a tick */
static void pl_rows_upto(int upto)
{
    if (!pl_list) return;
    for (; pl_rows_made < pl_count && pl_rows_made < upto; pl_rows_made++)
    {
        int i = pl_rows_made;
        lv_obj_t *r = pl_row(pl_list, "", " ", false, pl_pick_cb, i);
        lv_label_set_text(lv_obj_get_child(r, 1), pl_entries[i].name);   /* a playlist's name: never translated */
        pl_subs[i] = lv_obj_get_child(r, 2);
        pl_subtitle(i);
    }
}

/* One playlist file per tick, so a big collection never stalls the sheet. */
static void pl_scan_cb(lv_timer_t *t)
{
    if (!pl_open_now || !pl_entries || (pl_next_scan >= pl_count && pl_rows_made >= pl_count)) { lv_timer_pause(t); return; }
    if ((int32_t)(lv_tick_get() - pl_scan_from) < 0) return;
    if (!pl_input) pl_rows_upto(pl_rows_made + 10);
    if (pl_next_scan >= pl_count) return;
    int i = pl_next_scan++;
    char path[MAX_PATH];
    if (!pl_entries[i].scanned) pl_scan(&pl_entries[i], pl_items() == 1 ? pl_item_path(0, path, sizeof path) : NULL);
    if (pl_subs[i] && !pl_input) pl_subtitle(i);
}

static void pl_new_cb(lv_event_t *e);
static void pl_list_view(void)
{
    pl_input = NULL;
    lv_obj_clean(pl_sheet);
    if (pl_subs) memset(pl_subs, 0, (size_t)pl_cap * sizeof *pl_subs);
    pl_rows_made = 0;
    pl_header("Add to playlist");
    pl_row(pl_sheet, "New playlist", NULL, true, pl_new_cb, 0);
    pl_list = box(pl_sheet);
    lv_obj_set_width(pl_list, lv_pct(100));
    lv_obj_set_height(pl_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(pl_list, H - 360, 0);
    lv_obj_set_flex_flow(pl_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(pl_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pl_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pl_list, LV_SCROLLBAR_MODE_OFF);
    pl_rows_upto(PL_FIRST);
    if (!pl_count)
    {
        lv_obj_t *hint = text(pl_list, "No playlists yet. Make your first one and this song starts it off.",
                              &ss_regular_18, C_DIM);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_all(hint, 12, 0);
    }
    if (pl_scan_timer && pl_rows_made < pl_count) lv_timer_resume(pl_scan_timer);
}

/* a button on the sheet: the pill of Create */
static lv_obj_t *pl_button(lv_obj_t *parent, const char *label, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 150, 50);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(lv_color_hex(color), 40), LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_21, 0xffffff);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    sl_fit(l);
    lv_obj_center(l);
    on_click(b, cb, NULL);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    return b;
}
static void pl_new_view(void)
{
    lv_obj_clean(pl_sheet);
    pl_list = NULL;
    if (pl_subs) memset(pl_subs, 0, (size_t)pl_cap * sizeof *pl_subs);
    pl_header("New playlist");
    char name[PL_NAME_MAX + 1], path[MAX_PATH];   /* the first free name, in the language */
    strlcpy(name, sl_tr("My playlist"), sizeof name);
    for (int k = 2; k < 100 && pl_path_for(path, sizeof path, name) && pl_exists(path); k++)
        snprintf(name, sizeof name, sl_tr("My playlist %d"), k);
    pl_input = lv_textarea_create(pl_sheet);
    lv_obj_set_size(pl_input, lv_pct(100), 62);
    lv_textarea_set_one_line(pl_input, true);
    lv_textarea_set_max_length(pl_input, PL_NAME_MAX);
    lv_textarea_set_placeholder_text(pl_input, sl_tr("Playlist name"));
    lv_textarea_set_text(pl_input, pl_replace_name[0] ? pl_replace_name : name);
    pl_replace_name[0] = '\0';
    lv_obj_set_style_text_font(pl_input, &ss_semibold_23, 0);
    lv_obj_set_style_text_color(pl_input, lv_color_white(), 0);
    lv_obj_set_style_bg_color(pl_input, lv_color_hex(0x142230), 0);
    lv_obj_set_style_border_color(pl_input, lv_color_hex(SW_PLAYLIST_COLOR), 0);
    lv_obj_set_style_border_width(pl_input, 2, 0);
    lv_obj_set_style_radius(pl_input, SL_R_MD, 0);
    lv_obj_add_state(pl_input, LV_STATE_FOCUSED);

    lv_obj_t *buttons = box(pl_sheet);
    lv_obj_set_size(buttons, lv_pct(100), 56);
    lv_obj_t *cancel = txt_btn(buttons, "Cancel", pl_cancel_cb);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(pl_button(buttons, "Create", SW_PLAYLIST_COLOR, pl_create_cb), LV_ALIGN_RIGHT_MID, 0, 0);

    struct kb *kb = kb_attach(pl_sheet, pl_input, KB_NAME, pl_create_cb, "Create");
    if (kb)   /* a tray inside the sheet, its corners the sheet's less its inset */
    {
        lv_obj_set_style_radius(kb->root, sl_zune() ? 0 : SL_R_MD, 0);
        lv_obj_set_style_border_width(kb->root, 0, 0);
    }
}
static void pl_new_cb(lv_event_t *e) { (void)e; pl_new_view(); }
/* the name is taken: Replace writes these songs over it (whole, then
 * swapped in), Add to it appends them, Back keeps typing */
static void pl_replace_go_cb(lv_event_t *e)
{
    (void)e;
    if (!pl_open_now || !pl_write_new(pl_replace_path)) return;
    char b[PL_NAME_MAX + 64];
    snprintf(b, sizeof b, sl_tr("Replaced %s"), pl_replace_name);
    toast(b);
    pl_replace_name[0] = '\0';
    pl_close();
}
static void pl_replace_add_cb(lv_event_t *e)
{
    (void)e;
    if (!pl_open_now) return;
    struct pl_entry en;
    memset(&en, 0, sizeof en);
    strlcpy(en.path, pl_replace_path, sizeof en.path);
    strlcpy(en.name, pl_replace_name, sizeof en.name);
    char path[MAX_PATH];
    pl_scan(&en, pl_items() == 1 ? pl_item_path(0, path, sizeof path) : NULL);
    if (pl_items() == 1 && en.has)
    {
        char b[PL_NAME_MAX + 96];
        snprintf(b, sizeof b, sl_tr("Already in %s"), en.name);
        toast(b);
        return;
    }
    if (!pl_append(&en)) return;
    pl_added_toast(en.name);
    pl_replace_name[0] = '\0';
    pl_close();
}
static void pl_replace_back_cb(lv_event_t *e) { (void)e; pl_new_view(); }
static void pl_replace_view(void)
{
    lv_obj_clean(pl_sheet);
    pl_input = NULL;
    pl_list = NULL;
    pl_header("Replace it?");
    char b[PL_NAME_MAX + 200];
    snprintf(b, sizeof b, sl_tr("You already have a playlist called %s. Replace what's in it with these songs, or add them to it."),
             pl_replace_name);
    lv_obj_t *t = text(pl_sheet, b, &ss_regular_18, C_TEXT);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(t, 12, 0);
    pl_row(pl_sheet, "Add to it", NULL, true, pl_replace_add_cb, 0);
    lv_obj_t *r = pl_row(pl_sheet, "Replace", NULL, false, pl_replace_go_cb, 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(r, 0), lv_color_hex(0xc8473f), 0);   /* it writes over the old: the red of Delete */
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(r, 0), 0), LV_SYMBOL_REFRESH);
    pl_row(pl_sheet, "Back", NULL, false, pl_replace_back_cb, 0);
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(pl_sheet, -1), 0), 0), LV_SYMBOL_LEFT);
}
static void pl_open(int t);
static void pl_open_now_for(int t);
/* the picked songs, to one playlist: the sheet as for one, then the list kept */
static void pl_open_many(const int *tracks, int n)
{
    if (n <= 0) return;
    pl_free_items();
    if (n > 1 && (pl_many = malloc((size_t)n * sizeof *pl_many)))
    {
        memcpy(pl_many, tracks, (size_t)n * sizeof *pl_many);
        pl_nmany = n;
    }
    pl_open_now_for(tracks[0]);
}
/* files by path (Files), the list taken over: pool and offsets */
static void pl_open_raw(char *pool, uint32_t *off, int n, const char *title)
{
    pl_free_items();
    pl_ppool = pool;
    pl_poff = off;
    pl_npaths = n;
    strlcpy(pl_ptitle, title, sizeof pl_ptitle);
    int t = -1;
    for (int k = 0; k < n && t < 0; k++) t = sl_lib_find_path(pool + off[k]);
    pl_open_now_for(t);
}
static void pl_open(int t)
{
    if (t < 0 || t >= sl_lib.n_tracks) return;
    pl_free_items();
    pl_open_now_for(t);
}
static void pl_open_now_for(int t)
{
    if (t >= sl_lib.n_tracks || (t < 0 && !pl_npaths)) return;
    pl_track = t;
    pl_replace_name[0] = '\0';
    pl_load();
    if (!pl_sheet)
    {
        pl_scrim = box(lv_layer_top());
        lv_obj_set_size(pl_scrim, W, H);
        lv_obj_set_style_bg_color(pl_scrim, lv_color_black(), 0);
        on_click(pl_scrim, pl_scrim_cb, NULL);
        pl_sheet = box(lv_layer_top());
        lv_obj_set_width(pl_sheet, W - 20);
        lv_obj_set_height(pl_sheet, LV_SIZE_CONTENT);
        lv_obj_align(pl_sheet, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_radius(pl_sheet, SL_R_XL, 0);
        lv_obj_set_style_bg_opa(pl_sheet, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(pl_sheet, 10, 0);
        lv_obj_set_style_pad_row(pl_sheet, 6, 0);
        lv_obj_set_flex_flow(pl_sheet, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(pl_sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* swallow taps */
        lv_obj_remove_flag(pl_sheet, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_set_style_bg_color(pl_sheet, lv_color_hex(C_SHEET), 0);   /* the theme as it is now */
    pl_open_now = true;
    if (!pl_scan_timer) pl_scan_timer = lv_timer_create(pl_scan_cb, 15, NULL);
    pl_list_view();
    lv_timer_resume(pl_scan_timer);
    lv_obj_move_foreground(pl_scrim);
    lv_obj_move_foreground(pl_sheet);
    lv_obj_remove_flag(pl_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(pl_sheet, LV_OBJ_FLAG_HIDDEN);
    anim_prop(pl_scrim, set_opa, 0, 110, SL_T_PAGE, NULL);
    anim_prop(pl_sheet, set_ty, 520, 0, SL_T_PAGE, NULL);
}

static bool pl_visible(void)
{
    return pl_open_now || (pl_sheet && !lv_obj_has_flag(pl_sheet, LV_OBJ_FLAG_HIDDEN));
}
