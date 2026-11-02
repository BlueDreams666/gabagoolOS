/*
 *  Gabagool tile: a man at a home bar, pointing at the counter (the photo
 *  in art/gabagool.jpg, embedded by art/make_gabagool.py), captioned
 *  "Gabagool? Ova haaaa!". A tap squishes the photo, which wobbles back,
 *  and someone at the table says something about it.
 *
 *  The voice: a 16-bit PCM WAV at .rockbox/sloppy/gabagool.wav (mono or
 *  stereo, any rate, up to 2 MB) is played on the sound effects channel at
 *  the sound effects volume, and the music dips for it the way it does for
 *  a game's sounds. GabagoolOS ships no such recording, so without the file a
 *  tap just gets a chomp. A tap while the clip is talking only wobbles the
 *  photo: one voice at a time, never a pile of them.
 *
 *  The picture: with .rockbox/sloppy/gabagool.vid beside it, the tap also
 *  opens a small theater over Start: the page dims and the clip plays in a
 *  card, in step with its voice. The file is raw RGB565 frames after a 12
 *  byte header ("SLV1", width, height, frames per second x 100, frames, all
 *  u16), so a frame is one read and one copy: no decoder on the player.
 *  Frames are read one at a time as they are due, never all at once.
 */

#define GB_CLIP     ROCKBOX_DIR "/sloppy/gabagool.wav"
#define GB_CLIP_MAX (2 * 1024 * 1024)
#define GB_VIDEO    ROCKBOX_DIR "/sloppy/gabagool.vid"
#define GB_VIDEO_W  448                  /* the most the card holds */
#define GB_VIDEO_H  300
#define GB_CHUNK    512
#define GB_LINE     "Gabagool? Ova haaaa!"   /* the caption on the photo */

#include "art/gabagool_photo.inc"

static struct
{
    uint8_t *file;
    const int16_t *pcm;
    uint32_t frames, step;               /* step: frames, Q16 */
    uint64_t pos;                        /* Q16 too: 32 bits only reach 65536 frames,
                                          * and the 3 s clip wrapped to 45 ms of scratch */
    int ch, vol;
    uint32_t ms, rate;
    bool tried;
    volatile bool playing;
} gb_clip;
static int16_t gb_buf[2][GB_CHUNK * 2] __attribute__((aligned(16)));
static int gb_buf_i;

static struct { lv_obj_t *art, *bubble, *quip; int n; } gb;

static uint32_t gb_le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    while (n--) v = v << 8 | p[n];
    return v;
}

/* Read the clip once; false (and a line in the log) when there is none. */
static bool gb_clip_load(void)
{
    if (gb_clip.pcm || gb_clip.tried) return gb_clip.pcm != NULL;
    gb_clip.tried = true;
    int fd = open(GB_CLIP, O_RDONLY);
    if (fd < 0) { sl_log("gabagool: no clip at " GB_CLIP); return false; }
    off_t len = lseek(fd, 0, SEEK_END);
    uint8_t *f = len > 44 && len <= GB_CLIP_MAX ? malloc((size_t)len) : NULL;
    bool ok = f && lseek(fd, 0, SEEK_SET) == 0 && read(fd, f, (size_t)len) == len;
    close(fd);
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0, bytes = 0;
    const uint8_t *data = NULL;
    if (ok && !memcmp(f, "RIFF", 4) && !memcmp(f + 8, "WAVE", 4))
        for (off_t p = 12; p + 8 <= len; )
        {
            uint32_t n = gb_le(f + p + 4, 4);
            if (!memcmp(f + p, "fmt ", 4) && n >= 16 && p + 24 <= len)
            {
                fmt = gb_le(f + p + 8, 2); ch = gb_le(f + p + 10, 2);
                rate = gb_le(f + p + 12, 4); bits = gb_le(f + p + 22, 2);
            }
            else if (!memcmp(f + p, "data", 4))
            {
                data = f + p + 8;   /* chunks are word aligned: so are the samples */
                bytes = n < (uint32_t)(len - p - 8) ? n : (uint32_t)(len - p - 8);
                break;
            }
            p += 8 + n + (n & 1);
        }
    if (!data || fmt != 1 || bits != 16 || (ch != 1 && ch != 2) || rate < 8000 || rate > 192000 ||
        bytes < ch * 2 * 2)
    {
        sl_log("gabagool: " GB_CLIP " is not a 16-bit PCM WAV");
        free(f);
        return false;
    }
    gb_clip.file = f;
    gb_clip.pcm = (const int16_t *)data;
    gb_clip.ch = (int)ch;
    gb_clip.frames = bytes / (ch * 2);
    gb_clip.ms = (uint32_t)((uint64_t)gb_clip.frames * 1000 / rate);
    gb_clip.rate = rate;
    return true;
}

