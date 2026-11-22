/*
 *  Tiles home: a Windows Phone style start screen (Customization > Home
 *  layout > Tiles).
 *
 *  Flat, square, accent-coloured live tiles, big light lowercase type, a
 *  turnstile entrance, a tilt on press and live tiles that peek at their
 *  back face now and then. The layout is yours, as on the phone: press and
 *  hold a tile to edit. The chosen tile can then be unpinned, resized
 *  (medium, small, wide), have its live face turned off, or be dragged to
 *  a new place; the spectrum analyzer's resize control cycles its modes
 *  instead; the "+" tile pins more. Tiles pack into a four-column grid
 *  in order, filling gaps, and the layout is saved in tiles.txt. Below the
 *  tiles is the "apps" list; press and hold an app to pin it.
 */

static bool sl_home_tiles(void) { return global_settings.sloppy_home_style == 1; }

#define MT_M     16                       /* screen margin */
#define MT_G     10                       /* gap between tiles */
#define MT_MED   ((W - 2 * MT_M - MT_G) / 2)   /* 219: matches SL_ART_GRID */
#define MT_WIDE  (W - 2 * MT_M)
#define MT_SMALL ((MT_MED - MT_G) / 2)
#define MT_PITCH4 (MT_WIDE + MT_G)       /* four grid units, with their gaps */
#define MT_TOP   10
#define MT_FILE  ROCKBOX_DIR "/sloppy/tiles.txt"

enum { SZ_SMALL, SZ_MED, SZ_WIDE };
enum { TL_MUSIC, TL_CLOCK, TL_PLAYLISTS, TL_PLAYROOM, TL_BLUETOOTH, TL_UPNEXT, TL_SETTINGS, TL_PET,
       TL_COVERFLOW, TL_ALBUMS, TL_ARTISTS, TL_SONGS, TL_SHUFFLE, TL_RADIO, TL_SNAKE, TL_CHESS,
       TL_QUIZ, TL_RAGDOLL, TL_SPOON, TL_COIN, TL_MAGIC,
       /* added later: new ids go last so saved layouts keep their meaning */
       TL_NOWPLAYING, TL_FILES, TL_LIBRARY, TL_EQ, TL_PLAYBACK, TL_CUSTOMIZE, TL_LOCK,
       TL_GABAGOOL, TL_SPECTRUM, TL_CLICKER, TL_COUNT };
/* tile actions that are pages rather than ACT_ codes */
enum { MT_OPEN_NP = 0x30001, MT_OPEN_LOCK, MT_OPEN_EQ, MT_OPEN_PLAYBACK, MT_OPEN_CUSTOMIZE };
static void spa_open(void);
static const struct { const char *name, *sym; int action; bool live; uint8_t icon; } mt_defs[TL_COUNT] = {
    [TL_MUSIC]     = { "Music", LV_SYMBOL_AUDIO, ACT_MUSIC, true, IC_NONE },
    [TL_CLOCK]     = { "Clock", LV_SYMBOL_BELL, 0, true, IC_CLOCK },
    [TL_PLAYLISTS] = { "Playlists", LV_SYMBOL_LIST, ACT_PLAYLISTS, false, IC_PLAYLISTS },
    [TL_PLAYROOM]  = { "Playroom", LV_SYMBOL_PLAY, ACT_GAMES, true, IC_PLAYROOM },
    [TL_BLUETOOTH] = { "Bluetooth", LV_SYMBOL_BLUETOOTH, ACT_BLUETOOTH, false, IC_NONE },
    [TL_UPNEXT]    = { "Up next", LV_SYMBOL_NEXT, ACT_NOW, true, IC_UPNEXT },
    [TL_SETTINGS]  = { "Settings", LV_SYMBOL_SETTINGS, ACT_SYSTEM, false, IC_NONE },
    [TL_PET]       = { "Tamagotchi", LV_SYMBOL_EYE_OPEN, ACT_PET, true, IC_PET },
    [TL_COVERFLOW] = { "Cover Flow", LV_SYMBOL_VIDEO, ACT_COVERFLOW, true, IC_COVERFLOW },
    [TL_ALBUMS]    = { "Albums", LV_SYMBOL_IMAGE, ACT_ALBUMS, true, IC_ALBUMS },
    [TL_ARTISTS]   = { "Artists", LV_SYMBOL_HOME, ACT_ARTISTS, true, IC_ARTISTS },
    [TL_SONGS]     = { "All songs", LV_SYMBOL_AUDIO, ACT_ALL, true, IC_NONE },
    [TL_SHUFFLE]   = { "Shuffle all", LV_SYMBOL_SHUFFLE, ACT_SHUFFLE, true, IC_NONE },
    [TL_RADIO]     = { "Sloppy FM", LV_SYMBOL_AUDIO, ACT_RADIO, true, IC_RADIO },
    [TL_SNAKE]     = { "Snake", LV_SYMBOL_RIGHT, ACT_SNAKE, false, IC_SNAKE },
    [TL_CHESS]     = { "Chess", LV_SYMBOL_GPS, ACT_CHESS, false, IC_CHESS },
    [TL_QUIZ]      = { "Music Bip Bop Smarty", LV_SYMBOL_AUDIO, ACT_QUIZ, true, IC_QUIZ },
    [TL_RAGDOLL]   = { "Ragdoll", LV_SYMBOL_PLAY, ACT_RAGDOLL, false, IC_RAGDOLL },
    [TL_SPOON]     = { "Freaky Spoon", LV_SYMBOL_UP, ACT_SPOON, false, IC_SPOON },
    [TL_COIN]      = { "Flip a Coin", LV_SYMBOL_REFRESH, ACT_COIN, true, IC_COIN },
    [TL_MAGIC]     = { "Magic 8 Ball", LV_SYMBOL_EYE_OPEN, ACT_MAGIC, true, IC_MAGIC },
    [TL_NOWPLAYING]= { "Now playing", LV_SYMBOL_PLAY, MT_OPEN_NP, true, IC_NONE },
    [TL_FILES]     = { "Files", LV_SYMBOL_DIRECTORY, ACT_FILES, false, IC_FILES },
    [TL_LIBRARY]   = { "Library", LV_SYMBOL_DRIVE, ACT_LIBRARY, true, IC_LIBRARY },
    [TL_EQ]        = { "Equalizer", SL_SYM_EQ, MT_OPEN_EQ, false, IC_NONE },
    [TL_PLAYBACK]  = { "Playback", LV_SYMBOL_LOOP, MT_OPEN_PLAYBACK, false, IC_NONE },
    [TL_CUSTOMIZE] = { "Customize", LV_SYMBOL_EDIT, MT_OPEN_CUSTOMIZE, false, IC_CUSTOMIZE },
    [TL_LOCK]      = { "Lock", LV_SYMBOL_EYE_CLOSE, MT_OPEN_LOCK, false, IC_LOCK },
    [TL_GABAGOOL]  = { "Gabagool", LV_SYMBOL_PLAY, 0, false, IC_GABAGOOL },
    [TL_SPECTRUM]  = { "Spectrum", LV_SYMBOL_BARS, ACT_SPECTRUM, true, IC_SPECTRUM },
    [TL_CLICKER]   = { "Clicker", LV_SYMBOL_PLUS, ACT_CLICKER, true, IC_CLICKER },
};
/* the "+" picker, grouped */
static const uint8_t mt_group_music[] = { TL_MUSIC, TL_NOWPLAYING, TL_UPNEXT, TL_PLAYLISTS, TL_COVERFLOW,
    TL_ALBUMS, TL_ARTISTS, TL_SONGS, TL_SHUFFLE, TL_FILES, TL_LIBRARY, TL_SPECTRUM };
static const uint8_t mt_group_apps[] = { TL_PLAYROOM, TL_PET, TL_CHESS, TL_COIN, TL_SPOON, TL_MAGIC,
    TL_QUIZ, TL_RAGDOLL, TL_SNAKE, TL_RADIO, TL_CLICKER, TL_GABAGOOL };
static const uint8_t mt_group_system[] = { TL_CLOCK, TL_BLUETOOTH, TL_SETTINGS, TL_EQ, TL_PLAYBACK,
    TL_CUSTOMIZE, TL_LOCK };

static void mt_open(int act)
{
    switch (act)
    {
        case MT_OPEN_NP:
            if (something_playing()) np_show(); else toast("Nothing is playing");
            break;
        case MT_OPEN_LOCK: sl_power_lock("the Lock tile"); break;
        case MT_OPEN_EQ: eq_page(); break;
        case MT_OPEN_PLAYBACK: play_settings_page(); break;
        case MT_OPEN_CUSTOMIZE: appearance_page(); break;
        default: action(act); break;
    }
}
/* the apps list: the same apps as the cards home, alphabetical */
static const uint8_t mt_apps[] = { TL_CHESS, TL_CLICKER, TL_COVERFLOW, TL_FILES, TL_COIN, TL_SPOON, TL_MAGIC,
                                   TL_QUIZ, TL_RAGDOLL, TL_RADIO, TL_SNAKE, TL_PET };

/* back: a live tile's back face, or the cover mosaic of a tile that swaps
 * covers in place instead of peeking. live: bit 0 is the live face; the
 * bits above it are the tile's own mode (the spectrum analyzer's). */
struct mt_slot { uint8_t id, size, live; lv_obj_t *obj, *faces, *back; int r, c, down_at; };

static struct
{
    lv_obj_t *page, *body, *grid, *music_front;
    lv_obj_t *clock_time, *clock_day, *clock_date, *clock_batt, *play_back;
    lv_obj_t *queue_badge, *lib_count, *lib_line, *pet_canvas;
    lv_obj_t *add_tile, *ctl_unpin, *ctl_size, *ctl_live, *done;
    lv_timer_t *timer, *pet_timer;
    struct mt_slot slot[TL_COUNT];
    int n, sel, rows;
    bool edit, dragging;
    lv_point_t press;
    int drag_x0, drag_y0, drag_sy0;       /* where the dragged tile was, and the page's scroll */
    int drag_slot;                        /* it follows the finger: no glide (-1: none) */
    int drag_from;                        /* where it was picked up from, to save only a real move */
    uint32_t drag_moved_at;               /* tiles last made room for it */
    char music_path[MAX_PATH];
    int last_min, ticks, peek_turn, game_ix;
    int peek_next;                         /* 1 + the slot whose back was made for the next peek, 0 none */
    int cf_pos, cf_flow_at;                /* Cover Flow tile: centre album, albums_by_title order */
    uint32_t seed;
    bool had_albums;                      /* the library had loaded when the tiles were filled */
    bool music_state;                     /* something was playing at the last build */
    int np_state;                         /* Now playing tile: 0 idle, 1 playing, 2 paused */
    int pet_x, pet_dir;
    unsigned pet_frame;
    uint32_t ink;                         /* type colour on a tile */
    int upnext_key;                       /* the queue the Up next widget shows */
    char pl_path[3][MAX_PATH];            /* the Playlists widget's rows, and where they are */
    int pl_y[3], pl_rows;
} mt;

/* Tile colour: the accent made vivid (WP accents were saturated), then
 * deepened a little so white type reads on it. A light accent such as
 * yellow is not dragged down to mustard: it stays bright and takes dark
 * type instead. Greys stay grey. Sets mt.ink to match. */
/* The gear on the home's Settings icon: on a Tile, the tile's ink as its
 * other glyphs (white was 1.8:1 on a light accent); on a Cards square,
 * white in every theme, as the squares' other icons */
