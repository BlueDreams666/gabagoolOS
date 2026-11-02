/* GabagoolOS battery log (PS22). GPLv2+. Included by sloppy.c after the
 * status bar and before Settings.
 *
 * Once a minute, a line on the card: the time, the level, the voltage, and
 * whether it charged, the screen was lit and music played. One small write
 * a minute and nothing else: the check rides on the status tick that runs
 * anyway. Time and power draws the last day of it as a graph, read from the
 * file's tail when the page opens, so nothing stays in memory. */

#define BL_FILE ROCKBOX_DIR "/sloppy/battery.csv"
#define BL_PREV ROCKBOX_DIR "/sloppy/battery.prev.csv"
#define BL_EVERY_MS 60000u
#define BL_MAX_BYTES (512 * 1024)        /* about nine days; then it starts again, the last kept beside it */
#define BL_SPAN_S (24 * 3600)            /* the graph: the last day */
#define BL_TAIL (BL_SPAN_S / 60 * 40)    /* a day of lines, at most 40 bytes each */

static bool bl_on(void) { return global_settings.sloppy_battery_log != 0; }

struct bl_pt { int32_t t; int8_t level; uint8_t flags; uint16_t mv; };
#define BL_CHARGING 1
#define BL_SCREEN   2
#define BL_PLAYING  4

static uint32_t bl_writes;               /* lines written this session, for the tests */
static void bl_write(void)
{
    if (log_held) return;                /* USB mode: the card is the computer's */
    int level = battery_level();
    if (level < 0) return;               /* not measured yet */
#if CONFIG_CHARGING
    bool charging = charger_inserted();
#else
    bool charging = false;
#endif
    char b[64];
    int n = snprintf(b, sizeof b, "%ld,%d,%d,%d,%d,%d\n", (long)time(NULL), level, battery_voltage(),
                     charging, !wallpaper_screen_dark, is_playing());
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(BL_FILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size == 0) write(fd, "time,level,mv,charging,screen,playing\n", 38);
    bool full = size > BL_MAX_BYTES;
    if (!full && write(fd, b, n) == n) bl_writes++;
    close(fd);
    if (full)
    {
        remove(BL_PREV);
        rename(BL_FILE, BL_PREV);
        bl_write();                      /* this minute, in the new file */
    }
}
/* from the status tick: a minute since the last line? */
static void bl_tick(void)
{
    static uint32_t next_ms;
    static bool started;
    uint32_t now = sl_ms();
    if (!bl_on()) { started = false; return; }
    if (!started) { started = true; next_ms = now + 5000; return; }   /* the first a little after boot, when the level has settled */
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + BL_EVERY_MS;
    bl_write();
}

/* The last day from the file's tail (and its previous file's, just after
 * a new one began), oldest first. Returns how many, up to max. */
static int bl_read_file(const char *path, struct bl_pt *pt, int n, int max, long since)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return n;
    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > BL_TAIL ? size - BL_TAIL : 0;
    lseek(fd, from, SEEK_SET);
    char *buf = malloc(BL_TAIL + 1);
    int got = buf ? read(fd, buf, BL_TAIL) : -1;
    close(fd);
    if (got <= 0) { free(buf); return n; }
    buf[got] = '\0';
    char *s = buf;
    if (from > 0) { char *nl = strchr(s, '\n'); s = nl ? nl + 1 : s + got; }   /* a line cut in half */
    while (*s && n < max)
    {
        char *nl = strchr(s, '\n');
        if (nl) *nl = '\0';
        long t; int level, mv, c, sc, pl;
        if (sscanf(s, "%ld,%d,%d,%d,%d,%d", &t, &level, &mv, &c, &sc, &pl) == 6 && t >= since && level >= 0 && level <= 100)
        {
            if (n && t < pt[n - 1].t) n = 0;   /* the clock went back: start from here */
            pt[n++] = (struct bl_pt){ (int32_t)t, (int8_t)level,
                                      (uint8_t)((c ? BL_CHARGING : 0) | (sc ? BL_SCREEN : 0) | (pl ? BL_PLAYING : 0)),
                                      (uint16_t)LV_CLAMP(0, mv, 65535) };
        }
        if (!nl) break;
        s = nl + 1;
    }
    free(buf);
    return n;
}
#define BL_MAX_PTS (BL_SPAN_S / 60 + 64)
static int bl_read(struct bl_pt *pt, long now)
{
    int n = bl_read_file(BL_PREV, pt, 0, BL_MAX_PTS, now - BL_SPAN_S);
    return bl_read_file(BL_FILE, pt, n, BL_MAX_PTS, now - BL_SPAN_S);
}