/* The mixer's thread: the next chunk, resampled to the mixer's rate. */
static void gb_get_more(const void **start, size_t *size)
{
    uint64_t last = (uint64_t)(gb_clip.frames - 1) << 16;
    if (!gb_clip.playing || gb_clip.pos >= last) { gb_clip.playing = false; return; }
    int16_t *out = gb_buf[gb_buf_i ^= 1];
    int n = 0;
    for (; n < GB_CHUNK && gb_clip.pos < last; n++, gb_clip.pos += gb_clip.step)
    {
        uint32_t i = (uint32_t)(gb_clip.pos >> 16);
        int t = (int)(gb_clip.pos & 0xffff) >> 4;
        const int16_t *a = gb_clip.pcm + i * gb_clip.ch, *b = a + gb_clip.ch;
        int l = a[0] + ((b[0] - a[0]) * t >> 12);
        int r = gb_clip.ch == 2 ? a[1] + ((b[1] - a[1]) * t >> 12) : l;
        out[n * 2] = (int16_t)(l * gb_clip.vol / 100);
        out[n * 2 + 1] = (int16_t)(r * gb_clip.vol / 100);
    }
    *start = out;
    *size = (size_t)n * 4;
}

/* Still talking? A game's sound may have taken the channel since. */
static bool gb_talking(void)
{
    int left;
    const int16_t *cur = mixer_channel_get_buffer(PCM_MIXER_CHAN_BEEP, &left);
    return gb_clip.playing && mixer_channel_status(PCM_MIXER_CHAN_BEEP) == CHANNEL_PLAYING &&
           cur >= gb_buf[0] && cur < gb_buf[0] + sizeof gb_buf / sizeof gb_buf[0][0];
}

/* Say it, if there is a clip and nothing is saying it already. */
static void gb_say(void)
{
    if (!gb_clip_load()) { sfx_play_vary(SFX_PET_CHOMP); return; }
    if (gb_talking() || !sfx_enabled()) return;
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    /* the mixer runs at the song's rate, which changes from song to song */
    uint32_t fs = mixer_get_frequency();
    if (fs < 8000) fs = 44100;
    pcm_play_lock();
    gb_clip.step = (uint32_t)(((uint64_t)gb_clip.rate << 16) / fs);
    gb_clip.pos = 0;
    gb_clip.vol = sfx_volume_pct();
    gb_clip.playing = true;
    /* taps and ticks wait for it; a game's own sounds still come first */
    sfx_playing_prio = P_NORMAL;
    sfx_playing_until = sl_ms() + gb_clip.ms;
    pcm_play_unlock();
    beep_duck(gb_clip.ms, global_settings.sound_duck_cue);
    const void *start = NULL;
    size_t size = 0;
    gb_get_more(&start, &size);
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_BEEP, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_BEEP, gb_get_more, start, size);
}

/* Done with the clip: it is only freed once the mixer has let go of it. */
static void gb_clip_free(void)
{
    if (gb_talking()) mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    gb_clip.playing = false;
    free(gb_clip.file);
    memset(&gb_clip, 0, sizeof gb_clip);
}

/* ------------------------------------------------------------- the art */
static void gb_gingham_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int period = lv_area_get_width(&a) < 150 ? 18 : 26;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(0xd23a40);
    d.bg_opa = 70;
    for (int x = a.x1 + period / 4; x <= a.x2; x += period)
    {
        lv_area_t s = { x, a.y1, x + period / 2 - 1, a.y2 };
        lv_draw_rect(lv_event_get_layer(e), &d, &s);
    }
    for (int y = a.y1 + period / 4; y <= a.y2; y += period)
    {
        lv_area_t s = { a.x1, y, a.x2, y + period / 2 - 1 };
        lv_draw_rect(lv_event_get_layer(e), &d, &s);
    }
}