static void home_settings_ink(lv_obj_t *glyph)
{
    if (sl_home_tiles()) lv_obj_set_style_text_color(glyph, lv_color_hex(mt.ink), 0);
    else if (global_settings.sloppy_home_style == 0) lv_obj_set_style_text_color(glyph, lv_color_white(), 0);
}
static uint32_t mt_tile_color(void)
{
    uint32_t c = C_ACCENT;
    lv_color_hsv_t hsv = lv_color_rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
    if (hsv.s > 15 && hsv.s < 78) hsv.s = 78;
    if (hsv.v > 92) hsv.v = 92;
    lv_color_t bright = lv_color_hsv_to_rgb(hsv.h, hsv.s, hsv.v);
    for (int v = hsv.v; v >= 70 || v == hsv.v; v -= 3)
    {
        lv_color_t rgb = lv_color_hsv_to_rgb(hsv.h, hsv.s, v);
        if (rgb.red * 299 + rgb.green * 587 + rgb.blue * 114 < 137000)
        {
            mt.ink = 0xffffff;
            return (uint32_t)rgb.red << 16 | (uint32_t)rgb.green << 8 | rgb.blue;
        }
    }
    mt.ink = 0x111111;
    return (uint32_t)bright.red << 16 | (uint32_t)bright.green << 8 | bright.blue;
}

/* A tile's own shade of the tile colour: a touch lighter or darker, kept
 * by the tile wherever it goes, so the start screen reads as a mosaic of
 * one colour. Never so light or dark that its type (mt.ink) loses out. */
static uint32_t mt_tile_shade(int id)
{
    static const int8_t dv[12] = { 0, -7, 5, -3, 8, -5, 2, -9, 6, -2, 4, -6 };
    uint32_t c = mt_tile_color();
    lv_color_hsv_t hsv = lv_color_rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
    int v = LV_CLAMP(10, hsv.v + dv[(id * 7) % 12], 100);
    lv_color_t rgb = lv_color_hsv_to_rgb(hsv.h, hsv.s, v);
    bool dark = rgb.red * 299 + rgb.green * 587 + rgb.blue * 114 < 137000;
    if (dark != (mt.ink == 0xffffff)) return c;
    return (uint32_t)rgb.red << 16 | (uint32_t)rgb.green << 8 | rgb.blue;
}

/* Type straight on the page: white over the photo wallpapers, which are
 * all dark, whatever the theme. */
static uint32_t mt_page_ink(void) { return wallpaper_id(false) >= 5 ? 0xffffff : C_TEXT; }

static lv_obj_t *mt_label(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_opa_t opa)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(mt.ink), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    return l;
}

static void mt_text(lv_obj_t *o, const char *value)
{
    if (o && strcmp(lv_label_get_text(o), value)) lv_label_set_text(o, value);
}

/* ------------------------------------------------------------ geometry */
static int mt_span_w(int size) { return size == SZ_WIDE ? 4 : size == SZ_MED ? 2 : 1; }
static int mt_span_h(int size) { return size == SZ_SMALL ? 1 : 2; }
static int mt_px(int units) { return units * MT_PITCH4 / 4 - MT_G; }   /* 104, 219, 448 */
static int mt_x(int c) { return MT_M + c * MT_PITCH4 / 4; }
static int mt_y(int r) { return MT_TOP + r * MT_PITCH4 / 4; }
static int mt_find(int id)
{
    for (int i = 0; i < mt.n; i++) if (mt.slot[i].id == id) return i;
    return -1;
}

/* First fit, in order: how the phone packs tiles, except that a tile is
 * never placed above the one before it. Filling any earlier gap made a
 * tile resized to small jump up the screen to the first hole it fitted;
 * now it stays where it was, and later tiles close up behind it. Returns
 * the rows used; in edit mode the "+" tile takes the next cell. */
static int mt_place(int *add_r, int *add_c)
{
    uint8_t occ[2 * TL_COUNT + 2][4];
    memset(occ, 0, sizeof occ);
    int rows = 0, from = 0;
    for (int i = 0; i <= mt.n; i++)
    {
        bool add = i == mt.n;
        if (add && !mt.edit) break;
        int w = add ? 1 : mt_span_w(mt.slot[i].size), h = add ? 1 : mt_span_h(mt.slot[i].size);
        for (int r = from; r + h <= (int)(sizeof occ / sizeof occ[0]); r++)
        {
            int c = 0;
            for (; c + w <= 4; c++)
            {
                bool free = true;
                for (int y = r; y < r + h && free; y++)
                    for (int x = c; x < c + w && free; x++)
                        if (occ[y][x]) free = false;
                if (free) break;
            }
            if (c + w > 4) continue;
            for (int y = r; y < r + h; y++)
                for (int x = c; x < c + w; x++) occ[y][x] = 1;
            if (add) { *add_r = r; *add_c = c; }
            else { mt.slot[i].r = r; mt.slot[i].c = c; }
            if (r + h > rows) rows = r + h;
            from = r;
            break;
        }
    }
    return rows;
}

/* ------------------------------------------------------------- saving */
static void mt_default_layout(void)
{
    /* No gaps: music and its covers on top, then time and quick ones,
     * then play. Small tiles come in fours to fill a medium's square. */
    static const uint8_t def[][2] = {
        { TL_MUSIC, SZ_WIDE },
        { TL_COVERFLOW, SZ_MED }, { TL_ALBUMS, SZ_MED },
        { TL_CLOCK, SZ_MED }, { TL_UPNEXT, SZ_SMALL }, { TL_SETTINGS, SZ_SMALL },
        { TL_PLAYLISTS, SZ_SMALL }, { TL_BLUETOOTH, SZ_SMALL },
        { TL_PET, SZ_MED }, { TL_PLAYROOM, SZ_MED },
        { TL_QUIZ, SZ_MED }, { TL_SHUFFLE, SZ_SMALL }, { TL_ARTISTS, SZ_SMALL },
        { TL_COIN, SZ_SMALL }, { TL_MAGIC, SZ_SMALL },
    };
    mt.n = 0;
    for (unsigned i = 0; i < sizeof def / sizeof def[0]; i++)
        mt.slot[mt.n++] = (struct mt_slot){ .id = def[i][0], .size = def[i][1], .live = 1 };
}
static void mt_save(void)
{
    char b[16 + TL_COUNT * 12];
    int len = snprintf(b, sizeof b, "SLT1\n");
    for (int i = 0; i < mt.n; i++)
        len += snprintf(b + len, sizeof b - len, "%d %d %d\n", mt.slot[i].id, mt.slot[i].size, mt.slot[i].live);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(MT_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;   /* a power cut must not leave an empty file */
    if (close(fd) < 0) ok = false;
    if (!ok || rename(MT_FILE ".tmp", MT_FILE) < 0) remove(MT_FILE ".tmp");
}
static void mt_load(void)
{
    char b[16 + TL_COUNT * 12 + 1];
    int fd = open(MT_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 5 || strncmp(b, "SLT1\n", 5)) { mt_default_layout(); return; }
    b[got] = 0;
    mt.n = 0;
    bool seen[TL_COUNT] = { false };
    for (char *p = b + 5; *p && mt.n < TL_COUNT; )
    {
        char *end;
        long id = strtol(p, &end, 10), size, live;
        if (end == p) break;
        size = strtol(p = end, &end, 10);
        live = strtol(p = end, &end, 10);
        p = end;
        while (*p == '\n' || *p == ' ') p++;
        if (id < 0 || id >= TL_COUNT || seen[id] || size < SZ_SMALL || size > SZ_WIDE) continue;
        seen[id] = true;
        mt.slot[mt.n++] = (struct mt_slot){ .id = (uint8_t)id, .size = (uint8_t)size, .live = (uint8_t)(live & 7) };
    }
}

/* ------------------------------------------------------------ motion */
static void mt_set_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void mt_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void mt_set_face(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void mt_set_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void mt_set_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

/* Turnstile: tiles swing in from the right, row by row, like a WP page load. */
static void mt_turnstile(lv_obj_t *o, unsigned delay)
{
    lv_obj_set_style_translate_x(o, 90, 0);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, 90, 0);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_delay(&a, home_intro_ms + delay);
    motion_decel(&a);
    lv_anim_set_exec_cb(&a, mt_set_tx);
    lv_anim_start(&a);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, mt_set_opa);
    lv_anim_start(&a);
}

/* Slide a live tile's face strip between its front (0) and back (-h).
 * The hold between is counted in ticks, not held by an animation: the main
 * loop only sleeps while no animation is running. */
static void mt_slide_faces(lv_obj_t *faces, int from, int to)
{
    if (!faces) return;
    lv_anim_delete(faces, mt_set_face);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, faces);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, mt_set_face);
    lv_anim_start(&a);
}
static void mt_peek(lv_obj_t *faces, int h) { mt_slide_faces(faces, 0, -h); }
static void mt_glide(lv_obj_t *o, lv_anim_exec_xcb_t cb, int from, int to)
{
    lv_anim_delete(o, cb);
    if (from == to) { cb(o, to); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}

/* ------------------------------------------------------- tile content */
static lv_obj_t *mt_title(lv_obj_t *tile, const char *s)
{
    lv_obj_t *l = mt_label(tile, sl_tr(s), &ss_regular_18, LV_OPA_COVER);   /* a tile's name, in the chosen language */
    int w = lv_obj_get_style_width(tile, LV_PART_MAIN);
    if (w > 20 && !LV_COORD_IS_SPEC(w)) { lv_obj_set_width(l, w - 20); label_lines(l, 1); }   /* inside its tile at any text size */
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    return l;
}
/* A tile's own icon: drawn when it has one, else its font symbol. */
static lv_obj_t *mt_glyph(lv_obj_t *tile, const char *sym, int size);
static lv_obj_t *mt_icon(lv_obj_t *tile, int id, int size)
{
    if (!mt_defs[id].icon) return mt_glyph(tile, mt_defs[id].sym, size);
    lv_obj_t *ic = sl_icon(tile, mt_defs[id].icon, size == SZ_SMALL ? 56 : 72, mt.ink, mt_tile_shade(id));
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, size == SZ_SMALL ? 0 : -12);
    return ic;
}
/* A small icon on its own tile-coloured square, for lists. */
static lv_obj_t *gb_photo(lv_obj_t *parent, int size);
static void mt_list_icon(lv_obj_t *square, int id)
{
    if (id == TL_GABAGOOL) lv_obj_center(gb_photo(square, 58));   /* his photo, as on the tile */
    else if (mt_defs[id].icon) lv_obj_center(sl_icon(square, mt_defs[id].icon, 36, mt.ink, mt_tile_color()));
    else lv_obj_center(mt_label(square, mt_defs[id].sym, &sl_sym_24, LV_OPA_COVER));
}
static lv_obj_t *mt_glyph(lv_obj_t *tile, const char *sym, int size)
{
    lv_obj_t *l = mt_label(tile, sym, size == SZ_SMALL ? &sl_sym_40 : &sl_sym_48,
                           LV_OPA_COVER);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, size == SZ_SMALL ? 0 : -12);
    return l;
}
/* A live tile: a strip twice the tile's height, front face on top. */
static lv_obj_t *mt_faces(struct mt_slot *s, lv_obj_t **front, lv_obj_t **back)
{
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_obj_t *f = box(s->obj);
    lv_obj_set_size(f, w, h * 2);
    *front = box(f);
    lv_obj_set_size(*front, w, h);
    *back = box(f);
    lv_obj_set_size(*back, w, h);
    lv_obj_set_y(*back, h);
    s->faces = f;
    return f;
}
static unsigned mt_rand(void)
{
    mt.seed = mt.seed * 1103515245u + 12345u;
    return mt.seed >> 8;
}
/* A random album, preferring one whose cover has been made and that is not
 * in avoid[]. -1 with an empty library. */
static int mt_pick_album(const int *avoid, int n_avoid)
{
    if (!sl_lib.n_albums) return -1;
    int a = -1;
    for (int tries = 0; tries < 8; tries++)
    {
        a = (int)(mt_rand() % (unsigned)sl_lib.n_albums);
        bool dup = false;
        for (int k = 0; k < n_avoid; k++) if (avoid[k] == a) dup = true;
        if (!dup && sl_art_src(a, SL_ART_PAGE)) break;
    }
    return a;
}

