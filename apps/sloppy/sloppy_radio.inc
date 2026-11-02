/* Sloppy FM: a radio idle game on a hi-fi receiver. Tap the tuning knob
 * for listeners (quick taps build Signal Lock), build a network, answer
 * the Request Line, then Syndicate for Signal Rights. The economy lives
 * in sloppy_radio_state.h. GPLv2+. */
#include "sloppy_radio_state.h"
#include "sloppy_radio_news.h"
#define RADIO_FILE ROCKBOX_DIR "/sloppy/fm.dat"
#define RADIO_BAK  ROCKBOX_DIR "/sloppy/fm.bak"
#define RADIO_TEMP ROCKBOX_DIR "/sloppy/fm.tmp"
#define RADIO_ROWS 40
#define RADIO_FLOATS 6
#define RADIO_SEGS 10
#define RADIO_STATS 12
/* the receiver is a physical object: same colours in every theme */
#define RX_PANEL    0x1d2026
#define RX_PANEL2   0x2c3139
#define RX_EDGE     0x3c424b
#define RX_GLASS    0x120e08
#define RX_AMBER    0xffb347
#define RX_AMBER_D  0x6e5126
#define RX_AMBER_TXT (sl_theme() == 1 ? 0xa86400 : RX_AMBER)   /* amber as words: deeper on the light card */
#define RX_CREAM    0xfff1d6
#define RX_RED      0xff5a47
#define RX_MUTED    0xa99c84
#define RX_INK      0x241a08
#define RX_W        (W - 28)
#define RX_SCALE_X  22
#define RX_SCALE_W  (RX_W - 72)
#define RX_KNOB     150
enum { RT_STUDIO, RT_UPGRADES, RT_SYNDICATE, RT_STATS, RT_COUNT };
enum { RB_NONE, RB_HIT, RB_VIRAL };
enum { ST_RUN, ST_ALL, ST_RATE, ST_TAP, ST_TAPS, ST_TAPEARN, ST_BUILD, ST_UPG,
       ST_CALLERS, ST_SYND, ST_AIR, ST_BOOST };
static struct sl_fm_state radio_state;
static struct sl_fm_rates radio_rates;
static bool radio_loaded, radio_dirty;
static uint32_t radio_last_saved_ms, radio_last_ms, radio_second_ms, radio_sig;
static lv_obj_t *radio_page_obj, *radio_dial, *radio_knob_dot, *radio_knob_caption, *radio_coins,
                *radio_unit, *radio_rate, *radio_station, *radio_needle, *radio_lamp, *radio_ticker,
                *radio_caller, *radio_list, *radio_tabs[RT_COUNT], *radio_amounts[4], *radio_stats,
                *radio_synd_btn, *radio_synd_level, *radio_synd_info, *radio_synd_bar, *radio_synd_next,
                *radio_buy_all, *radio_sale, *radio_formats[FMT_COUNT], *radio_format_info,
                *radio_rx, *radio_banner, *radio_tab_badge[RT_COUNT];
static int radio_badge_n[RT_COUNT];
static lv_obj_t *radio_float[RADIO_FLOATS], *radio_vu[2][RADIO_SEGS], *radio_stat_val[RADIO_STATS];
static lv_obj_t *radio_rows[RADIO_ROWS], *radio_row_sub[RADIO_ROWS], *radio_row_cost[RADIO_ROWS],
                *radio_row_pill[RADIO_ROWS], *radio_row_owned[RADIO_ROWS], *radio_row_bar[RADIO_ROWS];
static int16_t radio_row_id[RADIO_ROWS];
/* a row whose name's line reaches down to its price (the larger text sizes):
 * the name stops short of the price; its left and the row's width */
static lv_obj_t *radio_row_name[RADIO_ROWS];
static int16_t radio_row_tight[RADIO_ROWS], radio_row_w[RADIO_ROWS], radio_row_full[RADIO_ROWS];
static int radio_nrows, radio_tab, radio_amount, radio_float_next, radio_vu_lit[2];
static int radio_rows_tab = -1;   /* the tab the rows were built for: until the rebuild after a tab change they are the old tab's */
static lv_obj_t *radio_owned_head, *radio_tab_row;
/* upgrades bought since the Upgrades tab opened: they stay in the shop list,
 * marked Owned, so a purchase never just vanishes */
static uint32_t radio_fresh[FM_UPG_WORDS];
static bool radio_scroll_tabs;
/* Your upgrades: groups fold away once there are many; one can be opened */
#define RADIO_OPEN_ALL 30
static int radio_open_group = -1;
static int radio_buff, radio_knob_angle, radio_needle_x;
static double radio_combo;
static uint32_t radio_combo_ms, radio_ticker_ms, radio_ticker_len;
static uint32_t radio_buff_until, radio_caller_next, radio_caller_until, radio_confirm_until, radio_seed;
static lv_timer_t *radio_timer;
static const unsigned radio_amount_n[4] = { 1, 10, 100, 0 };

struct radio_badge { uint32_t color; const char *sym; };
static const struct radio_badge radio_building_badge[FM_B_COUNT] = {
    { 0x5aa9e6, LV_SYMBOL_AUDIO },  { 0xf29e4c, LV_SYMBOL_BELL },    { 0x72d8cd, LV_SYMBOL_CALL },
    { 0xe36588, LV_SYMBOL_GPS },    { 0x9b8cf2, LV_SYMBOL_WIFI },    { 0x7ee081, LV_SYMBOL_LOOP },
    { 0x4cc9f0, LV_SYMBOL_UPLOAD }, { 0xffb347, LV_SYMBOL_VOLUME_MAX }, { 0xf25f5c, LV_SYMBOL_PLAY },
    { 0xb8f2e6, LV_SYMBOL_EYE_OPEN }, { 0xd9dde6, LV_SYMBOL_TINT }, { 0xc77dff, LV_SYMBOL_SHUFFLE },
    { 0x8be9fd, LV_SYMBOL_VIDEO },  { 0xff79c6, LV_SYMBOL_IMAGE },   { 0xf1fa8c, LV_SYMBOL_CHARGE },
    { 0xbd93f9, LV_SYMBOL_REFRESH }, { 0x50fa7b, LV_SYMBOL_KEYBOARD }, { 0xff9e64, LV_SYMBOL_DIRECTORY },
    { 0x7aa2f7, LV_SYMBOL_BLUETOOTH }, { 0xe0af68, LV_SYMBOL_HOME },
};
static const struct radio_badge radio_kind_badge[] = {
    [FU_TAP_FLAT] = { 0xffb347, LV_SYMBOL_PLUS },    [FU_TAP_PCT] = { 0xffd166, LV_SYMBOL_CHARGE },
    [FU_GLOBAL_PCT] = { 0x7ee081, LV_SYMBOL_UP },    [FU_SYNERGY] = { 0x4cc9f0, LV_SYMBOL_SHUFFLE },
    [FU_CALLER] = { 0xffd166, LV_SYMBOL_CALL },      [FU_FANCLUB] = { 0xe36588, LV_SYMBOL_OK },
    [FU_COMBO] = { 0x9b8cf2, LV_SYMBOL_SETTINGS },
    [FU_TAP_BUILD] = { 0xffb347, LV_SYMBOL_PLUS },
};


static uint32_t radio_clock(void)
{
    struct tm *clock = get_time();
    if (!clock) return 0;
    struct tm copy = *clock;
    time_t value = mktime(&copy);
    return value > 0 ? (uint32_t)value : 0;
}
/* Temp file, fsync, keep the last good save as .bak, then swap in */
static bool radio_save(void)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    sl_fm_seal(&radio_state);
    int fd = open(RADIO_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool okay = write(fd, &radio_state, sizeof radio_state) == sizeof radio_state;
    if (okay) okay = fsync(fd) == 0;
    if (close(fd) < 0) okay = false;
    if (!okay) { remove(RADIO_TEMP); return false; }
    rename(RADIO_FILE, RADIO_BAK);
    if (rename(RADIO_TEMP, RADIO_FILE) < 0) { remove(RADIO_TEMP); return false; }
    radio_dirty = false;
    radio_last_saved_ms = sl_ms();
    return true;
}
static bool radio_read(const char *path)
{
    struct sl_fm_state saved;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool valid = read(fd, &saved, sizeof saved) == sizeof saved && sl_fm_valid(&saved);
    close(fd);
    if (valid) radio_state = saved;
    return valid;
}
static void radio_load(void)
{
    if (radio_loaded) return;
    radio_loaded = true;
    if (!radio_read(RADIO_FILE))
    {
        if (radio_read(RADIO_BAK)) sl_log("radio: fm.dat bad, restored fm.bak");
        else sl_fm_init(&radio_state, radio_clock());
        radio_dirty = true;
    }
    sl_fm_recalc(&radio_state, &radio_rates);
    if (radio_dirty && !radio_save()) sl_log("radio: could not save state");
}
/* Pays for the time the page was closed */
static void radio_catch_up(void)
{
    uint32_t was = radio_state.last_time;
    double away = sl_fm_offline(&radio_state, &radio_rates, radio_clock());
    if (away > 0)
    {
        radio_dirty = true;
        if (radio_state.last_time - was >= 60)
        {
            char n[40], msg[80];
            sl_fm_fmt(away, n, sizeof n);
            snprintf(msg, sizeof msg, "While you were away: +%s", n);
            toast(msg);
        }
    }
}
static void radio_changed(bool now)
{
    sl_fm_recalc(&radio_state, &radio_rates);
    radio_dirty = true;
    if ((now || sl_ms() - radio_last_saved_ms >= 2000) && !radio_save())
        toast_warn("Could not save. Check card space.");
}