/* The photo, at one of the sizes it is embedded at: 219 (medium and wide
 * tiles), 104 (small) or 58 (lists). The smaller two are cropped to his
 * face. Drawn 1:1, never scaled, so it costs a copy. */
static lv_obj_t *gb_photo(lv_obj_t *parent, int size)
{
    static lv_image_dsc_t dsc[3];
    int k = size >= 219 ? 0 : size >= 104 ? 1 : 2;
    static const uint16_t *const px[3] = { gb_photo_219, gb_photo_104, gb_photo_58 };
    static const int side[3] = { 219, 104, 58 };
    if (!dsc[k].data)
    {
        dsc[k].header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc[k].header.cf = LV_COLOR_FORMAT_RGB565;
        dsc[k].header.w = dsc[k].header.h = side[k];
        dsc[k].header.stride = side[k] * 2;
        dsc[k].data = (const uint8_t *)px[k];
        dsc[k].data_size = (uint32_t)side[k] * side[k] * 2;
    }
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &dsc[k]);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    return img;
}

/* ------------------------------------------------------------- the tap */
static void gb_set_squish(void *o, int32_t v)
{
    /* a squash that springs back: down to 78 %, then a fading wobble */
    float t = v / 1000.0f, k = 0.22f * (float)exp(-4.6f * t) * cosf(9.4f * t);
    lv_image_set_scale_y(o, (uint32_t)(256 * (1 - k)));
    lv_image_set_scale_x(o, (uint32_t)(256 * (1 + k * 0.7f)));
}
static void gb_set_bubble(void *o, int32_t v)
{
    /* pops in over the first fifth, stays, fades over the last third */
    int scale = v < 140 ? 140 + 136 * v / 140 : v < 200 ? 276 - 20 * (v - 140) / 60 : 256;
    lv_obj_set_style_transform_scale(o, scale, 0);
    lv_obj_set_style_opa(o, v < 700 ? LV_OPA_COVER : (lv_opa_t)(255 * (1000 - v) / 300), 0);
    lv_obj_set_style_translate_y(o, -(v / 60), 0);
}

/* ---------------------------------------------------------- the theater */
#define GB_RADIUS 18
static struct
{
    lv_obj_t *scrim, *card, *canvas, *bar;
    lv_timer_t *timer;
    uint16_t *px;
    int fd, w, h, cw, fps100, frames, shown;   /* cw: the card, no narrower than its words */
    uint32_t started, ended;
    bool leaving;
    uint8_t inset[GB_RADIUS];            /* the rounded corners, row by row */
} gbv = { .fd = -1 };

static bool gb_theater_open(void) { return gbv.scrim != NULL; }
static void gbv_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void gbv_set_rise(void *o, int32_t v)
{
    lv_obj_set_style_translate_y(o, v, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)(255 - v * 255 / 40), 0);
}

static void gbv_close(bool hush)
{
    if (!gbv.scrim) return;
    if (hush && gb_talking()) mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    if (gbv.timer) lv_timer_delete(gbv.timer);
    lv_anim_delete(gbv.scrim, NULL);
    lv_anim_delete(gbv.card, NULL);
    lv_obj_delete(gbv.scrim);
    if (gbv.fd >= 0) close(gbv.fd);
    free(gbv.px);
    memset(&gbv, 0, sizeof gbv);
    gbv.fd = -1;
}
static void gbv_closed(lv_anim_t *a) { (void)a; gbv_close(false); }
static void gbv_fade_out(void)
{
    if (gbv.leaving) return;
    gbv.leaving = true;
    if (gbv.timer) lv_timer_pause(gbv.timer);
    lv_anim_delete(gbv.scrim, gbv_set_opa);   /* from wherever its fade in got to */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, gbv.scrim);
    lv_anim_set_exec_cb(&a, gbv_set_opa);
    lv_anim_set_values(&a, lv_obj_get_style_opa(gbv.scrim, 0), 0);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, gbv_closed);
    lv_anim_start(&a);
}
static void gbv_tap(lv_event_t *e)
{
    (void)e;
    if (gb_talking()) mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    gbv_fade_out();
}