/* How fast it drains: the latest run off the charger of 20 minutes or
 * more, in tenths of a percent an hour; 0 when there is none yet. */
static int bl_rate_x10(const struct bl_pt *pt, int n, int *run_s, int *run_drop)
{
    int end = n - 1;
    while (end >= 0 && (pt[end].flags & BL_CHARGING)) end--;
    if (end < 0) return 0;
    int start = end;
    while (start > 0 && !(pt[start - 1].flags & BL_CHARGING) && pt[end].t - pt[start - 1].t <= 6 * 3600
           && pt[start].t - pt[start - 1].t <= 15 * 60) start--;   /* a gap of more than 15 min: off, not draining */
    int secs = pt[end].t - pt[start].t, drop = pt[start].level - pt[end].level;
    if (secs < 20 * 60 || drop <= 0) return 0;
    if (run_s) *run_s = secs;
    if (run_drop) *run_drop = drop;
    return (int)((int64_t)drop * 36000 / secs);
}

/* the graph: the level over the last day, the time it charged washed green */
struct bl_graph { struct bl_pt *pt; int n; long now; };
static void bl_graph_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    struct bl_graph *g = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a, full;
    lv_obj_get_content_coords(o, &a);    /* the data: inside, so the stroke's ends and the dot are whole */
    lv_obj_get_coords(o, &full);         /* the guide lines: edge to edge, with the words under them */
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    /* the 0, 50 and 100 % lines */
    for (int k = 0; k <= 2; k++)
    {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = lv_color_hex(C_SUB);
        d.opa = k == 1 ? 40 : 70;
        d.width = 1;
        d.dash_width = k == 1 ? 4 : 0;
        d.dash_gap = k == 1 ? 4 : 0;
        d.p1.x = full.x1; d.p2.x = full.x2;
        d.p1.y = d.p2.y = a.y1 + (h - 1) * k / 2;
        lv_draw_line(layer, &d);
    }
    if (!g || g->n < 1) return;
    long t0 = g->now - BL_SPAN_S;
#define BL_X(t) (a.x1 + (int)((int64_t)((t) - t0) * (w - 1) / BL_SPAN_S))
#define BL_Y(l) (a.y1 + (h - 1) - (h - 1) * (l) / 100)
    /* charging: a green wash behind the line */
    for (int i = 0; i < g->n; i++)
    {
        if (!(g->pt[i].flags & BL_CHARGING)) continue;
        int j = i;
        while (j + 1 < g->n && (g->pt[j + 1].flags & BL_CHARGING) && g->pt[j + 1].t - g->pt[j].t <= 15 * 60) j++;
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = lv_color_hex(0x3aa856);
        r.bg_opa = 46;
        r.radius = SL_R_MINI;
        lv_area_t band = { BL_X(g->pt[i].t), a.y1, LV_MAX(BL_X(g->pt[j].t), BL_X(g->pt[i].t) + 2), a.y2 };
        lv_draw_rect(layer, &r, &band);
        i = j;
    }
    /* the level: one stroke a run, broken where the player was off */
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(C_ACCENT_UI);
    d.width = 3;
    d.round_start = d.round_end = 1;
    for (int i = 1; i < g->n; i++)
    {
        if (g->pt[i].t - g->pt[i - 1].t > 15 * 60) continue;
        d.p1.x = BL_X(g->pt[i - 1].t); d.p1.y = BL_Y(g->pt[i - 1].level);
        d.p2.x = BL_X(g->pt[i].t); d.p2.y = BL_Y(g->pt[i].level);
        if (d.p1.x == d.p2.x && d.p1.y == d.p2.y) continue;
        lv_draw_line(layer, &d);
    }
    /* now: a dot at the last reading */
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(C_ACCENT_UI);
    r.radius = LV_RADIUS_CIRCLE;
    int x = BL_X(g->pt[g->n - 1].t), y = BL_Y(g->pt[g->n - 1].level);
    lv_area_t dot = { x - 5, y - 5, x + 5, y + 5 };
    lv_draw_rect(layer, &r, &dot);