/* -------------------------------------------------------- primitives */
static lv_obj_t *rx_box(lv_obj_t *parent, int w, int h, int radius, uint32_t bg)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}
static lv_obj_t *rx_text(lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = text(parent, s, f, color);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);   /* not theme mapped */
    return l;
}
static void radio_set(lv_obj_t *l, const char *s)
{
    if (l && strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}
/* The tick refreshes every 200 ms; setting a style, even to the same value,
 * redraws the object, so these only touch what really changed. */
static void radio_bg(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0);
}
static void radio_fg(lv_obj_t *o, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}
static lv_obj_t *radio_badge(lv_obj_t *parent, const struct radio_badge *b, int size, bool dim)
{
    lv_obj_t *o = rx_box(parent, size, size, SL_R_FULL, dim ? C_CARD_P : b->color);
    lv_obj_t *s = rx_text(o, dim ? "?" : b->sym, size > 50 ? &sl_sym_24 : &sl_sym_20,
                          dim ? C_DIM : 0x14161a);
    lv_obj_center(s);
    return o;
}
/* cost pill: amber when affordable */
static void radio_pill_on(int k, bool on)
{
    radio_bg(radio_row_pill[k], on ? RX_AMBER : C_CARD_P);
    radio_fg(radio_row_cost[k], on ? RX_INK : C_DIM);
}
static lv_obj_t *radio_row(const char *title, int id, lv_event_cb_t cb, const struct radio_badge *badge,
                           int height, int indent)
{
    if (radio_nrows >= RADIO_ROWS) return NULL;
    int k = radio_nrows++;
    lv_obj_t *r = box(radio_list);
    lv_obj_add_style(r, &st_card, 0);
    lv_obj_add_style(r, &st_card_pr, LV_STATE_PRESSED);
    lv_obj_set_size(r, RX_W - indent, height);
    lv_obj_set_style_margin_left(r, indent, 0);
    lv_obj_set_style_margin_bottom(r, 8, 0);
    int bs = height > 86 ? 56 : 48;
    lv_obj_t *b = radio_badge(r, badge, bs, id < 0);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 14, 0);
    int x = 14 + bs + 14;
    /* the price sits bottom right, so the name gets the full width and the
     * line under it stops short of the price */
    lv_obj_t *a = text(r, title, &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(a, x, height > 86 ? 12 : 10);
    lv_obj_set_width(a, RX_W - indent - x - (height > 86 ? 70 : 16));
    label_lines(a, 1);
    radio_row_name[k] = a;
    radio_row_w[k] = (int16_t)(RX_W - indent);
    radio_row_full[k] = (int16_t)lv_obj_get_style_width(a, 0);
    radio_row_tight[k] = (height > 86 ? 12 : 10) + lv_font_get_line_height(&ss_semibold_21) >
                         height - (height > 86 ? 12 : 10) - lv_font_get_line_height(&ss_semibold_18) - 8 ? (int16_t)x : 0;
    radio_row_sub[k] = text(r, "", &ss_regular_16, C_SUB);
    lv_obj_set_pos(radio_row_sub[k], x, height > 86 ? 42 : 46);
    lv_obj_set_width(radio_row_sub[k], RX_W - indent - x - 168);
    label_lines(radio_row_sub[k], 1);
    radio_row_pill[k] = rx_box(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT, 13, C_CARD_P);
    lv_obj_set_style_pad_hor(radio_row_pill[k], 12, 0);
    lv_obj_set_style_pad_ver(radio_row_pill[k], 4, 0);
    radio_row_cost[k] = text(radio_row_pill[k], "", &ss_semibold_18, C_DIM);
    lv_obj_align(radio_row_pill[k], LV_ALIGN_BOTTOM_RIGHT, -14, height > 86 ? -12 : -10);
    radio_row_owned[k] = radio_row_bar[k] = NULL;
    radio_rows[k] = r;
    radio_row_id[k] = (int16_t)id;
    if (cb) on_click(r, cb, (void *)(intptr_t)id);
    return r;
}
static lv_obj_t *radio_bar(lv_obj_t *parent, int w, int h, lv_obj_t **fill)
{
    lv_obj_t *t = rx_box(parent, w, h, SL_R_FULL, C_CARD_P);
    *fill = rx_box(t, 0, h, SL_R_FULL, RX_AMBER);
    return t;
}
static void radio_bar_set(lv_obj_t *fill, double frac)
{
    if (!fill) return;
    if (!(frac > 0)) frac = 0;
    if (frac > 1) frac = 1;
    int w = (int)(lv_obj_get_width(lv_obj_get_parent(fill)) * frac);
    if (lv_obj_get_width(fill) != w) lv_obj_set_width(fill, w);
}

/* segmented control used for the buy amount */
static lv_obj_t *radio_segment(lv_obj_t *parent, const char *label, lv_event_cb_t cb, int id)
{
    lv_obj_t *b = rx_box(parent, 0, 40, SL_R_FULL, C_CARD);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_center(text(b, label, &ss_semibold_18, 0xffffff));
    on_click(b, cb, (void *)(intptr_t)id);
    lv_obj_set_ext_click_area(b, (SL_TAP - 40) / 2);   /* a finger-sized tap */
    return b;
}
static void radio_segment_on(lv_obj_t *b, bool on)
{
    radio_bg(b, on ? RX_AMBER : C_CARD);
    radio_fg(lv_obj_get_child(b, 0), on ? RX_INK : C_SUB);
}
/* hardware preset button with an LED on top */
static void radio_preset_on(lv_obj_t *b, bool on)
{
    lv_obj_t *led = lv_obj_get_child(b, 0);
    lv_obj_set_style_bg_color(led, lv_color_hex(on ? RX_AMBER : C_CARD_P), 0);
    lv_obj_set_style_shadow_width(led, on ? 12 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 1), lv_color_hex(on ? C_TEXT : C_SUB), 0);
}

static bool radio_building_shown(int b)
{
    return b == 0 || radio_state.owned[b] || radio_state.owned[b - 1] ||
           radio_state.run_total >= fm_buildings[b].base_cost / 2;
}
/* Upgrades on sale, cheapest first */
static int radio_available(int16_t *out, int max)
{
    int n = 0;
    struct fm_upgrade u;
    for (int i = 0; i < FM_UPG_COUNT; i++)
    {
        if (sl_fm_has(&radio_state, i) || !sl_fm_upgrade(i, &u) || !sl_fm_unlocked(&radio_state, &u)) continue;
        int k = n < max ? n++ : max;
        if (k == max) { struct fm_upgrade l; sl_fm_upgrade(out[max - 1], &l); if (l.cost <= u.cost) continue; k = max - 1; }
        while (k > 0) { struct fm_upgrade p; sl_fm_upgrade(out[k - 1], &p); if (p.cost <= u.cost) break; out[k] = out[k - 1]; k--; }
        out[k] = (int16_t)i;
    }
    return n;
}
/* Changes when the current tab needs different rows */
static uint32_t radio_signature(void)
{
    uint32_t h = 2166136261u ^ (uint32_t)radio_tab;
    if (radio_tab == RT_STUDIO)
    {
        for (int b = 0; b < FM_B_COUNT; b++) h = (h ^ radio_building_shown(b)) * 16777619u;
        h = (h ^ sl_fm_formats_open(&radio_state)) * 16777619u;
    }
    else if (radio_tab == RT_UPGRADES)
    {
        int16_t ids[RADIO_ROWS];
        int n = radio_available(ids, RADIO_ROWS);
        for (int i = 0; i < n; i++) h = (h ^ (uint32_t)ids[i]) * 16777619u;
        for (int w = 0; w < FM_UPG_WORDS; w++) h = (h ^ radio_fresh[w]) * 16777619u;
        h = (h ^ (uint32_t)radio_open_group) * 16777619u;
    }
    else if (radio_tab == RT_SYNDICATE)
    {
        for (int w = 0; w < FM_PERK_WORDS; w++) h = (h ^ radio_state.perks[w]) * 16777619u;
        h = (h ^ radio_state.syndications) * 16777619u;
    }
    else
        for (int w = 0; w < FM_ACH_WORDS; w++) h = (h ^ radio_state.plaques[w]) * 16777619u;
    return h;
}
static int radio_perk_depth(int p)
{
    const struct fm_perk *k = &fm_perks[p];
    int d = 0;
    if (k->p1 >= 0) d = 1 + radio_perk_depth(k->p1);
    if (k->p2 >= 0 && 1 + radio_perk_depth(k->p2) > d) d = 1 + radio_perk_depth(k->p2);
    return d;
}
/* Depth first: each deal right under the deal it needs */
static void radio_perk_order(int parent, int *out, int *n)
{
    for (int p = 0; p < PK_COUNT; p++)
        if (fm_perks[p].p1 == parent) { out[(*n)++] = p; radio_perk_order(p, out, n); }
}
/* A new headline once the last one has scrolled all the way through */
static void radio_next_news(void)
{
    char line[160];
    for (int i = 0; i < 4; i++)
    {
        sl_fm_news(&radio_state, radio_rates.per_sec, &radio_seed, line, sizeof line);
        if (strcmp(line, lv_label_get_text(radio_ticker))) break;
    }
    radio_set(radio_ticker, line);
    int w = lv_text_get_width(line, strlen(line), &ss_regular_16, 0);
    radio_ticker_len = (uint32_t)(w + RX_W) * 1000 / 55 + 1500;
}