/* Where the clip is: by its voice while it talks, else by the clock. */
static uint32_t gbv_ms(void)
{
    if (gb_talking() && gb_clip.rate)
        return (uint32_t)((uint64_t)(gb_clip.pos >> 16) * 1000 / gb_clip.rate);
    return lv_tick_elaps(gbv.started);
}

static void gbv_show(int f)
{
    size_t bytes = (size_t)gbv.w * gbv.h * 2;
    if (lseek(gbv.fd, 12 + (off_t)f * (off_t)bytes, SEEK_SET) < 0 ||
        read(gbv.fd, gbv.px, bytes) != (ssize_t)bytes) { gbv.frames = f; return; }
    /* round the top corners into the card, when the picture fills it */
    uint16_t bg = sp_565(0x151515);
    for (int y = 0; y < GB_RADIUS && gbv.cw == gbv.w; y++)
        for (int x = 0; x < gbv.inset[y]; x++)
        {
            gbv.px[y * gbv.w + x] = bg;
            gbv.px[y * gbv.w + gbv.w - 1 - x] = bg;
        }
    gbv.shown = f;
    lv_obj_invalidate(gbv.canvas);
    lv_obj_set_width(gbv.bar, (int32_t)((int64_t)gbv.cw * (f + 1) / gbv.frames));
}

static void gbv_tick(lv_timer_t *t)
{
    (void)t;
    if (sl_power_hides_ui() || !mt.page) { gbv_close(true); return; }
    int f = (int)((uint64_t)gbv_ms() * gbv.fps100 / 100000);
    if (f < gbv.frames && f != gbv.shown) gbv_show(f);
    if (f >= gbv.frames - 1 && !gb_talking())
    {
        if (!gbv.ended) gbv.ended = lv_tick_get();
        else if (lv_tick_elaps(gbv.ended) > 450) gbv_fade_out();   /* a beat on the last frame */
    }
}

