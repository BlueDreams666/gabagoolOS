/*
 *  Now Playing's waveform (Customization > Now Playing > Waveform, P19):
 *  the seek bar drawn as the song's own shape, the glass under it.
 *
 *  No second decoder runs beside playback, so the shape cannot be worked
 *  out ahead. The mixer hook the visualizer listens with keeps the loudest
 *  sample it has passed (sp_peak); four times a second (once with the
 *  screen dark) that peak goes into the columns the song moved through.
 *  The shape fills in as the song plays. A finished one (and one played in
 *  part, so a later play adds to it) is kept per song on the card, in 256
 *  small folders as the thumbnails are (a flat folder of thousands is slow
 *  on exFAT), so the next play shows all of it at once.
 *
 *  The slider stays the control: its own press, drag and release seek as
 *  before, its parts made clear and the bars drawn over its box. Only the
 *  columns that change, and the strip the played part moved across, are
 *  redrawn. One small block (WV_N bytes and the song's path) while the
 *  layout is chosen, given back when it is not.
 */

#define WV_N      86                     /* columns */
#define WV_PITCH  5                      /* a bar and its gap */
#define WV_BAR    3
#define WV_MIN    4                      /* a column not heard yet: a short stub */
#define WV_DB     36.0f                  /* the quietest a bar shows, under the loudest */
#define WV_DIR    ROCKBOX_DIR "/sloppy/waves"
#define WV_MAGIC  0x31565753u            /* "SWV1" */

struct wv_file { uint32_t magic, check, len; uint16_t n, pad; };
static struct
{
    uint8_t col[WV_N];                   /* 0 not heard, else 1..255: dB over 60 below full scale */
    char path[MAX_PATH];                 /* the song these are for */
    int cue;
    long len;                            /* its length, ms (a cue sheet's track: its own) */
    long last_ms;                        /* where the song was at the last look */
    long last_tick;
    uint32_t last_bufs;
    bool have_last, changed;
    int prog;                            /* the played part, of 1000, as last drawn */
    int grow;                            /* a cached shape rising in, of 256 */
} *wv;
static int wv_saves, wv_loads;           /* the harness counts these */

static bool np_wave(void) { return global_settings.sloppy_np_style == 3; }

/* FNV-1a over the path and the cue track: the file's name, and with
 * another start its check */
static uint32_t wv_hash(const char *path, int cue, uint32_t h)
{
    for (const char *p = path; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ (uint32_t)(cue + 1)) * 16777619u;
    return h;
}
static void wv_path(char *buf, int n, uint32_t key) { snprintf(buf, n, WV_DIR "/%02x/%08x.wv", key >> 24, key); }

static void wv_save(void)
{
    if (!wv || !wv->changed || !wv->path[0] || wv->len <= 0) return;
    wv->changed = false;
    uint32_t key = wv_hash(wv->path, wv->cue, 2166136261u);
    char p[64];
    mkdir(ROCKBOX_DIR "/sloppy");
    mkdir(WV_DIR);
    snprintf(p, sizeof p, WV_DIR "/%02x", key >> 24);
    mkdir(p);
    wv_path(p, sizeof p, key);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;                  /* USB mode, a full card: kept in memory for this play */
    struct wv_file h = { WV_MAGIC, wv_hash(wv->path, wv->cue, 0x811c9dc5u ^ 0x5bd1e995u), (uint32_t)wv->len, WV_N, 0 };
    bool ok = write(fd, &h, sizeof h) == (ssize_t)sizeof h && write(fd, wv->col, WV_N) == WV_N;
    close(fd);
    if (!ok) remove(p);
    else wv_saves++;
}

static void wv_grow_cb(void *v, int32_t g)
{
    (void)v;
    if (!wv) return;
    wv->grow = (int)g;
    if (np_seek) lv_obj_invalidate(np_seek);
}
static void wv_load(const char *path, int cue, long len)
{
    memset(wv->col, 0, sizeof wv->col);
    strlcpy(wv->path, path, sizeof wv->path);
    wv->cue = cue;
    wv->len = len;
    wv->changed = wv->have_last = false;
    wv->grow = 256;
    uint32_t key = wv_hash(path, cue, 2166136261u);
    char p[64];
    wv_path(p, sizeof p, key);
    int fd = open(p, O_RDONLY);
    if (fd >= 0)
    {
        struct wv_file h;
        uint8_t col[WV_N];
        bool ok = read(fd, &h, sizeof h) == (ssize_t)sizeof h && h.magic == WV_MAGIC && h.n == WV_N &&
                  h.check == wv_hash(path, cue, 0x811c9dc5u ^ 0x5bd1e995u) && h.len == (uint32_t)len &&
                  read(fd, col, WV_N) == WV_N;
        close(fd);
        if (ok)
        {
            memcpy(wv->col, col, WV_N);
            wv_loads++;
            /* the shape rises in, once, as the song arrives */
            if (np_open && np_seek && !lv_obj_has_flag(np, LV_OBJ_FLAG_HIDDEN))
            {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, wv);
                lv_anim_set_values(&a, 0, 256);
                lv_anim_set_duration(&a, SL_T_SLOW);
                lv_anim_set_exec_cb(&a, wv_grow_cb);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                wv->grow = 0;
                lv_anim_start(&a);
            }
        }
    }
    if (np_seek) lv_obj_invalidate(np_seek);
}