static void mt_art_scale_cb(lv_event_t *e)
{
    lv_obj_t *child = lv_event_get_param(e);
    if (child && lv_obj_check_type(child, &lv_image_class))
        lv_image_set_scale(child, (uint32_t)(uintptr_t)lv_event_get_user_data(e));
}
/* An album cover at any size: the nearest thumbnail the art cache makes,
 * scaled. Until it exists, the album's colour and a note. */
static lv_obj_t *mt_art(lv_obj_t *parent, int album, const char *key, int size)
{
    lv_obj_t *art = box(parent);
    lv_obj_set_size(art, size, size);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    unsigned hue = 0;
    for (const char *p = key; p && *p; p++) hue = hue * 31 + (unsigned char)*p;
    lv_obj_set_style_bg_color(art, lv_color_hsv_to_rgb(hue % 360, 45, 38), 0);
    lv_obj_t *note = lv_label_create(art);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, size < 150 ? &sl_sym_32 : &sl_sym_48, 0);
    lv_obj_set_style_text_color(note, lv_color_white(), 0);
    lv_obj_set_style_text_opa(note, 150, 0);
    lv_obj_center(note);
    if (album < 0) return art;
    /* scaled once, so redraws of a live tile just copy pixels; read and
     * scaled beside the UI, so a peek or a swap does not hold it */
    if (sl_art_apply_scaled_async(art, album, size + 2)) return art;
    int thumb = size <= SL_ART_ROW ? SL_ART_ROW : size <= SL_ART_PAGE ? SL_ART_PAGE : SL_ART_GRID;
    /* rounded up and a touch over: a scaled cover never leaves a hairline of tile */
    if (size != thumb)
        lv_obj_add_event_cb(art, mt_art_scale_cb, LV_EVENT_CHILD_CREATED,
                            (void *)(uintptr_t)((256 * size + thumb - 1) / thumb + 2));
    sl_art_apply(art, album, thumb);
    return art;
}
static lv_obj_t *mt_album_art(lv_obj_t *parent, int album, int size)
{
    return mt_art(parent, album, album >= 0 ? sl_lib.albums[album].title : "", size);
}

/* Covers edge to edge: one on a small tile, 2x2 on a medium, 4x2 on a wide.
 * Each cell keeps its album in its user data. */
