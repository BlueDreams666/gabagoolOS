/* GabagoolOS Skip Graveyard. GPLv2+. Included by sloppy.c after the history.
 *
 * hist_poll notices skips and plays (sloppy_grave_state.h has the rules)
 * and passes them here. Songs with three skips get a tombstone on a
 * moonlit page, with the pet's ghost drifting over the fence. Tap a stone
 * to bring the song back (its skips are forgiven and it plays next) or to
 * haunt it: the ghost then skips it for you whenever it comes up. */
#include "sloppy_grave_state.h"

#define GRAVE_FILE ROCKBOX_DIR "/sloppy/graves.dat"
#define GRAVE_SW 150                 /* the scene, in LCD pixels */
#define GRAVE_SH 60
#define GRAVE_SCALE 3
#define GRAVE_BG 0x15111f

static struct sl_graves graves;
static bool graves_loaded;
static int grave_auto_skips;         /* haunted songs skipped in a row */
static bool grave_dirty;             /* skips not yet written */
static uint32_t grave_dirty_at;
static lv_obj_t *grave_page_obj, *grave_canvas, *grave_list;
static uint16_t *grave_buf, *grave_prev;   /* the scene, and one pixel per LCD dot last frame */
static lv_timer_t *grave_timer;
static int grave_sheet_track = -1;

static void graves_load(void)
{
    if (graves_loaded) return;
    graves_loaded = true;
    int fd = open(GRAVE_FILE, O_RDONLY);
    bool ok = fd >= 0 && read(fd, &graves, sizeof graves) == (ssize_t)sizeof graves &&
              sl_graves_valid(&graves);
    if (fd >= 0) close(fd);
    if (!ok) sl_graves_init(&graves);
}

static void graves_save(void)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(GRAVE_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, &graves, sizeof graves) == (ssize_t)sizeof graves && fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(GRAVE_FILE ".tmp", GRAVE_FILE) < 0) remove(GRAVE_FILE ".tmp");
}

/* ---- from hist_poll ----------------------------------------------------- */
/* A skip is when the next song is loading: write the table a few seconds
 * later (or on the way out), not with an fsync right then. */
static void grave_changed(void) { if (!grave_dirty) { grave_dirty = true; grave_dirty_at = sl_ms(); } }

static void grave_flush(bool now)
{
    if (!grave_dirty || (!now && sl_ms() - grave_dirty_at < 5000)) return;
    grave_dirty = false;
    graves_save();
}

static void grave_skipped(uint32_t hash)
{
    graves_load();
    sl_grave_skip(&graves, hash);
    grave_changed();
}

static void grave_played(uint32_t hash)
{
    graves_load();
    if (sl_grave_played(&graves, hash)) grave_changed();
}

/* A song just started. A haunted one is skipped, a few in a row at most, so
 * a queue of nothing but haunted songs still plays. */
static void grave_started(uint32_t hash)
{
    graves_load();
    if (!sl_grave_haunted(&graves, hash)) { grave_auto_skips = 0; return; }
    if (grave_auto_skips >= 5 || playlist_amount() < 2) return;
    grave_auto_skips++;
    sl_log("graveyard: skipped a haunted song");
    audio_next();
    sfx_play(SFX_PET_GHOST);
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "ghost", "BOO! The ghost skipped a haunted song.");
}