/* Open the theater if the clip has a picture; false if not. */
static bool gbv_open(void)
{
    if (gbv.scrim) return true;
    int fd = open(GB_VIDEO, O_RDONLY);
    uint8_t h[12];
    if (fd < 0) return false;
    if (read(fd, h, 12) != 12 || memcmp(h, "SLV1", 4)) { close(fd); return false; }
    int w = (int)gb_le(h + 4, 2), hh = (int)gb_le(h + 6, 2), fps100 = (int)gb_le(h + 8, 2), n = (int)gb_le(h + 10, 2);
    if (w < 32 || w > GB_VIDEO_W || hh < 32 || hh > GB_VIDEO_H || fps100 < 100 || fps100 > 6000 || n < 1)
    {
        close(fd);
        sl_log("gabagool: " GB_VIDEO " is not a picture this player can show");
        return false;
    }
    uint16_t *px = malloc((size_t)w * hh * 2);
    if (!px) { close(fd); return false; }
    gbv.fd = fd; gbv.px = px; gbv.w = w; gbv.h = hh; gbv.fps100 = fps100; gbv.frames = n; gbv.shown = -1;
    int cw = gbv.cw = w < 340 ? 340 : w;
    for (int y = 0; y < GB_RADIUS; y++)
    {
        float d = GB_RADIUS - y - 0.5f;
        gbv.inset[y] = (uint8_t)(GB_RADIUS - sqrtf(GB_RADIUS * GB_RADIUS - d * d) + 0.5f);
    }
    /* the page dims; the card rises in with the picture and who says it */
    gbv.scrim = box(lv_layer_top());
    lv_obj_set_size(gbv.scrim, W, H);
    lv_obj_set_style_bg_color(gbv.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(gbv.scrim, 175, 0);
    lv_obj_add_flag(gbv.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(gbv.scrim, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(gbv.scrim, gbv_tap, LV_EVENT_CLICKED, NULL);
    gbv.card = box(gbv.scrim);
    lv_obj_set_size(gbv.card, cw, hh + 78);
    lv_obj_center(gbv.card);
    lv_obj_set_style_bg_color(gbv.card, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(gbv.card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gbv.card, GB_RADIUS, 0);
    lv_obj_set_style_border_width(gbv.card, 1, 0);
    lv_obj_set_style_border_color(gbv.card, lv_color_hex(0x2c2c2c), 0);
    gbv.canvas = lv_canvas_create(gbv.card);
    lv_canvas_set_buffer(gbv.canvas, px, w, hh, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_x(gbv.canvas, (cw - w) / 2);
    memset(px, 0, (size_t)w * hh * 2);
    lv_obj_t *track = box(gbv.card);
    lv_obj_set_pos(track, 0, hh);
    lv_obj_set_size(track, cw, 3);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    gbv.bar = box(gbv.card);
    lv_obj_set_pos(gbv.bar, 0, hh);
    lv_obj_set_size(gbv.bar, 0, 3);
    lv_obj_set_style_bg_color(gbv.bar, lv_color_hex(0xd8343f), 0);
    lv_obj_set_style_bg_opa(gbv.bar, LV_OPA_COVER, 0);
    lv_obj_t *who = lv_label_create(gbv.card);
    lv_label_set_text(who, "Dr. Melfi");
    lv_obj_set_style_text_font(who, &ss_semibold_21, 0);
    lv_obj_set_style_text_color(who, lv_color_white(), 0);
    lv_obj_set_pos(who, 18, hh + 14);
    lv_obj_t *line = lv_label_create(gbv.card);
    lv_label_set_text(line, "\u201cLet\u2019s get back to that gabagool.\u201d");
    lv_obj_set_style_text_font(line, &ss_regular_18, 0);
    lv_obj_set_style_text_color(line, lv_color_hex(0xb9b9b9), 0);
    lv_obj_set_pos(line, 18, hh + 42);
    lv_obj_set_width(line, cw - 36);
    label_lines(line, 1);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, gbv.scrim);
    lv_anim_set_exec_cb(&a, gbv_set_opa);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 200);
    lv_anim_start(&a);
    lv_anim_set_var(&a, gbv.card);
    lv_anim_set_exec_cb(&a, gbv_set_rise);
    lv_anim_set_values(&a, 40, 0);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    gbv.started = lv_tick_get();
    gbv_show(0);
    gbv.timer = lv_timer_create(gbv_tick, 15, NULL);
    return true;
}

static void mt_gabagool_tap(struct mt_slot *s)
{
    static const char *const quips[] = { "gabagool!", "ooh, gabagool", "gabagool?", "the good gabagool",
                                         "sliced thin", "hot gabagool", "gaba-GOOL" };
    bool small = s->size == SZ_SMALL;
    if (gb.art)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, gb.art);
        lv_anim_set_exec_cb(&a, gb_set_squish);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, 750);
        lv_anim_delete(gb.art, gb_set_squish);
        lv_anim_start(&a);
    }
    if (gb.bubble)
    {
        gb.n = (gb.n + 1) % (int)(sizeof quips / sizeof quips[0]);
        lv_label_set_text(gb.quip, small ? "gabagool!" : quips[gb.n]);
        lv_obj_update_layout(gb.bubble);
        lv_obj_set_style_transform_pivot_x(gb.bubble, lv_obj_get_width(gb.bubble) / 2, 0);
        lv_obj_set_style_transform_pivot_y(gb.bubble, lv_obj_get_height(gb.bubble), 0);
        lv_obj_remove_flag(gb.bubble, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, gb.bubble);
        lv_anim_set_exec_cb(&a, gb_set_bubble);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, 1500);
        lv_anim_delete(gb.bubble, gb_set_bubble);
        lv_anim_start(&a);
    }
    if (gb_talking()) return;            /* already saying it */
    gb_say();
    gbv_open();
}

static void gb_forget(void)
{
    gbv_close(true);
    if (gb.art) lv_anim_delete(gb.art, gb_set_squish);
    if (gb.bubble) lv_anim_delete(gb.bubble, gb_set_bubble);
    gb.art = gb.bubble = gb.quip = NULL;
}