static void mt_cell_cover(lv_obj_t *cell, int album, int cw, int ch, bool slide)
{
    int side = cw > ch ? cw : ch;
    lv_obj_t *art = mt_album_art(cell, album, side);
    int x = (cw - side) / 2, y = (ch - side) / 2;
    lv_obj_set_pos(art, x, y);
    lv_obj_set_user_data(cell, (void *)(intptr_t)album);
    if (!slide) return;
    /* the new cover drops in over the old one */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, art);
    lv_anim_set_values(&a, y - ch, y);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, mt_set_y);
    lv_anim_start(&a);
}
static lv_obj_t *mt_mosaic(lv_obj_t *parent, int w, int h)
{
    int cols = w > h ? 4 : w > MT_SMALL ? 2 : 1, rows = h > MT_SMALL ? 2 : 1, used[8], n = 0;
    lv_obj_t *m = box(parent);
    lv_obj_set_size(m, w, h);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
        {
            lv_obj_t *cell = box(m);
            int x = c * w / cols, y = r * h / rows, cw = (c + 1) * w / cols - x, ch = (r + 1) * h / rows - y;
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_size(cell, cw, ch);
            used[n] = mt_pick_album(used, n);
            mt_cell_cover(cell, used[n], cw, ch, false);
            n++;
        }
    return m;
}
/* One cell turns to a cover not already showing. */
static void mt_mosaic_swap(lv_obj_t *m)
{
    int n = (int)lv_obj_get_child_count(m), used[8];
    if (n <= 0 || n > 8 || !sl_lib.n_albums) return;
    for (int k = 0; k < n; k++) used[k] = (int)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(m, k));
    lv_obj_t *cell = lv_obj_get_child(m, (int32_t)(mt_rand() % (unsigned)n));
    while (lv_obj_get_child_count(cell) > 1) lv_obj_delete(lv_obj_get_child(cell, 0));
    mt_cell_cover(cell, mt_pick_album(used, n), lv_obj_get_width(cell), lv_obj_get_height(cell), true);
}
/* white type over artwork, on a soft dark band so it always reads */
static void mt_over_art(lv_obj_t *parent, const char *s, int w, int h)
{
    lv_obj_t *band = box(parent);
    lv_obj_set_size(band, w, 64);
    lv_obj_set_y(band, h - 64);
    lv_obj_set_style_bg_color(band, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(band, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(band, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(band, 190, 0);
    lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, 0);
    lv_obj_t *l = lv_label_create(band);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_width(l, w - 20);
    label_lines(l, 1);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 10, -7);
}

/* Music: the song you are hearing, at whatever size the tile is. */
static void mt_music_front(void)
{
    int i = mt_find(TL_MUSIC);
    lv_obj_t *f = mt.music_front;
    if (i < 0 || !f) return;
    int size = mt.slot[i].size, w = mt_px(mt_span_w(size)), h = mt_px(mt_span_h(size));
    lv_obj_clean(f);
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    mt.music_state = id3 != NULL;
    strlcpy(mt.music_path, id3 ? id3->path : "", sizeof mt.music_path);
    if (!id3)
    {
        if (size == SZ_WIDE)
        {
            lv_obj_t *g = mt_label(f, LV_SYMBOL_AUDIO, &sl_sym_48, LV_OPA_COVER);
            lv_obj_align(g, LV_ALIGN_LEFT_MID, 58, -12);
            lv_obj_t *big = mt_label(f, "music", &ss_regular_60, LV_OPA_COVER);
            lv_obj_align(big, LV_ALIGN_LEFT_MID, 176, -18);
            lv_obj_t *hint = mt_label(f, sl_tr("tap to browse, or shuffle it all"), &ss_regular_18, 200);
            int fw = lv_obj_get_style_width(f, LV_PART_MAIN);
            if (fw > 188 && !LV_COORD_IS_SPEC(fw)) { lv_obj_set_width(hint, fw - 188); label_lines(hint, 1); }   /* inside the tile */
            lv_obj_align(hint, LV_ALIGN_LEFT_MID, 178, 30);
        }
        else mt_glyph(f, LV_SYMBOL_AUDIO, size);
        if (size != SZ_SMALL) mt_title(f, "Music");
        return;
    }
    int track = sl_lib_find_path(id3->path);
    int album = track >= 0 ? sl_lib.tracks[track].album : -1;
    char name[64];
    const char *title = track_title(id3, name, sizeof name);
    const char *artist = track_artist(id3);
    if (size == SZ_SMALL) { mt_art(f, album, id3->album ? id3->album : id3->path, w); return; }
    if (size == SZ_MED)
    {
        mt_art(f, album, id3->album ? id3->album : id3->path, w);
        mt_over_art(f, title, w, h);
        return;
    }
    mt_art(f, album, id3->album ? id3->album : id3->path, MT_MED);
    int x = MT_MED + 14, tw = w - x - 12;
    lv_obj_t *now = mt_label(f, sl_tr("NOW PLAYING"), &ss_semibold_18, 190);
    lv_obj_set_pos(now, x, 16);
    lv_obj_t *t = mt_label(f, title, &ss_semibold_23, LV_OPA_COVER);
    lv_obj_set_pos(t, x, 44);
    lv_obj_set_width(t, tw);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    label_lines(t, 2);
    lv_obj_t *ar = mt_label(f, artist, &ss_regular_18, 215);
    lv_obj_set_pos(ar, x, 44 + 2 * lv_font_get_line_height(&ss_semibold_23) + 6);
    lv_obj_set_width(ar, tw);
    label_lines(ar, 1);
    lv_obj_set_pos(mt_label(f, "Music", &ss_regular_18, LV_OPA_COVER), x, h - 31);
}

static void mt_clock_update(void)
{
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December" };
    struct tm *tm = get_time();
    char b[32];
    int hour = global_settings.timeformat ? (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12) : tm->tm_hour;
    snprintf(b, sizeof b, "%d:%02d", hour, tm->tm_min);
    mt_text(mt.clock_time, b);
    mt_text(mt.clock_day, sl_tr(days[(unsigned)tm->tm_wday % 7]));
    snprintf(b, sizeof b, "%s %d", sl_tr(months[(unsigned)tm->tm_mon % 12]), tm->tm_mday);
    mt_text(mt.clock_date, b);
    snprintf(b, sizeof b, "%d%%", battery_level());
    mt_text(mt.clock_batt, b);
    mt.last_min = tm->tm_min;
}

static void mt_lib_update(void)
{
    char b[64];
    snprintf(b, sizeof b, "%d", sl_lib.n_tracks);
    mt_text(mt.lib_count, b);
    snprintf(b, sizeof b, "song%s  ·  %d album%s  ·  %d artist%s", sl_lib.n_tracks == 1 ? "" : "s",
             sl_lib.n_albums, sl_lib.n_albums == 1 ? "" : "s", sl_lib.n_artists, sl_lib.n_artists == 1 ? "" : "s");
    mt_text(mt.lib_line, b);
}

static void mt_queue_update(void)
{
    if (!mt.queue_badge) return;
    int left = something_playing() ? playlist_amount() - playlist_get_display_index() : 0;
    char b[12];
    snprintf(b, sizeof b, "%d", left > 0 ? left : 0);
    mt_text(mt.queue_badge, b);
    if (left > 0) lv_obj_remove_flag(mt.queue_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(mt.queue_badge, LV_OBJ_FLAG_HIDDEN);
}

static const char *const mt_games[] = { "snake", "chess", "ragdoll", "freaky spoon", "magic 8 ball",
                                        "flip a coin", "sloppy fm", "bip bop smarty" };

/* A back face about one album. A wide tile has the cover on the left and
 * lines beside it; a square one is all cover, with the first line over it. */
static void mt_cover_back(struct mt_slot *s, int album, const char *kicker, const char *line1, const char *line2)
{
    lv_obj_t *b = s->back;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_obj_clean(b);
    if (w <= h)
    {
        mt_album_art(b, album, w);
        if (s->size != SZ_SMALL) mt_over_art(b, line1, w, h);
        return;
    }
    mt_album_art(b, album, h);
    int x = h + 14, tw = w - x - 12, y = 16;
    if (kicker)
    {
        lv_obj_set_pos(mt_label(b, kicker, &ss_semibold_18, 190), x, y);
        y = 44;
    }
    lv_obj_t *t = mt_label(b, line1, &ss_semibold_23, LV_OPA_COVER);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_width(t, tw);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    label_lines(t, 2);
    lv_obj_t *l2 = mt_label(b, line2, &ss_regular_18, 215);
    lv_obj_set_pos(l2, x, y + 2 * lv_font_get_line_height(&ss_semibold_23) + 6);
    lv_obj_set_width(l2, tw);
    label_lines(l2, 1);
    lv_obj_t *name = mt_label(b, sl_tr(mt_defs[s->id].name), &ss_regular_18, LV_OPA_COVER);
    lv_obj_set_pos(name, x, h - 31);
    lv_obj_set_width(name, tw);
    label_lines(name, 1);
}

/* Music's back face: covers from the library behind its counts. */
static void mt_music_back(struct mt_slot *s)
{
    lv_obj_t *b = s->back;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_obj_clean(b);
    bool art = sl_lib.n_albums > 0;
    if (art)
    {
        mt_mosaic(b, w, h);
        lv_obj_t *scrim = box(b);
        lv_obj_set_size(scrim, w, h);
        lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scrim, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(scrim, lv_color_black(), 0);
        lv_obj_set_style_bg_main_opa(scrim, 70, 0);
        lv_obj_set_style_bg_grad_opa(scrim, 200, 0);
        lv_obj_set_style_bg_grad_dir(scrim, LV_GRAD_DIR_VER, 0);
    }
    mt.lib_count = mt_label(b, "", &ss_regular_60, LV_OPA_COVER);
    lv_obj_set_pos(mt.lib_count, 14, 22);
    mt.lib_line = mt_label(b, "", &ss_regular_18, 215);
    lv_obj_set_pos(mt.lib_line, 18, 104);
    lv_obj_set_width(mt.lib_line, w - 30);
    label_lines(mt.lib_line, 2);
    lv_obj_set_style_text_align(mt.lib_line, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(mt.lib_line, LV_LABEL_LONG_WRAP);
    lv_obj_t *title = mt_title(b, mt_defs[s->id].name);
    if (art)                                /* white on the darkened covers */
    {
        lv_obj_set_style_text_color(mt.lib_count, lv_color_white(), 0);
        lv_obj_set_style_text_color(mt.lib_line, lv_color_white(), 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
    }
    mt_lib_update();
}

/* Cover Flow: albums in title order, as in the app */
static int mt_cf_album(int pos)
{
    int n = sl_lib.n_albums;
    return sl_lib.albums_by_title[((pos % n) + n) % n];
}
static void mt_cf_cover(lv_obj_t *fan, int pos, int size, int x, int y, lv_opa_t dim)
{
    lv_obj_t *art = mt_album_art(fan, mt_cf_album(pos), size);
    lv_obj_set_pos(art, x, y);
    if (!dim)
    {
        /* the centre cover stands forward of the rest: a short, close shadow
         * (a floating sheet's long one read as a smudge on the accent) */
        sl_elevate(art, SL_E3, 0);
        lv_obj_set_style_shadow_width(art, 14, 0);
        lv_obj_set_style_shadow_opa(art, 90, 0);
        lv_obj_set_style_shadow_offset_y(art, 3, 0);
        return;
    }
    lv_obj_t *shade = box(fan);             /* a sibling, so late art stays under it */
    lv_obj_set_size(shade, size, size);
    lv_obj_set_pos(shade, x, y);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shade, dim, 0);
}
/* Cover Flow's front: covers fanned out around the centre one, as in the
 * app. flow slides them one place along, as if flicked. */
static void mt_cf_front(struct mt_slot *s, bool flow)
{
    lv_obj_t *f = s->faces ? lv_obj_get_child(s->faces, 0) : NULL;
    if (!f) return;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_obj_clean(f);
    if (!sl_lib.n_albums || s->size == SZ_SMALL)
    {
        mt_icon(f, s->id, s->size);
        if (s->size != SZ_SMALL) mt_title(f, mt_defs[s->id].name);
        return;
    }
    bool wide = s->size == SZ_WIDE;
    int c = wide ? 168 : 150, near = c * 3 / 4, far = near * 4 / 5, top = 12;
    int cx = (w - c) / 2, nl = cx - near * 55 / 100, nr = cx + c - near * 45 / 100;
    lv_obj_t *fan = box(f);
    lv_obj_set_size(fan, w, h);
    int p = mt.cf_pos;
    if (wide)
    {
        mt_cf_cover(fan, p - 2, far, nl - far * 55 / 100, top + (c - far) / 2, 150);
        mt_cf_cover(fan, p + 2, far, nr + near - far * 45 / 100, top + (c - far) / 2, 150);
    }
    mt_cf_cover(fan, p - 1, near, nl, top + (c - near) / 2, 95);
    mt_cf_cover(fan, p + 1, near, nr, top + (c - near) / 2, 95);
    mt_cf_cover(fan, p, c, cx, top, 0);
    mt_title(f, mt_defs[s->id].name);
    if (!flow) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, fan);
    lv_anim_set_values(&a, nr - cx, 0);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, mt_set_tx);
    lv_anim_start(&a);
}
/* Where the fan starts: an album with a cover, found in title order. */
static int mt_cf_start(void)
{
    int a = mt_pick_album(NULL, 0);
    for (int i = 0; i < sl_lib.n_albums; i++)
        if (sl_lib.albums_by_title[i] == a) return i;
    return 0;
}
/* Its back face: the next album along with a cover. The fan flows on to
 * it once the tile has turned back. */
static bool mt_cf_back(struct mt_slot *s)
{
    if (!sl_lib.n_albums) return false;
    int step = 1;
    while (step < 6 && !sl_art_src(mt_cf_album(mt.cf_pos + step), SL_ART_PAGE)) step++;
    if (step == 6) step = 1;
    mt.cf_pos += step;
    int a = mt_cf_album(mt.cf_pos);
    mt_cover_back(s, a, NULL, sl_lib.albums[a].title, sl_lib.albums[a].artist);
    mt.cf_flow_at = mt.ticks + 5;
    return true;
}

/* Implemented after the games are included in sloppy.c. */
static bool mt_game_back(struct mt_slot *s);
static bool clicker_tile_back(struct mt_slot *s);   /* sloppy_clicker.inc */
static void cf_open_on(int index);

/* Tiles with a back face beyond the first ones. */
static bool mt_has_back(int id, int size)
{
    switch (id)
    {
        case TL_ARTISTS: case TL_SONGS: case TL_SHUFFLE: case TL_UPNEXT: return true;
        case TL_QUIZ: case TL_COIN: case TL_MAGIC: case TL_CLICKER: return true;
        case TL_LIBRARY: return size != SZ_SMALL;
        default: return false;
    }
}

/* Fill a live tile's back face just before it peeks: something new each
 * time. False when it has nothing to show just now. */
static bool mt_back(struct mt_slot *s)
{
    char b[64];
    int a = -1;
    switch (s->id)
    {
    case TL_MUSIC: mt_music_back(s); return true;
    case TL_COVERFLOW: return mt_cf_back(s);
    case TL_PLAYROOM:
        mt.game_ix = (mt.game_ix + 1) % (int)(sizeof mt_games / sizeof mt_games[0]);
        if (mt.play_back) lv_label_set_text(mt.play_back, mt_games[mt.game_ix]);
        return true;
    case TL_ARTISTS:
    {
        if ((a = mt_pick_album(NULL, 0)) < 0) return false;
        const char *artist = sl_lib.albums[a].artist;
        int n = 0;
        for (int k = 0; k < sl_lib.n_albums; k++) if (!strcmp(sl_lib.albums[k].artist, artist)) n++;
        snprintf(b, sizeof b, "%d album%s", n, n == 1 ? "" : "s");
        mt_cover_back(s, a, "ARTIST", artist, b);
        return true;
    }
    case TL_SONGS:
    case TL_SHUFFLE:
    {
        /* a song from an album with a cover, else any song */
        int t = -1;
        if ((a = mt_pick_album(NULL, 0)) >= 0 && sl_lib.albums[a].n_tracks)
            t = sl_lib.albums[a].tracks[mt_rand() % (unsigned)sl_lib.albums[a].n_tracks];
        else if (sl_lib.n_tracks)
            a = sl_lib.tracks[t = (int)(mt_rand() % (unsigned)sl_lib.n_tracks)].album;
        if (t < 0) return false;
        const char *title = sl_lib.tracks[t].title;
        mt_cover_back(s, a, s->id == TL_SHUFFLE ? "MAYBE NEXT" : "SONG", title ? title : "Unknown song",
                      a >= 0 && a < sl_lib.n_albums ? sl_lib.albums[a].artist : "");
        return true;
    }
    case TL_UPNEXT:
    {
        struct mp3entry *next = something_playing() ? audio_next_track() : NULL;
        if (!next) return false;
        int t = sl_lib_find_path(next->path);
        if (t >= 0) a = sl_lib.tracks[t].album;
        char name[64];
        mt_cover_back(s, a, "UP NEXT", track_title(next, name, sizeof name), track_artist(next));
        return true;
    }
    case TL_LIBRARY:
    {
        lv_obj_clean(s->back);
        snprintf(b, sizeof b, "%d", sl_lib.n_albums);
        lv_obj_set_pos(mt_label(s->back, b, &ss_regular_60, LV_OPA_COVER), 14, 22);
        snprintf(b, sizeof b, "album%s  ·  %d song%s  ·  %d artist%s", sl_lib.n_albums == 1 ? "" : "s",
                 sl_lib.n_tracks, sl_lib.n_tracks == 1 ? "" : "s", sl_lib.n_artists, sl_lib.n_artists == 1 ? "" : "s");
        lv_obj_t *l = mt_label(s->back, b, &ss_regular_18, 215);
        lv_obj_set_pos(l, 18, 104);
        lv_obj_set_width(l, mt_px(mt_span_w(s->size)) - 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        label_lines(l, 2);
        mt_title(s->back, mt_defs[s->id].name);
        return true;
    }
    case TL_QUIZ: case TL_COIN: case TL_MAGIC:
        return mt_game_back(s);
    case TL_CLICKER: return clicker_tile_back(s);
    default:
        return true;
    }
}
/* Implemented after the pet renderer is included in sloppy.c. */
static void mt_pet_fill(struct mt_slot *s);
static void mt_pet_tick(lv_timer_t *timer);
/* Implemented in sloppy_spectrum.inc and sloppy_gabagool.inc. */
static void mt_spectrum_fill(struct mt_slot *s);
enum { SP_MODES = 3 };
static const char *const sp_mode_names[SP_MODES];
static int sp_mode_of(const struct mt_slot *s);
static void sp_forget(void);
static void mt_gabagool_fill(struct mt_slot *s);
static void mt_gabagool_tap(struct mt_slot *s);
static void gb_forget(void);
static void gb_clip_free(void);

/* --------------------------------------------------------- widgets
 * Bigger, a tile shows more, the way phone widgets do: small stays the
 * icon it was. */

/* The k-th song after this one: its album (-1 unknown), title and artist;
 * -2 if nothing plays then. */
static int mt_next_info(int k, char *title, size_t n, const char **artist)
{
    struct playlist_track_info info;
    if (k > q_ahead() || playlist_get_track_info(NULL, q_at(k), &info) < 0) return -2;
    int t = sl_lib_find_path(info.filename), a = t >= 0 ? sl_lib.tracks[t].album : -1;
    if (t >= 0 && sl_lib.tracks[t].title) strlcpy(title, sl_lib.tracks[t].title, n);
    else
    {
        const char *b = strrchr(info.filename, '/');
        strlcpy(title, b ? b + 1 : info.filename, n);
        char *dot = strrchr(title, '.');
        if (dot && dot != title) *dot = '\0';
    }
    *artist = a >= 0 && a < sl_lib.n_albums ? sl_lib.albums[a].artist : "";
    return a;
}
static int mt_upnext_now(void) { return something_playing() ? q_cur() * 4096 + playlist_amount() : -1; }

/* Up next: medium, the next song with its cover and how many are to come;
 * wide, the next three as a little queue. False when nothing is next. */
static bool mt_upnext_fill(struct mt_slot *s)
{
    char title[96];
    const char *artist;
    mt.upnext_key = mt_upnext_now();
    int a = mt.upnext_key >= 0 ? mt_next_info(1, title, sizeof title, &artist) : -2;
    if (a == -2) return false;
    lv_obj_t *t = s->obj;
    int w = mt_px(mt_span_w(s->size)), ahead = q_ahead();
    char b[24];
    if (s->size == SZ_MED)
    {
        lv_obj_set_pos(mt_album_art(t, a, 104), 14, 14);
        lv_obj_set_pos(mt_label(t, "UP NEXT", &ss_semibold_18, 190), 130, 16);
        snprintf(b, sizeof b, "%d", ahead);
        lv_obj_set_pos(mt_label(t, b, &ss_regular_34, LV_OPA_COVER), 128, 40);
        lv_obj_set_pos(mt_label(t, sl_tr_count(ahead, "song", "songs"), &ss_regular_16, 200), 131, 84);
        lv_obj_t *tl = mt_label(t, title, &ss_semibold_21, LV_OPA_COVER);
        lv_obj_set_pos(tl, 14, 140);
        lv_obj_set_width(tl, w - 28);
        label_lines(tl, 1);
        lv_obj_t *ar = mt_label(t, artist, &ss_regular_18, 215);
        lv_obj_set_pos(ar, 14, 168);
        lv_obj_set_width(ar, w - 28);
        label_lines(ar, 1);
        return true;                     /* its heading says what it is */
    }
    lv_obj_set_pos(mt_label(t, sl_tr(mt_defs[s->id].name), &ss_semibold_21, LV_OPA_COVER), 14, 10);
    snprintf(b, sizeof b, ahead == 1 ? "1 song" : "%d songs", ahead);
    lv_obj_t *count = mt_label(t, b, &ss_regular_18, 200);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -14, 13);
    for (int k = 1; k <= 3; k++)
    {
        if (k > 1 && (a = mt_next_info(k, title, sizeof title, &artist)) == -2) break;
        int y = 44 + (k - 1) * 57;
        lv_obj_set_pos(mt_album_art(t, a, 49), 14, y);
        lv_obj_t *tl = mt_label(t, title, &ss_semibold_18, LV_OPA_COVER);
        lv_obj_set_pos(tl, 76, y + 2);
        lv_obj_set_width(tl, w - 90);
        label_lines(tl, 1);
        lv_obj_t *ar = mt_label(t, artist, &ss_regular_16, 200);
        lv_obj_set_pos(ar, 76, y + 26);
        lv_obj_set_width(ar, w - 90);
        label_lines(ar, 1);
    }
    return true;
}

/* Playlists: the first few saved ones, a tap on one plays it. False when
 * there are none. */
static bool mt_playlists_fill(struct mt_slot *s)
{
    lv_obj_t *t = s->obj;
    int w = mt_px(mt_span_w(s->size)), total = 0, rows = s->size == SZ_WIDE ? 3 : 3;
    mt.pl_rows = 0;
    const char *dirpath = sl_playlist_dir();
    DIR *dir = opendir(dirpath);
    char names[3][64];
    if (dir)
    {
        struct DIRENT *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            const char *name = entry->d_name;
            if (name[0] == '.' || (!sl_suffix(name, ".m3u") && !sl_suffix(name, ".m3u8"))) continue;
            if (mt.pl_rows < rows)
            {
                int k = mt.pl_rows;
                snprintf(mt.pl_path[k], sizeof mt.pl_path[k], "%s%s%s", dirpath,
                         dirpath[strlen(dirpath) - 1] == '/' ? "" : "/", name);
                strlcpy(names[k], name, sizeof names[k]);
                char *dot = strrchr(names[k], '.');
                if (dot && dot != names[k]) *dot = '\0';
                mt.pl_rows++;
            }
            total++;
        }
        closedir(dir);
    }
    if (!mt.pl_rows) return false;
    lv_obj_set_pos(mt_label(t, sl_tr(mt_defs[s->id].name), &ss_semibold_21, LV_OPA_COVER), 14, 10);
    char b[24];
    snprintf(b, sizeof b, "%d", total);
    lv_obj_t *count = mt_label(t, b, &ss_regular_18, 200);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -14, 13);
    for (int k = 0; k < mt.pl_rows; k++)
    {
        int y = 46 + k * 54;
        mt.pl_y[k] = y;
        lv_obj_t *dot = box(t);           /* a play button, a step lighter than the tile */
        lv_obj_set_size(dot, 40, 40);
        lv_obj_set_pos(dot, 14, y + 2);
        lv_obj_set_style_radius(dot, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(mt.ink), 0);
        lv_obj_set_style_bg_opa(dot, 46, 0);
        lv_obj_t *g = mt_label(dot, LV_SYMBOL_PLAY, &sl_sym_16, LV_OPA_COVER);
        lv_obj_align(g, LV_ALIGN_CENTER, 2, 0);
        lv_obj_t *n = mt_label(t, names[k], &ss_semibold_21, LV_OPA_COVER);
        lv_obj_set_pos(n, 66, y + 8);
        lv_obj_set_width(n, w - 80);
        label_lines(n, 1);
    }
    return true;
}