/* ---------------------------------------------------------- actions */
static void radio_rebuild(void);
static void radio_rebuild_async(void *unused) { (void)unused; if (radio_page_obj) radio_rebuild(); }
static void radio_refresh(void);
static void radio_badges(void);
static void radio_plaques(void)
{
    int first = -1, n = sl_fm_check_plaques(&radio_state, &first);
    if (!n) return;
    char name[48], msg[96];
    sl_fm_plaque_name(first, name, sizeof name);
    if (n > 1) snprintf(msg, sizeof msg, "Plaque: %s (+%d more)", name, n - 1);
    else snprintf(msg, sizeof msg, "Plaque: %s", name);
    toast(msg);
    radio_changed(false);
}
static void radio_float_anim(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, -v * 100 / 800, 0);
    lv_obj_set_style_opa(obj, (lv_opa_t)(v < 500 ? 255 : 255 - (v - 500) * 255 / 300), 0);
}
static void radio_float_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void radio_sink(void *obj, int32_t value)
{ lv_obj_set_style_translate_y(obj, value, 0); }
/* ringing caller: the border throbs (cheap, unlike scaling a shadowed object) */
static void radio_ring(void *obj, int32_t value)
{ lv_obj_set_style_border_width(obj, value, 0); }
static void radio_knob_place(void)
{
    double a = radio_knob_angle * 3.14159265358979 / 180;
    int c = RX_KNOB / 2, r = RX_KNOB / 2 - 20;
    lv_obj_set_pos(radio_knob_dot, c + (int)lround(r * sin(a)) - 7, c - (int)lround(r * cos(a)) - 7);
}
static void radio_tap_cb(lv_event_t *e)
{
    (void)e;
    radio_combo = radio_combo + FM_COMBO_STEP < 1 ? radio_combo + FM_COMBO_STEP : 1;
    radio_combo_ms = sl_ms();
    double mult = sl_fm_combo_mult(&radio_state, radio_combo) * (radio_buff == RB_VIRAL ? 777 : 1);
    double got = sl_fm_tap(&radio_state, &radio_rates, mult);
    sfx_play_vary(SFX_FM_TAP);
    radio_dirty = true;
    radio_knob_angle = (radio_knob_angle + 27) % 360;
    radio_knob_place();
    /* the knob sinks and springs back; a translate needs no layer, unlike a scale */
    lv_anim_delete(radio_dial, radio_sink);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, radio_dial);
    lv_anim_set_values(&anim, 5, 0);
    lv_anim_set_duration(&anim, 140);
    lv_anim_set_exec_cb(&anim, radio_sink);
    lv_anim_start(&anim);
    /* floating "+N" from a fixed pool */
    lv_obj_t *f = radio_float[radio_float_next];
    radio_float_next = (radio_float_next + 1) % RADIO_FLOATS;
    char n[40], label[44];
    sl_fm_fmt(got, n, sizeof n);
    snprintf(label, sizeof label, "+%s", n);
    lv_label_set_text(f, label);
    lv_obj_set_style_text_color(f, lv_color_hex(radio_combo > 0.6 ? RX_AMBER : RX_CREAM), 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align_to(f, radio_dial, LV_ALIGN_OUT_TOP_MID, (int)(sl_fm_rand(&radio_seed) % 120) - 60, 56);
    lv_anim_delete(f, radio_float_anim);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, f);
    lv_anim_set_values(&anim, 0, 800);
    lv_anim_set_duration(&anim, 800);
    lv_anim_set_exec_cb(&anim, radio_float_anim);
    lv_anim_set_completed_cb(&anim, radio_float_done);
    lv_anim_start(&anim);
    radio_refresh();
}
static void radio_flash_anim(void *obj, int32_t v)
{
    lv_obj_set_style_bg_color(obj, lv_color_mix(lv_color_hex(RX_AMBER), lv_color_hex(C_CARD), (uint8_t)v), 0);
}
static void radio_flash(lv_obj_t *obj)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 110, 0);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_exec_cb(&a, radio_flash_anim);
    lv_anim_start(&a);
}
static void radio_building_cb(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    if (!sl_fm_buy_building(&radio_state, b, radio_amount_n[radio_amount]))
    {
        sfx_play(SFX_ERROR);
        toast(radio_state.owned[b] >= FM_MAX_OWNED ? "That's the most this station can hold"
              : radio_amount_n[radio_amount] > FM_MAX_OWNED - radio_state.owned[b] ? "Not enough room. Try a smaller amount"
              : "Not enough listeners yet");
        return;
    }
    radio_changed(false);
    radio_plaques();
    sfx_play(SFX_FM_BUY);
    radio_flash(lv_event_get_current_target(e));
    radio_refresh();
    if (radio_signature() != radio_sig) lv_async_call(radio_rebuild_async, NULL);
    radio_badges();
}
static void radio_upgrade_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (sl_fm_has(&radio_state, i)) { toast("Already yours. See Your upgrades at the bottom"); return; }
    if (!sl_fm_buy_upgrade(&radio_state, i))
    { sfx_play(SFX_ERROR); toast("Not enough listeners yet"); return; }
    struct fm_upgrade u;
    char msg[96];
    fm_set(radio_fresh, i);
    sl_fm_upgrade(i, &u);
    snprintf(msg, sizeof msg, "%s is yours. It's listed under Your upgrades", u.name);
    toast(msg);
    sfx_play(SFX_FM_BUY);
    radio_changed(false);
    radio_refresh();
    lv_async_call(radio_rebuild_async, NULL);
}
/* Buys everything on sale that fits, cheapest first */
static void radio_buy_all_cb(lv_event_t *e)
{
    (void)e;
    int16_t ids[RADIO_ROWS];
    int n = radio_available(ids, RADIO_ROWS), got = 0;
    for (int i = 0; i < n; i++) if (sl_fm_buy_upgrade(&radio_state, ids[i])) { fm_set(radio_fresh, ids[i]); got++; }
    if (!got) { sfx_play(SFX_ERROR); toast("Nothing you can afford yet"); return; }
    char msg[48];
    snprintf(msg, sizeof msg, "Bought %d upgrade%s, marked Owned", got, got > 1 ? "s" : "");
    toast(msg);
    radio_changed(false);
    sfx_play(SFX_FM_BUY);
    radio_refresh();
    lv_async_call(radio_rebuild_async, NULL);
}
static void radio_perk_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (!sl_fm_buy_perk(&radio_state, p))
    {
        sfx_play(SFX_ERROR);
        toast(sl_fm_perk(&radio_state, p) ? "Already signed" : !sl_fm_perk_ready(&radio_state, p) ?
              "Sign the deal above it first" : "Not enough Signal Rights");
        return;
    }
    sfx_play(SFX_WIN);
    radio_changed(true);
    radio_refresh();
    lv_async_call(radio_rebuild_async, NULL);
}
/* Receiver-wide banner that swells in and fades away */
static void radio_banner_anim(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)(v < 200 ? v * 255 / 200 : v > 1800 ? (2200 - v) * 255 / 400 : 255), 0);
    lv_obj_set_style_transform_scale(obj, v < 200 ? 200 + v * 56 / 200 : 256, 0);
}
static void radio_banner_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void radio_celebrate(const char *title, const char *sub)
{
    if (!radio_banner) return;
    radio_set(lv_obj_get_child(radio_banner, 0), title);
    radio_set(lv_obj_get_child(radio_banner, 1), sub);
    lv_obj_remove_flag(radio_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(radio_banner);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, radio_banner);
    lv_anim_set_values(&a, 0, 2200);
    lv_anim_set_duration(&a, 2200);
    lv_anim_set_exec_cb(&a, radio_banner_anim);
    lv_anim_set_completed_cb(&a, radio_banner_done);
    lv_anim_start(&a);
}
static void radio_syndicate_cb(lv_event_t *e)
{
    (void)e;
    uint32_t gain = sl_fm_pending(&radio_state);
    if (!gain) { sfx_play(SFX_ERROR); toast("Earn more listeners first"); return; }
    if (sl_ms() > radio_confirm_until)
    {
        radio_confirm_until = sl_ms() + 4000;
        sfx_play(SFX_TAP);
        radio_refresh();
        return;
    }
    radio_confirm_until = 0;
    radio_state.last_time = radio_clock() ? radio_clock() : radio_state.last_time;
    sl_fm_syndicate(&radio_state);
    memset(radio_fresh, 0, sizeof radio_fresh);
    radio_buff = RB_NONE;
    radio_combo = 0;
    radio_changed(true);   /* saved before anything else changes */
    radio_plaques();
    char msg[80];
    snprintf(msg, sizeof msg, "+%u Signal Right%s  ·  network level %u", (unsigned)gain, gain > 1 ? "s" : "",
             (unsigned)radio_state.rights);
    radio_celebrate("SYNDICATED!", msg);
    sfx_play(SFX_WIN);
    radio_refresh();
    lv_async_call(radio_rebuild_async, NULL);
}
static void radio_caller_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = sl_ms();
    lv_anim_delete(radio_caller, radio_ring);
    lv_obj_add_flag(radio_caller, LV_OBJ_FLAG_HIDDEN);
    radio_caller_until = 0;
    if (radio_state.callers < UINT32_MAX) radio_state.callers++;
    double len = sl_fm_boost_len(&radio_state);
    char n[40], msg[96];
    const char *title = "PAYOLA!";
    switch (sl_fm_call_pick(&radio_seed))
    {
    case FM_CALL_HIT:
        radio_buff = RB_HIT;
        radio_buff_until = now + (uint32_t)(77000 * len);
        title = "HIT SINGLE!";
        snprintf(msg, sizeof msg, "Listeners ×7 for %u s", (unsigned)(77 * len));
        break;
    case FM_CALL_VIRAL:
        radio_buff = RB_VIRAL;
        radio_buff_until = now + (uint32_t)(13000 * len);
        title = "VIRAL CLIP!";
        snprintf(msg, sizeof msg, "Taps ×777 for %u s", (unsigned)(13 * len));
        break;
    default:
    {
        double got = sl_fm_payola(&radio_state, &radio_rates);
        sl_fm_earn(&radio_state, got);
        sl_fm_fmt(got, n, sizeof n);
        snprintf(msg, sizeof msg, "+%s listeners", n);
    }
    }
    radio_celebrate(title, msg);
    radio_changed(false);
    radio_plaques();
    sfx_play(SFX_QUIZ_CASH);
    radio_refresh();
}
static void radio_schedule_caller(uint32_t now)
{
    uint32_t wait = 60000 + sl_fm_rand(&radio_seed) % 120000;
    radio_caller_next = now + (uint32_t)(wait / sl_fm_caller_rate(&radio_state));
}
static void radio_format_cb(lv_event_t *e)
{
    uint32_t f = (uint32_t)(intptr_t)lv_event_get_user_data(e), now = radio_clock();
    if (f == radio_state.format) return;
    uint32_t wait = sl_fm_format_wait(&radio_state, now);
    if (!sl_fm_set_format(&radio_state, f, now))
    {
        char msg[64];
        snprintf(msg, sizeof msg, "Switch formats again in %u min", (unsigned)(wait + 59) / 60);
        sfx_play(SFX_ERROR);
        toast(msg);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "Now broadcasting: %s", fm_formats[f].name);
    toast(msg);
    radio_changed(true);
    sfx_play(SFX_FM_BUY);
    radio_refresh();
}
/* Saves before power off, USB and other system screens */
static void radio_flush(void)
{
    if (radio_loaded && radio_dirty && !radio_save()) sl_log("radio: flush failed");
}
/* Live tile: listeners including what the station earned while closed */
static void radio_tile(lv_obj_t *t, int w, bool wide)
{
    radio_load();
    double now = radio_state.listeners;
    uint32_t clock = radio_clock();
    if (clock > radio_state.last_time)
    {
        uint32_t away = clock - radio_state.last_time;
        now += radio_rates.per_sec * (away < sl_fm_offline_cap(&radio_state) ? away : sl_fm_offline_cap(&radio_state));
    }
    char a[40], b[40], unit[64];
    sl_fm_fmt(now, a, sizeof a);
    char *space = strchr(a, ' ');
    if (space) { *space = 0; snprintf(unit, sizeof unit, sl_tr("%s listeners"), space + 1); }
    else snprintf(unit, sizeof unit, "%s", sl_tr("listeners"));
    lv_obj_t *n = mt_label(t, a, wide ? &ss_regular_60 : &ss_regular_34, LV_OPA_COVER);
    lv_obj_set_pos(n, 12, wide ? 6 : 12);
    lv_obj_t *u = mt_label(t, unit, &ss_semibold_21, LV_OPA_COVER);
    lv_obj_set_pos(u, 14, wide ? 88 : 58);
    lv_obj_set_width(u, w - 28);
    label_lines(u, 1);
    sl_fm_fmt(radio_rates.per_sec, b, sizeof b);
    snprintf(unit, sizeof unit, sl_tr("%s per second"), b);
    lv_obj_t *r = mt_label(t, unit, &ss_regular_18, 215);
    lv_obj_set_pos(r, 14, wide ? 116 : 86);
    lv_obj_set_width(r, w - 28);
    label_lines(r, 1);
}
static void radio_plaque_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char name[48], msg[80];
    sl_fm_plaque_name(i, name, sizeof name);
    snprintf(msg, sizeof msg, "%s: %s", fm_bit(radio_state.plaques, i) ? "Earned" : "Locked", name);
    toast(msg);
}
static void radio_tab_cb(lv_event_t *e)
{
    radio_tab = (int)(intptr_t)lv_event_get_user_data(e);
    memset(radio_fresh, 0, sizeof radio_fresh);
    radio_open_group = -1;
    radio_scroll_tabs = true;
    sfx_play(SFX_TAP);
    lv_async_call(radio_rebuild_async, NULL);
}
static void radio_amount_cb(lv_event_t *e)
{
    radio_amount = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < 4; i++) if (radio_amounts[i]) radio_segment_on(radio_amounts[i], i == radio_amount);
    sfx_play(SFX_TAP);
    radio_refresh();
}