/* ---- the moonlit scene ---------------------------------------------------- */
static void grave_scene(unsigned t)
{
    uint16_t *old_tgt = pet_tgt;
    int old_w = pet_tgt_w, old_h = pet_tgt_h, old_s = pet_tgt_s;
    pet_target(grave_buf, GRAVE_SW, GRAVE_SH, GRAVE_SCALE);
    pet_ink[0] = lv_color_to_u16(lv_color_hex(0x2a2250));   /* sky */
    pet_ink[1] = lv_color_to_u16(lv_color_hex(0x0d0a18));   /* night */
    pet_ink[2] = lv_color_to_u16(lv_color_hex(0x8e84c0));   /* stone, ghost */
    pet_ink[3] = lv_color_to_u16(lv_color_hex(0xece6ff));   /* moonlight */

    pet_px(0, 0, GRAVE_SW, GRAVE_SH, 0);
    static const uint8_t sx[] = { 8, 27, 44, 71, 90, 104, 139, 18, 60, 118 };
    static const uint8_t sy[] = { 6, 14, 4, 9, 3, 16, 24, 26, 20, 5 };
    for (unsigned i = 0; i < sizeof sx; i++)
        if ((t / 4 + i * 3) % 11) pet_px(sx[i], sy[i], 1, 1, (t / 6 + i) % 5 ? 2 : 3);
    pet_disc(128, 13, 8, 3);                       /* a crescent moon */
    pet_disc(132, 10, 7, 0);
    /* hills, fence, three stones and a dead tree */
    pet_px(0, 49, GRAVE_SW, 11, 1);
    for (int x = 0; x < GRAVE_SW; x++)
    {
        int bump = (x * 7 / 13) % 9;
        if (bump < 4) pet_px(x, 47 + (bump & 1), 1, 2, 1);
    }
    pet_px(0, 40, GRAVE_SW, 1, 1);
    pet_px(0, 44, GRAVE_SW, 1, 1);
    for (int x = 2; x < GRAVE_SW; x += 7) { pet_px(x, 36, 2, 13, 1); pet_px(x, 35, 1, 1, 1); }
    static const uint8_t gx[] = { 14, 58, 104 };
    for (int i = 0; i < 3; i++)
    {
        int x = gx[i];
        pet_px(x, 36, 14, 14, 2); pet_px(x + 2, 34, 10, 2, 2); pet_px(x + 4, 33, 6, 1, 2);
        pet_px(x + 6, 38, 2, 7, 1); pet_px(x + 4, 40, 6, 2, 1);
        pet_px(x - 2, 50, 18, 1, 1);
    }
    pet_line(140, 49, 140, 30, 1); pet_line(140, 36, 146, 30, 1); pet_line(140, 40, 134, 33, 1);
    /* ground fog drifts left to right */
    for (int k = 0; k < 7; k++)
    {
        int x = (int)((t / 2 + k * 29) % (GRAVE_SW + 24)) - 20;
        pet_px(x, 51 + (k % 3) * 3, 14 + (k % 2) * 8, 1, 2);
    }
    /* the pet's ghost wanders over the graves */
    unsigned wander = (t / 3) % 160;
    int gxp = 20 + (int)(wander < 80 ? wander : 160 - wander);
    pet_hab_ghost(gxp, 4, t);

    pet_inks_init();
    pet_target(old_tgt, old_w, old_h, old_s);
}

/* Repaint only what moved, a band of rows at a time: the ghost, the fog
 * and a few stars change, not the whole scene. */
static void grave_invalidate_changed(void)
{
    lv_area_t at;
    lv_obj_get_coords(grave_canvas, &at);
    for (int band = 0; band < GRAVE_SH; band += 12)
    {
        int x1 = GRAVE_SW, x2 = -1;
        for (int y = band; y < band + 12 && y < GRAVE_SH; y++)
        {
            const uint16_t *row = grave_buf + (size_t)y * GRAVE_SCALE * GRAVE_SW * GRAVE_SCALE;
            uint16_t *prev = grave_prev + y * GRAVE_SW;
            for (int x = 0; x < GRAVE_SW; x++)
            {
                uint16_t c = row[x * GRAVE_SCALE];
                if (prev[x] == c) continue;
                prev[x] = c;
                if (x < x1) x1 = x;
                if (x > x2) x2 = x;
            }
        }
        if (x2 < 0) continue;
        int y2 = band + 12 > GRAVE_SH ? GRAVE_SH : band + 12;
        lv_area_t a = { at.x1 + x1 * GRAVE_SCALE, at.y1 + band * GRAVE_SCALE,
                        at.x1 + x2 * GRAVE_SCALE + GRAVE_SCALE - 1, at.y1 + y2 * GRAVE_SCALE - 1 };
        lv_obj_invalidate_area(grave_canvas, &a);
    }
}