/* Drop the pointers into a tile's content before it is rebuilt or removed. */
static void mt_forget(int id)
{
    switch (id)
    {
        case TL_MUSIC: mt.music_front = mt.lib_count = mt.lib_line = NULL; break;
        case TL_CLOCK: mt.clock_time = mt.clock_day = mt.clock_date = mt.clock_batt = NULL; break;
        case TL_PLAYROOM: mt.play_back = NULL; break;
        case TL_UPNEXT: mt.queue_badge = NULL; break;
        case TL_PET: mt.pet_canvas = NULL; break;
        case TL_SPECTRUM: sp_forget(); break;
        case TL_GABAGOOL: gb_forget(); break;
        case TL_SETTINGS: home_settings_icon = NULL; home_icon_small = false; break;
    }
}

static void mt_fill(struct mt_slot *s)
{
    lv_obj_t *t = s->obj, *front, *back;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_obj_clean(t);
    mt_forget(s->id);
    s->faces = s->back = NULL;
    s->down_at = 0;
    lv_obj_set_style_bg_color(t, lv_color_hex(mt_tile_shade(s->id)), 0);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_transform_pivot_x(t, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(t, h / 2, 0);
    bool live = (s->live & 1) && mt_defs[s->id].live, big = s->size != SZ_SMALL;
    const char *name = mt_defs[s->id].name;
    switch (s->id)
    {
    case TL_MUSIC:
        if (live && big)
        {
            mt_faces(s, &mt.music_front, &s->back);   /* covers and counts: filled when it peeks */
            mt_music_front();
        }
        else if (live) { mt.music_front = box(t); lv_obj_set_size(mt.music_front, w, h); mt_music_front(); }
        else { mt_glyph(t, LV_SYMBOL_AUDIO, s->size); if (big) mt_title(t, name); }
        return;
    case TL_CLOCK:
        if (s->size == SZ_SMALL)
        {
            mt.clock_time = mt_label(t, "", &ss_semibold_26, LV_OPA_COVER);
            lv_obj_align(mt.clock_time, LV_ALIGN_CENTER, 0, -10);
            mt.clock_date = mt_label(t, "", &ss_regular_16, 215);
            lv_obj_align(mt.clock_date, LV_ALIGN_CENTER, 0, 20);
            lv_obj_set_width(mt.clock_date, w - 8);
            lv_obj_set_style_text_align(mt.clock_date, LV_TEXT_ALIGN_CENTER, 0);
            label_lines(mt.clock_date, 1);
        }
        else
        {
            lv_obj_t *face = t;
            if (live && s->size == SZ_MED) { mt_faces(s, &front, &back); face = front; }
            mt.clock_time = mt_label(face, "", &ss_regular_60, LV_OPA_COVER);
            lv_obj_set_pos(mt.clock_time, 12, 6);
            mt.clock_day = mt_label(face, "", &ss_semibold_21, LV_OPA_COVER);
            lv_obj_set_pos(mt.clock_day, 14, 96);
            mt.clock_date = mt_label(face, "", &ss_regular_18, 215);
            lv_obj_set_pos(mt.clock_date, 14, 124);
            lv_obj_t *bface = s->faces ? back : t;
            if (s->faces || s->size == SZ_WIDE)
            {
                int bx = s->faces ? 12 : w / 2 + 20;
                mt.clock_batt = mt_label(bface, "", &ss_regular_60, LV_OPA_COVER);
                lv_obj_set_pos(mt.clock_batt, bx, 6);
                lv_obj_set_pos(mt_label(bface, "battery", &ss_regular_18, 215), bx + 2, 96);
            }
            mt_title(s->faces ? back : t, "Notification Center");
        }
        mt.last_min = -1;
        mt_clock_update();
        return;
    case TL_PLAYROOM:
    case TL_COVERFLOW:
    {
        lv_obj_t *face = t;
        if (live && (big || s->id == TL_COVERFLOW)) { mt_faces(s, &front, &s->back); face = front; }
        if (s->faces && s->id == TL_COVERFLOW) { mt_cf_front(s, false); return; }
        mt_icon(face, s->id, s->size);
        if (big) mt_title(face, name);
        if (!s->faces) return;
        back = s->back;
        lv_obj_set_pos(mt_label(back, "play", &ss_regular_18, 215), 14, 14);
        mt.play_back = mt_label(back, mt_games[mt.game_ix], &ss_regular_34, LV_OPA_COVER);
        lv_obj_set_pos(mt.play_back, 12, 40);
        lv_obj_set_width(mt.play_back, w - 24);
        lv_label_set_long_mode(mt.play_back, LV_LABEL_LONG_WRAP);
        mt_title(back, name);
        return;
    }
    case TL_NOWPLAYING:
    {
        /* the song and whether it is playing, live */
        struct mp3entry *id3 = live && something_playing() ? audio_current_track() : NULL;
        mt.np_state = !id3 ? 0 : is_playing() ? 1 : 2;
        if (!id3) { mt_glyph(t, LV_SYMBOL_PLAY, s->size); if (big) mt_title(t, name); return; }
        const char *state = mt.np_state == 1 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        if (!big) { mt_glyph(t, state, s->size); return; }
        lv_obj_t *g = mt_label(t, state, &sl_sym_28, LV_OPA_COVER);
        lv_obj_set_pos(g, 14, 14);
        char name[64];
        lv_obj_t *tl = mt_label(t, track_title(id3, name, sizeof name), &ss_semibold_23, LV_OPA_COVER);
        lv_obj_set_pos(tl, 14, 58);
        lv_obj_set_width(tl, w - 28);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
        label_lines(tl, 2);
        lv_obj_t *ar = mt_label(t, id3->albumartist ? id3->albumartist : id3->artist ? id3->artist : "",
                                &ss_regular_18, 215);
        lv_obj_set_pos(ar, 14, 58 + 2 * lv_font_get_line_height(&ss_semibold_23) + 4);
        lv_obj_set_width(ar, w - 28);
        label_lines(ar, 1);
        mt_title(t, mt.np_state == 1 ? "Now playing" : "Paused");
        return;
    }
    case TL_SETTINGS:
    {
        lv_obj_t *ic = box(t);
        lv_obj_set_size(ic, 80, 80);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, big ? -12 : 0);
        home_icon_small = !big;
        home_settings_icon = ic;
        settings_icon_fill(ic, global_settings.sloppy_skull_icon);
        if (big) mt_title(t, name);
        return;
    }
    case TL_RADIO:
        if (!live || !big) { mt_icon(t, s->id, s->size); if (big) mt_title(t, name); return; }
        radio_tile(t, w, s->size == SZ_WIDE);
        mt_title(t, name);
        return;
    case TL_SPECTRUM:
        if (live) { mt_spectrum_fill(s); return; }
        mt_icon(t, s->id, s->size);
        if (big) mt_title(t, name);
        return;
    case TL_GABAGOOL:
        mt_gabagool_fill(s);
        return;
    case TL_UPNEXT:
        if (live && big && mt_upnext_fill(s)) return;
        goto plain;
    case TL_PLAYLISTS:
        if (big && mt_playlists_fill(s)) return;
        goto plain;
    case TL_PET:
        if (live && big) { mt_pet_fill(s); return; }
        mt_icon(t, s->id, s->size);
        if (big) mt_title(t, name);
        return;
    case TL_ALBUMS:
        /* covers edge to edge, one swapping now and then */
        if (live && sl_lib.n_albums)
        {
            s->back = mt_mosaic(t, w, h);
            if (big) mt_over_art(t, name, w, h);
            return;
        }
        /* fall through */
    default:
    plain:
    {
        lv_obj_t *face = t;
        if (live && mt_has_back(s->id, s->size)) { mt_faces(s, &front, &s->back); face = front; }
        mt_icon(face, s->id, s->size);
        if (big) mt_title(face, name);
        if (s->id == TL_UPNEXT && live)
        {
            mt.queue_badge = mt_label(face, "", big ? &ss_regular_34 : &ss_semibold_18, LV_OPA_COVER);
            lv_obj_align(mt.queue_badge, LV_ALIGN_BOTTOM_RIGHT, -10, big ? -2 : -4);
            mt_queue_update();
        }
        return;
    }
    }
}