#undef BL_X
#undef BL_Y
}
static void bl_graph_free(lv_event_t *e)
{
    struct bl_graph *g = lv_event_get_user_data(e);
    if (g) { free(g->pt); free(g); }
}
static void bl_switch(int on) { global_settings.sloppy_battery_log = on; }

/* Time and power's battery history: the graph, what it says, the switch */
static void bl_card(lv_obj_t *body)
{
    lv_obj_t *c = set_group(body, "Battery history");
    struct bl_graph *g = calloc(1, sizeof *g);
    long now = (long)time(NULL);
    if (g) g->pt = malloc(BL_MAX_PTS * sizeof *g->pt);
    if (g && g->pt) { g->now = now; g->n = bl_read(g->pt, now); }
    int n = g && g->pt ? g->n : 0;
    char b[160];
    if (n < 2)
        strlcpy(b, sl_tr(bl_on() ? "No history yet. A reading is kept every minute." : "The battery log is off."), sizeof b);
    else
    {
        const struct bl_pt *last = &g->pt[n - 1];
        int run_s = 0, drop = 0, rate = bl_rate_x10(g->pt, n, &run_s, &drop);
        if (last->flags & BL_CHARGING) snprintf(b, sizeof b, sl_tr("Charging, %d%% now"), last->level);
        else if (!rate) snprintf(b, sizeof b, sl_tr("%d%% now; not long enough off the charger to say how fast it drains"), last->level);
        else
        {
            int left_min = (int)((int64_t)last->level * 600 / rate);
            snprintf(b, sizeof b, sl_tr("About %d.%d%% an hour lately: %d h %02d min left at %d%%"),
                     rate / 10, rate % 10, left_min / 60, left_min % 60, last->level);
        }
    }
    /* the graph, with the day's ends under it */
    lv_obj_t *box_ = box(c);
    lv_obj_set_size(box_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(box_, SL_INSET, 0);
    lv_obj_set_style_pad_top(box_, SET_PAD_V, 0);
    lv_obj_set_flex_flow(box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *gr = box(box_);
    lv_obj_set_size(gr, lv_pct(100), 120);
    lv_obj_set_style_pad_all(gr, 6, 0);    /* the stroke's round ends and the dot, inside */
    if (g && !g->pt) { free(g); g = NULL; }
    lv_obj_add_event_cb(gr, bl_graph_draw, LV_EVENT_DRAW_MAIN, g);
    lv_obj_add_event_cb(gr, bl_graph_free, LV_EVENT_DELETE, g);
    lv_obj_t *ends = box(box_);
    lv_obj_set_size(ends, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(ends, SL_SP_XS, 0);
    lv_obj_set_flex_flow(ends, LV_FLEX_FLOW_ROW);   /* as tall as its words, in any script */
    lv_obj_set_flex_align(ends, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_base_dir(ends, LV_BASE_DIR_LTR, 0);   /* time runs left to right, as the graph does */
    /* room under the line for the deepest letters (Arabic's hang lower) */
    lv_obj_set_style_pad_bottom(ends, sl_line_h(&ss_regular_16) - lv_font_get_line_height(&ss_regular_16), 0);
    sl_label_deep(text(ends, "24 h ago", &ss_regular_16, C_SUB));
    sl_label_deep(text(ends, "Now", &ss_regular_16, C_SUB));
    /* what it says, under the graph, in the same row */
    lv_obj_t *say = text(box_, b, &ss_regular_18, C_TEXT);
    lv_obj_set_width(say, lv_pct(100));
    lv_label_set_long_mode(say, LV_LABEL_LONG_WRAP);
    set_align(say);
    sl_label_deep(say);
    lv_obj_set_style_pad_top(say, SL_SP_S, 0);
    lv_obj_set_style_pad_bottom(say, SET_PAD_V, 0);
    opt_switch(c, "Keep a battery log", bl_on(), bl_switch);
    opt_note(body, "The level and voltage every minute, in a file on the card a computer can open.");
}