static void grave_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!grave_canvas || !npages || pages[npages - 1] != grave_page_obj) return;
    grave_scene(lv_tick_get() / 100);
    grave_invalidate_changed();
}

/* ---- the stones ---------------------------------------------------------- */
static void grave_fill(void);

static lv_obj_t *grave_button(lv_obj_t *parent, const char *label, uint32_t bg, uint32_t ink,
                              lv_event_cb_t cb, intptr_t k)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, lv_pct(100), 72);
    lv_obj_set_style_radius(b, SL_R_LG, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *l = text(b, label, &ss_semibold_23, ink);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    lv_obj_center(l);
    on_click(b, cb, (void *)k);
    return b;
}

static void grave_choice_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int t = grave_sheet_track;
    if (t < 0 || t >= sl_lib.n_tracks) { page_pop(); return; }
    uint32_t hash = sl_track_hist_hash(t);
    graves_load();
    if (k == 0)
    {
        sl_grave_resurrect(&graves, hash);
        grave_dirty = false;
        graves_save();
        sfx_play(SFX_PET_HATCH);
        bool playing = something_playing();
        q_play_next(t);
        if (playing) toast("IT'S ALIVE! It plays next.");
    }
    else
    {
        bool on = !sl_grave_haunted(&graves, hash);
        sl_grave_haunt(&graves, hash, on);
        grave_dirty = false;
        graves_save();
        sfx_play(on ? SFX_PET_GHOST : SFX_PET_CALM);
        toast(on ? "Haunted. The ghost will skip it for you." : "The ghost lets it rest.");
    }
    page_pop();
    grave_fill();
}

static void grave_sheet(int t)
{
    graves_load();
    const struct sl_track *tr = &sl_lib.tracks[t];
    bool haunted = sl_grave_haunted(&graves, sl_track_hist_hash(t));
    grave_sheet_track = t;
    lv_obj_t *p, *body = page_new("Here lies...", &p);
    chance_page_skin(p, GRAVE_BG, 0xece6ff);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_t *t1 = text(body, tr->title, &ss_bold_28, 0xece6ff);
    lv_obj_set_width(t1, lv_pct(100));
    lv_label_set_long_mode(t1, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(t1, lv_color_hex(0xece6ff), 0);
    lv_obj_t *t2 = text(body, sl_lib.albums[tr->album].artist, &ss_semibold_21, 0xb3a9dc);
    lv_obj_set_width(t2, lv_pct(100));
    label_lines(t2, 1);
    lv_obj_t *t3 = text(body, haunted ?
        "Haunted. Whenever it comes up, the ghost skips it for you." :
        "You kept skipping it, so it was laid to rest here.", &ss_regular_18, 0x8e84c0);
    lv_obj_set_width(t3, lv_pct(100));
    lv_label_set_long_mode(t3, LV_LABEL_LONG_WRAP);
    grave_button(body, "BRING IT BACK", 0xb6f0a0, 0x16301a, grave_choice_cb, 0);
    grave_button(body, haunted ? "STOP HAUNTING" : "HAUNT IT", 0x3b3160, 0xece6ff, grave_choice_cb, 1);
    page_push(p);
}

static void grave_stone_cb(lv_event_t *e) { grave_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

static int grave_hash_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct sl_grave *)a)->hash, y = ((const struct sl_grave *)b)->hash;
    return (x > y) - (x < y);
}