/* ------------------------------------------------------------- editing */
static void mt_layout(bool animate);
static void mt_edit_exit(void);

#define MT_CTL 48                         /* an edit control; the X reaches 18 px past it */

/* A shown control glides along with its tile; a hidden one just goes there. */
static void mt_ctl_move(lv_obj_t *b, int x, int y)
{
    if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) { lv_anim_delete(b, mt_set_x); lv_anim_delete(b, mt_set_y); lv_obj_set_pos(b, x, y); return; }
    mt_glide(b, mt_set_x, lv_obj_get_x(b), x);
    mt_glide(b, mt_set_y, lv_obj_get_y(b), y);
}

static void mt_ctl_place(void)
{
    if (!mt.edit || mt.dragging || mt.sel < 0 || mt.sel >= mt.n)   /* not over a tile in the air */
    {
        if (mt.ctl_unpin) lv_obj_add_flag(mt.ctl_unpin, LV_OBJ_FLAG_HIDDEN);
        if (mt.ctl_size) lv_obj_add_flag(mt.ctl_size, LV_OBJ_FLAG_HIDDEN);
        if (mt.ctl_live) lv_obj_add_flag(mt.ctl_live, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    struct mt_slot *s = &mt.slot[mt.sel];
    int x = mt_x(s->c), y = mt_y(s->r), w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    /* on the tile's corners, but never off the edge of the screen, nor
     * above the grid: a touch there never reaches the grid's children */
    int right = x + w - 32 > W - MT_CTL - 4 ? W - MT_CTL - 4 : x + w - 32, left = x - 16 < 4 ? 4 : x - 16;
    int top = y - 16 < 2 ? 2 : y - 16;
    mt_ctl_move(mt.ctl_unpin, right, top);
    mt_ctl_move(mt.ctl_size, right, y + h - 32);
    mt_ctl_move(mt.ctl_live, left, y + h - 32);
    lv_obj_remove_flag(mt.ctl_unpin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mt.ctl_size, LV_OBJ_FLAG_HIDDEN);
    /* the analyzer's control turns it to its next mode, and looks it */
    lv_label_set_text(lv_obj_get_child(mt.ctl_size, 0), s->id == TL_SPECTRUM ? LV_SYMBOL_REFRESH : "");
    lv_obj_invalidate(mt.ctl_size);
    if (mt_defs[s->id].live)
    {
        lv_label_set_text(lv_obj_get_child(mt.ctl_live, 0), s->live & 1 ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
        lv_obj_remove_flag(mt.ctl_live, LV_OBJ_FLAG_HIDDEN);
    }
    else lv_obj_add_flag(mt.ctl_live, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mt.ctl_unpin);
    lv_obj_move_foreground(mt.ctl_size);
    lv_obj_move_foreground(mt.ctl_live);
}

/* In edit mode every tile steps back and dims; the chosen one stays up,
 * and it can be dragged without scrolling the page. */
static void mt_select(int i)
{
    mt.sel = i;
    for (int k = 0; k < mt.n; k++)
    {
        lv_obj_t *o = mt.slot[k].obj;
        bool chosen = mt.edit && k == i;
        lv_anim_delete(o, mt_set_opa);
        lv_obj_set_style_opa(o, !mt.edit || chosen ? LV_OPA_COVER : 125, 0);
        if (chosen) { lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN); lv_obj_move_foreground(o); }
        else lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN);
    }
    mt_ctl_place();
}

/* Press: a veil darkens the tile under the finger, in at once and out
 * softly. It replaced a 95 % scale, which drew the whole tile through a
 * layer on every frame it was held and striped its covers. One at a time. */
#define MT_VEIL 64
static lv_obj_t *mt_veil_obj;
static void mt_veil_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void mt_veil_done(lv_anim_t *a) { lv_obj_delete(a->var); }
static void mt_veil_deleted(lv_event_t *e) { if (lv_event_get_target(e) == mt_veil_obj) mt_veil_obj = NULL; }
static void mt_veil(lv_obj_t *t, bool on)
{
    if (mt_veil_obj && (on || lv_obj_get_parent(mt_veil_obj) != t)) lv_obj_delete(mt_veil_obj);
    if (!on && !mt_veil_obj) return;
    lv_obj_t *v = mt_veil_obj;
    if (!v)
    {
        v = mt_veil_obj = box(t);
        lv_obj_add_flag(v, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(v, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(v, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(v, mt_veil_deleted, LV_EVENT_DELETE, NULL);
    }
    lv_anim_delete(v, mt_veil_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, v);
    lv_anim_set_values(&a, lv_obj_get_style_bg_opa(v, 0), on ? MT_VEIL : 0);
    lv_anim_set_duration(&a, on ? SL_T_TOUCH : SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, mt_veil_opa);
    if (!on) lv_anim_set_completed_cb(&a, mt_veil_done);
    lv_anim_start(&a);
}
static void mt_tile_event(lv_event_t *e);
static void mt_make_tile(int i, unsigned delay)
{
    struct mt_slot *s = &mt.slot[i];
    lv_obj_t *t = box(mt.grid);
    s->obj = t;
    lv_obj_set_style_bg_color(t, lv_color_hex(mt_tile_color()), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(t, mt_tile_event, LV_EVENT_ALL, NULL);
    mt_fill(s);
    if (delay != (unsigned)-1) home_rush_in(t, home_intro_ms + delay);
}

static void mt_layout(bool animate)
{
    int ar = 0, ac = 0;
    mt.rows = mt_place(&ar, &ac);
    int rows = mt.edit && mt_y(ar + 1) > mt_y(mt.rows) ? ar + 1 : mt.rows;
    lv_obj_set_height(mt.grid, mt_y(rows) + 4);
    for (int i = 0; i < mt.n; i++)
    {
        lv_obj_t *o = mt.slot[i].obj;
        int x = mt_x(mt.slot[i].c), y = mt_y(mt.slot[i].r);
        if (mt.dragging && i == mt.drag_slot)
        {
            lv_anim_delete(o, mt_set_x);
            lv_anim_delete(o, mt_set_y);
            lv_obj_set_pos(o, x, y);          /* it stays under the finger by its translate */
        }
        else if (animate)
        {
            mt_glide(o, mt_set_x, lv_obj_get_x(o), x);
            mt_glide(o, mt_set_y, lv_obj_get_y(o), y);
        }
        else lv_obj_set_pos(o, x, y);
    }
    if (mt.add_tile)
    {
        if (mt.edit)
        {
            lv_obj_set_pos(mt.add_tile, mt_x(ac), mt_y(ar));
            lv_obj_remove_flag(mt.add_tile, LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_add_flag(mt.add_tile, LV_OBJ_FLAG_HIDDEN);
    }
    mt_ctl_place();
}

static int mt_index_of(lv_obj_t *o)
{
    for (int i = 0; i < mt.n; i++) if (mt.slot[i].obj == o) return i;
    return -1;
}

static void mt_unpin_cb(lv_event_t *e)
{
    (void)e;
    if (mt.sel < 0 || mt.sel >= mt.n) return;
    struct mt_slot *s = &mt.slot[mt.sel];
    mt_forget(s->id);
    if (s->id == TL_GABAGOOL) gb_clip_free();
    sfx_play(SFX_UI_REMOVE);
    lv_obj_delete(s->obj);
    memmove(s, s + 1, (size_t)(mt.n - mt.sel - 1) * sizeof *s);
    mt.n--;
    /* nothing is chosen after: the X does not land on the next tile,
     * where a quick second tap would unpin that one too */
    mt_select(-1);
    mt_layout(true);
    mt_save();
}
/* A resize grows or shrinks the tile from its old size, and says what it
 * is now for a moment, so the change reads as one movement. */
static void mt_set_w(void *o, int32_t v) { lv_obj_set_width(o, v); }
static void mt_set_h(void *o, int32_t v) { lv_obj_set_height(o, v); }
/* delayed, not async: an anim on the badge goes with it if the tile goes
 * first, where an async call would delete it a second time */
static void mt_badge_gone(lv_anim_t *a) { lv_obj_delete_delayed(a->var, 0); }
static void mt_resize_glide(struct mt_slot *s, int ow, int oh)
{
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s->obj);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, mt_set_w);
    lv_anim_set_values(&a, ow, w);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, mt_set_h);
    lv_anim_set_values(&a, oh, h);
    lv_anim_start(&a);
    static const char *const names[] = { "Small", "Medium", "Wide" };
    /* quick taps: the new size's badge replaces the last one */
    for (int k = (int)lv_obj_get_child_count(s->obj) - 1; k >= 0; k--)
    {
        lv_obj_t *old = lv_obj_get_child(s->obj, k);
        if (lv_obj_get_user_data(old) == (void *)names) { lv_anim_delete(old, NULL); lv_obj_delete(old); }
    }
    lv_obj_t *b = box(s->obj);
    lv_obj_set_user_data(b, (void *)names);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(b, 200, 0);
    lv_obj_set_style_radius(b, SL_R_MD, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 0);   /* clear of the tile's own title */
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sl_tr(names[s->size]));
    lv_obj_set_style_text_font(l, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_anim_init(&a);
    lv_anim_set_var(&a, b);
    lv_anim_set_exec_cb(&a, mt_set_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 700);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_completed_cb(&a, mt_badge_gone);
    lv_anim_start(&a);
}
/* Keep the tile in sight when its new size moved it. */
static void mt_scroll_into_view(struct mt_slot *s)
{
    int top = mt_y(s->r), bottom = top + mt_px(mt_span_h(s->size));
    int sy = lv_obj_get_scroll_y(mt.body), view = H - SB_H - 120;
    if (top < sy) lv_obj_scroll_to_y(mt.body, top - 10, LV_ANIM_ON);
    else if (bottom > sy + view) lv_obj_scroll_to_y(mt.body, bottom - view + 10, LV_ANIM_ON);
}

static void mt_size_cb(lv_event_t *e)
{
    (void)e;
    if (mt.sel < 0 || mt.sel >= mt.n) return;
    struct mt_slot *s = &mt.slot[mt.sel];
    if (s->id == TL_SPECTRUM)
    {
        /* the next mode, live again if it was not */
        s->live = (uint8_t)(((s->live >> 1) + 1) % SP_MODES << 1 | 1);
        sfx_play(SFX_UI_ON);
        mt_fill(s);
        mt_ctl_place();
        mt_save();
        toast(sp_mode_names[sp_mode_of(s)]);
        return;
    }
    /* growing, then back to small: small, medium, wide */
    int ow = lv_obj_get_width(s->obj), oh = lv_obj_get_height(s->obj);
    s->size = s->size == SZ_SMALL ? SZ_MED : s->size == SZ_MED ? SZ_WIDE : SZ_SMALL;
    sfx_play(SFX_UI_DROP);
    mt_fill(s);
    mt_resize_glide(s, ow, oh);
    mt_layout(true);
    mt_save();
    mt_scroll_into_view(s);
}
static void mt_live_cb(lv_event_t *e)
{
    (void)e;
    if (mt.sel < 0 || mt.sel >= mt.n) return;
    struct mt_slot *s = &mt.slot[mt.sel];
    s->live ^= 1;
    sfx_play(s->live & 1 ? SFX_UI_ON : SFX_UI_OFF);
    mt_fill(s);
    mt_ctl_place();
    mt_save();
}

/* the resize control's diagonal double arrow, drawn: no font has one */
static void mt_arrow_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    if (mt.sel >= 0 && mt.sel < mt.n && mt.slot[mt.sel].id == TL_SPECTRUM) return;   /* it cycles modes there */
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int x0 = a.x1 + 13, y0 = a.y2 - 13, x1 = a.x2 - 13, y1 = a.y1 + 13;
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = lv_color_hex(0x111111);
    l.width = 3;
    l.round_start = l.round_end = 1;
    l.p1.x = x0 + 3; l.p1.y = y0 - 3; l.p2.x = x1 - 3; l.p2.y = y1 + 3;
    lv_draw_line(layer, &l);
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.bg_color = lv_color_hex(0x111111);
    t.bg_opa = LV_OPA_COVER;
    t.p[0].x = x1; t.p[0].y = y1; t.p[1].x = x1 - 9; t.p[1].y = y1; t.p[2].x = x1; t.p[2].y = y1 + 9;
    lv_draw_triangle(layer, &t);
    t.p[0].x = x0; t.p[0].y = y0; t.p[1].x = x0 + 9; t.p[1].y = y0; t.p[2].x = x0; t.p[2].y = y0 - 9;
    lv_draw_triangle(layer, &t);
}

static lv_obj_t *mt_ctl(const char *sym, lv_event_cb_t cb, bool arrow)
{
    bool unpin = cb == mt_unpin_cb;
    lv_obj_t *b = box(mt.grid);
    lv_obj_set_size(b, MT_CTL, MT_CTL);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x111111), 0);
    sl_elevate(b, SL_E2, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(0x111111), 0);   /* the glyph takes it */
    /* held: it sinks and darkens, the X turning red, so a press shows
     * before it is let go */
    lv_obj_set_style_transform_scale(b, 220, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(unpin ? 0xe5484d : 0xd4d4d4), LV_STATE_PRESSED);
    if (unpin)
    {
        lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(0xe5484d), LV_STATE_PRESSED);
    }
    lv_obj_set_style_transform_pivot_x(b, MT_CTL / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, MT_CTL / 2, 0);
    lv_obj_t *l = lv_label_create(b);                   /* child 0 */
    lv_label_set_text(l, sym ? sym : "");
    lv_obj_set_style_text_font(l, &sl_sym_20, 0);
    lv_obj_center(l);
    if (arrow) lv_obj_add_event_cb(b, mt_arrow_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    /* 84 px to hit for the X (7 mm on the R1), 72 for the others: they
     * sit above every tile, so a press there never reaches a tile below */
    lv_obj_set_ext_click_area(b, unpin ? 18 : 12);
    on_click(b, cb, NULL);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static void mt_pin(int id, int size);
static void mt_pick_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    mt_pin(id, id == TL_SPECTRUM ? SZ_WIDE : SZ_MED);   /* its control cycles modes, not sizes */
    page_pop();
}
/* "+": the tiles that are not on start yet */
static void mt_add_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *p, *body = page_new("Pin to start", &p);
    static const struct { const char *title; const uint8_t *ids; int n; } groups[] = {
        { "MUSIC", mt_group_music, sizeof mt_group_music },
        { "APPS & GAMES", mt_group_apps, sizeof mt_group_apps },
        { "SYSTEM", mt_group_system, sizeof mt_group_system },
    };
    int shown = 0;
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
    {
        bool header = false;
        for (int k = 0; k < groups[g].n; k++)
        {
            int id = groups[g].ids[k];
            if (mt_find(id) >= 0) continue;
            if (!header) { section(body, groups[g].title); header = true; }
            lv_obj_t *r = box(body);
            lv_obj_set_size(r, lv_pct(100), 76);
            lv_obj_add_style(r, &st_row, 0);
            lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
            lv_obj_t *ic = box(r);
            lv_obj_set_size(ic, 56, 56);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 4, 0);
            lv_obj_set_style_bg_color(ic, lv_color_hex(mt_tile_color()), 0);
            lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
            mt_list_icon(ic, id);
            lv_obj_t *l = text(r, mt_defs[id].name, &ss_regular_34, C_TEXT);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 76, -2);
            lv_obj_set_width(l, W - 130);
            label_lines(l, 1);
            on_click(r, mt_pick_cb, (void *)(intptr_t)id);
            shown++;
        }
    }
    if (!shown) text(body, "Every tile is already on start.", &ss_regular_18, C_SUB);
    page_push(p);
}