/* ----------------------------------------------------------- screen */
static void radio_vu_set(int m, int lit)
{
    if (lit < 0) lit = 0;
    if (lit > RADIO_SEGS) lit = RADIO_SEGS;
    if (lit == radio_vu_lit[m]) return;
    radio_vu_lit[m] = lit;
    for (int i = 0; i < RADIO_SEGS; i++)
        lv_obj_set_style_bg_opa(radio_vu[m][i], i < lit ? LV_OPA_COVER : LV_OPA_20, 0);
}
static void radio_refresh(void)
{
    if (!radio_page_obj) return;
    struct sl_fm_state *s = &radio_state;
    char a[40], b[40], label[160];
    double mult = radio_buff == RB_HIT ? 7 : 1;
    /* readout: big figure, then its scale word */
    sl_fm_fmt(s->listeners, a, sizeof a);
    char *space = strchr(a, ' ');
    if (space) { *space = 0; snprintf(label, sizeof label, sl_tr("%s listeners"), space + 1); }
    else snprintf(label, sizeof label, "%s", sl_tr("listeners"));
    radio_set(radio_coins, a);
    radio_set(radio_unit, label);
    sl_fm_fmt(radio_rates.per_sec * mult, a, sizeof a);
    sl_fm_fmt(radio_rates.per_tap * sl_fm_combo_mult(s, radio_combo) * (radio_buff == RB_VIRAL ? 777 : 1), b, sizeof b);
    snprintf(label, sizeof label, sl_tr("%s per second  ·  %s per tap"), a, b);
    radio_set(radio_rate, label);
    /* the needle drifts up the band as the station grows */
    double lg = log10(1 + radio_rates.per_sec * mult);
    double freq = 88.1 + (lg > 19.8 ? 19.8 : lg);
    snprintf(label, sizeof label, "%.1f MHz", freq);
    radio_set(radio_station, label);
    int nx = RX_SCALE_X + (int)((freq - 88) / 20 * RX_SCALE_W) - 1;
    if (nx != radio_needle_x)
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, radio_needle);
        lv_anim_set_values(&a, radio_needle_x < 0 ? nx : lv_obj_get_x(radio_needle), nx);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_start(&a);
        radio_needle_x = nx;
    }
    if (radio_buff)
    {
        uint32_t left = radio_buff_until > sl_ms() ? (radio_buff_until - sl_ms() + 999) / 1000 : 0;
        snprintf(label, sizeof label, radio_buff == RB_HIT ? "HIT ×7  ·  %us" : "VIRAL ×777  ·  %us", (unsigned)left);
    }
    else snprintf(label, sizeof label, "%s", sl_tr("ON AIR"));
    radio_set(lv_obj_get_child(radio_lamp, 0), label);
    radio_bg(radio_lamp, radio_buff ? RX_RED : 0x4a1d18);
    if (lv_obj_get_style_shadow_width(radio_lamp, 0) != (radio_buff ? 18 : 0))
        lv_obj_set_style_shadow_width(radio_lamp, radio_buff ? 18 : 0, 0);
    if (radio_combo > 0.05)
        snprintf(label, sizeof label, sl_tr("SIGNAL LOCK  ×%.1f"), sl_fm_combo_mult(s, radio_combo));
    else snprintf(label, sizeof label, "%s", sl_tr("TAP TO TUNE"));
    radio_set(radio_knob_caption, label);
    radio_fg(radio_knob_caption, radio_combo > 0.05 ? RX_AMBER : RX_MUTED);

    if (radio_rows_tab != radio_tab) return;   /* the rows' ids index the old tab's tables */
    struct fm_summary sum;
    if (radio_tab == RT_STUDIO) sl_fm_summary(s, &sum);
    for (int k = 0; k < radio_nrows; k++)
    {
        int id = radio_row_id[k];
        if (radio_tab == RT_STUDIO)
        {
            unsigned n = radio_amount_n[radio_amount], room = FM_MAX_OWNED - s->owned[id];
            if (!n) n = sl_fm_max_afford(s, id);
            double cost = sl_fm_building_cost(s, id, n ? n : 1);
            sl_fm_fmt(cost, a, sizeof a);
            if (!room) snprintf(label, sizeof label, "Maxed");
            else if (radio_amount && n) snprintf(label, sizeof label, "×%u  %s", n, a);
            else snprintf(label, sizeof label, "%s", a);
            radio_set(radio_row_cost[k], label);
            radio_pill_on(k, room && n && n <= room && cost <= s->listeners * (1 + 1e-12));
            unsigned next = sl_fm_next_tier(s, id);
            sl_fm_fmt(radio_rates.each[id], b, sizeof b);
            char boost[24] = "";
            if (sum.bmul[id] > 1) { sl_fm_fmt(sum.bmul[id], a, sizeof a); snprintf(boost, sizeof boost, " (×%s)", a); }
            if (next) snprintf(label, sizeof label, "+%s/s each%s  ·  ×2 at %u", b, boost, next);
            else snprintf(label, sizeof label, "+%s/s each%s  ·  fully tuned", b, boost);
            radio_set(radio_row_sub[k], label);
            snprintf(label, sizeof label, "%u", (unsigned)s->owned[id]);
            radio_set(radio_row_owned[k], label);
            unsigned prev = 0;
            for (int t = 0; t < FM_TIERS && fm_tier_need[t] <= s->owned[id]; t++) prev = fm_tier_need[t];
            radio_bar_set(radio_row_bar[k], next ? (double)(s->owned[id] - prev) / (next - prev) : 1);
        }
        else if (radio_tab == RT_UPGRADES)
        {
            struct fm_upgrade u;
            sl_fm_upgrade(id, &u);
            if (sl_fm_has(s, id))
            {
                radio_set(radio_row_cost[k], LV_SYMBOL_OK " Owned");
                radio_bg(radio_row_pill[k], 0x2f5a3a);
                radio_fg(radio_row_cost[k], 0x7ee081);
                continue;
            }
            sl_fm_fmt(u.cost, a, sizeof a);
            radio_set(radio_row_cost[k], a);
            radio_pill_on(k, u.cost <= s->listeners);
        }
        else if (radio_tab == RT_SYNDICATE)
        {
            bool have = sl_fm_perk(s, id), ready = sl_fm_perk_ready(s, id);
            if (have) snprintf(label, sizeof label, "Signed");
            else if (!ready) snprintf(label, sizeof label, "Locked");
            else snprintf(label, sizeof label, "%u Right%s", (unsigned)fm_perks[id].cost, fm_perks[id].cost > 1 ? "s" : "");
            radio_set(radio_row_cost[k], label);
            radio_pill_on(k, !have && ready && s->rights - s->rights_spent >= fm_perks[id].cost);
            if (have) radio_fg(radio_row_cost[k], 0x7ee081);
        }
    }
    for (int k = 0; k < radio_nrows; k++)
    {
        if (!radio_row_tight[k] || !radio_row_name[k]) continue;
        const char *t = lv_label_get_text(radio_row_cost[k]);
        int pw = lv_text_get_width(t, (uint32_t)strlen(t), &ss_semibold_18, 0) + 24;
        int w = radio_row_w[k] - 14 - pw - 10 - radio_row_tight[k];
        if (w > radio_row_full[k]) w = radio_row_full[k];
        if (lv_obj_get_style_width(radio_row_name[k], 0) != w) lv_obj_set_width(radio_row_name[k], w);
    }
    if (radio_tab == RT_STUDIO && radio_format_info)
    {
        for (int f = 1; f < FMT_COUNT; f++) radio_segment_on(radio_formats[f], s->format == (uint32_t)f);
        uint32_t wait = sl_fm_format_wait(s, radio_clock());
        if (wait) snprintf(label, sizeof label, "%s  ·  switch in %u min", fm_formats[s->format].desc, (unsigned)(wait + 59) / 60);
        else snprintf(label, sizeof label, "%s", fm_formats[s->format].desc);
        radio_set(radio_format_info, label);
    }
    if (radio_tab == RT_UPGRADES && radio_sale)
    {
        int16_t ids[RADIO_ROWS];
        int n = radio_available(ids, RADIO_ROWS), can = 0;
        double left = s->listeners;
        for (int i = 0; i < n; i++)
        {
            struct fm_upgrade u;
            sl_fm_upgrade(ids[i], &u);
            if (u.cost <= left) { left -= u.cost; can++; }
        }
        snprintf(label, sizeof label, "ON SALE %d  ·  %d AFFORDABLE", n, can);
        radio_set(radio_sale, label);

        radio_segment_on(radio_buy_all, can > 0);
    }
    if (radio_tab == RT_SYNDICATE && radio_synd_btn)
    {
        uint32_t gain = sl_fm_pending(s);
        double pct = s->rights * (sl_fm_perk(s, PK_PRIME) ? 2 : 1);
        snprintf(label, sizeof label, "%u", (unsigned)s->rights);
        radio_set(radio_synd_level, label);
        snprintf(label, sizeof label, "+%.0f%% listeners\n%u Signal Right%s to spend", pct,
                 (unsigned)(s->rights - s->rights_spent), s->rights - s->rights_spent == 1 ? "" : "s");
        radio_set(radio_synd_info, label);
        double prog = sl_fm_right_progress(s->all_total);
        sl_fm_fmt(pow((double)sl_fm_rights_for(s->all_total) + 1, 3) * FM_RIGHT_BASE, a, sizeof a);
        snprintf(label, sizeof label, "Next Right %d%%  ·  at %s ever", (int)(prog * 100), a);
        radio_set(radio_synd_next, label);
        radio_bar_set(radio_synd_bar, prog);
        if (!gain) snprintf(label, sizeof label, "Nothing to syndicate yet");
        else if (sl_ms() <= radio_confirm_until) snprintf(label, sizeof label, "Tap again: reset for +%u", (unsigned)gain);
        else snprintf(label, sizeof label, "Syndicate for +%u Right%s", (unsigned)gain, gain > 1 ? "s" : "");
        radio_set(lv_obj_get_child(radio_synd_btn, 0), label);
        radio_bg(radio_synd_btn, gain ? RX_AMBER : C_CARD);
        radio_fg(lv_obj_get_child(radio_synd_btn, 0), gain ? RX_INK : C_DIM);
    }
    if (radio_tab == RT_STATS && radio_stats)
    {
        unsigned bought = 0, run = s->last_time > s->run_started ? s->last_time - s->run_started : 0;
        for (int b2 = 0; b2 < FM_B_COUNT; b2++) bought += s->owned[b2];
        char v[RADIO_STATS][40];
        sl_fm_fmt(s->run_total, v[ST_RUN], 40);
        sl_fm_fmt(s->all_total, v[ST_ALL], 40);
        sl_fm_fmt(radio_rates.per_sec, v[ST_RATE], 40);
        sl_fm_fmt(radio_rates.per_tap, v[ST_TAP], 40);
        sl_fm_fmt(s->taps, v[ST_TAPS], 40);
        sl_fm_fmt(s->tap_total, v[ST_TAPEARN], 40);
        snprintf(v[ST_BUILD], 40, "%u", bought);
        snprintf(v[ST_UPG], 40, "%d / %d", fm_popcount(s->upgrades, FM_UPG_WORDS), sl_fm_upgrade_total());
        snprintf(v[ST_CALLERS], 40, "%u", (unsigned)s->callers);
        snprintf(v[ST_SYND], 40, "%u", (unsigned)s->syndications);
        snprintf(v[ST_AIR], 40, "%uh %02um", run / 3600, run / 60 % 60);
        sl_fm_fmt(radio_rates.global * 100 - 100, a, sizeof a);
        snprintf(v[ST_BOOST], 40, "+%s%%", a);
        for (int i = 0; i < RADIO_STATS; i++) radio_set(radio_stat_val[i], v[i]);
    }
}
/* A card in the Owned view: badge, title, a figure on the right, and a
 * wrapped list underneath */