static void wv_free(void)
{
    if (!wv) return;
    lv_anim_delete(wv, NULL);
    wv_save();
    free(wv);
    wv = NULL;
    sp_listen_for(SP_FOR_WAVE, false);
}

/* the bars' box inside the control, from its left */
static int wv_x0(void) { return (lv_obj_get_width(np_seek) - (WV_N * WV_PITCH - (WV_PITCH - WV_BAR))) / 2; }
static void wv_inval_cols(int c0, int c1)
{
    if (!np_seek || !np_open) return;
    lv_area_t a;
    lv_obj_get_coords(np_seek, &a);
    int x = a.x1 + wv_x0();
    a.x1 = x + c0 * WV_PITCH;
    a.x2 = x + c1 * WV_PITCH + WV_BAR - 1;
    lv_obj_invalidate_area(np_seek, &a);
}

/* np_update, in place of setting the slider: only the strip the played
 * part moved across is drawn again */
static void wv_progress(int prog)
{
    if (!wv || prog == wv->prog || !np_seek) return;
    int w = lv_obj_get_width(np_seek);
    int a = wv->prog * w / 1000, b = prog * w / 1000;
    wv->prog = prog;
    if (a == b) return;
    lv_area_t c;
    lv_obj_get_coords(np_seek, &c);
    int x0 = c.x1 + (a < b ? a : b) - 1, x1 = c.x1 + (a < b ? b : a) + 1;
    c.x1 = x0; c.x2 = x1;
    lv_obj_invalidate_area(np_seek, &c);
}