static void mt_drop(void);
static void mt_done_cb(lv_event_t *e) { (void)e; mt_edit_exit(); }
static void mt_edit_enter(int i)
{
    if (mt.edit) { mt_select(i); return; }
    mt.edit = true;
    sfx_play(SFX_UI_PICKUP);
    if (!mt.ctl_unpin)
    {
        mt.ctl_unpin = mt_ctl(LV_SYMBOL_CLOSE, mt_unpin_cb, false);
        mt.ctl_size = mt_ctl(NULL, mt_size_cb, true);
        mt.ctl_live = mt_ctl(LV_SYMBOL_EYE_OPEN, mt_live_cb, false);
        lv_obj_t *a = box(mt.grid);
        mt.add_tile = a;
        lv_obj_set_size(a, MT_SMALL, MT_SMALL);
        lv_obj_set_style_border_width(a, 2, 0);
        lv_obj_set_style_border_color(a, lv_color_hex(mt_page_ink()), 0);
        lv_obj_set_style_border_opa(a, 150, 0);
        lv_obj_set_style_bg_color(a, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(a, 40, LV_STATE_PRESSED);
        lv_obj_t *plus = text(a, LV_SYMBOL_PLUS, &sl_sym_40, mt_page_ink());
        lv_obj_center(plus);
        on_click(a, mt_add_cb, NULL);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_EVENT_BUBBLE);
        mt.done = box(mt.page);
        lv_obj_set_size(mt.done, 150, 52);
        lv_obj_align(mt.done, LV_ALIGN_BOTTOM_MID, 0, -112);
        lv_obj_set_style_radius(mt.done, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(mt.done, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_bg_opa(mt.done, LV_OPA_COVER, 0);
        sl_elevate(mt.done, SL_E3, 0);
        lv_obj_center(text(mt.done, "Done", &ss_semibold_21, C_BG));
        on_click(mt.done, mt_done_cb, NULL);
    }
    lv_obj_remove_flag(mt.done, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mt.done);
    mt_select(i);
    mt_layout(true);
}
static void mt_edit_exit(void)
{
    if (!mt.edit) return;
    mt_drop();
    mt.edit = false;
    if (mt.done) lv_obj_add_flag(mt.done, LV_OBJ_FLAG_HIDDEN);
    mt_select(-1);
    mt_layout(true);
    mt_save();
}

/* The Playlists widget's row under a tap, or -1. */
static int mt_playlist_hit(lv_obj_t *tile, int y)
{
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    for (int k = 0; k < mt.pl_rows; k++)
        if (y >= a.y1 + mt.pl_y[k] && y < a.y1 + mt.pl_y[k] + 50) return k;
    return -1;
}

/* The tile the dragged one's centre is well inside, if any; below them all
 * means last. */
static int mt_drag_target(int i, int cx, int cy)
{
    for (int k = 0; k < mt.n; k++)
    {
        if (k == i) continue;
        int x = mt_x(mt.slot[k].c), y = mt_y(mt.slot[k].r);
        int kw = mt_px(mt_span_w(mt.slot[k].size)), kh = mt_px(mt_span_h(mt.slot[k].size));
        int m = (kw < kh ? kw : kh) / 5;
        if (cx >= x + m && cx < x + kw - m && cy >= y + m && cy < y + kh - m) return k;
    }
    return cy > mt_y(mt.rows) ? mt.n - 1 : -1;
}
/* Tile i takes place to; the rest make room. Returns where it is now. */
static int mt_drag_move(int i, int to)
{
    struct mt_slot moved = mt.slot[i];
    if (to > i) memmove(&mt.slot[i], &mt.slot[i + 1], (size_t)(to - i) * sizeof moved);
    else memmove(&mt.slot[to + 1], &mt.slot[to], (size_t)(i - to) * sizeof moved);
    mt.slot[to] = moved;
    mt.sel = mt.drag_slot = to;
    mt.drag_moved_at = lv_tick_get();
    mt_layout(true);
    return to;
}

/* Let a dragged tile go: it lands under the finger and settles into its
 * cell. Also ends a drag whose release never came (the screen locked, a
 * page opened over Start), so no tile stays in the air or outlined.
 * Saves only when the tile changed place. */
static void mt_drop(void)
{
    if (!mt.dragging) return;
    int i = mt.drag_slot;
    mt.dragging = false;
    mt.drag_slot = -1;
    if (i < 0 || i >= mt.n) return;
    struct mt_slot *s = &mt.slot[i];
    lv_obj_t *o = s->obj;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    int vx = mt_x(s->c) + lv_obj_get_style_translate_x(o, 0), vy = mt_y(s->r) + lv_obj_get_style_translate_y(o, 0);
    int to = mt_drag_target(i, vx + w / 2, vy + h / 2);
    bool home = vx + w / 2 >= mt_x(s->c) && vx + w / 2 < mt_x(s->c) + w &&
                vy + h / 2 >= mt_y(s->r) && vy + h / 2 < mt_y(s->r) + h;
    if (to >= 0 && to != i && !home)
    {
        mt.dragging = true;               /* mt_drag_move lays out around it */
        i = mt_drag_move(i, to);
        mt.dragging = false;
        mt.drag_slot = -1;
    }
    lv_obj_set_style_translate_x(o, 0, 0);
    lv_obj_set_style_translate_y(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_pos(o, vx, vy);
    sfx_play(SFX_UI_DROP);
    mt_layout(true);
    if (i != mt.drag_from) mt_save();
}

/* Pull to refresh stays out of the way while tiles are edited. */
static bool mt_busy(void) { return mt.edit || mt.dragging; }

/* A tile: open it, or in edit mode choose it and drag it somewhere else. */
static void mt_tile_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    int i = mt_index_of(o);
    if (i < 0) return;
    lv_indev_t *in = lv_indev_active();
    lv_point_t pt = { 0, 0 };
    if (in) lv_indev_get_point(in, &pt);
    if (code == LV_EVENT_LONG_PRESSED && !mt.dragging)
    {
        if (mt_veil_obj) lv_obj_delete(mt_veil_obj);
        mt_edit_enter(i);
    }
    else if (code == LV_EVENT_PRESSED)
    {
        mt_drop();                    /* one left in the air by a lost release */
        mt.press = pt;
        i = mt_index_of(o);
        if (!mt.edit) mt_veil(o, true);
    }
    else if (code == LV_EVENT_PRESSING && mt.edit && i == mt.sel)
    {
        int dx = pt.x - mt.press.x, dy = pt.y - mt.press.y;
        if (!mt.dragging && dx * dx + dy * dy > 14 * 14)
        {
            /* picked up: lifted with a bright edge, the others make room.
             * It is taken from where it is seen, even halfway through a
             * glide, so it does not jump to its cell first. */
            mt.dragging = true;
            mt.drag_slot = mt.drag_from = i;
            lv_anim_delete(o, mt_set_x);
            lv_anim_delete(o, mt_set_y);
            mt.drag_x0 = lv_obj_get_x(o) + lv_obj_get_style_translate_x(o, 0);
            mt.drag_y0 = lv_obj_get_y(o) + lv_obj_get_style_translate_y(o, 0);
            mt.drag_sy0 = lv_obj_get_scroll_y(mt.body);
            mt.drag_moved_at = 0;
            lv_obj_set_pos(o, mt_x(mt.slot[i].c), mt_y(mt.slot[i].r));
            lv_obj_set_style_outline_width(o, 3, 0);
            lv_obj_set_style_outline_color(o, lv_color_white(), 0);
            lv_obj_set_style_outline_opa(o, 230, 0);
            sfx_play(SFX_UI_PICKUP);
        }
        if (mt.dragging)
        {
            /* near the top or bottom of the screen the page scrolls on,
             * faster the nearer the edge */
            int top = SB_H + 70 - pt.y, bottom = pt.y - (H - 150);
            int edge = top > 0 ? 3 + (top > 70 ? 70 : top) / 6 : bottom > 0 ? -3 - (bottom > 70 ? 70 : bottom) / 6 : 0;
            if (edge > 0 && lv_obj_get_scroll_top(mt.body) > 0) lv_obj_scroll_by_bounded(mt.body, 0, edge, LV_ANIM_OFF);
            if (edge < 0 && lv_obj_get_scroll_bottom(mt.body) > 0) lv_obj_scroll_by_bounded(mt.body, 0, edge, LV_ANIM_OFF);
            struct mt_slot *s = &mt.slot[i];
            int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
            int vx = mt.drag_x0 + dx, vy = mt.drag_y0 + dy + lv_obj_get_scroll_y(mt.body) - mt.drag_sy0;
            int cx = vx + w / 2, cy = vy + h / 2;
            /* over another tile, well inside it: take its place, and let the
             * rest settle a moment before moving them again */
            int to = mt_drag_target(i, cx, cy);
            /* still over its own new cell: it is where the finger wants it.
             * Without this a tile pushed down by a wide one swapped back. */
            bool home = cx >= mt_x(s->c) && cx < mt_x(s->c) + w && cy >= mt_y(s->r) && cy < mt_y(s->r) + h;
            if (to >= 0 && to != i && !home && (!mt.drag_moved_at || lv_tick_elaps(mt.drag_moved_at) > 220))
            {
                i = mt_drag_move(i, to);
                s = &mt.slot[i];
                sfx_play(SFX_UI_TICK);
            }
            lv_obj_set_style_translate_x(o, vx - mt_x(s->c), 0);
            lv_obj_set_style_translate_y(o, vy - mt_y(s->r), 0);
        }
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) mt_veil(o, false);
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && mt.dragging)
    {
        mt_drop();
        mt.press.x = LV_COORD_MIN;               /* the click that follows is no tap */
    }
    else if (code == LV_EVENT_CLICKED)
    {
        if (mt.dragging || mt.press.x == LV_COORD_MIN) return;
        if (g_dragged) return;                     /* a scroll, not a tap */
        if (mt.edit) { mt_select(i); return; }
        int id = mt.slot[i].id;
        if (id == TL_CLOCK) { qp_drag_to(H / 2); qp_release(H / 2, 1000); }
        else if (id == TL_PLAYLISTS && mt.slot[i].size != SZ_SMALL && mt.pl_rows && mt_playlist_hit(o, pt.y) >= 0)
            sl_play_saved(mt.pl_path[mt_playlist_hit(o, pt.y)]);
        else if (id == TL_GABAGOOL) mt_gabagool_tap(&mt.slot[i]);
        /* Cover Flow opens on the album its tile is showing */
        else if (id == TL_COVERFLOW && mt.slot[i].faces && sl_lib.n_albums)
            cf_open_on(((mt.cf_pos % sl_lib.n_albums) + sl_lib.n_albums) % sl_lib.n_albums);
        else mt_open(mt_defs[id].action);
    }
}

static void mt_pin(int id, int size)
{
    int i = mt_find(id);
    if (i < 0 && mt.n < TL_COUNT)
    {
        i = mt.n++;
        mt.slot[i] = (struct mt_slot){ .id = (uint8_t)id, .size = (uint8_t)size, .live = 1 };
        mt_make_tile(i, (unsigned)-1);
        lv_obj_set_pos(mt.slot[i].obj, mt_x(0), mt_y(mt.rows));
    }
    if (i < 0) return;
    sfx_play(SFX_UI_DROP);
    if (mt.edit) mt_select(i);
    mt_layout(true);
    mt_save();
}

/* --------------------------------------------------------------- tick */
/* Some of the tile shows between the status bar and the bottom of the screen. */
static bool mt_on_screen(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a.y2 > SB_H && a.y1 < H;
}
static void mt_tick(lv_timer_t *t)
{
    (void)t;
    if (mt.dragging && !(touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED)) mt_drop();
    if (!mt.page || npages > 1 || overlay_visible() || sl_power_hides_ui()) return;   /* only while seen */
    mt.ticks++;
    struct tm *tm = get_time();
    if (tm->tm_min != mt.last_min) mt_clock_update();
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    bool changed = (id3 != NULL) != mt.music_state || (id3 && strcmp(id3->path, mt.music_path));
    if (changed) mt_music_front();
    int np = mt_find(TL_NOWPLAYING);
    if (np >= 0 && (changed || mt.np_state != (!id3 ? 0 : is_playing() ? 1 : 2)))
        mt_fill(&mt.slot[np]);
    mt.music_state = id3 != NULL;           /* remembered even with no Music tile */
    strlcpy(mt.music_path, id3 ? id3->path : "", sizeof mt.music_path);
    mt_queue_update();
    int un = mt_find(TL_UPNEXT);
    if (un >= 0 && mt.slot[un].size != SZ_SMALL && (mt.slot[un].live & 1) && mt_upnext_now() != mt.upnext_key)
        mt_fill(&mt.slot[un]);               /* the widget follows the queue */
    mt_lib_update();
    if (sl_lib.n_albums && !mt.had_albums)
    {
        /* the library arrived after start was built: now there are covers */
        mt.had_albums = true;
        mt.cf_pos = mt_cf_start();
        for (int k = 0; k < mt.n; k++)
            if (mt.slot[k].id == TL_ALBUMS || mt.slot[k].id == TL_COVERFLOW) mt_fill(&mt.slot[k]);
    }
    if (mt.cf_flow_at && mt.ticks >= mt.cf_flow_at)
    {
        mt.cf_flow_at = 0;
        int cf = mt_find(TL_COVERFLOW);
        if (cf >= 0) mt_cf_front(&mt.slot[cf], true);
    }
    /* a peeking tile turns back to its front after about three seconds */
    for (int k = 0; k < mt.n; k++)
        if (mt.slot[k].down_at && mt.ticks >= mt.slot[k].down_at)
        {
            mt.slot[k].down_at = 0;
            mt_slide_faces(mt.slot[k].faces, -mt_px(mt_span_h(mt.slot[k].size)), 0);
        }
    if (mt.edit || !mt.n) return;
    /* A cover mosaic swaps a cover every six seconds, and a live tile
     * peeks every four (tiles take turns). Every animation redraws its
     * tile 60 times a second; at the old 4 s and 2 s the idle start
     * screen was redrawing several full screens' worth each second. */
    if (mt.ticks % 6 == 1)
        for (int k = 0; k < mt.n; k++)
            if (!mt.slot[k].faces && mt.slot[k].back && mt_on_screen(mt.slot[k].obj))
                mt_mosaic_swap(mt.slot[k].back);
    /* A live tile on screen peeks at its back face, taking turns: tiles
     * scrolled away do not use up turns. The back is made a second before
     * the peek, while it is out of sight, so its cover has come from the
     * card by the time it slides into view (sl_art_apply_scaled_async). */
    if (mt.ticks % 4 == 3)
    {
        mt.peek_next = 0;
        for (int k = 0; k < mt.n; k++)
        {
            int at = mt.peek_turn++ % mt.n;
            struct mt_slot *s = &mt.slot[at];
            if (!s->faces || s->down_at || lv_anim_get(s->faces, mt_set_face) || !mt_on_screen(s->obj) ||
                !mt_back(s))
                continue;
            mt.peek_next = at + 1;
            break;
        }
        return;
    }
    if (mt.ticks % 4) return;
    int at = mt.peek_next - 1;
    mt.peek_next = 0;
    if (at < 0 || at >= mt.n) return;
    struct mt_slot *s = &mt.slot[at];
    if (!s->faces || s->down_at || lv_anim_get(s->faces, mt_set_face) || !mt_on_screen(s->obj)) return;
    mt_peek(s->faces, mt_px(mt_span_h(s->size)));
    s->down_at = mt.ticks + 4;
}

static void mt_page_deleted(lv_event_t *e)
{
    (void)e;
    if (mt.timer) lv_timer_delete(mt.timer);
    if (mt.pet_timer) lv_timer_delete(mt.pet_timer);
    sp_forget();
    gb_forget();
    gb_clip_free();
    memset(&mt, 0, sizeof mt);
    home_icon_small = false;
}

/* ------------------------------------------------------------ apps list */
static void mt_app_event(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
    {
        /* as on the phone: hold an app to pin it to start */
        if (mt_find(id) >= 0) { toast("Already on start"); return; }
        mt_pin(id, SZ_MED);
        lv_obj_scroll_to_y(mt.body, 0, LV_ANIM_ON);
        toast("Pinned to start");
        lv_indev_wait_release(lv_indev_active());
    }
    else if (lv_event_get_code(e) == LV_EVENT_CLICKED && !g_dragged)
        mt_open(mt_defs[id].action);
}
static void mt_app(lv_obj_t *body, int id)
{
    lv_obj_t *r = box(body);
    lv_obj_set_size(r, lv_pct(100), 78);
    lv_obj_set_style_pad_left(r, MT_M, 0);
    lv_obj_t *ic = box(r);
    lv_obj_set_size(ic, 58, 58);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(mt_tile_color()), 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    mt_list_icon(ic, id);
    lv_obj_t *l = mt_label(r, sl_tr(mt_defs[id].name), &ss_regular_34, LV_OPA_COVER);
    lv_obj_set_style_text_color(l, lv_color_hex(mt_page_ink()), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 76, -2);
    lv_obj_set_style_text_opa(l, 150, LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(r, 4, LV_STATE_PRESSED);
    press_motion(r);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, mt_app_event, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_add_event_cb(r, mt_app_event, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)id);
}

/* a tap on the empty part of the page ends editing */
static void mt_page_click(lv_event_t *e) { (void)e; if (!g_dragged) mt_edit_exit(); }

static void metro_home_page(void)
{
    lv_obj_t *p = box(ui_screen());
    lv_obj_add_style(p, &st_page, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(C_BG), 0);
    lv_obj_set_size(p, W, H);
    lv_obj_add_event_cb(p, sl_wallpaper_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *body = lv_obj_create(p);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, SB_H);
    lv_obj_set_size(body, W, H - SB_H);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, mt_page_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(body, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_style_pad_bottom(body, 130, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    memset(&mt, 0, sizeof mt);
    mt.page = p;
    mt.body = body;
    mt.sel = -1;
    mt.last_min = -1;
    mt.seed = (uint32_t)current_tick * 2654435761u + 1;
    mt.had_albums = sl_lib.n_albums > 0;
    mt.cf_pos = mt.had_albums ? mt_cf_start() : 0;
    mt_tile_color();                      /* sets mt.ink */
    mt_load();

    mt.grid = box(body);
    lv_obj_set_width(mt.grid, W);
    lv_obj_add_flag(mt.grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mt.grid, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(mt.grid, mt_page_click, LV_EVENT_CLICKED, NULL);
    int ar, ac;
    mt_place(&ar, &ac);
    for (int i = 0; i < mt.n; i++)
        mt_make_tile(i, (unsigned)(mt.slot[i].r * 24 + mt.slot[i].c * 10));
    mt_layout(false);
    mt_queue_update();

    /* apps: the same list as the cards home, alphabetical */
    lv_obj_t *h = mt_label(body, "apps", &ss_regular_60, LV_OPA_COVER);
    lv_obj_set_style_text_color(h, lv_color_hex(mt_page_ink()), 0);
    lv_obj_set_style_pad_left(h, MT_M - 3, 0);
    lv_obj_set_style_margin_top(h, 22, 0);
    lv_obj_set_style_margin_bottom(h, 6, 0);
    mt_turnstile(h, 260);
    for (unsigned i = 0; i < sizeof mt_apps; i++)
        mt_app(body, mt_apps[i]);

    mt.timer = lv_timer_create(mt_tick, 1000, NULL);
    mt.pet_timer = lv_timer_create(mt_pet_tick, 220, NULL);
    lv_obj_add_event_cb(p, mt_page_deleted, LV_EVENT_DELETE, NULL);
    ptr_attach(p, body, mt_page_ink());
    pages[npages++] = p;
}