/* The buried songs of the library, most skipped first. */
static int grave_buried(int *out, int max)
{
    graves_load();
    static struct sl_grave sorted[SL_GRAVE_MAX];
    int n = 0, count = 0;
    for (int i = 0; i < graves.n; i++) if (sl_grave_buried(&graves.g[i])) sorted[n++] = graves.g[i];
    if (!n) return 0;
    qsort(sorted, n, sizeof *sorted, grave_hash_cmp);
    static uint16_t skips_of[SL_GRAVE_MAX];
    for (int t = 0; t < sl_lib.n_tracks && count < max; t++)
    {
        struct sl_grave want = { .hash = sl_track_hist_hash(t) };
        const struct sl_grave *g = bsearch(&want, sorted, n, sizeof *sorted, grave_hash_cmp);
        if (!g) continue;
        /* insertion by skips: the list is at most SL_GRAVE_MAX long */
        int at = count++;
        while (at > 0 && skips_of[at - 1] < g->skips)
        { out[at] = out[at - 1]; skips_of[at] = skips_of[at - 1]; at--; }
        out[at] = t; skips_of[at] = g->skips;
        if ((t & 1023) == 1023) yield();
    }
    return count;
}

static void grave_stone(lv_obj_t *parent, int t)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    int i = sl_grave_find(&graves, sl_track_hist_hash(t));
    bool haunted = i >= 0 && graves.g[i].haunted;
    int w = (W - 28 - 14) / 2, h = 236;
    uint32_t stone = haunted ? 0x6c5ea8 : 0x57536a, ink = 0xf2efff;
    lv_obj_t *s = box(parent);
    lv_obj_set_size(s, w, h);
    lv_obj_t *top = box(s);                  /* a round-topped stone: a disc on a slab */
    lv_obj_set_size(top, w - 16, w - 16);
    lv_obj_set_pos(top, 8, 0);
    lv_obj_set_style_radius(top, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *slab = box(s);
    lv_obj_set_size(slab, w - 16, h - (w - 16) / 2 - 16);
    lv_obj_set_pos(slab, 8, (w - 16) / 2);
    lv_obj_set_style_radius(slab, 4, 0);
    lv_obj_t *parts[2] = { top, slab };
    for (int k = 0; k < 2; k++)
    {
        lv_obj_set_style_bg_color(parts[k], lv_color_hex(stone), 0);
        lv_obj_set_style_bg_opa(parts[k], LV_OPA_COVER, 0);
    }
    lv_obj_t *grass = box(s);
    lv_obj_set_size(grass, w, 20);
    lv_obj_set_pos(grass, 0, h - 20);
    lv_obj_set_style_radius(grass, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(grass, lv_color_hex(0x24402c), 0);
    lv_obj_set_style_bg_opa(grass, LV_OPA_COVER, 0);

    lv_obj_t *rip = text(s, haunted ? "BOO!" : "R.I.P.", &ss_bold_28, ink);
    lv_obj_set_style_text_color(rip, lv_color_hex(haunted ? 0xd9ccff : 0x2c2838), 0);
    lv_obj_align(rip, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *title = text(s, tr->title, &ss_semibold_21, ink);
    lv_obj_set_style_text_color(title, lv_color_hex(ink), 0);
    lv_obj_set_width(title, w - 40);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(title, 2);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_t *artist = text(s, sl_lib.albums[tr->album].artist, &ss_regular_18, 0xd4cfe6);
    lv_obj_set_style_text_color(artist, lv_color_hex(0xd4cfe6), 0);
    lv_obj_set_width(artist, w - 40);
    lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(artist, 1);
    lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, 136);
    char b[40];
    unsigned skips = i >= 0 ? graves.g[i].skips : 0;
    if (haunted) strlcpy(b, sl_tr("HAUNTED"), sizeof b);
    else snprintf(b, sizeof b, sl_tr("SKIPPED %u TIMES"), skips);
    lv_obj_t *epitaph = text(s, b, &ss_semibold_18, 0x2c2838);
    lv_obj_set_style_text_color(epitaph, lv_color_hex(haunted ? 0xd9ccff : 0x2c2838), 0);
    lv_obj_align(epitaph, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_opa(s, LV_OPA_70, LV_STATE_PRESSED);
    on_click(s, grave_stone_cb, (void *)(intptr_t)t);
}

static void grave_fill(void)
{
    if (!grave_list) return;
    lv_obj_clean(grave_list);
    static int buried[SL_GRAVE_MAX];
    int n = grave_buried(buried, SL_GRAVE_MAX);
    char b[64];
    if (!n)
    {
        /* the empty pattern in the graveyard's own night colours */
        lv_obj_t *e = sl_empty(grave_list, IC_GRAVE, 96, "Nothing buried yet",
                               "Songs you skip three times end up here, and a song you play all the way through climbs back out.", 40);
        lv_obj_set_style_text_color(lv_obj_get_child(e, 0), lv_color_hex(0x6c5ea8), 0);
        lv_obj_set_style_bg_color(lv_obj_get_child(e, 0), lv_color_hex(GRAVE_BG), 0);   /* its cut-outs */
        lv_obj_set_style_text_color(lv_obj_get_child(e, 1), lv_color_hex(0xece6ff), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(e, 2), lv_color_hex(0xb3a9dc), 0);
        return;
    }
    sl_count(b, sizeof b, n, "%s SONG LAID TO REST", "%s SONGS LAID TO REST");
    lv_obj_t *s = text(grave_list, b, &ss_semibold_18, 0x8e84c0);
    lv_obj_set_style_text_color(s, lv_color_hex(0x8e84c0), 0);
    lv_obj_set_style_text_letter_space(s, 2, 0);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_style_pad_all(s, 6, 0);
    for (int i = 0; i < n; i++) grave_stone(grave_list, buried[i]);
}

static void grave_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != grave_page_obj) return;
    if (grave_timer) { lv_timer_delete(grave_timer); grave_timer = NULL; }
    free(grave_buf);
    free(grave_prev);
    grave_buf = grave_prev = NULL;
    grave_page_obj = grave_canvas = grave_list = NULL;
}