/* a peak, 0..32768, as a column's byte: 60 dB over 254 steps */
static uint8_t wv_level(int peak)
{
    if (peak <= 33) return 1;            /* -60 dB and under: silence */
    float db = 20.0f * log10f((float)peak / 32768.0f);
    int v = 1 + (int)((db + 60.0f) * 254.0f / 60.0f + 0.5f);
    return (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
}

/* periodic, four times a second (once while dark): the song's loudest
 * since the last look, into the columns it moved through */
static void wv_poll(void)
{
    if (!np_wave()) { wv_free(); return; }
    struct mp3entry *id3 = audio_current_track();
    int st = audio_status();
    bool playing = id3 && (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    sp_listen_for(SP_FOR_WAVE, id3 && (st & AUDIO_STATUS_PLAY));
    if (!id3) return;
    if (!wv && !(wv = calloc(1, sizeof *wv))) return;
    long cs, cl;
    int cue = cue_now(id3, &cs, &cl);
    if (cl <= 0) cl = (long)id3->length;
    if (!strcmp(wv->path, id3->path) && wv->cue == cue && wv->len != cl && cl > 0)
        wv->len = cl;                    /* the same song, its length known better: its columns stay */
    if (strcmp(wv->path, id3->path) || wv->cue != cue)
    {
        wv_save();                       /* the song before: what it heard */
        wv_load(id3->path, cue, cl);
        sp_peak = 0;
        return;
    }
    if (!playing || cl <= 0) { wv->have_last = false; return; }
    /* where the mixer is, not where the speaker is: the hook hears it there */
    unsigned long el = id3->elapsed > id3->length ? id3->length : id3->elapsed;
    long at = (long)el - cs;
    if (at < 0) at = 0;
    if (at > cl) at = cl;
    long now = current_tick;
    int peak = sp_peak;
    sp_peak = 0;
    uint32_t bufs = sp_bufs;
    bool heard = bufs != wv->last_bufs;
    wv->last_bufs = bufs;
    if (!wv->have_last) { wv->have_last = true; wv->last_ms = at; wv->last_tick = now; return; }
    long moved = at - wv->last_ms, waited = (now - wv->last_tick) * 1000 / HZ;
    wv->last_ms = at;
    wv->last_tick = now;
    if (moved < 0 || moved > waited + 1500 || !heard) return;   /* a seek, or nothing came through */
    int c0 = (int)((int64_t)(at - moved) * WV_N / cl), c1 = (int)((int64_t)at * WV_N / cl);
    if (c1 >= WV_N) c1 = WV_N - 1;
    if (c0 > c1) c0 = c1;
    uint8_t v = wv_level(peak);
    int d0 = WV_N, d1 = -1;
    for (int c = c0; c <= c1; c++)
        if (wv->col[c] < v)
        {
            wv->col[c] = v;
            wv->changed = true;
            if (c < d0) d0 = c;
            d1 = c;
        }
    if (d1 >= 0) wv_inval_cols(d0, d1);
    if (wv->changed)
    {
        int n = 0;
        for (int c = 0; c < WV_N; c++) n += wv->col[c] != 0;
        if (n == WV_N) wv_save();        /* whole: kept now, not only when the song ends */
    }
}

/* the bars, over the control's clear box: the played part in the seek
 * bar's own colour (the ink, or white on the glass), the rest as its track */
static void wv_draw_cb(lv_event_t *e)
{
    if (!np_wave() || !wv) return;
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t box;
    lv_obj_get_coords(o, &box);
    const lv_area_t *clip = &layer->_clip_area;
    int w = lv_area_get_width(&box), h = lv_area_get_height(&box);
    bool drag = lv_slider_is_dragged(o);
    int prog = drag ? lv_slider_get_value(o) : wv->prog;
    int px = box.x1 + prog * w / 1000;
    int top = 0;
    for (int c = 0; c < WV_N; c++) if (wv->col[c] > top) top = wv->col[c];
    float top_db = top ? (top - 1) * 60.0f / 254.0f - 60.0f : 0;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = sl_zune() ? 0 : LV_RADIUS_CIRCLE;   /* Zule: square */
    lv_color_t ink = lv_obj_get_style_bg_color(o, LV_PART_INDICATOR);
    lv_color_t rest = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    lv_opa_t rest_opa = (lv_opa_t)(sl_theme() == 1 ? 72 : 96);
    int x = box.x1 + wv_x0();
    for (int c = 0; c < WV_N; c++)
    {
        lv_area_t a = { x + c * WV_PITCH, 0, x + c * WV_PITCH + WV_BAR - 1, 0 };
        if (a.x2 < clip->x1 || a.x1 > clip->x2) continue;
        int bh = WV_MIN;
        if (wv->col[c])
        {
            float db = (wv->col[c] - 1) * 60.0f / 254.0f - 60.0f;
            float f = (db - (top_db - WV_DB)) / WV_DB;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            bh = WV_MIN + (int)((h - WV_MIN) * f * f * wv->grow / 256);
        }
        bh &= ~1;                        /* even: the bar sits on the middle line */
        a.y1 = box.y1 + (h - bh) / 2;
        a.y2 = a.y1 + bh - 1;
        bool known = wv->col[c] != 0;
        /* the rest: heard columns as the track, the ones still to come quieter */
        d.bg_color = rest;
        d.bg_opa = known ? rest_opa : (lv_opa_t)(rest_opa / 2);
        if (a.x1 >= px) { lv_draw_rect(layer, &d, &a); continue; }
        d.bg_color = ink;
        d.bg_opa = LV_OPA_COVER;
        if (a.x2 < px) { lv_draw_rect(layer, &d, &a); continue; }
        /* the bar the played part ends in: split where it is */
        lv_area_t keep = layer->_clip_area, left = keep;
        if (left.x2 > px - 1) left.x2 = px - 1;
        layer->_clip_area = left;
        lv_draw_rect(layer, &d, &a);
        lv_area_t right = keep;
        if (right.x1 < px) right.x1 = px;
        layer->_clip_area = right;
        d.bg_color = rest;
        d.bg_opa = known ? rest_opa : (lv_opa_t)(rest_opa / 2);
        lv_draw_rect(layer, &d, &a);
        layer->_clip_area = keep;
    }
    if (drag)
    {
        /* under the finger: a slim line where it will play from */
        lv_area_t a = { px - 1, box.y1 - 4, px, box.y2 + 4 };
        d.bg_color = ink;
        d.bg_opa = LV_OPA_COVER;
        d.radius = sl_zune() ? 0 : 1;
        lv_draw_rect(layer, &d, &a);
    }
}

/* np_paint, after the sliders are coloured: the seek slider's parts made
 * clear and its box as tall as the bars, or the thin track back */
static void wv_apply(void)
{
    if (!np_seek) return;
    bool on = np_wave();
    lv_obj_set_height(np_seek, on ? WV_H : NP_TRACK_H);
    lv_obj_set_style_opa(np_seek, on ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(np_seek, on ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
    if (on) lv_obj_set_style_bg_opa(np_seek, LV_OPA_TRANSP, LV_PART_MAIN);
    if (!on) wv_free();
    else if (wv) { wv->prog = -1; lv_obj_invalidate(np_seek); }
}