static void radio_owned_card(const struct radio_badge *badge, const char *title, const char *figure, const char *body)
{
    lv_obj_t *c = box(radio_list);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_size(c, RX_W, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(c, 14, 0);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    lv_obj_t *b = radio_badge(c, badge, 44, false);
    lv_obj_set_pos(b, 14, 14);
    lv_obj_t *t = text(c, title, &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(t, 72, 14);
    lv_obj_set_width(t, RX_W - 72 - 110);
    label_lines(t, 1);
    if (figure)
    {
        lv_obj_t *f = rx_text(c, figure, &ss_semibold_21, RX_AMBER);
        lv_obj_align(f, LV_ALIGN_TOP_RIGHT, -16, 14);
    }
    lv_obj_t *l = text(c, body, &ss_regular_16, C_SUB);
    lv_obj_set_pos(l, 72, 44);
    lv_obj_set_width(l, RX_W - 72 - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 4, 0);
}
static size_t radio_cat(char *buf, size_t n, size_t size, const char *fmt, ...)
{
    if (n >= size - 1) return n;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, size - n, fmt, ap);
    va_end(ap);
    return k < 0 ? n : n + (size_t)k < size ? n + (size_t)k : size - 1;
}
/* Everything bought this broadcast, so nothing is forgotten */
static void radio_owned_view(void)
{
    struct fm_summary m;
    char body[640], fig[40], a[40];
    size_t n = 0;
    sl_fm_summary(&radio_state, &m);
    body[0] = 0;
    /* one line per area, naming only what was actually bought */
    size_t line;
    if (m.tap_flat || m.tap_pct || m.combo_cap > 3)
    {
        n = line = radio_cat(body, n, sizeof body, "Taps:");
        sl_fm_fmt(m.tap_flat, a, sizeof a);
        if (m.tap_flat) n = radio_cat(body, n, sizeof body, " +%s each", a);
        if (m.tap_pct) n = radio_cat(body, n, sizeof body, "%s +%.0f%% of listeners/s", n > line ? "," : "", m.tap_pct);
        if (m.combo_cap > 3) n = radio_cat(body, n, sizeof body, "%s Signal Lock up to ×%.0f", n > line ? "," : "", m.combo_cap);
        n = radio_cat(body, n, sizeof body, "\n");
    }
    if (m.global_pct || m.fan > 1)
    {
        n = line = radio_cat(body, n, sizeof body, "All listeners:");
        if (m.global_pct) n = radio_cat(body, n, sizeof body, " +%.0f%% from ratings", m.global_pct);
        if (m.fan > 1) n = radio_cat(body, n, sizeof body, "%s ×%.2f from the fan club", n > line ? "," : "", m.fan);
        n = radio_cat(body, n, sizeof body, "\n");
    }
    if (m.caller_rate > 1 || m.boost_len > 1)
    {
        n = line = radio_cat(body, n, sizeof body, "Callers:");
        if (m.caller_rate > 1) n = radio_cat(body, n, sizeof body, " ring ×%.2f as often", m.caller_rate);
        if (m.boost_len > 1) n = radio_cat(body, n, sizeof body, "%s boosts last ×%.1f", n > line ? "," : "", m.boost_len);
        n = radio_cat(body, n, sizeof body, "\n");
    }
    radio_cat(body, n, sizeof body, "%s", m.owned_count ? "Each one is listed by name at the bottom of this tab."
                                                       : "Nothing bought this broadcast yet. Upgrades reset when you syndicate.");
    snprintf(fig, sizeof fig, "%d owned", m.owned_count);
    const struct radio_badge total = { RX_AMBER, LV_SYMBOL_OK };
    radio_owned_card(&total, "Your upgrades", fig, body);
}
/* One row per upgrade owned, looking like the shop rows, grouped under a
 * small heading per building and per kind. Static: nothing to refresh. */
static const struct radio_badge *radio_upg_badge(const struct fm_upgrade *u)
{
    return u->kind == FU_BUILDING || u->kind == FU_SYNERGY || u->kind == FU_INTERN
           ? &radio_building_badge[u->target] : &radio_kind_badge[u->kind];
}
static void radio_owned_row(const struct fm_upgrade *u)
{
    lv_obj_t *r = box(radio_list);
    lv_obj_add_style(r, &st_card, 0);
    lv_obj_set_size(r, RX_W, 70);
    lv_obj_set_style_margin_bottom(r, 6, 0);
    lv_obj_t *b = radio_badge(r, radio_upg_badge(u), 42, false);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *t = text(r, u->name, &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(t, 70, 9);
    lv_obj_set_width(t, RX_W - 70 - 60);
    label_lines(t, 1);
    lv_obj_t *d = text(r, u->desc, &ss_regular_16, C_SUB);
    lv_obj_set_pos(d, 70, 38);
    lv_obj_set_width(d, RX_W - 70 - 60);
    label_lines(d, 1);
    lv_obj_t *ok = rx_text(r, LV_SYMBOL_OK, &sl_sym_22, 0x7ee081);
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -20, 0);
}
/* Groups: one per building (its tiers, intern and crossovers), then kinds */
enum { RG_TAPS = FM_B_COUNT, RG_LOCK, RG_RATINGS, RG_CALLS, RG_FANS, RG_COUNT };
static int radio_group_of(const struct fm_upgrade *u)
{
    switch (u->kind)
    {
    case FU_BUILDING: case FU_SYNERGY: case FU_INTERN: return u->target;
    case FU_TAP_FLAT: case FU_TAP_PCT: case FU_TAP_BUILD: return RG_TAPS;
    case FU_COMBO: return RG_LOCK;
    case FU_GLOBAL_PCT: return RG_RATINGS;
    case FU_CALLER: return RG_CALLS;
    default: return RG_FANS;
    }
}
static void radio_group_cb(lv_event_t *e)
{
    int g = (int)(intptr_t)lv_event_get_user_data(e);
    radio_open_group = radio_open_group == g ? -1 : g;
    sfx_play(SFX_TAP);
    lv_async_call(radio_rebuild_async, NULL);
}
static void radio_owned_details(void)
{
    static const char *const titles[RG_COUNT - FM_B_COUNT] = { "Taps", "Signal Lock", "Ratings & playlists",
                                                                "Request Line", "Fan club" };
    static const struct radio_badge kind_badge[RG_COUNT - FM_B_COUNT] = {
        { 0xffb347, LV_SYMBOL_PLUS }, { 0x9b8cf2, LV_SYMBOL_SETTINGS }, { 0x7ee081, LV_SYMBOL_UP },
        { 0xffd166, LV_SYMBOL_CALL }, { 0xe36588, LV_SYMBOL_OK } };
    struct fm_summary m;
    struct fm_upgrade u;
    int count[RG_COUNT] = { 0 }, total = 0;
    char title[64], sub[64], a[40];
    sl_fm_summary(&radio_state, &m);
    for (int i = 0; i < FM_UPG_COUNT; i++)
        if (sl_fm_has(&radio_state, i) && sl_fm_upgrade(i, &u)) { count[radio_group_of(&u)]++; total++; }
    bool all = total <= RADIO_OPEN_ALL;
    for (int g = 0; g < RG_COUNT; g++)
    {
        if (!count[g]) continue;
        bool open = all || radio_open_group == g;
        if (g < FM_B_COUNT)
        {
            sl_fm_fmt(m.bmul[g], a, sizeof a);
            snprintf(title, sizeof title, "%s", fm_buildings[g].name);
            snprintf(sub, sizeof sub, "%d upgrade%s  ·  ×%s output", count[g], count[g] > 1 ? "s" : "", a);
        }
        else
        {
            snprintf(title, sizeof title, "%s", titles[g - FM_B_COUNT]);
            snprintf(sub, sizeof sub, "%d upgrade%s", count[g], count[g] > 1 ? "s" : "");
        }
        /* group header: tap to fold or unfold */
        lv_obj_t *h = box(radio_list);
        lv_obj_add_style(h, &st_card, 0);
        lv_obj_add_style(h, &st_card_pr, LV_STATE_PRESSED);
        lv_obj_set_size(h, RX_W, 62);
        lv_obj_set_style_margin_bottom(h, 6, 0);
        lv_obj_set_style_margin_top(h, 4, 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(C_CARD_P), 0);
        lv_obj_t *b = radio_badge(h, g < FM_B_COUNT ? &radio_building_badge[g] : &kind_badge[g - FM_B_COUNT], 38, false);
        sl_align(b, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *t = text(h, title, &ss_semibold_18, 0xffffff);
        sl_place(t, 64, 8);
        lv_obj_t *d = text(h, sub, &ss_regular_16, C_SUB);
        sl_place(d, 64, 34);
        if (!all)
        {
            lv_obj_t *c = text(h, open ? LV_SYMBOL_DOWN : sl_chevron(), &sl_sym_20, 0xffffff);
            sl_align(c, LV_ALIGN_RIGHT_MID, -18, 0);
            on_click(h, radio_group_cb, (void *)(intptr_t)g);
        }
        if (!open) continue;
        for (int i = 0; i < FM_UPG_COUNT; i++)
            if (sl_fm_has(&radio_state, i) && sl_fm_upgrade(i, &u) && radio_group_of(&u) == g) radio_owned_row(&u);
    }
}
/* The next few locked upgrades, nearest first, so there is always a goal */
static void radio_coming_up(void)
{
    int best[3] = { -1, -1, -1 };
    double score[3] = { -1, -1, -1 };
    struct fm_upgrade u;
    for (int i = 0; i < FM_UPG_COUNT; i++)
    {
        if (sl_fm_has(&radio_state, i) || !sl_fm_upgrade(i, &u) || sl_fm_unlocked(&radio_state, &u)) continue;
        /* closest to unlocking first; among equals, the cheapest */
        double sc = sl_fm_unlock_progress(&radio_state, &u) - log10(u.cost + 1) * 1e-4;
        for (int k = 0; k < 3; k++)
            if (sc > score[k])
            {
                for (int j = 2; j > k; j--) { score[j] = score[j - 1]; best[j] = best[j - 1]; }
                score[k] = sc; best[k] = i;
                break;
            }
    }
    if (best[0] < 0) return;
    section(radio_list, "COMING UP");
    for (int k = 0; k < 3 && best[k] >= 0; k++)
    {
        char why[96];
        sl_fm_upgrade(best[k], &u);
        sl_fm_unlock_text(&u, why, sizeof why);
        lv_obj_t *r = box(radio_list);
        lv_obj_add_style(r, &st_card, 0);
        lv_obj_set_size(r, RX_W, 84);
        lv_obj_set_style_margin_bottom(r, 6, 0);
        lv_obj_set_style_opa(r, LV_OPA_70, 0);
        lv_obj_t *b = radio_badge(r, radio_upg_badge(&u), 42, false);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *t = text(r, u.name, &ss_semibold_21, 0xffffff);
        lv_obj_set_pos(t, 70, 8);
        lv_obj_set_width(t, RX_W - 70 - 16);
        label_lines(t, 1);
        lv_obj_t *d = text(r, u.desc, &ss_regular_16, C_SUB);
        lv_obj_set_pos(d, 70, 34);
        lv_obj_set_width(d, RX_W - 70 - 16);
        label_lines(d, 1);
        lv_obj_t *w = rx_text(r, why, &ss_regular_16, RX_AMBER);
        lv_obj_set_pos(w, 70, 56);
        lv_obj_set_width(w, RX_W - 70 - 16);
        label_lines(w, 1);
        lv_obj_t *fill;
        lv_obj_t *track = radio_bar(r, 42, 4, &fill);
        lv_obj_set_pos(track, 14, 72);
        lv_obj_update_layout(track);
        radio_bar_set(fill, sl_fm_unlock_progress(&radio_state, &u));
    }
}

/* Counts behind the tab badges: affordable upgrades, signable deals */
static void radio_badges(void)
{
    if (!radio_page_obj) return;
    int16_t ids[RADIO_ROWS];
    int n = radio_available(ids, RADIO_ROWS), can = 0, deals = 0;
    for (int i = 0; i < n; i++)
    {
        struct fm_upgrade u;
        sl_fm_upgrade(ids[i], &u);
        if (u.cost <= radio_state.listeners) can++;
    }
    for (int p = 0; p < PK_COUNT; p++)
        if (sl_fm_perk_ready(&radio_state, p) && radio_state.rights - radio_state.rights_spent >= fm_perks[p].cost) deals++;
    radio_badge_n[RT_UPGRADES] = can;
    radio_badge_n[RT_SYNDICATE] = deals + (sl_fm_pending(&radio_state) > 0);
    for (int t = 0; t < RT_COUNT; t++)
    {
        lv_obj_t *b = radio_tab_badge[t];
        if (!b) continue;
        if (radio_badge_n[t] && t != radio_tab)
        {
            char c[8];
            snprintf(c, sizeof c, "%d", radio_badge_n[t] > 99 ? 99 : radio_badge_n[t]);
            radio_set(lv_obj_get_child(b, 0), t == RT_SYNDICATE ? "!" : c);
            if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
        else if (!lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}
static void radio_rebuild(void)
{
    lv_obj_clean(radio_list);
    radio_nrows = 0;
    radio_synd_btn = radio_synd_level = radio_synd_info = radio_synd_bar = radio_synd_next = NULL;
    radio_stats = radio_buy_all = radio_sale = radio_format_info = NULL;
    memset(radio_stat_val, 0, sizeof radio_stat_val);
    memset(radio_formats, 0, sizeof radio_formats);
    radio_owned_head = NULL;
    for (int i = 0; i < 4; i++) radio_amounts[i] = NULL;
    radio_rows_tab = radio_tab;
    for (int i = 0; i < RT_COUNT; i++) radio_preset_on(radio_tabs[i], i == radio_tab);
    char name[64];
    if (radio_tab == RT_STUDIO)
    {
        static const char *const amounts[4] = { "×1", "×10", "×100", "Max" };
        static const char *const formats[FMT_COUNT] = { "", "Top 40", "Talk", "Late Night", "Hotline" };
        lv_obj_t *fc = box(radio_list);
        lv_obj_add_style(fc, &st_card, 0);
        lv_obj_set_size(fc, RX_W, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(fc, 12, 0);
        lv_obj_set_style_margin_bottom(fc, 10, 0);
        lv_obj_set_flex_flow(fc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(fc, 8, 0);
        lv_obj_t *cap = rx_text(fc, "STATION FORMAT", &ss_semibold_18, RX_AMBER_TXT);
        lv_obj_set_style_pad_left(cap, 4, 0);
        if (sl_fm_formats_open(&radio_state))
        {
            lv_obj_t *fr = box(fc);
            lv_obj_set_size(fr, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(fr, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(fr, 4, 0);
            for (int f = 1; f < FMT_COUNT; f++)
            {
                radio_formats[f] = radio_segment(fr, formats[f], radio_format_cb, f);
            }
            radio_format_info = text(fc, "", &ss_regular_16, C_SUB);
            lv_obj_set_style_pad_left(radio_format_info, 4, 0);
            lv_obj_set_width(radio_format_info, lv_pct(100));   /* wraps inside the card */
            lv_label_set_long_mode(radio_format_info, LV_LABEL_LONG_WRAP);
        }
        else
        {
            lv_obj_t *l = text(fc, "Unlocks with your first Street Team", &ss_regular_16, C_DIM);
            lv_obj_set_style_pad_left(l, 4, 0);
        }
        lv_obj_t *seg = rx_box(radio_list, RX_W, LV_SIZE_CONTENT, SL_R_LG, C_CARD);
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(seg, 4, 0);
        lv_obj_set_style_pad_column(seg, 4, 0);
        lv_obj_set_style_margin_bottom(seg, 10, 0);
        for (int i = 0; i < 4; i++)
        {
            radio_amounts[i] = radio_segment(seg, amounts[i], radio_amount_cb, i);
            radio_segment_on(radio_amounts[i], i == radio_amount);
        }
        for (int b = 0; b < FM_B_COUNT; b++)
        {
            if (!radio_building_shown(b))
            {
                lv_obj_t *r = radio_row("???", -1, NULL, &radio_building_badge[b], 96, 0);
                if (!r) break;
                radio_nrows--;   /* a teaser, not a live row */
                lv_label_set_text(radio_row_sub[radio_nrows], sl_tr("Keep growing to tune this in"));
                lv_obj_add_flag(radio_row_pill[radio_nrows], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_opa(r, LV_OPA_60, 0);
                break;
            }
            lv_obj_t *r = radio_row(fm_buildings[b].name, b, radio_building_cb, &radio_building_badge[b], 96, 0);
            if (!r) break;
            int k = radio_nrows - 1;
            radio_row_owned[k] = text(r, "", &ss_bold_28, 0xffffff);
            lv_obj_align(radio_row_owned[k], LV_ALIGN_TOP_RIGHT, -16, 6);
            lv_obj_t *track = radio_bar(r, RX_W - 84 - 200, 5, &radio_row_bar[k]);
            lv_obj_set_pos(track, 84, 74);
        }
    }
    else if (radio_tab == RT_UPGRADES)
    {
        int16_t ids[RADIO_ROWS];
        int n = radio_available(ids, RADIO_ROWS);
        /* just bought ones stay in the list, in price order, marked Owned */
        for (int i = 0; i < FM_UPG_COUNT && n < RADIO_ROWS; i++)
        {
            struct fm_upgrade u, p;
            if (!fm_bit(radio_fresh, i) || !sl_fm_upgrade(i, &u)) continue;
            int k = n++;
            while (k > 0 && sl_fm_upgrade(ids[k - 1], &p) && p.cost > u.cost) { ids[k] = ids[k - 1]; k--; }
            ids[k] = (int16_t)i;
        }
        struct fm_summary m;
        sl_fm_summary(&radio_state, &m);
        radio_owned_view();
        lv_obj_t *bar = box(radio_list);
        lv_obj_set_size(bar, RX_W, 52);
        lv_obj_set_style_margin_bottom(bar, 6, 0);
        radio_sale = text(bar, "", &ss_semibold_18, C_SUB);   /* the theme's secondary words, light and dark */
        lv_obj_align(radio_sale, LV_ALIGN_LEFT_MID, 6, 0);
        radio_buy_all = radio_segment(bar, "Buy all", radio_buy_all_cb, 0);
        lv_obj_set_flex_grow(radio_buy_all, 0);
        lv_obj_set_size(radio_buy_all, 130, 44);
        lv_obj_align(radio_buy_all, LV_ALIGN_RIGHT_MID, 0, 0);
        if (!n)
        {
            lv_obj_t *l = text(radio_list, "Nothing on sale right now. See what's coming up below.", &ss_regular_18, C_DIM);
            lv_obj_set_style_pad_all(l, 12, 0);
        }
        for (int i = 0; i < n; i++)
        {
            struct fm_upgrade u;
            sl_fm_upgrade(ids[i], &u);
            if (!radio_row(u.name, ids[i], radio_upgrade_cb, radio_upg_badge(&u), 84, 0)) break;
            lv_label_set_text(radio_row_sub[radio_nrows - 1], u.desc);
        }
        radio_coming_up();
        snprintf(name, sizeof name, "YOUR UPGRADES  ·  %d of %d", m.owned_count, sl_fm_upgrade_total());
        radio_owned_head = section(radio_list, name);
        lv_obj_set_style_pad_top(radio_owned_head, 18, 0);
        radio_owned_details();
    }
    else if (radio_tab == RT_SYNDICATE)
    {
        lv_obj_t *card = box(radio_list);
        lv_obj_add_style(card, &st_card, 0);
        lv_obj_set_size(card, RX_W, 176);
        lv_obj_set_style_margin_bottom(card, 10, 0);
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(RX_PANEL), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
        lv_obj_t *cap = rx_text(card, "NETWORK LEVEL", &ss_semibold_18, RX_AMBER);
        lv_obj_set_pos(cap, 20, 16);
        radio_synd_level = rx_text(card, "", &ss_bold_56, RX_CREAM);
        lv_obj_set_pos(radio_synd_level, 18, 38);
        radio_synd_info = text(card, "", &ss_regular_18, C_SUB);
        lv_obj_set_style_text_align(radio_synd_info, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(radio_synd_info, LV_ALIGN_TOP_RIGHT, -20, 44);
        lv_obj_t *track = radio_bar(card, RX_W - 40, 8, &radio_synd_bar);
        lv_obj_set_pos(track, 20, 120);
        radio_synd_next = text(card, "", &ss_regular_16, C_DIM);
        lv_obj_set_pos(radio_synd_next, 20, 136);
        radio_synd_btn = rx_box(radio_list, RX_W, 60, SL_R_FULL, C_CARD);
        lv_obj_set_style_margin_bottom(radio_synd_btn, 6, 0);
        lv_obj_center(text(radio_synd_btn, "", &ss_semibold_21, 0xffffff));
        on_click(radio_synd_btn, radio_syndicate_cb, NULL);
        lv_obj_t *note = text(radio_list, "Resets listeners, buildings and upgrades. Keeps Rights, deals, plaques and stats.",
                              &ss_regular_16, C_DIM);
        lv_obj_set_width(note, RX_W);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        section(radio_list, "AFFILIATE DEALS");
        int order[PK_COUNT], n = 0;
        radio_perk_order(-1, order, &n);
        for (int i = 0; i < n; i++)
        {
            int p = order[i], d = radio_perk_depth(p);
            const struct radio_badge badge = {
                sl_fm_perk(&radio_state, p) ? 0x7ee081 : sl_fm_perk_ready(&radio_state, p) ? RX_AMBER : 0x5d646e,
                sl_fm_perk(&radio_state, p) ? LV_SYMBOL_OK : sl_fm_perk_ready(&radio_state, p) ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS };
            lv_obj_t *r = radio_row(fm_perks[p].name, p, radio_perk_cb, &badge, 84, (d > 2 ? 2 : d) * 18);
            if (!r) break;
            lv_label_set_text(radio_row_sub[radio_nrows - 1], fm_perks[p].desc);
        }
    }
    else
    {
        static const char *const names[RADIO_STATS] = {
            "This broadcast", "All time", "Listeners / s", "Per tap", "Taps", "Tap earnings",
            "Buildings", "Upgrades", "Callers answered", "Syndications", "On air", "Total boost" };
        radio_stats = box(radio_list);
        lv_obj_set_size(radio_stats, RX_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(radio_stats, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(radio_stats, 10, 0);
        lv_obj_set_style_pad_row(radio_stats, 10, 0);
        for (int i = 0; i < RADIO_STATS; i++)
        {
            lv_obj_t *t = box(radio_stats);
            lv_obj_add_style(t, &st_card, 0);
            lv_obj_set_size(t, (RX_W - 10) / 2, 84);
            radio_stat_val[i] = text(t, "", &ss_semibold_23, 0xffffff);
            lv_obj_set_pos(radio_stat_val[i], 16, 12);
            lv_obj_set_width(radio_stat_val[i], (RX_W - 10) / 2 - 28);
            label_lines(radio_stat_val[i], 1);
            lv_obj_t *l = text(t, names[i], &ss_regular_16, C_DIM);
            lv_obj_set_pos(l, 16, 48);
        }
        snprintf(name, sizeof name, "PLAQUES  %d / %d  ·  TAP TO READ", sl_fm_plaque_count(&radio_state), FM_PLAQUE_COUNT);
        section(radio_list, name);
        lv_obj_t *shelf = box(radio_list);
        lv_obj_set_size(shelf, RX_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(shelf, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(shelf, 5, 0);
        lv_obj_set_style_pad_row(shelf, 5, 0);
        for (int i = 0; i < FM_PLAQUE_COUNT; i++)
        {
            bool on = fm_bit(radio_state.plaques, i);
            lv_obj_t *d = rx_box(shelf, 17, 17, 4, on ? RX_AMBER : C_CARD_P);
            if (on) lv_obj_set_style_border_color(d, lv_color_hex(0xffe0a8), 0);
            lv_obj_set_style_border_width(d, on ? 2 : 0, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(on ? 0xffd9a0 : C_DIM), LV_STATE_PRESSED);
            lv_obj_set_ext_click_area(d, 3);
            on_click(d, radio_plaque_cb, (void *)(intptr_t)i);
        }
    }
    radio_sig = radio_signature();
    radio_badges();
    radio_refresh();
    if (radio_scroll_tabs && radio_tab_row)
    {
        /* bring the tab's content into view instead of leaving it under the receiver */
        radio_scroll_tabs = false;
        lv_obj_t *body = lv_obj_get_parent(radio_list);
        lv_obj_update_layout(body);
        lv_obj_scroll_to_y(body, lv_obj_get_y(radio_tab_row), LV_ANIM_ON);
    }
}
static void radio_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!radio_page_obj || lv_obj_has_flag(radio_page_obj, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t now = sl_ms(), dt = now - radio_last_ms;
    radio_last_ms = now;
    if (dt > sl_fm_offline_cap(&radio_state) * 1000u) dt = sl_fm_offline_cap(&radio_state) * 1000u;
    if (radio_buff && now >= radio_buff_until) radio_buff = RB_NONE;
    if (radio_rates.per_sec > 0)
    {
        sl_fm_earn(&radio_state, radio_rates.per_sec * (radio_buff == RB_HIT ? 7 : 1) * dt / 1000.0);
        radio_dirty = true;
    }
    uint32_t clock = radio_clock();
    if (clock) radio_state.last_time = clock;
    /* Signal Lock drains once tapping stops */
    if (radio_combo > 0 && now - radio_combo_ms > FM_COMBO_IDLE)
        radio_combo = radio_combo > FM_COMBO_DRAIN * dt / 1000 ? radio_combo - FM_COMBO_DRAIN * dt / 1000 : 0;
    radio_vu_set(0, (int)lround(radio_combo * RADIO_SEGS));
    double lg = log10(1 + radio_rates.per_sec * (radio_buff == RB_HIT ? 7 : 1));
    int out = radio_rates.per_sec > 0 ? 2 + (int)(lg * 0.6) : 0;
    radio_vu_set(1, out ? out + (int)(sl_fm_rand(&radio_seed) % 3) - 1 : 0);
    /* Request Line */
    if (radio_caller_until && now >= radio_caller_until)
    {
        lv_anim_delete(radio_caller, radio_ring);
        lv_obj_add_flag(radio_caller, LV_OBJ_FLAG_HIDDEN);
        radio_caller_until = 0;
        radio_schedule_caller(now);
    }
    else if (!radio_caller_until && now >= radio_caller_next)
    {
        radio_caller_until = now + (sl_fm_perk(&radio_state, PK_HOT2) ? 26000 : 13000);
        /* rings in the gutter between a meter and the knob */
        int x = 60 + (int)(sl_fm_rand(&radio_seed) % 12), y = 186 + (int)(sl_fm_rand(&radio_seed) % 60);
        if (sl_fm_rand(&radio_seed) & 1) lv_obj_align(radio_caller, LV_ALIGN_TOP_LEFT, x, y);
        else lv_obj_align(radio_caller, LV_ALIGN_TOP_RIGHT, -x, y);
        lv_obj_remove_flag(radio_caller, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t ring;
        lv_anim_init(&ring);
        lv_anim_set_var(&ring, radio_caller);
        lv_anim_set_values(&ring, 3, 11);
        lv_anim_set_duration(&ring, 420);
        lv_anim_set_playback_duration(&ring, 420);
        lv_anim_set_repeat_count(&ring, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ring, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&ring, radio_ring);
        lv_anim_start(&ring);
        radio_schedule_caller(radio_caller_until);
        sfx_play(SFX_UI_NOTIFY);
    }
    if (now - radio_ticker_ms >= radio_ticker_len) { radio_ticker_ms = now; radio_next_news(); }
    if (now - radio_second_ms >= 1000)
    {
        radio_second_ms = now;
        radio_plaques();
        if (radio_signature() != radio_sig) radio_rebuild();
        else radio_badges();
    }
    radio_refresh();
    /* purchases save at once and power off flushes, so play only needs a slow autosave */
    if (radio_dirty && now - radio_last_saved_ms >= 30000 && !radio_save())
        sl_log("radio: autosave failed");
}
static void radio_deleted(lv_event_t *e)
{
    (void)e;
    lv_async_call_cancel(radio_rebuild_async, NULL);
    if (radio_timer) { lv_timer_delete(radio_timer); radio_timer = NULL; }
    if (radio_dirty && !radio_save()) sl_log("radio: final save failed");
    radio_page_obj = radio_dial = radio_knob_dot = radio_knob_caption = radio_coins = radio_unit = NULL;
    radio_rate = radio_station = radio_needle = radio_lamp = radio_ticker = radio_caller = radio_list = NULL;
    radio_stats = radio_synd_btn = radio_synd_level = radio_synd_info = radio_synd_bar = radio_synd_next = NULL;
    radio_buy_all = radio_sale = radio_format_info = NULL;
    memset(radio_formats, 0, sizeof radio_formats);
    memset(radio_tabs, 0, sizeof radio_tabs);
    memset(radio_tab_badge, 0, sizeof radio_tab_badge);
    radio_owned_head = NULL;
    radio_rx = radio_banner = radio_tab_row = NULL;
    memset(radio_fresh, 0, sizeof radio_fresh);
    memset(radio_amounts, 0, sizeof radio_amounts);
    memset(radio_float, 0, sizeof radio_float);
    memset(radio_vu, 0, sizeof radio_vu);
    memset(radio_stat_val, 0, sizeof radio_stat_val);
    radio_nrows = 0;
    radio_buff = RB_NONE;
    radio_caller_until = 0;
    radio_combo = 0;
}
/* The receiver: tuner glass, station line, ticker, meters, knob, readout */
static lv_obj_t *radio_receiver(lv_obj_t *body)
{
    /* tall enough for the readout's last line at every text size (470 at Standard);
     * Arabic's letters hang lower than the Latin line, so its lines take
     * their full depth: the rate goes below the deeper unit, the set grows */
    bool deep = sl_has_arabic(sl_tr("listeners"));
    int rate_y = 448 + (deep ? sl_line_h(&ss_semibold_21) - lv_font_get_line_height(&ss_semibold_21) : 0);
    int rate_h = deep ? sl_line_h(&ss_regular_16) : lv_font_get_line_height(&ss_regular_16);
    lv_obj_t *rx = rx_box(body, RX_W, LV_MAX(470, rate_y + rate_h + 4), 26, RX_PANEL2);
    lv_obj_set_style_bg_grad_color(rx, lv_color_hex(RX_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(rx, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(rx, 2, 0);
    lv_obj_set_style_border_color(rx, lv_color_hex(RX_EDGE), 0);

    lv_obj_t *glass = rx_box(rx, RX_W - 28, 84, 12, 0x1f1709);
    lv_obj_set_pos(glass, 12, 12);
    lv_obj_set_style_bg_grad_color(glass, lv_color_hex(RX_GLASS), 0);
    lv_obj_set_style_bg_grad_dir(glass, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(glass, 1, 0);
    lv_obj_set_style_border_color(glass, lv_color_hex(RX_AMBER_D), 0);
    lv_obj_set_style_clip_corner(glass, true, 0);
    for (int i = 0; i <= 20; i++)
    {
        lv_obj_t *t = rx_box(glass, 2, i % 4 ? 8 : 16, 0, RX_AMBER);
        lv_obj_set_style_bg_opa(t, i % 4 ? LV_OPA_50 : LV_OPA_80, 0);
        lv_obj_set_pos(t, RX_SCALE_X + i * RX_SCALE_W / 20, 12);
        if (i % 4) continue;
        char f[8];
        snprintf(f, sizeof f, "%d", 88 + i);
        lv_obj_t *l = rx_text(glass, f, &ss_regular_16, RX_AMBER);
        lv_obj_set_style_opa(l, LV_OPA_80, 0);
        lv_obj_align_to(l, t, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    }
    radio_needle = rx_box(glass, 3, 70, 1, RX_RED);
    lv_obj_set_style_shadow_width(radio_needle, 12, 0);
    lv_obj_set_style_shadow_color(radio_needle, lv_color_hex(RX_RED), 0);
    lv_obj_set_style_shadow_opa(radio_needle, LV_OPA_60, 0);
    radio_needle_x = -1;
    lv_obj_set_pos(radio_needle, RX_SCALE_X, 7);

    lv_obj_t *name = rx_text(rx, "SLOPPY FM", &ss_semibold_18, RX_CREAM);
    lv_obj_set_pos(name, 20, 106);
    radio_station = rx_text(rx, "", &ss_semibold_18, RX_AMBER);
    lv_obj_align(radio_station, LV_ALIGN_TOP_RIGHT, -20, 106);
    radio_lamp = rx_box(rx, LV_SIZE_CONTENT, 26, 13, 0x4a1d18);
    lv_obj_set_style_pad_hor(radio_lamp, 12, 0);
    lv_obj_set_style_shadow_color(radio_lamp, lv_color_hex(RX_RED), 0);
    lv_obj_set_style_shadow_opa(radio_lamp, LV_OPA_70, 0);
    lv_obj_t *lamp = rx_text(radio_lamp, "ON AIR", &ss_semibold_18, RX_CREAM);
    if (deep)                            /* the lamp holds Arabic's deeper letters, still a pill */
    {
        lv_obj_set_height(radio_lamp, LV_MAX(26, sl_line_h(&ss_semibold_18)));
        lv_obj_set_style_radius(radio_lamp, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_center(lamp);
    lv_obj_align(radio_lamp, LV_ALIGN_TOP_MID, 0, 104);

    lv_obj_t *tick = rx_box(rx, RX_W - 28, 28, 8, 0x14161a);
    lv_obj_set_pos(tick, 12, 140);
    radio_ticker = rx_text(tick, "", &ss_regular_16, RX_MUTED);
    lv_obj_set_width(radio_ticker, RX_W - 48);
    lv_label_set_long_mode(radio_ticker, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(radio_ticker, lv_anim_speed_clamped(55, 2000, 60000), 0);
    lv_obj_align(radio_ticker, LV_ALIGN_LEFT_MID, 10, 0);

    static const char *const meters[2] = { "SIGNAL", "OUTPUT" };
    for (int m = 0; m < 2; m++)
    {
        int x = m ? RX_W - 30 - 26 : 30;
        for (int i = 0; i < RADIO_SEGS; i++)
        {
            uint32_t c = i >= 8 ? RX_RED : i >= 6 ? RX_AMBER : 0x7ee081;
            radio_vu[m][i] = rx_box(rx, 26, 10, 2, c);
            lv_obj_set_pos(radio_vu[m][i], x, 184 + (RADIO_SEGS - 1 - i) * 13);
            lv_obj_set_style_bg_opa(radio_vu[m][i], LV_OPA_20, 0);
        }
        lv_obj_t *l = rx_text(rx, meters[m], &ss_regular_16, RX_MUTED);
        lv_obj_align_to(l, radio_vu[m][0], LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        radio_vu_lit[m] = 0;
    }

    radio_dial = rx_box(rx, RX_KNOB, RX_KNOB, RX_KNOB / 2, RX_EDGE);
    lv_obj_align(radio_dial, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_bg_grad_color(radio_dial, lv_color_hex(0x16181c), 0);
    lv_obj_set_style_bg_grad_dir(radio_dial, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(radio_dial, 4, 0);
    lv_obj_set_style_border_color(radio_dial, lv_color_hex(0x0b0c0e), 0);
    lv_obj_set_style_shadow_width(radio_dial, 24, 0);
    lv_obj_set_style_shadow_opa(radio_dial, LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(radio_dial, 6, 0);
    lv_obj_t *cap = rx_box(radio_dial, RX_KNOB - 44, RX_KNOB - 44, (RX_KNOB - 44) / 2, 0x4a5059);
    lv_obj_center(cap);
    lv_obj_set_style_bg_grad_color(cap, lv_color_hex(0x262a30), 0);
    lv_obj_set_style_bg_grad_dir(cap, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(0x5d646e), 0);
    lv_obj_t *note = rx_text(cap, LV_SYMBOL_AUDIO, &sl_sym_28, RX_MUTED);
    lv_obj_center(note);
    radio_knob_dot = rx_box(radio_dial, 14, 14, 7, RX_AMBER);
    lv_obj_set_style_shadow_width(radio_knob_dot, 12, 0);
    lv_obj_set_style_shadow_color(radio_knob_dot, lv_color_hex(RX_AMBER), 0);
    radio_knob_place();
    on_click(radio_dial, radio_tap_cb, NULL);
    radio_knob_caption = rx_text(rx, "", &ss_semibold_18, RX_MUTED);
    lv_obj_align(radio_knob_caption, LV_ALIGN_TOP_MID, 0, 180 + RX_KNOB + 8);

    radio_coins = rx_text(rx, "", &ss_bold_56, RX_CREAM);
    lv_obj_align(radio_coins, LV_ALIGN_TOP_MID, 0, 356);
    radio_unit = rx_text(rx, "", &ss_semibold_21, RX_AMBER);
    lv_obj_align(radio_unit, LV_ALIGN_TOP_MID, 0, 422);   /* under the figure's comma */
    radio_rate = rx_text(rx, "", &ss_regular_16, RX_MUTED);
    lv_obj_align(radio_rate, LV_ALIGN_TOP_MID, 0, rate_y);
    lv_obj_set_style_text_align(radio_coins, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(radio_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(radio_rate, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(radio_coins, RX_W - 20);
    lv_obj_set_width(radio_unit, RX_W - 20);
    lv_obj_set_width(radio_rate, RX_W - 20);

    radio_banner = rx_box(rx, RX_W - 60, 120, SL_R_LG, RX_AMBER);
    lv_obj_align(radio_banner, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_shadow_width(radio_banner, 40, 0);
    lv_obj_set_style_shadow_color(radio_banner, lv_color_hex(RX_AMBER), 0);
    lv_obj_set_style_shadow_opa(radio_banner, LV_OPA_50, 0);
    lv_obj_set_style_transform_pivot_x(radio_banner, (RX_W - 60) / 2, 0);
    lv_obj_set_style_transform_pivot_y(radio_banner, 60, 0);
    lv_obj_t *bt = rx_text(radio_banner, "", &ss_bold_28, RX_INK);
    lv_obj_align(bt, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *bs = rx_text(radio_banner, "", &ss_semibold_18, RX_INK);
    lv_obj_align(bs, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(radio_banner, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < RADIO_FLOATS; i++)
    {
        radio_float[i] = rx_text(rx, "", &ss_semibold_23, RX_CREAM);
        lv_obj_add_flag(radio_float[i], LV_OBJ_FLAG_HIDDEN);
    }
    radio_caller = rx_box(rx, 72, 72, SL_R_FULL, 0xffd166);
    lv_obj_set_style_border_width(radio_caller, 4, 0);
    lv_obj_set_style_border_color(radio_caller, lv_color_hex(0xfff1c7), 0);
    lv_obj_center(rx_text(radio_caller, LV_SYMBOL_CALL, &sl_sym_28, 0x3a2a00));
    on_click(radio_caller, radio_caller_cb, NULL);
    lv_obj_add_flag(radio_caller, LV_OBJ_FLAG_HIDDEN);
    return rx;
}
static void radio_page(void)
{
    if (page_bring_back(radio_page_obj)) return;
    radio_load();
    radio_catch_up();
    lv_obj_t *p, *body = page_new("Sloppy FM", &p);
    radio_page_obj = p;
    lv_obj_add_event_cb(p, radio_deleted, LV_EVENT_DELETE, NULL);
    radio_seed = radio_clock() ^ sl_ms() ^ 0x5eed;
    radio_rx = radio_receiver(body);

    static const char *const tabs[RT_COUNT] = { "Studio", "Upgrades", "Network", "Stats" };
    lv_obj_t *row = box(body);
    radio_tab_row = row;
    lv_obj_set_size(row, RX_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 12, 0);
    for (int i = 0; i < RT_COUNT; i++)
    {
        lv_obj_t *b = rx_box(row, 0, 62, SL_R_MD, C_CARD);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_t *led = rx_box(b, 28, 5, SL_R_FULL, C_CARD_P);
        lv_obj_align(led, LV_ALIGN_TOP_MID, 0, 9);
        lv_obj_set_style_shadow_color(led, lv_color_hex(RX_AMBER), 0);
        lv_obj_t *l = text(b, tabs[i], &ss_semibold_18, 0xffffff);
        sl_fit(l);                       /* inside its tab at any text size */
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
        on_click(b, radio_tab_cb, (void *)(intptr_t)i);
        radio_tabs[i] = b;
        lv_obj_t *badge = rx_box(b, LV_SIZE_CONTENT, 22, SL_R_FULL, 0xd63a2a);   /* RX_RED, deep enough for its white count */
        lv_obj_set_style_min_width(badge, 22, 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_center(rx_text(badge, "", &ss_regular_16, 0xffffff));
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 4);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        radio_tab_badge[i] = badge;
    }
    radio_list = box(body);
    lv_obj_set_size(radio_list, RX_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(radio_list, LV_FLEX_FLOW_COLUMN);
    radio_last_ms = radio_second_ms = radio_ticker_ms = sl_ms();
    radio_schedule_caller(radio_last_ms);
    radio_next_news();
    radio_rebuild();
    radio_timer = lv_timer_create(radio_tick, 200, NULL);
    page_push(p);
}