static void grave_page(void)
{
    if (page_bring_back(grave_page_obj)) return;
    lv_obj_t *p, *body = page_new("Skip Graveyard", &p);
    grave_page_obj = p;
    chance_page_skin(p, GRAVE_BG, 0xece6ff);
    lv_obj_add_event_cb(p, grave_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(body, 10, 0);
    grave_buf = malloc(GRAVE_SW * GRAVE_SH * GRAVE_SCALE * GRAVE_SCALE * sizeof *grave_buf);
    grave_prev = calloc(GRAVE_SW * GRAVE_SH, sizeof *grave_prev);
    if (!grave_prev) { free(grave_buf); grave_buf = NULL; }
    if (grave_buf)
    {
        pet_load();
        lv_obj_t *frame = box(body);
        lv_obj_set_size(frame, GRAVE_SW * GRAVE_SCALE, GRAVE_SH * GRAVE_SCALE);
        lv_obj_set_style_radius(frame, SL_R_LG, 0);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_set_style_align(frame, LV_ALIGN_TOP_MID, 0);
        grave_canvas = lv_canvas_create(frame);
        lv_canvas_set_buffer(grave_canvas, grave_buf, GRAVE_SW * GRAVE_SCALE, GRAVE_SH * GRAVE_SCALE,
                             LV_COLOR_FORMAT_RGB565);
        grave_scene(lv_tick_get() / 100);
        grave_timer = lv_timer_create(grave_tick, 100, NULL);
    }
    lv_obj_t *blurb = text(body, "Songs you keep skipping rest here. Tap a stone to bring one "
                           "back, or haunt it and the ghost will skip it for you.",
                           &ss_regular_18, 0xb3a9dc);
    lv_obj_set_style_text_color(blurb, lv_color_hex(0xb3a9dc), 0);
    lv_obj_set_width(blurb, lv_pct(100));
    lv_obj_set_style_text_align(blurb, LV_TEXT_ALIGN_CENTER, 0);   /* the caption of the scene above it */
    lv_label_set_long_mode(blurb, LV_LABEL_LONG_WRAP);
    grave_list = box(body);
    lv_obj_set_size(grave_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grave_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grave_list, 14, 0);
    lv_obj_set_style_pad_row(grave_list, 14, 0);
    grave_fill();
    page_push(p);
}