static void mt_gabagool_fill(struct mt_slot *s)
{
    lv_obj_t *t = s->obj;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    bool small = s->size == SZ_SMALL, wide = s->size == SZ_WIDE;
    lv_obj_set_style_bg_color(t, lv_color_hex(0xfbf3e4), 0);
    if (wide)
    {
        /* the tablecloth beside him, under the deli's sign */
        lv_obj_t *cloth = box(t);
        lv_obj_set_size(cloth, w - h, h);
        lv_obj_set_x(cloth, h);
        lv_obj_add_event_cb(cloth, gb_gingham_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
    /* the photo fills the tile (wide: its left square); a tap squishes
     * it down onto the counter */
    int side = small ? 104 : 219;
    gb.art = gb_photo(t, side);
    lv_image_set_pivot(gb.art, side / 2, side * 46 / 48);
    if (wide)
    {
        lv_obj_t *sign = box(t);
        lv_obj_set_style_bg_color(sign, lv_color_hex(0xfffaf0), 0);
        lv_obj_set_style_bg_opa(sign, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sign, SL_R_SM, 0);
        lv_obj_set_style_pad_hor(sign, 16, 0);
        lv_obj_set_style_pad_ver(sign, 10, 0);
        lv_obj_set_style_shadow_width(sign, 12, 0);
        lv_obj_set_style_shadow_opa(sign, 50, 0);
        lv_obj_set_size(sign, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sign, LV_FLEX_FLOW_COLUMN);
        lv_obj_align(sign, LV_ALIGN_CENTER, h / 2, 0);
        lv_obj_t *word = lv_label_create(sign);
        lv_label_set_text(word, "Gabagool?");
        lv_obj_set_style_text_font(word, &ss_bold_28, 0);
        lv_obj_set_style_text_color(word, lv_color_hex(0x8e2032), 0);
        lv_obj_t *sub = lv_label_create(sign);
        lv_label_set_text(sub, "Ova haaaa!");
        lv_obj_set_style_text_font(sub, &ss_semibold_21, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(0x6b4a3a), 0);
    }
    else
    {
        /* the caption, on the counter he is pointing at */
        lv_obj_t *pill = box(t);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0xfffaf0), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 7, 0);
        lv_obj_set_style_pad_hor(pill, 8, 0);
        lv_obj_set_style_pad_ver(pill, 2, 0);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(pill, small ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_BOTTOM_LEFT, small ? 0 : 7, small ? -4 : -6);
        lv_obj_t *l = lv_label_create(pill);
        lv_label_set_text(l, small ? "Gabagool?\nOva haaaa!" : GB_LINE);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(l, -3, 0);
        lv_obj_set_style_text_font(l, small ? &ss_regular_16 : &ss_semibold_18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x5a2a22), 0);
    }
    /* the speech bubble, shown by a tap */
    gb.bubble = box(t);
    lv_obj_set_style_bg_color(gb.bubble, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(gb.bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gb.bubble, SL_R_MD, 0);
    lv_obj_set_style_border_width(gb.bubble, 2, 0);
    lv_obj_set_style_border_color(gb.bubble, lv_color_hex(0x2a1612), 0);
    lv_obj_set_style_pad_hor(gb.bubble, small ? 6 : 10, 0);
    lv_obj_set_style_pad_ver(gb.bubble, small ? 2 : 4, 0);
    lv_obj_set_size(gb.bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    gb.quip = lv_label_create(gb.bubble);
    lv_label_set_text(gb.quip, "gabagool!");
    lv_obj_set_style_text_font(gb.quip, small ? &ss_semibold_18 : &ss_semibold_21, 0);
    lv_obj_set_style_text_color(gb.quip, lv_color_hex(0x2a1612), 0);
    if (small) lv_obj_align(gb.bubble, LV_ALIGN_TOP_MID, 0, 4);
    else lv_obj_align(gb.bubble, LV_ALIGN_TOP_RIGHT, wide ? -150 : -8, 8);
    lv_obj_add_flag(gb.bubble, LV_OBJ_FLAG_HIDDEN);
}
